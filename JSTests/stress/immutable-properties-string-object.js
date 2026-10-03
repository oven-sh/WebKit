// The optimizing compilers turn a StringObject into its string without a call where they can check that nobody has touched the object,
// which they do by its structure. A StringObject whose properties were made immutable passes that check too: the operations below
// give the same results for it, and a site that sees one is not reoptimized for it.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

// Enough calls for a site to be compiled, and then to exit often enough to be reoptimized if it is going to be.
let hot = Math.ceil(testLoopCount / 4);

let operations = {
    add: "object + suffix",
    addLeft: "suffix + object",
    template: "`${object}${suffix}`",
    stringCall: "String(object) + suffix",
    toStringCall: "object.toString() + suffix",
    valueOfCall: "object.valueOf() + suffix",
    charAt: "object.charAt(1) + suffix",
    index: "object[1] + suffix",
    length: "object.length + suffix",
    equals: "(object == 'abc') + suffix",
    concat: "object.concat(suffix)",
};

// (Functions with the same source share what the compilers have learnt about it, exits included: each site gets a source of its own.)
let sites = 0;
function makeSite(source) {
    let site = new Function("object", "suffix", "return " + source + "; // site " + ++sites);
    noInline(site);
    return site;
}

function run(site, pick, expected, label) {
    for (let i = 0; i < hot; ++i)
        shouldBe(site(pick(i), "-" + (i & 3)), expected(i), label);
}

// The realm's first such object appears when sites are hot already. Their code is thrown away, once, and no exit is held against them.
{
    let hotSites = Object.values(operations).map(makeSite), ordinary = new String("abc");
    let results = hotSites.map(site => { run(site, () => ordinary, i => site(ordinary, "-" + (i & 3))); return site(ordinary, "!"); });
    let immutable = $vm.makePropertiesImmutable(new String("abc"));
    hotSites.forEach((site, index) => {
        shouldBe(site(immutable, "!"), results[index]);
        run(site, i => i & 1 ? immutable : ordinary, i => site(ordinary, "-" + (i & 3)));
    });
}

// A call of a method looks the method up by the object's structure first, and a site learns a second structure as it does for any
// other object, by being reoptimized if it was compiled too early. Those are here for their results.
let looksUpAMethod = new Set(["toStringCall", "valueOfCall", "charAt", "concat"]);

for (let [name, source] of Object.entries(operations)) {
    let reference = makeSite(source);
    let expected = i => reference(new String("abc"), "-" + (i & 3));

    // 1. The site is hot on ordinary objects, and then sees objects with immutable properties.
    let later = makeSite(source), ordinary = new String("abc");
    run(later, () => ordinary, expected, name + ", ordinary");
    let immutable = $vm.makePropertiesImmutable(new String("abc"));
    run(later, () => immutable, expected, name + ", immutable");
    run(later, i => i & 1 ? immutable : ordinary, expected, name + ", both");

    // 2. The site sees both from the start.
    let fromTheStart = makeSite(source);
    run(fromTheStart, i => i & 1 ? immutable : ordinary, expected, name + ", both from the start");

    // 3. The same number of calls with ordinary objects alone: whatever reoptimization this configuration causes by itself.
    let control = makeSite(source);
    for (let round = 0; round < 3; ++round)
        run(control, () => ordinary, expected, name + ", control");

    if (!looksUpAMethod.has(name)) {
        shouldBe(reoptimizationRetryCount(later) <= reoptimizationRetryCount(control), true, name + ": reoptimized for an object with immutable properties, which it saw later");
        shouldBe(reoptimizationRetryCount(fromTheStart) <= reoptimizationRetryCount(control), true, name + ": reoptimized for an object with immutable properties");
    }
    shouldBe($vm.hasImmutableProperties(immutable), true);
}

// What the check is for still holds. An object that was touched first does not pass it, immutable properties or not.
{
    let site = makeSite("object + suffix");
    let ordinary = new String("abc");
    let withOwnToString = new String("abc");
    withOwnToString.toString = () => "own toString";
    withOwnToString.valueOf = () => "own valueOf";
    $vm.makePropertiesImmutable(withOwnToString);
    let withOtherPrototype = Object.setPrototypeOf(new String("abc"), { __proto__: String.prototype, valueOf() { return "inherited valueOf"; } });
    $vm.makePropertiesImmutable(withOtherPrototype);
    class Derived extends String { valueOf() { return "derived valueOf"; } }
    let derived = $vm.makePropertiesImmutable(new Derived("abc"));
    for (let i = 0; i < hot; ++i) {
        shouldBe(site(ordinary, "!"), "abc!");
        shouldBe(site(withOwnToString, "!"), "own valueOf!");
        shouldBe(site(withOtherPrototype, "!"), "inherited valueOf!");
        shouldBe(site(derived, "!"), "derived valueOf!");
    }
}

