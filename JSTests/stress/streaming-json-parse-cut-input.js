// streamingJSONParse() is the jsc shell's entry to LiteralParser::tryStreamingParse (Bun.JSONL).
// A stream can end a chunk anywhere, so the prefix of a valid line is "needMoreData" wherever the cut falls.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}

var first = '{"first":1}';
var lines = [
    '{"name":"caf\\u00e9"}',
    '{"\\u006bey":"\\ud83d\\ude00 \\u2028"}',
    '"caf\\u00e9"',
    '[1e-7,2E+5,-3.5e10,0.25E-3,6e2]',
    '{"n":1e-7,"m":[2.5e+3],"t":true,"f":false,"z":null}',
    '{"jp":"日本","e":"\\u00e9","n":[1e-7,2E+5]}', // 16-bit input
];

for (var line of lines) {
    var input = first + "\n" + line + "\n";
    for (var cut = first.length + 2; cut < input.length - 1; ++cut) {
        var chunk = input.slice(0, cut);
        var result = streamingJSONParse(chunk);
        var message = JSON.stringify(chunk);
        shouldBe(result.status, "needMoreData", message);
        shouldBe(result.values.length, 1, message);
        shouldBe(result.charactersConsumed, first.length, message);
    }

    var result = streamingJSONParse(input);
    shouldBe(result.status, "complete", line);
    shouldBe(result.values.length, 2, line);
    shouldBe(JSON.stringify(result.values[1]), JSON.stringify(JSON.parse(line)), line);
}

// A top-level number cut inside its exponent produces no value for the digits in front of the 'e'.
for (var chunk of ["1e", "1e-", "1E+", "-2.5e", "-2.5E-", "0e"]) {
    var result = streamingJSONParse(chunk);
    shouldBe(result.status, "needMoreData", chunk);
    shouldBe(result.values.length, 0, chunk);
    shouldBe(result.charactersConsumed, 0, chunk);
}

// The character that makes the escape or the exponent malformed is in the input: no later chunk can fix it.
var malformed = [
    '"\\uz', '"\\u0z', '"\\u00zz', '"\\u00"', '"\\u00\n', '{"a":"\\u12"}\n', '{"\\u00":1}\n',
    '[1e-]', '[1e+,', '[1ex', '{"a":1e}\n', '[1.5e-\n', '1e-\n', '1e+x',
];
for (var chunk of malformed) {
    var result = streamingJSONParse(chunk);
    var message = JSON.stringify(chunk);
    shouldBe(result.status, "error", message);
    shouldBe(result.values.length, 0, message);
    shouldBe(result.charactersConsumed, 0, message);
}
