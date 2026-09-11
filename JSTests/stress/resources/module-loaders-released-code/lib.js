let calls = 0;
export function early(a) { ++calls; return helper(a) + 1; }
export function late(a) { ++calls; return a * 2; }
export function neverReadByTheFirstLoader(a) { ++calls; return a * 3; }
export function callCount() { return calls; }
function helper(a) { return a * 10; }
export const marker = early(1);
