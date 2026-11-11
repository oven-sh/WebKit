//@ defaultRun; run("zombie", "--useZombieMode=true", "--sweepSynchronously=true", "--slowPathAllocsBetweenGCs=10")

// A RegExp keeps the text of `source` from the first read on: every later read returns that one string, and the
// pattern is not escaped again. These are the values `source` must have, the receivers it accepts, and what keeps the
// kept string right: compile() gives the object another RegExp, and the collector sees the string.

function shouldBe(actual, expected, message)
{
    if (actual !== expected)
        throw new Error(`${message ? message + ": " : ""}expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function shouldThrowTypeError(func, message, errorConstructor = TypeError)
{
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorConstructor))
        throw new Error(`${message}: expected a TypeError but got ${error}`);
}

function sourceOf(regExp)
{
    return regExp.source;
}
noInline(sourceOf);

// The pattern text, and the `source` of a RegExp made from it.
const longClass = "[" + "a-b".repeat(400000) + "]";
const cases = [
    ["", "(?:)"],
    ["a", "a"],
    ["abc", "abc"],
    ["(?:)", "(?:)"],
    ["/", "\\/"],
    ["a/b", "a\\/b"],
    ["a\\/b", "a\\/b"],
    ["[/]", "[/]"],
    ["[/]/", "[/]\\/"],
    ["[\\]/]/", "[\\]/]\\/"],
    ["\\[/", "\\[\\/"],
    ["a\nb", "a\\nb"],
    ["a\rb", "a\\rb"],
    ["a\u2028b", "a\\u2028b"],
    ["a\u2029b", "a\\u2029b"],
    ["a\\\nb", "a\\nb"],
    ["[\n]", "[\\n]"],
    ["^/users/(\\d+)/items/(\\d+)$", "^\\/users\\/(\\d+)\\/items\\/(\\d+)$"],
    ["\u3042", "\u3042"],
    ["\u3042\u3044", "\u3042\u3044"],
    ["\u3042/\u3044", "\u3042\\/\u3044"],
    ["\u3042\n/", "\u3042\\n\\/"],
    ["x".repeat(5000) + "/", "x".repeat(5000) + "\\/"],
    // More than 1 MiB, and it needs an escape.
    [longClass + "/", longClass + "\\/"],
];

// Values. Every read returns the same cell.
for (const [text, expected] of cases) {
    const regExp = new RegExp(text);
    const first = regExp.source;
    shouldBe(first, expected);
    shouldBe(addressOf(regExp.source), addressOf(first), "second read");
    shouldBe(sourceOf(regExp), expected);
    shouldBe(String(regExp), "/" + expected + "/");
    // The source is a pattern text whose source is itself.
    shouldBe(new RegExp(first).source, expected);
}

// In every tier.
{
    const regExps = cases.slice(0, 22).map(([text]) => new RegExp(text));
    const addresses = regExps.map(regExp => addressOf(regExp.source));
    for (let i = 0; i < testLoopCount; ++i) {
        const index = i % regExps.length;
        const source = sourceOf(regExps[index]);
        if (source !== cases[index][1] || addressOf(source) !== addresses[index])
            throw new Error(`iteration ${i}: ${JSON.stringify(source)}`);
    }
}

// A literal, a RegExp made from text, and two objects of one text.
{
    const fromText = new RegExp("a/b");
    const again = new RegExp("a/b");
    const literal = /a\/b/;
    shouldBe(fromText.source, "a\\/b");
    shouldBe(literal.source, "a\\/b");
    shouldBe(again.source, "a\\/b");
    // While one object of a text and flags lives, a second one shares its RegExp, and so its source.
    shouldBe(addressOf(again.source), addressOf(fromText.source), "two objects of one text");

    const withFlags = new RegExp("a/b", "gi");
    shouldBe(withFlags.source, "a\\/b");
    shouldBe(withFlags.flags, "gi");
    shouldBe(addressOf(withFlags.source), addressOf(withFlags.source));
}

// compile() gives the object another RegExp. The source that was kept for the old one does not follow.
{
    const regExp = new RegExp("a/b");
    const other = new RegExp("a/b");
    shouldBe(sourceOf(regExp), "a\\/b");
    regExp.compile("c/d");
    shouldBe(sourceOf(regExp), "c\\/d");
    shouldBe(other.source, "a\\/b", "an object that was not recompiled");
    regExp.compile("c/d");
    shouldBe(sourceOf(regExp), "c\\/d");
    regExp.compile(/e\/f/g);
    shouldBe(sourceOf(regExp), "e\\/f");
    shouldBe(regExp.flags, "g");
    regExp.compile();
    shouldBe(sourceOf(regExp), "(?:)");
    regExp.compile("a/b");
    shouldBe(sourceOf(regExp), "a\\/b");
    shouldBe(addressOf(regExp.source), addressOf(other.source), "compiled back to a text that is alive");

    const texts = ["p/" + 0, "p/" + 1, "p/" + 2];
    for (let i = 0; i < testLoopCount; ++i) {
        regExp.compile(texts[i % 3]);
        const source = sourceOf(regExp);
        if (source !== "p\\/" + (i % 3))
            throw new Error(`compile, iteration ${i}: ${JSON.stringify(source)}`);
    }
}

// Receivers.
{
    const getter = Object.getOwnPropertyDescriptor(RegExp.prototype, "source").get;
    const regExp = new RegExp("r/s");
    shouldBe(getter.call(regExp), "r\\/s");
    shouldBe(Reflect.get(RegExp.prototype, "source", regExp), "r\\/s");

    class Derived extends RegExp { }
    const derived = new Derived("r/s");
    shouldBe(derived.source, "r\\/s");
    shouldBe(getter.call(derived), "r\\/s");

    shouldBe(RegExp.prototype.source, "(?:)");
    shouldBe(getter.call(RegExp.prototype), "(?:)");
    shouldBe(addressOf(RegExp.prototype.source), addressOf(RegExp.prototype.source), "the prototype");
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(sourceOf(RegExp.prototype), "(?:)");

    for (const receiver of [undefined, null, 1, "a/b", { }, [], function () { }, Object.create(regExp), Object.create(RegExp.prototype)])
        shouldThrowTypeError(() => getter.call(receiver), `receiver ${typeof receiver}`);

    // Another realm's RegExp is a RegExp. Another realm's prototype is not this getter's prototype.
    const realm = createGlobalObject();
    const foreign = new realm.RegExp("r/s");
    const foreignGetter = Object.getOwnPropertyDescriptor(realm.RegExp.prototype, "source").get;
    shouldBe(foreign.source, "r\\/s");
    shouldBe(getter.call(foreign), "r\\/s");
    shouldBe(foreignGetter.call(regExp), "r\\/s");
    shouldBe(realm.RegExp.prototype.source, "(?:)");
    shouldThrowTypeError(() => getter.call(realm.RegExp.prototype), "another realm's prototype");
    shouldThrowTypeError(() => foreignGetter.call(RegExp.prototype), "this realm's prototype in another realm's getter", realm.TypeError);
    // One VM has one RegExp for a text and flags, whichever realm asks.
    shouldBe(addressOf(foreign.source), addressOf(regExp.source), "two realms");

    // An own property, and a getter that replaced the built-in one, are what a read finds.
    const shadowed = new RegExp("r/s");
    shouldBe(shadowed.source, "r\\/s");
    Object.defineProperty(shadowed, "source", { value: "own" });
    shouldBe(sourceOf(shadowed), "own");
    shouldBe(getter.call(shadowed), "r\\/s");

    realm.eval(`Object.defineProperty(RegExp.prototype, "source", { get() { return "replaced"; }, configurable: true })`);
    shouldBe(foreign.source, "replaced");
    shouldBe(String(foreign), "/replaced/");
    shouldBe(foreignGetter.call(foreign), "r\\/s");
}

// The kept string is a string like any other.
{
    const regExp = new RegExp("k/ey");
    const source = regExp.source;
    const object = { };
    object[source] = 1;
    shouldBe(Object.keys(object)[0], "k\\/ey");
    shouldBe(regExp.source, "k\\/ey");
    shouldBe(regExp.source + "!", "k\\/ey!");
    shouldBe(regExp.source.length, 5);
    shouldBe(addressOf(regExp.source), addressOf(source));
}

// The collector keeps the string for as long as the RegExp lives, and it is the only thing that does here.
{
    const count = 400;
    const regExps = [];
    const addresses = [];
    for (let i = 0; i < count; ++i) {
        const regExp = new RegExp("gc/" + i);
        regExps.push(regExp);
        addresses.push(addressOf(regExp.source));
    }
    let garbage = [];
    for (let round = 0; round < 3; ++round) {
        edenGC();
        for (let i = 0; i < 2000; ++i)
            garbage.push("gc\\/" + i + "/" + round);
        garbage = [];
        fullGC();
        for (let i = 0; i < count; ++i) {
            const source = regExps[i].source;
            if (source !== "gc\\/" + i || addressOf(source) !== addresses[i])
                throw new Error(`after collection ${round}, RegExp ${i}: ${JSON.stringify(source)}`);
        }
    }
}

// The string outlives its RegExp, and a RegExp made again from a text has the same source.
{
    function sourceOfNewRegExp(text)
    {
        return new RegExp(text).source;
    }
    noInline(sourceOfNewRegExp);

    const kept = [];
    for (let i = 0; i < 200; ++i)
        kept.push(sourceOfNewRegExp("once/" + i));
    fullGC();
    for (let i = 0; i < 200; ++i) {
        shouldBe(kept[i], "once\\/" + i);
        shouldBe(sourceOfNewRegExp("once/" + i), "once\\/" + i);
    }
}

// The first-character filter of a constant RegExp and its source are allocated together, by whichever is asked for
// first. The filter is asked for by a compiler thread.
{
    function testSourceReadFirst(string)
    {
        return /^filter\/first/.test(string);
    }
    noInline(testSourceReadFirst);

    function testSourceReadLast(string)
    {
        return /^filter\/last/.test(string);
    }
    noInline(testSourceReadLast);

    shouldBe(/^filter\/first/.source, "^filter\\/first");
    const subjects = ["filter/first", "filter/last", "other", "", "f", "\u3042"];
    for (let i = 0; i < testLoopCount; ++i) {
        const subject = subjects[i % subjects.length];
        shouldBe(testSourceReadFirst(subject), subject === "filter/first");
        shouldBe(testSourceReadLast(subject), subject === "filter/last");
    }
    shouldBe(/^filter\/last/.source, "^filter\\/last");
    shouldBe(/^filter\/first/.source, "^filter\\/first");
    for (let i = 0; i < testLoopCount; ++i) {
        const subject = subjects[i % subjects.length];
        shouldBe(testSourceReadFirst(subject), subject === "filter/first");
        shouldBe(testSourceReadLast(subject), subject === "filter/last");
    }
}
