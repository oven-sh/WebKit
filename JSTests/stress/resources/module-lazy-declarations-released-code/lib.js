let calls = 0;
export function readEarly(a) { ++calls; return helper(a) + 1; }
export function neverReadUntilLate(a) { ++calls; return a * 2 + privateLate(a); }
export function alsoLate(a, b) { ++calls; return [a, b].map((x) => x + 1).join(":"); }
export function* lateGenerator(n) { for (let i = 0; i < n; ++i) yield i * i; }
export async function lateAsync(a) { ++calls; return (await a) + 1; }
export function callCount() { return calls; }
function helper(a) { return a * 10; }
function privateLate(a) { return neverCalledByAnyone === undefined ? -1 : a + 100; }
function neverCalledByAnyone() { return "never"; }
export const marker = readEarly(1);
