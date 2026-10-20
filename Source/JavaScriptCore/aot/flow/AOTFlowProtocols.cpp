/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowProtocols.h"

#if ENABLE(AOT)

#include "AOTFlowCallState.h"
#include "AOTFlowPropertyState.h"
#include "AOTSchemaOperandIndexes.h"

namespace JSC { namespace AOT { namespace Typed {

static FlowNodeID temporaryOf(FlowSolver& solver, const FlowProtocolRequest& request, FlowCallTemporaryRole role, uint32_t index)
{
    return solver.temporaryFor(callTemporaryKey(request.site, role, index));
}

static FlowNewObjects takeNewObjectsOf(FlowSolver& solver, const FlowProtocolRequest& request, FlowNodeID node)
{
    return solver.takeNewObjects(callTemporaryKey(request.site, FlowCallTemporaryRole::ProtocolOperands, node.index()), node);
}

static FlowNodeID primitiveArgument(FlowSolver& solver, const FlowProtocolRequest& request)
{
    FlowNodeID argument = temporaryOf(solver, request, FlowCallTemporaryRole::MemberValue, request.operand.index());
    solver.joinPrimitives(argument, Primitive::String | Primitive::Number);
    return argument;
}

static FlowNodeID serializedValuesNode(FlowSolver& solver, const FlowProtocolRequest& request)
{
    return temporaryOf(solver, request, FlowCallTemporaryRole::MemberValue, request.operand.index() ^ 0x80000000);
}

static FlowNodeID testedValueArgument(FlowSolver& solver, const FlowProtocolRequest& request)
{
    if (!request.origin)
        return unknownValueNode(solver);
    const Value& operation = solver.operationAt(request.site, request.origin);
    if (operation.opcode != Opcode::InstanceOf)
        return unknownValueNode(solver);
    FlowNodeID tested = solver.graph().nodeOf(request.site.instance, operation.operand(OperandIndex::InstanceOf::value));
    if (!tested)
        return unknownValueNode(solver);

    // The unknown object makes the call generic from its first run, so whatever the tested value holds later reaches the callee by a plain edge.
    FlowNodeID argument = temporaryOf(solver, request, FlowCallTemporaryRole::MemberValue, tested.index());
    solver.joinUnknown(argument, solver.witnessAt(request.site, request.origin));
    solver.connect(tested, argument);
    return argument;
}

static FlowCallRequest memberCallRequest(const FlowProtocolRequest& request, const FlowMemberCall& member)
{
    return {
        request.site,
        request.origin,
        FlowCallLinkKind::Protocol,
        BuiltinCallbackTimes::Once,
        true,
        { .thisValue = request.operand, .arguments = member.arguments, .result = member.result, .thrown = request.thrown },
    };
}

// Nothing in a request says that its operation is outside every cycle.
static FlowAccess loadOf(const FlowProtocolRequest& request, FlowNodeID target)
{
    return {
        .site = request.site,
        .origin = request.origin,
        .kind = FlowAccessKind::Load,
        .flags = FlowAccessFlag::IsInCycle,
        .receiver = request.operand,
        .value = target,
        .thrown = request.thrown,
    };
}

// The ownKeys, getOwnPropertyDescriptor and getPrototypeOf traps of a proxy may throw any value.
static void throwFromTraps(FlowSolver& solver, const FlowProtocolRequest& request, const FlowObjectSet& objects)
{
    const FlowObjectTable& table = solver.objects();
    bool mayBeProxy = objects.anyOf([&](FlowObjectID object) {
        return object == unknownFlowObject || table.kindOf(object) == FlowObjectKind::Opaque || table.isInstanceOf(object, BuiltinClasses::Proxy);
    });
    if (mayBeProxy)
        throwUnknownValue(solver, request.thrown, solver.witnessAt(request.site, request.origin));
}

bool areRunByNativeCode(Protocols protocols, bool resultIsRead)
{
    constexpr Protocols protocolsWithoutResult { Protocol::OwnKeys, Protocol::ToPrimitive, Protocol::PrototypeChain };
    constexpr Protocols protocolsWithResult { Protocol::OwnValues, Protocol::Thenable, Protocol::Iterate, Protocol::Named, Protocol::CallsNamed };
    return protocols.containsOnly(resultIsRead ? protocolsWithoutResult : protocolsWithoutResult | protocolsWithResult);
}

bool isReadByProgram(const FlowSolver& solver, FlowNodeID result)
{
    const FlowGraph& graph = solver.graph();
    return result && result != graph.nativeCode() && result != graph.unknownJavaScript();
}

static void leaveToNativeCode(FlowSolver& solver, const FlowProtocolRequest& request, FlowObjectSet& receivers)
{
    // The fixpoint check hands out every receiver again. Where none was left to native code, each was handled here before it escaped, and is handled here again.
    if (solver.isCheckingFixpoint() && !solver.valueOf(request.operand).hasUnknownObject())
        return;

    FlowObjectSet escaped = receivers;
    escaped.intersect(solver.escapedObjects());
    escaped.remove(unknownFlowObject);
    if (escaped.isEmpty())
        return;

    receivers.subtract(escaped);
    if (request.protocols.contains(Protocol::Thenable))
        solver.noteUseOf(escaped, FlowObjectUse::Awaited);
    if (request.protocols.contains(Protocol::Iterate))
        solver.noteUseOf(escaped, FlowObjectUse::Iterated);
    solver.joinUnknownObject(request.operand, solver.witnessAt(request.site, request.origin));
}

static void runProtocolsOutsideNativeCode(FlowSolver& solver, const FlowProtocolRequest& request)
{
    FlowTemporaryKey key = callTemporaryKey(request.site, FlowCallTemporaryRole::ReceiversOutsideNativeCode, request.operand.index());
    FlowProtocolRequest outside = request;
    outside.operand = solver.temporaryFor(key);

    // No value is inside never. The edge only says that the operand is used as the node that stands in for it.
    solver.connect(request.operand, outside.operand, TypeTable::never);
    solver.join(outside.operand, solver.valueOf(request.operand).withoutObjects());

    FlowObjectSet arrived = solver.takeNewObjects(key, request.operand);
    leaveToNativeCode(solver, outside, arrived);
    solver.joinObjects(outside.operand, arrived);

    // A receiver comes out again after a change, and the change may be that it reached native code.
    FlowNewObjects receivers = takeNewObjectsOf(solver, outside, outside.operand);
    FlowObjectSet remaining = receivers;
    leaveToNativeCode(solver, outside, remaining);
    runProtocolsOn(solver, outside, receivers.withObjects(WTF::move(remaining)));
}

void runProtocols(FlowSolver& solver, const FlowProtocolRequest& request)
{
    if (!request.operand || request.protocols.isEmpty())
        return;

    const FlowValue& value = solver.valueOf(request.operand);
    if (value.objects.isEmpty() && !actOnPrimitives(request.protocols, value.primitives)) {
        solver.waitOn(request.operand, request.site, wakeUpAtFirstReceiver(request.protocols));
        return;
    }

    if (areRunByNativeCode(request.protocols, !request.resultStaysInOperand && isReadByProgram(solver, request.result))) {
        runProtocolsOutsideNativeCode(solver, request);
        return;
    }
    runProtocolsOn(solver, request, takeNewObjectsOf(solver, request, request.operand));
    runProtocolsOnPrimitives(solver, request, solver.valueOf(request.operand).primitives);
}

static bool isForIn(const FlowSolver& solver, const FlowProtocolRequest& request)
{
    if (!request.origin)
        return false;
    Opcode opcode = solver.operationAt(request.site, request.origin).opcode;
    return opcode == Opcode::ForInOpen || opcode == Opcode::ForInStep;
}

// The unknown object stands for what has reached native code, which lists the keys of whatever reaches it.
static void forEachObjectInheritedFrom(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects, NOESCAPE const Invocable<void(FlowObjectID)> auto& functor)
{
    solver.forEachMember(objects, [&](FlowObjectID object) {
        FlowObjectSet visited(object);
        Vector<FlowObjectID, 8> worklist { object };
        while (!worklist.isEmpty()) {
            forEachPrototype(solver, worklist.takeLast(), request.site, [&](FlowObjectID prototype) {
                if (prototype == unknownFlowObject || !visited.add(prototype))
                    return;
                functor(prototype);
                worklist.append(prototype);
            });
        }
    });
}

static void noteEnumerationOfPrototypes(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    forEachObjectInheritedFrom(solver, request, objects, [&](FlowObjectID prototype) {
        solver.noteUse(prototype, FlowObjectUse::Enumerated);
    });
}

// A locked built-in prototype has no enumerable own property and gets none. The members that a class defines are not enumerable, so its prototype object has one only once the program has put it there.
static bool mayHaveEnumerableProperty(FlowSolver& solver, const FlowProtocolRequest& request, FlowObjectID object)
{
    constexpr OptionSet<ObjectMutation> definingMutations { ObjectMutation::AddProperty, ObjectMutation::RedefineProperty };
    FlowObject record = solver.objects().at(object);
    switch (record.kind) {
    case FlowObjectKind::Builtin:
        return !solver.builtins().hasLockedPrototypeLink(solver.builtins().canonicalID(record.builtin));
    case FlowObjectKind::Prototype: {
        FunctionKind kind = solver.program().at(record.function).kind;
        if (kind != FunctionKind::BaseConstructor && kind != FunctionKind::DerivedConstructor)
            return true;
        solver.waitOnMutations(object, request.site, definingMutations);
        return solver.records().possibleMutations(object).containsAny(definingMutations);
    }
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::ModuleBuiltin:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isSerialization(const FlowProtocolRequest& request)
{
    return request.isPartOfSerialization || request.protocols.contains(Protocol::ToJSON);
}

// SerializeJSONProperty converts an object that has [[NumberData]] or [[StringData]]. No function has either, and for a function without a toJSON method it does nothing more.
static FlowNewObjects receiversOfConversion(const FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    if (!isSerialization(request) && !request.convertsFunctionsUnderNamesOnly)
        return objects;

    FlowObjectSet converted = objects;
    objects.forEach([&](FlowObjectID object) {
        if (solver.objects().isCallable(object))
            converted.remove(object);
    });
    return objects.withObjects(WTF::move(converted));
}

void runProtocolsOn(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    for (Protocol protocol : request.protocols) {
        switch (protocol) {
        case Protocol::OwnKeys:
            runOwnKeys(solver, request, objects);
            break;
        case Protocol::OwnValues:
            runOwnValues(solver, request, objects);
            break;
        case Protocol::ToPrimitive:
            runToPrimitive(solver, request, receiversOfConversion(solver, request, objects));
            break;
        case Protocol::Thenable:
            runThenable(solver, request, objects);
            break;
        case Protocol::Iterate:
            runIterate(solver, request);
            break;
        case Protocol::Species:
            runSpecies(solver, request, objects);
            break;
        case Protocol::ToJSON:
            runToJSON(solver, request, objects);
            break;
        case Protocol::HasInstance:
            runHasInstance(solver, request, objects);
            break;
        case Protocol::PrototypeChain:
            if (isForIn(solver, request))
                noteEnumerationOfPrototypes(solver, request, objects);
            else if (request.protocols.containsAll({ Protocol::OwnKeys, Protocol::OwnValues }))
                readInheritedEnumerableValues(solver, request, objects);
            throwFromTraps(solver, request, objects);
            break;
        case Protocol::Named:
            runNamedMembers(solver, request, objects);
            break;
        case Protocol::CallsNamed:
            runCalledMembers(solver, request, objects);
            break;
        }
    }
}

void callProtocolMember(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& receivers, const FlowMemberCall& member)
{
    callMethods(solver, memberCallRequest(request, member), receivers, member.keys);
}

FlowKeySet keysOfNames(const Program& program, std::span<const BuiltinName> names)
{
    FlowKeySet keys;
    for (BuiltinName name : names)
        keys.unite(keySetOf(program, program.keyOf(name)));
    return keys;
}

FlowKeySet keysReadBy(const Program& program, Protocol protocol)
{
    return keysOfNames(program, program.builtins().keysReadBy(protocol));
}

void runOwnKeys(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    solver.noteUseOf(objects, FlowObjectUse::Enumerated);
    throwFromTraps(solver, request, objects);
}

static void callOwnGetter(FlowSolver& solver, const FlowProtocolRequest& request, FlowObjectID object, FlowNodeID getter)
{
    FlowCallRequest call {
        request.site,
        request.origin,
        FlowCallLinkKind::Accessor,
        BuiltinCallbackTimes::Once,
        true,
        { .thisValue = request.operand, .thisObject = object, .result = request.result, .thrown = request.thrown },
    };
    callValuesOf(solver, call, getter);
}

static void readUnlistedOwnValues(FlowSolver& solver, const FlowProtocolRequest& request, FlowObjectID object)
{
    Witness witness = solver.witnessAt(request.site, request.origin);
    // The unknown object stands only for what has reached native code, so the exports nobody lists have to reach it.
    if (solver.objects().kindOf(object) == FlowObjectKind::ModuleNamespace)
        solver.joinObject(solver.graph().nativeCode(), object, witness);
    solver.joinUnknown(request.result, witness);
    throwUnknownValue(solver, request.thrown, witness);
}

static void readOwnValue(FlowSolver& solver, const FlowProtocolRequest& request, FlowObjectID object, const FlowPropertySource& source)
{
    switch (source.kind) {
    case FlowPropertySourceKind::Unknown:
        readUnlistedOwnValues(solver, request, object);
        return;
    case FlowPropertySourceKind::Value:
        solver.noteLocationEvent(source.node, FlowLocationEvent::ReadAsValue);
        solver.connect(source.node, request.result);
        return;
    case FlowPropertySourceKind::Getter:
        callOwnGetter(solver, request, object, source.node);
        return;
    case FlowPropertySourceKind::BuiltinMember:
    case FlowPropertySourceKind::BuiltinGetter: {
        accessProperties(solver, loadOf(request, request.result), FlowObjectSet(object), keySetOf(solver.program(), source.key));
        return;
    }
    case FlowPropertySourceKind::Absent:
    case FlowPropertySourceKind::Setter:
    case FlowPropertySourceKind::BuiltinSetter:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// SerializeJSONProperty reads each key of the list that was taken at the start with Get, so a key that a toJSON method or a getter has deleted since then is looked for on the prototypes.
static void readDeletedKeysFromPrototypes(FlowSolver& solver, const FlowProtocolRequest& request, FlowObjectID object)
{
    solver.waitOnMutations(object, request.site, ObjectMutation::DeleteField);
    if (solver.records().possibleMutations(object).contains(ObjectMutation::DeleteField))
        accessProperties(solver, loadOf(request, request.result), FlowObjectSet(object), FlowKeySet::everyKey());
}

void runOwnValues(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    objects.forEach([&](FlowObjectID object) {
        if (object == unknownFlowObject)
            return;
        solver.noteUse(object, FlowObjectUse::Enumerated);
        solver.noteUse(object, FlowObjectUse::ReadByUnknownKey);
    });

    // The own values of the unknown object are unknown, and so whatever an object that has reached native code holds.
    std::optional<FlowNewObjects> remaining = holdersBesideUnknownObject(solver, objects, request.operand);
    solver.forEachMember(remaining ? *remaining : objects, [&](FlowObjectID object) {
        if (solver.isAboutToBeAbandoned(request.site))
            return;
        if (object == unknownFlowObject) {
            readUnlistedOwnValues(solver, request, object);
            return;
        }

        FlowPropertyWalk walk { .operation = FlowWalkOperation::CopyDataProperties, .waiter = request.site, .builtinMembers = FlowBuiltinMemberLookup::ExceptConstructorLinks };
        forEachPropertySource(solver, object, FlowKeySet::everyKey(), walk, [&](const FlowPropertySource& source) {
            readOwnValue(solver, request, object, source);
        });
        if (isSerialization(request))
            readDeletedKeysFromPrototypes(solver, request, object);
    });
}

// The link from the prototype object of a function to the function is not enumerable until the program deletes or redefines it.
static bool hidesConstructorLink(FlowSolver& solver, const FlowProtocolRequest& request, FlowObjectID object)
{
    constexpr OptionSet<ObjectMutation> revealingMutations { ObjectMutation::DeleteField, ObjectMutation::RedefineProperty };
    if (solver.objects().kindOf(object) != FlowObjectKind::Prototype)
        return false;
    solver.waitOnMutations(object, request.site, revealingMutations);
    return !solver.records().possibleMutations(object).containsAny(revealingMutations);
}

// A built-in that lists keys, reads values and follows the prototype links, as a formatter does, does the first two on what its operand inherits from too.
// A class pays nothing for being printed, and no function is read because what it constructed is printed.
void readInheritedEnumerableValues(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    PropertyKeyID constructorKey = solver.propertyState().keys.constructor;
    forEachObjectInheritedFrom(solver, request, objects, [&](FlowObjectID prototype) {
        if (!mayHaveEnumerableProperty(solver, request, prototype) || solver.isAboutToBeAbandoned(request.site))
            return;
        solver.noteUse(prototype, FlowObjectUse::Enumerated);
        solver.noteUse(prototype, FlowObjectUse::ReadByUnknownKey);
        bool skipsConstructorLink = hidesConstructorLink(solver, request, prototype);
        FlowPropertyWalk walk { .operation = FlowWalkOperation::CopyDataProperties, .waiter = request.site, .isEnumerableOnly = true, .builtinMembers = FlowBuiltinMemberLookup::ExceptConstructorLinks };
        forEachPropertySource(solver, prototype, FlowKeySet::everyKey(), walk, [&](const FlowPropertySource& source) {
            if (skipsConstructorLink && source.key == constructorKey)
                return;
            readOwnValue(solver, request, prototype, source);
        });
    });
}

void runToPrimitive(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    if (objects.isEmpty())
        return;

    const Program& program = solver.program();
    PropertyKeyID exoticMethod = program.keyOf(BuiltinNames::toPrimitiveSymbol);
    FlowKeySet ordinaryMethods = keysReadBy(program, Protocol::ToPrimitive);
    ordinaryMethods.names.remove(exoticMethod);

    FlowNodeID hint[] { primitiveArgument(solver, request) };
    callProtocolMember(solver, request, objects, { keySetOf(program, exoticMethod), hint, { } });
    callProtocolMember(solver, request, objects, { ordinaryMethods, { }, { } });
}

void runThenable(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    awaitObjects(solver, { request.site, request.origin, request.result, request.thrown }, objects);
}

void runIterate(FlowSolver& solver, const FlowProtocolRequest& request)
{
    connectIteratedValues(solver, { request.site, request.origin, IteratorKind::Sync, true, request.thrown }, request.operand, request.result);
}

// With Array itself for the species, the operation creates the array of holes that its own site stands for.
static bool createsAtOwnSite(const FlowSolver& solver, const FlowProtocolRequest& request, FlowObjectID constructor)
{
    if (!request.origin || solver.operationAt(request.site, request.origin).opcode != Opcode::ArraySpeciesCreate)
        return false;
    const FlowObject& object = solver.objects().at(constructor);
    return object.kind == FlowObjectKind::Builtin && object.builtin == Builtins::Array;
}

void runSpecies(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    const Program& program = solver.program();
    FlowNodeID constructors = temporaryOf(solver, request, FlowCallTemporaryRole::SpeciesConstructors, request.operand.index());
    FlowNodeID species = temporaryOf(solver, request, FlowCallTemporaryRole::SpeciesValues, request.operand.index());
    solver.keepObjectsLettingInUnseenCodeOutOf(species);

    FlowAccess read = loadOf(request, constructors);
    accessProperties(solver, read, objects, keySetOf(program, program.keyOf(BuiltinNames::constructor)));

    read.receiver = constructors;
    read.value = species;
    accessProperties(solver, read, takeNewObjectsOf(solver, request, constructors), keySetOf(program, program.keyOf(BuiltinNames::speciesSymbol)));

    // Each built-in chooses the arguments: a length, a buffer and a range, a regular expression and its flags, the executor of a promise.
    // Some are the operand or what it holds, and the unknown value stands for those only once they have reached native code.
    solver.waitOn(species, request.site, FlowWakeUp::ContinueAtObject);
    bool constructsWithOtherCode = solver.valueOf(species).objects.anyOf([&](FlowObjectID constructor) {
        FlowObjectKind kind = solver.objects().kindOf(constructor);
        return kind != FlowObjectKind::Builtin && kind != FlowObjectKind::ModuleBuiltin;
    });
    if (constructsWithOtherCode)
        solver.connect(request.operand, solver.graph().nativeCode(), { }, solver.witnessAt(request.site, request.origin));

    FlowCallRequest construct {
        request.site,
        request.origin,
        FlowCallLinkKind::Construct,
        BuiltinCallbackTimes::Once,
        true,
        { .argumentList = unknownValueNode(solver), .result = request.result, .thrown = request.thrown },
    };
    FlowNodeID constructed = temporaryOf(solver, request, FlowCallTemporaryRole::SpeciesValues, request.operand.index() ^ 0x80000000);
    takeNewObjectsOf(solver, request, species).forEach([&](FlowObjectID constructor) {
        if (!createsAtOwnSite(solver, request, constructor))
            solver.joinObject(constructed, constructor, solver.witnessAt(request.site, request.origin));
    });
    callValuesOf(solver, construct, constructed);
}

static void callToJSON(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects, FlowNodeID serialized)
{
    if (objects.isEmpty())
        return;
    solver.noteUseOf(objects, FlowObjectUse::Serialized);
    FlowNodeID arguments[] { primitiveArgument(solver, request) };
    callProtocolMember(solver, request, objects, { keysReadBy(solver.program(), Protocol::ToJSON), arguments, serialized });
}

static void callToJSONOfBigInts(FlowSolver& solver, const FlowProtocolRequest& request, uint16_t primitives, FlowNodeID serialized)
{
    if (!(primitives & Primitive::BigInt))
        return;
    FlowNodeID arguments[] { primitiveArgument(solver, request) };
    FlowMemberCall toJSON { keysReadBy(solver.program(), Protocol::ToJSON), arguments, serialized };
    callMethodsOfPrimitives(solver, memberCallRequest(request, toJSON), { Primitive::BigInt }, toJSON.keys);
}

void runToJSON(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    FlowNodeID serialized = serializedValuesNode(solver, request);
    callToJSON(solver, request, objects, serialized);

    // What user code returns while these values are serialized comes back to the same node, so one nested request serves every depth.
    FlowProtocolRequest nested = request;
    nested.operand = transitiveContentsOf(solver, request.site, serialized);
    nested.protocols.remove(Protocol::ToJSON);
    nested.result = serialized;
    nested.isPartOfSerialization = true;

    FlowNewObjects contents = takeNewObjectsOf(solver, nested, nested.operand);
    callToJSON(solver, nested, contents, serialized);
    callToJSONOfBigInts(solver, nested, solver.valueOf(nested.operand).primitives, serialized);
    runProtocolsOn(solver, nested, contents);

    solver.connect(serialized, request.result);
}

void runProtocolsOnPrimitives(FlowSolver& solver, const FlowProtocolRequest& request, uint16_t primitives)
{
    // Awaiting a primitive gives the primitive.
    if (request.protocols.contains(Protocol::Thenable))
        solver.join(request.result, solver.valueOf(request.operand).withoutObjects());
    if (request.protocols.contains(Protocol::ToJSON))
        callToJSONOfBigInts(solver, request, primitives, serializedValuesNode(solver, request));
}

void runHasInstance(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    if (objects.isEmpty())
        return;
    solver.noteUseOf(objects, FlowObjectUse::TestedByInstanceOf);

    const Program& program = solver.program();
    PropertyKeyID method = program.keyOf(BuiltinNames::hasInstanceSymbol);
    FlowKeySet readKeys = keysReadBy(program, Protocol::HasInstance);
    readKeys.names.remove(method);

    FlowNodeID arguments[] { testedValueArgument(solver, request) };
    callProtocolMember(solver, request, objects, { keySetOf(program, method), arguments, { } });
    accessProperties(solver, loadOf(request, FlowNodeID()), objects, readKeys);
}

static FlowKeySet keysOfMembers(const Program& program, std::span<const BuiltinName> members)
{
    return members.empty() ? FlowKeySet::everyKey() : keysOfNames(program, members);
}

static void readNamedMembers(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects, FlowNodeID values)
{
    if (objects.isEmpty())
        return;
    FlowKeySet keys = keysOfMembers(solver.program(), request.namedMembers);
    FlowAccess read = loadOf(request, values);
    read.builtinMembers = builtinMembersFoundByProtocol(keys);

    switch (request.namedMemberUse) {
    case NamedMembers::AreOnlyInspected:
        read.value = FlowNodeID();
        break;
    case NamedMembers::AreOnlyRead:
        break;
    }
    accessProperties(solver, read, objects, keys);
}

void runNamedMembers(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    if (!request.protocols.contains(Protocol::ToPrimitive)) {
        readNamedMembers(solver, request, objects, request.result);
        return;
    }

    // LengthOfArrayLike converts what it reads under "length", and a request has no other word for that.
    FlowNodeID values = temporaryOf(solver, request, FlowCallTemporaryRole::NamedMemberValues, request.operand.index());
    readNamedMembers(solver, request, objects, values);
    solver.connect(values, request.result);
    if (!request.convertsFunctionsUnderNamesOnly) {
        runProtocols(solver, { request.site, request.origin, values, Protocol::ToPrimitive, { }, { }, request.thrown });
        return;
    }

    FlowProtocolRequest conversion { request.site, request.origin, values, Protocol::ToPrimitive, { }, { }, request.thrown };
    conversion.convertsFunctionsUnderNamesOnly = true;
    runProtocols(solver, conversion);
    if (request.convertedMembers.empty())
        return;

    FlowProtocolRequest read = request;
    read.namedMembers = request.convertedMembers;
    FlowNodeID converted = temporaryOf(solver, request, FlowCallTemporaryRole::ConvertedMemberValues, request.operand.index());
    readNamedMembers(solver, read, objects, converted);
    runProtocols(solver, { request.site, request.origin, converted, Protocol::ToPrimitive, { }, { }, request.thrown });
}

void runCalledMembers(FlowSolver& solver, const FlowProtocolRequest& request, const FlowNewObjects& objects)
{
    if (objects.isEmpty())
        return;
    FlowCallRequest call {
        request.site,
        request.origin,
        FlowCallLinkKind::Protocol,
        BuiltinCallbackTimes::Once,
        true,
        { .thisValue = request.operand, .argumentList = unknownValueNode(solver), .result = request.result, .thrown = request.thrown },
    };
    callMethods(solver, call, objects, keysOfMembers(solver.program(), request.calledMembers));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
