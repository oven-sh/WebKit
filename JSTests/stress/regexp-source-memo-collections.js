//@ defaultRun; run("zombie", "--useZombieMode=true", "--sweepSynchronously=true")

// The string of `source` that a RegExp keeps (stress/regexp-source-memo.js) is the collector's to find. It lives as
// long as its RegExp, where the RegExp is the only thing that keeps it. It outlives its RegExp where something else
// holds it, and a RegExp made again from the text has the same source.

function shouldBe(actual, expected, message)
{
    if (actual !== expected)
        throw new Error(`${message ? message + ": " : ""}expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function sourceOfNewRegExp(text)
{
    return new RegExp(text).source;
}
noInline(sourceOfNewRegExp);

const count = 50;

function checkKept(regExps, addresses, prefix, when)
{
    // What a string that was freed by mistake left behind is taken by these.
    const garbage = [];
    for (let i = 0; i < 4 * count; ++i)
        garbage.push(prefix + "\\/" + i + "/" + when);
    for (let i = 0; i < count; ++i) {
        const source = regExps[i].source;
        if (source !== prefix + "\\/" + i || addressOf(source) !== addresses[i])
            throw new Error(`${when}, RegExp ${prefix}/${i}: ${JSON.stringify(source)}`);
    }
}

function checkOutlived(sources, when)
{
    for (let i = 0; i < count; ++i) {
        shouldBe(sources[i], "once\\/" + i, when);
        shouldBe(sourceOfNewRegExp("once/" + i), "once\\/" + i, when);
    }
}

const young = [];
const addressesOfYoung = [];
const old = [];
const outlived = [];
for (let i = 0; i < count; ++i) {
    const regExp = new RegExp("young/" + i);
    young.push(regExp);
    addressesOfYoung.push(addressOf(regExp.source));
    old.push(new RegExp("old/" + i));
    outlived.push(sourceOfNewRegExp("once/" + i));
}

// A young RegExp with a young string, and strings whose RegExps this collection frees.
edenGC();
checkKept(young, addressesOfYoung, "young", "after an eden collection");
checkOutlived(outlived, "after an eden collection");

// An old RegExp with a young string: the next eden collection finds the string by the write barrier alone.
const addressesOfOld = old.map(regExp => addressOf(regExp.source));
edenGC();
checkKept(old, addressesOfOld, "old", "after an eden collection");

fullGC();
checkKept(young, addressesOfYoung, "young", "after a full collection");
checkKept(old, addressesOfOld, "old", "after a full collection");
checkOutlived(outlived, "after a full collection");
