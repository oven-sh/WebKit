/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkLayouts.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTLinkTypesOnAllThreads.h"
#include "AOTLinkTypesTurnedOnce.h"
#include "AOTLinkUnions.h"
#include "AOTLinkStrings.h"
#include "AOTNameClass.h"
#include "AOTRowsOfLayouts.h"
#include "AOTSort.h"
#include "AOTUnionTaggedMakers.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

static bool variablesAreWrittenOnlyByBodiesInLinkLayouts(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::RuntimeSource:
        return true;
    case ModuleKind::Data:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Functor>
static void forEachVariableSharedWithUnseenCodeInLinkLayouts(const Program& program, const Functor& functor)
{
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        const Module& module = program.at(variable.module);
        if (variable.owner || variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun })
            || !module.body || !variablesAreWrittenOnlyByBodiesInLinkLayouts(module.kind))
            functor(VariableID(index));
    }

    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.variable)
                functor(binding.variable);
        }
    }
}

template<typename Functor>
static void forEachFieldSharedWithEvaluatedTextInLinkLayouts(const Program& program, const Functor& functor)
{
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.layout && binding.field)
                functor(FieldReference { binding.layout, *binding.field });
        }
    }
}

template<typename Functor>
static void forEachLayoutMadeAtNoNodeInLinkLayouts(const Program& program, const Functor& functor)
{
    Vector<const DataNode*, 32> nodesToVisit;
    for (const Module& module : program.modules) {
        if (!module.data)
            continue;
        nodesToVisit.append(&program.at(module.data).root);
        while (!nodesToVisit.isEmpty()) {
            const DataNode& node = *nodesToVisit.takeLast();
            switch (node.kind()) {
            case DataNode::Kind::Undefined:
            case DataNode::Kind::Null:
            case DataNode::Kind::Hole:
            case DataNode::Kind::Boolean:
            case DataNode::Kind::Number:
            case DataNode::Kind::String:
            case DataNode::Kind::BigInt:
                break;
            case DataNode::Kind::Array:
                for (const DataNode& element : node.as<DataNode::Array>().elements)
                    nodesToVisit.append(&element);
                break;
            case DataNode::Kind::Object: {
                const auto& object = node.as<DataNode::Object>();
                for (const DataMember& member : object.members)
                    nodesToVisit.append(&member.value);
                if (object.layout)
                    functor(object.layout);
                break;
            }
            }
        }
    }

    for (const ExpectedResult& expected : program.expectedResults) {
        if (!expected.flags.contains(ExpectedResultFlags::IsMadeByParser))
            continue;
        for (LayoutID layout : expected.layouts) {
            if (parsersCouldMakeRecordsOf(program, layout))
                functor(layout);
        }
    }

    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].has(LayoutFlags::IsOfCreatedObject))
            functor(LayoutID(index));
    }
}

static bool onlyCompiledCodeWritesFieldsInLinkLayouts(const Layout& layout)
{
    return layout.kind == LayoutKind::Environment || isFoundByShapeTest(layout);
}

void seedCellsSharedWithUnseenCode(LinkFlow& flow)
{
    const Program& program = flow.program;
    LinkType anything = LinkType::anything();
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (!variable.flags.contains(VariableFlags::OriginalMayRun) && variablesAreWrittenOnlyByBodiesInLinkLayouts(program.at(variable.module).kind))
            continue;
        FlowNodeID cell = flow.variable(VariableID(index));
        flow.escapedVariables.set(index);
        flow.arrive(cell, anything);
        flow.addEdge(cell, flow.unseen());
    }

    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (onlyCompiledCodeWritesFieldsInLinkLayouts(layout))
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (!isPrivateNameInLink(program, layout.fields[field].key))
                flow.arrive(flow.field({ LayoutID(index), field }), anything);
        }
    }
}

