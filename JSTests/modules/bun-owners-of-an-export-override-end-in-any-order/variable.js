export let a = "real";
export function assign(value) { a = value; }
const shared = "real";
export { shared as one, shared as other };
