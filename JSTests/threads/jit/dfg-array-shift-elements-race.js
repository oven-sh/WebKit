//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=50")
//@ runDefault()
//@ runDefault("--useFTLJIT=0")
// The optimized a.shift() checks 2 <= length in JIT code, then calls an
// operation that moves the elements in place. That move is only safe on a flat
// butterfly that this thread owns and that no other thread has written. For
// any other array, the operation must return to the generic shift, as
// JSArray::fastShift does.
//
// Part 1 is deterministic. The generic shift converts an array that another
// thread has written, or that another thread owns, to ArrayStorage, so the
// indexing mode shows which path ran.
//
// Part 2 is a race. Another thread pushes and pops the array while this thread
// shifts it. Each loop has a fixed trip count, so with the GIL on the threads
// run one after the other, and the race does not happen.

load("../harness.js", "caller relative");

function shiftInt(a) { return a.shift(); }
noInline(shiftInt);
function shiftDouble(a) { return a.shift(); }
noInline(shiftDouble);
function shiftObject(a) { return a.shift(); }
noInline(shiftObject);

// Each array has spare vector capacity, so pushes stay in the same butterfly.
//
// The arrays that Parts 1 and 2 convert to ArrayStorage each come from a maker
// of their own (kind.fresh), whose source text is unique: an array literal's
// allocation site starts making ArrayStorage arrays once an array it made has
// become one (when its profile next looks at that array, which depends on the
// tier the maker runs in at the time), and functions with the same source
// share that site through the code cache. So a converted array must not share
// its maker with an array whose starting shape is checked later.
const marker = { tag: 7 };
const makeIntSource = "const a = []; for (let i = 0; i < 32; ++i) a.push(i); a.length = 8; return a;";
const makeDoubleSource = "const a = []; for (let i = 0; i < 32; ++i) a.push(i + 0.5); a.length = 8; return a;";
const makeObjectSource = "const a = []; for (let i = 0; i < 32; ++i) a.push(marker); a.length = 8; return a;";
const makeInt = new Function("marker", makeIntSource);
noInline(makeInt);
const makeDouble = new Function("marker", makeDoubleSource);
noInline(makeDouble);
const makeObject = new Function("marker", makeObjectSource);
noInline(makeObject);
let freshMakers = 0;
function fresh(source) { return new Function("marker", source + "\n// maker " + (++freshMakers))(marker); }

const kinds = [
    { name: "Int32", shift: shiftInt, make: () => makeInt(marker), fresh: () => fresh(makeIntSource), value: 7, first: 0 },
    { name: "Double", shift: shiftDouble, make: () => makeDouble(marker), fresh: () => fresh(makeDoubleSource), value: 7.5, first: 0.5 },
    { name: "Contiguous", shift: shiftObject, make: () => makeObject(marker), fresh: () => fresh(makeObjectSource), value: marker, first: marker },
];

// The first write or push from another thread fires watchpoints on the array
// structures, which throws away code compiled before it. Do that here, once,
// before the shifts tier up.
for (const kind of kinds) {
    const a = kind.make();
    new Thread(() => { a[7] = kind.value; kind.make().push(kind.value); }).join();
}

for (const kind of kinds) {
    for (let n = 0; n < 500; ++n) {
        const a = kind.make();
        while (a.length)
            kind.shift(a);
    }
}

// Part 1.
for (const kind of kinds) {
    const written = kind.fresh();
    shouldBe($vm.indexingMode(written), "ArrayWith" + kind.name);
    new Thread(() => { written[7] = kind.value; }).join();
    shouldBe(kind.shift(written), kind.first);
    shouldBe(written.length, 7);
    shouldBe(written[6], kind.value);
    shouldBe($vm.indexingMode(written), "ArrayWithArrayStorage");

    const foreign = new Thread(() => kind.fresh()).join();
    shouldBe($vm.indexingMode(foreign), "ArrayWith" + kind.name);
    shouldBe(kind.shift(foreign), kind.first);
    shouldBe(foreign.length, 7);
    shouldBe($vm.indexingMode(foreign), "ArrayWithArrayStorage");
}

// Part 2.
const iterations = 3000;
for (const kind of kinds) {
    const shared = kind.fresh();
    const other = new Thread(() => {
        for (let n = 0; n < iterations; ++n) {
            shared.push(kind.value);
            shared.push(kind.value);
            shared.pop();
            shared.pop();
        }
    });

    for (let n = 0; n < iterations; ++n) {
        if (shared.length < 2)
            shared.push(kind.value, kind.value);
        const v = kind.shift(shared);
        if (v !== undefined && v !== kind.value && typeof v !== typeof kind.value)
            throw new Error("unexpected shifted value " + v + " at " + n);
        if (shared.length > 64)
            throw new Error("length grew to " + shared.length);
    }
    other.join();
}
