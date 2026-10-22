/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTGlobalBindings.h"
#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSGlobalLexicalEnvironment.h"
#include "JSGlobalProxy.h"
#include "SymbolTableInlines.h"
#include <bit>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT {

namespace {

struct BindingAccess {
    BindingAccess(Instance* instance, uint32_t number)
        : instance(*instance)
        , vm(*instance->vm)
        , globalObject(instance->globalObject)
        , bindings(*instance->state->globalBindings)
        , number(number)
        , name(bindings.nameOf(number))
    {
    }

    UniquedStringImpl* uid() const { return name.impl(); }
    void didNotFile() const { count(instance, RuntimeCounterID::GlobalBindingNotFiled); }

    Instance& instance;
    VM& vm;
    JSGlobalObject* globalObject;
    GlobalBindings& bindings;
    uint32_t number;
    Identifier name;
};

} // anonymous namespace

static bool canCachePropertiesOf(JSGlobalObject* globalObject)
{
    TypeInfo typeInfo = globalObject->structure()->typeInfo();
    return !typeInfo.prohibitsPropertyCaching() && !typeInfo.getOwnPropertySlotIsImpure() && !typeInfo.getOwnPropertySlotIsImpureForPropertyAbsence();
}

static bool isFixed(const PropertySlot& slot, const Identifier& identifier)
{
    constexpr unsigned fixed = PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
    return (slot.attributes() & fixed) == fixed || identifier.isPrivateName();
}

static bool isConstantVariable(const SymbolTableEntry::Fast& entry, const Identifier& identifier)
{
    return entry.isReadOnly() || identifier.isPrivateName();
}

// A host may let a later script declare a const again, and the variable then gets another value.
static bool constantsOfScriptsKeepTheirValues(VM& vm)
{
    return vm.globalConstRedeclarationShouldThrow() && !vm.allowRedeclaringSymbols();
}

static bool forwardsToGlobalObject(JSObject* globalThis, JSGlobalObject* globalObject)
{
    if (globalThis == globalObject)
        return true;
    return globalThis->classInfo() == JSGlobalProxy::info() && uncheckedDowncast<JSGlobalProxy>(globalThis)->target() == globalObject;
}

// An uncacheable dictionary moves and drops properties under one StructureID. It is flattened once, and the next miss files.
static bool structureSaysWherePropertiesAre(const BindingAccess& access)
{
    Structure* structure = access.globalObject->structure();
    if (structure->propertyAccessesAreCacheable())
        return true;
    if (structure->isUncacheableDictionary() && !structure->hasBeenFlattenedBefore())
        access.globalObject->flattenDictionaryObject(access.vm);
    return false;
}

// JSSegmentedVariableObject never moves a variable.
static int64_t locationOfVariable(JSSegmentedVariableObject* scope, const SymbolTableEntry::Fast& entry)
{
    return std::bit_cast<int64_t>(scope->variableAt(entry.scopeOffset()).slot());
}

static int64_t locationOfProperty(JSGlobalObject* globalObject, PropertyOffset offset)
{
    if (isInlineOffset(offset))
        return std::bit_cast<int64_t>(globalObject->locationForOffset(offset));
    return offsetInButterfly(offset) * static_cast<int64_t>(sizeof(EncodedJSValue));
}

// The test that compiled code makes where it probes a record inline. In a block that was taken for rare it calls the operation each time instead, and such a block may run often.
static EncodedJSValue* filedLocationOrNull(Instance* instance, uint32_t number, bool seesScriptBindings)
{
    const GlobalBinding& record = instance->state->globalBindings->at(number);
    JSGlobalObject* globalObject = instance->globalObject;
    if (record.structureID != globalObject->structureID().bits())
        return nullptr;
    if (seesScriptBindings && record.lexicalBindingEpoch != globalObject->globalLexicalBindingEpoch())
        return nullptr;
    int64_t base = record.location < 0 ? std::bit_cast<int64_t>(globalObject->butterfly()) : 0;
    return std::bit_cast<EncodedJSValue*>(base + record.location);
}

