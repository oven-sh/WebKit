/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkLayouts.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTLinkTypesOnAllThreads.h"
#include "AOTLinkTypesTurnedOnce.h"
#include "AOTLinkUnions.h"
#include "AOTLinkStrings.h"
#include "AOTNameClass.h"
#include "AOTRowsOfLayouts.h"
#include "AOTUnionTaggedMakers.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

// `anything` stands for what has escaped only. So a value that goes where no node follows it (a property that the layout does not list, `this` of an accessor,
// an object above which something else was put) flows into unseen().

// ---- What no summary shows

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

// A variable may be handed over more than once.
template<typename Functor>
static void forEachVariableSharedWithUnseenCodeInLinkLayouts(const Program& program, const Functor& functor)
{
    for (uint32_t index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        const Module& module = program.at(variable.module);
        // A static field is a property of the class object. Only the run time stores into its variable, whatever is stored under the name, and it empties it (object/AOTStaticFields.h).
        if (variable.owner || variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun })
            || !module.body || !variablesAreWrittenOnlyByBodiesInLinkLayouts(module.kind))
            functor(VariableID(index));
    }

    // The run time fills these.
    for (const Module& module : program.modules) {
        if (module.exportsVariable)
            functor(module.exportsVariable);
        for (const SyntheticExport& synthetic : module.syntheticExports) {
            if (synthetic.variable)
                functor(synthetic.variable);
        }
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

// What a data module exports, which the loader makes, and Object.create(). A layout may be handed over more than once.
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

    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].has(LayoutFlags::IsOfCreatedObject))
            functor(LayoutID(index));
    }
}

// Of any other layout native code makes objects too.
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

// ---- The observer of changes

