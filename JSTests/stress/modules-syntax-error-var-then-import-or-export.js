//@ requireOptions("--useImportDefer=1")

// An import binding, and a let, const or class that an export declaration declares, is a
// lexically declared name of the module. A var of the same name makes the module a SyntaxError,
// in both orders:
// "It is a Syntax Error if any element of the LexicallyDeclaredNames of ModuleItemList also
// occurs in the VarDeclaredNames of ModuleItemList."
// https://tc39.es/ecma262/#sec-module-semantics-static-semantics-early-errors

function shouldThrowSyntaxError(source, message) {
    let thrown = false;
    let error;
    try {
        checkModuleSyntax(source);
    } catch (e) {
        thrown = true;
        error = e;
    }
    if (!thrown)
        throw new Error(`"${source}" should throw "${message}" but did not throw`);
    // checkModuleSyntax() throws a string, so compare the string form.
    if (String(error) !== message)
        throw new Error(`"${source}" should throw "${message}" but threw "${String(error)}"`);
}

function shouldParse(source) {
    try {
        checkModuleSyntax(source);
    } catch (e) {
        throw new Error(`"${source}" should parse but threw "${String(e)}"`);
    }
}

// Each of these adds x to the VarDeclaredNames of the module.
const varDeclarations = [
    `var x;`,
    `var x = 1;`,
    `var w, x;`,
    `var [x] = [];`,
    `var { x } = {};`,
    `var { a: [{ x }] } = { a: [{}] };`,
    `{ var x; }`,
    `{ { var x; } }`,
    `if (0) var x;`,
    `if (0) ; else var x;`,
    `for (var x;;) break;`,
    `for (var x of []) ;`,
    `for (var x in {}) ;`,
    `for (;;) { var x; break; }`,
    `while (0) var x;`,
    `do var x; while (0);`,
    `label: var x;`,
    `switch (0) { case 0: var x; }`,
    `try { var x; } catch { }`,
    `try { } catch { var x; }`,
    `try { } finally { var x; }`,
    `export var x;`,
    `export var [x] = [];`,
];

// Each of these adds x to the LexicallyDeclaredNames of the module. The message is the one that
// the same declaration gives after a let of the same name.
const importedBindingMessage = `Cannot declare an imported binding name twice: 'x'.`;
const letMessage = `Cannot declare a let variable twice: 'x'.`;
const constMessage = `Cannot declare a const variable twice: 'x'.`;
const classMessage = `Cannot declare a class twice: 'x'.`;
const lexicalDeclarations = [
    [`import x from "m";`, importedBindingMessage],
    [`import * as x from "m";`, importedBindingMessage],
    [`import defer * as x from "m";`, importedBindingMessage],
    [`import { x } from "m";`, importedBindingMessage],
    [`import { y as x } from "m";`, importedBindingMessage],
    [`import { "a name" as x } from "m";`, importedBindingMessage],
    [`import { default as x } from "m";`, importedBindingMessage],
    [`import { a, x } from "m";`, importedBindingMessage],
    [`import d, { x } from "m";`, importedBindingMessage],
    [`import x, { a } from "m";`, importedBindingMessage],
    [`import x, * as ns from "m";`, importedBindingMessage],
    [`import d, * as x from "m";`, importedBindingMessage],
    [`import x from "m" with { type: "json" };`, importedBindingMessage],
    [`export let x;`, letMessage],
    [`export let x = 1;`, letMessage],
    [`export let a, x;`, letMessage],
    [`export const x = 1;`, constMessage],
    [`export const a = 1, x = 2;`, constMessage],
    [`export let { x } = {};`, `Unexpected token '}'. Cannot declare a lexical variable twice: 'x'.`],
    [`export let { a: x } = {};`, `Unexpected identifier 'x'. Cannot declare a lexical variable twice: 'x'.`],
    [`export const { a: [{ x }] } = { a: [{}] };`, `Unexpected token '}'. Cannot declare a lexical variable twice: 'x'.`],
    [`export let [x] = [];`, `Unexpected identifier 'x'. Cannot declare a lexical variable twice: 'x'.`],
    [`export const [a, ...x] = [];`, `Unexpected identifier 'x'. Cannot declare a lexical variable twice: 'x'.`],
    [`export class x { }`, classMessage],
    [`export default class x { }`, classMessage],
];

const varAfterLexicalMessage = `Cannot declare a var variable that shadows a let/const/class variable: 'x'.`;

