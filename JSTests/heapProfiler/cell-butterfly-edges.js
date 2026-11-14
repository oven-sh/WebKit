load("./driver/driver.js");

// What a JSCellButterfly references: the entries of a Map or a Set (it is their table, and the table an iterator was
// created on), the arguments of a bound function past the third, and the elements of an array that has it as storage.

class Holders { }
class MapKey { }
class MapValue { }
class SetMember { }
class GroupedValue { }
class BoundArgument { }
class ValueInReplacedTable { }
class MemberOfClearedTable { }
class SpreadElement { }
class ElementOfSuspendedSpread { }
class AddressAsDouble { }
class DeletedKey { }
class DeletedValue { }
class OverwrittenValue { }
class DeletedMember { }
class ClearedMember { }

let holders = new Holders;

// In a function: `deletedKey` and `deletedMember` have to be gone when the snapshot is taken. At the top level they
// would be variables of the global lexical environment, which holds them for the whole script.
function createHolders() {
    holders.mapOfKeys = new Map([[new MapKey, 1]]);
    holders.mapOfValues = new Map([["key", new MapValue]]);
    holders.set = new Set([new SetMember]);
    holders.grouped = Map.groupBy([new GroupedValue], () => "group");
    holders.bound = (function () { }).bind(null, 1, 2, 3, new BoundArgument);

    // An iterator keeps the table it was created on until it is advanced. A table that grew was replaced, and the
    // replaced one still has the entries it had.
    let grown = new Map([["key", new ValueInReplacedTable]]);
    holders.iteratorOfReplacedTable = grown.values();
    for (let i = 0; i < 100; ++i)
        grown.set(i, i);
    grown.delete("key");
    holders.grown = grown;

    let cleared = new Set([new MemberOfClearedTable]);
    holders.iteratorOfClearedTable = cleared.values();
    cleared.clear();
    holders.cleared = cleared;

    // An array made by a spread has a JSCellButterfly as storage until it is written to.
    holders.spread = [...[new SpreadElement]];

    // The arguments of a call with a spread are in a JSCellButterfly that no array has as storage. A generator that
    // is suspended before the call keeps it in its frame.
    holders.suspended = (function* () { Math.max(...[new ElementOfSuspendedSpread], yield); })();
    holders.suspended.next();

    // A literal of non-integer numbers has storage whose slots are raw doubles. addressOf() gives the double that has
    // the bits of a cell's address: read as a value, that slot would be a reference to the cell.
    holders.addressAsDouble = new AddressAsDouble;
    holders.doubles = new Function("return [" + addressOf(holders.addressAsDouble) + ", 1.5];")();

    // Entries that are gone, with no iterator: nothing keeps them alive.
    let deletedKey = new DeletedKey;
    holders.mapWithDeletedEntry = new Map([[deletedKey, new DeletedValue], ["kept", "kept"]]);
    holders.mapWithDeletedEntry.delete(deletedKey);
    holders.mapWithOverwrittenValue = new Map([["key", new OverwrittenValue]]);
    holders.mapWithOverwrittenValue.set("key", 1);
    let deletedMember = new DeletedMember;
    holders.setWithDeletedMember = new Set([deletedMember, "kept"]);
    holders.setWithDeletedMember.delete(deletedMember);
    holders.clearedSet = new Set([new ClearedMember]);
    holders.clearedSet.clear();
}
noInline(createHolders);
createHolders();

assert(/CopyOnWriteArrayWithContiguous/.test(describe(holders.spread)), "holders.spread should have copy-on-write storage: " + describe(holders.spread));
assert(/CopyOnWriteArrayWithDouble/.test(describe(holders.doubles)), "holders.doubles should have copy-on-write storage of doubles: " + describe(holders.doubles));
assert(holders.doubles[0] === addressOf(holders.addressAsDouble), "holders.doubles[0] should have the bits of the address");

let snapshot = createHeapSnapshot();

function onlyNodeWithClassName(className) {
    let nodes = snapshot.nodesWithClassName(className);
    assert(nodes.length === 1, "Snapshot should contain 1 '" + className + "' instance, has " + nodes.length);
    return nodes[0];
}

function excludeStructure(edges) {
    return edges.filter((x) => x.to.className !== "Structure");
}

let holdersNode = onlyNodeWithClassName("Holders");

// The edges to the only `className` instance. Each must be an Index edge from a 'Cell Butterfly', and `holder`,
// a property of `holders`, must reference one of those by an Internal edge.
function edgesFromCellButterflyOf(className, holder) {
    let node = onlyNodeWithClassName(className);
    assert(node.incomingEdges.length, "'" + className + "' should have an incoming edge");
    for (let edge of node.incomingEdges) {
        assert(edge.from.className === "Cell Butterfly", "'" + className + "' should be referenced by a 'Cell Butterfly', not by '" + edge.from.className + "'");
        assert(edge.type === "Index", "'" + className + "' should be referenced by an Index edge, not by " + edge.type);
    }
    let holderNode = followPath(holdersNode, [{edge: holder}]);
    let fromHolder = node.incomingEdges.filter((edge) => edge.from.incomingEdges.some((e) => e.from === holderNode && e.type === "Internal"));
    assert(fromHolder.length === 1, "'" + className + "' should be referenced by the 'Cell Butterfly' of holders." + holder);
    return node.incomingEdges;
}

