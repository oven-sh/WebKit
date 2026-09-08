//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-heap §10E: the shared heap keeps the empty marked blocks a collection
// leaves behind, so a steady allocation loop runs on a fixed block set after
// warm-up. Before the seventh landing round the cycle-end reclamation freed
// every empty block after nearly every collection GIL off, and each cycle
// minted its whole eden again (thousands of 16 KB blocks per second through
// the block allocator and first-touch faults). Counts blocks minted across
// a steady phase; a retained heap mints a handful (new size classes, growth),
// a churning one mints thousands.
load("./harness.js", "caller relative");

new Thread(() => 1).join(); // the heap is shared from here on in every mode

function churn(rounds) {
    let keep = 0;
    for (let r = 0; r < rounds; ++r) {
        // ~8 MB of short-lived objects per round, dead within a few hundred
        // allocations of their birth, so every collection finds about the same
        // small live set and the heap's working size is flat in every mode.
        for (let c = 0; c < 80; ++c) {
            let a = [];
            for (let i = 0; i < 500; ++i) a.push({ i, r, s: "x" + i, t: [i, r] });
            keep += a[c & 7].i;
        }
    }
    return keep;
}

// Warm-up: tier-up, and two full eden->Full periods so the heap has reached
// the working size it will cycle through below.
churn(120);
const before = $vm.markedBlocksCreated();
churn(120);
const minted = $vm.markedBlocksCreated() - before;
if (typeof AMPLIFY_VERBOSE !== "undefined") print("marked blocks minted in the steady phase: " + minted);
// A heap that keeps its blocks mints next to nothing here in any mode (0-100:
// a new size class, a slightly larger eden); the sixth-round shared heap freed
// its empty blocks after nearly every collection and minted its whole eden
// again each cycle (thousands over this phase).
if (minted > 1000) throw new Error("steady allocation loop minted " + minted + " marked blocks (block set not retained across collections?)");
print("PASS");
