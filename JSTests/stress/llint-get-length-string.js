//@ runDefault
//@ runDefault("--useJIT=0")
//@ runDefault("--useJIT=0", "--useLLIntStringLengthCaching=0")
//@ runDefault("--useJIT=0", "--collectContinuously=1")

// The LLInt reads "length" of a string in the fast path of get_length (GetByIdMode::StringLength).

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

// Functions of the same source share their code, and so their caches. Each function has its own source.
let functions = 0;
function makeLength() {
    return new Function("value", "return value.length; // " + functions++);
}

const warmUp = 20;
const options = jscOptions();
// The cache of a site can be told only while the function runs in the LLInt.
const interpreterOnly = !options.useJIT && options.useLLIntICs;
const cachesStringLength = interpreterOnly && options.useLLIntStringLengthCaching;

// The site has a cache of this kind, and the fast path serves each of the receivers from it.
function expectCache(length, kind, ...receivers) {
    if (!interpreterOnly)
        return;
    shouldBe($vm.llintGetByIdCaches(length)[0], kind);
    for (const receiver of receivers)
        shouldBe($vm.llintGetByIdCacheHits(length, receiver)[0], true, "fast path for " + typeof receiver);
}

// Strings of each representation.
{
    const length = makeLength();
    const latin1 = "hello";
    const utf16 = "h\u00e9llo \u4e16\u754c";
    const empty = "";
    const single = "x";
    for (let i = 0; i < warmUp; ++i) {
        shouldBe(length(latin1), 5);
        shouldBe(length(utf16), 8);
        shouldBe(length(empty), 0);
        shouldBe(length(single), 1);
    }
    if (cachesStringLength)
        expectCache(length, "stringLength", latin1, utf16, empty, single, latin1 + utf16, [1, 2]);
    else
        expectCache(length, "empty");
}

// Ropes. Reading the length must not need the rope to be resolved, and must be right after it is.
{
    const length = makeLength();
    function rope(n) {
        let left = "a".repeat(n % 7 + 1) + n;
        let right = "\u4e16".repeat(n % 5 + 1) + n;
        return left + right; // Not resolved.
    }
    for (let i = 0; i < warmUp * 5; ++i) {
        const left = "a".repeat(i % 7 + 1) + i;
        const right = "\u4e16".repeat(i % 5 + 1) + i;
        const s = rope(i);
        shouldBe(length(s), left.length + right.length, "rope");
        shouldBe(s.charCodeAt(0), 97); // Resolves the rope.
        shouldBe(length(s), left.length + right.length, "resolved rope");
    }
    // Three fibers, and a rope of ropes.
    const a = "abc".repeat(11), b = "defg".repeat(13), c = "hijkl".repeat(17);
    for (let i = 0; i < warmUp; ++i) {
        shouldBe(length(a + b + c), a.length + b.length + c.length, "three fibers");
        shouldBe(length((a + b) + (c + a)), 2 * a.length + b.length + c.length, "rope of ropes");
    }
    // Substrings.
    const long = "0123456789".repeat(20);
    for (let i = 0; i < warmUp; ++i) {
        shouldBe(length(long.substring(3, 150)), 147, "substring");
        shouldBe(length(long.slice(i)), 200 - i, "slice");
    }
}

// A long string.
{
    const length = makeLength();
    const big = "ab".repeat(1 << 24);
    for (let i = 0; i < warmUp; ++i)
        shouldBe(length(big), 1 << 25);
    const bigRope = big + big;
    for (let i = 0; i < warmUp; ++i)
        shouldBe(length(bigRope), 1 << 26, "long rope");
}

