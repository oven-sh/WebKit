/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsCheck.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTPropertyAccess.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct ValueSite {
    BlockID block;
    uint32_t index { 0 };
    ValueID value;

    ProgramPoint point() const { return { block, index }; }
};

void forEachValueSite(FunctionAnalyses&, const Function&, NOESCAPE const ScopedLambda<void(const ValueSite&)>&);

void checkRewrite(FunctionPassContext&, const ValueSite&, const ReplacedEffects&);

struct OriginalCopy {
    std::optional<uint32_t> operandIndex;
    ValueID operand;
    LayoutSetID receivers;
    CalleeSetID callees;
    // The operand is the value that it replaces with a narrower type, which holds wherever else the operation takes that value.
    bool isNarrowedValue { false };
};

class SiteExpansion {
    WTF_MAKE_NONCOPYABLE(SiteExpansion);
public:
    SiteExpansion(FunctionPassContext&, IRBuilder&);
    ~SiteExpansion();

    void begin(const ValueSite&);
    void end();
    bool isOpen() const { return !!m_site.value; }

    FunctionPassContext& context() { return m_context; }
    IRBuilder& builder() { return m_builder; }
    Function& function() { return m_builder.function(); }
    const ValueSite& site() const LIFETIME_BOUND { return m_site; }
    const Value& original() const LIFETIME_BOUND { return m_original; }
    BlockFrequency frequency() const { return m_frequency; }
    bool isTailCall() const { return m_tailCall || (FlagMask::isDemotedTailCall(m_original.opcode) && m_original.isDemotedTailCall()); }
    bool isRequiredTailCall() const { return m_tailCall && m_tailCall->requirement() == TailCallRequirement::Required; }

    void addReplaced(const Value&);
    void setOperationsBeforeOriginal(Vector<Value, 2>&&, uint32_t operandIndexInOriginal);

    ValueID append(Value&&);
    ValueID appendOriginal(const OriginalCopy& = { });

    void finishArm(ValueID result);
    void finishArmWithoutResult();
    void finishArmAsUnreachable();

private:
    ValueID appendOperationsBeforeOriginal(const OriginalCopy&);
    void checkAppendedOperations();
    void takeTailCall();
    bool canEndWithTailCall(ValueID result);
    void finishArmWithReturn(ValueID result);

    FunctionPassContext& m_context;
    IRBuilder& m_builder;
    ValueSite m_site;
    Value m_original;
    BlockFrequency m_frequency { BlockFrequency::Normal };
    IRBuilder::Expansion m_expansion;
    std::optional<Terminator> m_tailCall;
    Vector<BlockID, 2> m_continuationPredecessors;
    Vector<Value, 2> m_operationsBeforeOriginal;
    uint32_t m_operandIndexInOriginal { 0 };
    ReplacedEffects m_replaced;
    Vector<ValueID, 4> m_appended;
};

struct SplitArm {
    TypeID testedType;
    BlockFrequency frequency { BlockFrequency::Normal };
};

struct ValueSplit {
    ValueID value;
    Vector<SplitArm, 4> arms;
    bool hasRest { true };
    BlockFrequency restFrequency { BlockFrequency::Unlikely };
};

using EmitSplitArm = ScopedLambda<void(uint32_t armIndex, ValueID narrowedValue)>;
using EmitSplitRest = ScopedLambda<void(ValueID narrowedValue)>;

void emitSplit(SiteExpansion&, const ValueSplit&, NOESCAPE const EmitSplitArm&, NOESCAPE const EmitSplitRest&);

// For each part, how many of the accesses that take one of the values as their receiver, outside rare blocks, are resolved where the value is of that part.
struct AccessesResolvedOnParts {
    Vector<unsigned, 4> countOfPart;
    unsigned numberOfAccesses { 0 };
    unsigned numberOfResolvableAccesses { 0 };
    unsigned numberOfResolvableElementAccesses { 0 };
};

AccessesResolvedOnParts countAccessesResolvedOnParts(FunctionPassContext&, std::span<const ValueID> receivers, const UnionParts&);

enum class SplitReason : uint8_t { UnionParts, MemberArms, ConstantKeys, NumberKey };

struct PlannedSplit {
    ValueSite site;
    SplitReason reason { SplitReason::UnionParts };
    uint32_t operandIndex { 0 };
    ValueSplit split;
    Vector<LayoutSetID, 4> armReceivers;
    LayoutSetID restReceivers;
    // The site is a call, and these are the read of its callee and the casts of what was read. Each arm reads the callee from its own receiver.
    Vector<ValueSite, 2> memberRead;
};

class UnionSplitter {
    WTF_MAKE_NONCOPYABLE(UnionSplitter);
public:
    explicit UnionSplitter(FunctionPassContext&);

    PassOutcome run();

private:
    Vector<PlannedSplit> planSplits();
    std::optional<PlannedSplit> planSplit(const ValueSite&);
    std::optional<PlannedSplit> planSplitByParts(const ValueSite&, uint32_t operandIndex);
    std::optional<PlannedSplit> planSplitByArms(const ValueSite&);
    std::optional<PlannedSplit> planSplitByKeys(const ValueSite&);
    std::optional<PlannedSplit> planSplitOfNumberKey(const ValueSite&);
    void moveToMemberCall(PlannedSplit&);
    void rewrite(const PlannedSplit&);

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    IRBuilder m_builder;
    SiteExpansion m_expansion;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
