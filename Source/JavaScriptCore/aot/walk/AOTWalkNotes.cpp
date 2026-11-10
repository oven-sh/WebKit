/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNotes.h"

#if ENABLE(AOT)

#include "AOTCompileUnit.h"
#include "AOTOpcodeSets.h"
#include "AOTResultRules.h"
#include "AOTType.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet testsInWalkNotes = opcodesWhere([](Opcode opcode) {
    return familyOf(opcode) == OperationFamily::Guard && accessOf(opcode) == Access::None;
});
static constexpr OpcodeSet allocationsInWalkNotes = opcodesWhere([](Opcode opcode) {
    return interpretedOf(opcode) == Interpretation::Allocation;
});

static AccessKind kindOfCallInWalkNote(const Operation& call)
{
    bool constructs = entersByValueOf(call.opcode) == EntryByValue::Construction || (call.opcode == Opcode::CallDirect && call.isConstruction());
    return constructs ? AccessKind::Construct : AccessKind::Call;
}

static AccessKind kindOfAccessWithoutColumnInWalkNote(const Operation& access)
{
    if (access.numberOfNamedOperands() && access.operandUses(0).contains(OperandUse::DeletesByKey))
        return AccessKind::Delete;
    switch (interpretedOf(access.opcode)) {
    case Interpretation::Read:
        return AccessKind::Read;
    case Interpretation::Store:
        return AccessKind::Store;
    case Interpretation::Test:
        return AccessKind::Has;
    case Interpretation::None:
    case Interpretation::Allocation:
    case Interpretation::Cast:
    case Interpretation::List:
    case Interpretation::Guards:
        return AccessKind::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static AccessKind accessKindInWalkNote(const Operation& operation)
{
    Opcode opcode = operation.opcode;
    switch (accessOf(opcode)) {
    case Access::Read:
        return AccessKind::Read;
    case Access::Store:
        return AccessKind::Store;
    case Access::Define:
        return AccessKind::Define;
    case Access::Has:
        return AccessKind::Has;
    case Access::None:
        break;
    }

    switch (accessClassOf(opcode)) {
    case AccessClass::DirectCall:
    case AccessClass::IndirectCall:
        return kindOfCallInWalkNote(operation);
    case AccessClass::Slot:
    case AccessClass::ByName:
        return kindOfAccessWithoutColumnInWalkNote(operation);
    case AccessClass::None:
        break;
    }

    switch (operatorKindOf(opcode)) {
    case OperatorKind::Additive:
    case OperatorKind::Multiplicative:
    case OperatorKind::Bitwise:
        return AccessKind::Arithmetic;
    case OperatorKind::Relation:
        return AccessKind::Comparison;
    case OperatorKind::Equality:
        return AccessKind::Equality;
    case OperatorKind::NumberConversion:
        return AccessKind::Conversion;
    case OperatorKind::None:
        return AccessKind::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static WalkNoteKind kindOfOperationInWalkNote(const Operation& operation)
{
    if (testsItsOwnGuess(operation))
        return WalkNoteKind::TestThatRejoins;

    Opcode opcode = operation.opcode;
    switch (accessClassOf(opcode)) {
    case AccessClass::Slot:
    case AccessClass::DirectCall:
        return WalkNoteKind::ResolvedOperation;
    case AccessClass::ByName:
    case AccessClass::IndirectCall:
        return WalkNoteKind::GenericOperation;
    case AccessClass::None:
        break;
    }
    if (PayloadWord::form(opcode))
        return operation.form() == OperatorForm::Generic ? WalkNoteKind::GenericOperation : WalkNoteKind::ResolvedOperation;
    return formOf(opcode) == Form::Open ? WalkNoteKind::ResolvedOperation : WalkNoteKind::GenericOperation;
}

static AccessCost costOfWalkNoteKind(WalkNoteKind kind)
{
    switch (kind) {
    case WalkNoteKind::ResolvedOperation:
        return AccessCost::Free;
    case WalkNoteKind::TestThatRejoins:
    case WalkNoteKind::TestThatLeaves:
    case WalkNoteKind::TestThatThrows:
        return AccessCost::TestsReceiver;
    case WalkNoteKind::GenericOperation:
    case WalkNoteKind::CallNotInlined:
    case WalkNoteKind::ObjectBuilt:
    case WalkNoteKind::ObjectNotInFrame:
    case WalkNoteKind::ObjectStayedVirtual:
    case WalkNoteKind::LoopWalkedAgain:
    case WalkNoteKind::WayEnds:
        return AccessCost::ByName;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static UnprovenCause causeInWalkNote(AccessKind access)
{
    switch (access) {
    case AccessKind::Read:
    case AccessKind::Store:
    case AccessKind::Define:
    case AccessKind::Has:
    case AccessKind::Delete:
        return UnprovenCause::UnknownObject;
    case AccessKind::Call:
    case AccessKind::Construct:
        return UnprovenCause::UnknownCallee;
    case AccessKind::Truthiness:
    case AccessKind::Equality:
    case AccessKind::Arithmetic:
    case AccessKind::Comparison:
    case AccessKind::Conversion:
    case AccessKind::TypeOf:
    case AccessKind::InstanceOf:
    case AccessKind::Iteration:
    case AccessKind::Spread:
        return UnprovenCause::Untracked;
    case AccessKind::Unknown:
        return UnprovenCause::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool placeWalkNote(const Code& code, const Operation& operation, Origin& origin, FunctionID& writtenIn)
{
    std::span<const InlineFrame> frames = code.inlineFrames();
    if (!operation.hasPosition() || operation.inlineFrame >= frames.size())
        return false;
    origin = operation.origin();
    writtenIn = frames[operation.inlineFrame].callee;
    return true;
}

static LayoutID layoutOfValueInWalkNote(TypeReader types, const Code& code, ValueID value)
{
    return singleLayoutOf(types, types.at(code.at(value).type));
}

WalkNotes notesOfSweptCode(TypeReader types, const Code& code, bool notesResolvedOperations)
{
    RELEASE_ASSERT(code.isSwept());
    WalkNotes result { code.unit(), { }, 0 };

    for (uint32_t index = 0; index < code.numberOfValues(); ++index) {
        ValueID id(index);
        const Operation& operation = code.at(id);
        BlockID block = operation.block();
        if (!block || !code.isReached(block))
            continue;
        result.numberOfValuesInDouble += code.isNeeded(id) && (operation.representation == Representation::Double || operation.representation == Representation::OptionalDouble);

        WalkNote note;
        if (!placeWalkNote(code, operation, note.origin, note.writtenIn))
            continue;
        note.loopDepth = code.at(block).isInLoop;

        if (allocationsInWalkNotes.contains(operation.opcode)) {
            note.kind = code.isNeeded(id) ? WalkNoteKind::ObjectBuilt : WalkNoteKind::ObjectStayedVirtual;
            note.cost = costOfWalkNoteKind(note.kind);
            note.layout = layoutOfValueInWalkNote(types, code, id);
            if (note.kind == WalkNoteKind::ObjectBuilt) {
                note.objectBuilt = ObjectBuiltCause::OperationNeedsCell;
                if (ValueID user = code.onlyUserOf(id); user && placeWalkNote(code, code.at(user), note.neededAt, note.neededIn))
                    note.opcode = code.at(user).opcode;
            }
            result.notes.append(note);
            continue;
        }
        if (!code.isNeeded(id))
            continue;

        note.opcode = operation.opcode;
        if (code.unit().role == CodeRole::ParameterGuard) {
            if (operation.opcode != Opcode::TypeTest)
                continue;
            note.kind = WalkNoteKind::TestThatLeaves;
            note.cost = costOfWalkNoteKind(note.kind);
            note.layout = singleLayoutOf(types, types.at(operation.testedType()));
            result.notes.append(note);
            continue;
        }
        if (testsInWalkNotes.contains(operation.opcode)) {
            note.kind = WalkNoteKind::TestThatRejoins;
            note.cost = costOfWalkNoteKind(note.kind);
            if (ValueID tested = operation.child(0))
                note.layout = layoutOfValueInWalkNote(types, code, tested);
            result.notes.append(note);
            continue;
        }

        note.access = accessKindInWalkNote(operation);
        if (note.access == AccessKind::Unknown)
            continue;
        note.kind = kindOfOperationInWalkNote(operation);
        if (note.kind == WalkNoteKind::ResolvedOperation && !notesResolvedOperations)
            continue;
        note.cost = costOfWalkNoteKind(note.kind);
        if (note.kind != WalkNoteKind::ResolvedOperation)
            note.cause = causeInWalkNote(note.access);
        if (std::optional<uint8_t> receiver = receiverOf(operation.opcode)) {
            if (ValueID object = operation.child(*receiver))
                note.layout = layoutOfValueInWalkNote(types, code, object);
        }
        result.notes.append(note);
    }
    for (const Code::TestOfGuardInFrontOfCopy& noted : code.testsOfGuardsInFrontOfCopies()) {
        const Operation& operation = code.at(noted.test);
        BlockID block = operation.block();
        if (operation.opcode != Opcode::TypeTest || !block || !code.isReached(block) || !code.isNeeded(noted.test))
            continue;
        WalkNote note;
        note.kind = WalkNoteKind::TestThatLeaves;
        note.opcode = operation.opcode;
        note.cost = costOfWalkNoteKind(note.kind);
        note.loopDepth = code.at(block).isInLoop;
        note.origin = Origin { noted.positionOfCallee, 0, false, false };
        note.writtenIn = noted.callee;
        note.layout = singleLayoutOf(types, types.at(operation.testedType()));
        result.notes.append(note);
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
