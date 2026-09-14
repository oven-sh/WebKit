/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS)

#include <cstdint>

// BIR: the serialized form of a compiled C translation unit. Bun's C frontend produces it
// (src/cc/bir.rs is the mirror of this file); CModule lowers it to B3.
//
// Encoding. Little-endian. varuint = unsigned LEB128, varint = signed LEB128,
// str = varuint byte length + UTF-8 bytes, type = u8 (Type below).
//
//   module:
//     magic "BIR7"
//     u8 arch (Arch), u8 os (OS), u8 pointerBytes (8), u8 reserved (0)
//     varuint nsigs;    sig*:    { varuint nrets (0..4); type*; u8 flags (bit0 = variadic); varuint nparams; param* }
//                       param:   u8 kind (ParamKind), then for Value: type;
//                                for ByValStack: varuint size; varuint align; u8 exhausts (Exhausts);
//                                for IndirectResult: nothing
//     varuint nexterns; extern*: { str name; u8 kind (ExternKind); varuint sig }   (sig is 0 and unused for Data)
//     data:             { varuint size; varuint align; varuint readOnly; varuint ninit; u8[ninit];
//                         varuint nrelocs; reloc*: { varuint offset; u8 kind (RelocKind); varuint index; varint addend } }
//                       (the first readOnly bytes are what the program never writes: string literals, const objects. The
//                        whole pages among them cannot be written once the module is loaded, so what follows them starts
//                        on a multiple of 16384 when both parts exist)
//     tls:              { varuint size; varuint align; varuint ninit; u8[ninit];    (the image every thread's copy starts from)
//                         varuint nrelocs; reloc* }                                  (as in data; applied to each copy as it is created.
//                                                                                     kind Tls: the address of that same copy + index)
//     varuint nfuncs;   decl*:   { str name; varuint sig; u8 flags (bit0 = exported, bit1 = calls a function that returns twice: setjmp,
//                                  bit2 = always_inline, bit3 = noinline,
//                                  bit4 = declared `inline`: a hint) }
//                       body*:   { varuint nlocals; type*;                                  (nfuncs bodies, in decl order;
//                                  varuint nslots; slot*: { varuint size; varuint align };   every decl precedes every body so a
//                                  varuint nblocks; block*: { varuint ninsts; inst* } }      Call can name a later function)
//     varuint nexports; export*: { str name; varuint func; u8 ffiRet; varuint nargs; u8 ffiArg* }
//     varuint nlibraries; str*                                   (shared libraries to search for externs, in order,
//                                                                 before the process itself: `#pragma comment(lib, "sqlite3")`)
//     varuint nconstructors; varuint func*                       (`__attribute__((constructor))`: `void f(void)` functions the
//                                                                 loader calls, in this order, once the module is ready to run)
//     varuint ndestructors; varuint func*                        (`__attribute__((destructor))`, in the order they run. The loader
//                                                                 does not call them; it hands them to whoever owns the process's
//                                                                 exit, which must keep the module alive until then)
//
// data is one segment holding every global and string literal: the first ninit bytes are
// initialized, the rest (up to size) is zero. A reloc writes an 8-byte absolute address at
// data[offset]: Data => &data[index] + addend, Func => entry of function `index`,
// Extern => resolved address of extern `index` (+ addend).
//
// tls holds every `_Thread_local` object the module defines. Each thread that runs the module's code
// gets its own copy, created from the image the first time that thread asks for an address in it.
//
// exports give the JS-facing signature in bun:ffi FFIType numbering (JSC::FFI::Type).
//
// Signatures describe the machine-level call, after the frontend has applied the target's C ABI
// to any struct or union passed or returned by value:
//   - Value parameters and results are scalars. Each takes the next free argument (or result)
//     register of its class (integer or floating point), and the stack once those run out, the way
//     the platform passes `long` and `double`. A small struct the ABI passes in registers is
//     therefore 1 to 4 Value parameters; one returned in registers is 1 to 4 results.
//   - ByValStack is `size` bytes copied into the stack argument area at `align` (rounded up to 8):
//     the object itself lives in the caller's outgoing arguments (x86-64 SysV's MEMORY class; an
//     AArch64 composite that no longer fits the registers). `exhausts` marks the register class
//     that AArch64 closes to later arguments when that happens. At a call the operand is the
//     address (i64) of the bytes to copy; in the callee the parameter's value is the address (i64)
//     of its own copy.
//   - IndirectResult is the i64 address the callee writes a memory-class result to. AArch64 passes
//     it in x8; everywhere else it is an ordinary first integer argument, and the frontend uses a
//     Value parameter for it instead.
//   - A struct the ABI passes by reference (Win64, AArch64 above 16 bytes) is a Value i64 pointing
//     at a copy the caller made.
//
// Values. A function's parameters are values 0..nparams-1 (ByValStack and IndirectResult are
// i64). Every instruction that produces
// a result defines the next value id, in instruction order across the whole function. A call
// that returns n results defines n consecutive values. A
// value may only be used in the block that defines it; parameters may be used anywhere.
// Anything that lives across blocks goes through a local (LocalGet/LocalSet) or a slot.
// Locals are zero on entry. Block 0 is the entry block. Every block ends in exactly one
// terminator and has no other terminator.
//
// Instruction operands, in order (v = varuint value id):
//   ConstI32 varint | ConstI64 varint | ConstF32 u32le bits | ConstF64 u64le bits | ConstV128 16 bytes
//   Add Sub Mul Div UDiv Rem URem And Or Xor: v a, v b       (same type in, same type out; Div on floats is fdiv)
//   Shl ShrS ShrU: v a, v amount(i32)                         (result type of a)
//   Neg: v a
//   Clz Ctz Popcnt Bswap: v a(i32|i64) -> same type             (Clz/Ctz of 0 is the bit width)
//   RotL RotR: v a(i32|i64), v amount(i32) -> same type as a         (amount is taken modulo the width)
//   MulHigh UMulHigh: v a, v b (both i32 or both i64) -> same type   (upper half of the full-width product)
//   Eq Ne Lt Le Gt Ge ULt ULe UGt UGe: v a, v b -> i32        (Lt..Ge: signed on ints, ordered on floats; Ne is true when unordered)
//   SExt8 SExt16: v(i32) -> i32 | SExt32 ZExt32: v(i32) -> i64 | Trunc: v(i64) -> i32
//   SToF UToF: type result(f32|f64), v(i32|i64)
//   FToS FToU: type result(i32|i64), v(f32|f64)               (C semantics: out of range is undefined)
//   FPromote: v(f32) -> f64 | FDemote: v(f64) -> f32
//   Bitcast: type result, v                                   (i32<->f32, i64<->f64)
//   Load: u8 kind (MemKind), v addr, varint offset            (I8*/I16*/I32 -> i32, I64 -> i64, F32, F64, V128)
//                                                              (kind | 0x80 on a Load or Store: volatile. It is performed exactly as
//                                                               written: never merged with, forwarded to or reordered against another)
//   Store: u8 kind (MemKind; I8S/I16S are invalid), v value, v addr, varint offset
//   SlotAddr: varuint slot -> i64 | DataAddr: varuint offset -> i64
//   FuncAddr: varuint func -> i64 | ExternAddr: varuint extern -> i64
//   FrameAddress: -> i64                                       (`__builtin_frame_address(0)`: the frame pointer of the function as
//                                                              written; [it] is the caller's frame pointer, [it + 8] the return address)
//   InlineAsm: u8 flags (bit0 = has effects beyond its results: `volatile` or a "memory" clobber),
//              varuint nbytes, u8[nbytes]                       (machine code the FRONTEND assembled, operands already in the
//                                                               registers named below; it must fall out of its end)
//              varuint ninputs, (v value, u8 register)*         (i32/i64 values in integer registers, f32/f64/v128 in vector ones)
//              varuint noutputs, (u8 type, u8 register)*        (-> that many results, numbered like a call's)
//              varuint nclobbers, u8 register*                  (the condition flags are always clobbered)
//     Registers: x86-64 0..15 = rax rcx rdx rbx rsp rbp rsi rdi r8..r15 (rsp/rbp not allowed), 16..31 = xmm0..15;
//                arm64 0..30 = x0..x30 (x18, x29, x30 not allowed), 32..63 = v0..v31. A register may be both an input
//                and an output ("+r"). Inputs are read before any output is written only if the code does so.
//   CpuId: v leaf(i32), v subleaf(i32) -> i32 eax, i32 ebx, i32 ecx, i32 edx    (x86-64 only; four results, like a call's)
//   TlsAddr: varuint offset -> i64                            (address of tls[offset] in the calling thread's copy)
//   LocalGet: varuint local | LocalSet: varuint local, v value
//   Call: varuint func, varuint nargs, v*                     (defines one value per result of the sig;
//                                                              variadic sigs may pass nargs > nparams)
//   CallExtern: varuint extern (kind Function), varuint nargs, v*   (variadic sigs may pass nargs > nparams)
//   CallIndirect: varuint sig, v fnptr, varuint nargs, v*     (variadic sigs may pass nargs > nparams)
//   Select: v cond(i32), v a, v b
//   MemCopy: v dst, v src, v nbytes(i64) | MemSet: v dst, v byte(i32), v nbytes(i64)
//
// 128-bit vectors (type v128). `lane` is a u8 Lane; `signed` is a u8 0/1 that matters only where
// the operation differs by signedness (it is always present where listed). Every operation is
// defined for every lane shape listed for it; where the hardware has no instruction the backend
// expands it lane by lane.
//   VSplat: lane, v scalar -> v128                            (scalar is i32 for I8x16/I16x8/I32x4, else i64/f32/f64)
//   VExtract: lane, signed, u8 index, v -> scalar             (i32 for I8x16/I16x8/I32x4; signed picks sign/zero extension)
//   VReplace: lane, u8 index, v vec, v scalar -> v128
//   VAdd VSub VMul: lane, v a, v b -> v128
//   VDiv VRem: lane, signed, v a, v b -> v128                  (VRem: integer lanes only)
//   VMin VMax: lane, signed, v a, v b -> v128                  (float lanes: like fmin/fmax on non-NaN input)
//   VNeg VAbs: lane, v -> v128 | VSqrt: lane (F32x4|F64x2), v -> v128
//   VAnd VOr VXor: v a, v b -> v128 | VNot: v -> v128
//   VShl VShrS VShrU: lane (integer), v vec, v amount(i32) -> v128   (every lane shifted by amount mod lane width)
//   VEq VNe VLt VLe VGt VGe: lane, signed, v a, v b -> v128   (each lane all ones or all zeros)
//   VSelect: v mask, v a, v b -> v128                          ((mask & a) | (~mask & b), bitwise)
//   VShuffle: v a, v b, 16 x u8 -> v128                        (result byte i = byte index[i] of the 32 bytes a:b)
//   VConvert: u8 kind (VConvertKind), v -> v128
//   VAddSat VSubSat: lane (I8x16|I16x8), signed, v a, v b -> v128   (saturating)
//   VAvgU: lane (I8x16|I16x8), v a, v b -> v128                (unsigned (a + b + 1) >> 1)
//   VExtMul: lane (I16x8|I32x4|I64x2: the RESULT lanes), signed, u8 high (0 = low half, 1 = high half), v a, v b -> v128
//                                                              (multiplies that half of the next-narrower lanes, widening)
//   VNarrow: lane (I16x8|I32x4: the SOURCE lanes), signed, v a, v b -> v128
//                                                              (a's lanes then b's, each saturated to the next-narrower lane; signed picks the range)
//   VDot: v a, v b -> v128                                      (I16x8 signed pairs multiplied and added into I32x4)
//   VSwizzle: v bytes, v indices -> v128                       (result byte i = bytes[indices[i]], 0 when indices[i] >= 16)
//   VBitmask: lane (integer), v -> i32                         (top bit of each lane, lane 0 in bit 0)
//   VAnyTrue: v -> i32 (any bit set) | VAllTrue: lane (integer), v -> i32 (every lane non-zero)
//
// Atomics. `kind` is one of the integer MemKinds (I8S/I16S only for AtomicLoad); `order` is a u8
// MemOrder. The address must be naturally aligned.
//   AtomicLoad: kind, order, v addr -> value
//   AtomicStore: kind, order, v value, v addr
//   AtomicRmw: u8 op (AtomicOp), kind, order, v value, v addr -> the old value (zero-extended)
//   AtomicCas: kind, u8 successOrder, u8 failureOrder, v expected, v desired, v addr -> the old value
//                                                              (strong: it stored `desired` iff old == expected)
//   Fence: order
//
//   StackAlloc: v nbytes(i64), varuint align -> i64            (alloca: released when the function returns, or by StackRestore)
//   StackSave: -> i64 | StackRestore: v(i64)                   (the stack pointer; restoring releases every StackAlloc since the save)
//   VaStart: v va_list(i64)                                   (only in a function whose sig is variadic: initializes the
//                                                              target ABI's va_list object at that address, see below)
//   Jump: varuint block | Br: v cond(i32), varuint then, varuint else   (Br and Select take any non-zero cond as true)
//   Switch: v value(i32|i64), varuint default, varuint ncases, { varint case; varuint block }*
//   Ret: v* (one per result of the function's sig, at least one) | RetVoid | Unreachable | Trap
//                       (Unreachable: control never gets here; Trap: abort the process)
//
// va_list. A function whose sig is variadic may execute VaStart. It fills in the object the
// platform's <stdarg.h> describes, and the frontend implements va_arg/va_copy against it:
//   x86-64 SysV:     struct { u32 gp_offset; u32 fp_offset; void* overflow_arg_area; void* reg_save_area; }
//                    reg_save_area holds rdi,rsi,rdx,rcx,r8,r9 at 0..48 and xmm0..7 at 48 + 16*i.
//   AArch64 (Linux): struct { void* stack; void* gr_top; void* vr_top; i32 gr_offs; i32 vr_offs; }
//                    x0..x7 end at gr_top, q0..q7 (16 bytes each) end at vr_top.
//   AArch64 (Apple), Win64: char*, pointing at the first anonymous argument; every argument takes
//                    an 8-byte slot.

