import { a } from "./cycle-a.js";
export const results = [];
results.push(typeof a, a());
export function b() { return "b" + bPrivate(); }
function bPrivate() { return "?"; }
export function bCallsA() { return a(); }