static OptionSet<LayoutChange> changesMadeByInLinkLayouts(SummaryChange::Kind kind)
{
    switch (kind) {
    case SummaryChange::Kind::Freezes:
        return { LayoutChange::IsFrozen };
    case SummaryChange::Kind::Deletes:
        return { LayoutChange::LosesProperty };
    case SummaryChange::Kind::DefinesByDescriptor:
        return { LayoutChange::GetsAccessor, LayoutChange::GainsProperty, LayoutChange::IsFrozen };
    case SummaryChange::Kind::ChangesPrototype:
        return { LayoutChange::ChangesPrototype };
    case SummaryChange::Kind::BecomesPrototype:
        return { };
    case SummaryChange::Kind::CopiesInto:
        return { LayoutChange::GainsProperty, LayoutChange::ChangesPrototype };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct ObservedChangeInLinkLayouts {
    FlowNodeID subjectNode;
    FlowNodeID otherNode;
    LinkType subject;
    LinkType other;
    PropertyKeyID key;
    bool keyIsWritten { false };
    SummaryChange::Lays lays { SummaryChange::Lays::WhatDescriptorSays };
    bool descriptorIsInSight { false };
    bool descriptorLaysNoAccessor { false };
    bool isEntryWrittenInMap { false };
    bool laysAccessorAsWritten { false };
    bool keyIsSomeSymbol { false };
    bool namesOfCopyLeaveLinkAlone { false };
    bool laysValueInSight { false };
    FlowNodeID valueInSight;
    FlowNodeID valueLaidThroughConstant;
    bool heirIsMadeByCall { false };
    FlowNodeID laidAboveByConstructor;
    FlowNodeID getterInSight;
    FlowNodeID setterInSight;
    bool definesWhatItCopies { false };
    bool isSpelledByLoop { false };
    FunctionID function;
    uint32_t record { 0 };
    bool fieldIsAsked { false };
};

} // anonymous namespace

static LinkType typeOfClosuresInLinkLayouts(FunctionID function)
{
    LinkType type;
    type.functions = LinkType::Functions::Closure;
    type.function = function;
    return type;
}

static LinkType typeOfPrototypeInLinkLayouts(LayoutID layout)
{
    LinkType type;
    type.objects = LinkType::Objects::Prototype;
    type.layout = layout;
    return type;
}

static LinkType typeOfPrototypesInLinkLayouts(FamilyID family)
{
    LinkType type;
    type.objects = LinkType::Objects::Prototypes;
    type.family = family;
    return type;
}

static LinkType typeOfUnseenObjectsInLinkLayouts()
{
    LinkType type;
    type.hasUnknownObjects = true;
    return type;
}

template<typename Functor>
static void forEachLayoutInType(const LinkFlow& flow, const LinkType& type, const Functor& functor)
{
    switch (type.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Lost:
        return;
    case LinkType::Objects::Layout:
        functor(type.layout);
        return;
    case LinkType::Objects::Set:
        for (LayoutID member : flow.layoutsOfSet(type))
            functor(member);
        return;
    case LinkType::Objects::Family:
        flow.program.at(flow.families.layoutsOf(type.family)).forEach(functor);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Functor>
static void forEachClassOfPrototypeNamedInLinkLayouts(const LinkFlow& flow, const LinkType& type, const Functor& functor)
{
    switch (type.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Layout:
    case LinkType::Objects::Set:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        return;
    case LinkType::Objects::Prototype:
        functor(type.layout);
        return;
    case LinkType::Objects::Prototypes:
        flow.program.at(flow.families.layoutsOf(type.family)).forEach(functor);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Functor>
static void forEachClassOfClassObjectNamedInLinkLayouts(const LinkFlow& flow, const LinkType& type, const Functor& functor)
{
    switch (type.functions) {
    case LinkType::Functions::None:
    case LinkType::Functions::Set:
    case LinkType::Functions::Lost:
        return;
    case LinkType::Functions::Closure:
        if (LayoutID constructed = flow.program.at(type.function).constructedClass)
            functor(constructed);
        return;
    case LinkType::Functions::Constructors:
        flow.program.at(flow.families.layoutsOf(type.constructorsOf)).forEach(functor);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Functor>
static void forEachElementsNodeInLinkLayouts(LinkFlow& flow, const LinkType& type, const Functor& functor)
{
    if (type.classes == LinkType::Classes::None)
        return;
    functor(flow.contentsOf(type, ContentsSlot::Element));
    functor(flow.contentsOf(type, SlotOfContents::underAnyName()));
}

static bool namesOnlyInstancesInLinkLayouts(const LinkType& type)
{
    return type.objects != LinkType::Objects::Prototype && type.objects != LinkType::Objects::Prototypes && type.functions == LinkType::Functions::None && !type.builtin;
}

static bool namesBuiltinAloneInLinkLayouts(const LinkType& type)
{
    LinkType beside = type;
    beside.builtin = { };
    beside.primitives = 0;
    return type.builtin && beside.isNever();
}

using NodesStoredInLinkLayouts = Vector<FlowNodeID, SetsOfLayoutsInLinkFlow::mostMembers>;

static FlowNodeID nodeStoredInLayoutUnderInLinkLayouts(LinkFlow& flow, LayoutID layout, PropertyKeyID key)
{
    if (!key)
        return flow.anyFieldStoredThrough(LinkType::Objects::Layout, layout, { });
    if (std::optional<FieldReference> found = findField(flow.program, layout, key))
        return flow.field(*found);
    return { };
}

static NodesStoredInLinkLayouts nodesStoredUnderInLinkLayouts(LinkFlow& flow, const LinkType& type, PropertyKeyID key, LinkFlow::Lays lays)
{
    switch (type.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        return { };
    case LinkType::Objects::Layout:
        return { nodeStoredInLayoutUnderInLinkLayouts(flow, type.layout, key) };
    case LinkType::Objects::Set: {
        NodesStoredInLinkLayouts nodes;
        for (LayoutID member : flow.layoutsOfSet(type))
            nodes.append(nodeStoredInLayoutUnderInLinkLayouts(flow, member, key));
        return nodes;
    }
    case LinkType::Objects::Family:
        return { key ? flow.fieldStoredThrough(type.family, key, lays) : flow.anyFieldStoredThrough(type.objects, type.layout, type.family) };
    case LinkType::Objects::Lost:
        return { key ? flow.fieldStoredThroughLost(type.layout, key, lays) : flow.anyFieldStoredThrough(type.objects, type.layout, type.family) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void addChangesInLinkLayouts(LinkFlow& flow, LayoutID layout, OptionSet<LayoutChange> changes)
{
    if (!flow.changesByMoreThanStoresInSight[layout.index()].containsAll(changes))
        flow.note(LinkFlow::Note::ChangeOfLayout, layout.index(), changes.toRaw());
}

static void addChangesToLayoutsNamedInLinkLayouts(LinkFlow& flow, const LinkType& type, OptionSet<LayoutChange> changes)
{
    forEachLayoutInType(flow, type, [&](LayoutID layout) {
        addChangesInLinkLayouts(flow, layout, changes);
    });
}

static void handToUnseenCodeInLinkLayouts(LinkFlow& flow, FlowNodeID node, const LinkType& type)
{
    if (node != flow.unseen())
        flow.addEdge(node, flow.unseen());
    auto letGo = [&](LayoutID classLayout) {
        flow.escape(classLayout);
    };
    forEachClassOfPrototypeNamedInLinkLayouts(flow, type, letGo);
    forEachClassOfClassObjectNamedInLinkLayouts(flow, type, letGo);
}

static bool listsAccessorInLinkLayouts(const LinkFlow& flow, LayoutID layout)
{
    size_t stepsLeft = flow.program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = flow.program.at(current).base, --stepsLeft) {
        if (flow.layoutsWhoseOwnFieldsListAccessor.quickGet(current.index()))
            return true;
    }
    return false;
}

static void observeFreezeInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    OptionSet<LayoutChange> changes = changesMadeByInLinkLayouts(SummaryChange::Kind::Freezes);
    addChangesToLayoutsNamedInLinkLayouts(flow, change.subject, changes);
    if (FlowNodeID noLongerAsMade = flow.contentsOf(change.subject, SlotOfContents::noLongerAsMade()))
        flow.arrive(noLongerAsMade, flow.fromTypeID(TypeTable::undefined));
    forEachClassOfPrototypeNamedInLinkLayouts(flow, change.subject, [&](LayoutID classLayout) {
        addChangesInLinkLayouts(flow, classLayout, changes);
        for (LayoutID below : flow.program.layoutsExpectedBelow(classLayout))
            addChangesInLinkLayouts(flow, below, changes);
    });
}

static void observeDeletionInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    LinkType besideBuiltin = change.subject;
    besideBuiltin.builtin = { };
    if (!namesOnlyInstancesInLinkLayouts(besideBuiltin))
        handToUnseenCodeInLinkLayouts(flow, change.subjectNode, change.subject);
    addChangesToLayoutsNamedInLinkLayouts(flow, change.subject, changesMadeByInLinkLayouts(SummaryChange::Kind::Deletes));

    for (FlowNodeID emptied : nodesStoredUnderInLinkLayouts(flow, change.subject, change.key, LinkFlow::Lays::Assigns)) {
        if (!emptied)
            continue;
        flow.arrive(emptied, flow.fromTypeID(typeOfPrimitives(Primitive::Undefined | Primitive::Absent)));
        if (change.subject.objects == LinkType::Objects::Lost)
            flow.arrive(emptied, LinkType::anything());
    }
    if (!change.key || flow.program.isIndexLike(change.key)) {
        LinkType undefined = flow.fromTypeID(TypeTable::undefined);
        forEachElementsNodeInLinkLayouts(flow, change.subject, [&](FlowNodeID elements) {
            flow.arrive(elements, undefined);
        });
        flow.tellWrittenListIsChanged(change.subject);
    }
    if (change.subject.classes != LinkType::Classes::None && (!change.key || !flow.program.isIndexLike(change.key)))
        handToUnseenCodeInLinkLayouts(flow, change.subjectNode, change.subject);
}

static LayoutID onlyPrototypeOfConstructorFunctionInLinkLayouts(const LinkFlow& flow, const LinkType& type)
{
    if (type.objects != LinkType::Objects::Prototype || !flow.isOfConstructorFunction(type.layout))
        return { };
    LinkType withoutPrimitives = type;
    withoutPrimitives.primitives = 0;
    return withoutPrimitives == typeOfPrototypeInLinkLayouts(type.layout) ? type.layout : LayoutID();
}

static FunctionID onlyConstructorFunctionInLinkLayouts(const LinkFlow& flow, const LinkType& type)
{
    bool namesOne = type.functions == LinkType::Functions::Closure && type == typeOfClosuresInLinkLayouts(type.function) && flow.layoutOfConstructorFunction(type.function);
    return namesOne ? type.function : FunctionID();
}

static bool everyFunctionAboveIsConstructorFunctionWithBodyInLinkLayouts(const LinkFlow& flow, FunctionID below)
{
    const Program& program = flow.program;
    size_t stepsLeft = program.layouts.size();
    for (LayoutID above = program.at(flow.layoutOfConstructorFunction(below)).base; above; above = program.at(above).base) {
        FunctionID holder = program.at(above).constructor;
        if (!stepsLeft-- || !holder || !flow.layoutOfConstructorFunction(holder) || !program.at(holder).hasBody() || program.at(holder).isCoroutine())
            return false;
    }
    return true;
}

static bool layKnownAccessorInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    const Program& program = flow.program;
    if (change.lays == SummaryChange::Lays::WhatDescriptorSays || !change.key || !change.otherNode || change.subject.objects != LinkType::Objects::Layout)
        return false;
    LayoutID layout = change.subject.layout;
    LinkType objectsOfLayout;
    objectsOfLayout.objects = LinkType::Objects::Layout;
    objectsOfLayout.layout = layout;
    std::optional<FieldReference> field = findField(program, layout, change.key);
    if (field ? !fieldAt(program, *field).form.isEmpty() : !change.descriptorIsInSight)
        return false;
    bool namesNothingElse = change.subject == objectsOfLayout;
    if (change.other.isNever())
        return namesNothingElse;
    if (change.other.functions != LinkType::Functions::Closure)
        return false;
    namesNothingElse &= change.other == typeOfClosuresInLinkLayouts(change.other.function);

    FunctionID id = change.other.function;
    const Function& function = program.at(id);
    bool isGetter = change.lays == SummaryChange::Lays::Getter;
    bool isOfKindThatLiteralsLay = function.kind == (isGetter ? FunctionKind::Getter : FunctionKind::Setter);
    bool isOfKindThatDescriptorsLay = change.descriptorIsInSight && (function.kind == FunctionKind::Plain || function.kind == FunctionKind::Method || function.kind == FunctionKind::Arrow);
    if ((!isOfKindThatLiteralsLay && !isOfKindThatDescriptorsLay) || !function.hasBody() || function.isCoroutine() || function.results.size() != 1 || flow.summaries.at(id).has(SummaryTrait::UsesArguments))
        return false;

    flow.noteAccessorLaid(layout, field ? AccessorLaidInField { *field, id, isGetter, { } } : AccessorLaidInField { { layout, 0 }, id, isGetter, change.key });
    FlowNodeID held = field ? flow.field(*field) : flow.appendedField(layout, change.key);
    if (!change.fieldIsAsked && flow.typeAt(held).isNever()) {
        if (flow.copiesObservedIn(change.function).accessorsThatWait.add(static_cast<uint64_t>(change.function.index()) << 32 | change.record).isNewEntry)
            flow.observe(held, FlowObserver { FlowObserver::Kind::FieldOfAccessorLaid, change.function, change.record });
        return namesNothingElse;
    }
    flow.reach(id);
    flow.addEdge(flow.thrown(id), flow.unseen());
    LinkType undefined = flow.fromTypeID(TypeTable::undefined);
    uint32_t firstPositional = function.firstParameterIndex();
    uint32_t numberOfPositional = function.numberOfPositionalParameters();
    for (uint32_t index = 0; index < function.numberOfEntryParameters(); ++index) {
        FlowNodeID node = flow.parameter(id, index);
        switch (function.entryParameterKind(index)) {
        case EntryParameter::Capture:
            break;
        case EntryParameter::This:
            flow.addEdge(change.subjectNode, node);
            flow.addEdge(flow.whatStandsBelow(layout), node);
            flow.addEdge(flow.whatStandsBelow(layout), flow.heirsThatRun(id));
            break;
        case EntryParameter::Callee:
            flow.arrive(node, typeOfClosuresInLinkLayouts(id));
            break;
        case EntryParameter::NewTarget:
            flow.arrive(node, undefined);
            break;
        case EntryParameter::ArgumentCount:
            flow.arrive(node, flow.fromTypeID(typeOfPrimitives(Primitive::Integer)));
            break;
        case EntryParameter::Parameter:
            if (index - firstPositional >= numberOfPositional) {
                flow.arrive(node, LinkType::anything());
                if (!isGetter)
                    flow.addEdge(held, flow.unseen());
            } else if (!isGetter && index == firstPositional)
                flow.addEdge(held, node);
            else
                flow.arrive(node, undefined);
            break;
        case EntryParameter::HiddenRest:
            flow.arrive(node, LinkType::anything());
            if (!isGetter)
                flow.addEdge(held, flow.unseen());
            break;
        }
    }
    if (isGetter)
        flow.addEdge(flow.result(id, 0), held);
    else
        flow.arrive(held, undefined);
    return namesNothingElse;
}

static bool handObjectToHalvesInSightInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    for (FlowNodeID half : { change.getterInSight, change.setterInSight }) {
        if (!half)
            continue;
        LinkType held = flow.typeAt(half);
        held.primitives = 0;
        if (held.isNever())
            continue;
        if (held.functions != LinkType::Functions::Closure || held != typeOfClosuresInLinkLayouts(held.function))
            return false;
        const Function& function = flow.program.at(held.function);
        for (uint32_t index = 0; index < function.numberOfEntryParameters(); ++index) {
            if (function.entryParameterKind(index) == EntryParameter::This)
                flow.addEdge(change.subjectNode, flow.parameter(held.function, index));
        }
    }
    return true;
}

static bool definitionInSightIsStoreInLinkLayouts(const LinkFlow& flow, const LinkType& subject, PropertyKeyID key)
{
    if (onlyPrototypeOfConstructorFunctionInLinkLayouts(flow, subject))
        return true;
    return subject.functions == LinkType::Functions::Closure && subject == typeOfClosuresInLinkLayouts(subject.function) && flow.followsNamesOnClosuresOf(subject.function, key);
}

static bool halvesInSightTakeNoReceiverInLinkLayouts(const LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    for (FlowNodeID half : { change.getterInSight, change.setterInSight }) {
        if (!half)
            continue;
        LinkType held = flow.typeAt(half);
        held.primitives = 0;
        if (held.isNever())
            continue;
        if (held.functions != LinkType::Functions::Closure || held != typeOfClosuresInLinkLayouts(held.function))
            return false;
        const Function& function = flow.program.at(held.function);
        for (uint32_t index = 0; index < function.numberOfEntryParameters(); ++index) {
            if (function.entryParameterKind(index) == EntryParameter::This)
                return false;
        }
    }
    return true;
}

static bool layWhatIsWrittenOnOneClosureInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    if (!change.key || (!change.descriptorIsInSight && !change.isEntryWrittenInMap))
        return false;
    const LinkType& subject = change.subject;
    if (subject.functions != LinkType::Functions::Closure || subject != typeOfClosuresInLinkLayouts(subject.function) || !flow.followsNamesOnClosuresOf(subject.function, change.key))
        return false;
    if (!halvesInSightTakeNoReceiverInLinkLayouts(flow, change))
        return false;
    if (change.setterInSight)
        flow.addEdge(flow.laidOnClosuresOf(subject.function, change.key), flow.unseen());
    return true;
}

static bool namesOnlyPlainClosuresMadeWithInLinkLayouts(const LinkFlow& flow, const LinkType& subject, PropertyKeyID key)
{
    if (!key || (key != flow.program.keyOf(BuiltinNames::name) && key != flow.program.keyOf(BuiltinNames::length)))
        return false;
    LinkType others = subject.directPart();
    others.primitives = 0;
    bool areMadeWithName = false;
    if (subject.functions == LinkType::Functions::Closure)
        areMadeWithName = flow.isPlainClosureMadeWith(subject.function, key) || flow.isClassObjectMadeWith(subject.function, key);
    else
        areMadeWithName = subject.functions == LinkType::Functions::Set || subject.functions == LinkType::Functions::Lost;
    others.functions = LinkType::Functions::None;
    others.function = { };
    return areMadeWithName && !subject.hasUnknownObjects && others.isNever();
}

static void expectValueOfDescriptorUnderNameInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    if (!change.key || change.lays != SummaryChange::Lays::WhatDescriptorSays)
        return;
    PropertyKeyID value = flow.program.keyOf(BuiltinNames::value);
    NodesStoredInLinkLayouts held;
    if (change.valueLaidThroughConstant)
        held.append(change.valueLaidThroughConstant);
    else if (change.other.objects == LinkType::Objects::Family)
        held.append(flow.fieldReadThrough(change.other.family, value));
    else if (change.other.objects == LinkType::Objects::Lost)
        held.append(flow.fieldReadThroughLost(change.other.layout, value));
    else {
        forEachLayoutInType(flow, change.other, [&](LayoutID descriptor) {
            if (std::optional<FieldReference> field = findField(flow.program, descriptor, value))
                held.append(flow.field(*field));
        });
    }
    if (held.isEmpty() || (change.subject.objects != LinkType::Objects::Layout && change.subject.objects != LinkType::Objects::Set))
        return;
    forEachLayoutInType(flow, change.subject, [&](LayoutID layout) {
        FlowNodeID laid = flow.laidByDescriptorsOn(layout, change.key);
        for (FlowNodeID from : held)
            flow.addEdge(from, laid);
    });
}

static bool getterIsClosureWrittenInDescriptorInLinkLayouts(const LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    const ThinSummary& summary = flow.summaries.at(change.function);
    uint32_t getter = summary.changes[change.record].getterInSight;
    return getter != noSummaryNode && summary.nodes[getter].kind == SummaryNode::Kind::Closure;
}

static bool allocationInBodyFillsFieldInLinkLayouts(const LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    std::optional<FieldReference> field = findField(flow.program, change.subject.layout, change.key);
    const ThinSummary& summary = flow.summaries.at(change.function);
    return field && summary.nodes[summary.changes[change.record].subject].kind == SummaryNode::Kind::Allocation && !summary.leavesEmpty(*field);
}

static void observeDefinitionInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    LinkType whatCanBeDefinedOn = change.subject;
    whatCanBeDefinedOn.primitives = 0;
    if (whatCanBeDefinedOn.isNever())
        return;
    bool isStore = change.laysValueInSight && change.key && definitionInSightIsStoreInLinkLayouts(flow, change.subject, change.key);
    bool namesObjectsOfLayoutsAlone = (change.subject.objects == LinkType::Objects::Layout || change.subject.objects == LinkType::Objects::Set || change.subject.objects == LinkType::Objects::Family) && change.subject.functions == LinkType::Functions::None && change.subject.classes == LinkType::Classes::None && !change.subject.builtin;
    bool objectMayBeOutOfSight = change.subject.hasUnknownObjects || change.subject.holdsLostBuiltins;
    bool isOneClosureWhoseNamesAreFollowed = change.key && change.subject.functions == LinkType::Functions::Closure && change.subject == typeOfClosuresInLinkLayouts(change.subject.function) && flow.isPlainClosureOrConstructorFunction(change.subject.function) && flow.followsNamesOnClosuresOf(change.subject.function, change.key);
    bool accessLaysValue = change.key && change.descriptorIsInSight && change.descriptorLaysNoAccessor && ((namesObjectsOfLayoutsAlone && !objectMayBeOutOfSight) || isOneClosureWhoseNamesAreFollowed);
    if (change.valueInSight && !isStore && !accessLaysValue && !change.subject.isNever())
        flow.addEdge(change.valueInSight, flow.unseen());
    if (change.descriptorLaysNoAccessor && namesBuiltinAloneInLinkLayouts(change.subject)) {
        if (change.otherNode)
            handToUnseenCodeInLinkLayouts(flow, change.otherNode, change.other);
        return;
    }
    if (change.descriptorLaysNoAccessor && namesOnlyPlainClosuresMadeWithInLinkLayouts(flow, change.subject, change.key)) {
        if (!change.valueInSight && change.otherNode)
            handToUnseenCodeInLinkLayouts(flow, change.otherNode, change.other);
        return;
    }
    if (layKnownAccessorInLinkLayouts(flow, change))
        return;
    bool getterIsKnown = false;
    if (change.descriptorIsInSight && change.isEntryWrittenInMap && !change.descriptorLaysNoAccessor && namesObjectsOfLayoutsAlone) {
        for (FlowNodeID half : { change.getterInSight, change.setterInSight }) {
            if (half)
                handToUnseenCodeInLinkLayouts(flow, half, flow.typeAt(half));
        }
    }
    if (change.lays == SummaryChange::Lays::WhatDescriptorSays && change.descriptorIsInSight && change.key && change.getterInSight && !change.setterInSight && !objectMayBeOutOfSight && change.subject.objects == LinkType::Objects::Layout && flow.program.at(change.subject.layout).kind == LayoutKind::Record && getterIsClosureWrittenInDescriptorInLinkLayouts(flow, change)) {
        ObservedChangeInLinkLayouts asGetter = change;
        asGetter.lays = SummaryChange::Lays::Getter;
        asGetter.otherNode = change.getterInSight;
        asGetter.other = flow.typeAt(change.getterInSight);
        if (layKnownAccessorInLinkLayouts(flow, asGetter)) {
            std::optional<FieldReference> field = findField(flow.program, change.subject.layout, change.key);
            flow.arrive(field ? flow.field(*field) : flow.appendedField(change.subject.layout, change.key), flow.fromTypeID(TypeTable::undefined));
            getterIsKnown = !allocationInBodyFillsFieldInLinkLayouts(flow, change);
        }
    }
    if (change.descriptorIsInSight && namesObjectsOfLayoutsAlone && handObjectToHalvesInSightInLinkLayouts(flow, change)) {
        forEachLayoutInType(flow, change.subject, [&](LayoutID layout) {
            if (std::optional<FieldReference> field = change.descriptorLaysNoAccessor || !change.key ? std::nullopt : findField(flow.program, layout, change.key))
                flow.noteFieldGivenAccessorByDescriptorInSight(*field);
            else if (!change.descriptorLaysNoAccessor && !flow.layoutsGivenAccessorsByDescriptorsInSight.get(layout.index()))
                flow.note(LinkFlow::Note::LayoutGivenAccessorByDescriptorInSight, layout.index());
            if (!flow.layoutsDefinedOnUnderWrittenKeys.get(layout.index()))
                flow.note(LinkFlow::Note::LayoutDefinedOnUnderWrittenKeys, layout.index());
            if (!change.keyIsWritten && !flow.layoutsDefinedOnUnderKeysThatAreNotWritten.get(layout.index()))
                flow.note(LinkFlow::Note::LayoutDefinedOnUnderKeyThatIsNotWritten, layout.index());
            if (!change.key) {
                flow.addEdge(flow.unseen(), flow.extraPropertyStoreNode(layout));
                return;
            }
            if (change.setterInSight) {
                std::optional<FieldReference> field = findField(flow.program, layout, change.key);
                flow.addEdge(field ? flow.field(*field) : flow.appendedField(layout, change.key), flow.unseen());
            }
            if (!change.descriptorLaysNoAccessor && !getterIsKnown) {
                std::optional<FieldReference> field = findField(flow.program, layout, change.key);
                flow.arrive(field ? flow.field(*field) : flow.appendedField(layout, change.key), LinkType::anything());
            }
        });
        return;
    }
    bool descriptorIsOnlyRead = false;
    bool isAccessorWrittenInMap = change.laysAccessorAsWritten && change.isEntryWrittenInMap;
    if (change.key && change.otherNode && !change.descriptorIsInSight && !isAccessorWrittenInMap) {
        if (change.other.isNever())
            return;
        Vector<FlowNodeID, 4> values;
        FlowObserver toldAgain { FlowObserver::Kind::OtherOfChange, change.function, change.record };
        descriptorIsOnlyRead = flow.descriptorsInSightLayValuesAlone(change.other, &toldAgain, values);
        if (descriptorIsOnlyRead && (!namesObjectsOfLayoutsAlone || objectMayBeOutOfSight)) {
            for (FlowNodeID value : values)
                flow.addEdge(value, flow.unseen());
        }
        if (descriptorIsOnlyRead && namesObjectsOfLayoutsAlone) {
            expectValueOfDescriptorUnderNameInLinkLayouts(flow, change);
            forEachLayoutInType(flow, change.subject, [&](LayoutID layout) {
                if (!flow.layoutsDefinedOnUnderWrittenKeys.get(layout.index()))
                    flow.note(LinkFlow::Note::LayoutDefinedOnUnderWrittenKeys, layout.index());
                if (!change.keyIsWritten && !flow.layoutsDefinedOnUnderKeysThatAreNotWritten.get(layout.index()))
                    flow.note(LinkFlow::Note::LayoutDefinedOnUnderKeyThatIsNotWritten, layout.index());
                std::optional<FieldReference> field = findField(flow.program, layout, change.key);
                FlowNodeID laid = field ? flow.field(*field) : flow.appendedField(layout, change.key);
                for (FlowNodeID value : values)
                    flow.addEdge(value, laid);
                flow.arrive(laid, flow.fromTypeID(TypeTable::undefined));
            });
            return;
        }
    }
    bool isLaidByNameOnPrototype = change.laysAccessorAsWritten && flow.isDefinedByDescriptorSomewhere(change.key) && onlyPrototypeOfConstructorFunctionInLinkLayouts(flow, change.subject);
    if (isAccessorWrittenInMap && !isLaidByNameOnPrototype) {
        for (FlowNodeID half : { change.getterInSight, change.setterInSight }) {
            if (half)
                handToUnseenCodeInLinkLayouts(flow, half, flow.typeAt(half));
        }
    }
    if (isStore || layWhatIsWrittenOnOneClosureInLinkLayouts(flow, change))
        return;
    if (isLaidByNameOnPrototype)
        return;
    if (!isAccessorWrittenInMap && change.otherNode && !descriptorIsOnlyRead)
        handToUnseenCodeInLinkLayouts(flow, change.otherNode, change.other);
    expectValueOfDescriptorUnderNameInLinkLayouts(flow, change);
    if (LayoutID function = onlyPrototypeOfConstructorFunctionInLinkLayouts(flow, change.subject)) {
        flow.defineOnPrototypeOf(function, change.key, change.keyIsSomeSymbol ? LinkFlow::UnknownKey::IsSomeSymbol : LinkFlow::UnknownKey::IsAnyKey);
        return;
    }
    handToUnseenCodeInLinkLayouts(flow, change.subjectNode, change.subject);
    addChangesToLayoutsNamedInLinkLayouts(flow, change.subject, changesMadeByInLinkLayouts(SummaryChange::Kind::DefinesByDescriptor));
    LinkType anything = LinkType::anything();
    forEachElementsNodeInLinkLayouts(flow, change.subject, [&](FlowNodeID elements) {
        flow.arrive(elements, anything);
    });
}

static bool agreesWithLayoutsInLinkLayouts(const LinkFlow&, const LinkType& above, const LinkType& below);

static bool holdsNoObjectBesideLayoutsInLinkLayouts(const LinkType& type)
{
    return type.functions == LinkType::Functions::None && type.classes == LinkType::Classes::None && !type.builtin && !type.builtinClass && !type.hasUnknownObjects && !type.holdsLostBuiltins;
}

static bool putsNullAloneAboveRecordsInLinkLayouts(const LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    if (!change.otherNode || change.other.primitives != Primitive::Null || change.other.objects != LinkType::Objects::None || !holdsNoObjectBesideLayoutsInLinkLayouts(change.other))
        return false;
    if ((change.subject.objects != LinkType::Objects::Layout && change.subject.objects != LinkType::Objects::Set) || !holdsNoObjectBesideLayoutsInLinkLayouts(change.subject))
        return false;
    bool areRecords = true;
    forEachLayoutInType(flow, change.subject, [&](LayoutID layout) {
        const Layout& named = flow.program.at(layout);
        areRecords &= named.kind == LayoutKind::Record && !named.constructor && !named.has(LayoutFlags::IsConstructorFunction) && !named.has(LayoutFlags::IsOfCreatedObject);
    });
    return areRecords;
}

static void observeChangeOfPrototypeInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    if (putsNullAloneAboveRecordsInLinkLayouts(flow, change)) {
        forEachLayoutInType(flow, change.subject, [&](LayoutID layout) {
            if (!flow.layoutsThatGetNullAbove.get(layout.index()))
                flow.note(LinkFlow::Note::LayoutGetsNullAbove, layout.index());
        });
        return;
    }
    bool mayPutNullAbove = change.other.primitives & Primitive::Null;
    FunctionID functionLinked = onlyConstructorFunctionInLinkLayouts(flow, change.subject);
    if (functionLinked && !everyFunctionAboveIsConstructorFunctionWithBodyInLinkLayouts(flow, functionLinked))
        functionLinked = { };
    if (change.otherNode && !mayPutNullAbove && (functionLinked || onlyPrototypeOfConstructorFunctionInLinkLayouts(flow, change.subject)) && (change.other.isNever() || agreesWithLayoutsInLinkLayouts(flow, change.other, change.subject))) {
        if (functionLinked)
            flow.arrive(flow.foundOnClosuresOf(functionLinked), LinkType::anything());
        return;
    }
    if (change.otherNode)
        handToUnseenCodeInLinkLayouts(flow, change.otherNode, change.other);
    handToUnseenCodeInLinkLayouts(flow, change.subjectNode, change.subject);
    addChangesToLayoutsNamedInLinkLayouts(flow, change.subject, changesMadeByInLinkLayouts(SummaryChange::Kind::ChangesPrototype));
    notePrototypesLaidAboveOtherObjects(flow, change.other);
    LinkType anything = LinkType::anything();
    forEachElementsNodeInLinkLayouts(flow, change.subject, [&](FlowNodeID elements) {
        flow.arrive(elements, anything);
    });
}

static bool isWrittenRightAboveInLinkLayouts(const Program& program, LayoutID below, const LinkType& above, BuiltinID BuiltinClass::* part)
{
    BuiltinClassID written = program.at(below).base ? BuiltinClassID() : program.at(below).writtenBuiltinBase;
    BuiltinID partOfWritten = written ? program.builtins().at(written).*part : BuiltinID();
    if (!written && !program.at(below).base && program.at(below).has(LayoutFlags::HasHeritage) && namesBuiltinAloneInLinkLayouts(above)) {
        BuiltinKind kind = program.builtins().at(above.builtin).kind;
        return part == &BuiltinClass::constructor ? kind == BuiltinKind::Constructor : kind == BuiltinKind::Prototype;
    }
    return partOfWritten && namesBuiltinAloneInLinkLayouts(above) && above.builtin == program.builtins().canonicalID(partOfWritten);
}

static bool agreesWithLayoutsInLinkLayouts(const LinkFlow& flow, const LinkType& above, const LinkType& below)
{
    const Program& program = flow.program;
    LinkType aboveWithoutPrimitives = above;
    aboveWithoutPrimitives.primitives = 0;

    LayoutID classOfClosureBelow = below.functions == LinkType::Functions::Closure && below == typeOfClosuresInLinkLayouts(below.function) ? flow.classOfConstructor(below.function) : LayoutID();
    if (FunctionID function = onlyConstructorFunctionInLinkLayouts(flow, below))
        classOfClosureBelow = flow.layoutOfConstructorFunction(function);
    if (classOfClosureBelow) {
        if (above.primitives & Primitive::Null)
            return false;
        if (aboveWithoutPrimitives.isNever())
            return true;
        LayoutID base = program.at(classOfClosureBelow).base;
        FunctionID constructorOfBase = base ? program.at(base).constructor : FunctionID();
        if (isWrittenRightAboveInLinkLayouts(program, classOfClosureBelow, above, &BuiltinClass::constructor))
            return true;
        return constructorOfBase && aboveWithoutPrimitives == typeOfClosuresInLinkLayouts(constructorOfBase);
    }

    if (below.objects == LinkType::Objects::Prototype && below == typeOfPrototypeInLinkLayouts(below.layout)) {
        if (aboveWithoutPrimitives.isNever())
            return true;
        LayoutID base = program.at(below.layout).base;
        if (isWrittenRightAboveInLinkLayouts(program, below.layout, above, &BuiltinClass::prototype))
            return true;
        return base && aboveWithoutPrimitives == typeOfPrototypeInLinkLayouts(base);
    }

    if (!namesOnlyInstancesInLinkLayouts(below) || below.classes != LinkType::Classes::None)
        return false;
    switch (above.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Layout:
    case LinkType::Objects::Set:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        return false;
    case LinkType::Objects::Prototype:
        if (above != typeOfPrototypeInLinkLayouts(above.layout))
            return false;
        if (below.objects == LinkType::Objects::Layout)
            return below.layout == above.layout;
        if (below.objects == LinkType::Objects::Set)
            return std::ranges::contains(flow.layoutsOfSet(below), above.layout);
        return below.objects == LinkType::Objects::Family && flow.families.nearestCommonFamily(below.family, above.layout) == below.family;
    case LinkType::Objects::Prototypes:
        if (above != typeOfPrototypesInLinkLayouts(above.family))
            return false;
        if (below.objects == LinkType::Objects::Set) {
            return std::ranges::all_of(program.types.listOf(flow.families.layoutsOf(above.family)), [&](LayoutID classAbove) {
                return std::ranges::contains(flow.layoutsOfSet(below), classAbove);
            });
        }
        return below.objects == LinkType::Objects::Family && flow.families.nearestCommonFamily(below.family, above.family) == below.family;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LinkType objectsAmongInLinkLayouts(const LinkType& type)
{
    LinkType objects;
    objects.objects = type.objects;
    objects.layout = type.layout;
    objects.family = type.family;
    objects.classes = type.classes;
    objects.builtinClass = type.builtinClass;
    objects.site = type.site;
    return objects;
}

FlowNodeID LinkFlow::findWhatConstructorLaysAbove(FunctionID function, const SummaryChange& change) const
{
    if (change.kind != SummaryChange::Kind::BecomesPrototype || change.heirIsMadeByCall || change.other == noSummaryNode)
        return { };
    const ThinSummary& summary = summaries.at(function);
    const SummaryNode& heir = summary.nodes[change.other];
    const SummaryNode& parent = summary.nodes[change.subject];
    if (heir.kind != SummaryNode::Kind::Allocation || !program.at(LayoutID(heir.first)).has(LayoutFlags::IsOfCreatedObject) || parent.kind != SummaryNode::Kind::ResultOfAccess)
        return { };
    const SummaryAccess& read = summary.accesses[parent.first];
    const SummaryNode& holder = summary.nodes[read.subject];
    const Function& reader = program.at(function);
    if (read.kind != SummaryAccess::Kind::Read || !read.key || read.isToContents() || holder.kind != SummaryNode::Kind::Parameter || holder.first != reader.thisParameterIndex())
        return { };
    TypeReader types(program);
    TypeID written = reader.thisType.written();
    LayoutID classLayout = written ? singleLayoutOf(types, types.at(written)) : LayoutID();
    FunctionID constructor = classLayout ? program.at(classLayout).constructor : FunctionID();
    if (!constructor)
        return { };
    const ThinSummary& ofConstructor = summaries.at(constructor);
    uint32_t stored = noSummaryNode;
    for (const SummaryAccess& access : ofConstructor.accesses) {
        if (access.key != read.key || access.isToContents() || (access.kind != SummaryAccess::Kind::Store && access.kind != SummaryAccess::Kind::Define))
            continue;
        if (stored != noSummaryNode)
            return { };
        stored = access.value;
    }
    if (stored == noSummaryNode)
        return { };
    for (const SummaryChange& made : ofConstructor.changes) {
        if (made.kind == SummaryChange::Kind::BecomesPrototype && made.heirIsMadeByCall && made.other == stored)
            return nodeOf(constructor, made.subject);
    }
    return { };
}

static void tieHeirToWhatStandsAboveInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    const LinkType whatStandsAbove = change.laidAboveByConstructor ? flow.typeAt(change.laidAboveByConstructor) : change.subject;
    Vector<FlowNodeID, 4> heirsOfWhatStandsAbove;
    forEachLayoutInType(flow, whatStandsAbove, [&](LayoutID above) {
        heirsOfWhatStandsAbove.append(flow.whatStandsBelow(above));
    });
    if (whatStandsAbove.classes == LinkType::Classes::Site)
        heirsOfWhatStandsAbove.append(flow.whatStandsBelow(whatStandsAbove.site));
    auto tie = [&](FlowNodeID aboveHeir, FlowNodeID belowHeir) {
        flow.arrive(aboveHeir, objectsAmongInLinkLayouts(whatStandsAbove));
        for (FlowNodeID heirs : heirsOfWhatStandsAbove)
            flow.addEdge(belowHeir, heirs);
    };
    forEachLayoutInType(flow, change.other, [&](LayoutID heir) {
        if (flow.program.at(heir).has(LayoutFlags::IsOfCreatedObject))
            tie(flow.whatStandsAbove(heir), flow.whatStandsBelow(heir));
    });
    if (change.other.classes == LinkType::Classes::Site)
        tie(flow.whatStandsAbove(change.other.site), flow.whatStandsBelow(change.other.site));
    if (change.heirIsMadeByCall)
        return;
    for (FlowNodeID heirs : heirsOfWhatStandsAbove)
        flow.addEdge(change.otherNode, heirs);
}

static void observeNewHeirInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    if (!change.otherNode) {
        handToUnseenCodeInLinkLayouts(flow, change.subjectNode, change.subject);
        return;
    }
    if (change.heirIsMadeByCall) {
        tieHeirToWhatStandsAboveInLinkLayouts(flow, change);
        handToUnseenCodeInLinkLayouts(flow, change.subjectNode, change.subject);
        return;
    }
    if (change.other.isNever())
        return;
    if (change.subject.objects == LinkType::Objects::None && holdsNoObjectBesideLayoutsInLinkLayouts(change.subject) && namesOnlyInstancesInLinkLayouts(change.other))
        return;

    if (agreesWithLayoutsInLinkLayouts(flow, change.subject, change.other)) {
        if (change.subject.builtin && change.other.functions == LinkType::Functions::Closure && !flow.program.at(flow.classOfConstructor(change.other.function)).writtenBuiltinBase) {
            flow.note(LinkFlow::Note::BuiltinStandsAboveClass, flow.program.builtins().canonicalID(change.subject.builtin).index(), change.other.function.index());
            flow.letLeaveWithClassObject(flow.classOfConstructor(change.other.function), change.subjectNode);
        }
        if (change.subject.builtin && change.other.objects == LinkType::Objects::Prototype && !flow.program.at(change.other.layout).writtenBuiltinBase)
            flow.letLeaveWithClassObject(change.other.layout, change.subjectNode);
        return;
    }

    tieHeirToWhatStandsAboveInLinkLayouts(flow, change);
    handToUnseenCodeInLinkLayouts(flow, change.subjectNode, change.subject);
    handToUnseenCodeInLinkLayouts(flow, change.otherNode, change.other);
    flow.arrive(change.otherNode, typeOfUnseenObjectsInLinkLayouts());
    notePrototypesLaidAboveOtherObjects(flow, change.subject);
}

static NodesStoredInLinkLayouts nodesLaidUnderAnyKeyByCopyInLinkLayouts(LinkFlow& flow, const LinkType& subject, bool definesWhatItCopies)
{
    if (subject.objects != LinkType::Objects::Layout && subject.objects != LinkType::Objects::Set)
        return nodesStoredUnderInLinkLayouts(flow, subject, { }, definesWhatItCopies ? LinkFlow::Lays::Defines : LinkFlow::Lays::Assigns);
    NodesStoredInLinkLayouts nodes;
    forEachLayoutInType(flow, subject, [&](LayoutID layout) {
        bool isDefinedOn = definesWhatItCopies && flow.program.at(layout).kind == LayoutKind::Record && !listsAccessorInLinkLayouts(flow, layout);
        nodes.append(isDefinedOn ? flow.anyFieldDefinedOn(layout) : nodeStoredInLayoutUnderInLinkLayouts(flow, layout, { }));
    });
    return nodes;
}

namespace {

struct KeysOfCopyInLinkLayouts {
    bool findFieldsInTarget { true };
    bool somethingLiesBesideFields { false };
    bool somethingSeenLiesBesideFields { false };
    Vector<FlowNodeID, 2> fieldsCopiedOntoThemselves;
};

} // anonymous namespace

static bool isListedOnBothSidesInLinkLayouts(LinkFlow& flow, const LinkType& subject, const LinkType& other)
{
    if (subject.objects == LinkType::Objects::None || other.objects == LinkType::Objects::None)
        return true;
    auto isListedKind = [](LinkType::Objects objects) {
        return objects == LinkType::Objects::Layout || objects == LinkType::Objects::Set || objects == LinkType::Objects::Family;
    };
    if (!isListedKind(subject.objects) || !isListedKind(other.objects))
        return false;

    bool isListed = true;
    forEachLayoutInType(flow, other, [&](LayoutID layout) {
        isListed &= !flow.ownValuesOf(layout).empty();
    });
    forEachLayoutInType(flow, subject, [&](LayoutID layout) {
        isListed &= flow.program.at(layout).kind == LayoutKind::Record && !listsAccessorInLinkLayouts(flow, layout);
    });
    return isListed;
}

static bool copyFieldByFieldInLinkLayouts(LinkFlow& flow, FunctionID function, const LinkType& subject, const LinkType& other, bool definesWhatItCopies, bool appendsNames, KeysOfCopyInLinkLayouts& keys)
{
    if (subject.objects == LinkType::Objects::None || other.objects == LinkType::Objects::None)
        return true;
    if (!isListedOnBothSidesInLinkLayouts(flow, subject, other))
        return false;

    auto& layoutsTied = flow.copiesObservedIn(function).layoutsTiedByCopies[definesWhatItCopies << 1 | appendsNames];
    bool keepsTies = subject.objects == LinkType::Objects::Layout && !flow.tellsObserversOnceMore();
    NodesStoredInLinkLayouts underAnyKey = nodesLaidUnderAnyKeyByCopyInLinkLayouts(flow, subject, definesWhatItCopies);
    forEachLayoutInType(flow, other, [&](LayoutID layout) {
        std::span<const LinkFlow::OwnValue> values = flow.ownValuesOf(layout);
        uint64_t pair = static_cast<uint64_t>(layout.index()) << 32 | subject.layout.index();
        bool foundFieldsInTargetBefore = keys.findFieldsInTarget;
        keys.findFieldsInTarget = true;
        auto tied = keepsTies ? layoutsTied.find(pair) : layoutsTied.end();
        bool laysTie = keepsTies && tied == layoutsTied.end() && std::ranges::all_of(values.first(values.size() - LinkFlow::numberOfOwnValuesBesideFields), [](const LinkFlow::OwnValue& value) { return !!value.key; });
        if (tied != layoutsTied.end()) {
            keys.findFieldsInTarget = tied->value;
            values = values.last(LinkFlow::numberOfOwnValuesBesideFields);
        }
        for (const LinkFlow::OwnValue& value : values) {
            if (definesWhatItCopies && !value.key) {
                const LinkType& held = flow.typeAt(value.node);
                keys.somethingLiesBesideFields |= !held.isNever();
                keys.somethingSeenLiesBesideFields |= !held.directPart().isNever();
            }
            if (definesWhatItCopies && value.key && !appendsNames) {
                forEachLayoutInType(flow, subject, [&](LayoutID target) {
                    keys.findFieldsInTarget &= !!findField(flow.program, target, value.key);
                });
            }
            auto copyInto = [&](FlowNodeID stored) {
                if (stored == value.node)
                    keys.fieldsCopiedOntoThemselves.append(stored);
                flow.addEdge(value.node, stored ? stored : flow.unseen());
            };
            if (appendsNames && definesWhatItCopies && value.key) {
                forEachLayoutInType(flow, subject, [&](LayoutID target) {
                    std::optional<FieldReference> found = findField(flow.program, target, value.key);
                    copyInto(found ? flow.field(*found) : flow.appendedStoreNode(target, value.key));
                });
                continue;
            }
            for (FlowNodeID stored : value.key ? nodesStoredUnderInLinkLayouts(flow, subject, value.key, definesWhatItCopies ? LinkFlow::Lays::Defines : LinkFlow::Lays::Assigns) : underAnyKey)
                copyInto(stored);
        }
        if (laysTie)
            layoutsTied.add(pair, keys.findFieldsInTarget);
        keys.findFieldsInTarget &= foundFieldsInTargetBefore;
    });
    return true;
}

static bool mayHaveOwnValueUnderPrototypeLinkInLinkLayouts(LinkFlow& flow, const LinkType& source)
{
    bool mayHave = false;
    forEachLayoutInType(flow, source, [&](LayoutID layout) {
        for (const LinkFlow::OwnValue& value : flow.ownValuesOf(layout))
            mayHave |= value.key ? flow.nameClassOf(value.key) == NameClass::Proto : !flow.typeAt(value.node).isNever();
    });
    return mayHave;
}

static bool holdsOnlyObjectsWithListedOwnValuesInLinkLayouts(LinkFlow& flow, const LinkType& source)
{
    if (source.functions != LinkType::Functions::None || source.classes != LinkType::Classes::None || source.builtin)
        return false;
    switch (source.objects) {
    case LinkType::Objects::None:
        return true;
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
    case LinkType::Objects::Lost:
        return false;
    case LinkType::Objects::Layout:
    case LinkType::Objects::Set:
    case LinkType::Objects::Family:
        break;
    }
    bool isListed = true;
    forEachLayoutInType(flow, source, [&](LayoutID layout) {
        isListed &= !flow.ownValuesOf(layout).empty();
    });
    return isListed;
}

uint32_t LinkFlow::sourceOfCopySpelledByLoop(FunctionID function, ValueID site) const
{
    for (const SummaryChange& change : summaries.at(function).changes) {
        if (change.site == site && change.isSpelledByLoop)
            return change.other;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool LinkFlow::followsCopySpelledByLoop(const LinkType& target, const LinkType& source) const
{
    auto hasOnlyRecords = [&](const LinkType& type) {
        if (type.objects != LinkType::Objects::Layout && type.objects != LinkType::Objects::Set)
            return false;
        if (type.hasUnknownObjects || type.holdsLostBuiltins || type.builtin || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || (type.primitives & LinkType::closed(Primitive::AnyString)))
            return false;
        bool areRecords = true;
        forEachLayoutInType(*this, type, [&](LayoutID layout) {
            areRecords &= program.at(layout).kind == LayoutKind::Record && !listsAccessorInLinkLayouts(*this, layout);
        });
        return areRecords;
    };
    auto listsNamesOf = [&](LayoutID lister, LayoutID other) {
        for (const Field& field : program.at(other).fields) {
            if (field.key && !field.computedKey && !isPrivateNameInLink(program, field.key) && !findField(program, lister, field.key))
                return false;
        }
        return true;
    };
    auto listSameNames = [&](const LinkType& type) {
        LayoutID first;
        bool listSame = true;
        forEachLayoutInType(*this, type, [&](LayoutID layout) {
            if (!first)
                first = layout;
            else
                listSame = listSame && listsNamesOf(first, layout) && listsNamesOf(layout, first);
        });
        return listSame;
    };
    return hasOnlyRecords(target) && (source.isNever() || (hasOnlyRecords(source) && listSameNames(source)));
}

enum class CopyWaitsForInLinkLayouts : uint8_t { WhateverLiesBesideFields, WhatIsSeenAlone, Nothing };

static CopyWaitsForInLinkLayouts observeCopyInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    if (change.otherNode && change.other.isNever())
        return CopyWaitsForInLinkLayouts::WhateverLiesBesideFields;
    FlowNodeID unseen = flow.unseen();
    LinkType anything = LinkType::anything();
    const LinkType& subject = change.subject;
    const LinkType& other = change.other;
    bool onlyWritesOnBuiltin = namesBuiltinAloneInLinkLayouts(subject) && (change.namesOfCopyLeaveLinkAlone || flow.program.builtins().hasLockedPrototypeLink(subject.builtin))
        && (change.definesWhatItCopies || !lookupPassesHolderLinkedAnew(flow.program, subject.builtin, { }));
    bool landsOnObjectsOfProgramAlone = !subject.hasUnknownObjects && !subject.builtin && subject.functions == LinkType::Functions::None && subject.classes == LinkType::Classes::None;
    auto isOfObjectsThatNobodyLists = [](const LinkType& type) {
        return type.hasUnknownObjects || type.builtin || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || (type.primitives & LinkType::closed(Primitive::AnyString));
    };
    bool assignmentFindsNoSetter = !flow.program.admitsReplacedBuiltins && !change.definesWhatItCopies && change.otherNode && landsOnObjectsOfProgramAlone && !isOfObjectsThatNobodyLists(other)
        && isListedOnBothSidesInLinkLayouts(flow, subject, other) && !mayHaveOwnValueUnderPrototypeLinkInLinkLayouts(flow, other);
    bool definesWhatItCopies = change.definesWhatItCopies || assignmentFindsNoSetter;
    KeysOfCopyInLinkLayouts keys;
    bool isListedOnBothSides = change.otherNode && copyFieldByFieldInLinkLayouts(flow, change.function, subject, other, definesWhatItCopies, change.isSpelledByLoop, keys);
    for (FlowNodeID field : keys.fieldsCopiedOntoThemselves)
        flow.observe(field, FlowObserver { FlowObserver::Kind::FieldIsOnlyAsked, change.function, change.record });
    bool copiesFieldByField = isListedOnBothSides && landsOnObjectsOfProgramAlone;
    if (change.otherNode && subject.functions != LinkType::Functions::None) {
        forEachLayoutInType(flow, other, [&](LayoutID layout) {
            for (const LinkFlow::OwnValue& value : flow.ownValuesOf(layout)) {
                if (value.key)
                    flow.addEdge(value.node, flow.foundBesideFieldsUnder(value.key));
            }
        });
    }

    forEachLayoutInType(flow, other, [&](LayoutID layout) {
        if (listsAccessorInLinkLayouts(flow, layout))
            flow.escape(layout);
    });

    FlowNodeID read;
    switch (other.objects) {
    case LinkType::Objects::None:
        break;
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        handToUnseenCodeInLinkLayouts(flow, change.otherNode, other);
        break;
    case LinkType::Objects::Layout:
    case LinkType::Objects::Set:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        if (!copiesFieldByField)
            read = flow.anyFieldReadThrough(other.objects, other.layout, other.family);
        break;
    }
    forEachElementsNodeInLinkLayouts(flow, other, [&](FlowNodeID elements) {
        flow.addEdge(elements, unseen);
    });

    if (!onlyWritesOnBuiltin && (!namesOnlyInstancesInLinkLayouts(subject) || subject.classes != LinkType::Classes::None))
        handToUnseenCodeInLinkLayouts(flow, change.subjectNode, subject);
    if (read)
        flow.addEdge(read, unseen);
    forEachElementsNodeInLinkLayouts(flow, subject, [&](FlowNodeID elements) {
        flow.arrive(elements, anything);
    });

    uint16_t texts = LinkType::closed(Primitive::AnyString);
    bool isNoObjectOfProgram = other.builtin || other.functions != LinkType::Functions::None || other.classes != LinkType::Classes::None;
    bool gainsInSight = !isListedOnBothSides || isNoObjectOfProgram || other.hasDirectUnknownObjects() || (other.directPrimitives() & texts) || !keys.findFieldsInTarget || keys.somethingSeenLiesBesideFields;
    bool gainsAtAll = gainsInSight || other.hasUnknownObjects || (other.primitives & texts) || keys.somethingLiesBesideFields;
    if (!definesWhatItCopies)
        addChangesToLayoutsNamedInLinkLayouts(flow, subject, changesMadeByInLinkLayouts(SummaryChange::Kind::CopiesInto));
    else if (gainsInSight)
        addChangesToLayoutsNamedInLinkLayouts(flow, subject, { LayoutChange::GainsProperty });
    else if (gainsAtAll) {
        forEachLayoutInType(flow, subject, [&](LayoutID layout) {
            if (!flow.layoutsThatGainWhatUnseenCodeGaveSources.get(layout.index()))
                flow.note(LinkFlow::Note::LayoutGainsWhatUnseenCodeGaveSources, layout.index());
        });
    }
    forEachLayoutInType(flow, subject, [&](LayoutID layout) {
        if (flow.program.at(layout).kind != LayoutKind::Record || listsAccessorInLinkLayouts(flow, layout))
            flow.escape(layout);
    });
    if (subject.objects == LinkType::Objects::Lost)
        handToUnseenCodeInLinkLayouts(flow, change.subjectNode, subject);

    bool copiesWhatNobodyLists = !copiesFieldByField || other.hasUnknownObjects || other.builtin || other.functions != LinkType::Functions::None || other.classes != LinkType::Classes::None || (other.primitives & LinkType::closed(Primitive::AnyString));
    bool mayLayPrototype = !onlyWritesOnBuiltin && !definesWhatItCopies && (copiesWhatNobodyLists || mayHaveOwnValueUnderPrototypeLinkInLinkLayouts(flow, other));
    if (mayLayPrototype) {
        ObservedChangeInLinkLayouts relink = change;
        observeChangeOfPrototypeInLinkLayouts(flow, relink);
    }
    if (definesWhatItCopies && change.otherNode && !holdsOnlyObjectsWithListedOwnValuesInLinkLayouts(flow, other))
        handToUnseenCodeInLinkLayouts(flow, change.otherNode, other);
    if (!definesWhatItCopies || copiesWhatNobodyLists || read) {
        for (FlowNodeID stored : nodesLaidUnderAnyKeyByCopyInLinkLayouts(flow, subject, definesWhatItCopies)) {
            if (copiesWhatNobodyLists)
                flow.arrive(stored, anything);
            if (read)
                flow.addEdge(read, stored);
        }
    }
    if (assignmentFindsNoSetter)
        return CopyWaitsForInLinkLayouts::WhateverLiesBesideFields;
    if (definesWhatItCopies ? gainsInSight : mayLayPrototype)
        return CopyWaitsForInLinkLayouts::Nothing;
    return definesWhatItCopies && gainsAtAll ? CopyWaitsForInLinkLayouts::WhatIsSeenAlone : CopyWaitsForInLinkLayouts::WhateverLiesBesideFields;
}

bool LinkFlow::isPlainClosureOrConstructorFunction(FunctionID function) const
{
    LayoutID constructed = program.at(function).constructedClass;
    return !constructed || isOfConstructorFunction(constructed);
}

bool LinkFlow::isWatchedForFirstTime(FlowNodeID node, const FlowObserver& observer)
{
    std::pair<uint64_t, uint64_t> words { static_cast<uint64_t>(observer.function.index() + 1) << 32 | observer.record, static_cast<uint64_t>(observer.kind) << 32 | (node.index() + 1) };
    PartOfNodesThatDefinitionsWatch& part = m_nodesOfDescriptorsThatDefinitionsWatch[DefaultHash<std::pair<uint64_t, uint64_t>>::hash(words) >> (32 - log2OfPartsOfNodesThatDefinitionsWatch)];
    while (part.isLocked.load(std::memory_order_relaxed) || part.isLocked.exchange(true, std::memory_order_acquire))
        simde_mm_pause();
    bool isFirstTime = part.watched.add(words).isNewEntry;
    part.isLocked.store(false, std::memory_order_release);
    return isFirstTime;
}

bool LinkFlow::descriptorsInSightLayValuesAlone(const LinkType& descriptors, const FlowObserver* toldAgain, Vector<FlowNodeID, 4>& values)
{
    bool hasOnlyRecords = (descriptors.objects == LinkType::Objects::Layout || descriptors.objects == LinkType::Objects::Set) && descriptors.functions == LinkType::Functions::None && descriptors.classes == LinkType::Classes::None
        && !descriptors.builtin && !descriptors.hasUnknownObjects && !descriptors.holdsLostBuiltins;
    if (!hasOnlyRecords)
        return false;
    PropertyKeyID halves[] { program.keyOf(BuiltinNames::get), program.keyOf(BuiltinNames::set) };
    PropertyKeyID value = program.keyOf(BuiltinNames::value);
    for (BuiltinName name : { BuiltinNames::enumerable, BuiltinNames::configurable, BuiltinNames::writable, BuiltinNames::value, BuiltinNames::get, BuiltinNames::set }) {
        if (!isAbsentFromObjectPrototype(program.keyOf(name)))
            return false;
    }
    auto nodeUnder = [&](LayoutID layout, PropertyKeyID key) {
        std::optional<FieldReference> listed = findField(program, layout, key);
        return listed ? field(*listed) : appendedField(layout, key);
    };
    bool laysValuesAlone = true;
    auto staysEmpty = [&](FlowNodeID node) {
        if (!typeAt(node).isNever()) {
            laysValuesAlone = false;
            return;
        }
        if (!toldAgain)
            return;
        if (!isWatchedForFirstTime(node, *toldAgain))
            return;
        LockerOfWhatIsShared locker(*this);
        observe(node, *toldAgain);
    };
    forEachLayoutOf(descriptors, [&](LayoutID layout) {
        const Layout& record = program.at(layout);
        if (record.kind != LayoutKind::Record || record.has(LayoutFlags::IsOfCreatedObject) || escapedLayouts.get(layout.index()) || lostLayouts.get(layout.index()) || listsAccessorInLinkLayouts(*this, layout) || hasAccessorWritten(layout)) {
            laysValuesAlone = false;
            return;
        }
        staysEmpty(whereLossIsTold(layout));
        staysEmpty(extraPropertyReadNode(layout));
        for (PropertyKeyID half : halves)
            staysEmpty(nodeUnder(layout, half));
        values.append(nodeUnder(layout, value));
    });
    return laysValuesAlone;
}

void LinkFlow::changeObserved(const FlowObserver& observer, const LinkType& now)
{
    switch (observer.kind) {
    case FlowObserver::Kind::SubjectOfChange:
    case FlowObserver::Kind::OtherOfChange:
    case FlowObserver::Kind::FieldOfAccessorLaid:
        break;
    case FlowObserver::Kind::SubjectOfCall:
    case FlowObserver::Kind::CalleeOfCall:
    case FlowObserver::Kind::ReceiverOfCall:
    case FlowObserver::Kind::SubjectOfAccess:
    case FlowObserver::Kind::KeyOfAccess:
    case FlowObserver::Kind::MemberOfAccess:
    case FlowObserver::Kind::SymbolOfPreparation:
    case FlowObserver::Kind::OperandOfOperator:
    case FlowObserver::Kind::Inspected:
    case FlowObserver::Kind::MemberOfSharedCall:
    case FlowObserver::Kind::MemberOfSharedAccess:
    case FlowObserver::Kind::PassedToBeNamed:
    case FlowObserver::Kind::OperandOnlyLookedAt:
    case FlowObserver::Kind::KeysAreListed:
    case FlowObserver::Kind::FieldIsOnlyAsked:
        RELEASE_ASSERT_NOT_REACHED();
    }

    auto withoutKindsOfPrimitives = [](LinkType type) {
        constexpr uint16_t texts = LinkType::closed(Primitive::AnyString);
        if (type.primitives)
            type.primitives = (type.primitives & Primitive::Null) | (type.primitives & texts ? texts : 0) | Primitive::Undefined;
        return type;
    };
    const SummaryChange& record = summaries.at(observer.function).changes[observer.record];
    if (record.keyNode == noSummaryNode && !typeObservedBefore().isNever() && withoutKindsOfPrimitives(typeObservedBefore()) == withoutKindsOfPrimitives(now) && typeObservedBefore().hasDirectUnknownObjects() == now.hasDirectUnknownObjects())
        return;

    UnknownByEscapeAlone unknownByEscapeAlone(isUnknownByEscapeAloneIfAtAll(typeAt(nodeOf(observer.function, record.subject))) && (record.other == noSummaryNode || isUnknownByEscapeAloneIfAtAll(typeAt(nodeOf(observer.function, record.other)))), UnknownByEscapeAlone::Marks::OfRule);
    ObservedChangeInLinkLayouts change;
    change.function = observer.function;
    change.record = observer.record;
    change.fieldIsAsked = observer.kind == FlowObserver::Kind::FieldOfAccessorLaid;
    change.subjectNode = nodeOf(observer.function, record.subject);
    change.subject = typeAt(change.subjectNode);
    change.key = record.key;
    change.keyIsWritten = (record.key && record.keyNode == noSummaryNode) || record.isUnderNameThatNobodyElseNames;
    if (record.keyNode != noSummaryNode) {
        const LinkType& key = typeAt(nodeOf(observer.function, record.keyNode));
        if (key.isNever())
            return;
        change.key = keyHeldAt(observer.function, record.keyNode);
        change.keyIsSomeSymbol = holdsSymbolsAlone(key);
    }
    change.lays = record.lays;
    change.descriptorIsInSight = record.descriptorIsInSight;
    change.descriptorLaysNoAccessor = record.descriptorLaysNoAccessor;
    change.isEntryWrittenInMap = record.isEntryWrittenInMap;
    change.laysAccessorAsWritten = record.laysAccessorAsWritten;
    change.laysValueInSight = record.laysValueInSight;
    if (record.valueInSight != noSummaryNode)
        change.valueInSight = nodeOf(observer.function, record.valueInSight);
    if (record.valueLaidThroughConstant != noSummaryNode)
        change.valueLaidThroughConstant = nodeOf(observer.function, record.valueLaidThroughConstant);
    change.definesWhatItCopies = record.definesWhatItCopies;
    change.isSpelledByLoop = record.isSpelledByLoop;
    change.heirIsMadeByCall = record.heirIsMadeByCall;
    change.laidAboveByConstructor = findWhatConstructorLaysAbove(observer.function, record);
    if (record.getterInSight != noSummaryNode)
        change.getterInSight = nodeOf(observer.function, record.getterInSight);
    if (record.setterInSight != noSummaryNode)
        change.setterInSight = nodeOf(observer.function, record.setterInSight);
    if (record.other != noSummaryNode) {
        change.otherNode = nodeOf(observer.function, record.other);
        change.other = typeAt(change.otherNode);
    }
    if (change.subject.isNever() || (change.isSpelledByLoop && !followsCopySpelledByLoop(change.subject, change.other)))
        return;
    noteLossOfLightFormAmong(change.subject);
    noteLossOfLightFormAmong(change.other);
    if (record.kind != SummaryChange::Kind::BecomesPrototype) {
        if (change.subject.builtin && !isSpelledInBody(observer.function, record.subject))
            note(Note::BuiltinUsedViaFlow, program.builtins().canonicalID(change.subject.builtin).index());
        if (change.other.builtin && !isSpelledInBody(observer.function, record.other))
            note(Note::BuiltinUsedViaFlow, program.builtins().canonicalID(change.other.builtin).index());
    }

    CopiesObserved& copies = copiesObservedIn(observer.function);
    uint64_t placeOfCopy = static_cast<uint64_t>(observer.function.index()) << 32 | observer.record;
    if (record.kind == SummaryChange::Kind::CopiesInto) {
        auto lookedAt = copies.lookedAt.find(placeOfCopy);
        if (lookedAt != copies.lookedAt.end() && lookedAt->value.subject == change.subject && lookedAt->value.other == change.other
            && lookedAt->value.subject.hasSameShadowsAs(change.subject) && lookedAt->value.other.hasSameShadowsAs(change.other)
            && (lookedAt->value.waitsForNothingBesideFields || (lookedAt->value.waitsForWhatIsSeenAlone && now.directPart().isNever())))
            return;
    }

    if ((record.kind == SummaryChange::Kind::DefinesByDescriptor && !record.descriptorLaysNoAccessor) || (record.kind == SummaryChange::Kind::CopiesInto && record.definesWhatItCopies))
        noteAccessorMayBeLaidOn(change.subject, record.kind == SummaryChange::Kind::DefinesByDescriptor ? change.key : PropertyKeyID(), observer.function);
    if (record.kind == SummaryChange::Kind::ChangesPrototype && !putsNullAloneAboveRecordsInLinkLayouts(*this, change))
        noteAccessorMayBeLaidOn(change.subject, { }, observer.function);
    if (record.kind == SummaryChange::Kind::Deletes || record.kind == SummaryChange::Kind::DefinesByDescriptor) {
        noteWriteOn(change.subject, change.key, observer.function, record.kind == SummaryChange::Kind::Deletes ? WhatIsStored::Nothing : record.laysNewValueOfNativeCode || record.laysTextNumberOrBooleanThatIsWritten ? WhatIsStored::TextsNumbersAndBooleans : WhatIsStored::Anything);
        if (!change.key && record.other != noSummaryNode)
            noteSiteOfCopy(observer.function, record.site, change.subjectNode, change.otherNode);
        else
            noteSiteOfWrite(observer.function, record.site, change.subjectNode);
    } else if (record.kind == SummaryChange::Kind::CopiesInto) {
        Vector<PropertyKeyID> keys;
        bool knowsEveryKey = change.subject.builtin && record.other != noSummaryNode && !change.other.hasUnknownObjects && !change.other.holdsLostBuiltins
            && !(change.other.primitives & LinkType::closed(Primitive::AnyString)) && holdsOnlyObjectsWithListedOwnValuesInLinkLayouts(*this, change.other);
        if (knowsEveryKey) {
            forEachLayoutInType(*this, change.other, [&](LayoutID layout) {
                for (const OwnValue& value : ownValuesOf(layout)) {
                    if (value.key)
                        keys.append(value.key);
                    else
                        knowsEveryKey &= typeAt(value.node).isNever();
                }
            });
        }
        WhatIsStored stored = record.subjectIsMadeInBody ? WhatIsStored::TextsNumbersAndBooleans : WhatIsStored::Anything;
        if (!knowsEveryKey)
            noteWriteOn(change.subject, { }, observer.function, stored, record.subjectIsMadeInBody);
        else {
            for (PropertyKeyID key : keys)
                noteWriteOn(change.subject, key, observer.function, stored, record.subjectIsMadeInBody);
        }
        change.namesOfCopyLeaveLinkAlone = knowsEveryKey && std::ranges::none_of(keys, [&](PropertyKeyID key) { return nameClassOf(key) == NameClass::Proto; });
    if (record.kind == SummaryChange::Kind::CopiesInto) {
        forEachLayoutInType(*this, change.other, [&](LayoutID layout) {
            for (const OwnValue& value : ownValuesThatMayHaveNoKeyOf(layout)) {
                if (!value.key && copies.toldOfWhatLiesBesideFields.add(std::pair { static_cast<uint64_t>(value.node.index()) << 32 | observer.function.index(), observer.record }).isNewEntry)
                    observe(value.node, FlowObserver { FlowObserver::Kind::OtherOfChange, observer.function, observer.record });
            }
        });
    }
        if (record.other != noSummaryNode)
            noteSiteOfCopy(observer.function, record.site, change.subjectNode, change.otherNode);
    }

    switch (record.kind) {
    case SummaryChange::Kind::Freezes:
        observeFreezeInLinkLayouts(*this, change);
        return;
    case SummaryChange::Kind::Deletes:
        observeDeletionInLinkLayouts(*this, change);
        return;
    case SummaryChange::Kind::DefinesByDescriptor:
        observeDefinitionInLinkLayouts(*this, change);
        return;
    case SummaryChange::Kind::ChangesPrototype:
        if (!putsNullAloneAboveRecordsInLinkLayouts(*this, change))
            noteWhatMayMakeThenableOf(change.subject, observer.function, std::nullopt);
        observeChangeOfPrototypeInLinkLayouts(*this, change);
        return;
    case SummaryChange::Kind::BecomesPrototype:
        observeNewHeirInLinkLayouts(*this, change);
        return;
    case SummaryChange::Kind::CopiesInto:
    {
        CopyWaitsForInLinkLayouts waitsFor = observeCopyInLinkLayouts(*this, change);
        copies.lookedAt.set(placeOfCopy, CopyLookedAt { change.subject, change.other, waitsFor == CopyWaitsForInLinkLayouts::Nothing, waitsFor == CopyWaitsForInLinkLayouts::WhatIsSeenAlone });
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class SetBitsInOrderForLinkLayouts {
public:
    explicit SetBitsInOrderForLinkLayouts(const BitVector& bits)
    {
        bits.forEachSetBit([&](size_t bit) {
            m_bits.append(static_cast<uint32_t>(bit));
        });
    }

    std::span<const uint32_t> within(size_t first, size_t length) const
    {
        auto begin = std::ranges::lower_bound(m_bits, first);
        auto end = std::lower_bound(begin, m_bits.end(), first + length);
        return m_bits.span().subspan(begin - m_bits.begin(), end - begin);
    }

    bool holdsTheSameIn(size_t first, size_t second, size_t length) const
    {
        std::span<const uint32_t> inFirst = within(first, length);
        std::span<const uint32_t> inSecond = within(second, length);
        if (inFirst.size() != inSecond.size())
            return false;
        for (size_t index = 0; index < inFirst.size(); ++index) {
            if (inFirst[index] - first != inSecond[index] - second)
                return false;
        }
        return true;
    }

private:
    Vector<uint32_t> m_bits;
};

static bool constructionsAreFollowedInLinkLayouts(const Layout& layout)
{
    return isFoundByShapeTest(layout) && !layout.isDerived();
}

namespace {

class RowOfConstructorsInLinkLayouts {
public:
    enum class Follows : uint8_t { UntilFirstSight, UntilEnds };

    RowOfConstructorsInLinkLayouts(const LinkFlow& flow, LayoutID made, Follows follows = Follows::UntilFirstSight)
        : m_flow(flow)
        , m_program(flow.program)
        , m_made(made)
        , m_namesMayBeIncomplete(flow.namesMayBeIncomplete(made, ClassSide::Prototype))
        , m_goesToEnds(follows == Follows::UntilEnds)
    {
    }

    bool followWholeRow() { return followRow({ }) && m_hasGoneThroughRow; }

    Vector<PropertyKeyID, 32>& filled() LIFETIME_BOUND { return m_filled; }
    Vector<PropertyKeyID, 32>& lookedUp() LIFETIME_BOUND { return m_lookedUp; }
    bool hasLookedUpNameThatEveryClassHides() const { return m_hasLookedUpNameThatEveryClassHides; }
    Vector<PropertyKeyID, 32>& keysThatHoldObject() LIFETIME_BOUND { return m_keysThatHoldObject; }
    void setKeysThatHoldObjectAbove(std::span<const PropertyKeyID> keys) { m_keysThatHoldObjectAbove = keys; }

    std::optional<Vector<PropertyKeyID, 32>> findKeysFilledAbove(LayoutID classInRow)
    {
        if (!followRow(classInRow))
            return std::nullopt;
        return WTF::move(m_filled);
    }

private:
    bool followRow(LayoutID end)
    {
        Vector<LayoutID, 8> row;
        for (LayoutID current = m_made; current; current = m_program.at(current).base) {
            if (row.size() == m_program.layouts.size())
                return false;
            row.append(current);
        }
        if (end && !row.contains(end))
            return false;
        m_hasGoneThroughRow = followConstructorsOf(row.span(), end);
        return true;
    }

    size_t numberOfConstructorsThatMakeObject(std::span<const LayoutID> row) const
    {
        for (size_t index = 0; index < row.size(); ++index) {
            const Layout& layout = m_program.at(row[index]);
            if (!layout.constructor || m_program.at(layout.constructor).kind != FunctionKind::DerivedConstructor)
                return index + 1;
        }
        return row.size();
    }

    bool followConstructorsOf(std::span<const LayoutID> wholeRow, LayoutID end)
    {
        std::span<const LayoutID> row = m_goesToEnds ? wholeRow : wholeRow.first(numberOfConstructorsThatMakeObject(wholeRow));
        if (end && !std::ranges::contains(row, end))
            return false;
        for (size_t index = row.size(); index-- && row[index] != end;) {
            if (!followConstructorOf(row[index], index + 1 == row.size()))
                return false;
        }
        return true;
    }

public:
    bool followConstructorOf(LayoutID classInRow, bool isTopmost)
    {
        const Layout& layout = m_program.at(classInRow);
        if (layout.kind != LayoutKind::Instance || !layout.constructor || m_program.at(layout.constructor).constructedClass != classInRow)
            return false;
        const Function& constructor = m_program.at(layout.constructor);
        SummaryConstruction::Object object = SummaryConstruction::Object::AnsweredBySuperCall;
        if (!isTopmost) {
            if (constructor.kind != FunctionKind::DerivedConstructor)
                return false;
        } else if (layout.has(LayoutFlags::HasHeritage))
            return false;
        else if (constructor.kind == FunctionKind::BaseConstructor)
            object = SummaryConstruction::Object::Allocated;
        else if (constructor.takesThisFromConstruction())
            object = SummaryConstruction::Object::This;
        else
            return false;
        return addWhatBodyFills(layout.constructor, object, true);
    }

private:

    bool addWhatBodyFills(FunctionID body, SummaryConstruction::Object object, bool mayHaveParts)
    {
        const ThinSummary& summary = m_flow.summaries.at(body);
        if (!m_program.at(body).hasBody() || summary.has(SummaryTrait::HasDirectEval))
            return false;
        const SummaryConstruction* construction = nullptr;
        for (const SummaryConstruction& candidate : summary.constructions) {
            if (candidate.object != object)
                continue;
            if (construction)
                return false;
            construction = &candidate;
        }
        if (!construction || (construction->numberOfParts && !mayHaveParts))
            return false;

        if (m_goesToEnds)
            return addWhatBodyFillsToItsEnd(summary, *construction);

        std::span<const SummaryConstruction::Key> keys = summary.keysOfConstructions.span().subspan(construction->firstKey, construction->numberOfKeys);
        for (const SummaryConstruction::Key& key : keys) {
            if (key.isLookedUp && whatLookUpFinds(key.key) == Found::Accessor)
                return false;
        }

        size_t endOfKeysOfOthers = m_keysThatHoldObject.size();
        for (const SummaryConstruction::Key& key : keys) {
            if (key.holdsObject)
                m_keysThatHoldObject.append(key.key);
        }
        size_t endOfOwnKeys = m_keysThatHoldObject.size();
        for (const SummaryConstruction::Part& part : summary.partsOfConstructions.span().subspan(construction->firstPart, construction->numberOfParts)) {
            size_t numberFilledBefore = m_filled.size();
            bool isStillUnseen = addWhatBodyFills(part.function, SummaryConstruction::Object::This, false);
            if (!part.runsOnEveryWay)
                m_filled.shrink(numberFilledBefore);
            if (!isStillUnseen)
                return false;
        }

        std::span<const PropertyKeyID> held = m_keysThatHoldObject.span();
        for (const SummaryConstruction::Key& key : keys) {
            if (key.isLookedUp && (std::ranges::binary_search(m_keysThatHoldObjectAbove, key.key) || std::ranges::contains(held.first(endOfKeysOfOthers), key.key) || std::ranges::contains(held.subspan(endOfOwnKeys), key.key)))
                return false;
        }

        for (const SummaryConstruction::Key& key : keys) {
            if (key.isFilled && (!key.isLookedUp || whatLookUpFinds(key.key) == Found::Nothing))
                m_filled.append(key.key);
        }
        return !construction->isSeenFromOutside;
    }

    bool addWhatBodyFillsToItsEnd(const ThinSummary& summary, const SummaryConstruction& construction)
    {
        for (const SummaryConstruction::Part& part : summary.partsOfConstructions.span().subspan(construction.firstPart, construction.numberOfParts)) {
            size_t numberFilledBefore = m_filled.size();
            addWhatBodyFills(part.function, SummaryConstruction::Object::This, false);
            if (!part.runsOnEveryWayToEnd)
                m_filled.shrink(numberFilledBefore);
        }
        m_filled.append(summary.keysFilledAtEnds.span().subspan(construction.firstKeyFilledAtEnd, construction.numberOfKeysFilledAtEnd));
        return true;
    }

    enum class Found : uint8_t { Nothing, Method, Accessor };

    Found whatLookUpFinds(PropertyKeyID key)
    {
        m_lookedUp.append(key);
        m_hasLookedUpNameThatEveryClassHides |= classHolderIsMadeWithName(m_program, ClassSide::Prototype, key);
        if (m_namesMayBeIncomplete || m_program.nameClassOf(key) == NameClass::Proto)
            return Found::Accessor;
        const PropertyKey& record = m_program.at(key);
        if (record.kind == PropertyKeyKind::Text && m_program.isOpenedSomewhere(record.text))
            return Found::Accessor;
        if (std::optional<FieldReference> field = findField(m_program, m_made, key); field && fieldAt(m_program, *field).form.contains(FieldForm::IsAccessor))
            return Found::Accessor;
        Found found = m_flow.isAbsentFromObjectPrototype(key) ? Found::Nothing : Found::Method;
        for (const LinkFlow::FoundMember& member : m_flow.findMembers(m_made, ClassSide::Prototype, key)) {
            if (member.kind != MethodKind::Method)
                return Found::Accessor;
            found = Found::Method;
        }
        return found;
    }

    const LinkFlow& m_flow;
    const Program& m_program;
    LayoutID m_made;
    bool m_namesMayBeIncomplete;
    bool m_goesToEnds;
    bool m_hasGoneThroughRow { false };
    bool m_hasLookedUpNameThatEveryClassHides { false };
    Vector<PropertyKeyID, 32> m_filled;
    Vector<PropertyKeyID, 32> m_lookedUp;
    Vector<PropertyKeyID, 32> m_keysThatHoldObject;
    std::span<const PropertyKeyID> m_keysThatHoldObjectAbove;
};

} // anonymous namespace

void LinkFlow::noteFieldGivenAccessorByDescriptorInSight(FieldReference reference)
{
    uint32_t cell = m_firstFieldOfLayout[reference.layout.index()] + reference.field;
    if (!fieldsGivenAccessorsByDescriptorsInSight.get(cell))
        note(Note::FieldGivenAccessorByDescriptorInSight, cell);
}

void LinkFlow::noteFieldMayBeEmpty(FieldReference reference)
{
    uint32_t cell = m_firstFieldOfLayout[reference.layout.index()] + reference.field;
    if (!fieldsThatMayBeEmpty.get(cell))
        note(Note::FieldMayBeEmpty, cell);
}

FlowNodeID LinkFlow::foundWhereFieldIsEmpty(FieldReference reference)
{
    return nodeAskedOnceByLane(MadeOnDemand::FoundWhereFieldIsEmpty, static_cast<uint64_t>(reference.layout.index()) << 32 | reference.field, [&]() -> FlowNodeID {
        uint32_t cell = m_firstFieldOfLayout[reference.layout.index()] + reference.field;
        if (m_nodesFoundWhereFieldsAreEmpty.isEmpty())
            m_nodesFoundWhereFieldsAreEmpty.fill(UINT32_MAX, m_firstFieldOfLayout.last());
        if (m_nodesFoundWhereFieldsAreEmpty[cell] != UINT32_MAX)
            return FlowNodeID(m_nodesFoundWhereFieldsAreEmpty[cell]);

        FlowNodeID node = makeNode();
        m_nodesFoundWhereFieldsAreEmpty[cell] = node.index();
        if (fieldsThatMayBeEmpty.get(cell))
            arrive(node, fromTypeID(TypeTable::undefined));
        else if (!constructionsAreFollowedInLinkLayouts(program.at(reference.layout)))
            note(Note::FieldMayBeEmpty, cell);
        return node;
    });
}

bool LinkFlow::mayReadInFrontOfDefinition(FunctionID reader, const SummaryAccess& access, LayoutID layout) const
{
    if (!access.isPartOfConstruction || !access.subjectIsAnsweredBySuperCall)
        return access.isPartOfConstruction;
    std::optional<Vector<PropertyKeyID, 32>> filledAbove = RowOfConstructorsInLinkLayouts(*this, layout).findKeysFilledAbove(program.at(reader).constructedClass);
    return filledAbove && !filledAbove->contains(access.key);
}

void LinkFlow::findWhatRowsOfConstructorsFill()
{
    using Row = RowOfConstructorsInLinkLayouts;
    struct DownToClass {
        uint32_t filled { 0 };
        uint32_t lookedUp { 0 };
        uint32_t holdsObject { 0 };
        bool goesOn { true };
        bool hasLookedUpNameThatEveryClassHides { false };
        bool holdsWhatIsFilledAbove { false };
    };
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    Vector<Vector<PropertyKeyID>>& lists = m_listsOfKeysThatRowsOfConstructorsFill;
    lists.append(Vector<PropertyKeyID>());
    m_keysThatRowsOfConstructorsFill.grow(numberOfLayouts);
    Vector<Vector<uint32_t>>& listsOfFields = m_listsOfFieldsThatRowsOfConstructorsLeaveEmpty;
    listsOfFields.append(Vector<uint32_t>());
    auto placeOfFieldsLeftEmpty = [&](LayoutID id, uint32_t filled, std::optional<uint32_t> leftEmptyInBase) {
        const Layout& layout = program.at(id);
        Vector<uint32_t> leftEmpty;
        auto ask = [&](uint32_t field) {
            if (!layout.fields[field].key || !std::ranges::binary_search(lists[filled], layout.fields[field].key))
                leftEmpty.append(field);
        };
        uint32_t numberAskedOneByOne = leftEmptyInBase ? layout.ownFieldCount : static_cast<uint32_t>(layout.fields.size());
        for (uint32_t field = 0; field < numberAskedOneByOne; ++field)
            ask(field);
        if (leftEmptyInBase) {
            for (uint32_t fieldOfBase : listsOfFields[*leftEmptyInBase])
                ask(layout.ownFieldCount + fieldOfBase);
        }
        if (leftEmpty.isEmpty())
            return 0u;
        listsOfFields.append(WTF::move(leftEmpty));
        return static_cast<uint32_t>(listsOfFields.size() - 1);
    };

    uint32_t placeUnderListMadeLast = UINT32_MAX;
    Vector<PropertyKeyID, 32> addedToListMadeLast;
    auto placeOfListWith = [&](uint32_t place, Vector<PropertyKeyID, 32>& added) {
        std::ranges::sort(added);
        added.shrink(std::ranges::unique(added).begin() - added.begin());
        added.removeAllMatching([&](PropertyKeyID key) {
            return std::ranges::binary_search(lists[place], key);
        });
        if (added.isEmpty())
            return place;
        if (placeUnderListMadeLast == place && addedToListMadeLast == added)
            return static_cast<uint32_t>(lists.size() - 1);
        Vector<PropertyKeyID> merged(lists[place].size() + added.size());
        std::ranges::merge(lists[place], added, merged.begin());
        lists.append(WTF::move(merged));
        placeUnderListMadeLast = place;
        addedToListMadeLast = added;
        return static_cast<uint32_t>(lists.size() - 1);
    };
    auto hasAccessorFieldUnder = [&](LayoutID layout, PropertyKeyID key) {
        std::optional<FieldReference> field = findField(program, layout, key);
        return field && fieldAt(program, *field).form.contains(FieldForm::IsAccessor);
    };
    auto mayFindOtherThanClassAbove = [&](LayoutID id, const DownToClass& above) {
        const Vector<PropertyKeyID>& lookedUp = lists[above.lookedUp];
        if (lookedUp.isEmpty())
            return false;
        LayoutID base = program.at(id).base;
        if (above.hasLookedUpNameThatEveryClassHides || namesMayBeIncomplete(id, ClassSide::Prototype) != namesMayBeIncomplete(base, ClassSide::Prototype))
            return true;
        for (const MemberOfClass& member : membersOf(id, ClassSide::Prototype)) {
            if (std::ranges::binary_search(lookedUp, member.key))
                return true;
        }
        return std::ranges::any_of(program.at(id).fields, [&](const Field& field) {
            return !field.flags.contains(FieldFlags::IsInherited) && field.key && std::ranges::binary_search(lookedUp, field.key) && hasAccessorFieldUnder(id, field.key) != hasAccessorFieldUnder(base, field.key);
        });
    };
    auto withConstructorOf = [&](LayoutID id, Row::Follows follows, const DownToClass* above) {
        Row row(*this, id, follows);
        DownToClass result;
        if (above && mayFindOtherThanClassAbove(id, *above))
            result.goesOn = row.followWholeRow();
        else {
            if (above)
                result = *above;
            result.holdsWhatIsFilledAbove = above;
            if (!result.goesOn)
                return result;
            if (above)
                row.setKeysThatHoldObjectAbove(lists[above->holdsObject].span());
            result.goesOn = row.followConstructorOf(id, !above);
        }
        result.filled = placeOfListWith(result.filled, row.filled());
        result.lookedUp = placeOfListWith(result.lookedUp, row.lookedUp());
        result.holdsObject = placeOfListWith(result.holdsObject, row.keysThatHoldObject());
        result.hasLookedUpNameThatEveryClassHides |= row.hasLookedUpNameThatEveryClassHides();
        return result;
    };

    BitVector rowsThatNeverEnd = answerAlongRows(numberOfLayouts, true, [&](LayoutID id) -> StepAlongRow {
        if (LayoutID base = program.at(id).base)
            return { std::nullopt, base };
        return { false, { } };
    });
    Vector<DownToClass> untilFirstSight(numberOfLayouts);
    Vector<DownToClass> untilEnds(numberOfLayouts);
    BitVector isKnown(numberOfLayouts);
    Vector<LayoutID, 32> rowNotKnown;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& asked = program.layouts[index];
        if (!asked.constructor || program.at(asked.constructor).constructedClass != LayoutID(index) || asked.fields.isEmpty() || rowsThatNeverEnd.quickGet(index))
            continue;
        for (LayoutID current(index); current && !isKnown.quickSet(current.index()); current = program.at(current).base)
            rowNotKnown.append(current);
        while (!rowNotKnown.isEmpty()) {
            LayoutID id = rowNotKnown.takeLast();
            const Layout& layout = program.at(id);
            bool takesObjectFromSuperCall = layout.base && layout.constructor && program.at(layout.constructor).kind == FunctionKind::DerivedConstructor;
            untilFirstSight[id.index()] = withConstructorOf(id, Row::Follows::UntilFirstSight, takesObjectFromSuperCall ? &untilFirstSight[layout.base.index()] : nullptr);
            untilEnds[id.index()] = withConstructorOf(id, Row::Follows::UntilEnds, layout.base ? &untilEnds[layout.base.index()] : nullptr);
            m_keysThatRowsOfConstructorsFill[id.index()] = { untilFirstSight[id.index()].filled, untilEnds[id.index()].filled };
            m_keysThatRowsOfConstructorsFill[id.index()].lookedUpUntilFirstSight = untilFirstSight[id.index()].lookedUp;
            if (!layout.base)
                continue;
            bool takesListOfBase = baseWhoseFieldListIsCopiedWhole(program, id) == layout.base;
            auto leftEmptyInBase = [&](const DownToClass& row, uint32_t place) {
                return takesListOfBase && row.holdsWhatIsFilledAbove && place != UINT32_MAX ? std::optional(place) : std::nullopt;
            };
            KeysThatRowOfConstructorsFills ofBase = m_keysThatRowsOfConstructorsFill[layout.base.index()];
            m_keysThatRowsOfConstructorsFill[id.index()].leftEmptyUntilFirstSight = placeOfFieldsLeftEmpty(id, untilFirstSight[id.index()].filled, leftEmptyInBase(untilFirstSight[id.index()], ofBase.leftEmptyUntilFirstSight));
            m_keysThatRowsOfConstructorsFill[id.index()].leftEmptyUntilEnds = placeOfFieldsLeftEmpty(id, untilEnds[id.index()].filled, leftEmptyInBase(untilEnds[id.index()], ofBase.leftEmptyUntilEnds));
        }
    }
}

void LinkFlow::forEachFieldLeftEmptyByRowOfConstructors(LayoutID made, uint32_t placeOfKeysFilled, uint32_t placeOfFieldsLeftEmpty, const Invocable<void(uint32_t)> auto& note) const
{
    if (placeOfFieldsLeftEmpty != UINT32_MAX) {
        for (uint32_t field : m_listsOfFieldsThatRowsOfConstructorsLeaveEmpty[placeOfFieldsLeftEmpty])
            note(field);
        return;
    }
    const Vector<PropertyKeyID>& filled = m_listsOfKeysThatRowsOfConstructorsFill[placeOfKeysFilled];
    const Layout& layout = program.at(made);
    for (uint32_t field = 0; field < layout.fields.size(); ++field) {
        if (!layout.fields[field].key || !std::ranges::binary_search(filled, layout.fields[field].key))
            note(field);
    }
}

void LinkFlow::noteConstructionsOf(FunctionID id)
{
    noteBirthsOf(id);
    summaries.at(id).forEachLayoutLeftEmpty([&](LayoutID layout, std::span<const uint32_t> fieldsHeld) {
        fieldsThatLaneAtWorkHasNotNotedEmpty(layout).removeAllMatching([&](uint32_t field) {
            if (std::ranges::binary_search(fieldsHeld, field))
                return false;
            noteFieldMayBeEmpty({ layout, field });
            return true;
        });
    });

    LayoutID made = program.at(id).constructedClass;
    if (!made || program.at(made).constructor != id || program.at(made).fields.isEmpty())
        return;
    const KeysThatRowOfConstructorsFills& row = m_keysThatRowsOfConstructorsFill[made.index()];
    forEachFieldLeftEmptyByRowOfConstructors(made, row.untilFirstSight, row.leftEmptyUntilFirstSight, [&](uint32_t field) {
        noteFieldMayBeEmpty({ made, field });
    });
}

namespace {

static bool isConstantUndefinedInLinkLayouts(const SummaryNode& node)
{
    return node.kind == SummaryNode::Kind::Constant && TypeID::fromBits(node.first) == TypeID(TypeTable::undefined);
}

static bool isConstantUndefinedOrNullInLinkLayouts(const SummaryNode& node)
{
    return node.kind == SummaryNode::Kind::Constant && (TypeID::fromBits(node.first) == TypeID(TypeTable::undefined) || TypeID::fromBits(node.first) == TypeID(TypeTable::null));
}

enum class TraitOfWriteInLinkLayouts : uint8_t {
    ComesAfterConstruction = 1 << 0,
    AddsWhatIsNotListed = 1 << 1,
    MayRunSetter = 1 << 2,
    MayGiveValue = 1 << 3,
    GivesUndefinedToObjectBeingMade = 1 << 4,
    FillsObjectBeingConstructed = 1 << 5,
    BringsUndefinedOrNull = 1 << 6,
};

} // anonymous namespace

class WriterOfLinkLayouts {
    WTF_MAKE_NONCOPYABLE(WriterOfLinkLayouts);
public:
    WriterOfLinkLayouts(const LinkFlow&, LinkTables&);

    void lookAtSummaries();
    void writeWhatMakesNoType(BitVector&& functionsWhoseCallersAreAllKnown);
    void write(TypeArena&);

private:
    using Trait = TraitOfWriteInLinkLayouts;
    using Traits = OptionSet<TraitOfWriteInLinkLayouts>;

    uint32_t cellOf(FieldReference reference) const { return m_tables.firstFieldOfLayout[reference.layout.index()] + reference.field; }
    LinkType typeAt(FunctionID function, uint32_t nodeInSummary) const { return m_flow.typeAt(m_flow.nodeOf(function, nodeInSummary)); }
    bool hasEscaped(uint32_t layout) const
    {
        if (m_flow.escapedLayoutsAsRead().get(layout) || m_isMadeAtNoNode.quickGet(layout))
            return true;
        return m_liesBelowLostClassObject.quickGet(layout) && m_flow.reading() == WhatUnseenCodeDoes::Anything;
    }
    uint32_t lostFamilyOf(LayoutID member) const { return m_flow.unitedLayouts.representativeOf(member.index()); }
    bool fieldsHaveUnseenWriters(uint32_t layout) const { return hasEscaped(layout) || !onlyCompiledCodeWritesFieldsInLinkLayouts(m_program.layouts[layout]); }
    bool isWrittenLateThroughLost(LayoutID member, PropertyKeyID key) const
    {
        uint32_t family = lostFamilyOf(member);
        return (m_lostFamiliesWrittenLateUnderEveryKey.get(family) && !isPrivateNameInLink(m_program, key)) || (key && m_keysWrittenLateThroughLost.contains(static_cast<uint64_t>(family) << 32 | key.bits()));
    }
    bool isGivenValueThroughLost(LayoutID member, PropertyKeyID key) const
    {
        uint32_t family = lostFamilyOf(member);
        return m_lostFamiliesGivenValuesUnderEveryKey.get(family) || (key && m_keysGivenValuesThroughLost.contains(static_cast<uint64_t>(family) << 32 | key.bits()));
    }

    TypeID unlessAnything(TypeID) const;
    enum class Numbers : bool { AsHeld, ByKindAlone };
    TypeID claimedTypeAt(FlowNodeID, Numbers = Numbers::AsHeld);
    bool claimLeavesOutWhatArrivesAt(FlowNodeID) const;
    TypeID namingSymbolAt(FlowNodeID, TypeID);
    TypeID claimedTypeAtEither(FlowNodeID, FlowNodeID);
    TypeID typeEstablishedOf(const DeclaredType&) const;
    TypeID narrowerOf(TypeID byFlow, const DeclaredType&);
    TypeID typeOfWrittenUnit(WrittenUnit);

    void lookAtEdges(FunctionID, const ThinSummary&);
    void lookAtAccesses(FunctionID, const ThinSummary&);
    void lookAtChanges(FunctionID, const ThinSummary&);
    void noteChangeOfClosure(const LinkType& subject, PropertyKeyID);
    void noteNameDefinedPastLists(const LinkType& holder, PropertyKeyID, bool isDefinition);
    void noteHeritage(const LinkType& heritage, FunctionID constructor);
    LayoutID findLayoutWhosePrototypeObjectIsAt(const ThinSummary&, uint32_t node) const;
    void noteNameWrittenOnPrototype(FunctionID, const ThinSummary&, const SummaryAccess&);
    void noteHalfOfAccessorLaidOnPrototype(FunctionID, const ThinSummary&, const SummaryChange&);
    bool isWrittenOnPrototypeAbove(LayoutID, PropertyKeyID) const;
    void lookAtMembers(const ThinSummary&);
    void noteWrite(const LinkType& subject, PropertyKeyID, Traits);
    PropertyKeyID findNameThatConstructorStores(FunctionID, const ThinSummary&, const SummaryAccess&) const;
    bool storesIntoObjectThatConstructorMakesOnEveryWay(FunctionID, const ThinSummary&, const SummaryAccess&) const;
    struct ConstructorLookedAt {
        FunctionID function;
        BitVector nodesUnderConstruction;
        BlockID blockAboveEveryReturn;
    };
    mutable ConstructorLookedAt m_constructorLookedAt;
    void noteWriteInto(LayoutID, PropertyKeyID, Traits, bool mayComeToObject = true);
    void noteValueLeftOutOfDescriptor(const LinkType&);
    void noteStoreIntoOlderObject(FunctionID writer, const SummaryAccess&, PropertyKeyID, Traits);
    const Vector<LinkType, 1>& olderObjectsThatSuperCallAnswersWith(LayoutID constructed);
    HashMap<uint32_t, Vector<LinkType, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_olderObjectsThatSuperCallsAnswerWith;
    bool comesToAccessorAbove(LayoutID, PropertyKeyID) const;
    void noteReadInFrontOfDefinition(FunctionID reader, const SummaryAccess&);

    void noteLostClassObjects();
    bool objectPrototypeMayGainName() const;
    bool everyCallerIsKnown(FunctionID function) const { return m_functionsWhoseCallersAreAllKnown.get(function.index()); }
    BitVector m_functionsWhoseCallersAreAllKnown;
    bool mayBeAllocated(LayoutID) const;
    bool mayBeKnownByFamily(LayoutID id) const { return m_flow.layoutsMetInFamilies.get(id.index()) || m_flow.lostLayouts.get(id.index()) || hasEscaped(id.index()); }
    bool canHoldAppendedFields(LayoutID) const;
    bool canNameAppendedField(LayoutID, PropertyKeyID) const;
    bool isListedByClassBelow(LayoutID, PropertyKeyID) const;
    bool noteNameNotListed(LayoutID, PropertyKeyID);
    void noteNamesWrittenOfLayouts();
    void noteNamesThatMakersGive();
    uint32_t findMakingInFrontOfRun(uint32_t);
    void noteNamesThatFollowersOfMakersStore();
    void appendFields();
    void noteStoreOfMaker(const ThinSummary&, const SummaryAccess&, PropertyKeyID);
    Traits traitsOfStoreOfMaker(FunctionID, const ThinSummary&, const SummaryAccess&) const;
    void finishRunsOfMakersIn(FunctionID);
    void writeRunsOfMakers();
    void noteStoreOnObjectInField(const ThinSummary&, const SummaryAccess&, PropertyKeyID);
    void finishRunsOnObjectsInFieldsIn(FunctionID);
    void writeRunsBehindTaggedMakers();
    void writeLayouts();
    void gatherAtDeclarationsOfFields();
    void writeFields();
    void writeAppendedFields();
    void writeVariables();
    void writeCaptures();
    void writeMembers();
    void noteWriteOfPrototypeProperty(const LinkType& subject, PropertyKeyID, bool storesObjectMadeForIt = false);
    BitVector m_functionsGivenObjectMadeToBePrototype;
    void writeWhosePrototypePropertyStaysAsMade();
    void noteNameGivenToClosure(const LinkType& subject, PropertyKeyID = { }, bool laysValueInSight = false);
    void writeNamesOnFunctions();
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesThatStatementsLayOnLostFunctions;
    void writeRowsBelowNamesDefinedPastLists();
    BitVector m_sidesWithNamesDefinedPastLists;
    BitVector m_prototypesDefinedOnUnderKeysThatNobodyKnows;
    bool m_namesAreDefinedOnLostFunctions { false };
    BitVector m_sidesOfFamiliesWhollyPastLists;
    BitVector m_functionsWhoseClosuresGetWhatIsNotListed;
    bool m_someConstructorGetsName { false };
    BitVector m_functionsWhosePrototypePropertyIsWritten;
    bool m_prototypePropertyOfSomeConstructorIsWritten { false };

    const LinkFlow& m_flow;
    const Program& m_program;
    TypeArena* m_types { nullptr };
    TypesOfFlowTurnedOnce m_typesTurned;
    static thread_local TypesOfThreadInLink s_typesOfThread;
    TypeArena& types() const { return s_typesOfThread.types ? *s_typesOfThread.types : *m_types; }
    TypesOfFlowTurnedOnce& typesTurned() { return s_typesOfThread.turned ? *s_typesOfThread.turned : m_typesTurned; }
    void writeOnAllThreads(unsigned numberOfPositions, const auto& write)
    {
        writeTypedPositionsOnAllThreads(m_flow, m_flow.program.types, numberOfPositions, [](TypesOfThreadInLink types) {
            s_typesOfThread = types;
        }, write);
    }
    LinkTables& m_tables;

    Vector<OptionSet<LayoutChange>> m_changes;
    Vector<OptionSet<LayoutChange>> m_changesOfLostFamilies;
    BitVector m_isMadeAtNoNode;
    BitVector m_isMadeByParsers;
    BitVector m_liesBelowLostClassObject;
    BitVector m_gainsOnlyWhatWrittenDefinitionsLay;
    BitVector m_isRedefinedByWrittenDefinitionsAlone;
    BitVector m_appendedKeysThatDescriptorsDefine;
    BitVector m_appendedKeysThatDescriptorsAloneWrite;
    BitVector m_fieldsWrittenWithoutDefinition;
    BitVector m_fieldsInWhichDefinitionsLayForms;
    BitVector m_fieldsInWhichDescriptorsMayLayAccessors;
    bool m_changeByDescriptorIsAtHand { false };
    bool m_changeAtHandMayLayAccessor { false };
    BitVector m_isFilledUnderComputedKeys;
    Vector<LayoutID> m_heritages;
    BitVector m_hasHeritage;
    struct NameWrittenOnPrototype {
        FunctionID getter;
        FunctionID setter;
        FunctionID method;
        bool isWrittenOnce { false };
        bool isWrittenAgain { false };
    };
    HashMap<uint64_t, NameWrittenOnPrototype, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesWrittenOnPrototypes;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_keysWrittenOnPrototypes;
    HashMap<uint64_t, NameWrittenOnPrototype, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_accessorsWrittenInMaps;
    Vector<std::pair<uint64_t, uint64_t>> m_namesLaidOutOfMaps;

    BitVector m_fieldsWrittenLate;
    BitVector m_fieldsGivenValues;
    static constexpr uint32_t otherThanOneUnit = UINT32_MAX;
    Vector<uint32_t> m_unitsListedForFields;
    static constexpr uint32_t severalClasses = UINT32_MAX;
    static constexpr uint32_t layoutAmongWhatMakersLay = 1u << 31;
    uint32_t whatIsMadeInSightAt(FunctionID, const ThinSummary&, uint32_t node);
    void noteNamesStoredThroughWhatFieldsAreExpectedToHold();
    void noteFormsOfNamesDefinedThroughWrittenTypesOfParameters();
    static void noteWhatMakerLays(uint32_t& laid, uint32_t made)
    {
        if (laid != made)
            laid = laid ? severalClasses : made;
    }
    Vector<uint32_t> m_classesThatMakersLayInFields;
    static constexpr uint8_t unitsOfEveryFieldAreNoted = 1 << 0;
    static constexpr uint8_t everyFieldIsGivenValues = 1 << 1;
    static constexpr uint8_t everyFieldIsWrittenWithoutDefinition = 1 << 3;
    static constexpr uint8_t everyFieldIsWrittenLate = 1 << 2;
    Vector<uint8_t> m_whatIsNotedOfEveryField;
    struct FieldFactsFromEdges {
        BitVector fieldsGivenValues;
        BitVector fieldsGivenUndefinedByMakers;
        BitVector fieldsFilledByConstructors;
        BitVector fieldsWrittenLate;
        BitVector layoutsGivenUndefinedByMakers;
        Vector<uint32_t> unitsListedForFields;
        Vector<uint32_t> whatMakersLayInFields;
    };
    FieldFactsFromEdges m_fieldFactsFromEdges;
    BitVector m_keysOfTestedFields;
    BitVector m_fieldsGivenUndefinedByMakers;
    BitVector m_fieldsFilledByConstructors;
    BitVector m_fieldsGivenUndefinedOrNullLate;
    BitVector m_fieldsGivenUndefinedOrNullByMakers;
    BitVector m_fieldsGivenValuesByMakers;
    BitVector m_layoutsGivenUndefinedByMakers;
    bool m_asksWhatMakersGive { false };
    Vector<FunctionID> m_onlyFunctionsThatMakeObjects;
    BitVector m_layoutsMadeInSeveralFunctions;
    uint32_t m_thisOfObjectBeingMade { UINT32_MAX };
    BitVector m_fieldsWithUnseenWriters;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_keysWrittenLateThroughLost;
    BitVector m_lostFamiliesWrittenLateUnderEveryKey;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_keysGivenValuesThroughLost;
    BitVector m_lostFamiliesGivenValuesUnderEveryKey;
    BitVector m_fieldsReadInFrontOfDefinition;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_keysReadInFrontOfDefinitionThroughLost;
    HashMap<uint64_t, uint8_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_writesNotedThroughFamilies;

    struct NameNotListed {
        LayoutID layout;
        PropertyKeyID key;
        bool isRequiredBySomeWrittenType { false };
        bool isRequiredBySomeTypeOfThis { false };
        bool isOptionalInSomeTypeOfThis { false };

        bool isGivenBySecondConstructor { false };
        bool isStoredByFollowerOfMaker { false };
        bool isGivenByConstructor { false };
        bool isLaidOnObjectsOfLibrary { false };
        bool isStoredThroughWhatIsExpected { false };
        bool isBroughtByCopyingLoop { false };
        bool isLaidLate { false };
        uint16_t formsThatDescriptorsCreate { 0 };
        bool isDefinedInOtherWays { false };
        bool isDefinedByDescriptors { false };
        bool isWrittenWithoutDefinition { false };
        OptionSet<FieldForm> formToPlanFor() const
        {
            if (isDefinedInOtherWays || !hasOneBitSet(formsThatDescriptorsCreate))
                return { };
            return OptionSet<FieldForm>::fromRaw(static_cast<uint8_t>(WTF::ctz(static_cast<uint32_t>(formsThatDescriptorsCreate))));
        }

        bool isRequiredByTypeOfThis() const { return (isRequiredBySomeTypeOfThis && !isOptionalInSomeTypeOfThis) || isGivenByConstructor || isGivenBySecondConstructor || isLaidOnObjectsOfLibrary || isStoredThroughWhatIsExpected || isBroughtByCopyingLoop || !formToPlanFor().isEmpty(); }
    };
    Vector<NameNotListed> m_namesNotListed;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesThatConstructorsStore;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesLaidOnObjectsOfLibrary;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesStoredThroughWhatIsExpected;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesThatMakersStore;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesThatMembersAboveStore;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesThatHoldersLeaveToHeirs;
    const SummaryAccess* m_definitionAtHand { nullptr };
    bool m_accessorOfLiteralIsAtHand { false };
    static constexpr uint32_t formNobodyKnows = 1u << 16;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_formsLaidInListedFields;
    Vector<OptionSet<FieldForm>> m_formsOfAppendedKeys;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_pairsNotListed;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_keysStoredOnUnknownObjects;
    Vector<uint32_t> m_placesOfFirstStoresOfKeys;
    using LayoutsOfRun = Vector<LayoutID, SetsOfLayoutsInLinkFlow::mostMembers>;
    struct RunOfMaker {
        uint32_t subject { 0 };
        LayoutsOfRun layouts;
        Vector<PropertyKeyID, 8> keys;
        FunctionID givenBy;
        bool isMadeHere { false };
        bool isReturned { false };
        FunctionID body;
        ValueID firstStore;
        uint64_t keysOnlyGivenUndefined { 0 };
        uint64_t keysGivenObjectsMadeHere { 0 };
        bool isOnObjectThatLinkHasLost { false };
        Vector<ValueID, 8> firstStoresOfKeys;
        Vector<PropertyKeyID, 8> keysStoredOnEveryWayToLastStore;
    };
    Vector<RunOfMaker, 4> m_runsOfMakersInFunction;
    static constexpr size_t longestListGoneThrough = 8;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_placesOfRunsInFunction;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_keysOfLongRunsInFunction;
    Vector<RunOfMaker> m_runsOfMakers;
    struct RunOnObjectInField {
        uint32_t holder { 0 };
        PropertyKeyID keyInHolder;
        uint32_t subject { 0 };
        LayoutsOfRun layouts;
        Vector<PropertyKeyID, 8> keys;
        FunctionID givenBy;
        uint32_t runOnHolder { 0 };
        bool isOnObjectThatLinkHasLost { false };
    };
    Vector<RunOnObjectInField, 2> m_runsOnObjectsInFieldsInFunction;
    Vector<RunOnObjectInField> m_runsOnObjectsInFields;
    HashMap<uint64_t, uint32_t> m_runsOnFieldsOfWhatFunctionsReturn;
    HashMap<uint32_t, Vector<FunctionID, 4>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_functionsLaidIntoFieldsUnderKey;
    Vector<uint32_t> m_runsOnWhatFunctionsReturn;
    static constexpr uint32_t noMakingInFrontOfRun = UINT32_MAX;
    Vector<uint32_t> m_makingsInFrontOfRuns;
    static size_t placeAmongFirstKeysOfRun(const RunOfMaker& run, PropertyKeyID key)
    {
        std::span<const PropertyKeyID> firstKeys = run.keys.span().first(std::min<size_t>(run.keys.size(), 64));
        return static_cast<size_t>(std::ranges::find(firstKeys, key) - firstKeys.begin());
    }
    bool m_asksWhatBodyHandsOn { false };
    uint32_t m_numberOfKeysStored { 0 };

    BitVector m_variablesWrittenLate;
    BitVector m_variablesStoredInto;
    BitVector m_variablesSharedWithUnseenCode;

    Vector<LinkedMember> m_members;
    Vector<LayoutID> m_layoutsAllocatedHere;
};

WriterOfLinkLayouts::WriterOfLinkLayouts(const LinkFlow& flow, LinkTables& tables)
    : m_flow(flow)
    , m_program(flow.program)
    , m_tables(tables)
{
    size_t numberOfLayouts = m_program.layouts.size();
    RELEASE_ASSERT(tables.layouts.size() == numberOfLayouts && tables.firstFieldOfLayout.size() == numberOfLayouts + 1);
    RELEASE_ASSERT(tables.variables.size() == m_program.variables.size() && tables.firstCaptureOfFunction.size() == m_program.functions.size() + 1);
    m_changes.fill(OptionSet<LayoutChange> { }, numberOfLayouts);
    m_isMadeAtNoNode.ensureSize(numberOfLayouts);
    m_isMadeByParsers.ensureSize(numberOfLayouts);
    for (const ExpectedResult& expected : m_program.expectedResults) {
        if (!expected.flags.contains(ExpectedResultFlags::IsMadeByParser))
            continue;
        for (LayoutID layout : expected.layouts) {
            if (parsersCouldMakeRecordsOf(m_program, layout))
                m_isMadeByParsers.quickSet(layout.index());
        }
    }
    m_changesOfLostFamilies.grow(numberOfLayouts);
    m_lostFamiliesWrittenLateUnderEveryKey.ensureSize(numberOfLayouts);
    m_lostFamiliesGivenValuesUnderEveryKey.ensureSize(numberOfLayouts);
    m_liesBelowLostClassObject.ensureSize(numberOfLayouts);
    m_gainsOnlyWhatWrittenDefinitionsLay.ensureSize(numberOfLayouts);
    m_isRedefinedByWrittenDefinitionsAlone.ensureSize(numberOfLayouts);
    m_sidesWithNamesDefinedPastLists.ensureSize(2 * numberOfLayouts);
    m_prototypesDefinedOnUnderKeysThatNobodyKnows.ensureSize(numberOfLayouts);
    m_isFilledUnderComputedKeys.ensureSize(numberOfLayouts);
    m_heritages.fill(LayoutID(), numberOfLayouts);
    m_hasHeritage.ensureSize(numberOfLayouts);
    m_fieldsWrittenLate.ensureSize(tables.fields.size());
    m_fieldsWrittenWithoutDefinition.ensureSize(tables.fields.size());
    m_fieldsInWhichDefinitionsLayForms.ensureSize(tables.fields.size());
    m_fieldsInWhichDescriptorsMayLayAccessors.ensureSize(tables.fields.size());
    m_fieldsGivenValues.ensureSize(tables.fields.size());
    m_unitsListedForFields.fill(0, tables.fields.size());
    m_whatIsNotedOfEveryField.fill(0, numberOfLayouts);
    m_keysOfTestedFields.ensureSize(m_program.types.numberOfPropertyKeys());
    m_fieldsGivenUndefinedByMakers.ensureSize(tables.fields.size());
    m_fieldsFilledByConstructors.ensureSize(tables.fields.size());
    m_fieldsGivenUndefinedOrNullLate.ensureSize(tables.fields.size());
    m_fieldsGivenUndefinedOrNullByMakers.ensureSize(tables.fields.size());
    m_fieldsGivenValuesByMakers.ensureSize(tables.fields.size());
    m_layoutsGivenUndefinedByMakers.ensureSize(numberOfLayouts);
    m_asksWhatMakersGive = flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls;
    m_onlyFunctionsThatMakeObjects.fill(FunctionID(), m_asksWhatMakersGive ? numberOfLayouts : 0);
    m_layoutsMadeInSeveralFunctions.ensureSize(numberOfLayouts);
    m_fieldsReadInFrontOfDefinition.ensureSize(tables.fields.size());
    m_fieldsWithUnseenWriters.ensureSize(tables.fields.size());
    m_placesOfFirstStoresOfKeys.fill(UINT32_MAX, m_program.types.numberOfPropertyKeys());
    m_variablesWrittenLate.ensureSize(tables.variables.size());
    m_variablesStoredInto.ensureSize(tables.variables.size());
    m_variablesSharedWithUnseenCode.ensureSize(tables.variables.size());
}

TypeID WriterOfLinkLayouts::unlessAnything(TypeID type) const
{
    if (!type || holdsOnlyPrimitives(type))
        return type;
    return types().at(type).isAnything() ? TypeID() : type;
}

TypeID WriterOfLinkLayouts::claimedTypeAt(FlowNodeID node, Numbers numbers)
{
    LinkType type = m_flow.typeAt(node);
    if (numbers == Numbers::ByKindAlone)
        type.counts = LinkType::Counts::Unknown;
    if (m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls) {
        type.hasUnknownObjects = false;
        type.holdsLostBuiltins = false;
        type.builtin = { };
        if (type.functions != LinkType::Functions::None && type.functions != LinkType::Functions::Closure && type.functions != LinkType::Functions::Set) {
            type.functions = LinkType::Functions::Lost;
            type.function = { };
        }
    }
    return type.isNever() ? TypeID() : unlessAnything(typesTurned().turn(m_flow, types(), type));
}

bool WriterOfLinkLayouts::claimLeavesOutWhatArrivesAt(FlowNodeID node) const
{
    if (m_flow.reading() != WhatUnseenCodeDoes::OnlyReadsAndCalls)
        return false;
    LinkType type = m_flow.typeAt(node);
    return type.hasUnknownObjects || type.holdsLostBuiltins || type.builtin || (type.functions != LinkType::Functions::None && type.functions != LinkType::Functions::Closure && type.functions != LinkType::Functions::Set);
}

TypeID WriterOfLinkLayouts::claimedTypeAtEither(FlowNodeID first, FlowNodeID second)
{
    if (first == second || m_flow.typeAt(second).isNever())
        return claimedTypeAt(first);
    if (m_flow.typeAt(first).isNever())
        return claimedTypeAt(second);
    TypeID atFirst = claimedTypeAt(first);
    TypeID atSecond = claimedTypeAt(second);
    return atFirst && atSecond ? unlessAnything(joinOf(types(), *m_flow.familiesBehindFlow, atFirst, atSecond)) : TypeID();
}

TypeID WriterOfLinkLayouts::typeEstablishedOf(const DeclaredType& declared) const
{
    std::optional<ProvenInEveryVersion> established = declared.established();
    return established ? unlessAnything(*established) : TypeID();
}

TypeID WriterOfLinkLayouts::typeOfWrittenUnit(WrittenUnit unit)
{
    switch (unit.kind()) {
    case WrittenUnit::Kind::Text:
        return types().stringLiteral(unit.text());
    case WrittenUnit::Kind::Integer:
        return types().integer(Range::one(unit.integer()));
    case WrittenUnit::Kind::Boolean:
        return TypeArena::fromPrimitives(unit.boolean() ? Primitive::True : Primitive::False);
    case WrittenUnit::Kind::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeID WriterOfLinkLayouts::narrowerOf(TypeID byFlow, const DeclaredType& declared)
{
    TypeID established = typeEstablishedOf(declared);
    if (!established)
        return byFlow;
    return byFlow && types().isSubset(byFlow, established) ? byFlow : established;
}

uint32_t WriterOfLinkLayouts::whatIsMadeInSightAt(FunctionID id, const ThinSummary& summary, uint32_t node)
{
    if (node == noSummaryNode)
        return 0;
    const SummaryNode& made = summary.nodes[node];
    switch (made.kind) {
    case SummaryNode::Kind::ClassInstance:
        return made.first + 1;
    case SummaryNode::Kind::ResultOfCall: {
        const SummaryCall& call = summary.calls[made.first];
        if (made.second || !call.isConstruction || call.newTarget != noSummaryNode)
            return 0;
        if (call.callee == SummaryCall::Callee::Builtin) {
            BuiltinClassID builtinClass = call.builtin ? classMadeByHeritage(m_program.builtins(), call.builtin) : BuiltinClassID();
            return builtinClass ? builtinClass.index() + 1 : 0;
        }
        FunctionID callee = call.callee == SummaryCall::Callee::Binding ? call.function : FunctionID();
        if (call.callee == SummaryCall::Callee::Value && call.subject != noSummaryNode) {
            if (const LinkType& called = typeAt(id, call.subject); called.functions == LinkType::Functions::Closure && called.objects == LinkType::Objects::None && called.classes == LinkType::Classes::None)
                callee = called.function;
        }
        LayoutID constructed = callee ? m_program.at(callee).constructedClass : LayoutID();
        return constructed ? (constructed.index() + 1) | layoutAmongWhatMakersLay : 0;
    }
    default:
        return 0;
    }
}

void WriterOfLinkLayouts::noteNamesStoredThroughWhatFieldsAreExpectedToHold()
{
    if (m_classesThatMakersLayInFields.isEmpty())
        return;
    TypeReader types(m_program);
    for (size_t index : m_flow.reached) {
        FunctionID id(index);
        const Function& function = m_program.at(id);
        const ThinSummary& summary = m_flow.summaries.at(id);
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind != SummaryAccess::Kind::Store || !access.key || access.isToContents() || summary.nodes[access.subject].kind != SummaryNode::Kind::ResultOfAccess)
                continue;
            const SummaryAccess& read = summary.accesses[summary.nodes[access.subject].first];
            if (read.kind != SummaryAccess::Kind::Read || !read.key || read.isToContents())
                continue;
            if (LinkType::Objects known = typeAt(id, access.subject).objects; known != LinkType::Objects::None && known != LinkType::Objects::Lost)
                continue;
            const SummaryNode& holder = summary.nodes[read.subject];
            const DeclaredType* declared = holder.kind == SummaryNode::Kind::Parameter ? function.declaredEntryParameter(holder.first) : nullptr;
            std::span<const LayoutID> holders = declared && declared->written() ? membersOf(types, m_program.types.at(declared->written())) : std::span<const LayoutID>();
            bool someExpectedHolderHasName = std::ranges::any_of(holders, [&](LayoutID expected) {
                return !!findField(m_program, expected, read.key);
            });
            if (!someExpectedHolderHasName)
                holders = m_program.layoutsWithName(read.key);
            bool instancesSpeakFirst = std::ranges::any_of(holders, [&](LayoutID expected) {
                return m_program.at(expected).kind == LayoutKind::Instance && findField(m_program, expected, read.key);
            });
            for (LayoutID expected : holders) {
                if (instancesSpeakFirst && m_program.at(expected).kind != LayoutKind::Instance)
                    continue;
                std::optional<FieldReference> found = findField(m_program, expected, read.key);
                uint32_t laid = found ? m_classesThatMakersLayInFields[cellOf(declarationOf(m_program, *found))] : 0;
                if (laid == severalClasses || !(laid & layoutAmongWhatMakersLay))
                    continue;
                LayoutID stored((laid & ~layoutAmongWhatMakersLay) - 1);
                uint64_t layoutAndKey = static_cast<uint64_t>(stored.index()) << 32 | access.key.bits();
                if (findField(m_program, stored, access.key) || comesToAccessorAbove(stored, access.key) || !noteNameNotListed(stored, access.key))
                    continue;
                m_namesStoredThroughWhatIsExpected.add(layoutAndKey);
                m_namesNotListed[m_pairsNotListed.get(layoutAndKey)].isStoredThroughWhatIsExpected = true;
            }
        }
    }
}

void WriterOfLinkLayouts::noteFormsOfNamesDefinedThroughWrittenTypesOfParameters()
{
    TypeReader types(m_program);
    for (size_t index : m_flow.reached) {
        FunctionID id(index);
        const Function& function = m_program.at(id);
        const ThinSummary& summary = m_flow.summaries.at(id);
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind != SummaryAccess::Kind::Define || !access.isByDescriptor || !access.formCreated || !access.key || summary.nodes[access.subject].kind != SummaryNode::Kind::Parameter)
                continue;
            const LinkType& arrives = typeAt(id, access.subject);
            if (arrives.objects != LinkType::Objects::None && arrives.objects != LinkType::Objects::Lost)
                continue;
            const DeclaredType* declared = function.declaredEntryParameter(summary.nodes[access.subject].first);
            if (!declared || !declared->written())
                continue;
            m_definitionAtHand = &access;
            for (LayoutID written : membersOf(types, m_program.types.at(declared->written()))) {
                bool isInFamilyThatArrives = arrives.objects == LinkType::Objects::Lost && arrives.layout && m_flow.lostLayouts.get(written.index()) && lostFamilyOf(arrives.layout) == lostFamilyOf(written);
                if (m_program.at(written).kind == LayoutKind::Instance && (hasEscaped(written.index()) || isInFamilyThatArrives) && !findField(m_program, written, access.key) && !comesToAccessorAbove(written, access.key))
                    noteNameNotListed(written, access.key);
            }
            m_definitionAtHand = nullptr;
        }
    }
}

ALWAYS_INLINE void WriterOfLinkLayouts::lookAtEdges(FunctionID id, const ThinSummary& summary)
{
    for (VariableID variable : summary.functionVariablesStoredInto)
        m_variablesStoredInto.quickSet(variable.index());
    m_layoutsAllocatedHere.shrink(0);
    for (const SummaryNode& node : summary.nodes) {
        if (node.kind == SummaryNode::Kind::Allocation)
            m_layoutsAllocatedHere.append(LayoutID(node.first));
    }
    std::ranges::sort(m_layoutsAllocatedHere);
    for (LayoutID layout : m_asksWhatMakersGive ? m_layoutsAllocatedHere.span() : std::span<LayoutID> { }) {
        if (FunctionID& maker = m_onlyFunctionsThatMakeObjects[layout.index()]; maker && maker != id)
            m_layoutsMadeInSeveralFunctions.quickSet(layout.index());
        else
            maker = id;
    }

    for (PropertyKeyID key : summary.keysOfTestedFields)
        m_keysOfTestedFields.set(key.index());

    const Function& function = m_program.at(id);
    for (const SummaryEdge& edge : summary.edges) {
        const SummaryNode& to = summary.nodes[edge.to];
        if (to.kind == SummaryNode::Kind::Field) {
            LayoutID layout(to.first);
            m_fieldFactsFromEdges.fieldsGivenValues.quickSet(cellOf({ layout, to.second }));
            if (uint32_t made = m_fieldFactsFromEdges.whatMakersLayInFields.isEmpty() ? 0 : whatIsMadeInSightAt(id, summary, edge.from))
                noteWhatMakerLays(m_fieldFactsFromEdges.whatMakersLayInFields[cellOf({ layout, to.second })], made);
            uint32_t listed = summary.nodes[edge.from].kind == SummaryNode::Kind::Constant && summary.nodes[edge.from].second && !edge.passes ? summary.nodes[edge.from].second : otherThanOneUnit;
            if (uint32_t& unit = m_fieldFactsFromEdges.unitsListedForFields[cellOf({ layout, to.second })]; unit != listed)
                unit = unit ? otherThanOneUnit : listed;
            if (!m_asksWhatMakersGive) {
            } else if (isConstantUndefinedInLinkLayouts(summary.nodes[edge.from])) {
                m_fieldFactsFromEdges.fieldsGivenUndefinedByMakers.quickSet(cellOf({ layout, to.second }));
                m_fieldFactsFromEdges.layoutsGivenUndefinedByMakers.quickSet(layout.index());
            } else
                m_fieldFactsFromEdges.fieldsFilledByConstructors.quickSet(cellOf({ layout, to.second }));
            if (const SummaryNode& from = summary.nodes[edge.from]; from.kind == SummaryNode::Kind::Closure && !m_flow.taggedMakers.isEmpty()) {
                if (PropertyKeyID key = fieldAt(m_program, { layout, to.second }).key) {
                    Vector<FunctionID, 4>& functions = m_functionsLaidIntoFieldsUnderKey.add(key.bits(), Vector<FunctionID, 4> { }).iterator->value;
                    if (!functions.contains(FunctionID(from.first)))
                        functions.append(FunctionID(from.first));
                }
            }
            if (m_program.at(layout).isEnvironment() || !std::ranges::binary_search(m_layoutsAllocatedHere, layout))
                m_fieldFactsFromEdges.fieldsWrittenLate.quickSet(cellOf({ layout, to.second }));
        } else if (to.kind == SummaryNode::Kind::Variable) {
            const Variable& variable = m_program.variables[to.first];
            bool isStoredOnce = function.isModuleBody() && function.module == variable.module && variable.flags.contains(VariableFlags::IsImmutable)
                && !m_program.at(variable.module).flags.contains(ModuleFlags::MayBeReevaluated);
            if (!isStoredOnce)
                m_variablesWrittenLate.quickSet(to.first);
        }
    }
}

ALWAYS_INLINE void WriterOfLinkLayouts::lookAtAccesses(FunctionID id, const ThinSummary& summary)
{
    m_thisOfObjectBeingMade = UINT32_MAX;
    bool constructs = m_program.at(id).constructedClass && m_program.at(id).takesThisFromConstruction();
    FlowNodeID heirsThatRun = m_flow.findHeirsThatRun(id);
    if (const Function& function = m_program.at(id); m_asksWhatMakersGive && (function.kind == FunctionKind::Initializer || function.takesThisFromConstruction()))
        m_thisOfObjectBeingMade = function.thisParameterIndex().value_or(UINT32_MAX);
    for (const SummaryAccess& access : summary.accesses) {
        if (access.isToContents())
            continue;
        Traits traits { Trait::AddsWhatIsNotListed, Trait::MayGiveValue };
        switch (access.kind) {
        case SummaryAccess::Kind::Read:
            if (access.isPartOfConstruction)
                noteReadInFrontOfDefinition(id, access);
            continue;
        case SummaryAccess::Kind::Has:
            continue;
        case SummaryAccess::Kind::Store:
            if (access.isCopySpelledByLoop && m_flow.followsCopySpelledByLoop(typeAt(id, access.subject), typeAt(id, m_flow.sourceOfCopySpelledByLoop(id, access.site))))
                continue;
            traits.add(Trait::MayRunSetter);
            break;
        case SummaryAccess::Kind::Define:
            break;
        }
        if (!access.isPartOfConstruction)
            traits.add(Trait::ComesAfterConstruction);
        if (access.value != noSummaryNode && isConstantUndefinedOrNullInLinkLayouts(summary.nodes[access.value]))
            traits.add(Trait::BringsUndefinedOrNull);
        if (!access.isPartOfConstruction && access.kind == SummaryAccess::Kind::Store)
            traits.remove(Trait::MayGiveValue);
        if (access.isByDescriptor && access.formCreated && access.formCreated->contains(FieldForm::IsAccessor))
            traits.remove(Trait::MayGiveValue);
        noteNameWrittenOnPrototype(id, summary, access);
        if (access.key && access.key.index() < m_placesOfFirstStoresOfKeys.size() && m_placesOfFirstStoresOfKeys[access.key.index()] == UINT32_MAX)
            m_placesOfFirstStoresOfKeys[access.key.index()] = m_numberOfKeysStored++;
        PropertyKeyID key = m_flow.keyOf(id, access);
        if (!key && access.keyNode != noSummaryNode && m_flow.reading() != WhatUnseenCodeDoes::Anything && LinkFlow::isSymbolAsFarAsIsSeen(typeAt(id, access.keyNode)))
            continue;
        noteStoreOfMaker(summary, access, key);
        traits.add(traitsOfStoreOfMaker(id, summary, access));
        if (LayoutID constructed = traits.containsAny({ Trait::GivesUndefinedToObjectBeingMade, Trait::FillsObjectBeingConstructed }) ? m_program.at(id).constructedClass : LayoutID(); constructed && summary.nodes[access.subject].kind == SummaryNode::Kind::Parameter && typeAt(id, access.subject).objects == LinkType::Objects::Lost) {
            if (std::optional<FieldReference> found = findField(m_program, constructed, access.key)) {
                (traits.contains(Trait::FillsObjectBeingConstructed) ? m_fieldsFilledByConstructors : m_fieldsGivenUndefinedByMakers).quickSet(cellOf(*found));
                m_layoutsGivenUndefinedByMakers.quickSet(constructed.index());
            }
        }
        const LinkType& subject = typeAt(id, access.subject);
        if (uint32_t made = key && !m_classesThatMakersLayInFields.isEmpty() && (subject.objects == LinkType::Objects::Layout || subject.objects == LinkType::Objects::Set) ? whatIsMadeInSightAt(id, summary, access.value) : 0) {
            m_flow.forEachLayoutOf(subject, [&](LayoutID holder) {
                if (std::optional<FieldReference> found = findField(m_program, holder, key))
                    noteWhatMakerLays(m_classesThatMakersLayInFields[cellOf(declarationOf(m_program, *found))], made);
            });
        }
        if (constructs && key && (subject.objects == LinkType::Objects::Lost || subject.objects == LinkType::Objects::None) && findNameThatConstructorStores(id, summary, access))
            noteWriteInto(m_program.at(id).constructedClass, key, traits);
        noteChangeOfClosure(subject, key);
        noteNameDefinedPastLists(access.receiver != noSummaryNode ? typeAt(id, access.receiver) : subject, key, access.kind == SummaryAccess::Kind::Define);
        bool isUnderPrototype = key && m_program.nameClassOf(key) == NameClass::Prototype;
        if (!key || isUnderPrototype)
            noteWriteOfPrototypeProperty(subject, key, access.kind == SummaryAccess::Kind::Store && access.storesObjectMadeForIt);
        if (access.kind != SummaryAccess::Kind::Store || !isUnderPrototype)
            noteNameGivenToClosure(subject, key, access.laysValueInSightUnderName());
        if (key && subject.objects == LinkType::Objects::Layout && !access.isToContents() && m_program.at(id).role != FunctionRole::Library) {
            const Layout& stored = m_program.at(subject.layout);
            if (stored.kind == LayoutKind::Instance && stored.constructor && m_program.at(stored.constructor).role == FunctionRole::Library)
                m_namesLaidOnObjectsOfLibrary.add(static_cast<uint64_t>(subject.layout.index()) << 32 | key.bits());
        }
        if (const SummaryNode& made = summary.nodes[access.subject]; key && made.kind == SummaryNode::Kind::Allocation && !access.isToContents() && m_program.at(LayoutID(made.first)).has(LayoutFlags::IsOfCreatedObject)) {
            m_namesThatConstructorsStore.add(static_cast<uint64_t>(made.first) << 32 | key.bits());
            m_namesThatMakersStore.add(static_cast<uint64_t>(made.first) << 32 | key.bits());
        }
        if (const SummaryNode& stored = summary.nodes[access.subject]; heirsThatRun && key && access.kind == SummaryAccess::Kind::Store && !access.isToContents() && stored.kind == SummaryNode::Kind::Parameter && stored.first == m_program.at(id).thisParameterIndex()) {
            m_flow.forEachLayoutOf(m_flow.typeAt(heirsThatRun), [&](LayoutID heir) {
                if (!m_program.at(heir).has(LayoutFlags::IsOfCreatedObject) || findField(m_program, heir, key))
                    return;
                LayoutID holder = findHolderAboveCreatedLayout(m_flow, heir);
                if (!holder || findField(m_program, holder, key))
                    return;
                m_namesThatConstructorsStore.add(static_cast<uint64_t>(heir.index()) << 32 | key.bits());
                m_namesThatMembersAboveStore.add(static_cast<uint64_t>(heir.index()) << 32 | key.bits());
                m_namesThatHoldersLeaveToHeirs.add(static_cast<uint64_t>(holder.index()) << 32 | key.bits());
                noteWriteInto(heir, key, traits);
            });
        }
        m_definitionAtHand = access.kind == SummaryAccess::Kind::Define ? &access : nullptr;
        noteWrite(subject, key, traits);
        if (m_flow.mayStoreIntoOlderObjectThatSuperCallAnswered(id, access))
            noteStoreIntoOlderObject(id, access, key, traits);
        m_definitionAtHand = nullptr;
    }
    if (!m_runsOfMakersInFunction.isEmpty() || m_asksWhatBodyHandsOn)
        finishRunsOfMakersIn(id);
    if (!m_runsOnObjectsInFieldsInFunction.isEmpty())
        finishRunsOnObjectsInFieldsIn(id);
}

bool WriterOfLinkLayouts::storesIntoObjectThatConstructorMakesOnEveryWay(FunctionID id, const ThinSummary& summary, const SummaryAccess& access) const
{
    const Function& function = m_program.at(id);
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(id);
    if (function.kind != FunctionKind::BaseConstructor || m_program.at(function.constructedClass).constructor != id || !index || !access.site || index->isParameter(access.site))
        return false;
    ConstructorLookedAt& looked = m_constructorLookedAt;
    if (looked.function != id) {
        looked.function = id;
        looked.nodesUnderConstruction.clearAll();
        looked.nodesUnderConstruction.ensureSize(summary.nodes.size());
        for (const SummaryAccess& other : summary.accesses) {
            if (other.isPartOfConstruction)
                looked.nodesUnderConstruction.quickSet(other.subject);
        }
        looked.blockAboveEveryReturn = { };
        for (uint32_t number = 0; number < index->numberOfBlocks(); ++number) {
            BlockID block(number);
            if (!index->isReachable(block))
                continue;
            TerminatorKind end = index->terminator(block).kind();
            if (end != TerminatorKind::Return && end != TerminatorKind::ReturnCall)
                continue;
            if (!looked.blockAboveEveryReturn)
                looked.blockAboveEveryReturn = block;
            while (!index->dominates(looked.blockAboveEveryReturn, block))
                looked.blockAboveEveryReturn = index->immediateDominator(looked.blockAboveEveryReturn);
        }
    }
    return looked.blockAboveEveryReturn && looked.nodesUnderConstruction.quickGet(access.subject) && index->dominates(index->blockOf(access.site), looked.blockAboveEveryReturn);
}

PropertyKeyID WriterOfLinkLayouts::findNameThatConstructorStores(FunctionID id, const ThinSummary& summary, const SummaryAccess& access) const
{
    const Function& function = m_program.at(id);
    if (!function.constructedClass || access.isToContents() || (access.kind != SummaryAccess::Kind::Store && access.kind != SummaryAccess::Kind::Define))
        return { };
    const SummaryNode& subject = summary.nodes[access.subject];
    bool isThisThatIsHandedIn = function.takesThisFromConstruction() && subject.kind == SummaryNode::Kind::Parameter && subject.first == function.thisParameterIndex();
    if (!isThisThatIsHandedIn && function.kind != FunctionKind::BaseConstructor)
        return { };
    PropertyKeyID key = m_flow.keyOf(id, access);
    if (!key || findField(m_program, function.constructedClass, key))
        return { };
    return isThisThatIsHandedIn || storesIntoObjectThatConstructorMakesOnEveryWay(id, summary, access) ? key : PropertyKeyID();
}

WriterOfLinkLayouts::Traits WriterOfLinkLayouts::traitsOfStoreOfMaker(FunctionID, const ThinSummary& summary, const SummaryAccess& access) const
{
    if (!m_asksWhatMakersGive || !access.key || access.value == noSummaryNode)
        return { };
    const SummaryNode& subject = summary.nodes[access.subject];
    bool isConstructing = access.isPartOfConstruction || access.kind == SummaryAccess::Kind::Define || subject.kind == SummaryNode::Kind::Allocation;
    if (subject.kind == SummaryNode::Kind::ResultOfCall)
        isConstructing |= summary.calls[subject.first].isConstruction;
    else if (subject.kind == SummaryNode::Kind::Parameter)
        isConstructing |= subject.first == m_thisOfObjectBeingMade;
    if (!isConstructing && subject.kind != SummaryNode::Kind::ResultOfCall)
        return { };
    if (isConstantUndefinedInLinkLayouts(summary.nodes[access.value]))
        return Trait::GivesUndefinedToObjectBeingMade;
    return isConstructing ? Traits { Trait::FillsObjectBeingConstructed } : Traits { };
}

void WriterOfLinkLayouts::noteStoreOfMaker(const ThinSummary& summary, const SummaryAccess& access, PropertyKeyID key)
{
    SummaryNode::Kind kindOfSubject = summary.nodes[access.subject].kind;
    if (key && kindOfSubject == SummaryNode::Kind::ResultOfAccess)
        noteStoreOnObjectInField(summary, access, key);
    if (!key || (kindOfSubject != SummaryNode::Kind::ResultOfCall && kindOfSubject != SummaryNode::Kind::Allocation))
        return;
    size_t place = notFound;
    if (m_runsOfMakersInFunction.size() <= longestListGoneThrough) {
        place = m_runsOfMakersInFunction.findIf([&](const RunOfMaker& run) {
            return run.subject == access.subject;
        });
    } else if (auto found = m_placesOfRunsInFunction.find(access.subject); found != m_placesOfRunsInFunction.end())
        place = found->value;
    if (place == notFound) {
        place = m_runsOfMakersInFunction.size();
        m_runsOfMakersInFunction.append(RunOfMaker { access.subject, { }, { }, { }, false, false, { }, access.site });
        if (m_runsOfMakersInFunction.size() == longestListGoneThrough + 1) {
            for (uint32_t listed = 0; listed < place; ++listed)
                m_placesOfRunsInFunction.add(m_runsOfMakersInFunction[listed].subject, listed);
        }
        if (m_runsOfMakersInFunction.size() > longestListGoneThrough)
            m_placesOfRunsInFunction.add(access.subject, static_cast<uint32_t>(place));
    }

    if (access.value != noSummaryNode) {
        const SummaryNode& stored = summary.nodes[access.value];
        if (stored.kind == SummaryNode::Kind::Allocation || (stored.kind == SummaryNode::Kind::ResultOfCall && summary.calls[stored.first].isConstruction)) {
            RunOfMaker& run = m_runsOfMakersInFunction[place];
            if (size_t index = placeAmongFirstKeysOfRun(run, key); index < 64)
                run.keysGivenObjectsMadeHere |= 1ull << index;
        }
    }
    Vector<PropertyKeyID, 8>& keys = m_runsOfMakersInFunction[place].keys;
    uint64_t& keysOnlyGivenUndefined = m_runsOfMakersInFunction[place].keysOnlyGivenUndefined;
    bool givesUndefined = access.value != noSummaryNode && isConstantUndefinedInLinkLayouts(summary.nodes[access.value]);
    auto wordOf = [&](PropertyKeyID listedKey) {
        return static_cast<uint64_t>(place) << 32 | listedKey.bits();
    };
    bool isListed = keys.size() <= longestListGoneThrough ? keys.contains(key) : !m_keysOfLongRunsInFunction.add(wordOf(key)).isNewEntry;
    if (isListed) {
        if (size_t listed = !givesUndefined && keysOnlyGivenUndefined ? keys.find(key) : notFound; listed < 64)
            keysOnlyGivenUndefined &= ~(1ull << listed);
        return;
    }
    if (givesUndefined && keys.size() < 64)
        keysOnlyGivenUndefined |= 1ull << keys.size();
    keys.append(key);
    m_runsOfMakersInFunction[place].firstStoresOfKeys.append(access.site);
    if (keys.size() == longestListGoneThrough + 1) {
        for (PropertyKeyID listed : keys)
            m_keysOfLongRunsInFunction.add(wordOf(listed));
    }
}

void WriterOfLinkLayouts::noteStoreOnObjectInField(const ThinSummary& summary, const SummaryAccess& access, PropertyKeyID key)
{
    const SummaryAccess& read = summary.accesses[summary.nodes[access.subject].first];
    if (read.kind != SummaryAccess::Kind::Read || !read.key || read.isToContents())
        return;
    SummaryNode::Kind kindOfHolder = summary.nodes[read.subject].kind;
    if (kindOfHolder != SummaryNode::Kind::ResultOfCall && kindOfHolder != SummaryNode::Kind::Allocation)
        return;
    size_t place = m_runsOnObjectsInFieldsInFunction.findIf([&](const RunOnObjectInField& run) {
        return run.holder == read.subject && run.keyInHolder == read.key;
    });
    if (place == notFound) {
        if (m_runsOnObjectsInFieldsInFunction.size() > longestListGoneThrough)
            return;
        place = m_runsOnObjectsInFieldsInFunction.size();
        m_runsOnObjectsInFieldsInFunction.append({ read.subject, read.key, access.subject, { }, { }, { } });
    }
    Vector<PropertyKeyID, 8>& keys = m_runsOnObjectsInFieldsInFunction[place].keys;
    if (keys.size() <= maxFieldsInOrder && !keys.contains(key))
        keys.append(key);
}

void WriterOfLinkLayouts::finishRunsOnObjectsInFieldsIn(FunctionID id)
{
    if (m_runsOnObjectsInFieldsInFunction.isEmpty())
        return;
    const ThinSummary& summary = m_flow.summaries.at(id);
    uint32_t returned = noSummaryNode;
    bool returnsOneNode = true;
    for (const SummaryEdge& edge : summary.edges) {
        if (summary.nodes[edge.to].kind != SummaryNode::Kind::Result)
            continue;
        returnsOneNode &= returned == noSummaryNode || returned == edge.from;
        returned = edge.from;
    }
    for (RunOnObjectInField& run : m_runsOnObjectsInFieldsInFunction) {
        const LinkType& subject = typeAt(id, run.subject);
        if (subject.isNever())
            continue;
        if (subject.objects == LinkType::Objects::Layout || subject.objects == LinkType::Objects::Set) {
            forEachLayoutInType(m_flow, subject, [&](LayoutID layout) {
                run.layouts.append(layout);
            });
        }
        run.isOnObjectThatLinkHasLost = subject.hasUnknownObjects || subject.objects == LinkType::Objects::Lost;
        if (const SummaryNode& holder = summary.nodes[run.holder]; holder.kind == SummaryNode::Kind::ResultOfCall && !summary.calls[holder.first].isConstruction) {
            const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[id.index()] + holder.first];
            if (callees.listed().size() == 1)
                run.givenBy = callees.listed()[0];
        }
        if (returnsOneNode && run.holder == returned)
            m_runsOnFieldsOfWhatFunctionsReturn.set(static_cast<uint64_t>(id.index() + 1) << 32 | run.keyInHolder.bits(), static_cast<uint32_t>(m_runsOnObjectsInFields.size()) + 1);
        for (size_t place = m_runsOfMakers.size(); place-- && m_runsOfMakers[place].body == id;) {
            if (m_runsOfMakers[place].subject == run.holder) {
                run.runOnHolder = static_cast<uint32_t>(place) + 1;
                break;
            }
        }
        m_runsOnObjectsInFields.append(WTF::move(run));
    }
    m_runsOnObjectsInFieldsInFunction.shrink(0);
}

void WriterOfLinkLayouts::finishRunsOfMakersIn(FunctionID id)
{
    if (m_runsOfMakersInFunction.isEmpty() && !m_asksWhatBodyHandsOn)
        return;
    const ThinSummary& summary = m_flow.summaries.at(id);
    uint32_t returned = noSummaryNode;
    bool returnsOneNode = true;
    for (const SummaryEdge& edge : summary.edges) {
        if (summary.nodes[edge.to].kind != SummaryNode::Kind::Result)
            continue;
        returnsOneNode &= returned == noSummaryNode || returned == edge.from;
        returned = edge.from;
    }
    if (!returnsOneNode || returned == noSummaryNode || (summary.nodes[returned].kind != SummaryNode::Kind::ResultOfCall && summary.nodes[returned].kind != SummaryNode::Kind::Allocation))
        returned = noSummaryNode;
    if (returned != noSummaryNode && !m_runsOfMakersInFunction.containsIf([&](const RunOfMaker& run) { return run.subject == returned; }))
        m_runsOfMakersInFunction.append(RunOfMaker { returned, { }, { }, { }, false, false, { }, { } });
    for (RunOfMaker& run : m_runsOfMakersInFunction) {
        const LinkType& subject = typeAt(id, run.subject);
        if (subject.isNever())
            continue;
        if (subject.objects == LinkType::Objects::Layout || subject.objects == LinkType::Objects::Set) {
            forEachLayoutInType(m_flow, subject, [&](LayoutID layout) {
                run.layouts.append(layout);
            });
        }
        run.isOnObjectThatLinkHasLost = subject.hasUnknownObjects || subject.objects == LinkType::Objects::Lost;
        if (const SummaryNode& node = summary.nodes[run.subject]; node.kind == SummaryNode::Kind::ResultOfCall && !summary.calls[node.first].isConstruction) {
            const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[id.index()] + node.first];
            if (callees.listed().size() == 1)
                run.givenBy = callees.listed()[0];
        }
        run.isMadeHere = summary.nodes[run.subject].kind == SummaryNode::Kind::Allocation || summary.calls[summary.nodes[run.subject].first].isConstruction;
        run.isReturned = run.subject == returned;
        run.body = id;
        if (const BodyIndex* index = run.keys.size() > 1 ? bodiesLookedAt(m_flow.looks).find(id) : nullptr) {
            ValueID lastStore = *std::ranges::max_element(run.firstStoresOfKeys, { }, &ValueID::index);
            for (size_t place = 0; lastStore && place < run.keys.size(); ++place) {
                if (run.firstStoresOfKeys[place] && index->dominates(index->blockOf(run.firstStoresOfKeys[place]), index->blockOf(lastStore)))
                    run.keysStoredOnEveryWayToLastStore.append(run.keys[place]);
            }
        }
        if (run.subject == returned) {
            if (m_runsOnWhatFunctionsReturn.isEmpty())
                m_runsOnWhatFunctionsReturn.fill(0, m_program.functions.size());
            m_runsOnWhatFunctionsReturn[id.index()] = static_cast<uint32_t>(m_runsOfMakers.size()) + 1;
        }
        m_runsOfMakers.append(WTF::move(run));
    }
    m_runsOfMakersInFunction.shrink(0);
    if (!m_placesOfRunsInFunction.isEmpty())
        m_placesOfRunsInFunction.clear();
    if (!m_keysOfLongRunsInFunction.isEmpty())
        m_keysOfLongRunsInFunction.clear();
}

