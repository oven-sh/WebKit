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
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeReporter.h"
#include "AOTSchemaRuntimeOperations.h"
#include "GetterSetter.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

namespace {

struct FreshAnswer {
    KindOfAnswer kind { KindOfAnswer::Absent };
    JSValue value;
};

} // anonymous namespace

static ASCIILiteral textOf(KindOfAnswer kind)
{
    switch (kind) {
    case KindOfAnswer::Value:
        return "a value"_s;
    case KindOfAnswer::Absent:
        return "nothing"_s;
    case KindOfAnswer::Accessor:
        return "an accessor"_s;
    case KindOfAnswer::Present:
        return "something"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral textOf(StubThatAnswers stub)
{
    switch (stub) {
    case StubThatAnswers::CallMethodAtSite:
        return "the place of a call by name"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool areSameAnswer(JSValue first, JSValue second)
{
    if (first.isNumber() && second.isNumber())
        return first.asNumber() == second.asNumber() || (first.asNumber() != first.asNumber() && second.asNumber() != second.asNumber());
    return JSValue::encode(first) == JSValue::encode(second);
}

static std::optional<FreshAnswer> askAgain(Instance& instance, JSValue receiver, UniquedStringImpl* name)
{
    if (!receiver || receiver.isUndefinedOrNull() || !name)
        return std::nullopt;
    VM& vm = *instance.vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertyName propertyName(name);
    PropertySlot slot(receiver, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isFound = receiver.getPropertySlot(instance.globalObject, propertyName, slot);
    if (scope.exception()) [[unlikely]] {
        if (!vm.hasPendingTerminationException())
            scope.clearException();
        return std::nullopt;
    }
    if (slot.isTaintedByOpaqueObject())
        return std::nullopt;
    if (isFound && !slot.isValue() && !slot.isAccessor())
        return std::nullopt;
    countEvent(vm, RuntimeCounterID::AnswersAskedAgain);
    if (!isFound)
        return FreshAnswer { };
    if (slot.isValue())
        return FreshAnswer { KindOfAnswer::Value, slot.getValue(instance.globalObject, propertyName) };
    return FreshAnswer { KindOfAnswer::Accessor, slot.getterSetter()->getter() ? JSValue(slot.getterSetter()->getter()) : JSValue() };
}

[[noreturn]] static void stop(ASCIILiteral whoAnswered, UniquedStringImpl* name, KindOfAnswer kind, JSValue value, const FreshAnswer& fresh)
{
    dataLogLn("AOT: ", whoAnswered, " answered ", textOf(kind), " under the name ", String(name), ", and the object, asked again, answers ", fresh.kind == kind ? "another"_s : textOf(fresh.kind), ". Answered: ", value, ". Asked again: ", fresh.value, ". This is a bug in Bun.");
    CRASH();
}

void stopAtStaleAnswer(Instance& instance, JSValue receiver, UniquedStringImpl* name, KindOfAnswer kind, JSValue value, ASCIILiteral whoAnswered)
{
    std::optional<FreshAnswer> fresh = askAgain(instance, receiver, name);
    if (!fresh)
        return;
    if (kind == KindOfAnswer::Present ? fresh->kind != KindOfAnswer::Absent : kind == fresh->kind && (kind != KindOfAnswer::Value || areSameAnswer(fresh->value, value)))
        return;
    stop(whoAnswered, name, kind, value, *fresh);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopAtStaleAnswer, void, (Instance* instance, EncodedJSValue receiver, EncodedJSValue answer, uint32_t key, uint32_t kindAndStub))
{
    LeafOperationScope leaf(*instance->vm);
    stopAtStaleAnswer(*instance, JSValue::decode(receiver), instance->state->propertyCaches->nameOf(key).uid, static_cast<KindOfAnswer>(kindAndStub & 0xff), JSValue::decode(answer), textOf(static_cast<StubThatAnswers>(kindAndStub >> 8)));
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
    constexpr ASCIILiteral whoAnswered = "a stub that takes a member of a built-in class to be as the class was made"_s;
    if (JSValue readInFront = JSValue::decode(encodedReadInFront)) {
        countEvent(vm, RuntimeCounterID::AnswersAskedAgain);
        if (holdAgainstClassAsMade(KindOfAnswer::Value, readInFront) == Verdict::IsAnother) {
            dataLogLn("AOT: a stub took the method that was read in front of a call to be the one of a built-in class as the class was made (", members.isEmpty() ? String() : table.identifierOf(vm, table.at(members[0]).name).string(), "), and what was read is another: ", readInFront, ". This is a bug in Bun.");
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
            stop(whoAnswered, identifier.impl(), fresh->kind == KindOfAnswer::Accessor ? KindOfAnswer::Accessor : KindOfAnswer::Value, JSValue(), *fresh);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
