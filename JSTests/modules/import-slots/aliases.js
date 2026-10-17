import { count as one, count as two, increment, increment as again } from "./values.js";
import { count as three } from "./values.js";
import { total } from "./reexport-named.js";
export function read() { return [one, two, three, total, increment === again].join(); }
export { increment };
