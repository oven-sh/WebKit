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
#include "AOTLinkTables.h"
#include "AOTThinSummary.h"
#include "VM.h"
#include "YarrFlags.h"
#include <wtf/CommaPrinter.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

// What the scan and Link have left behind Facts: Facts::summaries(), linked() and families().

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
    { "mayWriteWhatIsSeen"_s, &EffectTraits::mayWriteWhatIsSeen },
    { "mayTouchMemory"_s, &EffectTraits::mayTouchMemory },
    { "mayWriteOutsideObjects"_s, &EffectTraits::mayWriteOutsideObjects },
};

// `anything` for effects that claim nothing, `none` for pure ones, else what Code text lists as the traits of an operation.
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

// How many of the 64 places of the fields of objects the effects write. `all`: the entire heap. Nothing is printed where none is written, or where any code may run.
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

// `none`, `any`, or how many of the 64 bits of layouts are set.
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

// `L0.1 "x"`, and the numbers alone for a field that the layout does not declare.
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

// `L0 Prototype 0`
static void printMemberInFactsText(IRPrinter& printer, LayoutID classLayout, ClassSide side, uint32_t index)
{
    printer.print(classLayout);
    printer.out().print(" "_s);
    printer.print(side);
    printer.out().print(" "_s, index);
}

// ` Method "size"`, ` Getter [computed]`
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

// `Element`, `Key`, `Value`, `field 1`, `"value"`, `state 12`.
static void printSlotOfContents(IRPrinter& printer, SlotOfContents slot)
{
    PrintStream& out = printer.out();
    if (std::optional<ContentsSlot> listed = slot.contentsSlot())
        printer.print(*listed);
    else if (std::optional<uint32_t> field = slot.indexOfInternalField())
        out.print("field "_s, *field);
    else if (slot == SlotOfContents::yieldedAlone())
        out.print("yielded"_s);
    else if (slot == SlotOfContents::returnedAlone())
        out.print("returned"_s);
    else if (slot.isUnderName())
        printer.print(slot.key());
    else
        out.print("state "_s, slot.number());
}

// A site is named by the number of the value that makes the object: `ClassInstance Array #3`. The object that a coroutine hands its caller has none.
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

// How an edge or a record names a node, so that no check line hangs on the number that the scan gave it:
// what is seen from outside and the sources by what they are (`Parameter 0`, `Field L0.1 "x"`, `Closure f2`), a result by its record (`c0.1`, `a2`, `o3`, `converted-c4` by the first call of the conversion), a join by its number (`n7`).
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

// Nothing where there is no node.
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
    if (call.resultIsOnlyIterated)
        out.print(" only-iterated"_s);
    printNamedNodeOfSummary(printer, summary, "throws-to="_s, call.thrownTo);
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
        printNamedNodeOfSummary(printer, summary, "else "_s, access.whatHoldsNothing);
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
    if (access.beginningOfKey) {
        out.print(" begins-with "_s);
        printer.print(access.beginningOfKey);
    }
    if (access.isPartOfConstruction)
        out.print(" at-construction"_s);
    if (access.liesOnNoWayToReturn)
        out.print(" on-no-way-to-return"_s);
    if (access.storesObjectMadeForIt)
        out.print(" made-for-it"_s);
    if (access.laysAccessorAsWritten) {
        out.print(" accessor-as-written"_s);
        if (access.writtenGetter) {
            out.print(" getter="_s);
            printer.print(access.writtenGetter);
        }
        if (access.writtenSetter) {
            out.print(" setter="_s);
            printer.print(access.writtenSetter);
        }
        if (access.mapOfWrittenAccessor) {
            out.print(" in-map="_s);
            printer.print(access.mapOfWrittenAccessor);
        }
    }
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
    out.println(")"_s);
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
    printNamedNodeOfSummary(printer, summary, "getter-in-sight="_s, change.getterInSight);
    printNamedNodeOfSummary(printer, summary, "setter-in-sight="_s, change.setterInSight);
    if (change.lays != SummaryChange::Lays::WhatDescriptorSays)
        out.print(change.lays == SummaryChange::Lays::Getter ? " getter"_s : " setter"_s);
    if (change.liesOnNoWayToReturn)
        out.print(" on-no-way-to-return"_s);
    if (change.definesWhatItCopies)
        out.print(" defines"_s);
    out.println();
}

