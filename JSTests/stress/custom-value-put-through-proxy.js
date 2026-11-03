// A store whose receiver is a Proxy without traps, to a property of the Proxy's target that has a native setter (PropertyAttribute::CustomValue):
// the setter gets the object that holds the property and that object's realm, as for an assignment to the target.

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

const other = $vm.createGlobalObject();

for (let wrap of [target => new Proxy(target, { }), target => new Proxy(new Proxy(target, { }), { })]) {
    let target = $vm.createCustomTestGetterSetter();
    let proxy = wrap(target);
    for (let i = 0; i < testLoopCount; i++) {
        proxy.customValue2 = i;
        shouldBe(target.customValue2, i);
    }
    shouldBe(proxy.customValue2, testLoopCount - 1);
    shouldBe(Object.hasOwn(target, "value2"), true); // Where this setter keeps the value: on the object it is given.
    shouldBe(Reflect.set(proxy, "customValue2", 43), true);
    shouldBe(target.customValue2, 43);
    // The Proxy is only the receiver: the store starts at an object that does not have the property.
    shouldBe(Reflect.set({ }, "customValue2", 44, proxy), true);
    shouldBe(target.customValue2, 44);
    shouldBe(Reflect.set(target, "customValue2", 45, proxy), true);
    shouldBe(target.customValue2, 45);

    // The realm is the holder's, whichever realm the Proxy and the code that stores are of.
    let result = { };
    proxy.customValueGlobalObject = result;
    shouldBe(result.result, globalThis);
    wrap(other.$vm.createCustomTestGetterSetter()).customValueGlobalObject = result;
    shouldBe(result.result, other.globalThis);
    let otherProxy = new other.Proxy(target, { });
    otherProxy.customValueGlobalObject = result;
    shouldBe(result.result, globalThis);
    otherProxy.customValue2 = 46;
    shouldBe(target.customValue2, 46);
}

// The setter does not run if the target's properties are immutable.
for (let wrap of [target => new Proxy(target, { }), target => new Proxy(new Proxy(target, { }), { }), target => new Proxy(target, Reflect)]) {
    let target = $vm.createCustomTestGetterSetter();
    target.customValue2 = 42;
    $vm.makePropertiesImmutable(target);
    let proxy = wrap(target);
    let result = { };
    for (let i = 0; i < testLoopCount; i++) {
        proxy.customValue2 = 43;
        proxy.customValueGlobalObject = result;
    }
    shouldThrow(() => { "use strict"; proxy.customValue2 = 43; }, TypeError);
    shouldThrow(() => { "use strict"; proxy.customValueGlobalObject = result; }, TypeError);
    shouldBe(Reflect.set(proxy, "customValue2", 43), false);
    shouldBe(Reflect.set({ }, "customValue2", 43, proxy), false);
    shouldBe("result" in result, false);
    shouldBe(target.customValue2, 42);
}
