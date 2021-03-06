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

#include "jxl/encode.h"

#include <algorithm>
#include <cstring>

#include "lib/jxl/aux_out.h"
#include "lib/jxl/base/span.h"
#include "lib/jxl/codec_in_out.h"
#include "lib/jxl/enc_external_image.h"
#include "lib/jxl/enc_file.h"
#include "lib/jxl/enc_icc_codec.h"
#include "lib/jxl/encode_internal.h"
#include "lib/jxl/jpeg/enc_jpeg_data.h"

// Debug-printing failure macro similar to JXL_FAILURE, but for the status code
// JXL_ENC_ERROR
#ifdef JXL_CRASH_ON_ERROR
#define JXL_API_ERROR(format, ...)                                           \
  (::jxl::Debug(("%s:%d: " format "\n"), __FILE__, __LINE__, ##__VA_ARGS__), \
   ::jxl::Abort(), JXL_ENC_ERROR)
#else  // JXL_CRASH_ON_ERROR
#define JXL_API_ERROR(format, ...)                                             \
  (((JXL_DEBUG_ON_ERROR) &&                                                    \
    ::jxl::Debug(("%s:%d: " format "\n"), __FILE__, __LINE__, ##__VA_ARGS__)), \
   JXL_ENC_ERROR)
#endif  // JXL_CRASH_ON_ERROR

namespace jxl {

Status ConvertExternalToInternalColorEncoding(const JxlColorEncoding& external,
                                              ColorEncoding* internal) {
  internal->SetColorSpace(static_cast<ColorSpace>(external.color_space));

  CIExy wp;
  wp.x = external.white_point_xy[0];
  wp.y = external.white_point_xy[1];
  JXL_RETURN_IF_ERROR(internal->SetWhitePoint(wp));

  if (external.color_space == JXL_COLOR_SPACE_RGB ||
      external.color_space == JXL_COLOR_SPACE_UNKNOWN) {
    internal->primaries = static_cast<Primaries>(external.primaries);
    PrimariesCIExy primaries;
    primaries.r.x = external.primaries_red_xy[0];
    primaries.r.y = external.primaries_red_xy[1];
    primaries.g.x = external.primaries_green_xy[0];
    primaries.g.y = external.primaries_green_xy[1];
    primaries.b.x = external.primaries_blue_xy[0];
    primaries.b.y = external.primaries_blue_xy[1];
    JXL_RETURN_IF_ERROR(internal->SetPrimaries(primaries));
  }
  CustomTransferFunction tf;
  if (external.transfer_function == JXL_TRANSFER_FUNCTION_GAMMA) {
    JXL_RETURN_IF_ERROR(tf.SetGamma(external.gamma));
  } else {
    tf.SetTransferFunction(
        static_cast<TransferFunction>(external.transfer_function));
  }
  internal->tf = tf;

  internal->rendering_intent =
      static_cast<RenderingIntent>(external.rendering_intent);

  return true;
}

}  // namespace jxl

uint32_t JxlEncoderVersion(void) {
  return JPEGXL_MAJOR_VERSION * 1000000 + JPEGXL_MINOR_VERSION * 1000 +
         JPEGXL_PATCH_VERSION;
}

constexpr unsigned char container_header[] = {
    0,   0,   0, 0xc, 'J',  'X', 'L', ' ', 0xd, 0xa, 0x87,
    0xa, 0,   0, 0,   0x14, 'f', 't', 'y', 'p', 'j', 'x',
    'l', ' ', 0, 0,   0,    0,   'j', 'x', 'l', ' '};

namespace {
// Extends vec with size, and returns a pointer to the beginning of the
// extension.
uint8_t* ExtendVector(std::vector<uint8_t>* vec, size_t size) {
  vec->resize(vec->size() + size, 0);
  return vec->data() + vec->size() - size;
}
}  // namespace

void JxlEncoderStruct::AppendBoxHeader(const jxl::BoxType& type, size_t size,
                                       bool unbounded) {
  uint64_t box_size = 0;
  bool large_size = false;
  if (!unbounded) {
    box_size = size + 8;
    if (box_size >= 0x100000000ull) {
      large_size = true;
    }
  }

  StoreBE32(large_size ? 1 : box_size, ExtendVector(&output_byte_queue, 4));

  output_byte_queue.insert(output_byte_queue.end(), type.data(),
                           type.data() + 4);

  if (large_size) {
    StoreBE64(box_size, ExtendVector(&output_byte_queue, 8));
  }
}

JxlEncoderStatus JxlEncoderStruct::RefillOutputByteQueue() {
  jxl::MemoryManagerUniquePtr<jxl::JxlEncoderQueuedFrame> input_frame =
      std::move(input_frame_queue[0]);
  input_frame_queue.erase(input_frame_queue.begin());

  jxl::BitWriter writer;

  if (!wrote_headers) {
    if (use_container) {
      output_byte_queue.insert(output_byte_queue.end(), container_header,
                               container_header + sizeof(container_header));
      if (store_jpeg_metadata && jpeg_metadata.size() > 0) {
        AppendBoxHeader(jxl::MakeBoxType("jbrd"), jpeg_metadata.size(), false);
        output_byte_queue.insert(output_byte_queue.end(), jpeg_metadata.begin(),
                                 jpeg_metadata.end());
      }
      AppendBoxHeader(jxl::MakeBoxType("jxlc"), 0, true);
    }
    if (!WriteHeaders(&metadata, &writer, nullptr)) {
      return JXL_ENC_ERROR;
    }
    // Only send ICC (at least several hundred bytes) if fields aren't enough.
    if (metadata.m.color_encoding.WantICC()) {
      if (!jxl::WriteICC(metadata.m.color_encoding.ICC(), &writer,
                         jxl::kLayerHeader, nullptr)) {
        return JXL_ENC_ERROR;
      }
    }

    // TODO(lode): preview should be added here if a preview image is added

    wrote_headers = true;
  }

  // Each frame should start on byte boundaries.
  writer.ZeroPadToByte();

  // TODO(zond): Handle progressive mode like EncodeFile does it.
  // TODO(zond): Handle animation like EncodeFile does it, by checking if
  //             JxlEncoderCloseInput has been called (to see if it's the
  //             last animation frame).

  if (metadata.m.xyb_encoded) {
    input_frame->option_values.cparams.color_transform =
        jxl::ColorTransform::kXYB;
  } else {
    // TODO(zond): Figure out when to use kYCbCr instead.
    input_frame->option_values.cparams.color_transform =
        jxl::ColorTransform::kNone;
  }

  jxl::PassesEncoderState enc_state;
  if (!jxl::EncodeFrame(input_frame->option_values.cparams, jxl::FrameInfo{},
                        &metadata, input_frame->frame, &enc_state,
                        thread_pool.get(), &writer,
                        /*aux_out=*/nullptr)) {
    return JXL_ENC_ERROR;
  }

  jxl::PaddedBytes bytes = std::move(writer).TakeBytes();
  output_byte_queue.insert(output_byte_queue.end(), bytes.data(),
                           bytes.data() + bytes.size());
  last_used_cparams = input_frame->option_values.cparams;
  return JXL_ENC_SUCCESS;
}

JxlEncoderStatus JxlEncoderSetColorEncoding(JxlEncoder* enc,
                                            const JxlColorEncoding* color) {
  if (!jxl::ConvertExternalToInternalColorEncoding(
          *color, &enc->metadata.m.color_encoding)) {
    return JXL_ENC_ERROR;
  }
  return JXL_ENC_SUCCESS;
}

JxlEncoderStatus JxlEncoderSetBasicInfo(JxlEncoder* enc,
                                        const JxlBasicInfo* info) {
  if (!enc->metadata.size.Set(info->xsize, info->ysize)) {
    return JXL_ENC_ERROR;
  }
  if (info->exponent_bits_per_sample) {
    if (info->exponent_bits_per_sample != 8) return JXL_ENC_NOT_SUPPORTED;
    if (info->bits_per_sample == 32) {
      enc->metadata.m.SetFloat32Samples();
    } else {
      return JXL_ENC_NOT_SUPPORTED;
    }
  } else {
    switch (info->bits_per_sample) {
      case 32:
      case 16:
      case 8:
        enc->metadata.m.SetUintSamples(info->bits_per_sample);
        break;
      default:
        return JXL_ENC_ERROR;
        break;
    }
  }
  if (info->alpha_bits > 0 && info->alpha_exponent_bits > 0) {
    return JXL_ENC_NOT_SUPPORTED;
  }
  switch (info->alpha_bits) {
    case 0:
      break;
    case 32:
    case 16:
      enc->metadata.m.SetAlphaBits(16);
      break;
    case 8:
      enc->metadata.m.SetAlphaBits(info->alpha_bits);
      break;
    default:
      return JXL_ENC_ERROR;
      break;
  }
  enc->metadata.m.xyb_encoded = !info->uses_original_profile;
  return JXL_ENC_SUCCESS;
}

JxlEncoderOptions* JxlEncoderOptionsCreate(JxlEncoder* enc,
                                           const JxlEncoderOptions* source) {
  auto opts =
      jxl::MemoryManagerMakeUnique<JxlEncoderOptions>(&enc->memory_manager);
  if (!opts) return nullptr;
  opts->enc = enc;
  if (source != nullptr) {
    opts->values = source->values;
  } else {
    opts->values.lossless = false;
  }
  JxlEncoderOptions* ret = opts.get();
  enc->encoder_options.emplace_back(std::move(opts));
  return ret;
}

JxlEncoderStatus JxlEncoderOptionsSetLossless(JxlEncoderOptions* options,
                                              const JXL_BOOL lossless) {
  options->values.lossless = lossless;
  return JXL_ENC_SUCCESS;
}

JxlEncoderStatus JxlEncoderOptionsSetEffort(JxlEncoderOptions* options,
                                            const int effort) {
  if (effort < 3 || effort > 9) {
    return JXL_ENC_ERROR;
  }
  options->values.cparams.speed_tier = static_cast<jxl::SpeedTier>(10 - effort);
  return JXL_ENC_SUCCESS;
}

JxlEncoderStatus JxlEncoderOptionsSetDistance(JxlEncoderOptions* options,
                                              float distance) {
  if (distance < 0 || distance > 15) {
    return JXL_ENC_ERROR;
  }
  options->values.cparams.butteraugli_distance = distance;
  return JXL_ENC_SUCCESS;
}

JxlEncoder* JxlEncoderCreate(const JxlMemoryManager* memory_manager) {
  JxlMemoryManager local_memory_manager;
  if (!jxl::MemoryManagerInit(&local_memory_manager, memory_manager)) {
    return nullptr;
  }

  void* alloc =
      jxl::MemoryManagerAlloc(&local_memory_manager, sizeof(JxlEncoder));
  if (!alloc) return nullptr;
  JxlEncoder* enc = new (alloc) JxlEncoder();
  enc->memory_manager = local_memory_manager;
  enc->wrote_headers = false;

  return enc;
}

void JxlEncoderReset(JxlEncoder* enc) {
  enc->thread_pool.reset();
  enc->input_frame_queue.clear();
  enc->encoder_options.clear();
  enc->output_byte_queue.clear();
  enc->wrote_headers = false;
  enc->metadata = jxl::CodecMetadata();
  enc->last_used_cparams = jxl::CompressParams();
}

void JxlEncoderDestroy(JxlEncoder* enc) {
  if (enc) {
    // Call destructor directly since custom free function is used.
    enc->~JxlEncoder();
    jxl::MemoryManagerFree(&enc->memory_manager, enc);
  }
}

JxlEncoderStatus JxlEncoderUseContainer(JxlEncoder* enc,
                                        JXL_BOOL use_container) {
  enc->use_container = static_cast<bool>(use_container);
  return JXL_ENC_SUCCESS;
}

JxlEncoderStatus JxlEncoderStoreJPEGMetadata(JxlEncoder* enc,
                                             JXL_BOOL store_jpeg_metadata) {
  enc->store_jpeg_metadata = static_cast<bool>(store_jpeg_metadata);
  return JXL_ENC_SUCCESS;
}

JxlEncoderStatus JxlEncoderSetParallelRunner(JxlEncoder* enc,
                                             JxlParallelRunner parallel_runner,
                                             void* parallel_runner_opaque) {
  if (enc->thread_pool) return JXL_API_ERROR("parallel runner already set");
  enc->thread_pool = jxl::MemoryManagerMakeUnique<jxl::ThreadPool>(
      &enc->memory_manager, parallel_runner, parallel_runner_opaque);
  if (!enc->thread_pool) {
    return JXL_ENC_ERROR;
  }
  return JXL_ENC_SUCCESS;
}

JxlEncoderStatus JxlEncoderAddJPEGFrame(const JxlEncoderOptions* options,
                                        const uint8_t* buffer, size_t size) {
  // TODO(zond): Return error if basic info or color encoding isn't set.
  // TODO(zond): Return error if the input has been closed.

  if (options->enc->metadata.m.xyb_encoded) {
    // Can't XYB encode a lossless JPEG.
    return JXL_ENC_ERROR;
  }

  jxl::CodecInOut io;
  if (!jxl::jpeg::DecodeImageJPG(jxl::Span<const uint8_t>(buffer, size), &io)) {
    return JXL_ENC_ERROR;
  }

  if (options->enc->store_jpeg_metadata) {
    jxl::jpeg::JPEGData data_in = *io.Main().jpeg_data;
    jxl::PaddedBytes jpeg_data;
    if (!EncodeJPEGData(data_in, &jpeg_data)) {
      return JXL_ENC_ERROR;
    }
    options->enc->jpeg_metadata = std::vector<uint8_t>(
        jpeg_data.data(), jpeg_data.data() + jpeg_data.size());
  }

  auto queued_frame = jxl::MemoryManagerMakeUnique<jxl::JxlEncoderQueuedFrame>(
      &options->enc->memory_manager,
      // JxlEncoderQueuedFrame is a struct with no constructors, so we use the
      // default move constructor there.
      jxl::JxlEncoderQueuedFrame{options->values,
                                 jxl::ImageBundle(&options->enc->metadata.m)});
  if (!queued_frame) {
    return JXL_ENC_ERROR;
  }
  queued_frame->frame.SetFromImage(std::move(*io.Main().color()),
                                   io.Main().c_current());
  queued_frame->frame.jpeg_data = std::move(io.Main().jpeg_data);
  queued_frame->frame.color_transform = io.Main().color_transform;
  queued_frame->frame.chroma_subsampling = io.Main().chroma_subsampling;

  if (options->values.lossless) {
    queued_frame->option_values.cparams.SetLossless();
  }

  options->enc->input_frame_queue.emplace_back(std::move(queued_frame));
  return JXL_ENC_SUCCESS;
}

JxlEncoderStatus JxlEncoderAddImageFrame(const JxlEncoderOptions* options,
                                         const JxlPixelFormat* pixel_format,
                                         const void* buffer, size_t size) {
  // TODO(zond): Return error if basic info or color encoding isn't set.
  // TODO(zond): Return error if the input has been closed.
  auto queued_frame = jxl::MemoryManagerMakeUnique<jxl::JxlEncoderQueuedFrame>(
      &options->enc->memory_manager,
      // JxlEncoderQueuedFrame is a struct with no constructors, so we use the
      // default move constructor there.
      jxl::JxlEncoderQueuedFrame{options->values,
                                 jxl::ImageBundle(&options->enc->metadata.m)});
  if (!queued_frame) {
    return JXL_ENC_ERROR;
  }

  jxl::ColorEncoding c_current;
  if (options->enc->metadata.m.xyb_encoded) {
    if (pixel_format->data_type == JXL_TYPE_FLOAT) {
      c_current =
          jxl::ColorEncoding::LinearSRGB(pixel_format->num_channels < 3);
    } else {
      c_current = jxl::ColorEncoding::SRGB(pixel_format->num_channels < 3);
    }
  } else {
    c_current = options->enc->metadata.m.color_encoding;
  }

  if (!jxl::BufferToImageBundle(*pixel_format, options->enc->metadata.xsize(),
                                options->enc->metadata.ysize(), buffer, size,
                                options->enc->thread_pool.get(), c_current,
                                &(queued_frame->frame))) {
    return JXL_ENC_ERROR;
  }

  if (options->values.lossless) {
    queued_frame->option_values.cparams.SetLossless();
  }

  options->enc->input_frame_queue.emplace_back(std::move(queued_frame));
  return JXL_ENC_SUCCESS;
}

void JxlEncoderCloseInput(JxlEncoder* enc) {
  // TODO(zond): Make this function mark the most recent frame as the last.
}

JxlEncoderStatus JxlEncoderProcessOutput(JxlEncoder* enc, uint8_t** next_out,
                                         size_t* avail_out) {
  while (*avail_out > 0 &&
         (!enc->output_byte_queue.empty() || !enc->input_frame_queue.empty())) {
    if (!enc->output_byte_queue.empty()) {
      size_t to_copy = std::min(*avail_out, enc->output_byte_queue.size());
      memcpy(static_cast<void*>(*next_out), enc->output_byte_queue.data(),
             to_copy);
      *next_out += to_copy;
      *avail_out -= to_copy;
      enc->output_byte_queue.erase(enc->output_byte_queue.begin(),
                                   enc->output_byte_queue.begin() + to_copy);
    } else if (!enc->input_frame_queue.empty()) {
      if (enc->RefillOutputByteQueue() != JXL_ENC_SUCCESS) {
        return JXL_ENC_ERROR;
      }
    }
  }

  if (!enc->output_byte_queue.empty() || !enc->input_frame_queue.empty()) {
    return JXL_ENC_NEED_MORE_OUTPUT;
  }
  return JXL_ENC_SUCCESS;
}

JXL_EXPORT void JxlColorEncodingSetToSRGB(JxlColorEncoding* color_encoding,
                                          JXL_BOOL is_gray) {
  ConvertInternalToExternalColorEncoding(jxl::ColorEncoding::SRGB(is_gray),
                                         color_encoding);
}

JXL_EXPORT void JxlColorEncodingSetToLinearSRGB(
    JxlColorEncoding* color_encoding, JXL_BOOL is_gray) {
  ConvertInternalToExternalColorEncoding(
      jxl::ColorEncoding::LinearSRGB(is_gray), color_encoding);
}