static OptionSet<LayoutChange> changesMadeByInLinkLayouts(SummaryChange::Kind kind)
{
    switch (kind) {
    case SummaryChange::Kind::Freezes:
        return { LayoutChange::IsFrozen };
    case SummaryChange::Kind::Deletes:
        return { LayoutChange::LosesProperty };
    // A descriptor may take `writable` away, which refuses stores as freezing does.
    case SummaryChange::Kind::DefinesByDescriptor:
        return { LayoutChange::GetsAccessor, LayoutChange::GainsProperty, LayoutChange::IsFrozen };
    case SummaryChange::Kind::ChangesPrototype:
        return { LayoutChange::ChangesPrototype };
    case SummaryChange::Kind::BecomesPrototype:
        return { };
    // A key that nobody knows may find the setter of the prototype link.
    case SummaryChange::Kind::CopiesInto:
        return { LayoutChange::GainsProperty, LayoutChange::ChangesPrototype };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct ObservedChangeInLinkLayouts {
    FlowNodeID subjectNode;
    // Null: the change names none.
    FlowNodeID otherNode;
    // Copies: asking for an aggregate may make a node, which moves the types.
    LinkType subject;
    LinkType other;
    PropertyKeyID key;
    SummaryChange::Lays lays { SummaryChange::Lays::WhatDescriptorSays };
    bool descriptorIsInSight { false };
    bool laysValueInSight { false };
    FlowNodeID valueInSight;
    // Null: the descriptor does not name it.
    FlowNodeID getterInSight;
    FlowNodeID setterInSight;
    // SummaryChange::definesWhatItCopies.
    bool definesWhatItCopies { false };
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
    type.holdsUnseen = true;
    return type;
}

// The layouts whose instances the type names one by one. The members of Lost are not among them: they are still being added.
template<typename Functor>
static void forEachLayoutNamedInLinkLayouts(const LinkFlow& flow, const LinkType& type, const Functor& functor)
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
    case LinkType::Objects::Family:
        flow.program.at(flow.families.layoutsOf(type.family)).forEach(functor);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The classes whose prototype objects the type names.
template<typename Functor>
static void forEachClassOfPrototypeNamedInLinkLayouts(const LinkFlow& flow, const LinkType& type, const Functor& functor)
{
    switch (type.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Layout:
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

// The classes whose class objects the type names. Those of the lost closures are not among them: noteLostClassObjects().
template<typename Functor>
static void forEachClassOfClassObjectNamedInLinkLayouts(const LinkFlow& flow, const LinkType& type, const Functor& functor)
{
    switch (type.functions) {
    case LinkType::Functions::None:
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

// A property under an index is an element, and a row may have laid something under a name. What a map or a set holds is no property.
template<typename Functor>
static void forEachElementsNodeInLinkLayouts(LinkFlow& flow, const LinkType& type, const Functor& functor)
{
    if (type.classes == LinkType::Classes::None)
        return;
    functor(flow.contentsOf(type, ContentsSlot::Element));
    functor(flow.contentsOf(type, SlotOfContents::underAnyName()));
}

// No prototype object, no function, no built-in: what is done to those changes what lookups find on other objects.
static bool namesOnlyInstancesInLinkLayouts(const LinkType& type)
{
    return type.objects != LinkType::Objects::Prototype && type.objects != LinkType::Objects::Prototypes && type.functions == LinkType::Functions::None && !type.builtin;
}

// Null: the key names no field there. A null key: every field.
static FlowNodeID nodeStoredUnderInLinkLayouts(LinkFlow& flow, const LinkType& type, PropertyKeyID key)
{
    switch (type.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        return { };
    case LinkType::Objects::Layout:
        if (!key)
            return flow.anyFieldStoredThrough(type.objects, type.layout, type.family);
        if (std::optional<FieldReference> found = findField(flow.program, type.layout, key))
            return flow.field(*found);
        return { };
    case LinkType::Objects::Family:
        return key ? flow.fieldStoredThrough(type.family, key) : flow.anyFieldStoredThrough(type.objects, type.layout, type.family);
    case LinkType::Objects::Lost:
        return key ? flow.fieldStoredThroughLost(type.layout, key) : flow.anyFieldStoredThrough(type.objects, type.layout, type.family);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void addChangesInLinkLayouts(LinkFlow& flow, LayoutID layout, OptionSet<LayoutChange> changes)
{
    flow.possibleChanges[layout.index()].add(changes);
}

// What is done through a lost value is gathered behind the flow, when every member of Lost is known: WriterOfLinkLayouts::lookAtChanges().
static void addChangesToLayoutsNamedInLinkLayouts(LinkFlow& flow, const LinkType& type, OptionSet<LayoutChange> changes)
{
    forEachLayoutNamedInLinkLayouts(flow, type, [&](LayoutID layout) {
        addChangesInLinkLayouts(flow, layout, changes);
    });
}

// What lies above the instances of a class lies above those of the classes below it.
static void escapeClassInLinkLayouts(LinkFlow& flow, LayoutID classLayout)
{
    flow.escape(classLayout);
    for (LayoutID below : flow.program.layoutsExpectedBelow(classLayout))
        flow.escape(below);
}

// A node that holds a class object or a prototype object does not climb when the class escapes. Whoever has read what lies above either has taken foundBesideFieldsOf() of the class along, which holds anything from then on.
static void handToUnseenCodeInLinkLayouts(LinkFlow& flow, FlowNodeID node, const LinkType& type)
{
    if (node != flow.unseen())
        flow.addEdge(node, flow.unseen());
    auto letGo = [&](LayoutID classLayout) {
        escapeClassInLinkLayouts(flow, classLayout);
    };
    forEachClassOfPrototypeNamedInLinkLayouts(flow, type, letGo);
    forEachClassOfClassObjectNamedInLinkLayouts(flow, type, letGo);
}

// The engine runs an accessor with the object as `this`, at no call.
static bool listsAccessorInLinkLayouts(const Program& program, LayoutID layout)
{
    // `class A extends B {}` beside `class B extends A {}` compiles.
    size_t stepsLeft = program.layouts.size();
    for (LayoutID current = layout; current && stepsLeft; current = program.at(current).base, --stepsLeft) {
        for (const Field& field : program.at(current).fields) {
            if (field.form.contains(FieldForm::IsAccessor))
                return true;
        }
    }
    return false;
}

static void observeFreezeInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    OptionSet<LayoutChange> changes = changesMadeByInLinkLayouts(SummaryChange::Kind::Freezes);
    addChangesToLayoutsNamedInLinkLayouts(flow, change.subject, changes);
    // A name that is read only above an object refuses the store on the object.
    forEachClassOfPrototypeNamedInLinkLayouts(flow, change.subject, [&](LayoutID classLayout) {
        addChangesInLinkLayouts(flow, classLayout, changes);
        for (LayoutID below : flow.program.layoutsExpectedBelow(classLayout))
            addChangesInLinkLayouts(flow, below, changes);
    });
}

static void observeDeletionInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    if (!namesOnlyInstancesInLinkLayouts(change.subject))
        handToUnseenCodeInLinkLayouts(flow, change.subjectNode, change.subject);
    addChangesToLayoutsNamedInLinkLayouts(flow, change.subject, changesMadeByInLinkLayouts(SummaryChange::Kind::Deletes));

    if (FlowNodeID emptied = nodeStoredUnderInLinkLayouts(flow, change.subject, change.key)) {
        flow.arrive(emptied, flow.fromTypeID(typeOfPrimitives(Primitive::Undefined | Primitive::Absent)));
        // From then on a read goes on upward. The flow opens a layout that is flagged, and the flags of the lost ones come behind the flow.
        if (change.subject.objects == LinkType::Objects::Lost)
            flow.arrive(emptied, LinkType::anything());
    }
    if (!change.key || flow.program.isIndexLike(change.key)) {
        LinkType undefined = flow.fromTypeID(TypeTable::undefined);
        forEachElementsNodeInLinkLayouts(flow, change.subject, [&](FlowNodeID elements) {
            flow.arrive(elements, undefined);
        });
    }
    // What a row has laid under a name on an object of one of the engine's classes. From then on a read goes on upward.
    if (change.subject.classes != LinkType::Classes::None && (!change.key || !flow.program.isIndexLike(change.key)))
        handToUnseenCodeInLinkLayouts(flow, change.subjectNode, change.subject);
}

// Null: the type names anything else than the object that one constructor function was made with under `prototype`.
static LayoutID onlyPrototypeOfConstructorFunctionInLinkLayouts(const LinkFlow& flow, const LinkType& type)
{
    if (type.objects != LinkType::Objects::Prototype || !flow.isOfConstructorFunction(type.layout))
        return { };
    LinkType withoutPrimitives = type;
    withoutPrimitives.primitives = 0;
    return withoutPrimitives == typeOfPrototypeInLinkLayouts(type.layout) ? type.layout : LayoutID();
}

// The engine hands the object to the accessors as `this`, and they have escaped with the descriptor. A value under the name of a method hides the method.
// `{ get x() { .. } }` on an object of one layout that lists the name as a field. Whoever reads the field runs the getter with the object as `this` and gets what it returns. Whoever stores into it hands the setter what is stored.
// False: nobody can tell who runs what.
// THE EDGES ARE LAID FOR THE OBJECTS OF THE LAYOUT WHATEVER ELSE THE TWO TYPES HOLD, and then the answer is false: an allocation holds unseen objects too once its layout has escaped, and what is laid may not hang on whether the definition was looked at before that.
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
    if (!field || !fieldAt(program, *field).form.isEmpty())
        return false;
    bool namesNothingElse = change.subject == objectsOfLayout;
    if (change.other.isNothing())
        return namesNothingElse;
    if (change.other.functions != LinkType::Functions::Closure)
        return false;
    namesNothingElse &= change.other == typeOfClosuresInLinkLayouts(change.other.function);

    FunctionID id = change.other.function;
    const Function& function = program.at(id);
    bool isGetter = change.lays == SummaryChange::Lays::Getter;
    // Only of an accessor do the tables know that something other than a call enters it. `arguments` lists what is stored, at a node that all such lists share.
    if (function.kind != (isGetter ? FunctionKind::Getter : FunctionKind::Setter) || !function.hasBody() || function.isCoroutine() || function.results.size() != 1 || flow.summaries.at(id).has(SummaryTrait::UsesArguments))
        return false;

    flow.layoutsWithKnownAccessors.set(layout.index());
    flow.fieldsWithKnownAccessors.append(*field);
    flow.accessorsLaidInFields.append({ *field, id, isGetter });
    flow.reach(id);
    // Whoever reads or stores may catch it, and no edge leads there.
    flow.addEdge(flow.thrown(id), flow.unseen());
    FlowNodeID held = flow.field(*field);
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

// The engine runs a half with the object as `this`, and a setter with what is stored under the key: that escapes in observeDefinitionInLinkLayouts(), as the halves have in ScanOfBody::layDescriptorsInSight().
// False: something other than closures of one function, or undefined, lies under `get` or `set`, and nobody can tell what is run.
static bool handObjectToHalvesInSightInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    for (FlowNodeID half : { change.getterInSight, change.setterInSight }) {
        if (!half)
            continue;
        LinkType held = flow.typeAt(half);
        held.primitives = 0;
        if (held.isNothing())
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

// The subject is the object that one constructor function is made with and nothing else, or the closures of one function whose names are followed and nothing else.
static bool definitionInSightIsStoreInLinkLayouts(const LinkFlow& flow, const LinkType& subject, PropertyKeyID key)
{
    if (onlyPrototypeOfConstructorFunctionInLinkLayouts(flow, subject))
        return true;
    return subject.functions == LinkType::Functions::Closure && subject == typeOfClosuresInLinkLayouts(subject.function) && flow.followsNamesOnClosuresOf(subject.function, key);
}

static void observeDefinitionInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    // Anywhere else the value lies under the name where anybody may read it, as the scan has it of every other descriptor.
    bool isStore = change.laysValueInSight && change.key && definitionInSightIsStoreInLinkLayouts(flow, change.subject, change.key);
    if (change.valueInSight && !isStore && !change.subject.isNothing())
        flow.addEdge(change.valueInSight, flow.unseen());
    if (layKnownAccessorInLinkLayouts(flow, change))
        return;
    // SummaryChange::descriptorIsInSight: that one key of the object changes. Of a prototype object or a class object the halves are run with the objects below as `this`: there all is as it was.
    bool namesObjectsOfLayoutsAlone = (change.subject.objects == LinkType::Objects::Layout || change.subject.objects == LinkType::Objects::Family) && change.subject.functions == LinkType::Functions::None && change.subject.classes == LinkType::Classes::None && !change.subject.builtin;
    if (change.descriptorIsInSight && namesObjectsOfLayoutsAlone && handObjectToHalvesInSightInLinkLayouts(flow, change)) {
        // What lies under the key from now on the definition's own access has brought to the field of that key, listed or appended. No other name answers otherwise than before, so the layout is not opened: openLayoutsThatChanged().
        forEachLayoutNamedInLinkLayouts(flow, change.subject, [&](LayoutID layout) {
            flow.layoutsDefinedOnUnderWrittenKeys.set(layout.index());
            // A name that nobody else names has no access of its own. What lies under it lies beside the fields all the same: whoever lists what the object holds, or reads under a computed key, finds it.
            if (!change.key) {
                flow.addEdge(flow.unseen(), flow.storedBesideFieldsOf(layout));
                return;
            }
            // From then on the engine hands the setter whatever is stored under the key, at no call that is written. The setter has escaped, so what it is handed has too.
            if (change.setterInSight) {
                std::optional<FieldReference> field = findField(flow.program, layout, change.key);
                flow.addEdge(field ? flow.field(*field) : flow.appendedField(layout, change.key), flow.unseen());
            }
        });
        return;
    }
    // SummaryChange::laysValueInSight: the call keeps nothing of the descriptor, no half is laid, and the definition's own access has brought the value under the name: resolveOnPrototype(), resolveOnClosure().
    if (isStore)
        return;
    if (change.otherNode)
        handToUnseenCodeInLinkLayouts(flow, change.otherNode, change.other);
    if (LayoutID function = onlyPrototypeOfConstructorFunctionInLinkLayouts(flow, change.subject)) {
        flow.defineOnPrototypeOf(function, change.key);
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

// From here on a lookup on the object finds what nobody can list, and what it finds is called with the object as `this`.
// `Object.setPrototypeOf(G.prototype, F.prototype)` lays the link that the layouts are written with. Until it runs, the lookups from below find less than Link lets them find.
static void observeChangeOfPrototypeInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    // Below null a lookup does not come to Object.prototype, where Link lets every lookup end.
    bool mayPutNullAbove = change.other.primitives & Primitive::Null;
    if (change.otherNode && !mayPutNullAbove && onlyPrototypeOfConstructorFunctionInLinkLayouts(flow, change.subject) && (change.other.isNothing() || agreesWithLayoutsInLinkLayouts(flow, change.other, change.subject)))
        return;
    if (change.otherNode)
        handToUnseenCodeInLinkLayouts(flow, change.otherNode, change.other);
    handToUnseenCodeInLinkLayouts(flow, change.subjectNode, change.subject);
    addChangesToLayoutsNamedInLinkLayouts(flow, change.subject, changesMadeByInLinkLayouts(SummaryChange::Kind::ChangesPrototype));
    notePrototypesLaidAboveOtherObjects(flow, change.other);
    // A read at a hole goes on upward.
    LinkType anything = LinkType::anything();
    forEachElementsNodeInLinkLayouts(flow, change.subject, [&](FlowNodeID elements) {
        flow.arrive(elements, anything);
    });
}

// Whether the layouts say it already: the class object of the written base above a class object, its prototype object above a prototype object, the prototype object of a class above an instance of that class.
static bool agreesWithLayoutsInLinkLayouts(const LinkFlow& flow, const LinkType& above, const LinkType& below)
{
    const Program& program = flow.program;
    LinkType aboveWithoutPrimitives = above;
    aboveWithoutPrimitives.primitives = 0;

    // ResolveHeritage stands in front of the definition of a class and throws for every primitive but null. A variable holds `absent` before it holds the class.
    if (below.functions == LinkType::Functions::Closure && below == typeOfClosuresInLinkLayouts(below.function) && flow.classOfConstructor(below.function)) {
        if (above.primitives & Primitive::Null)
            return false;
        if (aboveWithoutPrimitives.isNothing())
            return true;
        LayoutID base = program.at(flow.classOfConstructor(below.function)).base;
        FunctionID constructorOfBase = base ? program.at(base).constructor : FunctionID();
        return constructorOfBase && aboveWithoutPrimitives == typeOfClosuresInLinkLayouts(constructorOfBase);
    }

    // The row of ResolveHeritage always lists null. Whether the heritage may be null is asked above, which lets go of the class.
    if (below.objects == LinkType::Objects::Prototype && below == typeOfPrototypeInLinkLayouts(below.layout)) {
        if (aboveWithoutPrimitives.isNothing())
            return true;
        LayoutID base = program.at(below.layout).base;
        return base && aboveWithoutPrimitives == typeOfPrototypeInLinkLayouts(base);
    }

    // For the prototype object of a class the run time makes an instance of that class only if the class is written at or below the one that the allocation names, else an ordinary object (ClassTable::instanceWithPrototype()).
    // The scan lays the layout that the allocation names and all that are written below it at the object, and no other layout arrives there. So the class is one of those if the object's type names it, and its layout lies there already.
    // In place of a primitive the run time takes a prototype object of its own.
    if (!namesOnlyInstancesInLinkLayouts(below) || below.classes != LinkType::Classes::None)
        return false;
    switch (above.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Layout:
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        return false;
    case LinkType::Objects::Prototype:
        if (above != typeOfPrototypeInLinkLayouts(above.layout))
            return false;
        if (below.objects == LinkType::Objects::Layout)
            return below.layout == above.layout;
        return below.objects == LinkType::Objects::Family && flow.families.nearestCommonFamily(below.family, above.layout) == below.family;
    case LinkType::Objects::Prototypes:
        return above == typeOfPrototypesInLinkLayouts(above.family) && below.objects == LinkType::Objects::Family && flow.families.nearestCommonFamily(below.family, above.family) == below.family;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Where the layouts do not say it, lookups on what lies below find what nobody can list, and what lies above is found by objects that Link does not know as its instances.
// The run time may then have made an ordinary object, or one of the layout under another prototype, and what the allocation lists lies in it.
static void observeNewHeirInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    if (!change.otherNode) {
        handToUnseenCodeInLinkLayouts(flow, change.subjectNode, change.subject);
        return;
    }
    // Nothing lies below yet.
    if (change.other.isNothing())
        return;

    if (agreesWithLayoutsInLinkLayouts(flow, change.subject, change.other))
        return;

    handToUnseenCodeInLinkLayouts(flow, change.subjectNode, change.subject);
    handToUnseenCodeInLinkLayouts(flow, change.otherNode, change.other);
    flow.arrive(change.otherNode, typeOfUnseenObjectsInLinkLayouts());
    notePrototypesLaidAboveOtherObjects(flow, change.subject);
}

// Where what a copy brings under a key that nobody knows lands in the objects of the program among its targets. Null: there are none.
static FlowNodeID nodeLaidUnderAnyKeyByCopyInLinkLayouts(LinkFlow& flow, const LinkType& subject, bool definesWhatItCopies)
{
    if (definesWhatItCopies && subject.objects == LinkType::Objects::Layout && flow.program.at(subject.layout).kind == LayoutKind::Record && !listsAccessorInLinkLayouts(flow.program, subject.layout))
        return flow.anyFieldDefinedOn(subject.layout);
    return nodeStoredUnderInLinkLayouts(flow, subject, { });
}

namespace {

// Of a copy that defines.
struct KeysOfCopyInLinkLayouts {
    // False: a key that a source lists finds no field in some layout of the target.
    bool findFieldsInTarget { true };
    // Beside the fields of a source, under names that nobody lists here.
    bool somethingLiesBesideFields { false };
    bool somethingSeenLiesBesideFields { false };
};

} // anonymous namespace

// `{ ...record }`: the objects of the program among the sources have own values that can be listed, and those among the targets are records. What lies in a field of a source goes under the same key: into the field that the target lists for it.
// What a source has beside its fields may land in any field of the target. Neither object is handed to anybody, and only what finds no field in the target is out of sight.
// IT GOES BY THE LADDER OF Objects ALONE, so that whatever order the types climb in, the same has been done in the end: nothing where either side has no object yet, this for a layout and a family, and all that observeCopyInLinkLayouts() does above those.
// The node under any key of a copy that defines says nothing of what the target gains: whether something lies beside the fields of a source is looked at here, and the copy is told when that changes (LinkFlow::copiesToldOfWhatLiesBesideFields).
static bool isListedOnBothSidesInLinkLayouts(LinkFlow& flow, const LinkType& subject, const LinkType& other)
{
    if (subject.objects == LinkType::Objects::None || other.objects == LinkType::Objects::None)
        return true;
    auto isListedKind = [](LinkType::Objects objects) {
        return objects == LinkType::Objects::Layout || objects == LinkType::Objects::Family;
    };
    if (!isListedKind(subject.objects) || !isListedKind(other.objects))
        return false;

    bool isListed = true;
    forEachLayoutNamedInLinkLayouts(flow, other, [&](LayoutID layout) {
        isListed &= !flow.ownValuesOf(layout).empty();
    });
    forEachLayoutNamedInLinkLayouts(flow, subject, [&](LayoutID layout) {
        isListed &= flow.program.at(layout).kind == LayoutKind::Record && !listsAccessorInLinkLayouts(flow.program, layout);
    });
    return isListed;
}

static bool copyFieldByFieldInLinkLayouts(LinkFlow& flow, const LinkType& subject, const LinkType& other, bool definesWhatItCopies, KeysOfCopyInLinkLayouts& keys)
{
    if (subject.objects == LinkType::Objects::None || other.objects == LinkType::Objects::None)
        return true;
    if (!isListedOnBothSidesInLinkLayouts(flow, subject, other))
        return false;

    FlowNodeID underAnyKey = nodeLaidUnderAnyKeyByCopyInLinkLayouts(flow, subject, definesWhatItCopies);
    forEachLayoutNamedInLinkLayouts(flow, other, [&](LayoutID layout) {
        for (const LinkFlow::OwnValue& value : flow.ownValuesOf(layout)) {
            if (definesWhatItCopies && !value.key) {
                const LinkType& held = flow.typeAt(value.node);
                keys.somethingLiesBesideFields |= !held.isNothing();
                keys.somethingSeenLiesBesideFields |= !held.asSeen().isNothing();
            }
            // Asked of each layout: the aggregate of a family takes every key.
            if (definesWhatItCopies && value.key) {
                forEachLayoutNamedInLinkLayouts(flow, subject, [&](LayoutID target) {
                    keys.findFieldsInTarget &= !!findField(flow.program, target, value.key);
                });
            }
            FlowNodeID stored = value.key ? nodeStoredUnderInLinkLayouts(flow, subject, value.key) : underAnyKey;
            flow.addEdge(value.node, stored ? stored : flow.unseen());
        }
    });
    return true;
}

// `Object.assign(record, { ["__proto__"]: above })`: a property of the source's own under that name. What lies beside the fields lies under names that nobody lists here.
static bool mayHaveOwnValueUnderPrototypeLinkInLinkLayouts(LinkFlow& flow, const LinkType& source)
{
    bool mayHave = false;
    forEachLayoutNamedInLinkLayouts(flow, source, [&](LayoutID layout) {
        for (const LinkFlow::OwnValue& value : flow.ownValuesOf(layout))
            mayHave |= value.key ? flow.nameClassOf(value.key) == NameClass::Proto : !flow.typeAt(value.node).isNothing();
    });
    return mayHave;
}

// Each object among the sources is of a layout whose own values are listed: no accessor of its own answers the copy, and whatever it holds is led on from where it lies.
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
    case LinkType::Objects::Family:
        break;
    }
    bool isListed = true;
    forEachLayoutNamedInLinkLayouts(flow, source, [&](LayoutID layout) {
        isListed &= !flow.ownValuesOf(layout).empty();
    });
    return isListed;
}

// Which keys the source has nobody can tell: it may have gained some. So what is copied may land in any field, beside the fields, on top of a method, or in a setter.
// What it still waits for: LinkFlow::CopyLookedAt.
enum class CopyWaitsForInLinkLayouts : uint8_t { WhateverLiesBesideFields, WhatIsSeenAlone, Nothing };

static CopyWaitsForInLinkLayouts observeCopyInLinkLayouts(LinkFlow& flow, const ObservedChangeInLinkLayouts& change)
{
    if (change.otherNode && change.other.isNothing())
        return CopyWaitsForInLinkLayouts::WhateverLiesBesideFields;
    FlowNodeID unseen = flow.unseen();
    LinkType anything = LinkType::anything();
    const LinkType& subject = change.subject;
    const LinkType& other = change.other;
    // `Object.assign(module.exports, record)`: what lands on an unseen object, a function or a list is out of sight.
    bool landsOnObjectsOfProgramAlone = !subject.holdsUnseen && !subject.builtin && subject.functions == LinkType::Functions::None && subject.classes == LinkType::Classes::None;
    // `Object.assign({}, defaults, given)` ASSIGNS, and an assignment differs from a definition where it finds a setter, on the target or above it. A record lists no accessor, and above it stands Object.prototype,
    // which has one setter, under `__proto__`, unless a test run lays another. So where every key that a source can have is listed, and none is that name, the assignment lays what a definition lays and nobody gets the target.
    // A record that is linked below something else, or gets an accessor, is opened for that, as it is for the stores by name that are written out.
    // It can only cease to hold as the types climb, and all that is done for a definition is done for an assignment too.
    auto isOfObjectsThatNobodyLists = [](const LinkType& type) {
        return type.holdsUnseen || type.builtin || type.functions != LinkType::Functions::None || type.classes != LinkType::Classes::None || (type.primitives & LinkType::closed(Primitive::AnyString));
    };
    bool assignmentFindsNoSetter = !flow.program.admitsReplacedBuiltins && !change.definesWhatItCopies && change.otherNode && landsOnObjectsOfProgramAlone && !isOfObjectsThatNobodyLists(other)
        && isListedOnBothSidesInLinkLayouts(flow, subject, other) && !mayHaveOwnValueUnderPrototypeLinkInLinkLayouts(flow, other);
    bool definesWhatItCopies = change.definesWhatItCopies || assignmentFindsNoSetter;
    // The edges from field to field are laid whatever else the target holds: an allocation holds unseen objects too once its layout has escaped, and what is laid may not hang on whether the copy was looked at before that.
    KeysOfCopyInLinkLayouts keys;
    bool isListedOnBothSides = change.otherNode && copyFieldByFieldInLinkLayouts(flow, subject, other, definesWhatItCopies, keys);
    bool copiesFieldByField = isListedOnBothSides && landsOnObjectsOfProgramAlone;
    // `Object.assign(EventEmitter, { once, on, EventEmitter })`: what lands on a function is out of sight, and is held to lie there under its key as what a store by name lays there: AccessInLinkAccesses::resolveOnClosure().
    if (change.otherNode && subject.functions != LinkType::Functions::None) {
        forEachLayoutNamedInLinkLayouts(flow, other, [&](LayoutID layout) {
            for (const LinkFlow::OwnValue& value : flow.ownValuesOf(layout)) {
                if (value.key)
                    flow.addEdge(value.node, flow.foundBesideFieldsUnder(value.key));
            }
        });
    }

    forEachLayoutNamedInLinkLayouts(flow, other, [&](LayoutID layout) {
        if (listsAccessorInLinkLayouts(flow.program, layout))
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
    case LinkType::Objects::Family:
    case LinkType::Objects::Lost:
        if (!copiesFieldByField)
            read = flow.anyFieldReadThrough(other.objects, other.layout, other.family);
        break;
    }
    // Under an index, which no layout lists.
    forEachElementsNodeInLinkLayouts(flow, other, [&](FlowNodeID elements) {
        flow.addEdge(elements, unseen);
    });

    // On an object of one of the engine's classes it may land on top of what the class has under the name: SlotOfContents::underUnknownNames().
    if (!namesOnlyInstancesInLinkLayouts(subject) || subject.classes != LinkType::Classes::None)
        handToUnseenCodeInLinkLayouts(flow, change.subjectNode, subject);
    // It may land beside the fields, where a read gives `anything`.
    if (read)
        flow.addEdge(read, unseen);
    forEachElementsNodeInLinkLayouts(flow, subject, [&](FlowNodeID elements) {
        flow.arrive(elements, anything);
    });

    // `{ ...other }` DEFINES. Under `__proto__` that makes a property of that name, so the target stays below what it stands below. And where each key that a source can have is a field of the target, the target gains nothing.
    // What only unseen code may have given a source the proving reading alone counts.
    uint16_t texts = LinkType::closed(Primitive::AnyString);
    bool isNoObjectOfProgram = other.builtin || other.functions != LinkType::Functions::None || other.classes != LinkType::Classes::None;
    bool gainsInSight = !isListedOnBothSides || isNoObjectOfProgram || other.holdsUnseenInSight() || (other.primitivesSeen() & texts) || !keys.findFieldsInTarget || keys.somethingSeenLiesBesideFields;
    bool gainsAtAll = gainsInSight || other.holdsUnseen || (other.primitives & texts) || keys.somethingLiesBesideFields;
    if (!definesWhatItCopies)
        addChangesToLayoutsNamedInLinkLayouts(flow, subject, changesMadeByInLinkLayouts(SummaryChange::Kind::CopiesInto));
    else if (gainsInSight)
        addChangesToLayoutsNamedInLinkLayouts(flow, subject, { LayoutChange::GainsProperty });
    else if (gainsAtAll) {
        forEachLayoutNamedInLinkLayouts(flow, subject, [&](LayoutID layout) {
            flow.layoutsThatGainWhatUnseenCodeGaveSources.set(layout.index());
        });
    }
    // Above a record lies nothing that the program wrote. Which members lie above anything else, under which keys, no table lists.
    forEachLayoutNamedInLinkLayouts(flow, subject, [&](LayoutID layout) {
        if (flow.program.at(layout).kind != LayoutKind::Record || listsAccessorInLinkLayouts(flow.program, layout))
            flow.escape(layout);
    });
    if (subject.objects == LinkType::Objects::Lost)
        handToUnseenCodeInLinkLayouts(flow, change.subjectNode, subject);

    // Of a source that is no object of the program nobody knows what it has: an unseen object, a function, a list, a text.
    bool copiesWhatNobodyLists = !copiesFieldByField || other.holdsUnseen || other.builtin || other.functions != LinkType::Functions::None || other.classes != LinkType::Classes::None || (other.primitives & LinkType::closed(Primitive::AnyString));
    // The copy assigns, and under `__proto__` an assignment lays what is copied above the target.
    // ONE THAT DEFINES RUNS NO SETTER ON THE TARGET OR ABOVE IT AND RELINKS NOTHING: NOBODY GETS THE TARGET. What answers for a source that nobody lists (an accessor of its own, a trap) gets the source.
    bool mayLayPrototype = !definesWhatItCopies && (copiesWhatNobodyLists || mayHaveOwnValueUnderPrototypeLinkInLinkLayouts(flow, other));
    if (mayLayPrototype) {
        ObservedChangeInLinkLayouts relink = change;
        observeChangeOfPrototypeInLinkLayouts(flow, relink);
    }
    if (definesWhatItCopies && change.otherNode && !holdsOnlyObjectsWithListedOwnValuesInLinkLayouts(flow, other))
        handToUnseenCodeInLinkLayouts(flow, change.otherNode, other);
    if (!definesWhatItCopies || copiesWhatNobodyLists || read) {
        if (FlowNodeID stored = nodeLaidUnderAnyKeyByCopyInLinkLayouts(flow, subject, definesWhatItCopies)) {
            if (copiesWhatNobodyLists)
                flow.arrive(stored, anything);
            if (read)
                flow.addEdge(read, stored);
        }
    }
    // EACH LAYOUT OF THE SOURCE HAS TWO NODES THAT TELL THE COPY, AND A LOOK GOES THROUGH ALL THE LAYOUTS: a telling that cannot raise what has been said returns at once (LinkFlow::changeObserved()).
    // An assignment that finds no setter yet waits for whatever may come to lie under `__proto__`.
    if (assignmentFindsNoSetter)
        return CopyWaitsForInLinkLayouts::WhateverLiesBesideFields;
    if (definesWhatItCopies ? gainsInSight : mayLayPrototype)
        return CopyWaitsForInLinkLayouts::Nothing;
    return definesWhatItCopies && gainsAtAll ? CopyWaitsForInLinkLayouts::WhatIsSeenAlone : CopyWaitsForInLinkLayouts::WhateverLiesBesideFields;
}

void LinkFlow::changeObserved(const FlowObserver& observer, const LinkType& now)
{
    switch (observer.kind) {
    case FlowObserver::Kind::SubjectOfChange:
    case FlowObserver::Kind::OtherOfChange:
        break;
    case FlowObserver::Kind::SubjectOfCall:
    case FlowObserver::Kind::ReceiverOfCall:
    case FlowObserver::Kind::SubjectOfAccess:
    case FlowObserver::Kind::KeyOfAccess:
    case FlowObserver::Kind::MemberOfAccess:
    case FlowObserver::Kind::SymbolOfPreparation:
    case FlowObserver::Kind::OperandOfOperator:
    case FlowObserver::Kind::Inspected:
    case FlowObserver::Kind::MemberOfSharedCall:
    case FlowObserver::Kind::MemberOfSharedAccess:
        RELEASE_ASSERT_NOT_REACHED();
    }

    // Of the primitives a change asks only whether there are any, whether null is one and whether a text is one: `{ ...text }` defines its indexes. addEdge() knows only the newest edge of a node again.
    auto withoutKindsOfPrimitives = [](LinkType type) {
        constexpr uint16_t texts = LinkType::closed(Primitive::AnyString);
        if (type.primitives)
            type.primitives = (type.primitives & Primitive::Null) | (type.primitives & texts ? texts : 0) | Primitive::Undefined;
        return type;
    };
    if (!typeObservedBefore().isNothing() && withoutKindsOfPrimitives(typeObservedBefore()) == withoutKindsOfPrimitives(now) && typeObservedBefore().holdsUnseenInSight() == now.holdsUnseenInSight())
        return;

    const SummaryChange& record = summaries.at(observer.function).changes[observer.record];
    UnknownByEscapeAlone unknownByEscapeAlone(isUnknownByEscapeAloneIfAtAll(typeAt(nodeOf(observer.function, record.subject))) && (record.other == noSummaryNode || isUnknownByEscapeAloneIfAtAll(typeAt(nodeOf(observer.function, record.other)))), UnknownByEscapeAlone::Marks::OfRule);
    ObservedChangeInLinkLayouts change;
    change.subjectNode = nodeOf(observer.function, record.subject);
    change.subject = typeAt(change.subjectNode);
    change.key = record.key;
    change.lays = record.lays;
    change.descriptorIsInSight = record.descriptorIsInSight;
    change.laysValueInSight = record.laysValueInSight;
    if (record.valueInSight != noSummaryNode)
        change.valueInSight = nodeOf(observer.function, record.valueInSight);
    change.definesWhatItCopies = record.definesWhatItCopies;
    if (record.getterInSight != noSummaryNode)
        change.getterInSight = nodeOf(observer.function, record.getterInSight);
    if (record.setterInSight != noSummaryNode)
        change.setterInSight = nodeOf(observer.function, record.setterInSight);
    if (record.other != noSummaryNode) {
        change.otherNode = nodeOf(observer.function, record.other);
        change.other = typeAt(change.otherNode);
    }
    if (change.subject.isNothing())
        return;

    // `{ ...message }`: each layout that passes for Message has two nodes beside its fields, and each of them tells the copy whenever it climbs.
    uint64_t placeOfCopy = static_cast<uint64_t>(observer.function.index()) << 32 | observer.record;
    if (record.kind == SummaryChange::Kind::CopiesInto) {
        auto lookedAt = copiesLookedAt.find(placeOfCopy);
        if (lookedAt != copiesLookedAt.end() && lookedAt->value.subject == change.subject && lookedAt->value.other == change.other
            && (lookedAt->value.waitsForNothingBesideFields || (lookedAt->value.waitsForWhatIsSeenAlone && now.asSeen().isNothing())))
            return;
    }

    // `Object.defineProperty(prototype, name, d)`, `delete holder.member`, `Object.assign(holder, source)`
    if (record.kind == SummaryChange::Kind::Deletes || record.kind == SummaryChange::Kind::DefinesByDescriptor) {
        noteWriteOn(change.subject, record.key, observer.function);
        if (!record.key && record.other != noSummaryNode)
            noteSiteOfCopy(observer.function, record.site, change.subjectNode, change.otherNode);
        else
            noteSiteOfWrite(observer.function, record.site, change.subjectNode);
    } else if (record.kind == SummaryChange::Kind::CopiesInto) {
        noteWriteOn(change.subject, { }, observer.function);
    if (record.kind == SummaryChange::Kind::CopiesInto) {
        forEachLayoutNamedInLinkLayouts(*this, change.other, [&](LayoutID layout) {
            for (const OwnValue& value : ownValuesOf(layout)) {
                if (!value.key && copiesToldOfWhatLiesBesideFields.add(std::pair { static_cast<uint64_t>(value.node.index()) << 32 | observer.function.index(), observer.record }).isNewEntry)
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
        observeChangeOfPrototypeInLinkLayouts(*this, change);
        return;
    case SummaryChange::Kind::BecomesPrototype:
        observeNewHeirInLinkLayouts(*this, change);
        return;
    case SummaryChange::Kind::CopiesInto:
    {
        CopyWaitsForInLinkLayouts waitsFor = observeCopyInLinkLayouts(*this, change);
        copiesLookedAt.set(placeOfCopy, CopyLookedAt { change.subject, change.other, waitsFor == CopyWaitsForInLinkLayouts::Nothing, waitsFor == CopyWaitsForInLinkLayouts::WhatIsSeenAlone });
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- Which fields a construction fills

// Of any other layout no field counts as filled: the scan follows the allocations of these alone, and native code makes objects of the others.
static bool constructionsAreFollowedInLinkLayouts(const Layout& layout)
{
    return isFoundByShapeTest(layout) && !layout.isDerived();
}

namespace {

// The object of a class is made by the constructors of the row of classes, the one that allocates first, each with its initializers of fields.
// What they have defined when the object can first be seen from outside all of them is what every object of the layout has.
class RowOfConstructorsInLinkLayouts {
public:
    // UntilEnds: for the mark of the cell, no proof: what each body of the row has defined when it is through with the object, down the whole row, whoever has seen the object meanwhile and whatever lies above it.
    enum class Follows : uint8_t { UntilFirstSight, UntilEnds };

    RowOfConstructorsInLinkLayouts(const LinkFlow& flow, LayoutID made, Follows follows = Follows::UntilFirstSight)
        : m_flow(flow)
        , m_program(flow.program)
        , m_made(made)
        , m_namesMayBeIncomplete(flow.namesMayBeIncomplete(made, ClassSide::Prototype))
        , m_goesToEnds(follows == Follows::UntilEnds)
    {
    }

    // False: it ended above the class that is made, or in it.
    bool followWholeRow() { return followRow({ }) && m_hasGoneThroughRow; }

    // In no order, with repeats.
    Vector<PropertyKeyID, 32>& filled() LIFETIME_BOUND { return m_filled; }
    // The keys that whatLookUpFinds() was asked for. Nothing else of what the row fills hangs on the class that is made.
    Vector<PropertyKeyID, 32>& lookedUp() LIFETIME_BOUND { return m_lookedUp; }
    bool hasLookedUpNameThatEveryClassHides() const { return m_hasLookedUpNameThatEveryClassHides; }

    // By the constructors of the classes above the one named. Nothing: the class is not in the row.
    std::optional<Vector<PropertyKeyID, 32>> findKeysFilledAbove(LayoutID classInRow)
    {
        if (!followRow(classInRow))
            return std::nullopt;
        return WTF::move(m_filled);
    }

private:
    // Whether it came to `end`.
    bool followRow(LayoutID end)
    {
        // `class A extends B {}` beside `class B extends A {}` compiles.
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

    // `function Below(a) { Base.call(this, a); }` with `Object.setPrototypeOf(Below.prototype, Base.prototype)`: `new Below()` hands the object to Below. Base stands above it in the row and runs where Below calls it, if it does.
    // So the row of those that make the object begins with the lowest constructor that takes no object from `super()`.
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
    // One class of the row, behind all above it. False: the row ends here.
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

    // False: from some point of the body on the object may have been seen. What was added holds at that point too.
    bool addWhatBodyFills(FunctionID body, SummaryConstruction::Object object, bool mayHaveParts)
    {
        const ThinSummary& summary = m_flow.summaries.at(body);
        // Evaluated text names `this` where no operand shows it.
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

        for (const SummaryConstruction::Part& part : summary.partsOfConstructions.span().subspan(construction->firstPart, construction->numberOfParts)) {
            size_t numberFilledBefore = m_filled.size();
            bool isStillUnseen = addWhatBodyFills(part.function, SummaryConstruction::Object::This, false);
            if (!part.runsOnEveryWay)
                m_filled.shrink(numberFilledBefore);
            if (!isStillUnseen)
                return false;
        }

        // A method above that has become read only refuses the assignment, and outside strict code nothing is thrown.
        for (const SummaryConstruction::Key& key : keys) {
            if (key.isFilled && (!key.isLookedUp || whatLookUpFinds(key.key) == Found::Nothing))
                m_filled.append(key.key);
        }
        return !construction->isSeenFromOutside;
    }

    // True: the row goes on.
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

    // Above the object, whose class may be below the class of the body that looks.
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
        // `Object.freeze(Object.prototype)`: anybody may do that at any time.
        Found found = m_flow.isAbsentAboveClasses(key) ? Found::Nothing : Found::Method;
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
};

} // anonymous namespace

void LinkFlow::noteFieldMayBeEmpty(FieldReference reference)
{
    uint32_t cell = m_firstFieldOfLayout[reference.layout.index()] + reference.field;
    // It only grows, and whoever set it has done the rest.
    if (fieldsThatMayBeEmpty.get(cell))
        return;
    LockerOfWhatIsShared lockerOfWhatIsShared(*this);
    if (!fieldsThatMayBeEmpty.add(cell) || m_nodesFoundWhereFieldsAreEmpty.isEmpty() || m_nodesFoundWhereFieldsAreEmpty[cell] == UINT32_MAX)
        return;
    arrive(FlowNodeID(m_nodesFoundWhereFieldsAreEmpty[cell]), fromTypeID(TypeTable::undefined));
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
        if (!constructionsAreFollowedInLinkLayouts(program.at(reference.layout)))
            fieldsThatMayBeEmpty.set(cell);
        if (fieldsThatMayBeEmpty.get(cell))
            arrive(node, fromTypeID(TypeTable::undefined));
        return node;
    });
}

// Whatever else `super()` answers has left the construction that made it.
bool LinkFlow::mayReadInFrontOfDefinition(FunctionID reader, const SummaryAccess& access, LayoutID layout) const
{
    if (!access.isPartOfConstruction || !access.subjectIsAnsweredBySuperCall)
        return access.isPartOfConstruction;
    std::optional<Vector<PropertyKeyID, 32>> filledAbove = RowOfConstructorsInLinkLayouts(*this, layout).findKeysFilledAbove(program.at(reader).constructedClass);
    return filledAbove && !filledAbove->contains(access.key);
}

// WHAT THE ROW FILLS DOWN TO A CLASS IS WHAT IT FILLS DOWN TO THE CLASS ABOVE, AND WHAT THE CONSTRUCTOR OF THE CLASS ADDS. A walk down the whole row for every class is a step for each pair of classes in a row.
// All that is read here is laid down when the flow begins. What a body fills hangs on the class that is made in one way alone: by what a lookup finds above the object. So a class goes down the whole row by itself
// where that may differ from what the class above it found: it brings a member or another kind of field under a key that was looked up, its names are known where those above are not, or the name is one that every class hides.
void LinkFlow::findWhatRowsOfConstructorsFill()
{
    using Row = RowOfConstructorsInLinkLayouts;
    struct DownToClass {
        // Places in `lists`.
        uint32_t filled { 0 };
        uint32_t lookedUp { 0 };
        bool goesOn { true };
        bool hasLookedUpNameThatEveryClassHides { false };
    };
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    Vector<Vector<PropertyKeyID>>& lists = m_listsOfKeysThatRowsOfConstructorsFill;
    lists.append(Vector<PropertyKeyID>());
    m_keysThatRowsOfConstructorsFill.grow(numberOfLayouts);

    // The list at `place` and `added`. Sorted, without repeats. A class that adds nothing new has the list of the class above it.
    auto placeOfListWith = [&](uint32_t place, Vector<PropertyKeyID, 32>& added) {
        std::ranges::sort(added);
        added.shrink(std::ranges::unique(added).begin() - added.begin());
        added.removeAllMatching([&](PropertyKeyID key) {
            return std::ranges::binary_search(lists[place], key);
        });
        if (added.isEmpty())
            return place;
        Vector<PropertyKeyID> merged(lists[place].size() + added.size());
        std::ranges::merge(lists[place], added, merged.begin());
        lists.append(WTF::move(merged));
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
            if (!result.goesOn)
                return result;
            result.goesOn = row.followConstructorOf(id, !above);
        }
        result.filled = placeOfListWith(result.filled, row.filled());
        result.lookedUp = placeOfListWith(result.lookedUp, row.lookedUp());
        result.hasLookedUpNameThatEveryClassHides |= row.hasLookedUpNameThatEveryClassHides();
        return result;
    };

    // `class A extends B {}` beside `class B extends A {}` compiles: of a row that comes back to where it has been nothing is followed.
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
        // Who is asked for: noteConstructionsOf(), noteBirthsOf().
        const Layout& asked = program.layouts[index];
        if (!asked.constructor || program.at(asked.constructor).constructedClass != LayoutID(index) || asked.fields.isEmpty() || rowsThatNeverEnd.quickGet(index))
            continue;
        for (LayoutID current(index); current && !isKnown.quickSet(current.index()); current = program.at(current).base)
            rowNotKnown.append(current);
        while (!rowNotKnown.isEmpty()) {
            LayoutID id = rowNotKnown.takeLast();
            const Layout& layout = program.at(id);
            // RowOfConstructorsInLinkLayouts::numberOfConstructorsThatMakeObject().
            bool takesObjectFromSuperCall = layout.base && layout.constructor && program.at(layout.constructor).kind == FunctionKind::DerivedConstructor;
            untilFirstSight[id.index()] = withConstructorOf(id, Row::Follows::UntilFirstSight, takesObjectFromSuperCall ? &untilFirstSight[layout.base.index()] : nullptr);
            untilEnds[id.index()] = withConstructorOf(id, Row::Follows::UntilEnds, layout.base ? &untilEnds[layout.base.index()] : nullptr);
            m_keysThatRowsOfConstructorsFill[id.index()] = { untilFirstSight[id.index()].filled, untilEnds[id.index()].filled };
        }
    }
}

void LinkFlow::noteConstructionsOf(FunctionID id)
{
    noteBirthsOf(id);
    for (FieldReference field : summaries.at(id).fieldsLeftEmpty)
        noteFieldMayBeEmpty(field);

    LayoutID made = program.at(id).constructedClass;
    // The row is asked about the fields of the layout and nothing else.
    if (!made || program.at(made).constructor != id || program.at(made).fields.isEmpty())
        return;
    const Vector<PropertyKeyID>& filled = m_listsOfKeysThatRowsOfConstructorsFill[m_keysThatRowsOfConstructorsFill[made.index()].untilFirstSight];
    const Layout& layout = program.at(made);
    for (uint32_t field = 0; field < layout.fields.size(); ++field) {
        if (!layout.fields[field].key || !std::ranges::binary_search(filled, layout.fields[field].key))
            noteFieldMayBeEmpty({ made, field });
    }
}

// ---- What is written behind the flow

namespace {

static bool isConstantUndefinedInLinkLayouts(const SummaryNode& node)
{
    return node.kind == SummaryNode::Kind::Constant && TypeID::fromBits(node.first) == TypeID(TypeTable::undefined);
}

enum class TraitOfWriteInLinkLayouts : uint8_t {
    ComesAfterConstruction = 1 << 0,
    AddsWhatIsNotListed = 1 << 1,
    MayRunSetter = 1 << 2,
    // It may be what creates the property, as anything but an accessor whose function is named.
    MayGiveValue = 1 << 3,
    // `this.exports = undefined`, `node.jsDoc = undefined`: a body stores the constant on an object that it is making or that a call has just given it.
    GivesUndefinedToObjectBeingMade = 1 << 4,
    // `this.flags = flags`: the construction itself, or the body that constructs, stores anything else.
    FillsObjectBeingConstructed = 1 << 5,
};

} // anonymous namespace

// Types only climb, so the type at a subject now names every layout that it ever named.
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
    const LinkType& typeAt(FunctionID function, uint32_t nodeInSummary) const { return m_flow.typeAt(m_flow.nodeOf(function, nodeInSummary)); }
    // What the run time makes at no node brings values that no flow has seen: nothing is held of it either. What is done to a class object through a lost value is a closed operation, which reports to the guard words of the class.
    bool hasEscaped(uint32_t layout) const
    {
        if (m_flow.escapedLayoutsAsRead().get(layout) || m_isMadeAtNoNode.quickGet(layout))
            return true;
        return m_liesBelowLostClassObject.quickGet(layout) && m_flow.reading() == WhatUnseenCodeDoes::Anything;
    }
    // The families are as the flow left them: nothing is united any more.
    uint32_t lostFamilyOf(LayoutID member) const { return m_flow.unitedLayouts.representativeOf(member.index()); }
    bool answersOnlyWithObjectOfItsConstruction(FunctionID constructor) const;
    bool findWhetherConstructorAnswersOnlyWithObjectOfItsConstruction(FunctionID constructor) const;
    bool superCallMayAnswerWithOlderObject(LayoutID) const;
    bool isWrittenLateThroughLost(LayoutID member, PropertyKeyID key) const
    {
        uint32_t family = lostFamilyOf(member);
        return m_lostFamiliesWrittenLateUnderEveryKey.get(family) || (key && m_keysWrittenLateThroughLost.contains(static_cast<uint64_t>(family) << 32 | key.bits()));
    }

    TypeID unlessAnything(TypeID) const;
    // Null: nothing arrived, or anything may have.
    TypeID claimedTypeAt(FlowNodeID);
    TypeID claimedTypeAtEither(FlowNodeID, FlowNodeID);
    // What the compiler established of a position holds whoever writes there, and says what no flow does: that no read comes in front of the first store. Null: nothing.
    TypeID typeEstablishedOf(const DeclaredType&) const;
    TypeID narrowerOf(TypeID byFlow, const DeclaredType&);
    TypeID typeOfWrittenUnit(WrittenUnit);

    void lookAtEdges(FunctionID, const ThinSummary&);
    void lookAtAccesses(FunctionID, const ThinSummary&);
    void lookAtChanges(FunctionID, const ThinSummary&);
    void noteChangeOfClosure(const LinkType& subject);
    void noteHeritage(const LinkType& heritage, FunctionID constructor);
    void noteNameWrittenOnPrototype(FunctionID, const ThinSummary&, const SummaryAccess&);
    bool isWrittenOnPrototypeAbove(LayoutID, PropertyKeyID) const;
    void lookAtMembers(const ThinSummary&);
    void noteWrite(const LinkType& subject, PropertyKeyID, Traits);
    PropertyKeyID findNameThatConstructorStores(FunctionID, const ThinSummary&, const SummaryAccess&) const;
    // `mayComeToObject`: false where no object of the layout is known by the family that the write goes through.
    void noteWriteInto(LayoutID, PropertyKeyID, Traits, bool mayComeToObject = true);
    bool comesToAccessorAbove(LayoutID, PropertyKeyID) const;
    void noteReadInFrontOfDefinition(FunctionID reader, const SummaryAccess&);

    void noteLostClassObjects();
    bool objectPrototypeMayGainName() const;
    // LinkedFunction::everyCallerIsKnown, which WriterOfLink writes beside the look. While the summaries are looked at nobody has said, and it counts as not known: what the look answers may not hang on how far the other has come.
    bool everyCallerIsKnown(FunctionID function) const { return m_functionsWhoseCallersAreAllKnown.get(function.index()); }
    BitVector m_functionsWhoseCallersAreAllKnown;
    bool mayBeAllocated(LayoutID) const;
    bool mayBeKnownByFamily(LayoutID id) const { return m_flow.layoutsMetInFamilies.get(id.index()) || m_flow.lostLayouts.get(id.index()) || hasEscaped(id.index()); }
    bool canHoldAppendedFields(LayoutID) const;
    bool canNameAppendedField(LayoutID, PropertyKeyID) const;
    // False: it can be no field, so a store under it adds a property.
    bool isListedByClassBelow(LayoutID, PropertyKeyID) const;
    bool noteNameNotListed(LayoutID, PropertyKeyID);
    void noteNamesWrittenOfLayouts();
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
    void noteNameGivenToClosure(const LinkType& subject, PropertyKeyID storedUnder = { });
    void writeNamesOnFunctions();
    BitVector m_functionsWhoseClosuresGetWhatIsNotListed;
    BitVector m_functionsWhoseClosuresGetNames;
    bool m_someConstructorGetsName { false };
    BitVector m_functionsWhosePrototypePropertyIsWritten;
    bool m_prototypePropertyOfSomeConstructorIsWritten { false };

    const LinkFlow& m_flow;
    const Program& m_program;
    // Since write().
    TypeArena* m_types { nullptr };
    TypesOfFlowTurnedOnce m_typesTurned;
    // AOTLinkTypesOnAllThreads.h.
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

    // By layout. Beside LinkFlow::possibleChanges: what stores add.
    Vector<OptionSet<LayoutChange>> m_changes;
    // To all that were lost.
    // By representative.
    Vector<OptionSet<LayoutChange>> m_changesOfLostFamilies;
    BitVector m_isMadeAtNoNode;
    // By function.
    mutable Vector<uint8_t> m_whatConstructorsAnswerWith;
    mutable std::optional<AnswersAlongRows> m_rowsThatMayAnswerWithOlderObject;
    BitVector m_liesBelowLostClassObject;
    BitVector m_isFilledUnderComputedKeys;
    // LinkedLayout::heritage. Null behind a bit of the second: several things arrive, or something that is no class of the program.
    Vector<LayoutID> m_heritages;
    BitVector m_hasHeritage;
    // What the bodies lay on `F.prototype` as they are written, whatever has become of the layout in the flow: by the number of the layout above the key.
    struct NameWrittenOnPrototype {
        FunctionID getter;
        FunctionID setter;
        FunctionID method;
        // One descriptor, which holds an accessor as it is written, or one store of a closure that the body makes, and nothing else.
        bool isWrittenOnce { false };
        bool isWrittenAgain { false };
    };
    HashMap<uint64_t, NameWrittenOnPrototype, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesWrittenOnPrototypes;
    // `const accessors = { ended: { get() { .. } } }`: by the number of the layout of the map above the key. SummaryAccess::mapOfWrittenAccessor.
    HashMap<uint64_t, NameWrittenOnPrototype, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_accessorsWrittenInMaps;
    // `Object.defineProperties(Duplex.prototype, { ended: accessors.ended })`: the first is laid with what the second names, both as above.
    Vector<std::pair<uint64_t, uint64_t>> m_namesLaidOutOfMaps;

    // By cell of LinkTables::fields. writeFields() gathers both at the declaration of the field, because the classes below list it again and a reader may ask either cell.
    BitVector m_fieldsWrittenLate;
    // Something other than an accessor whose function is named may be what creates the property.
    BitVector m_fieldsGivenValues;
    // By cell: WrittenUnit::bits() of the one text, integer or boolean that allocations and constant data list there. 0: nothing has been listed yet. otherThanOneUnit: two of them, or anything else, or a write by name.
    static constexpr uint32_t otherThanOneUnit = UINT32_MAX;
    Vector<uint32_t> m_unitsListedForFields;
    // lookAtEdges(), until lookAtSummaries() has united them with those of the same names.
    struct SaidByEdges {
        BitVector fieldsGivenValues;
        BitVector fieldsGivenUndefinedByMakers;
        BitVector fieldsFilledByConstructors;
        BitVector fieldsWrittenLate;
        BitVector layoutsGivenUndefinedByMakers;
        Vector<uint32_t> unitsListedForFields;
    };
    SaidByEdges m_saidByEdges;
    // By key: some body tells holders apart by what it reads under the name. Of any other field the kind of value says all that anybody asks.
    BitVector m_keysOfTestedFields;
    // LinkedCell::isLeftUndefinedByMakers is the first without the second.
    BitVector m_fieldsGivenUndefinedByMakers;
    BitVector m_fieldsFilledByConstructors;
    // By layout: some cell of it is in the first.
    BitVector m_layoutsGivenUndefinedByMakers;
    // SlotPlan reads the answer off Facts::held() alone.
    bool m_asksWhatMakersGive { false };
    // By layout: LinkedLayout::onlyFunctionThatMakesObjects, and whether there are more.
    Vector<FunctionID> m_onlyFunctionsThatMakeObjects;
    BitVector m_layoutsMadeInSeveralFunctions;
    // Of the body whose accesses are looked at: the entry parameter that is `this` of an object being made. UINT32_MAX: none.
    uint32_t m_thisOfObjectBeingMade { UINT32_MAX };
    BitVector m_fieldsWithUnseenWriters;
    // The representative above the key.
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_keysWrittenLateThroughLost;
    BitVector m_lostFamiliesWrittenLateUnderEveryKey;
    // By cell of LinkTables::fields: a body that is making the object reads the name where it may not have defined it yet.
    BitVector m_fieldsReadInFrontOfDefinition;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_keysReadInFrontOfDefinitionThroughLost;
    // By family and key: the traits that its members have been told of.
    HashMap<uint64_t, uint8_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_writesNotedThroughFamilies;

    // The names that stores lay on objects of layouts that do not list them, as the functions and their accesses are gone through. Each pair once.
    struct NameNotListed {
        LayoutID layout;
        PropertyKeyID key;
        bool isRequiredBySomeWrittenType { false };
        // `function Type(this: Type)`: of the types that are written of `this`.
        bool isRequiredBySomeTypeOfThis { false };
        bool isOptionalInSomeTypeOfThis { false };

        // `function createObjectType(flags, symbol) { const type = createType(flags); type.objectFlags = flags; type.symbol = symbol; return type; }`: a body that stores on what was constructed for it and returns that.
        bool isGivenBySecondConstructor { false };
        // `this[kHeaders] = {}` in the constructor itself.
        bool isGivenByConstructor { false };
        // `req.res = res`, stored by the program on an object of a class of the library.
        bool isLaidOnObjectsOfLibrary { false };
        // By form, as the bits of FieldForm: a definition by a descriptor in sight creates the property in that form.
        uint16_t formsThatDescriptorsCreate { 0 };
        // A definition by name (a field of a class, a member of a literal), or by a descriptor of which nobody knows the flags. A store by name that finds no property creates one beside the slot: it decides nothing.
        bool isDefinedInOtherWays { false };
        OptionSet<FieldForm> formToPlanFor() const
        {
            if (isDefinedInOtherWays || !hasOneBitSet(formsThatDescriptorsCreate))
                return { };
            return OptionSet<FieldForm>::fromRaw(static_cast<uint8_t>(WTF::ctz(static_cast<uint32_t>(formsThatDescriptorsCreate))));
        }

        // `Object.defineProperty(this, "id", { value: id })`: whoever writes the attributes of a property out does it where the objects are made: the slot lies in the cell.
        bool isRequiredByTypeOfThis() const { return (isRequiredBySomeTypeOfThis && !isOptionalInSomeTypeOfThis) || isGivenByConstructor || isGivenBySecondConstructor || isLaidOnObjectsOfLibrary || !formToPlanFor().isEmpty(); }
    };
    Vector<NameNotListed> m_namesNotListed;
    // The number of the layout above the key. Filled in front of everything else, so that it does not matter which body is looked at first.
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesThatConstructorsStore;
    // `req.res = res; req.next = next` in a framework: the number of a layout of the library above the key.
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_namesLaidOnObjectsOfLibrary;
    // Of the access whose write is being noted: lookAtAccesses().
    const SummaryAccess* m_definitionAtHand { nullptr };
    // Beside LinkTables::appendedKeys.
    Vector<OptionSet<FieldForm>> m_formsOfAppendedKeys;
    // The number of the layout above the key: the place in m_namesNotListed.
    HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_pairsNotListed;
    // Stored under on an object of which nobody knows the layout: a lost one, or one that unseen code may have had.
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_keysStoredOnUnknownObjects;
    // By key: how many accesses of reached functions name it.
    Vector<uint32_t> m_mentionsOfKeys;
    // By key: how many other names the program is written to store in front of it. UINT32_MAX: it stores none under the key.
    Vector<uint32_t> m_placesOfFirstStoresOfKeys;
    // `const made = new F(..); made.a = ..; made.b = ..`: the names that one body stores on what a call or an allocation of its own has given it, each once, in the order of the stores.
    struct RunOfMaker {
        // In the body that is looked at: the node of the summary. Behind it: nothing.
        uint32_t subject { 0 };
        // Null: nobody knows what the body was given.
        LayoutID layout;
        Vector<PropertyKeyID, 8> keys;
        // The one function of the program whose call has given the body the object. Null: an allocation or a construction of its own, or nobody knows.
        FunctionID givenBy;
        // An allocation or a construction of the body's own has made the object.
        bool isMadeHere { false };
        // All that the body returns is the object.
        bool isHandedOn { false };
        FunctionID body;
        ValueID firstStore;
        // By place in `keys`, of the first 64: every store of the body under the key stores the constant undefined.
        uint64_t keysOnlyGivenUndefined { 0 };
    };
    Vector<RunOfMaker, 4> m_runsOfMakersInFunction;
    // A list of up to this many is gone through. A longer one is in a table: the body of a module makes objects by the ten thousand, and a literal has as many keys.
    static constexpr size_t longestListGoneThrough = 8;
    // By the node of the summary: the place in m_runsOfMakersInFunction, once those are many.
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_placesOfRunsInFunction;
    // The place in m_runsOfMakersInFunction above the key, of each run that has many.
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_keysOfLongRunsInFunction;
    Vector<RunOfMaker> m_runsOfMakers;
    // `result.links.target = symbol; result.links.mapper = mapper`: the same of the object that lies in a field of what a call or an allocation of the body has given. Each read of the field is a node of its own.
    struct RunOnObjectInField {
        uint32_t holder { 0 };
        PropertyKeyID keyInHolder;
        // The first read.
        uint32_t subject { 0 };
        LayoutID layout;
        Vector<PropertyKeyID, 8> keys;
        // The one function of the program whose call has given the body the holder.
        FunctionID givenBy;
    };
    Vector<RunOnObjectInField, 2> m_runsOnObjectsInFieldsInFunction;
    Vector<RunOnObjectInField> m_runsOnObjectsInFields;
    // By the function above the key: one more than the place in m_runsOnObjectsInFields of the run on what lies under the key in the object that the function returns.
    HashMap<uint64_t, uint32_t> m_runsOnFieldsOfWhatFunctionsReturn;
    // By key: the functions whose closures some body lays into a field under the key. Link's ladder has room for the closures of one function, so of `{ createBaseNode: kind => .. }`, written four times, it says nothing.
    // Kept where the program has tagged makers, for writeRunsBehindTaggedMakers() alone, which guesses.
    HashMap<uint32_t, Vector<FunctionID, 4>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_functionsLaidIntoFieldsUnderKey;
    // By function: one more than the place in m_runsOfMakers of the run on what the function returns, where all that it returns is what one call or one allocation of its own has given it. 0: none.
    // `function createObjectType(flags, symbol) { const type = createTypeWithSymbol(flags, symbol); type.objectFlags = flags; return type; }`: whoever gets the object goes on where this run ended.
    Vector<uint32_t> m_runsOnWhatFunctionsReturn;
    bool m_asksWhatBodyHandsOn { false };
    uint32_t m_numberOfKeysStored { 0 };

    BitVector m_variablesWrittenLate;
    BitVector m_variablesStoredInto;
    BitVector m_variablesSharedWithUnseenCode;

    Vector<LinkedMember> m_members;
    // Of the summary at hand, ascending.
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
    m_changesOfLostFamilies.grow(numberOfLayouts);
    m_lostFamiliesWrittenLateUnderEveryKey.ensureSize(numberOfLayouts);
    m_liesBelowLostClassObject.ensureSize(numberOfLayouts);
    m_isFilledUnderComputedKeys.ensureSize(numberOfLayouts);
    m_heritages.fill(LayoutID(), numberOfLayouts);
    m_hasHeritage.ensureSize(numberOfLayouts);
    m_fieldsWrittenLate.ensureSize(tables.fields.size());
    m_fieldsGivenValues.ensureSize(tables.fields.size());
    m_unitsListedForFields.fill(0, tables.fields.size());
    m_keysOfTestedFields.ensureSize(m_program.types.numberOfPropertyKeys());
    m_fieldsGivenUndefinedByMakers.ensureSize(tables.fields.size());
    m_fieldsFilledByConstructors.ensureSize(tables.fields.size());
    m_layoutsGivenUndefinedByMakers.ensureSize(numberOfLayouts);
    m_asksWhatMakersGive = flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls;
    m_onlyFunctionsThatMakeObjects.fill(FunctionID(), m_asksWhatMakersGive ? numberOfLayouts : 0);
    m_layoutsMadeInSeveralFunctions.ensureSize(numberOfLayouts);
    m_fieldsReadInFrontOfDefinition.ensureSize(tables.fields.size());
    m_fieldsWithUnseenWriters.ensureSize(tables.fields.size());
    m_mentionsOfKeys.fill(0, m_program.types.numberOfPropertyKeys());
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

// What is held of a field, a variable or a capture is an expectation, which is tested at each use: what was seen to arrive, whatever else unseen code may bring. As WriterOfLink::typeIDOf() writes the positions of functions.
TypeID WriterOfLinkLayouts::claimedTypeAt(FlowNodeID node)
{
    LinkType type = m_flow.typeAt(node);
    if (m_flow.reading() == WhatUnseenCodeDoes::OnlyReadsAndCalls) {
        type.holdsUnseen = false;
        type.holdsLostBuiltins = false;
        type.builtin = { };
        if (type.functions != LinkType::Functions::None && type.functions != LinkType::Functions::Closure)
            type.functions = LinkType::Functions::Lost;
    }
    return type.isNothing() ? TypeID() : unlessAnything(typesTurned().turn(m_flow, types(), type));
}

TypeID WriterOfLinkLayouts::claimedTypeAtEither(FlowNodeID first, FlowNodeID second)
{
    if (first == second || m_flow.typeAt(second).isNothing())
        return claimedTypeAt(first);
    if (m_flow.typeAt(first).isNothing())
        return claimedTypeAt(second);
    TypeID atFirst = claimedTypeAt(first);
    TypeID atSecond = claimedTypeAt(second);
    return atFirst && atSecond ? unlessAnything(joinOf(types(), m_flow.families, atFirst, atSecond)) : TypeID();
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

// Both hold.
TypeID WriterOfLinkLayouts::narrowerOf(TypeID byFlow, const DeclaredType& declared)
{
    TypeID established = typeEstablishedOf(declared);
    if (!established)
        return byFlow;
    return byFlow && types().isSubset(byFlow, established) ? byFlow : established;
}

// The scan lays a plain edge into a field only while the body that made the object has it alone, which is construction. An environment is stored into by every closure that holds it.
void WriterOfLinkLayouts::lookAtEdges(FunctionID id, const ThinSummary& summary)
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
            m_saidByEdges.fieldsGivenValues.quickSet(cellOf({ layout, to.second }));
            uint32_t listed = summary.nodes[edge.from].kind == SummaryNode::Kind::Constant && summary.nodes[edge.from].second && !edge.passes ? summary.nodes[edge.from].second : otherThanOneUnit;
            if (uint32_t& unit = m_saidByEdges.unitsListedForFields[cellOf({ layout, to.second })]; unit != listed)
                unit = unit ? otherThanOneUnit : listed;
            if (!m_asksWhatMakersGive) {
            } else if (isConstantUndefinedInLinkLayouts(summary.nodes[edge.from])) {
                m_saidByEdges.fieldsGivenUndefinedByMakers.quickSet(cellOf({ layout, to.second }));
                m_saidByEdges.layoutsGivenUndefinedByMakers.quickSet(layout.index());
            } else
                m_saidByEdges.fieldsFilledByConstructors.quickSet(cellOf({ layout, to.second }));
            if (const SummaryNode& from = summary.nodes[edge.from]; from.kind == SummaryNode::Kind::Closure && !m_flow.taggedMakers.isEmpty()) {
                if (PropertyKeyID key = fieldAt(m_program, { layout, to.second }).key) {
                    Vector<FunctionID, 4>& functions = m_functionsLaidIntoFieldsUnderKey.add(key.bits(), Vector<FunctionID, 4> { }).iterator->value;
                    if (!functions.contains(FunctionID(from.first)))
                        functions.append(FunctionID(from.first));
                }
            }
            if (m_program.at(layout).isEnvironment() || !std::ranges::binary_search(m_layoutsAllocatedHere, layout))
                m_saidByEdges.fieldsWrittenLate.quickSet(cellOf({ layout, to.second }));
        } else if (to.kind == SummaryNode::Kind::Variable) {
            // A binding that cannot be assigned to is stored into once, where its module declares it, if the module runs once.
            const Variable& variable = m_program.variables[to.first];
            bool isStoredOnce = function.isModuleBody() && function.module == variable.module && variable.flags.contains(VariableFlags::IsImmutable)
                && !m_program.at(variable.module).flags.contains(ModuleFlags::RunsAgain);
            if (!isStoredOnce)
                m_variablesWrittenLate.quickSet(to.first);
        }
    }
}

void WriterOfLinkLayouts::lookAtAccesses(FunctionID id, const ThinSummary& summary)
{
    m_thisOfObjectBeingMade = UINT32_MAX;
    bool constructs = m_program.at(id).constructedClass && m_program.at(id).takesThisFromConstruction();
    if (const Function& function = m_program.at(id); m_asksWhatMakersGive && (function.kind == FunctionKind::Initializer || function.takesThisFromConstruction()))
        m_thisOfObjectBeingMade = function.thisParameterIndex().value_or(UINT32_MAX);
    for (const SummaryAccess& access : summary.accesses) {
        if (access.isToContents())
            continue;
        if (access.key && access.key.index() < m_mentionsOfKeys.size())
            ++m_mentionsOfKeys[access.key.index()];
        Traits traits { Trait::AddsWhatIsNotListed, Trait::MayGiveValue };
        switch (access.kind) {
        case SummaryAccess::Kind::Read:
            if (access.isPartOfConstruction)
                noteReadInFrontOfDefinition(id, access);
            continue;
        case SummaryAccess::Kind::Has:
            continue;
        case SummaryAccess::Kind::Store:
            traits.add(Trait::MayRunSetter);
            break;
        case SummaryAccess::Kind::Define:
            break;
        }
        if (!access.isPartOfConstruction)
            traits.add(Trait::ComesAfterConstruction);
        // `meter.value = 1` on an object that somebody has made: where an accessor lies under the name the store runs it, and where nothing lies there the object was made without the field.
        if (!access.isPartOfConstruction && access.kind == SummaryAccess::Kind::Store)
            traits.remove(Trait::MayGiveValue);
        noteNameWrittenOnPrototype(id, summary, access);
        if (access.key && access.key.index() < m_placesOfFirstStoresOfKeys.size() && m_placesOfFirstStoresOfKeys[access.key.index()] == UINT32_MAX)
            m_placesOfFirstStoresOfKeys[access.key.index()] = m_numberOfKeysStored++;
        PropertyKeyID key = m_flow.keyOf(id, access);
        noteStoreOfMaker(summary, access, key);
        traits.add(traitsOfStoreOfMaker(id, summary, access));
        // Objects that have met others in unseen hands are known by no layout. What a constructor stores on `this` speaks of the layout that it constructs all the same.
        if (LayoutID constructed = traits.containsAny({ Trait::GivesUndefinedToObjectBeingMade, Trait::FillsObjectBeingConstructed }) ? m_program.at(id).constructedClass : LayoutID(); constructed && summary.nodes[access.subject].kind == SummaryNode::Kind::Parameter && typeAt(id, access.subject).objects == LinkType::Objects::Lost) {
            if (std::optional<FieldReference> found = findField(m_program, constructed, access.key)) {
                (traits.contains(Trait::FillsObjectBeingConstructed) ? m_fieldsFilledByConstructors : m_fieldsGivenUndefinedByMakers).quickSet(cellOf(*found));
                m_layoutsGivenUndefinedByMakers.quickSet(constructed.index());
            }
        }
        const LinkType& subject = typeAt(id, access.subject);
        // Objects that have met others in unseen hands are known by no layout, and the store would name none.
        if (constructs && key && (subject.objects == LinkType::Objects::Lost || subject.objects == LinkType::Objects::None) && findNameThatConstructorStores(id, summary, access))
            noteWriteInto(m_program.at(id).constructedClass, key, traits);
        noteChangeOfClosure(subject);
        noteWriteOfPrototypeProperty(subject, key, access.kind == SummaryAccess::Kind::Store && access.storesObjectMadeForIt);
        if (access.kind != SummaryAccess::Kind::Store || !key || m_program.nameClassOf(key) != NameClass::Prototype)
            noteNameGivenToClosure(subject, access.laysValueInSightUnderName() ? key : PropertyKeyID());
        // An object of a class of Bun's own library that the program has in hand, known by its layout: the library makes one for a request or a connection and hands it over, and whoever lays a name on it there does so to each.
        if (key && subject.objects == LinkType::Objects::Layout && !access.isToContents() && m_program.at(id).role != FunctionRole::Library) {
            const Layout& stored = m_program.at(subject.layout);
            if (stored.kind == LayoutKind::Instance && stored.constructor && m_program.at(stored.constructor).role == FunctionRole::Library)
                m_namesLaidOnObjectsOfLibrary.add(static_cast<uint64_t>(subject.layout.index()) << 32 | key.bits());
        }
        if (const SummaryNode& made = summary.nodes[access.subject]; key && made.kind == SummaryNode::Kind::Allocation && !access.isToContents() && m_program.at(LayoutID(made.first)).has(LayoutFlags::IsOfCreatedObject))
            m_namesThatConstructorsStore.add(static_cast<uint64_t>(made.first) << 32 | key.bits());
        m_definitionAtHand = access.kind == SummaryAccess::Kind::Define ? &access : nullptr;
        noteWrite(subject, key, traits);
        m_definitionAtHand = nullptr;
    }
    finishRunsOfMakersIn(id);
    finishRunsOnObjectsInFieldsIn(id);
}

// `function Reply(res) { this.raw = res; this[kHeaders] = {}; }`: the front end lists `raw`; that kHeaders is one symbol only Link knows. Null: the access is no store of a constructor on `this` under a name that its layout does not list.
PropertyKeyID WriterOfLinkLayouts::findNameThatConstructorStores(FunctionID id, const ThinSummary& summary, const SummaryAccess& access) const
{
    const Function& function = m_program.at(id);
    if (!function.constructedClass || !function.takesThisFromConstruction() || access.isToContents() || (access.kind != SummaryAccess::Kind::Store && access.kind != SummaryAccess::Kind::Define))
        return { };
    const SummaryNode& subject = summary.nodes[access.subject];
    if (subject.kind != SummaryNode::Kind::Parameter || subject.first != function.thisParameterIndex())
        return { };
    PropertyKeyID key = m_flow.keyOf(id, access);
    return key && !findField(m_program, function.constructedClass, key) ? key : PropertyKeyID();
}

// What a call or an allocation of its own has given the body, and `this` of a body that `new` or a constructor hands the object.
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
    // A body stores on the fields of few objects that it was just given. One that does so of many says nothing of the later ones.
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
        if (subject.isNothing())
            continue;
        if (subject.objects == LinkType::Objects::Layout)
            run.layout = subject.layout;
        if (const SummaryNode& holder = summary.nodes[run.holder]; holder.kind == SummaryNode::Kind::ResultOfCall && !summary.calls[holder.first].isConstruction) {
            const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[id.index()] + holder.first];
            if (callees.listed.size() == 1)
                run.givenBy = callees.listed[0];
        }
        if (returnsOneNode && run.holder == returned)
            m_runsOnFieldsOfWhatFunctionsReturn.set(static_cast<uint64_t>(id.index() + 1) << 32 | run.keyInHolder.bits(), static_cast<uint32_t>(m_runsOnObjectsInFields.size()) + 1);
        m_runsOnObjectsInFields.append(WTF::move(run));
    }
    m_runsOnObjectsInFieldsInFunction.shrink(0);
}

void WriterOfLinkLayouts::finishRunsOfMakersIn(FunctionID id)
{
    // Most bodies store on nothing that a call or an allocation has given them, and their edges are not gone through. Whether such a body hands on what it was given is asked where a maker builds on its answer: appendFields().
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
    // A body that only hands on what it was given has a run without a name.
    if (returned != noSummaryNode && !m_runsOfMakersInFunction.containsIf([&](const RunOfMaker& run) { return run.subject == returned; }))
        m_runsOfMakersInFunction.append(RunOfMaker { returned, { }, { }, { }, false, false, { }, { } });
    for (RunOfMaker& run : m_runsOfMakersInFunction) {
        const LinkType& subject = typeAt(id, run.subject);
        if (subject.isNothing())
            continue;
        if (subject.objects == LinkType::Objects::Layout)
            run.layout = subject.layout;
        if (const SummaryNode& node = summary.nodes[run.subject]; node.kind == SummaryNode::Kind::ResultOfCall && !summary.calls[node.first].isConstruction) {
            const LinkFlow::CalleesOfCall& callees = m_flow.calleesOfCallsAsSettled()[m_flow.firstCallOfFunction[id.index()] + node.first];
            if (callees.listed.size() == 1)
                run.givenBy = callees.listed[0];
        }
        run.isMadeHere = summary.nodes[run.subject].kind == SummaryNode::Kind::Allocation || summary.calls[summary.nodes[run.subject].first].isConstruction;
        run.isHandedOn = run.subject == returned;
        run.body = id;
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
    case LinkType::Objects::Family:
        m_program.at(m_flow.families.layoutsOf(subject.family)).forEach(note);
        return;
    case LinkType::Objects::Lost:
        m_keysReadInFrontOfDefinitionThroughLost.add(key.bits());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void WriterOfLinkLayouts::lookAtChanges(FunctionID id, const ThinSummary& summary)
{
    for (const SummaryChange& change : summary.changes) {
        const LinkType& subject = typeAt(id, change.subject);
        bool waitsForOther = change.kind == SummaryChange::Kind::CopiesInto && change.other != noSummaryNode && typeAt(id, change.other).isNothing();
        if (subject.isNothing() || waitsForOther)
            continue;
        noteChangeOfClosure(subject);
        // What a data descriptor in sight lays the definition's own access has listed.
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
            // A closure that the body makes is a node of the summary at which nothing arrives.
            if (change.other != noSummaryNode && summary.nodes[change.other].kind == SummaryNode::Kind::Closure)
                noteHeritage(subject, FunctionID(summary.nodes[change.other].first));
            break;
        case SummaryChange::Kind::Deletes:
            noteWriteOfPrototypeProperty(subject, change.key);
            noteWrite(subject, change.key, { Trait::ComesAfterConstruction, Trait::MayGiveValue });
            break;
        case SummaryChange::Kind::DefinesByDescriptor:
            noteWriteOfPrototypeProperty(subject, change.key);
            if (!change.isUnderNameThatNobodyElseNames)
                noteWrite(subject, change.key, change.lays == SummaryChange::Lays::WhatDescriptorSays ? Traits { Trait::ComesAfterConstruction, Trait::MayGiveValue } : Traits { Trait::ComesAfterConstruction });
            break;
        case SummaryChange::Kind::CopiesInto:
            noteWriteOfPrototypeProperty(subject, { });
            noteWrite(subject, { }, { Trait::ComesAfterConstruction, Trait::MayGiveValue });
            break;
        }
    }
}

// By the types that the flow ended with, so it is the same in whatever order the flow ran.
void WriterOfLinkLayouts::noteChangeOfClosure(const LinkType& subject)
{
    if (subject.functions == LinkType::Functions::Closure)
        m_tables.functions[subject.function.index()].closuresAreChanged = true;
}

// A function that has met another is lost, and not every caller of it is known. Constructors that have met are still known by their family.
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

void WriterOfLinkLayouts::noteNameGivenToClosure(const LinkType& subject, PropertyKeyID storedUnder)
{
    if (subject.functions == LinkType::Functions::Closure && !(storedUnder && m_flow.followsNamesOnClosuresOf(subject.function, storedUnder)))
        m_functionsWhoseClosuresGetWhatIsNotListed.set(subject.function.index());
    if (subject.functions == LinkType::Functions::Closure)
        m_functionsWhoseClosuresGetNames.set(subject.function.index());
    else if (subject.functions == LinkType::Functions::Constructors)
        m_someConstructorGetsName = true;
}

void WriterOfLinkLayouts::writeNamesOnFunctions()
{
    for (const LinkFlow::NameOnClosures& name : m_flow.namesOnClosures()) {
        const LinkType& held = m_flow.typeAt(name.laid);
        if (held.isNothing())
            continue;
        LinkedNameOnFunction linked { name.function, name.key, FunctionID() };
        LinkType oneClosure;
        oneClosure.functions = LinkType::Functions::Closure;
        oneClosure.function = held.function;
        if (held == oneClosure && m_tables.functions[name.function.index()].closuresGetOnlyNamesThatAreListed)
            linked.function = held.function;
        m_tables.namesOnFunctions.append(linked);
    }
    std::ranges::sort(m_tables.namesOnFunctions, { }, [](const LinkedNameOnFunction& name) {
        return std::pair { name.holder, name.key };
    });
}

void WriterOfLinkLayouts::writeWhosePrototypePropertyStaysAsMade()
{
    for (size_t index = 0; index < m_tables.functions.size(); ++index) {
        LinkedFunction& function = m_tables.functions[index];
        function.closuresGetNoNameButPrototype = function.everyCallerIsKnown && !m_someConstructorGetsName && !m_functionsWhoseClosuresGetNames.get(index);
        function.closuresGetOnlyNamesThatAreListed = function.everyCallerIsKnown && !m_someConstructorGetsName && !m_functionsWhoseClosuresGetWhatIsNotListed.get(index);
        bool isWrittenInNoOtherWay = function.everyCallerIsKnown && !m_prototypePropertyOfSomeConstructorIsWritten && !m_functionsWhosePrototypePropertyIsWritten.get(index);
        function.prototypePropertyStaysAsMade = isWrittenInNoOtherWay && !m_functionsGivenObjectMadeToBePrototype.get(index);
    }
}

void WriterOfLinkLayouts::noteHeritage(const LinkType& heritage, FunctionID constructor)
{
    LayoutID classLayout = m_flow.classOfConstructor(constructor);
    if (!classLayout || m_program.at(classLayout).base)
        return;
    // ResolveHeritage throws for every primitive but null.
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

// `F.prototype.name = ..`, `Object.defineProperties(F.prototype, { name: .. })`: the subject is a read under `prototype` off a closure that the body makes, or off the variable that is born as the function.
// The flow loses sight of the object once the layout has escaped, and whether it has by the time the access is told hangs on the order.
void WriterOfLinkLayouts::noteNameWrittenOnPrototype(FunctionID id, const ThinSummary& summary, const SummaryAccess& access)
{
    if (access.key && access.mapOfWrittenAccessor) {
        auto added = m_accessorsWrittenInMaps.add(static_cast<uint64_t>(access.mapOfWrittenAccessor.index()) << 32 | access.key.bits(), NameWrittenOnPrototype { access.writtenGetter, access.writtenSetter, { }, true });
        if (!added.isNewEntry && (added.iterator->value.getter != access.writtenGetter || added.iterator->value.setter != access.writtenSetter))
            added.iterator->value.isWrittenOnce = false;
    }
    const SummaryNode& object = summary.nodes[access.subject];
    if (!access.key || object.kind != SummaryNode::Kind::ResultOfAccess)
        return;
    const SummaryAccess& read = summary.accesses[object.first];
    if (read.kind != SummaryAccess::Kind::Read || read.key != m_program.keyOf(BuiltinNames::prototype))
        return;
    const SummaryNode& holder = summary.nodes[read.subject];
    FunctionID function;
    if (holder.kind == SummaryNode::Kind::Closure)
        function = FunctionID(holder.first);
    else if (holder.kind == SummaryNode::Kind::Variable && m_program.variables[holder.first].initialization == VariableInitialization::Function)
        function = m_program.variables[holder.first].function;
    LayoutID layout = function ? m_flow.layoutOfConstructorFunction(function) : LayoutID();
    if (!layout)
        return;
    NameWrittenOnPrototype written;
    if (access.kind == SummaryAccess::Kind::Define && access.laysAccessorAsWritten)
        written = { access.writtenGetter, access.writtenSetter, { }, true };
    else if (access.kind == SummaryAccess::Kind::Store && access.value != noSummaryNode && summary.nodes[access.value].kind == SummaryNode::Kind::Closure)
        written = { { }, { }, FunctionID(summary.nodes[access.value].first), true };
    auto added = m_namesWrittenOnPrototypes.add(static_cast<uint64_t>(layout.index()) << 32 | access.key.bits(), written);
    if (!added.isNewEntry) {
        added.iterator->value.isWrittenOnce = false;
        added.iterator->value.isWrittenAgain = true;
        return;
    }
    // By the types that the flow ended with: objects of one layout are all that the descriptor is read off.
    if (access.kind != SummaryAccess::Kind::Define || access.descriptorFromElsewhere == noSummaryNode || summary.nodes[access.descriptorFromElsewhere].kind != SummaryNode::Kind::ResultOfAccess)
        return;
    const SummaryAccess& taken = summary.accesses[summary.nodes[access.descriptorFromElsewhere].first];
    const LinkType& map = typeAt(id, taken.subject);
    if (taken.kind == SummaryAccess::Kind::Read && taken.key && map.objects == LinkType::Objects::Layout && map.functions == LinkType::Functions::None && !map.holdsUnseen)
        m_namesLaidOutOfMaps.append({ static_cast<uint64_t>(layout.index()) << 32 | access.key.bits(), static_cast<uint64_t>(map.layout.index()) << 32 | taken.key.bits() });
}

bool WriterOfLinkLayouts::isWrittenOnPrototypeAbove(LayoutID layout, PropertyKeyID key) const
{
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
    if (key && traits.contains(Trait::AddsWhatIsNotListed) && (subject.holdsUnseen || subject.objects == LinkType::Objects::Lost))
        m_keysStoredOnUnknownObjects.add(key.bits());

    switch (subject.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        return;
    case LinkType::Objects::Layout:
        noteWriteInto(subject.layout, key, traits);
        return;
    case LinkType::Objects::Family: {
        uint8_t& noted = m_writesNotedThroughFamilies.add(static_cast<uint64_t>(subject.family.index()) << 32 | key.bits(), 0).iterator->value;
        if ((noted & traits.toRaw()) == traits.toRaw())
            return;
        noted |= traits.toRaw();
        m_program.at(m_flow.families.layoutsOf(subject.family)).forEach([&](LayoutID member) {
            noteWriteInto(member, key, traits, mayBeKnownByFamily(member));
        });
        return;
    }
    case LinkType::Objects::Lost: {
        if (!subject.layout)
            return;
        uint32_t family = lostFamilyOf(subject.layout);
        // Under a name, LinkFlow::run() has noted which members lack it.
        if (traits.contains(Trait::AddsWhatIsNotListed) && !key)
            m_changesOfLostFamilies[family].add(LayoutChange::GainsProperty);
        if (traits.contains(Trait::MayRunSetter) && (!key || m_program.nameClassOf(key) == NameClass::Proto))
            m_changesOfLostFamilies[family].add(LayoutChange::ChangesPrototype);
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

void WriterOfLinkLayouts::noteWriteInto(LayoutID layout, PropertyKeyID key, Traits traits, bool mayComeToObject)
{
    OptionSet<LayoutChange>& changes = m_changes[layout.index()];
    if (traits.contains(Trait::MayRunSetter) && (!key || m_program.nameClassOf(key) == NameClass::Proto))
        changes.add(LayoutChange::ChangesPrototype);

    if (!key) {
        for (uint32_t cell = m_tables.firstFieldOfLayout[layout.index()]; cell < m_tables.firstFieldOfLayout[layout.index() + 1]; ++cell)
            m_unitsListedForFields[cell] = otherThanOneUnit;
        if (traits.contains(Trait::MayGiveValue)) {
            for (uint32_t cell = m_tables.firstFieldOfLayout[layout.index()]; cell < m_tables.firstFieldOfLayout[layout.index() + 1]; ++cell)
                m_fieldsGivenValues.quickSet(cell);
        }
        if (traits.contains(Trait::AddsWhatIsNotListed)) {
            changes.add(LayoutChange::GainsProperty);
            if (mayComeToObject)
                m_isFilledUnderComputedKeys.quickSet(layout.index());
        }
        if (traits.contains(Trait::ComesAfterConstruction)) {
            for (uint32_t cell = m_tables.firstFieldOfLayout[layout.index()]; cell < m_tables.firstFieldOfLayout[layout.index() + 1]; ++cell)
                m_fieldsWrittenLate.quickSet(cell);
        }
        return;
    }

    std::optional<FieldReference> found = findField(m_program, layout, key);
    if (!found) {
        if (!traits.contains(Trait::AddsWhatIsNotListed) || (traits.contains(Trait::MayRunSetter) && comesToAccessorAbove(layout, key)))
            return;
        // `class Below extends Base { c = 1 }`: the constructor below stores into what the constructor above has allocated, which is known there as an object of the class above and is one of the class below.
        bool isFieldOfClassBelow = !traits.contains(Trait::ComesAfterConstruction) && isListedByClassBelow(layout, key);
        if (!(mayComeToObject && !isFieldOfClassBelow && noteNameNotListed(layout, key)))
            changes.add(LayoutChange::GainsProperty);
        return;
    }
    m_unitsListedForFields[cellOf(*found)] = otherThanOneUnit;
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
}

// ---- The fields that are appended. A list claims nothing, so whom it names decides only how fast a name is found and how large the tables are.

// Only a compiled object has slots.
bool WriterOfLinkLayouts::canHoldAppendedFields(LayoutID id) const
{
    const Layout& layout = m_program.at(id);
    return isFoundByShapeTest(layout) && !layout.isDerived() && !layout.has(LayoutFlags::IsNeverAllocated) && mayBeAllocated(id);
}

// An index may be an element, and `__proto__` finds the accessor of Object.prototype. A store under the name of an accessor creates nothing, and under that of a method it hides the method in some of the objects.
bool WriterOfLinkLayouts::canNameAppendedField(LayoutID layout, PropertyKeyID key) const
{
    // `socket[kRequest] = request`, where all that kRequest can be is one symbol: the access goes by the key of the symbol (LinkFlow::keyOf()), and a layout lists fields under such keys like any other.
    PropertyKeyKind kind = m_program.at(key).kind;
    if (!m_program.types.hasStableNumber(key) || (kind != PropertyKeyKind::Text && kind != PropertyKeyKind::CreatedSymbol && kind != PropertyKeyKind::RegisteredSymbol))
        return false;
    NameClass nameClass = m_program.nameClassOf(key);
    // `Object.defineProperty(konstructor.prototype, name, { get, set })`, where nobody knows the name: a store under the name of an accessor finds its slot empty each time and goes upward, which no table remembers: so it refuses every name.
    // But for what the constructor itself stores on `this`: whoever makes the object means to lay the property, and the front end lists `this.raw = res` whatever is defined above. A guess at where a slot pays: a list claims nothing.
    uint64_t layoutAndKey = static_cast<uint64_t>(layout.index()) << 32 | key.bits();
    bool countsKeysThatNobodyKnows = kind == PropertyKeyKind::Text && !m_namesThatConstructorsStore.contains(layoutAndKey) && !m_namesLaidOnObjectsOfLibrary.contains(layoutAndKey);
    // What Object.create() makes stands below an object that nobody knows: `ctx.body = text` may find a setter there each time. Whoever makes the object means to lay what he stores on it.
    if (countsKeysThatNobodyKnows && m_program.at(layout).has(LayoutFlags::IsOfCreatedObject))
        return false;
    if (mayBeElementKey(nameClass) || nameClass == NameClass::Proto || m_flow.findMembers(layout, ClassSide::Prototype, key).mayAnswer() || m_flow.mayLieOnPrototypeAbove(layout, key, countsKeysThatNobodyKnows))
        return false;
    // The same of what stands above a class that is written without a base.
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
    // `class C1 extends C0 { f1 = 1 }   class C2 extends C1 { f2 = 2 }   ..`: each initializer stores into what the constructor at the top has allocated, so each name is asked of each class above the one that lists it,
    // and that one stands behind all the classes between. Program::layoutsWithName() leaves out some that list a name, so it says yes and never no.
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
    if (m_definitionAtHand) {
        NameNotListed& name = m_namesNotListed[added.iterator->value];
        if (m_definitionAtHand->isByDescriptor && m_definitionAtHand->formCreated)
            name.formsThatDescriptorsCreate |= static_cast<uint16_t>(1u << m_definitionAtHand->formCreated->toRaw());
        else
            name.isDefinedInOtherWays = true;
    }
    return true;
}

// `interface Links { seen?: boolean }   function Links(this: Links) { }   linksOf(node).seen = true`, where nobody knows what linksOf() answers.
// The store names a field of the layouts that may hide behind such a value and of whose objects a written type declares the name. A method lies above the objects, and what is written of an accessor is what its getter answers.
// `interface Type { flags: number }   interface ObjectType extends Type { objectFlags: number }   function Type(this: Type) { }   (made as ObjectType).objectFlags = 0`:
// a type that no layout with objects is written as speaks of the objects of the nearest types above it that some layout is written as: only a cast leads to it.
void WriterOfLinkLayouts::noteNamesWrittenOfLayouts()
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
    auto isWrittenTypeAlone = [&](LayoutID id) {
        return m_program.at(id).has(LayoutFlags::IsNeverAllocated);
    };
    // By written type. A cast leads down as well as up, and what a class declares is stored on its own objects: it speaks for no other layout.
    Vector<Vector<LayoutID>> layoutsWrittenAs(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!canHoldAppendedFields(LayoutID(index)))
            continue;
        for (LayoutID view : m_program.viewsAbove(LayoutID(index))) {
            if (isWrittenTypeAlone(view))
                layoutsWrittenAs[view.index()].append(LayoutID(index));
        }
    }

    Vector<LayoutID, 16> described;
    Vector<LayoutID, 16> pending;
    BitVector isVisited(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& written = m_program.layouts[index];
        if (!isWrittenTypeAlone(LayoutID(index)) || written.fields.isEmpty())
            continue;

        described.shrink(0);
        isVisited.clearAll();
        pending.append(LayoutID(index));
        while (!pending.isEmpty()) {
            LayoutID current = pending.takeLast();
            if (!current || current.index() >= numberOfLayouts || isVisited.quickSet(current.index()) || !isWrittenTypeAlone(current))
                continue;
            if (!layoutsWrittenAs[current.index()].isEmpty()) {
                described.append(layoutsWrittenAs[current.index()].span());
                continue;
            }
            pending.append(m_program.at(current).base);
            for (LayoutID above : m_program.at(current).supertypes)
                pending.append(above);
        }
        if (described.isEmpty())
            continue;
        std::ranges::sort(described);
        described.shrink(described.size() - std::ranges::unique(described).size());

        for (const Field& field : written.fields) {
            if (!field.key || field.form.contains(FieldForm::IsAccessor) || field.flags.contains(FieldFlags::IsMethod))
                continue;
            bool isStoredOnUnknownObjects = m_keysStoredOnUnknownObjects.contains(field.key.bits());
            for (LayoutID id : described) {
                auto noted = m_pairsNotListed.find(static_cast<uint64_t>(id.index()) << 32 | field.key.bits());
                if (noted == m_pairsNotListed.end()) {
                    bool mayHideBehindUnknownObjects = m_flow.lostLayouts.get(id.index()) || hasEscaped(id.index());
                    if (!isStoredOnUnknownObjects || !mayHideBehindUnknownObjects || findField(m_program, id, field.key) || !noteNameNotListed(id, field.key))
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

// The names of a layout come in the order in which the program is written to store them first: a maker stores one name after the other, and a granule that lies behind all that an Extension holds goes into the room that is left.
// A layout that is stored more names than it has room for keeps what the type of `this` requires, and then those that the program names most often. The others are extra properties.
void WriterOfLinkLayouts::appendFields()
{
    noteNamesWrittenOfLayouts();
    // What is found may build on the answer of another body in turn. Behind this no run is added.
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
    // A SECOND CONSTRUCTOR: a body that stores on an object which was made for it, by a construction of its own or by another such body, and returns it.
    // WHAT IT STORES LIES IN THE CELL WHERE OTHER MAKERS BUILD ON IT: `createType()` under `createTypeWithSymbol()` under `createObjectType()` under `createTypeReference()`: it is to them what the constructor of a base class is
    // to the classes below. What the last maker of a row stores only its own kind of object gets, and all the kinds have one cell: forty makers' names in every object.
    BitVector bodiesThatOtherMakersBuildOn(m_program.functions.size());
    for (const RunOfMaker& run : m_runsOfMakers) {
        if (run.givenBy)
            bodiesThatOtherMakersBuildOn.quickSet(run.givenBy.index());
    }
    // Where nobody knows what the body was given: the layouts that are stored every name of the run.
    HashMap<uint32_t, Vector<LayoutID, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> layoutsStoredUnderKey;
    auto placeOfName = [&](LayoutID layout, PropertyKeyID key) -> std::optional<uint32_t> {
        auto found = m_pairsNotListed.find(static_cast<uint64_t>(layout.index()) << 32 | key.bits());
        return found == m_pairsNotListed.end() ? std::nullopt : std::optional(found->value);
    };
    for (NameNotListed& name : m_namesNotListed)
        name.isGivenByConstructor = m_namesThatConstructorsStore.contains(static_cast<uint64_t>(name.layout.index()) << 32 | name.key.bits());
    for (NameNotListed& name : m_namesNotListed)
        name.isLaidOnObjectsOfLibrary = m_namesLaidOnObjectsOfLibrary.contains(static_cast<uint64_t>(name.layout.index()) << 32 | name.key.bits());
    Vector<const RunOfMaker*, 8> makersInFront;
    for (const RunOfMaker& run : m_runsOfMakers) {
        if (!run.isHandedOn || run.keys.isEmpty() || !bodiesThatOtherMakersBuildOn.quickGet(run.body.index()))
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
        if (LayoutID known = run.layout ? run.layout : first->layout) {
            giveNamesTo(known);
            continue;
        }
        if (layoutsStoredUnderKey.isEmpty()) {
            for (const NameNotListed& name : m_namesNotListed)
                layoutsStoredUnderKey.add(name.key.bits(), Vector<LayoutID, 2> { }).iterator->value.append(name.layout);
        }
        // The layouts that list every name themselves: `result.declarations = symbol.declarations; result.parent = symbol.parent`.
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
    m_pairsNotListed.clear();
    // What stands above a class that is written without a base is known now, whichever body was looked at first.
    m_namesNotListed.removeAllMatching([&](const NameNotListed& name) {
        if (canNameAppendedField(name.layout, name.key))
            return false;
        m_changes[name.layout.index()].add(LayoutChange::GainsProperty);
        return true;
    });
    std::ranges::stable_sort(m_namesNotListed, { }, [&](const NameNotListed& name) {
        return std::pair { name.layout.index(), m_placesOfFirstStoresOfKeys[name.key.index()] };
    });

    uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
    m_tables.firstAppendedFieldOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
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
        std::ranges::stable_sort(placesByWorth, std::ranges::greater { }, [&](uint32_t place) {
            return std::pair { names[place].isRequiredByTypeOfThis(), m_mentionsOfKeys[names[place].key.index()] };
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
    }
    m_tables.firstAppendedFieldOfLayout.append(static_cast<uint32_t>(m_tables.appendedKeys.size()));
    for (uint32_t index = 0; index + 1 < m_tables.firstAppendedFieldOfLayout.size(); ++index) {
        std::optional<uint32_t> namedMostOften;
        for (uint32_t place = m_tables.firstAppendedFieldOfLayout[index]; place < m_tables.firstAppendedFieldOfLayout[index + 1]; ++place) {
            if (m_tables.appendedFieldsThatTypeOfThisRequires.get(place))
                continue;
            if (!namedMostOften || m_mentionsOfKeys[m_tables.appendedKeys[place].index()] > m_mentionsOfKeys[m_tables.appendedKeys[*namedMostOften].index()])
                namedMostOften = place;
        }
        if (namedMostOften)
            m_tables.appendedFieldsNamedMostOften.set(*namedMostOften);
    }
    m_tables.appendedFields.fill(LinkedCell { }, m_tables.appendedKeys.size());
    for (size_t place = 0; place < m_formsOfAppendedKeys.size(); ++place)
        m_tables.appendedFields[place].formThatDefinitionsCreate = m_formsOfAppendedKeys[place];
}

// The order in which a maker stores is the order in which its objects get their keys: plan/AOTKeyOrders.cpp. A guess.
// What the layout lists itself its construction has created. A name that has no field is an extra property, and from there on the keys of the object are in no order of the image.
// Where nobody knows what the maker was given, its stores speak of the layouts that have fields for them: for all of them, or for the first two at least.
void WriterOfLinkLayouts::writeRunsOfMakers()
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_program.layouts.size());
    // By each key that some layout has appended: those of the layouts whose objects a maker may have been given without anybody knowing. A family of ten thousand classes has a key in common and as many makers.
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
    // `createObjectType()` stores behind `createTypeWithSymbol()`, which stores behind `createType()`, which constructs: the object has the names of all of them, the innermost first.
    // So the run of a maker begins with the runs of those that had the object in front of it. A guess like the run itself.
    Vector<const RunOfMaker*, 8> makers;
    Vector<PropertyKeyID, 32> keysOfMakers;
    auto keysOfAllMakersOf = [&](const RunOfMaker& last) -> std::span<const PropertyKeyID> {
        makers.shrink(0);
        for (const RunOfMaker* maker = &last; maker && !makers.contains(maker);) {
            makers.append(maker);
            uint32_t placePlusOne = maker->givenBy && !m_runsOnWhatFunctionsReturn.isEmpty() ? m_runsOnWhatFunctionsReturn[maker->givenBy.index()] : 0;
            maker = placePlusOne ? &m_runsOfMakers[placePlusOne - 1] : nullptr;
        }
        if (makers.size() == 1)
            return last.keys.span();
        keysOfMakers.shrink(0);
        for (const RunOfMaker* maker : makers | std::views::reverse) {
            for (PropertyKeyID key : maker->keys) {
                if (!keysOfMakers.contains(key))
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
    for (const RunOfMaker& run : m_runsOfMakers) {
        if (run.layout) {
            noteRunOf(run.layout, run, true);
            continue;
        }
        for (PropertyKeyID key : run.keys) {
            auto found = layoutsWithAppendedKey.find(key.bits());
            if (found == layoutsWithAppendedKey.end())
                continue;
            for (LayoutID layout : found->value)
                noteRunOf(layout, run, false);
            break;
        }
    }

    // The object in the field has the names of all that had its holder in their hands, the innermost first.
    Vector<const RunOnObjectInField*, 8> runsOnField;
    for (const RunOnObjectInField& last : m_runsOnObjectsInFields) {
        runsOnField.shrink(0);
        for (const RunOnObjectInField* run = &last; run && !runsOnField.contains(run);) {
            runsOnField.append(run);
            uint32_t placePlusOne = run->givenBy ? m_runsOnFieldsOfWhatFunctionsReturn.get(static_cast<uint64_t>(run->givenBy.index() + 1) << 32 | run->keyInHolder.bits()) : 0;
            run = placePlusOne ? &m_runsOnObjectsInFields[placePlusOne - 1] : nullptr;
        }
        RunOfMaker whole;
        LayoutID layout;
        for (const RunOnObjectInField* run : runsOnField | std::views::reverse) {
            layout = layout ? layout : run->layout;
            for (PropertyKeyID key : run->keys) {
                if (!whole.keys.contains(key))
                    whole.keys.append(key);
            }
        }
        if (layout) {
            noteRunOf(layout, whole, true);
            continue;
        }
        for (PropertyKeyID key : last.keys) {
            auto found = layoutsWithAppendedKey.find(key.bits());
            if (found == layoutsWithAppendedKey.end())
                continue;
            for (LayoutID candidate : found->value)
                noteRunOf(candidate, whole, false);
            break;
        }
    }

    std::ranges::stable_sort(runs, { }, &RunOfLayout::layout);
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

// `createPropertyAccessExpression()` stores on what `createBaseDeclaration()` gives, which has it from `createBaseNode()`, which has it from `baseFactory.createBaseNode()`: one of four functions, and each constructs a `Node`.
// So the run speaks of the members below `Node` and of none below `Token`. It follows what each body returns, and an argument that a body returns as it came. A guess: the order of keys is looked at where it counts.
void WriterOfLinkLayouts::writeRunsBehindTaggedMakers()
{
    if (m_flow.taggedMakers.isEmpty())
        return;

    // The edges of a body are gone over once, however many rows lead through it.
    struct WaysThroughBody {
        Vector<uint32_t, 2> returned;
        // The join, then what flows into it. Sorted.
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

    // A row is short. One that is not says nothing worth its price.
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
            // The table may grow below, so what is found in it is copied out first.
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
        for (FunctionID callee : callees.listed)
            followCallee(callee);
        for (uint32_t shared : callees.shared) {
            for (FunctionID callee : m_flow.calleesOfSharedCallsAsSettled()[shared].listed)
                followCallee(callee);
        }
        // Nobody knows the receiver: whatever some body has laid into a field under the name.
        if (call.callee == SummaryCall::Callee::Name && call.key && callees.listed.isEmpty() && callees.shared.isEmpty()) {
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

        // As writeRunsOfMakers() has it: the object has the names of all that had it in their hands, the innermost first.
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

// Whatever lays a property on an object under a name that Link does not follow shows as GainsProperty: a computed key, a copy, a definition by a descriptor, a name that got no field.
void WriterOfLinkLayouts::writeAppendedFields()
{
    for (uint32_t index = 0; index < m_tables.layouts.size(); ++index) {
        LayoutID id(index);
        if (hasEscaped(index) || m_tables.layouts[index].possibleChanges.contains(LayoutChange::GainsProperty))
            continue;
        std::span<const PropertyKeyID> keys = m_tables.appendedKeysOf(id);
        for (uint32_t place = 0; place < keys.size(); ++place) {
            // Without a node no access has come to the layout, by itself or by a family, and what a store through a lost value brings was followed no further.
            FlowNodeID node = m_flow.findAppendedField(id, keys[place]);
            if (!node)
                continue;
            LinkedCell& cell = m_tables.appendedFields[m_tables.firstAppendedFieldOfLayout[index] + place];
            cell.everyWriterIsKnown = true;
            cell.proven = m_flow.typeAt(node).isNothing() ? TypeID(TypeTable::never) : claimedTypeAt(node);
        }
    }
}

// The setter runs, or there is none and nothing is stored. Whoever lays another property in front of the accessor defines it, or changes what lies above: each is a change of its own.
bool WriterOfLinkLayouts::comesToAccessorAbove(LayoutID layout, PropertyKeyID key) const
{
    if (m_flow.namesMayBeIncomplete(layout, ClassSide::Prototype))
        return false;
    LinkFlow::FoundMembers members = m_flow.findMembers(layout, ClassSide::Prototype, key);
    // As AccessInLinkAccesses::resolveOnLayout(): until the function is linked below the class its objects find nothing there.
    if (members.lookupGoesOnAboveClasses())
        return false;
    return std::ranges::any_of(members, [](const LinkFlow::FoundMember& member) {
        return member.kind != MethodKind::Method;
    });
}

void WriterOfLinkLayouts::lookAtMembers(const ThinSummary& summary)
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

// An access on a lost closure reads anything and stores into unseen code. So nobody follows what is done to a lost class object, or to the prototype object that is read off it.
void WriterOfLinkLayouts::noteLostClassObjects()
{
    auto note = [&](LayoutID classLayout) {
        m_liesBelowLostClassObject.quickSet(classLayout.index());
        for (LayoutID below : m_program.layoutsExpectedBelow(classLayout))
            m_liesBelowLostClassObject.quickSet(below.index());
    };
    // Of a constructor function nothing but `prototype` is followed, and whoever asks a lost closure for that lets them all escape.
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

// The run time refuses every new name on it, but those that a test run has opened.
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
    // The entry that allocates for `new` stands at no node. It is an entry of the constructor: a class that is sent without one is there for the types that name it.
    case LayoutKind::Instance:
        if (!layout.constructor || !m_flow.reached.get(layout.constructor.index()))
            return false;
        // Every entry of the constructor is listed, and each is the `super()` of a class below: LinkFlow::allocatedLayouts has the class otherwise.
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
        linked.family = m_flow.families.nearestCommonFamily(id, id);
        linked.isAllocated = mayBeAllocated(id);
        linked.possibleChanges = LinkedLayout { }.possibleChanges;
        linked.listsEveryMemberAbove = !m_flow.namesMayBeIncomplete(id, ClassSide::Prototype);
        linked.listsEveryStaticMember = !m_flow.namesMayBeIncomplete(id, ClassSide::Static);
        linked.isFilledUnderComputedKeys = m_isFilledUnderComputedKeys.quickGet(index);
        linked.heritage = m_heritages[index];
        if (m_asksWhatMakersGive && !m_layoutsMadeInSeveralFunctions.quickGet(index))
            linked.onlyFunctionThatMakesObjects = m_onlyFunctionsThatMakeObjects[index];
        if (hasEscaped(index) || !isFoundByShapeTest(m_program.at(id)))
            continue;

        if (m_flow.reading() == WhatUnseenCodeDoes::Anything)
            linked.reachesUnseenCode = false;
        linked.possibleChanges = m_changes[index];
        linked.possibleChanges.add(m_flow.possibleChangesOf(index));
        // The walk goes by the layout, not by the name: to it the object may have got an accessor, a property more, and a field that refuses stores.
        if (m_flow.layoutsDefinedOnUnderWrittenKeys.get(index))
            linked.possibleChanges.add(changesMadeByInLinkLayouts(SummaryChange::Kind::DefinesByDescriptor));
        // What is done through a lost value counts in either reading.
        linked.isRedefinedByLiteralsAlone = m_flow.layoutsWithKnownAccessors.get(index) && !linked.possibleChanges.contains(LayoutChange::GetsAccessor)
            && !(m_flow.lostLayouts.get(index) && m_changesOfLostFamilies[lostFamilyOf(id)].contains(LayoutChange::GetsAccessor));
        if (m_flow.layoutsWithKnownAccessors.get(index))
            linked.possibleChanges.add(LayoutChange::GetsAccessor);
        // Which member it was done to nobody knows. A proof charges all of them. What is held is what is expected of an object of the layout, and the look at the object sees the rest:
        // on a large program strangers meet until most layouts are one family, in which each of the five is done to somebody.
        if (m_flow.lostLayouts.get(index) && m_flow.reading() == WhatUnseenCodeDoes::Anything)
            linked.possibleChanges.add(m_changesOfLostFamilies[lostFamilyOf(id)]);
        if (everyPrototypeMayChange)
            linked.possibleChanges.add(LayoutChange::ChangesPrototype);
    }
}

// What the body returns is the object that it allocates, `this` that `new` has handed it, what its own `super()` answers, or a primitive, in the place of which the construction answers with one of those.
// Every class of a family asks the constructors above it.
bool WriterOfLinkLayouts::answersOnlyWithObjectOfItsConstruction(FunctionID constructor) const
{
    enum : uint8_t { NotAsked, Yes, No };
    if (m_whatConstructorsAnswerWith.isEmpty())
        m_whatConstructorsAnswerWith.fill(NotAsked, m_program.functions.size());
    uint8_t& answer = m_whatConstructorsAnswerWith[constructor.index()];
    if (answer == NotAsked)
        answer = findWhetherConstructorAnswersOnlyWithObjectOfItsConstruction(constructor) ? Yes : No;
    return answer == Yes;
}

bool WriterOfLinkLayouts::findWhetherConstructorAnswersOnlyWithObjectOfItsConstruction(FunctionID constructor) const
{
    const Function& function = m_program.at(constructor);
    const ThinSummary& summary = m_flow.summaries.at(constructor);
    if (!function.hasBody() || summary.has(SummaryTrait::HasDirectEval))
        return false;
    BitVector isSeen(summary.nodes.size());
    Vector<uint32_t, 8> toVisit;
    for (uint32_t node = 0; node < summary.nodes.size(); ++node) {
        if (summary.nodes[node].kind == SummaryNode::Kind::Result && isSeen.set(node) == false)
            toVisit.append(node);
    }
    while (!toVisit.isEmpty()) {
        uint32_t to = toVisit.takeLast();
        for (const SummaryEdge& edge : summary.edges) {
            if (edge.to != to || isSeen.set(edge.from))
                continue;
            const SummaryNode& from = summary.nodes[edge.from];
            switch (from.kind) {
            case SummaryNode::Kind::Join:
                toVisit.append(edge.from);
                break;
            case SummaryNode::Kind::Constant:
                if (m_program.at(TypeID(from.first)).hasObjects())
                    return false;
                break;
            case SummaryNode::Kind::Allocation:
                if (function.kind != FunctionKind::BaseConstructor)
                    return false;
                break;
            case SummaryNode::Kind::Parameter:
                if (!function.takesThisFromConstruction() || function.thisParameterIndex() != std::optional<unsigned>(from.first))
                    return false;
                break;
            // `new X` names no new.target of its own.
            case SummaryNode::Kind::ResultOfCall:
                if (function.kind != FunctionKind::DerivedConstructor || !summary.calls[from.first].isConstruction || summary.calls[from.first].newTarget == noSummaryNode)
                    return false;
                break;
            default:
                return false;
            }
        }
    }
    return true;
}

// `let first; class A { constructor() { if (first) return first; first = this; } }  class B extends A { constructor(v) { super(); this.x = v; } }`: the scan counts every store into what `super()` answers
// as a part of the construction. Where a constructor above may answer with another object than the one that this construction makes, such a store lands in an object that was made long ago.
bool WriterOfLinkLayouts::superCallMayAnswerWithOlderObject(LayoutID id) const
{
    const Layout& layout = m_program.at(id);
    if (layout.kind != LayoutKind::Instance || (!layout.base && !layout.has(LayoutFlags::HasHeritage)))
        return false;
    if (!layout.base)
        return true;
    // `class A extends B {}` beside `class B extends A {}` compiles.
    if (!m_rowsThatMayAnswerWithOlderObject)
        m_rowsThatMayAnswerWithOlderObject.emplace(m_program.layouts.size(), true);
    return m_rowsThatMayAnswerWithOlderObject->answerFor(layout.base, [&](LayoutID above) -> StepAlongRow {
        const Layout& record = m_program.at(above);
        if (!record.constructor || !answersOnlyWithObjectOfItsConstruction(record.constructor))
            return { true, { } };
        if (!record.base)
            return { record.has(LayoutFlags::HasHeritage), { } };
        return { std::nullopt, record.base };
    });
}

// What is said of a cell is gathered at the declaration of the field, because the classes below list it again and a reader may ask either cell. It makes no type.
void WriterOfLinkLayouts::gatherAtDeclarationsOfFields()
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_tables.layouts.size());
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        const Layout& layout = m_program.layouts[index];
        RELEASE_ASSERT(m_tables.firstFieldOfLayout[index + 1] - m_tables.firstFieldOfLayout[index] == layout.fields.size());
        bool hasUnseenWriters = hasEscaped(index) || !onlyCompiledCodeWritesFieldsInLinkLayouts(layout);
        bool isLost = m_flow.lostLayouts.get(index);
        bool isStoredIntoWhenOlder = superCallMayAnswerWithOlderObject(LayoutID(index));
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            FieldReference reference { LayoutID(index), field };
            uint32_t cell = cellOf(reference);
            uint32_t declaration = cellOf(declarationOf(m_program, reference));
            if ((hasUnseenWriters && !isPrivateNameInLink(m_program, layout.fields[field].key)) || m_fieldsWithUnseenWriters.quickGet(cell))
                m_fieldsWithUnseenWriters.quickSet(declaration);
            if (isStoredIntoWhenOlder || m_fieldsWrittenLate.quickGet(cell) || m_flow.isWrittenAfterConstruction(cell) || (isLost && isWrittenLateThroughLost(LayoutID(index), layout.fields[field].key)))
                m_fieldsWrittenLate.quickSet(declaration);
            if (cell != declaration && m_fieldsGivenUndefinedByMakers.quickGet(cell))
                m_fieldsGivenUndefinedByMakers.quickSet(declaration);
            if (cell != declaration && m_fieldsFilledByConstructors.quickGet(cell))
                m_fieldsFilledByConstructors.quickSet(declaration);
        }
    }
}

// A field has one node, that of its declaration (LinkFlow::numberNodes()). So what arrives is typed once, where the field is declared, and each layout is written by itself: AOTLinkTypesOnAllThreads.h.
void WriterOfLinkLayouts::writeFields()
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(m_tables.layouts.size());
    Vector<TypeID> provenAtDeclaration;
    provenAtDeclaration.fill(TypeID(), m_tables.fields.size());
    static constexpr uint8_t somethingArrives = 1;
    static constexpr uint8_t anythingArrives = 2;
    Vector<uint8_t> arrivesAtDeclaration;
    arrivesAtDeclaration.fill(0, m_tables.fields.size());

    writeOnAllThreads(numberOfLayouts, [&](unsigned index) {
        uint32_t numberOfFields = m_tables.firstFieldOfLayout[index + 1] - m_tables.firstFieldOfLayout[index];
        bool isOfProgram = true;
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            FieldReference reference { LayoutID(index), field };
            FieldReference declared = declarationOf(m_program, reference);
            FlowNodeID node = m_flow.field(reference);
            if (declared != reference) {
                RELEASE_ASSERT(node == m_flow.field(declared));
                continue;
            }
            if (m_flow.typeAt(node).isNothing())
                continue;
            uint32_t cell = cellOf(reference);
            TypeID arrived = claimedTypeAt(node);
            provenAtDeclaration[cell] = arrived;
            arrivesAtDeclaration[cell] = arrived ? somethingArrives : somethingArrives | anythingArrives;
            isOfProgram &= isOfProgramInLink(arrived);
        }
        return isOfProgram;
    });

    BitVector hasKnownAccessor;
    hasKnownAccessor.ensureSize(m_tables.fields.size());
    for (FieldReference field : m_flow.fieldsWithKnownAccessors)
        hasKnownAccessor.quickSet(cellOf(field));

    writeOnAllThreads(numberOfLayouts, [&](unsigned index) {
        uint32_t numberOfFields = m_tables.firstFieldOfLayout[index + 1] - m_tables.firstFieldOfLayout[index];
        // A deletion empties the slot, and so does a definition by a descriptor, which moves the field out to the extra properties.
        bool everyConstructionIsSeen = constructionsAreFollowedInLinkLayouts(m_program.layouts[index]) && !hasEscaped(index);
        bool keepsWhatConstructionsFill = everyConstructionIsSeen && !m_tables.layouts[index].possibleChanges.containsAny({ LayoutChange::LosesProperty, LayoutChange::GetsAccessor });
        // `{ raw: 0, get value() { .. } }`: the accessors that literals lay under names of their own take no other field out of its slot.
        bool keepsWhatLiesBesideAccessors = everyConstructionIsSeen && m_tables.layouts[index].isRedefinedByLiteralsAlone && !m_tables.layouts[index].possibleChanges.contains(LayoutChange::LosesProperty);
        // The loader fills all that a data module lists.
        bool isRecordThatIsMade = m_asksWhatMakersGive && m_program.layouts[index].kind == LayoutKind::Record && m_flow.allocatedLayouts.get(index) && !m_isMadeAtNoNode.quickGet(index);
        bool isOfProgram = true;
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            FieldReference reference { LayoutID(index), field };
            uint32_t declaration = cellOf(declarationOf(m_program, reference));
            LinkedCell& cell = m_tables.fields[cellOf(reference)];
            cell.everyWriterIsKnown = !m_fieldsWithUnseenWriters.quickGet(declaration);
            cell.isWrittenAfterConstruction = !cell.everyWriterIsKnown || m_fieldsWrittenLate.quickGet(declaration);
            cell.proven = cell.everyWriterIsKnown && !(arrivesAtDeclaration[declaration] & anythingArrives) ? provenAtDeclaration[declaration] : TypeID();
            // A read of an environment's field is typed by this alone, and a read that gives `never` ends its block. So of such a field nothing is said where nothing arrives.
            if (cell.everyWriterIsKnown && !(arrivesAtDeclaration[declaration] & somethingArrives) && !m_program.layouts[index].isEnvironment())
                cell.proven = TypeTable::never;
            cell.proven = narrowerOf(cell.proven, fieldAt(m_program, reference).declared);
            // `{ kind: "num", value }`: every value that arrives has been listed by an allocation or by constant data, all of them list one and the same, and nothing is stored there by name.
            // A PROOF, in Facts::linked() alone: what a slot is laid out for and held to stays the kind of value.
            if (uint32_t unit = m_unitsListedForFields[cellOf(reference)]; unit && unit != otherThanOneUnit && m_flow.reading() == WhatUnseenCodeDoes::Anything && fieldAt(m_program, reference).key && m_keysOfTestedFields.get(fieldAt(m_program, reference).key.index()) && cellOf(reference) == declaration && cell.proven && !cell.isWrittenAfterConstruction && m_program.layouts[index].kind == LayoutKind::Record) {
                TypeID written = typeOfWrittenUnit(WrittenUnit::fromBits(unit));
                if (types().isSubset(written, cell.proven))
                    cell.proven = written;
            }
            // An allocation may list the mark of an empty cell as the value of a field. Where no store brings anything there is no object to speak of.
            const Field& record = fieldAt(m_program, reference);
            bool liesBesideAccessors = keepsWhatLiesBesideAccessors && !hasKnownAccessor.quickGet(cellOf(reference)) && !hasKnownAccessor.quickGet(declaration);
            cell.isFilled = (keepsWhatConstructionsFill || liesBesideAccessors || (everyConstructionIsSeen && isPrivateNameInLink(m_program, record.key))) && record.key && record.form.isEmpty() && !m_flow.fieldsThatMayBeEmpty.get(cellOf(reference))
                && (arrivesAtDeclaration[declaration] & somethingArrives) && !(m_flow.typeAt(m_flow.field(reference)).primitives & Primitive::Absent);
            cell.isOfBirth = constructionsAreFollowedInLinkLayouts(m_program.layouts[index]) && m_flow.allocatedLayouts.get(index) && record.key && record.form.isEmpty() && !m_flow.fieldsNotOfBirth.get(cellOf(reference));
            cell.isFilledAtEveryRead = cell.isFilled && !m_fieldsReadInFrontOfDefinition.quickGet(cellOf(reference))
                && !(m_flow.lostLayouts.get(index) && m_keysReadInFrontOfDefinitionThroughLost.contains(record.key.bits()));
            cell.isLeftUndefinedByMakers = m_layoutsGivenUndefinedByMakers.quickGet(index) && m_fieldsGivenUndefinedByMakers.quickGet(declaration) && !m_fieldsFilledByConstructors.quickGet(declaration);
            // `{ id, get extra() { .. } }`: the literal gives the field, as a pair of functions.
            cell.isGivenByNoMaker = isRecordThatIsMade && !m_fieldsGivenUndefinedByMakers.quickGet(declaration) && !m_fieldsFilledByConstructors.quickGet(declaration)
                && !hasKnownAccessor.quickGet(cellOf(reference)) && !hasKnownAccessor.quickGet(declaration);
            cell.isFilledByAccessorsAlone = m_program.layouts[index].kind == LayoutKind::Record && record.form.isEmpty() && hasKnownAccessor.quickGet(cellOf(reference))
                && !m_fieldsGivenValues.quickGet(cellOf(reference)) && !m_fieldsGivenValues.quickGet(declaration);
            cell.holdsAccessorFromBirth = cell.isFilledByAccessorsAlone && keepsWhatLiesBesideAccessors && m_flow.reading() == WhatUnseenCodeDoes::Anything && m_flow.allocatedLayouts.get(index)
                && !m_flow.fieldsThatMayBeEmpty.get(cellOf(reference)) && !m_fieldsReadInFrontOfDefinition.quickGet(cellOf(reference))
                && !(m_flow.lostLayouts.get(index) && m_keysReadInFrontOfDefinitionThroughLost.contains(record.key.bits()));
            isOfProgram &= isOfProgramInLink(cell.proven);
        }
        return isOfProgram;
    });
}

thread_local TypesOfThreadInLink WriterOfLinkLayouts::s_typesOfThread;

void WriterOfLinkLayouts::writeVariables()
{
    m_tables.orderOfScopes = m_flow.orderOfScopes;
    writeOnAllThreads(static_cast<unsigned>(m_tables.variables.size()), [&](unsigned index) {
        LinkedCell& cell = m_tables.variables[index];
        cell.everyWriterIsKnown = !m_variablesSharedWithUnseenCode.quickGet(index) && !m_flow.escapedVariables.get(index);
        cell.isWrittenAfterConstruction = !cell.everyWriterIsKnown || m_variablesWrittenLate.quickGet(index);
        cell.isStoredInto = !cell.everyWriterIsKnown || m_variablesStoredInto.quickGet(index);
        cell.isFilled = cell.isFilledAtEveryRead = m_flow.variablesStoredFirst.get(index);
        const DeclaredType& declared = m_program.variables[index].declared;
        cell.proven = typeEstablishedOf(declared);
        if (!cell.everyWriterIsKnown)
            return isOfProgramInLink(cell.proven);
        // What the variable holds at the start has arrived there. A type without `absent` says that no read comes in front of the first store: only the compiler can know that.
        cell.proven = narrowerOf(claimedTypeAt(m_flow.variable(VariableID(index))), declared);
        // As WriterOfLink::holdsOnlyInstancesThatStayAsMade().
        const LinkType& arrived = m_flow.typeAt(m_flow.variable(VariableID(index)));
        if (m_flow.reading() != WhatUnseenCodeDoes::Anything || arrived.classes != LinkType::Classes::Site || arrived.objects != LinkType::Objects::None || arrived.functions != LinkType::Functions::None
            || arrived.holdsUnseen || arrived.holdsLostBuiltins || arrived.builtin || m_flow.hasEscaped(arrived.site))
            return isOfProgramInLink(cell.proven);
        FlowNodeID underUnknownNames = m_flow.findContentsOf(arrived, SlotOfContents::underUnknownNames());
        cell.staysAsItsClassMakesIt = !underUnknownNames || (underUnknownNames != m_flow.unseen() && m_flow.typeAt(underUnknownNames).isNothing());
        return isOfProgramInLink(cell.proven);
    });
}

// No operation writes a capture. NewClosure fills it, and a direct call passes it as an entry parameter.
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
    m_tables.indexMembersByKey();

    for (auto [laid, taken] : m_namesLaidOutOfMaps) {
        auto name = m_namesWrittenOnPrototypes.find(laid);
        auto accessor = m_accessorsWrittenInMaps.find(taken);
        if (accessor != m_accessorsWrittenInMaps.end() && accessor->value.isWrittenOnce && !name->value.isWrittenAgain)
            name->value = accessor->value;
    }

    for (const LinkFlow::NameOnPrototype& name : m_flow.namesOnPrototypes()) {
        if (!name.isDefinedByDescriptor && (!name.isStoredInto || !name.key))
            continue;
        LinkedNameOnPrototype linked { name.layout, name.key, FunctionID() };
        const LinkType& held = m_flow.typeAt(name.stored);
        LinkType oneClosure;
        oneClosure.functions = LinkType::Functions::Closure;
        oneClosure.function = held.function;
        if (held == oneClosure && !hasEscaped(name.layout.index()))
            linked.function = held.function;
        oneClosure.function = m_flow.typeAt(name.storedInSight).function;
        if (m_flow.typeAt(name.storedInSight) == oneClosure && !name.isDefinedByDescriptor)
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
    // The names of which the flow has no record.
    for (const auto& [word, written] : m_namesWrittenOnPrototypes) {
        LinkedNameOnPrototype linked { LayoutID(static_cast<uint32_t>(word >> 32)), PropertyKeyID::fromBits(static_cast<uint32_t>(word)), FunctionID() };
        if (written.isWrittenOnce) {
            linked.expected = written.method;
            linked.expectedGetter = written.getter;
            linked.expectedSetter = written.setter;
        }
        m_tables.namesOnPrototypes.append(linked);
    }
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
        if (!m_program.at(id).constructedClass || !m_program.at(id).takesThisFromConstruction())
            continue;
        const ThinSummary& summary = m_flow.summaries.at(id);
        for (const SummaryAccess& access : summary.accesses) {
            if (PropertyKeyID key = findNameThatConstructorStores(id, summary, access))
                m_namesThatConstructorsStore.add(static_cast<uint64_t>(m_program.at(id).constructedClass.index()) << 32 | key.bits());
        }
    }

    // THE EDGES BESIDE THE RECORDS. What both say of a cell or a layout is only ever added, so the edges say it in tables of their own, which are united with the others behind the two.
    m_saidByEdges.fieldsGivenValues.ensureSize(m_tables.fields.size());
    m_saidByEdges.fieldsGivenUndefinedByMakers.ensureSize(m_tables.fields.size());
    m_saidByEdges.fieldsFilledByConstructors.ensureSize(m_tables.fields.size());
    m_saidByEdges.fieldsWrittenLate.ensureSize(m_tables.fields.size());
    m_saidByEdges.layoutsGivenUndefinedByMakers.ensureSize(m_program.layouts.size());
    m_saidByEdges.unitsListedForFields.fill(0, m_tables.fields.size());
    LinkFlow::ViewOfHeldReading* view = LinkFlow::ViewAtWork::current();
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
            LinkFlow::ViewAtWork viewAtWork(view);
            for (size_t index : m_flow.reached)
                lookAtEdges(FunctionID(index), m_flow.summaries.at(FunctionID(index)));
        },
    });
    m_fieldsGivenValues.merge(m_saidByEdges.fieldsGivenValues);
    m_fieldsGivenUndefinedByMakers.merge(m_saidByEdges.fieldsGivenUndefinedByMakers);
    m_fieldsFilledByConstructors.merge(m_saidByEdges.fieldsFilledByConstructors);
    m_fieldsWrittenLate.merge(m_saidByEdges.fieldsWrittenLate);
    m_layoutsGivenUndefinedByMakers.merge(m_saidByEdges.layoutsGivenUndefinedByMakers);
    for (size_t cell = 0; cell < m_unitsListedForFields.size(); ++cell) {
        uint32_t listed = m_saidByEdges.unitsListedForFields[cell];
        if (uint32_t& unit = m_unitsListedForFields[cell]; listed && unit != listed)
            unit = unit ? otherThanOneUnit : listed;
    }
    m_saidByEdges = { };
}

// Behind the look, in front of the turn at the table of types.
void WriterOfLinkLayouts::writeWhatMakesNoType(BitVector&& functionsWhoseCallersAreAllKnown)
{
    m_functionsWhoseCallersAreAllKnown = WTF::move(functionsWhoseCallersAreAllKnown);
    appendFields();
    writeRunsOfMakers();
    writeRunsBehindTaggedMakers();
    gatherAtDeclarationsOfFields();
}

void WriterOfLinkLayouts::write(TypeArena& types)
{
    m_types = &types;
    m_typesTurned = { };
    // WriterOfLink::writeFunctions() has said of which constructors every caller is known.
    writeLayouts();
    writeFields();
    writeAppendedFields();
    writeAccessorsExpectedInFields(m_flow, m_tables);
    writeVariables();
    writeCaptures();
    writeMembers();
    writeWhosePrototypePropertyStaysAsMade();
    writeNamesOnFunctions();
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
#include "AOTRowsOfLayouts.h"

#endif // ENABLE(AOT)
