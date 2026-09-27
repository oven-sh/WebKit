import * as assert from "../assert.js";
import { watToWasm } from "../gc/wast-wrapper.js";

// Code after an unconditional branch, return, throw or unreachable never runs, but it still has to
// type-check: the operand stack of the block it is in becomes polymorphic (popping from its bottom
// yields a value of any type), and everything pushed after that point has a concrete type again.
// See https://webassembly.github.io/spec/core/appendix/algorithm.html.

function check(bytes, expectedValid, what, message = "")
{
    assert.eq(WebAssembly.validate(bytes), expectedValid, what);
    if (expectedValid) {
        new WebAssembly.Module(bytes);
        return;
    }
    assert.throws(() => new WebAssembly.Module(bytes), WebAssembly.CompileError, message, what);
}

const valid = (wat) => check(watToWasm(wat), true, wat);
const invalid = (wat, message) => check(watToWasm(wat), false, wat, message);

// The operand types of numeric instructions.
invalid(`(module (func unreachable (i64.const 1) i32.add drop))`, "in unreachable context");
invalid(`(module (func (block (br 0) (i64.const 1) i32.add drop)))`);
invalid(`(module (func (return) (f32.const 0) i32.eqz drop))`);
invalid(`(module (func (param i32) (block (block (br_table 0 1 (local.get 0)) (i32.eqz (f32.const 0)) drop))))`);
valid(`(module (func unreachable i32.add drop))`);
valid(`(module (func unreachable (i32.const 1) i32.add drop))`);
valid(`(module (func (result i32) unreachable i32.add))`);
valid(`(module (func (result i32) (block (result i32) (i32.add (br 0 (i32.const 1)) (i32.const 2)))))`);

// What is on the stack when a block ends.
invalid(`(module (func (result f32) unreachable (i32.add (i32.const 1) (i32.const 2))))`);
invalid(`(module (func unreachable (i32.const 0)))`);
invalid(`(module (func (block unreachable (i32.const 0))))`);
invalid(`(module (func (result i32) (block (result i32) unreachable (i64.const 0))))`);
invalid(`(module (func (result i32) (loop (result i32) unreachable (i64.const 0))))`);
invalid(`(module (func (result i32) (i32.const 0) (if (result i32) (then unreachable (f32.const 0)) (else (i32.const 1)))))`);
invalid(`(module (func (result i32) (i32.const 0) (if (result i32) (then (i32.const 1)) (else unreachable (f32.const 0)))))`);
valid(`(module (func (result i32) unreachable))`);
valid(`(module (func (result i32) (block (result i32) (br 0 (i32.const 1)))))`);
valid(`(module (func (result i32) (block (result i32) (br 0 (i32.const 1)) (i32.const 2))))`);
valid(`(module (func (result i32 i64) unreachable (i64.const 0)))`);
valid(`(module (func (i32.const 1) (i32.const 2) (br 0)))`);

// Blocks that unreachable code enters are typed like any other block, and their stack is not polymorphic.
invalid(`(module (func unreachable (block drop)))`);
invalid(`(module (func unreachable (block (result i32))))`);
invalid(`(module (func unreachable (i32.const 0) (if (result i32) (then (f32.const 0)) (else (i64.const 0))) drop))`);
invalid(`(module (func unreachable (if (result i32) (then (i32.const 0))) drop))`);
invalid(`(module (func unreachable (block (result i32) (loop (result i32) (i64.const 0) (br 1))) drop))`);
invalid(`(module (func unreachable (loop (param i32) (f32.const 0) (br 0))))`);
invalid(`(module (func unreachable (block (param i64) i32.eqz drop)))`);
invalid(`(module (func (result i32) unreachable (select) (block (param i32))))`);
valid(`(module (func unreachable (block) drop))`);
valid(`(module (func unreachable (block (br 0) drop)))`);
valid(`(module (func unreachable (if (param i32) (result i32) (then)) drop))`);
valid(`(module (func unreachable (block (result i32) (loop (result i32) (i32.const 0) (br 1))) drop))`);
valid(`(module (func unreachable (loop (param i32) (br 0))))`);
valid(`(module (func unreachable (block (param i64) i64.eqz drop)))`);
valid(`(module (func (result i32) unreachable (select) (block (param i32) (result i32))))`);

