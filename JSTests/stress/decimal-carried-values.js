//@ requireOptions("--useDecimal=1", "--useDollarVM=1")

// A value that goes around a loop is kept in registers, and is still one object wherever that can be seen.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${String(expected)} but got ${String(actual)}`);
}

const D = text => new Decimal(text);
const amounts = ["0.25", "0.75", "1.5", "2.5", "0.05", "0.95"].map(D);
const names = list => {
    let ids = new Map;
    return list.map(value => {
        if (!ids.has(value))
            ids.set(value, ids.size);
        return ids.get(value) + ":" + String(value);
    }).join(" ");
};
function keep(list, value) { list.push(value); }
noInline(keep);

// Functions of one text share what is known of their values.
let numberOfCopies = 0;
const copyOf = (f, ...names) => Function(...names, `return ${f} // ${numberOfCopies++}`);

function test(f, ...cases) {
    noInline(f);
    for (let i = 0; i < testLoopCount; i++) {
        for (let check of cases)
            check(f);
    }
    return f;
}

test(list => { let total = new Decimal(0); for (let x of list) total = total.add(x); return total; },
    f => shouldBe(f(amounts).toString(), "6"),
    f => shouldBe(f([]).toString(), "0"));

test(list => { let total = new Decimal(0); for (let i = 0; i < list.length; i++) total = list[i].add(total); return total; },
    f => shouldBe(f(amounts).toString(), "6"));

test(list => { let total = new Decimal(3); for (let x of list) total = total.subtract(x); return total; },
    f => shouldBe(f(amounts).toString(), "-3"),
    f => shouldBe(f(amounts.slice(0, 3)).toString(), "0.5"),
    f => shouldBe(f([D("3")]).toExponential(), "0e+0"));

test(list => { let total = new Decimal(3); for (let x of list) total = x.subtract(total); return total; },
    f => shouldBe(f(amounts).toString(), "5.4"));

{
    let f = test(list => { let total = new Decimal("-0"); for (let x of list) total = total.add(x); return total.toString(); },
        f => shouldBe(f(amounts), "6"),
        f => shouldBe(f([D("-0")]), "-0"),
        f => shouldBe(f([D("1.5"), D("-1.5")]), "0"),
        f => shouldBe(f([D("-1.5"), D("1.5")]), "0"),
        f => shouldBe(f([D("1.5"), D("-4"), D("0.5"), D("7")]), "5"),
        f => shouldBe(f([D("-1.5"), D("-4"), D("0.5")]), "-5"));
    shouldBe(f([D("1"), D("NaN"), D("1")]), "NaN");
    shouldBe(f([D("1"), D("-Infinity"), D("1")]), "-Infinity");
    shouldBe(f([D("0.001"), D("4611686018427387"), D("4611686018427387"), D("4611686018427387"), D("4611686018427387"), D("0.999")]), "18446744073709549");
    shouldBe(f([D("1e-20"), D("1"), D("-1e-20"), D("1")]), "2");
    shouldBe(f([D("9e6144"), D("9e6144")]), "Infinity");
    shouldBe(f([D("18446744073709551626")]), "18446744073709551626");
    shouldBe(f([D("1.234567890123456789012345678901234"), D("1")]), "2.234567890123456789012345678901234");
}

test(list => { let product = new Decimal(8); for (let x of list) product = product.multiply(x); return product; },
    f => shouldBe(f(amounts).toString(), "0.2671875"));

// The first value is not new.
test((start, list) => { let total = start; for (let x of list) total = total.add(x); return total; },
    f => shouldBe(f(amounts[0], amounts).toString(), "6.25"),
    f => shouldBe(f(amounts[0], []), amounts[0]));

test((start, list, mask) => { let total = start; for (let i = 0; i < list.length; i++) { if (i & mask) total = total.add(list[i]); } return total; },
    f => shouldBe(f(amounts[0], amounts, 1).toString(), "4.45"),
    f => shouldBe(f(amounts[0], amounts, 0), amounts[0]));

test((a, b, flag, list, mask) => { let total = flag ? a : b; for (let i = 0; i < list.length; i++) { if (i & mask) total = total.add(list[i]); } return total; },
    f => shouldBe(f(amounts[0], amounts[1], true, amounts, 1).toString(), "4.45"),
    f => shouldBe(f(amounts[0], amounts[1], true, amounts, 0), amounts[0]),
    f => shouldBe(f(amounts[0], amounts[1], false, amounts, 0), amounts[1]));

test((start, list) => list.reduce((total, x) => total.add(x), start),
    f => shouldBe(f(amounts[0], amounts).toString(), "6.25"),
    f => shouldBe(f(amounts[0], []), amounts[0]));