assert(edgesFromCellButterflyOf("MapKey", "mapOfKeys").length === 1, "'MapKey' should have 1 incoming edge");
assert(edgesFromCellButterflyOf("MapValue", "mapOfValues").length === 1, "'MapValue' should have 1 incoming edge");
assert(edgesFromCellButterflyOf("SetMember", "set").length === 1, "'SetMember' should have 1 incoming edge");

let boundArgumentEdges = edgesFromCellButterflyOf("BoundArgument", "bound");
assert(boundArgumentEdges.length === 1 && boundArgumentEdges[0].data === 3, "The fourth bound argument should be at index 3");

// The Map holds an array that holds the value: the table has to reference that array.
let groupedValue = onlyNodeWithClassName("GroupedValue");
let group = followPath(holdersNode, [{edge: "grouped"}, {node: "Cell Butterfly"}, {node: "Array"}]);
assert(groupedValue.incomingEdges.length === 1 && groupedValue.incomingEdges[0].from === group, "'GroupedValue' should be referenced by the array that the table of holders.grouped references");

// Each table that was replaced while it held the entry still references it, and the table that took its place.
edgesFromCellButterflyOf("ValueInReplacedTable", "iteratorOfReplacedTable");
assert(edgesFromCellButterflyOf("MemberOfClearedTable", "iteratorOfClearedTable").length === 1, "'MemberOfClearedTable' should have 1 incoming edge");
for (let holder of ["iteratorOfReplacedTable", "iteratorOfClearedTable"]) {
    let table = followPath(holdersNode, [{edge: holder}, {node: "Cell Butterfly"}]);
    let nextTables = table.outgoingEdges.filter((edge) => edge.to.className === "Cell Butterfly");
    assert(nextTables.length === 1 && nextTables[0].type === "Index" && nextTables[0].data === 0, "The table of holders." + holder + " should reference the next table at index 0");
}

// The array reports its elements itself, and so does its storage, which cannot tell that an array owns it.
let spreadElement = onlyNodeWithClassName("SpreadElement");
let spread = followPath(holdersNode, [{edge: "spread"}]);
let storageOfSpread = followPath(spread, [{node: "Cell Butterfly"}]);
assert(spreadElement.incomingEdges.length === 2, "'SpreadElement' should have 2 incoming edges, has " + spreadElement.incomingEdges.length);
for (let edge of spreadElement.incomingEdges)
    assert(edge.type === "Index" && edge.data === 0 && (edge.from === spread || edge.from === storageOfSpread), "'SpreadElement' should be referenced at index 0 by holders.spread and by its storage, not by '" + edge.from.className + "'");

// No array owns the spread of the suspended call: only the JSCellButterfly itself can report what it holds.
let elementOfSuspendedSpread = onlyNodeWithClassName("ElementOfSuspendedSpread");
let suspendedSpreads = elementOfSuspendedSpread.incomingEdges.filter((edge) => edge.type === "Index" && edge.from.className === "Cell Butterfly" && !edge.from.incomingEdges.some((e) => e.from.className === "Array"));
assert(suspendedSpreads.length === 1, "'ElementOfSuspendedSpread' should be referenced by 1 'Cell Butterfly' that is not the storage of an array, is by " + suspendedSpreads.length);

// Storage of doubles has no values: nothing in it is a reference.
let storageOfDoubles = followPath(holdersNode, [{edge: "doubles"}, {node: "Cell Butterfly"}]);
assert(!excludeStructure(storageOfDoubles.outgoingEdges).length, "The storage of holders.doubles should reference nothing");
let addressAsDouble = onlyNodeWithClassName("AddressAsDouble");
assert(addressAsDouble.incomingEdges.length === 1 && addressAsDouble.incomingEdges[0].from === holdersNode, "'AddressAsDouble' should be referenced only by `holders`");

for (let className of ["DeletedKey", "DeletedValue", "OverwrittenValue", "DeletedMember", "ClearedMember"])
    assert(!snapshot.nodesWithClassName(className).length, "Snapshot should not contain a '" + className + "' instance");

// A removed entry leaves a marker in its slots. The VM keeps that cell alive, not the table: no edge to it.
for (let [holder, edgeCount] of [["mapWithDeletedEntry", 2], ["setWithDeletedMember", 1]]) {
    let table = followPath(holdersNode, [{edge: holder}, {node: "Cell Butterfly"}]);
    let edges = excludeStructure(table.outgoingEdges);
    assert(edges.length === edgeCount, "The table of holders." + holder + " should have " + edgeCount + " edges for its entry, has " + edges.length);
    for (let edge of edges)
        assert(edge.type === "Index" && edge.to.className === "string", "The table of holders." + holder + " should reference only the strings of its entry, not a '" + edge.to.className + "'");
}
