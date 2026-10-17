// Every indirect eval and every Function constructor call here has a source the code cache has not seen. Once the
// cache has admitted Options::codeCacheWorkingSetMaxBytes (16 MB, each entry counting as its length plus 1 KB: about
// 15000 of these sources), the sources after that evict entries.
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
