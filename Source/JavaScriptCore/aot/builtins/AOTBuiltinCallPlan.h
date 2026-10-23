/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTEffects.h"
#include "AOTVector.h"
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

struct OperandRequirement {
    BuiltinOperand operand;
    TypeID requiredType;
    PristineRequirement pristine;
};

// The receiver comes first, then the arguments in order.
bool precedes(BuiltinOperand, BuiltinOperand);

// The verifier rejects a TypeTest of a type with class proofs. The result of the test has them again, from the type of its operand.
Type withoutClassProofs(const Type&);

enum class BuiltinMatchState : uint8_t { Unknown, NeedsTests, Proven };

struct OverloadQuery {
    const TypingContext& context;
    BuiltinID builtin;
    BuiltinCallMode mode { BuiltinCallMode::Call };
    const BuiltinOperandTypes& proven;
    const BuiltinOperandTypes& expected;
};

struct OverloadMatch {
    BuiltinMatchState state { BuiltinMatchState::Unknown };
    BuiltinOverloadID overload;
    Vector<OperandRequirement, 2> requirements;
};

OverloadMatch matchOverload(const OverloadQuery&);
void forEachReachableOverload(const OverloadQuery&, NOESCAPE const ScopedLambda<void(BuiltinOverloadID)>&);

std::span<const Opcode> conversionOpcodesOf(BuiltinConversion);
Protocols protocolsRunBy(BuiltinConversion);
Type typeConvertedWithoutUserCode(const TypingContext&, const Type& operand, BuiltinConversion);

enum class OperandStepKind : uint8_t { Pass, PassUndefined, Convert, RequireObjectCoercible };

// Consecutive steps may name one operand: each works on the result of the one before. Pass is the only step of its operand.
struct OperandStep {
    BuiltinOperand operand;
    OperandStepKind kind { OperandStepKind::Pass };
    BuiltinConversion conversion { BuiltinConversion::None };
};

struct BuiltinCallPlan {
    BuiltinMatchState state { BuiltinMatchState::Unknown };
    BuiltinOverloadID overload;
    Vector<OperandRequirement, 2> requirements;
    Vector<OperandStep, 4> steps;
    bool hasReceiver { false };
    uint32_t numberOfArguments { 0 };

    bool isProven() const { return state == BuiltinMatchState::Proven; }
};

enum class NullishReceiver : uint8_t { ThrowsInBuiltin, ThrowsAtRead };

struct BuiltinCallQuery {
    OverloadQuery overloads;
    const EffectContext& effects;
    NullishReceiver nullishReceiver { NullishReceiver::ThrowsInBuiltin };
};

BuiltinCallPlan planBuiltinCall(const BuiltinCallQuery&);

struct BuiltinCallOperands {
    ValueID receiver;
    Vector<ValueID, 4> arguments;
};

struct BuiltinCallStamps {
    ExpectedResultID expectedResult;
    AccessSiteID accessSite;
    AllocationSiteID allocationSite;
};

struct NullishCheck {
    NullishAccess access { NullishAccess::Read };
    StringID name;
};

using InsertBeforeCall = ScopedLambda<ValueID(Value&&)>;

BuiltinCallOperands insertOperandSteps(const BuiltinCallPlan&, const BuiltinCallOperands&, const NullishCheck&, NOESCAPE const InsertBeforeCall&);
void becomeBuiltinCall(Value&, BuiltinOverloadID, BuiltinCallForm, const BuiltinCallOperands&, const BuiltinCallStamps&);
Value makeBuiltinCall(BuiltinOverloadID, BuiltinCallForm, const BuiltinCallOperands&, const BuiltinCallStamps&);

Effects effectsOfPlannedCall(const BuiltinCallQuery&, const BuiltinCallPlan&, BuiltinCallForm);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
