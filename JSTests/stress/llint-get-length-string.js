//@ runDefault
//@ runDefault("--useJIT=0", "--missCountForLLIntTierUp=250")
//@ runDefault("--useJIT=0", "--missCountForLLIntTierUp=250", "--useLLIntStringLengthFastPath=0")
//@ runDefault("--useJIT=0", "--collectContinuously=1")

// A get_length in the LLInt that misses its cache reads the length of a string with no call of the slow path.
// The slow path counts its calls for each site (Options::missCountForLLIntTierUp()), so the count of a site
// says which reads went there.

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
const fastPath = options.useLLIntStringLengthFastPath;
// The count of a site stops at the option.
const countsSlowPathCalls = interpreterOnly && options.missCountForLLIntTierUp >= 250;

// The site has a cache of this kind, and it is for each of the receivers.
function expectCache(length, kind, ...receivers) {
    if (!interpreterOnly)
        return;
    shouldBe($vm.llintGetByIdCaches(length)[0], kind);
    for (const receiver of receivers)
        shouldBe($vm.llintGetByIdCacheHits(length, receiver)[0], true, "cache for " + typeof receiver);
}

// The reads of the site made this many calls of the slow path: the first number with the fast path for strings,
// the second without.
function expectSlowPathCalls(length, withFastPath, withoutFastPath, message) {
    if (countsSlowPathCalls)
        shouldBe($vm.llintGetByIdMissCounts(length)[0], fastPath ? withFastPath : withoutFastPath, message);
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
    expectCache(length, "empty");
    expectSlowPathCalls(length, 0, 4 * warmUp, "strings of each representation");
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
    // These are 280 reads. The count stops at 250.
    expectSlowPathCalls(length, 0, 250, "ropes and substrings");
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
    expectSlowPathCalls(length, 1, 1, "an array");
    for (let i = 0; i < warmUp; ++i)
        shouldBe(length("four"), 4);
    // A string does not take the cache from the array.
    expectCache(length, "arrayLength", array);
    expectSlowPathCalls(length, 1, 1 + warmUp, "an array, then strings");
    array.push(4);
    for (let i = 0; i < warmUp; ++i) {
        shouldBe(length(array), 4);
        shouldBe(length("sixsix"), 6);
    }
    expectCache(length, "arrayLength", array);
    expectSlowPathCalls(length, 1, 1 + 2 * warmUp, "an array and strings in turn");
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
