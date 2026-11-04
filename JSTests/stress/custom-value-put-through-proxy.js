// A store whose receiver answers [[GetOwnProperty]] with the slot of another object, to a property of that object that has a native setter
// (PropertyAttribute::CustomValue): the setter gets the object that holds the property and that object's realm, as for an assignment to it.
// Two receivers answer so here: a Proxy without traps (the slot of its target), and $vm.createImpureGetter(delegate) (the slot of its
// delegate), which is not a Proxy.

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

// A receiver that is not a Proxy: $vm.createImpureGetter(holder). It has the [[DefineOwnProperty]] of an ordinary object, so a store
// asks it for its own slot only if a Proxy is part of the store: the object that the store starts at, or an object on the prototype
// chain of the receiver.
{
    // The setter of this static table entry puts a flag on the object it is given.
    let holder = $vm.createStaticCustomValue();
    let receiver = $vm.createImpureGetter(holder);
    shouldBe(Reflect.set(new Proxy({ }, { }), "testStaticValueSetFlag", 1, receiver), true);
    shouldBe(holder.testStaticValueSetterCalled, true, "the flag on the holder");
    shouldBe(Reflect.ownKeys(receiver).length, 0);
}

{
    let holder = $vm.createCustomTestGetterSetter();
    let receiver = $vm.createImpureGetter(holder);
    for (let i = 0; i < testLoopCount; i++) {
        shouldBe(Reflect.set(new Proxy({ }, { }), "customValue2", i, receiver), true);
        shouldBe(holder.customValue2, i);
    }
    shouldBe(Object.hasOwn(holder, "value2"), true);

    // The realm is the holder's, whichever realm the receiver is of.
    let result = { };
    shouldBe(Reflect.set(new Proxy({ }, { }), "customValueGlobalObject", result, receiver), true);
    shouldBe(result.result, globalThis);
    shouldBe(Reflect.set(new Proxy({ }, { }), "customValueGlobalObject", result, $vm.createImpureGetter(other.$vm.createCustomTestGetterSetter())), true);
    shouldBe(result.result, other.globalThis);
    shouldBe(Reflect.set(new Proxy({ }, { }), "customValueGlobalObject", result, other.$vm.createImpureGetter(holder)), true);
    shouldBe(result.result, globalThis);

    // An assignment to the receiver, with the Proxy on its prototype chain.
    Object.setPrototypeOf(receiver, new Proxy({ }, { }));
    for (let i = 0; i < testLoopCount; i++) {
        receiver.customValue2 = -i;
        shouldBe(holder.customValue2, -i);
    }
    shouldBe(Reflect.ownKeys(receiver).length, 0);
}

{
    // The setter does not run if the holder's properties are immutable. The receiver's [[DefineOwnProperty]] decides: it takes the property.
    let holder = $vm.createCustomTestGetterSetter();
    holder.customValue2 = 42;
    $vm.makePropertiesImmutable(holder);
    let receiver = $vm.createImpureGetter(holder);
    let result = { };
    shouldBe(Reflect.set(new Proxy({ }, { }), "customValue2", 43, receiver), true);
    shouldBe(Reflect.set(new Proxy({ }, { }), "customValueGlobalObject", result, receiver), true);
    shouldBe("result" in result, false);
    shouldBe(holder.customValue2, 42);
    shouldBe(Reflect.ownKeys(receiver).join(), "customValue2,customValueGlobalObject");
}
