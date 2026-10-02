//@ skip if $architecture != "arm64"
//@ requireOptions("--compileMainScriptAheadOfTime=1")
//@ defaultRun
//@ run("gc-stress", "--slowPathAllocsBetweenGCs=20")

function realmOfErrorFrom(f, realms) {
    try {
        f();
    } catch (e) {
        return realms.findIndex(realm => e instanceof realm.TypeError);
    }
    return -1;
}
noInline(realmOfErrorFrom);

const other = createGlobalObject();
const third = createGlobalObject();
const realms = [globalThis, other, third];
const actual = [];
for (let round = 0; round < 50; round++) {
    actual.length = 0;
    for (const realm of realms) {
        const { map, filter } = realm.Array.prototype;
        actual.push(
            isAOTCompiled(map), isAOTCompiled(filter),
            realmOfErrorFrom(() => map.call([1], null), realms),
            realmOfErrorFrom(() => filter.call([1], 1), realms),
            map.call([1, 2], x => x * 2).join(),
            Object.getPrototypeOf(realm.Array.from([1])) === realm.Array.prototype);
    }
}
const expected = [0, 1, 2].map(i => `true,true,${i},${i},2,4,true`).join();
if (actual.join() !== expected)
    throw new Error(`got ${actual.join()}`);
