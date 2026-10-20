/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyLocations.h"

#if ENABLE(AOT)

#include "AOTFlowCallOperations.h"
#include "AOTFlowModules.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowParts.h"
#include "AOTFlowPropertyState.h"
#include "AOTFlowPrototypes.h"
#include "AOTFlowUnseenCode.h"
#include "AOTLayout.h"
#include <array>
#include <optional>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct NamedLocation {
    PropertyKeyID key;
    FlowNodeID node;
};

struct AliasedLocations {
    Vector<NamedLocation, 16> named;
    Vector<FlowNodeID, 2> computedStaticFields;
};

constexpr std::array unnamedKeyParts { FlowOpenPart::IndexedProperties, FlowOpenPart::StringProperties, FlowOpenPart::SymbolProperties };

} // anonymous namespace

static bool isImmutable(FlowSolver& solver, FlowObjectID object)
{
    return mutabilityOf(FlowWalkInputs::of(solver), holderOf(solver.objects(), object)) == FlowHolderMutability::Immutable;
}

static bool isImmutable(const FlowWalkInputs& inputs, FlowObjectID object, PropertyKeyID key)
{
    return mutabilityOf(inputs, holderOf(inputs.objects, object), key) == FlowHolderMutability::Immutable;
}

bool sharesLocationsWithUnknownObject(const FlowWalkInputs& inputs, FlowObjectID object)
{
    if (!object || object == unknownFlowObject)
        return false;
    if (inputs.hasReachedNativeCode(object))
        return true;
    if (inputs.state.constructorsHeldAsUnknownValues.isEmpty())
        return false;
    const FlowObject& record = inputs.objects.at(object);
    return record.kind == FlowObjectKind::Builtin && inputs.builtins().at(record.builtin).kind == BuiltinKind::Constructor;
}

bool aliasesUnknownObject(const FlowWalkInputs& inputs, const FlowHolder& holder)
{
    return sharesLocationsWithUnknownObject(inputs, holder.object) && mutabilityOf(inputs, holder) == FlowHolderMutability::Ordinary;
}

static void forEachEscapedObject(FlowSolver& solver, NOESCAPE const Invocable<void(FlowObjectID)> auto& functor)
{
    FlowObjectSet escapedObjects = solver.valueOf(solver.graph().nativeCode()).objects;
    for (FlowObjectID constructor : solver.propertyState().constructorsHeldAsUnknownValues)
        escapedObjects.add(constructor);
    escapedObjects.forEach([&](FlowObjectID object) {
        if (object != unknownFlowObject)
            functor(object);
    });
}

static void connectIntoLocation(FlowSolver& solver, FlowNodeID source, FlowNodeID location)
{
    if (location)
        solver.connect(source, location, enforcedTypeOf(solver.graph(), location));
}

// A delete puts undefined into the node it goes through, and a deleted field reads undefined whatever its type.
static TypeID withUndefined(FlowSolver& solver, TypeID enforcedType)
{
    return enforcedType ? solver.types().unionOf(enforcedType, TypeTable::undefined) : TypeID();
}

static void connectUnnamedKeys(FlowSolver& solver, FlowObjectID holder, FlowNodeID unnamedKeys, FlowNodeID location)
{
    if (!location)
        return;

    TypeID enforcedType = enforcedTypeOf(solver.graph(), location);
    solver.connect(unnamedKeys, location, withUndefined(solver, enforcedType));
    solver.noteLocationEvent(location, overwrittenLocationEvents);
    if (!enforcedType || solver.program().at(enforcedType).isAnything())
        return;

    // A value the filter rejects stays in the unnamed-key node, and a named read looks there only on a holder with this mutation.
    solver.noteLocationEvent(location, FlowLocationEvent::StoredOutsideType);
    solver.noteMutation(holder, ObjectMutation::StoreOfOtherType, noWitness);
}

static void widenToDeclaredType(FlowSolver& solver, FlowNodeID location, const Witness& witness)
{
    TypeID type = enforcedTypeOf(solver.graph(), location);
    solver.join(location, type ? solver.values().valueOfType(type) : solver.values().unknownValue(), witness);
    solver.noteLocationEvent(location, overwrittenLocationEvents);
}

