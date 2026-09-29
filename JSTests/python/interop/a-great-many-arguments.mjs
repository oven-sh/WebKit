// A function of JavaScript's has its arguments on the stack, so there can be only so many, whoever gives them. That is a RangeError in JavaScript, and a RecursionError to Python, and nothing worse. A function of
// Python's that is given them by Python can be given any number: programs/a-great-many-arguments.py. Given them by JavaScript, it can be given what JavaScript can give.
import * as m from "./a-great-many-arguments.py";

function outcome(f) {
    try {
        return String(f());
    } catch (e) {
        return `${e.constructor.name}: ${String(e.message).slice(0, 60)}`;
    }
}
function count(...a) { return a.length; }
function countAndLast(...a) { const last = a[a.length - 1]; return `${a.length} ${typeof last === "object" ? JSON.stringify(last) : last}`; }
function usesArguments() { return arguments.length; }
const arrow = (...a) => a.length;
const bound = count.bind(null, -1);
class Counted { constructor(...a) { this.count = a.length; } }

// Whether the very large ones fit depends on how much stack there is. That they end in one of two ways does not.
const fitsOrNot = (text, n) => text === String(n) || text.startsWith("RecursionError") || text.startsWith("RangeError") ? "all of them or a proper refusal" : text;

for (const n of [0, 3, 1000, 60000]) {
    print(n, "Python to JavaScript:", m.give(count, n), m.give(usesArguments, n), m.give(arrow, n), m.give(bound, n), m.give(Math.max, n), m.give_by_name(countAndLast, n), m.make(Counted, n), m.make(m.derive(Counted), n));
    const values = Array.from({ length: n }, (_, i) => i);
    print(n, "JavaScript to Python:", outcome(() => m.rest(...values)), outcome(() => m.rest.apply(null, values)), outcome(() => Reflect.apply(m.rest, null, values)), outcome(() => new m.Plain(...values).count), outcome(() => new m.Plain().method(...values)), outcome(() => new m.Plain()(...values)));
    print(n, "that takes two:", outcome(() => m.two(...values)));
}
for (const n of [400000, 3000000]) {
    print(n, "Python to JavaScript:", [m.give(count, n), m.give(usesArguments, n), m.give(arrow, n), String(m.give(Math.max, n)).replace(String(n - 1), String(n)), m.make(Counted, n), m.make(m.derive(Counted), n)].map(text => fitsOrNot(String(text), n)).join(", "));
    const values = Array.from({ length: n }, (_, i) => i);
    print(n, "JavaScript to Python:", [outcome(() => m.rest(...values)[0]), outcome(() => new m.Plain(...values).count), outcome(() => new m.Plain().method(...values))].map(text => fitsOrNot(text, n)).join(", "));
}
// And there and back: Python gives them to a function of JavaScript's that gives them to one of Python's.
const passOn = (...a) => m.rest(...a)[0];
for (const n of [3, 60000, 3000000])
    print(n, "there and back:", fitsOrNot(String(m.give(passOn, n)), n));
