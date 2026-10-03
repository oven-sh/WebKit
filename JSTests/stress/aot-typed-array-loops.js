//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    if (!remarks || !remarks.includes("split-loop"))
        return;
    for (let pattern of patterns) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}
const hoists = "hoists-uint8array-storage-if-any", checksCounter = "checks-counter-at-loop-entry", reuses = "reuses-guarded-element-read";

function findNewline(buffer, startIndex) {
    const newline = 0x0a, carriage = 0x0d;
    for (let i = startIndex ?? 0; i < buffer.length; i++) {
        if (buffer[i] === newline)
            return i;
        if (buffer[i] === carriage)
            return -i;
    }
    return null;
}
function findDoubleNewline(buffer) {
    const newline = 0x0a, carriage = 0x0d;
    for (let i = 0; i < buffer.length - 1; i++) {
        if (buffer[i] === newline && buffer[i + 1] === newline)
            return i + 2;
        if (buffer[i] === carriage && buffer[i + 1] === carriage)
            return i + 2;
    }
    return -1;
}
function countLength(a) { let n = 0; for (let i = 0; i < a.length; i++) n += 1; return n; }
function sumElements(a) { let sum = 0; for (let i = 0; i < a.length; i++) sum += a[i]; return sum; }
function countDown(a, from) { let sum = 0; for (let i = from; i >= 0; i--) sum += a[i] === 7 ? 1 : 0; return sum; }
for (const f of [findNewline, findDoubleNewline, countLength, sumElements, countDown])
    noInline(f);
applies(findNewline, hoists, checksCounter, reuses);
applies(findDoubleNewline, hoists, reuses);
doesNotApply(findDoubleNewline, checksCounter);
applies(countDown, hoists, checksCounter);

function comparesWithUnknown(a, wanted) { for (let i = 0; i < a.length; i++) { if (a[i] === wanted) return i; } return -1; }
function writesElements(a) { let n = 0; for (let i = 0; i < a.length; i++) { if (a[i] === 1) a[i] = 2; if (a[i] === 2) n++; } return n; }
function baseChanges(x, y) { let n = 0; let a = x; for (let i = 0; i < a.length; i++) { n += a[i] === 1 ? 1 : 0; a = i & 1 ? y : x; } return n; }
function callsInside(a, f) { let n = 0; for (let i = 0; i < a.length; i++) { if (a[i] === 1) n += f(a); } return n; }
for (const f of [comparesWithUnknown, writesElements, baseChanges, callsInside])
    noInline(f);
doesNotApply(comparesWithUnknown, hoists);
doesNotApply(writesElements, reuses);
doesNotApply(baseChanges, hoists);
doesNotApply(callsInside, hoists);

