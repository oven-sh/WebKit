load("./resources/overloaded-operators.js", "caller relative");

// What would be concluded of the result if the operands could only be what the language has.
run([
    // That with a number for one operand it is a number.
    ["2 * a", a => 2 * a, wrapped(12)],
    ["a * 2", a => a * 2, wrapped(12)],
    ["a - 1", a => a - 1, wrapped(5)],
    ["1 - a", a => 1 - a, wrapped(-5)],
    ["a / 2", a => a / 2, wrapped(3)],
    ["a & 1", a => a & 1, wrapped(0)],
    ["1 << a", a => 1 << a, wrapped(64)],
    // That multiplying by one only makes a number of it.
    ["a * 1", a => a * 1, wrapped(6)],
    ["1 * a", a => 1 * a, wrapped(6)],
    // That it is a number, a BigInt or a string.
    ["(a * b) | 0", (a, b) => (a * b) | 0, wrapped(18)],
    ["(a & b) + 1", (a, b) => (a & b) + 1, wrapped(3)],
    ["2 * a - 1", a => 2 * a - 1, wrapped(11)],
    ["-a - 1", a => -a - 1, wrapped(-7)],
    ["~a & 1", a => ~a & 1, wrapped(1)],
    ["-(-a)", a => -(-a), wrapped(6)],
    ["(a - b).v", (a, b) => (a - b).v, 3],
    ["typeof (2 * a)", a => typeof (2 * a), "object"],
    ["typeof (a & 1)", a => typeof (a & 1), "object"],
    ["typeof -a", a => typeof -a, "object"],
    ["typeof a++", a => typeof a++, "object"],
    ["a + b + a", (a, b) => a + b + a, wrapped(15)],
    ["a, five times", a => { let sum = a; for (let i = 0; i < 4; i++) sum = sum + a; return sum; }, wrapped(30)],
    // That it equals zero if it is false.
    ["if ((a & 1) == 0)", a => (a & 1) == 0 ? "then" : "else", "then"],
    ["if ((a & 1) != 0)", a => (a & 1) != 0 ? "then" : "else", "else"],
    ["if ((a & 1) === 0)", a => (a & 1) === 0 ? "then" : "else", "else"],
]);
