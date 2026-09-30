/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperationsObjects.h"

#include "ObjectAllocationProfileInlines.h"
#include "ObjectConstructorInlines.h"

#if ENABLE(FTL_JIT)

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
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "JSAsyncFunction.h"
#include "JSAsyncFunctionGenerator.h"
#include "JSAsyncGenerator.h"
#include "JSAsyncGeneratorFunction.h"
#include "JSCellButterfly.h"
#include "JSGenerator.h"
#include "JSGeneratorFunction.h"
#include "JSLexicalEnvironmentInlines.h"
#include "JSPromise.h"
#include "JSPromiseConstructor.h"
#include "JSPropertyNameEnumeratorInlines.h"
#include "JSWithScope.h"
#include "MegamorphicCache.h"
#include "ObjectAllocationProfileInlines.h"
#include "ObjectConstructor.h"
#include "ObjectPrototype.h"
#include "RegExpObjectInlines.h"
#include "ScopedArguments.h"
#include "SymbolTableInlines.h"

namespace JSC { namespace AOT {

// ---- Allocation

//     cache: for allocation. cache[0].offset: the inline capacity.
JSC_DEFINE_JIT_OPERATION(operationAOTNewObject, JSObject*, (JSGlobalObject* globalObject, uint32_t inlineCapacity, Slot* cache))
{
    AOT_OPERATION_BEGIN(globalObject);
    if (StructureID structureID = cache[0].structureID)
        OPERATION_RETURN(scope, constructEmptyObject(vm, structureID.decode()));

    // The same structure the other tiers would use, found the way they find it when a function is linked.
    ObjectAllocationProfile profile;
    profile.initializeProfile(vm, globalObject, globalObject, globalObject->objectPrototype(), inlineCapacity);
    Structure* structure = profile.structure();
    fillAllocationCache(vm, callerData(globalObject, callFrame), cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), structure->inlineCapacity());
    OPERATION_RETURN(scope, constructEmptyObject(vm, structure));
}

// One that has nothing yet, of a family of structs: what it is given goes where the family has it.
JSC_DEFINE_JIT_OPERATION(operationAOTNewObjectOfFamily, JSObject*, (JSGlobalObject* globalObject, uint32_t family, Slot* cache))
{
    AOT_OPERATION_BEGIN(globalObject);
    if (StructureID structureID = cache[0].structureID)
        OPERATION_RETURN(scope, Instance::newObjectOf(vm, structureID.decode()));
    Structure* structure = Instance::ensure(globalObject).emptyStructureOfFamily(safeCast<uint16_t>(family));
    fillAllocationCache(vm, callerData(globalObject, callFrame), cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator));
    OPERATION_RETURN(scope, Instance::newObjectOf(vm, structure));
}

// A class has been defined whose instances are structs of that family: they are born into it.
JSC_DEFINE_JIT_OPERATION(operationAOTNoteClass, void, (JSGlobalObject* globalObject, EncodedJSValue encodedConstructor, EncodedJSValue encodedPrototype, uint32_t family))
{
    AOT_OPERATION_BEGIN(globalObject);
    auto* constructor = dynamicDowncast<JSFunction>(JSValue::decode(encodedConstructor));
    JSObject* prototype = JSValue::decode(encodedPrototype).getObject();
    RELEASE_ASSERT(constructor && prototype && constructor->canUseAllocationProfiles());
    FunctionRareData* rareData = constructor->ensureRareDataAndObjectAllocationProfile(globalObject, SlotsOfBornObjects::inlineSlots(safeCast<uint16_t>(family)));
    OPERATION_RETURN_IF_EXCEPTION(scope);
    Structure* usual = rareData->objectAllocationStructure();
    RELEASE_ASSERT(!usual->hasPolyProto() && usual->storedPrototypeObject() == prototype);
    rareData->objectAllocationProfile()->replaceStructure(vm, rareData, Instance::ensure(globalObject).emptyStructureOfFamily(safeCast<uint16_t>(family), prototype));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMakeAtom, void, (EncodedJSValue value))
{
    SlotsOfBornObjects::makeAtomIfString(JSValue::decode(value));
}

// { ...source }, which is to be of that family of structs, if of any. A copy of one of the family is one.
JSC_DEFINE_JIT_OPERATION(operationAOTCloneObject, JSObject*, (JSGlobalObject* globalObject, EncodedJSValue encodedSource, uint32_t family))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue source = JSValue::decode(encodedSource);
    if (!family)
        OPERATION_RETURN(scope, cloneObjectForSpread(globalObject, source));
    if (source.isCell() && source.asCell()->type() == FinalObjectType) {
        Structure* structure = source.asCell()->structure();
        if (structure->bornAs() == family && structure->canPerformFastPropertyEnumerationCommon()) {
            if (JSObject* copy = tryCreateObjectViaCloning(vm, globalObject, asObject(source)))
                OPERATION_RETURN(scope, copy);
        }
        if (Options::aotVerbose()) [[unlikely]]
            dataLogLn("AOT: a copy that is to be of family ", family, " is of what was born as ", structure->bornAs(), " and is made bit by bit");
    }
    OPERATION_RETURN(scope, cloneObjectForSpread(globalObject, source, Instance::newObjectOf(vm, Instance::ensure(globalObject).emptyStructureOfFamily(safeCast<uint16_t>(family)))));
}

