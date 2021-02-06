// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lib/jxl/dec_reconstruct.h"

#include <atomic>
#include <utility>

#include "lib/jxl/filters.h"
#include "lib/jxl/image_ops.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "lib/jxl/dec_reconstruct.cc"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "lib/jxl/aux_out.h"
#include "lib/jxl/base/data_parallel.h"
#include "lib/jxl/base/profiler.h"
#include "lib/jxl/common.h"
#include "lib/jxl/dec_noise.h"
#include "lib/jxl/dec_upsample.h"
#include "lib/jxl/dec_xyb-inl.h"
#include "lib/jxl/dec_xyb.h"
#include "lib/jxl/epf.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/gaborish.h"
#include "lib/jxl/loop_filter.h"
#include "lib/jxl/passes_state.h"
HWY_BEFORE_NAMESPACE();
namespace jxl {
namespace HWY_NAMESPACE {

Status ApplyImageFeaturesRow(Image3F* JXL_RESTRICT idct, const Rect& rect,
                             PassesDecoderState* dec_state, ssize_t y,
                             size_t thread) {
  const ImageFeatures& image_features = dec_state->shared->image_features;
  const FrameHeader& frame_header = dec_state->shared->frame_header;
  const OpsinParams& opsin_params = dec_state->shared->opsin_params;

  // ApplyLoopFiltersRow does a memcpy if no filters are applied.
  size_t output_y;
  bool has_output_row =
      ApplyLoopFiltersRow(dec_state, rect, y, thread, idct, &output_y);
  if (!has_output_row) return true;

  const Rect row_rect(rect.x0(), rect.y0() + output_y, rect.xsize(), 1);

  // At this point, `idct:rect` holds the decoded pixels, independently of epf
  // or gaborish having been applied.

  // TODO(veluca): Consider collapsing/inlining some of the following loops.
  image_features.patches.AddTo(idct, row_rect, row_rect);
  JXL_RETURN_IF_ERROR(image_features.splines.AddTo(
      idct, row_rect, row_rect, dec_state->shared_storage.cmap));

  if (frame_header.flags & FrameHeader::kNoise) {
    PROFILER_ZONE("AddNoise");
    AddNoise(image_features.noise_params, row_rect, dec_state->noise, row_rect,
             dec_state->shared_storage.cmap, idct);
  }

  // TODO(veluca): all blending should happen in this function, after the color
  // transform; all upsampling should happen before the color transform *and
  // before noise*. For now, we just skip the color transform entirely if
  // save_before_color_transform, and error out if the frame is supposed to be
  // displayed.

  if (frame_header.color_transform == ColorTransform::kXYB &&
      !frame_header.save_before_color_transform &&
      frame_header.upsampling == 1) {
    PROFILER_ZONE("ToXYB");
    float* JXL_RESTRICT row0 = row_rect.PlaneRow(idct, 0, 0);
    float* JXL_RESTRICT row1 = row_rect.PlaneRow(idct, 1, 0);
    float* JXL_RESTRICT row2 = row_rect.PlaneRow(idct, 2, 0);

    const HWY_FULL(float) d;

// rect is only aligned to blocks (8 floats = 32 bytes). For larger vectors,
// treat loads/stores as unaligned.
#undef LoadBlocks
#undef StoreBlocks
#if HWY_CAP_GE512
#define LoadBlocks LoadU
#define StoreBlocks StoreU
#else
#define LoadBlocks Load
#define StoreBlocks Store
#endif

    for (size_t x = 0; x < rect.xsize(); x += Lanes(d)) {
      const auto in_opsin_x = LoadBlocks(d, row0 + x);
      const auto in_opsin_y = LoadBlocks(d, row1 + x);
      const auto in_opsin_b = LoadBlocks(d, row2 + x);
      JXL_COMPILER_FENCE;
      auto linear_r = Undefined(d);
      auto linear_g = Undefined(d);
      auto linear_b = Undefined(d);
      XybToRgb(d, in_opsin_x, in_opsin_y, in_opsin_b, opsin_params, &linear_r,
               &linear_g, &linear_b);

      StoreBlocks(linear_r, d, row0 + x);
      StoreBlocks(linear_g, d, row1 + x);
      StoreBlocks(linear_b, d, row2 + x);
    }
  }

  return true;
}

Status FinalizeImageRect(Image3F* JXL_RESTRICT idct, const Rect& rect,
                         PassesDecoderState* dec_state, size_t thread) {
  const LoopFilter& lf = dec_state->shared->frame_header.loop_filter;
  JXL_DASSERT(dec_state->decoded_padding >= kMaxFilterBorder);

  for (ssize_t y = -lf.PaddingRows();
       y < static_cast<ssize_t>(lf.PaddingRows() + rect.ysize()); y++) {
    JXL_RETURN_IF_ERROR(
        ApplyImageFeaturesRow(idct, rect, dec_state, y, thread));
  }
  return true;
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace jxl
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace jxl {

HWY_EXPORT(FinalizeImageRect);
Status FinalizeImageRect(Image3F* JXL_RESTRICT idct, const Rect& rect,
                         PassesDecoderState* dec_state, size_t thread) {
  return HWY_DYNAMIC_DISPATCH(FinalizeImageRect)(idct, rect, dec_state, thread);
}

Status FinalizeFrameDecoding(Image3F* JXL_RESTRICT idct,
                             PassesDecoderState* dec_state, ThreadPool* pool) {
  std::vector<Rect> rects_to_process;

  const LoopFilter& lf = dec_state->shared->frame_header.loop_filter;
  const FrameHeader& frame_header = dec_state->shared->frame_header;
  const FrameDimensions& frame_dim = dec_state->shared->frame_dim;

  if ((lf.epf_iters > 0 || lf.gab) && frame_header.chroma_subsampling.Is444() &&
      frame_header.encoding != FrameEncoding::kModular &&
      !dec_state->has_partial_ac_groups) {
    size_t xsize = frame_dim.xsize_padded;
    size_t ysize = frame_dim.ysize_padded;
    size_t xsize_groups = frame_dim.xsize_groups;
    size_t ysize_groups = frame_dim.ysize_groups;
    size_t padx = lf.PaddingCols();
    size_t pady = lf.PaddingRows();
    // For every gap between groups, vertically, enqueue bottom gap with next
    // group ...
    for (size_t ygroup = 0; ygroup < ysize_groups - 1; ygroup++) {
      size_t gystart = ygroup * kGroupDim;
      size_t gyend = std::min(ysize, kGroupDim * (ygroup + 1));
      // Group is processed together with another group.
      if (gyend <= gystart + kBlockDim) continue;
      for (size_t xstart = 0; xstart < xsize;
           xstart += kApplyImageFeaturesTileDim) {
        rects_to_process.emplace_back(xstart, gyend - pady,
                                      kApplyImageFeaturesTileDim, 2 * pady,
                                      xsize, ysize);
      }
    }
    // For every gap between groups, horizontally, enqueue right gap with next
    // group, carefully avoiding overlaps with the horizontal gaps enqueued
    // before...
    for (size_t xgroup = 0; xgroup < xsize_groups - 1; xgroup++) {
      size_t gxstart = xgroup == 0 ? kBlockDim : xgroup * kGroupDim;
      size_t gxend = std::min(xsize, kGroupDim * (xgroup + 1));
      // Group is processed together with another group.
      if (gxend <= gxstart + kBlockDim) continue;
      for (size_t ygroup = 0; ygroup < ysize_groups; ygroup++) {
        size_t gystart = ygroup == 0 ? 0 : ygroup * kGroupDim + pady;
        size_t gyend = ygroup == ysize_groups - 1
                           ? ysize
                           : kGroupDim * (ygroup + 1) - pady;
        if (gyend <= gystart) continue;
        for (size_t ystart = gystart; ystart < gyend;
             ystart += kApplyImageFeaturesTileDim) {
          rects_to_process.emplace_back(gxend - padx, ystart, 2 * padx,
                                        kApplyImageFeaturesTileDim, xsize,
                                        gyend);
        }
      }
    }
  }
  // If we used chroma subsampling, we upsample chroma now and run
  // ApplyImageFeatures after.
  if (!frame_header.chroma_subsampling.Is444()) {
    for (size_t c = 0; c < 3; c++) {
      // This implementation of Upsample assumes that the image dimension in
      // xsize_padded, ysize_padded is a multiple of 8x8.
      JXL_DASSERT(frame_dim.xsize_padded %
                      (1u << frame_header.chroma_subsampling.HShift(c)) ==
                  0);
      JXL_DASSERT(frame_dim.ysize_padded %
                      (1u << frame_header.chroma_subsampling.VShift(c)) ==
                  0);
      ImageF& plane = const_cast<ImageF&>(dec_state->decoded.Plane(c));
      plane.ShrinkTo(
          (frame_dim.xsize_padded >>
           frame_header.chroma_subsampling.HShift(c)) +
              2 * dec_state->decoded_padding,
          frame_dim.ysize_padded >> frame_header.chroma_subsampling.VShift(c));
      for (size_t i = 0; i < frame_header.chroma_subsampling.HShift(c); i++) {
        plane.InitializePaddingForUnalignedAccesses();
        plane = UpsampleH2(plane, dec_state->decoded_padding, pool);
      }
      for (size_t i = 0; i < frame_header.chroma_subsampling.VShift(c); i++) {
        plane.InitializePaddingForUnalignedAccesses();
        plane = UpsampleV2(plane, pool);
      }
      JXL_DASSERT(SameSize(plane, dec_state->decoded));
    }
  }
  // ApplyImageFeatures was not yet run.
  if (frame_header.encoding == FrameEncoding::kModular ||
      !frame_header.chroma_subsampling.Is444() ||
      dec_state->has_partial_ac_groups) {
    // Pad the image if needed.
    if (lf.PaddingCols() != 0) {
      PadRectMirrorInPlace(&dec_state->decoded,
                           Rect(0, 0, frame_dim.xsize_padded, frame_dim.ysize),
                           frame_dim.xsize_padded, lf.PaddingCols(),
                           dec_state->decoded_padding);
    }
    if (lf.epf_iters > 0 && frame_header.encoding == FrameEncoding::kModular) {
      FillImage(kInvSigmaNum / lf.epf_sigma_for_modular,
                &dec_state->filter_weights.sigma);
    }
    for (size_t y = 0; y < idct->ysize(); y += kGroupDim) {
      for (size_t x = 0; x < idct->xsize(); x += kGroupDim) {
        Rect rect(x, y, kGroupDim, kGroupDim, frame_dim.xsize, frame_dim.ysize);
        if (rect.xsize() == 0 || rect.ysize() == 0) continue;
        rects_to_process.push_back(rect);
      }
    }
  }
  const auto allocate_storage = [&](size_t num_threads) {
    dec_state->EnsureStorage(num_threads);
    return true;
  };

  std::atomic<bool> apply_features_ok{true};
  auto run_apply_features = [&](size_t rect_id, size_t thread) {
    if (!FinalizeImageRect(idct, rects_to_process[rect_id], dec_state,
                           thread)) {
      apply_features_ok = false;
    }
  };

  RunOnPool(pool, 0, rects_to_process.size(), allocate_storage,
            run_apply_features, "ApplyFeatures");

  if (!apply_features_ok) {
    return JXL_FAILURE("FinalizeImageRect failed");
  }

  if (frame_header.color_transform == ColorTransform::kYCbCr &&
      !frame_header.save_before_color_transform) {
    // TODO(veluca): create per-pixel version of YcbcrToRgb for line-based
    // decoding in ApplyImageFeatures.
    YcbcrToRgb(idct->Plane(1), idct->Plane(0), idct->Plane(2), &idct->Plane(0),
               &idct->Plane(1), &idct->Plane(2), pool);
  }  // otherwise no color transform needed

  idct->ShrinkTo(frame_dim.xsize, frame_dim.ysize);
  // TODO(veluca): consider making upsampling happen per-line.
  if (frame_header.upsampling != 1) {
    Image3F temp(idct->xsize() * frame_header.upsampling,
                 idct->ysize() * frame_header.upsampling);
    dec_state->upsampler.UpsampleRect(*idct, Rect(*idct), &temp, Rect(temp));
    *idct = std::move(temp);
  }
  // Do color transform now if upsampling was done.
  if (frame_header.color_transform == ColorTransform::kXYB &&
      frame_header.upsampling != 1 &&
      !frame_header.save_before_color_transform) {
    const OpsinParams& opsin_params = dec_state->shared->opsin_params;
    OpsinToLinearInplace(idct, pool, opsin_params);
  }

  const size_t xsize = frame_dim.xsize_upsampled;
  const size_t ysize = frame_dim.ysize_upsampled;

  idct->ShrinkTo(xsize, ysize);

  return true;
}

}  // namespace jxl
#endif  // HWY_ONCE
