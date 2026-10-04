//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=1", "--useAOTPropertyNameIDs=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=1", "--useAOTPropertyNameIDs=1", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=1", "--useAOTPropertyNameIDs=0", "--useAOTInlineGuessedPlacesEverywhere=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=1", "--useAOTPropertyNameIDs=0", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1")

function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
function join(a, b) { return a + b; }

function makePoint(v) { return { x: v, y: v + 1, z: v + 2 }; }
function makePair(v) { return { first: v, second: v + 1 }; }
function makeBackwards(v) { return { z: v + 2, y: v + 1, x: v }; }
function readX(o) { return o.x; }
function readZ(o) { return o.z; }
function sum(o) { return o.x + o.y + o.z; }
function readSecond(o) { return o.second; }
function writeY(o, v) { o.y = v; }
function sumOfAll(all) {
    let total = 0;
    for (let i = 0; i < all.length; ++i)
        total += all[i].x + all[i].z;
    return total;
}

const isCounting = typeof aotOperationCount === "function" && !!jscOptions().useAOTOperationCounters && isAOTCompiled(makePoint);
function lookUps() { return isCounting ? (aotOperationCount("propertyNameIDIfKnown:known") || 0) + (aotOperationCount("propertyNameIDIfKnown:unknown") || 0) : 0; }

const points = [];
for (let i = 0; i < 200; ++i) {
    const point = makePoint(i);
    check(readX(point), i, "x");
    check(readZ(point), i + 2, "z");
    check(sum(point), 3 * i + 3, "the sum");
    writeY(point, -i);
    check(sum(point), i + 2, "the sum after a store");
    check(readSecond(point), undefined, "a name of another literal");
    check(readSecond(makePair(i)), i + 1, "second");
    check(readX(makePair(i)), undefined, "x of a pair");
    const backwards = makeBackwards(i);
    check(readX(backwards), i, "x in another slot");
    check(readZ(backwards), i + 2, "z in another slot");
    writeY(backwards, 7);
    check(sum(backwards), 2 * i + 9, "the sum in another order");
    points.push(point, backwards);
}
check(sumOfAll(points), 2 * (2 * (199 * 200 / 2) + 2 * 200), "a loop over both orders");

const parsed = JSON.parse('{"x":1,"y":2,"z":3}');
check(sum(parsed), 6, "parsed");
check(readX(JSON.parse('{"z":1,"x":2}')), 2, "parsed, in another order");
for (let i = 0; i < 100; ++i) {
    const name = join("made", i);
    const made = { [name]: i, x: -i };
    check(made[name], i, name);
    check(readX(made), -i, "x behind " + name);
    check(readZ(made), undefined, "z of an object with " + name);
}
check(readX({ get x() { return 5; } }), 5, "a getter");
check(readX(Object.create(makePoint(9))), 9, "inherited");
check(readX(Object.freeze(makePoint(4))), 4, "frozen");
const frozen = Object.freeze(makePoint(4));
writeY(frozen, 1);
check(frozen.y, 5, "a store to a frozen object");

if (isCounting) {
    if (jscOptions().useAOTPropertyNameIDs) {
        if (!lookUps())
            throw new Error("with IDs the names added to Structures are looked up");
    } else
        check(lookUps(), 0, "without IDs no name is looked up");
}
if (typeof aotRemarks === "function" && isAOTCompiled(readX) && !jscOptions().useAOTPropertyNameIDs) {
    for (const name of ["readX", "readZ", "sum", "readSecond", "writeY", "sumOfAll"]) {
        for (const remark of aotRemarks(name) || []) {
            for (const form of ["guessed-place-read-through-stub", "guessed-place-store-through-stub", "guessed-place-read:", "guessed-place-store:"])
                check(remark.startsWith(form), false, remark + " in " + name + ": a check of a name's number, which no name has");
        }
    }
}