static bool tryStoreAtFiledLocation(Instance* instance, uint32_t number, bool seesScriptBindings, JSValue value)
{
    EncodedJSValue* location = filedLocationOrNull(instance, number, seesScriptBindings);
    if (!location)
        return false;
    *location = JSValue::encode(value);
    instance->vm->writeBarrier(instance->globalObject, value);
    return true;
}

static void fileReadOfLexicalVariable(const BindingAccess& access, JSGlobalLexicalEnvironment* lexical, JSValue value)
{
    SymbolTableEntry::Fast entry = lexical->symbolTable()->get(access.uid());
    RELEASE_ASSERT(!entry.isNull());
    if (entry.isReadOnly() && constantsOfScriptsKeepTheirValues(access.vm)) {
        access.bindings.fileConstant(access.vm, access.number, nullptr, value);
        return;
    }
    access.bindings.fileLocation(access.number, access.globalObject->structure(), locationOfVariable(lexical, entry));
}

static void fileReadOfVariable(const BindingAccess& access, JSValue value)
{
    JSGlobalObject* globalObject = access.globalObject;
    SymbolTableEntry::Fast entry = globalObject->symbolTable()->get(access.uid());
    if (entry.isNull()) {
        access.didNotFile();
        return;
    }
    if (isConstantVariable(entry, access.name)) {
        access.bindings.fileConstant(access.vm, access.number, nullptr, value);
        return;
    }
    access.bindings.fileLocation(access.number, globalObject->structure(), locationOfVariable(globalObject, entry));
}

static void fileReadOfProperty(const BindingAccess& access, const PropertySlot& slot, JSValue value)
{
    JSGlobalObject* globalObject = access.globalObject;
    if (isFixed(slot, access.name) || globalObject->structure()->isImmutablePropertyForReader(access.uid())) {
        access.bindings.fileConstant(access.vm, access.number, nullptr, value);
        return;
    }
    if (access.bindings.fileConstant(access.vm, access.number, globalObject, value))
        return;
    if (!structureSaysWherePropertiesAre(access)) {
        access.didNotFile();
        return;
    }
    access.bindings.fileLocation(access.number, globalObject->structure(), locationOfProperty(globalObject, slot.cachedOffset()));
}

// The lookup may have created the property, so the Structure is asked for after it.
static void fileRead(const BindingAccess& access, const PropertySlot& slot, JSValue value)
{
    if (!canCachePropertiesOf(access.globalObject) || slot.isTaintedByOpaqueObject() || slot.slotBase() != access.globalObject || !value) {
        access.didNotFile();
        return;
    }
    if (slot.isCacheableValue()) {
        fileReadOfProperty(access, slot, value);
        return;
    }
    if (slot.isValue()) {
        fileReadOfVariable(access, value);
        return;
    }
    access.didNotFile();
}

static void fileAbsence(const BindingAccess& access)
{
    JSGlobalObject* globalObject = access.globalObject;
    if (!canCachePropertiesOf(globalObject)) {
        access.didNotFile();
        return;
    }
    ChainWalk walk = walkChain({ globalObject, globalObject->structure(), globalObject }, access.uid());
    if (walk.result != ChainWalkResult::Absent) {
        access.didNotFile();
        return;
    }
    access.bindings.fileAbsence(access.vm, access.number, walk.changeableObjects.span());
}

