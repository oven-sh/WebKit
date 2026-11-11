// Test performance of the RegExp.prototype.source getter, for a pattern text that is its own source.
var re = /^\/users\/(\d+)\/items\/(\d+)$/;
var result = 0;
for (var i = 0; i < 1e6; ++i)
    result += re.source.length;
if (result !== 30e6)
    throw new Error("Bad result: " + result);
