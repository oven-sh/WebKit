// Test performance of the RegExp.prototype.source getter, for a pattern text that has to be escaped.
var re = new RegExp("^/users/(\\d+)/items/(\\d+)$");
var result = 0;
for (var i = 0; i < 1e6; ++i)
    result += re.source.length;
if (result !== 30e6)
    throw new Error("Bad result: " + result);
