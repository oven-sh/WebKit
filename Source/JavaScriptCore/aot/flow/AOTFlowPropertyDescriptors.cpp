/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyAccessInternal.h"

#if ENABLE(AOT)

#include "AOTValueIdentity.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FlowDescriptorFields {
    FlowNodeID value;
    FlowNodeID getter;
    FlowNodeID setter;
    FlowNodeID writable;
    FlowNodeID enumerable;
    FlowNodeID configurable;
};

struct FlowDescriptorMapContext {
    FlowSolver& solver;
    const FlowAccess& access;
    const FlowNewObjects& receivers;
    FlowNodeID descriptorMap;
};

using FlowLocationList = Vector<FlowNodeID, 8>;

} // namespace

static FlowAccess createLoad(const FlowAccess& access, FlowNodeID receiver, FlowNodeID result)
{
    return { access.site, access.origin, FlowAccessKind::Load, FlowReadUse::Value, access.flags & FlowAccessFlag::IsInCycle, receiver, result, { }, access.thrown };
}

static void loadDescriptorField(FlowSolver& solver, const FlowAccess& access, FlowNodeID descriptor, const FlowNewObjects& descriptors, PropertyKeyID key, FlowNodeID result)
{
    accessProperties(solver, createLoad(access, descriptor, result), descriptors, key ? FlowKeySet::name(key) : FlowKeySet::everyString());
}

static FlowDescriptorFields loadDescriptorFields(FlowSolver& solver, const FlowAccess& access, FlowNodeID descriptor)
{
    uint32_t index = descriptor.index();
    FlowDescriptorFields fields {
        solver.temporaryFor(propertyTemporaryKey(access.site, FlowPropertyRole::DescriptorValue, index)),
        solver.temporaryFor(propertyTemporaryKey(access.site, FlowPropertyRole::DescriptorGetter, index)),
        solver.temporaryFor(propertyTemporaryKey(access.site, FlowPropertyRole::DescriptorSetter, index)),
        solver.temporaryFor(propertyTemporaryKey(access.site, FlowPropertyRole::DescriptorWritable, index)),
        solver.temporaryFor(propertyTemporaryKey(access.site, FlowPropertyRole::DescriptorEnumerable, index)),
        solver.temporaryFor(propertyTemporaryKey(access.site, FlowPropertyRole::DescriptorConfigurable, index)),
    };

    FlowNewObjects fresh = solver.takeNewObjects(propertyTemporaryKey(access.site, FlowPropertyRole::DescriptorObjects, index), descriptor);
    const FlowPropertyKeys& keys = solver.propertyState().keys;
    loadDescriptorField(solver, access, descriptor, fresh, keys.value, fields.value);
    loadDescriptorField(solver, access, descriptor, fresh, keys.get, fields.getter);
    loadDescriptorField(solver, access, descriptor, fresh, keys.set, fields.setter);
    loadDescriptorField(solver, access, descriptor, fresh, keys.writable, fields.writable);
    loadDescriptorField(solver, access, descriptor, fresh, keys.enumerable, fields.enumerable);
    loadDescriptorField(solver, access, descriptor, fresh, keys.configurable, fields.configurable);
    return fields;
}

// A descriptor that has get or set defines no value. One that has neither reads undefined under both.
static bool mayBeDataDescriptor(const FlowSolver& solver, const FlowDescriptorFields& fields)
{
    return (solver.valueOf(fields.getter).primitives & Primitive::Undefined) && (solver.valueOf(fields.setter).primitives & Primitive::Undefined);
}

// A slot holds a writable, enumerable, configurable data property. An attribute that the descriptor lacks reads undefined, and a property that the definition creates gets false for it.
static bool mayLackAttribute(const FlowSolver& solver, const FlowDescriptorFields& fields)
{
    auto mayBeFalsy = [&](FlowNodeID attribute) {
        return !!(solver.valueOf(attribute).primitives & ~Primitive::True);
    };
    return mayBeFalsy(fields.writable) || mayBeFalsy(fields.enumerable) || mayBeFalsy(fields.configurable);
}

