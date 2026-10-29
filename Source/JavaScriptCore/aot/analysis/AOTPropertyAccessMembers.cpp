/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyAccessMembers.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowResults.h"
#include "AOTPropertyAccessCache.h"
#include "AOTPropertyAccessInternal.h"
#include <algorithm>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/EnumTraits.h>

namespace JSC { namespace AOT { namespace Typed {

static void dumpFunction(PrintStream& out, FunctionID function)
{
    if (!function) {
        out.print("none");
        return;
    }
    out.print("F", function.index());
}

FunctionID MemberAnswer::accessorFor(Access access) const
{
    return access == Access::Read ? getter : access == Access::Store ? setter : FunctionID();
}

void MemberAnswer::dump(PrintStream& out) const
{
    out.print(enumName(kind), " level=", nameOf(level));
    if (holder)
        out.print(" holder=L", holder.index());
    switch (kind) {
    case MemberKind::Unknown:
    case MemberKind::Absent:
        break;
    case MemberKind::Field:
        if (holder)
            out.print(" field=", field);
        break;
    case MemberKind::Method:
        out.print(" method=");
        dumpFunction(out, method);
        break;
    case MemberKind::Accessor:
        out.print(" getter=");
        dumpFunction(out, getter);
        out.print(" setter=");
        dumpFunction(out, setter);
        break;
    case MemberKind::Variable:
        out.print(" variable=V", variable.index());
        break;
    case MemberKind::Namespace:
        out.print(" module=M", module.index());
        break;
    case MemberKind::Builtin:
        out.print(" builtin=B", builtin.index());
        break;
    }
    if (!receiverProofs.isEmpty()) {
        out.print(" proofs=");
        dumpProofs(out, receiverProofs);
    }
    if (!conditions.isEmpty()) {
        out.print(" conditions=");
        dumpConditions(out, conditions);
    }
    if (kind == MemberKind::Unknown)
        out.print(" cause=", nameOf(cause));
}

KeyLookup keyLookupOf(const Program& program, PropertyKeyID key)
{
    if (!key)
        return KeyLookup::ByKey;
    switch (program.nameClassOf(key)) {
    case NameClass::Index:
    case NameClass::CanonicalNumericString:
    case NameClass::Proto:
        return KeyLookup::ByKey;
    case NameClass::Ordinary:
    case NameClass::Constructor:
    case NameClass::ObjectPrototypeMember:
    case NameClass::Length:
    case NameClass::Prototype:
    case NameClass::CallOrApply:
    case NameClass::Bind:
    case NameClass::LastIndex:
    case NameClass::Then:
    case NameClass::ToJSON:
        return KeyLookup::Ordinary;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LayoutID classOfClassObject(const Program& program, const Type& classObject)
{
    if (!classObject.isJavaScriptValue())
        return { };
    FunctionID constructor = classObject.singleFunction();
    return constructor ? program.at(constructor).constructedClass : LayoutID();
}

void setLevel(MemberAnswer& answer)
{
    if (answer.kind == MemberKind::Unknown)
        answer.level = KnowledgeLevel::Unknown;
    else
        answer.level = answer.receiverProofs.isEmpty() && answer.conditions.isEmpty() ? KnowledgeLevel::Proven : KnowledgeLevel::Expected;
}

void setDeclaredMember(MemberAnswer& answer, const DeclaredMember& member, LayoutID holder)
{
    answer.holder = holder;
    if (member.isAccessor()) {
        answer.kind = MemberKind::Accessor;
        answer.getter = member.getter;
        answer.setter = member.setter;
        return;
    }
    answer.kind = MemberKind::Method;
    answer.method = member.method;
}

static void addReceiverProof(MemberAnswer& walked, Proof proof, UnprovenCause cause)
{
    walked.receiverProofs.add(proof);
    if (walked.cause == UnprovenCause::Unknown)
        walked.cause = cause;
}

static void requireLookupPastObject(const Program& program, MemberAnswer& walked, const MemberAnswer& found, const MemberQuery& query, Proof proof, UnprovenCause cause)
{
    if (isMemberOfClass(found) && !builtinBaseClassOf(program, query.layout))
        addConditionWithCause(walked, { MemberCondition::Kind::MemberNotHidden, declaredLayoutOf(program, query.layout), ClassSide::Prototype, query.key }, cause);
    else
        addReceiverProof(walked, proof, cause);
}

static bool isPrivateName(const Program& program, PropertyKeyID key)
{
    switch (program.at(key).kind) {
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::EnginePrivateName:
        return true;
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FunctionID& functionOfKind(DeclaredMember& member, MethodKind kind)
{
    switch (kind) {
    case MethodKind::Method:
        return member.method;
    case MethodKind::Getter:
        return member.getter;
    case MethodKind::Setter:
        return member.setter;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void defineMember(DeclaredMember& member, const ClassMember& definition)
{
    switch (definition.kind) {
    case MethodKind::Method:
        member.getter = { };
        member.setter = { };
        break;
    case MethodKind::Getter:
    case MethodKind::Setter:
        member.method = { };
        break;
    }
    functionOfKind(member, definition.kind) = definition.function;
}

static DeclaredMember findMemberAsDefined(const ClassFlow& classFlow, ClassSide side, PropertyKeyID key)
{
    DeclaredMember member;
    member.mutability = MemberMutability::Unknown;
    for (const ClassMember& definition : classFlow.members) {
        if (definition.side == side && definition.key == key)
            defineMember(member, definition);
    }
    return member;
}

static DeclaredMember findNamedMember(const ClassFlow& classFlow, ClassSide side, PropertyKeyID key)
{
    auto sideAndKeyOf = [](const ClassMember& entry) {
        return std::pair { entry.side, entry.key };
    };

    DeclaredMember member;
    bool hasCompetingDefinitions = false;
    auto entries = std::ranges::equal_range(classFlow.namedMembers, std::pair { side, key }, { }, sideAndKeyOf);
    member.isWritableData = !entries.empty() && std::ranges::all_of(entries, &ClassMember::isWritableData);
    for (const ClassMember& entry : entries) {
        FunctionID& function = functionOfKind(member, entry.kind);
        if (function && function != entry.function)
            hasCompetingDefinitions = true;
        function = entry.function;
        if (entry.mutability != MemberMutability::Fixed)
            member.mutability = MemberMutability::Unknown;
    }
    if (member.method && member.isAccessor()) {
        hasCompetingDefinitions = true;
        member.method = { };
    }
    if (hasCompetingDefinitions)
        member.mutability = MemberMutability::Unknown;
    return member;
}

DeclaredMember findDeclaredMember(const ClassFlow& classFlow, ClassSide side, PropertyKeyID key)
{
    DeclaredMember member = findNamedMember(classFlow, side, key);
    if (member.mutability == MemberMutability::Fixed)
        return member;
    DeclaredMember defined = findMemberAsDefined(classFlow, side, key);
    defined.isWritableData = member.isWritableData;
    return defined;
}

static MemberMutability linkMutabilityOf(const ClassFlow& classFlow, ClassSide side)
{
    switch (side) {
    case ClassSide::Prototype:
        return classFlow.prototypeLink;
    case ClassSide::Static:
        return classFlow.staticLink;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

NextHolder nextHolderOf(const Program& program, LayoutID classLayout, ClassSide side, MemberConditions& conditions)
{
    const Layout& layout = program.at(classLayout);
    bool hasHeritage = layout.has(LayoutFlags::HasHeritage);
    MemberMutability link = linkMutabilityOf(layout.classFlow, side);

    NextHolder next;
    switch (layout.flow.baseState) {
    case BaseState::Layout:
        next = { ChainStep::CompiledClass, layout.flow.base, { } };
        break;
    case BaseState::BuiltinClass:
        next = { ChainStep::BuiltinClass, { }, layout.flow.baseClass };
        break;
    case BaseState::None:
        if (LayoutID base = side == ClassSide::Prototype ? expectedBaseOf(layout) : LayoutID())
            next = { ChainStep::CompiledClass, base, { } };
        else if (!hasHeritage || side == ClassSide::Static)
            next.step = ChainStep::NoBase;
        break;
    case BaseState::Unknown:
        if (LayoutID expected = side == ClassSide::Prototype ? expectedBaseOf(layout) : predictedHeritageOf(layout)) {
            link = MemberMutability::Unknown;
            next = { ChainStep::CompiledClass, expected, { } };
        } else if (!hasHeritage)
            next.step = ChainStep::NoBase;
        break;
    }
    if (next.step != ChainStep::Unknown && link != MemberMutability::Fixed)
        addCondition(conditions, { MemberCondition::Kind::PrototypeLinkUnchanged, classLayout, side, { } });
    return next;
}

BuiltinLocationAnswer builtinLocationAnswerOf(const PropertyAccessContext& context, BuiltinID member)
{
    if (!member)
        return BuiltinLocationAnswer::Unknown;
    const BuiltinTable& table = context.program.builtins();
    if (table.has(BuiltinMark::MayBeAbsent, member))
        return BuiltinLocationAnswer::Unknown;
    if (table.at(member).mutability == BuiltinMutability::AlwaysLocked || table.isLocationOfLockedObject(member))
        return BuiltinLocationAnswer::Constant;
    if (context.flow && context.flow->builtinLocation(member) == BuiltinLocationState::Constant)
        return BuiltinLocationAnswer::Constant;
    return BuiltinLocationAnswer::Unknown;
}

BuiltinHolderGain builtinHolderGainOf(const PropertyAccessContext& context, BuiltinID holder, PropertyKeyID key)
{
    if (!holder)
        return BuiltinHolderGain::Unknown;
    holder = context.program.builtins().canonicalID(holder);
    if (!listsEveryMemberUnder(context.program, holder, key))
        return BuiltinHolderGain::Unknown;
    if (isClosedKey(context.program, holder, key))
        return BuiltinHolderGain::Impossible;

    return context.flow && context.flow->builtinHolderNeverGains(holder, key) ? BuiltinHolderGain::Impossible : BuiltinHolderGain::Unknown;
}

MemberAnswer findBuiltinObjectMember(const PropertyAccessContext& context, BuiltinID holder, PropertyKeyID key, Access)
{
    const Program& program = context.program;
    if (!holder)
        return MemberAnswer::unknown(UnprovenCause::UnknownBase);
    bool isLookedUpByName = keyLookupOf(program, key) == KeyLookup::Ordinary || (key && program.nameClassOf(key) == NameClass::CanonicalNumericString);
    if (!isLookedUpByName || isPrivateName(program, key))
        return MemberAnswer::unknown(UnprovenCause::Unknown);

    const BuiltinTable& table = program.builtins();
    BuiltinName name = program.builtinNameOf(key);
    BuiltinID current = table.canonicalID(holder);
    while (true) {
        if (BuiltinID member = name ? table.findOwnMember(current, name) : BuiltinID()) {
            if (builtinLocationAnswerOf(context, member) != BuiltinLocationAnswer::Constant)
                return MemberAnswer::unknown(UnprovenCause::UnknownWriter);
            return { .kind = MemberKind::Builtin, .level = KnowledgeLevel::Proven, .builtin = member };
        }
        if (builtinHolderGainOf(context, current, key) != BuiltinHolderGain::Impossible)
            return MemberAnswer::unknown(UnprovenCause::PropertyAdded);

        const Builtin& object = table.at(current);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return MemberAnswer::unknown(UnprovenCause::UnknownBase);
        case BuiltinPrototypeLink::Null:
            return createProvenAnswer(MemberKind::Absent);
        case BuiltinPrototypeLink::Builtin:
            current = table.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

static uint32_t hashOf(const PropertyAccessCache::InheritedMemberKey&);

static MemberAnswer findOnce(const PropertyAccessContext& context, const PropertyAccessCache::InheritedMemberKey& key, NOESCAPE const Invocable<MemberAnswer()> auto& find)
{
    PropertyAccessCache* cache = context.program.propertyAccessCache();
    if (!cache || TypeTable::isProvisional(key.key) || TypeTable::isProvisional(key.declaredClass))
        return find();

    const MemberAnswer& kept = findOrCompute(cache->inheritedMembers, key, hashOf(key), [&] {
        return PropertyAccessCache::InheritedMember { key, find() };
    }).value;
    RELEASE_ASSERT(!cache->checksAnswers || kept == find());
    return kept;
}

static uint32_t hashOf(const PropertyAccessCache::InheritedMemberKey& key)
{
    return computeHash(key.declaredClass, key.builtinClass, key.isOfInstances, key.key, key.access, key.flow);
}

static const MemberAnswer* findKeptAnswer(const PropertyAccessCache& cache, const PropertyAccessCache::InheritedMemberKey& key)
{
    const PropertyAccessCache::InheritedMember* kept = cache.inheritedMembers.find(hashOf(key), [&](const PropertyAccessCache::InheritedMember& entry) {
        return entry.key == key;
    });
    return kept ? &kept->value : nullptr;
}

static MemberAnswer findMemberFromPrototypeOf(const PropertyAccessContext& context, BuiltinClassID builtinClass, PropertyKeyID key, Access access)
{
    if (!builtinClass)
        return MemberAnswer::unknown(UnprovenCause::UnknownBase);
    if (keyLookupOf(context.program, key) != KeyLookup::Ordinary)
        return MemberAnswer::unknown(UnprovenCause::Unknown);
    return findOnce(context, { { }, builtinClass, false, key, access, context.flow }, [&] {
        return findBuiltinObjectMember(context, context.program.builtins().at(builtinClass).prototype, key, access);
    });
}

MemberAnswer findBuiltinInstanceMember(const PropertyAccessContext& context, BuiltinClassID builtinClass, PropertyKeyID key, Access access)
{
    if (!builtinClass)
        return MemberAnswer::unknown(UnprovenCause::UnknownBase);
    return findOnce(context, { { }, builtinClass, true, key, access, context.flow }, [&] {
        BuiltinMemberName name = builtinMemberNameOf(context.program, key);
        if (name && context.program.builtins().isRedefinedBelow(builtinClass, name.name))
            return MemberAnswer::unknown(UnprovenCause::UnknownBase);
        return findMemberFromPrototypeOf(context, builtinClass, key, access);
    });
}

MemberAnswer finishAtChainEnd(MemberAnswer&& walked, MemberAnswer&& chainEnd)
{
    if (!chainEnd.isKnown())
        return MemberAnswer::unknown(chainEnd.cause);
    chainEnd.receiverProofs |= walked.receiverProofs;
    addConditions(chainEnd.conditions, walked.conditions.span());
    if (walked.cause != UnprovenCause::Unknown)
        chainEnd.cause = walked.cause;
    setLevel(chainEnd);
    return WTF::move(chainEnd);
}

// A walk that has noted nothing when it comes to a class goes on as the walk that begins there, step for step. The two name their first class in what they note, and nowhere else.
// So an answer without conditions is the answer of both, and a long row of classes is walked once for a name, not once from each of its classes.
struct ClassesPassed {
    PropertyAccessCache* cache { nullptr };
    Vector<LayoutID, 8> classes;
};

static MemberAnswer walkPrototypes(const PropertyAccessContext& context, const MemberQuery& query, ClassesPassed& passed)
{
    const Program& program = context.program;
    PropertyKeyID key = query.key;
    if (keyLookupOf(program, key) != KeyLookup::Ordinary)
        return MemberAnswer::unknown(UnprovenCause::Unknown);

    bool isConstructorKey = program.nameClassOf(key) == NameClass::Constructor;
    MemberCondition memberUnchanged { MemberCondition::Kind::MemberUnchanged, query.layout, ClassSide::Prototype, key };
    MemberAnswer walked;
    const bool asksRunTime = query.access == Access::Store && program.at(query.layout).classFlow.evaluations == ExecutionCount::AtMostOnce;
    bool mayBeHidden = false;
    auto finish = [&](MemberAnswer&& chainEnd) {
        MemberAnswer answer = finishAtChainEnd(WTF::move(walked), WTF::move(chainEnd));
        if (mayBeHidden && answer.isKnown() && answer.kind != MemberKind::Absent)
            return MemberAnswer::unknown(UnprovenCause::PropertyAdded);
        if (!asksRunTime || answer.kind != MemberKind::Absent)
            return answer;
        bool asksForMember = answer.conditions.removeAllMatching([&](const MemberCondition& condition) {
            return condition == memberUnchanged;
        });
        if (asksForMember)
            addCondition(answer.conditions, { MemberCondition::Kind::MemberPassedByStores, query.layout, ClassSide::Prototype, key });
        return answer;
    };
    LayoutID current = query.layout;
    for (size_t unvisitedClasses = program.layouts.size(); unvisitedClasses; --unvisitedClasses) {
        const Layout& layout = program.at(current);
        if (passed.cache && current != query.layout) {
            bool hasNotedNothing = walked.conditions.isEmpty() && walked.cause == UnprovenCause::Unknown && !mayBeHidden;
            bool asksRunTimeFromHere = query.access == Access::Store && layout.classFlow.evaluations == ExecutionCount::AtMostOnce;
            if (!hasNotedNothing || asksRunTimeFromHere != asksRunTime || TypeTable::isProvisional(current))
                passed.cache = nullptr;
            else {
                const MemberAnswer* kept = findKeptAnswer(*passed.cache, { current, { }, false, key, query.access, context.flow });
                if (kept && kept->conditions.isEmpty())
                    return *kept;
                if (kept)
                    passed.cache = nullptr;
                else
                    passed.classes.append(current);
            }
        }
        bool isOfCreatedObject = layout.has(LayoutFlags::IsOfCreatedObject);
        if (layout.kind != LayoutKind::Instance || (!layout.constructor && (!isOfCreatedObject || layout.classFlow.evaluations != ExecutionCount::AtMostOnce)))
            return MemberAnswer::unknown(UnprovenCause::NonLayoutObject);
        if (isConstructorKey && isOfCreatedObject)
            return MemberAnswer::unknown(UnprovenCause::Unknown);
        if (isConstructorKey) {
            bool holdsOneClassObject = layout.classFlow.constructorProperty == MemberMutability::Fixed && layout.classFlow.evaluations == ExecutionCount::AtMostOnce && program.at(layout.constructor).captures.isEmpty();
            if (query.access != Access::Read || !holdsOneClassObject)
                return MemberAnswer::unknown(UnprovenCause::Unknown);
            setDeclaredMember(walked, { .method = layout.constructor }, current);
            setLevel(walked);
            return walked;
        }

        DeclaredMember member = findDeclaredMember(layout.classFlow, ClassSide::Prototype, key);
        bool isPassedByStore = query.access == Access::Store && member.isWritableData;
        if (isPassedByStore && member.mutability == MemberMutability::Fixed)
            return finish(createProvenAnswer(MemberKind::Absent));
        mayBeHidden |= isPassedByStore;
        bool isMethodLeftToRunTime = asksRunTime && !isPassedByStore && member.method && !member.isAccessor() && member.mutability != MemberMutability::Fixed;
        if (member.isDeclared() && !isPassedByStore && !isMethodLeftToRunTime) {
            if (mayBeHidden)
                return MemberAnswer::unknown(UnprovenCause::PropertyAdded);
            if (member.mutability != MemberMutability::Fixed)
                addConditionWithCause(walked, memberUnchanged, UnprovenCause::PropertyRedefined);
            setDeclaredMember(walked, member, current);
            setLevel(walked);
            return walked;
        }
        if ((member.isListedWithoutDefinition() || isMethodLeftToRunTime) && !isPassedByStore) {
            if (!asksRunTime)
                return MemberAnswer::unknown(UnprovenCause::PropertyAdded);
            addConditionWithCause(walked, memberUnchanged, UnprovenCause::PropertyAdded);
            return finish(createProvenAnswer(MemberKind::Absent));
        }
        if (!layout.classFlow.prototypeNamesAreComplete)
            addConditionWithCause(walked, memberUnchanged, UnprovenCause::UnknownKeyStore);

        NextHolder next = nextHolderOf(program, current, ClassSide::Prototype, walked.conditions);
        switch (next.step) {
        case ChainStep::Unknown:
            return MemberAnswer::unknown(UnprovenCause::UnknownBase);
        case ChainStep::CompiledClass:
            if (context.usesOnlyAnswersWithoutTests && !walked.conditions.isEmpty())
                return MemberAnswer::unknown(walked.cause);
            current = next.layout;
            continue;
        case ChainStep::BuiltinClass:
            return finish(findMemberFromPrototypeOf(context, next.builtinClass, key, query.access));
        case ChainStep::NoBase:
            return finish(findMemberFromPrototypeOf(context, BuiltinClasses::Object, key, query.access));
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    return MemberAnswer::unknown(UnprovenCause::UnknownBase);
}

MemberAnswer findPrototypeMember(const PropertyAccessContext& context, const MemberQuery& query)
{
    ClassesPassed passed { TypeTable::isProvisional(query.key) ? nullptr : context.program.propertyAccessCache(), { } };
    PropertyAccessCache* cache = passed.cache;
    MemberAnswer answer = walkPrototypes(context, query, passed);
    if (!cache)
        return answer;
    if (cache->checksAnswers) {
        ClassesPassed none;
        RELEASE_ASSERT(answer == walkPrototypes(context, query, none));
    }
    if (!answer.conditions.isEmpty() || passed.classes.size() < PropertyAccessCache::fewestClassesAbove)
        return answer;
    for (LayoutID passedClass : passed.classes) {
        PropertyAccessCache::InheritedMemberKey key { passedClass, { }, false, query.key, query.access, context.flow };
        cache->inheritedMembers.add(hashOf(key), PropertyAccessCache::InheritedMember { key, answer }, [&](const PropertyAccessCache::InheritedMember& entry) {
            return entry.key == key;
        });
    }
    return answer;
}

static bool isTupleLayout(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    return layout.kind == LayoutKind::BuiltinResult && program.builtins().at(layout.builtinLayout).isTuple;
}

static MemberAnswer findInheritedMember(const PropertyAccessContext& context, const MemberQuery& query)
{
    switch (context.program.at(query.layout).kind) {
    case LayoutKind::Instance: {
        LayoutID declaredClass = declaredLayoutOf(context.program, query.layout);
        return findOnce(context, { declaredClass, { }, false, query.key, query.access, context.flow }, [&] {
            return findPrototypeMember(context, { MemberStart::ClassPrototype, declaredClass, query.key, query.access, false });
        });
    }
    case LayoutKind::Record:
    case LayoutKind::Container:
        return findMemberFromPrototypeOf(context, BuiltinClasses::Object, query.key, query.access);
    case LayoutKind::BuiltinResult:
        return findMemberFromPrototypeOf(context, isTupleLayout(context.program, query.layout) ? BuiltinClasses::Array : BuiltinClasses::Object, query.key, query.access);
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
        return MemberAnswer::unknown(UnprovenCause::NonLayoutObject);
    case LayoutKind::ModuleNamespace:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

MemberAnswer findMemberOnPrototypes(const PropertyAccessContext& context, const MemberQuery& query)
{
    MemberAnswer inherited = findInheritedMember(context, query);
    MemberAnswer walked;
    if (!receiverStateOf(context.program, query.layout).cannot(ObjectMutation::ChangePrototype))
        requireLookupPastObject(context.program, walked, inherited, query, Proof::PrototypeLinkUnchanged, UnprovenCause::PrototypeChanged);
    return finishAtChainEnd(WTF::move(walked), WTF::move(inherited));
}

static MemberAnswer findTupleMember(const PropertyAccessContext& context, const MemberQuery& query)
{
    const Program& program = context.program;
    BuiltinName name = program.builtinNameOf(query.key);
    if (BuiltinID member = name ? program.builtins().findOwnPropertyOfInstances(BuiltinClasses::Array, name) : BuiltinID())
        return { .kind = MemberKind::Builtin, .level = KnowledgeLevel::Proven, .builtin = member };

    if (ownPropertyGainOf(program, query.layout, query.key) != OwnPropertyGain::Impossible)
        return MemberAnswer::unknown(UnprovenCause::OwnPropertyHidesMember);
    if (query.isOwnLookup || query.access == Access::Define)
        return createProvenAnswer(MemberKind::Absent);
    if (!receiverStateOf(program, query.layout).cannot(ObjectMutation::ChangePrototype))
        return MemberAnswer::unknown(UnprovenCause::PrototypeChanged);
    return findInheritedMember(context, query);
}

MemberAnswer findInstanceMember(const PropertyAccessContext& context, const MemberQuery& query)
{
    const Program& program = context.program;
    PropertyKeyID key = query.key;
    if (keyLookupOf(program, key) != KeyLookup::Ordinary)
        return MemberAnswer::unknown(UnprovenCause::Unknown);
    if (isTupleLayout(program, query.layout))
        return findTupleMember(context, query);

    BuiltinClassID builtinBase = builtinBaseClassOf(program, query.layout);
    if (builtinBase && !isPrivateName(program, key)) {
        const BuiltinTable& table = program.builtins();
        BuiltinName name = program.builtinNameOf(key);
        if (BuiltinID member = name ? table.findOwnPropertyOfInstances(builtinBase, name) : BuiltinID()) {
            if (query.access == Access::Has)
                return MemberAnswer::unknown(UnprovenCause::BuiltinBase);
            return { .kind = MemberKind::Builtin, .level = KnowledgeLevel::Proven, .builtin = member };
        }
        if (table.mayDefineOnNewInstances(builtinBase, name) || query.access == Access::Store || query.access == Access::Define)
            return MemberAnswer::unknown(UnprovenCause::BuiltinBase);
    }

    if (std::optional<FieldReference> found = findField(program, query.layout, key, BaseChain::Proven)) {
        if (builtinBase)
            return MemberAnswer::unknown(UnprovenCause::BuiltinBase);

        bool storesValue = query.access == Access::Store || query.access == Access::Define;
        if (storesValue && !isPrivateName(program, key) && isNeverStored(program, *found))
            return MemberAnswer::unknown(UnprovenCause::Unknown);
        return { .kind = MemberKind::Field, .level = KnowledgeLevel::Proven, .holder = found->layout, .field = found->field };
    }

    PropertyKeyKind keyKind = program.at(key).kind;
    if (builtinBase && keyKind == PropertyKeyKind::EnginePrivateName)
        return MemberAnswer::unknown(UnprovenCause::BuiltinBase);

    bool mayGainOwnProperty = ownPropertyGainOf(program, query.layout, key) != OwnPropertyGain::Impossible;
    if (builtinBase && mayGainOwnProperty && isCreatedWithProperties(program, query.layout, builtinBase) && !hasTestForOtherNames(program, query.layout))
        return MemberAnswer::unknown(UnprovenCause::BuiltinBase);
    MemberAnswer walked;
    if (query.isOwnLookup || query.access == Access::Define || keyKind == PropertyKeyKind::PrivateName) {
        if (mayGainOwnProperty && query.isOwnLookup)
            requireLookupPastObject(program, walked, findInheritedMember(context, query), query, Proof::NoExtraProperties, UnprovenCause::OwnPropertyHidesMember);
        else if (mayGainOwnProperty)
            addReceiverProof(walked, Proof::NoExtraProperties, UnprovenCause::OwnPropertyHidesMember);
        walked.kind = MemberKind::Absent;
        setLevel(walked);
        return walked;
    }

    MemberAnswer inherited = findMemberOnPrototypes(context, query);
    if (mayGainOwnProperty)
        requireLookupPastObject(program, walked, inherited, query, Proof::NoExtraProperties, UnprovenCause::OwnPropertyHidesMember);
    return finishAtChainEnd(WTF::move(walked), WTF::move(inherited));
}

MemberAnswer findMember(const PropertyAccessContext& context, const MemberQuery& query)
{
    RELEASE_ASSERT(query.access != Access::None);
    if (!query.layout || !query.key)
        return MemberAnswer::unknown(UnprovenCause::Unknown);

    switch (query.start) {
    case MemberStart::Instance:
        if (context.program.at(query.layout).kind == LayoutKind::ModuleNamespace)
            return findNamespaceExport(context, query);
        return findInstanceMember(context, query);
    case MemberStart::ClassPrototype:
        return findPrototypeMember(context, query);
    case MemberStart::ClassObject:
        return findStaticMember(context, query);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
