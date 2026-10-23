//@ runDefault("--countParseTimes=1", "--useCodeCache=1", "--releaseIdleCodeCacheEntriesWhenShrinkingFootprint=1")

// The unlinked code cache is bounded by bytes, not by a number of entries: a program that cycles through more distinct
// sources than an entry bound would hold (upstream: 2000) misses on every one of them. What the cache holds goes when the
// VM is idle: shrinkFootprintNow(KeepCodeInUse) drops the entries that no lookup has hit since the last full collection
// began, and keeps the others.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

var globalEval = eval;
var count = 2500; // over the entry bound this engine no longer has (2000), and no more: slow configurations run this too
var sources = [];
for (var i = 0; i < count; ++i)
    sources.push("1 + " + i);

function pass() {
    var sum = 0;
    for (var i = 0; i < sources.length; ++i)
        sum += globalEval(sources[i]);
    return sum;
}
var expected = count * (count - 1) / 2 + count;

assert(pass() === expected, "first pass");
var parses = $vm.parseCount();
assert(pass() === expected, "second pass");
assert($vm.parseCount() === parses, "the second pass parsed " + ($vm.parseCount() - parses) + " of " + count + " sources again");

fullGC();
assert(globalEval(sources[0]) === 1, "a source looked up after the collection began");
$vm.shrinkFootprintWhenIdle(true, true);
setTimeout(function () {
    try {
        parses = $vm.parseCount();
        assert(globalEval(sources[0]) === 1, "the source looked up after the collection began, again");
        assert($vm.parseCount() === parses, "the source looked up after the collection began is still cached");
        assert(globalEval(sources[1]) === 2, "a source not looked up since the collection began");
        assert($vm.parseCount() === parses + 1, "a source not looked up since the collection began is parsed again");
    } catch (e) {
        print("FAIL: " + e + "\n" + e.stack);
        $vm.abort();
    }
}, 0);
