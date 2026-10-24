/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTSchemaWalkers.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class TailCallOutcome : uint8_t { Preserved, Demoted };

} // anonymous namespace

// See giveLayoutsToCreatedObjects(). Where the layout has got no field, what Object.create makes is made by Object.create again: the call stands where the allocation stood.
static void restoreCallsOfObjectCreate(const FlowBodyApplication& body)
{
    Function& function = body.function;
    Vector<ValueID, 4> calls;
    for (Value& link : function.editAllValues()) {
        if (link.opcode != Opcode::SetLiteralPrototype || !link.refusesOtherValues())
            continue;
        ValueID callee = link.replacedCalleeOperand();
        ValueID thisValue = link.replacedThisOperand();
        link.setReplacedCalleeOperand({ });
        link.setReplacedThisOperand({ });
        Value& allocation = function.edit(link.objectOperand());
        if (!callee || allocation.opcode != Opcode::NewObject || !body.application.layouts[allocation.layout().index()].objectsAreDictionaries)
            continue;

        Value call = IRBuilder::make(Opcode::Call);
        call.setCalleeOperand(callee);
        call.setThisValueOperand(thisValue);
        ValueID prototype = link.prototypeOperand();
        call.setArgumentsOperands(singleElementSpan(prototype));
        call.setOrigin(allocation.origin());
        call.type = TypeTable::anything;
        allocation = WTF::move(call);
        calls.append(link.objectOperand());

        Value nothing = IRBuilder::make(Opcode::Undefined);
        nothing.setOrigin(link.origin());
        nothing.type = TypeTable::undefined;
        link = WTF::move(nothing);
    }
    if (calls.isEmpty())
        return;
    // Nothing is left to finish.
    for (Value& finished : function.editAllValues()) {
        if (finished.opcode != Opcode::FinishObject || !calls.contains(finished.objectOperand()))
            continue;
        Value same = IRBuilder::make(Opcode::Upcast, { finished.objectOperand() });
        same.setCastType(TypeTable::anything);
        same.setOrigin(finished.origin());
        same.type = TypeTable::anything;
        finished = WTF::move(same);
    }
}

static void stopListingStartsThatNobodyReads(const FlowBodyApplication& body, Value& allocation)
{
    const Vector<FlowFieldUpdate>& updates = body.application.layouts[allocation.layout().index()].fields;
    auto isDropped = [&](uint32_t field) {
        return field < updates.size() && updates[field].undefinedAtStartIsNeverRead;
    };
    if (std::ranges::none_of(allocation.fields(), isDropped))
        return;
    std::span<const uint32_t> fields = allocation.fields();
    std::span<const ValueID> values = allocation.variadicOperands();
    Vector<uint32_t, 16> remainingFields;
    Vector<ValueID, 16> remainingValues;
    for (size_t index = 0; index < fields.size(); ++index) {
        if (isDropped(fields[index]))
            continue;
        remainingFields.append(fields[index]);
        remainingValues.append(values[index]);
    }
    allocation.setFields(remainingFields.span());
    allocation.setVariadicOperands(remainingValues.span());
}

void applyStamps(const FlowBodyApplication& body, const FlowBodyUpdate& update)
{
    Function& function = body.function;
    const FlowAccessSiteNumbering& accessSites = body.application.accessSites;

    forEachAccessSiteValue(function, [&](ValueID value, uint32_t ordinal) {
        RELEASE_ASSERT(ordinal < accessSites.numberOfSitesOf(body.id));
        function.edit(value).setAccessSite(accessSites.siteOf(body.id, ordinal));
    });

    for (const FlowAccessStamp& stamp : update.accesses)
        function.edit(stamp.value).setReceivers(stamp.receivers);

    for (const FlowCallStamp& stamp : update.calls)
        function.edit(stamp.value).setCallees(stamp.callees);

    for (const FlowAllocationStamp& stamp : update.allocations) {
        Value& allocation = function.edit(stamp.value);
        allocation.setAllocationSite(stamp.site);
        if (PayloadWord::allocationClass(allocation.opcode))
            allocation.setAllocationClass(stamp.allocationClass);
        if (stamp.elements)
            allocation.setContents(stamp.elements);
        if (allocation.opcode == Opcode::NewObject && body.program.at(allocation.layout()).isEnvironment())
            stopListingStartsThatNobodyReads(body, allocation);
    }

    restoreCallsOfObjectCreate(body);
    if (!body.application.hasDictionaries)
        return;
    DictionaryTypeRewriter rewriter(body.application);
    for (Value& value : function.editAllValues()) {
        TypeID passed = value.opcode == Opcode::Check ? rewriter.typeTestedBehind(value.type) : TypeID();
        TypeID tested = value.opcode == Opcode::Check ? rewriter.typeTestedBehind(value.castType()) : value.opcode == Opcode::TypeTest ? rewriter.typeTestedBehind(value.testedType()) : TypeID();
        rewriter(value.type);
        if (passed)
            value.type = passed;
        TypeID expected = rewriter.typeBehind(value.expected());
        value.setExpected(expected == value.type ? TypeID() : expected);
        forEachReference(value, rewriter);
        if (value.opcode == Opcode::Check)
            value.setCastType(tested);
        if (value.opcode == Opcode::TypeTest)
            value.setTestedType(tested);
    }
    for (Block& block : function.editAllBlocks())
        forEachReference(block.terminator, rewriter);
    BitVector dictionaries;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        Value& value = function.edit(ValueID(index));
        LayoutID layout = layoutOfEmptyLiteral(body.program, value);
        if (!layout || !body.application.layouts[layout.index()].objectsAreDictionaries)
            continue;
        if (value.opcode == Opcode::NewObject)
            dictionaries.set(index);
        Value dictionary = IRBuilder::make(Opcode::NewDictionary);
        dictionary.setOrigin(value.origin());
        dictionary.type = TypeTable::object;
        value = WTF::move(dictionary);
    }
    if (dictionaries.isEmpty())
        return;
    // Nothing is left to finish.
    for (Value& finished : function.editAllValues()) {
        if (finished.opcode != Opcode::FinishObject || !dictionaries.get(finished.objectOperand().index()))
            continue;
        Value same = IRBuilder::make(Opcode::Upcast, { finished.objectOperand() });
        same.setCastType(TypeTable::object);
        same.setOrigin(finished.origin());
        same.type = TypeTable::object;
        finished = WTF::move(same);
    }
}

