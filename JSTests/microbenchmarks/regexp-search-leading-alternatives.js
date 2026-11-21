let lines = [
    "The quick brown fox jumps over the lazy dog, then stops; it looks (twice) at the fence and leaves.",
    "Nothing in this line needs to be decoded, which is what most lines of a document are like.",
    "Only the end of this one does: fish &amp;&amp; chips",
];
let re = /(?:&amp;|&lt;|&gt;|&quot;|&#39;)+/;
let result = 0;
for (let i = 0; i < 1e5; ++i) {
    for (let line of lines)
        result += re.test(line);
}
if (result !== 1e5)
    throw new Error("Bad result: " + result);
