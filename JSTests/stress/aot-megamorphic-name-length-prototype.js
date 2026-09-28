//@ runDefault("--useAOT=1")
// Names that functions, arrays and strings have a say of their own about, at sites that see many structures.
function name(o) { return o.name; }
function length(o) { return o.length; }
function prototype(o) { return o.prototype; }
noInline(name); noInline(length); noInline(prototype);

let objects = [];
for (let i = 0; i < 40; ++i) {
    let o = {};
    for (let j = 0; j < i; ++j)
        o["p" + j] = j;
    if (i % 3)
        o.name = "n" + i;
    if (i % 4)
        o.length = i;
    if (i % 5)
        o.prototype = "proto" + i;
    objects.push(o);
}
function f(a, b) { }
class C { static m() { } }
class D extends C { }
let bound = f.bind(null, 1);
let inheritsFromFunction = Object.create(f);
let inheritsFromArray = Object.create([1, 2, 3]);
let others = [f, C, D, bound, () => { }, [1, 2], [], new String("abc"), inheritsFromFunction, inheritsFromArray, new Uint8Array(4), function named() { }];

function expected(o, key) { return Reflect.get(o, key); }
for (let round = 0; round < 200; ++round) {
    for (let o of objects) {
        if (name(o) !== expected(o, "name") || length(o) !== expected(o, "length") || prototype(o) !== expected(o, "prototype"))
            throw new Error("plain object, round " + round);
    }
    for (let o of others) {
        if (name(o) !== expected(o, "name") || length(o) !== expected(o, "length") || prototype(o) !== expected(o, "prototype"))
            throw new Error("other object, round " + round + " " + String(o));
    }
    if (round == 100) {
        // What was absent is there now, one way and another.
        Object.prototype.name = "inherited";
        objects[0].length = 99;
        delete objects[1].name;
        Object.defineProperty(f, "name", { value: "renamed" });
        others[5].length = 1;
    }
}
delete Object.prototype.name;
if (name(objects[0]) !== undefined || name(f) !== "renamed" || length(others[5]) !== 1)
    throw new Error("at the end");
