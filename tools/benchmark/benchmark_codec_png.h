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

#ifndef TOOLS_BENCHMARK_BENCHMARK_CODEC_PNG_H_
#define TOOLS_BENCHMARK_BENCHMARK_CODEC_PNG_H_

#include <string>

#include "lib/jxl/base/status.h"
#include "tools/benchmark/benchmark_args.h"
#include "tools/benchmark/benchmark_codec.h"

namespace jxl {
ImageCodec* CreateNewPNGCodec(const BenchmarkArgs& args);

// Registers the png-specific command line options.
Status AddCommandLineOptionsPNGCodec(BenchmarkArgs* args);
}  // namespace jxl

#endif  // TOOLS_BENCHMARK_BENCHMARK_CODEC_PNG_H_