// An object literal: op_new_object and the op_put_by_id that follow it. values: what goes in each of the first `count` slots of the object:
// which is the values of the properties one after the other, unless the literal is made as a layout that says otherwise
// (KnownShape::slots). Then some may have nothing in them.
JSC_DEFINE_JIT_OPERATION(operationAOTNewObjectLiteral, JSObject*, (JSGlobalObject* globalObject, EncodedJSValue* values, uint32_t count, Slot* cache))
{
    AOT_OPERATION_BEGIN(globalObject);
    if (StructureID structureID = cache[0].structureID) {
        // All that was missing was room.
        Structure* structure = structureID.decode();
        if (uint16_t family = structure->bornAs(); family && SlotsOfBornObjects::areStructs() && structure->outOfLineCapacity()) [[unlikely]] {
            JSObject* object = Instance::newObjectOf(vm, structure);
            for (unsigned i = 0; i < count; ++i) {
                if (values[i])
                    object->putDirectOffset(vm, SlotsOfBornObjects::offsetInFamily(family, i), JSValue::decode(values[i]));
            }
            OPERATION_RETURN(scope, object);
        }
        JSObject* object = constructEmptyObject(vm, structure);
        for (unsigned i = 0; i < count; ++i)
            object->putDirectOffset(vm, i, JSValue::decode(values[i]));
        OPERATION_RETURN(scope, object);
    }

    FunctionRef function = caller(globalObject, callFrame);
    Vector<unsigned, 32> identifiers;
    unsigned inlineCapacityInBytecode;
    uint32_t shape = function.siteConstantOf(cache);
    if (Options::aotVerbose()) [[unlikely]]
        dataLogLn("AOT: a literal of ", count, " is made by function ", function.index, " as shape ", shape, SharedData::contains(cache) ? " (with nobody's slots)" : "");
    std::span<const uint16_t> slots;
    if (shape)
        slots = function.instance->slotsOfKnownShape(shape);
    unsigned numberOfSlots = count;
    auto slotOf = [&](unsigned property) -> unsigned { return slots.empty() ? property : slots[property]; };
    if (AllocationPlan plan = function.planOf(cache)) {
        RELEASE_ASSERT(slots.empty() ? plan.count() == count : plan.count() == slots.size());
        count = plan.count();
        for (unsigned i = 0; i < count; ++i)
            identifiers.append(plan.identifier(i));
        inlineCapacityInBytecode = plan.inlineCapacity();
    } else {
        RELEASE_ASSERT(slots.empty());
        // The first so many of Graph::storesOfLiteral(). Which they are was settled when the code was compiled: they are the stores
        // to the register, whatever else there is in between.
        UnlinkedCodeBlock* codeBlock = callerCode(globalObject, callFrame);
        const JSInstruction* instruction = codeBlock->instructions().at(bytecodeIndexOfCaller(globalObject, callFrame)).ptr();
        inlineCapacityInBytecode = instruction->as<OpNewObject>().m_inlineCapacity;
        auto& instructions = codeBlock->instructions();
        VirtualRegister object = instruction->as<OpNewObject>().m_dst;
        for (unsigned offset = bytecodeIndexOfCaller(globalObject, callFrame).offset() + instruction->size(); identifiers.size() < count; offset += instructions.at(offset)->size()) {
            auto next = instructions.at(offset);
            if (next->opcodeID() != op_put_by_id)
                continue;
            auto bytecode = next->as<OpPutById>();
            if (bytecode.m_base == object)
                identifiers.append(bytecode.m_property);
        }
    }
    auto identifierOf = [&](unsigned index) -> const Identifier& {
        return identifierAt(globalObject, callFrame, identifiers[index]);
    };
    ObjectAllocationProfile profile;
    profile.initializeProfile(vm, globalObject, globalObject, globalObject->objectPrototype(), inlineCapacityInBytecode);
    if ((count > 1 || !slots.empty()) && !(Options::aotDisableFastPaths() & 4096)) {
        // All of it is known, so there is no call for a structure for every property on the way.
        Vector<UniquedStringImpl*, 32> names;
        for (unsigned i = 0; i < count; ++i)
            names.append(identifierOf(i).impl());
        if (Structure* structure = shape ? function.instance->structureOfKnownShape(shape, names.span()) : function.instance->structureOfLiteral(profile.structure(), names.span())) {
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
                fillAllocationCache(vm, callerData(globalObject, callFrame), cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(inlineCapacity), AllocatorForMode::EnsureAllocator), inlineCapacity);
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
        fillAllocationCache(vm, callerData(globalObject, callFrame), cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), structure->inlineCapacity());
    OPERATION_RETURN(scope, object);
}

