/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinUses.h"
#include "AOTForwardedCalls.h"
#include "AOTIRBuilder.h"
#include "AOTSplitUnions.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

std::optional<TailCallRequirement> tailCallRequirementOf(const Function&, BlockID, ValueID call);
void demoteTailCall(FunctionPassContext&, IRBuilder&, BlockID);

struct BuiltinSite {
    ProgramPoint point;
    ValueID id;
    BuiltinUsePlan plan;
};

struct BuiltinResolutionCounts {
    uint32_t reads { 0 };
    uint32_t calls { 0 };
    uint32_t conversions { 0 };
    uint32_t demotedTailCalls { 0 };

    bool didChange() const { return reads || calls || demotedTailCalls; }
};

class BuiltinResolver {
    WTF_MAKE_NONCOPYABLE(BuiltinResolver);
public:
    explicit BuiltinResolver(FunctionPassContext&);

    PassOutcome run();

private:
    BuiltinUseInputs inputs() const;
    Vector<BuiltinSite> collectSites();
    void demoteTailCalls(std::span<const BuiltinSite>);
    void apply(const BuiltinSite&);
    void count(const BuiltinUsePlan&);

    FunctionPassContext& m_context;
    Function& m_function;
    IRBuilder m_builder;
    BuiltinResolutionCounts m_counts;
};

struct CalleeSplit {
    ValueSite site;
    ValueID callee;
    Vector<MemberOfClasses, 4> members;
};

bool predictReceiversFromSeenMethods(FunctionPassContext&);
Vector<CalleeSplit> planCalleeSplits(FunctionPassContext&);
void emitCalleeSplits(FunctionPassContext&, std::span<const CalleeSplit>);

enum class InstanceOfAnswer : uint8_t { Unknown, No, Yes };

struct BuiltinInstanceOfSite {
    ValueSite site;
    BuiltinClassID builtinClass;
    InstanceOfAnswer answer { InstanceOfAnswer::Unknown };
    // Set where the constructor is only predicted: the rest of the plan holds where the operand is this object.
    BuiltinID constructorToTest;
    bool testsClass { false };
};

InstanceOfAnswer instanceOfAnswerFor(const BuiltinLookupContext&, const Type& subject, BuiltinClassID);
Vector<BuiltinInstanceOfSite> planBuiltinInstanceOfSites(FunctionPassContext&);
void rewriteBuiltinInstanceOfSites(FunctionPassContext&, std::span<const BuiltinInstanceOfSite>);

// Once the requirements hold the call reads its arguments from the array. The list and the spread that fills it go away with the call: a call that passes an array as it is has neither.
// The plan is for a callee that is a built-in function with a list form. Any other callee stays what it is.
struct ListCallSite {
    ValueSite site;
    ValueID list;
    ValueID spread;
    ValueID array;
    Vector<UseRequirement, 2> requirements;
    BuiltinUsePlan plan;
};

Vector<ListCallSite> planListCalls(FunctionPassContext&);
void emitListCalls(FunctionPassContext&, std::span<const ListCallSite>);

struct ForwardedTailCall {
    ValueSite site;
    ForwardedCallPlan plan;
};

struct CalledOperandTest {
    ValueSite site;
    uint32_t operandIndex { 0 };
};

Vector<CalledOperandTest> planTestsOfCalledOperands(FunctionPassContext&);
void emitTestsOfCalledOperands(FunctionPassContext&, std::span<const CalledOperandTest>);

Vector<ForwardedTailCall> planForwardedTailCalls(FunctionPassContext&);
void rewriteForwardedTailCalls(FunctionPassContext&, std::span<const ForwardedTailCall>);

unsigned expandStaticRequires(FunctionPassContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
