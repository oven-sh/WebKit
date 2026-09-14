//@ skip if !$isFTLPlatform
//@ requireOptions("--useDollarVM=1")

// Two threads hand a counter back and forth through two cells of shared memory, each spinning on a load of
// the other's cell until it sees the round's number. The spinning loop has no store and no call in it, so a
// load the compiler treats as an ordinary one is read once, before the loop, and the loop never ends: every
// atomic load, the relaxed one too, and every volatile load has to be done each time round. (The spin gives up
// after `bound` tries so that a failure is an answer and not a hang.)
load("./resources/bir-assembler.js", "caller relative");

function eq(a, b, what) { if (a !== b) throw new Error(`${what}: expected ${b}, got ${a}`); }
const s = n => ({ s: n });
const b = n => ({ u8: n });
const ORDER = { relaxed: 0, acquire: 1, release: 2, acquireRelease: 3, sequentiallyConsistent: 4 };

// long exchange(char* cells, long mine, long theirs, int rounds, int first, long bound)
// {
//     for (int r = 1; r <= rounds; r++) {
//         if (first) store(cells + mine, r);
//         for (long n = 0; load(cells + theirs) != r;) if (++n == bound) return -r;
//         if (!first) store(cells + mine, r);
//     }
//     return rounds;
// }
function exchangeModule(emitLoad, emitStore, isWide) {
    const blocks = [];
    let next = 6;
    const block = () => { const result = new Block(next); blocks.push(result); return result; };
    const close = current => { next = current.next; };
    const cells = 0, mine = 1, theirs = 2, rounds = 3, first = 4, bound = 5;
    const R = 0, N = 1;
    const ENTRY = 0, HEAD = 1, BEFORE = 2, STORE_FIRST = 3, RESET = 4, SPIN = 5, COUNT = 6, FAIL = 7, AFTER = 8, STORE_SECOND = 9, NEXT = 10, DONE = 11;
    const round = current => { const r = current.def("LocalGet", R); return isWide ? current.def("SExt32", r) : r; };
    let current;

    current = block(); // ENTRY
    current.run("LocalSet", R, current.def("ConstI32", s(1))); current.run("Jump", HEAD); close(current);
    current = block(); // HEAD
    current.run("Br", current.def("Gt", current.def("LocalGet", R), rounds), DONE, BEFORE); close(current);
    current = block(); // BEFORE
    current.run("Br", first, STORE_FIRST, RESET); close(current);
    current = block(); // STORE_FIRST
    emitStore(current, round(current), current.def("Add", cells, mine)); current.run("Jump", RESET); close(current);
    current = block(); // RESET
    current.run("LocalSet", N, current.def("ConstI64", s(0))); current.run("Jump", SPIN); close(current);
    current = block(); // SPIN
    current.run("Br", current.def("Eq", emitLoad(current, current.def("Add", cells, theirs)), round(current)), AFTER, COUNT); close(current);
    current = block(); // COUNT
    { const n = current.def("Add", current.def("LocalGet", N), current.def("ConstI64", s(1))); current.run("LocalSet", N, n); current.run("Br", current.def("Eq", n, bound), FAIL, SPIN); } close(current);
    current = block(); // FAIL
    current.run("Ret", current.def("SExt32", current.def("Neg", current.def("LocalGet", R)))); close(current);
    current = block(); // AFTER
    current.run("Br", first, NEXT, STORE_SECOND); close(current);
    current = block(); // STORE_SECOND
    emitStore(current, round(current), current.def("Add", cells, mine)); current.run("Jump", NEXT); close(current);
    current = block(); // NEXT
    current.run("LocalSet", R, current.def("Add", current.def("LocalGet", R), current.def("ConstI32", s(1)))); current.run("Jump", HEAD); close(current);
    current = block(); // DONE
    current.run("Ret", current.def("SExt32", rounds)); close(current);

    return {
        sigs: [{ ret: T.i64, params: [T.i64, T.i64, T.i64, T.i32, T.i32, T.i64] }],
        funcs: [{ name: "exchange", sig: 0, exported: true, locals: [T.i32, T.i64], blocks: blocks.map(each => each.insts) }],
        exports: [{ name: "exchange", func: 0, ret: FFI.i64, args: [FFI.ptr, FFI.i64, FFI.i64, FFI.i32, FFI.i32, FFI.i64] }],
    };
}

const variants = [];
for (const [kindName, kind] of [["8-bit", MEM.i8u], ["16-bit", MEM.i16u], ["32-bit", MEM.i32], ["64-bit", MEM.i64]]) {
    const isWide = kind === MEM.i64;
    for (const [loadOrder, storeOrder] of [["relaxed", "relaxed"], ["acquire", "release"], ["sequentiallyConsistent", "sequentiallyConsistent"], ["relaxed", "release"], ["acquire", "relaxed"]]) {
        variants.push({
            what: `${kindName} atomic, ${loadOrder} loads and ${storeOrder} stores`, isWide,
            emitLoad: (block, address) => block.def("AtomicLoad", b(kind), b(ORDER[loadOrder]), address),
            emitStore: (block, value, address) => block.run("AtomicStore", b(kind), b(ORDER[storeOrder]), value, address),
        });
    }
    variants.push({
        what: `${kindName} volatile`, isWide,
        emitLoad: (block, address) => block.def("Load", b(kind | 0x80), address, s(0)),
        emitStore: (block, value, address) => block.run("Store", b(kind | 0x80), value, address, s(0)),
    });
    // An ordinary store is fine for the writer; what matters is that the reader's loads are all done. A relaxed
    // fence and an acquire fence in the loop do not make an ordinary load any more than it is, so the loads here
    // are atomic ones too.
    variants.push({
        what: `${kindName} relaxed loads with an acquire fence after each, release fence before ordinary stores`, isWide,
        emitLoad: (block, address) => { const loaded = block.def("AtomicLoad", b(kind), b(ORDER.relaxed), address); block.run("Fence", b(ORDER.acquire)); return loaded; },
        emitStore: (block, value, address) => { block.run("Fence", b(ORDER.release)); block.run("AtomicStore", b(kind), b(ORDER.relaxed), value, address); },
    });
}

const rounds = 100, bound = 2000000000n;
for (const variant of variants) {
    const bytes = assemble(exchangeModule(variant.emitLoad, variant.emitStore, variant.isWide));
    const shared = new SharedArrayBuffer(256 + bytes.length);
    new Uint8Array(shared, 256).set(bytes);
    $.agent.start(`
        $.agent.receiveBroadcast(function(shared) {
            const exchange = $vm.cModule(new Uint8Array(shared, 256).slice()).exchange;
            $.agent.report(String(exchange(new Uint8Array(shared), 0n, 128n, ${rounds}, 1, ${bound}n)));
            $.agent.leaving();
        });
    `);
    const exchange = $vm.cModule(bytes).exchange;
    $.agent.broadcast(shared);
    eq(exchange(new Uint8Array(shared), 128n, 0n, rounds, 0, bound), BigInt(rounds), `${variant.what}: the thread that answers`);
    let report;
    while (!(report = $.agent.getReport()))
        $.agent.sleep(1);
    eq(report, String(rounds), `${variant.what}: the thread that asks`);
}
print("atomic orders ok:", variants.length, "variants");
