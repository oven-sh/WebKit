import { readLet, readClass, readFunction } from "./tdz-user.js";
export const results = [];
for (const read of [readLet, readClass]) {
    try {
        read();
        results.push("no error");
    } catch (e) {
        results.push(String(e));
    }
}
results.push(readFunction()());
export let letBinding = 1;
export class ClassBinding { }
export function functionBinding() { return "function"; }
