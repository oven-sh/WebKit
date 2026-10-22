/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesAnswers.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTInstance.h"
#include "AOTModuleHost.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPropertyCaches.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "JSGlobalProxy.h"
#include "JSTypedArrayViewPrototypeInternal.h"
#include "PropertySlot.h"

namespace JSC { namespace AOT {

namespace {

struct FinishedRead {
    const ReadQuery& query;
    VM& vm;
    JSCell* cellOrNull;
};

}

static PropertyAnswer uncacheable(bool didFlatten = false)
{
    return PropertyAnswer::uncacheable(SlotAccess::Read, didFlatten);
}

static PropertyAnswer uncacheableOnce()
{
    return uncacheable(true);
}

static PropertyAnswer uncacheableAfter(ChainWalk&& walk)
{
    PropertyAnswer answer = uncacheable(walk.didFlatten);
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

static PropertyAnswer ownAnswer(PropertyAnswerKind kind, PropertyOffset offset = invalidOffset)
{
    PropertyAnswer answer;
    answer.kind = kind;
    answer.isOwn = true;
    answer.offset = offset;
    return answer;
}

static PropertyAnswer answerAboveReceiver(PropertyAnswerKind kind, ChainWalk&& walk)
{
    PropertyAnswer answer;
    answer.kind = kind;
    answer.offset = walk.offset;
    answer.holder = walk.holder;
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

static bool isLazyClassPrototype(JSObject* object)
{
    return object->classInfo() == ClassPrototype::info();
}

bool isFunctionOwnName(VM& vm, UniquedStringImpl* uid)
{
    return uid == vm.propertyNames->prototype.impl() || uid == vm.propertyNames->length.impl() || uid == vm.propertyNames->name.impl();
}

bool isNeverCachedName(VM& vm, UniquedStringImpl* uid)
{
    return parseIndex(PropertyName(uid)) || uid == vm.propertyNames->underscoreProto.impl() || (uid->isSymbol() && static_cast<SymbolImpl*>(uid)->isPrivate());
}

Structure* structureForPrimitive(JSGlobalObject* globalObject, JSValue primitive)
{
    if (primitive.isCell())
        return primitive.asCell()->structure();
    if (primitive.isNumber())
        return globalObject->numberObjectStructure();
    if (primitive.isBoolean())
        return globalObject->booleanObjectStructure();
    return nullptr;
}

std::optional<PropertyOffset> findOffset(VM& vm, Structure* structure, UniquedStringImpl* uid, ChainWalkMode mode, unsigned& attributes)
{
    std::optional<PropertyOffset> offset = structure->getWithoutMaterializing(uid, attributes);
    if (!offset && mode == ChainWalkMode::MayAllocate)
        offset = structure->get(vm, uid, attributes);
    return offset;
}

static bool findsLengthGetterOfTypedArrays(Structure* receiverStructure, const ChainWalk& walk)
{
    JSType type = receiverStructure->typeInfo().type();
    if (!isTypedArrayType(type) || !isValidOffset(walk.offset))
        return false;
    auto* accessors = uncheckedDowncast<GetterSetter>(walk.holder->getDirect(walk.offset).asCell());
    auto* getter = dynamicDowncast<JSFunction>(accessors->getter());
    // A host function has its intrinsic only where the JIT is on.
    if (!getter || !getter->isHostFunction())
        return false;
    if (getter->nativeFunction() == toTagged(NativeFunction(typedArrayViewProtoGetterFuncLength)))
        return true;
    return elementSize(typedArrayType(type)) == 1 && getter->nativeFunction() == toTagged(NativeFunction(typedArrayViewProtoGetterFuncByteLength));
}

static PropertyAnswer classifyReadAboveReceiver(const FinishedRead& read)
{
    const ReadQuery& query = read.query;
    const PropertySlot& slot = query.slot;
    if (slot.isCacheableCustom())
        return classifyCustomGetter(query);

    JSObject* receiver = query.base.isObject() ? asObject(read.cellOrNull) : nullptr;
    ChainWalk walk = walkChain({ query.globalObject, query.structureBefore, receiver, query.mode, ChainWalkUse::KeptForReadsThatAskObjects }, query.uid);
    if (walk.result == ChainWalkResult::Unknown)
        return uncacheableAfter(WTF::move(walk));
    // The empty slot may be filled by the next read, so what the walk found above it is not kept.
    if (walk.firstObjectWithEmptySlot) {
        walk.holder = walk.firstObjectWithEmptySlot;
        walk.offset = invalidOffset;
        return answerAboveReceiver(PropertyAnswerKind::InheritedField, WTF::move(walk));
    }
    if (slot.isUnset()) {
        if (walk.result == ChainWalkResult::Absent)
            return answerAboveReceiver(PropertyAnswerKind::Absent, WTF::move(walk));
        return uncacheableAfter(WTF::move(walk));
    }
    if (walk.result != ChainWalkResult::Found || walk.holder != slot.slotBase())
        return uncacheableAfter(WTF::move(walk));
    // A compiled object answers with the value of a field and names no offset, and lets nobody cache what its extra properties answer.
    bool isOwnedByCompiledObject = walk.owner->structure()->isCompiledStructure();
    bool isValue = slot.isCacheableValue() || (slot.isValue() && isOwnedByCompiledObject);
    if (isValue && !(walk.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
        return answerAboveReceiver(isValidOffset(walk.offset) ? PropertyAnswerKind::Value : PropertyAnswerKind::InheritedField, WTF::move(walk));
    bool isGetter = slot.isCacheableGetter() || (slot.isAccessor() && isOwnedByCompiledObject);
    if (isGetter && isValidOffset(walk.offset) && (walk.attributes & PropertyAttribute::Accessor)) {
        bool readsLength = findsLengthGetterOfTypedArrays(query.structureBefore, walk);
        return answerAboveReceiver(readsLength ? PropertyAnswerKind::TypedArrayLength : PropertyAnswerKind::Accessor, WTF::move(walk));
    }
    return uncacheableAfter(WTF::move(walk));
}

static std::optional<PropertyAnswer> classifyNeverCachedName(const FinishedRead& read)
{
    if (isNeverCachedName(read.vm, read.query.uid))
        return uncacheable();
    return std::nullopt;
}

static std::optional<PropertyAnswer> classifyPrimitiveRead(const FinishedRead& read)
{
    const ReadQuery& query = read.query;
    if (query.base.isObject())
        return std::nullopt;
    if (read.cellOrNull && read.vm.aotState().propertyCaches->isSharedByInstances())
        return uncacheable();
    if (query.base.isString()) {
        if (query.uid == read.vm.propertyNames->length.impl())
            return ownAnswer(PropertyAnswerKind::StringLength);
        // A property of the string itself is recorded without a slot base.
        if (!query.slot.isUnset() && !query.slot.slotBase())
            return uncacheable();
    }
    return classifyReadAboveReceiver(read);
}

// A global proxy gets another Structure with another target, and the realm of its Structure is its target.
static std::optional<PropertyAnswer> classifyGlobalProxyReceiver(const FinishedRead& read)
{
    const ReadQuery& query = read.query;
    Structure* structure = query.structureBefore;
    if (structure->typeInfo().type() != GlobalProxyType || structure->classInfoForCells() != JSGlobalProxy::info() || query.mode != ChainWalkMode::MayAllocate)
        return std::nullopt;
    if (read.cellOrNull->structure() != structure || structure->realm() != query.globalObject || uncheckedDowncast<JSGlobalProxy>(read.cellOrNull)->target() != query.globalObject)
        return std::nullopt;
    return classifyGlobalProxyRead(query);
}

static std::optional<PropertyAnswer> classifyNeverCachedReceiver(const FinishedRead& read)
{
    const ReadQuery& query = read.query;
    Structure* structure = query.structureBefore;
    JSObject* receiver = asObject(read.cellOrNull);
    bool prohibitsCaching = structure->typeInfo().prohibitsPropertyCaching() && !structure->isCompiledStructure();
    if (prohibitsCaching || isProxyType(structure->typeInfo().type()) || structure->needImpurePropertyWatchpoint())
        return uncacheable();
    if (structure->isDictionary()) {
        if (!structure->isUncacheableDictionary() && query.slot.isCacheableValue() && query.slot.slotBase() == receiver)
            return std::nullopt;
        return uncacheable(query.mode == ChainWalkMode::MayAllocate && flattenOnce(read.vm, receiver));
    }
    if (query.slot.isTaintedByOpaqueObject() && !isModuleNamespace(receiver)) {
        ChainWalk walk = walkChain({ query.globalObject, structure, receiver, query.mode }, query.uid);
        // The stand-in of a prototype object taints the lookup that gives it its members, and nothing on the chain changes behind that: what is filed now would stay. The next lookup is like any other.
        if (walk.result != ChainWalkResult::Unknown)
            return uncacheableOnce();
        return uncacheableAfter(WTF::move(walk));
    }
    return std::nullopt;
}

static std::optional<PropertyAnswer> classifyChangedReceiver(const FinishedRead& read)
{
    if (read.cellOrNull->structure() != read.query.structureBefore && !read.query.slot.isCacheableGetter())
        return uncacheableOnce();
    return std::nullopt;
}

static std::optional<PropertyAnswer> classifyStaticFieldRead(const FinishedRead& read)
{
    const ReadQuery& query = read.query;
    JSCell* cell = read.cellOrNull;
    if (!isCompiledClosure(cell) || query.mode != ChainWalkMode::MayAllocate)
        return std::nullopt;
    const FieldData* field = StaticFields::findFieldForCache(read.vm, cell, query.uid);
    if (!field)
        return std::nullopt;
    if (cell->structure() != query.structureBefore)
        return uncacheableOnce();
    PropertyAnswer answer = ownAnswer(PropertyAnswerKind::Variable);
    answer.field = field;
    answer.variable = field->slot;
    return answer;
}

// "in" and hasOwn answer from the kind of the read slot alone.
static constexpr bool impliesPresence(Slot::Kind kind)
{
    return Slot::presenceOf(kind) == SlotPresence::Present;
}

static std::optional<PropertyAnswer> classifyCompiledClosureRead(const FinishedRead& read)
{
    const ReadQuery& query = read.query;
    VM& vm = read.vm;
    if (query.structureBefore->typeInfo().type() == SlimClosureType) {
        if (query.uid == vm.propertyNames->length.impl())
            return ownAnswer(PropertyAnswerKind::ClosureLength);
        if (query.uid == vm.propertyNames->name.impl())
            return ownAnswer(PropertyAnswerKind::ClosureName);
    }
    if (impliesPresence(Slot::Kind::CustomGetter))
        return std::nullopt;
    if (query.slot.isUnset() && query.uid == vm.propertyNames->prototype.impl() && closureHandlesPrototypeProperty(read.cellOrNull))
        return classifyAbsentClosurePrototype(query);
    return std::nullopt;
}

static std::optional<PropertyAnswer> classifyCompiledReceiverRead(const FinishedRead& read)
{
    const ReadQuery& query = read.query;
    if (!query.structureBefore->isCompiledStructure())
        return std::nullopt;
    if (!Object::structureGuaranteesAbsence(query.structureBefore, query.uid)) {
        if (read.cellOrNull->structure() != query.structureBefore)
            return uncacheableOnce();
        return classifyCompiledObjectRead(query, *uncheckedDowncast<Object>(read.cellOrNull));
    }
    if (!query.slot.isUnset() && query.slot.slotBase() == read.cellOrNull)
        return uncacheable();
    return classifyReadAboveReceiver(read);
}

static std::optional<PropertyAnswer> classifyOwnOffsetRead(const FinishedRead& read)
{
    const ReadQuery& query = read.query;
    const PropertySlot& slot = query.slot;
    bool isValue = slot.isCacheableValue();
    if ((!isValue && !slot.isCacheableGetter()) || !query.structureBefore->propertyAccessesAreCacheable())
        return std::nullopt;
    unsigned attributes = 0;
    std::optional<PropertyOffset> offset = findOffset(read.vm, query.structureBefore, query.uid, query.mode, attributes);
    if (!offset || !isValidOffset(*offset) || *offset != slot.cachedOffset())
        return std::nullopt;
    if (isValue) {
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
            return std::nullopt;
        return ownAnswer(PropertyAnswerKind::Value, *offset);
    }
    if (!(attributes & PropertyAttribute::Accessor))
        return std::nullopt;
    return ownAnswer(PropertyAnswerKind::Accessor, *offset);
}

bool hasCacheableArrayLength(VM& vm, JSCell* base, Structure* structure, UniquedStringImpl* uid)
{
    if (!isJSArray(base) || uid != vm.propertyNames->length.impl() || structure->mayBePrototype())
        return false;
    IndexingType mode = structure->indexingMode();
    return hasUndecided(mode) || hasInt32(mode) || hasDouble(mode) || hasContiguous(mode);
}

static std::optional<PropertyAnswer> classifyOwnRead(const FinishedRead& read)
{
    const ReadQuery& query = read.query;
    const PropertySlot& slot = query.slot;
    if (isModuleNamespace(read.cellOrNull))
        return classifyNamespaceRead(query);
    if (slot.isUnset() || slot.slotBase() != read.cellOrNull)
        return std::nullopt;
    if (auto answer = classifyOwnOffsetRead(read))
        return answer;
    if (slot.isCacheableCustom())
        return classifyCustomGetter(query);
    if (hasCacheableArrayLength(read.vm, read.cellOrNull, query.structureBefore, query.uid))
        return ownAnswer(PropertyAnswerKind::ArrayLength);
    return uncacheable();
}

bool structureProvesOwnAbsence(VM& vm, JSCell* base, Structure* structure, UniquedStringImpl* uid, ChainWalkMode mode)
{
    if (structure->isDictionary() || !structure->propertyAccessesAreCacheable() || isModuleNamespace(base))
        return false;
    if (!structure->propertyAccessesAreCacheableForAbsence() && (mode == ChainWalkMode::AllocatesNothing || mayHaveLazyProperty(vm, base, uid, LookupPosition::Receiver)))
        return false;
    unsigned attributes = 0;
    std::optional<PropertyOffset> offset = findOffset(vm, structure, uid, mode, attributes);
    if (!offset || isValidOffset(*offset))
        return false;
    // A slim closure that has properties has its length, its name and, where it can construct, its prototype among them.
    JSType type = structure->typeInfo().type();
    return !((isJSFunctionType(type) || type == SlimClosureType) && isFunctionOwnName(vm, uid));
}

static std::optional<PropertyAnswer> classifyInheritedRead(const FinishedRead& read)
{
    const ReadQuery& query = read.query;
    JSCell* cell = read.cellOrNull;
    if (!structureProvesOwnAbsence(read.vm, cell, query.structureBefore, query.uid, query.mode))
        return uncacheable();
    ASSERT(!isCompiledClosure(cell) || !uncheckedDowncast<Closure>(cell)->isLazyClassObject());
    return classifyReadAboveReceiver(read);
}

PropertyAnswer classifyRead(const ReadQuery& query)
{
    using ReadRule = std::optional<PropertyAnswer> (*)(const FinishedRead&);
    static constexpr ReadRule rules[] = {
        classifyNeverCachedName,
        classifyPrimitiveRead,
        classifyGlobalProxyReceiver,
        classifyNeverCachedReceiver,
        classifyChangedReceiver,
        classifyStaticFieldRead,
        classifyCompiledClosureRead,
        classifyCompiledReceiverRead,
        classifyOwnRead,
        classifyInheritedRead,
    };

    if (!query.structureBefore)
        return uncacheable();
    FinishedRead read { query, query.globalObject->vm(), query.base.isCell() ? query.base.asCell() : nullptr };
    for (ReadRule rule : rules) {
        if (auto answer = rule(read)) {
            ASSERT(answer->access == SlotAccess::Read);
            return WTF::move(*answer);
        }
    }
    return uncacheable();
}

LazyPrototypeRead classifyReadThroughLazyPrototypes(JSGlobalObject* globalObject, JSObject* base, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    if (isNeverCachedName(vm, uid))
        return { };
    auto found = State::of(globalObject).objects->classes.findThroughLazyPrototypes(vm, base, uid, false);
    if (!found)
        return { };
    if (!found->member && found->firstObjectAbove)
        return { LazyPrototypeReadResult::ContinuesAbove, { }, found->firstObjectAbove };

    LazyPrototypeRead read;
    read.result = LazyPrototypeReadResult::Answered;
    read.answer.kind = PropertyAnswerKind::Absent;
    if (found->member) {
        auto& member = *found->member;
        read.answer.kind = member.isAccessor ? PropertyAnswerKind::Accessor : PropertyAnswerKind::Value;
        read.answer.offset = member.offset;
        read.answer.holder = member.holder;
    }
    for (JSObject* prototype = base->getPrototypeDirect().getObject(); prototype && isLazyClassPrototype(prototype); prototype = prototype->getPrototypeDirect().getObject())
        read.answer.changeableObjects.append(prototype);
    return read;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
