// Tracks the steady-state cost of a direct call of `RegExp.prototype[Symbol.replace]` on a primordial RegExp.
// Tuned to finish under ~100ms wall-clock on a release build.

var inputs = ["abc 123 def 456 ghi", "abc 124 def 457 ghi"];

var digits = /\d+/g;
var firstWord = /[a-z]+/;
var pairs = /(\d)(\d)/g;

function symbolReplace(regexp, string, replacement)
{
    return regexp[Symbol.replace](string, replacement);
}
noInline(symbolReplace);

function swap(match, first, second)
{
    return second + first;
}

for (var i = 0; i < 2e5; ++i)
    symbolReplace(digits, inputs[i & 1], "-");

for (var i = 0; i < 2e5; ++i)
    symbolReplace(firstWord, inputs[i & 1], "[$&]");

for (var i = 0; i < 1e5; ++i)
    symbolReplace(pairs, inputs[i & 1], swap);
