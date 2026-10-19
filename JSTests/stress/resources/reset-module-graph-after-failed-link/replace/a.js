import * as ns from "./b.js";

export function read() {
    return Object.keys(ns) + ": " + ns.v;
}
