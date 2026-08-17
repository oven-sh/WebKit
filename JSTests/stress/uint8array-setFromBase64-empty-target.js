// FromBase64 step 3: when the target has no room (maxLength is 0), nothing is read,
// so the string is never scanned and an invalid one is not a SyntaxError.
// https://tc39.es/proposal-arraybuffer-base64/spec/#sec-frombase64

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`FAIL${message ? ` (${message})` : ""}: expected '${expected}' actual '${actual}'`);
}

function shouldBeEmptyResult(result, message) {
    shouldBe(Object.keys(result).join(), "read,written", message);
    shouldBe(result.read, 0, message);
    shouldBe(result.written, 0, message);
}

function shouldThrow(callback, errorConstructor, message) {
    try {
        callback();
    } catch (e) {
        shouldBe(e instanceof errorConstructor, true, message);
        return;
    }
    throw new Error(`FAIL${message ? ` (${message})` : ""}: should have thrown`);
}

const inputs = [
    "#", "a#", "aa#", "aaa#", "aaaa#", "!!!",
    "a", "aa", "aaa", "aaaa", "aaaaaaaa",
    "=", "==", "aa=", "aa==", "aa===", "aaa=", "aaaa=", "a===",
    " ", "  ", " aaaa ", "\taa==\n",
    "+/", "-_",
    "\u00A0", "\u2028", "Z\u2212==", "\uFF21\uFF21",
];

const optionsList = [undefined, {}];
for (let alphabet of [undefined, "base64", "base64url"]) {
    for (let lastChunkHandling of [undefined, "loose", "strict", "stop-before-partial"]) {
        optionsList.push({alphabet, lastChunkHandling});
        optionsList.push({get alphabet() { return alphabet; }, get lastChunkHandling() { return lastChunkHandling; }});
    }
}

function describe(input, options) {
    return `${JSON.stringify(input)} ${options ? JSON.stringify({alphabet: options.alphabet, lastChunkHandling: options.lastChunkHandling}) : "(no options)"}`;
}

for (let input of inputs) {
    for (let options of optionsList) {
        const message = describe(input, options);

        shouldBeEmptyResult(new Uint8Array(0).setFromBase64(input, options), message);

        // A zero-length window into a buffer with data on both sides.
        const buffer = new Uint8Array([1, 2, 3, 4]);
        shouldBeEmptyResult(buffer.subarray(2, 2).setFromBase64(input, options), `subarray ${message}`);
        shouldBe(buffer.join(), "1,2,3,4", `subarray ${message}`);

        // A length-tracking view whose resizable buffer has been shrunk to nothing.
        const resizable = new ArrayBuffer(4, {maxByteLength: 8});
        const tracking = new Uint8Array(resizable);
        resizable.resize(0);
        shouldBeEmptyResult(tracking.setFromBase64(input, options), `length-tracking ${message}`);

        // A fixed-length view at the end of a resizable buffer that has grown: still zero length.
        const grown = new ArrayBuffer(2, {maxByteLength: 8});
        const atEnd = new Uint8Array(grown, 2, 0);
        grown.resize(8);
        shouldBeEmptyResult(atEnd.setFromBase64(input, options), `fixed zero-length ${message}`);
    }
}

// The result object is a fresh ordinary object each time.
{
    const first = new Uint8Array(0).setFromBase64("#");
    const second = new Uint8Array(0).setFromBase64("#");
    shouldBe(first === second, false);
    shouldBe(Object.getPrototypeOf(first), Object.prototype);
}

// Everything before FromBase64 still runs, in order, on a zero-length target.
for (let invalid of [undefined, null, false, true, 42, {}, [], Object("#")]) {
    shouldThrow(() => {
        new Uint8Array(0).setFromBase64(invalid);
    }, TypeError, `string argument ${String(invalid)}`);
}

for (let options of [null, false, true, 42, "#"]) {
    shouldThrow(() => {
        new Uint8Array(0).setFromBase64("#", options);
    }, TypeError, `options ${String(options)}`);
}

