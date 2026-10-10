//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function thrownBy(f, ...args) {
    try {
        f(...args);
    } catch (error) {
        return error instanceof Error ? error.constructor.name : "value " + String(error);
    }
    return "nothing";
}
const other = createGlobalObject();
let log = [];
function logged() { let text = log.join(); log = []; return text; }

function stores(o, value) { o.x = value; }
function storesStrictly(o, value) { "use strict"; o.x = value; }
function storesAndReads(o, value) { o.x = value; return o.x; }
function storesTwice(o, a, b) { o.x = a; o.x = b; }
function storesAsExpression(o, value) { return o.x = value; }
function storesToTwo(o, p, value) { o.x = value; p.x = value; }
function definesInLiteral(value) { return { x: value }; }
for (let f of [stores, storesStrictly, storesAndReads, storesTwice, storesAsExpression, storesToTwo, definesInLiteral])
    noInline(f);
const warmUp = 300;

class Point {
    constructor() { this.held = "nothing"; }
    get x() { return "got " + this.held; }
    set x(value) { log.push("set " + String(value)); this.held = value; }
}
class Point3D extends Point { }
class Overriding extends Point { set x(value) { log.push("overriding " + value); } }
class CallsSuper extends Point { set x(value) { log.push("before"); super.x = value; log.push("after"); } get x() { return super.x; } }
let point = new Point(), point3D = new Point3D(), overriding = new Overriding(), callsSuper = new CallsSuper();
for (let i = 0; i < warmUp; ++i) {
    stores(point, i);
    check(logged(), "set " + i, "a setter");
    check(point.held, i, "what a setter has stored");
    storesStrictly(point3D, i);
    check(logged(), "set " + i, "an inherited setter");
    check(storesAndReads(point, i), "got " + i, "a setter and a getter");
    check(logged(), "set " + i, "a setter and a getter");
    storesTwice(point, i, -i);
    check(logged(), "set " + i + ",set " + -i, "a setter called twice");
    check(storesAsExpression(point, i), i, "the value of an assignment that calls a setter");
    logged();
    stores(overriding, i);
    check(logged(), "overriding " + i, "a setter that overrides another");
    stores(callsSuper, i);
    check(logged(), "before,set " + i + ",after", "a setter that calls the inherited one");
    storesToTwo(point, overriding, i);
    check(logged(), "set " + i + ",overriding " + i, "two receivers");
    check(definesInLiteral(i).x, i, "a property of an object literal");
}
for (let value of [undefined, null, 1.5, "text", {}, Symbol.iterator, 7n]) {
    stores(point, value);
    check(point.held, value, "a value passed to a setter");
    log = [];
}

class Throws { set x(value) { if (value < 0) throw new RangeError("negative"); if (value === 0) throw "zero"; log.push(value); } }
let throws = new Throws();
function storesAndCatches(o, value) { try { o.x = value; return "stored"; } catch (error) { return "caught " + (error instanceof Error ? error.constructor.name : error); } }
noInline(storesAndCatches);
for (let i = 1; i < warmUp; ++i) {
    check(storesAndCatches(throws, i), "stored", "a setter that does not throw");
    check(storesAndCatches(throws, -i), "caught RangeError", "a setter that throws");
    check(storesAndCatches(throws, 0), "caught zero", "a setter that throws a string");
    check(thrownBy(stores, throws, -i), "RangeError", "a setter that throws to the caller");
    logged();
}
let stack = "";
function failing() { throw new Error("in a setter"); }
class Fails { set x(value) { failing(); } }
noInline(failing);
let fails = new Fails();
for (let i = 0; i < warmUp; ++i) {
    try {
        stores(fails, i);
    } catch (error) {
        stack = error.stack;
    }
    check(stack.split("\n").map(line => line.split("@")[0]).join(), "failing,x,stores,global code", "the stack of an error thrown below a setter");
}

