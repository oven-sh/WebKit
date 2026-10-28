// A substring rope caches whether its base is 8-bit. Atomizing the base can swap in an equal 16-bit StringImpl,
// so a switch that reads the characters of the base in place has to ask the base.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + ", expected " + expected);
}

const table = {};

function substringOfWidenedBase(upper, from, to) {
    const lower = upper.toLowerCase();
    const flat = lower + "☃";
    Symbol(flat);
    const wide = flat.slice(0, lower.length);
    Symbol(wide);
    table[wide] = 1;
    const narrow = upper.toLowerCase();
    const part = narrow.slice(from, to);
    shouldBe(table[narrow], 1);
    return part;
}

function which(string) {
    switch (string) {
    case "ert":
        return 1;
    case "ertyui":
        return 2;
    case "ertyuiop":
        return 3;
    case "ertyuiopasdfg":
        return 4;
    default:
        return 0;
    }
}
noInline(which);

function whichOfAnything(value) {
    switch (value) {
    case "ert":
        return 1;
    case "ertyui":
        return 2;
    case "ertyuiop":
        return 3;
    case "ertyuiopasdfg":
        return 4;
    default:
        return 0;
    }
}
noInline(whichOfAnything);

const long = "QWERTYUIOPASDFGHJKLZXCVBNMQWERTYUIOPASDFGHJKLZXCVBNM";
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(which(substringOfWidenedBase(long, 2, 5)), 1);
    shouldBe(which(substringOfWidenedBase(long, 2, 8)), 2);
    shouldBe(which(substringOfWidenedBase(long, 2, 10)), 3);
    shouldBe(which(substringOfWidenedBase(long, 2, 15)), 4);
    shouldBe(which(substringOfWidenedBase(long, 2, 7)), 0);
    shouldBe(which(substringOfWidenedBase(long, 3, 9)), 0);

    shouldBe(whichOfAnything(i & 1 ? substringOfWidenedBase(long, 2, 8) : i), i & 1 ? 2 : 0);
    shouldBe(whichOfAnything(substringOfWidenedBase(long, 2, 15)), 4);
}
