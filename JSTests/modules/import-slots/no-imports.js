export let hits = 0;
export function hit() { return ++hits; }
export function read() { return String(hits); }
export const increment = hit;
