globalThis.cutShort.log.push("joined-spin");
export const before = 1;
// An import() of this module, and its job, while the module is evaluating.
globalThis.cutShort.join();
Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, globalThis.cutShort.wait);
globalThis.cutShort.log.push("joined-spin: the wait ended");
