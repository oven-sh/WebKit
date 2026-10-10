import * as data from "./data.json" with { type: "json" };
export function readDefault() { return data.default; }
export function namespace() { return data; }
