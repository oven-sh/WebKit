/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTPropertyCachesNamedAccess.h"
#include "AOTState.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

namespace {

enum class Question : bool { In, HasOwn };

struct PresenceLookup {
    Instance* instance;
    JSObject* object;
    Structure* structureBefore;
    CachedName name;
    bool shouldFile;
};

struct KeyedPresenceLookup {
    JSGlobalObject* globalObject;
    JSObject* object;
    JSValue property;
    Question question;
};

}

static ReadQuery readQueryFor(const PresenceLookup& lookup, const PropertySlot& slot)
{
    return { lookup.instance->globalObject, lookup.object, lookup.structureBefore, lookup.name.uid, slot };
}

static bool answerImpliesResult(const PropertyAnswer& answer, Structure* structure, Question question, bool result)
{
    Slot slot = slotFor(answer, structure);
    switch (Slot::presenceOf(slot.kind)) {
    case SlotPresence::Unknown:
        return slot.kind == Slot::Kind::Uncacheable;
    case SlotPresence::ReadsSlot:
        return true;
    case SlotPresence::Present:
        return result == (question == Question::In || slot.has(Slot::isOwn));
    case SlotPresence::Absent:
        return !result;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void cacheIfImplied(const PresenceLookup& lookup, const PropertyAnswer& answer, Question question, bool result)
{
    if (!answerImpliesResult(answer, lookup.structureBefore, question, result))
        return;
    lookup.instance->state->propertyCaches->cache({ lookup.structureBefore, lookup.name, SlotAccess::Read }, answer);
}

static bool lookUpOnChain(const PresenceLookup& lookup, PropertySlot& slot, std::optional<PropertyAnswer>& lazyPrototypeAnswer)
{
    JSGlobalObject* globalObject = lookup.instance->globalObject;
    PropertyName propertyName(lookup.name.uid);
    LazyPrototypeRead lazy = classifyReadThroughLazyPrototypes(globalObject, lookup.object, lookup.name.uid);
    switch (lazy.result) {
    case LazyPrototypeReadResult::Answered:
        lazyPrototypeAnswer = WTF::move(lazy.answer);
        return lazyPrototypeAnswer->kind != PropertyAnswerKind::Absent;
    case LazyPrototypeReadResult::ContinuesAbove:
        return lazy.firstObjectAbove->getPropertySlot(globalObject, propertyName, slot);
    case LazyPrototypeReadResult::NotApplicable:
        return lookup.object->getPropertySlot(globalObject, propertyName, slot);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasPropertyAndCache(const PresenceLookup& lookup)
{
    auto scope = DECLARE_THROW_SCOPE(*lookup.instance->vm);
    PropertySlot slot(lookup.object, PropertySlot::InternalMethodType::HasProperty);
    std::optional<PropertyAnswer> answer;
    bool result = lookUpOnChain(lookup, slot, answer);
    RETURN_IF_EXCEPTION(scope, false);
    if (!lookup.shouldFile)
        return result;
    if (!answer)
        answer = classifyRead(readQueryFor(lookup, slot));
    cacheIfImplied(lookup, *answer, Question::In, result);
    return result;
}

static bool structureDecidesOwnAbsence(VM& vm, const PresenceLookup& lookup)
{
    Structure* structure = lookup.structureBefore;
    if (lookup.object->structure() != structure)
        return false;
    return structure->isCompiledStructure() || structureProvesOwnAbsence(vm, lookup.object, structure, lookup.name.uid, ChainWalkMode::MayAllocate);
}

static bool hasOwnPropertyAndCache(const PresenceLookup& lookup)
{
    VM& vm = *lookup.instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = lookup.object;

    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    bool result = object->methodTable()->getOwnPropertySlot(object, lookup.instance->globalObject, PropertyName(lookup.name.uid), slot);
    RETURN_IF_EXCEPTION(scope, false);
    if (!lookup.shouldFile)
        return result;
    if (result) {
        cacheIfImplied(lookup, classifyRead(readQueryFor(lookup, slot)), Question::HasOwn, true);
        return true;
    }

    if (!structureDecidesOwnAbsence(vm, lookup))
        return false;
    PropertySlot inquiry(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    std::optional<PropertyAnswer> answer;
    lookUpOnChain(lookup, inquiry, answer);
    inquiry.disallowVMEntry.reset();
    RETURN_IF_EXCEPTION(scope, false);
    if (inquiry.isTaintedByOpaqueObject())
        return false;
    if (!answer)
        answer = classifyRead(readQueryFor(lookup, inquiry));
    cacheIfImplied(lookup, *answer, Question::HasOwn, false);
    return false;
}

static bool hasAndCache(Instance* instance, JSObject* object, const CachedName& name, Question question)
{
    bool shouldFile = instance->state->propertyCaches->shouldFile(object->structureID(), name.uid, SlotAccess::Read);
    if (instance->state->propertyCaches->countsNames()) [[unlikely]]
        instance->state->propertyCaches->countMiss(question == Question::In ? "in"_s : "has own"_s, shouldFile, object, name.uid);
    if (shouldFile) {
        Object::ensureStructureForExtraPropertiesIfNeeded(*instance->vm, object);
        createClassObjectsOnChain(*instance->vm, object, name.uid);
    }
    Structure* structureBefore = WTF::opaque(object->structure());
    PresenceLookup lookup { instance, object, structureBefore, name, shouldFile };
    bool result = question == Question::In ? hasPropertyAndCache(lookup) : hasOwnPropertyAndCache(lookup);
#if !CPU(X86_64)
    if (!result && object->structure() == structureBefore)
        instance->state->propertyCaches->tables().fileAbsenceFilter(instance->globalObject, object);
#endif
    ensureStillAliveHere(structureBefore);
    return result;
}

bool hasPropertyAndCache(Instance* instance, JSObject* object, const CachedName& name)
{
    return hasAndCache(instance, object, name, Question::In);
}

JSC_DEFINE_JIT_OPERATION(operationAOTInByIdAndCache, size_t, (Instance* instance, EncodedJSValue encodedBase, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInByIdAndCache);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, hasAndCache(instance, asObject(base), instance->state->propertyCaches->nameOf(key), Question::In));
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasOwnByIdAndCache, size_t, (Instance* instance, EncodedJSValue encodedBase, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTHasOwnByIdAndCache);
    JSValue base = JSValue::decode(encodedBase);
    if (base.isObject()) [[likely]]
        OPERATION_RETURN(scope, hasAndCache(instance, asObject(base), instance->state->propertyCaches->nameOf(key), Question::HasOwn));
    JSObject* object = base.toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, object->hasOwnProperty(globalObject, identifierAt(instance, key)));
}

static bool hasIndexWithoutCaching(const KeyedPresenceLookup& lookup, uint32_t index)
{
    if (lookup.question == Question::In)
        return lookup.object->hasProperty(lookup.globalObject, index);
    return lookup.object->hasOwnProperty(lookup.globalObject, index);
}

static bool hasNameWithoutCaching(const KeyedPresenceLookup& lookup, PropertyName name)
{
    if (lookup.question == Question::In)
        return lookup.object->hasProperty(lookup.globalObject, name);
    return lookup.object->hasOwnProperty(lookup.globalObject, name);
}

static std::optional<CachedName> nameOfKeyForPresence(const KeyedPresenceLookup& lookup, Identifier& identifier, bool& result)
{
    JSGlobalObject* globalObject = lookup.globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue property = lookup.property;
    result = false;

    if (std::optional<uint32_t> index = property.tryGetAsUint32Index()) {
        scope.release();
        result = hasIndexWithoutCaching(lookup, *index);
        return std::nullopt;
    }
    if (UniquedStringImpl* atom = atomOfKey(property))
        identifier = Identifier::fromUid(vm, atom);
    else {
        if (property.isString()) {
            auto existingAtom = asString(property)->toExistingAtomString(globalObject);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            if (!existingAtom) {
                auto text = asString(property)->value(globalObject);
                RETURN_IF_EXCEPTION(scope, std::nullopt);
                if (!parseIndex(*text.data.impl()) && classifyChainLookups(lookup.object) == ChainLookups::Ordinary)
                    return std::nullopt;
            }
        }
        identifier = property.toPropertyKey(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
    }
    if (!property.isString() && !property.isSymbol()) {
        scope.release();
        result = hasNameWithoutCaching(lookup, identifier);
        return std::nullopt;
    }
    if (std::optional<uint32_t> index = parseIndex(identifier)) {
        scope.release();
        result = hasIndexWithoutCaching(lookup, *index);
        return std::nullopt;
    }
    return CachedName { identifier.impl(), std::nullopt };
}

static OperationReturnType<size_t> hasByValAndCache(Instance* instance, JSObject* object, JSValue property, Question question)
{
    auto scope = DECLARE_THROW_SCOPE(*instance->vm);
    if (auto longKeyName = findNameEqualToLongString(object, property, question == Question::In)) [[unlikely]]
        OPERATION_RETURN(scope, !!*longKeyName);

    Identifier identifier;
    bool result = false;
    std::optional<CachedName> name = nameOfKeyForPresence({ instance->globalObject, object, property, question }, identifier, result);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!name)
        OPERATION_RETURN(scope, result);
    OPERATION_RETURN(scope, hasAndCache(instance, object, *name, question));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInByValAndCache, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInByValAndCache);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, 0);
    }
    scope.release();
    return hasByValAndCache(instance, asObject(base), JSValue::decode(encodedProperty), Question::In);
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasOwnByValAndCache, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTHasOwnByValAndCache);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    if (base.isObject()) [[likely]] {
        scope.release();
        return hasByValAndCache(instance, asObject(base), property, Question::HasOwn);
    }
    JSObject* object = base.toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    Identifier name = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, object->hasOwnProperty(globalObject, name));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