template<bool seesScriptBindings>
static JSValue getGlobalBinding(Instance* instance, uint32_t number)
{
    if (EncodedJSValue* location = filedLocationOrNull(instance, number, seesScriptBindings)) {
        if (JSValue value = JSValue::decode(*location))
            return value;
    }
    BindingAccess access(instance, number);
    ASSERT(access.bindings.seesScriptBindings(number) == seesScriptBindings);
    VM& vm = access.vm;
    JSGlobalObject* globalObject = access.globalObject;
    const Identifier& identifier = access.name;
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* receiver = globalObject;
    if constexpr (seesScriptBindings) {
        JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
        PropertySlot lexicalSlot(lexical, PropertySlot::InternalMethodType::Get);
        if (JSGlobalLexicalEnvironment::getOwnPropertySlot(lexical, globalObject, identifier, lexicalSlot)) {
            JSValue value = lexicalSlot.getValue(globalObject, identifier);
            if (value == jsTDZValue()) {
                throwException(globalObject, scope, createTDZError(globalObject, identifier));
                return { };
            }
            fileReadOfLexicalVariable(access, lexical, value);
            return value;
        }
    } else {
        receiver = globalObject->globalThis();
        if (!forwardsToGlobalObject(receiver, globalObject)) [[unlikely]] {
            access.didNotFile();
            RELEASE_AND_RETURN(scope, JSValue(receiver).get(globalObject, identifier));
        }
    }

    PropertySlot slot(receiver, PropertySlot::InternalMethodType::Get);
    bool found = globalObject->getPropertySlot(globalObject, identifier, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found) {
        if (!access.bindings.answersUndefinedWhereAbsent(number)) {
            access.didNotFile();
            throwException(globalObject, scope, createUndefinedVariableError(globalObject, identifier));
            return { };
        }
        fileAbsence(access);
        return jsUndefined();
    }

    if (slot.isAccessor()) {
        access.didNotFile();
        RELEASE_AND_RETURN(scope, slot.getValue(globalObject, identifier));
    }
    JSValue value = slot.getValue(globalObject, identifier);
    RETURN_IF_EXCEPTION(scope, { });
    fileRead(access, slot, value);
    return value;
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetGlobalBinding, EncodedJSValue, (Instance* instance, uint32_t binding))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetGlobalBinding);
    OPERATION_RETURN(scope, JSValue::encode(getGlobalBinding<true>(instance, binding)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetGlobalObjectBinding, EncodedJSValue, (Instance* instance, uint32_t binding))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetGlobalObjectBinding);
    OPERATION_RETURN(scope, JSValue::encode(getGlobalBinding<false>(instance, binding)));
}

// The inline store tests the Structure and the epoch and nothing else, and its barrier names the global object. So only a writable data property of the global object itself is filed.
static void fileStore(const BindingAccess& access, const PutPropertySlot& slot)
{
    VM& vm = access.vm;
    JSGlobalObject* globalObject = access.globalObject;
    if (!canCachePropertiesOf(globalObject) || globalObject->structure()->isImmutableProperty(access.uid()) || !structureSaysWherePropertiesAre(access)) {
        access.didNotFile();
        return;
    }
    Structure* structure = globalObject->structure();
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, access.uid(), attributes);
    bool isWritableDataProperty = isValidOffset(offset) && !(attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue);
    if (!isWritableDataProperty || !slot.isCacheablePut() || slot.type() != PutPropertySlot::ExistingProperty || slot.base() != globalObject || slot.cachedOffset() != offset) {
        access.didNotFile();
        return;
    }

    structure->didCachePropertyReplacement(vm, offset);
    access.bindings.didFileStore(access.number);
    access.bindings.fileLocation(access.number, structure, locationOfProperty(globalObject, offset));
}

template<bool isStrict>
static void setGlobalBinding(Instance* instance, uint32_t number, JSValue value)
{
    if (tryStoreAtFiledLocation(instance, number, true, value))
        return;
    BindingAccess access(instance, number);
    ASSERT(access.bindings.seesScriptBindings(number));
    VM& vm = access.vm;
    JSGlobalObject* globalObject = access.globalObject;
    const Identifier& identifier = access.name;
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
    PropertySlot lexicalSlot(lexical, PropertySlot::InternalMethodType::Get);
    if (JSGlobalLexicalEnvironment::getOwnPropertySlot(lexical, globalObject, identifier, lexicalSlot)) {
        access.didNotFile();
        if (lexicalSlot.getValue(globalObject, identifier) == jsTDZValue()) {
            throwException(globalObject, scope, createTDZError(globalObject, identifier));
            return;
        }
        PutPropertySlot slot(lexical, true);
        scope.release();
        JSGlobalLexicalEnvironment::put(lexical, globalObject, identifier, value, slot);
        return;
    }
    if constexpr (isStrict) {
        bool exists = globalObject->hasProperty(globalObject, identifier);
        RETURN_IF_EXCEPTION(scope, void());
        if (!exists) {
            access.didNotFile();
            throwException(globalObject, scope, createUndefinedVariableError(globalObject, identifier));
            return;
        }
    }

    Structure* structureBefore = globalObject->structure();
    unsigned lexicalBindingEpochBefore = globalObject->globalLexicalBindingEpoch();
    PutPropertySlot slot(globalObject, isStrict);
    globalObject->methodTable()->put(globalObject, globalObject, identifier, value, slot);
    RETURN_IF_EXCEPTION(scope, void());

    if (globalObject->structure() != structureBefore || globalObject->globalLexicalBindingEpoch() != lexicalBindingEpochBefore) {
        access.didNotFile();
        return;
    }
    fileStore(access, slot);
}