void WriterOfLinkLayouts::noteReadInFrontOfDefinition(FunctionID reader, const SummaryAccess& access)
{
    const LinkType& subject = typeAt(reader, access.subject);
    PropertyKeyID key = access.key;
    auto note = [&](LayoutID layout) {
        std::optional<FieldReference> found = findField(m_program, layout, key);
        if (found && m_flow.mayReadInFrontOfDefinition(reader, access, layout))
            m_fieldsReadInFrontOfDefinition.quickSet(cellOf(*found));
    };
    switch (subject.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        return;
    case LinkType::Objects::Layout:
        note(subject.layout);
        return;
    case LinkType::Objects::Set:
        for (LayoutID member : m_flow.layoutsOfSet(subject))
            note(member);
        return;
    case LinkType::Objects::Family:
        m_program.at(m_flow.families.layoutsOf(subject.family)).forEach(note);
        return;
    case LinkType::Objects::Lost:
        m_keysReadInFrontOfDefinitionThroughLost.add(key.bits());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE void WriterOfLinkLayouts::lookAtChanges(FunctionID id, const ThinSummary& summary)
{
    for (const SummaryChange& change : summary.changes) {
        noteHalfOfAccessorLaidOnPrototype(id, summary, change);
        const LinkType& subject = typeAt(id, change.subject);
        if (change.kind == SummaryChange::Kind::DefinesByDescriptor && change.key && change.valueLaidThroughConstant != noSummaryNode && !m_classesThatMakersLayInFields.isEmpty()) {
            for (const SummaryEdge& edge : summary.edges) {
                uint32_t made = edge.to == change.valueLaidThroughConstant ? whatIsMadeInSightAt(id, summary, edge.from) : 0;
                if (!made)
                    continue;
                m_flow.forEachLayoutOf(subject, [&](LayoutID holder) {
                    if (std::optional<FieldReference> found = findField(m_program, holder, change.key))
                        noteWhatMakerLays(m_classesThatMakersLayInFields[cellOf(declarationOf(m_program, *found))], made);
                });
            }
        }
        if (change.kind == SummaryChange::Kind::CopiesInto && change.definesWhatItCopies && (subject.objects == LinkType::Objects::Layout || subject.objects == LinkType::Objects::Set)) {
            m_flow.forEachLayoutOf(subject, [&](LayoutID filled) {
                if (m_program.at(filled).kind == LayoutKind::Record)
                    m_tables.recordsThatSpreadsFill.set(filled.index());
            });
        }
        bool waitsForOther = change.kind == SummaryChange::Kind::CopiesInto && change.other != noSummaryNode && typeAt(id, change.other).isNever();
        if (subject.isNever() || waitsForOther || (change.isSpelledByLoop && !m_flow.followsCopySpelledByLoop(subject, typeAt(id, change.other))))
            continue;
        noteChangeOfClosure(subject, change.kind == SummaryChange::Kind::DefinesByDescriptor ? change.key : PropertyKeyID());
        if (change.kind != SummaryChange::Kind::DefinesByDescriptor || !change.laysValueInSight || !change.key)
            noteNameGivenToClosure(subject);
        if (subject.objects == LinkType::Objects::Lost && subject.layout) {
            OptionSet<LayoutChange> changes = changesMadeByInLinkLayouts(change.kind);
            if (change.definesWhatItCopies)
                changes.remove(LayoutChange::ChangesPrototype);
            m_changesOfLostFamilies[lostFamilyOf(subject.layout)].add(changes);
        }

        switch (change.kind) {
        case SummaryChange::Kind::Freezes:
        case SummaryChange::Kind::ChangesPrototype:
            break;
        case SummaryChange::Kind::BecomesPrototype:
            if (change.other != noSummaryNode && summary.nodes[change.other].kind == SummaryNode::Kind::Closure)
                noteHeritage(subject, FunctionID(summary.nodes[change.other].first));
            break;
        case SummaryChange::Kind::Deletes:
            noteWriteOfPrototypeProperty(subject, change.key);
            noteWrite(subject, change.key, { Trait::ComesAfterConstruction, Trait::MayGiveValue });
            break;
        case SummaryChange::Kind::DefinesByDescriptor: {
            PropertyKeyID key = change.keyNode != noSummaryNode ? m_flow.keyHeldAt(id, change.keyNode) : change.key;
            noteWriteOfPrototypeProperty(subject, key);
            noteNameDefinedPastLists(subject, key, true);
            m_accessorOfLiteralIsAtHand = change.lays != SummaryChange::Lays::WhatDescriptorSays;
            m_changeByDescriptorIsAtHand = !m_accessorOfLiteralIsAtHand;
            m_changeAtHandMayLayAccessor = !change.descriptorIsInSight || !change.descriptorLaysNoAccessor;
            if (!change.isUnderNameThatNobodyElseNames)
                noteWrite(subject, key, change.lays == SummaryChange::Lays::WhatDescriptorSays && !(change.descriptorIsInSight && change.getterInSight != noSummaryNode && change.setterInSight == noSummaryNode) ? Traits { Trait::ComesAfterConstruction, Trait::MayGiveValue } : Traits { Trait::ComesAfterConstruction });
            m_accessorOfLiteralIsAtHand = false;
            m_changeByDescriptorIsAtHand = false;
            m_changeAtHandMayLayAccessor = false;
            if ((change.laysValueInSight || change.valueInSight != noSummaryNode) && change.other != noSummaryNode)
                noteValueLeftOutOfDescriptor(typeAt(id, change.other));
            break;
        }
        case SummaryChange::Kind::CopiesInto:
            noteWriteOfPrototypeProperty(subject, { });
            noteNameDefinedPastLists(subject, { }, change.definesWhatItCopies);
            noteWrite(subject, { }, { Trait::ComesAfterConstruction, Trait::MayGiveValue });
            if (change.isSpelledByLoop) {
                const LinkType& sources = typeAt(id, change.other);
                forEachLayoutInType(m_flow, sources, [&](LayoutID source) {
                    for (const Field& field : m_program.at(source).fields) {
                        if (!field.key || field.computedKey || isPrivateNameInLink(m_program, field.key))
                            continue;
                        noteWrite(subject, field.key, { Trait::AddsWhatIsNotListed, Trait::ComesAfterConstruction, Trait::MayGiveValue });
                        m_flow.forEachLayoutOf(subject, [&](LayoutID target) {
                            if (auto noted = m_pairsNotListed.find(static_cast<uint64_t>(target.index()) << 32 | field.key.bits()); noted != m_pairsNotListed.end())
                                m_namesNotListed[noted->value].isBroughtByCopyingLoop = true;
                        });
                    }
                });
            }
            break;
        }
    }
}

void WriterOfLinkLayouts::noteChangeOfClosure(const LinkType& subject, PropertyKeyID key)
{
    if (subject.functions != LinkType::Functions::Closure)
        return;
    if (key && key == m_program.keyOf(BuiltinNames::name) && m_program.at(subject.function).has(FunctionFlags::IsNamedByComputedKey))
        return;
    m_tables.functions[subject.function.index()].closuresAreChanged = true;
}

void WriterOfLinkLayouts::noteNameDefinedPastLists(const LinkType& holder, PropertyKeyID key, bool isDefinition)
{
    if (key && m_program.at(key).kind == PropertyKeyKind::PrivateName)
        return;
    auto note = [&](LayoutID classLayout, ClassSide side) {
        const Layout& layout = m_program.at(classLayout);
        if (layout.has(LayoutFlags::IsConstructorFunction))
            return;
        if (key && (classHolderIsMadeWithName(m_program, side, key) || (side == ClassSide::Static && layout.findStaticField(key))))
            return;
        for (const LinkFlow::FoundMember& found : m_flow.findMembers(classLayout, side, key)) {
            if (found.classLayout == classLayout || (!isDefinition && found.kind != MethodKind::Method))
                return;
        }
        m_sidesWithNamesDefinedPastLists.quickSet(2 * static_cast<size_t>(classLayout.index()) + static_cast<size_t>(side));
        if (!key && side == ClassSide::Prototype)
            m_prototypesDefinedOnUnderKeysThatNobodyKnows.quickSet(classLayout.index());
    };
    auto noteOnClassObjectOf = [&](FunctionID function) {
        if (LayoutID classLayout = m_flow.classOfConstructor(function))
            note(classLayout, ClassSide::Static);
    };
    auto noteOnEachOf = [&](FamilyID family, ClassSide side, const auto& noteOn) {
        size_t sideOfFamily = 2 * static_cast<size_t>(family.index()) + static_cast<size_t>(side);
        if (m_sidesOfFamiliesWhollyPastLists.get(sideOfFamily) || (key && classHolderIsMadeWithName(m_program, side, key)))
            return;
        bool isWhollyPastLists = true;
        m_program.at(m_flow.families.layoutsOf(family)).forEach([&](LayoutID layout) {
            noteOn(layout);
            isWhollyPastLists &= m_sidesWithNamesDefinedPastLists.quickGet(2 * static_cast<size_t>(layout.index()) + static_cast<size_t>(side));
        });
        if (isWhollyPastLists)
            m_sidesOfFamiliesWhollyPastLists.set(sideOfFamily);
    };
    switch (holder.functions) {
    case LinkType::Functions::None:
        break;
    case LinkType::Functions::Lost:
        m_namesAreDefinedOnLostFunctions |= !key || !classHolderIsMadeWithName(m_program, ClassSide::Static, key);
        break;
    case LinkType::Functions::Closure:
        noteOnClassObjectOf(holder.function);
        break;
    case LinkType::Functions::Set:
        std::ranges::for_each(m_flow.functionsOfSet(holder), noteOnClassObjectOf);
        break;
    case LinkType::Functions::Constructors:
        noteOnEachOf(holder.constructorsOf, ClassSide::Static, [&](LayoutID layout) {
            if (FunctionID constructor = m_program.at(layout).constructor)
                noteOnClassObjectOf(constructor);
        });
        break;
    }
    if (holder.objects == LinkType::Objects::Prototype)
        note(holder.layout, ClassSide::Prototype);
    else if (holder.objects == LinkType::Objects::Prototypes) {
        noteOnEachOf(holder.family, ClassSide::Prototype, [&](LayoutID layout) {
            note(layout, ClassSide::Prototype);
        });
    }
}

void WriterOfLinkLayouts::noteWriteOfPrototypeProperty(const LinkType& subject, PropertyKeyID key, bool storesObjectMadeForIt)
{
    if (key && m_program.nameClassOf(key) != NameClass::Prototype)
        return;
    if (subject.functions == LinkType::Functions::Closure && storesObjectMadeForIt && !m_functionsGivenObjectMadeToBePrototype.set(subject.function.index()))
        return;
    if (subject.functions == LinkType::Functions::Closure)
        m_functionsWhosePrototypePropertyIsWritten.set(subject.function.index());
    else if (subject.functions == LinkType::Functions::Constructors)
        m_prototypePropertyOfSomeConstructorIsWritten = true;
}

void WriterOfLinkLayouts::noteNameGivenToClosure(const LinkType& subject, PropertyKeyID key, bool laysValueInSight)
{
    auto note = [&](FunctionID function) {
        bool isFollowed = key && m_flow.followsNamesOnClosuresOf(function, key);
        if (isFollowed && m_flow.lostFunctions.get(function.index()))
            m_namesThatStatementsLayOnLostFunctions.add(static_cast<uint64_t>(function.index()) << 32 | key.bits());
        if (!isFollowed || !laysValueInSight)
            m_functionsWhoseClosuresGetWhatIsNotListed.set(function.index());
    };
    switch (subject.functions) {
    case LinkType::Functions::None:
    case LinkType::Functions::Lost:
        return;
    case LinkType::Functions::Closure:
        note(subject.function);
        return;
    case LinkType::Functions::Set:
        std::ranges::for_each(m_flow.functionsOfSet(subject), note);
        return;
    case LinkType::Functions::Constructors:
        m_someConstructorGetsName = true;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void WriterOfLinkLayouts::writeNamesOnFunctions()
{
    for (const LinkFlow::NameOnClosures& name : m_flow.namesOnClosures()) {
        const LinkType& held = m_flow.typeAt(name.laid);
        if (held.isNever())
            continue;
        if (m_flow.lostFunctions.get(name.function.index()) && !m_namesThatStatementsLayOnLostFunctions.contains(static_cast<uint64_t>(name.function.index()) << 32 | name.key.bits()))
            continue;
        LinkedNameOnFunction linked { name.function, name.key, FunctionID() };
        LinkType oneClosure;
        oneClosure.functions = LinkType::Functions::Closure;
        oneClosure.function = held.function;
        if (held == oneClosure)
            linked.expected = held.function;
        m_tables.namesOnFunctions.append(linked);
    }
    std::ranges::sort(m_tables.namesOnFunctions, { }, [](const LinkedNameOnFunction& name) {
        return std::pair { name.holder, name.key };
    });
}

void WriterOfLinkLayouts::writeRowsBelowNamesDefinedPastLists()
{
    BitVector isAnswered(m_program.layouts.size());
    Vector<LayoutID, 16> row;
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        LayoutID current(index);
        for (; current && !isAnswered.quickSet(current.index()); current = m_program.at(current).base)
            row.append(current);
        std::array<bool, 2> areDefined = current ? m_tables.layouts[current.index()].namesAreDefinedPastLists : std::array<bool, 2> { };
        for (; !row.isEmpty(); row.removeLast()) {
            size_t layout = row.last().index();
            areDefined[static_cast<size_t>(ClassSide::Prototype)] |= m_sidesWithNamesDefinedPastLists.quickGet(2 * layout + static_cast<size_t>(ClassSide::Prototype));
            areDefined[static_cast<size_t>(ClassSide::Static)] |= m_sidesWithNamesDefinedPastLists.quickGet(2 * layout + static_cast<size_t>(ClassSide::Static)) || (m_namesAreDefinedOnLostFunctions && m_liesBelowLostClassObject.quickGet(layout));
            m_tables.layouts[layout].namesAreDefinedPastLists = areDefined;
        }
    }
}

void WriterOfLinkLayouts::writeWhosePrototypePropertyStaysAsMade()
{
    forEachRangeOfIndicesInParallel(m_flow.loopsOfReading(), m_tables.functions.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            LinkedFunction& function = m_tables.functions[index];
            function.closuresGetOnlyNamesThatAreListed = function.everyCallerIsKnown && !m_someConstructorGetsName && !m_functionsWhoseClosuresGetWhatIsNotListed.get(index);
            bool isWrittenInNoOtherWay = function.everyCallerIsKnown && !m_prototypePropertyOfSomeConstructorIsWritten && !m_functionsWhosePrototypePropertyIsWritten.get(index);
            function.prototypePropertyStaysAsMade = isWrittenInNoOtherWay && !m_functionsGivenObjectMadeToBePrototype.get(index);
        }
    });
}

void WriterOfLinkLayouts::noteHeritage(const LinkType& heritage, FunctionID constructor)
{
    LayoutID classLayout = m_flow.classOfConstructor(constructor);
    if (!classLayout || m_program.at(classLayout).base)
        return;
    LinkType objects = heritage;
    objects.primitives = 0;
    LayoutID above;
    if (!(heritage.primitives & Primitive::Null) && objects.functions == LinkType::Functions::Closure && objects == typeOfClosuresInLinkLayouts(objects.function)) {
        above = m_flow.classOfConstructor(objects.function);
        if (!above)
            above = m_flow.layoutOfConstructorFunction(objects.function);
    }
    if (m_hasHeritage.set(classLayout.index()) && m_heritages[classLayout.index()] != above)
        above = { };
    m_heritages[classLayout.index()] = above;
}

LayoutID WriterOfLinkLayouts::findLayoutWhosePrototypeObjectIsAt(const ThinSummary& summary, uint32_t node) const
{
    const SummaryNode& object = summary.nodes[node];
    if (object.kind != SummaryNode::Kind::ResultOfAccess)
        return { };
    const SummaryAccess& read = summary.accesses[object.first];
    if (read.kind != SummaryAccess::Kind::Read || read.key != m_program.keyOf(BuiltinNames::prototype))
        return { };
    const SummaryNode& holder = summary.nodes[read.subject];
    FunctionID function;
    if (holder.kind == SummaryNode::Kind::Closure)
        function = FunctionID(holder.first);
    else if (holder.kind == SummaryNode::Kind::Variable && m_program.variables[holder.first].initialization == VariableInitialization::Function)
        function = m_program.variables[holder.first].function;
    return function ? m_flow.layoutOfConstructorFunction(function) : LayoutID();
}

void WriterOfLinkLayouts::noteHalfOfAccessorLaidOnPrototype(FunctionID id, const ThinSummary& summary, const SummaryChange& change)
{
    if (change.kind != SummaryChange::Kind::DefinesByDescriptor || change.lays == SummaryChange::Lays::WhatDescriptorSays || !change.key)
        return;
    LayoutID layout = findLayoutWhosePrototypeObjectIsAt(summary, change.subject);
    if (!layout)
        layout = onlyPrototypeOfConstructorFunctionInLinkLayouts(m_flow, typeAt(id, change.subject));
    if (!layout)
        return;
    m_keysWrittenOnPrototypes.add(change.key.bits());
    NameWrittenOnPrototype& written = m_namesWrittenOnPrototypes.add(static_cast<uint64_t>(layout.index()) << 32 | change.key.bits(), NameWrittenOnPrototype { { }, { }, { }, true }).iterator->value;
    FunctionID& half = change.lays == SummaryChange::Lays::Getter ? written.getter : written.setter;
    bool laysClosure = change.other != noSummaryNode && summary.nodes[change.other].kind == SummaryNode::Kind::Closure;
    if (written.isWrittenOnce && !written.method && !half && laysClosure) {
        half = FunctionID(summary.nodes[change.other].first);
        return;
    }
    written.isWrittenOnce = false;
    written.isWrittenAgain = true;
}

void WriterOfLinkLayouts::noteNameWrittenOnPrototype(FunctionID id, const ThinSummary& summary, const SummaryAccess& access)
{
    if (access.key && access.mapOfWrittenAccessor) {
        auto added = m_accessorsWrittenInMaps.add(static_cast<uint64_t>(access.mapOfWrittenAccessor.index()) << 32 | access.key.bits(), NameWrittenOnPrototype { access.writtenGetter, access.writtenSetter, { }, true });
        if (!added.isNewEntry && (added.iterator->value.getter != access.writtenGetter || added.iterator->value.setter != access.writtenSetter))
            added.iterator->value.isWrittenOnce = false;
    }
    if (access.key && access.kind == SummaryAccess::Kind::Store && access.value != noSummaryNode && summary.nodes[access.value].kind == SummaryNode::Kind::Closure && summary.nodes[access.subject].kind == SummaryNode::Kind::ResultOfAccess) {
        const SummaryAccess& read = summary.accesses[summary.nodes[access.subject].first];
        if (read.kind == SummaryAccess::Kind::Read && read.key == m_program.keyOf(BuiltinNames::prototype))
            m_tables.functionsWrittenRightOfNamesOnPrototypes.append({ FunctionID(summary.nodes[access.value].first), access.key });
    }
    PropertyKeyID key = m_flow.keyOf(id, access);
    LayoutID layout = key ? findLayoutWhosePrototypeObjectIsAt(summary, access.subject) : LayoutID();
    if (!layout)
        return;
    NameWrittenOnPrototype written;
    if (access.kind == SummaryAccess::Kind::Define && access.laysAccessorAsWritten)
        written = { access.writtenGetter, access.writtenSetter, { }, true };
    else if (!access.isByDescriptor && access.value != noSummaryNode && summary.nodes[access.value].kind == SummaryNode::Kind::Closure)
        written = { { }, { }, FunctionID(summary.nodes[access.value].first), true };
    m_keysWrittenOnPrototypes.add(key.bits());
    auto added = m_namesWrittenOnPrototypes.add(static_cast<uint64_t>(layout.index()) << 32 | key.bits(), written);
    if (!added.isNewEntry) {
        added.iterator->value.isWrittenOnce = false;
        added.iterator->value.isWrittenAgain = true;
        return;
    }
    if (access.kind != SummaryAccess::Kind::Define || access.descriptorFromElsewhere == noSummaryNode || summary.nodes[access.descriptorFromElsewhere].kind != SummaryNode::Kind::ResultOfAccess)
        return;
    const SummaryAccess& taken = summary.accesses[summary.nodes[access.descriptorFromElsewhere].first];
    LinkType map = typeAt(id, taken.subject);
    if (isLibraryCode(m_program, m_program.at(id)))
        map = map.directPart();
    if (taken.kind == SummaryAccess::Kind::Read && taken.key && map.objects == LinkType::Objects::Layout && map.functions == LinkType::Functions::None && !map.hasUnknownObjects)
        m_namesLaidOutOfMaps.append({ static_cast<uint64_t>(layout.index()) << 32 | key.bits(), static_cast<uint64_t>(map.layout.index()) << 32 | taken.key.bits() });
}

bool WriterOfLinkLayouts::isWrittenOnPrototypeAbove(LayoutID layout, PropertyKeyID key) const
{
    if (!m_keysWrittenOnPrototypes.contains(key.bits()))
        return false;
    size_t stepsLeft = m_program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; --stepsLeft) {
        if (m_namesWrittenOnPrototypes.contains(static_cast<uint64_t>(current.index()) << 32 | key.bits()))
            return true;
        LayoutID base = m_program.at(current).base;
        current = base ? base : m_heritages[current.index()];
    }
    return false;
}

