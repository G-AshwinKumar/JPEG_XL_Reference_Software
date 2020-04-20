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

#include "hwy/examples/skeleton.h"

#include <assert.h>
#include <stdio.h>
#include "hwy/examples/skeleton_shared.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "hwy/examples/skeleton.cc"
#include "hwy/foreach_target.h"

// Optional: include shared *-inl.h, after foreach_target.h
#include "hwy/examples/skeleton-inl.h"

#undef HWY_USE_GTEST
#include "hwy/tests/test_util.h"  // required if test_util-inl is included.

namespace skeleton {

#include "hwy/begin_target-inl.h"

// Compiled once per target via multiple inclusion (foreach_target.h).
HWY_ATTR void Skeleton(const float* HWY_RESTRICT in1,
                       const float* HWY_RESTRICT in2, float* HWY_RESTRICT out) {
  printf("Target %s: %s\n", hwy::TargetName(HWY_TARGET),
         ExampleGatherStrategy());

  ExampleMulAdd(in1, in2, out);
}

#include "hwy/end_target-inl.h"

#if HWY_ONCE

HWY_EXPORT(Skeleton)

// Optional: anything to compile only once, e.g. non-SIMD implementations of
// public functions provided by this module, can go inside #if HWY_ONCE
// (after end_target-inl.h).

#endif  // HWY_ONCE

}  // namespace skeleton