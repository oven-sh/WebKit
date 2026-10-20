/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowObjects.h"
#include "AOTFlowStatements.h"
#include "AOTPrimitives.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/HashTraits.h>
#include <wtf/Hasher.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class FlowExternalKind : uint8_t {
    Variable,
    Heritage,
    NativeCode,
    UnknownJavaScript,
    SiteField,
    SiteCapture,
    SiteSlot,
    SitePosition,
    SiteUndeclaredPart,
};

class FlowExternalNode {
public:
    static FlowExternalNode variable(VariableID variable) { return { FlowExternalKind::Variable, variable.index(), 0 }; }
    static FlowExternalNode heritage(LayoutID layout) { return { FlowExternalKind::Heritage, layout.index(), 0 }; }
    static FlowExternalNode nativeCode() { return { FlowExternalKind::NativeCode, 0, 0 }; }
    static FlowExternalNode unknownJavaScript() { return { FlowExternalKind::UnknownJavaScript, 0, 0 }; }
    static FlowExternalNode siteField(uint32_t site, uint32_t fieldIndex) { return { FlowExternalKind::SiteField, site, fieldIndex }; }
    static FlowExternalNode siteCapture(uint32_t site, uint32_t captureIndex) { return { FlowExternalKind::SiteCapture, site, captureIndex }; }
    static FlowExternalNode siteSlot(uint32_t site, BuiltinSlotID slot) { return { FlowExternalKind::SiteSlot, site, slot.index() }; }
    static FlowExternalNode sitePosition(uint32_t site, uint32_t position) { return { FlowExternalKind::SitePosition, site, position }; }
    static FlowExternalNode siteUndeclaredPart(uint32_t site) { return { FlowExternalKind::SiteUndeclaredPart, site, 0 }; }

    FlowExternalKind kind() const { return m_kind; }

