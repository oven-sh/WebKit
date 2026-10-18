//@ requireOptions("--useDollarVM=true")

// The six URI functions, for 8-bit and 16-bit strings, against the results of V8 (node 26). The
// functions return the argument itself when no code unit changes, so each input is tried as an
// 8-bit string and as a 16-bit string with the same code units.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

function shouldThrow(func, errorType, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!error)
        throw new Error(message + ": did not throw");
    if (!(error instanceof errorType))
        throw new Error(message + ": expected " + errorType.name + " but got " + error);
}

function sixteenBit(string) {
    return $vm.make16BitStringIfPossible(string);
}

function is8Bit(string) {
    return $vm.value(string).includes("8Bit:(1)");
}

const expectations = {
    encodeURIComponent: [
        ["", ""],
        ["a", "a"],
        ["photos", "photos"],
        ["my-bucket-2026-10-16-cat.jpg", "my-bucket-2026-10-16-cat.jpg"],
        ["my file (1) \u00e9+copy&x=1.jpg", "my%20file%20(1)%20%C3%A9%2Bcopy%26x%3D1.jpg"],
        ["!'()*-._~", "!'()*-._~"],
        ["!#$&'()*+,-./:;=?@_~", "!%23%24%26'()*%2B%2C-.%2F%3A%3B%3D%3F%40_~"],
        ["%", "%25"],
        ["100%", "100%25"],
        ["%41", "%2541"],
        ["%zz", "%25zz"],
        ["%u0041", "%25u0041"],
        ["%u00e9", "%25u00e9"],
        ["%E2%82%AC", "%25E2%2582%25AC"],
        ["%F0%9F%98%80", "%25F0%259F%2598%2580"],
        ["\u00ff", "%C3%BF"],
        ["\u00e9t\u00e9", "%C3%A9t%C3%A9"],
        ["\u20ac", "%E2%82%AC"],
        ["\ud83d\ude00", "%F0%9F%98%80"],
        ["a\ud83d\ude00b", "a%F0%9F%98%80b"],
        ["\u0100", "%C4%80"],
        ["@*+-./_", "%40*%2B-.%2F_"],
        ["<>", "%3C%3E"],
        ["\ud800x", URIError],
        ["\udc00", URIError],
        ["x\ud83d", URIError],
    ],
    encodeURI: [
        ["", ""],
        ["a", "a"],
        ["photos", "photos"],
        ["my-bucket-2026-10-16-cat.jpg", "my-bucket-2026-10-16-cat.jpg"],
        ["my file (1) \u00e9+copy&x=1.jpg", "my%20file%20(1)%20%C3%A9+copy&x=1.jpg"],
        ["!'()*-._~", "!'()*-._~"],
        ["!#$&'()*+,-./:;=?@_~", "!#$&'()*+,-./:;=?@_~"],
        ["%", "%25"],
        ["100%", "100%25"],
        ["%41", "%2541"],
        ["%zz", "%25zz"],
        ["%u0041", "%25u0041"],
        ["%u00e9", "%25u00e9"],
        ["%E2%82%AC", "%25E2%2582%25AC"],
        ["%F0%9F%98%80", "%25F0%259F%2598%2580"],
        ["\u00ff", "%C3%BF"],
        ["\u00e9t\u00e9", "%C3%A9t%C3%A9"],
        ["\u20ac", "%E2%82%AC"],
        ["\ud83d\ude00", "%F0%9F%98%80"],
        ["a\ud83d\ude00b", "a%F0%9F%98%80b"],
        ["\u0100", "%C4%80"],
        ["@*+-./_", "@*+-./_"],
        ["<>", "%3C%3E"],
        ["\ud800x", URIError],
        ["\udc00", URIError],
        ["x\ud83d", URIError],
    ],
    escape: [
        ["", ""],
        ["a", "a"],
        ["photos", "photos"],
        ["my-bucket-2026-10-16-cat.jpg", "my-bucket-2026-10-16-cat.jpg"],
        ["my file (1) \u00e9+copy&x=1.jpg", "my%20file%20%281%29%20%E9+copy%26x%3D1.jpg"],
        ["!'()*-._~", "%21%27%28%29*-._%7E"],
        ["!#$&'()*+,-./:;=?@_~", "%21%23%24%26%27%28%29*+%2C-./%3A%3B%3D%3F@_%7E"],
        ["%", "%25"],
        ["100%", "100%25"],
        ["%41", "%2541"],
        ["%zz", "%25zz"],
        ["%u0041", "%25u0041"],
        ["%u00e9", "%25u00e9"],
        ["%E2%82%AC", "%25E2%2582%25AC"],
        ["%F0%9F%98%80", "%25F0%259F%2598%2580"],
        ["\u00ff", "%FF"],
        ["\u00e9t\u00e9", "%E9t%E9"],
        ["\u20ac", "%u20AC"],
        ["\ud83d\ude00", "%uD83D%uDE00"],
        ["a\ud83d\ude00b", "a%uD83D%uDE00b"],
        ["\u0100", "%u0100"],
        ["@*+-./_", "@*+-./_"],
        ["<>", "%3C%3E"],
        ["\ud800x", "%uD800x"],
        ["\udc00", "%uDC00"],
        ["x\ud83d", "x%uD83D"],
    ],
    decodeURIComponent: [
        ["", ""],
        ["a", "a"],
        ["photos", "photos"],
        ["my-bucket-2026-10-16-cat.jpg", "my-bucket-2026-10-16-cat.jpg"],
        ["my file (1) \u00e9+copy&x=1.jpg", "my file (1) \u00e9+copy&x=1.jpg"],
        ["!'()*-._~", "!'()*-._~"],
        ["!#$&'()*+,-./:;=?@_~", "!#$&'()*+,-./:;=?@_~"],
        ["%", URIError],
        ["100%", URIError],
        ["%41", "A"],
        ["%zz", URIError],
        ["%u0041", URIError],
        ["%u00e9", URIError],
        ["%E2%82%AC", "\u20ac"],
        ["%F0%9F%98%80", "\ud83d\ude00"],
        ["\u00ff", "\u00ff"],
        ["\u00e9t\u00e9", "\u00e9t\u00e9"],
        ["\u20ac", "\u20ac"],
        ["\ud83d\ude00", "\ud83d\ude00"],
        ["a\ud83d\ude00b", "a\ud83d\ude00b"],
        ["\u0100", "\u0100"],
        ["@*+-./_", "@*+-./_"],
        ["<>", "<>"],
        ["\ud800x", "\ud800x"],
        ["\udc00", "\udc00"],
        ["x\ud83d", "x\ud83d"],
    ],
    decodeURI: [
        ["", ""],
        ["a", "a"],
        ["photos", "photos"],
        ["my-bucket-2026-10-16-cat.jpg", "my-bucket-2026-10-16-cat.jpg"],
        ["my file (1) \u00e9+copy&x=1.jpg", "my file (1) \u00e9+copy&x=1.jpg"],
        ["!'()*-._~", "!'()*-._~"],
        ["!#$&'()*+,-./:;=?@_~", "!#$&'()*+,-./:;=?@_~"],
        ["%", URIError],
        ["100%", URIError],
        ["%41", "A"],
        ["%zz", URIError],
        ["%u0041", URIError],
        ["%u00e9", URIError],
        ["%E2%82%AC", "\u20ac"],
        ["%F0%9F%98%80", "\ud83d\ude00"],
        ["\u00ff", "\u00ff"],
        ["\u00e9t\u00e9", "\u00e9t\u00e9"],
        ["\u20ac", "\u20ac"],
        ["\ud83d\ude00", "\ud83d\ude00"],
        ["a\ud83d\ude00b", "a\ud83d\ude00b"],
        ["\u0100", "\u0100"],
        ["@*+-./_", "@*+-./_"],
        ["<>", "<>"],
        ["\ud800x", "\ud800x"],
        ["\udc00", "\udc00"],
        ["x\ud83d", "x\ud83d"],
    ],
    unescape: [
        ["", ""],
        ["a", "a"],
        ["photos", "photos"],
        ["my-bucket-2026-10-16-cat.jpg", "my-bucket-2026-10-16-cat.jpg"],
        ["my file (1) \u00e9+copy&x=1.jpg", "my file (1) \u00e9+copy&x=1.jpg"],
        ["!'()*-._~", "!'()*-._~"],
        ["!#$&'()*+,-./:;=?@_~", "!#$&'()*+,-./:;=?@_~"],
        ["%", "%"],
        ["100%", "100%"],
        ["%41", "A"],
        ["%zz", "%zz"],
        ["%u0041", "A"],
        ["%u00e9", "\u00e9"],
        ["%E2%82%AC", "\u00e2\u0082\u00ac"],
        ["%F0%9F%98%80", "\u00f0\u009f\u0098\u0080"],
        ["\u00ff", "\u00ff"],
        ["\u00e9t\u00e9", "\u00e9t\u00e9"],
        ["\u20ac", "\u20ac"],
        ["\ud83d\ude00", "\ud83d\ude00"],
        ["a\ud83d\ude00b", "a\ud83d\ude00b"],
        ["\u0100", "\u0100"],
        ["@*+-./_", "@*+-./_"],
        ["<>", "<>"],
        ["\ud800x", "\ud800x"],
        ["\udc00", "\udc00"],
        ["x\ud83d", "x\ud83d"],
    ],
};

