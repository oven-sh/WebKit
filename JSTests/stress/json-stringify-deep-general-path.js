// The general Stringifier (the path taken when FastStringifier bails out) detects cycles
// with a scan of the holder stack for shallow values, and with a set of the holders past
// holderScanLimit (256) ancestors, which it lets go again below half the limit. The cycle
// check of a chain of depth N then costs O(N), not O(N^2).

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected.length > 200 ? expected.slice(0, 100) + "..." : expected} but got ${actual.length > 200 ? actual.slice(0, 100) + "..." : actual}`);
}

function shouldThrow(fn, errorMessage, what) {
    let error;
    try {
        fn();
    } catch (e) {
        error = e;
    }
    if (!error)
        throw new Error(`${what}: did not throw`);
    if (String(error) !== errorMessage)
        throw new Error(`${what}: expected "${errorMessage}" but got "${String(error)}"`);
}

// An array chain of `depth` arrays. nodes[i] has i ancestors; nodes[depth - 1] is the innermost array.
function arrayChain(depth) {
    const root = JSON.parse("[".repeat(depth) + "]".repeat(depth));
    const nodes = [];
    for (let node = root; node; node = node[0])
        nodes.push(node);
    return nodes;
}

// An object chain {"a":{"a":...}} of `depth` objects.
function objectChain(depth) {
    const root = JSON.parse('{"a":'.repeat(depth) + "0" + "}".repeat(depth));
    const nodes = [];
    for (let node = root; typeof node === "object"; node = node.a)
        nodes.push(node);
    return nodes;
}

const identity = (key, value) => value;
const cyclicMessage = "TypeError: JSON.stringify cannot serialize cyclic structures.";
const stackOverflowMessage = "RangeError: Maximum call stack size exceeded.";

// Deep acyclic values on every route into the general Stringifier.
{
    const depth = 30000;
    const arrays = arrayChain(depth);
    const arrayText = "[".repeat(depth) + "]".repeat(depth);
    shouldBe(JSON.stringify(arrays[0], identity), arrayText, "array chain, replacer function");
    shouldBe(JSON.stringify(arrays[0], ["a"]), arrayText, "array chain, replacer array");

    const objects = objectChain(depth);
    const objectText = '{"a":'.repeat(depth) + "0" + "}".repeat(depth);
    shouldBe(JSON.stringify(objects[0], identity), objectText, "object chain, replacer function");
    shouldBe(JSON.stringify(objects[0], ["a"]), objectText, "object chain, replacer array");

    arrays[depth - 1][0] = new Date(0);
    shouldBe(JSON.stringify(arrays[0]), "[".repeat(depth) + '"1970-01-01T00:00:00.000Z"' + "]".repeat(depth), "array chain, Date leaf");

    arrays[depth - 1][0] = new Proxy({ p: 1 }, {});
    shouldBe(JSON.stringify(arrays[0]), "[".repeat(depth) + '{"p":1}' + "]".repeat(depth), "array chain, Proxy leaf");

    arrays[depth - 1][0] = { toJSON() { return "leaf"; } };
    shouldBe(JSON.stringify(arrays[0]), "[".repeat(depth) + '"leaf"' + "]".repeat(depth), "array chain, toJSON leaf");

    arrays[depth - 1][0] = Object.defineProperty({}, "g", { get() { return 1; }, enumerable: true });
    shouldBe(JSON.stringify(arrays[0]), "[".repeat(depth) + '{"g":1}' + "]".repeat(depth), "array chain, getter leaf");
}

// With a gap the output itself is quadratic in the depth, so this uses a shallower chain. A replacer
// or a Date leaf forces the general path, which indents between a push and its pop.
{
    const depth = 2000;
    const objects = objectChain(depth);
    let expected = "0";
    for (let i = depth - 1; i >= 0; --i)
        expected = `{\n${" ".repeat(i + 1)}"a": ${expected}\n${" ".repeat(i)}}`;
    shouldBe(JSON.stringify(objects[0], identity, 1), expected, "object chain, gap and replacer function");

    const arrays = arrayChain(depth);
    arrays[depth - 1][0] = new Date(0);
    expected = '"1970-01-01T00:00:00.000Z"';
    for (let i = depth - 1; i >= 0; --i)
        expected = `[\n${" ".repeat(i + 1)}${expected}\n${" ".repeat(i)}]`;
    shouldBe(JSON.stringify(arrays[0], null, 1), expected, "array chain, gap and Date leaf");

    arrays[depth - 1][0] = arrays[0];
    shouldThrow(() => JSON.stringify(arrays[0], identity, 1), cyclicMessage, "2000-deep cycle, gap and replacer function");
}

// A back-edge from nodes[source] to nodes[target] is a cycle, wherever the two sit relative
// to the scan limit: nodes[256] is the holder that fills the set, and nodes[255] and nodes[257]
// are its neighbours.
{
    const depth = 1000;
    const positions = [0, 1, 254, 255, 256, 257, 258, 999];
    for (const target of positions) {
        for (const source of positions) {
            if (source <= target)
                continue;
            const nodes = arrayChain(depth);
            nodes[source][0] = nodes[target];
            shouldThrow(() => JSON.stringify(nodes[0], identity), cyclicMessage, `back-edge ${source} -> ${target}, replacer function`);
            shouldThrow(() => JSON.stringify(nodes[0]), cyclicMessage, `back-edge ${source} -> ${target}`);

            const objects = objectChain(depth);
            objects[source].a = objects[target];
            shouldThrow(() => JSON.stringify(objects[0], ["a"]), cyclicMessage, `object back-edge ${source} -> ${target}, replacer array`);
        }
    }
}

// A deep cycle is found in O(depth).
{
    const depth = 30000;
    const nodes = arrayChain(depth);
    nodes[depth - 1][0] = nodes[0];
    shouldThrow(() => JSON.stringify(nodes[0], identity), cyclicMessage, "30000-deep cycle, replacer function");
    shouldThrow(() => JSON.stringify(nodes[0]), cyclicMessage, "30000-deep cycle");
    shouldThrow(() => JSON.stringify({ data: nodes[0] }), cyclicMessage, "30000-deep cycle under a key");
}

// An object reached twice through different holders is not a cycle, also past the scan limit.
{
    const depth = 300;
    const shared = { x: 1 };
    const nodes = arrayChain(depth);
    nodes[depth - 1].push(shared, shared, [shared]);
    shouldBe(JSON.stringify(nodes[0], identity), "[".repeat(depth) + '{"x":1},{"x":1},[{"x":1}]' + "]".repeat(depth), "shared object past the scan limit");

    const objects = objectChain(depth);
    objects[depth - 1].a = { first: shared, second: shared };
    shouldBe(JSON.stringify(objects[0], identity), '{"a":'.repeat(depth) + '{"first":{"x":1},"second":{"x":1}}' + "}".repeat(depth), "shared object in an object chain past the scan limit");
}

// A toJSON or a replacer that returns an ancestor makes a cycle.
{
    const depth = 300;
    const nodes = arrayChain(depth);
    nodes[depth - 1][0] = { toJSON() { return nodes[100]; } };
    shouldThrow(() => JSON.stringify(nodes[0]), cyclicMessage, "toJSON returns an ancestor past the scan limit");

    const objects = objectChain(depth);
    shouldThrow(() => JSON.stringify(objects[0], (key, value) => value === 0 ? objects[10] : value), cyclicMessage, "replacer returns an ancestor past the scan limit");
}

// Subtrees on both sides of the scan limit, one after the other.
{
    const depths = [];
    for (let i = 0; i < 100; ++i)
        depths.push(i % 2 ? 250 : 270);
    const root = [];
    let expected = "[";
    for (const depth of depths) {
        const nodes = arrayChain(depth);
        nodes[depth - 1][0] = new Date(0);
        root.push(nodes[0]);
        expected += "[".repeat(depth) + '"1970-01-01T00:00:00.000Z"' + "]".repeat(depth) + ",";
    }
    expected = expected.slice(0, -1) + "]";
    shouldBe(JSON.stringify(root), expected, "subtrees around the scan limit");
}

// The cycle check runs before the depth cap (maximumSideStackRecursion, 40000), on both sides of
// the scan limit. A holder with 40000 ancestors is the first one the cap rejects.
{
    const depth = 40002;
    for (const source of [39998, 39999, 40000, 40001]) {
        const nodes = arrayChain(depth);
        nodes[source][0] = nodes[0];
        const message = source < 40000 ? cyclicMessage : stackOverflowMessage;
        shouldThrow(() => JSON.stringify(nodes[0], identity), message, `back-edge ${source} -> 0 near the cap, replacer function`);
        shouldThrow(() => JSON.stringify(nodes[0]), message, `back-edge ${source} -> 0 near the cap`);
    }
    shouldBe(JSON.stringify(arrayChain(40000)[0], identity), "[".repeat(40000) + "]".repeat(40000), "40000-deep chain, replacer function");
    shouldThrow(() => JSON.stringify(arrayChain(40001)[0], identity), stackOverflowMessage, "40001-deep chain, replacer function");
}

// Past maximumSideStackRecursion the general Stringifier still throws a RangeError, fast.
{
    const depth = 200000;
    const root = JSON.parse("[".repeat(depth) + "]".repeat(depth));
    shouldThrow(() => JSON.stringify(root, identity), stackOverflowMessage, "200000-deep chain, replacer function");

    const fresh = { toJSON() { return { k: fresh }; } };
    shouldThrow(() => JSON.stringify(fresh), stackOverflowMessage, "toJSON that nests a fresh object per level");
}
