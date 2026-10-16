// JSGlobalObject::setStackTraceLimitOverride() takes the place of Error.stackTraceLimit, which it neither reads nor writes.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function frames(error) {
    return error.stack ? error.stack.split("\n").length : 0;
}

function deep(depth, create) {
    return depth ? deep(depth - 1, create) : create();
}

const creators = [
    () => new Error("message"),
    () => new TypeError("message"),
    () => { try { null.property; } catch (error) { return error; } },
    () => { let object = { }; Error.captureStackTrace(object); return object; },
];

for (let makeErrorImmutable of [false, true]) {
    if (makeErrorImmutable)
        $vm.makePropertiesImmutable(Error);
    let limit = Error.stackTraceLimit;
    for (let create of creators) {
        shouldBe(frames(deep(200, create)), limit);
        shouldBe($vm.setStackTraceLimitOverride(3), undefined);
        shouldBe(frames(deep(200, create)), 3);
        shouldBe($vm.setStackTraceLimitOverride(0), 3);
        shouldBe(frames(deep(200, create)), 0);
        shouldBe(Error.stackTraceLimit, limit);
        shouldBe($vm.setStackTraceLimitOverride(limit + 5), 0);
        shouldBe(frames(deep(200, create)), limit + 5);
        shouldBe($vm.setStackTraceLimitOverride(undefined), limit + 5);
        shouldBe(frames(deep(200, create)), limit);
    }
}

// It belongs to one global object.
{
    let other = $vm.createGlobalObject();
    $vm.setStackTraceLimitOverride(0);
    shouldBe(frames(deep(200, () => new other.Error("message"))), other.Error.stackTraceLimit);
    shouldBe(frames(deep(200, () => new Error("message"))), 0);
    $vm.setStackTraceLimitOverride(undefined);
}
