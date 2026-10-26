// Reading an array that became a copy-on-write array (see immutable-properties-array-storage.js): every read agrees with an ordinary
// array of the same contents, an object that inherits from the array still takes its own properties, and the elements, which are
// reachable only through the array, survive collections.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function snapshot(object) {
    return JSON.stringify(Reflect.ownKeys(object).map(key => {
        let d = Object.getOwnPropertyDescriptor(object, key);
        return [String(key), "value" in d ? (typeof d.value === "object" || typeof d.value === "function" ? typeof d.value : String(d.value)) : "accessor", d.writable, d.enumerable, d.configurable];
    })) + " length:" + String(object.length) + " extensible:" + Object.isExtensible(object);
}

// An allocation site learns from what becomes of its arrays: an array that is written to, frozen or given dictionary indexing
// teaches its site to hand out slower storage. So the arrays that must start without holes and in Int32, Double or Contiguous shape come from sites of their own, whose arrays
// are never written after they are built, and are all built before anything else here runs.
function int32Subject() { let a = []; for (let i = 0; i < 8; ++i) a.push(i); return a; }
function doubleSubject() { let a = []; for (let i = 0; i < 8; ++i) a.push(i + 0.5); return a; }
function contiguousSubject() { let a = []; for (let i = 0; i < 8; ++i) a.push(i % 2 ? "s" + i : { i }); return a; }
function largeSubject() { let a = []; for (let i = 0; i < 300; ++i) a.push(i); return a; }
function literalSubject() { return [1, 2, 3, 4]; }
class Derived extends Array { }
function derivedSubject() { let a = new Derived; for (let i = 0; i < 8; ++i) a.push(i); return a; }
let copyOnWriteKinds = { int32: int32Subject, double: doubleSubject, contiguous: contiguousSubject, large: largeSubject, literal: literalSubject, derived: derivedSubject };

// 1. Reads agree with an ordinary array of the same contents, hot.
{
    let reads = [(a, i) => a[i], (a, i) => a.at(-1 - i), a => a.length, (a, i) => a.indexOf(a[i]), (a, i) => a.includes(a[i]), (a, i) => a.lastIndexOf(a[i]), (a, i) => a.slice(i).length, a => a.map(x => x).length,
        a => a.filter(() => true).length, a => { let n = 0; for (let x of a) n++; return n; }, a => [...a].length, a => Math.max(...a.map(Number).filter(x => x === x), 0), a => a.join(), a => String(a), a => `${a}`, a => a.concat([1]).length,
        a => Object.keys(a).length, a => a.reduce(n => n + 1, 0), a => { let [x, y] = a; return typeof x + typeof y; }, a => Array.from(a).length, a => a.toReversed().length, a => a.toSorted(() => 0).length, a => a.with(0, 1)[0], a => a.flat().length,
        a => a.findLast(() => true) === a[a.length - 1], a => JSON.stringify(a), a => a.entries().next().value[0], a => a.slice().constructor === a.constructor, a => (function () { return arguments.length; })(...a)];
    for (let name in copyOnWriteKinds) {
        let subject = $vm.makePropertiesImmutable(copyOnWriteKinds[name]()), twin = copyOnWriteKinds[name]();
        for (let read of reads) {
            for (let i = 0; i < Math.ceil(testLoopCount / 100); ++i)
                shouldBe(String(read(subject, i % 4)), String(read(twin, i % 4)), name + " " + String(read));
        }
        let copy = subject.slice(); copy[0] = "changed"; copy.push(1);
        shouldBe(copy[0], "changed"); shouldBe($vm.hasImmutableProperties(copy), false); shouldBe($vm.hasImmutableProperties([...subject]), false);
        shouldBe(Object.isFrozen(subject), false); shouldBe(Object.getOwnPropertyDescriptor(subject, 0).writable, true);
    }
}

// 2. An inheritor: JSArray::put is reached with another receiver. The inheritor gets its own properties; the array is untouched.
for (let name in copyOnWriteKinds) {
    let array = $vm.makePropertiesImmutable(copyOnWriteKinds[name]()), before = snapshot(array), mode = $vm.indexingMode(array);
    let child = Object.create(array);
    let rounds = Math.ceil(testLoopCount / 100);
    for (let i = 0; i < rounds; ++i) { child.named = i; child[0] = "own"; child[20] = i; child.length = 50; Array.prototype.push.call(child, i); }
    shouldBe(child.named, rounds - 1); shouldBe(child[0], "own"); shouldBe(Object.hasOwn(child, "length"), true);
    shouldBe(snapshot(array), before, name + " under an inheritor"); shouldBe($vm.indexingMode(array), mode);
}

// 3. Garbage collection: the elements are reachable only through the array.
{
    let arrays = [];
    for (let i = 0; i < 100; ++i) { let a = []; for (let k = 0; k < 40; ++k) a.push({ value: i * 100 + k }); arrays.push($vm.makePropertiesImmutable(a)); }
    for (let round = 0; round < 3; ++round) { fullGC(); let junk = []; for (let i = 0; i < 2000; ++i) junk.push({ i }); edenGC(); }
    for (let i = 0; i < arrays.length; ++i) { for (let k = 0; k < 40; ++k) shouldBe(arrays[i][k].value, i * 100 + k); }
}
