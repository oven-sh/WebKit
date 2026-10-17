import { loadForAwait } from "./loader.js";
export function helper() { return "for-await"; }
export const ns = await loadForAwait("./child-for-await.js");
