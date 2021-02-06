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

// Fast SIMD floating-point (I)DCT, any power of two.

#if defined(LIB_JXL_DCT_INL_H_) == defined(HWY_TARGET_TOGGLE)
#ifdef LIB_JXL_DCT_INL_H_
#undef LIB_JXL_DCT_INL_H_
#else
#define LIB_JXL_DCT_INL_H_
#endif

#include <stddef.h>

#include <hwy/highway.h>

#include "lib/jxl/dct_block-inl.h"
#include "lib/jxl/dct_scales.h"
#include "lib/jxl/transpose-inl.h"
HWY_BEFORE_NAMESPACE();
namespace jxl {
namespace HWY_NAMESPACE {
namespace {

template <size_t SZ>
struct FVImpl {
  using type = HWY_CAPPED(float, SZ);
};

template <>
struct FVImpl<0> {
  using type = HWY_FULL(float);
};

template <size_t SZ>
using FV = typename FVImpl<SZ>::type;

// Implementation of Lowest Complexity Self Recursive Radix-2 DCT II/III
// Algorithms, by Siriani M. Perera and Jianhua Liu.

template <size_t N, size_t SZ>
struct CoeffBundle {
  static void AddReverse(const float* JXL_RESTRICT ain1,
                         const float* JXL_RESTRICT ain2,
                         float* JXL_RESTRICT aout) {
    for (size_t i = 0; i < N; i++) {
      auto in1 = Load(FV<SZ>(), ain1 + i * SZ);
      auto in2 = Load(FV<SZ>(), ain2 + (N - i - 1) * SZ);
      Store(in1 + in2, FV<SZ>(), aout + i * SZ);
    }
  }
  static void SubReverse(const float* JXL_RESTRICT ain1,
                         const float* JXL_RESTRICT ain2,
                         float* JXL_RESTRICT aout) {
    for (size_t i = 0; i < N; i++) {
      auto in1 = Load(FV<SZ>(), ain1 + i * SZ);
      auto in2 = Load(FV<SZ>(), ain2 + (N - i - 1) * SZ);
      Store(in1 - in2, FV<SZ>(), aout + i * SZ);
    }
  }
  static void B(float* JXL_RESTRICT coeff) {
    auto sqrt2 = Set(FV<SZ>(), square_root<2>::value);
    auto in1 = Load(FV<SZ>(), coeff);
    auto in2 = Load(FV<SZ>(), coeff + SZ);
    Store(MulAdd(in1, sqrt2, in2), FV<SZ>(), coeff);
    for (size_t i = 1; i + 1 < N; i++) {
      auto in1 = Load(FV<SZ>(), coeff + i * SZ);
      auto in2 = Load(FV<SZ>(), coeff + (i + 1) * SZ);
      Store(in1 + in2, FV<SZ>(), coeff + i * SZ);
    }
  }
  static void BTranspose(float* JXL_RESTRICT coeff) {
    for (size_t i = N - 1; i > 0; i--) {
      auto in1 = Load(FV<SZ>(), coeff + i * SZ);
      auto in2 = Load(FV<SZ>(), coeff + (i - 1) * SZ);
      Store(in1 + in2, FV<SZ>(), coeff + i * SZ);
    }
    auto sqrt2 = Set(FV<SZ>(), square_root<2>::value);
    auto in1 = Load(FV<SZ>(), coeff);
    Store(in1 * sqrt2, FV<SZ>(), coeff);
  }
  // Ideally optimized away by compiler (except the multiply).
  static void InverseEvenOdd(const float* JXL_RESTRICT ain,
                             float* JXL_RESTRICT aout) {
    for (size_t i = 0; i < N / 2; i++) {
      auto in1 = Load(FV<SZ>(), ain + i * SZ);
      Store(in1, FV<SZ>(), aout + 2 * i * SZ);
    }
    for (size_t i = N / 2; i < N; i++) {
      auto in1 = Load(FV<SZ>(), ain + i * SZ);
      Store(in1, FV<SZ>(), aout + (2 * (i - N / 2) + 1) * SZ);
    }
  }
  // Ideally optimized away by compiler.
  static void ForwardEvenOdd(const float* JXL_RESTRICT ain,
                             float* JXL_RESTRICT aout) {
    for (size_t i = 0; i < N / 2; i++) {
      auto in1 = Load(FV<SZ>(), ain + 2 * i * SZ);
      Store(in1, FV<SZ>(), aout + i * SZ);
    }
    for (size_t i = N / 2; i < N; i++) {
      auto in1 = Load(FV<SZ>(), ain + (2 * (i - N / 2) + 1) * SZ);
      Store(in1, FV<SZ>(), aout + i * SZ);
    }
  }
  // Invoked on full vector.
  static void Multiply(float* JXL_RESTRICT coeff) {
    for (size_t i = 0; i < N / 2; i++) {
      auto in1 = Load(FV<SZ>(), coeff + (N / 2 + i) * SZ);
      auto mul = Set(FV<SZ>(), WcMultipliers<N>::kMultipliers[i]);
      Store(in1 * mul, FV<SZ>(), coeff + (N / 2 + i) * SZ);
    }
  }
  static void MultiplyAndAdd(const float* JXL_RESTRICT coeff,
                             float* JXL_RESTRICT out) {
    for (size_t i = 0; i < N / 2; i++) {
      auto mul = Set(FV<SZ>(), WcMultipliers<N>::kMultipliers[i]);
      auto in1 = Load(FV<SZ>(), coeff + i * SZ);
      auto in2 = Load(FV<SZ>(), coeff + (N / 2 + i) * SZ);
      auto out1 = MulAdd(mul, in2, in1);
      auto out2 = NegMulAdd(mul, in2, in1);
      Store(out1, FV<SZ>(), out + i * SZ);
      Store(out2, FV<SZ>(), out + (N - i - 1) * SZ);
    }
  }
  template <typename Block>
  static void LoadFromBlock(const Block& in, size_t off,
                            float* JXL_RESTRICT coeff) {
    for (size_t i = 0; i < N; i++) {
      Store(in.LoadPart(FV<SZ>(), i, off), FV<SZ>(), coeff + i * SZ);
    }
  }
  template <typename Block>
  static void StoreToBlock(const float* JXL_RESTRICT coeff, const Block& out,
                           size_t off) {
    for (size_t i = 0; i < N; i++) {
      out.StorePart(FV<SZ>(), Load(FV<SZ>(), coeff + i * SZ), i, off);
    }
  }
  template <typename Block>
  static void StoreToBlockAndScale(const float* JXL_RESTRICT coeff,
                                   const Block& out, size_t off) {
    auto mul = Set(FV<SZ>(), 1.0f / N);
    for (size_t i = 0; i < N; i++) {
      out.StorePart(FV<SZ>(), mul * Load(FV<SZ>(), coeff + i * SZ), i, off);
    }
  }
};

template <size_t N, size_t SZ>
struct DCT1DImpl;

template <size_t SZ>
struct DCT1DImpl<1, SZ> {
  JXL_INLINE void operator()(float* JXL_RESTRICT mem) {}
};

template <size_t SZ>
struct DCT1DImpl<2, SZ> {
  JXL_INLINE void operator()(float* JXL_RESTRICT mem) {
    auto in1 = Load(FV<SZ>(), mem);
    auto in2 = Load(FV<SZ>(), mem + SZ);
    Store(in1 + in2, FV<SZ>(), mem);
    Store(in1 - in2, FV<SZ>(), mem + SZ);
  }
};

template <size_t N, size_t SZ>
struct DCT1DImpl {
  void operator()(float* JXL_RESTRICT mem) {
    // This is relatively small (4kB with 64-DCT and AVX-512)
    HWY_ALIGN float tmp[N * SZ];
    CoeffBundle<N / 2, SZ>::AddReverse(mem, mem + N / 2 * SZ, tmp);
    DCT1DImpl<N / 2, SZ>()(tmp);
    CoeffBundle<N / 2, SZ>::SubReverse(mem, mem + N / 2 * SZ, tmp + N / 2 * SZ);
    CoeffBundle<N, SZ>::Multiply(tmp);
    DCT1DImpl<N / 2, SZ>()(tmp + N / 2 * SZ);
    CoeffBundle<N / 2, SZ>::B(tmp + N / 2 * SZ);
    CoeffBundle<N, SZ>::InverseEvenOdd(tmp, mem);
  }
};

template <size_t N, size_t SZ>
struct IDCT1DImpl;

template <size_t SZ>
struct IDCT1DImpl<1, SZ> {
  JXL_INLINE void operator()(float* JXL_RESTRICT mem) {}
};

template <size_t SZ>
struct IDCT1DImpl<2, SZ> {
  JXL_INLINE void operator()(float* JXL_RESTRICT mem) {
    auto in1 = Load(FV<SZ>(), mem);
    auto in2 = Load(FV<SZ>(), mem + SZ);
    Store(in1 + in2, FV<SZ>(), mem);
    Store(in1 - in2, FV<SZ>(), mem + SZ);
  }
};

template <size_t N, size_t SZ>
struct IDCT1DImpl {
  void operator()(float* JXL_RESTRICT mem) {
    // This is relatively small (4kB with 64-DCT and AVX-512)
    HWY_ALIGN float tmp[N * SZ];
    CoeffBundle<N, SZ>::ForwardEvenOdd(mem, tmp);
    IDCT1DImpl<N / 2, SZ>()(tmp);
    CoeffBundle<N / 2, SZ>::BTranspose(tmp + N / 2 * SZ);
    IDCT1DImpl<N / 2, SZ>()(tmp + N / 2 * SZ);
    CoeffBundle<N, SZ>::MultiplyAndAdd(tmp, mem);
  }
};

template <size_t N, size_t M_or_0, typename FromBlock, typename ToBlock>
void DCT1DWrapper(const FromBlock& from, const ToBlock& to, size_t Mp) {
  size_t M = M_or_0 != 0 ? M_or_0 : Mp;
  constexpr size_t SZ = MaxLanes(FV<M_or_0>());
  HWY_ALIGN float tmp[N * SZ];
  for (size_t i = 0; i < M; i += Lanes(FV<M_or_0>())) {
    CoeffBundle<N, SZ>::LoadFromBlock(from, i, tmp);
    DCT1DImpl<N, SZ>()(tmp);
    CoeffBundle<N, SZ>::StoreToBlockAndScale(tmp, to, i);
  }
}

template <size_t N, size_t M_or_0, typename FromBlock, typename ToBlock>
void IDCT1DWrapper(const FromBlock& from, const ToBlock& to, size_t Mp) {
  size_t M = M_or_0 != 0 ? M_or_0 : Mp;
  constexpr size_t SZ = MaxLanes(FV<M_or_0>());
  HWY_ALIGN float tmp[N * SZ];
  for (size_t i = 0; i < M; i += Lanes(FV<M_or_0>())) {
    CoeffBundle<N, SZ>::LoadFromBlock(from, i, tmp);
    IDCT1DImpl<N, SZ>()(tmp);
    CoeffBundle<N, SZ>::StoreToBlock(tmp, to, i);
  }
}

template <size_t N, size_t M, typename = void>
struct DCT1D {
  template <typename FromBlock, typename ToBlock>
  void operator()(const FromBlock& from, const ToBlock& to) {
    return DCT1DWrapper<N, M>(from, to, M);
  }
};

template <size_t N, size_t M>
struct DCT1D<N, M, typename std::enable_if<(M > MaxLanes(FV<0>()))>::type> {
  template <typename FromBlock, typename ToBlock>
  void operator()(const FromBlock& from, const ToBlock& to) {
    return NoInlineWrapper(DCT1DWrapper<N, 0, FromBlock, ToBlock>, from, to, M);
  }
};

template <size_t N, size_t M, typename = void>
struct IDCT1D {
  template <typename FromBlock, typename ToBlock>
  void operator()(const FromBlock& from, const ToBlock& to) {
    return IDCT1DWrapper<N, M>(from, to, M);
  }
};

template <size_t N, size_t M>
struct IDCT1D<N, M, typename std::enable_if<(M > MaxLanes(FV<0>()))>::type> {
  template <typename FromBlock, typename ToBlock>
  void operator()(const FromBlock& from, const ToBlock& to) {
    return NoInlineWrapper(IDCT1DWrapper<N, 0, FromBlock, ToBlock>, from, to,
                           M);
  }
};

// Computes the in-place NxN transposed-scaled-DCT (tsDCT) of block.
// Requires that block is HWY_ALIGN'ed.
//
// See also DCTSlow, ComputeDCT
template <size_t N>
struct ComputeTransposedScaledDCT {
  // scratch_space must be aligned, and should have space for 2*N*N floats.
  template <class From, class To>
  HWY_MAYBE_UNUSED void operator()(const From& from, const To& to,
                                   float* JXL_RESTRICT scratch_space) {
    // TODO(user): it is possible to avoid using temporary array,
    // after generalizing "To" to be bi-directional; all sub-transforms could
    // be performed "in-place".
    float* JXL_RESTRICT block = scratch_space;
    // Vector size is capped to N, so this is aligned for all values of N.
    float* JXL_RESTRICT transposed_block = scratch_space + N * N;
    DCT1D<N, N>()(from, DCTTo(block, N));
    Transpose<N, N>::Run(DCTFrom(block, N), DCTTo(transposed_block, N));
    DCT1D<N, N>()(DCTFrom(transposed_block, N), to);
  }
};

// Computes the in-place NxN transposed-scaled-iDCT (tsIDCT)of block.
// Requires that block is HWY_ALIGN'ed.
//
// See also IDCTSlow, ComputeIDCT.

template <size_t N>
struct ComputeTransposedScaledIDCT {
  // scratch_space must be aligned, and should have space for 2*N*N floats.
  template <class From, class To>
  HWY_MAYBE_UNUSED void operator()(const From& from, const To& to,
                                   float* JXL_RESTRICT scratch_space) {
    // TODO(user): it is possible to avoid using temporary array,
    // after generalizing "To" to be bi-directional; all sub-transforms could
    // be performed "in-place".
    float* JXL_RESTRICT block = scratch_space;
    // Vector size is capped to N, so this is aligned for all values of N.
    float* JXL_RESTRICT transposed_block = scratch_space + N * N;
    IDCT1D<N, N>()(from, DCTTo(block, N));
    Transpose<N, N>::Run(DCTFrom(block, N), DCTTo(transposed_block, N));
    IDCT1D<N, N>()(DCTFrom(transposed_block, N), to);
  }
};
// Computes the non-transposed, scaled DCT of a block, that needs to be
// HWY_ALIGN'ed. Used for rectangular blocks.
template <size_t ROWS, size_t COLS>
struct ComputeScaledDCT {
  // scratch_space must be aligned, and should have space for 2*ROWS*COLS
  // floats.
  template <class From, class To>
  HWY_MAYBE_UNUSED void operator()(const From& from, const To& to,
                                   float* JXL_RESTRICT scratch_space) {
    float* JXL_RESTRICT block = scratch_space;
    // Vector size is capped to ROWS or COLS, so this is aligned for all values
    // of ROWS and COLS.
    float* JXL_RESTRICT transposed_block = scratch_space + ROWS * COLS;
    DCT1D<ROWS, COLS>()(from, DCTTo(block, COLS));
    Transpose<ROWS, COLS>::Run(DCTFrom(block, COLS),
                               DCTTo(transposed_block, ROWS));
    // Reusing block to reduce stack usage.
    if (ROWS < COLS) {
      DCT1D<COLS, ROWS>()(DCTFrom(transposed_block, ROWS), DCTTo(block, ROWS));
      Transpose<COLS, ROWS>::Run(DCTFrom(block, ROWS), to);
    } else {
      DCT1D<COLS, ROWS>()(DCTFrom(transposed_block, ROWS), to);
    }
  }
};
// Computes the non-transposed, scaled DCT of a block, that needs to be
// HWY_ALIGN'ed. Used for rectangular blocks.
template <size_t ROWS, size_t COLS>
struct ComputeScaledIDCT {
  // scratch_space must be aligned, and should have space for 2*ROWS*COLS
  // floats.
  template <class From, class To>
  HWY_MAYBE_UNUSED void operator()(const From& from, const To& to,
                                   float* JXL_RESTRICT scratch_space) {
    float* JXL_RESTRICT block = scratch_space;
    // Vector size is capped to ROWS or COLS, so this is aligned for all values
    // of ROWS and COLS.
    float* JXL_RESTRICT transposed_block = scratch_space + ROWS * COLS;
    // Reverse the steps done in ComputeScaledDCT.
    if (ROWS < COLS) {
      Transpose<ROWS, COLS>::Run(from, DCTTo(block, ROWS));
      IDCT1D<COLS, ROWS>()(DCTFrom(block, ROWS), DCTTo(transposed_block, ROWS));
    } else {
      IDCT1D<COLS, ROWS>()(from, DCTTo(transposed_block, ROWS));
    }
    Transpose<COLS, ROWS>::Run(DCTFrom(transposed_block, ROWS),
                               DCTTo(block, COLS));
    IDCT1D<ROWS, COLS>()(DCTFrom(block, COLS), to);
  }
};

}  // namespace
// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace jxl
HWY_AFTER_NAMESPACE();
#endif  // LIB_JXL_DCT_INL_H_