static Vector<FlowNodeID, 16> collectLocationsUnder(FlowSolver& solver, FlowObjectID holder, FlowOpenPart part)
{
    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    Vector<FlowNodeID, 16> locations;

    if (part == FlowOpenPart::IndexedProperties) {
        forEachIndexedLocation(inputs, holder, arrayIndexRange, [&](const FlowIndexedLocation& location) {
            locations.append(location.node);
        });
        return locations;
    }

    forEachOwnProperty(inputs, holderOf(solver.objects(), holder), [&](const FlowOwnProperty& property) {
        if (!property.value || (property.key && isImmutable(inputs, holder, property.key)))
            return;
        if (!property.key || unnamedKeyPartOf(classOf(inputs.program, property.key)) == part)
            locations.append(property.value);
    });
    return locations;
}

static AliasedLocations collectAliasedLocations(FlowSolver& solver, FlowObjectID object)
{
    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    AliasedLocations locations;

    forEachOwnProperty(inputs, holderOf(solver.objects(), object), [&](const FlowOwnProperty& property) {
        if (!property.value)
            return;
        if (!property.key) {
            locations.computedStaticFields.append(property.value);
            return;
        }
        if (classOf(inputs.program, property.key) == FlowKeyClass::Index || isImmutable(inputs, object, property.key))
            return;
        if (inputs.graph.objectOf(property.value) == object || inputs.graph.kindOf(property.value) == FlowNodeKind::Variable)
            locations.named.append(NamedLocation { property.key, property.value });
    });
    return locations;
}

