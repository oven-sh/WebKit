export let count = 0;
export function increment() { return ++count; }
export function set(value) { count = value; }
export const object = { tag: "object" };
export const constant = 42;
export var variable = "var";
export function declared() { return "declared"; }
export class Klass { static tag = "class"; }
export default function defaultFunction() { return "default"; }
