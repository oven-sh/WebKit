import defer * as deferred from "./deferred.js";
import defer * as throws from "./deferred-throws.js";
export function deferredNamespace() { return deferred; }
export function throwingNamespace() { return throws; }
