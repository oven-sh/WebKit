// Minimal BIR assembler for tests. Mirrors Source/JavaScriptCore/ffi/BIR.h.
const T = { void: 0, i32: 1, i64: 2, f32: 3, f64: 4, v128: 5 };
const FFI = { char: 0, i8: 1, u8: 2, i16: 3, u16: 4, i32: 5, u32: 6, i64: 7, u64: 8, f64: 9, f32: 10, bool: 11, ptr: 12, void: 13 };
const OP = { ConstI32: 0x01, ConstI64: 0x02, ConstF32: 0x03, ConstF64: 0x04, ConstV128: 0x05, VSplat: 0x70, VExtract: 0x71, VReplace: 0x72, VAdd: 0x73, VSub: 0x74, VMul: 0x75, VDiv: 0x76, VRem: 0x77, VMin: 0x78, VMax: 0x79, VNeg: 0x7a, VAbs: 0x7b, VSqrt: 0x7c, VAnd: 0x7d, VOr: 0x7e, VXor: 0x7f, VNot: 0x80, VShl: 0x81, VShrS: 0x82, VShrU: 0x83, VEq: 0x84, VNe: 0x85, VLt: 0x86, VLe: 0x87, VGt: 0x88, VGe: 0x89, VSelect: 0x8a, VShuffle: 0x8b, VConvert: 0x8c, VBitmask: 0x8d, VAnyTrue: 0x8e, VAllTrue: 0x8f, AtomicLoad: 0xa0, AtomicStore: 0xa1, AtomicRmw: 0xa2, AtomicCas: 0xa3, Fence: 0xa4, Add: 0x10, Sub: 0x11, Mul: 0x12, Div: 0x13, UDiv: 0x14, Rem: 0x15, URem: 0x16, And: 0x17, Or: 0x18, Xor: 0x19, Shl: 0x1a, ShrS: 0x1b, ShrU: 0x1c, Neg: 0x1d, Clz: 0x1e, Ctz: 0x1f, Popcnt: 0x2a, Bswap: 0x2b, MulHigh: 0x2c, UMulHigh: 0x2d, RotL: 0x2e, RotR: 0x2f, VAddSat: 0x90, VSubSat: 0x91, VAvgU: 0x92, VExtMul: 0x93, VNarrow: 0x94, VDot: 0x95, VSwizzle: 0x96, VaStart: 0x56, StackAlloc: 0x57, StackSave: 0x58, StackRestore: 0x59, TlsAddr: 0x5a, FrameAddress: 0x5b, CpuId: 0x5c, InlineAsm: 0x5d, Trap: 0x66, Eq: 0x20, Ne: 0x21, Lt: 0x22, Le: 0x23, Gt: 0x24, Ge: 0x25, ULt: 0x26, ULe: 0x27, UGt: 0x28, UGe: 0x29, SExt8: 0x30, SExt16: 0x31, SExt32: 0x32, ZExt32: 0x33, Trunc: 0x34, SToF: 0x35, UToF: 0x36, FToS: 0x37, FToU: 0x38, FPromote: 0x39, FDemote: 0x3a, Bitcast: 0x3b, Load: 0x40, Store: 0x41, SlotAddr: 0x42, DataAddr: 0x43, FuncAddr: 0x44, ExternAddr: 0x45, LocalGet: 0x46, LocalSet: 0x47, Call: 0x50, CallExtern: 0x51, CallIndirect: 0x52, Select: 0x53, MemCopy: 0x54, MemSet: 0x55, Jump: 0x60, Br: 0x61, Switch: 0x62, Ret: 0x63, RetVoid: 0x64, Unreachable: 0x65 };
const MEM = { i8s: 0, i8u: 1, i16s: 2, i16u: 3, i32: 4, i64: 5, f32: 6, f64: 7, v128: 8 };
const LANE = { i8x16: 0, i16x8: 1, i32x4: 2, i64x2: 3, f32x4: 4, f64x2: 5 };

