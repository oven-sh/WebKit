// A dictionary structure belongs to one object. A transition away from it (a delete, an attribute change,
// seal, freeze, preventExtensions, a new prototype, a private brand) used to copy the dictionary's pinned
// property table, so a delete on an object with n properties cost O(n) and allocated a table of n entries.
// The transition now takes the table. This checks the allocation, and that every kind of transition from a
// dictionary, with collections in between, still sees the right properties.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: ${actual}, expected ${expected}`);
}

function makeObject(count) {
    const object = {};
    for (let i = 0; i < count; ++i)
        object[`key${i}`] = i;
    return object;
}

// $vm.heapTotalBytesAllocated() is what the heap has counted as allocated so far, cells and their storage alike. It
// counts a block (16 KB or more) when the block is taken, so the average over many iterations is the measure. The
// least of several rounds leaves out the rounds in which the compilers allocate for the loop itself.
const rounds = 5;
const iterationsPerRound = 400;
const iterations = rounds * iterationsPerRound;
function bytesAllocatedPerIteration(object, callback) {
    let least = Infinity;
    for (let round = 0; round < rounds; ++round) {
        const before = $vm.heapTotalBytesAllocated();
        for (let i = 0; i < iterationsPerRound; ++i)
            callback(object, round * iterationsPerRound + i);
        least = Math.min(least, ($vm.heapTotalBytesAllocated() - before) / iterationsPerRound);
    }
    return least;
}

// A table of 1000 entries is over 20 KB. A Structure cell is under 256 bytes.
const perIterationBound = 1024;

{
    const object = makeObject(1000);
    const bytes = bytesAllocatedPerIteration(object, (object, i) => {
        delete object.key0;
        object.key0 = i;
    });
    shouldBe(bytes < perIterationBound, true, `delete and re-add on a 1000 property object allocated ${bytes} bytes per iteration`);
    shouldBe(object.key0, iterations - 1, "the re-added value");
    shouldBe(Object.keys(object).length, 1000, "the property count after deleting and re-adding");
    shouldBe(Object.keys(object)[999], "key0", "the re-added property is last in enumeration order");
}

// An add to a dictionary pins its table in place, and the attribute change after it used to copy the table.
{
    const object = makeObject(1000);
    const bytes = bytesAllocatedPerIteration(object, (object, i) => {
        object[`added${i}`] = i;
        Object.defineProperty(object, "key5", { value: i, writable: i % 2 === 0, enumerable: true, configurable: true });
    });
    // The added properties grow the table now and then: a few hundred bytes per iteration on average.
    shouldBe(bytes < 4 * perIterationBound, true, `an add and an attribute change on a 1000 property object allocated ${bytes} bytes per iteration`);
    shouldBe(object.key5, iterations - 1, "the redefined value");
    shouldBe(Object.getOwnPropertyDescriptor(object, "key5").writable, false, "the redefined attribute");
    shouldBe(Object.keys(object).length, 1000 + iterations, "the property count after the adds");
}

// The transitions that pin a copy of the table take a dictionary's table as well: a prototype change, and the
// transition to another dictionary kind.
{
    const object = makeObject(1000);
    const prototypes = [{ a: 1 }, { b: 2 }];
    const bytes = bytesAllocatedPerIteration(object, (object, i) => {
        Object.setPrototypeOf(object, prototypes[i & 1]);
    });
    shouldBe(bytes < perIterationBound, true, `a prototype change on a 1000 property object allocated ${bytes} bytes per iteration`);
    shouldBe(object.b, 2, "the last prototype");
    shouldBe(Object.keys(object).length, 1000, "the property count after the prototype changes");
}
{
    const object = makeObject(1000);
    const bytes = bytesAllocatedPerIteration(object, (object, i) => {
        $vm.toCacheableDictionary(object);
    });
    shouldBe(bytes < perIterationBound, true, `a dictionary transition on a 1000 property object allocated ${bytes} bytes per iteration`);
    shouldBe(object.key999, 999, "an own read after the dictionary transitions");
}

