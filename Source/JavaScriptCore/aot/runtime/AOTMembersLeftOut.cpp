/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMembersLeftOut.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include "Error.h"
#include "Exception.h"
#include "JSCInlines.h"
#include "JSNativeStdFunction.h"
#include "StrongInlines.h"
#include "TopExceptionScope.h"
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

struct MemberWithoutSlot {
    // The getter, or the object on which native code answers for the name.
    Strong<JSObject> answeredBy;
    String name;
    Strong<JSObject> standIn;
};

struct StandIn {
    Strong<JSObject> function;
    // What a call of it throws.
    String message;
};

struct MembersLeftOut {
    // The newest. It is held so that no other object can take its address while it is asked for.
    Strong<JSObject> error;
    String message;
    // A getter answers the same function each time, as it would answer the same object. There are a handful.
    Vector<MemberWithoutSlot> membersWithoutSlot;
    // One for each member that was left out and read.
    Vector<StandIn> standIns;
};

Lock membersLock;

HashMap<VM*, MembersLeftOut>& membersByVM() WTF_REQUIRES_LOCK(membersLock)
{
    static NeverDestroyed<HashMap<VM*, MembersLeftOut>> map;
    return map;
}

JSC_DECLARE_HOST_FUNCTION(constructStandIn);

JSC_DEFINE_HOST_FUNCTION(constructStandIn, (JSGlobalObject* realm, CallFrame* callFrame))
{
    return uncheckedDowncast<JSNativeStdFunction>(callFrame->jsCallee())->function()(realm, callFrame);
}

} // namespace

void noteErrorOfCodeLeftOut(JSGlobalObject* realm, JSObject* error)
{
    VM& vm = realm->vm();
    JSValue message = error->getDirect(vm, vm.propertyNames->message);
    if (!message || !message.isString())
        return;
    String text = asString(message)->tryGetValue();
    Locker locker { membersLock };
    MembersLeftOut& members = membersByVM().ensure(&vm, [] {
        return MembersLeftOut { };
    }).iterator->value;
    members.error.set(vm, error);
    members.message = WTF::move(text);
}

// The message of the pending error, which is taken. Null for any other exception.
static String takeMessageOfErrorOfCodeLeftOut(VM& vm)
{
    Exception* exception = vm.exceptionForInspection();
    if (!exception)
        return { };

    String message;
    {
        Locker locker { membersLock };
        auto found = membersByVM().find(&vm);
        if (found == membersByVM().end() || !found->value.error || exception->value() != JSValue(found->value.error.get()))
            return { };
        message = std::exchange(found->value.message, String());
        found->value.error.clear();
    }
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    scope.clearException();
    return message;
}

static JSValue standInThatThrows(JSGlobalObject* realm, String&& message, const String& nameOfMember, JSObject* answeredBy)
{
    VM& vm = realm->vm();
    if (answeredBy) {
        Locker locker { membersLock };
        for (const MemberWithoutSlot& known : membersByVM().find(&vm)->value.membersWithoutSlot) {
            if (known.answeredBy.get() == answeredBy && known.name == nameOfMember)
                return known.standIn.get();
        }
    }
    NativeStdFunction throwsWhenCalled = [message](JSGlobalObject* caller, CallFrame*) -> EncodedJSValue {
        auto throwScope = DECLARE_THROW_SCOPE(caller->vm());
        return throwVMError(caller, throwScope, createError(caller, message));
    };
    JSObject* standIn = JSNativeStdFunction::create(vm, realm, 0, nameOfMember, WTF::move(throwsWhenCalled), NoIntrinsic, NativeFunction(constructStandIn));

    Locker locker { membersLock };
    MembersLeftOut& members = membersByVM().find(&vm)->value;
    members.standIns.append(StandIn { Strong<JSObject>(vm, standIn), WTF::move(message) });
    if (answeredBy)
        members.membersWithoutSlot.append(MemberWithoutSlot { Strong<JSObject>(vm, answeredBy), nameOfMember, Strong<JSObject>(vm, standIn) });
    return standIn;
}

JSValue takeStandInForMemberLeftOut(JSGlobalObject* realm, const String& nameOfMember, JSObject* answeredBy)
{
    String message = takeMessageOfErrorOfCodeLeftOut(realm->vm());
    if (message.isNull())
        return { };
    return standInThatThrows(realm, WTF::move(message), nameOfMember, answeredBy);
}

JSValue takeStandInForGetterLeftOut(JSGlobalObject* realm, JSObject* getter)
{
    VM& vm = realm->vm();
    auto* function = dynamicDowncast<JSFunction>(getter);
    if (!function || !function->isHostOrBuiltinFunction())
        return { };
    // Nothing is asked of the getter while what it has thrown is pending: a bound function makes its name when it is first asked for, and clears whatever is pending then.
    String message = takeMessageOfErrorOfCodeLeftOut(vm);
    if (message.isNull())
        return { };
    String name = function->name(vm);
    if (name.startsWith("get "_s))
        name = name.substring(4);
    return standInThatThrows(realm, WTF::move(message), name, getter);
}

void explainErrorBehindMemberLeftOut(JSGlobalObject* realm, JSObject* error, JSValue base)
{
    VM& vm = realm->vm();
    String sentence;
    {
        Locker locker { membersLock };
        auto found = membersByVM().find(&vm);
        if (found == membersByVM().end())
            return;
        // The value is compared and not looked into.
        for (const StandIn& standIn : found->value.standIns) {
            if (base == JSValue(standIn.function.get())) {
                sentence = standIn.message;
                break;
            }
        }
    }
    if (sentence.isNull())
        return;
    JSValue message = error->getDirect(vm, vm.propertyNames->message);
    if (!message || !message.isString())
        return;
    String text = asString(message)->tryGetValue();
    if (text.isNull())
        return;
    error->putDirect(vm, vm.propertyNames->message, jsString(vm, makeString(text, text.endsWith('.') ? " "_s : ". "_s, sentence)), static_cast<unsigned>(PropertyAttribute::DontEnum));
}

void forgetErrorOfCodeLeftOut(VM& vm)
{
    Locker locker { membersLock };
    membersByVM().remove(&vm);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
