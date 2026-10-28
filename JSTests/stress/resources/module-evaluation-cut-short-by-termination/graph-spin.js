globalThis.cutShort.log.push("graph-spin");
Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, globalThis.cutShort.wait);
globalThis.cutShort.log.push("graph-spin: the wait ended");
