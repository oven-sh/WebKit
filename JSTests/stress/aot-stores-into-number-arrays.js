//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function checkElements(array, expected, what) {
    check(array.length, expected.length, "the length of " + what);
    for (let i = 0; i < expected.length; i++) {
        check(i in array, true, "element " + i + " of " + what + " is there");
        check(array[i], expected[i], "element " + i + " of " + what);
    }
}
function checkThroughAllShapes(array, expected, what) {
    checkElements(array, expected, what);
    array.push(0.5);
    expected = expected.concat([0.5]);
    checkElements(array, expected, what + " after a fraction");
    array.push("text");
    checkElements(array, expected.concat(["text"]), what + " after a string");
}
noInline(checkElements);
noInline(checkThroughAllShapes);

function arrayOf(value, count) { return new Array(count).fill(value); }
noInline(arrayOf);
const int32s = count => arrayOf(7, count);
const doubles = count => arrayOf(0.25, count);
const roomy = array => { array.length += 8; array.length -= 8; array.push(0); array.pop(); return array; };

const boxedAsDouble = new Float64Array([3, 0, -1, 255, 2147483647, -2147483648]);

function storesPropertyOfBytes(out, bytes, key, count) {
    $$t(bytes, 2560);
    const value = $$t(bytes[key], 8);
    for (let i = 0; i < count; i++)
        out[i] = value;
}
function pushesPropertyOfBytes(out, bytes, key, count) {
    $$t(bytes, 2560);
    const value = $$t(bytes[key], 8);
    for (let i = 0; i < count; i++)
        out.push(value);
}
function storesLengthOfBytes(out, bytes, count) {
    $$t(bytes, 2560);
    const value = $$t(bytes.length, 8);
    for (let i = 0; i < count; i++)
        out[i] = value;
}
for (let f of [storesPropertyOfBytes, pushesPropertyOfBytes, storesLengthOfBytes])
    noInline(f);

{
    const bytes = new Uint8Array(4);
    bytes.three = boxedAsDouble[0];
    bytes.zero = boxedAsDouble[1];
    bytes.most = boxedAsDouble[3];
    for (let [key, value] of [["three", 3], ["zero", 0], ["most", 255]]) {
        let out = int32s(4);
        storesPropertyOfBytes(out, bytes, key, 4);
        checkThroughAllShapes(out, [value, value, value, value], "integers overwritten with the property " + key);
        pushesPropertyOfBytes(roomy(int32s(2)), bytes, 0, 3);
        out = roomy(int32s(2));
        pushesPropertyOfBytes(out, bytes, key, 3);
        checkThroughAllShapes(out, [7, 7, value, value, value], "integers and the pushed property " + key);
        out = doubles(4);
        storesPropertyOfBytes(out, bytes, key, 4);
        checkThroughAllShapes(out, [value, value, value, value], "fractions overwritten with the property " + key);
    }
    const shadowed = new Uint8Array(4);
    Object.defineProperty(shadowed, "length", { value: boxedAsDouble[0] });
    const out = int32s(4);
    storesLengthOfBytes(out, shadowed, 4);
    checkThroughAllShapes(out, [3, 3, 3, 3], "integers overwritten with a length");
}

function storesAnything(out, value, count) { for (let i = 0; i < count; i++) out[i] = value; }
function pushesAnything(out, value, count) { for (let i = 0; i < count; i++) out.push(value); }
function copiesInt32s(out, from) { $$t(from, 6656); for (let i = 0; i < from.length; i++) out[i] = from[i]; }
function pushesInt32s(out, from) { $$t(from, 6656); for (let i = 0; i < from.length; i++) out.push(from[i]); }
function copiesUint32s(out, from) { $$t(from, 7680); for (let i = 0; i < from.length; i++) out[i] = from[i]; }
function pushesUint32s(out, from) { $$t(from, 7680); for (let i = 0; i < from.length; i++) out.push(from[i]); }
function copiesDoubles(out, from) { $$t(from, 10752); for (let i = 0; i < from.length; i++) out[i] = from[i]; }
function pushesDoubles(out, from) { $$t(from, 10752); for (let i = 0; i < from.length; i++) out.push(from[i]); }
function copiesSuccessors(out, from) { $$t(from, 6656); for (let i = 0; i < from.length; i++) out[i] = from[i] + 1; }
function pushesSuccessors(out, from) { $$t(from, 6656); for (let i = 0; i < from.length; i++) out.push(from[i] + 1); }
function copiesHalves(out, from) { $$t(from, 6656); for (let i = 0; i < from.length; i++) out[i] = from[i] / 2; }
function pushesHalves(out, from) { $$t(from, 6656); for (let i = 0; i < from.length; i++) out.push(from[i] / 2); }
for (let f of [storesAnything, pushesAnything, copiesInt32s, pushesInt32s, copiesUint32s, pushesUint32s, copiesDoubles, pushesDoubles, copiesSuccessors, pushesSuccessors, copiesHalves, pushesHalves])
    noInline(f);

