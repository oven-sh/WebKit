import * as values from "./values.js";
import { count, increment } from "./values.js";
import { first } from "./second.js";
export function read() { return [count, values.count, first, values.increment === increment].join(); }
export { increment };