void WriterOfLinkLayouts::noteWrite(const LinkType& subject, PropertyKeyID key, Traits traits)
{
    if (key && traits.contains(Trait::AddsWhatIsNotListed) && (subject.hasUnknownObjects || subject.objects == LinkType::Objects::Lost))
        m_keysStoredOnUnknownObjects.add(key.bits());

    switch (subject.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        return;
    case LinkType::Objects::Layout:
        noteWriteInto(subject.layout, key, traits);
        return;
    case LinkType::Objects::Set:
        for (LayoutID member : m_flow.layoutsOfSet(subject))
            noteWriteInto(member, key, traits);
        return;
    case LinkType::Objects::Family: {
        bool comesFromDescriptor = (m_definitionAtHand && m_definitionAtHand->isByDescriptor) || m_changeByDescriptorIsAtHand || m_accessorOfLiteralIsAtHand;
        if (!comesFromDescriptor) {
            uint8_t& noted = m_writesNotedThroughFamilies.add(static_cast<uint64_t>(subject.family.index()) << 32 | key.bits(), 0).iterator->value;
            if ((noted & traits.toRaw()) == traits.toRaw())
                return;
            noted |= traits.toRaw();
        }
        m_program.at(m_flow.families.layoutsOf(subject.family)).forEach([&](LayoutID member) {
            noteWriteInto(member, key, traits, mayBeKnownByFamily(member));
        });
        return;
    }
    case LinkType::Objects::Lost: {
        if (!subject.layout)
            return;
        uint32_t family = lostFamilyOf(subject.layout);
        if (traits.contains(Trait::AddsWhatIsNotListed) && !key)
            m_changesOfLostFamilies[family].add(LayoutChange::GainsProperty);
        if (traits.contains(Trait::MayRunSetter) && (!key || m_program.nameClassOf(key) == NameClass::Proto))
            m_changesOfLostFamilies[family].add(LayoutChange::ChangesPrototype);
        if (traits.contains(Trait::MayGiveValue) && key)
            m_keysGivenValuesThroughLost.add(static_cast<uint64_t>(family) << 32 | key.bits());
        else if (traits.contains(Trait::MayGiveValue))
            m_lostFamiliesGivenValuesUnderEveryKey.set(family);
        if (!traits.contains(Trait::ComesAfterConstruction))
            return;
        if (key)
            m_keysWrittenLateThroughLost.add(static_cast<uint64_t>(family) << 32 | key.bits());
        else
            m_lostFamiliesWrittenLateUnderEveryKey.set(family);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const Vector<LinkType, 1>& WriterOfLinkLayouts::olderObjectsThatSuperCallAnswersWith(LayoutID constructed)
{
    Vector<LayoutID, 16> row;
    for (LayoutID current = constructed; current && !m_olderObjectsThatSuperCallsAnswerWith.contains(current.index()); current = m_program.at(current).base)
        row.append(current);
    for (LayoutID current : row | std::views::reverse) {
        Vector<LinkType, 1> older;
        if (LayoutID base = m_program.at(current).base) {
            older = m_olderObjectsThatSuperCallsAnswerWith.get(base.index());
            if (FunctionID constructor = m_program.at(base).constructor; constructor && m_program.at(constructor).hasBody()) {
                m_flow.forEachNodeAnsweredBesideObjectOfConstruction(constructor, scopedLambda<void(uint32_t)>([&](uint32_t node) {
                    if (const LinkType& answered = typeAt(constructor, node); answered.objects != LinkType::Objects::None && !older.contains(answered))
                        older.append(answered);
                }));
            }
        }
        m_olderObjectsThatSuperCallsAnswerWith.add(current.index(), WTF::move(older));
    }
    return m_olderObjectsThatSuperCallsAnswerWith.find(constructed.index())->value;
}

void WriterOfLinkLayouts::noteStoreIntoOlderObject(FunctionID writer, const SummaryAccess& access, PropertyKeyID key, Traits traits)
{
    traits.add(Trait::ComesAfterConstruction);
    if (access.kind == SummaryAccess::Kind::Store)
        traits.remove(Trait::MayGiveValue);
    for (const LinkType& older : olderObjectsThatSuperCallAnswersWith(m_program.at(writer).constructedClass))
        noteWrite(older, key, traits);
}

void WriterOfLinkLayouts::noteValueLeftOutOfDescriptor(const LinkType& descriptor)
{
    PropertyKeyID key = m_program.keyOf(BuiltinNames::value);
    auto note = [&](LayoutID layout) {
        if (std::optional<FieldReference> held = findField(m_program, layout, key)) {
            m_fieldsWithUnseenWriters.quickSet(cellOf(*held));
            m_fieldsGivenValues.quickSet(cellOf(*held));
        }
    };
    m_flow.forEachLayoutOf(descriptor, note);
}

void WriterOfLinkLayouts::noteWriteInto(LayoutID layout, PropertyKeyID key, Traits traits, bool mayComeToObject)
{
    OptionSet<LayoutChange>& changes = m_changes[layout.index()];
    if (traits.contains(Trait::MayRunSetter) && (!key || m_program.nameClassOf(key) == NameClass::Proto))
        changes.add(LayoutChange::ChangesPrototype);

    if (!key) {
        uint8_t& noted = m_whatIsNotedOfEveryField[layout.index()];
        uint32_t firstCell = m_tables.firstFieldOfLayout[layout.index()];
        uint32_t endOfCells = m_tables.firstFieldOfLayout[layout.index() + 1];
        if (!(noted & unitsOfEveryFieldAreNoted)) {
            noted |= unitsOfEveryFieldAreNoted;
            for (uint32_t cell = firstCell; cell < endOfCells; ++cell)
                m_unitsListedForFields[cell] = otherThanOneUnit;
        }
        bool comesFromDefinition = m_accessorOfLiteralIsAtHand || (m_definitionAtHand && m_definitionAtHand->isByDescriptor) || m_changeByDescriptorIsAtHand;
        if (!comesFromDefinition && !(noted & everyFieldIsWrittenWithoutDefinition)) {
            noted |= everyFieldIsWrittenWithoutDefinition;
            for (uint32_t cell = firstCell; cell < endOfCells; ++cell)
                m_fieldsWrittenWithoutDefinition.quickSet(cell);
        }
        if (traits.contains(Trait::MayGiveValue) && !(noted & everyFieldIsGivenValues)) {
            noted |= everyFieldIsGivenValues;
            for (uint32_t cell = firstCell; cell < endOfCells; ++cell)
                m_fieldsGivenValues.quickSet(cell);
        }
        if (traits.contains(Trait::AddsWhatIsNotListed)) {
            changes.add(LayoutChange::GainsProperty);
            if (mayComeToObject)
                m_isFilledUnderComputedKeys.quickSet(layout.index());
        }
        if (traits.contains(Trait::ComesAfterConstruction) && !(noted & everyFieldIsWrittenLate)) {
            noted |= everyFieldIsWrittenLate;
            for (uint32_t cell = firstCell; cell < endOfCells; ++cell) {
                if (!isPrivateNameInLink(m_program, m_program.at(layout).fields[cell - firstCell].key))
                    m_fieldsWrittenLate.quickSet(cell);
            }
        }
        return;
    }

    std::optional<FieldReference> found = findField(m_program, layout, key);
    if (!found) {
        if (!traits.contains(Trait::AddsWhatIsNotListed) || (traits.contains(Trait::MayRunSetter) && comesToAccessorAbove(layout, key)))
            return;
        bool isFieldOfClassBelow = !traits.contains(Trait::ComesAfterConstruction) && isListedByClassBelow(layout, key);
        if (!(mayComeToObject && !isFieldOfClassBelow && noteNameNotListed(layout, key))) {
            changes.add(LayoutChange::GainsProperty);
            m_tables.namesStoredBesideFields.append({ layout, key });
        }
        return;
    }
    m_unitsListedForFields[cellOf(*found)] = otherThanOneUnit;
    if (m_accessorOfLiteralIsAtHand || (m_definitionAtHand && m_definitionAtHand->isByDescriptor))
        m_fieldsInWhichDefinitionsLayForms.quickSet(cellOf(*found));
    if (m_accessorOfLiteralIsAtHand)
        m_formsLaidInListedFields.add(cellOf(*found), 0).iterator->value |= 1u << OptionSet<FieldForm> { FieldForm::IsAccessor }.toRaw();
    else if (m_definitionAtHand && m_definitionAtHand->isByDescriptor)
        m_formsLaidInListedFields.add(cellOf(*found), 0).iterator->value |= m_definitionAtHand->formCreated ? 1u << m_definitionAtHand->formCreated->toRaw() : formNobodyKnows;
    else if (m_changeByDescriptorIsAtHand) {
        m_formsLaidInListedFields.add(cellOf(*found), 0);
        if (m_changeAtHandMayLayAccessor)
            m_fieldsInWhichDescriptorsMayLayAccessors.quickSet(cellOf(*found));
    } else
        m_fieldsWrittenWithoutDefinition.quickSet(cellOf(*found));
    if (traits.contains(Trait::ComesAfterConstruction))
        m_fieldsWrittenLate.quickSet(cellOf(*found));
    if (traits.contains(Trait::MayGiveValue))
        m_fieldsGivenValues.quickSet(cellOf(*found));
    if (traits.contains(Trait::GivesUndefinedToObjectBeingMade)) {
        m_fieldsGivenUndefinedByMakers.quickSet(cellOf(*found));
        m_layoutsGivenUndefinedByMakers.quickSet(layout.index());
    }
    if (traits.contains(Trait::FillsObjectBeingConstructed))
        m_fieldsFilledByConstructors.quickSet(cellOf(*found));
    if (traits.contains(Trait::BringsUndefinedOrNull))
        (traits.contains(Trait::ComesAfterConstruction) ? m_fieldsGivenUndefinedOrNullLate : m_fieldsGivenUndefinedOrNullByMakers).quickSet(cellOf(*found));
    else if (traits.contains(Trait::FillsObjectBeingConstructed))
        m_fieldsGivenValuesByMakers.quickSet(cellOf(*found));
}

bool WriterOfLinkLayouts::canHoldAppendedFields(LayoutID id) const
{
    const Layout& layout = m_program.at(id);
    return isFoundByShapeTest(layout) && !layout.isDerived() && !layout.has(LayoutFlags::IsNeverAllocated) && mayBeAllocated(id);
}

bool WriterOfLinkLayouts::canNameAppendedField(LayoutID layout, PropertyKeyID key) const
{
    PropertyKeyKind kind = m_program.at(key).kind;
    if (!m_program.types.hasStableNumber(key) || (kind != PropertyKeyKind::Text && kind != PropertyKeyKind::CreatedSymbol && kind != PropertyKeyKind::RegisteredSymbol))
        return false;
    NameClass nameClass = m_program.nameClassOf(key);
    uint64_t layoutAndKey = static_cast<uint64_t>(layout.index()) << 32 | key.bits();
    if (m_namesThatHoldersLeaveToHeirs.contains(layoutAndKey))
        return false;
    bool countsKeysThatNobodyKnows = kind == PropertyKeyKind::Text && !m_namesThatConstructorsStore.contains(layoutAndKey) && !m_namesLaidOnObjectsOfLibrary.contains(layoutAndKey);
    if (countsKeysThatNobodyKnows && m_program.at(layout).has(LayoutFlags::IsOfCreatedObject))
        return false;
    size_t numberOfClassesLeft = m_program.layouts.size();
    for (LayoutID current = countsKeysThatNobodyKnows ? layout : LayoutID(); current && numberOfClassesLeft--;) {
        if (m_prototypesDefinedOnUnderKeysThatNobodyKnows.quickGet(current.index()))
            return false;
        LayoutID base = writtenBaseOf(m_program.at(current));
        current = base ? base : m_heritages[current.index()];
    }
    bool constructorStoresOverMethod = m_namesThatConstructorsStore.contains(layoutAndKey) && m_program.at(layout).kind == LayoutKind::Instance && !m_program.at(layout).has(LayoutFlags::IsOfCreatedObject);
    if (mayBeElementKey(nameClass) || nameClass == NameClass::Proto || (!constructorStoresOverMethod && m_flow.findMembers(layout, ClassSide::Prototype, key).mayAnswer()) || m_flow.mayLieOnPrototypeAbove(layout, key, countsKeysThatNobodyKnows))
        return false;
    LayoutID heritage = m_heritages[layout.index()];
    return !isWrittenOnPrototypeAbove(layout, key) && (!heritage || (!m_flow.findMembers(heritage, ClassSide::Prototype, key).mayAnswer() && !m_flow.mayLieOnPrototypeAbove(heritage, key, countsKeysThatNobodyKnows)));
}

bool WriterOfLinkLayouts::isListedByClassBelow(LayoutID layout, PropertyKeyID key) const
{
    if (!m_program.hasDeclaredMembers(layout))
        return false;
    SetView<LayoutID, LayoutSetID> members = m_program.at(m_program.declaredMembersOf(layout));
    auto listsName = [&](LayoutID member) {
        return member != layout && writtenBaseOf(m_program.at(member)) && m_program.at(member).findField(key);
    };
    std::span<const LayoutID> layoutsWithName = m_program.layoutsWithName(key);
    if (layoutsWithName.size() <= members.size()) {
        for (LayoutID holder : layoutsWithName) {
            if (members.contains(holder) && listsName(holder))
                return true;
        }
    }
    return members.anyOf(listsName);
}

bool WriterOfLinkLayouts::noteNameNotListed(LayoutID layout, PropertyKeyID key)
{
    if (!canHoldAppendedFields(layout) || !canNameAppendedField(layout, key))
        return false;
    auto added = m_pairsNotListed.add(static_cast<uint64_t>(layout.index()) << 32 | key.bits(), static_cast<uint32_t>(m_namesNotListed.size()));
    if (added.isNewEntry)
        m_namesNotListed.append({ layout, key });
    if (!m_definitionAtHand)
        m_namesNotListed[added.iterator->value].isWrittenWithoutDefinition = true;
    if (m_definitionAtHand) {
        NameNotListed& name = m_namesNotListed[added.iterator->value];
        name.isDefinedByDescriptors |= m_definitionAtHand->isByDescriptor;
        if (m_definitionAtHand->isByDescriptor && m_definitionAtHand->formCreated)
            name.formsThatDescriptorsCreate |= static_cast<uint16_t>(1u << m_definitionAtHand->formCreated->toRaw());
        else
            name.isDefinedInOtherWays = true;
    }
    return true;
}

void WriterOfLinkLayouts::noteNamesWrittenOfLayouts()
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
    auto isWrittenTypeAlone = [&](LayoutID id) {
        return m_program.at(id).has(LayoutFlags::IsNeverAllocated);
    };
    Vector<Vector<LayoutID>> layoutsWrittenAs(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!canHoldAppendedFields(LayoutID(index)))
            continue;
        for (LayoutID view : m_program.viewsAbove(LayoutID(index))) {
            if (isWrittenTypeAlone(view))
                layoutsWrittenAs[view.index()].append(LayoutID(index));
        }
    }

    Vector<bool, 16> mayHideBehindUnknownObjects;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& written = m_program.layouts[index];
        std::span<const LayoutID> described = layoutsWrittenAs[index].span();
        if (described.empty() || written.fields.isEmpty())
            continue;

        mayHideBehindUnknownObjects.shrink(0);
        for (LayoutID id : described)
            mayHideBehindUnknownObjects.append(m_flow.lostLayouts.get(id.index()) || hasEscaped(id.index()));

        for (const Field& field : written.fields) {
            if (!field.key || field.form.contains(FieldForm::IsAccessor) || field.flags.contains(FieldFlags::IsMethod))
                continue;
            bool isStoredOnUnknownObjects = m_keysStoredOnUnknownObjects.contains(field.key.bits());
            for (size_t place = 0; place < described.size(); ++place) {
                LayoutID id = described[place];
                auto noted = m_pairsNotListed.find(static_cast<uint64_t>(id.index()) << 32 | field.key.bits());
                if (noted == m_pairsNotListed.end()) {
                    if (!isStoredOnUnknownObjects || !mayHideBehindUnknownObjects[place] || findField(m_program, id, field.key) || !noteNameNotListed(id, field.key))
                        continue;
                    noted = m_pairsNotListed.find(static_cast<uint64_t>(id.index()) << 32 | field.key.bits());
                }
                NameNotListed& name = m_namesNotListed[noted->value];
                bool isRequired = field.flags.contains(FieldFlags::IsRequired);
                name.isRequiredBySomeWrittenType |= isRequired;
                if (m_program.at(id).supertypes.contains(LayoutID(index)))
                    (isRequired ? name.isRequiredBySomeTypeOfThis : name.isOptionalInSomeTypeOfThis) = true;
            }
        }
    }
}

