/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperationsObjects.h"

#include "ObjectAllocationProfileInlines.h"
#include "ObjectConstructorInlines.h"

#if ENABLE(AOT)

#include "AOTGraph.h"
#include "AOTInlineCaches.h"
#include "AOTOperationHelpers.h"
#include "ArrayConstructor.h"
#include "ArrayPrototypeInlines.h"
#include "ClonedArguments.h"
#include "CommonSlowPathsInlines.h"
#include "DefinePropertyAttributes.h"
#include "DirectArguments.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "GetterSetter.h"
#include "JSAsyncFunction.h"
#include "JSAsyncFunctionGenerator.h"
#include "JSAsyncGenerator.h"
#include "JSAsyncGeneratorFunction.h"
#include "JSCellButterfly.h"
#include "JSGenerator.h"
#include "JSGeneratorFunction.h"
#include "JSLexicalEnvironmentInlines.h"
#include "JSMapInlines.h"
#include "JSPromise.h"
#include "JSPromiseConstructor.h"
#include "JSPropertyNameEnumeratorInlines.h"
#include "JSSentinel.h"
#include "JSSetInlines.h"
#include "JSWithScope.h"
#include "MegamorphicCache.h"
#include "ObjectConstructor.h"
#include "ObjectPrototype.h"
#include "RegExpObjectInlines.h"
#include "ScopedArguments.h"
#include "SymbolTableInlines.h"

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTNewObject, JSObject*, (Instance* instance, uint32_t inlineCapacity, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    if (StructureID structureID = cache[0].structureID)
        OPERATION_RETURN(scope, constructEmptyObject(vm, structureID.decode()));

    ObjectAllocationProfile profile;
    profile.initializeProfile(vm, globalObject, globalObject, globalObject->objectPrototype(), inlineCapacity);
    Structure* structure = profile.structure();
    fillAllocationCache(vm, callerData(instance, callFrame), cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), structure->inlineCapacity());
    OPERATION_RETURN(scope, constructEmptyObject(vm, structure));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewTypedObject, JSObject*, (Instance* instance, uint32_t layoutID, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    if (StructureID structureID = cache[0].structureID)
        OPERATION_RETURN(scope, Instance::newObjectOf(vm, structureID.decode()));
    Structure* structure = instance->emptyStructureForLayout(safeCast<uint16_t>(layoutID));
    fillAllocationCache(vm, callerData(instance, callFrame), cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator));
    OPERATION_RETURN(scope, Instance::newObjectOf(vm, structure));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNoteClass, void, (Instance* instance, EncodedJSValue encodedConstructor, EncodedJSValue encodedPrototype, uint32_t layoutID))
{
    AOT_OPERATION_BEGIN(instance);
    auto* constructor = dynamicDowncast<JSFunction>(JSValue::decode(encodedConstructor));
    JSObject* prototype = JSValue::decode(encodedPrototype).getObject();
    RELEASE_ASSERT(constructor && prototype && constructor->canUseAllocationProfiles());
    FunctionRareData* rareData = constructor->ensureRareDataAndObjectAllocationProfile(globalObject, TypedLayoutTable::inlineSlots(safeCast<uint16_t>(layoutID)));
    OPERATION_RETURN_IF_EXCEPTION(scope);
    Structure* usual = rareData->objectAllocationStructure();
    RELEASE_ASSERT(!usual->hasPolyProto() && usual->storedPrototypeObject() == prototype);
    if (!TypedLayoutTable::tryToInheritFrom(vm, safeCast<uint16_t>(layoutID), prototype)) [[unlikely]] {
        throwTypeError(globalObject, scope, TypedFieldOfInheritorError);
        OPERATION_RETURN(scope);
    }
    rareData->objectAllocationProfile()->replaceStructure(vm, rareData, instance->emptyStructureForLayout(safeCast<uint16_t>(layoutID), prototype));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMakeAtom, void, (EncodedJSValue value))
{
    TypedLayoutTable::atomizeIfString(JSValue::decode(value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCloneObject, JSObject*, (Instance* instance, EncodedJSValue encodedSource, uint32_t layoutID))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue source = JSValue::decode(encodedSource);
    if (!layoutID) {
        if (source.isCell() && source.asCell()->type() == FinalObjectType && source.asCell()->structure()->typedLayoutID() && !TypedLayoutTable::hasTypedFields()) {
            if (JSObject* copy = instance->tryCopySlotsForSpread(asObject(source)))
                OPERATION_RETURN(scope, copy);
        }
        OPERATION_RETURN(scope, cloneObjectForSpread(globalObject, source));
    }
    if (source.isCell() && source.asCell()->type() == FinalObjectType) {
        Structure* structure = source.asCell()->structure();
        if (structure->typedLayoutID() == layoutID && structure->canPerformFastPropertyEnumerationCommon()) {
            if (JSObject* copy = tryCreateObjectViaCloning(vm, globalObject, asObject(source)))
                OPERATION_RETURN(scope, copy);
        }
        if (Options::verboseAOTCompilation()) [[unlikely]]
            dataLogLn("AOT: a copy that is to be of family ", layoutID, " is of what was born as ", structure->typedLayoutID(), " and is made bit by bit");
    }
    OPERATION_RETURN(scope, cloneObjectForSpread(globalObject, source, Instance::newObjectOf(vm, instance->emptyStructureForLayout(safeCast<uint16_t>(layoutID)))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTryCopyDataProperties, size_t, (Instance* instance, EncodedJSValue encodedTarget, EncodedJSValue encodedSource, EncodedJSValue encodedExcludedSetIndex, uint32_t whose))
{
    AOT_OPERATION_BEGIN(instance);
    JSObject* target = asObject(JSValue::decode(encodedTarget));
    JSValue sourceValue = JSValue::decode(encodedSource);
    if (sourceValue.isUndefinedOrNull())
        OPERATION_RETURN(scope, true);
    if (!sourceValue.isObject() || target->type() != FinalObjectType)
        OPERATION_RETURN(scope, false);
    JSObject* source = asObject(sourceValue);
    Structure* targetStructure = target->structure();
    Structure* sourceStructure = source->structure();
    if (targetStructure->isDictionary() || !targetStructure->isStructureExtensible() || targetStructure->hasPolyProto() || (targetStructure->typedLayoutID() && TypedLayoutTable::hasTypedFields()))
        OPERATION_RETURN(scope, false);
    if (sourceStructure->isDictionary() || !sourceStructure->canPerformFastPropertyEnumerationCommon() || hasIndexedProperties(sourceStructure->indexingType()))
        OPERATION_RETURN(scope, false);
    if (sourceStructure->typeInfo().hasStaticPropertyTable() && !sourceStructure->staticPropertiesReified())
        OPERATION_RETURN(scope, false);
    const IdentifierSet* excluded = nullptr;
    if (JSValue index = JSValue::decode(encodedExcludedSetIndex))
        excluded = &callerBytecodeOwner(instance, callFrame, whose).constantIdentifierSet(index.asUInt32AsAnyInt());

    DeferGC deferGC(vm);
    auto& copied = instance->copiedProperties(targetStructure, sourceStructure, excluded);
    Structure* last = copied.last;
    if (!last)
        OPERATION_RETURN(scope, false);
    size_t oldCapacity = targetStructure->outOfLineCapacity();
    size_t newCapacity = last->outOfLineCapacity();
    if (oldCapacity != newCapacity) {
        Butterfly* butterfly = target->allocateMoreOutOfLineStorage(vm, oldCapacity, newCapacity);
        target->nukeStructureAndSetButterfly(vm, targetStructure->id(), butterfly);
    }
    for (auto [from, to] : copied.offsets)
        target->putDirectOffset(vm, to, source->getDirect(from));
    if (last != targetStructure || oldCapacity != newCapacity)
        target->setStructure(vm, last);
    OPERATION_RETURN(scope, true);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewObjectLiteral, JSObject*, (Instance* instance, EncodedJSValue* values, uint32_t count, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    if (StructureID structureID = cache[0].structureID) {
        Structure* structure = structureID.decode();
        if (uint16_t layoutID = structure->typedLayoutID(); layoutID && TypedLayoutTable::hasTypedFields() && structure->outOfLineCapacity()) [[unlikely]] {
            JSObject* object = Instance::newObjectOf(vm, structure);
            for (unsigned i = 0; i < count; ++i) {
                if (values[i])
                    object->putDirectOffset(vm, TypedLayoutTable::offsetInLayout(layoutID, i), JSValue::decode(values[i]));
            }
            OPERATION_RETURN(scope, object);
        }
        JSObject* object = constructEmptyObject(vm, structure);
        for (unsigned i = 0; i < count; ++i)
            object->putDirectOffset(vm, i, JSValue::decode(values[i]));
        OPERATION_RETURN(scope, object);
    }

    FunctionRef function = caller(instance, callFrame);
    Vector<unsigned, 32> identifiers;
    unsigned inlineCapacityInBytecode;
    uint32_t shape = function.siteConstantOf(cache);
    if (Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("AOT: a literal of ", count, " is made by function ", function.index, " as shape ", shape, SharedData::contains(cache) ? " (with nobody's slots)" : "");
    std::span<const uint16_t> slots;
    if (shape)
        slots = function.instance->knownShapeSlots(shape);
    unsigned numberOfSlots = count;
    auto slotOf = [&](unsigned property) -> unsigned { return slots.empty() ? property : slots[property]; };
    if (Structure* structure = shape ? function.instance->knownShapeStructureIfExists(shape) : nullptr; structure && !structure->outOfLineCapacity()) {
        unsigned numberOfProperties = slots.empty() ? count : slots.size();
        countOperationNamed(instance, __func__, "known-shape");
        JSObject* object = constructEmptyObject(vm, structure);
        for (unsigned i = 0; i < numberOfProperties; ++i)
            object->putDirectOffset(vm, slotOf(i), JSValue::decode(values[slotOf(i)]));
        if (!SharedData::contains(cache) && numberOfSlots <= structure->inlineCapacity())
            fillAllocationCache(vm, callerData(instance, callFrame), cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), structure->inlineCapacity());
        OPERATION_RETURN(scope, object);
    }
    if (AllocationPlan plan = function.planOf(cache)) {
        RELEASE_ASSERT(slots.empty() ? plan.count() == count : plan.count() == slots.size());
        count = plan.count();
        for (unsigned i = 0; i < count; ++i)
            identifiers.append(plan.identifier(i));
        inlineCapacityInBytecode = plan.inlineCapacity();
    } else {
        RELEASE_ASSERT(slots.empty());
        UnlinkedCodeBlock* codeBlock = callerCode(instance, callFrame);
        const JSInstruction* instruction = codeBlock->instructions().at(callerBytecodeIndex(instance, callFrame)).ptr();
        inlineCapacityInBytecode = instruction->as<OpNewObject>().m_inlineCapacity;
        auto& instructions = codeBlock->instructions();
        VirtualRegister object = instruction->as<OpNewObject>().m_dst;
        for (unsigned offset = callerBytecodeIndex(instance, callFrame).offset() + instruction->size(); identifiers.size() < count; offset += instructions.at(offset)->size()) {
            auto next = instructions.at(offset);
            if (next->opcodeID() != op_put_by_id)
                continue;
            auto bytecode = next->as<OpPutById>();
            if (bytecode.m_base == object)
                identifiers.append(bytecode.m_property);
        }
    }
    auto identifierOf = [&](unsigned index) -> const Identifier& {
        return identifierAt(instance, callFrame, identifiers[index]);
    };
    ObjectAllocationProfile profile;
    profile.initializeProfile(vm, globalObject, globalObject, globalObject->objectPrototype(), inlineCapacityInBytecode);
    if (count > 1 || !slots.empty()) {
        Vector<UniquedStringImpl*, 32> names;
        for (unsigned i = 0; i < count; ++i)
            names.append(identifierOf(i).impl());
        if (Structure* structure = shape ? function.instance->knownShapeStructure(shape, names.span()) : function.instance->literalStructure(profile.structure(), names.span())) {
            DeferGC deferGC(vm);
            unsigned inlineCapacity = structure->inlineCapacity();
            Butterfly* butterfly = nullptr;
            if (unsigned outOfLineCapacity = structure->outOfLineCapacity()) {
                butterfly = Butterfly::create(vm, nullptr, 0, outOfLineCapacity, false, IndexingHeader(), 0);
                gcSafeZeroMemory(std::bit_cast<EncodedJSValue*>(butterfly->propertyStorage() - outOfLineCapacity), outOfLineCapacity * sizeof(EncodedJSValue));
            }
            JSObject* object = JSFinalObject::createWithButterfly(vm, structure, butterfly);
            for (unsigned i = 0; i < count; ++i)
                object->putDirectOffset(vm, offsetForPropertyNumber(slotOf(i), inlineCapacity), JSValue::decode(values[slotOf(i)]));
            if (numberOfSlots <= inlineCapacity)
                fillAllocationCache(vm, callerData(instance, callFrame), cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(inlineCapacity), AllocatorForMode::EnsureAllocator), inlineCapacity);
            OPERATION_RETURN(scope, object);
        }
    }
    JSObject* object = constructEmptyObject(vm, profile.structure());
    bool inOrder = true;
    for (unsigned index = 0; index < count; ++index) {
        PutPropertySlot slot(object, true, PutPropertySlot::PutById);
        object->putDirect(vm, identifierOf(index), JSValue::decode(values[slotOf(index)]), slot);
        inOrder &= slots.empty() && slot.isCacheablePut() && slot.type() == PutPropertySlot::NewProperty && slot.cachedOffset() == static_cast<PropertyOffset>(index);
    }
    Structure* structure = object->structure();
    if (inOrder && !structure->isDictionary() && count <= structure->inlineCapacity() && !object->butterfly())
        fillAllocationCache(vm, callerData(instance, callFrame), cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), structure->inlineCapacity());
    OPERATION_RETURN(scope, object);
}

static void installTypedLayoutInAllocationProfile(VM& vm, Instance* instance, CallFrame* callFrame, JSFunction* constructor, uint16_t layoutID)
{
    if (!layoutID) [[likely]]
        return;
    FunctionRareData* rareData = constructor->rareData();
    Structure* usual = rareData->objectAllocationStructure();
    if (usual->typedLayoutID() || usual->hasPolyProto()) [[likely]]
        return;
    if (callerBytecodeOwner(instance, callFrame).executableIfExists() != constructor->executable())
        return;
    JSObject* prototype = usual->storedPrototypeObject();
    if (!TypedLayoutTable::tryToInheritFrom(vm, layoutID, prototype))
        return;
    rareData->objectAllocationProfile()->replaceStructure(vm, rareData, instance->emptyStructureForLayout(layoutID, prototype));
}

static JSObject* ensureTypedLayout(JSGlobalObject* globalObject, Instance* instance, JSObject* emptyObject, uint16_t layoutID)
{
    if (!layoutID || emptyObject->structure()->typedLayoutID() == layoutID) [[likely]]
        return emptyObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* prototype = asObject(emptyObject->getPrototypeDirect());
    if (!TypedLayoutTable::tryToInheritFrom(vm, layoutID, prototype)) [[unlikely]] {
        throwTypeError(globalObject, scope, TypedFieldOfInheritorError);
        return nullptr;
    }
    return Instance::newObjectOf(vm, instance->emptyStructureForLayout(layoutID, prototype));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateThisWithProperties, JSObject*, (Instance* instance, JSObject* callee, EncodedJSValue* values, uint32_t count, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    if (auto* function = dynamicDowncast<JSFunction>(callee)) {
        FunctionRareData* rareData = function->rareData();
        Structure* first = rareData ? rareData->objectAllocationStructure() : nullptr;
        StructureID last;
        if (first && !first->typedLayoutID()) {
            if (cache->pointer != function) {
                if (MegamorphicCache* megamorphicCache = vm.megamorphicCache())
                    last = megamorphicCache->lastStructureOfConstruction(first->id(), cache);
            } else if (first->id() == cache[2].structureID)
                last = cache[0].structureID;
        }
        if (last) {
            countOperationNamed(instance, __func__, cache->pointer == function ? "valid-cache" : "valid-megamorphic-entry");
            JSObject* object = constructEmptyObject(vm, last.decode());
            for (unsigned i = 0; i < count; ++i)
                object->putDirectOffset(vm, i, JSValue::decode(values[i]));
            OPERATION_RETURN(scope, object);
        }
    }

    Vector<NewObjectPlan::Property, 8> properties;
    unsigned inlineCapacityInBytecode;
    uint16_t plannedLayoutID = 0;
    if (AllocationPlan plan = caller(instance, callFrame).planOf(cache)) {
        for (unsigned i = 0; i < plan.count(); ++i)
            properties.append({ plan.identifier(i), plan.isDefined(i), plan.isStrict(i), plan.isAssigned(i) });
        inlineCapacityInBytecode = plan.inlineCapacity();
        plannedLayoutID = plan.thisLayoutID();
    } else {
        UnlinkedCodeBlock* codeBlock = callerCode(instance, callFrame);
        unsigned offset = callerBytecodeIndex(instance, callFrame).offset();
        properties = NewObjectPlan::forCreateThis(codeBlock, offset).properties;
        inlineCapacityInBytecode = codeBlock->instructions().at(offset)->as<OpCreateThis>().m_inlineCapacity;
    }
    RELEASE_ASSERT(properties.size() == count);

    JSObject* object = nullptr;
    JSFunction* constructor = dynamicDowncast<JSFunction>(callee);
    bool cacheable = false;
    if (constructor && constructor->canUseAllocationProfiles()) {
        ObjectAllocationProfileWithPrototype* allocationProfile = constructor->ensureRareDataAndObjectAllocationProfile(globalObject, instance->inlineCapacityFor(constructor, inlineCapacityInBytecode))->objectAllocationProfile();
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        installTypedLayoutInAllocationProfile(vm, instance, callFrame, constructor, plannedLayoutID);
        Structure* structure = allocationProfile->structure();
        instance->noteFirstStructure(structure, constructor);
        object = Instance::newObjectOf(vm, structure);
        if (structure->hasPolyProto()) {
            JSObject* prototype = allocationProfile->prototype();
            object->putDirectOffset(vm, knownPolyProtoOffset, prototype);
            prototype->didBecomePrototype(vm);
        } else
            cacheable = true;
    } else {
        JSValue proto = callee->get(globalObject, vm.propertyNames->prototype);
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        if (proto.isObject())
            object = constructEmptyObject(globalObject, asObject(proto));
        else {
            JSGlobalObject* functionGlobalObject = getFunctionRealm(globalObject, callee);
            OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
            object = constructEmptyObject(functionGlobalObject);
        }
    }
    if (JSObject* typedObject = ensureTypedLayout(globalObject, instance, object, plannedLayoutID); typedObject != object) [[unlikely]] {
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        object = typedObject;
        cacheable = false;
    }

    Structure* first = object->structure();
    bool matchesPlannedLayout = !TypedLayoutTable::hasTypedFields() || first->typedLayoutID() == plannedLayoutID;
    bool hasTypedLayout = first->typedLayoutID() && TypedLayoutTable::hasTypedFields();
    for (unsigned i = 0; i < count; ++i) {
        const Identifier& ident = identifierAt(instance, callFrame, properties[i].identifier);
        PutPropertySlot slot(object, properties[i].isStrict, putByIdContextOf(instance, callFrame));
        if (hasTypedLayout && (properties[i].isAssigned || !JSValue::decode(values[i]).isUndefined())) {
            auto* field = TypedLayoutTable::findField(vm, object->structure()->typedLayoutID(), ident.impl());
            if (field && TypedLayoutTable::checkStore(*field, JSValue::decode(values[i])) == TypedLayoutTable::StoreCheck::Rejected) [[unlikely]] {
                throwTypeError(globalObject, scope, TypedLayoutTable::describeRejectedStore(vm, object->structure(), ident.impl(), JSValue::decode(values[i])));
                OPERATION_RETURN(scope, static_cast<JSObject*>(nullptr));
            }
        }
        if (properties[i].isDefined)
            CommonSlowPaths::putDirectWithReify(vm, globalObject, object, ident, JSValue::decode(values[i]), slot);
        else
            object->methodTable()->put(object, globalObject, ident, JSValue::decode(values[i]), slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        bool isNewProperty = slot.isCacheablePut() && slot.type() == PutPropertySlot::NewProperty;
        bool isNewTypedField = hasTypedLayout && slot.isTypedFieldCacheablePut() && slot.type() == PutPropertySlot::NewTypedField;
        matchesPlannedLayout &= (isNewProperty || isNewTypedField) && slot.base() == object && (hasTypedLayout ? isInlineOffset(slot.cachedOffset()) : slot.cachedOffset() == static_cast<PropertyOffset>(i));
    }
    cacheable &= matchesPlannedLayout;

    Structure* last = object->structure();
    if (matchesPlannedLayout && !last->isDictionary() && count <= last->inlineCapacity()) {
        if (uint32_t shape = caller(instance, callFrame).siteConstantOf(cache))
            last->setKnownShape(vm, safeCast<uint16_t>(shape));
    }
    if (!cacheable || last->isDictionary() || count > last->inlineCapacity() || object->butterfly() || first->mayBePrototype() || last->mayBePrototype())
        OPERATION_RETURN(scope, object);

    if (cache->pointer && cache->pointer != constructor && !SharedData::contains(cache)) {
        if (first->propertyAccessesAreCacheable() && canUseMegamorphicPutFastPath(first)) {
            vm.ensureMegamorphicCache().initAsConstruction(first->id(), last->id(), cache);
            callerData(instance, callFrame)->hasSitesInMegamorphicCache = true;
        }
        OPERATION_RETURN(scope, object);
    }

    if (cache->pointer == constructor && cache[0].structureID == last->id() && cache[2].structureID == first->id()) {
        countOperationNamed(instance, __func__, "valid-cache-after-replay");
        OPERATION_RETURN(scope, object);
    }
    if ((cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        OPERATION_RETURN(scope, object);
    cache->offset += 1u << Slot::attemptsShift;

    makePrototypeChainWatchable(vm, object);
    ObjectPropertyConditionSet conditions;
    for (unsigned i = 0; i < count; ++i) {
        if (properties[i].isDefined)
            continue;
        UniquedStringImpl* uid = identifierAt(instance, callFrame, properties[i].identifier).impl();
        auto status = prepareChainForCaching(globalObject, object, uid, nullptr);
        if (!status || status->flattenedDictionary || status->usesPolyProto)
            OPERATION_RETURN(scope, object);
        ObjectPropertyConditionSet forThis = generateConditionsForPropertySetterMiss(vm, globalObject, globalObject, last, uid);
        if (!forThis.isValid())
            OPERATION_RETURN(scope, object);
        conditions = conditions.mergedWith(forThis);
    }
    if (!watchConditions(vm, callerData(instance, callFrame), cache, conditions))
        OPERATION_RETURN(scope, object);
    fillConstructionCache(vm, callerData(instance, callFrame), cache, constructor, first, last, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(last->inlineCapacity()), AllocatorForMode::EnsureAllocator));
    OPERATION_RETURN(scope, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutProperties, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue* values, uint32_t count, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    JSValue base = JSValue::decode(encodedBase);
    AllocationPlan plan = caller(instance, callFrame).planOf(cache);
    RELEASE_ASSERT(plan && plan.count() == count);

    if (base.isCell() && base.asCell()->type() == FinalObjectType) {
        JSObject* object = asObject(base);
        Structure* structure = object->structure();
        if (!structure->isDictionary() && structure->isStructureExtensible() && !(structure->typedLayoutID() && TypedLayoutTable::hasTypedFields()) && !structure->hasPolyProto() && !structure->mayBePrototype()) {
            auto& target = instance->propertyRunTarget(structure, plan.words, [&](Vector<UniquedStringImpl*, 16>& names) {
                for (unsigned i = 0; i < count; ++i)
                    names.append(identifierAt(instance, callFrame, plan.identifier(i)).impl());
            });
            Structure* last = target.last;
            JSValue prototype = structure->storedPrototype();
            for (StructureID expected : target.prototypeStructures) {
                if (!last || !prototype.isObject() || asObject(prototype)->structureID() != expected) {
                    last = nullptr;
                    break;
                }
                prototype = expected.decode()->storedPrototype();
            }
            if (last && prototype.isObject())
                last = nullptr;
            if (last) {
                PropertyOffset offset = structure->maxOffset();
                size_t oldCapacity = structure->outOfLineCapacity();
                size_t newCapacity = last->outOfLineCapacity();
                if (oldCapacity != newCapacity) {
                    Butterfly* butterfly = object->allocateMoreOutOfLineStorage(vm, oldCapacity, newCapacity);
                    object->nukeStructureAndSetButterfly(vm, structure->id(), butterfly);
                }
                for (unsigned i = 0; i < count; ++i) {
                    offset = Instance::offsetAfter(offset, structure->inlineCapacity());
                    object->putDirectOffset(vm, offset, JSValue::decode(values[i]));
                }
                RELEASE_ASSERT(offset == last->maxOffset());
                object->setStructure(vm, last);
                OPERATION_RETURN(scope);
            }
        }
    }

    countOperationNamed(instance, __func__, "one-by-one");
    CallSiteOverride where(*instance, callFrame->callerFrame());
    for (unsigned i = 0; i < count; ++i) {
        where.setItem(i);
        const Identifier& ident = identifierAt(instance, callFrame, plan.identifier(i));
        PutPropertySlot slot(base, plan.isStrict(i), putByIdContextOf(instance, callFrame));
        if (plan.isDefined(i))
            CommonSlowPaths::putDirectWithReify(vm, globalObject, asObject(base), ident, JSValue::decode(values[i]), slot);
        else
            base.putInline(globalObject, ident, JSValue::decode(values[i]), slot);
        OPERATION_RETURN_IF_EXCEPTION(scope);
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTConstructViaCall, UGPRPair, (CallFrame* callFrame))
{
    auto* function = uncheckedDowncast<JSFunction>(callFrame->jsCallee());
    VM& vm = function->vm();
    NativeCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSGlobalObject* globalObject = function->globalObject();
    JSObject* newTarget = asObject(callFrame->newTarget());

    JSObject* thisObject = nullptr;
    if (auto* constructor = dynamicDowncast<JSFunction>(newTarget); constructor && constructor->canUseAllocationProfiles()) {
        ObjectAllocationProfileWithPrototype* allocationProfile = constructor->ensureRareDataAndObjectAllocationProfile(globalObject, 0)->objectAllocationProfile();
        RETURN_IF_EXCEPTION(scope, encodeResult(nullptr, &vm));
        Structure* structure = allocationProfile->structure();
        thisObject = Instance::newObjectOf(vm, structure);
        if (structure->hasPolyProto()) {
            JSObject* prototype = allocationProfile->prototype();
            thisObject->putDirectOffset(vm, knownPolyProtoOffset, prototype);
            prototype->didBecomePrototype(vm);
        }
    } else {
        JSValue proto = newTarget->get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, encodeResult(nullptr, &vm));
        if (proto.isObject())
            thisObject = constructEmptyObject(globalObject, asObject(proto));
        else {
            JSGlobalObject* functionGlobalObject = getFunctionRealm(globalObject, newTarget);
            RETURN_IF_EXCEPTION(scope, encodeResult(nullptr, &vm));
            thisObject = constructEmptyObject(functionGlobalObject);
        }
    }

    MarkedArgumentBuffer arguments;
    arguments.ensureCapacity(callFrame->argumentCount());
    for (unsigned i = 0; i < callFrame->argumentCount(); ++i)
        arguments.append(callFrame->uncheckedArgument(i));
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return encodeResult(nullptr, &vm);
    }
    JSValue result = call(globalObject, function, JSC::getCallData(function), thisObject, arguments);
    RETURN_IF_EXCEPTION(scope, encodeResult(nullptr, &vm));
    return encodeResult(std::bit_cast<void*>(JSValue::encode(result.isObject() ? result : JSValue(thisObject))), nullptr);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateThis, JSObject*, (Instance* instance, JSObject* callee, uint32_t layoutIDAndInlineCapacity))
{
    AOT_OPERATION_BEGIN(instance);
    uint16_t layoutID = layoutIDAndInlineCapacity >> 16;
    unsigned inlineCapacity = layoutIDAndInlineCapacity & 0xffff;
    JSFunction* constructor = dynamicDowncast<JSFunction>(callee);
    if (constructor && constructor->canUseAllocationProfiles()) {
        ObjectAllocationProfileWithPrototype* allocationProfile = constructor->ensureRareDataAndObjectAllocationProfile(globalObject, instance->inlineCapacityFor(constructor, inlineCapacity))->objectAllocationProfile();
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        installTypedLayoutInAllocationProfile(vm, instance, callFrame, constructor, layoutID);
        Structure* structure = allocationProfile->structure();
        instance->noteFirstStructure(structure, constructor);
        JSObject* result = Instance::newObjectOf(vm, structure);
        if (structure->hasPolyProto()) {
            JSObject* prototype = allocationProfile->prototype();
            result->putDirectOffset(vm, knownPolyProtoOffset, prototype);
            prototype->didBecomePrototype(vm);
        }
        OPERATION_RETURN(scope, ensureTypedLayout(globalObject, instance, result, layoutID));
    }

    JSValue proto = callee->get(globalObject, vm.propertyNames->prototype);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
    if (proto.isObject())
        OPERATION_RETURN(scope, ensureTypedLayout(globalObject, instance, constructEmptyObject(globalObject, asObject(proto)), layoutID));
    JSGlobalObject* functionGlobalObject = getFunctionRealm(globalObject, callee);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
    OPERATION_RETURN(scope, ensureTypedLayout(globalObject, instance, constructEmptyObject(functionGlobalObject), layoutID));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArray, JSObject*, (Instance* instance, const EncodedJSValue* values, uint32_t count, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN(instance);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(static_cast<IndexingType>(indexingType));
    OPERATION_RETURN(scope, constructArray(globalObject, structure, std::bit_cast<const JSValue*>(values), count));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayWithSize, JSObject*, (Instance* instance, EncodedJSValue size))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, constructArrayWithSizeQuirk(globalObject, nullptr, JSValue::decode(size)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayBuffer, JSObject*, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance);
    auto* immutableButterfly = uncheckedDowncast<JSCellButterfly>(cell);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(immutableButterfly->indexingMode());
    OPERATION_RETURN(scope, CommonSlowPaths::allocateNewArrayBuffer(vm, structure, immutableButterfly));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayWithSpread, JSObject*, (Instance* instance, EncodedJSValue* encodedValues, uint32_t count, uint32_t pendingSpreads))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue* values = std::bit_cast<JSValue*>(encodedValues);
    auto spreadOf = [](JSValue value) -> JSCellButterfly* {
        return value.isCell() ? dynamicDowncast<JSCellButterfly>(value.asCell()) : nullptr;
    };

    if (pendingSpreads) {
        auto copySourceArray = [&](unsigned i) -> JSArray* {
            return pendingSpreads >> i & 1 ? copyableArray(values[i]) : nullptr;
        };
        bool allSourcesAreCopyableArrays = count > 1;
        for (unsigned i = 0; i < count; ++i)
            allSourcesAreCopyableArrays &= !(pendingSpreads >> i & 1) || copySourceArray(i);
        if (allSourcesAreCopyableArrays) {
            CheckedUint32 size = 0;
            for (unsigned i = 0; i < count; ++i) {
                if (JSArray* array = copySourceArray(i))
                    size += array->length();
                else if (auto* butterfly = spreadOf(values[i]))
                    size += butterfly->publicLength();
                else
                    size += 1;
            }
            if (!size.hasOverflowed() && size.value() < MIN_ARRAY_STORAGE_CONSTRUCTION_LENGTH) {
                if (JSArray* result = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), size.value())) {
                    unsigned index = 0;
                    for (unsigned i = 0; i < count; ++i) {
                        if (JSArray* array = copySourceArray(i)) {
                            for (unsigned j = 0, length = array->length(); j < length; ++j) {
                                JSValue element = array->tryGetIndexQuickly(j);
                                result->putDirectIndex(globalObject, index++, element ? element : jsUndefined());
                                OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
                            }
                        } else if (auto* butterfly = spreadOf(values[i])) {
                            for (unsigned j = 0; j < butterfly->publicLength(); ++j) {
                                result->putDirectIndex(globalObject, index++, butterfly->get(j));
                                OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
                            }
                        } else {
                            result->putDirectIndex(globalObject, index++, values[i]);
                            OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
                        }
                    }
                    OPERATION_RETURN(scope, result);
                }
            }
        }
        for (unsigned i = 0; i < count; ++i) {
            if (!(pendingSpreads >> i & 1))
                continue;
            JSCell* made = spread(globalObject, values[i]);
            OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
            values[i] = made;
        }
    }

    if (count == 1) {
        if (auto* butterfly = spreadOf(values[0])) {
            Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithContiguous);
            if (isCopyOnWrite(structure->indexingMode()))
                OPERATION_RETURN(scope, CommonSlowPaths::allocateNewArrayBuffer(vm, structure, butterfly));
        }
    }

    CheckedUint32 checkedArraySize = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (auto* butterfly = spreadOf(values[i]))
            checkedArraySize += butterfly->publicLength();
        else
            checkedArraySize += 1;
    }
    if (checkedArraySize.hasOverflowed() || checkedArraySize.value() >= MIN_ARRAY_STORAGE_CONSTRUCTION_LENGTH) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, static_cast<JSObject*>(nullptr));
    }

    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    JSArray* result = JSArray::tryCreate(vm, structure, checkedArraySize.value());
    if (!result) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, static_cast<JSObject*>(nullptr));
    }

    unsigned index = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (auto* butterfly = spreadOf(values[i])) {
            for (unsigned j = 0; j < butterfly->publicLength(); ++j) {
                RELEASE_ASSERT(butterfly->get(j));
                result->putDirectIndex(globalObject, index++, butterfly->get(j));
                OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
            }
        } else {
            result->putDirectIndex(globalObject, index++, values[i]);
            OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        }
    }
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayWithSpecies, JSObject*, (Instance* instance, EncodedJSValue encodedLength, JSObject* array))
{
    AOT_OPERATION_BEGIN(instance);
    uint64_t length = truncateDoubleToUint64(JSValue::decode(encodedLength).asNumber());
    std::pair<SpeciesConstructResult, JSObject*> speciesResult = speciesConstructArray(globalObject, array, length);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
    if (speciesResult.first == SpeciesConstructResult::CreatedObject)
        OPERATION_RETURN(scope, speciesResult.second);

    if (length > std::numeric_limits<unsigned>::max()) [[unlikely]] {
        throwException(globalObject, scope, createRangeError(globalObject, ArrayInvalidLengthError));
        OPERATION_RETURN(scope, static_cast<JSObject*>(nullptr));
    }
    OPERATION_RETURN(scope, constructEmptyArray(globalObject, nullptr, static_cast<unsigned>(length)));
}

