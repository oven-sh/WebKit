import { imported, shadowed } from "./parameter-expression-uses-variable-the-function-body-declares/names.js"
import { shouldBe } from "./resources/assert.js";

// A name in a parameter expression that the function body declares again is the variable from around the function. With
// one more function in between, the function that declares the variable did not know that it was captured, and the
// parameter expression read what was further out: an import of that name, or the global object.

globalThis.moduleVariable = "global";
let moduleVariable = 10;

// The variable is in an enclosing function, and an import has the same name.
function enclosing() {
    let shadowed = 10;
    return () => (b = shadowed) => { var shadowed = 7; return b; };
}
shouldBe(enclosing()()(), 10);

function enclosingFunctionExpression() {
    let shadowed = 10;
    return () => function (g = shadowed) { var shadowed = 7; return g; };
}
shouldBe(enclosingFunctionExpression()()(), 10);

// The variable is the module's own.
shouldBe((() => (b = moduleVariable) => { var moduleVariable = 7; return b; })()(), 10);
shouldBe((() => (b = () => moduleVariable) => { let moduleVariable = 7; return b(); })()(), 10);

// The variable is the import.
shouldBe((() => (b = imported) => { var imported = 7; return b; })()(), "imported");
shouldBe((() => (b = typeof imported) => { const imported = 7; return b; })()(), "string");
