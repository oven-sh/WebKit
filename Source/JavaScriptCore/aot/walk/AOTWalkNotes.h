/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"

namespace JSC { namespace AOT { namespace Typed {

enum class WalkNoteKind : uint8_t {
    ResolvedOperation,
    GenericOperation,
    TestThatRejoins,
    TestThatLeaves,
    TestThatThrows,
    CallNotInlined,
    ObjectBuilt,
    ObjectNotInFrame,
    ObjectStayedVirtual,
    LoopWalkedAgain,
    WayEnds,
};

// The operation of the finished code that made a value whose type is not proven. It is looked for behind the operations that only
// hand a value on, and behind the reads that are no resolved ones: what such a read gives is no more proven than what it reads from.
// NotSaid: the type of the value is proven, or its maker is of no kind below, or a join brings values of several kinds together.
enum class SourceOfNotedValue : uint8_t {
    NotSaid,
    EntryOfFunction,
    CaughtValue,
    EvaluatedText,
    BuiltinResult,
    ResultOfCallByValue,
    ResultOfDirectCall,
    ElementOfCollection,
    PropertyOrVariable,
    ObjectBuiltWithoutLayout,
    LostByWalk,
};
JS_EXPORT_PRIVATE ASCIILiteral nameOf(SourceOfNotedValue);

// What an operation asks where it runs, of values whose types are proven.
// StateOfObject: what no type says, that an object is as it was built. A test asks it, or the operation runs where such a test fails.
// PlaceOfElement: an access of an element tests the index, or how the list stores its elements. ElementThatMayBeAbsent: and the
// element may be absent. IndexThatMayBeUndefined: an access of an element runs where a test of the type of its index fails, and
// the index is undefined there. PropertyOfObjectWithoutLayout: an access by name asks an object that has no layout for a property:
// a list, a function, an object of a built-in class.
enum class AskedOfNotedValue : uint8_t {
    NotSaid,
    StateOfObject,
    PlaceOfElement,
    ElementThatMayBeAbsent,
    IndexThatMayBeUndefined,
    PropertyOfObjectWithoutLayout,
};
JS_EXPORT_PRIVATE ASCIILiteral nameOf(AskedOfNotedValue);

// A note says what the finished code shows of an operation, and nothing that the code does not show. The reports name a cause
// from it: causeOfWalkNote(), driver/AOTExplainRecords.cpp.
struct WalkNote {
    WalkNoteKind kind { WalkNoteKind::GenericOperation };
    AccessKind access { AccessKind::Unknown };
    Opcode opcode { Opcode::Undefined };
    // Of the value that the operation hangs on: the object of an access, the index of an access of an element of a proven list, the
    // first operand of an operator whose type is not proven, what a test looks at. Only a build that asks for causes looks for them.
    SourceOfNotedValue source { SourceOfNotedValue::NotSaid };
    AskedOfNotedValue asked { AskedOfNotedValue::NotSaid };
    AccessCost cost { AccessCost::ByName };
    uint16_t loopDepth { 0 };
    Origin origin;
    FunctionID writtenIn;
    LayoutID layout;
    // Of an object that is built: the operation that needs it as a cell. Of a test: the operation that made what it looks at.
    Origin neededAt;
    FunctionID neededIn;
};

static_assert(sizeof(WalkNote) <= 56 && std::is_trivially_copyable_v<WalkNote>);

struct WalkNotes {
    CodeUnit unit;
    Vector<WalkNote> notes;
    uint32_t numberOfValuesInDouble { 0 };
};

inline Wire::SourceLocation locationOf(const Program& program, const WalkNotes&, const WalkNote& note) { return program.at(note.writtenIn).locationAt(note.origin.position); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
