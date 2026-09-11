let depCalls = 0;
export function d0(x) { ++depCalls; return x * 100 + 0; }
export function d1(x) { ++depCalls; return x * 100 + 1; }
export function d2(x) { ++depCalls; return x * 100 + 2; }
export function d3(x) { ++depCalls; return x * 100 + 3; }
export function d4(x) { ++depCalls; return x * 100 + 4; }
export function d5(x) { ++depCalls; return x * 100 + 5; }
export function d6(x) { ++depCalls; return x * 100 + 6; }
export function d7(x) { ++depCalls; return x * 100 + 7; }
export function depCallCount() { return depCalls; }
function neverExportedNeverRead() { return depCalls; }
