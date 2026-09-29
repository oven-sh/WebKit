import { first, second } from "./second.js";
import { first as thirdFirst } from "./third.js";
import { Map, undefinedLike as NaNLike } from "./globals-named.js";
export function read(first) { return [first, second, thirdFirst].join(); }
export function inner() {
    let second = "local";
    return (() => [first, second].join())();
}
export function globals() { return [typeof Map, Map, NaNLike, typeof globalThis.Map].join(); }