function fresh() {
    class Base { set x(value) { log.push("base " + value); } }
    class Middle extends Base { }
    class Leaf extends Middle { }
    let leaf = new Leaf();
    for (let i = 0; i < warmUp; ++i)
        stores(leaf, i);
    log = [];
    return { Base, Middle, Leaf, leaf };
}
{
    let { Base, leaf } = fresh();
    Object.defineProperty(Base.prototype, "x", { set(value) { log.push("redefined " + value); }, configurable: true });
    stores(leaf, 1);
    check(logged(), "redefined 1", "a setter that has been redefined");
}
{
    let { Base, leaf } = fresh();
    Object.defineProperty(Base.prototype, "x", { value: "data", writable: true, configurable: true });
    stores(leaf, 1);
    check(logged(), "", "a setter that has been replaced by a data property");
    check(Object.getOwnPropertyDescriptor(leaf, "x").value, 1, "the own property made in place of calling the setter");
}
{
    let { Base, leaf } = fresh();
    Object.defineProperty(Base.prototype, "x", { value: "data", writable: false, configurable: true });
    stores(leaf, 1);
    check(leaf.x, "data", "a store to an inherited property that is read-only");
    check(thrownBy(storesStrictly, leaf, 1), "TypeError", "a strict store to an inherited property that is read-only");
}
{
    let { Base, leaf } = fresh();
    delete Base.prototype.x;
    stores(leaf, 1);
    check(logged(), "", "a setter that has been deleted");
    check(leaf.x, 1, "the own property made after the setter was deleted");
}
{
    let { Base, leaf } = fresh();
    Object.defineProperty(Base.prototype, "x", { get() { return "getter only"; }, set: undefined, configurable: true });
    stores(leaf, 1);
    check(leaf.x, "getter only", "a store to an accessor without setter");
    check(thrownBy(storesStrictly, leaf, 1), "TypeError", "a strict store to an accessor without setter");
}
{
    let { Middle, leaf } = fresh();
    Object.defineProperty(Middle.prototype, "x", { set(value) { log.push("middle " + value); }, configurable: true });
    stores(leaf, 1);
    check(logged(), "middle 1", "a setter added nearer to the receiver");
}
{
    let { Middle, leaf } = fresh();
    Middle.prototype.y = 1;
    stores(leaf, 1);
    check(logged(), "base 1", "another property added to a prototype");
    Object.defineProperty(Middle.prototype, "x", { value: "data", writable: true });
    stores(leaf, 2);
    check(logged(), "", "a data property added nearer to the receiver");
    check(Object.getOwnPropertyDescriptor(leaf, "x").value, 2, "the own property made then");
}
{
    let { Leaf, leaf } = fresh();
    Object.setPrototypeOf(Leaf.prototype, { set x(value) { log.push("another chain " + value); } });
    stores(leaf, 1);
    check(logged(), "another chain 1", "a prototype whose prototype has changed");
}
{
    let { leaf } = fresh();
    Object.setPrototypeOf(leaf, { set x(value) { log.push("another prototype " + value); } });
    stores(leaf, 1);
    check(logged(), "another prototype 1", "a receiver whose prototype has changed");
    Object.setPrototypeOf(leaf, null);
    stores(leaf, 2);
    check(logged(), "", "a receiver without prototype");
    check(leaf.x, 2, "the own property of a receiver without prototype");
}
{
    let { leaf } = fresh();
    Object.defineProperty(leaf, "x", { value: "own", writable: true });
    stores(leaf, 1);
    check(logged(), "", "a receiver with an own data property");
    check(leaf.x, 1, "the own data property");
}
{
    let { leaf } = fresh();
    Object.defineProperty(leaf, "x", { set(value) { log.push("own " + value); } });
    stores(leaf, 1);
    check(logged(), "own 1", "a receiver with an own setter");
}
{
    let { Leaf, leaf } = fresh();
    leaf.unrelated = 1;
    stores(leaf, 1);
    check(logged(), "base 1", "a receiver with another structure");
    stores(new Leaf(), 2);
    check(logged(), "base 2", "another receiver with the first structure");
}
{
    let { leaf } = fresh();
    Object.freeze(leaf);
    stores(leaf, 1);
    check(logged(), "base 1", "a frozen receiver");
}
{
    let { Leaf, leaf } = fresh();
    Object.setPrototypeOf(Leaf.prototype, new Proxy({}, { set(target, name, value) { log.push("trap " + String(name) + " " + value); return true; } }));
    stores(leaf, 1);
    check(logged(), "trap x 1", "a proxy in the prototype chain");
}
{
    let count = 0;
    class Redefines {
        set x(value) {
            log.push("first " + value);
            if (++count == warmUp)
                Object.defineProperty(Redefines.prototype, "x", { set(value) { log.push("second " + value); }, configurable: true });
        }
    }
    let redefines = new Redefines();
    for (let i = 0; i < warmUp; ++i)
        stores(redefines, i);
    log = [];
    stores(redefines, "later");
    check(logged(), "second later", "a setter that has redefined itself");
}
{
    let count = 0;
    class Shadows {
        set x(value) {
            log.push("setter " + value);
            if (++count == warmUp)
                Object.defineProperty(this, "x", { value: "own", writable: true });
        }
    }
    let shadows = new Shadows();
    for (let i = 0; i < warmUp; ++i)
        stores(shadows, i);
    log = [];
    stores(shadows, "later");
    check(logged(), "", "a setter that has shadowed itself");
    check(shadows.x, "later", "the property that shadows the setter");
}

