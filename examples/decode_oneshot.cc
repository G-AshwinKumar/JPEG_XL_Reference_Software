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

// This example decodes a JPEG XL image in one shot (all input bytes available
// at once). The example outputs the pixels and color information to a floating
// point image and an ICC profile on disk.

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include <vector>

#include "jxl/decode.h"
#include "jxl/thread_parallel_runner.h"

/** Decodes JPEG XL image to floating point pixels and ICC Profile. Pixel are
 * stored as little endian 32-bit floating point bytes in pixels, as interleaved
 * RGBA, line per line from top to bottom. The output has 16 bytes per pixel.
 * Pixel values have nominal range 0..1 but may go beyond this range for HDR
 * or wide gamut. The ICC profile describes the color format of the pixel data.
 */
bool DecodeJpegXlOneShot(const uint8_t* jxl, size_t size,
                         std::vector<uint8_t>* pixels, size_t* xsize,
                         size_t* ysize, std::vector<uint8_t>* icc_profile) {
  const uint8_t* next_in = jxl;
  size_t avail_in = size;
  JxlDecoder* dec = JxlDecoderCreate(NULL);

  if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(
                             dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
                                      JXL_DEC_FULL_IMAGE)) {
    fprintf(stderr, "JxlDecoderSubscribeEvents failed\n");
    JxlDecoderDestroy(dec);
    return false;
  }

  void* runner = JxlThreadParallelRunnerCreate(
      NULL, JxlThreadParallelRunnerDefaultNumWorkerThreads());
  if (JXL_DEC_SUCCESS !=
      JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner)) {
    fprintf(stderr, "JxlDecoderSetParallelRunner failed\n");
    JxlThreadParallelRunnerDestroy(runner);
    JxlDecoderDestroy(dec);
    return false;
  }

  JxlBasicInfo info;

  bool success = false;

  for (;;) {
    JxlDecoderStatus status =
        JxlDecoderProcessInput(dec, (const uint8_t**)&next_in, &avail_in);

    if (status == JXL_DEC_ERROR) {
      fprintf(stderr, "Decoder error\n");
      break;
    } else if (status == JXL_DEC_NEED_MORE_INPUT) {
      fprintf(stderr, "Error, already provided all input\n");
      break;
    } else if (status == JXL_DEC_BASIC_INFO) {
      if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) {
        fprintf(stderr, "JxlDecoderGetBasicInfo failed\n");
        break;
      }
      *xsize = info.xsize;
      *ysize = info.ysize;
      JxlPixelFormat format = {4, JXL_LITTLE_ENDIAN, JXL_TYPE_FLOAT};
      size_t buffer_size;
      if (JXL_DEC_SUCCESS !=
          JxlDecoderImageOutBufferSize(dec, &format, &buffer_size)) {
        fprintf(stderr, "JxlDecoderImageOutBufferSize failed\n");
        break;
      }
      if (buffer_size != *xsize * *ysize * 16) {
        fprintf(stderr, "Invalid out buffer size %zu %zu\n", buffer_size,
                *xsize * *ysize * 16);
        break;
      }
      pixels->resize(buffer_size);
      if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format,
                                                         pixels->data(),
                                                         pixels->size())) {
        fprintf(stderr, "JxlDecoderSetImageOutBuffer failed\n");
        break;
      }

    } else if (status == JXL_DEC_COLOR_ENCODING) {
      // Get the ICC color profile of the pixel data
      size_t icc_size;
      if (JXL_DEC_SUCCESS !=
          JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                      &icc_size)) {
        fprintf(stderr, "JxlDecoderGetICCProfileSize failed\n");
        break;
      }
      icc_profile->resize(icc_size);
      if (JXL_DEC_SUCCESS != JxlDecoderGetColorAsICCProfile(
                                 dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                 icc_profile->data(), icc_profile->size())) {
        fprintf(stderr, "JxlDecoderGetColorAsICCProfile failed\n");
        break;
      }
    } else if (status == JXL_DEC_FULL_IMAGE) {
      // This means the decoder has decoded all pixels into the buffer.
      success = true;
      break;
    } else if (status == JXL_DEC_SUCCESS) {
      fprintf(stderr, "Decoding finished before receiving pixel data\n");
      break;
    } else {
      fprintf(stderr, "Unknown decoder status\n");
      break;
    }
  }

  JxlThreadParallelRunnerDestroy(runner);
  JxlDecoderDestroy(dec);
  return success;
}

