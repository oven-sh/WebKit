globalThis.cutShort.log.push("tla-spin");
Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, globalThis.cutShort.wait);
globalThis.cutShort.log.push("tla-spin: the wait ended");
await 0;
globalThis.cutShort.log.push("tla-spin resumed");
