//@ requireOptions("--definePlainInstanceFieldsInConstructor=1", "--useSoundTypes=1")
function shouldBe(actual, expected) {
    if (!Object.is(actual, expected))
        throw new Error("bad value: " + actual + ", expected " + expected);
}
const show = o => JSON.stringify(o) + "/" + Object.keys(o).join();

class Fields { left; right; constructor(l, r) { this.left = l; this.right = r; } }
class NoFields { constructor(a, b) { this.a = a; this.b = b; this.c = 3; } }
class OnlyFields { x; y; z; }
class Some { a; b; c; constructor(v) { this.b = v; } }
class Backwards { a; b; constructor(x, y) { this.b = y; this.a = x; } }
class Twice { a; constructor(x) { this.a = x; this.a = x + 1; } }
class Checked { n; s; constructor(n, s) { $$t(n, 8); $$t(s, 16); this.n = $$t(n, 8); this.s = $$t(s, 16); } }
class Escapes { a; b; constructor(x, log) { this.a = x; log.push(show(this)); this.b = x; } }
class Computes { a; b; constructor(x) { this.a = x * 2; this.b = [x]; this.c = this.a + 1; } }
class Initialized { a = 1; b; constructor(x) { this.b = x; } }
class Private { #p; q; constructor(x) { this.#p = x; this.q = x; } get p() { return this.#p; } }
function Plain(x, y) { this.x = x; this.y = y; }

for (let i = 0; i < 200; i++) {
    shouldBe(show(new Fields(i, "r")), `{"left":${i},"right":"r"}/left,right`);
    shouldBe(show(new NoFields(i, "b")), `{"a":${i},"b":"b","c":3}/a,b,c`);
    shouldBe(show(new OnlyFields), "{}/x,y,z");
    shouldBe(show(new Some(i)), `{"b":${i}}/a,b,c`);
    shouldBe(show(new Backwards(1, 2)), '{"a":1,"b":2}/a,b');
    shouldBe(show(new Twice(i)), `{"a":${i + 1}}/a`);
    shouldBe(show(new Checked(i, "s")), `{"n":${i},"s":"s"}/n,s`);
    const log = [];
    shouldBe(show(new Escapes(i, log)), `{"a":${i},"b":${i}}/a,b`);
    shouldBe(log[0], `{"a":${i}}/a,b`);
    shouldBe(show(new Computes(i)), `{"a":${i * 2},"b":[${i}],"c":${i * 2 + 1}}/a,b,c`);
    shouldBe(show(new Initialized(i)), `{"a":1,"b":${i}}/a,b`);
    shouldBe(new Private(i).p + new Private(i).q, 2 * i);
    shouldBe(show(new Plain(i, 2)), `{"x":${i},"y":2}/x,y`);
    shouldBe(Object.getPrototypeOf(new Fields(1, 2)), Fields.prototype);
    shouldBe(new Fields(1, 2) instanceof Fields, true);
}
const d = Object.getOwnPropertyDescriptor(new Fields(1, 2), "left");
shouldBe(d.writable && d.enumerable && d.configurable, true);

let error;
try { new Checked(1, 2); } catch (e) { error = e; }
shouldBe(String(error), "TypeError: Type check failed: expected string, got number");

class Derived extends Fields { extra; constructor(l, r) { super(l, r); this.extra = l + r; } }
for (let i = 0; i < 50; i++) {
    const o = new Derived(i, 1);
    shouldBe(show(o), `{"left":${i},"right":1,"extra":${i + 1}}/left,right,extra`);
    shouldBe(Object.getPrototypeOf(o), Derived.prototype);
    shouldBe(Object.getPrototypeOf(Reflect.construct(Fields, [1, 2], Plain)), Plain.prototype);
    shouldBe(show(new Fields(i, 2)), `{"left":${i},"right":2}/left,right`);
}
function bound() { }
const proxied = new Proxy(bound, { get(target, key) { return key === "prototype" ? NoFields.prototype : target[key]; } });
shouldBe(Object.getPrototypeOf(Reflect.construct(Fields, [1, 2], proxied)), NoFields.prototype);

const before = Plain.prototype;
Plain.prototype = { marker: 1 };
shouldBe(new Plain(1, 2).marker, 1);
shouldBe(show(new Plain(1, 2)), '{"x":1,"y":2}/x,y');
Plain.prototype = before;
shouldBe(new Plain(1, 2).marker, undefined);
shouldBe(Object.getPrototypeOf(new Plain(1, 2)), before);

let calls = [];
Object.defineProperty(NoFields.prototype, "b", { set(v) { calls.push(v); }, configurable: true });
shouldBe(show(new NoFields(1, 2)), '{"a":1,"c":3}/a,c');
shouldBe(calls.join(), "2");
delete NoFields.prototype.b;
shouldBe(show(new NoFields(1, 2)), '{"a":1,"b":2,"c":3}/a,b,c');
Object.defineProperty(Object.prototype, "y", { set(v) { calls.push("y" + v); }, configurable: true });
shouldBe(show(new Plain(1, 2)), '{"x":1}/x');
shouldBe(calls.join(), "2,y2");
delete Object.prototype.y;
shouldBe(show(new Plain(1, 2)), '{"x":1,"y":2}/x,y');
Object.defineProperty(Fields.prototype, "left", { set(v) { throw new Error("a setter was called"); }, configurable: true });
shouldBe(show(new Fields(1, 2)), '{"left":1,"right":2}/left,right');
delete Fields.prototype.left;
Object.defineProperty(Object.prototype, "a", { value: 0, writable: false, configurable: true });
error = undefined;
try { new NoFields(1, 2); } catch (e) { error = e; }
shouldBe(error instanceof TypeError, true);
delete Object.prototype.a;
shouldBe(show(new NoFields(1, 2)), '{"a":1,"b":2,"c":3}/a,b,c');

function tree(depth) { return depth ? new Fields(tree(depth - 1), tree(depth - 1)) : new Fields(null, null); }
function count(t) { return t.left === null ? 1 : 1 + count(t.left) + count(t.right); }
shouldBe(count(tree(14)), 32767);

const Sloppy = new Function("a", "b", "this.a = a; this.b = b;");
shouldBe(show(new Sloppy(1, 2)), '{"a":1,"b":2}/a,b');
shouldBe(show(new Sloppy(1, 2)), '{"a":1,"b":2}/a,b');
Object.defineProperty(Sloppy.prototype, "a", { value: 5, writable: false, configurable: true });
shouldBe(show(new Sloppy(1, 2)), '{"b":2}/b');
shouldBe(new Sloppy(1, 2).a, 5);
delete Sloppy.prototype.a;
shouldBe(show(new Sloppy(1, 2)), '{"a":1,"b":2}/a,b');
