(function() {
    var replacer = function(c, offset) { return "<" + offset + ">"; };
    var result;
    for (var i = 0; i < 400000; ++i)
        result = "a.b.c.d.e".replaceAll(".", replacer);
    if (result != "a<1>b<3>c<5>d<7>e")
        throw "Error: bad result: " + result;
})();
