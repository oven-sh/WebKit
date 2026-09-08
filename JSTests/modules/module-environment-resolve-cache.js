import { readImportAndLocal1, readImportAndLocal2, readImportAndLocalNested, readSharedName1, readSharedName2, makeSharedNameReader, typeofLater1, typeofLater2, readMath1, readMath2, readCyclic1, tdzErrorsDuringCycle, readStar1, readStar2, readNamespace1, readNamespace2, readPreLexical1, readPreLexical2, readCounter, makeIncrement, makeFrozenWriter } from "./module-environment-resolve-cache/lib.js"
import { shouldBe, shouldThrow } from "./resources/assert.js";

// Several functions of one module resolving the same import, module local and globals when they are first called (linked).
shouldBe(readImportAndLocal1(), 42);
shouldBe(readImportAndLocal2(), 44);
shouldBe(readImportAndLocal1(), 44);
shouldBe(readImportAndLocalNested(), 44);
shouldBe(readMath1(), 2);
shouldBe(readMath2(), 1);

// A global property, then a global lexical binding that shadows it: functions linked after the binding appears see it.
globalThis.sharedName = "property";
shouldBe(readSharedName1(), "property");
$.evalScript(`let sharedName = "lexical";`);
shouldBe(readSharedName2(), "lexical");
shouldBe(makeSharedNameReader()(), "lexical");
shouldBe(readSharedName1(), "lexical");

// A name that does not resolve anywhere, then does.
shouldBe(typeofLater1(), "undefined");
globalThis.definedLater = 1;
shouldBe(typeofLater2(), "number");
shouldBe(typeofLater1(), "number");
delete globalThis.definedLater;
shouldBe(typeofLater2(), "undefined");

// Import cycle: dep.js read lib.js's binding in its TDZ while linking before lib.js ran; the same functions see it now.
shouldBe(tdzErrorsDuringCycle, 3);
shouldBe(readCyclic1(), 7);

// Import through `export * from`, and a namespace import (a module local).
shouldBe(readStar1(), 100);
shouldBe(readStar2(), 101);
shouldBe(readNamespace1(), 41);
shouldBe(readNamespace2(), "object");

// A global lexical binding that exists before the first function resolving it is linked.
$.evalScript(`let preLexical = 10;`);
shouldBe(readPreLexical1(), 10);
shouldBe(readPreLexical2(), 11);

// Writes through the module environment from nested functions after a read was resolved.
shouldBe(readCounter(), 0);
shouldBe(makeIncrement()(), 1);
shouldBe(makeIncrement()(), 2);
shouldBe(readCounter(), 2);
shouldThrow(() => { makeFrozenWriter()(); }, `TypeError: Attempted to assign to readonly property.`);