JSArray* copyableArray(JSValue iterable)
{
    if (!isJSArray(iterable))
        return nullptr;
    auto* array = uncheckedDowncast<JSArray>(iterable.asCell());
    return array->isIteratorProtocolFastAndNonObservable() && !array->hasSparseMap() ? array : nullptr;
}

JSCell* spread(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (iterable.isCell()) {
        auto* result = CommonSlowPaths::trySpreadFast(globalObject, iterable.asCell());
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (result)
            return result;
    }

    JSFunction* iterationFunction = globalObject->iteratorProtocolFunction();
    auto callData = JSC::getCallData(iterationFunction);
    auto arguments = WTF::toArray<EncodedJSValue>({ JSValue::encode(iterable) });
    JSValue arrayResult = call(globalObject, iterationFunction, callData, jsNull(), ArgList { arguments.data(), arguments.size() });
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, JSCellButterfly::createFromArray(globalObject, vm, uncheckedDowncast<JSArray>(arrayResult)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSpread, JSCell*, (Instance* instance, EncodedJSValue encodedIterable))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, spread(globalObject, JSValue::decode(encodedIterable)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewRegExp, JSObject*, (Instance* instance, JSCell* regExp))
{
    AOT_OPERATION_BEGIN(instance);
    static constexpr bool areLegacyFeaturesEnabled = true;
    OPERATION_RETURN(scope, RegExpObject::create(vm, globalObject->regExpStructure(), uncheckedDowncast<RegExp>(regExp), areLegacyFeaturesEnabled));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTValidateNewObject, void, (Instance* instance, JSObject* object))
{
    countOperationNamed(instance, __func__);
    object->evictMistypedFields(*instance->vm);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayIteratorMethod, EncodedJSValue, (Instance* instance, JSCell* array))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue(array).get(globalObject, vm.propertyNames->iteratorSymbol)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLinkTimeConstant, EncodedJSValue, (Instance* instance, uint32_t which))
{
    countOperationNamed(instance, __func__);
    JSGlobalObject* globalObject = instance->globalObject;
    EncodedJSValue result = JSValue::encode(instance->globalObject->linkTimeConstant(static_cast<LinkTimeConstant>(which)));
    instance->linkTimeConstants[which] = result;
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewRegExpForReceiver, JSObject*, (Instance* instance, JSCell* cell, uint32_t forTest, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    RegExp* regExp = uncheckedDowncast<RegExp>(cell);
    if (!Options::useSharedRegExpLiteralObjects() || !RegExpObject::canShareLiteralAsReceiver(globalObject, forTest))
        OPERATION_RETURN(scope, RegExpObject::create(vm, globalObject->regExpStructure(), regExp));
    RegExpObject* object = RegExpObject::createSharedLiteral(vm, globalObject->regExpStructure(), regExp);
    cacheSiteObject(vm, callerData(instance, callFrame), cache, object);
    OPERATION_RETURN(scope, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTIsMadeFromFunction, size_t, (Instance* instance, EncodedJSValue encodedCallee, uint32_t number, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    Data* data = callerData(instance, callFrame);
    JSValue callee = JSValue::decode(encodedCallee);
    auto* function = callee.isCell() ? dynamicDowncast<JSFunction>(callee.asCell()) : nullptr;
    Image* image = Image::withCode();
    auto isMadeFromIt = [&] {
        if (!function || !image)
            return false;
        bool hasWord = function->hasAOTFunctionWord();
        if (!hasWord && (function->isHostFunction() || !function->jsExecutable()->aotEntryFor(CodeSpecializationKind::CodeForCall)))
            return false;
        uint32_t index = hasWord ? function->aotFunctionIndex() : function->jsExecutable()->aotIndexFor(CodeSpecializationKind::CodeForCall);
        return index < image->header().numberOfFunctions && image->at<uint32_t>(image->header().functionNumbersOffset)[index] == number;
    };
    if (isMadeFromIt()) {
        cacheSiteObject(vm, data, cache, function);
        OPERATION_RETURN(scope, true);
    }
    if (!SharedData::contains(cache))
        cache->pointer = std::bit_cast<void*>(static_cast<uintptr_t>(1));
    OPERATION_RETURN(scope, false);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewRegExpForArgument, JSObject*, (Instance* instance, JSCell* cell, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    RegExpObject* object = RegExpObject::create(vm, globalObject->regExpStructure(), uncheckedDowncast<RegExp>(cell));
    if (globalObject->regExpPrimordialPropertiesWatchpointSet().state() == IsWatched)
        cacheSiteObject(vm, callerData(instance, callFrame), cache, object);
    OPERATION_RETURN(scope, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewFunction, JSObject*, (Instance* instance, JSScope* environment, uint32_t index, uint32_t isExpressionAndOwner, uint32_t kind, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    if (StructureID structureID = cache[0].structureID) {
        countOperationNamed(instance, __func__, "valid-cache");
        OPERATION_RETURN(scope, JSFunction::createWithAOTFunctionWord(vm, environment, structureID.decode(), std::bit_cast<uintptr_t>(cache[0].pointer)));
    }
    FunctionRef function = callerBytecodeOwner(instance, callFrame, isExpressionAndOwner >> 1);
    if (auto executableIndex = static_cast<FunctionKind>(kind) == FunctionKind::Normal ? function.nestedExecutableIndex(isExpressionAndOwner & 1, index) : std::nullopt) {
        if (JSFunction* result = instance->tryMakeFunctionWithoutExecutable(*executableIndex, environment)) {
            fillFunctionAllocationCache(vm, callerData(instance, callFrame), cache, result->structure(), subspaceFor<JSFunction>(vm)->allocatorFor(JSFunction::allocationSize(0), AllocatorForMode::EnsureAllocator), *JSFunction::tryEncodeAOTFunctionWord(instance->program->data().executableRow(*executableIndex).entry[0], instance->program->data().executableRow(*executableIndex).index[0]));
            OPERATION_RETURN(scope, result);
        }
    }
    FunctionExecutable* executable = isExpressionAndOwner & 1 ? function.functionExpr(index) : function.functionDecl(index);
    JSFunction* result = nullptr;
    switch (static_cast<FunctionKind>(kind)) {
    case FunctionKind::Normal:
        result = JSFunction::create(vm, globalObject, executable, environment, instance->functionStructure(JSFunction::selectStructureForNewFuncExp(globalObject, executable), executable, environment));
        break;
    case FunctionKind::Generator:
        result = JSGeneratorFunction::create(vm, globalObject, executable, environment, instance->functionStructure(globalObject->generatorFunctionStructure(), executable, environment));
        break;
    case FunctionKind::Async:
        result = JSAsyncFunction::create(vm, globalObject, executable, environment, instance->functionStructure(globalObject->asyncFunctionStructure(), executable, environment));
        break;
    case FunctionKind::AsyncGenerator:
        result = JSAsyncGeneratorFunction::create(vm, globalObject, executable, environment, instance->functionStructure(globalObject->asyncGeneratorFunctionStructure(), executable, environment));
        break;
    }
    if (executable->singletonHasBeenInvalidated())
        fillAllocationCache(vm, callerData(instance, callFrame), cache, result->structure(), subspaceFor<JSFunction>(vm)->allocatorFor(JSFunction::allocationSize(0), AllocatorForMode::EnsureAllocator), 0, executable);
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewFunctionWithCaptures, JSObject*, (Instance* instance, JSScope* environment, uint32_t index, uint32_t isExpressionAndOwner, EncodedJSValue* captures, uint32_t countAndKind, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    uint32_t count = countAndKind & 0xffff;
    FunctionKind kind = static_cast<FunctionKind>(countAndKind >> 16);
    std::span<const EncodedJSValue> values { captures, count };
    if (StructureID structureID = cache[0].structureID; structureID && cache[0].pointer) {
        countOperationNamed(instance, __func__, "valid-cache");
        OPERATION_RETURN(scope, JSFunctionWithCaptures::create(vm, environment, structureID.decode(), std::bit_cast<uintptr_t>(cache[0].pointer), values));
    }
    FunctionRef function = callerBytecodeOwner(instance, callFrame, isExpressionAndOwner >> 1);
    Allocator allocator = vm.heap.cellSpace.allocatorFor(JSFunctionWithCaptures::allocationSize(count), AllocatorForMode::EnsureAllocator);
    if (auto executableIndex = kind == FunctionKind::Normal ? function.nestedExecutableIndex(isExpressionAndOwner & 1, index) : std::nullopt) {
        if (JSFunctionWithCaptures* result = instance->tryMakeFunctionWithoutExecutable(*executableIndex, environment, values)) {
            fillFunctionAllocationCache(vm, callerData(instance, callFrame), cache, result->structure(), allocator, *JSFunction::tryEncodeAOTFunctionWord(instance->program->data().executableRow(*executableIndex).entry[0], instance->program->data().executableRow(*executableIndex).index[0]));
            OPERATION_RETURN(scope, result);
        }
    }
    FunctionExecutable* executable = isExpressionAndOwner & 1 ? function.functionExpr(index) : function.functionDecl(index);
    Structure* structure = instance->functionStructureWithCaptures(kind == FunctionKind::Async ? globalObject->asyncFunctionStructure() : JSFunction::selectStructureForNewFuncExp(globalObject, executable));
    JSFunctionWithCaptures* result = JSFunctionWithCaptures::create(vm, environment, structure, std::bit_cast<uintptr_t>(executable), values);
    fillAllocationCache(vm, callerData(instance, callFrame), cache, structure, allocator, 0, executable);
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasOwnProperty, size_t, (Instance* instance, JSObject* object, EncodedJSValue encodedKey))
{
    AOT_OPERATION_BEGIN(instance);
    auto name = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, object->hasOwnProperty(globalObject, name));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetFunctionName, void, (Instance* instance, JSObject* function, EncodedJSValue name))
{
    AOT_OPERATION_BEGIN(instance);
    uncheckedDowncast<JSFunction>(function)->setFunctionName(globalObject, JSValue::decode(name));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewInternalFieldObject, JSObject*, (Instance* instance, uint32_t kind))
{
    AOT_OPERATION_BEGIN(instance);
    switch (static_cast<InternalFieldObjectKind>(kind)) {
    case InternalFieldObjectKind::Promise:
        OPERATION_RETURN(scope, JSPromise::create(vm, globalObject->promiseStructure()));
    case InternalFieldObjectKind::Generator:
        prepareInlineAllocation<JSGenerator>(instance, Instance::InlineAllocation::Generator, globalObject->generatorStructure());
        OPERATION_RETURN(scope, JSGenerator::create(vm, globalObject->generatorStructure()));
    case InternalFieldObjectKind::AsyncFunctionGenerator:
        prepareInlineAllocation<JSAsyncFunctionGenerator>(instance, Instance::InlineAllocation::AsyncFunctionGenerator, globalObject->asyncFunctionGeneratorStructure());
        OPERATION_RETURN(scope, JSAsyncFunctionGenerator::create(vm, globalObject->asyncFunctionGeneratorStructure()));
    case InternalFieldObjectKind::AsyncGenerator:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewMapOrSet, JSObject*, (Instance* instance, uint32_t isSet))
{
    AOT_OPERATION_BEGIN(instance);
    if (isSet) {
        prepareInlineAllocation<JSSet>(instance, Instance::InlineAllocation::Set, globalObject->setStructure());
        OPERATION_RETURN(scope, JSSet::create(vm, globalObject->setStructure()));
    }
    prepareInlineAllocation<JSMap>(instance, Instance::InlineAllocation::Map, globalObject->mapStructure());
    OPERATION_RETURN(scope, JSMap::create(vm, globalObject->mapStructure()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTAsyncFunctionDrive, void, (Instance* instance, EncodedJSValue encodedResolution, JSCell* generator))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue resolution = JSValue::decode(encodedResolution);
    if (resolution != JSValue(vm.fastAsyncGeneratorSentinel())) {
        if (Options::useAOTOperationCounters()) [[unlikely]]
            countAwait(vm, "await:first-segment", resolution);
        JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, resolution, InternalMicrotask::AsyncFunctionResume, generator);
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateInternalFieldObject, JSObject*, (Instance* instance, JSObject* callee, uint32_t kind))
{
    AOT_OPERATION_BEGIN(instance);
    switch (static_cast<InternalFieldObjectKind>(kind)) {
    case InternalFieldObjectKind::Promise: {
        Structure* structure = JSC_GET_DERIVED_STRUCTURE(vm, promiseStructure, callee, globalObject->promiseConstructor());
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        OPERATION_RETURN(scope, JSPromise::create(vm, structure));
    }
    case InternalFieldObjectKind::Generator: {
        Structure* structure = InternalFunction::createSubclassStructure(globalObject, callee, globalObject->generatorStructure());
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        OPERATION_RETURN(scope, JSGenerator::create(vm, structure));
    }
    case InternalFieldObjectKind::AsyncGenerator: {
        Structure* structure = InternalFunction::createSubclassStructure(globalObject, callee, globalObject->asyncGeneratorStructure());
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        OPERATION_RETURN(scope, JSAsyncGenerator::create(vm, structure));
    }
    case InternalFieldObjectKind::AsyncFunctionGenerator:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateLexicalEnvironment, JSObject*, (Instance* instance, JSScope* currentScope, JSCell* symbolTableCell, EncodedJSValue initialValue, uint32_t))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSLexicalEnvironment::create(vm, globalObject, currentScope, uncheckedDowncast<SymbolTable>(symbolTableCell), JSValue::decode(initialValue)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPushWithScope, JSObject*, (Instance* instance, JSScope* currentScope, EncodedJSValue encodedObject))
{
    AOT_OPERATION_BEGIN(instance);
    JSObject* object = JSValue::decode(encodedObject).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
    OPERATION_RETURN(scope, JSWithScope::create(vm, globalObject, currentScope, object));
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolveScopeForHoistingFuncDeclInEval, EncodedJSValue, (Instance* instance, JSScope* environment, uint32_t identifierIndex))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSScope::resolveScopeForHoistingFuncDeclInEval(globalObject, environment, identifierAt(instance, callFrame, identifierIndex))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateDirectArguments, JSObject*, (Instance* instance, JSObject* callee, uint32_t count, EncodedJSValue* arguments, uint32_t numberOfParameters))
{
    AOT_OPERATION_BEGIN(instance);
    unsigned capacity = std::max(count, numberOfParameters);
    DirectArguments* result = DirectArguments::create(vm, globalObject->directArgumentsStructure(), count, capacity);
    for (unsigned i = count; i--;)
        result->setIndexQuickly(vm, i, JSValue::decode(arguments[i]));
    result->setCallee(vm, uncheckedDowncast<JSFunction>(callee));
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateScopedArguments, JSObject*, (Instance* instance, JSObject* environmentObject, JSObject* callee, uint32_t count, EncodedJSValue* arguments))
{
    AOT_OPERATION_BEGIN(instance);
    auto* environment = uncheckedDowncast<JSLexicalEnvironment>(environmentObject);
    OPERATION_RETURN(scope, ScopedArguments::createByCopyingFrom(vm, globalObject->scopedArgumentsStructure(), std::bit_cast<Register*>(arguments), count, uncheckedDowncast<JSFunction>(callee), environment->symbolTable()->arguments(), environment));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateClonedArguments, JSObject*, (Instance* instance, JSObject* callee, uint32_t count, EncodedJSValue* arguments))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, ClonedArguments::createByCopyingFrom(globalObject, globalObject->clonedArgumentsStructure(), std::bit_cast<Register*>(arguments), count, uncheckedDowncast<JSFunction>(callee), nullptr));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateRest, JSObject*, (Instance* instance, uint32_t count, EncodedJSValue* arguments, uint32_t numParametersToSkip))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, constructArray(globalObject, globalObject->restParameterStructure(), std::bit_cast<JSValue*>(arguments) + numParametersToSkip, count > numParametersToSkip ? count - numParametersToSkip : 0));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowNotAFunction, void, (Instance* instance, EncodedJSValue callee))
{
    AOT_OPERATION_BEGIN(instance);
    throwException(globalObject, scope, createNotAFunctionError(globalObject, JSValue::decode(callee)));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowNotAConstructor, void, (Instance* instance, EncodedJSValue callee))
{
    AOT_OPERATION_BEGIN(instance);
    throwException(globalObject, scope, createNotAConstructorError(globalObject, JSValue::decode(callee)));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTToThis, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(value).toThis(globalObject, isStrict ? ECMAMode::strict() : ECMAMode::sloppy())));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToObject, JSObject*, (Instance* instance, EncodedJSValue encodedValue, uint32_t messageIdentifierIndex))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isUndefinedOrNull()) [[unlikely]] {
        const Identifier& message = identifierAt(instance, callFrame, messageIdentifierIndex);
        if (!message.isEmpty()) {
            throwException(globalObject, scope, createTypeError(globalObject, message.impl()));
            OPERATION_RETURN(scope, static_cast<JSObject*>(nullptr));
        }
    }
    OPERATION_RETURN(scope, value.toObject(globalObject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToPrimitive, EncodedJSValue, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(value).toPrimitive(globalObject)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToPropertyKey, EncodedJSValue, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(value).toPropertyKeyValue(globalObject)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypeof, JSCell*, (Instance* instance, EncodedJSValue value))
{
    countOperationNamed(instance, __func__);
    JSGlobalObject* globalObject = instance->globalObject;
    return jsTypeStringForValue(globalObject, JSValue::decode(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypeofIsObject, size_t, (Instance* instance, EncodedJSValue value))
{
    countOperationNamed(instance, __func__);
    JSGlobalObject* globalObject = instance->globalObject;
    return jsTypeofIsObject(globalObject, JSValue::decode(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypeofIsFunction, size_t, (Instance* instance, EncodedJSValue value))
{
    countOperationNamed(instance, __func__);
    JSGlobalObject* globalObject = instance->globalObject;
    return jsTypeofIsFunction(globalObject, JSValue::decode(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsCallable, size_t, (EncodedJSValue value))
{
    return JSValue::decode(value).isCallable();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsConstructor, size_t, (EncodedJSValue value))
{
    return JSValue::decode(value).isConstructor();
}

JSC_DEFINE_JIT_OPERATION(operationAOTStrcat, EncodedJSValue, (Instance* instance, const EncodedJSValue* values, uint32_t count))
{
    AOT_OPERATION_BEGIN(instance);
    JSRopeString::RopeBuilder<RecordOverflow> ropeBuilder(vm);
    for (unsigned i = 0; i < count; ++i) {
        JSString* string = JSValue::decode(values[i]).toString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!ropeBuilder.append(string)) {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, encodedJSValue());
        }
    }
    OPERATION_RETURN(scope, JSValue::encode(ropeBuilder.release()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetPrototypeOf, EncodedJSValue, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(value).getPrototype(globalObject)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInstanceof, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedConstructor))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue value = JSValue::decode(encodedValue);
    JSValue constructor = JSValue::decode(encodedConstructor);
    if (!constructor.isObject()) {
        throwException(globalObject, scope, createTypeError(globalObject, "Right hand side of instanceof is not an object"_s));
        OPERATION_RETURN(scope, false);
    }

    JSObject* constructorObject = asObject(constructor);
    JSValue hasInstance = constructorObject->get(globalObject, vm.propertyNames->hasInstanceSymbol);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (hasInstance != globalObject->functionProtoHasInstanceSymbolFunction() || !constructorObject->structure()->typeInfo().implementsDefaultHasInstance())
        OPERATION_RETURN(scope, constructorObject->hasInstance(globalObject, value, hasInstance));
    if (!value.isObject())
        OPERATION_RETURN(scope, false);
    JSValue prototype = constructorObject->get(globalObject, vm.propertyNames->prototype);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, JSObject::defaultHasInstance(globalObject, value, prototype));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInstanceofAndCache, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedConstructor, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    JSValue value = JSValue::decode(encodedValue);
    JSValue constructor = JSValue::decode(encodedConstructor);
    if (!constructor.isObject()) {
        throwException(globalObject, scope, createTypeError(globalObject, "Right hand side of instanceof is not an object"_s));
        OPERATION_RETURN(scope, false);
    }

    JSObject* constructorObject = asObject(constructor);
    Structure* structureBefore = constructorObject->structure();
    PropertySlot hasInstanceSlot(constructorObject, PropertySlot::InternalMethodType::Get);
    bool hasHasInstance = constructorObject->getPropertySlot(globalObject, vm.propertyNames->hasInstanceSymbol, hasInstanceSlot);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    JSValue hasInstance = hasHasInstance ? hasInstanceSlot.getValue(globalObject, vm.propertyNames->hasInstanceSymbol) : jsUndefined();
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (hasInstance != globalObject->functionProtoHasInstanceSymbolFunction() || !constructorObject->structure()->typeInfo().implementsDefaultHasInstance())
        OPERATION_RETURN(scope, constructorObject->hasInstance(globalObject, value, hasInstance));
    if (!value.isObject())
        OPERATION_RETURN(scope, false);
    PropertySlot prototypeSlot(constructorObject, PropertySlot::InternalMethodType::Get);
    bool hasPrototype = constructorObject->getPropertySlot(globalObject, vm.propertyNames->prototype, prototypeSlot);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    JSValue prototype = hasPrototype ? prototypeSlot.getValue(globalObject, vm.propertyNames->prototype) : jsUndefined();
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (hasPrototype)
        cacheInstanceOf(globalObject, callerData(instance, callFrame), cache, constructorObject, structureBefore, hasInstanceSlot, prototypeSlot);
    OPERATION_RETURN(scope, JSObject::defaultHasInstance(globalObject, value, prototype));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInstanceofCustom, size_t, (Instance* instance, EncodedJSValue encodedValue, JSObject* constructor, EncodedJSValue encodedHasInstance))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, constructor->hasInstance(globalObject, JSValue::decode(encodedValue), JSValue::decode(encodedHasInstance)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefaultHasInstance, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedPrototype))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSObject::defaultHasInstance(globalObject, JSValue::decode(encodedValue), JSValue::decode(encodedPrototype)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowThisTDZError, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance);
    throwException(globalObject, scope, createReferenceError(globalObject, "'super()' must be called in derived constructor before accessing |this| or returning non-object."_s));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowTDZError, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance);
    if (auto quote = quoteSourceWithoutText(vm, callFrame)) {
        throwException(globalObject, scope, quote->text.isNull() ? createTDZError(globalObject) : createTDZError(globalObject, StringView { quote->text }));
        OPERATION_RETURN(scope);
    }
    auto [block, index] = getBytecodeIndex(vm, callFrame);
    auto info = block->expressionInfoForBytecodeIndex(index);
    RefPtr provider = block->source().provider();
    throwException(globalObject, scope, createTDZError(globalObject, provider->getRange(info.divot - info.startOffset, info.divot + info.endOffset)));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowStaticError, void, (Instance* instance, EncodedJSValue message, uint32_t errorType))
{
    AOT_OPERATION_BEGIN(instance);
    auto errorMessage = asString(JSValue::decode(message))->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    throwException(globalObject, scope, createError(globalObject, static_cast<ErrorTypeWithExtension>(errorType), errorMessage));
    OPERATION_RETURN(scope);
}

static ALWAYS_INLINE const Identifier& wellKnownIdentifier(VM& vm, uint32_t which)
{
    switch (static_cast<WellKnownIdentifier>(which)) {
    case WellKnownIdentifier::Length:
        return vm.propertyNames->length;
    case WellKnownIdentifier::Next:
        return vm.propertyNames->next;
    case WellKnownIdentifier::Done:
        return vm.propertyNames->done;
    case WellKnownIdentifier::Value:
        return vm.propertyNames->value;
    case WellKnownIdentifier::HasInstanceSymbol:
        return vm.propertyNames->hasInstanceSymbol;
    case WellKnownIdentifier::Prototype:
        return vm.propertyNames->prototype;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByIdWellKnown, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t which, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    JSValue base = JSValue::decode(encodedBase);
    if (which == static_cast<unsigned>(WellKnownIdentifier::Length) && base.isCell() && isTypedArrayType(base.asCell()->type()) && instance->typedArrayHasBuiltinLength(base.asCell()))
        OPERATION_RETURN(scope, JSValue::encode(jsNumber(uncheckedDowncast<JSArrayBufferView>(base.asCell())->length())));
    const Identifier& ident = wellKnownIdentifier(vm, which);
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
    Structure* structureBefore = base.isCell() ? base.asCell()->structure() : nullptr;
    JSValue result = base.get(globalObject, ident, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    cacheGetById(globalObject, callerData(instance, callFrame), base, structureBefore, ident, slot, cache);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByIdReallocating, void, (VM* vmPointer, JSObject* base, EncodedJSValue value, const void* entryPointer))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countOperation(__func__);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* entry = static_cast<const MegamorphicCache::StoreEntry*>(entryPointer);
    Structure* oldStructure = WTF::opaque(base->structure());
    Structure* newStructure = WTF::opaque(entry->m_newStructureID.decode());
    PropertyOffset offset = entry->m_offset;
    Butterfly* newButterfly = base->allocateMoreOutOfLineStorage(vm, oldStructure->outOfLineCapacity(), newStructure->outOfLineCapacity());
    base->nukeStructureAndSetButterfly(vm, oldStructure->id(), newButterfly);
    base->putDirectOffset(vm, offset, JSValue::decode(value));
    base->setStructure(vm, newStructure);
    ensureStillAliveHere(oldStructure);
    ensureStillAliveHere(newStructure);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTWriteBarrierAfterPut, void, (VM* vmPointer, JSCell* cell))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    vm.writeBarrierSlowPath(cell);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByIdDirect, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t identifierIndex, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& ident = identifierAt(instance, callFrame, identifierIndex);
    PropertySlot slot(base, PropertySlot::InternalMethodType::GetOwnProperty);
    bool found = base.getOwnPropertySlot(globalObject, ident, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    JSValue result = found ? slot.getValue(globalObject, ident) : jsUndefined();
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());

    if (!SharedData::contains(cache) && found && base.isCell() && slot.isCacheableValue() && slot.slotBase() == base.asCell()) {
        Structure* structure = base.asCell()->structure();
        auto location = propertyLocation(slot.cachedOffset());
        if (structure->propertyAccessesAreCacheable() && !structure->isDictionary() && !structure->needImpurePropertyWatchpoint() && location) {
            cache->offset = *location;
            WTF::storeStoreFence();
            cache->structureID = structure->id();
            didFillSlot(vm, callerData(instance, callFrame));
        }
    }
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByIdWithThis, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue thisValue, uint32_t identifierIndex, Slot* cache))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& ident = identifierAt(instance, callFrame, identifierIndex);
    PropertySlot slot(JSValue::decode(thisValue), PropertySlot::InternalMethodType::Get);
    Structure* structureBefore = base.isCell() ? base.asCell()->structure() : nullptr;
    JSValue result = base.get(globalObject, ident, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (slot.isValue())
        cacheGetById(globalObject, callerData(instance, callFrame), base, structureBefore, ident, slot, cache);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByValWithThis, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue thisValue, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);

