//@ runDefault("--useCodeCache=1", "--codeCacheWorkingSetMaxBytes=300000")

// With no bound on its number of entries, the unlinked code cache bounds a stream of sources that never repeat by bytes:
// once a window (codeCacheWorkingSetMaxBytes) of new sources is in, the next window evicts down to one, so the cache
// holds between one and two windows of them. Each source counts as its length plus 1 KB, so the bound holds for tiny
// sources too.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

var globalEval = eval;
var window = 300000;
var weight = 1024 + "1 + 2999".length;
var count = 3000; // about ten windows

var sum = 0;
for (var i = 0; i < count; ++i)
    sum += globalEval("1 + " + i);
assert(sum === count * (count - 1) / 2 + count, "sum");

var census = $vm.codeBlockCensus();
assert(census.codeCacheBytes <= 2 * window + weight, "at most two windows: " + census.codeCacheBytes + " bytes");
assert(census.codeCacheEntries <= 2 * window / 1024 + 2, "at most two windows of entries: " + census.codeCacheEntries);
assert(census.codeCacheEntries >= window / weight - 2, "at least one window of entries: " + census.codeCacheEntries);
