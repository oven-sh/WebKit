// Bun: a definition that gives a value to a custom value with a setter, and changes no attribute, hands the value to
// the setter, and the property stays what it is. A store through a proxy ends in such a definition. Upstream puts the
// value in the place of the custom value (reflect-set-proxy-receiver-custom-value.js).
//
// $vm.createCustomTestGetterSetter() has customValue2, whose setter stores what it gets under value2 and whose getter
// reads value2, and customValue, whose setter stores its holder under `result` of the object it gets. Both setters
// assert that they are called with the holder.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": got " + String(actual) + ", expected " + String(expected));
}

function warmUp(proxy) {
    Reflect.getOwnPropertyDescriptor(proxy, "warmUp");
    Reflect.defineProperty(proxy, "warmUp", { value: 0, writable: true, enumerable: true, configurable: true });
}

function isStillCustomValue(object) {
    const probe = {};
    object.customValue2 = probe;
    return object.value2 === probe;
}

const storesThatReachTheSetter = {
    "Object.defineProperty with a value"(object, value) {
        Object.defineProperty(object, "customValue2", { value });
    },
    "Object.defineProperty that repeats the attributes"(object, value) {
        Object.defineProperty(object, "customValue2", { value, writable: true, enumerable: true, configurable: true });
    },
    "Object.defineProperties"(object, value) {
        Object.defineProperties(object, { customValue2: { value } });
    },
    "Reflect.defineProperty"(object, value) {
        shouldBe(Reflect.defineProperty(object, "customValue2", { value }), true, "Reflect.defineProperty");
    },
    "a store through a proxy"(object, value) {
        new Proxy(object, {}).customValue2 = value;
    },
    "a store through a proxy that has answered before"(object, value) {
        const proxy = new Proxy(object, {});
        warmUp(proxy);
        proxy.customValue2 = value;
    },
    "a store through a proxy of a proxy"(object, value) {
        new Proxy(new Proxy(object, {}), {}).customValue2 = value;
    },
    "a store through a revocable proxy"(object, value) {
        Proxy.revocable(object, {}).proxy.customValue2 = value;
    },
    "a store in strict code through a proxy"(object, value) {
        "use strict";
        new Proxy(object, {}).customValue2 = value;
    },
    "Reflect.set with a proxy as the receiver"(object, value) {
        shouldBe(Reflect.set(object, "customValue2", value, new Proxy(object, {})), true, "Reflect.set");
    },
    "Reflect.set on another object with a proxy as the receiver"(object, value) {
        shouldBe(Reflect.set({}, "customValue2", value, new Proxy(object, {})), true, "Reflect.set");
    },
    "Object.assign onto a proxy"(object, value) {
        Object.assign(new Proxy(object, {}), { customValue2: value });
    },
    "a set trap that hands the receiver on"(object, value) {
        new Proxy(object, {
            set(target, key, newValue, receiver) {
                return Reflect.set(target, key, newValue, receiver);
            },
        }).customValue2 = value;
    },
    "a defineProperty trap that hands the definition on"(object, value) {
        let calls = 0;
        new Proxy(object, {
            defineProperty(target, key, descriptor) {
                calls++;
                return Reflect.defineProperty(target, key, descriptor);
            },
        }).customValue2 = value;
        shouldBe(calls, 1, "calls of the defineProperty trap");
    },
    "a getOwnPropertyDescriptor trap that hands the question on"(object, value) {
        new Proxy(object, {
            getOwnPropertyDescriptor(target, key) {
                return Reflect.getOwnPropertyDescriptor(target, key);
            },
        }).customValue2 = value;
    },
};

const definitionsThatReplaceTheProperty = {
    "a definition that changes an attribute"(object, value) {
        Object.defineProperty(object, "customValue2", { value, enumerable: false });
    },
    "a definition that makes the property read-only"(object, value) {
        Object.defineProperty(object, "customValue2", { value, writable: false });
    },
    "a definition of an accessor"(object, value) {
        Object.defineProperty(object, "customValue2", { get() { return value; }, configurable: true });
    },
    "delete, then a store"(object, value) {
        delete object.customValue2;
        object.customValue2 = value;
    },
};

function test() {
    for (const [what, store] of Object.entries(storesThatReachTheSetter)) {
        const object = $vm.createCustomTestGetterSetter();
        const value = {};
        store(object, value);
        shouldBe(object.value2, value, what + ", what the setter got");
        shouldBe(object.customValue2, value, what + ", what the getter reads");
        shouldBe(Object.hasOwn(object, "customValue2"), true, what + ", the property");
        shouldBe(isStillCustomValue(object), true, what + ", a store afterwards reaches the setter");
    }

    for (const [what, define] of Object.entries(definitionsThatReplaceTheProperty)) {
        const object = $vm.createCustomTestGetterSetter();
        const value = {};
        define(object, value);
        shouldBe(object.value2, undefined, what + ", the setter was not called");
        shouldBe(object.customValue2, value, what + ", the value");
        shouldBe(isStillCustomValue(object), false, what + ", a store afterwards reaches no setter");
    }

    {
        // The setter gets the holder, and refuses what is no object: the definition fails and changes nothing.
        const object = $vm.createCustomTestGetterSetter();
        const value = {};
        Object.defineProperty(object, "customValue", { value });
        shouldBe(value.result, object, "the holder that the setter of customValue got");
        shouldBe(Reflect.defineProperty(object, "customValue", { value: 1 }), false, "a value that the setter refuses");
        shouldBe(object.customValue, object, "customValue after a refused value");
    }

    {
        // A definition of the value that the property has changes nothing, and calls nothing.
        const object = $vm.createCustomTestGetterSetter();
        Object.defineProperty(object, "customValue", { value: object });
        shouldBe(Object.hasOwn(object, "result"), false, "a definition of the same value");
    }

    {
        // A custom value without a setter has nothing to hand the value to.
        const object = $vm.createCustomTestGetterSetter();
        Object.defineProperty(object, "customValueNoSetter", { value: 1 });
        shouldBe(object.customValueNoSetter, 1, "customValueNoSetter");
    }
}

for (let i = 0; i < testLoopCount; i++)
    test();

{
    // The setter is called in the realm of the holder, whichever realm defines.
    const other = createGlobalObject();
    const object = other.$vm.createCustomTestGetterSetter();
    const value = {};
    Object.defineProperty(object, "customValueGlobalObject", { value });
    shouldBe(value.result, other, "the realm that the setter of customValueGlobalObject got");
    const throughProxy = {};
    new Proxy(object, {}).customValueGlobalObject = throughProxy;
    shouldBe(throughProxy.result, other, "the realm through a proxy");
}
