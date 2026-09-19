function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual + ' expected: ' + expected);
}

// The result of each string access is only compared against a constant. The abstract
// interpreter folds the compare, so the access node has no users and DCE removes it. The
// in-bounds speculation must survive that removal.

function codePointAtIsUndefined(string, index) {
    return string.codePointAt(index) === undefined;
}
noInline(codePointAtIsUndefined);

function atIsUndefined(string, index) {
    return string.at(index) === undefined;
}
noInline(atIsUndefined);

function charCodeAtIsNaN(string, index) {
    var c = string.charCodeAt(index);
    return c !== c;
}
noInline(charCodeAtIsNaN);

var str8bit = "Hello, World!";
var str16bit = "こんにちは世界";

for (var i = 0; i < testLoopCount; ++i) {
    var index = i % str8bit.length;
    shouldBe(codePointAtIsUndefined(str8bit, index), false);
    shouldBe(atIsUndefined(str8bit, index), false);
    shouldBe(charCodeAtIsNaN(str8bit, index), false);
    index = i % str16bit.length;
    shouldBe(codePointAtIsUndefined(str16bit, index), false);
    shouldBe(atIsUndefined(str16bit, index), false);
    shouldBe(charCodeAtIsNaN(str16bit, index), false);
}

for (var i = 0; i < 10; ++i) {
    shouldBe(codePointAtIsUndefined(str8bit, str8bit.length), true);
    shouldBe(atIsUndefined(str8bit, str8bit.length), true);
    shouldBe(charCodeAtIsNaN(str8bit, str8bit.length), true);
    shouldBe(codePointAtIsUndefined(str16bit, str16bit.length), true);
    shouldBe(atIsUndefined(str16bit, str16bit.length), true);
    shouldBe(charCodeAtIsNaN(str16bit, str16bit.length), true);
}
