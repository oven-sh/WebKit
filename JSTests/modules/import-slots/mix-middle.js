import { count } from "./values.js";
import * as values from "./values.js";
import * as second from "./second.js";
import { increment } from "./values.js";
import { first } from "./second.js";
export function read() { return [count, values.count, first, second.first, values.increment === increment].join(); }
export { increment };
