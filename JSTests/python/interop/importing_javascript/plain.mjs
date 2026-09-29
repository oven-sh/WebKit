import * as self from "./plain.mjs";
globalThis.runs = (globalThis.runs || 0) + 1;
export let count = 0;
export function bump() { return ++count; }
export default function hello(name) { return "hello " + name; }
export class Thing { constructor(x) { this.x = x; } }
export const isMe = other => other === self;
export const _hidden = 1;
