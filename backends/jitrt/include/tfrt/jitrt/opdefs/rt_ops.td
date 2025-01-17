// Copyright 2021 The TensorFlow Runtime Authors
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

//===- rt_ops.td -------------------------------------------------------===//
//
// Operation definitions for RT dialect.
//
//===----------------------------------------------------------------------===//

#ifdef RT_OPS
#else
#define RT_OPS

include "tfrt/jitrt/opdefs/rt_base.td"

//===----------------------------------------------------------------------===//
// Op definitions.
//===----------------------------------------------------------------------===//

class RT_Op<string mnemonic, list<Trait> traits = []> :
      Op<RuntimeDialect, mnemonic, traits> {
}

//===----------------------------------------------------------------------===//
// SetOutputOp
//===----------------------------------------------------------------------===//

def SetOutputOp : RT_Op<"set_output"> {
  let summary = "set the kernel output to a result value";

  let description = [{
    This operation sets the kernel output at the given index to the result
    value. In kernel functions we do not return the results using the
    conventional return statement, but use the runtime context API to pass
    values back to the runtime.

    Example:

      ```mlir
      func @compute(%ctx: !rt.kernel_context) {
        %out0 = ... : memref<?xf32>
        %out1 = ... : memref<?x?xf32>
        rt.set_output %ctx, 0, %out0 : memref<?xf32>
        rt.set_output %ctx, 1, %out1 : memref<?x?xf32>
      }
      ```

    is an equivalent of a regular function:

      ```mlir
      func @compute() -> (memref<?xf32>, memref<?x?xf32) {
        %out0 = ... : memref<?xf32>
        %out1 = ... : memref<?x?xf32>
        return %out0, %out1 : memref<?xf32>, memref<?x?xf32>
      }
      ```
  }];

  let arguments = (ins
    KernelContextType:$ctx,
    Confined<I64Attr, [IntNonNegative]>:$index,
    AnyType:$value
  );

  let assemblyFormat = [{
    $ctx `,` $index `,` $value `:` type($value) attr-dict
  }];
}

//===----------------------------------------------------------------------===//
// SetErrorOp
//===----------------------------------------------------------------------===//

def SetErrorOp : RT_Op<"set_error"> {
  let summary = "set all kernel outputs to the error state";

  let description = [{
    This operation sets all kernel outputs to the error state. A kernel function
    can call set_error only once, and must not set any of the outputs in this
    case. The provided error message may be used by a runtime to propagate the
    error to the user.

    Example:

      ```mlir
      func @compute(%ctx: !rt.kernel_context) {
        %precondition = arith.cmpi ...
        cond_br %precondition, ^ok, ^err

      ^ok:
        %result = "compute_result"(): () -> memref<?xf32>
        rt.set_output %ctx, 0, %result : memref<?xf32>
        return

      ^err:
        rt.set_error %ctx, "Failed precondition"
        return
      }
      ```
  }];

  let arguments = (ins
    KernelContextType:$ctx,
    StrAttr:$error);

  let assemblyFormat = "$ctx `,` $error attr-dict";
}

//===----------------------------------------------------------------------===//
// IsOkOp
//===----------------------------------------------------------------------===//

def IsOkOp : RT_Op<"is_ok"> {
  let summary = "returns true if status is ok";
  let description = "Checks if the runtime status is ok.";

  let arguments = (ins StatusType:$status);
  let results = (outs I1:$ok);

  let assemblyFormat = "$status attr-dict";
}

//===----------------------------------------------------------------------===//
// CustomCallOp
//===----------------------------------------------------------------------===//

def CustomCallOp : RT_Op<"custom_call"> {
  let summary = "calls a custom function registered with the runtime";

  let description = [{
    This operation calls a custom function registered with the runtime. This
    mechanism allows to call any C++ function from the compiled kernel running
    on top of the JitRt, for example this can be used as an extension mechanism
    to register vendor specific kernels (e.g. call oneDNN convolution).

    Returns `!rt.status` value which can be checked to see if the custom call
    was successful.

    Example:

      ```mlir
      func @compute(%ctx: !rt.kernel_context, %arg0: memref<?xf32>,
                                              %arg1: memref<?xf32>) {
        %status = rt.custom_call %ctx["one_dnn.some_operation"] (%arg0, %arg1)
          : (memref<?xf32>, memref<?xf32>) -> ()
        %0 = rt.is_ok %status
        cf.assert %0, "failed to call one_dnn custom call"
        return
      }
      ```

    To avoid collisions users should group custom calls into libraries and put
    them into namespaces (similar to MLIR dialects). In this example there is
    an assumption that all OneDnn related custom calls will be registered with
    a `one_dnn` prefix.

    Indirect custom calls are resolved at runtime using the custom calls registry,
    which incurs additional overheads because the custom call handler has to
    be looked up by name (expensive string map lookup).

    Direct custom calls are linked to the custom call handler when compiling the
    JitRt executable, and the user must pass a runtime symbol map (see
    compilation options in JitRt header) that binds custom call callees to the
    function pointers implementing the custom call API:

      ```
      bool OneDnnSomeOpImpl(tfrt::jitrt::runtime::KernelContext* ctx,
                            void** args, void** attrs);
      ```
  }];

  let arguments = (ins
    KernelContextType:$ctx,
    StrAttr:$callee,
    UnitAttr:$direct,
    Variadic<AnyType>:$operands
  );

  let results = (outs
    StatusType:$status,
    Variadic<AnyType>:$results
  );

  let assemblyFormat = [{
    (`direct` $direct^)? $ctx `[` $callee `]`  `(` $operands `)`
    attr-dict `:` functional-type($operands, $results)
  }];
}

#endif // RT_OPS
