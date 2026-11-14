import * as self from "./struct-export.mjs";

function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }

export default struct Point { x; y; }
export struct Named { name; }
export shared struct Shared { value; }

const p = new self.default();
assert(self.default.name === "Point", "export default struct keeps its name");
assert(Object.keys(p).join() === "x,y" && p.x === undefined, "default export is a struct");
assert(Object.isSealed(p), "instances are sealed");
assert(new self.Named().name === undefined, "export struct");
assert(Object.getPrototypeOf(new self.Shared()) === null, "export shared struct");
