//@ if $buildType == "debug" then runDefault("--maxSingleAllocationSize=1048576") else skip end

// With this limit on the size of one allocation, the 8-bit input fits and the 16-bit result does not.
var exception;
try {
    unescape('%u0100' + 'a'.repeat(600000));
} catch (e) {
    exception = e;
}

if (exception != "RangeError: Out of memory")
    throw "FAILED";
