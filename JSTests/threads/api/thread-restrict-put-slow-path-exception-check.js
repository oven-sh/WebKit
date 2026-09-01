//@ requireOptions("--useJSThreads=1", "--validateExceptionChecks=1")
// A JIT put-by-id slow path calls JSObject::putInlineSlow directly (bypassing
// putInlineForJSObject's gate) when the receiver carries accessor properties.
// On a thread-restricted receiver (an uncacheable dictionary) putInlineSlow's
// own thread-restrict gate throws ConcurrentAccessError; the throw must leave
// the function's ThrowScope checked (validateExceptionChecks aborts otherwise).
load("../resources/assert.js", "caller relative");

function put(obj, v) { obj.f = v; }
noInline(put);

// Warm the put site on an unrestricted receiver so the spawned thread runs
// compiled code whose put_by_id misses its cache on the dictionary receiver.
const warm = { f: 0 };
Object.defineProperty(warm, "g", { get() { return 2; }, set(v) { }, configurable: true });
for (let i = 0; i < 2e3; ++i)
    put(warm, i);

const o = { f: 1 };
Object.defineProperty(o, "g", { get() { return 2; }, set(v) { }, configurable: true });
Thread.restrict(o);

const failures = new Thread(() => {
    const out = [];
    for (let i = 0; i < 200; ++i) {
        try {
            put(o, 99);
            out.push("put did not throw");
            break;
        } catch (e) {
            if (!(e instanceof ConcurrentAccessError)) {
                out.push("threw " + e + " (not ConcurrentAccessError)");
                break;
            }
        }
    }
    return out;
}).join();
shouldBe(failures.length, 0, "foreign-thread put failures: " + failures.join("; "));
shouldBe(o.f, 1);
put(o, 5);
shouldBe(o.f, 5);
