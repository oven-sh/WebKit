// TODO(bun): port upstream's RegExp buffer boundaries (\A \z \Z, 2f66f5ed23f9) to this fork's YARR. The upstream sync #455
// brought this test in but kept the fork's YARR (rewritten in #299) where the two conflicted, so the fork has neither the
// feature nor --useRegExpBufferBoundaries, and the parser rejects \A in a Unicode pattern.
//@ skip
//@ requireOptions("--useRegExpBufferBoundaries=1")

(function() {
    var source = "";
    for (var i = 0; i < 4000; i++)
        source += "const value" + i + " = compute(" + i + "); // no shebang, no BOM\n";

    var shebang = /\A#!.*\n/u;
    var bom = /\A\uFEFF/u;
    var n = 1000;
    var result = 0;
    for (var i = 0; i < n; i++) {
        if (shebang.test(source))
            result++;
        if (bom.test(source))
            result++;
    }
    if (result !== 0)
        throw "Error: bad result: " + result;
})();