test((start, list) => { let total = start; for (let x of list) total = x.add(total); return total; },
    f => shouldBe(f(amounts[0], amounts).toString(), "6.25"),
    f => shouldBe(f(amounts[0], []), amounts[0]));

// It has another name.
test((list, mask) => { let first = new Decimal(1), total = first; for (let i = 0; i < list.length; i++) { if (i & mask) total = total.add(list[i]); } return [first, total]; },
    f => shouldBe(names(f(amounts, 1)), "0:1 1:5.2"),
    f => shouldBe(names(f(amounts, 0)), "0:1 0:1"));

test((list, mask) => { let a = new Decimal(1), b = a; for (let i = 0; i < list.length; i++) { if (i & mask) { a = a.add(list[i]); b = b.subtract(list[i]); } } return [a, b]; },
    f => shouldBe(names(f(amounts, 1)), "0:5.2 1:-3.2"),
    f => shouldBe(names(f(amounts, 0)), "0:1 0:1"));

test((list, mask) => { let total = new Decimal(1), before = new Decimal(2); for (let i = 0; i < list.length; i++) { before = total; if (i & mask) total = total.add(list[i]); } return [before, total]; },
    f => shouldBe(names(f(amounts, 1)), "0:4.25 1:5.2"),
    f => shouldBe(names(f(amounts, 2)), "0:5 0:5"));

test(count => { let a = new Decimal("0.5"), b = new Decimal("0.5"); for (let i = 0; i < count; i++) { let sum = a.add(b); a = b; b = sum; } return [a, b]; },
    f => shouldBe(names(f(5)), "0:4 1:6.5"),
    f => shouldBe(names(f(0)), "0:0.5 1:0.5"));

// It has been seen.
test((list, mask) => { let seen = [], total = new Decimal(0); for (let i = 0; i < list.length; i++) { if (i & mask) total = total.add(list[i]); keep(seen, total); } return seen; },
    f => shouldBe(names(f(amounts, 1)), "0:0 1:0.75 1:0.75 2:3.25 2:3.25 3:4.2"));

test((list, mask) => { let seen = [], total = new Decimal(0); for (let i = 0; i < list.length; i++) { keep(seen, total); if (i & mask) total = total.add(list[i]); } seen.push(total); return seen; },
    f => shouldBe(names(f(amounts, 2)), "0:0 0:0 0:0 1:1.5 2:4 2:4 2:4"));

test((list, at) => { let seen = [], total = new Decimal(0); for (let i = 0; i < list.length; i++) { if (i === at) keep(seen, total); if (i < 2) total = total.add(list[i]); } seen.push(total); return seen; },
    f => shouldBe(names(f(amounts, 3)), "0:1 0:1"),
    f => shouldBe(names(f(amounts, 1)), "0:0.25 1:1"),
    f => shouldBe(names(f(amounts, 9)), "0:1"));

test(lists => { let seen = [], total = new Decimal(0); for (let list of lists) { for (let x of list) total = total.add(x); keep(seen, total); } return seen; },
    f => shouldBe(names(f([amounts, [], amounts, []])), "0:6 0:6 1:12 1:12"));

test((list, mask) => { let seen = [], total = new Decimal(0); keep(seen, total); for (let i = 0; i < list.length; i++) { if (i & mask) total = total.add(list[i]); } seen.push(total); return seen; },
    f => shouldBe(names(f(amounts, 1)), "0:0 1:4.2"),
    f => shouldBe(names(f(amounts, 0)), "0:0 0:0"));

// What needs the value without zeros at the end.
test((list, at) => {
    let seen = [], total = new Decimal(0);
    for (let i = 0; i < list.length; i++) {
        total = total.add(list[i]);
        if (i === at)
            seen.push(total.toString(), total.toExponential(), total.equals(new Decimal(1)), total.notEquals(new Decimal(1)), total.mantissa(), total.scale10(1), total.abs(), total.negate(), total.toFixed({ digits: 1 }), total.round(1, "ceil"));
    }
    return seen.join();
}, f => shouldBe(f(amounts, 1), "1,1e0,true,false,1,10,1,-1,1.0,1"));

test(list => { let count = 0, total = new Decimal(0); for (let x of list) { if (total.equals(new Decimal(1))) count++; total = total.add(x); } return count; },
    f => shouldBe(f(amounts), 1));

test(list => { let count = 0, total = new Decimal(0); for (let x of list) total = total.add(x); for (let x of list) { if (total.equals(new Decimal(6))) count++; } return count; },
    f => shouldBe(f(amounts), 6));

