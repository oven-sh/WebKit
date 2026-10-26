struct P { x; constructor(x) { this.x = x; } }
let p = new P(5);
if (p.x !== 5) throw new Error("ctor");
