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

// What --explain is made of. The walk takes notes only where WalkOptions::takesNotes is set.

enum class WalkNoteKind : uint8_t {
    ResolvedOperation,
    // The closed form.
    GenericOperation,
    TestThatRejoins,
    TestThatThrows,
    CallNotInlined,
    ObjectBuilt,
    ObjectNotInFrame,
    ObjectStayedVirtual,
    LoopWalkedAgain,
    WayEnds,
};

enum class ObjectBuiltCause : uint8_t { None, Retained, Returned, Thrown, DiffersAtJoin, MayThrowUnderHandler, RetainedInLoop, Suspension, OperationNeedsCell };

struct WalkNote {
    WalkNoteKind kind { WalkNoteKind::GenericOperation };
    AccessKind access { AccessKind::Unknown };
    Opcode opcode { Opcode::Undefined };
    UnprovenCause cause { UnprovenCause::Unknown };
    ObjectBuiltCause objectBuilt { ObjectBuiltCause::None };
    AccessCost cost { AccessCost::ByName };
    IllTypedCause illTypedCause { };
    uint16_t loopDepth { 0 };
    Origin origin;
    // The function in whose text the operation stands.
    FunctionID writtenIn;
    bool hasSubject { false };
    FactSubject subject;
    LayoutID layout;
    // Of ObjectBuilt and ObjectNotInFrame: where the operation stands that needs the cell. No function: none does.
    Origin neededAt;
    FunctionID neededIn;
};

// The notes of one unit, in the order in which the walk took them.
struct WalkNotes {
    CodeUnit unit;
    Vector<WalkNote> notes;
    // StatisticID::ValuesInDouble, of the unit: the records that are kept, parameters of blocks too, in a form that holds a double.
    uint32_t numberOfValuesInDouble { 0 };
};

inline Wire::SourceLocation locationOf(const Program& program, const WalkNotes&, const WalkNote& note) { return program.at(note.writtenIn).locationAt(note.origin.position); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
