/*
 * Copyright 2021 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef TFRT_BACKENDS_JITRT_CONVERSION_RT_PASSES_H_
#define TFRT_BACKENDS_JITRT_CONVERSION_RT_PASSES_H_

#include <memory>

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "tfrt/jitrt/conversion/custom_call_to_llvm.h"

namespace tfrt {
namespace jitrt {

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>>
CreateConvertRuntimeToLLVMPass(
    CustomCallArgEncodingSet arg_encoding = DefaultArgEncodings(),
    CustomCallAttrEncodingSet attr_encoding = DefaultAttrEncodings());

#define GEN_PASS_REGISTRATION
#include "tfrt/jitrt/conversion/rt_gen_passes.h.inc"

}  // namespace jitrt
}  // namespace tfrt

#endif  // TFRT_BACKENDS_JITRT_CONVERSION_RT_PASSES_H_
