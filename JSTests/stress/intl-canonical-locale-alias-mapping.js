function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

// Bun: upstream runs these only `if (Intl.getCanonicalLocales('tl')[0] === 'fil')`, which is so with Apple's internal SDK alone. This fork maps aliases on every build.
shouldBe(Intl.getCanonicalLocales('nb')[0], 'nb');
shouldBe(Intl.getCanonicalLocales('no')[0], 'no');
shouldBe(Intl.getCanonicalLocales('iw')[0], 'he');
shouldBe(Intl.getCanonicalLocales('prs')[0], 'fa-AF');
shouldBe(Intl.getCanonicalLocales('swc')[0], 'sw-CD');
shouldBe(Intl.getCanonicalLocales('tl')[0], 'fil');
