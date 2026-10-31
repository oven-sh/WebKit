globalThis.cutShort.log.push("nested-inner");
export const before = 1;
Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, globalThis.cutShort.wait);
globalThis.cutShort.log.push("nested-inner: the wait ended");
