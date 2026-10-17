// Stores, adds, deletes and indexed stores that were hot and cached before the object's properties were made immutable: no tier
// keeps a fast path into it. The object moves to a new structure, and no store is ever cached for such a structure.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function setX(object, value) { object.x = value; }
function setXStrict(object, value) { "use strict"; object.x = value; }
function setByKey(object, key, value) { object[key] = value; }
function setByIndex(object, index, value) { object[index] = value; }
function addFresh(object, value) { object.fresh = value; }
function deleteX(object) { delete object.x; }
function forInStore(object) { for (let key in object) object[key] = "X"; }
function defineByLiteralSpread(object) { return { ...object, x: "copy" }; }
for (let f of [setX, setXStrict, setByKey, setByIndex, addFresh, deleteX, forInStore, defineByLiteralSpread])
    noInline(f);

function manyShapes() {
    let result = [];
    for (let i = 0; i < 40; i++) {
        let object = { x: 0 };
        object["p" + i] = i;
        result.push(object);
    }
    return result;
}

function dictionary() {
    let object = { x: 0 };
    for (let i = 0; i < 300; i++)
        object["k" + i] = i;
    for (let i = 0; i < 150; i++)
        delete object["k" + i];
    return object;
}

let targets = {
    plain: { x: 0, keep: 1 },
    dictionary: dictionary(),
    usedAsPrototype: (() => { let o = { x: 0 }; Object.create(o); return o; })(),
    func: Object.assign(function () { }, { x: 0 }),
    array: Object.assign([1, 2, 3], { x: 0 }),
    withIndexed: { x: 0, 0: "a", 1: "b" },
};

function warm(includeTargets) {
    let shapes = manyShapes();
    for (let round = 0; round < Math.ceil(testLoopCount / 50); round++) {
        for (let object of shapes) {
            setX(object, round);
            setXStrict(object, round);
            setByKey(object, "x", round);
        }
    }
    for (let round = 0; round < Math.ceil(testLoopCount / 8); round++) {
        let scratch = { x: 0, 0: 0, 1: 1 };
        setByIndex(scratch, 1, round);
        addFresh({ x: 0 }, round);
        deleteX({ x: 0, y: 1 });
        forInStore({ x: 0, y: 1 });
        defineByLiteralSpread({ x: 0, y: 1 });
        setByIndex([1, 2, 3], 1, round);
    }
    if (!includeTargets)
        return;
    for (let target of Object.values(targets)) {
        for (let i = 0; i < Math.ceil(testLoopCount / 8); i++) {
            setX(target, i);
            setXStrict(target, i);
            setByKey(target, "x", i);
            setByIndex(target, 1, i);
        }
    }
}

warm(true); // every site is hot and has stored into each target through whatever fast path its tier offers
for (let target of Object.values(targets))
    $vm.makePropertiesImmutable(target);
warm(false);

for (let [name, target] of Object.entries(targets)) {
    let x = target.x;
    let one = target[1];
    let keys = Reflect.ownKeys(target).length;
    for (let i = 0; i < Math.ceil(testLoopCount / 4); i++) {
        setX(target, "X" + i);
        try { setXStrict(target, "X" + i); } catch (e) { if (!(e instanceof TypeError)) throw e; }
        setByKey(target, "x", "X" + i);
        setByIndex(target, 1, "X" + i);
        setByIndex(target, 7, "X" + i);
        addFresh(target, i);
        deleteX(target);
        forInStore(target);
    }
    shouldBe(target.x, x, name + ": x");
    shouldBe(target[1], one, name + ": [1]");
    shouldBe(Reflect.ownKeys(target).length, keys, name + ": own keys");
    shouldBe("fresh" in target, false, name + ": no property was added");
    let copy = defineByLiteralSpread(target);
    shouldBe($vm.hasImmutableProperties(copy), false, name + ": a spread copy is not immutable");
    shouldBe(copy.x, "copy", name);
}

// The same stores keep working for ordinary objects of the same original shapes.
{
    let object = { x: 0, keep: 1 };
    for (let i = 0; i < Math.ceil(testLoopCount / 4); i++)
        setX(object, i);
    shouldBe(object.x, Math.ceil(testLoopCount / 4) - 1);
}
