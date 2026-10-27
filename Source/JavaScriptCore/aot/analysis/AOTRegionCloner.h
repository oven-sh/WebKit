/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRBuilder.h"
#include "AOTVector.h"
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class Liveness;
class SSARepair;

enum class CloneCheck : uint8_t { Unknown, OperandMustBeDefinition, FunctionMayNotBeCloned, BoundToFrame, UniqueInFunction, HandlerOutsideRegion, CanClone };

class RegionCloner {
    WTF_MAKE_NONCOPYABLE(RegionCloner);
public:
    RegionCloner(IRBuilder& destination, const Function& source);

    static CloneCheck checkCanDuplicate(FunctionAnalyses&, std::span<const BlockID> region, std::span<const BlockID> entries = { });
    using LivenessSource = ScopedLambda<const Liveness&()>;
    static CloneCheck checkCanDuplicate(FunctionAnalyses&, std::span<const BlockID> region, std::span<const BlockID> entries, NOESCAPE const LivenessSource&);
    static std::unique_ptr<Liveness> computeLivenessInside(FunctionAnalyses&, std::span<const BlockID> blocks, std::span<const BlockID> entries);
    static CloneCheck checkCanExtract(FunctionAnalyses&, std::span<const BlockID> region);
    static CloneCheck checkCanInline(const Function& callee);
    static bool canMergeAt(FunctionAnalyses&, const BitVector& isInRegion, BlockID join);
    static Vector<ValueID> findLiveIns(const Function&, std::span<const BlockID> region);

    void mapValue(ValueID inSource, ValueID inDestination);
    void mapBlock(BlockID inSource, BlockID inDestination);

    void cloneBlocks(std::span<const BlockID> region, NOESCAPE const Invocable<void(ValueID inSource, Value& clone)> auto& didCloneValue)
    {
        cloneBlocksImpl(region, didCloneValue);
    }

    ValueID cloneOf(ValueID inSource) const;
    BlockID cloneOf(BlockID inSource) const;
    std::span<const BlockID> clonedBlocks() const { return m_clonedBlocks.span(); }
    const Function& source() const { return m_source; }

    void repairSSA(FunctionAnalyses&);
    static void repairSSA(FunctionAnalyses&, std::span<const std::unique_ptr<RegionCloner>> clonersOfOneRegion);
    void addVariablesTo(SSARepair&, FunctionAnalyses&) const;

private:
    static void addVariables(SSARepair&, FunctionAnalyses&, std::span<const RegionCloner* const> clonersOfOneRegion);

    void cloneBlocksImpl(std::span<const BlockID> region, NOESCAPE const ScopedLambda<void(ValueID, Value&)>& didCloneValue);
    bool saysAsMuchAs(ValueID inDestination, ValueID inSource) const;

    bool isSameFunction() const { return &m_source == &m_destination.function(); }

    IRBuilder& m_destination;
    const Function& m_source;
    Vector<ValueID> m_valueMap;
    Vector<BlockID> m_blockMap;
    BitVector m_isSeeded;
    BitVector m_isInRegion;
    Vector<BlockID> m_region;
    Vector<BlockID> m_clonedBlocks;
    Vector<ValueID> m_rematerializedClone;
    Vector<BlockID> m_rematerializedIn;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
