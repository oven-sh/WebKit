function shouldBe(actual, expected, what) {
    if (JSON.stringify(actual) !== JSON.stringify(expected)) throw new Error(`${what}: expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

// `@2 ` is a counter of how often what follows is reached, which is twice. `@!2 `: skipped.
function parse(marked) {
    const events = { "": 0, "!": 1 };
    const places = [];
    const expected = [];
    let removed = 0;
    const source = marked.replace(/@(!?)(\d+) /g, (mark, event, count, offset) => {
        places.push(offset - removed, expected.length + events[event] * 2 ** 30);
        expected.push(Number(count));
        removed += mark.length;
        return "";
    });
    return { source, places: new Uint32Array(places), expected };
}

function call(f, inputs) {
    return inputs.map((input) => {
        try {
            return f(...input);
        } catch (error) {
            return `threw ${error}`;
        }
    });
}

const rounds = Math.ceil(testLoopCount / 20);

// The counts are those of one call with each of the inputs.
function test(marked, ...inputs) {
    const { source, places, expected } = parse(`(${marked})`);
    const counted = $vm.evaluateWithCoverage(source, places, expected.length);
    const plain = (0, eval)(source);
    shouldBe(counted.toString(), plain.toString(), marked);
    shouldBe(call(counted, inputs), call(plain, inputs), marked);
    drainMicrotasks();
    shouldBe($vm.coverageCounts(counted), expected, marked);
    // The optimizing compilers get to see one input only, and exit for the others.
    for (const input of inputs) {
        for (let i = 0; i < rounds; i++) call(counted, [input]);
    }
    drainMicrotasks();
    shouldBe(
        $vm.coverageCounts(counted),
        expected.map((count) => count * (rounds + 1)),
        marked,
    );
}
