// len(o.__dict__) is kept count of by the object's table of properties, and is not found by going through them. What Python counts is what Object.keys() would list. So whatever is done to the object, from either
// language, the count is to be what going through them comes to.
import * as m from "./how-many-attributes.py";

let checks = 0;
let wrong = 0;
function check(o, label)
{
    const counts = Array.from(m.counts(o));
    const expected = Object.keys(o).length;
    ++checks;
    if (counts.some(count => count !== expected)) {
        ++wrong;
        print("WRONG", label, "Object.keys() has", expected, "and Python has", counts.join(" "));
    }
}

const symbols = [Symbol("a"), Symbol("b"), Symbol.iterator];
let refused = 0;
// The last is an object of JavaScript's, which Python has a __dict__ for as well. An attribute of an object of Python's cannot be hidden, so it is with that one that the count is put to it.
for (const kind of ["C", "E", "S", "Object"]) {
    for (const many of [0, 3, 40, 200, 2000]) {
        const o = kind === "Object" ? { } : m.make(kind);
        check(o, "new");
        // Past a certain number of them the object keeps its own table, which is changed where it is.
        for (let i = 0; i < many; ++i) {
            if (i % 3 === 0)
                o["js" + i] = i;
            else if (i % 3 === 1)
                m.put(o, "py" + i, i);
            else
                m.by_dict(o, "d" + i, i);
            if (many <= 40 || i % 97 === 0)
                check(o, "added " + i);
        }
        check(o, "filled");

        const steps = [
            ["a symbol", () => { o[symbols[0]] = 1; }],
            ["another", () => { o[symbols[1]] = 2; o[symbols[2]] = 3; }],
            ["one that is not listed", () => Object.defineProperty(o, "hidden", { value: 1, enumerable: false, configurable: true, writable: true })],
            ["is made to be", () => Object.defineProperty(o, "hidden", { enumerable: true })],
            ["and not to be again", () => Object.defineProperty(o, "hidden", { enumerable: false })],
            ["is written to", () => { o.hidden = 2; }],
            ["is deleted", () => { delete o.hidden; }],
            ["one that is listed is made not to be", () => { o.shown = 1; Object.defineProperty(o, "shown", { enumerable: false }); }],
            ["and is deleted", () => { delete o.shown; }],
            ["a getter", () => Object.defineProperty(o, "got", { get() { return 1; }, enumerable: true, configurable: true })],
            ["that is not listed", () => Object.defineProperty(o, "got", { enumerable: false })],
            ["becomes a value", () => Object.defineProperty(o, "got", { value: 1, enumerable: true, writable: true })],
            ["a symbol is deleted", () => { delete o[symbols[0]]; }],
            ["a symbol that is not listed", () => Object.defineProperty(o, symbols[0], { value: 1, enumerable: false, configurable: true })],
            ["is made to be", () => Object.defineProperty(o, symbols[0], { enumerable: true })],
            ["the same again", () => { o.got = 2; m.put(o, "got", 3); m.by_dict(o, "got", 4); }],
            ["deleted each way", () => { o.x1 = o.x2 = o.x3 = 1; delete o.x1; m.drop(o, "x2"); m.drop_by_dict(o, "x3"); }],
            ["deleted and put back", () => { for (let i = 0; i < 50; ++i) { o.again = i; delete o.again; } }],
            ["one for another", () => { o.one = 1; delete o.one; o[Symbol("in its place")] = 1; }],
            ["and the other way", () => { const s = Symbol("goes"); o[s] = 1; delete o[s]; o.comes = 1; }],
            ["half of them deleted", () => { let i = 0; for (const key of Object.keys(o)) { if (++i % 2) delete o[key]; } }],
            ["assigned from another", () => Object.assign(o, { p: 1, q: 2, [Symbol("r")]: 3 })],
            ["used as a prototype", () => { Object.create(o).own = 1; }],
            ["gone through", () => { for (const key in o) { } JSON.stringify(Object.entries(o).length); }],
            ["sealed", () => Object.seal(o)],
            ["frozen", () => Object.freeze(o)],
        ];
        for (const [label, step] of steps) {
            try {
                step();
            } catch (e) {
                if (kind === "Object" || !(e instanceof TypeError))
                    throw e;
                ++refused;
            }
            check(o, `${kind} ${many}: ${label}`);
        }
    }
}
print(checks, "checked,", wrong, "wrong,", refused, "things that an object of Python's would not have done to it");

// What is said when it changes while it is gone through goes by the same count.
const say = (label, change) => print(label, "=>", m.changed_while_going_through({ a: 1, b: 2, c: 3 }, change), "|", m.changed_while_going_through(Object.assign(m.make("C"), { a: 1, b: 2, c: 3 }), o => { try { change(o); } catch { } }));
let n = 0;
say("nothing is done", o => { });
say("one is added", o => { o["n" + n++] = 1; });
say("one is deleted", o => { delete o.c; });
say("one is written to", o => { o.a = 5; });
say("a symbol is added", o => { o[Symbol()] = 1; });
say("one that is not listed is added", o => Object.defineProperty(o, "h" + n++, { value: 1, enumerable: false }));
say("one is made not to be listed", o => Object.defineProperty(o, "c", { enumerable: false }));
say("one for another", o => { if ("c" in o) { delete o.c; o.d = 1; } });
