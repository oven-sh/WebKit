/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTFlowSolution.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class FlowShapeState : uint8_t {
    Unknown,
    KeepsShape,
    NeverHasShape,
};

struct FlowShapeResult {
    FlowShapeState state { FlowShapeState::Unknown };
    FlowSiteLoss loss;
    OptionSet<PristineAspects> pristine;
};

struct FlowContentsResult {
    Vector<TypeID, 2> contents;
    Vector<TypeID> predictedContents;
    ArrayStorage predictedStorage { ArrayStorage::Other };
    ArrayStorage provenStorage { ArrayStorage::Other };
    bool isProven { false };
};

struct FlowContentsResults {
    Vector<FlowContentsResult> byObject;
    // What the arrays of a site hold, whichever objects stand for them. No type where no storage is predicted.
    Vector<TypeID> elementsBySite;
};

struct FlowConversionTables {
    std::span<const FlowShapeResult> shapesByAllocationClass;
    std::span<const FlowContentsResult> contentsByObject;
    std::span<const PropertyKeyID> symbolsCreatedOnce;
    std::span<const FunctionID> boundFunctionByObject;
    std::span<const TypeID> elementsBySite;
    std::span<const LayoutFlow> layouts;
};

struct FlowPositionType {
    Type type;
    std::optional<Type> prediction;
    FlowSiteLoss loss;
    LayoutID lossLayout;
    bool isEmpty { true };

    bool hasLoss() const { return loss.cause != UnprovenCause::Unknown; }
};

struct FlowConversionSwitches {
    bool narrowedTypes { false };
    bool removedNullish { false };
    bool literalValues { false };
    bool integerRanges { false };
    bool wholeNumbers { false };
    bool exactFunctions { false };
    bool provenLayoutSets { false };
    bool provenContents { false };
    bool arrayStorageKinds { false };
    bool pristineInstances { false };
    bool initialization { false };
    bool predictions { false };

    static FlowConversionSwitches of(const BuildContext&, uint32_t subject);
    static constexpr FlowConversionSwitches allProofs();
};

constexpr FlowConversionSwitches FlowConversionSwitches::allProofs()
{
    return {
        .narrowedTypes = true,
        .removedNullish = true,
        .literalValues = true,
        .integerRanges = true,
        .wholeNumbers = true,
        .exactFunctions = true,
        .provenLayoutSets = true,
        .provenContents = true,
        .arrayStorageKinds = true,
        .pristineInstances = true,
        .initialization = true,
        .predictions = false,
    };
}

enum class TypeInclusion : uint8_t {
    Unknown,
    Inside,
};

class FlowTypeConverter {
    WTF_MAKE_NONCOPYABLE(FlowTypeConverter);
public:
    FlowTypeConverter(const FlowSolution&, const FlowConversionTables&);

    const FlowSolution& solution() const { return m_solution; }
    const FlowConversionTables& tables() const { return m_tables; }

    FlowPositionType convert(std::span<const FlowNodeID>, const Type& asRead, uint32_t switchSubject, TypeArena&) const;
    FlowPositionType convert(FlowNodeID, const Type& asRead, uint32_t switchSubject, TypeArena&) const;
    FlowPositionType convertWith(std::span<const FlowNodeID>, const Type& asRead, const FlowConversionSwitches&, TypeArena&) const;

    TypeInclusion isInside(std::span<const FlowNodeID>, const Type& target, TypeArena&) const;

    // With a loss, what is seen to arrive is not all that arrives: it refines what the checker expects of the position. It stands alone where nobody makes what the checker expects.
    std::optional<Type> refinePrediction(TypeID expectedByChecker, const FlowPositionType&, TypeArena&) const;

    LayoutSet receiversOf(std::span<const FlowNodeID>) const;
    FlowSiteLoss lossOfObjects(std::span<const FlowNodeID>, LayoutID& lossLayout) const;

    FlowShapeResult shapeOf(FlowObjectID) const;
    const FlowContentsResult& contentsOf(FlowObjectID) const LIFETIME_BOUND;
    TypeID elementsAllocatedAt(AllocationSiteID) const;
    // The function whose closure the application creates in place of the object.
    FunctionID boundFunctionOf(FlowObjectID) const;

private:
    bool isEverMade(const Type&) const;
    Type withMembers(const Type&, TypeArena&) const;
    // The contents of a container that has escaped list what was stored into it and into any other, so the rule holds for them as for the position.
    Type narrowExpectation(const Type& expected, const Type& seen, TypeArena&) const;

    const FlowSolution& m_solution;
    FlowConversionTables m_tables;
    FlowContentsResult m_unknownContents;
};

ArrayStorage predictedArrayStorageOf(const Type& elements);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
