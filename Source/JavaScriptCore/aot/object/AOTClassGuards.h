/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include <optional>

namespace JSC { namespace AOT {

struct State;

// A class has a guard for each of its sides, and a guard for each name of a side that compiled code asks about.
// Compiled code that resolved a member of a class without a proof tests the guard of the receiver's class, side and name. What it assumes of the class beside any name, it asks the guard of the side.
//
// The guard of (class, side) holds while all of this is true in the instance:
// - The class was evaluated at most once, and the holder of the side (the prototype object, or the class object) is the one that evaluation made.
// - The same goes for the holder of the same side of every class above it that the program defines, and the prototype link of each of those holders is the one that the record names (LayoutData::base or assumedBase).
// - Each of those holders tells the run time of every change, and nothing has changed all its members at one time.
//   `Below.prototype = new Above()`: a compiled object tells of every name but those of its own layout, whose slots compiled code fills without a word. The guards of those names, and of whatever else it has where the guards begin, never hold.
// - Each member that a record declares without a name is as declared.
// - ClassSide::Static of a constructor function: no record names what stands above the function, and compiled code takes that to be Function.prototype.
//   `Object.setPrototypeOf(Readable, Stream)`: the function may also stand below the function of the class that its record names (assumedBase), that one below the next, and the last of them below Function.prototype.
//   The guard of a name then says as well that none of those functions has the name. So code that looks past a constructor function asks the guard of the name, whatever it knows of the function's own names: walkAreMembersUnchanged() refuses a test that asks for the link alone.
// No guard speaks of the prototype of an instance: an object that stands below another one than its class made says so itself, in a bit of its cell, and compiled code asks the object (HasLayoutPrototype).
// One object that is moved says nothing of the others of its class.
//
// The guard of (class, side, name) holds while the guard of (class, side) holds and, under that name:
// - On each of those holders the member that the record declares is as declared, and where the record declares none, nothing was added, deleted or redefined.
//   ClassSide::Static: no guard says anything about a name under which the record declares a static field and no method or accessor.
//   The record is that of the holder's own class: with `class A { static count = 0 }` and `class B extends A { }`, a `count` added to the class object of B still ends the guard of that name at B.
//   Such a field is a plain property of the class object, or lies in its variable until the class object is made (AOTStaticFields.h). Compiled code resolves no member under its name, at the class or from a class below it.
// - ClassSide::Prototype only: no instance of the class got an own property under the name while one of those holders had it.
//
// `this.sent = false` below `Reply.prototype.sent = true`: a store into an instance asks less, and has a guard of its own (NamedClassGuardData::isAskedByStoresAlone).
// It holds while the guard of (class, Prototype) holds and nothing under the name takes the store: where its guards begin, each of those holders has nothing there or a plain value that may be written.
// An instance that gets the name ends no such guard. Whatever is laid under the name once the guards have begun ends it like the other: the run time hears of a change before it is made.
//
// Code that asks about several names of one side in one test asks one guard for all of them: `x.a(); x.b(); x.c`. The guard of (class, side, names) holds while the guard of each of the names holds.
// It has a bit like any other, which the record of the class lists under each of the names, so whatever ends the guard of one of them ends it.
//
// A guard that has ended never holds again. What ends a guard of a class ends the same guard of every class below it, and what ends the guard of a side ends the guards of all its names.
// So a guard of a name that holds says that the guard of its side holds. `A.prototype.extra = 1` ends the guards of `extra` and no other.
//
// ClassData::guardBit[side] is the number of the bit of the guard of the side, counted from the start of the instance's statics, or noClassGuardBit where no code tests it.
// ClassData::namedGuardsOffset[side] leads to the bits of the guards of names, by the number of the name. Only a name that code asks about is listed.
// Code mentions (class, side) or (class, side, name) through a relocation (NumberSpace::ClassGuard), Image numbers what was mentioned in the order of the keys, and writes the numbers into the class records.
// Image gives each guard a byte of its own, so a number is a multiple of eight and compiled code tests the lowest bit of the byte. The run time goes by the two functions below and assumes neither.
//
// ClassData::guardsHoldWhileClear is Plan's: true where the record names every member of the class, and the same goes for each class above it, as far up as the records name a parent (assumedBase).
// There every bit is clear from the start, the guards hold before the class exists, and ending one sets its bit. The first definition makes what the record says, or it is handed another parent than the record names and ends the guards at once.
// Elsewhere the bits are set by the first evaluation and ending a guard clears its bit, so a test that runs too early fails, which is safe.
//
// A test that is left with two guards or more, of whatever classes and sides, reads one word of 32 bits for all of them: `shape.area()`, where a shape is of one of ten classes.
// The word lies in the statics too. It counts those of its guards whose bits say that they do not hold, in whichever sense each bit is read, so the test passes while the word is zero. Tests that list the same guards share it.
// Each of the guards has its bit like any other. ProgramTables::classGuardCounts() lists the words under the number of each bit, and whatever changes a bit adds one to each of its words or takes one from it.
// A store that leaves a bit as it was counts nothing: what ends a side stores the bits of names that have ended before. Image writes what a word starts as: how many of its guards begin with the first evaluation of their class.
//
// An operation that may end a guard writes AbstractHeap::MemberGuards. A guard of the second kind begins to hold without any operation saying so (the first allocation of a constructor function's class defines the class):
// a test that failed is never a proof that it still fails.

inline constexpr uint32_t noClassGuardBit = UINT32_MAX;

constexpr uint32_t byteOffsetOfClassGuardBit(uint32_t bit) { return bit / 8; }
constexpr uint8_t maskOfClassGuardBit(uint32_t bit) { return static_cast<uint8_t>(1u << bit % 8); }

// For the run time's own questions, for --aot-validate and for tests: the guard of the side holds, and so would a guard of any name of it. The run time keeps that state of every side, tested or not.
// For a side that no code tests, of a class whose guards are set by the first evaluation, true also means "has not begun": there is no bit to tell the two apart.
bool classGuardHolds(const State&, const LayoutData& classLayout, ClassSide);
// The guard of one name, by the number of the name. Of a name that no code asks about it answers as classGuardHolds() does.
bool classGuardOfNameHolds(const State&, const LayoutData& classLayout, ClassSide, uint32_t key);

// Ends the guard of the side and of all its names, of the class and of every class below it. Does nothing to a guard that has ended.
void endClassGuard(State&, const LayoutData& classLayout, ClassSide);
// Ends the guards of the name, of the class and of every class below it. No number: the image does not know the name, so no code asks about it, and only classGuardHolds() answers otherwise from here on.
// StoresStillPass: but for the guards that stores alone ask. ReadsStillFind: those alone.
enum class GuardsOfNameToEnd : uint8_t { All, StoresStillPass, ReadsStillFind };
void endClassGuardsOfName(State&, const LayoutData& classLayout, ClassSide, std::optional<uint32_t> key, GuardsOfNameToEnd = GuardsOfNameToEnd::All);

// The program evaluated the class a second time in this instance: all its guards end.
void didEvaluateClassAgain(State&, const LayoutData& classLayout);

// A call through the method table tests no guard. An entry that stands on one (ProgramTables::methodTableDependents()) holds what the image has there until the guard ends, and the lookup of its selector from then on:
// whatever stores the bit of a guard repoints its entries, also where the bit stays as it was. A guard that begins only with the evaluation of its class finds the compiled method there:
// no object of a class exists before the run time has looked at what the definition has made, and that look ends whatever differs.

// Whether the record of the class itself, not of a class above it, declares a method or an accessor under the name number on the side.
bool declaresMember(const LayoutData& classLayout, ClassSide, uint32_t key);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
