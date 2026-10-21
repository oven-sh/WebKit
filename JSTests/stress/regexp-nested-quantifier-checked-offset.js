// Each alternative claims at most INT_MAX characters, but a nested one claims on top of the ones around it.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

let neverMatches = [
    ["(?:(?:^.)?a{2147483647})?b{2147483647}", "m"],
    ["(?:(?:\\b.)?a{2147483647})?b{2147483647}", ""],
];

for (let [source, flags] of neverMatches) {
    shouldBe(new RegExp(source, flags).exec("ab"), null);
    shouldBe(new RegExp(source, flags).test("ab"), false);
}

shouldBe(/(?:(?:^.)?a{2147483647})?b/m.exec("ab")[0], "b");
shouldBe(/(?:(?:^.)?a{2147483647})?b/m.test("ab"), true);