// The records stand in front of the edges, so that a check line can bind a record (`access [[read]] v1 Read Parameter 0 "x"`) and then follow what it gives (`edge [[read]] -> Result 0`).
//     summary f1 operations=4 traits=[HasLoops] effects=[mayThrow, mayCollect]
//       n0 Unknown
//       n1 Parameter 0
//       n2 Capture 0 of f3
//       n3 Member L0 Static 1
//       n4 Join
//       call c0 v5 Name "size" this=Parameter 0 (Constant string) in-loop
//       call c1 v6 Value Closure f2 new-target=Parameter 1 (n4) constructs
//       call c2 v7 Binding f2 (Parameter 0, c0.0)
//       call c3 v8 Builtin ObjectFreeze (Parameter 0)
//       access a0 v1 Read Parameter 0 "x" -> a0
//       access a1 v2 Store a0 [Parameter 1] <- Constant string
//       access a2 v3 Define Parameter 0 "y" <- Closure f2 at-construction
//       access a3 v4 Has Parameter 0 "x"
//       operator o0 v10 Add (Parameter 0 or converted-c4, Constant int32)
//       change h0 v8 Freezes Parameter 0
//       change h1 v9 DefinesByDescriptor Parameter 0 "x" other=Parameter 1
//       member L0 Prototype 0 Method "size" = Closure f2
//       member L0 Static 1 Getter [computed] = Closure f4
//       construction AnsweredBySuperCall fills ["b", "c"] looks-up ["c", "d"] parts [f3, f4 on-some-ways] seen-from-outside
//       leaves-empty L2.1 "y"
//       classes L0
//       hands-out m1
//       names-where-no-run-comes m2
//       never-entered b3, b4
//       still-being-made v9 = v2
//       handler b3 caught=n7
//       edge Parameter 1 -> Field L0.1 "x"
//       edge a0 -> Result 0 passes=string
//       edge Parameter 0 -> Unknown
// The last is an escape.
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
    printEffectsInFactsText(out, summary.ownEffects);
    if (summary.knownEffectsApart.ownWritesOnWaysToReturn())
        out.print(" writes-less-on-return"_s);
    if (const Effects* ofOperators = summary.knownEffectsApart.ownEffectsOfOperators()) {
        out.print(" operators="_s);
        printEffectsInFactsText(out, *ofOperators);
    }
    if (summary.operatorsLeftApartMayJoinTexts)
        out.print(" may-join-texts"_s);
    if (summary.someBlockHasHandler)
        out.print(" has-handler"_s);
    if (summary.layoutsOfOwnWrites)
        printLayoutsWrittenInFactsText(out, "own-writes-in"_s, summary.layoutsOfOwnWrites);
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
    if (std::span<const uint32_t> operands = summary.knownEffectsApart.operandsOfOperatorsLeftApart(); !operands.empty()) {
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
        // Whatever is filled wherever the object leaves is filled at the end: the keys are printed of which the end says more.
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
    for (FieldReference field : summary.fieldsLeftEmpty) {
        out.print("  leaves-empty "_s);
        printer.print(field.layout);
        out.print("."_s, field.field, " "_s);
        printer.print(fieldAt(printer.program(), field).key);
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
    if (!summary.nodesWhoseKeysAreListed.isEmpty()) {
        out.print("  lists-keys-of"_s);
        CommaPrinter comma(", "_s, " "_s);
        for (uint32_t node : summary.nodesWhoseKeysAreListed)
            out.print(comma, "n"_s, node);
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

// ---- link

enum class PartOfLinkTables : uint8_t { Functions, Calls, Accesses, Fields, Variables, Captures, Members, Layouts, Families, Counts };

// LookupsPastProgramObjects. It stands behind the enumerators.
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

// Behind the name of the cell. A cell of which nothing is claimed has nothing behind its name.
static void printLinkedCell(IRPrinter& printer, const LinkedCell& cell)
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
    // Whatever is filled is of the birth: the word is printed where it says more.
    if (cell.isOfBirth && !cell.isFilled)
        printer.out().print(" of-birth"_s);
    if (cell.isFilledByAccessorsAlone)
        printer.out().print(" filled-by-accessors-alone"_s);
    if (cell.holdsAccessorFromBirth)
        printer.out().print(" accessor-from-birth"_s);
    if (cell.isOnlyGivenUndefinedByMakers)
        printer.out().print(" only-given-undefined"_s);
    if (!cell.formThatDefinitionsCreate.isEmpty()) {
        printer.out().print(" form="_s);
        for (auto [flag, word] : { std::pair { FieldForm::IsReadOnly, "read-only"_s }, std::pair { FieldForm::IsNotEnumerable, "not-enumerable"_s }, std::pair { FieldForm::IsNotConfigurable, "not-configurable"_s }, std::pair { FieldForm::IsAccessor, "accessor"_s } }) {
            if (cell.formThatDefinitionsCreate.contains(flag))
                printer.out().print(word, "."_s);
        }
    }
    printer.out().println();
}

// f3 | anybody | constructors of F2 | shared call 0
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

//     function f1 callers-known one-callee fresh-result cycle=2 rank=1 result=number thrown=never returns=[mayCollect] throws=none
//       parameter 0 expected=number declared proven=int kept
//       referrers f0, f3, shared call 0
//     function f2 unreached cycle=0 rank=0 returns=anything throws=anything
//     catch f1 b3 caught=L0
//     group shared call 0 cycle=1 rank=0
//       referrers f0
// The parameters are the entry parameters: captures, this, the callee and the new target come in front of what is written. `kept`: the parameter does not escape.
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
        if (function.resultIsFresh)
            out.print(" fresh-result"_s);
        if (function.everyEntryIsConstruction)
            out.print(" only-constructed"_s);
        if (function.closuresAreChanged)
            out.print(" closures-changed"_s);
        out.print(" cycle="_s, function.cycle, " rank="_s, function.rank);
        printTypeFieldInFactsText(printer, "result"_s, function.result);
        printTypeFieldInFactsText(printer, "thrown"_s, function.thrown);
        out.print(" returns="_s);
        printEffectsInFactsText(out, function.effectsOnReturn);
        out.print(" throws="_s);
        printEffectsInFactsText(out, function.effectsOnThrow);
        printFieldsWrittenInFactsText(out, "fields-written-on-return"_s, function.effectsOnReturn);
        printFieldsWrittenInFactsText(out, "fields-written"_s, function.effectsOnThrow);
        if (!tables.known.layoutsWrittenByFunction.isEmpty() && !function.effectsOnThrow.mayCallUnknownCode)
            printLayoutsWrittenInFactsText(out, "layouts-written"_s, tables.known.layoutsWrittenBy(id));
        if (function.runsAtMostOnce)
            out.print(" runs-once"_s);
        if (function.isReached && !function.ringsOfTailCallsAreListed)
            out.print(" ring-by-value"_s);
        if (function.isInQueueFromStart)
            out.print(" in-queue-from-start"_s);
        if (function.everyCallerIsKnown && function.everyCallHasOneCallee && !function.isNamedInItsCycle)
            out.print(" takes-arrivals"_s);
        if (function.closuresStayAsMade)
            out.print(" closures-stay-as-made"_s);
        if (function.weightAsLeaf != LinkedFunction::notLeaf)
            out.print(" leaf="_s, function.weightAsLeaf);
        out.println();

        std::span<const LinkedParameter> parameters = tables.parametersOf(id);
        for (uint32_t parameter = 0; parameter < parameters.size(); ++parameter) {
            const LinkedParameter& linked = parameters[parameter];
            out.print("  parameter "_s, parameter);
            printTypeFieldInFactsText(printer, "expected"_s, linked.expected);
            if (linked.expectedIsDeclared)
                out.print(" declared"_s);
            printTypeFieldInFactsText(printer, "proven"_s, linked.proven);
            if (!linked.escapes)
                out.print(" kept"_s);
            if (linked.staysAsItsClassMakesIt)
                out.print(" stays-as-made"_s);
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
        out.println(" cycle="_s, tables.groups[index].cycle, " rank="_s, tables.groups[index].rank);
        printReferrersInFactsText(printer, tables, node);
    }
}

//     call f0 v5 -> f1
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

//     call f1 v9 finds MathAbs beside f3
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

//     access f1 v2 under symbol("depth")@m0:0
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
    //     read f1 v43 proven=undefined|L0
    for (const LinkedResultOfRead& read : tables.resultsOfReadsByKey) {
        printer.out().print("read "_s);
        printer.print(read.function);
        printer.out().print(" "_s);
        printer.print(read.site);
        printer.out().print(" proven="_s);
        printer.print(read.proven);
        printer.out().println();
    }
    //     receiver-as-made f1 v43
    for (const LinkedReceiverAsMade& receiver : tables.receiversThatStayAsMade) {
        printer.out().print("receiver-as-made "_s);
        printer.print(receiver.function);
        printer.out().print(" "_s);
        printer.print(receiver.site);
        printer.out().println();
    }
    //     iterable-as-made f1 v43
    for (const LinkedReceiverAsMade& iterable : tables.iterablesThatStayAsMade) {
        printer.out().print("iterable-as-made "_s);
        printer.print(iterable.function);
        printer.out().print(" "_s);
        printer.print(iterable.site);
        printer.out().println();
    }
    //     accessors L0.0 "total" getter=f2 setter=f3
    for (const LinkedAccessorsOfField& expected : tables.accessorsExpectedInFields) {
        printer.out().print("accessors "_s);
        printFieldInFactsText(printer, expected.field.layout, expected.field.field);
        if (expected.getter) {
            printer.out().print(" getter="_s);
            printer.print(expected.getter);
            if (expected.getterIsProven)
                printer.out().print(" proven"_s);
        }
        if (expected.setter) {
            printer.out().print(" setter="_s);
            printer.print(expected.setter);
            if (expected.setterIsProven)
                printer.out().print(" proven"_s);
        }
        printer.out().println();
    }
    //     read f1 v2 expects getter f3
    for (const LinkedGetterAtRead& read : tables.gettersExpectedAtReads) {
        printer.out().print("read "_s);
        printer.print(read.function);
        printer.out().print(" "_s);
        printer.print(read.site);
        printer.out().print(" expects getter "_s);
        printer.print(read.getter);
        printer.out().println();
    }
}

