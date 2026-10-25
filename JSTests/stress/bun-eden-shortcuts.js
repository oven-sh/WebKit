//@ runDefault("--verifyGC=true")
//@ runDefault("--verifyGC=true", "--useConcurrentGC=false")
//@ runNoJIT("--verifyGC=true", "--useConcurrentGC=false")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + ", expected " + expected);
}

function text(n) { return "" + n; }
noInline(text);

function viaToString(n) { return n.toString(); }
noInline(viaToString);

function digits(n) {
    let result = "";
    do {
        const digit = n % 10;
        result = String.fromCharCode(48 + digit) + result;
        n = (n - digit) / 10;
    } while (n > 0);
    return result;
}

function expectedText(n) {
    const whole = Math.floor(n);
    return n === whole ? digits(n) : digits(whole) + ".5";
}

function range(first, last, fraction = 0) {
    const numbers = [];
    for (let i = first; i <= last; ++i)
        numbers.push(i + fraction);
    return numbers;
}

function numberRanges() {
    return [range(10, 1023), range(1024, 5000), range(0, 3000, 0.5)];
}

function convertAll(numbers) {
    for (const n of numbers) {
        text(n);
        viaToString(n);
    }
}

function checkNewestFirst(numbers) {
    for (let i = numbers.length - 1; i >= 0; --i) {
        const expected = expectedText(numbers[i]);
        shouldBe(text(numbers[i]), expected);
        shouldBe(viaToString(numbers[i]), expected);
    }
}

function fillCaches() {
    fullGC();
    for (const numbers of numberRanges()) {
        convertAll(numbers);
        edenGC();
    }
}

function checkCaches() {
    for (const numbers of numberRanges())
        checkNewestFirst(numbers);
}

function fillThenEden() {
    fillCaches();
    checkCaches();
}

function nothingWritten() {
    fillCaches();
    edenGC();
    edenGC();
    edenGC();
    checkCaches();
}

function recordLimitExceeded() {
    const integers = range(100000, 109999);
    const doubles = range(100000, 104999, 0.5);
    fullGC();
    convertAll(integers);
    convertAll(doubles);
    edenGC();
    checkNewestFirst(integers.slice(-500));
    checkNewestFirst(doubles.slice(-500));
}

function overwrittenSlot() {
    fullGC();
    convertAll(range(1024, 3000));
    convertAll(range(3001, 6000));
    edenGC();
    checkNewestFirst(range(5000, 6000));
}

function acrossFullCollection() {
    fillCaches();
    fullGC();
    edenGC();
    checkCaches();
}

function retainedResults() {
    const retainedNumbers = [];
    const retainedStrings = [];
    fullGC();
    for (const numbers of numberRanges()) {
        for (let i = 0; i < numbers.length; ++i) {
            text(numbers[i]);
            const string = viaToString(numbers[i]);
            if (i % 10)
                continue;
            retainedNumbers.push(numbers[i]);
            retainedStrings.push(string);
        }
        edenGC();
    }
    fullGC();
    edenGC();
    for (let i = 0; i < retainedNumbers.length; ++i)
        shouldBe(retainedStrings[i], expectedText(retainedNumbers[i]));
}

function createWeakMap() { return new WeakMap; }
noInline(createWeakMap);

function createWeakSet() { return new WeakSet; }
noInline(createWeakSet);

function store(table, key, i) {
    if (table instanceof WeakMap)
        table.set(key, { payload: i });
    else
        table.add(key);
}

function createKeys(count) {
    const keys = [];
    for (let i = 0; i < count; ++i)
        keys.push({});
    return keys;
}

function storeAll(table, keys) {
    for (let i = 0; i < keys.length; ++i)
        store(table, keys[i], i);
}

function addKeys(table, count) {
    const keys = createKeys(count);
    storeAll(table, keys);
    return keys;
}

