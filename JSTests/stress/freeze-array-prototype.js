function assert(cond, msg) {
    if (!cond)
        throw new Error("FAIL: " + msg);
}

function throwsTypeError(f) {
    try {
        f();
    } catch (e) {
        return e instanceof TypeError;
    }
    return false;
}

function strictSetLength(o, v) { "use strict"; o.length = v; }
function strictSetIndex(o, i, v) { "use strict"; o[i] = v; }

function readHoles(n) {
    const holey = [1, , 3, , 5];
    let undefs = 0;
    for (let i = 0; i < n; i++) {
        if (holey[i % 5] === undefined)
            undefs++;
    }
    return undefs;
}
assert(readHoles(1e4) === 4e3, "holes before freeze");

const AP = Array.prototype;
Object.freeze(AP);

assert(Object.isFrozen(AP), "isFrozen");
assert(!Object.isExtensible(AP), "not extensible");
const lengthDesc = Object.getOwnPropertyDescriptor(AP, "length");
assert(lengthDesc.value === 0 && !lengthDesc.writable && !lengthDesc.configurable && !lengthDesc.enumerable, "length descriptor");
assert(Object.getOwnPropertyDescriptor(AP, "push").writable === false, "method read-only");

assert(throwsTypeError(() => strictSetLength(AP, 1)), "strict length write throws");
AP.length = 1;
assert(AP.length === 0, "sloppy length write ignored");
assert(throwsTypeError(() => strictSetLength(AP, 0)), "strict same-value length write throws");
assert(Reflect.set(AP, "length", 0) === false, "Reflect.set length");
assert(Reflect.defineProperty(AP, "length", { value: 0 }) === true, "same-value define length");
assert(Reflect.defineProperty(AP, "length", { value: 1 }) === false, "different-value define length");

assert(throwsTypeError(() => strictSetIndex(AP, 0, 1)), "strict index write throws");
AP[0] = 1;
assert(AP[0] === undefined && !Object.hasOwn(AP, 0), "sloppy index write ignored");
assert(throwsTypeError(() => Object.defineProperty(AP, 0, { value: 1 })), "define index throws");
assert(Reflect.defineProperty(AP, 3, { value: 1 }) === false, "Reflect.defineProperty index");

assert(throwsTypeError(() => AP.push.call(AP, 1)), "push throws");
assert(throwsTypeError(() => AP.push.call(AP)), "push with no args throws");
assert(throwsTypeError(() => AP.pop.call(AP)), "pop throws");
assert(throwsTypeError(() => AP.shift.call(AP)), "shift throws");
assert(throwsTypeError(() => AP.unshift.call(AP, 1)), "unshift throws");
assert(throwsTypeError(() => AP.unshift.call(AP)), "unshift no args throws");
assert(throwsTypeError(() => AP.splice.call(AP, 0, 0, 1)), "splice insert throws");
assert(AP.length === 0 && Object.getOwnPropertyNames(AP).every(k => isNaN(+k) || k === ""), "no indexed props leaked");

for (let i = 0; i < 1e4; i++) {
    assert(throwsTypeError(() => AP.push.call(AP, i)), "push throws (warm)");
    assert(throwsTypeError(() => AP.pop.call(AP)), "pop throws (warm)");
    assert(throwsTypeError(() => strictSetLength(AP, i)), "length write throws (warm)");
}

assert(readHoles(1e4) === 4e3, "holes after freeze");
assert([1, , 3].includes(undefined) && [, 2].indexOf(undefined) === -1, "includes/indexOf holes");
assert([...[1, , 3]].length === 3 && [...[1, , 3]][1] === undefined, "spread holes");
assert([1, , 3].slice(0)[1] === undefined && !(1 in [1, , 3].slice(0)), "slice holes");

const frozenEmpty = Object.freeze([]);
assert(throwsTypeError(() => frozenEmpty.push(1)) && throwsTypeError(() => frozenEmpty.pop()), "frozen empty literal");
const frozenNewArray = Object.freeze(new Array());
assert(throwsTypeError(() => frozenNewArray.push(1)) && throwsTypeError(() => frozenNewArray.pop()), "frozen new Array()");
assert(throwsTypeError(() => strictSetLength(frozenNewArray, 0)), "frozen new Array() length");
assert(Object.isFrozen(frozenNewArray) && frozenNewArray.length === 0, "frozen new Array() state");

const sealed = Object.seal(new Array());
assert(throwsTypeError(() => sealed.push(1)), "sealed empty push throws");
sealed.length = 5;
assert(sealed.length === 5 && !(0 in sealed), "sealed empty length writable");

if (typeof $vm !== "undefined")
    assert($vm.indexingMode(AP) === "ArrayClass", "frozen Array.prototype keeps blank indexing after rejected writes: " + $vm.indexingMode(AP));
