//@ requireOptions("--useDollarVM=1")
load("./resources/tagged-arithmetic.js", "caller relative");

// That what is put in an ordinary array comes out as it went in. An array may hold only int32s, or only doubles, for as long
// as that is all it is given. What it must not do is make the one into the other when it is given something else, or
// because of what other arrays made at the same place in the code were given.

// Each makes an array of x, y and z, in that order.
const ways = [
    "[x, y, z]",
    "{ let a = []; a.push(x); a.push(y); a.push(z); return a; }",
    "{ let a = []; a.push(x, y, z); return a; }",
    "{ let a = [x]; a.push(y); a.push(z); return a; }",
    "{ let a = [x, y]; a.push(z); return a; }",
    "{ let a = []; a[0] = x; a[1] = y; a[2] = z; return a; }",
    "{ let a = []; a[2] = z; a[1] = y; a[0] = x; return a; }",
    "{ let a = new Array(3); a[0] = x; a[1] = y; a[2] = z; return a; }",
    "{ let a = new Array(3); for (let k = 0; k < 3; ++k) a[k] = k ? k > 1 ? z : y : x; return a; }",
    "{ let a = [x, x, x]; a[1] = y; a[2] = z; return a; }",
    "{ let a = [z, z, z]; a[0] = x; a[1] = y; return a; }",
    "{ let a = [0, 0, 0]; a[0] = x; a[1] = y; a[2] = z; return a; }",
    "{ let a = [0.5, 0.5, 0.5]; a[0] = x; a[1] = y; a[2] = z; return a; }",
    "{ let a = new Array(3).fill(x); a[1] = y; a[2] = z; return a; }",
    "{ let a = [z, z, z]; a.fill(x, 0, 1); a.fill(y, 1, 2); return a; }",
    "{ let a = [x, y, y]; a.fill(z, 2); return a; }",
    "Array.of(x, y, z)",
    "new Array(x, y, z)",
    "Array(x, y, z)",
    "Array.from([x, y, z])",
    "Array.from({ length: 3, 0: x, 1: y, 2: z })",
    "Array.from([0, 1, 2], k => k ? k > 1 ? z : y : x)",
    "[x].concat([y], [z])",
    "[x].concat(y, z)",
    "[x, y].concat([z])",
    "[].concat([x, y, z])",
    "[].concat([x], [y, z])",
    "[x, y, z].concat()",
    "[...[x], ...[y, z]]",
    "[x, ...[y, z]]",
    "[...[x, y], z]",
    "[x, y, z].slice()",
    "[0, x, y, z, 0.5].slice(1, 4)",
    "[x, y, z].map(v => v)",
    "[0, 1, 2].map(k => k ? k > 1 ? z : y : x)",
    "[x, y, z].filter(() => true)",
    "[x, 0, y, 0.5, z].filter((v, k) => !(k & 1))",
    "[x, y, z].flat()",
    "[[x], [y, z]].flat()",
    "[x, y, z].flatMap(v => [v])",
    "[z, y, x].reverse()",
    "[z, y, x].toReversed()",
    "{ let a = [y, z]; a.unshift(x); return a; }",
    "{ let a = [z]; a.unshift(x, y); return a; }",
    "{ let a = [x, z]; a.splice(1, 0, y); return a; }",
    "{ let a = [x, 0, 0.5, z]; a.splice(1, 2, y); return a; }",
    "{ let a = [0, x, y, z, 0.5]; return a.splice(1, 3); }",
    "[x, z].toSpliced(1, 0, y)",
    "[x, x, z].with(1, y)",
    "{ let a = [x, y, z, 0]; a.pop(); return a; }",
    "{ let a = [x, y, z, 0.5]; a.pop(); return a; }",
    "{ let a = [0, x, y, z]; a.shift(); return a; }",
    "{ let a = [0.5, x, y, z]; a.shift(); return a; }",
    "{ let a = [x, y, z, 0, 0.5]; a.length = 3; return a; }",
    "{ let a = [0, x, y, z]; a.copyWithin(0, 1); a.length = 3; return a; }",
    "{ let [p, q, r] = [x, y, z]; return [p, q, r]; }",
    "{ let p = y, q = x; [p, q] = [q, p]; return [p, q, z]; }",
    "{ let p = x, q = y, r = z; for (let k = 0; k < 3; ++k) [p, q, r] = [q, r, p]; return [p, q, r]; }",
    "Object.values({ p: x, q: y, r: z })",
    "Object.entries({ p: x, q: y, r: z }).map(e => e[1])",
    "((...r) => r)(x, y, z)",
    "((p, ...r) => [p, ...r])(x, y, z)",
    "(function () { return Array.prototype.slice.call(arguments); })(x, y, z)",
    "(function () { return Array.from(arguments); })(x, y, z)",
    "(function () { return [...arguments]; })(x, y, z)",
    "[x, y, z].sort(() => 0)",
    "[x, y, z].toSorted(() => 0)",
    "[...[x, y, z].values()]",
    "[...new Map([[0, x], [1, y], [2, z]]).values()]",
    "{ let b = []; for (let v of [x, y, z]) b.push(v); return b; }",
    "{ let a = [x, y, z], b = []; for (let k = 0; k < 3; ++k) b[k] = a[k]; return b; }",
    "{ let b = []; [x, y, z].forEach(v => b.push(v)); return b; }",
    "[x, y, z].reduce((b, v) => (b.push(v), b), [])",
    "[[x, y, z].at(0), [x, y, z].at(1), [x, y, z].at(-1)]",
    "[[x, y, z].find(() => true), [x, y, z][1], [x, y, z].findLast(() => true)]",
    "Reflect.apply((p, q, r) => [p, q, r], null, [x, y, z])",
    "((p, q, r) => [p, q, r])(...[x, y, z])",
    "((p, q, r) => [p, q, r]).apply(null, [x, y, z])",
];

const values = $vm.tagged.newArray(int(1), float(2), float(0.5), int(-7), float(-3), float(1.25));

// One place that reads every array there is.
function read(a, k) { return a[k]; }
noInline(read);

for (let way of ways) {
    let f = eval(freshSource("x, y, z", way.startsWith("{") ? way : "{ return " + way + "; }"));
    noInline(f);
    // All of one kind for a while, to give the place ideas, and then every combination, over and over.
    for (let i = 0; i < testLoopCount; ++i) {
        let n = i < testLoopCount / 4 ? (i >> 6) % 6 * 43 : i * 7;
        let x = values[n % 6], y = values[(n / 6 | 0) % 6], z = values[(n / 36 | 0) % 6];
        let a = f(x, y, z);
        if (a.length !== 3)
            throw new Error(`${way} made an array of ${a.length}`);
        let got = show(read(a, 0)) + ", " + show(read(a, 1)) + ", " + show(read(a, 2));
        let wanted = show(x) + ", " + show(y) + ", " + show(z);
        if (got !== wanted)
            throw new Error(`${way} of ${wanted} holds ${got}, the ${i}th time (${$vm.indexingMode(a)})`);
    }
}
