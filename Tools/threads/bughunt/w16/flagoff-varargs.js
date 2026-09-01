// flagoff-varargs.js — flag-off (GIL-on, NO thread flags) identity gate for
// the A1 varargs fix family. Exercises exactly the flag-off-shared code the
// diff touched, with no dependence on the Thread/Lock globals:
//  - Interpreter.cpp setupVarargsFrame rewrite (direct pointer arithmetic +
//    RELEASE_ASSERT(newCallFrame < callFrame)): Function.prototype.apply,
//    Reflect.apply, spread calls, construct-varargs, arguments forwarding,
//    large and tiny argument counts, getter-throwing array-likes;
//  - LLIntSlowPaths varargsSetup RELEASE_ASSERTs + thread_local echoes
//    (run a long no-JIT-warmup loop so the LLInt slow path stays hot);
//  - ExceptionScope ctor reorder (Debug/EXCEPTION_SCOPE_VERIFICATION):
//    heavy throw/catch traffic incl. throws during varargs setup.
// PASS criterion: checksum matches the engine-independent expected value
// computed inline (pure JS), and no assert/crash.
"use strict";
function sum() { let s = 0; for (let i = 0; i < arguments.length; ++i) s += arguments[i]; return s; }
function fwd() { return sum.apply(null, arguments); }
class Pt { constructor(a, b, c) { this.v = (a | 0) + (b | 0) * 3 + (c | 0) * 7; } }

let chk = 0;
const small = [1, 2];
const big = new Array(2000); for (let i = 0; i < big.length; ++i) big[i] = i & 15;
const arrayLike = { length: 5, 0: 1, 1: 2, 2: 3, 3: 4, 4: 5 };
const evil = { length: 3, get 0() { return 1; }, get 1() { throw new Error("e1"); }, get 2() { return 3; } };

for (let i = 0; i < 200000; ++i) {
    chk += sum.apply(null, small);                 // 3
    chk += fwd(1, 2, 3, 4);                        // arguments forwarding, 10
    chk += Reflect.apply(sum, null, arrayLike);    // 15
    chk += Math.max(...small, i & 7);              // spread call
    chk += new Pt(...small, i & 3).v;              // construct varargs
    if (!(i & 63)) {
        chk += sum.apply(null, big);               // big frame: 2000 * avg 7.5
        try { sum.apply(null, Array.prototype.slice.call(evil)); } catch (e) { chk += e.message.length; }
        try { Reflect.apply(sum, null, { get length() { throw new Error("len"); } }); } catch (e) { chk += e.message.length; }
    }
    if (!(i & 255)) {
        try { throw new RangeError("r" + (i & 7)); } catch (e) { chk += e.message.length; } // ExceptionScope traffic
    }
}
// Expected value, recomputed structurally (same math, different code path).
let exp = 0;
for (let i = 0; i < 200000; ++i) {
    exp += 3 + 10 + 15 + Math.max(1, 2, i & 7) + (1 + 2 * 3 + (i & 3) * 7);
    if (!(i & 63)) { let b = 0; for (let j = 0; j < 2000; ++j) b += j & 15; exp += b + 2 + 3; }
    if (!(i & 255)) exp += 2;
}
if (chk !== exp)
    throw new Error("MISMATCH chk=" + chk + " exp=" + exp);
print("FLAGOFF-VARARGS PASS chk=" + chk);
