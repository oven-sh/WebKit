/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRelationalSummaries.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTFlowResults.h"
#include "AOTPropertyAccessInternal.h"
#include <algorithm>
#include <limits>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// The accesses that a callee makes to an object, asked of what the caller knows of the object. Every answer is "no" unless each step of each lookup is proven for the whole life of the object:
// by flow, which has seen every definition, deletion and change of a prototype in the program, wherever and whenever it runs, or by the run time, which locks the built-in prototypes.
// A condition that a guard watches and a proof that a test gives are worth nothing here: nobody tests anything in the callee.
// What the type itself proves of an instance of a built-in class has to hold as long as the accesses go on. What flow has proven does: it stands in the types of the positions at which the object arrives,
// an entry parameter of the callee for one, and not in the type of the operation that makes the object.
class AccessesToObject {
public:
    AccessesToObject(const Program& program, const FlowResults& flow, const LinkedFate& fate, GettersOfProgram* getters = nullptr)
        : m_program(program)
        , m_flow(flow)
        , m_table(program.builtins())
        , m_context { program, &flow }
        , m_fate(fate)
        , m_accesses(fate.itself & fatesThatMayRunCodeOfTheObject)
        , m_getters(getters)
    {
    }

    bool runNoCodeOn(const Type&) const;
    bool storesLandOn(const Type&) const;

private:
    bool has(ValueFate access) const { return m_accesses.contains(access); }
    bool namesItsKeys() const;
    bool allKeysRunNoCode(NOESCAPE const Invocable<bool(PropertyKeyID, Access)> auto& underKey, NOESCAPE const Invocable<bool(Access)> auto& underAnyKey) const;

    bool endsWithoutCode(const MemberAnswer&, Access) const;
    bool gainsNoMember(BuiltinID holder) const;
    bool chainGainsNoMember(BuiltinID firstHolder) const;

    bool runNoCodeOnEnvironment() const;
    bool runNoCodeOnLayouts(LayoutSetID) const;
    bool runNoCodeOnLayout(LayoutID) const;
    bool accessRunsNoCodeOnLayout(LayoutID, PropertyKeyID, Access) const;
    bool readUnderAnyKeyRunsNoCodeOnLayout(LayoutID) const;
    bool hasNoOwnAccessor(LayoutID) const;
    bool runNoCodeOnInstances(const ClassPart&) const;
    bool iterationRunsNoCodeOn(const ClassPart&) const;
    bool runNoCodeOnClosures(FunctionID) const;
    bool conversionRunsNoCodeOn(const Type&) const;
    bool allStoresLand(NOESCAPE const Invocable<bool(PropertyKeyID)> auto& underKey, NOESCAPE const Invocable<bool()> auto& underAnyKey) const;
    bool storeLandsOnLayout(LayoutID, PropertyKeyID) const;
    bool storesLandOnInstances(const ClassPart&) const;

    const Program& m_program;
    const FlowResults& m_flow;
    const BuiltinTable& m_table;
    PropertyAccessContext m_context;
    const LinkedFate& m_fate;
    OptionSet<ValueFate> m_accesses;
    // Whoever asks for the getters enters them himself.
    GettersOfProgram* m_getters;
};

} // anonymous namespace

static constexpr uint64_t mostLayouts = defaultLimitValue(LimitID::MaxAccessPlanMembers, hostArchitecture());

// A fate that says "members are read" and lists no key was made of the contents of another fate, whose keys nobody has listed.
bool AccessesToObject::namesItsKeys() const
{
    return (!has(ValueFate::HasMembersRead) || !m_fate.keysRead.isEmpty()) && (!has(ValueFate::HasMembersWritten) || !m_fate.keysWritten.isEmpty());
}