template<bool isStrict>
static void setGlobalObjectBinding(Instance* instance, uint32_t number, JSValue value)
{
    if (tryStoreAtFiledLocation(instance, number, false, value))
        return;
    BindingAccess access(instance, number);
    ASSERT(!access.bindings.seesScriptBindings(number));
    VM& vm = access.vm;
    JSGlobalObject* globalObject = access.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);

    Structure* structureBefore = globalObject->structure();
    JSObject* receiver = globalObject->globalThis();
    PutPropertySlot slot(receiver, isStrict);
    receiver->methodTable()->put(receiver, globalObject, access.name, value, slot);
    RETURN_IF_EXCEPTION(scope, void());

    if (globalObject->structure() != structureBefore) {
        access.didNotFile();
        return;
    }
    fileStore(access, slot);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetGlobalBinding, void, (Instance* instance, uint32_t binding, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetGlobalBinding);
    setGlobalBinding<true>(instance, binding, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetGlobalBindingSloppy, void, (Instance* instance, uint32_t binding, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetGlobalBindingSloppy);
    setGlobalBinding<false>(instance, binding, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetGlobalObjectBinding, void, (Instance* instance, uint32_t binding, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetGlobalObjectBinding);
    setGlobalObjectBinding<true>(instance, binding, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetGlobalObjectBindingSloppy, void, (Instance* instance, uint32_t binding, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetGlobalObjectBindingSloppy);
    setGlobalObjectBinding<false>(instance, binding, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

// A record holds what is the global object's own or a variable of a script, so an inquiry that runs no code of the program finds it.
static JSValue lookUpWithoutRunningCode(VM& vm, JSGlobalObject* globalObject, const Identifier& identifier, bool seesScriptBindings)
{
    if (seesScriptBindings) {
        JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
        SymbolTableEntry::Fast entry = lexical->symbolTable()->get(identifier.impl());
        if (!entry.isNull())
            return lexical->variableAt(entry.scopeOffset()).get();
    }
    PropertySlot slot(globalObject, PropertySlot::InternalMethodType::VMInquiry, &vm);
    if (!globalObject->methodTable()->getOwnPropertySlot(globalObject, globalObject, identifier, slot) || !slot.isValue())
        return { };
    return slot.getValue(globalObject, identifier);
}

static bool isAbsentWithoutRunningCode(VM& vm, JSGlobalObject* globalObject, const Identifier& identifier)
{
    PropertySlot slot(globalObject, PropertySlot::InternalMethodType::VMInquiry, &vm);
    return !globalObject->getPropertySlot(globalObject, identifier, slot) && !slot.isTaintedByOpaqueObject();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTValidateGlobalBinding, void, (Instance* instance, uint32_t binding, EncodedJSValue value))
{
    VM& vm = *instance->vm;
    DeferGCForAWhile deferGC(vm);
    GlobalBindings& bindings = *instance->state->globalBindings;
    Identifier identifier = bindings.nameOf(binding);
    JSValue answered = JSValue::decode(value);
    JSValue found = lookUpWithoutRunningCode(vm, instance->globalObject, identifier, bindings.seesScriptBindings(binding));
    if (found == answered)
        return;
    if (!found && answered.isUndefined() && bindings.answersUndefinedWhereAbsent(binding) && isAbsentWithoutRunningCode(vm, instance->globalObject, identifier))
        return;
    dataLogLn("AOT: the record of the global name ", identifier.string(), " answers with another value than a lookup finds");
    crashOnValidationFailure(vm, { ValidationFailureKind::LookupDiffers, { }, { }, answered });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