// Each kind of transition once, on a fresh object each time: the bytes of the transition alone, averaged over
// the objects, are the difference from making the objects with no transition.
{
    // 32 objects: a block (64 KB on Linux arm64) counted at the wrong moment is 2 KB per object.
    const objectCount = 32;
    const keptAlive = [];
    function bytesPerObject(transition) {
        gc();
        const before = $vm.heapTotalBytesAllocated();
        for (let i = 0; i < objectCount; ++i) {
            const object = makeObject(1000);
            transition(object);
            keptAlive.push(object);
        }
        return ($vm.heapTotalBytesAllocated() - before) / objectCount;
    }
    class Base {
        constructor(object) { return object; }
    }
    class Branded extends Base {
        #method() { return 1; }
        static has(object) { return #method in object; }
    }
    const transitions = {
        "none": () => { },
        "delete": object => { delete object.key0; },
        "defineProperty": object => { Object.defineProperty(object, "key0", { enumerable: false }); },
        "seal": object => { Object.seal(object); },
        "freeze": object => { Object.freeze(object); },
        "preventExtensions": object => { Object.preventExtensions(object); },
        "setPrototypeOf": object => { Object.setPrototypeOf(object, { a: 1 }); },
        "private brand": object => { new Branded(object); shouldBe(Branded.has(object), true, "the brand"); },
        "toUncacheableDictionary": object => { $vm.toUncacheableDictionary(object); },
    };
    // The objects are made in the measured window, so this average is coarser than the loops above: a copied
    // table is over 20 KB per object.
    const oneShotBound = 12 * perIterationBound;
    const none = bytesPerObject(transitions.none);
    for (const [name, transition] of Object.entries(transitions)) {
        if (name === "none")
            continue;
        const bytes = bytesPerObject(transition) - none;
        shouldBe(bytes < oneShotBound, true, `${name} on a 1000 property object allocated ${bytes} bytes`);
    }
}

// The transitions that pin their table cut the chain to the dictionary, as they did before the table moved:
// the structure transition list ends at the transition. A delete or an attribute change keeps its place in the
// chain, as before.
{
    function chainLength(object) {
        const list = $vm.getStructureTransitionList(object);
        return list ? list.length / 5 : 0;
    }
    for (const [name, transition] of Object.entries({
        "seal": object => Object.seal(object),
        "freeze": object => Object.freeze(object),
        "preventExtensions": object => Object.preventExtensions(object),
        "setPrototypeOf": object => Object.setPrototypeOf(object, { a: 1 }),
        "toUncacheableDictionary": object => $vm.toUncacheableDictionary(object),
    })) {
        // A dictionary for sure: once makeObject is compiled, its puts have the by-id threshold of 512 properties.
        const object = makeObject(300);
        $vm.toCacheableDictionary(object);
        // Attribute changes, not deletes: the inline cache of a delete flattens a dictionary it sees, and then
        // there is no dictionary to cut the chain of.
        for (let i = 0; i < 50; ++i)
            Object.defineProperty(object, `key${i}`, { enumerable: false });
        shouldBe(chainLength(object), 51, `the chain after 50 attribute changes, before ${name}`);
        transition(object);
        shouldBe(chainLength(object), 1, `the chain after ${name}`);
    }
}

// Every transition from a dictionary, with a collection after it: the structure the object left has no table
// any more, and the one it moved to must not need the old one to rebuild its table.
{
    const object = makeObject(300);
    $vm.toCacheableDictionary(object);
    delete object.key1;
    gc();
    shouldBe(object.key2, 2, "a read after a delete and a collection");
    shouldBe("key1" in object, false, "the deleted property after a collection");

    Object.defineProperty(object, "key2", { enumerable: false });
    gc();
    shouldBe(object.key2, 2, "a read after an attribute change and a collection");
    shouldBe(Object.keys(object).includes("key2"), false, "the non-enumerable property after a collection");

    Object.setPrototypeOf(object, { inherited: 7 });
    gc();
    shouldBe(object.inherited, 7, "a prototype read after a prototype change and a collection");
    shouldBe(object.key3, 3, "an own read after a prototype change and a collection");

    Object.preventExtensions(object);
    gc();
    object.added = 1;
    shouldBe(object.added, undefined, "an add on a non-extensible object");
    delete object.key3;
    shouldBe(object.key3, undefined, "a delete on a non-extensible object");

    Object.seal(object);
    gc();
    delete object.key4;
    shouldBe(object.key4, 4, "a delete on a sealed object");

    Object.freeze(object);
    gc();
    object.key4 = 0;
    shouldBe(object.key4, 4, "a write on a frozen object");
    shouldBe(Object.keys(object).length, 297, "the property count at the end");
}

{
    class Branded {
        #method() { return 11; }
        static call(object) { return object.#method(); }
        constructor() {
            for (let i = 0; i < 300; ++i)
                this[`key${i}`] = i;
            delete this.key0;
        }
    }
    const object = new Branded();
    gc();
    shouldBe(Branded.call(object), 11, "a private method on an object branded from a dictionary");
    shouldBe(object.key1, 1, "an own read on a branded object");
}

// A cacheable dictionary that becomes an uncacheable one takes its table along too.
{
    const object = makeObject(300);
    $vm.toCacheableDictionary(object);
    delete object.key0;
    $vm.toUncacheableDictionary(object);
    gc();
    for (let i = 0; i < 10; ++i) {
        delete object.key1;
        object.key1 = i;
    }
    shouldBe(object.key1, 9, "a re-added value on an uncacheable dictionary");
    shouldBe(object.key2, 2, "an own read on an uncacheable dictionary");
    shouldBe(Object.keys(object).length, 299, "the property count on an uncacheable dictionary");
}

// Functions whose inline caches saw the dictionary before the delete, and that the optimizing compilers compile
// after it, while the caches still list the departed structure: a read, a write, `in`, a getter and a setter.
{
    const object = makeObject(2000);
    let stored = 0;
    Object.defineProperty(object, "accessor", { get() { return this.key30; }, set(value) { stored = value; }, configurable: true });
    function read(object) { return object.key10 + object.key20 + object.key1999; }
    function write(object, value) { object.key40 = value; }
    function has(object) { return "key50" in object; }
    function getter(object) { return object.accessor; }
    function setter(object, value) { object.accessor = value; }
    noInline(read);
    noInline(write);
    noInline(has);
    noInline(getter);
    noInline(setter);
    for (let i = 0; i < 300; ++i) {
        read(object);
        write(object, i);
        has(object);
        getter(object);
        setter(object, i);
    }
    delete object.key0;
    let sum = 0;
    const count = Math.max(testLoopCount, 2000);
    for (let i = 0; i < count; ++i) {
        sum += read(object);
        write(object, i);
        sum += object.key40;
        sum += has(object) ? 1 : 0;
        sum += getter(object);
        setter(object, i);
        sum += stored;
        if (i % 1000 === 0) {
            delete object.key10;
            object.key10 = 10;
            gc();
        }
    }
    shouldBe(sum, count * (10 + 20 + 1999 + 1 + 30) + 2 * ((count - 1) * count / 2), "accesses through inline caches across deletes");
}