// One site that sees strings and arrays and other things.
{
    const length = makeLength();
    const values = [
        ["abc", 3],
        [[1, 2], 2],
        ["", 0],
        [[], 0],
        [[1.5, 2.5, 3.5], 3],
        ["x".repeat(3) + "y".repeat(40), 43],
        [{ length: "own" }, "own"],
        [new String("wrapped"), 7],
        [function (a, b, c) { }, 3],
        [new Uint8Array(9), 9],
        [(function () { return arguments; })(1, 2, 3, 4), 4],
        [{ }, undefined],
        [5, undefined],
        [Symbol(), undefined],
        [true, undefined],
    ];
    for (let round = 0; round < warmUp; ++round) {
        for (const [value, expected] of values)
            shouldBe(length(value), expected, "mixed site, round " + round);
    }
    // Strings only for a while, then the rest again.
    for (let i = 0; i < warmUp; ++i)
        shouldBe(length("abc" + i), 3 + String(i).length);
    for (const [value, expected] of values)
        shouldBe(length(value), expected, "mixed site after strings");
}

// An array first, then strings, then the array again.
{
    const length = makeLength();
    const array = [1, 2, 3];
    for (let i = 0; i < warmUp; ++i)
        shouldBe(length(array), 3);
    expectCache(length, "arrayLength", array);
    for (let i = 0; i < warmUp; ++i)
        shouldBe(length("four"), 4);
    // The mode for strings reads the length of an array too, so the site does not alternate.
    if (cachesStringLength)
        expectCache(length, "stringLength", "four", array);
    else
        expectCache(length, "arrayLength", array);
    array.push(4);
    for (let i = 0; i < warmUp; ++i) {
        shouldBe(length(array), 4);
        shouldBe(length("sixsix"), 6);
    }
    if (cachesStringLength)
        expectCache(length, "stringLength", "sixsix", array);
    array.length = 0;
    shouldBe(length(array), 0);
    // An array with more than 2^31 - 1 elements has a length that is not an int32.
    array.length = 0x80000000;
    shouldBe(length(array), 0x80000000);
    shouldBe(length("abc"), 3);
}

// "length" of a string does not come from the prototype.
{
    const length = makeLength();
    for (let i = 0; i < warmUp; ++i)
        shouldBe(length("abc"), 3);
    shouldBe(length(String.prototype), 0, "String.prototype is a String object");
    shouldBe(length(new String("abcd")), 4);
    Object.defineProperty(Object.prototype, "length", { get() { return "object prototype"; }, configurable: true });
    shouldBe(length("abc"), 3);
    shouldBe(length({ }), "object prototype");
    shouldBe(length(5), "object prototype");
    delete Object.prototype.length;
}

// null and undefined throw.
{
    const length = makeLength();
    for (let i = 0; i < warmUp; ++i)
        shouldBe(length("abc"), 3);
    for (const value of [null, undefined]) {
        let threw = false;
        try {
            length(value);
        } catch (error) {
            threw = error instanceof TypeError;
        }
        shouldBe(threw, true, "length of " + value);
    }
    shouldBe(length("abcd"), 4);
}

// Strings made by other realms, and strings read by code of other realms.
if (typeof createGlobalObject === "function") {
    const other = createGlobalObject();
    const length = makeLength();
    const otherLength = other.Function("value", "return value.length;");
    const otherString = other.eval("'from ' + 'elsewhere'");
    for (let i = 0; i < warmUp; ++i) {
        shouldBe(length(otherString), 14);
        shouldBe(otherLength("local"), 5);
        shouldBe(otherLength([1, 2]), 2);
    }
}

// The same in a loop that is long enough for every tier.
{
    function total(strings) {
        let sum = 0;
        for (let i = 0; i < strings.length; ++i)
            sum += strings[i].length;
        return sum;
    }
    const strings = [];
    let expected = 0;
    for (let i = 0; i < 1000; ++i) {
        const s = i % 3 ? "s" + i : "r".repeat(i % 11) + ("o" + i);
        strings.push(s);
        expected += s.length;
    }
    for (let i = 0; i < 200; ++i)
        shouldBe(total(strings), expected);
}
