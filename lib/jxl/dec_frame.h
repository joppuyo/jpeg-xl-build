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

#ifndef LIB_JXL_DEC_FRAME_H_
#define LIB_JXL_DEC_FRAME_H_

#include <stdint.h>

#include "lib/jxl/aux_out.h"
#include "lib/jxl/aux_out_fwd.h"
#include "lib/jxl/base/compiler_specific.h"
#include "lib/jxl/base/data_parallel.h"
#include "lib/jxl/base/span.h"
#include "lib/jxl/base/status.h"
#include "lib/jxl/codec_in_out.h"
#include "lib/jxl/common.h"
#include "lib/jxl/dec_bit_reader.h"
#include "lib/jxl/dec_cache.h"
#include "lib/jxl/dec_modular.h"
#include "lib/jxl/dec_params.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/headers.h"
#include "lib/jxl/image_bundle.h"

namespace jxl {

// TODO(veluca): remove DecodeDC, DecodeGlobalDCInfo and DecodeFrameHeader once
// the API migrates to FrameDecoder.

// `frame_header` must have nonserialized_metadata and
// nonserialized_is_preview set.
Status DecodeFrameHeader(BitReader* JXL_RESTRICT reader,
                         FrameHeader* JXL_RESTRICT frame_header);

// Decodes the global DC info from a frame section, exposed for use by API.
Status DecodeGlobalDCInfo(BitReader* reader, bool is_jpeg,
                          PassesDecoderState* state, ThreadPool* pool);

// Decodes the DC image, exposed for use by API.
// aux_outs may be nullptr if aux_out is nullptr.
Status DecodeDC(const FrameHeader& frame_header, PassesDecoderState* dec_state,
                ModularFrameDecoder& modular_frame_decoder,
                size_t group_codes_begin,
                const std::vector<uint64_t>& group_offsets,
                const std::vector<uint32_t>& group_sizes,
                ThreadPool* JXL_RESTRICT pool, BitReader* JXL_RESTRICT reader,
                std::vector<AuxOut>* aux_outs, AuxOut* JXL_RESTRICT aux_out,
                std::vector<bool>* has_dc_group = nullptr);

// Decodes a frame. Groups may be processed in parallel by `pool`.
// See DecodeFile for explanation of c_decoded.
// `io` is only used for reading maximum image size. Also updates
// `dec_state` with the new frame header.
// `metadata` is the metadata that applies to all frames of the codestream
// `decoded->metadata` must already be set and must match metadata.m.
Status DecodeFrame(const DecompressParams& dparams,
                   PassesDecoderState* dec_state, ThreadPool* JXL_RESTRICT pool,
                   BitReader* JXL_RESTRICT reader, AuxOut* JXL_RESTRICT aux_out,
                   ImageBundle* decoded, const CodecMetadata& metadata,
                   const SizeConstraints* constraints, bool is_preview = false);

// Leaves reader in the same state as DecodeFrame would. Used to skip preview.
// Also updates `dec_state` with the new frame header.
Status SkipFrame(const CodecMetadata& metadata, BitReader* JXL_RESTRICT reader,
                 bool is_preview = false);

// TODO(veluca): implement "forced drawing".
class FrameDecoder {
 public:
  // All parameters must outlive the FrameDecoder.
  FrameDecoder(PassesDecoderState* dec_state, const CodecMetadata& metadata,
               ThreadPool* pool, AuxOut* aux_out)
      : dec_state_(dec_state),
        pool_(pool),
        aux_out_(aux_out),
        frame_header_(&metadata) {}

  // `constraints` must outlive the FrameDecoder if not null, or stay alive
  // until the next call to SetFrameSizeLimits.
  void SetFrameSizeLimits(const SizeConstraints* constraints) {
    constraints_ = constraints;
  }

