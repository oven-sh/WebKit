import data from "./data.json" with { type: "json" };
import { count, increment } from "./values.js";
import * as ns from "./data.json" with { type: "json" };
export function read() { return [data.tag, data.n, count, ns.default === data].join(); }
export { increment, data };