//     coroutine f1 fulfilled=L0 returned=L0 as-made
//     coroutine f2 yielded=i32 sent=undefined as-made only-iterated
static void printLinkedCoroutines(IRPrinter& printer, const LinkTables& tables)
{
    for (const LinkedCoroutine& coroutine : tables.coroutines) {
        printer.out().print("coroutine "_s);
        printer.print(coroutine.function);
        printTypeFieldInFactsText(printer, "fulfilled"_s, coroutine.fulfilledWith);
        printTypeFieldInFactsText(printer, "returned"_s, coroutine.returned);
        printTypeFieldInFactsText(printer, "yielded"_s, coroutine.yielded);
        printTypeFieldInFactsText(printer, "sent"_s, coroutine.sent);
        if (coroutine.objectsStayAsMade)
            printer.out().print(" as-made"_s);
        if (coroutine.objectsAreOnlyIterated)
            printer.out().print(" only-iterated"_s);
        printer.out().println();
    }
}

//     field L0.1 "x" proven=number writers-known written-at-construction-alone
//     field L0.2 "y" appended proven=number writers-known
static void printLinkedFields(IRPrinter& printer, const LinkTables& tables)
{
    for (uint32_t layout = 0; layout + 1 < tables.firstFieldOfLayout.size(); ++layout) {
        uint32_t numberOfFields = tables.firstFieldOfLayout[layout + 1] - tables.firstFieldOfLayout[layout];
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            printer.out().print("field "_s);
            printFieldInFactsText(printer, LayoutID(layout), field);
            printLinkedCell(printer, tables.fields[tables.firstFieldOfLayout[layout] + field]);
        }
        std::span<const PropertyKeyID> appendedKeys = tables.appendedKeysOf(LayoutID(layout));
        for (uint32_t place = 0; place < appendedKeys.size(); ++place) {
            printer.out().print("field "_s);
            printFieldInFactsText(printer, LayoutID(layout), numberOfFields + place);
            printer.out().print(" "_s);
            printer.print(appendedKeys[place]);
            printer.out().print(" appended"_s);
            printLinkedCell(printer, *tables.findCellOfField({ LayoutID(layout), numberOfFields + place }));
        }
    }
}