// op_create_this and the stores that follow it (NewObjectPlan). values: what each property ends up as.
JSC_DEFINE_JIT_OPERATION(operationAOTCreateThisWithProperties, JSObject*, (JSGlobalObject* globalObject, JSObject* callee, EncodedJSValue* values, uint32_t count, Slot* cache))
{
    AOT_OPERATION_BEGIN(globalObject);
    Vector<NewObjectPlan::Property, 8> properties;
    unsigned inlineCapacityInBytecode;
    if (AllocationPlan plan = caller(globalObject, callFrame).planOf(cache)) {
        for (unsigned i = 0; i < plan.count(); ++i)
            properties.append({ plan.identifier(i), plan.isDefined(i), plan.isStrict(i) });
        inlineCapacityInBytecode = plan.inlineCapacity();
    } else {
        auto& instructions = callerCode(globalObject, callFrame)->instructions();
        unsigned offset = bytecodeIndexOfCaller(globalObject, callFrame).offset();
        properties = NewObjectPlan::forCreateThis(instructions, offset).properties;
        inlineCapacityInBytecode = instructions.at(offset)->as<OpCreateThis>().m_inlineCapacity;
    }
    RELEASE_ASSERT(properties.size() == count);

    JSObject* object = nullptr;
    JSFunction* constructor = dynamicDowncast<JSFunction>(callee);
    bool cacheable = false;
    if (constructor && constructor->canUseAllocationProfiles()) {
        ObjectAllocationProfileWithPrototype* allocationProfile = constructor->ensureRareDataAndObjectAllocationProfile(globalObject, inlineCapacityInBytecode)->objectAllocationProfile();
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        Structure* structure = allocationProfile->structure();
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

    Structure* first = object->structure();
    bool isLaidOutAsPlanned = true;
    for (unsigned i = 0; i < count; ++i) {
        const Identifier& ident = identifierAt(globalObject, callFrame, properties[i].identifier);
        PutPropertySlot slot(object, properties[i].isStrict, putByIdContextOf(globalObject, callFrame));
        if (properties[i].isDefined)
            CommonSlowPaths::putDirectWithReify(vm, globalObject, object, ident, JSValue::decode(values[i]), slot);
        else
            object->methodTable()->put(object, globalObject, ident, JSValue::decode(values[i]), slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        isLaidOutAsPlanned &= slot.isCacheablePut() && slot.base() == object && slot.type() == PutPropertySlot::NewProperty && slot.cachedOffset() == static_cast<PropertyOffset>(i);
    }
    cacheable &= isLaidOutAsPlanned;

    Structure* last = object->structure();
    if (isLaidOutAsPlanned && !last->isDictionary() && count <= last->inlineCapacity()) {
        if (uint32_t shape = caller(globalObject, callFrame).siteConstantOf(cache)) {
            last->setKnownShape(vm, safeCast<uint16_t>(shape));
            noteKnownShape(last, 2);
        }
    }
    // TEMPORARY-SLOT-STATS
    noteSlowPath("create_this_with_properties"_s, object, nullptr, !constructor ? "not a function"_s : !constructor->canUseAllocationProfiles() ? "no allocation profile"_s : first->hasPolyProto() ? "poly proto"_s : !cacheable ? "a store was not a plain addition in order"_s : last->isDictionary() ? "dictionary"_s : count > last->inlineCapacity() ? "more than fits inline"_s : object->butterfly() ? "has a butterfly"_s : (first->mayBePrototype() || last->mayBePrototype()) ? "may be a prototype"_s : "goes on to try"_s);
    if (!cacheable || last->isDictionary() || count > last->inlineCapacity() || object->butterfly() || first->mayBePrototype() || last->mayBePrototype())
        OPERATION_RETURN(scope, object);

    // The site knows one function, and this is another: a class that extends the one whose constructor this is, as a rule.
    if (cache->pointer && cache->pointer != constructor && !SharedData::contains(cache) && Options::aotCachesConstructionForManyFunctions()) {
        if (first->propertyAccessesAreCacheable() && canUseMegamorphicPutFastPath(first))
            vm.ensureMegamorphicCache().initAsConstruction(first->id(), last->id(), cache);
        OPERATION_RETURN(scope, object);
    }

    // What follows takes some doing. It works the first time or, as a rule, never.
    if ((cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        OPERATION_RETURN(scope, object);
    cache->offset += 1u << Slot::attemptsShift;

    // A store that leaves it to the prototypes made a property because none of them had anything to say. That has to stay so.
    makePrototypeChainWatchable(vm, object);
    ObjectPropertyConditionSet conditions;
    for (unsigned i = 0; i < count; ++i) {
        if (properties[i].isDefined)
            continue;
        UniquedStringImpl* uid = identifierAt(globalObject, callFrame, properties[i].identifier).impl();
        auto status = prepareChainForCaching(globalObject, object, uid, nullptr);
        if (!status || status->flattenedDictionary || status->usesPolyProto)
            OPERATION_RETURN(scope, object);
        ObjectPropertyConditionSet forThis = generateConditionsForPropertySetterMiss(vm, globalObject, globalObject, last, uid);
        if (!forThis.isValid())
            OPERATION_RETURN(scope, object);
        conditions = conditions.mergedWith(forThis);
    }
    if (!watchConditions(vm, callerData(globalObject, callFrame), cache, conditions)) {
        noteSlowPath("create_this_with_properties"_s, object, nullptr, "cannot watch"_s); // TEMPORARY-SLOT-STATS
        OPERATION_RETURN(scope, object);
    }
    noteSlowPath("create_this_with_properties"_s, object, nullptr, "filled"_s); // TEMPORARY-SLOT-STATS
    fillConstructionCache(vm, callerData(globalObject, callFrame), cache, constructor, first, last, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(last->inlineCapacity()), AllocatorForMode::EnsureAllocator));
    OPERATION_RETURN(scope, object);
}

// Stub::ConstructByCalling. What op_create_this and the op_ret of code that constructs do, around a call.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTConstructByCalling, UGPRPair, (CallFrame* callFrame))
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

JSC_DEFINE_JIT_OPERATION(operationAOTCreateThis, JSObject*, (JSGlobalObject* globalObject, JSObject* callee, uint32_t inlineCapacity))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSFunction* constructor = dynamicDowncast<JSFunction>(callee);
    if (constructor && constructor->canUseAllocationProfiles()) {
        ObjectAllocationProfileWithPrototype* allocationProfile = constructor->ensureRareDataAndObjectAllocationProfile(globalObject, inlineCapacity)->objectAllocationProfile();
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        Structure* structure = allocationProfile->structure();
        JSObject* result = Instance::newObjectOf(vm, structure);
        if (structure->hasPolyProto()) {
            JSObject* prototype = allocationProfile->prototype();
            result->putDirectOffset(vm, knownPolyProtoOffset, prototype);
            prototype->didBecomePrototype(vm);
        }
        OPERATION_RETURN(scope, result);
    }

    // https://tc39.es/ecma262/#sec-getprototypefromconstructor
    JSValue proto = callee->get(globalObject, vm.propertyNames->prototype);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
    if (proto.isObject())
        OPERATION_RETURN(scope, constructEmptyObject(globalObject, asObject(proto)));
    JSGlobalObject* functionGlobalObject = getFunctionRealm(globalObject, callee);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
    OPERATION_RETURN(scope, constructEmptyObject(functionGlobalObject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArray, JSObject*, (JSGlobalObject* globalObject, const EncodedJSValue* values, uint32_t count, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN(globalObject);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(static_cast<IndexingType>(indexingType));
    OPERATION_RETURN(scope, constructArray(globalObject, structure, std::bit_cast<const JSValue*>(values), count));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayWithSize, JSObject*, (JSGlobalObject* globalObject, EncodedJSValue size))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, constructArrayWithSizeQuirk(globalObject, nullptr, JSValue::decode(size)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayBuffer, JSObject*, (JSGlobalObject* globalObject, JSCell* cell))
{
    AOT_OPERATION_BEGIN(globalObject);
    auto* immutableButterfly = uncheckedDowncast<JSCellButterfly>(cell);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(immutableButterfly->indexingMode());
    OPERATION_RETURN(scope, CommonSlowPaths::allocateNewArrayBuffer(vm, structure, immutableButterfly));
}

// Which of the values are the results of op_spread is plain from the values: nothing else in a register is a JSCellButterfly.
//     yetToBeSpread: which of them (a bit for each) are what is to be spread, and not what came of spreading it: nothing happens between
//     when that would have been done and now (Graph::findListsOfArguments()).
JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayWithSpread, JSObject*, (JSGlobalObject* globalObject, EncodedJSValue* encodedValues, uint32_t count, uint32_t yetToBeSpread))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue* values = std::bit_cast<JSValue*>(encodedValues);
    auto spreadOf = [](JSValue value) -> JSCellButterfly* {
        return value.isCell() ? dynamicDowncast<JSCellButterfly>(value.asCell()) : nullptr;
    };

    if (yetToBeSpread) {
        // What iterating over an array comes to is what is in it, if nobody has said otherwise, and nobody can tell that it was not done.
        auto arrayToCopyFrom = [&](unsigned i) -> JSArray* {
            if (!(yetToBeSpread >> i & 1) || !values[i].isCell())
                return nullptr;
            auto* array = dynamicDowncast<JSArray>(values[i].asCell());
            return array && array->isIteratorProtocolFastAndNonObservable() ? array : nullptr;
        };
        bool allAreArraysToCopyFrom = count > 1;
        for (unsigned i = 0; i < count; ++i)
            allAreArraysToCopyFrom &= !(yetToBeSpread >> i & 1) || arrayToCopyFrom(i);
        if (allAreArraysToCopyFrom) {
            CheckedUint32 size = 0;
            for (unsigned i = 0; i < count; ++i) {
                if (JSArray* array = arrayToCopyFrom(i))
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
                        if (JSArray* array = arrayToCopyFrom(i)) {
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
        // Each in its turn, then, as it would have been. (What has been made is where the collector looks: in the caller's frame.)
        for (unsigned i = 0; i < count; ++i) {
            if (!(yetToBeSpread >> i & 1))
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

JSC_DEFINE_JIT_OPERATION(operationAOTNewArrayWithSpecies, JSObject*, (JSGlobalObject* globalObject, EncodedJSValue encodedLength, JSObject* array))
{
    AOT_OPERATION_BEGIN(globalObject);
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

JSC_DEFINE_JIT_OPERATION(operationAOTSpread, JSCell*, (JSGlobalObject* globalObject, EncodedJSValue encodedIterable))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, spread(globalObject, JSValue::decode(encodedIterable)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewRegExp, JSObject*, (JSGlobalObject* globalObject, JSCell* regExp))
{
    AOT_OPERATION_BEGIN(globalObject);
    static constexpr bool areLegacyFeaturesEnabled = true;
    // A RegExp is written to when it is matched with. One that was made when the program was built is the first VM's to write to.
    if (StaticHeap::isOnlyForFirstVM(regExp) && !StaticHeap::isFirst(vm)) [[unlikely]]
        regExp = RegExp::create(vm, uncheckedDowncast<RegExp>(regExp)->pattern(), uncheckedDowncast<RegExp>(regExp)->flags());
    OPERATION_RETURN(scope, RegExpObject::create(vm, globalObject->regExpStructure(), uncheckedDowncast<RegExp>(regExp), areLegacyFeaturesEnabled));
}

// op_new_reg_exp_shared. cache->pointer: the object that does for the site, which the code looks for before it comes here.
// TEMPORARY-SHAPE-COUNTS: how often something is tested that has passed the same test before. (Roughly: what is at an address may be something new.)
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNoteAssertion, void, (Instance* instance, EncodedJSValue value, uint32_t layouts))
{
    static NeverDestroyed<UncheckedKeyHashSet<uint64_t, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> seen;
    if (seen->size() > 200000)
        seen->clear();
    JSValue decoded = JSValue::decode(value);
    if (!decoded || !decoded.isCell())
        return;
    if (!seen->add((static_cast<uint64_t>(value) << 20) ^ layouts).isNewEntry)
        instance->shapeCounts[Instance::AssertionRepeated]++;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNoteExit, void, (Instance* instance, EncodedJSValue encodedBase, uint32_t layouts, uint32_t slot))
{
    JSValue base = JSValue::decode(encodedBase);
    auto count = [&](Instance::ShapeCount which) { instance->shapeCounts[which]++; };
    if (!base || !base.isCell())
        return count(Instance::ExitBaseIsNoCell);
    if (base.asCell()->type() != FinalObjectType)
        return count(Instance::ExitBaseIsNoPlainObject);
    Structure* structure = base.asCell()->structure();
    uint16_t bornAs = structure->bornAs();
    if (!bornAs)
        return count(structure->inlineCapacity() > slot && !structure->outOfLineCapacity() ? Instance::ExitBaseWasNeverBorn : Instance::ExitBaseWasNeverBornAndHasNoRoom);
    if (bornAs < (layouts >> 16) || bornAs > (layouts & 0xffff))
        return count(Instance::ExitBaseWasBornOtherwise);
    if (!asObject(base)->getDirect(slot))
        return count(Instance::ExitSlotIsEmpty);
    count(Instance::ExitOther);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSettleWhatWasBorn, void, (Instance* instance, JSObject* object))
{
    instance->shapeCounts[Instance::TakenOutAtBirth] += object->takeOutWhatItsSlotsDoNotHold(*instance->vm);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLinkTimeConstant, EncodedJSValue, (Instance* instance, uint32_t which))
{
    EncodedJSValue result = JSValue::encode(instance->globalObject->linkTimeConstant(static_cast<LinkTimeConstant>(which)));
    instance->linkTimeConstants[which] = result;
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewRegExpForReceiver, JSObject*, (JSGlobalObject* globalObject, JSCell* cell, uint32_t forTest, Slot* cache))
{
    AOT_OPERATION_BEGIN(globalObject);
    if (StaticHeap::isOnlyForFirstVM(cell) && !StaticHeap::isFirst(vm)) [[unlikely]]
        cell = RegExp::create(vm, uncheckedDowncast<RegExp>(cell)->pattern(), uncheckedDowncast<RegExp>(cell)->flags());
    auto* regExp = uncheckedDowncast<RegExp>(cell);
    // (The code takes what it finds there for good, which it is if nobody can put anything in the way of the builtin.)
    if (!Options::useSharedRegExpLiteralObjects() || !Options::useImmutableIntrinsics() || !RegExpObject::canShareLiteralAsReceiver(globalObject, forTest))
        OPERATION_RETURN(scope, RegExpObject::create(vm, globalObject->regExpStructure(), regExp));
    RegExpObject* object = RegExpObject::createSharedLiteral(vm, globalObject->regExpStructure(), regExp);
    cacheObjectOfSite(vm, callerData(globalObject, callFrame), cache, object);
    OPERATION_RETURN(scope, object);
}

//     cache: for allocation. cache[0].pointer: the executable.
//     isExpressionAndWhose: whether it is; above that, see functionOfBytecodeOfCaller().
JSC_DEFINE_JIT_OPERATION(operationAOTNewFunction, JSObject*, (JSGlobalObject* globalObject, JSScope* environment, uint32_t index, uint32_t isExpressionAndWhose, uint32_t kind, Slot* cache))
{
    AOT_OPERATION_BEGIN(globalObject);
    FunctionRef function = functionOfBytecodeOfCaller(globalObject, callFrame, isExpressionAndWhose >> 1);
    FunctionExecutable* executable = isExpressionAndWhose & 1 ? function.functionExpr(index) : function.functionDecl(index);
    JSFunction* result = nullptr;
    switch (static_cast<FunctionKind>(kind)) {
    case FunctionKind::Normal:
        result = JSFunction::create(vm, globalObject, executable, environment);
        break;
    case FunctionKind::Generator:
        result = JSGeneratorFunction::create(vm, globalObject, executable, environment);
        break;
    case FunctionKind::Async:
        result = JSAsyncFunction::create(vm, globalObject, executable, environment);
        break;
    case FunctionKind::AsyncGenerator:
        result = JSAsyncGeneratorFunction::create(vm, globalObject, executable, environment);
        break;
    }
    // Optimized code may take the only closure of a function for a constant. Once there have been two, nobody has to be told.
    if (executable->singletonHasBeenInvalidated())
        fillAllocationCache(vm, callerData(globalObject, callFrame), cache, result->structure(), subspaceFor<JSFunction>(vm)->allocatorFor(JSFunction::allocationSize(0), AllocatorForMode::EnsureAllocator), 0, executable);
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetFunctionName, void, (JSGlobalObject* globalObject, JSObject* function, EncodedJSValue name))
{
    AOT_OPERATION_BEGIN(globalObject);
    uncheckedDowncast<JSFunction>(function)->setFunctionName(globalObject, JSValue::decode(name));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewInternalFieldObject, JSObject*, (JSGlobalObject* globalObject, uint32_t kind))
{
    AOT_OPERATION_BEGIN(globalObject);
    switch (static_cast<InternalFieldObjectKind>(kind)) {
    case InternalFieldObjectKind::Promise:
        OPERATION_RETURN(scope, JSPromise::create(vm, globalObject->promiseStructure()));
    case InternalFieldObjectKind::Generator:
        OPERATION_RETURN(scope, JSGenerator::create(vm, globalObject->generatorStructure()));
    case InternalFieldObjectKind::AsyncFunctionGenerator:
        OPERATION_RETURN(scope, JSAsyncFunctionGenerator::create(vm, globalObject->asyncFunctionGeneratorStructure()));
    case InternalFieldObjectKind::AsyncGenerator:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateInternalFieldObject, JSObject*, (JSGlobalObject* globalObject, JSObject* callee, uint32_t kind))
{
    AOT_OPERATION_BEGIN(globalObject);
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

//     cache: for allocation. cache[0].offset: the number of variables.
JSC_DEFINE_JIT_OPERATION(operationAOTCreateLexicalEnvironment, JSObject*, (JSGlobalObject* globalObject, JSScope* currentScope, JSCell* symbolTableCell, EncodedJSValue initialValue, Slot* cache))
{
    AOT_OPERATION_BEGIN(globalObject);
    auto* symbolTable = uncheckedDowncast<SymbolTable>(symbolTableCell);
    JSLexicalEnvironment* result = JSLexicalEnvironment::create(vm, globalObject, currentScope, symbolTable, JSValue::decode(initialValue));
    // As for functions.
    if (symbolTable->singleton().hasBeenInvalidated())
        fillAllocationCache(vm, callerData(globalObject, callFrame), cache, result->structure(), subspaceFor<JSLexicalEnvironment>(vm)->allocatorFor(JSLexicalEnvironment::allocationSize(symbolTable), AllocatorForMode::EnsureAllocator), symbolTable->scopeSize());
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPushWithScope, JSObject*, (JSGlobalObject* globalObject, JSScope* currentScope, EncodedJSValue encodedObject))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSObject* object = JSValue::decode(encodedObject).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
    OPERATION_RETURN(scope, JSWithScope::create(vm, globalObject, currentScope, object));
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolveScopeForHoistingFuncDeclInEval, EncodedJSValue, (JSGlobalObject* globalObject, JSScope* environment, uint32_t identifierIndex))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSScope::resolveScopeForHoistingFuncDeclInEval(globalObject, environment, identifierAt(globalObject, callFrame, identifierIndex))));
}

// The arguments are read from where the caller of the function put them, which is not a place the function's code writes to.
JSC_DEFINE_JIT_OPERATION(operationAOTCreateDirectArguments, JSObject*, (JSGlobalObject* globalObject, JSObject* callee, uint32_t count, EncodedJSValue* arguments, uint32_t numberOfParameters))
{
    AOT_OPERATION_BEGIN(globalObject);
    // As DirectArguments::createByCopying().
    unsigned capacity = std::max(count, numberOfParameters);
    DirectArguments* result = DirectArguments::create(vm, globalObject->directArgumentsStructure(), count, capacity);
    for (unsigned i = count; i--;)
        result->setIndexQuickly(vm, i, JSValue::decode(arguments[i]));
    result->setCallee(vm, uncheckedDowncast<JSFunction>(callee));
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateScopedArguments, JSObject*, (JSGlobalObject* globalObject, JSObject* environmentObject, JSObject* callee, uint32_t count, EncodedJSValue* arguments))
{
    AOT_OPERATION_BEGIN(globalObject);
    auto* environment = uncheckedDowncast<JSLexicalEnvironment>(environmentObject);
    OPERATION_RETURN(scope, ScopedArguments::createByCopyingFrom(vm, globalObject->scopedArgumentsStructure(), std::bit_cast<Register*>(arguments), count, uncheckedDowncast<JSFunction>(callee), environment->symbolTable()->arguments(), environment));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateClonedArguments, JSObject*, (JSGlobalObject* globalObject, JSObject* callee, uint32_t count, EncodedJSValue* arguments))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, ClonedArguments::createByCopyingFrom(globalObject, globalObject->clonedArgumentsStructure(), std::bit_cast<Register*>(arguments), count, uncheckedDowncast<JSFunction>(callee), nullptr));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateRest, JSObject*, (JSGlobalObject* globalObject, uint32_t count, EncodedJSValue* arguments, uint32_t numParametersToSkip))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, constructArray(globalObject, globalObject->restParameterStructure(), std::bit_cast<JSValue*>(arguments) + numParametersToSkip, count > numParametersToSkip ? count - numParametersToSkip : 0));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowNotAFunction, void, (JSGlobalObject* globalObject, EncodedJSValue callee))
{
    AOT_OPERATION_BEGIN(globalObject);
    throwException(globalObject, scope, createNotAFunctionError(globalObject, JSValue::decode(callee)));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowNotAConstructor, void, (JSGlobalObject* globalObject, EncodedJSValue callee))
{
    AOT_OPERATION_BEGIN(globalObject);
    throwException(globalObject, scope, createNotAConstructorError(globalObject, JSValue::decode(callee)));
    OPERATION_RETURN(scope);
}

// ---- Conversions and tests

JSC_DEFINE_JIT_OPERATION(operationAOTToThis, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue value, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(value).toThis(globalObject, isStrict ? ECMAMode::strict() : ECMAMode::sloppy())));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToObject, JSObject*, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, uint32_t messageIdentifierIndex))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isUndefinedOrNull()) [[unlikely]] {
        const Identifier& message = identifierAt(globalObject, callFrame, messageIdentifierIndex);
        if (!message.isEmpty()) {
            throwException(globalObject, scope, createTypeError(globalObject, message.impl()));
            OPERATION_RETURN(scope, static_cast<JSObject*>(nullptr));
        }
    }
    OPERATION_RETURN(scope, value.toObject(globalObject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToPrimitive, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(value).toPrimitive(globalObject)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToPropertyKey, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(value).toPropertyKeyValue(globalObject)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypeof, JSCell*, (JSGlobalObject* globalObject, EncodedJSValue value))
{
    return jsTypeStringForValue(globalObject, JSValue::decode(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypeofIsObject, size_t, (JSGlobalObject* globalObject, EncodedJSValue value))
{
    return jsTypeofIsObject(globalObject, JSValue::decode(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypeofIsFunction, size_t, (JSGlobalObject* globalObject, EncodedJSValue value))
{
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

JSC_DEFINE_JIT_OPERATION(operationAOTStrcat, EncodedJSValue, (JSGlobalObject* globalObject, const EncodedJSValue* values, uint32_t count))
{
    AOT_OPERATION_BEGIN(globalObject);
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

JSC_DEFINE_JIT_OPERATION(operationAOTGetPrototypeOf, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(value).getPrototype(globalObject)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInstanceof, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, EncodedJSValue encodedConstructor))
{
    AOT_OPERATION_BEGIN(globalObject);
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

JSC_DEFINE_JIT_OPERATION(operationAOTThrowTDZError, void, (JSGlobalObject* globalObject, uint32_t isThis))
{
    AOT_OPERATION_BEGIN(globalObject);
    if (isThis) {
        throwException(globalObject, scope, createReferenceError(globalObject, "'super()' must be called in derived constructor before accessing |this| or returning non-object."_s));
        OPERATION_RETURN(scope);
    }
    // The name is what the source has at the call site that the caller left in its frame.
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

JSC_DEFINE_JIT_OPERATION(operationAOTThrowStaticError, void, (JSGlobalObject* globalObject, EncodedJSValue message, uint32_t errorType))
{
    AOT_OPERATION_BEGIN(globalObject);
    auto errorMessage = asString(JSValue::decode(message))->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    throwException(globalObject, scope, createError(globalObject, static_cast<ErrorTypeWithExtension>(errorType), errorMessage));
    OPERATION_RETURN(scope);
}

// ---- Properties

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
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByIdWellKnown, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, uint32_t which, Slot* cache))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& ident = wellKnownIdentifier(vm, which);
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
    Structure* structureBefore = base.isCell() ? base.asCell()->structure() : nullptr;
    JSValue result = base.get(globalObject, ident, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    cacheGetById(globalObject, callerData(globalObject, callFrame), base, structureBefore, ident, slot, cache);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

// The two ways for the thunks in front of the put operations to end that take more than a store. Neither throws, but they return
// to where an operation that may was called from.

// For a new property that the megamorphic cache knows the transition for, where the object needs more storage first.
JSC_DEFINE_JIT_OPERATION(operationAOTPutByIdReallocating, void, (VM* vmPointer, JSObject* base, EncodedJSValue value, const void* entryPointer))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
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
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    vm.writeBarrierSlowPath(cell);
    OPERATION_RETURN(scope);
}

//     cache: as for cacheGetById(), of which this only does the simplest case.
JSC_DEFINE_JIT_OPERATION(operationAOTGetByIdDirect, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, uint32_t identifierIndex, Slot* cache))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& ident = identifierAt(globalObject, callFrame, identifierIndex);
    PropertySlot slot(base, PropertySlot::InternalMethodType::GetOwnProperty);
    bool found = base.getOwnPropertySlot(globalObject, ident, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    JSValue result = found ? slot.getValue(globalObject, ident) : jsUndefined();
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());

    if (!(Options::aotDisableFastPaths() & 1) && !SharedData::contains(cache) && found && base.isCell() && slot.isCacheableValue() && slot.slotBase() == base.asCell()) {
        Structure* structure = base.asCell()->structure();
        auto location = locationOfProperty(slot.cachedOffset());
        if (structure->propertyAccessesAreCacheable() && !structure->isDictionary() && !structure->needImpurePropertyWatchpoint() && location) {
            cache->offset = *location;
            WTF::storeStoreFence();
            cache->structureID = structure->id();
            didFillSlot(vm, callerData(globalObject, callFrame));
        }
    }
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByIdWithThis, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue base, EncodedJSValue thisValue, uint32_t identifierIndex))
{
    AOT_OPERATION_BEGIN(globalObject);
    PropertySlot slot(JSValue::decode(thisValue), PropertySlot::InternalMethodType::Get);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(base).get(globalObject, identifierAt(globalObject, callFrame, identifierIndex), slot)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByValWithThis, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue thisValue, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_BEGIN(globalObject);
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

JSC_DEFINE_JIT_OPERATION(operationAOTPutByIdWithThis, void, (JSGlobalObject* globalObject, EncodedJSValue base, EncodedJSValue thisValue, EncodedJSValue value, uint32_t identifierIndex, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(globalObject);
    PutPropertySlot slot(JSValue::decode(thisValue), isStrict, putByIdContextOf(globalObject, callFrame));
    JSValue::decode(base).putInline(globalObject, identifierAt(globalObject, callFrame, identifierIndex), JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByValWithThis, void, (JSGlobalObject* globalObject, EncodedJSValue base, EncodedJSValue thisValue, EncodedJSValue property, EncodedJSValue value, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(globalObject);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PutPropertySlot slot(JSValue::decode(thisValue), isStrict);
    JSValue::decode(base).put(globalObject, key, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByValDirect, void, (JSGlobalObject* globalObject, JSObject* base, EncodedJSValue encodedProperty, EncodedJSValue encodedValue, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(globalObject);
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

JSC_DEFINE_JIT_OPERATION(operationAOTMapSet, void, (JSGlobalObject* globalObject, JSCell* map, EncodedJSValue key, EncodedJSValue value, int32_t hash))
{
    AOT_OPERATION_BEGIN(globalObject);
    uncheckedDowncast<JSMap>(map)->addNormalized(globalObject, JSValue::decode(key), JSValue::decode(value), hash);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetAdd, void, (JSGlobalObject* globalObject, JSCell* set, EncodedJSValue key, int32_t hash))
{
    AOT_OPERATION_BEGIN(globalObject);
    uncheckedDowncast<JSSet>(set)->addNormalized(globalObject, JSValue::decode(key), JSValue(), hash);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTInById, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, uint32_t identifierIndex))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, false);
    }
    const Identifier& identifier = identifierAt(globalObject, callFrame, identifierIndex);
    UniquedStringImpl* uid = identifier.impl();
    JSObject* baseObject = asObject(base);
    if ((Options::aotDisableFastPaths() & 1024) || parseIndex(*uid) || !vm.megamorphicCache())
        OPERATION_RETURN(scope, baseObject->hasProperty(globalObject, identifier));

    // What the other tiers do for a site that sees objects of all kinds: the answer is left where generateFrontEndInById() finds it.
    PropertySlot slot(base, PropertySlot::InternalMethodType::HasProperty);
    JSObject* object = baseObject;
    bool cacheable = true;
    while (true) {
        if (TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && object->type() != ArrayType && object->type() != JSFunctionType && object != globalObject->arrayPrototype()) [[unlikely]]
            OPERATION_RETURN(scope, object->getNonIndexPropertySlot(globalObject, uid, slot));
        Structure* structure = object->structure();
        bool hasProperty = object->getOwnNonIndexPropertySlot(vm, structure, uid, slot);
        structure = object->structure(); // (What is made of a static table when it is first asked for changes it.)
        cacheable &= structure->propertyAccessesAreCacheable();
        if (hasProperty) {
            if (cacheable && slot.isCacheable() && (slot.slotBase() == baseObject || !baseObject->structure()->isDictionary()))
                vm.megamorphicCache()->initAsHasHit(baseObject->structureID(), uid);
            OPERATION_RETURN(scope, true);
        }
        cacheable &= structure->propertyAccessesAreCacheableForAbsence() && structure->hasMonoProto();
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject()) {
            if (cacheable && !baseObject->structure()->isDictionary())
                vm.megamorphicCache()->initAsHasMiss(baseObject->structureID(), uid);
            OPERATION_RETURN(scope, false);
        }
        object = asObject(prototype);
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTInByVal, size_t, (JSGlobalObject* globalObject, EncodedJSValue base, EncodedJSValue property))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, CommonSlowPaths::opInByVal(globalObject, JSValue::decode(base), JSValue::decode(property)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDelById, size_t, (JSGlobalObject* globalObject, EncodedJSValue base, uint32_t identifierIndex, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    bool couldDelete = JSCell::deleteProperty(object, globalObject, identifierAt(globalObject, callFrame, identifierIndex));
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (!couldDelete && isStrict)
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
    OPERATION_RETURN(scope, couldDelete);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDelByVal, size_t, (JSGlobalObject* globalObject, EncodedJSValue base, EncodedJSValue encodedProperty, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(globalObject);
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

JSC_DEFINE_JIT_OPERATION(operationAOTGetPrivateName, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue base, EncodedJSValue property, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    object->getPrivateField(globalObject, key, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (JSValue::decode(base) == object && slot.isCacheableValue() && slot.slotBase() == object)
        cachePrivateName(vm, callerData(globalObject, callFrame), cache, object, JSValue::decode(property), slot.cachedOffset());
    OPERATION_RETURN(scope, JSValue::encode(slot.getValue(globalObject, key)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutPrivateName, void, (JSGlobalObject* globalObject, EncodedJSValue base, EncodedJSValue property, EncodedJSValue value, uint32_t, Slot* cache, uint32_t isDefine))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    // Private fields are only reachable from class bodies, which are strict.
    PutPropertySlot slot(object, true);
    if (isDefine)
        object->definePrivateField(globalObject, key, JSValue::decode(value), slot);
    else {
        Structure* structureBefore = object->structure();
        object->setPrivateField(globalObject, key, JSValue::decode(value), slot);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (JSValue::decode(base) == object && slot.isCacheablePut() && slot.type() == PutPropertySlot::ExistingProperty && slot.base() == object && object->structure() == structureBefore) {
            // Code that has folded the field to a constant has to hear about writes that go around the runtime.
            structureBefore->didCachePropertyReplacement(vm, slot.cachedOffset());
            cachePrivateName(vm, callerData(globalObject, callFrame), cache, object, JSValue::decode(property), slot.cachedOffset());
        }
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasPrivateName, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue property))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, false);
    }
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, asObject(base)->hasPrivateField(globalObject, key));
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasPrivateBrand, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue brand))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        OPERATION_RETURN(scope, false);
    }
    OPERATION_RETURN(scope, asObject(base)->hasPrivateBrand(globalObject, JSValue::decode(brand)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCheckPrivateBrand, void, (JSGlobalObject* globalObject, EncodedJSValue base, EncodedJSValue brand, uint32_t, Slot* cache, uint32_t))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    object->checkPrivateBrand(globalObject, JSValue::decode(brand));
    OPERATION_RETURN_IF_EXCEPTION(scope);
    // What brands an object has is a matter of its structure.
    if (JSValue::decode(base) == object)
        cachePrivateName(vm, callerData(globalObject, callFrame), cache, object, JSValue::decode(brand), std::nullopt);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetPrivateBrand, void, (JSGlobalObject* globalObject, JSObject* base, EncodedJSValue brand))
{
    AOT_OPERATION_BEGIN(globalObject);
    base->setPrivateBrand(globalObject, JSValue::decode(brand));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutAccessorById, void, (JSGlobalObject* globalObject, JSObject* base, uint32_t identifierIndex, uint32_t attributes, JSObject* accessor, uint32_t isSetter))
{
    AOT_OPERATION_BEGIN(globalObject);
    if (isSetter)
        base->putSetter(globalObject, identifierAt(globalObject, callFrame, identifierIndex), accessor, attributes);
    else
        base->putGetter(globalObject, identifierAt(globalObject, callFrame, identifierIndex), accessor, attributes);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutGetterSetterById, void, (JSGlobalObject* globalObject, JSObject* base, uint32_t identifierIndex, uint32_t attributes, EncodedJSValue getter, EncodedJSValue setter))
{
    AOT_OPERATION_BEGIN(globalObject);
    GetterSetter* accessor = GetterSetter::create(vm, globalObject, JSValue::decode(getter), JSValue::decode(setter));
    CommonSlowPaths::putDirectAccessorWithReify(vm, globalObject, base, identifierAt(globalObject, callFrame, identifierIndex), accessor, attributes);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutAccessorByVal, void, (JSGlobalObject* globalObject, JSObject* base, EncodedJSValue property, uint32_t attributes, JSObject* accessor, uint32_t isSetter))
{
    AOT_OPERATION_BEGIN(globalObject);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (isSetter)
        base->putSetter(globalObject, key, accessor, attributes);
    else
        base->putGetter(globalObject, key, accessor, attributes);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineDataProperty, void, (JSGlobalObject* globalObject, JSObject* base, EncodedJSValue property, EncodedJSValue value, int32_t attributes))
{
    AOT_OPERATION_BEGIN(globalObject);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PropertyDescriptor descriptor = toPropertyDescriptor(JSValue::decode(value), jsUndefined(), jsUndefined(), DefinePropertyAttributes(attributes));
    base->methodTable()->defineOwnProperty(base, globalObject, key, descriptor, true);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineAccessorProperty, void, (JSGlobalObject* globalObject, JSObject* base, EncodedJSValue property, EncodedJSValue getter, EncodedJSValue setter, int32_t attributes))
{
    AOT_OPERATION_BEGIN(globalObject);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PropertyDescriptor descriptor = toPropertyDescriptor(jsUndefined(), JSValue::decode(getter), JSValue::decode(setter), DefinePropertyAttributes(attributes));
    base->methodTable()->defineOwnProperty(base, globalObject, key, descriptor, true);
    OPERATION_RETURN(scope);
}

// ---- for-in

JSC_DEFINE_JIT_OPERATION(operationAOTGetPropertyEnumerator, JSCell*, (JSGlobalObject* globalObject, EncodedJSValue encodedBase))
{
    AOT_OPERATION_BEGIN(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    if (base.isUndefinedOrNull())
        OPERATION_RETURN(scope, static_cast<JSCell*>(vm.emptyPropertyNameEnumerator()));
    JSObject* object = base.toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSCell*>(nullptr));
    OPERATION_RETURN(scope, static_cast<JSCell*>(propertyNameEnumerator(globalObject, object)));
}

// Compiled code that knows no more of a number than that it is one hands it over as a double.
static ALWAYS_INLINE uint32_t enumeratorIndex(EncodedJSValue index)
{
    JSValue value = JSValue::decode(index);
    return value.isInt32() ? value.asUInt32() : static_cast<uint32_t>(value.asNumber());
}

static ALWAYS_INLINE JSPropertyNameEnumerator::Flag enumeratorMode(EncodedJSValue mode)
{
    return static_cast<JSPropertyNameEnumerator::Flag>(JSValue::decode(mode).asUInt32());
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorNext, JSCell*, (JSGlobalObject* globalObject, EncodedJSValue base, JSCell* enumerator, EncodedJSValue* modeAndIndex))
{
    AOT_OPERATION_BEGIN(globalObject);
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

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorGetByVal, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue base, EncodedJSValue propertyName, EncodedJSValue index, EncodedJSValue mode, JSCell* enumerator))
{
    AOT_OPERATION_BEGIN(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(CommonSlowPaths::opEnumeratorGetByVal(globalObject, JSValue::decode(base), JSValue::decode(propertyName), enumeratorIndex(index), enumeratorMode(mode), uncheckedDowncast<JSPropertyNameEnumerator>(enumerator))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorInByVal, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue propertyName, EncodedJSValue index, EncodedJSValue encodedMode, JSCell* enumerator))
{
    AOT_OPERATION_BEGIN(globalObject);
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

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorPutByVal, void, (JSGlobalObject* globalObject, EncodedJSValue base, EncodedJSValue propertyName, EncodedJSValue value, EncodedJSValue index, EncodedJSValue mode, JSCell* enumerator, uint32_t isStrict))
{
    AOT_OPERATION_BEGIN(globalObject);
    CommonSlowPaths::opEnumeratorPutByVal(globalObject, JSValue::decode(base), JSValue::decode(propertyName), JSValue::decode(value), isStrict ? ECMAMode::strict() : ECMAMode::sloppy(), enumeratorIndex(index), enumeratorMode(mode), uncheckedDowncast<JSPropertyNameEnumerator>(enumerator));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorHasOwnProperty, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue propertyName, EncodedJSValue index, EncodedJSValue encodedMode, JSCell* enumerator))
{
    AOT_OPERATION_BEGIN(globalObject);
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

#endif // ENABLE(FTL_JIT)