// Branches in unreachable code.
invalid(`(module (func (block (result i32) unreachable (br 0 (i64.const 0))) drop))`);
invalid(`(module (func (block (result i32) unreachable (br_if 0 (i64.const 0) (i32.const 1))) drop))`);
invalid(`(module (func (param i32) (block (block (result i32) unreachable (br_table 0 1 (local.get 0))) drop)))`);
invalid(`(module (func (param i32) unreachable (br_table 0 0 (i64.const 0))))`);
valid(`(module (func (block (result i32) unreachable (br 0)) drop))`);
valid(`(module (func (block (result i32) unreachable (br_if 0 (i32.const 0))) drop))`);
valid(`(module (func (param i32) (result i32) (block (result f32) (block (result i32) unreachable (br_table 0 1)) drop (f32.const 0)) drop (i32.const 0)))`);
valid(`(module (func (param i32) (block (block unreachable (br_table 0 1 (local.get 0))))))`);
// The values a polymorphic stack does hold are checked against every br_table target, not only the default.
invalid(`(module (func (block (result i32 f32) (block (result f32 i32) unreachable (f32.const 0) (i32.const 0) (br_table 0 1)) drop drop (i32.const 0) (f32.const 0)) drop drop))`);
valid(`(module (func (block (result f32 i32) (block (result i64 i32) unreachable (i32.const 0) (i32.const 0) (br_table 0 1)) drop drop (f32.const 0) (i32.const 0)) drop drop))`);

// Locals and globals.
invalid(`(module (func (local i32) unreachable (local.set 0 (i64.const 0))))`);
invalid(`(module (func (local f32) unreachable (local.tee 0) i32.eqz drop))`);
invalid(`(module (func unreachable (local.get 0) drop))`);
invalid(`(module (global $g i32 (i32.const 1)) (func unreachable (global.set $g (i32.const 0))))`, "is immutable");
invalid(`(module (global $g (mut i32) (i32.const 1)) (func unreachable (global.set $g (f32.const 0))))`);
invalid(`(module (global $g i64 (i64.const 1)) (func unreachable (global.get $g) i32.eqz drop))`);
invalid(`(module (type $f (func)) (func (local $x (ref $f)) unreachable (block (local.set $x (ref.func 0))) (drop (local.get $x))) (elem declare func 0))`);
valid(`(module (func (local i32) unreachable (local.set 0) (local.tee 0) i32.eqz drop))`);
valid(`(module (type $f (func)) (func (local $x (ref $f)) unreachable (local.set $x (ref.func 0)) (block (local.set $x (ref.func 0))) (drop (local.get $x))) (elem declare func 0))`);

// Calls.
invalid(`(module (func $f (param i32)) (func unreachable (call $f (i64.const 0))))`);
invalid(`(module (func $f (result i64) (i64.const 0)) (func unreachable (call $f) i32.eqz drop))`);
invalid(`(module (type $t (func (param i32))) (table 1 funcref) (func unreachable (call_indirect (type $t) (f32.const 0) (i32.const 0))))`);
invalid(`(module (type $t (func)) (table 1 funcref) (func unreachable (call_indirect (type $t) (i64.const 0))))`);
invalid(`(module (type $t (func (param i32))) (func unreachable (call_ref $t (i64.const 0) (ref.null $t))))`);
invalid(`(module (type $t (func)) (type $u (func (param i32))) (func (param (ref $u)) unreachable (call_ref $t (local.get 0))))`);
invalid(`(module (func $f (result i64) (i64.const 0)) (func (result i32) unreachable (return_call $f)))`);
invalid(`(module (func $f (param f32)) (func unreachable (return_call $f (i32.const 0))))`);
valid(`(module (func $f (param i32) (result i64) (i64.const 0)) (func (result i32) unreachable (call $f) i64.eqz))`);
valid(`(module (type $t (func (param i32) (result i32))) (func (result i32) unreachable (return_call_ref $t (call_ref $t) (ref.null $t))))`);
valid(`(module (func $f (result i32) (i32.const 0)) (func (result i32) unreachable (return_call $f) i64.add i64.eqz))`);

// Memories and tables.
invalid(`(module (memory 1) (func unreachable (i32.store (i32.const 0) (f32.const 0))))`);
invalid(`(module (memory 1) (func unreachable (i64.load (i32.const 0)) i32.eqz drop))`);
invalid(`(module (memory 1) (func unreachable (i32.load (i64.const 0)) drop))`);
invalid(`(module (memory 1) (func unreachable (memory.grow (f32.const 0)) drop))`);
invalid(`(module (memory 1) (func unreachable (memory.fill (i32.const 0) (f32.const 0) (i32.const 0))))`);
invalid(`(module (table $t 1 externref) (func unreachable (table.set $t (i32.const 0) (ref.func 0))) (elem declare func 0))`);
invalid(`(module (table $t 1 funcref) (func (result i32) unreachable (table.get $t (i32.const 0))))`);
valid(`(module (memory 1) (func (result i32) unreachable (f32.store (f32.load)) (memory.grow) memory.size i32.add))`);
valid(`(module (table $t 1 funcref) (func (result funcref) unreachable (table.set $t (i32.const 0) (ref.func 0)) (table.get $t)) (elem declare func 0))`);

