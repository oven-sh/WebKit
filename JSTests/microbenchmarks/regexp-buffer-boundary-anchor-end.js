// TODO(bun): port upstream's RegExp buffer boundaries (\A \z \Z, 2f66f5ed23f9) to this fork's YARR. The upstream sync #455
// brought this test in but kept the fork's YARR (rewritten in #299) where the two conflicted, so the fork has neither the
// feature nor --useRegExpBufferBoundaries, and the parser rejects \A in a Unicode pattern.
//@ skip
//@ requireOptions("--useRegExpBufferBoundaries=1")

(function() {
    var urls = [];
    for (var i = 0; i < 1000; i++)
        urls.push("https://cdn.example.com/assets/build/" + i + "/static/media/components/very/deeply/nested/directory/structure/image-" + i + (i % 7 == 0 ? ".png" : ".webp?width=1024&quality=80"));

    var re = /\.png\z/u;
    var n = 400;
    var result = 0;
    for (var i = 0; i < n; i++) {
        for (var j = 0; j < urls.length; j++) {
            if (re.test(urls[j]))
                result++;
        }
    }
    if (result !== n * 143)
        throw "Error: bad result: " + result;
})();
