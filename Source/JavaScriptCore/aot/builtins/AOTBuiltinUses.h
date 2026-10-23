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
    // An image that hides the names of functions answers the empty string for the name of a function of the program.
    bool keepsFunctionNames { false };
    bool asksOnlyForProvenPlans { false };
    // Set by a plan that follows from more than the use, the types of its operands and the program: from where the use stands or from who else uses a value.
    bool* asksAboutPlace { nullptr };

    TypingContext typing() const { return { program, function, types }; }
    BuiltinLookupContext lookup() const { return { program, flow }; }
    PristineQuery pristine() const { return { program, function, analyses, passedTests, asksAboutPlace }; }
    const UseLists& useLists() const;
};

// Undefined: the read finds nothing. HostValue: the read gives what the host made at the first read, which the instance keeps in a cell of its own.
enum class BuiltinUseKind : uint8_t { None, Constant, Undefined, HostValue, Call, UnmatchedCall, LibraryCall, NewArray };

// What is required of a value, or of a location: nothing has written the property that holds the member since start-up. A requirement on a location has no subject.
struct UseRequirement {
    ValueID subject;
    TypeID requiredType;
    PristineRequirement pristine;
    BuiltinID unchangedLocation { };
    // The subject is a function object with the native code of this built-in, of which no cell can be named.
    BuiltinID functionOfBuiltin { };
};

struct BuiltinUsePlan {
    BuiltinUseKind kind { BuiltinUseKind::None };
    BuiltinID member;
    PropertyKeyID key;
    // A constant that the table does not hold: the length or the name of a function of the program.
    std::optional<Value> constant;
    BuiltinCallPlan call;
    BuiltinCallForm form { BuiltinCallForm::Whole };
    BuiltinCallOperands operands;
    BuiltinCallStamps stamps;
    NullishCheck nullishCheck;
    FunctionID libraryFunction;
    // What a construction for another constructor than the built-in itself passes as new.target.
    ValueID newTarget;
    // What the operation is known to give and the function of the library does not declare. The direct call runs what the operation ran, so its result is assumed to have this type.
    TypeID assumedResult;
    bool isNeverInlined { false };
    Vector<UseRequirement, 2> requirements;

    explicit operator bool() const { return kind != BuiltinUseKind::None; }
    bool isProven() const { return kind != BuiltinUseKind::None && requirements.isEmpty(); }
};

// A plan has one requirement per subject and one per location. Both answer false when no value meets what is required of the subject together.
bool appendRequirement(TypeArena&, BuiltinUsePlan&, const UseRequirement&);
bool prependRequirement(TypeArena&, BuiltinUsePlan&, UseRequirement);
void degradeToUnmatchedCall(BuiltinUsePlan&);
// No object has a layout that is never allocated: a test for such layouts never passes, and an operation on a value of such a type never runs.
bool hasOnlyNeverAllocatedLayouts(const Program&, const Type&);
// The class Function has no test of its own. One test finds the closures of a function of the program, and another finds every function.
std::optional<Type> testedTypeOfExpectedInstances(const Program&, TypeArena&, BuiltinClassID, const Type& expected, const Type& present);

BuiltinUsePlan planBuiltinUse(const BuiltinUseInputs&, ValueID use, ProgramPoint);
// False only where planBuiltinUse(), asked for proven plans alone, finds none. It looks at the use and at the types of its operands, and makes nothing.
bool mayFindProvenPlan(const BuiltinUseInputs&, const Value& use);
bool mayFindProvenAccess(const BuiltinUseInputs&, const Value& access);
bool mayFindProvenCallee(const BuiltinUseInputs&, const Value& call);

BuiltinUsePlan planBuiltinRead(const BuiltinUseInputs&, ValueID use, ProgramPoint);
BuiltinUsePlan planBuiltinStore(const BuiltinUseInputs&, ValueID use, ProgramPoint);
BuiltinUsePlan planCallOfBuiltin(const BuiltinUseInputs&, ValueID use, ProgramPoint);
BuiltinUsePlan planSuperCallOfBuiltin(const BuiltinUseInputs&, ValueID use, ProgramPoint);
// The call reads its arguments from the array, which holds them at the point.
BuiltinUsePlan planListCallOfBuiltin(const BuiltinUseInputs&, ValueID use, ValueID array, ProgramPoint);
// The spread of f(...iterable), when it alone fills the list of the call and the list serves nothing else.
ValueID findOnlySpreadIntoListOf(const BuiltinUseInputs&, ValueID listCall);
// What has to be tested at the point before the elements of the array stand for what iterating it gives. No answer: the value is neither known nor expected to be an array.
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

// Compiled code names no cell of a function of which each module has an object of its own. It calls the native code behind an overload of such a function where the host has bound that code.
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
