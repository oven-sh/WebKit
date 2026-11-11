/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestHarness.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTFamilies.h"
#include "AOTIRPrinter.h"
#include "AOTLinkFlow.h"
#include "AOTLinkTables.h"
#include "AOTRemovableFeature.h"
#include "AOTThinSummary.h"
#include "VM.h"
#include "YarrFlags.h"
#include <wtf/CommaPrinter.h>
#include <wtf/HashMap.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

struct TraitInFactsText {
    ASCIILiteral name;
    bool (EffectTraits::*holds)() const;
};

static constexpr TraitInFactsText traitsInFactsText[] = {
    { "mayThrow"_s, &EffectTraits::mayThrow },
    { "mayThrowByRule"_s, &EffectTraits::mayThrowByRule },
    { "mayCollect"_s, &EffectTraits::mayCollect },
    { "mayCallUnknownCode"_s, &EffectTraits::mayCallUnknownCode },
    { "maySuspend"_s, &EffectTraits::maySuspend },
    { "mayBeTerminated"_s, &EffectTraits::mayBeTerminated },
    { "mayWriteObservableState"_s, &EffectTraits::mayWriteObservableState },
    { "mayTouchMemory"_s, &EffectTraits::mayTouchMemory },
    { "mayWriteOutsideObjects"_s, &EffectTraits::mayWriteOutsideObjects },
};

static void printEffectsInFactsText(PrintStream& out, const Effects& effects)
{
    if (effects == Effects { }) {
        out.print("anything"_s);
        return;
    }
    if (effects.isPure()) {
        out.print("none"_s);
        return;
    }
    EffectTraits traits(effects);
    out.print("["_s);
    CommaPrinter comma;
    for (const TraitInFactsText& trait : traitsInFactsText) {
        if ((traits.*trait.holds)())
            out.print(comma, trait.name);
    }
    out.print("]"_s);
}

static void printFieldsWrittenInFactsText(PrintStream& out, ASCIILiteral name, const Effects& effects)
{
    uint64_t places = effects.writes.maskOf(AbstractHeap::ObjectFields);
    if (!places || effects.mayCallUnknownCode)
        return;
    out.print(" "_s, name, "="_s);
    if (places == LocationMasks::entireHeap)
        out.print("all"_s);
    else
        out.print(std::popcount(places));
}

static void printLayoutsWrittenInFactsText(PrintStream& out, ASCIILiteral name, uint64_t layouts)
{
    out.print(" "_s, name, "="_s);
    if (!layouts)
        out.print("none"_s);
    else if (layouts == objectsOfAnyLayout)
        out.print("any"_s);
    else
        out.print(std::popcount(layouts));
}

static void printEntryParametersInFactsText(PrintStream& out, ASCIILiteral name, uint32_t parameters)
{
    if (!parameters)
        return;
    out.print(" "_s, name, "=["_s);
    CommaPrinter comma;
    for (uint32_t parameter = 0; parameter < 32; ++parameter) {
        if (parameters >> parameter & 1)
            out.print(comma, parameter);
    }
    out.print("]"_s);
}

