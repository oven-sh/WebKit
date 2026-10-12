/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperationsObjects.h"

#include "ObjectAllocationProfileInlines.h"
#include "ObjectConstructorInlines.h"

#if ENABLE(AOT)

#include "AOTInlineCaches.h"
#include "AOTOperationHelpers.h"
#include "AOTTypedOperations.h"
#include "ArrayConstructor.h"
#include "ArrayPrototypeInlines.h"
#include "ClonedArguments.h"
#include "CommonSlowPathsInlines.h"
#include "DefinePropertyAttributes.h"
#include "DirectArguments.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "GetterSetter.h"
#include "HasOwnPropertyCache.h"
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

struct NewObjectPlan {
    struct Property {
        unsigned identifier;
        bool isDefined;
        bool isStrict;
        bool isAssigned;
    };
    Vector<Property, 8> properties;
    struct Store {
        unsigned offset;
        unsigned property;
    };
    Vector<Store, 8> stores;

    static VirtualRegister registerOf(unsigned property) { return VirtualRegister(0x20000000 + static_cast<int>(property)); }

    static NewObjectPlan forCreateThis(UnlinkedCodeBlock*, unsigned offsetOfCreateThis);
};

JSC_DEFINE_JIT_OPERATION(operationAOTNewObject, JSObject*, (Instance* instance, uint32_t inlineCapacity, Slot* cache))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    countOperationBySlotState(instance, __func__, cache);
    if (StructureID structureID = cache[0].structureID)
        OPERATION_RETURN(scope, constructEmptyObject(vm, structureID.decode()));

    ObjectAllocationProfile profile;
    profile.initializeProfile(vm, globalObject, globalObject, globalObject->objectPrototype(), inlineCapacity);
    Structure* structure = profile.structure();
    fillAllocationCache(vm, callerData(instance, callFrame), cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), structure->inlineCapacity());
    OPERATION_RETURN(scope, constructEmptyObject(vm, structure));
}