// The members of the text that a typed result has beside its fields are never named like a field, so they are not linked into the fields.
static bool isCreatedWithUnnamedMembers(const FlowSiteDescription& description)
{
    switch (description.kind) {
    case FlowSiteKind::PlainObject:
        return true;
    case FlowSiteKind::Layout:
        return description.origin == FlowSiteOrigin::TypedResult;
    case FlowSiteKind::Closure:
    case FlowSiteKind::Instance:
    case FlowSiteKind::Opaque:
    case FlowSiteKind::PlainArray:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Unknown JavaScript stores the unknown value into every location of every object that native code can reach. What is stored through the unknown object
// has reached native code too, so the unknown value stands for it, and an edge from the unknown object's location would add nothing but work.
static bool isOpenedByUnknownJavaScript(const FlowSolver& solver, FlowObjectID escapedObject)
{
    return solver.records().mayRunUnknownJavaScript() && solver.objects().kindOf(escapedObject) != FlowObjectKind::ModuleNamespace;
}

bool hasUnnamedKeyStores(const FlowWalkInputs& inputs, FlowObjectID holder, FlowOpenPart part)
{
    if (inputs.state.hasLinkedUnnamedKeys(holder, part))
        return true;
    if (part == FlowOpenPart::IndexedProperties && isCreatedWithCharacters(inputs, holder))
        return true;

    AllocationSiteID site = inputs.objects.at(holder).site;
    return site && isCreatedWithUnnamedMembers(inputs.objects.at(site).description) && inputs.graph.find(holder, part);
}

void linkUnnamedKeyStores(FlowSolver& solver, FlowObjectID holder, FlowOpenPart part)
{
    RELEASE_ASSERT(holdsUnnamedKeys(part));
    if (solver.propertyState().setHasLinkedUnnamedKeys(holder, part))
        return;

    // A walk answers by this bit, and a holder that already has the mutations of such a store records no new one.
    solver.revisitMutationWaiters(holder);

    FlowNodeID unnamedKeys = solver.ensureOpenPart(holder, part);

    // This node flows into the unnamed-key node of every escaped object, which links its own locations, so the unknown object's need no edge.
    if (holder == unknownFlowObject) {
        forEachEscapedObject(solver, [&](FlowObjectID escapedObject) {
            linkAliasOfUnnamedKeys(solver, escapedObject, part);
        });
        return;
    }

    for (FlowNodeID location : collectLocationsUnder(solver, holder, part))
        connectUnnamedKeys(solver, holder, unnamedKeys, location);
}

void linkAliasOfUnnamedKeys(FlowSolver& solver, FlowObjectID escapedObject, FlowOpenPart part)
{
    RELEASE_ASSERT(holdsUnnamedKeys(part) && escapedObject != unknownFlowObject);

    FlowNodeID alias = solver.graph().find(unknownFlowObject, part);
    if (!alias || isImmutable(solver, escapedObject))
        return;

    FlowNodeID unnamedKeys = solver.ensureOpenPart(escapedObject, part);
    linkUnnamedKeyStores(solver, escapedObject, part);
    if (isOpenedByUnknownJavaScript(solver, escapedObject))
        return;
    solver.connect(alias, unnamedKeys);

    // A store into the element slot of an unknown receiver lands in the same node, whether or not the slot stands for indexes.
    if (part == FlowOpenPart::IndexedProperties)
        connectUnnamedKeys(solver, escapedObject, alias, contentsNode(solver.graph(), escapedObject, ContentsSlot::Element));
}

void linkAliasOfLocation(FlowSolver& solver, FlowObjectID escapedObject, PropertyKeyID key, FlowNodeID location)
{
    RELEASE_ASSERT(escapedObject != unknownFlowObject);
    if (!location || isOpenedByUnknownJavaScript(solver, escapedObject))
        return;
    FlowNodeID alias = solver.ensurePropertyValue(unknownFlowObject, key);
    solver.connect(alias, location, withUndefined(solver, enforcedTypeOf(solver.graph(), location)));
}

static void linkAliasOfPrototype(FlowSolver& solver, FlowObjectID escapedObject)
{
    FlowNodeID alias = solver.graph().find(unknownFlowObject, FlowOpenPart::PrototypeLink);
    if (!alias || mutabilityOfPrototypeLink(FlowWalkInputs::of(solver), holderOf(solver.objects(), escapedObject)) == FlowHolderMutability::Immutable)
        return;

    // What became the prototype of an object nobody knows has reached native code with it, so the unknown object stands for it.
    // An edge would hand every such prototype to every escaped object, and each lookup on one of them would walk them all.
    Witness witness = solver.records().witnessOf(unknownFlowObject, ObjectMutation::ChangePrototype);
    FlowNodeID link = ensurePrototypeLink(solver, escapedObject);
    solver.joinUnknownObject(link, witness);
    solver.joinPrimitives(link, Primitive::Null);

    // A lookup that passed a built-in holder without a link waits on the holder's mutations alone.
    solver.noteMutation(escapedObject, ObjectMutation::ChangePrototype, witness);
}

static void linkAliasOfInternalParts(FlowSolver& solver, FlowObjectID escapedObject)
{
    const FlowGraph& graph = solver.graph();
    FlowNodeID alias = graph.undeclaredPart(unknownFlowObject);
    if (!alias || isOpenedByUnknownJavaScript(solver, escapedObject))
        return;

    FlowObject record = solver.objects().at(escapedObject);
    switch (record.kind) {
    case FlowObjectKind::Allocation: {
        const Layout& layout = solver.program().at(record.layout);
        for (uint32_t index = 0; index < record.numberOfDeclaredParts; ++index) {
            if (!layout.fields[index].key)
                connectIntoLocation(solver, alias, graph.field(escapedObject, index));
        }
        break;
    }
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque: {
        if (solver.objects().hasCaptures(escapedObject))
            break;
        FlowNodeID indexedElements = graph.slot(escapedObject, indexedElementSlotOf(solver.builtins(), record.builtinClass));
        for (uint32_t index = 0; index < record.numberOfDeclaredParts; ++index) {
            FlowNodeID slot = graph.declaredPart(escapedObject, index);
            if (slot != indexedElements)
                connectIntoLocation(solver, alias, slot);
        }
        break;
    }
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        break;
    }
    connectIntoLocation(solver, alias, graph.undeclaredPart(escapedObject));
}

void didEscape(FlowSolver& solver, FlowObjectID object)
{
    if (object == unknownFlowObject)
        return;

    linkAliasOfInternalParts(solver, object);
    if (isImmutable(solver, object))
        return;

    AliasedLocations locations = collectAliasedLocations(solver, object);
    for (const NamedLocation& location : locations.named)
        linkAliasOfLocation(solver, object, location.key, location.node);

    // A store through the unknown object under any name may hit one of these, and no node stands for all of those names.
    for (FlowNodeID field : locations.computedStaticFields)
        widenToDeclaredType(solver, field, solver.records().witnessOf(object, ObjectMutation::ReachNativeCode));

    for (FlowOpenPart part : unnamedKeyParts)
        linkAliasOfUnnamedKeys(solver, object, part);
    linkAliasOfPrototype(solver, object);

    if (solver.records().eventsOf(unknownFlowObject).contains(FlowObjectEvent::ElementsMoved))
        joinPositions(solver, object, solver.records().witnessOf(unknownFlowObject, FlowObjectEvent::ElementsMoved));
}

static void didCreatePrototypeLink(FlowSolver& solver, FlowObjectID object)
{
    if (object == unknownFlowObject) {
        forEachEscapedObject(solver, [&](FlowObjectID escapedObject) {
            linkAliasOfPrototype(solver, escapedObject);
        });
        return;
    }
    if (sharesLocationsWithUnknownObject(FlowWalkInputs::of(solver), object))
        linkAliasOfPrototype(solver, object);
}

static void didCreateUnnamedKeyPart(FlowSolver& solver, FlowObjectID object, FlowOpenPart part)
{
    // A read through the unknown object answers Unknown, so only a store or a delete creates one of these nodes on it.
    if (object == unknownFlowObject) {
        linkUnnamedKeyStores(solver, unknownFlowObject, part);
        return;
    }

    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    if (part == FlowOpenPart::IndexedProperties && isCreatedWithCharacters(inputs, object))
        solver.joinPrimitives(solver.graph().find(object, part), Primitive::String);
    if (sharesLocationsWithUnknownObject(inputs, object))
        linkAliasOfUnnamedKeys(solver, object, part);
}

static void didCreateProperty(FlowSolver& solver, FlowObjectID object, PropertyKeyID key, FlowNodeID node)
{
    RELEASE_ASSERT(key);
    if (object == unknownFlowObject)
        return;

    // The definition of an accessor marks the value it finds under the name. A value that gets its location after that definition has run shares the name with the accessor all the same.
    const FlowProperty* entry = solver.graph().findProperty(object, key);
    if (entry && (entry->getter || entry->setter)) {
        solver.noteLocationEvent(node, FlowLocationEvent::Redefined);
        solver.noteMutation(object, ObjectMutation::RedefineProperty, noWitness);
    }

    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    if (isImmutable(inputs, object, key))
        return;
    bool isAlias = aliasesUnknownObject(inputs, holderOf(inputs.objects, object));
    bool hasUnnamedStores = hasUnnamedKeyStores(inputs, object, FlowOpenPart::StringProperties) || hasUnnamedKeyStores(inputs, object, FlowOpenPart::SymbolProperties);
    if (!isAlias && !hasUnnamedStores)
        return;

    std::optional<FlowOpenPart> part = unnamedKeyPartOf(classOf(inputs.program, key));
    if (part && hasUnnamedKeyStores(inputs, object, *part))
        connectUnnamedKeys(solver, object, inputs.graph.find(object, *part), node);
    if (isAlias)
        linkAliasOfLocation(solver, object, key, node);
}

// Listed as values of the read, the constructors would be callees of whatever calls that value, Function among them. In native code they would be called with unknown operands, and in a program
// that reads one key nobody can name their members and prototypes would follow. The program can store on one, define on it, delete from it and read what is there, and that is all.
void letConstructorsShareLocationsWithUnknownObject(FlowSolver& solver)
{
    FlowPropertyState& state = solver.propertyState();
    if (!state.constructorsHeldAsUnknownValues.isEmpty())
        return;

    const BuiltinTable& builtins = solver.builtins();
    Vector<FlowObjectID> constructors;
    for (unsigned index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID builtin(index);
        const Builtin& entry = builtins.at(builtin);
        if (entry.kind == BuiltinKind::Constructor && entry.scope == BuiltinScope::Realm && builtins.canonicalID(builtin) == builtin)
            constructors.append(ensureBuiltinObject(solver, builtin));
    }
    state.constructorsHeldAsUnknownValues = constructors;

    for (FlowObjectID constructor : constructors) {
        if (solver.hasReachedNativeCode(constructor))
            continue;
        state.lookups.didChange(constructor);
        solver.revisitMutationWaiters(constructor);
        didEscape(solver, constructor);
        exposeNodesOf(solver, constructor);
    }
}

void didCreateLocation(FlowSolver& solver, FlowObjectID object, PropertyKeyID key, FlowNodeID node)
{
    // What the program puts on a constructor comes back out of a read through the unknown object. The solver does the same for an object in native code.
    if (!solver.hasReachedNativeCode(object) && sharesLocationsWithUnknownObject(FlowWalkInputs::of(solver), object))
        exposeLocation(solver, object, node);

    FlowNodeKind kind = solver.graph().kindOf(node);
    switch (kind) {
    case FlowNodeKind::PrototypeLink:
        didCreatePrototypeLink(solver, object);
        return;
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
        didCreateUnnamedKeyPart(solver, object, *unnamedKeyPartOf(kind));
        return;
    case FlowNodeKind::Property:
        didCreateProperty(solver, object, key, node);
        return;
    case FlowNodeKind::Getter: {
        const FlowObject& record = solver.objects().at(object);
        if (record.kind == FlowObjectKind::ModuleBuiltin && record.builtin == Builtins::ModuleObject)
            didCreateGetterOfModuleObject(solver, object, key, node);
        return;
    }
    case FlowNodeKind::Setter:
        return;
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Variable:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::DeclaredPart:
    case FlowNodeKind::Position:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
