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
        JSObject* object = constructEmptyObject(vm, structure);
        for (unsigned i = 0; i < count; ++i)
            object->putDirectOffset(vm, i, JSValue::decode(values[i]));
        OPERATION_RETURN(scope, object);
    }

    FunctionRef function = caller(instance, callFrame);
    RELEASE_ASSERT(function.info().isTyped);
    OPERATION_RETURN(scope, newObjectLiteralAtSite(instance, globalObject, vm, function, cache, values, count));
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

JSC_DEFINE_JIT_OPERATION(operationAOTNewArray, JSObject*, (Instance* instance, const EncodedJSValue* values, uint32_t count, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN(instance);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(static_cast<IndexingType>(indexingType));
    OPERATION_RETURN(scope, constructArray(globalObject, structure, std::bit_cast<const JSValue*>(values), count));
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