for (const varDeclaration of varDeclarations) {
    for (const [lexicalDeclaration, message] of lexicalDeclarations) {
        shouldThrowSyntaxError(`${varDeclaration}\n${lexicalDeclaration}`, `SyntaxError: ${message}:2`);
        shouldThrowSyntaxError(`${lexicalDeclaration}\n${varDeclaration}`, `SyntaxError: ${varAfterLexicalMessage}:2`);
    }
}

// A function that an export declaration declares is a lexically declared name too.
const functionMessage = `Cannot declare a function that shadows a let/const/class/function variable 'x'.`;
const asyncFunctionMessage = `Cannot declare an async function that shadows a let/const/class/function variable 'x'.`;
const functionDeclarations = [
    [`export function x() { }`, functionMessage],
    [`export function* x() { }`, functionMessage],
    [`export async function x() { }`, asyncFunctionMessage],
    [`export async function* x() { }`, asyncFunctionMessage],
    [`export default function x() { }`, functionMessage],
    [`export default function* x() { }`, functionMessage],
    [`export default async function x() { }`, asyncFunctionMessage],
    [`export default async function* x() { }`, asyncFunctionMessage],
];

for (const varDeclaration of varDeclarations) {
    for (const [functionDeclaration, message] of functionDeclarations) {
        shouldThrowSyntaxError(`${varDeclaration}\n${functionDeclaration}`, `SyntaxError: ${message}:2`);
        shouldThrowSyntaxError(`${functionDeclaration}\n${varDeclaration}`, `SyntaxError: ${varAfterLexicalMessage}:2`);
    }
}

// The body of a function, and a static block, that is inside an export declaration has var
// declared names and lexically declared names of its own.
shouldThrowSyntaxError(`export default function () { var x; let x; }`, `SyntaxError: ${letMessage}:1`);
shouldThrowSyntaxError(`export function f() { var x; class x { } }`, `SyntaxError: ${classMessage}:1`);
shouldThrowSyntaxError(`export function f(x) { let x; }`, `SyntaxError: ${letMessage}:1`);
shouldThrowSyntaxError(`export let f = () => { var x; const x = 1; };`, `SyntaxError: ${constMessage}:1`);
shouldThrowSyntaxError(`export const f = function (x) { let x; };`, `SyntaxError: ${letMessage}:1`);
shouldThrowSyntaxError(`export class C { static { var x; let x; } }`, `SyntaxError: ${letMessage}:1`);
shouldThrowSyntaxError(`export default class { m() { var x; let x; } }`, `SyntaxError: ${letMessage}:1`);

// None of these declares x a second time in the scope of the module.
const notDeclaringX = [
    `import "m";`,
    `import { y } from "m";`,
    `import { x as y } from "m";`,
    `import * as y from "m";`,
    `export { x };`,
    `export { x as y };`,
    `export { x as default };`,
    `export * from "m";`,
    `export * as x from "m";`,
    `export { x } from "m";`,
    `export { y as x } from "m";`,
    `export var x;`,
    `export var [x] = [];`,
    `export let y = x;`,
    `export let { x: y } = {};`,
    `export const [y = x] = [];`,
    `export default x;`,
    `export default (class x { });`,
    `export default (function x() { });`,
    `export default function () { let x; }`,
    `export default function (x) { }`,
    `export default function f() { var x; }`,
    `export default class { static x = 1; static { let x; } }`,
    `export default class { [class x { }.name]() { } }`,
    `export function f(x) { let y; }`,
    `export function f() { let x; class y { } }`,
    `export function* f() { const x = 1; }`,
    `export async function f() { let x; }`,
    `export class C { static { let x; } m() { const x = 1; } }`,
    `export class C extends (class x { }) { }`,
    `export const y = class x { };`,
    `export const y = function x() { };`,
    `export let y = () => { let x; };`,
    `export let y = (x) => x;`,
    `export let y = function () { class x { } };`,
];

for (const declaration of notDeclaringX) {
    for (const varDeclaration of [`var x;`, `{ var x; }`, `for (var x of []) ;`]) {
        shouldParse(`${varDeclaration}\n${declaration}`);
        shouldParse(`${declaration}\n${varDeclaration}`);
    }
}

// A var of an inner function, and a lexical name of an inner block, is not a name of the module.
for (const [declaration] of [...lexicalDeclarations, ...functionDeclarations]) {
    for (const statement of [`function f() { var x; }`, `{ let x; }`, `{ class x { } }`, `for (let x of []) ;`, `try { } catch (x) { }`]) {
        shouldParse(`${statement}\n${declaration}`);
        shouldParse(`${declaration}\n${statement}`);
    }
}
