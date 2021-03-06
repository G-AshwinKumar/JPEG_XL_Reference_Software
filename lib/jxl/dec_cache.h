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

#ifndef LIB_JXL_DEC_CACHE_H_
#define LIB_JXL_DEC_CACHE_H_

#include <stdint.h>

#include <hwy/base.h>  // HWY_ALIGN_MAX

#include "lib/jxl/ac_strategy.h"
#include "lib/jxl/base/profiler.h"
#include "lib/jxl/coeff_order.h"
#include "lib/jxl/common.h"
#include "lib/jxl/convolve.h"
#include "lib/jxl/dec_noise.h"
#include "lib/jxl/dec_upsample.h"
#include "lib/jxl/filters.h"
#include "lib/jxl/image.h"
#include "lib/jxl/passes_state.h"
#include "lib/jxl/quant_weights.h"

namespace jxl {

// Per-frame decoder state. All the images here should be accessed through a
// group rect (either with block units or pixel units).
struct PassesDecoderState {
  PassesSharedState shared_storage;
  // Allows avoiding copies for encoder loop.
  const PassesSharedState* JXL_RESTRICT shared = &shared_storage;

  // Upsampler for the current frame.
  Upsampler upsampler;

  // DC upsampler
  Upsampler dc_upsampler;

  // Storage for RNG output for noise synthesis.
  Image3F noise;

  // Storage for pre-color-transform output for displayed
  // save_before_color_transform frames.
  Image3F pre_color_transform_frame;
  // Non-empty (contains originals) if extra-channels were cropped.
  std::vector<ImageF> pre_color_transform_ec;

  // For ANS decoding.
  std::vector<ANSCode> code;
  std::vector<std::vector<uint8_t>> context_map;

  // Multiplier to be applied to the quant matrices of the x channel.
  float x_dm_multiplier;
  float b_dm_multiplier;

  // Decoded image.
  Image3F decoded;

  // Seed for noise, to have different noise per-frame.
  size_t noise_seed = 0;

  // Keep track of the transform types used.
  std::atomic<uint32_t> used_acs{0};

  // Storage for coefficients if in "accumulate" mode.
  std::unique_ptr<ACImage> coefficients = make_unique<ACImageT<int32_t>>(0, 0);

  // Filter application pipeline used by ApplyImageFeatures. One entry is needed
  // per thread.
  std::vector<FilterPipeline> filter_pipelines;

  // Input weights used by the filters. These are shared from multiple threads
  // but are read-only for the filter application.
  FilterWeights filter_weights;

  static constexpr size_t kMaxFinalizeRectPadding = 9;

  // Amount of padding that will be accessed, in all directions, outside a rect
  // during a call to FinalizeImageRect().
  size_t FinalizeRectPadding() const {
    // TODO(veluca): add YCbCr upsampling here too.
    size_t padding = shared->frame_header.loop_filter.Padding();
    padding += shared->frame_header.upsampling == 1 ? 0 : 2;
    JXL_DASSERT(padding <= kMaxFinalizeRectPadding);
    return padding;
  }

  // Storage for intermediate data during FinalizeRect steps.
  std::vector<Image3F> filter_input_storage;
  std::vector<Image3F> padded_upsampling_input_storage;
  std::vector<Image3F> upsampling_input_storage;

