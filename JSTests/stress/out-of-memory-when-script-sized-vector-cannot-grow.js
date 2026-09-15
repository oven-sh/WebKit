//@ if $buildType == "debug" then runDefault("--maxSingleAllocationSize=1048576") else skip end

// Each builtin below keeps one entry per script-controlled item in a WTF::Vector. When that Vector
// cannot grow, because the allocation fails or because a Vector holds at most 2^31 bytes, the
// builtin has to throw the out-of-memory RangeError. It used to crash.
// --maxSingleAllocationSize makes every allocation of more than 1 MiB fail, so a few hundred thousand
// items are enough here. No string below is longer than 1 MiB, because creating one would crash too.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function shouldThrowOutOfMemory(func) {
    let error = null;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (String(error) !== "RangeError: Out of memory")
        throw new Error(`bad error: ${String(error)} from ${func}`);
}

// Never ends on its own: the only way out of the consumer is the throw, which has to close it.
function endless(value) {
    const iterable = {
        closed: false,
        [Symbol.iterator]() {
            return {
                next() { return { done: false, value }; },
                return() { iterable.closed = true; return { }; },
            };
        },
    };
    return iterable;
}

// String.prototype.replaceAll(string, string): the offset of every match.
shouldThrowOutOfMemory(() => "a".repeat(200000).replaceAll("a", "c"));
shouldThrowOutOfMemory(() => "a".repeat(200000).replaceAll("", "c"));
shouldBe("banana".replaceAll("a", "o"), "bonono");

// String.prototype.replace(regexp, string): one part per "$" reference of the replacement.
shouldThrowOutOfMemory(() => "x".replace(/(x)/g, "$1".repeat(100000)));
shouldThrowOutOfMemory(() => "x".replace(/(x)/g, "$$".repeat(100000)));
shouldThrowOutOfMemory(() => "x".replace(/(?<name>x)/g, "-$<name>".repeat(50000)));
shouldBe("x".replace(/(x)/g, "[$1$$$&]"), "[x$x]");

// String.prototype.split(string): the end of every piece.
shouldThrowOutOfMemory(() => ",".repeat(400000).split(","));
shouldThrowOutOfMemory(() => "\u3042".repeat(400000).split("\u3042"));
shouldThrowOutOfMemory(() => ",;".repeat(400000).split(",;"));
shouldBe(JSON.stringify("a,b,,c".split(",")), `["a","b","","c"]`);

// JSON.parse with a reviver: the source range of every array element.
shouldThrowOutOfMemory(() => JSON.parse("[" + "1,".repeat(50000) + "1]", (key, value) => value));
shouldBe(JSON.stringify(JSON.parse("[1,[2,3]]", (key, value) => value)), "[1,[2,3]]");

// FinalizationRegistry.prototype.register: every registration, in one list per unregister token.
for (const token of [undefined, { }]) {
    const registry = new FinalizationRegistry(() => { });
    const target = { };
    shouldThrowOutOfMemory(() => {
        for (;;)
            registry.register(target, 1, token);
    });
    if (token) {
        shouldBe(registry.unregister(token), true);
        shouldBe(registry.unregister(token), false);
        registry.register(target, 1, token);
        shouldBe(registry.unregister(token), true);
    }
}

// Intl.ListFormat: every string of the iterable.
{
    const listFormat = new Intl.ListFormat("en");
    for (const format of [items => listFormat.format(items), items => listFormat.formatToParts(items)]) {
        const items = endless("a");
        shouldThrowOutOfMemory(() => format(items));
        shouldBe(items.closed, true);
    }
    shouldBe(listFormat.format(["a", "b", "c"]), "a, b, and c");
}

// WebAssembly.Tag: every parameter type. The compile options: every name of the builtins list.
if (typeof WebAssembly === "object") {
    const parameters = endless("i32");
    shouldThrowOutOfMemory(() => new WebAssembly.Tag({ parameters }));
    shouldBe(parameters.closed, true);

    const emptyModule = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);
    const builtins = endless("js-string");
    shouldThrowOutOfMemory(() => WebAssembly.validate(emptyModule, { builtins }));
    shouldBe(builtins.closed, true);
    shouldBe(WebAssembly.validate(emptyModule, { builtins: ["js-string"] }), true);
}
