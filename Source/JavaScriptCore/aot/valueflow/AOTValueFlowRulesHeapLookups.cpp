/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesHeapInternal.h"

#if ENABLE(AOT)

#include "AOTFunctionKinds.h"

namespace JSC { namespace AOT { namespace Typed {

ValueFlowName writtenName(const HeapKey& key, HeapPropertyPart part)
{
    switch (part) {
    case HeapPropertyPart::Value:
        switch (key.keyClass) {
        case HeapKeyClass::Name:
            return ValueFlowName::property(key.name);
        case HeapKeyClass::Index:
            return elementsName;
        case HeapKeyClass::Unknown:
            return ValueFlowName::of(ValueFlowNameKind::UnknownKey);
        }
        break;
    case HeapPropertyPart::Getter:
        return key.keyClass == HeapKeyClass::Name ? ValueFlowName::getter(key.name) : ValueFlowName::of(ValueFlowNameKind::GetterUnderUnknownKey);
    case HeapPropertyPart::Setter:
        return key.keyClass == HeapKeyClass::Name ? ValueFlowName::setter(key.name) : ValueFlowName::of(ValueFlowNameKind::SetterUnderUnknownKey);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueFlowName foundName(const HeapKey& key, HeapPropertyPart part)
{
    if (key.keyClass != HeapKeyClass::Unknown)
        return writtenName(key, part);
    switch (part) {
    case HeapPropertyPart::Value:
        return ValueFlowName::of(ValueFlowNameKind::EveryProperty);
    case HeapPropertyPart::Getter:
        return ValueFlowName::of(ValueFlowNameKind::EveryGetter);
    case HeapPropertyPart::Setter:
        return ValueFlowName::of(ValueFlowNameKind::EverySetter);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

HeapKey keyOfFoundName(ValueFlowName name)
{
    switch (name.kind) {
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
        return { HeapKeyClass::Name, PropertyKeyID(name.payload) };
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
        return indexHeapKey;
    case ValueFlowNameKind::EveryProperty:
    case ValueFlowNameKind::EveryGetter:
    case ValueFlowNameKind::EverySetter:
        return unknownHeapKey;
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::PrototypeLink:
    case ValueFlowNameKind::CopiedFrom:
    case ValueFlowNameKind::DeletedMember:
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
    case ValueFlowNameKind::PrivateField:
    case ValueFlowNameKind::Slot:
    case ValueFlowNameKind::InternalFields:
    case ValueFlowNameKind::SteppedValues:
    case ValueFlowNameKind::ReturnedValue:
    case ValueFlowNameKind::SentValues:
    case ValueFlowNameKind::OpenedBy:
    case ValueFlowNameKind::ResolvedPromises:
    case ValueFlowNameKind::RejectedPromises:
    case ValueFlowNameKind::OpenedIterators:
    case ValueFlowNameKind::IteratedValues:
    case ValueFlowNameKind::IteratorResults:
    case ValueFlowNameKind::AwaitedValue:
    case ValueFlowNameKind::EveryMemberWritten:
    case ValueFlowNameKind::EveryMemberRead:
        return unknownHeapKey;
    case ValueFlowNameKind::MemberWritten:
        return { HeapKeyClass::Name, PropertyKeyID(name.payload) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BuiltinClassID wrapperOf(PrimitiveSet kind)
{
    uint16_t bit = static_cast<uint16_t>(kind);
    if (bit & Primitive::AnyString)
        return BuiltinClasses::String;
    if (bit & Primitive::Number)
        return BuiltinClasses::Number;
    if (bit & Primitive::Boolean)
        return BuiltinClasses::Boolean;
    if (bit & Primitive::AnySymbol)
        return BuiltinClasses::Symbol;
    if (bit & Primitive::BigInt)
        return BuiltinClasses::BigInt;
    return { };
}

static BuiltinClassID classOfFunctionObject(const Function& function)
{
    bool isAsync = function.has(FunctionFlags::IsAsync);
    if (function.has(FunctionFlags::IsGenerator))
        return isAsync ? BuiltinClasses::AsyncGeneratorFunction : BuiltinClasses::GeneratorFunction;
    return isAsync ? BuiltinClasses::AsyncFunction : BuiltinClasses::Function;
}

static BuiltinClassID classAbovePrototypeProperty(const Function& function)
{
    if (!function.has(FunctionFlags::IsGenerator))
        return BuiltinClasses::Object;
    return function.has(FunctionFlags::IsAsync) ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator;
}

// The run time answers the same question with FunctionTables::hasPrototypePropertyAtCreation.
static bool hasPrototypeProperty(const Program& program, const Function& function)
{
    if (function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor)
        return true;
    if (isInRuntimeModule(program, function))
        return false;
    if (function.has(FunctionFlags::IsGenerator))
        return true;
    return function.kind == FunctionKind::Plain && !function.has(FunctionFlags::IsAsync);
}

// The unknown object is looked at by itself, so that a set with it and the same set without it share one lookup. Nothing is looked up on undefined and null,
// and a constant key is a string or a symbol like any other.
ClimbingSet HeapRule::startOfLookup(ClimbingSet receivers)
{
    ClimbingSet start = m_context.restrict(receivers, m_tables.startsOfLookups());
    if (m_context.meet(receivers, m_universe.keys()))
        start = m_context.unite(start, ClimbingSet(m_tables.stringAndSymbolKinds()));
    return start;
}

HeapChain HeapRule::chainFrom(ClimbingSet receivers)
{
    ClimbingSet start = startOfLookup(receivers);
    ClimbingSet chain = m_context.valueOf(m_context.lookup(start, prototypeLinkName));
    return { start, ClimbingSet(ValueSets::nothing), known(chain), known(chain), mayBeUnknownObject(chain) || mayBeUnknownObject(receivers) };
}

// A built-in holder that has the name among its own members answers by itself. A store leaves the member where it is, so only a delete sends the lookup on to what lies above the holder.
// A locked location holds what the table says.
HeapChain HeapRule::chainFrom(ClimbingSet receivers, const HeapKey& key)
{
    auto withoutLockedLocations = [&](HeapChain chain) {
        ValueSetID locked = m_tables.lockedObjects();
        if (key.keyClass == HeapKeyClass::Name)
            locked = m_tables.under(key.name).holdersWithLockedLocation;
        else if (key.keyClass == HeapKeyClass::Index)
            locked = m_tables.holdersWithLockedIndexes();
        chain.writableObjects = m_context.restrict(chain.objects, ValueSets::complement(locked));
        return chain;
    };

    if (key.keyClass != HeapKeyClass::Name)
        return withoutLockedLocations(chainFrom(receivers));
    const ValueFlowMembersUnderKey& members = m_tables.under(key.name);
    ClimbingSet holders = m_context.restrict(receivers, members.holders);
    if (!holdsAnything(holders))
        return withoutLockedLocations(chainFrom(receivers));

    ClimbingSet holdersThatMayLoseMember = m_context.restrict(holders, ValueSets::complement(members.holdersWithLockedLocation));
    ValueFlowName deleted { ValueFlowNameKind::DeletedMember, key.name.index() };
    if (holdsAnything(holdersThatMayLoseMember) && (holdsAnything(m_context.valueOf(m_context.read(holdersThatMayLoseMember, deleted))) || storedThroughUnknownObjectInto(holdersThatMayLoseMember, deleted)))
        return withoutLockedLocations(chainFrom(receivers));

    HeapChain chain = chainFrom(m_context.restrict(receivers, ValueSets::complement(members.holders)));
    chain.holdersThatAnswer = holders;
    chain.objects = m_context.unite(chain.objects, holders);
    return withoutLockedLocations(chain);
}

// Most names are never stored through the unknown object, and then nobody has to look at what has escaped.
ValueFlowNodeID HeapRule::storedThroughUnknownObject(const HeapChain& chain, ValueFlowName name)
{
    return storedThroughUnknownObjectInto(chain.writableObjects, name);
}

ValueFlowNodeID HeapRule::storedThroughUnknownObjectInto(ClimbingSet objects, ValueFlowName name)
{
    ValueFlowNodeID stored = m_context.read(unknownObjectAlone(), name);
    if (!holdsAnything(m_context.valueOf(stored)) || !hasEscaped(objects))
        return { };
    return stored;
}

// What lies above the unknown object is handed out as the unknown object, and the prototype objects of the table are among it. So they have escaped: what is stored through that value is found on them.
void HeapRule::letPrototypeObjectsOfTableEscape()
{
    add(ValueFlowProgramNodes::escaped(), m_tables.prototypeObjects());
}

void HeapRule::appendPrototypeOf(BuiltinClassID builtinClass, Members& prototypes) const
{
    if (!builtinClass)
        return;
    if (BuiltinID prototype = m_builtins.at(builtinClass).prototype)
        prototypes.append(m_universe.objectOf(prototype));
    else
        prototypes.append(m_tables.undescribedNativeCode());
}

void HeapRule::appendPrototypeAtCreation(ValueFlowObjectID object, Members& prototypes) const
{
    auto hasHeritage = [&](LayoutID layout) {
        return layout && m_program.at(layout).has(LayoutFlags::HasHeritage);
    };

    switch (m_universe.kindOf(object)) {
    case ValueFlowObjectKind::Primitive:
        appendPrototypeOf(wrapperOf(static_cast<PrimitiveSet>(1u << object.index())), prototypes);
        return;
    case ValueFlowObjectKind::Key:
        appendPrototypeOf(m_program.at(m_universe.keyOf(object)).kind == PropertyKeyKind::Text ? BuiltinClasses::String : BuiltinClasses::Symbol, prototypes);
        return;
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Namespace:
    case ValueFlowObjectKind::ResultOfBuiltin:
        return;
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin: {
        const Builtin& entry = m_builtins.at(m_universe.builtinOf(object));
        switch (entry.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            if (object != m_tables.undescribedNativeCode())
                prototypes.append(m_tables.undescribedNativeCode());
            return;
        case BuiltinPrototypeLink::Null:
            return;
        case BuiltinPrototypeLink::Builtin:
            prototypes.append(m_universe.objectOf(entry.inheritsFrom));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    case ValueFlowObjectKind::Closure: {
        const Function& function = m_program.at(m_universe.closureFunction(object));
        if (!hasHeritage(function.constructedClass))
            appendPrototypeOf(classOfFunctionObject(function), prototypes);
        return;
    }
    case ValueFlowObjectKind::Instance:
        prototypes.append(m_universe.prototypeOf(m_universe.layoutOf(object)));
        return;
    case ValueFlowObjectKind::CoroutineResult:
    case ValueFlowObjectKind::RestArray:
        appendPrototypeOf(m_universe.classOf(object), prototypes);
        return;
    case ValueFlowObjectKind::Prototype: {
        LayoutID layout = m_universe.classLayoutOf(object);
        if (hasHeritage(layout))
            return;
        FunctionID constructor = m_program.at(layout).constructor;
        appendPrototypeOf(constructor ? classAbovePrototypeProperty(m_program.at(constructor)) : BuiltinClasses::Object, prototypes);
        return;
    }
    case ValueFlowObjectKind::Site: {
        const ValueFlowSite& site = *m_universe.siteOf(object);
        if (site.builtinClass)
            appendPrototypeOf(site.builtinClass == BuiltinClasses::Arguments ? BuiltinClasses::Object : site.builtinClass, prototypes);
        if (!site.layout)
            return;
        switch (m_program.at(site.layout).kind) {
        case LayoutKind::Record:
        case LayoutKind::Container:
        case LayoutKind::BuiltinResult:
            appendPrototypeOf(BuiltinClasses::Object, prototypes);
            return;
        case LayoutKind::Instance:
            prototypes.append(m_universe.prototypeOf(site.layout));
            return;
        case LayoutKind::Environment:
        case LayoutKind::Closure:
        case LayoutKind::CoroutineFrame:
        case LayoutKind::ModuleNamespace:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueSetID HeapRule::prototypesAtCreationOf(ClimbingSet objects)
{
    Members prototypes;
    m_context.forEachMember(objects, [&](ValueFlowObjectID object) {
        appendPrototypeAtCreation(object, prototypes);
    });
    return m_context.maker().of(prototypes.span());
}

BuiltinID HeapRule::memberUnder(BuiltinID holder, PropertyKeyID key) const
{
    if (BuiltinID member = findOwnMember(m_program, holder, key))
        return member;
    // The host's own modules reach some of its functions under private names of the global object.
    if (m_builtins.canonicalID(holder) != Builtins::GlobalThis || m_program.at(key).kind != PropertyKeyKind::EnginePrivateName)
        return { };
    return m_builtins.findOwnMember(Builtins::PrivateGlobals, builtinMemberNameOf(m_program, key).name);
}

ValueFlowObjectID HeapRule::objectOfMember(BuiltinID member, ModuleID module) const
{
    if (ValueFlowObjectID object = module ? m_universe.objectOf(member, module) : ValueFlowObjectID())
        return object;
    return m_universe.objectOf(member);
}

// What lies on the instances of a class is found at its prototype object: an instance has that object above it.
bool HeapRule::mayHoldUndeclaredProperty(ClimbingSet objects, const HeapKey& key) const
{
    return m_context.meet(objects, key.keyClass == HeapKeyClass::Name ? m_tables.under(key.name).holdersOfUndeclaredProperties : m_tables.holdersOfUndeclaredPropertiesUnderAnyKey());
}

// The unknown object stands for an instance of any class of the table, which inherits the members of the prototype object of the class, and for every built-in holder that has escaped.
ClimbingSet HeapRule::holdersFoundThroughUnknownObject(const HeapKey& key)
{
    switch (key.keyClass) {
    case HeapKeyClass::Name: {
        const ValueFlowMembersUnderKey& members = m_tables.under(key.name);
        if (members.holders == ValueSets::nothing)
            return ClimbingSet(ValueSets::nothing);
        return m_context.unite(escapedAmong(ClimbingSet(members.holders)), ClimbingSet(members.prototypeObjects));
    }
    case HeapKeyClass::Index:
        return ClimbingSet(ValueSets::nothing);
    case HeapKeyClass::Unknown:
        return m_context.unite(escapedAmong(ClimbingSet(m_tables.holdersWithMembers())), ClimbingSet(m_tables.prototypeObjects()));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ClimbingSet HeapRule::objectsHeldFromCreation(ClimbingSet objects)
{
    Members held;
    m_context.forEachMember(known(objects), [&](ValueFlowObjectID object) {
        appendPrototypeAtCreation(object, held);
        if (FunctionID function = m_universe.closureFunction(object)) {
            if (LayoutID layout = m_program.at(function).constructedClass)
                held.append(m_universe.prototypeOf(layout));
        }
        LayoutID layout = m_universe.classLayoutOf(object);
        if (FunctionID constructor = layout ? m_program.at(layout).constructor : FunctionID())
            held.append(m_universe.closureOf(constructor));
    });
    ValueSetID leftOut = m_context.maker().unite(m_universe.builtinObjects(), m_universe.unknownObjectAlone());
    return m_context.restrict(ClimbingSet(m_context.maker().of(held.span())), ValueSets::complement(leftOut));
}

void HeapRule::collectChain()
{
    ClimbingSet start = m_context.setOfKey();
    add(m_statement.result, start);
    if (mayBeUnknownObject(start))
        letPrototypeObjectsOfTableEscape();

    // A link that is locked is the one that the table names.
    ClimbingSet objects = m_context.restrict(known(start), ValueSets::complement(m_tables.holdersWithLockedPrototypeLink()));
    ClimbingSet above = m_context.unite(m_context.valueOf(m_context.read(objects, prototypeLinkName)), m_context.valueOf(m_context.read(known(start), copiedFromName)));
    above = m_context.unite(above, ClimbingSet(prototypesAtCreationOf(start)));
    if (ValueFlowNodeID stored = storedThroughUnknownObjectInto(objects, prototypeLinkName))
        above = m_context.unite(above, m_context.valueOf(stored));
    connect(m_context.lookup(known(above), prototypeLinkName), m_statement.result);
    if (mayBeUnknownObject(above))
        add(m_statement.result, m_universe.unknownObjectAlone());
}

void HeapRule::readMember(BuiltinID member, ModuleID module, ValueFlowNodeID result, Members& getters)
{
    use(member, BuiltinUse::Read);
    switch (m_builtins.at(member).kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
        add(result, only(objectOfMember(member, module)));
        return;
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
        getters.append(objectOfMember(member, module));
        return;
    case BuiltinKind::Constant:
        add(result, m_universe.anyPrimitive());
        return;
    case BuiltinKind::NativeBinding:
        add(result, unknownValue());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// At the highest bit in which the numbers of two members differ, so that the halves depend on nothing but their own members: sets that share members share halves. False for a set of one member or none.
bool HeapRule::splitInTwo(ClimbingSet set, ClimbingSet& first, ClimbingSet& second)
{
    uint32_t lowest = std::numeric_limits<uint32_t>::max();
    uint32_t highest = 0;
    m_context.forEachMember(set, [&](ValueFlowObjectID member) {
        lowest = std::min(lowest, member.index());
        highest = std::max(highest, member.index());
    });
    if (lowest >= highest)
        return false;

    uint32_t boundary = highest & ~(std::bit_floor(lowest ^ highest) - 1);
    ValueSetID lowerHalf = m_context.maker().ofRange(ValueFlowObjectID(0u), ValueFlowObjectID(boundary));
    first = m_context.restrict(set, lowerHalf);
    second = m_context.restrict(set, ValueSets::complement(lowerHalf));
    return true;
}

void HeapRule::callAccessors(ClimbingSet functions, ValueFlowCallKind kind, const HeapReceiver& receiver, const ValueFlowOperand* value, ValueFlowNodeID result)
{
    // An accessor or a data property of the table stands for the function behind it, which no read hands out.
    if (!m_context.meet(functions, m_tables.accessors()))
        return;

    if (m_accessorFunctor) {
        m_context.forEachMember(m_context.restrict(functions, m_tables.accessors()), [&](ValueFlowObjectID function) {
            (*m_accessorFunctor)(function == m_tables.undescribedNativeCode() ? ValueFlowUniverse::unknownObject() : function, kind);
        });
    }

    // Native code that nothing describes looks at what it is given. A built-in object is nothing new to it.
    if (m_context.contains(functions, m_tables.undescribedNativeCode())) {
        if (receiver.operand)
            connectThrough(m_tables.whatIsNewToNativeCode(), *receiver.operand, ValueFlowProgramNodes::escaped());
        else
            add(ValueFlowProgramNodes::escaped(), m_context.restrict(receiver.set, m_tables.whatIsNewToNativeCode()));
        if (value)
            connect(*value, ValueFlowProgramNodes::escaped());
        add(result, unknownValue());
    }

    // An accessor nobody knows, on an object nobody knows, may be a function that has escaped, which is entered with the unknown object as every operand.
    if (mayBeUnknownObject(functions)) {
        pass(receiver, ValueFlowProgramNodes::escaped());
        if (value)
            connect(*value, ValueFlowProgramNodes::escaped());
        add(result, unknownValue());
    }
    if (!m_context.meet(functions, m_tables.knownAccessors()))
        return;

    ValueFlowCallGroup group = m_context.group(m_context.restrict(functions, m_tables.knownAccessors()), kind);
    pass(receiver, group.thisValue);
    if (value)
        connect(*value, group.argument(0));
    connect(group.result, result);
    connect(group.thrown, ValueFlowProgramNodes::escaped());
}

// The unknown object may be a proxy, whose handler is in unknown hands like the proxy. Only the results have to hear of it.
void HeapRule::noteTrapOfUnknownObject(ClimbingSet objects, ValueFlowCallKind kind)
{
    if (m_accessorFunctor && mayBeUnknownObject(objects))
        (*m_accessorFunctor)(ValueFlowUniverse::unknownObject(), kind);
}

// A function that can construct has a prototype object from the start, and that object names the function as its constructor.
void HeapRule::readPropertiesAtCreation(ClimbingSet objects, const HeapKey& key, ValueFlowNodeID result)
{
    auto mayName = [&](PropertyKeyID name) {
        return key.keyClass == HeapKeyClass::Unknown || (key.keyClass == HeapKeyClass::Name && key.name == name);
    };

    if (mayName(m_tables.prototypeKey())) {
        m_context.forEachMember(m_context.restrict(objects, m_universe.closures()), [&](ValueFlowObjectID closure) {
            const Function& function = m_program.at(m_universe.closureFunction(closure));
            if (function.constructedClass)
                add(result, only(m_universe.prototypeOf(function.constructedClass)));
            else if (function.has(FunctionFlags::IsGenerator))
                // The prototype object of a generator function lies above its generators and nowhere else, so the object that stands for them stands for it too.
                add(result, only(m_universe.coroutineResultOf(m_universe.closureFunction(closure))));
            else if (hasPrototypeProperty(m_program, function))
                add(result, m_universe.unknownObjectAlone());
        });
    }
    if (mayName(m_tables.constructorKey())) {
        m_context.forEachMember(objects, [&](ValueFlowObjectID object) {
            LayoutID layout = m_universe.classLayoutOf(object);
            if (FunctionID constructor = layout ? m_program.at(layout).constructor : FunctionID())
                add(result, only(m_universe.closureOf(constructor)));
        });
    }
    // The array of the raw strings of a template object is followed by nobody.
    if (mayName(m_tables.rawKey())) {
        m_context.forEachMember(m_context.restrict(objects, m_universe.arrays()), [&](ValueFlowObjectID object) {
            const ValueFlowSite* site = m_universe.siteOf(object);
            if (site && site->origin == ValueFlowSiteOrigin::Operation && m_program.at(m_universe.functionOf(object)).at(site->value).opcode == Opcode::NewTemplateObject)
                add(result, m_universe.unknownObjectAlone());
        });
    }
}

// Under a key nobody knows every member is read. What that gives is known for each holder of the realm, and one statement for each set of holders records the uses.
void HeapRule::readMembers(ClimbingSet objects, const HeapKey& key, ValueFlowNodeID result, Members& getters)
{
    if (key.keyClass != HeapKeyClass::Unknown) {
        forEachMemberUnder(objects, key, [&](BuiltinID member, ModuleID module) {
            readMember(member, module, result, getters);
        });
        return;
    }

    ClimbingSet holders = m_context.restrict(objects, m_tables.holdersWithMembers());
    if (!holdsAnything(holders))
        return;
    m_context.lookup(holders, ValueFlowName::of(ValueFlowNameKind::EveryMemberRead));
    m_context.forEachMember(holders, [&](ValueFlowObjectID holder) {
        if (ModuleID module = m_universe.moduleOf(holder)) {
            for (BuiltinID member : m_builtins.membersOf(m_universe.builtinOf(holder)))
                readMember(member, module, result, getters);
            return;
        }
        const ValueFlowMembersOfHolder& members = m_tables.membersOf(m_universe.builtinOf(holder));
        add(result, members.values);
        m_context.sets().forEachMember(members.accessors, [&](ValueFlowObjectID accessor) {
            getters.append(accessor);
        });
    });
}

void HeapRule::readThroughChain(ClimbingSet start, const HeapKey& key, const HeapReceiver& receiver, ValueFlowNodeID result)
{
    Members builtinGetters;
    HeapChain chain = chainFrom(start, key);
    ValueFlowName value = foundName(key, HeapPropertyPart::Value);
    ValueFlowName getter = foundName(key, HeapPropertyPart::Getter);

    add(result, m_universe.undefinedValue());
    connect(m_context.read(chain.writableObjects, value), result);
    connect(storedThroughUnknownObject(chain, value), result);
    ClimbingSet getters = m_context.valueOf(m_context.read(chain.writableObjects, getter));
    if (ValueFlowNodeID stored = storedThroughUnknownObject(chain, getter))
        getters = m_context.unite(getters, m_context.valueOf(stored));

    readPropertiesAtCreation(chain.objects, key, result);
    readMembers(chain.objects, key, result, builtinGetters);
    if (mayHoldUndeclaredProperty(chain.objects, key))
        builtinGetters.append(m_tables.undescribedNativeCode());

    if (mayBeUnknownObject(start)) {
        add(result, unknownValue());
        // A constructor, and under a key nobody knows every member, is handed out as the unknown object. So it has escaped: whoever has the value may call it and store into it.
        // The run time refuses a function that starts unseen code to whoever has not named it.
        Members handedOutAsUnknownObject;
        forEachMemberUnder(holdersFoundThroughUnknownObject(key), key, [&](BuiltinID member, ModuleID module) {
            if (m_builtins.letsInUnseenCode(m_builtins.canonicalID(member)))
                return;
            if (key.keyClass == HeapKeyClass::Name && m_builtins.at(member).kind != BuiltinKind::Constructor) {
                readMember(member, module, result, builtinGetters);
                return;
            }
            // Likewise a member of the host that no name in the program leads to: the image holds the code behind it only for a program that names it.
            if (m_builtins.at(member).origin == BuiltinOrigin::Host)
                return;
            use(member, { BuiltinUse::Read, BuiltinUse::Escaped });
            if (isObjectKind(m_builtins.at(member).kind))
                handedOutAsUnknownObject.append(objectOfMember(member, module));
        });
        add(ValueFlowProgramNodes::escaped(), m_context.maker().of(handedOutAsUnknownObject.span()));
    } else if (chain.holdsUnknownObject) {
        // One lookup for each name records what is found through the unknown object.
        connect(m_context.lookup(unknownObjectAlone(), value), result);
    }
    if (chain.holdsUnknownObject)
        builtinGetters.append(ValueFlowUniverse::unknownObject());
    // A string has its characters under the indexes, whether it is a primitive or wrapped, and a typed array its numbers.
    if (key.keyClass != HeapKeyClass::Name && m_context.meet(chain.objects, m_tables.prototypesAboveNativeElements()))
        add(result, m_universe.anyPrimitive());

    getters = m_context.unite(getters, ClimbingSet(m_context.maker().of(builtinGetters.span())));

    // Only a receiver on whose own chain an accessor nobody knows may lie is its `this`. So the lookup is made for each half of the receivers: a half without such an accessor keeps all its members,
    // and in the end one receiver stands alone.
    ClimbingSet first;
    ClimbingSet second;
    if (!receiver.operand && mayBeAccessorNobodyKnows(getters) && splitInTwo(start, first, second)) {
        connect(m_context.lookup(first, value), result);
        connect(m_context.lookup(second, value), result);
        return;
    }
    callAccessors(getters, ValueFlowCallKind::Get, receiver, nullptr, result);
}

void HeapRule::collectAccessors(BuiltinCallMode mode)
{
    ValueFlowName name = m_context.nameOfKey();
    HeapKey key = keyOfFoundName(name);
    Members accessors;
    auto appendIfAccessor = [&](BuiltinID member, ModuleID module) {
        if (mode == BuiltinCallMode::Get ? m_tables.hasGetEntry(member) : m_tables.hasSetEntry(member))
            accessors.append(objectOfMember(member, module));
    };
    HeapChain chain = chainFrom(m_context.setOfKey(), key);

    connect(m_context.read(chain.writableObjects, name), m_statement.result);
    connect(storedThroughUnknownObject(chain, name), m_statement.result);

    auto appendAccessorsOf = [&](ClimbingSet objects) {
        if (key.keyClass != HeapKeyClass::Unknown) {
            forEachMemberUnder(objects, key, appendIfAccessor);
            return;
        }
        m_context.forEachMember(m_context.restrict(objects, m_tables.holdersWithMembers()), [&](ValueFlowObjectID holder) {
            if (ModuleID module = m_universe.moduleOf(holder)) {
                for (BuiltinID member : m_builtins.membersOf(m_universe.builtinOf(holder)))
                    appendIfAccessor(member, module);
                return;
            }
            const ValueFlowMembersOfHolder& members = m_tables.membersOf(m_universe.builtinOf(holder));
            add(m_statement.result, mode == BuiltinCallMode::Get ? members.accessorsWithGetEntry : members.accessorsWithSetEntry);
        });
    };
    appendAccessorsOf(chain.objects);
    if (mayHoldUndeclaredProperty(chain.objects, key))
        accessors.append(m_tables.undescribedNativeCode());
    if (chain.holdsUnknownObject) {
        accessors.append(ValueFlowUniverse::unknownObject());
        appendAccessorsOf(holdersFoundThroughUnknownObject(key));
    }
    add(m_statement.result, m_context.maker().of(accessors.span()));
}

// One statement for each set of holders, so that no store and no read goes over the members by itself. Through the unknown object a store reaches the holders that have escaped.
void HeapRule::noteUseOfMembers(const HeapKey& key, BuiltinUse noted)
{
    ClimbingSet holders = m_context.setOfKey();
    if (mayBeUnknownObject(holders))
        holders = m_context.unite(holders, escapedAmong(ClimbingSet(key.keyClass == HeapKeyClass::Name ? m_tables.under(key.name).holders : m_tables.holdersWithMembers())));
    forEachMemberUnder(holders, key, [&](BuiltinID member, ModuleID) {
        use(member, noted);
    });
}

// Beside the unknown object, a getter nobody knows would get every other receiver as `this`.
bool HeapRule::splitLookupThroughUnknownObject()
{
    ClimbingSet start = m_context.setOfKey();
    ValueSetID others = ValueSets::complement(m_universe.unknownObjectAlone());
    if (!mayBeUnknownObject(start) || !m_context.meet(start, others))
        return false;
    connect(m_context.lookup(m_context.restrict(start, others), m_context.nameOfKey()), m_statement.result);
    connect(m_context.lookup(unknownObjectAlone(), m_context.nameOfKey()), m_statement.result);
    return true;
}

void HeapRule::runLookup()
{
    ValueFlowName name = m_context.nameOfKey();
    if (name.kind == ValueFlowNameKind::PrototypeLink) {
        collectChain();
        return;
    }
    switch (name.kind) {
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
        noteUseOfMembers(keyOfFoundName(name), BuiltinUse::Written);
        return;
    case ValueFlowNameKind::EveryMemberRead:
        noteUseOfMembers(unknownHeapKey, BuiltinUse::Read);
        return;
    default:
        break;
    }
    if (splitLookupThroughUnknownObject())
        return;

    switch (name.kind) {
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::EveryProperty:
        readThroughChain(m_context.setOfKey(), keyOfFoundName(name), { nullptr, m_context.setOfKey() }, m_statement.result);
        return;
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::SetterUnderUnknownKey:
    case ValueFlowNameKind::EverySetter:
        collectAccessors(BuiltinCallMode::Set);
        return;
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::EveryGetter:
        collectAccessors(BuiltinCallMode::Get);
        return;
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::PrototypeLink:
    case ValueFlowNameKind::CopiedFrom:
    case ValueFlowNameKind::DeletedMember:
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
    case ValueFlowNameKind::PrivateField:
    case ValueFlowNameKind::Slot:
    case ValueFlowNameKind::InternalFields:
    case ValueFlowNameKind::SteppedValues:
    case ValueFlowNameKind::ReturnedValue:
    case ValueFlowNameKind::SentValues:
    case ValueFlowNameKind::OpenedBy:
    case ValueFlowNameKind::ResolvedPromises:
    case ValueFlowNameKind::RejectedPromises:
    case ValueFlowNameKind::OpenedIterators:
    case ValueFlowNameKind::IteratedValues:
    case ValueFlowNameKind::IteratorResults:
    case ValueFlowNameKind::AwaitedValue:
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
    case ValueFlowNameKind::EveryMemberRead:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void runLookup(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    HeapRule(statement, context).runLookup();
}

ClimbingSet objectsHeldFromCreation(ValueFlowRuleContext& context, ClimbingSet objects)
{
    ValueFlowStatement statement;
    return HeapRule(statement, context).objectsHeldFromCreation(objects);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
