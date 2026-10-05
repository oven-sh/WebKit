//@ runDefault("--useDollarVM=1", "--compileMainScriptAheadOfTime=1")
//@ runDefault("--useDollarVM=1", "--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--useDollarVM=1", "--compileMainScriptAheadOfTime=1", "--validateExceptionChecks=1")
//@ runDefault("--useDollarVM=1")
//@ runDefault("--useDollarVM=1", "--useJIT=0")
//@ runDefault("--useDollarVM=1", "--useJIT=0", "--validateExceptionChecks=1")

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const message = "Attempting to change the shape of an object whose shape is immutable.";
const structureOf = object => /Structure (0x[0-9a-f]+)/.exec(describe(object))[1];
const levels = [
    ["ordinary", source => Object.defineProperties({ }, Object.getOwnPropertyDescriptors(source))],
    ["stable fields", objectWithStableFields],
    ["immutable shape", objectWithImmutableShape],
];
class Returns { constructor(object) { return object; } }
const key = "computed";
const symbol = Symbol("s");
class ComputedField extends Returns { [key] = 1; }
class SymbolField extends Returns { [symbol] = 1; }
class IndexField extends Returns { 0 = 1; }
class ComputedIndexField extends Returns { [1 + 1] = 1; }
class ExistingField extends Returns { a = 7; }
class PrivateAccessor extends Returns { get #g() { return 1; } }
class PrivateStaticless extends Returns { #a = 1; #b = 2; }
class Base { }
class SuperStore extends Base { static store(receiver) { "use strict"; return Reflect.apply(function () { }, receiver, []); } }
const superStorer = { __proto__: { }, store() { "use strict"; super.added = 1; } };
const superStorerSloppy = { __proto__: { }, store() { super.added = 1; } };
const giving = object => function () { return object; };
const species = object => { const a = [1, 2, 3]; a.constructor = { [Symbol.species]: giving(object) }; return a; };
const roads = [
    ["a public field by a computed key", o => new ComputedField(o)],
    ["a public field by a symbol", o => new SymbolField(o)],
    ["a public field named by an index", o => new IndexField(o)],
    ["a public field by a computed index", o => new ComputedIndexField(o)],
    ["a public field that is there already", o => new ExistingField(o), "harmless"],
    ["a private accessor", o => new PrivateAccessor(o)],
    ["two private fields", o => new PrivateStaticless(o)],
    ["super.added = 1 with it as this, strict", o => superStorer.store.call(o)],
    ["super.added = 1 with it as this, sloppy", o => superStorerSloppy.store.call(o)],
    ["Reflect.set with it as the receiver", o => Reflect.set({ }, "added", 1, o)],
    ["Reflect.set of an index with it as the receiver", o => Reflect.set({ }, 0, 1, o)],
    ["Array.from.call", o => Array.from.call(giving(o), [1, 2])],
    ["Array.from.call of an iterable", o => Array.from.call(giving(o), new Set([1, 2]))],
    ["Array.of.call", o => Array.of.call(giving(o), 1, 2)],
    ["map with it as the species", o => species(o).map(x => x)],
    ["filter with it as the species", o => species(o).filter(x => x)],
    ["slice with it as the species", o => species(o).slice(0)],
    ["splice with it as the species", o => species(o).splice(0, 1)],
    ["concat with it as the species", o => species(o).concat([4])],
    ["flat with it as the species", o => species(o).flat()],
    ["flatMap with it as the species", o => species(o).flatMap(x => [x])],
    ["Array.prototype.push.call", o => Array.prototype.push.call(o, 1)],
    ["Array.prototype.unshift.call", o => Array.prototype.unshift.call(o, 1)],
    ["Array.prototype.pop.call", o => Array.prototype.pop.call(o)],
    ["Array.prototype.fill.call", o => Array.prototype.fill.call(o, 1), "harmless"],
    ["Array.prototype.copyWithin.call", o => Array.prototype.copyWithin.call(o, 0, 1), "harmless"],
    ["Array.prototype.reverse.call", o => Array.prototype.reverse.call(o), "harmless"],
    ["Array.prototype.sort.call", o => Array.prototype.sort.call(o), "harmless"],
    ["Object.defineProperties", o => Object.defineProperties(o, { added: { value: 1 }, more: { value: 2 } })],
    ["__defineGetter__ of a new name", o => o.__defineGetter__("added", () => 1)],
    ["__defineSetter__ of a new name", o => o.__defineSetter__("added", () => { })],
    ["__defineGetter__ of a field", o => o.__defineGetter__("a", () => 1), "harmful"],
    ["__defineSetter__ of a field", o => o.__defineSetter__("a", () => { }), "harmful"],
    ["a proxy of it: a new name", o => { "use strict"; new Proxy(o, { }).added = 1; }],
    ["a proxy of it: defineProperty", o => Object.defineProperty(new Proxy(o, { }), "added", { value: 1 })],
    ["a proxy of it: delete of a field", o => { "use strict"; delete new Proxy(o, { }).a; }, "harmful"],
    ["a proxy of it: preventExtensions", o => Object.preventExtensions(new Proxy(o, { }))],
    ["a proxy of it: setPrototypeOf", o => Object.setPrototypeOf(new Proxy(o, { }), { })],
    ["a proxy of it: freeze", o => Object.freeze(new Proxy(o, { })), "harmful, and preventExtensions comes first"],
    ["a proxy of it: seal", o => Object.seal(new Proxy(o, { }))],
    ["a proxy of it: an index", o => { "use strict"; new Proxy(o, { })[0] = 1; }],
    ["Error.captureStackTrace", o => Error.captureStackTrace(o)],
    ["Error.captureStackTrace with prepareStackTrace", o => { const old = Error.prepareStackTrace; Error.prepareStackTrace = () => "prepared"; try { Error.captureStackTrace(o); } finally { Error.prepareStackTrace = old; } }],
    ["lastIndex by RegExp.prototype[Symbol.matchAll]'s species", o => { const r = /x/g; r.constructor = { [Symbol.species]: giving(o) }; r[Symbol.matchAll]("x"); }],
    ["Object.assign of several names", o => Object.assign(o, { added: 1, more: 2 })],
    ["Object.assign of a symbol", o => Object.assign(o, { [symbol]: 1 })],
    ["Object.assign of an index", o => Object.assign(o, [1])],
    ["a destructuring target", o => { "use strict"; ({ x: o.added } = { x: 1 }); }],
    ["a rest target", o => { "use strict"; [...o.added] = [1]; }],
    ["for-in into it", o => { "use strict"; for (o.added in { k: 1 }); }],
    ["a logical assignment", o => { "use strict"; o.added ??= 1; }],
    ["an update of what is not there", o => { "use strict"; o.added++; }],
    ["with: an assignment", o => { Function("o", "with (o) { a = 5; }")(o); }, "harmless"],
    ["JSON.parse's reviver puts into it", o => JSON.parse('{"k":{"z":1}}', function (k, v) { if (k === "z") { this.added = 1; } return k === "k" ? o : v; }), "harmless"],
    ["Object.setPrototypeOf to what it has", o => Object.setPrototypeOf(o, Object.getPrototypeOf(o)), "harmless"],
    ["Object.fromEntries does not touch it", o => Object.fromEntries(Object.entries(o)), "harmless"],
    ["a WeakMap key, a WeakRef, a Set member", o => { new WeakMap().set(o, 1); new WeakRef(o); new Set().add(o); }, "harmless"],
    ["Symbol.toPrimitive and friends are read", o => { try { `${o}`; } catch { } o + 1; }, "harmless"],
];
const stateOf = object => JSON.stringify([
    Reflect.ownKeys(object).map(String),
    Object.entries(Object.getOwnPropertyDescriptors(object)).map(([name, d]) => [name, "value" in d, d.writable, d.enumerable, d.configurable]),
    Object.isExtensible(object),
    Object.getPrototypeOf(object) === Object.prototype,
]);
let refusals = 0;
let changesOfOrdinaryObjects = 0;
for (const [what, go, kind = ""] of roads) {
    for (let level = 0; level < levels.length; ++level) {
        const isRefused = level === 2 ? kind !== "harmless" : level === 1 && kind.startsWith("harmful");
        for (let i = 0; i < 30; ++i) {
            const object = levels[level][1]({ a: 1, b: 2 });
            const where = what + ", " + levels[level][0];
            const before = stateOf(object);
            const structure = structureOf(object);
            let outcome = "no exception";
            try {
                go(object);
            } catch (error) {
                outcome = error instanceof TypeError && error.message === message ? "refused" : String(error);
            }
            check(outcome === "refused", isRefused, where + ": is refused (" + outcome + ")");
            if (level === 2 || (isRefused && !kind.includes("comes first"))) {
                check(stateOf(object), before, where + ": it is what it was");
                check(structureOf(object), structure, where + ": with the Structure it had");
            }
            if (level)
                check(object.a + "," + object.b, kind === "harmless" && (what.startsWith("with") || what.includes("there already")) ? (what.startsWith("with") ? "5,2" : "7,2") : "1,2", where + ": the fields");
            refusals += isRefused;
            changesOfOrdinaryObjects += !level && stateOf(object) !== before;
        }
    }
}
check(refusals > 30 * 45, true, "most roads are refused at one level or the other: " + refusals);
check(changesOfOrdinaryObjects > 30 * 35, true, "and change an ordinary object: " + changesOfOrdinaryObjects);
