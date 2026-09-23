export function plain() { return current(); }
export function callsBack(callback) { return [current(), callback(), current()]; }
