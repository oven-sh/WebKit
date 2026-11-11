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
#include "ObjectConstructor.h"
#include "WeakInlines.h"
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

struct StandIn {
    Weak<JSObject> function;
    String message;
    Weak<JSObject> owner;
    String name;
    bool isKnownToBeFunction { false };
};

struct MembersLeftOut {
    Weak<JSObject> error;
    String message;
    bool isThrownToRead { false };
    Vector<StandIn> standIns;
};

Lock membersLock;

HashMap<VM*, MembersLeftOut>& membersByVM() WTF_REQUIRES_LOCK(membersLock)
{
    static NeverDestroyed<HashMap<VM*, MembersLeftOut>> map;
    return map;
}

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

class StandInForValue final : public InternalFunction {
public:
    using Base = InternalFunction;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | OverridesGetOwnPropertyNames | OverridesPut | ProhibitsPropertyCaching;

    static StandInForValue* create(VM& vm, JSGlobalObject* realm, const String& name, unsigned length, bool canBeConstructed)
    {
        Structure* structure = Structure::create(vm, realm, realm->functionPrototype(), TypeInfo(InternalFunctionType, StructureFlags), info());
        auto* standIn = new (NotNull, allocateCell<StandInForValue>(vm)) StandInForValue(vm, structure, canBeConstructed);
        standIn->finishCreation(vm, length, name);
        return standIn;
    }

    DECLARE_INFO;

    static bool getOwnPropertySlot(JSObject* object, JSGlobalObject* realm, PropertyName name, PropertySlot& slot)
    {
        const CommonIdentifiers& names = *realm->vm().propertyNames;
        bool isKnown = (name == names.name || name == names.length || name == names.prototype) && isOmittedFunctionPlaceholder(object);
        if (isKnown || slot.internalMethodType() == PropertySlot::InternalMethodType::VMInquiry)
            return Base::getOwnPropertySlot(object, realm, name, slot);
        return throwMessageOfStandIn(realm, object);
    }
    static bool getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* realm, unsigned index, PropertySlot& slot)
    {
        if (slot.internalMethodType() == PropertySlot::InternalMethodType::VMInquiry)
            return Base::getOwnPropertySlotByIndex(object, realm, index, slot);
        return throwMessageOfStandIn(realm, object);
    }
    static void getOwnPropertyNames(JSObject* object, JSGlobalObject* realm, PropertyNameArrayBuilder&, DontEnumPropertiesMode) { throwMessageOfStandIn(realm, object); }

    static bool put(JSCell* cell, JSGlobalObject* realm, PropertyName name, JSValue value, PutPropertySlot& slot)
    {
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
    StandInForValue(VM& vm, Structure* structure, bool canBeConstructed)
        : Base(vm, structure, useStandInForValue, canBeConstructed ? NativeFunction(useStandInForValue) : NativeFunction(callHostFunctionAsConstructor))
    {
    }
};
STATIC_ASSERT_ISO_SUBSPACE_SHARABLE(StandInForValue, InternalFunction);

const ClassInfo StandInForValue::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(StandInForValue) };

} // namespace

static thread_local bool reifiesMemberForPresenceCheck;
static thread_local bool reifiesMemberForRead;

static bool isMakingMemberForRead()
{
    return reifiesMemberForRead;
}

void noteOmittedCodeError(JSGlobalObject* realm, JSObject* error)
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
    members.isThrownToRead = isMakingMemberForRead();
}

static String takeOmittedCodeErrorMessage(VM& vm)
{
    Exception* exception = vm.exceptionForInspection();
    if (!exception)
        return { };

    String message;
    {
        Locker locker { membersLock };
        auto found = membersByVM().find(&vm);
        if (found == membersByVM().end())
            return { };
        MembersLeftOut& members = found->value;
        if (!members.error || exception->value() != JSValue(members.error.get()))
            return { };
        if (members.isThrownToRead)
            return { };
        message = std::exchange(members.message, String());
        members.error.clear();
    }
    UnverifiedExceptionScope scope(vm);
    bool isCleared = scope.tryClearException();
    RELEASE_ASSERT(isCleared);
    return message;
}

static JSValue createThrowingPlaceholder(JSGlobalObject* realm, String&& message, const String& nameOfMember, JSObject* owner, const OmittedFunction* function = nullptr)
{
    VM& vm = realm->vm();
    if (owner) {
        Locker locker { membersLock };
        for (const StandIn& known : membersByVM().find(&vm)->value.standIns) {
            if (known.function && known.owner.get() == owner && known.name == nameOfMember)
                return known.function.get();
        }
    }
    bool canBeConstructed = function && function->canBeConstructed;
    JSObject* standIn = StandInForValue::create(vm, realm, nameOfMember, function ? function->length : 0, !function || canBeConstructed);
    if (canBeConstructed && function->hasPrototype)
        standIn->putDirect(vm, vm.propertyNames->prototype, constructEmptyObject(realm), PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);

    Locker locker { membersLock };
    Vector<StandIn>& standIns = membersByVM().find(&vm)->value.standIns;
    standIns.removeAllMatching([](const StandIn& known) {
        return !known.function;
    });
    standIns.append(StandIn { Weak<JSObject>(standIn), WTF::move(message), owner ? Weak<JSObject>(owner) : Weak<JSObject>(), nameOfMember, !!function });
    return standIn;
}

JSValue takeOmittedMemberPlaceholder(JSGlobalObject* realm, const String& nameOfMember)
{
    String message = takeOmittedCodeErrorMessage(realm->vm());
    if (message.isNull())
        return { };
    return createThrowingPlaceholder(realm, WTF::move(message), nameOfMember, nullptr);
}

JSValue takeOmittedFunctionPlaceholder(JSGlobalObject* realm, const OmittedFunction& function, JSObject* owner)
{
    String message = takeOmittedCodeErrorMessage(realm->vm());
    if (message.isNull())
        return { };
    return createThrowingPlaceholder(realm, WTF::move(message), function.name, owner, &function);
}

MemberReificationScope::MemberReificationScope(bool isPresenceCheck)
    : m_previous(std::exchange(reifiesMemberForPresenceCheck, isPresenceCheck))
    , m_previousForRead(std::exchange(reifiesMemberForRead, !isPresenceCheck))
{
}

MemberReificationScope::~MemberReificationScope()
{
    reifiesMemberForPresenceCheck = m_previous;
    reifiesMemberForRead = m_previousForRead;
}

bool isReifyingForPresenceCheck()
{
    return reifiesMemberForPresenceCheck;
}

bool isPlaceholderForOmittedBuiltin(JSCell* cell)
{
    return cell->inherits<StandInForValue>();
}

bool isOmittedFunctionPlaceholder(JSCell* cell)
{
    if (!cell->inherits<StandInForValue>())
        return false;
    Locker locker { membersLock };
    auto found = membersByVM().find(&cell->vm());
    if (found == membersByVM().end())
        return false;
    return std::ranges::any_of(found->value.standIns, [&](const StandIn& standIn) {
        return standIn.isKnownToBeFunction && standIn.function.get() == cell;
    });
}

void explainOmittedMemberError(JSGlobalObject* realm, JSObject* error, JSValue base)
{
    VM& vm = realm->vm();
    String sentence;
    {
        Locker locker { membersLock };
        auto found = membersByVM().find(&vm);
        if (found == membersByVM().end())
            return;
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

bool hasOmittedMemberNotes(const VM* vm)
{
    Locker locker { membersLock };
    return membersByVM().contains(const_cast<VM*>(vm));
}

void clearOmittedCodeError(VM& vm)
{
    Locker locker { membersLock };
    membersByVM().remove(&vm);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
