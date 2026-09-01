//@ requireOptions("--useJSThreads=1", "--validateExceptionChecks=1")
// A named get or has on a receiver that overrides getOwnPropertySlot (an array,
// a function) walks the prototype chain in JSObject::getNonIndexPropertySlot.
// When a prototype on that chain is a thread-restricted object (an uncacheable
// dictionary) the walk's own thread-restrict gate throws ConcurrentAccessError
// on a foreign thread; the throw must leave the function's ThrowScope checked
// (validateExceptionChecks aborts otherwise).
load("../resources/assert.js", "caller relative");

const proto = { fromProto: "p" };
const arr = [1, 2, 3];
Object.setPrototypeOf(arr, proto);
function fn() { }
Object.setPrototypeOf(fn, proto);
Thread.restrict(proto);

// The owner reads through the chain unaffected.
shouldBe(arr.fromProto, "p");
shouldBe(arr.missing, undefined);
shouldBe("fromProto" in fn, true);
shouldBe("missing" in fn, false);

const failures = new Thread(() => {
    const out = [];
    function expectCAE(label, f) {
        try {
            f();
            out.push(label + ": did not throw");
        } catch (e) {
            if (!(e instanceof ConcurrentAccessError))
                out.push(label + ": threw " + e + " (not ConcurrentAccessError)");
        }
    }
    for (let i = 0; i < 20; ++i) {
        expectCAE("array get", () => arr.fromProto);
        expectCAE("array get (missing)", () => arr.missing);
        expectCAE("array has", () => "fromProto" in arr);
        expectCAE("function get", () => fn.fromProto);
        expectCAE("function has", () => "missing" in fn);
    }
    // Own properties of the receiver are found before the restricted
    // prototype is reached, so they stay readable from any thread.
    if (arr.length !== 3)
        out.push("arr.length: expected 3 but got " + arr.length);
    if (arr[0] !== 1)
        out.push("arr[0]: expected 1 but got " + arr[0]);
    return out;
}).join();
shouldBe(failures.length, 0, "foreign-thread failures: " + failures.join("; "));

// Owner still unaffected after the foreign attempts.
shouldBe(arr.fromProto, "p");
shouldBe(fn.fromProto, "p");
shouldBe("fromProto" in arr, true);
