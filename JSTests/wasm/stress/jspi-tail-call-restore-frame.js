//@ requireOptions("--useJSPI=1", "--useWasmTailCalls=true", "--wasmInliningMaximumWasmCalleeSize=0")

import { instantiate } from "../wabt-wrapper.js"
import * as assert from "../assert.js"

// A tail call can leave a RestoreFrame on the stack between the tail callee and the
// original caller: IPInt writes one for a cross-instance return_call, BBQ and OMG for a
// return_call to any import. When the tail callee then suspends, the JSPI stack scan has
// to accept that frame (and relink it when the stack is implanted again).

// Module A owns the Suspending import and a memory of its own.
let watA = `
(module
  (import "env" "suspending" (func $suspending (param i32) (result i32)))
  (memory (export "mem") 1)
  (data (i32.const 0) "\\2a\\00\\00\\00")  ;; memory[0] = 42

  ;; call -> suspend: a RestoreFrame sits above this frame.
  (func (export "suspend_once") (param i32) (result i32)
    local.get 0
    call $suspending
    (i32.load (i32.const 0))
    i32.add
  )

  ;; return_call -> suspend: the RestoreFrame sits directly above the WasmToJS frame.
  (func (export "tail_suspend") (param i32) (result i32)
    local.get 0
    return_call $suspending
  )

  ;; Two suspensions in one activation: the second one slices a stack that was implanted once already.
  (func (export "suspend_twice") (param i32) (result i32)
    local.get 0
    call $suspending
    call $suspending
    (i32.load (i32.const 0))
    i32.add
  )

  ;; Enough parameters to put some on the stack, so the tail call shifts the argument area.
  (func (export "suspend_many_params")
    (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)
    local.get 0
    local.get 1  i32.add
    local.get 2  i32.add
    local.get 3  i32.add
    local.get 4  i32.add
    local.get 5  i32.add
    local.get 6  i32.add
    local.get 7  i32.add
    local.get 8  i32.add
    local.get 9  i32.add
    call $suspending
    (i32.load (i32.const 0))
    i32.add
  )
)`;

// Module B tail-calls into A. Its memory differs from A's, so a wrong instance after the
// return shows up in the result.
let watB = `
(module
  (import "a" "suspend_once" (func $suspend_once (param i32) (result i32)))
  (import "a" "tail_suspend" (func $tail_suspend (param i32) (result i32)))
  (import "a" "suspend_twice" (func $suspend_twice (param i32) (result i32)))
  (import "a" "suspend_many_params" (func $suspend_many_params
    (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (memory (export "mem") 1)
  (data (i32.const 0) "\\e8\\03\\00\\00")  ;; memory[0] = 1000

  (func (export "tail_suspend_once") (param i32) (result i32)
    local.get 0
    return_call $suspend_once
  )

  (func (export "tail_tail_suspend") (param i32) (result i32)
    local.get 0
    return_call $tail_suspend
  )

  (func (export "tail_suspend_twice") (param i32) (result i32)
    local.get 0
    return_call $suspend_twice
  )

  (func (export "tail_suspend_many_params") (param i32) (result i32)
    local.get 0
    i32.const 1
    i32.const 2
    i32.const 3
    i32.const 4
    i32.const 5
    i32.const 6
    i32.const 7
    i32.const 8
    i32.const 9
    return_call $suspend_many_params
  )

  ;; The RestoreFrame must restore B's instance: after the tail-calling function returns,
  ;; this function reads B's memory.
  (func (export "call_tail_suspend_once") (param i32) (result i32)
    local.get 0
    call $tail_suspend_once_internal
    (i32.load (i32.const 0))
    i32.add
  )
  (func $tail_suspend_once_internal (param i32) (result i32)
    local.get 0
    return_call $suspend_once
  )
)`;

// Module C adds one more cross-instance tail call on top of B.
let watC = `
(module
  (import "b" "tail_suspend_once" (func $tail_suspend_once (param i32) (result i32)))
  (func (export "tail_tail_suspend_once") (param i32) (result i32)
    local.get 0
    return_call $tail_suspend_once
  )
)`;

let suspendCount = 0;
async function suspending(x) {
    suspendCount++;
    await Promise.resolve();
    return x + 100;
}

const rejectionReason = new Error("rejected from the Suspending import");
async function rejecting(x) {
    throw rejectionReason;
}

async function run(instance, name, arg, expected) {
    const promising = WebAssembly.promising(instance.exports[name]);
    for (let i = 0; i < wasmTestLoopCount; i++)
        assert.eq(await promising(arg), expected);
}

async function test() {
    const a = await instantiate(watA, { env: { suspending: new WebAssembly.Suspending(suspending) } }, { tail_call: true });
    const b = await instantiate(watB, { a: a.exports }, { tail_call: true });
    const c = await instantiate(watC, { b: b.exports }, { tail_call: true });

    // A -> return_call suspending, one module: BBQ and OMG write a RestoreFrame for it
    await run(a, "tail_suspend", 0, 0 + 100);
    // B.g -> return_call A.f -> call suspending
    await run(b, "tail_suspend_once", 1, 1 + 100 + 42);
    // B -> return_call A -> return_call suspending
    await run(b, "tail_tail_suspend", 2, 2 + 100);
    // Two suspensions behind one RestoreFrame
    await run(b, "tail_suspend_twice", 3, 3 + 100 + 100 + 42);
    // Stack arguments shifted by the RestoreFrame
    await run(b, "tail_suspend_many_params", 4, 4 + 45 + 100 + 42);
    // B's instance is restored after the resumed tail callee returns
    await run(b, "call_tail_suspend_once", 5, 5 + 100 + 42 + 1000);
    // Two cross-instance tail calls in a row
    await run(c, "tail_tail_suspend_once", 6, 6 + 100 + 42);

    assert.truthy(suspendCount > 0);
}

async function testRejection() {
    const a = await instantiate(watA, { env: { suspending: new WebAssembly.Suspending(rejecting) } }, { tail_call: true });
    const b = await instantiate(watB, { a: a.exports }, { tail_call: true });

    const promising = WebAssembly.promising(b.exports.tail_suspend_once);
    for (let i = 0; i < wasmTestLoopCount; i++) {
        try {
            await promising(1);
            throw new Error("expected a rejection");
        } catch (error) {
            assert.eq(error, rejectionReason);
        }
    }
}

await test();
await testRejection();
