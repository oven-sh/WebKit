/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTPass.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet builtinUseOpcodes { Opcode::PropGet, Opcode::PropSet, Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::CallBuiltin, Opcode::ArraySpeciesCreate };

struct BuiltinUseInputs {
    const Program& program;
    const Function& function;
    TypeArena& types;
    const FlowResults& flow;
    const EffectContext& effects;
    FunctionAnalyses* analyses { nullptr };
    std::span<const Value> passedTests { };

    TypingContext typing() const { return { program, function, types }; }
    BuiltinLookupContext lookup() const { return { program, flow }; }
    PristineQuery pristine() const { return { program, function, analyses, passedTests }; }
};

enum class BuiltinUseKind : uint8_t { None, Constant, Call, UnmatchedCall, LibraryCall, NewArray };

struct UseRequirement {
    ValueID subject;
    TypeID requiredType;
    PristineRequirement pristine;
};

struct BuiltinUsePlan {
    BuiltinUseKind kind { BuiltinUseKind::None };
    BuiltinID member;
    PropertyKeyID key;
    BuiltinCallPlan call;
    BuiltinCallForm form { BuiltinCallForm::Whole };
    BuiltinCallOperands operands;
    BuiltinCallStamps stamps;
    NullishCheck nullishCheck;
    FunctionID libraryFunction;
    // What a construction for another constructor than the built-in itself passes as new.target.
    ValueID newTarget;
    // What the table promises about the result and the function of the library does not declare. The result of the direct call is claimed as this type.
    TypeID claimedResult;
    bool isNeverInlined { false };
    Vector<UseRequirement, 2> requirements;

    explicit operator bool() const { return kind != BuiltinUseKind::None; }
    bool isProven() const { return kind != BuiltinUseKind::None && requirements.isEmpty(); }
};

// A plan has one requirement per subject. Both answer false when no value meets what is required of the subject together.
bool appendRequirement(TypeArena&, BuiltinUsePlan&, const UseRequirement&);
bool prependRequirement(TypeArena&, BuiltinUsePlan&, UseRequirement);
void degradeToUnmatchedCall(BuiltinUsePlan&);
// No object has a layout that is never allocated: a test for such layouts never passes, and an operation on a value of such a type never runs.
bool hasOnlyNeverAllocatedLayouts(const Program&, const Type&);

BuiltinUsePlan planBuiltinUse(const BuiltinUseInputs&, ValueID use, ProgramPoint);

BuiltinUsePlan planBuiltinRead(const BuiltinUseInputs&, ValueID use, ProgramPoint);
BuiltinUsePlan planBuiltinStore(const BuiltinUseInputs&, ValueID use, ProgramPoint);
BuiltinUsePlan planCallOfBuiltin(const BuiltinUseInputs&, ValueID use, ProgramPoint);
BuiltinUsePlan planSuperCallOfBuiltin(const BuiltinUseInputs&, ValueID use, ProgramPoint);
// The call reads its arguments from the array, which holds them at the point.
BuiltinUsePlan planListCallOfBuiltin(const BuiltinUseInputs&, ValueID use, ValueID array, ProgramPoint);
// The spread of f(...iterable), when it alone fills the list of the call and the list serves nothing else.
ValueID findOnlySpreadIntoListOf(const BuiltinUseInputs&, ValueID listCall);
// What has to be tested at the point before the elements of the array stand for what iterating it gives. No answer: the value is neither known nor predicted to be an array.
std::optional<UseRequirement> requirementOfArgumentArray(const BuiltinUseInputs&, ValueID array, ProgramPoint);
BuiltinUsePlan planRematch(const BuiltinUseInputs&, ValueID use, ProgramPoint);
BuiltinUsePlan planArraySpeciesCreate(const BuiltinUseInputs&, ValueID use, ProgramPoint);

struct KnownBuiltinCall {
    BuiltinID callee;
    BuiltinCallMode mode { BuiltinCallMode::Call };
    BuiltinCallOperands operands;
    BuiltinCallStamps stamps;
    NullishReceiver nullishReceiver { NullishReceiver::ThrowsInBuiltin };
};

BuiltinUsePlan planKnownBuiltinCall(const BuiltinUseInputs&, const KnownBuiltinCall&, ProgramPoint);

Vector<Value, 4> createTestsOf(std::span<const UseRequirement>);
Vector<Value, 4> createTestsOf(const BuiltinUsePlan&);
Effects effectsWhenApplied(const BuiltinUseInputs&, const BuiltinUsePlan&);

enum class ResultFit : uint8_t { Unknown, Within };

ResultFit resultFitOf(const BuiltinUseInputs&, ValueID use, const BuiltinUsePlan&);

struct BuiltinUseRewrite {
    FunctionPassContext& context;
    ValueID use;
    BlockID block;
    const InsertBeforeCall& insert;
};

void applyBuiltinUse(const BuiltinUseRewrite&, const BuiltinUsePlan&);

enum class BuiltinUseResolution : uint8_t { Unknown, Resolves };

BuiltinUseResolution resolutionWithReceiver(const BuiltinUseInputs&, const Value& operation, const Type& receiver);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
