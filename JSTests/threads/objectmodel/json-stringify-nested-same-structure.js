//@ requireOptions("--useJSThreads=1")
// JSON.stringify's fast path walks a Structure's property table with
// Structure::forEachProperty and recurses into nested objects from inside the
// walk. Nested objects that share their parent's shape share its Structure,
// so flag-on the walk must not run the per-object functor while holding that
// Structure's non-recursive m_lock: doing so self-deadlocks on the very first
// nested same-shape object. Single-threaded and deterministic.
load("../harness.js", "caller relative");

function check(value, expected) {
    const actual = JSON.stringify(value);
    if (actual !== expected)
        throw new Error("JSON.stringify produced " + actual + ", expected " + expected);
}

// Parent and child share one Structure (same empty structure, same add transition).
check({ a: { a: 1 } }, '{"a":{"a":1}}');

// Linked-list shape: every node has the same shape as the next.
let list = null;
for (let i = 0; i < 50; ++i)
    list = { value: i, next: list };
const listJSON = JSON.stringify(list);
if (!listJSON.startsWith('{"value":49,"next":{"value":48,"next":'))
    throw new Error("unexpected linked-list JSON prefix: " + listJSON.slice(0, 64));
if (JSON.parse(listJSON).next.next.value !== 47)
    throw new Error("linked-list JSON does not round-trip");

// Tree shape: siblings and parents share Structures; the walk descends into
// both children of every node.
function tree(depth) {
    if (!depth)
        return { left: null, right: null, depth: 0 };
    return { left: tree(depth - 1), right: tree(depth - 1), depth };
}
const treeJSON = JSON.stringify(tree(6));
if (JSON.parse(treeJSON).left.right.depth !== 4)
    throw new Error("tree JSON does not round-trip");

// Nested arrays of same-shape objects, several times in one stringify.
const rows = [];
for (let i = 0; i < 20; ++i)
    rows.push({ id: i, payload: { id: i, payload: { id: i, payload: null } } });
check(rows.slice(0, 1), '[{"id":0,"payload":{"id":0,"payload":{"id":0,"payload":null}}}]');
for (let i = 0; i < 10; ++i) {
    if (JSON.parse(JSON.stringify(rows))[19].payload.payload.id !== 19)
        throw new Error("rows JSON does not round-trip");
}

// The slow path (Stringifier::Holder) walks the same Structures with a
// replacer/indent and must agree with the fast path.
if (JSON.stringify({ a: { a: 1 } }, null, 1) !== '{\n "a": {\n  "a": 1\n }\n}')
    throw new Error("indented JSON.stringify of nested same-shape objects is wrong");
