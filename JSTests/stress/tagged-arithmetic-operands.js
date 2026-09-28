//@ requireOptions("--useDollarVM=1")
load("./resources/tagged-arithmetic.js", "caller relative");

// Every pair of operands, at a place in the code that has seen only that kind of pair, and then at one that has seen them all.

function makers() {
    // A function of its own each time, so that each is compiled for what it alone has seen.
    return {
        add: fresh("a, b", "return $vm.tagged.add(a, b);"),
        sub: fresh("a, b", "return $vm.tagged.sub(a, b);"),
        mul: fresh("a, b", "return $vm.tagged.mul(a, b);"),
        div: fresh("a, b", "return $vm.tagged.div(a, b);"),
    };
}

const groups = { ints, floats, others };
const small = { ints: [1, 2, 3].map(int), floats: [1, 2, 0.5].map(float), others: [undefined, "1"] };

for (let trainedLeft in groups) {
    for (let trainedRight in groups) {
        let functions = makers();
        for (let name in functions) {
            let f = functions[name];
            // Until it is compiled for these, and without overflow.
            for (let i = 0; i < testLoopCount; ++i) {
                let left = small[trainedLeft][i % small[trainedLeft].length], right = small[trainedRight][(i >> 2) % small[trainedRight].length];
                check(`${name}(${show(left)}, ${show(right)}) trained`, f(left, right), expected(name, left, right));
            }
            // Then everything, twice, so that what is compiled after the surprise is tried too.
            for (let round = 0; round < 2; ++round) {
                for (let left of [...ints, ...floats, ...others]) {
                    for (let right of [...ints, ...floats, ...others])
                        check(`${name}(${show(left)}, ${show(right)}) after ${trainedLeft}, ${trainedRight}`, f(left, right), expected(name, left, right));
                }
            }
        }
    }
}
