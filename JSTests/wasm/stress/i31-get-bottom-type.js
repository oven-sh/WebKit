//@ runDefaultWasm("-m", "--useConcurrentJIT=0", "--thresholdForBBQOptimizeAfterWarmUp=10", "--thresholdForOMGOptimizeAfterWarmUp=20")

import { instantiate } from "../gc/wast-wrapper.js";
import * as assert from "../assert.js";

// i31.get_s and i31.get_u accept any subtype of i31ref, including the
// bottom type none. A null operand traps at runtime.
function testI31GetBottom() {
  for (const op of ["i31.get_s", "i31.get_u"]) {
    const m = instantiate(`
      (module
        (func (export "f") (result i32)
          (${op} (ref.null none))))
    `);
    for (let i = 0; i < wasmTestLoopCount; i++)
      assert.throws(() => m.exports.f(), WebAssembly.RuntimeError, "i31.get_<sx> to a null reference");
  }
}

// any.convert_extern accepts any subtype of externref, including the
// bottom type noextern.
function testAnyConvertExternBottom() {
  const m = instantiate(`
    (module
      (func (export "f") (result anyref)
        (any.convert_extern (ref.null noextern))))
  `);
  for (let i = 0; i < wasmTestLoopCount; i++)
    assert.eq(m.exports.f(), null);
}

// After a reachable block end, the BBQ pass keeps the precise type of the
// block result, here (ref null none), while the validating pass used the
// declared i31ref. The i31.get_s type check then failed only in the BBQ
// pass, and BBQPlan::work dereferenced the failed compilation result
// (https://github.com/oven-sh/bun/issues/40770).
function testI31GetBrOnNullBlockResult() {
  const m = instantiate(`
    (module
      (func (export "f") (result i32)
        (block $l1
          (return
            (i31.get_s
              (br_on_null $l1
                (block $l2 (result i31ref)
                  (ref.null none))))))
        (i32.const -1)))
  `);
  for (let i = 0; i < wasmTestLoopCount; i++)
    assert.eq(m.exports.f(), -1);
}

testI31GetBottom();
testAnyConvertExternBottom();
testI31GetBrOnNullBlockResult();
