// A store whose receiver answers [[GetOwnProperty]] with the slot of another object, where that slot is a custom value
// with a setter. The setter belongs to the object that holds the property, so the store does not call it with the
// receiver. It ends in [[DefineOwnProperty]] on the receiver, as it does where the other object has a plain value.
//
// $vm.createImpureGetter(delegate) is such a receiver, and no proxy. It has the [[DefineOwnProperty]] of an ordinary
// object, so it takes the property itself. A store asks it for its own slot only if a proxy is part of the store: here
// the object that the store starts at.
//
// The setters of customValue, customValue2 and customValueGlobalObject assert that they get the object that holds them.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": got " + String(actual) + ", expected " + String(expected));
}

function store(key, value, receiver) {
    return Reflect.set(new Proxy({}, {}), key, value, receiver);
}
noInline(store);

function test() {
    {
        const holder = $vm.createCustomTestGetterSetter();
        const receiver = $vm.createImpureGetter(holder);
        const value = {};
        shouldBe(store("customValue2", value, receiver), true, "customValue2");
        shouldBe(store("customValue", value, receiver), true, "customValue");
        shouldBe(store("customValueGlobalObject", value, receiver), true, "customValueGlobalObject");
        shouldBe(Object.hasOwn(holder, "value2"), false, "the setter of customValue2 was not called");
        shouldBe(Object.hasOwn(value, "result"), false, "the other two setters were not called");
        shouldBe(Reflect.ownKeys(receiver).join(), "customValue2,customValue,customValueGlobalObject", "what the receiver took");

        // The holder's properties are what they were: a store to the holder reaches the setter.
        holder.customValue2 = value;
        shouldBe(holder.value2, value, "a store to the holder afterwards");
    }

    {
        // The same for a custom value of a static table, whose setter puts a flag on the object it gets.
        const holder = $vm.createStaticCustomValue();
        const receiver = $vm.createImpureGetter(holder);
        shouldBe(store("testStaticValueSetFlag", 1, receiver), true, "testStaticValueSetFlag");
        shouldBe(holder.testStaticValueSetterCalled, undefined, "the flag on the holder");
        shouldBe(Reflect.ownKeys(receiver).join(), "testStaticValueSetFlag", "what the receiver took");
    }

    {
        // ... and for an assignment to the receiver, with the proxy on its prototype chain.
        const holder = $vm.createCustomTestGetterSetter();
        const receiver = $vm.createImpureGetter(holder);
        Object.setPrototypeOf(receiver, new Proxy({}, {}));
        receiver.customValue2 = {};
        shouldBe(Object.hasOwn(holder, "value2"), false, "an assignment to the receiver");
        shouldBe(Reflect.ownKeys(receiver).join(), "customValue2", "what the receiver took by an assignment");
    }

    {
        // What such a store does where the other object has a plain value.
        const holder = { plain: 1 };
        const receiver = $vm.createImpureGetter(holder);
        shouldBe(store("plain", 2, receiver), true, "plain");
        shouldBe(holder.plain, 1, "the plain value of the holder");
        shouldBe(Reflect.ownKeys(receiver).join(), "plain", "what the receiver took of a plain value");
    }

    {
        // A receiver that holds the custom value itself: the setter stands in for the store, with the receiver.
        const holder = $vm.createCustomTestGetterSetter();
        const value = {};
        shouldBe(store("customValue2", value, holder), true, "customValue2 with the holder as the receiver");
        shouldBe(holder.value2, value, "what the setter got");
        shouldBe(store("customValue", value, holder), true, "customValue with the holder as the receiver");
        shouldBe(value.result, holder, "the object that the setter of customValue got");

        const staticHolder = $vm.createStaticCustomValue();
        shouldBe(store("testStaticValueSetFlag", 1, staticHolder), true, "testStaticValueSetFlag with the holder as the receiver");
        shouldBe(staticHolder.testStaticValueSetterCalled, true, "the flag on the holder");
    }
}

for (let i = 0; i < testLoopCount / 100; i++)
    test();
