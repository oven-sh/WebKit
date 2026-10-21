//@ if $buildType == "debug" then runDefault("--maxSingleAllocationSize=1048576") else skip end

// With this limit on the size of one allocation, the input fits and the result does not. Each "^" becomes "\^".
var exception;
try {
    RegExp.escape('퀀' + '^'.repeat?.(300000));
} catch (e) {
    exception = e;
}
if (exception != 'RangeError: Out of memory')
  throw 'FAILED';
