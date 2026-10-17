import * as values from "./values.js";
import * as second from "./second.js";
export function read() { return [values.count, second.first].join(); }
export function increment() { return values.increment(); }