// Each realm has its own.
{
    let other = $vm.createGlobalObject();
    let site = makeSite("object + suffix");
    let objects = [new String("abc"), $vm.makePropertiesImmutable(new String("abc")), new other.String("abc"), $vm.makePropertiesImmutable(new other.String("abc"))];
    for (let i = 0; i < hot; ++i)
        shouldBe(site(objects[i & 3], "!"), "abc!");
}

// The object comes from an inlined function of another realm: it is that realm's String.prototype that counts, which the site, in this
// realm, does not watch. Such an object, immutable properties or not, is not taken for one of this realm's. (What produces the object
// has to be in the other realm's code once that is inlined: a call that is not inlined itself, and a load from an object.)
for (let change of [object => object, object => $vm.makePropertiesImmutable(object)]) {
    noInline(change);
    let other = $vm.createGlobalObject();
    change(new other.String("abc"));
    let holder = { object: change(new other.String("abc")) };
    let fresh = other.Function("change", "return change(new String('abc')); // site " + ++sites);
    let existing = other.Function("holder", "return holder.object; // site " + ++sites);
    let addFresh = new Function("make", "change", "suffix", "return make(change) + suffix; // site " + ++sites);
    let addExisting = new Function("make", "holder", "suffix", "return make(holder) + suffix; // site " + ++sites);
    noInline(addFresh);
    noInline(addExisting);
    for (let i = 0; i < hot; ++i) {
        shouldBe(addFresh(fresh, change, "!"), "abc!");
        shouldBe(addExisting(existing, holder, "!"), "abc!");
    }
    other.String.prototype.valueOf = other.Function("return 'replaced';");
    for (let i = 0; i < hot; ++i) {
        shouldBe(addFresh(fresh, change, "!"), "replaced!");
        shouldBe(addExisting(existing, holder, "!"), "replaced!");
    }
}

// The structure is kept. A realm's only such object is gone, and what a collected Structure would have left is taken by others: a new
// such object gets the structure that compiled code checks for.
{
    let other = $vm.createGlobalObject();
    let makeAndDrop = () => { $vm.makePropertiesImmutable(new other.String("abc")); };
    let overwriteTheStack = depth => depth ? overwriteTheStack(depth - 1) + 1 : 0;
    noInline(makeAndDrop);
    noInline(overwriteTheStack);
    makeAndDrop();
    overwriteTheStack(200);
    let shapes = [];
    for (let round = 0; round < 3; ++round) {
        fullGC();
        for (let i = 0; i < 500; ++i)
            shapes.push({ ["property" + round + "_" + i]: i });
    }
    let makeSiteThere = () => {
        let site = other.Function("object", "suffix", "return object + suffix; // site " + ++sites);
        noInline(site);
        return site;
    };
    let site = makeSiteThere(), control = makeSiteThere();
    for (let i = 0; i < hot * 3; ++i) {
        shouldBe(site($vm.makePropertiesImmutable(new other.String("abc")), "!"), "abc!");
        shouldBe(control(new other.String("abc"), "!"), "abc!");
    }
    shouldBe(reoptimizationRetryCount(site) <= reoptimizationRetryCount(control), true, "reoptimized after a collection");
}

// A change to String.prototype reaches such an object, hot, as it reaches any other. (Last: no site of this realm that is compiled after
// this gets the fast conversion, whatever is put back.)
{
    let site = makeSite("object + suffix");
    let immutable = $vm.makePropertiesImmutable(new String("abc"));
    for (let i = 0; i < hot; ++i)
        shouldBe(site(immutable, "!"), "abc!");
    let valueOf = String.prototype.valueOf;
    String.prototype.valueOf = function () { return "replaced"; };
    for (let i = 0; i < hot; ++i)
        shouldBe(site(immutable, "!"), "replaced!");
    String.prototype.valueOf = valueOf;
    shouldBe(site(immutable, "!"), "abc!");
}