const functions = { encodeURIComponent, encodeURI, escape, decodeURIComponent, decodeURI, unescape };

for (const [name, rows] of Object.entries(expectations)) {
    const func = functions[name];
    for (const [input, expected] of rows) {
        for (const [kind, string] of [["8-bit", input], ["16-bit", sixteenBit(input)]]) {
            shouldBe(string, input, name + " " + kind + " input");
            const message = name + "(" + JSON.stringify(input) + ") " + kind;
            if (typeof expected === "string") {
                const result = func(string);
                shouldBe(result, expected, message);
                shouldBe(result.length, expected.length, message + " length");
                // A result is 8-bit when it can be: a 16-bit argument that is returned as is is
                // narrowed, and an escaped result is ASCII.
                if (result.length && [...result].every(c => c.charCodeAt(0) < 0x100))
                    shouldBe(is8Bit(result), true, message + " is 8-bit");
            } else
                shouldThrow(() => func(string), expected, message);
        }
    }
}

// The argument is read as a string once, before anything else.
for (const [name, func] of Object.entries(functions)) {
    let calls = 0;
    const object = { toString() { calls++; return "a b"; } };
    func(object);
    shouldBe(calls, 1, name + " toString calls");
    shouldThrow(() => func(Symbol()), TypeError, name + "(Symbol())");
    shouldThrow(() => func({ toString() { throw new RangeError("x"); } }), RangeError, name + " toString throws");
}
shouldBe(encodeURIComponent(1e21), "1e%2B21", "encodeURIComponent(1e21)");
shouldBe(encodeURIComponent(), "undefined", "encodeURIComponent()");
shouldBe(escape(null), "null", "escape(null)");
shouldBe(unescape(undefined), "undefined", "unescape(undefined)");

// A large result.
{
    const input = "\u00e9".repeat(100000);
    shouldBe(encodeURIComponent(input), "%C3%A9".repeat(100000), "encodeURIComponent of 100000 escapes");
    shouldBe(escape(input), "%E9".repeat(100000), "escape of 100000 escapes");
    shouldBe(escape(sixteenBit("\u20ac".repeat(1000))), "%u20AC".repeat(1000), "escape of 1000 %u escapes");
    shouldBe(decodeURIComponent("%C3%A9".repeat(100000)), input, "decodeURIComponent of 100000 escapes");
    shouldBe(unescape("%E9".repeat(100000)), input, "unescape of 100000 escapes");
}