//     variable g0 proven=f1 writers-known
static void printLinkedVariables(IRPrinter& printer, const LinkTables& tables)
{
    for (uint32_t index = 0; index < tables.variables.size(); ++index) {
        printer.out().print("variable "_s);
        printer.print(VariableID(index));
        printLinkedCell(printer, tables.variables[index]);
    }
}

//     capture f2.0 proven=env L1
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

//     member L0 Prototype 0 Method "size" function=f2 writers-known
//     member L0 Static 1 Getter [computed]
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
    //     on-prototype L0 "size" function=f2
    //     on-prototype L0 "area" expected=f3
    //     on-prototype L0 "width" getter=f4 setter=f5
    //     on-prototype L0 [some key]
    //     on-function f1 "of" function=f4 laid-ahead
    for (const LinkedNameOnFunction& name : tables.namesOnFunctions) {
        out.print("on-function "_s);
        printer.print(name.holder);
        out.print(" "_s);
        printer.print(name.key);
        if (name.function) {
            out.print(" function="_s);
            printer.print(name.function);
        }
        out.print("\n"_s);
    }
    for (const LinkedNameOnPrototype& name : tables.namesOnPrototypes) {
        out.print("on-prototype "_s);
        printer.print(name.layout);
        if (name.key) {
            out.print(" "_s);
            printer.print(name.key);
        } else
            out.print(" [some key]"_s);
        if (name.function) {
            out.print(" function="_s);
            printer.print(name.function);
        } else if (name.expected) {
            out.print(" expected="_s);
            printer.print(name.expected);
        }
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

//     layout L0 family=F1 changes=[GainsProperty]
//     layout L3 never-allocated changes=[]
//     layout L2 changes=[GainsProperty] filled-under-computed-keys
//     layout L4 changes=[] heritage=L2
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
        printFlagsInFactsText(out, layout.possibleChanges);
        if (!layout.reachesUnseenCode)
            out.print(" stays-with-compiled-code"_s);
        if (layout.isRedefinedByLiteralsAlone)
            out.print(" redefined-by-literals-alone"_s);
        if (layout.isFilledUnderComputedKeys)
            out.print(" filled-under-computed-keys"_s);
        if (!layout.keysMayBeListed && !layout.reachesUnseenCode)
            out.print(" keys-never-listed"_s);
        if (layout.onlyOwnObjectsStandBelowPrototype)
            out.print(" own-objects-below"_s);
        if (layout.prototypeObjectIsSettledAheadOfEveryObject)
            out.print(" prototype-object-settled"_s);
        if (layout.heritage) {
            out.print(" heritage="_s);
            printer.print(layout.heritage);
        }
        out.println();
    }
}

