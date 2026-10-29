/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMembersLeftOut.h"

#if ENABLE(AOT)

#include "AOTErrorMessages.h"
#include "AOTVector.h"
#include "Error.h"
#include "Exception.h"
#include "JSCInlines.h"
#include "JSNativeStdFunction.h"
#include "ObjectConstructor.h"
#include "StrongInlines.h"
#include "TopExceptionScope.h"
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

struct StandIn {
    Strong<JSObject> function;
    // What a call of it throws.
    String message;
    // Of a member without a slot: the getter, or the object on which native code answers for the name. It answers the same function each time, as it would answer the same object.
    Strong<JSObject> answeredBy;
    String name;
    bool isKnownToBeFunction { false };
};

struct MembersLeftOut {
    // The newest. It is held so that no other object can take its address while it is asked for.
    Strong<JSObject> error;
    String message;
    // One for each member that was left out and read.
    Vector<StandIn> standIns;
};

Lock membersLock;

HashMap<VM*, MembersLeftOut>& membersByVM() WTF_REQUIRES_LOCK(membersLock)
{
    static NeverDestroyed<HashMap<VM*, MembersLeftOut>> map;
    return map;
}

// Most callers find the exception with exceptionForInspection(), which does not count as a check, so a scope that verifies when it is constructed would fail here.
// They declare no scope of their own because it would make every caller above them check after each read.
class UnverifiedExceptionScope final : public ExceptionScope {
public:
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    explicit UnverifiedExceptionScope(VM& vm)
        : ExceptionScope(vm, ExceptionEventLocation(EXCEPTION_SCOPE_POSITION_FOR_ASAN(vm), __FUNCTION__, __FILE__, __LINE__))
    {
    }
#else
    explicit UnverifiedExceptionScope(VM& vm)
        : ExceptionScope(vm)
    {
    }
#endif
};

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
    UnverifiedExceptionScope scope(vm);
    bool isCleared = scope.tryClearException();
    RELEASE_ASSERT(isCleared);
    return message;
}

static JSValue standInThatThrows(JSGlobalObject* realm, String&& message, const String& nameOfMember, JSObject* answeredBy, const FunctionLeftOut* function = nullptr)
{
    VM& vm = realm->vm();
    if (answeredBy) {
        Locker locker { membersLock };
        for (const StandIn& known : membersByVM().find(&vm)->value.standIns) {
            if (known.answeredBy.get() == answeredBy && known.name == nameOfMember)
                return known.function.get();
        }
    }
    NativeStdFunction throwsWhenCalled = [message](JSGlobalObject* caller, CallFrame*) -> EncodedJSValue {
        auto throwScope = DECLARE_THROW_SCOPE(caller->vm());
        return throwVMError(caller, throwScope, createError(caller, message));
    };
    bool canBeConstructed = !function || function->canBeConstructed;
    JSObject* standIn = JSNativeStdFunction::create(vm, realm, function ? function->length : 0, nameOfMember, WTF::move(throwsWhenCalled), NoIntrinsic, canBeConstructed ? NativeFunction(constructStandIn) : NativeFunction(callHostFunctionAsConstructor));
    if (function && canBeConstructed)
        standIn->putDirect(vm, vm.propertyNames->prototype, constructEmptyObject(realm), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);

    Locker locker { membersLock };
    membersByVM().find(&vm)->value.standIns.append(StandIn { Strong<JSObject>(vm, standIn), WTF::move(message), answeredBy ? Strong<JSObject>(vm, answeredBy) : Strong<JSObject>(), nameOfMember, !!function });
    return standIn;
}

JSValue takeStandInForMemberLeftOut(JSGlobalObject* realm, const String& nameOfMember, JSObject* answeredBy)
{
    String message = takeMessageOfErrorOfCodeLeftOut(realm->vm());
    if (message.isNull())
        return { };
    return standInThatThrows(realm, WTF::move(message), nameOfMember, answeredBy);
}

JSValue takeStandInForFunctionLeftOut(JSGlobalObject* realm, const FunctionLeftOut& function, JSObject* answeredBy)
{
    String message = takeMessageOfErrorOfCodeLeftOut(realm->vm());
    if (message.isNull())
        return { };
    return standInThatThrows(realm, WTF::move(message), function.name, answeredBy, &function);
}

bool standsInForFunction(JSCell* cell)
{
    if (!cell->inherits<JSNativeStdFunction>())
        return false;
    Locker locker { membersLock };
    auto found = membersByVM().find(&cell->vm());
    if (found == membersByVM().end())
        return false;
    return std::ranges::any_of(found->value.standIns, [&](const StandIn& standIn) {
        return standIn.isKnownToBeFunction && standIn.function.get() == cell;
    });
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

JSObject* standInForCompiledGetter(JSGlobalObject* realm, JSObject* getter, const String& name)
{
    {
        Locker locker { membersLock };
        membersByVM().ensure(&realm->vm(), [] {
            return MembersLeftOut { };
        });
    }
    return asObject(standInThatThrows(realm, name.isEmpty() ? functionNotCompiledMessage() : functionNotCompiledMessage(name), name, getter));
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
