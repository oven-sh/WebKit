import "./first.js";
import lib, { x, last } from "./lib.deferred.js";

// Called by first.js before this module and lib.deferred.js have run.
export function read() { return x; }
export function run(n) {
    let length = 0;
    for (let i = 0; i < n; ++i)
        length += x.length + last;
    return length;
}
export function keys() { return Object.keys(lib).join(); }
globalThis.loadersLog.push("main:" + read());
