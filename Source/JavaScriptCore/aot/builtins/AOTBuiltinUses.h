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
#include "AOTVector.h"
#include <optional>
#include <span>

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
    bool keepsFunctionNames { false };
    bool asksOnlyForProvenPlans { false };
    bool* asksAboutPlace { nullptr };
    FunctionAnalyses* identities { nullptr };

    TypingContext typing() const { return { program, function, types }; }
    BuiltinLookupContext lookup() const { return { program, flow }; }
    PristineQuery pristine() const { return { program, function, analyses, passedTests, asksAboutPlace }; }
    const UseLists& useLists() const;
};

enum class BuiltinUseKind : uint8_t { None, Constant, Undefined, HostValue, Call, UnmatchedCall, LibraryCall, NewArray };

struct UseRequirement {
    ValueID subject;
    TypeID requiredType;
    PristineRequirement pristine;
    BuiltinID unchangedLocation { };
    BuiltinID functionOfBuiltin { };
};

struct BuiltinUsePlan {
    BuiltinUseKind kind { BuiltinUseKind::None };
    BuiltinID member;
    PropertyKeyID key;
    std::optional<Value> constant;
    BuiltinCallPlan call;
    BuiltinCallForm form { BuiltinCallForm::Whole };
    BuiltinCallOperands operands;
    BuiltinCallStamps stamps;
    NullishCheck nullishCheck;
    FunctionID libraryFunction;
    ValueID newTarget;
    TypeID assumedResult;
    bool isNeverInlined { false };
    // A private name of the engine finds the member, and the property that the program sees stays as it is.
    bool readsPrivateAlias { false };
    Vector<UseRequirement, 2> requirements;

    explicit operator bool() const { return kind != BuiltinUseKind::None; }
    bool isProven() const { return kind != BuiltinUseKind::None && requirements.isEmpty(); }
};

bool appendRequirement(TypeArena&, BuiltinUsePlan&, const UseRequirement&);
bool prependRequirement(TypeArena&, BuiltinUsePlan&, UseRequirement);
void degradeToUnmatchedCall(BuiltinUsePlan&);
bool hasOnlyNeverAllocatedLayouts(const Program&, const Type&);
std::optional<Type> testedTypeOfExpectedInstances(const Program&, TypeArena&, BuiltinClassID, const Type& expected, const Type& present);

BuiltinUsePlan planBuiltinUse(const BuiltinUseInputs&, ValueID use, ProgramPoint);
bool mayFindProvenPlan(const BuiltinUseInputs&, const Value& use);
bool mayFindProvenAccess(const BuiltinUseInputs&, const Value& access);
bool mayFindProvenCallee(const BuiltinUseInputs&, const Value& call);

BuiltinUsePlan planBuiltinRead(const BuiltinUseInputs&, ValueID use, ProgramPoint);
BuiltinUsePlan planBuiltinStore(const BuiltinUseInputs&, ValueID use, ProgramPoint);
BuiltinUsePlan planCallOfBuiltin(const BuiltinUseInputs&, ValueID use, ProgramPoint);
BuiltinUsePlan planSuperCallOfBuiltin(const BuiltinUseInputs&, ValueID use, ProgramPoint);
BuiltinUsePlan planListCallOfBuiltin(const BuiltinUseInputs&, ValueID use, ValueID array, ProgramPoint);
ValueID findOnlySpreadIntoListOf(const BuiltinUseInputs&, ValueID listCall);
std::optional<UseRequirement> requirementOfArgumentArray(const BuiltinUseInputs&, ValueID array, ProgramPoint);
BuiltinUsePlan planRematch(const BuiltinUseInputs&, ValueID use, ProgramPoint);
BuiltinUsePlan planArraySpeciesCreate(const BuiltinUseInputs&, ValueID use, ProgramPoint);

struct KnownBuiltinCall {
    BuiltinID callee;
    BuiltinCallMode mode { BuiltinCallMode::Call };
    BuiltinCallOperands operands;
    BuiltinCallStamps stamps;
    NullishReceiver nullishReceiver { NullishReceiver::ThrowsInBuiltin };
    BuiltinOverloadID overloadOfWholeCall;
};

bool isCalledOnlyDirectly(const BuiltinTable&, BuiltinID callee);
bool hasDirectFunction(const BuiltinTable&, BuiltinID callee);
BuiltinUsePlan planKnownBuiltinCall(const BuiltinUseInputs&, const KnownBuiltinCall&, ProgramPoint);
bool insertsOperations(const BuiltinCallPlan&);

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