//     family F1 [L0, L2]
static void printFamiliesInFactsText(IRPrinter& printer, const Families& families)
{
    for (uint32_t index = 0; index < families.size(); ++index) {
        printer.out().print("family F"_s, index, " "_s);
        printer.print(families.layoutsOf(FamilyID(index)));
        printer.out().println();
    }
}

// ---- link counts: how much Link has proven of a whole program. Numbers alone: no name of the program is printed.

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
    // The marker of an empty cell counts as undefined does.
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
    // Among the primitives. Each holds the next.
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
        // The marker of an empty cell is passed over.
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

    uint64_t withBody = 0, reached = 0, callersKnown = 0, oneCallee = 0, callersKnownAndOneCallee = 0, takesArrivals = 0, freshResult = 0;
    uint64_t returnsAnything = 0, returnsNothing = 0, coroutines = 0;
    uint64_t operationsOfAll = 0, operationsOfReached = 0, nodes = 0, edges = 0, edgesThroughTests = 0, escapes = 0, subjectsOfComputedKeys = 0, operators = 0, operandsConverted = 0;
    TypesInLinkCounts results, resultsOfCallersKnown, thrown, parameters, parametersOfCallersKnown, receivers;
    uint64_t expected = 0, expectedDeclared = 0, expectedAndNothingProven = 0, kept = 0;
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
        freshResult += linked.resultIsFresh;
        coroutines += function.isCoroutine();
        returnsAnything += linked.effectsOnReturn == Effects { };
        returnsNothing += linked.effectsOnReturn.isPure();
        ++sizesOfCycles.add(linked.cycle, 0).iterator->value;
        highestRank = std::max(highestRank, linked.rank);
        takesArrivals += linked.everyCallerIsKnown && linked.everyCallHasOneCallee && !linked.isNamedInItsCycle;

        results.add(types, linked.result);
        if (linked.everyCallerIsKnown)
            resultsOfCallersKnown.add(types, linked.result);
        thrown.add(types, linked.thrown);
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
                expected += !!one.expected;
                expectedDeclared += one.expected && one.expectedIsDeclared;
                expectedAndNothingProven += one.expected && !one.proven;
                kept += !one.escapes;
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
    out.println("counts functions reached | coroutines "_s, coroutines, " | fresh result "_s, freshResult, " | effects on return: anything "_s, returnsAnything, " | none "_s, returnsNothing, " | cycles "_s, sizesOfCycles.size(), " | alone in its cycle "_s, functionsAloneInCycle, " | the largest cycle "_s, largestCycle, " | the highest rank "_s, highestRank);
    out.println("counts summaries | operations of all bodies "_s, operationsOfAll, " | of reached bodies "_s, operationsOfReached, " | nodes "_s, nodes, " | edges "_s, edges, " | of them through a test "_s, edgesThroughTests, " | escapes "_s, escapes, " | subjects of computed keys "_s, subjectsOfComputedKeys, " | operators "_s, operators, " | of their operands may be objects "_s, operandsConverted);
    const SizeOfLinkFlow& flow = tables.sizeOfFlow;
    out.println("counts flow | nodes "_s, flow.nodes, " | edges "_s, flow.edges, " | observers "_s, flow.observers, " | nodes taken off the worklist "_s, flow.nodesTakenOffWorklist, " | observers told "_s, flow.observersTold);
    parameters.print(out, "parameters of reached functions"_s);
    parametersOfCallersKnown.print(out, "parameters where every caller is known"_s);
    out.println("counts parameters of reached functions | expected "_s, expected, " | of them declared "_s, expectedDeclared, " | seen to arrive "_s, expected - expectedDeclared, " | expected and nothing proven "_s, expectedAndNothingProven, " | kept (does not escape) "_s, kept);
    receivers.print(out, "`this` of reached functions"_s);
    results.print(out, "results of reached functions"_s);
    resultsOfCallersKnown.print(out, "results where every caller is known"_s);
    thrown.print(out, "thrown by reached functions"_s);

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
        // Namespace objects and the result records of built-ins are no objects of the program's.
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

