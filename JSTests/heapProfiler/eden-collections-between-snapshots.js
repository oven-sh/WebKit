var SimpleObject = $vm.SimpleObject;

// A retained snapshot is only pruned at the end of a full collection. These checks pin what
// that relies on: a cell that a snapshot holds survives every eden collection, so its node
// keeps its identifier, and the node of a cell that dies leaves the chain in the full
// collection that frees it, before anything can reuse the address.

function assert(condition, reason) {
    if (!condition)
        throw new Error(reason);
}

// "nodes" is a flat list of <nodeId>, <sizeInBytes>, <nodeClassNameIndex>, <flags>.
const nodeFieldCount = 4;

function simpleObjectIds() {
    let {nodes, nodeClassNames} = generateHeapSnapshot();
    let classNameIndex = nodeClassNames.indexOf("SimpleObject");
    let ids = [];
    for (let i = 0; i < nodes.length; i += nodeFieldCount) {
        if (nodes[i + 2] === classNameIndex)
            ids.push(nodes[i]);
    }
    return ids.sort((a, b) => a - b);
}

function edenCollections() {
    for (let round = 0; round < 20; ++round) {
        let garbage = [];
        for (let i = 0; i < 1000; ++i)
            garbage.push({ i });
        edenGC();
    }
}

// Every object lives in a property of `held`, so clearing the property is what frees it.
let held = {};

// Identifiers follow marking order, not allocation order, so each object gets a snapshot of its own.
(function() { held.survivor = new SimpleObject; })();
let survivorId;
(function() {
    let ids = simpleObjectIds();
    assert(ids.length === 1, "the first snapshot should hold 1 'SimpleObject' instance, has " + ids.length);
    survivorId = ids[0];
})();

(function() { held.doomed = new SimpleObject; })();
let doomedId;
(function() {
    let ids = simpleObjectIds();
    assert(ids.length === 2, "the second snapshot should hold 2 'SimpleObject' instances, has " + ids.length);
    assert(ids[0] === survivorId, "'survivor' should keep its identifier");
    doomedId = ids[1];
})();

edenCollections();

(function() { held.lateArrival = new SimpleObject; })();
edenGC();

let lateArrivalId;
(function() {
    let ids = simpleObjectIds();
    assert(ids.length === 3, "the third snapshot should hold 3 'SimpleObject' instances, has " + ids.length);
    assert(ids[0] === survivorId, "'survivor' should keep its identifier across eden collections");
    assert(ids[1] === doomedId, "'doomed' should keep its identifier across eden collections");
    lateArrivalId = ids[2];
})();

held.doomed = null;
edenCollections();
fullGC();

// 'doomed' is gone, so fresh objects are free to land on the address it had.
(function() {
    held.replacements = [];
    for (let i = 0; i < 1000; ++i)
        held.replacements.push(new SimpleObject);
})();
edenGC();

(function() {
    let ids = simpleObjectIds();
    assert(ids.length === 1002, "the last snapshot should hold 1002 'SimpleObject' instances, has " + ids.length);
    assert(ids[0] === survivorId, "'survivor' should still have its identifier");
    assert(ids[1] === lateArrivalId, "'lateArrival' should still have its identifier");
    assert(!ids.includes(doomedId), "no object should inherit the identifier of 'doomed'");
    assert(ids[2] > lateArrivalId, "every replacement should get a new, larger identifier");
})();
