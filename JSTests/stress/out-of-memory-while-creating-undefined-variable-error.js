//@ memoryHog!

let exception;
try {
    eval("a".repeat(2147483640));
} catch (e) {
    exception = e;
}

if (!(exception instanceof ReferenceError))
    throw "FAILED: expected ReferenceError, got " + exception;

if (exception.message != "Variable is not defined")
    throw "FAILED: " + exception.message;