bool tryCopyDataProperties(Instance* instance, VM& vm, JSObject* target, JSObject* source, const IdentifierSet* excluded, const char* operation)
{
    Structure* targetStructure = target->structure();
    Structure* sourceStructure = source->structure();
    if (targetStructure->hasImmutableShape())
        return false;
    if (targetStructure->isDictionary() || !targetStructure->isStructureExtensible() || targetStructure->hasPolyProto()) {
        countOperationNamed(instance, operation, targetStructure->isDictionary() ? "target-is-dictionary" : !targetStructure->isStructureExtensible() ? "target-is-not-extensible" : "target-has-poly-proto");
        return false;
    }
    if (sourceStructure->isDictionary() || !sourceStructure->canPerformFastPropertyEnumerationCommon() || hasIndexedProperties(sourceStructure->indexingType())) {
        countOperationNamed(instance, operation, sourceStructure->isDictionary() ? "source-is-dictionary" : hasIndexedProperties(sourceStructure->indexingType()) ? "source-has-indexed-properties" : "source-has-no-fast-enumeration");
        return false;
    }
    if (sourceStructure->typeInfo().hasStaticPropertyTable() && !sourceStructure->staticPropertiesReified()) {
        countOperationNamed(instance, operation, "source-has-unreified-static-properties");
        return false;
    }

    DeferGC deferGC(vm);
    auto& copied = instance->copiedProperties(targetStructure, sourceStructure, excluded);
    Structure* last = copied.last;
    if (!last) {
        countOperationNamed(instance, operation, "no-plan");
        return false;
    }
    Vector<JSValue, 16> values;
    for (unsigned i = 0; i < copied.fieldsToCheck.size(); ++i) {
        auto [from, to] = copied.offsets[i];
        JSValue value = source->getDirect(from);
        if (auto* field = copied.fieldsToCheck[i]) {
            bool isRejected = TypedLayoutTable::checkStore(*field, value) == TypedLayoutTable::StoreCheck::Rejected;
            if (!isRejected) {
                value = TypedLayoutTable::toFieldRepresentation(*field, value);
                isRejected = isInlineOffset(to) && TypedLayoutTable::checkStore(last->typedLayoutID(), to, value) == TypedLayoutTable::StoreCheck::Rejected;
            }
            if (isRejected) {
                countOperationNamed(instance, operation, "value-is-rejected");
                return false;
            }
        }
        values.append(value);
    }
    countOperationNamed(instance, operation, "copied");
    size_t oldCapacity = targetStructure->outOfLineCapacity();
    size_t newCapacity = last->outOfLineCapacity();
    if (oldCapacity != newCapacity) {
        Butterfly* butterfly = target->allocateMoreOutOfLineStorage(vm, oldCapacity, newCapacity);
        target->nukeStructureAndSetButterfly(vm, targetStructure->id(), butterfly);
    }
    for (unsigned i = 0; i < copied.offsets.size(); ++i)
        target->putDirectOffset(vm, copied.offsets[i].second, values.isEmpty() ? source->getDirect(copied.offsets[i].first) : values[i]);
    if (last != targetStructure || oldCapacity != newCapacity)
        target->setStructure(vm, last);
    return true;
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
        countOperationNamed(instance, __func__, "untyped");
        OPERATION_RETURN(scope, cloneObjectForSpread(globalObject, source));
    }
    if (source.isCell() && source.asCell()->type() == FinalObjectType) {
        Structure* structure = source.asCell()->structure();
        if (structure->typedLayoutID() == layoutID && structure->canPerformFastPropertyEnumerationCommon()) {
            if (JSObject* copy = tryCreateObjectViaCloning(vm, globalObject, asObject(source))) {
                countOperationNamed(instance, __func__, "same-layout");
                OPERATION_RETURN(scope, copy);
            }
        }
        if (Options::verboseAOTCompilation()) [[unlikely]]
            dataLogLn("AOT: a copy that is to be of family ", layoutID, " is of what was born as ", structure->typedLayoutID(), " and is made bit by bit");
    }
    JSObject* copy = Instance::newObjectOf(vm, instance->emptyStructureForLayout(safeCast<uint16_t>(layoutID)));
    if (source.isCell() && source.asCell()->type() == FinalObjectType && tryCopyDataProperties(instance, vm, copy, asObject(source), nullptr, __func__))
        OPERATION_RETURN(scope, copy);
    OPERATION_RETURN(scope, cloneObjectForSpread(globalObject, source, copy));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewObjectOfRow, JSObject*, (Instance* instance, EncodedJSValue* values, uint32_t count, Slot* row))
{
    AOT_OPERATION_BEGIN_AT(instance, row);
    if (StructureID structureID = row->structureID) {
        JSObject* object = constructEmptyObject(vm, structureID.decode());
        for (unsigned i = 0; i < count; ++i)
            object->putDirectOffset(vm, i, JSValue::decode(values[i]));
        OPERATION_RETURN(scope, object);
    }
    OPERATION_RETURN(scope, newObjectOfRow(instance, globalObject, vm, row - instance->rows, values, count));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewObjectLiteral, JSObject*, (Instance* instance, EncodedJSValue* values, uint32_t count, Slot* cache))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
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
    if (function.info().isTyped)
        OPERATION_RETURN(scope, newObjectLiteralAtSite(instance, globalObject, vm, function, cache, values, count));
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
    if (Structure* structure = shape ? function.instance->knownShapeStructureIfExists(shape) : nullptr; structure && structure->outOfLineCapacity() && structure->typedLayoutID() && TypedLayoutTable::hasTypedFields()) {
        countOperationNamed(instance, __func__, "known-shape-with-storage-outside");
        JSObject* object = Instance::newObjectOf(vm, structure);
        for (unsigned i = 0; i < numberOfSlots; ++i) {
            if (values[i])
                object->putDirectOffset(vm, TypedLayoutTable::offsetInLayout(structure->typedLayoutID(), i), JSValue::decode(values[i]));
        }
        OPERATION_RETURN(scope, object);
    }
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
                object->putDirectOffset(vm, structure->typedLayoutID() ? structure->get(vm, names[i]) : offsetForPropertyNumber(slotOf(i), inlineCapacity), JSValue::decode(values[slotOf(i)]));
            if (numberOfSlots <= inlineCapacity && !structure->outOfLineCapacity())
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
    if (callerBytecodeOwner(instance, callFrame).executableIfExists() != constructor->executable())
        return;
    instance->noteConstructorOfLayout(layoutID, constructor);
    FunctionRareData* rareData = constructor->rareData();
    Structure* usual = rareData->objectAllocationStructure();
    if (usual->typedLayoutID() || usual->hasPolyProto()) [[likely]]
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
    AOT_OPERATION_BEGIN_AT(instance, cache);
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
    uint16_t family = 0;
    if (AllocationPlan plan = caller(instance, callFrame).planOf(cache)) {
        for (unsigned i = 0; i < plan.count(); ++i)
            properties.append({ plan.identifier(i), plan.isDefined(i), plan.isStrict(i), plan.isAssigned(i) });
        inlineCapacityInBytecode = plan.inlineCapacity();
        plannedLayoutID = plan.thisLayoutID();
        family = plan.thisFamily();
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
        PutPropertySlot slot(object, properties[i].isStrict, PutPropertySlot::PutById);
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
        if (family) {
            Vector<UniquedStringImpl*, 16> names;
            for (unsigned i = 0; i < count; ++i)
                names.append(identifierAt(instance, callFrame, properties[i].identifier).impl());
            Instance::giveFamily(vm, last, family, names.span(), "construction");
        }
    }
    if (!cacheable || last->isDictionary() || count > last->inlineCapacity() || object->butterfly() || first->mayBePrototype() || last->mayBePrototype())
        OPERATION_RETURN(scope, object);

    if (cache->pointer && cache->pointer != constructor && !SharedData::contains(cache)) {
        if (first->propertyAccessesAreCacheable() && canUseMegamorphicPutFastPath(first)) {
            first->assertHandsFamilyOnTo(*last);
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
    Vector<UniquedStringImpl*, 16> stored;
    for (unsigned i = 0; i < count; ++i) {
        if (!properties[i].isDefined)
            stored.append(identifierAt(instance, callFrame, properties[i].identifier).impl());
    }
    if (!watchStores(globalObject, callerData(instance, callFrame), cache, object, last, stored.span()))
        OPERATION_RETURN(scope, object);
    fillConstructionCache(vm, callerData(instance, callFrame), cache, constructor, first, last, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(last->inlineCapacity()), AllocatorForMode::EnsureAllocator));
    OPERATION_RETURN(scope, object);
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
        } else {
            for (PropertyOffset offset = 0; offset <= structure->maxOffset(); ++offset)
                result->putDirectOffset(vm, offset, jsUndefined());
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

JSC_DEFINE_JIT_OPERATION(operationAOTNewFunction, JSObject*, (Instance* instance, JSScope* environment, uint32_t index, uint32_t isExpressionAndOwner, uint32_t kind, Slot* cache))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    countOperationBySlotState(instance, __func__, cache);
    if (StructureID structureID = cache[0].structureID) {
        countOperationNamed(instance, __func__, "valid-cache");
        OPERATION_RETURN(scope, JSFunction::createWithAOTFunctionWord(vm, environment, structureID.decode(), std::bit_cast<uintptr_t>(cache[0].pointer)));
    }
    RELEASE_ASSERT(!(isExpressionAndOwner >> 1));
    FunctionRef function = caller(instance, callFrame);
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

JSC_DEFINE_JIT_OPERATION(operationAOTCreateLexicalEnvironment, JSObject*, (Instance* instance, JSScope* currentScope, JSCell* symbolTableCell, EncodedJSValue initialValue, uint32_t))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSLexicalEnvironment::create(vm, globalObject, currentScope, uncheckedDowncast<SymbolTable>(symbolTableCell), JSValue::decode(initialValue)));
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

