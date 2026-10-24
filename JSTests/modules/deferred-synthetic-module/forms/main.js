import lib, { a, b } from "./lib.deferred.js";
import * as all from "./lib.deferred.js";
import { a as starA, own } from "./star.js";
import * as starAll from "./star.js";
import { renamed, theDefault } from "./indirect.js";
import { ns } from "./namespace.js";

export const result = JSON.stringify({
    a, b,
    lib: Object.keys(lib),
    all: Object.keys(all),
    starA, own,
    starAll: Object.keys(starAll),
    renamed,
    sameDefault: theDefault === lib && all.default === lib,
    ns: Object.keys(ns),
    sameNamespace: ns === all,
});