const kinds = [["integers", int32s, 7], ["fractions", doubles, 0.25]];
for (let [kind, make, filler] of kinds) {
    for (let value of [1, -1, 0, -0, 0.5, NaN, Infinity, -Infinity, 2147483647, 2147483648, -2147483648, -2147483649, 4294967296, 2 ** 53, boxedAsDouble[0], boxedAsDouble[1], boxedAsDouble[4], boxedAsDouble[5], undefined, null, true, "text"]) {
        let out = make(3);
        storesAnything(out, value, 3);
        checkThroughAllShapes(out, [value, value, value], kind + " overwritten with " + String(value));
        pushesAnything(roomy(make(2)), filler, 2);
        out = roomy(make(2));
        pushesAnything(out, value, 2);
        checkThroughAllShapes(out, [filler, filler, value, value], kind + " and pushed " + String(value));
        out = roomy(make(2));
        storesAnything(out, value, 4);
        checkThroughAllShapes(out, [value, value, value, value], kind + " overwritten and extended with " + String(value));
    }

    const typedCases = [
        [copiesInt32s, pushesInt32s, new Int32Array([0, 1, -1, 2147483647, -2147483648]), x => x],
        [copiesUint32s, pushesUint32s, new Uint32Array([0, 1, 2147483647]), x => x],
        [copiesUint32s, pushesUint32s, new Uint32Array([0, 2147483647, 2147483648, 4294967295, 5]), x => x],
        [copiesDoubles, pushesDoubles, new Float64Array([0, 1, -1, 2147483647, -2147483648]), x => x],
        [copiesDoubles, pushesDoubles, new Float64Array([1, -0, 2]), x => x],
        [copiesDoubles, pushesDoubles, new Float64Array([1, NaN, 2]), x => x],
        [copiesDoubles, pushesDoubles, new Float64Array([1, 0.5, 2, Infinity, -Infinity, 2147483648, -2147483649, 1e300]), x => x],
        [copiesSuccessors, pushesSuccessors, new Int32Array([0, -1, 5, -2147483648]), x => x + 1],
        [copiesSuccessors, pushesSuccessors, new Int32Array([0, 2147483646, 2147483647, 3]), x => x + 1],
        [copiesHalves, pushesHalves, new Int32Array([0, 2, -4, 2147483646]), x => x / 2],
        [copiesHalves, pushesHalves, new Int32Array([2, 1, -1, 4]), x => x / 2],
    ];
    for (let [copies, pushes, from, expectedFor] of typedCases) {
        const expected = Array.from(from, expectedFor);
        let out = make(from.length);
        copies(out, from);
        checkThroughAllShapes(out, expected, kind + " after " + copies.name + " of " + expected.join());
        out = roomy(make(1));
        copies(out, from);
        checkThroughAllShapes(out, expected, kind + " extended by " + copies.name + " of " + expected.join());
        pushes(roomy(make(2)), new from.constructor([2]));
        out = roomy(make(2));
        pushes(out, from);
        checkThroughAllShapes(out, [filler, filler].concat(expected), kind + " after " + pushes.name + " of " + expected.join());
    }
}

function sumsAfterStoring(out, bytes, key) {
    $$t(bytes, 2560);
    const value = $$t(bytes[key], 8);
    for (let i = 0; i < out.length; i++)
        out[i] = value;
    let sum = 0;
    for (let i = 0; i < out.length; i++)
        sum += out[i];
    return sum;
}
noInline(sumsAfterStoring);
{
    const bytes = new Uint8Array(4);
    bytes.three = boxedAsDouble[0];
    check(sumsAfterStoring(int32s(5), bytes, "three"), 15, "the sum of what was stored");
    check(sumsAfterStoring(int32s(5), bytes, 0), 0, "the sum of stored elements");
}
