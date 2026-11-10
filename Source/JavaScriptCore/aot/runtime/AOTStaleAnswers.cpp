/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStaleAnswers.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTChainDependents.h"
#include "AOTClassPrototype.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTOperationScope.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRealmStartup.h"
#include "AOTRuntimeCheckFailure.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeReporter.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "GetterSetter.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "NullGetterFunction.h"
#include <cmath>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

struct FreshAnswer {
    KindOfAnswer kind { KindOfAnswer::Absent };
    JSValue value;
    bool isReadOnly { false };
};

} // anonymous namespace

static ASCIILiteral textOf(KindOfAnswer kind)
{
    switch (kind) {
    case KindOfAnswer::Value:
        return "a value"_s;
    case KindOfAnswer::Absent:
        return "no property"_s;
    case KindOfAnswer::Accessor:
        return "an accessor"_s;
    case KindOfAnswer::Present:
        return "a property"_s;
    case KindOfAnswer::AbsentFromObjectItself:
        return "no own property"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr ASCIILiteral textOf(StubThatAnswers stub)
{
    switch (stub) {
    case StubThatAnswers::CallMethodAtSite:
        return "a method call cache"_s;
    case StubThatAnswers::FindsNoExtraPropertyAtSite:
        return "an absent property cache"_s;
    case StubThatAnswers::ReadByName:
    case StubThatAnswers::ReadByNameUnlessAsMade:
        return "an untyped property read"_s;
    case StubThatAnswers::TruthByName:
        return "an untyped property read that is used as a condition"_s;
    case StubThatAnswers::PresenceByName:
        return "an untyped `in` check"_s;
    case StubThatAnswers::PresenceOfOwnName:
        return "an untyped own property check"_s;
    case StubThatAnswers::GetterOfPendingRead:
        return "an untyped property read that is about to call a getter"_s;
    case StubThatAnswers::CachedGetterByName:
        return "a cached getter lookup"_s;
    case StubThatAnswers::ProofThatLookupFindsNothing:
        return "a proof that a property is absent"_s;
    case StubThatAnswers::TypedReadByName:
        return "a typed property read"_s;
    case StubThatAnswers::TypedPresenceOfOwnName:
        return "a typed own property check"_s;
    case StubThatAnswers::StateOfWalk:
        return "a property read that the compiler replaced with a known value"_s;
    case StubThatAnswers::FunctionThatNameFinds:
        return "a devirtualized method call"_s;
    case StubThatAnswers::StoreIntoSlot:
        return "a property store to a fixed slot"_s;
    case StubThatAnswers::DoorOfPlainInstance:
        return "the plain instance check"_s;
    case StubThatAnswers::HolderWithoutAddedNames:
        return "the check that a built-in object has no added properties"_s;
    case StubThatAnswers::ChainAbovePrototype:
        return "the check that the prototype chain of a class is unchanged"_s;
    case StubThatAnswers::MemberAsBuilt:
        return "the check that a built-in member is unmodified"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool areSameAnswer(JSValue first, JSValue second)
{
    if (first.isNumber() && second.isNumber())
        return (first.asNumber() == second.asNumber() && std::signbit(first.asNumber()) == std::signbit(second.asNumber())) || (first.asNumber() != first.asNumber() && second.asNumber() != second.asNumber());
    if (first.isString() && second.isString() && !asString(first)->isRope() && !asString(second)->isRope())
        return equal(asString(first)->tryGetValue().data.impl(), asString(second)->tryGetValue().data.impl());
    return JSValue::encode(first) == JSValue::encode(second);
}

static bool inquiryWouldMakePartsOf(JSObject* object)
{
    if (object->classInfo() == ClassPrototype::info())
        return !object->butterfly();
    if (auto* closure = dynamicDowncast<Closure>(object))
        return closure->isLazyClassObject();
    Object* compiled = compiledObjectOrNull(object);
    return compiled && hasPendingStars(*compiled);
}

static bool answersLookupsByRuleOfHost(JSObject* object)
{
    return object->type() == ObjectType && object->structure()->typeInfo().overridesGetOwnPropertySlot() && !compiledObjectOrNull(object) && object->classInfo() != ClassPrototype::info() && !dynamicDowncast<Closure>(object);
}

static std::optional<bool> inquireWithoutMakingAnything(JSGlobalObject* globalObject, JSObject* first, PropertyName name, PropertySlot& slot, bool asksObjectItself)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (parseIndex(name))
        return std::nullopt;
    for (JSObject* object = first; object; object = object->getPrototypeDirect().getObject()) {
        if (inquiryWouldMakePartsOf(object))
            return std::nullopt;
        bool isFound = object->methodTable()->getOwnPropertySlot(object, globalObject, name, slot);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (isFound)
            return true;
        if (answersLookupsByRuleOfHost(object))
            return std::nullopt;
        if (asksObjectItself || slot.isTaintedByOpaqueObject())
            return false;
        if (object->structure()->typeInfo().overridesGetPrototype())
            return std::nullopt;
    }
    return false;
}

static std::optional<FreshAnswer> askAgain(Instance& instance, JSValue receiver, UniquedStringImpl* name, bool asksObjectItself = false)
{
    if (!receiver || receiver.isUndefinedOrNull() || !name || (asksObjectItself && !receiver.isObject()))
        return std::nullopt;
    if (receiver.isCell() && receiver.asCell()->type() == SlimClosureType)
        return std::nullopt;
    VM& vm = *instance.vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertyName propertyName(name);
    PropertySlot slot(receiver, PropertySlot::InternalMethodType::VMInquiry, &vm);
    JSObject* lookupStart = nullptr;
    if (!asksObjectItself && receiver.isObject()) {
        LazyPrototypeRead lazy = classifyReadThroughLazyPrototypes(instance.globalObject, asObject(receiver), name);
        if (lazy.result == LazyPrototypeReadResult::Answered) {
            countEvent(vm, RuntimeCounterID::FastPathResultsVerified);
            if (lazy.answer.kind == PropertyAnswerKind::Absent)
                return FreshAnswer { };
            JSValue held = valueAtOffset(lazy.answer.holder, lazy.answer.offset);
            if (lazy.answer.kind == PropertyAnswerKind::Value)
                return FreshAnswer { KindOfAnswer::Value, held };
            auto* accessor = uncheckedDowncast<GetterSetter>(held.asCell());
            return FreshAnswer { KindOfAnswer::Accessor, accessor->isGetterNull() ? JSValue() : JSValue(accessor->getter()) };
        }
        lookupStart = lazy.firstObjectAbove;
    }
    std::optional<bool> found = lookupStart || receiver.isObject() ? inquireWithoutMakingAnything(instance.globalObject, lookupStart ? lookupStart : asObject(receiver), propertyName, slot, asksObjectItself)
        : std::optional<bool>(receiver.getPropertySlot(instance.globalObject, propertyName, slot));
    if (scope.exception()) [[unlikely]] {
        if (!vm.hasPendingTerminationException())
            scope.clearException();
        return std::nullopt;
    }
    if (!found)
        return std::nullopt;
    bool isFound = *found;
    if (slot.isTaintedByOpaqueObject())
        return std::nullopt;
    if (isFound && !slot.isValue() && !slot.isAccessor())
        return std::nullopt;
    countEvent(vm, RuntimeCounterID::FastPathResultsVerified);
    if (!isFound)
        return FreshAnswer { };
    if (slot.isValue())
        return FreshAnswer { KindOfAnswer::Value, slot.getValue(instance.globalObject, propertyName), !!(slot.attributes() & PropertyAttribute::ReadOnly) };
    return FreshAnswer { KindOfAnswer::Accessor, slot.getterSetter()->isGetterNull() ? JSValue() : JSValue(slot.getterSetter()->getter()) };
}

static String describeKey(Instance& instance, UniquedStringImpl* key)
{
    if (identifiersOf(instance).numberOf(key) || BuiltinTable::singleton().nameOf(*instance.vm, key))
        return makeString('\'', StringView(key), '\'');
    return "a computed key"_s;
}

template<typename... Parts>
static void reportFailedLookupCheck(const Parts&... detail)
{
    RuntimeCheckFailure failure("property lookup check failed"_s);
    failure.detail(detail...);
    failure.report();
}

[[noreturn]] static void stop(Instance& instance, ASCIILiteral whoAnswered, UniquedStringImpl* name, KindOfAnswer kind, const FreshAnswer& fresh)
{
    reportFailedLookupCheck(whoAnswered, " found ", textOf(kind), " for ", describeKey(instance, name), ", but a full lookup finds ", fresh.kind == kind ? "another one"_s : textOf(fresh.kind));
    CRASH();
}

void stopAtStaleAnswer(Instance& instance, JSValue receiver, UniquedStringImpl* name, KindOfAnswer kind, JSValue value, ASCIILiteral whoAnswered)
{
    std::optional<FreshAnswer> fresh = askAgain(instance, receiver, name, kind == KindOfAnswer::AbsentFromObjectItself);
    if (!fresh)
        return;
    if (kind == KindOfAnswer::Present ? fresh->kind != KindOfAnswer::Absent : kind == KindOfAnswer::AbsentFromObjectItself ? fresh->kind == KindOfAnswer::Absent : kind == fresh->kind && (kind != KindOfAnswer::Value || areSameAnswer(fresh->value, value)))
        return;
    stop(instance, whoAnswered, name, kind, *fresh);
}

void stopAtStaleGetterOfPendingRead(Instance& instance, JSValue receiver, JSValue accessorOrGetter)
{
    State& state = *instance.state;
    bool isPending = state.receiverOfPendingRead == JSValue::encode(receiver) && state.slowPathReadCountAtPendingRead == instance.propertyCaches.slowPathReadCount;
    state.receiverOfPendingRead = JSValue::encode(JSValue());
    if (!receiver || !isPending)
        return;
    constexpr ASCIILiteral whoAnswered = textOf(StubThatAnswers::GetterOfPendingRead);
    UniquedStringImpl* name = state.propertyCaches->nameOf(state.keyOfPendingRead).uid;
    if (!accessorOrGetter || !accessorOrGetter.isCell() || (!accessorOrGetter.isObject() && accessorOrGetter.asCell()->type() != GetterSetterType))
        stop(instance, whoAnswered, name, KindOfAnswer::Accessor, FreshAnswer { });
    JSValue getter = accessorOrGetter;
    if (auto* accessor = dynamicDowncast<GetterSetter>(accessorOrGetter.asCell()))
        getter = accessor->isGetterNull() ? JSValue() : JSValue(accessor->getter());
    std::optional<FreshAnswer> fresh = askAgain(instance, receiver, name);
    if (fresh && (fresh->kind != KindOfAnswer::Accessor || fresh->value != getter))
        stop(instance, whoAnswered, name, KindOfAnswer::Accessor, *fresh);
}

static void stopAtNameAddedToHolder(Instance& instance, BuiltinID holder)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    holder = table.canonicalID(holder);
    if (holder != table.canonicalID(Builtins::ObjectPrototype))
        return;
    JSCell* cell = instance.state->builtinCells->findObject(holder);
    JSObject* object = cell ? cell->getObject() : nullptr;
    if (!object)
        return;
    VM& vm = *instance.vm;
    countEvent(vm, RuntimeCounterID::FastPathResultsVerified);
    for (const PropertyTableEntry& entry : object->structure()->getPropertiesConcurrently()) {
        UniquedStringImpl* key = entry.key();
        if (key->isSymbol() && static_cast<SymbolImpl*>(key)->isPrivate())
            continue;
        BuiltinName name = table.nameOf(vm, key);
        if (name && table.findOwnMember(holder, name))
            continue;
        reportFailedLookupCheck(textOf(StubThatAnswers::HolderWithoutAddedNames), " passed, but the object has an added property, ", describeKey(instance, entry.key()));
        CRASH();
    }
}

static bool isYetToBeLaidOutOfTableOfClass(JSObject* holder, UniquedStringImpl* key)
{
    if (holder->staticPropertiesReified())
        return false;
    for (const ClassInfo* info = holder->classInfo(); info; info = info->parentClass) {
        if (info->staticPropHashTable && info->staticPropHashTable->entry(PropertyName(key)))
            return true;
    }
    return false;
}

static bool isAccessorLaidByHostOrEngine(JSValue found)
{
    if (!found.isCell())
        return false;
    if (found.asCell()->type() == CustomGetterSetterType)
        return true;
    auto* accessor = dynamicDowncast<GetterSetter>(found.asCell());
    if (!accessor || accessor->isGetterNull())
        return false;
    JSObject* getter = accessor->getter();
    if (auto* closure = dynamicDowncast<Closure>(getter))
        return closure->isBuiltinFunction();
    auto* function = dynamicDowncast<JSFunction>(getter);
    return function && function->isHostOrBuiltinFunction() && !function->inherits<JSBoundFunction>();
}

static void stopAtMemberThatIsNotAsBuilt(Instance& instance, BuiltinID member)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    VM& vm = *instance.vm;
    BuiltinID holderOfMember = table.canonicalID(table.at(member).holder);
    JSCell* holder = holderOfMember == Builtins::GlobalThis ? instance.globalObject : instance.state->builtinCells->findObject(holderOfMember);
    JSObject* object = holder ? holder->getObject() : nullptr;
    if (!object)
        return;
    countEvent(vm, RuntimeCounterID::FastPathResultsVerified);
    Identifier name = table.identifierOf(vm, table.at(member).name);
    JSValue found = object->getDirect(vm, name);
    if (!found && reportsCreationOfMember(object, name.impl()))
        return;
    BuiltinID canonical = table.canonicalID(member);
    bool isAccessor = table.at(canonical).kind == BuiltinKind::Accessor;
    if (isAccessor && !BuiltinCellKey::getter(canonical).hasCell(table)) {
        if (found ? isAccessorLaidByHostOrEngine(found) : isYetToBeLaidOutOfTableOfClass(object, name.impl()))
            return;
        reportFailedLookupCheck(textOf(StubThatAnswers::MemberAsBuilt), " passed, but '", String(name.impl()), "' ", found ? "is not the built-in accessor"_s : "is missing"_s);
        CRASH();
    }
    JSCell* built = isAccessor ? instance.state->builtinCells->findGetter(canonical) : instance.state->builtinCells->findObject(canonical);
    if (!built)
        return;
    if (auto* accessor = isAccessor && found && found.isCell() ? dynamicDowncast<GetterSetter>(found.asCell()) : nullptr)
        found = accessor->isGetterNull() ? JSValue() : JSValue(accessor->getter());
    if (found == JSValue(built))
        return;
    reportFailedLookupCheck(textOf(StubThatAnswers::MemberAsBuilt), " passed, but '", String(name.impl()), "' ", found ? "has another value"_s : "is missing"_s);
    CRASH();
}

static void stopAtFalseYesOfDoor(Instance& instance, JSObject* asked, uint32_t indexOfClassAsked)
{
    Object* compiled = compiledObjectOrNull(asked);
    if (!compiled)
        return;
    ObjectModel& objects = *instance.state->objects;
    const LayoutData* classAsked = indexOfClassAsked == UINT32_MAX ? nullptr : objects.classes.classWithIndex(indexOfClassAsked);
    auto isFieldOfClassAsked = [&](const FieldData& field) {
        return std::ranges::any_of(classAsked->fields(), [&](const FieldData& other) {
            return other.name == field.name;
        });
    };
    for (JSObject* between = asked->getPrototypeDirect().getObject(); between; between = between->getPrototypeDirect().getObject()) {
        ClassSide side = ClassSide::Prototype;
        auto* lazyPrototype = dynamicDowncast<ClassPrototype>(between);
        const LayoutData* held = lazyPrototype ? objects.classes.classOf(*lazyPrototype) : objects.classes.classHeldBy(between, side);
        bool isHeldByClass = lazyPrototype || held;
        if (isHeldByClass && (!classAsked || !held || side != ClassSide::Prototype || held->index == classAsked->index))
            return;
        for (const FieldData& field : compiled->layout().fields()) {
            if (field.isPrivate() || field.isBrand() || !field.name || field.isAccessor() || field.takesAccessorsOfLibraryAlone())
                continue;
            if (isHeldByClass && !isFieldOfClassAsked(field))
                continue;
            UniquedStringImpl* name = objects.layouts.nameOf(*instance.vm, field);
            std::optional<FreshAnswer> fresh = askAgain(instance, between, name, true);
            if (!fresh || fresh->kind == KindOfAnswer::Absent)
                continue;
            if (isHeldByClass) {
                if (fresh->kind != KindOfAnswer::Accessor)
                    continue;
                reportFailedLookupCheck(textOf(StubThatAnswers::DoorOfPlainInstance), " passed, but the prototype of a subclass has ", textOf(fresh->kind), " for the field '", String(name), "'");
                CRASH();
            }
            reportFailedLookupCheck(textOf(StubThatAnswers::DoorOfPlainInstance), " passed, but an object between the instance and the prototype of its class has ", textOf(fresh->kind), " for the field '", String(name), "'");
            CRASH();
        }
    }
}

static void stopAtChangedChainAbovePrototype(Instance& instance, uint32_t numberOfConstructor)
{
    VM& vm = *instance.vm;
    ClassTable& classes = instance.state->objects->classes;
    const LayoutData* current = classes.classConstructedByClosureWithNumber(numberOfConstructor);
    auto* classObject = current ? dynamicDowncast<Closure>(instance.state->objects->closures.closureIfCreated(FunctionID(current->classData().classConstructor))) : nullptr;
    if (!classObject)
        return;
    JSValue stored = classObject->getDirect(vm, vm.propertyNames->prototype);
    JSObject* object = stored ? stored.getObject() : classes.classPrototypeIfExists(*current);
    if (!stored && !object)
        return;
    countEvent(vm, RuntimeCounterID::FastPathResultsVerified);
    auto stopAt = [&](ASCIILiteral finding) {
        reportFailedLookupCheck(textOf(StubThatAnswers::ChainAbovePrototype), " passed, but ", finding);
        CRASH();
    };
    for (bool isClassAsked = true; current; current = classes.assumedBaseOf(*current), isClassAsked = false) {
        if (!object || classes.classPrototypeIfExists(*current) != object)
            stopAt(isClassAsked ? "the prototype of the class was replaced"_s : "a link above the prototype of the class has changed"_s);
        object = object->getPrototypeDirect().getObject();
    }
    if (object != instance.globalObject->objectPrototype())
        stopAt("a link above the prototype of the class has changed"_s);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopAtStaleAnswer, void, (Instance* instance, EncodedJSValue receiver, EncodedJSValue answer, uint32_t key, uint32_t kindAndStub))
{
    LeafOperationScope leaf(*instance->vm);
    StubThatAnswers stub = static_cast<StubThatAnswers>(kindAndStub >> 8 & 0xff);
    JSValue answered = JSValue::decode(answer);
    if (kindAndStub & answerIsYesOfQuestionOfLibrary)
        countEvent(*instance->vm, RuntimeCounterID::LibraryFastPathChecksPassed);
    if (stub == StubThatAnswers::GetterOfPendingRead) {
        stopAtStaleGetterOfPendingRead(*instance, JSValue::decode(receiver), answered);
        return;
    }
    if (stub == StubThatAnswers::HolderWithoutAddedNames) {
        stopAtNameAddedToHolder(*instance, BuiltinID(static_cast<uint32_t>(JSValue::decode(receiver).asInt32())));
        return;
    }
    if (stub == StubThatAnswers::MemberAsBuilt) {
        stopAtMemberThatIsNotAsBuilt(*instance, BuiltinID(static_cast<uint32_t>(JSValue::decode(receiver).asInt32())));
        return;
    }
    if (stub == StubThatAnswers::DoorOfPlainInstance) {
        stopAtFalseYesOfDoor(*instance, asObject(JSValue::decode(receiver)), key);
        return;
    }
    if (stub == StubThatAnswers::ChainAbovePrototype) {
        stopAtChangedChainAbovePrototype(*instance, static_cast<uint32_t>(answered.asInt32()));
        return;
    }
    instance->state->receiverOfPendingRead = JSValue::encode(JSValue());
    UniquedStringImpl* name = instance->state->propertyCaches->nameOf(key).uid;
    if (stub == StubThatAnswers::ProofThatLookupFindsNothing && !JSValue::decode(receiver).isObject())
        return;
    if (stub == StubThatAnswers::CallMethodAtSite || stub == StubThatAnswers::FindsNoExtraPropertyAtSite || stub == StubThatAnswers::ProofThatLookupFindsNothing) {
        stopAtStaleAnswer(*instance, JSValue::decode(receiver), name, static_cast<KindOfAnswer>(kindAndStub & 0xff), answered, textOf(stub));
        return;
    }
    if (!answered && stub == StubThatAnswers::ReadByNameUnlessAsMade)
        return;
    if (stub == StubThatAnswers::StoreIntoSlot) {
        std::optional<FreshAnswer> fresh = askAgain(*instance, JSValue::decode(receiver), name);
        if (fresh && (fresh->kind == KindOfAnswer::Accessor || fresh->isReadOnly)) {
            reportFailedLookupCheck(textOf(stub), " assumes that '", String(name), "' is a writable data property, but a full lookup finds ", fresh->isReadOnly ? "a read-only property"_s : textOf(fresh->kind));
            CRASH();
        }
        return;
    }
    if (stub == StubThatAnswers::FunctionThatNameFinds) {
        std::optional<FreshAnswer> fresh = askAgain(*instance, JSValue::decode(receiver), name);
        if (!fresh)
            return;
        KindOfAnswer wanted = static_cast<KindOfAnswer>(kindAndStub & 0xff);
        std::optional<uint32_t> found = fresh->kind == wanted && fresh->value && fresh->value.isCell() ? closureNumberOf(fresh->value.asCell()) : std::nullopt;
        if (!found || *found != static_cast<uint32_t>(answered.asInt32()))
            stop(*instance, textOf(stub), name, wanted, *fresh);
        return;
    }
    if (stub == StubThatAnswers::CachedGetterByName) {
        if (!answered || !answered.isObject())
            return;
        JSValue getter = dynamicDowncast<NullGetterFunction>(answered.asCell()) ? JSValue() : answered;
        std::optional<FreshAnswer> fresh = askAgain(*instance, JSValue::decode(receiver), name);
        if (fresh && (fresh->kind != KindOfAnswer::Accessor || fresh->value != getter))
            stop(*instance, textOf(stub), name, KindOfAnswer::Accessor, *fresh);
        return;
    }
    if ((kindAndStub & emptySlotAnswersNothingByItself) && (!answered || answered.isUndefined()))
        return;
    if (stub == StubThatAnswers::StateOfWalk && (!answered || (answered.isCell() && !answered.isObject() && !answered.isString() && !answered.isSymbol() && !answered.isHeapBigInt()) || (answered.isString() && asString(answered)->isRope())))
        return;
    if (!answered || (answered.isCell() && !answered.isObject() && !answered.isString() && !answered.isSymbol() && !answered.isHeapBigInt()))
        stop(*instance, textOf(stub), name, KindOfAnswer::Value, FreshAnswer { });
    if (kindAndStub & answerMayComeFromSlowPath)
        return;
    bool asksForOwnName = stub == StubThatAnswers::PresenceOfOwnName || stub == StubThatAnswers::TypedPresenceOfOwnName;
    std::optional<FreshAnswer> fresh = askAgain(*instance, JSValue::decode(receiver), name, asksForOwnName);
    if (!fresh)
        return;
    if (stub == StubThatAnswers::PresenceByName || asksForOwnName) {
        if (answered.isTrue() != (fresh->kind != KindOfAnswer::Absent))
            stop(*instance, textOf(stub), name, answered.isTrue() ? KindOfAnswer::Present : KindOfAnswer::Absent, *fresh);
        return;
    }
    if (stub == StubThatAnswers::StateOfWalk && fresh->kind == KindOfAnswer::Accessor && !(kindAndStub & accessorIsNoAnswerOfWalk))
        return;
    if (fresh->kind == KindOfAnswer::Accessor && fresh->value) {
        auto* function = dynamicDowncast<JSFunction>(fresh->value);
        if (!function || !function->isHostOrBuiltinFunction())
            stop(*instance, textOf(stub), name, KindOfAnswer::Value, *fresh);
        return;
    }
    JSValue found = fresh->kind == KindOfAnswer::Value ? fresh->value : jsUndefined();
    if (stub == StubThatAnswers::TruthByName ? answered.isTrue() != found.toBoolean(instance->globalObject) : !areSameAnswer(found, answered))
        stop(*instance, textOf(stub), name, KindOfAnswer::Value, *fresh);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopAtStaleMemberOfBuiltinClass, void, (Instance* instance, EncodedJSValue encodedReceiver, EncodedJSValue encodedReadInFront, uint32_t form))
{
    VM& vm = *instance->vm;
    LeafOperationScope leaf(vm);
    const BuiltinTable& table = BuiltinTable::singleton();
    Vector<BuiltinID, 4> members = table.membersThatHoldMethodOf(static_cast<BuiltinBoxedStub>(form));
    enum class Verdict : uint8_t { IsAsMade, IsAnother, CannotSay };
    auto holdAgainstClassAsMade = [&](KindOfAnswer kind, JSValue found) {
        bool someCellIsKnown = false;
        for (BuiltinID member : members) {
            BuiltinCells& cells = *instance->state->builtinCells;
            JSCell* asMade = kind == KindOfAnswer::Accessor ? cells.findGetter(table.canonicalID(member)) : cells.findObject(table.canonicalID(member));
            someCellIsKnown |= !!asMade;
            if (asMade && found && found.isCell() && found.asCell() == asMade)
                return Verdict::IsAsMade;
        }
        return someCellIsKnown ? Verdict::IsAnother : Verdict::CannotSay;
    };
    constexpr ASCIILiteral whoAnswered = "a stub that assumes an unmodified built-in method"_s;
    if (JSValue readInFront = JSValue::decode(encodedReadInFront)) {
        countEvent(vm, RuntimeCounterID::FastPathResultsVerified);
        if (holdAgainstClassAsMade(KindOfAnswer::Value, readInFront) == Verdict::IsAnother) {
            reportFailedLookupCheck(whoAnswered, " was called for '", members.isEmpty() ? String() : table.identifierOf(vm, table.at(members[0]).name).string(), "', but the callee that was read before the call is another function");
            CRASH();
        }
        return;
    }
    Vector<BuiltinName, 4> namesAsked;
    for (BuiltinID member : members) {
        BuiltinName name = table.at(member).name;
        if (namesAsked.contains(name))
            continue;
        namesAsked.append(name);
        Identifier identifier = table.identifierOf(vm, name);
        std::optional<FreshAnswer> fresh = askAgain(*instance, JSValue::decode(encodedReceiver), identifier.impl());
        if (fresh && (fresh->kind == KindOfAnswer::Absent || holdAgainstClassAsMade(fresh->kind, fresh->value) == Verdict::IsAnother))
            stop(*instance, whoAnswered, identifier.impl(), fresh->kind == KindOfAnswer::Accessor ? KindOfAnswer::Accessor : KindOfAnswer::Value, *fresh);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
