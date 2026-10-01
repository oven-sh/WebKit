import { count, increment } from "./values.js";
import { first } from "./second.js";
import thirdDefault, * as third from "./third.js";
import * as values from "./values.js";
export function read() { return [count, values.count, first, third.third, thirdDefault === third.default, values.increment === increment].join(); }
export { increment };
