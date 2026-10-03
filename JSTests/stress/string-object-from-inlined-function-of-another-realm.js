// The optimizing compilers turn a StringObject into its string without a call where the object has its realm's original structure and
// nobody has changed what String.prototype has for the conversion, which they watch. The two have to be about one realm. Here the
// object comes from an inlined function of another realm, and it is that realm's String.prototype that changes, while the site is hot.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

let hot = Math.ceil(testLoopCount / 4);

// One for each place the compilers make the conversion from: an addition, ToPrimitive, ToString, a call of String, and ToPropertyKey.
// What the conversion asks for first, of what is replaced below, is what answers.
let conversions = {
    add: ["make(holder) + '!'", { before: "abc!", valueOf: "replaced!", toString: "abc!", toPrimitive: "replaced!" }],
    addLeft: ["'!' + make(holder)", { before: "!abc", valueOf: "!replaced", toString: "!abc", toPrimitive: "!replaced" }],
    addThree: ["'<' + make(holder) + '>'", { before: "<abc>", valueOf: "<replaced>", toString: "<abc>", toPrimitive: "<replaced>" }],
    template: ["`${make(holder)}!`", { before: "abc!", valueOf: "abc!", toString: "replaced!", toPrimitive: "replaced!" }],
    stringCall: ["String(make(holder)) + '!'", { before: "abc!", valueOf: "abc!", toString: "replaced!", toPrimitive: "replaced!" }],
    concat: ["'!'.concat(make(holder))", { before: "!abc", valueOf: "!abc", toString: "!replaced", toPrimitive: "!replaced" }],
    propertyKey: ["Object.keys(class { static [make(holder)] = 1; })[0]", { before: "abc", valueOf: "abc", toString: "replaced", toPrimitive: "replaced" }],
};

// What produces the object is in the other realm's code once that is inlined.
let producers = {
    allocation: "return new String('abc');",
    load: "return holder.object;",
};

let replacements = {
    valueOf: other => { other.String.prototype.valueOf = other.Function("return 'replaced';"); },
    toString: other => { other.String.prototype.toString = other.Function("return 'replaced';"); },
    toPrimitive: other => { Object.defineProperty(other.String.prototype, Symbol.toPrimitive, { value: other.Function("return 'replaced';") }); },
};

// (Functions with the same source share what the compilers have learnt about it: each gets a source of its own.)
let sources = 0;

for (let [replacementName, replace] of Object.entries(replacements)) {
    // A realm for each: what is watched is not watched again once it has changed.
    let other = $vm.createGlobalObject();
    let holder = { object: new other.String("abc") };
    let sites = [];
    for (let [conversionName, [conversion, results]] of Object.entries(conversions)) {
        for (let [producerName, producer] of Object.entries(producers)) {
            let make = other.Function("holder", producer + " // " + ++sources);
            let site = new Function("make", "holder", "return " + conversion + "; // " + ++sources);
            noInline(site);
            for (let i = 0; i < hot; ++i)
                shouldBe(site(make, holder), results.before);
            sites.push({ site, make, after: results[replacementName], label: [conversionName, producerName, replacementName].join(", ") });
        }
    }
    replace(other);
    for (let { site, make, after, label } of sites) {
        for (let i = 0; i < hot; ++i)
            shouldBe(site(make, holder), after, label);
    }
}

// This realm's own objects still pass the check (a site is not reoptimized for them), and a change to this realm's String.prototype
// still reaches them.
{
    let make = new Function("holder", "return new String('abc'); // " + ++sources);
    let site = new Function("make", "holder", "return make(holder) + '!'; // " + ++sources);
    noInline(site);
    for (let i = 0; i < hot; ++i)
        shouldBe(site(make, null), "abc!");
    shouldBe(reoptimizationRetryCount(site), 0);
    String.prototype.valueOf = function () { return "replaced"; };
    for (let i = 0; i < hot; ++i)
        shouldBe(site(make, null), "replaced!");
}
