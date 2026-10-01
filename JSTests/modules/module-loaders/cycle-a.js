import { c } from "./c.js"
import { b, early } from "./cycle-b.js"

export function a() { return "a" + c; }
export const fromB = b();
export { early };