void WriterOfLinkLayouts::noteNamesThatMakersGive()
{
    noteNamesWrittenOfLayouts();
    BitVector bodiesAskedWhatTheyHandOn(m_program.functions.size());
    for (size_t place = 0; place < m_runsOfMakers.size(); ++place) {
        FunctionID givenBy = m_runsOfMakers[place].givenBy;
        if (!givenBy || bodiesAskedWhatTheyHandOn.quickSet(givenBy.index()) || !m_flow.reached.get(givenBy.index()))
            continue;
        if (!m_runsOnWhatFunctionsReturn.isEmpty() && m_runsOnWhatFunctionsReturn[givenBy.index()])
            continue;
        m_asksWhatBodyHandsOn = true;
        finishRunsOfMakersIn(givenBy);
        m_asksWhatBodyHandsOn = false;
    }
    BitVector bodiesThatOtherMakersBuildOn(m_program.functions.size());
    for (const RunOfMaker& run : m_runsOfMakers) {
        if (run.givenBy)
            bodiesThatOtherMakersBuildOn.quickSet(run.givenBy.index());
    }
    noteNamesThatFollowersOfMakersStore();
    HashMap<uint32_t, Vector<LayoutID, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> layoutsStoredUnderKey;
    auto placeOfName = [&](LayoutID layout, PropertyKeyID key) -> std::optional<uint32_t> {
        auto found = m_pairsNotListed.find(static_cast<uint64_t>(layout.index()) << 32 | key.bits());
        return found == m_pairsNotListed.end() ? std::nullopt : std::optional(found->value);
    };
    for (NameNotListed& name : m_namesNotListed)
        name.isGivenByConstructor = m_namesThatConstructorsStore.contains(static_cast<uint64_t>(name.layout.index()) << 32 | name.key.bits());
    for (NameNotListed& name : m_namesNotListed)
        name.isLaidOnObjectsOfLibrary = m_namesLaidOnObjectsOfLibrary.contains(static_cast<uint64_t>(name.layout.index()) << 32 | name.key.bits());
    for (NameNotListed& name : m_namesNotListed) {
        uint64_t layoutAndKey = static_cast<uint64_t>(name.layout.index()) << 32 | name.key.bits();
        name.isLaidLate = m_namesThatMembersAboveStore.contains(layoutAndKey) && !m_namesThatMakersStore.contains(layoutAndKey);
    }
    Vector<const RunOfMaker*, 8> makersInFront;
    for (const RunOfMaker& run : m_runsOfMakers) {
        if (!run.isReturned || run.keys.isEmpty() || !bodiesThatOtherMakersBuildOn.quickGet(run.body.index()))
            continue;
        makersInFront.shrink(0);
        const RunOfMaker* first = &run;
        while (!first->isMadeHere && first->givenBy && !m_runsOnWhatFunctionsReturn.isEmpty() && m_runsOnWhatFunctionsReturn[first->givenBy.index()] && !makersInFront.contains(first)) {
            makersInFront.append(first);
            first = &m_runsOfMakers[m_runsOnWhatFunctionsReturn[first->givenBy.index()] - 1];
        }
        if (!first->isMadeHere)
            continue;
        auto giveNamesTo = [&](LayoutID layout) {
            for (size_t index = 0; index < run.keys.size(); ++index) {
                PropertyKeyID key = run.keys[index];
                if (std::optional<uint32_t> place = placeOfName(layout, key))
                    m_namesNotListed[*place].isGivenBySecondConstructor = true;
                else if (std::optional<FieldReference> listed = index < 64 && run.keysOnlyGivenUndefined >> index & 1 ? std::nullopt : findField(m_program, layout, key))
                    m_fieldsFilledByConstructors.quickSet(cellOf(*listed));
            }
        };
        if (const LayoutsOfRun& known = run.layouts.isEmpty() ? first->layouts : run.layouts; !known.isEmpty()) {
            for (LayoutID layout : known)
                giveNamesTo(layout);
            continue;
        }
        if (layoutsStoredUnderKey.isEmpty()) {
            for (const NameNotListed& name : m_namesNotListed)
                layoutsStoredUnderKey.add(name.key.bits(), Vector<LayoutID, 2> { }).iterator->value.append(name.layout);
        }
        for (LayoutID layout : m_asksWhatMakersGive ? m_program.layoutsWithName(run.keys[0]) : std::span<const LayoutID> { }) {
            bool listsEveryName = m_layoutsGivenUndefinedByMakers.quickGet(layout.index()) && m_program.at(layout).kind == LayoutKind::Instance && std::ranges::all_of(run.keys, [&](PropertyKeyID key) {
                return !!findField(m_program, layout, key);
            });
            if (listsEveryName)
                giveNamesTo(layout);
        }
        auto candidates = layoutsStoredUnderKey.find(run.keys[0].bits());
        if (candidates == layoutsStoredUnderKey.end())
            continue;
        for (LayoutID layout : candidates->value) {
            bool hasEveryName = std::ranges::all_of(run.keys, [&](PropertyKeyID key) {
                return placeOfName(layout, key) || findField(m_program, layout, key);
            });
            if (hasEveryName)
                giveNamesTo(layout);
        }
    }
}

