import { count, increment, object, constant, declared, Klass } from "./values.js";
import * as namespace from "./values.js";
import defaultFunction from "./values.js";
import { first, second } from "./second.js";
export const evaluated = true;
export function neverCalled() { return [count, increment, object, constant, declared, Klass, namespace, defaultFunction, first, second]; }
export function onlySecond() { return second; }
