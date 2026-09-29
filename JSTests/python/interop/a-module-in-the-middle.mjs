// It shows itself to Python when it is half way through.
import * as self from "./a-module-in-the-middle.mjs";
import m from "./a-module-is-a-module.py";
export const before = 1;
export const seen = m.in_the_middle(self);
export const after = 2;
export let later;
export function hoisted() { return "a function is there from the start"; }
