//@ runDefault
//@ runDefault("--useConcurrentGC=false")

function emptyBetweenCollections(name, table, add) {
    const keys = [];
    for (let i = 0; i < 20000; i++) {
        const key = {};
        keys.push(key);
        add(key);
    }
    const afterFull = fullGC();
    for (const key of keys)
        table.delete(key);
    const afterEden = edenGC();
    if (afterEden < afterFull)
        throw new Error(name + ": " + afterEden + " bytes after an eden collection, " + afterFull + " after the full collection before it");
}

const map = new WeakMap;
emptyBetweenCollections("WeakMap", map, key => map.set(key, 0));
const set = new WeakSet;
emptyBetweenCollections("WeakSet", set, key => set.add(key));
