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

static void dumpReceiverProofs(PrintStream& out, ProofSet proofs)
{
    CommaPrinter comma;
    out.print("[");
    for (Proof proof : allMembersOf<Proof>) {
        if (proofs.contains(proof))
            out.print(comma, nameOf(proof));
    }
    out.print("]");
}

static void dumpCondition(PrintStream& out, const MemberCondition& condition)
{
    switch (condition.kind) {
    case MemberCondition::Kind::MemberUnchanged:
        out.print("MemberUnchanged(L", condition.layout.index(), ", ", nameOf(condition.side), ", K", condition.key.index(), ")");
        return;
    case MemberCondition::Kind::PrototypeLinkUnchanged:
        out.print("PrototypeLinkUnchanged(L", condition.layout.index(), ", ", nameOf(condition.side), ")");
        return;
    case MemberCondition::Kind::ClassEvaluatedOnce:
        out.print("ClassEvaluatedOnce(L", condition.layout.index(), ")");
        return;
    case MemberCondition::Kind::InstancePrototypeUnchanged:
        out.print("InstancePrototypeUnchanged(L", condition.layout.index(), ")");
        return;
    case MemberCondition::Kind::MemberNotHidden:
        out.print("MemberNotHidden(L", condition.layout.index(), ", ", nameOf(condition.side), ", K", condition.key.index(), ")");
        return;
    case MemberCondition::Kind::MemberPassedByStores:
        out.print("MemberPassedByStores(L", condition.layout.index(), ", K", condition.key.index(), ")");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpConditions(PrintStream& out, const MemberConditions& conditions)
{
    CommaPrinter comma;
    out.print("[");
    for (const MemberCondition& condition : conditions) {
        out.print(comma);
        dumpCondition(out, condition);
    }
    out.print("]");
}

MemberAnswer MemberAnswer::unknown(UnprovenCause cause)
{
    MemberAnswer answer;
    answer.cause = cause;
    return answer;
}

FunctionID MemberAnswer::accessorFor(Access access) const
{
    switch (access) {
    case Access::Read:
        return getter;
    case Access::Store:
        return setter;
    case Access::None:
    case Access::Has:
    case Access::Define:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
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
        dumpReceiverProofs(out, receiverProofs);
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
    if (answer.kind == MemberKind::Unknown) {
        answer.level = KnowledgeLevel::Unknown;
        return;
    }
    bool needsNoTest = answer.receiverProofs.isEmpty() && answer.conditions.isEmpty();
    answer.level = needsNoTest ? KnowledgeLevel::Proven : KnowledgeLevel::Expected;
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

static MemberAnswer createProvenAnswer(MemberKind kind)
{
    MemberAnswer answer;
    answer.kind = kind;
    answer.level = KnowledgeLevel::Proven;
    return answer;
}

static void addCause(MemberAnswer& answer, UnprovenCause cause)
{
    if (answer.cause == UnprovenCause::Unknown)
        answer.cause = cause;
}

static void addReceiverProof(MemberAnswer& walked, Proof proof, UnprovenCause cause)
{
    walked.receiverProofs.add(proof);
    addCause(walked, cause);
}

static void addConditionWithCause(MemberAnswer& walked, const MemberCondition& condition, UnprovenCause cause)
{
    addCondition(walked.conditions, condition);
    addCause(walked, cause);
}

static bool isMemberOfClass(const MemberAnswer& answer)
{
    return answer.kind == MemberKind::Method || answer.kind == MemberKind::Accessor;
}

// The object itself must not stand in the way of the lookup. The run time ends the condition of a member of a class as soon as one object of the layout
// no longer finds the member where the class definitions put it. Whatever else the lookup finds is tested for on the object.
static void requireLookupPastObject(const Program& program, MemberAnswer& walked, const MemberAnswer& found, const MemberQuery& query, Proof proof, UnprovenCause cause)
{
    if (isMemberOfClass(found))
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

// The test of a member that can change compares its holder with what the class definition creates.
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
        // The prototype object of a class that extends null inherits from no object.
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

    const FlowResults* flow = context.flow;
    if (!flow || !flow->hasResults() || flow->builtinHolder(holder) != BuiltinHolderState::Intact)
        return BuiltinHolderGain::Unknown;
    if (flow->usesOf(holder).contains(BuiltinUse::Escaped) || flow->findBuiltinLocation(holder, key))
        return BuiltinHolderGain::Unknown;
    return BuiltinHolderGain::Impossible;
}

// "Infinity" and "NaN" name no element of anything but a typed array, and no object of the table is one.
static bool isLookedUpByNameInBuiltinObjects(const Program& program, PropertyKeyID key)
{
    return keyLookupOf(program, key) == KeyLookup::Ordinary || (key && program.nameClassOf(key) == NameClass::CanonicalNumericString);
}

MemberAnswer findBuiltinObjectMember(const PropertyAccessContext& context, BuiltinID holder, PropertyKeyID key, Access)
{
    const Program& program = context.program;
    if (!holder)
        return MemberAnswer::unknown(UnprovenCause::UnknownBase);
    if (!isLookedUpByNameInBuiltinObjects(program, key) || isPrivateName(program, key))
        return MemberAnswer::unknown(UnprovenCause::Unknown);

    const BuiltinTable& table = program.builtins();
    BuiltinName name = program.builtinNameOf(key);
    BuiltinID current = table.canonicalID(holder);
    while (true) {
        if (BuiltinID member = name ? table.findOwnMember(current, name) : BuiltinID()) {
            if (builtinLocationAnswerOf(context, member) != BuiltinLocationAnswer::Constant)
                return MemberAnswer::unknown(UnprovenCause::UnknownWriter);
            MemberAnswer answer = createProvenAnswer(MemberKind::Builtin);
            answer.builtin = member;
            return answer;
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

// The lookup goes on at the prototype object of the class itself, so what the classes below it define is out of its way.
static MemberAnswer findMemberFromPrototypeOf(const PropertyAccessContext& context, BuiltinClassID builtinClass, PropertyKeyID key, Access access)
{
    if (!builtinClass)
        return MemberAnswer::unknown(UnprovenCause::UnknownBase);
    // The instance answers first, and it may be a typed array.
    if (keyLookupOf(context.program, key) != KeyLookup::Ordinary)
        return MemberAnswer::unknown(UnprovenCause::Unknown);
    return findBuiltinObjectMember(context, context.program.builtins().at(builtinClass).prototype, key, access);
}

MemberAnswer findBuiltinInstanceMember(const PropertyAccessContext& context, BuiltinClassID builtinClass, PropertyKeyID key, Access access)
{
    if (!builtinClass)
        return MemberAnswer::unknown(UnprovenCause::UnknownBase);
    BuiltinMemberName name = builtinMemberNameOf(context.program, key);
    if (name && context.program.builtins().isRedefinedBelow(builtinClass, name.name))
        return MemberAnswer::unknown(UnprovenCause::UnknownBase);
    return findMemberFromPrototypeOf(context, builtinClass, key, access);
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

MemberAnswer findPrototypeMember(const PropertyAccessContext& context, const MemberQuery& query)
{
    const Program& program = context.program;
    PropertyKeyID key = query.key;
    if (keyLookupOf(program, key) != KeyLookup::Ordinary)
        return MemberAnswer::unknown(UnprovenCause::Unknown);

    bool isConstructorKey = program.nameClassOf(key) == NameClass::Constructor;
    MemberCondition memberUnchanged { MemberCondition::Kind::MemberUnchanged, query.layout, ClassSide::Prototype, key };
    MemberAnswer walked;
    // The run time looks at the chain above the prototype object of the first evaluation.
    const bool asksRunTime = query.access == Access::Store && program.at(query.layout).classFlow.evaluations == ExecutionCount::AtMostOnce;
    // A store that creates a property asks for less than a read that finds nothing: the first object above that has the name may have a writable data property.
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
        if (layout.kind != LayoutKind::Instance || !layout.constructor)
            return MemberAnswer::unknown(UnprovenCause::NonLayoutObject);
        if (isConstructorKey)
            return MemberAnswer::unknown(UnprovenCause::Unknown);

        DeclaredMember member = findDeclaredMember(layout.classFlow, ClassSide::Prototype, key);
        // A store to an object that inherits a writable data property creates a property of the object, as it does where no holder has the name. While the property is there it hides whatever is above it.
        bool isPassedByStore = query.access == Access::Store && member.isWritableData;
        if (isPassedByStore && member.mutability == MemberMutability::Fixed)
            return finish(createProvenAnswer(MemberKind::Absent));
        // One that a store puts there comes at some time: until then the store goes on to what is above.
        mayBeHidden |= isPassedByStore;
        // A method is a writable data property until somebody redefines it or freezes its holder, which the run time sees: knowing the function may not make the store worse than knowing the name alone.
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
            // `Message.prototype.url = ""`: a default. What a plain store puts on the holder is a writable data property, which a store below passes like a method, and it hides whatever is above it.
            // Nobody says when the default is stored or what else is done under the name, so the run time looks at the chain that is there.
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

static bool isTupleLayout(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    return layout.kind == LayoutKind::BuiltinResult && program.builtins().at(layout.builtinLayout).isTuple;
}

static MemberAnswer findInheritedMember(const PropertyAccessContext& context, const MemberQuery& query)
{
    switch (context.program.at(query.layout).kind) {
    case LayoutKind::Instance:
        return findPrototypeMember(context, { MemberStart::ClassPrototype, declaredLayoutOf(context.program, query.layout), query.key, query.access, false });
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

// A tuple is an array: it has an own length, which its layout does not list, and inherits from Array.prototype.
// It has no shape word, so no test on the object says that it has gained no name and kept its prototype.
static MemberAnswer findTupleMember(const PropertyAccessContext& context, const MemberQuery& query)
{
    const Program& program = context.program;
    BuiltinName name = program.builtinNameOf(query.key);
    if (BuiltinID member = name ? program.builtins().findOwnPropertyOfInstances(BuiltinClasses::Array, name) : BuiltinID()) {
        MemberAnswer answer = createProvenAnswer(MemberKind::Builtin);
        answer.builtin = member;
        return answer;
    }

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

    // What the constructor of a built-in base class defines on the object comes before every field and every prototype.
    BuiltinClassID builtinBase = builtinBaseClassOf(program, query.layout);
    if (builtinBase && !isPrivateName(program, key)) {
        const BuiltinTable& table = program.builtins();
        BuiltinName name = program.builtinNameOf(key);
        if (BuiltinID member = name ? table.findOwnPropertyOfInstances(builtinBase, name) : BuiltinID()) {
            MemberAnswer answer = createProvenAnswer(MemberKind::Builtin);
            answer.builtin = member;
            return answer;
        }
        if (table.mayDefineOnNewInstances(builtinBase, name))
            return MemberAnswer::unknown(UnprovenCause::BuiltinBase);
    }

    if (std::optional<FieldReference> found = findField(program, query.layout, key, BaseChain::Proven)) {
        // The constructor of the built-in class makes the object: a cell of that class, which has no slots. Its fields are properties like any other.
        if (builtinBase)
            return MemberAnswer::unknown(UnprovenCause::BuiltinBase);

        // No object of this layout gets to a store of the field, and placement gives the field no slot. A receiver that is only expected to have the layout is expected in vain.
        bool storesValue = query.access == Access::Store || query.access == Access::Define;
        if (storesValue && !isPrivateName(program, key) && isNeverStored(program, *found))
            return MemberAnswer::unknown(UnprovenCause::Unknown);

        MemberAnswer answer = createProvenAnswer(MemberKind::Field);
        answer.holder = found->layout;
        answer.field = found->field;
        return answer;
    }

    // The host keeps state of the objects it makes under engine private names, and the table lists none of it.
    PropertyKeyKind keyKind = program.at(key).kind;
    if (builtinBase && keyKind == PropertyKeyKind::EnginePrivateName)
        return MemberAnswer::unknown(UnprovenCause::BuiltinBase);

    // A private name of a class is never inherited. An engine private name is looked up like any other key.
    bool mayGainOwnProperty = ownPropertyGainOf(program, query.layout, key) != OwnPropertyGain::Impossible;
    MemberAnswer walked;
    if (query.isOwnLookup || query.access == Access::Define || keyKind == PropertyKeyKind::PrivateName) {
        if (mayGainOwnProperty)
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