JSC_DEFINE_JIT_OPERATION(operationAOTToThis, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(value).toThis(globalObject, isStrict ? ECMAMode::strict() : ECMAMode::sloppy())));
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

static NEVER_INLINE void cacheInstanceOfWhereLeftSideIsNoObject(Instance* instance, CallFrame* callFrame, JSGlobalObject* globalObject, Slot* cache, JSObject* constructor, Structure* structureBefore, const PropertySlot& hasInstance)
{
    if (SharedData::contains(cache) || (cache->offset & Slot::attemptsMask) == Slot::attemptsMask) {
        countOperationNamed(instance, "operationAOTInstanceofAndCache", "left-side-is-no-object:the-slot-takes-nothing");
        return;
    }
    VM& vm = globalObject->vm();
    unsigned attributes;
    PropertyOffset offset = constructor->structure()->get(vm, vm.propertyNames->prototype, attributes);
    bool isPlainValue = isValidOffset(offset) && !(attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessorOrValue));
    countOperationNamed(instance, "operationAOTInstanceofAndCache", isPlainValue ? "left-side-is-no-object" : "left-side-is-no-object:no-prototype-in-the-structure");
    if (!isPlainValue)
        return;
    PropertySlot prototype(constructor, PropertySlot::InternalMethodType::VMInquiry, &vm);
    prototype.setValue(constructor, attributes, constructor->getDirect(offset), offset);
    cacheInstanceOf(globalObject, callerData(instance, callFrame), cache, constructor, structureBefore, hasInstance, prototype);
}

