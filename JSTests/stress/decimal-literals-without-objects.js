//@ requireOptions("--useDecimal=1", "--useDecimalLiterals=1")
// To optimized code a literal is a constant, and has an object where one can be seen.

function shouldBe(actual, expected) {
    if (!Object.is(actual, expected))
        throw new Error(`expected ${String(expected)}, got ${String(actual)}`);
}

function test(f, check) {
    noInline(f);
    for (let i = 0; i < testLoopCount; ++i)
        check(f, i);
    return f;
}

const amounts = ["0.25", "0.75", "1.5", "2.5", "0.05", "0.95"].map(text => new Decimal(text));

test(() => [1.255m.round(2), 1.255m.round(2, "floor"), (-1.255m).round(2, "ceil"), 1.5m.scale10(2), 1.5m.scale10(-7000), 1m.add(2m).multiply(3m), 1m.add(new Decimal(2))],
    f => shouldBe(f().join(), "1.26,1.25,-1.25,150,0,9,3"));

test(x => [x.divide(8m), x.divide(3m).round(2), 3m.divide(x), x.equals(1.50m), (-0m).multiply(x)],
    f => shouldBe(f(amounts[2]).join(), "0.1875,0.5,2,true,-0"));

// Each evaluation makes one, in a row and in a loop.
test(() => { const results = [1m, 1m]; for (let i = 0; i < 3; ++i) results.push(2m); return results; },
    f => shouldBe(new Set(f()).size, 5));

// An exit makes it, once for all names. The sum overflows.
{
    const f = test(n => { const x = -1.5m, y = x, z = x.subtract(1m); if (n + 1 === 2) return x.isNaN(); return [x, y, z]; },
        f => shouldBe(f(1), false));
    const [x, y, z] = f(2147483647);
    shouldBe(x, y);
    shouldBe(Object.getPrototypeOf(x), Decimal.prototype);
    shouldBe(`${x} ${z}`, "-1.5 -2.5");
}

// It escapes on one path.
{
    let escaped;
    test((x, escapes) => { const holder = { x, literal: 2m }; if (escapes) escaped = holder; return holder.x.lessThan(holder.literal); }, (f, i) => {
        shouldBe(f(amounts[2], !(i % 100)), true);
        shouldBe(escaped.literal.toString(), "2");
    });
}

// A total begins with it.
test(list => { let total = -0m; for (const x of list) total = total.add(x); return total; }, f => {
    shouldBe(f(amounts).toString(), "6");
    shouldBe(f([]).toString(), "-0");
});
test((list, mask) => { const first = 1m; let total = first; for (let i = 0; i < list.length; ++i) { if (i & mask) total = total.add(0.5m); } return [first, total]; }, f => {
    shouldBe(f(amounts, 1).join(), "1,2.5");
    const [first, total] = f(amounts, 0);
    shouldBe(first, total);
});

// It is of the realm of the code that has it.
{
    const other = createGlobalObject(), make = other.Function("return 1m");
    test((list, mask) => { let total = 0m; for (let i = 0; i < list.length; ++i) total = i & mask ? make() : list[i].add(total); return total; }, f => {
        shouldBe(f(amounts, 1) instanceof other.Decimal, true);
        shouldBe(f(amounts, 0) instanceof Decimal, true);
    });
}

// Its methods are those of the prototype. This is last.
{
    const f = test(() => 1.5m.negate(), f => shouldBe(f().toString(), "-1.5"));
    Decimal.prototype.negate = () => "own";
    shouldBe(f(), "own");
}
