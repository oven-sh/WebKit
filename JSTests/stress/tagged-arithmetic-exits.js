//@ requireOptions("--useDollarVM=1", "--keepNumberEncodings=1")
load("./resources/tagged-arithmetic.js", "caller relative");

// Loops whose variables the compiler would like to keep unboxed, abandoned part of the way through.

const add = $vm.tagged.add, sub = $vm.tagged.sub, mul = $vm.tagged.mul, list = $vm.tagged.newArray;
const F0 = float(0), F1 = float(1);

const sources = {
    // Starts as an int, and stays one if the steps are.
    "sum": "(function (n, step) { let s = 0; for (let k = 0; k < n; ++k) s = add(s, step); return s; })",
    "sum from a float": "(function (n, step) { let s = F0; for (let k = 0; k < n; ++k) s = add(s, step); return s; })",
    "sum of an array": "(function (n, step) { let a = list(step, step, step, step), s = 0; for (let k = 0; k < n; ++k) s = add(s, a[k & 3]); return s; })",
    "product": "(function (n, step) { let s = 1; for (let k = 0; k < n; ++k) s = mul(s, step); return s; })",
    "two at once": "(function (n, step) { let s = 0, t = 0; for (let k = 0; k < n; ++k) { s = add(s, step); t = sub(t, s); } return add(s, t); })",
    "the one before": "(function (n, step) { let s = 0, before = 0; for (let k = 0; k < n; ++k) { before = s; s = add(s, step); } return before; })",
    "kept in an object": "(function (n, step) { let o = { s: 0 }; for (let k = 0; k < n; ++k) o.s = add(o.s, step); return o.s; })",
    "kept in a closure": "(function (n, step) { let s = 0; let bump = () => { s = add(s, step); }; for (let k = 0; k < n; ++k) bump(); return s; })",
};

// The same, with nothing for a compiler to get hold of.
function model(name, n, step) {
    let value = { "sum": n * step, "sum from a float": n * step, "sum of an array": n * step, "product": step ** n, "kept in an object": n * step, "kept in a closure": n * step }[name];
    if (name === "two at once")
        value = n * step - step * n * (n + 1) / 2;
    if (name === "the one before")
        value = n ? (n - 1) * step : 0;
    let isFloat = kindOf(step) === "float" && (n > 0 || false);
    if (name === "sum from a float")
        isFloat = true;
    if (name === "the one before")
        isFloat = kindOf(step) === "float" && n > 1;
    return (isFloat ? "float " : "int ") + value;
}

const steps = { int: int(1), integral: float(1), fractional: float(0.5) };
for (let name in sources) {
    for (let trained in steps) {
        for (let surprise in steps) {
            let f = eval(sources[name]);
            noInline(f);
            let run = (n, step) => {
                let got = show(f(n, step)), wanted = model(name, n, step);
                if (got !== wanted)
                    throw new Error(`${name}(${n}, ${show(step)}), compiled for ${trained}: ${got} and not ${wanted}`);
            };
            for (let i = 0; i < testLoopCount / 10; ++i)
                run(i % 8, steps[trained]);
            for (let n of [0, 1, 2, 7])
                run(n, steps[surprise]);
            for (let i = 0; i < testLoopCount / 10; ++i)
                run(i % 8, i % 3 ? steps[trained] : steps[surprise]);
            // Long enough to be entered while it runs.
            if (name !== "product" && name !== "two at once") {
                run(30000, steps[trained]);
                run(30000, steps[surprise]);
            }
        }
    }
}
