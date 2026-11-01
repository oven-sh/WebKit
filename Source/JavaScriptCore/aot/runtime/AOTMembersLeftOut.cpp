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
#include "FunctionPrototype.h"
#include "InternalFunction.h"
#include "JSCInlines.h"
#include "JSNativeStdFunction.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"
#include "WeakInlines.h"
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

// The table lasts as long as the VM, and a realm may end long before. So it holds nothing: whoever has read a stand-in holds it, and one that nobody holds is made anew.
struct StandIn {
    Weak<JSObject> function;
    // What a call of it throws.
    String message;
    // Of a member without a slot: the getter, or the object on which native code answers for the name. It answers the same function each time, as it would answer the same object.
    Weak<JSObject> answeredBy;
    String name;
    bool isKnownToBeFunction { false };
};

struct MembersLeftOut {
    // The newest. Once it is dead the handle answers null, so no other object that takes its address is taken for it.
    Weak<JSObject> error;
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

bool throwMessageOfStandIn(JSGlobalObject* realm, JSCell* standIn)
{
    String message;
    {
        Locker locker { membersLock };
        for (const StandIn& known : membersByVM().find(&realm->vm())->value.standIns) {
            if (known.function.get() == standIn)
                message = known.message;
        }
    }
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwException(realm, scope, createError(realm, message));
    return false;
}

JSC_DECLARE_HOST_FUNCTION(useStandInForValue);

JSC_DEFINE_HOST_FUNCTION(useStandInForValue, (JSGlobalObject* realm, CallFrame* callFrame))
{
    throwMessageOfStandIn(realm, callFrame->jsCallee());
    return encodedJSValue();
}

// What was left out may be an object that the program has under another name. A write to what stands in for it would never arrive there, so it is refused as a call is.
class StandInForValue final : public InternalFunction {
public:
    using Base = InternalFunction;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesPut | ProhibitsPropertyCaching;

    static StandInForValue* create(VM& vm, JSGlobalObject* realm, const String& name)
    {
        Structure* structure = Structure::create(vm, realm, realm->functionPrototype(), TypeInfo(InternalFunctionType, StructureFlags), info());
        auto* standIn = new (NotNull, allocateCell<StandInForValue>(vm)) StandInForValue(vm, structure);
        standIn->finishCreation(vm, 0, name);
        return standIn;
    }

    DECLARE_INFO;

    static bool put(JSCell* cell, JSGlobalObject* realm, PropertyName name, JSValue value, PutPropertySlot& slot)
    {
        // An object below it takes the store itself.
        if (slot.thisValue() != JSValue(cell))
            return Base::put(cell, realm, name, value, slot);
        return throwMessageOfStandIn(realm, cell);
    }
    static bool putByIndex(JSCell* cell, JSGlobalObject* realm, unsigned, JSValue, bool) { return throwMessageOfStandIn(realm, cell); }
    static bool deleteProperty(JSCell* cell, JSGlobalObject* realm, PropertyName, DeletePropertySlot&) { return throwMessageOfStandIn(realm, cell); }
    static bool deletePropertyByIndex(JSCell* cell, JSGlobalObject* realm, unsigned) { return throwMessageOfStandIn(realm, cell); }
    static bool defineOwnProperty(JSObject* object, JSGlobalObject* realm, PropertyName, const PropertyDescriptor&, bool) { return throwMessageOfStandIn(realm, object); }
    static bool setPrototype(JSObject* object, JSGlobalObject* realm, JSValue, bool) { return throwMessageOfStandIn(realm, object); }
    static bool preventExtensions(JSObject* object, JSGlobalObject* realm) { return throwMessageOfStandIn(realm, object); }

private:
    StandInForValue(VM& vm, Structure* structure)
        : Base(vm, structure, useStandInForValue, useStandInForValue)
    {
    }
};
STATIC_ASSERT_ISO_SUBSPACE_SHARABLE(StandInForValue, InternalFunction);

const ClassInfo StandInForValue::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(StandInForValue) };

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
    members.error = Weak<JSObject>(error);
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
            if (known.function && known.answeredBy.get() == answeredBy && known.name == nameOfMember)
                return known.function.get();
        }
    }
    NativeStdFunction throwsWhenCalled = [message](JSGlobalObject* caller, CallFrame*) -> EncodedJSValue {
        auto throwScope = DECLARE_THROW_SCOPE(caller->vm());
        return throwVMError(caller, throwScope, createError(caller, message));
    };
    bool canBeConstructed = function && function->canBeConstructed;
    JSObject* standIn = function
        ? static_cast<JSObject*>(JSNativeStdFunction::create(vm, realm, function->length, nameOfMember, WTF::move(throwsWhenCalled), NoIntrinsic, canBeConstructed ? NativeFunction(constructStandIn) : NativeFunction(callHostFunctionAsConstructor)))
        : StandInForValue::create(vm, realm, nameOfMember);
    if (canBeConstructed)
        standIn->putDirect(vm, vm.propertyNames->prototype, constructEmptyObject(realm), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);

    Locker locker { membersLock };
    Vector<StandIn>& standIns = membersByVM().find(&vm)->value.standIns;
    standIns.removeAllMatching([](const StandIn& known) {
        return !known.function;
    });
    standIns.append(StandIn { Weak<JSObject>(standIn), WTF::move(message), answeredBy ? Weak<JSObject>(answeredBy) : Weak<JSObject>(), nameOfMember, !!function });
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
