export function helper() { return "opaque"; }
let resolve;
const done = new Promise(r => { resolve = r; });
setTimeout(() => { import("./child-opaque.js").then(m => setTimeout(() => resolve(m), 0)); }, 0);
export const ns = await done;
