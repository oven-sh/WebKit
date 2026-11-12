//@ requireOptions("--useDecimal=1", "--useConcurrentJIT=0")
//@ runDefault
// Only in this mode: others compile again on purpose.
// Neither the count of round() nor the realm of the Decimals may make optimized code be thrown away call after call, however many a
// function has: 1 compilation, or 2 where an argument changes its type. A check of either made one more for each call.

const x = new Decimal("19.99");
const counts = {
    integer: "2",
    fraction: "t[i & 1]",
    minimum: "Math.min(v, 3)",
    variable: "v",
    negativeZero: "(i ? -0 : -0)",
    constant: "1e10",
    logarithm: "Math.log10(100)",
};
for (const sites of [1, 3, 10]) {
    for (const name in counts) {
        const run = Function("x", "t", "v", `let s = 0; for (let i = 0; i < 10; ++i) { ${`s += x.round(${counts[name]}).isNaN() ? 1 : 2; `.repeat(sites)}} return s;`);
        noInline(run);
        for (let i = 0; i < 20000; ++i) {
            if (run(x, [2, 0, 0.5], i & 1 ? 2 : 1e10) != 20 * sites)
                throw new Error(`${name}: wrong result`);
        }
        // A million without the DFG.
        const compiles = numberOfDFGCompiles(run);
        if (compiles > 3 && compiles < 1000)
            throw new Error(`${sites} of ${name}: compiled ${compiles} times`);
    }
}

{
    const other = createGlobalObject();
    const [a, b] = [new Decimal("19.99"), new Decimal("4.5")];
    const [c, d] = [new other.Decimal("19.99"), new other.Decimal("4.5")];
    for (const methods of [["lessThan(y)", "equals(y)"], ["round(2)"], ["add(y)", "multiply(y)"], ["isNaN()", "abs()"]]) {
        let body = "let c = 0;";
        for (let i = 0; i < 12; ++i)
            body += `if (x.${methods[i % methods.length]}) ++c;`;
        const run = Function("x", "y", body + "return c;");
        noInline(run);
        for (let i = 0; i < 60000; ++i)
            i < 6000 ? run(a, b) : run(c, d);
        const compiles = numberOfDFGCompiles(run);
        if (compiles > 4 && compiles < 1000)
            throw new Error(`${methods} of two realms: compiled ${compiles} times`);
    }
}

// What fails a check at each call is left to the function from then on.
for (const [source, argument] of [["try { return x.add(y); } catch { }", 1], ["try { return x.add(y); } catch { }", { }], ["const o = { digits: 2 }; y(o); return x.toFixed(o);", o => o.other = 1],
    ["try { return new Decimal(y === x ? x.toString() : y); } catch { }", "x"], ["try { return x.round(y === x ? 1e10 : y); } catch { }", 0.5], ["try { return x.toFixed({ digits: y === x ? 1 : y }); } catch { }", -1]]) {
    const run = Function("x", "y", `${source} // ${typeof argument}`);
    noInline(run);
    if (typeof argument === "function")
        noInline(argument);
    for (let i = 0; i < 60000; ++i)
        run(x, i < 6000 && typeof argument !== "function" ? x : argument);
    const compiles = numberOfDFGCompiles(run);
    if (compiles > 3 && compiles < 1000)
        throw new Error(`${source}: compiled ${compiles} times`);
}
