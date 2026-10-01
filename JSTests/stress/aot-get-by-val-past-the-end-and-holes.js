//@ runDefault("--compileMainScriptAheadOfTime=1")
// Reading past the end of an array or in a hole is undefined only for as long as nothing it inherits from has such a property.
function at(a, i) { return a[i]; }
noInline(at);
function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
class Sub extends Array { }
let ints = [1, 2, 3], doubles = [1.5, 2.5], objects = [{}, "s"], holey = [1, , 3], empty = [], sub = Sub.from([1, 2]);
let withProto = [1, 2]; Object.setPrototypeOf(withProto, { 5: "own prototype" });
let sparse = []; sparse[100000] = 1;
for (let i = 0; i < 500; ++i) {
    for (let a of [ints, doubles, objects, empty, sub]) {
        check(at(a, 5), undefined, "past the end");
        check(at(a, 7), undefined, "past the end");
        check(at(a, 4294967294), undefined, "far past the end");
    }
    check(at(holey, 1), undefined, "hole");
    check(at(withProto, 5), "own prototype", "another prototype");
    check(at(sparse, 5), undefined, "sparse");
    check(at(sparse, 100000), 1, "sparse");
}
Array.prototype[5] = "array prototype";
for (let a of [ints, doubles, objects, empty, sub, sparse])
    check(at(a, 5), "array prototype", "after Array.prototype[5]");
check(at(ints, 7), undefined, "still nothing at 7");
Object.prototype[7] = "object prototype";
for (let a of [ints, doubles, objects, empty, sub, sparse, withProto])
    check(at(a, 7), "object prototype", "after Object.prototype[7]");
Object.defineProperty(Array.prototype, 1, { get() { return "getter"; }, configurable: true });
check(at(holey, 1), "getter", "hole, with a getter to inherit");
check(at(empty, 1), "getter", "past the end, with a getter to inherit");
delete Array.prototype[5]; delete Array.prototype[1]; delete Object.prototype[7];
check(at(ints, 5), undefined, "and gone again");
