// A string plus anything is a string, so a + b + c where a or b is a string is compiled to conversions and one concatenation, and x + "" to a conversion. Except that a
// string plus an object that overloads operators is whatever the object says. Each expression here is written twice: as one would write it, and as additions of two
// operands each, which are compiled to additions. Both have to come to the same thing, having done the same things in the same order.

let log = [];

function describe(value)
{
    if (typeof value === "string")
        return JSON.stringify(value);
    if (typeof value === "object" && value !== null)
        return value.label;
    return String(value);
}

// What an object that overloads operators can say to being added to something.
const answers = {
    string: (left, right) => "<" + describe(left) + "+" + describe(right) + ">",
    declines: (left, right) => typeof left === "string" || typeof right === "string" ? undefined : "<no string>",
    number: () => 5,
    overloading: () => made.string,
    another: () => made.number,
    ordinary: () => ordinary,
    throws: () => { throw new Error("thrown by the handler"); },
};

function make(name)
{
    const object = $vm.createObjectOverloadingOperators((operator, left, right, isCompoundAssignment) => {
        log.push(`${name}: ${describe(left)} ${operator}${isCompoundAssignment ? "=" : ""} ${describe(right)}`);
        return answers[name](left, right);
    });
    object.label = "[" + name + "]";
    object.toString = function () { log.push(name + ".toString"); return "(" + name + ")"; };
    object.valueOf = function () { log.push(name + ".valueOf"); return this; };
    return object;
}

const made = { };
for (const name in answers)
    made[name] = make(name);

const ordinary = {
    label: "[ordinary]",
    valueOf() { log.push("ordinary.valueOf"); return 7; },
    toString() { log.push("ordinary.toString"); return "never"; },
};
const throwing = {
    label: "[throwing]",
    valueOf() { log.push("throwing.valueOf"); throw new Error("thrown by valueOf"); },
};

const kinds = ["s", 1, 2.5, undefined, null, true, 10n, ordinary, throwing, ...Object.values(made)];

let operands;
function E(i) { log.push("evaluate " + i); return operands[i]; }
function add(a, b) { return a + b; }
function addTo(a, b) { a += b; return a; }
noInline(E);
noInline(add);
noInline(addTo);

// [how many operands, as written, as additions]
const expressions = [
    [1, () => "a" + E(0), () => add("a", E(0))],
    [1, () => E(0) + "a", () => add(E(0), "a")],
    [1, () => "" + E(0), () => add("", E(0))],
    [1, () => E(0) + "", () => add(E(0), "")],
    [1, () => { let d = E(0); d += ""; return d; }, () => addTo(E(0), "")],
    [1, () => { let d = E(0); d += "a"; return d; }, () => addTo(E(0), "a")],
    [1, () => { let d = "a"; d += E(0); return d; }, () => addTo("a", E(0))],
    [2, () => "a" + E(0) + E(1), () => { const t = add("a", E(0)); return add(t, E(1)); }],
    [2, () => E(0) + "a" + E(1), () => { const t = add(E(0), "a"); return add(t, E(1)); }],
    [2, () => E(0) + E(1) + "a", () => { const x = E(0); const t = add(x, E(1)); return add(t, "a"); }],
    [2, () => "a" + E(0) + "b" + E(1), () => { let t = add("a", E(0)); t = add(t, "b"); return add(t, E(1)); }],
    [2, () => "a" + E(0) + "b" + E(1) + "c", () => { let t = add("a", E(0)); t = add(t, "b"); t = add(t, E(1)); return add(t, "c"); }],
    [2, () => typeof E + E(0) + E(1), () => { const t = add("function", E(0)); return add(t, E(1)); }],
    [2, () => E(0) + typeof E + E(1), () => { const t = add(E(0), "function"); return add(t, E(1)); }],
    [2, () => E(0) + `t${1}` + E(1), () => { const t = add(E(0), "t1"); return add(t, E(1)); }],
    [2, () => { let d = E(0); d += E(1) + "a"; return d; }, () => { const d = E(0); return addTo(d, add(E(1), "a")); }],
    [2, () => { let d = E(0); d += "a" + E(1); return d; }, () => { const d = E(0); return addTo(d, add("a", E(1))); }],
    [2, () => E(0) + ("a" + E(1)), () => { const x = E(0); return add(x, add("a", E(1))); }],
    [2, () => "a" + (E(0) ? E(1) : "never") + "b", () => { const t = add("a", E(0) ? E(1) : "never"); return add(t, "b"); }],
    [3, () => "a" + E(0) + E(1) + E(2), () => { let t = add("a", E(0)); t = add(t, E(1)); return add(t, E(2)); }],
    [3, () => E(0) + "a" + E(1) + E(2), () => { let t = add(E(0), "a"); t = add(t, E(1)); return add(t, E(2)); }],
    [3, () => E(0) + ("a" + E(1)) + E(2), () => { const x = E(0); const t = add(x, add("a", E(1))); return add(t, E(2)); }],
    [3, () => "a" + E(0) + ("b" + E(1) + E(2)), () => { const t = add("a", E(0)); const u = add("b", E(1)); return add(t, add(u, E(2))); }],
    [3, () => { let d = E(0); d += "a" + E(1) + E(2); return d; }, () => { const d = E(0); const t = add("a", E(1)); return addTo(d, add(t, E(2))); }],
    [3, () => { let d = E(0); d += E(1) + "a" + E(2); return d; }, () => { const d = E(0); const t = add(E(1), "a"); return addTo(d, add(t, E(2))); }],
];

function outcome(f)
{
    log = [];
    let result;
    try {
        result = "gave " + typeof (result = f()) + " " + describe(result);
    } catch (error) {
        result = "threw " + error.message;
    }
    return log.join("; ") + " => " + result;
}

function check(count, written, additions, chosen)
{
    operands = chosen;
    const expected = outcome(additions);
    const actual = outcome(written);
    if (actual !== expected)
        throw new Error(`${written} of ${chosen.map(describe).join(", ")}\n    did      ${actual}\n    and not  ${expected}`);
}

function forEachChoice(count, from, f, chosen = [])
{
    if (chosen.length === count)
        return f(chosen);
    for (const kind of from)
        forEachChoice(count, from, f, [...chosen, kind]);
}

for (const [, written, additions] of expressions) {
    noInline(written);
    noInline(additions);
}

// First with nothing that overloads operators, for long enough to be compiled for that.
const plain = kinds.filter(kind => !Object.values(made).includes(kind));
for (const [count, written, additions] of expressions) {
    for (let i = 0; i < testLoopCount / plain.length ** count + 1; i++)
        forEachChoice(count, plain, chosen => check(count, written, additions, chosen));
}
// Then with everything, for long enough to be compiled for that.
for (const [count, written, additions] of expressions) {
    for (let i = 0; i < testLoopCount / kinds.length ** count + 1; i++)
        forEachChoice(count, kinds, chosen => check(count, written, additions, chosen));
}
// And each kind by itself, so that code is compiled that has seen nothing else.
for (const kind of kinds) {
    for (const [count, written, additions] of expressions.map(([count, written, additions]) => [count, eval("(" + written + ")"), additions])) {
        noInline(written);
        for (let i = 0; i < testLoopCount; i++)
            check(count, written, additions, Array(count).fill(kind));
    }
}