bool AccessesToObject::allKeysRunNoCode(NOESCAPE const Invocable<bool(PropertyKeyID, Access)> auto& underKey, NOESCAPE const Invocable<bool(Access)> auto& underAnyKey) const
{
    auto ask = [&](const KeysOfMembers& keys, Access access) {
        // One key stands for all the symbols that one operation creates, and what is known of the key is known of none of them.
        bool mayBeAnyKey = keys.mayBeAnyKey || std::ranges::any_of(keys.keys, [&](PropertyKeyID key) {
            return !key || m_flow.standsForSeveralSymbols(key);
        });
        if (mayBeAnyKey)
            return underAnyKey(access);
        return std::ranges::all_of(keys.keys, [&](PropertyKeyID key) {
            return underKey(key, access);
        });
    };
    return ask(m_fate.keysRead, Access::Read) && ask(m_fate.keysWritten, Access::Store);
}

// What a lookup above the object itself ends at.
// Nothing: a read answers undefined, a store creates the property or is refused. A method of a class: a read answers the function, and a store passes a writable property by or is refused.
// A member of a built-in prototype: the chains that get here hold Object.prototype, Array.prototype, Map.prototype, Set.prototype and Function.prototype, whose members start-up locks.
// Their accessors are the engine's, call nothing of the program and keep nothing of `this`: `size` of a Map or a Set, `caller` and `arguments` of Function.prototype.
// `__proto__` of Object.prototype is not among the answers: nobody answers a lookup under that name by name.
bool AccessesToObject::endsWithoutCode(const MemberAnswer& answer, Access access) const
{
    if (!answer.needsNoTest())
        return false;
    switch (answer.kind) {
    case MemberKind::Absent:
    case MemberKind::Method:
        return true;
    case MemberKind::Builtin: {
        const Builtin& member = m_table.at(answer.builtin);
        return member.kind != BuiltinKind::NativeBinding && (member.kind != BuiltinKind::Accessor || member.origin == BuiltinOrigin::Engine);
    }
    case MemberKind::Accessor:
        // An accessor that a class defines for good. Without a getter a read answers undefined. A setter is handed the stored value too, of which nobody here knows anything.
        if (access != Access::Read)
            return false;
        if (!answer.getter)
            return true;
        if (!m_getters)
            return false;
        insertInUniquedSortedVector(*m_getters, answer.getter);
        return true;
    case MemberKind::Unknown:
    case MemberKind::Field:
    case MemberKind::Variable:
    case MemberKind::Namespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The program puts nothing of its own on the built-in object, and replaces nothing there: under a key that nobody knows a lookup finds what the table lists or passes.
// Start-up locks the members of Array.prototype and leaves it open to other names, so `Object.defineProperty(Array.prototype, "last", { get() { .. } })` is for flow to rule out.
bool AccessesToObject::gainsNoMember(BuiltinID holder) const
{
    const Builtin& object = m_table.at(holder);
    if (object.flags.contains(BuiltinFlag::IsLockedObject))
        return true;
    if (!object.flags.contains(BuiltinFlag::ListsAllMembers) || !m_flow.hasResults())
        return false;
    if (m_flow.builtinHolder(holder) != BuiltinHolderState::Intact || m_flow.usesOf(holder).contains(BuiltinUse::Escaped))
        return false;
    // Whatever comes under a name on an intact holder has a record. The record of `length` speaks of the arrays: the property of Array.prototype itself is locked.
    auto locations = std::ranges::equal_range(m_flow.builtinLocations(), holder, { }, &BuiltinLocationFlow::holder);
    return std::ranges::all_of(locations, [&](const BuiltinLocationFlow& location) {
        return location.state == BuiltinLocationState::Constant || isLockedLocation(m_program, holder, location.key);
    });
}

bool AccessesToObject::chainGainsNoMember(BuiltinID firstHolder) const
{
    if (!firstHolder)
        return false;
    BuiltinID holder = m_table.canonicalID(firstHolder);
    while (true) {
        if (!gainsNoMember(holder))
            return false;
        const Builtin& object = m_table.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = m_table.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

// An environment is no object of the language: it has fields, no prototype and no accessors, and only compiled code reaches it.
bool AccessesToObject::runNoCodeOnEnvironment() const
{
    return OptionSet<ValueFate> { ValueFate::HasMembersRead, ValueFate::HasMembersWritten }.containsAll(m_accesses);
}

bool AccessesToObject::hasNoOwnAccessor(LayoutID id) const
{
    const Layout& layout = m_program.at(id);
    bool isCreatedWithAccessor = std::ranges::any_of(layout.fields, [](const Field& field) {
        return field.form.contains(FieldForm::IsAccessor);
    });
    if (isCreatedWithAccessor)
        return false;
    ReceiverState state = receiverStateOf(m_program, id);
    if (state.cannot(ObjectMutation::RedefineProperty) && state.cannot(ObjectMutation::AddProperty))
        return true;
    return layout.flow.ownAccessorsAreComplete && layout.flow.ownAccessors.isEmpty();
}

bool AccessesToObject::accessRunsNoCodeOnLayout(LayoutID layout, PropertyKeyID key, Access access) const
{
    MemberQuery query { MemberStart::Instance, layout, key, access, false };
    MemberAnswer found = findMember(m_context, query);
    // A private name that is no field is a method or an accessor of its class, which no lookup by key finds.
    if (found.kind != MemberKind::Field)
        return m_program.at(key).kind != PropertyKeyKind::PrivateName && endsWithoutCode(found, access);

    // A data property from its creation on, which no definition turns into an accessor. A store that a frozen object or a read-only property refuses runs nothing.
    FieldReference field { found.holder, found.field };
    if (fieldAt(m_program, field).form.contains(FieldForm::IsAccessor) || !isNeverRedefined(m_program, field))
        return false;
    // Every allocation fills it and nobody deletes it: no read gets past the object. That is said of the reads of the program, and the callee's are among them.
    // It is not said of the stores: the one that fills the field finds it empty, and so does any other that gets hold of an object whose constructors have not finished.
    // A constructor fills by a store, which a setter above the object takes. Flow has looked at what it knows to lie above the objects. Where they may be born below something else,
    // `F[key] = spy` with a key that may be "prototype", it says so of the link and not of the field.
    bool isFilledByStores = !!m_program.at(declaredLayoutOf(m_program, layout)).constructor;
    bool mayBeEmpty = isFilledByStores && (access == Access::Store || !receiverStateOf(m_program, layout).cannot(ObjectMutation::ChangePrototype));
    if (staysFilled(m_program, field) && !mayBeEmpty)
        return true;
    return endsWithoutCode(findMemberOnPrototypes(m_context, query), access);
}

// The key may name anything: an accessor of a class, so only what inherits from built-in prototypes alone gets a "yes". It may be "__proto__": the getter of Object.prototype is the engine's and answers the prototype.
bool AccessesToObject::readUnderAnyKeyRunsNoCodeOnLayout(LayoutID id) const
{
    const Layout& layout = m_program.at(id);
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Container:
        break;
    case LayoutKind::BuiltinResult:
        // A tuple is an array, of whose elements the layout says nothing.
        if (m_table.at(layout.builtinLayout).isTuple)
            return false;
        break;
    case LayoutKind::Instance:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    if (!receiverStateOf(m_program, id).cannot(ObjectMutation::ChangePrototype) || !hasNoOwnAccessor(id))
        return false;
    return chainGainsNoMember(Builtins::ObjectPrototype);
}

bool AccessesToObject::runNoCodeOnLayout(LayoutID layout) const
{
    // The constructor of a built-in base class makes the object, and native code keeps on it what no layout lists.
    if (builtinBaseClassOf(m_program, layout) || !hasOnlyCompiledBases(m_program, layout))
        return false;
    // An object without Symbol.iterator throws, and one with it runs the method.
    if (has(ValueFate::IsIterated))
        return false;
    // Whoever lists the keys may go on to read what they hold.
    if (has(ValueFate::HasKeysEnumerated) && !hasNoOwnAccessor(layout))
        return false;
    return allKeysRunNoCode([&](PropertyKeyID key, Access access) {
        return accessRunsNoCodeOnLayout(layout, key, access);
    }, [&](Access access) {
        // A store under "__proto__" gives the object another prototype.
        return access == Access::Read && readUnderAnyKeyRunsNoCodeOnLayout(layout);
    });
}

bool AccessesToObject::runNoCodeOnLayouts(LayoutSetID layouts) const
{
    Type type = Type::forLayoutSet(layouts);
    ReceiverCandidates candidates = candidatesOf(m_program, type, nullptr, { });
    if (!candidates.isProven() || candidates.hasOtherParts || candidates.proven.isEmpty() || candidates.proven.size() > mostLayouts)
        return false;
    // Listing the own keys of an ordinary object runs nothing. for-in goes up the chain, where a Proxy would have a say.
    if (has(ValueFate::HasKeysEnumerated) && !provenStepOf(m_program, type, Protocol::PrototypeChain).isComplete)
        return false;
    return std::ranges::all_of(candidates.proven, [&](LayoutID layout) {
        return runNoCodeOnLayout(layout);
    });
}

// The method under Symbol.iterator and the `next` of its iterators are what the realm was created with. The iterator reads the length and the elements, or the entries of the table.
// Closing such an iterator looks for `return` on the iterator, not on the object.
bool AccessesToObject::iterationRunsNoCodeOn(const ClassPart& part) const
{
    BuiltinID next = Builtins::ArrayIteratorPrototypeNext;
    if (part.builtinClass == BuiltinClasses::Map)
        next = Builtins::MapIteratorPrototypeNext;
    else if (part.builtinClass == BuiltinClasses::Set)
        next = Builtins::SetIteratorPrototypeNext;
    PropertyKeyID iterator = m_program.keyOf(m_table.nameOf(WellKnownSymbol::Iterator));
    BuiltinLookupContext lookup { m_program, m_flow };
    return iterator && findMemberOfInstances(lookup, part.builtinClass, iterator).isConstant() && locationProofOf(lookup, next) == LocationProof::Constant;
}

// Names: the instance has gained no name, nothing on it was redefined, it has the prototype of its class, and no code that nobody has seen got hold of it.
// Storage, of an array: no access under a number meets an accessor, in the array or above it. So a hole reads through prototypes that are closed to indexes.
// An array that may be frozen or sealed has neither proof.
bool AccessesToObject::runNoCodeOnInstances(const ClassPart& part) const
{
    bool isArray = part.builtinClass == BuiltinClasses::Array;
    if (!isArray && part.builtinClass != BuiltinClasses::Map && part.builtinClass != BuiltinClasses::Set)
        return false;

    bool hasNames = part.provenPristine.contains(PristineAspects::Names);
    bool hasNamesAndElements = hasNames && (!isArray || part.provenPristine.contains(PristineAspects::Storage));
    if (has(ValueFate::IsIterated) && (!hasNamesAndElements || !iterationRunsNoCodeOn(part)))
        return false;
    if (has(ValueFate::HasKeysEnumerated) && (!hasNamesAndElements || !m_table.hasLockedPrototypeChain(part.builtinClass)))
        return false;

    return allKeysRunNoCode([&](PropertyKeyID key, Access access) {
        if (m_program.isIndexLike(key))
            return isArray && part.provenPristine.contains(PristineAspects::Storage);
        // `length` of an array: a data property that nothing deletes or redefines. What a store makes a number of is the stored value.
        BuiltinName name = m_program.builtinNameOf(key);
        if (name && m_table.findOwnPropertyOfInstances(part.builtinClass, name))
            return true;
        return hasNames && endsWithoutCode(findBuiltinInstanceMember(m_context, part.builtinClass, key, access), access);
    }, [&](Access) {
        // `array[i]`, mostly. The key may as well be a name: then it finds `length`, or what the table lists on the prototypes, or nothing. Under "__proto__" it finds the accessor of Object.prototype,
        // which is the engine's. A store under a key that may be a name costs the array the proof of its names, so the stores that reach this one are stores of elements.
        return isArray && hasNamesAndElements && chainGainsNoMember(m_table.at(part.builtinClass).prototype);
    });
}

// A closure has `length`, `name` and maybe `prototype`, data properties that the engine makes, and inherits from Function.prototype.
bool AccessesToObject::runNoCodeOnClosures(FunctionID id) const
{
    const Function& function = m_program.at(id);
    // A class object is created with the static members of its definition, below the class object of its base. The closure of a coroutine inherits from the prototype of its own kind of function.
    LayoutID constructedClass = function.constructedClass;
    if (constructedClass && !m_program.at(constructedClass).has(LayoutFlags::IsConstructorFunction))
        return false;
    if (function.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) || function.role == FunctionRole::BoundFunction)
        return false;
    if (has(ValueFate::IsIterated))
        return false;
    // What a store has added is data: a definition of an accessor would have cost the closures the answer.
    if (has(ValueFate::HasKeysEnumerated) && (!m_flow.areClosuresAsCreated(id, { }) || !m_table.hasLockedPrototypeChain(BuiltinClasses::Function)))
        return false;

    return allKeysRunNoCode([&](PropertyKeyID key, Access access) {
        return m_flow.areClosuresAsCreated(id, singleElementSpan(key)) && endsWithoutCode(findBuiltinInstanceMember(m_context, BuiltinClasses::Function, key, access), access);
    }, [](Access) {
        return false;
    });
}

// Symbol.toPrimitive, valueOf and toString are looked up and what they hold is called with the object as `this`. Object.prototype.toString asks the object for Symbol.toStringTag.
bool AccessesToObject::conversionRunsNoCodeOn(const Type& type) const
{
    if (membersRunBy(m_program, type, Protocol::ToPrimitive, { }, { }, NamedMembers::AreOnlyRead).mayRunUserCode())
        return false;
    ProvenAccess tag = provenReadOf(m_program, type, m_table.nameOf(WellKnownSymbol::ToStringTag));
    return tag.isComplete && tag.accessors.isEmpty();
}

bool AccessesToObject::runNoCodeOn(const Type& type) const
{
    // Tonight nobody says under which key, and a definition may put an accessor there that the next read of the same callee runs.
    if (has(ValueFate::HasMembersDeletedOrDefined) || !namesItsKeys())
        return false;
    if (type.primitives || type.anyObject || type.anyFunction || type.opaque || !type.builtins.isEmpty())
        return false;
    if (type.environment)
        return !type.hasObjects() && runNoCodeOnEnvironment();
    if (!type.hasObjects())
        return false;

    if (has(ValueFate::IsConvertedToPrimitive) && !conversionRunsNoCodeOn(type))
        return false;
    if (type.layouts && !runNoCodeOnLayouts(type.layouts))
        return false;
    bool runNoCodeOnAllInstances = std::ranges::all_of(type.classes, [&](const ClassPart& part) {
        return runNoCodeOnInstances(part);
    });
    return runNoCodeOnAllInstances && std::ranges::all_of(type.functions, [&](FunctionID function) {
        return runNoCodeOnClosures(function);
    });
}

bool AccessesToObject::allStoresLand(NOESCAPE const Invocable<bool(PropertyKeyID)> auto& underKey, NOESCAPE const Invocable<bool()> auto& underAnyKey) const
{
    return allKeysRunNoCode([&](PropertyKeyID key, Access access) {
        return access == Access::Read || underKey(key);
    }, [&](Access access) {
        return access == Access::Read || underAnyKey();
    });
}

// A store that runs no code may still do nothing: a frozen object refuses it, one that is not extensible refuses to create the property, a read-only property refuses it under its name,
// and so does a read-only property of that name anywhere above an object that has none of its own. In sloppy code nobody hears of it.
// A method of a class that a store passes by is not among the answers: the lookup has gone on above it. What lies on a built-in prototype under the name is taken to refuse.
bool AccessesToObject::storeLandsOnLayout(LayoutID layout, PropertyKeyID key) const
{
    if (builtinBaseClassOf(m_program, layout) || !hasOnlyCompiledBases(m_program, layout) || !receiverStateOf(m_program, layout).cannot(ObjectMutation::ChangeIntegrity))
        return false;
    auto findsNothing = [](const MemberAnswer& answer) {
        return answer.needsNoTest() && answer.kind == MemberKind::Absent;
    };
    MemberQuery query { MemberStart::Instance, layout, key, Access::Store, false };
    MemberAnswer found = findMember(m_context, query);
    if (found.kind != MemberKind::Field)
        return m_program.at(key).kind != PropertyKeyKind::PrivateName && findsNothing(found);

    FieldReference field { found.holder, found.field };
    if (fieldAt(m_program, field).form.containsAny({ FieldForm::IsAccessor, FieldForm::IsReadOnly }) || !isNeverRedefined(m_program, field))
        return false;
    bool mayBeUnfinished = !!m_program.at(declaredLayoutOf(m_program, layout)).constructor;
    if (staysFilled(m_program, field) && !mayBeUnfinished)
        return true;
    return findsNothing(findMemberOnPrototypes(m_context, query));
}

// An array makes a number of what is stored as its length, so what it then holds is not what was stored.
bool AccessesToObject::storesLandOnInstances(const ClassPart& part) const
{
    bool isArray = part.builtinClass == BuiltinClasses::Array;
    if (!isArray && part.builtinClass != BuiltinClasses::Map && part.builtinClass != BuiltinClasses::Set)
        return false;
    bool hasNames = part.provenPristine.contains(PristineAspects::Names);
    bool hasElements = isArray && part.provenPristine.contains(PristineAspects::Storage);
    return allStoresLand([&](PropertyKeyID key) {
        if (m_program.isIndexLike(key))
            return hasElements;
        MemberAnswer found = findBuiltinInstanceMember(m_context, part.builtinClass, key, Access::Store);
        return hasNames && found.needsNoTest() && found.kind == MemberKind::Absent;
    }, [&] {
        return hasNames && hasElements && chainGainsNoMember(m_table.at(part.builtinClass).prototype);
    });
}

bool AccessesToObject::storesLandOn(const Type& type) const
{
    if (has(ValueFate::HasMembersDeletedOrDefined) || !namesItsKeys())
        return false;
    if (type.primitives || type.anyObject || type.anyFunction || type.opaque || !type.builtins.isEmpty() || !type.functions.isEmpty())
        return false;
    if (type.environment)
        return !type.hasObjects();
    if (!type.hasObjects())
        return false;

    if (type.layouts) {
        ReceiverCandidates candidates = candidatesOf(m_program, Type::forLayoutSet(type.layouts), nullptr, { });
        if (!candidates.isProven() || candidates.hasOtherParts || candidates.proven.isEmpty() || candidates.proven.size() > mostLayouts)
            return false;
        bool landOnAllLayouts = std::ranges::all_of(candidates.proven, [&](LayoutID layout) {
            return allStoresLand([&](PropertyKeyID key) {
                return storeLandsOnLayout(layout, key);
            }, [] {
                return false;
            });
        });
        if (!landOnAllLayouts)
            return false;
    }
    return std::ranges::all_of(type.classes, [&](const ClassPart& part) {
        return storesLandOnInstances(part);
    });
}

static bool runsNoOtherCodeOn(const Program& program, const FlowResults& flow, const Type& typeOfObject, const LinkedFate& fate, GettersOfProgram* getters)
{
    if (fate.isAnything())
        return false;
    if (!fate.itself.containsAny(fatesThatMayRunCodeOfTheObject))
        return true;
    return AccessesToObject(program, flow, fate, getters).runNoCodeOn(typeOfObject);
}

bool runsNoCodeOn(const Program& program, const FlowResults& flow, const Type& typeOfObject, const LinkedFate& fate)
{
    return runsNoOtherCodeOn(program, flow, typeOfObject, fate, nullptr);
}

bool storesLandInOwnFields(const Program& program, const FlowResults& flow, const Type& typeOfObject, const LinkedFate& fate)
{
    if (fate.isAnything())
        return false;
    if (!fate.itself.containsAny({ ValueFate::HasMembersWritten, ValueFate::HasMembersDeletedOrDefined }) && fate.keysWritten.isEmpty())
        return true;
    return AccessesToObject(program, flow, fate).storesLandOn(typeOfObject);
}

std::optional<GettersOfProgram> gettersRunOn(const Program& program, const FlowResults& flow, const Type& typeOfObject, const LinkedFate& fate)
{
    GettersOfProgram getters;
    if (!runsNoOtherCodeOn(program, flow, typeOfObject, fate, &getters))
        return std::nullopt;
    return getters;
}

// The object is of both types. The parts of `arriving` that `operand` rules out speak of what other callers pass: undefined where an argument is left out, objects of another kind.
static Type partsThatMayBeOperand(const Type& arriving, const Type& operand)
{
    Type type = arriving.withOnlyObjects();
    if (operand.anyObject)
        return type;
    if (!operand.layouts)
        type.layouts = { };
    type.classes.removeAllMatching([&](const ClassPart& part) {
        return !operand.findClassPart(part.builtinClass);
    });
    if (!operand.anyFunction) {
        SortedSet<FunctionID> functions;
        for (FunctionID function : type.functions) {
            if (operand.functions.contains(function))
                functions.add(function);
        }
        type.functions = WTF::move(functions);
        // What is left of "any function" is a function that the operand lists, of which this type then says nothing.
        if (type.anyFunction && operand.functions.isEmpty())
            type.anyFunction = false;
    }
    if (operand.builtins.isEmpty())
        type.builtins.clear();
    return type;
}

// The caller has two descriptions of the object that it passes: the type of the operand, and what flow has proven of everything that arrives at the entry parameter which the operand becomes.
// Only the second has what flow proves of an array, a Map or a Set, and the layout of an instance whose class is one of several. Whichever callee is entered, the object is among what arrives there.
static bool runsNoOtherCodeOnOperandAt(const Program& program, const FlowResults& flow, const Function& caller, const Value& call, uint32_t operandIndex, const LinkedFate& fate, GettersOfProgram* getters)
{
    const Type& operand = program.typeOf(caller, call.operand(operandIndex));
    if (runsNoOtherCodeOn(program, flow, operand, fate, getters))
        return true;
    if (fate.isAnything() || !operand.isObject())
        return false;
    if (getters)
        getters->clear();

    // A bound function puts its own arguments in front of these and its own `this` in the place of this one.
    if (Value::opcodesWithAccessSite.contains(call.opcode) && call.accessSite() && flow.mayCallBoundFunction(call.accessSite()))
        return false;
    CalleeList callees = calleesOfCall(program, call);
    if (!callees.isComplete || !callees.builtins.isEmpty() || callees.functions.isEmpty())
        return false;
    return std::ranges::all_of(callees.functions, [&](FunctionID id) {
        const Function& callee = program.at(id);
        std::optional<uint32_t> parameter = entryParameterOf(callee, call, operandIndex);
        if (!parameter || !callee.hasBody() || *parameter >= callee.blocks[0].parameters.size())
            return false;
        return runsNoOtherCodeOn(program, flow, partsThatMayBeOperand(program.typeOf(callee, callee.blocks[0].parameters[*parameter]), operand), fate, getters);
    });
}

bool runsNoCodeOnOperandAt(const Program& program, const FlowResults& flow, const Function& caller, const Value& call, uint32_t operandIndex, const LinkedFate& fate)
{
    return runsNoOtherCodeOnOperandAt(program, flow, caller, call, operandIndex, fate, nullptr);
}

std::optional<GettersOfProgram> gettersRunOnOperandAt(const Program& program, const FlowResults& flow, const Function& caller, const Value& call, uint32_t operandIndex, const LinkedFate& fate)
{
    GettersOfProgram getters;
    if (!runsNoOtherCodeOnOperandAt(program, flow, caller, call, operandIndex, fate, &getters))
        return std::nullopt;
    return getters;
}

// A construction answers `this` where the constructor returns no object, and the result of the constructor says nothing of `this`.
static bool mayAnswerThis(const Value& call)
{
    return OperandPosition::newTarget(call.opcode) || (call.opcode == Opcode::CallDirect && call.isConstruction());
}

static const Type* findProvenResult(const Program& program, const Function& function, uint32_t result)
{
    if (function.isUnsplitCoroutine() || result >= function.results.size())
        return nullptr;
    const DeclaredType& declared = function.results[result];
    return declared.type && !declared.flags.contains(PositionFlags::IsExpected) ? &program.at(declared.type) : nullptr;
}

// A cast in the block of the operation runs whenever the operation has run: what it says depends on no test. One in another block may stand behind a test that only some of the objects pass.
ProofsBehindValues::ProofsBehindValues(const Function& function)
{
    constexpr uint32_t noBlock = std::numeric_limits<uint32_t>::max();
    Vector<uint32_t> blockOfValue(function.values.size());
    blockOfValue.fill(noBlock);
    for (uint32_t block = 0; block < function.blocks.size(); ++block) {
        for (ValueID id : function.blocks[block].values) {
            blockOfValue[id.index()] = block;
            const Value& value = function.at(id);
            if (value.opcode == Opcode::AssumeType && blockOfValue[value.operand(0).index()] == block)
                m_proofs.append({ value.operand(0), value.type });
        }
    }
    std::ranges::stable_sort(m_proofs, { }, &Proof::value);
}

TypeID ProofsBehindValues::find(ValueID value) const
{
    auto found = std::ranges::lower_bound(m_proofs, value, { }, &Proof::value);
    return found != m_proofs.end() && found->value == value ? found->type : TypeID();
}

// Every description is of all the objects that one place of the program makes or lets through, whichever way each of them goes on from there. So any of them does for all the accesses of the fate.
static bool runsNoOtherCodeOnObjectMadeBy(const Program& program, const FlowResults& flow, const Function& function, const ProofsBehindValues& proofs, ValueID id, const LinkedFate& fate, GettersOfProgram* getters)
{
    const Value& value = function.at(id);
    if (fate.isAnything() || !value.type)
        return false;
    auto ask = [&](const Type& type) {
        if (getters)
            getters->clear();
        // What is no object has no code.
        return runsNoOtherCodeOn(program, flow, type.primitives ? type.withOnlyObjects() : type, fate, getters);
    };
    const Type& typeOfValue = program.at(value.type);
    if (ask(typeOfValue))
        return true;
    if (TypeID proven = proofs.find(id); proven && ask(program.at(proven)))
        return true;

    for (uint32_t result = 0; result < std::min<size_t>(function.results.size(), 32); ++result) {
        const Type* returned = fate.results >> result & 1 ? findProvenResult(program, function, result) : nullptr;
        if (returned && ask(partsThatMayBeOperand(*returned, typeOfValue)))
            return true;
    }

    if (effectRuleOf(value.opcode) != EffectRule::Call)
        return false;
    if (getters)
        getters->clear();
    CalleeList callees = calleesOfCall(program, value);
    if (!callees.isComplete || !callees.builtins.isEmpty() || callees.functions.isEmpty())
        return false;
    return std::ranges::all_of(callees.functions, [&](FunctionID calleeID) {
        const Function& callee = program.at(calleeID);
        const Type* answered = callee.results.size() == 1 ? findProvenResult(program, callee, 0) : nullptr;
        if (!answered || (answered->primitives && mayAnswerThis(value)))
            return false;
        return runsNoOtherCodeOn(program, flow, partsThatMayBeOperand(*answered, typeOfValue), fate, getters);
    });
}

bool runsNoCodeOnObjectMadeBy(const Program& program, const FlowResults& flow, const Function& function, const ProofsBehindValues& proofs, ValueID allocationOrCall, const LinkedFate& fate)
{
    return runsNoOtherCodeOnObjectMadeBy(program, flow, function, proofs, allocationOrCall, fate, nullptr);
}

std::optional<GettersOfProgram> gettersRunOnObjectMadeBy(const Program& program, const FlowResults& flow, const Function& function, const ProofsBehindValues& proofs, ValueID allocationOrCall, const LinkedFate& fate)
{
    GettersOfProgram getters;
    if (!runsNoOtherCodeOnObjectMadeBy(program, flow, function, proofs, allocationOrCall, fate, &getters))
        return std::nullopt;
    return getters;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
