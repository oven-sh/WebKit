//@ runDefault("--compileMainScriptAheadOfTime=1")
function same(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function messageOf(f) {
    try {
        f();
    } catch (error) {
        return error.constructor.name + ": " + error.message;
    }
    return "did not throw";
}

function destructures(a) { const [x, y] = a; return String(x) + "," + String(y); }
function joins(a) { let s = ""; for (const x of a) s += String(x) + ";"; return s; }
function spreads(a) { return [...a].length; }
function destructuresInTry(a) { try { const [x, y] = a; return String(x) + "," + String(y); } catch (error) { return "caught " + error.constructor.name; } }
function all(a) { return destructures(a) + " " + joins(a) + " " + spreads(a) + " " + destructuresInTry(a); }

same(all([1, 2, 3]), "1,2 1;2;3; 3 1,2", "integers");
same(all([1.5, 2.5]), "1.5,2.5 1.5;2.5; 2 1.5,2.5", "doubles");
same(all(["a", {}]), "a,[object Object] a;[object Object]; 2 a,[object Object]", "anything");
same(all([]), "undefined,undefined  0 undefined,undefined", "empty");
same(all([1]), "1,undefined 1; 1 1,undefined", "shorter than the pattern");
same(all([, 2]), "undefined,2 undefined;2; 2 undefined,2", "a hole");
same(all(new Array(2)), "undefined,undefined undefined;undefined; 2 undefined,undefined", "only holes");
let sparse = [1, 2];
sparse[100000] = 3;
same(destructures(sparse), "1,2", "sparse");
let grown = [];
for (let i = 0; i < 3; i++)
    grown.push(i);
same(all(grown), "0,1 0;1;2; 3 0,1", "grown by push");
same(all(Object.freeze([1, 2])), "1,2 1;2; 2 1,2", "frozen");
same(all("a-b".split("-")), "a,b a;b; 2 a,b", "made by a builtin");
same(all(/(a)(b)/.exec("ab")), "ab,a ab;a;b; 3 ab,a", "a match");

class Reversed extends Array {
    *[Symbol.iterator]() {
        for (let i = this.length; i--;)
            yield this[i];
    }
}
same(all(Reversed.from([1, 2, 3])), "3,2 3;2;1; 3 3,2", "a subclass with its own iterator");
class Plain extends Array { }
same(all(Plain.from([1, 2, 3])), "1,2 1;2;3; 3 1,2", "a subclass without one");

let withOwn = [1, 2, 3];
withOwn[Symbol.iterator] = function* () { yield "own"; };
same(all(withOwn), "own,undefined own; 1 own,undefined", "an array with its own iterator method");
let withNamed = [1, 2];
withNamed.extra = true;
same(all(withNamed), "1,2 1;2; 2 1,2", "an array with another property");
let otherPrototype = [1, 2];
Object.setPrototypeOf(otherPrototype, { *[Symbol.iterator]() { yield "inherited"; } });
same(all(otherPrototype), "inherited,undefined inherited; 1 inherited,undefined", "an array with another prototype");
let noPrototype = [1, 2];
Object.setPrototypeOf(noPrototype, null);
same(messageOf(() => destructures(noPrototype)).startsWith("TypeError"), true, "an array without a prototype");
same(destructuresInTry(noPrototype), "caught TypeError", "an array without a prototype, caught");
let foreign = createGlobalObject().Array.of(1, 2, 3);
same(all(foreign), "1,2 1;2;3; 3 1,2", "an array of another realm");

same(all("xyz"), "x,y x;y;z; 3 x,y", "a string");
same(all(new Set([1, 2])), "1,2 1;2; 2 1,2", "a set");
same(all(new Map([[1, 2]])), "1,2,undefined 1,2; 1 1,2,undefined", "a map");
same(all(new Uint8Array([1, 2])), "1,2 1;2; 2 1,2", "a typed array");
same(all((function () { return arguments; })(1, 2)), "1,2 1;2; 2 1,2", "an arguments object");
same(all({ *[Symbol.iterator]() { yield 1; yield 2; } }), "1,2 1;2; 2 1,2", "an object with a generator method");

let reads = 0;
let counted = { get [Symbol.iterator]() { reads++; return function* () { yield 1; }; } };
destructures(counted);
same(reads, 1, "the getter runs once for a pattern");
joins(counted);
same(reads, 2, "the getter runs once for a loop");
destructuresInTry(counted);
same(reads, 3, "the getter runs once in a try");

for (let [value, message] of [[undefined, "TypeError: undefined is not an object (evaluating '[x, y]')"], [null, "TypeError: null is not an object (evaluating '[x, y]')"],
    [5, "TypeError: number is not iterable"], [{ }, "TypeError: {} is not iterable"]])
    same(messageOf(() => destructures(value)), message, "what is thrown for " + String(value));
same(destructuresInTry(undefined), "caught TypeError", "undefined, caught");
same(messageOf(() => joins(undefined)).startsWith("TypeError: undefined is not"), true, "a loop over undefined");

function overString() { let s = ""; for (const c of "ab") s += c; return s; }
function overNewMap() { let m = new Map([[1, 2]]); let n = 0; for (const entry of m) n += entry[1]; return n; }
same(overString(), "ab", "a loop over a literal string");
same(overNewMap(), 2, "a loop over a new map");

if (aotRemarks("destructures")) {
    let has = (name, remark) => aotRemarks(name).includes(remark);
    let expect = (name, remark, applies) => {
        if (has(name, remark) !== applies)
            throw new Error(remark + (applies ? " does not apply to " : " applies to ") + name + ": " + aotRemarks(name).join(" "));
    };
    expect("destructures", "sunk-iterator-method-read", true);
    expect("destructures", "reads-iterator-method-of-array-inline", false);
    expect("joins", "sunk-iterator-method-read", true);
    expect("joins", "reads-iterator-method-of-array-inline", false);
    expect("destructuresInTry", "sunk-iterator-method-read", true);
    for (let name of ["overString", "overNewMap"]) {
        expect(name, "sunk-iterator-method-read", false);
        expect(name, "reads-iterator-method-of-array-inline", false);
    }
}