test((x, y, flag) => { let value = flag ? x.add(y) : x.subtract(y); return value.round(0, "ceil").toString() + value.toString(); },
    f => shouldBe(f(amounts[0], amounts[1], true), "11"),
    f => shouldBe(f(amounts[3], amounts[2], false), "11"));

// An exit makes the object, or finds it.
{
    let f = test((start, list, numbers, mask) => {
        let total = start, sum = 0;
        for (let i = 0; i < list.length; i++) {
            if (i & mask)
                total = total.add(list[i]);
            sum += numbers[i];
        }
        return total;
    }, f => shouldBe(f(amounts[0], amounts, [1, 1, 1, 1, 1, 1], 1).toString(), "4.45"));
    shouldBe(f(amounts[0], amounts, [1, 1, 1, "1", 1, 1], 0), amounts[0]);
}
{
    let f = test((list, numbers) => {
        let total = new Decimal(0), sum = 0;
        for (let i = 0; i < list.length; i++) {
            sum += numbers[i];
            if (i < 2)
                total = total.add(list[i]);
        }
        return total;
    }, f => shouldBe(f(amounts, [1, 1, 1, 1, 1, 1]).toString(), "1"));
    shouldBe(f(amounts, [1, 1, 1, "1", 1, 1]).toString(), "1");
}
{
    let f = test((list, numbers, mask) => {
        let first = new Decimal(1), total = first, sum = 0;
        for (let i = 0; i < list.length; i++) {
            if (i & mask)
                total = total.add(list[i]);
            sum += numbers[i];
            if (typeof sum === "string")
                return total === first;
        }
        return total.toString();
    }, f => shouldBe(f(amounts, [1, 1, 1, 1, 1, 1], 1), "5.2"));
    shouldBe(f(amounts, [1, 1, 1, "1", 1, 1], 0), true);
}
{
    function check(i, at) { if (i === at) throw new Error; }
    noInline(check);
    test((start, list, at, mask) => {
        let total = start;
        try {
            for (let i = 0; i < list.length; i++) {
                check(i, at);
                if (i & mask)
                    total = total.add(list[i]);
            }
        } catch { }
        return total;
    },
    f => shouldBe(f(amounts[0], amounts, 9, 1).toString(), "4.45"),
    f => shouldBe(f(amounts[0], amounts, 2, 1).toString(), "1"),
    f => shouldBe(f(amounts[0], amounts, 4, 0), amounts[0]));
}

// Optimized code is entered in the loop.
function long(start, mask) {
    let total = start;
    for (let i = 0; i < testLoopCount * 40; i++) {
        if (i & mask)
            total = total.add(new Decimal("0.5"));
    }
    return total;
}
noInline(long);
shouldBe(long(amounts[0], 1).toString(), String(testLoopCount * 10 + 0.25));
shouldBe(long(amounts[0], 0), amounts[0]);

// It is not always a Decimal of this class.
test((start, list) => { let total = start; for (let x of list) total = total ? total.add(x) : x; return total; },
    f => shouldBe(f(null, amounts).toString(), "6"),
    f => shouldBe(f(null, []), null));
{
    class Sub extends Decimal { add() { return new Decimal(7); } }
    let sub = new Sub(1);
    test((start, list) => { let total = start; for (let x of list) total = total.add(x); return total; },
        f => shouldBe(f(sub, amounts).toString(), "12.75"),
        f => shouldBe(f(amounts[0], amounts).toString(), "6.25"),
        f => shouldBe(f(sub, []), sub));
    test((start, list) => { let total = start; if (start.isFinite()) { for (let x of list) total = list.length > 9 ? total.add(x) : x.add(total); } return total; },
        f => shouldBe(f(sub, amounts).toString(), "7"),
        f => shouldBe(f(amounts[0], amounts).toString(), "6.25"),
        f => shouldBe(f(sub, [...amounts, ...amounts]).toString(), "18.75"));
}

