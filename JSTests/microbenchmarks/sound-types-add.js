//@ requireOptions("--useSoundTypes=1")

// The argument profiles of add() have seen strings, which the checks reject. The addition should still be compiled as
// ArithAdd rather than ValueAdd.

function add(a, b) {
    $$t(a, 8);
    $$t(b, 8);
    return a + b;
}
noInline(add);

for (let i = 0; i < 100; ++i) {
    try {
        add("s", i);
    } catch { }
    try {
        add(i, "s");
    } catch { }
}

let sum = 0;
for (let i = 0; i < 2e7; ++i)
    sum = add(sum, i & 7);
if (sum !== 70000000)
    throw new Error("bad sum: " + sum);
