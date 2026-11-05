//@ memoryHog!
//@ if $buildType == "release" then runDefault else skip end

var input = '\u0100'.repeat(2**30);
var result;
var exception;
try {
    result = unescape(input);
} catch (e) {
    exception = e;
}

// Bun: upstream expects exception == "RangeError: Out of memory". There a StringBuilder whose capacity is 2**30 asks for String::MaxLength characters next, which no buffer of 16-bit characters can have. Here it asks for as many as one can have (maxCapacity in wtf/text/StringBuilder.cpp).
if (exception !== undefined || result !== input)
    throw "FAILED";

// Bun: the rest is the fork's own, and is what upstream's test is for: a StringBuilder that overflows in unescape(). The last character makes 16-bit characters of 2**31 - 7 others, more than a string can have. Upstream throws the same.
input = result = undefined;
try {
    unescape('a'.repeat(2**31 - 7) + '%u0100');
} catch (e) {
    exception = e;
}

if (exception != "RangeError: Out of memory")
    throw "FAILED";
