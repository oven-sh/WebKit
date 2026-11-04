/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// WHAT A MODULE MAKES AT LOAD AND ONLY STORES IN A VARIABLE IS MADE AT THE FIRST READ: DeferredVariable, plan/AOTPlan.h.
// The body that defines: the operation that makes the object and the casts behind it are built as nothing, and in the place of the store stands a store of the mark, or nothing.
// Whoever reads: the read as written . a test for the mark . in a rare block the operation as the body writes it and a store of what it gives . the two meet.
// What the read is bound to is never the mark. What the variable held in front of its declaration goes on as it is, to the check that is written behind the read.
// At a suspension: nothing. At a throw: the operation that makes the object throws for lack of memory alone, under the handler of the block being walked.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet operationsThatMakeDeferredObjects { Opcode::NewRegExp, Opcode::Data };
static constexpr OpcodeSet opcodesOfDeferredDefinitions = operationsThatMakeDeferredObjects | inputOperationsThatHandOn | OpcodeSet { Opcode::GetModuleVariable, Opcode::PutModuleVariable };

// Of a value of the body being read, behind casts.
static const DeferredVariable* variableThatValueIsMadeFor(Walker& walker, ValueID value)
{
    const DeferredDefinitions& definitions = walker.plan().deferredDefinitions();
    FunctionID body = walker.currentFunction();
    if (!value || !definitions.bodiesThatDefine.get(body.index()))
        return nullptr;
    const BodyIndex& index = walker.currentBodyIndex();
    while (!index.isParameter(value) && inputOperationsThatHandOn.contains(index.value(value).opcode()))
        value = index.value(value).operand(0);
    return index.isParameter(value) ? nullptr : definitions.findMadeBy(body, value);
}

static Reference buildMarkOfDeferredVariable(Walker& walker, const DeferredVariable& deferred)
{
    Opcode opcode = deferred.mark == Primitive::Undefined ? Opcode::Undefined : deferred.mark == Primitive::Null ? Opcode::Null : Opcode::Absent;
    Draft mark(opcode, walker.originOfOperation());
    mark.representation = Representation::JSValue;
    return walker.constant(WTF::move(mark));
}

static void buildStoreIntoDeferredVariable(Walker& walker, const DeferredVariable& deferred, Reference stored)
{
    Draft store(Opcode::PutModuleVariable, walker.originOfOperation());
    store.setVariable(deferred.variable);
    store.setValueOperand(stored);
    walker.chooseConservativeFields(store);
    walker.build(WTF::move(store));
}

static Reference makeObjectOfDeferredVariable(Walker& walker, const DeferredVariable& deferred)
{
    const BodyIndex* index = walker.facts().bodies().find(deferred.body);
    RELEASE_ASSERT(index);
    BodyValue written = index->value(deferred.made);
    RELEASE_ASSERT(operationsThatMakeDeferredObjects.contains(written.opcode()));
    // It takes no operand, so nothing of the body that writes it is named.
    written.forEachOperand([](ValueID, uint32_t) {
        RELEASE_ASSERT_NOT_REACHED();
    });
    Draft operation = walker.draftOf(written);
    operation.setOrigin(walker.originOfOperation());
    operation.inputValueGiven = { };
    operation.resultIsReturnedAtOnce = false;
    walker.chooseConservativeFields(operation);
    return walker.build(WTF::move(operation));
}

static void buildReadOfDeferredVariable(Walker& walker, const BodyValue& read, const DeferredVariable& deferred)
{
    // Only what has filled the variable, or was found in it behind the test, is noted under it. Once it is there it stays.
    if (walker.has(Mechanism::Folding)) {
        if (Reference held = walker.state().variable(deferred.variable)) {
            walker.bind(read.id(), held);
            return;
        }
    }
    if (Reference kept = objectOfVariablePutOff(walker, deferred.variable)) {
        walker.bind(read.id(), kept);
        return;
    }

    Origin origin = walker.originOfOperation();
    Draft asWritten = walker.draftOf(read);
    walker.chooseConservativeFields(asWritten);
    Reference held = walker.build(WTF::move(asWritten));
    if (!held)
        return;
    if (!(walker.types().at(walker.typeOf(held)).primitives & deferred.mark)) {
        walker.fail("a variable that its first read fills is typed as if it never held the mark"_s);
        return;
    }

    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(held);
    test.setTestedType(TypeArena::fromPrimitives(deferred.mark));
    Reference isNotMade = walker.build(WTF::move(test));
    if (!isNotMade)
        return;

    BlockID makes = walker.addBlock(BlockFrequency::Rare);
    BlockID finds = walker.addBlock();
    BlockID meet = walker.addBlock();
    Reference result = walker.addParameter(meet, { }, Representation::JSValue);
    walker.branch(isNotMade, makes, finds);

    walker.continueIn(makes);
    Reference made = makeObjectOfDeferredVariable(walker, deferred);
    if (!made)
        return;
    buildStoreIntoDeferredVariable(walker, deferred, made);
    walker.jumpTo(meet, std::array { made });

    walker.continueIn(finds);
    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(held);
    narrowed.setTestOperand(isNotMade);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(false);
    Reference found = walker.build(WTF::move(narrowed));
    if (!found)
        return;
    walker.jumpTo(meet, std::array { found });

    walker.bind(read.id(), result);
    // What the variable held in front of its declaration is ended by the store as it is written, which the effects of the body name.
    walker.state().noteVariable(deferred.variable, result);
    noteObjectOfVariablePutOff(walker, deferred.variable, result);
}

constinit const OpcodeSet opcodesThatBuildDeferredDefinitionTakes = opcodesOfDeferredDefinitions;

bool buildDeferredDefinitionOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!opcodesOfDeferredDefinitions.contains(opcode))
        return false;

    if (opcode == Opcode::GetModuleVariable) {
        const DeferredVariable* deferred = walker.plan().deferredDefinitions().findDeferred(value.variable());
        if (!deferred)
            return false;
        buildReadOfDeferredVariable(walker, value, *deferred);
        return true;
    }

    if (opcode == Opcode::PutModuleVariable) {
        const DeferredVariable* deferred = variableThatValueIsMadeFor(walker, value.valueOperand());
        if (!deferred)
            return false;
        RELEASE_ASSERT(deferred->variable == value.variable());
        if (deferred->isMarkedAtDeclaration) {
            if (Reference mark = buildMarkOfDeferredVariable(walker, *deferred))
                buildStoreIntoDeferredVariable(walker, *deferred, mark);
        }
        return true;
    }

    return !!variableThatValueIsMadeFor(walker, value.id());
}

// Link ran in front of the plan, so a variable that a read in a callee may fill is not among the writes of the callee, and no row says that the read stores.
bool isVariableFilledAtFirstRead(const Plan& plan, VariableID variable)
{
    const DeferredVariable* deferred = plan.deferredDefinitions().findDeferred(variable);
    return deferred && deferred->made;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