  void EnsureStorage(size_t num_threads) {
    // We need one filter_storage per thread, ensure we have at least that many.
    if (shared->frame_header.loop_filter.epf_iters != 0 ||
        shared->frame_header.loop_filter.gab) {
      if (filter_pipelines.size() < num_threads) {
        filter_pipelines.resize(num_threads);
      }
    }
    // We allocate filter_input_storage unconditionally to ensure that the image
    // is allocated if we need it for DC upsampling.
    for (size_t _ = filter_input_storage.size(); _ < num_threads; _++) {
      // Extra padding along the x dimension to ensure memory accesses don't
      // load out-of-bounds pixels.
      filter_input_storage.emplace_back(
          kGroupDim +
              2 * DivCeil(kMaxFinalizeRectPadding, kBlockDim) * kBlockDim,
          kGroupDim + 2 * kMaxFinalizeRectPadding);
    }
    if (shared->frame_header.upsampling != 1) {
      for (size_t _ = upsampling_input_storage.size(); _ < num_threads; _++) {
        // At this point, we only need up to 2 pixels of border per side for
        // upsampling, but we add an extra border for aligned access.
        upsampling_input_storage.emplace_back(kGroupDim + 2 * kBlockDim,
                                              kGroupDim + 4);
        padded_upsampling_input_storage.emplace_back(kGroupDim + 2 * kBlockDim,
                                                     kGroupDim + 4);
      }
    }
  }

  // Color encoding that will be used for output.
  ColorEncoding output_encoding;

  // Initializes decoder-specific structures using information from *shared.
  void Init(ThreadPool* pool) {
    x_dm_multiplier =
        std::pow(1 / (1.25f), shared->frame_header.x_qm_scale - 2.0f);
    b_dm_multiplier =
        std::pow(1 / (1.25f), shared->frame_header.b_qm_scale - 2.0f);

    output_encoding =
        shared->frame_header.color_transform == ColorTransform::kXYB
            ? ColorEncoding::LinearSRGB(
                  shared->metadata->m.color_encoding.IsGray())
            : shared->metadata->m.color_encoding;
    // TODO(veluca): keep in sync with dec_reconstruct.cc.
    if (shared->metadata->m.xyb_encoded &&
        shared->frame_header.needs_color_transform() &&
        shared->metadata->m.color_encoding.IsSRGB()) {
      output_encoding = ColorEncoding::SRGB(output_encoding.IsGray());
    }
    used_acs = 0;

    if (shared->frame_header.flags & FrameHeader::kNoise) {
      noise = Image3F(shared->frame_dim.xsize_upsampled_padded,
                      shared->frame_dim.ysize_upsampled_padded);
      size_t num_x_groups = DivCeil(noise.xsize(), kGroupDim);
      size_t num_y_groups = DivCeil(noise.ysize(), kGroupDim);
      PROFILER_ZONE("GenerateNoise");
      auto generate_noise = [&](int group_index, int _) {
        size_t gx = group_index % num_x_groups;
        size_t gy = group_index / num_x_groups;
        Rect rect(gx * kGroupDim, gy * kGroupDim, kGroupDim, kGroupDim,
                  noise.xsize(), noise.ysize());
        RandomImage3(noise_seed + group_index, rect, &noise);
      };
      RunOnPool(pool, 0, num_x_groups * num_y_groups, ThreadPool::SkipInit(),
                generate_noise, "Generate noise");
      {
        PROFILER_ZONE("High pass noise");
        // 4 * (1 - box kernel)
        WeightsSymmetric5 weights{{HWY_REP4(-3.84)}, {HWY_REP4(0.16)},
                                  {HWY_REP4(0.16)},  {HWY_REP4(0.16)},
                                  {HWY_REP4(0.16)},  {HWY_REP4(0.16)}};
        // TODO(veluca): avoid copy.
        // TODO(veluca): avoid having a full copy of the image in main memory.
        ImageF noise_tmp(noise.xsize(), noise.ysize());
        for (size_t c = 0; c < 3; c++) {
          Symmetric5(noise.Plane(c), Rect(noise), weights, pool, &noise_tmp);
          std::swap(noise.Plane(c), noise_tmp);
        }
        noise_seed += shared->frame_dim.num_groups;
      }
    }

    // decoded must be padded to a multiple of kBlockDim rows since the last
    // rows may be used by the filters even if they are outside the frame
    // dimension.
    decoded =
        Image3F(shared->frame_dim.xsize_padded, shared->frame_dim.ysize_padded);
#if MEMORY_SANITIZER
    // Avoid errors due to loading vectors on the outermost padding.
    ZeroFillImage(&decoded);
#endif
    const LoopFilter& lf = shared->frame_header.loop_filter;
    filter_weights.Init(lf, shared->frame_dim);
    for (auto& fp : filter_pipelines) {
      // De-initialize FilterPipelines.
      fp.num_filters = 0;
    }
  }

