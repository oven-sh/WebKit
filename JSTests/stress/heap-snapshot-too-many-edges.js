//@ runDefault("--heapSnapshotBuilderEdgeLimitForTesting=50000")

// A heap snapshot that cannot hold all of its edges has to fail. It must not abort the process, and it must not
// return a document that lacks some of the edges. The option stands in for the real limit of 51,821,028 edges,
// which a heap reaches only with gigabytes of memory.

class Marker { }
let marker = new Marker;
let shared = { };
let holder = [];

function outcome(generate)
{
    try {
        return generate();
    } catch (error) {
        return String(error);
    }
}

function assertBuilt(generate, snapshot, when)
{
    let built = generate === generateHeapSnapshot ? snapshot.type === "Inspector" : snapshot.startsWith("{\"version\":");
    if (!built)
        throw new Error(generate.name + "() " + when + " should build a snapshot, not '" + snapshot + "'");
}

// The identifier that an Inspector snapshot gives to the one Marker.
function identifierOfMarker(snapshot)
{
    let className = snapshot.nodeClassNames.indexOf("Marker");
    for (let i = 0; i < snapshot.nodes.length; i += 4) {
        if (snapshot.nodes[i + 2] === className)
            return snapshot.nodes[i];
    }
    throw new Error("The snapshot has no Marker");
}

for (let generate of [generateHeapSnapshot, generateHeapSnapshotForGCDebugging]) {
    let first = outcome(generate);
    assertBuilt(generate, first, "under the limit");

    // Each element is an edge from the array.
    holder = new Array(60000).fill(shared);
    let second = outcome(generate);
    if (second !== "RangeError: Out of memory")
        throw new Error(generate.name + "() over the limit should throw an out of memory error, not '" + second + "'");

    holder.length = 0;
    let third = outcome(generate);
    assertBuilt(generate, third, "under the limit again");

    // A snapshot that failed changes nothing for the next one: a cell keeps the identifier it had.
    if (generate === generateHeapSnapshot && identifierOfMarker(third) !== identifierOfMarker(first))
        throw new Error("The Marker should keep the identifier " + identifierOfMarker(first) + ", not get " + identifierOfMarker(third));
}