void WriterOfLinkLayouts::appendFields()
{
    m_pairsNotListed.clear();
    m_namesNotListed.removeAllMatching([&](const NameNotListed& name) {
        bool bringsFormToObjectsFilledUnderComputedKeys = m_isFilledUnderComputedKeys.quickGet(name.layout.index()) && !name.formToPlanFor().isEmpty();
        if (!bringsFormToObjectsFilledUnderComputedKeys && canNameAppendedField(name.layout, name.key))
            return false;
        m_changes[name.layout.index()].add(LayoutChange::GainsProperty);
        m_tables.namesStoredBesideFields.append({ name.layout, name.key });
        return true;
    });
    AOT::stableSort(m_namesNotListed, { }, [&](const NameNotListed& name) {
        return std::pair { name.layout.index(), m_placesOfFirstStoresOfKeys[name.key.index()] };
    });

    uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
    m_tables.firstAppendedFieldOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    m_appendedKeysThatDescriptorsDefine.ensureSize(m_namesNotListed.size());
    m_appendedKeysThatDescriptorsAloneWrite.ensureSize(m_namesNotListed.size());
    Vector<uint32_t, 64> placesByWorth;
    Vector<uint32_t, 64> placesKept;
    size_t next = 0;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        m_tables.firstAppendedFieldOfLayout.append(static_cast<uint32_t>(m_tables.appendedKeys.size()));
        size_t first = next;
        while (next < m_namesNotListed.size() && m_namesNotListed[next].layout.index() == index)
            ++next;
        std::span<const NameNotListed> names = m_namesNotListed.span().subspan(first, next - first);
        auto append = [&](const NameNotListed& name) {
            if (name.isRequiredBySomeWrittenType)
                m_tables.appendedFieldsThatSomeWrittenTypeRequires.set(m_tables.appendedKeys.size());
            if (name.isRequiredByTypeOfThis())
                m_tables.appendedFieldsThatTypeOfThisRequires.set(m_tables.appendedKeys.size());
            if (name.isLaidLate && !name.isGivenBySecondConstructor)
                m_tables.appendedFieldsLaidLate.set(m_tables.appendedKeys.size());
            if (name.isStoredByFollowerOfMaker)
                m_tables.appendedFieldsThatFollowersOfMakersStore.set(m_tables.appendedKeys.size());
            if (name.isDefinedByDescriptors)
                m_appendedKeysThatDescriptorsDefine.set(m_tables.appendedKeys.size());
            if (name.isDefinedByDescriptors && !name.isDefinedInOtherWays && !name.isWrittenWithoutDefinition)
                m_appendedKeysThatDescriptorsAloneWrite.set(m_tables.appendedKeys.size());
            m_tables.appendedKeys.append(name.key);
            m_formsOfAppendedKeys.append(name.formToPlanFor());
        };
        size_t numberListed = m_program.layouts[index].fields.size();
        size_t roomInKeyOrder = maxFieldsInOrder - std::min<size_t>(maxFieldsInOrder, numberListed);
        size_t numberOutOfLine = names.size() - std::ranges::count_if(names, &NameNotListed::isRequiredByTypeOfThis);
        if (names.size() <= roomInKeyOrder && numberOutOfLine <= maxAppendedFieldsOutOfLine) {
            for (const NameNotListed& name : names)
                append(name);
            continue;
        }

        placesByWorth.shrink(0);
        for (uint32_t place = 0; place < names.size(); ++place)
            placesByWorth.append(place);
        AOT::stableSort(placesByWorth, std::ranges::greater { }, [&](uint32_t place) {
            return names[place].isRequiredByTypeOfThis();
        });
        placesKept.shrink(0);
        numberOutOfLine = 0;
        for (uint32_t place : placesByWorth) {
            if (placesKept.size() == roomInKeyOrder)
                break;
            if (!names[place].isRequiredByTypeOfThis() && numberOutOfLine++ >= maxAppendedFieldsOutOfLine)
                continue;
            placesKept.append(place);
        }
        std::ranges::sort(placesKept);
        for (uint32_t place : placesKept)
            append(names[place]);
        m_changes[index].add(LayoutChange::GainsProperty);
        for (uint32_t place = 0; place < names.size(); ++place) {
            if (!std::ranges::binary_search(placesKept, place))
                m_tables.namesStoredBesideFields.append({ LayoutID(index), names[place].key });
        }
    }
    std::ranges::sort(m_tables.namesStoredBesideFields);
    m_tables.namesStoredBesideFields.shrink(m_tables.namesStoredBesideFields.size() - std::ranges::unique(m_tables.namesStoredBesideFields).size());
    m_tables.firstAppendedFieldOfLayout.append(static_cast<uint32_t>(m_tables.appendedKeys.size()));
    for (uint32_t index = 0; index + 1 < m_tables.firstAppendedFieldOfLayout.size(); ++index) {
        for (uint32_t place = m_tables.firstAppendedFieldOfLayout[index]; place < m_tables.firstAppendedFieldOfLayout[index + 1]; ++place) {
            if (m_tables.appendedFieldsThatTypeOfThisRequires.get(place))
                continue;
            m_tables.appendedFieldsThatTakeWordLeftInCell.set(place);
            break;
        }
    }
    m_tables.appendedFields.fill(LinkedCell { }, m_tables.appendedKeys.size());
    for (size_t place = 0; place < m_formsOfAppendedKeys.size(); ++place)
        m_tables.appendedFields[place].formThatDefinitionsCreate = m_formsOfAppendedKeys[place];
}

