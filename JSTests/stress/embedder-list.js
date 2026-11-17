function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${String(expected)}, got ${String(actual)}`);
}

function read(list, index) { return list[index]; }
noInline(read);

function readOntoBase(list, index) { list = list[index]; return list; }
noInline(readOntoBase);

function sum(list, length) {
    let result = 0;
    for (let i = 0; i < length; ++i)
        result += list[i].value;
    return result;
}
noInline(sum);

const make = (first, length) => Array.from({ length }, (_, i) => ({ value: first + i }));

// The object is asked once, and then again only for what it has not published.
{
    const items = make(0, 8);
    const list = $vm.createEmbedderList(items);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(read(list, i & 7), items[i & 7]);
        shouldBe(readOntoBase(list, i & 7), items[i & 7]);
    }
    const asked = $vm.timesEmbedderListWasAsked(list);
    if ($vm.useJIT() && !jscOptions().useConcurrentJIT) {
        for (let i = 0; i < 100; ++i)
            read(list, i & 7);
        shouldBe($vm.timesEmbedderListWasAsked(list), asked);
    }
    for (const index of [8, 9, -1, 0x7fffffff, 0xfffffffe, 1.5, "8"])
        shouldBe(read(list, index), undefined);
    shouldBe(read(list, "3"), items[3]);
}

// Holes are not published.
{
    const list = $vm.createEmbedderList([1, , 3]);
    Object.setPrototypeOf(list, { 1: "above" });
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(read(list, i % 3), [1, "above", 3][i % 3]);
}

// The clock moves between and inside loops.
{
    const list = $vm.createEmbedderList(make(0, 4));
    const other = $vm.createEmbedderList(make(7, 1));
    for (let i = 0; i < testLoopCount; ++i) {
        const length = 1 + i % 5;
        $vm.changeEmbedderList(list, make(i, length));
        shouldBe(sum(list, length), i * length + length * (length - 1) / 2);
        shouldBe(read(list, length), undefined);
        shouldBe(read(other, 0).value, 7);
    }

    function readAround(list, next) {
        const before = list[0];
        $vm.changeEmbedderList(list, next);
        return [before, list[0]];
    }
    noInline(readAround);
    $vm.changeEmbedderList(list, [-1]);
    for (let i = 0; i < testLoopCount; ++i) {
        const [before, after] = readAround(list, [i]);
        shouldBe(before, i - 1);
        shouldBe(after, i);
    }
}

// Among other kinds of bases.
{
    const bases = [[5], $vm.createEmbedderList([5]), new Uint8Array([5]), (function () { return arguments; })(5), { 0: 5 }, [5, 0.5]];
    function readAny(base) { return base[0]; }
    noInline(readAny);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(readAny(bases[i % bases.length]), 5);
}

// Current items survive collections, and those that are not are let go.
{
    const lists = Array.from({ length: 50 }, (_, i) => $vm.createEmbedderList(make(i, 20)));
    for (let round = 0; round < 20; ++round) {
        lists.forEach((list, i) => shouldBe(sum(list, 20), i * 20 + 190));
        if (round & 1)
            $vm.changeEmbedderList($vm.createEmbedderList([]), []);
        round & 2 ? edenGC() : fullGC();
        if ((round & 3) === 1)
            shouldBe($vm.embedderListHoldsItems(lists[0]), false);
        make(0, 1000);
    }
}
