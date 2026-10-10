//@ skip if $architecture != "arm64"
//@ requireOptions("--compileMainScriptAheadOfTime=1")

function message(f) { try { f(); } catch (e) { return String(e); } return "no error"; }
function inInForIn(o, o2) { let c = 0; for (let p in o) { if (p in o2) ++c; } return c; }
function getInForIn(o, o2) { let c = 0; for (let p in o) c += o2[p]; return c; }
function putInForIn(o, o2) { for (let p in o) o2[p] = 1; }
class CallsInField { first = 1; second = this.missing(); }
class ReadsInField { first = this.nothing.x; }
class ConstructsInField { first = new this.missing(); }
function definesStaticField() { class HasStaticField { static first = HasStaticField.missing(); } }
function makesClass() { const outer = { }; return class { first = 1; second = outer.absent.deep; }; }
class CallsInArrowInField { first = () => this.missing(1, 2); }
class HasComputedField { ["computed"] = this.other.missing(); }
const cases = [
    () => Object.keys(null), () => Object.getPrototypeOf(undefined), () => Object.getOwnPropertyNames(null),
    () => Object.getOwnPropertyDescriptor(null, "a"), () => Reflect.ownKeys(1), () => new Map([1]), () => [].reduce((a, b) => a),
    () => inInForIn({ a: 1 }, null), () => getInForIn({ a: 1 }, null), () => putInForIn({ a: 1 }, undefined),
    () => JSON.parse("{"), () => "a".repeat(-1), () => new Array(-1), () => Symbol() + "",
    () => new CallsInField, () => new ReadsInField, () => new ConstructsInField, definesStaticField, () => new (makesClass()),
    () => new CallsInArrowInField().first(), () => new HasComputedField,
];
const expected = [
    "TypeError: null is not an object (evaluating 'Object.keys(null)')",
    "TypeError: undefined is not an object (evaluating 'Object.getPrototypeOf(undefined)')",
    "TypeError: null is not an object (evaluating 'Object.getOwnPropertyNames(null)')",
    "TypeError: null is not an object (evaluating 'Object.getOwnPropertyDescriptor(null, \"a\")')",
    "TypeError: Reflect.ownKeys requires the first argument be an object",
    "TypeError: Type error",
    "TypeError: reduce of empty array with no initial value",
    "TypeError: o2 is not an Object. (evaluating 'p in o2')",
    "TypeError: null is not an object (evaluating 'o2[p]')",
    "TypeError: undefined is not an object (evaluating 'o2[p] = 1')",
    "SyntaxError: JSON Parse error: Expected '}'",
    "RangeError: String.prototype.repeat argument must be greater than or equal to 0 and not be Infinity",
    "RangeError: Array length must be a positive integer of safe magnitude.",
    "TypeError: Cannot convert a symbol to a string",
    "TypeError: this.missing is not a function. (In 'this.missing()', 'this.missing' is undefined)",
    "TypeError: undefined is not an object (near '...(function () { })...')",
    "TypeError: undefined is not a constructor (evaluating 'new this.missing()')",
    "TypeError: HasStaticField.missing is not a function. (In 'HasStaticField.missing()', 'HasStaticField.missing' is undefined)",
    "TypeError: undefined is not an object (near '...(function () { })...')",
    "TypeError: this.missing is not a function. (In 'this.missing(1, 2)', 'this.missing' is undefined)",
    "TypeError: undefined is not an object (near '...(function () { })...')",
];
for (let i = 0; i < cases.length; i++) {
    if (message(cases[i]) !== expected[i])
        throw new Error(`case ${i}: expected ${expected[i]} but got ${message(cases[i])}`);
}
