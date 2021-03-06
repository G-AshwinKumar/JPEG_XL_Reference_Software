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

#ifndef LIB_JXL_MODULAR_ENCODING_ENCODING_H_
#define LIB_JXL_MODULAR_ENCODING_ENCODING_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "lib/jxl/aux_out_fwd.h"
#include "lib/jxl/base/compiler_specific.h"
#include "lib/jxl/base/padded_bytes.h"
#include "lib/jxl/base/span.h"
#include "lib/jxl/dec_ans.h"
#include "lib/jxl/enc_ans.h"
#include "lib/jxl/enc_bit_writer.h"
#include "lib/jxl/image.h"
#include "lib/jxl/modular/encoding/context_predict.h"
#include "lib/jxl/modular/encoding/ma.h"
#include "lib/jxl/modular/modular_image.h"
#include "lib/jxl/modular/options.h"
#include "lib/jxl/modular/transform/transform.h"

namespace jxl {

constexpr int32_t kWPPropRange = 512;

struct GroupHeader : public Fields {
  GroupHeader();

  const char *Name() const override { return "GroupHeader"; }

  Status VisitFields(Visitor *JXL_RESTRICT visitor) override {
    JXL_QUIET_RETURN_IF_ERROR(visitor->Bool(false, &use_global_tree));
    JXL_QUIET_RETURN_IF_ERROR(visitor->VisitNested(&wp_header));
    uint32_t num_transforms = static_cast<uint32_t>(transforms.size());
    JXL_QUIET_RETURN_IF_ERROR(visitor->U32(Val(0), Val(1), BitsOffset(4, 2),
                                           BitsOffset(8, 18), 0,
                                           &num_transforms));
    if (visitor->IsReading()) transforms.resize(num_transforms);
    for (size_t i = 0; i < num_transforms; i++) {
      JXL_QUIET_RETURN_IF_ERROR(visitor->VisitNested(&transforms[i]));
    }
    return true;
  }

  bool use_global_tree;
  weighted::Header wp_header;

  std::vector<Transform> transforms;
};

FlatTree FilterTree(const Tree &global_tree,
                    std::array<pixel_type, kNumStaticProperties> &static_props,
                    size_t *num_props, bool *use_wp, bool *wp_only);

// TODO(veluca): make cleaner interfaces.

// undo_transforms == N > 0: undo all transforms except the first N
//                           (e.g. to represent YCbCr420 losslessly)
// undo_transforms == 0: undo all transforms
// undo_transforms == -1: undo all transforms but don't clamp to range
// undo_transforms == -2: don't undo any transform
Status ModularGenericDecompress(BitReader *br, Image &image,
                                GroupHeader *header, size_t group_id,
                                ModularOptions *options,
                                int undo_transforms = -1,
                                const Tree *tree = nullptr,
                                const ANSCode *code = nullptr,
                                const std::vector<uint8_t> *ctx_map = nullptr,
                                bool allow_truncated_group = false);
}  // namespace jxl

#endif  // LIB_JXL_MODULAR_ENCODING_ENCODING_H_
