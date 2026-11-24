//@ requireOptions("--useConcurrentJIT=false")

// A custom accessor is called with the realm of the object that holds it. The DFG and the FTL call one directly, with
// the realm of the code origin, and GetByStatus / PutByStatus allow that only when the two realms are the same. The
// inline cache the status comes from can live in the optimized code block of a function that inlined this code origin,
// and that function can be in another realm, so the realm of that block is not the realm of the code origin.
//
// The concurrent JIT is off so that every tier-up happens inside testLoopCount iterations. Each case returns the first
// iteration whose accessor did not see the holder's realm, or -1.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`bad value: ${message}: ${actual}, expected ${expected}`);
}

const guest = runString("");

// The cases for one accessor of $vm.createCustomTestGetterSetter(). They are compiled from source text that has the
// name of the accessor in it, so that the cases of one accessor share no code block and no profile with the cases of
// the other: a case whose caller is already optimized for the other accessor inlines nothing and cannot fail.
function casesFor(name) {
    return (0, eval)(`
    (function (guest) {
        const guestRead = guest.eval("(function (object) { return object.${name}; })");
        const guestWrite = guest.eval("(function (object, value) { object.${name} = value; })");

        // An instance that inherits the accessor from another instance: it passes the class check of a DOMAttribute
        // accessor, and the holder is the alternate base of the access case.
        function heirOf(holder) {
            const instance = $vm.createCustomTestGetterSetter();
            delete instance.${name};
            Object.setPrototypeOf(instance, holder);
            return instance;
        }

        return {
            // The holder, the reader and the caller are all in this realm. This is the case the direct call is for.
            "read in its own realm"() {
                const holder = $vm.createCustomTestGetterSetter();
                const read = object => object.${name};
                noInline(read);
                for (let i = 0; i < testLoopCount; ++i) {
                    if (read(holder) !== globalThis)
                        return i;
                }
                return -1;
            },

            // One level of inlining: the host function inlines the guest reader. Once the DFG code of the host has an
            // inline cache at that inlined code origin, the FTL compile of the host reads the status from it.
            "read by an inlined guest function"() {
                const holder = $vm.createCustomTestGetterSetter();
                const host = object => guestRead(object);
                noInline(host);
                for (let i = 0; i < testLoopCount; ++i) {
                    if (host(holder) !== globalThis)
                        return i;
                }
                return -1;
            },

            // Two levels: the middle function is in this realm and inlines the guest reader. It is compiled on its
            // own, so its optimized code block holds the inline cache, and the outer function reads the status from
            // there. This needs no FTL.
            "read through two levels of inlining"() {
                const holder = $vm.createCustomTestGetterSetter();
                const middle = object => guestRead(object);
                const warm = () => {
                    for (let i = 0; i < testLoopCount; ++i) {
                        if (middle(holder) !== globalThis)
                            return i;
                    }
                    return -1;
                };
                const outer = () => {
                    for (let i = 0; i < testLoopCount; ++i) {
                        if (middle(holder) !== globalThis)
                            return i;
                    }
                    return -1;
                };
                noInline(warm);
                noInline(outer);
                const first = warm();
                return first >= 0 ? first : outer();
            },

            // The holder is the prototype of the object the guest reads, so the realm comes from an alternate base.
            "read from the prototype by an inlined guest function"() {
                const object = heirOf($vm.createCustomTestGetterSetter());
                const host = object => guestRead(object);
                noInline(host);
                for (let i = 0; i < testLoopCount; ++i) {
                    if (host(object) !== globalThis)
                        return i;
                }
                return -1;
            },

            // The direction reversed: the holder is in the guest realm, and a guest function inlines a reader of this
            // realm.
            "read from a guest holder by an inlined function of this realm"() {
                const hostRead = function (object) { return object.${name}; };
                return guest.eval(\`
                    (hostRead => {
                        const holder = $vm.createCustomTestGetterSetter();
                        const caller = object => hostRead(object);
                        noInline(caller);
                        for (let i = 0; i < testLoopCount; ++i) {
                            if (caller(holder) !== globalThis)
                                return i;
                        }
                        return -1;
                    }) // ${name}
                \`)(hostRead);
            },

            // The setter twin: PutByStatus has the same guard, and CallCustomAccessorSetter passes the same realm.
            "written by an inlined guest function"() {
                const holder = $vm.createCustomTestGetterSetter();
                const host = (object, value) => guestWrite(object, value);
                noInline(host);
                for (let i = 0; i < testLoopCount; ++i) {
                    const value = {};
                    host(holder, value);
                    if (value.result !== globalThis)
                        return i;
                }
                return -1;
            },

            "written through the prototype by an inlined guest function"() {
                const object = heirOf($vm.createCustomTestGetterSetter());
                const host = (object, value) => guestWrite(object, value);
                noInline(host);
                for (let i = 0; i < testLoopCount; ++i) {
                    const value = {};
                    host(object, value);
                    if (value.result !== globalThis)
                        return i;
                }
                return -1;
            },

            "written to a guest holder by an inlined function of this realm"() {
                const hostWrite = function (object, value) { object.${name} = value; };
                return guest.eval(\`
                    (hostWrite => {
                        const holder = $vm.createCustomTestGetterSetter();
                        const caller = (object, value) => hostWrite(object, value);
                        noInline(caller);
                        for (let i = 0; i < testLoopCount; ++i) {
                            const value = {};
                            caller(holder, value);
                            if (value.result !== globalThis)
                                return i;
                        }
                        return -1;
                    }) // ${name}
                \`)(hostWrite);
            },
        };
    })`)(guest);
}

// customAccessorGlobalObject is a plain CustomAccessor. customDOMAttributeGlobalObject has a DOMAttributeAnnotation and
// no DOMJIT snippet, which the DFG and the FTL call through CallDOMGetter. Both getters return the realm they were
// called with, and both setters store it on their argument. A write of either one is a CallCustomAccessorSetter, so
// the written cases run for the first only.
for (const name of ["customAccessorGlobalObject", "customDOMAttributeGlobalObject"]) {
    const cases = casesFor(name);
    for (const label of Object.keys(cases)) {
        if (label.startsWith("written") && name !== "customAccessorGlobalObject")
            continue;
        shouldBe(cases[label](), -1, `${name} ${label}`);
    }
}
