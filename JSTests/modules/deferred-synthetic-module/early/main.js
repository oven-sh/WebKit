import "./first.js";
import lib, { x } from "./lib.deferred.js";
import * as ns from "./lib.deferred.js";
import { x as barrelX } from "./barrel.js";

export function peek() { return [typeof x, typeof lib, Object.keys(ns).join()].join("|"); }
export function viaBarrel() { return typeof barrelX; }
globalThis.earlyLog.push("main:" + peek() + ":" + viaBarrel());
