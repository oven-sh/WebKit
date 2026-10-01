//@ runDefault("--compileMainScriptAheadOfTime=1")
// What op_get_by_val and a megamorphic op_get_by_id find without asking the runtime, and what they had better still ask it about.
function at(o, k) { return o[k]; }
function atInTry(o, k) { try { return o[k]; } catch (e) { return "caught " + e.message; } }
function value(o) { return o.value; }
function valueInTry(o) { try { return o.value; } catch (e) { return "caught " + e.message; } }
noInline(at); noInline(atInTry); noInline(value); noInline(valueInTry);

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

// ---- Indices that are doubles.
let array = [10, 11, 12, 13];
let doubles = [0.5, 1.5, 2.5];
let holey = [1, , 3];
let ints = new Int32Array([7, 8, 9]);
let floats = new Float64Array([1.25, 2.25]);
let half = 0.5;
for (let i = 0; i < 300; ++i) {
    check(at(array, half * 2), 11, "double index");
    check(at(array, half * 6), 13, "double index");
    check(at(array, -0), 10, "minus zero");
    check(at(array, half), undefined, "fraction");
    check(at(array, NaN), undefined, "NaN");
    check(at(array, -1.0 * (half * 2)), undefined, "negative");
    check(at(array, 4294967296), undefined, "too big");
    check(at(array, half * 8), undefined, "past the end");
    check(at(array, Infinity), undefined, "infinity");
    check(at(doubles, half * 4), 2.5, "doubles");
    check(at(holey, half * 2), undefined, "hole");
    check(at(ints, half * 4), 9, "typed array");
    check(at(ints, half * 6), undefined, "typed array, past the end");
    check(at(ints, half), undefined, "typed array, fraction");
    check(at(floats, half * 2), 2.25, "typed array of doubles");
    check(at("abc", half * 2), "b", "string");
}
array[1.5] = "named";
check(at(array, 1.5), "named", "a property whose name is a fraction");

// ---- Names.
let symbol = Symbol("s");
let calls = 0;
let shapes = [];
for (let i = 0; i < 30; ++i) {
    let o = {};
    for (let j = 0; j < i; ++j)
        o["pad" + j] = j;
    switch (i % 5) {
    case 0: o.value = "own" + i; break;
    case 1: Object.defineProperty(o, "value", { get() { calls++; return "got" + i + (this === o); }, configurable: true }); break;
    case 2: Object.defineProperty(o, "value", { get: function (a, b, c) { calls++; return "params" + i + [a, b, c].join() + arguments.length; }, configurable: true }); break;
    case 3: Object.defineProperty(o, "value", { get() { throw new Error("thrown" + i); }, configurable: true }); break;
    case 4: break;
    }
    o[symbol] = "sym" + i;
    shapes.push(o);
}
class Base { get value() { calls++; return "inherited " + this.tag; } }
class Derived extends Base { constructor(tag) { super(); this.tag = tag; } }
let inheriting = [new Derived(1), Object.assign(new Derived(2), { x: 1 }), Object.assign(new Derived(3), { y: 1 })];
let native = new Map([[1, 2]]);
let setterOnly = Object.defineProperty({}, "value", { set(v) { } });

function expected(o, i) {
    switch (i % 5) {
    case 0: return "own" + i;
    case 1: return "got" + i + "true";
    case 2: return "params" + i + ",,0";
    case 3: return "caught thrown" + i;
    default: return undefined;
    }
}
let key = ["val", "ue"].join(""); // Not an atom to begin with.
for (let round = 0; round < 300; ++round) {
    for (let i = 0; i < shapes.length; ++i) {
        check(atInTry(shapes[i], "value"), expected(shapes[i], i), "by val " + i);
        check(atInTry(shapes[i], key), expected(shapes[i], i), "by val, made up " + i);
        check(valueInTry(shapes[i]), expected(shapes[i], i), "by id " + i);
        check(at(shapes[i], symbol), "sym" + i, "symbol " + i);
        check(at(shapes[i], "nothing"), undefined, "absent " + i);
        check(at(shapes[i], Symbol.iterator), undefined, "absent symbol " + i);
    }
    for (let o of inheriting) {
        check(at(o, "value"), "inherited " + o.tag, "inherited getter, by val");
        check(value(o), "inherited " + o.tag, "inherited getter, by id");
    }
    check(at(native, "size"), 1, "native getter");
    check(at(setterOnly, "value"), undefined, "no getter");
    check(value(setterOnly), undefined, "no getter, by id");
    check(at([1, 2], "length"), 2, "length");
    check(at(at, "name"), "at", "name");

    if (round == 150) {
        // Everything changes under the caches.
        Object.defineProperty(shapes[1], "value", { get() { return "got1true"; }, configurable: true }); // Another getter, same answer.
        Object.defineProperty(Base.prototype, "value", { get() { return "inherited " + this.tag; }, configurable: true });
        delete shapes[4].pad0;
        Object.prototype.nothing = undefined;
    }
}
delete Object.prototype.nothing;
Object.defineProperty(shapes[0], "value", { get() { return "now a getter"; } });
check(at(shapes[0], "value"), "now a getter", "a value that became a getter");
check(value(shapes[0]), "now a getter", "a value that became a getter, by id");
Object.defineProperty(shapes[1], "value", { value: "now a value" });
check(at(shapes[1], "value"), "now a value", "a getter that became a value");
check(value(shapes[1]), "now a value", "a getter that became a value, by id");
delete shapes[6].value;
check(at(shapes[6], "value"), undefined, "a getter that went away");
Object.prototype.value = "from the prototype";
check(at(shapes[4], "value"), "from the prototype", "what was absent");
check(value(shapes[9]), "from the prototype", "what was absent, by id");
delete Object.prototype.value;
if (!calls)
    throw new Error("no getter was called");

// The stack is where it was after a getter that takes more than it is given.
function manyLocals(o) {
    let a = o.value, b = o["value"], c = o.value, d = o["value"];
    return [a, b, c, d].join("|");
}
noInline(manyLocals);
for (let i = 0; i < 300; ++i)
    check(manyLocals(shapes[2 + 5 * (i % 5)]), Array(4).fill("params" + (2 + 5 * (i % 5)) + ",,0").join("|"), "several in a row");

// ---- Characters of strings.
{
    let latin1 = "abc\xe9\xff", wide = "aĀ\xe9─b", rope = ["left ", "right"].map(String).reduce((a, b) => a + b + a.length);
    for (let i = 0; i < 300; ++i) {
        check(at(latin1, 0), "a", "first");
        check(at(latin1, 3), "\xe9", "latin1");
        check(at(latin1, 4), "\xff", "the last of the small ones");
        check(at(latin1, 5), undefined, "past the end of a string");
        check(at(latin1, -1), undefined, "before the start of a string");
        check(at(wide, 0), "a", "small, in a wide string");
        check(at(wide, 1), "Ā", "the first of the others");
        check(at(wide, 2), "\xe9", "small, in a wide string");
        check(at(wide, 3), "─", "wide");
        check(at(wide, 2.0 + half - half), "\xe9", "a double index into a string");
        check(at("", 0), undefined, "the empty string");
        check(at("x" + i, 1), String(i)[0], "a rope");
        check(at(rope, 5), "r", "a rope that has been read");
        check(at(latin1, "length"), 5, "length of a string");
        check(at(new String("boxed"), 1), "o", "a string object");
    }
    String.prototype[7] = "inherited";
    check(at(latin1, 7), "inherited", "past the end, with something to inherit");
    check(at(latin1, 1), "b", "and what is there is still there");
    delete String.prototype[7];
}
