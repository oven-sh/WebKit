//@ skip if $architecture == "arm"
//@ runDefault; runNoCJITCollectContinuously; runLockdown; runMiniMode

// A resolved rope owns a StringImpl, so its MarkedBlock has to run destructors at the next sweep.
// The first resolve in a block sets the block's destructible bit, and the later resolves in that
// block find it set. If a resolve leaves the bit clear, the sweep skips the destructors and the
// StringImpl leaks. Nothing else shows that, so this test resolves large ropes, drops them, and
// bounds the memory footprint.
//
// fullGC() and edenGC() leave the blocks to the sweep that runs when a block is allocated from again.
// gc() would also sweep synchronously and free the empty blocks, which hides a missing bit.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + " expected " + expected);
}
noInline(shouldBe);

const roundCount = 24;
const warmUpRoundCount = 4;
const ropesPerRound = 1000;
const megabyte = 1024 * 1024;
// Each round resolves about 8 MB of strings. With the bit set, a round's strings die at the next
// collections and the footprint stays flat. Without it, the 20 measured rounds keep 160 MB.
const footprintGrowthLimit = 64 * megabyte;
// ASan keeps freed memory in its quarantine, so the footprint grows there with nothing leaked.
const footprintIsMeaningful = !(typeof $vm !== "undefined" && $vm.asanEnabled());

const left8 = "a".repeat(4096);
const right8 = "b".repeat(4096);
const left16 = "\u3042".repeat(2048);
const right16 = "\u3044".repeat(2048);
const shortLeft = "c".repeat(1000);
const shortRight = "d".repeat(990);

function resolve8(i) {
    let rope = left8 + right8 + i;
    shouldBe(rope.charCodeAt(0), 97);
    shouldBe(rope.charCodeAt(8191), 98);
    return rope;
}
noInline(resolve8);

function resolve16(i) {
    let rope = left16 + right16 + i;
    shouldBe(rope.charCodeAt(0), 0x3042);
    shouldBe(rope.charCodeAt(4095), 0x3044);
    return rope;
}
noInline(resolve16);

let map = new Map;
function resolveSubstring(i) {
    let base = left8 + right8 + i;
    shouldBe(base.charCodeAt(0), 97);
    // A substring of a resolved string is a rope that points into its base. A hash lookup resolves it.
    let substring = base.substring(1, 8100 + String(i).length);
    shouldBe(map.has(substring), false);
    shouldBe(substring.length, 8099 + String(i).length);
    return substring;
}
noInline(resolveSubstring);

let object = { };
function resolveAsPropertyKey(i) {
    // Short enough for the on-stack resolve of a property key, which leaves the rope holding an atom.
    let rope = shortLeft + shortRight + i;
    shouldBe(object[rope], undefined);
    let longRope = left8 + right8 + i;
    shouldBe(object[longRope], undefined);
    return rope;
}
noInline(resolveAsPropertyKey);

function test(name, resolve, check) {
    // Some ropes survive several rounds, so blocks stay partly live across collections. The others die,
    // so blocks also become empty and are allocated from again.
    let survivors = new Array(64).fill(null);
    let footprintAfterWarmUp = 0;
    for (let round = 0; round < roundCount; ++round) {
        for (let i = 0; i < ropesPerRound; ++i) {
            let rope = resolve(round * ropesPerRound + i);
            if (!(i & 127))
                survivors[(round * 8 + (i >> 7)) & 63] = rope;
        }
        if (round & 1)
            fullGC();
        else
            edenGC();
        if (round + 1 === warmUpRoundCount)
            footprintAfterWarmUp = MemoryFootprint().current;
    }
    for (let survivor of survivors)
        check(survivor);
    let growth = MemoryFootprint().current - footprintAfterWarmUp;
    if (footprintIsMeaningful && growth > footprintGrowthLimit)
        throw new Error(name + ": the footprint grew by " + Math.round(growth / megabyte) + " MB while it resolved and dropped ropes");
}

test("8-bit", resolve8, rope => {
    shouldBe(rope.startsWith(left8 + right8), true);
});
test("16-bit", resolve16, rope => {
    shouldBe(rope.startsWith(left16 + right16), true);
});
test("substring", resolveSubstring, rope => {
    shouldBe(rope.startsWith(left8.substring(1) + right8.substring(0, 4)), true);
});
test("property key", resolveAsPropertyKey, rope => {
    shouldBe(rope.startsWith(shortLeft + shortRight), true);
});

// Ropes that are never resolved share blocks with resolved ones.
for (let round = 0; round < 4; ++round) {
    let unresolved = [];
    for (let i = 0; i < 20000; ++i) {
        let rope = "0123456789abcdef012345/" + i;
        if (i & 1)
            shouldBe(rope.charCodeAt(0), 48);
        else if (!(i & 255))
            unresolved.push(rope);
    }
    fullGC();
    for (let i = 0; i < unresolved.length; ++i)
        shouldBe(unresolved[i], "0123456789abcdef012345/" + (i * 256));
}
