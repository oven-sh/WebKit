//@ runDefault("--compileMainScriptAheadOfTime=1")
// The prototypes of plain functions all start out with the same structure, and some go on to have more in them.
function Other() { }
Other.prototype.method = function () { return 1; };

function Point(x, y, z) { this.x = x; this.y = y; this.z = z; }
function make(i) { return new Point(i, i + 1, i + 2); }
function absent(o) { return o.nothingOfTheKind; }
function inherited(o) { return o.constructor; }
function add(o, v) { o.added = v; return o; }
noInline(make); noInline(absent); noInline(inherited); noInline(add);

for (let i = 0; i < 2000; ++i) {
    let p = make(i);
    if (p.x !== i || p.y !== i + 1 || p.z !== i + 2 || absent(p) !== undefined || inherited(p) !== Point || add(p, i).added !== i)
        throw new Error("before, at " + i);
}

// Now the prototype has a say about all of it.
let stored = [];
Object.defineProperty(Point.prototype, "y", { set(v) { stored.push(v); }, get() { return "from the accessor"; }, configurable: true });
Object.defineProperty(Point.prototype, "added", { value: "read only", writable: false, configurable: true });
Point.prototype.nothingOfTheKind = "something";
Point.prototype.constructor = Other;
for (let i = 0; i < 2000; ++i) {
    let p = make(i);
    if (p.x !== i || p.y !== "from the accessor" || Object.hasOwn(p, "y") || p.z !== i + 2 || absent(p) !== "something" || inherited(p) !== Other)
        throw new Error("after, at " + i);
    if (add(p, i).added !== "read only" || Object.hasOwn(p, "added"))
        throw new Error("read only, at " + i);
}
if (stored.length !== 2000 || stored[1999] !== 2000)
    throw new Error("the setter was called " + stored.length + " times");

delete Point.prototype.y;
delete Point.prototype.added;
for (let i = 0; i < 2000; ++i) {
    let p = make(i);
    if (p.y !== i + 1 || !Object.hasOwn(p, "y") || add(p, i).added !== i)
        throw new Error("at the end, at " + i);
}