// References. A bottom operand is a valid reference operand, and what ref.as_non_null and br_on_null make
// of it is still bottom, as in V8 and SpiderMonkey (the spec's appendix says (ref bot), WebAssembly/spec#2235).
invalid(`(module (func unreachable (ref.is_null (i32.const 0)) drop))`);
invalid(`(module (func unreachable (ref.as_non_null (i32.const 0)) drop))`);
valid(`(module (func unreachable ref.as_non_null i32.eqz drop))`);
valid(`(module (func unreachable (br_on_null 0) i32.eqz drop))`);
invalid(`(module (func unreachable extern.convert_any i32.eqz drop))`);
valid(`(module (func (result (ref extern)) unreachable extern.convert_any))`);
invalid(`(module (type $s (struct (field i32))) (func unreachable (struct.new $s (i64.const 0)) drop))`);
invalid(`(module (type $s (struct (field i32))) (func (param (ref $s)) unreachable (struct.set $s 0 (local.get 0) (i32.const 0))))`);
invalid(`(module (type $s (struct (field i32))) (type $a (array i8)) (func (param (ref $a)) unreachable (struct.get $s 0 (local.get 0)) drop))`);
invalid(`(module (type $a (array (mut i32))) (func (param (ref $a)) unreachable (array.set $a (local.get 0) (i32.const 0) (f64.const 0))))`);
invalid(`(module (type $a (array i64)) (func (result i32) unreachable (array.get $a)))`);
invalid(`(module (func unreachable (ref.cast (ref any) (ref.func 0)) drop) (elem declare func 0))`);
invalid(`(module (func (result externref) unreachable (extern.convert_any (ref.null extern))))`);
invalid(`(module (func unreachable (select (ref.null func) (ref.null func) (i32.const 1)) drop))`);
invalid(`(module (func unreachable select (ref.null func) (i32.const 0) select drop))`);
valid(`(module (func unreachable select (i64.const 0) (i32.const 0) select i64.eqz drop))`);
invalid(`(module (type $f (func)) (func (result (ref $f)) unreachable (block (result funcref) (br_on_non_null 0 (ref.func 0)) (ref.null func))) (elem declare func 0))`);
valid(`(module (func (result i32) unreachable ref.is_null))`);
valid(`(module (func (result (ref any)) unreachable ref.as_non_null))`);
valid(`(module (func (result (ref func)) unreachable ref.as_non_null))`);
valid(`(module (func unreachable ref.as_non_null drop))`);
valid(`(module (func (result anyref) unreachable (br_on_null 0) drop))`);
valid(`(module (type $s (struct (field i32))) (func (result i32) unreachable (struct.get $s 0) (struct.new $s) (struct.get $s 0)))`);
valid(`(module (type $a (array (mut i32))) (func (result (ref $a)) unreachable (array.set $a) (array.new_fixed $a 2)))`);
valid(`(module (func (result i32) unreachable (ref.test (ref i31)) ref.i31 i31.get_s))`);
valid(`(module (func (result anyref) unreachable any.convert_extern))`);
valid(`(module (func (result (ref extern)) unreachable ref.as_non_null extern.convert_any))`);
valid(`(module (type $f (func)) (func (result (ref $f)) unreachable (block (result funcref) (br_on_non_null 0 (ref.func 0)) (ref.null func)) drop ref.as_non_null) (elem declare func 0))`);
valid(`(module (type $s (struct)) (func (param anyref) (result (ref $s)) unreachable (block (result anyref) (br_on_cast_fail 0 anyref (ref $s) (local.get 0)) (br 1)) unreachable))`);

// Vectors.
invalid(`(module (func unreachable (i32x4.splat (f32.const 0)) drop))`);
invalid(`(module (func unreachable (i8x16.extract_lane_s 0 (i32.const 0)) drop))`);
invalid(`(module (func (result i32) unreachable (f32x4.extract_lane 0 (v128.const i32x4 0 0 0 0))))`);
invalid(`(module (memory 1) (func unreachable (v128.store (i32.const 0) (i32.const 0))))`);
valid(`(module (memory 1) (func (result i32) unreachable (i8x16.shuffle 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15) (v128.store) (i32x4.extract_lane 1 (v128.load (i32.const 0)))))`);
valid(`(module (func (result v128) unreachable i32x4.add (i32x4.splat (i32x4.all_true)) i8x16.swizzle))`);

