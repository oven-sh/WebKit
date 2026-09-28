//@ requireOptions("--useDollarVM=1", "--keepNumberEncodings=1")
load("./resources/tagged-arithmetic.js", "caller relative");

// That a number which is only carried from one place to another arrives as it left: an int32 as an int32 and a double as a
// double. Each way of carrying one is compiled for one kind of number, and is then given another.

const list = $vm.tagged.newArray;
let closed, closedToo;
var globalVar;
let globalLexical;
const holder = { field: 0.5, other: 1 };
class Fields { field = 0.5; #hidden = 0.5; static make(x) { let o = new Fields; o.field = x; o.#hidden = x; return o; } get hidden() { return this.#hidden; } }
const accessor = { stored: 0.5, get value() { return this.stored; }, set value(x) { this.stored = x; } };
function* yields(x) { let sent = yield x; yield sent; }
function strictThis() { "use strict"; return this; }
function notInlined(x) { return x; }
noInline(notInlined);
const bound = notInlined.bind(null);
const map = new Map;

// Source text, so that each pair of kinds gets functions of its own, which have seen nothing else.
const routes = {
    "returned": "x",
    "a local": "{ let y = x; return y; }",
    "a local, assigned on one path": "{ let y = x; if (i & 1) y = x; return y; }",
    "a local, in a loop": "{ let y = x; for (let k = 0; k < 3; ++k) { let z = y; y = z; } return y; }",
    "a local, live around a loop": "{ let y = x, n = 0; for (let k = 0; k < 3; ++k) n += k; return n ? y : n; }",
    "two locals, swapped in a loop": "{ let a = x, b = x; for (let k = 0; k < 4; ++k) { let c = a; a = b; b = c; } return a; }",
    "a closure variable": "{ closed = x; return closed; }",
    "a closure variable of its own": "{ let y = x; return (() => y)(); }",
    "a closure variable written from inside": "{ let y; (() => { y = x; })(); return y; }",
    "a global variable": "{ globalVar = x; return globalVar; }",
    "a global lexical variable": "{ globalLexical = x; return globalLexical; }",
    "a property of a new object": "({ p: x }).p",
    "a property of an old object": "{ holder.field = x; return holder.field; }",
    "a property that has held ints": "{ holder.other = x; return holder.other; }",
    "a class field": "Fields.make(x).field",
    "a private field": "Fields.make(x).hidden",
    "a getter and a setter": "{ accessor.value = x; return accessor.value; }",
    "an element of an array that keeps them": "list(x)[0]",
    "an element stored later": "{ let a = list(0.5, 1, 'x'); a[i % 3] = x; return a[i % 3]; }",
    "an element pushed": "{ let a = list(); a.push(x); return a[0]; }",
    "an element popped": "{ let a = list(1, 0.5, x); return a.pop(); }",
    "an argument": "((a, b) => b)(0, x)",
    "an argument of a call that is not inlined": "notInlined(x)",
    "an argument of a bound function": "bound(x)",
    "the arguments object": "(function () { return arguments[0]; })(x)",
    "a rest parameter": "((...a) => a[0])(x)",
    "a default parameter": "((a, b = a) => b)(x)",
    "spread into a call": "notInlined(...list(x))",
    "Reflect.apply": "Reflect.apply(notInlined, null, list(x))",
    "this": "strictThis.call(x)",
    "destructuring an object": "{ let { p } = { p: x }; return p; }",
    "destructuring an array that keeps them": "{ let [p] = list(x); return p; }",
    "for-of": "{ for (let y of list(x)) return y; }",
    "an iterator's next()": "list(x)[Symbol.iterator]().next().value",
    "a generator's yield": "yields(x).next().value",
    "sent into a generator": "{ let g = yields(0); g.next(); return g.next(x).value; }",
    "thrown and caught": "{ try { throw x; } catch (e) { return e; } }",
    "finally": "{ try { return x; } finally { closedToo = i; } }",
    "a conditional": "(i & 1 ? x : x)",
    "||, && and ??": "((x || x) && (x ?? x))",
    "a switch": "{ switch (i & 1) { case 0: return x; default: return x; } }",
    "unary plus": "+x",
    "Number()": "Number(x)",
    "a Map's value": "{ map.set('k', x); return map.get('k'); }",
    "Object.values": "Object.values({ p: x })[0]",
    "Object.assign": "Object.assign({}, { p: x }).p",
    "an object spread": "({ ...{ p: x } }).p",
    "slice of an array that keeps them": "list(x, 'x').slice()[0]",
    "concat of arrays that keep them": "list('x').concat(list(x))[1]",
};

const kinds = { int: [1, 7, -3].map(int), integral: [2, 5, -4].map(float), fractional: [0.5, -1.5, 2.25].map(float) };

for (let name in routes) {
    let body = routes[name];
    if (!body.startsWith("{"))
        body = "{ return " + body + "; }";
    for (let trained in kinds) {
        for (let surprise in kinds) {
            if (trained === surprise)
                continue;
            let f = eval("(function (x, i) " + body + ")");
            noInline(f);
            let run = (values, i) => {
                let x = values[i % values.length];
                let wanted = show(x);
                let got = show(f(x, i));
                if (got !== wanted)
                    throw new Error(`${name}, compiled for ${trained} and given ${surprise}: ${wanted} arrived as ${got}, the ${i}th time`);
            };
            for (let i = 0; i < testLoopCount; ++i)
                run(kinds[trained], i);
            for (let i = 0; i < 20; ++i)
                run(kinds[surprise], i);
            for (let i = 0; i < testLoopCount; ++i)
                run(i % 5 < 2 ? kinds[surprise] : kinds[trained], i);
        }
    }
}
