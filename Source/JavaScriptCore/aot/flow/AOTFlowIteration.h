/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowCoroutines.h"
#include "IterationKind.h"
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowIterationState {
    HashMap<FlowObjectID, IterationKind> kindOfIterator;
    // By statement and state node: the built-in iterators whose own method a resumption finds, so that it reads their containers itself.
    HashMap<std::pair<FlowStatementSite, FlowNodeID>, FlowObjectSet> steppedIterators;
};

struct FlowIterationRequest {
    FlowStatementSite site;
    ValueID origin;
    IteratorKind kind { IteratorKind::Sync };
    bool isInCycle { true };
    FlowNodeID thrown;
};

struct FlowIteratorResumption {
    ResumeMode mode { ResumeMode::Next };
    FlowNodeID sent;
    FlowNodeID value;
};

void openIterators(FlowSolver&, const FlowIterationRequest&, FlowNodeID iterable, const FlowNewObjects& iterables, FlowNodeID state);
void openIteratorsOfPrimitives(FlowSolver&, const FlowIterationRequest&, uint16_t primitives, FlowNodeID state);
void openAsyncIterator(FlowSolver&, const FlowIterationRequest&, FlowNodeID iterable, FlowObjectID, FlowNodeID state);
void resumeIterators(FlowSolver&, const FlowIterationRequest&, FlowNodeID state, const FlowNewObjects& iterators, const FlowIteratorResumption&);
void stepArrayIterators(FlowSolver&, const FlowIterationRequest&, FlowNodeID state, const FlowNewObjects& iterators, const FlowIteratorResumption&);
void closeIterators(FlowSolver&, const FlowIterationRequest&, FlowNodeID state, const FlowNewObjects& iterators);

void connectIteratedValues(FlowSolver&, const FlowIterationRequest&, FlowNodeID iterable, FlowNodeID values);

enum class FlowDirectIteration : uint8_t {
    Unknown,
    BuiltinIterator,
    Generator,
};

FlowDirectIteration directIterationOf(FlowSolver&, FlowObjectID iterator);
void resumeIterator(FlowSolver&, const FlowIterationRequest&, FlowNodeID state, FlowObjectID iterator, const FlowIteratorResumption&);
void resumeDirectly(FlowSolver&, const FlowIterationRequest&, FlowNodeID state, FlowObjectID iterator, FlowDirectIteration, const FlowIteratorResumption&);
void connectContentsByKind(FlowSolver&, const FlowIterationRequest&, FlowObjectID container, IterationKind, FlowNodeID values);
FlowObjectID allocateEntriesPair(FlowSolver&, const FlowIterationRequest&, FlowObjectID container);
FlowNodeID elementTargetOf(FlowSolver&, const FlowIterationRequest&, FlowNodeID value);
void readIteratorResults(FlowSolver&, const FlowIterationRequest&, FlowNodeID results, FlowNodeID value);

void noteIteratorOf(FlowSolver&, FlowObjectID iterator, IterationKind);
void exposeBuiltinIterator(FlowSolver&, FlowObjectID, const Witness&);
std::optional<IterationKind> iterationKindOf(const FlowCallState&, FlowObjectID iterator);
PropertyKeyID methodKeyOf(const Program&, ResumeMode);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