let bound = Object.create({});
Object.defineProperty(Object.getPrototypeOf(bound), "x", { set: function (prefix, value) { log.push(prefix + value + (String(this) === "bound this")); }.bind("bound this", "bound ") });
let native = Object.create(Object.defineProperty({}, "x", { set: Array.prototype.push }));
native.length = 0;
let proxied = Object.create(Object.defineProperty({}, "x", { set: new Proxy(function (value) { log.push("target " + value); }, {}) }));
let foreign = Object.create(other.eval("({ set x(value) { this.received = [value]; } })"));
let sloppyThis = Object.create({ set x(value) { log.push(typeof this + " " + value); } });
for (let i = 0; i < warmUp; ++i) {
    stores(bound, i);
    check(logged(), "bound " + i + "true", "a bound function as setter");
    stores(native, i);
    check(native[i], i, "a host function as setter");
    stores(proxied, i);
    check(logged(), "target " + i, "a proxy as setter");
    stores(foreign, i);
    check(foreign.received[0], i, "a setter of another realm");
    check(foreign.received instanceof other.Array, true, "the realm of a setter of another realm");
    stores(sloppyThis, i);
    check(logged(), "object " + i, "the receiver of a setter");
}
check(native.length, warmUp, "a host function as setter");

let objects = [];
class Keeps { set x(value) { this.kept = value; } }
for (let round = 0; round < 4; ++round) {
    let keeps = new Keeps();
    for (let i = 0; i < warmUp; ++i)
        stores(keeps, { round, i });
    objects.push(keeps);
    gc();
    stores(keeps, { round, i: "after" });
    check(keeps.kept.i, "after", "a setter after a collection");
}
check(objects.map(o => o.kept.round).join(), "0,1,2,3", "what setters have stored");
for (let round = 0; round < 4; ++round) {
    (function () {
        class Temporary { set x(value) { log.push("temporary " + round + " " + value); } }
        let temporary = new Temporary();
        for (let i = 0; i < warmUp; ++i)
            stores(temporary, i);
        log = [];
    })();
    gc();
    stores(point, round);
    check(logged(), "set " + round, "a setter after other classes have died");
}
for (let notObject of [1, "text", true, Symbol(), 1n]) {
    stores(notObject, 1);
    check(thrownBy(storesStrictly, notObject, 1), "TypeError", "a strict store to a " + typeof notObject);
}
check(thrownBy(stores, undefined, 1), "TypeError", "a store to undefined");
check(thrownBy(stores, null, 1), "TypeError", "a store to null");
