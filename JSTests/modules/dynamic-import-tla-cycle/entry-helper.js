import { load } from "./loader.js";
export function helper() { return "helper"; }
export const ns = await load("./child-helper.js");