  // Initialize the decoder state after all of DC is decoded.
  void InitForAC() {
    shared_storage.coeff_order_size = 0;
    for (uint8_t o = 0; o < AcStrategy::kNumValidStrategies; ++o) {
      if (((1 << o) & used_acs) == 0) continue;
      uint8_t ord = kStrategyOrder[o];
      shared_storage.coeff_order_size =
          std::max(kCoeffOrderOffset[3 * (ord + 1)] * kDCTBlockSize,
                   shared_storage.coeff_order_size);
    }
    size_t sz = shared_storage.frame_header.passes.num_passes *
                shared_storage.coeff_order_size;
    if (sz > shared_storage.coeff_orders.size()) {
      shared_storage.coeff_orders.resize(sz);
    }
  }
};

// Temp images required for decoding a single group. Reduces memory allocations
// for large images because we only initialize min(#threads, #groups) instances.
struct GroupDecCache {
  void InitOnce(size_t num_passes, size_t used_acs) {
    PROFILER_FUNC;

    for (size_t i = 0; i < num_passes; i++) {
      if (num_nzeroes[i].xsize() == 0) {
        // Allocate enough for a whole group - partial groups on the
        // right/bottom border just use a subset. The valid size is passed via
        // Rect.

        num_nzeroes[i] = Image3I(kGroupDimInBlocks, kGroupDimInBlocks);
      }
    }
    size_t max_block_area = 0;

    for (uint8_t o = 0; o < AcStrategy::kNumValidStrategies; ++o) {
      AcStrategy acs = AcStrategy::FromRawStrategy(o);
      if ((used_acs & (1 << o)) == 0) continue;
      size_t area =
          acs.covered_blocks_x() * acs.covered_blocks_y() * kDCTBlockSize;
      max_block_area = std::max(area, max_block_area);
    }

    if (max_block_area > max_block_area_) {
      max_block_area_ = max_block_area;
      // We need 3x float blocks for dequantized coefficients and 1x for scratch
      // space for transforms.
      float_memory_ = hwy::AllocateAligned<float>(max_block_area_ * 4);
      // We need 3x int32 or int16 blocks for quantized coefficients.
      int32_memory_ = hwy::AllocateAligned<int32_t>(max_block_area_ * 3);
      int16_memory_ = hwy::AllocateAligned<int16_t>(max_block_area_ * 3);
    }

    dec_group_block = float_memory_.get();
    scratch_space = dec_group_block + max_block_area_ * 3;
    dec_group_qblock = int32_memory_.get();
    dec_group_qblock16 = int16_memory_.get();
  }

  // Scratch space used by DecGroupImpl().
  float* dec_group_block;
  int32_t* dec_group_qblock;
  int16_t* dec_group_qblock16;

  // For TransformToPixels.
  float* scratch_space;
  // Note that scratch_space is never used at the same time as dec_group_qblock.
  // Moreover, only one of dec_group_qblock16 is ever used.
  // TODO(veluca): figure out if we can save allocations.

  // AC decoding
  Image3I num_nzeroes[kMaxNumPasses];

 private:
  hwy::AlignedFreeUniquePtr<float[]> float_memory_;
  hwy::AlignedFreeUniquePtr<int32_t[]> int32_memory_;
  hwy::AlignedFreeUniquePtr<int16_t[]> int16_memory_;
  size_t max_block_area_ = 0;
};

}  // namespace jxl

#endif  // LIB_JXL_DEC_CACHE_H_
