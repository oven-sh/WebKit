import { bump, counter, taggedOnce } from "./dep.js";

const table = [];
for (let i = 0; i < 16; ++i)
    table.push({ id: i, weight: (i * 7) % 5 });

export function sum() { return table.reduce((a, e) => a + e.weight, 0); }
export const arrow = (x) => x + table.length;
export class Point {
    static origin = new Point(0, 0);
    constructor(x, y) { this.x = x; this.y = y; }
    get norm() { return Math.hypot(this.x, this.y); }
}
export function* numbers() { for (const e of table) yield e.id; }
export function bumpTwice() { bump(); return bump(); }
export function readCounter() { return counter; }
export function sameTemplateObject() { return taggedOnce() === taggedOnce(); }
export const meta = import.meta;
bump();
