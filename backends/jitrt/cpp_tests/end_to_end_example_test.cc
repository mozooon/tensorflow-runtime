/*
 * Copyright 2022 The TensorFlow Runtime Authors
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

#include <memory>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Conversion/TosaToLinalg/TosaToLinalg.h"
#include "mlir/Dialect/Bufferization/Transforms/Bufferize.h"
#include "mlir/Dialect/Bufferization/Transforms/Passes.h"
#include "mlir/Dialect/Func/Transforms/Passes.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/Tosa/IR/TosaOps.h"
#include "mlir/Transforms/Passes.h"
#include "tfrt/host_context/concurrent_work_queue.h"
#include "tfrt/host_context/host_allocator.h"
#include "tfrt/jitrt/custom_call_registry.h"
#include "tfrt/jitrt/jitrt.h"
#include "tfrt/jitrt/jitrt_compiler.h"
#include "tfrt/support/logging.h"
#include "tfrt/tensor/dense_host_tensor.h"

namespace tfrt {
namespace jitrt {
namespace {

using llvm::SmallVector;
using mlir::LogicalResult;
using mlir::success;

// Features supported in JitRt but missing in this example:
//   1. Launching async tasks.
//   2. Returning async results from the compiled function.

// TODO(ezhulenev): Show all the features supported by JitRt?

// JitRt input program can be defined in arbitrary dialects, the only
// requirement is that the user must pass a pipeline that can lower the input
// program to the LLVM dialect (see `create_compilation_pipeline` option below).
//
// In this example we use Tosa to define the compute function body becase it's
// available upstream, and transpose operation can showcase the input value
// specialization: Tosa can lower to Linalg (and then to LLVM) only transpose
// operations with constant permutation, without input value specialization this
// program can't be lowered to LLVM and executed.
static const char* mlir_module = R"(
  module {
    // Declare your own "runtime" intrinsics library in the compiled module.
    func.func private @my.runtime.intrinsic()
      attributes { rt.custom_call = "my.runtime.intrinsic" }

    // Permutation argument annotated with a jitrt constraint, which means that
    // before compiling the function body, argument must be sunk into the
    // function body as a constant. Otherwise tosa.transpose will not be lowered
    // to Linalg operation.
    func.func @compute(
      %input: tensor<?x?xf32>,
      %perm: tensor<2xi32> { jitrt.constraint = "value" }
    ) -> tensor<?x?xf32> {

      // Pass attributes to the runtime intrinsics.
      func.call @my.runtime.intrinsic() { api_version = 1 : i32 } : () -> ()

      // Transpose input tensor and return result to the caller.
      %transposed = "tosa.transpose"(%input, %perm)
        : (tensor<?x?xf32>, tensor<2xi32>)  -> (tensor<?x?xf32>)

      func.return %transposed : tensor<?x?xf32>
    }
  })";

static const char* entrypoint = "compute";

// Context structure that encapsulats all the state that has to be available
// to your runtime intrinsics.
struct MyRuntimeContext {};

// Implement your runtime intrinsic as a regular C++ function.
static LogicalResult MyRuntimeIntrinsic(MyRuntimeContext* ctx,
                                        int32_t api_version) {
  return success();
}

// Register your runtime support library with JitRt as custom calls.
void RegisterMyRuntimeIntrinsics(CustomCallRegistry* registry) {
  registry->Register(CustomCall::Bind("my.runtime.intrinsic")
                         .UserData<MyRuntimeContext*>()
                         .Attr<int32_t>("api_version")
                         .To(MyRuntimeIntrinsic));
}

// Static registration with the JitRt global registry.
JITRT_STATIC_CUSTOM_CALL_REGISTRATION(RegisterMyRuntimeIntrinsics);

TEST(EndToEndExampleTest, CompiledAndExecute) {
  // Step by step guide for compiling and executing your programs on top of the
  // JitRt library.

  // ------------------------------------------------------------------------ //
  // 1. Set up options for the JitRt executable compilation/recompilation.
  // ------------------------------------------------------------------------ //
  CompilationOptions opts;

  // Because one of the arguments requires value specialization, we must enable
  // specialization to be able to compile the executable.
  opts.specialization = CompilationOptions::Specialization::kEnabled;

  // Define what dialects are supported in the input IR module. If you have your
  // own custom dialects in the input IR you must pass a callback that registers
  // all the dialects that are considered legal for your input program.
  //
  // In this example in addition to "standard" JitRt dialects we add only Tosa.
  opts.register_dialects = [](mlir::DialectRegistry& registry) {
    registry.insert<mlir::tosa::TosaDialect>();
    RegisterDefaultJitRtDialects(registry);
  };

  // Convert all tensors in the compute function signature to memrefs, because
  // tensors do not have any runtime representation and can't be passed across
  // the ABI boundary. The expectation is that compiler pipeline will act
  // according to this calling convention, and the entrypoint will have the same
  // function signature.
  opts.calling_convention = CompilationOptions::DefaultCallingConvention(
      mlir::bufferization::BufferizeTypeConverter());

  // ------------------------------------------------------------------------ //
  // 2. Set up compilation pipeline that lowers input module to LLVM.
  // ------------------------------------------------------------------------ //

  // As a first step we lower from Tosa to Linalg on buffers, and then we rely
  // on a default JitRt compilation pipeline to lower further to LLVM.
  opts.create_compilation_pipeline = [&](mlir::PassManager& pm) {
    // 1. Lower Tosa to Linalg in tensors.
    pm.addNestedPass<mlir::func::FuncOp>(mlir::tosa::createTosaToLinalg());

    // 2. Lower Linalg on tensors to Linalg on buffers.
    pm.addPass(mlir::func::createFuncBufferizePass());
    pm.addNestedPass<mlir::func::FuncOp>(mlir::createLinalgBufferizePass());

    // 3. Clean up IR after lowering to Linalg on buffers.
    pm.addPass(mlir::createCSEPass());
    pm.addPass(mlir::createCanonicalizerPass());

    // 4. Continue compilation using the default JitRt pipeline.
    CompilationPipelineOptions copts;
    CreateDefaultJitRtCompilationPipeline(pm, copts);
  };

  // If your input IR requires specialization, you'll also need to define the
  // `opts.create_compilation_pipeline` callback. In this test we rely on the
  // fact that "value-specialized" arguments will be materialized as constants
  // in the function body.

  // ------------------------------------------------------------------------ //
  // 3. Instantiate JitExecutable from the input MLIR source.
  // ------------------------------------------------------------------------ //

  // JitExecutable does compilation/recompilation from the input source to the
  // Executable artifact.
  llvm::Expected<JitExecutable> jit_executable =
      JitExecutable::Instantiate(mlir_module, entrypoint, opts);
  if (auto err = jit_executable.takeError()) ASSERT_FALSE(err) << StrCat(err);

  // In this example default executable will be in error state, because the
  // program requires value specialization and can't be compiled without it.
  AsyncValuePtr<Executable> default_exec = jit_executable->DefaultExecutable();
  ASSERT_TRUE(default_exec.IsError());

  // ------------------------------------------------------------------------ //
  // 4. Prepare input data for the compiled program.
  // ------------------------------------------------------------------------ //

  // JitRt Executable knows how to pass MemrefDesc to the compiled program
  // according to the MLIR C ABI (memrefs passed as `StridedMemRefType` struct).
  //
  // For "real" programs instead of vectors we should have tensors flying
  // around.

  // Allocate storage for arguments.
  std::vector<float> input = {1.0, 2.0, 3.0, 4.0};
  std::vector<int32_t> perm = {1, 0};

  // Input is a 2x2 memref.
  std::array<int64_t, 2> sizes = {2, 2};
  std::array<int64_t, 2> strides = {2, 1};

  // Prepare memref descriptors for the executable.
  llvm::SmallVector<MemrefDesc> args;
  args.emplace_back(DType::F32, input.data(), 0, sizes, strides);
  args.emplace_back(DType::I32, perm.data(), 0, 2, 1);

  // ------------------------------------------------------------------------ //
  // 5. Prepare options for executing the JitRt executable.
  // ------------------------------------------------------------------------ //

  Executable::ExecuteOpts execute_opts;

  // We don't expect to launch any async tasks in this example.
  execute_opts.async_task_runner =
      reinterpret_cast<jitrt::AsyncTaskRunner*>(0XDEADBEEF);

  // Pass runtime context to all runtime intrinsics handlers.
  MyRuntimeContext runtime_context;

  CustomCall::UserData user_data;
  user_data.insert(&runtime_context);
  execute_opts.custom_call_data = &user_data;

  // ------------------------------------------------------------------------ //
  // 6. Get executable specialized for the concrete operands.
  // ------------------------------------------------------------------------ //

  // At this point we trigger compilation of the original input program for
  // the concrete value of the transpose permutation vector.
  llvm::Expected<AsyncValuePtr<Executable>> executable =
      jit_executable->GetExecutable(args);

  // Await the successful compilation completion.
  if (auto err = executable.takeError()) ASSERT_FALSE(err) << StrCat(err);
  Await(executable->value());

  // ------------------------------------------------------------------------ //
  // 7. Define how to convert returned values back to C++ objects.
  // ------------------------------------------------------------------------ //

  // Conversion context allows to pass data from the caller to the result
  // conversion function (e.g. auxiliary data structures to distinguish newly
  // allocated memrefs from forwarded arguments). In this example we don't pass
  // anything to the conversion functions.
  struct ResultConversionCtx {};
  ResultConversionCtx conversion_ctx;

  // TODO(ezhulenev): We should decouple JitRt from TFRT specific
  // RemainingResults, and do not force clients to deal with returned
  // AsyncValues.

  // Placeholders for returned values.
  unsigned num_results = (*executable)->num_results();
  llvm::SmallVector<RCReference<AsyncValue>> result_values(num_results);
  RemainingResults results(result_values);

  // If execution failed errors will be automatically allocated for all results.
  ReturnValueConverter<ResultConversionCtx> converter(results, conversion_ctx);
  converter.AddConversion(ReturnMemrefAsDenseHostTensor<ResultConversionCtx>);

  // ------------------------------------------------------------------------ //
  // 8. Call JitRt executable with the prepared operands.
  // ------------------------------------------------------------------------ //

  // Execute Jit compiled executable.
  auto err = (*executable)->Execute(args, converter, execute_opts);
  ASSERT_FALSE(err) << "Failed to execute: " << StrCat(err);

  // Check the result returned from the compiled function.
  ASSERT_TRUE(result_values[0]->IsAvailable());

  // Result must be a DenseHostTensor.
  auto& result_tensor = result_values[0]->get<tfrt::DenseHostTensor>();
  ASSERT_EQ(result_tensor.dtype(), DType::F32);
  ASSERT_EQ(result_tensor.NumElements(), 4);

  ArrayRef<float> data = {result_tensor.data<float>(), 4};
  std::vector<float> expected = {1.0, 3.0, 2.0, 4.0};
  EXPECT_EQ(data.vec(), std::vector<float>(expected.begin(), expected.end()));

  // ------------------------------------------------------------------------ //
  // 8. Saving/Restoring JitRt executable to/from object file.
  // ------------------------------------------------------------------------ //

  // See `aot_compilation_test` for an example of serializing JitRt executable
  // as an object file.
}

}  // namespace
}  // namespace jitrt
}  // namespace tfrt
