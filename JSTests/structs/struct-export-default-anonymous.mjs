import * as self from "./struct-export-default-anonymous.mjs";

function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }

struct Base { a; }
export default struct extends Base { b = 2; }

const p = new self.default();
assert(self.default.name === "default", "an anonymous default struct is named default: " + self.default.name);
assert(Object.keys(p).join() === "a,b" && p.b === 2 && Object.isSealed(p), "an anonymous default struct with a heritage");
assert(p instanceof Base, "the parent");
assert(String(self.default) === "struct extends Base { b = 2; }", "the source text: " + String(self.default));
