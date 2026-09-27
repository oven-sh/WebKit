load("./resources/overloaded-operators.js", "caller relative");

// Objects that are equal and not the same, in code that has compared only ordinary objects since it was compiled.
{
    const equal = (a, b) => a == b;
    const branch = (a, b) => a == b ? "then" : "else";
    const orNull = (a, b) => a == b;
    noInline(equal);
    noInline(branch);
    noInline(orNull);
    const x = {};
    const y = {};
    for (let round = 0; round < 3; round++) {
        repeat("x == y", equal, false, testLoopCount, x, y);
        repeat("x == x", equal, true, testLoopCount, x, x);
        repeat("if (x == y)", branch, "else", testLoopCount, x, y);
        for (let i = 0; i < testLoopCount; i++) {
            if (orNull(x, i & 1 ? null : x) !== !(i & 1))
                throw new Error("x == x or null");
        }
        repeat("a == b, equal", equal, true, 1, make(6), make(6));
        repeat("if (a == b), equal", branch, "then", 1, make(6), make(6));
        repeat("a == b, equal, where b was null at times", orNull, true, 1, make(6), make(6));
        repeat("a == null, where b was null at times", orNull, false, 1, make(6), null);
        repeat("a == x", equal, false, 1, make(6), x);
        repeat("x == a", equal, false, 1, x, make(6));
    }
}
