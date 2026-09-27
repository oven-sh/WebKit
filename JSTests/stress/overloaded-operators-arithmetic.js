load("./resources/overloaded-operators.js", "caller relative");

run([
    ["a + b", (a, b) => a + b, wrapped(9)],
    ["a - b", (a, b) => a - b, wrapped(3)],
    ["a * b", (a, b) => a * b, wrapped(18)],
    ["a / b", (a, b) => a / b, wrapped(2)],
    ["a % b", (a, b) => a % b, wrapped(0)],
    ["a ** b", (a, b) => a ** b, wrapped(216)],
    ["a << b", (a, b) => a << b, wrapped(48)],
    ["a >> b", (a, b) => a >> b, wrapped(0)],
    ["a & b", (a, b) => a & b, wrapped(2)],
    ["a | b", (a, b) => a | b, wrapped(7)],
    ["a ^ b", (a, b) => a ^ b, wrapped(5)],
]);