/** Writes to .pfm file (Portable FloatMap). Gimp, tev viewer and ImageMagick
 * support viewing this format.
 * The input pixels are given as 32-bit little endian floating point
 * RGBA, 16 bytes per pixel.
 */
bool WritePFM(const char* filename, const uint8_t* pixels, size_t xsize,
              size_t ysize) {
  FILE* file = fopen(filename, "wb");
  if (!file) {
    fprintf(stderr, "Could not open %s for writing", filename);
    return false;
  }
  fprintf(file, "PF\n%d %d\n-1.0\n", (int)xsize, (int)ysize);
  for (int y = ysize - 1; y >= 0; y--) {
    for (size_t x = 0; x < xsize; x++) {
      for (size_t c = 0; c < 3; c++) {
        const uint8_t* f = &pixels[(y * xsize + x) * 16 + c * 4];
        fwrite(f, 4, 1, file);
      }
    }
  }
  if (fclose(file) != 0) {
    return false;
  }
  return true;
}

bool LoadFile(const char* filename, std::vector<uint8_t>* out) {
  FILE* file = fopen(filename, "rb");
  if (!file) {
    return false;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return false;
  }

  long size = ftell(file);
  // Avoid invalid file or directory.
  if (size >= LONG_MAX || size < 0) {
    fclose(file);
    return false;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return false;
  }

  out->resize(size);
  size_t readsize = fread(out->data(), 1, size, file);
  if (fclose(file) != 0) {
    return false;
  }

  return readsize == static_cast<size_t>(size);
}

bool WriteFile(const char* filename, const uint8_t* data, size_t size) {
  FILE* file = fopen(filename, "wb");
  if (!file) {
    fprintf(stderr, "Could not open %s for writing", filename);
    return false;
  }
  fwrite(data, 1, size, file);
  if (fclose(file) != 0) {
    return false;
  }
  return true;
}

int main(int argc, char* argv[]) {
  if (argc != 4) {
    fprintf(stderr,
            "Usage: %s <jxl> <pfm> <icc>\n"
            "Where:\n"
            "  jxl = input JPEG XL image filename\n"
            "  pfm = output Portable FloatMap image filename\n"
            "  icc = output ICC color profile filename\n"
            "Output files will be overwritten.\n",
            argv[0]);
    return 1;
  }

  const char* jxl_filename = argv[1];
  const char* pfm_filename = argv[2];
  const char* icc_filename = argv[3];

  std::vector<uint8_t> jxl;
  if (!LoadFile(jxl_filename, &jxl)) {
    fprintf(stderr, "couldn't load %s\n", jxl_filename);
    return 1;
  }

  std::vector<uint8_t> pixels;
  std::vector<uint8_t> icc_profile;
  size_t xsize = 0, ysize = 0;
  if (!DecodeJpegXlOneShot(jxl.data(), jxl.size(), &pixels, &xsize, &ysize,
                           &icc_profile)) {
    fprintf(stderr, "Error while decoding the jxl file\n");
    return 1;
  }
  if (!WritePFM(pfm_filename, pixels.data(), xsize, ysize)) {
    fprintf(stderr, "Error while writing the PFM image file\n");
    return 1;
  }
  if (!WriteFile(icc_filename, icc_profile.data(), icc_profile.size())) {
    fprintf(stderr, "Error while writing the ICC profile file\n");
    return 1;
  }
  printf("Successfully wrote %s and %s\n", pfm_filename, icc_filename);
  return 0;
}