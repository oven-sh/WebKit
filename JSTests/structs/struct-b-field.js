struct P { x; y; }
let p = new P();
print(Object.keys(p).join());
if (p.x !== undefined || Object.keys(p).join() !== "x,y") throw new Error("fields");
