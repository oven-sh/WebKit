import defer * as inner from "./nested-inner.js";
globalThis.cutShort.log.push("nested-outer");
inner.before;
globalThis.cutShort.log.push("nested-outer resumed");