  // Read FrameHeader and table of contents from the given BitReader.
  // Also checks frame dimensions for their limits, and sets the output
  // image buffer.
  // TODO(veluca): remove the `allow_partial_frames` flag - this should be moved
  // on callers.
  Status InitFrame(BitReader* JXL_RESTRICT br, ImageBundle* decoded,
                   bool is_preview, bool allow_partial_frames,
                   bool allow_partial_dc_global);

  struct SectionInfo {
    BitReader* JXL_RESTRICT br;
    size_t id;
  };

  enum SectionStatus {
    // Processed correctly.
    kDone = 0,
    // Skipped because other required sections were not yet processed.
    kSkipped = 1,
    // Skipped because the section was already processed.
    kDuplicate = 2,
    // Only partially decoded: the section will need to be processed again.
    kPartial = 3,
  };

  // Processes `num` sections; each SectionInfo contains the index
  // of the section and a BitReader that only contains the data of the section.
  // `section_status` should point to `num` elements, and will be filled with
  // information about whether each section was processed or not.
  // A section is a part of the encoded file that is indexed by the TOC.
  // TODO(veluca): multiple calls to ProcessSections are not (yet) supported.
  Status ProcessSections(const SectionInfo* sections, size_t num,
                         SectionStatus* section_status);

  // Runs final operations once a frame data is decoded.
  // Must be called exactly once per frame, after all calls to ProcessSections.
  Status FinalizeFrame();

  const std::vector<uint64_t>& SectionOffsets() const {
    return section_offsets_;
  }
  const std::vector<uint32_t>& SectionSizes() const { return section_sizes_; }
  size_t NumSections() const { return section_sizes_.size(); }

  // TODO(veluca): remove once we remove --downsampling flag.
  void SetMaxPasses(size_t max_passes) { max_passes_ = max_passes; }
  const FrameHeader& GetFrameHeader() const { return frame_header_; }

 private:
  Status ProcessDCGlobal(BitReader* br);
  Status ProcessDCGroup(size_t dc_group_id, BitReader* br);
  void FinalizeDC();
  Status ProcessACGlobal(BitReader* br);
  Status ProcessACGroup(size_t ac_group_id, BitReader* JXL_RESTRICT* br,
                        size_t num_passes, size_t thread);

  // Sets the number of threads that will be used. The value of the "thread"
  // parameter passed to DecodeDCGroup and DecodeACGroup must be smaller than
  // the "num_threads" passed here.
  void SetNumThreads(size_t num_threads) {
    if (num_threads > group_dec_caches_size_) {
      group_dec_caches_size_ = num_threads;
      group_dec_caches_ =
          hwy::MakeUniqueAlignedArray<GroupDecCache>(num_threads);
    }
    dec_state_->EnsureStorage(num_threads);
  }

  PassesDecoderState* dec_state_;
  ThreadPool* pool_;
  AuxOut* aux_out_;
  std::vector<uint64_t> section_offsets_;
  std::vector<uint32_t> section_sizes_;
  size_t max_passes_;
  // TODO(veluca): figure out the duplication between these and dec_state_.
  FrameHeader frame_header_;
  FrameDimensions frame_dim_;
  ImageBundle* decoded_;
  ModularFrameDecoder modular_frame_decoder_;
  bool allow_partial_frames_;
  bool allow_partial_dc_global_;

  std::vector<uint8_t> processed_section_;
  std::vector<uint8_t> decoded_passes_per_ac_group_;
  std::vector<uint8_t> decoded_dc_groups_;
  bool decoded_dc_global_;
  bool decoded_ac_global_;
  bool finalized_dc_ = true;
  bool is_finalized_ = true;

  // Number of allocated GroupDecCache entries in the group_dec_caches_ smart
  // pointer. This is only needed to tell whether we need to reallocate the
  // cache.
  size_t group_dec_caches_size_ = 0;
  hwy::AlignedUniquePtr<GroupDecCache[]> group_dec_caches_;

  // Frame size limits.
  const SizeConstraints* constraints_ = nullptr;
};

}  // namespace jxl

#endif  // LIB_JXL_DEC_FRAME_H_