// Exceptions and atomics, which the text format encoder above does not know, as binaries. Each module has
// the types () -> () and (i32) -> (), a tag of each, one memory, and one function () -> () with the given body.
function binary(...body)
{
    const leb128 = (n) => n < 0x80 ? [n] : [0x80 | (n & 0x7f), ...leb128(n >> 7)];
    const section = (id, ...payload) => [id, ...leb128(payload.length), ...payload];
    const code = [0 /* locals */, ...body, 0x0b];
    return new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        ...section(1, 2, 0x60, 0, 0, 0x60, 1, 0x7f, 0),
        ...section(3, 1, 0),
        ...section(5, 1, 0, 1),
        ...section(13, 2, 0, 0, 0, 1),
        ...section(10, 1, ...leb128(code.length), ...code)]);
}
const unreachable = 0x00, block = 0x02, try_ = 0x06, catch_ = 0x07, throw_ = 0x08, rethrow = 0x09, throw_ref = 0x0a, end = 0x0b, br = 0x0c, delegate = 0x18, catch_all = 0x19, drop = 0x1a, try_table = 0x1f;
const i32_const = 0x41, i64_const = 0x42, f32_const = 0x43, i32_eqz = 0x45, epsilon = 0x40, i32 = 0x7f, exnref = 0x69;
const atomic = 0xfe, memory_atomic_notify = 0x00, i32_atomic_load = 0x10, i32_atomic_rmw_add = 0x1e;
const validBinary = (what, ...body) => check(binary(...body), true, what);
const invalidBinary = (what, ...body) => check(binary(...body), false, what);

invalidBinary("throw of an f32 to an i32 tag", unreachable, f32_const, 0, 0, 0, 0, throw_, 1);
invalidBinary("throw_ref of an i32", unreachable, i32_const, 0, throw_ref);
invalidBinary("code after a throw is typed", i32_const, 0, throw_, 1, i64_const, 0, i32_eqz, drop);
invalidBinary("try_table whose catch sends an i32 to a label that takes none", unreachable, try_table, epsilon, 1, 0x00, 1, 0, end);
invalidBinary("try_table whose catch_all_ref sends an exnref to a label that takes none", unreachable, try_table, epsilon, 1, 0x03, 0, end);
invalidBinary("try with an i32 left on its stack", unreachable, try_, epsilon, i32_const, 0, catch_, 0, end);
invalidBinary("catch with the tag's i32 left on its stack", unreachable, try_, epsilon, catch_, 1, end);
invalidBinary("rethrow out of a try that is not a catch", unreachable, try_, epsilon, rethrow, 0, end);
invalidBinary("delegate of a block", unreachable, block, epsilon, delegate, 0);
invalidBinary("a legacy try in a function that also uses try_table, as in V8", try_table, epsilon, 0, end, unreachable, try_, epsilon, catch_all, end);
validBinary("throw, throw_ref and rethrow from a polymorphic stack", unreachable, throw_, 1, throw_ref, try_, epsilon, catch_, 1, i32_eqz, drop, rethrow, 0, end);
validBinary("try_table in unreachable code", unreachable,
    block, i32, try_table, epsilon, 2, 0x00, 1, 0, 0x02, 1, br, 2, end, unreachable, end, drop,
    block, exnref, try_table, epsilon, 1, 0x03, 0, end, unreachable, end, drop);
validBinary("try with catch_all, and delegate, in unreachable code", unreachable,
    try_, i32, i32_const, 0, catch_all, i32_const, 1, end, drop,
    try_, epsilon, try_, epsilon, delegate, 0, delegate, 0);

invalidBinary("i32.atomic.load of an f32 address", unreachable, f32_const, 0, 0, 0, 0, atomic, i32_atomic_load, 2, 0, drop);
invalidBinary("i32.atomic.rmw.add of an i64 value", unreachable, i32_const, 0, i64_const, 0, atomic, i32_atomic_rmw_add, 2, 0, drop);
invalidBinary("memory.atomic.notify leaves an i32", unreachable, atomic, memory_atomic_notify, 2, 0);
validBinary("atomic accesses from a polymorphic stack", unreachable, atomic, i32_atomic_load, 2, 0, atomic, i32_atomic_rmw_add, 2, 0, atomic, memory_atomic_notify, 2, 0, i32_eqz, drop);
