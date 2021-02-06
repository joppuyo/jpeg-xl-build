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

#include "lib/jxl/dec_upsample.h"

#include "lib/jxl/image_ops.h"

namespace jxl {
namespace {

template <size_t N>
void InitKernel(const float* weights, float kernel[4][4][5][5]) {
  static_assert(N == 1 || N == 2 || N == 4,
                "Upsampling kernel init only implemented for N = 1,2,4");
  for (size_t i = 0; i < 5 * N; i++) {
    for (size_t j = 0; j < 5 * N; j++) {
      size_t y = std::min(i, j);
      size_t x = std::max(i, j);
      kernel[j / 5][i / 5][j % 5][i % 5] =
          weights[5 * N * y - y * (y - 1) / 2 + x - y];
    }
  }
}

template <size_t N>
float Kernel(size_t x, size_t y, size_t ix, size_t iy,
             const float kernel[4][4][5][5]) {
  if (N == 2) {
    return kernel[0][0][y % 2 ? 4 - iy : iy][x % 2 ? 4 - ix : ix];
  }
  if (N == 4) {
    return kernel[y % 4 < 2 ? y % 2 : 1 - y % 2][x % 4 < 2 ? x % 2 : 1 - x % 2]
                 [y % 4 < 2 ? iy : 4 - iy][x % 4 < 2 ? ix : 4 - ix];
  }
  if (N == 8) {
    return kernel[y % 8 < 4 ? y % 4 : 3 - y % 4][x % 8 < 4 ? x % 4 : 3 - x % 4]
                 [y % 8 < 4 ? iy : 4 - iy][x % 8 < 4 ? ix : 4 - ix];
  }
  JXL_ABORT("Invalid upsample");
}

template <int N>
void Upsample(const Image3F& src, const Rect& src_rect, Image3F* dst,
              const Rect& dst_rect, const float kernel[4][4][5][5]) {
  for (size_t c = 0; c < 3; c++) {
    for (size_t y = 0; y < dst_rect.ysize(); y++) {
      float* dst_row = dst_rect.PlaneRow(dst, c, y);
      const float* src_rows[5];
      for (int iy = -2; iy <= 2; iy++) {
        src_rows[iy + 2] =
            src.PlaneRow(c, Mirror(static_cast<int>(y) / N +
                                       static_cast<int>(src_rect.y0()) + iy,
                                   src.ysize())) +
            src_rect.x0();
      }
      for (size_t x = 0; x < dst_rect.xsize(); x++) {
        int src_x[5];
        for (int ix = -2; ix <= 2; ix++) {
          src_x[ix + 2] = Mirror(static_cast<int>(x) / N + ix, src.xsize());
        }
        float result = 0;
        float min = src_rows[0][src_x[0]];
        float max = src_rows[0][src_x[0]];
        for (size_t iy = 0; iy < 5; iy++) {
          for (size_t ix = 0; ix < 5; ix++) {
            float v = src_rows[iy][src_x[ix]];
            result += Kernel<N>(x, y, ix, iy, kernel) * v;
            min = std::min(v, min);
            max = std::max(v, max);
          }
        }
        // Avoid overshooting.
        dst_row[x] = std::min(std::max(result, min), max);
      }
    }
  }
}
}  // namespace

void Upsampler::Init(size_t upsampling, const CustomTransformData& data) {
  upsampling_ = upsampling;
  if (upsampling_ == 1) return;
  if (upsampling_ == 2) {
    InitKernel<1>(data.upsampling2_weights, kernel_);
  } else if (upsampling_ == 4) {
    InitKernel<2>(data.upsampling4_weights, kernel_);
  } else if (upsampling_ == 8) {
    InitKernel<4>(data.upsampling8_weights, kernel_);
  } else {
    JXL_ABORT("Invalid upsample");
  }
}

void Upsampler::UpsampleRect(const Image3F& src, const Rect& src_rect,
                             Image3F* dst, const Rect& dst_rect) const {
  if (upsampling_ == 1) return;
  if (upsampling_ == 2) {
    Upsample<2>(src, src_rect, dst, dst_rect, kernel_);
  } else if (upsampling_ == 4) {
    Upsample<4>(src, src_rect, dst, dst_rect, kernel_);
  } else if (upsampling_ == 8) {
    Upsample<8>(src, src_rect, dst, dst_rect, kernel_);
  } else {
    JXL_ABORT("Not implemented");
  }
}

}  // namespace jxl
