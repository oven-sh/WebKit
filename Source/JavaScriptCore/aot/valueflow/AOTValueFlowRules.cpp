/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRules.h"

#if ENABLE(AOT)

#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowRulesModules.h"

#include "AOTValueFlowRulesHeap.h"

namespace JSC { namespace AOT { namespace Typed {

ValueSetID unknownValue(ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    return context.maker().unite(universe.anyPrimitive(), universe.unknownObjectAlone());
}

// Native code may read whatever an escaped object holds, call whatever it can reach with whatever it can reach, and keep what it gets.
void runEscape(const ValueFlowStatement&, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    ValueFlowNodeID escapedNode = ValueFlowProgramNodes::escaped();
    ClimbingSet escaped = context.valueOf(escapedNode);

    for (ValueFlowNameKind kind : { ValueFlowNameKind::EveryProperty, ValueFlowNameKind::EveryGetter, ValueFlowNameKind::EverySetter, ValueFlowNameKind::PrototypeLink, ValueFlowNameKind::CopiedFrom })
        context.connect(context.read(escaped, { kind, 0 }), escapedNode);
    context.add(escapedNode, objectsHeldFromCreation(context, escaped));
    context.mark(escaped, ObjectMutation::ReachNativeCode);
    if (areEscapedObjectsOpen(context))
        letAnythingHappenTo(context, escaped);

    letNativeCodeRunProtocols(context, escaped);

    context.forEachMember(context.restrict(escaped, universe.closures()), [&](ValueFlowObjectID closure) {
        enterFromNativeCode(context, universe.closureFunction(closure));
    });
}

// Every operation that has a rule of its own by its opcode. The bodies make a statement for these, for the operations of modules and for those of the heap, and for no other.
#define FOR_EACH_OPERATION_WITH_RULE(v) \
    v(Call, runCall) \
    v(CallList, runCall) \
    v(Construct, runCall) \
    v(ConstructList, runCall) \
    v(SuperConstruct, runCall) \
    v(SuperConstructList, runCall) \
    v(NewClass, runNewClass) \
    v(DefineMethod, runDefineMethod) \
    v(ResolveHeritage, runReadOfPrototypeProperty) \
    v(GetClassPrototype, runReadOfPrototypeProperty) \
    v(GetConstructPrototype, runReadOfPrototypeProperty) \
    v(IteratorOpen, runIterationOperation) \
    v(IteratorResume, runIterationOperation) \
    v(IteratorClose, runIterationOperation) \
    v(AppendSpread, runIterationOperation) \
    v(StepArrayIterator, runStepOfLibrary) \
    v(MapNext, runStepOfLibrary) \
    v(SetNext, runStepOfLibrary) \
    v(ResumeGenerator, runGeneratorOperation) \
    v(StepGenerator, runGeneratorOperation) \
    v(Await, runAwait) \
    v(CreatePromise, runPromiseOperation) \
    v(ResolvePromise, runPromiseOperation) \
    v(RejectPromise, runPromiseOperation) \
    v(InstanceOf, runInstanceOf)

bool hasRuleOfItsOwn(Opcode opcode)
{
    switch (opcode) {
#define RETURN_TRUE(opcode, rule) case Opcode::opcode:
    FOR_EACH_OPERATION_WITH_RULE(RETURN_TRUE)
#undef RETURN_TRUE
        return true;
    default:
        return isModuleOperation(opcode) || isHeapOperation(opcode);
    }
}

static void runOperation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    switch (statement.opcode) {
#define RUN_RULE(opcode, rule) \
    case Opcode::opcode: \
        rule(statement, context); \
        return;
    FOR_EACH_OPERATION_WITH_RULE(RUN_RULE)
#undef RUN_RULE
    default:
        if (isModuleOperation(statement.opcode))
            runModuleOperation(statement, context);
        else {
            RELEASE_ASSERT(isHeapOperation(statement.opcode));
            runHeapOperation(statement, context);
        }
        return;
    }
}

void runValueFlowRule(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    switch (statement.kind) {
    case ValueFlowStatementKind::Operation:
        runOperation(statement, context);
        return;
    case ValueFlowStatementKind::CallGroup:
        runCallGroup(statement, context);
        return;
    case ValueFlowStatementKind::ModuleEvaluation:
        runModuleEvaluation(statement, context);
        return;
    case ValueFlowStatementKind::Coroutine:
        runCoroutine(statement, context);
        return;
    case ValueFlowStatementKind::HostRoots:
        runHostRoots(statement, context);
        return;
    case ValueFlowStatementKind::UsedBuiltins:
        runUsedBuiltins(statement, context);
        return;
    case ValueFlowStatementKind::UnseenCode:
        runUnseenCode(statement, context);
        return;
    case ValueFlowStatementKind::ModulesLoadedByName:
        runModulesLoadedByName(statement, context);
        return;
    case ValueFlowStatementKind::Escape:
        runEscape(statement, context);
        return;
    case ValueFlowStatementKind::Lookup:
        switch (context.nameOfKey().kind) {
        case ValueFlowNameKind::OpenedIterators:
            runOpenedIterators(statement, context);
            return;
        case ValueFlowNameKind::IteratedValues:
            runIteratedValues(statement, context);
            return;
        case ValueFlowNameKind::IteratorResults:
            runIteratorResults(statement, context);
            return;
        case ValueFlowNameKind::AwaitedValue:
            runAwaitedValue(statement, context);
            return;
        default:
            runLookup(statement, context);
            return;
        }
    case ValueFlowStatementKind::Behavior:
        runBehavior(statement, context);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
