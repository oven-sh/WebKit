//@ requireOptions("--immutableGlobals=Map, JSON ,parseInt,Intl,print,performance,noSuchGlobal,undefined,")

// An entry of --immutableGlobals other than * is a name looked up on the global object, whoever put it there: the object becomes
// immutable (JSObject::makePropertiesImmutable()), and so do the functions its properties hold and, for a function, its "prototype",
// and so on from each of those. A name that is not there, or whose value is not an object, is ignored.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function shouldThrow(func, errorType) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error("expected " + errorType.name + " but got " + String(error));
}

const isImmutable = $vm.hasImmutableProperties;

// A constructor, its prototype, and their methods, getters and setters, materialized by now or not.
shouldBe(isImmutable(Map), true);
shouldBe(isImmutable(Map.prototype), true);
shouldBe(isImmutable(Map.groupBy), true);
shouldBe(isImmutable(Map.prototype.get), true);
shouldBe(isImmutable(Map.prototype[Symbol.iterator]), true);
shouldBe(isImmutable(Object.getOwnPropertyDescriptor(Map.prototype, "size").get), true);
shouldBe(isImmutable(Object.getOwnPropertyDescriptor(Map, Symbol.species).get), true);
shouldThrow(() => { "use strict"; Map.prototype.get = 1; }, TypeError);
shouldThrow(() => { "use strict"; Map.prototype.set.call = 1; }, TypeError);
shouldBe(new Map([[1, 2]]).get(1), 2);

// A namespace object; a constructor it holds is a function it holds.
shouldBe(isImmutable(JSON), true);
shouldBe(isImmutable(JSON.parse), true);
shouldBe(isImmutable(Intl), true);
shouldBe(isImmutable(Intl.getCanonicalLocales), true);
shouldBe(isImmutable(Intl.Collator), true);
shouldBe(isImmutable(Intl.Collator.prototype), true);
shouldBe(isImmutable(Intl.Collator.prototype.resolvedOptions), true);
shouldBe(isImmutable(Intl.DateTimeFormat.prototype.formatToParts), true);

// A function.
shouldBe(isImmutable(parseInt), true);
shouldBe(Number.parseInt, parseInt);

// The embedder's.
shouldBe(isImmutable(print), true);
shouldBe(isImmutable(performance), true);
shouldBe(isImmutable(performance.now), true);

// Nothing else: not what a listed object inherits from, not an object that is not a function, not the global object.
for (let object of [Set, Set.prototype, Object, Object.prototype, Object.prototype.toString, Function.prototype, Function.prototype.call, Math, Math.max,
    parseFloat, isNaN, Number, Object.getPrototypeOf(new Map().entries()), debug, $, $.agent, globalThis])
    shouldBe(isImmutable(object), false);
Set.prototype.extra = 1;
Function.prototype.call.extra = 2;
globalThis.extra = 3;
shouldBe(new Set().extra + Map.call.extra + extra, 6);

// Each new global object looks the names up for itself.
{
    let other = $vm.createGlobalObject();
    shouldBe(isImmutable(other.Map.prototype.get), true);
    shouldBe(isImmutable(other.Set), false);
    shouldBe("print" in other, false);
    let shellGlobal = createGlobalObject();
    shouldBe(isImmutable(shellGlobal.print), true);
    shouldBe(isImmutable(shellGlobal.debug), false);
}
