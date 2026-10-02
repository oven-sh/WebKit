// A custom accessor is called with the realm of the object that holds it. The DFG and the FTL call one directly, with
// the realm of the code origin, and GetByStatus / PutByStatus allow that only when the two realms are the same. The
// inline cache the status comes from can live in the optimized code block of a function that inlined this code origin,
// and that function can be in another realm, so the realm of that block is not the realm of the code origin.
//
// Each case returns the first iteration whose accessor did not see the holder's realm, or -1.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`bad value: ${message}: ${actual}, expected ${expected}`);
}

// $vm.createCustomTestGetterSetter() holds these accessors. customAccessorGlobalObject is a plain CustomAccessor.
// customDOMAttributeGlobalObject has a DOMAttributeAnnotation and no DOMJIT snippet, which the DFG and the FTL call
// through CallDOMGetter. Both report the realm they were called with, and both setters store it on their argument.
const accessorNames = ["customAccessorGlobalObject", "customDOMAttributeGlobalObject"];

// A reader and a writer of each accessor, in a realm of their own.
const guest = runString(`
    var read = {};
    var write = {};
    for (var name of ["customAccessorGlobalObject", "customDOMAttributeGlobalObject"]) {
        read[name] = new Function("object", "return object." + name + ";");
        write[name] = new Function("object", "value", "object." + name + " = value;");
    }
`);

// One level of inlining: the host function inlines the guest reader. Once the host's DFG code has an inline cache at
// that inlined code origin, the FTL compile of the host reads the status from it.
function oneLevel(name, iterations) {
    const holder = $vm.createCustomTestGetterSetter();
    const guestRead = guest.read[name];
    const host = object => guestRead(object);
    noInline(host);
    for (let i = 0; i < iterations; ++i) {
        if (host(holder) !== globalThis)
            return i;
    }
    return -1;
}

// Two levels: the middle function is in the host realm and inlines the guest reader. It is compiled on its own, so its
// optimized code block holds the inline cache, and the outer function reads the status from there. This needs no FTL.
function twoLevels(name, iterations) {
    const holder = $vm.createCustomTestGetterSetter();
    const guestRead = guest.read[name];
    const middle = object => guestRead(object);
    const warm = count => {
        let bad = -1;
        for (let i = 0; i < count; ++i) {
            if (middle(holder) !== globalThis && bad < 0)
                bad = i;
        }
        return bad;
    };
    const outer = count => {
        let bad = -1;
        for (let i = 0; i < count; ++i) {
            if (middle(holder) !== globalThis && bad < 0)
                bad = i;
        }
        return bad;
    };
    noInline(warm);
    noInline(outer);
    const first = warm(iterations);
    const second = outer(iterations);
    return first >= 0 ? first : second;
}

// The holder is on the prototype chain of the object the guest reads, so the status comes from an alternate base.
function throughPrototype(name, iterations) {
    const holder = $vm.createCustomTestGetterSetter();
    const object = Object.create(holder);
    const guestRead = guest.read[name];
    const host = object => guestRead(object);
    noInline(host);
    for (let i = 0; i < iterations; ++i) {
        if (host(object) !== globalThis)
            return i;
    }
    return -1;
}

// The setter twin: PutByStatus has the same guard, and CallCustomAccessorSetter passes the same realm.
function oneLevelSetter(name, iterations) {
    const holder = $vm.createCustomTestGetterSetter();
    const guestWrite = guest.write[name];
    const host = (object, value) => guestWrite(object, value);
    noInline(host);
    for (let i = 0; i < iterations; ++i) {
        const value = {};
        host(holder, value);
        if (value.result !== globalThis)
            return i;
    }
    return -1;
}

// The same direction reversed: a guest-realm holder read by a host-realm function that the guest calls.
function guestHolder(name, iterations) {
    const guestRun = guest.eval(`
        const holder = $vm.createCustomTestGetterSetter();
        (host => {
            for (let i = 0; i < ${iterations}; ++i) {
                if (host(holder) !== globalThis)
                    return i;
            }
            return -1;
        });
    `);
    const guestRead = guest.read[name];
    return guestRun(object => guestRead(object));
}

// The engine first goes wrong well past 10000 iterations in the modes that leave the tier-up thresholds alone, so these
// counts are explicit rather than testLoopCount. A case stops at the first wrong value, so only a correct engine pays
// for every iteration.
const iterations = 300000;

for (const name of accessorNames) {
    // Same realm: the holder, the reader and the caller are all here. This is the case the direct call is for.
    const holder = $vm.createCustomTestGetterSetter();
    const read = object => object[name];
    noInline(read);
    for (let i = 0; i < iterations; ++i)
        shouldBe(read(holder), globalThis, `same realm ${name} at ${i}`);

    shouldBe(oneLevel(name, 2000000), -1, `${name} read by an inlined guest function`);
    shouldBe(twoLevels(name, iterations), -1, `${name} read through two levels of inlining`);
    shouldBe(oneLevelSetter(name, 2000000), -1, `${name} written by an inlined guest function`);
    shouldBe(guestHolder(name, iterations), -1, `${name} of a guest holder read by an inlined host function`);
}

// Only for the plain CustomAccessor: a DOMAttribute getter throws for a receiver that is not an instance of its class.
shouldBe(throughPrototype("customAccessorGlobalObject", 2000000), -1,
    "customAccessorGlobalObject read from the prototype by an inlined guest function");
