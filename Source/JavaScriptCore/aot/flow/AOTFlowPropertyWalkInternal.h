/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowPropertyState.h"
#include "AOTFlowPrototypes.h"
#include <array>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowPrimitiveWrapper {
    uint16_t primitives { 0 };
    BuiltinClassID builtinClass;
};

std::array<FlowPrimitiveWrapper, 5> primitiveWrappers();

bool isCalledByNativeCode(const FlowWalkInputs&, BuiltinID member);

constexpr bool followsPrototypes(FlowWalkOperation operation)
{
    switch (operation) {
    case FlowWalkOperation::Get:
    case FlowWalkOperation::Set:
    case FlowWalkOperation::HasProperty:
        return true;
    case FlowWalkOperation::DefineOwnProperty:
    case FlowWalkOperation::CopyDataProperties:
    case FlowWalkOperation::Delete:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr bool ensuresOwnLocations(FlowWalkOperation operation)
{
    switch (operation) {
    case FlowWalkOperation::Set:
    case FlowWalkOperation::DefineOwnProperty:
        return true;
    case FlowWalkOperation::Get:
    case FlowWalkOperation::HasProperty:
    case FlowWalkOperation::CopyDataProperties:
    case FlowWalkOperation::Delete:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// A definition and a deletion act on the locations of the receiver alone.
constexpr bool isAnsweredAtOpenedHolders(FlowWalkOperation operation)
{
    switch (operation) {
    case FlowWalkOperation::Get:
    case FlowWalkOperation::Set:
    case FlowWalkOperation::HasProperty:
    case FlowWalkOperation::CopyDataProperties:
        return true;
    case FlowWalkOperation::DefineOwnProperty:
    case FlowWalkOperation::Delete:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

class FlowPropertyWalker {
    WTF_MAKE_NONCOPYABLE(FlowPropertyWalker);
public:
    FlowPropertyWalker(const FlowWalkInputs&, const FlowKeySet&, const FlowPropertyWalk&, const FlowPropertySourceFunctor&);

    void walkFrom(FlowObjectID receiver);
    void walkFrom(FlowPrimitiveReceiver);

    bool readsUnnamedKeysOfUnknownObject() const { return m_readsUnnamedKeysOfUnknownObject; }
    bool passesHolderWithoutName() const { return m_passesHolderWithoutName; }
    std::span<const FlowPropertySource> passedMembers() const LIFETIME_BOUND { return m_passedMembers.span(); }
    size_t numberOfHolders() const { return m_pending.size(); }

    void forEachVisitedObject(NOESCAPE const Invocable<void(FlowObjectID)> auto& functor) const
    {
        for (const PendingHolder& pending : m_pending) {
            if (pending.holder.object)
                functor(pending.holder.object);
        }
    }

private:
    struct PendingHolder {
        FlowHolder holder;
        FlowCertainty certainty { FlowCertainty::Predicted };
    };

    bool isReceiver(const FlowHolder& holder) const { return holder.object && holder.object == m_receiver; }
    bool reportsTargetsOn(const FlowHolder& holder) const { return ensuresOwnLocations(m_walk.operation) && isReceiver(holder); }

    static constexpr size_t maxScannedHolders = 8;

    bool isEnqueued(const FlowHolder&) const;
    void addToVisitedSets(const FlowHolder&);
    void enqueue(const FlowHolder&, FlowCertainty);
    void drain();
    void visit(const PendingHolder&);
    void visitUnknownObject();
    bool isOpenedByUnseenJavaScript(const FlowHolder&) const;
    bool isReachedByUnseenCode(const FlowHolder&) const;
    void visitOpenedHolder(const FlowHolder&);
    void enqueuePrototypesOf(const FlowHolder&, FlowCertainty);
    void registerWaits(const FlowHolder&);
    FlowCertainty certaintyAt(const FlowHolder&, FlowCertainty incoming) const;
    void reportAbsentIfChainEnds();
    bool isConstantMember(const FlowHolder&, const FlowOwnProperty&) const;
    bool isClosedAbsentName(const FlowHolder&, const FlowOwnProperty&) const;
    bool resolvesNameElsewhere(const FlowHolder&, PropertyKeyID) const;
    void noteWriteUnderName(const FlowHolder&, PropertyKeyID) const;
    bool findsOnlyConstantMembers(const FlowHolder&) const;

    void reportOwnSources(const FlowHolder&, FlowCertainty);
    void reportNamedKey(const FlowHolder&, PropertyKeyID, FlowCertainty);
    void reportEveryNamedProperty(const FlowHolder&, FlowCertainty);
    void reportIndexes(const FlowHolder&, FlowCertainty);
    void reportIndexExports(const FlowHolder&, FlowCertainty);
    void reportUnnamedKeyPart(const FlowHolder&, FlowOpenPart, FlowCertainty);
    void reportUndeclaredPart(const FlowHolder&);
    void reportAliasOfNamedKey(const FlowHolder&, PropertyKeyID, const FlowOwnProperty& own, FlowCertainty);
    void reportProperty(const FlowHolder&, const FlowOwnProperty&, FlowCertainty);
    enum class LookupPath : uint8_t {
        PrototypeChain,
        UnknownObject,
    };

    void reportBuiltinMember(const FlowHolder&, const FlowOwnProperty&, FlowCertainty, LookupPath = LookupPath::PrototypeChain);
    void reportMembersOfBuiltinObjects();
    void reportMembersFoundByName(PropertyKeyID, std::span<const FlowMemberOfBuiltin>);
    void reportSettersUnderUnnamedKeys();
    void reportNode(FlowPropertySourceKind, const FlowHolder&, PropertyKeyID, FlowNodeID, FlowCertainty);
    void notePassedMember(FlowPropertySourceKind, const FlowHolder&, PropertyKeyID, FlowNodeID, FlowCertainty);
    void reportUnknown(const FlowHolder&);

    const FlowWalkInputs& m_inputs;
    const FlowKeySet& m_keys;
    const FlowPropertyWalk& m_walk;
    const FlowPropertySourceFunctor& m_functor;
    FlowObjectID m_receiver;
    Vector<PendingHolder, maxScannedHolders> m_pending;
    Vector<FlowPropertySource, 1> m_passedMembers;
    size_t m_numberOfForgottenHolders { 0 };
    FlowObjectSet m_visitedObjects;
    BitVector m_visitedBuiltins;
    bool m_chainMayEnd { false };
    FlowCertainty m_certaintyOfAbsence { FlowCertainty::Proven };
    bool m_receiverShowsEveryKey { false };
    bool m_lookupEndsAtHolder { false };
    bool m_holderIsImmutable { false };
    bool m_holderShowsConstantMembers { false };
    bool m_holderHasClosedAbsentName { false };
    // Unseen JavaScript defines accessors on what it holds, and a lookup that passes such a holder runs them.
    bool m_holderMayHaveUnseenAccessor { false };
    bool m_unknownSourceAnswersForHolder { false };
    bool m_hasReportedUnknown { false };
    bool m_hasReportedMembersOfBuiltinObjects { false };
    bool m_waitsOnNamesOfUnknownObject { false };
    bool m_readsUnnamedKeysOfUnknownObject { false };
    bool m_passesHolderWithoutName { false };
    bool m_hasFoundOnlyName { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