    if (base.isCell() && property.isString()) [[likely]] {
        Structure& structure = *base.asCell()->structure();
        if (JSCell::canUseFastGetOwnProperty(structure)) {
            auto existingAtomString = asString(property)->toExistingAtomString(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (existingAtomString) {
                if (JSValue result = base.asCell()->fastGetOwnProperty(vm, structure, existingAtomString.data))
                    OPERATION_RETURN(scope, JSValue::encode(result));
            }
        }
    }

    PropertySlot slot(JSValue::decode(thisValue), PropertySlot::InternalMethodType::Get);
    if (property.isUInt32()) {
        uint32_t i = property.asUInt32();
        if (isJSString(base) && asString(base)->canGetIndex(i))
            OPERATION_RETURN(scope, JSValue::encode(asString(base)->getIndex(globalObject, i)));
        OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, i, slot)));
    }

    base.requireObjectCoercible(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, key, slot)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByIdWithThis, void, (Instance* instance, EncodedJSValue base, EncodedJSValue thisValue, EncodedJSValue value, uint32_t identifierIndex, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance);
    PutPropertySlot slot(JSValue::decode(thisValue), isStrict, putByIdContextOf(instance, callFrame));
    JSValue::decode(base).putInline(globalObject, identifierAt(instance, callFrame, identifierIndex), JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByValWithThis, void, (Instance* instance, EncodedJSValue base, EncodedJSValue thisValue, EncodedJSValue property, EncodedJSValue value, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PutPropertySlot slot(JSValue::decode(thisValue), isStrict);
    JSValue::decode(base).put(globalObject, key, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByValDirect, void, (Instance* instance, JSObject* base, EncodedJSValue encodedProperty, EncodedJSValue encodedValue, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue property = JSValue::decode(encodedProperty);
    JSValue value = JSValue::decode(encodedValue);
    auto mode = isStrict ? PutDirectIndexShouldThrow : PutDirectIndexShouldNotThrow;
    if (std::optional<uint32_t> index = property.tryGetAsUint32Index()) {
        base->putDirectIndex(globalObject, *index, value, 0, mode);
        OPERATION_RETURN(scope);
    }

    auto key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (std::optional<uint32_t> index = parseIndex(key))
        base->putDirectIndex(globalObject, index.value(), value, 0, mode);
    else {
        PutPropertySlot slot(base, isStrict);
        CommonSlowPaths::putDirectWithReify(vm, globalObject, base, key, value, slot);
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTMapSet, EncodedJSValue, (Instance* instance, JSCell* map, EncodedJSValue key, EncodedJSValue value, int32_t hash))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uncheckedDowncast<JSMap>(map)->addNormalized(globalObject, JSValue::decode(key), JSValue::decode(value), hash);
    OPERATION_RETURN(scope, JSValue::encode(map));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetAdd, EncodedJSValue, (Instance* instance, JSCell* set, EncodedJSValue key, int32_t hash))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uncheckedDowncast<JSSet>(set)->addNormalized(globalObject, JSValue::decode(key), JSValue(), hash);
    OPERATION_RETURN(scope, JSValue::encode(set));
}

static bool hasPropertyAndCache(JSGlobalObject* globalObject, JSObject* baseObject, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (parseIndex(*uid) || !vm.megamorphicCache() || uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype || uid == vm.propertyNames->underscoreProto)
        RELEASE_AND_RETURN(scope, baseObject->hasProperty(globalObject, uid));

    PropertySlot slot(baseObject, PropertySlot::InternalMethodType::HasProperty);
    JSObject* object = baseObject;
    bool cacheable = true;
    while (true) {
        if (TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && object->type() != ArrayType && object->type() != JSFunctionType && object != globalObject->arrayPrototype()) [[unlikely]]
            RELEASE_AND_RETURN(scope, object->getNonIndexPropertySlot(globalObject, uid, slot));
        Structure* structure = object->structure();
        bool hasProperty = object->getOwnNonIndexPropertySlot(vm, structure, uid, slot);
        structure = object->structure();
        cacheable &= structure->propertyAccessesAreCacheable();
        if (hasProperty) {
            if (cacheable && slot.isCacheable() && (slot.slotBase() == baseObject || !baseObject->structure()->isDictionary()))
                vm.megamorphicCache()->initAsHasHit(baseObject->structureID(), uid);
            return true;
        }
        cacheable &= structure->propertyAccessesAreCacheableForAbsence() && structure->hasMonoProto();
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject()) {
            if (cacheable && !baseObject->structure()->isDictionary())
                vm.megamorphicCache()->initAsHasMiss(baseObject->structureID(), uid);
            return false;
        }
        object = asObject(prototype);
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTInById, size_t, (Instance* instance, EncodedJSValue encodedBase, uint32_t identifierIndex))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, false);
    }
    OPERATION_RETURN(scope, hasPropertyAndCache(globalObject, asObject(base), identifierAt(instance, callFrame, identifierIndex).impl()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInByVal, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    if (base.isObject()) {
        UniquedStringImpl* uid = nullptr;
        if (property.isSymbol())
            uid = &asSymbol(property)->privateName().uid();
        else if (property.isString()) {
            if (const StringImpl* impl = asString(property)->tryGetValueImpl(); impl && impl->isAtom())
                uid = static_cast<UniquedStringImpl*>(const_cast<StringImpl*>(impl));
        }
        if (uid)
            OPERATION_RETURN(scope, hasPropertyAndCache(globalObject, asObject(base), uid));
    }
    OPERATION_RETURN(scope, CommonSlowPaths::opInByVal(globalObject, base, property));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDelById, size_t, (Instance* instance, EncodedJSValue base, uint32_t identifierIndex, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    bool couldDelete = JSCell::deleteProperty(object, globalObject, identifierAt(instance, callFrame, identifierIndex));
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (!couldDelete && isStrict)
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
    OPERATION_RETURN(scope, couldDelete);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDelByVal, size_t, (Instance* instance, EncodedJSValue base, EncodedJSValue encodedProperty, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    JSValue property = JSValue::decode(encodedProperty);

    bool couldDelete;
    uint32_t i;
    if (property.getUInt32(i))
        couldDelete = object->methodTable()->deletePropertyByIndex(object, globalObject, i);
    else {
        auto key = property.toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, false);
        couldDelete = JSCell::deleteProperty(object, globalObject, key);
    }
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (!couldDelete && isStrict)
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
    OPERATION_RETURN(scope, couldDelete);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetPrivateName, EncodedJSValue, (Instance* instance, EncodedJSValue base, EncodedJSValue property, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    object->getPrivateField(globalObject, key, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (JSValue::decode(base) == object && slot.isCacheableValue() && slot.slotBase() == object)
        cachePrivateName(vm, callerData(instance, callFrame), cache, object, JSValue::decode(property), slot.cachedOffset());
    OPERATION_RETURN(scope, JSValue::encode(slot.getValue(globalObject, key)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutPrivateName, void, (Instance* instance, EncodedJSValue base, EncodedJSValue property, EncodedJSValue value, uint32_t, Slot* cache, uint32_t isDefine))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PutPropertySlot slot(object, true);
    if (isDefine) {
        Structure* structureBefore = object->structure();
        object->definePrivateField(globalObject, key, JSValue::decode(value), slot);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (JSValue::decode(base) == object && slot.isCacheablePut() && slot.type() == PutPropertySlot::NewProperty && slot.base() == object)
            cachePrivateNameTransition(vm, callerData(instance, callFrame), cache, object, structureBefore, JSValue::decode(property), slot.cachedOffset());
    } else {
        Structure* structureBefore = object->structure();
        object->setPrivateField(globalObject, key, JSValue::decode(value), slot);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (JSValue::decode(base) == object && slot.isCacheablePut() && slot.type() == PutPropertySlot::ExistingProperty && slot.base() == object && object->structure() == structureBefore) {
            structureBefore->didCachePropertyReplacement(vm, slot.cachedOffset());
            cachePrivateName(vm, callerData(instance, callFrame), cache, object, JSValue::decode(property), slot.cachedOffset());
        }
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasPrivateName, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue property))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, false);
    }
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, asObject(base)->hasPrivateField(globalObject, key));
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasPrivateBrand, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue brand))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, false);
    }
    OPERATION_RETURN(scope, asObject(base)->hasPrivateBrand(globalObject, JSValue::decode(brand)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCheckPrivateBrand, void, (Instance* instance, EncodedJSValue base, EncodedJSValue brand, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    object->checkPrivateBrand(globalObject, JSValue::decode(brand));
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (JSValue::decode(base) == object)
        cachePrivateName(vm, callerData(instance, callFrame), cache, object, JSValue::decode(brand), std::nullopt);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetPrivateBrand, void, (Instance* instance, JSObject* base, EncodedJSValue brand, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN(instance);
    countOperationBySlotState(instance, __func__, cache);
    Structure* structureBefore = base->structure();
    base->setPrivateBrand(globalObject, JSValue::decode(brand));
    OPERATION_RETURN_IF_EXCEPTION(scope);
    cachePrivateNameTransition(vm, callerData(instance, callFrame), cache, base, structureBefore, JSValue::decode(brand), std::nullopt);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutAccessorById, void, (Instance* instance, JSObject* base, uint32_t identifierIndex, uint32_t attributes, JSObject* accessor, uint32_t isSetter))
{
    AOT_OPERATION_BEGIN(instance);
    if (isSetter)
        base->putSetter(globalObject, identifierAt(instance, callFrame, identifierIndex), accessor, attributes);
    else
        base->putGetter(globalObject, identifierAt(instance, callFrame, identifierIndex), accessor, attributes);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutGetterSetterById, void, (Instance* instance, JSObject* base, uint32_t identifierIndex, uint32_t attributes, EncodedJSValue getter, EncodedJSValue setter))
{
    AOT_OPERATION_BEGIN(instance);
    GetterSetter* accessor = GetterSetter::create(vm, globalObject, JSValue::decode(getter), JSValue::decode(setter));
    CommonSlowPaths::putDirectAccessorWithReify(vm, globalObject, base, identifierAt(instance, callFrame, identifierIndex), accessor, attributes);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutAccessorByVal, void, (Instance* instance, JSObject* base, EncodedJSValue property, uint32_t attributes, JSObject* accessor, uint32_t isSetter))
{
    AOT_OPERATION_BEGIN(instance);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (isSetter)
        base->putSetter(globalObject, key, accessor, attributes);
    else
        base->putGetter(globalObject, key, accessor, attributes);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineDataProperty, void, (Instance* instance, JSObject* base, EncodedJSValue property, EncodedJSValue value, int32_t attributes))
{
    AOT_OPERATION_BEGIN(instance);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PropertyDescriptor descriptor = toPropertyDescriptor(JSValue::decode(value), jsUndefined(), jsUndefined(), DefinePropertyAttributes(attributes));
    base->methodTable()->defineOwnProperty(base, globalObject, key, descriptor, true);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineDataPropertyOnSingleton, void, (Instance* instance, JSObject* base, EncodedJSValue property, EncodedJSValue value, int32_t attributes))
{
    AOT_OPERATION_BEGIN(instance);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (Structure* structure = base->structure(); base->type() == FinalObjectType && !structure->isDictionary() && !structure->hasBeenFlattenedBefore())
        base->convertToDictionary(vm);
    PropertyDescriptor descriptor = toPropertyDescriptor(JSValue::decode(value), jsUndefined(), jsUndefined(), DefinePropertyAttributes(attributes));
    base->methodTable()->defineOwnProperty(base, globalObject, key, descriptor, true);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineAccessorProperty, void, (Instance* instance, JSObject* base, EncodedJSValue property, EncodedJSValue getter, EncodedJSValue setter, int32_t attributes))
{
    AOT_OPERATION_BEGIN(instance);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PropertyDescriptor descriptor = toPropertyDescriptor(jsUndefined(), JSValue::decode(getter), JSValue::decode(setter), DefinePropertyAttributes(attributes));
    base->methodTable()->defineOwnProperty(base, globalObject, key, descriptor, true);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetPropertyEnumerator, JSCell*, (Instance* instance, EncodedJSValue encodedBase))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    if (base.isUndefinedOrNull())
        OPERATION_RETURN(scope, static_cast<JSCell*>(vm.emptyPropertyNameEnumerator()));
    JSObject* object = base.toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSCell*>(nullptr));
    OPERATION_RETURN(scope, static_cast<JSCell*>(propertyNameEnumerator(globalObject, object)));
}

static ALWAYS_INLINE uint32_t enumeratorIndex(EncodedJSValue index)
{
    JSValue value = JSValue::decode(index);
    return value.isInt32() ? value.asUInt32() : static_cast<uint32_t>(value.asNumber());
}

static ALWAYS_INLINE JSPropertyNameEnumerator::Flag enumeratorMode(EncodedJSValue mode)
{
    return static_cast<JSPropertyNameEnumerator::Flag>(JSValue::decode(mode).asUInt32());
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorNext, JSCell*, (Instance* instance, EncodedJSValue base, JSCell* enumerator, EncodedJSValue* modeAndIndex))
{
    AOT_OPERATION_BEGIN(instance);
    auto mode = enumeratorMode(modeAndIndex[0]);
    uint32_t index = enumeratorIndex(modeAndIndex[1]);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSCell*>(nullptr));
    JSString* name = uncheckedDowncast<JSPropertyNameEnumerator>(enumerator)->computeNext(globalObject, object, index, mode);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSCell*>(nullptr));
    modeAndIndex[0] = JSValue::encode(jsNumber(static_cast<uint8_t>(mode)));
    modeAndIndex[1] = JSValue::encode(jsNumber(index));
    OPERATION_RETURN(scope, static_cast<JSCell*>(name ? name : vm.smallStrings.sentinelString()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorGetByVal, EncodedJSValue, (Instance* instance, EncodedJSValue base, EncodedJSValue propertyName, EncodedJSValue index, EncodedJSValue mode, JSCell* enumerator))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(CommonSlowPaths::opEnumeratorGetByVal(globalObject, JSValue::decode(base), JSValue::decode(propertyName), enumeratorIndex(index), enumeratorMode(mode), uncheckedDowncast<JSPropertyNameEnumerator>(enumerator))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorInByVal, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue propertyName, EncodedJSValue index, EncodedJSValue encodedMode, JSCell* enumerator))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    auto mode = enumeratorMode(encodedMode);
    if (auto* object = base.getObject()) {
        if (mode == JSPropertyNameEnumerator::OwnStructureMode && object->structureID() == uncheckedDowncast<JSPropertyNameEnumerator>(enumerator)->cachedStructureID())
            OPERATION_RETURN(scope, true);
        if (mode == JSPropertyNameEnumerator::IndexedMode)
            OPERATION_RETURN(scope, object->hasProperty(globalObject, enumeratorIndex(index)));
    }
    OPERATION_RETURN(scope, CommonSlowPaths::opInByVal(globalObject, base, JSValue::decode(propertyName)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorPutByVal, void, (Instance* instance, EncodedJSValue base, EncodedJSValue propertyName, EncodedJSValue value, EncodedJSValue index, EncodedJSValue mode, JSCell* enumerator, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance);
    CommonSlowPaths::opEnumeratorPutByVal(globalObject, JSValue::decode(base), JSValue::decode(propertyName), JSValue::decode(value), isStrict ? ECMAMode::strict() : ECMAMode::sloppy(), enumeratorIndex(index), enumeratorMode(mode), uncheckedDowncast<JSPropertyNameEnumerator>(enumerator));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorHasOwnProperty, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue propertyName, EncodedJSValue index, EncodedJSValue encodedMode, JSCell* enumerator))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    auto mode = enumeratorMode(encodedMode);
    if (auto* object = base.getObject()) {
        if (mode == JSPropertyNameEnumerator::OwnStructureMode && object->structureID() == uncheckedDowncast<JSPropertyNameEnumerator>(enumerator)->cachedStructureID())
            OPERATION_RETURN(scope, true);
        if (mode == JSPropertyNameEnumerator::IndexedMode)
            OPERATION_RETURN(scope, object->hasOwnProperty(globalObject, enumeratorIndex(index)));
    }
    auto key = asString(JSValue::decode(propertyName))->toIdentifier(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    JSObject* object = base.toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, objectPrototypeHasOwnProperty(globalObject, object, key));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
