/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedObject.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLockedOperation.h"
#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTSlotWatchpoint.h"
#include "AOTTypedCompactTable.h"
#include "AOTTypedType.h"
#include "AbstractModuleRecord.h"
#include "AddressSpaceBudget.h"
#include "AdaptiveInferredPropertyValueWatchpointBase.h"
#include "AsyncContextSwapScope.h"
#include "AsyncFunctionPrototype.h"
#include "AsyncGeneratorFunctionPrototype.h"
#include "AsyncGeneratorPrototype.h"
#include "AsyncIteratorPrototype.h"
#include "BlockDirectoryInlines.h"
#include "BuiltinNames.h"
#include "BytecodeStructs.h"
#include "CachedTypes.h"
#include "CustomGetterSetter.h"
#include "DateInstance.h"
#include "DeferGCInlines.h"
#include "ErrorInstance.h"
#include "FunctionPrototype.h"
#include "GeneratorFunctionPrototype.h"
#include "GeneratorPrototype.h"
#include "GetByIdMetadata.h"
#include "GetterSetter.h"
#include "HeapAnalyzer.h"
#include "HeapIterationScope.h"
#include "HeapObserver.h"
#include "InternalFieldTuple.h"
#include "IteratorOperations.h"
#include "JSAsyncFromSyncIterator.h"
#include "JSBoundFunction.h"
#include "JSCellButterfly.h"
#include "JSCInlines.h"
#include "ObjectPropertyConditionSet.h"
#include "JSGlobalObjectFunctions.h"
#include "JSIteratorPrototype.h"
#include "JSModuleLoader.h"
#include "JSModuleRecord.h"
#include "JSNativeStdFunction.h"
#include "JSPromise.h"
#include "JSPromiseCombinatorsGlobalContext.h"
#include "JSPromiseReaction.h"
#include "LLIntThunks.h"
#include "MarkedBlockCage.h"
#include "MarkedSpaceInlines.h"
#include "Microtask.h"
#include "MicrotaskQueueInlines.h"
#include "StackFrame.h"
#include "ObjectConstructor.h"
#include "ObjectConstructorInlines.h"
#include "ObjectPrototype.h"
#include "PreventCollectionScope.h"
#include "PropertyNameArray.h"
#include "ProtoCallFrameInlines.h"
#include "ProxyObject.h"
#include "StackVisitor.h"
#include "TopExceptionScope.h"
#include "TypeError.h"
#include "WeakGCMapInlines.h"
#include <unicode/utf16.h>
#include <map>
#include <mutex>
#include <wtf/BitVector.h>
#include <wtf/FlipBytes.h>
#include <wtf/HexNumber.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/SHA1.h>
#include <wtf/Scope.h>
#include <wtf/ThreadSpecific.h>

namespace JSC { namespace AOT { namespace Typed {

const ClassInfo Object::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Object) };

Structure* Object::createStructure(VM& vm, Instance& instance, JSValue prototype, const LayoutData* layout)
{
    Structure* structure = isObjectLayout(layout->kind)
        ? Structure::create(vm, instance.globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info(), NonArray, 0)
        : Structure::create(vm, nullptr, jsNull(), TypeInfo(CellType, StructureFlags), info(), NonArray, 0);
    structure->setCompiledLayout(layout);
    structure->setAOTInstance(&instance);
    structure->setMayBePrototype(true);
    structure->setHasImmutableShape(true);
    if (layout->hasFieldsWithSymbolKeys())
        structure->markingStampAndFlags() |= Structure::hasFieldsWithSymbolKeys;
    return structure;
}

static_assert(offsetOfEnvironmentShapeLowByte == JSCell::indexingTypeAndMiscOffset() && offsetOfEnvironmentShapeHighByte == JSCell::typeInfoFlagsOffset());

static void clearSlot(void* slot, Store);

Object* Object::create(VM& vm, Structure* structure, const LayoutData& layout, size_t size, uint32_t orderWord)
{
    RELEASE_ASSERT(size >= layout.size);
    void* cell;
    if (isObjectLayout(layout.kind))
        cell = allocateCell<Object>(vm, size);
    else {
        cell = vm.heap.aotClosureSpace.allocate(vm, WTF::roundUpToMultipleOf<atomSize>(size), nullptr, AllocationFailureMode::Assert);
        static_cast<JSCell*>(cell)->clearStructure();
    }
    Object* object = new (NotNull, cell) Object(vm, structure);
    zeroSpan(std::span { reinterpret_cast<uint8_t*>(object) + sizeof(JSCell), size - sizeof(JSCell) });
    if (isObjectLayout(layout.kind)) {
        for (uint32_t i = 0; i < layout.numberOfFields; ++i)
            object->clear(layout.fields()[i]);
        object->at<uint32_t>(offsetOfOrderWord) = orderWord;
        object->at<uint32_t>(offsetOfShapeWord) = shapeWordOf(layout.shape);
    } else {
        for (auto& variable : layout.describedVariables()) {
            if (!representsAbsentAsZero(variable.store()))
                clearSlot(reinterpret_cast<uint8_t*>(object) + variable.offset, variable.store());
        }
        object->at<uint8_t>(offsetOfEnvironmentShapeLowByte) = layout.shape;
        object->at<uint8_t>(offsetOfEnvironmentShapeHighByte) = layout.shape >> 8;
    }
    if (isObjectLayout(layout.kind))
        object->JSCell::finishCreation(vm);
    else
        vm.mutatorFence();
    return object;
}

Object* Object::createWithBody(VM& vm, Structure* structure, std::span<const uint8_t> body)
{
    Object* object = new (NotNull, allocateCell<Object>(vm, body.size() + sizeof(JSCell))) Object(vm, structure);
    memcpySpan(std::span { reinterpret_cast<uint8_t*>(object) + sizeof(JSCell), body.size() }, body);
    object->JSCell::finishCreation(vm);
    return object;
}

Object* Object::createWithInitializedFields(VM& vm, Structure* structure, const LayoutData& layout, size_t size, uint32_t orderWord)
{
    Object* object = create(vm, structure, layout, size, orderWord);
    const FieldData* farthest = nullptr;
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        const FieldData& field = layout.fields()[i];
        if (field.flags & fieldIsOutOfLine && field.flags & fieldAllowsUndefined && !(field.flags & fieldIsAbsentAtCreation) && (!farthest || field.offset > farthest->offset))
            farthest = &field;
    }
    if (farthest)
        object->ensureExtensionCovers(vm, object->state(), *farthest);
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        const FieldData& field = layout.fields()[i];
        if (field.flags & fieldIsAbsentAtCreation)
            continue;
        bool isUndefined = field.flags & fieldAllowsUndefined;
        if (isUndefined && !(field.flags & (fieldIsOutOfLine | fieldIsPinned))) {
            if (field.store() == Store::OptionalCell32) {
                object->at<uint32_t>(field.offset) = undefinedCell32;
                continue;
            }
            if (field.store() == Store::JSValue) {
                object->at<EncodedJSValue>(field.offset) = JSValue::encode(jsUndefined());
                continue;
            }
        }
        switch (field.store()) {
        case Store::JSValue:
        case Store::OptionalCell32:
        case Store::OptionalBoolean:
        case Store::LiteralString:
        case Store::LiteralString32:
            if (isUndefined) {
                bool didStore = object->store(vm, field, jsUndefined());
                RELEASE_ASSERT(didStore);
            }
            break;
        case Store::OptionalInt64:
            object->at<int64_t>(field.offset) = isUndefined ? undefinedInteger : 0;
            break;
        case Store::OptionalDouble:
            object->at<uint64_t>(field.offset) = isUndefined ? undefinedDouble : 0;
            break;
        case Store::Int32OrNumberBox:
            object->at<int32_t>(field.offset) = isUndefined ? undefinedInt32 : 0;
            break;
        default:
            break;
        }
    }
    return object;
}

JSFinalObject* Object::tryCopyExtraPropertiesForClone(VM& vm) const
{
    JSObject* extra = extraProperties();
    Structure* structure = extra ? extra->structure() : nullptr;
    if (!extra || orderWord() & orderWordIsFrozen || !structure->canPerformFastPropertyEnumerationCommon() || !checkStructureForClone(structure))
        return nullptr;
    unsigned capacity = extra->butterfly() ? structure->outOfLineCapacity() : 0;
    if (!capacity)
        return JSFinalObject::createWithButterflyCopyingInlineStorage(vm, structure, nullptr, extra->inlineStorage());
    DeferGC deferGC(vm);
    Butterfly* storage = Butterfly::createUninitialized(vm, nullptr, 0, capacity, false, 0);
    memcpy(storage->propertyStorage() - capacity, extra->butterfly()->propertyStorage() - capacity, capacity * sizeof(EncodedJSValue));
    return JSFinalObject::createWithButterflyCopyingInlineStorage(vm, structure, storage, extra->inlineStorage());
}

Object* Object::clone(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = this->state();
    const LayoutData& layout = this->layout();
    RELEASE_ASSERT(layout.kind == LayoutKind::Record);
    materializeLazyClosures();
    Structure* structure = state.structureFor(vm, layout.shape, globalObject->objectPrototype());
    size_t size = state.sizeWithSpecializationCapacity(layout.shape);
    JSFinalObject* copiedExtraProperties = tryCopyExtraPropertiesForClone(vm);
    if (hasExtraProperties() && !copiedExtraProperties) {
        Object* copy = create(vm, structure, layout, size, state.orderStartingWith(layout, { }));
        state.countObject(copy);
        PropertyNameArrayBuilder names(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
        getOwnPropertyNames(this, globalObject, names, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, nullptr);
        for (auto& name : names) {
            PropertySlot slot(this, PropertySlot::InternalMethodType::GetOwnProperty);
            bool has = getOwnPropertySlot(this, globalObject, name, slot);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (!has || slot.attributes() & PropertyAttribute::DontEnum)
                continue;
            JSValue value = slot.getValue(globalObject, name);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (const FieldData* field = copy->fieldNamed(state, name)) {
                bool didStore = copy->store(vm, *field, value);
                RELEASE_ASSERT(didStore);
                copy->didCreateField(state, *field);
                continue;
            }
            copy->createDataProperty(globalObject, name, value, true);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        return copy;
    }
    Extension* copiedExtension = nullptr;
    if (layout.hasExtension && extension()) {
        copiedExtension = Extension::create(vm, extension()->size());
        memcpySpan(copiedExtension->slots(), extension()->slots());
        state.countExtension(0, copiedExtension->size());
    }
    Object* copy = create(vm, structure, layout, size, orderWord() & orderAndCountMask);
    memcpySpan(std::span { &copy->at<uint8_t>(offsetOfFirstField), layout.size - offsetOfFirstField }, std::span { &at<uint8_t>(offsetOfFirstField), layout.size - offsetOfFirstField });
    if (layout.hasExtension)
        copy->setExtension(copiedExtension);
    copy->copyNumberBoxes(state);
    vm.mutatorFence();
    state.countObject(copy);
    if (copiedExtraProperties)
        copy->setExtraProperties(vm, copiedExtraProperties);
    return copy;
}

JSObject* Object::tryCopyAsPlainObject(JSGlobalObject* globalObject) const
{
    State& state = this->state();
    if (globalObject != state.instance.globalObject)
        return nullptr;
    VM& vm = globalObject->vm();
    if (hasExtraProperties())
        return tryCopyUsingPropertyList(vm, nullptr);
    auto& plan = state.plainCopyPlan(vm, layout(), orderWord() & orderAndCountMask);
    if (!plan.structure)
        return nullptr;
    JSFinalObject* copy = JSFinalObject::create(vm, plan.structure);
    auto* slot = copy->inlineStorage();
    for (auto& property : plan.properties) {
        JSValue value = readInline(*property.field);
        if (!value)
            return nullptr;
        (slot++)->setWithoutWriteBarrier(value);
    }
    return copy;
}

Structure* Object::plainCopyStructure(JSGlobalObject* globalObject) const
{
    State& state = this->state();
    if (globalObject != state.instance.globalObject)
        return nullptr;
    if (hasExtraProperties()) {
        DeferGC deferGC(globalObject->vm());
        auto* listing = cachedPropertyList(globalObject->vm());
        return listing ? state.plainCopyStructure(globalObject->vm(), *listing) : nullptr;
    }
    return state.plainCopyPlan(globalObject->vm(), layout(), orderWord() & orderAndCountMask).structure;
}

JSFinalObject* Object::tryCopyAsPlainObjectWithCapacity(JSGlobalObject* globalObject, Structure* plainCopyPrefixStructure) const
{
    VM& vm = globalObject->vm();
    if (hasExtraProperties())
        return tryCopyUsingPropertyList(vm, plainCopyPrefixStructure);
    auto& plan = state().plainCopyPlan(vm, layout(), orderWord() & orderAndCountMask);
    JSFinalObject* copy = JSFinalObject::create(vm, plainCopyPrefixStructure);
    auto* slot = copy->inlineStorage();
    for (auto& property : plan.properties) {
        JSValue value = readInline(*property.field);
        if (!value)
            return nullptr;
        (slot++)->setWithoutWriteBarrier(value);
    }
    return copy;
}

JSObject* Object::tryCopyAsPlainObject(JSGlobalObject* globalObject, std::span<const EncodedJSValue> excluded) const
{
    State& state = this->state();
    if (hasExtraProperties() || globalObject != state.instance.globalObject)
        return nullptr;
    Vector<UniquedStringImpl*, 8> atoms;
    for (EncodedJSValue key : excluded) {
        JSValue value = JSValue::decode(key);
        if (!value.isString() || asString(value)->isRope())
            return nullptr;
        auto string = asString(value)->tryGetValue();
        if (!string->impl()->isAtom())
            return nullptr;
        atoms.append(static_cast<UniquedStringImpl*>(string->impl()));
    }
    VM& vm = globalObject->vm();
    auto& plan = state.plainCopyExcludingPlan(vm, layout(), orderWord() & orderAndCountMask, atoms.span());
    if (!plan.structure)
        return nullptr;
    JSFinalObject* copy = JSFinalObject::create(vm, plan.structure);
    auto* slot = copy->inlineStorage();
    for (auto& property : plan.properties) {
        JSValue value = readInline(*property.field);
        if (!value)
            return nullptr;
        (slot++)->setWithoutWriteBarrier(value);
    }
    return copy;
}

Object* Object::tryCopyAs(JSGlobalObject* globalObject, const LayoutData& to, CopyNamesKind namesKind, std::span<const EncodedJSValue> copyNames, bool* neverCopyable)
{
    VM& vm = globalObject->vm();
    State& state = this->state();
    auto namesWhen = [&](CopyNamesKind which) {
        return copyNames.first(namesKind == which ? copyNames.size() : 0);
    };
    auto excluded = namesWhen(CopyNamesKind::Excluded);
    auto storedNext = namesWhen(CopyNamesKind::StoredNext);
    auto storedFirst = namesWhen(CopyNamesKind::StoredFirst);
    if (layout().kind != LayoutKind::Record || to.kind != LayoutKind::Record || hasExtraProperties() || storedFirst.size() % 2)
        return nullptr;
    Vector<UniquedStringImpl*, 8> atoms;
    bool areAtoms = orderOf(orderWord()) != keysAreInLayoutOrder;
    for (EncodedJSValue key : excluded) {
        JSValue value = JSValue::decode(key);
        if (value.isSymbol() && !state.hasFieldsWithSymbolKeys(layout()))
            continue;
        if (!value.isString() || asString(value)->isRope())
            return nullptr;
        if (auto string = asString(value)->tryGetValue(); string->impl()->isAtom())
            atoms.append(static_cast<UniquedStringImpl*>(string->impl()));
        else
            areAtoms = false;
    }
    auto appendName = [&](EncodedJSValue key) {
        JSValue value = JSValue::decode(key);
        if (!value.isString() || asString(value)->isRope())
            return false;
        if (auto string = asString(value)->tryGetValue(); string->impl()->isAtom())
            atoms.append(static_cast<UniquedStringImpl*>(string->impl()));
        else
            areAtoms = false;
        return true;
    };
    if (!storedNext.empty())
        atoms.append(nullptr);
    for (EncodedJSValue key : storedNext) {
        if (!appendName(key))
            return nullptr;
    }
    if (!storedFirst.empty()) {
        atoms.append(nullptr);
        atoms.append(nullptr);
    }
    for (size_t i = 0; i < storedFirst.size(); i += 2) {
        if (!appendName(storedFirst[i]))
            return nullptr;
    }
    auto put = [&](Object* copy, const FieldData& field, JSValue value) {
        const RuntimeType& type = state.ownTypeOf(field);
        return (value.isObject() ? state.matchesTypedSubtype(type, asObject(value)) : state.fits(type, value)) && copy->store(vm, field, value);
    };
    if (const State::CopyPlan* plan = areAtoms ? state.copyPlan(layout().shape, orderWord(), to.shape, atoms.span()) : nullptr) {
        if (!plan->structure) {
            if (neverCopyable)
                *neverCopyable = true;
            return nullptr;
        }
        Object* copy = create(vm, plan->structure, to, plan->size, plan->orderWord);
        for (auto& move : plan->moves)
            memcpy(&copy->at<uint8_t>(move.to), &at<uint8_t>(move.from), move.size);
        copy->copyNumberBoxes(state);
        for (auto& fill : plan->fills)
            memcpy(&copy->at<uint8_t>(fill.to), &fill.bits, fill.size);
        for (auto& conversion : plan->conversions) {
            if (!copy->store(vm, to.fields()[conversion.to], read(layout().fields()[conversion.from])) || copy->hasExtraProperties())
                return nullptr;
        }
        for (auto& conversion : plan->checkedConversions) {
            JSValue value = read(layout().fields()[conversion.from]);
            if (!value)
                return nullptr;
            const FieldData& field = to.fields()[conversion.to];
            bool isStored = conversion.acceptedKinds & State::CoercionPlan::kindOf(value) ? copy->store(vm, field, value) : put(copy, field, value);
            if (!isStored || copy->hasExtraProperties())
                return nullptr;
        }
        for (auto& given : plan->given) {
            if (!put(copy, to.fields()[given.field], JSValue::decode(storedFirst[given.index])) || copy->hasExtraProperties())
                return nullptr;
        }
        vm.mutatorFence();
        state.countObject(copy);
        return copy;
    }
    Vector<Identifier, 8> names;
    for (EncodedJSValue key : excluded) {
        if (JSValue value = JSValue::decode(key); value.isString())
            names.append(Identifier::fromString(vm, asString(value)->tryGetValue()));
    }
    Vector<Identifier, 4> next;
    for (EncodedJSValue key : storedNext)
        next.append(Identifier::fromString(vm, asString(JSValue::decode(key))->tryGetValue()));
    Object* copy = create(vm, state.structureFor(vm, to.shape, globalObject->objectPrototype()), to, state.sizeWithSpecializationCapacity(to.shape), state.orderStartingWith(to, { }));
    BitVector created;
    BitVector filled;
    BitVector given;
    auto fill = [&](const FieldData& field) {
        return copy->read(field) || copy->store(vm, field, jsUndefined());
    };
    Vector<uint16_t, 16> keys;
    bool isInlinedBlock = true;
    bool alwaysFails = false;
    for (size_t i = 0; i < storedFirst.size() && isInlinedBlock; i += 2) {
        const FieldData* field = state.field(vm, to, Identifier::fromString(vm, asString(JSValue::decode(storedFirst[i]))->tryGetValue()).impl());
        alwaysFails = !field || field->flags & fieldIsImmutable || copy->isDiscriminant(*field) || created.get(copy->indexOf(*field));
        isInlinedBlock = !alwaysFails && put(copy, *field, JSValue::decode(storedFirst[i + 1]));
        if (!isInlinedBlock)
            break;
        copy->willCreateField(state, *field);
        keys.append(copy->indexOf(*field));
        created.set(keys.last());
        given.set(keys.last());
    }
    forEachFieldAsProperty([&](UniquedStringImpl& name, JSValue value) {
        if (!isInlinedBlock)
            return false;
        if (names.containsIf([&](const Identifier& other) { return other.impl() == &name; }))
            return true;
        const FieldData* field = state.field(vm, to, &name);
        isInlinedBlock = field && !(field->flags & fieldIsImmutable) && !copy->isDiscriminant(*field);
        alwaysFails = !isInlinedBlock;
        bool isStoredNext = next.containsIf([&](const Identifier& other) { return other.impl() == &name; });
        if (isInlinedBlock && isStoredNext) {
            isInlinedBlock = fill(*field);
            filled.set(copy->indexOf(*field));
        } else if (isInlinedBlock)
            isInlinedBlock = put(copy, *field, value);
        if (isInlinedBlock && given.get(copy->indexOf(*field)))
            given.clear(copy->indexOf(*field));
        else if (isInlinedBlock) {
            copy->willCreateField(state, *field);
            keys.append(copy->indexOf(*field));
            created.set(keys.last());
        }
        return isInlinedBlock;
    }, SymbolKeys::Include);
    for (auto& name : next) {
        if (!isInlinedBlock)
            break;
        const FieldData* field = state.field(vm, to, name.impl());
        isInlinedBlock = field && !(field->flags & fieldIsImmutable) && !copy->isDiscriminant(*field);
        alwaysFails = !isInlinedBlock;
        if (!isInlinedBlock || created.get(copy->indexOf(*field)))
            continue;
        isInlinedBlock = fill(*field);
        if (isInlinedBlock) {
            copy->willCreateField(state, *field);
            keys.append(copy->indexOf(*field));
            created.set(keys.last());
            filled.set(keys.last());
        }
    }
    for (uint32_t i = 0; i < to.numberOfFields && isInlinedBlock; ++i) {
        isInlinedBlock = to.fields()[i].flags & fieldIsOptional || created.get(i);
        alwaysFails = !isInlinedBlock;
    }
    if (!isInlinedBlock || copy->hasExtraProperties()) {
        if (alwaysFails && areAtoms) {
            auto plan = makeUnique<State::CopyPlan>();
            for (UniquedStringImpl* atom : atoms)
                plan->excluded.append(atom);
            state.addCopyPlan(layout().shape, orderWord(), to.shape, WTF::move(plan));
            if (neverCopyable)
                *neverCopyable = true;
        }
        return nullptr;
    }
    copy->setOrderAndCount(state.orderStartingWith(to, keys.span()));
    state.countObject(copy);
    if (areAtoms && orderOf(copy->orderWord()) != keysAreInLayoutOrder && copy->structure() == state.structure(vm, to.shape)) {
        auto plan = makeUnique<State::CopyPlan>();
        plan->structure = copy->structure();
        plan->size = state.sizeWithSpecializationCapacity(to.shape);
        plan->orderWord = copy->orderWord();
        bool appliesToAll = true;
        for (uint16_t key : keys) {
            const FieldData& field = to.fields()[key];
            if (filled.get(key)) {
                appliesToAll = appliesToAll && !(field.flags & fieldIsOutOfLine) && sizeOf(field.store()) <= sizeof(uint64_t);
                if (appliesToAll) {
                    State::CopyPlan::Fill bits { field.offset, sizeOf(field.store()), 0 };
                    memcpy(&bits.bits, &copy->at<uint8_t>(field.offset), bits.size);
                    plan->fills.append(bits);
                }
                continue;
            }
            if (given.get(key)) {
                appliesToAll = appliesToAll && !(field.flags & fieldIsOutOfLine);
                plan->given.append({ key, static_cast<uint16_t>(2 * keys.find(key) + 1) });
                continue;
            }
            const FieldData* source = state.field(vm, layout(), state.name(vm, field.name).impl());
            appliesToAll = appliesToAll && source && !((source->flags | field.flags) & fieldIsOutOfLine);
            bool hasSameFieldType = appliesToAll && source->type() == field.type() && source->ownType() == field.ownType() && source->numberOfLiterals() == field.numberOfLiterals()
                && (!field.numberOfLiterals() || source->literalsOffset() == field.literalsOffset());
            if (hasSameFieldType && source->store() == field.store() && !((source->flags | field.flags) & fieldHoldsLazyClosure))
                plan->moves.append({ source->offset, field.offset, sizeOf(field.store()) });
            else if (hasSameFieldType)
                plan->conversions.append({ indexOf(*source), key });
            else if (appliesToAll)
                plan->checkedConversions.append({ indexOf(*source), key, state.kindsAcceptedBy(state.ownTypeOf(field)) });
        }
        if (appliesToAll) {
            for (UniquedStringImpl* atom : atoms)
                plan->excluded.append(atom);
            state.addCopyPlan(layout().shape, orderWord(), to.shape, WTF::move(plan));
        }
    }
    return copy;
}

Object* Object::tryCreateAs(JSGlobalObject* globalObject, State& state, const LayoutData& to, std::span<const EncodedJSValue> namesToStoreNext)
{
    VM& vm = globalObject->vm();
    if (to.kind != LayoutKind::Record || state.data.isCompiledForAudit)
        return nullptr;
    for (EncodedJSValue key : namesToStoreNext) {
        if (JSValue value = JSValue::decode(key); !value.isString() || asString(value)->isRope())
            return nullptr;
    }
    Object* made = create(vm, state.structureFor(vm, to.shape, globalObject->objectPrototype()), to, state.sizeWithSpecializationCapacity(to.shape), state.orderStartingWith(to, { }));
    BitVector created;
    Vector<uint16_t, 16> keys;
    for (EncodedJSValue key : namesToStoreNext) {
        const FieldData* field = state.field(vm, to, Identifier::fromString(vm, asString(JSValue::decode(key))->tryGetValue()).impl());
        if (!field || field->flags & fieldIsImmutable || made->isDiscriminant(*field))
            return nullptr;
        if (created.get(made->indexOf(*field)))
            continue;
        if (!made->read(*field) && !made->store(vm, *field, jsUndefined()))
            return nullptr;
        made->willCreateField(state, *field);
        keys.append(made->indexOf(*field));
        created.set(keys.last());
    }
    for (uint32_t i = 0; i < to.numberOfFields; ++i) {
        if (!(to.fields()[i].flags & fieldIsOptional) && !created.get(i))
            return nullptr;
    }
    if (made->hasExtraProperties())
        return nullptr;
    made->setOrderAndCount(state.orderStartingWith(to, keys.span()));
    state.countObject(made);
    return made;
}

Object* Object::tryMergeAs(JSGlobalObject* globalObject, const LayoutData& to, Object& first, Object& second)
{
    VM& vm = globalObject->vm();
    State& state = first.state();
    std::array<Object*, 2> sources { &first, &second };
    if (to.kind != LayoutKind::Record || state.data.isCompiledForAudit)
        return nullptr;
    for (Object* source : sources) {
        if (source->layout().kind != LayoutKind::Record || source->hasExtraProperties() || orderOf(source->orderWord()) == keysAreInLayoutOrder)
            return nullptr;
    }
    State::MergeKey key { first.layout().shape, first.orderWord(), second.layout().shape, second.orderWord(), to.shape };
    if (const State::MergePlan* plan = state.mergePlan(key)) {
        if (!plan->structure)
            return nullptr;
        DeferGC deferGC(vm);
        Object* merged = create(vm, plan->structure, to, plan->size, plan->orderWord);
        if (plan->lastOutOfLine)
            merged->ensureExtensionCovers(vm, state, *plan->lastOutOfLine);
        for (auto& move : plan->moves) {
            const void* from = &sources[move.source]->at<uint8_t>(move.from);
            void* slot = move.isToExtension ? merged->extension()->slot(move.to) : &merged->at<uint8_t>(move.to);
            switch (move.kind) {
            case State::MergePlan::MoveKind::Copy:
                memcpy(slot, from, move.size);
                break;
            case State::MergePlan::MoveKind::PurifiedDouble:
                *static_cast<double*>(slot) = purifyNaN(*static_cast<const double*>(from));
                break;
            case State::MergePlan::MoveKind::BoxedDouble:
                *static_cast<EncodedJSValue*>(slot) = JSValue::encode(jsNumber(purifyNaN(*static_cast<const double*>(from))));
                break;
            }
        }
        merged->copyNumberBoxes(state);
        for (auto& conversion : plan->conversions) {
            JSValue value = sources[conversion.source]->readInline(*conversion.from);
            if (!value)
                return nullptr;
            if (!conversion.alwaysMatches) {
                const RuntimeType& type = state.ownTypeOf(*conversion.to);
                if (!(value.isObject() ? state.matchesTypedSubtype(type, asObject(value)) : state.fits(type, value)))
                    return nullptr;
            }
            if (!merged->store(vm, *conversion.to, value))
                return nullptr;
        }
        if (merged->hasExtraProperties())
            return nullptr;
        vm.mutatorFence();
        state.countObject(merged);
        return merged;
    }
    Object* merged = create(vm, state.structureFor(vm, to.shape, globalObject->objectPrototype()), to, state.sizeWithSpecializationCapacity(to.shape), state.orderStartingWith(to, { }));
    BitVector created;
    Vector<uint16_t, 32> keys;
    Vector<uint16_t, 32> fieldSource(FillWith { }, to.numberOfFields, static_cast<uint16_t>(0));
    bool isInlinedBlock = true;
    for (uint16_t i = 0; i < sources.size() && isInlinedBlock; ++i) {
        sources[i]->forEachFieldAsProperty([&](UniquedStringImpl& name, JSValue value) {
            const FieldData* field = state.field(vm, to, &name);
            isInlinedBlock = field && !(field->flags & fieldIsImmutable) && !merged->isDiscriminant(*field);
            if (isInlinedBlock) {
                const RuntimeType& type = state.ownTypeOf(*field);
                isInlinedBlock = (value.isObject() ? state.matchesTypedSubtype(type, asObject(value)) : state.fits(type, value)) && merged->store(vm, *field, value);
            }
            if (isInlinedBlock) {
                uint16_t index = merged->indexOf(*field);
                if (!created.get(index)) {
                    merged->willCreateField(state, *field);
                    keys.append(index);
                    created.set(index);
                }
                fieldSource[index] = i;
            }
            return isInlinedBlock;
        }, SymbolKeys::Include);
    }
    for (uint32_t i = 0; i < to.numberOfFields && isInlinedBlock; ++i)
        isInlinedBlock = to.fields()[i].flags & fieldIsOptional || created.get(i);
    if (!isInlinedBlock || merged->hasExtraProperties()) {
        state.addMergePlan(key, makeUnique<State::MergePlan>());
        return nullptr;
    }
    merged->setOrderAndCount(state.orderStartingWith(to, keys.span()));
    state.countObject(merged);
    if (orderOf(merged->orderWord()) == keysAreInLayoutOrder || merged->structure() != state.structure(vm, to.shape))
        return merged;
    auto plan = makeUnique<State::MergePlan>();
    plan->structure = merged->structure();
    plan->size = state.sizeWithSpecializationCapacity(to.shape);
    plan->orderWord = merged->orderWord();
    for (uint16_t index : keys) {
        const FieldData& field = to.fields()[index];
        const FieldData* source = state.field(vm, sources[fieldSource[index]]->layout(), state.name(vm, field.name).impl());
        if (!source)
            return merged;
        bool isToExtension = field.flags & fieldIsOutOfLine;
        if (isToExtension && (!plan->lastOutOfLine || plan->lastOutOfLine->offset < field.offset))
            plan->lastOutOfLine = &field;
        bool hasSameType = source->type() == field.type() && source->ownType() == field.ownType() && source->numberOfLiterals() == field.numberOfLiterals() && (!field.numberOfLiterals() || source->literalsOffset() == field.literalsOffset());
        const RuntimeType& held = state.typeOf(*source);
        const RuntimeType& wanted = state.ownTypeOf(field);
        bool acceptsOnlyPrimitives = !held.allowsAnyObject() && !held.allowsAnyFunction() && !held.acceptsUntypedObjects() && !held.numberOfSupertypes() && !held.numberOfJSTypes();
        bool alwaysMatches = hasSameType || (acceptsOnlyPrimitives && !field.numberOfLiterals() && !(held.primitives & ~wanted.primitives) && (!(held.primitives & Primitive::Int) || (held.minInteger() >= wanted.minInteger() && held.maxInteger() <= wanted.maxInteger())));
        std::optional<State::MergePlan::MoveKind> kind;
        if (alwaysMatches && !(source->flags & (fieldIsOutOfLine | fieldHoldsLazyClosure)) && !(field.flags & (fieldIsPinned | fieldHoldsLazyClosure))) {
            if (source->store() == field.store())
                kind = State::MergePlan::MoveKind::Copy;
            else if (source->store() == Store::Double && field.store() == Store::OptionalDouble)
                kind = State::MergePlan::MoveKind::PurifiedDouble;
            else if (source->store() == Store::Double && field.store() == Store::JSValue)
                kind = State::MergePlan::MoveKind::BoxedDouble;
        }
        if (kind)
            plan->moves.append({ source->offset, field.offset, static_cast<uint8_t>(sizeOf(field.store())), static_cast<uint8_t>(fieldSource[index]), *kind, isToExtension });
        else
            plan->conversions.append({ source, &field, fieldSource[index], alwaysMatches });
    }
    state.addMergePlan(key, WTF::move(plan));
    return merged;
}

bool Object::tryCopyFieldsOf(VM& vm, const Object& source)
{
    State& state = this->state();
    if (state.data.isCompiledForAudit || this == &source || layout().kind != LayoutKind::Record || source.layout().kind != LayoutKind::Record || orderWord() & (orderWordIsFrozen | orderWordIsSealed | orderWordIsNotExtensible))
        return false;
    bool isInlinedBlock = true;
    bool hasOnlyFields = source.forEachFieldAsProperty([&](UniquedStringImpl& name, JSValue value) {
        const FieldData* field = state.field(vm, layout(), &name);
        isInlinedBlock = field && !isDiscriminant(*field);
        if (!isInlinedBlock)
            return false;
        bool wasPresent = !!read(*field);
        isInlinedBlock = write(vm, *field, value);
        if (isInlinedBlock && !wasPresent)
            didCreateField(state, *field);
        return isInlinedBlock;
    }, SymbolKeys::Include);
    return hasOnlyFields && isInlinedBlock;
}

} } // namespace AOT::Typed

static ALWAYS_INLINE const AOT::Typed::LayoutData* compiledLayoutOf(const Structure::AOTFields& aotFields, uint32_t firstCompiledEnvironmentShape, const JSCell* cell)
{
    if (auto* data = aotFields.compiledEnvironmentData())
        return &AOT::Typed::environmentLayout(*data, firstCompiledEnvironmentShape, cell);
    return aotFields.compiledLayout();
}

const AOT::Typed::LayoutData* Structure::compiledLayoutOf(const JSCell* cell) const
{
    return JSC::compiledLayoutOf(aotFieldsOrEmpty(), firstCompiledEnvironmentShape(), cell);
}

namespace AOT { namespace Typed {

const LayoutData& Object::layout() const
{
    return *compiledLayoutOf(structure()->existingAOTFields(), structure()->firstCompiledEnvironmentShape(), this);
}

State& Object::state() const
{
    return *structure()->existingAOTFields().aotInstance->typed;
}

static_assert(cell32Shift == MarkedBlockCage::shift);
static_assert(absentCell32 == JSValue::ValueEmpty && nullCell32 == JSValue::ValueNull && undefinedCell32 == JSValue::ValueUndefined);
static_assert(std::max({ JSValue::ValueEmpty, JSValue::ValueNull, JSValue::ValueFalse, JSValue::ValueTrue, JSValue::ValueUndefined }) <= UINT8_MAX);
static_assert(Closure::allocationSize(UINT8_MAX) <= MarkedSpace::largeCutoff);

static JSValue readAs(Store store, const void* address, State* state)
{
    auto as = [&]<typename T>(T) {
        return *static_cast<const T*>(address);
    };
    switch (store) {
    case Store::Boolean:
        return jsBoolean(as(uint8_t()));
    case Store::Uint8:
        return jsNumber(as(uint8_t()));
    case Store::Int8:
        return jsNumber(as(int8_t()));
    case Store::Uint16:
        return jsNumber(as(uint16_t()));
    case Store::Int16:
        return jsNumber(as(int16_t()));
    case Store::Int32:
        return jsNumber(as(int32_t()));
    case Store::Uint32:
        return jsNumber(as(uint32_t()));
    case Store::Int64:
        return jsNumber(static_cast<double>(as(int64_t())));
    case Store::Float:
        return jsNumber(purifyNaN(as(float())));
    case Store::Double:
        return jsNumber(purifyNaN(as(double())));
    case Store::Cell:
    case Store::JSValue:
        return JSValue::decode(as(EncodedJSValue()));
    case Store::Cell32:
    case Store::OptionalCell32: {
        uint32_t word = as(uint32_t());
        if (word == unmaterializedClosureCell32) [[unlikely]]
            return { };
        return word <= undefinedCell32 ? JSValue::decode(word) : JSValue(MarkedBlockCage::decode(word));
    }
    case Store::OptionalBoolean:
        return JSValue::decode(as(uint8_t()));
    case Store::LiteralString:
    case Store::LiteralString32: {
        uint64_t word = store == Store::LiteralString ? as(uint64_t()) : as(uint32_t());
        if (isLiteralStringWord(word))
            return state ? JSValue(state->stringForLiteralStringWord(word)) : jsBoolean(true);
        return store == Store::LiteralString || word <= undefinedCell32 ? JSValue::decode(word) : JSValue(MarkedBlockCage::decode(static_cast<uint32_t>(word)));
    }
    case Store::OptionalInt64:
        switch (int64_t integer = as(int64_t())) {
        case undefinedInteger:
            return jsUndefined();
        case nullInteger:
            return jsNull();
        case absentInteger:
            return { };
        default:
            return jsNumber(static_cast<double>(integer));
        }
    case Store::OptionalDouble:
        switch (as(uint64_t())) {
        case undefinedDouble:
            return jsUndefined();
        case nullDouble:
            return jsNull();
        case absentDouble:
            return { };
        default:
            return jsNumber(purifyNaN(as(double())));
        }
    case Store::Int32OrNumberBox:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool storeAs(State& state, Store store, void* address, JSValue value)
{
    auto keep = [&](auto bits) {
        *static_cast<decltype(bits)*>(address) = bits;
        return true;
    };
    auto integer = [&]<typename T>(int64_t low, int64_t high, T) {
        if (!value.isNumber())
            return false;
        double number = value.asNumber();
        if (!(number >= static_cast<double>(low) && number <= static_cast<double>(high)) || number != std::trunc(number))
            return false;
        return keep(static_cast<T>(static_cast<int64_t>(number)));
    };
    auto marker = [&](auto undefinedMarker, auto nullMarker) {
        return value.isUndefinedOrNull() && keep(value.isUndefined() ? undefinedMarker : nullMarker);
    };
    switch (store) {
    case Store::Boolean:
        return value.isBoolean() && keep(static_cast<uint8_t>(value.asBoolean()));
    case Store::Uint8:
        return integer(0, UINT8_MAX, uint8_t());
    case Store::Int8:
        return integer(INT8_MIN, INT8_MAX, int8_t());
    case Store::Uint16:
        return integer(0, UINT16_MAX, uint16_t());
    case Store::Int16:
        return integer(INT16_MIN, INT16_MAX, int16_t());
    case Store::Int32:
        return integer(INT32_MIN, INT32_MAX, int32_t());
    case Store::Uint32:
        return integer(0, UINT32_MAX, uint32_t());
    case Store::Int64:
        return integer(-maxExactInteger, maxExactInteger, int64_t());
    case Store::Float: {
        if (!value.isNumber())
            return false;
        double number = value.asNumber();
        if (number == number && static_cast<double>(static_cast<float>(number)) != number)
            return false;
        return keep(static_cast<float>(number));
    }
    case Store::Double:
        return value.isNumber() && keep(value.asNumber());
    case Store::OptionalInt64:
        return marker(undefinedInteger, nullInteger) || integer(-maxExactInteger, maxExactInteger, int64_t());
    case Store::OptionalDouble:
        return marker(undefinedDouble, nullDouble) || (value.isNumber() && keep(purifyNaN(value.asNumber())));
    case Store::Cell:
        if (!value.isCell())
            return false;
        [[fallthrough]];
    case Store::JSValue:
        return keep(JSValue::encode(value));
    case Store::OptionalCell32:
        if (!value || value.isUndefinedOrNull())
            return keep(static_cast<uint32_t>(JSValue::encode(value)));
        [[fallthrough]];
    case Store::Cell32:
        return value.isCell() && keep(MarkedBlockCage::encode(value.asCell()));
    case Store::OptionalBoolean:
        return (!value || value.isUndefinedOrNull() || value.isBoolean()) && keep(static_cast<uint8_t>(JSValue::encode(value)));
    case Store::LiteralString:
    case Store::LiteralString32: {
        if (value && !value.isUndefinedOrNull() && !value.isString())
            return false;
        uint64_t word = value && value.isString() ? state.literalStringWordFor(asString(value)) : static_cast<uint64_t>(JSValue::encode(value));
        if (store == Store::LiteralString)
            return keep(word);
        return keep(word < minCell32 ? static_cast<uint32_t>(word) : MarkedBlockCage::encode(asString(value)));
    }
    case Store::Int32OrNumberBox:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue Object::read(const FieldData& field) const
{
    if (field.store() != Store::Int32OrNumberBox) {
        const void* slot = slotOf(field);
        JSValue value = slot ? readAs(field.store(), slot, &state()) : JSValue();
        if (!value && field.flags & fieldMayExistWithoutStorage && isInKeyOrder(state(), field))
            return jsUndefined();
        if (!value && holdsUnmaterializedClosure(field))
            return const_cast<Object*>(this)->materializeLazyClosure(field);
        return value && isNotDefinedYet(field, value) ? JSValue() : value;
    }
    int32_t word = at<int32_t>(field.offset);
    if (refersToNumberBox(word))
        return jsNumber(purifyNaN(*numberBoxAt(word)));
    switch (word) {
    case absentInt32:
        return { };
    case undefinedInt32:
        return isNotDefinedYet(field, jsUndefined()) ? JSValue() : jsUndefined();
    case nullInt32:
        return jsNull();
    default:
        return jsNumber(word);
    }
}

void Object::materializeLazyClosures()
{
    const LayoutData& layout = this->layout();
    for (auto& field : std::span { layout.fields(), layout.numberOfFields }) {
        if (holdsUnmaterializedClosure(field))
            materializeLazyClosure(field);
    }
}

JSValue Object::materializeLazyClosure(const FieldData& field)
{
    VM& vm = this->vm();
    uint32_t function = field.literalsOffset() & ~lazyClosureCapturesObject;
    uint64_t captured = std::bit_cast<uint64_t>(this);
    bool captures = field.literalsOffset() & lazyClosureCapturesObject;
    SlimClosure* closure = SlimClosure::create(vm, state().slimClosureStructure(vm, function), function, std::span<const uint64_t> { &captured, captures ? 1u : 0u }, captures);
    bool didStore = storeAs(state(), field.store(), slotOf(field), closure);
    vm.writeBarrier(this, closure);
    RELEASE_ASSERT(didStore);
    return closure;
}

JSValue Object::tryGetOwnData(VM& vm, JSCell* cell, PropertyName name)
{
    const LayoutData* layout = cell->structure()->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return { };
    Object* object = uncheckedDowncast<Object>(cell);
    if (const FieldData* field = object->fieldNamed(object->state(), name))
        return object->read(*field);
    JSObject* extra = object->extraProperties();
    if (!extra)
        return { };
    unsigned attributes;
    JSValue value = extra->getDirect(vm, name, attributes);
    return value && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) ? value : JSValue();
}

bool Object::isDiscriminant(const FieldData& field) const
{
    const LayoutData& layout = this->layout();
    return layout.discriminant() != LayoutData::noBase && &field == layout.fields() + layout.discriminant();
}

bool Object::write(VM& vm, const FieldData& field, JSValue value)
{
    if (field.flags & fieldIsImmutable)
        return false;
    State& state = this->state();
    if (!state.fits(state.typeOf(field), value) || !store(vm, field, value))
        return false;
    if (field.flags & fieldMayBeUnlisted) [[unlikely]]
        listFieldStoredEarly(state, field);
    return true;
}

bool Object::assign(JSGlobalObject* globalObject, const FieldData& field, JSValue value, bool* givesUp)
{
    givesUp = layout().mayDespecialize ? givesUp : nullptr;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = this->state();
    if (isDiscriminant(field)) {
        JSValue current = read(field);
        if (current ? !(current.isNumber() && value.isNumber() && current.asNumber() == value.asNumber()) : !state.isAllowedLiteral(field, value)) {
            if (givesUp) {
                *givesUp = true;
                return false;
            }
            String message = makeString("Cannot assign to '"_s, StringView(state.name(vm, field.name).impl()), "' of "_s, state.nameOf(layout()), ": a discriminant property cannot change, got "_s, State::describe(value));
            bool goesOn = state.reportOrThrow(globalObject, { .kind = "discriminant-change"_s, .on = &layout(), .member = state.name(vm, field.name).impl(), .got = value }, message, State::InAudit::Performed);
            EXCEPTION_ASSERT(!!scope.exception() == !goesOn);
            if (!goesOn)
                return false;
        }
    }
    String failure;
    SetForScope onlyCopies(state.storesConvertPlainData, state.storesConvertPlainData && !(orderWord() & orderWordIsFrozen));
    JSValue held = field.flags & fieldIsImmutable ? JSValue() : state.coerceToFieldType(globalObject, field, value, failure);
    RETURN_IF_EXCEPTION(scope, false);
    if (!held || !store(vm, field, held)) {
        if (givesUp) {
            *givesUp = true;
            return false;
        }
        scope.release();
        state.reportOrThrow(globalObject, { .kind = "field-store-untyped"_s, .on = &layout(), .member = state.name(vm, field.name).impl(), .expected = &state.ownTypeOf(field), .got = value }, state.assignmentMessage(vm, makeString('\'', StringView(state.name(vm, field.name).impl()), "' of "_s, state.nameOf(layout())), field, value, failure), State::InAudit::Ignored);
        return false;
    }
    if (field.flags & fieldMayBeUnlisted) [[unlikely]]
        listFieldStoredEarly(state, field);
    return true;
}

bool Object::store(VM& vm, const FieldData& field, JSValue value)
{
    if (field.flags & fieldIsPinned) [[unlikely]]
        state().willModify(field, value);
    if (field.store() == Store::Int32OrNumberBox) {
        if (value.isUndefinedOrNull()) {
            at<int32_t>(field.offset) = value.isUndefined() ? undefinedInt32 : nullInt32;
            return true;
        }
        if (!value.isNumber())
            return false;
        double number = value.asNumber();
        if (number >= minInlineInt32 && number <= INT32_MAX && number == std::trunc(number) && !(!number && std::signbit(number)) && !refersToNumberBox(at<int32_t>(field.offset)))
            at<int32_t>(field.offset) = static_cast<int32_t>(number);
        else
            storeInNumberBox(vm, field, value);
        return true;
    }
    if (field.flags & fieldIsOutOfLine)
        ensureExtensionCovers(vm, state(), field);
    if (!storeAs(state(), field.store(), slotOf(field), value))
        return false;
    if (holdsCells(field.store()))
        vm.writeBarrier(this, value);
    return true;
}

void* Object::slotOf(const FieldData& field) const
{
    if (!(field.flags & fieldIsOutOfLine)) [[likely]]
        return &at<uint8_t>(field.offset);
    Extension* extension = this->extension();
    return extension && field.offset + sizeOf(field.store()) <= extension->size() ? extension->slot(field.offset) : nullptr;
}

void Object::ensureExtensionCovers(VM& vm, State& state, const FieldData& field)
{
    uint32_t needed = field.offset + sizeOf(field.store());
    Extension* current = extension();
    if (current && needed <= current->size())
        return;
    RELEASE_ASSERT(needed <= layout().extensionSize());
    replaceExtension(vm, state, needed);
}

void Object::ensureStorageOfSize(VM& vm, State& state, uint32_t size)
{
    if (Extension* current = extension(); !current || size > current->size())
        replaceExtension(vm, state, size);
}

void Object::replaceExtension(VM& vm, State& state, uint32_t size)
{
    Extension* replacement = Extension::create(vm, size);
    Extension* current = extension();
    if (current) {
        RELEASE_ASSERT(replacement->size() > current->size());
        memcpySpan(replacement->slots(), current->slots());
    }
    WTF::storeStoreFence();
    setExtension(replacement);
    vm.writeBarrier(this);
    state.countExtension(current ? current->size() : 0, replacement->size());
}

void Object::storeInNumberBox(VM&, const FieldData& field, JSValue number)
{
    RELEASE_ASSERT(field.store() == Store::Int32OrNumberBox && number.isNumber());
    if (int32_t word = at<int32_t>(field.offset); refersToNumberBox(word))
        *numberBoxAt(word) = number.asNumber();
    else
        storeInOwnNumberBox(state(), field.offset, number.asNumber());
}

void Object::storeInOwnNumberBox(State& state, uint32_t offset, double number)
{
    int32_t word = state.numberBoxes.wordFor(this, offset);
    *numberBoxAt(word) = number;
    at<int32_t>(offset) = word;
}

void Object::copyNumberBoxes(State& state)
{
    if (state.numberBoxes.isEmpty())
        return;
    for (auto& field : std::span { layout().fields(), layout().numberOfFields }) {
        if (field.store() != Store::Int32OrNumberBox)
            continue;
        if (int32_t word = at<int32_t>(field.offset); refersToNumberBox(word))
            storeInOwnNumberBox(state, field.offset, *numberBoxAt(word));
    }
}

static void clearSlot(void* slot, Store store)
{
    switch (store) {
    case Store::Cell:
    case Store::JSValue:
        *static_cast<EncodedJSValue*>(slot) = JSValue::encode(JSValue());
        return;
    case Store::LiteralString:
        *static_cast<uint64_t*>(slot) = absentCell32;
        return;
    case Store::Cell32:
    case Store::OptionalCell32:
    case Store::LiteralString32:
        *static_cast<uint32_t*>(slot) = absentCell32;
        return;
    case Store::OptionalBoolean:
        *static_cast<uint8_t*>(slot) = JSValue::ValueEmpty;
        return;
    case Store::OptionalInt64:
        *static_cast<int64_t*>(slot) = absentInteger;
        return;
    case Store::OptionalDouble:
        *static_cast<uint64_t*>(slot) = absentDouble;
        return;
    case Store::Int32OrNumberBox:
        *static_cast<int32_t*>(slot) = absentInt32;
        return;
    default:
        return;
    }
}

void Object::clear(const FieldData& field)
{
    if (void* slot = slotOf(field))
        clearSlot(slot, field.store());
}

template<typename Visitor>
void Object::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Object* object = uncheckedDowncast<Object>(cell);
    JSCell::visitChildren(cell, visitor);
    const LayoutData& layout = object->layout();
    if (uint32_t word = object->at<uint32_t>(offsetOfOrderWord); isObjectLayout(layout.kind) && word & orderWordHasExtraProperties && object->structure()->markingStampAndFlags() & Structure::isStructureWithExtraProperties) {
        if (uint32_t offset = layout.offsetOfExtraProperties())
            visitor.appendUnbarriered(MarkedBlockCage::decode(object->at<uint32_t>(offset)));
        else if (JSObject* extra = object->state().extraPropertiesHandleTable.extraPropertiesConcurrently(object, word & orderAndCountMask))
            visitor.appendUnbarriered(extra);
    }
    if (!layout.referencesOffset)
        return;
    const uint32_t* references = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(&layout) + layout.referencesOffset);
    std::span wide { references + 3, static_cast<size_t>(references[0]) + references[1] };
    for (uint32_t offset : wide)
        visitor.appendUnbarriered(JSValue::decode(object->at<EncodedJSValue>(offset)));
    std::span narrow { wide.data() + wide.size(), references[2] };
    for (uint32_t offset : narrow) {
        if (uint32_t word = object->at<uint32_t>(offset); word >= minCell32)
            visitor.appendUnbarriered(MarkedBlockCage::decode(word));
    }
    Extension* extension = layout.hasExtension ? object->extension() : nullptr;
    if (!extension)
        return;
    visitor.markAuxiliary(extension);
    uint32_t size = extension->size();
    const uint32_t* outOfLine = narrow.data() + narrow.size();
    std::span boxedOutOfLine { outOfLine + 2, outOfLine[0] };
    for (uint32_t offset : boxedOutOfLine) {
        if (offset + sizeof(EncodedJSValue) > size)
            break;
        visitor.appendUnbarriered(JSValue::decode(*static_cast<const EncodedJSValue*>(extension->slot(offset))));
    }
    for (std::span narrowOutOfLine { boxedOutOfLine.data() + boxedOutOfLine.size(), outOfLine[1] }; uint32_t offset : narrowOutOfLine) {
        if (offset + sizeof(uint32_t) > size)
            break;
        if (uint32_t word = *static_cast<const uint32_t*>(extension->slot(offset)); word >= minCell32)
            visitor.appendUnbarriered(MarkedBlockCage::decode(word));
    }
}

DEFINE_VISIT_CHILDREN(Object);

size_t Object::estimatedSize(JSCell* cell, VM&)
{
    return cell->cellSize();
}

void Object::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Object* object = uncheckedDowncast<Object>(cell);
    JSCell::analyzeHeap(cell, analyzer);
    State& state = object->state();
    const LayoutData& layout = object->layout();
    if (!isObjectLayout(layout.kind)) {
        for (auto& variable : layout.describedVariables()) {
            if (!holdsCells(variable.store()))
                continue;
            JSValue value = readAs(variable.store(), reinterpret_cast<const uint8_t*>(object) + variable.offset, nullptr);
            const Identifier& name = state.nameIfCreated(variable.name);
            if (value && value.isCell() && !name.isNull())
                analyzer.analyzeVariableNameEdge(object, value.asCell(), name.impl());
        }
        return;
    }
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        const FieldData& field = layout.fields()[i];
        if (!holdsCells(field.store()))
            continue;
        const void* slot = object->slotOf(field);
        JSValue value = slot ? readAs(field.store(), slot, nullptr) : JSValue();
        const Identifier& name = state.nameIfCreated(field.name);
        if (!value || !value.isCell() || name.isNull())
            continue;
        analyzer.analyzePropertyNameEdge(object, value.asCell(), name.impl());
    }
}

Extension* Extension::create(VM& vm, uint32_t size)
{
    RELEASE_ASSERT(size > offsetOfFirstExtensionSlot && size <= MarkedSpace::largeCutoff);
    void* storage = vm.auxiliarySpace().allocate(vm, size, nullptr, AllocationFailureMode::Assert);
    size_t cellSize = static_cast<HeapCell*>(storage)->cellSize();
    static_assert(minExtensionSize == MarkedBlock::atomSize);
    zeroSpan(std::span { static_cast<uint8_t*>(storage), cellSize });
    Extension* extension = static_cast<Extension*>(storage);
    extension->m_size = safeCast<uint32_t>(cellSize);
    vm.mutatorFence();
    return extension;
}

ExtraPropertiesTable::ExtraPropertiesTable(VM& vm)
    : m_vm(vm)
{
    vm.heap.registerWeakGCHashTable(this);
}

ExtraPropertiesTable::~ExtraPropertiesTable()
{
    m_vm.heap.unregisterWeakGCHashTable(this);
}

void ExtraPropertiesTable::set(Object* object, JSObject* extra)
{
    {
        Locker locker { m_lock };
        m_map.set(object, extra);
    }
    markDirty(m_vm);
}

void ExtraPropertiesTable::remove(Object* object)
{
    Locker locker { m_lock };
    m_map.remove(object);
}

static constexpr size_t handleSegmentSize = ExtraPropertiesHandleTable::handlesPerSegment * sizeof(ExtraPropertiesHandle);
static constexpr size_t handleSegmentTableSize = ((orderAndCountMask + 1) >> ExtraPropertiesHandleTable::segmentIndexBits) * sizeof(ExtraPropertiesHandle*);
static_assert(!(handleSegmentSize % (16 * KB)) && !(handleSegmentTableSize % (16 * KB)));
static constinit ExtraPropertiesHandle emptyHandle { nullptr, nullptr, 0, 0 };
static constinit ExtraPropertiesHandle* emptyHandleSegments[1] { &emptyHandle };

ExtraPropertiesHandleTable::ExtraPropertiesHandleTable(VM& vm)
    : m_vm(vm)
{
}

Ref<ExtraPropertiesHandleTable> ExtraPropertiesHandleTable::of(VM& vm, Instance& instance)
{
    for (Instance* other : vm.m_aotInstances) {
        if (!other->typed)
            continue;
        instance.typedExtraPropertiesHandleSegments = other->typedExtraPropertiesHandleSegments;
        return other->typed->vmExtraPropertiesHandleTable.copyRef();
    }
    instance.typedExtraPropertiesHandleSegments = emptyHandleSegments;
    return adoptRef(*new ExtraPropertiesHandleTable(vm));
}

ExtraPropertiesHandleTable::~ExtraPropertiesHandleTable()
{
    if (!m_segments)
        return;
    for (uint32_t number = 0; number < m_end; number += handlesPerSegment)
        OSAllocator::decommitAndRelease(m_segments[number >> segmentIndexBits], handleSegmentSize);
    OSAllocator::decommitAndRelease(m_segments, handleSegmentTableSize);
}

bool ExtraPropertiesHandleTable::tryGrow()
{
    static_assert(!(handlesPerSegment % handleGrowthIncrement));
    if (m_end > orderAndCountMask)
        return false;
    if (!m_segments) {
        m_segments = static_cast<ExtraPropertiesHandle**>(OSAllocator::tryReserveAndCommit(handleSegmentTableSize));
        if (!m_segments)
            return false;
    }
    if (!(m_end % handlesPerSegment)) {
        void* segment = OSAllocator::tryReserveAndCommit(handleSegmentSize);
        if (!segment)
            return false;
        m_segments[m_end >> segmentIndexBits] = static_cast<ExtraPropertiesHandle*>(segment);
        WTF::storeStoreFence();
        for (Instance* instance : m_vm.m_aotInstances) {
            if (instance->typed)
                instance->typedExtraPropertiesHandleSegments = m_segments;
        }
    }
    for (uint32_t number = m_end + handleGrowthIncrement; number-- > std::max(m_end, 1u);)
        release(nullptr, number);
    WTF::storeStoreFence();
    m_end += handleGrowthIncrement;
    return true;
}

uint32_t ExtraPropertiesHandleTable::take(const JSCell* object, JSObject* extraProperties, uint32_t orderAndCount)
{
    if (!hasFreeHandle() && !tryGrow()) [[unlikely]] {
        Locker locker { m_lock };
        m_unnumberedHandles.set(object, makeUniqueWithoutFastMallocCheck<ExtraPropertiesHandle>(ExtraPropertiesHandle { object, extraProperties, orderAndCount, 0 }));
        return 0;
    }
    uint32_t number = m_segments[0]->nextFree;
    m_segments[0]->nextFree = numbered(number)->nextFree;
    *numbered(number) = { object, extraProperties, orderAndCount, 0 };
    return number;
}

void ExtraPropertiesHandleTable::release(const JSCell* object, uint32_t number)
{
    if (!number) {
        Locker locker { m_lock };
        m_unnumberedHandles.remove(object);
        return;
    }
    *numbered(number) = { nullptr, nullptr, 0, m_segments[0]->nextFree };
    m_segments[0]->nextFree = number;
}

void ExtraPropertiesHandleTable::didReplaceObject(const JSCell* object, uint32_t number, const JSCell* itself)
{
    if (number) {
        numbered(number)->object = itself;
        return;
    }
    Locker locker { m_lock };
    auto handle = m_unnumberedHandles.take(object);
    handle->object = itself;
    m_unnumberedHandles.set(itself, WTF::move(handle));
}

JSObject* ExtraPropertiesHandleTable::extraPropertiesConcurrently(const JSCell* object, uint32_t number)
{
    if (number) {
        ExtraPropertiesHandle* handle = number < m_end ? numbered(number) : nullptr;
        return handle && handle->object == object ? handle->extraProperties : nullptr;
    }
    Locker locker { m_lock };
    ExtraPropertiesHandle* handle = m_unnumberedHandles.get(object);
    return handle ? handle->extraProperties : nullptr;
}

void ExtraPropertiesHandleTable::releaseHandlesOfDeadObjects(VM& vm)
{
    for (uint32_t number = 1; number < m_end; ++number) {
        if (const JSCell* object = numbered(number)->object; object && !vm.heap.isMarked(object))
            release(object, number);
    }
    if (m_unnumberedHandles.isEmpty()) [[likely]]
        return;
    Locker locker { m_lock };
    m_unnumberedHandles.removeIf([&](auto& entry) {
        return !vm.heap.isMarked(entry.key);
    });
}

void ExtraPropertiesTable::reconcileWeakReferencesAtGCEnd(VM& vm, CollectionScope)
{
    Locker locker { m_lock };
    m_map.removeIf([&](auto& entry) {
        return !vm.heap.isMarked(entry.key);
    });
}

class NumberBoxArena {
    WTF_MAKE_NONCOPYABLE(NumberBoxArena);
public:
    NumberBoxArena()
    {
        void* start = nullptr;
        for (unsigned i = 0; i < 8 && !start; ++i) {
            m_end = (static_cast<uint32_t>(minInlineInt32) - static_cast<uint32_t>(INT32_MIN)) >> i;
            if (static_cast<size_t>(m_end) << numberBoxShift <= addressSpaceToReserve() / addressSpaceDivisorForNumberBoxes)
                start = OSAllocator::tryReserveUncommitted(static_cast<size_t>(m_end) << numberBoxShift);
        }
        RELEASE_ASSERT(start);
        m_base = std::bit_cast<uintptr_t>(start);
    }

    static NumberBoxArena& singleton()
    {
        static LazyNeverDestroyed<NumberBoxArena> arena;
        static std::once_flag once;
        std::call_once(once, [] {
            arena.construct();
        });
        return arena;
    }

    uintptr_t base() const { return m_base; }

    uint32_t allocate()
    {
        Locker locker { m_lock };
        if (uint32_t place = m_firstFree) {
#if ASAN_ENABLED
            __asan_unpoison_memory_region(at(place), sizeof(uint64_t));
#endif
            m_firstFree = static_cast<uint32_t>(*at(place));
            return place;
        }
        if (m_next >= m_committed) {
            RELEASE_ASSERT(m_committed < m_end);
            uint32_t more = std::min(m_end - m_committed, entriesCommittedAtOnce);
            OSAllocator::commit(at(m_committed), static_cast<size_t>(more) << numberBoxShift, true, false);
#if ASAN_ENABLED
            __asan_poison_memory_region(at(m_committed), static_cast<size_t>(more) << numberBoxShift);
#endif
            m_committed += more;
        }
#if ASAN_ENABLED
        __asan_unpoison_memory_region(at(m_next), sizeof(uint64_t));
#endif
        return m_next++;
    }

    void free(uint32_t place)
    {
        Locker locker { m_lock };
        *at(place) = m_firstFree;
        m_firstFree = place;
#if ASAN_ENABLED
        __asan_poison_memory_region(at(place), sizeof(uint64_t));
#endif
    }

private:
    static constexpr uint32_t entriesCommittedAtOnce = 1 << 15;

    uint64_t* at(uint32_t place) const { return std::bit_cast<uint64_t*>(m_base + (static_cast<uintptr_t>(place) << numberBoxShift)); }

    Lock m_lock;
    uintptr_t m_base { 0 };
    uint32_t m_end { 0 };
    uint32_t m_committed { 0 };
    uint32_t m_next { static_cast<uint32_t>(nullInt32) - static_cast<uint32_t>(INT32_MIN) + 1 };
    uint32_t m_firstFree { 0 };
};

NumberBoxTable::NumberBoxTable(VM& vm)
    : m_vm(vm)
{
    vm.heap.registerWeakGCHashTable(this);
}

NumberBoxTable::~NumberBoxTable()
{
    m_vm.heap.unregisterWeakGCHashTable(this);
    for (auto& entry : m_map) {
        for (auto& box : entry.value)
            NumberBoxArena::singleton().free(box.place);
    }
}

uintptr_t NumberBoxTable::base()
{
    return NumberBoxArena::singleton().base();
}

int32_t NumberBoxTable::wordFor(Object* object, uint32_t offset)
{
    auto result = m_map.ensure(object, [] {
        return Vector<Box, 2> { };
    });
    if (result.isNewEntry)
        markDirty(m_vm);
    auto& boxes = result.iterator->value;
    size_t index = boxes.findIf([&](auto& box) {
        return box.offset == offset;
    });
    if (index == notFound) {
        index = boxes.size();
        boxes.append(Box { offset, NumberBoxArena::singleton().allocate() });
    }
    int32_t word = static_cast<int32_t>(static_cast<uint32_t>(INT32_MIN) + boxes[index].place);
    RELEASE_ASSERT(refersToNumberBox(word));
    return word;
}

void NumberBoxTable::reconcileWeakReferencesAtGCEnd(VM& vm, CollectionScope)
{
    m_map.removeIf([&](auto& entry) {
        if (vm.heap.isMarked(entry.key))
            return false;
        for (auto& box : entry.value)
            NumberBoxArena::singleton().free(box.place);
        return true;
    });
}

FieldAttributesTable::FieldAttributesTable(VM& vm)
    : m_vm(vm)
{
    vm.heap.registerWeakGCHashTable(this);
}

FieldAttributesTable::~FieldAttributesTable()
{
    m_vm.heap.unregisterWeakGCHashTable(this);
}

FieldAttributesTable::Attributes& FieldAttributesTable::ensure(Object* object)
{
    auto result = m_map.ensure(object, [&] {
        return makeUnique<Attributes>(Attributes { !!(object->orderWord() & orderWordIsFrozen), { } });
    });
    if (result.isNewEntry)
        markDirty(m_vm);
    return *result.iterator->value;
}

void FieldAttributesTable::reconcileWeakReferencesAtGCEnd(VM& vm, CollectionScope)
{
    m_map.removeIf([&](auto& entry) {
        return !vm.heap.isMarked(entry.key);
    });
}

bool Object::isFrozen() const
{
    if (!(orderWord() & orderWordIsFrozen))
        return false;
    auto* own = state().fieldAttributes.get(this);
    return !own || own->isFrozen;
}

unsigned Object::attributesOfSlow(const FieldData& field) const
{
    unsigned ofAll = static_cast<unsigned>(isSealed() ? PropertyAttribute::DontDelete : PropertyAttribute::None);
    auto* own = state().fieldAttributes.get(this);
    if (!own || own->isFrozen)
        ofAll = PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
    if (own) {
        for (auto& [name, attributes] : own->attributesByField) {
            if (name == field.name)
                return ofAll | attributes;
        }
    }
    return ofAll;
}

void Object::setAttributesOf(VM& vm, const FieldData& field, unsigned attributes)
{
    auto& own = state().fieldAttributes.ensure(this);
    own.attributesByField.removeFirstMatching([&](auto& entry) {
        return entry.first == field.name;
    });
    if (attributes)
        own.attributesByField.append({ field.name, static_cast<uint8_t>(attributes) });
    ensureExtraProperties(vm);
    at<uint32_t>(offsetOfOrderWord) |= orderWordIsFrozen;
}

JSObject* Object::extraPropertiesSlow() const
{
    Structure* structure = this->structure();
    if (!(structure->markingStampAndFlags() & Structure::isStructureWithExtraProperties))
        return nullptr;
    if (uint32_t offset = structure->compiledLayout()->offsetOfExtraProperties())
        return asObject(MarkedBlockCage::decode(at<uint32_t>(offset)));
    return state().extraPropertiesHandleTable.of(this, at<uint32_t>(offsetOfOrderWord) & orderAndCountMask)->extraProperties;
}

ExtraPropertiesHandle* Object::extraPropertiesHandle() const
{
    uint32_t word = at<uint32_t>(offsetOfOrderWord);
    if (!(word & orderWordHasExtraProperties))
        return nullptr;
    Structure* structure = this->structure();
    if (!(structure->markingStampAndFlags() & Structure::isStructureWithExtraProperties) || structure->compiledLayout()->offsetOfExtraProperties())
        return nullptr;
    return state().extraPropertiesHandleTable.of(this, word & orderAndCountMask);
}

uint32_t Object::orderWordSlow(uint32_t word) const
{
    ExtraPropertiesHandle* handle = extraPropertiesHandle();
    return handle ? (word & ~orderAndCountMask) | handle->orderAndCount : word;
}

void Object::setOrderAndCount(ExtraPropertiesHandle* handle, uint32_t orderAndCount)
{
    handle->orderAndCount = orderAndCount;
}

bool hasFieldNamedThen(Structure* structure)
{
    State& state = *structure->aotInstance()->typed;
    VM& vm = *state.instance.vm;
    return state.field(vm, *structure->compiledLayout(), vm.propertyNames->then.impl());
}

bool isClassPrototypeWithoutThen(JSObject* holder)
{
    return holder->classInfo() == ClassPrototype::info() && !ClassPrototype::hasMember(holder, holder->vm().propertyNames->then);
}

void countThenLookup(VM& vm)
{
    runtimeTable(vm).countOperation("a promise looks up then");
}

bool neverHasThen(Structure* structure)
{
    auto hasThenOrMayChangePrototype = [](Structure* current) {
        return current->hasSpecialProperties() || current->typeInfo().overridesGetPrototype() || !current->hasMonoProto() || current->isDictionary();
    };
    PropertyName then = structure->vm().propertyNames->then;
    if (hasThenOrMayChangePrototype(structure))
        return false;
    if (structure->compiledLayout() ? !Object::structureGuaranteesAbsence(structure, then.uid()) : structure->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence())
        return false;
    JSObject* objectPrototype = structure->realm()->objectPrototype();
    for (JSObject* holder = structure->storedPrototypeObject(); holder && holder != objectPrototype; holder = holder->structure()->storedPrototypeObject()) {
        Structure* current = holder->structure();
        if (hasThenOrMayChangePrototype(current))
            return false;
        if (current->classInfoForCells() == ClassPrototype::info()) {
            if (!ClassPrototype::willBeImmutable(holder) || ClassPrototype::hasMember(holder, then))
                return false;
        } else if (current->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence() || !current->hasImmutableProperties())
            return false;
    }
    return true;
}

JSObject* Object::ensureExtraProperties(VM& vm)
{
    if (JSObject* existing = extraProperties())
        return existing;
    JSObject* extra = constructEmptyObject(vm, structure()->realm()->nullPrototypeObjectStructure());
    setExtraProperties(vm, extra);
    return extra;
}

void Object::setExtraProperties(VM& vm, JSObject* extra, bool isForLookupTable)
{
    ASSERT(!extraProperties());
    Structure* withExtraProperties = isForLookupTable ? state().structureFor(vm, layout().shape, getPrototypeDirect(), *extra->structure()) : state().existingStructureFor(layout().shape, getPrototypeDirect(), *extra->structure());
    if (!withExtraProperties)
        withExtraProperties = state().structureFor(vm, layout().shape, getPrototypeDirect(), true);
    uint32_t word = at<uint32_t>(offsetOfOrderWord);
    if (uint32_t offset = layout().offsetOfExtraProperties())
        at<uint32_t>(offset) = MarkedBlockCage::encode(extra);
    else
        word = (word & ~orderAndCountMask) | state().extraPropertiesHandleTable.take(this, extra, word & orderAndCountMask);
    vm.mutatorFence();
    if (state().isCounting()) [[unlikely]]
        state().extraProperties.set(this, extra);
    at<uint32_t>(offsetOfOrderWord) = word | orderWordHasExtraProperties;
    vm.mutatorFence();
    JSCell::setStructure(vm, withExtraProperties);
    vm.writeBarrier(this, extra);
    state().countExtraPropertyTable(layout());
}

JSObject* Object::createExtraProperties(JSGlobalObject* realm, std::span<const ExtraProperty> inOrder)
{
    VM& vm = realm->vm();
    Structure* structure = realm->nullPrototypeObjectStructure();
    Vector<PropertyOffset, 16> offsets;
    for (auto& property : inOrder) {
        PropertyOffset offset = invalidOffset;
        structure = parseIndex(*property.name) ? nullptr : Structure::addPropertyTransitionToExistingStructure(structure, property.name, 0, offset);
        if (!structure)
            break;
        offsets.append(offset);
    }
    if (!structure) {
        JSObject* extra = constructEmptyObject(vm, realm->nullPrototypeObjectStructure());
        for (auto& property : inOrder) {
            if (auto index = parseIndex(*property.name))
                extra->putDirectIndex(realm, *index, property.value);
            else
                extra->putDirect(vm, property.name, property.value);
        }
        return extra;
    }
    Butterfly* butterfly = structure->outOfLineCapacity() ? Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0) : nullptr;
    for (size_t i = 0; i < offsets.size(); ++i) {
        if (!isInlineOffset(offsets[i]))
            butterfly->propertyStorage()[offsetInOutOfLineStorage(offsets[i])].setWithoutWriteBarrier(inOrder[i].value);
    }
    JSFinalObject* extra = JSFinalObject::createWithButterfly(vm, structure, butterfly);
    for (size_t i = 0; i < offsets.size(); ++i) {
        if (isInlineOffset(offsets[i]))
            extra->putDirectOffset(vm, offsets[i], inOrder[i].value);
    }
    return extra;
}

void Object::initializeExtraProperties(VM& vm, std::span<const ExtraProperty> inOrder)
{
    if (inOrder.empty())
        return;
    for (auto& property : inOrder)
        willAddExtraProperty(vm, property.name);
    setExtraProperties(vm, createExtraProperties(structure()->realm(), inOrder));
}

void Object::didChangeExtraProperties(VM& vm)
{
    JSObject* extra = extraProperties();
    Structure* structure = this->structure();
    StructureID known = structure->extraPropertiesStructureID();
    if (!extra || known == extra->structureID())
        return;
    State& state = *structure->aotInstance()->typed;
    uint32_t shape = structure->compiledLayout()->shape;
    Structure* now = state.existingStructureFor(shape, structure->storedPrototype(), *extra->structure());
    if (!now && !known)
        return;
    JSCell::setStructure(vm, now ? now : state.structureFor(vm, shape, structure->storedPrototype(), true));
}

void Object::specializeStructureForExtraProperties(VM& vm)
{
    JSObject* extra = extraProperties();
    if (!extra || structure()->extraPropertiesStructureID() == extra->structureID())
        return;
    Structure* now = state().structureFor(vm, layout().shape, getPrototypeDirect(), *extra->structure());
    if (now != structure())
        JSCell::setStructure(vm, now);
}

std::optional<PropertyOffset> Object::offsetOfExtraProperty(VM& vm, JSCell* cell, PropertyName name, unsigned& attributes)
{
    Structure* structure = cell->structure();
    const LayoutData* layout = structure->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return std::nullopt;
    StructureID ofExtraProperties = structure->extraPropertiesStructureID();
    Object* object = uncheckedDowncast<Object>(cell);
    if (!ofExtraProperties || parseIndex(name) || name.isPrivateName() || object->fieldNamed(object->state(), name))
        return std::nullopt;
    PropertyOffset offset = ofExtraProperties.decode()->get(vm, name, attributes);
    return isValidOffset(offset) ? std::optional(offset) : std::nullopt;
}

Object::ExtraPropertyStore Object::tryStoreExtraProperty(VM& vm, PropertyName name, JSValue value, PropertyOffset& offset, bool isKnownNotToBeField, bool isDefinition, bool* hasDefaultAttributes, bool isForLookupTable)
{
    State& state = this->state();
    const LayoutData& layout = this->layout();
    if (state.data.isCompiledForAudit || state.isCounting() || (!isKnownNotToBeField && fieldNamed(state, name)))
        return ExtraPropertyStore::NotHandled;
    unsigned attributes = 0;
    JSObject* extra = extraProperties();
    offset = extra ? extra->structure()->get(vm, name, attributes) : invalidOffset;
    if (!isValidOffset(offset) && (parseIndex(name) || name.isPrivateName() || name == vm.propertyNames->underscoreProto || state.hasImmutableMethod(layout, name.uid())))
        return ExtraPropertyStore::NotHandled;
    if (isValidOffset(offset)) {
        if (attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue) || (isDefinition && attributes))
            return ExtraPropertyStore::NotHandled;
        if (hasDefaultAttributes)
            *hasDefaultAttributes = !attributes;
        extra->putDirectOffset(vm, offset, value);
        return ExtraPropertyStore::Replaced;
    }
    if (isNotExtensible() || !state.storeNeverSpecializes(layout, name.uid()))
        return ExtraPropertyStore::NotHandled;
    for (JSValue next = isDefinition ? jsNull() : getPrototypeDirect(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        JSObject* holder = asObject(next);
        ClassPrototype::look(holder);
        Structure* structure = holder->structure();
        TypeInfo typeInfo = structure->typeInfo();
        if (typeInfo.overridesGetOwnPropertySlot() || (typeInfo.overridesPut() && structure->classInfoForCells() != PrototypeObject::info()) || typeInfo.overridesGetPrototype() || holder->hasNonReifiedStaticProperties())
            return ExtraPropertyStore::NotHandled;
        if (!holder->getDirect(vm, name, attributes))
            continue;
        if (attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue))
            return ExtraPropertyStore::NotHandled;
        break;
    }
    willAddExtraProperty(vm, name);
    bool hadNone = !extra;
    if (hadNone)
        extra = constructEmptyObject(vm, structure()->realm()->nullPrototypeObjectStructure());
    extra->putDirect(vm, name, value);
    if (hadNone)
        setExtraProperties(vm, extra, isForLookupTable);
    else if (isForLookupTable)
        specializeStructureForExtraProperties(vm);
    else
        didChangeExtraProperties(vm);
    offset = extra->structure()->get(vm, name);
    return ExtraPropertyStore::Added;
}

bool Object::mayDefineAccessorOnExtraProperties(VM& vm, State& state, PropertyName name, JSObject*& extra)
{
    if (fieldNamed(state, name) || parseIndex(name) || isNotExtensible() || state.hasImmutableMethod(layout(), name.uid()))
        return false;
    if (!extra)
        extra = ensureExtraProperties(vm);
    if (!isValidOffset(extra->structure()->get(vm, name)))
        willAddExtraProperty(vm, name);
    return true;
}

void Object::willAddExtraProperty(VM& vm, PropertyName name)
{
    if (name == vm.propertyNames->isConcatSpreadableSymbol) [[unlikely]]
        realm()->arrayIsConcatSpreadableWatchpointSet().fireAll(vm, "An object of the program has Symbol.isConcatSpreadable");
    state().countExtraProperty(layout(), name);
}

JSFinalObject* Object::copyAsOrdinaryObject(VM& vm, unsigned inlineCapacity, JSValue prototype)
{
    State& state = this->state();
    const LayoutData& layout = this->layout();
    JSGlobalObject* realm = structure()->realm();
    if (!prototype)
        prototype = getPrototypeDirect();
    Structure* empty = prototype.isObject() ? realm->structureCache().emptyObjectStructureForPrototype(realm, asObject(prototype), inlineCapacity) : JSFinalObject::createStructure(vm, realm, jsNull(), inlineCapacity);
    JSFinalObject* ordinary = JSFinalObject::create(vm, empty);
    JSObject* extra = extraProperties();
    auto copyExtra = [&](PropertyName name) {
        unsigned attributes;
        JSValue value = extra->getDirect(vm, name, attributes);
        if (!value || ordinary->getDirect(vm, name))
            return;
        if (attributes & PropertyAttribute::Accessor)
            ordinary->putDirectNonIndexAccessor(vm, name, uncheckedDowncast<GetterSetter>(value), attributes);
        else
            ordinary->putDirect(vm, name, value, attributes);
    };
    Vector<uint16_t, 16> keys;
    forEachKeyInOrder(state, [&](uint16_t key) {
        keys.append(key);
    });
    const FieldData* fields = layout.fields();
    auto extraKeyNames = this->extraKeyNames();
    size_t nextExtraKey = 0;
    for (uint16_t key : keys) {
        if (key >= layout.numberOfFields) {
            for (size_t end = std::min<size_t>(key - layout.numberOfFields + 1, extraKeyNames.size()); nextExtraKey < end; ++nextExtraKey)
                copyExtra(extraKeyNames[nextExtraKey]);
        } else if (JSValue value = read(fields[key]))
            ordinary->putDirect(vm, state.name(vm, fields[key].name), value, attributesOf(fields[key]));
    }
    if (extra) {
        Vector<RefPtr<UniquedStringImpl>, 8> names;
        extra->structure()->forEachProperty(vm, [&](const auto& entry) {
            names.append(entry.key());
            return true;
        });
        for (auto& name : names)
            copyExtra(name.get());
        PropertyNameArrayBuilder indices(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        extra->getOwnIndexedPropertyNames(realm, indices, DontEnumPropertiesMode::Include);
        for (auto& index : indices)
            ordinary->putDirectIndex(realm, *parseIndex(index), extra->getDirectIndex(realm, *parseIndex(index)));
    }
    if (isNotExtensible())
        ordinary->methodTable()->preventExtensions(ordinary, realm);
    return ordinary;
}

JSObject* Object::convertToOrdinaryObject(VM& vm, JSValue prototype)
{
    State& state = this->state();
    RELEASE_ASSERT(layout().mayDespecialize);
    static_assert(sizeof(JSObjectWithButterfly) == offsetOfFirstField && JSObjectWithButterfly::butterflyOffset() == offsetOfOrderWord);
    unsigned inlineCapacity = std::min<size_t>((cellSize() - offsetOfFirstField) / sizeof(EncodedJSValue), JSFinalObject::maxInlineCapacity);
    JSFinalObject* ordinary = copyAsOrdinaryObject(vm, inlineCapacity, prototype);
    if (state.isPrototypeShape(layout().shape))
        ordinary->didBecomePrototype(vm);
    RELEASE_ASSERT(ordinary->structure()->inlineCapacity() == inlineCapacity);

    bool hasHandle = extraPropertiesHandle();
    uint32_t number = at<uint32_t>(offsetOfOrderWord) & orderAndCountMask;
    if (extraProperties())
        state.removeExtraPropertiesOf(this);
    if (orderWord() & orderWordIsFrozen)
        state.fieldAttributes.remove(this);
    {
        PreventCollectionScope preventCollection(vm.heap);
        for (unsigned i = 0; i < inlineCapacity; ++i)
            at<EncodedJSValue>(offsetOfFirstField + i * sizeof(EncodedJSValue)) = JSValue::encode(ordinary->inlineStorage()[i].get());
        at<Butterfly*>(offsetOfOrderWord) = ordinary->butterfly();
        JSCell::setStructure(vm, ordinary->structure());
    }
    if (hasHandle)
        state.extraPropertiesHandleTable.release(this, number);
    vm.writeBarrier(this);
    ensureStillAliveHere(ordinary);
    return this;
}

void Object::dropExtraPropertiesIfEmpty(VM& vm)
{
    JSObject* extra = extraProperties();
    if (!extra)
        return;
    bool isEmpty = !hasIndexedProperties(extra->indexingType()) && !(orderWord() & orderWordIsFrozen && state().fieldAttributes.get(this));
    if (isEmpty) {
        extra->structure()->forEachProperty(vm, [&](const auto&) {
            isEmpty = false;
            return false;
        });
    }
    if (!isEmpty) {
        didChangeExtraProperties(vm);
        return;
    }
    Structure* withoutExtraProperties = state().structureFor(vm, layout().shape, getPrototypeDirect(), false);
    uint32_t word = orderWord();
    bool hasHandle = extraPropertiesHandle();
    uint32_t number = at<uint32_t>(offsetOfOrderWord) & orderAndCountMask;
    state().removeExtraPropertiesOf(this);
    JSCell::setStructure(vm, withoutExtraProperties);
    at<uint32_t>(offsetOfOrderWord) = word & ~orderWordHasExtraProperties;
    if (hasHandle)
        state().extraPropertiesHandleTable.release(this, number);
    updateNeedsSlowLookup(state());
}

bool State::descendantsAddNoProperties(JSObject* prototype, JSObject* classPrototype)
{
    static constexpr unsigned maxDescendants = 8;
    if (auto known = m_classPrototypeByDescendantPrototype.find(prototype); known != m_classPrototypeByDescendantPrototype.end())
        return known->value == classPrototype;
    VM& vm = *instance.vm;
    Vector<JSObject*, maxDescendants> descendants;
    bool addNoProperties = true;
    for (JSObject* next = prototype; addNoProperties && next != classPrototype; next = next->getPrototypeDirect().getObject()) {
        if (next)
            ClassPrototype::look(next);
        Structure* structure = next ? next->structure() : nullptr;
        if (!next || descendants.size() == maxDescendants || next->type() != FinalObjectType || structure->isDictionary() || structure->hasPolyProto() || hasIndexedProperties(next->indexingType()) || next->hasNonReifiedStaticProperties() || m_modifiedDescendantPrototypes.contains(next)) {
            addNoProperties = false;
            break;
        }
        JSValue constructor = next->getDirect(vm, vm.propertyNames->constructor);
        auto* classObject = constructor ? dynamicDowncast<Closure>(constructor) : nullptr;
        const LayoutData* descendantLayout = classObject && classObject->structure()->aotInstance() == &instance ? classObject->classData() : nullptr;
        if (descendantLayout && classPrototypeIfExists(descendantLayout->index) != next)
            descendantLayout = nullptr;
        structure->forEachProperty(vm, [&](const auto& entry) {
            addNoProperties = entry.key() == vm.propertyNames->constructor.impl() || (descendantLayout && !entry.key()->isSymbol() && std::ranges::any_of(descendantLayout->methods(), [&](const MethodData& method) {
                return !(method.flags & (methodHasComputedName | methodNameIsAssumedAbsent)) && isName(method.name, entry.key());
            }));
            return addNoProperties;
        });
        descendants.append(next);
    }
    m_classPrototypeByDescendantPrototype.add(prototype, addNoProperties ? classPrototype : nullptr);
    if (addNoProperties) {
        for (JSObject* heir : descendants)
            m_descendantPrototypes.add(heir);
    }
    return addNoProperties;
}

void State::didChangeDescendantPrototype(VM& vm, JSObject* changed, UniquedStringImpl* nameOfProperty)
{
    if (!m_descendantPrototypes.contains(changed) || nameOfProperty == vm.propertyNames->constructor.impl())
        return;
    m_modifiedDescendantPrototypes.add(changed);
    m_descendantPrototypes.clear();
    m_classPrototypeByDescendantPrototype.clear();
    forgetInitialObjectStates();
    revalidatePrototypes(vm, [&](Structure*, const LayoutData& layout) {
        return layout.kind == LayoutKind::Instance && layout.mayHaveOtherPrototype;
    });
}

void State::revalidatePrototypes(VM& vm, const ScopedLambda<bool(Structure*, const LayoutData&)>& shouldRevalidate)
{
    instance.forgetPrototypeChanges();
    DeferGC deferGC(vm);
    PreventCollectionScope preventCollection(vm.heap);
    Vector<Structure*> structures;
    Vector<Object*> objects;
    {
        HeapIterationScope iterationScope(vm.heap);
        vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* heapCell, HeapCell::Kind kind) {
            if (!isJSCellKind(kind))
                return IterationStatus::Continue;
            auto* cell = static_cast<JSCell*>(heapCell);
            if (cell->type() == StructureType) {
                auto* structure = uncheckedDowncast<Structure>(cell);
                const LayoutData* layout = structure->aotInstance() == &instance ? structure->compiledLayout() : nullptr;
                if (layout && isObjectLayout(layout->kind) && structure->markingStampAndFlags() & Structure::isStructureWithExtraProperties)
                    structures.append(structure);
                return IterationStatus::Continue;
            }
            Structure* structure = cell->structure();
            const LayoutData* layout = structure->compiledLayout();
            if (layout && isObjectLayout(layout->kind) && shouldRevalidate(structure, *layout))
                objects.append(uncheckedDowncast<Object>(cell));
            return IterationStatus::Continue;
        });
    }
    for (Structure* structure : structures)
        updatePrototypeMatchesLayout(structure);
    for (Object* object : objects)
        object->updateNeedsSlowLookup(*this);
}

void State::removeDeadDescendantPrototypes(VM& vm)
{
    auto isDead = [&](JSObject* object) {
        return !vm.heap.isMarked(object);
    };
    m_classPrototypeByDescendantPrototype.removeIf([&](auto& entry) {
        return isDead(entry.key) || (entry.value && isDead(entry.value));
    });
    m_descendantPrototypes.removeIf(isDead);
    m_modifiedDescendantPrototypes.removeIf(isDead);
}

bool State::prototypeMatchesLayout(const LayoutData& layout, JSValue prototype)
{
    if (layout.kind == LayoutKind::Instance) {
        JSObject* classPrototype = classPrototypeIfExists(layout.index);
        return prototype == JSValue(classPrototype) || (classPrototype && prototype.isObject() && descendantsAddNoProperties(asObject(prototype), classPrototype));
    }
    auto* functionPrototypeObject = dynamicDowncast<PrototypeObject>(prototype);
    if (prototype.isNull() || prototype == JSValue(instance.globalObject->objectPrototype()) || (functionPrototypeObject && (layout.mayBeConstructed || !(layout.mayHaveOtherPrototype || layout.mayDespecialize)) && functionPrototypeObject->isGuarded() && !functionPrototypeObject->isModified()))
        return true;
    return std::ranges::all_of(std::span { layout.fields(), layout.numberOfFields }, [](const FieldData& field) {
        return field.flags & fieldIsNeverObservedAbsent;
    });
}

void State::updatePrototypeMatchesLayout(Structure* structure)
{
    const LayoutData& layout = *structure->compiledLayout();
    if (!layout.mayHaveOtherPrototype || prototypeMatchesLayout(layout, structure->storedPrototype()))
        structure->markingStampAndFlags() |= Structure::hasPrototypeOfLayout;
    else
        structure->markingStampAndFlags() &= ~Structure::hasPrototypeOfLayout;
}

void Object::updateNeedsSlowLookup(State& state, bool isCreation)
{
    if (!layout().mayHaveOtherPrototype)
        return;
    if (!state.prototypeMatchesLayout(layout(), getPrototypeDirect()))
        at<uint32_t>(offsetOfOrderWord) |= orderWordHasExtraProperties;
    else if (!isCreation && !extraProperties())
        at<uint32_t>(offsetOfOrderWord) &= ~orderWordHasExtraProperties;
}

void State::didModifyPrototypeObject(VM& vm, PrototypeObject* prototype)
{
    for (uint32_t i = 0; i < data.numberOfAllocations; ++i) {
        Instance::TypedAllocation* row = instance.typedAllocation(i);
        if (row && std::bit_cast<StructureID>(static_cast<uint32_t>(row->header)).decode()->storedPrototype() == JSValue(prototype))
            row->words |= orderWordHasExtraProperties;
    }
    revalidatePrototypes(vm, [&](Structure* structure, const LayoutData&) {
        return structure->storedPrototype() == JSValue(prototype);
    });
}

bool Object::structureGuaranteesAbsence(Structure* structure, UniquedStringImpl* name)
{
    const LayoutData* layout = structure->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return false;
    if (parseIndex(*name) || (name->isSymbol() && static_cast<SymbolImpl*>(name)->isPrivate()))
        return false;
    State& state = *structure->aotInstance()->typed;
    VM& vm = *state.instance.vm;
    if (state.field(vm, *layout, name))
        return false;
    if (state.isStructureWithoutExtraProperties(structure))
        return true;
    StructureID ofExtraProperties = structure->extraPropertiesStructureID();
    return ofExtraProperties && !isValidOffset(ofExtraProperties.decode()->get(vm, name));
}

std::optional<Object::FieldAccess> Object::fieldAccessFor(VM&, PropertyName name, bool acceptsAnyMatchingValue)
{
    State& state = this->state();
    const FieldData* field = fieldNamed(state, name);
    if (!field)
        return std::nullopt;
    FieldAccess access { field->offset, field->store(), !!(field->flags & fieldIsOutOfLine), state.mayBeAbsent(*field) };
    if (field->flags & (fieldIsImmutable | fieldIsPinned | fieldMayBeUnlisted) || isDiscriminant(*field) || state.data.isCompiledForAudit)
        return access;
    const RuntimeType& type = state.ownTypeOf(*field);
    access.acceptedPrimitives = type.primitives & (Primitive::All & ~Primitive::Int);
    if (type.primitives & Primitive::Int) {
        access.acceptsInt32 = type.minInteger() <= INT32_MIN && type.maxInteger() >= INT32_MAX;
        if (type.minInteger() <= -maxTrackedInteger && type.maxInteger() >= maxTrackedInteger)
            access.acceptedPrimitives |= Primitive::Int;
    }
    access.acceptsAnyObject = type.allowsAnyObject() && (acceptsAnyMatchingValue || !type.numberOfSupertypes());
    if (!type.allowsAnyObject() && !(field->ownType() & 3) && (acceptsAnyMatchingValue || state.subtypesHaveNoSpecializations(type)))
        access.typeOfAcceptedObjects = field->ownType();
    return access;
}

const FieldData* Object::fieldNamed(State& state, PropertyName name) const
{
    return state.field(*state.instance.vm, layout(), name.uid());
}

void Object::forEachFieldInKeyOrder(State& state, const Invocable<void(uint16_t)> auto& functor) const
{
    uint32_t numberOfFields = layout().numberOfFields;
    forEachListedKey(state, [&](uint16_t entry) {
        if (entry < numberOfFields)
            functor(entry);
    });
}

bool Object::forEachFieldAsProperty(const ScopedLambda<bool(UniquedStringImpl&, JSValue)>& functor, SymbolKeys symbolKeys) const
{
    if (hasExtraProperties())
        return false;
    State& state = this->state();
    VM& vm = *state.instance.vm;
    if (auto properties = state.propertiesInOrder(vm, layout(), orderWord() & orderAndCountMask, symbolKeys)) {
        for (auto& property : *properties) {
            JSValue value = readInline(*property.field);
            if (!value) [[unlikely]]
                value = read(*property.field);
            if (value && !functor(*property.name, value))
                break;
        }
        return true;
    }
    const FieldData* fields = layout().fields();
    bool goesOn = true;
    forEachFieldInKeyOrder(state, [&](uint16_t index) {
        if (!goesOn || state.isPrivate(fields[index]) || (symbolKeys == SymbolKeys::Skip && state.hasSymbolKey(fields[index])))
            return;
        if (JSValue value = read(fields[index]))
            goesOn = functor(*state.name(vm, fields[index].name).impl(), value);
    });
    return true;
}

CachedPropertyList* Object::cachedPropertyList(VM& vm) const
{
    JSObject* extra = extraProperties();
    if (!extra || orderWord() & orderWordIsFrozen)
        return nullptr;
    State& state = this->state();
    if (state.hasFieldsWithSymbolKeys(layout()))
        return nullptr;
    Structure* structure = extra->structure();
    uint32_t shapeWord = shapeWordOf(shape());
    uint32_t orderAndCount = orderWord() & orderAndCountMask;
    auto& kept = state.propertyListCacheEntry(structure->id(), shapeWord, orderAndCount);
    if (kept.extraPropertiesStructureID == structure->id() && kept.shapeWord == shapeWord && kept.orderAndCount == orderAndCount)
        return &kept;
    if (extra->type() != FinalObjectType || structure->isDictionary() || !structure->isQuickPropertyAccessAllowedForEnumeration() || !structure->canPerformFastPropertyEnumeration() || structure->hasNonReifiedStaticProperties())
        return nullptr;
    const LayoutData& layout = this->layout();
    if (orderOf(orderAndCount) == keysAreInLayoutOrder || state.leavesFieldsUnlisted(layout, orderAndCount))
        return nullptr;
    const FieldData* fields = layout.fields();
    Vector<PropertyListEntry, 16> extraKeys;
    structure->forEachProperty(vm, [&](const auto& entry) {
        extraKeys.append(PropertyListEntry { entry.key(), nullptr, entry.offset() });
        return true;
    });
    Vector<PropertyListEntry> properties;
    size_t nextExtraKey = 0;
    bool isWhole = true;
    forEachListedKey(state, [&](uint16_t entry) {
        if (entry >= layout.numberOfFields) {
            for (size_t end = std::min<size_t>(entry - layout.numberOfFields + 1, extraKeys.size()); nextExtraKey < end; ++nextExtraKey)
                properties.append(extraKeys[nextExtraKey]);
            return;
        }
        if (state.isPrivate(fields[entry]))
            return;
        isWhole &= !!read(fields[entry]);
        properties.append(PropertyListEntry { state.name(vm, fields[entry].name).impl(), &fields[entry], invalidOffset });
    });
    if (!isWhole)
        return nullptr;
    for (; nextExtraKey < extraKeys.size(); ++nextExtraKey)
        properties.append(extraKeys[nextExtraKey]);
    auto& place = state.propertyListCacheEntry(structure->id(), shapeWord, orderAndCount);
    place = { structure->id(), shapeWord, orderAndCount, false, nullptr, nullptr, WTF::move(properties) };
    return &place;
}

ALWAYS_INLINE JSValue Object::readPropertyListEntry(const PropertyListEntry& property, JSObject* extra) const
{
    if (!property.field)
        return extra->getDirect(property.offsetInExtraProperties);
    JSValue value = readInline(*property.field);
    return value ? value : read(*property.field);
}

JSFinalObject* Object::tryCopyUsingPropertyList(VM& vm, Structure* plainCopyPrefixStructure) const
{
    DeferGC deferGC(vm);
    auto* listing = cachedPropertyList(vm);
    Structure* structure = !listing ? nullptr : plainCopyPrefixStructure ? plainCopyPrefixStructure : state().plainCopyStructure(vm, *listing);
    if (!structure)
        return nullptr;
    JSObject* extra = extraProperties();
    JSFinalObject* copy = JSFinalObject::create(vm, structure);
    auto* slot = copy->inlineStorage();
    for (auto& property : listing->properties)
        (slot++)->setWithoutWriteBarrier(readPropertyListEntry(property, extra));
    return copy;
}

bool Object::appendPlainCopyValues(VM& vm, Vector<JSValue, 16>& values) const
{
    if (hasExtraProperties()) {
        DeferGC deferGC(vm);
        auto* listing = cachedPropertyList(vm);
        if (!listing)
            return false;
        JSObject* extra = extraProperties();
        for (auto& property : listing->properties)
            values.append(readPropertyListEntry(property, extra));
        return true;
    }
    for (auto& property : state().plainCopyPlan(vm, layout(), orderWord() & orderAndCountMask).properties) {
        JSValue value = readInline(*property.field);
        if (!value)
            return false;
        values.append(value);
    }
    return true;
}

bool Object::keysDependOnlyOnStructureAndOrderWord() const
{
    return !hasExtraProperties() || (structure()->extraPropertiesStructureID() && layout().offsetOfExtraProperties());
}

JSCellButterfly* Object::keysWithExtraProperties(VM& vm) const
{
    DeferGC deferGC(vm);
    auto* listing = cachedPropertyList(vm);
    if (!listing || listing->keys)
        return listing ? listing->keys : nullptr;
    JSCellButterfly* list = JSCellButterfly::tryCreate(vm, CopyOnWriteArrayWithContiguous, listing->properties.size());
    if (!list)
        return nullptr;
    for (unsigned i = 0; i < listing->properties.size(); ++i)
        list->setIndex(vm, i, jsOwnedString(vm, listing->properties[i].name));
    listing->keys = list;
    return list;
}

JSArray* Object::tryCreateKeysArrayWithExtraProperties(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    DeferGC deferGC(vm);
    JSCellButterfly* keys = keysWithExtraProperties(vm);
    if (!keys)
        return nullptr;
    if (keysDependOnlyOnStructureAndOrderWord())
        return JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(keys->indexingMode()), keys->toButterfly());
    unsigned length = keys->length();
    JSArray* list = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), length);
    if (!list)
        return nullptr;
    auto values = list->butterfly()->contiguous();
    for (unsigned i = 0; i < length; ++i)
        values.at(list, i).setWithoutWriteBarrier(keys->get(i));
    return list;
}

bool Object::forEachFieldAndExtraProperty(const ScopedLambda<bool(UniquedStringImpl&, JSValue)>& functor) const
{
    bool mayHideFields = orderWord() & orderWordIsFrozen;
    State& state = this->state();
    VM& vm = *state.instance.vm;
    JSObject* extra = extraProperties();
    Structure* extraPropertiesStructure = extra ? extra->structure() : nullptr;
    if (extra && (extra->type() != FinalObjectType || !extraPropertiesStructure->canPerformFastPropertyEnumeration() || extraPropertiesStructure->hasNonReifiedStaticProperties() || extra->getDirect(vm, vm.propertyNames->toJSON)))
        return false;
    if (auto* listing = cachedPropertyList(vm)) {
        Vector<PropertyListEntry, 16> properties(listing->properties.span());
        for (auto& property : properties) {
            if (!functor(*property.name, readPropertyListEntry(property, extra)))
                break;
        }
        return true;
    }
    const LayoutData& layout = this->layout();
    const FieldData* fields = layout.fields();
    auto extraKeyNames = this->extraKeyNames();
    size_t nextExtraKey = 0;
    bool goesOn = true;
    forEachListedKey(state, [&](uint16_t entry) {
        if (!goesOn)
            return;
        if (entry >= layout.numberOfFields) {
            for (size_t end = std::min<size_t>(entry - layout.numberOfFields + 1, extraKeyNames.size()); goesOn && nextExtraKey < end; ++nextExtraKey) {
                UniquedStringImpl* name = extraKeyNames[nextExtraKey];
                unsigned attributes;
                JSValue value = extra->getDirect(vm, name, attributes);
                if (value && !name->isSymbol() && !(attributes & PropertyAttribute::DontEnum))
                    goesOn = functor(*name, value);
            }
            return;
        }
        if (!state.hasStringKey(fields[entry]) || (mayHideFields && attributesOf(fields[entry]) & PropertyAttribute::DontEnum))
            return;
        if (JSValue value = read(fields[entry]))
            goesOn = functor(*state.name(vm, fields[entry].name).impl(), value);
    });
    if (extra && goesOn) {
        size_t ordinal = 0;
        extraPropertiesStructure->forEachProperty(vm, [&](const auto& entry) -> bool {
            if (!hasPositionAmongKeys(entry.key()) || ordinal++ < nextExtraKey || entry.key()->isSymbol() || entry.attributes() & PropertyAttribute::DontEnum)
                return true;
            return functor(*entry.key(), extra->getDirect(entry.offset()));
        });
    }
    return true;
}

JSCellButterfly* Object::keys(VM& vm) const
{
    if (hasExtraProperties())
        return nullptr;
    State& state = this->state();
    uint32_t shapeWord = shapeWordOf(shape());
    auto& kept = state.instance.typedKeyLists[Instance::indexOfTypedKeyList(shapeWord, orderWord())];
    if (kept.shapeWord == shapeWord && kept.orderWord == orderWord())
        return uncheckedDowncast<JSCellButterfly>(kept.list);
    JSCellButterfly* list = state.keysInOrder(vm, layout(), orderWord() & orderAndCountMask);
    if (list)
        kept = { orderWord(), shapeWord, list };
    return list;
}

void Object::positionExtraKey(State& state, PropertyName name, uint32_t position)
{
    if (!hasPositionAmongKeys(name.uid()) || parseIndex(name) || orderOf(orderWord()) == keysAreInLayoutOrder)
        return;
    const LayoutData& layout = this->layout();
    auto entry = extraKeyOrdinal(name);
    if (!entry)
        return;
    *entry += layout.numberOfFields;
    uint32_t before = orderWord() & orderAndCountMask;
    if (auto placed = state.orderWithExtraKeyAt(layout, before, *entry, position)) {
        setOrderAndCount(*placed);
        return;
    }
    Vector<uint16_t, 16> keys;
    forEachKeyInOrder(state, [&](uint16_t key) {
        keys.append(key);
    });
    uint32_t result = before;
    if (!keys.containsIf([&](uint16_t key) { return key >= *entry; })) {
        keys.insert(std::min<size_t>(position, keys.size()), *entry);
        result = state.orderStartingWith(layout, keys.span(), false);
    }
    state.didPositionExtraKey(layout, before, *entry, position, result);
    setOrderAndCount(result);
}

void Object::positionExtraKeys(State& state, std::span<const std::pair<UniquedStringImpl*, uint32_t>> namesAndPositions)
{
    if (orderOf(orderWord()) == keysAreInLayoutOrder)
        return;
    const LayoutData& layout = this->layout();
    Vector<uint16_t, 16> keys;
    forEachKeyInOrder(state, [&](uint16_t key) {
        keys.append(key);
    });
    size_t positionedKeyCount = keys.size();
    for (auto [name, position] : namesAndPositions) {
        if (!hasPositionAmongKeys(name) || parseIndex(*name))
            continue;
        if (auto ordinal = extraKeyOrdinal(name); ordinal && !keys.containsIf([&](uint16_t key) { return key >= layout.numberOfFields + *ordinal; }))
            keys.insert(std::min<size_t>(position, keys.size()), static_cast<uint16_t>(layout.numberOfFields + *ordinal));
    }
    if (keys.size() != positionedKeyCount)
        setOrderAndCount(state.orderStartingWith(layout, keys.span(), false));
}

bool Object::hasPositionAmongKeys(UniquedStringImpl* name) const
{
    return !name->isSymbol() || state().hasPositionAmongKeys(layout(), name);
}

Vector<UniquedStringImpl*, 16> Object::extraKeyNames() const
{
    Vector<UniquedStringImpl*, 16> names;
    if (JSObject* extra = extraProperties()) {
        extra->structure()->forEachProperty(vm(), [&](const auto& property) {
            if (hasPositionAmongKeys(property.key()))
                names.append(property.key());
            return true;
        });
    }
    return names;
}

std::optional<uint16_t> Object::extraKeyOrdinal(PropertyName name) const
{
    JSObject* extra = extraProperties();
    if (!extra || !hasPositionAmongKeys(name.uid()))
        return std::nullopt;
    uint32_t ordinal = 0;
    bool isFound = false;
    extra->structure()->forEachProperty(vm(), [&](const auto& property) {
        isFound = property.key() == name.uid();
        ordinal += !isFound && hasPositionAmongKeys(property.key());
        return !isFound;
    });
    if (!isFound || layout().numberOfFields + ordinal >= UINT16_MAX)
        return std::nullopt;
    return static_cast<uint16_t>(ordinal);
}

void Object::didRemoveExtraKey(State& state, uint16_t ordinal, const FieldData* replacementField)
{
    const LayoutData& layout = this->layout();
    uint32_t numberOfFields = layout.numberOfFields;
    uint32_t positionedCount = 0;
    bool isPassed = false;
    bool changes = false;
    Vector<uint16_t, 16> keys;
    auto positionUpTo = [&](uint32_t end) {
        if (end <= positionedCount)
            return;
        keys.append(static_cast<uint16_t>(numberOfFields + end - 1));
        positionedCount = end;
    };
    forEachKeyInOrder(state, [&](uint16_t key) {
        if (key < numberOfFields) {
            keys.append(key);
            return;
        }
        uint32_t last = key - numberOfFields;
        if (last < ordinal) {
            positionUpTo(last + 1);
            return;
        }
        changes = true;
        if (!std::exchange(isPassed, true)) {
            positionUpTo(ordinal);
            if (replacementField)
                keys.append(indexOf(*replacementField));
        }
        positionUpTo(last);
    });
    if (replacementField && !isPassed) {
        keys.append(indexOf(*replacementField));
        changes = true;
    }
    if (changes)
        setOrderAndCount(state.orderStartingWith(layout, keys.span()));
}

void Object::forEachKeyInOrder(State& state, const Invocable<void(uint16_t)> auto& functor) const
{
    const LayoutData& layout = this->layout();
    uint32_t word = orderWord();
    if (orderOf(word) != keysAreInLayoutOrder) {
        for (uint16_t index : state.fieldsInOrder(layout, orderOf(word)).first(countOf(word)))
            functor(index);
        return;
    }
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        if (read(layout.fields()[i]))
            functor(i);
    }
}

void Object::forEachListedKey(State& state, const Invocable<void(uint16_t)> auto& functor) const
{
    forEachKeyInOrder(state, functor);
    const LayoutData& layout = this->layout();
    uint32_t word = orderWord();
    if (layout.kind != LayoutKind::Instance || orderOf(word) == keysAreInLayoutOrder)
        return;
    for (uint16_t index : state.fieldsInOrder(layout, orderOf(word)).subspan(countOf(word))) {
        if (!State::isDefinedByConstructor(layout, index))
            continue;
        if (JSValue stored = read(layout.fields()[index]); stored && !stored.isUndefined())
            functor(index);
    }
}

void Object::positionExtraKeys(State& state)
{
    JSObject* extra = extraProperties();
    const LayoutData& layout = this->layout();
    if (!extra || state.allExtraKeysHavePositions(layout, orderWord(), extra))
        return;
    uint32_t numberOfExtraKeys = 0;
    extra->structure()->forEachProperty(vm(), [&](const auto& property) {
        numberOfExtraKeys += hasPositionAmongKeys(property.key());
        return true;
    });
    uint32_t entry = layout.numberOfFields + numberOfExtraKeys - 1;
    bool allHavePositions = !numberOfExtraKeys;
    forEachKeyInOrder(state, [&](uint16_t key) {
        allHavePositions |= key >= entry;
    });
    if (allHavePositions)
        return;
    setOrderAndCount(entry < UINT16_MAX ? state.orderAfterCreating(layout, orderWord(), static_cast<uint16_t>(entry)) : orderWordOf(keysAreInLayoutOrder, 0));
}

bool Object::isInKeyOrder(State& state, const FieldData& field) const
{
    uint32_t word = orderWord();
    if (orderOf(word) == keysAreInLayoutOrder)
        return false;
    auto keys = state.fieldsInOrder(layout(), orderOf(word)).first(countOf(word));
    return std::ranges::find(keys, indexOf(field)) != keys.end();
}

void Object::listFieldsDefinedSoFar(State& state, const FieldData& named, bool includesField, bool mayUnlist)
{
    listFieldsBelow(state, indexOf(named) + includesField, mayUnlist);
}

void Object::listFieldStoredEarly(State& state, const FieldData& field)
{
    if (orderOf(orderWord()) == keysAreInLayoutOrder || isInKeyOrder(state, field))
        return;
    if (hasExtraProperties()) [[unlikely]]
        positionExtraKeys(state);
    setOrderAndCount(state.orderAfterCreating(layout(), orderWord(), indexOf(field)));
}

bool Object::isNotDefinedYet(const FieldData& field, JSValue held) const
{
    return field.flags & fieldMayBeUnlisted && held.isUndefined() && orderOf(orderWord()) != keysAreInLayoutOrder && !isInKeyOrder(state(), field);
}

void Object::listFieldsBelow(State& state, uint32_t end, bool mayUnlist)
{
    uint32_t word = orderWord();
    if (orderOf(word) == keysAreInLayoutOrder)
        return;
    const LayoutData& layout = this->layout();
    auto isDefinedByConstructor = [&](uint16_t key) {
        return State::isDefinedByConstructor(layout, key);
    };
    auto order = state.fieldsInOrder(layout, orderOf(word));
    uint32_t count = countOf(word);
    uint32_t kept = count;
    while (mayUnlist && kept && isDefinedByConstructor(order[kept - 1]) && order[kept - 1] >= end)
        --kept;
    if (kept < count) {
        setOrderAndCount(orderWordOf(orderOf(word), kept));
        return;
    }
    if (hasExtraProperties()) [[unlikely]] {
        positionExtraKeys(state);
        word = orderWord();
        if (orderOf(word) == keysAreInLayoutOrder)
            return;
        order = state.fieldsInOrder(layout, orderOf(word));
        count = countOf(word);
    }
    while (count < order.size() && isDefinedByConstructor(order[count]) && order[count] < end)
        ++count;
    setOrderAndCount(orderWordOf(orderOf(word), count));
    BitVector isListed;
    for (uint16_t key : order.first(count))
        isListed.set(key);
    for (uint32_t index = 0; index < end; ++index) {
        if (!isListed.get(index) && isDefinedByConstructor(index))
            setOrderAndCount(state.orderAfterCreating(layout, orderWord(), index));
    }
}

void Object::didCreateField(State& state, const FieldData& field, bool withStorage)
{
    if (field.flags & (fieldMayExistWithoutStorage | fieldMayBeUnlisted) && isInKeyOrder(state, field)) {
        if (withStorage && field.flags & fieldMayExistWithoutStorage)
            ensureExtensionCovers(vm(), state, field);
        return;
    }
    if (JSObject* extra = extraProperties(); extra && !state.allExtraKeysHavePositions(layout(), orderWord(), extra)) [[unlikely]] {
        uint64_t slotKey = withStorage && !(field.flags & (fieldMayExistWithoutStorage | fieldMayBeUnlisted)) ? slotKeyFor(field.offset, field.flags & fieldIsOutOfLine) : 0;
        if (slotKey) {
            if (auto known = state.orderAfterLateFieldCreation(*this, extra, slotKey)) {
                willCreateField(state, field);
                setOrderAndCount(*known);
                return;
            }
            uint32_t before = orderWord();
            positionExtraKeys(state);
            appendField(state, field);
            state.cacheLateFieldCreation(*this, extra, before, slotKey);
            return;
        }
        positionExtraKeys(state);
    }
    if (!withStorage) {
        state.countFieldCreation(layout(), field);
        setOrderAndCount(state.orderAfterCreating(layout(), orderWord(), indexOf(field)));
        if (orderOf(orderWord()) == keysAreInLayoutOrder) [[unlikely]] {
            ensureExtensionCovers(vm(), state, field);
            bool didStore = store(vm(), field, jsUndefined());
            RELEASE_ASSERT(didStore);
        }
        return;
    }
    appendField(state, field);
}

void Object::willKeepKeysInLayoutOrder()
{
    State& state = this->state();
    const LayoutData& layout = this->layout();
    uint32_t word = orderWord();
    for (uint16_t index : state.fieldsInOrder(layout, orderOf(word)).first(countOf(word))) {
        if (index >= layout.numberOfFields || !(layout.fields()[index].flags & fieldMayExistWithoutStorage))
            continue;
        const FieldData& field = layout.fields()[index];
        if (const void* slot = slotOf(field); slot && readAs(field.store(), slot, nullptr))
            continue;
        ensureExtensionCovers(vm(), state, field);
        bool didStore = store(vm(), field, jsUndefined());
        RELEASE_ASSERT(didStore);
    }
}

void Object::didMoveExtraPropertyToField(State& state, const FieldData& field)
{
    auto ordinal = orderOf(orderWord()) != keysAreInLayoutOrder ? extraKeyOrdinal(state.name(vm(), field.name)) : std::nullopt;
    if (!ordinal) {
        appendField(state, field);
        return;
    }
    willCreateField(state, field);
    didRemoveExtraKey(state, *ordinal, &field);
}

void Object::appendField(State& state, const FieldData& field)
{
    willCreateField(state, field);
    setOrderAndCount(state.orderAfterCreating(layout(), orderWord(), indexOf(field)));
}

void Object::willCreateField(State& state, const FieldData& field)
{
    if (field.flags & fieldIsOutOfLine)
        ensureExtensionCovers(vm(), state, field);
    state.countFieldCreation(layout(), field);
}

Object::PrivateElement Object::findPrivateElement(VM& vm, JSObject* object, PropertyName name)
{
    PrivateElement element;
    auto findIn = [&](JSObject* holder) {
        unsigned attributes;
        JSValue found = holder->getDirect(vm, name, attributes);
        if (!found)
            return false;
        if (attributes & PropertyAttribute::Accessor) {
            auto* accessor = uncheckedDowncast<GetterSetter>(found);
            element.kind = PrivateElement::Kind::Accessor;
            element.getter = accessor->isGetterNull() ? nullptr : accessor->getter();
            element.setter = accessor->isSetterNull() ? nullptr : accessor->setter();
            return true;
        }
        element.kind = attributes & PropertyAttribute::ReadOnly ? PrivateElement::Kind::Method : PrivateElement::Kind::Field;
        element.value = found;
        return true;
    };
    auto* compiled = dynamicDowncast<Object>(object);
    if (!compiled) {
        findIn(object);
        return element;
    }
    State& state = compiled->state();
    if (const FieldData* field = compiled->fieldNamed(state, name)) {
        element.value = compiled->read(*field);
        if (element.value)
            element.kind = PrivateElement::Kind::Field;
        return element;
    }
    if (state.findPrivateMethod(vm, compiled->layout(), name.uid(), element))
        return element;
    if (JSObject* extra = compiled->extraProperties(); extra && findIn(extra))
        return element;
    for (JSValue next = compiled->getPrototypeDirect(); next.isObject() && !findIn(asObject(next)); next = asObject(next)->getPrototypeDirect()) { }
    return element;
}

static bool isInheritedMethodBoundTo(VM& vm, Object* object, PropertyName name, JSValue value)
{
    JSObject* target = nullptr;
    for (JSValue next = value; next.isCell(); next = target) {
        if (auto* bound = dynamicDowncast<JSBoundFunction>(next)) {
            if (bound->boundThis() != JSValue(object) || bound->boundArgsLength())
                return false;
            target = bound->targetFunction();
            continue;
        }
        struct {
            uint32_t function;
            std::span<const uint64_t> words;
            uint32_t functionIndex() const { return function; }
            std::span<const uint64_t> captures() const { return words; }
        } bound, *closure = &bound;
        if (auto* full = dynamicDowncast<Closure>(next); full && full->kind() == Format::FunctionKind::Bound)
            bound = { full->functionIndex(), full->captures() };
        else if (auto* slim = dynamicDowncast<SlimClosure>(next); slim && static_cast<Format::FunctionKind>(slim->kindAndDeclaration() & functionKindMask) == Format::FunctionKind::Bound)
            bound = { slim->functionIndex(), slim->captures() };
        else
            break;
        const Data& data = object->state().data;
        std::span all { data.at<std::array<uint32_t, 3>>(data.boundFunctionsOffset), data.numberOfBoundFunctions };
        auto found = std::ranges::lower_bound(all, closure->functionIndex(), { }, [](auto& entry) {
            return entry[0];
        });
        RELEASE_ASSERT(found != all.end() && (*found)[0] == closure->functionIndex());
        if ((*found)[2] || closure->captures().size() != 1 || closure->captures()[0] != static_cast<uint64_t>(JSValue::encode(JSValue(object))))
            return false;
        target = object->state().closureObjectFor(vm, (*found)[1]);
    }
    if (!target)
        return false;
    for (JSValue next = object->getPrototypeDirect(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        ClassPrototype::look(asObject(next));
        if (JSValue inherited = asObject(next)->getDirect(vm, name))
            return inherited == JSValue(target);
    }
    return false;
}

static bool reportOrThrowIsClassMethod(ThrowScope& scope, JSGlobalObject* globalObject, State& state, const LayoutData& layout, PropertyName name)
{
    return state.reportOrThrow(scope, globalObject, { .kind = "method-shadow"_s, .on = &layout, .member = name.uid() }, makeString("Cannot replace method '"_s, StringView(name.uid()), "' of "_s, state.nameOf(layout), ": a method can be replaced only if TypeScript code of the program assigns to it (binding the method to its own object is allowed)"_s), State::InAudit::Performed);
}

bool Object::getOwnPropertySlot(JSObject* cell, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    if (const FieldData* field = object->fieldNamed(object->state(), name)) {
        JSValue value = object->readInline(*field);
        if (!value) [[unlikely]]
            value = object->read(*field);
        if (!value)
            return false;
        slot.setValue(object, object->attributesOf(*field), value);
        return true;
    }
    JSObject* extra = object->extraProperties();
    if (!extra)
        return false;
    bool found = extra->methodTable()->getOwnPropertySlot(extra, globalObject, name, slot);
    slot.disableCaching();
    return found;
}

bool Object::getOwnPropertySlotByIndex(JSObject* cell, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    JSObject* extra = uncheckedDowncast<Object>(cell)->extraProperties();
    if (!extra)
        return false;
    bool found = extra->methodTable()->getOwnPropertySlotByIndex(extra, globalObject, index, slot);
    slot.disableCaching();
    return found;
}

void Object::getOwnPropertyNames(JSObject* cell, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    State& state = object->state();
    JSObject* extra = object->extraProperties();
    if (extra)
        extra->getOwnIndexedPropertyNames(globalObject, names, mode);
    if (names.includeStringProperties()) {
        const LayoutData& layout = object->layout();
        const FieldData* fields = layout.fields();
        bool mayHideFields = mode == DontEnumPropertiesMode::Exclude && object->orderWord() & orderWordIsFrozen;
        auto extraKeyNames = object->extraKeyNames();
        size_t nextExtraKey = 0;
        object->forEachListedKey(state, [&](uint16_t index) {
            if (index >= layout.numberOfFields) {
                for (size_t end = std::min<size_t>(index - layout.numberOfFields + 1, extraKeyNames.size()); nextExtraKey < end; ++nextExtraKey) {
                    unsigned attributes;
                    if (!extraKeyNames[nextExtraKey]->isSymbol() && extra->getDirect(vm, extraKeyNames[nextExtraKey], attributes) && (mode == DontEnumPropertiesMode::Include || !(attributes & PropertyAttribute::DontEnum)))
                        names.add(Identifier::fromUid(vm, extraKeyNames[nextExtraKey]));
                }
                return;
            }
            if (state.hasStringKey(fields[index]) && !(mayHideFields && object->attributesOf(fields[index]) & PropertyAttribute::DontEnum))
                names.add(state.name(vm, fields[index].name));
        });
    }
    if (!state.hasFieldsWithSymbolKeys(object->layout())) {
        if (extra)
            extra->getOwnNonIndexPropertyNames(globalObject, names, mode);
        return;
    }
    auto addExtraKeys = [&](bool symbols) {
        if (!extra)
            return;
        extra->structure()->forEachProperty(vm, [&](const auto& entry) {
            if (entry.key()->isSymbol() == symbols && !PropertyName(entry.key()).isPrivateName() && (mode == DontEnumPropertiesMode::Include || !(entry.attributes() & PropertyAttribute::DontEnum)))
                names.add(Identifier::fromUid(vm, entry.key()));
            return true;
        });
    };
    if (names.includeStringProperties())
        addExtraKeys(false);
    if (names.includeSymbolProperties()) {
        const LayoutData& layout = object->layout();
        const FieldData* fields = layout.fields();
        bool mayHideFields = mode == DontEnumPropertiesMode::Exclude && object->orderWord() & orderWordIsFrozen;
        auto extraKeyNames = object->extraKeyNames();
        size_t nextExtraKey = 0;
        object->forEachListedKey(state, [&](uint16_t index) {
            if (index >= layout.numberOfFields) {
                for (size_t end = std::min<size_t>(index - layout.numberOfFields + 1, extraKeyNames.size()); nextExtraKey < end; ++nextExtraKey) {
                    unsigned attributes;
                    if (extraKeyNames[nextExtraKey]->isSymbol() && extra->getDirect(vm, extraKeyNames[nextExtraKey], attributes) && (mode == DontEnumPropertiesMode::Include || !(attributes & PropertyAttribute::DontEnum)))
                        names.add(Identifier::fromUid(vm, extraKeyNames[nextExtraKey]));
                }
                return;
            }
            if (state.hasSymbolKey(fields[index]) && !(mayHideFields && object->attributesOf(fields[index]) & PropertyAttribute::DontEnum))
                names.add(state.name(vm, fields[index].name));
        });
        addExtraKeys(true);
    }
}

static bool mayInterceptPut(VM& vm, JSObject* object, PropertyName name)
{
    if (name == vm.propertyNames->underscoreProto)
        return true;
    while (true) {
        Structure* structure = object->structure();
        if (structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || structure->typeInfo().overridesGetPrototype() || (structure->typeInfo().overridesPut() && structure->classInfoForCells() != PrototypeObject::info()))
            return true;
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject())
            return false;
        object = asObject(prototype);
    }
}

static std::pair<JSObject*, PropertyOffset> findAccessorOnOrdinaryPrototypeChain(VM& vm, JSObject* object, PropertyName name)
{
    if (name == vm.propertyNames->underscoreProto)
        return { nullptr, invalidOffset };
    while (true) {
        Structure* structure = object->structure();
        if (object->type() != FinalObjectType || structure->isUncacheableDictionary() || structure->hasPolyProto())
            return { nullptr, invalidOffset };
        unsigned attributes;
        PropertyOffset offset = structure->get(vm, name, attributes);
        if (isValidOffset(offset))
            return attributes & PropertyAttribute::Accessor ? std::pair { object, offset } : std::pair<JSObject*, PropertyOffset> { nullptr, invalidOffset };
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject())
            return { nullptr, invalidOffset };
        object = asObject(prototype);
    }
}

static bool ordinarySet(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyDescriptor ownDescriptor;
    bool isFound = object->getOwnPropertyDescriptor(globalObject, name, ownDescriptor);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isFound) {
        JSValue prototype = object->getPrototypeDirect();
        if (prototype.isObject())
            RELEASE_AND_RETURN(scope, asObject(prototype)->methodTable()->put(asObject(prototype), globalObject, name, value, slot));
        ownDescriptor = PropertyDescriptor(jsUndefined(), static_cast<unsigned>(PropertyAttribute::None));
    }
    RELEASE_AND_RETURN(scope, ordinarySetWithOwnDescriptor(globalObject, object, name, value, slot.thisValue(), WTF::move(ownDescriptor), slot.isStrictMode()));
}

bool Object::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    return putWithKnownField(cell, globalObject, name, value, slot, object->fieldNamed(object->state(), name));
}

bool Object::putWithKnownField(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot, const FieldData* field)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = object->state();
    uint32_t flags = object->at<uint32_t>(offsetOfOrderWord) & ~orderAndCountMask;
    if (field && !value.isObject() && slot.thisValue() == JSValue(object) && !(flags & ~orderWordHasExtraProperties) && !state.data.isCompiledForAudit) {
        if (!object->isDiscriminant(*field) && (object->readInline(*field) || (flags && object->read(*field))) && object->write(vm, *field, value))
            return true;
    }
    if (slot.thisValue() == JSValue(object) && state.storeByKnownTransition(vm, object, name.uid(), value))
        return true;
    if (slot.thisValue() != JSValue(object) || parseIndex(name)) [[unlikely]]
        RELEASE_AND_RETURN(scope, ordinarySet(globalObject, object, name, value, slot));
    if (field) {
        if (!name.isPrivateName() && object->attributesOf(*field) & PropertyAttribute::ReadOnly)
            return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
        if (object->read(*field)) {
            bool givesUp = false;
            bool isStored = object->assign(globalObject, *field, value, &givesUp);
            if (!givesUp)
                RELEASE_AND_RETURN(scope, isStored);
            scope.assertNoException();
            RELEASE_AND_RETURN(scope, object->convertToOrdinaryObject(vm)->methodTable()->put(cell, globalObject, name, value, slot));
        }
    } else if (PropertyOffset offset; object->tryStoreExtraProperty(vm, name, value, offset, true) != ExtraPropertyStore::NotHandled)
        return true;
    else if (JSObject* extra = object->extraProperties()) {
        unsigned attributes;
        if (JSValue existing = extra->getDirect(vm, name, attributes)) {
            if (attributes & PropertyAttribute::Accessor)
                RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(existing)->callSetter(globalObject, object, value, slot.isStrictMode()));
            if (attributes & PropertyAttribute::ReadOnly)
                return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
            if (state.hasImmutableMethod(object->layout(), name.uid()) && !isInheritedMethodBoundTo(vm, object, name, value) && !reportOrThrowIsClassMethod(scope, globalObject, state, object->layout(), name))
                return false;
            extra->putDirect(vm, name, value, attributes);
            return true;
        }
    }
    JSValue prototype = object->getPrototypeDirect();
    if (prototype.isObject() && !name.isPrivateName() && mayInterceptPut(vm, asObject(prototype), name)) {
        if (auto [holder, offset] = findAccessorOnOrdinaryPrototypeChain(vm, asObject(prototype), name); holder) {
            slot.setCacheableSetter(holder, offset);
            RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(holder->getDirect(offset))->callSetter(globalObject, object, value, slot.isStrictMode()));
        }
        RELEASE_AND_RETURN(scope, asObject(prototype)->methodTable()->put(asObject(prototype), globalObject, name, value, slot));
    }
    RELEASE_AND_RETURN(scope, defineOwnProperty(object, globalObject, name, PropertyDescriptor(value, static_cast<unsigned>(PropertyAttribute::None)), slot.isStrictMode()));
}

void Object::putExtraWithoutRunningCode(JSGlobalObject* globalObject, PropertyName name, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = this->state();
    ASSERT(!fieldNamed(state, name));
    auto messageFor = [&](ASCIILiteral why) {
        return makeString("Cannot set property '"_s, StringView(name.uid()), "' of "_s, state.nameOf(layout()), ": "_s, why);
    };
    auto refuse = [&](ASCIILiteral why) {
        state.reportOrThrow(scope, globalObject, { .kind = "runtime-accessor"_s, .on = &layout() }, messageFor(why), State::InAudit::Ignored);
    };
    auto rejectPerSpec = [&](ASCIILiteral why) {
        throwTypeError(globalObject, scope, messageFor(why));
    };
    unsigned attributes;
    JSObject* extra = extraProperties();
    if (extra && extra->getDirect(vm, name, attributes)) {
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
            return refuse("TypeScript code of the program cannot call a setter that was added at run time"_s);
        if (attributes & PropertyAttribute::ReadOnly)
            return rejectPerSpec("the property is read only"_s);
        extra->putDirect(vm, name, value, attributes);
        return;
    }
    for (JSValue next = getPrototypeDirect(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        JSObject* holder = asObject(next);
        ClassPrototype::look(holder);
        Structure* structure = holder->structure();
        TypeInfo typeInfo = structure->typeInfo();
        if (typeInfo.overridesGetOwnPropertySlot() || (typeInfo.overridesPut() && structure->classInfoForCells() != PrototypeObject::info()) || typeInfo.overridesGetPrototype() || holder->hasNonReifiedStaticProperties())
            return refuse("its prototype chain has a Proxy or another exotic object"_s);
        if (!holder->getDirect(vm, name, attributes))
            continue;
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
            return refuse("TypeScript code of the program cannot call an inherited setter that is not declared in the type"_s);
        if (attributes & PropertyAttribute::ReadOnly && !structure->inheritorsMayOverride(vm, name.uid()))
            return rejectPerSpec("the inherited property is read only"_s);
        break;
    }
    if (!isNotExtensible() && !parseIndex(name) && !state.hasImmutableMethod(layout(), name.uid())) {
        bool isStored = state.specializeToStore(globalObject, this, name, value);
        RETURN_IF_EXCEPTION(scope, void());
        if (isStored)
            return;
        willAddExtraProperty(vm, name);
        ensureExtraProperties(vm)->putDirect(vm, name, value);
        didChangeExtraProperties(vm);
        return;
    }
    scope.release();
    defineOwnProperty(this, globalObject, name, PropertyDescriptor(value, static_cast<unsigned>(PropertyAttribute::None)), true);
}

bool Object::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    PutPropertySlot slot(cell, shouldThrow);
    return ordinarySet(globalObject, uncheckedDowncast<Object>(cell), Identifier::from(globalObject->vm(), index), value, slot);
}

bool Object::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    State& state = object->state();
    if (const FieldData* field = object->fieldNamed(state, name)) {
        if (!object->read(*field))
            return true;
        if (object->attributesOf(*field) & PropertyAttribute::DontDelete)
            return false;
        const LayoutData& layout = object->layout();
        if (layout.mayDespecialize && (!(field->flags & fieldIsOptional) || object->isDiscriminant(*field)))
            return object->convertToOrdinaryObject(vm)->methodTable()->deleteProperty(cell, globalObject, name, slot);
        if (!(field->flags & fieldIsOptional))
            return state.reportOrThrow(globalObject, { .kind = "field-delete"_s, .on = &layout, .member = name.uid() }, makeString("Cannot delete property '"_s, StringView(name.uid()), "' of "_s, state.nameOf(layout), ": only optional properties can be deleted"_s), State::InAudit::Ignored);
        if (object->isDiscriminant(*field) && !state.reportOrThrow(globalObject, { .kind = "discriminant-change"_s, .on = &layout, .member = name.uid() }, makeString("Cannot delete property '"_s, StringView(name.uid()), "' of "_s, state.nameOf(layout), ": a discriminant property cannot change"_s), State::InAudit::Performed))
            return false;
        RELEASE_ASSERT(state.mayBeAbsent(*field));
        if (orderOf(object->orderWord()) != keysAreInLayoutOrder) {
            Vector<uint16_t, 16> remaining;
            object->forEachKeyInOrder(state, [&](uint16_t index) {
                if (index != object->indexOf(*field))
                    remaining.append(index);
            });
            object->setOrderAndCount(state.orderStartingWith(layout, remaining.span()));
        }
        if (field->flags & fieldIsPinned) [[unlikely]]
            state.willModify(*field, JSValue());
        object->clear(*field);
        if (object->orderWord() & orderWordIsFrozen) [[unlikely]]
            object->setAttributesOf(vm, *field, 0);
        return true;
    }
    JSObject* extra = object->extraProperties();
    if (!extra)
        return true;
    auto ordinal = orderOf(object->orderWord()) != keysAreInLayoutOrder ? object->extraKeyOrdinal(name) : std::nullopt;
    bool isDeleted = extra->methodTable()->deleteProperty(extra, globalObject, name, slot);
    slot.disableCaching();
    if (isDeleted) {
        if (ordinal)
            object->didRemoveExtraKey(state, *ordinal);
        object->dropExtraPropertiesIfEmpty(vm);
    }
    return isDeleted;
}

bool Object::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    Object* object = uncheckedDowncast<Object>(cell);
    JSObject* extra = object->extraProperties();
    if (!extra)
        return true;
    bool isDeleted = extra->methodTable()->deletePropertyByIndex(extra, globalObject, index);
    object->didChangeExtraProperties(globalObject->vm());
    return isDeleted;
}

bool Object::defineOwnProperty(JSObject* cell, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = object->state();
    const LayoutData& layout = object->layout();
    if (state.isPrototypeShape(layout.shape)) [[unlikely]]
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change, object, name.uid());
    const FieldData* field = object->fieldNamed(state, name);
    if (!field) {
        JSObject* extra = object->extraProperties();
        bool exists = extra && extra->hasOwnProperty(globalObject, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (parseIndex(name) && !object->isFrozen() && (descriptor.isAccessorDescriptor() || !(descriptor.writablePresent() ? descriptor.writable() : exists)) && layout.mayDespecialize)
            RELEASE_AND_RETURN(scope, object->convertToOrdinaryObject(vm)->methodTable()->defineOwnProperty(cell, globalObject, name, descriptor, shouldThrow));
        if (parseIndex(name) && !object->isFrozen() && (descriptor.isAccessorDescriptor() || !(descriptor.writablePresent() ? descriptor.writable() : exists)))
            return state.reportOrThrow(scope, globalObject, { .kind = "index-accessor"_s, .on = &layout }, makeString("Cannot define property '"_s, StringView(name.uid()), "' of "_s, state.nameOf(layout), ": indexed properties of an object of a declared type cannot be accessors or read only"_s), State::InAudit::Ignored);
        if (state.hasImmutableMethod(layout, name.uid()) && (descriptor.isAccessorDescriptor() || (descriptor.value() ? !isInheritedMethodBoundTo(vm, object, name, descriptor.value()) : !exists)) && !reportOrThrowIsClassMethod(scope, globalObject, state, layout, name))
            return false;
        if (!exists) {
            if (object->isNotExtensible())
                return typeError(globalObject, scope, shouldThrow, NonExtensibleObjectPropertyDefineError);
            if (descriptor.value() && descriptor.writable() && descriptor.enumerable() && descriptor.configurable()) {
                bool isStored = state.specializeToStore(globalObject, object, name, descriptor.value());
                RETURN_IF_EXCEPTION(scope, false);
                if (isStored)
                    return true;
            }
            object->willAddExtraProperty(vm, name);
            extra = object->ensureExtraProperties(vm);
        }
        scope.release();
        bool isDefined = extra->methodTable()->defineOwnProperty(extra, globalObject, name, descriptor, shouldThrow);
        object->didChangeExtraProperties(vm);
        return isDefined;
    }

    JSValue current = object->read(*field);
    unsigned currentAttributes = object->attributesOf(*field);
    PropertyDescriptor currentDescriptor;
    if (current)
        currentDescriptor.setDescriptor(current, currentAttributes);
    bool isValid = validateAndApplyPropertyDescriptor(globalObject, nullptr, name, !object->isNotExtensible(), descriptor, !!current, currentDescriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isValid)
        return false;
    unsigned attributes = current ? descriptor.attributesOverridingCurrent(currentDescriptor) : descriptor.attributes();
    bool getsAttributes = attributes != currentAttributes && layout.mayBeFrozen && !descriptor.isAccessorDescriptor() && !name.isPrivateName() && (!(attributes & PropertyAttribute::DontEnum) || state.mayBeAbsent(*field));
    if (attributes != currentAttributes && !getsAttributes) {
        if (layout.mayDespecialize)
            RELEASE_AND_RETURN(scope, object->convertToOrdinaryObject(vm)->methodTable()->defineOwnProperty(cell, globalObject, name, descriptor, shouldThrow));
        bool goesOn = state.reportOrThrow(globalObject, { .kind = "field-redefine"_s, .on = &layout, .member = name.uid() }, makeString("Cannot redefine property '"_s, StringView(name.uid()), "' of "_s, state.nameOf(layout), ": declared properties stay writable, enumerable and configurable data properties"_s), State::InAudit::Ignored);
        EXCEPTION_ASSERT(!!scope.exception() == !goesOn);
        if (!goesOn)
            return false;
        if (descriptor.isAccessorDescriptor())
            return true;
    }
    if (object->isFrozen())
        return true;
    if (!current || descriptor.value()) {
        bool givesUp = false;
        bool isStored = object->assign(globalObject, *field, descriptor.value() ? descriptor.value() : jsUndefined(), &givesUp);
        RETURN_IF_EXCEPTION(scope, false);
        if (givesUp)
            RELEASE_AND_RETURN(scope, object->convertToOrdinaryObject(vm)->methodTable()->defineOwnProperty(cell, globalObject, name, descriptor, shouldThrow));
        if (!isStored)
            return false;
        if (!current)
            object->didCreateField(state, *field);
    }
    if (getsAttributes)
        object->setAttributesOf(vm, *field, attributes);
    return true;
}

JSValue Object::getDirectFromNativeCode(VM& vm, PropertyName name, unsigned* attributes)
{
    if (const FieldData* field = fieldNamed(state(), name)) {
        if (attributes)
            *attributes = attributesOf(*field);
        return read(*field);
    }
    JSObject* extra = extraProperties();
    if (!extra)
        return { };
    unsigned ignored;
    return extra->getDirect(vm, name, attributes ? *attributes : ignored);
}

bool Object::putDirectFromNativeCode(VM& vm, PropertyName name, JSValue value, unsigned attributes)
{
    State& state = this->state();
    if (const FieldData* field = fieldNamed(state, name)) {
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue || isDiscriminant(*field))
            return false;
        bool wasPresent = !!read(*field);
        if (!write(vm, *field, value))
            return false;
        if (!wasPresent)
            didCreateField(state, *field);
        if (attributes != attributesOf(*field) && (layout().mayBeFrozen || !(attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete))) && !name.isPrivateName() && (!(attributes & PropertyAttribute::DontEnum) || state.mayBeAbsent(*field)))
            setAttributesOf(vm, *field, attributes);
        return true;
    }
    JSObject* extra = extraProperties();
    if (!extra || !extra->getDirect(vm, name))
        willAddExtraProperty(vm, name);
    extra = ensureExtraProperties(vm);
    if (attributes & PropertyAttribute::Accessor)
        extra->putDirectNonIndexAccessor(vm, name, uncheckedDowncast<GetterSetter>(value), attributes);
    else
        extra->putDirect(vm, name, value, attributes);
    didChangeExtraProperties(vm);
    return true;
}

bool Object::setIntegrity(JSGlobalObject* globalObject, Integrity integrity)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t bits = orderWordIsNotExtensible;
    if (integrity != Integrity::NotExtensible)
        bits |= orderWordIsSealed;
    if (integrity == Integrity::Frozen)
        bits |= orderWordIsFrozen;
    auto* own = orderWord() & orderWordIsFrozen ? state().fieldAttributes.get(this) : nullptr;
    bool isToBeFrozen = own && !own->isFrozen && integrity == Integrity::Frozen;
    if ((orderWord() & bits) == bits && !isToBeFrozen)
        return true;
    if (state().isPrototypeShape(layout().shape)) [[unlikely]]
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change, this);
    if (!layout().mayBeFrozen && layout().mayDespecialize) {
        JSObject* ordinary = convertToOrdinaryObject(vm);
        switch (integrity) {
        case Integrity::NotExtensible:
            RELEASE_AND_RETURN(scope, ordinary->methodTable()->preventExtensions(ordinary, globalObject));
        case Integrity::Sealed:
            objectConstructorSeal(globalObject, ordinary);
            break;
        case Integrity::Frozen:
            objectConstructorFreeze(globalObject, ordinary);
            break;
        }
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }
    if (!layout().mayBeFrozen && !state().reportOrThrow(scope, globalObject, { .kind = "object-freeze"_s, .on = &layout() }, makeString("Cannot freeze, seal or prevent extensions of "_s, state().nameOf(layout()), ": no TypeScript code of the program passes a value to Object.freeze, Object.seal or Object.preventExtensions as this type, so its objects cannot be frozen"_s), State::InAudit::Performed))
        return false;
    if (JSObject* extra = extraProperties()) {
        switch (integrity) {
        case Integrity::NotExtensible:
            extra->methodTable()->preventExtensions(extra, globalObject);
            break;
        case Integrity::Sealed:
            objectConstructorSeal(globalObject, extra);
            break;
        case Integrity::Frozen:
            objectConstructorFreeze(globalObject, extra);
            break;
        }
        didChangeExtraProperties(vm);
        RETURN_IF_EXCEPTION(scope, false);
    }
    at<uint32_t>(offsetOfOrderWord) |= bits;
    if (isToBeFrozen)
        own->isFrozen = true;
    return true;
}

bool Object::preventExtensions(JSObject* cell, JSGlobalObject* globalObject)
{
    return uncheckedDowncast<Object>(cell)->setIntegrity(globalObject, Integrity::NotExtensible);
}

bool Object::isExtensible(JSObject* cell, JSGlobalObject*)
{
    return !uncheckedDowncast<Object>(cell)->isNotExtensible();
}

bool Object::setPrototype(JSObject* cell, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (object->getPrototypeDirect() == prototype)
        return true;
    if (object->isNotExtensible())
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
    for (JSValue next = prototype; next.isObject() && asObject(next)->type() != ProxyObjectType; next = asObject(next)->getPrototypeDirect()) {
        if (next == JSValue(object))
            return typeError(globalObject, scope, shouldThrow, "cyclic __proto__ value"_s);
    }
    State& state = object->state();
    const LayoutData& layout = object->layout();
    if (layout.kind != LayoutKind::Record || !(state.prototypeMatchesLayout(layout, prototype) || layout.mayHaveOtherPrototype)) {
        if (layout.mayDespecialize) {
            if (JSObject* ordinary = object->convertToOrdinaryObject(vm, prototype); ordinary->mayBePrototype()) [[unlikely]]
                vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Prototype, ordinary);
            if (state.isPrototypeShape(layout.shape) && object->anyObjectInChainMayInterceptIndexedAccesses()) [[unlikely]]
                object->realm()->haveABadTime(vm);
            return true;
        }
        String message = makeString("Cannot set prototype of "_s, state.nameOf(layout), ": objects of a declared type keep their prototype"_s);
        State::Violation violation { .kind = "object-set-prototype"_s, .on = &layout };
        if (!state.data.isCompiledForAudit) {
            if (shouldThrow)
                state.recordViolation(vm, violation);
            return typeError(globalObject, scope, shouldThrow, withAOTErrorNote(message));
        }
        state.reportOrThrow(scope, globalObject, violation, message, State::InAudit::Performed);
    }
    if (prototype.isObject())
        asObject(prototype)->didBecomePrototype(vm);
    if (state.isPrototypeShape(layout.shape)) [[unlikely]]
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Prototype, object);
    Structure* before = object->structure();
    bool isPlain = !object->hasFlagsInOrderWord() && state.isStructureWithoutExtraProperties(before);
    object->JSCell::setStructure(vm, state.structureWithExtraPropertiesMatching(vm, before, layout.shape, prototype));
    object->updateNeedsSlowLookup(state);
    if (isPlain && prototype.isObject() && !object->hasFlagsInOrderWord() && !object->anyObjectInChainMayInterceptIndexedAccesses()) {
        state.instance.prototypeChangeCache[Instance::prototypeChangeCacheIndex(std::bit_cast<uintptr_t>(prototype.asCell()))] = { prototype.asCell(), before->id().bits(), object->structureID().bits() };
        state.instance.prototypeChangeCacheIsDirty = true;
    }
    if (state.isPrototypeShape(layout.shape) && object->anyObjectInChainMayInterceptIndexedAccesses()) [[unlikely]]
        object->realm()->haveABadTime(vm);
    return true;
}

void Object::setPrototypeAtCreation(VM& vm, JSObject* prototype)
{
    if (getPrototypeDirect() == JSValue(prototype))
        return;
    JSCell::setStructure(vm, state().structureWithExtraPropertiesMatching(vm, structure(), layout().shape, prototype));
    updateNeedsSlowLookup(state(), true);
}

void Object::specialize(VM& vm, State& state, uint32_t shape)
{
    bool hasRoom = specialize(vm, state, state.transitionPlanFor(vm, this, shape));
    RELEASE_ASSERT(hasRoom);
}

bool Object::specialize(VM& vm, State& state, TransitionPlan& plan)
{
    ASSERT(plan.from == &layout());
    if (plan.size > cellSize())
        return false;
    uint32_t word = orderWord() & orderAndCountMask;
    size_t known = plan.orderWords.findIf([&](auto& fromAndTo) {
        return fromAndTo.first == word;
    });
    uint32_t wordInSpecialization = known != notFound ? plan.orderWords[known].second : state.orderInSpecialization(vm, *plan.from, *plan.to, word);
    if (known == notFound && plan.orderWords.size() < TransitionPlan::maxOrderWords) [[unlikely]]
        plan.orderWords.append({ word, wordInSpecialization });
    if (orderOf(wordInSpecialization) == keysAreInLayoutOrder && orderOf(word) != keysAreInLayoutOrder) [[unlikely]]
        willKeepKeysInLayoutOrder();
    for (auto& slot : plan.addedSlots)
        clearSlot(&at<uint8_t>(slot.offset), slot.store);
    WTF::storeStoreFence();
    JSCell::setStructure(vm, plan.structure);
    if (ExtraPropertiesHandle* handle = extraPropertiesHandle())
        handle->orderAndCount = wordInSpecialization;
    else
        at<uint32_t>(offsetOfOrderWord) = (at<uint32_t>(offsetOfOrderWord) & ~orderAndCountMask) | wordInSpecialization;
    at<uint32_t>(offsetOfShapeWord) = plan.shapeWord;
    state.countTransition(*plan.from, *plan.to);
    return true;
}

bool Object::specializeWithPlan(VM& vm, const TransitionPlan& plan)
{
    if (plan.size > cellSize())
        return false;
    uint32_t word = orderWord();
    for (auto [before, after] : plan.orderWords) {
        if (before != word)
            continue;
        if (orderOf(after) == keysAreInLayoutOrder && orderOf(word) != keysAreInLayoutOrder)
            return false;
        for (auto& slot : plan.addedSlots)
            clearSlot(&at<uint8_t>(slot.offset), slot.store);
        WTF::storeStoreFence();
        JSCell::setStructure(vm, plan.structure);
        at<uint32_t>(offsetOfOrderWord) = after;
        at<uint32_t>(offsetOfShapeWord) = plan.shapeWord;
        return true;
    }
    return false;
}

const ClassInfo EvalScope::s_info = { "EvalScope"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(EvalScope) };

EvalScope::EvalScope(VM& vm, Structure* structure, JSScope* next, JSObject* bindings)
    : Base(vm, structure, next)
    , m_bindings(bindings, WriteBarrierEarlyInit)
{
}

Structure* EvalScope::createStructure(VM& vm, JSGlobalObject* globalObject)
{
    return Structure::create(vm, globalObject, jsNull(), TypeInfo(StrictEvalActivationType, StructureFlags), info());
}

EvalScope* EvalScope::create(VM& vm, Structure* structure, JSScope* next, JSObject* bindings)
{
    EvalScope* scope = new (NotNull, allocateCell<EvalScope>(vm)) EvalScope(vm, structure, next, bindings);
    scope->finishCreation(vm);
    return scope;
}

template<typename Visitor>
void EvalScope::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* scope = uncheckedDowncast<EvalScope>(cell);
    Base::visitChildren(scope, visitor);
    visitor.append(scope->m_bindings);
}

DEFINE_VISIT_CHILDREN(EvalScope);

Identifier EvalScope::newTargetKey(VM& vm)
{
    static constexpr Latin1Character key[] = { 0, 't', 'a', 'r', 'g', 'e', 't' };
    return Identifier::fromString(vm, String(std::span<const Latin1Character> { key }));
}

bool EvalScope::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    JSObject* bindings = uncheckedDowncast<EvalScope>(object)->m_bindings.get();
    if (name == vm.propertyNames->builtinNames().newTargetLocalPrivateName())
        return bindings->methodTable()->getOwnPropertySlot(bindings, globalObject, newTargetKey(vm), slot);
    return bindings->methodTable()->getOwnPropertySlot(bindings, globalObject, name, slot);
}

bool EvalScope::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    JSObject* bindings = uncheckedDowncast<EvalScope>(cell)->m_bindings.get();
    PutPropertySlot ofBindings(bindings, slot.isStrictMode());
    return bindings->methodTable()->put(bindings, globalObject, name, value, ofBindings);
}

bool EvalScope::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot&)
{
    return !uncheckedDowncast<EvalScope>(cell)->m_bindings->hasOwnProperty(globalObject, name);
}

Structure* State::evalScopeStructure(VM& vm)
{
    if (!m_evalScopeStructure)
        m_evalScopeStructure.set(vm, EvalScope::createStructure(vm, instance.globalObject));
    return m_evalScopeStructure.get();
}

const ClassInfo Closure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Closure) };

Structure* Closure::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype, bool isBound)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(JSFunctionType, isBound ? StructureFlags & ~ImplementsDefaultHasInstance : StructureFlags), info());
}

Closure* Closure::create(VM& vm, Structure* structure, uint32_t function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    RELEASE_ASSERT(numberOfReferences <= captures.size() && captures.size() <= maxNumberOfCaptures);
    FunctionTables& tables = FunctionTables::of(vm);
    RELEASE_ASSERT(ProgramData::get()->closureWordOf(function));
    RELEASE_ASSERT((tables.data.at<std::array<uint8_t, 2>>(tables.data.closureCountsOffset)[function] == std::array { static_cast<uint8_t>(numberOfReferences), static_cast<uint8_t>(captures.size()) }));
    Closure* closure = new (NotNull, allocateCell<Closure>(vm, allocationSize(captures.size()))) Closure(vm, structure, tables.executableOf(function), tables.words[function]);
    memcpySpan(std::span { reinterpret_cast<uint64_t*>(closure + 1), captures.size() }, captures);
    closure->finishCreation(vm);
    if (Instance* instance = structure->aotInstance())
        instance->typed->countClosure(function, captures.size());
    else
        tables.closuresWithoutInstance.append(Weak<JSCell>(closure));
    return closure;
}

Closure* Closure::createForCompiledCode(VM& vm, Structure* structure, size_t allocationSize)
{
    RELEASE_ASSERT(allocationSize >= Closure::allocationSize(0));
    Closure* closure = new (NotNull, allocateCell<Closure>(vm, allocationSize)) Closure(vm, structure, nullptr, 0);
    closure->finishCreation(vm);
    return closure;
}

template<typename Visitor>
void Closure::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Closure* closure = uncheckedDowncast<Closure>(cell);
    Base::visitChildren(cell, visitor);
    if (!closure->m_entry)
        return;
    visitor.appendValues(reinterpret_cast<const WriteBarrier<Unknown>*>(closure->captures().data()), closure->numberOfReferences());
}

DEFINE_VISIT_CHILDREN(Closure);

void Closure::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Base::analyzeHeap(cell, analyzer);
    Closure* closure = uncheckedDowncast<Closure>(cell);
    if (!closure->m_entry)
        return;
    for (unsigned i = 0; i < closure->numberOfReferences(); ++i) {
        if (JSValue reference = JSValue::decode(closure->captures()[i]); reference && reference.isCell())
            analyzer.analyzeIndexEdge(cell, reference.asCell(), i);
    }
}

static void setNameReportedForFrames(uint32_t function, const String&, bool isOnlyClosure);
static bool usesOwnPropertiesForStackTraceName(JSCell* closure);
static std::atomic<bool> anyClosureHasDisplayName;

JSC_DEFINE_HOST_FUNCTION(callClosure, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    ProtoCallFrame protoCallFrame;
    protoCallFrame.init(nullptr, globalObject, callFrame->jsCallee(), callFrame->thisValue(), nullptr, callFrame->argumentCountIncludingThis(), reinterpret_cast<EncodedJSValue*>(callFrame->addressOfArgumentsStart()));
    return vmEntryToJavaScript(closureEntrypoint().taggedPtr(), &vm, &protoCallFrame);
}

static JSC_DECLARE_HOST_FUNCTION(callClassConstructor);
static JSC_DECLARE_HOST_FUNCTION(constructClosure);

JSC_DEFINE_HOST_FUNCTION(callClassConstructor, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* classObject = uncheckedDowncast<Closure>(callFrame->jsCallee());
    String name = classObject->structure()->aotInstance()->typed->nameOfFunction(classObject->functionIndex());
    return throwVMTypeError(globalObject, scope, name.isEmpty() ? "Cannot call a class constructor without |new|"_str : makeString("Cannot call a class constructor "_s, name, " without |new|"_s));
}

class NewTargetScope {
public:
    NewTargetScope(const Closure* closure, JSValue newTarget)
        : m_state(closure->isDeclaredAs(functionReadsNewTarget) ? closure->structure()->aotInstance()->typed : nullptr)
    {
        if (m_state)
            m_state->newTarget = newTarget;
    }

    ~NewTargetScope()
    {
        if (m_state)
            m_state->newTarget = { };
    }

private:
    State* m_state;
};

class ConstructionScope {
public:
    ConstructionScope(VM& vm, State& state)
        : m_state(state)
        , m_construction { vm.topCallFrame, state.constructionsFromNative }
    {
        state.constructionsFromNative = &m_construction;
    }

    ~ConstructionScope()
    {
        m_state.constructionsFromNative = m_construction.previous;
    }

private:
    State& m_state;
    State::ConstructionFromNative m_construction;
};

static void appendConstructArguments(MarkedArgumentBuffer& all, const Closure* closure, JSValue newTarget, const ArgList& arguments)
{
    if (const LayoutData* ofClass = closure->classData(); ofClass && ofClass->constructorTakesNewTarget)
        all.append(newTarget);
    for (unsigned i = 0; i < arguments.size(); ++i)
        all.append(arguments.at(i));
}

static JSValue createThisFromCache(JSGlobalObject* globalObject, State& state, Closure* constructor, JSObject* newTarget)
{
    VM& vm = globalObject->vm();
    Instance& instance = state.instance;
    AOT::Slot* remembered = instance.thisAllocationFor(newTarget);
    uintptr_t epoch = instance.negativeOffsetFields().typedPrototypeEpoch;
    if (remembered[0].structureID && remembered[0].pointer == newTarget && remembered[3].pointer == constructor && std::bit_cast<uintptr_t>(remembered[2].pointer) == epoch) {
        if (Options::useAOTOperationCounters()) [[unlikely]]
            runtimeTable(vm).countOperation("createThis", "remembered");
        return constructEmptyObject(vm, WTF::opaque(remembered[0].structureID.decode()));
    }
    Structure* empty = state.structureForThis(globalObject, constructor, newTarget, JSFinalObject::defaultInlineCapacity);
    if (!empty)
        return state.createThis(globalObject, constructor, newTarget);
    if (Allocator allocator = subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(empty->inlineCapacity()), AllocatorForMode::EnsureAllocator)) {
        remembered[0].structureID = StructureID();
        remembered[0].offset = empty->inlineCapacity() | AOT::Slot::pointerIsCell;
        remembered[0].pointer = newTarget;
        remembered[1].offset = empty->typeInfoBlob();
        remembered[1].pointer = allocator.localAllocator();
        remembered[2].pointer = std::bit_cast<void*>(epoch);
        remembered[3].offset = constructor->isDeclaredAs(functionReadsNewTarget);
        remembered[3].pointer = constructor;
        remembered[0].structureID = empty->id();
        instance.hasThisAllocations = true;
    }
    return constructEmptyObject(vm, empty);
}

JSC_DEFINE_HOST_FUNCTION(constructClosure, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* closure = uncheckedDowncast<Closure>(callFrame->jsCallee());
    JSObject* newTarget = asObject(callFrame->newTarget());
    JSValue thisValue = createThisFromCache(globalObject, *closure->structure()->aotInstance()->typed, closure, newTarget);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    appendConstructArguments(arguments, closure, newTarget, ArgList(callFrame));
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    ProtoCallFrame protoCallFrame;
    protoCallFrame.init(nullptr, globalObject, closure, thisValue, nullptr, arguments.size() + 1, arguments.data());
    NewTargetScope newTargetScope(closure, newTarget);
    ConstructionScope constructionScope(vm, *closure->structure()->aotInstance()->typed);
    JSValue result = JSValue::decode(vmEntryToJavaScript(closureEntrypoint().taggedPtr(), &vm, &protoCallFrame));
    RETURN_IF_EXCEPTION(scope, { });
    if (!result.isObject())
        result = thisValue;
    RELEASE_ASSERT(result.isObject());
    return JSValue::encode(result);
}

const ClassInfo PrototypeObject::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PrototypeObject) };

Structure* PrototypeObject::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

PrototypeObject* PrototypeObject::create(VM& vm, Structure* structure, uint32_t function)
{
    PrototypeObject* object = new (NotNull, allocateCell<PrototypeObject>(vm)) PrototypeObject(vm, structure, function);
    object->finishCreation(vm);
    return object;
}

bool PrototypeObject::isGuarded() const
{
    return !(structure()->aotInstance()->typed->kindAndDeclarationOf(m_function) & functionIsInBuiltinModule);
}

bool PrototypeObject::allowsModification(VM& vm)
{
    State& state = *structure()->aotInstance()->typed;
    if (!state.mayModifyPrototypeObjectOf(m_function))
        return false;
    if (!std::exchange(m_isModified, true))
        state.didModifyPrototypeObject(vm, this);
    return true;
}

bool PrototypeObject::allowsPrototypeChange(VM& vm)
{
    return !structure()->aotInstance()->typed->createsObjectsWithDefinitelyInitializedFields(m_function) && allowsModification(vm);
}

bool PrototypeObject::mayDefine(JSGlobalObject* globalObject, PropertyName name, bool isWritableData)
{
    State& state = *structure()->aotInstance()->typed;
    if (name.isPrivateName() || !isGuarded() || !state.isReservedFieldName(globalObject->vm(), name.uid(), m_function))
        return true;
    if ((isWritableData || !state.createsObjectsWithDefinitelyInitializedFields(m_function)) && allowsModification(globalObject->vm()))
        return true;
    return state.reportOrThrow(globalObject, { .kind = "function-prototype-write"_s, .onFunction = m_function, .member = name.uid() }, makeString("Cannot define property '"_s, StringView(name.uid()), "' on the prototype of function '"_s, state.nameOfFunction(m_function), "': its objects have a declared field of that name, and the compiler did not see this definition"_s), State::InAudit::Performed);
}

bool PrototypeObject::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    if (slot.thisValue() == JSValue(cell) && !uncheckedDowncast<PrototypeObject>(cell)->mayDefine(globalObject, name, true))
        return false;
    bool result = Base::put(cell, globalObject, name, value, slot);
    slot.disableCaching();
    return result;
}

bool PrototypeObject::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    if (!uncheckedDowncast<PrototypeObject>(object)->mayDefine(globalObject, name, descriptor.isDataDescriptor() && descriptor.writable()))
        return false;
    return Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
}

bool PrototypeObject::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    State& state = *object->structure()->aotInstance()->typed;
    if (object->getPrototypeDirect() != prototype && uncheckedDowncast<PrototypeObject>(object)->isGuarded() && !uncheckedDowncast<PrototypeObject>(object)->allowsPrototypeChange(globalObject->vm()) && !state.reportOrThrow(globalObject, { .kind = "function-prototype-write"_s, .onFunction = uncheckedDowncast<PrototypeObject>(object)->m_function }, makeString("Cannot set the prototype of the 'prototype' object of function '"_s, state.nameOfFunction(uncheckedDowncast<PrototypeObject>(object)->m_function), "': TypeScript code in the program creates instances of it"_s), State::InAudit::Performed))
        return false;
    return Base::setPrototype(object, globalObject, prototype, shouldThrowIfCantSet);
}

const ClassInfo ClassPrototype::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ClassPrototype) };

static_assert(sizeof(ClassPrototype) == sizeof(JSObjectWithButterfly) + ClassPrototype::inlineCapacity * sizeof(WriteBarrierBase<Unknown>));

Structure* ClassPrototype::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    Structure* structure = Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    structure->setMayBePrototype(true);
    return structure;
}

ClassPrototype* ClassPrototype::create(VM& vm, Structure* structure, uint32_t classIndex)
{
    ClassPrototype* object = new (NotNull, allocateCell<ClassPrototype>(vm)) ClassPrototype(vm, structure, classIndex);
    object->finishCreation(vm);
    return object;
}

void ClassPrototype::convertToOrdinaryObject(VM& vm, Structure* ordinary)
{
    RELEASE_ASSERT(ordinary->classInfoForCells() == JSFinalObject::info() && ordinary->inlineCapacity() == inlineCapacity && ordinary->storedPrototype() == structure()->storedPrototype());
    m_classIndex = 0;
    m_declaredMemberLookups = 0;
    WTF::storeStoreFence();
    if (butterfly())
        nukeStructureAndSetButterfly(vm, structureID(), nullptr);
    setStructure(vm, ordinary);
}

void ClassPrototype::createMembers(JSObject* object)
{
    State& state = *object->structure()->aotInstance()->typed;
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(object->vm()).countOperation("ClassPrototype", "made");
    MarkedArgumentBuffer entryOwners;
    auto entries = state.collectDeclaredMemberEntries(object->vm(), *state.classWithIndex(uncheckedDowncast<ClassPrototype>(object)->classIndex()), entryOwners);
    PropertyLookupTable& table = PropertyLookupTable::of(object->vm());
    table.willCreateDeclaredMembers();
    Closure* classObject = state.closure(object->vm(), state.classWithIndex(static_cast<uint32_t>(uncheckedDowncast<ClassPrototype>(object)->m_classIndex))->classData().classConstructor);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(object->vm());
    state.createClass(state.instance.globalObject, classObject, JSValue(), State::ClassPart::PrototypeMembers);
    scope.assertNoException();
    table.didCreateDeclaredMembers();
    RELEASE_ASSERT(object->type() == FinalObjectType);
    state.repointDeclaredMemberEntries(object->vm(), object, entries);
}

bool ClassPrototype::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    if (object->vm().heap.currentThreadIsDoingGCWork()) [[unlikely]] {
        slot.setIsTaintedByOpaqueObject();
        return false;
    }
    if (auto answer = uncheckedDowncast<ClassPrototype>(object)->tryGetDeclaredMember(globalObject->vm(), name, slot))
        return *answer;
    State& state = *object->structure()->aotInstance()->typed;
    createMembers(object);
    state.didGetDeclaredMember = true;
    return JSObject::getOwnPropertySlot(object, globalObject, name, slot);
}

std::optional<bool> ClassPrototype::tryGetDeclaredMember(VM& vm, PropertyName name, PropertySlot& slot)
{
    auto methodType = slot.internalMethodType();
    if ((methodType != PropertySlot::InternalMethodType::Get && methodType != PropertySlot::InternalMethodType::HasProperty) || butterfly() || !name.uid())
        return std::nullopt;
    State& state = *structure()->aotInstance()->typed;
    const LayoutData& ofClass = *state.classWithIndex(static_cast<uint32_t>(m_classIndex));
    if (name.uid()->isSymbol() && std::ranges::none_of(ofClass.methods(), [](auto& method) { return method.flags & methodHasComputedName; })) {
        state.didGetDeclaredMember = true;
        slot.setIsTaintedByOpaqueObject();
        return false;
    }
    constexpr uint64_t oneRepeatedLookup = 1ull << 16;
    constexpr unsigned firstNameBit = 20;
    uint64_t nameBit = 1ull << (firstNameBit + name.uid()->hash() % (64 - firstNameBit));
    bool isRepeated = m_declaredMemberLookups & nameBit;
    bool hasRepeatedBefore = m_declaredMemberLookups & ((1ull << firstNameBit) - oneRepeatedLookup);
    if ((m_declaredMemberLookups & (oneRepeatedLookup - 1)) >= std::max<size_t>(4, ofClass.methods().size()) || (isRepeated && hasRepeatedBefore))
        return std::nullopt;
    uint64_t lookupsWithThisOne = (m_declaredMemberLookups + 1 + (isRepeated ? oneRepeatedLookup : 0)) | nameBit;
    auto member = state.declaredMethod(vm, ofClass, name.uid());
    if (!member) {
        GetterSetter* accessor = state.declaredAccessor(vm, ofClass, name.uid());
        if (!accessor)
            return std::nullopt;
        m_declaredMemberLookups = lookupsWithThisOne;
        state.didGetDeclaredMember = true;
        slot.setIsTaintedByOpaqueObject();
        slot.setGetterSlot(this, PropertyAttribute::Accessor | PropertyAttribute::DontEnum, accessor);
        return true;
    }
    m_declaredMemberLookups = lookupsWithThisOne;
    state.didGetDeclaredMember = true;
    slot.setIsTaintedByOpaqueObject();
    if (!*member)
        return false;
    slot.setValue(this, static_cast<unsigned>(PropertyAttribute::DontEnum), *member);
    return true;
}

bool ClassPrototype::hasMember(JSObject* object, PropertyName name)
{
    UniquedStringImpl* uid = name.uid();
    if (!uid || uid == object->vm().propertyNames->constructor.impl())
        return !!uid;
    constexpr uint64_t knowsWhetherThenIsDeclared = 1ull << 62;
    constexpr uint64_t declaresThen = 1ull << 63;
    auto* prototype = uncheckedDowncast<ClassPrototype>(object);
    bool isThen = uid == object->vm().propertyNames->then.impl();
    if (isThen && prototype->m_classIndex & knowsWhetherThenIsDeclared)
        return prototype->m_classIndex & declaresThen;
    State& state = *object->structure()->aotInstance()->typed;
    bool isDeclared = state.declaresMethod(*state.classWithIndex(prototype->classIndex()), uid);
    if (isThen)
        prototype->m_classIndex |= knowsWhetherThenIsDeclared | (isDeclared ? declaresThen : 0);
    return isDeclared;
}

bool ClassPrototype::willBeImmutable(JSObject* object)
{
    State& state = *object->structure()->aotInstance()->typed;
    const LayoutData& ofClass = *state.classWithIndex(static_cast<uint32_t>(uncheckedDowncast<ClassPrototype>(object)->m_classIndex));
    return ofClass.hasFixedLayout && !ofClass.constructorMayBeReplaced && std::ranges::none_of(ofClass.methods(), [](auto& method) {
        return method.flags & methodMayBeReplaced;
    });
}

void ClassPrototype::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    createMembers(object);
    JSObject::getOwnPropertyNames(object, globalObject, names, mode);
}

bool ClassPrototype::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    createMembers(asObject(cell));
    return JSObject::put(cell, globalObject, name, value, slot);
}

bool ClassPrototype::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    createMembers(asObject(cell));
    return JSObject::putByIndex(cell, globalObject, index, value, shouldThrow);
}

bool ClassPrototype::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    createMembers(asObject(cell));
    return JSObject::deleteProperty(cell, globalObject, name, slot);
}

bool ClassPrototype::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    createMembers(asObject(cell));
    return JSObject::deletePropertyByIndex(cell, globalObject, index);
}

bool ClassPrototype::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    createMembers(object);
    return JSObject::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
}

bool ClassPrototype::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    createMembers(object);
    return JSObject::preventExtensions(object, globalObject);
}

bool ClassPrototype::isExtensible(JSObject* object, JSGlobalObject* globalObject)
{
    createMembers(object);
    return JSObject::isExtensible(object, globalObject);
}

bool ClassPrototype::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    createMembers(object);
    return JSObject::setPrototype(object, globalObject, prototype, shouldThrowIfCantSet);
}

const ClassInfo GuardedObject::s_info = { "Object"_s, JSFinalObject::info(), nullptr, nullptr, CREATE_METHOD_TABLE(GuardedObject) };

template<typename Visitor>
void GuardedObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSFinalObject::visitChildren(cell, visitor);
}

DEFINE_VISIT_CHILDREN(GuardedObject);

enum class RejectedChange : uint8_t { No, Silently, Thrown };

static RejectedChange checkGuard(JSGlobalObject* globalObject, JSObject* object, unsigned change, UniquedStringImpl* name, unsigned given, JSValue value, bool throws = true)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto guard = vm.declaredObjectGuard;
    if (!guard || (name && name->isSymbol()))
        return RejectedChange::No;
    String refusal = guard(globalObject, object, change, name, given, value);
    RETURN_IF_EXCEPTION(scope, RejectedChange::Thrown);
    if (refusal.isNull())
        return RejectedChange::No;
    if (!throws)
        return RejectedChange::Silently;
    throwTypeError(globalObject, scope, withAOTErrorNote(refusal));
    return RejectedChange::Thrown;
}

enum class StoredBy : uint8_t { UntypedCode, TypedCodeByKey, TypedCode };

static StoredBy storeOrigin(VM& vm)
{
    CallFrame* frame = vm.topCallFrame;
    if (!frame || frame->isNativeCalleeFrame())
        return StoredBy::UntypedCode;
    CodeBlock* codeBlock = frame->codeBlock();
    if (!codeBlock)
        return StoredBy::UntypedCode;
    CodeOrigin origin = frame->codeOrigin();
    if (auto* inlined = origin.inlineCallFrame())
        codeBlock = inlined->baselineCodeBlock.get();
    struct Known {
        CodeBlock* codeBlock { nullptr };
        uint32_t bytecodeOffset { 0 };
        StoredBy storedBy { StoredBy::UntypedCode };
    };
    struct KnownOrigins {
        std::array<Known, 512> entries;
        HeapVersion markingVersion { 0 };
    };
    static LazyNeverDestroyed<ThreadSpecific<KnownOrigins>> s_knownOrigins;
    static std::once_flag s_knownOriginsOnce;
    std::call_once(s_knownOriginsOnce, [] {
        s_knownOrigins.construct();
    });
    KnownOrigins& origins = *s_knownOrigins.get();
    if (HeapVersion now = vm.heap.objectSpace().markingVersion(); now != origins.markingVersion) {
        origins.entries.fill({ });
        origins.markingVersion = now;
    }
    uint32_t bytecodeOffset = origin.bytecodeIndex().offset();
    Known& known = origins.entries[(std::bit_cast<uintptr_t>(codeBlock) / sizeof(void*) + bytecodeOffset * 31) % origins.entries.size()];
    if (known.codeBlock == codeBlock && known.bytecodeOffset == bytecodeOffset)
        return known.storedBy;
    SourceProvider* provider = codeBlock->source().provider();
    const AuditSites* sites = provider ? provider->auditSites() : nullptr;
    StoredBy storedBy = StoredBy::UntypedCode;
    OpcodeID opcode = codeBlock->instructionAt(origin.bytecodeIndex())->opcodeID();
    bool isUnauditedActivation = opcode == op_put_by_val && codeBlock->instructionAt(origin.bytecodeIndex())->as<OpPutByVal>().m_property.isConstant();
    if (sites && !isUnauditedActivation && (opcode == op_put_by_id || opcode == op_put_by_val)) {
        auto info = codeBlock->expressionInfoForBytecodeIndex(origin.bytecodeIndex());
        if (!sites->storesThroughUntyped(info.divot - info.startOffset, info.divot))
            storedBy = opcode == op_put_by_id ? StoredBy::TypedCode : StoredBy::TypedCodeByKey;
    }
    known = { codeBlock, bytecodeOffset, storedBy };
    return storedBy;
}

bool GuardedObject::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    JSObject* object = asObject(cell);
    StoredBy storedBy = slot.thisValue() == JSValue(object) ? storeOrigin(globalObject->vm()) : StoredBy::TypedCode;
    if (storedBy == StoredBy::TypedCode)
        return JSObject::put(cell, globalObject, name, value, slot);
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    RejectedChange refused = checkGuard(globalObject, object, storedBy == StoredBy::TypedCodeByKey ? 5 : 4, name.uid(), 0, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (refused != RejectedChange::No)
        return false;
    scope.release();
    bool isStored = JSObject::put(cell, globalObject, name, value, slot);
    slot.disableCaching();
    return isStored;
}

bool GuardedObject::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    RejectedChange refused = checkGuard(globalObject, asObject(cell), 0, name.uid(), 0, JSValue());
    RETURN_IF_EXCEPTION(scope, false);
    if (refused != RejectedChange::No)
        return false;
    scope.release();
    bool isDeleted = JSObject::deleteProperty(cell, globalObject, name, slot);
    slot.disableCaching();
    return isDeleted;
}

static bool isDefinedByBuiltin(VM& vm)
{
    CallFrame* frame = vm.topCallFrame;
    if (!frame || frame->isNativeCalleeFrame())
        return false;
    CodeBlock* codeBlock = frame->codeBlock();
    if (!codeBlock)
        return true;
    CodeOrigin origin = frame->codeOrigin();
    if (auto* inlined = origin.inlineCallFrame())
        codeBlock = inlined->baselineCodeBlock.get();
    switch (codeBlock->instructionAt(origin.bytecodeIndex())->opcodeID()) {
    case op_call:
    case op_call_ignore_result:
    case op_tail_call:
    case op_call_varargs:
    case op_tail_call_varargs:
        return true;
    default:
        return false;
    }
}

bool GuardedObject::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    unsigned given = descriptor.isAccessorDescriptor() ? 1 : 0;
    if (descriptor.writablePresent())
        given |= descriptor.writable() ? 2 | 4 : 2;
    if (descriptor.enumerablePresent())
        given |= descriptor.enumerable() ? 8 | 16 : 8;
    if (descriptor.configurablePresent())
        given |= descriptor.configurable() ? 32 | 64 : 32;
    if (descriptor.value() && isDefinedByBuiltin(globalObject->vm()))
        given |= 128;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    RejectedChange refused = checkGuard(globalObject, object, 1, name.uid(), given, descriptor.value());
    RETURN_IF_EXCEPTION(scope, false);
    if (refused != RejectedChange::No)
        return false;
    RELEASE_AND_RETURN(scope, JSObject::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow));
}

bool GuardedObject::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    RejectedChange refused = checkGuard(globalObject, object, 2, nullptr, 0, JSValue());
    RETURN_IF_EXCEPTION(scope, false);
    if (refused != RejectedChange::No)
        return false;
    RELEASE_AND_RETURN(scope, JSObject::preventExtensions(object, globalObject));
}

bool GuardedObject::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (object->getPrototypeDirect() != prototype) {
        RejectedChange refused = checkGuard(globalObject, object, 3, nullptr, 0, prototype, shouldThrowIfCantSet);
        RETURN_IF_EXCEPTION(scope, false);
        if (refused != RejectedChange::No)
            return false;
    }
    RELEASE_AND_RETURN(scope, JSObject::setPrototype(object, globalObject, prototype, shouldThrowIfCantSet));
}

const ClassInfo ModuleNamespace::s_info = { "Module"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleNamespace) };

Structure* ModuleNamespace::createStructure(VM& vm, JSGlobalObject* globalObject, bool hasHostPrototype)
{
    JSValue prototype = hasHostPrototype ? globalObject->moduleNamespaceObjectStructure()->storedPrototype() : jsNull();
    return Structure::preventExtensionsTransition(vm, Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info()));
}

ModuleNamespace* ModuleNamespace::create(VM& vm, Structure* structure, JSObject* read, JSCellButterfly* names, JSObject* star, JSCellButterfly* starExportNames, bool isDeferred)
{
    ModuleNamespace* object = new (NotNull, allocateCell<ModuleNamespace>(vm)) ModuleNamespace(vm, structure, read, names, star, starExportNames, isDeferred);
    object->finishCreation(vm);
    return object;
}

ModuleNamespace* ModuleNamespace::createWithLazyNames(VM& vm, Structure* structure, JSObject* read, std::span<const uint32_t> nameStrings, JSObject* star, JSCellButterfly* starExportNames, bool isDeferred)
{
    ModuleNamespace* object = create(vm, structure, read, nullptr, star, starExportNames, isDeferred);
    object->m_nameStrings = nameStrings.data();
    object->m_numberOfNameStrings = nameStrings.size();
    return object;
}

template<typename Visitor>
void ModuleNamespace::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_read);
    visitor.append(thisObject->m_names);
    visitor.append(thisObject->m_star);
    visitor.append(thisObject->m_starExportNames);
}

DEFINE_VISIT_CHILDREN(ModuleNamespace);

static unsigned indexIn(JSCellButterfly* names, UniquedStringImpl* name)
{
    if (name->isSymbol())
        return ModuleNamespace::noExport;
    unsigned from = 0;
    unsigned to = names->length();
    while (from < to) {
        unsigned middle = from + (to - from) / 2;
        const StringImpl* candidate = asString(names->get(middle))->tryGetValueImpl();
        if (candidate == name)
            return middle;
        if (codePointCompare(StringView(*candidate), StringView(*name)) < 0)
            from = middle + 1;
        else
            to = middle;
    }
    return ModuleNamespace::noExport;
}

static String constantString(const Data&, uint32_t index);

unsigned ModuleNamespace::indexOfName(const Data& data, std::span<const uint32_t> nameStrings, UniquedStringImpl* name)
{
    if (name->isSymbol())
        return noExport;
    StringView wanted { *name };
    auto compare = [&](uint32_t string) {
        if (data.stringFacts(string) & Data::stringIsASCII)
            return codePointCompare(StringView(byteCast<Latin1Character>(data.stringInImage(string))), wanted);
        String text = constantString(data, string);
        return codePointCompare(StringView(text), wanted);
    };
    size_t from = 0;
    size_t to = nameStrings.size();
    while (from < to) {
        size_t middle = from + (to - from) / 2;
        auto order = compare(nameStrings[middle]);
        if (order == std::strong_ordering::equal)
            return middle;
        if (order < 0)
            from = middle + 1;
        else
            to = middle;
    }
    return noExport;
}

JSCellButterfly* ModuleNamespace::ensureNames(JSGlobalObject* globalObject)
{
    if (JSCellButterfly* names = m_names.get())
        return names;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = *this->globalObject()->aotInstance()->typed;
    auto* names = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, m_numberOfNameStrings);
    for (unsigned i = 0; i < m_numberOfNameStrings; ++i) {
        JSString* name = state.string(vm, m_nameStrings[i]);
        name->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        names->setIndex(vm, i, name);
    }
    m_names.set(vm, this, names);
    return names;
}

bool ModuleNamespace::isSymbolLikeKey(UniquedStringImpl* name) const
{
    return name->isSymbol() || (m_isDeferred && name == vm().propertyNames->then.impl());
}

bool ModuleNamespace::evaluateIfNeeded(JSGlobalObject* globalObject)
{
    if (!m_mayNeedEvaluation)
        return true;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* read = m_read.get();
    MarkedArgumentBuffer arguments;
    arguments.append(jsNumber(-1));
    call(globalObject, read, JSC::getCallData(read), jsUndefined(), arguments);
    RETURN_IF_EXCEPTION(scope, false);
    m_mayNeedEvaluation = false;
    return true;
}

unsigned ModuleNamespace::numberOf(UniquedStringImpl* name) const
{
    if (isSymbolLikeKey(name))
        return noExport;
    if (JSCellButterfly* names = m_names.get())
        return indexIn(names, name);
    return indexOfName(globalObject()->aotInstance()->typed->data, { m_nameStrings, m_numberOfNameStrings }, name);
}

bool ModuleNamespace::isStarExport(UniquedStringImpl* name) const
{
    return m_starExportNames && !isSymbolLikeKey(name) && indexIn(m_starExportNames.get(), name) != noExport;
}

static JSC_DECLARE_HOST_FUNCTION(readDetachedNamespaceExport);

JSC_DEFINE_HOST_FUNCTION(readDetachedNamespaceExport, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = asObject(callFrame->thisValue())->getDirectIndex(globalObject, callFrame->uncheckedArgument(0).asUInt32());
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        return throwVMError(globalObject, scope, createTDZError(globalObject));
    return JSValue::encode(value);
}

void ModuleNamespace::detach(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    unsigned count = numberOfNames();
    JSArray* values = constructEmptyArray(globalObject, nullptr, count);
    if (scope.exception()) [[unlikely]]
        return;
    JSObject* read = m_read.get();
    for (unsigned number = 0; number < count; ++number) {
        MarkedArgumentBuffer arguments;
        arguments.append(jsNumber(number));
        JSValue value = call(globalObject, read, JSC::getCallData(read), jsUndefined(), arguments);
        if (scope.exception()) [[unlikely]] {
            if (!scope.clearExceptionExceptTermination())
                return;
            continue;
        }
        values->putDirectIndex(globalObject, number, value);
        if (scope.exception()) [[unlikely]]
            return;
    }
    JSFunction* target = JSFunction::create(vm, globalObject, 1, emptyString(), readDetachedNamespaceExport, ImplementationVisibility::Private);
    JSObject* detached = JSBoundFunction::create(vm, globalObject, target, values, { }, 1, nullptr, SourceTaintedOrigin::Untainted);
    if (scope.exception()) [[unlikely]]
        return;
    m_read.set(vm, this, detached);
    m_mayNeedEvaluation = false;
}

JSValue ModuleNamespace::fixedProperty(VM& vm, PropertyName name) const
{
    if (name == vm.propertyNames->toStringTagSymbol)
        return m_isDeferred ? jsNontrivialString(vm, "Deferred Module"_s) : jsNontrivialString(vm, "Module"_s);
    if (name == vm.propertyNames->__esModule && m_hasESModuleFlag)
        return jsBoolean(true);
    return { };
}

bool ModuleNamespace::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    slot.setIsTaintedByOpaqueObject();
    if (slot.internalMethodType() != PropertySlot::InternalMethodType::VMInquiry && !thisObject->isSymbolLikeKey(name.uid())) {
        thisObject->evaluateIfNeeded(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    }
    unsigned number = thisObject->numberOf(name.uid());
    bool isStarExport = number == noExport && thisObject->isStarExport(name.uid());
    if (number == noExport && !isStarExport) {
        JSValue fixed = thisObject->fixedProperty(vm, name);
        if (!fixed || (name == vm.propertyNames->__esModule && slot.internalMethodType() == PropertySlot::InternalMethodType::GetOwnProperty))
            return false;
        slot.setValue(thisObject, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete, fixed);
        return true;
    }
    switch (slot.internalMethodType()) {
    case PropertySlot::InternalMethodType::GetOwnProperty:
    case PropertySlot::InternalMethodType::Get: {
        if (isStarExport) {
            JSValue value = thisObject->m_star->get(globalObject, name);
            RETURN_IF_EXCEPTION(scope, false);
            slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), value);
            return true;
        }
        JSObject* read = thisObject->m_read.get();
        MarkedArgumentBuffer arguments;
        arguments.append(jsNumber(number));
        JSValue value = call(globalObject, read, JSC::getCallData(read), jsUndefined(), arguments);
        RETURN_IF_EXCEPTION(scope, false);
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), value);
        return true;
    }
    case PropertySlot::InternalMethodType::HasProperty:
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), jsUndefined());
        return true;
    case PropertySlot::InternalMethodType::VMInquiry:
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::None), jsUndefined());
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

bool ModuleNamespace::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    return getOwnPropertySlot(object, globalObject, Identifier::from(globalObject->vm(), index), slot);
}

void ModuleNamespace::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    thisObject->evaluateIfNeeded(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    JSCellButterfly* own = thisObject->ensureNames(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    JSCellButterfly* ofStar = thisObject->m_starExportNames.get();
    unsigned numberOfStar = ofStar ? ofStar->length() : 0;
    for (unsigned number = 0, place = 0; number < own->length() || place < numberOfStar;) {
        bool isOwn = place == numberOfStar || (number < own->length() && codePointCompare(StringView(*asString(own->get(number))->tryGetValueImpl()), StringView(*asString(ofStar->get(place))->tryGetValueImpl())) < 0);
        Identifier name = asString(isOwn ? own->get(number++) : ofStar->get(place++))->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (thisObject->isSymbolLikeKey(name.impl()))
            continue;
        if (mode == DontEnumPropertiesMode::Exclude) {
            PropertySlot slot(thisObject, PropertySlot::InternalMethodType::GetOwnProperty);
            getOwnPropertySlot(thisObject, globalObject, name, slot);
            RETURN_IF_EXCEPTION(scope, void());
        }
        names.add(name);
    }
    if (names.includeSymbolProperties() && mode == DontEnumPropertiesMode::Include)
        names.add(vm.propertyNames->toStringTagSymbol);
}

bool ModuleNamespace::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell); name == vm.propertyNames->__esModule && thisObject->numberOf(name.uid()) == noExport && !thisObject->isStarExport(name.uid())) {
        thisObject->m_hasESModuleFlag = value.toBoolean(globalObject);
        return true;
    }
    if (slot.isStrictMode())
        throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
    return false;
}

bool ModuleNamespace::putByIndex(JSCell*, JSGlobalObject* globalObject, unsigned, JSValue, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (shouldThrow)
        throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
    return false;
}

bool ModuleNamespace::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot&)
{
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    VM& vm = globalObject->vm();
    if (!thisObject->isSymbolLikeKey(name.uid()) && !thisObject->evaluateIfNeeded(globalObject))
        return false;
    return thisObject->numberOf(name.uid()) == noExport && !thisObject->isStarExport(name.uid()) && (name == vm.propertyNames->__esModule || !thisObject->fixedProperty(vm, name));
}

bool ModuleNamespace::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    Identifier name = Identifier::from(globalObject->vm(), index);
    if (!thisObject->evaluateIfNeeded(globalObject))
        return false;
    return thisObject->numberOf(name.impl()) == noExport && !thisObject->isStarExport(name.impl());
}

bool ModuleNamespace::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* thisObject = uncheckedDowncast<ModuleNamespace>(object); name == vm.propertyNames->__esModule && descriptor.value().isTrue() && thisObject->numberOf(name.uid()) == noExport && !thisObject->isStarExport(name.uid())) {
        thisObject->m_hasESModuleFlag = true;
        return true;
    }
    PropertyDescriptor current;
    bool isCurrentDefined = object->getOwnPropertyDescriptor(globalObject, name, current);
    RETURN_IF_EXCEPTION(scope, false);
    bool isValid = validateAndApplyPropertyDescriptor(globalObject, nullptr, name, false, descriptor, isCurrentDefined, current, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isValid || (uncheckedDowncast<ModuleNamespace>(object)->numberOf(name.uid()) == noExport && !uncheckedDowncast<ModuleNamespace>(object)->isStarExport(name.uid())))
        return isValid;
    bool isSame = !(descriptor.writablePresent() && !descriptor.writable());
    if (isSame && descriptor.value()) {
        isSame = sameValue(globalObject, descriptor.value(), current.value());
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (!isSame && shouldThrow)
        throwTypeError(globalObject, scope, "Cannot replace module namespace object's binding's value"_s);
    return isSame;
}

const ClassInfo Generator::s_info = { "Generator"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Generator) };

Structure* Generator::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

Generator* Generator::create(VM& vm, Structure* structure, Closure* resumeClosure, bool hasUnmodifiedIteration, uint32_t offsetOfState)
{
    Generator* generator = new (NotNull, allocateCell<Generator>(vm)) Generator(vm, structure, resumeClosure);
    generator->finishCreation(vm);
    generator->m_offsetOfState = offsetOfState;
    generator->m_hasOtherPrototype = !hasUnmodifiedIteration;
    return generator;
}

uint32_t Generator::offsetOfStateIn(Closure* resumeClosure)
{
    return uncheckedDowncast<Object>(resumeClosure->environment())->layout().describedVariables()[CoroutineFrame::stateField].offset;
}

bool Generator::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    uncheckedDowncast<Generator>(object)->m_hasOtherPrototype = true;
    return Base::setPrototype(object, globalObject, prototype, shouldThrowIfCantSet);
}

Generator::Template Generator::asTemplate() const
{
    ASSERT(!m_isRunning);
    Template result { static_cast<uint64_t>(structure()->typeInfoBlob()) << 32 | structure()->id().bits(), 0 };
    memcpy(&result.offsetOfStateAndFlags, &m_offsetOfState, sizeof(uint64_t));
    return result;
}

void State::setGeneratorTemplate(uint32_t index, uint32_t numberOfTemplates, Generator::Template filled)
{
    if (m_generatorTemplates.isEmpty()) {
        m_generatorTemplates.fill(Generator::Template { }, numberOfTemplates);
        instance.generatorTemplates = m_generatorTemplates.mutableSpan().data();
    }
    m_generatorTemplates[index] = filled;
}

Generator* Generator::forBodyFrame(const uint64_t* frame)
{
    uint32_t afterCall;
    memcpy(&afterCall, std::bit_cast<const void*>(frame[1]), sizeof(afterCall));
    return afterCall == markerAfterBodyCall ? std::bit_cast<Generator* const*>(frame[0])[generatorSlotInStubFrame] : nullptr;
}

void Generator::didThrow()
{
    m_isRunning = false;
    state() = CoroutineFrame::completedState;
}

template<typename Visitor>
void Generator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    visitor.appendUnbarriered(uncheckedDowncast<Generator>(cell)->m_resumeClosure);
}

DEFINE_VISIT_CHILDREN(Generator);

const ClassInfo SavedFrame::s_info = { "SavedFrame"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(SavedFrame) };

Structure* SavedFrame::createStructure(VM& vm, Instance& instance)
{
    Structure* structure = Structure::create(vm, instance.globalObject, jsNull(), TypeInfo(ObjectType, StructureFlags), info());
    structure->setAOTInstance(&instance);
    return structure;
}

Structure* State::savedFrameStructure(VM& vm)
{
    if (!m_savedFrameStructure) {
        m_savedFrameStructure = SavedFrame::createStructure(vm, instance);
        didCreate(m_savedFrameStructure);
        instance.savedFrameSpace = &vm.cellSpace();
        instance.savedFrameHeader = static_cast<uint64_t>(m_savedFrameStructure->typeInfoBlob()) << 32 | m_savedFrameStructure->id().bits();
    }
    return m_savedFrameStructure;
}

SavedFrame* SavedFrame::forFrame(const Instance& instance, const uint64_t* frame)
{
    if (std::bit_cast<const void*>(frame[1]) != instance.savedFrameReturnAddress)
        return nullptr;
    return uncheckedDowncast<SavedFrame>(std::bit_cast<CallFrame*>(frame[0])->jsCallee());
}

bool SavedFrame::isEnteredByJobLoop(const Instance& instance, const uint64_t* frame)
{
    auto* returnAddress = std::bit_cast<const void*>(frame[1]);
    if (returnAddress != instance.savedFrameReturnAddress && returnAddress != instance.resumeFunctionReturnAddress()) {
        if (AsyncGenerator::wasEnteredByStub(frame))
            return std::bit_cast<const uint64_t*>(frame[0])[AsyncGenerator::modeSlotInStubFrame] & CoroutineFrame::isEnteredByJobLoop;
        if (!AsyncGenerator::wasEnteredByJobLoopAdapter(frame))
            return false;
    }
    return std::bit_cast<CallFrame*>(frame[0])->uncheckedArgument(0).asInt32() & CoroutineFrame::isEnteredByJobLoop;
}

SavedFrame* SavedFrame::save(VM& vm, Instance& instance, uint64_t* frame, const uint64_t* bottom, void* resumeAddress, const uint64_t* extra)
{
    size_t numberOfWords = frame - bottom;
    SavedFrame* saved = forFrame(instance, frame);
    if (!saved) {
        Structure* structure = instance.typed->savedFrameStructure(vm);
        saved = new (NotNull, allocateCell<SavedFrame>(vm, allocationSize(numberOfWords))) SavedFrame(vm, structure, static_cast<uint32_t>(numberOfWords));
        saved->finishCreation(vm);
        uint32_t place = static_cast<uint32_t>(extra[0] >> promiseLocationShift);
        uint64_t promise = place == locationInRegister ? extra[1] : frame[-static_cast<intptr_t>(place)];
        saved->m_promiseOrWaiter = promise ? std::bit_cast<JSCell*>(promise) : saved;
    }
    RELEASE_ASSERT(saved->m_numberOfWords == numberOfWords && !saved->m_liveSlotMapOffsetPlusOne);
    saved->willWrite();
    uint64_t* words = saved->words();
    for (size_t i = 0; i < numberOfWords; ++i)
        words[i] = bottom[i];
    saved->m_register = extra[isARM64() ? 0 : 1];
    saved->m_resumeAddress = resumeAddress;
    saved->m_liveSlotMapOffsetPlusOne = (static_cast<uint32_t>(extra[0]) & ((1u << liveSlotMapOffsetBits) - 1)) + 1;
    saved->didWrite();
    vm.writeBarrier(saved);
    return saved;
}

template<typename Visitor>
void SavedFrame::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    auto* saved = uncheckedDowncast<SavedFrame>(cell);
    visitor.appendUnbarriered(saved->m_promiseOrWaiter);
    visitor.appendUnbarriered(JSValue::decode(saved->m_asyncContext));
    uint32_t version = saved->m_version;
    WTF::loadLoadFence();
    Vector<JSCell*, 32> cells;
    bool isInKnownFunction = true;
    if (uint32_t liveSlotMapOffsetPlusOne = saved->m_liveSlotMapOffsetPlusOne; liveSlotMapOffsetPlusOne && !(version & 1)) {
        ImageAddressInfo where = classifyAddress(static_cast<const uint8_t*>(saved->m_resumeAddress) - 1);
        isInKnownFunction = where.kind == ImageAddressInfo::Function;
        if (where.kind == ImageAddressInfo::Function) {
            const uint32_t* row = saved->structure()->aotInstance()->infos[where.index].function()->plans() + liveSlotMapOffsetPlusOne - 1;
            for (uint32_t i = 0; i < row[0]; ++i) {
                for (uint32_t bits = row[1 + i]; bits; bits &= bits - 1) {
                    uint32_t place = i * 32 + ctz(bits);
                    uint64_t word = place == locationInRegister ? saved->m_register : saved->words()[saved->m_numberOfWords - place];
                    JSValue value = JSValue::decode(static_cast<EncodedJSValue>(word));
                    if (value && value.isCell())
                        cells.append(value.asCell());
                }
            }
        }
    }
    WTF::loadLoadFence();
    if (version & 1 || saved->m_version != version) {
        visitor.didRace(cell, "a SavedFrame is being written");
        return;
    }
    RELEASE_ASSERT(isInKnownFunction);
    for (JSCell* kept : cells)
        visitor.appendUnbarriered(kept);
}

DEFINE_VISIT_CHILDREN(SavedFrame);

JSPromise* SavedFrame::promise() const
{
    return m_promiseOrWaiter->type() == JSPromiseType ? uncheckedDowncast<JSPromise>(m_promiseOrWaiter) : nullptr;
}

SavedFrame* SavedFrame::waiter() const
{
    return m_promiseOrWaiter->type() != ObjectType || m_promiseOrWaiter == this ? nullptr : uncheckedDowncast<SavedFrame>(m_promiseOrWaiter);
}

JSPromiseCombinatorsGlobalContext* SavedFrame::promiseAllContext() const
{
    return m_promiseOrWaiter->type() == JSPromiseCombinatorsGlobalContextType ? uncheckedDowncast<JSPromiseCombinatorsGlobalContext>(m_promiseOrWaiter) : nullptr;
}

void SavedFrame::setPromiseAllContext(VM& vm, JSPromiseCombinatorsGlobalContext* context, uint32_t index)
{
    RELEASE_ASSERT(m_promiseOrWaiter == this);
    m_indexInPromiseAll = index;
    m_promiseOrWaiter = context;
    vm.writeBarrier(this);
}

void SavedFrame::setWaiter(VM& vm, SavedFrame* waiter, JSValue asyncContext)
{
    RELEASE_ASSERT(m_promiseOrWaiter == this);
    waiter->m_asyncContext = JSValue::encode(asyncContext ? asyncContext : jsUndefined());
    vm.writeBarrier(waiter);
    m_promiseOrWaiter = waiter;
    vm.writeBarrier(this);
}

void SavedFrame::fulfill(JSGlobalObject* globalObject, VM& vm, JSValue value)
{
    if (SavedFrame* waiter = this->waiter()) {
        globalObject->queueMicrotask(vm, InternalMicrotask::AOTAsyncFunctionResume, static_cast<uint8_t>(JSPromise::Status::Fulfilled), jsUndefined(), value, waiter, JSValue::decode(waiter->m_asyncContext));
        return;
    }
    if (auto* context = promiseAllContext()) {
        globalObject->queueMicrotask(vm, InternalMicrotask::PromiseAllResolveJob, static_cast<uint8_t>(JSPromise::Status::Fulfilled), context, value, jsNumber(m_indexInPromiseAll));
        return;
    }
    ensurePromise(globalObject, vm)->fulfill(vm, value);
}

void SavedFrame::reject(JSGlobalObject* globalObject, VM& vm, JSValue error)
{
    if (SavedFrame* waiter = this->waiter()) {
        globalObject->queueMicrotask(vm, InternalMicrotask::AOTAsyncFunctionResume, static_cast<uint8_t>(JSPromise::Status::Rejected), jsUndefined(), error, waiter, JSValue::decode(waiter->m_asyncContext));
        return;
    }
    if (auto* context = promiseAllContext()) {
        globalObject->queueMicrotask(vm, InternalMicrotask::PromiseAllResolveJob, static_cast<uint8_t>(JSPromise::Status::Rejected), context, error, jsNumber(m_indexInPromiseAll));
        return;
    }
    ensurePromise(globalObject, vm)->reject(vm, error);
}

JSPromise* SavedFrame::ensurePromise(JSGlobalObject* globalObject, VM& vm)
{
    if (JSPromise* promise = this->promise())
        return promise;
    SavedFrame* waiter = this->waiter();
    auto* context = promiseAllContext();
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    if (waiter)
        promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::AOTAsyncFunctionResume, nullptr, waiter, JSValue::decode(waiter->m_asyncContext));
    else if (context)
        promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::PromiseAllResolveJob, context, jsNumber(m_indexInPromiseAll));
    m_promiseOrWaiter = promise;
    vm.writeBarrier(this);
    return promise;
}

static JSValue resumeCoroutine(JSGlobalObject* globalObject, Closure* resumeClosure, int32_t mode, JSValue value, JSCell* context = nullptr)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    RELEASE_ASSERT(!resumeClosure->mayBeCalled());
    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return { };
    }
    JSValue result = JSValue::decode(vmEntryToJavaScriptWith2Arguments(resumeEntrypoint().taggedPtr(), &vm, nullptr, resumeClosure, jsUndefined(), context, jsNumber(mode), value));
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static void* s_resumeEntrypoint;

static ALWAYS_INLINE JSValue enterResumeFunction(VM& vm, Closure* resumeClosure, int32_t mode, JSValue value)
{
    if (!s_resumeEntrypoint) [[unlikely]]
        s_resumeEntrypoint = resumeEntrypoint().taggedPtr();
    return JSValue::decode(vmEntryToJavaScriptWith2Arguments(s_resumeEntrypoint, &vm, nullptr, resumeClosure, jsUndefined(), nullptr, jsNumber(mode), value));
}

JSValue Generator::resume(JSGlobalObject* globalObject, ResumeMode mode, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isDone;
    JSValue result = resume(globalObject, mode, value, isDone);
    RETURN_IF_EXCEPTION(scope, { });
    return createIteratorResultObject(globalObject, result, isDone);
}

bool Generator::hasUnmodifiedIteration(JSGlobalObject* globalObject, Structure* structure)
{
    JSObject* prototype = structure->storedPrototypeObject();
    if (structure->maxOffset() != invalidOffset || structure->inlineCapacity() || !prototype || prototype->type() != FinalObjectType || prototype->structure()->maxOffset() != invalidOffset || !prototype->structure()->hasImmutableProperties())
        return false;
    JSObject* shared = globalObject->generatorPrototype();
    return prototype->getPrototypeDirect() == JSValue(shared) && shared->getPrototypeDirect() == JSValue(globalObject->iteratorPrototype());
}

int32_t& Generator::state() const
{
    return uncheckedDowncast<Object>(m_resumeClosure->environment())->at<int32_t>(m_offsetOfState);
}

bool Generator::isCompleted() const
{
    return state() == CoroutineFrame::completedState;
}

bool Generator::isSuspendedAtStart() const
{
    return !m_isRunning && !state();
}

JSValue Generator::resume(JSGlobalObject* globalObject, ResumeMode mode, JSValue value, bool& isDone)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    isDone = true;
    if (m_isRunning) {
        throwTypeError(globalObject, scope, "Generator is executing"_s);
        return { };
    }
    int32_t& state = this->state();
    if (!state && mode != ResumeMode::Next)
        state = CoroutineFrame::completedState;
    if (state == CoroutineFrame::completedState) {
        if (mode == ResumeMode::Throw) {
            throwException(globalObject, scope, value);
            return { };
        }
        return mode == ResumeMode::Return ? value : jsUndefined();
    }
    m_isRunning = true;
    JSValue result = resumeCoroutine(globalObject, m_resumeClosure, static_cast<int32_t>(mode), value);
    m_isRunning = false;
    if (scope.exception()) [[unlikely]] {
        state = CoroutineFrame::completedState;
        return { };
    }
    isDone = state == CoroutineFrame::completedState;
    return result;
}

const ClassInfo AsyncGenerator::s_info = { "AsyncGenerator"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(AsyncGenerator) };

Structure* AsyncGenerator::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

AsyncGenerator* AsyncGenerator::create(VM& vm, JSGlobalObject*, Structure* structure, Closure* resumeClosure)
{
    auto* frame = uncheckedDowncast<Object>(resumeClosure->environment());
    int32_t* state = &frame->at<int32_t>(frame->layout().describedVariables()[CoroutineFrame::stateField].offset);
    AsyncGenerator* generator = new (NotNull, allocateCell<AsyncGenerator>(vm)) AsyncGenerator(vm, structure, resumeClosure, state);
    generator->finishCreation(vm);
    State& typedState = *resumeClosure->structure()->aotInstance()->typed;
    generator->set(canBeEnteredByStubFlag, typedState.stubCanEnterAsyncGenerator(resumeClosure));
    if (JSPromise* step = typedState.asyncGeneratorImmediateStep)
        generator->m_immediateStep.set(vm, generator, step);
    if (!typedState.instance.iteratorResultHeader) [[unlikely]] {
        Structure* ofResults = typedState.instance.globalObject->iteratorResultObjectStructure();
        typedState.instance.iteratorResultHeader = static_cast<uint64_t>(ofResults->typeInfoBlob()) << 32 | ofResults->id().bits();
    }
    return generator;
}

bool State::stubCanEnterAsyncGenerator(Closure* resumeClosure)
{
    uint32_t resumeFunction = resumeClosure->functionIndex();
    uint64_t& answer = m_asyncGeneratorStubEntryResults[resumeFunction % std::size(m_asyncGeneratorStubEntryResults)];
    if (answer >> 1 != resumeFunction) {
        ImageAddressInfo body = classifyAddress(instance.code + (resumeClosure->entry() & ClosureWord::offsetMask) * ClosureWord::offsetUnit);
        answer = static_cast<uint64_t>(resumeFunction) << 1 | (body.kind == ImageAddressInfo::Function && AsyncGenerator::bodyCapturesExceptions(resumeFunction) && instance.frameSizeInBytesOf(body.index) <= AsyncGenerator::maxBodyFrameSizeForStubEntry);
    }
    return answer & 1;
}

template<typename Visitor>
void AsyncGenerator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    auto* generator = uncheckedDowncast<AsyncGenerator>(cell);
    visitor.appendUnbarriered(generator->m_resumeClosure);
    if (JSObject* target = generator->m_target.pointer())
        visitor.appendUnbarriered(target);
    visitor.append(generator->m_value);
    visitor.append(generator->m_yieldedInPlace);
    visitor.append(generator->m_lastRequest);
    visitor.append(generator->m_cachedDriverResult);
    visitor.append(generator->m_cachedDriverResultTarget);
    visitor.append(generator->m_immediateStep);
}

DEFINE_VISIT_CHILDREN(AsyncGenerator);

static void enterJobLoop(JSGlobalObject*, VM&, Instance&, AsyncGenerator* first, JSValue);

static ALWAYS_INLINE bool nextJobRunsImmediately(JSGlobalObject* globalObject, VM& vm)
{
    return globalObject->microtaskQueue().isEmpty() && globalObject->canFastQueueMicrotask() && !vm.hostObservesMicrotaskBoundaries();
}

static ALWAYS_INLINE bool isSettledForAwait(JSGlobalObject* globalObject, JSValue awaited)
{
    if (!awaited.isObject())
        return true;
    if (awaited.asCell()->structureID() == globalObject->promiseStructure()->id())
        return uncheckedDowncast<JSPromise>(awaited)->status() == JSPromise::Status::Fulfilled;
    return !awaited.inherits<JSPromise>() && isDefinitelyNonThenable(asObject(awaited), globalObject);
}

static ALWAYS_INLINE JSValue valueOfSettled(JSGlobalObject* globalObject, JSValue awaited)
{
    if (!awaited.isObject() || awaited.asCell()->structureID() != globalObject->promiseStructure()->id())
        return awaited;
    auto* promise = uncheckedDowncast<JSPromise>(awaited);
    promise->markAsHandled();
    return promise->result();
}

bool AsyncGenerator::tryToYieldInPlace(Instance& instance, VM& vm, JSValue operand)
{
    uint16_t flags = m_target.type();
    if (!(flags & isEnteredByJobLoopFlag) || instance.awaitMustSuspend || !instance.microtaskQueue->isEmpty())
        return false;
    if (operand.isCell() && operand.asCell()->type() >= ObjectType) {
        JSGlobalObject* globalObject = instance.globalObject;
        if (operand.asCell()->type() == JSPromiseType) {
            if (!isSettledForAwait(globalObject, operand))
                return false;
            operand = valueOfSettled(globalObject, operand);
        } else if (!isDefinitelyNonThenable(asObject(operand), globalObject))
            return false;
    }
    m_yieldedInPlace.set(vm, this, operand);
    m_target.setType(flags | isYieldingInPlaceFlag);
    return true;
}

void AsyncGenerator::append(VM& vm, Generator::ResumeMode mode, JSValue value, JSObject* target)
{
    if (!m_target.pointer()) {
        m_target.setPointer(target);
        m_target.setType((m_target.type() & ~modeMask) | static_cast<uint8_t>(mode));
        vm.writeBarrier(this, target);
        m_value.set(vm, this, value);
        return;
    }
    JSSlimPromiseReaction* last = m_lastRequest.get();
    auto* request = JSSlimPromiseReaction::createAsyncGeneratorRequest(vm, target, value, static_cast<uint8_t>(mode), last ? last->next() : nullptr);
    if (last)
        last->setNext(vm, request);
    else
        request->setNext(vm, request);
    m_lastRequest.set(vm, this, request);
}

JSObject* AsyncGenerator::takeFrontRequest(VM& vm)
{
    JSObject* target = m_target.pointer();
    JSSlimPromiseReaction* last = m_lastRequest.get();
    if (!last) {
        m_target.setPointer(nullptr);
        m_value.clear();
        return target;
    }
    auto* first = uncheckedDowncast<JSSlimPromiseReaction>(last->next());
    JSObject* next = asObject(first->promise());
    m_target.setPointer(next);
    m_target.setType((m_target.type() & ~modeMask) | first->asyncGeneratorResumeMode());
    vm.writeBarrier(this, next);
    m_value.set(vm, this, first->handlerOrContext());
    if (first == last)
        m_lastRequest.clear();
    else
        last->setNext(vm, first->next());
    return target;
}

void AsyncGenerator::appendRequestTargets(Vector<JSCell*, 4>& targets) const
{
    JSObject* front = m_target.pointer();
    if (!front)
        return;
    if (front != this)
        targets.append(front);
    JSSlimPromiseReaction* last = m_lastRequest.get();
    if (!last)
        return;
    for (auto* request = uncheckedDowncast<JSSlimPromiseReaction>(last->next());; request = uncheckedDowncast<JSSlimPromiseReaction>(request->next())) {
        targets.append(asObject(request->promise()));
        if (request == last)
            return;
    }
}

JSPromise* AsyncGenerator::enqueue(JSGlobalObject* globalObject, Generator::ResumeMode mode, JSValue value)
{
    VM& vm = globalObject->vm();
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    append(vm, mode, value, promise);
    if (!is(isBusyFlag))
        drain(globalObject);
    return promise;
}

bool AsyncGenerator::hasUnmodifiedIteration(JSGlobalObject* globalObject)
{
    if (structure()->maxOffset() != invalidOffset) {
        VM& vm = globalObject->vm();
        if (structure()->get(vm, vm.propertyNames->next) != invalidOffset || structure()->get(vm, vm.propertyNames->asyncIteratorSymbol) != invalidOffset)
            return false;
    }
    JSObject* prototype = getPrototypeDirect().getObject();
    return prototype && prototype->type() == FinalObjectType && prototype->structure()->maxOffset() == invalidOffset && prototype->getPrototypeDirect() == JSValue(globalObject->asyncGeneratorPrototype()) && globalObject->asyncGeneratorPrototype()->getPrototypeDirect() == JSValue(globalObject->asyncIteratorPrototype());
}

ALWAYS_INLINE JSValue AsyncGenerator::enter(VM& vm, int32_t mode, JSValue value)
{
    State& typedState = *m_resumeClosure->structure()->aotInstance()->typed;
    AsyncGenerator* enteredOutside = std::exchange(typedState.enteredAsyncGenerator, this);
    JSValue result = enterResumeFunction(vm, m_resumeClosure, mode, value);
    typedState.enteredAsyncGenerator = enteredOutside;
    return result;
}

void AsyncGenerator::startEagerly(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& typedState = *m_resumeClosure->structure()->aotInstance()->typed;
    AsyncGenerator* enteredOutside = std::exchange(typedState.enteredAsyncGenerator, this);
    resumeCoroutine(globalObject, m_resumeClosure, static_cast<int32_t>(Generator::ResumeMode::Next), jsUndefined());
    typedState.enteredAsyncGenerator = enteredOutside;
    RETURN_IF_EXCEPTION(scope, void());
    if (!is(hasThrownFlag)) [[likely]]
        return;
    JSValue error = m_yieldedInPlace.get();
    m_yieldedInPlace.clear();
    set(hasThrownFlag, false);
    state() = CoroutineFrame::completedState;
    throwException(globalObject, scope, error);
}

void AsyncGenerator::didThrow(VM& vm, JSValue error)
{
    m_yieldedInPlace.set(vm, this, error);
    set(hasThrownFlag, true);
}

static const Data& dataInImage();

bool AsyncGenerator::bodyCapturesExceptions(uint32_t resumeFunction)
{
    const Data& data = dataInImage();
    return std::ranges::binary_search(std::span { data.at<uint32_t>(data.asyncGeneratorBodiesOffset), data.numberOfAsyncGeneratorBodies }, resumeFunction);
}

bool AsyncGenerator::wasEnteredByStub(const uint64_t* frame)
{
    uint32_t afterCall;
    memcpy(&afterCall, std::bit_cast<const void*>(frame[1]), sizeof(afterCall));
    return afterCall == markerAfterBodyCall;
}

bool AsyncGenerator::wasEnteredByJobLoopAdapter(const uint64_t* frame)
{
    uint32_t afterCall;
    memcpy(&afterCall, std::bit_cast<const void*>(frame[1]), sizeof(afterCall));
    return afterCall == markerAfterBodyCallInJobLoop;
}

AsyncGenerator* AsyncGenerator::forBodyFrame(const Instance& instance, const uint64_t* frame)
{
    if (wasEnteredByJobLoopAdapter(frame))
        return std::bit_cast<AsyncGenerator* const*>(frame[0])[static_cast<int>(CallFrameSlot::callee)];
    if (std::bit_cast<const void*>(frame[1]) == instance.resumeFunctionReturnAddress())
        return instance.typed->enteredAsyncGenerator;
    if (wasEnteredByStub(frame))
        return std::bit_cast<AsyncGenerator* const*>(frame[0])[generatorSlotInStubFrame];
    return nullptr;
}

JSPromise* AsyncGenerator::immediateStep(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    JSPromise* step = m_immediateStep.get();
    if (!step) [[unlikely]] {
        State& typedState = *m_resumeClosure->structure()->aotInstance()->typed;
        step = typedState.asyncGeneratorImmediateStep;
        if (!step) {
            step = JSPromise::create(vm, globalObject->promiseStructure());
            step->fulfill(vm, createIteratorResultObject(globalObject, jsUndefined(), false));
            typedState.asyncGeneratorImmediateStep = step;
        }
        m_immediateStep.set(vm, this, step);
    }
    JSObject* result = asObject(step->result());
    result->putDirectOffset(vm, iteratorResultObjectValuePropertyOffset, value);
    result->putDirectOffset(vm, iteratorResultObjectDonePropertyOffset, jsBoolean(false));
    return step;
}

JSValue AsyncGenerator::enqueueDriver(JSGlobalObject* globalObject, JSValue value, bool driverCanContinueImmediately)
{
    VM& vm = globalObject->vm();
    if (m_target.pointer())
        return enqueue(globalObject, Generator::ResumeMode::Next, value);
    static_assert(!static_cast<uint16_t>(Generator::ResumeMode::Next));
    uint16_t flags = m_target.type();
    m_value.set(vm, this, value);
    if (!(flags & isBusyFlag) && state() != CoroutineFrame::completedState && vm.isSafeToRecurseSoft()) [[likely]] {
        m_target = { this, static_cast<uint16_t>((flags & ~(modeMask | isSuspendingAtAwaitFlag | isEnteredByJobLoopFlag)) | isDrainingBeforeDriverFlag | isBusyFlag | (driverCanContinueImmediately ? isEnteredByJobLoopFlag : 0)) };
        return finishStepForDriver(globalObject, enter(vm, static_cast<int32_t>(Generator::ResumeMode::Next) | (driverCanContinueImmediately ? CoroutineFrame::isEnteredByJobLoop : 0), value));
    }
    m_target = { this, static_cast<uint16_t>((flags & ~modeMask) | isDrainingBeforeDriverFlag) };
    return valueForDriverToAwait(globalObject);
}

JSValue AsyncGenerator::finishStepForDriver(JSGlobalObject* globalObject, JSValue result)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    uint16_t flags = m_target.type();
    bool hasThrown = scope.exception() || flags & hasThrownFlag;
    if (!hasThrown && flags & isSuspendingAtAwaitFlag && m_target.pointer() == this) [[likely]] {
        m_target.setType(flags & ~(isDrainingBeforeDriverFlag | isEnteredByJobLoopFlag));
        return this;
    }
    if (!hasThrown && flags & isYieldingInPlaceFlag && m_target.pointer() == this && !m_lastRequest) [[likely]] {
        JSValue yielded = m_yieldedInPlace.get();
        m_yieldedInPlace.clear();
        m_value.clear();
        m_target = { nullptr, static_cast<uint16_t>(flags & ~(isDrainingBeforeDriverFlag | isEnteredByJobLoopFlag | isBusyFlag | isYieldingInPlaceFlag)) };
        return immediateStep(globalObject, yielded);
    }
    if (!hasThrown && !(flags & (isSuspendingAtAwaitFlag | isYieldingInPlaceFlag)) && state() == CoroutineFrame::completedState && m_target.pointer() == this && !m_lastRequest) {
        m_value.clear();
        m_target = { nullptr, static_cast<uint16_t>(flags & ~(isDrainingBeforeDriverFlag | isEnteredByJobLoopFlag | isBusyFlag)) };
        if (!globalObject->microtaskQueue().isEmpty()) {
            JSPromise* ownStep = JSPromise::create(vm, globalObject->promiseStructure());
            ownStep->fulfill(vm, createIteratorResultObject(globalObject, result, true));
            return ownStep;
        }
        JSPromise* step = immediateStep(globalObject, result);
        asObject(step->result())->putDirectOffset(vm, iteratorResultObjectDonePropertyOffset, jsBoolean(true));
        return step;
    }
    finishRun(globalObject, result, nullptr);
    return valueForDriverToAwait(globalObject);
}

JSValue AsyncGenerator::valueForDriverToAwait(JSGlobalObject* globalObject)
{
    if (!is(isBusyFlag) && m_target.pointer()) [[unlikely]]
        drain(globalObject);
    set(isDrainingBeforeDriverFlag, false);
    if (isAwaitingDriver())
        return this;
    JSObject* promise = m_cachedDriverResultTarget.get();
    m_cachedDriverResultTarget.clear();
    return promise;
}

void AsyncGenerator::setDriver(VM& vm, JSObject* driver, JSValue asyncContext)
{
    m_target.setPointer(driver);
    vm.writeBarrier(this, driver);
    m_value.set(vm, this, asyncContext);
}

void AsyncGenerator::settleFrontRequest(JSGlobalObject* globalObject, JSValue value, bool isDone, bool isRejected, DriverToResume* atOnce)
{
    VM& vm = globalObject->vm();
    JSValue asyncContext = m_value.get();
    JSObject* target = takeFrontRequest(vm);
    if (target == this) {
        target = JSPromise::create(vm, globalObject->promiseStructure());
        m_cachedDriverResult.clear();
        m_cachedDriverResultTarget.set(vm, this, target);
    }
    if (auto* promise = target->type() == JSPromiseType ? uncheckedDowncast<JSPromise>(target) : nullptr) {
        if (isRejected)
            promise->reject(vm, value);
        else
            promise->resolve(globalObject, vm, createIteratorResultObject(globalObject, value, isDone));
        return;
    }
    JSObject* driver = target;
    InternalMicrotask task = driver->type() == JSFunctionType || driver->classInfo() == SavedFrame::info() ? InternalMicrotask::AOTAsyncFunctionResume : InternalMicrotask::AOTAsyncGeneratorResume;
    bool resumesImmediately = atOnce && !atOnce->driver && nextJobRunsImmediately(globalObject, vm) && vm.isSafeToRecurseSoft();
    if (isRejected) {
        if (resumesImmediately)
            *atOnce = { driver, value, asyncContext, true };
        else
            globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(JSPromise::Status::Rejected), jsUndefined(), value, driver, asyncContext);
        return;
    }
    JSObject* result = m_cachedDriverResult.get();
    if (result && m_cachedDriverResultTarget.get() == driver) {
        result->putDirectOffset(vm, iteratorResultObjectValuePropertyOffset, value);
        result->putDirectOffset(vm, iteratorResultObjectDonePropertyOffset, jsBoolean(isDone));
    } else {
        result = createIteratorResultObject(globalObject, value, isDone);
        m_cachedDriverResult.set(vm, this, result);
        m_cachedDriverResultTarget.set(vm, this, driver);
    }
    if (resumesImmediately)
        *atOnce = { driver, result, asyncContext, false };
    else
        globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(JSPromise::Status::Fulfilled), jsUndefined(), result, driver, asyncContext);
}

void AsyncGenerator::drain(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    while (!is(isBusyFlag) && m_target.pointer()) {
        auto mode = static_cast<Generator::ResumeMode>(m_target.type() & modeMask);
        JSValue value = m_value.get();
        if (!state() && mode != Generator::ResumeMode::Next)
            state() = CoroutineFrame::completedState;
        if (state() == CoroutineFrame::completedState) {
            switch (mode) {
            case Generator::ResumeMode::Next:
                settleFrontRequest(globalObject, jsUndefined(), true, false);
                continue;
            case Generator::ResumeMode::Throw:
                settleFrontRequest(globalObject, value, true, true);
                continue;
            case Generator::ResumeMode::Return:
                set(isBusyFlag | isAwaitingReturnFlag, true);
                JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, value, InternalMicrotask::AOTAsyncGeneratorResume, this);
                return;
            }
        }
        run(globalObject, static_cast<int32_t>(mode), value);
    }
}

void AsyncGenerator::run(JSGlobalObject* globalObject, int32_t mode, JSValue value, DriverToResume* atOnce)
{
    m_target.setType((m_target.type() & ~(isSuspendingAtAwaitFlag | isEnteredByJobLoopFlag)) | isBusyFlag | (atOnce ? isEnteredByJobLoopFlag : 0));
    State& typedState = *m_resumeClosure->structure()->aotInstance()->typed;
    AsyncGenerator* enteredOutside = std::exchange(typedState.enteredAsyncGenerator, this);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    JSValue result = resumeCoroutine(globalObject, m_resumeClosure, mode | (atOnce ? CoroutineFrame::isEnteredByJobLoop : 0), value);
    EXCEPTION_ASSERT_UNUSED(scope, !scope.exception() || !result);
    typedState.enteredAsyncGenerator = enteredOutside;
    finishRun(globalObject, result, atOnce);
}

void AsyncGenerator::finishRun(JSGlobalObject* globalObject, JSValue result, DriverToResume* atOnce)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    if (Exception* exception = scope.exception()) [[unlikely]] {
        JSValue error = exception->value();
        set(isEnteredByJobLoopFlag, false);
        if (!scope.clearExceptionExceptTermination())
            return;
        state() = CoroutineFrame::completedState;
        set(isBusyFlag, false);
        settleFrontRequest(globalObject, error, true, true, atOnce);
        return;
    }
    uint16_t flags = m_target.type();
    if (flags & hasThrownFlag) [[unlikely]] {
        JSValue error = m_yieldedInPlace.get();
        m_yieldedInPlace.clear();
        m_target.setType(flags & ~(hasThrownFlag | isEnteredByJobLoopFlag | isBusyFlag));
        state() = CoroutineFrame::completedState;
        settleFrontRequest(globalObject, error, true, true, atOnce);
        return;
    }
    if (flags & isSuspendingAtAwaitFlag) {
        m_target.setType(flags & ~isEnteredByJobLoopFlag);
        return;
    }
    m_target.setType(flags & ~(isEnteredByJobLoopFlag | isBusyFlag | isYieldingInPlaceFlag));
    if (flags & isYieldingInPlaceFlag) {
        result = m_yieldedInPlace.get();
        m_yieldedInPlace.clear();
    }
    settleFrontRequest(globalObject, result, state() == CoroutineFrame::completedState, false, atOnce);
}

void AsyncGenerator::finishAfterJobLoop(JSGlobalObject* globalObject, JSValue result)
{
    finishRun(globalObject, result, nullptr);
    drain(globalObject);
}

void AsyncGenerator::resumeFromAwait(JSGlobalObject* globalObject, JSValue resolution, bool isRejected, bool isEnteredByJobLoop)
{
    if (uint16_t flags = m_target.type(); isEnteredByJobLoop && !isRejected && flags & canBeEnteredByStubFlag && !(flags & (isAwaitingReturnFlag | isAwaitingYieldOperandFlag)) && std::bit_cast<uintptr_t>(currentStackPointer()) >= std::bit_cast<uintptr_t>(globalObject->vm().softStackLimit()) + stackSpaceForJobLoop) [[likely]] {
        enterJobLoop(globalObject, globalObject->vm(), *m_resumeClosure->structure()->aotInstance(), this, resolution);
        return;
    }
    DriverToResume driverToResume;
    DriverToResume* atOnce = isEnteredByJobLoop ? &driverToResume : nullptr;
    VM& vm = globalObject->vm();
    if (uint16_t flags = m_target.type(); isEnteredByJobLoop && !isRejected && !(flags & (isAwaitingReturnFlag | isAwaitingYieldOperandFlag)) && vm.isSafeToRecurseSoft()) [[likely]] {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        m_target.setType((flags & ~isSuspendingAtAwaitFlag) | isBusyFlag | isEnteredByJobLoopFlag);
        JSValue result = enter(vm, CoroutineFrame::isEnteredByJobLoop, resolution);
        flags = m_target.type();
        if (!scope.exception() && !(flags & hasThrownFlag)) [[likely]] {
            if (flags & isSuspendingAtAwaitFlag) {
                m_target.setType(flags & ~isEnteredByJobLoopFlag);
                return;
            }
            JSObject* driver = m_target.pointer();
            JSObject* iteratorResult = m_cachedDriverResult.get();
            if (flags & isYieldingInPlaceFlag && !m_lastRequest && iteratorResult && m_cachedDriverResultTarget.get() == driver && vm.isSafeToRecurseSoft()) [[likely]] {
                JSValue asyncContext = m_value.get();
                iteratorResult->putDirectOffset(vm, iteratorResultObjectValuePropertyOffset, m_yieldedInPlace.get());
                iteratorResult->putDirectOffset(vm, iteratorResultObjectDonePropertyOffset, jsBoolean(false));
                m_yieldedInPlace.clear();
                m_value.clear();
                m_target = { nullptr, static_cast<uint16_t>(flags & ~(isEnteredByJobLoopFlag | isBusyFlag | isYieldingInPlaceFlag)) };
                AsyncContextSwapScope asyncContextScope(vm, globalObject, asyncContext);
                if (driver->classInfo() == info())
                    uncheckedDowncast<AsyncGenerator>(driver)->resumeFromAwait(driver->realm(), iteratorResult, false, true);
                else
                    resumeDriver(vm, driver, iteratorResult, false, true);
                return;
            }
        }
        finishRun(globalObject, result, atOnce);
        drain(globalObject);
        if (driverToResume.driver) {
            AsyncContextSwapScope asyncContextScope(vm, globalObject, driverToResume.asyncContext);
            resumeDriver(vm, driverToResume.driver, driverToResume.resolution, driverToResume.isRejected, true);
        }
        return;
    }
    bool wasAwaitingYieldOperand = is(isAwaitingYieldOperandFlag);
    if (is(isAwaitingReturnFlag)) {
        set(isBusyFlag | isAwaitingReturnFlag, false);
        settleFrontRequest(globalObject, resolution, true, isRejected, atOnce);
    } else {
        set(isAwaitingYieldOperandFlag, false);
        if (wasAwaitingYieldOperand && !isRejected) {
            set(isBusyFlag, false);
            settleFrontRequest(globalObject, resolution, false, false, atOnce);
        } else
            run(globalObject, isRejected, resolution, atOnce);
    }
    drain(globalObject);
    if (driverToResume.driver) {
        AsyncContextSwapScope asyncContextScope(vm, globalObject, driverToResume.asyncContext);
        resumeDriver(vm, driverToResume.driver, driverToResume.resolution, driverToResume.isRejected, true);
    }
}

bool driveAsyncIterator(JSGlobalObject* globalObject, VM& vm, JSCell* awaited, JSObject* driver)
{
    if (awaited->type() == JSAsyncFromSyncIteratorType) {
        driveAsyncFromSyncIteratorWithDriver(globalObject, uncheckedDowncast<JSAsyncFromSyncIterator>(awaited), driver, JSValue());
        return true;
    }
    auto* generator = awaited->type() == ObjectType && awaited->classInfo() == AsyncGenerator::info() ? uncheckedDowncast<AsyncGenerator>(awaited) : nullptr;
    if (!generator || !generator->isAwaitingDriver())
        return false;
    generator->setDriver(vm, driver, AsyncContextSwapScope::current(vm, globalObject));
    return true;
}

static JSObject* generatorMethod(JSGlobalObject* globalObject, JSObject* prototype, const Identifier& name, unsigned& attributes)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue found = prototype->get(globalObject, name);
    scope.assertNoException();
    RELEASE_ASSERT(found.isCallable() && prototype->getDirect(vm, name, attributes) == found);
    return asObject(found);
}

template<Generator::ResumeMode mode>
static void installGeneratorMethod(JSGlobalObject* globalObject, JSObject* prototype, const Identifier& name)
{
    VM& vm = globalObject->vm();
    unsigned attributes;
    JSObject* original = generatorMethod(globalObject, prototype, name, attributes);
    if (original->inherits<JSNativeStdFunction>() || original->inherits<Closure>())
        return;
    auto* function = JSNativeStdFunction::create(vm, globalObject, 1, name.string(), [original](JSGlobalObject* globalObject, CallFrame* callFrame) {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        auto* generator = dynamicDowncast<Generator>(callFrame->thisValue());
        if (!generator) [[unlikely]]
            RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, original, JSC::getCallData(original), callFrame->thisValue(), ArgList(callFrame))));
        RELEASE_AND_RETURN(scope, JSValue::encode(generator->resume(globalObject, mode, callFrame->argument(0))));
    }, JSValue(original));
    prototype->putDirect(vm, name, function, attributes);
}

template<Generator::ResumeMode mode, auto original>
static ALWAYS_INLINE EncodedJSValue callAsyncGeneratorMethod(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* generator = dynamicDowncast<AsyncGenerator>(callFrame->thisValue());
    if (!generator) [[unlikely]]
        RELEASE_AND_RETURN(scope, original(globalObject, callFrame));
    RELEASE_AND_RETURN(scope, JSValue::encode(generator->enqueue(globalObject, mode, callFrame->argument(0))));
}

static JSC_DECLARE_HOST_FUNCTION(asyncGeneratorNext);
static JSC_DECLARE_HOST_FUNCTION(asyncGeneratorReturn);
static JSC_DECLARE_HOST_FUNCTION(asyncGeneratorThrow);

JSC_DEFINE_HOST_FUNCTION(asyncGeneratorNext, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return callAsyncGeneratorMethod<Generator::ResumeMode::Next, asyncGeneratorPrototypeNext>(globalObject, callFrame);
}

JSC_DEFINE_HOST_FUNCTION(asyncGeneratorReturn, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return callAsyncGeneratorMethod<Generator::ResumeMode::Return, asyncGeneratorPrototypeReturn>(globalObject, callFrame);
}

JSC_DEFINE_HOST_FUNCTION(asyncGeneratorThrow, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return callAsyncGeneratorMethod<Generator::ResumeMode::Throw, asyncGeneratorPrototypeThrow>(globalObject, callFrame);
}

static void installAsyncGeneratorMethod(JSGlobalObject* globalObject, JSObject* prototype, const Identifier& name, NativeFunction function)
{
    VM& vm = globalObject->vm();
    unsigned attributes;
    auto* original = uncheckedDowncast<JSFunction>(generatorMethod(globalObject, prototype, name, attributes));
    if (original->nativeFunction() != toTagged(function))
        prototype->putDirect(vm, name, JSFunction::create(vm, globalObject, 1, name.string(), function, ImplementationVisibility::Public), attributes);
}

void installGeneratorMethods(JSGlobalObject* globalObject)
{
    auto& names = *globalObject->vm().propertyNames;
    JSObject* prototype = globalObject->generatorPrototype();
    installGeneratorMethod<Generator::ResumeMode::Next>(globalObject, prototype, names.next);
    installGeneratorMethod<Generator::ResumeMode::Return>(globalObject, prototype, names.returnKeyword);
    installGeneratorMethod<Generator::ResumeMode::Throw>(globalObject, prototype, names.throwKeyword);
    prototype = globalObject->asyncGeneratorPrototype();
    installAsyncGeneratorMethod(globalObject, prototype, names.next, asyncGeneratorNext);
    installAsyncGeneratorMethod(globalObject, prototype, names.returnKeyword, asyncGeneratorReturn);
    installAsyncGeneratorMethod(globalObject, prototype, names.throwKeyword, asyncGeneratorThrow);
}

uint32_t State::resumeFunctionOf(uint32_t generatorFunction) const
{
    return resumeFunctionAndFlagOf(generatorFunction) & ~Data::generatorTakesCalleeClosure;
}

bool State::takesCalleeClosure(uint32_t generatorFunction) const
{
    return resumeFunctionAndFlagOf(generatorFunction) & Data::generatorTakesCalleeClosure;
}

uint32_t State::resumeFunctionAndFlagOf(uint32_t generatorFunction) const
{
    std::span functions { data.at<std::array<uint32_t, 2>>(data.generatorFunctionsOffset), data.numberOfGeneratorFunctions };
    auto found = std::ranges::lower_bound(functions, generatorFunction, { }, [](auto& generatorAndResumeFunction) {
        return generatorAndResumeFunction[0];
    });
    RELEASE_ASSERT(found != functions.end() && (*found)[0] == generatorFunction);
    return (*found)[1];
}

void State::setGeneratorPrototype(VM& vm, uint32_t resumeFunction, bool isAsync, JSObject* prototype)
{
    JSGlobalObject* globalObject = instance.globalObject;
    Structure* structure = isAsync ? AsyncGenerator::createStructure(vm, globalObject, prototype) : Generator::createStructure(vm, globalObject, prototype);
    m_generatorStructures.set(resumeFunction, GeneratorStructure { structure, 0, !isAsync && Generator::hasUnmodifiedIteration(globalObject, structure) });
    didCreate(structure);
}

State::GeneratorStructure& State::generatorStructure(VM& vm, uint32_t resumeFunction, bool isAsync)
{
    if (auto known = m_generatorStructures.find(resumeFunction); known != m_generatorStructures.end()) [[likely]]
        return known->value;
    JSGlobalObject* globalObject = instance.globalObject;
    JSObject* prototype = constructEmptyObject(globalObject, isAsync ? static_cast<JSObject*>(globalObject->asyncGeneratorPrototype()) : globalObject->generatorPrototype());
    prototype->lockAndMakePropertiesImmutable(vm);
    setGeneratorPrototype(vm, resumeFunction, isAsync, prototype);
    return m_generatorStructures.find(resumeFunction)->value;
}

State::GeneratorStructure& State::generatorStructure(VM& vm, Closure* generatorFunction, bool isAsync)
{
    if (HeapVersion version = vm.heap.objectSpace().newlyAllocatedVersion(); version != m_generatorStructuresVersion) [[unlikely]] {
        m_closureGeneratorStructures.fill({ });
        m_generatorStructuresVersion = version;
    }
    auto& known = m_closureGeneratorStructures[(std::bit_cast<uintptr_t>(generatorFunction) >> 4) % m_closureGeneratorStructures.size()];
    if (known.closure == generatorFunction && known.epoch == instance.negativeOffsetFields().typedPrototypeEpoch) [[likely]]
        return known.structure;
    JSGlobalObject* globalObject = instance.globalObject;
    Structure*& ofRealm = m_realmGeneratorStructures[isAsync];
    if (!ofRealm) {
        ofRealm = isAsync ? AsyncGenerator::createStructure(vm, globalObject, globalObject->asyncGeneratorPrototype()) : Generator::createStructure(vm, globalObject, globalObject->generatorPrototype());
        didCreate(ofRealm);
    }
    JSValue prototype = generatorFunction->ensurePrototypeProperty(vm) ? generatorFunction->getDirect(vm, vm.propertyNames->prototype) : JSValue();
    Structure* structure = prototype && prototype.isObject() ? globalObject->structureCache().emptyStructureForPrototypeFromBaseStructure(globalObject, asObject(prototype), ofRealm) : ofRealm;
    known = { generatorFunction, { structure, 0, !isAsync && Generator::hasUnmodifiedIteration(globalObject, structure) }, instance.negativeOffsetFields().typedPrototypeEpoch };
    return known.structure;
}

static JSC_DECLARE_HOST_FUNCTION(getLiveBinding);

JSC_DEFINE_HOST_FUNCTION(getLiveBinding, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto name = callFrame->argument(0).toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(callFrame->thisValue().get(globalObject, name)));
}

JSFunction* State::liveBindingGetter(VM& vm)
{
    if (!m_liveBindingGetter)
        m_liveBindingGetter = JSFunction::create(vm, instance.globalObject, 1, String(), getLiveBinding, ImplementationVisibility::Private);
    return m_liveBindingGetter;
}

static JSC_DECLARE_CUSTOM_GETTER(getHolderLiveBinding);

JSC_DEFINE_CUSTOM_GETTER(getHolderLiveBinding, (JSGlobalObject* globalObject, EncodedJSValue holder, PropertyName name))
{
    return JSValue::encode(asObject(JSValue::decode(holder))->getDirect(0).get(globalObject, name));
}

CustomGetterSetter* State::liveBindingAccessor(VM& vm)
{
    if (!m_liveBindingAccessor)
        m_liveBindingAccessor = CustomGetterSetter::create(vm, getHolderLiveBinding, nullptr);
    return m_liveBindingAccessor;
}

bool tryScheduleImmediateResume(Instance& instance, const uint64_t* frame, JSPromise* promise, JSValue resolution)
{
    if (instance.waiterToResume || instance.awaitMustSuspend || !SavedFrame::forFrame(instance, frame))
        return false;
    if (!(std::bit_cast<CallFrame*>(frame[0])->uncheckedArgument(0).asInt32() & CoroutineFrame::isEnteredByJobLoop))
        return false;
    if (resolution.isObject() && !isDefinitelyNonThenable(asObject(resolution), instance.globalObject))
        return false;
    if (promise->inlineReactionKind() != JSPromise::InlineReactionKind::InternalMicrotask)
        return false;
    JSValue waiter = promise->inlineReactionContext();
    if (!waiter || !waiter.isCell() || waiter.asCell()->structureID() != std::bit_cast<StructureID>(static_cast<uint32_t>(instance.savedFrameHeader)))
        return false;
    auto job = promise->tryFulfillTakingInlineJob(*instance.vm, resolution);
    if (!job)
        return false;
    RELEASE_ASSERT(job->task == InternalMicrotask::AOTAsyncFunctionResume);
    instance.waiterToResume = waiter.asCell();
    instance.waiterResolution = JSValue::encode(resolution);
    instance.waiterAsyncContext = JSValue::encode(job->asyncContext);
    return true;
}

static NEVER_INLINE void resumeRemainingWaiters(JSGlobalObject* globalObject, VM& vm, Instance& instance)
{
    if (AsyncGenerator* left = std::exchange(instance.typed->asyncGeneratorToFinishAfterJobLoop, nullptr)) {
        if (instance.waiterToResume == left)
            instance.waiterToResume = nullptr;
        if (!vm.hasPendingTerminationException()) [[likely]]
            left->finishAfterJobLoop(globalObject, JSValue::decode(instance.typed->asyncGeneratorResultAfterJobLoop));
        if (!instance.waiterToResume)
            return;
    }
    if (instance.isResumingWaiters)
        return;
    instance.isResumingWaiters = true;
    while (JSCell* waiter = std::exchange(instance.waiterToResume, nullptr)) {
        if (vm.hasPendingTerminationException()) [[unlikely]]
            break;
        AsyncContextSwapScope asyncContextScope(vm, globalObject, JSValue::decode(instance.waiterAsyncContext));
        resumeAsyncFunction(globalObject, vm, waiter, JSValue::decode(instance.waiterResolution), false, true);
    }
    instance.isResumingWaiters = false;
}

static void enterJobLoop(JSGlobalObject* globalObject, VM& vm, Instance& instance, AsyncGenerator* first, JSValue value)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSCell* enteredOutside = std::exchange(instance.enteredSavedFrame, nullptr);
    vmEntryToJavaScriptWith2Arguments(asyncGeneratorEntrypoint().taggedPtr(), &vm, nullptr, first, jsUndefined(), nullptr, jsNumber(CoroutineFrame::isEnteredByJobLoop | SavedFrame::mayRunNextMicrotask), value);
    JSCell* saved = std::exchange(instance.enteredSavedFrame, enteredOutside);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        JSValue error = exception->value();
        if (scope.clearExceptionExceptTermination() && saved)
            uncheckedDowncast<SavedFrame>(saved)->reject(globalObject, vm, error);
    }
    if (instance.waiterToResume)
        resumeRemainingWaiters(globalObject, vm, instance);
}

void resumeAsyncFunction(JSGlobalObject* globalObject, VM& vm, JSValue context, JSValue resolution, bool isRejected, bool isEnteredByJobLoop)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (context.asCell()->type() != JSFunctionType) {
        auto* saved = uncheckedDowncast<SavedFrame>(context);
        if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
            saved->willBeRestoredToStack();
            saved->reject(globalObject, vm, createStackOverflowError(globalObject));
            return;
        }
        saved->willBeRestoredToStack();
        Instance& instance = *saved->structure()->aotInstance();
        JSCell* enteredOutside = std::exchange(instance.enteredSavedFrame, saved);
        vmEntryToJavaScriptWith2Arguments(savedFrameEntrypoint().taggedPtr(), &vm, nullptr, saved, jsUndefined(), saved, jsNumber(isRejected | (isEnteredByJobLoop ? CoroutineFrame::isEnteredByJobLoop | SavedFrame::mayRunNextMicrotask : 0)), resolution);
        saved = uncheckedDowncast<SavedFrame>(std::exchange(instance.enteredSavedFrame, enteredOutside));
        if (Exception* exception = scope.exception()) [[unlikely]] {
            JSValue error = exception->value();
            if (scope.clearExceptionExceptTermination())
                saved->reject(globalObject, vm, error);
        }
        if (instance.waiterToResume) [[unlikely]]
            resumeRemainingWaiters(globalObject, vm, instance);
        return;
    }
    auto* resumeClosure = uncheckedDowncast<Closure>(context);
    resumeCoroutine(globalObject, resumeClosure, isRejected | (isEnteredByJobLoop ? CoroutineFrame::isEnteredByJobLoop : 0), resolution, resumeClosure);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        JSValue error = exception->value();
        if (!scope.clearExceptionExceptTermination())
            return;
        auto* frame = uncheckedDowncast<Object>(resumeClosure->environment());
        auto& promise = frame->layout().describedVariables()[CoroutineFrame::promiseField];
        uncheckedDowncast<JSPromise>(readAs(promise.store(), reinterpret_cast<const uint8_t*>(frame) + promise.offset, nullptr))->reject(vm, error);
    }
}

struct SuspendedAsyncFunction {
    JSObject* resumeClosure { nullptr };
    JSPromise* promise { nullptr };
    int32_t state { 0 };
    bool isHiddenInStackTraces { false };
    bool isSavedFrame { false };
    JSCell* waiter { nullptr };
    std::tuple<uint32_t, uint32_t, BytecodeIndex> savedFrameLocation { };

    static std::tuple<uint32_t, uint32_t, BytecodeIndex> resumeAddressLocation(Instance* instance, const void* address)
    {
        using Place = Instance::StackTraceInfo;
        const Instance::CachedAddressInfo* asked = instance->resolveReturnAddress(address);
        RELEASE_ASSERT(asked);
        Place place = instance->stackTraceInfos()[asked - instance->cachedReturnAddresses].forSite();
        if (place.kind == Place::IsVisible || place.kind == Place::IsHidden) [[likely]]
            return { asked->function, place.typedFunction, CallSiteIndex(place.callSiteBits).bytecodeIndex() };
        Instance::CachedAddressInfo& inlinedFunction = instance->cachedAddressInfo(address);
        if (place.kind == Place::IsInInlinedBody && !instance->callSiteOverrides && inlinedFunction.address == address && inlinedFunction.site != Instance::CachedAddressInfo::siteNotResolved)
            return { asked->function, inlinedFunction.site, CallSiteIndex(PackedSite::bits(asked->site)).bytecodeIndex() };
        uint32_t function = asked->function;
        auto location = FunctionRef { instance, function }.locationForReturnAddress(address, nullptr);
        if (place.kind == Place::IsInInlinedBody && !instance->callSiteOverrides)
            inlinedFunction = { address, function, location.typedFunction };
        return { function, location.typedFunction, location.bytecodeIndex };
    }

    std::tuple<uint32_t, uint32_t, BytecodeIndex> stackTraceLocation(Instance* instance, uintptr_t code) const
    {
        if (isSavedFrame)
            return savedFrameLocation;
        auto* closure = uncheckedDowncast<Closure>(resumeClosure);
        return { FunctionRef::at(instance, std::bit_cast<const void*>(ClosureWord::addressOf(closure->entry(), code))).index, closure->functionIndex(), BytecodeIndex(state) };
    }
};

static const Data& dataInImage();

static ALWAYS_INLINE JSCell* cellStoredIn(const VariableData& variable, const JSCell* environment)
{
    auto* address = reinterpret_cast<const uint8_t*>(environment) + variable.offset;
    switch (variable.store()) {
    case Store::Cell:
    case Store::JSValue: {
        JSValue value = JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(address));
        return value && value.isCell() ? value.asCell() : nullptr;
    }
    case Store::Cell32:
    case Store::OptionalCell32: {
        uint32_t word = *reinterpret_cast<const uint32_t*>(address);
        return word < minCell32 ? nullptr : MarkedBlockCage::decode(word);
    }
    default:
        return nullptr;
    }
}

static ALWAYS_INLINE SavedFrame* savedFrameIn(JSCell* cell)
{
    return cell->type() == ObjectType && cell->classInfo() == SavedFrame::info() ? uncheckedDowncast<SavedFrame>(cell) : nullptr;
}

static ALWAYS_INLINE SuspendedAsyncFunction suspendedAsyncFunctionIn(JSCell* cell)
{
    if (auto* saved = savedFrameIn(cell)) {
        SuspendedAsyncFunction result { saved, saved->promise(), 0, false, true, saved->waiter(), SuspendedAsyncFunction::resumeAddressLocation(saved->structure()->aotInstance(), saved->resumeAddress()) };
        const Data& image = dataInImage();
        uint8_t kindByte = image.at<uint8_t>(image.functionKindsOffset)[std::get<1>(result.savedFrameLocation)];
        result.isHiddenInStackTraces = kindByte & functionIsHiddenInStackTraces || (kindByte & functionKindMask) == static_cast<uint8_t>(Format::FunctionKind::Module);
        return result;
    }
    if (cell->type() != JSFunctionType || cell->classInfo() != Closure::info())
        return { };
    auto* closure = uncheckedDowncast<Closure>(cell);
    uint64_t entry = closure->entry();
    if (!(entry & 1ULL << ClosureWord::isNotCallableBit))
        return { };
    const Data& image = dataInImage();
    uint8_t kindByte = image.at<uint8_t>(image.functionKindsOffset)[ClosureWord::functionOf(entry)];
    if ((kindByte & functionKindMask) == constructorFunctionKind)
        return { };
    if (!image.at<std::array<uint8_t, 2>>(image.closureCountsOffset)[ClosureWord::functionOf(entry)][0])
        return { };
    auto* environment = std::bit_cast<JSCell*>(*reinterpret_cast<const uint64_t*>(closure + 1));
    if (!environment)
        return { };
    Structure* structure = environment->structure();
    const Data* data = structure->compiledEnvironmentData();
    if (!data)
        return { };
    auto variables = environmentLayout(*data, structure->firstCompiledEnvironmentShape(), environment).describedVariables();
    if (variables.size() <= CoroutineFrame::resumeClosureField || variables[CoroutineFrame::stateField].store() != Store::Int32)
        return { };
    if (cellStoredIn(variables[CoroutineFrame::resumeClosureField], environment) != closure)
        return { };
    JSCell* driver = cellStoredIn(variables[CoroutineFrame::promiseField], environment);
    auto* promise = driver ? dynamicDowncast<JSPromise>(driver) : nullptr;
    if (!promise)
        return { };
    int32_t state = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(environment) + variables[CoroutineFrame::stateField].offset);
    return { closure, promise, std::max(state, 0), kindByte & functionIsHiddenInStackTraces || (kindByte & functionKindMask) == static_cast<uint8_t>(Format::FunctionKind::Module) };
}

static void append(VM& vm, JSCell* owner, Vector<StackFrame>& results, const SuspendedAsyncFunction& function)
{
    if (!function.resumeClosure || function.isHiddenInStackTraces)
        return;
    Instance* instance = function.resumeClosure->structure()->aotInstance();
    auto [code, reported, callSite] = function.stackTraceLocation(instance, std::bit_cast<uintptr_t>(Image::withCode()->code()));
    results.constructAndAppend(vm, owner, tokenOf(instance), code, reported, callSite, true);
}

bool isAsyncFunctionResumeClosure(JSCell* cell)
{
    return savedFrameIn(cell) || suspendedAsyncFunctionIn(cell).resumeClosure;
}

JSPromise* asyncFunctionPromise(JSCell* resumeClosure)
{
    if (auto* saved = savedFrameIn(resumeClosure))
        return saved->promise();
    return suspendedAsyncFunctionIn(resumeClosure).promise;
}

JSCell* promiseAllContextAwaiting(JSCell* resumeClosure)
{
    auto* saved = savedFrameIn(resumeClosure);
    return saved ? saved->promiseAllContext() : nullptr;
}

bool settleElementOfPromiseAll(JSGlobalObject* globalObject, VM& vm, JSValue& element, JSPromiseCombinatorsGlobalContext* context, uint64_t index)
{
    if (element.isCell()) {
        if (element.asCell()->type() == JSPromiseType)
            return false;
        if (auto* saved = savedFrameIn(element.asCell())) {
            if (JSPromise* promise = saved->promise()) {
                element = promise;
                return false;
            }
            saved->setPromiseAllContext(vm, context, static_cast<uint32_t>(index));
            return true;
        }
    }
    globalObject->queueMicrotask(vm, InternalMicrotask::PromiseAllResolveJob, static_cast<uint8_t>(JSPromise::Status::Fulfilled), context, element, jsNumber(index));
    return true;
}

Vector<JSPromise*, 4> promisesSettledBy(JSCell* coroutine)
{
    constexpr unsigned maxGenerators = 64;
    AssertNoGC assertNoGC;
    Vector<JSPromise*, 4> promises;
    if (auto* tuple = coroutine ? dynamicDowncast<InternalFieldTuple>(coroutine) : nullptr) {
        JSValue context = tuple->getInternalField(0);
        coroutine = context && context.isCell() ? context.asCell() : nullptr;
    }
    if (!coroutine)
        return promises;
    Vector<JSCell*, 4> work { coroutine };
    unsigned numberOfGenerators = 0;
    for (size_t i = 0; i < work.size(); ++i) {
        JSCell* current = work[i];
        if (auto* promise = dynamicDowncast<JSPromise>(current))
            promises.append(promise);
        else if (JSPromise* own = asyncFunctionPromise(current))
            promises.append(own);
        else if (auto* saved = savedFrameIn(current); saved && saved->waiter())
            work.append(saved->waiter());
        else if (auto* context = promiseAllContextAwaiting(current))
            work.append(uncheckedDowncast<JSPromiseCombinatorsGlobalContext>(context)->promise().asCell());
        else if (auto* generator = current->type() == ObjectType ? dynamicDowncast<AsyncGenerator>(current) : nullptr; generator && numberOfGenerators++ < maxGenerators)
            generator->appendRequestTargets(work);
    }
    return promises;
}

Vector<JSPromise*, 4> promisesSettledAfter(JSPromise* promise)
{
    AssertNoGC assertNoGC;
    Vector<JSPromise*, 4> promises;
    if (promise->status() != JSPromise::Status::Pending)
        return promises;
    promise->forEachPendingReaction([&](InternalMicrotask task, JSValue, JSValue context) {
        if ((task == InternalMicrotask::AOTAsyncFunctionResume || task == InternalMicrotask::AOTAsyncGeneratorResume) && context && context.isCell())
            promises.appendVector(promisesSettledBy(context.asCell()));
        return true;
    });
    return promises;
}

void appendSuspendedAsyncFunction(VM& vm, JSCell* owner, Vector<StackFrame>& results, JSCell* resumeClosure)
{
    append(vm, owner, results, suspendedAsyncFunctionIn(resumeClosure));
}

JSCell* appendDirectAsyncCallers(VM& vm, JSCell* owner, Vector<StackFrame>& results, JSCell* resumeClosure, size_t maxStackSize)
{
    constexpr uint16_t asked = JSPromise::stateMask | JSPromise::inlineReactionKindMask | JSPromise::inlineReactionMicrotaskMask;
    constexpr uint16_t isAwaitedByCompiledCode = static_cast<uint16_t>(JSPromise::Status::Pending)
        | static_cast<uint16_t>(JSPromise::InlineReactionKind::InternalMicrotask) << JSPromise::inlineReactionKindShift
        | static_cast<uint16_t>(InternalMicrotask::AOTAsyncFunctionResume) << JSPromise::inlineReactionMicrotaskShift;
    SuspendedAsyncFunction current = suspendedAsyncFunctionIn(resumeClosure);
    if (!current.resumeClosure)
        return resumeClosure;
    Instance* instance = nullptr;
    JSCell* token = nullptr;
    uintptr_t code = 0;
    auto leave = [&](JSCell* result) {
        if (token)
            vm.writeBarrier(owner, token);
        return result;
    };
    for (;;) {
        JSValue context = current.waiter;
        if (!current.waiter) {
            if (!current.promise)
                return leave(promiseAllContextAwaiting(current.resumeClosure) ? current.resumeClosure : nullptr);
            uint16_t flags = current.promise->flags();
            if ((flags & asked) != isAwaitedByCompiledCode || results.size() >= maxStackSize) {
                bool hasNoAwaiters = (flags & JSPromise::stateMask) != static_cast<uint16_t>(JSPromise::Status::Pending) || (!(flags & JSPromise::inlineReactionKindMask) && !current.promise->payloadCell());
                return leave(hasNoAwaiters ? nullptr : current.resumeClosure);
            }
            context = current.promise->inlineReactionContext();
        } else if (results.size() >= maxStackSize)
            return leave(nullptr);
        if (!context || !context.isCell())
            return leave(current.resumeClosure);
        SuspendedAsyncFunction caller = suspendedAsyncFunctionIn(context.asCell());
        if (!caller.resumeClosure)
            return leave(current.resumeClosure);
        current = caller;
        if (Instance* closureInstance = current.resumeClosure->structure()->aotInstance(); closureInstance != instance) {
            if (token)
                vm.writeBarrier(owner, token);
            instance = closureInstance;
            token = tokenOf(instance);
            code = std::bit_cast<uintptr_t>(Image::withCode()->code());
        }
        if (!current.isHiddenInStackTraces) {
            auto [inImage, reported, callSite] = current.stackTraceLocation(instance, code);
            results.constructAndAppend(StackFrame::Suspended, token, inImage, reported, callSite);
        }
    }
}

void resumeDriver(VM& vm, JSObject* driver, JSValue resolution, bool isRejected, bool isEnteredByJobLoop)
{
    JSGlobalObject* globalObject = driver->realm();
    if (driver->type() == JSFunctionType || driver->classInfo() == SavedFrame::info())
        resumeAsyncFunction(globalObject, vm, driver, resolution, isRejected, isEnteredByJobLoop);
    else
        resumeAsyncGenerator(globalObject, driver, resolution, isRejected, isEnteredByJobLoop);
}

void resumeAsyncGenerator(JSGlobalObject* globalObject, JSValue context, JSValue resolution, bool isRejected, bool isEnteredByJobLoop)
{
    uncheckedDowncast<AsyncGenerator>(context)->resumeFromAwait(globalObject, resolution, isRejected, isEnteredByJobLoop);
}

bool Closure::mayBeCalled() const
{
    return !(m_entry & 1ULL << ClosureWord::isNotCallableBit);
}

static FunctionSourceProvider s_functionSourceProvider;
static JSString* functionSourceText(JSGlobalObject*, uint32_t function);

JSValue Closure::toString(JSGlobalObject* globalObject) const
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (kind() == Format::FunctionKind::Bound)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, "function "_s, structure()->aotInstance()->typed->nameOfFunction(functionIndex()).substring("bound "_s.length()), "() { [native code] }"_s));
    ASCIILiteral async = isDeclaredAs(functionIsAsync) ? "async "_s : ""_s;
    bool isProgramObject = !isDeclaredAs(functionIsInBuiltinModule);
    JSString* text = isProgramObject ? functionSourceText(globalObject, functionIndex()) : nullptr;
    RETURN_IF_EXCEPTION(scope, { });
    if (text)
        return text;
    if (isProgramObject && kind() == Format::FunctionKind::Arrow)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, "() => { [native code] }"_s));
    String name = kind() == Format::FunctionKind::Arrow ? emptyString() : structure()->aotInstance()->typed->nameOfFunction(functionIndex());
    if (kind() == Format::FunctionKind::Constructor)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, "class "_s, name, name.isEmpty() ? ""_s : " "_s, "{ [native code] }"_s));
    if (isProgramObject && kind() == Format::FunctionKind::Method)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, isDeclaredAs(functionIsGenerator) ? "*"_s : ""_s, name, "() { [native code] }"_s));
    RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, isDeclaredAs(functionIsGenerator) ? "function* "_s : "function "_s, name, "() { [native code] }"_s));
}

static const Data& dataInImage();

Format::FunctionKind Closure::kind() const
{
    const Data& data = dataInImage();
    return static_cast<Format::FunctionKind>(data.at<uint8_t>(data.functionKindsOffset)[functionIndex()] & functionKindMask);
}

std::array<uint8_t, 2> Closure::counts() const
{
    const Data& data = dataInImage();
    return data.at<std::array<uint8_t, 2>>(data.closureCountsOffset)[functionIndex()];
}

bool Closure::isDeclaredAs(uint8_t asyncOrGenerator) const
{
    const Data& data = dataInImage();
    return data.at<uint8_t>(data.functionKindsOffset)[functionIndex()] & asyncOrGenerator;
}

bool Closure::createsFixedLayoutObjects() const
{
    return kind() == Format::FunctionKind::Plain && !isDeclaredAs(functionIsAsync | functionIsGenerator) && structure()->aotInstance()->typed->createsFixedLayoutObjects(functionIndex());
}

bool Closure::hasImmutablePrototypeProperty() const
{
    State& state = *structure()->aotInstance()->typed;
    return createsFixedLayoutObjects() && (!state.mayModifyPrototypeObjectOf(functionIndex()) || state.createsObjectsWithDefinitelyInitializedFields(functionIndex()));
}

bool Closure::sharesPrototypeProperty() const
{
    Instance* instance = structure()->aotInstance();
    return isDeclaredAs(functionIsGenerator) && instance->typed->closureIfCreated(functionIndex()) != this && !instance->typed->takesCalleeClosure(functionIndex());
}

bool hasNoLazyPropertyNamed(VM& vm, JSCell* cell, UniquedStringImpl* uid, bool isReceiver)
{
    if (cell->type() == ErrorInstanceType)
        return !ErrorInstance::isLazilyMaterialized(vm, uid);
    if (uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype)
        return false;
    if (isSlimClosureType(cell->type()))
        return isReceiver;
    if (cell->type() != JSFunctionType || cell->classInfo() != Closure::info())
        return false;
    auto* closure = uncheckedDowncast<Closure>(cell);
    if (closure->kind() != Format::FunctionKind::Constructor)
        return true;
    const LayoutData* ofClass = closure->classData();
    return ofClass && closure->getDirect(vm, vm.propertyNames->prototype) && !closure->structure()->aotInstance()->typed->staticField(*ofClass, uid);
}

static JSC_DECLARE_CUSTOM_GETTER(closurePrototypeProperty);

JSC_DEFINE_CUSTOM_GETTER(closurePrototypeProperty, (JSGlobalObject* globalObject, EncodedJSValue encodedClosure, PropertyName name))
{
    JSCell* cell = JSValue::decode(encodedClosure).asCell();
    if (cell->type() == SlimClosureType) {
        auto* closure = uncheckedDowncast<SlimClosure>(cell);
        return JSValue::encode(closure->canConstruct() ? closure->get(globalObject, name) : jsUndefined());
    }
    JSValue prototype = uncheckedDowncast<Closure>(cell)->prototypeProperty(globalObject->vm());
    return JSValue::encode(prototype ? prototype : jsUndefined());
}

bool closureHandlesPrototypeProperty(JSCell* cell)
{
    return cell->type() == SlimClosureType || (cell->type() == JSFunctionType && cell->classInfo() == Closure::info());
}

GetValueFunc closurePrototypePropertyGetter()
{
    return closurePrototypeProperty;
}

const LayoutData* Closure::classData() const
{
    return structure()->aotInstance()->typed->classConstructedBy(functionIndex());
}

JSValue Closure::callAsConstructor(JSGlobalObject* globalObject, JSValue thisValue, JSValue newTarget, const ArgList& arguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer all;
    appendConstructArguments(all, this, newTarget, arguments);
    if (all.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    NewTargetScope newTargetScope(this, newTarget);
    ConstructionScope constructionScope(vm, *structure()->aotInstance()->typed);
    CallData callData;
    callData.type = CallData::Type::JS;
    callData.js.functionExecutable = nullptr;
    callData.js.scope = this->scope();
    RELEASE_AND_RETURN(scope, call(globalObject, this, callData, thisValue, all));
}

bool Closure::isLazyClassObject() const
{
    if (kind() != Format::FunctionKind::Constructor || getDirect(vm(), vm().propertyNames->prototype))
        return false;
    const LayoutData* ofClass = classData();
    if (!ofClass)
        return false;
    State& state = *structure()->aotInstance()->typed;
    return state.createsClassObjectOnMutation(*ofClass) && (state.createsPrototypeBeforeClassObject(*ofClass) || state.classPrototypeIfExists(ofClass->index));
}

static NEVER_INLINE void createClassObject(Closure* classObject)
{
    State& state = *classObject->structure()->aotInstance()->typed;
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(classObject->vm()).countOperation("ClassObject", "made");
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(classObject->vm());
    state.createClass(state.instance.globalObject, classObject, JSValue(), State::ClassPart::ClassObject);
    scope.assertNoException();
}

void Closure::adoptBoundFunctionStructure(VM& vm)
{
    State& state = *structure()->aotInstance()->typed;
    if (structure() == state.closureStructure(vm, functionIndex()))
        setStructure(vm, state.boundClosureStructure(vm, functionIndex()));
}

void Closure::ensureClassObject(VM& vm)
{
    if (kind() == Format::FunctionKind::Bound) [[unlikely]] {
        adoptBoundFunctionStructure(vm);
        return;
    }
    if (!isLazyClassObject())
        return;
    createClassObject(this);
}

JSValue Closure::prototypeProperty(VM& vm)
{
    if (JSValue stored = getDirect(vm, vm.propertyNames->prototype))
        return stored;
    if (isLazyClassObject())
        return structure()->aotInstance()->typed->classPrototype(vm, classData()->index);
    ensurePrototypeProperty(vm);
    return getDirect(vm, vm.propertyNames->prototype);
}

static bool hasPrototypePropertyAtCreation(const Closure& closure)
{
    if (closure.kind() == Format::FunctionKind::Constructor)
        return true;
    if (closure.isDeclaredAs(functionIsInBuiltinModule) && !closure.structure()->aotInstance()->typed->hasGuardedPrototype(closure.functionIndex()))
        return false;
    return closure.isDeclaredAs(functionIsGenerator) || (closure.kind() == Format::FunctionKind::Plain && !closure.isDeclaredAs(functionIsAsync));
}

bool Closure::ensurePrototypeProperty(VM& vm)
{
    if (getDirect(vm, vm.propertyNames->prototype))
        return true;
    ensureClassObject(vm);
    if (getDirect(vm, vm.propertyNames->prototype))
        return true;
    if (kind() == Format::FunctionKind::Constructor || !hasPrototypePropertyAtCreation(*this))
        return false;
    State& state = *structure()->aotInstance()->typed;
    JSObject* prototype;
    if (isDeclaredAs(functionIsGenerator)) {
        JSGlobalObject* globalObject = state.instance.globalObject;
        if (state.takesCalleeClosure(functionIndex())) {
            prototype = constructEmptyObject(globalObject, isDeclaredAs(functionIsAsync) ? static_cast<JSObject*>(globalObject->asyncGeneratorPrototype()) : globalObject->generatorPrototype());
            prototype->lockAndMakePropertiesImmutable(vm);
        } else
            prototype = state.generatorStructure(vm, state.resumeFunctionOf(functionIndex()), isDeclaredAs(functionIsAsync)).structure->storedPrototypeObject();
    } else {
        if (state.hasGuardedPrototype(functionIndex()))
            prototype = PrototypeObject::create(vm, state.prototypeObjectStructure(vm), functionIndex());
        else
            prototype = constructEmptyObject(state.instance.globalObject);
        prototype->putDirect(vm, vm.propertyNames->constructor, this, static_cast<unsigned>(PropertyAttribute::DontEnum));
    }
    putDirect(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | (isDeclaredAs(functionIsGenerator) && !state.data.isCompiledForAudit ? PropertyAttribute::ReadOnly : PropertyAttribute::None));
    return true;
}

bool Closure::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    if (JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot))
        return true;
    State& state = *object->structure()->aotInstance()->typed;
    if (const LayoutData* ofClass = closure->classData()) {
        if (closure->isLazyClassObject()) {
            if (propertyName != vm.propertyNames->prototype && propertyName != vm.propertyNames->name && propertyName != vm.propertyNames->length && !state.hasStaticMethodNamed(*ofClass, propertyName.uid()))
                return false;
            closure->ensureClassObject(vm);
            if (JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot))
                return true;
        }
    }
    if (propertyName == vm.propertyNames->prototype)
        return closure->ensurePrototypeProperty(vm) && JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot);
    if ((propertyName == vm.propertyNames->hasInstanceSymbol || propertyName == vm.propertyNames->length || propertyName == vm.propertyNames->name) && closure->kind() == Format::FunctionKind::Bound) [[unlikely]]
        closure->adoptBoundFunctionStructure(vm);
    return Base::getOwnPropertySlot(object, globalObject, propertyName, slot);
}

} } } // namespace JSC::AOT::Typed
extern "C" void* g_aotSlimClosureEntrypoint;
namespace JSC { namespace AOT { namespace Typed {

const ClassInfo SlimClosure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(SlimClosure) };

Structure* SlimClosure::createStructure(VM& vm, Instance& instance, JSValue prototype, bool hasProperties, bool constructs, bool isBound)
{
    if (hasProperties) {
        Structure* structure = Structure::create(vm, instance.globalObject, prototype, TypeInfo(SlimClosureWithPropertiesType, constructs ? ReifiedConstructorStructureFlags : isBound ? ReifiedBoundStructureFlags : ReifiedStructureFlags), info(), NonArray, 0);
        structure->setAOTInstance(&instance);
        return structure;
    }
    Structure* structure = Structure::create(vm, instance.globalObject, prototype, TypeInfo(SlimClosureType, StructureFlags), info(), NonArray, 0);
    structure->setAOTInstance(&instance);
    structure->setHasImmutableShape(true);
    return structure;
}

SlimClosure* SlimClosure::create(VM& vm, Structure* structure, uint32_t function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    FunctionTables& tables = FunctionTables::of(vm);
    RELEASE_ASSERT(ProgramData::get()->closureWordOf(function) && structure->aotInstance()->typed->usesSlimClosures(function));
    RELEASE_ASSERT((tables.data.at<std::array<uint8_t, 2>>(tables.data.closureCountsOffset)[function] == std::array { static_cast<uint8_t>(numberOfReferences), static_cast<uint8_t>(captures.size()) }));
    SlimClosure* closure = new (NotNull, allocateCell<SlimClosure>(vm, allocationSize(captures.size()))) SlimClosure(vm, structure, tables.words[function]);
    memcpySpan(std::span { reinterpret_cast<uint64_t*>(closure + 1), captures.size() }, captures);
    closure->finishCreation(vm);
    structure->aotInstance()->typed->countClosure(function, captures.size());
    return closure;
}

SlimClosure* SlimClosure::createForCompiledCode(VM& vm, Structure* structure, size_t allocationSize)
{
    RELEASE_ASSERT(allocationSize >= SlimClosure::allocationSize(0));
    SlimClosure* closure = new (NotNull, allocateCell<SlimClosure>(vm, allocationSize)) SlimClosure(vm, structure, 0);
    closure->finishCreation(vm);
    return closure;
}

State& SlimClosure::state() const
{
    return *structure()->existingAOTFields().aotInstance->typed;
}

uint8_t SlimClosure::kindAndDeclaration() const
{
    const Data& data = dataInImage();
    return data.at<uint8_t>(data.functionKindsOffset)[functionIndex()];
}

std::span<const uint64_t> SlimClosure::captures() const
{
    const Data& data = dataInImage();
    return { reinterpret_cast<const uint64_t*>(this + 1), data.at<std::array<uint8_t, 2>>(data.closureCountsOffset)[functionIndex()][1] };
}

unsigned SlimClosure::numberOfReferences() const
{
    const Data& data = dataInImage();
    return data.at<std::array<uint8_t, 2>>(data.closureCountsOffset)[functionIndex()][0];
}

String SlimClosure::nameOfFunction() const
{
    return state().nameOfFunction(functionIndex());
}

template<typename Visitor>
void SlimClosure::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(cell);
    uint32_t function;
    if (visitor.mutatorsAreStopped()) {
        if (closure->isReified())
            Base::visitChildren(cell, visitor);
        else {
            JSCell::visitChildren(cell, visitor);
            if (!closure->word())
                return;
        }
        function = closure->functionIndex();
        const Data& data = dataInImage();
        visitor.appendValues(reinterpret_cast<const WriteBarrier<Unknown>*>(closure + 1), data.at<std::array<uint8_t, 2>>(data.closureCountsOffset)[function][0]);
        return;
    }
    StructureID structureID = closure->structureID();
    if (structureID.isNuked()) {
        visitor.didRace(closure, "SlimClosure::reifyProperties");
        return;
    }
    WTF::loadLoadFence();
    if (structureID.decode()->typeInfo().type() == SlimClosureWithPropertiesType) {
        Base::visitChildren(cell, visitor);
        function = closure->functionIndexInButterfly();
    } else {
        JSCell::visitChildren(cell, visitor);
        uint64_t word = closure->word();
        WTF::loadLoadFence();
        if (closure->structureID() != structureID) {
            visitor.didRace(closure, "SlimClosure::reifyProperties");
            return;
        }
        if (!word)
            return;
        function = ClosureWord::functionOf(word);
    }
    const Data& data = dataInImage();
    visitor.appendValues(reinterpret_cast<const WriteBarrier<Unknown>*>(closure + 1), data.at<std::array<uint8_t, 2>>(data.closureCountsOffset)[function][0]);
}

DEFINE_VISIT_CHILDREN(SlimClosure);

void SlimClosure::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(cell);
    if (closure->isReified())
        Base::analyzeHeap(cell, analyzer);
    else {
        JSCell::analyzeHeap(cell, analyzer);
        if (!closure->word())
            return;
    }
    for (unsigned i = 0; i < closure->numberOfReferences(); ++i) {
        if (JSValue reference = JSValue::decode(closure->captures()[i]); reference && reference.isCell())
            analyzer.analyzeIndexEdge(cell, reference.asCell(), i);
    }
}

size_t SlimClosure::estimatedSize(JSCell* cell, VM& vm)
{
    return uncheckedDowncast<SlimClosure>(cell)->isReified() ? Base::estimatedSize(cell, vm) : cell->cellSize();
}

static JSC_DECLARE_HOST_FUNCTION(constructSlimClosure);

CallData SlimClosure::getCallData(JSCell* cell)
{
    CallData callData;
    callData.type = CallData::Type::JS;
    callData.js.functionExecutable = nullptr;
    callData.js.scope = cell->structure()->realm();
    return callData;
}

CallData SlimClosure::getConstructData(JSCell* cell)
{
    CallData constructData;
    if (!uncheckedDowncast<SlimClosure>(cell)->canConstruct())
        return constructData;
    constructData.type = CallData::Type::Native;
    constructData.native.function = constructSlimClosure;
    constructData.native.isBoundFunction = false;
    constructData.native.isWasm = false;
    return constructData;
}

JSObject* ordinaryCreateFromConstructor(JSGlobalObject* globalObject, JSObject* newTarget)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue prototype = newTarget->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (prototype.isObject())
        return constructEmptyObject(globalObject, asObject(prototype));
    JSGlobalObject* realm = getFunctionRealm(globalObject, newTarget);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return constructEmptyObject(realm);
}

Structure* State::structureForThisWithSlimClosure(JSGlobalObject* globalObject, JSObject* constructor, JSObject* newTarget, unsigned inlineCapacity)
{
    VM& vm = globalObject->vm();
    auto* slimConstructor = dynamicDowncast<SlimClosure>(constructor);
    if (!slimConstructor || thisAllocation(slimConstructor->functionIndex()) || newTarget->structure()->aotInstance() != &instance)
        return nullptr;
    unsigned attributes = 0;
    JSValue prototype;
    uint32_t targetFunction;
    if (auto* target = dynamicDowncast<SlimClosure>(newTarget)) {
        if (!target->canConstruct())
            return nullptr;
        target->reifyProperties(vm);
        prototype = target->getDirect(vm, vm.propertyNames->prototype, attributes);
        targetFunction = target->functionIndex();
    } else if (auto* target = dynamicDowncast<Closure>(newTarget)) {
        prototype = target->getDirect(vm, vm.propertyNames->prototype, attributes);
        if (!prototype)
            prototype = target->prototypeProperty(vm);
        targetFunction = target->functionIndex();
    } else
        return nullptr;
    if (!prototype || attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return nullptr;
    JSObject* parent = prototype.isObject() ? asObject(prototype) : globalObject->objectPrototype();
    unsigned capacity = m_inlineCapacitiesForThis.ensure(targetFunction, [&] {
        return inlineCapacityForThis(vm, slimConstructor->functionIndex(), targetFunction, inlineCapacity, parent);
    }).iterator->value;
    return globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, parent, capacity);
}

JSC_DEFINE_HOST_FUNCTION(constructSlimClosure, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* closure = callFrame->jsCallee();
    JSObject* thisObject = ordinaryCreateFromConstructor(globalObject, asObject(callFrame->newTarget()));
    RETURN_IF_EXCEPTION(scope, { });
    ProtoCallFrame protoCallFrame;
    protoCallFrame.init(nullptr, globalObject, closure, thisObject, nullptr, callFrame->argumentCountIncludingThis(), reinterpret_cast<EncodedJSValue*>(callFrame->addressOfArgumentsStart()));
    ConstructionScope constructionScope(vm, *closure->structure()->aotInstance()->typed);
    JSValue result = JSValue::decode(vmEntryToJavaScript(slimClosureEntrypoint().taggedPtr(), &vm, &protoCallFrame));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result.isObject() ? result : JSValue(thisObject));
}

JSValue SlimClosure::toString(JSGlobalObject* globalObject) const
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    uint8_t kind = kindAndDeclaration();
    if (static_cast<Format::FunctionKind>(kind & functionKindMask) == Format::FunctionKind::Bound)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, "function "_s, nameOfFunction().substring("bound "_s.length()), "() { [native code] }"_s));
    ASCIILiteral async = kind & functionIsAsync ? "async "_s : ""_s;
    bool isProgramObject = !(kind & functionIsInBuiltinModule);
    JSString* text = isProgramObject ? functionSourceText(globalObject, functionIndex()) : nullptr;
    RETURN_IF_EXCEPTION(scope, { });
    if (text)
        return text;
    if (isProgramObject && static_cast<Format::FunctionKind>(kind & functionKindMask) == Format::FunctionKind::Arrow)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, "() => { [native code] }"_s));
    if (isProgramObject && static_cast<Format::FunctionKind>(kind & functionKindMask) == Format::FunctionKind::Method)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, nameOfFunction(), "() { [native code] }"_s));
    String name = static_cast<Format::FunctionKind>(kind & functionKindMask) == Format::FunctionKind::Arrow ? emptyString() : nameOfFunction();
    RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, "function "_s, name, "() { [native code] }"_s));
}

} // namespace Typed

void setFunctionSourceProvider(FunctionSourceProvider function)
{
    Typed::s_functionSourceProvider = function;
}

Structure* cachedBoundFunctionStructure(JSObject* closure)
{
    return uncheckedDowncast<Typed::SlimClosure>(closure)->state().asyncBoundFunctionStructure;
}

void setCachedBoundFunctionStructure(JSObject* closure, Structure* structure)
{
    uncheckedDowncast<Typed::SlimClosure>(closure)->state().setAsyncBoundFunctionStructure(structure);
}

namespace Typed {

void State::setAsyncBoundFunctionStructure(Structure* structure)
{
    if (asyncBoundFunctionStructure || structure->storedPrototype() != JSValue(instance.globalObject->asyncFunctionPrototype()))
        return;
    didCreate(structure);
    asyncBoundFunctionStructure = structure;
}

void SlimClosure::reifyProperties(VM& vm)
{
    if (isReified())
        return;
    State& state = this->state();
    uint32_t function = functionIndex();
    Structure* withProperties = state.reifiedSlimClosureStructure(vm, function);
    JSValue values[] = { jsNumber(function), jsNumber(state.data.functionLengthOf(function)), state.string(vm, state.data.at<uint32_t>(state.data.functionsOffset)[function]), JSValue() };
    if (canConstruct()) {
        JSObject* prototype = constructEmptyObject(state.instance.globalObject);
        prototype->putDirect(vm, vm.propertyNames->constructor, this, static_cast<unsigned>(PropertyAttribute::DontEnum));
        values[3] = prototype;
    }
    static_assert(std::size(values) == initialOutOfLineCapacity);
    RELEASE_ASSERT(withProperties->outOfLineCapacity() == std::size(values) && getPrototypeDirect() == withProperties->storedPrototype());
    Butterfly* butterfly = Butterfly::createUninitialized(vm, this, 0, std::size(values), false, 0);
    for (unsigned i = 0; i < std::size(values); ++i)
        butterfly->propertyStorage()[offsetInOutOfLineStorage(firstOutOfLineOffset + i)].setWithoutWriteBarrier(values[i]);
    nukeStructureAndSetButterfly(vm, structureID(), butterfly);
    setStructure(vm, withProperties);
    vm.writeBarrier(this);
}

static String functionComputedName(JSGlobalObject* globalObject, const Identifier& key, Format::MethodKind kind)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = key.string();
    if (UniquedStringImpl* uid = key.impl(); uid->isSymbol()) {
        name = static_cast<SymbolImpl&>(*uid).isNullSymbol() ? emptyString() : tryMakeString('[', StringView(uid), ']');
        if (!name) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope, "Function name is too long"_s);
            return { };
        }
    }
    if (kind == Format::MethodKind::Method)
        return name;
    bool isGetter = kind == Format::MethodKind::Getter;
    name = tryMakeString(isGetter ? "get "_s : "set "_s, name);
    if (!name) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope, isGetter ? "Getter name is too long"_s : "Setter name is too long"_s);
    return name;
}

void SlimClosure::setComputedName(VM& vm, const Identifier& key, Format::MethodKind kind)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = functionComputedName(globalObject(), key, kind);
    RETURN_IF_EXCEPTION(scope, void());
    reifyProperties(vm);
    putDirect(vm, vm.propertyNames->name, jsString(vm, name), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
}

static JSObject* boundFunctionTarget(VM& vm, State& state, uint32_t function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    const Data& data = state.data;
    std::span all { data.at<std::array<uint32_t, 3>>(data.boundFunctionsOffset), data.numberOfBoundFunctions };
    auto found = std::ranges::lower_bound(all, function, { }, [](auto& entry) {
        return entry[0];
    });
    RELEASE_ASSERT(found != all.end() && (*found)[0] == function);
    if (!((*found)[2] & boundFunctionHoldsTarget))
        return state.closureObjectFor(vm, (*found)[1]);
    RELEASE_ASSERT(numberOfReferences && numberOfReferences <= captures.size());
    return std::bit_cast<JSObject*>(captures[numberOfReferences - 1]);
}

bool Closure::customHasInstance(JSObject* object, JSGlobalObject* globalObject, JSValue value)
{
    Closure* closure = uncheckedDowncast<Closure>(object);
    return boundFunctionTarget(globalObject->vm(), *object->structure()->aotInstance()->typed, closure->functionIndex(), closure->captures(), closure->numberOfReferences())->hasInstance(globalObject, value);
}

bool SlimClosure::customHasInstance(JSObject* object, JSGlobalObject* globalObject, JSValue value)
{
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(object);
    return boundFunctionTarget(globalObject->vm(), closure->state(), closure->functionIndex(), closure->captures(), closure->numberOfReferences())->hasInstance(globalObject, value);
}

bool SlimClosure::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(object);
    if (!closure->isReified()) {
        bool isShown = name == vm.propertyNames->length || name == vm.propertyNames->name;
        if (!isShown && (name != vm.propertyNames->prototype || !closure->canConstruct()) && (name != vm.propertyNames->hasInstanceSymbol || !closure->isBound()))
            return false;
        if (isShown && !closure->perCellBit()) {
            closure->setPerCellBit(true);
            State& state = closure->state();
            if (name == vm.propertyNames->length)
                slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, jsNumber(state.data.functionLengthOf(closure->functionIndex())));
            else
                slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, state.string(vm, state.data.at<uint32_t>(state.data.functionsOffset)[closure->functionIndex()]));
            return true;
        }
        closure->reifyProperties(vm);
    }
    return Base::getOwnPropertySlot(object, globalObject, name, slot);
}

bool SlimClosure::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    return uncheckedDowncast<SlimClosure>(object)->isReified() && Base::getOwnPropertySlotByIndex(object, globalObject, index, slot);
}

void SlimClosure::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    uncheckedDowncast<SlimClosure>(object)->reifyProperties(globalObject->vm());
    Base::getOwnPropertyNames(object, globalObject, names, mode);
}

bool SlimClosure::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(cell);
    bool wasUnmodified = !closure->isReified();
    closure->reifyProperties(globalObject->vm());
    bool result = Base::put(cell, globalObject, name, value, slot);
    bool isPrototype = name == globalObject->vm().propertyNames->prototype;
    if (name == globalObject->vm().propertyNames->displayName) [[unlikely]]
        anyClosureHasDisplayName.store(true, std::memory_order_relaxed);
    if (wasUnmodified || isPrototype)
        slot.disableCaching();
    if (isPrototype) [[unlikely]]
        ++closure->state().instance.negativeOffsetFields().typedPrototypeEpoch;
    return result;
}

bool SlimClosure::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    uncheckedDowncast<SlimClosure>(cell)->reifyProperties(globalObject->vm());
    return Base::putByIndex(cell, globalObject, index, value, shouldThrow);
}

bool SlimClosure::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(cell);
    bool wasUnmodified = !closure->isReified();
    closure->reifyProperties(globalObject->vm());
    bool isDeleted = Base::deleteProperty(cell, globalObject, name, slot);
    if (wasUnmodified)
        slot.disableCaching();
    return isDeleted;
}

bool SlimClosure::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    return !uncheckedDowncast<SlimClosure>(cell)->isReified() || Base::deletePropertyByIndex(cell, globalObject, index);
}

bool SlimClosure::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    uncheckedDowncast<SlimClosure>(object)->reifyProperties(globalObject->vm());
    if (name == globalObject->vm().propertyNames->prototype) [[unlikely]]
        ++uncheckedDowncast<SlimClosure>(object)->state().instance.negativeOffsetFields().typedPrototypeEpoch;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(object);
    if (name == vm.propertyNames->displayName) [[unlikely]]
        anyClosureHasDisplayName.store(true, std::memory_order_relaxed);
    if (result && name == vm.propertyNames->name && descriptor.value() && descriptor.value().isString() && usesOwnPropertiesForStackTraceName(closure)) [[unlikely]] {
        String reported = asString(descriptor.value())->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        setNameReportedForFrames(closure->functionIndex(), reported, closure->state().closureIfCreated(closure->functionIndex()) == closure);
    }
    return result;
}

bool SlimClosure::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    uncheckedDowncast<SlimClosure>(object)->reifyProperties(globalObject->vm());
    return Base::preventExtensions(object, globalObject);
}

bool SlimClosure::isExtensible(JSObject* object, JSGlobalObject* globalObject)
{
    return !uncheckedDowncast<SlimClosure>(object)->isReified() || Base::isExtensible(object, globalObject);
}

bool SlimClosure::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    if (object->getPrototypeDirect() == prototype)
        return true;
    uncheckedDowncast<SlimClosure>(object)->reifyProperties(globalObject->vm());
    return Base::setPrototype(object, globalObject, prototype, shouldThrow);
}

static String describeStaticField(State& state, const LayoutData& ofClass, PropertyName name)
{
    return makeString("static '"_s, StringView(name.uid()), "' of "_s, state.nameOf(ofClass));
}

static uint32_t kindsStoredUnchanged(const RuntimeType& type)
{
    uint32_t kinds = 0;
    if (type.primitives & Primitive::Undefined)
        kinds |= SoundTypeUndefined;
    if (type.primitives & Primitive::Null)
        kinds |= SoundTypeNull;
    if ((type.primitives & (Primitive::False | Primitive::True)) == (Primitive::False | Primitive::True))
        kinds |= SoundTypeBoolean;
    if (type.primitives & Primitive::Double && type.primitives & Primitive::Int && type.minInteger() <= -maxTrackedInteger && type.maxInteger() >= maxTrackedInteger)
        kinds |= SoundTypeNumber;
    if (type.primitives & Primitive::String)
        kinds |= SoundTypeString;
    if (type.allowsAnyObject() && !type.numberOfSupertypes())
        kinds |= SoundTypeAnyObject;
    return kinds;
}

static JSValue coerceToStaticFieldType(JSGlobalObject* globalObject, State& state, const LayoutData& ofClass, const FieldData& field, PropertyName name, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String failure;
    JSValue held = state.coerceToFieldType(globalObject, field, value, failure);
    RETURN_IF_EXCEPTION(scope, { });
    if (!held)
        state.reportOrThrow(scope, globalObject, { .kind = "class-static-write"_s, .on = &ofClass, .member = name.uid(), .expected = &state.ownTypeOf(field), .got = value }, state.assignmentMessage(vm, describeStaticField(state, ofClass, name), field, value, failure), State::InAudit::Ignored);
    return held;
}

bool Closure::mayReplacePrototypeProperty(JSGlobalObject* globalObject, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    bool isGenerator = isDeclaredAs(functionIsGenerator);
    if (!isGenerator && !hasImmutablePrototypeProperty())
        return true;
    ensurePrototypeProperty(vm);
    if (getDirect(vm, vm.propertyNames->prototype) == value)
        return true;
    State& state = *structure()->aotInstance()->typed;
    if (isGenerator && !shouldThrow && !state.data.isCompiledForAudit)
        return false;
    return state.reportOrThrow(globalObject, { .kind = isGenerator ? "generator-prototype-write"_s : "function-prototype-write"_s, .onFunction = functionIndex() }, makeString("Cannot replace 'prototype' of "_s, isGenerator ? "generator function '"_s : "function '"_s, state.nameOfFunction(functionIndex()), isGenerator ? "': it is fixed when the program is compiled"_s : "': TypeScript code of the program creates its objects"_s), State::InAudit::Performed);
}

static bool reportOrThrowSharesPrototypeProperty(JSGlobalObject* globalObject, State& state, Closure* closure)
{
    return state.reportOrThrow(globalObject, { .kind = "generator-prototype-write"_s, .onFunction = closure->functionIndex() }, makeString("Cannot replace 'prototype' of generator function '"_s, state.nameOfFunction(closure->functionIndex()), "': all closures of a nested generator function share one prototype"_s), State::InAudit::Ignored);
}

bool Closure::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    State& state = *cell->structure()->aotInstance()->typed;
    const LayoutData* ofClass = closure->classData();
    const FieldData* field = ofClass && slot.thisValue() == JSValue(closure) ? state.staticField(*ofClass, name.uid()) : nullptr;
    JSValue given = value;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (field) [[unlikely]] {
        value = coerceToStaticFieldType(globalObject, state, *ofClass, *field, name, value);
        RETURN_IF_EXCEPTION(scope, false);
        if (!value)
            return false;
    }
    scope.release();
    closure->ensureClassObject(vm);
    if (name == vm.propertyNames->prototype && slot.thisValue() == JSValue(closure) && state.data.isCompiledForAudit && closure->sharesPrototypeProperty()) [[unlikely]]
        return reportOrThrowSharesPrototypeProperty(globalObject, state, closure);
    if (name == vm.propertyNames->prototype && slot.thisValue() == JSValue(closure) && !closure->mayReplacePrototypeProperty(globalObject, value, slot.isStrictMode() || slot.context() != PutPropertySlot::ReflectSet)) [[unlikely]]
        return false;
    if (name == vm.propertyNames->prototype)
        closure->ensurePrototypeProperty(vm);
    bool result = Base::put(cell, globalObject, name, value, slot);
    if (field) [[unlikely]] {
        bool mayBeCached = value == given && !state.data.isCompiledForAudit;
        uint32_t kinds = mayBeCached ? kindsStoredUnchanged(state.ownTypeOf(*field)) : 0;
        if (Object* object = mayBeCached && !(kinds & SoundTypeOtherObject) ? state.compiledObject(value) : nullptr) {
            uint32_t shapeWord = static_cast<uint32_t>(object->orderAndShapeWords() >> 32);
            if (shapeWord >> 16 == 0xffff)
                kinds |= shapeWord << 16;
        }
        if (kinds && slot.isCacheablePut() && slot.type() == PutPropertySlot::ExistingProperty && slot.base() == closure)
            slot.setExistingTypedField(closure, slot.cachedOffset(), kinds);
        else
            slot.disableCaching();
    }
    if (name == vm.propertyNames->prototype) [[unlikely]] {
        slot.disableCaching();
        state.didChangePrototypeOf(vm, closure);
    }
    if (name == vm.propertyNames->displayName) [[unlikely]]
        anyClosureHasDisplayName.store(true, std::memory_order_relaxed);
    return result;
}

bool Closure::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& given, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* closure = uncheckedDowncast<Closure>(object);
    State& state = *object->structure()->aotInstance()->typed;
    const LayoutData* ofClass = closure->classData();
    PropertyDescriptor descriptor = given;
    if (const FieldData* field = ofClass && descriptor.value() ? state.staticField(*ofClass, name.uid()) : nullptr) [[unlikely]] {
        JSValue held = coerceToStaticFieldType(globalObject, state, *ofClass, *field, name, descriptor.value());
        RETURN_IF_EXCEPTION(scope, false);
        if (!held)
            return false;
        descriptor.setValue(held);
    }
    closure->ensureClassObject(vm);
    if (name == vm.propertyNames->prototype && descriptor.value() && state.data.isCompiledForAudit && closure->sharesPrototypeProperty()) [[unlikely]]
        RELEASE_AND_RETURN(scope, reportOrThrowSharesPrototypeProperty(globalObject, state, closure));
    if (name == vm.propertyNames->prototype && descriptor.value()) [[unlikely]] {
        bool mayReplace = closure->mayReplacePrototypeProperty(globalObject, descriptor.value(), shouldThrow);
        RETURN_IF_EXCEPTION(scope, false);
        if (!mayReplace)
            return false;
    }
    if (name == vm.propertyNames->prototype)
        closure->ensurePrototypeProperty(vm);
    bool result = Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (name == vm.propertyNames->prototype) [[unlikely]]
        state.didChangePrototypeOf(vm, closure);
    if (name == vm.propertyNames->displayName) [[unlikely]]
        anyClosureHasDisplayName.store(true, std::memory_order_relaxed);
    if (result && name == vm.propertyNames->name && descriptor.value() && descriptor.value().isString() && (closure->isDeclaredAs(functionIsInBuiltinModule) || usesOwnPropertiesForStackTraceName(closure))) [[unlikely]] {
        String reported = asString(descriptor.value())->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        setNameReportedForFrames(closure->functionIndex(), reported, closure->isDeclaredAs(functionIsInBuiltinModule) || state.closureIfCreated(closure->functionIndex()) == closure);
    }
    return result;
}

bool Closure::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    closure->ensureClassObject(vm);
    if (name == vm.propertyNames->prototype)
        closure->ensurePrototypeProperty(vm);
    return Base::deleteProperty(cell, globalObject, name, slot);
}

bool Closure::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    uncheckedDowncast<Closure>(cell)->ensureClassObject(globalObject->vm());
    return Base::putByIndex(cell, globalObject, index, value, shouldThrow);
}

bool Closure::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    uncheckedDowncast<Closure>(object)->ensureClassObject(globalObject->vm());
    return Base::preventExtensions(object, globalObject);
}

bool Closure::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    uncheckedDowncast<Closure>(object)->ensureClassObject(vm);
    const LayoutData* ofClass = uncheckedDowncast<Closure>(object)->classData();
    if (!ofClass || !ofClass->hasImmutableMembers || object->getPrototypeDirect() == prototype || (ofClass->base() == LayoutData::noBase && !ofClass->hasUntypedBase)) {
        if (ofClass && object->getPrototypeDirect() != prototype)
            object->structure()->aotInstance()->typedModifiedClassObjects[ofClass->index] = 1;
        RELEASE_AND_RETURN(scope, Base::setPrototype(object, globalObject, prototype, shouldThrow));
    }
    State& state = *object->structure()->aotInstance()->typed;
    String message = makeString("Cannot set prototype of class "_s, state.nameOf(*ofClass), ": a class keeps its superclass"_s);
    State::Violation violation { .kind = "class-static-write"_s, .on = ofClass };
    if (state.data.isCompiledForAudit)
        RELEASE_AND_RETURN(scope, state.reportOrThrow(globalObject, violation, message, State::InAudit::Ignored));
    if (shouldThrow)
        state.recordViolation(vm, violation);
    return typeError(globalObject, scope, shouldThrow, withAOTErrorNote(message));
}

void Closure::getOwnSpecialPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    closure->ensureClassObject(vm);
    Base::getOwnSpecialPropertyNames(object, globalObject, names, mode);
    if (mode == DontEnumPropertiesMode::Include && hasPrototypePropertyAtCreation(*closure) && closure->ensurePrototypeProperty(vm))
        names.add(vm.propertyNames->prototype);
}

static size_t sizeOfZeroedTable(size_t count, size_t size)
{
    return roundUpToMultipleOf(pageSize(), std::max<size_t>(count, 1) * size);
}

template<typename T>
static T* createZeroedTable(size_t count)
{
    static_assert(std::is_trivially_default_constructible_v<T>, "this table is zeroed pages and its entries are never constructed, so default member initializers would be ignored");
    return static_cast<T*>(OSAllocator::reserveAndCommit(sizeOfZeroedTable(count, sizeof(T))));
}

template<typename T>
static void destroyZeroedTable(T* table, size_t count)
{
    OSAllocator::decommitAndRelease(table, sizeOfZeroedTable(count, sizeof(T)));
}

static size_t allocationEntriesSize(const Data& data)
{
    return sizeOfZeroedTable(std::min(data.numberOfAllocations, State::maxAllocationEntries), sizeof(Instance::TypedAllocation));
}

static size_t allocationEntriesAndIndicesSize(const Data& data)
{
    return allocationEntriesSize(data) + sizeOfZeroedTable(data.numberOfAllocations, sizeof(uint16_t));
}

static constexpr uint32_t stringTableInitialCapacity = 2048;

static uint64_t* createStringTable(uint32_t capacity)
{
    uint64_t* slots = createZeroedTable<uint64_t>(capacity);
    slots[0] = static_cast<uint64_t>(capacity - 1) << 32 | UINT32_MAX;
    return slots;
}

static uint32_t stringTableCapacity(const uint64_t* slots)
{
    return static_cast<uint32_t>(slots[0] >> 32) + 1;
}

struct State::Counts {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Counts);

    struct Allocation {
        uint64_t objects { 0 };
        size_t cellSize { 0 };
    };
    using CountsByPair = UncheckedKeyHashMap<std::pair<uint32_t, uint32_t>, uint64_t>;

    Vector<Allocation> allocations;
    CountsByPair objectsWithoutAllocation;
    CountsByPair transitions;
    CountsByPair orderStates;
    Vector<Vector<uint64_t>> fieldCreations;
    UncheckedKeyHashMap<uint32_t, uint64_t> extensions;
    CountsByPair extensionGrowth;
    CountsByPair closures;
    CountsByPair plainAllocations;
    CountsByPair plainCellBytes;
    CountsByPair plainStorageBytes;
    UncheckedKeyHashMap<String, uint64_t> arrayGets;
    UncheckedKeyHashMap<String, uint64_t> coercions;
    UncheckedKeyHashMap<String, String> coercionTypes;
    UncheckedKeyHashMap<String, uint64_t> arrayPuts;
    UncheckedKeyHashMap<String, uint64_t> remainingArrays;
    UncheckedKeyHashMap<String, std::pair<uint64_t, uint64_t>> compactTables;
    Vector<uint64_t> extraPropertyTables;
    Vector<UncheckedKeyHashMap<String, uint64_t>> extraProperties;
    enum class Place : uint8_t { Field, AbsentField, Extra, NotOwn, OtherReceiver };
    struct Access {
        uint64_t count { 0 };
        RefPtr<UniquedStringImpl> name;
    };
    UncheckedKeyHashMap<std::tuple<uintptr_t, uint32_t, uintptr_t>, Access> accesses;

    struct Collection {
        bool isFull;
        size_t size;
        size_t capacity;
        size_t extraMemorySize;
        size_t blocks;
        size_t bytesInPreciseAllocations;
    };
    enum class LiveKind : uint32_t { Layout = 1, Closure, Extension, ExtraProperties, ExtraPropertiesStorage, Class, ClassStorage, OtherAuxiliary };
    struct FreeCells {
        uint64_t cells { 0 };
        uint64_t blocks { 0 };
        uint64_t emptyBlocks { 0 };
    };
    struct LiveCells {
        size_t collection { 0 };
        UncheckedKeyHashMap<std::tuple<uint32_t, uintptr_t, size_t>, uint64_t> cells;
        UncheckedKeyHashMap<std::pair<Subspace*, size_t>, FreeCells> freeCells;
        uint64_t extraPropertyTables { 0 };
        uint64_t strings { 0 };
        uint64_t stringBytes { 0 };
        uint64_t ropes { 0 };
        uint64_t staticStrings { 0 };
        uint64_t externalStrings { 0 };
        uint64_t substrings { 0 };
        struct Slots {
            uint64_t objects { 0 };
            uint64_t bytes { 0 };
            uint64_t filledBoxed { 0 };
            uint64_t filledWide { 0 };
            uint64_t filledNarrow { 0 };
        };
        struct Field {
            uint64_t filled { 0 };
            uint64_t arrays { 0 };
            uint64_t emptyArrays { 0 };
            uint64_t elements { 0 };
            uint64_t capacity { 0 };
            uint64_t maps { 0 };
            uint64_t emptyMaps { 0 };
        };
        static constexpr unsigned numberOfBuckets = 50;
        static unsigned bucketOf(uint64_t number) { return number <= 16 ? number : 12 + WTF::fastLog2(number - 1) + 1; }
        static uint64_t boundOf(unsigned bucket) { return bucket <= 16 ? bucket : 1ull << (bucket - 12); }
        UncheckedKeyHashMap<const LayoutData*, Slots> slots;
        CountsByPair orderStates;
        UncheckedKeyHashMap<const LayoutData*, Vector<Field>> fields;
        std::array<std::array<uint64_t, numberOfBuckets>, numberOfBuckets> arrays { };
        std::array<uint64_t, numberOfBuckets> maps { };
        std::array<uint64_t, numberOfBuckets> sets { };
        uint64_t arraysReferencedOnce { 0 };
        uint64_t arraysReferencedMoreThanOnce { 0 };
    };
    struct Observer final : HeapObserver {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Observer);
        explicit Observer(State& state)
            : state(state)
        {
        }
        void willGarbageCollect() final { }
        void didGarbageCollect(CollectionScope scope) final { state.countCollection(scope); }
        State& state;
    };
    Lock collectionsLock;
    Vector<Collection> collections;
    LiveCells liveCells;
    std::unique_ptr<Observer> observer;
};

static Lock statesWithCountsLock;
static Vector<State*>& statesWithCounts()
{
    static NeverDestroyed<Vector<State*>> states;
    return states;
}

void State::initializeStatics(uint32_t firstWord, uint32_t endWord)
{
    for (auto& run : std::span { data.at<std::array<uint32_t, 4>>(data.staticsRunsOffset), data.numberOfStaticsRuns }) {
        if (run[0] >= firstWord && run[0] < endWord)
            std::ranges::fill(std::span { reinterpret_cast<uint64_t*>(instance.typedStatics) + run[0], run[1] }, run[2] | static_cast<uint64_t>(run[3]) << 32);
    }
}

void State::createStatics()
{
    RELEASE_ASSERT(data.numberOfCards <= Instance::roundedUpToGranule(data.sizeOfStatics) >> Instance::typedCardShift);
    instance.typedCards = reinterpret_cast<uint8_t*>(&instance) + Instance::typedCardsDisplacement(data.sizeOfStatics);
    instance.typedStatics = reinterpret_cast<uint8_t*>(&instance) + Instance::typedStaticsDisplacement(data.sizeOfStatics);
    initializeStatics(data.sizeOfLazilyInitializedStatics / sizeof(uint64_t), std::numeric_limits<uint32_t>::max());
    m_inaccessibleStaticsSize = static_cast<uint32_t>(WTF::roundDownToMultipleOf(pageSize(), static_cast<size_t>(data.sizeOfLazilyInitializedStatics)));
    if (m_inaccessibleStaticsSize)
        OSAllocator::protect(instance.typedStatics, m_inaccessibleStaticsSize, false, false);
}

void State::initializeProgramStatics()
{
    if (m_inaccessibleStaticsSize)
        OSAllocator::protect(instance.typedStatics, m_inaccessibleStaticsSize, true, true);
    initializeStatics(0, data.sizeOfLazilyInitializedStatics / sizeof(uint64_t));
    WTF::storeStoreFence();
    m_inaccessibleStaticsSize = 0;
    m_programStaticsInitialized = true;
}

void State::destroyStatics()
{
    if (m_inaccessibleStaticsSize)
        OSAllocator::protect(instance.typedStatics, m_inaccessibleStaticsSize, true, true);
}

State::State(VM& vm, Instance& instance, const Data& data)
    : instance(instance)
    , data(data)
    , extraProperties(vm)
    , vmExtraPropertiesHandleTable(ExtraPropertiesHandleTable::of(vm, instance))
    , extraPropertiesHandleTable(vmExtraPropertiesHandleTable.get())
    , numberBoxes(vm)
    , fieldAttributes(vm)
    , m_structuresWithOtherPrototype(vm)
    , m_structuresByExtraProperties(vm)
{
    if (!data.usesWideReferences)
        MarkedBlockCage::require();
    structures = createZeroedTable<uint32_t>(data.numberOfLayouts);
    m_cachedFields = createZeroedTable<CachedField>(numberOfCachedFields);
    m_cachedFieldNames = createZeroedTable<CachedFieldName>(numberOfCachedFieldNames);
    m_cachedTransitions = createZeroedTable<CachedTransition>(numberOfCachedTransitions);
    m_previousTransitions = createZeroedTable<PreviousTransition>(numberOfPreviousTransitions);
    m_castTransitions = createZeroedTable<CastTransition>(1u << CastTransition::indexBits);
    instance.typedCastTransitions = m_castTransitions;
    m_cachedStores = createZeroedTable<CachedStore>(numberOfCachedStores);
    instance.typedKnownStoreTransitions = createZeroedTable<KnownStoreTransition>(1 << KnownStoreTransition::indexBits);
    instance.typedExtraPropertyLocations = createZeroedTable<ExtraPropertyLocation>(1 << ExtraPropertyLocation::indexBits);
    instance.typedLateFieldCreations = createZeroedTable<LateFieldCreation>(1 << LateFieldCreation::indexBits);
    instance.typedCloneCache = createZeroedTable<TypedCloneCacheEntry>(1 << TypedCloneCacheEntry::indexBits);
    instance.typedConversionCache = createZeroedTable<ConversionCacheEntry>(1 << ConversionCacheEntry::indexBits);
    m_propertyConversionCache = createZeroedTable<uintptr_t>(propertyConversionCacheSize);
    m_propertiesWithSupertypes = createZeroedTable<PropertiesWithSupertypes>(numberOfPropertiesWithSupertypes);
    createStatics();
    instance.typedAllocationIndices = reinterpret_cast<uint16_t*>(createZeroedTable<uint8_t>(allocationEntriesAndIndicesSize(data)) + allocationEntriesSize(data));
    instance.typedModifiedClassObjects = createZeroedTable<uint8_t>(data.numberOfLayoutIndices);
    instance.typedClassPrototypes = createZeroedTable<JSCell*>(data.numberOfLayoutIndices);
    instance.typedShapeSets = data.at<uint32_t>(data.shapeSetsOffset);
    instance.negativeOffsetFields().typedStrings = createStringTable(stringTableInitialCapacity);
    instance.typedLiteralStrings = createZeroedTable<JSCell*>(data.numberOfLiteralStrings);
    instance.typedLiteralStringCache = createZeroedTable<uint64_t>(1 << literalStringCacheIndexBits);
    instance.typedData = data.at<uint8_t>(0);
    RELEASE_ASSERT(data.numberOfStructureDescriptors <= Instance::maxStructureDescriptors);
    instance.realmObjectPrototype = instance.globalObject->objectPrototype();
    instance.firstThisStructureDescriptorPlusOne = data.firstThisStructureDescriptor + 1;
    instance.nameDisplacements = data.at<uint32_t>(data.nameDisplacementsOffset);
    instance.offsetsByNameAndStructure = data.at<uint32_t>(data.offsetsByNameAndStructureOffset);
    instance.typedFunctions = FunctionTables::of(vm).words;
    for (unsigned kind = 0; kind < Instance::numberOfTypedExecutables; ++kind)
        instance.typedExecutables[kind] = FunctionTables::of(vm).executables[kind].get();
    instance.typedClosures = createZeroedTable<uint8_t>(data.numberOfFunctions * (data.usesWideReferences ? sizeof(JSCell*) : sizeof(uint32_t)));
    instance.negativeOffsetFields().typedBuiltins = createZeroedTable<JSCell*>(Builtins::numberOfCells());
    realmIsLocked = instance.globalObject->hasImmutableIntrinsics();
    if (realmIsLocked) {
        instance.lockedRealmObjectPrototype = instance.globalObject->objectPrototype();
        for (unsigned index : Builtins::singleton().eagerCells())
            instance.negativeOffsetFields().typedBuiltins[index] = Builtins::singleton().findCell(instance.globalObject, index);
        for (uint32_t index : std::span { data.at<uint32_t>(data.eagerBuiltinCellsOffset), data.numberOfEagerBuiltinCells })
            instance.negativeOffsetFields().typedBuiltins[index] = Builtins::singleton().findCell(instance.globalObject, index);
    }
    instance.typedOrderKeys = createZeroedTable<uintptr_t*>(Instance::numberOfTypedOrderKeyIndices(data.numberOfLayouts));
    instance.typedOrderKeyIndices = createZeroedTable<uint16_t>(data.numberOfLayouts);
    instance.typedKeyLists = createZeroedTable<Instance::TypedKeyList>(Instance::numberOfTypedKeyLists);
    instance.emptyKeyList = JSArray::tryCreate(vm, instance.globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), 0);
    RELEASE_ASSERT(instance.emptyKeyList);
    didCreate(instance.emptyKeyList);
    installGeneratorMethods(instance.globalObject);
    if (data.countsAllocations || Options::countAOTTypedAllocations()) [[unlikely]] {
        m_counts = makeUnique<Counts>();
        m_counts->allocations.grow(data.numberOfAllocations);
        m_counts->fieldCreations.grow(data.numberOfLayouts);
        m_counts->extraPropertyTables.fill(0, data.numberOfLayouts);
        m_counts->extraProperties.grow(data.numberOfLayouts);
        m_counts->observer = makeUnique<Counts::Observer>(*this);
        instance.vm->heap.addObserver(m_counts->observer.get());
        Locker locker { statesWithCountsLock };
        statesWithCounts().append(this);
        static std::once_flag didRegister;
        std::call_once(didRegister, [] {
            std::atexit(printCountsAtExit);
#if OS(LINUX)
            std::at_quick_exit(printCountsAtExit);
#endif
        });
    }
    RELEASE_ASSERT(vm.heap.isDeferred());
}

State::~State()
{
    if (m_counts) [[unlikely]] {
        instance.vm->heap.removeObserver(m_counts->observer.get());
        {
            Locker locker { statesWithCountsLock };
            statesWithCounts().removeFirst(this);
        }
        printCounts();
    }
    destroyZeroedTable(structures, data.numberOfLayouts);
    for (auto& cached : std::span { m_cachedFields, numberOfCachedFields }) {
        if (cached.name)
            cached.name->deref();
    }
    destroyZeroedTable(m_cachedFields, numberOfCachedFields);
    for (auto& cached : std::span { m_cachedFieldNames, numberOfCachedFieldNames }) {
        if (cached.name)
            cached.name->deref();
    }
    destroyZeroedTable(m_cachedFieldNames, numberOfCachedFieldNames);
    destroyZeroedTable(m_cachedTransitions, numberOfCachedTransitions);
    destroyZeroedTable(m_previousTransitions, numberOfPreviousTransitions);
    destroyZeroedTable(m_castTransitions, 1u << CastTransition::indexBits);
    destroyZeroedTable(m_cachedStores, numberOfCachedStores);
    destroyZeroedTable(static_cast<KnownStoreTransition*>(instance.typedKnownStoreTransitions), 1 << KnownStoreTransition::indexBits);
    destroyZeroedTable(static_cast<ExtraPropertyLocation*>(instance.typedExtraPropertyLocations), 1 << ExtraPropertyLocation::indexBits);
    destroyZeroedTable(static_cast<LateFieldCreation*>(instance.typedLateFieldCreations), 1 << LateFieldCreation::indexBits);
    destroyZeroedTable(static_cast<TypedCloneCacheEntry*>(instance.typedCloneCache), 1 << TypedCloneCacheEntry::indexBits);
    destroyZeroedTable(static_cast<ConversionCacheEntry*>(instance.typedConversionCache), 1 << ConversionCacheEntry::indexBits);
    destroyZeroedTable(m_propertyConversionCache, propertyConversionCacheSize);
    destroyZeroedTable(m_propertiesWithSupertypes, numberOfPropertiesWithSupertypes);
    fastFree(m_youngRoots);
    destroyStatics();
    destroyZeroedTable(reinterpret_cast<uint8_t*>(instance.typedAllocationIndices) - allocationEntriesSize(data), allocationEntriesAndIndicesSize(data));
    destroyZeroedTable(instance.typedModifiedClassObjects, data.numberOfLayoutIndices);
    destroyZeroedTable(instance.typedClassPrototypes, data.numberOfLayoutIndices);
    destroyZeroedTable(instance.negativeOffsetFields().typedStrings, stringTableCapacity(instance.negativeOffsetFields().typedStrings));
    destroyZeroedTable(instance.typedLiteralStrings, data.numberOfLiteralStrings);
    destroyZeroedTable(instance.typedLiteralStringCache, 1 << literalStringCacheIndexBits);
    for (uint32_t capacity = stringTableInitialCapacity; uint64_t* former : m_retiredStringTables) {
        destroyZeroedTable(former, capacity);
        capacity *= 2;
    }
    destroyZeroedTable(static_cast<uint8_t*>(instance.typedClosures), data.numberOfFunctions * (data.usesWideReferences ? sizeof(JSCell*) : sizeof(uint32_t)));
    destroyZeroedTable(instance.negativeOffsetFields().typedBuiltins, Builtins::numberOfCells());
    destroyZeroedTable(instance.typedOrderKeys, Instance::numberOfTypedOrderKeyIndices(data.numberOfLayouts));
    destroyZeroedTable(instance.typedOrderKeyIndices, data.numberOfLayouts);
    destroyZeroedTable(instance.typedKeyLists, Instance::numberOfTypedKeyLists);
}

} // namespace Typed

bool convertsToOrdinaryObject(const ClassInfo* from, const ClassInfo* to)
{
    return ((from == Typed::ClassPrototype::info() || from == Typed::Object::info()) && to == JSFinalObject::info()) || (from == JSFinalObject::info() && (to == Typed::Object::info() || to == Typed::GuardedObject::info()));
}

namespace Typed {

JSObject* State::importMetaForModule(JSGlobalObject* globalObject, JSString* path)
{
    String text = path->value(globalObject);
    for (auto& [known, meta] : m_moduleImportMetas) {
        if (known == text)
            return meta;
    }
    JSObject* meta = globalObject->moduleLoader()->createImportMetaProperties(globalObject, path, uncheckedDowncast<JSModuleRecord>(moduleRecord), nullptr);
    if (!meta)
        return nullptr;
    didCreate(meta);
    m_moduleImportMetas.append({ WTF::move(text), meta });
    return meta;
}

void State::didCreate(JSCell* cell)
{
    Locker locker { m_cellsLock };
    m_cells.append(cell);
}

void State::captureRealmBuiltins()
{
    VM& vm = *instance.vm;
    auto& builtins = Builtins::singleton();
    if (Instance* first = instance.globalObject->aotInstance(); first != &instance) {
        memcpySpan(std::span { instance.negativeOffsetFields().typedBuiltins, Builtins::numberOfCells() }, std::span { first->negativeOffsetFields().typedBuiltins, Builtins::numberOfCells() });
        return;
    }
    for (unsigned index = 0; index < Builtins::firstPrototypeCell(); ++index) {
        if (index >= builtins.originalStructureCell(0) || builtins.isReplaceableGlobalCell(index)) {
            instance.negativeOffsetFields().typedBuiltins[index] = vm.structureStructure.get();
            continue;
        }
        if (!builtins.hasCellAt(index))
            continue;
        instance.negativeOffsetFields().typedBuiltins[index] = builtins.captureCell(instance.globalObject, nullptr, index);
        didCreate(instance.negativeOffsetFields().typedBuiltins[index]);
    }
}

void State::captureBuiltinsFrom(ASCIILiteral global, JSObject* object)
{
    auto& builtins = Builtins::singleton();
    instance.negativeOffsetFields().typedBuiltins[builtins.cellOf(builtins.nameOf(asBytes(global.span8())))] = object;
    didCreate(object);
    for (unsigned index : builtins.replaceableGlobalCells()) {
        if (builtins.at(BuiltinID(index)).holder != global)
            continue;
        instance.negativeOffsetFields().typedBuiltins[index] = builtins.captureCell(instance.globalObject, object, index);
        didCreate(instance.negativeOffsetFields().typedBuiltins[index]);
    }
}

Structure* State::compactTableStructure()
{
    if (!m_compactTableStructure) {
        m_compactTableStructure = CompactTable::createStructure(instance.globalObject->vm());
        didCreate(m_compactTableStructure);
    }
    return m_compactTableStructure;
}

static String textOf(std::span<const uint8_t>);

static const Data* dataInImageIfAny()
{
    Image* image = Image::withCode();
    return image && image->header().typedDataSize ? image->at<Data>(image->header().typedDataOffset) : nullptr;
}

bool isModuleNamespace(JSCell* cell)
{
    return cell->inherits<ModuleNamespace>();
}

void detachModuleNamespace(JSGlobalObject* globalObject, JSObject* object)
{
    uncheckedDowncast<ModuleNamespace>(object)->detach(globalObject);
}

bool isEntryPoint(uint32_t module)
{
    const Data* data = dataInImageIfAny();
    return data && data->isEntryPoint(module);
}

JSValue requireCompiledEntryPoint(JSGlobalObject* globalObject, AbstractModuleRecord* record)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* yielded = Instance::compiledEntryPointImport(record);
    if (!yielded)
        return { };
    MarkedArgumentBuffer arguments;
    arguments.append(jsBoolean(true));
    JSValue seen = call(globalObject, yielded, JSC::getCallData(yielded), jsUndefined(), arguments);
    RETURN_IF_EXCEPTION(scope, { });
    if (seen.inherits<JSPromise>())
        return { };
    return seen;
}

static std::span<const std::array<uint32_t, 3>> builtinFunctionsInImage()
{
    const Data* data = dataInImageIfAny();
    if (!data)
        return { };
    return { data->at<std::array<uint32_t, 3>>(data->builtinFunctionsOffset), data->numberOfBuiltinFunctions };
}

static const std::array<uint32_t, 3>* findBuiltinFunction(uint32_t id)
{
    auto functions = builtinFunctionsInImage();
    auto found = std::ranges::lower_bound(functions, id, { }, [](auto& function) {
        return function[0];
    });
    return found != functions.end() && (*found)[0] == id ? &*found : nullptr;
}

static const LayoutData* classConstructedBy(const Data& data, uint32_t function)
{
    if (static_cast<Format::FunctionKind>(data.at<uint8_t>(data.functionKindsOffset)[function] & functionKindMask) != Format::FunctionKind::Constructor)
        return nullptr;
    std::span constructors { data.at<std::array<uint32_t, 2>>(data.constructorsOffset), data.numberOfConstructors };
    auto found = std::ranges::lower_bound(constructors, function, { }, [](auto& constructorAndClass) {
        return constructorAndClass[0];
    });
    RELEASE_ASSERT(found != constructors.end() && (*found)[0] == function);
    uint32_t offset = data.offsetOfClass((*found)[1]);
    return offset ? data.at<LayoutData>(offset) : nullptr;
}

static const AllocationData* thisAllocation(const Data& data, uint32_t function)
{
    std::span functions { data.at<std::array<uint32_t, 2>>(data.constructorFunctionsOffset), data.numberOfConstructorFunctions };
    auto found = std::ranges::lower_bound(functions, function, { }, [](auto& functionAndAllocation) {
        return functionAndAllocation[0];
    });
    return found != functions.end() && (*found)[0] == function ? &data.at<AllocationData>(data.allocationsOffset)[(*found)[1]] : nullptr;
}

FunctionTables::FunctionTables(VM& vm)
    : data(*dataInImageIfAny())
    , words(ProgramData::get()->closureWords())
{
    static_assert(numberOfExecutables == Instance::numberOfTypedExecutables);
    RELEASE_ASSERT(data.numberOfFunctions <= ClosureWord::firstReservedFunction);
    for (auto& builtin : builtinFunctionsInImage()) {
        if (static_cast<ImplementationVisibility>(builtin[2]) == ImplementationVisibility::PrivateRecursive)
            functionsHiddenWithCallees.append(builtin[1]);
    }
    RELEASE_ASSERT(!Options::useJIT());
    for (unsigned kind = 0; kind < executables.size(); ++kind) {
        bool isClass = kind == 4;
        bool isBound = kind == boundClosureExecutableKind;
        auto visibility = !isClass && !isBound && kind & 1 ? ImplementationVisibility::PrivateRecursive : ImplementationVisibility::Private;
        NativeExecutable* executable = vm.getHostFunction(isClass ? callClassConstructor : callClosure, visibility, isClass || (!isBound && kind & 2) ? constructClosure : callHostFunctionAsConstructor, 0, String());
        executable->setIsSharedByAOTTypedClosures();
        if (!isClass)
            executable->setCallEntrypoint(closureEntrypoint());
        executables[kind] = Strong<JSCell>(vm, executable);
    }
}

NativeExecutable* FunctionTables::executableOf(uint32_t function) const
{
    return uncheckedDowncast<NativeExecutable>(executables[executableKind(data.at<uint8_t>(data.functionKindsOffset)[function], functionsHiddenWithCallees.contains(function))].get());
}

FunctionTables& FunctionTables::of(VM& vm)
{
    VMProgram* program = VMProgram::of(vm);
    RELEASE_ASSERT(program);
    if (!program->typedFunctionTables)
        program->typedFunctionTables = new FunctionTables(vm);
    return *program->typedFunctionTables;
}

void destroy(FunctionTables* tables)
{
    delete tables;
}

static uint64_t closureWord(const Data& data, uint32_t function, uint64_t relative)
{
    uint64_t word = ClosureWord::encode(relative, function);
    uint8_t kindAndDeclaration = data.at<uint8_t>(data.functionKindsOffset)[function];
    auto kind = static_cast<Format::FunctionKind>(kindAndDeclaration & functionKindMask);
    bool constructs = kind == Format::FunctionKind::Constructor || (kind == Format::FunctionKind::Plain && !(kindAndDeclaration & (functionIsAsync | functionIsGenerator)));
    if (kind == Format::FunctionKind::Constructor)
        word |= 1ULL << ClosureWord::isNotCallableBit;
    std::span constructors { data.at<std::array<uint32_t, 2>>(data.constructorsOffset), data.numberOfConstructors };
    bool isListed = std::ranges::binary_search(constructors, function, { }, [](auto& constructorAndClass) {
        return constructorAndClass[0];
    });
    const LayoutData* ofClass = isListed ? classConstructedBy(data, function) : nullptr;
    if (ofClass ? !ofClass->hasUntypedBase && !ofClass->constructorTakesNewTarget : constructs && kind == Format::FunctionKind::Plain)
        word |= 1ULL << ClosureWord::constructsOrdinaryObjectBit;
    if (ofClass && !ofClass->hasFixedLayout && ofClass->hasUntypedBase && ofClass->constructorTakesNewTarget)
        word |= 1ULL << ClosureWord::isDerivedConstructorBit;
    RELEASE_ASSERT((word >> ClosureWord::isDerivedConstructorBit & 3) != 3);
    return word;
}

static Structure* realmClosureStructure(VM& vm, JSGlobalObject* globalObject, unsigned which)
{
    if (Structure* structure = globalObject->aotClosureStructure(which))
        return structure;
    JSObject* prototype = nullptr;
    switch (which) {
    case 0:
        prototype = globalObject->functionPrototype();
        break;
    case 1:
        prototype = globalObject->asyncFunctionPrototype();
        break;
    case 2:
        prototype = globalObject->generatorFunctionPrototype();
        break;
    case 3:
        prototype = globalObject->asyncGeneratorFunctionPrototype();
        break;
    case classObjectClosureStructure:
        prototype = globalObject->functionPrototype();
        break;
    }
    RELEASE_ASSERT(prototype);
    Structure* structure = Closure::createStructure(vm, globalObject, prototype);
    globalObject->setAOTClosureStructure(vm, which, structure);
    return structure;
}

bool hasBuiltinFunction(VM&, uint32_t id)
{
    return !!findBuiltinFunction(id);
}

JSFunction* builtinFunction(JSGlobalObject* globalObject, uint32_t id, JSObject* prototype)
{
    VM& vm = globalObject->vm();
    auto* builtin = findBuiltinFunction(id);
    RELEASE_ASSERT(builtin);
    uint32_t function = (*builtin)[1];
    const Data& data = *dataInImageIfAny();
    RELEASE_ASSERT(closureStructureOf(data.at<uint8_t>(data.functionKindsOffset)[function]) != classObjectClosureStructure);
    Closure* closure = Closure::create(vm, realmClosureStructure(vm, globalObject, closureStructureOf(data.at<uint8_t>(data.functionKindsOffset)[function])), function);
    if (prototype)
        closure->setPrototypeDirect(vm, prototype);
    return closure;
}

void didFinishCreation(JSGlobalObject* globalObject)
{
    VMProgram* program = globalObject->vm().m_aotProgram.get();
    if (!program || !program->typedFunctionTables)
        return;
    for (auto& closure : program->typedFunctionTables->closuresWithoutInstance) {
        if (closure && closure->structure()->realm() == globalObject) {
            Instance::ensure(globalObject);
            return;
        }
    }
}

void State::adoptRealmClosures(VM& vm)
{
    closureStructures(vm);
    JSGlobalObject* globalObject = instance.globalObject;
    if (instance.loader() != globalObject->moduleLoader())
        return;
    FunctionTables::of(vm).closuresWithoutInstance.removeAllMatching([&](Weak<JSCell>& closure) {
        if (!closure)
            return true;
        if (closure->structure()->realm() != globalObject)
            return false;
        for (Structure* structure = closure->structure(); structure && !structure->aotInstance(); structure = structure->previousID())
            structure->setAOTInstance(&instance);
        return true;
    });

}

static std::optional<uint32_t> builtinModuleEntrypoint(uint32_t id)
{
    Image* image = Image::withCode();
    if (!image || !image->header().typedDataSize)
        return std::nullopt;
    auto& data = *image->at<Data>(image->header().typedDataOffset);
    std::span modules { data.at<std::array<uint32_t, 2>>(data.builtinModulesOffset), data.numberOfBuiltinModules };
    auto found = std::ranges::lower_bound(modules, id, { }, [](auto& idAndEntrypoint) {
        return idAndEntrypoint[0];
    });
    if (found == modules.end() || (*found)[0] != id)
        return std::nullopt;
    return (*found)[1];
}

bool hasBuiltinModule(JSGlobalObject*, uint32_t id)
{
    return !!builtinModuleEntrypoint(id);
}

void willRequestFileFromProgram(JSGlobalObject* globalObject, const String& path)
{
    State& state = *Instance::ensure(globalObject).typed;
    if (!state.moduleRecord && state.requestedFilePath.isNull())
        state.requestedFilePath = path;
}

Identifier State::importerKey(VM& vm) const
{
    if (moduleRecord) [[likely]]
        return moduleRecord->moduleKey();
    return requestedFilePath.isNull() ? vm.propertyNames->emptyIdentifier : Identifier::fromString(vm, requestedFilePath);
}

JSValue evaluateBuiltinModule(JSGlobalObject* globalObject, uint32_t id)
{
    constexpr uint32_t programFilesByPath = 0x7fffffff;
    if (id == programFilesByPath && !globalObject->hasImmutableIntrinsics()) [[unlikely]]
        return jsUndefined();
    auto entrypoint = builtinModuleEntrypoint(id);
    RELEASE_ASSERT(entrypoint);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = *Instance::ensure(globalObject).typed;
    using Outcome = State::BuiltinModule::Outcome;
    if (auto earlier = state.m_builtinModules.find(id); earlier != state.m_builtinModules.end()) [[unlikely]] {
        switch (earlier->value.outcome) {
        case Outcome::Exports:
            return JSValue::decode(earlier->value.value);
        case Outcome::Thrown:
            throwException(globalObject, scope, JSValue::decode(earlier->value.value));
            return { };
        case Outcome::None:
            throwStackOverflowError(globalObject, scope);
            return { };
        }
    }
    constexpr size_t stackToStartWith = 128 * KB;
    if (std::bit_cast<uintptr_t>(currentStackPointer()) - std::bit_cast<uintptr_t>(vm.softStackLimit()) < stackToStartWith || !vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return { };
    }
    JSObject* module = state.closureObjectFor(vm, *entrypoint);
    state.m_builtinModules.add(id, State::BuiltinModule { });
    JSValue exports = call(globalObject, module, JSC::getCallData(module), jsUndefined(), ArgList());
    Exception* exception = scope.exception();
    if (exception && vm.isTerminationException(exception)) [[unlikely]]
        return { };
    JSValue outcome = exception ? exception->value() : exports;
    if (outcome.isCell())
        state.didCreate(outcome.asCell());
    state.m_builtinModules.set(id, State::BuiltinModule { exception ? Outcome::Thrown : Outcome::Exports, JSValue::encode(outcome) });
    return exception ? JSValue() : exports;
}

Identifier enginePrivateName(VM& vm, const String& text)
{
    if (PrivateSymbolImpl* symbol = vm.propertyNames->builtinNames().lookUpPrivateName(text))
        return Identifier::fromUid(vm, symbol);
    Ref<PrivateSymbolImpl> symbol = PrivateSymbolImpl::create(*text.impl());
    Identifier name = Identifier::fromUid(vm, symbol.ptr());
    vm.propertyNames->appendExternalName(Identifier::fromString(vm, text), name);
    SUPPRESS_UNCOUNTED_LOCAL auto* leaked = &symbol.leakRef();
    UNUSED_VARIABLE(leaked);
    return name;
}

void didCreateReplaceableGlobal(JSGlobalObject* globalObject, ASCIILiteral name, JSObject* object)
{
    if (Instance* instance = globalObject->aotInstance(); instance && instance->typed)
        instance->typed->captureBuiltinsFrom(name, object);
}

void didCreateHostClassStructure(JSGlobalObject* globalObject, unsigned number, Structure* structure)
{
    if (Instance* instance = globalObject->aotInstance(); instance && number < Instance::numberOfHostClasses)
        instance->hostClassStructureIDs[number] = structure->id().bits();
}

static String textOf(std::span<const uint8_t> bytes)
{
    if (charactersAreAllASCII(byteCast<Latin1Character>(bytes)))
        return StringImpl::createWithoutCopying(byteCast<Latin1Character>(bytes));
    if (String text = String::fromUTF8(bytes); !text.isNull())
        return text;
    return String(decodeProgramString(bytes).span());
}

static String constantString(const Data&, uint32_t index);

static String atomOf(const Data& data, uint32_t index)
{
    if (uint32_t facts = data.stringFacts(index); facts & Data::stringIsASCII) {
        auto characters = byteCast<Latin1Character>(data.stringInImage(index));
        WTF::HashTranslatorCharBuffer<Latin1Character> buffer { characters, facts & Data::stringHasIdentifierNumber ? data.identifierHash(facts & Data::stringHashMask) : facts & Data::stringHashMask };
        RELEASE_ASSERT(AtomStringImpl::isValidPrecomputedHash(buffer.hash));
        ASSERT(buffer.hash == StringHasher::computeHashAndMaskTop8Bits(buffer.characters));
        return AtomStringImpl::addWithoutCopying(buffer);
    }
    return AtomString(constantString(data, index)).string();
}

static Identifier createConstantSymbol(VM& vm, std::span<const uint8_t> name)
{
    size_t endOfHead = 1;
    while (name[endOfHead] != ':' && name[endOfHead] != ';')
        ++endOfHead;
    if (name[endOfHead] == ';')
        return Identifier::fromUid(SymbolImpl::createNullSymbol().get());
    String text = textOf(name.subspan(endOfHead + 1));
    if (name[1] == 'r')
        return Identifier::fromUid(vm.symbolRegistry().symbolForKey(text).get());
    if (name[1] == 'w') {
#define RETURN_IF_NAMED(symbol) \
        if (text == #symbol##_s) \
            return vm.propertyNames->symbol##Symbol;
        JSC_COMMON_PRIVATE_IDENTIFIERS_EACH_WELL_KNOWN_SYMBOL(RETURN_IF_NAMED)
#undef RETURN_IF_NAMED
        RELEASE_ASSERT_NOT_REACHED();
    }
    return Identifier::fromUid(SymbolImpl::create(*text.impl()).get());
}

static Identifier createNumberedIdentifier(VM& vm, const Data& data, uint32_t index)
{
    if (data.stringFacts(index) & Data::stringIsASCII) [[likely]]
        return Identifier::fromString(vm, atomOf(data, index));
    auto name = data.string(index);
    if (isConstantSymbol(name))
        return createConstantSymbol(vm, name);
    if (isEnginePrivateName(name))
        return enginePrivateName(vm, textOf(name.subspan(1)));
    return Identifier::fromString(vm, atomOf(data, index));
}

Identifier tryCreateIdentifier(VM& vm, uint32_t identifier)
{
    const Data* data = dataInImageIfAny();
    uint32_t stringPlusOne = data ? data->identifierStringPlusOne(identifier) : 0;
    if (!stringPlusOne)
        return Identifier();
    return createNumberedIdentifier(vm, *data, stringPlusOne - 1);
}

static String constantString(const Data& data, uint32_t index)
{
    if (uint32_t facts = data.stringFacts(index); facts & Data::stringIsASCII)
        return StringImpl::createWithoutCopyingWithHash(byteCast<Latin1Character>(data.stringInImage(index)), facts & Data::stringHasIdentifierNumber ? data.identifierHash(facts & Data::stringHashMask) : facts & Data::stringHashMask);
    std::span decoded { data.at<std::array<uint32_t, 3>>(data.decodedStringsOffset), data.numberOfDecodedStrings };
    auto found = std::ranges::lower_bound(decoded, index, { }, [](auto& entry) {
        return entry[0];
    });
    if (found == decoded.end() || (*found)[0] != index) {
        RELEASE_ASSERT_WITH_MESSAGE(!data.hasOnlyDecodedForm(index), "AOT: the text of a string is asked for that was left out of the image: nothing was seen to name it");
        return textOf(data.string(index));
    }
    size_t length = (*found)[2] & ~Data::isDecodedTo16Bits;
    if ((*found)[2] & Data::isDecodedTo16Bits)
        return StringImpl::createWithoutCopying(std::span { data.at<char16_t>((*found)[1]), length });
    return StringImpl::createWithoutCopying(std::span { data.at<Latin1Character>((*found)[1]), length });
}

std::span<const uint8_t> Data::encodedBytesOfDecodedString(uint32_t index) const
{
    static Lock lock;
    static NeverDestroyed<std::map<std::pair<const Data*, uint32_t>, Vector<uint8_t>>> encodedForms;
    Locker locker { lock };
    auto [entry, isNew] = encodedForms->try_emplace({ this, index });
    if (!isNew)
        return entry->second.span();
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        dataLogLn("AOT: the bytes of string ", index, " are asked for, which stands in the image decoded alone");
    String text = constantString(*this, index);
    auto& bytes = entry->second;
    for (unsigned i = 0; i < text.length(); ++i) {
        char32_t character = text[i];
        if (U16_IS_LEAD(character) && i + 1 < text.length() && U16_IS_TRAIL(text[i + 1]))
            character = U16_GET_SUPPLEMENTARY(character, text[++i]);
        if (character < 0x80)
            bytes.append(static_cast<uint8_t>(character));
        else if (character < 0x800)
            bytes.appendList({ static_cast<uint8_t>(0xc0 | character >> 6), static_cast<uint8_t>(0x80 | (character & 0x3f)) });
        else if (character < 0x10000)
            bytes.appendList({ static_cast<uint8_t>(0xe0 | character >> 12), static_cast<uint8_t>(0x80 | (character >> 6 & 0x3f)), static_cast<uint8_t>(0x80 | (character & 0x3f)) });
        else
            bytes.appendList({ static_cast<uint8_t>(0xf0 | character >> 18), static_cast<uint8_t>(0x80 | (character >> 12 & 0x3f)), static_cast<uint8_t>(0x80 | (character >> 6 & 0x3f)), static_cast<uint8_t>(0x80 | (character & 0x3f)) });
    }
    RELEASE_ASSERT(bytes.size() <= stringSize(index));
    return bytes.span();
}

std::span<GlobalPropertyCache> State::globalPropertyCaches() const
{
    return { reinterpret_cast<GlobalPropertyCache*>(instance.typedStatics + data.globalPropertyCachesOffset), data.numberOfGlobalPropertyCaches };
}

bool State::trustGlobalProperty(VM& vm, JSGlobalObject* globalObject, uint32_t cacheNumber, UniquedStringImpl* uid, JSValue value)
{
    if (m_globalPropertyWatchpoints.isEmpty())
        m_globalPropertyWatchpoints.grow(data.numberOfGlobalPropertyCaches);
    auto& watchpoint = m_globalPropertyWatchpoints[cacheNumber - 1];
    if (watchpoint && watchpoint->isWatching())
        return watchpoint->key().requiredValue() == value;
    ObjectPropertyCondition condition = generateConditionForSelfEquivalence(vm, nullptr, globalObject, uid);
    if (!condition || condition.requiredValue() != value || !condition.isWatchable(PropertyCondition::EnsureWatchability))
        return false;
    if (!watchpoint)
        watchpoint = makeUnique<GlobalPropertyWatchpoint>();
    watchpoint->watch(vm, condition, globalPropertyCaches()[cacheNumber - 1]);
    return true;
}

bool State::trustGlobalMember(VM& vm, JSGlobalObject* globalObject, uint32_t cacheNumber, UniquedStringImpl* global, JSObject* holder, UniquedStringImpl* name)
{
    if (Structure* structure = holder->structure(); structure->isDictionary() && !structure->isUncacheableDictionary() && !structure->hasBeenFlattenedBefore())
        structure->flattenDictionaryStructure(vm, holder);
    Structure* structure = holder->structure();
    if (structure->isDictionary() || !structure->propertyAccessesAreCacheable() || structure->typeInfo().overridesGetOwnPropertySlot())
        return false;
    unsigned attributes;
    if (!isValidOffset(structure->get(vm, name, attributes)) || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
        return false;
    ObjectPropertyCondition ofGlobal = generateConditionForSelfEquivalence(vm, nullptr, globalObject, global);
    if (!ofGlobal || ofGlobal.requiredValue() != JSValue(holder) || !ofGlobal.isWatchable(PropertyCondition::EnsureWatchability))
        return false;
    ObjectPropertyCondition forHolder = generateConditionForSelfEquivalence(vm, nullptr, holder, name);
    if (!forHolder || !forHolder.isWatchable(PropertyCondition::EnsureWatchability))
        return false;
    if (m_globalPropertyWatchpoints.isEmpty())
        m_globalPropertyWatchpoints.grow(data.numberOfGlobalPropertyCaches);
    if (m_memberHolderWatchpoints.isEmpty())
        m_memberHolderWatchpoints.grow(data.numberOfGlobalPropertyCaches);
    GlobalPropertyCache& cache = globalPropertyCaches()[cacheNumber - 1];
    auto& holderWatchpoint = m_memberHolderWatchpoints[cacheNumber - 1];
    auto& memberWatchpoint = m_globalPropertyWatchpoints[cacheNumber - 1];
    holderWatchpoint = makeUnique<GlobalPropertyWatchpoint>();
    memberWatchpoint = makeUnique<GlobalPropertyWatchpoint>();
    holderWatchpoint->watch(vm, ofGlobal, cache, &memberWatchpoint);
    memberWatchpoint->watch(vm, forHolder, cache, &holderWatchpoint);
    cache = { 0, 0, 0, JSValue::encode(forHolder.requiredValue()) };
    instance.didStoreRootInStatics(data.globalPropertyCachesOffset + (cacheNumber - 1) * sizeof(GlobalPropertyCache) + OBJECT_OFFSETOF(GlobalPropertyCache, value));
    return true;
}

const Identifier& State::name(VM& vm, uint32_t index)
{
    if (uint32_t facts = data.stringFacts(index); facts & Data::stringHasIdentifierNumber) [[likely]] {
        if (const Identifier* made = instance.program->identifierIfCreated(facts & Data::stringHashMask)) [[likely]]
            return *made;
    }
    return nameSlow(vm, index);
}

NEVER_INLINE const Identifier& State::nameSlow(VM& vm, uint32_t index)
{
    if (uint32_t facts = data.stringFacts(index); facts & Data::stringHasIdentifierNumber) {
        uint32_t number = facts & Data::stringHashMask;
        ASSERT(data.identifierStringPlusOne(number) == index + 1);
        if (instance.program->isIdentifierRetained(number)) [[unlikely]]
            return instance.program->identifierAsIdentifier(number);
        return instance.program->retainIdentifier(number, createNumberedIdentifier(vm, data, index));
    }
    if (auto found = m_nameIndices.find(index); found != m_nameIndices.end())
        return m_names[found->value];
    Identifier name;
    {
        uint32_t facts = data.stringFacts(index);
        if (facts & Data::stringIsPrivateName) {
            auto bytes = data.string(index);
            auto written = bytes.subspan(1, std::ranges::find(bytes, static_cast<uint8_t>('@')) - bytes.begin() - 1);
            name = Identifier::fromUid(PrivateSymbolImpl::create(*textOf(written).impl()).get());
        } else if (facts & Data::stringIsEnginePrivateName) {
            name = enginePrivateName(vm, textOf(data.string(index).subspan(1)));
        } else if (!(facts & Data::stringIsASCII) && isConstantSymbol(data.string(index)))
            name = createConstantSymbol(vm, data.string(index));
        else
            name = Identifier::fromString(vm, atomOf(data, index));
    }
    m_nameIndices.add(index, static_cast<uint32_t>(m_names.size()));
    m_names.append(WTF::move(name));
    return m_names.last();
}

JSCell* State::stringIfCreated(uint32_t number) const
{
    const uint64_t* slots = instance.negativeOffsetFields().typedStrings;
    uint32_t mask = stringTableCapacity(slots) - 1;
    uint32_t at = startInStringTable(number + 1);
    for (uint32_t step = 1;; at += step++) {
        uint64_t slot = slots[at & mask];
        if (static_cast<uint32_t>(slot) == number + 1)
            return data.usesWideReferences ? instance.typedWideStrings[slot >> 32] : MarkedBlockCage::decode(slot >> 32);
        if (!slot)
            return nullptr;
    }
}

void State::setString(uint32_t number, JSCell* cell)
{
    Locker locker { m_cellsLock };
    auto put = [](uint64_t* slots, uint64_t slot) {
        uint32_t mask = stringTableCapacity(slots) - 1;
        uint32_t home = startInStringTable(static_cast<uint32_t>(slot)) & mask;
        if (home)
            std::swap(slot, slots[home]);
        if (!slot)
            return;
        uint32_t at = startInStringTable(static_cast<uint32_t>(slot));
        uint32_t step = 1;
        if (home) {
            while ((at & mask) != home)
                at += step++;
        }
        while (slots[at & mask])
            at += step++;
        slots[at & mask] = slot;
    };
    uint32_t capacity = stringTableCapacity(instance.negativeOffsetFields().typedStrings);
    if (++m_createdStringCount >= capacity - capacity / 4) {
        uint64_t* grown = createStringTable(2 * capacity);
        for (uint64_t slot : std::span { instance.negativeOffsetFields().typedStrings, capacity }.subspan(1)) {
            if (slot)
                put(grown, slot);
        }
        m_retiredStringTables.append(std::exchange(instance.negativeOffsetFields().typedStrings, grown));
#if !OS(WINDOWS)
        OSAllocator::decommit(m_retiredStringTables.last(), sizeOfZeroedTable(capacity, sizeof(uint64_t)));
#endif
    }
    uint64_t kept;
    if (data.usesWideReferences) {
        kept = m_wideStrings.size();
        m_wideStrings.append(cell);
        instance.typedWideStrings = m_wideStrings.mutableSpan().data();
    } else {
        kept = MarkedBlockCage::encode(cell);
        auto& recentlyCreated = m_isLastStringConstantBackedByStaticData ? m_recentStaticDataStrings : m_recentStrings;
        recentlyCreated.cells.append(static_cast<uint32_t>(kept));
    }
    put(instance.negativeOffsetFields().typedStrings, kept << 32 | (number + 1));
}

JSString* State::createStringConstant(VM& vm, uint32_t index)
{
    DeferGCForAWhile deferGC(vm);
    if (data.stringFacts(index) & Data::stringIsEnginePrivateName)
        return jsEmptyString(vm);
    String text = data.isAtomString(index) ? atomOf(data, index) : constantString(data, index);
    m_isLastStringConstantBackedByStaticData = Image::isBackedByStaticData(*text.impl());
    if (m_isLastStringConstantBackedByStaticData)
        return jsOwnedString(vm, text);
    return jsString(vm, text);
}

JSString* State::createAndCacheStringConstant(VM& vm, uint32_t index)
{
    JSString* string = createStringConstant(vm, index);
    setString(index, string);
    return string;
}

void State::setBigIntConstant(uint32_t index, JSCell* cell)
{
    didCreate(cell);
    m_bigIntConstants.add(index, cell);
}

JSString* State::string(VM& vm, uint32_t index)
{
    if (JSCell* cell = stringIfCreated(index))
        return uncheckedDowncast<JSString>(cell);
    return createAndCacheStringConstant(vm, index);
}

JSString* State::stringForLiteralStringWord(uint64_t word)
{
    uint32_t number = literalStringIndex(static_cast<uint32_t>(word));
    JSCell*& cell = instance.typedLiteralStrings[number];
    if (!cell) {
        VM& vm = *instance.vm;
        uint32_t text = data.literalStringID(number);
        auto bytes = data.string(text);
        if (bytes.empty())
            cell = jsEmptyString(vm);
        else if (bytes.size() == 1 && isASCII(bytes[0]))
            cell = vm.smallStrings.singleCharacterString(bytes[0]);
        else
            cell = string(vm, text);
    }
    return uncheckedDowncast<JSString>(cell);
}

uint64_t State::literalStringWordFor(JSString* string)
{
    unsigned length = string->length();
    if (!length)
        return emptyLiteralStringWord;
    uint64_t itself = std::bit_cast<uint64_t>(string);
    auto find = [&](uint32_t hash, StringView text) -> uint64_t {
        auto* table = data.at<uint32_t>(data.literalStringTableOffset);
        constexpr uint32_t indexMask = (1u << Data::literalStringTableIndexBits) - 1;
        for (uint32_t place = hash & data.literalStringTableMask;; place = (place + 1) & data.literalStringTableMask) {
            uint32_t entry = table[place];
            if (!entry)
                return itself;
            uint32_t index = (entry & indexMask) - 1;
            if (entry != Data::literalStringTableEntry(index, hash))
                continue;
            uint32_t id = data.literalStringID(index);
            auto characters = byteCast<Latin1Character>(data.stringInImage(id));
            if (data.stringFacts(id) & Data::stringIsASCII ? characters.size() == text.length() && equal(text, characters) : hasText(id, text))
                return literalStringWord(index);
        }
    };
    if (length > data.maxLiteralStringLength)
        return itself;
    if (StringImpl* impl = string->tryGetValueImpl()) {
        uint64_t word = find(impl->hash(), StringView(*impl));
        uintptr_t address = std::bit_cast<uintptr_t>(impl);
        uint64_t place = literalStringCacheIndex(address);
        m_literalStringCacheKeys[place] = impl;
        instance.typedLiteralStringCache[place] = literalStringCacheEntry(address, word == itself ? 0 : static_cast<uint32_t>(word));
        return word;
    }
    if (string->isSubstring()) {
        auto characters = string->view(instance.globalObject);
        return find(characters->is8Bit() ? StringHasher::computeHashAndMaskTop8Bits<Latin1Character>(characters->span8()) : StringHasher::computeHashAndMaskTop8Bits<char16_t>(characters->span16()), characters.data);
    }
    if (string->is8Bit()) {
        Vector<Latin1Character, 64> characters(length);
        string->resolveToBuffer(characters.mutableSpan());
        return find(StringHasher::computeHashAndMaskTop8Bits<Latin1Character>(characters.span()), StringView(characters.span()));
    }
    Vector<char16_t, 64> characters(length);
    string->resolveToBuffer(characters.mutableSpan());
    return find(StringHasher::computeHashAndMaskTop8Bits<char16_t>(characters.span()), StringView(characters.span()));
}

RegExp* State::regExpLiteral(VM& vm, uint32_t source, uint32_t flags)
{
    RegExp*& regExp = m_regExpLiterals.add(static_cast<uint64_t>(source) << 32 | flags, nullptr).iterator->value;
    if (!regExp) {
        String text = constantString(data, source);
#if ENABLE(YARR_JIT)
        regExp = RegExp::tryCreateFromImage(vm, text, OptionSet<Yarr::Flags>::fromRaw(flags));
#endif
        if (!regExp)
            regExp = RegExp::create(vm, text, OptionSet<Yarr::Flags>::fromRaw(flags));
        regExp->setIsCompiledProgramLiteral();
        didCreate(regExp);
    }
    return regExp;
}

HashedString::Digest HashedString::digestOf(std::span<const char16_t> characters)
{
    SHA1 sha1;
    sha1.addBytes(std::as_bytes(characters));
    SHA1::Digest whole;
    sha1.computeHash(whole);
    HashedString::Digest digest;
    memcpySpan(std::span { digest }, std::span { whole }.first(digest.size()));
    return digest;
}

HashedString::Digest HashedString::digestOfNarrowestEncoding(StringView text)
{
    SHA1 sha1;
    if (text.is8Bit())
        sha1.addBytes(asBytes(text.span8()));
    else if (WTF::charactersAreAllLatin1(text.span16()))
        sha1.addBytes(asBytes(String::make8Bit(text.span16()).span8()));
    else
        sha1.addBytes(asBytes(text.span16()));
    SHA1::Digest whole;
    sha1.computeHash(whole);
    HashedString::Digest digest;
    memcpySpan(std::span { digest }, std::span { whole }.first(digest.size()));
    return digest;
}

HashedString HashedString::create(uint32_t string, std::span<const char16_t> characters)
{
    return { string, safeCast<uint32_t>(characters.size()), StringHasher::computeHashAndMaskTop8Bits(characters), digestOf(characters) };
}

bool HashedString::matches(StringView text) const
{
    if (text.length() != length)
        return false;
    if ((text.is8Bit() ? StringHasher::computeHashAndMaskTop8Bits(text.span8()) : StringHasher::computeHashAndMaskTop8Bits(text.span16())) != hash)
        return false;
    return digestOf(text.upconvertedCharacters().span()) == digest;
}

bool State::hasText(uint32_t string, StringView text) const
{
    if (const HashedString* hashed = data.hasOnlyDecodedForm(string) ? data.hashedString(string) : nullptr)
        return hashed->matches(text);
    if (data.hasOnlyDecodedForm(string))
        return !(data.stringFacts(string) & (Data::stringIsPrivateName | Data::stringIsEnginePrivateName)) && text == constantString(data, string);
    auto characters = byteCast<Latin1Character>(data.stringInImage(string));
    if (characters.size() < text.length() || isPrivateName(data.string(string)) || isEnginePrivateName(data.string(string)) || isConstantSymbol(data.string(string)))
        return false;
    bool isASCII = charactersAreAllASCII(characters);
    if (characters.size() == text.length())
        return isASCII && equal(text, characters);
    return !isASCII && text == textOf(data.string(string));
}

const Identifier& State::nameIfCreated(uint32_t string) const
{
    static NeverDestroyed<Identifier> none;
    if (uint32_t facts = data.stringFacts(string); facts & Data::stringHasIdentifierNumber) {
        const Identifier* made = instance.program->tryGetIdentifierConcurrently(facts & Data::stringHashMask);
        return made ? *made : none.get();
    }
    auto found = m_nameIndices.find(string);
    return found == m_nameIndices.end() ? none.get() : m_names[found->value];
}

void State::createNamesForHeapAnalysis(VM& vm)
{
    auto make = [&](uint32_t string, Store store) {
        if (holdsCells(store) && data.stringSize(string))
            name(vm, string);
    };
    for (uint32_t shape = 1; shape < data.numberOfLayouts; ++shape) {
        if (!hasLayoutForShape(shape))
            continue;
        const LayoutData& layout = layoutForShape(shape);
        if (!isObjectLayout(layout.kind)) {
            for (auto& variable : layout.describedVariables())
                make(variable.name, variable.store());
            continue;
        }
        if (!structures[shape])
            continue;
        for (uint32_t i = 0; i < layout.numberOfFields; ++i)
            make(layout.fields()[i].name, layout.fields()[i].store());
    }
}

bool State::isName(uint32_t string, UniquedStringImpl* name) const
{
    if (uint32_t facts = data.stringFacts(string); facts & Data::stringIsASCII && (facts & Data::stringHasIdentifierNumber ? data.identifierHash(facts & Data::stringHashMask) : facts & Data::stringHashMask) != name->hash())
        return false;
    if (const Identifier& known = nameIfCreated(string); !known.isNull())
        return known.impl() == name;
    return hasText(string, StringView { *name });
}

bool State::isPrivate(const FieldData& field) const
{
    return !data.stringSize(field.name) || data.stringFacts(field.name) & Data::stringIsPrivateName;
}

const FieldData* State::field(VM& vm, const LayoutData& layout, UniquedStringImpl* name)
{
    if (name->isSymbol() && !layout.hasFieldsWithSymbolKeys() && !static_cast<SymbolImpl*>(name)->isPrivate())
        return nullptr;
    CachedField& cached = m_cachedFields[(layout.shape * 0x9e3779b1u ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(name) >> 4)) % numberOfCachedFields];
    if (cached.shape == layout.shape && cached.name == name) [[likely]]
        return cached.field;
    const FieldData* found = findField(vm, layout, name);
    name->ref();
    if (cached.name)
        cached.name->deref();
    cached = { layout.shape, name, found };
    return found;
}

std::optional<uint32_t> State::fieldNameIndex(UniquedStringImpl* name) const
{
    CachedFieldName& cached = m_cachedFieldNames[(std::bit_cast<uintptr_t>(name) >> 4) % numberOfCachedFieldNames];
    if (cached.name == name) [[likely]]
        return cached.numberPlusOne ? std::optional(cached.numberPlusOne - 1) : std::nullopt;
    auto number = findFieldNameIndex(name);
    name->ref();
    if (cached.name)
        cached.name->deref();
    cached = { name, number ? *number + 1 : 0 };
    return number;
}

std::optional<uint32_t> State::findFieldNameIndex(UniquedStringImpl* name) const
{
    auto names = std::span { data.at<std::array<uint32_t, 2>>(data.fieldNamesOffset), data.numberOfFieldNames };
    if (name->isSymbol()) {
        auto found = std::ranges::lower_bound(names, UINT32_MAX, { }, [](auto& entry) {
            return entry[0];
        });
        for (; found != names.end(); ++found) {
            uint32_t string = (*found)[1];
            if (!(data.stringFacts(string) & Data::stringIsConstantSymbol))
                continue;
            bool mayExistUnnamed = data.string(string)[1] == 'r' || data.string(string)[1] == 'w';
            if ((mayExistUnnamed ? const_cast<State&>(*this).name(*instance.vm, string) : nameIfCreated(string)).impl() == name)
                return string;
        }
        return std::nullopt;
    }
    for (uint32_t hash : { name->hash(), UINT32_MAX }) {
        auto found = std::ranges::lower_bound(names, hash, { }, [](auto& entry) {
            return entry[0];
        });
        for (; found != names.end() && (*found)[0] == hash; ++found) {
            if (hasText((*found)[1], StringView { *name }))
                return (*found)[1];
        }
    }
    return std::nullopt;
}

const FieldData* State::findField(VM& vm, const LayoutData& layout, UniquedStringImpl* name)
{
    auto fields = std::span { layout.fields(), layout.numberOfFields };
    if (name->isSymbol()) {
        for (auto& field : fields) {
            if (data.stringFacts(field.name) & (Data::stringIsPrivateName | Data::stringIsEnginePrivateName | Data::stringIsConstantSymbol) && this->name(vm, field.name).impl() == name)
                return &field;
        }
        return nullptr;
    }
    auto number = fieldNameIndex(name);
    if (!number)
        return nullptr;
    if (!layout.fieldsByNameOffset()) {
        auto found = std::ranges::find(fields, *number, &FieldData::name);
        return found == fields.end() ? nullptr : &*found;
    }
    auto byName = std::span { data.at<uint16_t>(layout.fieldsByNameOffset()), layout.numberOfFields };
    auto found = std::ranges::lower_bound(byName, *number, { }, [&](uint16_t field) {
        return fields[field].name;
    });
    return found == byName.end() || fields[*found].name != *number ? nullptr : &fields[*found];
}

bool State::mayBeAbsent(const FieldData& field) const
{
    return typeOf(field).primitives & Primitive::Absent || field.flags & fieldMayBeUnlisted;
}

Object* State::compiledObject(JSValue value) const
{
    auto* object = dynamicDowncast<Object>(value);
    return object && isObjectLayout(object->layout().kind) && &object->state().data == &data ? object : nullptr;
}

bool State::passes(const RuntimeShapeTest& test, uint32_t shape) const
{
    uint32_t indexFromLast = test.first + test.count - 1 - shape;
    return indexFromLast < test.count && (test.set == UINT32_MAX || data.at<uint32_t>(data.shapeSetsOffset)[(test.set + indexFromLast) / 32] >> (test.set + indexFromLast) % 32 & 1);
}

bool State::acceptsShape(const RuntimeType& type, uint32_t shape) const
{
    for (uint32_t i = 0; i < type.numberOfSupertypes(); ++i) {
        if (passes(type.view(i), shape))
            return true;
    }
    return false;
}

bool State::subtypesHaveNoSpecializations(const RuntimeType& type) const
{
    static constexpr uint32_t maxNumberOfShapes = 1024;
    uint32_t numberOfShapes = 0;
    for (uint32_t i = 0; i < type.numberOfSupertypes(); ++i) {
        auto& test = type.view(i);
        numberOfShapes += test.count;
        if (test.count > maxNumberOfShapes || numberOfShapes > maxNumberOfShapes)
            return false;
        for (uint32_t shape = test.first; shape < test.first + test.count; ++shape) {
            if (passes(test, shape) && hasLayoutForShape(shape) && layoutForShape(shape).numberOfSpecializations())
                return false;
        }
    }
    return true;
}

void State::willModify(const FieldData& field, JSValue value)
{
    if (!field.numberOfLiterals()) {
        instance.typedModifiedLiterals[field.flags >> pinnedFieldGuardShift] = 1;
        return;
    }
    if (!isStringEqualTo(value, data.at<LiteralData>(field.literalsOffset())->string()))
        instance.typedModifiedLiterals[field.flags >> pinnedFieldGuardShift] = 1;
}

bool State::isStringEqualTo(JSValue value, uint32_t string) const
{
    return value && value.isString() && asString(value)->length() <= data.stringSize(string) && hasText(string, asString(value)->tryGetValue());
}

bool State::isAllowedLiteral(const FieldData& field, JSValue value) const
{
    return isAllowedLiteral(std::span { data.at<LiteralData>(field.literalsOffset()), field.numberOfLiterals() }, value);
}

bool State::isAllowedLiteralWithoutField(VM& vm, const LayoutData& layout, UniquedStringImpl* property, JSValue value)
{
    if (!layout.literalsWithoutFieldOffset()) [[likely]]
        return true;
    const uint32_t* words = data.at<uint32_t>(layout.literalsWithoutFieldOffset());
    for (uint32_t i = 0; i < words[0]; ++i) {
        const uint32_t* entry = words + 1 + 3 * i;
        if (name(vm, entry[0]).impl() == property)
            return isAllowedLiteral(std::span { data.at<LiteralData>(entry[2]), entry[1] }, value);
    }
    return true;
}

bool State::isAllowedLiteral(std::span<const LiteralData> literals, JSValue value) const
{
    bool isNumber = value.isNumber();
    if (literals.empty() || (!isNumber && !value.isString()))
        return true;
    bool hasLiteralsOfKind = false;
    for (auto& literal : literals) {
        if (literal.isNumber() != isNumber)
            continue;
        hasLiteralsOfKind = true;
        if (isNumber ? literal.number() == value.asNumber() : hasText(literal.string(), asString(value)->tryGetValue()))
            return true;
    }
    return !hasLiteralsOfKind;
}

bool State::fits(const RuntimeType& type, JSValue value) const
{
    if (value.isUndefined())
        return type.primitives & Primitive::Undefined;
    if (value.isNull())
        return type.primitives & Primitive::Null;
    if (value.isBoolean())
        return type.primitives & (value.asBoolean() ? Primitive::True : Primitive::False);
    if (value.isNumber()) {
        double number = value.asNumber();
        bool isInteger = number == std::trunc(number) && std::abs(number) <= static_cast<double>(maxTrackedInteger) && !(!number && std::signbit(number));
        if (isInteger)
            return type.primitives & Primitive::Int && number >= static_cast<double>(type.minInteger()) && number <= static_cast<double>(type.maxInteger());
        return type.primitives & Primitive::Double;
    }
    if (value.isBigInt())
        return type.primitives & Primitive::BigInt;
    if (!value.isCell())
        return false;
    JSCell* cell = value.asCell();
    if (cell->isString())
        return type.primitives & Primitive::String;
    if (cell->isSymbol())
        return type.primitives & Primitive::Symbol;
    if (!cell->isObject())
        return false;
    return type.acceptsUncheckedObjects() || matchesTypedSubtype(type, asObject(cell));
}

bool State::matchesTypedSubtype(const RuntimeType& type, JSObject* cell) const
{
    if (type.allowsAnyObject())
        return true;
    if (Object* object = compiledObject(cell))
        return acceptsShape(type, object->shape());
    for (uint32_t i = 0; i < type.numberOfJSTypes(); ++i) {
        if (type.jsTypes()[i] == cell->type())
            return true;
        if (type.jsTypes()[i] == ArrayType && cell->type() == DerivedArrayType)
            return true;
    }
    return type.allowsAnyFunction() && cell->isCallable();
}

static String functionName(JSObject* function)
{
    if (auto* closure = dynamicDowncast<Closure>(function))
        return closure->structure()->aotInstance()->typed->nameOfFunction(closure->functionIndex());
    if (auto* closure = dynamicDowncast<SlimClosure>(function))
        return closure->nameOfFunction();
    if (auto* jsFunction = dynamicDowncast<JSFunction>(function))
        return jsFunction->name(function->vm());
    if (auto* internalFunction = dynamicDowncast<InternalFunction>(function))
        return internalFunction->name();
    return { };
}

static String className(JSObject* object)
{
    VM& vm = object->vm();
    if (JSValue prototype = object->getPrototypeDirect(); prototype.isObject()) {
        ClassPrototype::look(asObject(prototype));
        if (JSValue constructor = asObject(prototype)->getDirect(vm, vm.propertyNames->constructor); constructor && constructor.isObject()) {
            if (String name = functionName(asObject(constructor)); !name.isEmpty())
                return name;
        }
    }
    return object->classInfo()->className;
}

static bool hasPlainPrototype(JSObject* object)
{
    JSValue prototype = object->getPrototypeDirect();
    return prototype.isNull() || prototype == JSValue(object->realm()->objectPrototype());
}

static bool isNativeObject(JSObject* object)
{
    return object->type() != FinalObjectType && !object->isCallable() && !dynamicDowncast<JSArray>(object) && !dynamicDowncast<Object>(object);
}

static thread_local bool s_describesKindsOnly;

String State::describe(JSValue value)
{
    if (!value)
        return "nothing"_s;
    if (value.isUndefined())
        return "undefined"_s;
    if (value.isNull())
        return "null"_s;
    if (value.isBoolean() && s_describesKindsOnly)
        return "boolean"_s;
    if (value.isBoolean())
        return value.asBoolean() ? "boolean true"_s : "boolean false"_s;
    if (value.isNumber() && s_describesKindsOnly)
        return "number"_s;
    if (value.isNumber())
        return makeString("number "_s, value.asNumber());
    if (value.isString()) {
        static constexpr unsigned maxLength = 40;
        StringImpl* text = s_describesKindsOnly ? nullptr : asString(value)->tryGetValueImpl();
        if (!text)
            return "string"_s;
        return makeString("string \""_s, StringView { *text }.left(maxLength), text->length() > maxLength ? "...\""_s : "\""_s);
    }
    if (value.isSymbol())
        return "symbol"_s;
    if (value.isBigInt())
        return "bigint"_s;
    if (auto* object = dynamicDowncast<Object>(value))
        return object->state().nameOf(object->layout());
    if (!value.isObject())
        return "internal cell"_s;
    JSObject* object = asObject(value);
    if (auto* closure = dynamicDowncast<Closure>(object); closure && closure->kind() == Format::FunctionKind::Constructor)
        return makeString("class "_s, functionName(closure));
    if (object->isCallable()) {
        String name = functionName(object);
        return name.isEmpty() ? "function"_str : makeString("function "_s, name);
    }
    if (dynamicDowncast<JSArray>(object))
        return "Array"_s;
    if (isNativeObject(object))
        return makeString("native "_s, className(object));
    return hasPlainPrototype(object) ? "untyped object"_str : makeString("untyped "_s, className(object));
}

ASCIILiteral State::kindName(JSValue value, const RuntimeType* expectedType)
{
    if (value.isUndefined())
        return "undefined"_s;
    if (value.isNull())
        return "null"_s;
    if (value.isBoolean())
        return "boolean"_s;
    if (value.isNumber()) {
        if (!expectedType || (expectedType->primitives & Primitive::Number) != Primitive::Int)
            return "number"_s;
        return value.isInt32AsAnyInt() ? "number, out of range"_s : "number, no int32"_s;
    }
    if (value.isString())
        return "string"_s;
    if (auto* closure = dynamicDowncast<Closure>(value); closure && closure->kind() == Format::FunctionKind::Constructor)
        return { };
    if (value.isCallable())
        return "function"_s;
    return { };
}

static void appendUnionMember(StringBuilder& result, bool has, StringView name)
{
    if (has)
        result.append(result.isEmpty() ? ""_s : " | "_s, name);
}

static uint32_t withoutImplicitNullish(uint32_t primitives, bool hasOtherMembers)
{
    bool hasBoth = (primitives & Primitive::UndefinedOrNull) == Primitive::UndefinedOrNull;
    return hasBoth && (hasOtherMembers || primitives & ~Primitive::UndefinedOrNull) ? primitives & ~Primitive::UndefinedOrNull : primitives;
}

static void appendPrimitives(StringBuilder& result, uint32_t primitives)
{
    primitives = withoutImplicitNullish(primitives, !result.isEmpty());
    appendUnionMember(result, (primitives & Primitive::Boolean) == Primitive::Boolean, "boolean"_s);
    appendUnionMember(result, (primitives & Primitive::Boolean) == Primitive::True, "true"_s);
    appendUnionMember(result, (primitives & Primitive::Boolean) == Primitive::False, "false"_s);
    appendUnionMember(result, primitives & Primitive::Double, "number"_s);
    appendUnionMember(result, (primitives & Primitive::Number) == Primitive::Int, "integer"_s);
    appendUnionMember(result, primitives & Primitive::String, "string"_s);
    appendUnionMember(result, primitives & Primitive::Symbol, "symbol"_s);
    appendUnionMember(result, primitives & Primitive::BigInt, "bigint"_s);
    appendUnionMember(result, primitives & Primitive::Undefined, "undefined"_s);
    appendUnionMember(result, primitives & Primitive::Null, "null"_s);
    appendUnionMember(result, result.isEmpty(), "never"_s);
}

String State::nameOf(const RuntimeShapeTest& view) const
{
    if (view.supertypeShape)
        return nameOf(layoutForShape(view.supertypeShape));
    if (data.stringSize(view.name))
        return text(view.name);
    auto fields = view.fields ? data.at<SupertypeFields>(view.fields)->decode(data.at<uint32_t>(data.typesOffset)) : Vector<SupertypeField>();
    return describeAnonymousType(fields.size(), [&](uint32_t i) {
        return fields[i].name;
    });
}

String State::describeAnonymousType(size_t numberOfProperties, const Invocable<uint32_t(uint32_t)> auto& nameOfProperty) const
{
    static constexpr size_t maxNamesShown = 4;
    StringBuilder result;
    result.append('{');
    for (uint32_t i = 0; i < std::min(numberOfProperties, maxNamesShown); ++i)
        result.append(i ? ", "_s : " "_s, text(nameOfProperty(i)));
    result.append(numberOfProperties > maxNamesShown ? ", ... }"_s : " }"_s);
    return result.toString();
}

String describe(const DescribedType& type, bool logsTypeErrors)
{
    static constexpr uint32_t allValues = Primitive::Boolean | Primitive::Number | Primitive::String | Primitive::Symbol | Primitive::BigInt | Primitive::UndefinedOrNull;
    if (type.allowsAnyObject && (type.primitives & allValues) == allValues)
        return "any"_s;
    StringBuilder result;
    for (auto& view : type.views)
        appendUnionMember(result, true, view);
    for (uint32_t jsType : type.jsTypes) {
        if (jsType == ArrayType && !type.elements.isNull()) {
            appendUnionMember(result, true, type.elements.contains(" | "_s) ? makeString('(', type.elements, ")[]"_s) : makeString(type.elements, "[]"_s));
            continue;
        }
        ASCIILiteral name = "object"_s;
        for (auto& engineClass : Builtins::singleton().classes()) {
            if (static_cast<uint32_t>(engineClass.type) == jsType) {
                name = engineClass.text;
                break;
            }
        }
        appendUnionMember(result, true, name);
    }
    appendUnionMember(result, type.allowsAnyObject, "object"_s);
    appendUnionMember(result, type.allowsAnyFunction && !type.allowsAnyObject, "Function"_s);
    if (logsTypeErrors && (type.primitives & allValues) == allValues && !result.isEmpty())
        return result.toString();
    if ((type.primitives & Primitive::Number) == Primitive::Int && type.minInteger >= -maxExactInteger && type.maxInteger <= maxExactInteger) {
        appendUnionMember(result, true, type.minInteger == type.maxInteger ? makeString(type.minInteger) : makeString("integer "_s, type.minInteger, ".."_s, type.maxInteger));
        appendPrimitives(result, type.primitives & ~Primitive::Number);
    } else
        appendPrimitives(result, type.primitives);
    return result.toString();
}

String State::describe(const RuntimeType& type) const
{
    DescribedType described { type.primitives, type.allowsAnyObject(), type.allowsAnyFunction(), type.minInteger(), type.maxInteger(), { }, { }, { } };
    for (uint32_t i = 0; i < type.numberOfSupertypes(); ++i)
        described.views.append(nameOf(type.view(i)));
    described.jsTypes.append(std::span { type.jsTypes(), type.numberOfJSTypes() });
    if (type.elements())
        described.elements = describe(*data.at<RuntimeType>(type.elements()));
    return Typed::describe(described, data.logsTypeErrors);
}

String State::describeKinds(uint32_t kinds, uint32_t name) const
{
    StringBuilder result;
    appendUnionMember(result, name != UINT32_MAX || kinds & typeAllowsObjects, name != UINT32_MAX ? text(name) : "object"_str);
    appendUnionMember(result, kinds & typeAllowsFunctions, "Function"_s);
    appendPrimitives(result, kinds & Primitive::All);
    return result.toString();
}

String State::nameOf(const LayoutData& layout) const
{
    if (data.stringSize(layout.name))
        return text(layout.name);
    return describeAnonymousType(layout.numberOfFields, [&](uint32_t i) {
        return layout.fields()[i].name;
    });
}

bool State::isAssignable(const RuntimeType& from, const RuntimeType& to) const
{
    if (&from == &to)
        return true;
    if (from.primitives & ~to.primitives & Primitive::All)
        return false;
    if (from.primitives & Primitive::Int && (from.minInteger() < to.minInteger() || from.maxInteger() > to.maxInteger()))
        return false;
    if (to.allowsAnyObject() || to.acceptsUntypedObjects())
        return true;
    if (from.allowsAnyObject() || from.acceptsUntypedObjects() || (from.allowsAnyFunction() && !to.allowsAnyFunction()))
        return false;
    std::span toJSTypes { to.jsTypes(), to.numberOfJSTypes() };
    for (uint32_t i = 0; i < from.numberOfJSTypes(); ++i) {
        if (std::ranges::find(toJSTypes, from.jsTypes()[i]) == toJSTypes.end())
            return false;
    }
    for (uint32_t i = 0; i < from.numberOfSupertypes(); ++i) {
        auto& view = from.view(i);
        for (uint32_t shape = view.first; shape < view.first + view.count; ++shape) {
            if (passes(view, shape) && hasLayoutForShape(shape) && !acceptsShape(to, shape))
                return false;
        }
    }
    return !from.elements() || !to.elements() || isAssignable(*data.at<RuntimeType>(from.elements()), *data.at<RuntimeType>(to.elements()));
}

static std::optional<JSValue> readDataProperty(VM& vm, JSObject* object, PropertyName name)
{
    for (JSObject* holder = object; holder;) {
        ClassPrototype::look(holder);
        auto* compiled = dynamicDowncast<Object>(holder);
        if (const FieldData* field = compiled ? compiled->state().field(vm, compiled->layout(), name.uid()) : nullptr) {
            if (JSValue value = compiled->read(*field))
                return value;
        }
        JSObject* properties = compiled ? compiled->extraProperties() : holder;
        unsigned attributes;
        if (JSValue value = properties ? properties->getDirect(vm, name, attributes) : JSValue()) {
            if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
                return std::nullopt;
            return value;
        }
        JSValue prototype = holder->getPrototypeDirect();
        holder = prototype.isObject() ? asObject(prototype) : nullptr;
    }
    return JSValue();
}

String State::explainMismatch(VM& vm, JSValue value, const RuntimeType& type, bool subtypesOnly)
{
    if (!value.isObject() || type.numberOfSupertypes() != 1)
        return { };
    SetForScope kindsOnlyScope(s_describesKindsOnly, s_describesKindsOnly || subtypesOnly);
    JSObject* object = asObject(value);
    Object* compiled = compiledObject(object);
    auto& view = type.view(0);
    if (subtypesOnly && isNativeObject(object))
        return { };
    if (isNativeObject(object))
        return makeString("a native object cannot be used as "_s, nameOf(view));
    if (!compiled && object->type() != FinalObjectType)
        return { };
    String reason;
    String differentDeclaration;
    auto sourceOrDescription = [&](uint32_t typeSource, const RuntimeType& type) {
        return typeSource == UINT32_MAX ? describe(type) : text(typeSource);
    };
    auto typeSourceOf = [&](const LayoutData& layout, size_t field) {
        return layout.typeStringsOffset() ? data.at<uint32_t>(layout.typeStringsOffset())[field] : UINT32_MAX;
    };
    auto check = [&](uint32_t nameIndex, const RuntimeType& declared, bool isOptional, uint32_t declaredTypeSource) {
        if (data.stringFacts(nameIndex) & Data::stringIsPrivateName)
            return true;
        const Identifier& name = this->name(vm, nameIndex);
        const FieldData* ownField = compiled ? field(vm, compiled->layout(), name.impl()) : nullptr;
        if (ownField && !subtypesOnly) {
            if (ownField->flags & fieldIsOptional && !isOptional) {
                reason = makeString("property '"_s, name.string(), "' is optional in "_s, describe(value), " but required in "_s, nameOf(view));
                return false;
            }
            uint32_t ownTypeSource = typeSourceOf(compiled->layout(), ownField - compiled->layout().fields());
            if (!isAssignable(ownTypeOf(*ownField), declared)) {
                reason = makeString("property '"_s, name.string(), "': "_s, sourceOrDescription(ownTypeSource, ownTypeOf(*ownField)), " is not assignable to "_s, sourceOrDescription(declaredTypeSource, declared));
                return false;
            }
            if (differentDeclaration.isNull() && (ownTypeSource != declaredTypeSource || ownTypeSource == UINT32_MAX) && isAssignable(declared, ownTypeOf(*ownField))) {
                String ownWords = sourceOrDescription(ownTypeSource, ownTypeOf(*ownField));
                String declaredWords = sourceOrDescription(declaredTypeSource, declared);
                if (ownWords != declaredWords)
                    differentDeclaration = makeString("property '"_s, name.string(), "': "_s, ownWords, " in "_s, describe(value), ", "_s, declaredWords, " in "_s, nameOf(view));
            }
            return true;
        }
        auto found = ownField ? std::optional<JSValue>(compiled->read(*ownField)) : readDataProperty(vm, object, name);
        if (!found)
            return true;
        JSValue actual = *found;
        if (!actual) {
            if (isOptional)
                return true;
            reason = makeString("property '"_s, name.string(), "' is missing in "_s, describe(value), " but required in "_s, nameOf(view));
            return false;
        }
        if (!fits(declared, actual)) {
            reason = makeString("property '"_s, name.string(), "': expected "_s, sourceOrDescription(declaredTypeSource, declared), ", got "_s, describe(actual));
            return false;
        }
        return true;
    };
    if (view.supertypeShape) {
        const LayoutData& layout = layoutForShape(view.supertypeShape);
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            const FieldData& declared = layout.fields()[i];
            if (!check(declared.name, ownTypeOf(declared), declared.flags & fieldIsOptional, typeSourceOf(layout, i)))
                break;
        }
    } else if (view.fields) {
        for (auto& declared : data.at<SupertypeFields>(view.fields)->decode(data.at<uint32_t>(data.typesOffset))) {
            if (!check(declared.name, *data.at<RuntimeType>(declared.type), declared.isOptional, declared.typeSource))
                break;
        }
    }
    if (!reason.isNull() || subtypesOnly)
        return reason;
    if (!differentDeclaration.isNull())
        return differentDeclaration;
    if (compiled)
        return makeString(describe(value), " is not assignable to "_s, nameOf(view));
    if (!hasPlainPrototype(object))
        return makeString("an instance of an untyped class cannot be used as "_s, nameOf(view));
    if (view.supertypeShape && layoutForShape(view.supertypeShape).kind == LayoutKind::Instance)
        return makeString("an untyped object cannot be used as an instance of class "_s, nameOf(view));
    return makeString("an object from untyped code is accepted as "_s, nameOf(view), " only at a type annotation or an 'as "_s, nameOf(view), "' cast"_s);
}

String State::mismatchMessage(VM& vm, JSValue value, const RuntimeType& expected, const String& knownReason)
{
    String reason = knownReason.isEmpty() ? explainMismatch(vm, value, expected) : knownReason;
    String expectedDescription = describe(expected);
    String actualDescription = describe(value);
    if (Object* object = expectedDescription == actualDescription && expected.numberOfSupertypes() == 1 ? compiledObject(value) : nullptr) {
        auto withLocation = [&](const String& name, uint32_t path, uint32_t line) {
            return path == UINT32_MAX ? name : makeString(name, " (declared at "_s, text(path), ':', line, ')');
        };
        auto& view = expected.view(0);
        const LayoutData* supertypeLayout = view.supertypeShape ? &layoutForShape(view.supertypeShape) : nullptr;
        expectedDescription = withLocation(expectedDescription, supertypeLayout ? supertypeLayout->declaredPath() : view.hasDeclaredLocation ? view.declaredPath : UINT32_MAX, supertypeLayout ? supertypeLayout->declaredLine() : view.hasDeclaredLocation ? view.declaredLine : 0);
        actualDescription = withLocation(actualDescription, object->layout().declaredPath(), object->layout().declaredLine());
    }
    return makeString("expected "_s, expectedDescription, ", got "_s, actualDescription, reason.isEmpty() ? ""_s : "\n  "_s, reason);
}

String State::nullishMessage(JSValue value, const String& property, bool isWrite)
{
    ASCIILiteral valueName = value.isNull() ? "null"_s : "undefined"_s;
    if (property.isEmpty() || property.impl()->isSymbol())
        return makeString(isWrite ? "Cannot set properties of "_s : "Cannot read properties of "_s, valueName);
    return makeString(isWrite ? "Cannot set properties of "_s : "Cannot read properties of "_s, valueName, isWrite ? " (setting '"_s : " (reading '"_s, property, "')"_s);
}

String State::nullishMessage(JSGlobalObject* globalObject, JSValue value, JSValue key, bool isWrite)
{
    String property;
    if (key.isInt32())
        property = String::number(key.asInt32());
    else if (key.isString() && !asString(key)->isRope())
        property = asString(key)->value(globalObject);
    return nullishMessage(value, property, isWrite);
}

String State::nonNullMessage(JSValue value, const String& operandSource)
{
    if (operandSource.isEmpty())
        return makeString("non-null assertion failed: value is "_s, describe(value));
    return makeString("non-null assertion failed: `"_s, operandSource, "` is "_s, describe(value));
}

String State::assignmentMessage(VM& vm, const String& target, const FieldData& field, JSValue value, const String& reason)
{
    if (field.flags & fieldIsImmutable)
        return makeString("Cannot assign to read only property "_s, target);
    return makeString("Cannot assign to "_s, target, ": "_s, mismatchMessage(vm, value, ownTypeOf(field), reason));
}

String State::nameOfFunction(uint32_t function) const
{
    return text(data.at<uint32_t>(data.functionsOffset)[function]);
}

String State::text(uint32_t string) const
{
    return constantString(data, string);
}

String State::describeSite(AuditSite site) const
{
    RELEASE_ASSERT(data.isCompiledForAudit);
    if (site.function >= data.numberOfFunctions)
        return "an element read"_s;
    String name = nameOfFunction(site.function);
    return makeString(name.isEmpty() ? "<anonymous>"_str : name, " ("_s, text(data.at<uint32_t>(data.functionSourcesOffset)[site.function]), ':', site.lineAndColumn >> AuditSite::columnBits, ':', site.lineAndColumn & AuditSite::maxColumn, ')');
}

const LayoutData* State::classWithIndex(uint32_t index) const
{
    uint32_t offset = data.offsetOfClass(index);
    return offset ? data.at<LayoutData>(offset) : nullptr;
}

const LayoutData* State::classConstructedBy(uint32_t function) const
{
    auto& recentLookup = recentClassLookups[function % recentClassLookups.size()];
    uint64_t known = recentLookup.load(std::memory_order_relaxed);
    if (known >> 32 == static_cast<uint64_t>(function) + 1) [[likely]]
        return static_cast<uint32_t>(known) ? data.at<LayoutData>(static_cast<uint32_t>(known)) : nullptr;
    const LayoutData* found = Typed::classConstructedBy(data, function);
    uint64_t offset = found ? reinterpret_cast<const uint8_t*>(found) - data.at<uint8_t>(0) : 0;
    if (offset <= UINT32_MAX)
        recentLookup.store((static_cast<uint64_t>(function) + 1) << 32 | offset, std::memory_order_relaxed);
    return found;
}

static uint8_t* addressOfVariable(const Instance& instance, const Closure* classObject, const LayoutData& ofClass, const FieldData& variable)
{
    if (ofClass.classData().environment == LayoutData::noBase)
        return instance.typedStatics + variable.offset;
    return reinterpret_cast<uint8_t*>(classObject->environment()) + variable.offset;
}

JSValue State::readVariable(const Closure* classObject, const LayoutData& ofClass, const FieldData& variable) const
{
    return readAs(variable.store(), addressOfVariable(instance, classObject, ofClass, variable), const_cast<State*>(this));
}

const FieldData* State::staticField(const LayoutData& ofClass, UniquedStringImpl* name) const
{
    if (name->isSymbol())
        return nullptr;
    for (const LayoutData* declaring = &ofClass; declaring; declaring = baseOf(*declaring)) {
        for (auto& field : declaring->staticFields()) {
            if (isName(field.name, name))
                return &field;
        }
    }
    return nullptr;
}

bool State::isImmutableIn(std::span<const MethodData> methods, UniquedStringImpl* name, bool& isDeclared) const
{
    for (auto& method : methods) {
        if (method.flags & methodHasComputedName || !isName(method.name, name))
            continue;
        isDeclared = true;
        if (method.flags & methodMayBeReplaced)
            return false;
    }
    return isDeclared;
}

void Closure::setComputedName(VM& vm, const Identifier& key, Format::MethodKind kind)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = functionComputedName(globalObject(), key, kind);
    RETURN_IF_EXCEPTION(scope, void());
    putDirect(vm, vm.propertyNames->name, jsString(vm, name), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
    ensureRareData(vm)->setHasModifiedNameForBoundOrNonHostFunction();
}

struct State::WatchedMembers {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(WatchedMembers);

    class Member final : public AdaptiveInferredPropertyValueWatchpointBase {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Member);
    public:
        Member(const ObjectPropertyCondition& condition, WatchedMembers& owner)
            : AdaptiveInferredPropertyValueWatchpointBase(condition)
            , m_owner(owner)
        {
        }

    private:
        void handleFire(VM&, const FireDetail&) final { m_owner.state.didModifyMembers(m_owner); }

        WatchedMembers& m_owner;
    };

    WatchedMembers(State& state, JSObject* holder, const LayoutData& ofClass)
        : state(state)
        , holder(holder)
        , parent(holder->getPrototypeDirect())
        , ofClass(ofClass)
    {
    }

    State& state;
    JSObject* holder;
    JSValue parent;
    const LayoutData& ofClass;
    bool isModified { false };
    TransitionWatchpoint transition;
    Vector<std::unique_ptr<Member>> members;
};

void State::updateStaticFieldLocations(VM& vm, WatchedMembers& watched)
{
    uint32_t first = watched.ofClass.classData().firstStaticFieldLocation;
    if (first == LayoutData::noBase)
        return;
    for (auto& member : watched.members)
        member->key().isWatchable(PropertyCondition::EnsureWatchability);
    int32_t* place = reinterpret_cast<int32_t*>(instance.typedStatics + data.staticFieldSlotsOffset) + first;
    bool isWatchedAlready = !!*reinterpret_cast<JSObject**>(place);
    *reinterpret_cast<JSObject**>(place) = watched.holder;
    place += 2;
    Structure* structure = watched.holder->structure();
    UniquedStringImpl* added = isWatchedAlready && !watched.isModified && structure->transitionKind() == TransitionKind::PropertyAddition ? structure->transitionPropertyName() : nullptr;
    const FieldData* readyAndSkipped = nullptr;
    bool didAnnounce = false;
    for (auto& field : watched.ofClass.staticFields()) {
        if (added && name(vm, field.name).impl() != added) {
            if (*place)
                readyAndSkipped = &field;
            ++place;
            continue;
        }
        *place = 0;
        unsigned attributes;
        PropertyOffset offset = watched.isModified ? invalidOffset : structure->get(vm, name(vm, field.name), attributes);
        if (isValidOffset(offset) && isOutOfLineOffset(offset) && !attributes && fits(ownTypeOf(field), watched.holder->getDirect(offset))) {
            structure->didCachePropertyReplacement(vm, offset);
            didAnnounce = true;
            *place = static_cast<int32_t>(reinterpret_cast<uint8_t*>(watched.holder->locationForOffset(offset)) - reinterpret_cast<uint8_t*>(watched.holder->butterfly()));
        }
        ++place;
    }
    if (readyAndSkipped && !didAnnounce)
        structure->didCachePropertyReplacement(vm, structure->get(vm, name(vm, readyAndSkipped->name)));
}

void State::didModifyMembers(WatchedMembers& watched)
{
    if (std::exchange(watched.isModified, true))
        return;
    updateStaticFieldLocations(*instance.vm, watched);
    for (uint32_t index = 0; index < data.numberOfLayoutIndices; ++index) {
        for (const LayoutData* next = classWithIndex(index); next; next = baseOf(*next)) {
            if (next->index == watched.ofClass.index)
                instance.typedModifiedClassObjects[index] = 1;
        }
    }
}

void State::didTransition(VM& vm, WatchedMembers& watched)
{
    if (watched.isModified)
        return;
    bool findsDeclaredMembers = watched.holder->getPrototypeDirect() == watched.parent && !watched.holder->getDirect(vm, vm.propertyNames->hasInstanceSymbol);
    for (const LayoutData* next = baseOf(watched.ofClass); next && findsDeclaredMembers; next = baseOf(*next)) {
        for (auto& method : next->staticMethods()) {
            if (!(method.flags & methodHasComputedName) && !(data.stringFacts(method.name) & Data::stringIsPrivateName) && watched.holder->getDirect(vm, name(vm, method.name)))
                findsDeclaredMembers = false;
        }
    }
    auto again = [](VM& vm, void* context) {
        auto& watched = *static_cast<WatchedMembers*>(context);
        watched.state.didTransition(vm, watched);
    };
    if (!findsDeclaredMembers || !watched.transition.install(watched.holder, again, &watched))
        didModifyMembers(watched);
    else
        updateStaticFieldLocations(vm, watched);
}

void State::watchMembers(VM& vm, JSObject* holder, const LayoutData& ofClass)
{
    m_watchedMembers.append(makeUnique<WatchedMembers>(*this, holder, ofClass));
    WatchedMembers& watched = *m_watchedMembers.last();
    for (auto& method : ofClass.staticMethods()) {
        if (method.flags & (methodHasComputedName | methodMayBeReplaced) || data.stringFacts(method.name) & Data::stringIsPrivateName)
            continue;
        ObjectPropertyCondition condition = generateConditionForSelfEquivalence(vm, nullptr, holder, name(vm, method.name).impl());
        if (!condition || !condition.isWatchable(PropertyCondition::EnsureWatchability))
            return didModifyMembers(watched);
        watched.members.append(makeUnique<WatchedMembers::Member>(condition, watched));
        watched.members.last()->install(vm);
    }
    didTransition(vm, watched);
}

void State::createClass(JSGlobalObject* globalObject, Closure* classObject, JSValue base, ClassPart part, JSValue basePrototypeIfKnown)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const LayoutData& ofClass = *classObject->classData();
    if (part == ClassPart::Everything && ofClass.isEvaluatedOnce && (classObject->getDirect(vm, vm.propertyNames->prototype) || (!createsPrototypeBeforeClassObject(ofClass) && classPrototypes.get(ofClass.index))))
        return;
    if (!classObject->isDeclaredAs(functionIsInBuiltinModule))
        ensureProgramStaticsInitialized();
    bool createsClassObject = part != ClassPart::PrototypeMembers;
    bool isAtDeclaration = part == ClassPart::Everything;
    bool wasDeclared = part == ClassPart::ClassObject && !createsPrototypeBeforeClassObject(ofClass);
    RELEASE_ASSERT(part == ClassPart::Everything || createsMembersLazily(ofClass));
    RELEASE_ASSERT(!createsClassObject || !classObject->getDirect(vm, vm.propertyNames->prototype));
    if (ofClass.classData().environment == LayoutData::noBase)
        RELEASE_ASSERT(classObject->captures().empty());
    else
        RELEASE_ASSERT(classObject->captures().size() == 1 && classObject->environment() && uncheckedDowncast<Object>(classObject->environment())->layout().index == ofClass.classData().environment);

    JSValue prototypeParent = globalObject->objectPrototype();
    JSObject* classObjectParent = nullptr;
    if (!createsClassObject)
        prototypeParent = classPrototypes.get(ofClass.index)->getPrototypeDirect();
    else if (wasDeclared) {
        classObjectParent = classObject->getPrototypeDirect().getObject();
        prototypeParent = classPrototypes.get(ofClass.index)->getPrototypeDirect();
    } else if (const LayoutData* baseClass = baseOf(ofClass)) {
        auto* baseClassObject = baseClass->isEvaluatedOnce ? closure(vm, baseClass->classData().classConstructor) : dynamicDowncast<Closure>(base);
        RELEASE_ASSERT(baseClassObject && baseClassObject->classData() == baseClass);
        classObjectParent = baseClassObject;
        prototypeParent = baseClassObject->prototypeProperty(vm);
        RELEASE_ASSERT(prototypeParent);
    } else if (ofClass.hasUntypedBase && base.isNull())
        prototypeParent = jsNull();
    else if (ofClass.hasUntypedBase) {
        if (!base.isConstructor()) {
            throwTypeError(globalObject, scope, "The superclass is not a constructor."_s);
            return;
        }
        classObjectParent = asObject(base);
        prototypeParent = basePrototypeIfKnown ? basePrototypeIfKnown : classObjectParent->get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, void());
        if (!prototypeParent.isObject() && !prototypeParent.isNull()) {
            throwTypeError(globalObject, scope, "The value of the superclass's prototype property is not an object or null."_s);
            return;
        }
    }

    bool methodsStayReplaceable = ofClass.hasFixedLayout && (ofClass.constructorMayBeReplaced || std::ranges::any_of(ofClass.methods(), [](auto& method) {
        return method.flags & methodMayBeReplaced;
    }));
    if (prototypeParent.isObject())
        asObject(prototypeParent)->didBecomePrototype(vm);
    if (part == ClassPart::Everything && createsMembersLazily(ofClass) && prototypeParent.isObject())
        part = ClassPart::ClassObject;
    bool createsPrototypeMembers = part != ClassPart::ClassObject;
    bool mayShareStructures = part == ClassPart::Everything && !ofClass.isEvaluatedOnce && !ofClass.hasFixedLayout && !classObject->mayBePrototype();
    JSCell* prototypeParentOrNull = prototypeParent.isCell() ? prototypeParent.asCell() : nullptr;
    Instance::ClassStructures shared;
    if (mayShareStructures)
        shared = instance.classStructures(ofClass.index, classObjectParent, prototypeParentOrNull);
    if (shared.classObjectStructure && (shared.classObjectStructure->realm() != globalObject || shared.classObjectStructure->storedPrototype() != (classObjectParent ? JSValue(classObjectParent) : classObject->getPrototypeDirect())))
        shared = { };
    bool isShared = shared.classObjectStructure;
    bool hasOnlyPlainMembers = true;
    auto storageFor = [&](Structure* structure) {
        return structure->outOfLineCapacity() ? Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0) : nullptr;
    };
    auto storeShared = [&](JSObject* target, UniquedStringImpl* key, JSValue value) {
        PropertyOffset offset = target->structure()->get(vm, key);
        RELEASE_ASSERT(isValidOffset(offset));
        target->putDirectOffset(vm, offset, value);
    };
    JSObject* prototype;
    if (isShared)
        prototype = JSFinalObject::createWithButterfly(vm, shared.ofPrototype, storageFor(shared.ofPrototype));
    else if (part == ClassPart::Everything) {
        prototype = constructEmptyObject(vm, JSFinalObject::createStructure(vm, globalObject, prototypeParent, JSFinalObject::defaultInlineCapacity));
        if (ofClass.isEvaluatedOnce) {
            RELEASE_ASSERT(!classPrototypes.get(ofClass.index));
            classPrototypes.set(ofClass.index, prototype);
            didCreate(prototype);
        }
    } else {
        prototype = classPrototypes.get(ofClass.index);
        if (!prototype)
            prototype = createClassPrototype(vm, ofClass.index, asObject(prototypeParent));
        prototypeParent = prototype->getPrototypeDirect();
    }
    if (isAtDeclaration && part == ClassPart::ClassObject && classObjectParent && createsClassObjectOnMutation(ofClass) && !classObject->mayBePrototype()) {
        RELEASE_ASSERT(!classObject->butterfly());
        classObject->setStructure(vm, lazyClassObjectStructureInheritingFrom(vm, classObjectParent));
        if (!canDeferClassObject(classObjectParent, ofClass))
            classObject->ensureClassObject(vm);
        return;
    }
    using EarlyStores = Vector<std::pair<RefPtr<UniquedStringImpl>, unsigned>, 2>;
    EarlyStores earlyStores;
    MarkedArgumentBuffer earlyStoredValues;
    EarlyStores earlyPrototypeStores;
    MarkedArgumentBuffer earlyPrototypeStoredValues;
    auto take = [&](JSObject* from, EarlyStores& stored, MarkedArgumentBuffer& values) {
        from->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            stored.append({ entry.key(), entry.attributes() });
            values.append(from->getDirect(entry.offset()));
            return true;
        });
    };
    bool classObjectMayBePrototype = false;
    auto markAsPrototypes = makeScopeExit([&] {
        if (classObjectMayBePrototype)
            classObject->structure()->setMayBePrototype(true);
        if (part == ClassPart::PrototypeMembers)
            prototype->structure()->setMayBePrototype(true);
    });
    if (isShared) {
        RELEASE_ASSERT(!classObject->butterfly());
        if (Butterfly* storage = storageFor(shared.classObjectStructure))
            classObject->nukeStructureAndSetButterfly(vm, classObject->structureID(), storage);
        classObject->setStructure(vm, shared.classObjectStructure);
        storeShared(classObject, vm.propertyNames->prototype.impl(), prototype);
        storeShared(prototype, vm.propertyNames->constructor.impl(), classObject);
    } else if (createsClassObject) {
        RELEASE_ASSERT(!classObjectParent || !createsClassObjectOnMutation(ofClass) || classObject->getPrototypeDirect() == classObjectParent);
        Structure* ofThisClassObject = Closure::createStructure(vm, globalObject, classObjectParent ? classObjectParent : classObject->getPrototypeDirect().getObject());
        ofThisClassObject->setAOTInstance(&instance);
        classObjectMayBePrototype = classObject->mayBePrototype();
        if (classObject->butterfly()) {
            take(classObject, earlyStores, earlyStoredValues);
            classObject->nukeStructureAndSetButterfly(vm, classObject->structureID(), nullptr);
        }
        classObject->setStructure(vm, ofThisClassObject);
        classObject->putDirectWithoutTransition(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
    }
    if (part == ClassPart::PrototypeMembers) {
        Structure* ordinary = JSFinalObject::createStructure(vm, globalObject, prototypeParent, ClassPrototype::inlineCapacity);
        if (prototype->butterfly())
            take(prototype, earlyPrototypeStores, earlyPrototypeStoredValues);
        uncheckedDowncast<ClassPrototype>(prototype)->convertToOrdinaryObject(vm, ordinary);
    }
    if (createsPrototypeMembers && !isShared)
        prototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, classObject, static_cast<unsigned>(PropertyAttribute::DontEnum));

    auto isPrivate = [&](const MethodData& method) {
        return !(method.flags & methodHasComputedName) && data.stringFacts(method.name) & Data::stringIsPrivateName;
    };
    auto keyFor = [&](const MethodData& method) {
        if (!(method.flags & methodHasComputedName))
            return name(vm, method.name);
        RELEASE_ASSERT(method.name < ofClass.classData().numberOfVariables);
        JSValue key = readVariable(classObject, ofClass, ofClass.variables()[method.name]);
        RELEASE_ASSERT(key && (key.isString() || key.isSymbol()));
        Identifier identifier = key.toPropertyKey(globalObject);
        scope.assertNoException();
        return identifier;
    };
    auto define = [&](JSObject* target, const Identifier& key, const PropertyDescriptor& descriptor) {
        JSObject::defineOwnProperty(target, globalObject, key, descriptor, true);
        scope.assertNoException();
    };
    auto defineAll = [&](JSObject* target, std::span<const MethodData> methods) {
        for (auto& method : methods) {
            bool isStoredPerObject = isPrivate(method) && target == prototype && ofClass.hasFixedLayout && !ofClass.isEvaluatedOnce;
            if (isPrivate(method) && !isStoredPerObject)
                continue;
            Identifier key = keyFor(method);
            auto kind = static_cast<Format::MethodKind>(method.kind);
            bool takesEnvironment = method.flags & methodTakesEnvironment;
            JSObject* function;
            if (ofClass.isEvaluatedOnce)
                function = closureObjectFor(vm, method.function);
            else if (usesSlimClosures(method.function))
                function = SlimClosure::create(vm, slimClosureStructure(vm, method.function), method.function, classObject->captures().first(takesEnvironment), takesEnvironment);
            else
                function = Closure::create(vm, nonClassClosureStructure(vm, method.function), method.function, classObject->captures().first(takesEnvironment), takesEnvironment);
            if (method.flags & methodHasComputedName) {
                if (auto* slim = dynamicDowncast<SlimClosure>(function))
                    slim->setComputedName(vm, key, kind);
                else
                    uncheckedDowncast<Closure>(function)->setComputedName(vm, key, kind);
                if (scope.exception()) [[unlikely]]
                    return;
                hasOnlyPlainMembers = false;
            }
            if (isShared) {
                bool isGetter = kind == Format::MethodKind::Getter;
                storeShared(target, key.impl(), kind == Format::MethodKind::Method ? static_cast<JSCell*>(function) : GetterSetter::create(vm, globalObject, isGetter ? function : nullptr, isGetter ? nullptr : function));
                continue;
            }
            bool mayBeCreatedByFunction = target == classObject && (key == vm.propertyNames->name || key == vm.propertyNames->length || key == vm.propertyNames->prototype);
            if (!mayBeCreatedByFunction && !parseIndex(key) && !isValidOffset(target->structure()->get(vm, key))) {
                if (kind == Format::MethodKind::Method)
                    target->putDirectWithoutTransition(vm, key, function, PropertyAttribute::DontEnum | (isStoredPerObject ? PropertyAttribute::ReadOnly : PropertyAttribute::None));
                else {
                    bool isGetter = kind == Format::MethodKind::Getter;
                    target->putDirectNonIndexAccessorWithoutTransition(vm, key, GetterSetter::create(vm, globalObject, isGetter ? function : nullptr, isGetter ? nullptr : function), PropertyAttribute::Accessor | PropertyAttribute::DontEnum);
                }
                continue;
            }
            hasOnlyPlainMembers = false;
            PropertyDescriptor descriptor;
            switch (kind) {
            case Format::MethodKind::Method:
                descriptor.setValue(function);
                descriptor.setWritable(!isStoredPerObject);
                break;
            case Format::MethodKind::Getter:
                descriptor.setGetter(function);
                break;
            case Format::MethodKind::Setter:
                descriptor.setSetter(function);
                break;
            }
            descriptor.setEnumerable(false);
            descriptor.setConfigurable(true);
            define(target, key, descriptor);
            if (target == classObject && key == vm.propertyNames->length)
                classObject->ensureRareData(vm)->setHasModifiedLengthForBoundOrNonHostFunction();
            if (target == classObject && key == vm.propertyNames->name)
                classObject->ensureRareData(vm)->setHasModifiedNameForBoundOrNonHostFunction();
        }
    };
    if (createsPrototypeMembers)
        defineAll(prototype, ofClass.methods());
    RETURN_IF_EXCEPTION(scope, void());
    if (createsClassObject)
        defineAll(classObject, ofClass.staticMethods());
    RETURN_IF_EXCEPTION(scope, void());

    auto fix = [&](JSObject* target, const Identifier& key, bool isAccessor) {
        Structure* structure = target->structure();
        unsigned attributes = 0;
        if (structure->isUncacheableDictionary() && !parseIndex(key) && isValidOffset(structure->get(vm, key, attributes)) && !(attributes & PropertyAttribute::CustomAccessorOrValue) && isAccessor == !!(attributes & PropertyAttribute::Accessor)) {
            attributes |= PropertyAttribute::DontDelete;
            if (!isAccessor)
                attributes |= PropertyAttribute::ReadOnly;
            Structure* same = Structure::attributeChangeTransition(vm, structure, key, attributes, nullptr);
            RELEASE_ASSERT(same == structure);
            return;
        }
        PropertyDescriptor descriptor;
        descriptor.setConfigurable(false);
        if (!isAccessor)
            descriptor.setWritable(false);
        define(target, key, descriptor);
    };
    auto lockAll = [&](JSObject* target, std::span<const MethodData> methods) {
        for (auto& method : methods) {
            if (!isPrivate(method) && !(method.flags & methodMayBeReplaced))
                fix(target, keyFor(method), static_cast<Format::MethodKind>(method.kind) != Format::MethodKind::Method);
        }
    };
    auto restoreEarlyStores = [&](JSObject* target, EarlyStores& stored, MarkedArgumentBuffer& values) {
        for (size_t i = 0; i < stored.size(); ++i) {
            auto& [key, attributes] = stored[i];
            if (attributes & PropertyAttribute::Accessor)
                target->putDirectNonIndexAccessor(vm, key.get(), uncheckedDowncast<GetterSetter>(values.at(i)), attributes);
            else if (attributes & PropertyAttribute::CustomAccessorOrValue)
                target->putDirectCustomAccessor(vm, key.get(), values.at(i), attributes);
            else
                target->putDirect(vm, key.get(), values.at(i), attributes);
        }
    };
    restoreEarlyStores(classObject, earlyStores, earlyStoredValues);
    restoreEarlyStores(prototype, earlyPrototypeStores, earlyPrototypeStoredValues);
    if (createsClassObject && ofClass.hasImmutableMembers && ofClass.isEvaluatedOnce)
        watchMembers(vm, classObject, ofClass);
    if (mayShareStructures && !isShared && hasOnlyPlainMembers)
        instance.cacheClassStructures(ofClass.index, classObjectParent, prototypeParentOrNull, { classObject->structure(), prototype->structure() });
    if (!ofClass.hasFixedLayout || !createsPrototypeMembers)
        return;
    if (!methodsStayReplaceable) {
        bool isImmutable = prototype->lockAndMakePropertiesImmutable(vm);
        RELEASE_ASSERT(isImmutable);
        return;
    }
    prototype->convertToUncacheableDictionary(vm);
    lockAll(prototype, ofClass.methods());
    if (!ofClass.constructorMayBeReplaced)
        fix(prototype, vm.propertyNames->constructor, false);
    JSObject::preventExtensions(prototype, globalObject);
    scope.assertNoException();
    prototype->flattenDictionaryObject(vm);
    prototype->structure()->setInheritorsMayOverrideReadOnlyProperties(true);
}

bool State::createsMembersLazily(const LayoutData& ofClass) const
{
    return ofClass.createsMembersLazily;
}

bool State::createsPrototypeBeforeClassObject(const LayoutData& ofClass) const
{
    return ofClass.createsPrototypeBeforeClassObject;
}

bool State::createsClassObjectOnMutation(const LayoutData& ofClass)
{
    return ofClass.createsClassObjectOnMutation;
}

Structure* State::lazyClassObjectStructure(VM& vm, const LayoutData& ofClass)
{
    const LayoutData* baseClass = baseOf(ofClass);
    if (!baseClass)
        return closureStructure(vm, ofClass.classData().classConstructor);
    return lazyClassObjectStructureInheritingFrom(vm, closure(vm, baseClass->classData().classConstructor));
}

Structure* State::lazyClassObjectStructureInheritingFrom(VM& vm, JSObject* parent)
{
    Structure* structure = lazyClassObjectStructures.get(parent);
    if (!structure) {
        structure = Closure::createStructure(vm, instance.globalObject, parent);
        structure->setAOTInstance(&instance);
        lazyClassObjectStructures.set(parent, structure);
        didCreate(structure);
    }
    return structure;
}

bool State::hasStaticMethodNamed(const LayoutData& ofClass, UniquedStringImpl* name) const
{
    if (name->isSymbol())
        return false;
    return std::ranges::any_of(ofClass.staticMethods(), [&](auto& method) {
        return isName(method.name, name);
    });
}

bool State::canDeferClassObject(JSObject* parent, const LayoutData& ofClass)
{
    if (!ofClass.classData().numberOfStaticMethods)
        return true;
    LazySubclasses& descendants = lazySubclasses.add(parent, LazySubclasses { }).iterator->value;
    for (auto& name : descendants.assumedAbsent) {
        if (hasStaticMethodNamed(ofClass, name.get()))
            return false;
    }
    descendants.withStaticMembers.append(ofClass.index);
    return true;
}

bool State::isLazySubclassStructureOf(VM& vm, Structure* structure, JSObject* parent)
{
    return structure->storedPrototypeObject() == parent && (structure == lazyClassObjectStructures.get(parent) || structure == closureStructures(vm)[classObjectClosureStructure]);
}

bool State::tryAssumeAbsentFromLazySubclasses(VM& vm, JSObject* parent, UniquedStringImpl* name)
{
    Vector<Closure*, 4> declaring;
    {
        LazySubclasses& descendants = lazySubclasses.add(parent, LazySubclasses { }).iterator->value;
        if (descendants.assumedAbsent.contains(name))
            return true;
        for (uint32_t index : descendants.withStaticMembers) {
            const LayoutData* ofClass = classWithIndex(index);
            if (!hasStaticMethodNamed(*ofClass, name))
                continue;
            JSCell* classObject = closureIfCreated(ofClass->classData().classConstructor);
            if (!classObject)
                return false;
            if (uncheckedDowncast<Closure>(classObject)->isLazyClassObject())
                declaring.append(uncheckedDowncast<Closure>(classObject));
        }
        descendants.assumedAbsent.append(name);
    }
    for (Closure* heir : declaring) {
        if (Options::useAOTOperationCounters()) [[unlikely]]
            runtimeTable(vm).countOperation("ClassObject", "made-because-a-sibling-is-cached");
        heir->ensureClassObject(vm);
    }
    return true;
}

JSObject* State::createClassPrototype(VM& vm, uint32_t classIndex, JSObject* parent)
{
    Structure* structure = classPrototypeStructures.get(parent);
    if (!structure) {
        structure = ClassPrototype::createStructure(vm, instance.globalObject, parent);
        structure->setAOTInstance(&instance);
        classPrototypeStructures.set(parent, structure);
        didCreate(structure);
    }
    JSObject* prototype = ClassPrototype::create(vm, structure, classIndex);
    classPrototypes.set(classIndex, prototype);
    didCreate(prototype);
    return prototype;
}

JSObject* State::classPrototype(VM& vm, uint32_t classIndex)
{
    if (JSObject* prototype = classPrototypes.get(classIndex))
        return prototype;
    const LayoutData& ofClass = *classWithIndex(classIndex);
    if (createsPrototypeBeforeClassObject(ofClass)) {
        const LayoutData* baseClass = baseOf(ofClass);
        return createClassPrototype(vm, classIndex, baseClass ? classPrototype(vm, baseClass->index) : instance.globalObject->objectPrototype());
    }
    closure(vm, ofClass.classData().classConstructor);
    JSObject* prototype = classPrototypes.get(classIndex);
    RELEASE_ASSERT(prototype);
    return prototype;
}

Object* State::createClassInstance(VM& vm, const LayoutData& ofClass, JSObject* prototype)
{
    RELEASE_ASSERT(ofClass.hasFixedLayout && ofClass.shape);
    std::optional<uint32_t> orderWord = orderTableFor(ofClass).initialInstanceOrderWord;
    if (!orderWord) [[unlikely]] {
        Vector<uint16_t, 16> fields;
        for (uint32_t i = 0; i < ofClass.numberOfFields; ++i) {
            if (!(ofClass.fields()[i].flags & fieldIsAbsentAtCreation))
                fields.append(static_cast<uint16_t>(i));
        }
        orderWord = orderStartingWith(ofClass, fields.span());
        orderTableFor(ofClass).initialInstanceOrderWord = orderWord;
    }
    Object* object = Object::createWithInitializedFields(vm, structureFor(vm, ofClass.shape, prototype), ofClass, sizeWithSpecializationCapacity(ofClass.shape), *orderWord);
    object->updateNeedsSlowLookup(*this, true);
    countObject(object);
    return object;
}

void State::forgetInitialObjectStates()
{
    instance.forgetTypedThisAllocations();
    m_initialObjectStatesByNewTarget.fill({ });
}

void State::shareWithStub(VM& vm, const InitialObjectState& known)
{
    static_assert(std::tuple_size_v<decltype(m_initialObjectStatesByNewTarget)> == Instance::numberOfTypedThisAllocations);
    auto& shared = instance.typedThisAllocations.get()[(std::bit_cast<uintptr_t>(known.newTarget) >> 4) % Instance::numberOfTypedThisAllocations];
    shared = { };
    size_t bodySize = known.body.size();
    if (!bodySize || bodySize % sizeof(uint64_t) || known.constructor->isDeclaredAs(functionReadsNewTarget))
        return;
    LocalAllocator* allocator = vm.cellSpace().allocatorFor(bodySize + sizeof(JSCell), AllocatorForMode::EnsureAllocator).localAllocator();
    if (!allocator)
        return;
    shared.constructor = known.constructor;
    shared.allocator = allocator;
    shared.body = known.body.span().data();
    shared.header = static_cast<uint64_t>(known.structure->typeInfoBlob()) << 32 | known.structure->id().bits();
    shared.epoch = known.epoch;
    shared.bodySize = bodySize;
    shared.newTarget = known.newTarget;
    instance.hasTypedThisAllocations = true;
}

JSValue State::createThis(JSGlobalObject* globalObject, Closure* constructor, JSObject* newTarget)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (HeapVersion version = vm.heap.objectSpace().newlyAllocatedVersion(); version != m_initialObjectStatesVersion) [[unlikely]] {
        forgetInitialObjectStates();
        m_initialObjectStatesVersion = version;
    }
    InitialObjectState& known = m_initialObjectStatesByNewTarget[(std::bit_cast<uintptr_t>(newTarget) >> 4) % m_initialObjectStatesByNewTarget.size()];
    if (known.newTarget == newTarget && known.constructor == constructor && known.epoch == instance.negativeOffsetFields().typedPrototypeEpoch) {
        shareWithStub(vm, known);
        return Object::createWithBody(vm, known.structure, known.body.span());
    }
    const LayoutData* ofClass = constructor->classData();
    if (ofClass && ofClass->hasUntypedBase)
        return jsUndefined();
    const LayoutData* made = ofClass;
    JSValue prototype;
    bool constructorAcceptsAnyObject = ofClass && ofClass->hasFixedLayout && ofClass->mayDespecialize;
    bool isCreatedWithoutLayout = false;
    if (auto* target = dynamicDowncast<Closure>(newTarget); ofClass && target && target->structure()->aotInstance() == &instance) {
        const LayoutData* ofTarget = target->classData();
        for (const LayoutData* next = ofTarget; next; next = baseOf(*next)) {
            if (next != ofClass)
                continue;
            made = ofTarget;
            isCreatedWithoutLayout = ofTarget != ofClass && constructorAcceptsAnyObject;
            prototype = target->prototypeProperty(vm);
            break;
        }
    }
    if (!prototype) {
        prototype = newTarget->get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, { });
        isCreatedWithoutLayout = constructorAcceptsAnyObject && prototype != constructor->prototypeProperty(vm);
        if (ofClass && ofClass->hasFixedLayout && !isCreatedWithoutLayout && !ofClass->mayHaveOtherPrototype && prototype != constructor->prototypeProperty(vm) && !reportOrThrow(scope, globalObject, { .kind = "construct-new-target"_s, .on = ofClass }, makeString("Cannot construct "_s, nameOf(*ofClass), " with a different new.target: only a subclass in TypeScript code of the program can extend it"_s), InAudit::Performed, "instanceof answers by the class the object was declared as"_s))
            return { };
    }
    JSObject* parent;
    if (prototype.isObject())
        parent = asObject(prototype);
    else {
        JSGlobalObject* realm = getFunctionRealm(globalObject, newTarget);
        RETURN_IF_EXCEPTION(scope, { });
        parent = realm->objectPrototype();
    }
    if (!ofClass) {
        if (const AllocationData* allocation = thisAllocation(constructor->functionIndex())) {
            if (parent != instance.globalObject->objectPrototype() && !parent->inherits<PrototypeObject>() && (!layoutForShape(allocation->shape).mayHaveOtherPrototype || createsObjectsWithDefinitelyInitializedFields(constructor->functionIndex()))) [[unlikely]] {
                bool goesOn = reportOrThrow(globalObject, { .kind = "construct-new-target"_s, .onFunction = constructor->functionIndex() }, makeString("Cannot construct '"_s, nameOfFunction(constructor->functionIndex()), "' with a new.target whose 'prototype' is not that of a function in TypeScript code of the program"_s), InAudit::Performed);
                RETURN_IF_EXCEPTION(scope, { });
                ASSERT_UNUSED(goesOn, goesOn);
            }
            Object* object = Object::create(vm, structureFor(vm, allocation->shape, parent), layoutForShape(allocation->shape), allocation->size, allocation->orderWord);
            object->updateNeedsSlowLookup(*this, true);
            countAllocation(static_cast<uint32_t>(allocation - &this->allocation(0)), object);
            return object;
        }
    }
    if (!ofClass || !ofClass->hasFixedLayout || isCreatedWithoutLayout) {
        auto* target = dynamicDowncast<Closure>(newTarget);
        uint32_t function = (target && target->structure()->aotInstance() == &instance ? target : constructor)->functionIndex();
        unsigned capacity = m_inlineCapacitiesForThis.ensure(function, [&] {
            return inlineCapacityForThis(vm, constructor->functionIndex(), function, JSFinalObject::defaultInlineCapacity, parent);
        }).iterator->value;
        JSObject* object = constructEmptyObject(globalObject, parent, capacity);
        if (isCreatedWithoutLayout) {
            for (const FieldData& field : std::span { ofClass->fields(), ofClass->numberOfFields }) {
                if (!(field.flags & (fieldIsAbsentAtCreation | fieldIsPlaceholder)))
                    object->putDirect(vm, name(vm, field.name), jsUndefined());
            }
            for (const LayoutData* declaring = ofClass; declaring; declaring = baseOf(*declaring))
                addPrivateMethods(globalObject, object, *declaring);
            Vector<const LayoutData*, 4> descendants;
            JSObject* next = newTarget;
            while (next && next != constructor) {
                auto* heir = dynamicDowncast<Closure>(next);
                if (const LayoutData* descendantLayout = heir && heir->structure()->aotInstance() == &instance ? heir->classData() : nullptr; descendantLayout && descendantLayout->hasFixedLayout)
                    descendants.append(descendantLayout);
                JSValue parentClass = next->getPrototypeDirect();
                next = parentClass.isObject() ? asObject(parentClass) : nullptr;
            }
            if (next) {
                for (const LayoutData* descendantLayout : descendants)
                    addPrivateMethods(globalObject, object, *descendantLayout);
            }
        }
        return object;
    }
    if (!made->shape) {
        recordViolation(vm, { .kind = "construct-new-target"_s, .on = made });
        throwTypeError(globalObject, scope, withAOTErrorNote(makeString("Cannot construct "_s, nameOf(*made), ": no TypeScript code of the program calls 'new "_s, nameOf(*made), "', so the class was compiled without instances"_s)));
        return { };
    }
    Object* object = createClassInstance(vm, *made, parent);
    auto* target = dynamicDowncast<Closure>(newTarget);
    bool hasWatchedField = std::ranges::any_of(std::span { made->fields(), made->numberOfFields }, [](const FieldData& field) {
        return field.flags & fieldIsPinned && !(field.flags & fieldIsAbsentAtCreation);
    });
    bool prototypeIsAllowedByClass = made->mayHaveOtherPrototype || parent == classPrototypeIfExists(made->index);
    if (target && target->structure()->aotInstance() == &instance && prototypeIsAllowedByClass && !isCounting() && !data.isCompiledForAudit && !hasWatchedField && !(made->hasExtension && object->extension())) {
        instance.typedThisAllocations.get()[(std::bit_cast<uintptr_t>(newTarget) >> 4) % Instance::numberOfTypedThisAllocations] = { };
        known = { constructor, newTarget, object->structure(), instance.negativeOffsetFields().typedPrototypeEpoch, Vector<uint8_t>(std::span { std::bit_cast<const uint8_t*>(object) + sizeof(JSCell), sizeWithSpecializationCapacity(made->shape) - sizeof(JSCell) }) };
        shareWithStub(vm, known);
    }
    return object;
}

Structure* State::structureForThis(JSGlobalObject* globalObject, Closure* constructor, JSObject* newTarget, unsigned inlineCapacity)
{
    VM& vm = globalObject->vm();
    auto* target = dynamicDowncast<Closure>(newTarget);
    if (!target || target->structure()->aotInstance() != &instance)
        return nullptr;
    const LayoutData* ofClass = constructor->classData();
    if (ofClass ? ofClass->hasUntypedBase || ofClass->hasFixedLayout || baseOf(*ofClass) : !!thisAllocation(constructor->functionIndex()))
        return nullptr;
    unsigned attributes = 0;
    JSValue prototype = target->getDirect(vm, vm.propertyNames->prototype, attributes);
    if (!prototype)
        prototype = target == constructor && ofClass && createsClassObjectOnMutation(*ofClass) ? JSValue(classPrototype(vm, ofClass->index)) : target->prototypeProperty(vm);
    if (prototype && attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return nullptr;
    JSObject* parent = prototype && prototype.isObject() ? asObject(prototype) : globalObject->objectPrototype();
    unsigned capacity = m_inlineCapacitiesForThis.ensure(target->functionIndex(), [&] {
        return inlineCapacityForThis(vm, constructor->functionIndex(), target->functionIndex(), inlineCapacity, parent);
    }).iterator->value;
    return globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, parent, capacity);
}

unsigned State::inlineCapacityForThis(VM& vm, uint32_t constructor, uint32_t newTarget, unsigned seen, JSObject* prototype) const
{
    std::span counts { data.at<std::array<uint32_t, 2>>(data.thisPropertyCountsOffset), data.numberOfThisPropertyCounts };
    auto countOf = [&](uint32_t function) -> uint32_t {
        auto found = std::ranges::lower_bound(counts, function, { }, [](auto& functionAndCount) {
            return functionAndCount[0];
        });
        return found != counts.end() && (*found)[0] == function ? (*found)[1] : 0;
    };
    auto countAlongFollowedCalls = [](uint32_t count) -> unsigned {
        return count & Data::propertyCountMask;
    };
    auto countFromItself = [](uint32_t count) -> unsigned {
        return count >> Data::propertyCountBits & Data::propertyCountMask;
    };
    uint32_t ofTarget = countOf(newTarget);
    unsigned given = countAlongFollowedCalls(ofTarget);
    unsigned countFromConstructor = constructor == newTarget ? 0 : countAlongFollowedCalls(countOf(constructor));
    given = ofTarget & Data::propertyCountIncludesPrototypeChain ? std::max(given, countFromConstructor) : given + countFromConstructor;
    static constexpr unsigned maxLevels = 16;
    Vector<uint32_t, maxLevels + 1> constructors { newTarget };
    for (unsigned level = 0; prototype && level < maxLevels; ++level) {
        Structure* structure = prototype->structure();
        TypeInfo info = structure->typeInfo();
        if (info.overridesGetOwnPropertySlot() || info.overridesGetPrototype() || structure->hasPolyProto())
            break;
        unsigned attributes = 0;
        JSValue named = prototype->getDirect(vm, vm.propertyNames->constructor, attributes);
        if (named && named.isCell() && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) && named.asCell()->structure()->aotInstance() == &instance) {
            std::optional<uint32_t> function;
            if (auto* closure = dynamicDowncast<Closure>(named.asCell()))
                function = closure->functionIndex();
            else if (auto* closure = dynamicDowncast<SlimClosure>(named.asCell()))
                function = closure->functionIndex();
            if (function && !constructors.contains(*function))
                constructors.append(*function);
        }
        JSValue next = structure->storedPrototype();
        prototype = next.isObject() ? asObject(next) : nullptr;
    }
    unsigned countAlongPrototypeChain = 0;
    for (size_t i = constructors.size(); i--;) {
        uint32_t count = countOf(constructors[i]);
        unsigned itself = countFromItself(count);
        unsigned followed = countAlongFollowedCalls(count);
        countAlongPrototypeChain = itself + std::max(followed > itself ? followed - itself : 0, countAlongPrototypeChain);
    }
    if (Options::useAOTOperationCounters() && countAlongPrototypeChain > given) [[unlikely]]
        runtimeTable(vm).countOperation("createThis", "places-found-along-the-prototype-chain", countAlongPrototypeChain - given);
    given = std::max(given, countAlongPrototypeChain);
    seen = std::min<unsigned>(std::max(seen, given), JSFinalObject::maxInlineCapacity);
    size_t room = (MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(seen)) - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue);
    return std::min<size_t>(room, JSFinalObject::maxInlineCapacity);
}

const AllocationData* State::thisAllocation(uint32_t function) const
{
    return Typed::thisAllocation(data, function);
}

bool State::rejectsFreshObject(uint32_t function, uint32_t shape) const
{
    std::span refused { data.at<std::array<uint32_t, 2>>(data.rejectedFreshObjectsOffset), data.numberOfRejectedFreshObjects };
    return std::ranges::binary_search(refused, std::array { function, shape });
}

uint32_t State::objectCreatorFlags(uint32_t function) const
{
    static_assert(!(isObjectCreator & (Data::createsObjectsOfFlaggedLayout | Data::createsObjectsWithDefinitelyInitializedFields)));
    std::span makers { data.at<uint32_t>(data.objectCreatorsOffset), data.numberOfObjectCreators };
    auto found = std::ranges::lower_bound(makers, function << 2);
    return found != makers.end() && *found >> 2 == function ? isObjectCreator | (*found & 3) : 0;
}

bool State::hasGuardedPrototype(uint32_t function)
{
    return createsFixedLayoutObjects(function);
}

bool State::isReservedFieldName(VM&, UniquedStringImpl* name, uint32_t function)
{
    auto number = fieldNameIndex(name);
    if (!number)
        return false;
    if (std::ranges::binary_search(std::span { data.at<uint32_t>(data.functionsWithoutFieldNamesOnPrototypeOffset), data.numberOfFunctionsWithoutFieldNamesOnPrototype }, function))
        return true;
    return std::ranges::binary_search(std::span { data.at<uint32_t>(data.reservedFieldNamesOffset), data.numberOfReservedFieldNames }, *number);
}

Structure* State::moduleNamespaceStructure(VM& vm, bool hasHostPrototype)
{
    if (!m_moduleNamespaceStructure) {
        m_moduleNamespaceStructure = ModuleNamespace::createStructure(vm, instance.globalObject, hasHostPrototype);
        didCreate(m_moduleNamespaceStructure);
    }
    return m_moduleNamespaceStructure;
}

Structure* State::structureForNewModuleNamespace(VM& vm, bool hasHostPrototype)
{
    moduleNamespaceStructure(vm, hasHostPrototype);
    return ModuleNamespace::createStructure(vm, instance.globalObject, hasHostPrototype);
}

Structure* State::prototypeObjectStructure(VM& vm)
{
    if (!m_prototypeObjectStructure) {
        m_prototypeObjectStructure = PrototypeObject::createStructure(vm, instance.globalObject, instance.globalObject->objectPrototype());
        m_prototypeObjectStructure->setAOTInstance(&instance);
        didCreate(m_prototypeObjectStructure);
    }
    return m_prototypeObjectStructure;
}

const State::NamedPropertiesTransition* State::namedPropertiesTransition(VM& vm, Structure* from, std::span<const EncodedJSValue> names)
{
    auto isFor = [&](const NamedPropertiesTransition& known) {
        return known.from == from && std::ranges::equal(known.names, names);
    };
    if (m_lastNamedPropertiesTransitionIndex < m_namedPropertiesTransitions.size() && isFor(m_namedPropertiesTransitions[m_lastNamedPropertiesTransitionIndex])) [[likely]]
        return &m_namedPropertiesTransitions[m_lastNamedPropertiesTransitionIndex];
    for (auto& known : m_namedPropertiesTransitions) {
        if (isFor(known)) {
            m_lastNamedPropertiesTransitionIndex = &known - m_namedPropertiesTransitions.span().data();
            return &known;
        }
    }
    static constexpr size_t maxTransitions = 32;
    if (m_namedPropertiesTransitions.size() == maxTransitions)
        return nullptr;
    NamedPropertiesTransition transition;
    transition.from = from;
    for (EncodedJSValue name : names)
        transition.names.append(static_cast<uint32_t>(name));
    transition.to = [&]() -> Structure* {
        if (from->typeInfo().type() != ArrayType || from->isDictionary() || !from->isStructureExtensible() || from->hasImmutableProperties() || isCopyOnWrite(from->indexingMode()) || hasAnyArrayStorage(from->indexingType()))
            return nullptr;
        JSGlobalObject* globalObject = instance.globalObject;
        JSObject* arrayPrototype = globalObject->arrayPrototype();
        JSObject* objectPrototype = globalObject->objectPrototype();
        if (from->storedPrototype() != JSValue(arrayPrototype) || arrayPrototype->getPrototypeDirect() != JSValue(objectPrototype) || !objectPrototype->getPrototypeDirect().isNull())
            return nullptr;
        for (JSObject* holder : { arrayPrototype, objectPrototype }) {
            if (!holder->structure()->hasImmutableProperties() || holder->structure()->isStructureExtensible())
                return nullptr;
        }
        Structure* to = from;
        for (uint32_t index : transition.names) {
            const Identifier& name = this->name(vm, index);
            if (parseIndex(name) || name == vm.propertyNames->length || to->get(vm, name) != invalidOffset)
                return nullptr;
            PropertySlot slot(arrayPrototype, PropertySlot::InternalMethodType::VMInquiry, &vm);
            if (arrayPrototype->getPropertySlot(globalObject, name, slot) || slot.isTaintedByOpaqueObject())
                return nullptr;
            PropertyOffset offset;
            to = Structure::addPropertyTransition(vm, to, name, static_cast<unsigned>(PropertyAttribute::None), offset);
            if (to->isDictionary() || !isOutOfLineOffset(offset))
                return nullptr;
            transition.offsets.append(offset);
        }
        transition.oldOutOfLineCapacity = from->outOfLineCapacity();
        transition.newOutOfLineCapacity = to->outOfLineCapacity();
        transition.elementsDependOnVectorLength = hasUndecided(from->indexingType()) || hasInt32(from->indexingType()) || hasDouble(from->indexingType()) || hasContiguous(from->indexingType());
        return to;
    }();
    didCreate(from);
    if (transition.to)
        didCreate(transition.to);
    m_namedPropertiesTransitions.append(WTF::move(transition));
    return &m_namedPropertiesTransitions.last();
}

std::span<Structure* const> State::closureStructures(VM& vm)
{
    if (!m_closureStructures[0]) {
        JSGlobalObject* globalObject = instance.globalObject;
        JSObject* prototypes[] = { globalObject->functionPrototype(), globalObject->asyncFunctionPrototype(), globalObject->generatorFunctionPrototype(), globalObject->asyncGeneratorFunctionPrototype(), globalObject->functionPrototype() };
        static_assert(std::size(prototypes) == Instance::numberOfTypedClosureStructures && std::size(prototypes) == std::size(decltype(m_closureStructures) { }));
        static_assert(constructorFunctionKind == static_cast<uint8_t>(Format::FunctionKind::Constructor));
        for (unsigned i = 0; i < std::size(prototypes); ++i) {
            m_closureStructures[i] = instance.loader() == globalObject->moduleLoader() ? realmClosureStructure(vm, globalObject, i) : Closure::createStructure(vm, globalObject, prototypes[i]);
            m_closureStructures[i]->setAOTInstance(&instance);
            instance.typedClosureStructureID(i) = m_closureStructures[i]->id().bits();
        }
        instance.typedClosureStructureIDPair |= static_cast<uint64_t>(m_closureStructures[0]->id().bits()) << 32;
        createSlimClosureStructures(vm);
    }
    return m_closureStructures;
}

Structure* State::slimClosureStructure(VM& vm, uint32_t function)
{
    closureStructures(vm);
    return m_slimClosureStructures[slimClosureKindOf(kindAndDeclarationOf(function))];
}

void State::createSlimClosureStructures(VM& vm)
{
    JSGlobalObject* globalObject = instance.globalObject;
    JSObject* prototypes[] = { globalObject->functionPrototype(), globalObject->asyncFunctionPrototype() };
    static_assert(std::size(prototypes) == numberOfSlimClosureKinds && std::size(prototypes) == std::size(decltype(instance.typedSlimClosureHeaders) { }));
    for (unsigned i = 0; i < numberOfSlimClosureKinds; ++i) {
        m_slimClosureStructures[i] = SlimClosure::createStructure(vm, instance, prototypes[i], false);
        didCreate(m_slimClosureStructures[i]);
    }
    for (unsigned i = 0; i < numberOfSlimClosureKinds; ++i)
        instance.typedSlimClosureHeaders[i] = static_cast<uint64_t>(m_slimClosureStructures[i]->typeInfoBlob()) << 32 | m_slimClosureStructures[i]->id().bits();
    instance.typedClosureStructureIDPair |= m_slimClosureStructures[0]->id().bits();
    g_aotSlimClosureEntrypoint = slimClosureEntrypoint().taggedPtr();
    for (unsigned i = 0; i < std::size(instance.typedSlimClosureAllocators); ++i)
        instance.typedSlimClosureAllocators[i] = vm.heap.aotClosureSpace.allocatorFor(16 * (i + 1), AllocatorForMode::EnsureAllocator).localAllocator();
}

JSObject* State::slimClosureLazyPropertyHolder(VM& vm)
{
    if (!m_slimClosureLazyPropertyHolder) {
        JSGlobalObject* globalObject = instance.globalObject;
        Structure* empty = globalObject->nullPrototypeObjectStructure();
        JSObject* holder = constructEmptyObject(vm, empty);
        didCreate(holder);
        JSObject* length = constructEmptyObject(vm, empty);
        holder->putDirectAccessor(globalObject, vm.propertyNames->length, GetterSetter::create(vm, globalObject, length, nullptr), PropertyAttribute::Accessor | PropertyAttribute::DontEnum);
        JSObject* name = constructEmptyObject(vm, empty);
        holder->putDirectAccessor(globalObject, vm.propertyNames->name, GetterSetter::create(vm, globalObject, name, nullptr), PropertyAttribute::Accessor | PropertyAttribute::DontEnum);
        instance.typedSlimClosureLengthGetter = length;
        instance.typedSlimClosureNameGetter = name;
        m_slimClosureLazyPropertyHolder = holder;
    }
    return m_slimClosureLazyPropertyHolder;
}

Structure* State::reifiedSlimClosureStructure(VM& vm, uint32_t function)
{
    uint8_t kind = kindAndDeclarationOf(function);
    bool isAsync = kind & functionIsAsync;
    bool constructs = !(kind & (functionKindMask | functionIsAsync));
    bool isBound = static_cast<Format::FunctionKind>(kind & functionKindMask) == Format::FunctionKind::Bound;
    Structure*& structure = m_reifiedSlimClosureStructures[isAsync][isBound ? 2 : constructs];
    if (!structure) {
        if (!m_functionIndexPrivateName)
            m_functionIndexPrivateName = PrivateSymbolImpl::create(*StringImpl::create("function"_span8).ptr());
        JSGlobalObject* globalObject = instance.globalObject;
        Structure* next = SlimClosure::createStructure(vm, instance, isAsync ? static_cast<JSObject*>(globalObject->asyncFunctionPrototype()) : globalObject->functionPrototype(), true, constructs, isBound);
        std::pair<PropertyName, unsigned> properties[] = {
            { PropertyName(m_functionIndexPrivateName.get()), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete },
            { vm.propertyNames->length, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum },
            { vm.propertyNames->name, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum },
            { vm.propertyNames->prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete },
        };
        for (unsigned i = 0; i < 3u + constructs; ++i) {
            PropertyOffset offset;
            next = Structure::addPropertyTransition(vm, next, properties[i].first, properties[i].second, offset);
            RELEASE_ASSERT(offset == static_cast<PropertyOffset>(firstOutOfLineOffset + i));
        }
        structure = next;
        didCreate(structure);
    }
    return structure;
}

Structure* State::boundClosureStructure(VM& vm, uint32_t function)
{
    bool isAsync = kindAndDeclarationOf(function) & functionIsAsync;
    Structure*& structure = m_boundClosureStructures[isAsync];
    if (!structure) {
        JSGlobalObject* globalObject = instance.globalObject;
        structure = Closure::createStructure(vm, globalObject, isAsync ? static_cast<JSObject*>(globalObject->asyncFunctionPrototype()) : globalObject->functionPrototype(), true);
        structure->setAOTInstance(&instance);
        didCreate(structure);
    }
    return structure;
}

Structure* State::closureStructure(VM& vm, uint32_t function)
{
    return closureStructures(vm)[closureStructureOf(kindAndDeclarationOf(function))];
}

Structure* State::nonClassClosureStructure(VM& vm, uint32_t function)
{
    RELEASE_ASSERT(kindOf(function) != Format::FunctionKind::Constructor);
    return closureStructure(vm, function);
}

static JSC_DECLARE_CUSTOM_GETTER(lazyClosureGetter);

JSC_DEFINE_CUSTOM_GETTER(lazyClosureGetter, (JSGlobalObject* globalObject, EncodedJSValue encodedHolder, PropertyName name))
{
    JSObject* holder = asObject(JSValue::decode(encodedHolder));
    return JSValue::encode(holder->realm()->aotInstance()->typed->reifyLazyClosure(globalObject->vm(), holder, name));
}

JSObject* State::createClosureOver(VM& vm, uint32_t function, JSValue captured)
{
    uint64_t capture = JSValue::encode(captured);
    if (!(kindAndDeclarationOf(function) & functionIsInBuiltinModule))
        ensureProgramStaticsInitialized();
    if (usesSlimClosures(function))
        return SlimClosure::create(vm, slimClosureStructure(vm, function), function, std::span { &capture, 1 }, 1);
    return Closure::create(vm, nonClassClosureStructure(vm, function), function, std::span { &capture, 1 }, 1);
}

JSObject* State::reifyLazyClosure(VM& vm, JSObject* holder, PropertyName name)
{
    unsigned attributes;
    PropertyOffset offset = holder->structure()->get(vm, name, attributes);
    RELEASE_ASSERT(isValidOffset(offset) && attributes & PropertyAttribute::CustomValue);
    uint32_t function = lazyClosureFunction(holder->getDirect(offset).asCell());
    JSObject* closure = createClosureOver(vm, function, holder);
    holder->putDirect(vm, name, closure, attributes & ~PropertyAttribute::CustomValue);
    return closure;
}

JSObject* State::reifyLazyClosureIfPresent(VM& vm, JSObject* holder, UniquedStringImpl* name, PropertyOffset& offset, bool& isCreatedByStubs)
{
    if (m_lazyClosureFunctions.isEmpty() || !holder->structure()->hasAnyKindOfGetterSetterProperties())
        return nullptr;
    unsigned attributes;
    offset = holder->structure()->get(vm, name, attributes);
    if (!isValidOffset(offset) || !(attributes & PropertyAttribute::CustomValue))
        return nullptr;
    JSCell* cell = holder->getDirect(offset).asCell();
    if (!m_lazyClosureFunctions.contains(cell))
        return nullptr;
    isCreatedByStubs = uncheckedDowncast<CustomGetterSetter>(cell)->lazyClosureRequest();
    return reifyLazyClosure(vm, holder, name);
}

void State::reifyLazyClosures(VM& vm, JSObject* holder)
{
    Structure* structure = holder->structure();
    if (m_lazyClosureFunctions.isEmpty() || !structure->hasAnyKindOfGetterSetterProperties())
        return;
    Vector<RefPtr<UniquedStringImpl>, 8> names;
    structure->forEachProperty(vm, [&](const auto& entry) {
        if (entry.attributes() & PropertyAttribute::CustomValue && m_lazyClosureFunctions.contains(holder->getDirect(entry.offset()).asCell()))
            names.append(entry.key());
        return true;
    });
    for (auto& name : names)
        reifyLazyClosure(vm, holder, name.get());
}

CustomGetterSetter* State::lazyClosureCell(VM& vm, uint32_t function)
{
    if (JSCell* cell = closureIfCreated(function))
        return uncheckedDowncast<CustomGetterSetter>(cell);
    CustomGetterSetter* cell = CustomGetterSetter::create(vm, lazyClosureGetter, nullptr);
    if (usesSlimClosures(function))
        cell->setLazyClosureRequest(function | slimClosureKindOf(kindAndDeclarationOf(function)) << ClosureRequest::executableShift | 1u << ClosureRequest::isSlimBit);
    m_lazyClosureFunctions.add(cell, function);
    didCreateClosureFor(function, cell);
    return cell;
}

bool State::closuresHaveCaptures(uint32_t function) const
{
    return data.at<std::array<uint8_t, 2>>(data.closureCountsOffset)[function][1];
}

void State::putLazyClosure(JSGlobalObject* globalObject, JSValue base, const Identifier& name, uint32_t function)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (tryPutLazyClosure(globalObject, base, name, function))
        return;
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, nullishMessage(base, name.string(), true));
        return;
    }
    JSValue closure = createClosureOver(vm, function, base);
    PutPropertySlot slot(base, true);
    scope.release();
    base.put(globalObject, name, closure, slot);
}

bool State::tryPutLazyClosure(JSGlobalObject* globalObject, JSValue base, const Identifier& name, uint32_t function)
{
    VM& vm = globalObject->vm();
    if (!base.isCell() || base.asCell()->type() != FinalObjectType || parseIndex(name) || name == vm.propertyNames->underscoreProto)
        return false;
    JSObject* holder = asObject(base);
    Structure* structure = holder->structure();
    if (structure->realm() != instance.globalObject || structure->compiledLayout() || structure->isDictionary() || !structure->isStructureExtensible() || structure->hasImmutableShape() || structure->hasImmutableProperties() || structure->hasPolyProto()
        || structure->transitionCountEstimate() > Structure::s_maxTransitionLength)
        return false;
    unsigned ownAttributes;
    if (isValidOffset(structure->get(vm, name.impl(), ownAttributes))) {
        if (ownAttributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue)
            return false;
        return holder->replaceOwnPropertyWithCustomValue(vm, name, lazyClosureCell(vm, function), ownAttributes | PropertyAttribute::CustomValue);
    }
    for (JSValue next = structure->storedPrototype(); next.isObject();) {
        JSObject* prototype = asObject(next);
        ClassPrototype::look(prototype);
        Structure* prototypeStructure = prototype->structure();
        TypeInfo info = prototypeStructure->typeInfo();
        bool overridesPut = info.overridesPut() && !prototype->inherits<PrototypeObject>();
        if (prototypeStructure->hasPolyProto() || overridesPut || info.overridesGetPrototype() || info.overridesGetOwnPropertySlot() || prototype->hasNonReifiedStaticProperties())
            return false;
        unsigned attributes;
        if (isValidOffset(prototypeStructure->get(vm, name.impl(), attributes)) && attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue)
            return false;
        next = prototypeStructure->storedPrototype();
    }
    return holder->putDirectCustomAccessor(vm, name, lazyClosureCell(vm, function), static_cast<unsigned>(PropertyAttribute::CustomValue));
}

JSObject* State::prototypeForInstancesOf(VM& vm, Closure* constructor)
{
    JSValue prototype = constructor->prototypeProperty(vm);
    return prototype.isObject() ? asObject(prototype) : instance.globalObject->objectPrototype();
}

void State::didChangePrototypeOf(VM& vm, Closure* constructor)
{
    ++instance.negativeOffsetFields().typedPrototypeEpoch;
    if (closureIfCreated(constructor->functionIndex()) != constructor)
        return;
    if (constructor->isDeclaredAs(functionIsGenerator)) {
        bool isAsync = constructor->isDeclaredAs(functionIsAsync);
        JSObject* prototype = constructor->getDirect(vm, vm.propertyNames->prototype).getObject();
        if (!prototype)
            prototype = isAsync ? static_cast<JSObject*>(instance.globalObject->asyncGeneratorPrototype()) : instance.globalObject->generatorPrototype();
        setGeneratorPrototype(vm, resumeFunctionOf(constructor->functionIndex()), isAsync, prototype);
        m_generatorTemplates.fill(Generator::Template { });
        return;
    }
    uint32_t function = constructor->functionIndex();
    std::span allocationsByConstructor { data.at<std::array<uint32_t, 2>>(data.allocationsByConstructorOffset), data.numberOfAllocationsByConstructor };
    for (auto found = std::ranges::lower_bound(allocationsByConstructor, std::array<uint32_t, 2> { function, 0 }); found != allocationsByConstructor.end() && (*found)[0] == function; ++found) {
        if (structureIDOf((*found)[1]))
            fill(vm, (*found)[1]);
    }
}

Structure* State::structure(VM& vm, uint32_t shape)
{
    const LayoutData& layout = layoutForShape(shape);
    if (layout.kind == LayoutKind::Environment) {
        uint32_t page = shape >> 16;
        while (m_environmentStructures.size() <= page)
            m_environmentStructures.append(nullptr);
        if (!m_environmentStructures[page]) {
            Structure* created = Object::createStructure(vm, instance, jsNull(), &layout);
            created->setCompiledEnvironments(&data, page);
            m_environmentStructures[page] = created;
            didCreate(created);
        }
        return m_environmentStructures[page];
    }
    if (layout.kind == LayoutKind::Closure)
        return closureStructures(vm)[0];
    if (uint32_t known = structures[shape])
        return std::bit_cast<StructureID>(known).decode();
    Structure* structure = nullptr;
    JSValue prototype = instance.globalObject->objectPrototype();
    switch (layout.kind) {
    case LayoutKind::Closure:
    case LayoutKind::Environment:
        RELEASE_ASSERT_NOT_REACHED();
    case LayoutKind::Record:
        break;
    case LayoutKind::Instance:
        if (layout.isEvaluatedOnce)
            prototype = classPrototype(vm, layout.index);
        break;
    }
    structure = Object::createStructure(vm, instance, prototype, &layout);
    structures[shape] = structure->id().bits();
    didCreate(structure);
    return structure;
}

static constexpr uint32_t keyHasExtraProperties = 1u << 31;

Structure* State::structureFor(VM& vm, uint32_t shape, JSValue prototype, bool hasExtraProperties)
{
    Structure* usual = structure(vm, shape);
    if (!hasExtraProperties && usual->storedPrototype() == prototype)
        return usual;
    RELEASE_ASSERT(shape < keyHasExtraProperties);
    std::pair<uint32_t, JSObject*> key { shape | (hasExtraProperties ? keyHasExtraProperties : 0), prototype.getObject() };
    if (Structure* existing = m_structuresWithOtherPrototype.get(key))
        return existing;
    const LayoutData& layout = layoutForShape(shape);
    RELEASE_ASSERT(layout.offsetOfExtraProperties() + sizeOfExtraPropertiesWord <= MarkedSpace::optimalSizeFor(layout.size));
    Structure* created = Object::createStructure(vm, instance, prototype, &layout);
    if (hasExtraProperties) {
        created->markingStampAndFlags() |= Structure::isStructureWithExtraProperties;
        created->setExtraPropertiesStructure(nullptr, static_cast<uint16_t>(layout.offsetOfExtraProperties()), 0);
    }
    m_structuresWithOtherPrototype.set(key, created);
    if (hasExtraProperties)
        updatePrototypeMatchesLayout(created);
    return created;
}

Structure* State::structureFor(VM& vm, uint32_t shape, JSValue prototype, Structure& ofExtraProperties)
{
    if (Structure* found = recentStructureFor(shape, prototype, ofExtraProperties))
        return found;
    if (ofExtraProperties.isDictionary() || !ofExtraProperties.propertyAccessesAreCacheable())
        return structureFor(vm, shape, prototype, true);
    std::tuple<uint32_t, JSObject*, Structure*> key { shape, prototype.getObject(), &ofExtraProperties };
    if (Structure* existing = m_structuresByExtraProperties.get(key)) {
        m_recentStructuresByExtraProperties[recentStructureIndex(shape, ofExtraProperties)] = existing->id();
        return existing;
    }
    const LayoutData& layout = layoutForShape(shape);
    RELEASE_ASSERT(layout.offsetOfExtraProperties() + sizeOfExtraPropertiesWord <= MarkedSpace::optimalSizeFor(layout.size));
    Structure* created = Object::createStructure(vm, instance, prototype, &layout);
    created->markingStampAndFlags() |= Structure::isStructureWithExtraProperties;
    created->setExtraPropertiesStructure(&ofExtraProperties, static_cast<uint16_t>(layout.offsetOfExtraProperties()), static_cast<uint16_t>(WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(JSFinalObject::allocationSize(ofExtraProperties.inlineCapacity()))));
    m_structuresByExtraProperties.set(key, created);
    m_recentStructuresByExtraProperties[recentStructureIndex(shape, ofExtraProperties)] = created->id();
    m_requestedExtraPropertiesStructures.set(requestedStructureIndex(ofExtraProperties));
    updatePrototypeMatchesLayout(created);
    return created;
}

Structure* State::existingStructureFor(uint32_t shape, JSValue prototype, Structure& ofExtraProperties)
{
    if (Structure* found = recentStructureFor(shape, prototype, ofExtraProperties))
        return found;
    if (!m_requestedExtraPropertiesStructures.get(requestedStructureIndex(ofExtraProperties)))
        return nullptr;
    Structure* existing = m_structuresByExtraProperties.get({ shape, prototype.getObject(), &ofExtraProperties });
    if (existing)
        m_recentStructuresByExtraProperties[recentStructureIndex(shape, ofExtraProperties)] = existing->id();
    return existing;
}

Structure* State::recentStructureFor(uint32_t shape, JSValue prototype, Structure& ofExtraProperties) const
{
    StructureID id = m_recentStructuresByExtraProperties[recentStructureIndex(shape, ofExtraProperties)];
    if (!id)
        return nullptr;
    Structure* found = id.decode();
    return found->extraPropertiesStructureID() == ofExtraProperties.id() && found->compiledLayout()->shape == shape && found->storedPrototype() == prototype ? found : nullptr;
}

Structure* State::structureWithExtraPropertiesMatching(VM& vm, Structure* model, uint32_t shape, JSValue prototype)
{
    if (StructureID ofExtraProperties = model->extraPropertiesStructureID())
        return structureFor(vm, shape, prototype, *ofExtraProperties.decode());
    return structureFor(vm, shape, prototype, !isStructureWithoutExtraProperties(model));
}

bool State::isStructureWithoutExtraProperties(Structure* structure)
{
    return !(structure->markingStampAndFlags() & Structure::isStructureWithExtraProperties);
}

uint32_t State::initialOrderWord(VM& vm, const AllocationData& allocation, Structure* structure)
{
    const LayoutData* layout = structure->compiledLayout();
    bool isCreatedByClass = allocation.constructedBy != LayoutData::noBase && closure(vm, allocation.constructedBy)->classData();
    bool asks = layout && isObjectLayout(layout->kind) && layout->mayHaveOtherPrototype && !isCreatedByClass && !prototypeMatchesLayout(*layout, structure->storedPrototype());
    return allocation.orderWord | (asks ? orderWordHasExtraProperties : 0);
}

void State::fill(VM& vm, uint32_t number)
{
    const AllocationData& allocation = this->allocation(number);
    Structure* structure = this->structure(vm, allocation.shape);
    if (allocation.constructedBy != LayoutData::noBase) {
        if (const LayoutData* ofClass = closure(vm, allocation.constructedBy)->classData(); ofClass && ofClass->shape != allocation.shape)
            RELEASE_ASSERT(!hasPrototypeMembers(*ofClass));
        structure = structureFor(vm, allocation.shape, prototypeForInstancesOf(vm, closure(vm, allocation.constructedBy)));
        didCreate(structure);
    }
    const LayoutData* layout = structure->compiledLayout();
    size_t size = allocation.size;
    Instance::TypedAllocation slot { };
    RELEASE_ASSERT((layoutForShape(allocation.shape).kind == LayoutKind::Closure) == !structure->isCompiledCellStructure());
    slot.header = static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
    if (structure->compiledEnvironmentData()) {
        slot.header &= ~(0xffull << 8 * offsetOfEnvironmentShapeLowByte | 0xffull << 8 * offsetOfEnvironmentShapeHighByte);
        slot.header |= static_cast<uint64_t>(allocation.shape & 0xff) << 8 * offsetOfEnvironmentShapeLowByte | static_cast<uint64_t>(allocation.shape >> 8 & 0xff) << 8 * offsetOfEnvironmentShapeHighByte;
    }
    slot.words = static_cast<uint64_t>(shapeWordOf(allocation.shape)) << 32 | initialOrderWord(vm, allocation, structure);
    CompleteSubspace& space = layout && isObjectLayout(layout->kind) ? vm.cellSpace() : vm.heap.aotClosureSpace;
    LocalAllocator* allocator = space.allocatorFor(size, AllocatorForMode::EnsureAllocator).localAllocator();
    if (allocation.usesFieldTemplate && allocator && layout && isObjectLayout(layout->kind) && allocator->cellSize() > offsetOfFirstField)
        slot.fieldTemplate = fieldTemplate(*layout, allocator->cellSize());
    slot.allocator = allocator;
    if (Instance::TypedAllocation* row = instance.typedAllocation(number)) {
        *row = slot;
        return;
    }
    if (!allocator || m_numberOfAllocationEntries == maxAllocationEntries) {
        m_structureIDsOfAllocationsWithoutEntry.set(number, structure->id().bits());
        return;
    }
    uint16_t row = ++m_numberOfAllocationEntries;
    *(reinterpret_cast<Instance::TypedAllocation*>(instance.typedAllocationIndices) - row) = slot;
    instance.typedAllocationIndices[number] = row;
}

uint32_t State::structureIDOf(uint32_t number) const
{
    if (const Instance::TypedAllocation* row = instance.typedAllocation(number))
        return static_cast<uint32_t>(row->header);
    return m_structureIDsOfAllocationsWithoutEntry.get(number);
}

const void* State::fieldTemplate(const LayoutData& layout, uint32_t cellSize)
{
    RELEASE_ASSERT(cellSize >= layout.size && cellSize > offsetOfFirstField);
    return m_fieldTemplates.ensure({ layout.shape, cellSize }, [&] {
        auto bytes = MallocSpan<uint8_t>::zeroedMalloc(cellSize - offsetOfFirstField);
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            const FieldData& field = layout.fields()[i];
            if (!(field.flags & fieldIsOutOfLine))
                clearSlot(&bytes[field.offset - offsetOfFirstField], field.store());
        }
        return bytes;
    }).iterator->value.span().data();
}

JSCell* State::allocate(VM& vm, uint32_t number)
{
    if (!structureIDOf(number))
        fill(vm, number);
    Structure* structure = std::bit_cast<StructureID>(structureIDOf(number)).decode();
    JSCell* cell = nullptr;
    if (const LayoutData& layout = layoutForShape(allocation(number).shape); layout.kind != LayoutKind::Closure)
        cell = Object::create(vm, structure, layout, allocation(number).size, initialOrderWord(vm, allocation(number), structure));
    else
        cell = Closure::createForCompiledCode(vm, structure, allocation(number).size);
    countAllocation(number, cell);
    return cell;
}

uint32_t State::sizeWithSpecializationCapacity(uint32_t shape)
{
    return layoutForShape(shape).sizeWithSpecializationCapacity();
}

bool State::hasImmutableMethod(const LayoutData& layout, UniquedStringImpl* name) const
{
    if (name->isSymbol())
        return false;
    bool isDeclared = false;
    for (const LayoutData* next = &layout; next && !isDeclared; next = baseOf(*next)) {
        if (isImmutableIn(next->methods(), name, isDeclared))
            return true;
    }
    return false;
}

bool State::storeNeverSpecializes(const LayoutData& layout, UniquedStringImpl* name) const
{
    if (!layout.numberOfSpecializations() || PropertyName(name).isPrivateName() || isPublicSymbolThatKeysNoField(name))
        return true;
    auto found = m_specializationsWithField.find({ layout.shape, name });
    return found != m_specializationsWithField.end() && found->value.isEmpty();
}

bool State::declaresMethod(const LayoutData& ofClass, UniquedStringImpl* name) const
{
    if (name->isSymbol())
        return false;
    return std::ranges::any_of(ofClass.methods(), [&](auto& method) {
        return !(method.flags & methodHasComputedName) && isName(method.name, name);
    });
}

std::optional<JSObject*> State::declaredMethod(VM& vm, const LayoutData& ofClass, UniquedStringImpl* name)
{
    if (name->isSymbol())
        return nullptr;
    const MethodData* found = nullptr;
    for (auto& method : ofClass.methods()) {
        if (method.flags & methodHasComputedName)
            return std::nullopt;
        if (data.stringFacts(method.name) & Data::stringIsPrivateName || !isName(method.name, name))
            continue;
        if (static_cast<Format::MethodKind>(method.kind) != Format::MethodKind::Method)
            return std::nullopt;
        found = &method;
    }
    if (!found && name == vm.propertyNames->constructor.impl())
        return closure(vm, ofClass.classData().classConstructor);
    return found ? closureObjectFor(vm, found->function) : nullptr;
}

std::optional<State::LazyPrototypeLookupResult> State::findThroughLazyPrototypesByString(VM& vm, JSObject* base, UniquedStringImpl* name, uint32_t stringPlusOne, bool baseIsKnownMissing)
{
    Structure* structure = base->structure();
    JSObject* prototype = structure->hasMonoProto() ? structure->storedPrototypeObject() : nullptr;
    if (!prototype || (prototype->classInfo() != ClassPrototype::info() && !baseIsKnownMissing) || name == vm.propertyNames->underscoreProto.impl())
        return std::nullopt;
    bool structureGuaranteesAbsent = baseIsKnownMissing ? true : structure->compiledLayout() ? Object::structureGuaranteesAbsence(structure, name)
        : base->type() == FinalObjectType && !structure->isDictionary() && structure->propertyAccessesAreCacheable() && structure->propertyAccessesAreCacheableForAbsence() && !structure->typeInfo().overridesGetOwnPropertySlot() && !isValidOffset(structure->get(vm, name));
    if (!structureGuaranteesAbsent || !stringPlusOne)
        return std::nullopt;
    for (; prototype && prototype->classInfo() == ClassPrototype::info() && prototype->structure()->aotInstance() == &instance && !prototype->butterfly(); prototype = prototype->getPrototypeDirect().getObject()) {
        const LayoutData& ofClass = *classWithIndex(uncheckedDowncast<ClassPrototype>(prototype)->classIndex());
        const MethodData* method = nullptr;
        const MethodData* getter = nullptr;
        const MethodData* setter = nullptr;
        for (auto& member : ofClass.methods()) {
            if (member.flags & methodHasComputedName)
                return std::nullopt;
            if (member.name != stringPlusOne - 1)
                continue;
            auto kind = static_cast<Format::MethodKind>(member.kind);
            (kind == Format::MethodKind::Method ? method : kind == Format::MethodKind::Getter ? getter : setter) = &member;
        }
        if (method && (getter || setter))
            return std::nullopt;
        if (name == vm.propertyNames->constructor.impl() && !method && !getter && !setter)
            return LazyPrototypeLookupResult { declaredMemberLocation(vm, ofClass.classData().classConstructor, nullptr, nullptr), nullptr };
        if (Options::validateAOTTypedFacts() && !getter && !setter) [[unlikely]]
            RELEASE_ASSERT(declaredMethod(vm, ofClass, name) == std::optional<JSObject*>(method ? closureObjectFor(vm, method->function) : nullptr));
        if (method || getter || setter)
            return LazyPrototypeLookupResult { declaredMemberLocation(vm, method ? std::optional(method->function) : std::nullopt, getter, setter), nullptr };
    }
    if (prototype && (prototype->classInfo() == ClassPrototype::info() || prototype->structure()->compiledLayout()))
        return std::nullopt;
    return LazyPrototypeLookupResult { std::nullopt, prototype };
}

State::DeclaredMemberLocation State::declaredMemberLocation(VM& vm, std::optional<uint32_t> method, const MethodData* getter, const MethodData* setter)
{
    uint32_t function = method ? *method : (getter ? getter : setter)->function;
    uint32_t place = declaredMemberLocations.size();
    if (auto known = declaredMemberLocations.find(function); known != declaredMemberLocations.end())
        place = known->value;
    else {
        if (place == declaredMemberHolders.size() * declaredMembersPerHolder) {
            Structure* structure = place ? declaredMemberHolders[0]->structure() : JSFinalObject::createStructure(vm, instance.globalObject, jsNull(), declaredMembersPerHolder);
            declaredMemberHolders.append(constructEmptyObject(vm, structure));
            didCreate(declaredMemberHolders.last());
        }
        JSCell* member = method ? closureObjectFor(vm, function) : nullptr;
        if (!method) {
            JSObject* get = getter ? closureObjectFor(vm, getter->function) : nullptr;
            JSObject* set = setter ? closureObjectFor(vm, setter->function) : nullptr;
            member = GetterSetter::create(vm, instance.globalObject, get, set);
            didCreate(member);
        }
        declaredMemberHolders.last()->putDirectOffset(vm, place % declaredMembersPerHolder, member);
        declaredMemberLocations.add(function, place);
    }
    return { declaredMemberHolders[place / declaredMembersPerHolder], static_cast<PropertyOffset>(place % declaredMembersPerHolder), !method };
}

auto State::collectDeclaredMemberEntries(VM& vm, const LayoutData& ofClass, MarkedArgumentBuffer& liveOwners) -> Vector<DeclaredMemberEntries>
{
    Vector<DeclaredMemberEntries> result;
    if (declaredMemberLocations.isEmpty())
        return result;
    PropertyLookupTable& table = PropertyLookupTable::of(vm);
    auto add = [&](uint32_t function, UniquedStringImpl* uid) {
        auto place = declaredMemberLocations.find(function);
        if (place == declaredMemberLocations.end())
            return;
        auto entries = table.entriesReferencing(uid, declaredMemberHolders[place->value / declaredMembersPerHolder], static_cast<PropertyOffset>(place->value % declaredMembersPerHolder));
        if (entries.owners.isEmpty())
            return;
        for (Structure* owner : entries.owners)
            liveOwners.append(owner);
        result.append({ uid, entries.name, WTF::move(entries.owners) });
    };
    add(ofClass.classData().classConstructor, vm.propertyNames->constructor.impl());
    for (auto& member : ofClass.methods()) {
        if (member.flags & methodHasComputedName)
            continue;
        if (const Identifier& name = nameIfCreated(member.name); !name.isNull())
            add(member.function, name.impl());
    }
    return result;
}

void State::repointDeclaredMemberEntries(VM& vm, JSObject* prototype, const Vector<DeclaredMemberEntries>& members)
{
    if (members.isEmpty())
        return;
    Structure* structure = prototype->structure();
    bool mayEnter = !structure->isDictionary() && structure->propertyAccessesAreCacheable();
    PropertyLookupTable& table = PropertyLookupTable::of(vm);
    for (auto& member : members) {
        unsigned attributes = 0;
        PropertyOffset offset = mayEnter ? structure->get(vm, member.uid, attributes) : invalidOffset;
        bool isPlain = isValidOffset(offset) && !(attributes & PropertyAttribute::CustomAccessorOrValue);
        for (Structure* owner : member.owners) {
            bool isEntered = isPlain && (attributes & PropertyAttribute::Accessor ? table.setAccessor(owner, member.nameInTable, prototype, offset, member.uid) : table.setInherited(owner, member.nameInTable, prototype, offset, member.uid));
            if (!isEntered)
                table.forget(owner, member.nameInTable);
        }
    }
}

GetterSetter* State::declaredAccessor(VM& vm, const LayoutData& ofClass, UniquedStringImpl* name)
{
    const MethodData* getter = nullptr;
    const MethodData* setter = nullptr;
    for (auto& method : ofClass.methods()) {
        if (method.flags & methodHasComputedName)
            return nullptr;
        if (data.stringFacts(method.name) & Data::stringIsPrivateName || !isName(method.name, name))
            continue;
        auto kind = static_cast<Format::MethodKind>(method.kind);
        if (kind == Format::MethodKind::Method)
            getter = setter = nullptr;
        else
            (kind == Format::MethodKind::Getter ? getter : setter) = &method;
    }
    if (!getter && !setter)
        return nullptr;
    JSObject* get = getter ? closureObjectFor(vm, getter->function) : nullptr;
    JSObject* set = setter ? closureObjectFor(vm, setter->function) : nullptr;
    return GetterSetter::create(vm, instance.globalObject, get, set);
}

bool State::hasPrototypeMembers(const LayoutData& layout) const
{
    for (const LayoutData* next = &layout; next; next = baseOf(*next)) {
        if (!next->methods().empty())
            return true;
    }
    return false;
}

bool State::findPrivateMethod(VM& vm, const LayoutData& layout, UniquedStringImpl* name, Object::PrivateElement& element)
{
    std::pair<const LayoutData*, UniquedStringImpl*> key { &layout, name };
    if (auto found = m_privateMethods.find(key); found != m_privateMethods.end()) {
        element.kind = Object::PrivateElement::Kind::Method;
        element.value = closureObjectFor(vm, found->value);
        return true;
    }
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countOperation("a private method is searched for");
    for (const LayoutData* next = &layout; next; next = baseOf(*next)) {
        for (auto& method : next->methods()) {
            if (!next->isEvaluatedOnce || method.flags & methodHasComputedName || nameIfCreated(method.name).impl() != name)
                continue;
            switch (static_cast<Format::MethodKind>(method.kind)) {
            case Format::MethodKind::Method:
                element.kind = Object::PrivateElement::Kind::Method;
                element.value = closureObjectFor(vm, method.function);
                m_privateMethods.add(key, method.function);
                break;
            case Format::MethodKind::Getter:
                element.kind = Object::PrivateElement::Kind::Accessor;
                element.getter = closureObjectFor(vm, method.function);
                break;
            case Format::MethodKind::Setter:
                element.kind = Object::PrivateElement::Kind::Accessor;
                element.setter = closureObjectFor(vm, method.function);
                break;
            }
        }
        if (element.kind != Object::PrivateElement::Kind::None)
            return true;
    }
    return false;
}

void State::addPrivateMethods(JSGlobalObject* globalObject, JSObject* object, const LayoutData& ofClass)
{
    VM& vm = globalObject->vm();
    for (auto& method : ofClass.methods()) {
        if (method.flags & methodHasComputedName || !(data.stringFacts(method.name) & Data::stringIsPrivateName))
            continue;
        const Identifier& key = name(vm, method.name);
        if (isValidOffset(object->getDirectOffset(vm, key)))
            continue;
        Object::PrivateElement element;
        if (!findPrivateMethod(vm, ofClass, key.impl(), element))
            continue;
        if (element.kind == Object::PrivateElement::Kind::Method)
            object->putDirect(vm, key, element.value, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
        else
            object->putDirectNonIndexAccessor(vm, key, GetterSetter::create(vm, globalObject, element.getter, element.setter), PropertyAttribute::Accessor | PropertyAttribute::DontEnum);
    }
}

static constexpr uint64_t emptyFieldsHash = 0xcbf29ce484222325ull;

static uint64_t hashWithField(uint64_t hash, uint16_t field)
{
    return (hash ^ (field + 1)) * 0x100000001b3ull;
}

static uint64_t keyOf(uint64_t hash)
{
    return hash >> 1;
}

std::span<const uint16_t> State::fieldsInOrder(const LayoutData& layout, uint32_t order)
{
    if (order >= layout.numberOfOrders)
        return orderTableFor(layout).added[order - layout.numberOfOrders].span();
    const uint16_t* lengthAndFields = data.at<uint16_t>(data.at<uint32_t>(layout.ordersOffset)[order]);
    return { lengthAndFields + 1, lengthAndFields[0] };
}

bool State::leavesFieldsUnlisted(const LayoutData& layout, uint32_t orderWord)
{
    return layout.kind == LayoutKind::Instance && std::ranges::any_of(fieldsInOrder(layout, orderOf(orderWord)).subspan(countOf(orderWord)), [&](uint16_t key) {
        return isDefinedByConstructor(layout, key);
    });
}

std::optional<std::span<const State::FieldAsProperty>> State::propertiesInOrder(VM& vm, const LayoutData& layout, uint32_t orderWord, Object::SymbolKeys symbolKeys)
{
    if (orderOf(orderWord) == keysAreInLayoutOrder)
        return std::nullopt;
    auto& table = orderTableFor(layout);
    bool includesSymbolKeys = symbolKeys == Object::SymbolKeys::Include && hasFieldsWithSymbolKeys(layout);
    if (table.lastOrderWord == orderWord && !includesSymbolKeys) [[likely]]
        return table.lastProperties;
    auto& lists = includesSymbolKeys ? table.propertyListsWithSymbolKeys : table.propertyLists;
    auto remember = [&](std::span<const FieldAsProperty> properties) {
        if (includesSymbolKeys)
            return properties;
        table.lastOrderWord = orderWord;
        table.lastProperties = properties;
        return properties;
    };
    if (auto known = lists.find(orderWord); known != lists.end()) [[likely]]
        return remember(known->value.span());
    if (leavesFieldsUnlisted(layout, orderWord)) [[unlikely]]
        return std::nullopt;
    Vector<FieldAsProperty> list;
    for (uint16_t index : fieldsInOrder(layout, orderOf(orderWord)).first(countOf(orderWord))) {
        if (index >= layout.numberOfFields)
            return std::nullopt;
        auto& field = layout.fields()[index];
        if (hasStringKey(field) || (includesSymbolKeys && hasSymbolKey(field)))
            list.append({ name(vm, field.name).impl(), &field });
    }
    return remember(lists.add(orderWord, FixedVector<FieldAsProperty>(WTF::move(list))).iterator->value.span());
}

const State::KeysAsJSON* State::keysAsJSON(VM& vm, const LayoutData& layout, uint32_t orderWord)
{
    auto& table = orderTableFor(layout);
    if (table.lastKeysAsJSONOrderWord == orderWord) [[likely]]
        return table.lastKeysAsJSON;
    auto properties = propertiesInOrder(vm, layout, orderWord);
    if (!properties)
        return nullptr;
    auto remember = [&](const KeysAsJSON* keys) {
        table.lastKeysAsJSONOrderWord = orderWord;
        table.lastKeysAsJSON = keys;
        return keys;
    };
    auto result = table.keysAsJSON.add(orderWord, nullptr);
    if (!result.isNewEntry)
        return remember(result.iterator->value.get());
    auto made = makeUnique<KeysAsJSON>();
    for (auto& property : *properties) {
        if (!property.name->is8Bit())
            return remember(nullptr);
        auto name = property.name->span8();
        for (Latin1Character character : name) {
            if (character < 0x20 || character == '"' || character == '\\')
                return remember(nullptr);
        }
        uint32_t start = made->text.size();
        made->text.append(',');
        made->text.append('"');
        made->text.append(name);
        made->text.append('"');
        made->text.append(':');
        made->keys.append({ Object::readKindAndOffset(*property.field), static_cast<uint32_t>(property.field - layout.fields()), start, static_cast<uint32_t>(made->text.size()) - start });
    }
    for (unsigned i = 0; i < KeysAsJSON::copyStride; ++i)
        made->text.append(0);
    result.iterator->value = WTF::move(made);
    return remember(result.iterator->value.get());
}

Structure* State::plainObjectStructureWith(VM& vm, std::span<const FieldAsProperty> properties)
{
    JSGlobalObject* globalObject = instance.globalObject;
    Structure* structure = properties.size() <= JSFinalObject::defaultInlineCapacity ? globalObject->objectStructureForObjectConstructor()
        : properties.size() <= JSFinalObject::maxInlineCapacity ? globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, globalObject->objectPrototype(), properties.size()) : nullptr;
    PropertyOffset expected = 0;
    for (auto& property : properties) {
        if (!structure)
            break;
        PropertyOffset offset = invalidOffset;
        bool isOrdinaryName = !parseIndex(*property.name) && property.name != vm.propertyNames->underscoreProto.impl();
        structure = isOrdinaryName ? Structure::addPropertyTransition(vm, structure, PropertyName(property.name), 0, offset) : nullptr;
        if (structure && (offset != expected++ || structure->isDictionary()))
            structure = nullptr;
    }
    if (structure)
        didCreate(structure);
    return structure;
}

const State::PlainCopyExcludingPlan& State::plainCopyExcludingPlan(VM& vm, const LayoutData& layout, uint32_t orderWord, std::span<UniquedStringImpl* const> excluded)
{
    auto& plans = orderTableFor(layout).plainCopyExcludingPlans.add(orderWord, Vector<std::unique_ptr<PlainCopyExcludingPlan>, 1> { }).iterator->value;
    for (auto& plan : plans) {
        if (std::ranges::equal(plan->excluded, excluded, [](const RefPtr<UniquedStringImpl>& known, UniquedStringImpl* asked) { return known.get() == asked; }))
            return *plan;
    }
    auto plan = makeUnique<PlainCopyExcludingPlan>();
    for (UniquedStringImpl* name : excluded)
        plan->excluded.append(name);
    if (auto listed = propertiesInOrder(vm, layout, orderWord, Object::SymbolKeys::Include)) {
        Vector<FieldAsProperty> kept;
        for (auto& property : *listed) {
            if (std::ranges::find(excluded, property.name) == excluded.end())
                kept.append(property);
        }
        plan->properties = FixedVector<FieldAsProperty>(WTF::move(kept));
        plan->structure = plainObjectStructureWith(vm, plan->properties.span());
    }
    plans.append(WTF::move(plan));
    return *plans.last();
}

const State::PlainCopyPlan& State::plainCopyPlan(VM& vm, const LayoutData& layout, uint32_t orderWord)
{
    auto& table = orderTableFor(layout);
    if (table.lastPlainCopyOrderWord == orderWord) [[likely]]
        return table.lastPlainCopy;
    auto remember = [&](const PlainCopyPlan& plan) -> const PlainCopyPlan& {
        table.lastPlainCopyOrderWord = orderWord;
        table.lastPlainCopy = plan;
        return table.lastPlainCopy;
    };
    if (auto found = table.plainCopyPlans.find(orderWord); found != table.plainCopyPlans.end()) [[likely]]
        return remember(found->value);
    auto listed = propertiesInOrder(vm, layout, orderWord, Object::SymbolKeys::Include);
    std::span<const FieldAsProperty> properties = listed.value_or(std::span<const FieldAsProperty> { });
    Structure* structure = listed ? plainObjectStructureWith(vm, properties) : nullptr;
    return remember(table.plainCopyPlans.add(orderWord, PlainCopyPlan { properties, structure }).iterator->value);
}

JSCellButterfly* State::keysInOrder(VM& vm, const LayoutData& layout, uint32_t orderWord)
{
    if (orderOf(orderWord) == keysAreInLayoutOrder)
        return nullptr;
    if (JSCellButterfly* known = orderTableFor(layout).keyLists.get(orderWord))
        return known;
    if (leavesFieldsUnlisted(layout, orderWord)) [[unlikely]]
        return nullptr;
    Vector<uint16_t, 16> listed;
    for (uint16_t index : fieldsInOrder(layout, orderOf(orderWord)).first(countOf(orderWord))) {
        if (index >= layout.numberOfFields)
            return nullptr;
        if (hasStringKey(layout.fields()[index]))
            listed.append(index);
    }
    JSCellButterfly* list = JSCellButterfly::tryCreate(vm, CopyOnWriteArrayWithContiguous, listed.size());
    if (!list)
        return nullptr;
    for (unsigned i = 0; i < listed.size(); ++i)
        list->setIndex(vm, i, jsOwnedString(vm, name(vm, layout.fields()[listed[i]].name).string()));
    didCreate(list);
    orderTableFor(layout).keyLists.add(orderWord, list);
    return list;
}

State::OrderTable& State::orderTableFor(const LayoutData& layout)
{
    auto& recentLookup = m_recentOrderTables[layout.shape % m_recentOrderTables.size()];
    if (recentLookup.second && recentLookup.first == layout.shape) [[likely]]
        return *recentLookup.second;
    OrderTable* table = m_orderTables.get(layout.shape);
    if (!table) [[unlikely]] {
        auto created = makeUnique<OrderTable>();
        table = created.get();
        m_orderTables.add(layout.shape, table);
        m_createdOrderTables.append(WTF::move(created));
        RELEASE_ASSERT(layout.numberOfOrders < keysAreInLayoutOrder);
    }
    recentLookup = { layout.shape, table };
    return *table;
}

bool State::allExtraKeysHavePositions(const LayoutData& layout, uint32_t orderWord, JSObject* extraProperties)
{
    uint32_t order = orderOf(orderWord);
    if (order == keysAreInLayoutOrder)
        return true;
    OrderTable::ExtraKeysInOrder placed;
    if (order >= layout.numberOfOrders)
        placed = orderTableFor(layout).extraKeysInAddedOrders[order - layout.numberOfOrders];
    Structure* structure = extraProperties->structure();
    return countOf(orderWord) >= placed.end && placed.count == numberOfSlotsForMaxOffset(structure->maxOffset(), structure->inlineCapacity());
}

static uint64_t placementKey(uint32_t orderAndCount, uint16_t entry, uint32_t position)
{
    static_assert(orderAndCountMask < 1u << 28 && maxFieldsInOrder < 1u << 12);
    return static_cast<uint64_t>(orderAndCount) << 28 | static_cast<uint64_t>(entry) << 12 | std::min(position, maxFieldsInOrder);
}

std::optional<uint32_t> State::orderWithExtraKeyAt(const LayoutData& layout, uint32_t orderAndCount, uint16_t entry, uint32_t position)
{
    auto& placements = orderTableFor(layout).placements;
    auto found = placements.find(placementKey(orderAndCount, entry, position));
    return found == placements.end() ? std::nullopt : std::optional(found->value);
}

void State::didPositionExtraKey(const LayoutData& layout, uint32_t orderAndCount, uint16_t entry, uint32_t position, uint32_t result)
{
    orderTableFor(layout).placements.add(placementKey(orderAndCount, entry, position), result);
}

uint32_t State::orderStartingWith(const LayoutData& layout, std::span<const uint16_t> fields, bool registersPrefixes)
{
    static constexpr uint32_t noOrder = orderWordOf(keysAreInLayoutOrder, 0);
    if (fields.size() > maxFieldsInOrder)
        return noOrder;
    OrderTable& table = orderTableFor(layout);
    if (!table.hasExpectedOrderPrefixes) {
        table.hasExpectedOrderPrefixes = true;
        for (uint16_t order = 0; order < layout.numberOfOrders; ++order) {
            uint64_t hash = emptyFieldsHash;
            table.byPrefix.add(keyOf(hash), order);
            for (uint16_t field : fieldsInOrder(layout, order)) {
                hash = hashWithField(hash, field);
                table.byPrefix.add(keyOf(hash), order);
            }
        }
    }
    Vector<uint64_t, 16> hashes { emptyFieldsHash };
    for (uint16_t field : fields)
        hashes.append(hashWithField(hashes.last(), field));
    auto find = [&](size_t length) -> std::optional<uint16_t> {
        auto found = table.byPrefix.find(keyOf(hashes[length]));
        if (found == table.byPrefix.end())
            return std::nullopt;
        auto all = fieldsInOrder(layout, found->value);
        if (all.size() < length || !equalSpans(all.first(length), fields.first(length)))
            return std::nullopt;
        return found->value;
    };
    if (auto order = find(fields.size()))
        return orderWordOf(*order, fields.size());
    if (auto order = fields.empty() ? std::nullopt : find(fields.size() - 1); order && *order >= layout.numberOfOrders) {
        auto& all = table.added[*order - layout.numberOfOrders];
        if (all.size() + 1 == fields.size()) {
            all.append(fields.back());
            table.byPrefix.add(keyOf(hashes.last()), *order);
            didChangeOrder(layout, table, *order);
            return orderWordOf(*order, fields.size());
        }
    }
    uint32_t order = layout.numberOfOrders + table.added.size();
    if (order >= keysAreInLayoutOrder)
        return noOrder;
    table.added.append(Vector<uint16_t>(fields));
    for (uint64_t hash : registersPrefixes ? hashes.span() : hashes.span().last(1))
        table.byPrefix.add(keyOf(hash), static_cast<uint16_t>(order));
    didChangeOrder(layout, table, order);
    return orderWordOf(order, fields.size());
}

void State::didChangeOrder(const LayoutData& layout, OrderTable& table, uint32_t order)
{
    auto fields = fieldsInOrder(layout, order);
    OrderTable::ExtraKeysInOrder extraKeys;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i] >= layout.numberOfFields + extraKeys.count) {
            extraKeys.count = static_cast<uint16_t>(fields[i] - layout.numberOfFields + 1);
            extraKeys.end = static_cast<uint16_t>(i + 1);
        }
    }
    if (order >= layout.numberOfOrders) {
        if (size_t added = order - layout.numberOfOrders; added >= table.extraKeysInAddedOrders.size())
            table.extraKeysInAddedOrders.grow(added + 1);
        table.extraKeysInAddedOrders[order - layout.numberOfOrders] = extraKeys;
    }
    if (table.orderKeys.isEmpty())
        return;
    if (order >= table.slotsInOrder.size()) {
        table.slotsInOrder.grow(order + 1);
        table.orderKeys.grow(order + 2);
    }
    auto& slots = table.slotsInOrder[order];
    slots.shrink(0);
    slots.append(static_cast<uint16_t>(fields.size()));
    static_assert(!noSlotInCell);
    for (uint16_t index : fields)
        slots.append(index < layout.numberOfFields ? static_cast<uint16_t>(slotKeyFor(layout.fields()[index].offset, layout.fields()[index].flags & fieldIsOutOfLine)) : noSlotInCell);
    for (size_t i = 0; i < 2 * (fields.size() + 1); ++i)
        slots.append(noSlotInCell);
    slots.append(extraKeys.count);
    slots.append(extraKeys.end);
    table.orderKeys[0] = table.slotsInOrder.size();
    table.orderKeys[1 + order] = std::bit_cast<uintptr_t>(slots.span().data());
    instance.setTypedOrderKeys(layout.shape, table.orderKeys.mutableSpan().data());
}

void State::didFindTransition(const LayoutData& layout, OrderTable& table, uint32_t order, uint32_t count, uint16_t field, uint32_t next)
{
    if (order >= table.slotsInOrder.size() || next == order || next >= keysAreInLayoutOrder || field >= layout.numberOfFields)
        return;
    auto& created = layout.fields()[field];
    auto& slots = table.slotsInOrder[order];
    uint32_t key = slotKeyFor(created.offset, created.flags & fieldIsOutOfLine);
    if (!key || slots.isEmpty() || count > slots[0])
        return;
    RELEASE_ASSERT(slots.size() == 1u + slots[0] + 2u * (slots[0] + 1u) + 2u);
    size_t pair = 1 + slots[0] + 2 * count;
    slots[pair] = static_cast<uint16_t>(key);
    slots[pair + 1] = static_cast<uint16_t>(next);
}

const FieldData& State::fieldAt(const LayoutData& layout, uint32_t slotKey)
{
    OrderTable& table = orderTableFor(layout);
    if (table.orderKeys.isEmpty()) [[unlikely]] {
        table.orderKeys.append(0);
        instance.setTypedOrderKeys(layout.shape, table.orderKeys.mutableSpan().data());
        for (uint32_t order = 0; order < layout.numberOfOrders + table.added.size(); ++order)
            didChangeOrder(layout, table, order);
        table.fieldsBySlot.fill(UINT16_MAX, layout.size);
        table.fieldsByExtensionSlot.fill(UINT16_MAX, layout.extensionSize());
        for (uint32_t i = 0; i < layout.numberOfFields; ++i)
            (layout.fields()[i].flags & fieldIsOutOfLine ? table.fieldsByExtensionSlot : table.fieldsBySlot)[layout.fields()[i].offset] = static_cast<uint16_t>(i);
    }
    uint16_t index = slotKey & outOfLineSlotKey ? table.fieldsByExtensionSlot[slotKey & ~outOfLineSlotKey] : table.fieldsBySlot[slotKey];
    RELEASE_ASSERT(index < layout.numberOfFields);
    return layout.fields()[index];
}

uint32_t State::orderInSpecialization(VM& vm, const LayoutData& from, const LayoutData& to, uint32_t orderWord)
{
    if (orderOf(orderWord) == keysAreInLayoutOrder)
        return orderWordOf(keysAreInLayoutOrder, 0);
    Vector<uint16_t, 16> order;
    for (uint16_t index : fieldsInOrder(from, orderOf(orderWord)).first(countOf(orderWord))) {
        if (index < from.numberOfFields)
            order.append(static_cast<uint16_t>(field(vm, to, name(vm, from.fields()[index].name).impl()) - to.fields()));
        else if (uint32_t entry = to.numberOfFields + (index - from.numberOfFields); entry < UINT16_MAX)
            order.append(static_cast<uint16_t>(entry));
    }
    return orderStartingWith(to, order.span());
}

uint32_t State::orderAfterCreating(const LayoutData& layout, uint32_t orderWord, uint16_t field)
{
    uint32_t order = orderOf(orderWord);
    uint32_t count = countOf(orderWord);
    auto counted = [&](uint32_t word) {
        if (m_counts) [[unlikely]]
            ++m_counts->orderStates.add({ layout.shape, word }, 0).iterator->value;
        return word;
    };
    if (order == keysAreInLayoutOrder)
        return counted(orderWord & orderAndCountMask);
    auto fields = fieldsInOrder(layout, order);
    if (count < fields.size() && fields[count] == field) [[likely]]
        return counted(orderWordOf(order, count + 1));
    OrderTable& table = orderTableFor(layout);
    uint64_t key = static_cast<uint64_t>(order) << 32 | count << 16 | field;
    if (auto found = table.transitions.find(key); found != table.transitions.end()) {
        didFindTransition(layout, table, order, count, field, found->value);
        return counted(orderWordOf(found->value, found->value == keysAreInLayoutOrder ? 0 : count + 1));
    }
    Vector<uint16_t, 16> next(fields.first(count));
    next.append(field);
    uint32_t result = orderStartingWith(layout, next.span());
    table.transitions.add(key, static_cast<uint16_t>(orderOf(result)));
    didFindTransition(layout, table, order, count, field, orderOf(result));
    return counted(result);
}

ALWAYS_INLINE std::optional<bool> State::mayConvertNestedValue(JSValue value, const RuntimeType& ofField, bool leavesUntypedObjects) const
{
    if (!value.isObject())
        return false;
    JSObject* below = asObject(value);
    switch (below->type()) {
    case FinalObjectType:
        if (leavesUntypedObjects || matchesTypedSubtype(ofField, below))
            return false;
        return std::nullopt;
    case ObjectType:
        if (Object* compiled = compiledObject(below); compiled && !layoutForShape(compiled->shape()).numberOfSpecializations())
            return false;
        return std::nullopt;
    default:
        return false;
    }
}

static bool isPlainDataIgnoringIntegrity(JSObject* object)
{
    if (object->type() != FinalObjectType)
        return false;
    JSValue prototype = object->getPrototypeDirect();
    return prototype.isNull() || prototype == JSValue(object->realm()->objectPrototype());
}

static bool isPlainData(JSObject* object)
{
    return object->isStructureExtensible() && isPlainDataIgnoringIntegrity(object);
}

static std::optional<Object::Integrity> plainDataIntegrity(VM& vm, JSObject* object)
{
    Structure* structure = object->structure();
    if (structure->isStructureExtensible())
        return std::nullopt;
    return structure->isFrozen(vm) ? Object::Integrity::Frozen : structure->isSealed(vm) ? Object::Integrity::Sealed : Object::Integrity::NotExtensible;
}

static unsigned dataAttributesFor(std::optional<Object::Integrity> integrity)
{
    if (!integrity || *integrity == Object::Integrity::NotExtensible)
        return 0;
    return *integrity == Object::Integrity::Frozen ? PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete : static_cast<unsigned>(PropertyAttribute::DontDelete);
}

class Coercion {
public:
    enum class Mode : uint8_t { Cast, Narrowing, Coerce, CoerceFreshObject, CoerceFreshGraph, Test };
    enum class Completeness : uint8_t { MayLackAddedFields, HasAddedFields };
    enum class Explains : bool { No, Yes };

    Coercion(JSGlobalObject* globalObject, State& state, Mode mode = Mode::Cast, Completeness completeness = Completeness::HasAddedFields, Explains explains = Explains::Yes)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_state(state)
        , m_mode(mode)
        , m_completeness(completeness)
        , m_explains(explains == Explains::Yes)
    {
    }

    JSValue run(JSValue value, const RuntimeType& type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue result = coerce(value, type);
        RETURN_IF_EXCEPTION(scope, { });
        if (!result)
            return { };
        if (JSObject* copied = sharedObjectThatWouldBeCopied(); copied && (m_disallowsCopies || m_state.rejectsCopiesOfSharedObjects())) {
            if (staysBecauseNotConvertible(copied, type, value))
                return value;
            return failToCopy(copied, type, value);
        }
        commit();
        result = settled(result);
        RELEASE_ASSERT(m_state.fits(type, result) || type.acceptsOnlyOtherLayouts());
        bool hasCopiedSharedObject = m_copied.containsIf([&](JSObject* object) {
            return object != m_unsharedObject && (m_isOrdinaryCopy.contains(object) || m_convertedInPlace.isEmpty());
        });
        if (hasCopiedSharedObject && m_mode != Mode::CoerceFreshObject && m_mode != Mode::CoerceFreshGraph) [[unlikely]] {
            StringBuilder parts;
            for (auto& [original, ordinary] : m_ordinaryCopies) {
                if (Object* copy = m_inlinedCopies.get(ordinary))
                    parts.append(parts.isEmpty() ? "copied: "_s : ", "_s, State::describe(JSValue(original)), " as "_s, State::describe(JSValue(copy)));
            }
            String reason = parts.toString();
            m_state.recordViolation(m_vm, { .kind = "copy-on-enter"_s, .what = m_mode == Mode::Coerce ? "Coerce"_s : m_mode == Mode::Narrowing ? "Flow"_s : "As"_s, .expected = &type, .got = value, .reason = reason.isEmpty() ? nullptr : &reason, .action = State::ActionOnViolation::Copied });
        }
        return result;
    }

    void allowInPlaceConversion() { m_allowsInPlaceConversion = true; }
    void allowCopyingProgramObjects() { m_copiesProgramObjects = true; }
    void disallowCopies() { m_disallowsCopies = true; }
    void allowTransitionsToMembers() { m_mayConvertToSubtypes = true; }
    void setUnsharedObject(JSObject* object)
    {
        m_unsharedObject = object;
        m_isOrdinaryCopy.add(object);
    }

    bool run(Object* object, const LayoutData& layout, UniquedStringImpl* stored)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!canConvertTo(object, layout, stored))
            return false;
        JSValue result = specializeTo(object, layout);
        RETURN_IF_EXCEPTION(scope, false);
        if (!result)
            return false;
        commit();
        return true;
    }

    const String& failure() const { return m_failure; }

private:
    bool isCoercion() const { return m_mode == Mode::Coerce || m_mode == Mode::CoerceFreshObject || m_mode == Mode::CoerceFreshGraph; }

    JSValue settled(JSValue value) const
    {
        if (m_convertedInPlace.isEmpty() || !value || !value.isCell())
            return value;
        JSObject* object = m_convertedInPlace.get(value.asCell());
        return object ? JSValue(object) : value;
    }

    JSObject* sharedObjectThatWouldBeCopied()
    {
        if (m_mode == Mode::CoerceFreshObject || m_mode == Mode::CoerceFreshGraph)
            return nullptr;
        bool convertInPlaceAll = allCopiedHaveCapacity();
        JSObject* first = nullptr;
        for (JSObject* object : m_copied) {
            bool isProgramObject = m_isOrdinaryCopy.contains(object);
            if (m_copiedForLackOfCapacity.contains(object))
                return object;
            if (object == m_unsharedObject || (!isProgramObject && convertInPlaceAll))
                continue;
            if (isProgramObject || !m_state.hasCapacityToConvertInPlace(object, m_inlinedCopies.get(object)))
                return object;
            if (!first)
                first = object;
        }
        return first;
    }

    void recordCopyPath(JSObject* object)
    {
        if (!m_explains || m_path.isEmpty())
            return;
        StringBuilder path;
        for (UniquedStringImpl* name : m_path)
            path.append(path.isEmpty() ? ""_s : "."_s, StringView(name));
        m_copyPaths.add(object, path.toString());
    }

    JSValue failToCopy(JSObject* copied, const RuntimeType& type, JSValue cast)
    {
        if (!m_explains || !m_failure.isNull())
            return { };
        Object* copy = m_inlinedCopies.get(copied);
        bool isBelow = copy && cast != JSValue(copied) && !m_isOrdinaryCopy.contains(copied);
        String name = isBelow ? m_state.nameOf(copy->layout()) : m_state.describe(type);
        String why = m_isOrdinaryCopy.contains(copied) && !m_copiedForLackOfCapacity.contains(copied) ? "TypeScript code of the program may use it as another declared type"_str : copied->structure()->mayBePrototype() ? "it is the prototype of another object"_str : makeString(name, " has more fields than the object has space for"_s);
        String path = isBelow ? m_copyPaths.get(copied) : String();
        m_failure = makeString(!isBelow ? "the object matches "_str : path.isEmpty() ? "an object it holds matches "_str : makeString("property '"_s, path, "' holds an object that matches "_s), name, ", but it cannot be converted in place ("_s, why, "), and a copy would not see later changes made through other references to it\n  convert the data where it enters the program, before anything else refers to it ('JSON.parse(text) as "_s, name, "'), or declare this place as 'unknown' and check it"_s);
        return { };
    }

    bool allCopiedHaveCapacity()
    {
        return m_allowsInPlaceConversion && std::ranges::all_of(m_copied, [&](JSObject* object) {
            return m_isOrdinaryCopy.contains(object) || m_state.hasCapacityToConvertInPlace(object, m_inlinedCopies.get(object));
        });
    }

    void convertObjectsInPlace()
    {
        if (!allCopiedHaveCapacity())
            return;
        for (JSObject* object : m_copied) {
            if (m_isOrdinaryCopy.contains(object))
                continue;
            Object* copy = m_inlinedCopies.get(object);
            m_state.become(m_vm, object, copy);
            m_convertedInPlace.add(copy, object);
        }
        for (JSObject* object : m_copied) {
            Object* holder = m_isOrdinaryCopy.contains(object) ? m_inlinedCopies.get(object) : uncheckedDowncast<Object>(object);
            const LayoutData& layout = holder->layout();
            for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
                const FieldData& field = layout.fields()[i];
                JSValue held = holdsCells(field.store()) ? holder->read(field) : JSValue();
                if (JSValue now = settled(held); now != held) {
                    bool didStore = holder->store(m_vm, field, now);
                    RELEASE_ASSERT(didStore);
                }
            }
        }
    }

    void commit()
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        convertObjectsInPlace();
        for (auto& specialization : m_specializations) {
            Object* object = specialization.object;
            object->specialize(m_vm, m_state, specialization.shape);
            const LayoutData& layout = object->layout();
            auto& moved = specialization.movedIntoFields;
            if (!moved.isEmpty()) {
                JSObject* extra = object->extraProperties();
                size_t next = 0;
                for (auto& property : propertiesOf(extra)) {
                    const FieldData* field = next < moved.size() ? &layout.fields()[moved[next].first] : nullptr;
                    if (!field || m_state.name(m_vm, field->name).impl() != property.name) {
                        object->positionExtraKey(m_state, property.name, UINT32_MAX);
                        continue;
                    }
                    object->didMoveExtraPropertyToField(m_state, *field);
                    bool didStore = object->store(m_vm, *field, settled(moved[next++].second));
                    RELEASE_ASSERT(didStore);
                    DeletePropertySlot slot;
                    extra->methodTable()->deleteProperty(extra, m_globalObject, property.name, slot);
                    scope.assertNoException();
                }
                RELEASE_ASSERT(next == moved.size());
            }
            object->dropExtraPropertiesIfEmpty(m_vm);
        }
        for (auto& store : m_propertyStores)
            store.object->putDirect(m_vm, store.name, settled(store.value));
    }

    struct Property {
        UniquedStringImpl* name;
        PropertyOffset offset;
        unsigned attributes;
    };
    using Properties = Vector<Property, 16>;
    struct Specialization {
        Object* object;
        uint32_t shape;
        Vector<std::pair<uint16_t, JSValue>, 1> movedIntoFields;
    };
    struct PropertyStore {
        JSObject* object;
        UniquedStringImpl* name;
        JSValue value;
    };
    struct Distance {
        unsigned otherLiterals { 0 };
        unsigned extraProperties { 0 };

        friend auto operator<=>(const Distance&, const Distance&) = default;
    };
    uint32_t shapeOf(Object* object) const
    {
        auto planned = m_plannedShapes.find(object);
        return planned == m_plannedShapes.end() ? object->shape() : planned->value;
    }

    bool fits(JSValue value, const RuntimeType& type) const
    {
        if (Object* object = m_state.compiledObject(value))
            return type.allowsAnyObject() || m_state.acceptsShape(type, shapeOf(object));
        return value.isObject() ? m_state.matchesTypedSubtype(type, asObject(value)) : m_state.fits(type, value);
    }

    JSValue orUnchanged(JSValue value, bool mayStayUnchanged, const Invocable<JSValue()> auto& attempt)
    {
        if (!mayStayUnchanged)
            return attempt();
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        size_t numberOfCopies = m_copied.size();
        size_t numberOfSpecializations = m_specializations.size();
        size_t keptObjectCount = m_keptInOrder.size();
        size_t numberOfPropertyStores = m_propertyStores.size();
        size_t lengthOfPath = m_path.size();
        JSValue result = attempt();
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
        for (size_t i = numberOfCopies; i < m_copied.size(); ++i)
            m_inlinedCopies.remove(m_copied[i]);
        m_copied.shrink(numberOfCopies);
        m_specializations.shrink(numberOfSpecializations);
        m_plannedShapes.clear();
        for (auto& specialization : m_specializations)
            m_plannedShapes.set(specialization.object, specialization.shape);
        for (size_t i = keptObjectCount; i < m_keptInOrder.size(); ++i)
            m_kept.remove(m_keptInOrder[i]);
        m_keptInOrder.shrink(keptObjectCount);
        m_propertyStores.shrink(numberOfPropertyStores);
        m_path.shrink(lengthOfPath);
        m_failure = String();
        return value;
    }

    String subject() const
    {
        if (m_path.isEmpty())
            return "object"_s;
        StringBuilder path;
        for (UniquedStringImpl* name : m_path)
            path.append(path.isEmpty() ? ""_s : "."_s, StringView(name));
        return makeString("property '"_s, path.toString(), '\'');
    }

    JSValue fail(const Invocable<String()> auto& predicate)
    {
        if (m_explains && m_failure.isNull())
            m_failure = makeString(subject(), ' ', predicate());
        return { };
    }

    JSValue failCoercion(JSValue value, const RuntimeType& type)
    {
        if (!m_explains || !m_failure.isNull())
            return { };
        m_failure = m_path.isEmpty() ? emptyString() : makeString(subject(), ": expected "_s, m_state.describe(type), ", got "_s, State::describe(value));
        return { };
    }

    static ASCIILiteral describeAttributes(unsigned attributes)
    {
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
            return "is an accessor"_s;
        if (attributes & PropertyAttribute::ReadOnly)
            return "is read only"_s;
        return attributes & PropertyAttribute::DontEnum ? "is not enumerable"_s : "is not configurable"_s;
    }

    JSValue coerce(JSValue value, const RuntimeType& type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!value.isObject())
            return m_state.fits(type, value) ? value : failCoercion(value, type);
        if (!m_vm.isSafeToRecurse()) [[unlikely]] {
            throwStackOverflowError(m_globalObject, scope);
            return { };
        }
        if (Object* object = m_state.compiledObject(value)) {
            bool isOfType = fits(value, type);
            if (isOfType && (m_mode == Mode::Test || !shouldSpecialize(object, type)))
                return value;
            if (m_mode != Mode::Test && m_state.layoutForShape(shapeOf(object)).numberOfSpecializations()) {
                JSValue result = orUnchanged(value, isOfType || type.acceptsUntypedObjectsByNature() || staysIfCopyWouldMatch(object, type) || mayCopyProgramObject(object), [&] {
                    SetForScope copiesOnlyWhenNeeded(m_copiesProgramObjects, m_copiesProgramObjects && !isOfType && !type.acceptsUntypedObjectsByNature());
                    return specialize(object, type);
                });
                RETURN_IF_EXCEPTION(scope, { });
                if (!result || fits(result, type))
                    return result;
            }
            bool stays = staysIfCopyWouldMatch(object, type);
            if (type.acceptsUntypedObjectsByNature() || (!stays && !mayCopyProgramObject(object)))
                return type.acceptsUntypedObjectsByNature() ? value : failCoercion(value, type);
            if (stays && m_completeness == Completeness::MayLackAddedFields && m_lackedCapacity.contains(object))
                return value;
            if (stays && !m_explains) {
                if (auto matchesByName = matchesWhenReadByName(object, type))
                    return *matchesByName ? value : JSValue();
            }
            JSObject* ordinary = m_ordinaryCopies.get(object);
            if (!ordinary) {
                ordinary = object->copyAsOrdinaryObject(m_vm, JSFinalObject::defaultInlineCapacity);
                m_copiesKeptAlive.append(ordinary);
                m_ordinaryCopies.add(object, ordinary);
                m_isOrdinaryCopy.add(ordinary);
                if (m_lackedCapacity.contains(object) && object != m_unsharedObject)
                    m_copiedForLackOfCapacity.add(ordinary);
            }
            if (Object* copy = m_inlinedCopies.get(ordinary)) {
                if (stays)
                    return value;
                return fits(copy, type) ? JSValue(copy) : failCoercion(copy, type);
            }
            if (!stays)
                RELEASE_AND_RETURN(scope, copy(ordinary, type));
            bool copyMatches = false;
            String failure;
            {
                SetForScope asCast(m_mode, Mode::Cast);
                SetForScope disableFurtherCopies(m_copiesProgramObjects, false);
                orUnchanged(value, true, [&]() -> JSValue {
                    copyMatches = !!copy(ordinary, type);
                    failure = m_failure;
                    return { };
                });
            }
            RETURN_IF_EXCEPTION(scope, { });
            if (copyMatches)
                return value;
            m_failure = failure;
            return { };
        }
        ClassPrototype::look(asObject(value));
        if (m_mode == Mode::Narrowing)
            return (type.checksUntypedObjects() ? fits(value, type) : m_state.fits(type, value)) ? value : failCoercion(value, type);
        if (fits(value, type))
            return value;
        bool isCheckOnlyCast = m_mode == Mode::Test || m_mode == Mode::Coerce || (m_mode == Mode::CoerceFreshObject && !m_copied.isEmpty());
        JSObject* object = asObject(value);
        if (Object* copy = m_inlinedCopies.get(object))
            return fits(copy, type) || type.acceptsUntypedObjectsByNature() ? JSValue(copy) : failCoercion(copy, type);
        if (!type.acceptsUntypedObjectsByNature() && isCheckOnlyCast)
            return failCoercion(value, type);
        if (type.acceptsUntypedObjectsByNature() && object == m_unsharedObject && m_mode == Mode::Cast) {
            JSValue made = orUnchanged(value, true, [&] {
                SetForScope copiesOnlyWhenNeeded(m_copiesProgramObjects, false);
                return copy(object, type);
            });
            RETURN_IF_EXCEPTION(scope, { });
            if (made != value)
                return made;
        }
        if (type.acceptsUntypedObjectsByNature() && (isCheckOnlyCast || !isPlainObject(object) || !mayConvertProperties(type)))
            return value;
        RELEASE_AND_RETURN(scope, orUnchanged(value, type.acceptsUntypedObjectsByNature() || mayStayUnconverted(object, type), [&] {
            SetForScope copiesOnlyWhenNeeded(m_copiesProgramObjects, m_copiesProgramObjects && !type.acceptsUntypedObjectsByNature());
            return type.acceptsUntypedObjectsByNature() ? keep(object, type) : copy(object, type);
        }));
    }

    bool mayStayUnconverted(JSObject* object, const RuntimeType& type)
    {
        if (!type.acceptsOnlyOtherLayouts() || m_mode != Mode::Cast)
            return false;
        Structure* structure = object->structure();
        bool isAllPlainData = isPlainData(object) && !structure->hasAnyKindOfGetterSetterProperties() && !structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() && !structure->hasNonEnumerableProperties() && !structure->hasNonConfigurableProperties();
        return !isAllPlainData && maySatisfyWhenReadByName(object, type);
    }

    bool maySatisfyWhenReadByName(JSObject* object, const RuntimeType& type)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        bool result = false;
        forEachInstantiableLayout(type, [&](const LayoutData& candidate) {
            for (uint32_t i = 0; i < candidate.numberOfFields; ++i) {
                const FieldData& field = candidate.fields()[i];
                PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
                bool isFound = object->getPropertySlot(m_globalObject, m_state.name(m_vm, field.name), slot);
                scope.assertNoException();
                if (slot.isTaintedByOpaqueObject())
                    continue;
                if (!isFound) {
                    if (field.flags & fieldIsOptional)
                        continue;
                    return IterationStatus::Continue;
                }
                if (!slot.isValue())
                    continue;
                JSValue held = slot.getValue(m_globalObject, m_state.name(m_vm, field.name));
                if (!held.isObject() && !fits(held, m_state.ownTypeOf(field)))
                    return IterationStatus::Continue;
            }
            result = true;
            return IterationStatus::Done;
        });
        return result;
    }

    std::optional<bool> matchesWhenReadByName(Object* object, const RuntimeType& type)
    {
        if (object->orderWord() & (orderWordIsFrozen | orderWordIsSealed | orderWordIsNotExtensible))
            return std::nullopt;
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isNull() && prototype != JSValue(object->realm()->objectPrototype()))
            return std::nullopt;
        const LayoutData& own = m_state.layoutForShape(shapeOf(object));
        JSObject* extra = object->extraProperties();
        if (extra && hasIndexedProperties(extra->indexingType()))
            return std::nullopt;
        std::optional<bool> result = false;
        forEachInstantiableLayout(type, [&](const LayoutData& candidate) {
            bool matchesCandidate = true;
            for (uint32_t i = 0; i < candidate.numberOfFields && matchesCandidate; ++i) {
                const FieldData& field = candidate.fields()[i];
                UniquedStringImpl* name = m_state.name(m_vm, field.name).impl();
                JSValue held;
                if (const FieldData* ownField = m_state.field(m_vm, own, name))
                    held = object->read(*ownField);
                else if (extra) {
                    unsigned attributes = 0;
                    held = extra->getDirect(m_vm, name, attributes);
                    if (held && attributes) {
                        result = std::nullopt;
                        return IterationStatus::Done;
                    }
                }
                if (!held) {
                    matchesCandidate = field.flags & fieldIsOptional;
                    continue;
                }
                if (fits(held, m_state.ownTypeOf(field)))
                    continue;
                if (held.isObject()) {
                    result = std::nullopt;
                    return IterationStatus::Done;
                }
                matchesCandidate = false;
            }
            if (!matchesCandidate)
                return IterationStatus::Continue;
            result = true;
            return IterationStatus::Done;
        });
        if (!result || *result)
            return result;
        for (uint32_t i = 0; i < type.numberOfSupertypes(); ++i) {
            auto& view = type.view(i);
            if (view.supertypeShape || !view.fields)
                continue;
            bool matchesSupertype = true;
            for (auto& declared : m_state.data.at<SupertypeFields>(view.fields)->decode(m_state.data.at<uint32_t>(m_state.data.typesOffset))) {
                UniquedStringImpl* name = m_state.name(m_vm, declared.name).impl();
                JSValue held;
                if (const FieldData* ownField = m_state.field(m_vm, own, name))
                    held = object->read(*ownField);
                else if (extra) {
                    unsigned attributes = 0;
                    held = extra->getDirect(m_vm, name, attributes);
                    if (held && attributes)
                        return std::nullopt;
                }
                if (!held) {
                    matchesSupertype = declared.isOptional;
                    if (!matchesSupertype)
                        break;
                    continue;
                }
                if (fits(held, *m_state.data.at<RuntimeType>(declared.type)))
                    continue;
                if (held.isObject())
                    return std::nullopt;
                matchesSupertype = false;
                break;
            }
            if (matchesSupertype)
                return true;
        }
        return result;
    }

    bool staysBecauseNotConvertible(JSObject* copied, const RuntimeType& type, JSValue value) const
    {
        return type.acceptsOnlyOtherLayouts() && value.isObject() && !m_state.compiledObject(value) && !m_isOrdinaryCopy.contains(copied);
    }

    bool staysIfCopyWouldMatch(Object* object, const RuntimeType& type) const
    {
        return type.acceptsOnlyOtherLayouts() && m_state.layoutForShape(shapeOf(object)).kind == LayoutKind::Record;
    }

    bool mayCopyProgramObject(Object* object) const
    {
        return m_copiesProgramObjects && m_mode == Mode::Cast && m_state.layoutForShape(shapeOf(object)).kind == LayoutKind::Record;
    }

    bool shouldSpecialize(Object* object, const RuntimeType& type) const
    {
        uint32_t shape = shapeOf(object);
        const LayoutData& layout = m_state.layoutForShape(shape);
        if (!layout.numberOfSpecializations() || type.allowsAnyObject())
            return false;
        std::span specializations { m_state.data.at<uint32_t>(layout.specializationsOffset()), layout.numberOfSpecializations() };
        for (uint32_t i = 0; i < type.numberOfSupertypes(); ++i) {
            auto& view = type.view(i);
            if (m_state.passes(view, shape) && !(view.supertypeShape && std::ranges::binary_search(specializations, view.supertypeShape)))
                return false;
        }
        return true;
    }

    bool mayBeCoerced(JSValue value, const RuntimeType& type)
    {
        if (fits(value, type))
            return true;
        if (!value.isObject())
            return false;
        if (type.acceptsUntypedObjectsByNature())
            return true;
        if (Object* object = m_state.compiledObject(value)) {
            bool hasSpecialization = false;
            forEachSpecialization(object, type, [&](const LayoutData&) {
                hasSpecialization = true;
                return IterationStatus::Done;
            });
            if (staysIfCopyWouldMatch(object, type))
                return matchesWhenReadByName(object, type).value_or(true);
            if (hasSpecialization || !m_expectsProgramObjectCopies || !mayCopyProgramObject(object))
                return hasSpecialization;
            forEachInstantiableLayout(type, [&](const LayoutData&) {
                hasSpecialization = true;
                return IterationStatus::Done;
            });
            return hasSpecialization;
        }
        if (m_mode == Mode::Narrowing)
            return false;
        if (Object* copy = m_inlinedCopies.get(asObject(value)))
            return fits(copy, type);
        if (asObject(value)->type() != FinalObjectType)
            return false;
        if (m_checksNestedValues && isPlainDataIgnoringIntegrity(asObject(value))) {
            SetForScope oneLess(m_checksNestedValues, m_checksNestedValues - 1);
            return chooseLayout(asObject(value), propertiesOf(asObject(value)), type, false);
        }
        bool hasLayout = false;
        forEachInstantiableLayout(type, [&](const LayoutData&) {
            hasLayout = true;
            return IterationStatus::Done;
        });
        return hasLayout;
    }

    void forEachInstantiableLayout(const RuntimeType& type, const Invocable<IterationStatus(const LayoutData&)> auto& functor, bool includingAbstractLayoutSubtypes = false) const
    {
        auto isDoneAfterVisiting = [&](uint32_t shape) {
            const LayoutData& layout = m_state.layoutForShape(shape);
            return layout.kind == LayoutKind::Record && functor(layout) == IterationStatus::Done;
        };
        for (uint32_t i = 0; i < type.numberOfSupertypes(); ++i) {
            auto& view = type.view(i);
            if (view.supertypeShape) {
                if (isDoneAfterVisiting(view.supertypeShape))
                    return;
                if (!includingAbstractLayoutSubtypes || !m_state.layoutForShape(view.supertypeShape).isAbstract)
                    continue;
            }
            for (uint32_t shape = view.first; shape < view.first + view.count; ++shape) {
                if (shape != view.supertypeShape && m_state.passes(view, shape) && m_state.hasLayoutForShape(shape) && isDoneAfterVisiting(shape))
                    return;
            }
        }
    }

public:
    std::optional<uint32_t> outcomeByStructure(Object* object, const RuntimeType& type, bool staysIfNotConvertible, bool matchesInDiscriminatingField, DiscriminatingField& decides)
    {
        bool isOfType = fits(object, type);
        if (isOfType && (m_mode == Mode::Test || !shouldSpecialize(object, type)))
            return 0;
        if (m_mode == Mode::Test)
            return std::nullopt;
        Vector<uint32_t, 2> candidates;
        SetForScope ignoreCapacity(m_checksCapacity, false);
        forEachSpecialization(object, type, [&](const LayoutData& candidate) {
            if (!candidates.contains(candidate.shape))
                candidates.append(candidate.shape);
            return IterationStatus::Continue;
        });
        if (candidates.size() == 1)
            return candidates[0];
        if (candidates.size() > 1) {
            auto [numberOfFields, field] = distinguishingFields(object, candidates.span());
            if (!numberOfFields || matchesInDiscriminatingField)
                return chooseSpecialization(object, type)->shape;
            const LayoutData& layout = object->layout();
            if (numberOfFields == 1 && layout.discriminant() == LayoutData::noBase) {
                decides.field = &layout.fields()[field];
                for (uint32_t shape : candidates) {
                    const FieldData& inCandidate = *m_state.field(m_vm, m_state.layoutForShape(shape), m_state.name(m_vm, decides.field->name).impl());
                    for (auto& literal : std::span { m_state.data.at<LiteralData>(inCandidate.literalsOffset()), inCandidate.numberOfLiterals() }) {
                        if (literal.isNumber())
                            decides.numbers.add(JSValue::encode(jsNumber(literal.number())));
                        else if (const HashedString* hashed = m_state.data.hashedString(literal.string())) {
                            if (!decides.hashedStrings.contains(hashed))
                                decides.hashedStrings.append(hashed);
                        } else
                            decides.strings.add(m_state.text(literal.string()), literal.string());
                    }
                }
            }
        }
        if (candidates.isEmpty() && (isOfType || staysIfNotConvertible || type.acceptsUntypedObjectsByNature()))
            return 0;
        return std::nullopt;
    }

private:
    bool m_checksCapacity { true };
    bool m_mayConvertToSubtypes { false };
    unsigned m_checksNestedValues { 0 };

    std::pair<unsigned, uint32_t> distinguishingFields(Object* object, std::span<const uint32_t> candidates)
    {
        const LayoutData& layout = object->layout();
        std::pair<unsigned, uint32_t> result { 0, 0 };
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            if (i == layout.discriminant())
                continue;
            UniquedStringImpl* name = m_state.name(m_vm, layout.fields()[i].name).impl();
            const RuntimeType* type = nullptr;
            bool distinguishes = false;
            for (uint32_t shape : candidates) {
                const FieldData* field = m_state.field(m_vm, m_state.layoutForShape(shape), name);
                RELEASE_ASSERT(field);
                distinguishes |= field->numberOfLiterals() || (type && type != &m_state.ownTypeOf(*field));
                type = &m_state.ownTypeOf(*field);
            }
            if (distinguishes)
                result = { result.first + 1, i };
        }
        return result;
    }

    bool canConvertTo(Object* object, const LayoutData& to, UniquedStringImpl* stored = nullptr)
    {
        const LayoutData& from = m_state.layoutForShape(shapeOf(object));
        if (!stored && !std::ranges::binary_search(std::span { m_state.data.at<uint32_t>(from.specializationsOffset()), from.numberOfSpecializations() }, to.shape))
            return false;
        if (m_state.hasPrototypeMembers(from) || to.kind != LayoutKind::Record || to.hasExtension != from.hasExtension)
            return false;
        if (isCoercion() && to.isAbstract)
            return false;
        JSValue discriminant = discriminantOf(object, from);
        if (discriminant && to.discriminant() != LayoutData::noBase && !m_state.isAllowedLiteral(to.fields()[to.discriminant()], discriminant))
            return false;
        if (m_checksCapacity && to.size > object->cellSize()) {
            m_state.reportInsufficientCapacity(from, object->cellSize(), to);
            m_lackedCapacity.add(object);
            return false;
        }
        if (m_completeness == Completeness::MayLackAddedFields)
            return true;
        auto& required = m_state.requiredFieldsAdded(from, to);
        if (required.isEmpty() || !m_visited.add(object).isNewEntry)
            return true;
        JSObject* extra = object->extraProperties();
        bool hasAddedFields = std::ranges::all_of(required, [&](uint16_t index) {
            const FieldData& field = to.fields()[index];
            UniquedStringImpl* name = m_state.name(m_vm, field.name).impl();
            if (name == stored)
                return true;
            unsigned attributes;
            JSValue value = extra ? extra->getDirect(m_vm, name, attributes) : JSValue();
            return value && !attributes && mayBeCoerced(value, m_state.ownTypeOf(field));
        });
        m_visited.remove(object);
        return hasAddedFields;
    }

    static JSValue discriminantOf(Object* object, const LayoutData& layout)
    {
        return layout.discriminant() != LayoutData::noBase ? object->read(layout.fields()[layout.discriminant()]) : JSValue();
    }

    void forEachSpecialization(Object* object, const RuntimeType& type, const Invocable<IterationStatus(const LayoutData&)> auto& functor)
    {
        const LayoutData& layout = m_state.layoutForShape(shapeOf(object));
        if (!layout.numberOfSpecializations())
            return;
        bool hasCandidate = false;
        forEachInstantiableLayout(type, [&](const LayoutData& candidate) {
            if (!canConvertTo(object, candidate))
                return IterationStatus::Continue;
            hasCandidate = true;
            return functor(candidate);
        }, !!discriminantOf(object, layout));
        if (hasCandidate || !m_mayConvertToSubtypes)
            return;
        Properties extraProperties;
        if (JSObject* extra = object->extraProperties())
            extraProperties = propertiesOf(extra);
        for (uint32_t shape : std::span { m_state.data.at<uint32_t>(layout.specializationsOffset()), layout.numberOfSpecializations() }) {
            const LayoutData& candidate = m_state.layoutForShape(shape);
            if (candidate.kind != LayoutKind::Record || !m_state.acceptsShape(type, shape) || !canConvertTo(object, candidate))
                continue;
            auto distance = distanceTo(object, extraProperties, candidate);
            if (distance && !distance->otherLiterals && functor(candidate) == IterationStatus::Done)
                return;
        }
    }

    std::optional<Distance> distanceTo(Object* object, const Properties& extraProperties, const LayoutData& layout)
    {
        Distance distance;
        const LayoutData& current = object->layout();
        for (uint32_t i = 0; i < current.numberOfFields; ++i) {
            JSValue value = object->read(current.fields()[i]);
            const FieldData* field = value ? m_state.field(m_vm, layout, m_state.name(m_vm, current.fields()[i].name).impl()) : nullptr;
            if (field)
                distance.otherLiterals += !fits(value, m_state.ownTypeOf(*field)) || !m_state.isAllowedLiteral(*field, value);
        }
        for (auto& property : extraProperties) {
            const FieldData* field = m_state.field(m_vm, layout, property.name);
            if (!field) {
                ++distance.extraProperties;
                continue;
            }
            JSValue value = object->extraProperties()->getDirect(property.offset);
            if (property.attributes || !mayBeCoerced(value, m_state.ownTypeOf(*field)))
                return std::nullopt;
            distance.otherLiterals += !m_state.isAllowedLiteral(*field, value);
        }
        return distance;
    }

    const LayoutData* chooseSpecialization(Object* object, const RuntimeType& type)
    {
        Vector<const LayoutData*, 4> candidates;
        forEachSpecialization(object, type, [&](const LayoutData& candidate) {
            candidates.append(&candidate);
            return IterationStatus::Continue;
        });
        if (candidates.size() < 2)
            return candidates.isEmpty() ? nullptr : candidates[0];
        Properties extraProperties;
        if (JSObject* extra = object->extraProperties())
            extraProperties = propertiesOf(extra);
        const LayoutData* layout = nullptr;
        Distance least;
        for (const LayoutData* candidate : candidates) {
            if (auto distance = distanceTo(object, extraProperties, *candidate); distance && (!layout || *distance < least)) {
                least = *distance;
                layout = candidate;
            }
        }
        return layout ? layout : candidates[0];
    }

    Properties propertiesOf(JSObject* object)
    {
        m_state.reifyLazyClosures(m_vm, object);
        Properties properties;
        object->structure()->forEachProperty(m_vm, [&](const auto& entry) {
            properties.append({ entry.key(), entry.offset(), entry.attributes() });
            return true;
        });
        return properties;
    }

    JSValue specialize(Object* object, const RuntimeType& type)
    {
        const LayoutData* chosen = chooseSpecialization(object, type);
        return chosen ? specializeTo(object, *chosen) : failCoercion(object, type);
    }

    JSValue specializeTo(Object* object, const LayoutData& layout)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        uint32_t from = shapeOf(object);
        uint32_t to = layout.shape;
        if (auto* own = object->orderWord() & orderWordIsFrozen ? m_state.fieldAttributes.get(object) : nullptr) {
            for (auto& [name, attributes] : own->attributesByField) {
                if (!(attributes & PropertyAttribute::DontEnum))
                    continue;
                for (auto& field : std::span { layout.fields(), layout.numberOfFields }) {
                    if (field.name == name && !m_state.mayBeAbsent(field))
                        return fail([&] { return makeString("has a non-enumerable property that "_s, m_state.nameOf(layout), " requires"_s); });
                }
            }
        }
        if (object->orderWord() & (orderWordIsNotExtensible | orderWordIsFrozen) && !layout.mayBeFrozen)
            return fail([&] { return makeString("is frozen, sealed or non-extensible and cannot be used as "_s, m_state.nameOf(layout)); });
        m_plannedShapes.set(object, to);
        size_t index = m_specializations.size();
        m_specializations.append({ object, to, { } });
        if (JSObject* extra = object->extraProperties()) {
            for (auto& property : propertiesOf(extra)) {
                const FieldData* field = m_state.field(m_vm, layout, property.name);
                if (!field || m_state.field(m_vm, m_state.layoutForShape(from), property.name))
                    continue;
                m_path.append(property.name);
                if (property.attributes)
                    return fail([&] { return String(describeAttributes(property.attributes)); });
                JSValue coerced = coerce(extra->getDirect(property.offset), m_state.ownTypeOf(*field));
                RETURN_IF_EXCEPTION(scope, { });
                if (!coerced)
                    return { };
                m_path.removeLast();
                m_specializations[index].movedIntoFields.append(std::pair { static_cast<uint16_t>(field - layout.fields()), coerced });
            }
        }
        return object;
    }

    bool isPlainObject(JSObject* object) const { return isPlainData(object); }

    std::optional<Distance> distanceTo(JSObject* object, const Properties& properties, const LayoutData& layout, bool explains)
    {
        Distance distance;
        BitVector isPresent;
        auto integrity = plainDataIntegrity(m_vm, object);
        if (integrity && !layout.mayBeFrozen) {
            if (explains)
                fail([&] { return makeString("is frozen, sealed or non-extensible and cannot be used as "_s, m_state.nameOf(layout), ", which the program stores to"_s); });
            return std::nullopt;
        }
        unsigned allAttributes = dataAttributesFor(integrity);
        for (auto& property : properties) {
            const FieldData* field = m_state.field(m_vm, layout, property.name);
            if (!field) {
                ++distance.extraProperties;
                distance.otherLiterals += !m_state.isAllowedLiteralWithoutField(m_vm, layout, property.name, object->getDirect(property.offset));
                continue;
            }
            isPresent.set(field - layout.fields());
            JSValue value = object->getDirect(property.offset);
            bool hasFieldAttributes = property.attributes == allAttributes || (layout.mayBeFrozen && (property.attributes & ~(PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete)) == 0 && (property.attributes & allAttributes) == allAttributes);
            if (hasFieldAttributes && mayBeCoerced(value, m_state.ownTypeOf(*field))) {
                distance.otherLiterals += !m_state.isAllowedLiteral(*field, value);
                continue;
            }
            if (explains) {
                m_path.append(property.name);
                if (!hasFieldAttributes)
                    fail([&] { return String(describeAttributes(property.attributes)); });
                else
                    failCoercion(value, m_state.ownTypeOf(*field));
            }
            return std::nullopt;
        }
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            if (layout.fields()[i].flags & fieldIsOptional || isPresent.get(i))
                continue;
            if (explains) {
                m_path.append(m_state.name(m_vm, layout.fields()[i].name).impl());
                fail([&] { return makeString("is missing in "_s, State::describe(object), " but required in "_s, m_state.nameOf(layout)); });
            }
            return std::nullopt;
        }
        return distance;
    }

    static bool hasCustomProperties(const Properties& properties)
    {
        return properties.containsIf([](auto& property) {
            return property.attributes & PropertyAttribute::CustomAccessorOrValue;
        });
    }

    const LayoutData* chooseLayout(JSObject* object, const Properties& properties, const RuntimeType& type, bool explains)
    {
        if (const State::CoercionPlan* plan = m_state.coercionPlan(object, type); plan && plan->chosen) {
            SetForScope below(m_checksNestedValues, plan->hasAlternatives && !m_checksNestedValues ? 3u : m_checksNestedValues);
            if (distanceTo(object, properties, *plan->chosen, false))
                return plan->chosen;
        }
        const LayoutData* layout = nullptr;
        const LayoutData* firstCandidate = nullptr;
        Distance least;
        unsigned viableCount = 0;
        forEachInstantiableLayout(type, [&](const LayoutData& candidate) {
            if (!firstCandidate)
                firstCandidate = &candidate;
            auto distance = distanceTo(object, properties, candidate, false);
            viableCount += !!distance;
            if (distance && (!layout || *distance < least)) {
                least = *distance;
                layout = &candidate;
            }
            return IterationStatus::Continue;
        });
        if (viableCount > 1 && !m_checksNestedValues) {
            SetForScope below(m_checksNestedValues, 3u);
            if (const LayoutData* chosenByNestedValues = chooseLayout(object, properties, type, false))
                return chosenByNestedValues;
        }
        if (!layout && firstCandidate && explains && m_explains)
            distanceTo(object, properties, *firstCandidate, true);
        return layout;
    }

    bool mayConvertProperties(const RuntimeType& type) const
    {
        uintptr_t& asked = m_state.propertyConversionCacheEntry(type);
        if ((asked & ~static_cast<uintptr_t>(1)) == std::bit_cast<uintptr_t>(&type))
            return asked & 1;
        bool result = false;
        forEachInstantiableLayout(type, [&](const LayoutData& layout) {
            for (uint32_t i = 0; i < layout.numberOfFields && !result; ++i)
                result = !(layout.fields()[i].flags & fieldIsPlaceholder) && m_state.ownTypeOf(layout.fields()[i]).numberOfSupertypes();
            return result ? IterationStatus::Done : IterationStatus::Continue;
        });
        asked = std::bit_cast<uintptr_t>(&type) | result;
        return result;
    }

    bool mayConvertNestedValues(JSObject* object, const RuntimeType& type)
    {
        unsigned nestedObjectBudget = 32;
        return mayConvertNestedValues(object, type, nestedObjectBudget);
    }

    bool mayConvertNestedValues(JSObject* object, const RuntimeType& type, unsigned& nestedObjectBudget)
    {
        static constexpr unsigned maxLayouts = 4;
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        Structure* structure = object->structure();
        bool hasNoPlans = m_inlinedCopies.isEmpty() && m_plannedShapes.isEmpty();
        auto mayChange = [&](JSValue value, const RuntimeType& ofField, bool leavesUntypedObjects) {
            if (auto told = hasNoPlans ? m_state.mayConvertNestedValue(value, ofField, leavesUntypedObjects) : std::nullopt)
                return *told;
            if (!value.isObject())
                return false;
            if (Object* compiled = m_state.compiledObject(value)) {
                if (!m_state.layoutForShape(shapeOf(compiled)).numberOfSpecializations() || (fits(value, ofField) && !shouldSpecialize(compiled, ofField)))
                    return false;
                bool isSpecialized = false;
                orUnchanged(value, true, [&]() -> JSValue {
                    isSpecialized = !!specialize(compiled, ofField);
                    return { };
                });
                return scope.exception() || isSpecialized;
            }
            JSObject* below = asObject(value);
            ClassPrototype::look(below);
            if (fits(value, ofField))
                return false;
            if (!ofField.acceptsUntypedObjectsByNature() || m_inlinedCopies.contains(below))
                return isPlainObject(below);
            if (!mayConvertProperties(ofField) || !isPlainObject(below))
                return false;
            if (!nestedObjectBudget)
                return true;
            bool mayConvertNested = mayConvertNestedValues(below, ofField, --nestedObjectBudget);
            return scope.exception() || mayConvertNested;
        };
        for (unsigned part = 0; auto* entry = m_state.propertiesWithSupertypes(m_vm, structure, type, part); ++part) {
            auto known = *entry;
            for (auto& property : known.span()) {
                if (mayChange(object->getDirect(property.offset), *m_state.data.at<RuntimeType>(property.type), property.leavesUntypedObjects))
                    return true;
            }
            if (known.isLast)
                return false;
        }
        Vector<State::PropertiesWithSupertypes::Property, State::PropertiesWithSupertypes::maxSize> found;
        bool hasFoundAll = !structure->isDictionary();
        unsigned numberOfLayouts = 0;
        bool result = false;
        forEachInstantiableLayout(type, [&](const LayoutData& layout) {
            result = ++numberOfLayouts > maxLayouts;
            for (uint32_t i = 0; i < layout.numberOfFields && !result; ++i) {
                const FieldData& field = layout.fields()[i];
                const RuntimeType& ofField = m_state.ownTypeOf(field);
                if (!ofField.numberOfSupertypes())
                    continue;
                unsigned attributes = 0;
                PropertyOffset offset = structure->get(m_vm, m_state.name(m_vm, field.name), attributes);
                if (!isValidOffset(offset) || attributes)
                    continue;
                hasFoundAll &= found.size() < State::PropertiesWithSupertypes::maxSize * State::PropertiesWithSupertypes::maxParts;
                bool leavesUntypedObjects = hasFoundAll && ofField.acceptsUntypedObjectsByNature() && !mayConvertProperties(ofField);
                if (hasFoundAll)
                    found.append({ offset, field.ownType(), leavesUntypedObjects });
                result = mayChange(object->getDirect(offset), ofField, leavesUntypedObjects);
            }
            return result ? IterationStatus::Done : IterationStatus::Continue;
        });
        RELEASE_ASSERT(object->structure() == structure);
        if (!result && hasFoundAll) {
            std::span rest = found.span();
            uint8_t part = 0;
            do {
                auto ofPart = rest.first(std::min<size_t>(rest.size(), State::PropertiesWithSupertypes::maxSize));
                rest = rest.subspan(ofPart.size());
                State::PropertiesWithSupertypes entry { structure->id().bits(), static_cast<uint16_t>(ofPart.size()), part++, rest.empty(), &type, { } };
                std::ranges::copy(ofPart, entry.properties);
                m_state.setPropertiesWithSupertypes(m_vm, structure, entry);
            } while (!rest.empty());
        }
        return result;
    }

    JSValue keep(JSObject* object, const RuntimeType& type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        bool mayChange = m_state.data.isCompiledForAudit || mayConvertNestedValues(object, type);
        RETURN_IF_EXCEPTION(scope, { });
        if (!mayChange)
            return object;
        if (!m_kept.add({ object, &type }).isNewEntry)
            return object;
        m_keptInOrder.append({ object, &type });
        Structure* structure = object->structure();
        if (auto& known = m_state.keepFailure(m_vm, structure, type); known.structureID == structure->id().bits() && known.type == &type) {
            const FieldData* field = known.field;
            unsigned attributes = 0;
            PropertyOffset offset = structure->get(m_vm, m_state.name(m_vm, field->name), attributes);
            bool stillFails = false;
            if (isValidOffset(offset) && !attributes) {
                orUnchanged(object, true, [&]() -> JSValue {
                    stillFails = !coerce(object->getDirect(offset), m_state.ownTypeOf(*field));
                    return { };
                });
                RETURN_IF_EXCEPTION(scope, { });
            }
            if (stillFails)
                return { };
            m_state.keepFailure(m_vm, structure, type) = { };
        }
        Properties properties = propertiesOf(object);
        const LayoutData* layout = hasCustomProperties(properties) ? nullptr : chooseLayout(object, properties, type, false);
        if (!layout)
            return object;
        if (!m_state.coercionPlan(object, type) && m_state.canAddCoercionPlans()) {
            State::CoercionPlan::Discriminants discriminants;
            auto plan = planFor(object, structure, properties, type, *layout, 0, discriminants);
            plan->keepsObject = !!plan->layout;
            plan->layout = nullptr;
            m_state.addCoercionPlan(object, structure, type, WTF::move(plan), discriminants);
        }
        for (auto& property : properties) {
            const FieldData* field = m_state.field(m_vm, *layout, property.name);
            if (!field)
                continue;
            JSValue value = object->getDirect(property.offset);
            m_path.append(property.name);
            JSValue coerced = coerce(value, m_state.ownTypeOf(*field));
            RETURN_IF_EXCEPTION(scope, { });
            if (!coerced) {
                unsigned numberOfLayouts = 0;
                forEachInstantiableLayout(type, [&](const LayoutData&) {
                    return ++numberOfLayouts > 1 ? IterationStatus::Done : IterationStatus::Continue;
                });
                if (numberOfLayouts == 1 && object->structure() == structure)
                    m_state.keepFailure(m_vm, structure, type) = { structure->id().bits(), &type, field };
                return { };
            }
            m_path.removeLast();
            if (coerced != value)
                m_propertyStores.append({ object, property.name, coerced });
        }
        return object;
    }

    bool choiceDependsOnNamesAndLiterals(JSObject* object, const Properties& properties, const RuntimeType& type, const LayoutData& chosen, State::CoercionPlan::Discriminants& discriminants, bool& isExact, bool& hasAlternatives, const LayoutData** nearest = nullptr)
    {
        const LayoutData* first = nullptr;
        Distance least;
        BitVector isCheckedAgainstLiterals;
        Vector<std::pair<const LayoutData*, Distance>, 4> candidates;
        forEachInstantiableLayout(type, [&](const LayoutData& candidate) {
            Distance distance;
            Vector<const Property*, 4> checkedHere;
            BitVector isPresent;
            for (auto& property : properties) {
                const FieldData* field = m_state.field(m_vm, candidate, property.name);
                JSValue value = object->getDirect(property.offset);
                if (!field) {
                    ++distance.extraProperties;
                    if (candidate.literalsWithoutFieldOffset()) {
                        checkedHere.append(&property);
                        distance.otherLiterals += !m_state.isAllowedLiteralWithoutField(m_vm, candidate, property.name, value);
                    }
                    continue;
                }
                isPresent.set(field - candidate.fields());
                if (field->numberOfLiterals()) {
                    checkedHere.append(&property);
                    distance.otherLiterals += !m_state.isAllowedLiteral(*field, value);
                }
            }
            for (uint32_t i = 0; i < candidate.numberOfFields; ++i) {
                if (!(candidate.fields()[i].flags & fieldIsOptional) && !isPresent.get(i))
                    return IterationStatus::Continue;
            }
            for (const Property* property : checkedHere)
                isCheckedAgainstLiterals.set(property - properties.begin());
            candidates.append({ &candidate, distance });
            if (!first || distance < least) {
                first = &candidate;
                least = distance;
            }
            return IterationStatus::Continue;
        });
        for (auto& [candidate, distance] : candidates) {
            if (candidate == first || distance != least)
                continue;
            bool acceptsSameTypes = std::ranges::all_of(properties, [&, candidate = candidate](auto& property) {
                const FieldData* field = m_state.field(m_vm, *first, property.name);
                const FieldData* other = m_state.field(m_vm, *candidate, property.name);
                return field && other ? &m_state.ownTypeOf(*field) == &m_state.ownTypeOf(*other) : field == other;
            });
            if (!acceptsSameTypes)
                return false;
        }
        if (isCheckedAgainstLiterals.bitCount() > State::CoercionPlan::maxNumberOfDiscriminants)
            return false;
        for (size_t index : isCheckedAgainstLiterals)
            discriminants.append(properties[index].offset);
        isExact = !least.otherLiterals;
        hasAlternatives = candidates.size() > 1;
        if (nearest)
            *nearest = first;
        return first == &chosen;
    }

    std::unique_ptr<State::CoercionPlan> planFor(JSObject* object, Structure* structure, const Properties& properties, const RuntimeType& type, const LayoutData& layout, uint32_t orderAndCount, State::CoercionPlan::Discriminants& discriminants)
    {
        auto plan = makeUnique<State::CoercionPlan>();
        if (object->structure() != structure || structure->isDictionary() || structure->hasPolyProto() || hasIndexedProperties(structure->indexingType()))
            return plan;
        bool isExact = false;
        if (properties.containsIf([](auto& property) { return !!property.attributes; }) || !choiceDependsOnNamesAndLiterals(object, properties, type, layout, discriminants, isExact, plan->hasAlternatives))
            return plan;
        plan->chosen = &layout;
        if (!isExact)
            return plan;
        for (auto& property : properties) {
            if (const FieldData* field = m_state.field(m_vm, layout, property.name))
                plan->fields.append({ property.offset, field, property.name, 0 });
            else if (property.name->isSymbol() || layout.literalsWithoutFieldOffset())
                return plan;
            else
                plan->extraProperties.append({ property.offset, property.name });
        }
        for (auto& planned : plan->fields)
            planned.acceptedKinds = m_state.kindsAcceptedBy(m_state.ownTypeOf(*planned.field));
        m_state.finishCoercionPlan(*plan, layout, orderAndCount);
        return plan;
    }

public:
    const State::CoercionPlan* planForNearestLayout(JSObject* object, const RuntimeType& type)
    {
        Properties properties = propertiesOf(object);
        Structure* structure = object->structure();
        if (structure->isDictionary() || structure->hasPolyProto() || hasIndexedProperties(structure->indexingType()) || properties.size() > maxFieldsInOrder || properties.containsIf([](auto& property) { return !!property.attributes; }))
            return nullptr;
        const LayoutData* someLayout = nullptr;
        forEachInstantiableLayout(type, [&](const LayoutData& candidate) {
            someLayout = &candidate;
            return IterationStatus::Done;
        });
        if (!someLayout)
            return nullptr;
        auto plan = makeUnique<State::CoercionPlan>();
        const LayoutData* nearest = nullptr;
        State::CoercionPlan::Discriminants discriminants;
        bool isExact = false;
        choiceDependsOnNamesAndLiterals(object, properties, type, *someLayout, discriminants, isExact, plan->hasAlternatives, &nearest);
        if (!nearest || !isExact || nearest->literalsWithoutFieldOffset())
            return nullptr;
        Vector<uint16_t, 16> order;
        for (auto& property : properties) {
            const FieldData* field = m_state.field(m_vm, *nearest, property.name);
            if (!field)
                return nullptr;
            plan->fields.append({ property.offset, field, property.name, m_state.kindsAcceptedBy(m_state.ownTypeOf(*field)) });
            order.append(field - nearest->fields());
        }
        plan->chosen = nearest;
        m_state.finishCoercionPlan(*plan, *nearest, m_state.orderStartingWith(*nearest, order.span()));
        m_state.addCoercionPlan(object, structure, type, WTF::move(plan), discriminants);
        return m_state.coercionPlan(object, type);
    }

private:
    JSValue copyByPlan(JSObject* object, const State::CoercionPlan& plan)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const LayoutData& layout = *plan.layout;
        Structure* structure = m_state.structureFor(m_vm, layout.shape, object->getPrototypeDirect());
        Object* result = Object::create(m_vm, structure, layout, m_state.sizeWithSpecializationCapacity(layout.shape), orderWordOf(keysAreInLayoutOrder, 0));
        m_copiesKeptAlive.append(result);
        m_copiesKeptAlive.append(object);
        if (m_copiesKeptAlive.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(m_globalObject, scope);
            return { };
        }
        m_inlinedCopies.add(object, result);
        m_copied.append(object);
        recordCopyPath(object);
        for (auto& planned : plan.fields) {
            const FieldData* field = planned.field;
            JSValue value = object->getDirect(planned.offset);
            m_path.append(planned.name);
            JSValue coerced = coerce(value, m_state.ownTypeOf(*field));
            RETURN_IF_EXCEPTION(scope, { });
            if (!coerced)
                return { };
            if (!result->store(m_vm, *field, coerced))
                return failCoercion(value, m_state.ownTypeOf(*field));
            m_path.removeLast();
        }
        result->setOrderAndCount(plan.orderAndCount);
        ensureStillAliveHere(object);
        m_state.countObject(result);
        return result;
    }

    JSValue copy(JSObject* object, const RuntimeType& type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!isPlainDataIgnoringIntegrity(object))
            return failCoercion(object, type);
        Structure* sourceStructure = object->structure();
        auto integrity = plainDataIntegrity(m_vm, object);
        const State::CoercionPlan* plan = integrity ? nullptr : m_state.coercionPlan(object, type);
        if (plan && plan->layout && plan->extraProperties.isEmpty()) {
            bool hasOnlyCoercibleValues = std::ranges::all_of(plan->fields, [&](auto& planned) {
                return mayBeCoerced(object->getDirect(planned.offset), m_state.ownTypeOf(*planned.field));
            });
            if (hasOnlyCoercibleValues)
                RELEASE_AND_RETURN(scope, copyByPlan(object, *plan));
        }
        Properties properties = propertiesOf(object);
        if (hasCustomProperties(properties))
            return fail([] { return "has native getters and cannot be converted"_str; });
        bool mayChooseAgain = m_copiesProgramObjects && m_mode == Mode::Cast;
        const LayoutData* layout = chooseLayout(object, properties, type, !mayChooseAgain);
        if (!layout && mayChooseAgain) {
            SetForScope expectCopies(m_expectsProgramObjectCopies, true);
            layout = chooseLayout(object, properties, type, true);
        }
        if (!layout)
            return failCoercion(object, type);

        Structure* structure = m_state.structureFor(m_vm, layout->shape, object->getPrototypeDirect());
        Object* result = Object::create(m_vm, structure, *layout, m_state.sizeWithSpecializationCapacity(layout->shape), orderWordOf(keysAreInLayoutOrder, 0));
        m_copiesKeptAlive.append(result);
        m_copiesKeptAlive.append(object);
        if (m_copiesKeptAlive.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(m_globalObject, scope);
            return { };
        }
        m_inlinedCopies.add(object, result);
        m_copied.append(object);
        recordCopyPath(object);
        Vector<uint16_t, 16> order;
        uint32_t numberOfExtraKeys = 0;
        uint32_t positionedExtraKeyCount = 0;
        unsigned allAttributes = dataAttributesFor(integrity);
        Vector<std::pair<const FieldData*, unsigned>, 2> attributesPerField;
        for (auto& property : properties) {
            JSValue value = object->getDirect(property.offset);
            const FieldData* field = m_state.field(m_vm, *layout, property.name);
            if (!field) {
                result->willAddExtraProperty(m_vm, property.name);
                if (property.attributes & PropertyAttribute::Accessor)
                    result->ensureExtraProperties(m_vm)->putDirectAccessor(m_globalObject, property.name, uncheckedDowncast<GetterSetter>(value), property.attributes);
                else
                    result->ensureExtraProperties(m_vm)->putDirect(m_vm, property.name, value, property.attributes);
                numberOfExtraKeys += m_state.hasPositionAmongKeys(*layout, property.name);
                continue;
            }
            m_path.append(property.name);
            JSValue coerced = coerce(value, m_state.ownTypeOf(*field));
            RETURN_IF_EXCEPTION(scope, { });
            if (!coerced)
                return { };
            if (!result->store(m_vm, *field, coerced))
                return failCoercion(value, m_state.ownTypeOf(*field));
            if (property.attributes != allAttributes)
                attributesPerField.append({ field, property.attributes & ~allAttributes });
            m_path.removeLast();
            if (uint32_t entry = layout->numberOfFields + numberOfExtraKeys - 1; std::exchange(positionedExtraKeyCount, numberOfExtraKeys) != numberOfExtraKeys && entry < UINT16_MAX)
                order.append(static_cast<uint16_t>(entry));
            order.append(field - layout->fields());
        }
        if (order.size() > maxFieldsInOrder) [[unlikely]] {
            order.removeAllMatching([&](uint16_t entry) {
                return entry >= layout->numberOfFields;
            });
        }
        if (hasIndexedProperties(object->indexingType())) {
            PropertyNameArrayBuilder indices(m_vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
            object->getOwnIndexedPropertyNames(m_globalObject, indices, DontEnumPropertiesMode::Include);
            for (auto& name : indices) {
                uint32_t index = *parseIndex(name);
                PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
                if (!JSObject::getOwnPropertySlotByIndex(object, m_globalObject, index, slot) || !slot.isValue() || slot.attributes() & PropertyAttribute::ReadOnly)
                    return fail([] { return "has an indexed accessor or read only element and cannot be converted"_str; });
                result->ensureExtraProperties(m_vm)->putDirectIndex(m_globalObject, index, slot.getValue(m_globalObject, index), slot.attributes(), PutDirectIndexLikePutDirect);
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
        result->didChangeExtraProperties(m_vm);
        uint32_t orderAndCount = m_state.orderStartingWith(*layout, order.span());
        result->setOrderAndCount(orderAndCount);
        m_state.countObject(result);
        for (auto [field, attributes] : attributesPerField)
            result->setAttributesOf(m_vm, *field, attributes);
        if (integrity) {
            RELEASE_ASSERT(layout->mayBeFrozen);
            result->setIntegrity(m_globalObject, *integrity);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (integrity || !attributesPerField.isEmpty())
            return result;
        if (!plan && m_state.canAddCoercionPlans()) {
            State::CoercionPlan::Discriminants discriminants;
            auto made = planFor(object, sourceStructure, properties, type, *layout, orderAndCount, discriminants);
            m_state.addCoercionPlan(object, sourceStructure, type, WTF::move(made), discriminants);
        }
        return result;
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    State& m_state;
    Mode m_mode;
    Completeness m_completeness;
    bool m_explains;
    UncheckedKeyHashSet<Object*> m_visited;
    UncheckedKeyHashMap<JSObject*, Object*> m_inlinedCopies;
    MarkedArgumentBuffer m_copiesKeptAlive;
    bool m_allowsInPlaceConversion { false };
    UncheckedKeyHashMap<JSCell*, JSObject*> m_convertedInPlace;
    bool m_copiesProgramObjects { false };
    bool m_disallowsCopies { false };
    bool m_expectsProgramObjectCopies { false };
    UncheckedKeyHashMap<Object*, JSObject*> m_ordinaryCopies;
    UncheckedKeyHashSet<JSObject*> m_isOrdinaryCopy;
    UncheckedKeyHashMap<JSObject*, String> m_copyPaths;
    UncheckedKeyHashSet<Object*> m_lackedCapacity;
    UncheckedKeyHashSet<JSObject*> m_copiedForLackOfCapacity;
    JSObject* m_unsharedObject { nullptr };
    UncheckedKeyHashMap<Object*, uint32_t> m_plannedShapes;
    UncheckedKeyHashSet<std::pair<JSObject*, const RuntimeType*>> m_kept;
    Vector<JSObject*> m_copied;
    Vector<std::pair<JSObject*, const RuntimeType*>> m_keptInOrder;
    Vector<Specialization> m_specializations;
    Vector<PropertyStore> m_propertyStores;
    Vector<UniquedStringImpl*, 8> m_path;
    String m_failure;
};

auto State::listedFieldsFor(VM& vm, const LayoutData& from, const LayoutData& to) -> const ListedFields&
{
    RELEASE_ASSERT(&from != &to);
    return m_listedFields.ensure(static_cast<uint64_t>(from.shape) << 32 | to.shape, [&] {
        ListedFields result;
        for (uint32_t i = 0; i < from.numberOfFields; ++i) {
            const FieldData* there = i == from.discriminant() ? nullptr : field(vm, to, name(vm, from.fields()[i].name).impl());
            if (there && there->numberOfLiterals())
                result.append({ &from.fields()[i], there });
        }
        return result;
    }).iterator->value;
}

bool State::hasListedFields(Object* object, const ListedFields& listed) const
{
    return std::ranges::all_of(listed, [&](auto& fields) {
        JSValue held = object->read(*fields.first);
        return !held || isAllowedLiteral(*fields.second, held);
    });
}

bool State::store(VM& vm, Object* object, const StoreSpecializations& known, JSValue value)
{
    for (auto& transition : known.transitions) {
        uint32_t before = object->orderWord();
        if (!fits(ownTypeOf(*transition.created), value) || !hasListedFields(object, transition.listed) || !object->specialize(vm, *this, *transition.plan))
            continue;
        size_t known = transition.orderWords.findIf([&](auto& beforeAndAfter) {
            return beforeAndAfter.first == before;
        });
        if (known != notFound) [[likely]] {
            object->willCreateField(*this, *transition.created);
            object->setOrderAndCount(transition.orderWords[known].second);
        } else {
            object->didCreateField(*this, *transition.created);
            if (transition.orderWords.size() < TransitionPlan::maxOrderWords)
                transition.orderWords.append({ before, object->orderWord() });
        }
        bool didStore = object->store(vm, *transition.created, value);
        RELEASE_ASSERT(didStore);
        return true;
    }
    return false;
}

bool State::storeByKnownTransition(VM& vm, Object* object, UniquedStringImpl* name, JSValue value)
{
    if (object->hasFlagsInOrderWord())
        return false;
    Structure* structure = object->structure();
    const LayoutData& from = *structure->compiledLayout();
    EncodedJSValue discriminant = from.discriminant() != LayoutData::noBase ? JSValue::encode(object->read(from.fields()[from.discriminant()])) : 0;
    CachedStore& cached = cachedStore(structure, name, discriminant);
    if (cached.structure != structure || cached.name != name || cached.discriminant != discriminant || !cached.transitions->dependOnlyOnPrototypeChain)
        return false;
    for (JSValue next = structure->storedPrototype(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        Structure* forHolder = asObject(next)->structure();
        TypeInfo typeInfo = forHolder->typeInfo();
        if (forHolder->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || typeInfo.overridesGetPrototype() || typeInfo.overridesGetOwnPropertySlot() || (typeInfo.overridesPut() && forHolder->classInfoForCells() != PrototypeObject::info()) || asObject(next)->hasNonReifiedStaticProperties())
            return false;
    }
    return store(vm, object, *cached.transitions, value);
}

State::KeepFailure& State::keepFailure(VM& vm, Structure* structure, const RuntimeType& type)
{
    if (HeapVersion version = vm.heap.objectSpace().newlyAllocatedVersion(); version != m_keepFailuresVersion) [[unlikely]] {
        zeroSpan(std::span { m_keepFailures });
        m_keepFailuresVersion = version;
    }
    return m_keepFailures[placeOf<numberOfKeepFailures>(static_cast<uint64_t>(structure->id().bits()) << 32 ^ std::bit_cast<uintptr_t>(&type))];
}

std::span<State::PropertiesWithSupertypes> State::propertiesWithSupertypesCandidates(VM& vm, Structure* structure, const RuntimeType& type, unsigned part)
{
    if (HeapVersion version = vm.heap.objectSpace().newlyAllocatedVersion(); version != m_propertiesWithSupertypesVersion) [[unlikely]] {
        for (auto& entry : std::span { m_propertiesWithSupertypes, numberOfPropertiesWithSupertypes }) {
            if (entry.type)
                entry.type = nullptr;
        }
        m_propertiesWithSupertypesVersion = version;
    }
    constexpr unsigned numberOfSets = numberOfPropertiesWithSupertypes / propertiesWithSupertypesWays;
    unsigned set = (WTF::intHash(static_cast<uint64_t>(structure->id().bits()) << 32 ^ std::bit_cast<uintptr_t>(&type)) + part) % numberOfSets;
    return std::span { m_propertiesWithSupertypes, numberOfPropertiesWithSupertypes }.subspan(set * propertiesWithSupertypesWays, propertiesWithSupertypesWays);
}

const State::PropertiesWithSupertypes* State::propertiesWithSupertypes(VM& vm, Structure* structure, const RuntimeType& type, unsigned part)
{
    for (auto& entry : propertiesWithSupertypesCandidates(vm, structure, type, part)) {
        if (entry.isOf(structure, type, part))
            return &entry;
    }
    return nullptr;
}

void State::setPropertiesWithSupertypes(VM& vm, Structure* structure, const PropertiesWithSupertypes& entry)
{
    auto places = propertiesWithSupertypesCandidates(vm, structure, *entry.type, entry.part);
    auto* place = &places[m_nextPropertiesWithSupertypesVictim % propertiesWithSupertypesWays];
    bool isTaken = true;
    for (auto& candidate : places) {
        if (candidate.isOf(structure, *entry.type, entry.part) || !candidate.type) {
            place = &candidate;
            isTaken = false;
            break;
        }
    }
    m_nextPropertiesWithSupertypesVictim += isTaken;
    *place = entry;
}

bool State::isUnchangedByCoercion(VM& vm, JSObject* object, const RuntimeType& type)
{
    Structure* structure = object->structure();
    for (unsigned part = 0; ; ++part) {
        auto* known = propertiesWithSupertypes(vm, structure, type, part);
        if (!known)
            return false;
        for (auto& property : known->span()) {
            if (mayConvertNestedValue(object->getDirect(property.offset), *data.at<RuntimeType>(property.type), property.leavesUntypedObjects) != std::optional(false))
                return false;
        }
        if (known->isLast)
            return true;
    }
}

bool State::isAcceptedUnchanged(JSObject* object, const RuntimeType& type) const
{
    return propertyConversionCacheEntry(type) == std::bit_cast<uintptr_t>(&type) || !isPlainData(object);
}

static std::optional<StoreTransition::Cleared> clearedSlot(const TransitionPlan::Slot& slot)
{
    switch (slot.store) {
    case Store::Cell:
    case Store::JSValue:
        return StoreTransition::Cleared { slot.offset, 8, static_cast<uint64_t>(JSValue::encode(JSValue())) };
    case Store::LiteralString:
        return StoreTransition::Cleared { slot.offset, 8, absentCell32 };
    case Store::Cell32:
    case Store::OptionalCell32:
    case Store::LiteralString32:
        return StoreTransition::Cleared { slot.offset, 4, absentCell32 };
    case Store::OptionalBoolean:
        return StoreTransition::Cleared { slot.offset, 1, JSValue::ValueEmpty };
    case Store::OptionalInt64:
        return StoreTransition::Cleared { slot.offset, 8, static_cast<uint64_t>(absentInteger) };
    case Store::OptionalDouble:
        return StoreTransition::Cleared { slot.offset, 8, absentDouble };
    case Store::Int32OrNumberBox:
        return StoreTransition::Cleared { slot.offset, 4, static_cast<uint32_t>(absentInt32) };
    default:
        return std::nullopt;
    }
}

const StoreTransition* State::storeTransitionFor(Structure* before, uint64_t wordsBefore, Object* object, UniquedStringImpl* name, uint64_t fieldWrite)
{
    Structure* after = object->structure();
    if (before->compiledLayout()->discriminant() != LayoutData::noBase || data.isCompiledForAudit || isCounting())
        return nullptr;
    if (before->typeInfo().type() != after->typeInfo().type() || before->typeInfo().inlineTypeFlags() != after->typeInfo().inlineTypeFlags() || before->indexingModeIncludingHistory() != after->indexingModeIncludingHistory())
        return nullptr;
    CachedStore& cached = cachedStore(before, name, 0);
    if (cached.structure != before || cached.name != name || cached.discriminant)
        return nullptr;
    for (auto& transition : cached.transitions->transitions) {
        if (transition.plan->structure != after || !transition.listed.isEmpty())
            return nullptr;
        auto& forSites = m_storeTransitions.add({ transition.plan, transition.created }, Vector<std::unique_ptr<StoreTransition>, 1> { }).iterator->value;
        for (auto& known : forSites) {
            if (known->wordsBefore == wordsBefore)
                return known.get();
        }
        if (transition.created->flags & fieldIsOutOfLine || forSites.size() >= TransitionPlan::maxOrderWords)
            return nullptr;
        auto made = makeUnique<StoreTransition>();
        *made = { wordsBefore, object->orderAndShapeWords(), fieldWrite, { }, { }, after->id().bits(), transition.plan->size, 0, nullptr, { } };
        unsigned numberOfHolders = 0;
        for (JSValue next = before->storedPrototype(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
            Structure* forHolder = asObject(next)->structure();
            if (numberOfHolders == StoreTransition::maxHolders || forHolder->isDictionary() || forHolder->hasPolyProto())
                return nullptr;
            if (m_planStructures.add(forHolder).isNewEntry)
                didCreate(forHolder);
            made->holders[numberOfHolders] = asObject(next);
            made->holderStructureIDs[numberOfHolders++] = forHolder->id().bits();
        }
        for (auto& slot : transition.plan->addedSlots) {
            if (auto cleared = clearedSlot(slot))
                made->clearedSlots.append(*cleared);
        }
        made->cleared = made->clearedSlots.span().data();
        made->clearedSlotCount = made->clearedSlots.size();
        forSites.append(WTF::move(made));
        return forSites.last().get();
    }
    return nullptr;
}

const StoreTransition* State::fieldCreatingStoreTransition(const FieldData& field, uint64_t wordsBefore, Object* object, uint64_t fieldWrite, bool checksPrototypeChain)
{
    Structure* structure = object->structure();
    uint64_t wordsAfter = object->orderAndShapeWords();
    uint32_t orderWordBefore = static_cast<uint32_t>(wordsBefore);
    bool hasExtraProperties = !isStructureWithoutExtraProperties(structure);
    if (data.isCompiledForAudit || isCounting() || (field.flags & fieldIsOutOfLine && !hasExtraProperties) || object->isDiscriminant(field))
        return nullptr;
    if (hasExtraProperties && (!structure->extraPropertiesStructureID() || field.flags & (fieldMayExistWithoutStorage | fieldMayBeUnlisted)))
        return nullptr;
    if (orderWordBefore & (orderWordIsFrozen | orderWordIsSealed | orderWordIsNotExtensible) || orderOf(orderWordBefore) == keysAreInLayoutOrder || orderOf(static_cast<uint32_t>(wordsAfter)) == keysAreInLayoutOrder)
        return nullptr;
    auto& known = (hasExtraProperties ? m_lateFieldCreationStoreTransitions : m_fieldCreatingStoreTransitions)[checksPrototypeChain].add({ structure, &field }, Vector<std::unique_ptr<StoreTransition>, 1> { }).iterator->value;
    for (auto& transition : known) {
        if (transition->wordsBefore == wordsBefore)
            return transition.get();
    }
    if (known.size() >= TransitionPlan::maxOrderWords)
        return nullptr;
    auto made = makeUnique<StoreTransition>();
    *made = { wordsBefore, wordsAfter, fieldWrite, { }, { }, structure->id().bits(), structure->compiledLayout()->size, 0, nullptr, { } };
    unsigned numberOfHolders = 0;
    for (JSValue next = checksPrototypeChain ? structure->storedPrototype() : jsNull(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        Structure* forHolder = asObject(next)->structure();
        if (numberOfHolders == StoreTransition::maxHolders || forHolder->isDictionary() || forHolder->hasPolyProto() || forHolder->typeInfo().overridesGetPrototype())
            return nullptr;
        made->holders[numberOfHolders] = asObject(next);
        made->holderStructureIDs[numberOfHolders++] = forHolder->id().bits();
    }
    for (unsigned i = 0; i < numberOfHolders; ++i) {
        if (m_planStructures.add(made->holders[i]->structure()).isNewEntry)
            didCreate(made->holders[i]->structure());
    }
    if (!hasExtraProperties && m_planStructures.add(structure).isNewEntry)
        didCreate(structure);
    known.append(WTF::move(made));
    return known.last().get();
}

const StoreTransition* State::storageCreatingStoreTransition(const FieldData& field, Object* object, uint64_t fieldWrite)
{
    Structure* structure = object->structure();
    uint64_t words = object->orderAndShapeWords();
    uint32_t orderWord = static_cast<uint32_t>(words);
    if (data.isCompiledForAudit || isCounting() || !(field.flags & fieldIsOutOfLine) || !(field.flags & fieldMayExistWithoutStorage) || object->isDiscriminant(field) || object->extraPropertiesHandle())
        return nullptr;
    if (orderWord & (orderWordIsFrozen | orderWordIsSealed | orderWordIsNotExtensible) || orderOf(orderWord) == keysAreInLayoutOrder || !object->isInKeyOrder(*this, field))
        return nullptr;
    auto& known = m_storageCreatingStoreTransitions.add({ structure, &field }, Vector<std::unique_ptr<StoreTransition>, 1> { }).iterator->value;
    for (auto& transition : known) {
        if (transition->wordsBefore == words)
            return transition.get();
    }
    if (known.size() >= TransitionPlan::maxOrderWords)
        return nullptr;
    auto made = makeUnique<StoreTransition>();
    *made = { words, words, fieldWrite, { }, { }, structure->id().bits(), structure->compiledLayout()->size, 0, nullptr, { }, static_cast<uint32_t>(WTF::roundUpToMultipleOf<minExtensionSize>(field.offset + sizeOf(field.store()))) };
    known.append(WTF::move(made));
    return known.last().get();
}

void State::removeDeadStoreTransitions(VM& vm)
{
    m_storageCreatingStoreTransitions.removeIf([&](auto& entry) {
        return !vm.heap.isMarked(entry.key.first);
    });
    for (auto& transitions : m_lateFieldCreationStoreTransitions) {
        transitions.removeIf([&](auto& entry) {
            return !vm.heap.isMarked(entry.key.first);
        });
    }
}

bool State::tryCreateFieldWithCachedTransition(VM& vm, Object* object, const FieldData& field, JSValue value)
{
    auto& transitions = (isStructureWithoutExtraProperties(object->structure()) ? m_fieldCreatingStoreTransitions : m_lateFieldCreationStoreTransitions)[true];
    auto known = transitions.find({ object->structure(), &field });
    if (known == transitions.end())
        return false;
    uint64_t words = object->orderAndShapeWords();
    for (auto& transition : known->value) {
        if (transition->wordsBefore != words)
            continue;
        for (unsigned i = 0; i < StoreTransition::maxHolders && transition->holders[i]; ++i) {
            if (transition->holders[i]->structureID().bits() != transition->holderStructureIDs[i])
                return false;
        }
        if (field.flags & fieldIsOutOfLine)
            object->willCreateField(*this, field);
        if (!object->write(vm, field, value))
            return false;
        if (object->extraPropertiesHandle())
            object->setOrderAndCount(static_cast<uint32_t>(transition->wordsAfter) & orderAndCountMask);
        else
            object->at<uint64_t>(offsetOfOrderWord) = transition->wordsAfter;
        return true;
    }
    return false;
}

bool State::specializeToStore(JSGlobalObject* globalObject, Object* object, PropertyName name, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const LayoutData& from = object->layout();
    if (!from.numberOfSpecializations() || name.isPrivateName() || isPublicSymbolThatKeysNoField(name.uid()))
        return false;
    bool usesPlans = !object->hasFlagsInOrderWord() && !data.isCompiledForAudit;
    JSValue discriminant = from.discriminant() != LayoutData::noBase ? object->read(from.fields()[from.discriminant()]) : JSValue();
    std::tuple<Structure*, RefPtr<UniquedStringImpl>, EncodedJSValue> planKey { object->structure(), name.uid(), JSValue::encode(discriminant) };
    auto storeByPlans = [&](const StoreSpecializations& known) {
        CachedStore& cached = cachedStore(object->structure(), name.uid(), JSValue::encode(discriminant));
        if (cached.structure && (cached.structure != object->structure() || cached.name != name.uid() || cached.discriminant != JSValue::encode(discriminant)))
            m_cachedStoreIndices.didReplaceEntry();
        cached = { object->structure(), name.uid(), JSValue::encode(discriminant), &known };
        return store(vm, object, known, value);
    };
    if (usesPlans) {
        if (auto known = m_storeSpecializations.find(planKey); known != m_storeSpecializations.end()) [[likely]]
            return storeByPlans(*known->value);
    }
    if (m_specializationsWithField.size() >= maxNumberOfOutcomes) [[unlikely]]
        m_specializationsWithField.clear();
    auto& shapes = m_specializationsWithField.ensure({ from.shape, name.uid() }, [&] {
        Vector<uint32_t, 1> result;
        Vector<uint32_t, 16> reached { from.shape };
        UncheckedKeyHashMap<uint32_t, unsigned> distance { { from.shape, 0u } };
        for (size_t at = 0; at < reached.size(); ++at) {
            const LayoutData& via = layoutForShape(reached[at]);
            for (uint32_t shape : std::span { data.at<uint32_t>(via.specializationsOffset()), via.numberOfSpecializations() }) {
                if (distance.add(shape, distance.get(via.shape) + 1).isNewEntry)
                    reached.append(shape);
            }
        }
        for (uint32_t shape : reached.span().subspan(1)) {
            const LayoutData& to = layoutForShape(shape);
            const FieldData* created = field(vm, to, name.uid());
            if (to.isAbstract || !created || created->flags & fieldIsImmutable)
                continue;
            bool mayLackAddedFields = true;
            for (uint32_t i = 0; i < to.numberOfFields && mayLackAddedFields; ++i)
                mayLackAddedFields = mayBeAbsent(to.fields()[i]) || field(vm, from, this->name(vm, to.fields()[i].name).impl());
            if (mayLackAddedFields)
                result.append(shape);
        }
        std::ranges::sort(result, [&](uint32_t a, uint32_t b) {
            return std::tuple { distance.get(a), layoutForShape(a).numberOfFields, layoutForShape(a).index } < std::tuple { distance.get(b), layoutForShape(b).numberOfFields, layoutForShape(b).index };
        });
        return result;
    }).iterator->value;
    if (usesPlans) {
        auto known = makeUnique<StoreSpecializations>();
        auto& transitions = known->transitions;
        known->dependOnlyOnPrototypeChain = !parseIndex(name) && name != vm.propertyNames->underscoreProto && !hasImmutableMethod(from, name.uid());
        if (m_planStructures.add(object->structure()).isNewEntry)
            didCreate(object->structure());
        for (uint32_t shape : shapes) {
            const LayoutData& to = layoutForShape(shape);
            bool requiresMore = requiredFieldsAdded(from, to).containsIf([&](uint16_t index) {
                return this->name(vm, to.fields()[index].name).impl() != name.uid();
            });
            if (requiresMore || hasPrototypeMembers(from) || to.kind != LayoutKind::Record || to.hasExtension != from.hasExtension)
                continue;
            if (discriminant && to.discriminant() != LayoutData::noBase && !isAllowedLiteral(to.fields()[to.discriminant()], discriminant))
                continue;
            transitions.append({ field(vm, to, name.uid()), &transitionPlanFor(vm, object, shape), listedFieldsFor(vm, from, to) });
        }
        if (m_storeSpecializations.size() >= maxNumberOfOutcomes) [[unlikely]] {
            m_storeSpecializations.clear();
            zeroSpan(std::span { m_cachedStores, m_cachedStoreIndices.count() });
        }
        return storeByPlans(*m_storeSpecializations.add(planKey, WTF::move(known)).iterator->value);
    }
    for (uint32_t shape : shapes) {
        const LayoutData& to = layoutForShape(shape);
        const FieldData& created = *field(vm, to, name.uid());
        if (!fits(ownTypeOf(created), value))
            continue;
        bool requiresMore = requiredFieldsAdded(from, to).containsIf([&](uint16_t index) {
            return this->name(vm, to.fields()[index].name).impl() != name.uid();
        });
        if (requiresMore && !object->extraProperties())
            continue;
        if (!hasListedFields(object, listedFieldsFor(vm, from, to)))
            continue;
        bool isSpecialized = Coercion(globalObject, *this).run(object, to, name.uid());
        RETURN_IF_EXCEPTION(scope, false);
        if (!isSpecialized)
            continue;
        object->didCreateField(*this, created);
        bool didStore = object->store(vm, created, value);
        RELEASE_ASSERT(didStore);
        return true;
    }
    return false;
}

const Vector<uint16_t, 2>& State::requiredFieldsAdded(const LayoutData& from, const LayoutData& to)
{
    return m_requiredFieldsAdded.ensure({ from.shape, to.shape }, [&] {
        VM& vm = *instance.vm;
        Vector<uint16_t, 2> result;
        for (uint32_t i = 0; i < to.numberOfFields; ++i) {
            if (!(to.fields()[i].flags & fieldIsOptional) && !field(vm, from, name(vm, to.fields()[i].name).impl()))
                result.append(static_cast<uint16_t>(i));
        }
        return result;
    }).iterator->value;
}

static Coercion coercionFor(JSGlobalObject* globalObject, State& state, Format::CastReason reason, Coercion::Explains explains)
{
    bool isNarrowing = reason == Format::CastReason::Flow;
    bool isCoercion = reason == Format::CastReason::Coerce || reason == Format::CastReason::CoerceFreshObject || reason == Format::CastReason::CoerceFreshGraph;
    bool isCast = reason == Format::CastReason::As || isCoercion || isNarrowing;
    if (!isCast)
        return Coercion(globalObject, state, Coercion::Mode::Test, Coercion::Completeness::HasAddedFields, explains);
    return Coercion(globalObject, state, isNarrowing ? Coercion::Mode::Narrowing : reason == Format::CastReason::Coerce ? Coercion::Mode::Coerce : reason == Format::CastReason::CoerceFreshObject ? Coercion::Mode::CoerceFreshObject : isCoercion ? Coercion::Mode::CoerceFreshGraph : Coercion::Mode::Cast, Coercion::Completeness::MayLackAddedFields, explains);
}

TransitionPlan* State::transitionPlan(JSGlobalObject* globalObject, Object* object, const RuntimeType& type, Format::CastReason reason)
{
    const LayoutData& from = object->layout();
    EncodedJSValue discriminant = from.discriminant() != LayoutData::noBase ? JSValue::encode(object->read(from.fields()[from.discriminant()])) : 0;
    CastOutcome outcome = castOutcome(globalObject, object, type, reason, discriminant, false);
    m_lastPlanDependsOnlyOnStructure = !outcome.discriminatingField && from.discriminant() == LayoutData::noBase;
    if (!outcome.discriminatingField) [[likely]]
        return outcome.plan;
    auto heldValueClass = discriminantKeyFor(*outcome.discriminatingField, object->read(*outcome.discriminatingField->field));
    return heldValueClass ? castOutcome(globalObject, object, type, reason, *heldValueClass, true).plan : nullptr;
}

std::optional<EncodedJSValue> State::discriminantKeyFor(const DiscriminatingField& deciding, JSValue value)
{
    auto codeOf = [](uint32_t string) {
        return static_cast<EncodedJSValue>(static_cast<uint64_t>(string) << 4 | 1);
    };
    constexpr uint32_t isAbsent = UINT32_MAX;
    constexpr uint32_t isUnlistedString = UINT32_MAX - 1;
    if (!value)
        return codeOf(isAbsent);
    if (value.isNumber()) {
        EncodedJSValue number = JSValue::encode(jsNumber(value.asNumber()));
        return deciding.numbers.contains(number) ? std::optional(number) : std::nullopt;
    }
    if (value.isUndefinedOrNull() || value.isBoolean())
        return JSValue::encode(value);
    if (!value.isString())
        return std::nullopt;
    auto written = asString(value)->tryGetValue();
    if (written->isNull())
        return std::nullopt;
    auto listed = deciding.strings.find(written.data);
    if (listed != deciding.strings.end())
        return codeOf(listed->value);
    for (const HashedString* hashed : deciding.hashedStrings) {
        if (hashed->matches(written.data))
            return codeOf(hashed->string);
    }
    return codeOf(isUnlistedString);
}

State::CastOutcome State::castOutcome(JSGlobalObject* globalObject, Object* object, const RuntimeType& type, Format::CastReason reason, EncodedJSValue discriminant, bool isDiscriminantKey)
{
    VM& vm = globalObject->vm();
    Structure* structure = object->structure();
    uint64_t key = std::bit_cast<uintptr_t>(structure) ^ std::rotl(static_cast<uint64_t>(std::bit_cast<uintptr_t>(&type)), 23) ^ static_cast<uint64_t>(discriminant) * 31 ^ static_cast<uint64_t>(reason) << 57;
    CachedTransition& cached = m_cachedTransitions[m_cachedTransitionIndices.placeOf(key)];
    if (cached.structure == structure && cached.type == &type && cached.discriminant == discriminant && cached.reason == reason) [[likely]]
        return cached.outcome;
    if (m_castOutcomes.size() >= maxNumberOfOutcomes) [[unlikely]] {
        m_castOutcomes.clear();
        zeroSpan(std::span { m_cachedTransitions, m_cachedTransitionIndices.count() });
        zeroSpan(std::span { m_previousTransitions, m_previousTransitionIndices.count() });
        zeroSpan(std::span { m_castTransitions, static_cast<size_t>(1u << CastTransition::indexBits) });
        m_planClearedSlots.clear();
        m_discriminatingFields.clear();
    }
    CastOutcome outcome = m_castOutcomes.ensure({ structure, &type, discriminant, static_cast<unsigned>(reason) }, [&]() -> CastOutcome {
        if (m_planStructures.add(structure).isNewEntry)
            didCreate(structure);
        auto decides = makeUnique<DiscriminatingField>();
        auto shape = coercionFor(globalObject, *this, reason, Coercion::Explains::No).outcomeByStructure(object, type, reason == Format::CastReason::Coerce, isDiscriminantKey, *decides);
        if (shape)
            return { *shape ? &transitionPlanFor(vm, object, *shape) : &m_planToStay, nullptr };
        if (!decides->field)
            return { };
        m_discriminatingFields.append(WTF::move(decides));
        return { nullptr, m_discriminatingFields.last().get() };
    }).iterator->value;
    if (cached.structure)
        m_cachedTransitionIndices.didReplaceEntry();
    cached = { structure, &type, discriminant, reason, outcome };
    return outcome;
}

const State::CoercionPlan* State::coercionPlan(Structure* structure, const RuntimeType& type) const
{
    if (m_lastCoercionPlanLookup.structure == structure && m_lastCoercionPlanLookup.type == &type)
        return m_lastCoercionPlanLookup.plan;
    auto found = m_coercionPlans.find({ structure, &type });
    m_lastCoercionPlanLookup = { structure, &type, found == m_coercionPlans.end() ? nullptr : found->value.get() };
    return m_lastCoercionPlanLookup.plan;
}

const State::CoercionPlan* State::coercionPlan(JSObject* object, const RuntimeType& type) const
{
    const CoercionPlan* found = coercionPlan(object->structure(), type);
    while (found && found->discriminant != invalidOffset) {
        JSValue held = object->getDirect(found->discriminant);
        if (!held.isString() || asString(held)->isRope())
            return found;
        auto next = found->byDiscriminant.find(asString(held)->tryGetValue());
        if (next == found->byDiscriminant.end())
            return found->byDiscriminant.size() < CoercionPlan::maxNumberByDiscriminant && m_coercionPlansByDiscriminantCount < maxCoercionPlansByDiscriminant ? nullptr : found;
        found = next->value.get();
    }
    return found;
}

void State::addCoercionPlan(JSObject* object, Structure* structure, const RuntimeType& type, std::unique_ptr<CoercionPlan>&& plan, const CoercionPlan::Discriminants& discriminants)
{
    if (m_coercionPlans.size() >= maxCoercionPlans)
        return;
    m_lastCoercionPlanLookup = { };
    m_hasNewCoercionPlans = true;
    if (discriminants.isEmpty()) {
        m_coercionPlans.add({ structure, &type }, WTF::move(plan));
        return;
    }
    auto planWithDiscriminant = [](PropertyOffset discriminant) {
        auto made = makeUnique<CoercionPlan>();
        made->discriminant = discriminant;
        return made;
    };
    CoercionPlan* at = m_coercionPlans.ensure({ structure, &type }, [&] {
        return planWithDiscriminant(discriminants[0]);
    }).iterator->value.get();
    for (size_t i = 0; i < discriminants.size(); ++i) {
        JSValue held = object->getDirect(discriminants[i]);
        if (at->discriminant != discriminants[i] || !held.isString() || asString(held)->isRope())
            return;
        auto text = asString(held)->tryGetValue();
        auto next = at->byDiscriminant.find(text);
        if (next != at->byDiscriminant.end()) {
            at = next->value.get();
            continue;
        }
        if (at->byDiscriminant.size() >= CoercionPlan::maxNumberByDiscriminant || m_coercionPlansByDiscriminantCount >= maxCoercionPlansByDiscriminant)
            return;
        ++m_coercionPlansByDiscriminantCount;
        bool isLast = i + 1 == discriminants.size();
        at = at->byDiscriminant.add(text->isolatedCopy(), isLast ? WTF::move(plan) : planWithDiscriminant(discriminants[i + 1])).iterator->value.get();
    }
}

uint8_t State::kindsAcceptedBy(const RuntimeType& type) const
{
    uint8_t kinds = 0;
    if (type.primitives & Primitive::Int && type.minInteger() <= INT32_MIN && type.maxInteger() >= INT32_MAX)
        kinds |= CoercionPlan::acceptsInt32;
    if (type.primitives & Primitive::String)
        kinds |= CoercionPlan::acceptsString;
    if (type.primitives & Primitive::True)
        kinds |= CoercionPlan::acceptsTrue;
    if (type.primitives & Primitive::False)
        kinds |= CoercionPlan::acceptsFalse;
    if (type.primitives & Primitive::Undefined)
        kinds |= CoercionPlan::acceptsUndefined;
    if (type.primitives & Primitive::Null)
        kinds |= CoercionPlan::acceptsNull;
    if ((type.primitives & Primitive::Number) == Primitive::Number && type.minInteger() <= -maxExactInteger && type.maxInteger() >= maxExactInteger)
        kinds |= CoercionPlan::acceptsDouble;
    if (type.acceptsUntypedObjectsByNature()) {
        bool mayConvertProperties = false;
        auto look = [&](uint32_t shape) {
            const LayoutData& layout = layoutForShape(shape);
            for (uint32_t i = 0; layout.kind == LayoutKind::Record && i < layout.numberOfFields && !mayConvertProperties; ++i)
                mayConvertProperties = ownTypeOf(layout.fields()[i]).numberOfSupertypes();
        };
        for (uint32_t i = 0; i < type.numberOfSupertypes(); ++i) {
            auto& view = type.view(i);
            if (view.supertypeShape) {
                look(view.supertypeShape);
                continue;
            }
            for (uint32_t shape = view.first; shape < view.first + view.count; ++shape) {
                if (passes(view, shape) && hasLayoutForShape(shape))
                    look(shape);
            }
        }
        if (!mayConvertProperties)
            kinds |= CoercionPlan::takesUntypedObject;
    }
    return kinds;
}

bool State::hasCapacityToConvertInPlace(JSObject* object, Object* copy)
{
    return !object->structure()->mayBePrototype() && object->cellSize() >= copy->layout().size;
}

void State::become(VM& vm, JSObject* object, Object* copy)
{
    Structure* structure = copy->structure();
    size_t sizeOfCell = object->cellSize();
    size_t size = std::min<size_t>(sizeWithSpecializationCapacity(copy->shape()), sizeOfCell);
    RELEASE_ASSERT(size >= sizeof(JSCell) && size >= copy->layout().size && size <= copy->cellSize());
    Object* itself = std::bit_cast<Object*>(object);
    JSObject* extra = copy->extraProperties();
    auto move = [&] {
        uint8_t* cell = std::bit_cast<uint8_t*>(object);
        memcpySpan(std::span { cell + sizeof(JSCell), size - sizeof(JSCell) }, std::span { std::bit_cast<const uint8_t*>(copy) + sizeof(JSCell), size - sizeof(JSCell) });
        zeroSpan(std::span { cell + size, sizeOfCell - size });
        uint32_t offsetOfExtraProperties = structure->compiledLayout()->offsetOfExtraProperties();
        if (extra && offsetOfExtraProperties)
            *reinterpret_cast<uint32_t*>(cell + offsetOfExtraProperties) = MarkedBlockCage::encode(extra);
        if (copy->extraPropertiesHandle())
            extraPropertiesHandleTable.didReplaceObject(copy, copy->at<uint32_t>(offsetOfOrderWord) & orderAndCountMask, itself);
        object->JSCell::setStructure(vm, structure);
        if (extra && isCounting()) [[unlikely]] {
            removeExtraPropertiesOf(copy);
            extraProperties.set(itself, extra);
        }
    };
    if (vm.heap.mutatorShouldBeFenced()) [[unlikely]] {
        PreventCollectionScope preventCollection(vm.heap);
        move();
    } else
        move();
    if (auto* attributes = fieldAttributes.get(copy)) [[unlikely]] {
        FieldAttributesTable::Attributes moved { attributes->isFrozen, attributes->attributesByField };
        fieldAttributes.remove(copy);
        auto& own = fieldAttributes.ensure(itself);
        own.isFrozen = moved.isFrozen;
        own.attributesByField = WTF::move(moved.attributesByField);
    }
    vm.writeBarrier(object);
    itself->copyNumberBoxes(*this);
    ensureStillAliveHere(copy);
    ensureStillAliveHere(extra);
}

Object* State::tryConvertOrCopyByPlan(VM& vm, JSObject* object, const CoercionPlan& plan, bool mayConvertInPlace)
{
    static constexpr size_t maxSize = CoercionPlan::maxInPlaceConversionSize;
    size_t size = plan.initialBody.size() + sizeof(JSCell);
    if (plan.initialBody.isEmpty())
        return nullptr;
    size_t sizeOfCell = object->cellSize();
    bool convertsInPlace = mayConvertInPlace && !object->structure()->mayBePrototype() && sizeOfCell >= plan.layout->size;
    bool hasExtraProperties = !plan.extraProperties.isEmpty();
    alignas(sizeof(EncodedJSValue)) uint8_t newCellBytes[maxSize];
    memcpySpan(std::span { newCellBytes + sizeof(JSCell), size - sizeof(JSCell) }, plan.initialBody.span());
    if (hasExtraProperties && convertsInPlace)
        *reinterpret_cast<uint32_t*>(newCellBytes + offsetOfOrderWord) |= orderWordHasExtraProperties;
    for (auto& planned : plan.fields) {
        const FieldData& field = *planned.field;
        JSValue value = object->getDirect(planned.offset);
        void* slot = newCellBytes + field.offset;
        if (field.store() != Store::Int32OrNumberBox) {
            if (!storeAs(*this, field.store(), slot, value))
                return nullptr;
            continue;
        }
        if (value.isInt32() && value.asInt32() >= minInlineInt32)
            *static_cast<int32_t*>(slot) = value.asInt32();
        else if (value.isUndefinedOrNull())
            *static_cast<int32_t*>(slot) = value.isUndefined() ? undefinedInt32 : nullInt32;
        else
            return nullptr;
    }
    Structure* structure = structureFor(vm, plan.layout->shape, object->getPrototypeDirect(), hasExtraProperties && convertsInPlace);
    for (auto& planned : plan.fields) {
        if (planned.field->flags & fieldIsPinned) [[unlikely]]
            willModify(*planned.field, object->getDirect(planned.offset));
    }
    JSObject* extra = nullptr;
    if (hasExtraProperties) {
        if (plan.extraProperties.size() == 1) {
            extra = constructEmptyObject(vm, structure->realm()->nullPrototypeObjectStructure());
            extra->putDirect(vm, plan.extraProperties[0].second, object->getDirect(plan.extraProperties[0].first));
        } else {
            Vector<Object::ExtraProperty, 8> properties;
            for (auto& [offset, name] : plan.extraProperties)
                properties.append({ name, object->getDirect(offset) });
            extra = Object::createExtraProperties(structure->realm(), properties.span());
        }
        if (convertsInPlace)
            structure = structureFor(vm, plan.layout->shape, object->getPrototypeDirect(), *extra->structure());
    }
    if (!convertsInPlace) {
        Object* copy = Object::createWithBody(vm, structure, std::span { newCellBytes + sizeof(JSCell), size - sizeof(JSCell) });
        if (extra)
            copy->setExtraProperties(vm, extra);
        ensureStillAliveHere(object);
        countObject(copy);
        return copy;
    }
    if (extra && !plan.layout->offsetOfExtraProperties()) {
        uint32_t& word = *reinterpret_cast<uint32_t*>(newCellBytes + offsetOfOrderWord);
        word = (word & ~orderAndCountMask) | extraPropertiesHandleTable.take(object, extra, word & orderAndCountMask);
    }
    auto move = [&] {
        uint8_t* cell = std::bit_cast<uint8_t*>(object);
        size_t moved = std::min(size, sizeOfCell);
        memcpySpan(std::span { cell + sizeof(JSCell), moved - sizeof(JSCell) }, std::span { newCellBytes + sizeof(JSCell), moved - sizeof(JSCell) });
        zeroSpan(std::span { cell + moved, sizeOfCell - moved });
        if (uint32_t offset = extra ? plan.layout->offsetOfExtraProperties() : 0)
            *reinterpret_cast<uint32_t*>(cell + offset) = MarkedBlockCage::encode(extra);
        object->JSCell::setStructure(vm, structure);
    };
    if (vm.heap.mutatorShouldBeFenced()) [[unlikely]] {
        PreventCollectionScope preventCollection(vm.heap);
        move();
    } else
        move();
    if (extra && isCounting()) [[unlikely]]
        extraProperties.set(std::bit_cast<Object*>(object), extra);
    vm.writeBarrier(object);
    countObject(std::bit_cast<Object*>(object));
    return std::bit_cast<Object*>(object);
}

bool State::tryConvertInPlaceByPlans(VM& vm, JSObject* top, const RuntimeType& typeOfTop)
{
    struct ConvertedInPlace {
        JSObject* object;
        const CoercionPlan* plan;
        size_t body;
        Structure* structure;
        Structure* before;
        const RuntimeType* type;
    };
    static constexpr unsigned maxNumber = 32;
    Vector<ConvertedInPlace, maxNumber> all;
    Vector<uint64_t, 64> bodies;
    auto lay = [&](auto& lay, JSObject* object, const RuntimeType& type) -> bool {
        for (auto& known : all) {
            if (known.object == object)
                return acceptsShape(type, known.plan->layout->shape);
        }
        const CoercionPlan* plan = coercionPlan(object, type);
        if (!plan)
            plan = firstCoercionPlanForShape(vm, object, type);
        if (!plan || !plan->layout || plan->initialBody.isEmpty() || !plan->extraProperties.isEmpty() || all.size() >= maxNumber || object->structure()->mayBePrototype() || object->cellSize() < plan->layout->size)
            return false;
        size_t body = bodies.size();
        bodies.grow(body + WTF::roundUpToMultipleOf<sizeof(uint64_t)>(plan->initialBody.size()) / sizeof(uint64_t));
        memcpySpan(asMutableByteSpan(bodies.mutableSpan().subspan(body)).first(plan->initialBody.size()), plan->initialBody.span());
        all.append({ object, plan, body, nullptr, object->structure(), &type });
        for (auto& planned : plan->fields) {
            const FieldData& field = *planned.field;
            JSValue value = object->getDirect(planned.offset);
            if (!(planned.acceptedKinds & CoercionPlan::kindOf(value))) {
                const RuntimeType& ofField = ownTypeOf(field);
                if (!value.isObject()) {
                    if (!fits(ofField, value) || !isAllowedLiteral(field, value))
                        return false;
                } else if (Object* compiled = compiledObject(value)) {
                    bool isOfType = ofField.allowsAnyObject() || acceptsShape(ofField, compiled->shape());
                    if (!isOfType || (layoutForShape(compiled->shape()).numberOfSpecializations() && !ofField.allowsAnyObject()))
                        return false;
                } else if (asObject(value)->classInfo() == ClassPrototype::info())
                    return false;
                else if (!(planned.acceptedKinds & CoercionPlan::takesUntypedObject) && !matchesTypedSubtype(ofField, asObject(value)) && (ofField.acceptsUntypedObjectsByNature() || asObject(value)->type() != FinalObjectType || !lay(lay, asObject(value), ofField)))
                    return false;
            }
            void* slot = reinterpret_cast<uint8_t*>(bodies.mutableSpan().data() + body) + field.offset - sizeof(JSCell);
            if (field.store() != Store::Int32OrNumberBox) {
                if (!storeAs(*this, field.store(), slot, value))
                    return false;
                continue;
            }
            if (value.isInt32() && value.asInt32() >= minInlineInt32)
                *static_cast<int32_t*>(slot) = value.asInt32();
            else if (value.isUndefinedOrNull())
                *static_cast<int32_t*>(slot) = value.isUndefined() ? undefinedInt32 : nullInt32;
            else
                return false;
        }
        return true;
    };
    if (!lay(lay, top, typeOfTop))
        return false;
    for (auto& made : all)
        made.structure = structureFor(vm, made.plan->layout->shape, made.object->getPrototypeDirect());
    for (auto& made : all) {
        for (auto& planned : made.plan->fields) {
            if (planned.field->flags & fieldIsPinned) [[unlikely]]
                willModify(*planned.field, made.object->getDirect(planned.offset));
        }
    }
    auto move = [&] {
        for (auto& made : all) {
            uint8_t* cell = std::bit_cast<uint8_t*>(made.object);
            size_t size = std::min(made.plan->initialBody.size() + sizeof(JSCell), made.object->cellSize());
            memcpySpan(std::span { cell + sizeof(JSCell), size - sizeof(JSCell) }, asByteSpan(bodies.span().subspan(made.body)).first(size - sizeof(JSCell)));
            zeroSpan(std::span { cell + size, made.object->cellSize() - size });
            made.object->JSCell::setStructure(vm, made.structure);
        }
    };
    if (vm.heap.mutatorShouldBeFenced()) [[unlikely]] {
        PreventCollectionScope preventCollection(vm.heap);
        move();
    } else
        move();
    for (auto& made : all) {
        vm.writeBarrier(made.object);
        countObject(std::bit_cast<Object*>(made.object));
        if (made.plan->graphConversionEpoch != m_quickConversionEpoch && coercionPlan(made.before, *made.type) == made.plan) {
            made.plan->graphConversionEpoch = m_quickConversionEpoch;
            cacheQuickConversion(made.before, *made.type, *made.plan, *made.plan, 0, std::bit_cast<Object*>(made.object), WTF::roundUpToMultipleOf<MarkedBlock::atomSize>(JSFinalObject::allocationSize(made.before->inlineCapacity())), 0);
        }
    }
    return true;
}

uint64_t State::discriminantWord(JSValue value)
{
    if (!value.isString() || asString(value)->isRope())
        return 0;
    const StringImpl* impl = asString(value)->tryGetValueImpl();
    if (!impl || !impl->is8Bit() || impl->length() > 7)
        return 0;
    uint64_t word = impl->length() + 1;
    auto characters = impl->span8();
    for (size_t i = 0; i < characters.size(); ++i)
        word |= static_cast<uint64_t>(characters[i]) << (8 * (i + 1));
    return word;
}

auto State::quickConversion(uint32_t structureID, const RuntimeType& type, uint64_t discriminant, bool evictsIfFull) -> QuickConversion*
{
    uint32_t hash = structureID * 0x9e3779b1u ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(&type) >> 3) ^ static_cast<uint32_t>(discriminant * 0x9e3779b97f4a7c15ull >> 32);
    hash ^= hash >> 16;
    QuickConversion* entries = m_quickConversions->data();
    for (unsigned i = 0; i < 4; ++i) {
        QuickConversion* entry = entries + (hash + i) % numberOfQuickConversions;
        if (entry->structureID == structureID && entry->type == &type && entry->discriminant == discriminant)
            return entry;
        if (evictsIfFull && !entry->structureID)
            return entry;
    }
    return evictsIfFull ? entries + hash % numberOfQuickConversions : nullptr;
}

static unsigned widthOf(Store store)
{
    switch (store) {
    case Store::Uint8:
    case Store::Int8:
    case Store::Boolean:
    case Store::OptionalBoolean:
        return 1;
    case Store::Uint16:
    case Store::Int16:
        return 2;
    case Store::Int32:
    case Store::Uint32:
    case Store::Float:
    case Store::Int32OrNumberBox:
    case Store::Cell32:
    case Store::OptionalCell32:
    case Store::LiteralString32:
        return 4;
    case Store::Int64:
    case Store::Double:
    case Store::Cell:
    case Store::JSValue:
    case Store::OptionalInt64:
    case Store::OptionalDouble:
    case Store::LiteralString:
        return 8;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<uint32_t> State::literalStringWordOfConstant(uint32_t string) const
{
    auto characters = byteCast<Latin1Character>(data.stringInImage(string));
    if (characters.empty())
        return emptyLiteralStringWord;
    if (!(data.stringFacts(string) & Data::stringIsASCII))
        return std::nullopt;
    uint32_t hash = StringHasher::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    auto* table = data.at<uint32_t>(data.literalStringTableOffset);
    constexpr uint32_t indexMask = (1u << Data::literalStringTableIndexBits) - 1;
    for (uint32_t place = hash & data.literalStringTableMask;; place = (place + 1) & data.literalStringTableMask) {
        uint32_t entry = table[place];
        if (!entry)
            return std::nullopt;
        uint32_t index = (entry & indexMask) - 1;
        if (entry != Data::literalStringTableEntry(index, hash))
            continue;
        uint32_t id = data.literalStringID(index);
        if (data.stringFacts(id) & Data::stringIsASCII && std::ranges::equal(byteCast<Latin1Character>(data.stringInImage(id)), characters))
            return literalStringWord(index);
    }
}

std::unique_ptr<ConversionPlan> State::createConversionPlan(const CoercionPlan& plan, size_t sizeOfCell, uint64_t header, void* allocator, PropertyOffset discriminant, const Object* converted)
{
    using Kind = ConversionMove::Kind;
    constexpr uint8_t number = CoercionPlan::acceptsInt32 | CoercionPlan::acceptsDouble;
    constexpr uint8_t boolean = CoercionPlan::acceptsTrue | CoercionPlan::acceptsFalse;
    if (sizeOfCell > ConversionPlan::maxCellSize || sizeOfCell % (2 * sizeof(uint64_t)) || sizeOfCell < plan.layout->size)
        return nullptr;
    auto made = makeUnique<ConversionPlan>();
    const FieldData* discriminantField = nullptr;
    for (auto& planned : plan.fields) {
        const FieldData& field = *planned.field;
        const RuntimeType& type = ownTypeOf(field);
        uint8_t kinds = planned.acceptedKinds;
        if (field.flags & fieldIsOutOfLine)
            return nullptr;
        bool isOutOfLine = !isInlineOffset(planned.offset);
        size_t distanceBelowButterfly = isOutOfLine ? (planned.offset - firstOutOfLineOffset + 2) * sizeof(EncodedJSValue) : 0;
        if (distanceBelowButterfly > ConversionMove::outOfLineBias || (isOutOfLine && !allocator))
            return nullptr;
        uint16_t from = static_cast<uint16_t>(isOutOfLine ? ConversionMove::outOfLineBias - distanceBelowButterfly : JSObject::offsetOfInlineStorage() + offsetInInlineStorage(planned.offset) * sizeof(EncodedJSValue));
        if (planned.offset == discriminant && converted) {
            discriminantField = &field;
            continue;
        }
        if (field.flags & fieldIsPinned)
            return nullptr;
        Store store = field.store();
        if (field.numberOfLiterals()) {
            if ((store != Store::LiteralString && store != Store::LiteralString32) || made->texts.size() + field.numberOfLiterals() > ConversionPlan::maxTextCount)
                return nullptr;
            ConversionMove move { from, static_cast<uint16_t>(field.offset), Kind::LiteralString, 0, 0, isOutOfLine, static_cast<uint32_t>(made->texts.size()), field.numberOfLiterals() };
            move.flags = (kinds & CoercionPlan::acceptsUndefined ? ConversionMove::acceptsUndefined : 0) | (kinds & CoercionPlan::acceptsNull ? ConversionMove::acceptsNull : 0) | (store == Store::LiteralString ? ConversionMove::isWide : 0);
            for (auto& literal : std::span { data.at<LiteralData>(field.literalsOffset()), field.numberOfLiterals() }) {
                if (literal.isNumber())
                    return nullptr;
                auto characters = byteCast<Latin1Character>(data.stringInImage(literal.string()));
                if (characters.size() > 7 || (!characters.empty() && !(data.stringFacts(literal.string()) & Data::stringIsASCII)))
                    return nullptr;
                auto word = literalStringWordOfConstant(literal.string());
                uint64_t text = characters.size() + 1;
                for (size_t i = 0; i < characters.size(); ++i)
                    text |= static_cast<uint64_t>(characters[i]) << (8 * (i + 1));
                made->texts.append({ text, word.value_or(0) });
            }
            made->moves.append(move);
            continue;
        }
        ConversionMove move { from, static_cast<uint16_t>(field.offset), Kind::Value, 0, 0, isOutOfLine, 0, 0 };
        bool holdsAnything = store == Store::JSValue && type.allowsAnyObject() && type.allowsAnyFunction() && (type.primitives & Primitive::All) == Primitive::All && (kinds & number) == number;
        if (holdsAnything) {
            made->moves.append(move);
            continue;
        }
        if (store == Store::OptionalCell32 || store == Store::JSValue || store == Store::Int32OrNumberBox || store == Store::OptionalDouble || store == Store::OptionalBoolean)
            move.flags = (kinds & CoercionPlan::acceptsUndefined ? ConversionMove::acceptsUndefined : 0) | (kinds & CoercionPlan::acceptsNull ? ConversionMove::acceptsNull : 0);
        switch (store) {
        case Store::Double:
        case Store::OptionalDouble:
            if ((kinds & number) != number)
                return nullptr;
            move.kind = Kind::NumberAsDouble;
            break;
        case Store::Int32OrNumberBox:
            if ((kinds & number) != number)
                return nullptr;
            move.kind = Kind::NumberAsInlineInt32;
            break;
        case Store::Boolean:
        case Store::OptionalBoolean:
            if ((kinds & boolean) != boolean)
                return nullptr;
            move.kind = store == Store::Boolean ? Kind::BooleanAsByte : Kind::BooleanAsValueByte;
            break;
        case Store::Cell:
        case Store::JSValue:
        case Store::Cell32:
        case Store::OptionalCell32: {
            if (store == Store::Cell || store == Store::JSValue)
                move.flags |= ConversionMove::isWide;
            if (type.allowsAnyObject())
                move.kind = Kind::Object;
            else if (kinds & CoercionPlan::acceptsString) {
                move.kind = Kind::CellOfType;
                move.cellType = StringType;
            } else if (type.allowsAnyFunction())
                move.kind = Kind::Function;
            else if (type.numberOfJSTypes()) {
                move.kind = Kind::CellOfType;
                move.cellType = type.jsTypes()[0];
            } else if (type.numberOfSupertypes() && type.view(0).set == UINT32_MAX && subtypesHaveNoSpecializations(type)) {
                move.kind = Kind::CellOfShape;
                move.lastShape = type.view(0).first + type.view(0).count - 1;
                move.shapeCount = type.view(0).count;
            } else
                return nullptr;
            break;
        }
        default:
            return nullptr;
        }
        made->moves.append(move);
    }
    if (size_t firstOutOfLine = made->moves.findIf([](auto& move) { return move.isOutOfLine; }); firstOutOfLine != notFound) {
        std::ranges::stable_partition(made->moves, [](auto& move) { return !move.isOutOfLine; });
        firstOutOfLine = made->moves.findIf([](auto& move) { return move.isOutOfLine; });
        made->moves.insert(firstOutOfLine, ConversionMove { 0, 0, Kind::SwitchSource, 0, 0, 0, 0, 0 });
        made->moves.append(ConversionMove { made->moves.last().from, 0, Kind::SwitchSource, 0, 0, 0, 0, 0 });
    }
    made->cell.fill(0, sizeOfCell / sizeof(uint64_t));
    made->cell[0] = header;
    size_t bodySize = std::min(plan.initialBody.size(), sizeOfCell - sizeof(JSCell));
    memcpySpan(asMutableByteSpan(made->cell.mutableSpan().subspan(1)).first(bodySize), plan.initialBody.span().first(bodySize));
    if (discriminantField)
        memcpySpan(asMutableByteSpan(made->cell.mutableSpan()).subspan(discriminantField->offset, widthOf(discriminantField->store())), std::span { std::bit_cast<const uint8_t*>(converted) + discriminantField->offset, widthOf(discriminantField->store()) });
    made->firstText = made->texts.span().data();
    made->allocator = allocator;
    made->words = made->cell.span().data();
    made->wordCount = made->cell.size();
    made->firstMove = made->moves.span().data();
    made->moveCount = made->moves.size();
    return made;
}

void State::cacheConversion(uint32_t structureID, uint32_t typeAndReason, const ConversionPlan& plan)
{
    static_cast<ConversionCacheEntry*>(instance.typedConversionCache)[ConversionCacheEntry::placeOf(structureID, typeAndReason)] = { structureID, typeAndReason, &plan };
    m_hasCachedConversions = true;
}

void State::cacheCopyConversion(VM& vm, Structure* structure, const RuntimeType& type, const CoercionPlan& plan, Structure* made, uint32_t typeAndReason)
{
    if (plan.copyConversionEpoch == m_quickConversionEpoch)
        return;
    plan.copyConversionEpoch = m_quickConversionEpoch;
    if (m_counts || data.isCompiledForAudit || plan.initialBody.isEmpty() || !plan.extraProperties.isEmpty() || coercionPlan(structure, type) != &plan || structures[plan.layout->shape] != made->id().bits())
        return;
    if (!plan.copyConversion) {
        size_t size = WTF::roundUpToMultipleOf<MarkedBlock::atomSize>(plan.initialBody.size() + sizeof(JSCell));
        void* allocator = vm.cellSpace().allocatorFor(size, AllocatorForMode::EnsureAllocator).localAllocator();
        if (!allocator)
            return;
        plan.copyConversion = createConversionPlan(plan, size, static_cast<uint64_t>(made->typeInfoBlob()) << 32 | made->id().bits(), allocator);
    }
    if (plan.copyConversion)
        cacheConversion(structure->id().bits(), typeAndReason, *plan.copyConversion);
}

void State::cacheQuickConversion(Structure* structure, const RuntimeType& type, const CoercionPlan& root, const CoercionPlan& plan, uint64_t discriminant, Object* converted, size_t sizeOfCell, uint32_t typeAndReason)
{
    Structure* made = converted->structure();
    size_t bodySize = plan.initialBody.size();
    if (m_counts || data.isCompiledForAudit || !bodySize || sizeOfCell % 16 || sizeOfCell < 32 || sizeOfCell > maxSizeOfQuickConversion || sizeOfCell < plan.layout->size || plan.fields.size() > CoercionPlan::maxNumberOfQuickMoves || !plan.extraProperties.isEmpty() || structure->outOfLineCapacity())
        return;
    if (structures[plan.layout->shape] != made->id().bits())
        return;
    bool dependsOnDiscriminant = &root != &plan;
    if (dependsOnDiscriminant && (!discriminant || !isInlineOffset(root.discriminant)))
        return;
    for (auto& planned : plan.fields) {
        if (!isInlineOffset(planned.offset))
            return;
        if (planned.field->flags & fieldIsPinned && !(dependsOnDiscriminant && planned.offset == root.discriminant))
            return;
    }
    if (plan.quickBody.size() != sizeOfCell - sizeof(JSCell)) {
        using Move = CoercionPlan::QuickMove;
        plan.quickBody.fill(0, sizeOfCell - sizeof(JSCell));
        memcpySpan(plan.quickBody.mutableSpan().first(std::min(bodySize, plan.quickBody.size())), plan.initialBody.span().first(std::min(bodySize, plan.quickBody.size())));
        plan.quickMoves.shrink(0);
        for (auto& planned : plan.fields) {
            Store store = planned.field->store();
            uint8_t kinds = planned.acceptedKinds;
            Move::Kind kind = Move::Kind::Any;
            if (kinds == CoercionPlan::acceptsString && (store == Store::JSValue || store == Store::Cell))
                kind = Move::Kind::StringAsValue;
            else if (kinds == CoercionPlan::acceptsString && store == Store::Cell32)
                kind = Move::Kind::StringAsCell32;
            else if (kinds == (CoercionPlan::acceptsInt32 | CoercionPlan::acceptsDouble) && store == Store::Int32OrNumberBox)
                kind = Move::Kind::InlineInt32;
            else if (kinds == (CoercionPlan::acceptsInt32 | CoercionPlan::acceptsDouble) && store == Store::Double)
                kind = Move::Kind::NumberAsDouble;
            else if (kinds == (CoercionPlan::acceptsTrue | CoercionPlan::acceptsFalse) && store == Store::Boolean)
                kind = Move::Kind::BooleanAsByte;
            plan.quickMoves.append({ static_cast<uint16_t>(JSObject::offsetOfInlineStorage() + offsetInInlineStorage(planned.offset) * sizeof(EncodedJSValue)), static_cast<uint16_t>(planned.field->offset), kind, static_cast<uint8_t>(widthOf(store)), kinds, store });
        }
    }
    if (!m_quickConversions)
        m_quickConversions = makeUniqueWithoutFastMallocCheck<std::array<QuickConversion, numberOfQuickConversions>>();
    uint32_t structureID = structure->id().bits();
    if (dependsOnDiscriminant)
        *quickConversion(structureID, type, 0, true) = { structureID, root.discriminant, &type, 0, nullptr, 0, 0 };
    uint64_t header = static_cast<uint64_t>(made->typeInfoBlob() & 0xffffff) << 32 | made->id().bits();
    *quickConversion(structureID, type, dependsOnDiscriminant ? discriminant : 0, true) = { structureID, invalidOffset, &type, dependsOnDiscriminant ? discriminant : 0, &plan, header, static_cast<uint32_t>(sizeOfCell) };
    if (!typeAndReason)
        return;
    if (!plan.inPlaceConversion)
        plan.inPlaceConversion = createConversionPlan(plan, sizeOfCell, header, nullptr, dependsOnDiscriminant ? root.discriminant : invalidOffset, converted);
    if (!plan.inPlaceConversion)
        return;
    if (!dependsOnDiscriminant) {
        cacheConversion(structureID, typeAndReason, *plan.inPlaceConversion);
        return;
    }
    if (!root.inPlaceConversion) {
        root.inPlaceConversion = makeUnique<ConversionPlan>();
        root.inPlaceConversion->discriminantFrom = JSObject::offsetOfInlineStorage() + offsetInInlineStorage(root.discriminant) * sizeof(EncodedJSValue);
    }
    ConversionPlan& members = *root.inPlaceConversion;
    if (!members.texts.containsIf([&](auto& member) { return member.text == discriminant; })) {
        if (members.texts.size() >= ConversionPlan::maxTextCount)
            return;
        members.texts.append({ discriminant, std::bit_cast<uint64_t>(plan.inPlaceConversion.get()) });
        members.firstText = members.texts.span().data();
        members.memberCount = members.texts.size();
    }
    cacheConversion(structureID, typeAndReason, members);
}

bool State::canConvertQuickly(JSValue value, const RuntimeType& type, ChildrenOfQuickConversion& seen, unsigned depth)
{
    if (!value.isCell() || depth >= maxDepthOfChildConversions)
        return false;
    JSCell* cell = value.asCell();
    for (auto& [object, typeOfObject] : seen) {
        if (object == cell)
            return typeOfObject == &type;
    }
    if (seen.size() == seen.capacity())
        return false;
    seen.append({ cell, &type });
    uint32_t structureID = cell->structureID().bits();
    const QuickConversion* entry = quickConversion(structureID, type, 0, false);
    if (!entry)
        return false;
    if (entry->discriminantOffset != invalidOffset) {
        uint64_t discriminant = discriminantWord(asObject(cell)->getDirect(entry->discriminantOffset));
        if (!discriminant)
            return false;
        entry = quickConversion(structureID, type, discriminant, false);
        if (!entry)
            return false;
    }
    if (!entry->plan || entry->plan->keepsObject)
        return false;
    using Move = CoercionPlan::QuickMove;
    const CoercionPlan& plan = *entry->plan;
    uint8_t* bytes = std::bit_cast<uint8_t*>(cell);
    std::span<const Move> moves = plan.quickMoves.span();
    for (size_t i = 0; i < moves.size(); ++i) {
        const Move& move = moves[i];
        JSValue held = JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(bytes + move.from));
        bool isGood = true;
        switch (move.kind) {
        case Move::Kind::StringAsValue:
        case Move::Kind::StringAsCell32:
            isGood = held.isString();
            break;
        case Move::Kind::InlineInt32:
            isGood = held.isInt32() && held.asInt32() >= minInlineInt32;
            break;
        case Move::Kind::NumberAsDouble:
            isGood = held.isNumber();
            break;
        case Move::Kind::BooleanAsByte:
            isGood = held.isBoolean();
            break;
        case Move::Kind::Any: {
            const FieldData& field = *plan.fields[i].field;
            if (!(move.acceptedKinds & CoercionPlan::kindOf(held)) && !canHoldWithoutConversion(field, move.acceptedKinds, held)) {
                isGood = canConvertQuickly(held, ownTypeOf(field), seen, depth + 1);
                break;
            }
            uint64_t slot = 0;
            if (move.store != Store::Int32OrNumberBox)
                isGood = storeAs(*this, move.store, &slot, held);
            else
                isGood = (held.isInt32() && held.asInt32() >= minInlineInt32) || held.isUndefinedOrNull();
            break;
        }
        }
        if (!isGood)
            return false;
    }
    return true;
}

bool State::canSettleByPlans(JSValue value, const RuntimeType& type, ChildrenOfQuickConversion& seen, ChildrenOfQuickConversion& converted, unsigned depth)
{
    if (!type.acceptsUntypedObjectsByNature()) {
        if (converted.size() == converted.capacity() || !canConvertQuickly(value, type, seen, depth))
            return false;
        converted.append({ value.asCell(), &type });
        return true;
    }
    if (depth >= maxDepthOfChildConversions || !value.isObject() || asObject(value)->type() != FinalObjectType)
        return false;
    JSObject* object = asObject(value);
    for (auto& [known, typeOfKnown] : seen) {
        if (known == object)
            return typeOfKnown == &type;
    }
    if (seen.size() == seen.capacity())
        return false;
    seen.append({ object, &type });
    const CoercionPlan* plan = coercionPlan(object, type);
    if (!plan || !plan->keepsObject)
        return false;
    for (auto& planned : plan->fields) {
        JSValue held = object->getDirect(planned.offset);
        if (planned.acceptedKinds & CoercionPlan::kindOf(held) || canHoldWithoutConversion(*planned.field, planned.acceptedKinds, held))
            continue;
        if (!canSettleByPlans(held, ownTypeOf(*planned.field), seen, converted, depth + 1))
            return false;
    }
    return true;
}

bool State::tryKeepByPlan(VM& vm, JSObject* object, const RuntimeType& type)
{
    if (!m_quickConversions || vm.heap.mutatorShouldBeFenced())
        return false;
    ChildrenOfQuickConversion seen;
    ChildrenOfQuickConversion converted;
    if (!canSettleByPlans(object, type, seen, converted, 0))
        return false;
    for (auto& [below, typeOfBelow] : converted) {
        if (!compiledObject(below) && !tryQuickConversion(vm, below, *typeOfBelow))
            return false;
    }
    Structure* structure = object->structure();
    if (const CoercionPlan* plan = coercionPlan(structure, type); plan && plan->keepsObject)
        *quickConversion(structure->id().bits(), type, 0, true) = { structure->id().bits(), invalidOffset, &type, 0, plan, 0, 0 };
    return true;
}

bool State::keepQuickly(VM& vm, JSObject* object, const CoercionPlan& plan)
{
    ChildrenOfQuickConversion converted;
    for (auto& planned : plan.fields) {
        JSValue held = object->getDirect(planned.offset);
        if (planned.acceptedKinds & CoercionPlan::kindOf(held) || canHoldWithoutConversion(*planned.field, planned.acceptedKinds, held))
            continue;
        const RuntimeType& ofField = ownTypeOf(*planned.field);
        if (!held.isCell() || ofField.acceptsUntypedObjectsByNature() || converted.size() == converted.capacity())
            return false;
        converted.append({ held.asCell(), &ofField });
    }
    if (converted.size() > 1) {
        ChildrenOfQuickConversion seen;
        for (auto& [below, typeOfBelow] : converted) {
            if (!canConvertQuickly(below, *typeOfBelow, seen, 1))
                return false;
        }
    }
    for (auto& [below, typeOfBelow] : converted) {
        if (!compiledObject(below) && !tryQuickConversion(vm, below, *typeOfBelow))
            return false;
    }
    return true;
}

void State::collectObjectsBelowKeptObject(JSObject* object, const RuntimeType& type, ObjectsBelowKeptObject& found, unsigned depth)
{
    const CoercionPlan* plan = depth < maxDepthOfChildConversions ? coercionPlan(object, type) : nullptr;
    if (!plan || (!plan->keepsObject && !plan->layout))
        return;
    for (auto& planned : plan->fields) {
        JSValue held = object->getDirect(planned.offset);
        if (!held.isObject() || asObject(held)->type() != FinalObjectType || found.size() == found.capacity() || found.containsIf([&](auto& known) { return known.object == asObject(held); }))
            continue;
        const RuntimeType& ofField = ownTypeOf(*planned.field);
        if (!ofField.acceptsUntypedObjectsByNature())
            found.append({ asObject(held), asObject(held)->structure(), &ofField });
        collectObjectsBelowKeptObject(asObject(held), ofField, found, depth + 1);
    }
}

void State::cacheConversionsBelowKeptObject(const ObjectsBelowKeptObject& found)
{
    for (auto& below : found) {
        Object* made = compiledObject(below.object);
        const CoercionPlan* plan = made ? coercionPlan(below.structure, *below.type) : nullptr;
        if (!plan || !plan->layout || plan->discriminant != invalidOffset || plan->layout->shape != made->shape() || plan->graphConversionEpoch == m_quickConversionEpoch)
            continue;
        plan->graphConversionEpoch = m_quickConversionEpoch;
        cacheQuickConversion(below.structure, *below.type, *plan, *plan, 0, made, WTF::roundUpToMultipleOf<MarkedBlock::atomSize>(JSFinalObject::allocationSize(below.structure->inlineCapacity())), 0);
    }
}

bool State::tryQuickConversion(VM& vm, JSValue value, const RuntimeType& type)
{
    if (!m_quickConversions || !value.isCell())
        return false;
    JSCell* cell = value.asCell();
    uint32_t structureID = cell->structureID().bits();
    const QuickConversion* entry = quickConversion(structureID, type, 0, false);
    if (!entry)
        return false;
    if (entry->discriminantOffset != invalidOffset) {
        uint64_t discriminant = discriminantWord(asObject(cell)->getDirect(entry->discriminantOffset));
        if (!discriminant)
            return false;
        entry = quickConversion(structureID, type, discriminant, false);
        if (!entry)
            return false;
    }
    if (!entry->plan || vm.heap.mutatorShouldBeFenced()) [[unlikely]]
        return false;
    using Move = CoercionPlan::QuickMove;
    const CoercionPlan& plan = *entry->plan;
    if (plan.keepsObject)
        return keepQuickly(vm, asObject(cell), plan);
    uint8_t* bytes = std::bit_cast<uint8_t*>(cell);
    std::span<const Move> moves = plan.quickMoves.span();
    uint64_t staged[CoercionPlan::maxNumberOfQuickMoves];
    bool hasCheckedChildren = false;
    for (size_t i = 0; i < moves.size(); ++i) {
        const Move& move = moves[i];
        JSValue held = JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(bytes + move.from));
        switch (move.kind) {
        case Move::Kind::StringAsValue:
            if (!held.isString())
                return false;
            staged[i] = JSValue::encode(held);
            break;
        case Move::Kind::StringAsCell32:
            if (!held.isString())
                return false;
            staged[i] = MarkedBlockCage::encode(held.asCell());
            break;
        case Move::Kind::InlineInt32:
            if (!held.isInt32() || held.asInt32() < minInlineInt32)
                return false;
            staged[i] = static_cast<uint32_t>(held.asInt32());
            break;
        case Move::Kind::NumberAsDouble:
            if (!held.isNumber())
                return false;
            staged[i] = std::bit_cast<uint64_t>(held.asNumber());
            break;
        case Move::Kind::BooleanAsByte:
            if (!held.isBoolean())
                return false;
            staged[i] = held.asBoolean();
            break;
        case Move::Kind::Any:
            if (!(move.acceptedKinds & CoercionPlan::kindOf(held)) && !canHoldWithoutConversion(*plan.fields[i].field, move.acceptedKinds, held)) {
                bool isOutermost = !m_isConvertingChildren;
                if (isOutermost && !hasCheckedChildren) {
                    ChildrenOfQuickConversion seen;
                    if (!canConvertQuickly(value, type, seen, 0))
                        return false;
                    hasCheckedChildren = true;
                }
                m_isConvertingChildren = true;
                bool isConverted = tryQuickConversion(vm, held, ownTypeOf(*plan.fields[i].field));
                m_isConvertingChildren = !isOutermost;
                if (!isConverted || !canHoldWithoutConversion(*plan.fields[i].field, move.acceptedKinds, held))
                    return false;
            }
            staged[i] = 0;
            if (move.store != Store::Int32OrNumberBox) {
                if (!storeAs(*this, move.store, &staged[i], held))
                    return false;
            } else if (held.isInt32() && held.asInt32() >= minInlineInt32)
                staged[i] = static_cast<uint32_t>(held.asInt32());
            else if (held.isUndefinedOrNull())
                staged[i] = static_cast<uint32_t>(held.isUndefined() ? undefinedInt32 : nullInt32);
            else
                return false;
            break;
        }
    }
    const uint8_t* body = plan.quickBody.span().data();
    uint8_t* newBody = bytes + sizeof(JSCell);
    size_t sizeOfCell = cell->cellSize();
    RELEASE_ASSERT(sizeOfCell >= entry->sizeOfCell);
    if (sizeOfCell != entry->sizeOfCell) [[unlikely]]
        zeroSpan(std::span { bytes + entry->sizeOfCell, sizeOfCell - entry->sizeOfCell });
    switch (entry->sizeOfCell) {
    case 32:
        memcpy(newBody, body, 24);
        break;
    case 48:
        memcpy(newBody, body, 40);
        break;
    case 64:
        memcpy(newBody, body, 56);
        break;
    case 80:
        memcpy(newBody, body, 72);
        break;
    case 96:
        memcpy(newBody, body, 88);
        break;
    case 112:
        memcpy(newBody, body, 104);
        break;
    case 128:
        memcpy(newBody, body, 120);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    for (size_t i = 0; i < moves.size(); ++i) {
        const Move& move = moves[i];
        uint8_t* slot = bytes + move.to;
        switch (move.width) {
        case 1:
            *slot = static_cast<uint8_t>(staged[i]);
            break;
        case 2:
            *reinterpret_cast<uint16_t*>(slot) = static_cast<uint16_t>(staged[i]);
            break;
        case 4:
            *reinterpret_cast<uint32_t*>(slot) = static_cast<uint32_t>(staged[i]);
            break;
        default:
            *reinterpret_cast<uint64_t*>(slot) = staged[i];
            break;
        }
    }
    static_assert(!JSCell::structureIDOffset() && JSCell::indexingTypeAndMiscOffset() == 4 && JSCell::typeInfoFlagsOffset() == 6 && JSCell::cellStateOffset() == 7);
    constexpr uint64_t preservedHeaderBits = 0xffull << 56 | static_cast<uint64_t>(TypeInfoPerCellBit) << 48 | static_cast<uint64_t>(static_cast<uint8_t>(~AllArrayTypesAndHistory)) << 32;
    uint64_t* header = std::bit_cast<uint64_t*>(cell);
    *header = (*header & preservedHeaderBits) | entry->header;
    return true;
}

void State::finishCoercionPlan(CoercionPlan& plan, const LayoutData& layout, uint32_t orderAndCount)
{
    plan.orderAndCount = orderAndCount;
    plan.layout = &layout;
    if (sizeWithSpecializationCapacity(layout.shape) > CoercionPlan::maxInPlaceConversionSize || plan.fields.containsIf([](auto& planned) { return !!(planned.field->flags & fieldIsOutOfLine); }))
        return;
    plan.initialBody.fill(0, sizeWithSpecializationCapacity(layout.shape) - sizeof(JSCell));
    uint8_t* cell = plan.initialBody.mutableSpan().data() - sizeof(JSCell);
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        if (const FieldData& field = layout.fields()[i]; !(field.flags & fieldIsOutOfLine))
            clearSlot(cell + field.offset, field.store());
    }
    *reinterpret_cast<uint32_t*>(cell + offsetOfOrderWord) = (orderWordOf(keysAreInLayoutOrder, 0) & ~orderAndCountMask) | orderAndCount;
    *reinterpret_cast<uint32_t*>(cell + offsetOfShapeWord) = shapeWordOf(layout.shape);
}

const State::CoercionPlan* State::firstCoercionPlanForShape(VM& vm, JSObject* object, const RuntimeType& type)
{
    if (data.isCompiledForAudit || type.allowsAnyObject() || type.acceptsUncheckedObjects() || !canAddCoercionPlans() || !isPlainData(object))
        return nullptr;
    if (type.numberOfSupertypes() != 1 || !type.view(0).supertypeShape) {
        Coercion planner(instance.globalObject, *this, Coercion::Mode::Cast, Coercion::Completeness::HasAddedFields, Coercion::Explains::No);
        return planner.planForNearestLayout(object, type);
    }
    const LayoutData& layout = layoutForShape(type.view(0).supertypeShape);
    if (layout.kind != LayoutKind::Record || layout.literalsWithoutFieldOffset() || layout.numberOfFields > 64)
        return nullptr;
    reifyLazyClosures(vm, object);
    Structure* structure = object->structure();
    if (structure->isDictionary() || structure->hasPolyProto() || hasIndexedProperties(structure->indexingType()))
        return nullptr;
    auto plan = makeUnique<CoercionPlan>();
    Vector<uint16_t, 16> order;
    uint64_t isPresent = 0;
    bool hasOnlyFields = true;
    structure->forEachProperty(vm, [&](const auto& entry) {
        const FieldData* field = entry.attributes() ? nullptr : this->field(vm, layout, entry.key());
        if (!field || (field->numberOfLiterals() && field->flags & fieldIsPinned)) {
            hasOnlyFields = false;
            return false;
        }
        uint8_t kinds = kindsAcceptedBy(ownTypeOf(*field));
        if (field->numberOfLiterals())
            kinds &= ~(CoercionPlan::acceptsString | CoercionPlan::acceptsInt32 | CoercionPlan::acceptsDouble);
        plan->fields.append({ entry.offset(), field, entry.key(), kinds });
        isPresent |= 1ull << (field - layout.fields());
        order.append(field - layout.fields());
        return true;
    });
    if (!hasOnlyFields || order.size() > maxFieldsInOrder)
        return nullptr;
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        if (!(layout.fields()[i].flags & fieldIsOptional) && !(isPresent >> i & 1))
            return nullptr;
    }
    plan->chosen = &layout;
    finishCoercionPlan(*plan, layout, orderStartingWith(layout, order.span()));
    const CoercionPlan* made = plan.get();
    addCoercionPlan(object, structure, type, WTF::move(plan), { });
    return coercionPlan(structure, type) == made ? made : nullptr;
}

bool State::canHoldWithoutConversion(const FieldData& field, uint8_t acceptedKinds, JSValue value) const
{
    const RuntimeType& type = ownTypeOf(field);
    if (!value.isObject())
        return fits(type, value) && isAllowedLiteral(field, value);
    if (Object* compiled = compiledObject(value)) {
        if (type.allowsAnyObject())
            return true;
        return acceptsShape(type, compiled->shape()) && !layoutForShape(compiled->shape()).numberOfSpecializations();
    }
    if (asObject(value)->classInfo() == ClassPrototype::info())
        return false;
    return acceptedKinds & CoercionPlan::takesUntypedObject || matchesTypedSubtype(type, asObject(value));
}

const TypedClonePlan& State::typedClonePlan(VM& vm, Structure* structure, uint64_t target)
{
    if (auto found = m_typedClonePlans.find({ structure, target }); found != m_typedClonePlans.end())
        return *found->value;
    if (m_typedClonePlans.size() >= maxTypedClonePlans)
        return m_noTypedClonePlan;
    auto plan = makeUnique<TypedClonePlan>();
    uint32_t shape = TypedCloneTarget::shapeOf(target);
    const LayoutData& layout = layoutForShape(shape);
    size_t size = sizeWithSpecializationCapacity(shape);
    auto storedFieldAt = [&](unsigned i) -> std::optional<uint32_t> {
        uint32_t fieldPlusOne = TypedCloneTarget::storedFieldPlusOne(target, i);
        return fieldPlusOne ? std::optional(fieldPlusOne - 1) : std::nullopt;
    };
    auto isStoredByCaller = [&](uint32_t index) {
        for (unsigned i = 0; i < TypedCloneTarget::maxStoredFields; ++i) {
            if (storedFieldAt(i) == index)
                return true;
        }
        return false;
    };
    auto kindOfMove = [&](const FieldData& field, uint8_t kinds) {
        using Kind = TypedCloneMove::Kind;
        constexpr uint8_t nullish = CoercionPlan::acceptsUndefined | CoercionPlan::acceptsNull;
        const RuntimeType& type = ownTypeOf(field);
        if (field.store() == Store::JSValue && type.allowsAnyObject() && type.allowsAnyFunction() && (type.primitives & Primitive::All) == Primitive::All && kinds & CoercionPlan::acceptsInt32 && kinds & CoercionPlan::acceptsDouble)
            return Kind::Value;
        if (field.store() == Store::Double && kinds == (CoercionPlan::acceptsInt32 | CoercionPlan::acceptsDouble))
            return Kind::NumberAsDouble;
        if (field.store() == Store::Int32OrNumberBox && kinds == (CoercionPlan::acceptsInt32 | CoercionPlan::acceptsDouble))
            return Kind::NumberAsInlineInt32;
        if (field.store() == Store::Cell32 && kinds == CoercionPlan::acceptsString)
            return Kind::StringAsCell32;
        if (field.store() == Store::OptionalCell32 && kinds == (CoercionPlan::acceptsString | nullish))
            return Kind::NullableStringAsCell32;
        if (field.store() == Store::Boolean && kinds == (CoercionPlan::acceptsTrue | CoercionPlan::acceptsFalse))
            return Kind::BooleanAsByte;
        return Kind::General;
    };
    bool isEligible = !isCounting() && !data.isCompiledForAudit && layout.kind == LayoutKind::Record && !layout.hasExtension && !layout.literalsWithoutFieldOffset() && layout.numberOfFields <= 64
        && size <= CoercionPlan::maxInPlaceConversionSize && !(size % sizeof(uint64_t)) && structure->isStructureExtensible() && !structure->isDictionary() && !structure->hasPolyProto()
        && !hasIndexedProperties(structure->indexingType()) && !structure->outOfLineCapacity() && structure->realm() == instance.globalObject;
    Vector<uint16_t, 16> order;
    uint64_t isPresent = 0;
    bool hasGeneralMove = false;
    if (isEligible) {
        structure->forEachProperty(vm, [&](const auto& entry) {
            const FieldData* field = entry.attributes() ? nullptr : this->field(vm, layout, entry.key());
            if (!field || field->numberOfLiterals() || field->flags & (fieldIsOutOfLine | fieldIsPinned | fieldIsImmutable) || !isInlineOffset(entry.offset())) {
                isEligible = false;
                return false;
            }
            uint32_t index = field - layout.fields();
            isPresent |= 1ull << index;
            order.append(index);
            if (isStoredByCaller(index))
                return true;
            uint8_t kinds = kindsAcceptedBy(ownTypeOf(*field));
            auto kind = kindOfMove(*field, kinds);
            hasGeneralMove |= kind == TypedCloneMove::Kind::General;
            plan->moves.append({ static_cast<uint16_t>(JSObject::offsetOfInlineStorage() + offsetInInlineStorage(entry.offset()) * sizeof(EncodedJSValue)), static_cast<uint16_t>(field->offset), kind, kinds, field->store(), 0 });
            plan->fields.append(field);
            return true;
        });
    }
    for (unsigned i = 0; i < TypedCloneTarget::maxStoredFields && isEligible; ++i) {
        auto index = storedFieldAt(i);
        if (!index || isPresent >> *index & 1)
            continue;
        isPresent |= 1ull << *index;
        order.append(*index);
    }
    for (uint32_t i = 0; i < layout.numberOfFields && isEligible; ++i)
        isEligible = layout.fields()[i].flags & fieldIsOptional || isPresent >> i & 1;
    uint32_t orderAndCount = isEligible && order.size() <= maxFieldsInOrder ? orderStartingWith(layout, order.span()) : orderWordOf(keysAreInLayoutOrder, 0);
    if (isEligible && orderOf(orderAndCount) != keysAreInLayoutOrder) {
        plan->body.fill(0, (size - sizeof(JSCell)) / sizeof(uint64_t));
        uint8_t* cell = reinterpret_cast<uint8_t*>(plan->body.mutableSpan().data()) - sizeof(JSCell);
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            if (const FieldData& field = layout.fields()[i]; !(field.flags & fieldIsOutOfLine))
                clearSlot(cell + field.offset, field.store());
        }
        *reinterpret_cast<uint32_t*>(cell + offsetOfOrderWord) = (orderWordOf(keysAreInLayoutOrder, 0) & ~orderAndCountMask) | orderAndCount;
        *reinterpret_cast<uint32_t*>(cell + offsetOfShapeWord) = shapeWordOf(shape);
        plan->structure = structureFor(vm, shape, instance.globalObject->objectPrototype());
        plan->header = static_cast<uint64_t>(plan->structure->typeInfoBlob()) << 32 | plan->structure->id().bits();
        plan->allocator = vm.cellSpace().allocatorFor(size, AllocatorForMode::EnsureAllocator).localAllocator();
        plan->bodyWords = plan->body.span().data();
        plan->bodyWordCount = plan->body.size();
        plan->firstMove = plan->moves.span().data();
        plan->moveCount = plan->moves.size();
        plan->path = hasGeneralMove || !plan->allocator ? TypedClonePlan::Path::Runtime : TypedClonePlan::Path::Stub;
    }
    m_hasNewTypedClonePlans = true;
    return *m_typedClonePlans.add({ structure, target }, WTF::move(plan)).iterator->value;
}

Object* State::tryCloneAs(VM& vm, JSObject* source, uint64_t target)
{
    Structure* structure = source->structure();
    uint32_t structureID = structure->id().bits();
    TypedCloneCacheEntry& cached = static_cast<TypedCloneCacheEntry*>(instance.typedCloneCache)[TypedCloneCacheEntry::placeOf(structureID, target)];
    const TypedClonePlan* found = cached.plan;
    if (cached.structureID != structureID || cached.target != target) [[unlikely]] {
        DeferGC deferGC(vm);
        found = &typedClonePlan(vm, structure, target);
        cached = { target, found, structureID };
        m_hasCachedTypedClones = true;
    }
    const TypedClonePlan& plan = *found;
    if (plan.path == TypedClonePlan::Path::PlainClone)
        return nullptr;
    Object* copy = Object::createWithBody(vm, plan.structure, asBytes(plan.body.span()));
    const uint8_t* from = std::bit_cast<const uint8_t*>(source);
    uint8_t* to = std::bit_cast<uint8_t*>(copy);
    for (unsigned i = 0; i < plan.moves.size(); ++i) {
        auto& move = plan.moves[i];
        JSValue value = JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(from + move.from));
        if (!(move.acceptedKinds & CoercionPlan::kindOf(value)) && !canHoldWithoutConversion(*plan.fields[i], move.acceptedKinds, value))
            return nullptr;
        if (move.store != Store::Int32OrNumberBox) {
            if (!storeAs(*this, move.store, to + move.to, value))
                return nullptr;
            continue;
        }
        if (value.isInt32() && value.asInt32() >= minInlineInt32)
            *reinterpret_cast<int32_t*>(to + move.to) = value.asInt32();
        else if (value.isUndefinedOrNull())
            *reinterpret_cast<int32_t*>(to + move.to) = value.isUndefined() ? undefinedInt32 : nullInt32;
        else
            return nullptr;
    }
    vm.mutatorFence();
    ensureStillAliveHere(source);
    return copy;
}

bool State::tryConvertPlainObjectInPlace(VM& vm, JSValue value, const RuntimeType& type, uint32_t typeAndReason)
{
    if (tryQuickConversion(vm, value, type))
        return true;
    if (data.isCompiledForAudit || !value.isObject() || asObject(value)->type() != FinalObjectType || type.acceptsUntypedObjectsByNature() || compiledObject(value))
        return false;
    if (Object* made = copyUnchangedByPlan(vm, asObject(value), type, true, typeAndReason))
        return JSValue(made) == value;
    return tryConvertInPlaceByPlans(vm, asObject(value), type);
}

Object* State::copyUnchangedByPlan(VM& vm, JSObject* object, const RuntimeType& type, bool mayConvertInPlace, uint32_t typeAndReason)
{
    const CoercionPlan* plan = coercionPlan(object, type);
    if (!plan)
        plan = firstCoercionPlanForShape(vm, object, type);
    if (!plan || !plan->layout)
        return nullptr;
    Structure* structureBefore = object->structure();
    const CoercionPlan* root = plan;
    uint64_t discriminant = 0;
    bool mayBeQuick = mayConvertInPlace && plan->quickConversionEpoch != m_quickConversionEpoch;
    if (mayBeQuick) {
        plan->quickConversionEpoch = m_quickConversionEpoch;
        root = coercionPlan(structureBefore, type);
        if (root != plan) {
            JSValue held = object->getDirect(root->discriminant);
            discriminant = discriminantWord(held);
            auto child = discriminant ? root->byDiscriminant.find(asString(held)->tryGetValue()) : root->byDiscriminant.end();
            mayBeQuick = child != root->byDiscriminant.end() && child->value.get() == plan;
        }
    }
    for (auto& planned : plan->fields) {
        JSValue value = object->getDirect(planned.offset);
        uint8_t kind = CoercionPlan::kindOf(value);
        if (planned.acceptedKinds & kind) [[likely]]
            continue;
        const RuntimeType& ofField = ownTypeOf(*planned.field);
        if (!value.isObject()) {
            if (!fits(ofField, value) || !isAllowedLiteral(*planned.field, value))
                return nullptr;
            continue;
        }
        if (Object* compiled = compiledObject(value)) {
            bool isOfType = ofField.allowsAnyObject() || acceptsShape(ofField, compiled->shape());
            if (!isOfType || (layoutForShape(compiled->shape()).numberOfSpecializations() && !ofField.allowsAnyObject()))
                return nullptr;
            continue;
        }
        if (asObject(value)->classInfo() == ClassPrototype::info() || (!(planned.acceptedKinds & CoercionPlan::takesUntypedObject) && !matchesTypedSubtype(ofField, asObject(value))))
            return nullptr;
    }
    if (Object* made = tryConvertOrCopyByPlan(vm, object, *plan, mayConvertInPlace)) {
        if (mayBeQuick && JSValue(made) == JSValue(object))
            cacheQuickConversion(structureBefore, type, *root, *plan, discriminant, made, WTF::roundUpToMultipleOf<MarkedBlock::atomSize>(JSFinalObject::allocationSize(structureBefore->inlineCapacity())), typeAndReason);
        else if (typeAndReason && !mayConvertInPlace)
            cacheCopyConversion(vm, structureBefore, type, *plan, made->structure(), typeAndReason);
        return made;
    }
    if (!plan->extraProperties.isEmpty())
        return nullptr;
    const LayoutData& layout = *plan->layout;
    Object* result = Object::create(vm, structureFor(vm, layout.shape, object->getPrototypeDirect()), layout, sizeWithSpecializationCapacity(layout.shape), orderWordOf(keysAreInLayoutOrder, 0));
    for (auto& planned : plan->fields) {
        if (!result->store(vm, *planned.field, object->getDirect(planned.offset)))
            return nullptr;
    }
    result->setOrderAndCount(plan->orderAndCount);
    ensureStillAliveHere(object);
    countObject(result);
    return result;
}

static unsigned nestedPlanCount(const State::CoercionPlan& plan)
{
    unsigned number = plan.byDiscriminant.size();
    for (auto& below : plan.byDiscriminant.values())
        number += nestedPlanCount(*below);
    return number;
}

JSString* State::cachedKeyFor(VM& vm, std::span<const Latin1Character> characters)
{
    if (characters.size() < 2)
        return characters.empty() ? jsEmptyString(vm) : jsSingleCharacterString(vm, characters[0]);
    unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    if (JSString* key = tryFindCachedKey(characters, hash))
        return key;
    return makeKey(vm, characters, hash);
}

JSString* State::caseConvertedString(VM& vm, std::span<const Latin1Character> characters)
{
    ASSERT(characters.size() >= 2);
    unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    uint64_t& entry = m_caseConvertedStrings[hash % m_caseConvertedStrings.size()];
    if (entry && entry >> 32 == hash) {
        auto* candidate = uncheckedDowncast<JSString>(MarkedBlockCage::decode(static_cast<uint32_t>(entry)));
        if (auto* impl = candidate->tryGetValueImpl(); impl->is8Bit() && CompactTable::hasCharacters<Latin1Character>(*impl, characters))
            return candidate;
    }
    JSString* made = jsNontrivialString(vm, String(characters));
    if (MarkedBlockCage::contains(made))
        entry = static_cast<uint64_t>(hash) << 32 | MarkedBlockCage::encode(made);
    return made;
}

JSString* State::makeKey(VM& vm, std::span<const Latin1Character> characters, unsigned hash)
{
    static constexpr size_t minTableSize = 1 << 8;
    static constexpr size_t maxTableSize = 1 << 16;
    if (m_cachedKeys.isEmpty()) [[unlikely]]
        m_cachedKeys.fill(0, minTableSize);
    auto firstIndexFor = [&](unsigned hash) {
        return (hash & (m_cachedKeys.size() - 1)) & ~static_cast<size_t>(1);
    };
    JSString* made = jsNontrivialString(vm, String(characters));
    ASSERT(!made->tryGetValueImpl()->hasHash());
    *std::bit_cast<unsigned*>(std::bit_cast<uintptr_t>(made->tryGetValueImpl()) + StringImpl::flagsOffset()) |= hash << StringImpl::s_flagCount;
    ASSERT(made->tryGetValueImpl()->existingHash() == StringHasher::computeHashAndMaskTop8Bits(characters));
    if (!MarkedBlockCage::contains(made))
        return made;
    auto put = [&](uint64_t entry) {
        unsigned hash = entry >> 32;
        size_t place = firstIndexFor(hash);
        if (m_cachedKeys[place] && !m_cachedKeys[place + 1])
            ++place;
        else if (m_cachedKeys[place]) {
            place += hash >> 23 & 1;
            ++m_cachedKeyEvictionCount;
        }
        m_cachedKeys[place] = entry;
    };
    put(static_cast<uint64_t>(hash) << 32 | MarkedBlockCage::encode(made));
    if (m_cachedKeyEvictionCount > m_cachedKeys.size() / 4 && m_cachedKeys.size() < maxTableSize) {
        Vector<uint64_t> former = std::exchange(m_cachedKeys, { });
        m_cachedKeys.fill(0, former.size() * 2);
        for (uint64_t entry : former) {
            if (entry)
                put(entry);
        }
        m_cachedKeyEvictionCount = 0;
    }
    return made;
}

void State::cacheFirstKeyByParts(const KeyByParts& taken, JSString* key)
{
    static constexpr size_t minTableSize = 1 << 8;
    if (data.usesWideReferences || !MarkedBlockCage::contains(key))
        return;
    m_keysByParts.fill({ }, minTableSize);
    cacheKeyByParts(taken, key);
}

void State::growKeysByParts()
{
    static constexpr size_t maxTableSize = 1 << 14;
    auto put = [&](KeyByParts entry, uint32_t hash) {
        size_t place = hash & (m_keysByParts.size() - 1) & ~static_cast<size_t>(1);
        if (m_keysByParts[place].countAndKinds && !m_keysByParts[place + 1].countAndKinds)
            ++place;
        else if (m_keysByParts[place].countAndKinds) {
            place += hash >> 31;
            ++m_keysByPartsEvictionCount;
        }
        m_keysByParts[place] = entry;
    };
    auto hashOf = [](const KeyByParts& entry) {
        uint32_t count = entry.countAndKinds & 0xff;
        uint64_t mixed = count;
        for (uint32_t i = 0; i < count; ++i)
            mixed = (mixed ^ entry.parts[i]) * 0x9e3779b97f4a7c15ull;
        return static_cast<uint32_t>((mixed ^ entry.countAndKinds >> 8) >> 32);
    };
    if (m_keysByParts.size() < maxTableSize) {
        Vector<KeyByParts> former = std::exchange(m_keysByParts, { });
        m_keysByParts.fill({ }, former.size() * 2);
        for (auto& entry : former) {
            if (entry.countAndKinds)
                put(entry, hashOf(entry));
        }
    }
    m_keysByPartsEvictionCount = 0;
}

void State::cacheExtraPropertyRead(Object* object, JSObject* extra, uint32_t name, PropertyOffset offset, unsigned attributes)
{
    Structure* structure = extra->structure();
    if (isCounting() || structure->isDictionary() || (isValidOffset(offset) && attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
        return;
    int32_t displacement = 0;
    if (isValidOffset(offset))
        displacement = isInlineOffset(offset) ? static_cast<int32_t>(JSObject::offsetOfInlineStorage() + offsetInInlineStorage(offset) * sizeof(EncodedJSValue)) : offsetInButterfly(offset) * static_cast<int32_t>(sizeof(EncodedJSValue));
    uint32_t structureID = structure->id().bits();
    uint32_t shapeWord = shapeWordOf(object->shape());
    m_hasCachedExtraPropertyReads = true;
    static_cast<ExtraPropertyLocation*>(instance.typedExtraPropertyLocations)[ExtraPropertyLocation::placeOf(structureID, name, shapeWord)] = { structureID, name, shapeWord, displacement };
}

void State::removeExtraPropertiesOf(Object* object)
{
    if (isCounting()) [[unlikely]]
        extraProperties.remove(object);
}

void State::clearExtraPropertyReads()
{
    if (!std::exchange(m_hasCachedExtraPropertyReads, false))
        return;
    for (auto& entry : std::span { static_cast<ExtraPropertyLocation*>(instance.typedExtraPropertyLocations), static_cast<size_t>(1) << ExtraPropertyLocation::indexBits }) {
        if (entry.structureID)
            entry = { };
    }
}

std::optional<uint32_t> State::orderAfterLateFieldCreation(const Object& object, JSObject* extraProperties, uint64_t slotKeys) const
{
    uint32_t structureID = extraProperties->structureID().bits();
    uint32_t shapeWord = shapeWordOf(object.shape());
    uint32_t before = object.orderWord() & orderAndCountMask;
    auto& known = static_cast<const LateFieldCreation*>(instance.typedLateFieldCreations)[LateFieldCreation::placeOf(structureID, shapeWord, before, slotKeys)];
    if (known.extraPropertiesStructureID != structureID || known.shapeWord != shapeWord || known.orderAndCountBefore != before || known.slotKeys != slotKeys)
        return std::nullopt;
    return known.orderAndCountAfter;
}

void State::cacheLateFieldCreation(const Object& object, JSObject* extraProperties, uint32_t orderWordBefore, uint64_t slotKeys)
{
    uint32_t after = object.orderWord() & orderAndCountMask;
    if (isCounting() || !slotKeys || extraProperties->structure()->isDictionary() || orderOf(orderWordBefore) == keysAreInLayoutOrder || orderOf(after) == keysAreInLayoutOrder)
        return;
    uint32_t structureID = extraProperties->structureID().bits();
    uint32_t shapeWord = shapeWordOf(object.shape());
    uint32_t before = orderWordBefore & orderAndCountMask;
    m_hasCachedLateFieldCreations = true;
    static_cast<LateFieldCreation*>(instance.typedLateFieldCreations)[LateFieldCreation::placeOf(structureID, shapeWord, before, slotKeys)] = { slotKeys, structureID, shapeWord, before, after };
}

CachedPropertyList& State::propertyListCacheEntry(StructureID structureID, uint32_t shapeWord, uint32_t orderAndCount)
{
    if (!m_propertyListCache)
        m_propertyListCache = makeUniqueWithoutFastMallocCheck<std::array<CachedPropertyList, propertyListCacheSize>>();
    m_propertyListCacheIsDirty = true;
    return (*m_propertyListCache)[((structureID.bits() >> 4) + shapeWord + orderAndCount * 31) % propertyListCacheSize];
}

Structure* State::plainCopyStructure(VM& vm, CachedPropertyList& listing)
{
    if (std::exchange(listing.didLookUpPlainCopyStructure, true))
        return listing.plainCopyStructure;
    JSGlobalObject* globalObject = instance.globalObject;
    size_t size = listing.properties.size();
    Structure* structure = size <= JSFinalObject::defaultInlineCapacity ? globalObject->objectStructureForObjectConstructor()
        : size <= JSFinalObject::maxInlineCapacity ? globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, globalObject->objectPrototype(), size) : nullptr;
    PropertyOffset expected = 0;
    for (auto& property : listing.properties) {
        if (!structure)
            break;
        PropertyOffset offset = invalidOffset;
        structure = Structure::addPropertyTransition(vm, structure, PropertyName(property.name), 0, offset);
        if (offset != expected++ || structure->isDictionary())
            structure = nullptr;
    }
    listing.plainCopyStructure = structure;
    return structure;
}

void State::clearPropertyListCache()
{
    if (!std::exchange(m_propertyListCacheIsDirty, false))
        return;
    for (auto& listing : *m_propertyListCache)
        listing = { };
}

void State::clearLateFieldCreations()
{
    if (!std::exchange(m_hasCachedLateFieldCreations, false))
        return;
    zeroSpan(std::span { static_cast<LateFieldCreation*>(instance.typedLateFieldCreations), static_cast<size_t>(1) << LateFieldCreation::indexBits });
}

void State::removeDeadCachedKeys(VM& vm)
{
    for (uint64_t& entry : m_cachedKeys) {
        if (entry && !vm.heap.isMarked(MarkedBlockCage::decode(static_cast<uint32_t>(entry))))
            entry = 0;
    }
    for (uint64_t& entry : m_caseConvertedStrings) {
        if (entry && !vm.heap.isMarked(MarkedBlockCage::decode(static_cast<uint32_t>(entry))))
            entry = 0;
    }
    for (auto& entry : m_keysByParts) {
        if (!entry.countAndKinds)
            continue;
        bool isAlive = vm.heap.isMarked(MarkedBlockCage::decode(entry.key));
        for (uint32_t areCells = entry.countAndKinds >> 8, i = 0; areCells && isAlive; areCells >>= 1, ++i) {
            if (areCells & 1)
                isAlive = vm.heap.isMarked(MarkedBlockCage::decode(entry.parts[i]));
        }
        if (!isAlive)
            entry = { };
    }
}

void State::removeDeadCoercionPlans(VM& vm, bool newOnly)
{
    m_lastCoercionPlanLookup = { };
    if (std::exchange(m_hasCachedTypedClones, false))
        zeroSpan(std::span { static_cast<TypedCloneCacheEntry*>(instance.typedCloneCache), static_cast<size_t>(1) << TypedCloneCacheEntry::indexBits });
    if (std::exchange(m_hasNewTypedClonePlans, false) || !newOnly) {
        m_typedClonePlans.removeIf([&](auto& entry) {
            return !vm.heap.isMarked(entry.key.first);
        });
    }
    if (std::exchange(m_hasCachedConversions, false))
        zeroSpan(std::span { static_cast<ConversionCacheEntry*>(instance.typedConversionCache), static_cast<size_t>(1) << ConversionCacheEntry::indexBits });
    if (m_quickConversions)
        m_quickConversions->fill({ });
    ++m_quickConversionEpoch;
    if (!std::exchange(m_hasNewCoercionPlans, false) && newOnly)
        return;
    m_coercionPlans.removeIf([&](auto& entry) {
        if (vm.heap.isMarked(entry.key.first))
            return false;
        m_coercionPlansByDiscriminantCount -= nestedPlanCount(*entry.value);
        return true;
    });
}

const State::CopyPlan* State::copyPlan(uint32_t shape, uint32_t orderWord, uint32_t copyShape, std::span<UniquedStringImpl* const> excluded) const
{
    auto found = m_copyPlans.find({ shape, orderWord, copyShape });
    if (found == m_copyPlans.end())
        return nullptr;
    for (auto& plan : found->value) {
        if (std::ranges::equal(plan->excluded, excluded, [](const RefPtr<UniquedStringImpl>& a, UniquedStringImpl* b) { return a.get() == b; }))
            return plan.get();
    }
    return nullptr;
}

void State::addCopyPlan(uint32_t shape, uint32_t orderWord, uint32_t copyShape, std::unique_ptr<CopyPlan>&& plan)
{
    m_copyPlans.add({ shape, orderWord, copyShape }, Vector<std::unique_ptr<CopyPlan>, 1> { }).iterator->value.append(WTF::move(plan));
}

TransitionPlan& State::transitionPlanFor(VM& vm, Object* object, uint32_t shape)
{
    Structure* structure = object->structure();
    return *m_transitionPlans.ensure({ structure, shape }, [&] {
        const LayoutData& from = *structure->compiledLayout();
        const LayoutData& to = layoutForShape(shape);
        RELEASE_ASSERT(from.hasExtension == to.hasExtension && from.extraPropertiesSlotIndex == to.extraPropertiesSlotIndex);
        auto plan = makeUnique<TransitionPlan>();
        plan->structure = structureWithExtraPropertiesMatching(vm, structure, shape, object->getPrototypeDirect());
        plan->shapeWord = shapeWordOf(shape);
        plan->size = to.size;
        plan->from = &from;
        plan->to = &to;
        for (uint32_t i = 0; i < from.numberOfFields; ++i) {
            const FieldData& old = from.fields()[i];
            if (old.flags & fieldIsPlaceholder)
                continue;
            const FieldData* kept = field(vm, to, name(vm, old.name).impl());
            RELEASE_ASSERT(kept && kept->offset == old.offset && kept->store() == old.store() && kept->type() == old.type() && (kept->flags & fieldIsOutOfLine) == (old.flags & fieldIsOutOfLine));
        }
        for (uint32_t i = 0; i < to.numberOfFields; ++i) {
            const FieldData& added = to.fields()[i];
            if (field(vm, from, name(vm, added.name).impl()))
                continue;
            RELEASE_ASSERT(mayBeAbsent(added));
            if (!(added.flags & fieldIsOutOfLine))
                plan->addedSlots.append({ added.offset, added.store() });
        }
        for (Structure* kept : { structure, plan->structure }) {
            if (m_planStructures.add(kept).isNewEntry)
                didCreate(kept);
        }
        return plan;
    }).iterator->value;
}

class ViolationLog {
    WTF_MAKE_NONCOPYABLE(ViolationLog);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ViolationLog);
public:
    struct Site {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Site);
        uint64_t id { 0 };
        ASCIILiteral kind;
        ASCIILiteral what;
        String file;
        LineColumn position;
        String function;
        String on;
        String expected;
        State::ActionOnViolation action { State::ActionOnViolation::Threw };
        Vector<std::pair<String, uint64_t>, 2> counts;
    };
    using Address = std::pair<const void*, const char*>;
    static constexpr unsigned maxNumberOfSites = 200;
    static constexpr unsigned maxNumberOfAddresses = 8 * maxNumberOfSites;
    static constexpr unsigned maxNumberOfKindsAtSite = 8;
    static constexpr unsigned maxLengthOfType = 80;
    static constexpr unsigned maxNumberOfCallers = 3;
    static constexpr unsigned maxLengthOfReason = 200;
    static inline bool s_isRetainedForHost;

    static bool isEnabled(bool isRequestedByImage)
    {
        ViolationLog* log = singletonIfEnabled(isRequestedByImage);
        return log && log->m_file;
    }

    String throwLocations()
    {
        Locker locker { lock };
        StringBuilder lines;
        for (Site* site : m_sitesInOrder) {
            if (m_file || site->action != State::ActionOnViolation::Threw)
                continue;
            for (auto& [got, count] : site->counts) {
                lines.append(hex(site->id, 12, Lowercase), ' ', site->file, ':', site->position.line, ':', site->position.column, " ("_s, site->function, "): "_s);
                if (!site->on.isNull())
                    lines.append(site->kind, " on "_s, site->on, "; "_s);
                if (!site->expected.isNull())
                    lines.append("expected "_s, site->expected, got.isEmpty() ? "; "_s : ", "_s);
                if (!got.isEmpty())
                    lines.append("got "_s, got, "; "_s);
                lines.append(count, count == 1 ? " time\n"_s : " times\n"_s);
            }
        }
        if (m_hasOverflowed)
            lines.append("... and more: the first "_s, maxNumberOfSites, " places are kept\n"_s);
        return lines.toString();
    }

    static ViolationLog* singletonIfEnabled(bool isRequestedByImage)
    {
        static ViolationLog* log;
        static std::once_flag once;
        std::call_once(once, [&] {
            const char* where = getenv("BUN_AOT_TYPE_ERROR_LOG");
            if ((!where || !*where) && !isRequestedByImage && s_isRetainedForHost) {
                log = new ViolationLog(nullptr, false);
                return;
            }
            if (!where || !*where)
                where = isRequestedByImage ? "stderr" : "off";
            if (!strcmp(where, "off"))
                return;
#if OS(WINDOWS)
            constexpr const char* mode = "ab";
#else
            constexpr const char* mode = "abe";
#endif
            FILE* file = strcmp(where, "stderr") ? fopen(where, mode) : nullptr;
            if (file)
                setvbuf(file, nullptr, _IONBF, 0);
            const char* format = getenv("BUN_AOT_TYPE_ERROR_LOG_FORMAT");
            log = new ViolationLog(file ? file : stderr, format && *format ? !strcmp(format, "json") : !!file);
            void (*writeTotals)() = [] {
                log->writeTotals();
            };
            atexit(writeTotals);
#if !OS(DARWIN) && !OS(WINDOWS)
            at_quick_exit(writeTotals);
#endif
        });
        return log;
    }

    static uint64_t idOf(const String& file, LineColumn position, ASCIILiteral kind, unsigned ordinal)
    {
        uint64_t hash = 0xcbf29ce484222325ULL;
        auto bytes = makeString(file, ':', position.line, ':', position.column, ':', kind, ':', ordinal).utf8();
        for (char8_t byte : bytes.span())
            hash = (hash ^ byte) * 0x100000001b3ULL;
        return (hash >> 48) ^ (hash & 0xffffffffffffULL);
    }

    std::optional<Site*> siteAt(const Address& address) const
    {
        auto found = m_sitesByAddress.find(address);
        return found == m_sitesByAddress.end() ? std::nullopt : std::optional { found->value };
    }

    Site* addSite(const Address& address, uint64_t id, const Invocable<std::unique_ptr<Site>()> auto& make)
    {
        Site* site = m_sites.get(id);
        if (!site && m_sites.size() < maxNumberOfSites) {
            site = m_sites.add(id, make()).iterator->value.get();
            m_sitesInOrder.append(site);
        }
        if (!site && !std::exchange(m_hasOverflowed, true))
            write(m_isJSON ? makeString("{\"aot\":\"type-error\",\"kind\":\"overflow\",\"sites\":"_s, maxNumberOfSites, "}\n"_s) : makeString("[aot] type-error log is full ("_s, maxNumberOfSites, " places); further places are not recorded\n"_s));
        if (m_sitesByAddress.size() < maxNumberOfAddresses)
            m_sitesByAddress.add(address, site);
        return site;
    }

    static uint64_t count(Site& site, ASCIILiteral word, const String& name)
    {
        for (auto& [kind, count] : site.counts) {
            if (word.isNull() ? kind == name : kind == word)
                return ++count;
        }
        if (site.counts.size() < maxNumberOfKindsAtSite || word == "other"_s) {
            site.counts.append({ word.isNull() ? name.isolatedCopy() : String(word), 1 });
            return 1;
        }
        return 0;
    }

    static bool isPowerOfTen(uint64_t count)
    {
        while (count && !(count % 10))
            count /= 10;
        return count == 1;
    }

    void writeTotals()
    {
        Locker locker { lock };
        for (Site* site : m_sitesInOrder) {
            for (auto& [kind, count] : site->counts) {
                if (!isPowerOfTen(count))
                    write(*site, kind, site->action, count, { }, true);
            }
        }
    }

    void write(const Site& site, const String& got, State::ActionOnViolation action, uint64_t count, const Vector<String, maxNumberOfCallers>& callers, bool isTotal = false, const String& reason = { })
    {
        static constexpr ASCIILiteral actions[] = { "threw"_s, "continued"_s, "dropped"_s, "converted"_s, "copied"_s };
        StringBuilder line;
        if (m_isJSON) {
            auto member = [&](ASCIILiteral name, const String& text) {
                if (text.isNull())
                    return;
                line.append(",\""_s, name, "\":"_s);
                line.appendQuotedJSONString(text);
            };
            line.append("{\"aot\":\"type-error\",\"id\":\""_s, hex(site.id, 12, Lowercase), "\",\"kind\":\""_s, site.kind, '"');
            member("what"_s, site.what);
            member("file"_s, site.file);
            line.append(",\"line\":"_s, site.position.line, ",\"col\":"_s, site.position.column);
            member("fn"_s, site.function);
            member("on"_s, site.on);
            member("expected"_s, site.expected);
            member("got"_s, got);
            line.append(",\"action\":\""_s, actions[static_cast<unsigned>(action)], "\",\"count\":"_s, count);
            member("reason"_s, reason);
            for (auto& caller : callers) {
                line.append(&caller == callers.begin() ? ",\"from\":["_s : ","_s);
                line.appendQuotedJSONString(caller);
            }
            line.append(callers.isEmpty() ? ""_s : "]"_s, isTotal ? ",\"total\":true}\n"_s : "}\n"_s);
        } else {
            line.append("[aot] type-error "_s, hex(site.id, 12, Lowercase), ' ', site.kind, ' ', site.file, ':', site.position.line, ':', site.position.column, " ("_s, site.function, "): "_s);
            if (!site.on.isNull())
                line.append("on "_s, site.on, "; "_s);
            if (!site.expected.isNull())
                line.append("expected "_s, site.expected, got.isNull() ? "; "_s : ", "_s);
            if (!got.isNull())
                line.append("got "_s, got, "; "_s);
            line.append(actions[static_cast<unsigned>(action)], "; count "_s, count);
            if (!reason.isNull())
                line.append("; reason "_s, reason);
            for (auto& caller : callers)
                line.append(&caller == callers.begin() ? "; from "_s : " < "_s, caller);
            line.append(isTotal ? "; total\n"_s : "\n"_s);
        }
        write(line.toString());
    }

    Lock lock;

private:
    ViolationLog(FILE* file, bool isJSON)
        : m_file(file)
        , m_isJSON(isJSON)
    {
    }

    void write(const String& line)
    {
        if (!m_file)
            return;
        auto bytes = line.utf8();
        for (char8_t& byte : bytes.mutableSpan().first(bytes.length() - 1)) {
            if (byte < ' ')
                byte = ' ';
        }
        fwrite(bytes.data(), 1, bytes.length(), m_file);
    }

    FILE* const m_file;
    const bool m_isJSON;
    bool m_hasOverflowed { false };
    UncheckedKeyHashMap<uint64_t, std::unique_ptr<Site>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_sites;
    UncheckedKeyHashMap<Address, Site*> m_sitesByAddress;
    Vector<Site*> m_sitesInOrder;
};

static JSObject* lockedPrototypeConstructor(VM& vm, JSObject* object)
{
    if (!object->mayBePrototype() || object->isCallable())
        return nullptr;
    unsigned attributes;
    JSValue constructor = object->getDirect(vm, vm.propertyNames->constructor, attributes);
    return constructor && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) && constructor.isCallable() ? asObject(constructor) : nullptr;
}

static bool isHostClassPrototype(VM& vm, JSObject* object)
{
    return object->mayBePrototype() && object->getDirect(vm, vm.propertyNames->builtinNames().hostClassPrototypePrivateName());
}

static String nameOfLockedObject(VM& vm, JSObject* object)
{
    if (object->isGlobalObject())
        return "globalThis"_s;
    if (object == object->globalObject()->functionPrototype())
        return "Function.prototype"_s;
    if (object->isCallable())
        return functionName(object);
    if (JSObject* constructor = lockedPrototypeConstructor(vm, object))
        return makeString(functionName(constructor), ".prototype"_s);
    if (isHostClassPrototype(vm, object))
        return makeString(object->classInfo()->className, ".prototype"_s);
    return object->classInfo()->className;
}

static const void* returnAddressIntoTopCompiledFrame(VM& vm, bool forCaller)
{
    CallFrame* frame = vm.topCallFrame;
    if (!frame)
        return nullptr;
    const void* address = returnAddressForFrame(frame, __builtin_frame_address(0));
    while (address && frame && addressKind(address) == ImageAddressInfo::Stub) {
        address = removeCodePtrTag(frame->rawReturnPC());
        frame = frame->callerFrame();
    }
    if (forCaller && address && frame && addressKind(address) == ImageAddressInfo::Function)
        address = removeCodePtrTag(frame->rawReturnPC());
    return address && addressKind(address) == ImageAddressInfo::Function ? address : nullptr;
}

std::optional<State::ListedSite> State::listedSite(const String& file, LineColumn position, const Violation& violation, ASCIILiteral kind) const
{
    std::span words { data.at<uint32_t>(data.sitesOffset), data.numberOfSiteWords };
    size_t at = 0;
    bool isWhole = true;
    auto word = [&] {
        isWhole &= at < words.size();
        return isWhole ? words[at++] : 0;
    };
    auto text = [&] {
        uint32_t length = word();
        size_t start = at;
        at += length / 4 + 1;
        isWhole &= at <= words.size();
        return isWhole ? asBytes(words.subspan(start)).first(length) : std::span<const uint8_t>();
    };
    auto literal = [&] {
        auto bytes = text();
        return isWhole ? ASCIILiteral::fromLiteralUnsafe(byteCast<char>(bytes.data())) : ASCIILiteral();
    };
    Vector<ASCIILiteral, 32> kinds;
    for (uint32_t left = word(); left-- && isWhole;)
        kinds.append(literal());
    Vector<ASCIILiteral, 8> reasons;
    for (uint32_t left = word(); left-- && isWhole;)
        reasons.append(literal());
    auto filePath = file.utf8();
    for (uint32_t left = word(); left-- && isWhole;) {
        auto path = text();
        uint32_t numberOfRows = word();
        size_t start = at;
        at += static_cast<size_t>(numberOfRows) * 2;
        isWhole &= at <= words.size();
        if (!isWhole || !equalSpans(byteCast<uint8_t>(filePath.span()), path))
            continue;
        std::optional<ListedSite> found;
        unsigned rank = 0;
        for (size_t row = start; row < at; row += 2) {
            if (words[row] != position.line || words[row + 1] >> 12 != position.column)
                continue;
            uint32_t numberOfKind = words[row + 1] & 31;
            uint32_t numberOfReason = words[row + 1] >> 5 & 7;
            if (numberOfKind >= kinds.size())
                continue;
            ListedSite listed { kinds[numberOfKind], words[row + 1] >> 8 & 15 };
            bool hasKind = listed.kind == kind || (listed.kind == "as-convert"_s && kind == "as-refuse"_s) || (listed.kind == "field-redefine"_s && kind == "field-store-untyped"_s);
            if (kind != defaultViolationKind && !hasKind)
                continue;
            if (violation.parameter && listed.kind == "enter-function"_s && listed.ordinal != std::min(*violation.parameter, 15u))
                continue;
            unsigned violationRank = 1 + (numberOfReason < reasons.size() && reasons[numberOfReason] == violation.what);
            if (violationRank > rank) {
                rank = violationRank;
                found = listed;
            }
        }
        if (found && kind != defaultViolationKind)
            found->kind = kind;
        if (found && violation.parameter && found->kind == "enter-function"_s)
            found->ordinal = *violation.parameter;
        return found;
    }
    return std::nullopt;
}

void State::recordViolation(VM& vm, const Violation& violation)
{
    ViolationLog* log = ViolationLog::singletonIfEnabled(data.logsTypeErrors);
    if (!log) [[likely]]
        return;
    ASCIILiteral word = violation.got ? kindName(violation.got, violation.expected) : ASCIILiteral();
    String name = violation.got && word.isNull() ? describe(violation.got) : String();
    const void* topAddress = returnAddressIntoTopCompiledFrame(vm, violation.isAtCaller);
    Locker locker { log->lock };
    ViolationLog::Site* site = nullptr;
    uint64_t count = 0;
    auto countAt = [&](ViolationLog::Site* found) {
        site = found;
        if (!site)
            return false;
        count = ViolationLog::count(*site, word, name);
        if (!count) {
            word = "other"_s;
            count = ViolationLog::count(*site, word, name);
        }
        return ViolationLog::isEnabled(data.logsTypeErrors) && ViolationLog::isPowerOfTen(count);
    };
    auto find = [&](const void* address, bool isEntry, const Invocable<std::pair<String, LineColumn>()> auto& place, const Invocable<String()> auto& function) {
        ViolationLog::Address key { address, violation.kind.characters() };
        if (auto known = log->siteAt(key))
            return *known;
        ASCIILiteral kind = isEntry && violation.kind == defaultViolationKind ? "enter-function"_s : violation.kind;
        auto [file, position] = place();
        unsigned ordinal = violation.parameter.value_or(0);
        if (auto listed = listedSite(file, position, violation, kind)) {
            kind = listed->kind;
            ordinal = listed->ordinal;
        }
        uint64_t id = ViolationLog::idOf(file, position, kind, ordinal);
        return log->addSite(key, id + static_cast<uint64_t>(violation.action), [&, file = file, position = position] {
            auto made = makeUnique<ViolationLog::Site>();
            made->id = id;
            made->kind = kind;
            made->what = violation.what;
            made->action = violation.action;
            made->file = file.isolatedCopy();
            made->position = position;
            made->function = function().isolatedCopy();
            if (violation.on)
                made->on = nameOf(*violation.on).isolatedCopy();
            else if (violation.onBuiltin)
                made->on = nameOfLockedObject(vm, violation.onBuiltin).isolatedCopy();
            else if (violation.onFunction != UINT32_MAX)
                made->on = makeString(nameOfFunction(violation.onFunction), ".prototype"_s);
            if (violation.member)
                made->on = makeString(made->on, '.', StringView(violation.member));
            if (violation.expected)
                made->expected = describe(*violation.expected).isolatedCopy();
            else if (violation.expectedKinds)
                made->expected = describeKinds(*violation.expectedKinds, violation.expectedName).isolatedCopy();
            if (made->expected.length() > ViolationLog::maxLengthOfType)
                made->expected = makeString(StringView(made->expected).left(ViolationLog::maxLengthOfType - 3), "..."_s);
            return made;
        });
    };
    bool isCounted = false;
    bool isWritten = false;
    if (auto known = topAddress ? log->siteAt({ topAddress, violation.kind.characters() }) : std::nullopt) {
        isCounted = true;
        isWritten = countAt(*known);
        if (!isWritten)
            return;
    }
    auto placeOf = [](StackVisitor& visitor) {
        auto position = visitor->aotFunction().reportedPositionFor(visitor->bytecodeIndex());
        return std::pair { position && position->source ? ProgramData::get()->sourceName(position->source) : "unknown"_str, position ? position->lineColumn : LineColumn() };
    };
    Vector<String, ViolationLog::maxNumberOfCallers> callers;
    bool isAtTheTop = !violation.isAtCaller;
    bool isAfterSite = false;
    StackVisitor::visit(vm.topCallFrame, vm, [&](StackVisitor& visitor) {
        if (visitor->isTailDeleted() || (visitor->aotFunction() && visitor->isHiddenAOTFrame(isAtTheTop)))
            return IterationStatus::Continue;
        bool wasAtTop = std::exchange(isAtTheTop, false);
        if (!visitor->isTypedAOTFrame())
            return IterationStatus::Continue;
        if (isAfterSite) {
            auto [file, position] = placeOf(visitor);
            callers.append(makeString(file, ':', position.line));
            return callers.size() < ViolationLog::maxNumberOfCallers ? IterationStatus::Continue : IterationStatus::Done;
        }
        isAfterSite = true;
        auto parameterCheck = violation.kind == defaultViolationKind ? ProgramData::get()->parameterCheckAt(visitor->aotFunction().index, visitor->bytecodeIndex().offset()) : std::nullopt;
        if (parameterCheck) {
            auto [file, position] = placeOf(visitor);
            callers.append(makeString(file, ':', position.line));
        }
        if (!isCounted) {
            uint32_t function = parameterCheck ? parameterCheck->typedFunction : visitor->typedAOTFunction();
            isCounted = true;
            isWritten = countAt(find(removeCodePtrTag(visitor->returnPC()), parameterCheck || (wasAtTop && typedFunctionIsHiddenInStackTraces(function)), [&] {
                return parameterCheck ? std::pair { ProgramData::get()->sourceName(parameterCheck->source), LineColumn { parameterCheck->line, parameterCheck->column } } : placeOf(visitor);
            }, [&] {
                String name = typedFunctionCodeType(function) == ModuleCode ? "module"_str : typedFunctionName(function);
                return name.isEmpty() ? "anonymous"_str : name;
            }));
        }
        return isWritten ? IterationStatus::Continue : IterationStatus::Done;
    });
    if (!isCounted) {
        isWritten = countAt(find(nullptr, false, [] {
            return std::pair { "unknown"_str, LineColumn() };
        }, [] {
            return "unknown"_str;
        }));
    }
    if (isWritten) {
        String reason = violation.reason && !violation.reason->isEmpty() ? *violation.reason : String();
        if (reason.isNull() && violation.expected && violation.got && violation.action != ActionOnViolation::Copied) {
            SetForScope kindsOnly(s_describesKindsOnly, true);
            reason = explainMismatch(vm, violation.got, *violation.expected);
            if (reason.isEmpty())
                reason = String();
        }
        if (reason.length() > ViolationLog::maxLengthOfReason)
            reason = makeString(StringView(reason).left(ViolationLog::maxLengthOfReason - 3), "..."_s);
        log->write(*site, word.isNull() ? name : String(word), violation.action, count, callers, false, reason);
    }
}

static ASCIILiteral describeAssertion(uint32_t what);

void State::cacheCastTransition(Structure* before, uint32_t typeAndReason, uint64_t wordsBefore, Object* object, const TransitionPlan& plan)
{
    Structure* after = object->structure();
    if (before->typeInfo().type() != after->typeInfo().type() || before->typeInfo().inlineTypeFlags() != after->typeInfo().inlineTypeFlags() || before->indexingModeIncludingHistory() != after->indexingModeIncludingHistory())
        return;
    CastTransition transition { before->id().bits(), typeAndReason, wordsBefore, object->orderAndShapeWords(), after->id().bits(), 0, nullptr, 0, 0, 0 };
    if (plan.structure) {
        using Cleared = StoreTransition::Cleared;
        auto& cleared = *m_planClearedSlots.ensure(&plan, [&] {
            Vector<Cleared, 8> slots;
            for (auto& slot : plan.addedSlots) {
                if (auto planClearedSlot = clearedSlot(slot))
                    slots.append(*planClearedSlot);
            }
            std::ranges::sort(slots, { }, &Cleared::offset);
            auto merged = makeUnique<Vector<Cleared, 4>>();
            for (size_t i = 0; i < slots.size(); ++i) {
                auto slot = slots[i];
                if (slot.width == 4 && !(slot.offset % 8) && i + 1 < slots.size() && slots[i + 1].width == 4 && slots[i + 1].offset == slot.offset + 4) {
                    slot.width = 8;
                    slot.absent = static_cast<uint32_t>(slot.absent) | slots[++i].absent << 32;
                }
                merged->append(slot);
            }
            std::ranges::stable_sort(*merged, std::ranges::greater { }, &Cleared::width);
            return merged;
        }).iterator->value;
        transition.size = plan.size;
        transition.cleared = cleared.span().data();
        for (auto& slot : cleared)
            ++(slot.width == 8 ? transition.numberOfEightByteSlots : slot.width == 4 ? transition.numberOfFourByteSlots : transition.numberOfOneByteSlots);
    }
    m_castTransitions[CastTransition::placeOf(transition.structureID, typeAndReason, wordsBefore)] = transition;
}

bool State::tryPreviousTransition(VM& vm, JSValue value, uint32_t typeAndReason)
{
    if (!value.isCell())
        return false;
    const RuntimeType& type = this->type(typeAndReason >> 4);
    auto reason = static_cast<Format::CastReason>(typeAndReason & 15);
    Structure* structure = value.asCell()->structure();
    auto& made = previousTransition(structure, type, reason);
    if (made.structure != structure || made.type != &type || made.reason != reason)
        return false;
    Object* object = uncheckedDowncast<Object>(value.asCell());
    if (object->hasFlagsInOrderWord())
        return false;
    uint64_t wordsBefore = object->orderAndShapeWords();
    if (made.plan->structure && !object->specializeWithPlan(vm, *made.plan))
        return false;
    cacheCastTransition(structure, typeAndReason, wordsBefore, object, *made.plan);
    return true;
}

JSValue State::coerce(JSGlobalObject* globalObject, JSValue value, const RuntimeType& type, Format::CastReason reason, AuditSite site, CoercionResult resultMode, uint32_t typeAndReason)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool hasObjects = type.allowsAnyObject() || type.allowsAnyFunction() || type.acceptsUntypedObjects() || type.numberOfSupertypes() || type.numberOfJSTypes();
    bool keepsUnconvertibleObjects = reason == Format::CastReason::AsIfConvertible || reason == Format::CastReason::AsFreshObjectIfConvertible;
    if (keepsUnconvertibleObjects)
        reason = reason == Format::CastReason::AsIfConvertible ? Format::CastReason::As : Format::CastReason::AsFreshObject;
    bool isFreshObject = reason == Format::CastReason::AsFreshObject || reason == Format::CastReason::CoerceFreshObject;
    bool isConvertedEvenIfAccepted = reason == Format::CastReason::AsFreshObject;
    if (reason == Format::CastReason::AsFreshObject)
        reason = Format::CastReason::As;
    Format::CastReason requestedReason = reason;
    if (reason == Format::CastReason::CoerceFreshObject || reason == Format::CastReason::CoerceFreshGraph)
        reason = Format::CastReason::Coerce;
    if (reason == Format::CastReason::Coerce && !(value.isObject() && hasObjects)) {
        countCoercion(value, nullptr, type, reason, "converts"_s, value);
        JSValue converted = convert(globalObject, value, type, site);
        RETURN_IF_EXCEPTION(scope, { });
        if (converted != value)
            recordViolation(vm, { .kind = "as-convert"_s, .what = "Coerce"_s, .expected = &type, .got = value, .action = ActionOnViolation::Converted });
        return converted;
    }
    Object* compiled = compiledObject(value);
    if (!compiled && value.isObject() && type.acceptsUntypedObjectsByNature() && requestedReason == Format::CastReason::As) {
        countCoercion(value, nullptr, type, reason, "left as it is"_s, value);
        return value;
    }
    if (!compiled && reason == Format::CastReason::Coerce && (requestedReason == reason || !isPlainData(asObject(value))) && !data.isCompiledForAudit) {
        countCoercion(value, nullptr, type, reason, "no data"_s, value);
        return value;
    }
    if (!compiled && type.acceptsUntypedObjectsByNature() && !isConvertedEvenIfAccepted && reason != Format::CastReason::Flow && value.isObject() && isAcceptedUnchanged(asObject(value), type)) {
        countCoercion(value, nullptr, type, reason, "admitted"_s, value);
        return value;
    }
    if (!compiled && type.acceptsUntypedObjectsByNature() && !isConvertedEvenIfAccepted && reason != Format::CastReason::Flow && value.isObject() && !data.isCompiledForAudit && isUnchangedByCoercion(vm, asObject(value), type)) {
        countCoercion(value, nullptr, type, reason, "nothing below"_s, value);
        return value;
    }
    bool mayKeepByPlan = !compiled && type.acceptsUntypedObjectsByNature() && requestedReason == Format::CastReason::As && !isFreshObject && value.isObject() && asObject(value)->type() == FinalObjectType && !data.isCompiledForAudit;
    if (mayKeepByPlan && tryKeepByPlan(vm, asObject(value), type)) {
        countCoercion(value, nullptr, type, reason, "kept by plan"_s, value);
        return value;
    }
    ObjectsBelowKeptObject objectsBelow;
    if (mayKeepByPlan)
        collectObjectsBelowKeptObject(asObject(value), type, objectsBelow, 0);
    const LayoutData* before = compiled ? &compiled->layout() : nullptr;
    ASCIILiteral way = !compiled ? "foreign"_s : "flags"_s;
    if (Object* object = data.isCompiledForAudit ? nullptr : compiled; object && !object->hasFlagsInOrderWord()) {
        Structure* structureBefore = object->structure();
        if (TransitionPlan* plan = transitionPlan(globalObject, object, type, reason); plan && (!plan->structure || object->specialize(vm, *this, *plan))) {
            if (m_lastPlanDependsOnlyOnStructure && requestedReason == reason && !isFreshObject && !m_counts) {
                PreviousTransition& made = previousTransition(structureBefore, type, reason);
                if (made.structure && (made.structure != structureBefore || made.type != &type || made.reason != reason))
                    m_previousTransitionIndices.didReplaceEntry();
                made = { structureBefore, &type, plan, reason };
            }
            countCoercion(value, before, type, reason, "plan"_s, value);
            return value;
        }
        way = "no-plan"_s;
    }
    if (!compiled && !data.isCompiledForAudit && value.isCell() && value.asCell()->type() == FinalObjectType && (!type.acceptsUntypedObjectsByNature() || isConvertedEvenIfAccepted) && (requestedReason == Format::CastReason::As || requestedReason == Format::CastReason::CoerceFreshObject || requestedReason == Format::CastReason::CoerceFreshGraph)) {
        bool mayConvertInPlace = !isFreshObject && requestedReason != Format::CastReason::CoerceFreshGraph;
        if (Object* copy = copyUnchangedByPlan(vm, asObject(value), type, mayConvertInPlace, requestedReason == Format::CastReason::As ? typeAndReason : 0)) {
            bool didConvertInPlace = JSValue(copy) == value;
            bool hasRoom = didConvertInPlace || (mayConvertInPlace && hasCapacityToConvertInPlace(asObject(value), copy));
            bool copiesSharedObject = requestedReason == Format::CastReason::As && !isFreshObject && !hasRoom;
            if (!copiesSharedObject || !rejectsCopiesOfSharedObjects()) {
                if (copiesSharedObject)
                    recordViolation(vm, { .kind = "copy-on-enter"_s, .what = "As"_s, .expected = &type, .got = value, .action = ActionOnViolation::Copied });
                if (hasRoom && !didConvertInPlace)
                    become(vm, asObject(value), copy);
                countCoercion(value, nullptr, type, reason, "plan of a copy"_s, hasRoom ? value : JSValue(copy));
                return hasRoom ? value : JSValue(copy);
            }
        } else if (mayConvertInPlace && tryConvertInPlaceByPlans(vm, asObject(value), type)) {
            countCoercion(value, nullptr, type, reason, "plans of a graph"_s, value);
            return value;
        }
    }
    Coercion attempt = coercionFor(globalObject, *this, requestedReason, Coercion::Explains::No);
    if (requestedReason != Format::CastReason::CoerceFreshGraph)
        attempt.allowInPlaceConversion();
    if (isFreshObject && value.isObject())
        attempt.setUnsharedObject(asObject(value));
    if (resultMode == CoercionResult::ConvertedValue)
        attempt.allowCopyingProgramObjects();
    JSValue result = attempt.run(value, type);
    RETURN_IF_EXCEPTION(scope, { });
    countCoercion(value, before, type, reason, way, result);
    if (result == value)
        cacheConversionsBelowKeptObject(objectsBelow);
    if (result)
        return result;
    if (keepsUnconvertibleObjects && value.isObject() && type.acceptsOnlyOtherLayouts())
        return value;
    if ((reason == Format::CastReason::Flow || ((reason == Format::CastReason::Claim || reason == Format::CastReason::Store) && type.checksUntypedObjects())) && !compiled && value.isObject() && !data.isCompiledForAudit) {
        bool didConvertInPlace = convertInPlace(globalObject, asObject(value), type);
        RETURN_IF_EXCEPTION(scope, { });
        if (didConvertInPlace)
            return value;
    }
    if (reason == Format::CastReason::Coerce && !data.isCompiledForAudit) [[likely]]
        return value;
    Coercion coercion = coercionFor(globalObject, *this, requestedReason, Coercion::Explains::Yes);
    if (requestedReason != Format::CastReason::CoerceFreshGraph)
        coercion.allowInPlaceConversion();
    if (isFreshObject && value.isObject())
        coercion.setUnsharedObject(asObject(value));
    if (resultMode == CoercionResult::ConvertedValue)
        coercion.allowCopyingProgramObjects();
    {
        SetForScope kindsOnly(s_describesKindsOnly, ViolationLog::isEnabled(data.logsTypeErrors));
        result = coercion.run(value, type);
    }
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_ASSERT(!result);
    if (reason == Format::CastReason::Coerce) {
        reportCoercion(site, makeString("'as "_s, describe(type), "' left "_s, describe(value), " unchanged"_s, coercion.failure().isEmpty() ? ""_s : ": "_s, coercion.failure()));
        return value;
    }
    bool acceptsOnlyNullish = !hasObjects && !(type.primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent));
    if (data.isCompiledForAudit) [[unlikely]] {
        JSValue goesOn = assertionFailed(value, type, static_cast<uint32_t>(reason), site, coercion.failure());
        if (!acceptsOnlyNullish)
            return goesOn;
    }
    bool continuesAsUntypedObject = type.isRelaxedForLogging() && value.isObject();
    recordViolation(vm, { .kind = reason == Format::CastReason::Flow ? "narrowing"_s : defaultViolationKind, .what = describeAssertion(static_cast<uint32_t>(reason)), .expected = &type, .got = value, .reason = &coercion.failure(), .action = continuesAsUntypedObject ? ActionOnViolation::Continued : ActionOnViolation::Threw });
    if (continuesAsUntypedObject)
        return value;
    throwTypeError(globalObject, scope, withAOTErrorNote(mismatchMessage(vm, value, type, coercion.failure())));
    return { };
}

bool State::accepts(JSGlobalObject* globalObject, const RuntimeType& type, JSValue value)
{
    if (fits(type, value))
        return true;
    if (!type.acceptsOnlyOtherLayouts() || !compiledObject(value))
        return false;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = Coercion(globalObject, *this, Coercion::Mode::Test, Coercion::Completeness::HasAddedFields, Coercion::Explains::No).run(value, type);
    RETURN_IF_EXCEPTION(scope, true);
    return !!result;
}

JSValue State::hold(JSGlobalObject* globalObject, JSValue value, const RuntimeType& type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Coercion attempt(globalObject, *this, Coercion::Mode::Cast, Coercion::Completeness::HasAddedFields, Coercion::Explains::No);
    attempt.allowCopyingProgramObjects();
    if (!type.acceptsUntypedObjectsByNature())
        attempt.allowInPlaceConversion();
    JSValue result = attempt.run(value, type);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return result;
    Coercion coercion(globalObject, *this);
    coercion.allowCopyingProgramObjects();
    if (!type.acceptsUntypedObjectsByNature())
        coercion.allowInPlaceConversion();
    {
        SetForScope kindsOnly(s_describesKindsOnly, ViolationLog::isEnabled(data.logsTypeErrors));
        result = coercion.run(value, type);
    }
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_ASSERT(!result);
    if (data.isCompiledForAudit) [[unlikely]]
        return assertionFailed(value, type, static_cast<uint32_t>(Format::CastReason::Store), { }, coercion.failure());
    bool continuesAsUntypedObject = type.isRelaxedForLogging() && value.isObject();
    recordViolation(vm, { .kind = defaultViolationKind, .what = "Store"_s, .expected = &type, .got = value, .reason = &coercion.failure(), .action = continuesAsUntypedObject ? ActionOnViolation::Continued : ActionOnViolation::Threw });
    if (continuesAsUntypedObject)
        return value;
    throwTypeError(globalObject, scope, withAOTErrorNote(mismatchMessage(vm, value, type, coercion.failure())));
    return { };
}

bool State::convertInPlace(JSGlobalObject* globalObject, JSObject* object, const RuntimeType& type, String* failureReason)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!data.isCompiledForAudit && object->type() == FinalObjectType && !type.acceptsUntypedObjectsByNature() && tryConvertInPlaceByPlans(vm, object, type)) {
        countCoercion(object, nullptr, type, Format::CastReason::Claim, "plans of a graph"_s, object);
        return true;
    }
    Object* compiled = compiledObject(object);
    const LayoutData* before = compiled ? &compiled->layout() : nullptr;
    Coercion attempt(globalObject, *this, Coercion::Mode::Cast, Coercion::Completeness::HasAddedFields, Coercion::Explains::No);
    attempt.allowInPlaceConversion();
    attempt.disallowCopies();
    attempt.allowTransitionsToMembers();
    JSValue result = attempt.run(object, type);
    RETURN_IF_EXCEPTION(scope, false);
    countCoercion(object, before, type, Format::CastReason::Claim, compiled ? "held"_s : "foreign"_s, result);
    RELEASE_ASSERT(!result || result == JSValue(object));
    if (!result && failureReason) {
        Coercion explaining(globalObject, *this, Coercion::Mode::Cast, Coercion::Completeness::HasAddedFields, Coercion::Explains::Yes);
        explaining.allowInPlaceConversion();
        explaining.disallowCopies();
        explaining.allowTransitionsToMembers();
        explaining.run(object, type);
        RETURN_IF_EXCEPTION(scope, false);
        *failureReason = explaining.failure();
    }
    return !!result;
}

JSValue State::convert(JSGlobalObject* globalObject, JSValue value, const RuntimeType& type, AuditSite site)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (fits(type, value))
        return value;
    bool hasNumber = type.primitives & Primitive::Number;
    bool hasString = type.primitives & Primitive::String;
    bool hasBoolean = type.primitives & Primitive::Boolean;
    bool hasBigInt = type.primitives & Primitive::BigInt;
    bool hasMultipleKinds = hasNumber + hasString + hasBoolean + hasBigInt + !!(type.primitives & Primitive::Symbol) > 1;
    JSValue result = value;
    auto recordRejection = [&] {
        if (scope.exception() && vm.isTerminationException(scope.exception()))
            return;
        recordViolation(vm, { .kind = "as-refuse"_s, .what = "Coerce"_s, .expected = &type, .got = value });
    };
    if (hasMultipleKinds) {
        result = value.toPrimitive(globalObject);
        if (scope.exception()) [[unlikely]] {
            recordRejection();
            return { };
        }
        hasBigInt = false;
    }
    if (!fits(type, result)) {
        bool isConvertible = hasString ? !result.isSymbol() : hasNumber ? !result.isSymbol() && !result.isBigInt() : hasBoolean || result.isBoolean();
        if (!isConvertible && data.isCompiledForAudit) [[unlikely]]
            return assertionFailed(value, type, static_cast<uint32_t>(Format::CastReason::Coerce), site);
        if (hasString)
            result = result.toString(globalObject);
        else if (hasNumber)
            result = jsNumber(result.toNumber(globalObject));
        else if (hasBoolean)
            result = jsBoolean(result.toBoolean(globalObject));
        else if (hasBigInt)
            result = result.toBigInt(globalObject);
        if (scope.exception()) [[unlikely]] {
            recordRejection();
            return { };
        }
    }
    if (!fits(type, result)) {
        if (data.isCompiledForAudit) [[unlikely]]
            return assertionFailed(value, type, static_cast<uint32_t>(Format::CastReason::Coerce), site);
        recordRejection();
        throwTypeError(globalObject, scope, withAOTErrorNote(makeString("Cannot convert "_s, describe(value), " to "_s, describe(type))));
        return { };
    }
    if (data.isCompiledForAudit) [[unlikely]]
        reportCoercion(site, makeString("'as "_s, describe(type), "' converted "_s, describe(value), " to "_s, describe(result)));
    return result;
}

static uint64_t locationKey(uint32_t what, AuditSite site)
{
    return static_cast<uint64_t>(what & 0x7f) << 56 | static_cast<uint64_t>(site.function & 0xffffff) << 32 | site.lineAndColumn;
}

void State::reportCoercion(AuditSite site, const String& what)
{
    if (m_reportedLocations.add(locationKey(static_cast<uint32_t>(Format::CastReason::Coerce), site)).isNewEntry)
        dataLogLn("AOT: Coerce at ", describeSite(site), ": ", what, ignoredRulesNote());
}

void State::reportInsufficientCapacity(const LayoutData& layout, size_t cellSize, const LayoutData& wanted)
{
    if (data.isCompiledForAudit && m_reportedLocations.add(3ULL << 62 | static_cast<uint64_t>(layout.shape) << 32 | wanted.shape).isNewEntry)
        dataLogLn(makeString("AOT: cannot convert "_s, nameOf(layout), " to "_s, nameOf(wanted), " in place: the object has "_s, cellSize, " bytes, "_s, nameOf(wanted), " needs "_s, wanted.size, ignoredRulesNote()));
}

bool State::reportOrThrow(JSGlobalObject* globalObject, const Violation& violation, const String& message, InAudit inAudit, ASCIILiteral departure)
{
    VM& vm = globalObject->vm();
    if (data.isCompiledForAudit) {
        String place = "code that is not compiled"_s;
        StackVisitor::visit(vm.topCallFrame, vm, [&](StackVisitor& visitor) {
            if (!visitor->isTypedAOTFrame())
                return IterationStatus::Continue;
            String name = visitor->functionName();
            auto position = visitor->computeLineAndColumn();
            place = makeString(name.isEmpty() ? "<anonymous>"_str : name, " ("_s, visitor->sourceURL(), ':', position.line, ':', position.column, ')');
            return IterationStatus::Done;
        });
        if (m_reportedRules.add(makeString(place, ": "_s, message)).isNewEntry)
            dataLogLn("AOT: Rule at ", place, ": ", message, inAudit == InAudit::Ignored ? "; ignored" : "; carried out", departure.isNull() ? "" : "; ", departure.isNull() ? ""_s : departure, ignoredRulesNote());
        m_hasIgnoredRule |= inAudit == InAudit::Ignored;
        return true;
    }
    auto scope = DECLARE_THROW_SCOPE(vm);
    recordViolation(vm, violation);
    throwTypeError(globalObject, scope, withAOTErrorNote(message));
    return false;
}

void State::countAllocation(uint32_t number, JSCell* cell)
{
    if (!m_counts) [[likely]]
        return;
    auto& counts = m_counts->allocations[number];
    ++counts.objects;
    counts.cellSize = cell->cellSize();
    if (auto* object = dynamicDowncast<Object>(cell))
        countFieldsOf(object);
}

void State::countObject(Object* object)
{
    if (!m_counts) [[likely]]
        return;
    ++m_counts->objectsWithoutAllocation.add({ object->layout().shape, static_cast<uint32_t>(object->cellSize()) }, 0).iterator->value;
    countFieldsOf(object);
}

void State::countFieldsOf(Object* object)
{
    const LayoutData& layout = object->layout();
    if (!isObjectLayout(layout.kind))
        return;
    ++m_counts->orderStates.add({ layout.shape, object->orderWord() & orderAndCountMask }, 0).iterator->value;
    object->forEachFieldInKeyOrder(*this, [&](uint16_t index) {
        countFieldCreation(layout, layout.fields()[index]);
    });
}

void State::countTransition(const LayoutData& from, const LayoutData& to)
{
    if (!m_counts) [[likely]]
        return;
    ++m_counts->transitions.add({ from.shape, to.shape }, 0).iterator->value;
}

void State::countFieldCreation(const LayoutData& layout, const FieldData& field)
{
    if (!m_counts) [[likely]]
        return;
    auto& counts = m_counts->fieldCreations[layout.shape];
    if (counts.isEmpty())
        counts.fill(0, layout.numberOfFields);
    ++counts[&field - layout.fields()];
}

void State::countCompactTable(const String& event, uint32_t entries)
{
    if (!m_counts) [[likely]]
        return;
    auto& counts = m_counts->compactTables.add(event, std::pair<uint64_t, uint64_t> { }).iterator->value;
    ++counts.first;
    counts.second += entries;
}

void State::countExtension(uint32_t previousSize, uint32_t size)
{
    if (!m_counts) [[likely]]
        return;
    ++m_counts->extensions.add(size, 0).iterator->value;
    ++m_counts->extensionGrowth.add({ previousSize, size }, 0).iterator->value;
}

static ASCIILiteral describeElementKey(JSValue key)
{
    if (key.tryGetAsUint32Index())
        return key.isInt32() ? "int32"_s : "double"_s;
    return key.asNumber() < 0 && key.asNumber() == std::trunc(key.asNumber()) ? "negative"_s : "other"_s;
}

void State::countArrayGet(JSArray* array, JSValue key)
{
    if (!m_counts) [[likely]]
        return;
    auto index = key.tryGetAsUint32Index();
    ASCIILiteral place = " beyond"_s;
    if (index && *index < array->length()) {
        place = " within"_s;
        if (hasAnyArrayStorage(array->indexingType())) {
            ArrayStorage* storage = array->butterfly()->arrayStorage();
            place = *index >= storage->vectorLength() ? " map"_s : storage->m_vector[*index] ? " vector"_s : " hole"_s;
        }
    }
    String reason = makeString("key "_s, describeElementKey(key), " shape "_s, static_cast<unsigned>(array->indexingType() & IndexingShapeMask), place, array->type() != ArrayType ? " changed"_s : ""_s);
    ++m_counts->arrayGets.add(reason, 0).iterator->value;
}

void State::countCoercion(JSValue value, const LayoutData* before, const RuntimeType& type, Format::CastReason reason, ASCIILiteral way, JSValue result)
{
    if (!m_counts) [[likely]]
        return;
    static constexpr ASCIILiteral reasons[] = { "Store"_s, "As"_s, "NonNull"_s, "Claim"_s, "Flow"_s, "Upcast"_s, "Coerce"_s };
    String from = "primitive"_s;
    if (before) {
        String name = text(before->name);
        from = makeString(before->index, ' ', name.isEmpty() ? "-"_str : name);
    } else if (value.isCell())
        from = makeString("- "_s, value.asCell()->classInfo()->className);
    Object* after = result == value ? compiledObject(value) : nullptr;
    ASCIILiteral outcome = !result ? "failed"_s : result != value ? "other"_s : after && &after->layout() != before ? "retagged"_s : "same"_s;
    String key = makeString(reasons[static_cast<unsigned>(reason)], " from "_s, from, " way "_s, way, " outcome "_s, outcome, " type "_s, static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(&type) - reinterpret_cast<const uint8_t*>(&data)));
    auto added = m_counts->coercions.add(key, 0);
    if (added.isNewEntry)
        m_counts->coercionTypes.add(key, describe(type));
    ++added.iterator->value;
    if (Object* made = !before && result && value.isCell() ? compiledObject(result) : nullptr)
        ++m_counts->coercions.add(makeString("room: cell "_s, value.asCell()->cellSize(), " wanted "_s, sizeWithSpecializationCapacity(made->shape()), result == value ? " became"_s : " copied"_s, made->extraProperties() ? " superset"_s : " exact"_s, " at type "_s, static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(&type) - reinterpret_cast<const uint8_t*>(&data))), 0).iterator->value;
}

void State::countArrayPut(JSArray* array, JSValue key, IndexingType shapeBefore, unsigned vectorLengthBefore)
{
    if (!m_counts) [[likely]]
        return;
    if (shapeBefore < ArrayStorageShape && hasAnyArrayStorage(array->indexingType())) {
        ArrayStorage* storage = array->butterfly()->arrayStorage();
        size_t elements = storage->m_numValuesInVector + (storage->m_sparseMap ? storage->m_sparseMap->size() : 0);
        String record = makeString("shape "_s, static_cast<unsigned>(shapeBefore), " index "_s, key.isNumber() ? String::number(key.asNumber()) : "?"_str, " vector "_s, vectorLengthBefore, " elements "_s, elements ? elements - 1 : 0);
        ++m_counts->remainingArrays.add(record, 0).iterator->value;
    }
    String reason = makeString("key "_s, describeElementKey(key), " shape "_s, static_cast<unsigned>(shapeBefore), " to "_s, static_cast<unsigned>(array->indexingType() & IndexingShapeMask), array->type() != ArrayType ? " changed"_s : ""_s);
    ++m_counts->arrayPuts.add(reason, 0).iterator->value;
}

void State::countClosure(uint32_t function, unsigned numberOfCaptures)
{
    if (!m_counts) [[likely]]
        return;
    ++m_counts->closures.add({ function + 1, numberOfCaptures }, 0).iterator->value;
}

void State::countPlainAllocation(JSCell* cell, uint32_t function, uint32_t kindAndValue)
{
    if (!m_counts) [[likely]]
        return;
    Structure* structure = cell->structure();
    if (structure->compiledLayout() || !cell->isObject() || isSlimClosureType(cell->type()))
        return;
    size_t storage = 0;
    if (Butterfly* butterfly = asObject(cell)->butterfly(); butterfly && !isCopyOnWrite(structure->indexingMode())) {
        bool hasIndexingHeader = structure->hasIndexingHeader(cell);
        storage = Butterfly::totalSize(0, structure->outOfLineCapacity(), hasIndexingHeader, hasIndexingHeader ? butterfly->indexingHeader()->indexingPayloadSizeInBytes(structure) : 0);
    }
    std::pair<uint32_t, uint32_t> key { function + 1, kindAndValue };
    ++m_counts->plainAllocations.add(key, 0).iterator->value;
    m_counts->plainCellBytes.add(key, 0).iterator->value += cell->cellSize();
    m_counts->plainStorageBytes.add(key, 0).iterator->value += storage;
}

void State::countExtraPropertyTable(const LayoutData& layout)
{
    if (!m_counts) [[likely]]
        return;
    ++m_counts->extraPropertyTables[layout.shape];
}

void State::didBecomePrototype(const LayoutData& layout)
{
    if (m_prototypeShapes.get(layout.shape))
        return;
    m_prototypeShapes.set(layout.shape);
    for (uint32_t shape : std::span { data.at<uint32_t>(layout.specializationsOffset()), layout.numberOfSpecializations() })
        didBecomePrototype(layoutForShape(shape));
}

void State::countExtraProperty(const LayoutData& layout, PropertyName name)
{
    if (!m_counts) [[likely]]
        return;
    String key = parseIndex(name) ? "[index]"_str : name.isSymbol() ? makeString('[', StringView(name.uid()), ']') : String(name.uid());
    ++m_counts->extraProperties[layout.shape].add(key, 0).iterator->value;
}

void State::countAccess(AccessKind kind, const void* returnAddress, PropertyName name, JSValue receiver)
{
    if (!m_counts) [[likely]]
        return;
    auto place = Counts::Place::OtherReceiver;
    uintptr_t what = receiver.isCell() ? std::bit_cast<uintptr_t>(receiver.asCell()->classInfo()) : 0;
    if (Object* object = compiledObject(receiver)) {
        what = object->layout().shape;
        JSObject* extra = object->extraProperties();
        if (const FieldData* field = object->fieldNamed(*this, name))
            place = object->read(*field) ? Counts::Place::Field : Counts::Place::AbsentField;
        else
            place = extra && extra->getDirectOffset(*instance.vm, name) != invalidOffset ? Counts::Place::Extra : Counts::Place::NotOwn;
    }
    auto& access = m_counts->accesses.add({ std::bit_cast<uintptr_t>(returnAddress), static_cast<uint32_t>(kind) << 8 | static_cast<uint32_t>(place), what }, Counts::Access { }).iterator->value;
    ++access.count;
    access.name = name.uid();
}

void State::countCollection(CollectionScope scope)
{
    JSC::Heap& heap = instance.vm->heap;
    MarkedSpace& space = heap.objectSpace();
    Locker locker { m_counts->collectionsLock };
    size_t bytesInPreciseAllocations = 0;
    for (PreciseAllocation* allocation : space.preciseAllocations())
        bytesInPreciseAllocations += allocation->isLive() ? allocation->cellSize() : 0;
    size_t size = heap.size();
    size_t sizeAtLastWalk = m_counts->liveCells.collection ? m_counts->collections[m_counts->liveCells.collection - 1].size : 0;
    m_counts->collections.append({ scope == CollectionScope::Full, size, heap.capacity(), heap.extraMemorySize(), space.blocks().set().size(), bytesInPreciseAllocations });
    if (size < sizeAtLastWalk + sizeAtLastWalk / 20)
        return;

    using LiveKind = Counts::LiveKind;
    Counts::LiveCells live;
    live.collection = m_counts->collections.size();
    UncheckedKeyHashSet<JSCell*> extraPropertyTables;
    extraProperties.forEachConcurrently([&](Object*, JSObject* table) {
        extraPropertyTables.add(table);
    });
    live.extraPropertyTables = extraPropertyTables.size();
    UncheckedKeyHashSet<const StringImpl*> strings;
    UncheckedKeyHashMap<size_t, uint64_t> auxiliaryCells;
    UncheckedKeyHashMap<size_t, uint64_t> ownedAuxiliaryCells;
    auto count = [&](LiveKind kind, const void* what, size_t cellSize) {
        ++live.cells.add({ static_cast<uint32_t>(kind), std::bit_cast<uintptr_t>(what), cellSize }, 0).iterator->value;
    };
    auto countStorage = [&](LiveKind kind, const void* what, JSObject* object) {
        Structure* structure = object->structure();
        Butterfly* butterfly = object->butterfly();
        if (!butterfly || isCopyOnWrite(structure->indexingMode()))
            return;
        size_t cellSize = static_cast<HeapCell*>(butterfly->base(structure))->cellSize();
        count(kind, what, cellSize);
        ++ownedAuxiliaryCells.add(cellSize, 0).iterator->value;
    };
    auto countString = [&](JSString* string) {
        if (string->isRope()) {
            ++live.ropes;
            return;
        }
        const StringImpl* impl = string->tryGetValueImpl();
        if (!impl || !strings.add(impl).isNewEntry)
            return;
        ++live.strings;
        if (impl->isStatic())
            ++live.staticStrings;
        else if (impl->isExternal()) {
            ++live.externalStrings;
            live.stringBytes += sizeof(StringImpl);
        } else if (impl->isSubString()) {
            ++live.substrings;
            live.stringBytes += sizeof(StringImpl);
        } else
            live.stringBytes += impl->sizeInBytes();
    };
    auto arraySizes = [](JSArray* array) -> std::pair<uint64_t, uint64_t> {
        Butterfly* butterfly = array->butterfly();
        return { array->length(), butterfly ? butterfly->vectorLength() : 0 };
    };
    UncheckedKeyHashMap<JSCell*, uint32_t> arrayHolders;
    auto countSlots = [&](const LayoutData& layout, Object* object, size_t cellSize) {
        auto& slots = live.slots.add(&layout, Counts::LiveCells::Slots { }).iterator->value;
        if (isObjectLayout(layout.kind))
            ++live.orderStates.add({ layout.shape, object->orderWord() }, 0).iterator->value;
        ++slots.objects;
        slots.bytes += cellSize;
        if (layout.referencesOffset) {
            const uint32_t* references = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(&layout) + layout.referencesOffset);
            const uint32_t* offset = references + 3;
            auto containsCell = [&](uint32_t offset) {
                JSValue value = JSValue::decode(object->at<EncodedJSValue>(offset));
                return value && value.isCell();
            };
            for (uint32_t i = 0; i < references[0]; ++i)
                slots.filledBoxed += containsCell(*offset++);
            for (uint32_t i = 0; i < references[1]; ++i)
                slots.filledWide += containsCell(*offset++);
            for (uint32_t i = 0; i < references[2]; ++i)
                slots.filledNarrow += object->at<uint32_t>(*offset++) >= minCell32;
        }
        auto& fields = live.fields.add(&layout, Vector<Counts::LiveCells::Field>()).iterator->value;
        if (fields.isEmpty())
            fields.grow(layout.numberOfFields);
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            const FieldData& field = layout.fields()[i];
            if (!holdsCells(field.store()))
                continue;
            const void* slot = object->slotOf(field);
            JSValue value = slot ? readAs(field.store(), slot, nullptr) : JSValue();
            if (!value || !value.isCell())
                continue;
            auto& counts = fields[i];
            ++counts.filled;
            JSCell* held = value.asCell();
            const ClassInfo* classInfo = held->classInfo();
            if (classInfo == JSArray::info()) {
                auto [length, capacity] = arraySizes(uncheckedDowncast<JSArray>(held));
                ++counts.arrays;
                counts.emptyArrays += !length;
                counts.elements += length;
                counts.capacity += capacity;
                ++arrayHolders.add(held, 0).iterator->value;
            } else if (classInfo == JSMap::info()) {
                ++counts.maps;
                counts.emptyMaps += !uncheckedDowncast<JSMap>(held)->size();
            }
        }
    };
    auto visit = [&](HeapCell* heapCell, HeapCell::Kind kind, size_t cellSize) {
        if (!isJSCellKind(kind)) {
            ++auxiliaryCells.add(cellSize, 0).iterator->value;
            return;
        }
        if (heapCell->isZapped()) {
            count(LiveKind::Class, nullptr, cellSize);
            return;
        }
        JSCell* cell = static_cast<JSCell*>(heapCell);
        const ClassInfo* classInfo = cell->classInfo();
        if (const LayoutData* layout = cell->structure()->compiledLayoutOf(cell)) {
            count(LiveKind::Layout, layout, cellSize);
            countSlots(*layout, uncheckedDowncast<Object>(cell), cellSize);
            if (Extension* extension = layout->hasExtension ? uncheckedDowncast<Object>(cell)->extension() : nullptr) {
                count(LiveKind::Extension, nullptr, extension->size());
                ++ownedAuxiliaryCells.add(extension->size(), 0).iterator->value;
            }
        } else if (extraPropertyTables.contains(cell)) {
            count(LiveKind::ExtraProperties, nullptr, cellSize);
            countStorage(LiveKind::ExtraPropertiesStorage, nullptr, asObject(cell));
        } else {
            bool isClosure = classInfo == Closure::info() || classInfo == SlimClosure::info();
            count(isClosure ? LiveKind::Closure : LiveKind::Class, isClosure ? Closure::info() : classInfo, cellSize);
            if (classInfo == JSArray::info()) {
                auto [length, capacity] = arraySizes(uncheckedDowncast<JSArray>(cell));
                ++live.arrays[Counts::LiveCells::bucketOf(length)][Counts::LiveCells::bucketOf(capacity)];
            } else if (classInfo == JSMap::info())
                ++live.maps[Counts::LiveCells::bucketOf(uncheckedDowncast<JSMap>(cell)->size())];
            else if (classInfo == JSSet::info())
                ++live.sets[Counts::LiveCells::bucketOf(uncheckedDowncast<JSSet>(cell)->size())];
            if (classInfo == SlimClosure::info()) { }
            else if (cell->isObject())
                countStorage(LiveKind::ClassStorage, classInfo, asObject(cell));
            else if (cell->isString())
                countString(static_cast<JSString*>(cell));
        }
    };
    HeapIterationScope iterationScope(heap);
    space.forEachBlock([&](MarkedBlock::Handle* block) {
        size_t cellSize = block->cellSize();
        uint64_t liveCellsInBlock = 0;
        block->forEachLiveCell([&](size_t, HeapCell* cell, HeapCell::Kind kind) {
            ++liveCellsInBlock;
            visit(cell, kind, cellSize);
            return IterationStatus::Continue;
        });
        auto& freeCells = live.freeCells.add({ block->subspace(), cellSize }, Counts::FreeCells { }).iterator->value;
        freeCells.cells += block->cellsPerBlock() - liveCellsInBlock;
        ++freeCells.blocks;
        freeCells.emptyBlocks += !liveCellsInBlock;
    });
    for (PreciseAllocation* allocation : space.preciseAllocations()) {
        if (allocation->isLive())
            visit(allocation->cell(), allocation->attributes().cellKind, allocation->cellSize());
    }
    for (auto& entry : auxiliaryCells) {
        uint64_t owned = ownedAuxiliaryCells.get(entry.key);
        if (entry.value > owned)
            live.cells.add({ static_cast<uint32_t>(LiveKind::OtherAuxiliary), 0, entry.key }, entry.value - owned);
    }
    for (auto& entry : arrayHolders)
        ++(entry.value == 1 ? live.arraysReferencedOnce : live.arraysReferencedMoreThanOnce);
    m_counts->liveCells = WTF::move(live);
}

void State::printCounts() const
{
    auto nameOrDash = [&](uint32_t string) {
        String name = text(string);
        return name.isEmpty() ? "-"_str : name;
    };
    auto asWord = [](StringView name) -> String {
        if (name.isEmpty())
            return "-"_str;
        StringBuilder builder;
        for (char16_t unit : name.codeUnits()) {
            if (unit > ' ' && unit != 0x7f && unit != '\\')
                builder.append(unit);
            else
                builder.append("\\u"_s, hex(unit, 4, Lowercase));
        }
        return builder.toString();
    };
    auto sorted = [](const Counts::CountsByPair& counts) {
        Vector<std::tuple<uint32_t, uint32_t, uint64_t>> result;
        for (auto& entry : counts)
            result.append({ entry.key.first, entry.key.second, entry.value });
        std::ranges::sort(result);
        return result;
    };
    std::array<uint32_t, numberOfPinnedFieldGuards> pinnedFields { };
    for (uint32_t shape = 1; shape < data.numberOfLayouts; ++shape) {
        if (!hasLayoutForShape(shape))
            continue;
        const LayoutData& layout = layoutForShape(shape);
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            if (layout.fields()[i].flags & fieldIsPinned)
                ++pinnedFields[layout.fields()[i].flags >> pinnedFieldGuardShift];
        }
    }
    for (uint32_t guard = 0; guard < numberOfPinnedFieldGuards; ++guard) {
        if (pinnedFields[guard])
            dataLogLn("AOT count: guard ", guard, " pinned fields ", pinnedFields[guard], " set ", instance.typedModifiedLiterals[guard]);
    }
    for (uint32_t number = 0; number < m_counts->allocations.size(); ++number) {
        auto& counts = m_counts->allocations[number];
        if (!counts.objects)
            continue;
        const LayoutData& layout = layoutForShape(allocation(number).shape);
        dataLogLn("AOT count: allocation ", number, " layout ", layout.index, " ", nameOrDash(layout.name), " size ", allocation(number).size, " cell ", counts.cellSize, " objects ", counts.objects);
    }
    for (auto [shape, cellSize, objects] : sorted(m_counts->objectsWithoutAllocation)) {
        const LayoutData& layout = layoutForShape(shape);
        dataLogLn("AOT count: layout ", layout.index, " ", nameOrDash(layout.name), " cell ", cellSize, " objects ", objects);
    }
    for (auto [shape, word, times] : sorted(m_counts->orderStates)) {
        const LayoutData& layout = layoutForShape(shape);
        dataLogLn("AOT count: order-state ", layout.index, " ", nameOrDash(layout.name), " order ", orderOf(word), " count ", countOf(word), " times ", times);
    }
    for (auto [fromShape, toShape, objects] : sorted(m_counts->transitions)) {
        const LayoutData& from = layoutForShape(fromShape);
        const LayoutData& to = layoutForShape(toShape);
        dataLogLn("AOT count: transition ", from.index, " ", nameOrDash(from.name), " ", to.index, " ", nameOrDash(to.name), " objects ", objects);
    }
    for (uint32_t shape = 0; shape < m_counts->fieldCreations.size(); ++shape) {
        auto& counts = m_counts->fieldCreations[shape];
        for (uint32_t i = 0; i < counts.size(); ++i) {
            if (!counts[i])
                continue;
            const LayoutData& layout = layoutForShape(shape);
            dataLogLn("AOT count: field ", layout.index, " ", nameOrDash(layout.name), " ", nameOrDash(layout.fields()[i].name), " created ", counts[i]);
        }
    }
    Vector<std::pair<uint32_t, uint64_t>> extensions;
    for (auto& entry : m_counts->extensions)
        extensions.append({ entry.key, entry.value });
    std::ranges::sort(extensions);
    for (auto [size, objects] : extensions)
        dataLogLn("AOT count: extension size ", size, " objects ", objects);
    for (auto [from, to, objects] : sorted(m_counts->extensionGrowth))
        dataLogLn("AOT count: extension-growth from ", from, " to ", to, " objects ", objects);
    Vector<std::tuple<String, uint64_t, uint64_t>> compactTables;
    for (auto& entry : m_counts->compactTables)
        compactTables.append({ entry.key, entry.value.first, entry.value.second });
    std::ranges::sort(compactTables, [](auto& a, auto& b) {
        return codePointCompareLessThan(std::get<0>(a), std::get<0>(b));
    });
    for (auto& [event, tables, entries] : compactTables)
        dataLogLn("AOT count: compact-table ", event, " tables ", tables, " entries ", entries);
    Vector<std::pair<String, uint64_t>> arrayGets;
    for (auto& entry : m_counts->arrayGets)
        arrayGets.append({ entry.key, entry.value });
    std::ranges::sort(arrayGets, [](auto& a, auto& b) {
        return codePointCompareLessThan(a.first, b.first);
    });
    for (auto& [reason, reads] : arrayGets)
        dataLogLn("AOT count: array-get ", reason, " reads ", reads);
    Vector<std::pair<String, uint64_t>> coercions;
    for (auto& entry : m_counts->coercions)
        coercions.append({ entry.key, entry.value });
    std::ranges::sort(coercions, [](auto& a, auto& b) {
        return codePointCompareLessThan(a.first, b.first);
    });
    for (auto& [cast, calls] : coercions)
        dataLogLn("AOT count: coerce ", cast, " calls ", calls, " ", m_counts->coercionTypes.get(cast));
    Vector<std::pair<String, uint64_t>> arrayPuts;
    for (auto& entry : m_counts->arrayPuts)
        arrayPuts.append({ entry.key, entry.value });
    std::ranges::sort(arrayPuts, [](auto& a, auto& b) {
        return codePointCompareLessThan(a.first, b.first);
    });
    for (auto& [reason, stores] : arrayPuts)
        dataLogLn("AOT count: array-put ", reason, " stores ", stores);
    Vector<std::pair<String, uint64_t>> remainingArrays;
    for (auto& entry : m_counts->remainingArrays)
        remainingArrays.append({ entry.key, entry.value });
    std::ranges::sort(remainingArrays, [](auto& a, auto& b) {
        return codePointCompareLessThan(a.first, b.first);
    });
    for (auto& [record, arrays] : remainingArrays)
        dataLogLn("AOT count: array-left ", record, " arrays ", arrays);
    for (auto [function, numberOfCaptures, objects] : sorted(m_counts->closures)) {
        String name = nameOfFunction(function - 1);
        size_t cell = MarkedSpace::optimalSizeFor(usesSlimClosures(function - 1) ? SlimClosure::allocationSize(numberOfCaptures) : Closure::allocationSize(numberOfCaptures));
        dataLogLn("AOT count: closure f", function - 1, " ", name.isEmpty() ? "-"_str : name, " words ", numberOfCaptures, " cell ", cell, " objects ", objects);
    }
    for (auto [function, kindAndValue, objects] : sorted(m_counts->plainAllocations)) {
        static constexpr ASCIILiteral kinds[] = { "object"_s, "this"_s, "array"_s, "clone"_s };
        std::pair<uint32_t, uint32_t> key { function, kindAndValue };
        dataLogLn("AOT count: plain ", kinds[kindAndValue >> plainAllocationKindShift], " f", function - 1, " v", kindAndValue & ((1u << plainAllocationKindShift) - 1), " cell-bytes ", m_counts->plainCellBytes.get(key), " storage-bytes ", m_counts->plainStorageBytes.get(key), " objects ", objects);
    }
    for (uint32_t shape = 0; shape < m_counts->extraPropertyTables.size(); ++shape) {
        if (uint64_t objects = m_counts->extraPropertyTables[shape]) {
            const LayoutData& layout = layoutForShape(shape);
            dataLogLn("AOT count: extras ", layout.index, " ", nameOrDash(layout.name), " objects ", objects);
        }
    }
    for (uint32_t shape = 0; shape < m_counts->extraProperties.size(); ++shape) {
        Vector<std::pair<String, uint64_t>> properties;
        for (auto& entry : m_counts->extraProperties[shape])
            properties.append({ entry.key, entry.value });
        std::ranges::sort(properties, [](auto& a, auto& b) {
            return codePointCompareLessThan(a.first, b.first);
        });
        for (auto& [name, created] : properties) {
            const LayoutData& layout = layoutForShape(shape);
            dataLogLn("AOT count: extra ", layout.index, " ", nameOrDash(layout.name), " ", asWord(name), " created ", created);
        }
    }
    Vector<std::tuple<uint64_t, uintptr_t, uint32_t, uintptr_t, UniquedStringImpl*>> accesses;
    for (auto& entry : m_counts->accesses)
        accesses.append({ ~entry.value.count, std::get<0>(entry.key), std::get<1>(entry.key), std::get<2>(entry.key), entry.value.name.get() });
    std::ranges::sort(accesses);
    for (auto [invertedCount, address, kindAndLocation, what, name] : accesses) {
        static constexpr ASCIILiteral kinds[] = { "read"_s, "write"_s, "read-extra"_s, "write-extra"_s };
        static constexpr ASCIILiteral places[] = { "field"_s, "absent-field"_s, "extra"_s, "not-own"_s, "other-receiver"_s };
        auto place = static_cast<Counts::Place>(kindAndLocation & 0xff);
        auto location = FunctionRef::at(&instance, std::bit_cast<const void*>(address)).locationForReturnAddress(std::bit_cast<const void*>(address), nullptr);
        String function = location.typedFunction != FunctionRef::none ? typedFunctionName(location.typedFunction) : String();
        String where = "-"_s;
        if (auto position = location.function.reportedPositionFor(location.bytecodeIndex); position && position->source)
            where = makeString(ProgramData::get()->sourceName(position->source), ':', position->lineColumn.line, ':', position->lineColumn.column);
        String receiver = "- primitive"_s;
        if (place != Counts::Place::OtherReceiver) {
            const LayoutData& layout = layoutForShape(static_cast<uint32_t>(what));
            receiver = makeString(layout.index, ' ', nameOrDash(layout.name));
        } else if (what)
            receiver = makeString("- "_s, std::bit_cast<const ClassInfo*>(what)->className);
        dataLogLn("AOT count: access ", kinds[kindAndLocation >> 8], " ", function.isEmpty() ? "-"_str : function, " ", where, " ", asWord(name), " ", receiver, " ", places[static_cast<unsigned>(place)], " ", ~invertedCount);
    }

    Locker locker { m_counts->collectionsLock };
    auto printCollection = [&](ASCIILiteral prefix, size_t number) {
        auto& collection = m_counts->collections[number - 1];
        dataLogLn(prefix, number, collection.isFull ? " full"_s : " eden"_s, " size ", collection.size, " capacity ", collection.capacity, " extra ", collection.extraMemorySize, " blocks ", collection.blocks, " precise ", collection.bytesInPreciseAllocations);
    };
    for (size_t number = 1; number <= m_counts->collections.size(); ++number)
        printCollection("AOT collection: "_s, number);
    auto& live = m_counts->liveCells;
    if (!live.collection)
        return;
    printCollection("AOT live: collection "_s, live.collection);
    Vector<std::tuple<uint32_t, uint32_t, String, size_t, uint64_t>> lines;
    for (auto& entry : live.cells) {
        auto [kind, what, cellSize] = entry.key;
        auto className = [&] {
            return what ? String(std::bit_cast<const ClassInfo*>(what)->className) : "-"_str;
        };
        uint32_t index = 0;
        String start;
        switch (static_cast<Counts::LiveKind>(kind)) {
        case Counts::LiveKind::Layout: {
            const LayoutData& layout = *std::bit_cast<const LayoutData*>(what);
            index = layout.index;
            start = makeString("layout "_s, layout.index, ' ', nameOrDash(layout.name));
            break;
        }
        case Counts::LiveKind::Closure:
            start = "closure"_s;
            break;
        case Counts::LiveKind::Extension:
            start = "extension"_s;
            break;
        case Counts::LiveKind::ExtraProperties:
            start = "extras"_s;
            break;
        case Counts::LiveKind::ExtraPropertiesStorage:
            start = "extras-storage"_s;
            break;
        case Counts::LiveKind::Class:
            start = makeString("class "_s, className());
            break;
        case Counts::LiveKind::ClassStorage:
            start = makeString("auxiliary-of "_s, className());
            break;
        case Counts::LiveKind::OtherAuxiliary:
            start = "auxiliary"_s;
            break;
        }
        lines.append({ kind, index, WTF::move(start), cellSize, entry.value });
    }
    std::ranges::sort(lines, [](auto& a, auto& b) {
        if (std::get<0>(a) != std::get<0>(b))
            return std::get<0>(a) < std::get<0>(b);
        if (std::get<1>(a) != std::get<1>(b))
            return std::get<1>(a) < std::get<1>(b);
        if (auto order = codePointCompare(std::get<2>(a), std::get<2>(b)); order != std::strong_ordering::equal)
            return order == std::strong_ordering::less;
        return std::get<3>(a) < std::get<3>(b);
    });
    for (auto& [kind, index, start, cellSize, objects] : lines)
        dataLogLn("AOT live: ", start, " cell ", cellSize, " objects ", objects);
    for (auto [shape, word, objects] : sorted(live.orderStates)) {
        const LayoutData& layout = layoutForShape(shape);
        dataLogLn("AOT live: order-state ", layout.index, " ", nameOrDash(layout.name), " order ", orderOf(word), " count ", countOf(word), " flags ", word >> 28, " objects ", objects);
    }
    Vector<const LayoutData*> layoutsWithSlots;
    for (auto& entry : live.slots)
        layoutsWithSlots.append(entry.key);
    std::ranges::sort(layoutsWithSlots, [](auto* a, auto* b) {
        return a->index < b->index;
    });
    for (const LayoutData* layout : layoutsWithSlots) {
        auto& slots = live.slots.find(layout)->value;
        const uint32_t none[3] = { 0, 0, 0 };
        const uint32_t* references = layout->referencesOffset ? reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(layout) + layout->referencesOffset) : none;
        dataLogLn("AOT live: slots ", layout->index, " ", nameOrDash(layout->name), " size ", layout->size, " objects ", slots.objects, " bytes ", slots.bytes, " boxed ", references[0], " wide ", references[1], " narrow ", references[2], " filled-boxed ", slots.filledBoxed, " filled-wide ", slots.filledWide, " filled-narrow ", slots.filledNarrow);
        dataLog("AOT live: placed ", layout->index, " ", nameOrDash(layout->name), " size ", layout->size, " extension ", layout->hasExtension ? 1 : 0, " :");
        for (uint32_t i = 0; i < layout->numberOfFields; ++i) {
            const FieldData& field = layout->fields()[i];
            if (!(field.flags & fieldIsOutOfLine))
                dataLog(" ", field.offset, ":", static_cast<unsigned>(field.store()));
        }
        dataLogLn("");
        auto& fields = live.fields.find(layout)->value;
        for (uint32_t i = 0; i < layout->numberOfFields; ++i) {
            const FieldData& field = layout->fields()[i];
            if (!holdsCells(field.store()))
                continue;
            auto& counts = fields[i];
            dataLogLn("AOT live: field ", layout->index, " ", nameOrDash(layout->name), " ", nameOrDash(field.name), field.flags & fieldIsOutOfLine ? " out-of-line" : " in-line", " store ", static_cast<unsigned>(field.store()), " filled ", counts.filled, " arrays ", counts.arrays, " empty ", counts.emptyArrays, " elements ", counts.elements, " capacity ", counts.capacity, " maps ", counts.maps, " empty-maps ", counts.emptyMaps);
        }
    }
    for (unsigned length = 0; length < Counts::LiveCells::numberOfBuckets; ++length) {
        for (unsigned capacity = 0; capacity < Counts::LiveCells::numberOfBuckets; ++capacity) {
            if (uint64_t arrays = live.arrays[length][capacity])
                dataLogLn("AOT live: array length ", Counts::LiveCells::boundOf(length), " capacity ", Counts::LiveCells::boundOf(capacity), " arrays ", arrays);
        }
    }
    dataLogLn("AOT live: arrays-held once ", live.arraysReferencedOnce, " more ", live.arraysReferencedMoreThanOnce);
    for (unsigned size = 0; size < Counts::LiveCells::numberOfBuckets; ++size) {
        if (live.maps[size])
            dataLogLn("AOT live: map size ", Counts::LiveCells::boundOf(size), " objects ", live.maps[size]);
    }
    for (unsigned size = 0; size < Counts::LiveCells::numberOfBuckets; ++size) {
        if (live.sets[size])
            dataLogLn("AOT live: set size ", Counts::LiveCells::boundOf(size), " objects ", live.sets[size]);
    }
    dataLogLn("AOT live: extras-entries ", live.extraPropertyTables);
    dataLogLn("AOT live: strings ", live.strings, " bytes ", live.stringBytes, " ropes ", live.ropes, " static ", live.staticStrings, " external ", live.externalStrings, " substrings ", live.substrings);
    Vector<std::tuple<size_t, String, Counts::FreeCells>> freeCells;
    for (auto& entry : live.freeCells)
        freeCells.append({ entry.key.second, String::fromLatin1(entry.key.first->name().data()), entry.value });
    std::ranges::sort(freeCells, [](auto& a, auto& b) {
        if (std::get<0>(a) != std::get<0>(b))
            return std::get<0>(a) < std::get<0>(b);
        return codePointCompareLessThan(std::get<1>(a), std::get<1>(b));
    });
    for (auto& [cellSize, subspace, free] : freeCells)
        dataLogLn("AOT live: free cell ", cellSize, " cells ", free.cells, " blocks ", free.blocks, " empty ", free.emptyBlocks, " subspace ", subspace);
}

void State::printCountsAtExit()
{
    Locker locker { statesWithCountsLock };
    for (State* state : statesWithCounts())
        state->printCounts();
}

static bool isReliedOnInAudit(const RuntimeType& type)
{
    if (type.allowsAnyObject() || type.allowsAnyFunction() || type.acceptsUntypedObjects() || type.numberOfSupertypes() || type.numberOfJSTypes() || !type.primitives)
        return false;
    return !(type.primitives & ~Primitive::Boolean) || !(type.primitives & ~(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent));
}

JSValue State::nearestValueOfType(const RuntimeType& type, JSValue value) const
{
    if (!(type.primitives & (Primitive::Number | Primitive::Boolean)))
        return type.primitives & Primitive::Undefined ? jsUndefined() : jsNull();
    if (!(type.primitives & Primitive::Number)) {
        bool isTruthy = value.pureToBoolean() != TriState::False;
        return jsBoolean(type.primitives & (isTruthy ? Primitive::True : Primitive::False) ? isTruthy : !isTruthy);
    }
    double number = PNaN;
    if (value.isNumber())
        number = value.asNumber();
    else if (value.isBoolean())
        number = value.asBoolean();
    else if (value.isNull())
        number = 0;
    else if (value.isString()) {
        if (StringImpl* text = asString(value)->tryGetValueImpl())
            number = jsToNumber(StringView { *text });
    }
    if (fits(type, jsNumber(number)))
        return jsNumber(number);
    if (std::isnan(number) && type.primitives & Primitive::Undefined)
        return jsUndefined();
    double integer = std::isnan(number) ? 0 : std::trunc(number) + 0.0;
    return jsNumber(std::clamp(integer, static_cast<double>(type.minInteger()), static_cast<double>(type.maxInteger())));
}

static ASCIILiteral describeAssertion(uint32_t what)
{
    static constexpr ASCIILiteral reasons[] = { "Store"_s, "As"_s, "NonNull"_s, "Claim"_s, "Flow"_s, "Upcast"_s, "Coerce"_s };
    static constexpr ASCIILiteral operations[] = {
#define AOT_TYPED_OPCODE_NAME(name) #name ""_s,
        FOR_EACH_AOT_TYPED_OPCODE(AOT_TYPED_OPCODE_NAME)
#undef AOT_TYPED_OPCODE_NAME
    };
    if (!(what & assertionIsOnResult))
        return what < std::size(reasons) ? reasons[what] : "Cast"_s;
    uint32_t operation = what & ~assertionIsOnResult;
    return operation < std::size(operations) ? operations[operation] : "CallBuiltin"_s;
}

JSValue State::assertionFailed(JSValue value, const RuntimeType& type, uint32_t what, AuditSite site, const String& failure, JSValue storedTo)
{
    bool isConverted = isReliedOnInAudit(type) && type.primitives & (Primitive::Number | Primitive::Boolean);
    JSValue goesOn = isReliedOnInAudit(type) ? nearestValueOfType(type, value) : value;
    RELEASE_ASSERT(!isReliedOnInAudit(type) || fits(type, goesOn));
    if (Object* object = storedTo ? compiledObject(storedTo) : nullptr; object && object->isFrozen())
        return goesOn;
    if (!m_reportedLocations.add(locationKey(what & assertionIsOnResult ? 0x40 | what : what, site)).isNewEntry)
        return goesOn;
    bool isNonNull = what == static_cast<uint32_t>(Format::CastReason::NonNull) && value.isUndefinedOrNull();
    String message = isNonNull ? nonNullMessage(value) : makeString("expected "_s, describe(type), ", got "_s, describe(value));
    String reason = failure.isEmpty() ? explainMismatch(*instance.vm, value, type) : failure;
    dataLogLn(makeString("AOT: "_s, describeAssertion(what), " at "_s, describeSite(site), ": "_s, message, reason.isEmpty() ? ""_s : "; "_s, reason, isConverted ? "; converted to "_s : ""_s, isConverted ? describe(goesOn) : emptyString(), ignoredRulesNote()));
    return goesOn;
}

JSValue State::coerceToFieldType(JSGlobalObject* globalObject, const FieldData& field, JSValue value, String& failure)
{
    const RuntimeType& type = ownTypeOf(field);
    if (data.isCompiledForAudit && isReliedOnInAudit(type) && !fits(type, value)) [[unlikely]] {
        const RuntimeType& slot = typeOf(field);
        JSValue held = fits(slot, value) ? value : nearestValueOfType(slot, value);
        if (m_reportedLocations.add(1ULL << 63 | std::bit_cast<uintptr_t>(&field)).isNewEntry)
            dataLogLn("AOT: Store to '", text(field.name), "': expected ", describe(type), ", got ", describe(value), held == value ? "" : "; converted to ", held == value ? String() : describe(held), ignoredRulesNote());
        return held;
    }
    bool slotIsWiderThanType = field.type() != field.ownType();
    if (!value.isObject())
        return fits(type, value) || (slotIsWiderThanType && fits(typeOf(field), value)) ? value : JSValue();
    bool isCheckOnlyCast = !(field.flags & fieldIsAnnotated);
    if (isCheckOnlyCast && matchesTypedSubtype(type, asObject(value)))
        return value;
    if (Object* record = slotIsWiderThanType ? compiledObject(value) : nullptr; record && !acceptsShape(type, record->shape()) && acceptsShape(typeOf(field), record->shape()))
        return value;
    bool mayConvertInPlace = !isCheckOnlyCast && storesConvertPlainData;
    if (mayConvertInPlace && !type.acceptsUntypedObjectsByNature() && !data.isCompiledForAudit && value.asCell()->type() == FinalObjectType) {
        VM& vm = globalObject->vm();
        if (Object* copy = copyUnchangedByPlan(vm, asObject(value), type, true)) {
            bool didConvertInPlace = JSValue(copy) == value;
            bool hasRoom = didConvertInPlace || hasCapacityToConvertInPlace(asObject(value), copy);
            if (hasRoom || !rejectsCopiesOfSharedObjects()) {
                if (!hasRoom)
                    recordViolation(vm, { .kind = "copy-on-enter"_s, .what = "As"_s, .expected = &type, .got = value, .action = ActionOnViolation::Copied });
                if (hasRoom && !didConvertInPlace)
                    become(vm, asObject(value), copy);
                countCoercion(value, nullptr, type, Format::CastReason::Store, "plan of a copy"_s, hasRoom ? value : JSValue(copy));
                return hasRoom ? value : JSValue(copy);
            }
        } else if (tryConvertInPlaceByPlans(vm, asObject(value), type)) {
            countCoercion(value, nullptr, type, Format::CastReason::Store, "plans of a graph"_s, value);
            return value;
        }
    }
    Object* compiled = compiledObject(value);
    const LayoutData* before = compiled ? &compiled->layout() : nullptr;
    Coercion coercion(globalObject, *this, isCheckOnlyCast ? Coercion::Mode::Test : Coercion::Mode::Cast);
    coercion.allowCopyingProgramObjects();
    if (mayConvertInPlace)
        coercion.allowInPlaceConversion();
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = coercion.run(value, type);
    RETURN_IF_EXCEPTION(scope, { });
    countCoercion(value, before, type, Format::CastReason::Store, compiled ? "stored"_s : "foreign"_s, result);
    if (!result && isCheckOnlyCast && !compiled && !data.isCompiledForAudit) {
        bool didConvertInPlace = convertInPlace(globalObject, asObject(value), type);
        RETURN_IF_EXCEPTION(scope, { });
        if (didConvertInPlace)
            return value;
    }
    if (!result && type.isRelaxedForLogging()) [[unlikely]] {
        recordViolation(globalObject->vm(), { .kind = defaultViolationKind, .what = "Store"_s, .expected = &type, .got = value, .reason = &coercion.failure(), .action = ActionOnViolation::Continued });
        return value;
    }
    if (!result)
        failure = coercion.failure();
    return result;
}

JSCell* State::closureIfCreated(uint32_t function) const
{
    if (data.usesWideReferences)
        return static_cast<JSCell**>(instance.typedClosures)[function];
    uint32_t row = static_cast<uint32_t*>(instance.typedClosures)[function];
    return row ? MarkedBlockCage::decode(row + 1) : nullptr;
}

void State::didCreateClosureFor(uint32_t function, JSCell* cell)
{
    if (!(kindAndDeclarationOf(function) & functionIsInBuiltinModule))
        ensureProgramStaticsInitialized();
    if (data.usesWideReferences) {
        static_cast<JSCell**>(instance.typedClosures)[function] = cell;
        didCreate(cell);
        return;
    }
    Locker locker { m_cellsLock };
    static_cast<uint32_t*>(instance.typedClosures)[function] = MarkedBlockCage::encode(cell) - 1;
    m_recentClosures.cells.append(MarkedBlockCage::encode(cell));
    m_chunksWithClosures.set(function >> closuresPerChunkShift);
}

void State::registerConstructorClosure(Closure* closure)
{
    uint32_t function = closure->functionIndex();
    if (closureIfCreated(function) || closuresHaveCaptures(function))
        return;
    std::span allocationsByConstructor { data.at<std::array<uint32_t, 2>>(data.allocationsByConstructorOffset), data.numberOfAllocationsByConstructor };
    auto found = std::ranges::lower_bound(allocationsByConstructor, std::array<uint32_t, 2> { function, 0 });
    if (found == allocationsByConstructor.end() || (*found)[0] != function)
        return;
    didCreateClosureFor(function, closure);
}

JSObject* State::closureObjectFor(VM& vm, uint32_t function)
{
    if (!usesSlimClosures(function))
        return closure(vm, function);
    JSCell* cell = closureIfCreated(function);
    if (!cell) {
        cell = SlimClosure::create(vm, slimClosureStructure(vm, function), function);
        didCreateClosureFor(function, cell);
    }
    return asObject(cell);
}

Closure* State::closure(VM& vm, uint32_t function)
{
    JSCell* cell = closureIfCreated(function);
    if (!cell) {
        const LayoutData* ofClass = classConstructedBy(function);
        bool isCreatedOnChange = ofClass && createsClassObjectOnMutation(*ofClass) && createsPrototypeBeforeClassObject(*ofClass);
        Structure* structure = isCreatedOnChange ? lazyClassObjectStructure(vm, *ofClass) : closureStructure(vm, function);
        RELEASE_ASSERT(!closureIfCreated(function) && !usesSlimClosures(function));
        cell = Closure::create(vm, structure, function);
        didCreateClosureFor(function, cell);
        if (isCreatedOnChange && !canDeferClassObject(structure->storedPrototypeObject(), *ofClass))
            uncheckedDowncast<Closure>(cell)->ensureClassObject(vm);
        if (ofClass && !ofClass->hasUntypedBase && !isCreatedOnChange) {
            RELEASE_ASSERT(ofClass->isEvaluatedOnce);
            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            createClass(instance.globalObject, uncheckedDowncast<Closure>(cell), JSValue());
            scope.assertNoException();
        }
    }
    return uncheckedDowncast<Closure>(cell);
}

void State::rememberElement(VM& vm, JSObject* array, JSValue value)
{
    if (vm.heap.mutatorShouldBeFenced() || array->butterfly()->vectorLength() < minVectorLengthToRememberElements) {
        vm.writeBarrierSlowPath(array);
        return;
    }
    if (!value.isCell() || value.asCell()->cellState() == CellState::PossiblyBlack)
        return;
    if (instance.youngRootsCursor == instance.youngRootsEnd) {
        size_t size = instance.youngRootsCursor - m_youngRoots;
        size_t capacity = std::max<size_t>(2 * size, 4096);
        m_youngRoots = static_cast<JSCell**>(fastRealloc(m_youngRoots, capacity * sizeof(JSCell*)));
        instance.youngRootsCursor = m_youngRoots + size;
        instance.youngRootsEnd = m_youngRoots + capacity;
    }
    *instance.youngRootsCursor++ = value.asCell();
}

template<typename Visitor, typename Functor>
bool State::RecentlyCreated::visit(VM& vm, Visitor& visitor, const Functor& handOver)
{
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        HeapVersion version = vm.heap.objectSpace().newlyAllocatedVersion();
        bool isFirstVisitInCollection = version != lastVisitVersion;
        if (isFirstVisitInCollection) {
            lastVisitVersion = version;
            earlier.append(cells.span().first(numberVisited));
            if (numberVisited == cells.size())
                cells.clear();
            else
                cells.removeAt(0, numberVisited);
        }
        size_t visitedInThisCollection = isFirstVisitInCollection ? 0 : numberVisited;
        numberVisited = cells.size();
        if (vm.heap.collectionScope() != CollectionScope::Eden && isFirstVisitInCollection) {
            for (uint32_t cell : earlier)
                handOver(visitor, MarkedBlockCage::decode(cell));
        }
        for (uint32_t cell : cells.span().subspan(visitedInThisCollection))
            handOver(visitor, MarkedBlockCage::decode(cell));
        return true;
    }
    return false;
}

template<typename Visitor>
void State::visitClosures(Visitor& visitor)
{
    Locker locker { m_cellsLock };
    if (data.usesWideReferences || m_recentClosures.visit(*instance.vm, visitor, [](auto& visitor, JSCell* cell) {
        visitor.appendUnbarriered(cell);
    }))
        return;
    std::span rows { static_cast<const uint32_t*>(instance.typedClosures), data.numberOfFunctions };
    for (size_t stretch : m_chunksWithClosures) {
        size_t first = stretch << closuresPerChunkShift;
        for (uint32_t row : rows.subspan(first, std::min<size_t>(1 << closuresPerChunkShift, rows.size() - first))) {
            if (row)
                visitor.appendUnbarriered(MarkedBlockCage::decode(row + 1));
        }
    }
}

template<typename Visitor>
void State::visitStrings(Visitor& visitor)
{
    Locker locker { m_cellsLock };
    for (JSCell* cell : m_wideStrings)
        visitor.appendUnbarriered(cell);
    if (data.usesWideReferences)
        return;
    bool didVisit = m_recentStrings.visit(*instance.vm, visitor, [](auto& visitor, JSCell* cell) {
        StringImpl* impl = uncheckedDowncast<JSString>(cell)->tryGetValueImpl();
        if (!impl) [[unlikely]] {
            visitor.appendUnbarriered(cell);
            return;
        }
        if (visitor.markLeafCell(cell))
            visitor.reportLeafCellExtraMemoryVisited(cell->vm().heap, impl->costDuringGC());
    });
    didVisit &= m_recentStaticDataStrings.visit(*instance.vm, visitor, [](auto& visitor, JSCell* cell) {
        visitor.markLeafCell(cell);
    });
    if (didVisit)
        return;
    for (uint64_t slot : std::span { instance.negativeOffsetFields().typedStrings, stringTableCapacity(instance.negativeOffsetFields().typedStrings) }.subspan(1)) {
        if (slot)
            visitor.appendUnbarriered(MarkedBlockCage::decode(slot >> 32));
    }
}

template<typename Visitor>
void State::visitRootsWithCards(Visitor& visitor)
{
    static_assert(Instance::typedCardShift == Data::cardShift);
    const uint32_t* masks = data.at<uint32_t>(data.rootMasksOffset);
    auto visitCard = [&](uint32_t card) {
        auto* words = reinterpret_cast<const EncodedJSValue*>(instance.typedStatics + (static_cast<size_t>(card) << Instance::typedCardShift));
        for (uint32_t mask = masks[card]; mask; mask &= mask - 1)
            visitor.appendUnbarriered(JSValue::decode(words[std::countr_zero(mask)]));
    };
    if constexpr (!std::is_same_v<Visitor, SlotVisitor>) {
        for (uint32_t card = m_inaccessibleStaticsSize >> Instance::typedCardShift; card < data.numberOfCards; ++card)
            visitCard(card);
        return;
    } else {
        JSC::Heap& heap = instance.vm->heap;
        bool all = false;
        if (heap.collectionScope() != CollectionScope::Eden && m_lastFullCardVisitVersion != heap.objectSpace().markingVersion()) {
            m_lastFullCardVisitVersion = heap.objectSpace().markingVersion();
            all = true;
        }
        bool worldIsStopped = heap.worldIsStopped();
        for (uint32_t first = 0; first < data.numberOfCards; first += sizeof(uint64_t)) {
            static_assert(Instance::typedCardIsDirty == 1 && Instance::typedCardWasScanned == 2);
            uint64_t states = *reinterpret_cast<const uint64_t*>(instance.typedCards + first);
            if (!(all ? states : states & 0x0101010101010101ull))
                continue;
            for (uint32_t card = first; card < first + sizeof(uint64_t); ++card) {
                uint8_t state = instance.typedCards[card];
                if (state != Instance::typedCardIsDirty && !(all && state))
                    continue;
                if (worldIsStopped)
                    instance.typedCards[card] = Instance::typedCardWasScanned;
                visitCard(card);
            }
        }
    }
}

template<typename Visitor>
void State::visit(Visitor& visitor)
{
    visitStrings(visitor);
    visitClosures(visitor);
    {
        Locker locker { m_cellsLock };
        size_t first = 0;
        if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
            if (HeapVersion version = instance.vm->heap.objectSpace().newlyAllocatedVersion(); version != m_lastVisitVersion) {
                m_lastVisitVersion = version;
                m_previouslyMarkedCellCount = m_numberOfCellsVisited;
                if (instance.vm->heap.collectionScope() == CollectionScope::Eden)
                    first = m_previouslyMarkedCellCount;
            } else
                first = m_numberOfCellsVisited;
            m_numberOfCellsVisited = m_cells.size();
        }
        for (JSCell* cell : m_cells.span().subspan(first))
            visitor.appendUnbarriered(cell);
    }
    if (instance.vm->heap.collectionScope() == CollectionScope::Eden) {
        for (JSCell* cell : std::span { m_youngRoots, instance.youngRootsCursor })
            visitor.appendUnbarriered(cell);
    }
    instance.youngRootsCursor = m_youngRoots;
    for (JSCell* cell : std::initializer_list<JSCell*> { moduleRecord, importMeta, m_closureStructures[0], m_closureStructures[1], m_closureStructures[2], m_closureStructures[3], m_closureStructures[4], m_liveBindingGetter, m_liveBindingAccessor, asyncGeneratorImmediateStep }) {
        if (cell)
            visitor.appendUnbarriered(cell);
    }
    visitor.appendUnbarriered(newTarget);
    for (auto& [entryPoint, import] : entryPointImports) {
        visitor.appendUnbarriered(entryPoint);
        visitor.appendUnbarriered(import);
    }
    const uint32_t* roots = data.at<uint32_t>(data.rootsOffset);
    uint32_t sizeWithoutAccess = m_inaccessibleStaticsSize;
    WTF::loadLoadFence();
    for (uint32_t i = 0; i < data.numberOfRoots; ++i) {
        if (roots[i] >= sizeWithoutAccess)
            visitor.appendUnbarriered(JSValue::decode(*reinterpret_cast<EncodedJSValue*>(instance.typedStatics + roots[i])));
    }
    visitRootsWithCards(visitor);
}

template void State::visit(AbstractSlotVisitor&);
template void State::visit(SlotVisitor&);

static const Data& dataInImage()
{
    static const Data* s_data;
    if (!s_data) [[unlikely]] {
        Image* image = Image::withCode();
        RELEASE_ASSERT(image && image->header().typedDataSize);
        const Data* data = image->at<Data>(image->header().typedDataOffset);
        WTF::storeStoreFence();
        s_data = data;
    }
    return *s_data;
}

static uint8_t kindByteOf(uint32_t function)
{
    const Data& data = dataInImage();
    RELEASE_ASSERT(function < data.numberOfFunctions);
    return data.at<uint8_t>(data.functionKindsOffset)[function];
}

static uint32_t functionOwner(const Data& data, uint32_t function)
{
    const uint32_t* runs = data.at<uint32_t>(data.functionAliasesOffset);
    const uint16_t* lowHalves = reinterpret_cast<const uint16_t*>(runs + 2 * data.numberOfFunctionAliasRuns);
    const uint8_t* highBytes = reinterpret_cast<const uint8_t*>(lowHalves + data.numberOfFunctionAliases);
    uint32_t begin = 0;
    uint32_t end = data.numberOfFunctionAliasRuns;
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2;
        if (runs[2 * middle] <= function)
            begin = middle + 1;
        else
            end = middle;
    }
    if (!begin)
        return function;
    uint32_t index = runs[2 * begin - 1] + (function - runs[2 * begin - 2]);
    uint32_t afterRun = begin < data.numberOfFunctionAliasRuns ? runs[2 * begin + 1] : data.numberOfFunctionAliases;
    return index < afterRun ? static_cast<uint32_t>(highBytes[index]) << 16 | lowHalves[index] : function;
}

static JSString* functionSourceText(JSGlobalObject* globalObject, uint32_t function)
{
    const Data& data = dataInImage();
    if (!data.functionTextsOffset)
        return nullptr;
    const FunctionTexts& texts = *data.at<FunctionTexts>(data.functionTextsOffset);
    std::span entries { data.at<std::array<uint32_t, 3>>(texts.entriesOffset), texts.numberOfEntries };
    auto entry = std::ranges::lower_bound(entries, function, { }, [](auto& entry) {
        return entry[0];
    });
    if (entry == entries.end() || (*entry)[0] != function)
        return nullptr;
    VM& vm = globalObject->vm();
    if (texts.textOffset)
        return jsOwnedString(vm, StringImpl::createWithoutCopying(std::span { data.at<Latin1Character>(texts.textOffset) + (*entry)[1], (*entry)[2] - (*entry)[1] }));
    if (!s_functionSourceProvider)
        return nullptr;
    const uint32_t* path = data.at<uint32_t>(texts.pathsOffset) + 2 * (*entry)[1];
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = s_functionSourceProvider(globalObject, String::fromUTF8(std::span { data.at<char8_t>(path[0]), path[1] }), (*entry)[2]);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return text.isNull() ? nullptr : jsString(vm, WTF::move(text));
}

static Lock namesReportedForFramesLock;
static std::atomic<bool> hasNamesReportedForFrames;
static UncheckedKeyHashMap<uint32_t, String, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>& namesReportedForFrames()
{
    static NeverDestroyed<UncheckedKeyHashMap<uint32_t, String, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> names;
    return names;
}

static void setNameReportedForFrames(uint32_t function, const String& name, bool isOnlyClosure)
{
    Locker locker { namesReportedForFramesLock };
    uint32_t owner = functionOwner(dataInImage(), function);
    if (isOnlyClosure)
        namesReportedForFrames().set(owner, name.isolatedCopy());
    else if (auto result = namesReportedForFrames().add(owner, String()); result.isNewEntry)
        result.iterator->value = name.isolatedCopy();
    else if (result.iterator->value != name)
        result.iterator->value = String();
    hasNamesReportedForFrames.store(true, std::memory_order_release);
}

static String nameReportedForFrames(const Data& data, uint32_t function)
{
    if (!hasNamesReportedForFrames.load(std::memory_order_acquire)) [[likely]]
        return String();
    Locker locker { namesReportedForFramesLock };
    return namesReportedForFrames().get(functionOwner(data, function)).isolatedCopy();
}

static bool usesOwnPropertiesForStackTraceName(JSCell* closure)
{
    DeclaredAs declared = declaredAs(closure);
    return declared == DeclaredAs::Function || declared == DeclaredAs::Class;
}

static Vector<uint32_t, 4> functionsOwnedBy(VM& vm, const Data& data, uint32_t owner)
{
    static NeverDestroyed<UncheckedKeyHashMap<uint32_t, Vector<uint32_t, 4>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> known;
    Locker locker { namesReportedForFramesLock };
    auto result = known->add(owner, Vector<uint32_t, 4> { });
    if (!result.isNewEntry)
        return result.iterator->value;
    result.iterator->value.append(owner);
    const uint32_t* runs = data.at<uint32_t>(data.functionAliasesOffset);
    const uint16_t* lowHalves = reinterpret_cast<const uint16_t*>(runs + 2 * data.numberOfFunctionAliasRuns);
    const uint8_t* highBytes = reinterpret_cast<const uint8_t*>(lowHalves + data.numberOfFunctionAliases);
    uint64_t numberOfReads = 0;
    auto ownerAt = [&](uint32_t index) {
        ++numberOfReads;
        return static_cast<uint32_t>(highBytes[index]) << 16 | lowHalves[index];
    };
    uint32_t run = 0;
    for (uint32_t end = data.numberOfFunctionAliasRuns; run < end;) {
        uint32_t middle = run + (end - run) / 2;
        if (runs[2 * middle] <= data.firstGenericEntrypoint)
            run = middle + 1;
        else
            end = middle;
    }
    if (run)
        --run;
    for (; run < data.numberOfFunctionAliasRuns && runs[2 * run] < data.firstGenericEntrypoint + data.numberOfConsecutiveGenericEntrypoints; ++run) {
        uint32_t first = runs[2 * run + 1];
        uint32_t behind = run + 1 < data.numberOfFunctionAliasRuns ? runs[2 * run + 3] : data.numberOfFunctionAliases;
        uint32_t index = first;
        for (uint32_t end = behind; index < end;) {
            uint32_t middle = index + (end - index) / 2;
            if (ownerAt(middle) < owner)
                index = middle + 1;
            else
                end = middle;
        }
        for (; index < behind && ownerAt(index) == owner; ++index)
            result.iterator->value.append(runs[2 * run] + (index - first));
    }
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countOperation("functionsOwnedBy", "aliases-read", numberOfReads);
    return result.iterator->value;
}

static String singleClosureDisplayName(VM& vm, const Data& data, uint32_t function)
{
    if (!anyClosureHasDisplayName.load(std::memory_order_relaxed)) [[likely]]
        return String();
    auto candidates = functionsOwnedBy(vm, data, functionOwner(data, function));
    for (Instance* instance : vm.m_aotInstances) {
        if (!instance->typed)
            continue;
        for (uint32_t candidate : candidates) {
            JSCell* cell = instance->typed->closureIfCreated(candidate);
            if (!cell || !usesOwnPropertiesForStackTraceName(cell))
                continue;
            JSObject* closure = asObject(cell);
            unsigned attributes;
            PropertyOffset offset = closure->structure()->getConcurrently(vm.propertyNames->displayName.impl(), attributes);
            if (offset == invalidOffset || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessorOrValue))
                continue;
            JSValue value = closure->getDirect(offset);
            if (!value || !value.isString())
                continue;
            String displayName = asString(value)->tryGetValueWithoutGC();
            if (!displayName.isEmpty())
                return displayName;
        }
    }
    return String();
}

DeclaredAs declaredAs(JSCell* cell)
{
    if (auto* slim = dynamicDowncast<SlimClosure>(cell))
        return slim->kindAndDeclaration() & functionIsAsync && static_cast<Format::FunctionKind>(slim->kindAndDeclaration() & functionKindMask) != Format::FunctionKind::Bound ? DeclaredAs::AsyncFunction : DeclaredAs::Function;
    auto* closure = dynamicDowncast<Closure>(cell);
    if (!closure)
        return DeclaredAs::NotCompiled;
    if (closure->kind() == Format::FunctionKind::Constructor)
        return DeclaredAs::Class;
    if (closure->kind() == Format::FunctionKind::Bound)
        return DeclaredAs::Function;
    if (closure->isDeclaredAs(functionIsGenerator))
        return closure->isDeclaredAs(functionIsAsync) ? DeclaredAs::AsyncGeneratorFunction : DeclaredAs::GeneratorFunction;
    return closure->isDeclaredAs(functionIsAsync) ? DeclaredAs::AsyncFunction : DeclaredAs::Function;
}

bool isGeneratorObject(JSCell* cell)
{
    return cell->inherits<Generator>() || cell->inherits<AsyncGenerator>();
}

void reifyClassPrototypeIfNeeded(JSObject* object)
{
    ClassPrototype::look(object);
}

bool tryAssignPrimitivesToCompiledObjectFields(VM& vm, JSObject* target, JSObject* source)
{
    const LayoutData* layout = target->structure()->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return false;
    auto* typedTarget = uncheckedDowncast<Object>(target);
    State& state = typedTarget->state();
    if (typedTarget->hasFlagsInOrderWord() || state.data.isCompiledForAudit || source == target)
        return false;
    Vector<std::pair<const FieldData*, JSValue>, 8> stores;
    auto note = [&](UniquedStringImpl& name, JSValue value) {
        const FieldData* field = value.isObject() ? nullptr : typedTarget->fieldNamed(state, &name);
        if (!field || typedTarget->isDiscriminant(*field) || !typedTarget->readInline(*field))
            return false;
        stores.append({ field, value });
        return true;
    };
    bool isNoted = true;
    Structure* sourceStructure = source->structure();
    if (sourceStructure->compiledLayout()) {
        if (!isObjectLayout(sourceStructure->compiledLayout()->kind))
            return false;
        bool isWalked = uncheckedDowncast<Object>(source)->forEachFieldAsProperty([&](UniquedStringImpl& name, JSValue value) {
            isNoted = note(name, value);
            return isNoted;
        }, Object::SymbolKeys::Include);
        if (!isWalked)
            return false;
    } else {
        if (source->type() != FinalObjectType || source->hasNonReifiedStaticProperties() || source->canHaveExistingOwnIndexedProperties() || !sourceStructure->canPerformFastPropertyEnumerationCommon())
            return false;
        sourceStructure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
            if (entry.attributes() & PropertyAttribute::DontEnum)
                return true;
            isNoted = note(*entry.key(), source->getDirect(entry.offset()));
            return isNoted;
        });
    }
    if (!isNoted)
        return false;
    for (auto& [field, value] : stores) {
        if (!typedTarget->write(vm, *field, value))
            return false;
    }
    ensureStillAliveHere(source);
    return true;
}

JSValue tryGetCompiledObjectOwnData(VM& vm, JSCell* cell, UniquedStringImpl* name)
{
    return Object::tryGetOwnData(vm, cell, name);
}

std::optional<std::pair<uint32_t, uint32_t>> fieldOffsetAndKindForInterpreter(JSCell* cell, UniquedStringImpl* name)
{
    static_assert(getByIdMaxCell32Immediate == undefinedCell32 && getByIdCell32Shift == MarkedBlockCage::shift && 1ull << getByIdMarkedBlockCageBits == MarkedBlockCage::maxSize && getByIdMinInlineInt32 == minInlineInt32);
    const LayoutData* layout = cell->structure()->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return std::nullopt;
    auto* object = uncheckedDowncast<Object>(cell);
    const FieldData* field = object->fieldNamed(object->state(), name);
    static_assert(getByIdAbsentInt32 == absentInt32);
    if (!field || field->flags & fieldIsOutOfLine || field->offset > UINT16_MAX)
        return std::nullopt;
    auto kind = [&]() -> std::optional<GetByIdCompiledFieldKind> {
        switch (field->store()) {
        case Store::Cell:
        case Store::JSValue:
            return GetByIdCompiledFieldKind::JSValue;
        case Store::Cell32:
        case Store::OptionalCell32:
            return GetByIdCompiledFieldKind::Cell32;
        case Store::Int32OrNumberBox:
            return GetByIdCompiledFieldKind::Int32OrNumberBox;
        case Store::Int32:
            return GetByIdCompiledFieldKind::Int32;
        case Store::Uint8:
            return GetByIdCompiledFieldKind::Uint8;
        case Store::Int8:
            return GetByIdCompiledFieldKind::Int8;
        case Store::Uint16:
            return GetByIdCompiledFieldKind::Uint16;
        case Store::Int16:
            return GetByIdCompiledFieldKind::Int16;
        case Store::Uint32:
            return GetByIdCompiledFieldKind::Uint32;
        case Store::Int64:
            return GetByIdCompiledFieldKind::Int64;
        case Store::Boolean:
            return GetByIdCompiledFieldKind::Boolean;
        case Store::Double:
        case Store::OptionalDouble:
            return GetByIdCompiledFieldKind::Double;
        case Store::OptionalInt64:
            return GetByIdCompiledFieldKind::OptionalInt64;
        case Store::OptionalBoolean:
            return GetByIdCompiledFieldKind::OptionalBoolean;
        default:
            return std::nullopt;
        }
    }();
    if (!kind)
        return std::nullopt;
    return std::pair { field->offset, static_cast<uint32_t>(*kind) };
}

bool tryStorePrimitiveToCompiledObjectField(VM& vm, JSCell* cell, UniquedStringImpl* name, JSValue value)
{
    const LayoutData* layout = value.isObject() ? nullptr : cell->structure()->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return false;
    auto* object = uncheckedDowncast<Object>(cell);
    State& state = object->state();
    if (object->hasFlagsInOrderWord() || state.data.isCompiledForAudit)
        return false;
    const FieldData* field = object->fieldNamed(state, name);
    return field && !object->isDiscriminant(*field) && object->readInline(*field) && object->write(vm, *field, value);
}

std::optional<bool> compiledObjectHasOwnProperty(JSObject* object, UniquedStringImpl* name)
{
    const LayoutData* layout = object->structure()->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return std::nullopt;
    auto* typedObject = uncheckedDowncast<Object>(object);
    if (const FieldData* field = typedObject->fieldNamed(typedObject->state(), name))
        return typedObject->readInline(*field) || typedObject->read(*field);
    if (typedObject->extraProperties())
        return std::nullopt;
    return false;
}

bool forEachCompiledObjectOwnProperty(JSObject* object, const ScopedLambda<bool(UniquedStringImpl&, JSValue)>& functor)
{
    const LayoutData* layout = object->structure()->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return false;
    auto* typedObject = uncheckedDowncast<Object>(object);
    if (typedObject->forEachFieldAsProperty(functor, Object::SymbolKeys::Include))
        return true;
    if (typedObject->state().hasFieldsWithSymbolKeys(*layout))
        return false;
    bool goesOn = true;
    bool isWalked = typedObject->forEachFieldAndExtraProperty([&](UniquedStringImpl& name, JSValue value) {
        goesOn = functor(name, value);
        return goesOn;
    });
    if (!isWalked || !goesOn)
        return isWalked;
    JSObject* extra = typedObject->extraProperties();
    if (!extra)
        return true;
    extra->structure()->forEachProperty(object->vm(), [&](const auto& entry) -> bool {
        if (entry.attributes() & PropertyAttribute::DontEnum || !entry.key()->isSymbol() || PropertyName(entry.key()).isPrivateName())
            return true;
        return functor(*entry.key(), extra->getDirect(entry.offset()));
    });
    return true;
}

bool belongsToBuiltinModule(JSCell* cell)
{
    auto* closure = dynamicDowncast<Closure>(cell);
    return closure && closure->isDeclaredAs(functionIsInBuiltinModule);
}

void didChangePrototypeProperty(VM& vm, JSCell* function)
{
    if (auto* closure = dynamicDowncast<Closure>(function))
        closure->structure()->aotInstance()->typed->didChangePrototypeOf(vm, closure);
}

void State::setExpectedRowType(VM& vm, JSObject* query, uint32_t type)
{
    if (!m_expectedRowTypeSymbol)
        m_expectedRowTypeSymbol = PrivateSymbolImpl::create(*String("expectedRowType"_s).impl());
    query->putDirect(vm, PropertyName(m_expectedRowTypeSymbol.get()), jsNumber(type + 1));
}

uint32_t State::expectedRowType(VM& vm, JSObject* query)
{
    if (!m_expectedRowTypeSymbol)
        return 0;
    JSValue expected = query->getDirect(vm, PropertyName(m_expectedRowTypeSymbol.get()));
    return expected && expected.isInt32() ? expected.asInt32() : 0;
}

auto State::createRowDecoder(VM& vm, JSGlobalObject* globalObject, const RuntimeType& type, std::span<const RowColumn> columns) -> std::unique_ptr<RowDecoder>
{
    using Step = RowDecoder::Step;
    if (data.isCompiledForAudit || isCounting() || type.allowsAnyObject() || type.numberOfSupertypes() != 1 || !type.view(0).supertypeShape || !hasLayoutForShape(type.view(0).supertypeShape))
        return nullptr;
    const LayoutData& layout = layoutForShape(type.view(0).supertypeShape);
    if (layout.kind != LayoutKind::Record || layout.isAbstract || layout.literalsWithoutFieldOffset() || layout.numberOfFields > 64 || columns.size() > maxFieldsInOrder)
        return nullptr;
    auto decoder = makeUnique<RowDecoder>();
    CoercionPlan stores;
    Vector<uint16_t, 16> order;
    uint64_t isPresent = 0;
    JSObject* sampleDate = nullptr;
    for (auto& column : columns) {
        std::span columnName { column.name, column.nameLength };
        RefPtr name = charactersAreAllASCII(columnName) ? AtomStringImpl::add(byteCast<Latin1Character>(columnName)) : nullptr;
        const FieldData* field = name ? this->field(vm, layout, name.get()) : nullptr;
        if (!field || field->flags & (fieldIsOutOfLine | fieldIsPinned | fieldHoldsLazyClosure | fieldMayExistWithoutStorage | fieldIsPlaceholder) || isPresent >> (field - layout.fields()) & 1)
            return nullptr;
        isPresent |= 1ull << (field - layout.fields());
        order.append(field - layout.fields());
        const RuntimeType& fieldType = ownTypeOf(*field);
        Store store = field->store();
        uint8_t kinds = kindsAcceptedBy(fieldType);
        bool storesCell32 = store == Store::Cell32 || store == Store::OptionalCell32;
        bool storesValue = store == Store::Cell || store == Store::JSValue;
        bool storesDouble = store == Store::Double || store == Store::OptionalDouble;
        Step step = Step::General;
        size_t firstLiteral = 0;
        switch (column.format) {
        case WireFormat::Int32:
            if (kinds & CoercionPlan::acceptsInt32 && !field->numberOfLiterals())
                step = store == Store::Int32OrNumberBox ? Step::InlineInt32 : store == Store::Int32 ? Step::Int32AsInt32 : storesDouble ? Step::Int32AsDouble : store == Store::JSValue ? Step::Int32AsValue : Step::General;
            break;
        case WireFormat::Float32:
        case WireFormat::Float64:
            if (kinds & CoercionPlan::acceptsInt32 && kinds & CoercionPlan::acceptsDouble && !field->numberOfLiterals() && storesDouble)
                step = column.format == WireFormat::Float32 ? Step::Float32AsDouble : Step::Float64AsDouble;
            break;
        case WireFormat::Boolean:
            if (kinds & CoercionPlan::acceptsTrue && kinds & CoercionPlan::acceptsFalse)
                step = store == Store::Boolean ? Step::BooleanAsByte : store == Store::OptionalBoolean ? Step::BooleanAsLowByteOfValue : Step::General;
            break;
        case WireFormat::Text:
            if (kinds & CoercionPlan::acceptsString && !field->numberOfLiterals())
                step = storesCell32 ? Step::TextAsCell32 : storesValue ? Step::TextAsValue : Step::General;
            else if (kinds & CoercionPlan::acceptsString && store != Store::Int32OrNumberBox) {
                std::span allowed { data.at<LiteralData>(field->literalsOffset()), field->numberOfLiterals() };
                firstLiteral = decoder->literals.size();
                for (auto& literal : allowed) {
                    uint64_t bits = 0;
                    if (literal.isNumber() || !(data.stringFacts(literal.string()) & Data::stringIsASCII) || !storeAs(*this, store, &bits, string(vm, literal.string())))
                        break;
                    decoder->literals.append({ data.string(literal.string()), bits });
                }
                if (decoder->literals.size() - firstLiteral == allowed.size())
                    step = Step::TextAsLiteral;
                else
                    decoder->literals.shrink(firstLiteral);
            }
            break;
        case WireFormat::TimestampMicroseconds:
            if (!sampleDate)
                sampleDate = DateInstance::create(vm, globalObject->dateStructure(), 0);
            if (matchesTypedSubtype(fieldType, sampleDate))
                step = storesCell32 ? Step::DateAsCell32 : storesValue ? Step::DateAsValue : Step::General;
            break;
        }
        uint64_t nullBits = 0;
        uint8_t nullWidth = 0;
        if (fits(fieldType, jsNull())) {
            if (store == Store::Int32OrNumberBox) {
                nullBits = static_cast<uint32_t>(nullInt32);
                nullWidth = sizeof(int32_t);
            } else if (storeAs(*this, store, &nullBits, jsNull()))
                nullWidth = widthOf(store);
        }
        decoder->columns.append({ field, nullBits, field->offset, column.format, step, nullWidth, kinds });
        if (step == Step::TextAsLiteral) {
            decoder->columns.last().literalWidth = widthOf(store);
            decoder->columns.last().firstLiteral = firstLiteral;
            decoder->columns.last().literalCount = decoder->literals.size() - firstLiteral;
        }
    }
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        if (!(layout.fields()[i].flags & fieldIsOptional) && !(isPresent >> i & 1))
            return nullptr;
    }
    finishCoercionPlan(stores, layout, orderStartingWith(layout, order.span()));
    if (stores.initialBody.isEmpty())
        return nullptr;
    decoder->layout = &layout;
    decoder->initialBody = WTF::move(stores.initialBody);
    return decoder;
}

bool State::tryConvertRow(VM& vm, JSObject* row, const RuntimeType& type)
{
    return row->type() == FinalObjectType && !data.isCompiledForAudit && !isCounting() && (tryQuickConversion(vm, row, type) || tryConvertInPlaceByPlans(vm, row, type));
}

Object* State::tryDecodeRow(VM& vm, JSGlobalObject* globalObject, const RowDecoder& decoder, std::span<const uint8_t> row)
{
    using Step = RowDecoder::Step;
    static constexpr int64_t unixMillisecondsAtYear2000 = 946684800000;
    size_t bodySize = decoder.initialBody.size();
    alignas(sizeof(EncodedJSValue)) uint8_t cellBytes[CoercionPlan::maxInPlaceConversionSize];
    memcpySpan(std::span { cellBytes + sizeof(JSCell), bodySize }, decoder.initialBody.span());
    const uint8_t* cursor = row.data();
    const uint8_t* end = cursor + row.size();
    auto take = [&]<typename T>(T) {
        T bits;
        memcpy(&bits, cursor, sizeof(T));
        cursor += sizeof(T);
        return flipBytes(bits);
    };
    auto takeText = [&](uint32_t length) -> JSString* {
        std::span text { cursor, length };
        cursor += length;
        if (charactersAreAllASCII(text))
            return jsString(vm, String(byteCast<Latin1Character>(text)));
        String decoded = String::fromUTF8(text);
        return decoded.isNull() ? nullptr : jsString(vm, WTF::move(decoded));
    };
    auto takeDate = [&]() -> JSObject* {
        int64_t microseconds = static_cast<int64_t>(take(uint64_t()));
        if (microseconds == INT64_MAX || microseconds == INT64_MIN)
            return nullptr;
        int64_t milliseconds = microseconds / 1000 - (microseconds % 1000 < 0);
        return DateInstance::create(vm, globalObject->dateStructure(), static_cast<double>(milliseconds + unixMillisecondsAtYear2000));
    };
    if (row.size() < sizeof(uint16_t) || take(uint16_t()) != decoder.columns.size())
        return nullptr;
    DeferGC deferGC(vm);
    for (auto& column : decoder.columns) {
        if (static_cast<size_t>(end - cursor) < sizeof(uint32_t))
            return nullptr;
        uint32_t length = take(uint32_t());
        uint8_t* slot = cellBytes + column.offset;
        auto keep = [&](auto bits) {
            memcpy(slot, &bits, sizeof(bits));
        };
        if (length == UINT32_MAX) {
            if (!column.nullWidth)
                return nullptr;
            memcpy(slot, &column.nullBits, column.nullWidth);
            continue;
        }
        static constexpr uint8_t fixedLengths[] = { 4, 4, 8, 1, 0, 8 };
        if (static_cast<size_t>(end - cursor) < length || (column.format != WireFormat::Text && length != fixedLengths[static_cast<unsigned>(column.format)]))
            return nullptr;
        switch (column.step) {
        case Step::InlineInt32: {
            int32_t number = static_cast<int32_t>(take(uint32_t()));
            if (number < minInlineInt32)
                return nullptr;
            keep(number);
            continue;
        }
        case Step::Int32AsInt32:
            keep(take(uint32_t()));
            continue;
        case Step::Int32AsDouble:
            keep(static_cast<double>(static_cast<int32_t>(take(uint32_t()))));
            continue;
        case Step::Int32AsValue:
            keep(JSValue::encode(jsNumber(static_cast<int32_t>(take(uint32_t())))));
            continue;
        case Step::Float32AsDouble:
            keep(purifyNaN(static_cast<double>(std::bit_cast<float>(take(uint32_t())))));
            continue;
        case Step::Float64AsDouble:
            keep(purifyNaN(std::bit_cast<double>(take(uint64_t()))));
            continue;
        case Step::BooleanAsByte:
            keep(static_cast<uint8_t>(*cursor++ == 1));
            continue;
        case Step::BooleanAsLowByteOfValue:
            keep(static_cast<uint8_t>(JSValue::encode(jsBoolean(*cursor++ == 1))));
            continue;
        case Step::TextAsCell32:
        case Step::TextAsValue: {
            JSString* text = takeText(length);
            if (!text)
                return nullptr;
            if (column.step == Step::TextAsCell32)
                keep(MarkedBlockCage::encode(text));
            else
                keep(JSValue::encode(text));
            continue;
        }
        case Step::TextAsLiteral: {
            std::span text { cursor, length };
            cursor += length;
            auto allowed = decoder.literals.span().subspan(column.firstLiteral, column.literalCount);
            auto found = std::ranges::find_if(allowed, [&](auto& literal) { return equalSpans(literal.text, text); });
            if (found == allowed.end())
                return nullptr;
            memcpy(slot, &found->bits, column.literalWidth);
            countOperationNamed(&instance, "RowDecoder::literalText");
            continue;
        }
        case Step::DateAsCell32:
        case Step::DateAsValue: {
            JSObject* date = takeDate();
            if (!date)
                return nullptr;
            if (column.step == Step::DateAsCell32)
                keep(MarkedBlockCage::encode(date));
            else
                keep(JSValue::encode(date));
            continue;
        }
        case Step::General:
            break;
        }
        JSValue value;
        switch (column.format) {
        case WireFormat::Int32:
            value = jsNumber(static_cast<int32_t>(take(uint32_t())));
            break;
        case WireFormat::Float32:
            value = jsNumber(purifyNaN(static_cast<double>(std::bit_cast<float>(take(uint32_t())))));
            break;
        case WireFormat::Float64:
            value = jsNumber(purifyNaN(std::bit_cast<double>(take(uint64_t()))));
            break;
        case WireFormat::Boolean:
            value = jsBoolean(*cursor++ == 1);
            break;
        case WireFormat::TimestampMicroseconds:
            value = takeDate();
            break;
        case WireFormat::Text:
            value = takeText(length);
            break;
        }
        if (!value)
            return nullptr;
        const FieldData& field = *column.field;
        if (!(column.acceptedKinds & CoercionPlan::kindOf(value)) && (value.isObject() ? !matchesTypedSubtype(ownTypeOf(field), asObject(value)) : !fits(ownTypeOf(field), value)))
            return nullptr;
        if (field.numberOfLiterals() && !isAllowedLiteral(field, value))
            return nullptr;
        if (field.store() != Store::Int32OrNumberBox) {
            if (!storeAs(*this, field.store(), slot, value))
                return nullptr;
            continue;
        }
        if (!value.isInt32() || value.asInt32() < minInlineInt32)
            return nullptr;
        keep(value.asInt32());
    }
    if (cursor != end)
        return nullptr;
    return Object::createWithBody(vm, structureFor(vm, decoder.layout->shape, globalObject->objectPrototype()), std::span { cellBytes + sizeof(JSCell), bodySize });
}

} // namespace Typed

String typedClosureName(const JSFunction* closure)
{
    const Typed::Data& data = Typed::dataInImage();
    return Typed::constantString(data, data.at<uint32_t>(data.functionsOffset)[uncheckedDowncast<Typed::Closure>(closure)->functionIndex()]);
}

unsigned typedClosureLength(const JSFunction* closure)
{
    const Typed::Data& data = Typed::dataInImage();
    return data.functionLengthOf(uncheckedDowncast<Typed::Closure>(closure)->functionIndex());
}

uint64_t typedFunctionClosureWord(std::span<const uint8_t> image, uint32_t function, uint64_t relative)
{
    auto& header = *reinterpret_cast<const ImageHeader*>(image.data());
    RELEASE_ASSERT(header.typedDataSize);
    return Typed::closureWord(*reinterpret_cast<const Typed::Data*>(image.subspan(header.typedDataOffset).data()), function, relative);
}

ClosureWordRange closureWordRangeIn(std::span<const uint8_t> image)
{
    auto& header = *reinterpret_cast<const ImageHeader*>(image.data());
    RELEASE_ASSERT(header.typedDataSize);
    auto& data = *reinterpret_cast<const Typed::Data*>(image.subspan(header.typedDataOffset).data());
    return { data.firstClosureFunction, data.endOfClosureFunctions, data.omittedClosureWordCount };
}

static Typed::BuiltinID builtinEntryOfTypedFunction(uint32_t function)
{
    const Typed::Data& data = Typed::dataInImage();
    auto* functions = data.at<uint32_t>(data.preludeFunctionsOffset);
    for (unsigned which = 0; which < Typed::numberOfPreludeFunctions; ++which) {
        if (functions[which] == function)
            return Typed::Builtins::singleton().entryImplementedBy(static_cast<Typed::PreludeFunction>(which));
    }
    return { };
}

StackTraceBuiltinKind builtinKindOfTypedFunction(uint32_t function)
{
    Typed::BuiltinID id = builtinEntryOfTypedFunction(function);
    if (!id)
        return StackTraceBuiltinKind::None;
    auto& builtins = Typed::Builtins::singleton();
    if (builtins.isHostFunctionInEngine(id))
        return StackTraceBuiltinKind::HostFunction;
    return builtins.at(id).kind == Typed::BuiltinKind::Constructor ? StackTraceBuiltinKind::JSBuiltinConstructor : StackTraceBuiltinKind::JSBuiltin;
}

String typedFunctionName(uint32_t function, bool asWritten)
{
    const Typed::Data& data = Typed::dataInImage();
    RELEASE_ASSERT(function < data.numberOfFunctions);
    if (!asWritten) {
        if (String reported = Typed::nameReportedForFrames(data, function); !reported.isNull()) [[unlikely]]
            return reported;
    }
    if (static_cast<Typed::Format::FunctionKind>(Typed::kindByteOf(function) & Typed::functionKindMask) == Typed::Format::FunctionKind::Module)
        return emptyString();
    String name = Typed::constantString(data, data.at<uint32_t>(data.functionsOffset)[function]);
    bool isMethod = static_cast<Typed::Format::FunctionKind>(Typed::kindByteOf(function) & Typed::functionKindMask) == Typed::Format::FunctionKind::Method;
    if (isMethod && (name.startsWith("get "_s) || name.startsWith("set "_s)))
        return name.substring(4);
    return name;
}

String typedFunctionName(VM& vm, uint32_t function, bool asWritten)
{
    const Typed::Data& data = Typed::dataInImage();
    RELEASE_ASSERT(function < data.numberOfFunctions);
    if (asWritten)
        return typedFunctionName(function, true);
    if (String reported = Typed::nameReportedForFrames(data, function); !reported.isNull()) [[unlikely]]
        return reported;
    if (String displayName = Typed::singleClosureDisplayName(vm, data, function); !displayName.isNull()) [[unlikely]]
        return displayName;
    return typedFunctionName(function, true);
}

CodeType typedFunctionCodeType(uint32_t function)
{
    uint8_t kindByte = Typed::kindByteOf(function);
    return static_cast<Typed::Format::FunctionKind>(kindByte & Typed::functionKindMask) == Typed::Format::FunctionKind::Module && !(kindByte & Typed::moduleIsCommonJS) ? ModuleCode : FunctionCode;
}

bool wasEnteredByConstruct(const Instance* instance, const CallFrame* previousTopCallFrame)
{
    if (!instance->typed)
        return false;
    for (auto* construction = instance->typed->constructionsFromNative; construction && construction->topCallFrame <= previousTopCallFrame; construction = construction->previous) {
        if (construction->topCallFrame == previousTopCallFrame)
            return true;
    }
    return false;
}

bool typedFunctionIsConstructor(uint32_t function)
{
    return static_cast<Typed::Format::FunctionKind>(Typed::kindByteOf(function) & Typed::functionKindMask) == Typed::Format::FunctionKind::Constructor;
}

bool isCompiledClassObject(JSFunction* function)
{
    return function->classInfo() == Typed::Closure::info() && uncheckedDowncast<Typed::Closure>(function)->kind() == Typed::Format::FunctionKind::Constructor;
}

bool typedFunctionHasGivenName(uint32_t function)
{
    return (Typed::kindByteOf(function) & Typed::functionKindMask) == Typed::resumeFunctionNamedByOwnerKind;
}

bool typedFunctionIsHiddenInStackTraces(uint32_t function)
{
    return Typed::kindByteOf(function) & Typed::functionIsHiddenInStackTraces;
}

bool typedFunctionWraps(uint32_t wrapper, uint32_t function)
{
    return typedFunctionIsHiddenInStackTraces(wrapper) && Typed::functionOwner(Typed::dataInImage(), wrapper) == Typed::functionOwner(Typed::dataInImage(), function);
}

bool typedFunctionIsSynthetic(uint32_t function)
{
    return (Typed::kindByteOf(function) & Typed::functionKindMask) == Typed::sharedBodyFunctionKind;
}

Vector<uint8_t> imageSymbols(std::span<const uint8_t> bytes)
{
    auto sizeOfImage = aotImageSize(bytes);
    auto codeRange = aotImageCodeRange(bytes);
    if (!sizeOfImage || !codeRange || !codeRange->second || *sizeOfImage >= bytes.size())
        return { };
    auto& header = *reinterpret_cast<const ImageHeader*>(bytes.data());
    const ProgramData* program = ProgramData::tryUse(bytes.subspan(*sizeOfImage));
    if (!program || !header.typedDataSize)
        return { };
    const Typed::Data& data = *reinterpret_cast<const Typed::Data*>(bytes.data() + header.typedDataOffset);

    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> methodClass;
    for (uint32_t shape = 1; shape < data.numberOfLayouts; ++shape) {
        if (!data.at<uint32_t>(data.layoutsOffset)[shape])
            continue;
        const Typed::LayoutData& layout = *data.at<Typed::LayoutData>(data.at<uint32_t>(data.layoutsOffset)[shape]);
        if (!layout.isClass())
            continue;
        for (auto methods : { layout.methods(), layout.staticMethods() }) {
            for (auto& method : methods)
                methodClass.add(Typed::functionOwner(data, method.function), layout.name);
        }
    }

    struct Row {
        uint32_t offset;
        uint32_t size;
        uint32_t name;
    };
    Vector<Row> rows;
    Vector<uint8_t> names;
    UncheckedKeyHashMap<String, uint32_t> nameIndices;
    auto placeOf = [&](const String& name) {
        return nameIndices.ensure(name, [&] {
            uint32_t place = safeCast<uint32_t>(names.size());
            names.append(byteCast<uint8_t>(name.utf8().span()));
            names.append(0);
            return place;
        }).iterator->value;
    };
    const uint32_t* starts = reinterpret_cast<const uint32_t*>(bytes.data() + header.functionStartsOffset);
    for (uint32_t index = 0; index < header.numberOfFunctions; ++index) {
        uint32_t end = index + 1 < header.numberOfFunctions ? starts[index + 1] : header.functionsEnd;
        for (unsigned i = 0; i < header.numberOfStubCopies; ++i) {
            if (header.stubCopies[i] >= starts[index])
                end = std::min(end, header.stubCopies[i]);
        }
        if (end <= starts[index])
            continue;
        if (!program->infos()[index].isTyped) {
            rows.append({ starts[index], end - starts[index], placeOf("(compiled code)"_s) });
            continue;
        }
        uint32_t function = program->functionIndexAndFlags()[index] >> 3;
        auto kind = static_cast<Typed::Format::FunctionKind>(data.at<uint8_t>(data.functionKindsOffset)[function] & Typed::functionKindMask);
        String name = kind == Typed::Format::FunctionKind::Module ? String("(module)"_s) : Typed::constantString(data, data.at<uint32_t>(data.functionsOffset)[function]);
        if (name.isEmpty())
            name = "(anonymous)"_s;
        if (auto found = methodClass.find(Typed::functionOwner(data, function)); found != methodClass.end() && data.stringSize(found->value))
            name = makeString(Typed::constantString(data, found->value), '.', name);
        if (const uint8_t* at = kind == Typed::Format::FunctionKind::Module ? program->typedFunctionPositions(index) : nullptr) {
            auto readVarint = [&] {
                uint64_t value = 0;
                for (unsigned shift = 0;; shift += 7) {
                    uint8_t byte = *at++;
                    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
                    if (!(byte & 0x80))
                        return value;
                }
            };
            for (unsigned i = 0; i < 3; ++i)
                readVarint();
            if (uint64_t source = readVarint(); source && source <= program->numberOfSources)
                name = makeString(name, ' ', program->sourceName(static_cast<uint32_t>(source)));
        } else if (data.at<uint8_t>(data.functionKindsOffset)[function] & Typed::functionIsHiddenInStackTraces)
            name = makeString(name, " (entry)"_s);
        rows.append({ starts[index], end - starts[index], placeOf(name) });
    }
    for (unsigned i = 0; i < header.numberOfStubCopies; ++i)
        rows.append({ header.stubCopies[i], header.stubsSize, placeOf("(stubs of compiled code)"_s) });
    std::ranges::sort(rows, { }, &Row::offset);
    for (size_t i = 0; i < rows.size(); ++i)
        rows[i].size = (i + 1 < rows.size() ? rows[i + 1].offset : header.functionsEnd) - rows[i].offset;

    Vector<uint8_t> result;
    auto appendWord = [&](uint32_t word) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            result.append(static_cast<uint8_t>(word >> shift));
    };
    result.reserveInitialCapacity(16 + rows.size() * sizeof(Row) + names.size());
    appendWord(safeCast<uint32_t>(rows.size()));
    appendWord(safeCast<uint32_t>(names.size()));
    appendWord(safeCast<uint32_t>(codeRange->first));
    appendWord(safeCast<uint32_t>(codeRange->second));
    for (auto& row : rows) {
        appendWord(row.offset);
        appendWord(row.size);
        appendWord(row.name);
    }
    result.appendVector(names);
    return result;
}

std::optional<FunctionRef::ReportedPosition> typedFunctionDefinition(uint32_t function)
{
    const ProgramData* program = ProgramData::get();
    Image* image = Image::withCode();
    if (!program || !image)
        return std::nullopt;
    using Compiled = std::pair<uint32_t, uint32_t>;
    static LazyNeverDestroyed<Vector<Compiled>> compiled;
    static std::once_flag once;
    std::call_once(once, [&] {
        compiled.construct();
        for (uint32_t index = 0; index < program->numberOfFunctions; ++index) {
            if (auto& info = program->infos()[index]; info.isTyped)
                compiled->append({ info.typedFunction(), index });
        }
        std::ranges::sort(compiled.get());
    });
    for (uint32_t asked : { function, Typed::functionOwner(Typed::dataInImage(), function) }) {
        auto found = std::ranges::lower_bound(compiled.get(), asked, { }, &Compiled::first);
        if (found != compiled->end() && found->first == asked)
            return FunctionRef { nullptr, found->second }.reportedPositionFor(BytecodeIndex(0));
    }
    return std::nullopt;
}

uint32_t typedFunctionWithInnerEntryAt(const void* pc)
{
    if (Options::aotProfileBreakForTesting() && StringView::fromLatin1(byteCast<char>(Options::aotProfileBreakForTesting())) == "guest"_s) [[unlikely]]
        return FunctionRef::none;
    ImageAddressInfo asked = classifyAddress(pc);
    const ProgramData* program = ProgramData::get();
    if (asked.kind != ImageAddressInfo::Function || !asked.offset || !program)
        return FunctionRef::none;
    using Entry = std::pair<uint32_t, uint32_t>;
    static LazyNeverDestroyed<Vector<Entry>> entries;
    static std::once_flag once;
    std::call_once(once, [&] {
        entries.construct();
        auto* code = static_cast<const uint8_t*>(Image::withCode()->code());
        for (uint32_t function = program->firstClosureFunction; function - program->omittedClosureWordCount < program->numberOfTypedEntries; ++function) {
            uint64_t word = program->closureWords()[function];
            if (!word)
                continue;
            uint64_t offset = (word & ClosureWord::offsetMask) * ClosureWord::offsetUnit;
            if (ImageAddressInfo entry = classifyAddress(code + offset); entry.kind == ImageAddressInfo::Function && entry.offset)
                entries->append({ safeCast<uint32_t>(offset), function });
        }
        std::ranges::sort(entries.get());
    });
    uint32_t offset = *imageCodeOffset(pc);
    auto behind = std::ranges::upper_bound(entries.get(), offset, { }, &Entry::first);
    if (behind == entries->begin())
        return FunctionRef::none;
    const Entry& entry = *std::prev(behind);
    return entry.first > offset - asked.offset ? entry.second : FunctionRef::none;
}

bool typedFunctionRejectsOnThrow(uint32_t function)
{
    constexpr uint8_t relevantFlags = Typed::functionIsAsync | Typed::functionIsGenerator | Typed::functionIsHiddenInStackTraces;
    if ((Typed::kindByteOf(function) & relevantFlags) == (Typed::functionIsAsync | Typed::functionIsHiddenInStackTraces))
        return true;
    return Typed::AsyncGenerator::bodyCapturesExceptions(function);
}

void startRecordingThrownTypeErrors()
{
    Typed::ViolationLog::s_isRetainedForHost = true;
}

String recordedThrownTypeErrors()
{
    Typed::ViolationLog* log = Typed::ViolationLog::singletonIfEnabled(false);
    return log ? log->throwLocations() : String();
}

String slimClosureName(JSObject* closure)
{
    return uncheckedDowncast<Typed::SlimClosure>(closure)->nameOfFunction();
}

String closureDeclaredName(JSObject* object)
{
    uint32_t function;
    if (auto* slim = dynamicDowncast<Typed::SlimClosure>(object))
        function = slim->functionIndex();
    else if (auto* closure = dynamicDowncast<Typed::Closure>(object))
        function = closure->functionIndex();
    else
        return String();
    const Typed::Data& data = Typed::dataInImage();
    if (data.at<uint8_t>(data.givenNamesOffset)[function / 8] >> function % 8 & 1)
        return emptyString();
    String name = Typed::constantString(data, data.at<uint32_t>(data.functionsOffset)[function]);
    if (static_cast<Typed::Format::FunctionKind>(Typed::kindByteOf(function) & Typed::functionKindMask) == Typed::Format::FunctionKind::Bound)
        return name.substring("bound "_s.length());
    return name;
}

String closureDisplayName(JSObject* object)
{
    uint32_t function;
    if (auto* slim = dynamicDowncast<Typed::SlimClosure>(object))
        function = slim->functionIndex();
    else if (auto* closure = dynamicDowncast<Typed::Closure>(object))
        function = closure->functionIndex();
    else
        return String();
    const Typed::Data& data = Typed::dataInImage();
    String name = Typed::constantString(data, data.at<uint32_t>(data.functionsOffset)[function]);
    if (static_cast<Typed::Format::FunctionKind>(Typed::kindByteOf(function) & Typed::functionKindMask) == Typed::Format::FunctionKind::Bound)
        return name.substring("bound "_s.length());
    if (name.startsWith('[') && data.at<uint8_t>(data.givenNamesOffset)[function / 8] >> function % 8 & 1)
        return emptyString();
    return name;
}

bool isSloppyPlainClosure(JSCell* cell)
{
    uint32_t function;
    if (auto* slim = dynamicDowncast<Typed::SlimClosure>(cell))
        function = slim->functionIndex();
    else if (auto* closure = dynamicDowncast<Typed::Closure>(cell))
        function = closure->functionIndex();
    else
        return false;
    const Typed::Data& data = Typed::dataInImage();
    return std::ranges::binary_search(std::span { data.at<uint32_t>(data.sloppyPlainFunctionsOffset), data.numberOfSloppyPlainFunctions }, function);
}

unsigned slimClosureLength(JSObject* closure)
{
    const Typed::Data& data = Typed::dataInImage();
    return data.functionLengthOf(uncheckedDowncast<Typed::SlimClosure>(closure)->functionIndex());
}

JSValue slimClosureSourceText(JSGlobalObject* globalObject, JSObject* closure)
{
    return uncheckedDowncast<Typed::SlimClosure>(closure)->toString(globalObject);
}

bool isTypedFunctionClosure(JSCell* cell, uint32_t function)
{
    const Typed::Data& data = Typed::dataInImage();
    if (auto* slim = dynamicDowncast<Typed::SlimClosure>(cell))
        return Typed::functionOwner(data, slim->functionIndex()) == Typed::functionOwner(data, function);
    auto* closure = dynamicDowncast<Typed::Closure>(cell);
    if (!closure) {
        Typed::BuiltinID id = builtinEntryOfTypedFunction(function);
        JSObject* object = id ? cell->getObject() : nullptr;
        auto& builtins = Typed::Builtins::singleton();
        return object && builtins.findCell(object->realm(), builtins.hasCell(id) ? builtins.cellOf(id) : builtins.cellOf(builtins.at(id).nameName)) == cell;
    }
    return Typed::functionOwner(data, closure->functionIndex()) == Typed::functionOwner(data, function);
}

} // namespace AOT

JSValue JSObject::getDirectFromCompiledObject(VM& vm, PropertyName name, unsigned* attributes) const
{
    if (!structure()->compiledLayout())
        return { };
    return uncheckedDowncast<AOT::Typed::Object>(const_cast<JSObject*>(this))->getDirectFromNativeCode(vm, name, attributes);
}

std::optional<bool> JSObject::getOwnPropertySlotFromCompiledObject(PropertyName name, PropertySlot& slot)
{
    const AOT::Typed::LayoutData* layout = structure()->compiledLayout();
    if (!layout || !AOT::Typed::isObjectLayout(layout->kind))
        return std::nullopt;
    auto* object = uncheckedDowncast<AOT::Typed::Object>(this);
    if (const AOT::Typed::FieldData* field = object->fieldNamed(object->state(), name)) {
        JSValue value = object->readInline(*field);
        if (!value) [[unlikely]]
            value = object->read(*field);
        if (!value)
            return false;
        slot.setValue(object, object->attributesOf(*field), value);
        return true;
    }
    if (object->hasExtraProperties())
        return std::nullopt;
    return false;
}

bool JSObject::reifyIfSlimClosure(VM& vm)
{
    if (type() != SlimClosureType)
        return false;
    uncheckedDowncast<AOT::Typed::SlimClosure>(this)->reifyProperties(vm);
    return true;
}

void compiledObjectDidBecomePrototype(JSObject*);

void compiledObjectDidBecomePrototype(JSObject* object)
{
    for (JSValue next = object; next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        const auto* layout = asObject(next)->structure()->compiledLayout();
        if (layout && AOT::Typed::isObjectLayout(layout->kind))
            asObject(next)->structure()->aotInstance()->typed->didBecomePrototype(*layout);
    }
}

bool compiledObjectHasIndexedProperties(JSObject*);

bool compiledObjectHasIndexedProperties(JSObject* object)
{
    JSObject* extra = uncheckedDowncast<AOT::Typed::Object>(object)->extraProperties();
    return extra && hasIndexedProperties(extra->indexingType());
}

bool isWritableWithoutLock(VM&, UniquedStringImpl*, JSObject*, JSValue);

bool isWritableWithoutLock(VM& vm, UniquedStringImpl* name, JSObject* holder, JSValue found)
{
    if (name->isSymbol())
        return found.isCallable() && name != vm.propertyNames->hasInstanceSymbol.impl() && name != vm.propertyNames->toPrimitiveSymbol.impl();
    if (found.isObject())
        return name != vm.propertyNames->prototype.impl();
    if (found.isString())
        return !holder->isCallable();
    return found.isNumber() && name == vm.propertyNames->length.impl() && dynamicDowncast<JSArray>(holder);
}

bool JSObject::isRejectedOnlyByLock(VM& vm, UniquedStringImpl* name, JSValue& held)
{
    held = { };
    if (vm.m_aotInstances.isEmpty() ? !vm.useImmutableIntrinsics : !vm.m_aotInstances[0]->typed)
        return false;
    if (auto* proxy = dynamicDowncast<JSGlobalProxy>(this))
        return proxy->target()->isRejectedOnlyByLock(vm, name, held);
    if (isGlobalObject()) {
        if (!vm.useImmutableIntrinsics || !name)
            return false;
        unsigned bindingAttributes;
        PropertyOffset offset = structure()->get(vm, name, bindingAttributes);
        if (isValidOffset(offset)) {
            held = getDirect(offset);
            return held && held.isObject() && !asObject(held)->isGlobalObject() && asObject(held)->isLockedBuiltin();
        }
        return structure()->hasNonReifiedStaticProperties() && structure()->findPropertyHashEntry(name);
    }
    if (!structure()->hasImmutableProperties() && !structure()->inheritorsMayOverrideReadOnlyProperties())
        return false;
    if (!name)
        return true;
    if (name == vm.propertyNames->prototype.impl() && isCallable() && this != globalObject()->functionPrototype())
        return false;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertySlot slot(this, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isFound = getPropertySlot(globalObject(), PropertyName(name), slot);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return false;
    }
    if (!isFound)
        return true;
    if (structure()->inheritorsMayOverrideReadOnlyProperties() && !structure()->inheritorsMayOverride(vm, name))
        return false;
    if (!slot.isValue()) {
        if (!slot.isAccessor() || name != vm.propertyNames->underscoreProto.impl())
            return false;
        held = getPrototypeDirect();
        return true;
    }
    JSValue found = slot.getValue(globalObject(), PropertyName(name));
    if (slot.slotBase() == this)
        held = found;
    return isWritableWithoutLock(vm, name, slot.slotBase(), found);
}

static bool isProgramClass(JSObject* function)
{
    if (dynamicDowncast<AOT::Typed::Closure>(function))
        return true;
    auto* ordinary = dynamicDowncast<JSFunction>(function);
    return ordinary && !ordinary->isHostOrBuiltinFunction();
}

String describeValueForAOTError(JSValue value)
{
    return AOT::Typed::State::describe(value);
}

bool JSObject::becomeGuarded(VM& vm)
{
    Structure* from = structure();
    if (from->classInfoForCells() != JSFinalObject::info() || from->isDictionary() || from->hasPolyProto() || from->mayBePrototype() || from->hasAnyKindOfGetterSetterProperties() || from->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || from->hasNonConfigurableProperties() || !from->storedPrototype().isObject() || hasIndexedProperties(from->indexingType()) || !from->isStructureExtensible() || from->hasImmutableProperties())
        return false;
    Vector<std::tuple<RefPtr<UniquedStringImpl>, unsigned, PropertyOffset>, 16> properties;
    from->forEachProperty(vm, [&](const auto& entry) {
        properties.append({ entry.key(), entry.attributes(), entry.offset() });
        return true;
    });
    JSGlobalObject* globalObject = from->globalObject();
    Structure* twin = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, asObject(from->storedPrototype()), from->inlineCapacity(), false, nullptr, true);
    for (auto& [name, attributes, offset] : properties) {
        PropertyOffset offsetInAlias = invalidOffset;
        twin = Structure::addPropertyTransition(vm, twin, name.get(), attributes, offsetInAlias);
        if (offsetInAlias != offset || twin->isDictionary())
            return false;
    }
    if (twin->outOfLineCapacity() != from->outOfLineCapacity())
        return false;
    from->didTransitionFromThisStructureWithoutFiringWatchpoint();
    from->fireStructureTransitionWatchpoint(nullptr);
    setStructure(vm, twin);
    return true;
}

void JSObject::lockAsClassPrototype(JSGlobalObject* globalObject, std::span<const Identifier> replaceable, bool constructorMayBeReplaced)
{
    VM& vm = globalObject->vm();
    if (replaceable.empty() && !constructorMayBeReplaced) {
        lockAndMakePropertiesImmutable(vm);
        return;
    }
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertyNameArrayBuilder names(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    getOwnNonIndexPropertyNames(globalObject, names, DontEnumPropertiesMode::Include);
    scope.assertNoException();
    convertToUncacheableDictionary(vm);
    for (auto& name : names) {
        if (name == vm.propertyNames->constructor ? constructorMayBeReplaced : std::ranges::contains(replaceable, name))
            continue;
        unsigned attributes = 0;
        if (!isValidOffset(structure()->get(vm, name, attributes)))
            continue;
        PropertyDescriptor descriptor;
        descriptor.setConfigurable(false);
        if (!(attributes & PropertyAttribute::Accessor))
            descriptor.setWritable(false);
        JSObject::defineOwnProperty(this, globalObject, name, descriptor, false);
        scope.assertNoException();
    }
    JSObject::preventExtensions(this, globalObject);
    scope.assertNoException();
    flattenDictionaryObject(vm);
    structure()->setInheritorsMayOverrideReadOnlyProperties(true);
}

bool JSObject::recordIfOnlyLockRejects(VM& vm, UniquedStringImpl* name, bool failsSilently, bool isDelete)
{
    if (auto* proxy = dynamicDowncast<JSGlobalProxy>(this))
        return proxy->target()->recordIfOnlyLockRejects(vm, name, failsSilently, isDelete);
    JSValue held;
    if (!isLockedBuiltin())
        return false;
    bool isRejectedOnlyByLockHere = isRejectedOnlyByLock(vm, name, held) && !(isDelete && name == vm.propertyNames->length.impl() && dynamicDowncast<JSArray>(this));
    if (!isRejectedOnlyByLockHere && isDelete && name && !isGlobalObject() && (vm.m_aotInstances.isEmpty() || vm.m_aotInstances[0]->typed) && (name != vm.propertyNames->prototype.impl() || !isCallable())) {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        PropertySlot slot(this, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isOwn = methodTable()->getOwnPropertySlot(this, globalObject(), PropertyName(name), slot);
        if (scope.exception()) [[unlikely]] {
            scope.clearException();
            return false;
        }
        if (isOwn && name->isSymbol())
            isRejectedOnlyByLockHere = this != globalObject()->functionPrototype() || name != vm.propertyNames->hasInstanceSymbol.impl();
        else if (isOwn && !slot.isValue())
            isRejectedOnlyByLockHere = true;
        else if (isOwn) {
            JSValue found = slot.getValue(globalObject(), PropertyName(name));
            isRejectedOnlyByLockHere = found.isCell() ? !found.isSymbol() : found.isNumber() && isCallable() && name == vm.propertyNames->length.impl();
        }
    }
    if (!isRejectedOnlyByLockHere)
        return false;
    AOT::Typed::State* state = vm.m_aotInstances.isEmpty() ? nullptr : vm.m_aotInstances[0]->typed;
    JSObject* constructor = isCallable() || isGlobalObject() ? nullptr : AOT::Typed::lockedPrototypeConstructor(vm, this);
    bool belongsToProgramClass = isProgramClass(constructor ? constructor : this);
    bool throwsInstead = failsSilently && belongsToProgramClass && (!state || !state->data.logsTypeErrors);
    recordLockViolation(vm, name, { }, failsSilently && !throwsInstead);
    return !failsSilently || throwsInstead;
}

String JSObject::lockViolationMessage(VM& vm, UniquedStringImpl* name, ASCIILiteral otherwise)
{
    if (auto* proxy = dynamicDowncast<JSGlobalProxy>(this))
        return proxy->target()->lockViolationMessage(vm, name, otherwise);
    JSValue held;
    if (!isRejectedOnlyByLock(vm, name, held))
        return otherwise;
    return AOT::lockedOperationErrorMessage(vm, this, AOT::LockedOperation::Change, name);
}

String AOT::lockedOperationErrorMessage(VM& vm, JSObject* object, LockedOperation refused, UniquedStringImpl* name)
{
    if (auto* proxy = dynamicDowncast<JSGlobalProxy>(object))
        object = proxy->target();
    JSObject* constructor = object->isCallable() || object->isGlobalObject() ? nullptr : Typed::lockedPrototypeConstructor(vm, object);
    String why = isProgramClass(constructor ? constructor : object) ? "classes are fixed in a program compiled with --aot"_s : "built-ins are locked in a program compiled with --aot"_s;
    if (Typed::isHostClassPrototype(vm, object))
        why = object->getDirect(vm, vm.propertyNames->constructor) ? makeString(why, ". Use a subclass of "_s, object->classInfo()->className, " or a wrapper function instead"_s) : makeString(why, ". Use a wrapper function instead"_s);
    String holder = Typed::nameOfLockedObject(vm, object);
    if (refused == LockedOperation::ChangePrototype)
        return withAOTErrorNote(makeString("Cannot change the prototype of "_s, holder, ": "_s, why));
    ASCIILiteral what = refused == LockedOperation::Delete ? "Cannot delete "_s : "Cannot change "_s;
    if (!name || PropertyName(name).isPrivateName())
        return withAOTErrorNote(makeString(what, holder, ": "_s, why));
    if (name->isSymbol())
        return withAOTErrorNote(makeString(what, "["_s, StringView(name), "] of "_s, holder, ": "_s, why));
    return withAOTErrorNote(makeString(what, "'"_s, StringView(name), "' of "_s, holder, ": "_s, why));
}

void JSObject::recordLockViolation(VM& vm, UniquedStringImpl* name, ASCIILiteral kind, bool failsSilently)
{
    AOT::Typed::State* state = vm.m_aotInstances.isEmpty() ? nullptr : vm.m_aotInstances[0]->typed;
    if (!state)
        return;
    if (auto* proxy = dynamicDowncast<JSGlobalProxy>(this)) {
        proxy->target()->recordLockViolation(vm, name, kind, failsSilently);
        return;
    }
    if (isGlobalObject()) {
        if (!vm.useImmutableIntrinsics || !name)
            return;
        unsigned bindingAttributes;
        PropertyOffset offset = structure()->get(vm, name, bindingAttributes);
        if (isValidOffset(offset)) {
            JSValue bound = getDirect(offset);
            if (!bound || !bound.isObject() || asObject(bound)->isGlobalObject() || !asObject(bound)->isLockedBuiltin())
                return;
        } else if (!structure()->hasNonReifiedStaticProperties() || !structure()->findPropertyHashEntry(name))
            return;
    }
    if (name && !isGlobalObject()) {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        PropertySlot slot(this, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isFound = getPropertySlot(globalObject(), PropertyName(name), slot);
        if (scope.exception()) [[unlikely]] {
            scope.clearException();
            return;
        }
        if (isFound) {
            if (slot.isAccessor()) {
                if (name != vm.propertyNames->underscoreProto.impl() || this == globalObject()->objectPrototype())
                    return;
            } else if (!slot.isValue() || !isWritableWithoutLock(vm, name, slot.slotBase(), slot.getValue(globalObject(), PropertyName(name))))
                return;
        }
    }
    if (kind.isNull()) {
        JSObject* constructor = AOT::Typed::lockedPrototypeConstructor(vm, this);
        if (isGlobalObject())
            kind = "global-binding-write"_s;
        else if (!constructor)
            kind = mayBePrototype() && !isCallable() ? "generator-prototype-write"_s : "builtin-member-replace"_s;
        else
            kind = dynamicDowncast<AOT::Typed::Closure>(constructor) ? "class-prototype-write"_s : "builtin-prototype-write"_s;
    }
    unsigned attributes;
    bool isOwn = name && !PropertyName(name).isPrivateName() && (isValidOffset(structure()->get(vm, name, attributes)) || (structure()->hasNonReifiedStaticProperties() && structure()->findPropertyHashEntry(name)));
    state->recordViolation(vm, { .kind = kind, .onBuiltin = this, .member = isOwn ? name : nullptr, .action = failsSilently ? AOT::Typed::State::ActionOnViolation::Dropped : AOT::Typed::State::ActionOnViolation::Threw });
}

ASCIILiteral JSObject::putDirectToCompiledObject(VM& vm, PropertyName name, JSValue value, unsigned attributes)
{
    const auto* layout = structure()->compiledLayout();
    if (layout && uncheckedDowncast<AOT::Typed::Object>(this)->putDirectFromNativeCode(vm, name, value, attributes))
        return { };
    if (layout && layout->mayDespecialize) {
        uncheckedDowncast<AOT::Typed::Object>(this)->convertToOrdinaryObject(vm);
        PutPropertySlot slot(this);
        return putDirectInternal<PutModeDefineOwnProperty>(vm, name, value, attributes, slot);
    }
    return throwImmutableShapeError(vm);
}

} // namespace JSC

#endif // ENABLE(AOT)
