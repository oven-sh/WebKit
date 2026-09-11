import { letBinding, ClassBinding, functionBinding } from "./tdz.js";
export function readLet() { return letBinding; }
export function readClass() { return ClassBinding; }
export function readFunction() { return functionBinding; }
