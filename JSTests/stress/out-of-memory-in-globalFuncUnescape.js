//@ memoryHog!
//@ if $buildType == "release" then runDefault else skip end

// Bun: unescape returns its argument when it has no '%'. Upstream's StringBuilder cannot hold the
// 2^30 16-bit code units and throws "RangeError: Out of memory".
var input = '\u0100'.repeat(2**30);
if (unescape(input) !== input)
    throw "FAILED";

// A result that does not fit in a string: six code units for each of 2^29 code units.
var exception;
try {
    encodeURIComponent('\u00ff'.repeat(2**29));
} catch (e) {
    exception = e;
}

if (exception != "RangeError: Out of memory")
    throw "FAILED";
