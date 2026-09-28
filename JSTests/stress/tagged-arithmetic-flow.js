//@ requireOptions("--useDollarVM=1")
load("./resources/tagged-arithmetic.js", "caller relative");

// That a number stays what it is on its way from one operation to the next, however the code is compiled.

const add = $vm.tagged.add, sub = $vm.tagged.sub, mul = $vm.tagged.mul, div = $vm.tagged.div, isInt = $vm.tagged.isInt;

// Not [a, b]: an array of an int and a float is an array of two floats.
const list = $vm.tagged.newArray;
const F0 = float(0), F1 = float(1), F2 = float(2), F4 = float(4), H = float(0.5);

const cases = {
    "constants": [() => list(add(1, 2), add(1, F2), add(F1, F1), mul(F2, F2), div(4, 2), sub(F2, F2), mul(0, -1), mul(F0, -1), add(2147483647, 1), float(2), float(F2), isInt(2), isInt(F2), isInt(float(2)), isInt("2")),
        "int 3, float 3, float 2, float 4, float 2, float 0, int 0, float -0, none, float 2, float 2, other, other, other, other"],
    "chain of floats with integral values": [(i, o, t) => add(mul(t, t), sub(t, F1)), "float 5"],
    "chain of ints": [(i, o, t) => add(mul(o, 3), sub(o, 5)), "int -1"],
    "int into float": [(i, o, t) => list(mul(add(o, o), t), add(o, o)), "float 4, int 2"],
    "division of ints, then back among ints": [(i, o, t) => list(div(add(o, 3), 2), add(add(o, 3), 2)), "float 2, int 6"],
    "a local that is an int or a float by turns": [(i, o, t) => { let x = i & 1 ? o : t; let y = add(x, x); return i & 1 ? list(x, y) : list(o, add(o, o)); }, "int 1, int 2"],
    "a local that is an int, and is used in float arithmetic": [(i, o, t) => { let x = o; let y = add(mul(x, H), mul(x, F2)); return list(x, y); }, "int 1, float 2.5"],
    "an accumulator that starts as an int": [(i, o, t) => { let s = 0, first = s; for (let k = 0; k < 4; ++k) { if (!k) first = s; s = add(s, H); } return list(first, s); }, "int 0, float 2"],
    "an accumulator of floats with integral values": [(i, o, t) => { let s = F0; for (let k = 0; k < 4; ++k) s = add(s, t); return s; }, "float 8"],
    "an accumulator of ints": [(i, o, t) => { let s = 0; for (let k = 0; k < 4; ++k) s = add(s, o); return s; }, "int 4"],
    "an accumulator that overflows": [(i, o, t) => { let s = 1073741824, n = 0; while (s !== undefined) { s = add(s, s); ++n; } return n; }, "int 1"],
    "an int loop counter given to float arithmetic": [(i, o, t) => { let s = F0, last; for (let k = 0; k < 3; k = add(k, 1)) { s = add(s, mul(k, F1)); last = k; } return list(s, last); }, "float 3, int 2"],
    "through an object": [(i, o, t) => { let p = { a: mul(t, t), b: add(o, o) }; return list(p.a, p.b, add(p.a, p.b)); }, "float 4, int 2, float 6"],
    "through an array that keeps them": [(i, o, t) => { let a = $vm.tagged.newArray(o, t, H); a.push(add(o, o)); a.push(mul(t, t)); a[0] = t; a[1] = o; return a; }, "float 2, int 1, float 0.5, int 2, float 4"],
    "an empty array that keeps them": [(i, o, t) => { let a = $vm.tagged.newArray(); a.push(o); a.push(H); a.push(t); a.push(o); return a; }, "int 1, float 0.5, float 2, int 1"],
    "through a closure": [(i, o, t) => { let x = mul(t, t), y = add(o, o); return (() => list(x, y))(); }, "float 4, int 2"],
    "through a call": [(i, o, t) => ((a, b) => list(b, a, add(a, b)))(add(o, o), mul(t, t)), "float 4, int 2, float 6"],
    "chosen by a condition": [(i, o, t) => { let x = i % 3 ? mul(t, t) : add(o, 3); return i % 3 ? x : F4; }, "float 4"],
    "compared": [(i, o, t) => [t === 2, t == add(o, o), t > o, isInt(t), isInt(add(o, o)), isInt(mul(t, o))].map(Number).join(""), "other"],
    "branching on which it is": [(i, o, t) => { let x = i & 1 ? o : t; return isInt(x) === !!(i & 1) ? "right" : "wrong"; }, "other"],
    "not a number, sometimes": [(i, o, t) => { let x = i % 5 ? t : "s"; let r = add(x, o); return i % 5 ? r : add(t, o); }, "float 3"],
};

// A float that is being held unboxed when the code around it has to be abandoned.
function acrossAnExit(o, t, surprise) {
    let x = mul(t, t);
    let y = add(x, F1);
    let z = add(o, surprise);
    return list(x, y, z);
}
noInline(acrossAnExit);

// Long enough to be entered while it runs.
function enteredWhileRunning(o, t, n) {
    let s = F0, c = 0;
    for (let k = 0; k < n; ++k) {
        s = add(s, t);
        s = sub(s, t);
        c = add(c, o);
    }
    return list(s, c);
}
noInline(enteredWhileRunning);

const showAll = value => Array.isArray(value) ? value.map(show).join(", ") : show(value);
for (let name in cases) {
    let [f, wanted] = cases[name];
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i) {
        let shown = showAll(f(i, int(1), F2));
        if (shown !== wanted)
            throw new Error(`${name}: ${shown} and not ${wanted}, the ${i}th time`);
    }
}
if (cases["compared"][0](0, 1, F2) !== "111010")
    throw new Error("compared: " + cases["compared"][0](0, 1, F2));
if (cases["branching on which it is"][0](0, 1, F2) !== "right" || cases["branching on which it is"][0](1, 1, F2) !== "right")
    throw new Error("branching on which it is");

for (let i = 0; i < testLoopCount; ++i) {
    let shown = showAll(acrossAnExit(int(1), F2, int(1)));
    if (shown !== "float 4, float 5, int 2")
        throw new Error("Before the exit: " + shown);
}
for (let [surprise, wanted] of [[2147483647, "none"], [H, "float 1.5"], ["s", "none"], [F1, "float 2"]]) {
    let shown = showAll(acrossAnExit(int(1), F2, surprise));
    if (shown !== "float 4, float 5, " + wanted)
        throw new Error("Across the exit: " + shown);
}

for (let i = 0; i < 3; ++i) {
    let shown = showAll(enteredWhileRunning(int(1), F2, 200000));
    if (shown !== "float 0, int 200000")
        throw new Error("Entered while running: " + shown);
}
