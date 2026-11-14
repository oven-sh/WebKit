//@ runDefault("--useDollarVM=1")

// A store to an own index accessor calls the accessor's setter from
// JSObject::putByIndexBeyondVectorLengthWithArrayStorage(), which defers termination while it changes the array
// storage. It held that deferral across the call of the setter, so nothing could terminate the setter: with
// $vm.callWithTimeLimit(fn, ms) (node:vm's `timeout`), every store below that has to time out ran forever.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

function timesOut(fn, ms) {
    try {
        $vm.callWithTimeLimit(fn, ms);
    } catch (e) {
        if (e instanceof RangeError && e.message == "timed out")
            return true;
        throw e;
    }
    return false;
}

function endless() {
    return { set(value) { for (;;) { } }, configurable: true };
}

const stores = {
    "object[0] = value": () => { Object.defineProperty({ }, 0, endless())[0] = 1; },
    "object[0] = value in strict code": () => { "use strict"; Object.defineProperty({ }, 0, endless())[0] = 1; },
    "object['0'] = value": () => { Object.defineProperty({ }, 0, endless())["0"] = 1; },
    "object[100000] = value": () => { Object.defineProperty({ }, 100000, endless())[100000] = 1; },
    "Reflect.set(object, 0, value)": () => { Reflect.set(Object.defineProperty({ }, 0, endless()), 0, 1); },
    "Object.assign(object, [value])": () => { Object.assign(Object.defineProperty({ }, 0, endless()), [1]); },
    "Array.prototype.push.call(object, value)": () => { Array.prototype.push.call(Object.defineProperty({ length: 0 }, 0, endless()), 1); },
    "array[1] = value": () => { Object.defineProperty([1, 2, 3], 1, endless())[1] = 1; },
    "array.fill(value)": () => { Object.defineProperty([1, 2, 3], 1, endless()).fill(7); },
    "arguments[0] = value": () => { (function () { Object.defineProperty(arguments, 0, endless()); arguments[0] = 1; })(1); },
    "a setter of an object literal": () => { ({ set 0(value) { for (;;) { } } })[0] = 1; },
    "a static setter of a class": () => { (class { static set 0(value) { for (;;) { } } })[0] = 1; },
    // These two did time out: neither setter is called under the deferral.
    "object.name = value": () => { Object.defineProperty({ }, "name", endless()).name = 1; },
    "an index setter that the object inherits": () => { Object.create(Object.defineProperty({ }, 0, endless()))[0] = 1; },
};
for (const [what, store] of Object.entries(stores)) {
    shouldBe(timesOut(store, 2), true, what);
    shouldBe($vm.hasPendingTermination(), false, `${what}: something is still pending`);
}

// The same store from code that the JITs compiled.
{
    let spinning = false;
    const object = { set 0(value) { while (spinning) { } } };
    const store = function (value) { object[0] = value; };
    noInline(store);
    // Until the DFG has compiled the store. Where there is no DFG, numberOfDFGCompiles() is not zero.
    for (let i = 0; i < testLoopCount || (i < 100 * testLoopCount && !numberOfDFGCompiles(store)); i++)
        store(i);
    spinning = true;
    shouldBe(timesOut(() => store(1), 2), true, "a store that the JITs compiled");
    shouldBe($vm.hasPendingTermination(), false, "a store that the JITs compiled: something is still pending");
}

// A limit on a call that the setter makes cuts that call short inside the setter, and the store goes on within its
// own limit. The call ends by itself after five seconds: under the deferral it ran that long, and then
// VM::cancelTermination() asserted that no termination is deferred.
{
    let cutShort;
    let ranToItsEnd = false;
    const spin = () => {
        for (const end = preciseTime() + 5; preciseTime() < end;) { }
        ranToItsEnd = true;
    };
    noDFG(spin); // LLInt and Baseline code poll for the termination at each turn of a loop.
    const object = { set 0(value) { cutShort = timesOut(spin, 2); } };
    $vm.callWithTimeLimit(() => { object[0] = 1; }, 60000);
    shouldBe(cutShort, true, "a limit inside the setter");
    shouldBe(ranToItsEnd, false, "a call with a limit inside the setter ran to its end");
    shouldBe($vm.hasPendingTermination(), false, "a limit inside the setter: something is still pending");
}

// A store that begins with a termination under way, as in native code that goes on behind a call of its own that
// was terminated. The end of the deferral is then more than a decrement (VMTraps::undoDeferTerminationSlow()): it
// throws the TerminationException that was pending when the store began, or it asks for the trap again when the
// termination was only requested. Either way the body of the setter does not run. Under the deferral it ran.
for (const thrown of [true, false]) {
    const what = thrown ? "with a TerminationException pending" : "with a termination requested";
    let ran = 0;
    $vm.putByIndexDuringTermination({ set 0(value) { ran++; } }, 0, 1, thrown);
    shouldBe(ran, 0, `calls of an own index setter by a store that began ${what}`);

    // A value still goes into the map that is there, below a length that is written first.
    const array = [1];
    array[0x10000000] = 1;
    shouldBe($vm.putByIndexDuringTermination(array, 0x20000000, 2, thrown), true, `a store of a value that began ${what}`);
    shouldBe(array[0x20000000], 2, `the value of a store that began ${what}`);
    shouldBe(array.length, 0x20000001, `the length after a store that began ${what}`);
    shouldBe($vm.hasPendingTermination(), false, `a store that began ${what}: something is still pending`);
}

// No entry of a sparse map is at or beyond the length of its array, also in an array whose stores a limit cut
// short: a store writes the length before it adds the entry, and polls no trap in between. Upstream's bug 299499
// (https://bugs.webkit.org/show_bug.cgi?id=299499) was an entry that was added first. This holds with and without
// the deferral.
for (let round = 0; round < testLoopCount / 500; round++) {
    let array;
    const fill = () => {
        for (;;) {
            array = [1];
            if (round & 1) {
                array[0xfffffffe] = 1; // A new map.
                array[0xfffffff0] = 2; // The map that is there.
            } else {
                array[0x10000000] = 1; // A new map.
                array[0x20000000] = 2; // The map that is there, and a longer array.
            }
        }
    };
    noDFG(fill);
    timesOut(fill, 1);
    if (!array)
        continue;
    for (const key of Object.keys(array))
        shouldBe(+key < array.length, true, `index ${key} of an array of length ${array.length}`);
}
shouldBe($vm.hasPendingTermination(), false, "after the cut stores: something is still pending");
