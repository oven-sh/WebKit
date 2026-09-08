import { importedValue, bump, readCyclic1, tdzErrorsDuringCycle } from "./dep.js"
import { starValue } from "./star.js"
import * as depNamespace from "./dep.js"

let moduleLocal = 2;

export function readImportAndLocal1() { return importedValue + moduleLocal; }
export function readImportAndLocal2() { bump(); moduleLocal += 1; return importedValue + moduleLocal; }
export function readImportAndLocalNested() { return (() => { { let unrelated = 1; return importedValue + moduleLocal + unrelated - 1; } })(); }

export function readSharedName1() { return sharedName; }
export function readSharedName2() { return sharedName; }
export function makeSharedNameReader() { return function () { return sharedName; }; }

export function typeofLater1() { return typeof definedLater; }
export function typeofLater2() { return typeof definedLater; }

export function readMath1() { return Math.max(1, 2); }
export function readMath2() { return Math.min(1, 2); }

export let cyclicFromLib = 7;
export { readCyclic1, tdzErrorsDuringCycle };

export function readStar1() { return starValue; }
export function readStar2() { return starValue + 1; }

export function readNamespace1() { return depNamespace.importedValue; }
export function readNamespace2() { return typeof depNamespace; }

export function readPreLexical1() { return preLexical; }
export function readPreLexical2() { return preLexical + 1; }

let counter = 0;
export function readCounter() { return counter; }
export function makeIncrement() { return function () { counter += 1; return counter; }; }
export const frozen = 1;
export function makeFrozenWriter() { return function () { frozen = 2; }; }