function bytes(text) { return Uint8Array.from(text, c => c.charCodeAt(0)); }
class Short extends Uint8Array { get length() { return 2; } }
class Plain extends Uint8Array { }
class Deeper extends Plain { }
class Deepest extends Deeper { }
class Changing extends Uint8Array { }
function straightLength(a) { return a.length; }
noInline(straightLength);
const ownLength = new Uint8Array(4);
Object.defineProperty(ownLength, "length", { value: 1 });
const textLength = new Uint8Array(4);
Object.defineProperty(textLength, "length", { value: "3" });
const otherPrototype = new Uint8Array(4);
Object.setPrototypeOf(otherPrototype, { get length() { return 3; } });
const resizable = new Uint8Array(new ArrayBuffer(4, { maxByteLength: 16 }));
const detached = new Uint8Array(8);
detached.buffer.transfer();
for (let round = 0; round < 60; round++) {
    check(findNewline(bytes("ab\ncd"), null), 2, "a newline");
    check(findNewline(bytes("ab\rcd"), undefined), -2, "a carriage return");
    check(findNewline(bytes("abcd"), 0), null, "neither");
    check(findNewline(bytes("a\nb\nc"), 2), 3, "from an index");
    check(findNewline(bytes("a\nb"), 1.5), null, "from an index that is no integer");
    check(findNewline(bytes("a\nb"), "1"), "1", "from a string");
    check(findNewline(bytes("ab\n"), "1"), 2, "from a string, further on");
    check(findNewline(bytes("a\nb"), -1), 1, "from a negative index");
    check(findNewline(bytes("a\nb"), 100), null, "from behind the end");
    check(findNewline(bytes("a\nb"), true), null, "from true");
    let message = "none";
    try { findNewline(bytes("ab"), 0n); } catch (error) { message = error.constructor.name; }
    check(message, "none", "from a BigInt");
    check(findNewline([97, 10], null), 1, "an array");
    check(findNewline([97, , 10], null), 2, "an array with a hole");
    check(findNewline([97.5, 10], null), 1, "an array of doubles");
    check(findNewline(["a", 10], null), 1, "an array of anything");
    check(findNewline("a\n", null), null, "a string");
    check(findNewline({ length: 2, 0: 1, 1: 10 }, null), 1, "an object with a length");
    check(findNewline(new Int8Array([-1, 10]), null), 1, "an Int8Array");
    check(findNewline(new Uint16Array([266, 10]), null), 1, "a Uint16Array");
    check(findNewline(new Float64Array([10.5, 10]), null), 1, "a Float64Array");
    check(findNewline(new Uint8ClampedArray([300, 10]), null), 1, "a Uint8ClampedArray");
    check(findNewline(new Plain([1, 10]), null), 1, "an object of a subclass");
    check(findNewline(new Uint8Array(0), null), null, "no bytes");
    check(findNewline(new Uint8Array([1, 2, 10, 3]).subarray(1), null), 1, "a part of a buffer");
    check(findDoubleNewline(bytes("ab\n\ncd")), 4, "two newlines");
    check(findDoubleNewline(bytes("ab\r\rcd")), 4, "two carriage returns");
    check(findDoubleNewline(bytes("a\nb\n")), -1, "no two in a row");
    check(findDoubleNewline(bytes("\n")), -1, "one byte");
    check(findDoubleNewline(new Uint8Array(0)), -1, "no byte");
    check(findDoubleNewline([10, 10]), 2, "two newlines in an array");
    check(countLength(new Uint8Array(4)), 4, "the length");
    check(countLength(ownLength), 1, "a length of its own");
    check(countLength(textLength), 3, "a length of its own that is a string");
    check(countLength(new Short(4)), 2, "the length of a subclass");
    check(countLength(new Plain(4)) + countLength(new Deeper(5)) + countLength(new Deepest(6)), 15, "the lengths of subclasses that have none of their own");
    check([new Uint8Array(1), new Plain(2), new Uint8Array(3), new Deeper(4), new Short(5), ownLength, new Int16Array(6), "seven..", [8]].map(straightLength).join(), "1,2,3,4,2,1,6,7,1", "a length that is read outside loops");
    check(countLength(otherPrototype), 3, "the length of another prototype");
    check(countLength(resizable), 4, "a buffer that can be resized");
    check(countLength(detached), 0, "a detached buffer");
    check(sumElements(new Uint8Array([1, 2, 3])), 6, "a sum of bytes");
    check(sumElements(new Int8Array([-1, -2])), -3, "a sum of signed bytes");
    check(sumElements(new Uint32Array([4294967295, 1])), 4294967296, "a sum of large numbers");
    check(sumElements(new Float32Array([0.5, 0.25])), 0.75, "a sum of floats");
    check(sumElements(new Short([5, 6, 7])), 11, "a sum up to the length of a subclass");
    check(sumElements([1, 2, 3]), 6, "a sum of an array");
    check(countDown(new Uint8Array([7, 1, 7]), 2), 2, "down");
    check(countDown(new Uint8Array([7, 1, 7]), 5), 2, "down from behind the end");
    check(countDown(new Uint8Array([7, 1, 7]), "2"), 2, "down from a string");
    check(countDown(new Uint8Array([7, 1, 7]), 1.5), 0, "down from a number that is no integer");
    check(comparesWithUnknown(new Uint8Array([1, 2]), 2) + comparesWithUnknown(["a", "b"], "b"), 2, "a comparison with anything");
    check(writesElements(new Uint8Array([1, 2, 1])), 3, "elements that are written in the loop");
    check(baseChanges(new Uint8Array([1, 1, 1]), [1, 0, 1]), 3, "a base that changes in the loop");
    check(callsInside(new Uint8Array([1, 0, 1]), a => a.length), 6, "a call in the loop");
    const shrinks = new Uint8Array(new ArrayBuffer(8, { maxByteLength: 8 }));
    shrinks.fill(1);
    check(callsInside(shrinks, a => { a.buffer.resize(2); return 1; }), 2, "a buffer that is resized in the loop");
}
resizable.buffer.resize(6);
check(countLength(resizable), 6, "a buffer that was resized");

for (let round = 0; round < 20; round++)
    check(countLength(new Changing(4)) + straightLength(new Changing(3)), 7, "a subclass before it changes");
Object.defineProperty(Changing.prototype, "length", { get() { return 1; }, configurable: true });
check(countLength(new Changing(4)) + straightLength(new Changing(3)), 2, "after its prototype gets a length");
delete Changing.prototype.length;
check(countLength(new Changing(4)) + straightLength(new Changing(3)), 7, "after it is deleted");
Object.setPrototypeOf(Changing.prototype, { get length() { return 2; } });
check(countLength(new Changing(4)) + straightLength(new Changing(3)), 4, "after its prototype gets another prototype");
Object.setPrototypeOf(Changing.prototype, Uint8Array.prototype);
check(countLength(new Changing(4)) + straightLength(new Changing(3)), 7, "after it gets its prototype back");

const viewPrototype = Object.getPrototypeOf(Uint8Array.prototype);
const original = Object.getOwnPropertyDescriptor(viewPrototype, "length");
check(countLength(new Uint8Array(4)), 4, "before the getter is replaced");
Object.defineProperty(viewPrototype, "length", { get() { return 1; }, configurable: true });
check(countLength(new Uint8Array(4)), 1, "after the getter is replaced");
check(straightLength(new Uint8Array(4)) + straightLength(new Plain(4)), 2, "and outside loops");
check(findNewline(bytes("ab\n"), null), null, "and in the search");
Object.defineProperty(viewPrototype, "length", original);
check(countLength(new Uint8Array(4)), 4, "after the getter is put back");
Object.defineProperty(Uint8Array.prototype, "length", { get() { return 2; }, configurable: true });
check(countLength(new Uint8Array(4)), 2, "after Uint8Array.prototype gets a length");
check(straightLength(new Uint8Array(4)) + straightLength(new Plain(4)) + countLength(new Plain(4)), 6, "which subclasses see too");
check(countLength(new Int8Array(4)), 4, "which other typed arrays do not see");
delete Uint8Array.prototype.length;
check(countLength(new Uint8Array(4)), 4, "after it is deleted");
Object.setPrototypeOf(Uint8Array.prototype, { get length() { return 3; } });
check(countLength(new Uint8Array(4)), 3, "after Uint8Array.prototype gets another prototype");
Object.setPrototypeOf(Uint8Array.prototype, viewPrototype);
check(countLength(new Uint8Array(4)), 4, "after it gets its prototype back");