{
    const make = () => copyOf((start, list) => { let total = start; for (let x of list) total = total.add(x); return total; })();
    for (let start of [undefined, null, 1, "1", { }, [], new (createGlobalObject().Decimal)(1)]) {
        let f = test(make(), f => shouldBe(f(amounts[0], amounts).toString(), "6.25"));
        shouldBe(f(start, []), start);
    }
    let f = test(make(), f => shouldBe(f(amounts[0], amounts).toString(), "6.25"));
    shouldBe(f({ add() { return new Decimal(1); } }, amounts).toString(), "6.75");

    // The check in front of the loop is given up where it fails.
    let other = new ArrayBuffer(8);
    test(make(), f => shouldBe(f(amounts[0], amounts).toString(), "6.25"), f => shouldBe(f(other, []), other));
    test((start, list, mask) => {
        let total = start, count = 0;
        for (let i = 0; i < list.length; i++) {
            if (i & mask)
                total = total.add(list[i]);
            if (total.lessThan(new Decimal(2)))
                count++;
        }
        return count ? total.toString() : total;
    }, f => shouldBe(f(amounts[0], amounts, 1), "4.45"), f => shouldBe(f(other, [], 1), other));
}
{
    let f = test((a, b, flag, list) => { let total = new Decimal(0); for (let x of list) total = total.add(x); let value = flag ? a : b; return value.add(total).toString(); },
        f => shouldBe(f(amounts[0], amounts[1], true, amounts), "6.25"),
        f => shouldBe(f(amounts[0], amounts[1], false, amounts), "6.75"));
    shouldBe(f(Object.assign(new ArrayBuffer(8), { add: () => "own" }), amounts[1], true, amounts), "own");
}
{
    let other = createGlobalObject();
    let make = other.Function("return new Decimal(1)");
    test((list, mask) => { let total = new Decimal(0); for (let i = 0; i < list.length; i++) total = i & mask ? make() : list[i].add(total); return total; },
        f => shouldBe(f(amounts, 1) instanceof other.Decimal, true),
        f => shouldBe(f(amounts, 0) instanceof Decimal, true));
}
test((list, skip) => {
    try {
        switch (skip) {
        case 0:
            let total = new Decimal(0);
        default:
            for (let x of list)
                total = total.add(x);
            return total.toString();
        }
    } catch (error) {
        return error.name;
    }
}, f => shouldBe(f(amounts, 0), "6"), f => shouldBe(f(amounts, 1), "ReferenceError"), f => shouldBe(f([], 1), "ReferenceError"));

// A check that was made for something else stays.
{
    class Sub extends Decimal { isFinite() { return "sub"; } }
    let f = (start, list, asks) => { let total = start, answer; for (let x of list) { if (asks) answer = total.isFinite(); total = total.add(x); } return asks ? answer : total.toString(); };
    noInline(f);
    for (let i = 0; i < 200; i++)
        shouldBe(f(new Sub(1), [amounts[0]], true), "sub");
    test(f, f => shouldBe(f(amounts[0], amounts, false), "6.25"));
    shouldBe(f(amounts[0], amounts, true), true);
}
{
    let node = $vm.createDOMJITCheckJSCastObject(), func = node.func;
    let f = (start, list, asks) => { let total = start, answer; for (let x of list) { if (asks) answer = func.call(total); total = x.add(asks === 1 ? x : total); } return asks ? answer : total.toString(); };
    noInline(f);
    for (let i = 0; i < 200; i++)
        shouldBe(f(node, [amounts[0]], 1), 42);
    test(f, f => shouldBe(f(amounts[0], amounts, 0), "6.25"));
    let name;
    try {
        f(amounts[0], amounts, 2);
    } catch (error) {
        name = error.name;
    }
    shouldBe(name, "TypeError");
}

// What else has the object changes it. This is last: from here on a Decimal may have properties.
{
    function change(value, i, at) { if (i === at) value.add = () => new Decimal(42); }
    noInline(change);
    const make = () => copyOf((start, list, at) => { let total = start; for (let i = 0; i < list.length; i++) { change(start, i, at); if (i > 2) total = total.add(list[i]); } return total; }, "change")(change);
    let f = test(make(), f => shouldBe(f(amounts[0], amounts, 9).toString(), "3.75"));
    shouldBe(f(D("1"), amounts, 1).toString(), "43");
    f = test(make(), f => shouldBe(f(amounts[0], amounts, 9).toString(), "3.75"));
    shouldBe(f(D("1"), amounts, 1).toString(), "43");
    f = test((list, at) => { let total = new Decimal(0); for (let i = 0; i < list.length; i++) { if (i >= at) change(total, i, at); total = total.add(list[i]); } return total; },
        f => shouldBe(f(amounts, 9).toString(), "6"),
        f => shouldBe(f(amounts, -1).toString(), "6"));
    shouldBe(f(amounts, 4).toString(), "42.95");
    f = test((list, at) => { let total = new Decimal(0); for (let i = 0; i < list.length; i++) { if (i === 4) change(total, i, at); else total = total.add(list[i]); } return total.toString(); },
        f => shouldBe(f(amounts, -1), "5.95"));
    shouldBe(f(amounts, 4), "42");
}

test(list => { let total = new Decimal(0); for (let x of list) total = total.add(x); return total; },
    f => shouldBe(f(amounts).toString(), "6"));