static ASCIILiteral nameInFactsText(SummaryNode::Kind kind)
{
    switch (kind) {
    case SummaryNode::Kind::Unknown:
        return "Unknown"_s;
    case SummaryNode::Kind::Parameter:
        return "Parameter"_s;
    case SummaryNode::Kind::Result:
        return "Result"_s;
    case SummaryNode::Kind::Thrown:
        return "Thrown"_s;
    case SummaryNode::Kind::Capture:
        return "Capture"_s;
    case SummaryNode::Kind::Variable:
        return "Variable"_s;
    case SummaryNode::Kind::Field:
        return "Field"_s;
    case SummaryNode::Kind::Member:
        return "Member"_s;
    case SummaryNode::Kind::Contents:
        return "Contents"_s;
    case SummaryNode::Kind::Constant:
        return "Constant"_s;
    case SummaryNode::Kind::Allocation:
        return "Allocation"_s;
    case SummaryNode::Kind::ClassInstance:
        return "ClassInstance"_s;
    case SummaryNode::Kind::Closure:
        return "Closure"_s;
    case SummaryNode::Kind::Builtin:
        return "Builtin"_s;
    case SummaryNode::Kind::Join:
        return "Join"_s;
    case SummaryNode::Kind::ResultOfCall:
        return "ResultOfCall"_s;
    case SummaryNode::Kind::ResultOfAccess:
        return "ResultOfAccess"_s;
    case SummaryNode::Kind::ResultOfOperator:
        return "ResultOfOperator"_s;
    case SummaryNode::Kind::ResultOfConversion:
        return "ResultOfConversion"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameInFactsText(SummaryAccess::Kind kind)
{
    switch (kind) {
    case SummaryAccess::Kind::Read:
        return "Read"_s;
    case SummaryAccess::Kind::Store:
        return "Store"_s;
    case SummaryAccess::Kind::Define:
        return "Define"_s;
    case SummaryAccess::Kind::Has:
        return "Has"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameInFactsText(SummaryChange::Kind kind)
{
    switch (kind) {
    case SummaryChange::Kind::Freezes:
        return "Freezes"_s;
    case SummaryChange::Kind::Deletes:
        return "Deletes"_s;
    case SummaryChange::Kind::DefinesByDescriptor:
        return "DefinesByDescriptor"_s;
    case SummaryChange::Kind::ChangesPrototype:
        return "ChangesPrototype"_s;
    case SummaryChange::Kind::BecomesPrototype:
        return "BecomesPrototype"_s;
    case SummaryChange::Kind::CopiesInto:
        return "CopiesInto"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameInFactsText(SummaryConstruction::Object object)
{
    switch (object) {
    case SummaryConstruction::Object::Allocated:
        return "Allocated"_s;
    case SummaryConstruction::Object::AnsweredBySuperCall:
        return "AnsweredBySuperCall"_s;
    case SummaryConstruction::Object::This:
        return "This"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameInFactsText(SummaryTrait trait)
{
    switch (trait) {
    case SummaryTrait::HasLoops:
        return "HasLoops"_s;
    case SummaryTrait::Suspends:
        return "Suspends"_s;
    case SummaryTrait::UsesArguments:
        return "UsesArguments"_s;
    case SummaryTrait::HasDirectEval:
        return "HasDirectEval"_s;
    case SummaryTrait::ListsKeys:
        return "ListsKeys"_s;
    case SummaryTrait::MayThrow:
        return "MayThrow"_s;
    case SummaryTrait::ReadsUnderComputedKey:
        return "ReadsUnderComputedKey"_s;
    case SummaryTrait::StoresUnderComputedKey:
        return "StoresUnderComputedKey"_s;
    case SummaryTrait::OnlyPassesOnGatheredArguments:
        return "OnlyPassesOnGatheredArguments"_s;
    case SummaryTrait::HoldsInt32Promise:
        return "HoldsInt32Promise"_s;
    case SummaryTrait::ReturnsUInt32:
        return "ReturnsUInt32"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral nameInFactsText(LayoutChange change)
{
    switch (change) {
    case LayoutChange::IsFrozen:
        return "IsFrozen"_s;
    case LayoutChange::GainsProperty:
        return "GainsProperty"_s;
    case LayoutChange::LosesProperty:
        return "LosesProperty"_s;
    case LayoutChange::ChangesPrototype:
        return "ChangesPrototype"_s;
    case LayoutChange::GetsAccessor:
        return "GetsAccessor"_s;
    case LayoutChange::GetsObjectAbove:
        return "GetsObjectAbove"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Flag>
static void printFlagsInFactsText(PrintStream& out, OptionSet<Flag> flags)
{
    out.print("["_s);
    CommaPrinter comma;
    for (Flag flag : flags)
        out.print(comma, nameInFactsText(flag));
    out.print("]"_s);
}

static void printFieldInFactsText(IRPrinter& printer, LayoutID layout, uint32_t field)
{
    printer.print(layout);
    printer.out().print("."_s, field);
    const Program& program = printer.program();
    if (layout.index() >= program.layouts.size() || field >= program.at(layout).fields.size() || !program.at(layout).fields[field].key)
        return;
    printer.out().print(" "_s);
    printer.print(program.at(layout).fields[field].key);
}

static void printMemberInFactsText(IRPrinter& printer, LayoutID classLayout, ClassSide side, uint32_t index)
{
    printer.print(classLayout);
    printer.out().print(" "_s);
    printer.print(side);
    printer.out().print(" "_s, index);
}

static void printKindAndKeyOfMemberInFactsText(IRPrinter& printer, MethodKind kind, PropertyKeyID key)
{
    printer.out().print(" "_s);
    printer.print(kind);
    if (!key) {
        printer.out().print(" [computed]"_s);
        return;
    }
    printer.out().print(" "_s);
    printer.print(key);
}

static void printSlotOfContents(IRPrinter& printer, SlotOfContents slot)
{
    PrintStream& out = printer.out();
    if (std::optional<ContentsSlot> listed = slot.contentsSlot())
        printer.print(*listed);
    else if (std::optional<uint32_t> field = slot.indexOfInternalField())
        out.print("field "_s, *field);
    else if (slot == SlotOfContents::returnedAlone())
        out.print("returned"_s);
    else if (slot.isUnderName())
        printer.print(slot.key());
    else
        out.print("state "_s, slot.number());
}

static void printNodeOfSummary(IRPrinter& printer, const ThinSummary& summary, const SummaryNode& node)
{
    static constexpr uint32_t markOfStaticSide = 1u << 31;

    PrintStream& out = printer.out();
    out.print(nameInFactsText(node.kind));
    switch (node.kind) {
    case SummaryNode::Kind::Unknown:
    case SummaryNode::Kind::Thrown:
    case SummaryNode::Kind::Join:
        return;
    case SummaryNode::Kind::Parameter:
    case SummaryNode::Kind::Result:
        out.print(" "_s, node.first);
        return;
    case SummaryNode::Kind::Capture:
        out.print(" "_s, node.first);
        if (node.second) {
            out.print(" of "_s);
            printer.print(FunctionID(node.second - 1));
        }
        return;
    case SummaryNode::Kind::Variable:
        out.print(" "_s);
        printer.print(VariableID(node.first));
        return;
    case SummaryNode::Kind::Field:
        out.print(" "_s);
        printFieldInFactsText(printer, LayoutID(node.first), node.second);
        return;
    case SummaryNode::Kind::Member:
        out.print(" "_s);
        printMemberInFactsText(printer, LayoutID(node.first), node.second & markOfStaticSide ? ClassSide::Static : ClassSide::Prototype, node.second & ~markOfStaticSide);
        return;
    case SummaryNode::Kind::Contents:
        out.print(" "_s);
        printSlotOfContents(printer, SlotOfContents::fromNumber(node.second));
        out.print(" of "_s);
        printNodeOfSummary(printer, summary, summary.nodes[node.first]);
        return;
    case SummaryNode::Kind::Constant:
        out.print(" "_s);
        printer.print(TypeID::fromBits(node.first));
        return;
    case SummaryNode::Kind::Allocation:
        out.print(" "_s);
        printer.print(LayoutID(node.first));
        return;
    case SummaryNode::Kind::ClassInstance:
        out.print(" "_s);
        printer.print(BuiltinClassID(node.first));
        if (node.second)
            out.print(" #"_s, node.second - 1);
        return;
    case SummaryNode::Kind::Closure:
        out.print(" "_s);
        printer.print(FunctionID(node.first));
        return;
    case SummaryNode::Kind::Builtin:
        out.print(" "_s);
        printer.print(BuiltinID(node.first));
        return;
    case SummaryNode::Kind::ResultOfCall:
        out.print(" c"_s, node.first, "."_s, node.second);
        return;
    case SummaryNode::Kind::ResultOfAccess:
        out.print(" a"_s, node.first);
        return;
    case SummaryNode::Kind::ResultOfOperator:
        out.print(" o"_s, node.first);
        return;
    case SummaryNode::Kind::ResultOfConversion:
        out.print(" c"_s, node.first);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printReferenceToNodeOfSummary(IRPrinter& printer, const ThinSummary& summary, uint32_t index)
{
    PrintStream& out = printer.out();
    if (index >= summary.nodes.size()) {
        out.print("<invalid node "_s, index, ">"_s);
        return;
    }

    const SummaryNode& node = summary.nodes[index];
    switch (node.kind) {
    case SummaryNode::Kind::Join:
        out.print("n"_s, index);
        return;
    case SummaryNode::Kind::ResultOfCall:
        out.print("c"_s, node.first, "."_s, node.second);
        return;
    case SummaryNode::Kind::ResultOfAccess:
        out.print("a"_s, node.first);
        return;
    case SummaryNode::Kind::ResultOfOperator:
        out.print("o"_s, node.first);
        return;
    case SummaryNode::Kind::ResultOfConversion:
        out.print("converted-c"_s, node.first);
        return;
    case SummaryNode::Kind::Unknown:
    case SummaryNode::Kind::Parameter:
    case SummaryNode::Kind::Result:
    case SummaryNode::Kind::Thrown:
    case SummaryNode::Kind::Capture:
    case SummaryNode::Kind::Variable:
    case SummaryNode::Kind::Field:
    case SummaryNode::Kind::Member:
    case SummaryNode::Kind::Contents:
    case SummaryNode::Kind::Constant:
    case SummaryNode::Kind::Allocation:
    case SummaryNode::Kind::ClassInstance:
    case SummaryNode::Kind::Closure:
    case SummaryNode::Kind::Builtin:
        printNodeOfSummary(printer, summary, node);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printNamedNodeOfSummary(IRPrinter& printer, const ThinSummary& summary, ASCIILiteral name, uint32_t index)
{
    if (index == noSummaryNode)
        return;
    printer.out().print(" "_s, name);
    printReferenceToNodeOfSummary(printer, summary, index);
}

static void printCallOfSummary(IRPrinter& printer, const ThinSummary& summary, uint32_t index)
{
    PrintStream& out = printer.out();
    const SummaryCall& call = summary.calls[index];
    out.print("  call c"_s, index, " "_s);
    printer.print(call.site);
    switch (call.callee) {
    case SummaryCall::Callee::Binding:
        out.print(" Binding "_s);
        printer.print(call.function);
        break;
    case SummaryCall::Callee::Name:
        out.print(" Name "_s);
        printer.print(call.key);
        break;
    case SummaryCall::Callee::Value:
        out.print(" Value "_s);
        printReferenceToNodeOfSummary(printer, summary, call.subject);
        break;
    case SummaryCall::Callee::Builtin:
        out.print(" Builtin "_s);
        printer.print(call.builtin);
        break;
    }
    printNamedNodeOfSummary(printer, summary, "this="_s, call.receiver);
    printNamedNodeOfSummary(printer, summary, "new-target="_s, call.newTarget);
    out.print(" ("_s);
    CommaPrinter comma;
    for (uint32_t argument : summary.arguments.span().subspan(call.firstArgument, call.numberOfArguments)) {
        out.print(comma);
        printReferenceToNodeOfSummary(printer, summary, argument);
    }
    out.print(")"_s);
    if (call.isConstruction)
        out.print(" constructs"_s);
    if (call.isInLoop)
        out.print(" in-loop"_s);
    if (call.isInTailPosition)
        out.print(" in-tail-position"_s);
    if (call.liesOnNoWayToReturn)
        out.print(" on-no-way-to-return"_s);
    if (call.passesList)
        out.print(" passes-list"_s);
    if (call.passesOnGatheredArguments)
        out.print(" passes-on-gathered-arguments"_s);
    printNamedNodeOfSummary(printer, summary, "throws-to="_s, call.thrownTo);
    if (call.expectedBuiltin) {
        out.print(call.expectsCalleeItself ? " expected-to-call="_s : " on-what-is-expected-of="_s);
        printer.print(call.expectedBuiltin);
    }
    out.println();
}

static void printAccessOfSummary(IRPrinter& printer, const ThinSummary& summary, uint32_t index)
{
    PrintStream& out = printer.out();
    const SummaryAccess& access = summary.accesses[index];
    out.print("  access a"_s, index, " "_s);
    printer.print(access.site);
    out.print(" "_s, nameInFactsText(access.kind), " "_s);
    printReferenceToNodeOfSummary(printer, summary, access.subject);
    printNamedNodeOfSummary(printer, summary, "this="_s, access.receiver);
    out.print(" "_s);
    if (access.isToContents()) {
        out.print("contents "_s);
        printSlotOfContents(printer, access.slot);
        printNamedNodeOfSummary(printer, summary, "else "_s, access.whatHoldsNothing());
        printNamedNodeOfSummary(printer, summary, "then "_s, access.keyNode);
    } else if (access.key)
        printer.print(access.key);
    else {
        out.print("["_s);
        printReferenceToNodeOfSummary(printer, summary, access.keyNode);
        out.print("]"_s);
    }
    switch (access.kind) {
    case SummaryAccess::Kind::Read:
        printNamedNodeOfSummary(printer, summary, "-> "_s, access.value);
        break;
    case SummaryAccess::Kind::Store:
    case SummaryAccess::Kind::Define:
        printNamedNodeOfSummary(printer, summary, "<- "_s, access.value);
        break;
    case SummaryAccess::Kind::Has:
        break;
    }
    if (StringID beginning = access.beginningOfKey()) {
        out.print(" begins-with "_s);
        printer.print(beginning);
    }
    if (access.isPartOfConstruction)
        out.print(" at-construction"_s);
    if (access.liesOnNoWayToReturn)
        out.print(" on-no-way-to-return"_s);
    if (access.storesObjectMadeForIt)
        out.print(" made-for-it"_s);
    if (access.laysAccessorAsWritten) {
        const SummaryWrittenAccessor& written = summary.writtenAccessorOf(index);
        out.print(" accessor-as-written"_s);
        if (written.getter) {
            out.print(" getter="_s);
            printer.print(written.getter);
        }
        if (written.setter) {
            out.print(" setter="_s);
            printer.print(written.setter);
        }
        if (written.map) {
            out.print(" in-map="_s);
            printer.print(written.map);
        }
    }
    if (access.kind == SummaryAccess::Kind::Define)
        printNamedNodeOfSummary(printer, summary, "descriptor-from="_s, access.descriptorFromElsewhere);
    printNamedNodeOfSummary(printer, summary, "throws-to="_s, access.thrownTo);
    out.println();
}

static void printOperatorOfSummary(IRPrinter& printer, const ThinSummary& summary, uint32_t index)
{
    PrintStream& out = printer.out();
    const SummaryOperator& operation = summary.operators[index];
    out.print("  operator o"_s, index, " "_s);
    printer.print(operation.site);
    out.print(" "_s, nameOf(operation.opcode), " ("_s);
    CommaPrinter comma;
    for (uint32_t operand = 0; operand < SummaryOperator::mostOperands && operation.operands[operand] != noSummaryNode; ++operand) {
        out.print(comma);
        printReferenceToNodeOfSummary(printer, summary, operation.operands[operand]);
        printNamedNodeOfSummary(printer, summary, "or "_s, operation.converted[operand]);
    }
    out.println(operation.writtenDivisor == SummaryOperator::WrittenDivisor::None ? ")"_s : operation.writtenDivisor == SummaryOperator::WrittenDivisor::FromOneBelowTwoTo31 ? ") divisor-written-below-two-to-31"_s : ") divisor-written-up-to-two-to-32"_s);
}

static void printChangeOfSummary(IRPrinter& printer, const ThinSummary& summary, uint32_t index)
{
    PrintStream& out = printer.out();
    const SummaryChange& change = summary.changes[index];
    out.print("  change h"_s, index, " "_s);
    printer.print(change.site);
    out.print(" "_s, nameInFactsText(change.kind), " "_s);
    printReferenceToNodeOfSummary(printer, summary, change.subject);
    if (change.key) {
        out.print(" "_s);
        printer.print(change.key);
    }
    printNamedNodeOfSummary(printer, summary, "other="_s, change.other);
    printNamedNodeOfSummary(printer, summary, "getter="_s, change.getter);
    if (change.isUnderNameThatNobodyElseNames)
        out.print(" under-a-name-that-nobody-else-names"_s);
    if (change.descriptorIsInSight)
        out.print(" descriptor-in-sight"_s);
    if (change.isEntryWrittenInMap)
        out.print(" entry-written-in-map"_s);
    if (change.laysTextNumberOrBooleanThatIsWritten)
        out.print(" lays-text-number-or-boolean"_s);
    printNamedNodeOfSummary(printer, summary, "getter-in-sight="_s, change.getterInSight);
    printNamedNodeOfSummary(printer, summary, "setter-in-sight="_s, change.setterInSight);
    printNamedNodeOfSummary(printer, summary, "laid-through-constant="_s, change.valueLaidThroughConstant);
    if (change.lays != SummaryChange::Lays::WhatDescriptorSays)
        out.print(change.lays == SummaryChange::Lays::Getter ? " getter"_s : " setter"_s);
    if (change.liesOnNoWayToReturn)
        out.print(" on-no-way-to-return"_s);
    if (change.definesWhatItCopies)
        out.print(" defines"_s);
    out.println();
}

static void printSummaryInFactsText(IRPrinter& printer, FunctionID function, const ThinSummary& summary)
{
    PrintStream& out = printer.out();
    out.print("summary "_s);
    printer.print(function);
    out.print(" operations="_s, summary.numberOfOperations);
    if (!summary.traits.isEmpty()) {
        out.print(" traits="_s);
        printFlagsInFactsText(out, summary.traits);
    }
    out.print(" effects="_s);
    printEffectsInFactsText(out, summary.ownEffects());
    if (!summary.ownWritesOnWaysToReturn.isEmpty())
        out.print(" writes-less-on-return"_s);
    if (std::optional<Effects> ofOperators = summary.ownEffectsOfOperators()) {
        out.print(" operators="_s);
        printEffectsInFactsText(out, *ofOperators);
    }
    if (summary.operatorsLeftApartMayJoinTexts)
        out.print(" may-join-texts"_s);
    if (summary.someBlockHasHandler)
        out.print(" has-handler"_s);
    if (summary.layoutsOfOwnWrites())
        printLayoutsWrittenInFactsText(out, "own-writes-in"_s, summary.layoutsOfOwnWrites());
    out.println();

    for (uint32_t index = 0; index < summary.nodes.size(); ++index) {
        out.print("  n"_s, index, " "_s);
        printNodeOfSummary(printer, summary, summary.nodes[index]);
        out.println();
    }
    for (uint32_t index = 0; index < summary.calls.size(); ++index)
        printCallOfSummary(printer, summary, index);
    for (uint32_t index = 0; index < summary.accesses.size(); ++index)
        printAccessOfSummary(printer, summary, index);
    if (std::span<const uint32_t> operands = summary.operandsOfOperatorsLeftApart.span(); !operands.empty()) {
        out.print("  operands-of-operators"_s);
        CommaPrinter comma(", "_s, " "_s);
        for (uint32_t node : operands) {
            out.print(comma);
            printReferenceToNodeOfSummary(printer, summary, node);
        }
        out.println();
    }
    for (uint32_t index = 0; index < summary.operators.size(); ++index)
        printOperatorOfSummary(printer, summary, index);
    for (uint32_t index = 0; index < summary.changes.size(); ++index)
        printChangeOfSummary(printer, summary, index);
    for (const ThinSummary::Inspection& inspection : summary.inspections) {
        out.print("  inspection "_s);
        printer.print(inspection.site);
        out.print(inspection.kind == ThinSummary::Inspection::Kind::WalksChain ? " WalksChain "_s : " ListsKeys "_s);
        printReferenceToNodeOfSummary(printer, summary, inspection.subject);
        out.println();
    }
    for (const SummaryMember& member : summary.members) {
        out.print("  member "_s);
        printMemberInFactsText(printer, member.classLayout, member.side, member.index);
        printKindAndKeyOfMemberInFactsText(printer, member.kind, member.key);
        out.print(" = "_s);
        printReferenceToNodeOfSummary(printer, summary, member.value);
        out.println();
    }
    for (const SummaryConstruction& construction : summary.constructions) {
        std::span<const SummaryConstruction::Key> keys = summary.keysOfConstructions.span().subspan(construction.firstKey, construction.numberOfKeys);
        auto printKeys = [&](ASCIILiteral what, bool SummaryConstruction::Key::* isMeant) {
            out.print(" "_s, what, " ["_s);
            CommaPrinter comma;
            for (const SummaryConstruction::Key& key : keys) {
                if (!(key.*isMeant))
                    continue;
                out.print(comma);
                printer.print(key.key);
            }
            out.print("]"_s);
        };
        out.print("  construction "_s, nameInFactsText(construction.object));
        printKeys("fills"_s, &SummaryConstruction::Key::isFilled);
        printKeys("looks-up"_s, &SummaryConstruction::Key::isLookedUp);
        if (construction.numberOfParts) {
            out.print(" parts ["_s);
            CommaPrinter comma;
            for (const SummaryConstruction::Part& part : summary.partsOfConstructions.span().subspan(construction.firstPart, construction.numberOfParts)) {
                out.print(comma);
                printer.print(part.function);
                if (!part.runsOnEveryWay)
                    out.print(" on-some-ways"_s);
            }
            out.print("]"_s);
        }
        if (construction.isSeenFromOutside)
            out.print(" seen-from-outside"_s);
        Vector<PropertyKeyID, 16> filledAtEndAlone;
        for (PropertyKeyID key : summary.keysFilledAtEnds.span().subspan(construction.firstKeyFilledAtEnd, construction.numberOfKeysFilledAtEnd)) {
            if (!std::ranges::any_of(keys, [&](const SummaryConstruction::Key& listed) { return listed.key == key && listed.isFilled; }))
                filledAtEndAlone.append(key);
        }
        if (!filledAtEndAlone.isEmpty()) {
            std::ranges::sort(filledAtEndAlone);
            out.print(" fills-at-end ["_s);
            CommaPrinter comma;
            for (PropertyKeyID key : filledAtEndAlone) {
                out.print(comma);
                printer.print(key);
            }
            out.print("]"_s);
        }
        out.println();
    }
    Vector<FieldReference> fieldsLeftEmpty;
    summary.forEachLayoutLeftEmpty([&](LayoutID layout, std::span<const uint32_t> fieldsHeld) {
        for (uint32_t field = 0; field < printer.program().at(layout).fields.size(); ++field) {
            if (!std::ranges::binary_search(fieldsHeld, field))
                fieldsLeftEmpty.append(FieldReference { layout, field });
        }
    });
    std::ranges::sort(fieldsLeftEmpty);
    fieldsLeftEmpty.shrink(std::ranges::unique(fieldsLeftEmpty).begin() - fieldsLeftEmpty.begin());
    for (FieldReference field : fieldsLeftEmpty) {
        out.print("  leaves-empty "_s);
        printer.print(field.layout);
        out.print("."_s, field.field, " "_s);
        printer.print(fieldAt(printer.program(), field).key);
        out.println();
    }
    for (const SummaryMakerThatEndsWhereItLeaves& maker : summary.makersThatEndWhereTheyLeave) {
        out.print("  ends-where-it-leaves v"_s, maker.allocation.index(), " b"_s, maker.blockOfLeaving, " at "_s, maker.positionOfLeaving);
        out.println();
    }
    for (FieldReference field : summary.fieldsFilledAtEndAlone) {
        out.print("  fills-at-end "_s);
        printer.print(field.layout);
        out.print("."_s, field.field, " "_s);
        printer.print(fieldAt(printer.program(), field).key);
        out.println();
    }
    summary.forEachListOfAllocation([&](LayoutID layout, std::span<const uint32_t> listed) {
        out.print("  allocation-lists "_s);
        printer.print(layout);
        for (uint32_t field : listed) {
            out.print(" "_s);
            printer.print(field & ThinSummary::keyInListOfAllocation ? PropertyKeyID(field & ~ThinSummary::keyInListOfAllocation) : fieldAt(printer.program(), FieldReference { layout, field }).key);
        }
        out.println();
    });
    if (!summary.classesEvaluated.isEmpty()) {
        out.print("  classes"_s);
        CommaPrinter comma(", "_s, " "_s);
        for (LayoutID layout : summary.classesEvaluated) {
            out.print(comma);
            printer.print(layout);
        }
        out.println();
    }
    for (const SummaryDefinition& definition : summary.definitions) {
        out.print("  defines "_s);
        printer.print(definition.variable);
        out.print(" made "_s);
        printer.print(definition.made);
        out.print(" stored "_s);
        printer.print(definition.store);
        out.println();
    }
    for (const ThinSummary::AnswerOfRequire& answer : summary.answersOfRequire)
        out.println("  answer-of-require m"_s, answer.module.index(), " -> n"_s, answer.node);
    for (const ThinSummary::AnswerOfRequire& answer : summary.objectsOfCompiledModules)
        out.println("  object-of-compiled-module m"_s, answer.module.index(), " -> n"_s, answer.node);
    for (const ThinSummary::NameThatLoaderTakes& name : summary.namesThatLoaderTakes)
        out.println("  loader-takes n"_s, name.read, " -> g"_s, name.variable.index());
    for (uint32_t read : summary.namesThatHostLooksFor)
        out.println("  host-looks-for n"_s, read);
    if (!summary.firstStoresOfBody.isEmpty()) {
        out.print("  stores-first"_s);
        CommaPrinter comma(", "_s, " "_s);
        for (SummaryPlaceInBody store : summary.firstStoresOfBody) {
            out.print(comma);
            printer.print(VariableID(store.number));
            out.print(" at "_s);
            printer.print(ValueID(store.position / 2));
        }
        out.println();
    }
    if (!summary.firstUsesOfFunctions.isEmpty()) {
        out.print("  uses-first"_s);
        CommaPrinter comma(", "_s, " "_s);
        for (SummaryPlaceInBody use : summary.firstUsesOfFunctions) {
            out.print(comma);
            printer.print(FunctionID(use.number));
            out.print(use.position & 1 ? " at "_s : " at the end in front of "_s);
            printer.print(ValueID(use.position / 2));
        }
        out.println();
    }
    if (!summary.otherModulesNamed.isEmpty()) {
        out.print("  names-records-of"_s);
        CommaPrinter comma(", "_s, " "_s);
        for (ModuleID module : summary.otherModulesNamed) {
            out.print(comma);
            printer.print(module);
        }
        out.println();
    }
    if (!summary.modulesHandedOut.isEmpty()) {
        out.print("  hands-out"_s);
        CommaPrinter comma(", "_s, " "_s);
        for (ModuleID module : summary.modulesHandedOut) {
            out.print(comma);
            printer.print(module);
        }
        out.println();
    }
    if (!summary.modulesNamedInBlocksNeverEntered.isEmpty()) {
        out.print("  names-where-no-run-comes"_s);
        CommaPrinter comma(", "_s, " "_s);
        for (ModuleID module : summary.modulesNamedInBlocksNeverEntered) {
            out.print(comma);
            printer.print(module);
        }
        out.println();
    }
    if (!summary.blocksNeverEntered.isEmpty()) {
        out.print("  never-entered"_s);
        CommaPrinter comma(", "_s, " "_s);
        for (BlockID block : summary.blocksNeverEntered)
            out.print(comma, "b"_s, block.index());
        out.println();
    }
    for (const OtherNameOfObjectBeingMade& name : summary.otherNamesOfObjectsBeingMade) {
        out.print("  still-being-made "_s);
        printer.print(name.parameter);
        out.print(" = "_s);
        printer.print(name.object);
        out.println();
    }
    for (const SummaryHandler& handler : summary.handlers) {
        out.print("  handler b"_s, handler.handler.index(), " caught="_s);
        printReferenceToNodeOfSummary(printer, summary, handler.caught);
        out.println();
    }
    for (const ThinSummary::AccessUnderConstantKey& access : summary.accessesUnderConstantKeys) {
        out.print("  under-constant-key "_s);
        printer.print(access.site);
        out.print(" "_s);
        printer.print(access.key);
        out.println();
    }
    if (!summary.modulesWhosePathIsRead.isEmpty()) {
        out.print("  reads-paths-of"_s);
        CommaPrinter comma(", "_s, " "_s);
        for (ModuleID module : summary.modulesWhosePathIsRead) {
            out.print(comma);
            printer.print(module);
        }
        out.println();
    }
    if (summary.mayReadPathOfAnyModule)
        out.println("  may-read-path-of-any-module"_s);
    for (const SummaryEdge& edge : summary.edges) {
        out.print("  edge "_s);
        printReferenceToNodeOfSummary(printer, summary, edge.from);
        out.print(" -> "_s);
        printReferenceToNodeOfSummary(printer, summary, edge.to);
        if (edge.passes) {
            out.print(" passes="_s);
            printer.print(edge.passes);
        }
        out.println();
    }
}

static void runSummaryCommand(TestCommandContext& context)
{
    std::optional<Vector<FunctionID>> functions = functionsNamedBy(*context.program, context.arguments, context.out);
    if (!functions)
        return;

    StagedBuild stage = context.stage();
    const Facts* facts = stage.facts();
    if (!facts)
        return;

    const Program& program = stage.program();
    IRPrinter printer(context.out, program);
    for (FunctionID function : *functions) {
        if (!program.at(function).hasBody()) {
            context.out.println("summary f"_s, function.index(), " has no body"_s);
            continue;
        }
        printSummaryInFactsText(printer, function, facts->summaries().at(function));
    }
}

enum class PartOfLinkTables : uint8_t { Functions, Calls, Accesses, Fields, Variables, Captures, Members, Layouts, Modules, Families, Counts };

static constexpr PartOfLinkTables lookupsAmongPartsOfLinkTables = static_cast<PartOfLinkTables>(UINT8_MAX - 1);

struct NamedPartOfLinkTables {
    ASCIILiteral name;
    PartOfLinkTables part;
};

static constexpr NamedPartOfLinkTables namedPartsOfLinkTables[] {
    { "functions"_s, PartOfLinkTables::Functions },
    { "calls"_s, PartOfLinkTables::Calls },
    { "accesses"_s, PartOfLinkTables::Accesses },
    { "fields"_s, PartOfLinkTables::Fields },
    { "variables"_s, PartOfLinkTables::Variables },
    { "captures"_s, PartOfLinkTables::Captures },
    { "members"_s, PartOfLinkTables::Members },
    { "layouts"_s, PartOfLinkTables::Layouts },
    { "modules"_s, PartOfLinkTables::Modules },
    { "families"_s, PartOfLinkTables::Families },
    { "lookups"_s, lookupsAmongPartsOfLinkTables },
    { "counts"_s, PartOfLinkTables::Counts },
};

static void printTypeFieldInFactsText(IRPrinter& printer, ASCIILiteral name, TypeID type)
{
    if (!type)
        return;
    printer.out().print(" "_s, name, "="_s);
    printer.print(type);
}

static void printLinkedCell(IRPrinter& printer, const LinkedCell& cell, bool isLaidLate = false, bool isStoredByFollowerOfMaker = false, bool isShownToComparisons = false)
{
    printTypeFieldInFactsText(printer, "proven"_s, cell.proven);
    if (cell.everyWriterIsKnown)
        printer.out().print(" writers-known"_s);
    if (!cell.isWrittenAfterConstruction)
        printer.out().print(" written-at-construction-alone"_s);
    if (cell.staysAsItsClassMakesIt)
        printer.out().print(" stays-as-made"_s);
    if (cell.isFilled)
        printer.out().print(" filled"_s);
    if (cell.isFilled && !cell.isFilledAtEveryRead)
        printer.out().print(" read-by-its-makers-while-empty"_s);
    if (cell.isOfBirth && !cell.isFilled)
        printer.out().print(" of-birth"_s);
    if (cell.isFilledByAccessorsAlone)
        printer.out().print(" filled-by-accessors-alone"_s);
    if (cell.holdsAccessorFromBirth)
        printer.out().print(" accessor-from-birth"_s);
    if (cell.isOnlyGivenUndefinedByMakers)
        printer.out().print(" only-given-undefined"_s);
    if (cell.liesBesideWhatDescriptorsDefine)
        printer.out().print(" beside-what-descriptors-define"_s);
    if (!cell.formThatDefinitionsCreate.isEmpty()) {
        printer.out().print(" form="_s);
        for (auto [flag, word] : { std::pair { FieldForm::IsReadOnly, "read-only"_s }, std::pair { FieldForm::IsNotEnumerable, "not-enumerable"_s }, std::pair { FieldForm::IsNotConfigurable, "not-configurable"_s }, std::pair { FieldForm::IsAccessor, "accessor"_s } }) {
            if (cell.formThatDefinitionsCreate.contains(flag))
                printer.out().print(word, "."_s);
        }
    }
    if (isLaidLate)
        printer.out().print(" laid-late"_s);
    if (isStoredByFollowerOfMaker)
        printer.out().print(" stored-by-follower-of-maker"_s);
    if (isShownToComparisons)
        printer.out().print(" shown-to-comparisons"_s);
    printer.out().println();
}

static void printNodeOfReferrersInFactsText(IRPrinter& printer, const LinkTables& tables, FunctionID node)
{
    if (!tables.isGroup(node)) {
        printer.print(node);
        return;
    }
    const LinkedGroup& group = tables.group(node);
    switch (group.kind) {
    case LinkedGroup::Kind::Anybody:
        printer.out().print("anybody"_s);
        return;
    case LinkedGroup::Kind::Constructors:
        printer.out().print("constructors of F"_s, group.subject);
        return;
    case LinkedGroup::Kind::FunctionsInFields:
        printer.out().print("functions in the fields of L"_s, group.subject);
        return;
    case LinkedGroup::Kind::SharedCall:
        printer.out().print("shared call "_s, group.subject);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printReferrersInFactsText(IRPrinter& printer, const LinkTables& tables, FunctionID node)
{
    if (node.index() + 1 >= tables.firstReferrer.size() || tables.referrersOf(node).empty())
        return;
    printer.out().print("  referrers"_s);
    CommaPrinter comma(", "_s, " "_s);
    for (FunctionID referrer : tables.referrersOf(node)) {
        printer.out().print(comma);
        printNodeOfReferrersInFactsText(printer, tables, referrer);
    }
    printer.out().println();
}

static void printLinkedFunctions(IRPrinter& printer, const LinkTables& tables)
{
    PrintStream& out = printer.out();
    for (uint32_t index = 0; index < tables.functions.size(); ++index) {
        FunctionID id(index);
        const LinkedFunction& function = tables.function(id);
        out.print("function "_s);
        printer.print(id);
        if (!function.isReached)
            out.print(" unreached"_s);
        if (function.everyCallerIsKnown)
            out.print(" callers-known"_s);
        if (function.everyCallHasOneCallee)
            out.print(" one-callee"_s);
        if (function.closuresAreChanged)
            out.print(" closures-changed"_s);
        out.print(" cycle="_s, function.cycle, " rank="_s, tables.rankOfCycle[function.cycle]);
        printTypeFieldInFactsText(printer, "result"_s, function.result);
        out.print(" returns="_s);
        printEffectsInFactsText(out, function.effectsOnReturn());
        out.print(" throws="_s);
        printEffectsInFactsText(out, function.effectsOnThrow());
        printFieldsWrittenInFactsText(out, "fields-written-on-return"_s, function.effectsOnReturn());
        printFieldsWrittenInFactsText(out, "fields-written"_s, function.effectsOnThrow());
        if (!tables.known.layoutsWrittenByFunction.isEmpty() && !function.effectsOnThrow().mayCallUnknownCode)
            printLayoutsWrittenInFactsText(out, "layouts-written"_s, tables.known.layoutsWrittenBy(id));
        if (function.runsAtMostOnce)
            out.print(" runs-once"_s);
        if (function.isReached && !function.ringsOfTailCallsAreListed)
            out.print(" ring-by-value"_s);
        if (function.isInQueueFromStart)
            out.print(" in-queue-from-start"_s);
        if (function.everyCallerIsKnown && function.everyCallHasOneCallee && !function.isNamedInItsCycle)
            out.print(" takes-arrivals"_s);
        if (function.weightAsLeaf != LinkedFunction::notLeaf)
            out.print(" leaf="_s, function.weightAsLeaf);
        out.println();

        std::span<const LinkedParameter> parameters = tables.parametersOf(id);
        for (uint32_t parameter = 0; parameter < parameters.size(); ++parameter) {
            const LinkedParameter& linked = parameters[parameter];
            out.print("  parameter "_s, parameter);
            printTypeFieldInFactsText(printer, "expected"_s, linked.writtenOrSeenToArrive());
            if (linked.written)
                out.print(" declared"_s);
            printTypeFieldInFactsText(printer, "proven"_s, linked.proven);
            if (linked.staysAsItsClassMakesIt)
                out.print(" stays-as-made"_s);
            if (linked.isOnlyCalled)
                out.print(" only-called"_s);
            if (linked.isCapturedWhereOnlyCalled)
                out.print(" captured"_s);
            if (linked.isOnlyLookedAt)
                out.print(" only-looked-at"_s);
            out.println();
        }

        if (const LinkedEffectsGivenArguments* given = tables.known.findEffectsGivenArguments(id)) {
            out.print("  given-arguments"_s);
            printEntryParametersInFactsText(out, "as-expected"_s, given->parametersTakenAsExpected);
            printEntryParametersInFactsText(out, "called"_s, given->parametersCalled);
            printLayoutsWrittenInFactsText(out, "layouts-written"_s, given->layoutsWritten);
            out.print(" returns="_s);
            printEffectsInFactsText(out, given->onReturn);
            out.print(" throws="_s);
            printEffectsInFactsText(out, given->onThrow);
            printFieldsWrittenInFactsText(out, "fields-written"_s, given->onThrow);
            out.println();
        }
        printReferrersInFactsText(printer, tables, id);
    }

    for (const LinkedCatch& row : tables.catches) {
        out.print("catch "_s);
        printer.print(row.function);
        out.print(" b"_s, row.handler.index());
        printTypeFieldInFactsText(printer, "caught"_s, row.caught);
        out.println();
    }

    for (uint32_t index = 0; index < tables.groups.size(); ++index) {
        FunctionID node(static_cast<uint32_t>(tables.functions.size()) + index);
        out.print("group "_s);
        printNodeOfReferrersInFactsText(printer, tables, node);
        out.println(" cycle="_s, tables.groups[index].cycle, " rank="_s, tables.rankOfCycle[tables.groups[index].cycle]);
        printReferrersInFactsText(printer, tables, node);
    }
}

static void printLinkedCalls(IRPrinter& printer, const LinkTables& tables)
{
    for (const LinkedCall& call : tables.calls) {
        printer.out().print("call "_s);
        printer.print(call.caller);
        printer.out().print(" "_s);
        printer.print(call.site);
        printer.out().print(" -> "_s);
        printer.print(call.callee);
        printer.out().println();
    }
}

static void printLinkedCallsThatFindBuiltins(IRPrinter& printer, const LinkTables& tables)
{
    for (const LinkedCallThatFindsBuiltin& call : tables.callsThatFindBuiltins) {
        printer.out().print("call "_s);
        printer.print(call.caller);
        printer.out().print(" "_s);
        printer.print(call.site);
        printer.out().print(" finds "_s);
        printer.print(call.builtin);
        if (call.callee) {
            printer.out().print(" beside "_s);
            printer.print(call.callee);
        }
        printer.out().println();
    }
}

static void printLinkedAccesses(IRPrinter& printer, const LinkTables& tables)
{
    for (const LinkedAccess& access : tables.accessesUnderOneKey) {
        printer.out().print("access "_s);
        printer.print(access.function);
        printer.out().print(" "_s);
        printer.print(access.site);
        printer.out().print(" under "_s);
        printer.print(access.key);
        printer.out().println();
    }
    for (const LinkedResultOfRead& read : tables.resultsOfReadsByKey) {
        printer.out().print("read "_s);
        printer.print(read.function);
        printer.out().print(" "_s);
        printer.print(read.site);
        printer.out().print(" proven="_s);
        printer.print(read.proven);
        printer.out().println();
    }
    for (const LinkedReceiverAsMade& receiver : tables.receiversThatStayAsMade) {
        printer.out().print("receiver-as-made "_s);
        printer.print(receiver.function);
        printer.out().print(" "_s);
        printer.print(receiver.site);
        printer.out().println();
    }
    for (const LinkedReceiverAsMade& receiver : tables.receiversThatNobodyChanges) {
        printer.out().print("receiver-unchanged "_s);
        printer.print(receiver.function);
        printer.out().print(" "_s);
        printer.print(receiver.site);
        printer.out().println();
    }
    for (const LinkedReadOffViews& read : tables.readsOffViewsOfWrittenLengths) {
        printer.out().print("read-off-views "_s);
        printer.print(read.function);
        printer.out().print(" "_s);
        printer.print(read.site);
        printer.out().println(" least-length="_s, read.leastLength);
    }
    for (const LinkedReceiverAsMade& iterable : tables.iterablesThatStayAsMade) {
        printer.out().print("iterable-as-made "_s);
        printer.print(iterable.function);
        printer.out().print(" "_s);
        printer.print(iterable.site);
        printer.out().println();
    }
    for (const LinkedAccessorsOfField& expected : tables.accessorsExpectedInFields) {
        printer.out().print("accessors "_s);
        printFieldInFactsText(printer, expected.field.layout, expected.field.field);
        if (expected.getter) {
            printer.out().print(" getter="_s);
            printer.print(expected.getter);
            if (expected.getterIsProven)
                printer.out().print(" proven"_s);
            if (expected.getterMayNotBeLaidYet)
                printer.out().print(" once-laid"_s);
        }
        if (expected.setter) {
            printer.out().print(" setter="_s);
            printer.print(expected.setter);
            if (expected.setterIsProven)
                printer.out().print(" proven"_s);
        }
        printer.out().println();
    }
    for (const LinkedGetterAtRead& read : tables.gettersExpectedAtReads) {
        printer.out().print("read "_s);
        printer.print(read.function);
        printer.out().print(" "_s);
        printer.print(read.site);
        printer.out().print(" expects getter "_s);
        printer.print(read.getter);
        if (read.isHeardInLoopsAlone)
            printer.out().print(" in-loops-alone"_s);
        printer.out().println();
    }
}

static void printLinkedCoroutines(IRPrinter& printer, const LinkTables& tables)
{
    for (const LinkedCoroutine& coroutine : tables.coroutines) {
        printer.out().print("coroutine "_s);
        printer.print(coroutine.function);
        printTypeFieldInFactsText(printer, "fulfilled"_s, coroutine.fulfilledWith);
        printTypeFieldInFactsText(printer, "returned"_s, coroutine.returned);
        printer.out().println();
    }
}

static void printLinkedFields(IRPrinter& printer, const LinkTables& tables)
{
    for (uint32_t layout = 0; layout + 1 < tables.firstFieldOfLayout.size(); ++layout) {
        uint32_t numberOfFields = tables.firstFieldOfLayout[layout + 1] - tables.firstFieldOfLayout[layout];
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            printer.out().print("field "_s);
            printFieldInFactsText(printer, LayoutID(layout), field);
            const LinkedCell& cell = tables.fields[tables.firstFieldOfLayout[layout] + field];
            const Field& written = printer.program().layouts[layout].fields[field];
            bool isWrittenPrivateName = written.key && printer.program().at(written.key).kind == PropertyKeyKind::PrivateName && !written.flags.contains(FieldFlags::IsHidden);
            printLinkedCell(printer, cell, false, false, isWrittenPrivateName && !cell.isKeptFromComparisons);
        }
        std::span<const PropertyKeyID> appendedKeys = tables.appendedKeysOf(LayoutID(layout));
        for (uint32_t place = 0; place < appendedKeys.size(); ++place) {
            printer.out().print("field "_s);
            printFieldInFactsText(printer, LayoutID(layout), numberOfFields + place);
            printer.out().print(" "_s);
            printer.print(appendedKeys[place]);
            printer.out().print(" appended"_s);
            printLinkedCell(printer, *tables.findCellOfField({ LayoutID(layout), numberOfFields + place }), tables.isAppendedFieldLaidLate({ LayoutID(layout), numberOfFields + place }), tables.isStoredByFollowerOfMaker({ LayoutID(layout), numberOfFields + place }));
        }
    }
}

static void printLinkedVariables(IRPrinter& printer, const LinkTables& tables)
{
    for (uint32_t index = 0; index < tables.variables.size(); ++index) {
        printer.out().print("variable "_s);
        printer.print(VariableID(index));
        printLinkedCell(printer, tables.variables[index]);
    }
    for (auto [variable, function] : tables.functionsTakenOutOfRequiredFiles) {
        printer.out().print("taken-out-of-required-file "_s);
        printer.print(variable);
        printer.out().print(" "_s);
        printer.print(function);
        printer.out().println();
    }
    for (const LinkedFunctionUnderName& laid : tables.functionsLaidUnderNamesOfVariables) {
        printer.out().print("laid-under-name "_s);
        printer.print(VariableID(laid.variable));
        printer.out().print(" "_s);
        printer.print(PropertyKeyID::fromBits(laid.key));
        printer.out().print(" "_s);
        printer.print(laid.function);
        printer.out().println();
    }
}

static void printLinkedCaptures(IRPrinter& printer, const LinkTables& tables)
{
    for (uint32_t function = 0; function + 1 < tables.firstCaptureOfFunction.size(); ++function) {
        uint32_t numberOfCaptures = tables.firstCaptureOfFunction[function + 1] - tables.firstCaptureOfFunction[function];
        for (uint32_t capture = 0; capture < numberOfCaptures; ++capture) {
            printer.out().print("capture "_s);
            printer.print(FunctionID(function));
            printer.out().print("."_s, capture);
            printLinkedCell(printer, tables.captures[tables.firstCaptureOfFunction[function] + capture]);
        }
    }
}

static void printLinkedMembers(IRPrinter& printer, const LinkTables& tables)
{
    PrintStream& out = printer.out();
    for (const LinkedMember& member : tables.members) {
        out.print("member "_s);
        printMemberInFactsText(printer, member.classLayout, member.side, member.index);
        printKindAndKeyOfMemberInFactsText(printer, member.kind, member.key);
        if (member.function) {
            out.print(" function="_s);
            printer.print(member.function);
        }
        if (member.everyWriterIsKnown)
            out.print(" writers-known"_s);
        out.println();
    }
    for (const LinkedMember& member : tables.membersAboveCreatedLayouts) {
        out.print("above-created "_s);
        printer.print(member.classLayout);
        printKindAndKeyOfMemberInFactsText(printer, member.kind, member.key);
        out.print(" function="_s);
        printer.print(member.function);
        out.println();
    }
    for (const LinkedNameOnFunction& name : tables.namesOnFunctions) {
        out.print("on-function "_s);
        printer.print(name.holder);
        out.print(" "_s);
        printer.print(name.key);
        out.print("\n"_s);
    }
    for (const LinkedNameOnPrototype& name : tables.namesOnPrototypes) {
        out.print("on-prototype "_s);
        printer.print(name.layout);
        if (name.key) {
            out.print(" "_s);
            printer.print(name.key);
        } else
            out.print(name.isUnderSymbolsAlone ? " [some symbol]"_s : " [some key]"_s);
        if (name.function) {
            out.print(" function="_s);
            printer.print(name.function);
        } else if (name.expected) {
            out.print(" expected="_s);
            printer.print(name.expected);
        }
        if (name.isStoredIntoFromOutside)
            out.print(" stored-into-from-outside"_s);
        if (name.expectedGetter) {
            out.print(" getter="_s);
            printer.print(name.expectedGetter);
        }
        if (name.expectedSetter) {
            out.print(" setter="_s);
            printer.print(name.expectedSetter);
        }
        out.println();
    }
}

static void printLinkedLayouts(IRPrinter& printer, const LinkTables& tables)
{
    PrintStream& out = printer.out();
    for (uint32_t index = 0; index < tables.layouts.size(); ++index) {
        const LinkedLayout& layout = tables.layouts[index];
        out.print("layout "_s);
        printer.print(LayoutID(index));
        if (layout.family)
            out.print(" family=F"_s, layout.family.index());
        if (!layout.isAllocated)
            out.print(" never-allocated"_s);
        out.print(" changes="_s);
        printFlagsInFactsText(out, layout.possibleChanges - LayoutChange::GetsObjectAbove);
        if (!layout.reachesUnseenCode)
            out.print(" stays-with-compiled-code"_s);
        if (layout.isAllocated && !layout.hasDirectInstances)
            out.print(" has-no-direct-instances"_s);
        if (layout.isRedefinedByLiteralsAlone)
            out.print(" redefined-by-literals-alone"_s);
        if (layout.isFilledUnderComputedKeys)
            out.print(" filled-under-computed-keys"_s);
        if (layout.onlyOwnObjectsStandBelowPrototype)
            out.print(" own-objects-below"_s);
        if (bool onPrototype = layout.namesAreDefinedPastLists[static_cast<size_t>(ClassSide::Prototype)], onClassObject = layout.namesAreDefinedPastLists[static_cast<size_t>(ClassSide::Static)]; onPrototype || onClassObject)
            out.print(" names-defined-past-lists=["_s, onPrototype ? "prototype"_s : ""_s, onPrototype && onClassObject ? ", "_s : ""_s, onClassObject ? "static"_s : ""_s, "]"_s);
        if (layout.prototypeObjectIsSettledAheadOfEveryObject)
            out.print(" prototype-object-settled"_s);
        if (layout.heritage) {
            out.print(" heritage="_s);
            printer.print(layout.heritage);
        }
        if (layout.possibleChanges.contains(LayoutChange::ChangesPrototype) && !layout.possibleChanges.contains(LayoutChange::GetsObjectAbove))
            out.print(" only-null-comes-above"_s);
        out.println();
    }
}

static void printLinkedModules(IRPrinter& printer, const LinkTables& tables)
{
    for (size_t index : tables.modulesWhoseValueStaysIn)
        printer.out().println("module m"_s, index, tables.modulesWhoseRecordIsHandedOutFrozen.get(index) ? " record-is-handed-out-frozen"_s : " value-stays-with-compiled-code"_s);
    for (size_t index : tables.exportsThatStayIn)
        printer.out().println("export g"_s, index, " stays-in"_s);
}

static void printFamiliesInFactsText(IRPrinter& printer, const Families& families)
{
    for (uint32_t index = 0; index < families.size(); ++index) {
        printer.out().print("family F"_s, index, " "_s);
        printer.print(families.layoutsOf(FamilyID(index)));
        printer.out().println();
    }
}

enum class TypeInLinkCounts : uint8_t { Nothing, Never, Primitives, OneLayout, SeveralLayouts, OneFunction, OneBuiltin, ObjectsOfClasses, OtherListedObjects, ListedObjectsBesidePrimitives, UnboundedObjects, NoJavaScriptValue };
static constexpr unsigned numberOfTypesInLinkCounts = 12;
static constexpr ASCIILiteral namesOfTypesInLinkCounts[numberOfTypesInLinkCounts] {
    "nothing"_s, "never"_s, "primitives"_s, "one-layout"_s, "several-layouts"_s, "one-function"_s, "one-builtin"_s, "objects-of-classes"_s, "other-listed-objects"_s, "listed-objects-beside-primitives"_s, "unbounded-objects"_s, "no-javascript-value"_s,
};

static TypeInLinkCounts classifyForLinkCounts(TypeReader types, TypeID id)
{
    if (!id)
        return TypeInLinkCounts::Nothing;
    Type type = types.at(id);
    if (!type.isJavaScriptValue())
        return TypeInLinkCounts::NoJavaScriptValue;
    if (type.isNever())
        return TypeInLinkCounts::Never;
    if (!type.hasObjects())
        return TypeInLinkCounts::Primitives;
    if (type.hasUnboundedUnknown())
        return TypeInLinkCounts::UnboundedObjects;
    if (type.primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent))
        return TypeInLinkCounts::ListedObjectsBesidePrimitives;
    type.primitives &= Primitive::UndefinedOrNull;
    if (singleFunctionOf(types, type))
        return TypeInLinkCounts::OneFunction;
    if (singleBuiltinOf(types, type))
        return TypeInLinkCounts::OneBuiltin;
    if (type.hasOnlyLayouts())
        return singleLayoutOf(types, type) ? TypeInLinkCounts::OneLayout : TypeInLinkCounts::SeveralLayouts;
    if (type.hasOnlyClassObjects())
        return TypeInLinkCounts::ObjectsOfClasses;
    return TypeInLinkCounts::OtherListedObjects;
}

struct TypesInLinkCounts {
    uint64_t total { 0 };
    uint64_t byKind[numberOfTypesInLinkCounts] { };
    uint64_t numbers { 0 };
    uint64_t integers { 0 };
    uint64_t int32s { 0 };

    void add(TypeReader types, TypeID id)
    {
        ++total;
        TypeInLinkCounts kind = classifyForLinkCounts(types, id);
        ++byKind[static_cast<unsigned>(kind)];
        if (kind != TypeInLinkCounts::Primitives)
            return;
        Type type = types.at(id);
        type.primitives &= ~Primitive::Absent;
        numbers += type.isNumber();
        integers += type.isInteger();
        int32s += type.isInt32();
    }
    void print(PrintStream& out, ASCIILiteral what) const
    {
        out.print("counts "_s, what, " | all "_s, total, " | something proven "_s, total - byKind[0]);
        for (unsigned kind = 0; kind < numberOfTypesInLinkCounts; ++kind) {
            if (byKind[kind])
                out.print(" | "_s, namesOfTypesInLinkCounts[kind], " "_s, byKind[kind]);
        }
        out.println(" | of the primitives: numbers "_s, numbers, " | of them integers "_s, integers, " | of them int32 "_s, int32s);
    }
};

struct CellsInLinkCounts {
    TypesInLinkCounts proven;
    uint64_t everyWriterIsKnown { 0 };
    uint64_t isWrittenAfterConstruction { 0 };
    uint64_t provenAndEveryWriterIsKnown { 0 };
    uint64_t isFilled { 0 };
    uint64_t isFilledAndProven { 0 };
    uint64_t isFilledAtEveryRead { 0 };
    uint64_t isOfBirth { 0 };

    void add(TypeReader types, const LinkedCell& cell)
    {
        proven.add(types, cell.proven);
        everyWriterIsKnown += cell.everyWriterIsKnown;
        isWrittenAfterConstruction += cell.isWrittenAfterConstruction;
        provenAndEveryWriterIsKnown += cell.everyWriterIsKnown && cell.proven;
        isFilled += cell.isFilled;
        isFilledAndProven += cell.isFilled && cell.proven;
        isFilledAtEveryRead += cell.isFilledAtEveryRead;
        isOfBirth += cell.isOfBirth;
    }
    void print(PrintStream& out, ASCIILiteral what) const
    {
        proven.print(out, what);
        out.println("counts "_s, what, " | of the birth "_s, isOfBirth, " | filled "_s, isFilled);
        out.println("counts "_s, what, " | every writer known "_s, everyWriterIsKnown, " | of them with something proven "_s, provenAndEveryWriterIsKnown, " | written after construction "_s, isWrittenAfterConstruction, " | at construction alone "_s, proven.total - isWrittenAfterConstruction, " | filled "_s, isFilled, " | of them with something proven "_s, isFilledAndProven, " | filled at every read "_s, isFilledAtEveryRead);
    }
};

static void printLinkCounts(PrintStream& out, const Program& program, const Facts& facts, const LinkTables& tables)
{
    const ThinSummaries& summaries = facts.summaries();
    TypeReader types(program);

    uint64_t withBody = 0, reached = 0, callersKnown = 0, oneCallee = 0, callersKnownAndOneCallee = 0, takesArrivals = 0;
    uint64_t returnsAnything = 0, returnsNothing = 0, coroutines = 0;
    uint64_t operationsOfAll = 0, operationsOfReached = 0, nodes = 0, edges = 0, edgesThroughTests = 0, escapes = 0, subjectsOfComputedKeys = 0, operators = 0, operandsConverted = 0;
    TypesInLinkCounts results, resultsOfCallersKnown, parameters, parametersOfCallersKnown, receivers;
    uint64_t expected = 0, expectedDeclared = 0, expectedAndNothingProven = 0;
    static constexpr unsigned numberOfCalleeKinds = 4;
    static constexpr ASCIILiteral namesOfCalleeKinds[numberOfCalleeKinds] { "by binding"_s, "by name"_s, "by value"_s, "built-in"_s };
    uint64_t calls[numberOfCalleeKinds] { }, callsWithOneCallee[numberOfCalleeKinds] { }, callsInLoops[numberOfCalleeKinds] { }, callsInLoopsWithOneCallee[numberOfCalleeKinds] { }, constructions[numberOfCalleeKinds] { };
    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> sizesOfCycles;
    uint32_t highestRank = 0;

    for (uint32_t index = 0; index < tables.functions.size(); ++index) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (!function.hasBody())
            continue;
        ++withBody;
        const ThinSummary& summary = summaries.at(id);
        operationsOfAll += summary.numberOfOperations;
        const LinkedFunction& linked = tables.function(id);
        if (!linked.isReached)
            continue;
        ++reached;
        operationsOfReached += summary.numberOfOperations;
        nodes += summary.nodes.size();
        edges += summary.edges.size();
        for (const SummaryEdge& edge : summary.edges)
            edgesThroughTests += !!edge.passes;
        for (const SummaryEdge& edge : summary.edges)
            escapes += !edge.to;
        for (const SummaryAccess& access : summary.accesses)
            subjectsOfComputedKeys += !access.key && !access.isToContents();
        operators += summary.operators.size();
        for (const SummaryOperator& operation : summary.operators) {
            for (uint32_t converted : operation.converted)
                operandsConverted += converted != noSummaryNode;
        }
        callersKnown += linked.everyCallerIsKnown;
        oneCallee += linked.everyCallHasOneCallee;
        callersKnownAndOneCallee += linked.everyCallerIsKnown && linked.everyCallHasOneCallee;
        coroutines += function.isCoroutine();
        returnsAnything += linked.effectsOnReturn() == Effects { };
        returnsNothing += linked.effectsOnReturn().isPure();
        ++sizesOfCycles.add(linked.cycle, 0).iterator->value;
        highestRank = std::max(highestRank, tables.rankOfCycle[linked.cycle]);
        takesArrivals += linked.everyCallerIsKnown && linked.everyCallHasOneCallee && !linked.isNamedInItsCycle;

        results.add(types, linked.result);
        if (linked.everyCallerIsKnown)
            resultsOfCallersKnown.add(types, linked.result);
        std::span<const LinkedParameter> linkedParameters = tables.parametersOf(id);
        for (uint32_t parameter = 0; parameter < linkedParameters.size(); ++parameter) {
            const LinkedParameter& one = linkedParameters[parameter];
            switch (function.entryParameterKind(parameter)) {
            case EntryParameter::This:
                receivers.add(types, one.proven);
                break;
            case EntryParameter::Parameter:
                parameters.add(types, one.proven);
                if (linked.everyCallerIsKnown)
                    parametersOfCallersKnown.add(types, one.proven);
                expected += !!one.writtenOrSeenToArrive();
                expectedDeclared += !!one.written;
                expectedAndNothingProven += one.writtenOrSeenToArrive() && !one.proven;
                break;
            default:
                break;
            }
        }

        for (const SummaryCall& call : summary.calls) {
            unsigned kind = static_cast<unsigned>(call.callee);
            bool hasOneCallee = !!tables.calleeAt(id, call.site);
            ++calls[kind];
            callsWithOneCallee[kind] += hasOneCallee;
            callsInLoops[kind] += call.isInLoop;
            callsInLoopsWithOneCallee[kind] += call.isInLoop && hasOneCallee;
            constructions[kind] += call.isConstruction;
        }
    }

    uint32_t largestCycle = 0;
    uint64_t functionsAloneInCycle = 0;
    for (uint32_t size : sizesOfCycles.values()) {
        largestCycle = std::max(largestCycle, size);
        functionsAloneInCycle += size == 1;
    }

    out.println("counts functions | read "_s, tables.functions.size(), " | with a body "_s, withBody, " | reached "_s, reached, " | every caller known "_s, callersKnown, " | every call has one callee "_s, oneCallee, " | both "_s, callersKnownAndOneCallee, " | both and no referrer in its cycle (takes arrivals) "_s, takesArrivals);
    out.println("counts functions reached | coroutines "_s, coroutines, " | effects on return: anything "_s, returnsAnything, " | none "_s, returnsNothing, " | cycles "_s, sizesOfCycles.size(), " | alone in its cycle "_s, functionsAloneInCycle, " | the largest cycle "_s, largestCycle, " | the highest rank "_s, highestRank);
    out.println("counts summaries | operations of all bodies "_s, operationsOfAll, " | of reached bodies "_s, operationsOfReached, " | nodes "_s, nodes, " | edges "_s, edges, " | of them through a test "_s, edgesThroughTests, " | escapes "_s, escapes, " | subjects of computed keys "_s, subjectsOfComputedKeys, " | operators "_s, operators, " | of their operands may be objects "_s, operandsConverted);
    const SizeOfLinkFlow& flow = tables.sizeOfFlow;
    out.println("counts flow | nodes "_s, flow.nodes, " | edges "_s, flow.edges, " | observers "_s, flow.observers, " | nodes taken off the worklist "_s, flow.nodesTakenOffWorklist, " | observers told "_s, flow.observersTold);
    out.println("counts cells of fields expected by the names asked of what lies in them | "_s, tables.numberOfCellsExpectedByNamesAsked);
    out.println("counts flow ties of the rows of lost functions | all "_s, flow.tiesOfPools.all, " | of rows of unequal length "_s, flow.tiesOfPools.ofRowsOfUnequalLength, " | behind another of the same settling "_s, flow.tiesOfPools.behindAnotherOfSameSettling, " |"_s);
    parameters.print(out, "parameters of reached functions"_s);
    parametersOfCallersKnown.print(out, "parameters where every caller is known"_s);
    out.println("counts parameters of reached functions | expected "_s, expected, " | of them declared "_s, expectedDeclared, " | seen to arrive "_s, expected - expectedDeclared, " | expected and nothing proven "_s, expectedAndNothingProven);
    receivers.print(out, "`this` of reached functions"_s);
    results.print(out, "results of reached functions"_s);
    resultsOfCallersKnown.print(out, "results where every caller is known"_s);

    uint64_t allCalls = 0, allWithOneCallee = 0;
    for (unsigned kind = 0; kind < numberOfCalleeKinds; ++kind) {
        allCalls += calls[kind];
        allWithOneCallee += callsWithOneCallee[kind];
        out.println("counts calls in summaries of reached functions | "_s, namesOfCalleeKinds[kind], " "_s, calls[kind], " | listed with one callee "_s, callsWithOneCallee[kind], " | in loops "_s, callsInLoops[kind], " | of them with one callee "_s, callsInLoopsWithOneCallee[kind], " | constructions "_s, constructions[kind]);
    }
    out.println("counts calls in summaries of reached functions | all "_s, allCalls, " | listed with one callee "_s, allWithOneCallee, " | rows of LinkTables::calls "_s, tables.calls.size());
    out.println("counts who may name a function | groups "_s, tables.groups.size(), " | referrers "_s, tables.referrers.size(), " | cycles, those of groups alone too "_s, tables.rankOfCycle.size(), " | cycles awaited "_s, tables.cyclesAwaited.size());
    uint64_t mayTakeArrivals = 0, takeArrivals = 0, inQueueFromStart = 0, mostFunctionsInOneCycle = 0;
    Vector<uint32_t> reachedFunctionsOfCycle;
    reachedFunctionsOfCycle.fill(0, tables.rankOfCycle.size());
    for (const LinkedFunction& function : tables.functions) {
        if (!function.isReached)
            continue;
        mayTakeArrivals += function.everyCallerIsKnown && function.everyCallHasOneCallee;
        takeArrivals += function.everyCallerIsKnown && function.everyCallHasOneCallee && !function.isNamedInItsCycle;
        inQueueFromStart += function.isInQueueFromStart;
        mostFunctionsInOneCycle = std::max<uint64_t>(mostFunctionsInOneCycle, ++reachedFunctionsOfCycle[function.cycle]);
    }
    out.println("counts callers first | every caller known and every call with one callee "_s, mayTakeArrivals, " | of those take arrivals "_s, takeArrivals, " | in the queue from the start "_s, inQueueFromStart, " | reached functions in the largest cycle "_s, mostFunctionsInOneCycle);

    CellsInLinkCounts fieldsOfObjects, fieldsOfEnvironments, fieldsOfLayoutsNeverAllocated, appendedFields, variables, captures;
    uint64_t layoutsWithAppendedFields = 0, mostAppendedFields = 0;
    uint64_t layoutsOfObjects = 0, allocated = 0, withoutChange = 0, withAllFive = 0, withFamily = 0, environments = 0, environmentsAllocated = 0, madeByRunTimeAlone = 0;
    static constexpr LayoutChange changes[] { LayoutChange::IsFrozen, LayoutChange::GainsProperty, LayoutChange::LosesProperty, LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor };
    uint64_t byChange[5] { };
    uint64_t byNumberOfChanges[6] { };
    for (uint32_t index = 0; index < tables.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        const LinkedLayout& linked = tables.layouts[index];
        std::span<const LinkedCell> cells = tables.fields.span().subspan(tables.firstFieldOfLayout[index], tables.firstFieldOfLayout[index + 1] - tables.firstFieldOfLayout[index]);
        if (layout.isEnvironment()) {
            ++environments;
            environmentsAllocated += linked.isAllocated;
            for (const LinkedCell& cell : cells)
                (linked.isAllocated ? fieldsOfEnvironments : fieldsOfLayoutsNeverAllocated).add(types, cell);
            continue;
        }
        if (layout.kind == LayoutKind::ModuleNamespace || layout.kind == LayoutKind::BuiltinResult) {
            ++madeByRunTimeAlone;
            continue;
        }
        ++layoutsOfObjects;
        for (const LinkedCell& cell : cells)
            (linked.isAllocated ? fieldsOfObjects : fieldsOfLayoutsNeverAllocated).add(types, cell);
        uint32_t numberOfAppendedFields = static_cast<uint32_t>(tables.appendedKeysOf(LayoutID(index)).size());
        layoutsWithAppendedFields += !!numberOfAppendedFields;
        mostAppendedFields = std::max<uint64_t>(mostAppendedFields, numberOfAppendedFields);
        for (uint32_t place = 0; place < numberOfAppendedFields; ++place)
            appendedFields.add(types, tables.appendedFields[tables.firstAppendedFieldOfLayout[index] + place]);
        if (!linked.isAllocated)
            continue;
        ++allocated;
        withFamily += !!linked.family;
        unsigned numberOfChanges = 0;
        for (unsigned change = 0; change < 5; ++change) {
            bool may = linked.possibleChanges.contains(changes[change]);
            byChange[change] += may;
            numberOfChanges += may;
        }
        ++byNumberOfChanges[numberOfChanges];
        withoutChange += !numberOfChanges;
        withAllFive += numberOfChanges == 5;
    }
    for (const LinkedCell& cell : tables.variables)
        variables.add(types, cell);
    for (uint32_t index = 0; index + 1 < tables.firstCaptureOfFunction.size(); ++index) {
        if (!tables.functions[index].isReached)
            continue;
        for (uint32_t capture = tables.firstCaptureOfFunction[index]; capture < tables.firstCaptureOfFunction[index + 1]; ++capture)
            captures.add(types, tables.captures[capture]);
    }

    out.println("counts layouts | of the program's objects "_s, layoutsOfObjects, " | allocated "_s, allocated, " | of them with no possible change "_s, withoutChange, " | with all five "_s, withAllFive, " | with a family "_s, withFamily, " | families "_s, facts.families().size(), " | environments "_s, environments, " | of them allocated "_s, environmentsAllocated, " | made by the run time alone "_s, madeByRunTimeAlone);
    out.println("counts allocated layouts of objects | by number of possible changes: 0: "_s, byNumberOfChanges[0], " | 1: "_s, byNumberOfChanges[1], " | 2: "_s, byNumberOfChanges[2], " | 3: "_s, byNumberOfChanges[3], " | 4: "_s, byNumberOfChanges[4], " | 5: "_s, byNumberOfChanges[5]);
    out.print("counts allocated layouts of objects | may"_s);
    for (unsigned change = 0; change < 5; ++change)
        out.print(" | "_s, nameInFactsText(changes[change]), " "_s, byChange[change]);
    out.println();
    fieldsOfObjects.print(out, "fields of allocated layouts of objects"_s);
    out.println("counts appended fields | layouts that have any "_s, layoutsWithAppendedFields, " | the most of one layout "_s, mostAppendedFields);
    appendedFields.print(out, "appended fields"_s);
    fieldsOfEnvironments.print(out, "fields of allocated environments"_s);
    fieldsOfLayoutsNeverAllocated.print(out, "fields of layouts never allocated"_s);
    variables.print(out, "variables"_s);
    captures.print(out, "captures of reached functions"_s);
}

static void printFactsOfStringsInFactsText(IRPrinter& printer, const Facts& facts)
{
    PrintStream& out = printer.out();
    for (uint32_t index = 0; index < printer.program().layouts.size(); ++index) {
        if (!facts.parsersMayMake(LayoutID(index)))
            continue;
        out.print("parsers-may-make "_s);
        printer.print(LayoutID(index));
        out.println();
    }
    for (uint32_t index = 0; index < printer.program().modules.size(); ++index) {
        if (facts.isPathReadByOperation(ModuleID(index)))
            continue;
        out.print("path-unread "_s);
        printer.print(ModuleID(index));
        out.println();
    }
}

static void printLookupsInFactsText(IRPrinter& printer, const LookupsPastProgramObjects& lookups)
{
    PrintStream& out = printer.out();
    auto printKeys = [&](ASCIILiteral word, const BitVector& keys) {
        for (size_t index : keys) {
            out.print("lookup "_s, word, " "_s);
            printer.print(PropertyKeyID(static_cast<uint32_t>(index)));
            out.println();
        }
    };
    auto printBuiltins = [&](ASCIILiteral word, const BitVector& builtins) {
        for (size_t index : builtins) {
            out.print("lookup "_s, word, " "_s);
            printer.print(BuiltinID(static_cast<uint32_t>(index)));
            out.println();
        }
    };
    auto printFlag = [&](ASCIILiteral word, bool isSet) {
        if (isSet)
            out.println("lookup "_s, word);
    };
    auto printPlace = [&](FunctionID function, ValueID site) {
        out.print(" at "_s);
        printer.print(function);
        if (!site)
            return;
        out.print(" "_s);
        printer.print(site);
    };

    printKeys("name-read"_s, lookups.namesRead);
    printKeys("name-called"_s, lookups.namesCalled);
    printBuiltins("read"_s, lookups.builtinsRead);
    printBuiltins("called"_s, lookups.builtinsCalled);
    printBuiltins("let-go"_s, lookups.builtinsLetGo);
    printBuiltins("called-out-of-sight"_s, lookups.builtinsCalledOutOfSight);
    printBuiltins("taken-for-constant"_s, lookups.membersTakenForConstants);
    printBuiltins("written"_s, lookups.membersWritten);
    for (const WriteFollowedToBuiltin& write : lookups.writesFollowedToBuiltins) {
        out.print("lookup write "_s);
        printer.print(write.holder);
        printPlace(write.function, write.site);
        out.println();
    }
    printBuiltins("written-under-any-name"_s, lookups.holdersWrittenUnderAnyName);
    printBuiltins("lost"_s, lookups.lostBuiltins);
    printBuiltins("at-unseen"_s, lookups.builtinsAtUnseen);
    for (size_t index : lookups.classesWithInstancesAtUnseen) {
        out.print("lookup instance-at-unseen "_s);
        printer.print(BuiltinClassID(static_cast<uint32_t>(index)));
        out.println();
    }
    for (size_t index : lookups.entriesThatHandHostNothing)
        out.println("lookup entry-hands-host-nothing "_s, index);
    printKeys("name-read-through-lost"_s, lookups.namesReadThroughLostBuiltins);
    printKeys("name-written-through-lost"_s, lookups.namesWrittenThroughLostBuiltins);
    printKeys("name-written-through-unseen-by-program"_s, lookups.namesWrittenThroughUnseenByProgram);
    printKeys("name-written-through-unseen-by-library"_s, lookups.namesWrittenThroughUnseenByLibrary);
    printFlag("writes-under-any-name-through-unseen-by-program"_s, lookups.writesUnderAnyNameThroughUnseenByProgram);
    printFlag("writes-under-any-name-through-lost"_s, lookups.writesUnderAnyNameThroughLostBuiltins);
    printFlag("climbs-from-lost"_s, lookups.climbsFromLostBuiltins);
    printFlag("climbs-from-unseen"_s, lookups.climbsFromUnseenObjects);
    printFlag("lost-reach-unseen"_s, lookups.lostBuiltinsReachUnseen);
    printFlag("reads-under-keys-nobody-knows"_s, lookups.readsUnderKeysNobodyKnows);
    printFlag("native-code-may-hold-objects"_s, lookups.nativeCodeMayHoldObjects);
}

static void printLinkOfChainInWhyGone(PrintStream& out, const LinkFlow& flow, FlowNodeID node)
{
    out.print("  "_s);
    flow.describe(out, node);
    LinkType type = flow.typeAt(node);
    out.print(" : "_s, type);
    if (type.objects != LinkType::Objects::None && type.objects != LinkType::Objects::Layout) {
        size_t numberOfLayouts = 0;
        flow.forEachLayoutOf(type, [&](LayoutID) {
            ++numberOfLayouts;
        });
        out.print(" {"_s, numberOfLayouts, " layouts:"_s);
        size_t named = 0;
        flow.forEachLayoutOf(type, [&](LayoutID member) {
            if (named++ < 12)
                out.print(" L"_s, member.index());
        });
        out.print(numberOfLayouts > 12 ? " ..}"_s : "}"_s);
    }
    for (const FlowObserver& observer : flow.observersOf(node))
        out.print(" | told: "_s, observer);
    out.println();
}

static void printWhyGone(PrintStream& out, const LinkFlow& flow, const String& asked)
{
    size_t numberOfNodes = flow.numberOfNodesWithLists();
    Vector<uint32_t> cameFrom;
    cameFrom.fill(UINT32_MAX, numberOfNodes);
    Vector<uint32_t> reached;
    auto reach = [&](uint32_t node, uint32_t from) {
        if (node >= numberOfNodes || cameFrom[node] != UINT32_MAX)
            return;
        cameFrom[node] = from;
        reached.append(node);
    };
    auto printChainFrom = [&](uint32_t node) {
        for (;; node = cameFrom[node]) {
            printLinkOfChainInWhyGone(out, flow, FlowNodeID(node));
            if (cameFrom[node] == node)
                return;
        }
    };

    bool namesLayout = asked.length() > 1 && asked[0] == 'L' && !asked.contains('.');
    if (namesLayout) {
        std::optional<uint32_t> number = parseInteger<uint32_t>(StringView(asked).substring(1));
        if (!number || *number >= flow.program.layouts.size()) {
            out.println("why-gone "_s, asked, ": no such layout"_s);
            return;
        }
        LayoutID layout(*number);
        out.println("why-gone "_s, asked, flow.escapedLayouts.get(*number) ? ": it has gone"_s : ": it has not gone"_s);
        auto holds = [&](uint32_t node) {
            bool isHeld = false;
            flow.forEachLayoutOf(flow.typeAt(FlowNodeID(node)), [&](LayoutID member) {
                isHeld |= member == layout;
            });
            return isHeld;
        };
        for (uint32_t function = 0; function < flow.program.functions.size(); ++function) {
            const ThinSummary& summary = flow.summaries.at(FunctionID(function));
            for (uint32_t node = 0; node < summary.nodes.size(); ++node) {
                if (summary.nodes[node].kind == SummaryNode::Kind::Allocation && summary.nodes[node].first == *number) {
                    uint32_t made = flow.nodeOf(FunctionID(function), node).index();
                    reach(made, made);
                }
            }
        }
        out.println("made at "_s, reached.size(), " nodes"_s);
        for (size_t place = 0; place < reached.size(); ++place) {
            for (FlowNodeID end : flow.endsOfEdgesOutOf(FlowNodeID(reached[place]))) {
                if (end == flow.unseen()) {
                    out.println("an edge leads it to what is unseen; the chain, the root last:"_s);
                    out.println("  unseen"_s);
                    printChainFrom(reached[place]);
                    return;
                }
                if (holds(end.index()))
                    reach(end.index(), reached[place]);
            }
        }
        out.println(flow.escapedLayouts.get(*number) ? "no edge leads it to what is unseen: a rule let it go; "_s : ""_s, "every node that it reaches, "_s, reached.size(), ", with what is told there:"_s);
        for (uint32_t node : reached)
            printLinkOfChainInWhyGone(out, flow, FlowNodeID(node));
        return;
    }

    std::optional<uint32_t> subject;
    for (uint32_t node = 0; node < numberOfNodes && !subject; ++node) {
        StringPrintStream described;
        flow.describe(described, FlowNodeID(node));
        if (described.toString() == asked)
            subject = node;
    }
    if (!subject) {
        out.println("why-gone "_s, asked, ": no node is described so"_s);
        return;
    }
    auto holdsWhatIsUnseen = [&](uint32_t node) {
        LinkType type = flow.typeAt(FlowNodeID(node));
        return type.hasUnknownObjects || type.objects == LinkType::Objects::Lost || type.functions == LinkType::Functions::Lost;
    };
    out.println("why-gone "_s, asked, holdsWhatIsUnseen(*subject) ? ": it holds what is unseen"_s : ": it holds nothing unseen"_s);
    if (!holdsWhatIsUnseen(*subject)) {
        printLinkOfChainInWhyGone(out, flow, FlowNodeID(*subject));
        return;
    }
    Vector<Vector<uint32_t, 2>> comesInto;
    comesInto.grow(numberOfNodes);
    for (uint32_t node = 0; node < numberOfNodes; ++node) {
        for (FlowNodeID end : flow.endsOfEdgesOutOf(FlowNodeID(node))) {
            if (end.index() < numberOfNodes)
                comesInto[end.index()].append(node);
        }
    }
    reach(*subject, *subject);
    uint32_t root = *subject;
    for (size_t place = 0; place < reached.size(); ++place) {
        bool hasSource = false;
        for (uint32_t from : comesInto[reached[place]]) {
            if (!holdsWhatIsUnseen(from))
                continue;
            hasSource = true;
            reach(from, reached[place]);
        }
        if (!hasSource || reached[place] == flow.unseen().index()) {
            root = reached[place];
            break;
        }
    }
    Vector<uint32_t> chain;
    for (uint32_t node = root;; node = cameFrom[node]) {
        chain.append(node);
        if (cameFrom[node] == node)
            break;
    }
    out.println("the chain, the root last:"_s);
    for (size_t place = chain.size(); place--;)
        printLinkOfChainInWhyGone(out, flow, FlowNodeID(chain[place]));
}

static void runLinkCommand(TestCommandContext& context)
{
    Vector<PartOfLinkTables, 9> parts;
    OrderOfWorklist order = OrderOfWorklist::NewestFirst;
    bool printsWhatIsHeld = false;
    bool printsCoroutines = false;
    if (context.arguments.size() >= 2 && context.arguments[0] == "why-gone"_s) {
        context.build.askOfFlowAtRest([&](const LinkFlow& flow) {
            for (size_t place = 1; place < context.arguments.size(); ++place)
                printWhyGone(context.out, flow, context.arguments[place]);
        });
        StagedBuild stage = context.stage(order);
        stage.facts();
        return;
    }
    for (const String& word : context.arguments) {
        if (word == "held"_s) {
            printsWhatIsHeld = true;
            continue;
        }
        if (word == "coroutines"_s) {
            printsCoroutines = true;
            continue;
        }
        if (word == "oldest-first"_s) {
            order = OrderOfWorklist::OldestFirst;
            continue;
        }
        if (word == "scattered"_s) {
            order = OrderOfWorklist::Scattered;
            continue;
        }
        size_t numberOfPartsBefore = parts.size();
        for (const NamedPartOfLinkTables& named : namedPartsOfLinkTables) {
            if (word == named.name)
                parts.append(named.part);
        }
        if (parts.size() == numberOfPartsBefore) {
            context.out.println("bad argument "_s, word);
            return;
        }
    }
    bool namesSomeTable = false;
    for (const String& word : context.arguments)
        namesSomeTable |= word != "held"_s && word != "oldest-first"_s && word != "scattered"_s;
    printsCoroutines |= !namesSomeTable;
    bool namesNoPart = namesSomeTable && parts.isEmpty();
    if (parts.isEmpty()) {
        for (const NamedPartOfLinkTables& named : namedPartsOfLinkTables) {
            if (named.part != PartOfLinkTables::Counts)
                parts.append(named.part);
        }
    }

    if (namesNoPart)
        parts.clear();
    StagedBuild stage = context.stage(order);
    const Facts* facts = stage.facts();
    if (!facts)
        return;
    const LinkTables& tables = printsWhatIsHeld ? facts->held() : facts->linked();

    context.out.print("mechanisms "_s);
    printMechanisms(context.out, facts->mechanisms());
    context.out.println();

    IRPrinter printer(context.out, stage.program());
    if (printsCoroutines)
        printLinkedCoroutines(printer, tables);
    for (PartOfLinkTables part : parts) {
        if (part == lookupsAmongPartsOfLinkTables) {
            printLookupsInFactsText(printer, tables.lookups);
            continue;
        }
        switch (part) {
        case PartOfLinkTables::Functions:
            printLinkedFunctions(printer, tables);
            break;
        case PartOfLinkTables::Calls:
            printLinkedCalls(printer, tables);
            printLinkedCallsThatFindBuiltins(printer, tables);
            break;
        case PartOfLinkTables::Accesses:
            printLinkedAccesses(printer, tables);
            break;
        case PartOfLinkTables::Fields:
            printLinkedFields(printer, tables);
            break;
        case PartOfLinkTables::Variables:
            printLinkedVariables(printer, tables);
            break;
        case PartOfLinkTables::Captures:
            printLinkedCaptures(printer, tables);
            break;
        case PartOfLinkTables::Members:
            printLinkedMembers(printer, tables);
            break;
        case PartOfLinkTables::Layouts:
            printLinkedLayouts(printer, tables);
            printFactsOfStringsInFactsText(printer, *facts);
            break;
        case PartOfLinkTables::Modules:
            printLinkedModules(printer, tables);
            break;
        case PartOfLinkTables::Families:
            printFamiliesInFactsText(printer, facts->families());
            break;
        case PartOfLinkTables::Counts:
            printLinkCounts(context.out, stage.program(), *facts, tables);
            break;
        }
    }
}

static void runLocksCommand(TestCommandContext& context)
{
    StagedBuild stage = context.stage();
    const Facts* facts = stage.facts();
    if (!facts)
        return;
    IRPrinter printer(context.out, stage.program());
    for (size_t index : facts->builtinMembersLockedAtStartUp()) {
        BuiltinID member(static_cast<uint32_t>(index));
        context.out.print("locked "_s);
        printer.print(member);
        context.out.println(facts->builtinLocation(member) == BuiltinLocationState::Constant ? " constant"_s : ""_s, facts->linked().lookups.membersTakenForConstants.get(index) ? " link"_s : ""_s);
    }
    bool printsOpenMembers = std::ranges::any_of(context.arguments, [](const String& word) {
        return word == "open"_s;
    });
    if (!printsOpenMembers)
        return;
    auto nameOf = [](ReasonBuiltinStaysOpen reason) -> ASCIILiteral {
        switch (reason) {
        case ReasonBuiltinStaysOpen::None:
            break;
        case ReasonBuiltinStaysOpen::NoLockIsLaid:
            return "no-lock-is-laid"_s;
        case ReasonBuiltinStaysOpen::IsOpenToDebugger:
            return "open-to-debugger"_s;
        case ReasonBuiltinStaysOpen::IsLeftOpenForTests:
            return "left-open-for-tests"_s;
        case ReasonBuiltinStaysOpen::IsWrittenUnderItsName:
            return "written-under-its-name"_s;
        case ReasonBuiltinStaysOpen::HolderIsWrittenUnderAnyName:
            return "holder-written-under-any-name"_s;
        case ReasonBuiltinStaysOpen::IsWrittenThroughLostBuiltins:
            return "written-through-lost-built-ins"_s;
        case ReasonBuiltinStaysOpen::IsSpelledThroughUnseen:
            return "spelled-through-unseen"_s;
        case ReasonBuiltinStaysOpen::HolderIsLetGoByProgram:
            return "holder-let-go-by-program"_s;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    for (const BuiltinLeftOpen& open : facts->builtinMembersLeftOpen()) {
        context.out.print("open "_s);
        printer.print(open.member);
        context.out.println(" "_s, nameOf(open.reason));
    }
}

static void runRegExpTextsCommand(TestCommandContext& context)
{
    StagedBuild stage = context.stage();
    uint64_t identifierInFront = VMIdentifier::generate().toUInt64();
    const Facts* facts = stage.facts();
    if (!facts)
        return;
    auto isAsked = [&](ASCIILiteral asked) {
        return std::ranges::any_of(context.arguments, [&](const String& word) {
            return word == asked;
        });
    };
    bool hashes = isAsked("hashes"_s);
    if (isAsked("realms"_s))
        context.out.println("realms "_s, VMIdentifier::generate().toUInt64() - identifierInFront - 1);
    auto print = [&](StringView pattern, uint16_t flags, bool isOneOfAll) {
        Yarr::FlagsString written = Yarr::flagsString(OptionSet<Yarr::Flags>::fromRaw(flags));
        if (hashes)
            context.out.print("text "_s, pattern.length(), " "_s, pattern.hash());
        else
            context.out.print("text /"_s, pattern, "/"_s);
        context.out.println(hashes ? " "_s : ""_s, StringView(written.span()), isOneOfAll ? ""_s : " with flags that nobody knows"_s);
    };
    for (const RegExpTextInSight& text : facts->regExpTextsInSight()) {
        Vector<char16_t> buffer;
        print(stage.program().strings.viewOf(text.pattern, buffer), text.flags, text.isOneOfAllAtSomeCall);
    }
    for (const RegExpTextWorkedOut& text : facts->regExpTextsWorkedOut())
        print(text.pattern, text.flags, text.isOneOfAllAtSomeCall);
}

static void runLocalesCommand(TestCommandContext& context)
{
    StagedBuild stage = context.stage();
    const Facts* facts = stage.facts();
    if (!facts)
        return;
    IRPrinter printer(context.out, stage.program());
    for (const LocaleUse& use : facts->localeUses()) {
        switch (use.kind) {
        case LocaleUseKind::Unknown:
            context.out.print("locale unknown"_s);
            break;
        case LocaleUseKind::HostDefault:
            context.out.print("locale host-default"_s);
            break;
        case LocaleUseKind::Constant:
            context.out.print("locale constant "_s);
            printer.print(use.tag);
            break;
        }
        if (use.builtin) {
            context.out.print(" "_s);
            printer.print(use.builtin);
        }
        if (!hasFunction(use.witness))
            context.out.print(" nowhere"_s);
        else {
            context.out.print(" at f"_s, use.witness.function);
            if (use.witness.value != Wire::none)
                context.out.print(" v"_s, use.witness.value);
        }
        context.out.println();
    }
}

static void runCausesCommand(TestCommandContext& context)
{
    StagedBuild stage = context.stage();
    const Facts* facts = stage.facts();
    if (!facts)
        return;
    IRPrinter printer(context.out, stage.program());
    auto print = [&](const UnseenCodeRecord& record) {
        if (record.builtin != Wire::none) {
            context.out.print(" "_s);
            printer.print(BuiltinID(record.builtin));
        }
        if (!hasFunction(record.witness))
            context.out.print(" nowhere"_s);
        else {
            context.out.print(" at f"_s, record.witness.function);
            if (record.witness.value != Wire::none)
                context.out.print(" v"_s, record.witness.value);
        }
        context.out.println();
    };
    for (const UnseenCodeRecord& record : facts->unseenCode()) {
        context.out.print("cause "_s, nameOf(record.cause));
        print(record);
    }
    for (const UnseenCodeRecord& record : facts->loadsOfMachineCode()) {
        context.out.print("machine-code"_s);
        print(record);
    }
    context.out.println("unseen-code "_s, facts->unseenCodeCanRun() ? "can-run"_s : "cannot-run"_s);
    context.out.println("unseen-scripts "_s, facts->unseenScriptsCanRun() ? "can-run"_s : "cannot-run"_s);
}

static void runLackedCommand(TestCommandContext& context)
{
    StagedBuild stage = context.stage();
    const Facts* facts = stage.facts();
    if (!facts)
        return;
    for (RuntimeFunctionID function : facts->missingLibraryFunctions())
        context.out.println("lacked "_s, tokenOf(stage.program().builtins(), function));
}

static void printFeature(PrintStream& out, uint32_t feature)
{
    switch (feature) {
#define JSC_PRINT_REMOVABLE_FEATURE(name, index) \
    case index: \
        out.print(#name##_s); \
        return;
        FOR_EACH_ENGINE_REMOVABLE_FEATURE(JSC_PRINT_REMOVABLE_FEATURE)
#undef JSC_PRINT_REMOVABLE_FEATURE
    }
    out.print(feature);
}

static void runFeaturesCommand(TestCommandContext& context)
{
    StagedBuild stage = context.stage();
    const Facts* facts = stage.facts();
    if (!facts)
        return;
    IRPrinter printer(context.out, stage.program());
    for (const FeatureUseRecord& record : facts->featureUses()) {
        if (record.builtin == Wire::none)
            continue;
        context.out.print("feature "_s);
        printFeature(context.out, record.feature);
        context.out.print(" by "_s);
        printer.print(BuiltinID(record.builtin));
        context.out.print(record.kind == FeatureUseKind::Named ? " named"_s : record.kind == FeatureUseKind::Unnamed ? " unnamed"_s : record.kind == FeatureUseKind::FoundByName ? " found-by-name"_s : " named-at-run-time"_s);
        if (hasFunction(record.witness)) {
            context.out.print(" at f"_s, record.witness.function);
            if (record.witness.value != Wire::none)
                context.out.print(" v"_s, record.witness.value);
        }
        context.out.println();
    }
}

static void runClosureCommand(TestCommandContext& context)
{
    printBreachesOfClosureOfLink(context.out, *context.program, context.build, context.mechanisms);
}

void runFactsCommand(TestCommandContext&);

std::span<const TestCommand> factsTestCommands()
{
    static constexpr TestCommand commands[] {
        { "locks"_s, runLocksCommand, ProgramOfCommand::AsWritten, ""_s },
        { "summary"_s, runSummaryCommand, ProgramOfCommand::AsWritten, "f.."_s },
        { "link"_s, runLinkCommand, ProgramOfCommand::AsWritten, "[functions|calls|accesses|fields|variables|captures|members|layouts|families|counts].. [oldest-first|scattered] | why-gone <L12 | L12.field3 | f7.parameter0>.."_s },
        { "locales"_s, runLocalesCommand, ProgramOfCommand::AsWritten, ""_s },
        { "causes"_s, runCausesCommand, ProgramOfCommand::AsWritten, ""_s },
        { "features"_s, runFeaturesCommand, ProgramOfCommand::AsWritten, ""_s },
        { "lacked"_s, runLackedCommand, ProgramOfCommand::AsWritten, ""_s },
        { "closure"_s, runClosureCommand, ProgramOfCommand::AsWritten, ""_s },
        { "facts"_s, runFactsCommand, ProgramOfCommand::AsWritten, "enforced [counts]"_s },
        { "regexp-texts"_s, runRegExpTextsCommand, ProgramOfCommand::AsWritten, "[hashes] [realms]"_s },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
