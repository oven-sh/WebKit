// The frames of an error's stack trace are memory outside the heap. The collector is told about it, as it is for an exception's.
Error.stackTraceLimit = Infinity;

const depth = 1000;
const count = Math.max(20, Math.min(200, testLoopCount / 100 | 0));
const sizeOfFrame = 32;
const expected = depth * count * sizeOfFrame * 0.5;

function makeErrors(depth, callback) {
    if (!depth)
        return callback();
    const result = makeErrors(depth - 1, callback);
    return result;
}

// The conservative scan of the stack must not find the errors in what the frames of makeErrors() left there.
function clobberStack(depth) {
    const array = [depth, depth, depth, depth, depth, depth];
    return depth ? clobberStack(depth - 1) + array.length : 0;
}
noInline(clobberStack);

function measure(name, make) {
    let errors = [];
    const before = fullGC();
    makeErrors(depth, () => {
        for (let i = 0; i < count; ++i)
            errors.push(make());
    });
    const kept = fullGC() - before;
    if (kept < expected)
        throw new Error(name + ": expected the heap to grow by at least " + expected + ", it grew by " + kept);
    errors = null;
    clobberStack(2 * depth);
    const left = fullGC() - before;
    if (left > expected / 2)
        throw new Error(name + ": expected the heap to shrink again, " + left + " are left");
}

measure("new Error", () => new Error("message"));
measure("thrown by the engine", () => {
    try {
        null.property;
    } catch (error) {
        return error;
    }
});
measure("Error.captureStackTrace", () => {
    const limit = Error.stackTraceLimit;
    Error.stackTraceLimit = 0;
    const error = new Error("message");
    Error.stackTraceLimit = limit;
    Error.captureStackTrace(error);
    return error;
});
