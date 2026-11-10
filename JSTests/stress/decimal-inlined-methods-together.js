//@ requireOptions("--useDecimal=1")
// Two of the inlined methods on one value in one function, and receivers and methods of another realm.

function shouldBe(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(`${what}: expected ${String(expected)}, got ${String(actual)}`);
}

const methods = ["isNaN", "isFinite", "exponent", "mantissa", "abs", "negate", "toString", "toExponential"];
const texts = ["NaN", "Infinity", "-Infinity", "0", "-0", "1", "-1.5", "12345678901234567890123", "1e-6176", "-1e6144"];
const text = value => value instanceof Decimal ? "d" + value.toExponential() : typeof value + String(value) + Object.is(value, -0);

// What each answers where nothing is compiled together.
const answers = new Map;
for (const t of texts) {
    for (const method of methods)
        answers.set(t + method, text(Decimal.prototype[method].call(new Decimal(t))));
}

for (const first of methods) {
    for (const second of methods) {
        const pair = Function("x", `const a = x.${first}(); const b = x.${second}(); const c = x.${first}(); return [a, b, c];`);
        noInline(pair);
        for (let i = 0; i < testLoopCount / 20; ++i) {
            const t = texts[i % texts.length];
            const [a, b, c] = pair(new Decimal(t));
            shouldBe(text(a), answers.get(t + first), `${t} ${first} before ${second}`);
            shouldBe(text(b), answers.get(t + second), `${t} ${second} behind ${first}`);
            shouldBe(text(c), answers.get(t + first), `${t} ${first} around ${second}`);
        }
    }
}

{
    const both = x => !x.isNaN() && x.isFinite();
    noInline(both);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(both(new Decimal("1.5")), true, "finite");
        shouldBe(both(new Decimal("NaN")), false, "NaN");
        shouldBe(both(new Decimal("Infinity")), false, "infinity");
    }
}

// The result has the prototype of the realm of the method.
{
    const other = createGlobalObject();
    const own = { Decimal };
    for (const method of ["abs", "negate", "mantissa"]) {
        for (const [realmOfReceiver, realmOfMethod] of [[other, other], [other, own], [own, other], [own, own]]) {
            const f = Function("x", "m", "return m.call(x)");
            const g = Function("x", `return x.${method}()`);
            noInline(f);
            noInline(g);
            const x = new realmOfReceiver.Decimal("-1.5");
            const m = realmOfMethod.Decimal.prototype[method];
            for (let i = 0; i < testLoopCount * 2; ++i) {
                shouldBe(Object.getPrototypeOf(f(x, m)), realmOfMethod.Decimal.prototype, `${method} of a realm`);
                shouldBe(Object.getPrototypeOf(g(x)), realmOfReceiver.Decimal.prototype, `${method} of the receiver's realm`);
            }
        }
    }
    const x = new other.Decimal("1.5");
    const read = x => [x.isNaN(), x.isFinite(), x.exponent(), x.toString()].join();
    noInline(read);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(read(i & 1 ? x : new Decimal("1.5")), "false,true,0,1.5", "both realms");
}

// A name that is no name of the code that calls it.
{
    const call = d => d[0]();
    noInline(call);
    const d = new Decimal("1.5");
    d[0] = Decimal.prototype.isNaN;
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(call(d), false, "indexed");
}

shouldBe(Reflect.ownKeys(Decimal.prototype).slice(0, 7).join(), "isNaN,isFinite,exponent,mantissa,abs,negate,add", "the order of the proposal");
