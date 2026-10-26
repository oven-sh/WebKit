/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsCheck.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTPropertyAccess.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

void forEachValueSite(FunctionAnalyses&, const Function&, NOESCAPE const ScopedLambda<void(const ValueSite&)>&);

// The functor may not edit the blocks or their lists.
void forEachValueSite(FunctionAnalyses& analyses, const Function& function, const OpcodeSet& wanted, NOESCAPE const Invocable<void(const ValueSite&)> auto& functor)
{
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        std::span<const ValueID> values = function.at(block).values.span();
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (wanted.contains(function.at(values[index]).opcode))
                functor(ValueSite { block, index, values[index] });
        }
    }
}

// The operations for which isNamedAccess() or elementAccessOf() answers. SplitUnions looks at no other.
inline constexpr OpcodeSet accessesByNameOrKey = namedAccesses | OpcodeSet { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemDefine, Opcode::ElemHas };

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
    SiteExpansion(FunctionPassContext& context, IRBuilder& builder)
        : m_context(context)
        , m_builder(builder)
    {
    }
    ~SiteExpansion() { RELEASE_ASSERT(!isOpen()); }

    void begin(const ValueSite&);
    void end();
    bool isOpen() const { return !!m_site.value; }

    FunctionPassContext& context() { return m_context; }
    IRBuilder& builder() { return m_builder; }
    Function& function() { return m_builder.function(); }
    const Value& original() const LIFETIME_BOUND { return m_original; }
    BlockFrequency frequency() const { return m_frequency; }
    bool isTailCall() const { return m_tailCall || m_original.isTailCall; }
    bool isRequiredTailCall() const { return m_tailCall && m_tailCall->requirement() == TailCallRequirement::Required; }

    void addReplaced(const Value&);
    void setOperationsBeforeOriginal(Vector<Value, 2>&&, uint32_t operandIndexInOriginal);

    ValueID append(Value&&);
    ValueID appendOriginal(const OriginalCopy& = { });

    void finishArm(ValueID result);
    void finishArmWithoutResult();
    void finishArmAsUnreachable();

private:
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
