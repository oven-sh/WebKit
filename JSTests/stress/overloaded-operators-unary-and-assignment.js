load("./resources/overloaded-operators.js", "caller relative");

run([
    ["-a", a => -a, wrapped(-6)],
    ["~a", a => ~a, wrapped(-7)],
    ["++a", a => ++a, wrapped(7)],
    ["--a", a => --a, wrapped(5)],
    ["a++", a => [a++, a], [wrapped(6), wrapped(7)]],
    ["a--", a => [a--, a], [wrapped(6), wrapped(5)]],
    ["a += b", (a, b) => a += b, wrapped(9)],
    ["a *= b", (a, b) => a *= b, wrapped(18)],
]);
