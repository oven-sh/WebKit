// A set of sources that is evaluated again and again, with 300 sources the code cache has not seen between two
// uses. The cache is full, so every new source evicts an entry. A source of the set that was evicted is parsed again.
var globalEval = eval;

var body = "";
for (var i = 0; i < 100; ++i)
    body += "function f" + i + "(a, b) { return a * " + i + " + b; }\n";
var reused = [];
for (var i = 0; i < 50; ++i)
    reused.push(body + "f1(" + i + ", 1)");

function test(rounds) {
    var numbers = 0;
    var next = 0;
    for (var round = 0; round < rounds; ++round) {
        for (var i = 0; i < reused.length; ++i) {
            if (globalEval(reused[i]) === i + 1)
                ++numbers;
        }
        for (var i = 0; i < 300; ++i) {
            if (typeof globalEval("1 + " + next++) === "number")
                ++numbers;
        }
    }
    return numbers;
}
noInline(test);

// Fill the cache first.
for (var i = 0; i < 3000; ++i)
    globalEval("2 + " + i);

var rounds = 40;
var result = test(rounds);
if (result !== rounds * (reused.length + 300))
    throw new Error("bad result: " + result);
