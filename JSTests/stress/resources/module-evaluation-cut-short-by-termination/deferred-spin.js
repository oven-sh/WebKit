globalThis.cutShort.log.push("deferred-spin");
export const before = 1;
Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, globalThis.cutShort.wait);
globalThis.cutShort.log.push("deferred-spin: the wait ended");
export const after = 2;
