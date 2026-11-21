// What few RegExps are asked for is in one struct behind one pointer of the cell: the string of `source`, and the
// first-character filter that compiled code asks a constant RegExp for. Whichever is asked for first allocates the
// struct. stress/regexp-source-memo.js has the values of `source`.

function shouldBe(actual, expected, message)
{
    if (actual !== expected)
        throw new Error(`${message ? message + ": " : ""}expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

// A heap snapshot counts the struct that keeps the string: a RegExp whose source was read is two words larger, and
// one that nothing asked is as large as it was. This comes first, while no RegExp of this file can die.
{
    function countsOfRegExpSizes()
    {
        const snapshot = generateHeapSnapshot();
        const className = snapshot.nodeClassNames.indexOf("RegExp");
        const counts = new Map;
        for (let i = 0; i < snapshot.nodes.length; i += 4) {
            if (snapshot.nodes[i + 2] === className)
                counts.set(snapshot.nodes[i + 1], (counts.get(snapshot.nodes[i + 1]) || 0) + 1);
        }
        return counts;
    }

    const count = 50;
    const read = [];
    const unread = [];
    for (let i = 0; i < count; ++i) {
        read.push(new RegExp("size/read" + i));
        unread.push(new RegExp("size/unread" + i));
    }
    const before = countsOfRegExpSizes();
    let length = 0;
    for (const regExp of read)
        length += regExp.source.length;
    shouldBe(length > count * 10, true);
    const after = countsOfRegExpSizes();

    const changes = [];
    for (const size of new Set([...before.keys(), ...after.keys()])) {
        const change = (after.get(size) || 0) - (before.get(size) || 0);
        if (change)
            changes.push([size, change]);
    }
    changes.sort((a, b) => a[0] - b[0]);
    shouldBe(changes.length, 2, JSON.stringify(changes));
    const [[sizeBefore, left], [sizeAfter, came]] = changes;
    shouldBe(JSON.stringify([left, came, sizeAfter - sizeBefore]), JSON.stringify([-count, count, 16]));
    // Both lists are read here, behind the second snapshot: optimized code keeps a list no longer than its last use,
    // and a RegExp that died between the snapshots would change the counts too.
    shouldBe(read.length + unread.length, 2 * count);
}

// The first-character filter of a constant RegExp and its source are allocated together, by whichever is asked for
// first. A compiler thread asks for the filter, for exec() of an anchored RegExp in DFG and FTL code. This comes
// before the first compile() of this realm: after one, the DFG takes no RegExp for a constant.
{
    function execSourceReadFirst(string)
    {
        return /^(?:filter\/first|case|return)$/.exec(string) !== null;
    }
    noInline(execSourceReadFirst);

    function execSourceReadLast(string)
    {
        return /^(?:filter\/last|case|return)$/.exec(string) !== null;
    }
    noInline(execSourceReadLast);

    function runAll()
    {
        const subjects = ["filter/first", "filter/last", "case", "config", "other", "", ";", "\u3042"];
        for (let i = 0; i < testLoopCount; ++i) {
            const subject = subjects[i % subjects.length];
            if (execSourceReadFirst(subject) !== (subject === "filter/first" || subject === "case"))
                throw new Error(`execSourceReadFirst(${JSON.stringify(subject)}), iteration ${i}`);
            if (execSourceReadLast(subject) !== (subject === "filter/last" || subject === "case"))
                throw new Error(`execSourceReadLast(${JSON.stringify(subject)}), iteration ${i}`);
        }
    }

    shouldBe(/^(?:filter\/first|case|return)$/.source, "^(?:filter\\/first|case|return)$");
    runAll();
    shouldBe(/^(?:filter\/last|case|return)$/.source, "^(?:filter\\/last|case|return)$");
    shouldBe(/^(?:filter\/first|case|return)$/.source, "^(?:filter\\/first|case|return)$");
    runAll();
}
