/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeSizeEstimate.h"

#if ENABLE(AOT)

#include "AOTEffectSummaries.h"
#include "AOTEffects.h"
#include "AOTIR.h"
#include "AOTPropertyAccess.h"
#include "AOTValueIdentity.h"
#include <wtf/SaturatingArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

namespace CodeSize {

constexpr unsigned simpleOperation = 8;
constexpr unsigned cellConstant = 7;
constexpr unsigned typeCheck = 16;
constexpr unsigned directCall = 8;
constexpr unsigned indirectCall = 24;
constexpr unsigned perOperand = 4;
constexpr unsigned allocation = 48;
constexpr unsigned accessByName = 20;
constexpr unsigned slotAccess = 8;
constexpr unsigned branch = 8;
constexpr unsigned jump = 4;
constexpr unsigned perCase = 8;
constexpr unsigned throwSite = 16;
constexpr unsigned stringSwitch = 24;

} // namespace CodeSize

static constexpr OpcodeSet opcodesWithoutCode {
    Opcode::Parameter,
    Opcode::Undefined,
    Opcode::Null,
    Opcode::Absent,
    Opcode::Boolean,
    Opcode::Integer,
    Opcode::Double,
    Opcode::KeyConstant,
    Opcode::Unspecified,
    Opcode::Results,
    Opcode::ResultAt,
    Opcode::LoopHint,
    Opcode::Upcast,
    Opcode::Predict,
    Opcode::AssumeType,
    Opcode::FinishObject,
    Opcode::AllGuards,
    Opcode::LogicalNot,
    Opcode::ValidateType,
    Opcode::ValidateCallee,
    Opcode::ValidateFirstRun,
    Opcode::ValidateSameValue,
};

static bool isTypeCheck(Opcode opcode)
{
    Proof proof = provesOf(opcode);
    return isCast(opcode) || provesForStretch(proof) || proof == Proof::TestedType;
}

// A body is measured before its accesses are resolved. Where the type of the receiver names its layouts the access ends as a slot operation.
static bool namesMemberOfProvenLayouts(const Program& program, const Function& function, const Value& access)
{
    return isNamedAccess(access.opcode) && program.typeOf(function, access.operand(*receiverOf(access.opcode))).layouts;
}

static unsigned switchCaseBytes(const Terminator& terminator)
{
    ASSERT(!terminator.targets.isEmpty());
    return CodeSize::perCase * static_cast<unsigned>(terminator.targets.size() - 1);
}

unsigned estimatedCodeBytes(const Program& program, const Function& function, const Value& value)
{
    Opcode opcode = value.opcode;

    if (opcodesWithoutCode.contains(opcode))
        return 0;
    if (isRematerializable(value))
        return CodeSize::cellConstant;
    if (isTypeCheck(opcode))
        return CodeSize::typeCheck;
    if (opcode == Opcode::ToBoolean)
        return program.typeOf(function, value.valueOperand()).isBoolean() ? 0 : CodeSize::simpleOperation;

    unsigned operandBytes = CodeSize::perOperand * value.numberOfOperands();

    switch (accessClassOf(opcode)) {
    case AccessClass::None:
        break;
    case AccessClass::Slot:
        return CodeSize::slotAccess;
    case AccessClass::ByName:
        return namesMemberOfProvenLayouts(program, function, value) ? CodeSize::slotAccess : CodeSize::accessByName;
    case AccessClass::DirectCall:
        return CodeSize::directCall + operandBytes;
    case AccessClass::IndirectCall:
        return CodeSize::indirectCall + operandBytes;
    }

    EffectSummaries unknownSummaries;
    Effects effects = effectsOf(EffectContext { program, unknownSummaries }, function, value).normal;
    if (effects.mayCallUnknownCode)
        return CodeSize::indirectCall + operandBytes;
    if (allocatesInlineOf(opcode))
        return CodeSize::allocation + operandBytes;
    if (effects.mayCollect || effects.mayThrow)
        return CodeSize::indirectCall + operandBytes;
    return CodeSize::simpleOperation;
}

unsigned estimatedCodeBytes(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Jump:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
        return CodeSize::jump;
    case TerminatorKind::Branch:
    case TerminatorKind::TestAndBranch:
        return CodeSize::branch;
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchOnShape:
        return CodeSize::branch + switchCaseBytes(terminator);
    case TerminatorKind::SwitchString:
        return CodeSize::stringSwitch + switchCaseBytes(terminator);
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
        return CodeSize::throwSite;
    case TerminatorKind::TryCallInline:
        return CodeSize::indirectCall + CodeSize::perOperand * terminator.numberOfOperands();
    case TerminatorKind::Unreachable:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

unsigned estimatedCodeBytes(const Program& program, const Function& function, const Block& block)
{
    unsigned bytes = estimatedCodeBytes(block.terminator);
    for (ValueID id : block.values)
        bytes = saturatingSum<unsigned>(bytes, estimatedCodeBytes(program, function, function.at(id)));
    return bytes;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
