(function() {
    var replacement = "<span class=\"" + "hl" + "\">" + "x".repeat(40) + "</span>";
    var replacer = function() { return replacement; };
    var result;
    for (var i = 0; i < 100000; ++i)
        result = "a.b.c.d.e.f.g.h".replace(/\./g, replacer);
    if (result.length != 8 + 7 * replacement.length)
        throw "Error: bad result: " + result;
})();
