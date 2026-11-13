//@ requireOptions("--useShadowRealm=1")

// A store to an array index of a global object ends in JSObject::putByIndex() on the JSGlobalObject, and putByIndex() takes
// no receiver. Script holds a global object only as its proxy, globalThis, so that is the receiver of the store: a setter
// gets it as `this`, and the set trap of a Proxy gets it as its receiver argument.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + String(expected) + " but got " + String(actual));
}

// The TypeError of a store is the one of the realm whose code stores.
function shouldThrowTypeError(global, run, message) {
    let thrown = null;
    try {
        run();
    } catch (error) {
        thrown = error;
    }
    if (!(thrown instanceof global.TypeError))
        throw new Error(message + ": expected a TypeError but got " + String(thrown));
}

const otherRealm = createGlobalObject();
const realms = [["the main realm", globalThis], ["another realm", otherRealm]];

// 1000000000 is beyond any vector, and 4294967294 is the largest array index.
const keys = [0, 7, 1000000000, 4294967294];

const stores = [
    ["global[key] = value", (global, key) => { global[key] = "value"; }],
    ["'use strict'; global[key] = value", (global, key) => { "use strict"; global[key] = "value"; }],
    ["global[String(key)] = value", (global, key) => { global[String(key)] = "value"; }],
    ["Reflect.set(global, key, value)", (global, key) => { shouldBe(Reflect.set(global, key, "value"), true, "Reflect.set"); }],
    ["Reflect.set(global, key, value, global)", (global, key) => { shouldBe(Reflect.set(global, key, "value", global), true, "Reflect.set"); }],
    ["Object.assign(global, { [key]: value })", (global, key) => { Object.assign(global, { [key]: "value" }); }],
    ["[global[key]] = [value]", (global, key) => { [global[key]] = ["value"]; }],
    ["for (global[key] of [value]);", (global, key) => { for (global[key] of ["value"]); }],
    ["this[key] = value in global code", (global, key) => { global.eval(`this[${key}] = "value";`); }],
];

// A sloppy function replaces a `this` that is no object with the global object of its own realm. So a sloppy setter
// of one realm on the global object of the other tells "no receiver" from the right receiver, as a strict one does.
const setterMakers = [
    ["a strict setter", realm => realm.Function("report", "return function (value) { 'use strict'; report(this, value); }")],
    ["a sloppy setter", realm => realm.Function("report", "return function (value) { report(this, value); }")],
];

// Defines a setter for `key` on `holder`, runs `store`, and says what the setter was called with.
function storeWithSetter(holder, key, makeSetter, store) {
    const seen = { calls: 0, receiver: undefined, value: undefined };
    Object.defineProperty(holder, key, {
        set: makeSetter((thisValue, value) => {
            seen.calls++;
            seen.receiver = thisValue;
            seen.value = value;
        }),
        configurable: true,
    });
    try {
        store();
    } finally {
        delete holder[key];
    }
    return seen;
}

function holdersOf(global) {
    return [["Object.prototype", global.Object.prototype], ["the global object itself", global]];
}

for (const [realmName, global] of realms) {
    for (const [holderName, holder] of holdersOf(global)) {
        for (const [setterName, makeSetter] of setterMakers) {
            for (const [setterRealmName, setterRealm] of realms) {
                for (const [storeName, store] of stores) {
                    for (const key of keys) {
                        const message = `${storeName}, key ${key}, global object of ${realmName}, ${setterName} of ${setterRealmName} on ${holderName}`;
                        const seen = storeWithSetter(holder, key, makeSetter(setterRealm), () => store(global, key));
                        shouldBe(seen.calls, 1, message + ": calls of the setter");
                        shouldBe(seen.receiver, global, message + ": this");
                        shouldBe(seen.value, "value", message + ": value");
                    }
                }
            }
        }
    }
}

