//@ memoryHog!
//@ skip if $buildType == "debug" or $addressBits <= 32
//@ runDefault

// unescape() reserves an 8-bit StringBuilder buffer of the input length, and the first %uXXXX converts
// that buffer to 16-bit. Twice the input length is more than the longest 16-bit string here. The result
// is a sixth of the input length, so this must not throw an out of memory error.
const count = 178956971;
const input = "%u1234".repeat(count);
if (input.length <= 2 ** 30)
    throw new Error("input is too short: " + input.length);

const output = unescape(input);
if (output.length !== count)
    throw new Error("bad length: " + output.length);
if (output.charCodeAt(0) !== 0x1234 || output.charCodeAt(count - 1) !== 0x1234)
    throw new Error("bad content");
