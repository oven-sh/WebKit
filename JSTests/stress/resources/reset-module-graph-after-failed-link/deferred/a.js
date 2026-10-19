import defer * as ns from "./s.js";
import "./b.js";

export function read() {
    return Object.keys(ns) + ": " + ns.v;
}
