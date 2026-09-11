import { a } from "./cycle-a.js"

export function b() { return "b" + a(); }
export const early = a();
