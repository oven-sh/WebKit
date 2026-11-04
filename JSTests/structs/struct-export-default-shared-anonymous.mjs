import * as self from "./struct-export-default-shared-anonymous.mjs";

function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }

export default shared struct { x; y = 2; static make() { return new self.default(); } }

const p = self.default.make();
assert(self.default.name === "default", "an anonymous default shared struct is named default: " + self.default.name);
assert(Object.getPrototypeOf(p) === null && Object.keys(p).join() === "x,y" && p.y === 2, "an anonymous default shared struct");
assert(self.default.prototype === null, "no prototype");
assert(String(self.default) === "shared struct { x; y = 2; static make() { return new self.default(); } }", "the source text: " + String(self.default));