static bool isAffectedByRedefinition(const FlowWalkInputs& inputs, FlowObjectID receiver, FlowNodeID location)
{
    switch (inputs.graph.kindOf(location)) {
    case FlowNodeKind::Variable:
    case FlowNodeKind::DeclaredPart:
    case FlowNodeKind::Position:
    case FlowNodeKind::IndexedProperties:
        return true;
    case FlowNodeKind::Property:
        // The names that are stored on the objects of a layout become fields of it.
        return inputs.objects.hasLayout(receiver) || enforcedTypeOf(inputs.graph, location);
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
        return inputs.records.recordOf(location).events.contains(FlowLocationEvent::InitializedAtAllocation);
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isOwnMemberOfInstances(const Program& program, BuiltinClassID builtinClass, PropertyKeyID name)
{
    const BuiltinTable& builtins = program.builtins();
    BuiltinID prototype = builtinClass ? builtins.at(builtinClass).prototype : BuiltinID();
    BuiltinName builtinName = program.builtinNameOf(name);
    if (!prototype || !builtinName)
        return false;

    BuiltinID member = builtins.findOwnMember(prototype, builtinName);
    return member && builtins.at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
}

static void collectIndexedLocations(const FlowAccessContext& context, const FlowWalkInputs& inputs, FlowObjectID object, FlowLocationList& locations)
{
    FlowIndexedStorage storage = forEachIndexedLocation(inputs, object, context.keys.indexes, [&](const FlowIndexedLocation& location) {
        locations.append(location.node);
    });

    // An index that gets an accessor or stops being writable costs a typed object its shape.
    if (storage == FlowIndexedStorage::OpenPart)
        locations.append(context.solver.ensureOpenPart(object, FlowOpenPart::IndexedProperties));
}

static void collectUnnamedLocations(const FlowAccessContext& context, const FlowWalkInputs& inputs, const FlowHolder& holder, FlowLocationList& locations)
{
    FlowSolver& solver = context.solver;
    const FlowKeySet& keys = context.keys;
    if (keys.mayNameIndex())
        collectIndexedLocations(context, inputs, holder.object, locations);
    if (!keys.hasUnlistedNames())
        return;

    if (holder.object == unknownFlowObject) {
        if (keys.anyString)
            locations.append(solver.ensureOpenPart(unknownFlowObject, FlowOpenPart::StringProperties));
        if (keys.anySymbol)
            locations.append(solver.ensureOpenPart(unknownFlowObject, FlowOpenPart::SymbolProperties));
        return;
    }

    solver.waitOnProperties(holder.object, context.access.site);
    forEachOwnProperty(inputs, holder, [&](const FlowOwnProperty& own) {
        if (!own.key || keys.mayName(own.key, inputs.program))
            locations.append(own.value);
    });
}

static void noteRedefinitions(const FlowAccessContext& context, FlowObjectID receiver)
{
    FlowSolver& solver = context.solver;
    if (solver.isAboutToBeAbandoned(context.access.site))
        return;

    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    FlowHolder holder = holderOf(solver.objects(), receiver);
    if (mutabilityOf(inputs, holder) == FlowHolderMutability::Immutable)
        return;

    BuiltinClassID builtinClass = solver.objects().at(receiver).builtinClass;
    // A key that nobody can name may be the key of a field that only other objects of the layout are seen to get.
    bool isRedefined = context.keys.hasUnlistedNames() && solver.objects().hasLayout(receiver);
    FlowLocationList locations;
    for (PropertyKeyID name : context.keys.names) {
        FlowOwnProperty own = findOwnProperty(inputs, holder, name);
        locations.appendList({ own.value, own.getter, own.setter });
        isRedefined |= isOwnMemberOfInstances(inputs.program, builtinClass, name);
    }
    collectUnnamedLocations(context, inputs, holder, locations);
    solver.charge(context.access.site, 1 + locations.size());

    for (FlowNodeID location : locations) {
        if (!location)
            continue;
        if (receiver != unknownFlowObject && !isAffectedByRedefinition(inputs, receiver, location))
            continue;
        solver.noteLocationEvent(location, FlowLocationEvent::Redefined);
        keepLowerWitness(solver.records().ensureLocationRecord(location).firstLoss, context.witness);
        isRedefined = true;
    }
    if (isRedefined)
        solver.noteMutation(receiver, ObjectMutation::RedefineProperty, context.witness);
}

void noteAttributeChanges(FlowSolver& solver, const FlowAccess& access, const FlowNewObjects& receivers, const FlowKeySet& keys)
{
    FlowAccessContext context { solver, access, keys, solver.witnessAt(access.site, access.origin) };
    // What is noted on the unknown object holds for every object that has reached native code.
    std::optional<FlowNewObjects> remaining = receiversBesideUnknownObject(solver, receivers, access.receiver);
    solver.forEachMember(remaining ? *remaining : receivers, [&](FlowObjectID receiver) {
        noteRedefinitions(context, receiver);
    });
}

namespace {

enum class FlowDescriptorForm : uint8_t {
    IsObject = 1 << 0,
    MayDefineValue = 1 << 1,
    MayLackAttribute = 1 << 2,
    MayDefineGetter = 1 << 3,
    MayDefineSetter = 1 << 4,
    MayLackGetter = 1 << 5,
    MayLackSetter = 1 << 6,
};

constexpr OptionSet<FlowDescriptorForm> getterForms { FlowDescriptorForm::MayDefineGetter, FlowDescriptorForm::MayLackGetter };
constexpr OptionSet<FlowDescriptorForm> setterForms { FlowDescriptorForm::MayDefineSetter, FlowDescriptorForm::MayLackSetter };

} // anonymous namespace

static OptionSet<FlowDescriptorForm> formOf(const FlowSolver& solver, FlowNodeID descriptor, const FlowDescriptorFields& fields)
{
    OptionSet<FlowDescriptorForm> form;
    if (!solver.valueOf(descriptor).mayBeObject())
        return form;
    form.add(FlowDescriptorForm::IsObject);
    if (mayBeDataDescriptor(solver, fields))
        form.add(FlowDescriptorForm::MayDefineValue);
    if (mayLackAttribute(solver, fields))
        form.add(FlowDescriptorForm::MayLackAttribute);
    if (solver.valueOf(fields.getter).mayBeObject())
        form.add(FlowDescriptorForm::MayDefineGetter);
    if (solver.valueOf(fields.setter).mayBeObject())
        form.add(FlowDescriptorForm::MayDefineSetter);
    if (solver.valueOf(fields.getter).primitives)
        form.add(FlowDescriptorForm::MayLackGetter);
    if (solver.valueOf(fields.setter).primitives)
        form.add(FlowDescriptorForm::MayLackSetter);
    return form;
}

// Every run defines a data property, or every run defines an accessor with the same halves.
static bool definesOneKindOfProperty(OptionSet<FlowDescriptorForm> form)
{
    bool mayDefineAccessor = form.containsAny({ FlowDescriptorForm::MayDefineGetter, FlowDescriptorForm::MayDefineSetter });
    if (form.contains(FlowDescriptorForm::MayDefineValue))
        return !mayDefineAccessor;
    return !form.containsAll(getterForms) && !form.containsAll(setterForms);
}

void definePropertiesFromDescriptor(FlowSolver& solver, const FlowAccess& access, const FlowNewObjects& receivers, const FlowKeySet& keys, FlowNodeID descriptor)
{
    if (!descriptor || keys.isEmpty())
        return;

    FlowDescriptorFields fields = loadDescriptorFields(solver, access, descriptor);
    for (FlowNodeID field : { descriptor, fields.getter, fields.setter, fields.writable, fields.enumerable, fields.configurable })
        solver.waitOn(field, access.site);

    // Values reach the receivers over edges. Most of what arrives in a field of the descriptor decides nothing: only another form brings back the receivers that have come out.
    OptionSet<FlowDescriptorForm> form = formOf(solver, descriptor, fields);
    auto knownForm = solver.propertyState().formsOfDescriptors.add({ access.site, descriptor }, form.toRaw());
    if (!knownForm.isNewEntry && knownForm.iterator->value != form.toRaw()) {
        knownForm.iterator->value = form.toRaw();
        solver.revisit(access.site);
    }

    FlowAccess define = access;
    if (!definesOneKindOfProperty(form))
        define.completion = FlowStoreCompletion::MayLeaveStoreOut;
    if (form.contains(FlowDescriptorForm::MayDefineValue)) {
        define.kind = FlowAccessKind::Define;
        define.value = fields.value;
        accessProperties(solver, define, receivers, keys);
    }

    if (form.contains(FlowDescriptorForm::MayDefineGetter)) {
        define.kind = FlowAccessKind::DefineGetter;
        define.value = fields.getter;
        accessProperties(solver, define, receivers, keys);
    }
    if (form.contains(FlowDescriptorForm::MayDefineSetter)) {
        define.kind = FlowAccessKind::DefineSetter;
        define.value = fields.setter;
        accessProperties(solver, define, receivers, keys);
    }

    // Last: the operation may run twice, so what it defines is among what it may find.
    if (form.contains(FlowDescriptorForm::MayLackAttribute))
        noteAttributeChanges(solver, access, receivers, keys);
}

static void defineFromMapSource(const FlowDescriptorMapContext& context, FlowObjectID map, const FlowPropertySource& source)
{
    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;
    FlowKeySet keys = source.key ? keySetOf(solver.program(), source.key) : FlowKeySet::everyKey();
    FlowNodeID descriptors = solver.temporaryFor(source.key
        ? propertyTemporaryKey(access.site, FlowPropertyRole::DescriptorOfMapKey, source.key.index())
        : propertyTemporaryKey(access.site, FlowPropertyRole::DescriptorOfUnnamedMapKeys, context.descriptorMap.index()));

    switch (source.kind) {
    case FlowPropertySourceKind::Value:
        solver.noteLocationEvent(source.node, valueReadEvents);
        solver.connect(source.node, descriptors);
        break;
    case FlowPropertySourceKind::Unknown:
    case FlowPropertySourceKind::Getter:
    case FlowPropertySourceKind::BuiltinMember:
    case FlowPropertySourceKind::BuiltinGetter: {
        FlowAccess load = createLoad(access, context.descriptorMap, descriptors);
        loadFromSource(FlowAccessContext { solver, load, keys, solver.witnessAt(access.site, access.origin) }, map, source);
        break;
    }
    case FlowPropertySourceKind::Absent:
    case FlowPropertySourceKind::Setter:
    case FlowPropertySourceKind::BuiltinSetter:
        return;
    }
    definePropertiesFromDescriptor(solver, access, context.receivers, keys, descriptors);
}

static constexpr OptionSet<ObjectMutation> mutationsChangingAttributes = reachMutations | OptionSet<ObjectMutation> { ObjectMutation::RedefineProperty, ObjectMutation::ChangeIntegrity };

// A property that a literal or a store has created is writable, enumerable and configurable, and so is what its descriptor defines.
static bool hasAttributesOfStoredProperty(const FlowDescriptorMapContext& context, FlowObjectID described)
{
    FlowSolver& solver = context.solver;
    if (solver.objects().kindOf(described) != FlowObjectKind::Allocation)
        return false;
    solver.waitOnMutations(described, context.access.site, mutationsChangingAttributes);
    return !solver.records().possibleMutations(described).containsAny(mutationsChangingAttributes);
}

static void defineAsDescribed(const FlowDescriptorMapContext& context, FlowNodeID describedObjects, FlowObjectID described, const FlowPropertySource& source)
{
    FlowSolver& solver = context.solver;
    FlowKeySet keys = source.key ? keySetOf(solver.program(), source.key) : FlowKeySet::everyKey();
    FlowAccess define = context.access;
    switch (source.kind) {
    case FlowPropertySourceKind::Value:
        // A read creates the node of an unnamed key, so only a node that holds a value makes this a definition under keys nobody can name.
        if (!source.key && solver.valueOf(source.node).isEmpty()) {
            solver.waitOn(source.node, define.site, FlowWakeUp::Revisit);
            return;
        }
        solver.noteLocationEvent(source.node, valueReadEvents);
        define.kind = FlowAccessKind::Define;
        define.value = source.node;
        break;
    case FlowPropertySourceKind::Getter:
        define.kind = FlowAccessKind::DefineGetter;
        define.value = source.node;
        break;
    case FlowPropertySourceKind::Setter:
        define.kind = FlowAccessKind::DefineSetter;
        define.value = source.node;
        break;
    case FlowPropertySourceKind::Unknown:
    case FlowPropertySourceKind::BuiltinMember:
    case FlowPropertySourceKind::BuiltinGetter:
    case FlowPropertySourceKind::BuiltinSetter: {
        // Nobody follows what the descriptor holds: a value, or functions that the receivers then call.
        FlowNodeID descriptors = solver.temporaryFor(propertyTemporaryKey(define.site, FlowPropertyRole::DescriptorOfUnnamedMapKeys, describedObjects.index()));
        solver.joinUnknownObject(descriptors, solver.witnessAt(define.site, define.origin));
        definePropertiesFromDescriptor(solver, define, context.receivers, keys, descriptors);
        return;
    }
    case FlowPropertySourceKind::Absent:
        return;
    }
    accessProperties(solver, define, context.receivers, keys);
    if (!hasAttributesOfStoredProperty(context, described))
        noteAttributeChanges(solver, define, context.receivers, keys);
}

static ValueID findOnlyUserOf(const Function& function, ValueID value)
{
    ValueID onlyUser;
    bool hasOtherUse = false;
    auto isAmong = [&](const Target& target) {
        return target.arguments.contains(value);
    };
    for (const Block& block : function.blocks) {
        hasOtherUse |= block.subject == value || (block.handler && isAmong(*block.handler)) || std::ranges::any_of(block.terminator.targets, isAmong);
        block.terminator.forEachOperand([&](ValueID used, uint32_t) {
            hasOtherUse |= used == value;
        });
        for (ValueID user : block.values) {
            function.at(user).forEachOperand([&](ValueID used, uint32_t) {
                if (used != value)
                    return;
                hasOtherUse |= !!onlyUser;
                onlyUser = user;
            });
        }
    }
    return hasOtherUse ? ValueID() : onlyUser;
}

// The map goes from the call that makes it straight into this operation: nobody has read a descriptor out of it, and nobody has changed it.
static FlowNodeID findObjectsDescribedBy(const FlowDescriptorMapContext& context, FlowObjectID map)
{
    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;
    FlowNodeID describedObjects = solver.propertyState().objectsDescribedByMap.get({ map, access.site.instance });
    if (!describedObjects)
        return { };

    const Function& function = solver.functionOf(access.site);
    ValueID user = findOnlyUserOf(function, solver.objects().at(solver.objects().at(map).site).description.value);
    while (user && user != access.origin && isCast(function.at(user).opcode))
        user = findOnlyUserOf(function, user);
    return user == access.origin ? describedObjects : FlowNodeID();
}

// A map that Object.getOwnPropertyDescriptors has made gives the receivers the own properties of the objects it describes, a value as a value and an accessor as an accessor.
static void defineAsDescribed(const FlowDescriptorMapContext& context, FlowNodeID describedObjects)
{
    FlowSolver& solver = context.solver;
    solver.waitOn(describedObjects, context.access.site, FlowWakeUp::Revisit);
    FlowPropertyWalk walk { .operation = FlowWalkOperation::CopyDataProperties, .waiter = context.access.site };
    solver.valueOf(describedObjects).objects.forEach([&](FlowObjectID described) {
        solver.noteUse(described, FlowObjectUse::Enumerated);
        forEachPropertySource(solver, described, FlowKeySet::everyKey(), walk, [&](const FlowPropertySource& source) {
            defineAsDescribed(context, describedObjects, described, source);
        });
    });
}

void definePropertiesFromDescriptors(FlowSolver& solver, const FlowAccess& access, const FlowNewObjects& receivers, FlowNodeID descriptorMap)
{
    if (!descriptorMap)
        return;

    // One operation reads every key of the map and every descriptor, so an accessor it calls may run many times.
    FlowAccess repeatedAccess = access;
    repeatedAccess.flags.add(FlowAccessFlag::IsInCycle);
    repeatedAccess.completion = FlowStoreCompletion::MayLeaveStoreOut;

    solver.waitOn(descriptorMap, access.site, FlowWakeUp::Revisit);
    FlowDescriptorMapContext context { solver, repeatedAccess, receivers, descriptorMap };
    FlowPropertyWalk walk { .operation = FlowWalkOperation::CopyDataProperties, .waiter = access.site, .isEnumerableOnly = true };
    FlowObjectSet maps = solver.valueOf(descriptorMap).objects;
    maps.forEach([&](FlowObjectID map) {
        solver.noteUse(map, FlowObjectUse::Enumerated);
    });
    // The unknown object as a map defines anything under every key, and so whatever a map that has reached native code defines.
    if (std::optional<FlowNewObjects> remaining = holdersBesideUnknownObject(solver, maps, { }))
        maps = WTF::move(*remaining);
    maps.forEach([&](FlowObjectID map) {
        if (FlowNodeID describedObjects = findObjectsDescribedBy(context, map)) {
            defineAsDescribed(context, describedObjects);
            return;
        }
        forEachPropertySource(solver, map, FlowKeySet::everyKey(), walk, [&](const FlowPropertySource& source) {
            defineFromMapSource(context, map, source);
        });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
