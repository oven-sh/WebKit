// Every way script is made from a string, as a function of this module's that says what it got.
// One realm: making a global object per call is most of what a loop of these would measure.
const realm = new ShadowRealm();
function attempt(make) { try { return make(); } catch (error) { return error.constructor.name + ": " + error.message; } }
export const routes = {
    "direct eval": () => attempt(() => eval("1 + 1")),
    "direct eval, in a nested function": () => attempt(() => (() => eval("1 + 1"))()),
    "indirect eval": () => attempt(() => (0, eval)("1 + 1")),
    "eval as a callback": () => attempt(() => ["1 + 1"].map(eval)[0]),
    "bound eval": () => attempt(() => eval.bind(null, "1 + 1")()),
    "Reflect.apply of eval": () => attempt(() => Reflect.apply(eval, undefined, ["1 + 1"])),
    "new Function": () => attempt(() => new Function("return 1 + 1")()),
    "Function()": () => attempt(() => Function("return 1 + 1")()),
    "Reflect.construct of Function": () => attempt(() => Reflect.construct(Function, ["return 1 + 1"])()),
    "a function's constructor": () => attempt(() => (() => { }).constructor("return 1 + 1")()),
    "GeneratorFunction": () => attempt(() => new (function* () { }).constructor("yield 1 + 1")().next().value),
    "AsyncFunction": () => attempt(() => typeof new (async function () { }).constructor("return 1 + 1")),
    "AsyncGeneratorFunction": () => attempt(() => typeof new (async function* () { }).constructor("yield 1 + 1")),
    "ShadowRealm.prototype.evaluate": () => attempt(() => realm.evaluate("1 + 1")),
    // Not script made from a string.
    "eval of a number": () => attempt(() => eval(2)),
    "a function expression": () => attempt(() => (function () { return 1 + 1; })()),
};
export function hot(n) { let refused = 0; for (let i = 0; i < n; ++i) { try { eval("i"); } catch { refused++; } } return refused; }
export async function afterAwait() { await null; return routes["new Function"](); }
export function callsBack(callback) { return callback(); }
