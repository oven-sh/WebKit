import { b, bCallsA, results as bResults } from "./cycle-b.js";
export const results = [];
// cycle-b.js has not been evaluated when this runs first, or it has and then a() was already called from there.
results.push(typeof b, b());
results.push(bCallsA());
export function a() { return "a" + aPrivate(); }
function aPrivate() { return "!"; }
export { bResults };
