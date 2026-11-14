//@ requireOptions("--useShadowRealm=1")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

// A callable that a wrapped function returns is wrapped for the realm of that
// wrapped function, never for the realm of its target. A target that is not a
// plain JSFunction takes remoteFunctionCallGeneric, so check one target of each
// kind. https://tc39.es/proposal-shadowrealm/#sec-ordinarywrappedfunctioncall
const sources = [
    `() => () => 1`,                                 // JSFunction
    `(() => () => 1).bind(undefined)`,               // JSBoundFunction
    `Function`,                                      // InternalFunction
    `new Proxy(() => () => 1, {})`,                  // callable Proxy
    `new Proxy(() => {}, { apply: () => () => 1 })`, // Proxy with an apply trap
];

function wrapperComesFromThisRealm(wrapped) {
    return Object.getPrototypeOf(wrapped()) === Function.prototype;
}

for (const source of sources) {
    const wrapped = new ShadowRealm().evaluate(source);
    for (let i = 0; i < 200; i++)
        shouldBe(wrapperComesFromThisRealm(wrapped), true);
}

// The shadow realm's own Function constructor stays unreachable. The prototype
// of the returned wrapper is this realm's Function.prototype, so the
// constructor on it compiles code in this realm.
{
    const made = new ShadowRealm().evaluate(`Function`)("return 1");
    shouldBe(Object.getPrototypeOf(made), Function.prototype);
    const F = Object.getPrototypeOf(made).constructor;
    shouldBe(F, Function);
    shouldBe(F("return globalThis")(), globalThis);
}

// The rule holds in the other direction too: the shadow realm calls a wrapped
// function of this realm, and the callable that it returns is wrapped for the
// shadow realm.
{
    const realm = new ShadowRealm();
    const check = realm.evaluate(`(f) => Object.getPrototypeOf(f()) === Function.prototype`);
    shouldBe(check(() => () => 1), true);
    shouldBe(check(new Proxy(() => () => 1, {})), true);
    shouldBe(check(Function), true);
}