void WriterOfLinkLayouts::writeRunsOfMakers()
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
    HashMap<uint32_t, Vector<LayoutID, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> layoutsWithAppendedKey;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        bool mayBeGivenUnknown = m_flow.lostLayouts.get(index) || hasEscaped(index);
        for (PropertyKeyID key : m_tables.appendedKeysOf(LayoutID(index))) {
            Vector<LayoutID, 2>& layouts = layoutsWithAppendedKey.add(key.bits(), Vector<LayoutID, 2> { }).iterator->value;
            if (mayBeGivenUnknown)
                layouts.append(LayoutID(index));
        }
    }
    if (layoutsWithAppendedKey.isEmpty())
        return;

    struct RunOfLayout {
        LayoutID layout;
        uint32_t firstKey { 0 };
        uint32_t numberOfKeys { 0 };
    };
    Vector<RunOfLayout> runs;
    Vector<PropertyKeyID> keys;
    Vector<const RunOfMaker*, 8> makers;
    Vector<PropertyKeyID, 32> keysOfMakers;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> keysOfMakersListed;
    bool leavesOutWhatSomeWaysStore = false;
    auto keysTakenOf = [&](const RunOfMaker& maker) {
        return leavesOutWhatSomeWaysStore && !maker.keysStoredOnEveryWayToLastStore.isEmpty() ? maker.keysStoredOnEveryWayToLastStore.span() : maker.keys.span();
    };
    auto keysOfAllMakersOf = [&](const RunOfMaker& last) -> std::span<const PropertyKeyID> {
        makers.shrink(0);
        for (const RunOfMaker* maker = &last; maker && !makers.contains(maker);) {
            makers.append(maker);
            uint32_t placePlusOne = maker->givenBy && !m_runsOnWhatFunctionsReturn.isEmpty() ? m_runsOnWhatFunctionsReturn[maker->givenBy.index()] : 0;
            maker = placePlusOne ? &m_runsOfMakers[placePlusOne - 1] : nullptr;
        }
        if (makers.size() == 1)
            return keysTakenOf(last);
        keysOfMakers.shrink(0);
        keysOfMakersListed.clear();
        for (const RunOfMaker* maker : makers | std::views::reverse) {
            for (PropertyKeyID key : keysTakenOf(*maker)) {
                if (keysOfMakersListed.add(key.bits()).isNewEntry)
                    keysOfMakers.append(key);
            }
        }
        return keysOfMakers.span();
    };
    auto noteRunOf = [&](LayoutID layout, const RunOfMaker& run, bool isKnownToBeGiven) {
        std::span<const PropertyKeyID> appendedKeys = m_tables.appendedKeysOf(layout);
        uint32_t firstKey = static_cast<uint32_t>(keys.size());
        bool everyKeyHasField = true;
        for (PropertyKeyID key : keysOfAllMakersOf(run)) {
            if (findField(m_program, layout, key))
                continue;
            if (std::ranges::find(appendedKeys, key) == appendedKeys.end()) {
                everyKeyHasField = false;
                break;
            }
            keys.append(key);
        }
        uint32_t numberOfKeys = static_cast<uint32_t>(keys.size()) - firstKey;
        if (!numberOfKeys || !(isKnownToBeGiven || everyKeyHasField || numberOfKeys >= 2)) {
            keys.shrink(firstKey);
            return;
        }
        runs.append({ layout, firstKey, numberOfKeys });
    };
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> numbersOfLists;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> listsNoted;
    auto noteRunOnLayoutsThatUnknownObjectsMayBe = [&](std::span<const PropertyKeyID> keysOfRun, const RunOfMaker& run) {
        for (PropertyKeyID key : keysOfRun) {
            auto found = layoutsWithAppendedKey.find(key.bits());
            if (found == layoutsWithAppendedKey.end())
                continue;
            uint32_t list = 0;
            for (PropertyKeyID listed : keysOfAllMakersOf(run))
                list = numbersOfLists.add(static_cast<uint64_t>(list) << 32 | listed.bits(), static_cast<uint32_t>(numbersOfLists.size() + 1)).iterator->value;
            if (listsNoted.add(static_cast<uint64_t>(key.bits()) << 32 | list).isNewEntry) {
                for (LayoutID layout : found->value)
                    noteRunOf(layout, run, false);
            }
            return;
        }
    };
    for (const RunOfMaker& run : m_runsOfMakers) {
        if (!run.layouts.isEmpty()) {
            for (LayoutID layout : run.layouts)
                noteRunOf(layout, run, run.layouts.size() == 1);
            continue;
        }
        noteRunOnLayoutsThatUnknownObjectsMayBe(run.keys.span(), run);
    }
    leavesOutWhatSomeWaysStore = true;
    for (const RunOfMaker& run : m_runsOfMakers) {
        if (!leavesOutWhatSomeWaysStore || run.keysStoredOnEveryWayToLastStore.isEmpty() || run.keysStoredOnEveryWayToLastStore.size() == run.keys.size())
            continue;
        for (LayoutID layout : run.layouts)
            noteRunOf(layout, run, run.layouts.size() == 1);
        if (run.layouts.isEmpty())
            noteRunOnLayoutsThatUnknownObjectsMayBe(run.keysStoredOnEveryWayToLastStore.span(), run);
    }
    leavesOutWhatSomeWaysStore = false;

    Vector<const RunOnObjectInField*, 8> runsOnField;
    for (const RunOnObjectInField& last : m_runsOnObjectsInFields) {
        runsOnField.shrink(0);
        for (const RunOnObjectInField* run = &last; run && !runsOnField.contains(run);) {
            runsOnField.append(run);
            uint32_t placePlusOne = run->givenBy ? m_runsOnFieldsOfWhatFunctionsReturn.get(static_cast<uint64_t>(run->givenBy.index() + 1) << 32 | run->keyInHolder.bits()) : 0;
            run = placePlusOne ? &m_runsOnObjectsInFields[placePlusOne - 1] : nullptr;
        }
        RunOfMaker whole;
        std::span<const LayoutID> layouts;
        for (const RunOnObjectInField* run : runsOnField | std::views::reverse) {
            if (layouts.empty())
                layouts = run->layouts.span();
            for (PropertyKeyID key : run->keys) {
                if (!whole.keys.contains(key))
                    whole.keys.append(key);
            }
        }
        if (!layouts.empty()) {
            for (LayoutID layout : layouts)
                noteRunOf(layout, whole, layouts.size() == 1);
            continue;
        }
        noteRunOnLayoutsThatUnknownObjectsMayBe(last.keys.span(), whole);
    }

    AOT::stableSort(runs, { }, &RunOfLayout::layout);
    m_tables.firstRunOfMakersOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    size_t next = 0;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        m_tables.firstRunOfMakersOfLayout.append(static_cast<uint32_t>(m_tables.firstKeyOfRunOfMakers.size()));
        for (; next < runs.size() && runs[next].layout.index() == index; ++next) {
            m_tables.firstKeyOfRunOfMakers.append(static_cast<uint32_t>(m_tables.keysOfRunsOfMakers.size()));
            m_tables.keysOfRunsOfMakers.append(keys.span().subspan(runs[next].firstKey, runs[next].numberOfKeys));
        }
    }
    m_tables.firstRunOfMakersOfLayout.append(static_cast<uint32_t>(m_tables.firstKeyOfRunOfMakers.size()));
    m_tables.firstKeyOfRunOfMakers.append(static_cast<uint32_t>(m_tables.keysOfRunsOfMakers.size()));
}

void WriterOfLinkLayouts::writeRunsBehindTaggedMakers()
{
    if (m_flow.taggedMakers.isEmpty())
        return;

    struct WaysThroughBody {
        Vector<uint32_t, 2> returned;
        Vector<std::pair<uint32_t, uint32_t>, 2> intoJoins;
    };
    HashMap<uint32_t, WaysThroughBody, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> waysThroughBodies;
    auto waysThrough = [&](FunctionID id) -> const WaysThroughBody& {
        auto added = waysThroughBodies.add(id.index(), WaysThroughBody { });
        if (added.isNewEntry) {
            const ThinSummary& summary = m_flow.summaries.at(id);
            for (const SummaryEdge& edge : summary.edges) {
                if (summary.nodes[edge.to].kind == SummaryNode::Kind::Result)
                    added.iterator->value.returned.append(edge.from);
                else if (summary.nodes[edge.to].kind == SummaryNode::Kind::Join)
                    added.iterator->value.intoJoins.append({ edge.to, edge.from });
            }
            std::ranges::sort(added.iterator->value.intoJoins);
        }
        return added.iterator->value;
    };

    static constexpr size_t mostNodesFollowed = 64;
    Vector<std::pair<uint32_t, uint32_t>, 16> nodesFollowed;
    Vector<uint32_t, 8> nodesToFollow;
    Vector<LayoutID, 2> makers;
    auto follow = [&](auto& self, FunctionID id, uint32_t node) -> void {
        if (nodesFollowed.size() >= mostNodesFollowed || nodesFollowed.contains(std::pair { id.index(), node }))
            return;
        nodesFollowed.append({ id.index(), node });
        const ThinSummary& summary = m_flow.summaries.at(id);
        const SummaryNode& record = summary.nodes[node];
        if (record.kind == SummaryNode::Kind::Join) {
            size_t first = nodesToFollow.size();
            for (auto [join, from] : std::ranges::equal_range(waysThrough(id).intoJoins, node, { }, &std::pair<uint32_t, uint32_t>::first))
                nodesToFollow.append(from);
            for (size_t next = first; next < nodesToFollow.size(); ++next)
                self(self, id, nodesToFollow[next]);
            nodesToFollow.shrink(first);
            return;
        }
        if (record.kind != SummaryNode::Kind::ResultOfCall)
            return;
        const SummaryCall& call = summary.calls[record.first];
        auto followCallee = [&](FunctionID callee) {
            if (call.isConstruction) {
                if (const TaggedMaker* maker = findTaggedMaker(m_flow.taggedMakers.span(), callee); maker && !makers.contains(m_program.taggedFamilies[maker->family].maker))
                    makers.append(m_program.taggedFamilies[maker->family].maker);
                return;
            }
            const ThinSummary& inner = m_flow.summaries.at(callee);
            size_t first = nodesToFollow.size();
            nodesToFollow.appendVector(waysThrough(callee).returned);
            for (size_t next = first; next < nodesToFollow.size(); ++next) {
                const SummaryNode& returned = inner.nodes[nodesToFollow[next]];
                if (returned.kind != SummaryNode::Kind::Parameter) {
                    self(self, callee, nodesToFollow[next]);
                    continue;
                }
                uint32_t firstParameterPassed = call.callee == SummaryCall::Callee::Binding ? 0 : m_program.at(callee).firstParameterIndex();
                if (!call.passesList && returned.first >= firstParameterPassed && returned.first - firstParameterPassed < call.numberOfArguments)
                    self(self, id, summary.arguments[call.firstArgument + returned.first - firstParameterPassed]);
            }
            nodesToFollow.shrink(first);
        };
        const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[id.index()] + record.first];
        for (FunctionID callee : callees.listed())
            followCallee(callee);
        for (uint32_t shared : callees.shared()) {
            for (FunctionID callee : m_flow.calleesOfSharedCallsAsSettled()[shared].listed())
                followCallee(callee);
        }
        if (call.callee == SummaryCall::Callee::Name && call.key && callees.listed().isEmpty() && callees.shared().isEmpty()) {
            if (auto found = m_functionsLaidIntoFieldsUnderKey.find(call.key.bits()); found != m_functionsLaidIntoFieldsUnderKey.end()) {
                for (FunctionID callee : found->value)
                    followCallee(callee);
            }
        }
    };

    Vector<const RunOfMaker*, 8> row;
    for (const RunOfMaker& last : m_runsOfMakers) {
        if (last.keys.isEmpty() || !last.firstStore)
            continue;
        nodesFollowed.shrink(0);
        makers.shrink(0);
        follow(follow, last.body, last.subject);
        if (makers.isEmpty())
            continue;

        row.shrink(0);
        for (const RunOfMaker* run = &last; run && !row.contains(run);) {
            row.append(run);
            uint32_t placePlusOne = run->givenBy && !m_runsOnWhatFunctionsReturn.isEmpty() ? m_runsOnWhatFunctionsReturn[run->givenBy.index()] : 0;
            run = placePlusOne ? &m_runsOfMakers[placePlusOne - 1] : nullptr;
        }
        uint32_t firstKey = static_cast<uint32_t>(m_tables.keysOfRunsBehindTaggedMakers.size());
        for (const RunOfMaker* run : row | std::views::reverse) {
            for (PropertyKeyID key : run->keys) {
                std::span<const PropertyKeyID> keysOfRow = m_tables.keysOfRunsBehindTaggedMakers.span().subspan(firstKey);
                if (std::ranges::find(keysOfRow, key) == keysOfRow.end())
                    m_tables.keysOfRunsBehindTaggedMakers.append(key);
            }
        }
        uint32_t firstMaker = static_cast<uint32_t>(m_tables.makersOfRunsBehindTaggedMakers.size());
        m_tables.makersOfRunsBehindTaggedMakers.appendVector(makers);
        m_tables.runsBehindTaggedMakers.append({ last.body, last.firstStore, firstKey, static_cast<uint32_t>(m_tables.keysOfRunsBehindTaggedMakers.size()) - firstKey, firstMaker, static_cast<uint32_t>(makers.size()) });
    }
}

void WriterOfLinkLayouts::writeAppendedFields()
{
    for (uint32_t index = 0; index < m_tables.layouts.size(); ++index) {
        LayoutID id(index);
        bool isExpectedOfCreatedObjects = m_flow.reading() != WhatUnseenCodeDoes::Anything && m_program.layouts[index].has(LayoutFlags::IsOfCreatedObject);
        bool mayGainWhatNobodyNames = m_tables.layouts[index].possibleChanges.contains(LayoutChange::GainsProperty) && !m_gainsOnlyWhatWrittenDefinitionsLay.quickGet(index);
        if (!isExpectedOfCreatedObjects && (hasEscaped(index) || mayGainWhatNobodyNames))
            continue;
        std::span<const PropertyKeyID> keys = m_tables.appendedKeysOf(id);
        for (uint32_t place = 0; place < keys.size(); ++place) {
            FlowNodeID node = m_flow.findAppendedField(id, keys[place]);
            if (FlowNodeID stored = m_flow.reading() != WhatUnseenCodeDoes::Anything ? m_flow.findAppendedStoreNode(id, keys[place]) : FlowNodeID())
                node = stored;
            if (!node || m_namesStoredThroughWhatIsExpected.contains(static_cast<uint64_t>(index) << 32 | keys[place].bits()))
                continue;
            LinkedCell& cell = m_tables.appendedFields[m_tables.firstAppendedFieldOfLayout[index] + place];
            cell.everyWriterIsKnown = true;
            cell.liesBesideWhatDescriptorsDefine = m_isRedefinedByWrittenDefinitionsAlone.quickGet(index) && !m_appendedKeysThatDescriptorsDefine.get(m_tables.firstAppendedFieldOfLayout[index] + place);
            cell.proven = m_flow.typeAt(node).isNever() ? TypeID(TypeTable::never) : claimedTypeAt(node, Numbers::ByKindAlone);
            cell.leavesOutWhatArrives = claimLeavesOutWhatArrivesAt(node);
            cell.keepsAccessorOnceLaid = cell.proven && !m_flow.lostLayouts.get(index) && m_flow.reading() == WhatUnseenCodeDoes::Anything && m_isRedefinedByWrittenDefinitionsAlone.quickGet(index)
                && m_appendedKeysThatDescriptorsAloneWrite.get(m_tables.firstAppendedFieldOfLayout[index] + place) && cell.formThatDefinitionsCreate.containsAll({ FieldForm::IsAccessor, FieldForm::IsNotConfigurable });
        }
    }
}

bool WriterOfLinkLayouts::comesToAccessorAbove(LayoutID layout, PropertyKeyID key) const
{
    if (m_flow.namesMayBeIncomplete(layout, ClassSide::Prototype))
        return false;
    LinkFlow::FoundMembers members = m_flow.findMembers(layout, ClassSide::Prototype, key);
    if (members.lookupGoesOnAboveClasses())
        return false;
    return std::ranges::any_of(members, [](const LinkFlow::FoundMember& member) {
        return member.kind != MethodKind::Method;
    });
}

ALWAYS_INLINE void WriterOfLinkLayouts::lookAtMembers(const ThinSummary& summary)
{
    for (const SummaryMember& member : summary.members) {
        LinkedMember linked { member.classLayout, member.side, member.kind, member.index, member.key, FunctionID(), !hasEscaped(member.classLayout.index()), FunctionID() };
        if (const SummaryNode& given = summary.nodes[member.value]; given.kind == SummaryNode::Kind::Closure)
            linked.written = FunctionID(given.first);
        if (linked.everyWriterIsKnown) {
            const LinkType& held = m_flow.typeAt(m_flow.member(member.classLayout, member.side, member.index));
            LinkType oneClosure;
            oneClosure.functions = LinkType::Functions::Closure;
            oneClosure.function = held.function;
            if (held == oneClosure)
                linked.function = held.function;
        }
        m_members.append(linked);
    }
}

void WriterOfLinkLayouts::noteLostClassObjects()
{
    auto note = [&](LayoutID classLayout) {
        m_liesBelowLostClassObject.quickSet(classLayout.index());
        for (LayoutID below : m_program.layoutsExpectedBelow(classLayout))
            m_liesBelowLostClassObject.quickSet(below.index());
    };
    for (size_t index : m_flow.lostFunctions) {
        if (LayoutID constructed = m_program.functions[index].constructedClass; constructed && !m_flow.isOfConstructorFunction(constructed))
            note(constructed);
    }
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        FunctionID constructor = m_program.layouts[index].constructor;
        if (constructor && m_flow.lostFunctions.get(constructor.index()) && !m_flow.isOfConstructorFunction(LayoutID(index)))
            note(LayoutID(index));
    }
}

bool WriterOfLinkLayouts::objectPrototypeMayGainName() const
{
    const BuiltinTable& builtins = m_program.builtins();
    BuiltinID objectPrototype = builtins.canonicalID(Builtins::ObjectPrototype);
    if (!builtins.at(objectPrototype).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames }) || !builtins.openNamesOf(objectPrototype).empty())
        return true;
    return m_program.isSomeNameOpenedOn(objectPrototype);
}

bool WriterOfLinkLayouts::mayBeAllocated(LayoutID id) const
{
    const Layout& layout = m_program.at(id);
    if (m_flow.allocatedLayouts.get(id.index()) || m_isMadeAtNoNode.quickGet(id.index()) || layout.isDerived())
        return true;
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Environment:
        return false;
    case LayoutKind::Instance:
        if (!layout.constructor || !m_flow.reached.get(layout.constructor.index()))
            return false;
        return m_program.at(layout.constructor).kind != FunctionKind::DerivedConstructor || !everyCallerIsKnown(layout.constructor);
    case LayoutKind::Container:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void WriterOfLinkLayouts::writeLayouts()
{
    bool everyPrototypeMayChange = objectPrototypeMayGainName();
    BitVector haveConstructorFunctionInRow = answerAlongRows(m_tables.layouts.size(), true, [&](LayoutID id) -> StepAlongRow {
        const Layout& layout = m_program.at(id);
        if (layout.has(LayoutFlags::IsConstructorFunction))
            return { true, { } };
        if (!layout.base)
            return { false, { } };
        return { std::nullopt, layout.base };
    });
    for (uint32_t index = 0; index < m_tables.layouts.size(); ++index) {
        LayoutID id(index);
        LinkedLayout& linked = m_tables.layouts[index];
        linked.hasConstructorFunctionInRow = haveConstructorFunctionInRow.quickGet(index);
        linked.family = m_flow.familiesBehindFlow->nearestCommonFamily(id, id);
        linked.isAllocated = mayBeAllocated(id);
        linked.possibleChanges = LinkedLayout { }.possibleChanges;
        linked.listsEveryMemberAbove = !m_flow.namesMayBeIncomplete(id, ClassSide::Prototype);
        linked.listsEveryStaticMember = !m_flow.namesMayBeIncomplete(id, ClassSide::Static);
        linked.isFilledUnderComputedKeys = m_isFilledUnderComputedKeys.quickGet(index);
        linked.instancesAreLaidAsPrototypes = m_flow.layoutsWhoseInstancesAreLaidAsPrototypes.quickGet(index);
        linked.heritage = m_heritages[index];
        if (m_asksWhatMakersGive && !m_layoutsMadeInSeveralFunctions.quickGet(index))
            linked.onlyFunctionThatMakesObjects = m_onlyFunctionsThatMakeObjects[index];
        if (hasEscaped(index) || !isFoundByShapeTest(m_program.at(id)))
            continue;

        if (m_flow.reading() == WhatUnseenCodeDoes::Anything)
            linked.reachesUnseenCode = false;
        linked.possibleChanges = m_changes[index];
        linked.possibleChanges.add(m_flow.possibleChangesOf(index));
        if (writtenBaseOf(m_program.at(id)) && m_program.at(id).inheritedFields != InheritedFields::AllOfRow)
            linked.possibleChanges.add(LayoutChange::GainsProperty);
        bool gainsBesideWhatDefinitionsLay = linked.possibleChanges.contains(LayoutChange::GainsProperty) || (m_flow.lostLayouts.get(index) && m_changesOfLostFamilies[lostFamilyOf(id)].contains(LayoutChange::GainsProperty));
        if (!gainsBesideWhatDefinitionsLay && m_flow.layoutsDefinedOnUnderWrittenKeys.get(index) && !m_flow.layoutsDefinedOnUnderKeysThatAreNotWritten.get(index))
            m_gainsOnlyWhatWrittenDefinitionsLay.quickSet(index);
        bool isRedefinedBesideWrittenDefinitions = linked.possibleChanges.contains(LayoutChange::GetsAccessor) || (m_flow.lostLayouts.get(index) && m_changesOfLostFamilies[lostFamilyOf(id)].contains(LayoutChange::GetsAccessor));
        if (!isRedefinedBesideWrittenDefinitions && m_program.layouts[index].kind == LayoutKind::Record && m_flow.layoutsDefinedOnUnderWrittenKeys.get(index) && !m_flow.layoutsDefinedOnUnderKeysThatAreNotWritten.get(index))
            m_isRedefinedByWrittenDefinitionsAlone.quickSet(index);
        linked.isDefinedOnByDescriptorsThatLayValuesAlone = m_flow.reading() != WhatUnseenCodeDoes::Anything && m_flow.layoutsDefinedOnUnderWrittenKeys.get(index) && !m_flow.layoutsGivenAccessorsByDescriptorsInSight.get(index);
        if (linked.isDefinedOnByDescriptorsThatLayValuesAlone)
            linked.possibleChanges.add(changesMadeByInLinkLayouts(SummaryChange::Kind::DefinesByDescriptor) - LayoutChange::GetsAccessor);
        else if (m_flow.layoutsDefinedOnUnderWrittenKeys.get(index))
            linked.possibleChanges.add(changesMadeByInLinkLayouts(SummaryChange::Kind::DefinesByDescriptor));
        linked.isRedefinedByLiteralsAlone = m_flow.layoutsWithKnownAccessors.get(index) && !linked.possibleChanges.contains(LayoutChange::GetsAccessor)
            && !(m_flow.lostLayouts.get(index) && m_changesOfLostFamilies[lostFamilyOf(id)].contains(LayoutChange::GetsAccessor));
        if (m_flow.layoutsWithKnownAccessors.get(index))
            linked.possibleChanges.add(LayoutChange::GetsAccessor);
        if (m_flow.lostLayouts.get(index) && m_flow.reading() == WhatUnseenCodeDoes::Anything)
            linked.possibleChanges.add(m_changesOfLostFamilies[lostFamilyOf(id)]);
        if (everyPrototypeMayChange)
            linked.possibleChanges.add(LayoutChange::ChangesPrototype);
        if (linked.possibleChanges.contains(LayoutChange::ChangesPrototype))
            linked.possibleChanges.add(LayoutChange::GetsObjectAbove);
        if (m_flow.layoutsThatGetNullAbove.get(index))
            linked.possibleChanges.add(LayoutChange::ChangesPrototype);
    }
}

void LinkFlow::forEachNodeAnsweredBesideObjectOfConstruction(FunctionID constructor, NOESCAPE const ScopedLambda<void(uint32_t nodeInSummary)>& functor) const
{
    const Function& function = program.at(constructor);
    const ThinSummary& summary = summaries.at(constructor);
    BitVector isSeen(summary.nodes.size());
    Vector<uint32_t, 8> toVisit;
    for (uint32_t node = 0; node < summary.nodes.size(); ++node) {
        if (summary.nodes[node].kind == SummaryNode::Kind::Result && isSeen.set(node) == false)
            toVisit.append(node);
    }
    while (!toVisit.isEmpty()) {
        uint32_t to = toVisit.takeLast();
        bool isEnteredInSummary = false;
        for (const SummaryEdge& edge : summary.edges) {
            if (edge.to != to)
                continue;
            isEnteredInSummary = true;
            if (isSeen.set(edge.from))
                continue;
            const SummaryNode& from = summary.nodes[edge.from];
            switch (from.kind) {
            case SummaryNode::Kind::Join:
                toVisit.append(edge.from);
                break;
            case SummaryNode::Kind::Constant:
                if (program.at(TypeID(from.first)).hasObjects())
                    functor(edge.from);
                break;
            case SummaryNode::Kind::Allocation:
                if (function.kind != FunctionKind::BaseConstructor || !function.constructedClass || !standsAtOrBelow(LayoutID(from.first), function.constructedClass))
                    functor(edge.from);
                break;
            case SummaryNode::Kind::Parameter:
                if (!function.takesThisFromConstruction() || function.thisParameterIndex() != std::optional<unsigned>(from.first))
                    functor(edge.from);
                break;
            case SummaryNode::Kind::ResultOfCall:
                if (function.kind != FunctionKind::DerivedConstructor || !summary.calls[from.first].isConstruction || summary.calls[from.first].newTarget == noSummaryNode)
                    functor(edge.from);
                break;
            default:
                functor(edge.from);
                break;
            }
        }
        if (!isEnteredInSummary && summary.nodes[to].kind == SummaryNode::Kind::Join)
            functor(to);
    }
}

bool LinkFlow::answersOnlyWithObjectOfItsConstruction(FunctionID constructor) const
{
    if (!program.at(constructor).hasBody() || summaries.at(constructor).has(SummaryTrait::HasDirectEval))
        return false;
    bool answersWithAnother = false;
    forEachNodeAnsweredBesideObjectOfConstruction(constructor, scopedLambda<void(uint32_t)>([&](uint32_t) {
        answersWithAnother = true;
    }));
    return !answersWithAnother;
}

void LinkFlow::findSuperCallsThatMayAnswerWithOlderObject()
{
    numberTreeOfBases();
    size_t numberOfLayouts = program.layouts.size();
    m_layoutsWhoseConstructionMayAnswerWithOlderObject = answerAlongRows(numberOfLayouts, true, [&](LayoutID above) -> StepAlongRow {
        const Layout& record = program.at(above);
        if (record.kind != LayoutKind::Instance || !record.constructor || !answersOnlyWithObjectOfItsConstruction(record.constructor))
            return { true, { } };
        if (!record.base)
            return { record.has(LayoutFlags::HasHeritage), { } };
        return { std::nullopt, record.base };
    });
    m_layoutsWhoseSuperCallMayAnswerWithOlderObject.ensureSize(numberOfLayouts);
    for (size_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || (!layout.base && !layout.has(LayoutFlags::HasHeritage)))
            continue;
        if (!layout.base || m_layoutsWhoseConstructionMayAnswerWithOlderObject.quickGet(layout.base.index()))
            m_layoutsWhoseSuperCallMayAnswerWithOlderObject.quickSet(index);
    }
}

void LinkFlow::numberTreeOfBases()
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    Vector<uint32_t> firstBelow(FillWith { }, numberOfLayouts + 1, 0u);
    for (const Layout& layout : program.layouts) {
        if (layout.base)
            ++firstBelow[layout.base.index() + 1];
    }
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        firstBelow[index + 1] += firstBelow[index];
    Vector<uint32_t> below(FillWith { }, firstBelow[numberOfLayouts], 0u);
    Vector<uint32_t> nextBelow(firstBelow.span().first(numberOfLayouts));
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (LayoutID base = program.layouts[index].base)
            below[nextBelow[base.index()]++] = index;
    }

    m_stretchesAtAndBelowLayouts.grow(numberOfLayouts);
    m_topsOfRows.grow(numberOfLayouts);
    nextBelow.shrink(0);
    nextBelow.append(firstBelow.span().first(numberOfLayouts));
    uint32_t next = 0;
    Vector<uint32_t, 32> open;
    for (uint32_t root = 0; root < numberOfLayouts; ++root) {
        if (program.layouts[root].base)
            continue;
        m_stretchesAtAndBelowLayouts[root].first = next++;
        m_topsOfRows[root] = LayoutID(root);
        open.append(root);
        while (!open.isEmpty()) {
            uint32_t current = open.last();
            if (nextBelow[current] == firstBelow[current + 1]) {
                m_stretchesAtAndBelowLayouts[current].end = next;
                open.removeLast();
                continue;
            }
            uint32_t entered = below[nextBelow[current]++];
            m_stretchesAtAndBelowLayouts[entered].first = next++;
            m_topsOfRows[entered] = LayoutID(root);
            open.append(entered);
        }
    }

    m_stretchesOfFamilies.grow(families.size());
    auto widen = [](StretchInTreeOfBases& stretch, const StretchInTreeOfBases& held) {
        stretch.first = std::min(stretch.first, held.first);
        stretch.end = std::max(stretch.end, held.end);
    };
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (FamilyID nearest = families.nearestFamilyOf(LayoutID(index)))
            widen(m_stretchesOfFamilies[nearest.index()], { m_stretchesAtAndBelowLayouts[index].first, m_stretchesAtAndBelowLayouts[index].first + 1 });
    }
    for (uint32_t family = 0; family < families.size(); ++family) {
        if (FamilyID above = families.nearestFamilyAbove(FamilyID(family)))
            widen(m_stretchesOfFamilies[above.index()], m_stretchesOfFamilies[family]);
    }
}

bool LinkFlow::mayStoreIntoOlderObjectThatSuperCallAnswered(FunctionID writer, const SummaryAccess& access) const
{
    const Function& function = program.at(writer);
    return access.isPartOfConstruction && function.kind == FunctionKind::DerivedConstructor && superCallMayAnswerWithOlderObject(function.constructedClass)
        && summaries.at(writer).nodes[access.subject].kind != SummaryNode::Kind::Allocation;
}

bool LinkFlow::isPartOfConstructionOf(LayoutID layout, FunctionID writer, const SummaryAccess& access) const
{
    if (!mayStoreIntoOlderObjectThatSuperCallAnswered(writer, access))
        return access.isPartOfConstruction;
    return standsAtOrBelow(layout, program.at(writer).constructedClass);
}

bool LinkFlow::isPartOfConstructionOfAllOf(FamilyID family, FunctionID writer, const SummaryAccess& access) const
{
    if (!mayStoreIntoOlderObjectThatSuperCallAnswered(writer, access))
        return access.isPartOfConstruction;
    return everyMemberStandsAtOrBelow(family, program.at(writer).constructedClass);
}

