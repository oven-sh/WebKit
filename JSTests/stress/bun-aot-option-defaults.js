//@ skip if !$aot
//@ runDefault("--useAOT=1")
//@ runDefault

const options = jscOptions();

function shouldBe(name, expected) {
    // Loose equality: jscOptions() reports a Bool option as 0 or 1.
    if (options[name] != expected)
        throw new Error(`${name}: expected ${expected} but got ${options[name]}`);
}

if (options.useAOT) {
    for (const name of ["useJIT", "useRegExpJIT", "useDFGJIT", "useFTLJIT", "useBBQJIT"])
        shouldBe(name, false);
}
shouldBe("sizeClassProgression", options.useAOT ? 1.1 : 1.4);
shouldBe("heapGrowthMaxIncrease", 3);
