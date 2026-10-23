/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTPass.h"
#include "AOTVector.h"
#include "AOTVirtualObjectShapes.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class AllocationSinking;
class FunctionAnalyses;
class IRBuilder;
class PassRunner;

struct FreshResultField {
    VirtualField field;
    uint32_t part { 0 };

    friend bool operator==(const FreshResultField&, const FreshResultField&) = default;
};

struct FreshResultVariant {
    VirtualShapeKind kind { VirtualShapeKind::Cell };
    VirtualConstant constant { VirtualConstant::Undefined };
    VirtualObjectDescription description;
    Value prototype;
    uint32_t length { 0 };
    Vector<FreshResultField, 4> properties;
    Vector<FreshResultField, 1> parts;
    // Of the arm of the wrapper that builds the variant.
    BlockFrequency frequency { BlockFrequency::Normal };

    bool buildsSameValueAs(const FreshResultVariant&) const;
};

enum class FreshResultsState : uint8_t { Unknown, NotFresh, Fresh };

struct FreshResultShape {
    FreshResultsState state { FreshResultsState::Unknown };
    Vector<FreshResultVariant, 2> variants;
    Vector<VirtualField, 4> fieldOfPart;
    Vector<TypeID, 4> partTypes;
    std::optional<uint32_t> cellPart;

    bool isFresh() const { return state == FreshResultsState::Fresh; }
    bool hasTag() const { return variants.size() > 1; }
    uint32_t numberOfResults() const { return hasTag() + partTypes.size(); }
    uint32_t resultOfPart(uint32_t part) const { return hasTag() + part; }
    int64_t tagOf(uint32_t variant) const { return int64_t(1) << variant; }

    void dump(PrintStream&) const;
};

bool canReturnParts(const Function&);

FreshResultShape computeFreshResults(FunctionAnalyses&);
// Also for a function that cannot return parts: a caller that holds a copy of the body can leave the aggregate unbuilt.
bool returnsFreshAggregate(FunctionAnalyses&);
enum class ObjectsThatBecomeOneCell : uint8_t { StayWhole, AreFreshLikeOthers };
FreshResultShape shapeOfReturns(const AllocationSinking&, ObjectsThatBecomeOneCell = ObjectsThatBecomeOneCell::StayWhole);
FreshResultVariant variantOfShape(const AllocationSinking&, VirtualObjectID, const VirtualShape&);

struct ResultWorkerRequest {
    FunctionID origin;
    const Function& body;
};

struct ResultWorker {
    Function function;
    FreshResultShape shape;
};

ResultWorker createResultWorker(const AnalysisContext&, const ResultWorkerRequest&, TypeArena&);
Function createResultWrapper(const AnalysisContext&, FunctionID worker, const Function& original, const FreshResultShape&, TypeArena&);
ValueID appendRebuiltResult(IRBuilder&, const FreshResultShape&, ValueID workerCall);

void splitFreshResults(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