JSC_DEFINE_JIT_OPERATION(operationAOTInstanceofAndCache, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedConstructor, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
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
    if (!value.isObject()) {
        cacheInstanceOfWhereLeftSideIsNoObject(instance, callFrame, globalObject, cache, constructorObject, structureBefore, hasInstanceSlot);
        OPERATION_RETURN(scope, false);
    }
    PropertySlot prototypeSlot(constructorObject, PropertySlot::InternalMethodType::Get);
    bool hasPrototype = constructorObject->getPropertySlot(globalObject, vm.propertyNames->prototype, prototypeSlot);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    JSValue prototype = hasPrototype ? prototypeSlot.getValue(globalObject, vm.propertyNames->prototype) : jsUndefined();
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (hasPrototype)
        cacheInstanceOf(globalObject, callerData(instance, callFrame), cache, constructorObject, structureBefore, hasInstanceSlot, prototypeSlot);
    OPERATION_RETURN(scope, JSObject::defaultHasInstance(globalObject, value, prototype));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefaultHasInstance, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedPrototype))
{
    AOT_OPERATION_BEGIN(instance);
    OPERATION_RETURN(scope, JSObject::defaultHasInstance(globalObject, JSValue::decode(encodedValue), JSValue::decode(encodedPrototype)));
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
    AOT_OPERATION_BEGIN_AT(instance, cache);
    countOperationBySlotState(instance, __func__, cache);
    JSValue base = JSValue::decode(encodedBase);
    if (which == static_cast<unsigned>(WellKnownIdentifier::Length) && base.isCell() && isTypedArrayType(base.asCell()->type()) && instance->typedArrayHasBuiltinLength(base.asCell()))
        OPERATION_RETURN(scope, JSValue::encode(jsNumber(uncheckedDowncast<JSArrayBufferView>(base.asCell())->length())));
    const Identifier& ident = wellKnownIdentifier(vm, which);
    if (JSValue value = instance->typed && base.isCell() ? Typed::Object::tryGetOwnData(vm, base.asCell(), ident) : JSValue()) {
        cacheCompiledField(vm, callerData(instance, callFrame), cache, base.asCell(), ident, false);
        OPERATION_RETURN(scope, JSValue::encode(value));
    }
    if (instance->typed && base.isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, Typed::State::nullishMessage(base, ident.string(), false));
        OPERATION_RETURN(scope, encodedJSValue());
    }
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