void WriterOfLinkLayouts::gatherAtDeclarationsOfFields()
{
    Vector<SetBitsInOrderForLinkLayouts, 9> noted;
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_tables.layouts.size());
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& layout = m_program.layouts[index];
        RELEASE_ASSERT(m_tables.firstFieldOfLayout[index + 1] - m_tables.firstFieldOfLayout[index] == layout.fields.size());
        bool hasUnseenWriters = fieldsHaveUnseenWriters(index);
        bool isLost = m_flow.lostLayouts.get(index);
        bool isStoredIntoWhenOlder = m_flow.superCallMayAnswerWithOlderObject(LayoutID(index));
        auto gather = [&](uint32_t field) {
            FieldReference reference { LayoutID(index), field };
            uint32_t cell = cellOf(reference);
            uint32_t declaration = cellOf(declarationOf(m_program, reference));
            if ((hasUnseenWriters && !isPrivateNameInLink(m_program, layout.fields[field].key)) || m_fieldsWithUnseenWriters.quickGet(cell))
                m_fieldsWithUnseenWriters.quickSet(declaration);
            bool isRoomForPrivateNameOfParent = isPrivateNameInLink(m_program, layout.fields[field].key) && !m_flow.standsAtOrBelow(reference.layout, declarationOf(m_program, reference).layout);
            if ((isStoredIntoWhenOlder && !isRoomForPrivateNameOfParent) || m_fieldsWrittenLate.quickGet(cell) || m_flow.isWrittenAfterConstruction(cell) || (isLost && isWrittenLateThroughLost(LayoutID(index), layout.fields[field].key)))
                m_fieldsWrittenLate.quickSet(declaration);
            if (cell != declaration && m_fieldsGivenUndefinedByMakers.quickGet(cell))
                m_fieldsGivenUndefinedByMakers.quickSet(declaration);
            if (cell != declaration && m_fieldsFilledByConstructors.quickGet(cell))
                m_fieldsFilledByConstructors.quickSet(declaration);
            for (BitVector* given : { &m_fieldsGivenUndefinedOrNullLate, &m_fieldsGivenUndefinedOrNullByMakers, &m_fieldsGivenValuesByMakers }) {
                if (cell != declaration && given->quickGet(cell))
                    given->quickSet(declaration);
            }
        };
        auto numberOfFields = static_cast<uint32_t>(layout.fields.size());
        uint32_t numberAskedOneByOne = numberOfFields;
        if (LayoutID base = baseWhoseFieldListIsCopiedWhole(m_program, LayoutID(index)); base && !isLost && (!hasUnseenWriters || fieldsHaveUnseenWriters(base.index())) && (!isStoredIntoWhenOlder || m_flow.superCallMayAnswerWithOlderObject(base)))
            numberAskedOneByOne = layout.ownFieldCount;
        for (uint32_t field = 0; field < numberAskedOneByOne; ++field)
            gather(field);
        if (numberAskedOneByOne == numberOfFields)
            continue;
        size_t firstCell = m_tables.firstFieldOfLayout[index];
        if (noted.isEmpty()) {
            for (const BitVector* set : { &std::as_const(m_fieldsWithUnseenWriters), &std::as_const(m_fieldsWrittenLate), &m_flow.fieldsWrittenAfterConstruction, &m_flow.fieldsThatUnseenCodeMayWrite, &std::as_const(m_fieldsGivenUndefinedByMakers), &std::as_const(m_fieldsFilledByConstructors), &std::as_const(m_fieldsGivenUndefinedOrNullLate), &std::as_const(m_fieldsGivenUndefinedOrNullByMakers), &std::as_const(m_fieldsGivenValuesByMakers) })
                noted.append(SetBitsInOrderForLinkLayouts(*set));
        }
        for (const SetBitsInOrderForLinkLayouts& set : noted) {
            for (uint32_t cell : set.within(firstCell + numberAskedOneByOne, numberOfFields - numberAskedOneByOne))
                gather(static_cast<uint32_t>(cell - firstCell));
        }
    }
}

TypeID WriterOfLinkLayouts::namingSymbolAt(FlowNodeID node, TypeID type)
{
    PropertyKeyID symbol = type ? m_flow.symbolAt(node) : PropertyKeyID();
    return symbol ? types().unionOf(types().withoutPrimitives(type, Primitive::AnySymbol), types().symbolLiteral(symbol)) : type;
}

void WriterOfLinkLayouts::writeFields()
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_tables.layouts.size());
    Vector<TypeID> provenAtDeclaration;
    provenAtDeclaration.fill(TypeID(), m_tables.fields.size());
    static constexpr uint8_t somethingArrives = 1;
    static constexpr uint8_t anythingArrives = 2;
    static constexpr uint8_t partIsLeftOut = 4;
    Vector<uint8_t> arrivesAtDeclaration;
    arrivesAtDeclaration.fill(0, m_tables.fields.size());

    writeOnAllThreads(numberOfLayouts, [&](unsigned index) {
        uint32_t numberOfFields = baseWhoseFieldListIsCopiedWhole(m_program, LayoutID(index)) ? m_program.layouts[index].ownFieldCount : m_tables.firstFieldOfLayout[index + 1] - m_tables.firstFieldOfLayout[index];
        if constexpr (ASSERT_ENABLED) {
            for (uint32_t field = numberOfFields; field < m_program.layouts[index].fields.size(); ++field)
                ASSERT(m_flow.field({ LayoutID(index), field }) == m_flow.field(declarationOf(m_program, { LayoutID(index), field })));
        }
        bool isOfProgram = true;
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            FieldReference reference { LayoutID(index), field };
            FieldReference declared = declarationOf(m_program, reference);
            FlowNodeID node = m_flow.field(reference);
            if (declared != reference) {
                RELEASE_ASSERT(node == m_flow.field(declared));
                continue;
            }
            if (m_flow.typeAt(node).isNever())
                continue;
            uint32_t cell = cellOf(reference);
            TypeID arrived = claimedTypeAt(node, m_program.layouts[index].isEnvironment() ? Numbers::AsHeld : Numbers::ByKindAlone);
            if (PropertyKeyID key = fieldAt(m_program, reference).key; key && arrived && m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls) {
                if (FlowNodeID laid = m_flow.findLaidByDescriptorsOn(LayoutID(index), key); laid && !m_flow.typeAt(laid).isNever()) {
                    TypeID byDescriptors = claimedTypeAt(laid);
                    arrived = byDescriptors ? types().unionOf(arrived, byDescriptors) : TypeID();
                }
            }
            if (m_program.layouts[index].isEnvironment())
                arrived = namingSymbolAt(node, arrived);
            bool saysWhatMakersLay = false;
            if (uint32_t laid = (arrived && !types().isUninformative(arrived)) || m_classesThatMakersLayInFields.isEmpty() ? 0 : m_classesThatMakersLayInFields[cell]; laid && laid != severalClasses) {
                TypeID made = laid & layoutAmongWhatMakersLay ? types().forLayout(LayoutID((laid & ~layoutAmongWhatMakersLay) - 1)) : types().builtinClass(BuiltinClassID(laid - 1));
                arrived = types().withPrimitives(made, Primitive::UndefinedOrNull);
                saysWhatMakersLay = true;
            }
            provenAtDeclaration[cell] = arrived;
            arrivesAtDeclaration[cell] = (arrived ? somethingArrives : somethingArrives | anythingArrives) | (claimLeavesOutWhatArrivesAt(node) || saysWhatMakersLay ? partIsLeftOut : 0);
            isOfProgram &= isOfProgramInLink(arrived);
        }
        return isOfProgram;
    });

    BitVector hasKnownAccessor;
    hasKnownAccessor.ensureSize(m_tables.fields.size());
    for (FieldReference field : m_flow.fieldsWithKnownAccessors)
        hasKnownAccessor.quickSet(cellOf(field));

    struct WhatLayoutSaysOfCells {
        bool everyConstructionIsSeen { false };
        bool keepsWhatConstructionsFill { false };
        bool keepsWhatLiesBesideAccessors { false };
        bool isRecordThatIsMade { false };
        bool isEnvironment { false };
        bool isRecord { false };
        bool constructionsAreFollowed { false };
        bool isAllocated { false };
        bool isLost { false };
        bool isGivenUndefinedByMakers { false };
        bool operator==(const WhatLayoutSaysOfCells&) const = default;
    };
    auto whatLayoutSays = [&](uint32_t index) {
        WhatLayoutSaysOfCells says;
        says.everyConstructionIsSeen = constructionsAreFollowedInLinkLayouts(m_program.layouts[index]) && !hasEscaped(index);
        says.keepsWhatConstructionsFill = says.everyConstructionIsSeen && !m_tables.layouts[index].possibleChanges.containsAny({ LayoutChange::LosesProperty, LayoutChange::GetsAccessor });
        says.keepsWhatLiesBesideAccessors = says.everyConstructionIsSeen && m_tables.layouts[index].isRedefinedByLiteralsAlone && !m_tables.layouts[index].possibleChanges.contains(LayoutChange::LosesProperty);
        says.isRecordThatIsMade = m_asksWhatMakersGive && m_program.layouts[index].kind == LayoutKind::Record && m_flow.allocatedLayouts.get(index) && !m_isMadeAtNoNode.quickGet(index);
        says.isEnvironment = m_program.layouts[index].isEnvironment();
        says.isRecord = m_program.layouts[index].kind == LayoutKind::Record;
        says.constructionsAreFollowed = constructionsAreFollowedInLinkLayouts(m_program.layouts[index]);
        says.isAllocated = m_flow.allocatedLayouts.get(index);
        says.isLost = m_flow.lostLayouts.get(index);
        says.isGivenUndefinedByMakers = m_layoutsGivenUndefinedByMakers.quickGet(index);
        return says;
    };

    BitVector takesCellsOfBase;
    takesCellsOfBase.ensureSize(numberOfLayouts);
    Vector<SetBitsInOrderForLinkLayouts, 5> noted;
    uint64_t numberOfCellsWrittenOneByOne = 0;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        numberOfCellsWrittenOneByOne += m_program.layouts[index].fields.size() - m_program.layouts[index].ownFieldCount;
        LayoutID base = baseWhoseFieldListIsCopiedWhole(m_program, LayoutID(index));
        if (!base)
            continue;
        WhatLayoutSaysOfCells says = whatLayoutSays(index);
        if (says.isLost || says.isRecord || says != whatLayoutSays(base.index()))
            continue;
        size_t first = cellOf({ LayoutID(index), m_program.layouts[index].ownFieldCount });
        size_t firstOfBase = cellOf({ base, 0 });
        size_t length = m_program.at(base).fields.size();
        if (noted.isEmpty()) {
            for (const BitVector* set : { &std::as_const(hasKnownAccessor), &m_flow.fieldsThatMayBeEmpty, &m_flow.fieldsNotOfBirth, &std::as_const(m_fieldsReadInFrontOfDefinition), &std::as_const(m_fieldsGivenValues) })
                noted.append(SetBitsInOrderForLinkLayouts(*set));
        }
        if (std::ranges::all_of(noted, [&](const SetBitsInOrderForLinkLayouts& set) {
            return set.holdsTheSameIn(first, firstOfBase, length);
        })) {
            takesCellsOfBase.quickSet(index);
            m_tables.layouts[index].takesCellsOfBase = true;
            numberOfCellsWrittenOneByOne -= length;
        }
    }
    m_flow.build.statistics().count(StatisticID::InheritedFieldEntriesWrittenIndividually, numberOfCellsWrittenOneByOne);

    writeOnAllThreads(numberOfLayouts, [&](unsigned index) {
        uint32_t numberOfFields = takesCellsOfBase.quickGet(index) ? m_program.layouts[index].ownFieldCount : m_tables.firstFieldOfLayout[index + 1] - m_tables.firstFieldOfLayout[index];
        auto [everyConstructionIsSeen, keepsWhatConstructionsFill, keepsWhatLiesBesideAccessors, isRecordThatIsMade, isEnvironment, isRecord, constructionsAreFollowed, isAllocated, isLost, isGivenUndefinedByMakers] = whatLayoutSays(index);
        bool isOfProgram = true;
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            FieldReference reference { LayoutID(index), field };
            uint32_t declaration = cellOf(declarationOf(m_program, reference));
            LinkedCell& cell = m_tables.fields[cellOf(reference)];
            cell.everyWriterIsKnown = !m_fieldsWithUnseenWriters.quickGet(declaration);
            cell.leavesOutWhatArrives = arrivesAtDeclaration[declaration] & partIsLeftOut;
            cell.isWrittenAfterConstruction = !cell.everyWriterIsKnown || m_fieldsWrittenLate.quickGet(declaration);
            cell.proven = cell.everyWriterIsKnown && !(arrivesAtDeclaration[declaration] & anythingArrives) ? provenAtDeclaration[declaration] : TypeID();
            if (cell.everyWriterIsKnown && !(arrivesAtDeclaration[declaration] & somethingArrives) && !isEnvironment)
                cell.proven = TypeTable::never;
            cell.proven = narrowerOf(cell.proven, fieldAt(m_program, reference).declared);
            cell.isTestedByValue = fieldAt(m_program, reference).key && m_keysOfTestedFields.get(fieldAt(m_program, reference).key.index());
            if (uint32_t unit = m_unitsListedForFields[cellOf(reference)]; unit && unit != otherThanOneUnit && m_flow.reading() == WhatUnseenCodeDoes::Anything && fieldAt(m_program, reference).key && m_keysOfTestedFields.get(fieldAt(m_program, reference).key.index()) && cellOf(reference) == declaration && cell.proven && !cell.isWrittenAfterConstruction && m_program.layouts[index].kind == LayoutKind::Record) {
                TypeID written = typeOfWrittenUnit(WrittenUnit::fromBits(unit));
                if (types().isSubset(written, cell.proven))
                    cell.proven = written;
            }
            const Field& record = fieldAt(m_program, reference);
            bool liesBesideAccessors = keepsWhatLiesBesideAccessors && !hasKnownAccessor.quickGet(cellOf(reference)) && !hasKnownAccessor.quickGet(declaration);
            cell.liesBesideWhatDescriptorsDefine = m_isRedefinedByWrittenDefinitionsAlone.quickGet(index) && record.form.isEmpty() && !hasKnownAccessor.quickGet(cellOf(reference)) && !hasKnownAccessor.quickGet(declaration)
                && !m_formsLaidInListedFields.contains(cellOf(reference)) && !m_formsLaidInListedFields.contains(declaration);
            liesBesideAccessors |= cell.liesBesideWhatDescriptorsDefine && everyConstructionIsSeen && !m_tables.layouts[index].possibleChanges.contains(LayoutChange::LosesProperty);
            cell.isFilled = (keepsWhatConstructionsFill || liesBesideAccessors || (everyConstructionIsSeen && isPrivateNameInLink(m_program, record.key))) && !m_flow.fieldsGivenAccessorsByDescriptorsInSight.get(cellOf(reference)) && record.key && record.form.isEmpty() && !m_flow.fieldsThatMayBeEmpty.get(cellOf(reference))
                && (arrivesAtDeclaration[declaration] & somethingArrives) && !(m_flow.typeAt(m_flow.field(reference)).primitives & Primitive::Absent);
            bool parsersMayLeaveItOut = m_isMadeByParsers.quickGet(index) && !record.flags.contains(FieldFlags::IsRequired);
            cell.isOfBirth = constructionsAreFollowed && isAllocated && record.key && record.form.isEmpty() && !parsersMayLeaveItOut && !m_flow.fieldsNotOfBirth.get(cellOf(reference));
            cell.isKeptFromComparisons = isPrivateNameInLink(m_program, record.key) && !m_flow.isInspectedByComparisonOfPrivateFields(m_flow.field(declarationOf(m_program, reference)));
            cell.isFilledAtEveryRead = cell.isFilled && !m_fieldsReadInFrontOfDefinition.quickGet(cellOf(reference))
                && !(isLost && m_keysReadInFrontOfDefinitionThroughLost.contains(record.key.bits()));
            cell.isLeftUndefinedByMakers = isGivenUndefinedByMakers && m_fieldsGivenUndefinedByMakers.quickGet(declaration) && !m_fieldsFilledByConstructors.quickGet(declaration);
            cell.isGivenUndefinedOrNullInSight = m_fieldsGivenUndefinedOrNullLate.quickGet(declaration) || (m_fieldsGivenUndefinedOrNullByMakers.quickGet(declaration) && !m_fieldsGivenValuesByMakers.quickGet(declaration));
            cell.isGivenByNoMaker = isRecordThatIsMade && !m_fieldsGivenUndefinedByMakers.quickGet(declaration) && !m_fieldsFilledByConstructors.quickGet(declaration)
                && !hasKnownAccessor.quickGet(cellOf(reference)) && !hasKnownAccessor.quickGet(declaration);
            cell.isFilledByAccessorsAlone = isRecord && record.form.isEmpty() && hasKnownAccessor.quickGet(cellOf(reference))
                && !m_fieldsGivenValues.quickGet(cellOf(reference)) && !m_fieldsGivenValues.quickGet(declaration);
            bool isGivenValuesByDefinitionsAlone = isRecord && !isLost && (m_fieldsInWhichDefinitionsLayForms.quickGet(cellOf(reference)) || m_fieldsInWhichDefinitionsLayForms.quickGet(declaration)) && record.form.isEmpty() && !hasKnownAccessor.quickGet(cellOf(reference)) && !hasKnownAccessor.quickGet(declaration) && cell.everyWriterIsKnown && !cell.isOfBirth
                && m_flow.fieldsNotOfBirth.get(cellOf(reference)) && !m_fieldsWrittenWithoutDefinition.get(cellOf(reference)) && !m_fieldsWrittenWithoutDefinition.get(declaration)
                && !m_fieldsInWhichDescriptorsMayLayAccessors.quickGet(cellOf(reference)) && !m_fieldsInWhichDescriptorsMayLayAccessors.quickGet(declaration);
            if (uint32_t forms = cell.isFilledByAccessorsAlone || isGivenValuesByDefinitionsAlone ? m_formsLaidInListedFields.get(cellOf(reference)) | m_formsLaidInListedFields.get(declaration) : 0; hasOneBitSet(forms) && forms != formNobodyKnows) {
                OptionSet<FieldForm> created = OptionSet<FieldForm>::fromRaw(static_cast<uint8_t>(WTF::ctz(forms)));
                if (cell.isFilledByAccessorsAlone || !created.contains(FieldForm::IsAccessor))
                    cell.formThatDefinitionsCreate = created;
            }
            cell.holdsAccessorFromBirth = cell.isFilledByAccessorsAlone && keepsWhatLiesBesideAccessors && m_flow.reading() == WhatUnseenCodeDoes::Anything && isAllocated
                && !m_flow.fieldsThatMayBeEmpty.get(cellOf(reference)) && !m_fieldsReadInFrontOfDefinition.quickGet(cellOf(reference))
                && !(isLost && (m_keysReadInFrontOfDefinitionThroughLost.contains(record.key.bits()) || isGivenValueThroughLost(LayoutID(index), record.key)));
            cell.keepsAccessorOnceLaid = cell.isFilledByAccessorsAlone && !cell.holdsAccessorFromBirth && !isLost && !m_fieldsWrittenWithoutDefinition.get(cellOf(reference)) && !m_fieldsWrittenWithoutDefinition.get(declaration) && cell.everyWriterIsKnown && cell.proven && m_flow.reading() == WhatUnseenCodeDoes::Anything
                && m_isRedefinedByWrittenDefinitionsAlone.quickGet(index) && cell.formThatDefinitionsCreate.containsAll({ FieldForm::IsAccessor, FieldForm::IsNotConfigurable });
            isOfProgram &= isOfProgramInLink(cell.proven);
        }
        return isOfProgram;
    });

    BitVector holdsCellsOfBase;
    holdsCellsOfBase.ensureSize(numberOfLayouts);
    Vector<uint32_t, 16> rowToCopyDown;
    std::span<LinkedCell> cells = m_tables.fields.mutableSpan();
    for (size_t below : takesCellsOfBase) {
        for (auto layout = static_cast<uint32_t>(below); takesCellsOfBase.quickGet(layout) && !holdsCellsOfBase.quickSet(layout); layout = writtenBaseOf(m_program.layouts[layout]).index())
            rowToCopyDown.append(layout);
        while (!rowToCopyDown.isEmpty()) {
            LayoutID layout(rowToCopyDown.takeLast());
            LayoutID base = writtenBaseOf(m_program.at(layout));
            std::ranges::copy(cells.subspan(cellOf({ base, 0 }), m_program.at(base).fields.size()), cells.subspan(cellOf({ layout, m_program.at(layout).ownFieldCount })).begin());
        }
    }
}

thread_local TypesOfThreadInLink WriterOfLinkLayouts::s_typesOfThread;

void WriterOfLinkLayouts::writeVariables()
{
    m_tables.variablesStoredFirst = m_flow.firstReadersOfVariablesStoredFirst;
    writeOnAllThreads(static_cast<unsigned>(m_tables.variables.size()), [&](unsigned index) {
        LinkedCell& cell = m_tables.variables[index];
        cell.everyWriterIsKnown = !m_variablesSharedWithUnseenCode.quickGet(index) && !m_flow.escapedVariables.get(index);
        cell.isWrittenAfterConstruction = !cell.everyWriterIsKnown || m_variablesWrittenLate.quickGet(index);
        cell.isStoredInto = !cell.everyWriterIsKnown || m_variablesStoredInto.quickGet(index);
        cell.isFilled = cell.isFilledAtEveryRead = m_flow.variablesStoredFirst.get(index);
        const DeclaredType& declared = m_program.variables[index].declared;
        cell.proven = typeEstablishedOf(declared);
        bool saysWhatWasSeenOfStaticField = m_flow.reading() != WhatUnseenCodeDoes::Anything && m_program.variables[index].owner;
        if (!cell.everyWriterIsKnown && !saysWhatWasSeenOfStaticField)
            return isOfProgramInLink(cell.proven);
        cell.proven = narrowerOf(claimedTypeAt(m_flow.variable(VariableID(index))), declared);
        cell.proven = namingSymbolAt(m_flow.variable(VariableID(index)), cell.proven);
        const LinkType& arrived = m_flow.typeAt(m_flow.variable(VariableID(index)));
        if (m_flow.reading() != WhatUnseenCodeDoes::Anything || arrived.classes != LinkType::Classes::Site || arrived.objects != LinkType::Objects::None || arrived.functions != LinkType::Functions::None
            || arrived.hasUnknownObjects || arrived.holdsLostBuiltins || arrived.builtin || m_flow.hasEscaped(arrived.site))
            return isOfProgramInLink(cell.proven);
        cell.staysAsItsClassMakesIt = std::ranges::all_of(std::array { SlotOfContents::underUnknownNames(), SlotOfContents::noLongerAsMade() }, [&](SlotOfContents slot) {
            FlowNodeID held = m_flow.findContentsOf(arrived, slot);
            return !held || (held != m_flow.unseen() && m_flow.typeAt(held).isNever());
        });
        return isOfProgramInLink(cell.proven);
    });
}

void WriterOfLinkLayouts::writeCaptures()
{
    writeOnAllThreads(static_cast<unsigned>(m_program.functions.size()), [&](unsigned index) {
        if (!m_flow.reached.get(index))
            return true;
        FunctionID id(index);
        uint32_t first = m_tables.firstCaptureOfFunction[index];
        uint32_t numberOfCaptures = m_tables.firstCaptureOfFunction[index + 1] - first;
        bool isOfProgram = true;
        for (uint32_t capture = 0; capture < numberOfCaptures; ++capture) {
            LinkedCell& cell = m_tables.captures[first + capture];
            cell.everyWriterIsKnown = true;
            cell.isWrittenAfterConstruction = false;
            cell.proven = claimedTypeAtEither(m_flow.capture(id, capture), m_flow.parameter(id, capture));
            cell.proven = narrowerOf(cell.proven, m_program.at(id).captures[capture]);
            isOfProgram &= isOfProgramInLink(cell.proven);
        }
        return isOfProgram;
    });
}

void WriterOfLinkLayouts::writeMembers()
{
    auto placeOf = [](const LinkedMember& member) {
        return std::tuple { member.classLayout, member.side, member.index };
    };
    std::ranges::sort(m_members, { }, placeOf);
    auto repeated = std::ranges::unique(m_members, { }, placeOf);
    m_members.shrink(repeated.begin() - m_members.begin());
    m_tables.members = WTF::move(m_members);
    m_tables.indexMembersByKey(m_program, m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls ? LinkTables::AskedByKey::ByLongRowsAlone : LinkTables::AskedByKey::ByWalkToo);

    for (auto [laid, taken] : m_namesLaidOutOfMaps) {
        auto name = m_namesWrittenOnPrototypes.find(laid);
        auto accessor = m_accessorsWrittenInMaps.find(taken);
        if (accessor != m_accessorsWrittenInMaps.end() && accessor->value.isWrittenOnce && !name->value.isWrittenAgain)
            name->value = accessor->value;
    }

    for (const LinkFlow::NameOnPrototype& name : m_flow.namesOnPrototypes()) {
        if (!name.mayHoldAccessor() && (!name.isStoredInto || !name.key))
            continue;
        LinkedNameOnPrototype linked { name.layout, name.key, FunctionID() };
        linked.isStoredIntoFromOutside = name.isStoredIntoFromOutside;
        linked.isUnderSymbolsAlone = !name.key && !name.isDefinedByDescriptor;
        linked.mayHoldAccessor = name.mayHoldAccessor();
        const LinkType& held = m_flow.typeAt(name.stored);
        LinkType oneClosure;
        oneClosure.functions = LinkType::Functions::Closure;
        oneClosure.function = held.function;
        if (held == oneClosure && !name.mayHoldAccessor() && !hasEscaped(name.layout.index()))
            linked.function = held.function;
        oneClosure.function = m_flow.typeAt(name.storedInSight).function;
        if (m_flow.typeAt(name.storedInSight) == oneClosure && !name.mayHoldAccessor())
            linked.expected = oneClosure.function;
        if (auto written = m_namesWrittenOnPrototypes.find(static_cast<uint64_t>(name.layout.index()) << 32 | name.key.bits()); name.key && written != m_namesWrittenOnPrototypes.end()) {
            if (written->value.isWrittenOnce && !name.isStoredInto) {
                linked.expectedGetter = written->value.getter;
                linked.expectedSetter = written->value.setter;
            }
            m_namesWrittenOnPrototypes.remove(written);
        }
        m_tables.namesOnPrototypes.append(linked);
    }
    for (const auto& [word, written] : m_namesWrittenOnPrototypes) {
        LinkedNameOnPrototype linked { LayoutID(static_cast<uint32_t>(word >> 32)), PropertyKeyID::fromBits(static_cast<uint32_t>(word)), FunctionID() };
        if (written.isWrittenOnce) {
            linked.expected = written.method;
            linked.expectedGetter = written.getter;
            linked.expectedSetter = written.setter;
        }
        m_tables.namesOnPrototypes.append(linked);
    }
    std::ranges::sort(m_tables.functionsWrittenRightOfNamesOnPrototypes);
    m_tables.functionsWrittenRightOfNamesOnPrototypes.shrink(std::ranges::unique(m_tables.functionsWrittenRightOfNamesOnPrototypes).begin() - m_tables.functionsWrittenRightOfNamesOnPrototypes.begin());
    std::ranges::sort(m_tables.namesOnPrototypes, { }, [](const LinkedNameOnPrototype& name) {
        return std::pair { name.layout, name.key };
    });
}

void WriterOfLinkLayouts::lookAtSummaries()
{
    forEachLayoutMadeAtNoNodeInLinkLayouts(m_program, [&](LayoutID layout) {
        m_isMadeAtNoNode.quickSet(layout.index());
    });
    forEachVariableSharedWithUnseenCodeInLinkLayouts(m_program, [&](VariableID variable) {
        m_variablesSharedWithUnseenCode.quickSet(variable.index());
    });
    forEachFieldSharedWithEvaluatedTextInLinkLayouts(m_program, [&](FieldReference field) {
        m_fieldsWithUnseenWriters.quickSet(cellOf(field));
    });
    noteLostClassObjects();

    for (size_t index : m_flow.reached) {
        FunctionID id(index);
        if (!m_program.at(id).constructedClass)
            continue;
        const ThinSummary& summary = m_flow.summaries.at(id);
        for (const SummaryAccess& access : summary.accesses) {
            if (PropertyKeyID key = findNameThatConstructorStores(id, summary, access))
                m_namesThatConstructorsStore.add(static_cast<uint64_t>(m_program.at(id).constructedClass.index()) << 32 | key.bits());
        }
    }

    m_fieldFactsFromEdges.fieldsGivenValues.ensureSize(m_tables.fields.size());
    m_fieldFactsFromEdges.fieldsGivenUndefinedByMakers.ensureSize(m_tables.fields.size());
    m_fieldFactsFromEdges.fieldsFilledByConstructors.ensureSize(m_tables.fields.size());
    m_fieldFactsFromEdges.fieldsWrittenLate.ensureSize(m_tables.fields.size());
    m_fieldFactsFromEdges.layoutsGivenUndefinedByMakers.ensureSize(m_program.layouts.size());
    m_fieldFactsFromEdges.unitsListedForFields.fill(0, m_tables.fields.size());
    if (m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls)
        m_classesThatMakersLayInFields.fill(0, m_tables.fields.size());
    m_fieldFactsFromEdges.whatMakersLayInFields.fill(0, m_classesThatMakersLayInFields.size());
    runLanes(m_flow.loopsOfReading().ofCurrentThread(), {
        [&] {
            for (size_t index : m_flow.reached) {
                FunctionID id(index);
                const ThinSummary& summary = m_flow.summaries.at(id);
                lookAtAccesses(id, summary);
                lookAtChanges(id, summary);
                lookAtMembers(summary);
            }
        },
        [&] {
            for (size_t index : m_flow.reached)
                lookAtEdges(FunctionID(index), m_flow.summaries.at(FunctionID(index)));
        },
    });
    m_fieldsGivenValues.merge(m_fieldFactsFromEdges.fieldsGivenValues);
    if (!AOT::hasFlag(m_flow.build.options().testing, Wire::TestingSwitch::TreatLiteralFieldsAsDefined))
        m_fieldsWrittenWithoutDefinition.merge(m_fieldFactsFromEdges.fieldsGivenValues);
    m_fieldsGivenUndefinedByMakers.merge(m_fieldFactsFromEdges.fieldsGivenUndefinedByMakers);
    m_fieldsFilledByConstructors.merge(m_fieldFactsFromEdges.fieldsFilledByConstructors);
    m_fieldsWrittenLate.merge(m_fieldFactsFromEdges.fieldsWrittenLate);
    m_layoutsGivenUndefinedByMakers.merge(m_fieldFactsFromEdges.layoutsGivenUndefinedByMakers);
    for (size_t cell : m_fieldFactsFromEdges.fieldsGivenValues) {
        if (uint32_t made = m_classesThatMakersLayInFields.isEmpty() ? 0 : m_fieldFactsFromEdges.whatMakersLayInFields[cell])
            noteWhatMakerLays(m_classesThatMakersLayInFields[cell], made);
        uint32_t listed = m_fieldFactsFromEdges.unitsListedForFields[cell];
        if (uint32_t& unit = m_unitsListedForFields[cell]; listed && unit != listed)
            unit = unit ? otherThanOneUnit : listed;
    }
    noteNamesStoredThroughWhatFieldsAreExpectedToHold();
    noteFormsOfNamesDefinedThroughWrittenTypesOfParameters();
    m_fieldFactsFromEdges = { };
    m_whatIsNotedOfEveryField = { };
}

void WriterOfLinkLayouts::writeWhatMakesNoType(BitVector&& functionsWhoseCallersAreAllKnown)
{
    m_functionsWhoseCallersAreAllKnown = WTF::move(functionsWhoseCallersAreAllKnown);
    noteNamesThatMakersGive();
    runLanes(m_flow.loopsOfReading().ofCurrentThread(), {
        [&] {
            appendFields();
            writeRunsOfMakers();
            writeRunsBehindTaggedMakers();
        },
        [&] {
            gatherAtDeclarationsOfFields();
        },
    });
}

void WriterOfLinkLayouts::write(TypeArena& types)
{
    m_types = &types;
    m_typesTurned = { };
    writeLayouts();
    writeFields();
    writeAppendedFields();
    writeAccessorsExpectedInFields(m_flow, m_tables);
    writeMembersAboveCreatedLayouts(m_flow, m_tables);
    writeVariables();
    if (m_flow.reading() == WhatUnseenCodeDoes::Anything)
        writeCaptures();
    writeMembers();
    writeWhosePrototypePropertyStaysAsMade();
    writeNamesOnFunctions();
    writeRowsBelowNamesDefinedPastLists();
    m_typesTurned = { };
}

void DeleterOfWriterOfLinkLayouts::operator()(WriterOfLinkLayouts* writer) const
{
    delete writer;
}

void lookAtSummariesForLayoutsAndCells(const LinkFlow& flow, LinkTables& tables)
{
    flow.writerOfLayoutsOfReading().reset(new WriterOfLinkLayouts(flow, tables));
    flow.writerOfLayoutsOfReading()->lookAtSummaries();
}

void writeLayoutsAndCellsAheadOfTypes(const LinkFlow& flow, BitVector&& functionsWhoseCallersAreAllKnown)
{
    flow.writerOfLayoutsOfReading()->writeWhatMakesNoType(WTF::move(functionsWhoseCallersAreAllKnown));
}

void writeLayoutsAndCells(const LinkFlow& flow, TypeArena& types)
{
    flow.writerOfLayoutsOfReading()->write(types);
    flow.writerOfLayoutsOfReading() = nullptr;
}

} } } // namespace JSC::AOT::Typed

#include "AOTLinkBirthsInlines.h"
#include "AOTLinkFollowersOfMakersInlines.h"
#include "AOTRowsOfLayouts.h"

#endif // ENABLE(AOT)
