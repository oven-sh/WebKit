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

enum class ObjectBuiltCause : uint8_t { None, Retained, Returned, Thrown, DiffersAtJoin, MayThrowUnderHandler, RetainedInLoop, Suspension, OperationNeedsCell };

struct WalkNote {
    WalkNoteKind kind { WalkNoteKind::GenericOperation };
    AccessKind access { AccessKind::Unknown };
    Opcode opcode { Opcode::Undefined };
    UnprovenCause cause { UnprovenCause::Unknown };
    ObjectBuiltCause objectBuilt { ObjectBuiltCause::None };
    AccessCost cost { AccessCost::ByName };
    uint16_t loopDepth { 0 };
    bool isOnUsualPath { true };
    Origin origin;
    FunctionID writtenIn;
    LayoutID layout;
    Origin neededAt;
    FunctionID neededIn;
    Wire::TypeOriginReason reason { Wire::TypeOriginReason::Unknown };
    Origin sourceAt;
    FunctionID sourceIn;
    BuiltinID sourceBuiltin;
    FunctionID sourceFunction;
    bool sourceIsCall { false };
    bool sourceIsOnUsualPath { true };
};

struct WalkNotes {
    CodeUnit unit;
    Vector<WalkNote> notes;
    uint32_t numberOfValuesInDouble { 0 };
};

inline Wire::SourceLocation locationOf(const Program& program, const WalkNotes&, const WalkNote& note) { return program.at(note.writtenIn).locationAt(note.origin.position); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
