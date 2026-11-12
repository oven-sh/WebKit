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
#include "AOTDeclaredFunctionsInScopes.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTLinkTypesOnAllThreads.h"
#include "AOTLinkTypesTurnedOnce.h"
#include "AOTLinkUnions.h"
#include "AOTLinkStrings.h"
#include "AOTNameClass.h"
#include "AOTRowsOfLayouts.h"
#include "AOTSort.h"
#include "AOTTypedArrayKinds.h"
#include "AOTUnionTaggedMakers.h"
#include "JSObject.h"
#include <algorithm>
#include <array>
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
static void forEachVariableSharedWithUnseenCodeInLinkLayouts(const Program& program, std::span<const EvalScope> scopesOfEvals, const Functor& functor)
{
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        const Module& module = program.at(variable.module);
        if (variable.owner || variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun, VariableFlags::HoldsInstance })
            || !module.body || !variablesAreWrittenOnlyByBodiesInLinkLayouts(module.kind))
            functor(VariableID(index));
    }

    for (const EvalScope& scope : scopesOfEvals) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.variable)
                functor(binding.variable);
        }
    }
}

template<typename Functor>
static void forEachFieldSharedWithEvaluatedTextInLinkLayouts(std::span<const EvalScope> scopesOfEvals, const Functor& functor)
{
    for (const EvalScope& scope : scopesOfEvals) {
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
    bool descriptorSetsFlagsAlone { false };
    bool isEntryWrittenInMap { false };
    bool laysAccessorAsWritten { false };
    bool keyIsSomeSymbol { false };
    bool namesOfCopyLeaveLinkAlone { false };
    bool laysValueInSight { false };
    FlowNodeID valueInSight;
    FlowNodeID valueLaidThroughConstant;
    bool heirIsMadeByCall { false };
    FlowNodeID laidAboveByConstructor;
    bool getterEscapesOnlyThroughLink { false };
    bool accessorEscapesOnlyThroughLink { false };
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
    forEachLayoutInType(flow, change.subject, [&](LayoutID layout) {
        flow.noteDeletion(layout, change.key);
    });

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

static LayoutID onlyPrototypeGainingNameInLinkLayouts(const LinkFlow& flow, const LinkType& type, PropertyKeyID key)
{
    if (LayoutID function = onlyPrototypeOfConstructorFunctionInLinkLayouts(flow, type))
        return function;
    LinkType withoutPrimitives = type;
    withoutPrimitives.primitives = 0;
    if (!key || type.objects != LinkType::Objects::Prototype || withoutPrimitives != typeOfPrototypeInLinkLayouts(type.layout) || !flow.followsNamesOnPrototypeOf(type.layout))
        return { };
    NameClass nameClass = flow.nameClassOf(key);
    if (nameClass == NameClass::Constructor || nameClass == NameClass::Proto || flow.instancesMayHoldName(type.layout, key))
        return { };
    return flow.findMembers(type.layout, ClassSide::Prototype, key).lookupGoesOnAboveClasses() ? type.layout : LayoutID();
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
        flow.addEdge(flow.resultToFollow(id, 0), held);
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

static bool hasClosureAccessorsInLinkLayouts(const LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    const ThinSummary& summary = flow.summaries.at(change.function);
    const SummaryChange& record = summary.changes[change.record];
    auto isClosureOrAbsent = [&](uint32_t half) {
        return half == noSummaryNode || summary.nodes[half].kind == SummaryNode::Kind::Closure;
    };
    return isClosureOrAbsent(record.getterInSight) && isClosureOrAbsent(record.setterInSight);
}

static bool allocationInBodyFillsFieldInLinkLayouts(const LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    std::optional<FieldReference> field = findField(flow.program, change.subject.layout, change.key);
    const ThinSummary& summary = flow.summaries.at(change.function);
    const SummaryChange& record = summary.changes[change.record];
    return field && !record.fillsObjectBeingMade && summary.nodes[record.subject].kind == SummaryNode::Kind::Allocation && !summary.leavesEmpty(*field);
}

static bool definesPrototypeAccessorInLinkLayouts(const LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    return change.laysAccessorAsWritten && flow.isDefinedByDescriptorSomewhere(change.key) && onlyPrototypeGainingNameInLinkLayouts(flow, change.subject, change.key);
}

static bool defineAccessorHalvesInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change, LayoutID layout)
{
    ObservedChangeInLinkLayouts laid = change;
    laid.subject.objects = LinkType::Objects::Layout;
    laid.subject.layout = layout;
    laid.subject.family = { };
    bool isKnown = true;
    auto lay = [&](FlowNodeID node, SummaryChange::Lays lays) {
        laid.lays = lays;
        laid.otherNode = node;
        laid.other = flow.typeAt(node);
        isKnown &= layKnownAccessorInLinkLayouts(flow, laid);
    };
    if (change.getterInSight)
        lay(change.getterInSight, SummaryChange::Lays::Getter);
    if (change.setterInSight)
        lay(change.setterInSight, SummaryChange::Lays::Setter);
    return isKnown;
}

static bool defineDescriptorAccessorInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    if (change.lays != SummaryChange::Lays::WhatDescriptorSays || !change.descriptorIsInSight || !change.key || (!change.getterInSight && !change.setterInSight) || change.subject.hasUnknownObjects || change.subject.holdsLostBuiltins)
        return false;
    if (change.subject.objects != LinkType::Objects::Layout && change.subject.objects != LinkType::Objects::Set && change.subject.objects != LinkType::Objects::Family)
        return false;
    bool canHold = hasClosureAccessorsInLinkLayouts(flow, change);
    forEachLayoutInType(flow, change.subject, [&](LayoutID layout) {
        bool isFill = flow.program.at(layout).kind == LayoutKind::Record || flow.summaries.at(change.function).changes[change.record].fillsObjectBeingMade;
        canHold &= isFill && canHoldAccessors(flow.program, layout) && (change.subject.objects == LinkType::Objects::Layout || findField(flow.program, layout, change.key));
    });
    if (!canHold)
        return false;
    bool areLaid = true;
    forEachLayoutInType(flow, change.subject, [&](LayoutID layout) {
        std::optional<FieldReference> listed = findField(flow.program, layout, change.key);
        areLaid &= (listed || !change.setterInSight) && defineAccessorHalvesInLinkLayouts(flow, change, layout);
        if (areLaid)
            flow.arrive(listed ? flow.field(*listed) : flow.appendedField(layout, change.key), flow.fromTypeID(TypeTable::undefined));
    });
    return areLaid && (change.subject.objects != LinkType::Objects::Layout || !allocationInBodyFillsFieldInLinkLayouts(flow, change));
}

static void observeDefinitionInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    LinkType whatCanBeDefinedOn = change.subject;
    whatCanBeDefinedOn.primitives = 0;
    if (whatCanBeDefinedOn.isNever())
        return;
    bool namesFunctionsAlone = change.subject.functions != LinkType::Functions::None && change.subject.objects == LinkType::Objects::None && change.subject.classes == LinkType::Classes::None && !change.subject.builtin && !change.subject.hasUnknownObjects && !change.subject.holdsLostBuiltins;
    if (change.descriptorSetsFlagsAlone && change.key == flow.program.keyOf(BuiltinNames::prototype) && namesFunctionsAlone)
        return;
    bool accessorIsKnown = defineDescriptorAccessorInLinkLayouts(flow, change);
    if (change.accessorEscapesOnlyThroughLink && !change.getterEscapesOnlyThroughLink && !accessorIsKnown && !definesPrototypeAccessorInLinkLayouts(flow, change)) {
        for (FlowNodeID half : { change.getterInSight, change.setterInSight }) {
            if (half)
                handToUnseenCodeInLinkLayouts(flow, half, flow.typeAt(half));
        }
    }
    bool isStore = change.laysValueInSight && change.key && definitionInSightIsStoreInLinkLayouts(flow, change.subject, change.key);
    bool namesObjectsOfLayoutsAlone = (change.subject.objects == LinkType::Objects::Layout || change.subject.objects == LinkType::Objects::Set || change.subject.objects == LinkType::Objects::Family) && change.subject.functions == LinkType::Functions::None && change.subject.classes == LinkType::Classes::None && !change.subject.builtin;
    bool objectMayBeOutOfSight = change.subject.hasUnknownObjects || change.subject.holdsLostBuiltins;
    bool isOneClosureWhoseNamesAreFollowed = change.key && change.subject.functions == LinkType::Functions::Closure && change.subject == typeOfClosuresInLinkLayouts(change.subject.function) && flow.isPlainClosureOrConstructorFunction(change.subject.function) && flow.followsNamesOnClosuresOf(change.subject.function, change.key);
    bool accessLaysValue = change.key && change.descriptorIsInSight && change.descriptorLaysNoAccessor && ((namesObjectsOfLayoutsAlone && !objectMayBeOutOfSight) || isOneClosureWhoseNamesAreFollowed);
    LayoutID holderOfValue = change.valueInSight && !isStore && change.key && change.descriptorIsInSight && change.descriptorLaysNoAccessor ? onlyPrototypeGainingNameInLinkLayouts(flow, change.subject, change.key) : LayoutID();
    if (holderOfValue) {
        flow.addEdge(change.valueInSight, flow.storedOnPrototypeOf(holderOfValue, change.key, false, true, true));
        flow.defineOnPrototypeOf(holderOfValue, change.key, LinkFlow::UnknownKey::IsAnyKey);
        return;
    }
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
    if (change.descriptorIsInSight && change.isEntryWrittenInMap && !change.descriptorLaysNoAccessor && namesObjectsOfLayoutsAlone && !accessorIsKnown) {
        for (FlowNodeID half : { change.getterInSight, change.setterInSight }) {
            if (half)
                handToUnseenCodeInLinkLayouts(flow, half, flow.typeAt(half));
        }
    }
    if (change.getterEscapesOnlyThroughLink && !accessorIsKnown)
        handToUnseenCodeInLinkLayouts(flow, change.getterInSight, flow.typeAt(change.getterInSight));
    if (change.descriptorIsInSight && namesObjectsOfLayoutsAlone && handObjectToHalvesInSightInLinkLayouts(flow, change)) {
        forEachLayoutInType(flow, change.subject, [&](LayoutID layout) {
            if (std::optional<FieldReference> field = change.descriptorLaysNoAccessor || !change.key ? std::nullopt : findField(flow.program, layout, change.key)) {
                flow.noteFieldGivenAccessorByDescriptorInSight(*field);
                if (!accessorIsKnown)
                    flow.noteFieldWithUnseenAccessor(*field);
            } else if (!change.descriptorLaysNoAccessor && !flow.layoutsGivenAccessorsByDescriptorsInSight.get(layout.index()))
                flow.note(LinkFlow::Note::LayoutGivenAccessorByDescriptorInSight, layout.index());
            if (!flow.layoutsDefinedOnUnderWrittenKeys.get(layout.index()))
                flow.note(LinkFlow::Note::LayoutDefinedOnUnderWrittenKeys, layout.index());
            if (!change.keyIsWritten && !flow.layoutsDefinedOnUnderKeysThatAreNotWritten.get(layout.index()))
                flow.note(LinkFlow::Note::LayoutDefinedOnUnderKeyThatIsNotWritten, layout.index());
            if (!change.key) {
                flow.addEdge(flow.unseen(), flow.extraPropertyStoreNode(layout));
                return;
            }
            if (change.setterInSight && !accessorIsKnown) {
                std::optional<FieldReference> field = findField(flow.program, layout, change.key);
                flow.addEdge(field ? flow.field(*field) : flow.appendedField(layout, change.key), flow.unseen());
            }
            if (!change.descriptorLaysNoAccessor && !accessorIsKnown) {
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
    bool isLaidByNameOnPrototype = definesPrototypeAccessorInLinkLayouts(flow, change);
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
    if (LayoutID holder = onlyPrototypeGainingNameInLinkLayouts(flow, change.subject, change.key)) {
        flow.defineOnPrototypeOf(holder, change.key, change.keyIsSomeSymbol ? LinkFlow::UnknownKey::IsSomeSymbol : LinkFlow::UnknownKey::IsAnyKey);
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
        if (functionLinked && change.other.functions == LinkType::Functions::Closure)
            flow.linkFunctionBelow(functionLinked, change.other.function);
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

bool LinkFlow::namesParentOfPrototype(const LinkType& parent, LayoutID classLayout) const
{
    return !(parent.primitives & Primitive::Null) && agreesWithLayoutsInLinkLayouts(*this, parent, typeOfPrototypeInLinkLayouts(classLayout));
}

void LinkFlow::linkFunctionBelow(FunctionID child, FunctionID parent)
{
    FlowNodeID found = foundOnClosuresOf(child);
    addEdge(foundOnClosuresOf(parent), found);
    arrive(found, fromTypeID(TypeTable::undefined));
}

bool LinkFlow::everyFunctionAboveIsConstructorFunction(FunctionID function) const
{
    return layoutOfConstructorFunction(function) && everyFunctionAboveIsConstructorFunctionWithBodyInLinkLayouts(*this, function);
}

bool LinkFlow::namesParentOfFunction(const LinkType& parent, FunctionID child) const
{
    return !(parent.primitives & Primitive::Null) && layoutOfConstructorFunction(child) && everyFunctionAboveIsConstructorFunctionWithBodyInLinkLayouts(*this, child) && agreesWithLayoutsInLinkLayouts(*this, parent, typeOfClosuresInLinkLayouts(child));
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

static bool allocatesForNewTargetInLinkLayouts(const LinkFlow& flow, FunctionID function)
{
    LayoutID classLayout = flow.classOfConstructor(function);
    if (!classLayout || flow.isOfConstructorFunction(classLayout))
        return false;
    const Layout& top = flow.program.at(flow.topOfRow(classLayout));
    return !top.writtenBuiltinBase && top.constructor && isClassConstructorOf(flow.program.at(top.constructor).kind);
}

static void linkSuperCallsToOwnReceivers(LinkFlow& flow, const LinkType& heritage, const LinkType& below)
{
    LayoutID classLayout = below.functions == LinkType::Functions::Closure ? flow.classOfConstructor(below.function) : LayoutID();
    if (!classLayout || flow.program.at(classLayout).base)
        return;
    bool someClassAllocates = heritage.functions == LinkType::Functions::Constructors;
    if (heritage.functions == LinkType::Functions::Closure)
        someClassAllocates = allocatesForNewTargetInLinkLayouts(flow, heritage.function);
    else if (heritage.functions == LinkType::Functions::Set) {
        someClassAllocates = std::ranges::any_of(flow.functionsOfSet(heritage), [&](FunctionID function) {
            return allocatesForNewTargetInLinkLayouts(flow, function);
        });
    }
    if (someClassAllocates)
        flow.addEdge(flow.objectsMadeThroughConstructorOf(classLayout), flow.objectsMadeBySuperCallsOf(classLayout));
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

    linkSuperCallsToOwnReceivers(flow, change.subject, change.other);
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

static bool namesAppendedFieldsInLinkLayouts(const LinkType& subject, const LinkType& other, const SummaryChange& copy)
{
    auto isOfLayouts = [](const LinkType& type) {
        return (type.objects == LinkType::Objects::Layout || type.objects == LinkType::Objects::Set) && type.functions == LinkType::Functions::None && type.classes == LinkType::Classes::None && !type.builtin;
    };
    return copy.definesWhatItCopies && !copy.isSpelledByLoop && copy.other != noSummaryNode && isOfLayouts(subject) && isOfLayouts(other) && !subject.hasUnknownObjects;
}

static bool mayBeLateFieldInLinkLayouts(const LinkFlow& flow, LayoutID layout, PropertyKeyID key)
{
    NameClass nameClass = flow.nameClassOf(key);
    return nameClass != NameClass::Proto && !mayBeElementKey(nameClass) && !flow.program.isIndexLike(key) && flow.program.at(layout).kind == LayoutKind::Record && !listsAccessorInLinkLayouts(flow, layout);
}

static void noteVisibleNameAddedInLinkLayouts(LinkFlow& flow, LayoutID layout)
{
    if (!flow.possibleChanges[layout.index()].contains(LayoutChange::GainsProperty))
        flow.note(LinkFlow::Note::ChangeOfLayout, layout.index(), OptionSet<LayoutChange> { LayoutChange::GainsProperty }.toRaw() | LinkFlow::changeIsByStoreInSight);
}

static NodesStoredInLinkLayouts nodesDefinedByCopyInLinkLayouts(LinkFlow& flow, const LinkType& subject, PropertyKeyID key)
{
    if (subject.objects != LinkType::Objects::Layout && subject.objects != LinkType::Objects::Set)
        return nodesStoredUnderInLinkLayouts(flow, subject, key, LinkFlow::Lays::Defines);
    NodesStoredInLinkLayouts nodes;
    forEachLayoutInType(flow, subject, [&](LayoutID target) {
        FlowNodeID stored = nodeStoredInLayoutUnderInLinkLayouts(flow, target, key);
        nodes.append(stored || !mayBeLateFieldInLinkLayouts(flow, target, key) ? stored : flow.appendedStoreNode(target, key));
    });
    return nodes;
}

void LinkFlow::noteCopyOfAppendedFields(FunctionID function, uint32_t record)
{
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (m_notedAppendedFieldCopies.add(static_cast<uint64_t>(function.index()) << 32 | record).isNewEntry)
        m_copiesOfAppendedFields.append({ function, record, { }, { }, 0 });
}

bool LinkFlow::copyAppendedFields()
{
    bool hasCopied = false;
    for (size_t place = 0; place < m_copiesOfAppendedFields.size(); ++place) {
        FunctionID function = m_copiesOfAppendedFields[place].function;
        uint32_t record = m_copiesOfAppendedFields[place].record;
        const SummaryChange& copy = summaries.at(function).changes[record];
        LinkType subject = typeAt(nodeOf(function, copy.subject));
        LinkType other = typeAt(nodeOf(function, copy.other));
        if (CopyOfAppendedFields& before = m_copiesOfAppendedFields[place]; before.numberOfAppendedFields == m_appendedFields.size() && before.subject == subject && before.other == other)
            continue;
        else {
            before.numberOfAppendedFields = m_appendedFields.size();
            before.subject = subject;
            before.other = other;
        }
        if (!namesAppendedFieldsInLinkLayouts(subject, other, copy) || !isListedOnBothSidesInLinkLayouts(*this, subject, other))
            continue;
        Vector<OwnValue, 8> appended;
        forEachLayoutInType(*this, other, [&](LayoutID source) {
            if (program.at(source).kind != LayoutKind::Record)
                return;
            forEachAppendedField(source, [&](PropertyKeyID key, FlowNodeID node) {
                uint64_t copyInWord = static_cast<uint64_t>(function.index()) << 32 | record;
                if (m_appendedFieldsCopied.add(std::pair { static_cast<uint64_t>(node.index()) << 32 | static_cast<uint64_t>(subject.layout.index()) << 1 | (subject.objects == LinkType::Objects::Set), copyInWord }).isNewEntry)
                    appended.append({ key, node });
            });
        });
        for (const OwnValue& value : appended) {
            hasCopied = true;
            bool findsField = true;
            forEachLayoutInType(*this, subject, [&](LayoutID target) {
                if (findField(program, target, value.key))
                    return;
                if (mayBeLateFieldInLinkLayouts(*this, target, value.key))
                    noteVisibleNameAddedInLinkLayouts(*this, target);
                else
                    findsField = false;
            });
            if (!findsField)
                addChangesToLayoutsNamedInLinkLayouts(*this, subject, { LayoutChange::GainsProperty });
            for (FlowNodeID stored : nodesDefinedByCopyInLinkLayouts(*this, subject, value.key))
                addEdge(value.node, stored ? stored : unseen());
        }
    }
    if (hasCopied)
        settle();
    return hasCopied;
}

static bool copyFieldByFieldInLinkLayouts(LinkFlow& flow, FunctionID function, uint32_t record, const LinkType& subject, const LinkType& other, bool definesWhatItCopies, bool appendsNames, bool assignsNamesOfRecords, KeysOfCopyInLinkLayouts& keys)
{
    if (subject.objects == LinkType::Objects::None || other.objects == LinkType::Objects::None)
        return true;
    if (!assignsNamesOfRecords && !isListedOnBothSidesInLinkLayouts(flow, subject, other))
        return false;

    auto& layoutsTied = flow.copiesObservedIn(function).layoutsTiedByCopies[definesWhatItCopies << 1 | appendsNames];
    bool keepsTies = subject.objects == LinkType::Objects::Layout && !flow.tellsObserversOnceMore();
    NodesStoredInLinkLayouts underAnyKey;
    if (!assignsNamesOfRecords)
        underAnyKey = nodesLaidUnderAnyKeyByCopyInLinkLayouts(flow, subject, definesWhatItCopies);
    const SummaryChange& copy = flow.summaries.at(function).changes[record];
    bool isSpread = copy.definesWhatItCopies && !appendsNames;
    bool namesAppendedFields = !appendsNames && namesAppendedFieldsInLinkLayouts(subject, other, copy);
    if (namesAppendedFields)
        flow.noteCopyOfAppendedFields(function, record);
    forEachLayoutInType(flow, other, [&](LayoutID layout) {
        std::span<const LinkFlow::OwnValue> values = flow.ownValuesOf(layout);
        FlowNodeID joined = namesAppendedFields && flow.program.at(layout).kind == LayoutKind::Record ? flow.joinedExtraProperties(layout) : FlowNodeID();
        uint64_t pair = static_cast<uint64_t>(layout.index()) << 32 | subject.layout.index();
        bool foundFieldsInTargetBefore = keys.findFieldsInTarget;
        keys.findFieldsInTarget = true;
        auto tied = keepsTies ? layoutsTied.find(pair) : layoutsTied.end();
        bool laysTie = keepsTies && tied == layoutsTied.end() && std::ranges::all_of(values.first(values.size() - LinkFlow::numberOfOwnValuesBesideFields), [](const LinkFlow::OwnValue& value) { return !!value.key; });
        if (tied != layoutsTied.end()) {
            keys.findFieldsInTarget = tied->value;
            values = values.last(LinkFlow::numberOfOwnValuesBesideFields);
        }
        for (LinkFlow::OwnValue value : values) {
            if (!value.key && joined && value.node == joined)
                value.node = flow.extraPropertyStoreNode(layout);
            if (definesWhatItCopies && !value.key) {
                const LinkType& held = flow.typeAt(value.node);
                keys.somethingLiesBesideFields |= !held.isNever();
                keys.somethingSeenLiesBesideFields |= !held.directPart().isNever();
            }
            if (definesWhatItCopies && value.key && !appendsNames) {
                forEachLayoutInType(flow, subject, [&](LayoutID target) {
                    if (findField(flow.program, target, value.key))
                        return;
                    if (isSpread && subject.objects != LinkType::Objects::Family && mayBeLateFieldInLinkLayouts(flow, target, value.key))
                        noteVisibleNameAddedInLinkLayouts(flow, target);
                    else
                        keys.findFieldsInTarget = false;
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
                    if (assignsNamesOfRecords) {
                        flow.lookForSetterOnPrototypesAbove(target, value.key);
                        if (found)
                            flow.noteLateFieldStore(*found);
                    }
                    copyInto(found ? flow.field(*found) : flow.appendedStoreNode(target, value.key));
                });
                continue;
            }
            for (FlowNodeID stored : !value.key ? underAnyKey : isSpread ? nodesDefinedByCopyInLinkLayouts(flow, subject, value.key) : nodesStoredUnderInLinkLayouts(flow, subject, value.key, definesWhatItCopies ? LinkFlow::Lays::Defines : LinkFlow::Lays::Assigns))
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
        FlowNodeID joined = flow.program.at(layout).kind == LayoutKind::Record ? flow.joinedExtraProperties(layout) : FlowNodeID();
        for (const LinkFlow::OwnValue& value : flow.ownValuesOf(layout)) {
            bool isJoined = !value.key && joined && value.node == joined;
            mayHave |= value.key ? flow.nameClassOf(value.key) == NameClass::Proto : !flow.typeAt(isJoined ? flow.extraPropertyStoreNode(layout) : value.node).isNever();
        }
        if (joined) {
            flow.forEachAppendedField(layout, [&](PropertyKeyID key, FlowNodeID) {
                mayHave |= flow.nameClassOf(key) == NameClass::Proto;
            });
        }
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

template<typename OwnValuesOf> static bool takesNamesByAssignmentInLinkLayouts(const LinkFlow&, LayoutID target, const LinkType& source, const OwnValuesOf&);

uint32_t LinkFlow::sourceOfCopySpelledByLoop(FunctionID function, ValueID site) const
{
    for (const SummaryChange& change : summaries.at(function).changes) {
        if (change.site == site && change.isSpelledByLoop)
            return change.other;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool LinkFlow::holdsOnlyPlainRecords(const LinkType& type) const
{
    if (type.objects != LinkType::Objects::Layout && type.objects != LinkType::Objects::Set)
        return false;
    if (type.hasUnknownObjects || type.holdsLostBuiltins || type.builtin || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || (type.primitives & LinkType::closed(Primitive::AnyString)))
        return false;
    bool areRecords = true;
    forEachLayoutInType(*this, type, [&](LayoutID layout) {
        areRecords &= program.at(layout).kind == LayoutKind::Record && !listsAccessorInLinkLayouts(*this, layout);
    });
    return areRecords;
}

bool LinkFlow::findsPlainProperties(FunctionID constructor) const
{
    for (const SummaryStoreOnPrototype& listed : std::ranges::equal_range(storesBeforeInstances, constructor, { }, &SummaryStoreOnPrototype::function)) {
        if (listed.kind != SummaryStoreOnPrototype::Kind::FindsPlainProperty)
            continue;
        auto isPlain = [&](const LinkType& holder) {
            bool isPlain = holdsOnlyPlainRecords(holder);
            forEachLayoutInType(*this, holder, [&](LayoutID layout) {
                isPlain &= !possibleChangesOf(layout.index()).containsAny({ LayoutChange::IsFrozen, LayoutChange::GetsAccessor, LayoutChange::ChangesPrototype, LayoutChange::GetsObjectAbove });
            });
            return isPlain;
        };
        if (listed.position) {
            LinkType made;
            made.objects = LinkType::Objects::Layout;
            made.layout = LayoutID(listed.position - 1);
            if (!isPlain(made))
                return false;
            continue;
        }
        bool isFound = false;
        for (const SummaryAccess& access : summaries.at(listed.method).accesses) {
            if (access.site != listed.site || access.isToContents())
                continue;
            if (access.key != listed.key || access.receiver != noSummaryNode || !isPlain(typeAt(nodeOf(listed.method, access.subject))))
                return false;
            isFound = true;
        }
        if (!isFound)
            return false;
    }
    return true;
}

bool LinkFlow::followsCopySpelledByLoop(const LinkType& target, const LinkType& source) const
{
    auto hasOnlyRecords = [&](const LinkType& type) { return holdsOnlyPlainRecords(type); };
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
    auto hasOnlyInstances = [&](const LinkType& type) {
        bool areInstances = (type.objects == LinkType::Objects::Layout || type.objects == LinkType::Objects::Set) && holdsNoObjectBesideLayoutsInLinkLayouts(type) && !(type.primitives & LinkType::closed(Primitive::AnyString));
        forEachLayoutInType(*this, type, [&](LayoutID layout) {
            areInstances = areInstances && program.at(layout).kind == LayoutKind::Instance && !program.at(layout).has(LayoutFlags::IsOfCreatedObject) && hasPristinePrototypes(layout) && !listsAccessorInLinkLayouts(*this, layout)
                && !escapedLayouts.get(layout.index()) && !possibleChanges[layout.index()].containsAny({ LayoutChange::ChangesPrototype, LayoutChange::GetsAccessor });
        });
        return areInstances;
    };
    if (!source.isNever() && !((hasOnlyRecords(source) || hasOnlyInstances(source)) && listSameNames(source)))
        return false;
    if (hasOnlyRecords(target))
        return true;
    if ((target.objects != LinkType::Objects::Layout && target.objects != LinkType::Objects::Set) || target.hasUnknownObjects || target.holdsLostBuiltins || target.builtin || target.functions != LinkType::Functions::None || target.classes != LinkType::Classes::None)
        return false;
    bool takesNames = true;
    forEachLayoutInType(*this, target, [&](LayoutID layout) {
        takesNames = takesNames && takesNamesByAssignmentInLinkLayouts(*this, layout, source, [&](LayoutID holder) {
            Vector<OwnValue, 8> values;
            for (const Field& field : program.at(holder).fields) {
                if (!isPrivateNameInLink(program, field.key))
                    values.append(OwnValue { field.key, { } });
            }
            return values;
        });
    });
    return takesNames;
}

template<typename OwnValuesOf>
static bool takesNamesByAssignmentInLinkLayouts(const LinkFlow& flow, LayoutID target, const LinkType& source, const OwnValuesOf& ownValuesOf)
{
    const Layout& layout = flow.program.at(target);
    if (layout.has(LayoutFlags::IsOfCreatedObject) || listsAccessorInLinkLayouts(flow, target) || (layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance))
        return false;
    if (layout.kind == LayoutKind::Record)
        return true;
    if (flow.namesMayBeIncomplete(target, ClassSide::Prototype))
        return false;
    bool takesEveryName = true;
    forEachLayoutInType(flow, source, [&](LayoutID holder) {
        for (const LinkFlow::OwnValue& value : ownValuesOf(holder))
            takesEveryName = takesEveryName && (!value.key || (!flow.program.isIndexLike(value.key) && flow.isAbsentFromObjectPrototype(value.key) && !flow.findMembers(target, ClassSide::Prototype, value.key).mayAnswer()));
    });
    return takesEveryName;
}

template<typename OwnValuesOf>
static bool assignsNamesInLinkLayouts(const LinkFlow& flow, const LinkType& target, const LinkType& source, const OwnValuesOf& ownValuesOf)
{
    auto hasOnlyObjectsOfProgram = [](const LinkType& type) {
        return !type.hasUnknownObjects && !type.holdsLostBuiltins && !type.builtin && type.functions == LinkType::Functions::None && type.classes == LinkType::Classes::None;
    };
    if (flow.program.admitsReplacedBuiltins || !hasOnlyObjectsOfProgram(target) || !hasOnlyObjectsOfProgram(source) || (source.primitives & LinkType::closed(Primitive::AnyString)))
        return false;
    if (target.objects != LinkType::Objects::Layout && target.objects != LinkType::Objects::Set)
        return false;
    if (source.objects != LinkType::Objects::None && source.objects != LinkType::Objects::Layout && source.objects != LinkType::Objects::Set && source.objects != LinkType::Objects::Family)
        return false;
    bool isListed = true;
    forEachLayoutInType(flow, source, [&](LayoutID holder) {
        std::span<const LinkFlow::OwnValue> values = ownValuesOf(holder);
        isListed = isListed && !values.empty() && std::ranges::all_of(values, [&](const LinkFlow::OwnValue& value) {
            return value.key ? flow.nameClassOf(value.key) != NameClass::Proto : flow.typeAt(value.node).isNever();
        });
    });
    forEachLayoutInType(flow, target, [&](LayoutID layout) {
        isListed = isListed && takesNamesByAssignmentInLinkLayouts(flow, layout, source, ownValuesOf);
    });
    return isListed;
}

bool LinkFlow::assignsNamesOfRecords(const LinkType& target, const LinkType& source)
{
    return assignsNamesInLinkLayouts(*this, target, source, [&](LayoutID layout) { return ownValuesOf(layout); });
}

bool LinkFlow::assignedNamesOfRecords(const LinkType& target, const LinkType& source) const
{
    return assignsNamesInLinkLayouts(*this, target, source, [&](LayoutID layout) { return findOwnValues(layout); });
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
    bool assignsNamesOfRecords = !change.definesWhatItCopies && change.otherNode && flow.assignsNamesOfRecords(subject, other);
    bool assignmentFindsNoSetter = assignsNamesOfRecords || (!flow.program.admitsReplacedBuiltins && !change.definesWhatItCopies && change.otherNode && landsOnObjectsOfProgramAlone && !isOfObjectsThatNobodyLists(other)
        && isListedOnBothSidesInLinkLayouts(flow, subject, other) && !mayHaveOwnValueUnderPrototypeLinkInLinkLayouts(flow, other));
    bool definesWhatItCopies = change.definesWhatItCopies || assignmentFindsNoSetter;
    KeysOfCopyInLinkLayouts keys;
    bool isListedOnBothSides = change.otherNode && copyFieldByFieldInLinkLayouts(flow, change.function, change.record, subject, other, definesWhatItCopies, change.isSpelledByLoop || assignsNamesOfRecords, assignsNamesOfRecords, keys);
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
    bool copiesCharacters = change.definesWhatItCopies && copiesFieldByField && (other.primitives & texts) && (subject.objects == LinkType::Objects::Layout || subject.objects == LinkType::Objects::Set);
    if (copiesCharacters) {
        LinkType characters;
        characters.primitives = LinkType::closed(Primitive::String);
        forEachLayoutInType(flow, subject, [&](LayoutID layout) {
            flow.arrive(flow.anyFieldStoredThrough(LinkType::Objects::Layout, layout, FamilyID(), true), characters);
        });
    }
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
        if (!assignsNamesOfRecords && (flow.program.at(layout).kind != LayoutKind::Record || listsAccessorInLinkLayouts(flow, layout)))
            flow.escape(layout);
    });
    if (subject.objects == LinkType::Objects::Lost)
        handToUnseenCodeInLinkLayouts(flow, change.subjectNode, subject);

    bool copiesWhatNobodyLists = !copiesFieldByField || other.hasUnknownObjects || isNoObjectOfProgram || (!copiesCharacters && (other.primitives & texts));
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

bool LinkFlow::storesCopiedNamesOnPrototype(const FlowObserver& observer, const LinkType& target, const LinkType& source, CopiesObserved& copies)
{
    const SummaryChange& record = summaries.at(observer.function).changes[observer.record];
    if (record.definesWhatItCopies || record.isSpelledByLoop || record.other == noSummaryNode || program.admitsReplacedBuiltins)
        return false;
    if (target.objects != LinkType::Objects::Prototype || !(target == typeOfPrototypeInLinkLayouts(target.layout)) || !followsNamesOnPrototypeOf(target.layout))
        return false;
    if (source.hasUnknownObjects || source.holdsLostBuiltins || source.builtin || source.functions != LinkType::Functions::None || source.classes != LinkType::Classes::None || (source.primitives & LinkType::closed(Primitive::AnyString)))
        return false;
    if (source.objects != LinkType::Objects::None && source.objects != LinkType::Objects::Layout && source.objects != LinkType::Objects::Set)
        return false;

    Vector<OwnValue, 16> stored;
    bool namesAreListed = true;
    forEachLayoutInType(*this, source, [&](LayoutID layout) {
        LayoutKind kind = program.at(layout).kind;
        std::span<const OwnValue> values = ownValuesOf(layout);
        namesAreListed = namesAreListed && !values.empty() && (kind == LayoutKind::ModuleNamespace || (kind == LayoutKind::Record && !listsAccessorInLinkLayouts(*this, layout)));
        for (const OwnValue& value : values) {
            if (value.key) {
                namesAreListed = namesAreListed && nameClassOf(value.key) != NameClass::Proto && !program.isIndexLike(value.key) && !mayBeElementKey(nameClassOf(value.key));
                stored.append(value);
                continue;
            }
            if (copies.toldOfWhatLiesBesideFields.add(std::pair { static_cast<uint64_t>(value.node.index()) << 32 | observer.function.index(), observer.record }).isNewEntry)
                observe(value.node, FlowObserver { FlowObserver::Kind::OtherOfChange, observer.function, observer.record });
            namesAreListed = namesAreListed && typeAt(value.node).isNever();
        }
    });
    if (!namesAreListed)
        return false;
    for (const OwnValue& value : stored) {
        if (copies.namesStoredOnPrototypes.add(std::pair { static_cast<uint64_t>(value.node.index()) << 32 | target.layout.index(), static_cast<uint64_t>(value.key.bits()) << 32 | observer.record }).isNewEntry)
            storeOnClassHolder(observer.function, target.layout, ClassSide::Prototype, value.key, value.node);
    }
    return true;
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
    case FlowObserver::Kind::PassedToBeLinked:
    case FlowObserver::Kind::OperandOnlyLookedAt:
    case FlowObserver::Kind::KeysAreListed:
    case FlowObserver::Kind::FieldIsOnlyAsked:
        RELEASE_ASSERT_NOT_REACHED();
    }

    constexpr uint16_t texts = LinkType::closed(Primitive::AnyString);
    auto withoutKindsOfPrimitives = [](LinkType type) {
        if (type.primitives)
            type.primitives = (type.primitives & Primitive::Null) | (type.primitives & texts ? texts : 0) | Primitive::Undefined;
        return type;
    };
    const SummaryChange& record = summaries.at(observer.function).changes[observer.record];
    if (record.keyNode == noSummaryNode && !typeObservedBefore().isNever() && withoutKindsOfPrimitives(typeObservedBefore()) == withoutKindsOfPrimitives(now) && typeObservedBefore().hasDirectUnknownObjects() == now.hasDirectUnknownObjects()
        && !(typeObservedBefore().directPrimitives() & texts) == !(now.directPrimitives() & texts) && typeObservedBefore().directPart().isNever() == now.directPart().isNever())
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
    change.descriptorSetsFlagsAlone = record.descriptorSetsFlagsAlone;
    change.isEntryWrittenInMap = record.isEntryWrittenInMap;
    change.laysAccessorAsWritten = record.laysAccessorAsWritten;
    change.getterEscapesOnlyThroughLink = record.getterEscapesOnlyThroughLink;
    change.accessorEscapesOnlyThroughLink = record.accessorEscapesOnlyThroughLink;
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
        auto watch = [&](FlowNodeID node) {
            if (copies.toldOfWhatLiesBesideFields.add(std::pair { static_cast<uint64_t>(node.index()) << 32 | observer.function.index(), observer.record }).isNewEntry)
                observe(node, FlowObserver { FlowObserver::Kind::OtherOfChange, observer.function, observer.record });
        };
        bool namesAppendedFields = namesAppendedFieldsInLinkLayouts(change.subject, change.other, record);
        forEachLayoutInType(*this, change.other, [&](LayoutID layout) {
            for (const OwnValue& value : ownValuesThatMayHaveNoKeyOf(layout)) {
                if (!value.key)
                    watch(value.node);
            }
            if (namesAppendedFields && program.at(layout).kind == LayoutKind::Record)
                watch(extraPropertyStoreNode(layout));
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
        if (storesCopiedNamesOnPrototype(observer, change.subject, change.other, copies))
            return;
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

class DecidedPathsInLinkLayouts {
public:
    explicit DecidedPathsInLinkLayouts(const BodyIndex& index)
        : m_index(index)
    {
    }

    struct Decision {
        BlockID block;
        uint32_t target { 0 };
        ValueID asked;
    };

    void decide(BlockID block, uint32_t target, ValueID asked) { m_decisions.append({ block, target, asked }); }
    bool isEmpty() const { return m_decisions.isEmpty(); }
    std::span<const BlockID> blocksInDominatorOrder() const LIFETIME_BOUND { return m_order.span(); }
    bool dominates(BlockID from, BlockID to) const { return m_dominators[to.index()].quickGet(from.index()); }
    const Vector<Vector<BlockID, 2>>& entering() const LIFETIME_BOUND { return m_entering; }

    bool fallThroughBefore(BlockID block, uint32_t site) const
    {
        return std::ranges::all_of(m_decisions, [&](const Decision& decision) {
            return decision.block == block ? decision.asked.index() < site : dominates(decision.block, block);
        });
    }

    void findDominators()
    {
        uint32_t numberOfBlocks = m_index.numberOfBlocks();
        Vector<Vector<BlockID, 2>>& entering = m_entering;
        entering.resize(numberOfBlocks);
        BitVector every(numberOfBlocks);
        every.quickSet(entryBlock.index());
        Vector<BlockID, 16> live { entryBlock };
        for (size_t next = 0; next < live.size(); ++next) {
            BlockID from = live[next];
            auto decided = std::ranges::find(m_decisions, from, &Decision::block);
            uint32_t number = 0;
            for (BodyTarget target : m_index.block(from).terminator().targets()) {
                if (decided != m_decisions.end() && number++ != decided->target)
                    continue;
                entering[target.block().index()].append(from);
                if (!every.quickSet(target.block().index()))
                    live.append(target.block());
            }
        }

        m_dominators.fill(every, numberOfBlocks);
        BitVector entryAlone(numberOfBlocks);
        entryAlone.quickSet(entryBlock.index());
        m_dominators[entryBlock.index()] = entryAlone;
        for (bool hasChanged = true; std::exchange(hasChanged, false);) {
            for (BlockID block : live.span().subspan(1)) {
                BitVector dominators = every;
                for (BlockID from : entering[block.index()])
                    dominators.filter(m_dominators[from.index()]);
                dominators.quickSet(block.index());
                if (dominators == m_dominators[block.index()])
                    continue;
                m_dominators[block.index()] = WTF::move(dominators);
                hasChanged = true;
            }
        }

        Vector<uint32_t> depths(FillWith { }, numberOfBlocks, 0u);
        for (BlockID block : live)
            depths[block.index()] = static_cast<uint32_t>(m_dominators[block.index()].bitCount());
        Vector<Vector<BlockID, 2>> below(numberOfBlocks);
        for (BlockID block : live) {
            BlockID nearest;
            for (size_t dominator : m_dominators[block.index()]) {
                if (dominator != block.index() && (!nearest || depths[dominator] > depths[nearest.index()]))
                    nearest = BlockID(static_cast<uint32_t>(dominator));
            }
            if (nearest)
                below[nearest.index()].append(block);
        }
        Vector<BlockID, 16> waiting { entryBlock };
        while (!waiting.isEmpty()) {
            BlockID block = waiting.takeLast();
            m_order.append(block);
            std::ranges::sort(below[block.index()], std::ranges::greater());
            waiting.appendVector(below[block.index()]);
        }
    }

private:
    const BodyIndex& m_index;
    Vector<Decision, 1> m_decisions;
    Vector<BlockID, 16> m_order;
    Vector<BitVector> m_dominators;
    Vector<Vector<BlockID, 2>> m_entering;
};

enum class ElementBound : uint8_t { Unknown, ArrayLength, CountOfHolder, Other };

class InitializersInLinkLayouts {
public:
    InitializersInLinkLayouts(const LinkFlow& flow, LayoutID made)
        : m_flow(flow)
        , m_program(flow.program)
        , m_made(made)
        , m_numberOfWords((m_program.at(made).fields.size() + 63) / 64)
    {
    }

    struct Fields {
        uint32_t firstWord { 0 };
        Vector<uint64_t, 2> words;

        bool contains(uint32_t field) const
        {
            uint32_t word = field / 64;
            return word >= firstWord && word - firstWord < words.size() && words[word - firstWord] >> field % 64 & 1;
        }
    };

    std::optional<Fields> findFilled()
    {
        FunctionID constructor = m_program.at(m_made).constructor;
        const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(constructor);
        if (index && m_program.at(constructor).kind == FunctionKind::Plain && !m_program.at(constructor).thisParameterIndex()) {
            Fields nothing;
            nothing.words.fill(0, m_numberOfWords);
            return nothing;
        }
        ValueID object = index ? findNewObject(*index, m_program.at(constructor)) : ValueID();
        if (!object)
            return std::nullopt;
        DecidedPathsInLinkLayouts ways(*index);
        if (!index->hasIrreducibleFlow() && m_program.at(constructor).kind == FunctionKind::Plain && m_flow.prototypeKeepsConstructor(m_made))
            decideQuestionsAboutConstructor(constructor, *index, object, ways);
        if (!ways.isEmpty()) {
            ways.findDominators();
            m_ways = &ways;
            std::optional<Fields> filled = findFilledAfter(constructor, object);
            m_ways = nullptr;
            if (m_objectKeepsConstructor)
                return filled;
            m_bodies.clear();
            m_effects.shrink(0);
            m_methods.shrink(0);
        }
        return findFilledAfter(constructor, object);
    }

    const Vector<FunctionID, 8>& methods() const LIFETIME_BOUND { return m_methods; }
    bool showsObject() const { return m_showsObject; }
    bool readsEmpty(uint32_t field) const { return m_readEmpty.contains(field); }

    std::optional<Fields> findFilledByFirstMethod(FunctionID caller, ValueID construction, const ScopedLambda<bool(FunctionID)>& isEnteredByNewAlone)
    {
        const BodyIndex* found = bodiesLookedAt(m_flow.looks).find(caller);
        if (!found || m_flow.summaries.at(caller).has(SummaryTrait::HasDirectEval) || found->isParameter(construction) || found->opcodeOf(construction) != Opcode::Construct)
            return std::nullopt;
        const BodyIndex& index = *found;
        BodyBlock block = index.block(index.blockOf(construction));
        BitVector names(index.numberOfValues());
        names.quickSet(construction.index());
        auto isObject = [&](ValueID value) {
            return value && names.quickGet(value.index());
        };
        LayoutID madeByCaller = index.blockOf(construction) == entryBlock ? m_program.at(caller).constructedClass : LayoutID();
        std::optional<InitializersInLinkLayouts> ofHolder;
        ValueID holder;
        if (madeByCaller && m_program.at(madeByCaller).constructor == caller && !m_program.at(madeByCaller).base) {
            ofHolder.emplace(m_flow, madeByCaller);
            holder = ofHolder->findNewObject(index, m_program.at(caller));
        }
        BitVector namesOfHolder(index.numberOfValues());
        if (holder)
            namesOfHolder.quickSet(holder.index());
        auto isHolder = [&](ValueID value) {
            return value && namesOfHolder.quickGet(value.index());
        };
        auto isReadForCall = [&](const BodyValue& operation) {
            if (operation.opcode() != Opcode::PropGet || operation.isFreeName() || !operation.key())
                return false;
            ValueID user = index.onlyUsingValue(operation.id());
            return user && index.opcodeOf(user) == Opcode::Call && callsMethodOfObject(index, index.value(user), isObject);
        };
        auto passesObject = [&](BodyBlock passing) {
            bool passes = isObject(passing.subject()) || (passing.hasHandler() && std::ranges::any_of(passing.handler().arguments(), isObject));
            BodyTerminator terminator = passing.terminator();
            terminator.forEachOperand([&](ValueID operand, uint32_t) {
                passes |= isObject(operand);
            });
            for (BodyTarget target : terminator.targets())
                passes |= std::ranges::any_of(target.arguments(), isObject);
            return passes;
        };
        auto isNamedAfterThrow = [&](BlockID blockOfCall) {
            BodyBlock thrower = index.block(blockOfCall);
            if (!thrower.hasHandler())
                return false;
            if (std::ranges::any_of(thrower.handler().arguments(), isObject))
                return true;
            BitVector reached(index.numberOfBlocks());
            reached.quickSet(blockOfCall.index());
            reached.quickSet(index.blockOf(construction).index());
            Vector<BlockID, 8> waiting;
            auto reach = [&](BodyTarget target) {
                if (!reached.quickSet(target.block().index()))
                    waiting.append(target.block());
            };
            reach(thrower.handler());
            while (!waiting.isEmpty()) {
                BodyBlock after = index.block(waiting.takeLast());
                if (passesObject(after))
                    return true;
                for (BodyValue operation : after.values()) {
                    bool namesObject = false;
                    operation.forEachOperand([&](ValueID operand, uint32_t) {
                        namesObject |= isObject(operand);
                    });
                    if (namesObject)
                        return true;
                }
                after.forEachOutgoingEdge([&](BodyTarget target, bool) {
                    reach(target);
                });
            }
            return false;
        };
        auto findFilledByCall = [&](const BodyValue& operation) -> std::optional<Fields> {
            if (operation.opcode() != Opcode::Call || !callsMethodOfObject(index, operation, isObject) || isNamedAfterThrow(index.blockOf(operation.id())))
                return std::nullopt;
            bool isUnknownYet = false;
            FunctionID method = findOnlyMethod(caller, operation.id(), isUnknownYet);
            if (isUnknownYet)
                return everyField();
            const Function* entered = method ? &m_program.at(method) : nullptr;
            if (!entered || (entered->kind != FunctionKind::Plain && entered->kind != FunctionKind::Method))
                return std::nullopt;
            std::optional<unsigned> parameter = entered->thisParameterIndex();
            const BodyIndex* body = bodiesLookedAt(m_flow.looks).find(method);
            if (!body || !parameter)
                return std::nullopt;
            return findFilledAfter(method, body->block(entryBlock).parameter(*parameter));
        };
        Fields stored;
        bool hasStored = false;
        auto storesField = [&](const BodyValue& operation) {
            bool defines = operation.opcode() == Opcode::PropDefine;
            if (block.hasHandler() || (!defines && (operation.opcode() != Opcode::PropSet || operation.isFreeName())) || !operation.key())
                return false;
            if (!isObject(operation.objectOperand()) || isObject(operation.valueOperand()))
                return false;
            std::optional<FieldReference> field = findField(m_program, m_made, operation.key());
            if (!field || (!defines && (mayRunAccessor(operation.key()) || !nothingLiesAbove(operation.key()))))
                return false;
            if (!std::exchange(hasStored, true))
                stored.words.fill(0, m_numberOfWords);
            stored.words[field->field / 64] |= 1ull << field->field % 64;
            return true;
        };
        auto addStored = [&](std::optional<Fields>&& filled) -> std::optional<Fields> {
            if (!hasStored)
                return WTF::move(filled);
            if (!filled)
                return WTF::move(stored);
            for (uint32_t word = 0; word < stored.words.size(); ++word)
                filled->words[word] |= stored.words[word];
            return WTF::move(filled);
        };
        bool isHolderShown = false;
        PropertyKeyID keyInHolder;
        for (BodyValue operation : block.values()) {
            bool mentionsObject = false;
            bool mentionsHolder = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                mentionsObject |= isObject(operand);
                mentionsHolder |= isHolder(operand);
            });
            if (mentionsHolder && !mentionsObject) {
                bool isPlainAccess = (operation.opcode() == Opcode::PropGet || operation.opcode() == Opcode::PropSet) && !operation.isFreeName() && operation.key() && isHolder(operation.objectOperand());
                if (inputPassThroughOpcodes.contains(operation.opcode()) || operation.opcode() == Opcode::ToThis)
                    namesOfHolder.quickSet(operation.id().index());
                else if (!isPlainAccess || (operation.opcode() == Opcode::PropSet && isHolder(operation.valueOperand())) || ofHolder->mayRunAccessor(operation.key()))
                    isHolderShown = true;
                else if (keyInHolder && operation.key() == keyInHolder) {
                    if (operation.opcode() == Opcode::PropSet)
                        return std::nullopt;
                    names.quickSet(operation.id().index());
                }
                if (isHolderShown && keyInHolder)
                    return std::nullopt;
                continue;
            }
            if (!mentionsObject)
                continue;
            if (inputPassThroughOpcodes.contains(operation.opcode())) {
                names.quickSet(operation.id().index());
                continue;
            }
            if (mentionsHolder) {
                bool isStoredInHolder = operation.opcode() == Opcode::PropSet && operation.key() && isHolder(operation.objectOperand()) && !isObject(operation.objectOperand()) && isObject(operation.valueOperand());
                if (!isStoredInHolder || keyInHolder || isHolderShown || block.hasHandler() || ofHolder->mayRunAccessor(operation.key()) || !isEnteredByNewAlone(caller))
                    return std::nullopt;
                keyInHolder = operation.key();
                continue;
            }
            if (isReadForCall(operation)) {
                m_lookedUp.add(operation.key().bits());
                continue;
            }
            if (storesField(operation))
                continue;
            return addStored(findFilledByCall(operation));
        }
        if (hasStored)
            return stored;
        if (keyInHolder || passesObject(block))
            return std::nullopt;

        BlockID blockOfConstruction = index.blockOf(construction);
        BlockID blockOfCall;
        std::optional<Fields> filled;
        for (BlockID number : index.blocksInDominatorOrder()) {
            if (number == blockOfConstruction || !index.dominates(blockOfConstruction, number) || (blockOfCall && index.dominates(blockOfCall, number)))
                continue;
            BodyBlock below = index.block(number);
            for (BodyValue operation : below.values()) {
                bool mentionsObject = false;
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    mentionsObject |= isObject(operand);
                });
                if (!mentionsObject)
                    continue;
                if (inputPassThroughOpcodes.contains(operation.opcode())) {
                    names.quickSet(operation.id().index());
                    continue;
                }
                if (blockOfCall)
                    return std::nullopt;
                if (isReadForCall(operation)) {
                    m_lookedUp.add(operation.key().bits());
                    continue;
                }
                filled = findFilledByCall(operation);
                if (!filled)
                    return std::nullopt;
                blockOfCall = number;
                break;
            }
            if (blockOfCall != number && passesObject(below))
                return std::nullopt;
        }
        return filled;
    }

    Fields everyField() const
    {
        Fields fields;
        fields.words.fill(~0ull, m_numberOfWords);
        return fields;
    }

    bool testsKeepObject() const
    {
        Vector<FunctionID, 8> bodies;
        size_t stepsLeft = m_program.layouts.size();
        for (LayoutID current = m_made; current && stepsLeft; current = m_program.at(current).base, --stepsLeft) {
            if (FunctionID constructor = m_program.at(current).constructor)
                bodies.append(constructor);
        }
        for (size_t next = 0; next < bodies.size(); ++next) {
            const ThinSummary& summary = m_flow.summaries.at(bodies[next]);
            for (const SummaryConstruction& construction : summary.constructions) {
                if (construction.isTested && !onlyTestsThisUntil(bodies[next], construction.lastTest))
                    return false;
                for (const SummaryConstruction::Part& part : summary.partsOfConstructions.span().subspan(construction.firstPart, construction.numberOfParts)) {
                    if (!bodies.contains(part.function))
                        bodies.append(part.function);
                }
            }
        }
        return true;
    }

private:
    std::optional<Fields> findFilledAfter(FunctionID constructor, ValueID object)
    {
        Vector<std::pair<FunctionID, ValueID>, 8> waiting { { constructor, object } };
        while (!waiting.isEmpty()) {
            auto [id, objectOfBody] = waiting.last();
            auto added = m_bodies.add(id.bits(), beingFollowed);
            if (added.iterator->value == beingFollowed) {
                Vector<std::pair<FunctionID, ValueID>, 8> methods;
                Effects effects = followBody(id, objectOfBody, id == constructor, added.isNewEntry ? &methods : nullptr);
                if (!methods.isEmpty()) {
                    waiting.appendVector(methods);
                    continue;
                }
                added.iterator->value = m_effects.size();
                m_effects.append(WTF::move(effects));
            }
            waiting.removeLast();
        }

        const Effects& effects = m_effects[m_bodies.get(constructor.bits())];
        if (!effects.keepsObject || std::ranges::any_of(m_lookedUp, [&](uint32_t key) { return mayRunAccessor(PropertyKeyID::fromBits(key)); }))
            return std::nullopt;
        m_readEmpty = effects.readEmpty;
        Fields filled;
        filled.words.fill(effects.fillsEveryField ? ~0ull : 0, m_numberOfWords);
        for (uint32_t word = 0; word < effects.filled.words.size(); ++word)
            filled.words[effects.filled.firstWord + word] |= effects.filled.words[word];
        for (uint32_t word = 0; word < effects.readEmpty.words.size(); ++word)
            filled.words[effects.readEmpty.firstWord + word] &= ~effects.readEmpty.words[word];
        return filled;
    }

    struct Effects {
        Fields filled;
        Fields readEmpty;
        bool keepsObject { false };
        bool returnsObject { false };
        bool fillsEveryField { false };
    };

    class FilledFields {
    public:
        FilledFields(const BodyIndex& index, size_t numberOfWords, const DecidedPathsInLinkLayouts* ways)
            : m_index(index)
            , m_ways(ways)
        {
            m_filled.fill(0, numberOfWords);
            m_filledAtPoints.fill(0, numberOfWords);
            m_readEmpty.fill(0, numberOfWords);
            m_filledAtEnds.resize(index.numberOfBlocks());
            if (ways)
                return;
            m_entering.resize(index.numberOfBlocks());
            for (BlockID block : index.blocksInDominatorOrder()) {
                for (BodyTarget target : index.block(block).terminator().targets())
                    m_entering[target.block().index()].append(block);
            }
        }

        void followJoins()
        {
            m_joins.resize(m_index.numberOfBlocks());
            for (BlockID number : m_index.blocksInDominatorOrder()) {
                for (BodyTarget target : m_index.block(number).terminator().targets())
                    ++m_joins[target.block().index()].numberOfEdges;
            }
        }

        void leaveBlock()
        {
            if (m_joins.isEmpty())
                return;
            for (BodyTarget target : m_index.block(m_block).terminator().targets()) {
                Join& join = m_joins[target.block().index()];
                if (join.numberOfEdges < 2)
                    continue;
                if (!join.numberOfEdgesFollowed++)
                    join.filled.fill(~0ull, m_filled.size());
                for (size_t word = 0; word < m_filled.size() && !m_everyFieldFrom; ++word)
                    join.filled[word] &= m_filled[word];
            }
        }

        void enterBlock(BlockID block)
        {
            if (m_block)
                m_filledAtEnds[m_block.index()] = m_everyFieldFrom ? Vector<uint64_t, 2>(FillWith { }, m_filled.size(), ~0ull) : m_filled;
            while (!m_changes.isEmpty() && !dominates(m_changes.last().block, block)) {
                Change change = m_changes.takeLast();
                m_filled[change.word] = change.before;
            }
            if (m_everyFieldFrom && !dominates(m_everyFieldFrom, block))
                m_everyFieldFrom = { };
            m_block = block;
            addWhatEveryArmFills();
            const Join* join = m_joins.isEmpty() ? nullptr : &m_joins[block.index()];
            if (!join || join->numberOfEdges < 2 || join->numberOfEdgesFollowed != join->numberOfEdges)
                return;
            for (size_t word = 0; word < m_filled.size(); ++word)
                addWord(word, join->filled[word]);
        }

        BlockID block() const { return m_block; }

        bool contains(uint32_t field) const { return m_everyFieldFrom || m_filled[field / 64] >> field % 64 & 1; }
        void add(uint32_t field) { addWord(field / 64, 1ull << field % 64); }
        void noteRead(uint32_t field) { noteReadOfWord(field / 64, 1ull << field % 64); }

        void addEveryField()
        {
            if (!m_everyFieldFrom)
                m_everyFieldFrom = m_block;
        }

        void add(const Effects& effects)
        {
            for (uint32_t word = 0; word < effects.readEmpty.words.size(); ++word)
                noteReadOfWord(effects.readEmpty.firstWord + word, effects.readEmpty.words[word]);
            for (uint32_t word = 0; word < effects.filled.words.size(); ++word)
                addWord(effects.filled.firstWord + word, effects.filled.words[word]);
            if (effects.fillsEveryField)
                addEveryField();
        }

        void notePoint()
        {
            m_hasPoint = true;
            if (m_everyFieldFrom)
                return;
            for (size_t word = 0; word < m_filled.size(); ++word)
                m_filledAtPoints[word] = m_filled[word] & (m_hasPointWithoutEveryField ? m_filledAtPoints[word] : ~0ull);
            m_hasPointWithoutEveryField = true;
        }

        void finish(Effects& effects)
        {
            effects.readEmpty = withoutEmptyEnds(m_readEmpty);
            effects.filled = withoutEmptyEnds(m_filledAtPoints);
            effects.fillsEveryField = m_hasPoint && !m_hasPointWithoutEveryField;
        }

    private:
        struct Change {
            BlockID block;
            uint32_t word;
            uint64_t before;
        };
        struct Join {
            uint32_t numberOfEdges { 0 };
            uint32_t numberOfEdgesFollowed { 0 };
            Vector<uint64_t, 2> filled;
        };

        bool dominates(BlockID from, BlockID to) const { return m_ways ? m_ways->dominates(from, to) : m_index.dominates(from, to); }

        void addWhatEveryArmFills()
        {
            const Vector<BlockID, 2>& arms = m_ways ? m_ways->entering()[m_block.index()] : m_entering[m_block.index()];
            if (arms.size() < 2 || m_everyFieldFrom)
                return;
            Vector<uint64_t, 2> filled(FillWith { }, m_filled.size(), ~0ull);
            bool isEntered = false;
            for (BlockID arm : arms) {
                if (dominates(m_block, arm))
                    continue;
                const Vector<uint64_t, 2>& atEnd = m_filledAtEnds[arm.index()];
                if (atEnd.isEmpty())
                    return;
                isEntered = true;
                for (size_t word = 0; word < filled.size(); ++word)
                    filled[word] &= atEnd[word];
            }
            for (uint32_t word = 0; isEntered && word < filled.size(); ++word)
                addWord(word, filled[word]);
        }

        void addWord(uint32_t word, uint64_t bits)
        {
            if (m_everyFieldFrom || !(bits & ~m_filled[word]))
                return;
            m_changes.append({ m_block, word, m_filled[word] });
            m_filled[word] |= bits;
        }

        void noteReadOfWord(uint32_t word, uint64_t bits)
        {
            if (!m_everyFieldFrom)
                m_readEmpty[word] |= bits & ~m_filled[word];
        }

        static Fields withoutEmptyEnds(const Vector<uint64_t, 2>& words)
        {
            size_t first = 0;
            size_t end = words.size();
            while (first < end && !words[first])
                ++first;
            while (end > first && !words[end - 1])
                --end;
            return { static_cast<uint32_t>(first), words.subspan(first, end - first) };
        }

        const BodyIndex& m_index;
        Vector<uint64_t, 2> m_filled;
        Vector<uint64_t, 2> m_filledAtPoints;
        Vector<uint64_t, 2> m_readEmpty;
        Vector<Vector<uint64_t, 2>> m_filledAtEnds;
        Vector<Vector<BlockID, 2>> m_entering;
        Vector<Change, 16> m_changes;
        Vector<Join> m_joins;
        const DecidedPathsInLinkLayouts* m_ways;
        BlockID m_block;
        BlockID m_everyFieldFrom;
        bool m_hasPoint { false };
        bool m_hasPointWithoutEveryField { false };
    };

    ValueID findNewObject(const BodyIndex& index, const Function& constructor) const
    {
        if (constructor.kind == FunctionKind::Plain) {
            std::optional<unsigned> parameter = constructor.thisParameterIndex();
            return parameter ? index.block(entryBlock).parameter(*parameter) : ValueID();
        }
        if (constructor.kind != FunctionKind::BaseConstructor)
            return { };
        for (BodyValue operation : index.block(entryBlock).values()) {
            if (operation.opcode() == Opcode::NewObject && operation.as<Opcode::NewObject>().layout() == m_made)
                return operation.id();
        }
        return { };
    }

    FunctionID findMethod(FunctionID caller, ValueID site, bool& isUnknownYet) const
    {
        const ThinSummary& summary = m_flow.summaries.at(caller);
        auto call = std::ranges::lower_bound(summary.calls, site, { }, &SummaryCall::site);
        if (call == summary.calls.end() || call->site != site)
            return { };
        const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[caller.index()] + (call - summary.calls.begin())];
        if (callees.mayEnterUnseen || callees.mayEnterLost || !callees.builtins().isEmpty() || !callees.shared().isEmpty() || callees.listed().size() > 1)
            return { };
        isUnknownYet = callees.listed().isEmpty();
        return isUnknownYet ? FunctionID() : callees.listed()[0];
    }

    FunctionID findOnlyMethod(FunctionID caller, ValueID site, bool& isUnknownYet) const
    {
        const ThinSummary& summary = m_flow.summaries.at(caller);
        auto call = std::ranges::find_if(summary.calls, [&](const SummaryCall& listed) {
            return listed.site == site && listed.callee == SummaryCall::Callee::Name && !listed.isMadeByProtocol;
        });
        if (call == summary.calls.end())
            return { };
        FunctionID method;
        auto takesCalleesOf = [&](const LinkFlow::CalleesOfCall& callees) {
            return !callees.mayEnterUnseen && !callees.mayEnterLost && callees.builtins().isEmpty() && std::ranges::all_of(callees.listed(), [&](FunctionID callee) {
                bool isOnly = !method || method == callee;
                method = callee;
                return isOnly;
            });
        };
        const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[caller.index()] + (call - summary.calls.begin())];
        bool takesEveryCallee = takesCalleesOf(callees) && std::ranges::all_of(callees.shared(), [&](uint32_t shared) {
            const LinkFlow::CalleesOfCall& ofShared = m_flow.calleesOfSharedCallsAsSettled()[shared];
            return ofShared.shared().isEmpty() && takesCalleesOf(ofShared);
        });
        isUnknownYet = takesEveryCallee && !method;
        return takesEveryCallee ? method : FunctionID();
    }

    FunctionID findForwardedMethod(FunctionID caller, ValueID site, bool& isUnknownYet) const
    {
        const ThinSummary& summary = m_flow.summaries.at(caller);
        auto call = std::ranges::lower_bound(summary.calls, site, { }, &SummaryCall::site);
        if (call == summary.calls.end() || call->site != site || call->callee != SummaryCall::Callee::Name || call->isConstruction || call->isMadeByProtocol || !call->receiver || call->receiver == noSummaryNode)
            return { };
        const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[caller.index()] + (call - summary.calls.begin())];
        const BuiltinTable& table = m_program.builtins();
        bool entersOthers = std::ranges::any_of(callees.builtins(), [&](BuiltinID entered) {
            BuiltinID builtin = table.canonicalID(entered);
            return builtin != table.canonicalID(Builtins::FunctionPrototypeApply) && builtin != table.canonicalID(Builtins::FunctionPrototypeCall);
        });
        if (entersOthers || callees.mayEnterUnseen || callees.mayEnterLost || !callees.shared().isEmpty() || callees.listed().size() > 1)
            return { };
        isUnknownYet = callees.listed().isEmpty();
        if (isUnknownYet || callees.builtins().isEmpty())
            return { };
        LinkType receiver = m_flow.typeAt(m_flow.nodeOf(caller, call->receiver));
        receiver.primitives = 0;
        FunctionID entered = callees.listed()[0];
        return receiver == typeOfClosuresInLinkLayouts(entered) && !m_flow.mayBeOnClosuresOf(entered, call->key) ? entered : FunctionID();
    }

    static void addNarrowedNames(const BodyIndex& index, const BodyBlock& block, BitVector& names)
    {
        for (uint32_t position = 0; position < block.numberOfParameters(); ++position) {
            if (ValueID narrowed = index.valueNarrowedInto(block.parameter(position)); narrowed && names.quickGet(narrowed.index()))
                names.quickSet(block.parameter(position).index());
        }
    }

    bool onlyTestsInstances(FunctionID caller, uint32_t numberOfCall) const
    {
        const BuiltinTable& table = m_program.builtins();
        auto entersTestAlone = [&](const auto& callees) {
            return !callees.mayEnterUnseen && !callees.mayEnterLost && callees.listed().isEmpty() && std::ranges::all_of(callees.builtins(), [&](BuiltinID builtin) {
                return table.canonicalID(builtin) == table.canonicalID(Builtins::FunctionPrototypeSymbolHasInstance);
            });
        };
        const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[caller.index()] + numberOfCall];
        return entersTestAlone(callees) && std::ranges::all_of(callees.shared(), [&](uint32_t shared) {
            return entersTestAlone(m_flow.calleesOfSharedCallsAsSettled()[shared]);
        });
    }

    std::optional<uint32_t> findOnlyCallAt(FunctionID caller, ValueID site) const
    {
        const ThinSummary& summary = m_flow.summaries.at(caller);
        auto call = std::ranges::lower_bound(summary.calls, site, { }, &SummaryCall::site);
        if (call == summary.calls.end() || call->site != site || (call + 1 != summary.calls.end() && (call + 1)->site == site))
            return std::nullopt;
        return static_cast<uint32_t>(call - summary.calls.begin());
    }

    bool onlyTestsInstanceAt(FunctionID caller, const BodyValue& test) const
    {
        std::optional<uint32_t> number = findOnlyCallAt(caller, test.id());
        if (!number)
            return false;
        const SummaryCall& call = m_flow.summaries.at(caller).calls[*number];
        return call.callee == SummaryCall::Callee::Name && call.isMadeByProtocol && onlyTestsInstances(caller, *number);
    }

    bool onlyTestsParameter(FunctionID id, uint32_t position) const
    {
        const Function& function = m_program.at(id);
        const ThinSummary& summary = m_flow.summaries.at(id);
        if (!function.hasBody() || function.kind != FunctionKind::Plain || function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }) || summary.has(SummaryTrait::HasDirectEval) || summary.has(SummaryTrait::UsesArguments)
            || summary.has(SummaryTrait::Suspends) || summary.has(SummaryTrait::OnlyPassesOnGatheredArguments) || position >= function.numberOfPositionalParameters())
            return false;
        const BodyIndex* found = bodiesLookedAt(m_flow.looks).find(id);
        if (!found)
            return false;
        const BodyIndex& index = *found;
        BitVector names(index.numberOfValues());
        names.quickSet(index.block(entryBlock).parameter(function.firstParameterIndex() + position).index());
        auto isParameter = [&](ValueID value) {
            return value && names.quickGet(value.index());
        };
        for (BlockID number : index.blocksInDominatorOrder()) {
            BodyBlock block = index.block(number);
            addNarrowedNames(index, block, names);
            for (BodyValue operation : block.values()) {
                bool mentionsParameter = false;
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    mentionsParameter |= isParameter(operand);
                });
                if (!mentionsParameter || operation.opcode() == Opcode::TypeTest || operation.opcode() == Opcode::ToBoolean)
                    continue;
                if (inputPassThroughOpcodes.contains(operation.opcode())) {
                    names.quickSet(operation.id().index());
                    continue;
                }
                if (operation.opcode() != Opcode::InstanceOf || isParameter(operation.operand(1)) || !onlyTestsInstanceAt(id, operation))
                    return false;
            }
            bool mentionsParameter = false;
            BodyTerminator terminator = block.terminator();
            terminator.forEachOperand([&](ValueID operand, uint32_t) {
                mentionsParameter |= isParameter(operand);
            });
            block.forEachOutgoingEdge([&](BodyTarget target, bool) {
                for (ValueID argument : target.arguments())
                    mentionsParameter |= isParameter(argument);
            });
            if (mentionsParameter)
                return false;
        }
        return true;
    }

    bool onlyTestsObject(FunctionID caller, const BodyValue& operation, const Invocable<bool(ValueID)> auto& isObject) const
    {
        if (operation.opcode() == Opcode::InstanceOf)
            return !isObject(operation.operand(1)) && onlyTestsInstanceAt(caller, operation);
        std::optional<uint32_t> number = findOnlyCallAt(caller, operation.id());
        if (!number || operation.opcode() != Opcode::Call)
            return false;
        const SummaryCall& call = m_flow.summaries.at(caller).calls[*number];
        if (call.isConstruction || call.passesList || (call.callee != SummaryCall::Callee::Value && call.callee != SummaryCall::Callee::Binding))
            return false;
        auto written = operation.as<Opcode::Call>();
        std::span<const ValueID> arguments = written.argumentsOperands();
        auto handed = std::ranges::find_if(arguments, isObject);
        if (handed == arguments.end() || isObject(written.calleeOperand()) || isObject(written.thisValueOperand()) || std::ranges::count_if(arguments, isObject) != 1)
            return false;
        const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[caller.index()] + *number];
        return !callees.mayEnterUnseen && !callees.mayEnterLost && callees.builtins().isEmpty() && callees.shared().isEmpty() && std::ranges::all_of(callees.listed(), [&](FunctionID callee) {
            return onlyTestsParameter(callee, static_cast<uint32_t>(handed - arguments.begin()));
        });
    }

    bool onlyTestsThisUntil(FunctionID id, uint32_t lastTest) const
    {
        const BodyIndex* found = bodiesLookedAt(m_flow.looks).find(id);
        std::optional<unsigned> parameter = m_program.at(id).thisParameterIndex();
        if (!found || !parameter)
            return false;
        const BodyIndex& index = *found;
        BitVector names(index.numberOfValues());
        ValueID object = index.block(entryBlock).parameter(*parameter);
        names.quickSet(object.index());
        for (const OtherNameOfObjectBeingMade& other : m_flow.summaries.at(id).otherNamesOfObjectsBeingMade) {
            if (other.object == object)
                names.quickSet(other.parameter.index());
        }
        auto isObject = [&](ValueID value) {
            return value && names.quickGet(value.index());
        };
        for (BlockID number : index.blocksInDominatorOrder()) {
            BodyBlock block = index.block(number);
            addNarrowedNames(index, block, names);
            for (BodyValue operation : block.values()) {
                bool mentionsObject = false;
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    mentionsObject |= isObject(operand);
                });
                if (!mentionsObject || operation.id().index() > lastTest)
                    continue;
                if (inputPassThroughOpcodes.contains(operation.opcode()) || operation.opcode() == Opcode::ToThis) {
                    names.quickSet(operation.id().index());
                    continue;
                }
                auto isPassedToFunction = [&](const BodyValue& call) {
                    auto written = call.as<Opcode::Call>();
                    ValueID callee = written.calleeOperand();
                    while (callee && !index.isParameter(callee) && inputPassThroughOpcodes.contains(index.opcodeOf(callee)))
                        callee = index.value(callee).operand(0);
                    if (!callee || index.isParameter(callee) || std::ranges::none_of(written.argumentsOperands(), isObject))
                        return false;
                    Opcode kept = index.opcodeOf(callee);
                    return kept == Opcode::GetModuleVariable || kept == Opcode::GetEnvironmentField || kept == Opcode::NewClosure;
                };
                bool isTest = operation.opcode() == Opcode::InstanceOf || (operation.opcode() == Opcode::Call && isPassedToFunction(operation));
                if (isTest && !onlyTestsObject(id, operation, isObject))
                    return false;
            }
        }
        return true;
    }

    bool namesFunctionItself(const BodyIndex& index, FunctionID function, ValueID value) const
    {
        ValueID named = index.stripCasts(value);
        if (index.isParameter(named)) {
            std::optional<unsigned> callee = m_program.at(function).calleeParameterIndex();
            return callee && named == index.block(entryBlock).parameter(*callee);
        }
        if (index.opcodeOf(named) == Opcode::GetModuleVariable) {
            const Variable& variable = m_program.at(index.value(named).variable());
            return variable.initialization == VariableInitialization::Function && variable.function == function && !variable.owner && variable.module && variable.flags.contains(VariableFlags::IsImmutable)
                && !variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun }) && loaderEntersBodyAtMostOnce(m_program.at(variable.module));
        }
        if (index.opcodeOf(named) != Opcode::GetEnvironmentField)
            return false;
        auto read = index.value(named).as<Opcode::GetEnvironmentField>();
        std::optional<uint32_t> capture = index.block(entryBlock).indexOfParameter(index.stripCasts(read.environmentOperand()));
        if (!capture || *capture >= m_program.at(function).captures.size())
            return false;
        return scopeThatCaptureTakes(m_program, function, *capture) == read.layout() && functionDeclaredInScopeSlot(m_program, read.layout(), read.field()) == function;
    }

    void decideQuestionsAboutConstructor(FunctionID constructor, const BodyIndex& index, ValueID object, DecidedPathsInLinkLayouts& ways) const
    {
        auto isObject = [&](ValueID value) {
            while (!index.isParameter(value) && (inputPassThroughOpcodes.contains(index.opcodeOf(value)) || index.opcodeOf(value) == Opcode::ToThis))
                value = index.value(value).operand(0);
            return value == object;
        };
        auto findReadOfConstructor = [&](ValueID value) -> ValueID {
            value = index.stripCasts(value);
            if (index.isParameter(value) || index.opcodeOf(value) != Opcode::PropGet || index.value(value).isFreeName())
                return { };
            auto read = index.value(value).as<Opcode::PropGet>();
            return read.key() == m_program.keyOf(BuiltinNames::constructor) && isObject(read.objectOperand()) ? value : ValueID();
        };
        for (BlockID number : index.blocksInDominatorOrder()) {
            BodyTerminator terminator = index.block(number).terminator();
            if (!terminator.isConditionalBranch() || index.loopOf(number))
                continue;
            ValueID condition = terminator.operand(0);
            uint32_t target = 0;
            while (!index.isParameter(condition) && (index.opcodeOf(condition) == Opcode::ToBoolean || index.opcodeOf(condition) == Opcode::LogicalNot)) {
                target ^= index.opcodeOf(condition) == Opcode::LogicalNot;
                condition = index.value(condition).operand(0);
            }
            if (index.isParameter(condition) || index.opcodeOf(condition) != Opcode::StrictEq)
                continue;
            BodyValue equality = index.value(condition);
            for (uint32_t side : { 0u, 1u }) {
                ValueID asked = findReadOfConstructor(equality.operand(side));
                if (!asked || !namesFunctionItself(index, constructor, equality.operand(1 - side)))
                    continue;
                ways.decide(number, target, asked);
                break;
            }
        }
    }

    bool mayRunAccessor(PropertyKeyID key) const
    {
        if (m_flow.namesMayBeIncomplete(m_made, ClassSide::Prototype) || m_program.nameClassOf(key) == NameClass::Proto)
            return true;
        const PropertyKey& record = m_program.at(key);
        if (record.kind == PropertyKeyKind::Text && m_program.isOpenedSomewhere(record.text))
            return true;
        if (std::optional<FieldReference> field = findField(m_program, m_made, key); field && fieldAt(m_program, *field).form.contains(FieldForm::IsAccessor))
            return true;
        for (const LinkFlow::FoundMember& member : m_flow.findMembers(m_made, ClassSide::Prototype, key)) {
            if (member.kind != MethodKind::Method)
                return true;
        }
        return m_flow.mayHoldAccessorOnPrototype(m_made, key);
    }

    bool nothingLiesAbove(PropertyKeyID key) const
    {
        return m_flow.isAbsentFromObjectPrototype(key) && !m_flow.findMembers(m_made, ClassSide::Prototype, key).mayAnswer();
    }

    bool callsMethodOfObject(const BodyIndex& index, const BodyValue& call, const Invocable<bool(ValueID)> auto& isObject) const
    {
        auto written = call.as<Opcode::Call>();
        ValueID callee = written.calleeOperand();
        if (!isObject(written.thisValueOperand()) || !callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::PropGet || std::ranges::any_of(written.argumentsOperands(), isObject))
            return false;
        auto read = index.value(callee).template as<Opcode::PropGet>();
        return isObject(read.objectOperand()) && read.key() && !index.value(callee).isFreeName() && !findField(m_program, m_made, read.key());
    }

    Effects followBody(FunctionID id, ValueID object, bool isConstructor, Vector<std::pair<FunctionID, ValueID>, 8>* methodsToFollow)
    {
        Effects effects;
        const BodyIndex* found = bodiesLookedAt(m_flow.looks).find(id);
        if (!found || m_program.at(id).isCoroutine() || m_flow.summaries.at(id).has(SummaryTrait::HasDirectEval))
            return effects;
        const BodyIndex& index = *found;

        BitVector names(index.numberOfValues());
        names.quickSet(object.index());
        auto isObject = [&](ValueID value) {
            return value && names.quickGet(value.index());
        };
        const DecidedPathsInLinkLayouts* ways = isConstructor ? m_ways : nullptr;
        FilledFields filled(index, m_numberOfWords, ways);
        if (!ways && index.numberOfBlocks() > 3 && !index.hasIrreducibleFlow())
            filled.followJoins();
        auto notePoint = [&](uint32_t site) {
            m_showsObject |= site != UINT32_MAX;
            m_objectKeepsConstructor &= !ways || ways->fallThroughBefore(filled.block(), site);
            filled.notePoint();
        };
        auto callsMethod = [&](const BodyValue& call) {
            return callsMethodOfObject(index, call, isObject);
        };
        auto forwardsObject = [&](const BodyValue& call) {
            auto written = call.as<Opcode::Call>();
            ValueID callee = written.calleeOperand();
            std::span<const ValueID> arguments = written.argumentsOperands();
            if (!written.thisValueOperand() || isObject(written.thisValueOperand()) || arguments.empty() || !isObject(arguments[0]) || std::ranges::any_of(arguments.subspan(1), isObject))
                return false;
            if (!callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::PropGet || index.value(callee).isFreeName())
                return false;
            auto read = index.value(callee).as<Opcode::PropGet>();
            bool namesForwarding = read.key() && (read.key() == m_program.keyOf(BuiltinNames::call) || read.key() == m_program.keyOf(BuiltinNames::apply));
            return namesForwarding && index.stripCasts(read.objectOperand()) == index.stripCasts(written.thisValueOperand());
        };
        for (BlockID number : ways ? ways->blocksInDominatorOrder() : index.blocksInDominatorOrder()) {
            BodyBlock block = index.block(number);
            if (block.hasHandler())
                return effects;
            filled.enterBlock(number);
            addNarrowedNames(index, block, names);
            for (BodyValue operation : block.values()) {
                Opcode opcode = operation.opcode();
                bool mentionsObject = false;
                operation.forEachOperand([&](ValueID operand, uint32_t) {
                    mentionsObject |= isObject(operand);
                });
                if (!mentionsObject)
                    continue;
                if (inputPassThroughOpcodes.contains(opcode) || opcode == Opcode::ToThis) {
                    names.quickSet(operation.id().index());
                    continue;
                }
                if (opcode == Opcode::TypeTest || opcode == Opcode::ToBoolean)
                    continue;
                if ((opcode == Opcode::InstanceOf || opcode == Opcode::Call) && onlyTestsObject(id, operation, isObject))
                    continue;
                if (opcode == Opcode::PropGet && !operation.isFreeName() && operation.key()) {
                    m_lookedUp.add(operation.key().bits());
                    ValueID user = index.onlyUsingValue(operation.id());
                    bool isRead = !user || index.opcodeOf(user) != Opcode::Call || !callsMethod(index.value(user));
                    if (std::optional<FieldReference> field = isRead ? findField(m_program, m_made, operation.key()) : std::nullopt)
                        filled.noteRead(field->field);
                    continue;
                }
                if (opcode == Opcode::PropSet && !operation.isFreeName() && operation.key() && !isObject(operation.valueOperand())) {
                    m_lookedUp.add(operation.key().bits());
                    m_objectKeepsConstructor &= operation.key() != m_program.keyOf(BuiltinNames::constructor);
                    if (std::optional<FieldReference> field = findField(m_program, m_made, operation.key()); field && !filled.contains(field->field) && nothingLiesAbove(operation.key()))
                        filled.add(field->field);
                    continue;
                }
                FunctionID method;
                ValueID objectInMethod;
                bool isUnknownYet = false;
                bool isForwarded = opcode == Opcode::Call && forwardsObject(operation);
                if (isForwarded || (opcode == Opcode::Call && callsMethod(operation))) {
                    method = isForwarded ? findForwardedMethod(id, operation.id(), isUnknownYet) : findMethod(id, operation.id(), isUnknownYet);
                    if (const Function* entered = method ? &m_program.at(method) : nullptr; entered && (entered->kind == FunctionKind::Plain || entered->kind == FunctionKind::Method)) {
                        std::optional<unsigned> parameter = entered->thisParameterIndex();
                        const BodyIndex* body = bodiesLookedAt(m_flow.looks).find(method);
                        if (!parameter && body)
                            continue;
                        objectInMethod = parameter && body ? body->block(entryBlock).parameter(*parameter) : ValueID();
                    }
                } else if (opcode == Opcode::CallDirect && m_program.at(operation.as<Opcode::CallDirect>().function()).kind == FunctionKind::Initializer) {
                    auto direct = operation.as<Opcode::CallDirect>();
                    method = direct.function();
                    std::optional<unsigned> parameter = m_program.at(method).thisParameterIndex();
                    const BodyIndex* body = bodiesLookedAt(m_flow.looks).find(method);
                    std::span<const ValueID> arguments = direct.argumentsOperands();
                    bool isHandedOnce = parameter && *parameter < arguments.size() && isObject(arguments[*parameter]) && std::ranges::count_if(arguments, isObject) == 1;
                    objectInMethod = isHandedOnce && body ? body->block(entryBlock).parameter(*parameter) : ValueID();
                }
                if (isUnknownYet) {
                    filled.addEveryField();
                    continue;
                }
                auto body = objectInMethod ? m_bodies.find(method.bits()) : m_bodies.end();
                if (objectInMethod && methodsToFollow && body == m_bodies.end()) {
                    methodsToFollow->append({ method, objectInMethod });
                    continue;
                }
                if (const Effects* called = body == m_bodies.end() || body->value == beingFollowed ? nullptr : &m_effects[body->value]; called && called->keepsObject && (!called->returnsObject || index.isUnused(operation.id()))) {
                    m_methods.append(method);
                    filled.add(*called);
                    continue;
                }
                if (!isConstructor)
                    return effects;
                notePoint(operation.id().index());
            }
            BodyTerminator terminator = block.terminator();
            bool mentionsObject = false;
            terminator.forEachOperand([&](ValueID operand, uint32_t) {
                mentionsObject |= isObject(operand);
            });
            bool returns = terminator.kind() == TerminatorKind::Return || terminator.kind() == TerminatorKind::ReturnCall;
            for (BodyTarget target : terminator.targets()) {
                for (ValueID argument : target.arguments())
                    mentionsObject |= isObject(argument);
            }
            filled.leaveBlock();
            if (!mentionsObject && !returns)
                continue;
            if (ValueID returned = isConstructor && !mentionsObject && terminator.kind() == TerminatorKind::Return ? terminator.as<TerminatorKind::Return>().valueOperand() : ValueID(); returned && (returned = index.stripCasts(returned)) && !index.isParameter(returned) && index.opcodeOf(returned) == Opcode::Construct)
                continue;
            if (mentionsObject && terminator.kind() != TerminatorKind::Return && !isConstructor)
                return effects;
            effects.returnsObject |= mentionsObject;
            notePoint(UINT32_MAX);
        }
        effects.keepsObject = true;
        filled.finish(effects);
        return effects;
    }

    const LinkFlow& m_flow;
    const Program& m_program;
    LayoutID m_made;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_lookedUp;
    Vector<FunctionID, 8> m_methods;
    static constexpr uint32_t beingFollowed = UINT32_MAX;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_bodies;
    Vector<Effects, 4> m_effects;
    size_t m_numberOfWords;
    const DecidedPathsInLinkLayouts* m_ways { nullptr };
    bool m_objectKeepsConstructor { true };
    Fields m_readEmpty;
    bool m_showsObject { false };
};

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
    bool testsObject() const { return m_testsObject; }
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

        m_testsObject |= construction->isTested;
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
            bool isParent = m_program.at(part.function).kind != FunctionKind::Initializer;
            bool isStillUnseen = (!isParent || m_flow.answersOwnThis(part.function)) && addWhatBodyFills(part.function, SummaryConstruction::Object::This, isParent);
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
            bool isParent = m_program.at(part.function).kind != FunctionKind::Initializer;
            if (!isParent || m_flow.answersOwnThis(part.function))
                addWhatBodyFills(part.function, SummaryConstruction::Object::This, isParent);
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
        for (LayoutID current = m_flow.keysFilledOnObjectsBeingMade.get(key.index()) ? m_made : LayoutID(); current; current = m_program.at(current).base) {
            FunctionID constructor = m_program.at(current).constructor;
            if (constructor && std::ranges::any_of(m_flow.summaries.at(constructor).changes, [&](const SummaryChange& change) { return change.fillsObjectBeingMade && change.key == key; }))
                return Found::Accessor;
        }
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
    bool m_testsObject { false };
    Vector<PropertyKeyID, 32> m_filled;
    Vector<PropertyKeyID, 32> m_lookedUp;
    Vector<PropertyKeyID, 32> m_keysThatHoldObject;
    std::span<const PropertyKeyID> m_keysThatHoldObjectAbove;
};

} // anonymous namespace

bool LinkFlow::ignoresThis(FunctionID id) const
{
    const Function& function = program.at(id);
    const ThinSummary& summary = summaries.at(id);
    return function.hasBody() && !function.isCoroutine() && !summary.has(SummaryTrait::HasDirectEval) && answersNothing(id)
        && std::ranges::none_of(summary.nodes, [&](const SummaryNode& node) { return node.kind == SummaryNode::Kind::Parameter && function.thisParameterIndex() == node.first; });
}

std::optional<bool> LinkFlow::onlyForwardsThis(LayoutID made) const
{
    FunctionID constructor = program.at(made).constructor;
    const ThinSummary& summary = summaries.at(constructor);
    if (!summary.has(SummaryTrait::OnlyPassesOnGatheredArguments) || summary.has(SummaryTrait::HasDirectEval) || !answersNothing(constructor) || summary.calls.size() != 1 || !summary.changes.isEmpty() || !summary.copiesOfMembers.isEmpty())
        return false;
    auto isThis = [&](uint32_t node) { return summary.nodes[node].kind == SummaryNode::Kind::Parameter && program.at(constructor).thisParameterIndex() == summary.nodes[node].first; };
    auto namesThis = [&](uint32_t node) {
        if (isThis(node))
            return true;
        return summary.nodes[node].kind == SummaryNode::Kind::Join && std::ranges::any_of(summary.edges, [&](const SummaryEdge& edge) { return edge.to == node && isThis(edge.from); });
    };
    const SummaryCall& call = summary.calls[0];
    if (!call.appliesGatheredArguments || call.receiver == noSummaryNode || summary.nodes[call.receiver].kind != SummaryNode::Kind::ResultOfAccess || !call.numberOfArguments || !namesThis(summary.arguments[call.firstArgument]))
        return false;
    for (const SummaryEdge& edge : summary.edges) {
        if (isThis(edge.from) ? !namesThis(edge.to) : namesThis(edge.from))
            return false;
    }
    for (const SummaryAccess& access : summary.accesses) {
        bool readsArguments = access.isToContents() && !namesThis(access.subject);
        if (access.kind != SummaryAccess::Kind::Read || (!access.key && !readsArguments) || (access.isToContents() && !readsArguments))
            return false;
    }
    const SummaryAccess& read = summary.accesses[summary.nodes[call.receiver].first];
    const NameOnPrototype* name = findField(program, made, read.key) ? nullptr : findNameOnPrototype(made, read.key);
    if (typeAt(nodeOf(constructor, call.receiver)).isNever())
        return std::nullopt;
    if (!name)
        return false;
    LinkType held = typeAt(name->stored);
    LinkType alone;
    alone.functions = LinkType::Functions::Closure;
    alone.function = held.function;
    alone.primitives = held.primitives & Primitive::Undefined;
    return namesThis(read.subject) && held.functions == LinkType::Functions::Closure && held == alone && !mayHoldAccessorOnPrototype(made, read.key) && !mayBeDefinedUnderUnknownKeyOnPrototypeOf(made, read.key) && ignoresThis(held.function);
}

bool LinkFlow::constructionKeepsThis(LayoutID made) const
{
    const Layout& layout = program.at(made);
    bool isPlainConstructor = layout.kind == LayoutKind::Instance && !layout.has(LayoutFlags::HasHeritage) && layout.constructor && program.at(layout.constructor).constructedClass == made && program.at(layout.constructor).takesThisFromConstruction();
    if (isPlainConstructor && (ignoresThis(layout.constructor) || onlyForwardsThis(made) == true))
        return true;
    if (isPlainConstructor && !answersOwnThis(layout.constructor))
        return false;
    RowOfConstructorsInLinkLayouts row(*this, made);
    if (!row.followConstructorOf(made, true) || row.testsObject())
        return false;
    auto mayRunAccessor = [&](PropertyKeyID key) {
        size_t stepsLeft = program.layouts.size();
        for (LayoutID current = made; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
            const NameOnPrototype* name = findNameOnPrototype(current, key);
            if ((name && name->mayHoldAccessor()) || mayBeDefinedUnderUnknownKeyOnPrototypeOf(current, key))
                return true;
        }
        return false;
    };
    if (std::ranges::any_of(row.filled(), mayRunAccessor) || std::ranges::any_of(row.lookedUp(), mayRunAccessor))
        return false;
    Vector<FunctionID, 4> waiting { program.at(made).constructor };
    Vector<FunctionID, 8> seen;
    while (!waiting.isEmpty()) {
        FunctionID body = waiting.takeLast();
        if (seen.contains(body))
            continue;
        seen.append(body);
        const ThinSummary& summary = summaries.at(body);
        if (!summary.changes.isEmpty() || !summary.copiesOfMembers.isEmpty())
            return false;
        for (const SummaryAccess& access : summary.accesses) {
            bool isWrite = access.kind == SummaryAccess::Kind::Store || access.kind == SummaryAccess::Kind::Define;
            if (isWrite && (!access.key || access.isToContents()))
                return false;
        }
        for (const SummaryConstruction& construction : summary.constructions) {
            for (const SummaryConstruction::Part& part : summary.partsOfConstructions.span().subspan(construction.firstPart, construction.numberOfParts))
                waiting.append(part.function);
        }
    }
    return true;
}

void LinkFlow::noteFieldGivenAccessorByDescriptorInSight(FieldReference reference)
{
    uint32_t cell = m_firstFieldOfLayout[reference.layout.index()] + reference.field;
    if (!fieldsGivenAccessorsByDescriptorsInSight.get(cell))
        note(Note::FieldGivenAccessorByDescriptorInSight, cell);
}

void LinkFlow::noteFieldWithUnseenAccessor(FieldReference reference)
{
    uint32_t cell = m_firstFieldOfLayout[reference.layout.index()] + reference.field;
    if (!fieldsWithUnseenAccessors.get(cell))
        note(Note::FieldWithUnseenAccessor, cell);
}

void LinkFlow::noteDeletion(LayoutID layout, PropertyKeyID key)
{
    if (!key) {
        if (!layoutsDeletedUnderUnknownKeys.get(layout.index()))
            note(Note::LayoutDeletedUnderUnknownKey, layout.index());
        return;
    }
    std::optional<FieldReference> field = findField(program, layout, key);
    if (!field)
        return;
    uint32_t cell = m_firstFieldOfLayout[field->layout.index()] + field->field;
    if (!fieldsDeletedByName.get(cell))
        note(Note::FieldDeletedByName, cell);
}

void LinkFlow::noteFieldMayBeEmpty(FieldReference reference)
{
    uint32_t cell = m_firstFieldOfLayout[reference.layout.index()] + reference.field;
    if (!fieldsThatMayBeEmpty.get(cell))
        note(Note::FieldMayBeEmpty, cell);
}

void LinkFlow::noteLockableInstances(LayoutID id)
{
    for (LayoutID current = declaredLayoutOf(program, id); current; current = program.at(current).base) {
        if (m_classesWithLockableInstances.set(current.index()))
            return;
        if (auto told = m_instanceLockNodes.find(current.index()); told != m_instanceLockNodes.end())
            arrive(FlowNodeID(told->value), fromTypeID(TypeTable::undefined));
    }
}

FlowNodeID LinkFlow::instanceLockNode(LayoutID classLayout)
{
    return nodeAskedOnceByLane(MadeOnDemand::InstanceLocks, classLayout.index(), [&]() -> FlowNodeID {
        if (auto told = m_instanceLockNodes.find(classLayout.index()); told != m_instanceLockNodes.end())
            return FlowNodeID(told->value);
        FlowNodeID node = makeNode();
        m_instanceLockNodes.add(classLayout.index(), node.index());
        if (hasLockableInstances(classLayout))
            arrive(node, fromTypeID(TypeTable::undefined));
        return node;
    });
}

FlowNodeID LinkFlow::foundWhereFieldIsEmpty(FieldReference reference)
{
    uint32_t cell = m_firstFieldOfLayout[reference.layout.index()] + reference.field;
    auto findOrMake = [&] {
        uint32_t& known = m_nodesFoundWhereFieldsAreEmpty[cell];
        if (known != UINT32_MAX)
            return FlowNodeID(known);
        FlowNodeID node = makeNode();
        WTF::atomicStore<MemoryOrder::Release>(&known, node.index());
        if (fieldsThatMayBeEmpty.get(cell))
            arrive(node, fromTypeID(TypeTable::undefined));
        else if (!constructionsAreFollowedInLinkLayouts(program.at(reference.layout)))
            note(Note::FieldMayBeEmpty, cell);
        return node;
    };
    if (!m_severalThreadsTell) {
        if (m_nodesFoundWhereFieldsAreEmpty.isEmpty())
            m_nodesFoundWhereFieldsAreEmpty.fill(UINT32_MAX, m_firstFieldOfLayout.last());
        return findOrMake();
    }
    if (uint32_t known = WTF::atomicLoad<MemoryOrder::Acquire>(&m_nodesFoundWhereFieldsAreEmpty[cell]); known != UINT32_MAX)
        return FlowNodeID(known);
    PartOfWhatSomeLaneWasGiven& part = lockPartOfWhatSomeLaneWasGiven(MadeOnDemand::FoundWhereFieldIsEmpty, cell);
    FlowNodeID node = findOrMake();
    part.isLocked.store(false, std::memory_order_release);
    return node;
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
        bool testsObject { false };
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
        result.testsObject |= row.testsObject();
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
            m_keysThatRowsOfConstructorsFill[id.index()].testsObject = untilFirstSight[id.index()].testsObject;
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
    listConstructions();
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

bool LinkFlow::canCopySkipField(LayoutID layout) const
{
    uint32_t index = layout.index();
    return possibleChanges[index].containsAny({ LayoutChange::LosesProperty, LayoutChange::GetsAccessor }) || layoutsDefinedOnUnderWrittenKeys.get(index) || layoutsDefinedOnUnderKeysThatAreNotWritten.get(index)
        || layoutsGivenAccessorsByDescriptorsInSight.get(index) || layoutsWithKnownAccessors.get(index) || lostLayouts.get(index) || escapedLayouts.get(index) || m_parserAllocatedLayouts.quickGet(index);
}

bool LinkFlow::markFieldsSkippedByCopies()
{
    if (!m_copyingFunctions) {
        m_copyingFunctions = Vector<FunctionID>(summaries.copyingFunctions());
        m_parserAllocatedLayouts.ensureSize(program.layouts.size());
        for (const ExpectedResult& expected : program.expectedResults) {
            for (LayoutID made : expected.layouts) {
                if (expected.flags.contains(ExpectedResultFlags::IsMadeByParser))
                    m_parserAllocatedLayouts.quickSet(made.index());
            }
        }
        m_plainFieldLayouts.ensureSize(program.layouts.size());
        for (FunctionID id : *m_copyingFunctions) {
            summaries.at(id).forEachUnfilledLayout([&](LayoutID layout, std::span<const uint32_t>, uint32_t copy) {
                if (copy == ThinSummary::noCopy || program.at(layout).kind != LayoutKind::Record || m_plainFieldLayouts.get(layout.index()))
                    return;
                bool hasPlainFieldsOnly = std::ranges::all_of(program.at(layout).fields, [](const Field& field) {
                    return field.key && field.form.isEmpty();
                });
                if (hasPlainFieldsOnly)
                    m_plainFieldLayouts.set(layout.index());
            });
        }
    }
    if (m_copyingFunctions->isEmpty())
        return false;
    auto noteFieldsThatCopyLeaves = [&](const LinkType& source, LayoutID layout, std::span<const uint32_t> fieldsHeld, NOESCAPE const Invocable<void(FieldReference)> auto& note) {
        if (source.isNever())
            return true;
        LinkType objects;
        objects.objects = source.objects;
        objects.layout = source.layout;
        objects.family = source.family;
        bool holdsOnlyRecords = source == objects && (source.objects == LinkType::Objects::Layout || source.objects == LinkType::Objects::Set || source.objects == LinkType::Objects::Family);
        bool holdsOtherLayouts = false;
        if (holdsOnlyRecords) {
            forEachLayoutOf(source, [&](LayoutID copied) {
                holdsOtherLayouts |= copied != layout;
                holdsOnlyRecords = holdsOnlyRecords && program.at(copied).kind == LayoutKind::Record && !canCopySkipField(copied);
            });
        }
        if (holdsOnlyRecords && !holdsOtherLayouts && m_plainFieldLayouts.get(layout.index()))
            return true;
        auto copyFills = [&](uint32_t field) {
            PropertyKeyID key = program.at(layout).fields[field].key;
            bool isFilled = holdsOnlyRecords && !!key;
            if (isFilled) {
                forEachLayoutOf(source, [&](LayoutID copied) {
                    if (!isFilled)
                        return;
                    const Layout& from = program.at(copied);
                    bool isAtSameIndex = field < from.fields.size() && from.fields[field].key == key;
                    std::optional<FieldReference> found = isAtSameIndex ? std::optional(FieldReference { copied, field }) : findField(program, copied, key);
                    isFilled = found && fieldAt(program, *found).form.isEmpty() && !fieldsThatMayBeEmpty.get(m_firstFieldOfLayout[found->layout.index()] + found->field);
                });
            }
            return isFilled;
        };
        bool someIsFilled = false;
        for (uint32_t field = 0; field < program.at(layout).fields.size(); ++field) {
            if (std::ranges::binary_search(fieldsHeld, field) || fieldsThatMayBeEmpty.get(m_firstFieldOfLayout[layout.index()] + field))
                continue;
            if (copyFills(field)) {
                someIsFilled = true;
                continue;
            }
            note({ layout, field });
        }
        return someIsFilled;
    };
    Vector<bool> waits(FillWith { }, m_copyingFunctions->size(), true);
    Vector<FieldReference> fieldsLeft = collectInParallel<FieldReference>(loops, static_cast<unsigned>(m_copyingFunctions->size()), [&](unsigned place, Vector<FieldReference>& left) {
        FunctionID id = (*m_copyingFunctions)[place];
        if (!reached.get(id.index()))
            return;
        bool someIsFilled = false;
        const ThinSummary& summary = summaries.at(id);
        summary.forEachUnfilledLayout([&](LayoutID layout, std::span<const uint32_t> fieldsHeld, uint32_t copy) {
            if (copy == ThinSummary::noCopy)
                return;
            someIsFilled |= noteFieldsThatCopyLeaves(typeAt(nodeOf(id, summary.changes[copy].other)), layout, fieldsHeld, [&](FieldReference field) { left.append(field); });
        });
        waits[place] = someIsFilled;
    });
    if (!fieldsLeft.isEmpty()) {
        static constexpr uint32_t fillsNothing = UINT32_MAX;
        uint32_t clock = 1;
        Vector<uint32_t> changedAt(FillWith { }, program.layouts.size(), 0u);
        Vector<uint32_t> judgedAt;
        auto noteField = [&](FieldReference field) {
            noteFieldMayBeEmpty(field);
            changedAt[field.layout.index()] = clock;
        };
        for (FieldReference field : fieldsLeft)
            noteField(field);
        bool someAreLeft = true;
        while (someAreLeft) {
            someAreLeft = false;
            size_t place = 0;
            for (size_t copying = 0; copying < m_copyingFunctions->size(); ++copying) {
                FunctionID id = (*m_copyingFunctions)[copying];
                if (!reached.get(id.index()) || !waits[copying])
                    continue;
                const ThinSummary& summary = summaries.at(id);
                summary.forEachUnfilledLayout([&](LayoutID layout, std::span<const uint32_t> fieldsHeld, uint32_t copy) {
                    if (copy == ThinSummary::noCopy)
                        return;
                    if (place == judgedAt.size())
                        judgedAt.append(0);
                    uint32_t& judged = judgedAt[place++];
                    if (judged == fillsNothing)
                        return;
                    uint32_t numberOfFields = program.at(layout).fields.size();
                    uint32_t field = 0;
                    while (field < numberOfFields && (fieldsThatMayBeEmpty.get(m_firstFieldOfLayout[layout.index()] + field) || std::ranges::binary_search(fieldsHeld, field)))
                        ++field;
                    if (field == numberOfFields) {
                        judged = fillsNothing;
                        return;
                    }
                    const LinkType& source = typeAt(nodeOf(id, summary.changes[copy].other));
                    if (source.objects != LinkType::Objects::Layout && source.objects != LinkType::Objects::Set && source.objects != LinkType::Objects::Family)
                        return;
                    bool hasChanged = false;
                    forEachLayoutOf(source, [&](LayoutID copied) {
                        hasChanged |= changedAt[copied.index()] > judged;
                    });
                    if (!hasChanged)
                        return;
                    judged = clock++;
                    noteFieldsThatCopyLeaves(source, layout, fieldsHeld, [&](FieldReference field) {
                        noteField(field);
                        someAreLeft = true;
                    });
                });
            }
        }
    }
    size_t numberWaiting = 0;
    for (size_t place = 0; place < waits.size(); ++place) {
        if (waits[place])
            (*m_copyingFunctions)[numberWaiting++] = (*m_copyingFunctions)[place];
    }
    m_copyingFunctions->shrink(numberWaiting);
    return !fieldsLeft.isEmpty();
}

bool LinkFlow::mayHoldAccessorOnPrototype(LayoutID layout, PropertyKeyID key) const
{
    const NameOnPrototype* name = findNameOnPrototype(layout, key);
    return (name && name->mayHoldAccessor()) || mayBeDefinedUnderUnknownKeyOnPrototypeOf(layout, key);
}

bool LinkFlow::callsMethodsOfNewObject(LayoutID made) const
{
    const Layout& layout = program.at(made);
    if (layout.kind != LayoutKind::Instance || layout.base || layout.has(LayoutFlags::HasHeritage) || !constructionsAreFollowedInLinkLayouts(layout))
        return false;
    const Function& constructor = program.at(layout.constructor);
    if (constructor.kind != FunctionKind::Plain && constructor.kind != FunctionKind::BaseConstructor)
        return false;
    const ThinSummary& summary = summaries.at(layout.constructor);
    std::optional<unsigned> own = constructor.kind == FunctionKind::Plain ? constructor.thisParameterIndex() : std::nullopt;
    return std::ranges::any_of(summary.calls, [&](const SummaryCall& call) {
        if (call.isConstruction)
            return false;
        if (call.callee == SummaryCall::Callee::Name)
            return call.receiver != noSummaryNode;
        return own && std::ranges::any_of(summary.arguments.span().subspan(call.firstArgument, call.numberOfArguments), [&](uint32_t argument) {
            return summary.nodes[argument] == SummaryNode { SummaryNode::Kind::Parameter, *own, 0 };
        });
    });
}

static Vector<uint32_t, 8> findConstructionsStoredInto(const ThinSummary& summary)
{
    Vector<uint32_t, 8> constructions;
    for (const SummaryAccess& access : summary.accesses) {
        bool isWrite = access.kind == SummaryAccess::Kind::Store || access.kind == SummaryAccess::Kind::Define;
        if (!isWrite || !access.key || access.subject == noSummaryNode)
            continue;
        const SummaryNode& object = summary.nodes[access.subject];
        if (object.kind == SummaryNode::Kind::ResultOfCall && summary.calls[object.first].isConstruction)
            constructions.append(object.first);
    }
    std::ranges::sort(constructions);
    return constructions;
}

static bool isFollowedByMethod(const ThinSummary& summary, uint32_t construction)
{
    static constexpr uint32_t mostCallsBetween = 4;
    uint32_t end = std::min<uint32_t>(summary.calls.size(), construction + 2 + mostCallsBetween);
    for (uint32_t number = construction + 1; number < end; ++number) {
        const SummaryCall& next = summary.calls[number];
        if (next.isConstruction || next.callee != SummaryCall::Callee::Name || next.receiver == noSummaryNode)
            continue;
        const SummaryNode& receiver = summary.nodes[next.receiver];
        if (receiver.kind == SummaryNode::Kind::ResultOfAccess || (receiver.kind == SummaryNode::Kind::ResultOfCall && receiver.first == construction))
            return true;
    }
    return false;
}

void LinkFlow::listConstructions()
{
    m_constructorsBeforeMethods.ensureSize(program.functions.size());
    struct FoundConstruction {
        FunctionID caller;
        uint32_t call { 0 };
        bool isStoredInto { false };
    };
    Vector<FoundConstruction> found = collectInParallel<FoundConstruction>(build.parallelLoops(), static_cast<unsigned>(program.functions.size()), [&](unsigned index, Vector<FoundConstruction>& constructions) {
        const ThinSummary& summary = summaries.at(FunctionID(index));
        std::span<const SummaryCall> calls = summary.calls.span();
        std::optional<Vector<uint32_t, 8>> storedInto;
        for (uint32_t number = 0; number < calls.size(); ++number) {
            if (!calls[number].isConstruction)
                continue;
            if (!storedInto)
                storedInto = findConstructionsStoredInto(summary);
            constructions.append({ FunctionID(index), number, std::ranges::binary_search(*storedInto, number) });
        }
    });
    m_constructions = WTF::map(found, [](const FoundConstruction& construction) {
        return ConstructionInSummary { construction.caller, construction.call };
    });
    Vector<uint32_t> placesBeforeMethods = findIndicesInParallel(build.parallelLoops(), static_cast<unsigned>(m_constructions.size()), [&](unsigned place) {
        auto [caller, number] = m_constructions[place];
        const ThinSummary& summary = summaries.at(caller);
        return summary.calls[number].callee != SummaryCall::Callee::Builtin && (found[place].isStoredInto || isFollowedByMethod(summary, number));
    });
    for (uint32_t place : placesBeforeMethods) {
        auto [caller, number] = m_constructions[place];
        const SummaryCall& call = summaries.at(caller).calls[number];
        if (call.callee == SummaryCall::Callee::Binding && call.function) {
            m_constructorsBeforeMethods.quickSet(call.function.index());
            continue;
        }
        const BodyIndex* body = bodiesLookedAt(looks).find(caller);
        if (body && !body->isParameter(call.site) && body->opcodeOf(call.site) == Opcode::Construct) {
            auto construct = body->value(call.site).as<Opcode::Construct>();
            if (StringID name = construct.calleeName())
                m_namesConstructedBeforeMethods.add(name.bits());
            ValueID callee = body->stripCasts(construct.calleeOperand());
            if (callee && !body->isParameter(callee) && body->opcodeOf(callee) == Opcode::GetModuleVariable) {
                const Variable& variable = program.at(body->value(callee).as<Opcode::GetModuleVariable>().variable());
                if (variable.initialization == VariableInitialization::Function)
                    m_constructorsBeforeMethods.quickSet(variable.function.index());
            }
        }
    }
}

bool LinkFlow::isFilledAfterConstructions(LayoutID made) const
{
    const Layout& layout = program.at(made);
    if (layout.kind != LayoutKind::Instance || layout.has(LayoutFlags::HasHeritage) || !constructionsAreFollowedInLinkLayouts(layout))
        return false;
    const Function& constructor = program.at(layout.constructor);
    if (layout.base && (constructor.kind != FunctionKind::Plain || constructor.thisParameterIndex()))
        return false;
    if (!m_constructorsBeforeMethods.get(layout.constructor.index()) && !(constructor.name && m_namesConstructedBeforeMethods.contains(constructor.name.bits())))
        return false;
    return constructor.kind == FunctionKind::Plain || constructor.kind == FunctionKind::BaseConstructor;
}

bool LinkFlow::noteFieldsEmptiedByMethods()
{
    using Initializers = InitializersInLinkLayouts;
    auto entersFunction = [&](FunctionID caller, uint32_t number, FunctionID entered) {
        const CalleesOfCall& callees = calleesOfCallsAsSettled()[firstCallOfFunction[caller.index()] + number];
        return callees.listed().contains(entered) || std::ranges::any_of(callees.shared(), [&](uint32_t shared) {
            return calleesOfSharedCallsAsSettled()[shared].listed().contains(entered);
        });
    };
    HashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> functionsEnteredByNewAlone;
    auto computeEnteredByNewAlone = [&](FunctionID entered) {
        if (lostFunctions.get(entered.index()) || escapedFunctions.get(entered.index()))
            return false;
        for (auto [caller, number] : m_constructions) {
            if (!reached.get(caller.index()) || !entersFunction(caller, number, entered))
                continue;
            const SummaryCall& call = summaries.at(caller).calls[number];
            const BodyIndex* index = bodiesLookedAt(looks).find(caller);
            if (!index || index->isParameter(call.site) || index->opcodeOf(call.site) != Opcode::Construct)
                return false;
        }
        if (isClassConstructorOf(program.at(entered).kind))
            return true;
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            if (!reached.get(index))
                continue;
            const ThinSummary& summary = summaries.at(FunctionID(index));
            for (uint32_t number = 0; number < summary.calls.size(); ++number) {
                if (!summary.calls[number].isConstruction && entersFunction(FunctionID(index), number, entered))
                    return false;
            }
        }
        return true;
    };
    auto askWhetherEnteredByNewAlone = [&](FunctionID entered) {
        auto added = functionsEnteredByNewAlone.add(entered.bits(), false);
        if (added.isNewEntry)
            added.iterator->value = computeEnteredByNewAlone(entered);
        return added.iterator->value;
    };
    auto isEnteredByNewAlone = scopedLambda<bool(FunctionID)>(askWhetherEnteredByNewAlone);
    HashMap<uint32_t, Vector<uint32_t, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> constructionsOfFunctions;
    bool hasIndexedConstructions = false;
    auto indexConstructions = [&] {
        hasIndexedConstructions = true;
        for (LayoutID made : m_layoutsFilledByMethods)
            constructionsOfFunctions.add(program.at(made).constructor.bits(), Vector<uint32_t, 2> { });
        for (uint32_t place = 0; place < m_constructions.size(); ++place) {
            auto [caller, number] = m_constructions[place];
            if (!reached.get(caller.index()))
                continue;
            auto add = [&](FunctionID entered) {
                auto listed = constructionsOfFunctions.find(entered.bits());
                if (listed != constructionsOfFunctions.end() && (listed->value.isEmpty() || listed->value.last() != place))
                    listed->value.append(place);
            };
            const CalleesOfCall& callees = calleesOfCallsAsSettled()[firstCallOfFunction[caller.index()] + number];
            std::ranges::for_each(callees.listed(), add);
            for (uint32_t shared : callees.shared())
                std::ranges::for_each(calleesOfSharedCallsAsSettled()[shared].listed(), add);
        }
    };
    auto findFilledAfterConstructions = [&](LayoutID made) -> std::optional<Vector<uint64_t>> {
        FunctionID constructor = program.at(made).constructor;
        if (!isFilledAfterConstructions(made) || lostFunctions.get(constructor.index()) || escapedFunctions.get(constructor.index()) || escapedLayouts.get(made.index()))
            return std::nullopt;
        std::optional<Vector<uint64_t>> common;
        if (!hasIndexedConstructions)
            indexConstructions();
        auto constructions = constructionsOfFunctions.find(constructor.bits());
        if (constructions == constructionsOfFunctions.end())
            return std::nullopt;
        for (uint32_t place : constructions->value) {
            auto [caller, number] = m_constructions[place];
            const CalleesOfCall& callees = calleesOfCallsAsSettled()[firstCallOfFunction[caller.index()] + number];
            const SummaryCall& call = summaries.at(caller).calls[number];
            if (callees.mayEnterUnseen || callees.mayEnterLost || !callees.builtins().isEmpty() || !callees.shared().isEmpty() || callees.listed().size() != 1 || (call.callee != SummaryCall::Callee::Binding && call.callee != SummaryCall::Callee::Value))
                return std::nullopt;
            Initializers initializers(*this, made);
            std::optional<Initializers::Fields> filled = initializers.findFilledByFirstMethod(caller, call.site, isEnteredByNewAlone);
            if (!filled)
                return std::nullopt;
            size_t numberOfWords = (program.at(made).fields.size() + 63) / 64;
            if (!common)
                common = Vector<uint64_t>(FillWith { }, numberOfWords, ~0ull);
            for (uint32_t field = 0; field < program.at(made).fields.size(); ++field) {
                if (!filled->contains(field))
                    (*common)[field / 64] &= ~(1ull << field % 64);
            }
        }
        return common;
    };
    bool hasNoted = false;
    m_layoutsTestedByConstructors.removeAllMatching([&](LayoutID made) {
        if (InitializersInLinkLayouts(*this, made).testsKeepObject())
            return false;
        for (uint32_t field = 0; field < program.at(made).fields.size(); ++field) {
            if (fieldsThatMayBeEmpty.get(m_firstFieldOfLayout[made.index()] + field))
                continue;
            noteFieldMayBeEmpty({ made, field });
            hasNoted = true;
        }
        return true;
    });
    m_layoutsFilledByMethods.removeAllMatching([&](LayoutID made) {
        const Vector<PropertyKeyID>& filledBeforeSight = m_listsOfKeysThatRowsOfConstructorsFill[m_keysThatRowsOfConstructorsFill[made.index()].untilFirstSight];
        const Layout& layout = program.at(made);
        auto mayBeFilledByMethod = [&](uint32_t field) {
            PropertyKeyID key = layout.fields[field].key;
            return !fieldsThatMayBeEmpty.get(m_firstFieldOfLayout[made.index()] + field) && !(key && std::ranges::binary_search(filledBeforeSight, key));
        };
        Vector<uint32_t, 16> fields;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (mayBeFilledByMethod(field))
                fields.append(field);
        }
        if (fields.isEmpty())
            return true;
        InitializersInLinkLayouts initializers(*this, made);
        std::optional<InitializersInLinkLayouts::Fields> filled = initializers.findFilled();
        if (filled) {
            for (FunctionID method : initializers.methods())
                m_functionsFillingNewObjects.set(method.index());
        }
        std::optional<Vector<uint64_t>> filledAfterConstructions = filled && !initializers.showsObject() ? findFilledAfterConstructions(made) : std::nullopt;
        for (uint32_t field : fields) {
            uint32_t cell = m_firstFieldOfLayout[made.index()] + field;
            bool isFilledByConstructor = layout.fields[field].key && filled && filled->contains(field);
            bool isFilledLater = !isFilledByConstructor && layout.fields[field].key && filledAfterConstructions && (*filledAfterConstructions)[field / 64] >> field % 64 & 1 && !initializers.readsEmpty(field);
            fieldsFilledAfterConstructions.set(cell, isFilledLater);
            if (isFilledByConstructor || isFilledLater)
                continue;
            noteFieldMayBeEmpty({ made, field });
            hasNoted = true;
        }
        return false;
    });
    if (hasNoted)
        settle();
    return hasNoted;
}

void LinkFlow::noteConstructionsOf(FunctionID id)
{
    noteBirthsOf(id);
    summaries.at(id).forEachUnfilledLayout([&](LayoutID layout, std::span<const uint32_t> fieldsHeld, uint32_t copy) {
        if (copy != ThinSummary::noCopy)
            return;
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
    if (m_keysThatRowsOfConstructorsFill[made.index()].testsObject) {
        LockerOfWhatIsShared lockerOfWhatIsShared(*this);
        m_layoutsTestedByConstructors.append(made);
    }
    if (callsMethodsOfNewObject(made) || isFilledAfterConstructions(made)) {
        LockerOfWhatIsShared lockerOfWhatIsShared(*this);
        m_layoutsFilledByMethods.append(made);
        return;
    }
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
    MayChangePrototype = 1 << 7,
};

class OnlyFunctionsInLinkLayouts {
public:
    bool isEmpty() const { return m_functions.isEmpty(); }
    void startFor(size_t numberOfPlaces) { m_functions.fill(none, numberOfPlaces); }

    void note(size_t place, FunctionID function)
    {
        uint32_t* noted = &m_functions[place];
        if (WTF::atomicLoad<MemoryOrder::Relaxed>(noted) != several && WTF::atomicExchange<MemoryOrder::Relaxed>(noted, function.index()) != none)
            noteSeveral(place);
    }
    void noteSeveral(size_t place) { WTF::atomicStore<MemoryOrder::Relaxed>(&m_functions[place], several); }

    FunctionID at(size_t place) const
    {
        uint32_t noted = WTF::atomicLoad<MemoryOrder::Relaxed>(&m_functions[place]);
        return noted < several ? FunctionID(noted) : FunctionID();
    }

private:
    static constexpr uint32_t several = UINT32_MAX - 1;
    static constexpr uint32_t none = UINT32_MAX;

    mutable Vector<uint32_t> m_functions;
};

} // anonymous namespace

class WriterOfLinkLayouts {
    WTF_MAKE_NONCOPYABLE(WriterOfLinkLayouts);
public:
    WriterOfLinkLayouts(const LinkFlow&, LinkTables&);

    void lookAtSummaries();
    void writeWhatMakesNoType(BitVector&& functionsWhoseCallersAreAllKnown, LateFills);
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
    enum class Numbers : uint8_t { AsHeld, ByKindAlone, AsSlotHolds, AsVariableHolds };
    TypeID claimedTypeAt(FlowNodeID, Numbers = Numbers::AsHeld);
    bool claimLeavesOutWhatArrivesAt(FlowNodeID) const;
    TypeID namingSymbolAt(FlowNodeID, TypeID);
    TypeID claimedTypeAtEither(FlowNodeID, FlowNodeID);
    TypeID typeEstablishedOf(const DeclaredType&) const;
    TypeID narrowerOf(TypeID byFlow, const DeclaredType&);
    TypeID typeOfWrittenUnit(WrittenUnit);

    void lookAtEdges(FunctionID, const ThinSummary&);
    struct AccessInSummary {
        FunctionID function;
        FlowNodeID subject;
        const SummaryAccess* access { nullptr };
    };
    Vector<Vector<AccessInSummary>> findRelevantAccesses() const;
    static void prefetchAccess(const LinkFlow&, std::span<const AccessInSummary>);
    void lookAtAccesses(FunctionID, const ThinSummary&, std::span<const AccessInSummary>& accessesLeft);
    void lookAtChanges(FunctionID, const ThinSummary&);
    void noteChangeOfClosure(const LinkType& subject, PropertyKeyID);
    void noteStaticFieldStore(FunctionID, const ThinSummary&, const SummaryAccess&, PropertyKeyID, const LinkType& subject);
    void noteStaticFieldChange(const LinkType& subject, PropertyKeyID);
    void noteStaticFieldChange(FunctionID constructor, PropertyKeyID);
    void noteStaticFieldRead(FunctionID, const SummaryAccess&);
    void findStaticPropertyReads(const BitVector& keys);
    void noteStaticFieldUnit(VariableID, uint32_t unit);
    void notePrimitivesOfStaticField(VariableID, FunctionID, const SummaryAccess&);
    void lookAtInitializer(FunctionID);
    void writeUnitsOfStaticFields();
    void writePrimitivesOfStaticFields();
    void findVariablesOfFilledClasses(FunctionID moduleBody);
    void noteNameDefinedPastLists(const LinkType& holder, PropertyKeyID, bool isDefinition);
    void noteHeritage(const LinkType& heritage, FunctionID constructor);
    LayoutID findLayoutWhosePrototypeObjectIsAt(const ThinSummary&, uint32_t node) const;
    std::pair<FunctionID, ValueID> findCallAtOnce(FunctionID callee, ValueID site) const;
    struct PassedStore {
        LayoutID layout;
        FunctionID caller;
        ValueID call;
    };
    PassedStore findPassedStore(FunctionID, const SummaryAccess&, PropertyKeyID, const LinkType& subject) const;
    void noteNameWrittenOnPrototype(FunctionID, const ThinSummary&, const SummaryAccess&, PropertyKeyID, const LinkType& subject);
    LayoutID findClassOfPrototypeAt(FunctionID, const ThinSummary&, uint32_t node) const;
    bool isOnEveryPath(FunctionID, ValueID site) const;
    FunctionID findOnlyFunctionAt(FunctionID, uint32_t node) const;
    LayoutID findClassAssignedAt(FunctionID, const ThinSummary&, uint32_t node) const;
    FunctionID findFunctionAssignedAt(FunctionID, const ThinSummary&, uint32_t node, LayoutID, PropertyKeyID) const;
    bool isBesideClass(FunctionID writer, LayoutID) const;
    void noteMemberAssignment(FunctionID, const ThinSummary&, const SummaryAccess&, PropertyKeyID, const LinkType& subject);
    void noteComputedMemberAssignment(FunctionID writer, const ThinSummary&, const SummaryAccess&);
    void noteAssignmentsByHelpers();
    bool assignsThroughParameter(FunctionID helper);
    void noteAssignmentsOfCall(FunctionID caller, const SummaryCall&, FunctionID helper, uint32_t argument, LayoutID, bool passesPrototype);
    LayoutID noteNameAssignedToClass(const LinkType& subject, LayoutID namedClass, PropertyKeyID);
    bool isNameAssignedToClass(LayoutID, PropertyKeyID) const;
    void noteAssignmentOutsideClass(FunctionID writer, ValueID site, LayoutID, PropertyKeyID, FunctionID method);
    void noteLastAssignment(FunctionID writer, ValueID site, LayoutID, PropertyKeyID, FunctionID method);
    bool appendStoresUnderNames(FunctionID, const ThinSummary&, const SummaryAccess&, Vector<SummaryAccess>& stores);
    void noteAffixedNamesOfStore(FunctionID, const SummaryAccess&);
    void noteListedStoreNames(FunctionID, const ThinSummary&, const SummaryAccess&);
    bool mayStorePrototype(FunctionID, const SummaryAccess&, PropertyKeyID) const;
    bool comesBeforeFirstObject(FunctionID, ValueID site, LayoutID) const;
    LayoutID findDecoratedClass(FunctionID, ValueID site) const;
    void noteAccessorAssignedToClass(FunctionID writer, ValueID site, LayoutID, PropertyKeyID, FunctionID getter, FunctionID setter);
    void noteAccessorAssignedToClass(FunctionID, const ThinSummary&, const SummaryChange&);
    void noteHalfOfAccessorLaidOnPrototype(FunctionID, const ThinSummary&, const SummaryChange&);
    bool isWrittenOnPrototypeAbove(LayoutID, PropertyKeyID) const;
    void lookAtMembers(const ThinSummary&);
    void noteReplacement(LinkedMember&, const LinkType& held) const;
    void noteWrite(const LinkType& subject, PropertyKeyID, Traits);
    PropertyKeyID findNameThatConstructorStores(FunctionID, const ThinSummary&, const SummaryAccess&) const;
    bool storesIntoObjectThatConstructorMakesOnEveryWay(FunctionID, const ThinSummary&, const SummaryAccess&) const;
    struct ConstructorLookedAt {
        FunctionID function;
        BitVector nodesUnderConstruction;
        BlockID blockAboveEveryReturn;
    };
    mutable ConstructorLookedAt m_constructorLookedAt;
    mutable HashMap<uint32_t, BlockID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_lastBlocksOnEveryPath;
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
    LateFills m_lateFills { LateFills::FoundHere };
    bool mayBeAllocated(LayoutID) const;
    bool isConstructedOnlyBySuper(LayoutID) const;
    bool mayBeKnownByFamily(LayoutID id) const { return m_flow.layoutsMetInFamilies.get(id.index()) || m_flow.lostLayouts.get(id.index()) || hasEscaped(id.index()); }
    bool canHoldAppendedFields(LayoutID) const;
    bool canNameAppendedField(LayoutID, PropertyKeyID) const;
    bool isListedByClassBelow(LayoutID, PropertyKeyID) const;
    bool noteNameNotListed(LayoutID, PropertyKeyID);
    bool isStoredOnlyAtAllocation(LayoutID, PropertyKeyID) const;
    void noteStoreOnPrototypeObjects(const LinkType&, PropertyKeyID);
    FunctionID findMethodOnlyFromClass(LayoutID, PropertyKeyID) const;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesStoredOnPrototypeObjects;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesStoredAtAllocations;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesStoredElsewhere;
    bool m_storedObjectIsAllocatedInBody { false };
    void noteNamesWrittenOfLayouts();
    void noteNamesThatMakersGive();
    uint32_t findMakingInFrontOfRun(uint32_t);
    void noteNamesThatFollowersOfMakersStore();
    OnlyFunctionsInLinkLayouts m_onlyCallersOutsideLoops;
    BitVector m_bodiesEnteredOnce;
    OnlyFunctionsInLinkLayouts m_onlyMakersOfRecords;
    void listMakersOfRecords();
    void listCallsThatEnter();
    bool runsAtMostOnce(FunctionID);
    bool isSingletonRecord(LayoutID);
    void appendFields();
    void noteStoreOfMaker(const ThinSummary&, const SummaryAccess&, PropertyKeyID);
    void noteLateFieldsFilledByAccess(FunctionID, const ThinSummary&, const SummaryAccess&, PropertyKeyID, const LinkType& subject);
    void findLateFieldsFilledTogether();
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
    void noteHelperSteps();
    void noteNameGivenToClosure(const LinkType& subject, PropertyKeyID = { }, bool laysValueInSight = false);
    void writeNamesOnFunctions();
    bool onlyStatementsLayName(FunctionID, PropertyKeyID) const;
    void forEachFollowedName(const LinkType& subject, PropertyKeyID, NOESCAPE const Invocable<void(FunctionID)> auto&);
    void findFunctionNamesRead();
    void noteStoreOfFunctionName(FunctionID, const SummaryAccess&, const LinkType& subject, PropertyKeyID);
    void noteOtherWriteOfFunctionName(const LinkType& subject, PropertyKeyID);
    using FunctionNames = HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    FunctionNames m_functionNamesRead;
    FunctionNames m_functionNamesWrittenOtherwise;
    Vector<LinkedFunctionNameStore> m_storesOfFunctionNames;
    BitVector m_keysOfFunctionNames;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesThatStatementsLayOnLostFunctions;
    void writeRowsBelowNamesDefinedPastLists();
    BitVector m_sidesWithNamesDefinedPastLists;
    Vector<uint64_t> m_namesDefinedOnPrototypes;
    BitVector m_prototypesGainingUnknownNames;
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
    BitVector m_layoutsCopiedInto;
    BitVector m_lostFamiliesCopiedInto;
    BitVector m_layoutsExtendedAlone;
    BitVector m_layoutsWithComputedAccessors;
    UncheckedKeyHashMap<uint32_t, LayoutID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_layoutsAboveLayouts;
    void noteComputedAccessorsOn(const LinkType&);
    void lookAtDefinersOfAccessors(FunctionID, const ThinSummary&);
    bool standsBelowComputedAccessors(LayoutID) const;
    bool isFilledByCreatorsAlone(LayoutID) const;
    Vector<OptionSet<LayoutChange>> m_changesOfLostFamilies;
    BitVector m_isMadeAtNoNode;
    BitVector m_isMadeByParsers;
    BitVector m_liesBelowLostClassObject;
    BitVector m_gainsOnlyWhatWrittenDefinitionsLay;
    BitVector m_gainsUnknownNames;
    BitVector m_gainsListedNamesAlone;
    BitVector m_isRedefinedByWrittenDefinitionsAlone;
    BitVector m_appendedKeysThatDescriptorsDefine;
    BitVector m_appendedKeysThatDescriptorsAloneWrite;
    BitVector m_fieldsWrittenWithoutDefinition;
    BitVector m_fieldsInWhichDefinitionsLayForms;
    BitVector m_fieldsInWhichDescriptorsMayLayAccessors;
    bool m_changeByDescriptorIsAtHand { false };
    bool m_currentChangeDefinesAttributes { false };
    bool m_changeAtHandMayLayAccessor { false };
    BitVector m_isFilledUnderComputedKeys;
    Vector<LayoutID> m_heritages;
    BitVector m_hasHeritage;
    BitVector m_hasSeveralHeritages;
    struct NameWrittenOnPrototype {
        FunctionID getter;
        FunctionID setter;
        FunctionID method;
        bool isWrittenOnce { false };
        bool isWrittenAgain { false };
        bool isCopied { false };
        FunctionID lastWriter;
        ValueID lastSite;
        FunctionID lastMethod;
        bool hasSeveralWriters { false };
        bool isOnPassedHolder { false };
        Vector<std::pair<ValueID, FunctionID>> methods;

        bool addOtherHalf(const NameWrittenOnPrototype& other)
        {
            bool addsSetter = getter && !setter && other.setter && !other.getter;
            bool addsGetter = setter && !getter && other.getter && !other.setter;
            if (!isWrittenOnce || !other.isWrittenOnce || method || other.method || isCopied || (!addsSetter && !addsGetter))
                return false;
            (addsSetter ? setter : getter) = addsSetter ? other.setter : other.getter;
            return true;
        }
    };
    HashMap<uint64_t, NameWrittenOnPrototype, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesWrittenOnPrototypes;
    HashMap<uint64_t, NameWrittenOnPrototype, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_accessorsAssignedToClasses;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_accessorsBeforeObjects;
    struct LastAssignment {
        FunctionID writer;
        ValueID site;
        FunctionID method;
    };
    HashMap<uint64_t, LastAssignment, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_lastAssignments;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesAssignedToClasses;
    HashMap<uint32_t, Vector<FamilyID, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_familiesAssignedNames;
    BitVector m_classesAssignedComputedNames;
    struct OwnNameAssignment {
        FunctionID writer;
        ValueID site;
        LayoutID classLayout;
        FunctionID method;
    };
    Vector<OwnNameAssignment> m_assignmentsOfOwnNames;
    struct AssignmentByHelper {
        FunctionID helper;
        LayoutID classLayout;
        PropertyKeyID key;
        FunctionID method;
    };
    Vector<AssignmentByHelper> m_assignmentsByHelpers;
    BitVector m_helpersAsked;
    BitVector m_helpersThatAssign;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_keysWrittenOnPrototypes;
    HashMap<uint64_t, NameWrittenOnPrototype, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_accessorsWrittenInMaps;
    struct MethodByDecorator {
        FunctionID writer;
        ValueID site;
        LayoutID classLayout;
        PropertyKeyID key;
        FunctionID method;
    };
    Vector<MethodByDecorator> m_methodsByDecorators;
    Vector<std::pair<uint64_t, uint64_t>> m_namesLaidOutOfMaps;
    struct CopiedPrototypeMember {
        uint64_t copy;
        uint64_t original;
    };
    Vector<CopiedPrototypeMember> m_copiedPrototypeMembers;
    void noteCopiedPrototypeMember(FunctionID, const ThinSummary&, const SummaryAccess& store, uint64_t copy);
    void findCopiedPrototypeMemberFunctions();

    BitVector m_fieldsWrittenLate;
    BitVector m_fieldsGivenValues;
    BitVector m_fieldsRedefinedWithAttributes;
    static constexpr uint32_t otherThanOneUnit = UINT32_MAX;
    Vector<uint32_t> m_unitsListedForFields;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_unitsOfStaticFields;
    HashMap<uint32_t, uint16_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_primitivesOfStaticFields;
    static constexpr uint32_t firstUnitOfNumber = 2;
    Vector<double> m_numbersOfStaticFields;
    struct FieldOfInitializer {
        bool isDefinedFirst { false };
        std::optional<double> number;
    };
    FunctionID m_initializer;
    HashMap<uint32_t, FieldOfInitializer, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_fieldsOfInitializer;
    BitVector m_staticFieldsFilledFirst;
    BitVector m_staticFieldsDefinedByInitializers;
    BitVector m_staticFieldsRemovedOrRedefined;
    BitVector m_staticKeysReadAsProperties;
    BitVector m_staticFieldsReadAsProperties;
    bool m_lostClassStaticFieldsAreProperties { false };
    bool m_allStaticFieldsAreProperties { false };
    BitVector m_lostClassChangedStaticKeys;
    BitVector m_staticKeysChangedOnFamilies;
    bool m_anyLostClassStaticFieldMayChange { false };
    bool m_anyStaticFieldMayChange { false };
    static constexpr uint32_t severalClasses = UINT32_MAX;
    static constexpr uint32_t layoutAmongWhatMakersLay = 1u << 31;
    enum class AsksForElements : bool { No, Yes };
    uint32_t whatIsMadeInSightAt(FunctionID, const ThinSummary&, uint32_t node, AsksForElements = AsksForElements::Yes);
    static constexpr uint32_t partsOfNumbersLaid = layoutAmongWhatMakersLay - 16;
    static constexpr uint32_t mayBeFractions = partsOfNumbersLaid | 2;
    static constexpr uint32_t numbersAmongWhatMakersLay = partsOfNumbersLaid | 4;
    static constexpr uint32_t integersOfOperators = numbersAmongWhatMakersLay | 1;
    static constexpr uint32_t fractions = numbersAmongWhatMakersLay | 2;
    static constexpr uint32_t otherPrimitivesAmongWhatMakersLay = layoutAmongWhatMakersLay - 2;
    static bool areNumbersAmongWhatMakersLay(uint32_t made) { return (made & ~3u) == numbersAmongWhatMakersLay; }
    static bool areIntegersAmongWhatMakersLay(uint32_t made) { return made == integersOfOperators; }
    static bool arePrimitivesAmongWhatMakersLay(uint32_t made) { return (made & ~7u) == partsOfNumbersLaid || made == otherPrimitivesAmongWhatMakersLay; }
    uint32_t numbersOfConstant(const SummaryNode&) const;
    struct NumberOperand {
        PropertyKeyID key;
        bool isElement { false };
    };
    using NumberOperands = Vector<NumberOperand, 4>;
    uint32_t numbersOfOperator(const ThinSummary&, const SummaryOperator&, PropertyKeyID storedUnder, NumberOperands*, unsigned depth = 0) const;
    uint32_t whatIsStoredInSightAt(FunctionID, const ThinSummary&, uint32_t node, PropertyKeyID storedUnder = { }, NumberOperands* = nullptr);
    enum class StoredNumberPlace : uint8_t { Field, LayoutName, Name };
    struct NumberDependency {
        StoredNumberPlace place;
        uint64_t index;
        NumberOperand operand;
    };
    Vector<NumberDependency> m_numberDependencies;
    void noteFieldsOfSteppedIntegers();
    void noteNumberDependencies(StoredNumberPlace, uint64_t index, const NumberOperands&);
    uint32_t* findNumbersStoredAt(const NumberDependency&);
    uint32_t numbersOfOperand(const NumberOperand&) const;
    void propagateNumberDependencies();
    void noteNamesStoredThroughWhatFieldsAreExpectedToHold();
    LayoutID recordStoredInSight(const ThinSummary&, uint32_t node);
    uint32_t classStoredByMember(LayoutID, FunctionID, const ThinSummary&, uint32_t node, std::span<const uint32_t> nodesOfThis);
    void noteStoredClasses(LayoutID, FunctionID);
    void noteLostInstanceFields();
    void noteNamesStoredByMethodsOfLostClasses();
    void noteNamesReadFromComputedLiterals();
    void noteLiteralsFilledThroughProperties();
    void noteFormsOfNamesDefinedThroughWrittenTypesOfParameters();
    static constexpr unsigned shiftOfElements = 16;
    static constexpr uint32_t severalElements = (1u << 15) - 1;
    static bool isBuiltinAmongMadeClasses(uint32_t made) { return made && made != severalClasses && !(made & layoutAmongWhatMakersLay) && !arePrimitivesAmongWhatMakersLay(made); }
    static uint32_t builtinAmongMadeClasses(uint32_t made) { return made & ((1u << shiftOfElements) - 1); }
    void noteWhatMakerLays(uint32_t& laid, uint32_t made) const
    {
        if (laid == made)
            return;
        if (arePrimitivesAmongWhatMakersLay(made)) {
            if (!laid)
                laid = made;
            else if (arePrimitivesAmongWhatMakersLay(laid))
                laid = laid == otherPrimitivesAmongWhatMakersLay || made == otherPrimitivesAmongWhatMakersLay ? otherPrimitivesAmongWhatMakersLay : laid | made;
            return;
        }
        if (arePrimitivesAmongWhatMakersLay(laid)) {
            laid = made;
            return;
        }
        const uint32_t array = BuiltinClassID(BuiltinClasses::Array).index() + 1;
        if (isBuiltinAmongMadeClasses(laid) && isBuiltinAmongMadeClasses(made) && builtinAmongMadeClasses(laid) == builtinAmongMadeClasses(made)) {
            uint32_t elements = laid >> shiftOfElements;
            uint32_t madeElements = made >> shiftOfElements;
            laid = builtinAmongMadeClasses(laid) | (!elements ? madeElements : !madeElements ? elements : severalElements) << shiftOfElements;
        } else if (!laid || (isBuiltinAmongMadeClasses(laid) && builtinAmongMadeClasses(laid) == array && isTypedArrayAmongMadeClasses(made)))
            laid = made;
        else if (!isBuiltinAmongMadeClasses(made) || builtinAmongMadeClasses(made) != array || !isTypedArrayAmongMadeClasses(laid))
            laid = severalClasses;
    }
    bool isTypedArrayAmongMadeClasses(uint32_t made) const { return isBuiltinAmongMadeClasses(made) && isTypedView(m_program.builtins().typedArrayTypeOf(BuiltinClassID(builtinAmongMadeClasses(made) - 1))); }
    uint32_t elementsOfArraysFilledIn(FunctionID);
    TypeID storedElementClass(uint32_t cell)
    {
        uint32_t laid = m_classesThatMakersLayInFields.isEmpty() ? 0 : m_classesThatMakersLayInFields[cell];
        uint32_t elements = isBuiltinAmongMadeClasses(laid) ? laid >> shiftOfElements : 0;
        return elements && elements != severalElements ? types().builtinClass(BuiltinClassID(elements - 1)) : TypeID();
    }
    Vector<uint32_t> m_classesThatMakersLayInFields;
    void noteTypedArraysStoredUnderNames();
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_typedArraysStoredUnderNames;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_typedArraysStoredInVariables;
    LayoutID knownClassFilledAt(FunctionID, const ThinSummary&, uint32_t node);
    struct ReturnedClass {
        uint32_t made { 0 };
        StringID selectedBy;
        bool isSelectedByOtherString { false };
    };
    struct ReturnedClasses {
        ValueID selector;
        Vector<ReturnedClass, 4> classes;
    };
    ReturnedClasses findReturnedClasses(FunctionID) const;
    uint32_t classReturnedByCall(FunctionID caller, const SummaryCall&, uint32_t numberOfCall, AsksForElements);
    Lock m_lockOfReturnedClasses;
    HashMap<uint32_t, ReturnedClasses, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_returnedClasses;
    Vector<std::pair<uint32_t, uint32_t>> m_copiesBetweenFields;
    void noteCopyBetweenFields(FunctionID, const ThinSummary&, const SummaryAccess& store, const LinkType& subject);
    void carryClassesAlongCopiesBetweenFields();
    uint32_t classInFieldReadAt(FunctionID, const ThinSummary&, uint32_t node);
    void noteClassesPassedToParameters();
    void noteFieldsReadAsStrings();
    BitVector m_fieldsReadAsStrings;
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

    static constexpr uint32_t formsOfDataProperties = [] {
        uint32_t forms = 0;
        for (uint32_t raw = 0; raw < 16; ++raw) {
            if (!(raw & static_cast<uint32_t>(FieldForm::IsAccessor)))
                forms |= 1u << raw;
        }
        return forms;
    }();
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
        bool isBroughtByCopy { false };
        bool isStoredByModuleLoop { false };
        bool isLaidLate { false };
        uint16_t formsThatDescriptorsCreate { 0 };
        bool isDefinedInOtherWays { false };
        bool isDefinedByDescriptors { false };
        bool isWrittenWithoutDefinition { false };
        bool hasDefaultOnPrototype { false };
        bool isStoredUnderListedKey { false };
        bool isStoredUnderWrittenName { false };
        bool isStoredByMaker { false };
        bool isStoredBehindMaker { false };
        OptionSet<FieldForm> formToPlanFor() const
        {
            uint32_t forms = formsThatDescriptorsCreate;
            if (forms && !hasOneBitSet(forms))
                forms &= formsOfDataProperties;
            if (isDefinedInOtherWays || !hasOneBitSet(forms))
                return { };
            return OptionSet<FieldForm>::fromRaw(static_cast<uint8_t>(WTF::ctz(forms)));
        }

        bool isRequiredByTypeOfThis() const { return (isRequiredBySomeTypeOfThis && !isOptionalInSomeTypeOfThis) || isGivenByConstructor || isGivenBySecondConstructor || isLaidOnObjectsOfLibrary || isStoredThroughWhatIsExpected || isBroughtByCopy || isStoredByModuleLoop || hasDefaultOnPrototype || isStoredUnderListedKey || !formToPlanFor().isEmpty(); }
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
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesCountedInLoops;
    bool m_storeIsUnderListedKey { false };
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
        uint64_t keysStoredTogether { 0 };
        struct Store {
            ValueID site;
            PropertyKeyID key;
        };
        Vector<Store> laterStoresOfKeys;
        Vector<PropertyKeyID> keysOfOtherArm;
    };
    static void findOtherArmKeys(RunOfMaker&, const BodyIndex&);
    struct LateFieldsFilled {
        LayoutID layout;
        FunctionID function;
        Vector<std::pair<PropertyKeyID, ValueID>, 8> stores;
    };
    class LateFillFinder;
    struct LateFillAllocation {
        LayoutID layout;
        FunctionID function;
        ValueID object;
    };
    BlockID blockAboveReturns(FunctionID, const BodyIndex&) const;
    bool runsOnEveryPath(FunctionID, const BodyIndex&, BlockID) const;
    void addMethodsOfConstructor(FunctionID, Vector<FunctionID, 8>& methods) const;
    BitVector findReachedMethods(LayoutID, Vector<FunctionID, 8>& methods) const;
    mutable std::pair<FunctionID, BlockID> m_blockAboveReturns;
    struct Allocation {
        LayoutID layout;
        ValueID object;
    };
    struct AllocatingFunction {
        FunctionID function;
        Vector<Allocation, 2> allocations;
    };
    Vector<AllocatingFunction> m_allocatingFunctions;
    struct AllocatedLayout {
        LayoutID layout;
        FunctionID function;
    };
    Vector<AllocatedLayout> m_allocatedLayouts;
    void findAllocatingFunctions(LateFillFinder&, const BitVector& layouts);
    void findInlineCapacities();
    static constexpr size_t minLateFieldsFilledTogether = 3;
    struct LateFieldOfMethod {
        LayoutID layout;
        FunctionID method;
        PropertyKeyID key;
    };
    Vector<LateFieldOfMethod> m_lateFieldsOfMethods;
    bool fillsMostLateFields(LayoutID, std::span<const PropertyKeyID>) const;
    void addCalledMethods(FunctionID, uint32_t object, LayoutID, Vector<FunctionID, 8>& methods) const;
    void noteLateFieldsOfMethods();
    Vector<LateFieldsFilled, 2> m_lateFieldsFilledInFunction;
    Vector<LateFieldsFilled> m_lateFieldsFilled;
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
        Vector<ValueID, 8> firstStoresOfKeys;
        uint64_t keysStoredTogether { 0 };
    };
    uint64_t findKeysStoredTogether(FunctionID, std::span<const ValueID> firstStores) const;
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
    static void sortKeysBySite(RunOfMaker&);
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
    m_layoutsCopiedInto.ensureSize(numberOfLayouts);
    m_layoutsExtendedAlone.ensureSize(numberOfLayouts);
    m_layoutsWithComputedAccessors.ensureSize(numberOfLayouts);
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
    m_gainsUnknownNames.ensureSize(numberOfLayouts);
    m_gainsListedNamesAlone.ensureSize(numberOfLayouts);
    m_isRedefinedByWrittenDefinitionsAlone.ensureSize(numberOfLayouts);
    m_sidesWithNamesDefinedPastLists.ensureSize(2 * numberOfLayouts);
    m_prototypesDefinedOnUnderKeysThatNobodyKnows.ensureSize(numberOfLayouts);
    m_prototypesGainingUnknownNames.ensureSize(numberOfLayouts);
    m_isFilledUnderComputedKeys.ensureSize(numberOfLayouts);
    m_heritages.fill(LayoutID(), numberOfLayouts);
    m_hasHeritage.ensureSize(numberOfLayouts);
    m_hasSeveralHeritages.ensureSize(numberOfLayouts);
    m_fieldsWrittenLate.ensureSize(tables.fields.size());
    m_fieldsWrittenWithoutDefinition.ensureSize(tables.fields.size());
    m_fieldsInWhichDefinitionsLayForms.ensureSize(tables.fields.size());
    m_fieldsInWhichDescriptorsMayLayAccessors.ensureSize(tables.fields.size());
    m_fieldsGivenValues.ensureSize(tables.fields.size());
    m_fieldsRedefinedWithAttributes.ensureSize(tables.fields.size());
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
    m_staticFieldsFilledFirst.ensureSize(tables.variables.size());
    m_staticFieldsReadAsProperties.ensureSize(tables.variables.size());
    m_staticFieldsDefinedByInitializers.ensureSize(tables.variables.size());
    m_staticFieldsRemovedOrRedefined.ensureSize(tables.variables.size());
}

TypeID WriterOfLinkLayouts::unlessAnything(TypeID type) const
{
    if (!type || holdsOnlyPrimitives(type))
        return type;
    return types().at(type).isAnything() ? TypeID() : type;
}

TypeID WriterOfLinkLayouts::claimedTypeAt(FlowNodeID node, Numbers numbers)
{
    const bool readsAnything = m_flow.reading() == WhatUnseenCodeDoes::Anything;
    LinkType type = numbers == Numbers::AsSlotHolds ? m_flow.typeAt(node).asSlotHolds(readsAnything) : numbers == Numbers::AsVariableHolds && readsAnything ? m_flow.typeAt(node).asVariableHolds() : m_flow.typeAt(node).withoutBoundPast32Bits();
    if (numbers == Numbers::ByKindAlone || (numbers == Numbers::AsSlotHolds && m_flow.reading() != WhatUnseenCodeDoes::Anything))
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
    return type.isNever() ? TypeID() : typesTurned().turnUnlessAnything(m_flow, types(), type);
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

uint32_t WriterOfLinkLayouts::whatIsMadeInSightAt(FunctionID id, const ThinSummary& summary, uint32_t node, AsksForElements asksForElements)
{
    if (node == noSummaryNode)
        return 0;
    const SummaryNode& made = summary.nodes[node];
    switch (made.kind) {
    case SummaryNode::Kind::ClassInstance:
        return made.first + 1;
    case SummaryNode::Kind::ResultOfCall: {
        const SummaryCall& call = summary.calls[made.first];
        if (made.second || call.newTarget != noSummaryNode)
            return 0;
        if (!call.isConstruction)
            return classReturnedByCall(id, call, made.first, asksForElements);
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
        if (constructed)
            return (constructed.index() + 1) | layoutAmongWhatMakersLay;
        if (callee || call.callee != SummaryCall::Callee::Value || call.subject == noSummaryNode)
            return 0;
        if (summary.nodes[call.subject].kind == SummaryNode::Kind::Variable) {
            auto stored = m_typedArraysStoredInVariables.find(summary.nodes[call.subject].first);
            return stored != m_typedArraysStoredInVariables.end() && isTypedArrayAmongMadeClasses(stored->value) ? stored->value : 0;
        }
        if (m_typedArraysStoredUnderNames.isEmpty() || summary.nodes[call.subject].kind != SummaryNode::Kind::ResultOfAccess)
            return 0;
        PropertyKeyID name = summary.accesses[summary.nodes[call.subject].first].key;
        auto stored = name ? m_typedArraysStoredUnderNames.find(name.bits()) : m_typedArraysStoredUnderNames.end();
        return stored != m_typedArraysStoredUnderNames.end() && isTypedArrayAmongMadeClasses(stored->value) ? stored->value : 0;
    }
    default:
        return 0;
    }
}

LayoutID WriterOfLinkLayouts::knownClassFilledAt(FunctionID id, const ThinSummary& summary, uint32_t node)
{
    const SummaryNode& filled = summary.nodes[node];
    const Function& function = m_program.at(id);
    if (filled.kind == SummaryNode::Kind::Parameter)
        return function.takesThisFromConstruction() && filled.first == function.thisParameterIndex() ? function.constructedClass : LayoutID();
    uint32_t made = filled.kind == SummaryNode::Kind::ResultOfCall ? whatIsMadeInSightAt(id, summary, node, AsksForElements::No) : 0;
    return made && made != severalClasses && made & layoutAmongWhatMakersLay ? LayoutID((made & ~layoutAmongWhatMakersLay) - 1) : LayoutID();
}

void WriterOfLinkLayouts::noteTypedArraysStoredUnderNames()
{
    auto note = [&](PropertyKeyID name, const SummaryNode& stored) {
        if (BuiltinClassID builtinClass = name ? classMadeByHeritage(m_program.builtins(), BuiltinID(stored.first)) : BuiltinClassID())
            noteWhatMakerLays(m_typedArraysStoredUnderNames.add(name.bits(), 0).iterator->value, builtinClass.index() + 1);
    };
    m_tables.typedArraysNamedInModules.fill(0, m_program.modules.size());
    Vector<uint32_t> functionsNamingTypedArrays = findIndicesInParallel(m_flow.loopsOfReading(), static_cast<unsigned>(m_program.functions.size()), [&](unsigned index) {
        if (!m_flow.reached.get(index))
            return false;
        const ThinSummary& summary = m_flow.summaries.at(FunctionID(index));
        return summary.holdsNodesOf(SummaryNode::Kind::Builtin) && std::ranges::any_of(summary.nodes, [&](const SummaryNode& node) {
            BuiltinClassID builtinClass = node.kind == SummaryNode::Kind::Builtin ? classMadeByHeritage(m_program.builtins(), BuiltinID(node.first)) : BuiltinClassID();
            return builtinClass && isTypedArrayAmongMadeClasses(builtinClass.index() + 1);
        });
    });
    for (uint32_t index : functionsNamingTypedArrays) {
        const ThinSummary& summary = m_flow.summaries.at(FunctionID(index));
        if (ModuleID module = m_program.at(FunctionID(index)).module) {
            for (const SummaryNode& node : summary.nodes) {
                BuiltinClassID builtinClass = node.kind == SummaryNode::Kind::Builtin ? classMadeByHeritage(m_program.builtins(), BuiltinID(node.first)) : BuiltinClassID();
                if (builtinClass && isTypedArrayAmongMadeClasses(builtinClass.index() + 1))
                    m_tables.typedArraysNamedInModules[module.index()] |= uint16_t(1) << m_program.builtins().typedArrayTypeOf(builtinClass);
            }
        }
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind != SummaryAccess::Kind::Read && access.kind != SummaryAccess::Kind::Has && access.value != noSummaryNode && summary.nodes[access.value].kind == SummaryNode::Kind::Builtin)
                note(access.key, summary.nodes[access.value]);
        }
        for (const SummaryEdge& edge : summary.edges) {
            const SummaryNode& to = summary.nodes[edge.to];
            if (summary.nodes[edge.from].kind != SummaryNode::Kind::Builtin)
                continue;
            if (to.kind == SummaryNode::Kind::Field)
                note(fieldAt(m_program, { LayoutID(to.first), to.second }).key, summary.nodes[edge.from]);
            else if (BuiltinClassID builtinClass = to.kind == SummaryNode::Kind::Variable ? classMadeByHeritage(m_program.builtins(), BuiltinID(summary.nodes[edge.from].first)) : BuiltinClassID())
                noteWhatMakerLays(m_typedArraysStoredInVariables.add(to.first, 0).iterator->value, builtinClass.index() + 1);
        }
    }
}

uint32_t WriterOfLinkLayouts::numbersOfConstant(const SummaryNode& node) const
{
    LinkType constant = m_flow.fromTypeID(TypeID::fromBits(node.first));
    uint16_t primitives = constant.primitives & LinkType::closed(Primitive::All) & ~Primitive::UndefinedOrNull;
    if (constant.hasUnknownObjects || constant.builtin || constant.objects != LinkType::Objects::None || constant.functions != LinkType::Functions::None || constant.classes != LinkType::Classes::None || !primitives)
        return 0;
    if (primitives & ~LinkType::closed(Primitive::Number))
        return otherPrimitivesAmongWhatMakersLay;
    return constant.counts == LinkType::Counts::BelowTwoTo31 || constant.counts == LinkType::Counts::Int32 ? numbersAmongWhatMakersLay : fractions;
}

uint32_t WriterOfLinkLayouts::numbersOfOperator(const ThinSummary& summary, const SummaryOperator& numeric, PropertyKeyID storedUnder, NumberOperands* operands, unsigned depth) const
{
    static constexpr unsigned mostDepth = 4;
    if (numeric.opcode == Opcode::Mul || numeric.opcode == Opcode::Mod)
        operands = nullptr;
    auto numbersAt = [&](uint32_t node) -> uint32_t {
        if (node == noSummaryNode)
            return mayBeFractions;
        const SummaryNode& operand = summary.nodes[node];
        if (operand.kind == SummaryNode::Kind::ResultOfOperator && depth < mostDepth)
            return numbersOfOperator(summary, summary.operators[operand.first], storedUnder, operands, depth + 1);
        if (operand.kind == SummaryNode::Kind::Constant) {
            uint32_t constant = numbersOfConstant(operand);
            return areNumbersAmongWhatMakersLay(constant) ? constant : mayBeFractions;
        }
        if (operand.kind != SummaryNode::Kind::ResultOfAccess || !storedUnder)
            return mayBeFractions;
        const SummaryAccess& read = summary.accesses[operand.first];
        if (read.key == storedUnder)
            return partsOfNumbersLaid;
        PropertyKeyID key = read.key;
        bool isElement = !key && read.kind == SummaryAccess::Kind::Read && read.keyNode != noSummaryNode && summary.nodes[read.subject].kind == SummaryNode::Kind::ResultOfAccess;
        if (isElement)
            key = summary.accesses[summary.nodes[read.subject].first].key;
        if (!key || !operands)
            return mayBeFractions;
        operands->append({ key, isElement });
        return partsOfNumbersLaid;
    };
    auto numbersOfOperands = [&] {
        return numbersAt(numeric.operands[0]) | numbersAt(numeric.operands[1]);
    };
    switch (numeric.opcode) {
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::Increment:
    case Opcode::Decrement:
        return integersOfOperators;
    case Opcode::URShift:
        return numeric.shiftsRightByConstant ? integersOfOperators : numbersAmongWhatMakersLay;
    case Opcode::Add: {
        if (numeric.step)
            return integersOfOperators;
        uint32_t first = numbersAt(numeric.operands[0]);
        uint32_t second = numbersAt(numeric.operands[1]);
        bool areNumbers = areNumbersAmongWhatMakersLay(first) && areNumbersAmongWhatMakersLay(second);
        return areNumbers ? first | second : (first | second) & ~4u;
    }
    case Opcode::Sub:
        return numeric.step ? integersOfOperators : numbersOfOperands() | numbersAmongWhatMakersLay;
    case Opcode::Mul:
    case Opcode::Mod:
        return (numbersOfOperands() & ~1u) | numbersAmongWhatMakersLay;
    case Opcode::Div:
        return fractions;
    default:
        return 0;
    }
}

void WriterOfLinkLayouts::noteNumberDependencies(StoredNumberPlace place, uint64_t index, const NumberOperands& operands)
{
    for (const NumberOperand& operand : operands)
        m_numberDependencies.append({ place, index, operand });
}

uint32_t* WriterOfLinkLayouts::findNumbersStoredAt(const NumberDependency& dependency)
{
    uint32_t* stored = nullptr;
    switch (dependency.place) {
    case StoredNumberPlace::Field:
        stored = &m_classesThatMakersLayInFields[static_cast<uint32_t>(dependency.index)];
        break;
    case StoredNumberPlace::LayoutName:
        if (auto found = m_tables.classesStoredUnderNames.find(dependency.index); found != m_tables.classesStoredUnderNames.end())
            stored = &found->value;
        break;
    case StoredNumberPlace::Name:
        if (auto found = m_tables.typedArraysHeldUnderNames.find(static_cast<uint32_t>(dependency.index)); found != m_tables.typedArraysHeldUnderNames.end())
            stored = &found->value;
        break;
    }
    return stored && arePrimitivesAmongWhatMakersLay(*stored) && *stored != otherPrimitivesAmongWhatMakersLay ? stored : nullptr;
}

uint32_t WriterOfLinkLayouts::numbersOfOperand(const NumberOperand& operand) const
{
    auto found = m_tables.typedArraysHeldUnderNames.find(operand.key.bits());
    uint32_t held = found == m_tables.typedArraysHeldUnderNames.end() ? 0 : found->value;
    if (!operand.isElement)
        return areNumbersAmongWhatMakersLay(held) && held & 1 ? held & 3 : 2;
    if (!isTypedArrayAmongMadeClasses(held))
        return 2;
    TypedArrayType type = m_program.builtins().typedArrayTypeOf(BuiltinClassID(builtinAmongMadeClasses(held) - 1));
    return (hasIntegerElements(type) && type != TypeUint32) || type == TypeUint8Clamped ? 1 : 2;
}

void WriterOfLinkLayouts::propagateNumberDependencies()
{
    auto keyOfOperand = [](const NumberDependency& dependency) {
        return dependency.operand.key.bits();
    };
    std::ranges::stable_sort(m_numberDependencies, { }, keyOfOperand);
    Vector<uint32_t> open;
    for (uint32_t spread : { 1u, 2u }) {
        for (const NumberDependency& dependency : m_numberDependencies) {
            if (open.isEmpty() || open.last() != keyOfOperand(dependency))
                open.append(keyOfOperand(dependency));
        }
        while (!open.isEmpty()) {
            for (const NumberDependency& dependency : std::ranges::equal_range(m_numberDependencies, open.takeLast(), { }, keyOfOperand)) {
                uint32_t* stored = findNumbersStoredAt(dependency);
                uint32_t added = stored ? numbersOfOperand(dependency.operand) & spread & ~*stored : 0;
                if (!added)
                    continue;
                *stored |= added;
                if (dependency.place == StoredNumberPlace::Name)
                    open.append(static_cast<uint32_t>(dependency.index));
            }
        }
    }
    noteFieldsOfSteppedIntegers();
    m_numberDependencies = { };
}

void WriterOfLinkLayouts::noteFieldsOfSteppedIntegers()
{
    if (m_classesThatMakersLayInFields.isEmpty())
        return;
    m_tables.fieldsOfSteppedIntegers.ensureSize(m_classesThatMakersLayInFields.size());
    for (size_t cell = 0; cell < m_classesThatMakersLayInFields.size(); ++cell) {
        if (areIntegersAmongWhatMakersLay(m_classesThatMakersLayInFields[cell]))
            m_tables.fieldsOfSteppedIntegers.quickSet(cell);
    }
    for (const NumberDependency& dependency : m_numberDependencies) {
        if (dependency.place == StoredNumberPlace::Field)
            m_tables.fieldsOfSteppedIntegers.quickClear(static_cast<size_t>(dependency.index));
    }
}

uint32_t WriterOfLinkLayouts::whatIsStoredInSightAt(FunctionID id, const ThinSummary& summary, uint32_t node, PropertyKeyID storedUnder, NumberOperands* operands)
{
    if (node == noSummaryNode)
        return 0;
    const SummaryNode& stored = summary.nodes[node];
    if (stored.kind == SummaryNode::Kind::ResultOfOperator)
        return numbersOfOperator(summary, summary.operators[stored.first], storedUnder, operands);
    return stored.kind == SummaryNode::Kind::Constant ? numbersOfConstant(stored) : whatIsMadeInSightAt(id, summary, node);
}

auto WriterOfLinkLayouts::findReturnedClasses(FunctionID id) const -> ReturnedClasses
{
    ReturnedClasses result;
    const Function& function = m_program.at(id);
    const BodyIndex* index = function.isCoroutine() || function.results.size() != 1 || isLibraryCode(m_program, function) ? nullptr : bodiesLookedAt(m_flow.looks).find(id);
    if (!index)
        return result;
    if (m_flow.typeAt(m_flow.result(id, 0)).classes == LinkType::Classes::None)
        return result;
    struct Edge {
        BlockID to;
        BlockID from;
        uint32_t targetIndex;
        std::span<const ValueID> arguments;
    };
    Vector<Edge> edges;
    Vector<std::pair<ValueID, uint32_t>, 8> open;
    static constexpr uint32_t otherString = UINT32_MAX;
    Vector<uint32_t> caseAbove;
    BitVector isInHandler;
    BlockID selecting;
    caseAbove.fill(0, index->numberOfBlocks());
    isInHandler.ensureSize(index->numberOfBlocks());
    auto caseOn = [&](BlockID from, uint32_t targetIndex) -> uint32_t {
        if (!result.selector || from != selecting)
            return 0;
        return targetIndex < index->terminator(selecting).as<TerminatorKind::SwitchString>().strings().size() ? targetIndex + 1 : otherString;
    };
    for (BlockID block : index->blocksInDominatorOrder()) {
        if (block != entryBlock) {
            BlockID above = index->immediateDominator(block);
            std::optional<BodyIncomingEdge> edge = index->onlyIncomingEdge(block);
            uint32_t chosen = edge ? caseOn(edge->from, edge->targetIndex) : 0;
            caseAbove[block.index()] = chosen ? chosen : caseAbove[above.index()];
            if (isInHandler.quickGet(above.index()) || index->block(block).isHandlerBlock())
                isInHandler.quickSet(block.index());
        }
        BodyTerminator terminator = index->terminator(block);
        if (terminator.kind() == TerminatorKind::Return) {
            if (ValueID returned = terminator.as<TerminatorKind::Return>().valueOperand())
                open.append({ returned, caseAbove[block.index()] });
        }
        if (terminator.kind() == TerminatorKind::SwitchString && !result.selector) {
            ValueID subject = index->stripCasts(terminator.as<TerminatorKind::SwitchString>().subjectOperand());
            if (index->isParameter(subject) && index->isInBlock(subject, entryBlock)) {
                result.selector = subject;
                selecting = block;
            }
        }
        uint32_t targetIndex = 0;
        for (BodyTarget target : terminator.targets())
            edges.append(Edge { target.block(), block, targetIndex++, target.arguments() });
    }
    std::ranges::stable_sort(edges, { }, &Edge::to);
    auto classMadeAt = [&](const BodyValue& value) -> uint32_t {
        if (value.opcode() == Opcode::NewArray || (value.opcode() == Opcode::Data && m_program.at(value.as<Opcode::Data>().data()).root.tryAs<DataNode::Array>()))
            return BuiltinClassID(BuiltinClasses::Array).index() + 1;
        if (value.opcode() != Opcode::Construct)
            return 0;
        ValueID callee = index->stripCasts(value.calleeOperand());
        BuiltinClassID builtinClass = index->isParameter(callee) || index->opcodeOf(callee) != Opcode::Builtin ? BuiltinClassID() : classMadeByHeritage(m_program.builtins(), index->value(callee).as<Opcode::Builtin>().builtin());
        return builtinClass ? builtinClass.index() + 1 : 0;
    };
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> joinsGoneThrough;
    for (size_t next = 0; next < open.size(); ++next) {
        auto [operand, place] = open[next];
        ValueID value = index->stripCasts(operand);
        if (index->isParameter(value)) {
            BlockID block = index->blockOf(value);
            if (block == entryBlock || index->isEnteredByHandlerEdge(block) || !joinsGoneThrough.add(value.index()).isNewEntry)
                continue;
            uint32_t position = value.index() - index->block(block).parameter(0).index();
            for (const Edge& edge : std::ranges::equal_range(edges, block, { }, &Edge::to)) {
                if (position >= edge.arguments.size() || !edge.arguments[position])
                    continue;
                uint32_t chosen = caseOn(edge.from, edge.targetIndex);
                open.append({ edge.arguments[position], chosen ? chosen : caseAbove[edge.from.index()] });
            }
            continue;
        }
        BlockID block = index->blockOf(value);
        ReturnedClass returned { classMadeAt(index->value(value)), { }, false };
        if (!returned.made || isInHandler.quickGet(block.index()))
            continue;
        uint32_t chosen = caseAbove[block.index()] ? caseAbove[block.index()] : place;
        if (chosen == otherString)
            returned.isSelectedByOtherString = true;
        else if (chosen)
            returned.selectedBy = index->terminator(selecting).as<TerminatorKind::SwitchString>().strings()[chosen - 1];
        result.classes.append(returned);
    }
    return result;
}

uint32_t WriterOfLinkLayouts::elementsOfArraysFilledIn(FunctionID id)
{
    const ThinSummary& summary = m_flow.summaries.at(id);
    uint32_t elements = 0;
    for (const SummaryAccess& access : summary.accesses) {
        if (access.kind != SummaryAccess::Kind::Store || access.key || summary.nodes[access.subject].kind != SummaryNode::Kind::ClassInstance || summary.nodes[access.subject].first != BuiltinClassID(BuiltinClasses::Array).index())
            continue;
        uint32_t made = whatIsMadeInSightAt(id, summary, access.value, AsksForElements::No);
        if (!isTypedArrayAmongMadeClasses(made) || (elements && elements != made))
            return 0;
        elements = made;
    }
    return elements;
}

uint32_t WriterOfLinkLayouts::classReturnedByCall(FunctionID caller, const SummaryCall& call, uint32_t numberOfCall, AsksForElements asksForElements)
{
    const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[caller.index()] + numberOfCall];
    FunctionID callee;
    auto isOnlyCallee = [&](FunctionID found) {
        callee = callee && callee != found ? FunctionID() : found;
        return !!callee;
    };
    if (!std::ranges::all_of(callees.listed(), isOnlyCallee))
        return 0;
    for (uint32_t shared : callees.shared()) {
        if (!std::ranges::all_of(m_flow.calleesOfSharedCallsAsSettled()[shared].listed(), isOnlyCallee))
            return 0;
    }
    if (!callee || call.passesList)
        return 0;
    ReturnedClasses returned;
    {
        Locker locker { m_lockOfReturnedClasses };
        returned = m_returnedClasses.ensure(callee.index(), [&] {
            return findReturnedClasses(callee);
        }).iterator->value;
    }
    if (returned.classes.isEmpty())
        return 0;
    StringID written;
    if (const BodyIndex* index = returned.selector ? bodiesLookedAt(m_flow.looks).find(caller) : nullptr) {
        BodyValue site = index->value(call.site);
        uint32_t first = m_program.at(callee).firstParameterIndex();
        uint32_t position = returned.selector.index() - first;
        if (site.opcode() == Opcode::Call && returned.selector.index() >= first && position < site.variadicOperands().size()) {
            ValueID argument = index->stripCasts(site.variadicOperands()[position]);
            if (!index->isParameter(argument) && index->opcodeOf(argument) == Opcode::String)
                written = index->value(argument).as<Opcode::String>().string();
        }
    }
    uint32_t made = 0;
    for (const ReturnedClass& returnedClass : returned.classes) {
        if (!written || returnedClass.selectedBy == written || (!returnedClass.selectedBy && !returnedClass.isSelectedByOtherString))
            noteWhatMakerLays(made, returnedClass.made);
    }
    if (asksForElements == AsksForElements::Yes && made == BuiltinClassID(BuiltinClasses::Array).index() + 1)
        made |= elementsOfArraysFilledIn(callee) << shiftOfElements;
    return made == severalClasses ? 0 : made;
}

void WriterOfLinkLayouts::noteCopyBetweenFields(FunctionID id, const ThinSummary& summary, const SummaryAccess& store, const LinkType& subject)
{
    if (store.value == noSummaryNode || summary.nodes[store.value].kind != SummaryNode::Kind::ResultOfAccess || subject.objects != LinkType::Objects::Layout)
        return;
    const SummaryAccess& read = summary.accesses[summary.nodes[store.value].first];
    if (read.kind != SummaryAccess::Kind::Read || !read.key || !store.key || read.isToContents())
        return;
    const LinkType& holder = typeAt(id, read.subject);
    if (holder.objects != LinkType::Objects::Layout)
        return;
    std::optional<FieldReference> from = findField(m_program, holder.layout, read.key);
    std::optional<FieldReference> to = findField(m_program, subject.layout, store.key);
    if (from && to)
        m_copiesBetweenFields.append({ cellOf(declarationOf(m_program, *from)), cellOf(declarationOf(m_program, *to)) });
}

uint32_t WriterOfLinkLayouts::classInFieldReadAt(FunctionID id, const ThinSummary& summary, uint32_t node)
{
    if (node == noSummaryNode || summary.nodes[node].kind != SummaryNode::Kind::ResultOfAccess)
        return 0;
    const SummaryAccess& read = summary.accesses[summary.nodes[node].first];
    if (read.kind != SummaryAccess::Kind::Read || !read.key || read.isToContents())
        return 0;
    const LinkType& holder = typeAt(id, read.subject);
    std::optional<FieldReference> field = holder.objects == LinkType::Objects::Layout ? findField(m_program, holder.layout, read.key) : std::nullopt;
    uint32_t laid = field ? m_classesThatMakersLayInFields[cellOf(declarationOf(m_program, *field))] : 0;
    return isBuiltinAmongMadeClasses(laid) ? laid : 0;
}

void WriterOfLinkLayouts::noteFieldsReadAsStrings()
{
    if (m_flow.reading() != WhatUnseenCodeDoes::OnlyReadsAndCalls)
        return;
    const PropertyKeyID charCodeAt = m_program.keyOf(BuiltinNames::charCodeAt);
    const PropertyKeyID codePointAt = m_program.keyOf(BuiltinNames::codePointAt);
    Vector<uint32_t> cells = collectInParallel<uint32_t>(m_flow.loopsOfReading(), static_cast<unsigned>(m_program.functions.size()), [&](unsigned index, Vector<uint32_t>& cells) {
        FunctionID id(index);
        if (!m_flow.reached.get(index) || isLibraryCode(m_program, m_program.at(id)))
            return;
        const ThinSummary& summary = m_flow.summaries.at(id);
        for (const SummaryCall& call : summary.calls) {
            if (call.callee != SummaryCall::Callee::Name || !call.key || (call.key != charCodeAt && call.key != codePointAt) || call.receiver == noSummaryNode)
                continue;
            const SummaryNode& receiver = summary.nodes[call.receiver];
            if (receiver.kind != SummaryNode::Kind::ResultOfAccess)
                continue;
            const SummaryAccess& read = summary.accesses[receiver.first];
            if (read.kind != SummaryAccess::Kind::Read || !read.key || read.isToContents())
                continue;
            const LinkType& holder = typeAt(id, read.subject);
            auto noteFieldOf = [&](LayoutID layout) {
                if (std::optional<FieldReference> field = findField(m_program, layout, read.key))
                    cells.append(cellOf(declarationOf(m_program, *field)));
            };
            if (holder.objects == LinkType::Objects::Layout)
                noteFieldOf(holder.layout);
            else if (holder.objects == LinkType::Objects::Set)
                std::ranges::for_each(m_flow.layoutsOfSet(holder), noteFieldOf);
            else if (holder.objects == LinkType::Objects::Family)
                m_program.at(m_flow.families.layoutsOf(holder.family)).forEach(noteFieldOf);
        }
    });
    for (uint32_t cell : cells) {
        m_fieldsReadAsStrings.ensureSize(cell + 1);
        if (!m_fieldsReadAsStrings.quickSet(cell))
            m_flow.build.statistics().count(StatisticID::FieldsReadAsStrings);
    }
}

void WriterOfLinkLayouts::noteClassesPassedToParameters()
{
    if (m_classesThatMakersLayInFields.isEmpty())
        return;
    auto forEachCallee = [&](FunctionID id, uint32_t call, NOESCAPE const Invocable<void(FunctionID)> auto& functor) {
        const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[id.index()] + call];
        std::ranges::for_each(callees.listed(), functor);
        for (uint32_t shared : callees.shared())
            std::ranges::for_each(m_flow.calleesOfSharedCallsAsSettled()[shared].listed(), functor);
    };
    struct ArgumentPassed {
        FunctionID caller;
        uint32_t call;
        uint32_t place;
        uint32_t made;
    };
    Vector<ArgumentPassed> arguments = collectInParallel<ArgumentPassed>(m_flow.loopsOfReading(), static_cast<unsigned>(m_program.functions.size()), [&](unsigned index, Vector<ArgumentPassed>& found) {
        FunctionID id(index);
        if (!m_flow.reached.get(index) || m_flow.escapedFunctions.get(index) || isLibraryCode(m_program, m_program.at(id)))
            return;
        const ThinSummary& summary = m_flow.summaries.at(id);
        for (uint32_t number = 0; number < summary.calls.size(); ++number) {
            const SummaryCall& call = summary.calls[number];
            if (call.isConstruction || call.passesList || call.callee == SummaryCall::Callee::Builtin || !call.numberOfArguments)
                continue;
            for (uint32_t place = 0; place < call.numberOfArguments; ++place) {
                uint32_t node = summary.arguments[call.firstArgument + place];
                if (node == noSummaryNode)
                    continue;
                const SummaryNode& argument = summary.nodes[node];
                bool isMadeHere = argument.kind == SummaryNode::Kind::ClassInstance || (argument.kind == SummaryNode::Kind::ResultOfCall && summary.calls[argument.first].isConstruction);
                if (!isMadeHere && argument.kind != SummaryNode::Kind::ResultOfAccess)
                    continue;
                uint32_t made = isMadeHere ? whatIsMadeInSightAt(id, summary, node) : classInFieldReadAt(id, summary, node);
                if (isBuiltinAmongMadeClasses(made))
                    found.append(ArgumentPassed { id, number, place, builtinAmongMadeClasses(made) });
            }
        }
    });
    for (const ArgumentPassed& argument : arguments) {
        forEachCallee(argument.caller, argument.call, [&](FunctionID callee) {
            const Function& function = m_program.at(callee);
            if (!m_flow.summaries.at(callee).has(SummaryTrait::HasLoops) || isLibraryCode(m_program, function) || argument.place >= function.numberOfPositionalParameters())
                return;
            uint32_t& passed = m_tables.classesPassedToParameters.add(m_flow.parameter(callee, function.firstParameterIndex() + argument.place).index(), argument.made).iterator->value;
            passed = passed == argument.made ? argument.made : severalClasses;
        });
    }
    m_tables.classesPassedToParameters.removeIf([](const auto& passed) {
        return passed.value == severalClasses;
    });
}

LayoutID WriterOfLinkLayouts::recordStoredInSight(const ThinSummary& summary, uint32_t node)
{
    auto recordMadeAt = [&](const SummaryNode& made) {
        LayoutID layout = made.kind == SummaryNode::Kind::Allocation ? LayoutID(made.first) : LayoutID();
        return layout && m_program.at(layout).kind == LayoutKind::Record ? layout : LayoutID();
    };
    const SummaryNode& stored = summary.nodes[node];
    if (stored.kind != SummaryNode::Kind::ResultOfCall || stored.second)
        return recordMadeAt(stored);
    const SummaryCall& call = summary.calls[stored.first];
    if (call.isConstruction || call.callee != SummaryCall::Callee::Binding || m_program.at(call.function).isCoroutine())
        return { };
    const ThinSummary& callee = m_flow.summaries.at(call.function);
    LayoutID returned;
    for (const SummaryEdge& edge : callee.edges) {
        const SummaryNode& to = callee.nodes[edge.to];
        if (to.kind != SummaryNode::Kind::Result || to.first)
            continue;
        LayoutID made = recordMadeAt(callee.nodes[edge.from]);
        if (!made || (returned && returned != made))
            return { };
        returned = made;
    }
    return returned;
}

uint32_t WriterOfLinkLayouts::classStoredByMember(LayoutID holder, FunctionID id, const ThinSummary& summary, uint32_t node, std::span<const uint32_t> nodesOfThis)
{
    if (node == noSummaryNode)
        return 0;
    if (uint32_t made = whatIsMadeInSightAt(id, summary, node))
        return made;
    if (LayoutID record = recordStoredInSight(summary, node))
        return (record.index() + 1) | layoutAmongWhatMakersLay;
    const SummaryNode& stored = summary.nodes[node];
    if (stored.kind != SummaryNode::Kind::ResultOfCall || stored.second)
        return 0;
    const SummaryCall& call = summary.calls[stored.first];
    if (call.isConstruction || call.callee != SummaryCall::Callee::Name || std::ranges::find(nodesOfThis, call.receiver) == nodesOfThis.end())
        return 0;
    const LinkFlow::NameOnPrototype* name = m_flow.findNameOnPrototype(holder, call.key);
    const LinkType method = name ? m_flow.typeAt(name->storedInSight) : LinkType();
    if (method.functions != LinkType::Functions::Closure || !m_flow.reached.get(method.function.index()))
        return 0;
    ReturnedClasses returned;
    {
        Locker locker { m_lockOfReturnedClasses };
        returned = m_returnedClasses.ensure(method.function.index(), [&] {
            return findReturnedClasses(method.function);
        }).iterator->value;
    }
    uint32_t made = 0;
    for (const ReturnedClass& returnedClass : returned.classes)
        noteWhatMakerLays(made, returnedClass.made);
    return made == severalClasses ? 0 : made;
}

void WriterOfLinkLayouts::noteStoredClasses(LayoutID holder, FunctionID id)
{
    std::optional<unsigned> receiver = m_flow.reached.get(id.index()) ? m_program.at(id).thisParameterIndex() : std::nullopt;
    if (!receiver)
        return;
    const ThinSummary& summary = m_flow.summaries.at(id);
    const SummaryNode parameter { SummaryNode::Kind::Parameter, *receiver, 0 };
    Vector<uint32_t, 4> nodesOfThis;
    for (uint32_t node = 0; node < summary.nodes.size(); ++node) {
        if (summary.nodes[node] == parameter)
            nodesOfThis.append(node);
    }
    for (const SummaryEdge& edge : summary.edges) {
        if (summary.nodes[edge.from] == parameter && summary.nodes[edge.to].kind == SummaryNode::Kind::Join && edge.passes == TypeTable::object)
            nodesOfThis.append(edge.to);
    }
    for (const SummaryAccess& access : summary.accesses) {
        if (access.kind != SummaryAccess::Kind::Store || !access.key || access.isToContents() || access.isCopySpelledByLoop || !nodesOfThis.contains(access.subject))
            continue;
        std::optional<FieldReference> field = findField(m_program, holder, access.key);
        uint32_t made = field ? classStoredByMember(holder, id, summary, access.value, nodesOfThis.span()) : 0;
        if (!made)
            continue;
        noteWhatMakerLays(m_classesThatMakersLayInFields[cellOf(declarationOf(m_program, *field))], made);
        m_flow.build.statistics().count(StatisticID::LostInstanceFieldsExpected);
    }
}

void WriterOfLinkLayouts::noteLostInstanceFields()
{
    if (m_classesThatMakersLayInFields.isEmpty())
        return;
    auto isLost = [&](LayoutID layout) {
        return m_flow.lostLayouts.get(layout.index()) || hasEscaped(layout.index());
    };
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        const Layout& layout = m_program.layouts[index];
        if (layout.kind == LayoutKind::Instance && layout.constructor && isLost(LayoutID(index)) && m_program.at(layout.constructor).constructedClass == LayoutID(index) && m_program.at(layout.constructor).role != FunctionRole::Library)
            noteStoredClasses(LayoutID(index), layout.constructor);
    }
    for (const LinkFlow::NameOnPrototype& name : m_flow.namesOnPrototypes()) {
        if (!isLost(name.layout))
            continue;
        const LinkType held = m_flow.typeAt(name.storedInSight);
        if (held.functions == LinkType::Functions::Closure && m_program.at(held.function).role != FunctionRole::Library)
            noteStoredClasses(name.layout, held.function);
    }
}

void WriterOfLinkLayouts::carryClassesAlongCopiesBetweenFields()
{
    std::ranges::sort(m_copiesBetweenFields);
    m_copiesBetweenFields.shrink(std::ranges::unique(m_copiesBetweenFields).begin() - m_copiesBetweenFields.begin());
    Vector<uint32_t> open;
    for (auto [from, to] : m_copiesBetweenFields) {
        if (open.isEmpty() || open.last() != from)
            open.append(from);
    }
    while (!open.isEmpty()) {
        uint32_t carried = m_classesThatMakersLayInFields[open.last()];
        auto copies = std::ranges::equal_range(m_copiesBetweenFields, open.takeLast(), { }, &std::pair<uint32_t, uint32_t>::first);
        if (!carried || carried == severalClasses)
            continue;
        for (auto [from, to] : copies) {
            uint32_t held = m_classesThatMakersLayInFields[to];
            noteWhatMakerLays(m_classesThatMakersLayInFields[to], carried);
            if (m_classesThatMakersLayInFields[to] != held)
                open.append(to);
        }
    }
    m_copiesBetweenFields = { };
}

void WriterOfLinkLayouts::noteNamesStoredThroughWhatFieldsAreExpectedToHold()
{
    if (m_classesThatMakersLayInFields.isEmpty())
        return;
    using StoreThroughRead = std::pair<FunctionID, uint32_t>;
    Vector<StoreThroughRead> stores = collectInParallel<StoreThroughRead>(m_flow.loopsOfReading(), static_cast<unsigned>(m_program.functions.size()), [&](unsigned index, Vector<StoreThroughRead>& found) {
        if (!m_flow.reached.get(index))
            return;
        FunctionID id(index);
        const ThinSummary& summary = m_flow.summaries.at(id);
        for (uint32_t place = 0; place < summary.accesses.size(); ++place) {
            const SummaryAccess& access = summary.accesses[place];
            if (access.kind != SummaryAccess::Kind::Store || !access.key || access.isToContents() || summary.nodes[access.subject].kind != SummaryNode::Kind::ResultOfAccess)
                continue;
            const SummaryAccess& read = summary.accesses[summary.nodes[access.subject].first];
            if (read.kind != SummaryAccess::Kind::Read || !read.key || read.isToContents())
                continue;
            if (LinkType::Objects known = typeAt(id, access.subject).objects; known == LinkType::Objects::None || known == LinkType::Objects::Lost)
                found.append({ id, place });
        }
    });
    TypeReader types(m_program);
    for (auto [id, place] : stores) {
        const Function& function = m_program.at(id);
        const ThinSummary& summary = m_flow.summaries.at(id);
        const SummaryAccess& access = summary.accesses[place];
        const SummaryAccess& read = summary.accesses[summary.nodes[access.subject].first];
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

void WriterOfLinkLayouts::noteNamesStoredByMethodsOfLostClasses()
{
    if (m_flow.reading() != WhatUnseenCodeDoes::OnlyReadsAndCalls)
        return;
    Vector<uint32_t, 8> nodesOfThis;
    for (const LinkFlow::NameOnPrototype& name : m_flow.namesOnPrototypes()) {
        LayoutID layout = name.layout;
        if (!m_flow.lostLayouts.get(layout.index()) && !hasEscaped(layout.index()))
            continue;
        const LinkType& held = m_flow.typeAt(name.storedInSight);
        FunctionID method = held.functions == LinkType::Functions::Closure ? held.function : FunctionID();
        std::optional<unsigned> parameter = method && m_flow.reached.get(method.index()) ? m_program.at(method).thisParameterIndex() : std::nullopt;
        if (!parameter)
            continue;
        const ThinSummary& summary = m_flow.summaries.at(method);
        const SummaryNode nodeOfThis { SummaryNode::Kind::Parameter, *parameter, 0 };
        bool joinsAreListed = false;
        auto isThis = [&](uint32_t node) {
            if (summary.nodes[node].kind != SummaryNode::Kind::Join)
                return summary.nodes[node] == nodeOfThis;
            if (!std::exchange(joinsAreListed, true)) {
                nodesOfThis.shrink(0);
                for (const SummaryEdge& edge : summary.edges) {
                    if (summary.nodes[edge.from] == nodeOfThis && summary.nodes[edge.to].kind == SummaryNode::Kind::Join && edge.passes == TypeTable::object)
                        nodesOfThis.append(edge.to);
                }
            }
            return nodesOfThis.contains(node);
        };
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind != SummaryAccess::Kind::Store || !access.key || access.isToContents())
                continue;
            if (!isThis(access.subject))
                continue;
            if (findField(m_program, layout, access.key) || comesToAccessorAbove(layout, access.key) || !noteNameNotListed(layout, access.key))
                continue;
            uint64_t layoutAndKey = static_cast<uint64_t>(layout.index()) << 32 | access.key.bits();
            m_namesStoredThroughWhatIsExpected.add(layoutAndKey);
            m_namesNotListed[m_pairsNotListed.get(layoutAndKey)].isStoredThroughWhatIsExpected = true;
            m_flow.build.statistics().count(StatisticID::NamesStoredByMethodsAppended);
        }
    }
}

void WriterOfLinkLayouts::noteNamesReadFromComputedLiterals()
{
    const ParallelLoops& loops = m_flow.loopsOfReading();
    unsigned numberOfFunctions = static_cast<unsigned>(m_program.functions.size());
    Vector<LayoutID> layoutsOfComputedLiterals = collectInParallel<LayoutID>(loops, numberOfFunctions, [&](unsigned index, Vector<LayoutID>& found) {
        if (!m_flow.reached.get(index))
            return;
        const ThinSummary& summary = m_flow.summaries.at(FunctionID(index));
        if (!summary.definesComputedKeys)
            return;
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind != SummaryAccess::Kind::Define || access.key || summary.nodes[access.subject].kind != SummaryNode::Kind::Allocation)
                continue;
            LayoutID layout(summary.nodes[access.subject].first);
            if (m_program.at(layout).kind == LayoutKind::Record && m_isFilledUnderComputedKeys.quickGet(layout.index()))
                found.append(layout);
        }
    });
    if (layoutsOfComputedLiterals.isEmpty())
        return;
    BitVector computedLiterals(m_program.layouts.size());
    for (LayoutID layout : layoutsOfComputedLiterals)
        computedLiterals.quickSet(layout.index());
    HashMap<uint32_t, Vector<PropertyKeyID, maxComputedLiteralNames>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> namesOfLiterals;
    Vector<std::pair<LayoutID, PropertyKeyID>> namesRead;
    auto noteName = [&](LayoutID layout, PropertyKeyID key) {
        if (!computedLiterals.quickGet(layout.index()) || findField(m_program, layout, key) || !canHoldAppendedFields(layout) || !canNameAppendedField(layout, key))
            return;
        Vector<PropertyKeyID, maxComputedLiteralNames>& names = namesOfLiterals.add(layout.index(), Vector<PropertyKeyID, maxComputedLiteralNames>()).iterator->value;
        if (names.contains(key))
            return;
        names.append(key);
        namesRead.append({ layout, key });
        if (m_program.at(layout).fields.size() + names.size() > maxComputedLiteralNames)
            computedLiterals.quickClear(layout.index());
    };
    using NameRead = std::pair<LayoutID, PropertyKeyID>;
    Vector<NameRead> namesReadOffLiterals = collectInParallel<NameRead>(loops, numberOfFunctions, [&](unsigned index, Vector<NameRead>& found) {
        if (!m_flow.reached.get(index))
            return;
        FunctionID id(index);
        const ThinSummary& summary = m_flow.summaries.at(id);
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind != SummaryAccess::Kind::Read || !access.key)
                continue;
            FlowNodeID node = m_flow.nodeOf(id, access.subject);
            if (!m_flow.holdsLayoutsAt(node))
                continue;
            LinkType subject = m_flow.typeAt(node);
            std::span<const LayoutID> layouts;
            if (subject.objects == LinkType::Objects::Layout)
                layouts = std::span(&subject.layout, 1);
            else if (subject.objects == LinkType::Objects::Set)
                layouts = m_flow.layoutsOfSet(subject);
            for (LayoutID layout : layouts) {
                if (computedLiterals.quickGet(layout.index()))
                    found.append({ layout, access.key });
            }
        }
    });
    for (auto [layout, key] : namesReadOffLiterals)
        noteName(layout, key);
    for (auto [layout, key] : namesRead) {
        if (computedLiterals.quickGet(layout.index()) && noteNameNotListed(layout, key))
            m_tables.computedLiterals.set(layout.index());
    }
}

void WriterOfLinkLayouts::noteLiteralsFilledThroughProperties()
{
    BitVector propertiesWithElements;
    BitVector propertiesWithMembers;
    struct FilledProperty {
        PropertyKeyID property;
        LayoutID literal;
        bool isMember { false };
    };
    Vector<FilledProperty> filledProperties = collectInParallel<FilledProperty>(m_flow.loopsOfReading(), static_cast<unsigned>(m_program.functions.size()), [&](unsigned index, Vector<FilledProperty>& filled) {
        if (!m_flow.reached.get(index))
            return;
        const ThinSummary& summary = m_flow.summaries.at(FunctionID(index));
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind != SummaryAccess::Kind::Store && access.kind != SummaryAccess::Kind::Define)
                continue;
            if (access.isToContents())
                continue;
            if (access.key && access.value != noSummaryNode && summary.nodes[access.value].kind == SummaryNode::Kind::Allocation) {
                LayoutID stored(summary.nodes[access.value].first);
                if (const Layout& layout = m_program.at(stored); layout.kind == LayoutKind::Record && !layout.name && layout.fields.isEmpty())
                    filled.append({ access.key, stored });
            }
            if (summary.nodes[access.subject].kind != SummaryNode::Kind::ResultOfAccess)
                continue;
            const SummaryAccess& read = summary.accesses[summary.nodes[access.subject].first];
            if (read.kind == SummaryAccess::Kind::Read && read.key && !read.isToContents())
                filled.append({ read.key, LayoutID(), access.key && !m_program.isIndexLike(access.key) });
        }
    });
    for (const FilledProperty& filled : filledProperties) {
        if (!filled.literal)
            (filled.isMember ? propertiesWithMembers : propertiesWithElements).set(filled.property.index());
    }
    for (auto [property, literal, isMember] : filledProperties) {
        if (!literal || !propertiesWithElements.get(property.index()) || propertiesWithMembers.get(property.index()) || m_isFilledUnderComputedKeys.quickGet(literal.index()))
            continue;
        m_isFilledUnderComputedKeys.quickSet(literal.index());
        m_flow.build.statistics().count(StatisticID::PropertiesKeptAsDictionaries);
    }
}

void WriterOfLinkLayouts::noteFormsOfNamesDefinedThroughWrittenTypesOfParameters()
{
    using DefinitionThroughParameter = std::pair<FunctionID, uint32_t>;
    Vector<DefinitionThroughParameter> definitions = collectInParallel<DefinitionThroughParameter>(m_flow.loopsOfReading(), static_cast<unsigned>(m_program.functions.size()), [&](unsigned index, Vector<DefinitionThroughParameter>& found) {
        if (!m_flow.reached.get(index))
            return;
        const ThinSummary& summary = m_flow.summaries.at(FunctionID(index));
        for (uint32_t place = 0; place < summary.accesses.size(); ++place) {
            const SummaryAccess& access = summary.accesses[place];
            if (access.kind == SummaryAccess::Kind::Define && access.isByDescriptor && access.formCreated && access.key && summary.nodes[access.subject].kind == SummaryNode::Kind::Parameter)
                found.append({ FunctionID(index), place });
        }
    });
    TypeReader types(m_program);
    for (auto [id, place] : definitions) {
        const ThinSummary& summary = m_flow.summaries.at(id);
        const SummaryAccess& access = summary.accesses[place];
        const LinkType& arrives = typeAt(id, access.subject);
        if (arrives.objects != LinkType::Objects::None && arrives.objects != LinkType::Objects::Lost)
            continue;
        const DeclaredType* declared = m_program.at(id).declaredEntryParameter(summary.nodes[access.subject].first);
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

ALWAYS_INLINE void WriterOfLinkLayouts::lookAtEdges(FunctionID id, const ThinSummary& summary)
{
    for (VariableID variable : summary.functionVariablesStoredInto)
        m_variablesStoredInto.quickSet(variable.index());
    m_layoutsAllocatedHere.shrink(0);
    for (const SummaryNode& node : summary.holdsNodesOf(SummaryNode::Kind::Allocation) ? summary.nodes.span() : std::span<const SummaryNode>()) {
        if (node.kind == SummaryNode::Kind::Allocation)
            m_layoutsAllocatedHere.append(LayoutID(node.first));
    }
    std::ranges::sort(m_layoutsAllocatedHere);
    for (LayoutID layout : m_layoutsAllocatedHere)
        m_allocatedLayouts.append({ layout, id });
    for (LayoutID layout : m_asksWhatMakersGive ? m_layoutsAllocatedHere.span() : std::span<LayoutID> { }) {
        if (FunctionID& maker = m_onlyFunctionsThatMakeObjects[layout.index()]; maker && maker != id)
            m_layoutsMadeInSeveralFunctions.quickSet(layout.index());
        else
            maker = id;
    }

    for (PropertyKeyID key : summary.keysOfTestedFields)
        m_keysOfTestedFields.set(key.index());

    const Function& function = m_program.at(id);
    for (const SummaryEdge& edge : summary.holdsNodesOf(SummaryNode::Kind::Field, SummaryNode::Kind::Variable) ? summary.edges.span() : std::span<const SummaryEdge>()) {
        const SummaryNode& to = summary.nodes[edge.to];
        if (to.kind == SummaryNode::Kind::Field) {
            LayoutID layout(to.first);
            m_fieldFactsFromEdges.fieldsGivenValues.quickSet(cellOf({ layout, to.second }));
            if (uint32_t made = m_fieldFactsFromEdges.whatMakersLayInFields.isEmpty() ? 0 : whatIsStoredInSightAt(id, summary, edge.from))
                noteWhatMakerLays(m_fieldFactsFromEdges.whatMakersLayInFields[cellOf({ layout, to.second })], made);
            uint32_t listed = summary.nodes[edge.from].kind == SummaryNode::Kind::Constant && WrittenUnit::fromBits(summary.nodes[edge.from].second) && !edge.passes ? summary.nodes[edge.from].second : otherThanOneUnit;
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

void WriterOfLinkLayouts::noteAffixedNamesOfStore(FunctionID id, const SummaryAccess& access)
{
    const LinkType& subject = typeAt(id, access.subject);
    if (subject.objects != LinkType::Objects::Layout || m_program.at(subject.layout).kind != LayoutKind::Record)
        return;
    LayoutID layout = subject.layout;
    for (PropertyKeyID name : m_flow.findAffixedNamesOfStore(id, access)) {
        if (findField(m_program, layout, name) || comesToAccessorAbove(layout, name) || !noteNameNotListed(layout, name))
            continue;
        m_namesNotListed[m_pairsNotListed.get(static_cast<uint64_t>(layout.index()) << 32 | name.bits())].isStoredByModuleLoop = true;
    }
}

static bool runsInEveryIteration(const BodyIndex& index, BlockID block)
{
    LoopID loop = index.loopOf(block);
    if (!loop)
        return false;
    BlockID head = index.loop(loop).head;
    return std::ranges::all_of(index.membersOf(loop), [&](BlockID from) {
        bool closesLoop = false;
        for (BodyTarget target : index.terminator(from).targets())
            closesLoop |= target.block() == head;
        return !closesLoop || index.dominates(block, from);
    });
}

void WriterOfLinkLayouts::noteListedStoreNames(FunctionID id, const ThinSummary& summary, const SummaryAccess& access)
{
    const SummaryNode& made = summary.nodes[access.subject];
    LayoutID literal = made.kind == SummaryNode::Kind::Allocation ? LayoutID(made.first) : LayoutID();
    const BodyIndex* index = literal && access.site && m_program.at(literal).kind == LayoutKind::Record ? bodiesLookedAt(m_flow.looks).find(id) : nullptr;
    if (!index || index->isParameter(access.site) || index->opcodeOf(access.site) != Opcode::ElemSet)
        return;
    std::optional<LinkFlow::ListedObject> listed = m_flow.findListedObject(id, *index, index->value(access.site).as<Opcode::ElemSet>().propertyOperand());
    ValueID object = listed && listed->function == id ? listed->object : ValueID();
    if (object && !index->isParameter(object) && index->opcodeOf(object) == Opcode::CheckInitialized)
        object = index->stripCasts(index->value(object).operand(0));
    if (!object || !runsInEveryIteration(*index, index->blockOf(access.site)))
        return;
    bool isCapture = index->isParameter(object) && index->blockOf(object) == entryBlock && object.index() < m_program.at(id).captures.size();
    bool isVariable = !index->isParameter(object) && (index->opcodeOf(object) == Opcode::GetModuleVariable || index->opcodeOf(object) == Opcode::GetEnvironmentField);
    const LinkType& records = isCapture || isVariable ? m_flow.typeAt(m_flow.nodeOf(id, listed->node)) : LinkType();
    if (records.objects != LinkType::Objects::Layout || m_program.at(records.layout).kind != LayoutKind::Record || records.layout == literal)
        return;
    auto note = [&](PropertyKeyID name) {
        if (!name || m_program.at(name).kind != PropertyKeyKind::Text || m_program.isIndexLike(name) || findField(m_program, literal, name) || comesToAccessorAbove(literal, name) || !noteNameNotListed(literal, name))
            return;
        m_namesNotListed[m_pairsNotListed.get(static_cast<uint64_t>(literal.index()) << 32 | name.bits())].isStoredUnderListedKey = true;
    };
    for (const Field& field : m_program.at(records.layout).fields)
        note(field.computedKey || field.flags.contains(FieldFlags::IsMethod) ? PropertyKeyID() : field.key);
    m_flow.forEachAppendedField(records.layout, [&](PropertyKeyID name, FlowNodeID) {
        note(name);
    });
}

static bool readsUnderKeyOfStore(const BodyIndex& index, ValueID site)
{
    LoopID loop = index.loopOf(index.blockOf(site));
    if (!loop)
        return false;
    auto store = index.value(site).as<Opcode::ElemSet>();
    ValueID object = index.stripCasts(store.objectOperand());
    ValueID key = index.stripCasts(store.propertyOperand());
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        for (BodyValue value : index.block(member).values()) {
            auto names = [&](ValueID readOff, ValueID readUnder) { return index.stripCasts(readOff) == object && index.stripCasts(readUnder) == key; };
            if (value.opcode() == Opcode::ElemGet && names(value.as<Opcode::ElemGet>().objectOperand(), value.as<Opcode::ElemGet>().propertyOperand()))
                return true;
            if (value.opcode() == Opcode::ElemHas && names(value.as<Opcode::ElemHas>().objectOperand(), value.as<Opcode::ElemHas>().propertyOperand()))
                return true;
        }
        return false;
    });
}

static bool holdsInstancesOfOneClass(const LinkFlow& flow, const LinkType& type)
{
    if (type.objects != LinkType::Objects::Layout || type.hasUnknownObjects || type.holdsLostBuiltins || type.builtin || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.primitives & LinkType::closed(Primitive::AnyString))
        return false;
    const Layout& layout = flow.program.at(type.layout);
    return layout.kind == LayoutKind::Instance && !layout.has(LayoutFlags::IsConstructorFunction) && !layout.has(LayoutFlags::IsOfCreatedObject) && !layout.has(LayoutFlags::HasHeritage) && !layout.base && flow.program.layoutsExpectedBelow(type.layout).empty() && !listsAccessorInLinkLayouts(flow, type.layout);
}

static bool holdsFollowedInstances(const LinkFlow& flow, const LinkType& type)
{
    if (type.objects != LinkType::Objects::Layout && type.objects != LinkType::Objects::Set)
        return false;
    if (type.hasUnknownObjects || type.holdsLostBuiltins || type.builtin || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.primitives & LinkType::closed(Primitive::AnyString))
        return false;
    bool areFollowed = true;
    flow.forEachLayoutOf(type, [&](LayoutID layout) {
        areFollowed = areFollowed && flow.program.at(layout).kind == LayoutKind::Instance && !flow.program.at(layout).has(LayoutFlags::IsOfCreatedObject) && (flow.hasPristinePrototypes(layout) || flow.hasFollowedPrototypes(layout)) && !listsAccessorInLinkLayouts(flow, layout);
    });
    return areFollowed;
}

NEVER_INLINE bool WriterOfLinkLayouts::appendStoresUnderNames(FunctionID id, const ThinSummary& summary, const SummaryAccess& access, Vector<SummaryAccess>& stores)
{
    Vector<PropertyKeyID, 16> names(m_flow.findNamesOfStore(id, access));
    const LinkType& holders = typeAt(id, access.subject);
    if (!names.isEmpty()) {
        bool holdsPrototypes = holders.objects == LinkType::Objects::Prototype || holders.objects == LinkType::Objects::Prototypes;
        if (!holdsPrototypes && !m_flow.holdsOnlyPlainRecords(holders) && !holdsInstancesOfOneClass(m_flow, holders) && !holdsFollowedInstances(m_flow, holders))
            return false;
    } else {
        if (!holdsFollowedInstances(m_flow, holders) || (access.isCopySpelledByLoop && m_flow.followsCopySpelledByLoop(holders, typeAt(id, m_flow.sourceOfCopySpelledByLoop(id, access.site)))))
            return false;
        names = m_flow.findListedNamesOfStore(id, access);
        if (names.isEmpty())
            return false;
    }
    if (m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls)
        noteComputedMemberAssignment(id, summary, access);
    if (const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(id); index && readsUnderKeyOfStore(*index, access.site)) {
        m_flow.forEachLayoutOf(holders, [&](LayoutID holder) {
            for (PropertyKeyID name : names)
                m_namesCountedInLoops.add(static_cast<uint64_t>(holder.index()) << 32 | name.bits());
        });
    }
    for (PropertyKeyID name : names) {
        stores.append(access);
        stores.last().keyNode = noSummaryNode;
        stores.last().key = name;
    }
    return true;
}

bool WriterOfLinkLayouts::mayStorePrototype(FunctionID id, const SummaryAccess& access, PropertyKeyID key) const
{
    auto holdsObjects = [](const LinkType& type) {
        return type.objects != LinkType::Objects::None || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || type.builtin || type.hasUnknownObjects || type.holdsLostBuiltins;
    };
    if (!key && access.keyNode != noSummaryNode) {
        const LinkType& names = typeAt(id, access.keyNode);
        if (!holdsObjects(names) && !(names.primitives & Primitive::String))
            return false;
    }
    if (access.value == noSummaryNode)
        return true;
    const LinkType& stored = typeAt(id, access.value);
    return holdsObjects(stored) || stored.primitives & LinkType::closed(Primitive::Null);
}

Vector<Vector<WriterOfLinkLayouts::AccessInSummary>> WriterOfLinkLayouts::findRelevantAccesses() const
{
    return collectChunksInParallel<AccessInSummary>(m_flow.loopsOfReading(), static_cast<unsigned>(m_program.functions.size()), [&](unsigned index, Vector<AccessInSummary>& found) {
        if (!m_flow.reached.get(index))
            return;
        FunctionID id(index);
        const ThinSummary& summary = m_flow.summaries.at(id);
        for (const SummaryAccess& access : summary.accesses) {
            if (access.isToContents() || access.kind == SummaryAccess::Kind::Has)
                continue;
            if (access.kind == SummaryAccess::Kind::Read && !access.isPartOfConstruction)
                continue;
            found.append({ id, access.subject == noSummaryNode ? FlowNodeID() : m_flow.nodeOf(id, access.subject), &access });
        }
    });
}

ALWAYS_INLINE void WriterOfLinkLayouts::prefetchAccess(const LinkFlow& flow, std::span<const AccessInSummary> accesses)
{
    static constexpr size_t ahead = 12;
    if (accesses.size() <= ahead)
        return;
    __builtin_prefetch(accesses[ahead].access);
    if (accesses[ahead].subject)
        flow.prefetchTypeAt(accesses[ahead].subject);
}

ALWAYS_INLINE void WriterOfLinkLayouts::lookAtAccesses(FunctionID id, const ThinSummary& summary, std::span<const AccessInSummary>& accessesLeft)
{
    m_thisOfObjectBeingMade = UINT32_MAX;
    bool constructs = m_program.at(id).constructedClass && m_program.at(id).takesThisFromConstruction();
    FlowNodeID heirsThatRun = m_flow.findHeirsThatRun(id);
    if (const Function& function = m_program.at(id); m_asksWhatMakersGive && (function.kind == FunctionKind::Initializer || function.takesThisFromConstruction()))
        m_thisOfObjectBeingMade = function.thisParameterIndex().value_or(UINT32_MAX);
    Vector<SummaryAccess> storesUnderNames;
    size_t numberOfAccesses = 0;
    while (numberOfAccesses < accessesLeft.size() && accessesLeft[numberOfAccesses].function == id)
        ++numberOfAccesses;
    std::span<const AccessInSummary> accesses = accessesLeft;
    accessesLeft = accessesLeft.subspan(numberOfAccesses);
    for (size_t place = 0; place < numberOfAccesses + storesUnderNames.size(); ++place) {
        if (place < numberOfAccesses)
            prefetchAccess(m_flow, accesses.subspan(place));
        const SummaryAccess& access = place < numberOfAccesses ? *accesses[place].access : storesUnderNames[place - numberOfAccesses];
        if (access.isToContents())
            continue;
        SetForScope storeIsUnderListedKey(m_storeIsUnderListedKey, place >= numberOfAccesses);
        if (access.kind == SummaryAccess::Kind::Store && !access.key && access.keyNode != noSummaryNode) [[unlikely]] {
            if (appendStoresUnderNames(id, summary, access, storesUnderNames))
                continue;
            noteAffixedNamesOfStore(id, access);
            noteListedStoreNames(id, summary, access);
        }
        Traits traits { Trait::AddsWhatIsNotListed, Trait::MayGiveValue };
        switch (access.kind) {
        case SummaryAccess::Kind::Read:
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
        PropertyKeyID key = m_flow.keyOf(id, access);
        if (access.kind == SummaryAccess::Kind::Store && (!key || m_program.nameClassOf(key) == NameClass::Proto)) {
            if (mayStorePrototype(id, access, key))
                traits.add(Trait::MayChangePrototype);
            else if (m_program.at(id).role != FunctionRole::Library)
                m_flow.build.statistics().count(StatisticID::StoresKeepingPrototypes);
        }
        const LinkType& subject = typeAt(id, access.subject);
        noteNameWrittenOnPrototype(id, summary, access, key, subject);
        if (access.key && access.key.index() < m_placesOfFirstStoresOfKeys.size() && m_placesOfFirstStoresOfKeys[access.key.index()] == UINT32_MAX)
            m_placesOfFirstStoresOfKeys[access.key.index()] = m_numberOfKeysStored++;
        if (!key && access.keyNode != noSummaryNode && m_flow.reading() != WhatUnseenCodeDoes::Anything && LinkFlow::isSymbolAsFarAsIsSeen(typeAt(id, access.keyNode)))
            continue;
        noteStoreOfMaker(summary, access, key);
        noteLateFieldsFilledByAccess(id, summary, access, key, subject);
        traits.add(traitsOfStoreOfMaker(id, summary, access));
        if (LayoutID constructed = traits.containsAny({ Trait::GivesUndefinedToObjectBeingMade, Trait::FillsObjectBeingConstructed }) ? m_program.at(id).constructedClass : LayoutID(); constructed && summary.nodes[access.subject].kind == SummaryNode::Kind::Parameter && subject.objects == LinkType::Objects::Lost) {
            if (std::optional<FieldReference> found = findField(m_program, constructed, access.key)) {
                (traits.contains(Trait::FillsObjectBeingConstructed) ? m_fieldsFilledByConstructors : m_fieldsGivenUndefinedByMakers).quickSet(cellOf(*found));
                m_layoutsGivenUndefinedByMakers.quickSet(constructed.index());
            }
        }
        NumberOperands operands;
        uint32_t storedInSight = key && !m_classesThatMakersLayInFields.isEmpty() ? whatIsStoredInSightAt(id, summary, access.value, key, &operands) : 0;
        if (storedInSight && !liesInThread(key)) {
            noteWhatMakerLays(m_tables.typedArraysHeldUnderNames.add(key.bits(), 0).iterator->value, storedInSight);
            noteNumberDependencies(StoredNumberPlace::Name, key.bits(), operands);
        }
        if (uint32_t made = subject.objects == LinkType::Objects::Layout || subject.objects == LinkType::Objects::Set ? storedInSight : 0) {
            m_flow.forEachLayoutOf(subject, [&](LayoutID holder) {
                if (std::optional<FieldReference> found = findField(m_program, holder, key)) {
                    uint32_t cell = cellOf(declarationOf(m_program, *found));
                    noteWhatMakerLays(m_classesThatMakersLayInFields[cell], made);
                    noteNumberDependencies(StoredNumberPlace::Field, cell, operands);
                } else if (!liesInThread(key) && !(made & layoutAmongWhatMakersLay)) {
                    uint64_t name = LinkTables::layoutAndName(holder, key);
                    noteWhatMakerLays(m_tables.classesStoredUnderNames.add(name, 0).iterator->value, made);
                    noteNumberDependencies(StoredNumberPlace::LayoutName, name, operands);
                }
            });
        } else if (key && !m_classesThatMakersLayInFields.isEmpty()) {
            noteCopyBetweenFields(id, summary, access, subject);
            if (uint32_t made = access.isCopySpelledByLoop ? 0 : whatIsMadeInSightAt(id, summary, access.value); isTypedArrayAmongMadeClasses(made)) {
                LayoutID filled = knownClassFilledAt(id, summary, access.subject);
                if (std::optional<FieldReference> found = filled ? findField(m_program, filled, key) : std::nullopt)
                    noteWhatMakerLays(m_classesThatMakersLayInFields[cellOf(declarationOf(m_program, *found))], made);
            }
        }
        if (constructs && key && (subject.objects == LinkType::Objects::Lost || subject.objects == LinkType::Objects::None) && findNameThatConstructorStores(id, summary, access))
            noteWriteInto(m_program.at(id).constructedClass, key, traits);
        noteChangeOfClosure(subject, key);
        noteStaticFieldStore(id, summary, access, key, subject);
        noteNameDefinedPastLists(access.receiver != noSummaryNode ? typeAt(id, access.receiver) : subject, key, access.kind == SummaryAccess::Kind::Define);
        bool isUnderPrototype = key && m_program.nameClassOf(key) == NameClass::Prototype;
        if (!key || (isUnderPrototype && !access.setsFlagsAlone))
            noteWriteOfPrototypeProperty(subject, key, access.kind == SummaryAccess::Kind::Store && access.storesObjectMadeForIt);
        if (access.kind != SummaryAccess::Kind::Store || !isUnderPrototype)
            noteNameGivenToClosure(subject, key, access.laysValueInSightUnderName());
        if (key && m_keysOfFunctionNames.get(key.index()))
            noteStoreOfFunctionName(id, access, subject, key);
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
        const SummaryNode& stored = summary.nodes[access.subject];
        m_storedObjectIsAllocatedInBody = access.kind == SummaryAccess::Kind::Store && (access.subjectIsMadeInBody || (stored.kind == SummaryNode::Kind::ResultOfCall && summary.calls[stored.first].isConstruction));
        noteWrite(subject, key, traits);
        m_storedObjectIsAllocatedInBody = false;
        if (m_flow.mayStoreIntoOlderObjectThatSuperCallAnswered(id, access))
            noteStoreIntoOlderObject(id, access, key, traits);
        m_definitionAtHand = nullptr;
    }
    for (LateFieldsFilled& filled : m_lateFieldsFilledInFunction) {
        if (filled.stores.size() >= minLateFieldsFilledTogether)
            m_lateFieldsFilled.append(WTF::move(filled));
    }
    m_lateFieldsFilledInFunction.shrink(0);
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

static ValueID stripChecksInLinkLayouts(const BodyIndex& index, ValueID value)
{
    value = index.stripCasts(value);
    while (value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::CheckInitialized)
        value = index.stripCasts(index.value(value).operand(0));
    return value;
}

static bool isSameReadInLinkLayouts(const BodyIndex& index, ValueID a, ValueID b, unsigned depth = 3)
{
    a = stripChecksInLinkLayouts(index, a);
    b = stripChecksInLinkLayouts(index, b);
    if (!a || !b || a == b)
        return a && a == b;
    if (!depth || index.isParameter(a) || index.isParameter(b) || index.opcodeOf(a) != index.opcodeOf(b))
        return false;
    if (index.opcodeOf(a) == Opcode::GetModuleVariable)
        return index.value(a).variable() == index.value(b).variable();
    if (index.opcodeOf(a) != Opcode::PropGet || index.value(a).isFreeName() || index.value(b).isFreeName())
        return false;
    PropertyKeyID key = index.value(a).key();
    return key && key == index.value(b).key() && isSameReadInLinkLayouts(index, index.value(a).objectOperand(), index.value(b).objectOperand(), depth - 1);
}

static PropertyKeyID findArrayKeyInLinkLayouts(const BodyIndex& index, ValueID array)
{
    array = stripChecksInLinkLayouts(index, array);
    bool isNamed = array && !index.isParameter(array) && index.opcodeOf(array) == Opcode::PropGet && !index.value(array).isFreeName();
    return isNamed ? index.value(array).key() : PropertyKeyID();
}

static ElementBound findElementBoundInLinkLayouts(const BodyIndex& index, const Program& program, ValueID bound, ValueID array)
{
    static constexpr OpcodeSet numbers { Opcode::Integer, Opcode::Double };
    bound = bound ? index.stripCasts(bound) : ValueID();
    if (!bound || (!index.isParameter(bound) && numbers.contains(index.opcodeOf(bound))))
        return ElementBound::Unknown;
    if (index.isParameter(bound) || index.opcodeOf(bound) != Opcode::PropGet || index.value(bound).isFreeName() || !index.value(bound).key())
        return ElementBound::Other;
    ValueID counted = index.value(bound).objectOperand();
    if (index.value(bound).key() == program.keyOf(BuiltinNames::length) && isSameReadInLinkLayouts(index, counted, array))
        return ElementBound::ArrayLength;
    array = stripChecksInLinkLayouts(index, array);
    return findArrayKeyInLinkLayouts(index, array) && isSameReadInLinkLayouts(index, counted, index.value(array).objectOperand()) ? ElementBound::CountOfHolder : ElementBound::Other;
}

static ElementBound findStoreBoundInLinkLayouts(const BodyIndex& index, const Program& program, BlockID store, ValueID read, ValueID& boundOfCounter)
{
    if (index.hasIrreducibleFlow() || index.isParameter(read) || index.opcodeOf(read) != Opcode::ElemGet)
        return ElementBound::Unknown;
    LoopID loop = index.loopOf(index.blockOf(read));
    if (!loop || !index.isInLoop(store, loop))
        return ElementBound::Unknown;
    BlockID head = index.loop(loop).head;
    BodyBlock blockOfHead = index.block(head);
    ValueID counter = index.stripCasts(index.value(read).propertyOperand());
    if (!counter || !index.isParameter(counter) || index.blockOf(counter) != head)
        return ElementBound::Unknown;
    uint32_t position = 0;
    while (position < blockOfHead.numberOfParameters() && blockOfHead.parameter(position) != counter)
        ++position;
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (target.block() != head)
                continue;
            ValueID stepped = position < target.arguments().size() ? index.stripCasts(target.arguments()[position]) : ValueID();
            if (!index.dominates(store, member) || !stepped || index.isParameter(stepped) || !steps.contains(index.opcodeOf(stepped)) || index.stripCasts(index.value(stepped).operand(0)) != counter)
                return ElementBound::Unknown;
        }
    }
    BodyTerminator end = index.terminator(head);
    ValueID condition = end.kind() == TerminatorKind::Branch ? index.stripCasts(end.operand(0)) : ValueID();
    static constexpr OpcodeSet relations { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };
    if (!condition || index.isParameter(condition) || !relations.contains(index.opcodeOf(condition)))
        return ElementBound::Unknown;
    ValueID left = index.stripCasts(index.value(condition).operand(0));
    ValueID right = index.stripCasts(index.value(condition).operand(1));
    boundOfCounter = left == counter ? right : right == counter ? left : ValueID();
    return findElementBoundInLinkLayouts(index, program, boundOfCounter, index.value(read).objectOperand());
}

void WriterOfLinkLayouts::noteLateFieldsFilledByAccess(FunctionID id, const ThinSummary& summary, const SummaryAccess& access, PropertyKeyID key, const LinkType& subject)
{
    const SummaryNode& stored = summary.nodes[access.subject];
    bool isAllocatedHere = stored.kind == SummaryNode::Kind::Allocation || (stored.kind == SummaryNode::Kind::ResultOfCall && summary.calls[stored.first].isConstruction);
    if (!key || access.kind != SummaryAccess::Kind::Store || access.isPartOfConstruction || isAllocatedHere || subject.objects != LinkType::Objects::Layout || findField(m_program, subject.layout, key))
        return;
    if (stored.kind == SummaryNode::Kind::Parameter && stored.first == m_program.at(id).thisParameterIndex())
        m_lateFieldsOfMethods.append({ subject.layout, id, key });
    if (!access.site)
        return;
    size_t place = m_lateFieldsFilledInFunction.findIf([&](const LateFieldsFilled& filled) {
        return filled.layout == subject.layout;
    });
    if (place == notFound) {
        if (m_lateFieldsFilledInFunction.size() == longestListGoneThrough)
            return;
        place = m_lateFieldsFilledInFunction.size();
        m_lateFieldsFilledInFunction.append(LateFieldsFilled { subject.layout, id, { } });
    }
    if (Vector<std::pair<PropertyKeyID, ValueID>, 8>& stores = m_lateFieldsFilledInFunction[place].stores; stores.size() < maxFieldsInOrder)
        stores.append({ key, access.site });
}

BlockID WriterOfLinkLayouts::blockAboveReturns(FunctionID id, const BodyIndex& index) const
{
    if (m_blockAboveReturns.first == id)
        return m_blockAboveReturns.second;
    BlockID above;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        BlockID block(number);
        if (!index.isReachable(block))
            continue;
        TerminatorKind end = index.terminator(block).kind();
        if (end != TerminatorKind::Return && end != TerminatorKind::ReturnCall)
            continue;
        if (!above)
            above = block;
        while (!index.dominates(above, block))
            above = index.immediateDominator(above);
    }
    m_blockAboveReturns = { id, above };
    return above;
}

bool WriterOfLinkLayouts::runsOnEveryPath(FunctionID id, const BodyIndex& index, BlockID block) const
{
    if (index.hasIrreducibleFlow())
        return false;
    for (LoopID loop = index.loopOf(block); loop; loop = index.loop(loop).parent) {
        BlockID head = index.loop(loop).head;
        for (BlockID member : index.membersOf(loop)) {
            for (BodyTarget target : index.terminator(member).targets()) {
                if (target.block() == head && !index.dominates(block, member))
                    return false;
            }
        }
        block = head;
    }
    BlockID above = blockAboveReturns(id, index);
    return above && index.dominates(block, above);
}

static bool isAllocatedUpToInLinkLayouts(const BodyIndex& index, ValueID object, ValueID bound)
{
    LoopID loop = index.loopOf(index.blockOf(object));
    if (!loop)
        return false;
    BodyTerminator end = index.terminator(index.loop(loop).head);
    ValueID condition = end.kind() == TerminatorKind::Branch ? index.stripCasts(end.operand(0)) : ValueID();
    static constexpr OpcodeSet relations { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };
    if (!condition || index.isParameter(condition) || !relations.contains(index.opcodeOf(condition)))
        return false;
    for (uint32_t side = 0; side < 2; ++side) {
        ValueID most = index.stripCasts(index.value(condition).operand(side));
        if (isSameReadInLinkLayouts(index, most, bound))
            return true;
        if (!most || index.isParameter(most) || index.opcodeOf(most) != Opcode::PropGet || index.value(most).isFreeName() || !index.value(most).key())
            continue;
        for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
            for (BodyValue value : index.block(BlockID(block)).values()) {
                if (value.opcode() == Opcode::PropSet && !value.isFreeName() && value.key() == index.value(most).key() && isSameReadInLinkLayouts(index, value.valueOperand(), bound))
                    return true;
            }
        }
    }
    return false;
}

void WriterOfLinkLayouts::findInlineCapacities()
{
    constexpr size_t mostProperties = 32;
    BitVector layoutsWithPrototypeData;
    for (const LinkFlow::NameOnPrototype& name : m_flow.namesOnPrototypes()) {
        const LinkType& stored = m_flow.typeAt(name.stored);
        if (stored.primitives || stored.objects != LinkType::Objects::None)
            layoutsWithPrototypeData.set(name.layout.index());
    }
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
    m_tables.inlineCapacities.fill(0, numberOfLayouts);
    Vector<PropertyKeyID, 16> keys;
    for (uint32_t number = 0; number < numberOfLayouts; ++number) {
        LayoutID id(number);
        const Layout& layout = m_program.at(id);
        if (m_tables.appendedKeysOf(id).empty())
            continue;
        uint8_t& capacity = m_tables.inlineCapacities[number];
        if (layout.kind == LayoutKind::Record) {
            capacity = std::min(layout.fields.size(), mostProperties);
            continue;
        }
        FunctionID constructor = layout.constructor;
        bool isClass = constructor && m_program.at(constructor).kind == FunctionKind::BaseConstructor && m_program.at(constructor).constructedClass == id;
        if (!constructor || (!isClass && m_program.at(constructor).kind != FunctionKind::Plain) || !m_flow.reached.get(constructor.index()))
            continue;
        std::optional<unsigned> own = m_program.at(constructor).thisParameterIndex();
        const ThinSummary& summary = m_flow.summaries.at(constructor);
        keys.shrink(0);
        if (isClass) {
            auto noteStore = [&](PropertyKeyID key) {
                if (key && keys.size() < mostProperties && !keys.contains(key) && (findField(m_program, id, key) || std::ranges::contains(m_tables.appendedKeysOf(id), key)))
                    keys.append(key);
            };
            for (PropertyKeyID key : summary.namesStored)
                noteStore(key);
            for (const SummaryAccess& access : summary.accesses) {
                if (access.kind == SummaryAccess::Kind::Store)
                    noteStore(m_flow.keyOf(constructor, access));
            }
            if (!keys.isEmpty())
                capacity = keys.size();
            else if (!layoutsWithPrototypeData.get(number))
                capacity = JSFinalObject::defaultInlineCapacity;
            continue;
        }
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind != SummaryAccess::Kind::Store || !own || summary.nodes[access.subject] != SummaryNode { SummaryNode::Kind::Parameter, *own, 0 })
                continue;
            if (PropertyKeyID key = m_flow.keyOf(constructor, access); key && keys.size() < mostProperties && !keys.contains(key))
                keys.append(key);
        }
        if (!keys.isEmpty())
            capacity = keys.size();
        else if (own && m_flow.prototypeKeepsConstructor(id) && !layoutsWithPrototypeData.get(number))
            capacity = JSFinalObject::defaultInlineCapacity;
    }
}

void WriterOfLinkLayouts::addCalledMethods(FunctionID caller, uint32_t object, LayoutID layout, Vector<FunctionID, 8>& methods) const
{
    auto isObject = [&](uint32_t node) {
        if (node == object || node == noSummaryNode)
            return node != noSummaryNode;
        const LinkType& type = typeAt(caller, node);
        return layout && type.objects == LinkType::Objects::Layout && type.layout == layout;
    };
    const ThinSummary& summary = m_flow.summaries.at(caller);
    auto callees = m_flow.calleesOfCallsAsSettled();
    PropertyKeyID call = m_program.keyOf(BuiltinNames::call);
    PropertyKeyID apply = m_program.keyOf(BuiltinNames::apply);
    for (uint32_t number = 0; number < summary.calls.size(); ++number) {
        const SummaryCall& made = summary.calls[number];
        if (made.isConstruction)
            continue;
        bool isForwarded = made.key && (made.key == call || made.key == apply) && made.numberOfArguments && isObject(summary.arguments[made.firstArgument]);
        if (!isForwarded && !isObject(made.receiver))
            continue;
        for (FunctionID callee : callees[m_flow.firstCallOfFunction[caller.index()] + number].listed())
            methods.append(callee);
    }
}

void WriterOfLinkLayouts::addMethodsOfConstructor(FunctionID constructor, Vector<FunctionID, 8>& methods) const
{
    if (!m_flow.reached.get(constructor.index()))
        return;
    const ThinSummary& summary = m_flow.summaries.at(constructor);
    LayoutID made = m_program.at(constructor).constructedClass;
    std::optional<unsigned> own = m_program.at(constructor).thisParameterIndex();
    uint32_t object = 0;
    while (object < summary.nodes.size() && !(own && summary.nodes[object] == SummaryNode { SummaryNode::Kind::Parameter, *own, 0 }))
        ++object;
    addCalledMethods(constructor, object < summary.nodes.size() ? object : noSummaryNode, made, methods);
}

BitVector WriterOfLinkLayouts::findReachedMethods(LayoutID layout, Vector<FunctionID, 8>& methods) const
{
    BitVector seen;
    for (size_t next = 0; next < methods.size(); ++next) {
        FunctionID method = methods[next];
        if (!seen.add(method.index()))
            continue;
        const ThinSummary& summary = m_flow.summaries.at(method);
        std::optional<unsigned> own = m_program.at(method).thisParameterIndex();
        uint32_t object = 0;
        while (object < summary.nodes.size() && !(own && summary.nodes[object] == SummaryNode { SummaryNode::Kind::Parameter, *own, 0 }))
            ++object;
        addCalledMethods(method, object < summary.nodes.size() ? object : noSummaryNode, layout, methods);
    }
    return seen;
}

void WriterOfLinkLayouts::noteLateFieldsOfMethods()
{
    if (m_lateFieldsOfMethods.isEmpty())
        return;
    auto callees = m_flow.calleesOfCallsAsSettled();
    HashMap<uint32_t, Vector<FunctionID, 8>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> methodsOfLayouts;
    for (const LateFieldOfMethod& field : m_lateFieldsOfMethods)
        methodsOfLayouts.add(field.layout.index(), Vector<FunctionID, 8> { });
    for (const AllocatingFunction& allocating : m_allocatingFunctions) {
        FunctionID id = allocating.function;
        const ThinSummary& summary = m_flow.summaries.at(id);
        for (uint32_t number = 0; number < summary.calls.size(); ++number) {
            const SummaryCall& call = summary.calls[number];
            if (call.isConstruction || call.receiver == noSummaryNode)
                continue;
            const SummaryNode& receiver = summary.nodes[call.receiver];
            bool isNew = receiver.kind == SummaryNode::Kind::Allocation || (receiver.kind == SummaryNode::Kind::ResultOfCall && summary.calls[receiver.first].isConstruction);
            if (!isNew)
                continue;
            const LinkType& type = typeAt(id, call.receiver);
            if (type.objects != LinkType::Objects::Layout)
                continue;
            auto found = methodsOfLayouts.find(type.layout.index());
            if (found == methodsOfLayouts.end())
                continue;
            for (FunctionID callee : callees[m_flow.firstCallOfFunction[id.index()] + number].listed())
                found->value.append(callee);
        }
    }
    AOT::stableSort(m_lateFieldsOfMethods, { }, [](const LateFieldOfMethod& field) {
        return field.layout.index();
    });
    std::span<const LateFieldOfMethod> fields = m_lateFieldsOfMethods.span();
    for (size_t first = 0, end = 0; first < fields.size(); first = end) {
        LayoutID filled = fields[first].layout;
        while (end < fields.size() && fields[end].layout == filled)
            ++end;
        Vector<FunctionID, 8>& methods = methodsOfLayouts.find(filled.index())->value;
        for (LayoutID layout = filled; layout; layout = m_program.at(layout).base) {
            FunctionID constructor = m_program.at(layout).constructor;
            if (!constructor || !m_flow.reached.get(constructor.index()))
                break;
            addMethodsOfConstructor(constructor, methods);
        }
        BitVector seen = findReachedMethods(filled, methods);
        for (const LateFieldOfMethod& field : fields.subspan(first, end - first)) {
            if (seen.get(field.method.index()))
                m_tables.lateFieldsStoredThroughThis.append(LinkTables::lateFieldOf(field.layout, field.key));
        }
    }
}

bool WriterOfLinkLayouts::fillsMostLateFields(LayoutID layout, std::span<const PropertyKeyID> keys) const
{
    std::span<const PropertyKeyID> appended = m_tables.appendedKeysOf(layout);
    size_t numberFilled = std::ranges::count_if(appended, [&](PropertyKeyID key) {
        return std::ranges::contains(keys, key);
    });
    return numberFilled >= minLateFieldsFilledTogether && numberFilled + appended.size() / 4 >= appended.size();
}

static constexpr size_t maxLaterStoresInConstructor = 8;

void WriterOfLinkLayouts::findOtherArmKeys(RunOfMaker& run, const BodyIndex& index)
{
    auto isInFrontOrIn = [&](ValueID site, BlockID arm) {
        return index.blockOf(site) == arm || index.dominates(index.blockOf(site), arm);
    };
    std::ranges::sort(run.laterStoresOfKeys, { }, &RunOfMaker::Store::site);
    auto other = std::ranges::find_if(run.laterStoresOfKeys, [&](const RunOfMaker::Store& store) {
        ValueID first = run.firstStoresOfKeys[run.keys.find(store.key)];
        return first && !isInFrontOrIn(first, index.blockOf(store.site));
    });
    if (other == run.laterStoresOfKeys.end())
        return;
    BlockID arm = index.blockOf(other->site);
    Vector<RunOfMaker::Store, 16> stores;
    for (size_t place = 0; place < run.keys.size(); ++place) {
        if (ValueID site = run.firstStoresOfKeys[place]; site && isInFrontOrIn(site, arm))
            stores.append({ site, run.keys[place] });
    }
    for (const RunOfMaker::Store& store : run.laterStoresOfKeys) {
        if (isInFrontOrIn(store.site, arm))
            stores.append(store);
    }
    std::ranges::sort(stores, { }, &RunOfMaker::Store::site);
    for (const RunOfMaker::Store& store : stores) {
        if (!run.keysOfOtherArm.contains(store.key))
            run.keysOfOtherArm.append(store.key);
    }
    if (run.keysOfOtherArm.size() <= run.keys.size() && std::ranges::equal(run.keysOfOtherArm, run.keys.span().first(run.keysOfOtherArm.size())))
        run.keysOfOtherArm.clear();
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
        if (Vector<RunOfMaker::Store>& stores = m_runsOfMakersInFunction[place].laterStoresOfKeys; access.site && stores.size() < maxLaterStoresInConstructor)
            stores.append({ access.site, key });
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
    if (keys.size() <= maxFieldsInOrder && !keys.contains(key)) {
        keys.append(key);
        m_runsOnObjectsInFieldsInFunction[place].firstStoresOfKeys.append(access.site);
    }
}

uint64_t WriterOfLinkLayouts::findKeysStoredTogether(FunctionID id, std::span<const ValueID> firstStores) const
{
    const BodyIndex* index = firstStores.size() > 1 && firstStores[0] ? bodiesLookedAt(m_flow.looks).find(id) : nullptr;
    uint64_t together = 1;
    for (size_t place = 1; index && place < std::min<size_t>(firstStores.size(), 64); ++place) {
        if (firstStores[place] && index->blockOf(firstStores[place]) == index->blockOf(firstStores[0]))
            together |= 1ull << place;
    }
    return together;
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
        run.keysStoredTogether = findKeysStoredTogether(id, run.firstStoresOfKeys.span());
        m_runsOnObjectsInFields.append(WTF::move(run));
    }
    m_runsOnObjectsInFieldsInFunction.shrink(0);
}

void WriterOfLinkLayouts::sortKeysBySite(RunOfMaker& run)
{
    Vector<uint32_t, 8> places;
    for (uint32_t place = 0; place < run.keys.size(); ++place)
        places.append(place);
    AOT::stableSort(places, { }, [&](uint32_t place) {
        return run.firstStoresOfKeys[place].index();
    });
    auto permute = [&](const auto& list) {
        std::remove_cvref_t<decltype(list)> sorted;
        for (uint32_t place : places)
            sorted.append(list[place]);
        return sorted;
    };
    auto permuteBits = [&](uint64_t bits) {
        uint64_t sorted = 0;
        for (size_t to = 0; to < std::min<size_t>(places.size(), 64); ++to)
            sorted |= (places[to] < 64 ? bits >> places[to] & 1 : 0) << to;
        return sorted;
    };
    run.keys = permute(run.keys);
    run.firstStoresOfKeys = permute(run.firstStoresOfKeys);
    run.keysOnlyGivenUndefined = permuteBits(run.keysOnlyGivenUndefined);
    run.keysGivenObjectsMadeHere = permuteBits(run.keysGivenObjectsMadeHere);
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
        if (!run.keys.isEmpty())
            run.keysStoredTogether = findKeysStoredTogether(id, run.firstStoresOfKeys.span());
        if (!std::ranges::is_sorted(run.firstStoresOfKeys, { }, &ValueID::index))
            sortKeysBySite(run);
        if (const BodyIndex* index = run.keys.size() > 1 ? bodiesLookedAt(m_flow.looks).find(id) : nullptr) {
            ValueID lastStore = *std::ranges::max_element(run.firstStoresOfKeys, { }, &ValueID::index);
            for (size_t place = 0; lastStore && place < run.keys.size(); ++place) {
                if (run.firstStoresOfKeys[place] && index->dominates(index->blockOf(run.firstStoresOfKeys[place]), index->blockOf(lastStore)))
                    run.keysStoredOnEveryWayToLastStore.append(run.keys[place]);
            }
            if (!run.laterStoresOfKeys.isEmpty() && run.keys.size() <= longestListGoneThrough)
                findOtherArmKeys(run, *index);
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

void WriterOfLinkLayouts::noteComputedAccessorsOn(const LinkType& target)
{
    if (target.objects != LinkType::Objects::Layout && target.objects != LinkType::Objects::Set)
        return;
    m_flow.forEachLayoutOf(target, [&](LayoutID layout) {
        m_layoutsWithComputedAccessors.quickSet(layout.index());
    });
}

void WriterOfLinkLayouts::lookAtDefinersOfAccessors(FunctionID id, const ThinSummary& summary)
{
    if (!summary.has(SummaryTrait::CallsAccessorDefiner))
        return;
    const BuiltinTable& builtins = m_program.builtins();
    auto keyOfDefiner = [&](BuiltinID definer) { return m_program.keyOf(builtins.at(builtins.canonicalID(definer)).name); };
    PropertyKeyID definesGetter = keyOfDefiner(Builtins::ObjectPrototype__defineGetter__);
    PropertyKeyID definesSetter = keyOfDefiner(Builtins::ObjectPrototype__defineSetter__);
    for (const SummaryCall& call : summary.calls) {
        if (call.callee != SummaryCall::Callee::Name || !call.key || (call.key != definesGetter && call.key != definesSetter) || call.receiver == noSummaryNode)
            continue;
        const LinkType& target = typeAt(id, call.receiver);
        noteComputedAccessorsOn(target);
        if (target.objects == LinkType::Objects::Prototype)
            m_layoutsWithComputedAccessors.quickSet(target.layout.index());
    }
}

bool WriterOfLinkLayouts::standsBelowComputedAccessors(LayoutID layout) const
{
    static constexpr unsigned maxDepth = 4;
    for (unsigned depth = 0; depth < maxDepth; ++depth) {
        if (m_layoutsWithComputedAccessors.quickGet(layout.index()))
            return true;
        auto above = m_layoutsAboveLayouts.find(layout.index());
        if (above == m_layoutsAboveLayouts.end())
            return false;
        layout = above->value;
    }
    return false;
}

bool WriterOfLinkLayouts::isFilledByCreatorsAlone(LayoutID id) const
{
    const Layout& layout = m_program.at(id);
    if (layout.has(LayoutFlags::IsOfCreatedObject))
        return true;
    if (!layout.has(LayoutFlags::IsConstructorFunction) || !layout.constructor)
        return false;
    const ThinSummary& summary = m_flow.summaries.at(layout.constructor);
    return summary.accesses.isEmpty() && summary.calls.isEmpty() && summary.changes.isEmpty();
}

static bool storesAccessorIntoDescriptorInLinkLayouts(const Program& program, const ThinSummary& summary, const SummaryChange& change)
{
    if (change.other == noSummaryNode)
        return false;
    auto sourceOf = [&](uint32_t node) {
        if (summary.nodes[node].kind != SummaryNode::Kind::Join)
            return node;
        for (const SummaryEdge& edge : summary.edges) {
            if (edge.to == node)
                return edge.from;
        }
        return node;
    };
    PropertyKeyID get = program.keyOf(BuiltinNames::get);
    PropertyKeyID set = program.keyOf(BuiltinNames::set);
    return std::ranges::any_of(summary.accesses, [&](const SummaryAccess& access) {
        return access.kind == SummaryAccess::Kind::Store && access.key && (access.key == get || access.key == set) && access.value != noSummaryNode && summary.nodes[access.value].kind == SummaryNode::Kind::Closure && sourceOf(access.subject) == sourceOf(change.other);
    });
}

ALWAYS_INLINE void WriterOfLinkLayouts::lookAtChanges(FunctionID id, const ThinSummary& summary)
{
    for (const SummaryChange& change : summary.changes) {
        noteHalfOfAccessorLaidOnPrototype(id, summary, change);
        noteAccessorAssignedToClass(id, summary, change);
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
        if (change.kind == SummaryChange::Kind::DefinesByDescriptor && !change.key && (change.getter != noSummaryNode || change.setter != noSummaryNode || storesAccessorIntoDescriptorInLinkLayouts(m_program, summary, change)))
            noteComputedAccessorsOn(subject);
        if (change.kind == SummaryChange::Kind::ChangesPrototype && change.other != noSummaryNode && subject.objects == LinkType::Objects::Layout) {
            const LinkType& above = typeAt(id, change.other);
            if (above.objects == LinkType::Objects::Layout && above.layout != subject.layout)
                m_layoutsAboveLayouts.add(subject.layout.index(), above.layout);
        }
        bool waitsForOther = change.kind == SummaryChange::Kind::CopiesInto && change.other != noSummaryNode && typeAt(id, change.other).isNever();
        if (subject.isNever() || waitsForOther || (change.isSpelledByLoop && !m_flow.followsCopySpelledByLoop(subject, typeAt(id, change.other))))
            continue;
        if (change.kind != SummaryChange::Kind::BecomesPrototype && change.kind != SummaryChange::Kind::Freezes)
            noteStoreOnPrototypeObjects(subject, change.kind == SummaryChange::Kind::DefinesByDescriptor ? change.key : PropertyKeyID());
        if ((change.kind == SummaryChange::Kind::DefinesByDescriptor || change.kind == SummaryChange::Kind::CopiesInto) && subject.objects == LinkType::Objects::Layout && !subject.hasUnknownObjects)
            m_layoutsExtendedAlone.quickSet(subject.layout.index());
        noteChangeOfClosure(subject, change.kind == SummaryChange::Kind::DefinesByDescriptor ? change.key : PropertyKeyID());
        if (change.kind == SummaryChange::Kind::Deletes || change.kind == SummaryChange::Kind::DefinesByDescriptor || change.kind == SummaryChange::Kind::CopiesInto)
            noteStaticFieldChange(subject, change.kind == SummaryChange::Kind::CopiesInto ? PropertyKeyID() : change.key);
        if (change.kind != SummaryChange::Kind::DefinesByDescriptor || !change.laysValueInSight || !change.key)
            noteNameGivenToClosure(subject);
        else if (m_keysOfFunctionNames.get(change.key.index()))
            noteOtherWriteOfFunctionName(subject, change.key);
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
            if (!key || !change.descriptorSetsFlagsAlone)
                noteWriteOfPrototypeProperty(subject, key);
            noteNameDefinedPastLists(subject, key, true);
            m_accessorOfLiteralIsAtHand = change.lays != SummaryChange::Lays::WhatDescriptorSays;
            m_changeByDescriptorIsAtHand = !m_accessorOfLiteralIsAtHand;
            m_currentChangeDefinesAttributes = change.definesValueWithAttributes;
            m_changeAtHandMayLayAccessor = !change.descriptorIsInSight || !change.descriptorLaysNoAccessor;
            if (!change.isUnderNameThatNobodyElseNames)
                noteWrite(subject, key, change.lays == SummaryChange::Lays::WhatDescriptorSays && !(change.descriptorIsInSight && (change.getterInSight != noSummaryNode || change.setterInSight != noSummaryNode)) ? Traits { Trait::ComesAfterConstruction, Trait::MayGiveValue } : Traits { Trait::ComesAfterConstruction });
            m_accessorOfLiteralIsAtHand = false;
            m_changeByDescriptorIsAtHand = false;
            m_currentChangeDefinesAttributes = false;
            m_changeAtHandMayLayAccessor = false;
            if ((change.laysValueInSight || change.valueInSight != noSummaryNode) && change.other != noSummaryNode)
                noteValueLeftOutOfDescriptor(typeAt(id, change.other));
            break;
        }
        case SummaryChange::Kind::CopiesInto: {
            bool isNotedLostFamily = subject.objects == LinkType::Objects::Lost && subject.layout && m_lostFamiliesCopiedInto.set(lostFamilyOf(subject.layout));
            if (!isNotedLostFamily) {
                m_flow.forEachLayoutOf(subject, [&](LayoutID target) {
                    m_layoutsCopiedInto.quickSet(target.index());
                });
            }
            noteWriteOfPrototypeProperty(subject, { });
            noteNameDefinedPastLists(subject, { }, change.definesWhatItCopies);
            auto noteNameCopied = [&](PropertyKeyID key) {
                noteWrite(subject, key, { Trait::AddsWhatIsNotListed, Trait::ComesAfterConstruction, Trait::MayGiveValue });
                m_flow.forEachLayoutOf(subject, [&](LayoutID target) {
                    if (auto noted = m_pairsNotListed.find(static_cast<uint64_t>(target.index()) << 32 | key.bits()); noted != m_pairsNotListed.end())
                        m_namesNotListed[noted->value].isBroughtByCopy = true;
                });
            };
            if (!change.definesWhatItCopies && change.other != noSummaryNode && m_flow.assignedNamesOfRecords(subject, typeAt(id, change.other))) {
                forEachLayoutInType(m_flow, typeAt(id, change.other), [&](LayoutID source) {
                    for (const LinkFlow::OwnValue& value : m_flow.findOwnValues(source)) {
                        if (value.key)
                            noteNameCopied(value.key);
                    }
                });
                break;
            }
            noteWrite(subject, { }, { Trait::ComesAfterConstruction, Trait::MayGiveValue });
            if (change.isSpelledByLoop) {
                const LinkType& sources = typeAt(id, change.other);
                forEachLayoutInType(m_flow, sources, [&](LayoutID source) {
                    for (const Field& field : m_program.at(source).fields) {
                        if (field.key && !field.computedKey && !isPrivateNameInLink(m_program, field.key))
                            noteNameCopied(field.key);
                    }
                });
            }
            break;
        }
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

void WriterOfLinkLayouts::noteStaticFieldUnit(VariableID variable, uint32_t unit)
{
    uint32_t& listed = m_unitsOfStaticFields.add(variable.index(), unit).iterator->value;
    if (listed != unit)
        listed = otherThanOneUnit;
}

static bool makesInstancesOfPrimitives(BuiltinID builtin)
{
    const BuiltinID classes[] { Builtins::Map, Builtins::Set, Builtins::WeakMap, Builtins::WeakSet, Builtins::ArrayBuffer, Builtins::Int8Array, Builtins::Uint8Array, Builtins::Uint8ClampedArray, Builtins::Int16Array, Builtins::Uint16Array, Builtins::Int32Array, Builtins::Uint32Array, Builtins::Float32Array, Builtins::Float64Array, Builtins::BigInt64Array, Builtins::BigUint64Array };
    return std::ranges::contains(classes, builtin);
}

void WriterOfLinkLayouts::lookAtInitializer(FunctionID id)
{
    static constexpr OpcodeSet runOtherCode = opcodesWhere([](Opcode opcode) { return declaredBoundOf(opcode).mayRunOtherCode(); });
    m_initializer = id;
    m_fieldsOfInitializer.clear();
    const Function& function = m_program.at(id);
    std::optional<unsigned> parameter = function.thisParameterIndex();
    if (function.kind != FunctionKind::Initializer || function.body.numberOfBlocks() != 1 || !parameter)
        return;
    BodyBlock block = function.body.entry();
    Vector<ValueID, 4> holders { block.parameter(*parameter) };
    Vector<std::pair<ValueID, double>> numbers;
    static constexpr OpcodeSet writtenPrimitives { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double };
    Vector<ValueID> primitives;
    Vector<std::pair<ValueID, BuiltinID>> builtins;
    auto isPrimitive = [&](ValueID operand) {
        return !operand || std::ranges::binary_search(primitives, operand);
    };
    auto isMadeOfPrimitives = [&](const BodyValue& value) {
        if (allNumericOperators.contains(value.opcode())) {
            bool takesPrimitives = true;
            value.forEachOperand([&](ValueID operand, uint32_t) {
                takesPrimitives &= isPrimitive(operand);
            });
            if (takesPrimitives)
                primitives.append(value.id());
            return takesPrimitives;
        }
        if (value.opcode() != Opcode::Construct || m_program.admitsReplacedBuiltins)
            return false;
        auto construct = value.as<Opcode::Construct>();
        auto callee = std::ranges::lower_bound(builtins, construct.calleeOperand(), { }, &std::pair<ValueID, BuiltinID>::first);
        return callee != builtins.end() && callee->first == construct.calleeOperand() && makesInstancesOfPrimitives(m_program.builtins().canonicalID(callee->second)) && !construct.newTargetOperand() && std::ranges::all_of(construct.argumentsOperands(), isPrimitive);
    };
    bool mayHaveRunOtherCode = false;
    for (BodyValue value : block.values()) {
        if (writtenPrimitives.contains(value.opcode()) || (value.opcode() == Opcode::Convert && value.operand(0) && isPrimitive(value.operand(0))))
            primitives.append(value.id());
        else if (value.opcode() == Opcode::Builtin)
            builtins.append({ value.id(), value.builtin() });
        if (value.opcode() == Opcode::Convert && holders.contains(value.operand(0)))
            holders.append(value.id());
        else if (value.opcode() == Opcode::Double)
            numbers.append({ value.id(), value.as<Opcode::Double>().number() });
        else if (value.opcode() == Opcode::PropDefine && holders.contains(value.as<Opcode::PropDefine>().objectOperand())) {
            auto define = value.as<Opcode::PropDefine>();
            FieldOfInitializer field;
            field.isDefinedFirst = !mayHaveRunOtherCode;
            auto number = std::ranges::lower_bound(numbers, define.valueOperand(), { }, &std::pair<ValueID, double>::first);
            if (number != numbers.end() && number->first == define.valueOperand())
                field.number = number->second;
            auto added = m_fieldsOfInitializer.add(define.key().bits(), field);
            if (!added.isNewEntry)
                added.iterator->value = { };
        } else if (runOtherCode.contains(value.opcode()) && !isMadeOfPrimitives(value))
            mayHaveRunOtherCode = true;
    }
}

void WriterOfLinkLayouts::noteStaticFieldStore(FunctionID id, const ThinSummary& summary, const SummaryAccess& access, PropertyKeyID key, const LinkType& subject)
{
    if (subject.functions != LinkType::Functions::Closure || !key) {
        noteStaticFieldChange(subject, key);
        return;
    }
    LayoutID classLayout = m_program.at(subject.function).constructedClass;
    std::optional<uint32_t> place = classLayout ? m_program.at(classLayout).findStaticField(key) : std::nullopt;
    VariableID variable = place ? m_program.at(classLayout).staticFields[*place].variable : VariableID();
    if (!variable)
        return;
    notePrimitivesOfStaticField(variable, id, access);
    bool isWritten = !access.isByDescriptor && access.value != noSummaryNode && summary.nodes[access.value].kind == SummaryNode::Kind::Constant && WrittenUnit::fromBits(summary.nodes[access.value].second);
    const SummaryNode& holder = summary.nodes[access.subject];
    bool definesOwn = access.kind == SummaryAccess::Kind::Define && !access.isByDescriptor && holder.kind == SummaryNode::Kind::Parameter && m_program.at(id).thisParameterIndex() == holder.first;
    if (definesOwn && m_initializer != id)
        lookAtInitializer(id);
    auto defined = definesOwn ? m_fieldsOfInitializer.find(key.bits()) : m_fieldsOfInitializer.end();
    bool isDefinedHere = defined != m_fieldsOfInitializer.end();
    if (isDefinedHere)
        m_staticFieldsDefinedByInitializers.quickSet(variable.index());
    else if (access.kind == SummaryAccess::Kind::Define)
        m_staticFieldsRemovedOrRedefined.quickSet(variable.index());
    if (isDefinedHere && defined->value.isDefinedFirst)
        m_staticFieldsFilledFirst.quickSet(variable.index());
    uint32_t unit = isWritten ? summary.nodes[access.value].second : otherThanOneUnit;
    if (!isWritten && isDefinedHere && defined->value.number) {
        unit = firstUnitOfNumber + m_numbersOfStaticFields.size();
        m_numbersOfStaticFields.append(*defined->value.number);
    }
    noteStaticFieldUnit(variable, unit);
}

void WriterOfLinkLayouts::notePrimitivesOfStaticField(VariableID variable, FunctionID id, const SummaryAccess& access)
{
    static constexpr uint16_t comparedByBits = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Symbol;
    uint16_t stored = Primitive::All;
    if (!access.isByDescriptor && access.value != noSummaryNode) {
        const LinkType& type = typeAt(id, access.value);
        uint16_t primitives = LinkType::inNamesOfPrimitive(type.primitives);
        if (primitives & Primitive::Boolean)
            primitives |= Primitive::Boolean;
        if (primitives & Primitive::AnySymbol)
            primitives = static_cast<uint16_t>((primitives & ~Primitive::AnySymbol) | Primitive::Symbol);
        if (type.objects == LinkType::Objects::None && type.functions == LinkType::Functions::None && type.classes == LinkType::Classes::None && !type.hasUnknownObjects && !type.holdsLostBuiltins && primitives && !(primitives & ~comparedByBits))
            stored = primitives;
    }
    m_primitivesOfStaticFields.add(variable.index(), 0).iterator->value |= stored;
}

void WriterOfLinkLayouts::writePrimitivesOfStaticFields()
{
    static constexpr OptionSet<VariableFlags> storedByOthers { VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun, VariableFlags::HoldsInstance };
    BitVector storedByEvaluatedText;
    for (const EvalScope& scope : m_program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.variable)
                storedByEvaluatedText.set(binding.variable.index());
        }
    }
    for (auto& [index, primitives] : m_primitivesOfStaticFields) {
        VariableID id(index);
        const Variable& variable = m_program.at(id);
        if (primitives == Primitive::All || primitives & Primitive::Number || storedByEvaluatedText.get(index) || variable.flags.containsAny(storedByOthers) || !m_program.at(variable.module).body || !variablesAreWrittenOnlyByBodiesInLinkLayouts(m_program.at(variable.module).kind))
            continue;
        m_tables.primitivesOfStaticFields.append({ id, primitives });
    }
    std::ranges::sort(m_tables.primitivesOfStaticFields, { }, &std::pair<VariableID, uint16_t>::first);
}

void WriterOfLinkLayouts::noteStaticFieldChange(const LinkType& subject, PropertyKeyID key)
{
    switch (subject.functions) {
    case LinkType::Functions::None:
        return;
    case LinkType::Functions::Closure:
        noteStaticFieldChange(subject.function, key);
        return;
    case LinkType::Functions::Set:
        for (FunctionID member : m_flow.functionsOfSet(subject))
            noteStaticFieldChange(member, key);
        return;
    case LinkType::Functions::Constructors:
        if (key)
            m_staticKeysChangedOnFamilies.set(key.index());
        else
            m_anyStaticFieldMayChange = true;
        return;
    case LinkType::Functions::Lost:
        if (key)
            m_lostClassChangedStaticKeys.set(key.index());
        else
            m_anyLostClassStaticFieldMayChange = true;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void WriterOfLinkLayouts::noteStaticFieldRead(FunctionID id, const SummaryAccess& access)
{
    PropertyKeyID key = m_flow.keyOf(id, access);
    auto noteAllOf = [&](FunctionID constructor) {
        LayoutID classLayout = m_program.at(constructor).constructedClass;
        for (const StaticField& field : classLayout ? m_program.at(classLayout).staticFields.span() : std::span<const StaticField>()) {
            if (field.variable)
                m_staticFieldsReadAsProperties.quickSet(field.variable.index());
        }
    };
    const LinkType& subject = typeAt(id, access.subject);
    switch (subject.functions) {
    case LinkType::Functions::None:
        return;
    case LinkType::Functions::Closure:
        if (!key)
            noteAllOf(subject.function);
        else if (LayoutID classLayout = m_program.at(subject.function).constructedClass; !classLayout || !m_program.at(classLayout).findStaticField(key))
            m_staticKeysReadAsProperties.set(key.index());
        return;
    case LinkType::Functions::Set:
        if (key)
            m_staticKeysReadAsProperties.set(key.index());
        else
            std::ranges::for_each(m_flow.functionsOfSet(subject), noteAllOf);
        return;
    case LinkType::Functions::Constructors:
        if (key)
            m_staticKeysReadAsProperties.set(key.index());
        else
            m_allStaticFieldsAreProperties = true;
        return;
    case LinkType::Functions::Lost:
        if (key)
            m_staticKeysReadAsProperties.set(key.index());
        else
            m_lostClassStaticFieldsAreProperties = true;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void WriterOfLinkLayouts::findStaticPropertyReads(const BitVector& keys)
{
    struct ReadOfClass {
        FunctionID reader;
        uint32_t place { 0 };
    };
    Vector<ReadOfClass> reads = collectInParallel<ReadOfClass>(m_flow.loopsOfReading(), static_cast<unsigned>(m_program.functions.size()), [&](unsigned index, Vector<ReadOfClass>& reads) {
        if (!m_flow.reached.get(index))
            return;
        FunctionID id(index);
        const auto& accesses = m_flow.summaries.at(id).accesses;
        for (uint32_t place = 0; place < accesses.size(); ++place) {
            const SummaryAccess& access = accesses[place];
            if (access.kind == SummaryAccess::Kind::Read && !access.isToContents() && (!access.key || (!liesInThread(access.key) && keys.get(access.key.index()))) && typeAt(id, access.subject).functions != LinkType::Functions::None)
                reads.append({ id, place });
        }
    });
    for (const ReadOfClass& read : reads)
        noteStaticFieldRead(read.reader, m_flow.summaries.at(read.reader).accesses[read.place]);
}

void WriterOfLinkLayouts::noteStaticFieldChange(FunctionID constructor, PropertyKeyID key)
{
    LayoutID classLayout = m_program.at(constructor).constructedClass;
    if (!classLayout)
        return;
    for (const StaticField& field : m_program.at(classLayout).staticFields) {
        if (!field.variable || (key && field.key != key))
            continue;
        noteStaticFieldUnit(field.variable, otherThanOneUnit);
        m_staticFieldsRemovedOrRedefined.quickSet(field.variable.index());
    }
}

void WriterOfLinkLayouts::findVariablesOfFilledClasses(FunctionID moduleBody)
{
    struct ClassMade {
        LayoutID layout;
        bool isFilled { false };
    };
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> placesOfClasses;
    Vector<ClassMade> classesMade;
    auto madeAt = [&](ValueID value) -> ClassMade* {
        auto place = value ? placesOfClasses.find(value.index()) : placesOfClasses.end();
        return place == placesOfClasses.end() ? nullptr : &classesMade[place->value];
    };
    m_program.at(moduleBody).body.forEachBlock([&](const BodyBlock& block) {
        placesOfClasses.clear();
        classesMade.shrink(0);
        for (BodyValue value : block.values()) {
            switch (value.opcode()) {
            case Opcode::FinishClass:
                placesOfClasses.set(value.id().index(), classesMade.size());
                classesMade.append({ value.as<Opcode::FinishClass>().layout() });
                break;
            case Opcode::Convert:
                if (madeAt(value.operand(0)))
                    placesOfClasses.set(value.id().index(), placesOfClasses.get(value.operand(0).index()));
                break;
            case Opcode::CallDirect:
                if (ClassMade* made = value.numberOfVariadicOperands() ? madeAt(value.operand(0)) : nullptr; made && m_program.at(value.function()).kind == FunctionKind::Initializer)
                    made->isFilled = true;
                break;
            case Opcode::PutModuleVariable:
                if (ClassMade* made = madeAt(value.operand(0)); made && made->isFilled && m_program.at(value.variable()).flags.contains(VariableFlags::IsImmutable))
                    m_tables.variablesOfFilledClasses.set(value.variable().index());
                break;
            default:
                break;
            }
        }
    });
}

void WriterOfLinkLayouts::writeUnitsOfStaticFields()
{
    if (m_flow.reading() != WhatUnseenCodeDoes::Anything || m_anyStaticFieldMayChange)
        return;
    static constexpr OptionSet<VariableFlags> storedByOthers { VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun, VariableFlags::HoldsInstance };
    Vector<FunctionID> bodiesOfClasses;
    auto keepsInitializerValue = [&](const StaticField& field) {
        return m_staticFieldsDefinedByInitializers.quickGet(field.variable.index()) && !m_staticFieldsRemovedOrRedefined.quickGet(field.variable.index());
    };
    BitVector permanentKeys;
    for (const Layout& layout : m_program.layouts) {
        for (const StaticField& field : layout.staticFields) {
            if (field.key && field.variable && !liesInThread(field.key) && keepsInitializerValue(field))
                permanentKeys.set(field.key.index());
        }
    }
    if (!permanentKeys.isEmpty())
        findStaticPropertyReads(permanentKeys);
    for (const Layout& layout : m_program.layouts) {
        bool isLost = !layout.constructor || m_flow.lostFunctions.get(layout.constructor.index());
        if (isLost && m_anyLostClassStaticFieldMayChange)
            continue;
        for (const StaticField& field : layout.staticFields) {
            if (!field.key || !field.variable || liesInThread(field.key) || m_staticKeysChangedOnFamilies.get(field.key.index()) || (isLost && m_lostClassChangedStaticKeys.get(field.key.index())) || m_flow.escapedVariables.get(field.variable.index()))
                continue;
            const Variable& variable = m_program.at(field.variable);
            if (variable.flags.containsAny(storedByOthers) || !m_program.at(variable.module).body || !variablesAreWrittenOnlyByBodiesInLinkLayouts(m_program.at(variable.module).kind))
                continue;
            bool isFilledFirst = m_staticFieldsFilledFirst.quickGet(field.variable.index());
            bool isReadAsProperty = m_allStaticFieldsAreProperties || (isLost && m_lostClassStaticFieldsAreProperties) || m_staticKeysReadAsProperties.get(field.key.index()) || m_staticFieldsReadAsProperties.quickGet(field.variable.index());
            bool staysAsDefined = !isReadAsProperty && keepsInitializerValue(field);
            TypeID arrived = isFilledFirst || staysAsDefined ? claimedTypeAt(m_flow.variable(field.variable)) : TypeID();
            if (TypeID type = arrived && staysAsDefined ? unlessAnything(arrived) : TypeID(); type && !(types().at(type).primitives & (Primitive::Number | Primitive::Absent))) {
                m_tables.staticFields.append({ field.variable, type, isFilledFirst });
                m_tables.knownStaticFieldKeys.set(field.key.index());
                if (!isFilledFirst)
                    bodiesOfClasses.append(m_program.at(variable.module).body);
            }
            auto unit = m_unitsOfStaticFields.find(field.variable.index());
            if (!isFilledFirst || unit == m_unitsOfStaticFields.end() || unit->value == otherThanOneUnit)
                continue;
            m_tables.knownStaticFieldKeys.set(field.key.index());
            if (unit->value >= firstUnitOfNumber && !WrittenUnit::fromBits(unit->value)) {
                if (arrived && types().at(arrived).primitives & Primitive::FractionalNumber)
                    m_tables.numbersOfStaticFields.append({ field.variable, m_numbersOfStaticFields[unit->value - firstUnitOfNumber] });
                continue;
            }
            TypeID written = typeOfWrittenUnit(WrittenUnit::fromBits(unit->value));
            if (arrived && types().isSubset(written, arrived))
                m_tables.unitsOfStaticFields.append({ field.variable, written });
        }
    }
    std::ranges::sort(bodiesOfClasses);
    bodiesOfClasses.shrink(std::ranges::unique(bodiesOfClasses).begin() - bodiesOfClasses.begin());
    for (FunctionID body : bodiesOfClasses)
        findVariablesOfFilledClasses(body);
    std::ranges::sort(m_tables.staticFields, { }, &LinkedStaticField::variable);
    std::ranges::sort(m_tables.unitsOfStaticFields, { }, &std::pair<VariableID, TypeID>::first);
    std::ranges::sort(m_tables.numbersOfStaticFields, { }, &std::pair<VariableID, double>::first);
}

void WriterOfLinkLayouts::noteNameDefinedPastLists(const LinkType& holder, PropertyKeyID key, bool isDefinition)
{
    if (key && m_program.at(key).kind == PropertyKeyKind::PrivateName)
        return;
    auto note = [&](LayoutID classLayout, ClassSide side, bool isOneOfFamily = false) {
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
        if (side != ClassSide::Prototype)
            return;
        if (!key)
            m_prototypesDefinedOnUnderKeysThatNobodyKnows.quickSet(classLayout.index());
        if (!key || isOneOfFamily)
            m_prototypesGainingUnknownNames.quickSet(classLayout.index());
        else
            m_namesDefinedOnPrototypes.append(static_cast<uint64_t>(classLayout.index()) << 32 | key.bits());
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
            isWhollyPastLists &= side == ClassSide::Prototype ? m_prototypesGainingUnknownNames.quickGet(layout.index()) : m_sidesWithNamesDefinedPastLists.quickGet(2 * static_cast<size_t>(layout.index()) + static_cast<size_t>(side));
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
            note(layout, ClassSide::Prototype, true);
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

void WriterOfLinkLayouts::forEachFollowedName(const LinkType& subject, PropertyKeyID key, NOESCAPE const Invocable<void(FunctionID)> auto& functor)
{
    auto visit = [&](FunctionID function) {
        if (m_flow.followsNamesOnClosuresOf(function, key))
            functor(function);
    };
    if (subject.functions == LinkType::Functions::Closure)
        visit(subject.function);
    else if (subject.functions == LinkType::Functions::Set)
        std::ranges::for_each(m_flow.functionsOfSet(subject), visit);
}

void WriterOfLinkLayouts::findFunctionNamesRead()
{
    if (m_keysOfFunctionNames.isEmpty())
        return;
    Vector<uint64_t> namesRead = collectInParallel<uint64_t>(m_flow.loopsOfReading(), static_cast<unsigned>(m_program.functions.size()), [&](unsigned index, Vector<uint64_t>& names) {
        if (!m_flow.reached.get(index))
            return;
        FunctionID id(index);
        const ThinSummary& summary = m_flow.summaries.at(id);
        auto noteRead = [&](uint32_t holder, PropertyKeyID key) {
            if (holder == noSummaryNode)
                return;
            forEachFollowedName(typeAt(id, holder), key, [&](FunctionID function) {
                names.append(static_cast<uint64_t>(function.index()) << 32 | key.bits());
            });
        };
        for (const SummaryCall& call : summary.calls) {
            if (call.callee != SummaryCall::Callee::Name || !call.key || !m_keysOfFunctionNames.get(call.key.index()))
                continue;
            noteRead(call.subject, call.key);
            noteRead(call.receiver, call.key);
        }
        for (const SummaryAccess& access : summary.accesses) {
            if (access.kind == SummaryAccess::Kind::Read && !access.isToContents() && access.key && m_keysOfFunctionNames.get(access.key.index()))
                noteRead(access.subject, access.key);
        }
    });
    for (uint64_t name : namesRead)
        m_functionNamesRead.add(name);
}

void WriterOfLinkLayouts::noteStoreOfFunctionName(FunctionID id, const SummaryAccess& access, const LinkType& subject, PropertyKeyID key)
{
    if (access.kind != SummaryAccess::Kind::Store || access.receiver != noSummaryNode || !access.site || access.isByDescriptor || access.isSpelledByCopy || access.isCopySpelledByLoop) {
        noteOtherWriteOfFunctionName(access.receiver != noSummaryNode ? typeAt(id, access.receiver) : subject, key);
        return;
    }
    forEachFollowedName(subject, key, [&](FunctionID function) {
        m_storesOfFunctionNames.append(LinkedFunctionNameStore { id, access.site, function, key });
    });
}

void WriterOfLinkLayouts::noteOtherWriteOfFunctionName(const LinkType& subject, PropertyKeyID key)
{
    forEachFollowedName(subject, key, [&](FunctionID function) {
        m_functionNamesWrittenOtherwise.add(static_cast<uint64_t>(function.index()) << 32 | key.bits());
    });
}

bool WriterOfLinkLayouts::onlyStatementsLayName(FunctionID function, PropertyKeyID key) const
{
    if (m_flow.reading() != WhatUnseenCodeDoes::Anything || m_flow.escapedFunctions.get(function.index()) || m_flow.lostFunctions.get(function.index()))
        return false;
    LayoutID classLayout = m_program.at(function).constructedClass;
    if (m_someConstructorGetsName || m_functionsWhoseClosuresGetWhatIsNotListed.get(function.index()) || (classLayout && !m_flow.isOfConstructorFunction(classLayout)) || !m_flow.closuresInheritFunctionPrototype(function))
        return false;
    return std::ranges::all_of(std::array { Builtins::FunctionPrototype, Builtins::ObjectPrototype }, [&](BuiltinID above) {
        BuiltinID holder = m_program.builtins().canonicalID(above);
        return !findOwnMember(m_program, holder, key) && listsEveryMemberUnder(m_program, holder, key) && isClosedKey(m_program, holder, key);
    });
}

void WriterOfLinkLayouts::writeNamesOnFunctions()
{
    m_tables.rootsOfClassRows = Vector<LayoutID>(m_flow.rootsOfClassRows());
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
        linked.seen = claimedTypeAt(name.laid);
        if (linked.seen && !m_types->isUninformative(linked.seen) && onlyStatementsLayName(name.function, name.key))
            linked.proven = m_types->unionOf(TypeTable::undefined, linked.seen);
        uint64_t word = static_cast<uint64_t>(name.function.index()) << 32 | name.key.bits();
        linked.isLaidByListedStores = linked.proven && m_functionNamesRead.contains(word) && !m_functionNamesWrittenOtherwise.contains(word);
        m_tables.namesOnFunctions.append(linked);
        m_tables.keysNamedOnFunctions.set(name.key.index());
    }
    std::ranges::sort(m_tables.namesOnFunctions, { }, [](const LinkedNameOnFunction& name) {
        return std::pair { name.holder, name.key };
    });
    m_storesOfFunctionNames.removeAllMatching([&](const LinkedFunctionNameStore& store) {
        const LinkedNameOnFunction* name = m_tables.findNameOnFunction(store.holder, store.key);
        return !name || !name->isLaidByListedStores;
    });
    std::ranges::sort(m_storesOfFunctionNames, { }, [](const LinkedFunctionNameStore& store) {
        return std::tuple { store.function, store.site, store.holder };
    });
    m_tables.storesOfFunctionNames = WTF::move(m_storesOfFunctionNames);
}

void WriterOfLinkLayouts::writeRowsBelowNamesDefinedPastLists()
{
    std::ranges::sort(m_namesDefinedOnPrototypes);
    auto repeated = std::ranges::unique(m_namesDefinedOnPrototypes);
    m_namesDefinedOnPrototypes.shrink(static_cast<size_t>(repeated.begin() - m_namesDefinedOnPrototypes.begin()));
    m_tables.namesDefinedOnPrototypes = WTF::map(m_namesDefinedOnPrototypes, [](uint64_t word) {
        return std::pair { LayoutID(static_cast<uint32_t>(word >> 32)), PropertyKeyID::fromBits(static_cast<uint32_t>(word)) };
    });
    BitVector isAnswered(m_program.layouts.size());
    Vector<LayoutID, 16> row;
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        LayoutID current(index);
        for (; current && !isAnswered.quickSet(current.index()); current = m_program.at(current).base)
            row.append(current);
        std::array<bool, 2> areDefined = current ? m_tables.layouts[current.index()].namesAreDefinedPastLists : std::array<bool, 2> { };
        bool getUnknownNames = current && m_tables.layouts[current.index()].prototypesGetUnknownNames;
        for (; !row.isEmpty(); row.removeLast()) {
            size_t layout = row.last().index();
            getUnknownNames |= m_prototypesGainingUnknownNames.quickGet(layout);
            m_tables.layouts[layout].prototypesGetUnknownNames = getUnknownNames;
            areDefined[static_cast<size_t>(ClassSide::Prototype)] |= m_sidesWithNamesDefinedPastLists.quickGet(2 * layout + static_cast<size_t>(ClassSide::Prototype));
            areDefined[static_cast<size_t>(ClassSide::Static)] |= m_sidesWithNamesDefinedPastLists.quickGet(2 * layout + static_cast<size_t>(ClassSide::Static)) || (m_namesAreDefinedOnLostFunctions && m_liesBelowLostClassObject.quickGet(layout));
            m_tables.layouts[layout].namesAreDefinedPastLists = areDefined;
        }
    }
}

void WriterOfLinkLayouts::noteHelperSteps()
{
    using Kind = LinkFlow::PrototypeHelper::Step::Kind;
    for (const LinkFlow::PrototypeHelperStep& step : m_flow.prototypeHelperSteps()) {
        const LinkType closures = typeOfClosuresInLinkLayouts(step.object);
        switch (step.kind) {
        case Kind::LaysPrototype:
            m_flow.build.statistics().count(StatisticID::PrototypesLaidByHelpers);
            noteChangeOfClosure(closures, m_program.keyOf(BuiltinNames::prototype));
            noteWriteOfPrototypeProperty(closures, m_program.keyOf(BuiltinNames::prototype), true);
            break;
        case Kind::LinksParent:
            if (!step.objectIsPrototype) {
                noteChangeOfClosure(closures, { });
                noteNameGivenToClosure(closures);
            }
            break;
        case Kind::StoresName: {
            if (!step.objectIsPrototype) {
                noteChangeOfClosure(closures, step.key);
                noteNameGivenToClosure(closures, step.key, true);
                noteOtherWriteOfFunctionName(closures, step.key);
                break;
            }
            LayoutID layout = m_flow.layoutOfConstructorFunction(step.object);
            m_keysWrittenOnPrototypes.add(step.key.bits());
            auto added = m_namesWrittenOnPrototypes.add(static_cast<uint64_t>(layout.index()) << 32 | step.key.bits(), NameWrittenOnPrototype { { }, { }, step.valueIsPrototype ? FunctionID() : step.value, true });
            if (!added.isNewEntry) {
                added.iterator->value.isWrittenOnce = false;
                added.iterator->value.isWrittenAgain = true;
            }
            break;
        }
        case Kind::CopiesNames:
            noteChangeOfClosure(closures, { });
            noteNameGivenToClosure(closures);
            break;
        case Kind::AsksOwnName:
            break;
        case Kind::CallsFunction:
            RELEASE_ASSERT_NOT_REACHED();
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
            function.prototypePropertyStaysAsLiteral = isWrittenInNoOtherWay && m_functionsGivenObjectMadeToBePrototype.get(index);
            function.fillsNewObjects = m_flow.fillsNewObjects(FunctionID(static_cast<uint32_t>(index)));
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
    bool hadHeritage = m_hasHeritage.set(classLayout.index());
    bool holdsSeveralClasses = objects.functions == LinkType::Functions::Constructors || objects.functions == LinkType::Functions::Set;
    if (holdsSeveralClasses || (hadHeritage && above && m_heritages[classLayout.index()] && m_heritages[classLayout.index()] != above))
        m_hasSeveralHeritages.quickSet(classLayout.index());
    if (hadHeritage && m_heritages[classLayout.index()] != above)
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
    if (change.kind != SummaryChange::Kind::DefinesByDescriptor || change.lays == SummaryChange::Lays::WhatDescriptorSays || change.laysAccessorAsWritten || !change.key)
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

LayoutID WriterOfLinkLayouts::findClassOfPrototypeAt(FunctionID id, const ThinSummary& summary, uint32_t node) const
{
    if (node == noSummaryNode || summary.nodes[node].kind != SummaryNode::Kind::ResultOfAccess)
        return { };
    const SummaryAccess& read = summary.accesses[summary.nodes[node].first];
    if (read.kind != SummaryAccess::Kind::Read || read.key != m_program.keyOf(BuiltinNames::prototype) || read.subject == noSummaryNode)
        return { };
    return m_flow.findClassNamedAt(id, summary.nodes[read.subject]);
}

FunctionID WriterOfLinkLayouts::findOnlyFunctionAt(FunctionID id, uint32_t node) const
{
    if (node == noSummaryNode)
        return { };
    LinkType type = typeAt(id, node);
    type.primitives = 0;
    LinkType oneClosure;
    oneClosure.functions = LinkType::Functions::Closure;
    oneClosure.function = type.function;
    return type == oneClosure ? type.function : FunctionID();
}

static BlockID findLastBlockOnEveryPath(const BodyIndex& index)
{
    BlockID last;
    for (BlockID block : index.blocksInDominatorOrder()) {
        if (TerminatorKind kind = index.terminator(block).kind(); kind != TerminatorKind::Return && kind != TerminatorKind::ReturnCall)
            continue;
        if (!last)
            last = block;
        while (!index.dominates(last, block))
            last = index.immediateDominator(last);
    }
    return last;
}

bool WriterOfLinkLayouts::isOnEveryPath(FunctionID id, ValueID site) const
{
    const BodyIndex* index = site ? bodiesLookedAt(m_flow.looks).find(id) : nullptr;
    if (!index)
        return false;
    BlockID last = m_lastBlocksOnEveryPath.ensure(id.index(), [&] { return findLastBlockOnEveryPath(*index); }).iterator->value;
    return last && index->dominates(index->blockOf(site), last);
}

std::pair<FunctionID, ValueID> WriterOfLinkLayouts::findCallAtOnce(FunctionID callee, ValueID site) const
{
    if (m_program.at(callee).isModuleBody() || !isOnEveryPath(callee, site))
        return { };
    FunctionID maker = m_flow.findOnlyReferrer(callee);
    const BodyIndex* index = maker && maker != callee ? bodiesLookedAt(m_flow.looks).find(maker) : nullptr;
    if (!index)
        return { };
    for (uint32_t number = 0; number < index->numberOfBlocks(); ++number) {
        for (BodyValue value : index->block(BlockID(number)).values()) {
            if (value.opcode() != Opcode::Call)
                continue;
            ValueID called = value.as<Opcode::Call>().calleeOperand();
            ValueID closure = called ? index->stripCasts(called) : ValueID();
            if (closure && !index->isParameter(closure) && index->opcodeOf(closure) == Opcode::NewClosure && index->value(closure).as<Opcode::NewClosure>().function() == callee && index->hasOneUse(closure))
                return { maker, value.id() };
        }
    }
    return { };
}

void WriterOfLinkLayouts::noteAssignmentOutsideClass(FunctionID writer, ValueID site, LayoutID classLayout, PropertyKeyID key, FunctionID method)
{
    if (m_flow.reading() != WhatUnseenCodeDoes::OnlyReadsAndCalls)
        return;
    if (!isOnEveryPath(writer, site))
        return;
    if (isBesideClass(writer, classLayout))
        noteLastAssignment(writer, site, classLayout, key, method);
    else if (method && !liesInThread(key))
        m_assignmentsByHelpers.append({ writer, classLayout, key, method });
}

bool WriterOfLinkLayouts::isBesideClass(FunctionID writer, LayoutID classLayout) const
{
    return m_program.at(writer).isModuleBody() || std::ranges::contains(m_flow.summaries.at(writer).classesEvaluated, classLayout);
}

FunctionID WriterOfLinkLayouts::findFunctionAssignedAt(FunctionID writer, const ThinSummary& summary, uint32_t node, LayoutID classLayout, PropertyKeyID key) const
{
    if (FunctionID only = findOnlyFunctionAt(writer, node))
        return only;
    if (node == noSummaryNode || typeAt(writer, node).functions != LinkType::Functions::Set)
        return { };
    for (const SummaryMember& member : summary.members) {
        if (member.classLayout != classLayout || member.side != ClassSide::Prototype || member.key != key || member.isRedefined || summary.nodes[member.value].kind != SummaryNode::Kind::Closure)
            continue;
        FunctionID written(summary.nodes[member.value].first);
        return std::ranges::contains(m_flow.functionsOfSet(typeAt(writer, node)), written) ? written : FunctionID();
    }
    return { };
}

LayoutID WriterOfLinkLayouts::findClassAssignedAt(FunctionID writer, const ThinSummary& summary, uint32_t node) const
{
    if (node == noSummaryNode)
        return { };
    if (LayoutID named = findClassOfPrototypeAt(writer, summary, node))
        return named;
    const LinkType& held = typeAt(writer, node);
    return held.objects == LinkType::Objects::Prototype ? held.layout : LayoutID();
}

bool WriterOfLinkLayouts::assignsThroughParameter(FunctionID helper)
{
    if (m_helpersAsked.set(helper.index()))
        return m_helpersThatAssign.get(helper.index());
    const ThinSummary& summary = m_flow.summaries.at(helper);
    auto isParameter = [&](uint32_t node) {
        return node != noSummaryNode && summary.nodes[node].kind == SummaryNode::Kind::Parameter;
    };
    bool assigns = std::ranges::any_of(summary.accesses, [&](const SummaryAccess& access) {
        if (access.kind != SummaryAccess::Kind::Store || access.subject == noSummaryNode)
            return false;
        const SummaryNode& subject = summary.nodes[access.subject];
        return subject.kind == SummaryNode::Kind::ResultOfAccess ? isParameter(summary.accesses[subject.first].subject) : isParameter(access.subject);
    });
    if (assigns)
        m_helpersThatAssign.set(helper.index());
    return assigns;
}

void WriterOfLinkLayouts::noteAssignmentsOfCall(FunctionID caller, const SummaryCall& call, FunctionID helper, uint32_t argument, LayoutID classLayout, bool passesPrototype)
{
    const ThinSummary& summary = m_flow.summaries.at(helper);
    uint32_t first = m_program.at(helper).firstParameterIndex();
    auto holdsArgument = [&](uint32_t node) {
        return node != noSummaryNode && summary.nodes[node].kind == SummaryNode::Kind::Parameter && summary.nodes[node].first == first + argument;
    };
    auto isPrototypeOfArgument = [&](uint32_t node) {
        if (passesPrototype || node == noSummaryNode || summary.nodes[node].kind != SummaryNode::Kind::ResultOfAccess)
            return passesPrototype && holdsArgument(node);
        const SummaryAccess& read = summary.accesses[summary.nodes[node].first];
        return read.kind == SummaryAccess::Kind::Read && read.key == m_program.keyOf(BuiltinNames::prototype) && holdsArgument(read.subject);
    };
    for (const SummaryAccess& access : summary.accesses) {
        if (access.kind != SummaryAccess::Kind::Store || access.isByDescriptor || !isPrototypeOfArgument(access.subject))
            continue;
        FunctionID method = findOnlyFunctionAt(helper, access.value);
        if (!method || !isOnEveryPath(helper, access.site) || !isOnEveryPath(caller, call.site))
            continue;
        PropertyKeyID key = access.key;
        if (!key) {
            const BodyIndex& index = *bodiesLookedAt(m_flow.looks).find(helper);
            ValueID name = !index.isParameter(access.site) && index.opcodeOf(access.site) == Opcode::ElemSet ? index.stripCasts(index.value(access.site).as<Opcode::ElemSet>().propertyOperand()) : ValueID();
            if (!name || !index.isParameter(name) || index.blockOf(name) != entryBlock || name.index() < first)
                continue;
            const BodyIndex& body = *bodiesLookedAt(m_flow.looks).find(caller);
            if (body.isParameter(call.site) || body.opcodeOf(call.site) != Opcode::Call)
                continue;
            std::span<const ValueID> arguments = body.value(call.site).as<Opcode::Call>().argumentsOperands();
            uint32_t place = name.index() - first;
            ValueID passed = place < arguments.size() && arguments[place] ? body.stripCasts(arguments[place]) : ValueID();
            if (!passed || body.isParameter(passed) || body.opcodeOf(passed) != Opcode::String)
                continue;
            key = m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = body.value(passed).as<Opcode::String>().string() }).value_or(PropertyKeyID());
        }
        if (key)
            noteLastAssignment(caller, call.site, classLayout, key, method);
    }
}

void WriterOfLinkLayouts::noteAssignmentsByHelpers()
{
    if (m_flow.reading() != WhatUnseenCodeDoes::OnlyReadsAndCalls)
        return;
    std::ranges::stable_sort(m_assignmentsByHelpers, { }, &AssignmentByHelper::helper);
    const auto& calleesOfCalls = m_flow.calleesOfCallsAsSettled();
    Vector<uint32_t> callers = findIndicesInParallel(m_flow.loopsOfReading(), static_cast<unsigned>(m_program.functions.size()), [&](unsigned index) {
        const Function& caller = m_program.at(FunctionID(index));
        return m_flow.reached.get(index) && (caller.isModuleBody() || !m_flow.summaries.at(FunctionID(index)).classesEvaluated.isEmpty()) && !isLibraryCode(m_program, caller);
    });
    for (uint32_t index : callers) {
        FunctionID caller(index);
        const ThinSummary& summary = m_flow.summaries.at(caller);
        for (uint32_t place = 0; place < summary.calls.size(); ++place) {
            const SummaryCall& call = summary.calls[place];
            std::span<const FunctionID> callees = calleesOfCalls[m_flow.firstCallOfFunction[index] + place].listed();
            if (callees.size() != 1 || call.isMadeByProtocol || call.passesList || isLibraryCode(m_program, m_program.at(callees[0])))
                continue;
            for (const AssignmentByHelper& assignment : std::ranges::equal_range(m_assignmentsByHelpers, callees[0], { }, &AssignmentByHelper::helper)) {
                if (isBesideClass(caller, assignment.classLayout) && isOnEveryPath(caller, call.site))
                    noteLastAssignment(caller, call.site, assignment.classLayout, assignment.key, assignment.method);
            }
            for (uint32_t argument = 0; argument < call.numberOfArguments && assignsThroughParameter(callees[0]); ++argument) {
                uint32_t passed = summary.arguments[call.firstArgument + argument];
                if (passed == noSummaryNode)
                    continue;
                LayoutID classPassed = m_flow.findClassNamedAt(caller, summary.nodes[passed]);
                LayoutID classOfPrototype = classPassed ? LayoutID() : findClassOfPrototypeAt(caller, summary, passed);
                if (LayoutID classLayout = classPassed ? classPassed : classOfPrototype; classLayout && isBesideClass(caller, classLayout))
                    noteAssignmentsOfCall(caller, call, callees[0], argument, classLayout, !classPassed);
            }
        }
    }
}

void WriterOfLinkLayouts::noteComputedMemberAssignment(FunctionID writer, const ThinSummary& summary, const SummaryAccess& access)
{
    LayoutID classLayout = findClassAssignedAt(writer, summary, access.subject);
    if (!classLayout)
        return;
    FunctionID method = findOnlyFunctionAt(writer, access.value);
    auto noteName = [&](PropertyKeyID key) {
        m_namesAssignedToClasses.add(static_cast<uint64_t>(classLayout.index()) << 32 | key.bits());
    };
    if (PropertyKeyID key = m_flow.keyOf(writer, access)) {
        noteAssignmentOutsideClass(writer, access.site, classLayout, key, method);
        return;
    }
    LinkFlow::ComputedStoreNames stored = m_flow.findComputedStoreNames(writer, access.site);
    for (PropertyKeyID key : stored.names) {
        if (!liesInThread(key))
            noteName(key);
    }
    if (stored.names.isEmpty())
        m_classesAssignedComputedNames.set(classLayout.index());
    const BodyIndex* index = access.site ? bodiesLookedAt(m_flow.looks).find(writer) : nullptr;
    if (!index)
        return;
    if (!isBesideClass(writer, classLayout))
        return;
    LoopID loop = index->loopOf(index->blockOf(access.site));
    if (!loop || !isOnEveryPath(writer, index->block(index->loop(index->loop(loop).outermost).head).firstValue()))
        return;
    for (PropertyKeyID key : stored.names)
        noteLastAssignment(writer, access.site, classLayout, key, method);
    if (stored.areAllOwnNames && method)
        m_assignmentsOfOwnNames.append({ writer, access.site, classLayout, method });
}

void WriterOfLinkLayouts::noteLastAssignment(FunctionID writer, ValueID site, LayoutID classLayout, PropertyKeyID key, FunctionID method)
{
    if (liesInThread(key))
        return;
    auto added = m_lastAssignments.add(static_cast<uint64_t>(classLayout.index()) << 32 | key.bits(), LastAssignment { writer, site, method });
    if (added.isNewEntry)
        return;
    LastAssignment& last = added.iterator->value;
    if (last.writer != writer)
        last.method = { };
    else if (last.site.index() <= site.index())
        last = { writer, site, method };
}

LayoutID WriterOfLinkLayouts::findDecoratedClass(FunctionID id, ValueID site) const
{
    const BodyIndex* index = site ? bodiesLookedAt(m_flow.looks).find(id) : nullptr;
    if (!index || index->opcodeOf(site) != Opcode::Call)
        return { };
    std::span<const ValueID> arguments = index->value(site).as<Opcode::Call>().argumentsOperands();
    ValueID holder = arguments.size() < 2 ? ValueID() : index->stripCasts(arguments[1]);
    if (!holder || index->isParameter(holder) || index->opcodeOf(holder) != Opcode::PropGet || index->value(holder).key() != m_program.keyOf(BuiltinNames::prototype))
        return { };
    ValueID classObject = index->stripCasts(index->value(holder).objectOperand());
    if (!classObject || index->isParameter(classObject) || index->opcodeOf(classObject) != Opcode::FinishClass)
        return { };
    return index->value(classObject).as<Opcode::FinishClass>().layout();
}

bool WriterOfLinkLayouts::comesBeforeFirstObject(FunctionID id, ValueID site, LayoutID classLayout) const
{
    const BodyIndex* index = site ? bodiesLookedAt(m_flow.looks).find(id) : nullptr;
    if (!index)
        return false;
    uint32_t first = index->block(index->blockOf(site)).firstValue().index();
    for (uint32_t place = site.index(); place-- > first;) {
        BodyValue value = index->value(ValueID(place));
        if (value.opcode() == Opcode::NewClass && value.as<Opcode::NewClass>().layout() == classLayout)
            return true;
        if (effectRuleOf(value.opcode()) != EffectRule::Call)
            continue;
        ValueID callee = value.opcode() == Opcode::Call ? index->stripCasts(value.as<Opcode::Call>().calleeOperand()) : ValueID();
        if (!callee || index->isParameter(callee) || index->opcodeOf(callee) != Opcode::PropGet)
            return false;
        ValueID holder = index->stripCasts(index->value(callee).objectOperand());
        if (!holder || index->isParameter(holder) || index->opcodeOf(holder) != Opcode::PropGet || !index->value(holder).isFreeName())
            return false;
    }
    return false;
}

void WriterOfLinkLayouts::noteAccessorAssignedToClass(FunctionID writer, ValueID site, LayoutID classLayout, PropertyKeyID key, FunctionID getter, FunctionID setter)
{
    if (!classLayout || !key || liesInThread(key))
        return;
    uint64_t name = static_cast<uint64_t>(classLayout.index()) << 32 | key.bits();
    if (comesBeforeFirstObject(writer, site, classLayout)) {
        m_keysWrittenOnPrototypes.add(key.bits());
        m_accessorsBeforeObjects.add(name);
    }
    NameWrittenOnPrototype assigned { getter, setter, { }, getter || setter };
    auto added = m_accessorsAssignedToClasses.add(name, assigned);
    if (added.isNewEntry || added.iterator->value.addOtherHalf(assigned))
        return;
    NameWrittenOnPrototype& listed = added.iterator->value;
    listed.isWrittenOnce &= listed.getter == getter && listed.setter == setter;
}

void WriterOfLinkLayouts::noteAccessorAssignedToClass(FunctionID id, const ThinSummary& summary, const SummaryChange& change)
{
    if (change.kind == SummaryChange::Kind::DefinesByDescriptor && change.laysAccessorAsWritten && change.key && change.other != noSummaryNode && m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls) {
        LayoutID decorated = findDecoratedClass(id, change.site);
        if (FunctionID getter = decorated ? findOnlyFunctionAt(id, change.other) : FunctionID()) {
            noteAccessorAssignedToClass(id, change.site, decorated, change.key, getter, { });
            noteAssignmentOutsideClass(id, change.site, decorated, change.key, { });
        }
    }
    if (change.kind != SummaryChange::Kind::DefinesByDescriptor || change.laysAccessorAsWritten)
        return;
    auto closureAt = [&](uint32_t node) {
        return node != noSummaryNode && summary.nodes[node].kind == SummaryNode::Kind::Closure ? FunctionID(summary.nodes[node].first) : FunctionID();
    };
    LayoutID classLayout = findClassOfPrototypeAt(id, summary, change.subject);
    if (change.subject != noSummaryNode)
        noteNameAssignedToClass(typeAt(id, change.subject), classLayout, change.key);
    if (!classLayout || !change.key)
        return;
    if (FunctionID stored = findOnlyFunctionAt(id, change.other)) {
        noteAssignmentOutsideClass(id, change.site, classLayout, change.key, stored);
        return;
    }
    FunctionID getter = closureAt(change.getterInSight);
    FunctionID setter = closureAt(change.setterInSight);
    if (getter || setter || !change.descriptorLaysNoAccessor)
        noteAccessorAssignedToClass(id, change.site, classLayout, change.key, getter, setter);
    noteAssignmentOutsideClass(id, change.site, classLayout, change.key, findOnlyFunctionAt(id, change.valueInSight));
}

LayoutID WriterOfLinkLayouts::noteNameAssignedToClass(const LinkType& subject, LayoutID namedClass, PropertyKeyID key)
{
    if (m_flow.reading() != WhatUnseenCodeDoes::OnlyReadsAndCalls || !key || liesInThread(key))
        return { };
    LayoutID classLayout = namedClass ? namedClass : subject.objects == LinkType::Objects::Prototype ? subject.layout : LayoutID();
    if (classLayout)
        m_namesAssignedToClasses.add(static_cast<uint64_t>(classLayout.index()) << 32 | key.bits());
    else if (subject.objects == LinkType::Objects::Prototypes)
        m_familiesAssignedNames.add(key.bits(), Vector<FamilyID, 1>()).iterator->value.appendIfNotContains(subject.family);
    return classLayout;
}

bool WriterOfLinkLayouts::isNameAssignedToClass(LayoutID classLayout, PropertyKeyID key) const
{
    if (!key || liesInThread(key))
        return false;
    if (m_namesAssignedToClasses.contains(static_cast<uint64_t>(classLayout.index()) << 32 | key.bits()) || m_classesAssignedComputedNames.get(classLayout.index()))
        return true;
    auto families = m_familiesAssignedNames.find(key.bits());
    return families != m_familiesAssignedNames.end() && std::ranges::any_of(families->value, [&](FamilyID family) {
        return std::ranges::binary_search(m_flow.layoutsOfFamily(family), classLayout);
    });
}

void WriterOfLinkLayouts::noteMemberAssignment(FunctionID id, const ThinSummary& summary, const SummaryAccess& access, PropertyKeyID key, const LinkType& subject)
{
    bool isWrite = access.kind != SummaryAccess::Kind::Read;
    if (isWrite && !access.key && !access.isByDescriptor && access.kind == SummaryAccess::Kind::Store && m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls)
        noteComputedMemberAssignment(id, summary, access);
    LayoutID namedClass = access.key && isWrite ? findClassOfPrototypeAt(id, summary, access.subject) : LayoutID();
    LayoutID assignedClass = isWrite ? noteNameAssignedToClass(subject, namedClass, key) : LayoutID();
    LayoutID classLayout = namedClass || !access.key || access.laysAccessorAsWritten ? namedClass : assignedClass;
    if (classLayout && access.laysAccessorAsWritten) {
        const SummaryWrittenAccessor& accessor = summary.writtenAccessorOf(static_cast<uint32_t>(&access - summary.accesses.begin()));
        noteAccessorAssignedToClass(id, access.site, classLayout, access.key, accessor.getter, accessor.setter);
        noteAssignmentOutsideClass(id, access.site, classLayout, access.key, { });
    }
    if (!classLayout || access.laysAccessorAsWritten)
        return;
    noteAssignmentOutsideClass(id, access.site, classLayout, access.key, findFunctionAssignedAt(id, summary, access.value, classLayout, access.key));
}

void WriterOfLinkLayouts::noteNameWrittenOnPrototype(FunctionID id, const ThinSummary& summary, const SummaryAccess& access, PropertyKeyID key, const LinkType& subject)
{
    noteMemberAssignment(id, summary, access, key, subject);
    if (access.isByMethodDecorator && access.key && m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls) {
        if (LayoutID decorated = findDecoratedClass(id, access.site)) {
            FunctionID method = findOnlyFunctionAt(id, access.value);
            noteLastAssignment(id, access.site, decorated, access.key, method);
            m_methodsByDecorators.append({ id, access.site, decorated, access.key, method });
        }
    }
    SummaryWrittenAccessor accessor;
    if (access.laysAccessorAsWritten)
        accessor = summary.writtenAccessorOf(static_cast<uint32_t>(&access - summary.accesses.begin()));
    if (access.key && accessor.map) {
        auto added = m_accessorsWrittenInMaps.add(static_cast<uint64_t>(accessor.map.index()) << 32 | access.key.bits(), NameWrittenOnPrototype { accessor.getter, accessor.setter, { }, true });
        if (!added.isNewEntry && (added.iterator->value.getter != accessor.getter || added.iterator->value.setter != accessor.setter))
            added.iterator->value.isWrittenOnce = false;
    }
    if (access.key && access.kind == SummaryAccess::Kind::Store && access.value != noSummaryNode && summary.nodes[access.value].kind == SummaryNode::Kind::Closure && summary.nodes[access.subject].kind == SummaryNode::Kind::ResultOfAccess) {
        const SummaryAccess& read = summary.accesses[summary.nodes[access.subject].first];
        if (read.kind == SummaryAccess::Kind::Read && read.key == m_program.keyOf(BuiltinNames::prototype))
            m_tables.functionsWrittenRightOfNamesOnPrototypes.append({ FunctionID(summary.nodes[access.value].first), access.key });
    }
    LayoutID layout = key ? findLayoutWhosePrototypeObjectIsAt(summary, access.subject) : LayoutID();
    PassedStore passed = key && m_storeIsUnderListedKey ? findPassedStore(id, access, key, subject) : PassedStore();
    if (!layout)
        layout = passed.layout;
    bool isOnPassedHolder = !layout && key && access.laysAccessorAsWritten;
    if (isOnPassedHolder)
        layout = onlyPrototypeOfConstructorFunctionInLinkLayouts(m_flow, subject);
    if (!layout)
        return;
    NameWrittenOnPrototype written;
    if (access.kind == SummaryAccess::Kind::Define && access.laysAccessorAsWritten)
        written = { accessor.getter, accessor.setter, { }, true };
    else if (!access.isByDescriptor && access.value != noSummaryNode && summary.nodes[access.value].kind == SummaryNode::Kind::Closure)
        written = { { }, { }, FunctionID(summary.nodes[access.value].first), true };
    m_keysWrittenOnPrototypes.add(key.bits());
    bool runsAlways = access.kind == SummaryAccess::Kind::Store && isOnEveryPath(id, access.site) && (!passed.caller || isOnEveryPath(passed.caller, passed.call));
    written.isOnPassedHolder = isOnPassedHolder;
    if (runsAlways) {
        written.lastWriter = passed.caller ? passed.caller : id;
        written.lastSite = passed.caller ? passed.call : access.site;
        written.lastMethod = written.method;
    }
    auto added = m_namesWrittenOnPrototypes.add(static_cast<uint64_t>(layout.index()) << 32 | key.bits(), written);
    if (NameWrittenOnPrototype& listed = added.iterator->value; !added.isNewEntry && access.kind == SummaryAccess::Kind::Store) {
        FunctionID writer = passed.caller ? passed.caller : id;
        ValueID site = passed.caller ? passed.call : access.site;
        static constexpr unsigned mostCallsAtOnce = 4;
        for (unsigned step = 0; step < mostCallsAtOnce && listed.lastWriter && writer && writer != listed.lastWriter; ++step)
            std::tie(writer, site) = findCallAtOnce(writer, site);
        bool hasOtherWriter = listed.lastWriter && writer != listed.lastWriter;
        listed.hasSeveralWriters |= hasOtherWriter;
        if (written.method && !hasOtherWriter) {
            if (listed.methods.isEmpty() && listed.lastMethod)
                listed.methods.append({ listed.lastSite, listed.lastMethod });
            listed.methods.append({ site, written.method });
        }
        if (runsAlways && !hasOtherWriter && isOnEveryPath(writer, site) && (!listed.lastWriter || listed.lastSite.index() <= site.index())) {
            listed.lastWriter = writer;
            listed.lastSite = site;
            listed.lastMethod = written.method;
        }
    }
    if (!added.isNewEntry && added.iterator->value.addOtherHalf(written))
        return;
    if (!added.isNewEntry) {
        NameWrittenOnPrototype& listed = added.iterator->value;
        bool areAccessors = (listed.getter || listed.setter) && (written.getter || written.setter) && !listed.method;
        if (areAccessors && listed.isOnPassedHolder != isOnPassedHolder) {
            if (listed.isOnPassedHolder)
                listed = written;
            return;
        }
        listed.isWrittenOnce = false;
        listed.isWrittenAgain = true;
        return;
    }
    noteCopiedPrototypeMember(id, summary, access, static_cast<uint64_t>(layout.index()) << 32 | key.bits());
    if (access.kind != SummaryAccess::Kind::Define || access.descriptorFromElsewhere == noSummaryNode || summary.nodes[access.descriptorFromElsewhere].kind != SummaryNode::Kind::ResultOfAccess)
        return;
    const SummaryAccess& taken = summary.accesses[summary.nodes[access.descriptorFromElsewhere].first];
    LinkType map = typeAt(id, taken.subject);
    if (isLibraryCode(m_program, m_program.at(id)))
        map = map.directPart();
    if (taken.kind == SummaryAccess::Kind::Read && taken.key && map.objects == LinkType::Objects::Layout && map.functions == LinkType::Functions::None && !map.hasUnknownObjects)
        m_namesLaidOutOfMaps.append({ static_cast<uint64_t>(layout.index()) << 32 | key.bits(), static_cast<uint64_t>(map.layout.index()) << 32 | taken.key.bits() });
}

auto WriterOfLinkLayouts::findPassedStore(FunctionID id, const SummaryAccess& access, PropertyKeyID key, const LinkType& subject) const -> PassedStore
{
    const BodyIndex* index = access.site ? bodiesLookedAt(m_flow.looks).find(id) : nullptr;
    if (!index || index->isParameter(access.site) || index->opcodeOf(access.site) != Opcode::ElemSet)
        return { };
    auto store = index->value(access.site).as<Opcode::ElemSet>();
    std::optional<uint32_t> position = m_flow.findPassedPosition(id, *index, store.propertyOperand());
    if (!position)
        return { };
    std::optional<uint32_t> subjectPosition = m_flow.findPassedPosition(id, *index, store.objectOperand());
    const LinkFlow::CallsInSight& found = m_flow.findCallsInSight(id);
    PassedStore passed;
    for (size_t call = 0; call < found.calls.size(); ++call) {
        if (m_flow.findPassedText(found, call, *position) != m_program.at(key).text)
            continue;
        if (passed.caller)
            return { };
        passed.caller = found.caller;
        passed.call = found.calls[call].first;
        FlowNodeID node = subjectPosition ? m_flow.findPassedNode(id, found, call, *subjectPosition) : FlowNodeID();
        passed.layout = onlyPrototypeOfConstructorFunctionInLinkLayouts(m_flow, node ? m_flow.typeAt(node) : subject);
    }
    return passed.layout ? passed : PassedStore();
}

void WriterOfLinkLayouts::noteCopiedPrototypeMember(FunctionID id, const ThinSummary& summary, const SummaryAccess& store, uint64_t copy)
{
    if (store.kind != SummaryAccess::Kind::Store || store.isByDescriptor || store.value == noSummaryNode || summary.nodes[store.value].kind != SummaryNode::Kind::ResultOfAccess)
        return;
    const SummaryAccess& read = summary.accesses[summary.nodes[store.value].first];
    if (read.kind != SummaryAccess::Kind::Read || !read.key)
        return;
    LayoutID original = findLayoutWhosePrototypeObjectIsAt(summary, read.subject);
    if (!original)
        original = onlyPrototypeOfConstructorFunctionInLinkLayouts(m_flow, typeAt(id, read.subject));
    if (original)
        m_copiedPrototypeMembers.append({ copy, static_cast<uint64_t>(original.index()) << 32 | read.key.bits() });
}

void WriterOfLinkLayouts::findCopiedPrototypeMemberFunctions()
{
    for (auto [copy, original] : m_copiedPrototypeMembers) {
        auto found = m_namesWrittenOnPrototypes.find(original);
        auto copied = m_namesWrittenOnPrototypes.find(copy);
        if (found == m_namesWrittenOnPrototypes.end() || !found->value.isWrittenOnce || !found->value.method || copied->value.isWrittenAgain)
            continue;
        const Function& method = m_program.at(found->value.method);
        const Function& constructor = m_program.at(m_program.at(LayoutID(static_cast<uint32_t>(copy >> 32))).constructor);
        if (!isLibraryCode(m_program, method) || !isLibraryCode(m_program, constructor) || (method.module != constructor.module && !method.captures.isEmpty()))
            continue;
        copied->value.method = found->value.method;
        copied->value.isWrittenOnce = true;
        copied->value.isCopied = true;
    }
}

bool WriterOfLinkLayouts::isWrittenOnPrototypeAbove(LayoutID layout, PropertyKeyID key) const
{
    if (!m_keysWrittenOnPrototypes.contains(key.bits()))
        return false;
    size_t stepsLeft = m_program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; --stepsLeft) {
        uint64_t name = static_cast<uint64_t>(current.index()) << 32 | key.bits();
        if (m_accessorsBeforeObjects.contains(name))
            return true;
        if (auto written = m_namesWrittenOnPrototypes.find(name); written != m_namesWrittenOnPrototypes.end()) {
            const LinkFlow::NameOnPrototype* laid = m_flow.findNameOnPrototype(current, key);
            if (written->value.getter || written->value.setter || !laid || (!m_flow.hasOnlyPrimitives(*laid) && !m_flow.hasNoFunctionsInSight(*laid)))
                return true;
        }
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
        return;
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        noteStoreOnPrototypeObjects(subject, key);
        return;
    case LinkType::Objects::Layout:
        if (traits.contains(Trait::AddsWhatIsNotListed) && !key && !subject.hasUnknownObjects)
            m_layoutsExtendedAlone.quickSet(subject.layout.index());
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
        if (traits.contains(Trait::MayChangePrototype) && (!key || m_program.nameClassOf(key) == NameClass::Proto))
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
    for (const LinkType& older : olderObjectsThatSuperCallAnswersWith(m_flow.classUnderConstruction(writer)))
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
    if (traits.contains(Trait::MayChangePrototype) && (!key || m_program.nameClassOf(key) == NameClass::Proto))
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
            m_gainsUnknownNames.quickSet(layout.index());
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
            m_layoutsExtendedAlone.quickSet(layout.index());
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
    if (traits.contains(Trait::MayGiveValue)) {
        bool definesReadOnlyValue = m_currentChangeDefinesAttributes || (m_definitionAtHand && m_definitionAtHand->isByDescriptor && m_definitionAtHand->formCreated && m_definitionAtHand->formCreated->contains(FieldForm::IsReadOnly));
        (definesReadOnlyValue ? m_fieldsRedefinedWithAttributes : m_fieldsGivenValues).quickSet(cellOf(*found));
    }
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
    bool isInstanceOfClass = m_program.at(layout).kind == LayoutKind::Instance && !m_program.at(layout).has(LayoutFlags::IsOfCreatedObject);
    bool constructorStoresOverMethod = isInstanceOfClass && (m_namesThatConstructorsStore.contains(layoutAndKey) || isStoredOnlyAtAllocation(layout, key));
    if (mayBeElementKey(nameClass) || nameClass == NameClass::Proto || (!constructorStoresOverMethod && m_flow.findMembers(layout, ClassSide::Prototype, key).mayAnswer()) || m_flow.mayLieOnPrototypeAbove(layout, key, countsKeysThatNobodyKnows, LinkFlow::PrimitivesOnPrototypes::AreIgnoredWithObjects))
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

bool WriterOfLinkLayouts::isStoredOnlyAtAllocation(LayoutID layout, PropertyKeyID key) const
{
    uint64_t layoutAndKey = static_cast<uint64_t>(layout.index()) << 32 | key.bits();
    if (m_namesStoredAtAllocations.isEmpty() || !m_namesStoredAtAllocations.contains(layoutAndKey) || m_namesStoredElsewhere.contains(layoutAndKey) || m_flow.escapedLayouts.get(layout.index()))
        return false;
    const Layout& record = m_program.at(layout);
    return !record.base && !record.has(LayoutFlags::HasHeritage) && m_program.layoutsExpectedBelow(layout).empty();
}

void WriterOfLinkLayouts::noteStoreOnPrototypeObjects(const LinkType& subject, PropertyKeyID key)
{
    auto note = [&](LayoutID layout) {
        m_namesStoredOnPrototypeObjects.add(static_cast<uint64_t>(layout.index()) << 32 | key.bits());
    };
    if (subject.objects == LinkType::Objects::Prototype)
        note(subject.layout);
    else if (subject.objects == LinkType::Objects::Prototypes)
        m_program.at(m_flow.families.layoutsOf(subject.family)).forEach(note);
}

FunctionID WriterOfLinkLayouts::findMethodOnlyFromClass(LayoutID layout, PropertyKeyID key) const
{
    LinkFlow::FoundMembers members = m_flow.findMembers(layout, ClassSide::Prototype, key);
    if (members.lookupGoesOnAboveClasses() || members.size() != 1 || members[0].kind != MethodKind::Method || m_flow.namesMayBeIncomplete(layout, ClassSide::Prototype))
        return { };
    size_t stepsLeft = m_program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = m_program.at(current).base, --stepsLeft) {
        uint64_t prototype = static_cast<uint64_t>(current.index()) << 32;
        if (hasEscaped(current.index()) || m_namesStoredOnPrototypeObjects.contains(prototype | key.bits()) || m_namesStoredOnPrototypeObjects.contains(prototype | PropertyKeyID().bits()))
            return { };
        if (current != members[0].classLayout)
            continue;
        auto found = std::ranges::find_if(m_members, [&](const LinkedMember& member) {
            return member.classLayout == current && member.side == ClassSide::Prototype && member.index == members[0].index;
        });
        return found != m_members.end() && found->kind == MethodKind::Method && found->key == key ? found->written : FunctionID();
    }
    return { };
}

bool WriterOfLinkLayouts::noteNameNotListed(LayoutID layout, PropertyKeyID key)
{
    if (m_program.at(layout).kind == LayoutKind::Instance && m_flow.findMembers(layout, ClassSide::Prototype, key).mayAnswer())
        (m_storedObjectIsAllocatedInBody ? m_namesStoredAtAllocations : m_namesStoredElsewhere).add(static_cast<uint64_t>(layout.index()) << 32 | key.bits());
    if (!canHoldAppendedFields(layout) || !canNameAppendedField(layout, key))
        return false;
    auto added = m_pairsNotListed.add(static_cast<uint64_t>(layout.index()) << 32 | key.bits(), static_cast<uint32_t>(m_namesNotListed.size()));
    if (added.isNewEntry)
        m_namesNotListed.append({ layout, key });
    m_namesNotListed[added.iterator->value].isStoredUnderWrittenName |= !m_storeIsUnderListedKey;
    (m_storedObjectIsAllocatedInBody ? m_namesNotListed[added.iterator->value].isStoredByMaker : m_namesNotListed[added.iterator->value].isStoredBehindMaker) = true;
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

    HashMap<uint32_t, Vector<uint32_t, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> namesUnderKey;
    for (uint32_t place = 0; place < m_namesNotListed.size(); ++place)
        namesUnderKey.add(m_namesNotListed[place].key.bits(), Vector<uint32_t, 2>()).iterator->value.append(place);

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
            bool isRequired = field.flags.contains(FieldFlags::IsRequired);
            auto noteDescription = [&](NameNotListed& name) {
                name.isRequiredBySomeWrittenType |= isRequired;
                if (m_program.at(name.layout).supertypes.contains(LayoutID(index)))
                    (isRequired ? name.isRequiredBySomeTypeOfThis : name.isOptionalInSomeTypeOfThis) = true;
            };
            bool isStoredOnUnknownObjects = m_keysStoredOnUnknownObjects.contains(field.key.bits());
            if (!isStoredOnUnknownObjects) {
                auto names = namesUnderKey.find(field.key.bits());
                if (names == namesUnderKey.end())
                    continue;
                if (names->value.size() < described.size()) {
                    for (uint32_t place : names->value) {
                        if (std::ranges::binary_search(described, m_namesNotListed[place].layout))
                            noteDescription(m_namesNotListed[place]);
                    }
                    continue;
                }
            }
            for (size_t place = 0; place < described.size(); ++place) {
                LayoutID id = described[place];
                auto noted = m_pairsNotListed.find(static_cast<uint64_t>(id.index()) << 32 | field.key.bits());
                if (noted == m_pairsNotListed.end()) {
                    if (!isStoredOnUnknownObjects || !mayHideBehindUnknownObjects[place] || findField(m_program, id, field.key) || !noteNameNotListed(id, field.key))
                        continue;
                    noted = m_pairsNotListed.find(static_cast<uint64_t>(id.index()) << 32 | field.key.bits());
                }
                noteDescription(m_namesNotListed[noted->value]);
            }
        }
    }
}

void LinkTables::sortInitialNames()
{
    auto bitsOf = [](PropertyKeyID key) {
        return key.bits();
    };
    for (LinkedInitialNames& names : initialNames)
        std::ranges::sort(names.keys, { }, bitsOf);
    std::ranges::sort(initialNames, [&](const LinkedInitialNames& first, const LinkedInitialNames& second) {
        return first.layout == second.layout ? std::ranges::lexicographical_compare(first.keys, second.keys, { }, bitsOf, bitsOf) : first.layout.index() < second.layout.index();
    });
    auto repeated = std::ranges::unique(initialNames, [](const LinkedInitialNames& first, const LinkedInitialNames& second) {
        return first.layout == second.layout && std::ranges::equal(first.keys, second.keys);
    });
    initialNames.shrink(initialNames.size() - repeated.size());
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
        name.hasDefaultOnPrototype = m_flow.mayLieOnPrototypeAbove(name.layout, name.key, false) && !m_flow.mayLieOnPrototypeAbove(name.layout, name.key, false, LinkFlow::PrimitivesOnPrototypes::AreIgnored);
    for (NameNotListed& name : m_namesNotListed)
        name.isLaidOnObjectsOfLibrary = m_namesLaidOnObjectsOfLibrary.contains(static_cast<uint64_t>(name.layout.index()) << 32 | name.key.bits());
    for (NameNotListed& name : m_namesNotListed) {
        uint64_t layoutAndKey = static_cast<uint64_t>(name.layout.index()) << 32 | name.key.bits();
        name.isLaidLate = m_namesThatMembersAboveStore.contains(layoutAndKey) && !m_namesThatMakersStore.contains(layoutAndKey);
    }
    Vector<const RunOfMaker*, 8> makersInFront;
    constexpr uint32_t extensionCostFactor = 8;
    HashSet<std::pair<uint64_t, uint32_t>> makersNoted;
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> numbersOfMakers;
    constexpr uint32_t anyName = 0;
    auto makersWithName = [](FunctionID first, uint32_t name) {
        return static_cast<uint64_t>(first.index()) << 32 | name;
    };
    auto countMaker = [&](FunctionID first, uint32_t name, FunctionID maker) {
        if (makersNoted.add({ makersWithName(first, name), maker.index() }).isNewEntry)
            ++numbersOfMakers.add(makersWithName(first, name), 0).iterator->value;
    };
    for (const RunOfMaker& run : m_runsOfMakers) {
        makersInFront.shrink(0);
        makersInFront.append(&run);
        while (!makersInFront.last()->isMadeHere && makersInFront.last()->givenBy && !m_runsOnWhatFunctionsReturn.isEmpty() && m_runsOnWhatFunctionsReturn[makersInFront.last()->givenBy.index()]) {
            const RunOfMaker* above = &m_runsOfMakers[m_runsOnWhatFunctionsReturn[makersInFront.last()->givenBy.index()] - 1];
            if (makersInFront.contains(above))
                break;
            makersInFront.append(above);
        }
        const RunOfMaker* first = makersInFront.last();
        if (!first->isMadeHere)
            continue;
        countMaker(first->body, anyName, run.body);
        for (const RunOfMaker* maker : makersInFront) {
            for (PropertyKeyID key : maker->keys)
                countMaker(first->body, key.bits() + 1, run.body);
        }
    }
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
                    m_namesNotListed[*place].isGivenBySecondConstructor |= extensionCostFactor * numbersOfMakers.get(makersWithName(first->body, key.bits() + 1)) >= numbersOfMakers.get(makersWithName(first->body, anyName));
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

void WriterOfLinkLayouts::listCallsThatEnter()
{
    m_onlyCallersOutsideLoops.startFor(m_program.functions.size());
    std::span<const LinkFlow::CalleesOfCall> calleesOfCalls = m_flow.calleesOfCallsAsSettled();
    forEachRangeOfIndicesInParallel(m_flow.loopsOfReading(), m_program.functions.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            if (!m_flow.reached.get(index))
                continue;
            std::span<const SummaryCall> calls = m_flow.summaries.at(FunctionID(index)).calls.span();
            for (size_t place = 0; place < calls.size(); ++place) {
                for (FunctionID callee : calleesOfCalls[m_flow.firstCallOfFunction[index] + place].listed()) {
                    if (calls[place].isInLoop)
                        m_onlyCallersOutsideLoops.noteSeveral(callee.index());
                    else
                        m_onlyCallersOutsideLoops.note(callee.index(), FunctionID(index));
                }
            }
        }
    });
}

void WriterOfLinkLayouts::listMakersOfRecords()
{
    m_bodiesEnteredOnce = bodiesThatLoaderEntersAtMostOnce(m_program);
    m_onlyMakersOfRecords.startFor(m_program.layouts.size());
    forEachRangeOfIndicesInParallel(m_flow.loopsOfReading(), m_program.functions.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            FunctionID function(index);
            const ThinSummary& summary = m_flow.summaries.at(function);
            if (!m_flow.reached.get(index) || !summary.holdsNodesOf(SummaryNode::Kind::Allocation))
                continue;
            for (const SummaryNode& node : summary.nodes) {
                if (node.kind == SummaryNode::Kind::Allocation && m_onlyMakersOfRecords.at(node.first) != function)
                    m_onlyMakersOfRecords.note(node.first, function);
            }
        }
    });
}

bool WriterOfLinkLayouts::runsAtMostOnce(FunctionID function)
{
    for (size_t steps = 0; steps < m_program.functions.size(); ++steps) {
        if (m_bodiesEnteredOnce.get(function.index()))
            return true;
        if (!everyCallerIsKnown(function))
            return false;
        if (m_onlyCallersOutsideLoops.isEmpty())
            listCallsThatEnter();
        function = m_onlyCallersOutsideLoops.at(function.index());
        if (!function)
            return false;
    }
    return false;
}

bool WriterOfLinkLayouts::isSingletonRecord(LayoutID layout)
{
    if (m_program.at(layout).kind != LayoutKind::Record || m_isMadeAtNoNode.quickGet(layout.index()))
        return false;
    if (m_onlyMakersOfRecords.isEmpty())
        listMakersOfRecords();
    FunctionID maker = m_onlyMakersOfRecords.at(layout.index());
    if (!maker || !runsAtMostOnce(maker))
        return false;
    const BodyIndex* index = bodiesLookedAt(m_flow.looks).find(maker);
    if (!index || index->hasIrreducibleFlow())
        return false;
    for (uint32_t block = 0; block < index->numberOfBlocks(); ++block) {
        if (!index->loopOf(BlockID(block)))
            continue;
        for (BodyValue value : index->block(BlockID(block)).values()) {
            if (value.opcode() == Opcode::NewObject && value.as<Opcode::NewObject>().layout() == layout)
                return false;
        }
    }
    return true;
}

void WriterOfLinkLayouts::appendFields()
{
    m_pairsNotListed.clear();
    BitVector countingLiterals;
    m_namesNotListed.removeAllMatching([&](const NameNotListed& name) {
        bool bringsFormToObjectsFilledUnderComputedKeys = m_isFilledUnderComputedKeys.quickGet(name.layout.index()) && !name.formToPlanFor().isEmpty();
        bool mayHideMethod = !isStoredOnlyAtAllocation(name.layout, name.key) || findMethodOnlyFromClass(name.layout, name.key);
        bool mayStoreToAccessor = standsBelowComputedAccessors(name.layout) && !name.isRequiredByTypeOfThis() && !name.isRequiredBySomeWrittenType && !name.isDefinedByDescriptors && (name.isStoredBehindMaker || !name.isStoredByMaker || !isFilledByCreatorsAlone(name.layout));
        bool isCountedAlone = !name.isStoredUnderWrittenName && m_namesCountedInLoops.contains(static_cast<uint64_t>(name.layout.index()) << 32 | name.key.bits()) && m_program.at(name.layout).kind == LayoutKind::Record && m_program.at(name.layout).fields.isEmpty();
        if (isCountedAlone)
            countingLiterals.set(name.layout.index());
        if (!isCountedAlone && !bringsFormToObjectsFilledUnderComputedKeys && mayHideMethod && !mayStoreToAccessor && canNameAppendedField(name.layout, name.key))
            return false;
        m_changes[name.layout.index()].add(LayoutChange::GainsProperty);
        m_layoutsExtendedAlone.quickSet(name.layout.index());
        m_tables.namesStoredBesideFields.append({ name.layout, name.key });
        return true;
    });
    for (size_t layout : countingLiterals) {
        m_isFilledUnderComputedKeys.quickSet(layout);
        m_flow.build.statistics().count(StatisticID::CountersKeptAsDictionaries);
    }
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
            if (FunctionID method = isStoredOnlyAtAllocation(name.layout, name.key) && !liesInThread(name.key) ? findMethodOnlyFromClass(name.layout, name.key) : FunctionID()) {
                m_tables.keysOfFieldsOverMethods.set(name.key.index());
                m_tables.methodsUnderFields.add(LinkTables::layoutAndName(name.layout, name.key), method);
            }
            m_formsOfAppendedKeys.append(name.formToPlanFor());
        };
        size_t numberListed = m_program.layouts[index].fields.size();
        size_t roomInKeyOrder = maxFieldsInOrder - std::min<size_t>(maxFieldsInOrder, numberListed);
        size_t numberOutOfLine = names.size() - std::ranges::count_if(names, &NameNotListed::isRequiredByTypeOfThis);
        size_t mostOutOfLine = maxAppendedFieldsOf(numberListed);
        bool isSingleton = numberOutOfLine > mostOutOfLine && isSingletonRecord(LayoutID(index));
        if (isSingleton)
            m_tables.singletonRecords.set(index);
        if (names.size() <= roomInKeyOrder && (numberOutOfLine <= mostOutOfLine || isSingleton)) {
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
            if (!isSingleton && !names[place].isRequiredByTypeOfThis() && numberOutOfLine++ >= mostOutOfLine)
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
    for (const LinkedNameBesideFields& name : m_tables.namesStoredBesideFields)
        m_tables.keysStoredOutsideFields.set(name.key.bits());
    m_tables.firstAppendedFieldOfLayout.append(static_cast<uint32_t>(m_tables.appendedKeys.size()));
    for (uint32_t index = 0; index + 1 < m_tables.firstAppendedFieldOfLayout.size(); ++index) {
        for (uint32_t place = m_tables.firstAppendedFieldOfLayout[index]; place < m_tables.firstAppendedFieldOfLayout[index + 1]; ++place) {
            if (m_tables.appendedFieldsThatTypeOfThisRequires.get(place))
                continue;
            m_tables.appendedFieldsThatTakeWordLeftInCell.set(place);
            break;
        }
    }
    findLateFieldsFilledTogether();
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
    bool takesOtherArm = false;
    auto keysTakenOf = [&](const RunOfMaker& maker) {
        if (takesOtherArm && !maker.keysOfOtherArm.isEmpty())
            return maker.keysOfOtherArm.span();
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
    takesOtherArm = true;
    for (const RunOfMaker& run : m_runsOfMakers) {
        if (run.keysOfOtherArm.isEmpty())
            continue;
        for (LayoutID layout : run.layouts)
            noteRunOf(layout, run, run.layouts.size() == 1);
        if (run.layouts.isEmpty())
            noteRunOnLayoutsThatUnknownObjectsMayBe(run.keysOfOtherArm.span(), run);
    }
    takesOtherArm = false;

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
        bool mayGainWhatNobodyNames = m_tables.layouts[index].possibleChanges.contains(LayoutChange::GainsProperty) && !m_gainsOnlyWhatWrittenDefinitionsLay.quickGet(index) && !m_gainsListedNamesAlone.quickGet(index);
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
            LinkType held = m_flow.typeAt(node);
            bool holdsInt32sAlone = m_flow.reading() == WhatUnseenCodeDoes::Anything && !(held.primitives & Primitive::Double) && (held.counts == LinkType::Counts::BelowTwoTo31 || held.counts == LinkType::Counts::Int32);
            cell.proven = held.isNever() ? TypeID(TypeTable::never) : claimedTypeAt(node, holdsInt32sAlone ? Numbers::AsHeld : Numbers::ByKindAlone);
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

void WriterOfLinkLayouts::noteReplacement(LinkedMember& member, const LinkType& held) const
{
    member.isReplaced = held.functions == LinkType::Functions::Set || held.functions == LinkType::Functions::Lost || held.builtin || held.hasDirectUnknownObjects();
    if (held.functions != LinkType::Functions::Set || member.kind != MethodKind::Method)
        return;
    std::span<const FunctionID> functions = m_flow.functionsOfSet(held);
    if (functions.size() == 2 && (functions[0] == member.written || functions[1] == member.written))
        member.replacement = functions[0] == member.written ? functions[1] : functions[0];
}

ALWAYS_INLINE void WriterOfLinkLayouts::lookAtMembers(const ThinSummary& summary)
{
    for (const SummaryMember& member : summary.members) {
        if (member.isRedefined)
            continue;
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
            else if (m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls && linked.written)
                noteReplacement(linked, held);
        }
        if (m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls && linked.kind != MethodKind::Method && linked.key)
            linked.isReplaced |= m_flow.isDefinedByDescriptorSomewhere(linked.key);
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

bool WriterOfLinkLayouts::isConstructedOnlyBySuper(LayoutID id) const
{
    const Layout& layout = m_program.at(id);
    if (layout.kind != LayoutKind::Instance || !layout.constructor || layout.isDerived() || m_flow.allocatedLayouts.get(id.index()) || m_isMadeAtNoNode.quickGet(id.index()))
        return false;
    return m_program.at(layout.constructor).kind == FunctionKind::BaseConstructor && everyCallerIsKnown(layout.constructor);
}

void WriterOfLinkLayouts::writeLayouts()
{
    bool everyPrototypeMayChange = objectPrototypeMayGainName();
    BitVector belowSeveralHeritages = answerAlongRows(m_tables.layouts.size(), false, [&](LayoutID id) -> StepAlongRow {
        if (m_hasSeveralHeritages.quickGet(id.index()))
            return { true, { } };
        LayoutID above = m_program.at(id).base ? m_program.at(id).base : m_heritages[id.index()];
        if (!above)
            return { false, { } };
        return { std::nullopt, above };
    });
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
        const Layout& layout = m_program.at(id);
        bool isOfClass = layout.kind == LayoutKind::Instance && layout.constructor && !layout.isDerived() && isClassConstructorOf(m_program.at(layout.constructor).kind);
        linked.isMadeInSight = !isOfClass || m_flow.constructedClasses.get(index);
        linked.hasLockableInstances = m_flow.hasLockableInstances(id);
        linked.possibleChanges = LinkedLayout { }.possibleChanges;
        linked.listsEveryMemberAbove = !m_flow.namesMayBeIncomplete(id, ClassSide::Prototype);
        linked.listsEveryStaticMember = !m_flow.namesMayBeIncomplete(id, ClassSide::Static);
        linked.isFilledUnderComputedKeys = m_isFilledUnderComputedKeys.quickGet(index);
        linked.instancesAreLaidAsPrototypes = m_flow.layoutsWhoseInstancesAreLaidAsPrototypes.quickGet(index);
        linked.heritage = m_heritages[index];
        linked.hasSeveralHeritages = belowSeveralHeritages.quickGet(index);
        if (m_asksWhatMakersGive && !m_layoutsMadeInSeveralFunctions.quickGet(index))
            linked.onlyFunctionThatMakesObjects = m_onlyFunctionsThatMakeObjects[index];
        linked.isExtendedAlone = m_layoutsExtendedAlone.quickGet(index);
        if (hasEscaped(index) || !isFoundByShapeTest(m_program.at(id)))
            continue;

        if (m_flow.reading() == WhatUnseenCodeDoes::Anything)
            linked.reachesUnseenCode = false;
        linked.hasDirectInstances = linked.isAllocated && (linked.reachesUnseenCode || !isConstructedOnlyBySuper(id));
        OptionSet<LayoutChange> changesOfFlow = m_flow.possibleChangesOf(index);
        bool isGivenPrototypesByStoresAlone = !m_flow.changesByMoreThanStoresInSight[index].contains(LayoutChange::ChangesPrototype) && !m_flow.layoutsThatUnseenCodeMayChange.get(index) && !m_flow.lostLayouts.get(index) && !m_layoutsCopiedInto.quickGet(index);
        if (isGivenPrototypesByStoresAlone)
            changesOfFlow.remove(LayoutChange::ChangesPrototype);
        linked.possibleChanges = m_changes[index];
        linked.possibleChanges.add(changesOfFlow);
        if (writtenBaseOf(m_program.at(id)) && m_program.at(id).inheritedFields != InheritedFields::AllOfRow)
            linked.possibleChanges.add(LayoutChange::GainsProperty);
        bool gainsBesideWhatDefinitionsLay = linked.possibleChanges.contains(LayoutChange::GainsProperty) || (m_flow.lostLayouts.get(index) && m_changesOfLostFamilies[lostFamilyOf(id)].contains(LayoutChange::GainsProperty));
        if (gainsBesideWhatDefinitionsLay && m_flow.reading() != WhatUnseenCodeDoes::Anything && !m_gainsUnknownNames.quickGet(index) && !changesOfFlow.contains(LayoutChange::GainsProperty) && !m_flow.lostLayouts.get(index) && !writtenBaseOf(m_program.at(id)))
            m_gainsListedNamesAlone.quickSet(index);
        if (!gainsBesideWhatDefinitionsLay && m_flow.layoutsDefinedOnUnderWrittenKeys.get(index) && !m_flow.layoutsDefinedOnUnderKeysThatAreNotWritten.get(index))
            m_gainsOnlyWhatWrittenDefinitionsLay.quickSet(index);
        bool isRedefinedBesideWrittenDefinitions = linked.possibleChanges.contains(LayoutChange::GetsAccessor) || (m_flow.lostLayouts.get(index) && m_changesOfLostFamilies[lostFamilyOf(id)].contains(LayoutChange::GetsAccessor));
        if (!isRedefinedBesideWrittenDefinitions && m_flow.layoutsDefinedOnUnderWrittenKeys.get(index) && !m_flow.layoutsDefinedOnUnderKeysThatAreNotWritten.get(index) && canHoldAccessors(m_program, id))
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
        bool deletionsAreNamed = !m_changes[index].contains(LayoutChange::LosesProperty) && !m_flow.layoutsDeletedUnderUnknownKeys.get(index) && !m_flow.layoutsThatUnseenCodeMayChange.get(index);
        bool isDeletedFromThroughLost = m_flow.lostLayouts.get(index) && m_changesOfLostFamilies[lostFamilyOf(id)].contains(LayoutChange::LosesProperty);
        linked.losesNamedFieldsAlone = linked.possibleChanges.contains(LayoutChange::LosesProperty) && deletionsAreNamed && !isDeletedFromThroughLost;
        if (m_flow.isConstructedPrototype(id)) {
            linked.possibleChanges = LinkedLayout { }.possibleChanges;
            linked.losesNamedFieldsAlone = false;
        }
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

LayoutID LinkFlow::classUnderConstruction(FunctionID writer) const
{
    const Function& function = program.at(writer);
    if (function.kind == FunctionKind::DerivedConstructor)
        return function.constructedClass;
    TypeID own = function.kind == FunctionKind::Initializer ? function.thisType.written() : TypeID();
    if (!own || holdsOnlyPrimitives(own) || !program.at(own).hasOnlyLayouts())
        return { };
    LayoutID constructed;
    for (LayoutID member : Typed::membersOf(TypeReader(program), program.at(own))) {
        if (program.at(member).kind == LayoutKind::Instance && (!constructed || standsAtOrBelow(constructed, member)))
            constructed = member;
    }
    return constructed;
}

bool LinkFlow::mayStoreIntoOlderObjectThatSuperCallAnswered(FunctionID writer, const SummaryAccess& access) const
{
    if (!access.isPartOfConstruction || summaries.at(writer).nodes[access.subject].kind == SummaryNode::Kind::Allocation)
        return false;
    LayoutID constructed = classUnderConstruction(writer);
    return constructed && superCallMayAnswerWithOlderObject(constructed);
}

bool LinkFlow::isPartOfConstructionOf(LayoutID layout, FunctionID writer, const SummaryAccess& access) const
{
    if (!mayStoreIntoOlderObjectThatSuperCallAnswered(writer, access))
        return access.isPartOfConstruction;
    return standsAtOrBelow(layout, classUnderConstruction(writer));
}

bool LinkFlow::isPartOfConstructionOfAllOf(FamilyID family, FunctionID writer, const SummaryAccess& access) const
{
    if (!mayStoreIntoOlderObjectThatSuperCallAnswered(writer, access))
        return access.isPartOfConstruction;
    return everyMemberStandsAtOrBelow(family, classUnderConstruction(writer));
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
    static constexpr uint8_t isWhatMakersLay = 8;
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
            uint32_t cell = cellOf(reference);
            if (m_flow.typeAt(node).isNever() && (m_classesThatMakersLayInFields.isEmpty() || !m_classesThatMakersLayInFields[cell]))
                continue;
            Numbers numbers = m_program.layouts[index].isEnvironment() ? Numbers::AsVariableHolds : m_flow.reading() == WhatUnseenCodeDoes::Anything ? Numbers::AsHeld : Numbers::ByKindAlone;
            TypeID arrived = claimedTypeAt(node, keepsExactIntegersInFields(m_program.layouts[index].kind) ? Numbers::AsSlotHolds : numbers);
            if (PropertyKeyID key = fieldAt(m_program, reference).key; key && arrived && m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls) {
                if (FlowNodeID laid = m_flow.findLaidByDescriptorsOn(LayoutID(index), key); laid && !m_flow.typeAt(laid).isNever()) {
                    TypeID byDescriptors = claimedTypeAt(laid);
                    arrived = byDescriptors ? types().unionOf(arrived, byDescriptors) : TypeID();
                }
            }
            if (m_program.layouts[index].isEnvironment())
                arrived = namingSymbolAt(node, arrived);
            bool saysWhatMakersLay = false;
            uint32_t laid = m_classesThatMakersLayInFields.isEmpty() ? 0 : m_classesThatMakersLayInFields[cell];
            bool tellsClassAmongSeveral = isTypedArrayAmongMadeClasses(laid) && arrived && types().at(arrived).hasObjects() && types().at(types().withOnlyObjects(arrived)).hasOnlyClassObjects()
                && classPartsOf(types(), types().at(arrived)).size() > 1;
            bool hasEveryPrimitive = arrived && (types().at(arrived).primitives & Primitive::All) == Primitive::All;
            bool isReadAsString = (!laid || laid == otherPrimitivesAmongWhatMakersLay) && m_fieldsReadAsStrings.get(cell) && (!arrived || types().isUninformative(arrived) || hasEveryPrimitive);
            bool mayHoldAnyObject = arrived && !arePrimitivesAmongWhatMakersLay(laid) && types().at(arrived).hasUnboundedUnknown();
            if (arePrimitivesAmongWhatMakersLay(laid) && !areNumbersAmongWhatMakersLay(laid) && laid != otherPrimitivesAmongWhatMakersLay)
                laid = 0;
            bool isNumberOfUnknownOperator = areNumbersAmongWhatMakersLay(laid) && arrived && !types().at(arrived).hasObjects() && types().at(arrived).primitives & Primitive::Number && types().at(arrived).primitives & Primitive::BigInt;
            if ((arrived && !types().isUninformative(arrived) && !tellsClassAmongSeveral && !hasEveryPrimitive && !mayHoldAnyObject && !isNumberOfUnknownOperator) || laid == otherPrimitivesAmongWhatMakersLay)
                laid = 0;
            if (areNumbersAmongWhatMakersLay(laid)) {
                arrived = areIntegersAmongWhatMakersLay(laid) ? TypeTable::int32 : TypeTable::number;
                saysWhatMakersLay = true;
            } else if (isReadAsString) {
                arrived = TypeTable::string;
                saysWhatMakersLay = true;
            } else if (laid && laid != severalClasses) {
                TypeID ofElements = storedElementClass(cell);
                TypeID made = laid & layoutAmongWhatMakersLay ? types().forLayout(LayoutID((laid & ~layoutAmongWhatMakersLay) - 1))
                    : types().builtinClass(BuiltinClassID(builtinAmongMadeClasses(laid) - 1), ofElements ? singleElementSpan(ofElements) : std::span<const TypeID>());
                arrived = types().withPrimitives(made, Primitive::UndefinedOrNull);
                saysWhatMakersLay = true;
            } else if (TypeID ofElements = storedElementClass(cell); ofElements && arrived && types().at(arrived).hasObjects() && types().withOnlyObjects(arrived) == types().builtinClass(BuiltinClasses::Array))
                arrived = types().withPrimitives(types().builtinClass(BuiltinClasses::Array, singleElementSpan(ofElements)), types().at(arrived).primitives);
            provenAtDeclaration[cell] = arrived;
            arrivesAtDeclaration[cell] = (arrived ? somethingArrives : somethingArrives | anythingArrives) | (claimLeavesOutWhatArrivesAt(node) || saysWhatMakersLay ? partIsLeftOut : 0) | (saysWhatMakersLay ? isWhatMakersLay : 0);
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
        says.keepsWhatConstructionsFill = says.everyConstructionIsSeen && !m_tables.layouts[index].possibleChanges.contains(LayoutChange::GetsAccessor) && (!m_tables.layouts[index].possibleChanges.contains(LayoutChange::LosesProperty) || m_tables.layouts[index].losesNamedFieldsAlone);
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
    Vector<uint32_t> layoutsLikeBase = findIndicesInParallel(m_flow.loopsOfReading(), numberOfLayouts, [&](unsigned index) {
        LayoutID base = baseWhoseFieldListIsCopiedWhole(m_program, LayoutID(index));
        if (!base)
            return false;
        WhatLayoutSaysOfCells says = whatLayoutSays(index);
        bool isRedefined = m_isRedefinedByWrittenDefinitionsAlone.quickGet(index) || m_isRedefinedByWrittenDefinitionsAlone.quickGet(base.index());
        return !says.isLost && !says.isRecord && !isRedefined && says == whatLayoutSays(base.index());
    });
    if (!layoutsLikeBase.isEmpty()) {
        const std::array sets { &std::as_const(hasKnownAccessor), &m_flow.fieldsThatMayBeEmpty, &m_flow.fieldsFilledAfterConstructions, &m_flow.fieldsNotOfBirth, &m_flow.fieldsDeletedByName, &std::as_const(m_fieldsReadInFrontOfDefinition), &std::as_const(m_fieldsGivenValues) };
        std::array<std::optional<SetBitsInOrderForLinkLayouts>, sets.size()> noted;
        forEachIndexInParallel(m_flow.loopsOfReading(), static_cast<unsigned>(sets.size()), [&](unsigned set) {
            noted[set].emplace(*sets[set]);
        });
        Vector<uint32_t> placesTakingCells = findIndicesInParallel(m_flow.loopsOfReading(), static_cast<unsigned>(layoutsLikeBase.size()), [&](unsigned place) {
            const Layout& layout = m_program.layouts[layoutsLikeBase[place]];
            LayoutID base = baseWhoseFieldListIsCopiedWhole(m_program, LayoutID(layoutsLikeBase[place]));
            size_t first = cellOf({ LayoutID(layoutsLikeBase[place]), layout.ownFieldCount });
            return std::ranges::all_of(noted, [&](const std::optional<SetBitsInOrderForLinkLayouts>& set) {
                return set->holdsTheSameIn(first, cellOf({ base, 0 }), m_program.at(base).fields.size());
            });
        });
        for (uint32_t place : placesTakingCells) {
            takesCellsOfBase.quickSet(layoutsLikeBase[place]);
            m_tables.layouts[layoutsLikeBase[place]].takesCellsOfBase = true;
        }
    }
    uint64_t numberOfCellsWrittenOneByOne = 0;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!takesCellsOfBase.quickGet(index))
            numberOfCellsWrittenOneByOne += m_program.layouts[index].fields.size() - m_program.layouts[index].ownFieldCount;
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
            if (!(arrivesAtDeclaration[declaration] & isWhatMakersLay) || !fieldAt(m_program, reference).declared.isExpected())
                cell.proven = narrowerOf(cell.proven, fieldAt(m_program, reference).declared);
            cell.isTestedByValue = fieldAt(m_program, reference).key && m_keysOfTestedFields.get(fieldAt(m_program, reference).key.index());
            if (uint32_t unit = m_unitsListedForFields[cellOf(reference)]; unit && unit != otherThanOneUnit && m_flow.reading() == WhatUnseenCodeDoes::Anything && fieldAt(m_program, reference).key && (cell.isTestedByValue || WrittenUnit::fromBits(unit).kind() != WrittenUnit::Kind::Text) && cellOf(reference) == declaration && cell.proven && !cell.isWrittenAfterConstruction && m_program.layouts[index].kind == LayoutKind::Record) {
                TypeID written = typeOfWrittenUnit(WrittenUnit::fromBits(unit));
                if (types().isSubset(written, cell.proven))
                    cell.proven = written;
            }
            const Field& record = fieldAt(m_program, reference);
            bool liesBesideAccessors = keepsWhatLiesBesideAccessors && !hasKnownAccessor.quickGet(cellOf(reference)) && !hasKnownAccessor.quickGet(declaration);
            cell.liesBesideWhatDescriptorsDefine = m_isRedefinedByWrittenDefinitionsAlone.quickGet(index) && record.form.isEmpty() && !hasKnownAccessor.quickGet(cellOf(reference)) && !hasKnownAccessor.quickGet(declaration)
                && !m_formsLaidInListedFields.contains(cellOf(reference)) && !m_formsLaidInListedFields.contains(declaration);
            liesBesideAccessors |= cell.liesBesideWhatDescriptorsDefine && everyConstructionIsSeen && !m_tables.layouts[index].possibleChanges.contains(LayoutChange::LosesProperty);
            cell.isDeletedByName = m_flow.fieldsDeletedByName.get(cellOf(reference)) || m_flow.fieldsDeletedByName.get(declaration);
            cell.isFilled = ((keepsWhatConstructionsFill && !cell.isDeletedByName) || liesBesideAccessors || (everyConstructionIsSeen && isPrivateNameInLink(m_program, record.key))) && !m_flow.fieldsGivenAccessorsByDescriptorsInSight.get(cellOf(reference)) && record.key && record.form.isEmpty() && !m_flow.fieldsThatMayBeEmpty.get(cellOf(reference))
                && (arrivesAtDeclaration[declaration] & somethingArrives) && !(m_flow.typeAt(m_flow.field(reference)).primitives & Primitive::Absent);
            bool parsersMayLeaveItOut = m_isMadeByParsers.quickGet(index) && !record.flags.contains(FieldFlags::IsRequired);
            cell.isOfBirth = constructionsAreFollowed && isAllocated && record.key && record.form.isEmpty() && !parsersMayLeaveItOut && !record.flags.contains(FieldFlags::MayBeDefinedAbove) && !m_flow.fieldsNotOfBirth.get(cellOf(reference));
            cell.isKeptFromComparisons = isPrivateNameInLink(m_program, record.key) && !m_flow.isInspectedByComparisonOfPrivateFields(m_flow.field(declarationOf(m_program, reference)));
            cell.isFilledAtEveryRead = cell.isFilled && !m_fieldsReadInFrontOfDefinition.quickGet(cellOf(reference))
                && !(isLost && m_keysReadInFrontOfDefinitionThroughLost.contains(record.key.bits()));
            cell.isFilled &= !m_flow.fieldsFilledAfterConstructions.get(cellOf(reference));
            cell.isLeftUndefinedByMakers = isGivenUndefinedByMakers && m_fieldsGivenUndefinedByMakers.quickGet(declaration) && !m_fieldsFilledByConstructors.quickGet(declaration);
            cell.isGivenUndefinedOrNullInSight = m_fieldsGivenUndefinedOrNullLate.quickGet(declaration) || (m_fieldsGivenUndefinedOrNullByMakers.quickGet(declaration) && !m_fieldsGivenValuesByMakers.quickGet(declaration));
            cell.isGivenByNoMaker = isRecordThatIsMade && !m_fieldsGivenUndefinedByMakers.quickGet(declaration) && !m_fieldsFilledByConstructors.quickGet(declaration)
                && !hasKnownAccessor.quickGet(cellOf(reference)) && !hasKnownAccessor.quickGet(declaration);
            bool isRedefinedAsValue = m_fieldsRedefinedWithAttributes.quickGet(cellOf(reference)) || m_fieldsRedefinedWithAttributes.quickGet(declaration);
            cell.isFilledByAccessorsAlone = hasKnownAccessor.quickGet(cellOf(reference)) && canHoldAccessors(m_program, LayoutID(index)) && record.form.isEmpty()
                && !m_fieldsGivenValues.quickGet(cellOf(reference)) && !m_fieldsGivenValues.quickGet(declaration);
            bool isGivenValuesByDefinitionsAlone = isRecord && !isLost && (m_fieldsInWhichDefinitionsLayForms.quickGet(cellOf(reference)) || m_fieldsInWhichDefinitionsLayForms.quickGet(declaration)) && record.form.isEmpty() && !hasKnownAccessor.quickGet(cellOf(reference)) && !hasKnownAccessor.quickGet(declaration) && cell.everyWriterIsKnown && !cell.isOfBirth
                && m_flow.fieldsNotOfBirth.get(cellOf(reference)) && !m_fieldsWrittenWithoutDefinition.get(cellOf(reference)) && !m_fieldsWrittenWithoutDefinition.get(declaration)
                && !m_fieldsInWhichDescriptorsMayLayAccessors.quickGet(cellOf(reference)) && !m_fieldsInWhichDescriptorsMayLayAccessors.quickGet(declaration);
            if (uint32_t forms = cell.isFilledByAccessorsAlone || isGivenValuesByDefinitionsAlone ? m_formsLaidInListedFields.get(cellOf(reference)) | m_formsLaidInListedFields.get(declaration) : 0; hasOneBitSet(forms) && forms != formNobodyKnows) {
                OptionSet<FieldForm> created = OptionSet<FieldForm>::fromRaw(static_cast<uint8_t>(WTF::ctz(forms)));
                if (cell.isFilledByAccessorsAlone || !created.contains(FieldForm::IsAccessor))
                    cell.formThatDefinitionsCreate = created;
            }
            bool hasOnlyKnownDefinitions = everyConstructionIsSeen && m_isRedefinedByWrittenDefinitionsAlone.quickGet(index) && !m_tables.layouts[index].possibleChanges.contains(LayoutChange::LosesProperty)
                && cell.formThatDefinitionsCreate.contains(FieldForm::IsAccessor) && !m_flow.fieldsWithUnseenAccessors.get(cellOf(reference)) && !m_flow.fieldsWithUnseenAccessors.get(declaration);
            cell.holdsAccessorFromBirth = cell.isFilledByAccessorsAlone && !isRedefinedAsValue && (keepsWhatLiesBesideAccessors || hasOnlyKnownDefinitions) && m_flow.reading() == WhatUnseenCodeDoes::Anything && isAllocated
                && !m_flow.fieldsThatMayBeEmpty.get(cellOf(reference)) && !m_fieldsReadInFrontOfDefinition.quickGet(cellOf(reference))
                && !(isLost && (m_keysReadInFrontOfDefinitionThroughLost.contains(record.key.bits()) || isGivenValueThroughLost(LayoutID(index), record.key)));
            cell.keepsAccessorOnceLaid = cell.isFilledByAccessorsAlone && !cell.holdsAccessorFromBirth && !isRedefinedAsValue && !isLost && !m_fieldsWrittenWithoutDefinition.get(cellOf(reference)) && !m_fieldsWrittenWithoutDefinition.get(declaration) && cell.everyWriterIsKnown && cell.proven && m_flow.reading() == WhatUnseenCodeDoes::Anything
                && m_isRedefinedByWrittenDefinitionsAlone.quickGet(index) && cell.formThatDefinitionsCreate.containsAll({ FieldForm::IsAccessor, FieldForm::IsNotConfigurable });
            isOfProgram &= isOfProgramInLink(cell.proven);
        }
        return isOfProgram;
    });

    uint64_t numberFilledBesideDeletions = 0;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (m_tables.layouts[index].losesNamedFieldsAlone)
            numberFilledBesideDeletions += std::ranges::count_if(m_tables.fields.span().subspan(m_tables.firstFieldOfLayout[index], m_tables.firstFieldOfLayout[index + 1] - m_tables.firstFieldOfLayout[index]), &LinkedCell::isFilled);
    }
    m_flow.build.statistics().count(StatisticID::FieldsFilledBesideDeletions, numberFilledBesideDeletions);

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
    m_tables.classesInVariables = m_flow.classesInVariables;
    m_tables.methodsOnPrototypes = m_flow.methodsOnPrototypes;
    writeOnAllThreads(static_cast<unsigned>(m_tables.variables.size()), [&](unsigned index) {
        LinkedCell& cell = m_tables.variables[index];
        cell.everyWriterIsKnown = !m_variablesSharedWithUnseenCode.quickGet(index) && !m_flow.escapedVariables.get(index);
        cell.isWrittenAfterConstruction = !cell.everyWriterIsKnown || m_variablesWrittenLate.quickGet(index);
        cell.isStoredInto = !cell.everyWriterIsKnown || m_variablesStoredInto.quickGet(index);
        cell.isFilled = cell.isFilledAtEveryRead = m_flow.variablesStoredFirst.get(index);
        const DeclaredType& declared = m_program.variables[index].declared;
        cell.proven = typeEstablishedOf(declared);
        bool saysWhatWasStored = m_flow.reading() != WhatUnseenCodeDoes::Anything && (m_program.variables[index].owner || !m_program.variables[index].flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun }));
        if (!cell.everyWriterIsKnown && !saysWhatWasStored)
            return isOfProgramInLink(cell.proven);
        cell.proven = narrowerOf(claimedTypeAt(m_flow.variable(VariableID(index)), Numbers::AsVariableHolds), declared);
        cell.proven = namingSymbolAt(m_flow.variable(VariableID(index)), cell.proven);
        if (cell.everyWriterIsKnown && cell.isFilled && !cell.isWrittenAfterConstruction && cell.proven) {
            auto written = std::ranges::equal_range(m_flow.unitsInVariables, VariableID(index), { }, &SummaryUnitInVariable::variable);
            if (TypeID unit = written.size() == 1 ? typeOfWrittenUnit(WrittenUnit::fromBits(written.front().unit)) : TypeID(); unit && types().isSubset(unit, cell.proven))
                cell.proven = unit;
        }
        const LinkType& arrived = m_flow.typeAt(m_flow.variable(VariableID(index)));
        if (m_flow.reading() != WhatUnseenCodeDoes::Anything || arrived.classes != LinkType::Classes::Site || arrived.objects != LinkType::Objects::None || arrived.functions != LinkType::Functions::None
            || arrived.hasUnknownObjects || arrived.holdsLostBuiltins || arrived.builtin || m_flow.hasEscaped(arrived.site))
            return isOfProgramInLink(cell.proven);
        cell.staysAsItsClassMakesIt = std::ranges::all_of(std::array { SlotOfContents::underUnknownNames(), SlotOfContents::noLongerAsMade(), SlotOfContents::mayHaveNumberNames() }, [&](SlotOfContents slot) {
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
    for (LinkedMember& member : m_tables.members) {
        if (member.replacement) {
            member.isAssignedToClass = member.side == ClassSide::Prototype && isNameAssignedToClass(member.classLayout, member.key);
            m_tables.replacedMethods.append({ member.replacement, member.written, member.key });
            m_tables.functionsReplacingMethods.set(member.replacement.index());
        }
        m_tables.someAccessorIsReplaced |= member.isReplaced && member.kind != MethodKind::Method;
    }
    std::ranges::sort(m_tables.replacedMethods);
    m_tables.replacedMethods.shrink(std::ranges::unique(m_tables.replacedMethods).begin() - m_tables.replacedMethods.begin());
    m_tables.indexMembersByKey(m_program, m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls ? LinkTables::AskedByKey::ByLongRowsAlone : LinkTables::AskedByKey::ByWalkToo);

    for (size_t place = 0; place < m_methodsByDecorators.size(); ++place) {
        const MethodByDecorator& outer = m_methodsByDecorators[place];
        const MethodByDecorator* below = place ? &m_methodsByDecorators[place - 1] : nullptr;
        FunctionID inner;
        if (below && below->writer == outer.writer && below->site == outer.site)
            inner = below->method;
        else {
            for (uint32_t declared : m_tables.membersUnder(outer.classLayout, ClassSide::Prototype, outer.key))
                inner = m_tables.members[declared].written;
        }
        if (!outer.method || !inner || outer.method == inner)
            continue;
        m_tables.replacedMethods.append({ outer.method, inner, outer.key });
        m_tables.functionsReplacingMethods.set(outer.method.index());
    }
    std::ranges::sort(m_tables.replacedMethods);
    m_tables.replacedMethods.shrink(std::ranges::unique(m_tables.replacedMethods).begin() - m_tables.replacedMethods.begin());
    for (auto [laid, taken] : m_namesLaidOutOfMaps) {
        auto name = m_namesWrittenOnPrototypes.find(laid);
        auto accessor = m_accessorsWrittenInMaps.find(taken);
        if (accessor != m_accessorsWrittenInMaps.end() && accessor->value.isWrittenOnce && !name->value.isWrittenAgain)
            name->value = accessor->value;
    }

    findCopiedPrototypeMemberFunctions();
    noteAssignmentsByHelpers();
    for (const OwnNameAssignment& assignment : m_assignmentsOfOwnNames) {
        for (const LinkedMember& member : std::ranges::equal_range(m_tables.members, std::tuple { assignment.classLayout, ClassSide::Prototype }, { }, [](const LinkedMember& member) { return std::tuple { member.classLayout, member.side }; })) {
            if (member.key && member.kind == MethodKind::Method)
                noteLastAssignment(assignment.writer, assignment.site, assignment.classLayout, member.key, assignment.method);
        }
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
        if (m_flow.hasOnlyPrimitives(name) && !hasEscaped(name.layout.index()))
            linked.primitives = claimedTypeAt(name.stored, Numbers::ByKindAlone);
        if (m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls && !linked.expected && !name.mayHoldAccessor())
            linked.seen = claimedTypeAt(name.storedInSight);
        linked.mayBeReadOnly = name.isDefinedInSight;
        bool isAccessorAlone = name.key && name.isAccessorAsWritten && !name.isStoredInto && !name.isDefinedByDescriptor && !name.isDefinedUnderSomeSymbol && !hasEscaped(name.layout.index());
        if (isAccessorAlone) {
            auto halfAt = [&](FlowNodeID node, FunctionID& half) {
                LinkType oneHalf;
                oneHalf.functions = LinkType::Functions::Closure;
                oneHalf.function = m_flow.typeAt(node).function;
                if (m_flow.typeAt(node) == oneHalf)
                    half = oneHalf.function;
                return half || m_flow.typeAt(node) == m_flow.fromTypeID(TypeTable::undefined);
            };
            if (!halfAt(name.getter, linked.getter) || !halfAt(name.setter, linked.setter))
                linked.getter = linked.setter = { };
        }
        if (auto written = m_namesWrittenOnPrototypes.find(static_cast<uint64_t>(name.layout.index()) << 32 | name.key.bits()); name.key && written != m_namesWrittenOnPrototypes.end()) {
            if (written->value.isWrittenOnce && !name.isStoredInto) {
                linked.expectedGetter = written->value.getter;
                linked.expectedSetter = written->value.setter;
            }
            if (written->value.isCopied && !linked.expected && !name.mayHoldAccessor())
                linked.expected = written->value.method;
            if (written->value.isWrittenAgain && !written->value.hasSeveralWriters && !linked.expected && !name.mayHoldAccessor())
                linked.expected = written->value.lastMethod;
            if (written->value.isWrittenAgain && linked.expected && linked.expected == written->value.lastMethod)
                m_flow.build.statistics().count(StatisticID::RewrittenMethodsSpeculated);
            if (Vector<std::pair<ValueID, FunctionID>>& methods = written->value.methods; !methods.isEmpty()) {
                std::ranges::sort(methods, { }, [](const auto& method) { return method.first.index(); });
                for (size_t place = 1; place < methods.size(); ++place) {
                    if (methods[place].second == methods[place - 1].second)
                        continue;
                    m_tables.replacedMethods.append({ methods[place].second, methods[place - 1].second, name.key });
                    m_tables.functionsReplacingMethods.set(methods[place].second.index());
                    m_flow.build.statistics().count(StatisticID::InnerMethodsSpeculated);
                }
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
    std::ranges::sort(m_tables.replacedMethods);
    m_tables.replacedMethods.shrink(std::ranges::unique(m_tables.replacedMethods).begin() - m_tables.replacedMethods.begin());
    for (const auto& [word, last] : m_lastAssignments)
        m_tables.lastAssignments.append({ LayoutID(static_cast<uint32_t>(word >> 32)), PropertyKeyID::fromBits(static_cast<uint32_t>(word)), last.method });
    std::ranges::sort(m_tables.lastAssignments, { }, &LinkedAssignment::place);
    Vector<ReplacedMethod> replacedByAssignments;
    for (const LinkedAssignment& assignment : m_tables.lastAssignments) {
        FunctionID written;
        for (LayoutID current = assignment.layout; assignment.method && current && !written; current = m_program.at(current).base) {
            for (uint32_t place : m_tables.membersUnder(current, ClassSide::Prototype, assignment.key)) {
                if (m_tables.members[place].kind == MethodKind::Method)
                    written = m_tables.members[place].written;
            }
        }
        bool isListed = std::ranges::contains(std::ranges::equal_range(m_tables.replacedMethods, assignment.method, { }, &ReplacedMethod::replacement), assignment.key, &ReplacedMethod::key);
        if (!written || written == assignment.method || isListed)
            continue;
        replacedByAssignments.append({ assignment.method, written, assignment.key });
        m_tables.functionsReplacingMethods.set(assignment.method.index());
    }
    m_tables.replacedMethods.appendVector(replacedByAssignments);
    std::ranges::sort(m_tables.replacedMethods);
    m_tables.replacedMethods.shrink(std::ranges::unique(m_tables.replacedMethods).begin() - m_tables.replacedMethods.begin());
    for (const auto& [word, written] : m_accessorsAssignedToClasses) {
        if (written.isWrittenOnce)
            m_tables.accessorsAssignedToClasses.append(LinkedNameOnPrototype { LayoutID(static_cast<uint32_t>(word >> 32)), PropertyKeyID::fromBits(static_cast<uint32_t>(word)), { }, { }, written.getter, written.setter });
    }
    std::ranges::sort(m_tables.accessorsAssignedToClasses, { }, [](const LinkedNameOnPrototype& name) {
        return std::pair { name.layout, name.key };
    });
    for (const LinkedNameOnPrototype& name : m_tables.accessorsAssignedToClasses) {
        for (uint32_t place : m_tables.membersUnder(name.layout, ClassSide::Prototype, name.key)) {
            const LinkedMember& declared = m_tables.members[place];
            FunctionID replacement = declared.kind == MethodKind::Getter ? name.expectedGetter : declared.kind == MethodKind::Setter ? name.expectedSetter : FunctionID();
            if (!replacement || !declared.written || replacement == declared.written)
                continue;
            m_tables.replacedMethods.append({ replacement, declared.written, name.key });
            m_tables.functionsReplacingMethods.set(replacement.index());
        }
    }
    std::ranges::sort(m_tables.replacedMethods);
    m_tables.replacedMethods.shrink(std::ranges::unique(m_tables.replacedMethods).begin() - m_tables.replacedMethods.begin());
    std::ranges::sort(m_tables.functionsWrittenRightOfNamesOnPrototypes);
    m_tables.functionsWrittenRightOfNamesOnPrototypes.shrink(std::ranges::unique(m_tables.functionsWrittenRightOfNamesOnPrototypes).begin() - m_tables.functionsWrittenRightOfNamesOnPrototypes.begin());
    auto placeOfName = [](const LinkedNameOnPrototype& name) {
        return std::pair { name.layout, name.key };
    };
    std::ranges::sort(m_tables.namesOnPrototypes, { }, placeOfName);
    for (const LinkedMethodStoredFirst& laid : m_flow.methodsStoredFirst) {
        LayoutID layout = m_program.at(laid.holder).constructedClass;
        if (!layout || !m_program.at(layout).has(LayoutFlags::IsConstructorFunction) || m_program.at(layout).constructor != laid.holder)
            continue;
        for (LinkedNameOnPrototype& name : std::ranges::equal_range(m_tables.namesOnPrototypes, std::pair { layout, laid.key }, { }, placeOfName))
            name.isLaidFirst |= name.function == laid.method;
    }
    FunctionID constructor;
    bool findsPlainProperties = false;
    for (const SummaryStoreOnPrototype& laid : m_flow.storesBeforeInstances) {
        using Kind = SummaryStoreOnPrototype::Kind;
        LayoutID layout = laid.method ? m_flow.layoutOfConstructorFunction(laid.function) : LayoutID();
        if (!layout || laid.kind == Kind::ParentBeforeInstances || laid.kind == Kind::FindsPlainProperty)
            continue;
        if (std::exchange(constructor, laid.function) != laid.function)
            findsPlainProperties = m_flow.findsPlainProperties(constructor);
        if (!findsPlainProperties)
            continue;
        for (LinkedNameOnPrototype& name : std::ranges::equal_range(m_tables.namesOnPrototypes, std::pair { layout, laid.key }, { }, placeOfName))
            name.isLaidFirst |= laid.method == (laid.kind == Kind::GetterBeforeInstances ? name.getter : laid.kind == Kind::SetterBeforeInstances ? name.setter : name.function);
    }
}

void WriterOfLinkLayouts::lookAtSummaries()
{
    forEachLayoutMadeAtNoNodeInLinkLayouts(m_program, [&](LayoutID layout) {
        m_isMadeAtNoNode.quickSet(layout.index());
    });
    std::span<const EvalScope> scopesOfEvals = m_flow.leavesOutDirectEvals ? std::span<const EvalScope>() : m_program.evalScopes.span();
    forEachVariableSharedWithUnseenCodeInLinkLayouts(m_program, scopesOfEvals, [&](VariableID variable) {
        m_variablesSharedWithUnseenCode.quickSet(variable.index());
    });
    forEachFieldSharedWithEvaluatedTextInLinkLayouts(scopesOfEvals, [&](FieldReference field) {
        m_fieldsWithUnseenWriters.quickSet(cellOf(field));
    });
    for (FieldReference field : m_program.exportedFields()) {
        if (fieldAt(m_program, field).flags.contains(FieldFlags::IsReplaceable))
            m_fieldsWithUnseenWriters.quickSet(cellOf(field));
    }
    for (const Program::InstanceField& filled : m_program.instanceFields())
        m_fieldsWithUnseenWriters.quickSet(cellOf(filled.field));
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
    if (m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls) {
        m_classesThatMakersLayInFields.fill(0, m_tables.fields.size());
        noteTypedArraysStoredUnderNames();
    }
    m_fieldFactsFromEdges.whatMakersLayInFields.fill(0, m_classesThatMakersLayInFields.size());
    if (m_flow.reading() == WhatUnseenCodeDoes::Anything) {
        for (const LinkFlow::NameOnClosures& name : m_flow.namesOnClosures())
            m_keysOfFunctionNames.set(name.key.index());
    }
    Vector<Vector<AccessInSummary>> chunksOfAccesses = findRelevantAccesses();
    runLanes(m_flow.loopsOfReading(), {
        [&] {
            size_t nextChunk = 0;
            std::span<const AccessInSummary> accessesLeft;
            for (size_t index : m_flow.reached) {
                while (accessesLeft.empty() && nextChunk < chunksOfAccesses.size())
                    accessesLeft = chunksOfAccesses[nextChunk++].span();
                FunctionID id(index);
                const ThinSummary& summary = m_flow.summaries.at(id);
                lookAtAccesses(id, summary, accessesLeft);
                lookAtChanges(id, summary);
                lookAtDefinersOfAccessors(id, summary);
                lookAtMembers(summary);
            }
            noteHelperSteps();
        },
        [&] {
            for (size_t index : m_flow.reached)
                lookAtEdges(FunctionID(index), m_flow.summaries.at(FunctionID(index)));
        },
        [&] { findFunctionNamesRead(); },
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
    noteLostInstanceFields();
    carryClassesAlongCopiesBetweenFields();
    propagateNumberDependencies();
    m_tables.typedArraysHeldUnderNames.removeIf([&](const auto& stored) {
        return !isTypedArrayAmongMadeClasses(stored.value);
    });
    m_tables.classesStoredUnderNames.removeIf([](const auto& stored) {
        return stored.value == severalClasses || (arePrimitivesAmongWhatMakersLay(stored.value) && !areNumbersAmongWhatMakersLay(stored.value));
    });
    for (auto& stored : m_tables.classesStoredUnderNames) {
        if (areNumbersAmongWhatMakersLay(stored.value))
            stored.value = areIntegersAmongWhatMakersLay(stored.value) ? LinkTables::integersStoredUnderName : LinkTables::numbersStoredUnderName;
    }
    noteClassesPassedToParameters();
    noteFieldsReadAsStrings();
    noteNamesStoredThroughWhatFieldsAreExpectedToHold();
    noteNamesStoredByMethodsOfLostClasses();
    noteNamesReadFromComputedLiterals();
    noteLiteralsFilledThroughProperties();
    noteFormsOfNamesDefinedThroughWrittenTypesOfParameters();
    m_fieldFactsFromEdges = { };
    m_whatIsNotedOfEveryField = { };
}

void WriterOfLinkLayouts::writeWhatMakesNoType(BitVector&& functionsWhoseCallersAreAllKnown, LateFills lateFills)
{
    m_functionsWhoseCallersAreAllKnown = WTF::move(functionsWhoseCallersAreAllKnown);
    m_lateFills = lateFills;
    noteNamesThatMakersGive();
    runLanes(m_flow.loopsOfReading(), {
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
    writeUnitsOfStaticFields();
    writePrimitivesOfStaticFields();
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

void writeLayoutsAndCellsAheadOfTypes(const LinkFlow& flow, BitVector&& functionsWhoseCallersAreAllKnown, LateFills lateFills)
{
    flow.writerOfLayoutsOfReading()->writeWhatMakesNoType(WTF::move(functionsWhoseCallersAreAllKnown), lateFills);
}

void writeLayoutsAndCells(const LinkFlow& flow, TypeArena& types)
{
    flow.writerOfLayoutsOfReading()->write(types);
    flow.writerOfLayoutsOfReading() = nullptr;
}

} } } // namespace JSC::AOT::Typed

#include "AOTLinkBirthsInlines.h"
#include "AOTLinkLateFillsInlines.h"
#include "AOTLinkFollowersOfMakersInlines.h"
#include "AOTRowsOfLayouts.h"

#endif // ENABLE(AOT)
