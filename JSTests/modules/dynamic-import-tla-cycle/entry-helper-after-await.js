import { loadAfterAwait } from "./loader.js";
export function helper() { return "helper-after-await"; }
export const ns = await loadAfterAwait("./child-helper-after-await.js");