class W {
    constructor() { this.b = []; }
    u8(v) { this.b.push(v & 0xff); return this; }
    uv(v) { v = BigInt(v); do { let byte = Number(v & 0x7fn); v >>= 7n; if (v) byte |= 0x80; this.b.push(byte); } while (v); return this; }
    sv(v) { v = BigInt(v); for (;;) { let byte = Number(v & 0x7fn); v >>= 7n; const done = (v === 0n && !(byte & 0x40)) || (v === -1n && (byte & 0x40)); if (!done) byte |= 0x80; this.b.push(byte); if (done) break; } return this; }
    str(s) { const bytes = [...s].map(c => c.charCodeAt(0)); this.uv(bytes.length); this.b.push(...bytes); return this; }
    f64(v) { const dv = new DataView(new ArrayBuffer(8)); dv.setFloat64(0, v, true); for (let i = 0; i < 8; i++) this.b.push(dv.getUint8(i)); return this; }
    raw(bytes) { this.b.push(...bytes); return this; }
}

// module = { sigs: [{ret, params, variadic}], externs: [{name, sig}], data: {size, align, init: [], relocs: []},
//            funcs: [{name, sig, exported, locals: [], slots: [], blocks: [[inst...]]}], exports: [{name, func, ret, args}] }
// inst = [opName, ...operands] where operands are already in wire order; numbers tagged via helper objects:
//   {s: n} signed varint, {f64: x} raw f64, {u8: n} byte, plain number => varuint
function assemble(m) {
    const w = new W();
    // These modules use nothing that differs between targets, so they say they are for whichever this is.
    const [arch, os] = $vm.cModuleHost();
    w.raw([0x42, 0x49, 0x52, 0x36]).u8(arch).u8(os).u8(8).u8(0);
    w.uv(m.sigs.length);
    // sig = { ret: type | [types], variadic, params: [type | {byval: size, align, exhausts} | {sret: true}] }
    for (const s of m.sigs) {
        const rets = Array.isArray(s.ret) ? s.ret : s.ret === T.void ? [] : [s.ret];
        w.uv(rets.length); rets.forEach(r => w.u8(r));
        w.u8(s.variadic ? 1 : 0).uv(s.params.length);
        for (const p of s.params) {
            if (typeof p === "number") w.u8(0).u8(p);
            else if (p.byval) w.u8(1).uv(p.byval).uv(p.align || 8).u8(p.exhausts || 0);
            else w.u8(2);
        }
    }
    const externs = m.externs || [];
    w.uv(externs.length);
    for (const e of externs) w.str(e.name).u8(e.kind || 0).uv(e.sig);
    const d = m.data || { size: 0, align: 1, init: [], relocs: [] };
    w.uv(d.size).uv(d.align).uv(d.init.length).raw(d.init).uv(d.relocs.length);
    for (const r of d.relocs) w.uv(r.offset).u8(r.kind).uv(r.index).sv(r.addend || 0);
    const tls = m.tls || { size: 0, align: 1, init: [] };
    w.uv(tls.size).uv(tls.align).uv(tls.init.length).raw(tls.init);
    w.uv((tls.relocs || []).length);
    for (const r of tls.relocs || []) w.uv(r.offset).u8(r.kind).uv(r.index).sv(r.addend || 0);
    w.uv(m.funcs.length);
    for (const f of m.funcs) w.str(f.name).uv(f.sig).u8(f.exported ? 1 : 0);
    for (const f of m.funcs) {
        w.uv((f.locals || []).length); (f.locals || []).forEach(t => w.u8(t));
        w.uv((f.slots || []).length); (f.slots || []).forEach(s => w.uv(s.size).uv(s.align));
        w.uv(f.blocks.length);
        for (const block of f.blocks) {
            w.uv(block.length);
            for (const inst of block) {
                w.u8(OP[inst[0]]);
                for (const operand of inst.slice(1)) {
                    if (typeof operand === "number") w.uv(operand);
                    else if ("s" in operand) w.sv(operand.s);
                    else if ("f64" in operand) w.f64(operand.f64);
                    else if ("u8" in operand) w.u8(operand.u8);
                    else if ("bytes" in operand) w.raw(operand.bytes);
                    else throw new Error("bad operand");
                }
            }
        }
    }
    const exports = m.exports || [];
    w.uv(exports.length);
    for (const e of exports) { w.str(e.name).uv(e.func).u8(e.ret).uv(e.args.length); e.args.forEach(a => w.u8(a)); }
    const libraries = m.libraries || [];
    w.uv(libraries.length);
    for (const name of libraries) w.str(name);
    for (const list of [m.constructors || [], m.destructors || []]) { w.uv(list.length); list.forEach(f => w.uv(f)); }
    return new Uint8Array(w.b);
}
