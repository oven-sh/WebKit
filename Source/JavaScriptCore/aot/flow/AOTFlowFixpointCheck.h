/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowSolver.h"
#include <memory>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// Runs after the queues are empty. Every cursor goes back to its start, nothing counts as done, and every rule of the solve runs once more over the solved sets.
// At a fixpoint that adds nothing. Whatever it adds is an update that the solve lost, whichever shortcut lost it.
class FlowSolver::FixpointCheck {
    WTF_MAKE_NONCOPYABLE(FixpointCheck);
public:
    explicit FixpointCheck(FlowSolver&);
    ~FixpointCheck();

    Vector<FlowFixpointChange> run();

    void willInitialize(FlowInstanceID);
    void willRun(FlowStatementSite);
    void willProcess(FlowNodeID);
    void didChangeValue(FlowNodeID, const FlowObjectSet& addedObjects, bool hasNewPrimitives);
    void didAddEdge(FlowNodeID from, FlowNodeID to);
    void didAddMutations(FlowObjectID, OptionSet<ObjectMutation>);
    void didAddUse(FlowObjectID, FlowObjectUse);
    void didAddEvent(FlowObjectID, FlowObjectEvent);
    void didAddLocationEvents(FlowNodeID, OptionSet<FlowLocationEvent>);
    void didAddDirectUses(FlowNodeID, OperandUses);

private:
    enum class Rule : uint8_t {
        None,
        Roots,
        UnseenCode,
        OtherNewTarget,
        BuiltinUse,
        ModuleEvaluation,
        ArrivalInNativeCode,
        ArrivalInUnknownJavaScript,
        LocationCreation,
        UnnamedKeyStores,
        MovedElements,
        Seeds,
        Statement,
        AbandonedCall,
        AbandonedInstance,
        Edges,
        Refinement,
    };

    struct Step {
        Rule rule { Rule::None };
        FlowStatementSite site;
        FlowNodeID node;
        FlowObjectID object;
        uint32_t index { 0 };
    };

    struct Sizes {
        unsigned nodes { 0 };
        unsigned edges { 0 };
        unsigned instances { 0 };
        unsigned objects { 0 };
        size_t builtinCalls { 0 };
        size_t linkedRequests { 0 };
        size_t callLinks { 0 };
        size_t operandFlows { 0 };

        friend bool operator==(const Sizes&, const Sizes&) = default;
    };

    struct Progress;

    Sizes sizes() const;
    void begin(const Step&);
    void finishStep();
    void addChange(FlowNodeID, const String& whatIsGained);
    String describeStep() const;
    String nameOf(FlowNodeID) const;
    String nameOf(FlowObjectID) const;
    ValueID originOf(FlowStatementSite) const;

    void forgetProgress();
    void compareAndRestoreProgress();
    void repeatEveryRule();

    void repeatRoots();
    void repeatUnseenCode();
    void repeatBuiltinUses();
    void repeatModuleEvaluations();
    void repeatArrivalsInUnseenCode();
    void repeatLocationCreations();
    void repeatUnnamedKeyStores();
    void repeatMovedElements();
    void repeatInstances();
    void repeatBuiltinCalls();
    void checkAbandonedInstance(FlowInstanceID);
    void repeatEdges();
    void repeatRefinements();
    void reportWaitsOfNativeCode();

    FlowSolver& m_solver;
    std::unique_ptr<Progress> m_progress;
    Step m_step;
    Sizes m_sizes;
    Vector<FlowFixpointChange> m_changes;
    size_t m_numberOfChanges { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
