// For the overloaded-operators-*.js tests. Each gives expressions, as functions of (a, b), and what they give for
// wrapped(6) and wrapped(3). run() has each compiled for numbers and ordinary objects before there is any object that
// overloads operators, gives that code such objects, and goes on until it has been compiled for those, and then for both.

class Wrapped {
    constructor(v) { this.v = v; }
}
function wrapped(v) { return new Wrapped(v); }

const asked = [];

function unwrap(x)
{
    return typeof x === "object" && x !== null && "v" in x ? x.v : x;
}

function handler(operator, left, right)
{
    if (asked.length < 100)
        asked.push(operator);
    const l = unwrap(left);
    const r = unwrap(right);
    switch (operator) {
    case "+": return make(l + r);
    case "-": return make(l - r);
    case "*": return make(l * r);
    case "/": return make(l / r);
    case "%": return make(l % r);
    case "**": return make(l ** r);
    case "<<": return make(l << r);
    case ">>": return make(l >> r);
    case "&": return make(l & r);
    case "|": return make(l | r);
    case "^": return make(l ^ r);
    case "negate": return make(-l);
    case "~": return make(~l);
    case "==": return l == r;
    case "<": return l < r;
    case "<=": return l <= r;
    case ">": return l > r;
    case ">=": return l >= r;
    }
    throw new Error("Not an operator: " + operator);
}

function valueOf() { return this.v; }
function toString() { return "W" + this.v; }

function make(v)
{
    const object = $vm.createObjectOverloadingOperators(handler);
    object.v = v;
    object.valueOf = valueOf;
    object.toString = toString;
    return object;
}

function isSame(actual, expected)
{
    if (expected instanceof Wrapped)
        return typeof actual === "object" && actual !== null && !(actual instanceof Wrapped) && Object.is(actual.v, expected.v);
    if (Array.isArray(expected))
        return Array.isArray(actual) && actual.length === expected.length && expected.every((e, i) => isSame(actual[i], e));
    return Object.is(actual, expected);
}

function describe(value)
{
    if (Array.isArray(value))
        return "[" + value.map(describe).join(", ") + "]";
    if (typeof value === "object" && value !== null && "v" in value)
        return "wrapped(" + value.v + ")";
    return typeof value + " " + String(value);
}

function repeat(name, f, expected, count, ...operands)
{
    for (let i = 0; i < count; i++) {
        const actual = f(...operands);
        if (!isSame(actual, expected))
            throw new Error(`${name} gave ${describe(actual)} the ${i}th time, not ${describe(expected)}`);
    }
}

function run(expressions)
{
    for (const [name, f] of expressions)
        noInline(f);

    const six = { valueOf() { return 6; }, toString() { return "six"; } };
    const three = { valueOf() { return 3; }, toString() { return "three"; } };
    const forNumbers = new Map;
    const forObjects = new Map;
    for (const [name, f] of expressions) {
        forNumbers.set(name, f(6, 3));
        forObjects.set(name, f(six, three));
        repeat(name, f, forNumbers.get(name), testLoopCount, 6, 3);
        repeat(name, f, forObjects.get(name), testLoopCount, six, three);
    }

    const a = make(6);
    const b = make(3);
    for (const [name, f, expected] of expressions)
        repeat(name, f, expected, testLoopCount, a, b);
    for (const [name, f, expected] of expressions) {
        for (let i = 0; i < 4; i++) {
            repeat(name, f, forNumbers.get(name), testLoopCount / 10, 6, 3);
            repeat(name, f, expected, testLoopCount / 10, a, b);
            repeat(name, f, forObjects.get(name), testLoopCount / 10, six, three);
        }
    }
}
