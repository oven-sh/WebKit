load("./driver/driver.js");

// A scope slot that no name of the symbol table maps to: a parameter that a function declaration of the same name
// shadows, and the first of two parameters of one name. The arguments object of the call still reads the value there.

class Holders { }
class ShadowedParameter { }
class NamedParameter { }
class DuplicateParameter { }

let holders = new Holders;

function shadowed(a, b) {
    function a() { }
    holders.closureOfShadowed = function () { return b; };
    return arguments;
}
holders.argumentsOfShadowed = shadowed(new ShadowedParameter, new NamedParameter);

function duplicate(a, a) {
    holders.closureOfDuplicate = function () { return a; };
    return arguments;
}
holders.argumentsOfDuplicate = duplicate(new DuplicateParameter, 1);

let snapshot = createHeapSnapshot();

assert(holders.argumentsOfShadowed[0] instanceof ShadowedParameter, "The first argument of shadowed() should still be the 'ShadowedParameter'");
assert(holders.argumentsOfDuplicate[0] instanceof DuplicateParameter, "The first argument of duplicate() should still be the 'DuplicateParameter'");

function onlyEdgeTo(className) {
    let nodes = snapshot.nodesWithClassName(className);
    assert(nodes.length === 1, "Snapshot should contain 1 '" + className + "' instance, has " + nodes.length);
    assert(nodes[0].incomingEdges.length === 1, "'" + className + "' should have 1 incoming edge, has " + nodes[0].incomingEdges.length);
    let edge = nodes[0].incomingEdges[0];
    assert(edge.from.className === "JSLexicalEnvironment", "'" + className + "' should be referenced by a 'JSLexicalEnvironment', not by '" + edge.from.className + "'");
    return edge;
}

assert(onlyEdgeTo("ShadowedParameter").type === "Internal", "'ShadowedParameter' should be referenced by an Internal edge");
assert(onlyEdgeTo("DuplicateParameter").type === "Internal", "'DuplicateParameter' should be referenced by an Internal edge");

let namedParameterEdge = onlyEdgeTo("NamedParameter");
assert(namedParameterEdge.type === "Variable" && namedParameterEdge.data === "b", "'NamedParameter' should be referenced by the Variable edge 'b'");