//     parsers-may-make L0
//     path-unread m1
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

//     lookup name-read "size"
//     lookup called ArrayPrototypePush
//     lookup write Date at f0 v3
// LookupsPastProgramObjects, field by field. The uses of built-ins, the locks and the reports are made of it.
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

// The first line is `mechanisms <list>`: those that Facts was made with. Then the parts in the order of the words.
static void runLinkCommand(TestCommandContext& context)
{
    Vector<PartOfLinkTables, 9> parts;
    OrderOfWorklist order = OrderOfWorklist::NewestFirst;
    bool printsWhatIsHeld = false;
    bool printsCoroutines = false;
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
    // A bare `link` prints the coroutines too. Words that name only tables with a word of their own print none of the parts.
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
        case PartOfLinkTables::Families:
            printFamiliesInFactsText(printer, facts->families());
            break;
        case PartOfLinkTables::Counts:
            printLinkCounts(context.out, stage.program(), *facts, tables);
            break;
        }
    }
}

// ---- locks

//     locked DateNow constant link
//     open DateParse written-under-its-name
// The members that the program locks when it starts, ascending. `constant`: Facts::builtinLocation() says so. `link`: Link has gone by it (LookupsPastProgramObjects::membersTakenForConstants).
// With the word `open`: behind them the members that the program uses, that a lock can be laid on and that stay open, each with the reason.
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

// A line for each text that Facts list for RegExp, those that the program spells in front. `hashes`: of a text only its length and a hash. `realms`: how many VMs were made on the way, in front.
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

static void runClosureCommand(TestCommandContext& context)
{
    printBreachesOfClosureOfLink(context.out, *context.program, context.build, context.mechanisms);
}

// AOTEnforcedFieldsText.cpp.
void runFactsCommand(TestCommandContext&);

std::span<const TestCommand> factsTestCommands()
{
    static constexpr TestCommand commands[] {
        { "locks"_s, runLocksCommand, ProgramOfCommand::AsWritten, ""_s },
        { "summary"_s, runSummaryCommand, ProgramOfCommand::AsWritten, "f.."_s },
        { "link"_s, runLinkCommand, ProgramOfCommand::AsWritten, "[functions|calls|accesses|fields|variables|captures|members|layouts|families|counts].. [oldest-first|scattered]"_s },
        { "closure"_s, runClosureCommand, ProgramOfCommand::AsWritten, ""_s },
        { "facts"_s, runFactsCommand, ProgramOfCommand::AsWritten, "enforced [counts]"_s },
        { "regexp-texts"_s, runRegExpTextsCommand, ProgramOfCommand::AsWritten, "[hashes] [realms]"_s },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
