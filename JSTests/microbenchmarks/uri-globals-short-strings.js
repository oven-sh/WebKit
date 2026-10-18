// The six URI functions on short strings: path pieces and query values that need no change,
// and values with a few escapes, as a URL builder or a query-string parser sees them.

const plain = ["photos", "my-bucket-2026-10-16-cat.jpg", "user_42", "v1", "index.html", "a-b-c-d-e-f", "2026", "x"];
const escaped = ["my file (1) \u00e9+copy&x=1.jpg", "a b", "100% legit", "q=\u20ac", "<b>bold</b>", "key=value&k2=v 2", "caf\u00e9", "\u{1F600}!"];
const encoded = escaped.map(encodeURIComponent);
const escapedForm = escaped.map(escape);

let expected = 0;
for (let j = 0; j < 8; j++) {
    expected += encodeURIComponent(plain[j]).length + encodeURI(plain[j]).length + escape(plain[j]).length;
    expected += decodeURIComponent(plain[j]).length + decodeURI(plain[j]).length + unescape(plain[j]).length;
    expected += encodeURIComponent(escaped[j]).length + encodeURI(escaped[j]).length + escape(escaped[j]).length;
    expected += decodeURIComponent(encoded[j]).length + decodeURI(encoded[j]).length + unescape(escapedForm[j]).length;
}

const iterations = 100000;
let sum = 0;
for (let i = 0; i < iterations; i++) {
    const j = i & 7;
    sum += encodeURIComponent(plain[j]).length + encodeURI(plain[j]).length + escape(plain[j]).length;
    sum += decodeURIComponent(plain[j]).length + decodeURI(plain[j]).length + unescape(plain[j]).length;
    sum += encodeURIComponent(escaped[j]).length + encodeURI(escaped[j]).length + escape(escaped[j]).length;
    sum += decodeURIComponent(encoded[j]).length + decodeURI(encoded[j]).length + unescape(escapedForm[j]).length;
}

if (sum !== expected * (iterations / 8))
    throw new Error("bad result: " + sum);
