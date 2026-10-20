/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowCallOperations.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowPropertyState.h"
#include "AOTFlowPrototypes.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

enum class FlowPropertyRole : uint16_t {
    CopySources,
    CopyTargets,
    CopiedGetterResult,
    CopiedCharacters,
    CopiedUnknownValue,
    DescriptorObjects,
    DescriptorValue,
    DescriptorGetter,
    DescriptorSetter,
    DescriptorWritable,
    DescriptorEnumerable,
    DescriptorConfigurable,
    DescriptorOfMapKey,
    DescriptorOfUnnamedMapKeys,
    BuiltinAccessorValue,
};

inline FlowTemporaryKey propertyTemporaryKey(FlowStatementSite site, FlowPropertyRole role, uint32_t index = 0)
{
    return { site, FlowTemporaryOwner::Properties, static_cast<uint16_t>(role), index };
}

// What an unknown accessor takes from an access whatever the receiver is, passed once for all receivers.
struct FlowPassedToUnknownAccessor {
    bool operands { false };
    bool usesOfReceiverNode { false };
};

struct FlowAccessContext {
    FlowSolver& solver;
    const FlowAccess& access;
    const FlowKeySet& keys;
    Witness witness;
    mutable FlowPassedToUnknownAccessor passedToNativeCode { };
    mutable FlowPassedToUnknownAccessor passedToUnknownJavaScript { };
    mutable bool hasLoadedUnknownValue { false };
    // The object that has the stored value as a property of its own once the operation has completed with the receiver at hand.
    mutable FlowObjectID holderOfWrittenMember { };
    // The receiver at hand comes out again, and the store is judged then.
    mutable bool storeWaitsForQuietQueues { false };
};

FlowWalkOperation walkOperationOf(FlowAccessKind);
FlowPropertyWalk walkOf(const FlowAccess&);
OperandUses receiverUsesOf(FlowAccessKind, const FlowKeySet&);
void noteDeclaredOperandUses(FlowSolver&, FlowStatementSite, ValueID origin);

void loadProperties(const FlowAccessContext&, FlowObjectID receiver);
void loadProperties(const FlowAccessContext&, FlowPrimitiveReceiver);
void testProperties(const FlowAccessContext&, FlowObjectID receiver);
void testProperties(const FlowAccessContext&, FlowPrimitiveReceiver);

void storeProperties(const FlowAccessContext&, FlowObjectID receiver);
void storeProperties(const FlowAccessContext&, FlowPrimitiveReceiver);
void defineProperties(const FlowAccessContext&, FlowObjectID receiver);
void defineAccessors(const FlowAccessContext&, FlowObjectID receiver);
void deleteProperties(const FlowAccessContext&, FlowObjectID receiver);

void loadFromSource(const FlowAccessContext&, FlowObjectID receiver, const FlowPropertySource&);
void joinBuiltinMember(const FlowAccessContext&, const FlowPropertySource&);
void callAccessor(const FlowAccessContext&, FlowObjectID receiver, const FlowPropertySource&);
void passToUnknownAccessor(const FlowAccessContext&, FlowObjectID receiver, const FlowPropertySource&);
void storeIntoTarget(const FlowAccessContext&, const FlowPropertySource& target);

struct FlowLocationStore {
    FlowStatementSite site;
    ValueID origin;
    FlowNodeID value;
    FlowNodeID location;
    bool isInCycle { false };
    FlowStoreNaming naming { FlowStoreNaming::Unknown };
};

enum class FlowStoredType : uint8_t {
    MayBeOutside,
    Inside,
};

void noteStore(FlowSolver&, const FlowLocationStore&);
FlowStoredType checkStoredType(FlowSolver&, const FlowLocationStore&);

std::optional<FlowKeySet> certainlyExcludedKeys(FlowSolver&, FlowStatementSite, const FlowPropertyCopy&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
