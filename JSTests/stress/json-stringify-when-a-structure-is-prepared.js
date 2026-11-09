// Bun: JSON.stringify's fast path prepares a Structure where another object of it is likely: for an element of a list of
// two or more at once, and for any other object when its Structure comes the second time. Upstream prepares every one.
// Every case below has few objects of a Structure, as elements and as members, in every order that the rule can tell apart.
// The reference output comes from the general Stringifier, which an identity replacer forces.

const spaces = [undefined, 2, '\t'];

function check(name, value) {
    for (const space of spaces) {
        const fast = JSON.stringify(value, null, space);
        const slow = JSON.stringify(value, (key, x) => x, space);
        if (fast !== slow) {
            let index = 0;
            while (index < fast.length && fast[index] === slow[index])
                ++index;
            throw new Error(`${name}: mismatch with space ${JSON.stringify(space)} at ${index} (lengths ${fast.length} and ${slow.length})\n fast: ${fast.slice(Math.max(0, index - 40), index + 40)}\n slow: ${slow.slice(Math.max(0, index - 40), index + 40)}`);
        }
    }
}

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`Expected ${expected} but got ${actual}`);
}

shouldBe(JSON.stringify({ a: 1, b: "x" }), '{"a":1,"b":"x"}');
shouldBe(JSON.stringify([{ a: 1 }, { a: 2 }]), '[{"a":1},{"a":2}]');
shouldBe(JSON.stringify([{ a: 1 }, { a: 2 }, { a: 3 }]), '[{"a":1},{"a":2},{"a":3}]');
shouldBe(JSON.stringify([{ a: 1 }, { a: undefined }, { a: 3 }, { a: 4 }]), '[{"a":1},{},{"a":3},{"a":4}]');
shouldBe(JSON.stringify({ a: { x: 1 }, b: { x: 2 }, c: { x: 3 }, d: { x: 4 } }), '{"a":{"x":1},"b":{"x":2},"c":{"x":3},"d":{"x":4}}');

// One to six objects of one Structure, alone and under other values.
function makePoint(i) {
    return { x: i, y: i / 4, label: i % 3 ? "p" + i : null, nested: i % 2 ? [i, "\n"] : { deep: i } };
}

for (let count = 1; count <= 6; count++) {
    const list = [];
    for (let i = 0; i < count; i++)
        list.push(makePoint(i));
    check(`${count} objects`, list);
    check(`${count} objects as members`, Object.fromEntries(list.map((point, i) => ["member" + i, point])));
    check(`${count} objects two levels down`, { first: list.slice(0, 1), rest: [list.slice(1)], again: list });
}

// The first or the second object of a Structure is the one whose properties are recorded. A value that is undefined in it,
// in the one before or in the ones behind, in a list and as members.
function makeMasked(keys, mask, seed) {
    const object = {};
    for (let i = 0; i < keys.length; i++)
        object[keys[i]] = (mask >> i) & 1 ? undefined : seed * 100 + i;
    return object;
}

for (let firstMask = 0; firstMask < 8; firstMask++) {
    for (let secondMask = 0; secondMask < 8; secondMask++) {
        const keys = ['a', 'b', 'c'];
        const list = [makeMasked(keys, firstMask, 1), makeMasked(keys, secondMask, 2)];
        for (let mask = 0; mask < 8; mask++)
            list.push(makeMasked(keys, mask, mask + 3));
        check(`undefined masks ${firstMask} and ${secondMask}`, list);
        check(`undefined masks ${firstMask} and ${secondMask} as members`, Object.fromEntries(list.map((object, i) => ["member" + i, object])));
        // A third object that is recorded where the second one could not be.
        check(`undefined masks ${firstMask} and ${secondMask}, then two whole ones`, [list[0], list[1], makeMasked(keys, 0, 20), makeMasked(keys, 0, 21), makeMasked(keys, 5, 22)]);
    }
}

// Many Structures that take turns. Some of them share a slot of the table, and a Structure that shares its slot with
// the one that came in between is seen for the first time again.
function makeShaped(shape, i) {
    const object = { id: i };
    object["key" + shape] = "s" + shape;
    object.value = i * 1.5;
    if (shape % 3 === 0)
        object["extra" + shape] = [shape];
    return object;
}

for (const shapeCount of [2, 3, 15, 16, 17, 40, 300]) {
    for (const repeats of [1, 2, 3, 5]) {
        const inTurn = [];
        const inRuns = [];
        for (let round = 0; round < repeats; round++) {
            for (let shape = 0; shape < shapeCount; shape++)
                inTurn.push(makeShaped(shape, round * 1000 + shape));
        }
        for (let shape = 0; shape < shapeCount; shape++) {
            for (let round = 0; round < repeats; round++)
                inRuns.push(makeShaped(shape, round * 1000 + shape));
        }
        check(`${shapeCount} shapes in turn, ${repeats} times`, inTurn);
        check(`${shapeCount} shapes in runs of ${repeats}`, inRuns);
        check(`${shapeCount} shapes both ways, ${repeats}`, { inTurn, inRuns, one: inTurn[0] });
    }
}

// Objects of one Structure inside each other: the inner one ends, and is recorded, before the outer one.
function makeChain(length) {
    let object = { next: null, value: 0 };
    for (let i = 1; i < length; i++)
        object = { next: object, value: i };
    return object;
}

function makeTree(depth, i) {
    if (!depth)
        return { value: i, children: [] };
    return { value: i, children: [makeTree(depth - 1, i + 1), makeTree(depth - 1, i + 2)] };
}

for (const length of [1, 2, 3, 4, 5, 31, 32, 33, 34, 64, 65, 66, 100])
    check(`chain of ${length}`, makeChain(length));
for (let depth = 0; depth <= 6; depth++)
    check(`tree of depth ${depth}`, makeTree(depth, 0));

// Objects with more properties than a prepared Structure holds, and with none.
function makeWide(count, seed) {
    const object = {};
    for (let i = 0; i < count; i++)
        object["p" + i] = seed + i;
    return object;
}

for (const count of [0, 1, 31, 32, 33, 64, 65, 200]) {
    const list = [];
    for (let i = 0; i < 4; i++)
        list.push(makeWide(count, i * 1000));
    check(`four objects with ${count} properties`, list);
}

// Results that need the dynamic buffer, where the table is larger. Two big results make the next call start there.
function makeBig(count, shapes) {
    const list = [];
    for (let i = 0; i < count; i++)
        list.push(makeShaped(i % shapes, i));
    return list;
}

for (const shapes of [1, 2, 255, 256, 257, 700]) {
    const big = makeBig(1500, shapes);
    if (JSON.stringify(big).length <= 8192)
        throw new Error("Output should be larger than the static buffer");
    check(`big with ${shapes} shapes`, big);
    check(`big with ${shapes} shapes again`, big);
    check(`small after big with ${shapes} shapes`, makeBig(3, shapes));
    check(`one object after big`, makeShaped(0, 1));
}

// A result that ends in a long string, written again and again: from the third call on the buffer starts with room for a
// result like the last two, and the string asks for six times its length before it is written.
for (const tailLength of [63, 64, 65, 300, 4095, 4096, 4097, 20000]) {
    for (const unit of ["x", "\u4e2d", "\"", "\u0001"]) {
        const list = makeBig(1200, 3);
        list.push(unit.repeat(tailLength));
        for (let call = 0; call < 3; call++)
            check(`ends in ${tailLength} times ${JSON.stringify(unit)}, call ${call}`, list);
        check(`a short result behind it`, makeBig(2, 3));
        check(`the long string alone`, unit.repeat(tailLength));
    }
}
