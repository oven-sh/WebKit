load("./asm.js", "caller relative");
function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const t8 = n => ({ u8: n });
const m = $vm.cModule(assemble({
    sigs: [{ ret: T.i64, params: [] }, { ret: T.i32, params: [] }, { ret: T.i64, params: [T.i64] }],
    externs: [{ name: "bun_bir_test_symbol_nobody_defines", kind: 0x80, sig: 0 }, { name: "strlen", kind: 0, sig: 2 }],
    data: { size: 8, align: 8, init: [7, 0, 0, 0], relocs: [] },
    // _Thread_local int *p = &global (data+0);  _Thread_local int v = 5;  _Thread_local int *q = &v;
    tls: { size: 24, align: 8, init: [0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0], relocs: [{ offset: 0, kind: 0, index: 0 }, { offset: 16, kind: 3, index: 8 }] },
    funcs: [
        { name: "through_p", sig: 1, exported: true, blocks: [[["TlsAddr", 0], ["Load", t8(MEM.i64), 0, s(0)], ["Load", t8(MEM.i32), 1, s(0)], ["Ret", 2]]] },
        { name: "through_q", sig: 1, exported: true, blocks: [[["TlsAddr", 16], ["Load", t8(MEM.i64), 0, s(0)], ["Load", t8(MEM.i32), 1, s(0)], ["Ret", 2]]] },
        { name: "q_points_into_this_copy", sig: 1, exported: true, blocks: [[["TlsAddr", 16], ["Load", t8(MEM.i64), 0, s(0)], ["TlsAddr", 8], ["Eq", 1, 2], ["Ret", 3]]] },
        { name: "weak_address", sig: 0, exported: true, blocks: [[["ExternAddr", 0], ["Ret", 0]]] },
        // frame[0] is the caller's frame pointer: non-zero and above this frame
        { name: "frame_links_up", sig: 1, exported: true, blocks: [[["FrameAddress"], ["Load", t8(MEM.i64), 0, s(0)], ["UGt", 1, 0], ["Ret", 2]]] },
        { name: "return_address", sig: 0, exported: true, blocks: [[["FrameAddress"], ["Load", t8(MEM.i64), 0, s(8)], ["Ret", 1]]] },
    ],
    exports: [
        { name: "through_p", func: 0, ret: FFI.i32, args: [] }, { name: "through_q", func: 1, ret: FFI.i32, args: [] },
        { name: "q_points_into_this_copy", func: 2, ret: FFI.i32, args: [] }, { name: "weak_address", func: 3, ret: FFI.ptr, args: [] },
        { name: "frame_links_up", func: 4, ret: FFI.i32, args: [] }, { name: "return_address", func: 5, ret: FFI.ptr, args: [] },
    ],
}));
eq(m.through_p(), 7, "thread-local pointer initialized with the address of a global");
eq(m.through_q(), 5, "thread-local pointer initialized with the address of another thread-local");
eq(m.q_points_into_this_copy(), 1, "and it points into this thread's copy");
eq(m.weak_address(), null, "an undefined weak symbol is null");
for (let i = 0; i < 1e5; i++) { eq(m.frame_links_up(), 1, "frame pointer chain"); if (!m.return_address()) throw new Error("no return address"); }
print("bir6 ok");