function shouldFind(table, key, i) {
    shouldBe(table.has(key), true);
    if (table instanceof WeakMap)
        shouldBe(table.get(key).payload, i);
}

function shouldFindAll(table, keys) {
    for (let i = 0; i < keys.length; ++i)
        shouldFind(table, keys[i], i);
}

function makeOld() {
    fullGC();
}

function oldTableNothingStored(createTable) {
    const table = createTable();
    const keys = addKeys(table, 100);
    makeOld();
    edenGC();
    edenGC();
    edenGC();
    shouldFindAll(table, keys);
}

function oldMapNewValues(insert) {
    const map = createWeakMap();
    const oldKeys = addKeys(map, 100);
    makeOld();
    const newKeys = createKeys(100);
    for (let i = 0; i < newKeys.length; ++i)
        insert(map, newKeys[i], { payload: i });
    edenGC();
    shouldFindAll(map, newKeys);
    shouldFindAll(map, oldKeys);
}

function oldMapReplacedValues() {
    const map = createWeakMap();
    const keys = addKeys(map, 100);
    makeOld();
    keys.reverse();
    storeAll(map, keys);
    edenGC();
    shouldFindAll(map, keys);
}

function oldTableDyingKeys(createTable) {
    const table = createTable();
    const oldKeys = addKeys(table, 100);
    makeOld();
    for (let i = 0; i < 1000; ++i)
        store(table, {}, i);
    edenGC();
    shouldFindAll(table, oldKeys);
}

function linkMaps(first, second, key, i) {
    const link = {};
    first.set(key, link);
    second.set(link, { payload: i });
}
noInline(linkMaps);

function ephemeronChainAcrossMaps() {
    const first = createWeakMap();
    const second = createWeakMap();
    const keys = createKeys(100);
    makeOld();
    for (let i = 0; i < keys.length; ++i) {
        linkMaps(first, second, keys[i], i);
        linkMaps(second, first, keys[i], i);
    }
    edenGC();
    for (let i = 0; i < keys.length; ++i) {
        shouldBe(second.get(first.get(keys[i])).payload, i);
        shouldBe(first.get(second.get(keys[i])).payload, i);
    }
}

function oldTableDeleteOnly(createTable) {
    const table = createTable();
    const keys = addKeys(table, 100);
    makeOld();
    for (let i = 0; i < keys.length; i += 2)
        table.delete(keys[i]);
    edenGC();
    for (let i = 0; i < keys.length; ++i) {
        if (i % 2)
            shouldFind(table, keys[i], i);
        else
            shouldBe(table.has(keys[i]), false);
    }
}

function oldTableGrown(createTable) {
    const table = createTable();
    const oldKeys = addKeys(table, 100);
    makeOld();
    const newKeys = addKeys(table, 5000);
    edenGC();
    shouldFindAll(table, newKeys);
    shouldFindAll(table, oldKeys);
}

function newTables(createTable) {
    const keys = createKeys(2000);
    const tables = [];
    for (let i = 0; i < keys.length; ++i) {
        tables.push(createTable());
        store(tables[i], keys[i], i);
    }
    edenGC();
    edenGC();
    for (let i = 0; i < keys.length; ++i)
        shouldFind(tables[i], keys[i], i);
}

function runAllSections() {
    fillThenEden();
    nothingWritten();
    recordLimitExceeded();
    overwrittenSlot();
    acrossFullCollection();
    retainedResults();
    for (const createTable of [createWeakMap, createWeakSet]) {
        oldTableNothingStored(createTable);
        oldTableDyingKeys(createTable);
        oldTableDeleteOnly(createTable);
        oldTableGrown(createTable);
        newTables(createTable);
    }
    oldMapNewValues((map, key, value) => map.set(key, value));
    oldMapNewValues((map, key, value) => map.getOrInsert(key, value));
    oldMapReplacedValues();
    ephemeronChainAcrossMaps();
}

for (let round = 0; round < 3; ++round)
    runAllSections();
