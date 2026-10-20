/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyLocations.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTFlowNamespaces.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowParts.h"
#include "AOTFlowPropertyState.h"
#include "AOTLayout.h"
#include <optional>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

using OwnProperties = Vector<FlowOwnProperty, 16>;

static FlowOwnProperty makeOwnProperty(FlowOwnLookup lookup, PropertyKeyID key)
{
    FlowOwnProperty property;
    property.lookup = lookup;
    property.key = key;
    return property;
}

static FlowOwnPresence presenceOf(const FlowWalkInputs& inputs, FlowNodeID value)
{
    if (!value)
        return FlowOwnPresence::MayBeAbsent;

    switch (inputs.graph.kindOf(value)) {
    case FlowNodeKind::DeclaredPart:
    case FlowNodeKind::Variable:
        return FlowOwnPresence::ShownByValue;
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::Position:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
    case FlowNodeKind::Property:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        break;
    }

    OptionSet<FlowLocationEvent> events = inputs.records.recordOf(value).events;
    if (events.contains(FlowLocationEvent::InitializedAtAllocation) && !events.contains(FlowLocationEvent::MissingAtAllocation))
        return FlowOwnPresence::ShownByValue;
    return FlowOwnPresence::MayBeAbsent;
}

static FlowOwnProperty propertyOf(const FlowWalkInputs& inputs, const FlowProperty& entry)
{
    FlowOwnProperty property = makeOwnProperty(FlowOwnLookup::Found, entry.key);
    property.presence = presenceOf(inputs, entry.value);
    property.value = entry.value;
    property.getter = entry.getter;
    property.setter = entry.setter;
    return property;
}

static FlowOwnProperty propertyOfEntry(const FlowWalkInputs& inputs, FlowObjectID object, PropertyKeyID key, FlowOwnLookup whenMissing)
{
    const FlowProperty* entry = inputs.graph.findProperty(object, key);
    if (!entry)
        return makeOwnProperty(whenMissing, key);
    return propertyOf(inputs, *entry);
}

static void addAccessorsOfEntry(const FlowWalkInputs& inputs, FlowObjectID object, FlowOwnProperty& property)
{
    const FlowProperty* entry = inputs.graph.findProperty(object, property.key);
    if (!entry)
        return;
    property.getter = entry->getter;
    property.setter = entry->setter;
}

static FlowOwnProperty propertyOfDeclaredLocation(const FlowWalkInputs& inputs, FlowObjectID object, PropertyKeyID key, FlowNodeID location)
{
    FlowOwnProperty property = makeOwnProperty(FlowOwnLookup::Found, key);
    property.presence = FlowOwnPresence::ShownByValue;
    property.value = location;
    addAccessorsOfEntry(inputs, object, property);
    return property;
}

static bool isInheritedField(const Program& program, LayoutID layout, PropertyKeyID key)
{
    std::optional<FieldReference> reference = findField(program, layout, key, BaseChain::Predicted);
    return reference && reference->layout != layout;
}

static void seedCreationProperty(FlowSolver& solver, FlowObjectID object, PropertyKeyID key, FlowObjectID value)
{
    FlowNodeID node = solver.ensurePropertyValue(object, key);
    solver.joinObject(node, value);
    solver.noteLocationEvent(node, FlowLocationEvent::InitializedAtAllocation);
}

FlowWalkInputs FlowWalkInputs::of(FlowSolver& solver)
{
    return { solver.program(), solver.objects(), solver.graph(), solver.records(), solver.propertyState(), &solver };
}

FlowWalkInputs FlowWalkInputs::of(const FlowSolution& solution)
{
    return { solution.program, solution.objects, solution.graph, solution.records, solution.propertyState, nullptr };
}

