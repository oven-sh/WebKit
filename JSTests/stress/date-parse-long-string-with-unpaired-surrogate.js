//@ memoryHog!
//@ skip if $addressBits <= 32 or $buildType == "debug"
//@ slow!
//@ runDefault

// Date.parse() converts its argument to UTF-8. This string has an unpaired surrogate, and its UTF-8 form is
// 1,431,655,768 bytes. The conversion measured the string, and grew a Vector of the measured size by 1.5x. That
// is more than a Vector can hold, and the process aborted. Needs 6 GB.
// The surrogate is not the last code unit: simdutf's arm64 kernel can count 4 bytes for an unpaired surrogate at
// the very end, and then the Vector does not grow.

const string = "\u65e5".repeat(477218588) + "\ud800a";
const result = Date.parse(string);
if (result === result)
    throw new Error("Date.parse() returned " + result);