// Functions of Array.prototype store to their `this`. Each of these stores "value" at index 0.
const mutators = [
    ["push", 0, global => { Array.prototype.push.call(global, "value"); }],
    ["unshift", 0, global => { Array.prototype.unshift.call(global, "value"); }],
    ["splice", 0, global => { Array.prototype.splice.call(global, 0, 0, "value"); }],
    ["fill", 1, global => { Array.prototype.fill.call(global, "value"); }],
    ["copyWithin", 2, global => { Array.prototype.copyWithin.call(global, 0, 1); }],
    ["reverse", 2, global => { Array.prototype.reverse.call(global); }],
];
const strictSetterOfMainRealm = setterMakers[0][1](globalThis);
for (const [realmName, global] of realms) {
    for (const [holderName, holder] of holdersOf(global)) {
        for (const [mutatorName, length, mutate] of mutators) {
            const message = `Array.prototype.${mutatorName} on the global object of ${realmName}, setter on ${holderName}`;
            global.length = length;
            if (length === 2)
                global[1] = "value";
            const seen = storeWithSetter(holder, 0, strictSetterOfMainRealm, () => mutate(global));
            delete global[1];
            shouldBe(delete global.length, true, message + ": delete of length");
            shouldBe(seen.calls, 1, message + ": calls of the setter");
            shouldBe(seen.receiver, global, message + ": this");
            shouldBe(seen.value, "value", message + ": value");
        }
    }
}

// A program that is a JSONP assignment is not compiled: the interpreter does the store itself.
for (const [realmName, global] of realms) {
    for (const [holderName, holder] of holdersOf(global)) {
        const message = `a JSONP program in ${realmName}, setter on ${holderName}`;
        const seen = storeWithSetter(holder, 5, strictSetterOfMainRealm, () => global.loadString(`globalThis[5] = {"jsonp": true}`));
        shouldBe(seen.calls, 1, message + ": calls of the setter");
        shouldBe(seen.receiver, global, message + ": this");
        shouldBe(seen.value.jsonp, true, message + ": value");
    }
}

// The same store from every tier.
{
    let receiver;
    Object.defineProperty(Object.prototype, 3, {
        set(value) {
            "use strict";
            receiver = this;
        },
        configurable: true,
    });
    function sloppyStore(global, value) {
        global[3] = value;
    }
    noInline(sloppyStore);
    function strictStore(global, value) {
        "use strict";
        global[3] = value;
    }
    noInline(strictStore);
    for (let i = 0; i < testLoopCount; i++) {
        receiver = null;
        sloppyStore(globalThis, i);
        shouldBe(receiver, globalThis, "sloppy store, iteration " + i);
        receiver = null;
        strictStore(globalThis, i);
        shouldBe(receiver, globalThis, "strict store, iteration " + i);
    }

    // Other receivers stay what they are: an object that is no global object, and a primitive.
    const object = {};
    receiver = null;
    object[3] = 1;
    shouldBe(receiver, object, "a store to a plain object");
    const array = [];
    receiver = null;
    array[3] = 1;
    shouldBe(receiver, array, "a store to an array");
    receiver = null;
    (function () {
        "use strict";
        try {
            (5)[3] = 1;
        } catch { }
    })();
    shouldBe(receiver, 5, "a store to a number");

    delete Object.prototype[3];
}

