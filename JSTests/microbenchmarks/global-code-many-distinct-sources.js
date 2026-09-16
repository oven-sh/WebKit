// Every indirect eval and every Function constructor call here has a source the code cache has not seen. Once the
// cache holds CodeCacheMap::workingSetMaxEntries (2000) entries, each of them evicts one.
var globalEval = eval;

function test(count) {
    var numbers = 0;
    for (var i = 0; i < count; ++i) {
        if (typeof globalEval("1 + " + i) === "number")
            ++numbers;
        if (typeof new Function("return 2 + " + i)() === "number")
            ++numbers;
    }
    return numbers;
}
noInline(test);

var count = 15000;
var result = test(count);
if (result !== 2 * count)
    throw new Error("bad result: " + result);
