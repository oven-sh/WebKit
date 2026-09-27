load("./resources/overloaded-operators.js", "caller relative");

// Which operator was asked for, and of which operand.
{
    const a = make(6);
    const b = make(3);
    const check = (f, ...expected) => {
        asked.length = 0;
        f();
        if (asked.join() !== expected.join())
            throw new Error(`${f} asked for ${asked}, not ${expected}`);
    };
    check(() => a < b, "<");
    check(() => a > b, ">");
    check(() => a <= b, "<=");
    check(() => a >= b, ">=");
    check(() => a != b, "==");
    check(() => 1 > a, ">");
    check(() => { let x = a; x++; }, "+");
    check(() => a == null);
    check(() => undefined != a);
    check(() => a === b);
}

// What the handler throws is thrown, and a comparison gives a boolean whatever the handler gives.
{
    const error = new Error("thrown");
    const throws = $vm.createObjectOverloadingOperators(() => { throw error; });
    for (const f of [() => throws + 1, () => 1 - throws, () => -throws, () => ~throws, () => throws == 1, () => 1 < throws, () => { let x = throws; x++; }]) {
        noInline(f);
        for (let i = 0; i < testLoopCount; i++) {
            let caught;
            try {
                f();
            } catch (e) {
                caught = e;
            }
            if (caught !== error)
                throw new Error(`${f} threw ${caught}`);
        }
    }
    for (const [given, expected] of [[1, true], [0, false], ["", false], [{}, true], [undefined, false]]) {
        const gives = $vm.createObjectOverloadingOperators(() => given);
        if ((gives == 1) !== expected || (gives < 1) !== expected || (gives != 1) !== !expected)
            throw new Error(`A comparison that gave ${String(given)}`);
    }
}
