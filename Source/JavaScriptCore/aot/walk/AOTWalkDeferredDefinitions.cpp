/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet operationsThatMakeDeferredObjects { Opcode::NewRegExp, Opcode::Data };
static constexpr OpcodeSet opcodesOfDeferredDefinitions = operationsThatMakeDeferredObjects | inputPassThroughOpcodes | OpcodeSet { Opcode::GetModuleVariable, Opcode::PutModuleVariable };

static const DeferredVariable* variableThatValueIsMadeFor(Walker& walker, ValueID value)
{
    const DeferredDefinitions& definitions = walker.plan().deferredDefinitions();
    FunctionID body = walker.currentFunction();
    if (!value || !definitions.bodiesThatDefine.get(body.index()))
        return nullptr;
    const BodyIndex& index = walker.currentBodyIndex();
    while (!index.isParameter(value) && inputPassThroughOpcodes.contains(index.value(value).opcode()))
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

static std::optional<uint32_t> orderWordThatObjectKeepsForLife(Walker& walker, Reference made)
{
    if (!made.isValue() || walker.code().at(made.id()).opcode != Opcode::NewObject)
        return std::nullopt;
    const Operation& allocation = walker.code().at(made.id());
    LayoutID layout = allocation.layout();
    const LinkedLayout& linked = walker.facts().linked().layout(layout);
    if (linked.reachesUnseenCode || linked.isFilledUnderComputedKeys || linked.possibleChanges.containsAny({ LayoutChange::IsFrozen, LayoutChange::GainsProperty, LayoutChange::LosesProperty, LayoutChange::GetsAccessor }))
        return std::nullopt;
    const PlannedLayout& planned = walker.plan().layout(layout);
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        if (planned.slots[field].hasStorage() && !std::ranges::contains(allocation.fields(), field))
            return std::nullopt;
    }
    return walker.plan().keyOrders().findOrderWord(layout, allocation.fields());
}

static Reference findOrMakeObjectOfDeferredVariable(Walker&, const DeferredVariable&, Reference held, Origin);

static void buildReadOfDeferredVariable(Walker& walker, const BodyValue& read, const DeferredVariable& deferred)
{
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

    Draft asWritten = walker.draftOf(read);
    walker.chooseConservativeFields(asWritten);
    Reference held = walker.build(WTF::move(asWritten));
    if (!held)
        return;
    if (Reference result = findOrMakeObjectOfDeferredVariable(walker, deferred, held, walker.originOfOperation()))
        walker.bind(read.id(), result);
}

static Reference findOrMakeObjectOfDeferredVariable(Walker& walker, const DeferredVariable& deferred, Reference held, Origin origin)
{
    if (!(walker.types().at(walker.typeOf(held)).primitives & deferred.mark)) {
        walker.fail("a variable that its first read fills is typed as if it never held the mark"_s);
        return { };
    }

    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(held);
    test.setTestedType(TypeArena::fromPrimitives(deferred.mark));
    Reference isNotMade = walker.build(WTF::move(test));
    if (!isNotMade)
        return { };

    BlockID makes = walker.addBlock(BlockFrequency::Rare);
    BlockID finds = walker.addBlock();
    BlockID meet = walker.addBlock();
    Reference result = walker.addParameter(meet, { }, Representation::JSValue);
    walker.branch(isNotMade, makes, finds);

    walker.continueIn(makes);
    Reference made = makeObjectOfDeferredVariable(walker, deferred);
    if (!made)
        return { };
    std::optional<uint32_t> orderWordForLife = orderWordThatObjectKeepsForLife(walker, made);
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
        return { };
    walker.jumpTo(meet, std::array { found });

    walker.state().noteVariable(deferred.variable, result);
    if (orderWordForLife)
        walker.state().noteOrderWord(result, orderWordForLife);
    noteObjectOfVariablePutOff(walker, deferred.variable, result);
    return result;
}

bool takeObjectOfDeferredVariable(Walker& walker, VariableID variable, Reference held, Origin origin)
{
    const DeferredVariable* deferred = walker.plan().deferredDefinitions().findDeferred(variable);
    if (!deferred)
        return false;
    findOrMakeObjectOfDeferredVariable(walker, *deferred, held, origin);
    return true;
}

constinit const OpcodeSet opcodesThatBuildDeferredDefinitionTakes = opcodesOfDeferredDefinitions;
static_assert(opcodesThatBuildDeferredDefinitionTakesInChains == opcodesOfDeferredDefinitions);

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

    if (!walker.plan().deferredDefinitions().bodiesThatDefine.get(walker.currentFunction().index()))
        return false;

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

bool isVariableFilledAtFirstRead(const Plan& plan, VariableID variable)
{
    const DeferredVariable* deferred = plan.deferredDefinitions().findDeferred(variable);
    return deferred && deferred->made;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