namespace JSC { namespace FFI { namespace BIR {

constexpr uint8_t magic[4] = { 'B', 'I', 'R', '7' };

enum class Arch : uint8_t { X86_64 = 0, ARM64 = 1 };
enum class OS : uint8_t { Linux = 0, Darwin = 1, Windows = 2, FreeBSD = 3 };

enum class Type : uint8_t { Void = 0, I32 = 1, I64 = 2, F32 = 3, F64 = 4, V128 = 5 };

enum class RelocKind : uint8_t { Data = 0, Func = 1, Extern = 2, Tls = 3 /* in the tls segment only */ };

enum class ParamKind : uint8_t { Value = 0, ByValStack = 1, IndirectResult = 2 };
enum class Exhausts : uint8_t { Nothing = 0, IntegerRegisters = 1, FloatRegisters = 2 };

// Function: a function in another library (`int printf(const char*, ...);`).
// Data: an object in another library (`extern FILE* stdout;`); only its address is used.
enum class ExternKind : uint8_t { Function = 0, Data = 1 };

// Or'ed into an extern's kind byte: `__attribute__((weak))`. Its address is null when nothing defines it.
constexpr uint8_t weakExtern = 0x80;

constexpr uint8_t volatileAccess = 0x80; // Or'ed into the MemKind byte of a Load or Store.

enum class MemKind : uint8_t { I8S = 0, I8U = 1, I16S = 2, I16U = 3, I32 = 4, I64 = 5, F32 = 6, F64 = 7, V128 = 8 };

enum class Lane : uint8_t { I8x16 = 0, I16x8 = 1, I32x4 = 2, I64x2 = 3, F32x4 = 4, F64x2 = 5 };

// Lane-wise conversions. "Low"/"High" name which half of the source lanes is read; "Zero" means the
// upper result lanes are zero. Float -> int truncates toward zero and saturates; NaN gives 0.
enum class VConvertKind : uint8_t {
    I32x4ToF32x4S = 0, I32x4ToF32x4U = 1,
    F32x4ToI32x4S = 2, F32x4ToI32x4U = 3,
    I32x4LowToF64x2S = 4, I32x4LowToF64x2U = 5,
    F64x2ToI32x4ZeroS = 6, F64x2ToI32x4ZeroU = 7,
    F32x4LowToF64x2 = 8, F64x2ToF32x4Zero = 9,
    I8x16LowToI16x8S = 10, I8x16LowToI16x8U = 11, I8x16HighToI16x8S = 12, I8x16HighToI16x8U = 13,
    I16x8LowToI32x4S = 14, I16x8LowToI32x4U = 15, I16x8HighToI32x4S = 16, I16x8HighToI32x4U = 17,
    I32x4LowToI64x2S = 18, I32x4LowToI64x2U = 19, I32x4HighToI64x2S = 20, I32x4HighToI64x2U = 21,
    I64x2ToF64x2S = 22, I64x2ToF64x2U = 23, F64x2ToI64x2S = 24, F64x2ToI64x2U = 25,
};

enum class MemOrder : uint8_t { Relaxed = 0, Acquire = 1, Release = 2, AcquireRelease = 3, SequentiallyConsistent = 4 };
enum class AtomicOp : uint8_t { Add = 0, Sub = 1, And = 2, Or = 3, Xor = 4, Exchange = 5 };

#define FOR_EACH_BIR_OP(macro) \
    macro(ConstI32, 0x01) \
    macro(ConstI64, 0x02) \
    macro(ConstF32, 0x03) \
    macro(ConstF64, 0x04) \
    macro(ConstV128, 0x05) \
    macro(Add, 0x10) \
    macro(Sub, 0x11) \
    macro(Mul, 0x12) \
    macro(Div, 0x13) \
    macro(UDiv, 0x14) \
    macro(Rem, 0x15) \
    macro(URem, 0x16) \
    macro(And, 0x17) \
    macro(Or, 0x18) \
    macro(Xor, 0x19) \
    macro(Shl, 0x1a) \
    macro(ShrS, 0x1b) \
    macro(ShrU, 0x1c) \
    macro(Neg, 0x1d) \
    macro(Clz, 0x1e) \
    macro(Ctz, 0x1f) \
    macro(Eq, 0x20) \
    macro(Ne, 0x21) \
    macro(Lt, 0x22) \
    macro(Le, 0x23) \
    macro(Gt, 0x24) \
    macro(Ge, 0x25) \
    macro(ULt, 0x26) \
    macro(ULe, 0x27) \
    macro(UGt, 0x28) \
    macro(UGe, 0x29) \
    macro(Popcnt, 0x2a) \
    macro(Bswap, 0x2b) \
    macro(MulHigh, 0x2c) \
    macro(UMulHigh, 0x2d) \
    macro(RotL, 0x2e) \
    macro(RotR, 0x2f) \
    macro(SExt8, 0x30) \
    macro(SExt16, 0x31) \
    macro(SExt32, 0x32) \
    macro(ZExt32, 0x33) \
    macro(Trunc, 0x34) \
    macro(SToF, 0x35) \
    macro(UToF, 0x36) \
    macro(FToS, 0x37) \
    macro(FToU, 0x38) \
    macro(FPromote, 0x39) \
    macro(FDemote, 0x3a) \
    macro(Bitcast, 0x3b) \
    macro(Load, 0x40) \
    macro(Store, 0x41) \
    macro(SlotAddr, 0x42) \
    macro(DataAddr, 0x43) \
    macro(FuncAddr, 0x44) \
    macro(ExternAddr, 0x45) \
    macro(LocalGet, 0x46) \
    macro(LocalSet, 0x47) \
    macro(Call, 0x50) \
    macro(CallExtern, 0x51) \
    macro(CallIndirect, 0x52) \
    macro(Select, 0x53) \
    macro(MemCopy, 0x54) \
    macro(MemSet, 0x55) \
    macro(VaStart, 0x56) \
    macro(StackAlloc, 0x57) \
    macro(StackSave, 0x58) \
    macro(StackRestore, 0x59) \
    macro(TlsAddr, 0x5a) \
    macro(FrameAddress, 0x5b) \
    macro(CpuId, 0x5c) \
    macro(InlineAsm, 0x5d) \
    macro(VSplat, 0x70) \
    macro(VExtract, 0x71) \
    macro(VReplace, 0x72) \
    macro(VAdd, 0x73) \
    macro(VSub, 0x74) \
    macro(VMul, 0x75) \
    macro(VDiv, 0x76) \
    macro(VRem, 0x77) \
    macro(VMin, 0x78) \
    macro(VMax, 0x79) \
    macro(VNeg, 0x7a) \
    macro(VAbs, 0x7b) \
    macro(VSqrt, 0x7c) \
    macro(VAnd, 0x7d) \
    macro(VOr, 0x7e) \
    macro(VXor, 0x7f) \
    macro(VNot, 0x80) \
    macro(VShl, 0x81) \
    macro(VShrS, 0x82) \
    macro(VShrU, 0x83) \
    macro(VEq, 0x84) \
    macro(VNe, 0x85) \
    macro(VLt, 0x86) \
    macro(VLe, 0x87) \
    macro(VGt, 0x88) \
    macro(VGe, 0x89) \
    macro(VSelect, 0x8a) \
    macro(VShuffle, 0x8b) \
    macro(VConvert, 0x8c) \
    macro(VBitmask, 0x8d) \
    macro(VAnyTrue, 0x8e) \
    macro(VAllTrue, 0x8f) \
    macro(VAddSat, 0x90) \
    macro(VSubSat, 0x91) \
    macro(VAvgU, 0x92) \
    macro(VExtMul, 0x93) \
    macro(VNarrow, 0x94) \
    macro(VDot, 0x95) \
    macro(VSwizzle, 0x96) \
    macro(AtomicLoad, 0xa0) \
    macro(AtomicStore, 0xa1) \
    macro(AtomicRmw, 0xa2) \
    macro(AtomicCas, 0xa3) \
    macro(Fence, 0xa4) \
    macro(Jump, 0x60) \
    macro(Br, 0x61) \
    macro(Switch, 0x62) \
    macro(Ret, 0x63) \
    macro(RetVoid, 0x64) \
    macro(Unreachable, 0x65) \
    macro(Trap, 0x66)

enum class Op : uint8_t {
#define BIR_DEFINE_OP(name, value) name = value,
    FOR_EACH_BIR_OP(BIR_DEFINE_OP)
#undef BIR_DEFINE_OP
};

constexpr bool isTerminator(Op op)
{
    return op >= Op::Jump && op <= Op::Trap;
}

} } } // namespace JSC::FFI::BIR

#endif // USE(BUN_JSC_ADDITIONS)
