// A function writes to a variable of the module through a shorthand property of an object assignment pattern,
// and mentions the variable nowhere else. The module has to keep such a variable in its environment. See
// stress/destructuring-assignment-shorthand-target-is-a-used-variable.js.
import { shouldBe } from "./resources/assert.js";

let forOfTarget = "init";
(() => { for ({ forOfTarget } of [{ forOfTarget: "new" }]) ; })();
shouldBe(forOfTarget, "new");

let forInTarget = "init";
(() => { for ({ forInTarget } in { key: 0 }) ; })();
shouldBe(forInTarget, undefined);

let nestedTarget = "init";
(() => { for ([{ nestedTarget }] of [[{ nestedTarget: "new" }]]) ; })();
shouldBe(nestedTarget, "new");

let width, height;
function setOptions(options) { ({ width = 100, height } = options); }
setOptions({ height: 5 });
shouldBe(width, 100);
shouldBe(height, 5);

// What a bundler writes for a module that it wraps in a function: the declarations of the module outside, the
// statements inside.
var ids, id, label;
var init = () => {
    ids = [];
    for ({ id, label } of [{ id: 1, label: "a" }, { id: 2, label: "b" }])
        ids.push(id);
};
init();
shouldBe(JSON.stringify(ids), "[1,2]");
shouldBe(label, "b");

var first, second;
var initDeclaration = () => { ({ first = 1, second } = { second: 2 }); };
initDeclaration();
shouldBe(first, 1);
shouldBe(second, 2);

// The initializer of a field is code of the class scope, not a function of its own.
let fieldTarget = "init", fieldDefault;
class WithField {
    static field = ({ fieldDefault = 1, fieldTarget } = { fieldTarget: "new" });
}
shouldBe(fieldTarget, "new");
shouldBe(fieldDefault, 1);

// A variable of the module that a function reads is not the target of a function's own variable of that name.
let shadowed = "module";
const readShadowed = () => shadowed;
function assignShadowed() {
    let shadowed = "function";
    (() => { for ({ shadowed } of [{ shadowed: "new" }]) ; })();
    return shadowed;
}
shouldBe(assignShadowed(), "new");
shouldBe(readShadowed(), "module");

const constant = "init";
let error;
try {
    (() => { for ({ constant } of [{ constant: "new" }]) ; })();
} catch (e) {
    error = e;
}
shouldBe(String(error), "TypeError: Attempted to assign to readonly property.");
shouldBe(constant, "init");