// What decides whether a setter is called does not change.
for (const [realmName, global] of realms) {
    const hasOwn = key => global.Object.prototype.hasOwnProperty.call(global, key);
    const strictStore = global.Function("key", "value", "'use strict'; globalThis[key] = value;");

    // No accessor: the store makes a data property of the global object.
    for (const key of keys) {
        const message = `key ${key} of the global object of ${realmName}`;
        global[key] = "data";
        const descriptor = Object.getOwnPropertyDescriptor(global, key);
        shouldBe(descriptor.value, "data", message + ": value");
        shouldBe(descriptor.writable && descriptor.enumerable && descriptor.configurable, true, message + ": attributes");
        shouldBe(global[key], "data", message + ": read");
        shouldBe(delete global[key], true, message + ": delete");
    }

    for (const [holderName, holder] of holdersOf(global)) {
        const message = `the global object of ${realmName}, ${holderName}`;

        // A setter that was deleted takes no store.
        let calls = 0;
        Object.defineProperty(holder, 9, { set(value) { calls++; }, configurable: true });
        shouldBe(delete holder[9], true, message + ": delete of the setter");
        global[9] = "data";
        shouldBe(calls, 0, message + ": calls of a deleted setter");
        shouldBe(hasOwn(9) && global[9], "data", message + ": store after a deleted setter");
        shouldBe(delete global[9], true, message + ": delete");

        // A getter without a setter, and a value that is read-only: sloppy code gives up, strict code throws.
        for (const [propertyName, property] of [["a getter without a setter", { get() { return "fixed"; }, configurable: true }], ["a read-only value", { value: "fixed", writable: false, configurable: true }]]) {
            Object.defineProperty(holder, 9, property);
            global[9] = "other";
            shouldBe(global[9], "fixed", `${message}, ${propertyName}: sloppy store`);
            shouldBe(Reflect.set(global, 9, "other"), false, `${message}, ${propertyName}: Reflect.set`);
            shouldThrowTypeError(global, () => strictStore(9, "other"), `${message}, ${propertyName}: strict store`);
            shouldBe(global[9], "fixed", `${message}, ${propertyName}: strict store`);
            shouldBe(delete holder[9], true, `${message}, ${propertyName}: delete`);
        }
    }

    // A data property of the global object is in front of a setter on its prototype chain.
    {
        const message = `an own data property of the global object of ${realmName} in front of a setter`;
        let calls = 0;
        global[9] = "own";
        Object.defineProperty(global.Object.prototype, 9, { set(value) { calls++; }, configurable: true });
        global[9] = "again";
        strictStore(9, "again");
        shouldBe(calls, 0, message + ": calls of the setter");
        shouldBe(global[9], "again", message + ": value");
        shouldBe(delete global.Object.prototype[9] && delete global[9], true, message + ": delete");
    }
}

// A global object that is not extensible takes no new index, and a setter on its prototype chain is still called.
{
    const global = createGlobalObject();
    const strictStore = global.Function("key", "value", "'use strict'; globalThis[key] = value;");
    shouldBe(Reflect.preventExtensions(global), true, "preventExtensions of a global object");
    global[9] = "data";
    shouldBe(9 in global, false, "a sloppy store to a global object that is not extensible");
    shouldThrowTypeError(global, () => strictStore(9, "data"), "a strict store to a global object that is not extensible");
    const seen = storeWithSetter(global.Object.prototype, 9, strictSetterOfMainRealm, () => strictStore(9, "value"));
    shouldBe(seen.calls, 1, "a global object that is not extensible: calls of the setter");
    shouldBe(seen.receiver, global, "a global object that is not extensible: this");
}

// A Proxy on the prototype chain of a global object. The global object of the shell has an immutable prototype, the
// one of a ShadowRealm has not.
{
    const realm = new ShadowRealm;
    const result = realm.evaluate(`
        (function () {
            const log = [];
            const name = value => value === globalThis ? "globalThis" : typeof value;
            const target = {};
            const withTrap = new Proxy(target, {
                set(target, key, value, receiver) {
                    log.push("set trap " + key + " " + name(receiver));
                    return true;
                },
            });
            Object.setPrototypeOf(globalThis, Object.create(withTrap));
            globalThis[0] = 1;
            globalThis[1000000000] = 1;
            Reflect.set(globalThis, 1, 1);
            (function () {
                "use strict";
                globalThis[2] = 1;
            })();
            log.push("own " + Object.getOwnPropertyNames(globalThis).filter(key => key === "0" || key === "1" || key === "2" || key === "1000000000").length);

            // Without a set trap the store goes on at the target, with the same receiver.
            Object.defineProperty(target, 3, {
                set(value) {
                    "use strict";
                    log.push("setter behind the proxy " + name(this));
                },
                configurable: true,
            });
            Object.setPrototypeOf(globalThis, new Proxy(target, {}));
            globalThis[3] = 1;
            globalThis[4] = "four";
            log.push("own 4 " + Object.hasOwn(globalThis, 4) + " " + Object.hasOwn(target, 4) + " " + globalThis[4]);
            return log.join(", ");
        })()
    `);
    shouldBe(result, "set trap 0 globalThis, set trap 1000000000 globalThis, set trap 1 globalThis, set trap 2 globalThis, own 0, setter behind the proxy globalThis, own 4 true false four", "a Proxy on the prototype chain of the global object of a ShadowRealm");
}