for (let alphabet of [null, false, 42, "invalid", Object("base64"), {toString() { return "base64"; }}]) {
    shouldThrow(() => {
        new Uint8Array(0).setFromBase64("#", {alphabet});
    }, TypeError, `alphabet ${String(alphabet)}`);
}

for (let lastChunkHandling of [null, false, 42, "invalid", Object("loose"), {toString() { return "loose"; }}]) {
    shouldThrow(() => {
        new Uint8Array(0).setFromBase64("#", {lastChunkHandling});
    }, TypeError, `lastChunkHandling ${String(lastChunkHandling)}`);
}

{
    const calls = [];
    const options = {
        get alphabet() { calls.push("alphabet"); return "base64url"; },
        get lastChunkHandling() { calls.push("lastChunkHandling"); return "strict"; },
    };
    shouldBeEmptyResult(new Uint8Array(0).setFromBase64("#", options));
    shouldBe(calls.join(), "alphabet,lastChunkHandling");
}

{
    class MyError extends Error {}
    shouldThrow(() => {
        new Uint8Array(0).setFromBase64("#", {get alphabet() { throw new MyError(); }});
    }, MyError, "alphabet getter throws");
    shouldThrow(() => {
        new Uint8Array(0).setFromBase64("#", {get lastChunkHandling() { throw new MyError(); }});
    }, MyError, "lastChunkHandling getter throws");
}

shouldThrow(() => {
    const uint8array = new Uint8Array(0);
    $.detachArrayBuffer(uint8array.buffer);
    uint8array.setFromBase64("#");
}, TypeError, "detached before the call");

shouldThrow(() => {
    const uint8array = new Uint8Array(0);
    uint8array.setFromBase64("#", {
        get lastChunkHandling() {
            $.detachArrayBuffer(uint8array.buffer);
            return undefined;
        },
    });
}, TypeError, "detached by an options getter");

shouldThrow(() => {
    const resizable = new ArrayBuffer(4, {maxByteLength: 8});
    const view = new Uint8Array(resizable, 4, 0);
    resizable.resize(2);
    view.setFromBase64("#");
}, TypeError, "zero-length view that went out of bounds");

// Once there is room, the string is scanned again.
shouldThrow(() => {
    new Uint8Array(1).setFromBase64("#");
}, SyntaxError, "one byte of room, invalid character");
shouldThrow(() => {
    new Uint8Array(1).setFromBase64("a", {lastChunkHandling: "strict"});
}, SyntaxError, "one byte of room, strict partial chunk");
{
    const target = new Uint8Array(1);
    const result = target.setFromBase64("  ");
    shouldBe(result.read, 2, "one byte of room, whitespace only");
    shouldBe(result.written, 0, "one byte of room, whitespace only");
}
{
    const tracking = new Uint8Array(new ArrayBuffer(0, {maxByteLength: 8}));
    shouldBeEmptyResult(tracking.setFromBase64("#"), "length-tracking, empty");
    tracking.buffer.resize(1);
    shouldThrow(() => {
        tracking.setFromBase64("#");
    }, SyntaxError, "length-tracking, grown to one byte");
    const result = tracking.setFromBase64("/w==");
    shouldBe(result.read, 4, "length-tracking, grown to one byte");
    shouldBe(result.written, 1, "length-tracking, grown to one byte");
    shouldBe(tracking[0], 255, "length-tracking, grown to one byte");
}

// Uint8Array.fromBase64 has no maxLength: an invalid string is still rejected even when
// it could not have produced any bytes.
for (let input of ["#", "a", "=", "a#", "!!!"]) {
    shouldThrow(() => {
        Uint8Array.fromBase64(input);
    }, SyntaxError, `Uint8Array.fromBase64(${JSON.stringify(input)})`);
}
shouldBe(Uint8Array.fromBase64("").length, 0);
shouldBe(Uint8Array.fromBase64("  ").length, 0);
