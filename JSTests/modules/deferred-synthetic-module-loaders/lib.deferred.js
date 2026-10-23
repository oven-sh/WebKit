const load = globalThis.deferredLoads = (globalThis.deferredLoads ?? 0) + 1;
globalThis.loadersLog.push("lib" + load);
// `x` is at another place among the exports of every loader's record.
for (let i = 1; i < load; ++i)
    exports["padding" + i] = i;
exports.x = "x" + load;
exports.last = load;
