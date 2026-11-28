//@ runDefault

// streamingJSONParse() (Bun.JSONL) tells text that ends inside a value from text that is wrong by where the lexer
// stopped: it answers "needMoreData" only when that is the end of the text. Text that ends right after a backslash in
// a string ends inside the string, and the character that the backslash escapes comes with the next chunk. Upstream's
// Lexer::lexStringError() leaves the lexer on that backslash, so the fork moves it to the end there.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

function check(text, status, valueCount, charactersConsumed) {
    const result = streamingJSONParse(text);
    const what = JSON.stringify(text);
    shouldBe(result.status, status, "status of " + what);
    shouldBe(result.values.length, valueCount, "number of values of " + what);
    shouldBe(result.charactersConsumed, charactersConsumed, "charactersConsumed of " + what);
    return result.values;
}

const line = '{"a":1}';

// "\u4e00" makes the text 16-bit.
for (const character of ["x", "\u4e00"]) {
    const run = character.repeat(40);
    const heads = ['"', '["', '{"', '{"a":"', '{"a":"' + character + '\\n', '{"a":"' + run + '\\t' + run];
    for (const head of heads) {
        const text = head + character + "\\";
        check(text, "needMoreData", 0, 0);
        check(line + "\n" + text, "needMoreData", 1, line.length);
    }

    // With the character that the backslash escapes, the string is whole.
    const whole = '"' + character + '\\""';
    shouldBe(check(whole, "complete", 1, whole.length)[0], character + '"', "value of " + JSON.stringify(whole));

    // Nothing that follows makes an escape of "\q", so this is an error at the end of the text too.
    const wrong = '"' + character + "\\q";
    check(wrong, "error", 0, 0);
    check(line + "\n" + wrong, "error", 1, line.length);
}
