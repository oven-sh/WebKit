//@ runDefault("--verifyGC=true")
//@ runDefault("--verifyGC=true", "--useConcurrentGC=false")
//@ runNoJIT("--verifyGC=true", "--useConcurrentGC=false")

// The heap counts the buffer of a WeakMap or WeakSet as it is behind a collection. But an eden collection only collects what
// was allocated since the last collection, and the heap expects its size not to go below what that one left
// (Heap::updateAllocationLimits() asserts it, and subtracts one size from the other). So the buffer of an old table that
// has shrunk since stops counting at the next full collection, and everything else counts at once.

const count = 4096;
// 4096 keys need 16384 buckets, of a key (WeakSet) or of a key and a value (WeakMap). Half of the smaller one
// is well above what else changes between two collections here.
const margin = 16384 * 8 / 2;

let sink;

// fullGC() answers with the heap's size after the last full collection, edenGC() with the one after the last eden collection.
// Where every collection is a full one (without the JIT, or without the generational collector) there was none: 0.
function collectEden()
{
    sink = { }; // A collection of an empty eden does nothing at all.
    return edenGC();
}

function makeKeys()
{
    const keys = [];
    for (let i = 0; i < count; ++i)
        keys.push({ });
    return keys;
}

function shouldNotBeBelow(size, last, what)
{
    if (size < last)
        throw new Error(`${what}: the eden collection left ${size} bytes, below the ${last} of the collection before it`);
}

function testOldTableShrunkByDelete(table, add)
{
    const keys = makeKeys();
    for (let i = 0; i < count; ++i)
        add(table, keys[i]);

    const afterFull = fullGC();
    for (let i = 0; i < count; ++i)
        table.delete(keys[i]);

    let last = afterFull;
    for (let i = 0; i < 2; ++i) {
        const afterEden = collectEden();
        if (!afterEden)
            break;
        shouldNotBeBelow(afterEden, last, "shrunk by delete");
        last = afterEden;
    }

    const afterSecondFull = fullGC();
    if (afterSecondFull > afterFull - margin)
        throw new Error(`the buffer that went still counts behind a full collection: ${afterFull} bytes, then ${afterSecondFull}`);
}

// Shrinks, grows past where it was and shrinks again, with an eden collection behind each.
function testOldTableShrinksAndGrows(table, add)
{
    const keys = makeKeys();
    const more = makeKeys();
    for (let i = 0; i < count; ++i)
        add(table, keys[i]);

    let last = fullGC();
    function step(what, mayGrow)
    {
        const afterEden = collectEden();
        if (!afterEden)
            return;
        shouldNotBeBelow(afterEden, last, what);
        if (!mayGrow && afterEden > last + margin)
            throw new Error(`${what}: a buffer no larger than the one that counts already counts again: ${last} bytes, then ${afterEden}`);
        last = afterEden;
    }

    for (let i = 0; i < count; ++i)
        table.delete(keys[i]);
    step("shrunk", false);
    for (let i = 0; i < count / 2; ++i)
        add(table, keys[i]);
    step("half way back", false);
    for (let i = count / 2; i < count; ++i)
        add(table, keys[i]);
    step("all the way back", false);
    for (let i = 0; i < more.length; ++i)
        add(table, more[i]);
    step("larger than before", true);
    for (let i = 0; i < more.length; ++i)
        table.delete(more[i]);
    step("back", false);
}

// What an old table has grown by counts from the next eden collection on.
function testOldTableGrows(table, add)
{
    const keys = makeKeys();
    add(table, keys[0]);
    fullGC();
    const before = collectEden();
    for (let i = 0; i < count; ++i)
        add(table, keys[i]);
    const after = collectEden();
    if (before && after < before + margin)
        throw new Error(`the buffer of an old table that grew does not count: ${before} bytes, then ${after}`);
}

// A table that the eden collection is the first to visit. Its keys are old, so that its buffer is all that is new.
function testYoungTableKeepsItsKeys(make, add)
{
    const keys = makeKeys();
    fullGC();
    const before = collectEden();
    const table = make();
    for (let i = 0; i < count; ++i)
        add(table, keys[i]);
    const after = collectEden();
    if (before && after < before + margin)
        throw new Error(`the buffer of a young table does not count: ${before} bytes, then ${after}`);
    sink = table;
}

// One that loses its keys to that collection: the buffer that had room for them never counts.
function testYoungTableLosesItsKeys(make, add)
{
    fullGC();
    let last = collectEden();
    const tables = [];
    for (let round = 0; round < 2; ++round) {
        const table = make();
        tables.push(table);
        for (let i = 0; i < count; ++i)
            add(table, { });
        const afterEden = collectEden();
        if (!afterEden)
            return;
        shouldNotBeBelow(afterEden, last, "young table");
        if (afterEden > last + margin)
            throw new Error(`the buffer that a young table had before its keys went counts: ${last} bytes, then ${afterEden}`);
        last = afterEden;
    }
}

// Optimized code allocates the table itself, and leaves what the heap counts for it uninitialized.
function testTablesFromOptimizedCode(make, add)
{
    function makeMany(tables)
    {
        for (let i = 0; i < tables.length; ++i)
            tables[i] = make();
    }
    noInline(makeMany);

    const keys = makeKeys();
    const tables = new Array(16);
    for (let i = 0; i < testLoopCount / tables.length; ++i)
        makeMany(tables);

    fullGC();
    let last = collectEden();
    makeMany(tables);
    for (let i = 0; i < tables.length; i += 2)
        add(tables[i], keys[i]);
    for (let i = 0; i < 2; ++i) {
        const afterEden = collectEden();
        if (!afterEden)
            return;
        shouldNotBeBelow(afterEden, last, "tables from optimized code");
        if (afterEden > last + margin)
            throw new Error(`tables with a key or none count for more than they are: ${last} bytes, then ${afterEden}`);
        last = afterEden;
    }
}

const kinds = [
    [() => new WeakMap, (table, key) => { table.set(key, 0); }],
    [() => new WeakSet, (table, key) => { table.add(key); }],
];

for (const [make, add] of kinds) {
    testOldTableShrunkByDelete(make(), add);
    testOldTableShrinksAndGrows(make(), add);
    testOldTableGrows(make(), add);
    testYoungTableKeepsItsKeys(make, add);
    testYoungTableLosesItsKeys(make, add);
    testTablesFromOptimizedCode(make, add);
}
