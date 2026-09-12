import * as assert from "../assert.js";

const tag = new WebAssembly.Tag({ parameters: ["i32"] });

function makeBoth() {
    return [new WebAssembly.Exception(tag, [0], { traceStack: true }), new Error()];
}

// Both are created on one line, so only the column of the first frame differs.
const withoutFirstColumn = stack => stack.replace(/:\d+$/m, "");

const [exception, error] = makeBoth();
assert.eq(typeof exception.stack, "string");
assert.eq(withoutFirstColumn(exception.stack), withoutFirstColumn(error.stack));
assert.eq(Object.hasOwn(exception, "stack"), false);
