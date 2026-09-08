//@ requireOptions("--useShadowRealm=1")

var abort = $vm.abort;

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${String(expected)} but got ${String(actual)}`);
}

const modulePath = "./resources/shadow-realm-module-that-throws.js";
const genericMessage = "Error encountered during evaluation";

// Import, into a fresh ShadowRealm, a module whose evaluation throws |thrown| (source text evaluated
// inside the realm). importValue must reject with a TypeError that belongs to this realm, must not
// run any of the thrown value's code to describe it, and must not let anything else out instead.
async function shouldRejectImportWith(thrown, expectedMessage) {
    let realm = new ShadowRealm();
    realm.evaluate(`globalThis.ran = 0; globalThis.valueToThrow = ${thrown}; undefined`);

    let error;
    let rejected = false;
    try {
        await realm.importValue(modulePath, "x");
    } catch (e) {
        rejected = true;
        error = e;
    }

    shouldBe(rejected, true, `${thrown}: rejected`);
    shouldBe(typeof error, "object", `${thrown}: typeof rejection`);
    shouldBe(Object.getPrototypeOf(error), TypeError.prototype, `${thrown}: rejection prototype`);
    shouldBe(error.constructor, TypeError, `${thrown}: rejection constructor`);
    shouldBe(error.message, expectedMessage, `${thrown}: message`);
    shouldBe(realm.evaluate(`globalThis.evaluations`), 1, `${thrown}: module evaluations`);
    shouldBe(realm.evaluate(`globalThis.ran`), 0, `${thrown}: code of the thrown value that ran`);
}

(async function () {
    // A Symbol has no ToString. It is described the way String(symbol) would.
    await shouldRejectImportWith(`Symbol("m")`, "Symbol(m)");
    await shouldRejectImportWith(`Symbol()`, "Symbol()");
    await shouldRejectImportWith(`Symbol.for("registered")`, "Symbol(registered)");
    await shouldRejectImportWith(`Symbol.asyncIterator`, "Symbol(Symbol.asyncIterator)");

    // Other primitives.
    await shouldRejectImportWith(`"a string"`, "a string");
    await shouldRejectImportWith(`""`, genericMessage);
    await shouldRejectImportWith(`42`, "42");
    await shouldRejectImportWith(`-0`, "0");
    await shouldRejectImportWith(`10n ** 30n`, "1000000000000000000000000000000");
    await shouldRejectImportWith(`undefined`, "undefined");
    await shouldRejectImportWith(`null`, "null");
    await shouldRejectImportWith(`true`, "true");

    // An object contributes its own "message" data property when that is a primitive other than a
    // Symbol, and nothing otherwise.
    await shouldRejectImportWith(`new RangeError("boom")`, "boom");
    await shouldRejectImportWith(`new Error("")`, genericMessage);
    await shouldRejectImportWith(`Object.assign(new Error("x"), { message: Symbol("m") })`, genericMessage);
    await shouldRejectImportWith(`{ message: "plain object" }`, "plain object");
    await shouldRejectImportWith(`{ message: 42 }`, "42");
    await shouldRejectImportWith(`{ message: Symbol("m") }`, genericMessage);
    await shouldRejectImportWith(`{}`, genericMessage);
    await shouldRejectImportWith(`[1, 2, 3]`, genericMessage);
    await shouldRejectImportWith(`function f() {}`, genericMessage);

    // None of the thrown value's code runs: no toString(), no Symbol.toPrimitive, no getter, no
    // proxy trap. Before, toString() ran, and when it threw an object that object (and through it
    // the whole shadow realm) escaped to the caller as the rejection value.
    await shouldRejectImportWith(`{ toString() { globalThis.ran++; return "toString ran"; } }`, genericMessage);
    await shouldRejectImportWith(`{ toString() { globalThis.ran++; throw Object.assign(new Error("toString threw"), { leaked: globalThis }); } }`, genericMessage);
    await shouldRejectImportWith(`{ [Symbol.toPrimitive]() { globalThis.ran++; return "toPrimitive ran"; } }`, genericMessage);
    await shouldRejectImportWith(`{ get message() { globalThis.ran++; return "getter ran"; } }`, genericMessage);
    await shouldRejectImportWith(`{ message: { toString() { globalThis.ran++; return "message toString ran"; } } }`, genericMessage);
    await shouldRejectImportWith(`Object.assign(new Error("x"), { toString() { globalThis.ran++; return "toString ran"; } })`, "x");
    await shouldRejectImportWith(`new Proxy(new Error("proxy"), { getOwnPropertyDescriptor() { globalThis.ran++; }, get() { globalThis.ran++; } })`, genericMessage);

    // A module that cannot be loaded at all rejects the same way. The message is the loader's.
    {
        let realm = new ShadowRealm();
        let error;
        try {
            await realm.importValue("./resources/there-is-no-such-module.js", "x");
        } catch (e) {
            error = e;
        }
        shouldBe(Object.getPrototypeOf(error), TypeError.prototype, `missing module: rejection prototype`);
    }

    // The export getter's own TypeError is from this realm too, and is left alone.
    {
        let realm = new ShadowRealm();
        let error;
        try {
            await realm.importValue("./resources/shadow-realm-example-module.js", "nothing");
        } catch (e) {
            error = e;
        }
        shouldBe(Object.getPrototypeOf(error), TypeError.prototype, `missing export: rejection prototype`);
        shouldBe(error.message, "%ShadowRealm%.importValue requires |exportName| to exist in the |specifier|", `missing export: message`);
    }

    // Many times over, against one realm whose module is already known to have failed.
    {
        let realm = new ShadowRealm();
        realm.evaluate(`globalThis.valueToThrow = Symbol("again"); undefined`);
        for (let i = 0; i < 1000; ++i) {
            let error;
            try {
                await realm.importValue(modulePath, "x");
            } catch (e) {
                error = e;
            }
            shouldBe(error.constructor, TypeError, `#${i}: rejection constructor`);
            shouldBe(error.message, "Symbol(again)", `#${i}: message`);
        }
        shouldBe(realm.evaluate(`globalThis.evaluations`), 1, `again: module evaluations`);
    }
}()).catch((error) => {
    print(String(error));
    print(String(error.stack));
    abort();
});
