import { count as first, count as second } from "./values.js";
export function one() { if (first !== second) throw new Error("aliases differ"); return first; }