JSC_DEFINE_JIT_OPERATION(operationAOTInById, size_t, (Instance* instance, EncodedJSValue encodedBase, uint32_t identifierIndex))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, false);
    }
    noteNamedAccess(instance, __func__, base, { }, identifierAt(instance, callFrame, identifierIndex).impl());
    bool goesByStructure;
    OPERATION_RETURN(scope, hasPropertyAndFillMegamorphicCache(globalObject, asObject(base), identifierAt(instance, callFrame, identifierIndex).impl(), goesByStructure));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInByVal, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    noteNamedAccess(instance, __func__, base, property);
    if (base.isObject()) {
        UniquedStringImpl* uid = nullptr;
        if (property.isSymbol())
            uid = &asSymbol(property)->privateName().uid();
        else if (property.isString()) {
            if (const StringImpl* impl = asString(property)->tryGetValueImpl(); impl && impl->isAtom())
                uid = static_cast<UniquedStringImpl*>(const_cast<StringImpl*>(impl));
        }
        bool goesByStructure;
        if (uid)
            OPERATION_RETURN(scope, hasPropertyAndFillMegamorphicCache(globalObject, asObject(base), uid, goesByStructure));
    }
    OPERATION_RETURN(scope, CommonSlowPaths::opInByVal(globalObject, base, property));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetPrivateName, EncodedJSValue, (Instance* instance, EncodedJSValue base, EncodedJSValue property, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
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
    AOT_OPERATION_BEGIN_AT(instance, cache);
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

JSC_DEFINE_JIT_OPERATION(operationAOTCheckPrivateBrand, void, (Instance* instance, EncodedJSValue base, EncodedJSValue brand, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
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
    AOT_OPERATION_BEGIN_AT(instance, cache);
    countOperationBySlotState(instance, __func__, cache);
    Structure* structureBefore = base->structure();
    base->setPrivateBrand(globalObject, JSValue::decode(brand));
    OPERATION_RETURN_IF_EXCEPTION(scope);
    cachePrivateNameTransition(vm, callerData(instance, callFrame), cache, base, structureBefore, JSValue::decode(brand), std::nullopt);
    OPERATION_RETURN(scope);
}

NewObjectPlan NewObjectPlan::forCreateThis(UnlinkedCodeBlock* codeBlock, unsigned offsetOfCreateThis)
{
    NewObjectPlan plan;
    auto& instructions = codeBlock->instructions();
    auto* handler = codeBlock->handlerForBytecodeIndex(BytecodeIndex(offsetOfCreateThis));
    auto createThis = instructions.at(offsetOfCreateThis);
    Vector<VirtualRegister, 4> aliases;
    aliases.append(createThis->as<OpCreateThis>().m_dst);
    constexpr unsigned maximumCount = 64;
    for (unsigned offset = offsetOfCreateThis + createThis->size(); offset < instructions.size(); offset += instructions.at(offset)->size()) {
        auto instruction = instructions.at(offset);
        switch (instruction->opcodeID()) {
        case op_mov: {
            auto bytecode = instruction->as<OpMov>();
            if (aliases.contains(bytecode.m_src)) {
                if (!aliases.contains(bytecode.m_dst))
                    aliases.append(bytecode.m_dst);
            } else
                aliases.removeAll(bytecode.m_dst);
            if (aliases.isEmpty())
                return plan;
            continue;
        }
        case op_put_by_id: {
            auto bytecode = instruction->as<OpPutById>();
            if (!aliases.contains(bytecode.m_base) || aliases.contains(bytecode.m_value) || codeBlock->handlerForBytecodeIndex(BytecodeIndex(offset)) != handler)
                return plan;
            size_t index = plan.properties.findIf([&](auto& property) { return property.identifier == bytecode.m_property; });
            if (index == notFound) {
                if (plan.properties.size() == maximumCount)
                    return plan;
                index = plan.properties.size();
                plan.properties.append({ bytecode.m_property, bytecode.m_flags.isDirect(), bytecode.m_flags.ecmaMode().isStrict(), !bytecode.m_flags.isDirect() });
            } else if (!plan.properties[index].isDefined)
                return plan;
            else
                plan.properties[index].isAssigned |= !bytecode.m_flags.isDirect();
            plan.stores.append({ offset, static_cast<unsigned>(index) });
            continue;
        }
        default:
            return plan;
        }
    }
    return plan;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