    bool isPartOfSite() const
    {
        switch (m_kind) {
        case FlowExternalKind::SiteField:
        case FlowExternalKind::SiteCapture:
        case FlowExternalKind::SiteSlot:
        case FlowExternalKind::SitePosition:
        case FlowExternalKind::SiteUndeclaredPart:
            return true;
        case FlowExternalKind::Variable:
        case FlowExternalKind::Heritage:
        case FlowExternalKind::NativeCode:
        case FlowExternalKind::UnknownJavaScript:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    VariableID variable() const
    {
        RELEASE_ASSERT(m_kind == FlowExternalKind::Variable);
        return VariableID(m_id);
    }

    LayoutID layout() const
    {
        RELEASE_ASSERT(m_kind == FlowExternalKind::Heritage);
        return LayoutID(m_id);
    }

    uint32_t site() const
    {
        RELEASE_ASSERT(isPartOfSite());
        return m_id;
    }

    uint32_t index() const
    {
        RELEASE_ASSERT(m_kind == FlowExternalKind::SiteField || m_kind == FlowExternalKind::SiteCapture || m_kind == FlowExternalKind::SitePosition);
        return m_index;
    }

    BuiltinSlotID slot() const
    {
        RELEASE_ASSERT(m_kind == FlowExternalKind::SiteSlot);
        return BuiltinSlotID(m_index);
    }

    friend bool operator==(const FlowExternalNode&, const FlowExternalNode&) = default;

    friend void add(Hasher& hasher, const FlowExternalNode& node)
    {
        add(hasher, static_cast<uint8_t>(node.m_kind), node.m_id, node.m_index);
    }

private:
    FlowExternalNode(FlowExternalKind kind, uint32_t id, uint32_t index)
        : m_kind(kind)
        , m_id(id)
        , m_index(index)
    {
    }

    FlowExternalKind m_kind;
    uint32_t m_id;
    uint32_t m_index;
};

static_assert(sizeof(FlowExternalNode) == 12);

template<typename T>
struct FlowAdjacency {
    Vector<uint32_t> offsets;
    Vector<T> entries;

    std::span<const T> of(FlowLocalID node) const LIFETIME_BOUND
    {
        uint32_t first = offsets[node.index()];
        uint32_t end = offsets[node.index() + 1];
        RELEASE_ASSERT(first <= end && end <= entries.size());
        return entries.subspan(first, end - first);
    }
};

enum class FlowClosureUse : uint8_t { Other, StoredOnce };
enum class FlowClosureCapture : uint8_t { Other, Nothing, StoredIntoObject };

struct FlowClosureCreation {
    FlowLocalID node;
    ValueID origin;
    FunctionID function;
    uint32_t site { 0 };
    FlowClosureUse use { FlowClosureUse::Other };
    FlowClosureCapture capture { FlowClosureCapture::Other };
};

struct FlowCoroutineSummary {
    std::optional<uint32_t> resultSite;
    FlowLocalID yielded;
    FlowLocalID sent;
};

enum class FlowSummaryFlag : uint8_t {
    PassesParameterOn = 1 << 0,
    CreatesObjects = 1 << 1,
    HasCalls = 1 << 2,
    // A result is what an allocation of the function or one of its calls gave.
    HandsUpNewObject = 1 << 3,
};

struct FlowSummary {
    FunctionID function;
    uint32_t numberOfLocals { 0 };
    uint32_t numberOfEntryParameters { 0 };
    uint32_t numberOfResults { 0 };
    uint32_t numberOfValues { 0 };
    OptionSet<FlowSummaryFlag> flags;
    // Values that hold the same in every solution share a local.
    Vector<FlowLocalID> localOfValue;
    Vector<ValueID> valueOfLocal;
    Vector<FlowExternalNode> externals;
    Vector<FlowSiteDescription> sites;
    Vector<FlowClosureCreation> closureCreations;
    FlowCoroutineSummary coroutine;
    std::optional<uint32_t> restSite;
    Vector<Range> ranges;
    Vector<FlowSeed> seeds;
    FlowAdjacency<FlowLocalID> successors;
    Vector<std::pair<FlowLocalID, FlowLocalID>> copiesFromExternals;
    FlowStatements statements;
    FlowStatementNumbering numbering;
    FlowAdjacency<FlowStatementReference> waiters;
    // Of the waiters of a local, those that have to run again when more comes into it.
    FlowAdjacency<FlowStatementReference> statementsToWake;

    bool hasBody() const { return numberOfLocals; }

    FlowLocalID entryParameter(uint32_t index) const
    {
        RELEASE_ASSERT(index < numberOfEntryParameters);
        return FlowLocalID(index);
    }

    FlowLocalID result(uint32_t index) const
    {
        RELEASE_ASSERT(index < numberOfResults);
        return FlowLocalID(numberOfEntryParameters + index);
    }

    FlowLocalID thrown() const
    {
        RELEASE_ASSERT(hasBody());
        return FlowLocalID(numberOfEntryParameters + numberOfResults);
    }

    bool isExternal(FlowLocalID node) const
    {
        ASSERT(node);
        return node.index() >= numberOfLocals;
    }

    const FlowExternalNode& externalAt(FlowLocalID node) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(isExternal(node));
        return externals[node.index() - numberOfLocals];
    }

    ValueID originOf(FlowLocalID node) const
    {
        return node.index() < valueOfLocal.size() ? valueOfLocal[node.index()] : ValueID();
    }

    const FlowClosureCreation* closureCreationAt(FlowLocalID node) const LIFETIME_BOUND
    {
        auto found = std::ranges::lower_bound(closureCreations, node, { }, &FlowClosureCreation::node);
        if (found == closureCreations.end() || found->node != node)
            return nullptr;
        return found;
    }
};

} } } // namespace JSC::AOT::Typed

namespace WTF {

template<> struct HashTraits<JSC::AOT::Typed::FlowExternalNode> : GenericHashTraits<JSC::AOT::Typed::FlowExternalNode> {
    using FlowExternalNode = JSC::AOT::Typed::FlowExternalNode;
    using VariableID = JSC::AOT::Typed::VariableID;

    static FlowExternalNode emptyValue() { return FlowExternalNode::variable(HashTraits<VariableID>::emptyValue()); }

    static void constructDeletedValue(FlowExternalNode& slot)
    {
        VariableID variable;
        HashTraits<VariableID>::constructDeletedValue(variable);
        slot = FlowExternalNode::variable(variable);
    }

    static bool isDeletedValue(const FlowExternalNode& node)
    {
        return node.kind() == JSC::AOT::Typed::FlowExternalKind::Variable && HashTraits<VariableID>::isDeletedValue(node.variable());
    }
};

} // namespace WTF

#endif // ENABLE(AOT)
