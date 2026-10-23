import { onlyA, onlyLib } from "./barrel.js";
import * as ns from "./barrel.js";
export const result = [onlyA, onlyLib, Object.keys(ns).join()].join("|");
