/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowResultFamilies.h"
#include "AOTFlowSinkEquations.h"
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class SinkNodeKind : uint8_t {
    GraphNode,
    ObjectLifetime,
    ResultHub,
    OuterMemory,
};

struct SinkNodeNumbering {
    uint32_t numberOfGraphNodes { 0 };
    uint32_t numberOfObjects { 0 };
    uint32_t numberOfCallLinks { 0 };

    uint32_t graphNode(FlowNodeID node) const { return node.index(); }
    uint32_t lifetime(FlowObjectID object) const { return numberOfGraphNodes + object.index(); }
    uint32_t resultHub(FlowCallLinkID link) const { return numberOfGraphNodes + numberOfObjects + link.index(); }
    // Stands for any object that the storing activation did not allocate. Its base use is Retains: that object outlives the activation even if it never leaves its own allocator's frame.
    uint32_t outerMemory() const { return numberOfGraphNodes + numberOfObjects + numberOfCallLinks; }
    uint32_t size() const { return outerMemory() + 1; }

    SinkNodeKind kindOf(uint32_t node) const
    {
        if (node < numberOfGraphNodes)
            return SinkNodeKind::GraphNode;
        if (node < numberOfGraphNodes + numberOfObjects)
            return SinkNodeKind::ObjectLifetime;
        return node < outerMemory() ? SinkNodeKind::ResultHub : SinkNodeKind::OuterMemory;
    }

    FlowNodeID graphNodeAt(uint32_t node) const
    {
        RELEASE_ASSERT(kindOf(node) == SinkNodeKind::GraphNode);
        return FlowNodeID(node);
    }

    FlowObjectID objectAt(uint32_t node) const
    {
        RELEASE_ASSERT(kindOf(node) == SinkNodeKind::ObjectLifetime);
        return FlowObjectID(node - numberOfGraphNodes);
    }

    FlowCallLinkID callLinkAt(uint32_t node) const
    {
        RELEASE_ASSERT(kindOf(node) == SinkNodeKind::ResultHub);
        return FlowCallLinkID(node - numberOfGraphNodes - numberOfObjects);
    }
};

struct SinkRule {
    uint32_t subject { 0 };
    SinkDependency dependency;
};

struct BaseUses {
    uint32_t subject { 0 };
    OperandUses uses;
};

struct CollectedSinkRules {
    Vector<SinkRule> dependencies;
    Vector<BaseUses> baseUses;
    Vector<uint32_t> operandsOfNativeCode;
};

SinkNodeNumbering numberSinkNodes(const FlowSolution&);
Vector<CollectedSinkRules> collectSinkRules(const FlowFamilyInputs&, const SinkNodeNumbering&);
SinkEquations buildSinkEquations(const FlowFamilyInputs&, const SinkNodeNumbering&);

enum class StoreContainerKind : uint8_t {
    Unknown,
    EntryParameter,
    OwnAllocation,
};

struct StoreContainer {
    StoreContainerKind kind { StoreContainerKind::Unknown };
    FlowLocalID root;
};

StoreContainer findStoreContainer(const Program&, const FlowSummary&, FlowLocalID receiver);

struct SinkRetention {
    RetainedIn retainedIn { RetainedIn::UnknownMemory };
    uint32_t retainingOperand { 0 };

    friend bool operator==(const SinkRetention&, const SinkRetention&) = default;
};

struct SinkRetentionInputs {
    const FlowFamilyInputs& families;
    const FlowVersionPlan& versions;
    const SinkNodeNumbering& numbering;
    const SinkEquations& equations;
};

Vector<Vector<SinkRetention>> computeSinkRetention(const SinkRetentionInputs&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
