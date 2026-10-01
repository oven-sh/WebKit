import { loadAll } from "./loader.js";
export function helper() { return "all"; }
export const [ns] = await loadAll("./child-all.js");
