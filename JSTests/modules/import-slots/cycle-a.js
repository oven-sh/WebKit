import { b, bValue, readA } from "./cycle-b.js";
export let aValue = "a";
export function a() { return "a()"; }
export const sawFromB = b();
export const bValueSeen = bValue;
export const viaB = readA();
export function setA(value) { aValue = value; }
