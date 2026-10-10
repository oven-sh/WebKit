//@ skip if $memoryLimited or $addressBits <= 32 or $buildType == "debug"
//@ slow!

// Date.parse() converts its argument to UTF-8. This string has an unpaired surrogate, and its UTF-8 form is over
// 1,431,655,764 bytes. The conversion then grew a Vector of that size by 1.5x, which is more than a Vector can
// hold, and the process aborted. Needs 6 GB.
// The character after the surrogate makes the first measured length less than 3 bytes for each code unit on
// every CPU: simdutf's arm64 kernel counts 4 bytes for an unpaired surrogate at the very end.

const string = "\u65e5".repeat(477218588) + "\ud800a";
const result = Date.parse(string);
if (result === result)
    throw new Error("Date.parse() returned " + result);