FlowHolder holderOf(const FlowObjectTable& objects, FlowObjectID object)
{
    RELEASE_ASSERT(object);
    const FlowObject& record = objects.at(object);
    switch (record.kind) {
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return { object, record.builtin };
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::ModuleNamespace:
        return { object, { } };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowHolder holderOf(const FlowWalkInputs& inputs, BuiltinID builtin)
{
    BuiltinID canonical = inputs.builtins().canonicalID(builtin);
    if (inputs.isSolving())
        return { ensureBuiltinObject(*inputs.solver, canonical), canonical };
    return { inputs.objects.findBuiltin(canonical), canonical };
}

FlowHolderMutability mutabilityOf(const FlowWalkInputs& inputs, const FlowHolder& holder)
{
    if (holder.builtin)
        return inputs.builtins().at(holder.builtin).flags.contains(BuiltinFlag::IsLockedObject) ? FlowHolderMutability::Immutable : FlowHolderMutability::Ordinary;

    const FlowObject& record = inputs.objects.at(holder.object);
    switch (record.kind) {
    case FlowObjectKind::ModuleNamespace:
        return FlowHolderMutability::Immutable;
    case FlowObjectKind::Allocation:
        return inputs.program.at(record.layout).isEnvironment() ? FlowHolderMutability::Immutable : FlowHolderMutability::Ordinary;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        return FlowHolderMutability::Ordinary;
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Neither writable nor configurable, unlike the one of a constructor function, which is writable.
static bool isPrototypePropertyOfClass(const FlowWalkInputs& inputs, const FlowHolder& holder, PropertyKeyID key)
{
    if (!holder.object || key != inputs.state.keys.prototype)
        return false;
    const FlowObject& record = inputs.objects.at(holder.object);
    if (record.kind != FlowObjectKind::Closure)
        return false;
    FunctionKind kind = inputs.program.at(record.function).kind;
    return kind == FunctionKind::BaseConstructor || kind == FunctionKind::DerivedConstructor;
}

bool isPrototypePropertyOfFunction(const FlowWalkInputs& inputs, const FlowHolder& holder, PropertyKeyID key)
{
    if (!holder.object || key != inputs.state.keys.prototype)
        return false;
    const FlowObject& record = inputs.objects.at(holder.object);
    return record.kind == FlowObjectKind::Closure && hasPrototypeProperty(inputs.program, inputs.program.at(record.function));
}

FlowHolderMutability mutabilityOf(const FlowWalkInputs& inputs, const FlowHolder& holder, PropertyKeyID key)
{
    if (holder.builtin && key && isLockedLocation(inputs.program, holder.builtin, key))
        return FlowHolderMutability::Immutable;
    if (isPrototypePropertyOfClass(inputs, holder, key))
        return FlowHolderMutability::Immutable;
    return mutabilityOf(inputs, holder);
}

FlowHolderMutability mutabilityOfIndexes(const FlowWalkInputs& inputs, const FlowHolder& holder)
{
    if (holder.builtin && inputs.builtins().closesIndexKeys(holder.builtin))
        return FlowHolderMutability::Immutable;
    return mutabilityOf(inputs, holder);
}

FlowHolderMutability mutabilityOfPrototypeLink(const FlowWalkInputs& inputs, const FlowHolder& holder)
{
    if (holder.builtin && inputs.builtins().hasLockedPrototypeLink(holder.builtin))
        return FlowHolderMutability::Immutable;
    return mutabilityOf(inputs, holder);
}

static FlowOwnProperty findPropertyOfLayout(const FlowWalkInputs& inputs, FlowObjectID object, const FlowObject& record, PropertyKeyID key)
{
    if (inputs.program.at(record.layout).isEnvironment())
        return makeOwnProperty(FlowOwnLookup::Absent, key);

    FlowFieldLocation field = findFieldLocation(inputs.graph, object, key);
    if (field.lookup == FlowFieldLookup::Found)
        return propertyOfDeclaredLocation(inputs, object, key, field.node);

    if (inputs.isSolving() && isInheritedField(inputs.program, record.layout, key))
        inputs.solver->ensurePropertyValue(object, key);

    // What a constructor stores on every path before its object leaks is there for whoever reads the location. The refinement of the constructions marks the other locations as missing.
    FlowOwnProperty property = propertyOfEntry(inputs, object, key, FlowOwnLookup::Absent);
    if (!property.value || !inputs.records.eventsOf(object).contains(FlowObjectEvent::AllocatedByConstructor))
        return property;
    if (!inputs.records.recordOf(property.value).events.contains(FlowLocationEvent::MissingAtAllocation))
        property.presence = FlowOwnPresence::ShownByValue;
    return property;
}

static FlowOwnProperty findAllocationProperty(const FlowWalkInputs& inputs, FlowObjectID object, const FlowObject& record, PropertyKeyID key)
{
    FlowOwnProperty property = findPropertyOfLayout(inputs, object, record, key);
    if (!inputs.objects.isMadeByBuiltinConstructor(object) || classOf(inputs.program, key) == FlowKeyClass::PrivateName)
        return property;

    // The constructor of a built-in class defines own properties on the object it makes for a subclass, and they come before everything on a prototype.
    const BuiltinTable& builtins = inputs.builtins();
    BuiltinClassID builtinClass = inputs.objects.at(record.site).description.builtinClass;
    BuiltinName name = inputs.program.builtinNameOf(key);
    if (BuiltinID member = builtinClass && name ? builtins.findOwnPropertyOfInstances(builtinClass, name) : BuiltinID()) {
        property.lookup = FlowOwnLookup::Found;
        property.presence = FlowOwnPresence::ShownByValue;
        property.builtin = member;
        return property;
    }
    if (!builtinClass || builtins.mayDefineOnNewInstances(builtinClass, name))
        property.lookup = FlowOwnLookup::Unknown;
    return property;
}

static FlowOwnProperty findClosureProperty(const FlowWalkInputs& inputs, FlowObjectID object, const FlowObject& record, PropertyKeyID key)
{
    if (VariableID variable = findStaticFieldVariable(inputs.program, record.function, key))
        return propertyOfDeclaredLocation(inputs, object, key, inputs.graph.variable(variable));

    if (key != inputs.state.keys.prototype)
        return propertyOfEntry(inputs, object, key, FlowOwnLookup::Absent);

    if (!inputs.isSolving())
        return propertyOfEntry(inputs, object, key, hasPrototypeProperty(inputs.program, inputs.program.at(record.function)) ? FlowOwnLookup::Unknown : FlowOwnLookup::Absent);

    if (FlowObjectID prototype = ensurePrototypeObject(*inputs.solver, object))
        seedCreationProperty(*inputs.solver, object, key, prototype);
    return propertyOfEntry(inputs, object, key, FlowOwnLookup::Absent);
}

static FlowOwnProperty findPrototypeProperty(const FlowWalkInputs& inputs, FlowObjectID object, const FlowObject& record, PropertyKeyID key)
{
    bool isCreatedWithConstructor = !inputs.program.at(record.function).has(FunctionFlags::IsGenerator);
    if (key != inputs.state.keys.constructor || !isCreatedWithConstructor)
        return propertyOfEntry(inputs, object, key, FlowOwnLookup::Absent);

    if (!inputs.isSolving())
        return propertyOfEntry(inputs, object, key, FlowOwnLookup::Unknown);

    seedCreationProperty(*inputs.solver, object, key, record.owner);
    return propertyOfEntry(inputs, object, key, FlowOwnLookup::Absent);
}

FlowOwnProperty findOwnProperty(const FlowWalkInputs& inputs, const FlowHolder& holder, PropertyKeyID key)
{
    RELEASE_ASSERT(key);
    if (holder.builtin)
        return findOwnBuiltinProperty(inputs, holder, key);

    FlowObjectID object = holder.object;
    const FlowObject record = inputs.objects.at(object);
    switch (record.kind) {
    case FlowObjectKind::Unknown: {
        FlowOwnProperty property = propertyOfEntry(inputs, object, key, FlowOwnLookup::Unknown);
        property.lookup = FlowOwnLookup::Unknown;
        return property;
    }
    case FlowObjectKind::Allocation:
        return findAllocationProperty(inputs, object, record, key);
    case FlowObjectKind::Closure:
        return findClosureProperty(inputs, object, record, key);
    case FlowObjectKind::Prototype:
        return findPrototypeProperty(inputs, object, record, key);
    case FlowObjectKind::Instance:
        return propertyOfEntry(inputs, object, key, FlowOwnLookup::Absent);
    case FlowObjectKind::Opaque:
        return propertyOfEntry(inputs, object, key, FlowOwnLookup::Unknown);
    case FlowObjectKind::ModuleNamespace:
        return findNamespaceExport(inputs, object, key);
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The table does not say which members are writable. The specification creates these so.
static constexpr bool isCreatedWritable(BuiltinKind kind)
{
    switch (kind) {
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Method:
        return true;
    case BuiltinKind::Namespace:
    case BuiltinKind::Prototype:
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool stopsStoresFromBelow(const FlowWalkInputs& inputs, const FlowHolder& holder, PropertyKeyID name)
{
    constexpr OptionSet<ObjectMutation> mutationsMakingReadOnly { ObjectMutation::RedefineProperty, ObjectMutation::ChangeIntegrity };
    // Only a descriptor makes one property read-only, and no operation that names its location takes a descriptor.
    constexpr OptionSet<FlowLocationEvent> eventsMakingReadOnly { FlowLocationEvent::WrittenByUnknownCode, FlowLocationEvent::Redefined, FlowLocationEvent::StoredWithoutNamingLocation };

    FlowOwnProperty own = findOwnProperty(inputs, holder, name);
    if (own.lookup == FlowOwnLookup::Unknown || own.getter || own.setter)
        return true;
    if (own.builtin && !isCreatedWritable(inputs.builtins().at(own.builtin).kind))
        return true;
    if (own.value && inputs.records.recordOf(own.value).events.containsAny(eventsMakingReadOnly))
        return true;
    if (!own.value && !own.builtin)
        return false;
    return holder.object && inputs.records.possibleMutations(holder.object).containsAny(mutationsMakingReadOnly);
}

static void appendEntries(const FlowWalkInputs& inputs, FlowObjectID object, OwnProperties& properties, NOESCAPE const Invocable<bool(const FlowProperty&)> auto& shouldVisit)
{
    for (const FlowProperty& entry : inputs.graph.propertiesOf(object)) {
        if (shouldVisit(entry))
            properties.append(propertyOf(inputs, entry));
    }
}

static void appendEveryEntry(const FlowWalkInputs& inputs, FlowObjectID object, OwnProperties& properties)
{
    appendEntries(inputs, object, properties, [](const FlowProperty&) {
        return true;
    });
}

static FlowOwnListing appendCreationProperty(const FlowWalkInputs& inputs, const FlowHolder& holder, PropertyKeyID key, OwnProperties& properties)
{
    FlowOwnProperty property = findOwnProperty(inputs, holder, key);
    switch (property.lookup) {
    case FlowOwnLookup::Unknown:
        return FlowOwnListing::MayHaveOthers;
    case FlowOwnLookup::Absent:
        return FlowOwnListing::Complete;
    case FlowOwnLookup::Found:
        properties.append(property);
        return FlowOwnListing::Complete;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void collectAllocationProperties(const FlowWalkInputs& inputs, FlowObjectID object, const FlowObject& record, OwnProperties& properties)
{
    const Layout& layout = inputs.program.at(record.layout);
    if (layout.isEnvironment())
        return;

    for (uint32_t index = 0; index < record.numberOfDeclaredParts; ++index) {
        if (PropertyKeyID key = layout.fields[index].key)
            properties.append(propertyOfDeclaredLocation(inputs, object, key, inputs.graph.field(object, index)));
    }
    appendEntries(inputs, object, properties, [&](const FlowProperty& entry) {
        return !entry.value || inputs.graph.kindOf(entry.value) != FlowNodeKind::DeclaredPart;
    });
}

static void appendStaticFields(const FlowWalkInputs& inputs, FlowObjectID object, FunctionID classConstructor, OwnProperties& properties)
{
    LayoutID layout = inputs.program.at(classConstructor).constructedClass;
    if (!layout)
        return;

    for (const StaticField& field : inputs.program.at(layout).staticFields) {
        FlowNodeID variable = inputs.graph.variable(field.variable);
        if (field.key) {
            properties.append(propertyOfDeclaredLocation(inputs, object, field.key, variable));
            continue;
        }
        FlowOwnProperty property = makeOwnProperty(FlowOwnLookup::Found, { });
        property.value = variable;
        properties.append(property);
    }
}

static FlowOwnListing collectClosureProperties(const FlowWalkInputs& inputs, const FlowHolder& holder, const FlowObject& record, OwnProperties& properties)
{
    appendStaticFields(inputs, holder.object, record.function, properties);

    PropertyKeyID prototypeKey = inputs.state.keys.prototype;
    FlowOwnListing listing = appendCreationProperty(inputs, holder, prototypeKey, properties);
    appendEntries(inputs, holder.object, properties, [&](const FlowProperty& entry) {
        return entry.key != prototypeKey && !findStaticFieldVariable(inputs.program, record.function, entry.key);
    });
    return listing;
}

static FlowOwnListing collectPrototypeProperties(const FlowWalkInputs& inputs, const FlowHolder& holder, OwnProperties& properties)
{
    PropertyKeyID constructorKey = inputs.state.keys.constructor;
    FlowOwnListing listing = appendCreationProperty(inputs, holder, constructorKey, properties);
    appendEntries(inputs, holder.object, properties, [&](const FlowProperty& entry) {
        return entry.key != constructorKey;
    });
    return listing;
}

FlowOwnListing forEachOwnProperty(const FlowWalkInputs& inputs, const FlowHolder& holder, NOESCAPE const FlowOwnPropertyFunctor& functor)
{
    if (holder.builtin)
        return forEachOwnBuiltinProperty(inputs, holder, functor);

    FlowObjectID object = holder.object;
    const FlowObject record = inputs.objects.at(object);
    OwnProperties properties;
    FlowOwnListing listing = FlowOwnListing::MayHaveOthers;
    switch (record.kind) {
    case FlowObjectKind::Unknown:
        appendEveryEntry(inputs, object, properties);
        for (FlowOwnProperty& property : properties)
            property.lookup = FlowOwnLookup::Unknown;
        break;
    case FlowObjectKind::Allocation:
        collectAllocationProperties(inputs, object, record, properties);
        if (!inputs.objects.isMadeByBuiltinConstructor(object))
            listing = FlowOwnListing::Complete;
        break;
    case FlowObjectKind::Closure:
        listing = collectClosureProperties(inputs, holder, record, properties);
        break;
    case FlowObjectKind::Prototype:
        listing = collectPrototypeProperties(inputs, holder, properties);
        break;
    case FlowObjectKind::Instance:
        appendEveryEntry(inputs, object, properties);
        listing = FlowOwnListing::Complete;
        break;
    case FlowObjectKind::Opaque:
        appendEveryEntry(inputs, object, properties);
        break;
    case FlowObjectKind::ModuleNamespace:
        return forEachNamespaceExport(inputs, object, functor);
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        RELEASE_ASSERT_NOT_REACHED();
    }

    for (const FlowOwnProperty& property : properties)
        functor(property);
    return listing;
}

FlowOwnListing ownListingOf(const FlowWalkInputs& inputs, const FlowHolder& holder)
{
    if (!holder.builtin) {
        switch (inputs.objects.kindOf(holder.object)) {
        case FlowObjectKind::Unknown:
        case FlowObjectKind::Opaque:
            return FlowOwnListing::MayHaveOthers;
        case FlowObjectKind::Instance:
            return FlowOwnListing::Complete;
        case FlowObjectKind::Allocation:
            return inputs.objects.isMadeByBuiltinConstructor(holder.object) ? FlowOwnListing::MayHaveOthers : FlowOwnListing::Complete;
        case FlowObjectKind::Closure:
        case FlowObjectKind::Prototype:
        case FlowObjectKind::ModuleNamespace:
        case FlowObjectKind::Builtin:
        case FlowObjectKind::ModuleBuiltin:
            break;
        }
    }
    return forEachOwnProperty(inputs, holder, [](const FlowOwnProperty&) { });
}

FlowNodeID ensureOwnValue(FlowSolver& solver, FlowObjectID holder, PropertyKeyID key)
{
    RELEASE_ASSERT(classOf(solver.program(), key) != FlowKeyClass::Index);

    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    FlowHolder ownHolder = holderOf(solver.objects(), holder);
    if (mutabilityOf(inputs, ownHolder, key) == FlowHolderMutability::Immutable)
        return { };
    if (FlowNodeID value = findOwnProperty(inputs, ownHolder, key).value)
        return value;
    return solver.ensurePropertyValue(holder, key);
}

FlowNodeID ensureOwnAccessor(FlowSolver& solver, FlowObjectID holder, PropertyKeyID key, MethodKind kind)
{
    if (mutabilityOf(FlowWalkInputs::of(solver), holderOf(solver.objects(), holder), key) == FlowHolderMutability::Immutable)
        return { };

    switch (kind) {
    case MethodKind::Getter:
        return solver.ensureGetter(holder, key);
    case MethodKind::Setter:
        return solver.ensureSetter(holder, key);
    case MethodKind::Method:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<FlowOpenPart> unnamedKeyPartOf(FlowNodeKind kind)
{
    switch (kind) {
    case FlowNodeKind::IndexedProperties:
        return FlowOpenPart::IndexedProperties;
    case FlowNodeKind::StringProperties:
        return FlowOpenPart::StringProperties;
    case FlowNodeKind::SymbolProperties:
        return FlowOpenPart::SymbolProperties;
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Variable:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::DeclaredPart:
    case FlowNodeKind::Position:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::Property:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowKeySet keysHeldBy(FlowOpenPart unnamedKeyPart)
{
    switch (unnamedKeyPart) {
    case FlowOpenPart::StringProperties:
        return FlowKeySet::everyString();
    case FlowOpenPart::IndexedProperties:
        return FlowKeySet::index(arrayIndexRange);
    case FlowOpenPart::SymbolProperties: {
        FlowKeySet keys;
        keys.anySymbol = true;
        return keys;
    }
    case FlowOpenPart::PrototypeLink:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void forEachIndexKeyField(const FlowWalkInputs& inputs, FlowObjectID holder, Range indexes, NOESCAPE const ScopedLambda<void(const FlowIndexedLocation&)>& functor)
{
    if (!inputs.objects.hasLayout(holder))
        return;

    const FlowObject record = inputs.objects.at(holder);
    const Layout& layout = inputs.program.at(record.layout);
    if (layout.isEnvironment())
        return;

    for (uint32_t index = 0; index < record.numberOfDeclaredParts; ++index) {
        PropertyKeyID key = layout.fields[index].key;
        if (!key)
            continue;
        std::optional<uint32_t> arrayIndex = arrayIndexOf(inputs.program, key);
        if (arrayIndex && indexes.contains(*arrayIndex))
            functor(FlowIndexedLocation { inputs.graph.field(holder, index), key });
    }
}

FlowIndexedStorage forEachIndexedLocation(const FlowWalkInputs& inputs, FlowObjectID holder, Range indexes, NOESCAPE const ScopedLambda<void(const FlowIndexedLocation&)>& functor)
{
    BuiltinClassID builtinClass = inputs.objects.at(holder).builtinClass;

    forEachElementPart(inputs.graph, holder, indexes, [&](FlowNodeID node) {
        functor(FlowIndexedLocation { node, { } });
    });
    forEachIndexKeyField(inputs, holder, indexes, functor);

    return indexedElementSlotOf(inputs.builtins(), builtinClass) ? FlowIndexedStorage::ElementParts : FlowIndexedStorage::OpenPart;
}

bool isCreatedWithCharacters(const FlowWalkInputs& inputs, FlowObjectID object)
{
    const FlowObject& record = inputs.objects.at(object);
    BuiltinClassID builtinClass = record.builtinClass;
    if (!builtinClass && inputs.objects.isMadeByBuiltinConstructor(object))
        builtinClass = inputs.objects.at(record.site).description.builtinClass;
    return builtinClass && inputs.builtins().definesCharactersOnNewInstances(builtinClass);
}

VariableID findStaticFieldVariable(const Program& program, FunctionID classConstructor, PropertyKeyID key)
{
    LayoutID layout = program.at(classConstructor).constructedClass;
    if (!layout || !key)
        return { };

    for (const StaticField& field : program.at(layout).staticFields) {
        if (field.key == key)
            return field.variable;
    }
    return { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