static void demoteTailCall(const FlowBodyApplication& body, IRBuilder& builder, BlockID block)
{
    builder.demoteReturnCall(block);
    body.build.statistics().count(StatisticID::ReturnCallsDemoted);
}

static bool isCheck(const TailCast& cast)
{
    return cast.resolution == CastResolution::Check;
}

// A test after the call ends the tail call.
static TailCallOutcome applyTailChecks(const FlowBodyApplication& body, IRBuilder& builder, BlockID block, const FlowTailCallUpdate& entry, FlowBodyCounts& counts)
{
    Terminator& terminator = body.function.edit(block).terminator;
    Vector<TailCast, 2> casts;
    for (TailCast cast : terminator.casts())
        casts.append(cast);
    uint64_t numberOfChecks = static_cast<uint64_t>(std::ranges::count_if(casts, isCheck));
    if (!numberOfChecks)
        return TailCallOutcome::Preserved;

    if (!entry.claimsAreRedundant && !provesEverythingIn(body)) {
        demoteTailCall(body, builder, block);
        return TailCallOutcome::Demoted;
    }

    for (TailCast& cast : casts) {
        if (isCheck(cast))
            cast.resolution = CastResolution::Assume;
    }
    terminator.setCasts(casts.span());
    counts.removedChecks += numberOfChecks;
    return TailCallOutcome::Preserved;
}

static void applyTailCalls(const FlowBodyApplication& body, IRBuilder& builder, const FlowBodyUpdate& update, FlowBodyCounts& counts)
{
    Function& function = body.function;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        Terminator& terminator = function.edit(block).terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;

        // A call that flow says nothing about gets the unknown answers: past form AsRead no Check may stay on a terminator.
        ValueID call = terminator.callOperand();
        const FlowTailCallUpdate* known = findByValue(update.tailCalls.span(), call, &FlowTailCallUpdate::call);
        FlowTailCallUpdate entry = known ? *known : FlowTailCallUpdate { };
        terminator.setRequirement(entry.requirement);
        if (applyTailChecks(body, builder, block, entry, counts) == TailCallOutcome::Demoted)
            continue;

        // The constant takes the place of the result, and a tail call leaves no place behind it.
        if (findByValue(update.symbolConstants.span(), call, &FlowSymbolConstant::call))
            demoteTailCall(body, builder, block);
    }
}

static bool isTailCallWithCheck(const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::ReturnCall)
        return false;
    for (TailCast cast : terminator.casts()) {
        if (isCheck(cast))
            return true;
    }
    return false;
}

void demoteTailCallsWithChecks(const Program& program, FunctionID id, Function& function, TypeArena& types, BuildContext& build)
{
    bool hasTailCallWithCheck = std::ranges::any_of(function.blocks, isTailCallWithCheck, &Block::terminator);
    if (!hasTailCallWithCheck)
        return;

    AnalysisContext context { build, program, nullptr, nullptr, id };
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        if (!isTailCallWithCheck(function.at(block).terminator))
            continue;
        builder.demoteReturnCall(block);
        build.statistics().count(StatisticID::ReturnCallsDemoted);
    }
}

static uint64_t countChecks(const Function& function)
{
    uint64_t numberOfChecks = 0;
    for (const Block& block : function.blocks) {
        numberOfChecks += static_cast<uint64_t>(std::ranges::count_if(block.values, [&](ValueID id) {
            return function.at(id).opcode == Opcode::Check;
        }));
    }
    return numberOfChecks;
}

FlowBodyCounts applyBodyUpdate(const FlowBodyApplication& body)
{
    // The update names values by their ids as read. Typing may renumber them, so nothing after it reads the update.
    const FlowBodyUpdate& update = body.application.bodies[body.id.index()];
    applyStamps(body, update);
    FlowBodyCounts counts;
    counts.removedChecks = applyCheckUpdates(body, update);

    AnalysisContext context { body.build, body.program, nullptr, nullptr, body.id };
    FunctionAnalyses analyses(context, body.function);
    IRBuilder builder(context, body.function, body.types, &analyses);
    applyTailCalls(body, builder, update, counts);

    FlowCastInsertion insertion { body, analyses, builder };
    insertProofCasts(insertion, update);
    insertSymbolConstants(insertion, update);
    replaceDefinedValues(insertion, update);
    builder.applyInsertions();

    TypePropagationInputs inputs { body.program, body.id, body.function, analyses, body.types, body.build };
    TypePropagationOutcome typing = typeValuesFromNever(inputs);

    Vector<ValueID> casts = insertCastsForRequiredTypes(insertion);
    if (!casts.isEmpty()) {
        builder.applyInsertions();
        typing += narrowValueTypes(inputs, casts.span());
    }
    // Whoever types every value of a function that waits to have code cut looks again whether it can go.
    if (!typing.waitsToCutCode)
        body.function.setValuesAreTypedAsDefined();
    if (body.build.validates())
        insertRuntimeValidation(insertion);

    // Flow lists the checks that it proves to pass. The others are whatever is left, those of demoted tail calls among them.
    counts.remainingChecks = countChecks(body.function);
    return counts;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
