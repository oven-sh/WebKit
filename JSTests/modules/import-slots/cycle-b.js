import { a, aValue } from "./cycle-a.js";
export function b() { return "b:" + a(); }
export let earlyError = "";
try {
    aValue;
} catch (error) {
    earlyError = String(error);
}
export const bValue = "bValue";
export function readA() { return aValue; }
