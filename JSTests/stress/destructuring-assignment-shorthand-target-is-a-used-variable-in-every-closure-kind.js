//@ runDefault
//@ runNoCJITValidatePhases
//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1")
// Every spelling of a shorthand target of an object assignment pattern, in every kind of function, with every kind
// of declaration of the target. See destructuring-assignment-shorthand-target-is-a-used-variable.js for what went
// wrong. Each text here runs once, so the configurations that compile early or collect all the time add nothing.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${String(expected)} but got ${String(actual)}`);
}

// Each text assigns to the variable `v` and mentions it nowhere else. `u` and `w` are other targets.
const assignments = [
    ["for-of head", v => `for ({ ${v} } of [{ ${v}: "new" }]) ;`, "new"],
    ["for-of head with two targets", v => `for ({ u, ${v} } of [{ ${v}: "new" }]) ;`, "new"],
    ["for-of head after a default value", v => `for ({ w = 1, ${v} } of [{ ${v}: "new" }]) ;`, "new"],
    ["for-of head in an array pattern", v => `for ([{ ${v} }] of [[{ ${v}: "new" }]]) ;`, "new"],
    ["for-of head under a key", v => `for ({ k: { ${v} } } of [{ k: { ${v}: "new" } }]) ;`, "new"],
    ["for-of head in a rest element", v => `for ([...[{ ${v} }]] of [[{ ${v}: "new" }]]) ;`, "new"],
    ["for-in head", v => `for ({ ${v} } in { key: 0 }) ;`, undefined],
    ["assignment after a default value", v => `({ w = 1, ${v} } = { ${v}: "new" });`, "new"],
    ["assignment after a default value and another target", v => `({ w = 1, u, ${v} } = { ${v}: "new" });`, "new"],
    ["assignment after a default value in an array pattern", v => `[{ w = 1, ${v} }] = [{ ${v}: "new" }];`, "new"],
    ["assignment after a default value under a key", v => `({ k: { w = 1, ${v} } } = { k: { ${v}: "new" } });`, "new"],
    ["assignment after a default value in an object before it", v => `({ k: { w = 1 }, ${v} } = { k: { }, ${v}: "new" });`, "new"],
    ["assignment after a default value in an array element before it", v => `[{ w = 1 }, { ${v} }] = [{ }, { ${v}: "new" }];`, "new"],
    ["assignment after a second __proto__ key", v => `({ __proto__: u, __proto__: w, ${v} } = { ${v}: "new" });`, "new"],
    ["assignment after a default value in the init of a for statement", v => `for ({ w = 1, ${v} } = { ${v}: "new" }; ;) break;`, "new"],
];

// Each text runs the body in a function of its own.
const closures = [
    ["an arrow function", body => `(() => { ${body} })();`],
    ["a function expression", body => `(function () { ${body} })();`],
    ["a method", body => `({ method() { ${body} } }).method();`],
    ["a getter", body => `({ get getter() { ${body} return 0; } }).getter;`],
    ["a generator", body => `(function* () { ${body} })().next();`],
    ["an async arrow function", body => `(async () => { ${body} })();`],
    ["a class static block", body => `class C { static { ${body} } }`],
    ["an arrow function in an arrow function", body => `(() => () => { ${body} })()();`],
];

// Each text declares the variable `v` with the value "init", runs `use` and returns the variable.
const declarations = [
    ["let", (v, use) => `let ${v} = "init"; ${use} return ${v};`],
    ["var", (v, use) => `var ${v} = "init"; ${use} return ${v};`],
    ["a let in a block", (v, use) => `{ let ${v} = "init"; ${use} return ${v}; }`],
    ["a parameter", (v, use) => `return (function (${v}) { ${use} return ${v}; })("init");`],
    ["a parameter with a default value", (v, use) => `return (function (${v} = "init") { ${use} return ${v}; })();`],
    ["a catch parameter", (v, use) => `try { throw "init"; } catch (${v}) { ${use} return ${v}; }`],
];

// Every check has a variable name of its own, so that a global variable that one check leaves behind is not
// found by the next one.
let count = 0;
function check(strict, [declarationName, declaration], [closureName, closure], [assignmentName, assignment, expected]) {
    const name = `v${count++}`;
    const what = `${assignmentName} in ${closureName}, target declared by ${declarationName}${strict ? ", strict" : ""} (${name})`;
    const source = `${strict ? `"use strict"; ` : ""}let u, w; ${declaration(name, closure(assignment(name)))}`;
    let actual;
    try {
        actual = new Function(source)();
    } catch (error) {
        actual = error;
    }
    shouldBe(actual, expected, what);
    shouldBe(name in globalThis, false, what);
}

for (const strict of [false, true]) {
    for (const assignment of assignments) {
        for (const closure of closures)
            check(strict, declarations[0], closure, assignment);
    }
    for (const declaration of declarations.slice(1)) {
        check(strict, declaration, closures[0], assignments[0]);
        check(strict, declaration, closures[0], assignments[7]);
    }
}

// The initializer of a field is code of the class scope, not a function of its own. Only an expression can be one.
const fieldInitializers = [
    ["an instance field initializer", body => `class C { field = ${body} } new C;`],
    ["a static field initializer", body => `class C { static field = ${body} }`],
];

for (const strict of [false, true]) {
    for (const assignment of assignments.slice(7, 14)) {
        for (const fieldInitializer of fieldInitializers)
            check(strict, declarations[0], fieldInitializer, assignment);
    }
}

// There the name of the class is a constant.
let error;
try {
    new Function(`let w; class Named { static field = ({ w = 1, Named } = { }); }`)();
} catch (e) {
    error = e;
}
shouldBe(error instanceof TypeError, true, `the name of the class as the target in a field initializer: ${String(error)}`);

// `u` and `w` are the other targets of the patterns above.
shouldBe("u" in globalThis, false, "the other shorthand target");
shouldBe("w" in globalThis, false, "the target with a default value");
