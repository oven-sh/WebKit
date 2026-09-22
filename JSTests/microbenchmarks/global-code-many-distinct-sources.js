// Every indirect eval and every Function constructor call here has a source the code cache has not seen. Once the
// cache holds CodeCacheMap::workingSetMaxEntries (2000) entries, each of them evicts one.
var globalEval = eval;

function test(count) {
    var sum = 0;
    for (var i = 0; i < count; ++i) {
        sum += globalEval("1 + " + i);
        sum += new Function("return 2 + " + i)();
    }
    return sum;
}
noInline(test);

var count = 15000;
var result = test(count);
var expected = count * (count - 1) + 3 * count;
if (result !== expected)
    throw new Error("bad result: " + result + ", expected " + expected);
