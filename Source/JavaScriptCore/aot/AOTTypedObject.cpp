/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedObject.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTRuntime.h"
#include "AOTSlotWatchpoint.h"
#include "AOTTypedCompactTable.h"
#include "AOTTypedType.h"
#include "AbstractModuleRecord.h"
#include "AdaptiveInferredPropertyValueWatchpointBase.h"
#include "AsyncContextSwapScope.h"
#include "AsyncFunctionPrototype.h"
#include "AsyncGeneratorFunctionPrototype.h"
#include "AsyncGeneratorPrototype.h"
#include "AsyncIteratorPrototype.h"
#include "BlockDirectoryInlines.h"
#include "BuiltinNames.h"
#include "CachedTypes.h"
#include "CustomGetterSetter.h"
#include "FunctionPrototype.h"
#include "GeneratorFunctionPrototype.h"
#include "GeneratorPrototype.h"
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
#include "JSPromiseReaction.h"
#include "LLIntThunks.h"
#include "MarkedBlockCage.h"
#include "MarkedSpaceInlines.h"
#include "Microtask.h"
#include "MicrotaskQueueInlines.h"
#include "StackFrame.h"
#include "ObjectConstructor.h"
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
#include <wtf/HexNumber.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

const ClassInfo Object::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Object) };

Structure* Object::createStructure(VM& vm, Instance& instance, JSValue prototype, const LayoutData* layout)
{
    // As of a proxy: it comes to have indexed properties without a transition, and nothing is to give it indexed storage.
    Structure* structure = isObjectLayout(layout->kind)
        ? Structure::create(vm, instance.globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info(), NonArray | MayHaveIndexedAccessors, 0)
        : Structure::create(vm, nullptr, jsNull(), TypeInfo(CellType, StructureFlags), info(), NonArray, 0);
    structure->setCompiledLayout(layout);
    structure->setAOTInstance(&instance);
    // `Object.create(object)`: becoming a prototype is a transition otherwise, and these have none.
    structure->setMayBePrototype(true);
    // What goes past the method table (putDirect(), setPrivateBrand(), ensureArrayStorage()) throws a TypeError where it would make a transition.
    structure->setHasImmutableShape(true);
    return structure;
}

static_assert(offsetOfLowByteOfEnvironmentShape == JSCell::indexingTypeAndMiscOffset() && offsetOfHighByteOfEnvironmentShape == JSCell::typeInfoFlagsOffset());

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
            if (!isAbsentAsZero(variable.store()))
                clearSlot(reinterpret_cast<uint8_t*>(object) + variable.offset, variable.store());
        }
        object->at<uint8_t>(offsetOfLowByteOfEnvironmentShape) = layout.shape;
        object->at<uint8_t>(offsetOfHighByteOfEnvironmentShape) = layout.shape >> 8;
    }
    vm.mutatorFence();
    return object;
}

Object* Object::createWithBody(VM& vm, Structure* structure, std::span<const uint8_t> body)
{
    Object* object = new (NotNull, allocateCell<Object>(vm, body.size() + sizeof(JSCell))) Object(vm, structure);
    memcpySpan(std::span { reinterpret_cast<uint8_t*>(object) + sizeof(JSCell), body.size() }, body);
    vm.mutatorFence();
    return object;
}

Object* Object::createWithBornFields(VM& vm, Structure* structure, const LayoutData& layout, size_t size, uint32_t orderWord)
{
    Object* object = create(vm, structure, layout, size, orderWord);
    // Once, and not a little with each store.
    const FieldData* farthest = nullptr;
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        const FieldData& field = layout.fields()[i];
        if (field.flags & fieldIsOutOfLine && field.flags & fieldAllowsUndefined && !(field.flags & fieldIsAbsentAtBirth) && (!farthest || field.offset > farthest->offset))
            farthest = &field;
    }
    if (farthest)
        object->ensureExtensionCovers(vm, object->state(), *farthest);
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        const FieldData& field = layout.fields()[i];
        if (field.flags & fieldIsAbsentAtBirth)
            continue;
        // What cannot hold `undefined` is stored to before anyone looks.
        bool isUndefined = field.flags & fieldAllowsUndefined;
        switch (field.store()) {
        case Store::JSValue:
        case Store::OptionalCell32:
        case Store::OptionalBoolean:
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
        case Store::Int32OrElsewhere:
            object->at<int32_t>(field.offset) = isUndefined ? undefinedInt32 : 0;
            break;
        default:
            break;
        }
    }
    return object;
}

Object* Object::clone(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = this->state();
    const LayoutData& layout = this->layout();
    RELEASE_ASSERT(layout.kind == LayoutKind::Record);
    Structure* structure = state.structureFor(vm, layout.shape, globalObject->objectPrototype());
    size_t size = state.sizeWithRoomToSpecialize(layout.shape);
    if (hasExtraProperties()) {
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
            // Whatever a getter has made of the record, it still has the fields of the layout it had, and they hold what they may hold. Also a
            // field that is immutable: the copy is born with it.
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
    // Before the copy, which nothing is allocated after until it is filled: the collector has not looked at it by then.
    Extension* extensionOfCopy = nullptr;
    if (layout.hasExtension && extension()) {
        extensionOfCopy = Extension::create(vm, extension()->size());
        memcpySpan(extensionOfCopy->slots(), extension()->slots());
        state.countExtension(0, extensionOfCopy->size());
    }
    // What the record has been made (frozen, sealed) the copy is not.
    Object* copy = create(vm, structure, layout, size, orderWord() & orderAndCountMask);
    memcpySpan(std::span { &copy->at<uint8_t>(offsetOfFirstField), layout.size - offsetOfFirstField }, std::span { &at<uint8_t>(offsetOfFirstField), layout.size - offsetOfFirstField });
    if (layout.hasExtension)
        copy->setExtension(extensionOfCopy);
    copy->copyNumberBoxes(state);
    vm.mutatorFence();
    state.countObject(copy);
    return copy;
}

JSObject* Object::tryCopyAsPlainObject(JSGlobalObject* globalObject) const
{
    State& state = this->state();
    if (hasExtraProperties() || globalObject != state.instance.globalObject)
        return nullptr;
    VM& vm = globalObject->vm();
    auto& plan = state.planOfPlainCopy(vm, layout(), orderWord() & orderAndCountMask);
    if (!plan.structure)
        return nullptr;
    // Its slots are empty until they are filled: the collector may look at it at any time, and so may nobody else.
    JSFinalObject* copy = JSFinalObject::create(vm, plan.structure);
    PropertyOffset offset = 0;
    for (auto& property : plan.properties) {
        JSValue value = readInline(*property.field);
        if (!value)
            return nullptr;
        copy->putDirectOffset(vm, offset++, value);
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
    auto& plan = state.planOfPlainCopyWithout(vm, layout(), orderWord() & orderAndCountMask, atoms.span());
    if (!plan.structure)
        return nullptr;
    JSFinalObject* copy = JSFinalObject::create(vm, plan.structure);
    PropertyOffset offset = 0;
    for (auto& property : plan.properties) {
        JSValue value = readInline(*property.field);
        if (!value)
            return nullptr;
        copy->putDirectOffset(vm, offset++, value);
    }
    return copy;
}

Object* Object::tryCopyAs(JSGlobalObject* globalObject, const LayoutData& to, NamesOfCopy namesAre, std::span<const EncodedJSValue> namesOfCopy, bool* isNeverCopied)
{
    VM& vm = globalObject->vm();
    State& state = this->state();
    auto namesIf = [&](NamesOfCopy which) {
        return namesOfCopy.first(namesAre == which ? namesOfCopy.size() : 0);
    };
    auto excluded = namesIf(NamesOfCopy::Excluded);
    auto storedNext = namesIf(NamesOfCopy::StoredNext);
    auto storedFirst = namesIf(NamesOfCopy::StoredFirst);
    if (layout().kind != LayoutKind::Record || to.kind != LayoutKind::Record || hasExtraProperties() || storedFirst.size() % 2)
        return nullptr;
    // A symbol names no field. What is no property key yet is for the operation to convert, once.
    Vector<UniquedStringImpl*, 8> atoms;
    bool areAtoms = orderOf(orderWord()) != keysAreInLayoutOrder;
    for (EncodedJSValue key : excluded) {
        JSValue value = JSValue::decode(key);
        if (value.isSymbol())
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
        return (value.isObject() ? state.fitsAsOneOfItsOwn(type, asObject(value)) : state.fits(type, value)) && copy->store(vm, field, value);
    };
    if (const State::PlanOfCopy* plan = areAtoms ? state.planOfCopy(layout().shape, orderWord(), to.shape, atoms.span()) : nullptr) {
        if (!plan->structure) {
            if (isNeverCopied)
                *isNeverCopied = true;
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
        for (auto& conversion : plan->fitted) {
            JSValue value = read(layout().fields()[conversion.from]);
            if (!value)
                return nullptr;
            const FieldData& field = to.fields()[conversion.to];
            bool isStored = conversion.kindsTaken & State::PlanOfCoercion::kindOf(value) ? copy->store(vm, field, value) : put(copy, field, value);
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
    Object* copy = create(vm, state.structureFor(vm, to.shape, globalObject->objectPrototype()), to, state.sizeWithRoomToSpecialize(to.shape), state.orderStartingWith(to, { }));
    BitVector created;
    BitVector filled;
    BitVector given;
    auto fill = [&](const FieldData& field) {
        return copy->read(field) || copy->store(vm, field, jsUndefined());
    };
    Vector<uint16_t, 16> keys;
    bool isCopied = true;
    bool failsForAll = false;
    for (size_t i = 0; i < storedFirst.size() && isCopied; i += 2) {
        const FieldData* field = state.field(vm, to, Identifier::fromString(vm, asString(JSValue::decode(storedFirst[i]))->tryGetValue()).impl());
        failsForAll = !field || field->flags & fieldIsImmutable || copy->isDiscriminant(*field) || created.get(copy->indexOf(*field));
        isCopied = !failsForAll && put(copy, *field, JSValue::decode(storedFirst[i + 1]));
        if (!isCopied)
            break;
        copy->willCreateField(state, *field);
        keys.append(copy->indexOf(*field));
        created.set(keys.last());
        given.set(keys.last());
    }
    forEachFieldAsProperty([&](UniquedStringImpl& name, JSValue value) {
        if (!isCopied)
            return false;
        if (names.containsIf([&](const Identifier& other) { return other.impl() == &name; }))
            return true;
        const FieldData* field = state.field(vm, to, &name);
        isCopied = field && !(field->flags & fieldIsImmutable) && !copy->isDiscriminant(*field);
        failsForAll = !isCopied;
        bool isStoredNext = next.containsIf([&](const Identifier& other) { return other.impl() == &name; });
        if (isCopied && isStoredNext) {
            isCopied = fill(*field);
            filled.set(copy->indexOf(*field));
        } else if (isCopied)
            isCopied = put(copy, *field, value);
        if (isCopied && given.get(copy->indexOf(*field)))
            given.clear(copy->indexOf(*field));
        else if (isCopied) {
            copy->willCreateField(state, *field);
            keys.append(copy->indexOf(*field));
            created.set(keys.last());
        }
        return isCopied;
    });
    for (auto& name : next) {
        if (!isCopied)
            break;
        const FieldData* field = state.field(vm, to, name.impl());
        isCopied = field && !(field->flags & fieldIsImmutable) && !copy->isDiscriminant(*field);
        failsForAll = !isCopied;
        if (!isCopied || created.get(copy->indexOf(*field)))
            continue;
        isCopied = fill(*field);
        if (isCopied) {
            copy->willCreateField(state, *field);
            keys.append(copy->indexOf(*field));
            created.set(keys.last());
            filled.set(keys.last());
        }
    }
    for (uint32_t i = 0; i < to.numberOfFields && isCopied; ++i) {
        isCopied = to.fields()[i].flags & fieldIsOptional || created.get(i);
        failsForAll = !isCopied;
    }
    if (!isCopied || copy->hasExtraProperties()) {
        if (failsForAll && areAtoms) {
            auto plan = makeUnique<State::PlanOfCopy>();
            for (UniquedStringImpl* atom : atoms)
                plan->excluded.append(atom);
            state.addPlanOfCopy(layout().shape, orderWord(), to.shape, WTF::move(plan));
            if (isNeverCopied)
                *isNeverCopied = true;
        }
        return nullptr;
    }
    copy->setOrderAndCount(state.orderStartingWith(to, keys.span()));
    state.countObject(copy);
    if (areAtoms && orderOf(copy->orderWord()) != keysAreInLayoutOrder && copy->structure() == state.structure(vm, to.shape)) {
        auto plan = makeUnique<State::PlanOfCopy>();
        plan->structure = copy->structure();
        plan->size = state.sizeWithRoomToSpecialize(to.shape);
        plan->orderWord = copy->orderWord();
        bool holdsForAll = true;
        for (uint16_t key : keys) {
            const FieldData& field = to.fields()[key];
            if (filled.get(key)) {
                holdsForAll = holdsForAll && !(field.flags & fieldIsOutOfLine) && sizeOf(field.store()) <= sizeof(uint64_t);
                if (holdsForAll) {
                    State::PlanOfCopy::Fill bits { field.offset, sizeOf(field.store()), 0 };
                    memcpy(&bits.bits, &copy->at<uint8_t>(field.offset), bits.size);
                    plan->fills.append(bits);
                }
                continue;
            }
            if (given.get(key)) {
                holdsForAll = holdsForAll && !(field.flags & fieldIsOutOfLine);
                plan->given.append({ key, static_cast<uint16_t>(2 * keys.find(key) + 1) });
                continue;
            }
            const FieldData* source = state.field(vm, layout(), state.name(vm, field.name).impl());
            holdsForAll = holdsForAll && source && !((source->flags | field.flags) & fieldIsOutOfLine);
            bool isTypedAlike = holdsForAll && source->type() == field.type() && source->ownType() == field.ownType() && source->numberOfLiterals() == field.numberOfLiterals()
                && (!field.numberOfLiterals() || source->literalsOffset() == field.literalsOffset());
            if (isTypedAlike && source->store() == field.store())
                plan->moves.append({ source->offset, field.offset, sizeOf(field.store()) });
            else if (isTypedAlike)
                plan->conversions.append({ indexOf(*source), key });
            else if (holdsForAll)
                plan->fitted.append({ indexOf(*source), key, state.kindsTakenBy(state.ownTypeOf(field)) });
        }
        if (holdsForAll) {
            for (UniquedStringImpl* atom : atoms)
                plan->excluded.append(atom);
            state.addPlanOfCopy(layout().shape, orderWord(), to.shape, WTF::move(plan));
        }
    }
    return copy;
}

Object* Object::tryMakeAs(JSGlobalObject* globalObject, State& state, const LayoutData& to, std::span<const EncodedJSValue> namesStoredNext)
{
    VM& vm = globalObject->vm();
    if (to.kind != LayoutKind::Record || state.data.isCompiledForAudit)
        return nullptr;
    for (EncodedJSValue key : namesStoredNext) {
        if (JSValue value = JSValue::decode(key); !value.isString() || asString(value)->isRope())
            return nullptr;
    }
    Object* made = create(vm, state.structureFor(vm, to.shape, globalObject->objectPrototype()), to, state.sizeWithRoomToSpecialize(to.shape), state.orderStartingWith(to, { }));
    BitVector created;
    Vector<uint16_t, 16> keys;
    for (EncodedJSValue key : namesStoredNext) {
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
    State::KeyOfMerge key { first.layout().shape, first.orderWord(), second.layout().shape, second.orderWord(), to.shape };
    if (const State::PlanOfMerge* plan = state.planOfMerge(key)) {
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
            case State::PlanOfMerge::MoveKind::Copy:
                memcpy(slot, from, move.size);
                break;
            case State::PlanOfMerge::MoveKind::PurifiedDouble:
                *static_cast<double*>(slot) = purifyNaN(*static_cast<const double*>(from));
                break;
            case State::PlanOfMerge::MoveKind::BoxedDouble:
                *static_cast<EncodedJSValue*>(slot) = JSValue::encode(jsNumber(purifyNaN(*static_cast<const double*>(from))));
                break;
            }
        }
        merged->copyNumberBoxes(state);
        for (auto& conversion : plan->conversions) {
            JSValue value = sources[conversion.source]->readInline(*conversion.from);
            if (!value)
                return nullptr;
            if (!conversion.alwaysFits) {
                const RuntimeType& type = state.ownTypeOf(*conversion.to);
                if (!(value.isObject() ? state.fitsAsOneOfItsOwn(type, asObject(value)) : state.fits(type, value)))
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
    Object* merged = create(vm, state.structureFor(vm, to.shape, globalObject->objectPrototype()), to, state.sizeWithRoomToSpecialize(to.shape), state.orderStartingWith(to, { }));
    BitVector created;
    Vector<uint16_t, 32> keys;
    Vector<uint16_t, 32> sourceOfField(FillWith { }, to.numberOfFields, static_cast<uint16_t>(0));
    bool isCopied = true;
    for (uint16_t i = 0; i < sources.size() && isCopied; ++i) {
        sources[i]->forEachFieldAsProperty([&](UniquedStringImpl& name, JSValue value) {
            const FieldData* field = state.field(vm, to, &name);
            isCopied = field && !(field->flags & fieldIsImmutable) && !merged->isDiscriminant(*field);
            if (isCopied) {
                const RuntimeType& type = state.ownTypeOf(*field);
                isCopied = (value.isObject() ? state.fitsAsOneOfItsOwn(type, asObject(value)) : state.fits(type, value)) && merged->store(vm, *field, value);
            }
            if (isCopied) {
                uint16_t index = merged->indexOf(*field);
                if (!created.get(index)) {
                    merged->willCreateField(state, *field);
                    keys.append(index);
                    created.set(index);
                }
                sourceOfField[index] = i;
            }
            return isCopied;
        });
    }
    for (uint32_t i = 0; i < to.numberOfFields && isCopied; ++i)
        isCopied = to.fields()[i].flags & fieldIsOptional || created.get(i);
    if (!isCopied || merged->hasExtraProperties()) {
        state.addPlanOfMerge(key, makeUnique<State::PlanOfMerge>());
        return nullptr;
    }
    merged->setOrderAndCount(state.orderStartingWith(to, keys.span()));
    state.countObject(merged);
    if (orderOf(merged->orderWord()) == keysAreInLayoutOrder || merged->structure() != state.structure(vm, to.shape))
        return merged;
    auto plan = makeUnique<State::PlanOfMerge>();
    plan->structure = merged->structure();
    plan->size = state.sizeWithRoomToSpecialize(to.shape);
    plan->orderWord = merged->orderWord();
    for (uint16_t index : keys) {
        const FieldData& field = to.fields()[index];
        const FieldData* source = state.field(vm, sources[sourceOfField[index]]->layout(), state.name(vm, field.name).impl());
        if (!source)
            return merged;
        bool isToExtension = field.flags & fieldIsOutOfLine;
        if (isToExtension && (!plan->lastOutOfLine || plan->lastOutOfLine->offset < field.offset))
            plan->lastOutOfLine = &field;
        bool isOfSameType = source->type() == field.type() && source->ownType() == field.ownType() && source->numberOfLiterals() == field.numberOfLiterals() && (!field.numberOfLiterals() || source->literalsOffset() == field.literalsOffset());
        const RuntimeType& held = state.typeOf(*source);
        const RuntimeType& wanted = state.ownTypeOf(field);
        bool holdsOnlyPrimitives = !held.allowsAnyObject() && !held.allowsAnyFunction() && !held.admitsForeignObjects() && !held.numberOfViews() && !held.numberOfJSTypes();
        bool alwaysFits = isOfSameType || (holdsOnlyPrimitives && !field.numberOfLiterals() && !(held.primitives & ~wanted.primitives) && (!(held.primitives & Primitive::Int) || (held.minInteger() >= wanted.minInteger() && held.maxInteger() <= wanted.maxInteger())));
        std::optional<State::PlanOfMerge::MoveKind> kind;
        if (alwaysFits && !(source->flags & fieldIsOutOfLine) && !(field.flags & fieldIsPinned)) {
            if (source->store() == field.store())
                kind = State::PlanOfMerge::MoveKind::Copy;
            else if (source->store() == Store::Double && field.store() == Store::OptionalDouble)
                kind = State::PlanOfMerge::MoveKind::PurifiedDouble;
            else if (source->store() == Store::Double && field.store() == Store::JSValue)
                kind = State::PlanOfMerge::MoveKind::BoxedDouble;
        }
        if (kind)
            plan->moves.append({ source->offset, field.offset, static_cast<uint8_t>(sizeOf(field.store())), static_cast<uint8_t>(sourceOfField[index]), *kind, isToExtension });
        else
            plan->conversions.append({ source, &field, sourceOfField[index], alwaysFits });
    }
    state.addPlanOfMerge(key, WTF::move(plan));
    return merged;
}

bool Object::tryCopyFieldsOf(VM& vm, const Object& source)
{
    State& state = this->state();
    // There write() asks what the slot has room for, and what is asserted is for the general way to report.
    if (state.data.isCompiledForAudit || this == &source || layout().kind != LayoutKind::Record || source.layout().kind != LayoutKind::Record || orderWord() & (orderWordIsFrozen | orderWordIsSealed | orderWordIsNotExtensible))
        return false;
    bool isCopied = true;
    bool hasOnlyFields = source.forEachFieldAsProperty([&](UniquedStringImpl& name, JSValue value) {
        const FieldData* field = state.field(vm, layout(), &name);
        isCopied = field && !isDiscriminant(*field);
        if (!isCopied)
            return false;
        bool wasThere = !!read(*field);
        isCopied = write(vm, *field, value);
        if (isCopied && !wasThere)
            didCreateField(state, *field);
        return isCopied;
    });
    return hasOnlyFields && isCopied;
}

} } // namespace AOT::Typed

static ALWAYS_INLINE const AOT::Typed::LayoutData* compiledLayoutOf(const Structure::AOTFields& aotFields, uint32_t firstShapeOfCompiledEnvironments, const JSCell* cell)
{
    if (auto* data = aotFields.dataOfCompiledEnvironments())
        return &AOT::Typed::layoutOfEnvironment(*data, firstShapeOfCompiledEnvironments, cell);
    return aotFields.compiledLayout();
}

const AOT::Typed::LayoutData* Structure::compiledLayoutOf(const JSCell* cell) const
{
    return JSC::compiledLayoutOf(aotFieldsOrEmpty(), firstShapeOfCompiledEnvironments(), cell);
}

namespace AOT { namespace Typed {

const LayoutData& Object::layout() const
{
    return *compiledLayoutOf(structure()->existingAOTFields(), structure()->firstShapeOfCompiledEnvironments(), this);
}

State& Object::state() const
{
    return *structure()->existingAOTFields().aotInstance->typed;
}

static_assert(cell32Shift == MarkedBlockCage::shift);
static_assert(absentCell32 == JSValue::ValueEmpty && nullCell32 == JSValue::ValueNull && undefinedCell32 == JSValue::ValueUndefined);
// Store::OptionalBoolean: each is told by its low byte, and is that byte.
static_assert(std::max({ JSValue::ValueEmpty, JSValue::ValueNull, JSValue::ValueFalse, JSValue::ValueTrue, JSValue::ValueUndefined }) <= UINT8_MAX);
// A slot for any function may be a 32-bit one, so no closure is a PreciseAllocation. The number of captures is kept in a byte.
static_assert(Closure::allocationSize(UINT8_MAX) <= MarkedSpace::largeCutoff);

// What is kept at the address that way. Empty: nothing.
static JSValue readAs(Store store, const void* address)
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
        return word <= undefinedCell32 ? JSValue::decode(word) : JSValue(MarkedBlockCage::decode(word));
    }
    case Store::OptionalBoolean:
        return JSValue::decode(as(uint8_t()));
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
    // Only a field of an object is kept so, and it takes the object: Object::read().
    case Store::Int32OrElsewhere:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// False: the value cannot be kept that way. Whoever owns the memory sees to the write barrier.
static bool storeAs(Store store, void* address, JSValue value)
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
    // See Object::store().
    case Store::Int32OrElsewhere:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue Object::read(const FieldData& field) const
{
    if (field.store() != Store::Int32OrElsewhere) {
        const void* slot = slotOf(field);
        JSValue value = slot ? readAs(field.store(), slot) : JSValue();
        if (!value && field.flags & fieldMayExistWithoutStorage && isAmongKeysInOrder(state(), field))
            return jsUndefined();
        return value;
    }
    int32_t word = at<int32_t>(field.offset);
    if (refersToNumberBox(word))
        return jsNumber(purifyNaN(*numberBoxAt(word)));
    switch (word) {
    case absentInt32:
        return { };
    case undefinedInt32:
        return jsUndefined();
    case nullInt32:
        return jsNull();
    default:
        return jsNumber(word);
    }
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
    // Compiled code takes it that such a field keeps the value the object was born with.
    if (field.flags & fieldIsImmutable)
        return false;
    State& state = this->state();
    return state.fits(state.typeOf(field), value) && store(vm, field, value);
}

bool Object::assign(JSGlobalObject* globalObject, const FieldData& field, JSValue value, bool* givesUp)
{
    givesUp = layout().mayGiveUp ? givesUp : nullptr;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = this->state();
    // The layout of the object goes by its discriminant, from the allocation on. Compiled code never stores another value by name.
    if (isDiscriminant(field)) {
        JSValue current = read(field);
        if (current ? !(current.isNumber() && value.isNumber() && current.asNumber() == value.asNumber()) : !state.isAmongLiterals(field, value)) {
            if (givesUp) {
                *givesUp = true;
                return false;
            }
            String message = makeString("Cannot assign to '"_s, StringView(state.name(vm, field.name).impl()), "' of "_s, state.nameOf(layout()), ": a discriminant property cannot change, got "_s, State::describe(value));
            if (!state.reportOrThrow(globalObject, { .kind = "discriminant-change"_s, .on = &layout(), .member = state.name(vm, field.name).impl(), .got = value }, message, State::InAudit::CarriedOut))
                return false;
        }
    }
    String failure;
    SetForScope onlyCopies(state.storesMakeOverPlainData, state.storesMakeOverPlainData && !(orderWord() & orderWordIsFrozen));
    JSValue held = field.flags & fieldIsImmutable ? JSValue() : state.holdToTypeOf(globalObject, field, value, failure);
    RETURN_IF_EXCEPTION(scope, false);
    if (!held || !store(vm, field, held)) {
        if (givesUp) {
            *givesUp = true;
            return false;
        }
        state.reportOrThrow(globalObject, { .kind = "field-store-untyped"_s, .on = &layout(), .member = state.name(vm, field.name).impl(), .expected = &state.ownTypeOf(field), .got = value }, state.assignmentMessage(vm, makeString('\'', StringView(state.name(vm, field.name).impl()), "' of "_s, state.nameOf(layout())), field, value, failure), State::InAudit::Ignored);
        return false;
    }
    return true;
}

bool Object::store(VM& vm, const FieldData& field, JSValue value)
{
    if (field.flags & fieldIsPinned) [[unlikely]]
        state().willAlter(field, value);
    if (field.store() == Store::Int32OrElsewhere) {
        if (value.isUndefinedOrNull()) {
            at<int32_t>(field.offset) = value.isUndefined() ? undefinedInt32 : nullInt32;
            return true;
        }
        if (!value.isNumber())
            return false;
        double number = value.asNumber();
        // Negative zero is no int32.
        if (number >= leastInt32InPlace && number <= INT32_MAX && number == std::trunc(number) && !(!number && std::signbit(number)) && !refersToNumberBox(at<int32_t>(field.offset)))
            at<int32_t>(field.offset) = static_cast<int32_t>(number);
        else
            storeElsewhere(vm, field, value);
        return true;
    }
    if (field.flags & fieldIsOutOfLine)
        ensureExtensionCovers(vm, state(), field);
    if (!storeAs(field.store(), slotOf(field), value))
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

void Object::storeElsewhere(VM&, const FieldData& field, JSValue number)
{
    RELEASE_ASSERT(field.store() == Store::Int32OrElsewhere && number.isNumber());
    if (int32_t word = at<int32_t>(field.offset); refersToNumberBox(word))
        *numberBoxAt(word) = number.asNumber();
    else
        storeInNumberBoxOfItsOwn(state(), field.offset, number.asNumber());
}

void Object::storeInNumberBoxOfItsOwn(State& state, uint32_t offset, double number)
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
        if (field.store() != Store::Int32OrElsewhere)
            continue;
        if (int32_t word = at<int32_t>(field.offset); refersToNumberBox(word))
            storeInNumberBoxOfItsOwn(state, field.offset, *numberBoxAt(word));
    }
}

static void clearSlot(void* slot, Store store)
{
    switch (store) {
    case Store::Cell:
    case Store::JSValue:
        *static_cast<EncodedJSValue*>(slot) = JSValue::encode(JSValue());
        return;
    case Store::Cell32:
    case Store::OptionalCell32:
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
    case Store::Int32OrElsewhere:
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
    // While specialize() is at work this may be the old layout or the new one. The fields of either are fit to be visited.
    const LayoutData& layout = object->layout();
    if (isObjectLayout(layout.kind) && object->orderWord() & orderWordHasExtraProperties) {
        if (JSObject* extra = object->state().extraProperties.getConcurrently(object))
            visitor.appendUnbarriered(extra);
    }
    if (!layout.referencesOffset)
        return;
    // Boxed slots, then cell slots. A cell slot holds a cell or nothing, which reads the same as a boxed value. Then 32-bit slots.
    const uint32_t* references = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(&layout) + layout.referencesOffset);
    std::span wide { references + 3, static_cast<size_t>(references[0]) + references[1] };
    for (uint32_t offset : wide)
        visitor.appendUnbarriered(JSValue::decode(object->at<EncodedJSValue>(offset)));
    std::span narrow { wide.data() + wide.size(), references[2] };
    for (uint32_t offset : narrow) {
        if (uint32_t word = object->at<uint32_t>(offset); word > undefinedCell32)
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
        if (uint32_t word = *static_cast<const uint32_t*>(extension->slot(offset)); word > undefinedCell32)
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
            JSValue value = readAs(variable.store(), reinterpret_cast<const uint8_t*>(object) + variable.offset);
            const Identifier& name = state.nameIfMade(variable.name);
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
        JSValue value = slot ? readAs(field.store(), slot) : JSValue();
        // The collector cannot make an identifier of a name that nothing has asked for yet.
        const Identifier& name = state.nameIfMade(field.name);
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
    static_assert(leastExtensionSize == MarkedBlock::atomSize);
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

JSObject* ExtraPropertiesTable::getConcurrently(const Object* object)
{
    Locker locker { m_lock };
    return get(object);
}

void ExtraPropertiesTable::set(Object* object, JSObject* extra)
{
    {
        Locker locker { m_lock };
        m_map.set(object, extra);
    }
    recentlyRead[ExtraPropertiesOfObject::placeOf(object)] = { };
    markDirty(m_vm);
}

void ExtraPropertiesTable::remove(Object* object)
{
    Locker locker { m_lock };
    m_map.remove(object);
    recentlyRead[ExtraPropertiesOfObject::placeOf(object)] = { };
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
            m_end = (static_cast<uint32_t>(leastInt32InPlace) - static_cast<uint32_t>(INT32_MIN)) >> i;
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
            m_firstFree = static_cast<uint32_t>(*at(place));
            return place;
        }
        if (m_next >= m_committed) {
            RELEASE_ASSERT(m_committed < m_end);
            uint32_t more = std::min(m_end - m_committed, placesCommittedAtOnce);
            OSAllocator::commit(at(m_committed), static_cast<size_t>(more) << numberBoxShift, true, false);
            m_committed += more;
        }
        return m_next++;
    }

    void free(uint32_t place)
    {
        Locker locker { m_lock };
        *at(place) = m_firstFree;
        m_firstFree = place;
    }

private:
    static constexpr uint32_t placesCommittedAtOnce = 1 << 15;

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

unsigned Object::attributesOfFieldOfWhatAsks(const FieldData& field) const
{
    unsigned ofAll = static_cast<unsigned>(isSealed() ? PropertyAttribute::DontDelete : PropertyAttribute::None);
    auto* own = state().fieldAttributes.get(this);
    if (!own || own->isFrozen)
        ofAll = PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
    if (own) {
        for (auto& [name, attributes] : own->ofFields) {
            if (name == field.name)
                return ofAll | attributes;
        }
    }
    return ofAll;
}

void Object::setAttributesOf(VM& vm, const FieldData& field, unsigned attributes)
{
    auto& own = state().fieldAttributes.ensure(this);
    own.ofFields.removeFirstMatching([&](auto& entry) {
        return entry.first == field.name;
    });
    if (attributes)
        own.ofFields.append({ field.name, static_cast<uint8_t>(attributes) });
    ensureExtraProperties(vm);
    at<uint32_t>(offsetOfOrderWord) |= orderWordIsFrozen;
}

JSObject* Object::extraProperties() const
{
    return hasExtraProperties() ? state().extraProperties.get(this) : nullptr;
}

bool hasFieldNamedThen(Structure* structure)
{
    State& state = *structure->aotInstance()->typed;
    VM& vm = *state.instance.vm;
    return state.field(vm, *structure->compiledLayout(), vm.propertyNames->then.impl());
}

bool neverHasThen(Structure* structure)
{
    auto hasThenOrMayChangePrototype = [](Structure* current) {
        return current->hasSpecialProperties() || current->typeInfo().overridesGetPrototype() || !current->hasMonoProto() || current->isDictionary();
    };
    PropertyName then = structure->vm().propertyNames->then;
    if (hasThenOrMayChangePrototype(structure))
        return false;
    if (structure->compiledLayout() ? !Object::absenceIsByStructure(structure, then.uid()) : structure->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence())
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

void Object::setExtraProperties(VM& vm, JSObject* extra)
{
    ASSERT(!extraProperties());
    Structure* withExtraProperties = state().structureFor(vm, layout().shape, getPrototypeDirect(), true);
    state().extraProperties.set(this, extra);
    state().noteExtraPropertiesOf(this, extra);
    at<uint32_t>(offsetOfOrderWord) |= orderWordHasExtraProperties;
    JSCell::setStructure(vm, withExtraProperties);
    vm.writeBarrier(this, extra);
    state().countExtraPropertyTable(layout());
}

// Array.prototype.concat() takes it from the Structure that what inherits from Object.prototype has no such property.
bool Object::mayDefineAccessorOnExtraProperties(VM& vm, State& state, PropertyName name, JSObject*& extra)
{
    if (fieldNamed(state, name) || parseIndex(name) || isNotExtensible() || state.hasFixedMethod(layout(), name.uid()))
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

JSFinalObject* Object::copyAsOrdinary(VM& vm, unsigned inlineCapacity)
{
    State& state = this->state();
    const LayoutData& layout = this->layout();
    JSGlobalObject* realm = structure()->realm();
    JSValue prototype = getPrototypeDirect();
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
    for (uint16_t key : keys) {
        if (key >= layout.numberOfFields)
            copyExtra(state.extraKeyName(layout, key));
        else if (JSValue value = read(fields[key]))
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

JSObject* Object::becomeOrdinary(VM& vm)
{
    State& state = this->state();
    RELEASE_ASSERT(layout().mayGiveUp);
    static_assert(sizeof(JSObjectWithButterfly) == offsetOfFirstField && JSObjectWithButterfly::butterflyOffset() == offsetOfOrderWord);
    unsigned inlineCapacity = std::min<size_t>((cellSize() - offsetOfFirstField) / sizeof(EncodedJSValue), JSFinalObject::maxInlineCapacity);
    JSFinalObject* ordinary = copyAsOrdinary(vm, inlineCapacity);
    ordinary->didBecomePrototype(vm);
    RELEASE_ASSERT(ordinary->structure()->inlineCapacity() == inlineCapacity);

    if (extraProperties())
        state.extraProperties.remove(this);
    if (orderWord() & orderWordIsFrozen)
        state.fieldAttributes.remove(this);
    {
        PreventCollectionScope preventCollection(vm.heap);
        for (unsigned i = 0; i < inlineCapacity; ++i)
            at<EncodedJSValue>(offsetOfFirstField + i * sizeof(EncodedJSValue)) = JSValue::encode(ordinary->inlineStorage()[i].get());
        at<Butterfly*>(offsetOfOrderWord) = ordinary->butterfly();
        JSCell::setStructure(vm, ordinary->structure());
    }
    vm.writeBarrier(this);
    ensureStillAliveHere(ordinary);
    return this;
}

void Object::dropExtraPropertiesIfEmpty(VM& vm)
{
    JSObject* extra = extraProperties();
    if (!extra || hasIndexedProperties(extra->indexingType()) || (orderWord() & orderWordIsFrozen && state().fieldAttributes.get(this)))
        return;
    bool isEmpty = true;
    extra->structure()->forEachProperty(vm, [&](const auto&) {
        isEmpty = false;
        return false;
    });
    if (!isEmpty)
        return;
    Structure* withoutExtraProperties = state().structureFor(vm, layout().shape, getPrototypeDirect(), false);
    at<uint32_t>(offsetOfOrderWord) &= ~orderWordHasExtraProperties;
    state().extraProperties.remove(this);
    JSCell::setStructure(vm, withoutExtraProperties);
    noteWhetherToAsk(state());
}

bool State::isPrototypeByLayout(const LayoutData& layout, JSValue prototype) const
{
    if (layout.kind == LayoutKind::Instance)
        return prototype == JSValue(prototypeOfClassIfExists(layout.index));
    auto* prototypeOfFunction = dynamicDowncast<PrototypeObject>(prototype);
    return prototype.isNull() || prototype == JSValue(instance.globalObject->objectPrototype()) || (prototypeOfFunction && (layout.mayBeMadeByNew || !(layout.mayHaveOtherPrototype || layout.mayGiveUp)) && prototypeOfFunction->isGuarded() && !prototypeOfFunction->isAltered());
}

void Object::noteWhetherToAsk(State& state, bool isBirth)
{
    if (!layout().mayHaveOtherPrototype)
        return;
    if (!state.isPrototypeByLayout(layout(), getPrototypeDirect()))
        at<uint32_t>(offsetOfOrderWord) |= orderWordHasExtraProperties;
    else if (!isBirth && !state.extraProperties.get(this))
        at<uint32_t>(offsetOfOrderWord) &= ~orderWordHasExtraProperties;
}

void State::didAlterPrototypeObject(VM& vm, PrototypeObject* prototype)
{
    for (uint32_t i = 0; i < data.numberOfAllocations; ++i) {
        // An allocation without a row is asked each time: orderWordAtBirth().
        Instance::TypedAllocation* row = instance.typedAllocation(i);
        if (row && std::bit_cast<StructureID>(static_cast<uint32_t>(row->header)).decode()->storedPrototype() == JSValue(prototype))
            row->words |= orderWordHasExtraProperties;
    }
    HeapIterationScope iterationScope(vm.heap);
    vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* cell, HeapCell::Kind kind) {
        if (!isJSCellKind(kind))
            return IterationStatus::Continue;
        Structure* structure = static_cast<JSCell*>(cell)->structure();
        const LayoutData* layout = structure->compiledLayout();
        if (layout && isObjectLayout(layout->kind) && structure->storedPrototype() == JSValue(prototype))
            uncheckedDowncast<Object>(static_cast<JSCell*>(cell))->noteWhetherToAsk(*this);
        return IterationStatus::Continue;
    });
}

bool Object::absenceIsByStructure(Structure* structure, UniquedStringImpl* name)
{
    const LayoutData* layout = structure->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return false;
    if (parseIndex(*name) || (name->isSymbol() && static_cast<SymbolImpl*>(name)->isPrivate()))
        return false;
    State& state = *structure->aotInstance()->typed;
    return state.isOfObjectsWithoutExtraProperties(structure) && (name->isSymbol() || !state.field(*state.instance.vm, *layout, name));
}

std::optional<Object::FieldAccess> Object::fieldAccessFor(VM&, PropertyName name, bool takesWhateverFits)
{
    State& state = this->state();
    const FieldData* field = fieldNamed(state, name);
    if (!field)
        return std::nullopt;
    FieldAccess access { field->offset, field->store(), !!(field->flags & fieldIsOutOfLine), state.mayBeAbsent(*field) };
    // See assign(). In an audit image every store is looked at.
    if (field->flags & (fieldIsImmutable | fieldIsPinned) || isDiscriminant(*field) || state.data.isCompiledForAudit)
        return access;
    const RuntimeType& type = state.ownTypeOf(*field);
    access.acceptedPrimitives = type.primitives & (Primitive::All & ~Primitive::Int);
    if (type.primitives & Primitive::Int) {
        access.acceptsInt32 = type.minInteger() <= INT32_MIN && type.maxInteger() >= INT32_MAX;
        if (type.minInteger() <= -maxKeptInteger && type.maxInteger() >= maxKeptInteger)
            access.acceptedPrimitives |= Primitive::Int;
    }
    // Of any other type an object may be made one of the program's on its way in: see State::holdToTypeOf().
    access.acceptsAnyObject = type.allowsAnyObject() && (takesWhateverFits || !type.numberOfViews());
    if (!type.allowsAnyObject() && !(field->ownType() & 3) && (takesWhateverFits || state.membersHaveNoSpecializations(type)))
        access.typeOfAcceptedObjects = field->ownType();
    return access;
}

const FieldData* Object::fieldNamed(State& state, PropertyName name) const
{
    if (name.isSymbol() && !name.isPrivateName())
        return nullptr;
    return state.field(*state.instance.vm, layout(), name.uid());
}

void Object::forEachFieldInKeyOrder(State& state, const Invocable<void(uint16_t)> auto& functor) const
{
    uint32_t numberOfFields = layout().numberOfFields;
    forEachKeyInOrder(state, [&](uint16_t entry) {
        if (entry < numberOfFields)
            functor(entry);
    });
}

bool Object::forEachFieldAsProperty(const ScopedLambda<bool(UniquedStringImpl&, JSValue)>& functor) const
{
    if (hasExtraProperties())
        return false;
    State& state = this->state();
    VM& vm = *state.instance.vm;
    if (auto properties = state.propertiesInOrder(vm, layout(), orderWord() & orderAndCountMask)) {
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
        if (!goesOn || state.isPrivate(fields[index]))
            return;
        if (JSValue value = read(fields[index]))
            goesOn = functor(*state.name(vm, fields[index].name).impl(), value);
    });
    return true;
}

bool Object::forEachFieldAndExtraProperty(const ScopedLambda<bool(UniquedStringImpl&, JSValue)>& functor) const
{
    bool mayHideFields = orderWord() & orderWordIsFrozen;
    State& state = this->state();
    VM& vm = *state.instance.vm;
    JSObject* extra = extraProperties();
    Structure* structureOfExtra = extra ? extra->structure() : nullptr;
    if (extra && (extra->type() != FinalObjectType || !structureOfExtra->canPerformFastPropertyEnumeration() || structureOfExtra->hasNonReifiedStaticProperties() || extra->getDirect(vm, vm.propertyNames->toJSON)))
        return false;
    const LayoutData& layout = this->layout();
    const FieldData* fields = layout.fields();
    Vector<UniquedStringImpl*, 8> placed;
    bool goesOn = true;
    forEachKeyInOrder(state, [&](uint16_t entry) {
        if (!goesOn)
            return;
        if (entry >= layout.numberOfFields) {
            if (!extra)
                return;
            const Identifier& name = state.extraKeyName(layout, entry);
            unsigned attributes;
            JSValue value = extra->getDirect(vm, name, attributes);
            if (!value || attributes & PropertyAttribute::DontEnum)
                return;
            placed.append(name.impl());
            goesOn = functor(*name.impl(), value);
            return;
        }
        if (state.isPrivate(fields[entry]) || (mayHideFields && attributesOf(fields[entry]) & PropertyAttribute::DontEnum))
            return;
        if (JSValue value = read(fields[entry]))
            goesOn = functor(*state.name(vm, fields[entry].name).impl(), value);
    });
    if (extra && goesOn) {
        structureOfExtra->forEachProperty(vm, [&](const auto& entry) -> bool {
            if (entry.attributes() & PropertyAttribute::DontEnum || entry.key()->isSymbol() || placed.contains(entry.key()))
                return true;
            return functor(*entry.key(), extra->getDirect(entry.offset()));
        });
    }
    return true;
}

JSCellButterfly* Object::keys(VM& vm) const
{
    return hasExtraProperties() ? nullptr : state().keysInOrder(vm, layout(), orderWord() & orderAndCountMask);
}

void Object::placeExtraKey(State& state, PropertyName name, uint32_t position)
{
    if (name.isSymbol() || parseIndex(name) || orderOf(orderWord()) == keysAreInLayoutOrder)
        return;
    const LayoutData& layout = this->layout();
    auto entry = state.extraKeyEntry(layout, name.uid(), true);
    if (!entry)
        return;
    uint32_t before = orderWord() & orderAndCountMask;
    if (auto placed = state.orderWithExtraKeyPlaced(layout, before, *entry, position)) {
        setOrderAndCount(*placed);
        return;
    }
    Vector<uint16_t, 16> keys;
    forEachKeyInOrder(state, [&](uint16_t key) {
        keys.append(key);
    });
    uint32_t result = before;
    // Unless it is the second accessor of a pair.
    if (!keys.contains(*entry)) {
        keys.insert(std::min<size_t>(position, keys.size()), *entry);
        result = state.orderStartingWith(layout, keys.span(), false);
    }
    state.didPlaceExtraKey(layout, before, *entry, position, result);
    setOrderAndCount(result);
}

void Object::placeExtraKeys(State& state, std::span<const std::pair<UniquedStringImpl*, uint32_t>> namesAndPositions)
{
    if (orderOf(orderWord()) == keysAreInLayoutOrder)
        return;
    const LayoutData& layout = this->layout();
    Vector<uint16_t, 16> keys;
    forEachKeyInOrder(state, [&](uint16_t key) {
        keys.append(key);
    });
    size_t numberOfPlacedKeys = keys.size();
    for (auto [name, position] : namesAndPositions) {
        if (name->isSymbol() || parseIndex(*name))
            continue;
        // Unless it is the second accessor of a pair.
        if (auto entry = state.extraKeyEntry(layout, name, true); entry && !keys.contains(*entry))
            keys.insert(std::min<size_t>(position, keys.size()), *entry);
    }
    if (keys.size() != numberOfPlacedKeys)
        setOrderAndCount(state.orderStartingWith(layout, keys.span(), false));
}

void Object::removeExtraKey(State& state, PropertyName name)
{
    if (name.isSymbol() || orderOf(orderWord()) == keysAreInLayoutOrder)
        return;
    const LayoutData& layout = this->layout();
    auto entry = state.extraKeyEntry(layout, name.uid(), false);
    if (!entry)
        return;
    Vector<uint16_t, 16> keys;
    forEachKeyInOrder(state, [&](uint16_t key) {
        if (key != *entry)
            keys.append(key);
    });
    if (keys.size() != countOf(orderWord()))
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

void Object::placeExtraKeys(State& state)
{
    JSObject* extra = extraProperties();
    if (!extra || orderOf(orderWord()) == keysAreInLayoutOrder)
        return;
    const LayoutData& layout = this->layout();
    Vector<uint16_t, 16> keys;
    forEachKeyInOrder(state, [&](uint16_t key) {
        keys.append(key);
    });
    size_t numberOfPlacedKeys = keys.size();
    extra->structure()->forEachProperty(vm(), [&](const auto& property) {
        auto entry = property.key()->isSymbol() ? std::nullopt : state.extraKeyEntry(layout, property.key(), true);
        if (entry && !keys.contains(*entry))
            keys.append(*entry);
        // Beyond that there is no order: see orderStartingWith().
        return keys.size() <= maxFieldsInOrder;
    });
    if (keys.size() != numberOfPlacedKeys)
        setOrderAndCount(state.orderStartingWith(layout, keys.span()));
}

bool Object::isAmongKeysInOrder(State& state, const FieldData& field) const
{
    uint32_t word = orderWord();
    if (orderOf(word) == keysAreInLayoutOrder)
        return false;
    auto keys = state.fieldsInOrder(layout(), orderOf(word)).first(countOf(word));
    return std::ranges::find(keys, indexOf(field)) != keys.end();
}

void Object::didCreateField(State& state, const FieldData& field, bool withStorage)
{
    if (field.flags & fieldMayExistWithoutStorage && isAmongKeysInOrder(state, field)) {
        if (withStorage)
            ensureExtensionCovers(vm(), state, field);
        return;
    }
    // Keys are listed in the order of their creation, and an extra property without a place after all that have one.
    if (hasExtraProperties()) [[unlikely]]
        placeExtraKeys(state);
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
        if (const void* slot = slotOf(field); slot && readAs(field.store(), slot))
            continue;
        ensureExtensionCovers(vm(), state, field);
        bool didStore = store(vm(), field, jsUndefined());
        RELEASE_ASSERT(didStore);
    }
}

void Object::didMoveExtraPropertyToField(State& state, const FieldData& field)
{
    const LayoutData& layout = this->layout();
    auto entry = orderOf(orderWord()) != keysAreInLayoutOrder ? state.extraKeyEntry(layout, state.name(vm(), field.name).impl(), false) : std::nullopt;
    Vector<uint16_t, 16> keys;
    if (entry) {
        forEachKeyInOrder(state, [&](uint16_t key) {
            keys.append(key);
        });
    }
    size_t position = entry ? keys.find(*entry) : notFound;
    if (position == notFound) {
        appendField(state, field);
        return;
    }
    keys[position] = indexOf(field);
    willCreateField(state, field);
    setOrderAndCount(state.orderStartingWith(layout, keys.span()));
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

// Nothing is inherited, and no proxy is asked. What is not in a field or of the class is kept as a property by the name: read-only if a method.
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
    // Of a class that is not evaluated once they are functions of the evaluation the object is of: its prototype object keeps them.
    for (JSValue next = compiled->getPrototypeDirect(); next.isObject() && !findIn(asObject(next)); next = asObject(next)->getPrototypeDirect()) { }
    return element;
}

// Compiled code calls a method of a class by the shape of the object, whatever the object has by the name, unless the program is seen to
// replace the method. The one thing the object may have is what makes no difference to that: the method it inherits, bound to the
// object itself. `this.stop = this.stop.bind(this)`.
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
        // It captures what the function captures, which of the one closure of a method is nothing, then `this`, then the arguments.
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

static bool reportOrThrowIsMethodOfClass(JSGlobalObject* globalObject, State& state, const LayoutData& layout, PropertyName name)
{
    return state.reportOrThrow(globalObject, { .kind = "method-shadow"_s, .on = &layout, .member = name.uid() }, makeString("Cannot replace method '"_s, StringView(name.uid()), "' of "_s, state.nameOf(layout), ": a method can be replaced only if TypeScript code of the program assigns to it (binding the method to its own object is allowed)"_s), State::InAudit::CarriedOut);
}

bool Object::getOwnPropertySlot(JSObject* cell, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    if (const FieldData* field = object->fieldNamed(object->state(), name)) {
        // A field that is absent is no property: the search goes on along the prototype chain.
        JSValue value = object->read(*field);
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

// Integer indices, the fields in the order of their creation, the other strings, the symbols.
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
        object->forEachKeyInOrder(state, [&](uint16_t index) {
            // What is listed here is not listed again with the other extra properties below.
            if (index >= layout.numberOfFields) {
                const Identifier& name = state.extraKeyName(layout, index);
                unsigned attributes;
                if (extra && extra->getDirect(vm, name, attributes) && (mode == DontEnumPropertiesMode::Include || !(attributes & PropertyAttribute::DontEnum)))
                    names.add(name);
                return;
            }
            if (!state.isPrivate(fields[index]) && !(mayHideFields && object->attributesOf(fields[index]) & PropertyAttribute::DontEnum))
                names.add(state.name(vm, fields[index].name));
        });
    }
    if (extra)
        extra->getOwnNonIndexPropertyNames(globalObject, names, mode);
}

// Whether [[Set]] of the object with another receiver may do anything but have the receiver define the property.
static bool mayInterceptPut(VM& vm, JSObject* object, PropertyName name)
{
    if (name == vm.propertyNames->underscoreProto)
        return true;
    while (true) {
        Structure* structure = object->structure();
        // What a PrototypeObject does for another receiver is what an ordinary object does.
        if (structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || structure->typeInfo().overridesGetPrototype() || (structure->typeInfo().overridesPut() && structure->classInfoForCells() != PrototypeObject::info()))
            return true;
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject())
            return false;
        object = asObject(prototype);
    }
}

// OrdinarySet step by step: with another receiver, as when the object is on the prototype chain of the receiver, and with an index. What
// the object has not got is for [[Set]] of its parent. ordinarySetSlow() walks the chain by itself, and takes for read-only what
// inheritors may override (Structure::inheritorsMayOverrideReadOnlyProperties()).
static bool setStepByStep(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue value, PutPropertySlot& slot)
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

// OrdinarySet.
bool Object::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = object->state();
    if (!value.isObject() && slot.thisValue() == JSValue(object) && !(object->orderWord() & ~orderAndCountMask) && !state.data.isCompiledForAudit) {
        const FieldData* field = object->fieldNamed(state, name);
        if (field && !object->isDiscriminant(*field) && object->readInline(*field) && object->write(vm, *field, value))
            return true;
    }
    if (slot.thisValue() == JSValue(object) && state.storeByKnownTransition(vm, object, name.uid(), value))
        return true;
    if (slot.thisValue() != JSValue(object) || parseIndex(name)) [[unlikely]]
        RELEASE_AND_RETURN(scope, setStepByStep(globalObject, object, name, value, slot));
    if (const FieldData* field = object->fieldNamed(state, name)) {
        // Also of a field that is absent, whatever is inherited: a compiled store does not ask the prototype chain either. `#name` is no property.
        if (!name.isPrivateName() && object->attributesOf(*field) & PropertyAttribute::ReadOnly)
            return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
        if (object->read(*field)) {
            bool givesUp = false;
            bool isStored = object->assign(globalObject, *field, value, &givesUp);
            if (!givesUp)
                RELEASE_AND_RETURN(scope, isStored);
            RELEASE_AND_RETURN(scope, object->becomeOrdinary(vm)->methodTable()->put(cell, globalObject, name, value, slot));
        }
    } else if (JSObject* extra = object->extraProperties()) {
        unsigned attributes;
        if (JSValue existing = extra->getDirect(vm, name, attributes)) {
            if (attributes & PropertyAttribute::Accessor)
                RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(existing)->callSetter(globalObject, object, value, slot.isStrictMode()));
            if (attributes & PropertyAttribute::ReadOnly)
                return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
            if (state.hasFixedMethod(object->layout(), name.uid()) && !isInheritedMethodBoundTo(vm, object, name, value) && !reportOrThrowIsMethodOfClass(globalObject, state, object->layout(), name))
                return false;
            extra->putDirect(vm, name, value, attributes);
            return true;
        }
    }
    // It has no such property, so its parent is asked, where a setter or a read-only property may take the store.
    JSValue prototype = object->getPrototypeDirect();
    if (prototype.isObject() && !name.isPrivateName() && mayInterceptPut(vm, asObject(prototype), name))
        RELEASE_AND_RETURN(scope, asObject(prototype)->methodTable()->put(asObject(prototype), globalObject, name, value, slot));
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
        state.reportOrThrow(globalObject, { .kind = "runtime-accessor"_s, .on = &layout() }, messageFor(why), State::InAudit::Ignored);
    };
    auto refuseAsTheLanguageDoes = [&](ASCIILiteral why) {
        throwTypeError(globalObject, scope, messageFor(why));
    };
    unsigned attributes;
    JSObject* extra = extraProperties();
    if (extra && extra->getDirect(vm, name, attributes)) {
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
            return refuse("TypeScript code of the program cannot call a setter that was added at run time"_s);
        if (attributes & PropertyAttribute::ReadOnly)
            return refuseAsTheLanguageDoes("the property is read only"_s);
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
            return refuseAsTheLanguageDoes("the inherited property is read only"_s);
        break;
    }
    if (!isNotExtensible() && !parseIndex(name) && !state.hasFixedMethod(layout(), name.uid())) {
        bool isStored = state.specializeToStore(globalObject, this, name, value);
        RETURN_IF_EXCEPTION(scope, void());
        if (isStored)
            return;
        willAddExtraProperty(vm, name);
        ensureExtraProperties(vm)->putDirect(vm, name, value);
        return;
    }
    scope.release();
    defineOwnProperty(this, globalObject, name, PropertyDescriptor(value, static_cast<unsigned>(PropertyAttribute::None)), true);
}

bool Object::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    PutPropertySlot slot(cell, shouldThrow);
    return setStepByStep(globalObject, uncheckedDowncast<Object>(cell), Identifier::from(globalObject->vm(), index), value, slot);
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
        if (layout.mayGiveUp && (!(field->flags & fieldIsOptional) || object->isDiscriminant(*field)))
            return object->becomeOrdinary(vm)->methodTable()->deleteProperty(cell, globalObject, name, slot);
        if (!(field->flags & fieldIsOptional))
            return state.reportOrThrow(globalObject, { .kind = "field-delete"_s, .on = &layout, .member = name.uid() }, makeString("Cannot delete property '"_s, StringView(name.uid()), "' of "_s, state.nameOf(layout), ": only optional properties can be deleted"_s), State::InAudit::Ignored);
        if (object->isDiscriminant(*field) && !state.reportOrThrow(globalObject, { .kind = "discriminant-change"_s, .on = &layout, .member = name.uid() }, makeString("Cannot delete property '"_s, StringView(name.uid()), "' of "_s, state.nameOf(layout), ": a discriminant property cannot change"_s), State::InAudit::CarriedOut))
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
            state.willAlter(*field, JSValue());
        object->clear(*field);
        if (object->orderWord() & orderWordIsFrozen) [[unlikely]]
            object->setAttributesOf(vm, *field, 0);
        return true;
    }
    JSObject* extra = object->extraProperties();
    if (!extra)
        return true;
    bool isDeleted = extra->methodTable()->deleteProperty(extra, globalObject, name, slot);
    slot.disableCaching();
    if (isDeleted) {
        object->removeExtraKey(state, name);
        object->dropExtraPropertiesIfEmpty(vm);
    }
    return isDeleted;
}

bool Object::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    JSObject* extra = uncheckedDowncast<Object>(cell)->extraProperties();
    return !extra || extra->methodTable()->deletePropertyByIndex(extra, globalObject, index);
}

bool Object::defineOwnProperty(JSObject* cell, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    Object* object = uncheckedDowncast<Object>(cell);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    State& state = object->state();
    const LayoutData& layout = object->layout();
    const FieldData* field = object->fieldNamed(state, name);
    if (!field) {
        JSObject* extra = object->extraProperties();
        bool exists = extra && extra->hasOwnProperty(globalObject, name);
        RETURN_IF_EXCEPTION(scope, false);
        // A store to a hole of what inherits from the object looks for setters and read-only elements in indexed storage only.
        if (parseIndex(name) && !object->isFrozen() && (descriptor.isAccessorDescriptor() || !(descriptor.writablePresent() ? descriptor.writable() : exists)) && layout.mayGiveUp)
            RELEASE_AND_RETURN(scope, object->becomeOrdinary(vm)->methodTable()->defineOwnProperty(cell, globalObject, name, descriptor, shouldThrow));
        if (parseIndex(name) && !object->isFrozen() && (descriptor.isAccessorDescriptor() || !(descriptor.writablePresent() ? descriptor.writable() : exists)))
            return state.reportOrThrow(globalObject, { .kind = "index-accessor"_s, .on = &layout }, makeString("Cannot define property '"_s, StringView(name.uid()), "' of "_s, state.nameOf(layout), ": indexed properties of an object of a declared type cannot be accessors or read only"_s), State::InAudit::Ignored);
        if (state.hasFixedMethod(layout, name.uid()) && (descriptor.isAccessorDescriptor() || (descriptor.value() ? !isInheritedMethodBoundTo(vm, object, name, descriptor.value()) : !exists)) && !reportOrThrowIsMethodOfClass(globalObject, state, layout, name))
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
        RELEASE_AND_RETURN(scope, extra->methodTable()->defineOwnProperty(extra, globalObject, name, descriptor, shouldThrow));
    }

    // ValidateAndApplyPropertyDescriptor. What a field holds is a data property, writable, enumerable and configurable unless the object is frozen.
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
        if (layout.mayGiveUp)
            RELEASE_AND_RETURN(scope, object->becomeOrdinary(vm)->methodTable()->defineOwnProperty(cell, globalObject, name, descriptor, shouldThrow));
        if (!state.reportOrThrow(globalObject, { .kind = "field-redefine"_s, .on = &layout, .member = name.uid() }, makeString("Cannot redefine property '"_s, StringView(name.uid()), "' of "_s, state.nameOf(layout), ": declared properties stay writable, enumerable and configurable data properties"_s), State::InAudit::Ignored))
            return false;
        if (descriptor.isAccessorDescriptor())
            return true;
    }
    if (object->isFrozen())
        return true;
    // What is there stays as it is.
    if (!current || descriptor.value()) {
        bool givesUp = false;
        bool isStored = object->assign(globalObject, *field, descriptor.value() ? descriptor.value() : jsUndefined(), &givesUp);
        RETURN_IF_EXCEPTION(scope, false);
        if (givesUp)
            RELEASE_AND_RETURN(scope, object->becomeOrdinary(vm)->methodTable()->defineOwnProperty(cell, globalObject, name, descriptor, shouldThrow));
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
        bool wasThere = !!read(*field);
        if (!write(vm, *field, value))
            return false;
        if (!wasThere)
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
    if (!layout().mayBeFrozen && layout().mayGiveUp) {
        JSObject* ordinary = becomeOrdinary(vm);
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
    if (!layout().mayBeFrozen && !state().reportOrThrow(globalObject, { .kind = "object-freeze"_s, .on = &layout() }, makeString("Cannot freeze, seal or prevent extensions of "_s, state().nameOf(layout()), ": no TypeScript code of the program passes a value to Object.freeze, Object.seal or Object.preventExtensions as this type, so its objects cannot be frozen"_s), State::InAudit::CarriedOut))
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
    // OrdinarySetPrototypeOf.
    if (object->isNotExtensible())
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
    for (JSValue next = prototype; next.isObject() && asObject(next)->type() != ProxyObjectType; next = asObject(next)->getPrototypeDirect()) {
        if (next == JSValue(object))
            return typeError(globalObject, scope, shouldThrow, "cyclic __proto__ value"_s);
    }
    State& state = object->state();
    const LayoutData& layout = object->layout();
    // A record may cease to inherit, and inherit from what a record may be born with: see createThis(). Compiled code tells objects by the
    // shape word, which stays. What it takes for absent stays absent, and the names that Object.prototype or the `prototype` of a function
    // supplies it asks for: see mayInherit(). Any other parent could supply a name that the layout takes for absent, unless the layout
    // says that nothing is known of the chain. The methods of a class are called by the shape.
    if (layout.kind != LayoutKind::Record || !(state.isPrototypeByLayout(layout, prototype) || layout.mayHaveOtherPrototype)) {
        if (layout.mayGiveUp)
            RELEASE_AND_RETURN(scope, object->becomeOrdinary(vm)->methodTable()->setPrototype(cell, globalObject, prototype, shouldThrow));
        String message = makeString("Cannot set prototype of "_s, state.nameOf(layout), ": objects of a declared type keep their prototype"_s);
        State::Violation violation { .kind = "object-set-prototype"_s, .on = &layout };
        if (!state.data.isCompiledForAudit) {
            if (shouldThrow)
                state.noteViolation(vm, violation);
            return typeError(globalObject, scope, shouldThrow, withAOTErrorNote(message));
        }
        state.reportOrThrow(globalObject, violation, message, State::InAudit::CarriedOut);
    }
    if (prototype.isObject())
        asObject(prototype)->didBecomePrototype(vm);
    object->JSCell::setStructure(vm, state.structureFor(vm, layout.shape, prototype, !!object->extraProperties()));
    object->noteWhetherToAsk(state);
    return true;
}

void Object::setPrototypeAtBirth(VM& vm, JSObject* prototype)
{
    if (getPrototypeDirect() == JSValue(prototype))
        return;
    JSCell::setStructure(vm, state().structureFor(vm, layout().shape, prototype, !!extraProperties()));
    noteWhetherToAsk(state(), true);
}

void Object::specialize(VM& vm, State& state, uint32_t shape)
{
    bool hasRoom = specialize(vm, state, state.planOfTransition(vm, this, shape));
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
    // The collector goes by the Structure, and may be marking.
    WTF::storeStoreFence();
    JSCell::setStructure(vm, plan.structure);
    at<uint32_t>(offsetOfOrderWord) = (orderWord() & ~orderAndCountMask) | wordInSpecialization;
    at<uint32_t>(offsetOfShapeWord) = plan.shapeWord;
    state.countTransition(*plan.from, *plan.to);
    return true;
}

bool Object::specializeAsBefore(VM& vm, const TransitionPlan& plan)
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

Identifier EvalScope::keyOfNewTarget(VM& vm)
{
    static constexpr Latin1Character key[] = { 0, 't', 'a', 'r', 'g', 'e', 't' };
    return Identifier::fromString(vm, String(std::span<const Latin1Character> { key }));
}

bool EvalScope::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    JSObject* bindings = uncheckedDowncast<EvalScope>(object)->m_bindings.get();
    if (name == vm.propertyNames->builtinNames().newTargetLocalPrivateName())
        return bindings->methodTable()->getOwnPropertySlot(bindings, globalObject, keyOfNewTarget(vm), slot);
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
    // Whoever hands the captures over keeps them alive: both of these may collect garbage.
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
    // See createForCompiledCode().
    if (!closure->m_entry)
        return;
    visitor.appendValues(reinterpret_cast<const WriteBarrier<Unknown>*>(closure->captures().data()), closure->numberOfReferences());
}

DEFINE_VISIT_CHILDREN(Closure);

void Closure::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Base::analyzeHeap(cell, analyzer);
    Closure* closure = uncheckedDowncast<Closure>(cell);
    // See createForCompiledCode().
    if (!closure->m_entry)
        return;
    for (unsigned i = 0; i < closure->numberOfReferences(); ++i) {
        if (JSValue reference = JSValue::decode(closure->captures()[i]); reference && reference.isCell())
            analyzer.analyzeIndexEdge(cell, reference.asCell(), i);
    }
}

static void setNameReportedForFrames(uint32_t function, const String&, bool isOnlyClosure);

// What tells a closure that [[Call]] runs: see JSFunction::getCallDataInline(). Whoever goes by that enters at closureEntrypoint() by itself.
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
    // In the words of the engine: see ScriptExecutable::prepareForExecutionImpl().
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

// What the function is handed when it is constructed: a constructor that asks for it gets `new.target` first.
static void appendArgumentsOfConstruct(MarkedArgumentBuffer& all, const Closure* closure, JSValue newTarget, const ArgList& arguments)
{
    if (const LayoutData* ofClass = closure->classData(); ofClass && ofClass->constructorTakesNewTarget)
        all.append(newTarget);
    for (unsigned i = 0; i < arguments.size(); ++i)
        all.append(arguments.at(i));
}

JSC_DEFINE_HOST_FUNCTION(constructClosure, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* closure = uncheckedDowncast<Closure>(callFrame->jsCallee());
    JSObject* newTarget = asObject(callFrame->newTarget());
    JSValue thisValue = closure->structure()->aotInstance()->typed->createThis(globalObject, closure, newTarget);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    appendArgumentsOfConstruct(arguments, closure, newTarget, ArgList(callFrame));
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
    // Under a foreign root the constructor hands back what that has made.
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
    return !(structure()->aotInstance()->typed->kindAndDeclarationOf(m_function) & functionIsOfBuiltinModule);
}

bool PrototypeObject::letsItselfBeAltered(VM& vm)
{
    State& state = *structure()->aotInstance()->typed;
    if (!state.mayAlterPrototypeObjectOf(m_function))
        return false;
    if (!std::exchange(m_isAltered, true))
        state.didAlterPrototypeObject(vm, this);
    return true;
}

bool PrototypeObject::letsItselfBeReparented(VM& vm)
{
    return !structure()->aotInstance()->typed->makesObjectsWithDefinitelyInitializedFields(m_function) && letsItselfBeAltered(vm);
}

bool PrototypeObject::mayDefine(JSGlobalObject* globalObject, PropertyName name, bool isWritableData)
{
    State& state = *structure()->aotInstance()->typed;
    if (name.isSymbol() || !isGuarded() || !state.isReservedFieldName(globalObject->vm(), name.uid(), m_function))
        return true;
    // `this.x = x` in the function is [[Set]]: under a setter, or a property that cannot be stored over, the field would never be created. Compiled code stores to what it has
    // just made without asking, where some field of it is read without a test: isBornUnderAnyPrototype().
    if ((isWritableData || !state.makesObjectsWithDefinitelyInitializedFields(m_function)) && letsItselfBeAltered(globalObject->vm()))
        return true;
    return state.reportOrThrow(globalObject, { .kind = "function-prototype-write"_s, .onFunction = m_function, .member = name.uid() }, makeString("Cannot define property '"_s, StringView(name.uid()), "' on the prototype of function '"_s, state.nameOfFunction(m_function), "': its objects have a declared field of that name, and the compiler did not see this definition"_s), State::InAudit::CarriedOut);
}

bool PrototypeObject::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    // With another receiver nothing is defined here.
    if (slot.thisValue() == JSValue(cell) && !uncheckedDowncast<PrototypeObject>(cell)->mayDefine(globalObject, name, true))
        return false;
    bool result = Base::put(cell, globalObject, name, value, slot);
    // A cache would store to the next one without coming here.
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
    if (object->getPrototypeDirect() != prototype && uncheckedDowncast<PrototypeObject>(object)->isGuarded() && !uncheckedDowncast<PrototypeObject>(object)->letsItselfBeReparented(globalObject->vm()) && !state.reportOrThrow(globalObject, { .kind = "function-prototype-write"_s, .onFunction = uncheckedDowncast<PrototypeObject>(object)->m_function }, makeString("Cannot set the prototype of the 'prototype' object of function '"_s, state.nameOfFunction(uncheckedDowncast<PrototypeObject>(object)->m_function), "': TypeScript code in the program creates instances of it"_s), State::InAudit::CarriedOut))
        return false;
    return Base::setPrototype(object, globalObject, prototype, shouldThrowIfCantSet);
}

const ClassInfo ClassPrototype::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ClassPrototype) };

static_assert(sizeof(ClassPrototype) == sizeof(JSObjectWithButterfly) + ClassPrototype::inlineCapacity * sizeof(WriteBarrierBase<Unknown>));

Structure* ClassPrototype::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    Structure* structure = Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    // Every object of it is one: none makes a transition for that.
    structure->setMayBePrototype(true);
    return structure;
}

ClassPrototype* ClassPrototype::create(VM& vm, Structure* structure, uint32_t classIndex)
{
    ClassPrototype* object = new (NotNull, allocateCell<ClassPrototype>(vm)) ClassPrototype(vm, structure, classIndex);
    object->finishCreation(vm);
    return object;
}

void ClassPrototype::becomeOrdinary(VM& vm, Structure* ordinary)
{
    RELEASE_ASSERT(ordinary->classInfoForCells() == JSFinalObject::info() && ordinary->inlineCapacity() == inlineCapacity && ordinary->storedPrototype() == structure()->storedPrototype());
    m_classIndex = 0;
    m_numberOfDeclaredMemberLookups = 0;
    // A marker that sees the Structure takes the two words for values.
    WTF::storeStoreFence();
    if (butterfly())
        nukeStructureAndSetButterfly(vm, structureID(), nullptr);
    setStructure(vm, ordinary);
}

void ClassPrototype::makeMembers(JSObject* object)
{
    State& state = *object->structure()->aotInstance()->typed;
    // `constructor` is one of them.
    Closure* classObject = state.closure(object->vm(), state.classWithIndex(static_cast<uint32_t>(uncheckedDowncast<ClassPrototype>(object)->m_classIndex))->classData().classConstructor);
    state.makeClass(state.instance.globalObject, classObject, JSValue(), State::PartOfClass::MembersOfPrototype);
    RELEASE_ASSERT(object->type() == FinalObjectType);
}

bool ClassPrototype::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    // An inquiry while the heap is collected or walked: nothing can be made.
    if (object->vm().heap.currentThreadIsDoingGCWork()) [[unlikely]] {
        slot.setIsTaintedByOpaqueObject();
        return false;
    }
    if (auto answer = uncheckedDowncast<ClassPrototype>(object)->tryGetDeclaredMember(globalObject->vm(), name, slot))
        return *answer;
    makeMembers(object);
    return JSObject::getOwnPropertySlot(object, globalObject, name, slot);
}

std::optional<bool> ClassPrototype::tryGetDeclaredMember(VM& vm, PropertyName name, PropertySlot& slot)
{
    auto methodType = slot.internalMethodType();
    if ((methodType != PropertySlot::InternalMethodType::Get && methodType != PropertySlot::InternalMethodType::HasProperty) || butterfly() || !name.uid())
        return std::nullopt;
    State& state = *structure()->aotInstance()->typed;
    const LayoutData& ofClass = *state.classWithIndex(static_cast<uint32_t>(m_classIndex));
    if (m_numberOfDeclaredMemberLookups >= std::max<size_t>(4, ofClass.methods().size()))
        return std::nullopt;
    auto member = state.declaredMethod(vm, ofClass, name.uid());
    if (!member) {
        GetterSetter* accessor = state.declaredAccessor(vm, ofClass, name.uid());
        if (!accessor)
            return std::nullopt;
        ++m_numberOfDeclaredMemberLookups;
        state.didGetDeclaredMember = true;
        slot.setIsTaintedByOpaqueObject();
        slot.setGetterSlot(this, PropertyAttribute::Accessor | PropertyAttribute::DontEnum, accessor);
        return true;
    }
    ++m_numberOfDeclaredMemberLookups;
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
    State& state = *object->structure()->aotInstance()->typed;
    return state.declaresMethod(*state.classWithIndex(static_cast<uint32_t>(uncheckedDowncast<ClassPrototype>(object)->m_classIndex)), uid);
}

bool ClassPrototype::willBeImmutable(JSObject* object)
{
    State& state = *object->structure()->aotInstance()->typed;
    const LayoutData& ofClass = *state.classWithIndex(static_cast<uint32_t>(uncheckedDowncast<ClassPrototype>(object)->m_classIndex));
    return ofClass.isLaidOut && !ofClass.constructorMayBeReplaced && std::ranges::none_of(ofClass.methods(), [](auto& method) {
        return method.flags & methodMayBeReplaced;
    });
}

void ClassPrototype::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    makeMembers(object);
    JSObject::getOwnPropertyNames(object, globalObject, names, mode);
}

bool ClassPrototype::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    makeMembers(asObject(cell));
    return JSObject::put(cell, globalObject, name, value, slot);
}

bool ClassPrototype::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    makeMembers(asObject(cell));
    return JSObject::putByIndex(cell, globalObject, index, value, shouldThrow);
}

bool ClassPrototype::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    makeMembers(asObject(cell));
    return JSObject::deleteProperty(cell, globalObject, name, slot);
}

bool ClassPrototype::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    makeMembers(asObject(cell));
    return JSObject::deletePropertyByIndex(cell, globalObject, index);
}

bool ClassPrototype::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    makeMembers(object);
    return JSObject::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
}

bool ClassPrototype::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    makeMembers(object);
    return JSObject::preventExtensions(object, globalObject);
}

bool ClassPrototype::isExtensible(JSObject* object, JSGlobalObject* globalObject)
{
    makeMembers(object);
    return JSObject::isExtensible(object, globalObject);
}

bool ClassPrototype::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    makeMembers(object);
    return JSObject::setPrototype(object, globalObject, prototype, shouldThrowIfCantSet);
}

const ClassInfo ModuleNamespace::s_info = { "Module"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleNamespace) };

Structure* ModuleNamespace::createStructure(VM& vm, JSGlobalObject* globalObject, bool hasPrototypeOfHost)
{
    JSValue prototype = hasPrototypeOfHost ? globalObject->moduleNamespaceObjectStructure()->storedPrototype() : jsNull();
    return Structure::preventExtensionsTransition(vm, Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info()));
}

ModuleNamespace* ModuleNamespace::create(VM& vm, Structure* structure, JSObject* read, JSCellButterfly* names, JSObject* star, JSCellButterfly* namesOfStar)
{
    ModuleNamespace* object = new (NotNull, allocateCell<ModuleNamespace>(vm)) ModuleNamespace(vm, structure, read, names, star, namesOfStar);
    object->finishCreation(vm);
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
    visitor.append(thisObject->m_namesOfStar);
}

DEFINE_VISIT_CHILDREN(ModuleNamespace);

static unsigned placeAmong(JSCellButterfly* names, UniquedStringImpl* name)
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

unsigned ModuleNamespace::numberOf(UniquedStringImpl* name) const
{
    return placeAmong(m_names.get(), name);
}

bool ModuleNamespace::isBroughtByStar(UniquedStringImpl* name) const
{
    return m_namesOfStar && placeAmong(m_namesOfStar.get(), name) != noExport;
}

static JSC_DECLARE_HOST_FUNCTION(readExportOfDetachedNamespace);

JSC_DEFINE_HOST_FUNCTION(readExportOfDetachedNamespace, (JSGlobalObject* globalObject, CallFrame* callFrame))
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
    unsigned count = m_names->length();
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
    JSFunction* target = JSFunction::create(vm, globalObject, 1, emptyString(), readExportOfDetachedNamespace, ImplementationVisibility::Private);
    JSObject* detached = JSBoundFunction::create(vm, globalObject, target, values, { }, 1, nullptr, SourceTaintedOrigin::Untainted);
    if (scope.exception()) [[unlikely]]
        return;
    m_read.set(vm, this, detached);
}

JSValue ModuleNamespace::fixedProperty(VM& vm, PropertyName name) const
{
    if (name == vm.propertyNames->toStringTagSymbol)
        return jsNontrivialString(vm, "Module"_s);
    if (name == vm.propertyNames->__esModule && m_saysItIsAModule)
        return jsBoolean(true);
    return { };
}

bool ModuleNamespace::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    slot.setIsTaintedByOpaqueObject();
    unsigned number = thisObject->numberOf(name.uid());
    bool isBroughtByStar = number == noExport && thisObject->isBroughtByStar(name.uid());
    if (number == noExport && !isBroughtByStar) {
        JSValue fixed = thisObject->fixedProperty(vm, name);
        // Bun's namespace inherits its marker: whoever asks for the object's own properties does not find it.
        if (!fixed || (name == vm.propertyNames->__esModule && slot.internalMethodType() == PropertySlot::InternalMethodType::GetOwnProperty))
            return false;
        slot.setValue(thisObject, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete, fixed);
        return true;
    }
    switch (slot.internalMethodType()) {
    case PropertySlot::InternalMethodType::GetOwnProperty:
    case PropertySlot::InternalMethodType::Get: {
        if (isBroughtByStar) {
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
    // It has the export, whether or not that can be read yet.
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
    JSCellButterfly* own = thisObject->m_names.get();
    JSCellButterfly* ofStar = thisObject->m_namesOfStar.get();
    unsigned numberOfStar = ofStar ? ofStar->length() : 0;
    for (unsigned number = 0, place = 0; number < own->length() || place < numberOfStar;) {
        bool isOwn = place == numberOfStar || (number < own->length() && codePointCompare(StringView(*asString(own->get(number))->tryGetValueImpl()), StringView(*asString(ofStar->get(place))->tryGetValueImpl())) < 0);
        Identifier name = asString(isOwn ? own->get(number++) : ofStar->get(place++))->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (mode == DontEnumPropertiesMode::Exclude) {
            // Whoever asks which are enumerable asks for each: that throws in the dead zone.
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
    if (auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell); name == vm.propertyNames->__esModule && thisObject->numberOf(name.uid()) == noExport && !thisObject->isBroughtByStar(name.uid())) {
        thisObject->m_saysItIsAModule = value.toBoolean(globalObject);
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
    return thisObject->numberOf(name.uid()) == noExport && !thisObject->isBroughtByStar(name.uid()) && (name == vm.propertyNames->__esModule || !thisObject->fixedProperty(vm, name));
}

bool ModuleNamespace::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    Identifier name = Identifier::from(globalObject->vm(), index);
    return thisObject->numberOf(name.impl()) == noExport && !thisObject->isBroughtByStar(name.impl());
}

bool ModuleNamespace::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // What `require` of the module does.
    if (auto* thisObject = uncheckedDowncast<ModuleNamespace>(object); name == vm.propertyNames->__esModule && descriptor.value().isTrue() && thisObject->numberOf(name.uid()) == noExport && !thisObject->isBroughtByStar(name.uid())) {
        thisObject->m_saysItIsAModule = true;
        return true;
    }
    PropertyDescriptor current;
    bool isCurrentDefined = object->getOwnPropertyDescriptor(globalObject, name, current);
    RETURN_IF_EXCEPTION(scope, false);
    // Nothing is applied. An export is described as writable, and yet takes no other value.
    bool isValid = validateAndApplyPropertyDescriptor(globalObject, nullptr, name, false, descriptor, isCurrentDefined, current, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isValid || (uncheckedDowncast<ModuleNamespace>(object)->numberOf(name.uid()) == noExport && !uncheckedDowncast<ModuleNamespace>(object)->isBroughtByStar(name.uid())))
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

Generator* Generator::create(VM& vm, Structure* structure, Closure* resumeClosure)
{
    Generator* generator = new (NotNull, allocateCell<Generator>(vm)) Generator(vm, structure, resumeClosure);
    generator->finishCreation(vm);
    return generator;
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
        instance.spaceOfSavedFrames = &vm.cellSpace();
        instance.headerOfSavedFrames = static_cast<uint64_t>(m_savedFrameStructure->typeInfoBlob()) << 32 | m_savedFrameStructure->id().bits();
    }
    return m_savedFrameStructure;
}

SavedFrame* SavedFrame::ofFrame(const Instance& instance, const uint64_t* frame)
{
    if (std::bit_cast<const void*>(frame[1]) != instance.returnAddressOfSavedFrames)
        return nullptr;
    return uncheckedDowncast<SavedFrame>(std::bit_cast<CallFrame*>(frame[0])->jsCallee());
}

SavedFrame* SavedFrame::save(VM& vm, Instance& instance, uint64_t* frame, const uint64_t* bottom, void* resumeAddress, const uint64_t* extra)
{
    size_t numberOfWords = frame - bottom;
    SavedFrame* saved = ofFrame(instance, frame);
    if (!saved) {
        Structure* structure = instance.typed->savedFrameStructure(vm);
        saved = new (NotNull, allocateCell<SavedFrame>(vm, allocationSize(numberOfWords))) SavedFrame(vm, structure, static_cast<uint32_t>(numberOfWords));
        saved->finishCreation(vm);
        uint32_t place = static_cast<uint32_t>(extra[0]) >> shiftOfPlaceOfPromise;
        uint64_t promise = place == placeInRegister ? extra[1] : frame[-static_cast<intptr_t>(place)];
        saved->m_promise = uncheckedDowncast<JSPromise>(std::bit_cast<JSCell*>(promise));
    }
    RELEASE_ASSERT(saved->m_numberOfWords == numberOfWords && !saved->m_rowPlusOne);
    saved->willWrite();
    uint64_t* words = saved->words();
    for (size_t i = 0; i < numberOfWords; ++i)
        words[i] = bottom[i];
    saved->m_register = extra[1];
    saved->m_resumeAddress = resumeAddress;
    saved->m_rowPlusOne = (static_cast<uint32_t>(extra[0]) & ((1u << bitsOfRow) - 1)) + 1;
    saved->didWrite();
    vm.writeBarrier(saved);
    return saved;
}

template<typename Visitor>
void SavedFrame::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    auto* saved = uncheckedDowncast<SavedFrame>(cell);
    visitor.appendUnbarriered(saved->m_promise);
    uint32_t version = saved->m_version;
    WTF::loadLoadFence();
    Vector<JSCell*, 32> cells;
    bool isInKnownFunction = true;
    if (uint32_t rowPlusOne = saved->m_rowPlusOne; rowPlusOne && !(version & 1)) {
        ImageAddressInfo where = classifyAddress(static_cast<const uint8_t*>(saved->m_resumeAddress) - 1);
        isInKnownFunction = where.kind == ImageAddressInfo::Function;
        if (where.kind == ImageAddressInfo::Function) {
            const uint32_t* row = saved->structure()->aotInstance()->infos[where.index].function()->plans() + rowPlusOne - 1;
            for (uint32_t i = 0; i < row[0]; ++i) {
                for (uint32_t bits = row[1 + i]; bits; bits &= bits - 1) {
                    uint32_t place = i * 32 + ctz(bits);
                    uint64_t word = place == placeInRegister ? saved->m_register : saved->words()[saved->m_numberOfWords - place];
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

// Runs the coroutine until it suspends or completes. Whoever asks has entered the VM. Empty: it has thrown, or the function yields nothing.
static JSValue resumeCoroutine(JSGlobalObject* globalObject, Closure* resumeClosure, int32_t mode, JSValue value, JSCell* context = nullptr)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // Its entry is the resume function itself: see Stub::EnterResume. Nothing else can call it.
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

bool Generator::iteratesAsMade(JSGlobalObject* globalObject)
{
    // Neither it nor the `prototype` of its function has a property.
    JSObject* prototype = getPrototypeDirect().getObject();
    if (structure()->maxOffset() != invalidOffset || !prototype || prototype->type() != FinalObjectType || prototype->structure()->maxOffset() != invalidOffset || !prototype->structure()->hasImmutableProperties())
        return false;
    JSObject* shared = globalObject->generatorPrototype();
    // What those two have cannot be changed where a compiled program runs: see Instance::ensure() and installGeneratorMethods().
    return prototype->getPrototypeDirect() == JSValue(shared) && shared->getPrototypeDirect() == JSValue(globalObject->iteratorPrototype());
}

int32_t& Generator::state() const
{
    Object* frame = uncheckedDowncast<Object>(m_resumeClosure->environment());
    if (!m_offsetOfState) [[unlikely]]
        m_offsetOfState = frame->layout().describedVariables()[CoroutineFrame::stateField].offset;
    return frame->at<int32_t>(m_offsetOfState);
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
    // A generator that has not started completes without running: there is no `try` around its first instruction.
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
    return generator;
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
}

DEFINE_VISIT_CHILDREN(AsyncGenerator);

static ALWAYS_INLINE bool nextJobRunsAtOnce(JSGlobalObject* globalObject, VM& vm)
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

void AsyncGenerator::appendTargetsOfRequests(Vector<JSCell*, 4>& targets) const
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

bool AsyncGenerator::iteratesAsMade(JSGlobalObject* globalObject)
{
    JSObject* prototype = getPrototypeDirect().getObject();
    return structure()->maxOffset() == invalidOffset && prototype && prototype->type() == FinalObjectType && prototype->structure()->maxOffset() == invalidOffset && prototype->getPrototypeDirect() == JSValue(globalObject->asyncGeneratorPrototype()) && globalObject->asyncGeneratorPrototype()->getPrototypeDirect() == JSValue(globalObject->asyncIteratorPrototype());
}

JSValue AsyncGenerator::enqueueDriver(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    if (m_target.pointer())
        return enqueue(globalObject, Generator::ResumeMode::Next, value);
    static_assert(!static_cast<uint16_t>(Generator::ResumeMode::Next));
    uint16_t flags = m_target.type();
    m_value.set(vm, this, value);
    if (!(flags & isBusyFlag) && state() != CoroutineFrame::completedState && vm.isSafeToRecurseSoft()) [[likely]] {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        m_target = { this, static_cast<uint16_t>((flags & ~(modeMask | isSuspendingAtAwaitFlag | isEnteredByJobLoopFlag)) | isDrainingAheadOfDriverFlag | isBusyFlag) };
        JSValue result = enterResumeFunction(vm, m_resumeClosure, static_cast<int32_t>(Generator::ResumeMode::Next), value);
        flags = m_target.type();
        if (!scope.exception() && flags & isSuspendingAtAwaitFlag && m_target.pointer() == this) [[likely]] {
            m_target.setType(flags & ~isDrainingAheadOfDriverFlag);
            return this;
        }
        finishRun(globalObject, result, nullptr);
    } else
        m_target = { this, static_cast<uint16_t>((flags & ~modeMask) | isDrainingAheadOfDriverFlag) };
    if (!is(isBusyFlag) && m_target.pointer()) [[unlikely]]
        drain(globalObject);
    set(isDrainingAheadOfDriverFlag, false);
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
    bool resumesAtOnce = atOnce && !atOnce->driver && nextJobRunsAtOnce(globalObject, vm) && vm.isSafeToRecurseSoft();
    if (isRejected) {
        if (resumesAtOnce)
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
    if (resumesAtOnce)
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
    JSValue result = resumeCoroutine(globalObject, m_resumeClosure, mode, value);
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

void AsyncGenerator::resumeFromAwait(JSGlobalObject* globalObject, JSValue resolution, bool isRejected, bool isEnteredByJobLoop)
{
    DriverToResume driverToResume;
    DriverToResume* atOnce = isEnteredByJobLoop ? &driverToResume : nullptr;
    VM& vm = globalObject->vm();
    if (uint16_t flags = m_target.type(); isEnteredByJobLoop && !isRejected && !(flags & (isAwaitingReturnFlag | isAwaitingYieldOperandFlag)) && vm.isSafeToRecurseSoft()) [[likely]] {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        m_target.setType((flags & ~isSuspendingAtAwaitFlag) | isBusyFlag | isEnteredByJobLoopFlag);
        JSValue result = enterResumeFunction(vm, m_resumeClosure, 0, resolution);
        flags = m_target.type();
        if (!scope.exception()) [[likely]] {
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

static JSObject* methodOfGenerators(JSGlobalObject* globalObject, JSObject* prototype, const Identifier& name, unsigned& attributes)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    // It makes a property of what is in the static table of the class.
    JSValue found = prototype->get(globalObject, name);
    scope.assertNoException();
    RELEASE_ASSERT(found.isCallable() && prototype->getDirect(vm, name, attributes) == found);
    return asObject(found);
}

// next(), return() or throw() for the generators of the program. Whatever else it is called on is for the function it takes the place of.
template<Generator::ResumeMode mode>
static void installGeneratorMethod(JSGlobalObject* globalObject, JSObject* prototype, const Identifier& name)
{
    VM& vm = globalObject->vm();
    unsigned attributes;
    JSObject* original = methodOfGenerators(globalObject, prototype, name, attributes);
    if (original->inherits<JSNativeStdFunction>())
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
static ALWAYS_INLINE EncodedJSValue callMethodOfAsyncGenerators(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* generator = dynamicDowncast<AsyncGenerator>(callFrame->thisValue());
    if (!generator) [[unlikely]]
        RELEASE_AND_RETURN(scope, original(globalObject, callFrame));
    RELEASE_AND_RETURN(scope, JSValue::encode(generator->enqueue(globalObject, mode, callFrame->argument(0))));
}

static JSC_DECLARE_HOST_FUNCTION(nextOfAsyncGenerators);
static JSC_DECLARE_HOST_FUNCTION(returnOfAsyncGenerators);
static JSC_DECLARE_HOST_FUNCTION(throwOfAsyncGenerators);

JSC_DEFINE_HOST_FUNCTION(nextOfAsyncGenerators, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return callMethodOfAsyncGenerators<Generator::ResumeMode::Next, asyncGeneratorPrototypeNext>(globalObject, callFrame);
}

JSC_DEFINE_HOST_FUNCTION(returnOfAsyncGenerators, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return callMethodOfAsyncGenerators<Generator::ResumeMode::Return, asyncGeneratorPrototypeReturn>(globalObject, callFrame);
}

JSC_DEFINE_HOST_FUNCTION(throwOfAsyncGenerators, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return callMethodOfAsyncGenerators<Generator::ResumeMode::Throw, asyncGeneratorPrototypeThrow>(globalObject, callFrame);
}

static void installMethodOfAsyncGenerators(JSGlobalObject* globalObject, JSObject* prototype, const Identifier& name, NativeFunction function)
{
    VM& vm = globalObject->vm();
    unsigned attributes;
    auto* original = uncheckedDowncast<JSFunction>(methodOfGenerators(globalObject, prototype, name, attributes));
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
    installMethodOfAsyncGenerators(globalObject, prototype, names.next, nextOfAsyncGenerators);
    installMethodOfAsyncGenerators(globalObject, prototype, names.returnKeyword, returnOfAsyncGenerators);
    installMethodOfAsyncGenerators(globalObject, prototype, names.throwKeyword, throwOfAsyncGenerators);
}

uint32_t State::resumeFunctionOf(uint32_t generatorFunction) const
{
    return resumeFunctionAndFlagOf(generatorFunction) & ~Data::generatorTakesItsClosure;
}

bool State::takesItsClosure(uint32_t generatorFunction) const
{
    return resumeFunctionAndFlagOf(generatorFunction) & Data::generatorTakesItsClosure;
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

void State::setPrototypeOfGenerators(VM& vm, uint32_t resumeFunction, bool isAsync, JSObject* prototype)
{
    JSGlobalObject* globalObject = instance.globalObject;
    Structure* structure = isAsync ? AsyncGenerator::createStructure(vm, globalObject, prototype) : Generator::createStructure(vm, globalObject, prototype);
    m_generatorStructures.set(resumeFunction, structure);
    didCreate(structure);
}

Structure* State::generatorStructure(VM& vm, uint32_t resumeFunction, bool isAsync)
{
    if (!m_generatorStructures.get(resumeFunction)) {
        JSGlobalObject* globalObject = instance.globalObject;
        JSObject* prototype = constructEmptyObject(globalObject, isAsync ? static_cast<JSObject*>(globalObject->asyncGeneratorPrototype()) : globalObject->generatorPrototype());
        prototype->fixAndMakePropertiesImmutable(vm);
        setPrototypeOfGenerators(vm, resumeFunction, isAsync, prototype);
    }
    return m_generatorStructures.get(resumeFunction);
}

Structure* State::generatorStructure(VM& vm, Closure* generatorFunction, bool isAsync)
{
    if (HeapVersion version = vm.heap.objectSpace().newlyAllocatedVersion(); version != m_versionOfGeneratorStructures) [[unlikely]] {
        m_generatorStructuresOfClosures.fill({ });
        m_versionOfGeneratorStructures = version;
    }
    auto& known = m_generatorStructuresOfClosures[(std::bit_cast<uintptr_t>(generatorFunction) >> 4) % m_generatorStructuresOfClosures.size()];
    if (known.closure == generatorFunction && known.epoch == instance.typedPrototypeEpoch) [[likely]]
        return known.structure;
    JSGlobalObject* globalObject = instance.globalObject;
    Structure*& ofRealm = m_generatorStructuresOfRealm[isAsync];
    if (!ofRealm) {
        ofRealm = isAsync ? AsyncGenerator::createStructure(vm, globalObject, globalObject->asyncGeneratorPrototype()) : Generator::createStructure(vm, globalObject, globalObject->generatorPrototype());
        didCreate(ofRealm);
    }
    // OrdinaryCreateFromConstructor, with what the realm has for a `prototype` that is no object.
    JSValue prototype = generatorFunction->ensurePrototypeProperty(vm) ? generatorFunction->getDirect(vm, vm.propertyNames->prototype) : JSValue();
    Structure* structure = prototype && prototype.isObject() ? globalObject->structureCache().emptyStructureForPrototypeFromBaseStructure(globalObject, asObject(prototype), ofRealm) : ofRealm;
    known = { generatorFunction, structure, instance.typedPrototypeEpoch };
    return structure;
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

static JSC_DECLARE_CUSTOM_GETTER(getLiveBindingOfHolder);

// It is handed the object that has the property, and the name: so one does for every property of every such object. What the object is a view of is its first property.
JSC_DEFINE_CUSTOM_GETTER(getLiveBindingOfHolder, (JSGlobalObject* globalObject, EncodedJSValue holder, PropertyName name))
{
    return JSValue::encode(asObject(JSValue::decode(holder))->getDirect(0).get(globalObject, name));
}

CustomGetterSetter* State::liveBindingAccessor(VM& vm)
{
    if (!m_liveBindingAccessor)
        m_liveBindingAccessor = CustomGetterSetter::create(vm, getLiveBindingOfHolder, nullptr);
    return m_liveBindingAccessor;
}

bool tryToLeaveWaiterToBeResumedAtOnce(Instance& instance, const uint64_t* frame, JSPromise* promise, JSValue resolution)
{
    if (instance.waiterToResume || instance.awaitMustSuspend || !SavedFrame::ofFrame(instance, frame))
        return false;
    if (!(std::bit_cast<CallFrame*>(frame[0])->uncheckedArgument(0).asInt32() & CoroutineFrame::isEnteredByJobLoop))
        return false;
    if (resolution.isObject() && !isDefinitelyNonThenable(asObject(resolution), instance.globalObject))
        return false;
    if (promise->inlineReactionKind() != JSPromise::InlineReactionKind::InternalMicrotask)
        return false;
    JSValue waiter = promise->inlineReactionContext();
    if (!waiter || !waiter.isCell() || waiter.asCell()->structureID() != std::bit_cast<StructureID>(static_cast<uint32_t>(instance.headerOfSavedFrames)))
        return false;
    auto job = promise->tryFulfillTakingInlineJob(*instance.vm, resolution);
    if (!job)
        return false;
    RELEASE_ASSERT(job->task == InternalMicrotask::AOTAsyncFunctionResume);
    instance.waiterToResume = waiter.asCell();
    instance.resolutionForWaiter = JSValue::encode(resolution);
    instance.asyncContextOfWaiter = JSValue::encode(job->asyncContext);
    return true;
}

static NEVER_INLINE void resumeWaitersLeftBehind(JSGlobalObject* globalObject, VM& vm, Instance& instance)
{
    if (instance.isResumingWaiters)
        return;
    instance.isResumingWaiters = true;
    while (JSCell* waiter = std::exchange(instance.waiterToResume, nullptr)) {
        if (vm.hasPendingTerminationException()) [[unlikely]]
            break;
        AsyncContextSwapScope asyncContextScope(vm, globalObject, JSValue::decode(instance.asyncContextOfWaiter));
        resumeAsyncFunction(globalObject, vm, waiter, JSValue::decode(instance.resolutionForWaiter), false, true);
    }
    instance.isResumingWaiters = false;
}

void resumeAsyncFunction(JSGlobalObject* globalObject, VM& vm, JSValue context, JSValue resolution, bool isRejected, bool isEnteredByJobLoop)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (context.asCell()->type() != JSFunctionType) {
        auto* saved = uncheckedDowncast<SavedFrame>(context);
        if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
            saved->willBeLaidOnStack();
            saved->promise()->reject(vm, createStackOverflowError(globalObject));
            return;
        }
        saved->willBeLaidOnStack();
        Instance& instance = *saved->structure()->aotInstance();
        JSCell* enteredOutside = std::exchange(instance.savedFrameEntered, saved);
        vmEntryToJavaScriptWith2Arguments(savedFrameEntrypoint().taggedPtr(), &vm, nullptr, saved, jsUndefined(), saved, jsNumber(isRejected | (isEnteredByJobLoop ? CoroutineFrame::isEnteredByJobLoop | SavedFrame::nothingIsLeftBehindIt : 0)), resolution);
        saved = uncheckedDowncast<SavedFrame>(std::exchange(instance.savedFrameEntered, enteredOutside));
        if (Exception* exception = scope.exception()) [[unlikely]] {
            JSValue error = exception->value();
            if (scope.clearExceptionExceptTermination())
                saved->promise()->reject(vm, error);
        }
        if (instance.waiterToResume) [[unlikely]]
            resumeWaitersLeftBehind(globalObject, vm, instance);
        return;
    }
    auto* resumeClosure = uncheckedDowncast<Closure>(context);
    // Whoever runs jobs has entered the VM.
    resumeCoroutine(globalObject, resumeClosure, isRejected | (isEnteredByJobLoop ? CoroutineFrame::isEnteredByJobLoop : 0), resolution, resumeClosure);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        JSValue error = exception->value();
        if (!scope.clearExceptionExceptTermination())
            return;
        auto* frame = uncheckedDowncast<Object>(resumeClosure->environment());
        auto& promise = frame->layout().describedVariables()[CoroutineFrame::promiseField];
        uncheckedDowncast<JSPromise>(readAs(promise.store(), reinterpret_cast<const uint8_t*>(frame) + promise.offset))->reject(vm, error);
    }
}

struct SuspendedAsyncFunction {
    JSObject* resumeClosure { nullptr };
    JSPromise* promise { nullptr };
    int32_t state { 0 };
    bool isHiddenInStackTraces { false };
    bool isSavedFrame { false };
    std::tuple<uint32_t, uint32_t, BytecodeIndex> placeOfSavedFrame { };

    static std::tuple<uint32_t, uint32_t, BytecodeIndex> placeOfResumeAddress(Instance* instance, const void* address)
    {
        using Place = Instance::PlaceInTrace;
        const Instance::CachedAddressInfo* asked = instance->resolveReturnAddress(address);
        RELEASE_ASSERT(asked);
        Place place = instance->placesInTraces()[asked - instance->cachedReturnAddresses].ofSite();
        if (place.kind == Place::IsShown || place.kind == Place::IsHidden) [[likely]]
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

    std::tuple<uint32_t, uint32_t, BytecodeIndex> placeInTrace(Instance* instance, uintptr_t code) const
    {
        if (isSavedFrame)
            return placeOfSavedFrame;
        auto* closure = uncheckedDowncast<Closure>(resumeClosure);
        return { FunctionRef::at(instance, std::bit_cast<const void*>(ClosureWord::addressOf(closure->entry(), code))).index, closure->functionIndex(), BytecodeIndex(state) };
    }
};

static const Data& dataInImage();

static ALWAYS_INLINE JSCell* cellKeptIn(const VariableData& variable, const JSCell* environment)
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
        return word <= undefinedCell32 ? nullptr : MarkedBlockCage::decode(word);
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
        SuspendedAsyncFunction result { saved, saved->promise(), 0, false, true, SuspendedAsyncFunction::placeOfResumeAddress(saved->structure()->aotInstance(), saved->resumeAddress()) };
        const Data& image = dataInImage();
        uint8_t kindByte = image.at<uint8_t>(image.functionKindsOffset)[std::get<1>(result.placeOfSavedFrame)];
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
    if ((kindByte & functionKindMask) == functionKindOfConstructors)
        return { };
    if (!image.at<std::array<uint8_t, 2>>(image.closureCountsOffset)[ClosureWord::functionOf(entry)][0])
        return { };
    auto* environment = std::bit_cast<JSCell*>(*reinterpret_cast<const uint64_t*>(closure + 1));
    if (!environment)
        return { };
    Structure* structure = environment->structure();
    const Data* data = structure->dataOfCompiledEnvironments();
    if (!data)
        return { };
    auto variables = layoutOfEnvironment(*data, structure->firstShapeOfCompiledEnvironments(), environment).describedVariables();
    if (variables.size() <= CoroutineFrame::resumeClosureField || variables[CoroutineFrame::stateField].store() != Store::Int32)
        return { };
    if (cellKeptIn(variables[CoroutineFrame::resumeClosureField], environment) != closure)
        return { };
    JSCell* driver = cellKeptIn(variables[CoroutineFrame::promiseField], environment);
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
    auto [code, reported, callSite] = function.placeInTrace(instance, std::bit_cast<uintptr_t>(Image::withCode()->code()));
    results.constructAndAppend(vm, owner, tokenOf(instance), code, reported, callSite, true);
}

bool isResumeClosureOfAsyncFunction(JSCell* cell)
{
    return savedFrameIn(cell) || suspendedAsyncFunctionIn(cell).resumeClosure;
}

JSPromise* promiseOfAsyncFunction(JSCell* resumeClosure)
{
    if (auto* saved = savedFrameIn(resumeClosure))
        return saved->promise();
    return suspendedAsyncFunctionIn(resumeClosure).promise;
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
        else if (JSPromise* own = promiseOfAsyncFunction(current))
            promises.append(own);
        else if (auto* generator = current->type() == ObjectType ? dynamicDowncast<AsyncGenerator>(current) : nullptr; generator && numberOfGenerators++ < maxGenerators)
            generator->appendTargetsOfRequests(work);
    }
    return promises;
}

Vector<JSPromise*, 4> promisesSettledBehind(JSPromise* promise)
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
    constexpr uint16_t isAwaitedByOneOfOurs = static_cast<uint16_t>(JSPromise::Status::Pending)
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
        uint16_t flags = current.promise->flags();
        if ((flags & asked) != isAwaitedByOneOfOurs || results.size() >= maxStackSize) {
            bool isAwaitedByNobody = (flags & JSPromise::stateMask) != static_cast<uint16_t>(JSPromise::Status::Pending) || (!(flags & JSPromise::inlineReactionKindMask) && !current.promise->payloadCell());
            return leave(isAwaitedByNobody ? nullptr : current.resumeClosure);
        }
        JSValue context = current.promise->inlineReactionContext();
        if (!context || !context.isCell())
            return leave(current.resumeClosure);
        SuspendedAsyncFunction caller = suspendedAsyncFunctionIn(context.asCell());
        if (!caller.resumeClosure)
            return leave(current.resumeClosure);
        current = caller;
        if (Instance* itsInstance = current.resumeClosure->structure()->aotInstance(); itsInstance != instance) {
            if (token)
                vm.writeBarrier(owner, token);
            instance = itsInstance;
            token = tokenOf(instance);
            code = std::bit_cast<uintptr_t>(Image::withCode()->code());
        }
        if (!current.isHiddenInStackTraces) {
            auto [inImage, reported, callSite] = current.placeInTrace(instance, code);
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

static TextOfFunctionInFile s_textOfFunctionInFile;
static JSString* textOfFunction(JSGlobalObject*, uint32_t function);

JSValue Closure::toString(JSGlobalObject* globalObject) const
{
    // As the engine has it for its bound functions, which are native: under the name of the function that is bound.
    if (kind() == Format::FunctionKind::Bound)
        return jsMakeNontrivialString(globalObject, "function "_s, structure()->aotInstance()->typed->nameOfFunction(functionIndex()).substring("bound "_s.length()), "() { [native code] }"_s);
    ASCIILiteral async = isDeclaredAs(functionIsAsync) ? "async "_s : ""_s;
    bool isOfProgram = !isDeclaredAs(functionIsOfBuiltinModule);
    if (JSString* text = isOfProgram ? textOfFunction(globalObject, functionIndex()) : nullptr)
        return text;
    if (isOfProgram && kind() == Format::FunctionKind::Arrow)
        return jsMakeNontrivialString(globalObject, async, "() => { [native code] }"_s);
    // What an arrow function is called it has from where it is written, and its text would not say.
    String name = kind() == Format::FunctionKind::Arrow ? emptyString() : structure()->aotInstance()->typed->nameOfFunction(functionIndex());
    if (kind() == Format::FunctionKind::Constructor)
        return jsMakeNontrivialString(globalObject, "class "_s, name, name.isEmpty() ? ""_s : " "_s, "{ [native code] }"_s);
    if (isOfProgram && kind() == Format::FunctionKind::Method)
        return jsMakeNontrivialString(globalObject, async, isDeclaredAs(functionIsGenerator) ? "*"_s : ""_s, name, "() { [native code] }"_s);
    return jsMakeNontrivialString(globalObject, async, isDeclaredAs(functionIsGenerator) ? "function* "_s : "function "_s, name, "() { [native code] }"_s);
}

static const Data& dataInImage();

// By the image alone: a builtin function is asked these while its realm is being created, when it has no Instance.
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

bool Closure::makesLaidOutObjects() const
{
    return kind() == Format::FunctionKind::Plain && !isDeclaredAs(functionIsAsync | functionIsGenerator) && structure()->aotInstance()->typed->makesLaidOutObjects(functionIndex());
}

bool Closure::keepsPrototypeProperty() const
{
    // What is defined later on an ordinary object nobody sees. The checker sees `.prototype` replaced wherever that is written down, and then no field of the objects is read without a test.
    State& state = *structure()->aotInstance()->typed;
    return makesLaidOutObjects() && (!state.mayAlterPrototypeObjectOf(functionIndex()) || state.makesObjectsWithDefinitelyInitializedFields(functionIndex()));
}

bool Closure::sharesPrototypeProperty() const
{
    Instance* instance = structure()->aotInstance();
    return isDeclaredAs(functionIsGenerator) && instance->typed->closureIfMade(functionIndex()) != this && !instance->typed->takesItsClosure(functionIndex());
}

bool closureLacksWhatItsStructureLacks(VM& vm, JSCell* cell, UniquedStringImpl* uid, bool isReceiver)
{
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

static JSC_DECLARE_CUSTOM_GETTER(prototypePropertyOfClosure);

JSC_DEFINE_CUSTOM_GETTER(prototypePropertyOfClosure, (JSGlobalObject* globalObject, EncodedJSValue encodedClosure, PropertyName name))
{
    JSCell* cell = JSValue::decode(encodedClosure).asCell();
    if (cell->type() == SlimClosureType) {
        auto* closure = uncheckedDowncast<SlimClosure>(cell);
        return JSValue::encode(closure->canConstruct() ? closure->get(globalObject, name) : jsUndefined());
    }
    JSValue prototype = uncheckedDowncast<Closure>(cell)->prototypeProperty(globalObject->vm());
    return JSValue::encode(prototype ? prototype : jsUndefined());
}

bool closureAnswersForPrototypeProperty(JSCell* cell)
{
    return cell->type() == SlimClosureType || (cell->type() == JSFunctionType && cell->classInfo() == Closure::info());
}

GetValueFunc getterOfPrototypePropertyOfClosures()
{
    return prototypePropertyOfClosure;
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
    appendArgumentsOfConstruct(all, this, newTarget, arguments);
    if (all.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    NewTargetScope newTargetScope(this, newTarget);
    ConstructionScope constructionScope(vm, *structure()->aotInstance()->typed);
    // Not what getCallData() says: [[Call]] of a class throws.
    CallData callData;
    callData.type = CallData::Type::JS;
    callData.js.functionExecutable = nullptr;
    callData.js.scope = this->scope();
    RELEASE_AND_RETURN(scope, call(globalObject, this, callData, thisValue, all));
}

bool Closure::isUnmadeClassObject() const
{
    // The usual closure is no class object, and the usual class object that is asked has been made.
    if (kind() != Format::FunctionKind::Constructor || getDirect(vm(), vm().propertyNames->prototype))
        return false;
    const LayoutData* ofClass = classData();
    if (!ofClass)
        return false;
    State& state = *structure()->aotInstance()->typed;
    // What extends something foreign is nothing at all until its declaration has been evaluated, which makes the prototype.
    return state.makesClassObjectWhenChanged(*ofClass) && (state.hasPrototypeAheadOfClassObject(*ofClass) || state.prototypeOfClassIfExists(ofClass->index));
}

static NEVER_INLINE void makeClassObject(Closure* classObject)
{
    State& state = *classObject->structure()->aotInstance()->typed;
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(classObject->vm()).countOperation("ClassObject", "made");
    state.makeClass(state.instance.globalObject, classObject, JSValue(), State::PartOfClass::ClassObject);
}

void Closure::takeStructureOfBoundFunction(VM& vm)
{
    State& state = *structure()->aotInstance()->typed;
    if (structure() == state.closureStructure(vm, functionIndex()))
        setStructure(vm, state.structureOfBoundClosures(vm, functionIndex()));
}

void Closure::ensureClassObject(VM& vm)
{
    if (kind() == Format::FunctionKind::Bound) [[unlikely]] {
        takeStructureOfBoundFunction(vm);
        return;
    }
    if (!isUnmadeClassObject())
        return;
    makeClassObject(this);
}

JSValue Closure::prototypeProperty(VM& vm)
{
    if (JSValue stored = getDirect(vm, vm.propertyNames->prototype))
        return stored;
    if (isUnmadeClassObject())
        return structure()->aotInstance()->typed->prototypeOfClass(vm, classData()->index);
    ensurePrototypeProperty(vm);
    return getDirect(vm, vm.propertyNames->prototype);
}

static bool isBornWithPrototypeProperty(const Closure& closure)
{
    if (closure.kind() == Format::FunctionKind::Constructor)
        return true;
    // As a function of the engine's or the host's own code, which the host's modules are: it has none until it is given one, and that is an
    // ordinary property. But objects that are laid out are born from a PrototypeObject.
    if (closure.isDeclaredAs(functionIsOfBuiltinModule) && !closure.structure()->aotInstance()->typed->hasGuardedPrototype(closure.functionIndex()))
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
    if (kind() == Format::FunctionKind::Constructor || !isBornWithPrototypeProperty(*this))
        return false;
    State& state = *structure()->aotInstance()->typed;
    JSObject* prototype;
    if (isDeclaredAs(functionIsGenerator)) {
        // What the generators it makes inherit from.
        JSGlobalObject* globalObject = state.instance.globalObject;
        if (state.takesItsClosure(functionIndex())) {
            prototype = constructEmptyObject(globalObject, isDeclaredAs(functionIsAsync) ? static_cast<JSObject*>(globalObject->asyncGeneratorPrototype()) : globalObject->generatorPrototype());
            prototype->fixAndMakePropertiesImmutable(vm);
        } else
            prototype = state.generatorStructure(vm, state.resumeFunctionOf(functionIndex()), isDeclaredAs(functionIsAsync))->storedPrototypeObject();
    } else {
        // What `new` on an ordinary function gives its objects.
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
    // What is stored comes first, also what a host has stored with putDirect() under a name that is made a property only when it is asked for.
    if (JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot))
        return true;
    State& state = *object->structure()->aotInstance()->typed;
    if (const LayoutData* ofClass = closure->classData()) {
        // Whoever looks for what it has not leaves it as it is.
        if (closure->isUnmadeClassObject()) {
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
        closure->takeStructureOfBoundFunction(vm);
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
        structure->setMayBePrototype(true);
        return structure;
    }
    Structure* structure = Structure::create(vm, instance.globalObject, prototype, TypeInfo(SlimClosureType, StructureFlags), info(), NonArray | MayHaveIndexedAccessors, 0);
    structure->setAOTInstance(&instance);
    structure->setMayBePrototype(true);
    structure->setHasImmutableShape(true);
    return structure;
}

SlimClosure* SlimClosure::create(VM& vm, Structure* structure, uint32_t function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    FunctionTables& tables = FunctionTables::of(vm);
    RELEASE_ASSERT(ProgramData::get()->closureWordOf(function) && structure->aotInstance()->typed->makesSlimClosures(function));
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
    if (visitor.mutatorIsStopped()) {
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
    if (!slimConstructor || allocationOfThis(slimConstructor->functionIndex()) || newTarget->structure()->aotInstance() != &instance)
        return nullptr;
    unsigned attributes = 0;
    JSValue prototype;
    uint32_t functionOfTarget;
    if (auto* target = dynamicDowncast<SlimClosure>(newTarget)) {
        if (!target->canConstruct())
            return nullptr;
        target->reifyProperties(vm);
        prototype = target->getDirect(vm, vm.propertyNames->prototype, attributes);
        functionOfTarget = target->functionIndex();
    } else if (auto* target = dynamicDowncast<Closure>(newTarget)) {
        prototype = target->getDirect(vm, vm.propertyNames->prototype, attributes);
        if (!prototype)
            prototype = target->prototypeProperty(vm);
        functionOfTarget = target->functionIndex();
    } else
        return nullptr;
    if (!prototype || attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return nullptr;
    JSObject* parent = prototype.isObject() ? asObject(prototype) : globalObject->objectPrototype();
    unsigned capacity = m_inlineCapacitiesForThis.ensure(functionOfTarget, [&] {
        return inlineCapacityForThis(slimConstructor->functionIndex(), functionOfTarget, inlineCapacity);
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
    uint8_t kind = kindAndDeclaration();
    if (static_cast<Format::FunctionKind>(kind & functionKindMask) == Format::FunctionKind::Bound)
        return jsMakeNontrivialString(globalObject, "function "_s, nameOfFunction().substring("bound "_s.length()), "() { [native code] }"_s);
    ASCIILiteral async = kind & functionIsAsync ? "async "_s : ""_s;
    bool isOfProgram = !(kind & functionIsOfBuiltinModule);
    if (JSString* text = isOfProgram ? textOfFunction(globalObject, functionIndex()) : nullptr)
        return text;
    if (isOfProgram && static_cast<Format::FunctionKind>(kind & functionKindMask) == Format::FunctionKind::Arrow)
        return jsMakeNontrivialString(globalObject, async, "() => { [native code] }"_s);
    if (isOfProgram && static_cast<Format::FunctionKind>(kind & functionKindMask) == Format::FunctionKind::Method)
        return jsMakeNontrivialString(globalObject, async, nameOfFunction(), "() { [native code] }"_s);
    String name = static_cast<Format::FunctionKind>(kind & functionKindMask) == Format::FunctionKind::Arrow ? emptyString() : nameOfFunction();
    return jsMakeNontrivialString(globalObject, async, "function "_s, name, "() { [native code] }"_s);
}

} // namespace Typed

void setTextOfFunctionInFile(TextOfFunctionInFile function)
{
    Typed::s_textOfFunctionInFile = function;
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
    JSValue values[] = { jsNumber(function), jsNumber(state.data.lengthOfFunction(function)), state.string(vm, state.data.at<uint32_t>(state.data.functionsOffset)[function]), JSValue() };
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

static String computedNameOfFunction(JSGlobalObject* globalObject, const Identifier& key, Format::MethodKind kind)
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
    String name = computedNameOfFunction(globalObject(), key, kind);
    RETURN_IF_EXCEPTION(scope, void());
    reifyProperties(vm);
    putDirect(vm, vm.propertyNames->name, jsString(vm, name), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
}

static JSObject* targetOfBoundFunction(VM& vm, State& state, uint32_t function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    const Data& data = state.data;
    std::span all { data.at<std::array<uint32_t, 3>>(data.boundFunctionsOffset), data.numberOfBoundFunctions };
    auto found = std::ranges::lower_bound(all, function, { }, [](auto& entry) {
        return entry[0];
    });
    RELEASE_ASSERT(found != all.end() && (*found)[0] == function);
    if (!((*found)[2] & boundFunctionHoldsItsTarget))
        return state.closureObjectFor(vm, (*found)[1]);
    RELEASE_ASSERT(numberOfReferences && numberOfReferences <= captures.size());
    return std::bit_cast<JSObject*>(captures[numberOfReferences - 1]);
}

bool Closure::customHasInstance(JSObject* object, JSGlobalObject* globalObject, JSValue value)
{
    Closure* closure = uncheckedDowncast<Closure>(object);
    return targetOfBoundFunction(globalObject->vm(), *object->structure()->aotInstance()->typed, closure->functionIndex(), closure->captures(), closure->numberOfReferences())->hasInstance(globalObject, value);
}

bool SlimClosure::customHasInstance(JSObject* object, JSGlobalObject* globalObject, JSValue value)
{
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(object);
    return targetOfBoundFunction(globalObject->vm(), closure->state(), closure->functionIndex(), closure->captures(), closure->numberOfReferences())->hasInstance(globalObject, value);
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
                slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, jsNumber(state.data.lengthOfFunction(closure->functionIndex())));
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
    bool wasAsMade = !closure->isReified();
    closure->reifyProperties(globalObject->vm());
    bool result = Base::put(cell, globalObject, name, value, slot);
    bool isPrototype = name == globalObject->vm().propertyNames->prototype;
    if (wasAsMade || isPrototype)
        slot.disableCaching();
    if (isPrototype) [[unlikely]]
        ++closure->state().instance.typedPrototypeEpoch;
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
    bool wasAsMade = !closure->isReified();
    closure->reifyProperties(globalObject->vm());
    bool isDeleted = Base::deleteProperty(cell, globalObject, name, slot);
    if (wasAsMade)
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
        ++uncheckedDowncast<SlimClosure>(object)->state().instance.typedPrototypeEpoch;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool result = Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(object);
    if (result && name == vm.propertyNames->name && descriptor.value() && descriptor.value().isString()) [[unlikely]] {
        String reported = asString(descriptor.value())->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        setNameReportedForFrames(closure->functionIndex(), reported, closure->state().closureIfMade(closure->functionIndex()) == closure);
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

static uint32_t kindsStoredAsGiven(const RuntimeType& type)
{
    uint32_t kinds = 0;
    if (type.primitives & Primitive::Undefined)
        kinds |= SoundTypeUndefined;
    if (type.primitives & Primitive::Null)
        kinds |= SoundTypeNull;
    if ((type.primitives & (Primitive::False | Primitive::True)) == (Primitive::False | Primitive::True))
        kinds |= SoundTypeBoolean;
    if (type.primitives & Primitive::Double && type.primitives & Primitive::Int && type.minInteger() <= -maxKeptInteger && type.maxInteger() >= maxKeptInteger)
        kinds |= SoundTypeNumber;
    if (type.primitives & Primitive::String)
        kinds |= SoundTypeString;
    if (type.allowsAnyObject() && !type.numberOfViews())
        kinds |= SoundTypeAnyObject;
    return kinds;
}

static JSValue holdToTypeOfStaticField(JSGlobalObject* globalObject, State& state, const LayoutData& ofClass, const FieldData& field, PropertyName name, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String failure;
    JSValue held = state.holdToTypeOf(globalObject, field, value, failure);
    RETURN_IF_EXCEPTION(scope, { });
    if (!held)
        state.reportOrThrow(globalObject, { .kind = "class-static-write"_s, .on = &ofClass, .member = name.uid(), .expected = &state.ownTypeOf(field), .got = value }, state.assignmentMessage(vm, describeStaticField(state, ofClass, name), field, value, failure), State::InAudit::Ignored);
    return held;
}

// Compiled code that calls a generator function by itself has no closure at hand to ask for its `prototype`. So the closures of such a function
// share theirs, and only where there is one closure can it be replaced. Not where every call goes through a closure: State::takesItsClosure().
bool Closure::mayReplacePrototypeProperty(JSGlobalObject* globalObject, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    bool isGenerator = isDeclaredAs(functionIsGenerator);
    if (!isGenerator && !keepsPrototypeProperty())
        return true;
    ensurePrototypeProperty(vm);
    if (getDirect(vm, vm.propertyNames->prototype) == value)
        return true;
    State& state = *structure()->aotInstance()->typed;
    if (isGenerator && !shouldThrow && !state.data.isCompiledForAudit)
        return false;
    return state.reportOrThrow(globalObject, { .kind = isGenerator ? "generator-prototype-write"_s : "function-prototype-write"_s, .onFunction = functionIndex() }, makeString("Cannot replace 'prototype' of "_s, isGenerator ? "generator function '"_s : "function '"_s, state.nameOfFunction(functionIndex()), isGenerator ? "': it is fixed when the program is compiled"_s : "': TypeScript code of the program creates its objects"_s), State::InAudit::CarriedOut);
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
    if (field) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        value = holdToTypeOfStaticField(globalObject, state, *ofClass, *field, name, value);
        RETURN_IF_EXCEPTION(scope, false);
        if (!value)
            return false;
    }
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
        uint32_t kinds = mayBeCached ? kindsStoredAsGiven(state.ownTypeOf(*field)) : 0;
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
        // A cache would store to the next closure without coming here.
        slot.disableCaching();
        state.didChangePrototypeOf(vm, closure);
    }
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
        JSValue held = holdToTypeOfStaticField(globalObject, state, *ofClass, *field, name, descriptor.value());
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
    // The host hides frames of its own modules by the name of the function. A frame has no closure to ask: most calls pass none.
    if (result && name == vm.propertyNames->name && descriptor.value() && descriptor.value().isString()) [[unlikely]] {
        String reported = asString(descriptor.value())->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        setNameReportedForFrames(closure->functionIndex(), reported, closure->isDeclaredAs(functionIsOfBuiltinModule) || state.closureIfMade(closure->functionIndex()) == closure);
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

// Elements need storage, and a Structure that says so.
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
    // `super(..)` in the constructor of a class that extends another goes by the declaration.
    if (!ofClass || !ofClass->hasFixedMembers || object->getPrototypeDirect() == prototype || (ofClass->base() == LayoutData::noBase && !ofClass->hasForeignRoot)) {
        // Where that is allowed to change, compiled code asks: see CallResolution::resolveConstructSuper().
        if (ofClass && object->getPrototypeDirect() != prototype)
            object->structure()->aotInstance()->typedAlteredClassObjects[ofClass->index] = 1;
        RELEASE_AND_RETURN(scope, Base::setPrototype(object, globalObject, prototype, shouldThrow));
    }
    State& state = *object->structure()->aotInstance()->typed;
    String message = makeString("Cannot set prototype of class "_s, state.nameOf(*ofClass), ": a class keeps its superclass"_s);
    State::Violation violation { .kind = "class-static-write"_s, .on = ofClass };
    if (state.data.isCompiledForAudit)
        RELEASE_AND_RETURN(scope, state.reportOrThrow(globalObject, violation, message, State::InAudit::Ignored));
    if (shouldThrow)
        state.noteViolation(vm, violation);
    return typeError(globalObject, scope, shouldThrow, withAOTErrorNote(message));
}

// `length`, `name` and `prototype` are made when they are asked for.
void Closure::getOwnSpecialPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    closure->ensureClassObject(vm);
    Base::getOwnSpecialPropertyNames(object, globalObject, names, mode);
    if (mode == DontEnumPropertiesMode::Include && isBornWithPrototypeProperty(*closure) && closure->ensurePrototypeProperty(vm))
        names.add(vm.propertyNames->prototype);
}

static size_t sizeOfZeroedTable(size_t count, size_t size)
{
    return roundUpToMultipleOf(pageSize(), std::max<size_t>(count, 1) * size);
}

template<typename T>
static T* createZeroedTable(size_t count)
{
    return static_cast<T*>(OSAllocator::reserveAndCommit(sizeOfZeroedTable(count, sizeof(T))));
}

template<typename T>
static void destroyZeroedTable(T* table, size_t count)
{
    OSAllocator::decommitAndRelease(table, sizeOfZeroedTable(count, sizeof(T)));
}

static size_t sizeOfAllocationRows(const Data& data)
{
    return sizeOfZeroedTable(std::min(data.numberOfAllocations, State::maxNumberOfAllocationRows), sizeof(Instance::TypedAllocation));
}

static size_t sizeOfAllocationRowsAndNumbers(const Data& data)
{
    return sizeOfAllocationRows(data) + sizeOfZeroedTable(data.numberOfAllocations, sizeof(uint16_t));
}

// A page.
static constexpr uint32_t initialCapacityOfStringTable = 2048;

static uint64_t* createStringTable(uint32_t capacity)
{
    uint64_t* slots = createZeroedTable<uint64_t>(capacity);
    slots[0] = static_cast<uint64_t>(capacity - 1) << 32 | UINT32_MAX;
    return slots;
}

static uint32_t capacityOfStringTable(const uint64_t* slots)
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

    // By the number of the AllocationData.
    Vector<Allocation> allocations;
    // By shape and cell size.
    CountsByPair objectsWithoutAllocation;
    // By the shape before and the shape after.
    CountsByPair transitions;
    // By shape and order word.
    CountsByPair orderStates;
    // By shape, then by field. Empty until a field of the layout is created.
    Vector<Vector<uint64_t>> fieldCreations;
    // By Extension::size().
    UncheckedKeyHashMap<uint32_t, uint64_t> extensions;
    // By the size before, which is that of the empty one for the first, and the size after.
    CountsByPair extensionGrowth;
    // By function and number of captures.
    CountsByPair closures;
    // By function, and kind and value: see State::countPlainAllocation().
    CountsByPair plainAllocations;
    CountsByPair bytesOfPlainCells;
    CountsByPair bytesOfPlainStorage;
    // By what countArrayGet() makes of the read, and countArrayPut() of the store.
    UncheckedKeyHashMap<String, uint64_t> arrayGets;
    // By what countCoercion() makes of the cast, and the type of each as text.
    UncheckedKeyHashMap<String, uint64_t> coercions;
    UncheckedKeyHashMap<String, String> typesOfCoercions;
    UncheckedKeyHashMap<String, uint64_t> arrayPuts;
    // The arrays that a store has made ArrayStorage of.
    UncheckedKeyHashMap<String, uint64_t> arraysLeft;
    // By the event: tables, and their entries.
    UncheckedKeyHashMap<String, std::pair<uint64_t, uint64_t>> compactTables;
    // By shape.
    Vector<uint64_t> extraPropertyTables;
    // By shape, then by the name of the property.
    Vector<UncheckedKeyHashMap<String, uint64_t>> extraProperties;
    enum class Place : uint8_t { Field, AbsentField, Extra, NotOwn, OtherReceiver };
    struct Access {
        uint64_t count { 0 };
        RefPtr<UniquedStringImpl> name;
    };
    // By return address; AccessKind and Place; the shape of the receiver, or the ClassInfo of a cell that is no object of the program.
    UncheckedKeyHashMap<std::tuple<uintptr_t, uint32_t, uintptr_t>, Access> accesses;

    struct Collection {
        bool isFull;
        size_t size;
        size_t capacity;
        size_t extraMemorySize;
        size_t blocks;
        size_t bytesInPreciseAllocations;
    };
    // From 1, so that no key is all zeros.
    enum class LiveKind : uint32_t { Layout = 1, Closure, Extension, ExtraProperties, StorageOfExtraProperties, Class, StorageOfClass, OtherAuxiliary };
    struct FreeCells {
        uint64_t cells { 0 };
        uint64_t blocks { 0 };
        uint64_t emptyBlocks { 0 };
    };
    struct LiveCells {
        size_t collection { 0 };
        // By LiveKind; the LayoutData or the ClassInfo, or zero; the cell size.
        UncheckedKeyHashMap<std::tuple<uint32_t, uintptr_t, size_t>, uint64_t> cells;
        // By Subspace and cell size.
        UncheckedKeyHashMap<std::pair<Subspace*, size_t>, FreeCells> freeCells;
        uint64_t extraPropertyTables { 0 };
        // Each StringImpl once. The bytes are malloc memory. Those of a static or external one, and the characters of a substring, are not counted.
        uint64_t strings { 0 };
        uint64_t bytesOfStrings { 0 };
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
        // 0 to 16 as they are, above that by the power of two that bounds the number.
        static constexpr unsigned numberOfBuckets = 50;
        static unsigned bucketOf(uint64_t number) { return number <= 16 ? number : 12 + WTF::fastLog2(number - 1) + 1; }
        static uint64_t boundOf(unsigned bucket) { return bucket <= 16 ? bucket : 1ull << (bucket - 12); }
        UncheckedKeyHashMap<const LayoutData*, Slots> slots;
        CountsByPair orderStates;
        UncheckedKeyHashMap<const LayoutData*, Vector<Field>> fields;
        std::array<std::array<uint64_t, numberOfBuckets>, numberOfBuckets> arrays { };
        std::array<uint64_t, numberOfBuckets> maps { };
        std::array<uint64_t, numberOfBuckets> sets { };
        uint64_t arraysHeldOnce { 0 };
        uint64_t arraysHeldByMore { 0 };
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
    // The collector's thread writes these three, whoever prints reads them.
    Lock collectionsLock;
    Vector<Collection> collections;
    LiveCells liveCells;
    std::unique_ptr<Observer> observer;
};

// Held by whoever looks at the list.
static Lock statesWithCountsLock;
static Vector<State*>& statesWithCounts()
{
    static NeverDestroyed<Vector<State*>> states;
    return states;
}

void State::fillStatics(uint32_t firstWord, uint32_t endWord)
{
    for (auto& run : std::span { data.at<std::array<uint32_t, 4>>(data.staticsRunsOffset), data.numberOfStaticsRuns }) {
        if (run[0] >= firstWord && run[0] < endWord)
            std::ranges::fill(std::span { reinterpret_cast<uint64_t*>(instance.typedStatics) + run[0], run[1] }, run[2] | static_cast<uint64_t>(run[3]) << 32);
    }
}

void State::createStatics()
{
    RELEASE_ASSERT(data.numberOfCards <= Instance::roundedUpToGranule(data.sizeOfStatics) >> Instance::typedCardShift);
    instance.typedCards = reinterpret_cast<uint8_t*>(&instance) + Instance::displacementOfTypedCards(data.sizeOfStatics);
    instance.typedStatics = reinterpret_cast<uint8_t*>(&instance) + Instance::displacementOfTypedStatics(data.sizeOfStatics);
    fillStatics(data.sizeOfStaticsFilledLate / sizeof(uint64_t), std::numeric_limits<uint32_t>::max());
    m_sizeOfStaticsWithoutAccess = static_cast<uint32_t>(WTF::roundDownToMultipleOf(pageSize(), static_cast<size_t>(data.sizeOfStaticsFilledLate)));
    if (m_sizeOfStaticsWithoutAccess)
        OSAllocator::protect(instance.typedStatics, m_sizeOfStaticsWithoutAccess, false, false);
}

void State::fillStaticsOfProgram()
{
    if (m_sizeOfStaticsWithoutAccess)
        OSAllocator::protect(instance.typedStatics, m_sizeOfStaticsWithoutAccess, true, true);
    fillStatics(0, data.sizeOfStaticsFilledLate / sizeof(uint64_t));
    WTF::storeStoreFence();
    m_sizeOfStaticsWithoutAccess = 0;
    m_areStaticsOfProgramFilled = true;
}

void State::destroyStatics()
{
    if (m_sizeOfStaticsWithoutAccess)
        OSAllocator::protect(instance.typedStatics, m_sizeOfStaticsWithoutAccess, true, true);
}

State::State(VM& vm, Instance& instance, const Data& data)
    : instance(instance)
    , data(data)
    , extraProperties(vm)
    , numberBoxes(vm)
    , fieldAttributes(vm)
    , m_structuresWithOtherPrototype(vm)
{
    if (!data.usesWideReferences)
        MarkedBlockCage::require();
    structures = createZeroedTable<uint32_t>(data.numberOfLayouts);
    m_cachedFields = createZeroedTable<CachedField>(numberOfCachedFields);
    m_cachedFieldNames = createZeroedTable<CachedFieldName>(numberOfCachedFieldNames);
    m_cachedTransitions = createZeroedTable<CachedTransition>(numberOfCachedTransitions);
    m_transitionsMadeBefore = createZeroedTable<TransitionMadeBefore>(numberOfTransitionsMadeBefore);
    m_castTransitions = createZeroedTable<CastTransition>(1u << CastTransition::bitsOfPlace);
    instance.typedCastTransitions = m_castTransitions;
    m_cachedStores = createZeroedTable<CachedStore>(numberOfCachedStores);
    instance.typedKnownStoreTransitions = createZeroedTable<KnownStoreTransition>(1 << KnownStoreTransition::bitsOfPlace);
    extraProperties.recentlyRead = createZeroedTable<ExtraPropertiesOfObject>(1 << ExtraPropertiesOfObject::bitsOfPlace);
    instance.typedExtraPropertiesOfObjects = extraProperties.recentlyRead;
    instance.typedPlacesOfExtraProperties = createZeroedTable<PlaceOfExtraProperty>(1 << PlaceOfExtraProperty::bitsOfPlace);
    m_typesAskedAboutProperties = createZeroedTable<uintptr_t>(numberOfTypesAskedAboutProperties);
    m_propertiesWithViews = createZeroedTable<PropertiesWithViews>(numberOfPropertiesWithViews);
    // Here, and not where the program is entered: the host evaluates modules of its own before that.
    createStatics();
    instance.typedAllocationRows = reinterpret_cast<uint16_t*>(createZeroedTable<uint8_t>(sizeOfAllocationRowsAndNumbers(data)) + sizeOfAllocationRows(data));
    instance.typedAlteredClassObjects = createZeroedTable<uint8_t>(data.numberOfLayoutIndices);
    instance.typedShapeSets = data.at<uint32_t>(data.shapeSetsOffset);
    instance.typedStrings = createStringTable(initialCapacityOfStringTable);
    instance.typedData = data.at<uint8_t>(0);
    instance.rows = createZeroedTable<Slot>(data.numberOfRows);
    instance.objectPrototypeOfRealm = instance.globalObject->objectPrototype();
    instance.firstRowOfThisPlusOne = data.firstNumberOfRowsOfThis + 1;
    instance.displacementsOfNames = data.at<uint32_t>(data.displacementsOfNamesOffset);
    instance.placesByNameAndRow = data.at<uint32_t>(data.placesByNameAndRowOffset);
    instance.typedFunctions = FunctionTables::of(vm).words;
    for (unsigned kind = 0; kind < Instance::numberOfTypedExecutables; ++kind)
        instance.typedExecutables[kind] = FunctionTables::of(vm).executables[kind].get();
    instance.typedClosures = createZeroedTable<uint8_t>(data.numberOfFunctions * (data.usesWideReferences ? sizeof(JSCell*) : sizeof(uint32_t)));
    instance.typedBuiltins = createZeroedTable<JSCell*>(Builtins::numberOfCells());
    isOfLockedRealm = instance.globalObject->hasImmutableIntrinsics();
    if (isOfLockedRealm) {
        for (unsigned index : Builtins::singleton().cellsFilledFromStart())
            instance.typedBuiltins[index] = Builtins::singleton().findCell(instance.globalObject, index);
    }
    instance.typedOrderKeys = createZeroedTable<uintptr_t*>(data.numberOfLayouts);
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
    // The collector does not visit this State before the Instance is registered, and is held off until then: see Instance::ensure().
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
    destroyZeroedTable(m_transitionsMadeBefore, numberOfTransitionsMadeBefore);
    destroyZeroedTable(m_castTransitions, 1u << CastTransition::bitsOfPlace);
    destroyZeroedTable(m_cachedStores, numberOfCachedStores);
    destroyZeroedTable(static_cast<KnownStoreTransition*>(instance.typedKnownStoreTransitions), 1 << KnownStoreTransition::bitsOfPlace);
    destroyZeroedTable(extraProperties.recentlyRead, 1 << ExtraPropertiesOfObject::bitsOfPlace);
    destroyZeroedTable(static_cast<PlaceOfExtraProperty*>(instance.typedPlacesOfExtraProperties), 1 << PlaceOfExtraProperty::bitsOfPlace);
    destroyZeroedTable(m_typesAskedAboutProperties, numberOfTypesAskedAboutProperties);
    destroyZeroedTable(m_propertiesWithViews, numberOfPropertiesWithViews);
    fastFree(m_youngRoots);
    destroyStatics();
    destroyZeroedTable(reinterpret_cast<uint8_t*>(instance.typedAllocationRows) - sizeOfAllocationRows(data), sizeOfAllocationRowsAndNumbers(data));
    destroyZeroedTable(instance.typedAlteredClassObjects, data.numberOfLayoutIndices);
    destroyZeroedTable(instance.typedStrings, capacityOfStringTable(instance.typedStrings));
    for (uint32_t capacity = initialCapacityOfStringTable; uint64_t* former : m_formerStringTables) {
        destroyZeroedTable(former, capacity);
        capacity *= 2;
    }
    destroyZeroedTable(static_cast<uint8_t*>(instance.typedClosures), data.numberOfFunctions * (data.usesWideReferences ? sizeof(JSCell*) : sizeof(uint32_t)));
    destroyZeroedTable(instance.typedBuiltins, Builtins::numberOfCells());
    destroyZeroedTable(instance.typedOrderKeys, data.numberOfLayouts);
    destroyZeroedTable(instance.rows, data.numberOfRows);
    destroyZeroedTable(instance.typedKeyLists, Instance::numberOfTypedKeyLists);
}

} // namespace Typed

bool becomesOrdinaryObject(const ClassInfo* from, const ClassInfo* to)
{
    return ((from == Typed::ClassPrototype::info() || from == Typed::Object::info()) && to == JSFinalObject::info()) || (from == JSFinalObject::info() && to == Typed::Object::info());
}

namespace Typed {

JSObject* State::importMetaOfModule(JSGlobalObject* globalObject, JSString* path)
{
    String text = path->value(globalObject);
    for (auto& [known, meta] : m_importMetaOfModules) {
        if (known == text)
            return meta;
    }
    JSObject* meta = globalObject->moduleLoader()->createImportMetaProperties(globalObject, path, uncheckedDowncast<JSModuleRecord>(moduleRecord), nullptr);
    if (!meta)
        return nullptr;
    didCreate(meta);
    m_importMetaOfModules.append({ WTF::move(text), meta });
    return meta;
}

void State::didCreate(JSCell* cell)
{
    Locker locker { m_cellsLock };
    m_cells.append(cell);
}

void State::captureBuiltinsOfRealm()
{
    VM& vm = *instance.vm;
    auto& builtins = Builtins::singleton();
    if (Instance* first = instance.globalObject->aotInstance(); first != &instance) {
        memcpySpan(std::span { instance.typedBuiltins, Builtins::numberOfCells() }, std::span { first->typedBuiltins, Builtins::numberOfCells() });
        return;
    }
    for (unsigned index = 0; index < Builtins::numberOfCells(); ++index) {
        if (index >= builtins.cellOfOriginalStructure(0) || builtins.isCellOfReplaceableGlobal(index)) {
            instance.typedBuiltins[index] = vm.structureStructure.get();
            continue;
        }
        if (!builtins.hasCellAt(index))
            continue;
        instance.typedBuiltins[index] = builtins.captureCell(instance.globalObject, nullptr, index);
        didCreate(instance.typedBuiltins[index]);
    }
}

void State::captureBuiltinsOf(ASCIILiteral global, JSObject* object)
{
    auto& builtins = Builtins::singleton();
    for (unsigned index : builtins.cellsOfReplaceableGlobals()) {
        if (builtins.at(BuiltinID(index)).holder != global)
            continue;
        instance.typedBuiltins[index] = builtins.captureCell(instance.globalObject, object, index);
        didCreate(instance.typedBuiltins[index]);
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

// By the image alone: there may be no State yet.
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

static const AllocationData* allocationOfThis(const Data& data, uint32_t function)
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
            functionsHiddenWithWhatTheyCall.append(builtin[1]);
    }
    // A new one each time, whatever the name: there is no JIT, so the VM shares none.
    RELEASE_ASSERT(!Options::useJIT());
    for (unsigned kind = 0; kind < executables.size(); ++kind) {
        bool isClass = kind == 4;
        // Private: where the engine comes in through the host function, the frame of that is not the function's. The compiled frame is.
        auto visibility = !isClass && kind & 1 ? ImplementationVisibility::PrivateRecursive : ImplementationVisibility::Private;
        NativeExecutable* executable = vm.getHostFunction(isClass ? callClassConstructor : callClosure, visibility, isClass || kind & 2 ? constructClosure : callHostFunctionAsConstructor, 0, String());
        executable->setIsSharedByAOTTypedClosures();
        // Interpreted code that calls it goes there without a native frame in between.
        if (!isClass)
            executable->setCallEntrypoint(closureEntrypoint());
        executables[kind] = Strong<JSCell>(vm, executable);
    }
}

NativeExecutable* FunctionTables::executableOf(uint32_t function) const
{
    return uncheckedDowncast<NativeExecutable>(executables[kindOfExecutable(data.at<uint8_t>(data.functionKindsOffset)[function], functionsHiddenWithWhatTheyCall.contains(function))].get());
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
    // As createThis() and constructClosure() go about it. Closures go by the generic entrypoint of a constructor, which is what is listed.
    std::span constructors { data.at<std::array<uint32_t, 2>>(data.constructorsOffset), data.numberOfConstructors };
    bool isListed = std::ranges::binary_search(constructors, function, { }, [](auto& constructorAndClass) {
        return constructorAndClass[0];
    });
    const LayoutData* ofClass = isListed ? classConstructedBy(data, function) : nullptr;
    if (ofClass ? !ofClass->isLaidOut && !ofClass->hasForeignRoot && !ofClass->constructorTakesNewTarget : constructs && kind == Format::FunctionKind::Plain && !allocationOfThis(data, function))
        word |= 1ULL << ClosureWord::constructsOrdinaryObjectBit;
    if (ofClass && !ofClass->isLaidOut && ofClass->hasForeignRoot && ofClass->constructorTakesNewTarget)
        word |= 1ULL << ClosureWord::isDerivedConstructorBit;
    RELEASE_ASSERT((word >> ClosureWord::isDerivedConstructorBit & 3) != 3);
    return word;
}

static Structure* closureStructureOfRealm(VM& vm, JSGlobalObject* globalObject, unsigned which)
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
    case closureStructureOfClassObjects:
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
    RELEASE_ASSERT(closureStructureOf(data.at<uint8_t>(data.functionKindsOffset)[function]) != closureStructureOfClassObjects);
    Closure* closure = Closure::create(vm, closureStructureOfRealm(vm, globalObject, closureStructureOf(data.at<uint8_t>(data.functionKindsOffset)[function])), function);
    if (prototype)
        closure->setPrototypeDirect(vm, prototype);
    return closure;
}

void didFinishCreation(JSGlobalObject* globalObject)
{
    VMProgram* program = globalObject->vm().m_aotProgram.get();
    if (!program || !program->typedFunctionTables)
        return;
    // Nothing has called one of them so far. What does needs the Instance.
    for (auto& closure : program->typedFunctionTables->closuresWithoutInstance) {
        if (closure && closure->structure()->realm() == globalObject) {
            Instance::ensure(globalObject);
            return;
        }
    }
}

void State::adoptClosuresOfRealm(VM& vm)
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
        // The one it has by now, if it has been given properties or another prototype, and those it went through: another closure may go the same way.
        for (Structure* structure = closure->structure(); structure && !structure->aotInstance(); structure = structure->previousID())
            structure->setAOTInstance(&instance);
        return true;
    });

}

static std::optional<uint32_t> entrypointOfBuiltinModule(uint32_t id)
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
    return !!entrypointOfBuiltinModule(id);
}

void willAskProgramForFile(JSGlobalObject* globalObject, const String& path)
{
    State& state = *Instance::ensure(globalObject).typed;
    if (!state.moduleRecord && state.pathOfFileAskedFor.isNull())
        state.pathOfFileAskedFor = path;
}

Identifier State::keyOfImporter(VM& vm) const
{
    if (moduleRecord) [[likely]]
        return moduleRecord->moduleKey();
    return pathOfFileAskedFor.isNull() ? vm.propertyNames->emptyIdentifier : Identifier::fromString(vm, pathOfFileAskedFor);
}

JSValue evaluateBuiltinModule(JSGlobalObject* globalObject, uint32_t id)
{
    constexpr uint32_t filesOfProgramByPath = 0x7fffffff;
    if (id == filesOfProgramByPath && !globalObject->hasImmutableIntrinsics()) [[unlikely]]
        return jsUndefined();
    auto entrypoint = entrypointOfBuiltinModule(id);
    RELEASE_ASSERT(entrypoint);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // That of the default loader of the global object, whichever loader asks.
    State& state = *Instance::ensure(globalObject).typed;
    using Outcome = State::BuiltinModule::Outcome;
    if (auto earlier = state.m_builtinModules.find(id); earlier != state.m_builtinModules.end()) [[unlikely]] {
        switch (earlier->value.outcome) {
        case Outcome::Exports:
            return JSValue::decode(earlier->value.value);
        case Outcome::Thrown:
            throwException(globalObject, scope, JSValue::decode(earlier->value.value));
            return { };
        // A cycle at load: run again and again, that is how it would end.
        case Outcome::None:
            throwStackOverflowError(globalObject, scope);
            return { };
        }
    }
    // What fails for want of stack would not fail the next time. It is not begun.
    constexpr size_t stackToStartWith = 128 * KB;
    if (std::bit_cast<uintptr_t>(currentStackPointer()) - std::bit_cast<uintptr_t>(vm.softStackLimit()) < stackToStartWith || !vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return { };
    }
    JSObject* module = state.closureObjectFor(vm, *entrypoint);
    state.m_builtinModules.add(id, State::BuiltinModule { });
    JSValue exports = call(globalObject, module, JSC::getCallData(module), jsUndefined(), ArgList());
    Exception* exception = scope.exception();
    // The end of the thread is no outcome of the module.
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
    // What is not registered no native code can spell. The registry holds no reference, and lives as long as the VM: so does the symbol.
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
        instance->typed->captureBuiltinsOf(name, object);
}

// UTF-8, but that half of a surrogate pair on its own, which a string of the language may have, is encoded like any other code unit.
static String textOf(std::span<const uint8_t> bytes)
{
    if (charactersAreAllASCII(byteCast<Latin1Character>(bytes)))
        return StringImpl::createWithoutCopying(byteCast<Latin1Character>(bytes));
    if (String text = String::fromUTF8(bytes); !text.isNull())
        return text;
    return String(decodeTextOfProgram(bytes).span());
}

// The characters of an ASCII text stay where they are, in the image: an atom may outlive the State (a property table, a JSString holds it), so an image is never unmapped.
static String textOfConstant(const Data&, uint32_t index);

static String atomOf(const Data& data, uint32_t index)
{
    if (uint32_t facts = data.stringFacts(index); facts & Data::stringIsASCII) {
        auto characters = byteCast<Latin1Character>(data.stringInImage(index));
        WTF::HashTranslatorCharBuffer<Latin1Character> buffer { characters, facts & Data::stringHasIdentifierNumber ? data.hashOfIdentifier(facts & Data::stringHashMask) : facts & Data::stringHashMask };
        RELEASE_ASSERT(AtomStringImpl::isValidPrecomputedHash(buffer.hash));
        ASSERT(buffer.hash == StringHasher::computeHashAndMaskTop8Bits(buffer.characters));
        return AtomStringImpl::addWithoutCopying(buffer);
    }
    return AtomString(textOfConstant(data, index)).string();
}

Identifier tryMakeIdentifier(VM& vm, uint32_t identifier)
{
    const Data* data = dataInImageIfAny();
    uint32_t stringPlusOne = data ? data->stringOfIdentifierPlusOne(identifier) : 0;
    return stringPlusOne ? Identifier::fromString(vm, atomOf(*data, stringPlusOne - 1)) : Identifier();
}

// The characters stay where they are, in the image.
static String textOfConstant(const Data& data, uint32_t index)
{
    if (uint32_t facts = data.stringFacts(index); facts & Data::stringIsASCII)
        return StringImpl::createWithoutCopyingWithHash(byteCast<Latin1Character>(data.stringInImage(index)), facts & Data::stringHasIdentifierNumber ? data.hashOfIdentifier(facts & Data::stringHashMask) : facts & Data::stringHashMask);
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

std::span<const uint8_t> Data::encodedFormOfDecodedString(uint32_t index) const
{
    static Lock lock;
    static NeverDestroyed<std::map<std::pair<const Data*, uint32_t>, Vector<uint8_t>>> encodedForms;
    Locker locker { lock };
    auto [entry, isNew] = encodedForms->try_emplace({ this, index });
    if (!isNew)
        return entry->second.span();
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        dataLogLn("AOT: the bytes of string ", index, " are asked for, which stands in the image decoded alone");
    String text = textOfConstant(*this, index);
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
    RELEASE_ASSERT(bytes.size() <= sizeOfString(index));
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

bool State::trustMemberOfGlobal(VM& vm, JSGlobalObject* globalObject, uint32_t cacheNumber, UniquedStringImpl* global, JSObject* holder, UniquedStringImpl* name)
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
    ObjectPropertyCondition ofHolder = generateConditionForSelfEquivalence(vm, nullptr, holder, name);
    if (!ofHolder || !ofHolder.isWatchable(PropertyCondition::EnsureWatchability))
        return false;
    if (m_globalPropertyWatchpoints.isEmpty())
        m_globalPropertyWatchpoints.grow(data.numberOfGlobalPropertyCaches);
    if (m_holderOfMemberWatchpoints.isEmpty())
        m_holderOfMemberWatchpoints.grow(data.numberOfGlobalPropertyCaches);
    GlobalPropertyCache& cache = globalPropertyCaches()[cacheNumber - 1];
    auto& holderWatchpoint = m_holderOfMemberWatchpoints[cacheNumber - 1];
    auto& memberWatchpoint = m_globalPropertyWatchpoints[cacheNumber - 1];
    holderWatchpoint = makeUnique<GlobalPropertyWatchpoint>();
    memberWatchpoint = makeUnique<GlobalPropertyWatchpoint>();
    holderWatchpoint->watch(vm, ofGlobal, cache, &memberWatchpoint);
    memberWatchpoint->watch(vm, ofHolder, cache, &holderWatchpoint);
    cache = { 0, 0, 0, JSValue::encode(ofHolder.requiredValue()) };
    instance.didStoreRootInStatics(data.globalPropertyCachesOffset + (cacheNumber - 1) * sizeof(GlobalPropertyCache) + OBJECT_OFFSETOF(GlobalPropertyCache, value));
    return true;
}

const Identifier& State::name(VM& vm, uint32_t index)
{
    if (uint32_t facts = data.stringFacts(index); facts & Data::stringHasIdentifierNumber) [[likely]] {
        if (const Identifier* made = instance.program->identifierIfMade(facts & Data::stringHashMask)) [[likely]]
            return *made;
    }
    return nameSlow(vm, index);
}

NEVER_INLINE const Identifier& State::nameSlow(VM& vm, uint32_t index)
{
    if (uint32_t facts = data.stringFacts(index); facts & Data::stringHasIdentifierNumber) {
        uint32_t number = facts & Data::stringHashMask;
        ASSERT(data.stringOfIdentifierPlusOne(number) == index + 1);
        if (instance.program->isIdentifierKept(number)) [[unlikely]]
            return instance.program->identifierAsIdentifier(number);
        return instance.program->keepIdentifier(number, Identifier::fromString(vm, atomOf(data, index)));
    }
    if (auto found = m_placesOfNames.find(index); found != m_placesOfNames.end())
        return m_names[found->value];
    Identifier name;
    {
        uint32_t facts = data.stringFacts(index);
        if (facts & Data::stringIsPrivateName) {
            // A name of its own, that no key the program comes by is equal to.
            auto bytes = data.string(index);
            auto written = bytes.subspan(1, std::ranges::find(bytes, static_cast<uint8_t>('@')) - bytes.begin() - 1);
            name = Identifier::fromUid(PrivateSymbolImpl::create(*textOf(written).impl()).get());
        } else if (facts & Data::stringIsEnginePrivateName) {
            name = enginePrivateName(vm, textOf(data.string(index).subspan(1)));
        } else
            name = Identifier::fromString(vm, atomOf(data, index));
    }
    m_placesOfNames.add(index, static_cast<uint32_t>(m_names.size()));
    m_names.append(WTF::move(name));
    return m_names.last();
}

JSCell* State::stringIfMade(uint32_t number) const
{
    const uint64_t* slots = instance.typedStrings;
    uint32_t mask = capacityOfStringTable(slots) - 1;
    uint32_t at = startInStringTable(number + 1);
    for (uint32_t step = 1;; at += step++) {
        uint64_t slot = slots[at & mask];
        if (static_cast<uint32_t>(slot) == number + 1)
            return data.usesWideReferences ? instance.typedWideStrings[slot >> 32] : MarkedBlockCage::decode(slot >> 32);
        if (!slot)
            return nullptr;
    }
}

void State::keepString(uint32_t number, JSCell* cell)
{
    Locker locker { m_cellsLock };
    // The last to come gets the slot it is looked for first, and what was there goes on from there on its own way: the strings a program asks for over and over are those of what it
    // is doing now, and they come when the table is at its fullest.
    auto put = [](uint64_t* slots, uint64_t slot) {
        uint32_t mask = capacityOfStringTable(slots) - 1;
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
    uint32_t capacity = capacityOfStringTable(instance.typedStrings);
    // One slot in four stays free.
    if (++m_numberOfStringsMade >= capacity - capacity / 4) {
        uint64_t* grown = createStringTable(2 * capacity);
        for (uint64_t slot : std::span { instance.typedStrings, capacity }.subspan(1)) {
            if (slot)
                put(grown, slot);
        }
        m_formerStringTables.append(std::exchange(instance.typedStrings, grown));
#if !OS(WINDOWS)
        OSAllocator::decommit(m_formerStringTables.last(), sizeOfZeroedTable(capacity, sizeof(uint64_t)));
#endif
    }
    uint64_t kept;
    if (data.usesWideReferences) {
        kept = m_wideStrings.size();
        m_wideStrings.append(cell);
        instance.typedWideStrings = m_wideStrings.mutableSpan().data();
    } else {
        kept = MarkedBlockCage::encode(cell);
        auto& madeLately = m_isLastStringConstantBackedByStaticData ? m_stringsBackedByStaticDataMadeLately : m_stringsMadeLately;
        madeLately.cells.append(static_cast<uint32_t>(kept));
    }
    put(instance.typedStrings, kept << 32 | (number + 1));
}

JSString* State::makeStringConstant(VM& vm, uint32_t index)
{
    // Compiled code takes the string of a number for a function of the number, see Lowering::stringConstant(): it may ask between an allocation and the first stores to the cell, and goes on by what it has read of the
    // collector's state. So no collection starts here, nor does one that is under way get a word in.
    DeferGCForAWhile deferGC(vm);
    // A private name of the engine is no value: it is the key of an operation, which takes it by its number.
    if (data.stringFacts(index) & Data::stringIsEnginePrivateName)
        return jsEmptyString(vm);
    String text = data.isAtomString(index) ? atomOf(data, index) : textOfConstant(data, index);
    m_isLastStringConstantBackedByStaticData = Image::isBackedByStaticData(*text.impl());
    if (m_isLastStringConstantBackedByStaticData)
        return jsOwnedString(vm, text);
    return jsString(vm, text);
}

JSString* State::makeAndKeepStringConstant(VM& vm, uint32_t index)
{
    JSString* string = makeStringConstant(vm, index);
    keepString(index, string);
    return string;
}

JSString* State::string(VM& vm, uint32_t index)
{
    if (JSCell* cell = stringIfMade(index))
        return uncheckedDowncast<JSString>(cell);
    return makeAndKeepStringConstant(vm, index);
}

RegExp* State::regExpLiteral(VM& vm, uint32_t source, uint32_t flags)
{
    RegExp*& regExp = m_regExpLiterals.add(static_cast<uint64_t>(source) << 32 | flags, nullptr).iterator->value;
    if (!regExp) {
        String text = textOfConstant(data, source);
#if ENABLE(YARR_JIT)
        regExp = RegExp::tryCreateFromImage(vm, text, OptionSet<Yarr::Flags>::fromRaw(flags));
#endif
        if (!regExp)
            regExp = RegExp::create(vm, text, OptionSet<Yarr::Flags>::fromRaw(flags));
        didCreate(regExp);
    }
    return regExp;
}

bool State::hasText(uint32_t string, StringView text) const
{
    if (data.hasOnlyDecodedForm(string))
        return !(data.stringFacts(string) & (Data::stringIsPrivateName | Data::stringIsEnginePrivateName)) && text == textOfConstant(data, string);
    auto characters = byteCast<Latin1Character>(data.stringInImage(string));
    if (characters.size() < text.length() || isPrivateName(data.string(string)) || isEnginePrivateName(data.string(string)))
        return false;
    bool isASCII = charactersAreAllASCII(characters);
    if (characters.size() == text.length())
        return isASCII && equal(text, characters);
    return !isASCII && text == textOf(data.string(string));
}

const Identifier& State::nameIfMade(uint32_t string) const
{
    static NeverDestroyed<Identifier> none;
    if (uint32_t facts = data.stringFacts(string); facts & Data::stringHasIdentifierNumber) {
        const Identifier* made = instance.program->tryGetIdentifierConcurrently(facts & Data::stringHashMask);
        return made ? *made : none.get();
    }
    auto found = m_placesOfNames.find(string);
    return found == m_placesOfNames.end() ? none.get() : m_names[found->value];
}

void State::makeNamesForHeapAnalysis(VM& vm)
{
    auto make = [&](uint32_t string, Store store) {
        if (holdsCells(store) && data.sizeOfString(string))
            name(vm, string);
    };
    for (uint32_t shape = 1; shape < data.numberOfLayouts; ++shape) {
        if (!hasLayoutOfShape(shape))
            continue;
        const LayoutData& layout = layoutOfShape(shape);
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
    if (const Identifier& known = nameIfMade(string); !known.isNull())
        return known.impl() == name;
    return hasText(string, StringView { *name });
}

// `#name` of a class is no property. Nor is what has no name.
bool State::isPrivate(const FieldData& field) const
{
    return !data.sizeOfString(field.name) || data.stringFacts(field.name) & Data::stringIsPrivateName;
}

const FieldData* State::field(VM& vm, const LayoutData& layout, UniquedStringImpl* name)
{
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

std::optional<uint32_t> State::numberOfFieldName(UniquedStringImpl* name) const
{
    CachedFieldName& cached = m_cachedFieldNames[(std::bit_cast<uintptr_t>(name) >> 4) % numberOfCachedFieldNames];
    if (cached.name == name) [[likely]]
        return cached.numberPlusOne ? std::optional(cached.numberPlusOne - 1) : std::nullopt;
    auto number = findNumberOfFieldName(name);
    name->ref();
    if (cached.name)
        cached.name->deref();
    cached = { name, number ? *number + 1 : 0 };
    return number;
}

std::optional<uint32_t> State::findNumberOfFieldName(UniquedStringImpl* name) const
{
    auto names = std::span { data.at<std::array<uint32_t, 2>>(data.fieldNamesOffset), data.numberOfFieldNames };
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
        // `#name`, `@name`: a name of its own, which name() makes or finds.
        for (auto& field : fields) {
            if (data.stringFacts(field.name) & (Data::stringIsPrivateName | Data::stringIsEnginePrivateName) && this->name(vm, field.name).impl() == name)
                return &field;
        }
        return nullptr;
    }
    auto number = numberOfFieldName(name);
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
    return typeOf(field).primitives & Primitive::Absent;
}

Object* State::compiledObject(JSValue value) const
{
    auto* object = dynamicDowncast<Object>(value);
    return object && isObjectLayout(object->layout().kind) && &object->state().data == &data ? object : nullptr;
}

bool State::passes(const RuntimeShapeTest& test, uint32_t shape) const
{
    uint32_t placeFromLast = test.first + test.count - 1 - shape;
    return placeFromLast < test.count && (test.set == UINT32_MAX || data.at<uint32_t>(data.shapeSetsOffset)[(test.set + placeFromLast) / 32] >> (test.set + placeFromLast) % 32 & 1);
}

bool State::admitsShape(const RuntimeType& type, uint32_t shape) const
{
    for (uint32_t i = 0; i < type.numberOfViews(); ++i) {
        if (passes(type.view(i), shape))
            return true;
    }
    return false;
}

bool State::membersHaveNoSpecializations(const RuntimeType& type) const
{
    static constexpr uint32_t maxNumberOfShapes = 1024;
    uint32_t numberOfShapes = 0;
    for (uint32_t i = 0; i < type.numberOfViews(); ++i) {
        auto& test = type.view(i);
        numberOfShapes += test.count;
        if (test.count > maxNumberOfShapes || numberOfShapes > maxNumberOfShapes)
            return false;
        for (uint32_t shape = test.first; shape < test.first + test.count; ++shape) {
            if (passes(test, shape) && hasLayoutOfShape(shape) && layoutOfShape(shape).numberOfSpecializations())
                return false;
        }
    }
    return true;
}

void State::willAlter(const FieldData& field, JSValue value)
{
    if (!holdsText(value, data.at<LiteralData>(field.literalsOffset())->string()))
        instance.typedAlteredLiterals[field.flags >> guardOfPinnedFieldShift] = 1;
}

bool State::holdsText(JSValue value, uint32_t string) const
{
    // A longer one is another, and is not made flat to find that out.
    return value && value.isString() && asString(value)->length() <= data.sizeOfString(string) && hasText(string, asString(value)->tryGetValue());
}

bool State::isAmongLiterals(const FieldData& field, JSValue value) const
{
    return isAmongLiterals(std::span { data.at<LiteralData>(field.literalsOffset()), field.numberOfLiterals() }, value);
}

bool State::isAmongLiteralsWithoutField(VM& vm, const LayoutData& layout, UniquedStringImpl* property, JSValue value)
{
    if (!layout.literalsWithoutFieldOffset()) [[likely]]
        return true;
    const uint32_t* words = data.at<uint32_t>(layout.literalsWithoutFieldOffset());
    for (uint32_t i = 0; i < words[0]; ++i) {
        const uint32_t* entry = words + 1 + 3 * i;
        if (name(vm, entry[0]).impl() == property)
            return isAmongLiterals(std::span { data.at<LiteralData>(entry[2]), entry[1] }, value);
    }
    return true;
}

bool State::isAmongLiterals(std::span<const LiteralData> literals, JSValue value) const
{
    bool isNumber = value.isNumber();
    if (literals.empty() || (!isNumber && !value.isString()))
        return true;
    bool hasLiteralsOfTheKind = false;
    for (auto& literal : literals) {
        if (literal.isNumber() != isNumber)
            continue;
        hasLiteralsOfTheKind = true;
        if (isNumber ? literal.number() == value.asNumber() : hasText(literal.string(), asString(value)->tryGetValue()))
            return true;
    }
    return !hasLiteralsOfTheKind;
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
        bool isInteger = number == std::trunc(number) && std::abs(number) <= static_cast<double>(maxKeptInteger) && !(!number && std::signbit(number));
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
    return type.admitsUncheckedObjects() || fitsAsOneOfItsOwn(type, asObject(cell));
}

bool State::fitsAsOneOfItsOwn(const RuntimeType& type, JSObject* cell) const
{
    if (type.allowsAnyObject())
        return true;
    if (Object* object = compiledObject(cell))
        return admitsShape(type, object->shape());
    for (uint32_t i = 0; i < type.numberOfJSTypes(); ++i) {
        if (type.jsTypes()[i] == cell->type())
            return true;
        // See EngineClass::type.
        if (type.jsTypes()[i] == ArrayType && cell->type() == DerivedArrayType && hasAnyArrayStorage(cell->indexingType()))
            return true;
    }
    return type.allowsAnyFunction() && cell->isCallable();
}

// Error messages. Nothing here runs user code.

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

// `Foo` for `new Foo`, read from prototype.constructor. Falls back to the engine's class name: Object, Map, Promise.
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

// `{ }`, `Object.create(null)`, what JSON.parse makes.
static bool hasPlainPrototype(JSObject* object)
{
    JSValue prototype = object->getPrototypeDirect();
    return prototype.isNull() || prototype == JSValue(object->realm()->objectPrototype());
}

// Made by the engine or the host: a Map, a Date, `performance`. Not a function, an array, or anything made by `{ }` or by `new` on code of the program.
static bool isNativeObject(JSObject* object)
{
    return object->type() != FinalObjectType && !object->isCallable() && !dynamicDowncast<JSArray>(object) && !dynamicDowncast<Object>(object);
}

static thread_local bool s_describesKindsAlone;

// A value as it appears after "got": undefined, number 3.5, string "abc", Foo, function foo, plain object, native Map, untyped Foo. A bare
// name is a type of the program: `expected Performance, got Performance` said nothing when the second was the host's.
String State::describe(JSValue value)
{
    if (!value)
        return "nothing"_s;
    if (value.isUndefined())
        return "undefined"_s;
    if (value.isNull())
        return "null"_s;
    if (value.isBoolean() && s_describesKindsAlone)
        return "boolean"_s;
    if (value.isBoolean())
        return value.asBoolean() ? "boolean true"_s : "boolean false"_s;
    if (value.isNumber() && s_describesKindsAlone)
        return "number"_s;
    if (value.isNumber())
        return makeString("number "_s, value.asNumber());
    if (value.isString()) {
        static constexpr unsigned maxLength = 40;
        StringImpl* text = s_describesKindsAlone ? nullptr : asString(value)->tryGetValueImpl();
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

ASCIILiteral State::wordForKind(JSValue value, const RuntimeType* heldTo)
{
    if (value.isUndefined())
        return "undefined"_s;
    if (value.isNull())
        return "null"_s;
    if (value.isBoolean())
        return "boolean"_s;
    if (value.isNumber()) {
        if (!heldTo || (heldTo->primitives & Primitive::Number) != Primitive::Int)
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

// Every reference type of the front end admits undefined and null, declared or not. Both together next to something else are left out.
static uint32_t withoutImplicitNil(uint32_t primitives, bool hasOtherMembers)
{
    bool hasBoth = (primitives & Primitive::UndefinedOrNull) == Primitive::UndefinedOrNull;
    return hasBoth && (hasOtherMembers || primitives & ~Primitive::UndefinedOrNull) ? primitives & ~Primitive::UndefinedOrNull : primitives;
}

static void appendPrimitives(StringBuilder& result, uint32_t primitives)
{
    primitives = withoutImplicitNil(primitives, !result.isEmpty());
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
    if (view.shapeOfView)
        return nameOf(layoutOfShape(view.shapeOfView));
    if (data.sizeOfString(view.name))
        return text(view.name);
    auto fields = view.fields ? data.at<ViewFields>(view.fields)->decode(data.at<uint32_t>(data.typesOffset)) : Vector<ViewField>();
    return describeAnonymousType(fields.size(), [&](uint32_t i) {
        return fields[i].name;
    });
}

// `{ a, b, c, d, ... }`
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

// A type in TypeScript syntax, as it appears after "expected".
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
    for (uint32_t i = 0; i < type.numberOfViews(); ++i)
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
    if (data.sizeOfString(layout.name))
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
    if (to.allowsAnyObject() || to.admitsForeignObjects())
        return true;
    if (from.allowsAnyObject() || from.admitsForeignObjects() || (from.allowsAnyFunction() && !to.allowsAnyFunction()))
        return false;
    std::span toJSTypes { to.jsTypes(), to.numberOfJSTypes() };
    for (uint32_t i = 0; i < from.numberOfJSTypes(); ++i) {
        if (std::ranges::find(toJSTypes, from.jsTypes()[i]) == toJSTypes.end())
            return false;
    }
    for (uint32_t i = 0; i < from.numberOfViews(); ++i) {
        auto& view = from.view(i);
        for (uint32_t shape = view.first; shape < view.first + view.count; ++shape) {
            if (passes(view, shape) && hasLayoutOfShape(shape) && !admitsShape(to, shape))
                return false;
        }
    }
    return !from.elements() || !to.elements() || isAssignable(*data.at<RuntimeType>(from.elements()), *data.at<RuntimeType>(to.elements()));
}

// An own or inherited data property. Empty: there is none. nullopt: an accessor, whose value is unknown without running it.
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

String State::explainMismatch(VM& vm, JSValue value, const RuntimeType& type, bool membersAlone)
{
    // A union would need a guess at which member was meant.
    if (!value.isObject() || type.numberOfViews() != 1)
        return { };
    SetForScope kindsOnlyScope(s_describesKindsAlone, s_describesKindsAlone || membersAlone);
    JSObject* object = asObject(value);
    Object* compiled = compiledObject(object);
    auto& view = type.view(0);
    if (membersAlone && isNativeObject(object))
        return { };
    if (isNativeObject(object))
        return makeString("a native object cannot be used as "_s, nameOf(view));
    if (!compiled && object->type() != FinalObjectType)
        return { };
    String reason;
    // The two types declare the property differently, though the run time cannot tell the two declarations apart (`readonly T[]` and `T[]`).
    // Given as the reason if nothing else is wrong.
    String differentDeclaration;
    auto textOrDescription = [&](uint32_t typeText, const RuntimeType& type) {
        return typeText == UINT32_MAX ? describe(type) : text(typeText);
    };
    auto typeTextOf = [&](const LayoutData& layout, size_t field) {
        return layout.typeTextsOffset() ? data.at<uint32_t>(layout.typeTextsOffset())[field] : UINT32_MAX;
    };
    // False: stop.
    auto check = [&](uint32_t nameIndex, const RuntimeType& declared, bool isOptional, uint32_t declaredText) {
        if (data.stringFacts(nameIndex) & Data::stringIsPrivateName)
            return true;
        const Identifier& name = this->name(vm, nameIndex);
        // Whether a compiled object is of a type goes by its layout, not by what it holds: a field that is declared and has not been stored
        // to yet is no reason. So the two declarations are compared.
        const FieldData* ownField = compiled ? field(vm, compiled->layout(), name.impl()) : nullptr;
        if (ownField && !membersAlone) {
            if (ownField->flags & fieldIsOptional && !isOptional) {
                reason = makeString("property '"_s, name.string(), "' is optional in "_s, describe(value), " but required in "_s, nameOf(view));
                return false;
            }
            uint32_t ownText = typeTextOf(compiled->layout(), ownField - compiled->layout().fields());
            if (!isAssignable(ownTypeOf(*ownField), declared)) {
                reason = makeString("property '"_s, name.string(), "': "_s, textOrDescription(ownText, ownTypeOf(*ownField)), " is not assignable to "_s, textOrDescription(declaredText, declared));
                return false;
            }
            if (differentDeclaration.isNull() && (ownText != declaredText || ownText == UINT32_MAX) && isAssignable(declared, ownTypeOf(*ownField))) {
                String ownWords = textOrDescription(ownText, ownTypeOf(*ownField));
                String declaredWords = textOrDescription(declaredText, declared);
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
            reason = makeString("property '"_s, name.string(), "': expected "_s, textOrDescription(declaredText, declared), ", got "_s, describe(actual));
            return false;
        }
        return true;
    };
    if (view.shapeOfView) {
        const LayoutData& layout = layoutOfShape(view.shapeOfView);
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            const FieldData& declared = layout.fields()[i];
            if (!check(declared.name, ownTypeOf(declared), declared.flags & fieldIsOptional, typeTextOf(layout, i)))
                break;
        }
    } else if (view.fields) {
        for (auto& declared : data.at<ViewFields>(view.fields)->decode(data.at<uint32_t>(data.typesOffset))) {
            if (!check(declared.name, *data.at<RuntimeType>(declared.type), declared.isOptional, declared.typeText))
                break;
        }
    }
    if (!reason.isNull() || membersAlone)
        return reason;
    if (!differentDeclaration.isNull())
        return differentDeclaration;
    if (compiled)
        return makeString(describe(value), " is not assignable to "_s, nameOf(view));
    if (!hasPlainPrototype(object))
        return makeString("an instance of an untyped class cannot be used as "_s, nameOf(view));
    if (view.shapeOfView && layoutOfShape(view.shapeOfView).kind == LayoutKind::Instance)
        return makeString("an untyped object cannot be used as an instance of class "_s, nameOf(view));
    return makeString("an object from untyped code is accepted as "_s, nameOf(view), " only at a type annotation or an 'as "_s, nameOf(view), "' cast"_s);
}

String State::mismatchMessage(VM& vm, JSValue value, const RuntimeType& expected, const String& knownReason)
{
    String reason = knownReason.isEmpty() ? explainMismatch(vm, value, expected) : knownReason;
    String expectedText = describe(expected);
    String actualText = describe(value);
    // Two types of one name are told apart by where they are declared.
    if (Object* object = expectedText == actualText && expected.numberOfViews() == 1 ? compiledObject(value) : nullptr) {
        auto withPlace = [&](const String& name, uint32_t path, uint32_t line) {
            return path == UINT32_MAX ? name : makeString(name, " (declared at "_s, text(path), ':', line, ')');
        };
        auto& view = expected.view(0);
        const LayoutData* layoutOfView = view.shapeOfView ? &layoutOfShape(view.shapeOfView) : nullptr;
        expectedText = withPlace(expectedText, layoutOfView ? layoutOfView->declaredPath() : view.hasDeclaredPlace ? view.declaredPath : UINT32_MAX, layoutOfView ? layoutOfView->declaredLine() : view.hasDeclaredPlace ? view.declaredLine : 0);
        actualText = withPlace(actualText, object->layout().declaredPath(), object->layout().declaredLine());
    }
    return makeString("expected "_s, expectedText, ", got "_s, actualText, reason.isEmpty() ? ""_s : "\n  "_s, reason);
}

String State::nullishMessage(JSValue value, const String& property, bool isWrite)
{
    ASCIILiteral valueName = value.isNull() ? "null"_s : "undefined"_s;
    if (property.isEmpty())
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

String State::nonNullMessage(JSValue value, const String& operandText)
{
    if (operandText.isEmpty())
        return makeString("non-null assertion failed: value is "_s, describe(value));
    return makeString("non-null assertion failed: `"_s, operandText, "` is "_s, describe(value));
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
    return textOfConstant(data, string);
}

String State::describeSite(AuditSite site) const
{
    RELEASE_ASSERT(data.isCompiledForAudit);
    // An operation that is the same in every build was not told.
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
    return Typed::classConstructedBy(data, function);
}

static uint8_t* addressOfVariable(const Instance& instance, const Closure* classObject, const LayoutData& ofClass, const FieldData& variable)
{
    if (ofClass.classData().environment == LayoutData::noBase)
        return instance.typedStatics + variable.offset;
    return reinterpret_cast<uint8_t*>(classObject->environment()) + variable.offset;
}

JSValue State::readVariable(const Closure* classObject, const LayoutData& ofClass, const FieldData& variable) const
{
    return readAs(variable.store(), addressOfVariable(instance, classObject, ofClass, variable));
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

// The first class that declares the name decides: that is where a search by name ends.
bool State::isFixedIn(std::span<const MethodData> methods, UniquedStringImpl* name, bool& isDeclared) const
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

const EngineClass* State::engineClassConstructedBy(JSObject* constructor)
{
    auto& builtins = Builtins::singleton();
    if (m_engineClassesByConstructor.isEmpty()) {
        for (auto& engineClass : builtins.classes()) {
            if (builtins.mayBeExtended(engineClass.name) && builtins.isGlobal(engineClass.name))
                m_engineClassesByConstructor.add(asObject(builtins.findCell(instance.globalObject, builtins.cellOf(engineClass.name))), engineClass.index);
        }
    }
    auto found = m_engineClassesByConstructor.find(constructor);
    return found == m_engineClassesByConstructor.end() ? nullptr : &builtins.classes()[found->value];
}

void Closure::setComputedName(VM& vm, const Identifier& key, Format::MethodKind kind)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = computedNameOfFunction(globalObject(), key, kind);
    RETURN_IF_EXCEPTION(scope, void());
    putDirect(vm, vm.propertyNames->name, jsString(vm, name), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
    // Function.prototype.bind() takes the name out of the executable otherwise.
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
        void handleFire(VM&, const FireDetail&) final { m_owner.state.didAlterMembers(m_owner); }

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
    // Both are kept by the class, which stays.
    JSObject* holder;
    JSValue parent;
    const LayoutData& ofClass;
    bool isAltered { false };
    TransitionWatchpoint transition;
    Vector<std::unique_ptr<Member>> members;
};

void State::updateStaticFieldPlaces(VM& vm, WatchedMembers& watched)
{
    uint32_t first = watched.ofClass.classData().firstStaticFieldPlace;
    if (first == LayoutData::noBase)
        return;
    for (auto& member : watched.members)
        member->key().isWatchable(PropertyCondition::EnsureWatchability);
    int32_t* place = reinterpret_cast<int32_t*>(instance.typedStatics + data.staticFieldPlacesOffset) + first;
    bool isWatchedAlready = !!*reinterpret_cast<JSObject**>(place);
    *reinterpret_cast<JSObject**>(place) = watched.holder;
    place += 2;
    Structure* structure = watched.holder->structure();
    UniquedStringImpl* added = isWatchedAlready && !watched.isAltered && structure->transitionKind() == TransitionKind::PropertyAddition ? structure->transitionPropertyName() : nullptr;
    const FieldData* readyAndSkipped = nullptr;
    bool isAnnounced = false;
    for (auto& field : watched.ofClass.staticFields()) {
        if (added && name(vm, field.name).impl() != added) {
            if (*place)
                readyAndSkipped = &field;
            ++place;
            continue;
        }
        *place = 0;
        unsigned attributes;
        PropertyOffset offset = watched.isAltered ? invalidOffset : structure->get(vm, name(vm, field.name), attributes);
        if (isValidOffset(offset) && isOutOfLineOffset(offset) && !attributes && fits(ownTypeOf(field), watched.holder->getDirect(offset))) {
            structure->didCachePropertyReplacement(vm, offset);
            isAnnounced = true;
            *place = static_cast<int32_t>(reinterpret_cast<uint8_t*>(watched.holder->locationForOffset(offset)) - reinterpret_cast<uint8_t*>(watched.holder->butterfly()));
        }
        ++place;
    }
    if (readyAndSkipped && !isAnnounced)
        structure->didCachePropertyReplacement(vm, structure->get(vm, name(vm, readyAndSkipped->name)));
}

void State::didAlterMembers(WatchedMembers& watched)
{
    if (std::exchange(watched.isAltered, true))
        return;
    updateStaticFieldPlaces(*instance.vm, watched);
    // What a class that extends it inherits is looked for here too.
    for (uint32_t index = 0; index < data.numberOfLayoutIndices; ++index) {
        for (const LayoutData* next = classWithIndex(index); next; next = baseOf(*next)) {
            if (next->index == watched.ofClass.index)
                instance.typedAlteredClassObjects[index] = 1;
        }
    }
}

void State::didMakeTransition(VM& vm, WatchedMembers& watched)
{
    if (watched.isAltered)
        return;
    // Another parent, or a property of its own in front of what it inherits.
    bool findsWhatIsDeclared = watched.holder->getPrototypeDirect() == watched.parent && !watched.holder->getDirect(vm, vm.propertyNames->hasInstanceSymbol);
    for (const LayoutData* next = baseOf(watched.ofClass); next && findsWhatIsDeclared; next = baseOf(*next)) {
        for (auto& method : next->staticMethods()) {
            if (!(method.flags & methodHasComputedName) && !(data.stringFacts(method.name) & Data::stringIsPrivateName) && watched.holder->getDirect(vm, name(vm, method.name)))
                findsWhatIsDeclared = false;
        }
    }
    auto again = [](VM& vm, void* context) {
        auto& watched = *static_cast<WatchedMembers*>(context);
        watched.state.didMakeTransition(vm, watched);
    };
    if (!findsWhatIsDeclared || !watched.transition.install(watched.holder, again, &watched))
        didAlterMembers(watched);
    else
        updateStaticFieldPlaces(vm, watched);
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
            return didAlterMembers(watched);
        watched.members.append(makeUnique<WatchedMembers::Member>(condition, watched));
        watched.members.last()->install(vm);
    }
    didMakeTransition(vm, watched);
}

// ClassDefinitionEvaluation, but for what can be seen to happen at the declaration, which compiled code does there: what stands after
// `extends` and the computed names are evaluated, the static fields are initialized, the static blocks run.
void State::makeClass(JSGlobalObject* globalObject, Closure* classObject, JSValue base, PartOfClass part, JSValue prototypeOfBaseIfRead)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const LayoutData& ofClass = *classObject->classData();
    if (part == PartOfClass::Everything && ofClass.isEvaluatedOnce && (classObject->getDirect(vm, vm.propertyNames->prototype) || (!hasPrototypeAheadOfClassObject(ofClass) && prototypesOfClasses.get(ofClass.index))))
        return;
    if (!classObject->isDeclaredAs(functionIsOfBuiltinModule))
        ensureStaticsOfProgramAreFilled();
    bool makesClassObject = part != PartOfClass::MembersOfPrototype;
    bool isAtDeclaration = part == PartOfClass::Everything;
    // The declaration has been evaluated, and has left the class object unmade.
    bool wasDeclared = part == PartOfClass::ClassObject && !hasPrototypeAheadOfClassObject(ofClass);
    RELEASE_ASSERT(part == PartOfClass::Everything || makesMembersWhenLookedAt(ofClass));
    // The members of the prototype may be looked at while the class object is still unmade.
    RELEASE_ASSERT(!makesClassObject || !classObject->getDirect(vm, vm.propertyNames->prototype));
    // Compiled code has made it with the environment of the class and nothing else. The variables of the class are reached through that.
    if (ofClass.classData().environment == LayoutData::noBase)
        RELEASE_ASSERT(classObject->captures().empty());
    else
        RELEASE_ASSERT(classObject->captures().size() == 1 && classObject->environment() && uncheckedDowncast<Object>(classObject->environment())->layout().index == ofClass.classData().environment);

    JSValue parentOfPrototype = globalObject->objectPrototype();
    JSObject* parentOfClassObject = nullptr;
    // What the class extends was evaluated when the class object was made.
    if (!makesClassObject)
        parentOfPrototype = prototypesOfClasses.get(ofClass.index)->getPrototypeDirect();
    else if (wasDeclared) {
        parentOfClassObject = classObject->getPrototypeDirect().getObject();
        parentOfPrototype = prototypesOfClasses.get(ofClass.index)->getPrototypeDirect();
    } else if (const LayoutData* baseClass = baseOf(ofClass)) {
        // Of a class that is evaluated once there is one class object. Of any other, compiled code says which this one extends.
        auto* baseClassObject = baseClass->isEvaluatedOnce ? closure(vm, baseClass->classData().classConstructor) : dynamicDowncast<Closure>(base);
        RELEASE_ASSERT(baseClassObject && baseClassObject->classData() == baseClass);
        parentOfClassObject = baseClassObject;
        parentOfPrototype = baseClassObject->prototypeProperty(vm);
        RELEASE_ASSERT(parentOfPrototype);
    } else if (ofClass.hasForeignRoot && base.isNull())
        parentOfPrototype = jsNull();
    else if (ofClass.hasForeignRoot) {
        if (!base.isConstructor()) {
            throwTypeError(globalObject, scope, "The superclass is not a constructor."_s);
            return;
        }
        parentOfClassObject = asObject(base);
        parentOfPrototype = prototypeOfBaseIfRead ? prototypeOfBaseIfRead : parentOfClassObject->get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, void());
        if (!parentOfPrototype.isObject() && !parentOfPrototype.isNull()) {
            throwTypeError(globalObject, scope, "The value of the superclass's prototype property is not an object or null."_s);
            return;
        }
        // Compiled code calls the methods of a class of the engine without looking them up, unless the class is seen to be extended. What the
        // host's own modules make reaches the program as what a declaration without a body says, which is tested.
        for (JSObject* next = classObject->isDeclaredAs(functionIsOfBuiltinModule) ? nullptr : parentOfClassObject; next;) {
            if (auto* engineClass = engineClassConstructedBy(next); engineClass && !data.at<uint8_t>(data.extendedClassesOffset)[engineClass->index]) {
                if (!reportOrThrow(globalObject, { .kind = "extends-untyped-builtin"_s, .on = &ofClass }, makeString("Class "_s, nameOf(ofClass), " cannot extend "_s, engineClass->text, " through an untyped expression: the 'extends' clause must have the static type of "_s, engineClass->text), InAudit::CarriedOut))
                    return;
                break;
            }
            if (auto* proxy = dynamicDowncast<ProxyObject>(next))
                next = proxy->target();
            else if (auto* bound = dynamicDowncast<JSBoundFunction>(next))
                next = bound->targetFunction();
            else
                next = next->getPrototypeDirect().getObject();
        }
    }

    // See below.
    bool keepsMethodsReplaceable = ofClass.isLaidOut && (ofClass.constructorMayBeReplaced || std::ranges::any_of(ofClass.methods(), [](auto& method) {
        return method.flags & methodMayBeReplaced;
    }));
    // A Structure that no other object has: the members are added to it where it is, without a Structure for each.
    if (parentOfPrototype.isObject())
        asObject(parentOfPrototype)->didBecomePrototype(vm);
    if (part == PartOfClass::Everything && makesMembersWhenLookedAt(ofClass) && parentOfPrototype.isObject())
        part = PartOfClass::ClassObject;
    bool makesMembersOfPrototype = part != PartOfClass::ClassObject;
    bool mayShareStructures = part == PartOfClass::Everything && !ofClass.isEvaluatedOnce && !ofClass.isLaidOut && !classObject->mayBePrototype();
    JSCell* parentOfPrototypeOrNull = parentOfPrototype.isCell() ? parentOfPrototype.asCell() : nullptr;
    Instance::StructuresOfClass shared;
    if (mayShareStructures)
        shared = instance.structuresOfClass(ofClass.index, parentOfClassObject, parentOfPrototypeOrNull);
    if (shared.ofClassObject && (shared.ofClassObject->realm() != globalObject || shared.ofClassObject->storedPrototype() != (parentOfClassObject ? JSValue(parentOfClassObject) : classObject->getPrototypeDirect())))
        shared = { };
    bool isShared = shared.ofClassObject;
    bool hasOnlyPlainMembers = true;
    auto storageFor = [&](Structure* structure) {
        return structure->outOfLineCapacity() ? Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0) : nullptr;
    };
    auto storeShared = [&](JSObject* target, UniquedStringImpl* key, JSValue value) {
        PropertyOffset offset = target->structure()->get(vm, key);
        RELEASE_ASSERT(isValidOffset(offset));
        target->putDirectOffset(vm, offset, value);
    };
    // A ClassPrototype until somebody looks: made here, or before, for an object of the class: see prototypeOfClass().
    JSObject* prototype;
    if (isShared)
        prototype = JSFinalObject::createWithButterfly(vm, shared.ofPrototype, storageFor(shared.ofPrototype));
    else if (part == PartOfClass::Everything) {
        prototype = constructEmptyObject(vm, JSFinalObject::createStructure(vm, globalObject, parentOfPrototype, JSFinalObject::defaultInlineCapacity));
        if (ofClass.isEvaluatedOnce) {
            RELEASE_ASSERT(!prototypesOfClasses.get(ofClass.index));
            prototypesOfClasses.set(ofClass.index, prototype);
            didCreate(prototype);
        }
    } else {
        prototype = prototypesOfClasses.get(ofClass.index);
        if (!prototype)
            prototype = makeClassPrototype(vm, ofClass.index, asObject(parentOfPrototype));
        parentOfPrototype = prototype->getPrototypeDirect();
    }
    // All that the declaration has to do is done. See Closure::isUnmadeClassObject().
    if (isAtDeclaration && part == PartOfClass::ClassObject && parentOfClassObject && makesClassObjectWhenChanged(ofClass) && !classObject->mayBePrototype()) {
        RELEASE_ASSERT(!classObject->butterfly());
        classObject->setStructure(vm, structureOfUnmadeClassObjectInheritingFrom(vm, parentOfClassObject));
        if (!mayLeaveClassObjectUnmade(parentOfClassObject, ofClass))
            classObject->ensureClassObject(vm);
        return;
    }
    using StoredAhead = Vector<std::pair<RefPtr<UniquedStringImpl>, unsigned>, 2>;
    StoredAhead storedAhead;
    MarkedArgumentBuffer valuesStoredAhead;
    StoredAhead storedAheadOnPrototype;
    MarkedArgumentBuffer valuesStoredAheadOnPrototype;
    auto take = [&](JSObject* from, StoredAhead& stored, MarkedArgumentBuffer& values) {
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
        if (part == PartOfClass::MembersOfPrototype)
            prototype->structure()->setMayBePrototype(true);
    });
    if (isShared) {
        RELEASE_ASSERT(!classObject->butterfly());
        if (Butterfly* storage = storageFor(shared.ofClassObject))
            classObject->nukeStructureAndSetButterfly(vm, classObject->structureID(), storage);
        classObject->setStructure(vm, shared.ofClassObject);
        storeShared(classObject, vm.propertyNames->prototype.impl(), prototype);
        storeShared(prototype, vm.propertyNames->constructor.impl(), classObject);
    } else if (makesClassObject) {
        // A Structure that no other object has: every transition the class object makes is the first from where it is, and is told. See watchMembers().
        RELEASE_ASSERT(!parentOfClassObject || !makesClassObjectWhenChanged(ofClass) || classObject->getPrototypeDirect() == parentOfClassObject);
        Structure* ofThisClassObject = Closure::createStructure(vm, globalObject, parentOfClassObject ? parentOfClassObject : classObject->getPrototypeDirect().getObject());
        ofThisClassObject->setAOTInstance(&instance);
        classObjectMayBePrototype = classObject->mayBePrototype();
        if (classObject->butterfly()) {
            take(classObject, storedAhead, valuesStoredAhead);
            classObject->nukeStructureAndSetButterfly(vm, classObject->structureID(), nullptr);
        }
        classObject->setStructure(vm, ofThisClassObject);
        classObject->putDirectWithoutTransition(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
    }
    if (part == PartOfClass::MembersOfPrototype) {
        Structure* ordinary = JSFinalObject::createStructure(vm, globalObject, parentOfPrototype, ClassPrototype::inlineCapacity);
        if (prototype->butterfly())
            take(prototype, storedAheadOnPrototype, valuesStoredAheadOnPrototype);
        uncheckedDowncast<ClassPrototype>(prototype)->becomeOrdinary(vm, ordinary);
    }
    if (makesMembersOfPrototype && !isShared)
        prototype->putDirectWithoutTransition(vm, vm.propertyNames->constructor, classObject, static_cast<unsigned>(PropertyAttribute::DontEnum));

    auto isPrivate = [&](const MethodData& method) {
        return !(method.flags & methodHasComputedName) && data.stringFacts(method.name) & Data::stringIsPrivateName;
    };
    auto keyFor = [&](const MethodData& method) {
        if (!(method.flags & methodHasComputedName))
            return name(vm, method.name);
        RELEASE_ASSERT(method.name < ofClass.classData().numberOfVariables);
        // The declaration has stored it.
        JSValue key = readVariable(classObject, ofClass, ofClass.variables()[method.name]);
        RELEASE_ASSERT(key && (key.isString() || key.isSymbol()));
        return key.toPropertyKey(globalObject);
    };
    // Nothing is in the way: the objects are new, and this goes past what a class object refuses.
    auto define = [&](JSObject* target, const Identifier& key, const PropertyDescriptor& descriptor) {
        JSObject::defineOwnProperty(target, globalObject, key, descriptor, true);
        scope.assertNoException();
    };
    // `#name() {}` is no property. See findPrivateElement() for a class that is laid out. For any other, and for `static #name() {}`,
    // compiled code sees to it.
    auto defineAll = [&](JSObject* target, std::span<const MethodData> methods) {
        for (auto& method : methods) {
            bool isKeptForObjects = isPrivate(method) && target == prototype && ofClass.isLaidOut && !ofClass.isEvaluatedOnce;
            if (isPrivate(method) && !isKeptForObjects)
                continue;
            Identifier key = keyFor(method);
            auto kind = static_cast<Format::MethodKind>(method.kind);
            // Each evaluation of a class makes its own functions. One that captures anything captures what the class object does.
            bool takesEnvironment = method.flags & methodTakesEnvironment;
            JSObject* function;
            if (ofClass.isEvaluatedOnce)
                function = closureObjectFor(vm, method.function);
            else if (makesSlimClosures(method.function))
                function = SlimClosure::create(vm, slimClosureStructure(vm, method.function), method.function, classObject->captures().first(takesEnvironment), takesEnvironment);
            else
                function = Closure::create(vm, structureOfClosureThatIsNoClassObject(vm, method.function), method.function, classObject->captures().first(takesEnvironment), takesEnvironment);
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
            // What defineOwnProperty makes of it on an object that has no such property. An index is no property of the Structure. Nor are, until
            // somebody asks, the properties that a function makes of itself: JSFunction::defineOwnProperty makes them first.
            bool mayBeMadeByFunction = target == classObject && (key == vm.propertyNames->name || key == vm.propertyNames->length || key == vm.propertyNames->prototype);
            if (!mayBeMadeByFunction && !parseIndex(key) && !isValidOffset(target->structure()->get(vm, key))) {
                if (kind == Format::MethodKind::Method)
                    target->putDirectWithoutTransition(vm, key, function, PropertyAttribute::DontEnum | (isKeptForObjects ? PropertyAttribute::ReadOnly : PropertyAttribute::None));
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
                descriptor.setWritable(!isKeptForObjects);
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
    if (makesMembersOfPrototype)
        defineAll(prototype, ofClass.methods());
    RETURN_IF_EXCEPTION(scope, void());
    if (makesClassObject)
        defineAll(classObject, ofClass.staticMethods());
    RETURN_IF_EXCEPTION(scope, void());

    // From here on it cannot be stored to, redefined or deleted.
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
    auto fixAll = [&](JSObject* target, std::span<const MethodData> methods) {
        for (auto& method : methods) {
            if (!isPrivate(method) && !(method.flags & methodMayBeReplaced))
                fix(target, keyFor(method), static_cast<Format::MethodKind>(method.kind) != Format::MethodKind::Method);
        }
    };
    auto putBack = [&](JSObject* target, StoredAhead& stored, MarkedArgumentBuffer& values) {
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
    putBack(classObject, storedAhead, valuesStoredAhead);
    putBack(prototype, storedAheadOnPrototype, valuesStoredAheadOnPrototype);
    // Only of such a class does compiled code look up what the class object has.
    if (makesClassObject && ofClass.hasFixedMembers && ofClass.isEvaluatedOnce)
        watchMembers(vm, classObject, ofClass);
    if (mayShareStructures && !isShared && hasOnlyPlainMembers)
        instance.noteStructuresOfClass(ofClass.index, parentOfClassObject, parentOfPrototypeOrNull, { classObject->structure(), prototype->structure() });
    if (!ofClass.isLaidOut || !makesMembersOfPrototype)
        return;
    // Compiled code calls the methods by the shape of the object, and takes an absent field for `undefined` without looking here.
    if (!keepsMethodsReplaceable) {
        bool isImmutable = prototype->fixAndMakePropertiesImmutable(vm);
        RELEASE_ASSERT(isImmutable);
        return;
    }
    // The methods the program is seen to replace stay as the language has them, writable and configurable: compiled code looks them up.
    // makePropertiesImmutable() knows no exceptions, so what it does to an object is done here to all the rest: every other property is
    // fixed, nothing can be added, the prototype stays, and what inherits a property that has become read-only may still have its own.
    // In place: nobody has looked at it yet, and no other object has its Structure.
    prototype->convertToUncacheableDictionary(vm);
    fixAll(prototype, ofClass.methods());
    if (!ofClass.constructorMayBeReplaced)
        fix(prototype, vm.propertyNames->constructor, false);
    JSObject::preventExtensions(prototype, globalObject);
    scope.assertNoException();
    prototype->flattenDictionaryObject(vm);
    prototype->structure()->setInheritorsMayOverrideReadOnlyProperties(true);
}

bool State::makesMembersWhenLookedAt(const LayoutData& ofClass) const
{
    return ofClass.makesMembersWhenLookedAt;
}

bool State::hasPrototypeAheadOfClassObject(const LayoutData& ofClass) const
{
    return ofClass.hasPrototypeAheadOfClassObject;
}

bool State::makesClassObjectWhenChanged(const LayoutData& ofClass)
{
    return ofClass.makesClassObjectWhenChanged;
}

Structure* State::structureOfUnmadeClassObject(VM& vm, const LayoutData& ofClass)
{
    const LayoutData* baseClass = baseOf(ofClass);
    if (!baseClass)
        return closureStructure(vm, ofClass.classData().classConstructor);
    return structureOfUnmadeClassObjectInheritingFrom(vm, closure(vm, baseClass->classData().classConstructor));
}

Structure* State::structureOfUnmadeClassObjectInheritingFrom(VM& vm, JSObject* parent)
{
    Structure* structure = structuresOfUnmadeClassObjects.get(parent);
    if (!structure) {
        structure = Closure::createStructure(vm, instance.globalObject, parent);
        structure->setAOTInstance(&instance);
        structuresOfUnmadeClassObjects.set(parent, structure);
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

bool State::mayLeaveClassObjectUnmade(JSObject* parent, const LayoutData& ofClass)
{
    if (!ofClass.classData().numberOfStaticMethods)
        return true;
    UnmadeHeirs& heirs = unmadeHeirs.add(parent, UnmadeHeirs { }).iterator->value;
    for (auto& name : heirs.assumedAbsent) {
        if (hasStaticMethodNamed(ofClass, name.get()))
            return false;
    }
    heirs.withStaticMembers.append(ofClass.index);
    return true;
}

bool State::isStructureOfUnmadeHeirsOf(VM& vm, Structure* structure, JSObject* parent)
{
    return structure->storedPrototypeObject() == parent && (structure == structuresOfUnmadeClassObjects.get(parent) || structure == closureStructures(vm)[closureStructureOfClassObjects]);
}

bool State::tryAssumeAbsentFromUnmadeHeirs(VM& vm, JSObject* parent, UniquedStringImpl* name)
{
    Vector<Closure*, 4> declaring;
    {
        UnmadeHeirs& heirs = unmadeHeirs.add(parent, UnmadeHeirs { }).iterator->value;
        if (heirs.assumedAbsent.contains(name))
            return true;
        for (uint32_t index : heirs.withStaticMembers) {
            const LayoutData* ofClass = classWithIndex(index);
            if (!hasStaticMethodNamed(*ofClass, name))
                continue;
            JSCell* classObject = closureIfMade(ofClass->classData().classConstructor);
            if (!classObject)
                return false;
            if (uncheckedDowncast<Closure>(classObject)->isUnmadeClassObject())
                declaring.append(uncheckedDowncast<Closure>(classObject));
        }
        heirs.assumedAbsent.append(name);
    }
    for (Closure* heir : declaring) {
        if (Options::useAOTOperationCounters()) [[unlikely]]
            runtimeTable(vm).countOperation("ClassObject", "made-because-a-sibling-is-cached");
        heir->ensureClassObject(vm);
    }
    return true;
}

JSObject* State::makeClassPrototype(VM& vm, uint32_t classIndex, JSObject* parent)
{
    Structure* structure = structuresOfClassPrototypes.get(parent);
    if (!structure) {
        structure = ClassPrototype::createStructure(vm, instance.globalObject, parent);
        structure->setAOTInstance(&instance);
        structuresOfClassPrototypes.set(parent, structure);
        didCreate(structure);
    }
    JSObject* prototype = ClassPrototype::create(vm, structure, classIndex);
    prototypesOfClasses.set(classIndex, prototype);
    didCreate(prototype);
    return prototype;
}

JSObject* State::prototypeOfClass(VM& vm, uint32_t classIndex)
{
    if (JSObject* prototype = prototypesOfClasses.get(classIndex))
        return prototype;
    const LayoutData& ofClass = *classWithIndex(classIndex);
    if (hasPrototypeAheadOfClassObject(ofClass)) {
        const LayoutData* baseClass = baseOf(ofClass);
        return makeClassPrototype(vm, classIndex, baseClass ? prototypeOfClass(vm, baseClass->index) : instance.globalObject->objectPrototype());
    }
    closure(vm, ofClass.classData().classConstructor);
    JSObject* prototype = prototypesOfClasses.get(classIndex);
    RELEASE_ASSERT(prototype);
    return prototype;
}

Object* State::createObjectOfClass(VM& vm, const LayoutData& ofClass, JSObject* prototype)
{
    RELEASE_ASSERT(ofClass.isLaidOut && ofClass.shape);
    Vector<uint16_t, 16> fields;
    for (uint32_t i = 0; i < ofClass.numberOfFields; ++i) {
        if (!(ofClass.fields()[i].flags & fieldIsAbsentAtBirth))
            fields.append(static_cast<uint16_t>(i));
    }
    Object* object = Object::createWithBornFields(vm, structureFor(vm, ofClass.shape, prototype), ofClass, sizeWithRoomToSpecialize(ofClass.shape), orderStartingWith(ofClass, fields.span()));
    object->noteWhetherToAsk(*this, true);
    countObject(object);
    return object;
}

// OrdinaryCreateFromConstructor. Whoever constructs makes the object, of the class that is constructed: the constructors of the classes
// that one extends work on it.
JSValue State::createThis(JSGlobalObject* globalObject, Closure* constructor, JSObject* newTarget)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const LayoutData* ofClass = constructor->classData();
    if (ofClass && ofClass->hasForeignRoot)
        return jsUndefined();
    const LayoutData* made = ofClass;
    JSValue prototype;
    if (auto* target = dynamicDowncast<Closure>(newTarget); ofClass && target && target->structure()->aotInstance() == &instance) {
        const LayoutData* ofTarget = target->classData();
        for (const LayoutData* next = ofTarget; next; next = baseOf(*next)) {
            if (next != ofClass)
                continue;
            made = ofTarget;
            // It cannot be stored to or redefined.
            prototype = target->prototypeProperty(vm);
            break;
        }
    }
    if (!prototype) {
        prototype = newTarget->get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, { });
        // Compiled code calls the methods of the class by the shape of the object, whatever its prototype object has.
        if (ofClass && ofClass->isLaidOut && !ofClass->mayHaveOtherPrototype && prototype != constructor->prototypeProperty(vm) && !reportOrThrow(globalObject, { .kind = "construct-new-target"_s, .on = ofClass }, makeString("Cannot construct "_s, nameOf(*ofClass), " with a different new.target: only a subclass in TypeScript code of the program can extend it"_s), InAudit::CarriedOut, "instanceof answers by the class the object was declared as"_s))
            return { };
    }
    JSObject* parent = prototype.isObject() ? asObject(prototype) : globalObject->objectPrototype();
    if (!ofClass) {
        // `function Point(this: Point, ..)`: as `new` makes it where it knows the function.
        if (const AllocationData* allocation = allocationOfThis(constructor->functionIndex())) {
            if (parent != instance.globalObject->objectPrototype() && !parent->inherits<PrototypeObject>() && (!layoutOfShape(allocation->shape).mayHaveOtherPrototype || makesObjectsWithDefinitelyInitializedFields(constructor->functionIndex()))) [[unlikely]] {
                bool goesOn = reportOrThrow(globalObject, { .kind = "construct-new-target"_s, .onFunction = constructor->functionIndex() }, makeString("Cannot construct '"_s, nameOfFunction(constructor->functionIndex()), "' with a new.target whose 'prototype' is not that of a function in TypeScript code of the program"_s), InAudit::CarriedOut);
                RETURN_IF_EXCEPTION(scope, { });
                ASSERT_UNUSED(goesOn, goesOn);
            }
            Object* object = Object::create(vm, structureFor(vm, allocation->shape, parent), layoutOfShape(allocation->shape), allocation->size, allocation->orderWord);
            object->noteWhetherToAsk(*this, true);
            countAllocation(static_cast<uint32_t>(allocation - &this->allocation(0)), object);
            return object;
        }
    }
    if (!ofClass || !ofClass->isLaidOut) {
        auto* target = dynamicDowncast<Closure>(newTarget);
        uint32_t function = (target && target->structure()->aotInstance() == &instance ? target : constructor)->functionIndex();
        auto capacity = m_inlineCapacitiesForThis.find(function);
        return constructEmptyObject(globalObject, parent, capacity == m_inlineCapacitiesForThis.end() ? inlineCapacityForThis(constructor->functionIndex(), function, JSFinalObject::defaultInlineCapacity) : capacity->value);
    }
    if (!made->shape) {
        noteViolation(vm, { .kind = "construct-new-target"_s, .on = made });
        throwTypeError(globalObject, scope, withAOTErrorNote(makeString("Cannot construct "_s, nameOf(*made), ": no TypeScript code of the program calls 'new "_s, nameOf(*made), "', so the class was compiled without instances"_s)));
        return { };
    }
    return createObjectOfClass(vm, *made, parent);
}

Structure* State::structureForThis(JSGlobalObject* globalObject, Closure* constructor, JSObject* newTarget, unsigned inlineCapacity)
{
    VM& vm = globalObject->vm();
    // The `prototype` of anything else may be a getter, or a trap.
    auto* target = dynamicDowncast<Closure>(newTarget);
    if (!target || target->structure()->aotInstance() != &instance)
        return nullptr;
    const LayoutData* ofClass = constructor->classData();
    if (ofClass ? ofClass->hasForeignRoot || ofClass->isLaidOut || baseOf(*ofClass) : !!allocationOfThis(constructor->functionIndex()))
        return nullptr;
    unsigned attributes = 0;
    JSValue prototype = target->getDirect(vm, vm.propertyNames->prototype, attributes);
    // Made now, or what an unmade class object shows: a data property either way. The class of the constructor is at hand, and as a rule that is the new target.
    if (!prototype)
        prototype = target == constructor && ofClass && makesClassObjectWhenChanged(*ofClass) ? JSValue(prototypeOfClass(vm, ofClass->index)) : target->prototypeProperty(vm);
    if (prototype && attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return nullptr;
    JSObject* parent = prototype && prototype.isObject() ? asObject(prototype) : globalObject->objectPrototype();
    unsigned capacity = m_inlineCapacitiesForThis.ensure(target->functionIndex(), [&] {
        return inlineCapacityForThis(constructor->functionIndex(), target->functionIndex(), inlineCapacity);
    }).iterator->value;
    return globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, parent, capacity);
}

unsigned State::inlineCapacityForThis(uint32_t constructor, uint32_t newTarget, unsigned seen) const
{
    std::span counts { data.at<std::array<uint32_t, 2>>(data.propertyCountsOfThisOffset), data.numberOfPropertyCountsOfThis };
    auto countOf = [&](uint32_t function) -> uint32_t {
        auto found = std::ranges::lower_bound(counts, function, { }, [](auto& functionAndCount) {
            return functionAndCount[0];
        });
        return found != counts.end() && (*found)[0] == function ? (*found)[1] : 0;
    };
    uint32_t ofTarget = countOf(newTarget);
    unsigned given = ofTarget & ~Data::propertyCountIsOfWholeChain;
    unsigned givenByConstructor = constructor == newTarget ? 0 : countOf(constructor) & ~Data::propertyCountIsOfWholeChain;
    given = ofTarget & Data::propertyCountIsOfWholeChain ? std::max(given, givenByConstructor) : given + givenByConstructor;
    seen = std::min<unsigned>(std::max(seen, given), JSFinalObject::maxInlineCapacity);
    size_t room = (MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(seen)) - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue);
    return std::min<size_t>(room, JSFinalObject::maxInlineCapacity);
}

const AllocationData* State::allocationOfThis(uint32_t function) const
{
    return Typed::allocationOfThis(data, function);
}

bool State::refusesFreshObject(uint32_t function, uint32_t shape) const
{
    std::span refused { data.at<std::array<uint32_t, 2>>(data.refusedFreshObjectsOffset), data.numberOfRefusedFreshObjects };
    return std::ranges::binary_search(refused, std::array { function, shape });
}

uint32_t State::makerFlags(uint32_t function) const
{
    static_assert(!(isMaker & (Data::makesObjectsOfFlaggedLayout | Data::makesObjectsWithDefinitelyInitializedFields)));
    std::span makers { data.at<uint32_t>(data.makersOffset), data.numberOfMakers };
    auto found = std::ranges::lower_bound(makers, function << 2);
    return found != makers.end() && *found >> 2 == function ? isMaker | (*found & 3) : 0;
}

bool State::hasGuardedPrototype(uint32_t function)
{
    return makesLaidOutObjects(function);
}

bool State::isReservedFieldName(VM&, UniquedStringImpl* name, uint32_t function)
{
    auto number = numberOfFieldName(name);
    if (!number)
        return false;
    if (std::ranges::binary_search(std::span { data.at<uint32_t>(data.functionsThatKeepFieldNamesOffTheirPrototypesOffset), data.numberOfFunctionsThatKeepFieldNamesOffTheirPrototypes }, function))
        return true;
    return std::ranges::binary_search(std::span { data.at<uint32_t>(data.reservedFieldNamesOffset), data.numberOfReservedFieldNames }, *number);
}

Structure* State::moduleNamespaceStructure(VM& vm, bool hasPrototypeOfHost)
{
    if (!m_moduleNamespaceStructure) {
        m_moduleNamespaceStructure = ModuleNamespace::createStructure(vm, instance.globalObject, hasPrototypeOfHost);
        didCreate(m_moduleNamespaceStructure);
    }
    return m_moduleNamespaceStructure;
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
    if (m_namedPropertiesTransitionFoundLast < m_namedPropertiesTransitions.size() && isFor(m_namedPropertiesTransitions[m_namedPropertiesTransitionFoundLast])) [[likely]]
        return &m_namedPropertiesTransitions[m_namedPropertiesTransitionFoundLast];
    for (auto& known : m_namedPropertiesTransitions) {
        if (isFor(known)) {
            m_namedPropertiesTransitionFoundLast = &known - m_namedPropertiesTransitions.span().data();
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
        // [[Set]] asks the chain. These two take no property any more and keep what they have, so what they say now they say for good.
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
        transition.elementsGoByVectorLength = hasUndecided(from->indexingType()) || hasInt32(from->indexingType()) || hasDouble(from->indexingType()) || hasContiguous(from->indexingType());
        return to;
    }();
    // No other Structure comes to be at the address of one that is remembered.
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
        static_assert(functionKindOfConstructors == static_cast<uint8_t>(Format::FunctionKind::Constructor));
        for (unsigned i = 0; i < std::size(prototypes); ++i) {
            // Those of the realm are those of the Instance of its default loader: see builtinFunction().
            m_closureStructures[i] = instance.loader() == globalObject->moduleLoader() ? closureStructureOfRealm(vm, globalObject, i) : Closure::createStructure(vm, globalObject, prototypes[i]);
            m_closureStructures[i]->setAOTInstance(&instance);
            instance.typedClosureStructureIDs[i] = m_closureStructures[i]->id().bits();
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

Structure* State::structureOfBoundClosures(VM& vm, uint32_t function)
{
    bool isAsync = kindAndDeclarationOf(function) & functionIsAsync;
    Structure*& structure = m_structuresOfBoundClosures[isAsync];
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

Structure* State::structureOfClosureThatIsNoClassObject(VM& vm, uint32_t function)
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
    if (!(kindAndDeclarationOf(function) & functionIsOfBuiltinModule))
        ensureStaticsOfProgramAreFilled();
    if (makesSlimClosures(function))
        return SlimClosure::create(vm, slimClosureStructure(vm, function), function, std::span { &capture, 1 }, 1);
    return Closure::create(vm, structureOfClosureThatIsNoClassObject(vm, function), function, std::span { &capture, 1 }, 1);
}

JSObject* State::reifyLazyClosure(VM& vm, JSObject* holder, PropertyName name)
{
    unsigned attributes;
    PropertyOffset offset = holder->structure()->get(vm, name, attributes);
    RELEASE_ASSERT(isValidOffset(offset) && attributes & PropertyAttribute::CustomValue);
    uint32_t function = functionOfLazyClosure(holder->getDirect(offset).asCell());
    JSObject* closure = createClosureOver(vm, function, holder);
    holder->putDirect(vm, name, closure, attributes & ~PropertyAttribute::CustomValue);
    return closure;
}

void State::reifyLazyClosures(VM& vm, JSObject* holder)
{
    Structure* structure = holder->structure();
    if (m_functionsOfLazyClosures.isEmpty() || !structure->hasAnyKindOfGetterSetterProperties())
        return;
    Vector<RefPtr<UniquedStringImpl>, 8> names;
    structure->forEachProperty(vm, [&](const auto& entry) {
        if (entry.attributes() & PropertyAttribute::CustomValue && m_functionsOfLazyClosures.contains(holder->getDirect(entry.offset()).asCell()))
            names.append(entry.key());
        return true;
    });
    for (auto& name : names)
        reifyLazyClosure(vm, holder, name.get());
}

CustomGetterSetter* State::lazyClosureCell(VM& vm, uint32_t function)
{
    if (JSCell* cell = closureIfMade(function))
        return uncheckedDowncast<CustomGetterSetter>(cell);
    CustomGetterSetter* cell = CustomGetterSetter::create(vm, lazyClosureGetter, nullptr);
    m_functionsOfLazyClosures.add(cell, function);
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
        holder->putDirect(vm, name, lazyClosureCell(vm, function), ownAttributes | PropertyAttribute::CustomValue);
        holder->structure()->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(false);
        return true;
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

// OrdinaryCreateFromConstructor: the `prototype` of the function if that is an object. It is a data property that cannot be deleted.
JSObject* State::prototypeForObjectsOf(VM& vm, Closure* constructor)
{
    JSValue prototype = constructor->prototypeProperty(vm);
    return prototype.isObject() ? asObject(prototype) : instance.globalObject->objectPrototype();
}

// The function has another `prototype`: what it makes from now on has that.
void State::didChangePrototypeOf(VM& vm, Closure* constructor)
{
    ++instance.typedPrototypeEpoch;
    if (closureIfMade(constructor->functionIndex()) != constructor)
        return;
    if (constructor->isDeclaredAs(functionIsGenerator)) {
        // OrdinaryCreateFromConstructor, with what the realm has for a `prototype` that is no object.
        bool isAsync = constructor->isDeclaredAs(functionIsAsync);
        JSObject* prototype = constructor->getDirect(vm, vm.propertyNames->prototype).getObject();
        if (!prototype)
            prototype = isAsync ? static_cast<JSObject*>(instance.globalObject->asyncGeneratorPrototype()) : instance.globalObject->generatorPrototype();
        setPrototypeOfGenerators(vm, resumeFunctionOf(constructor->functionIndex()), isAsync, prototype);
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
    const LayoutData& layout = layoutOfShape(shape);
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
    if (uint32_t known = structures[shape])
        return std::bit_cast<StructureID>(known).decode();
    Structure* structure = nullptr;
    JSValue prototype = instance.globalObject->objectPrototype();
    switch (layout.kind) {
    case LayoutKind::Closure:
        // Compiled code allocates every closure with it, and then sees to the others: see Instance::typedClosureStructureIDs.
        structure = closureStructures(vm)[0];
        structures[shape] = structure->id().bits();
        return structure;
    case LayoutKind::Environment:
        RELEASE_ASSERT_NOT_REACHED();
    case LayoutKind::Record:
        break;
    case LayoutKind::Instance:
        // Where there is a class object for each evaluation, whoever makes an object says which it is of: see createObjectOfClass().
        if (layout.isEvaluatedOnce)
            prototype = prototypeOfClass(vm, layout.index);
        break;
    }
    structure = Object::createStructure(vm, instance, prototype, &layout);
    structures[shape] = structure->id().bits();
    didCreate(structure);
    return structure;
}

static constexpr uint32_t isKeyOfObjectsWithExtraProperties = 1u << 31;

Structure* State::structureFor(VM& vm, uint32_t shape, JSValue prototype, bool hasExtraProperties)
{
    Structure* usual = structure(vm, shape);
    if (!hasExtraProperties && usual->storedPrototype() == prototype)
        return usual;
    RELEASE_ASSERT(shape < isKeyOfObjectsWithExtraProperties);
    std::pair<uint32_t, JSObject*> key { shape | (hasExtraProperties ? isKeyOfObjectsWithExtraProperties : 0), prototype.getObject() };
    if (Structure* existing = m_structuresWithOtherPrototype.get(key))
        return existing;
    Structure* created = Object::createStructure(vm, instance, prototype, &layoutOfShape(shape));
    if (hasExtraProperties)
        created->markingStampAndFlags() |= Structure::isOfObjectsWithExtraProperties;
    m_structuresWithOtherPrototype.set(key, created);
    return created;
}

bool State::isOfObjectsWithoutExtraProperties(Structure* structure)
{
    uint32_t shape = structure->compiledLayout()->shape;
    return structures[shape] == structure->id().bits() || m_structuresWithOtherPrototype.get({ shape, structure->storedPrototype().getObject() }) == structure;
}

// With orderWordHasExtraProperties where the object has to be asked from the start: see Object::noteWhetherToAsk().
uint32_t State::orderWordAtBirth(VM& vm, const AllocationData& allocation, Structure* structure)
{
    const LayoutData* layout = structure->compiledLayout();
    // `new C() as T`, made as a T: the prototype object of a class has no members then, see fill(), and takes none.
    bool isMadeByClass = allocation.constructedBy != LayoutData::noBase && closure(vm, allocation.constructedBy)->classData();
    bool asks = layout && isObjectLayout(layout->kind) && layout->mayHaveOtherPrototype && !isMadeByClass && !isPrototypeByLayout(*layout, structure->storedPrototype());
    return allocation.orderWord | (asks ? orderWordHasExtraProperties : 0);
}

void State::fill(VM& vm, uint32_t number)
{
    const AllocationData& allocation = this->allocation(number);
    Structure* structure = this->structure(vm, allocation.shape);
    if (allocation.constructedBy != LayoutData::noBase) {
        // For what is on the prototype of a class compiled code goes by the shape, which is not that of the class here.
        if (const LayoutData* ofClass = closure(vm, allocation.constructedBy)->classData(); ofClass && ofClass->shape != allocation.shape)
            RELEASE_ASSERT(!hasPrototypeMembers(*ofClass));
        structure = structureFor(vm, allocation.shape, prototypeForObjectsOf(vm, closure(vm, allocation.constructedBy)));
        didCreate(structure);
    }
    const LayoutData* layout = structure->compiledLayout();
    size_t size = allocation.size;
    Instance::TypedAllocation slot { };
    RELEASE_ASSERT((layoutOfShape(allocation.shape).kind == LayoutKind::Closure) == !structure->isOfCompiledCells());
    slot.header = static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
    if (structure->dataOfCompiledEnvironments()) {
        slot.header &= ~(0xffull << 8 * offsetOfLowByteOfEnvironmentShape | 0xffull << 8 * offsetOfHighByteOfEnvironmentShape);
        slot.header |= static_cast<uint64_t>(allocation.shape & 0xff) << 8 * offsetOfLowByteOfEnvironmentShape | static_cast<uint64_t>(allocation.shape >> 8 & 0xff) << 8 * offsetOfHighByteOfEnvironmentShape;
    }
    slot.words = static_cast<uint64_t>(shapeWordOf(allocation.shape)) << 32 | orderWordAtBirth(vm, allocation, structure);
    CompleteSubspace& space = layout && isObjectLayout(layout->kind) ? vm.cellSpace() : vm.heap.aotClosureSpace;
    LocalAllocator* allocator = space.allocatorFor(size, AllocatorForMode::EnsureAllocator).localAllocator();
    if (allocation.usesFieldTemplate && allocator && layout && isObjectLayout(layout->kind) && allocator->cellSize() > offsetOfFirstField)
        slot.fieldTemplate = fieldTemplate(*layout, allocator->cellSize());
    slot.allocator = allocator;
    if (Instance::TypedAllocation* row = instance.typedAllocation(number)) {
        *row = slot;
        return;
    }
    if (!allocator || m_numberOfAllocationRows == maxNumberOfAllocationRows) {
        m_structureIDsOfAllocationsWithoutRow.set(number, structure->id().bits());
        return;
    }
    uint16_t row = ++m_numberOfAllocationRows;
    *(reinterpret_cast<Instance::TypedAllocation*>(instance.typedAllocationRows) - row) = slot;
    // Compiled code allocates by itself from here on.
    instance.typedAllocationRows[number] = row;
}

uint32_t State::structureIDOf(uint32_t number) const
{
    if (const Instance::TypedAllocation* row = instance.typedAllocation(number))
        return static_cast<uint32_t>(row->header);
    return m_structureIDsOfAllocationsWithoutRow.get(number);
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
    if (const LayoutData& layout = layoutOfShape(allocation(number).shape); layout.kind != LayoutKind::Closure)
        cell = Object::create(vm, structure, layout, allocation(number).size, orderWordAtBirth(vm, allocation(number), structure));
    else
        cell = Closure::createForCompiledCode(vm, structure, allocation(number).size);
    countAllocation(number, cell);
    return cell;
}

uint32_t State::sizeWithRoomToSpecialize(uint32_t shape)
{
    return layoutOfShape(shape).sizeWithRoomToSpecialize();
}

bool State::hasFixedMethod(const LayoutData& layout, UniquedStringImpl* name) const
{
    if (name->isSymbol())
        return false;
    bool isDeclared = false;
    for (const LayoutData* next = &layout; next && !isDeclared; next = baseOf(*next)) {
        if (isFixedIn(next->methods(), name, isDeclared))
            return true;
    }
    return false;
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
    if (name == vm.propertyNames->constructor.impl())
        return closure(vm, ofClass.classData().classConstructor);
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
    return found ? closureObjectFor(vm, found->function) : nullptr;
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

// `#name() {}`, `get #name() {}` of a class that is evaluated once: every object of the class has it, and of a class that extends it. The
// name has been asked for by name().
bool State::findPrivateMethod(VM& vm, const LayoutData& layout, UniquedStringImpl* name, Object::PrivateElement& element)
{
    for (const LayoutData* next = &layout; next; next = baseOf(*next)) {
        for (auto& method : next->methods()) {
            if (!next->isEvaluatedOnce || method.flags & methodHasComputedName || nameIfMade(method.name).impl() != name)
                continue;
            switch (static_cast<Format::MethodKind>(method.kind)) {
            case Format::MethodKind::Method:
                element.kind = Object::PrivateElement::Kind::Method;
                element.value = closureObjectFor(vm, method.function);
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

static constexpr uint64_t hashOfNoFields = 0xcbf29ce484222325ull;

static uint64_t hashWithField(uint64_t hash, uint16_t field)
{
    return (hash ^ (field + 1)) * 0x100000001b3ull;
}

// The two greatest numbers are no keys.
static uint64_t keyOf(uint64_t hash)
{
    return hash >> 1;
}

std::span<const uint16_t> State::fieldsInOrder(const LayoutData& layout, uint32_t order)
{
    if (order >= layout.numberOfOrders)
        return ordersOf(layout).added[order - layout.numberOfOrders].span();
    const uint16_t* lengthAndFields = data.at<uint16_t>(data.at<uint32_t>(layout.ordersOffset)[order]);
    return { lengthAndFields + 1, lengthAndFields[0] };
}

std::optional<std::span<const State::FieldAsProperty>> State::propertiesInOrder(VM& vm, const LayoutData& layout, uint32_t orderWord)
{
    if (orderOf(orderWord) == keysAreInLayoutOrder)
        return std::nullopt;
    auto& table = ordersOf(layout);
    if (table.lastOrderWord == orderWord) [[likely]]
        return table.lastProperties;
    auto& lists = table.propertyLists;
    auto remember = [&](std::span<const FieldAsProperty> properties) {
        table.lastOrderWord = orderWord;
        table.lastProperties = properties;
        return properties;
    };
    if (auto known = lists.find(orderWord); known != lists.end()) [[likely]]
        return remember(known->value.span());
    Vector<FieldAsProperty> list;
    for (uint16_t index : fieldsInOrder(layout, orderOf(orderWord)).first(countOf(orderWord))) {
        if (index >= layout.numberOfFields)
            return std::nullopt;
        auto& field = layout.fields()[index];
        if (!isPrivate(field))
            list.append({ name(vm, field.name).impl(), &field });
    }
    return remember(lists.add(orderWord, FixedVector<FieldAsProperty>(WTF::move(list))).iterator->value.span());
}

const State::KeysAsJSON* State::keysAsJSON(VM& vm, const LayoutData& layout, uint32_t orderWord)
{
    auto& table = ordersOf(layout);
    if (table.orderWordOfLastKeysAsJSON == orderWord) [[likely]]
        return table.lastKeysAsJSON;
    auto properties = propertiesInOrder(vm, layout, orderWord);
    if (!properties)
        return nullptr;
    auto remember = [&](const KeysAsJSON* keys) {
        table.orderWordOfLastKeysAsJSON = orderWord;
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
        made->keys.append({ property.field, start, static_cast<uint32_t>(made->text.size()) - start });
    }
    for (unsigned i = 0; i < KeysAsJSON::copyStride; ++i)
        made->text.append(0);
    result.iterator->value = WTF::move(made);
    return remember(result.iterator->value.get());
}

Structure* State::structureOfPlainObjectWith(VM& vm, std::span<const FieldAsProperty> properties)
{
    JSGlobalObject* globalObject = instance.globalObject;
    // As constructEmptyObject() has it where the properties fit: then it is the Structure that the general way comes to.
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

const State::PlanOfPlainCopyWithout& State::planOfPlainCopyWithout(VM& vm, const LayoutData& layout, uint32_t orderWord, std::span<UniquedStringImpl* const> excluded)
{
    auto& plans = ordersOf(layout).plansOfPlainCopiesWithout.add(orderWord, Vector<std::unique_ptr<PlanOfPlainCopyWithout>, 1> { }).iterator->value;
    for (auto& plan : plans) {
        if (std::ranges::equal(plan->excluded, excluded, [](const RefPtr<UniquedStringImpl>& known, UniquedStringImpl* asked) { return known.get() == asked; }))
            return *plan;
    }
    auto plan = makeUnique<PlanOfPlainCopyWithout>();
    for (UniquedStringImpl* name : excluded)
        plan->excluded.append(name);
    if (auto listed = propertiesInOrder(vm, layout, orderWord)) {
        Vector<FieldAsProperty> kept;
        for (auto& property : *listed) {
            if (std::ranges::find(excluded, property.name) == excluded.end())
                kept.append(property);
        }
        plan->properties = FixedVector<FieldAsProperty>(WTF::move(kept));
        plan->structure = structureOfPlainObjectWith(vm, plan->properties.span());
    }
    plans.append(WTF::move(plan));
    return *plans.last();
}

const State::PlanOfPlainCopy& State::planOfPlainCopy(VM& vm, const LayoutData& layout, uint32_t orderWord)
{
    auto& table = ordersOf(layout);
    if (table.orderWordOfLastPlainCopy == orderWord) [[likely]]
        return table.lastPlainCopy;
    auto remember = [&](const PlanOfPlainCopy& plan) -> const PlanOfPlainCopy& {
        table.orderWordOfLastPlainCopy = orderWord;
        table.lastPlainCopy = plan;
        return table.lastPlainCopy;
    };
    if (auto found = table.plansOfPlainCopies.find(orderWord); found != table.plansOfPlainCopies.end()) [[likely]]
        return remember(found->value);
    auto listed = propertiesInOrder(vm, layout, orderWord);
    std::span<const FieldAsProperty> properties = listed.value_or(std::span<const FieldAsProperty> { });
    Structure* structure = listed ? structureOfPlainObjectWith(vm, properties) : nullptr;
    return remember(table.plansOfPlainCopies.add(orderWord, PlanOfPlainCopy { properties, structure }).iterator->value);
}

JSCellButterfly* State::keysInOrder(VM& vm, const LayoutData& layout, uint32_t orderWord)
{
    if (orderOf(orderWord) == keysAreInLayoutOrder)
        return nullptr;
    if (JSCellButterfly* known = ordersOf(layout).keyLists.get(orderWord))
        return known;
    Vector<uint16_t, 16> listed;
    for (uint16_t index : fieldsInOrder(layout, orderOf(orderWord)).first(countOf(orderWord))) {
        // The place of an extra property that is gone.
        if (index >= layout.numberOfFields)
            return nullptr;
        if (!isPrivate(layout.fields()[index]))
            listed.append(index);
    }
    JSCellButterfly* list = JSCellButterfly::tryCreate(vm, CopyOnWriteArrayWithContiguous, listed.size());
    if (!list)
        return nullptr;
    for (unsigned i = 0; i < listed.size(); ++i)
        list->setIndex(vm, i, jsOwnedString(vm, name(vm, layout.fields()[listed[i]].name).string()));
    didCreate(list);
    ordersOf(layout).keyLists.add(orderWord, list);
    return list;
}

State::OrderTable& State::ordersOf(const LayoutData& layout)
{
    OrderTable* table = m_orderTables.get(layout.shape);
    if (table) [[likely]]
        return *table;
    auto created = makeUnique<OrderTable>();
    table = created.get();
    m_orderTables.add(layout.shape, table);
    m_createdOrderTables.append(WTF::move(created));
    RELEASE_ASSERT(layout.numberOfOrders < keysAreInLayoutOrder);
    return *table;
}

std::optional<uint16_t> State::extraKeyEntry(const LayoutData& layout, UniquedStringImpl* name, bool create)
{
    OrderTable& table = ordersOf(layout);
    auto found = table.indexesOfExtraKeys.find(name);
    if (found != table.indexesOfExtraKeys.end())
        return static_cast<uint16_t>(layout.numberOfFields + found->value);
    size_t index = table.extraKeys.size();
    if (!create || layout.numberOfFields + index >= UINT16_MAX)
        return std::nullopt;
    table.extraKeys.append(Identifier::fromUid(*instance.vm, name));
    table.indexesOfExtraKeys.add(name, static_cast<uint16_t>(index));
    return static_cast<uint16_t>(layout.numberOfFields + index);
}

const Identifier& State::extraKeyName(const LayoutData& layout, uint16_t entry)
{
    return ordersOf(layout).extraKeys[entry - layout.numberOfFields];
}

static uint64_t keyOfPlacement(uint32_t orderAndCount, uint16_t entry, uint32_t position)
{
    static_assert(orderAndCountMask < 1u << 28 && maxFieldsInOrder < 1u << 12);
    return static_cast<uint64_t>(orderAndCount) << 28 | static_cast<uint64_t>(entry) << 12 | std::min(position, maxFieldsInOrder);
}

std::optional<uint32_t> State::orderWithExtraKeyPlaced(const LayoutData& layout, uint32_t orderAndCount, uint16_t entry, uint32_t position)
{
    auto& placements = ordersOf(layout).placements;
    auto found = placements.find(keyOfPlacement(orderAndCount, entry, position));
    return found == placements.end() ? std::nullopt : std::optional(found->value);
}

void State::didPlaceExtraKey(const LayoutData& layout, uint32_t orderAndCount, uint16_t entry, uint32_t position, uint32_t result)
{
    ordersOf(layout).placements.add(keyOfPlacement(orderAndCount, entry, position), result);
}

uint32_t State::orderStartingWith(const LayoutData& layout, std::span<const uint16_t> fields, bool entersPrefixes)
{
    static constexpr uint32_t noOrder = orderWordOf(keysAreInLayoutOrder, 0);
    if (fields.size() > maxFieldsInOrder)
        return noOrder;
    OrderTable& table = ordersOf(layout);
    // Whoever only lists keys has no use for it.
    if (!table.hasPrefixesOfExpectedOrders) {
        table.hasPrefixesOfExpectedOrders = true;
        for (uint16_t order = 0; order < layout.numberOfOrders; ++order) {
            uint64_t hash = hashOfNoFields;
            table.byPrefix.add(keyOf(hash), order);
            for (uint16_t field : fieldsInOrder(layout, order)) {
                hash = hashWithField(hash, field);
                table.byPrefix.add(keyOf(hash), order);
            }
        }
    }
    Vector<uint64_t, 16> hashes { hashOfNoFields };
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
    // An order that has come up when the program ran, and ends where this one goes on, is made longer.
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
    for (uint64_t hash : entersPrefixes ? hashes.span() : hashes.span().last(1))
        table.byPrefix.add(keyOf(hash), static_cast<uint16_t>(order));
    didChangeOrder(layout, table, order);
    return orderWordOf(order, fields.size());
}

void State::didChangeOrder(const LayoutData& layout, OrderTable& table, uint32_t order)
{
    if (table.orderKeys.isEmpty())
        return;
    if (order >= table.slotsInOrder.size()) {
        table.slotsInOrder.grow(order + 1);
        table.orderKeys.grow(order + 2);
    }
    auto& slots = table.slotsInOrder[order];
    auto fields = fieldsInOrder(layout, order);
    slots.shrink(0);
    slots.append(static_cast<uint16_t>(fields.size()));
    static_assert(!noSlotInCell);
    for (uint16_t index : fields)
        slots.append(index < layout.numberOfFields ? static_cast<uint16_t>(slotKeyFor(layout.fields()[index].offset, layout.fields()[index].flags & fieldIsOutOfLine)) : noSlotInCell);
    for (size_t i = 0; i < 2 * (fields.size() + 1); ++i)
        slots.append(noSlotInCell);
    table.orderKeys[0] = table.slotsInOrder.size();
    table.orderKeys[1 + order] = std::bit_cast<uintptr_t>(slots.span().data());
    instance.typedOrderKeys[layout.shape] = table.orderKeys.mutableSpan().data();
}

void State::didFindTransition(const LayoutData& layout, OrderTable& table, uint32_t order, uint32_t count, uint16_t field, uint32_t next)
{
    // Made longer where it is: the field is its next key now.
    if (order >= table.slotsInOrder.size() || next == order || next >= keysAreInLayoutOrder || field >= layout.numberOfFields)
        return;
    auto& created = layout.fields()[field];
    auto& slots = table.slotsInOrder[order];
    uint32_t key = slotKeyFor(created.offset, created.flags & fieldIsOutOfLine);
    if (!key || slots.isEmpty() || count > slots[0])
        return;
    RELEASE_ASSERT(slots.size() == 1u + slots[0] + 2u * (slots[0] + 1u));
    size_t pair = 1 + slots[0] + 2 * count;
    slots[pair] = static_cast<uint16_t>(key);
    slots[pair + 1] = static_cast<uint16_t>(next);
}

const FieldData& State::fieldAt(const LayoutData& layout, uint32_t slotKey)
{
    OrderTable& table = ordersOf(layout);
    if (table.orderKeys.isEmpty()) [[unlikely]] {
        table.orderKeys.append(0);
        instance.typedOrderKeys[layout.shape] = table.orderKeys.mutableSpan().data();
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
        else if (auto entry = extraKeyEntry(to, extraKeyName(from, index).impl(), true))
            order.append(*entry);
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
    OrderTable& table = ordersOf(layout);
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

// What becomes of an object that is cast. A plain data object is copied into the layout of the type, through its properties. An object
// of the program becomes one of a specialization of its layout where it is. Nothing the program can see is changed before all of it is
// known to work out, and no code of the program runs.
//
// An array is of an array type whatever it holds, and is not walked. An element is tested when it is read, and converted then:
// operationAOTTypedCoerceElement. No cast writes an element, and compiled code counts on that.
//
// A type that admits foreign objects has plain objects in its places as they are, so one more is not copied: it would be two from then
// on. It stays itself, and what its properties hold is coerced where it is. An object that nothing can be made of is of such a type as
// it is, whatever it has: what is read through such a type is tested where it is read. See ASSERTED.md 3. For any other type the cast fails.
//
// Where a value is narrowed, by a type guard or a discriminant, only the second of these is done: nothing is copied, and no plain object
// gets anything stored to it. Compiled code counts on that: see narrowingMayRetag().
// An ordinary object that inherits from Object.prototype or from nothing, and can be extended.
ALWAYS_INLINE std::optional<bool> State::mayBeChangedBelow(JSValue value, const RuntimeType& ofField, bool leavesForeignObjects) const
{
    if (!value.isObject())
        return false;
    JSObject* below = asObject(value);
    switch (below->type()) {
    case FinalObjectType:
        // Neither an object of the program nor a ClassPrototype.
        if (leavesForeignObjects || fitsAsOneOfItsOwn(ofField, below))
            return false;
        return std::nullopt;
    case ObjectType:
        if (Object* compiled = compiledObject(below); compiled && !layoutOfShape(compiled->shape()).numberOfSpecializations())
            return false;
        return std::nullopt;
    default:
        // No plain data: it fits, or it stays, or all fails.
        return false;
    }
}

static bool isPlainDataWhateverItsIntegrity(JSObject* object)
{
    if (object->type() != FinalObjectType)
        return false;
    JSValue prototype = object->getPrototypeDirect();
    return prototype.isNull() || prototype == JSValue(object->realm()->objectPrototype());
}

static bool isPlainData(JSObject* object)
{
    return object->isStructureExtensible() && isPlainDataWhateverItsIntegrity(object);
}

static std::optional<Object::Integrity> integrityOfPlainData(VM& vm, JSObject* object)
{
    Structure* structure = object->structure();
    if (structure->isStructureExtensible())
        return std::nullopt;
    return structure->isFrozen(vm) ? Object::Integrity::Frozen : structure->isSealed(vm) ? Object::Integrity::Sealed : Object::Integrity::NotExtensible;
}

static unsigned attributesOfDataWith(std::optional<Object::Integrity> integrity)
{
    if (!integrity || *integrity == Object::Integrity::NotExtensible)
        return 0;
    return *integrity == Object::Integrity::Frozen ? PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete : static_cast<unsigned>(PropertyAttribute::DontDelete);
}

class Coercion {
public:
    // Coerce: `x as T`, which never turns an object into one of an abstract layout, and makes nothing of one that is not of the program.
    // CoerceFreshObject, CoerceFreshGraph: the same, of what nobody else can hold: plain data is copied: the object in hand, or all that hangs
    // below it as well. Test: nothing becomes of anything.
    enum class Mode : uint8_t { Cast, Narrowing, Coerce, CoerceFreshObject, CoerceFreshGraph, Test };
    // Whether a transition may leave absent a required field that the layout adds. It may where the program casts, or the front end for
    // it: an object is made as less, cast, and then filled. Where a value is only tested, and wherever the run time decides by itself, an
    // object becomes what it has everything for.
    enum class Completeness : uint8_t { MayLackWhatIsAdded, HasWhatIsAdded };
    // Whether failure() says why. That is text, which costs more than all the rest: for whoever throws or reports it.
    enum class Explains : bool { No, Yes };

    Coercion(JSGlobalObject* globalObject, State& state, Mode mode = Mode::Cast, Completeness completeness = Completeness::HasWhatIsAdded, Explains explains = Explains::Yes)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_state(state)
        , m_mode(mode)
        , m_completeness(completeness)
        , m_explains(explains == Explains::Yes)
    {
    }

    // Empty: it cannot be done, see failure(), or an exception has been thrown.
    JSValue run(JSValue value, const RuntimeType& type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue result = coerce(value, type);
        RETURN_IF_EXCEPTION(scope, { });
        if (!result)
            return { };
        if (JSObject* copied = whatOthersMayHoldAndWouldBeCopied(m_refusesCopies); copied && (m_refusesCopies || m_state.refusesCopiesOfWhatOthersMayHold())) {
            if (staysBecauseItCannotBecomeTheRecord(copied, type, value))
                return value;
            return failToCopy(copied, type, value);
        }
        commit();
        result = settled(result);
        RELEASE_ASSERT(m_state.fits(type, result) || type.admitsOnlyOtherLayouts());
        bool hasCopiedWhatOthersMayHold = m_copied.containsIf([&](JSObject* object) {
            return object != m_heldByNobodyElse && (m_isOrdinaryCopy.contains(object) || m_becameTheirCopies.isEmpty());
        });
        if (hasCopiedWhatOthersMayHold && m_mode != Mode::CoerceFreshObject && m_mode != Mode::CoerceFreshGraph) [[unlikely]] {
            StringBuilder parts;
            for (auto& [original, ordinary] : m_ordinaryCopies) {
                if (Object* copy = m_copies.get(ordinary))
                    parts.append(parts.isEmpty() ? "copied: "_s : ", "_s, State::describe(JSValue(original)), " as "_s, State::describe(JSValue(copy)));
            }
            String reason = parts.toString();
            m_state.noteViolation(m_vm, { .kind = "copy-on-enter"_s, .what = m_mode == Mode::Coerce ? "Coerce"_s : m_mode == Mode::Narrowing ? "Flow"_s : "As"_s, .expected = &type, .got = value, .reason = reason.isEmpty() ? nullptr : &reason, .action = State::ActionOnViolation::Copied });
        }
        return result;
    }

    void allowObjectsToBecomeTheirCopies() { m_objectsMayBecomeTheirCopies = true; }
    void allowCopiesOfObjectsOfProgram() { m_copiesObjectsOfProgram = true; }
    void refuseCopies() { m_refusesCopies = true; }
    void allowTransitionsToMembers() { m_mayBecomeMembers = true; }
    void noteThatNobodyElseHolds(JSObject* object)
    {
        m_heldByNobodyElse = object;
        m_isOrdinaryCopy.add(object);
    }

    // The object becomes one of the layout, which it can become over some number of transitions: see State::specializeToStore().
    // `stored`: the name of a field of the layout that the caller stores to right after. False: it cannot be done, or an exception
    // has been thrown.
    bool run(Object* object, const LayoutData& layout, UniquedStringImpl* stored)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!mayBecome(object, layout, stored))
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
        if (m_becameTheirCopies.isEmpty() || !value.isCell())
            return value;
        JSObject* object = m_becameTheirCopies.get(value.asCell());
        return object ? JSValue(object) : value;
    }

    JSObject* whatOthersMayHoldAndWouldBeCopied(bool withObjectsOfProgram)
    {
        if (m_mode == Mode::CoerceFreshObject || m_mode == Mode::CoerceFreshGraph)
            return nullptr;
        bool becomeTheirCopies = allCopiedHaveRoom();
        JSObject* first = nullptr;
        for (JSObject* object : m_copied) {
            bool isOfProgram = m_isOrdinaryCopy.contains(object);
            if (m_isCopiedForLackOfRoom.contains(object))
                return object;
            if (object == m_heldByNobodyElse || (isOfProgram ? !withObjectsOfProgram : becomeTheirCopies))
                continue;
            if (isOfProgram || !m_state.hasRoomToBecome(object, m_copies.get(object)))
                return object;
            if (!first)
                first = object;
        }
        return first;
    }

    void recordPathOfCopy(JSObject* object)
    {
        if (!m_explains || m_path.isEmpty())
            return;
        StringBuilder path;
        for (UniquedStringImpl* name : m_path)
            path.append(path.isEmpty() ? ""_s : "."_s, StringView(name));
        m_pathsOfCopies.add(object, path.toString());
    }

    JSValue failToCopy(JSObject* copied, const RuntimeType& type, JSValue cast)
    {
        if (!m_explains || !m_failure.isNull())
            return { };
        Object* copy = m_copies.get(copied);
        bool isBelow = copy && cast != JSValue(copied) && !m_isOrdinaryCopy.contains(copied);
        String name = isBelow ? m_state.nameOf(copy->layout()) : m_state.describe(type);
        String why = m_isOrdinaryCopy.contains(copied) && !m_isCopiedForLackOfRoom.contains(copied) ? "TypeScript code of the program may use it as another declared type"_str : copied->structure()->mayBePrototype() ? "it is the prototype of another object"_str : makeString(name, " has more fields than the object has space for"_s);
        String path = isBelow ? m_pathsOfCopies.get(copied) : String();
        m_failure = makeString(!isBelow ? "the object matches "_str : path.isEmpty() ? "an object it holds matches "_str : makeString("property '"_s, path, "' holds an object that matches "_s), name, ", but it cannot be converted in place ("_s, why, "), and a copy would not see later changes made through other references to it\n  convert the data where it enters the program, before anything else refers to it ('JSON.parse(text) as "_s, name, "'), or declare this place as 'unknown' and check it"_s);
        return { };
    }

    bool allCopiedHaveRoom()
    {
        return m_objectsMayBecomeTheirCopies && std::ranges::all_of(m_copied, [&](JSObject* object) {
            return m_isOrdinaryCopy.contains(object) || m_state.hasRoomToBecome(object, m_copies.get(object));
        });
    }

    void letObjectsBecomeTheirCopies()
    {
        if (!allCopiedHaveRoom())
            return;
        for (JSObject* object : m_copied) {
            if (m_isOrdinaryCopy.contains(object))
                continue;
            Object* copy = m_copies.get(object);
            m_state.become(m_vm, object, copy);
            m_becameTheirCopies.add(copy, object);
        }
        for (JSObject* object : m_copied) {
            Object* holder = m_isOrdinaryCopy.contains(object) ? m_copies.get(object) : uncheckedDowncast<Object>(object);
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
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        letObjectsBecomeTheirCopies();
        for (auto& specialization : m_specializations) {
            Object* object = specialization.object;
            object->specialize(m_vm, m_state, specialization.shape);
            const LayoutData& layout = object->layout();
            auto& moved = specialization.movedIntoFields;
            if (!moved.isEmpty()) {
                // In the order of their creation, which the keys keep: one becomes a field, another gets a place among the keys.
                JSObject* extra = object->extraProperties();
                size_t next = 0;
                for (auto& property : propertiesOf(extra)) {
                    const FieldData* field = next < moved.size() ? &layout.fields()[moved[next].first] : nullptr;
                    if (!field || m_state.name(m_vm, field->name).impl() != property.name) {
                        object->placeExtraKey(m_state, property.name, UINT32_MAX);
                        continue;
                    }
                    // The extra property keeps the value alive until the field has it: this may collect garbage.
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
        // They are writable data properties: there is no transition.
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
        // Extra properties by the names of fields that the shape has more.
        Vector<std::pair<uint16_t, JSValue>, 1> movedIntoFields;
    };
    struct PropertyStore {
        JSObject* object;
        UniquedStringImpl* name;
        JSValue value;
    };
    // How far a plain object is from a layout it can be made one of. The less the better, in this order.
    struct Distance {
        // Properties that hold none of the strings, or of the numbers, that are all the type of the field allows.
        unsigned otherLiterals { 0 };
        // Properties the layout has no field for.
        unsigned extraProperties { 0 };

        friend auto operator<=>(const Distance&, const Distance&) = default;
    };
    uint32_t shapeOf(Object* object) const
    {
        auto planned = m_plannedShapes.find(object);
        return planned == m_plannedShapes.end() ? object->shape() : planned->value;
    }

    // Whether there is nothing to make of the value.
    bool fits(JSValue value, const RuntimeType& type) const
    {
        if (Object* object = m_state.compiledObject(value))
            return type.allowsAnyObject() || m_state.admitsShape(type, shapeOf(object));
        return value.isObject() ? m_state.fitsAsOneOfItsOwn(type, asObject(value)) : m_state.fits(type, value);
    }

    // Nothing is left of an attempt that has failed.
    JSValue orAsItIs(JSValue value, bool mayStayAsItIs, const Invocable<JSValue()> auto& attempt)
    {
        if (!mayStayAsItIs)
            return attempt();
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        size_t numberOfCopies = m_copied.size();
        size_t numberOfSpecializations = m_specializations.size();
        size_t numberOfKeptObjects = m_keptInOrder.size();
        size_t numberOfPropertyStores = m_propertyStores.size();
        size_t lengthOfPath = m_path.size();
        JSValue result = attempt();
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
        for (size_t i = numberOfCopies; i < m_copied.size(); ++i)
            m_copies.remove(m_copied[i]);
        m_copied.shrink(numberOfCopies);
        m_specializations.shrink(numberOfSpecializations);
        m_plannedShapes.clear();
        for (auto& specialization : m_specializations)
            m_plannedShapes.set(specialization.object, specialization.shape);
        for (size_t i = numberOfKeptObjects; i < m_keptInOrder.size(); ++i)
            m_kept.remove(m_keptInOrder[i]);
        m_keptInOrder.shrink(numberOfKeptObjects);
        m_propertyStores.shrink(numberOfPropertyStores);
        m_path.shrink(lengthOfPath);
        m_failure = String();
        return value;
    }

    // "property 'a.b'", or "object" for the value that is being cast.
    String subject() const
    {
        if (m_path.isEmpty())
            return "object"_s;
        StringBuilder path;
        for (UniquedStringImpl* name : m_path)
            path.append(path.isEmpty() ? ""_s : "."_s, StringView(name));
        return makeString("property '"_s, path.toString(), '\'');
    }

    // The first failure is the reason. What `predicate` yields completes a sentence that starts with subject().
    JSValue fail(const Invocable<String()> auto& predicate)
    {
        if (m_explains && m_failure.isNull())
            m_failure = makeString(subject(), ' ', predicate());
        return { };
    }

    JSValue failToFit(JSValue value, const RuntimeType& type)
    {
        if (!m_explains || !m_failure.isNull())
            return { };
        // About the cast value itself the first line of the message says this already.
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
            return m_state.fits(type, value) ? value : failToFit(value, type);
        if (!m_vm.isSafeToRecurse()) [[unlikely]] {
            throwStackOverflowError(m_globalObject, scope);
            return { };
        }
        if (Object* object = m_state.compiledObject(value)) {
            bool isOfType = fits(value, type);
            if (isOfType && (m_mode == Mode::Test || !shouldSpecialize(object, type)))
                return value;
            // Nothing can be made of an object whose layout has no specializations: forEachSpecialization().
            if (m_mode != Mode::Test && m_state.layoutOfShape(shapeOf(object)).numberOfSpecializations()) {
                JSValue result = orAsItIs(value, isOfType || type.admitsForeignObjectsByNature() || staysIfCopyWouldFit(object, type) || mayCopyObjectOfProgram(object), [&] {
                    SetForScope copiesOnlyWhatCannotStay(m_copiesObjectsOfProgram, m_copiesObjectsOfProgram && !isOfType && !type.admitsForeignObjectsByNature());
                    return specialize(object, type);
                });
                RETURN_IF_EXCEPTION(scope, { });
                if (!result || fits(result, type))
                    return result;
            }
            bool stays = staysIfCopyWouldFit(object, type);
            if (type.admitsForeignObjectsByNature() || (!stays && !mayCopyObjectOfProgram(object)))
                return type.admitsForeignObjectsByNature() ? value : failToFit(value, type);
            if (stays && !m_explains) {
                if (auto fitsByName = fitsWhenReadByName(object, type))
                    return *fitsByName ? value : JSValue();
            }
            JSObject* ordinary = m_ordinaryCopies.get(object);
            if (!ordinary) {
                ordinary = object->copyAsOrdinary(m_vm, JSFinalObject::defaultInlineCapacity);
                m_copiesKeptAlive.append(ordinary);
                m_ordinaryCopies.add(object, ordinary);
                m_isOrdinaryCopy.add(ordinary);
                if (m_lackedRoom.contains(object) && object != m_heldByNobodyElse)
                    m_isCopiedForLackOfRoom.add(ordinary);
            }
            if (Object* copy = m_copies.get(ordinary)) {
                if (stays)
                    return value;
                return fits(copy, type) ? JSValue(copy) : failToFit(copy, type);
            }
            if (!stays)
                RELEASE_AND_RETURN(scope, copy(ordinary, type));
            bool copyFits = false;
            String failure;
            {
                SetForScope asCast(m_mode, Mode::Cast);
                SetForScope copiesNothingElse(m_copiesObjectsOfProgram, false);
                orAsItIs(value, true, [&]() -> JSValue {
                    copyFits = !!copy(ordinary, type);
                    failure = m_failure;
                    return { };
                });
            }
            RETURN_IF_EXCEPTION(scope, { });
            if (copyFits)
                return value;
            m_failure = failure;
            return { };
        }
        // getPrototypeOf() hands it out without looking. What follows goes by what kind of object it is and by what it has.
        ClassPrototype::look(asObject(value));
        if (m_mode == Mode::Narrowing)
            return (type.checksForeignObjects() ? fits(value, type) : m_state.fits(type, value)) ? value : failToFit(value, type);
        if (fits(value, type))
            return value;
        // Somebody else may hold it: nothing is made of it. It passes where the type admits it as it is. Else the attempt fails, and State::coerce() hands back the object that was
        // cast. What hangs below a fresh object that is being copied is in the way of the copy then.
        bool onlyTests = m_mode == Mode::Test || m_mode == Mode::Coerce || (m_mode == Mode::CoerceFreshObject && !m_copied.isEmpty());
        JSObject* object = asObject(value);
        if (Object* copy = m_copies.get(object))
            return fits(copy, type) || type.admitsForeignObjectsByNature() ? JSValue(copy) : failToFit(copy, type);
        if (!type.admitsForeignObjectsByNature() && onlyTests)
            return failToFit(value, type);
        if (type.admitsForeignObjectsByNature() && object == m_heldByNobodyElse && m_mode == Mode::Cast) {
            JSValue made = orAsItIs(value, true, [&] {
                SetForScope copiesOnlyWhatCannotStay(m_copiesObjectsOfProgram, false);
                return copy(object, type);
            });
            RETURN_IF_EXCEPTION(scope, { });
            if (made != value)
                return made;
        }
        if (type.admitsForeignObjectsByNature() && (onlyTests || !isPlainObject(object) || !mayConvertProperties(type)))
            return value;
        RELEASE_AND_RETURN(scope, orAsItIs(value, type.admitsForeignObjectsByNature() || mayStayWhereNothingCanBeMadeOfIt(object, type), [&] {
            SetForScope copiesOnlyWhatCannotStay(m_copiesObjectsOfProgram, m_copiesObjectsOfProgram && !type.admitsForeignObjectsByNature());
            return type.admitsForeignObjectsByNature() ? keep(object, type) : copy(object, type);
        }));
    }

    bool mayStayWhereNothingCanBeMadeOfIt(JSObject* object, const RuntimeType& type)
    {
        if (!type.admitsOnlyOtherLayouts() || m_mode != Mode::Cast)
            return false;
        Structure* structure = object->structure();
        bool isAllPlainData = isPlainData(object) && !structure->hasAnyKindOfGetterSetterProperties() && !structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() && !structure->hasNonEnumerableProperties() && !structure->hasNonConfigurableProperties();
        return !isAllPlainData && maySatisfyWhenReadByName(object, type);
    }

    bool maySatisfyWhenReadByName(JSObject* object, const RuntimeType& type)
    {
        bool result = false;
        forEachLayoutToMakeObjectsOf(type, [&](const LayoutData& candidate) {
            for (uint32_t i = 0; i < candidate.numberOfFields; ++i) {
                const FieldData& field = candidate.fields()[i];
                PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
                bool isFound = object->getPropertySlot(m_globalObject, m_state.name(m_vm, field.name), slot);
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

    std::optional<bool> fitsWhenReadByName(Object* object, const RuntimeType& type)
    {
        if (object->orderWord() & (orderWordIsFrozen | orderWordIsSealed | orderWordIsNotExtensible))
            return std::nullopt;
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isNull() && prototype != JSValue(object->realm()->objectPrototype()))
            return std::nullopt;
        const LayoutData& own = m_state.layoutOfShape(shapeOf(object));
        JSObject* extra = object->extraProperties();
        if (extra && hasIndexedProperties(extra->indexingType()))
            return std::nullopt;
        std::optional<bool> result = false;
        forEachLayoutToMakeObjectsOf(type, [&](const LayoutData& candidate) {
            bool fitsCandidate = true;
            for (uint32_t i = 0; i < candidate.numberOfFields && fitsCandidate; ++i) {
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
                    fitsCandidate = field.flags & fieldIsOptional;
                    continue;
                }
                if (fits(held, m_state.ownTypeOf(field)))
                    continue;
                if (held.isObject()) {
                    result = std::nullopt;
                    return IterationStatus::Done;
                }
                fitsCandidate = false;
            }
            if (!fitsCandidate)
                return IterationStatus::Continue;
            result = true;
            return IterationStatus::Done;
        });
        return result;
    }

    bool staysBecauseItCannotBecomeTheRecord(JSObject* copied, const RuntimeType& type, JSValue value) const
    {
        return type.admitsOnlyOtherLayouts() && value.isObject() && !m_state.compiledObject(value) && !m_isOrdinaryCopy.contains(copied);
    }

    bool staysIfCopyWouldFit(Object* object, const RuntimeType& type) const
    {
        return type.admitsOnlyOtherLayouts() && m_state.layoutOfShape(shapeOf(object)).kind == LayoutKind::Record;
    }

    bool mayCopyObjectOfProgram(Object* object) const
    {
        return m_copiesObjectsOfProgram && m_mode == Mode::Cast && m_state.layoutOfShape(shapeOf(object)).kind == LayoutKind::Record;
    }

    // Of an object that is of the type. Every view that admits it is one whose own layout it can still become: it got among the members
    // without having what that layout declares, which would be extra properties for good. An A that is cast to `A | B` stays an A.
    bool shouldSpecialize(Object* object, const RuntimeType& type) const
    {
        uint32_t shape = shapeOf(object);
        const LayoutData& layout = m_state.layoutOfShape(shape);
        if (!layout.numberOfSpecializations() || type.allowsAnyObject())
            return false;
        std::span specializations { m_state.data.at<uint32_t>(layout.specializationsOffset()), layout.numberOfSpecializations() };
        for (uint32_t i = 0; i < type.numberOfViews(); ++i) {
            auto& view = type.view(i);
            if (m_state.passes(view, shape) && !(view.shapeOfView && std::ranges::binary_search(specializations, view.shapeOfView)))
                return false;
        }
        return true;
    }

    // Whether the value is one that coerce() may make something of: for choosing among the parts of a union.
    bool mayBeCoerced(JSValue value, const RuntimeType& type)
    {
        if (fits(value, type))
            return true;
        if (!value.isObject())
            return false;
        if (type.admitsForeignObjectsByNature())
            return true;
        if (Object* object = m_state.compiledObject(value)) {
            bool hasSpecialization = false;
            forEachSpecialization(object, type, [&](const LayoutData&) {
                hasSpecialization = true;
                return IterationStatus::Done;
            });
            if (staysIfCopyWouldFit(object, type))
                return true;
            if (hasSpecialization || !m_reckonsWithCopiesOfObjectsOfProgram || !mayCopyObjectOfProgram(object))
                return hasSpecialization;
            forEachLayoutToMakeObjectsOf(type, [&](const LayoutData&) {
                hasSpecialization = true;
                return IterationStatus::Done;
            });
            return hasSpecialization;
        }
        if (m_mode == Mode::Narrowing)
            return false;
        if (Object* copy = m_copies.get(asObject(value)))
            return fits(copy, type);
        if (asObject(value)->type() != FinalObjectType)
            return false;
        if (m_looksBelow && isPlainDataWhateverItsIntegrity(asObject(value))) {
            SetForScope oneLess(m_looksBelow, m_looksBelow - 1);
            return chooseLayout(asObject(value), propertiesOf(asObject(value)), type, false);
        }
        bool hasLayout = false;
        forEachLayoutToMakeObjectsOf(type, [&](const LayoutData&) {
            hasLayout = true;
            return IterationStatus::Done;
        });
        return hasLayout;
    }

    // What an object may be made one of so that it is of the type: the layout of each view. Nothing is made as what the parts of a large
    // union have in common, which is one view: there it is the members. So it is, if asked for, with an abstract layout: an object whose
    // discriminant is known belongs in the member that lists it.
    void forEachLayoutToMakeObjectsOf(const RuntimeType& type, const Invocable<IterationStatus(const LayoutData&)> auto& functor, bool withMembersOfAbstractLayouts = false) const
    {
        auto isDoneWith = [&](uint32_t shape) {
            const LayoutData& layout = m_state.layoutOfShape(shape);
            return layout.kind == LayoutKind::Record && functor(layout) == IterationStatus::Done;
        };
        for (uint32_t i = 0; i < type.numberOfViews(); ++i) {
            auto& view = type.view(i);
            if (view.shapeOfView) {
                if (isDoneWith(view.shapeOfView))
                    return;
                if (!withMembersOfAbstractLayouts || !m_state.layoutOfShape(view.shapeOfView).isAbstract)
                    continue;
            }
            for (uint32_t shape = view.first; shape < view.first + view.count; ++shape) {
                if (shape != view.shapeOfView && m_state.passes(view, shape) && m_state.hasLayoutOfShape(shape) && isDoneWith(shape))
                    return;
            }
        }
    }

public:
    // What the cast makes of every object that is like this one in its Structure and its discriminant, has no extra properties, has not been
    // frozen, sealed or made non-extensible, and has the room: the shape it becomes one of, or zero, it stays what it is. Nothing: that
    // is not all it goes by, or the cast fails. `staysWhereNothingCanBeMade`: as `x as T` has it, see State::coerce().
    // Where it goes by what one field holds besides, `decides` is told which. `isLikeInThatField`: the objects asked about are like this one
    // in State::classOfValue() of that too.
    std::optional<uint32_t> outcomeByStructure(Object* object, const RuntimeType& type, bool staysWhereNothingCanBeMade, bool isLikeInThatField, DecidingField& decides)
    {
        bool isOfType = fits(object, type);
        if (isOfType && (m_mode == Mode::Test || !shouldSpecialize(object, type)))
            return 0;
        if (m_mode == Mode::Test)
            return std::nullopt;
        Vector<uint32_t, 2> candidates;
        SetForScope asksNothingAboutRoom(m_asksAboutRoom, false);
        forEachSpecialization(object, type, [&](const LayoutData& candidate) {
            if (!candidates.contains(candidate.shape))
                candidates.append(candidate.shape);
            return IterationStatus::Continue;
        });
        if (candidates.size() == 1)
            return candidates[0];
        if (candidates.size() > 1) {
            auto [numberOfFields, field] = fieldsThatTellApart(object, candidates.span());
            if (!numberOfFields || isLikeInThatField)
                return chooseSpecialization(object, type)->shape;
            const LayoutData& layout = object->layout();
            if (numberOfFields == 1 && layout.discriminant() == LayoutData::noBase) {
                decides.field = &layout.fields()[field];
                for (uint32_t shape : candidates) {
                    const FieldData& inCandidate = *m_state.field(m_vm, m_state.layoutOfShape(shape), m_state.name(m_vm, decides.field->name).impl());
                    for (auto& literal : std::span { m_state.data.at<LiteralData>(inCandidate.literalsOffset()), inCandidate.numberOfLiterals() }) {
                        if (literal.isNumber())
                            decides.numbers.add(JSValue::encode(jsNumber(literal.number())));
                        else
                            decides.strings.add(m_state.text(literal.string()), literal.string());
                    }
                }
            }
        }
        if (candidates.isEmpty() && (isOfType || staysWhereNothingCanBeMade || type.admitsForeignObjectsByNature()))
            return 0;
        return std::nullopt;
    }

private:
    bool m_asksAboutRoom { true };
    bool m_mayBecomeMembers { false };
    unsigned m_looksBelow { 0 };

    // What chooseSpecialization() goes by, for objects that are like this one in their layout and their discriminant and have no extra
    // properties. It takes the least distanceTo(), which counts the fields whose value a candidate takes amiss: that comes to the same for
    // all candidates where they hold the field to one type and list no literals for it. How many fields are not like that, and the last.
    std::pair<unsigned, uint32_t> fieldsThatTellApart(Object* object, std::span<const uint32_t> candidates)
    {
        const LayoutData& layout = object->layout();
        std::pair<unsigned, uint32_t> result { 0, 0 };
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            if (i == layout.discriminant())
                continue;
            UniquedStringImpl* name = m_state.name(m_vm, layout.fields()[i].name).impl();
            const RuntimeType* type = nullptr;
            bool tellsApart = false;
            for (uint32_t shape : candidates) {
                const FieldData* field = m_state.field(m_vm, m_state.layoutOfShape(shape), name);
                RELEASE_ASSERT(field);
                tellsApart |= field->numberOfLiterals() || (type && type != &m_state.ownTypeOf(*field));
                type = &m_state.ownTypeOf(*field);
            }
            if (tellsApart)
                result = { result.first + 1, i };
        }
        return result;
    }

    // Whether the object may become one of the layout where it is. Every cast asks this, and every store that makes a transition.
    bool mayBecome(Object* object, const LayoutData& to, UniquedStringImpl* stored = nullptr)
    {
        const LayoutData& from = m_state.layoutOfShape(shapeOf(object));
        // A cast goes by the list, as compiled code does. Whoever stores has looked through the lists of the lists.
        if (!stored && !std::ranges::binary_search(std::span { m_state.data.at<uint32_t>(from.specializationsOffset()), from.numberOfSpecializations() }, to.shape))
            return false;
        // An object of a class keeps its prototype. But for what it finds there compiled code goes by the shape, which it loses.
        if (m_state.hasPrototypeMembers(from) || to.kind != LayoutKind::Record || to.hasExtension != from.hasExtension)
            return false;
        if (isCoercion() && to.isAbstract)
            return false;
        // A discriminant never changes after allocation, so the object can never be of a layout that rules its value out.
        JSValue discriminant = discriminantOf(object, from);
        if (discriminant && to.discriminant() != LayoutData::noBase && !m_state.isAmongLiterals(to.fields()[to.discriminant()], discriminant))
            return false;
        // How much room an object has is settled where it is made.
        if (m_asksAboutRoom && to.size > object->cellSize()) {
            m_state.reportLackOfRoom(from, object->cellSize(), to);
            m_lackedRoom.add(object);
            return false;
        }
        if (m_completeness == Completeness::MayLackWhatIsAdded)
            return true;
        auto& required = m_state.requiredFieldsAdded(from, to);
        // What refers back to the object is taken to be in order, as coerce() takes it.
        if (required.isEmpty() || !m_asked.add(object).isNewEntry)
            return true;
        JSObject* extra = object->extraProperties();
        bool hasWhatIsAdded = std::ranges::all_of(required, [&](uint16_t index) {
            const FieldData& field = to.fields()[index];
            UniquedStringImpl* name = m_state.name(m_vm, field.name).impl();
            if (name == stored)
                return true;
            unsigned attributes;
            JSValue value = extra ? extra->getDirect(m_vm, name, attributes) : JSValue();
            return value && !attributes && mayBeCoerced(value, m_state.ownTypeOf(field));
        });
        m_asked.remove(object);
        return hasWhatIsAdded;
    }

    static JSValue discriminantOf(Object* object, const LayoutData& layout)
    {
        return layout.discriminant() != LayoutData::noBase ? object->read(layout.fields()[layout.discriminant()]) : JSValue();
    }

    // Of what the type offers, what the object can be made one of where it is.
    void forEachSpecialization(Object* object, const RuntimeType& type, const Invocable<IterationStatus(const LayoutData&)> auto& functor)
    {
        const LayoutData& layout = m_state.layoutOfShape(shapeOf(object));
        if (!layout.numberOfSpecializations())
            return;
        bool hasCandidate = false;
        forEachLayoutToMakeObjectsOf(type, [&](const LayoutData& candidate) {
            if (!mayBecome(object, candidate))
                return IterationStatus::Continue;
            hasCandidate = true;
            return functor(candidate);
        }, !!discriminantOf(object, layout));
        if (hasCandidate || !m_mayBecomeMembers)
            return;
        Properties extraProperties;
        if (JSObject* extra = object->extraProperties())
            extraProperties = propertiesOf(extra);
        for (uint32_t shape : std::span { m_state.data.at<uint32_t>(layout.specializationsOffset()), layout.numberOfSpecializations() }) {
            const LayoutData& candidate = m_state.layoutOfShape(shape);
            if (candidate.kind != LayoutKind::Record || !m_state.admitsShape(type, shape) || !mayBecome(object, candidate))
                continue;
            auto distance = distanceTo(object, extraProperties, candidate);
            if (distance && !distance->otherLiterals && functor(candidate) == IterationStatus::Done)
                return;
        }
    }

    // As distanceTo() for a plain object. What the object has in its fields stays there whatever it is, so that rules nothing out.
    // Nothing: an extra property cannot become the field that the layout has by its name.
    std::optional<Distance> distanceTo(Object* object, const Properties& extraProperties, const LayoutData& layout)
    {
        Distance distance;
        const LayoutData& current = object->layout();
        for (uint32_t i = 0; i < current.numberOfFields; ++i) {
            JSValue value = object->read(current.fields()[i]);
            const FieldData* field = value ? m_state.field(m_vm, layout, m_state.name(m_vm, current.fields()[i].name).impl()) : nullptr;
            if (field)
                distance.otherLiterals += !fits(value, m_state.ownTypeOf(*field)) || !m_state.isAmongLiterals(*field, value);
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
            distance.otherLiterals += !m_state.isAmongLiterals(*field, value);
        }
        return distance;
    }

    // Of the parts of a union, as chooseLayout() has it. Null: there is none.
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
        // If none will do, specialize() says why the first will not.
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

    // In the order of their creation.
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
        return chosen ? specializeTo(object, *chosen) : failToFit(object, type);
    }

    JSValue specializeTo(Object* object, const LayoutData& layout)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        uint32_t from = shapeOf(object);
        uint32_t to = layout.shape;
        // Only stores to layouts that may be frozen look at what the object has been made.
        if (auto* own = object->orderWord() & orderWordIsFrozen ? m_state.fieldAttributes.get(object) : nullptr) {
            for (auto& [name, attributes] : own->ofFields) {
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
        // First, for what refers back to the object.
        m_plannedShapes.set(object, to);
        size_t index = m_specializations.size();
        m_specializations.append({ object, to, { } });
        if (JSObject* extra = object->extraProperties()) {
            // As they were created, which is the order they keep as fields.
            for (auto& property : propertiesOf(extra)) {
                const FieldData* field = m_state.field(m_vm, layout, property.name);
                if (!field || m_state.field(m_vm, m_state.layoutOfShape(from), property.name))
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

    // Its properties are looked at one by one.
    bool isPlainObject(JSObject* object) const { return isPlainData(object); }

    // Nothing: the object cannot be made one of the layout. With `explains`, says why. It goes by what the type of the layout says of
    // each property: the slot is shared with the other parts of a union, and holds what any of them does.
    std::optional<Distance> distanceTo(JSObject* object, const Properties& properties, const LayoutData& layout, bool explains)
    {
        Distance distance;
        BitVector isPresent;
        auto integrity = integrityOfPlainData(m_vm, object);
        if (integrity && !layout.mayBeFrozen) {
            if (explains)
                fail([&] { return makeString("is frozen, sealed or non-extensible and cannot be used as "_s, m_state.nameOf(layout), ", which the program stores to"_s); });
            return std::nullopt;
        }
        unsigned attributesOfAll = attributesOfDataWith(integrity);
        for (auto& property : properties) {
            const FieldData* field = m_state.field(m_vm, layout, property.name);
            if (!field) {
                ++distance.extraProperties;
                // A discriminant that nobody stores has no field, and tells the parts of a union apart all the same.
                distance.otherLiterals += !m_state.isAmongLiteralsWithoutField(m_vm, layout, property.name, object->getDirect(property.offset));
                continue;
            }
            isPresent.set(field - layout.fields());
            JSValue value = object->getDirect(property.offset);
            bool hasAttributesOfField = property.attributes == attributesOfAll || (layout.mayBeFrozen && (property.attributes & ~(PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete)) == 0 && (property.attributes & attributesOfAll) == attributesOfAll);
            if (hasAttributesOfField && mayBeCoerced(value, m_state.ownTypeOf(*field))) {
                distance.otherLiterals += !m_state.isAmongLiterals(*field, value);
                continue;
            }
            if (explains) {
                m_path.append(property.name);
                if (!hasAttributesOfField)
                    fail([&] { return String(describeAttributes(property.attributes)); });
                else
                    failToFit(value, m_state.ownTypeOf(*field));
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

    // Of the parts of a union the one whose discriminants the object has. A discriminant that no part knows rules none of them out: the
    // program may well look at it and go on. Then the one that leaves the fewest properties without a field, and of those the first.
    // Null: there is none. With `explains`, says why the first is not it.
    const LayoutData* chooseLayout(JSObject* object, const Properties& properties, const RuntimeType& type, bool explains)
    {
        if (const State::PlanOfCoercion* plan = m_state.planOfCoercion(object, type); plan && plan->chosen) {
            SetForScope below(m_looksBelow, plan->hasRivals && !m_looksBelow ? 3u : m_looksBelow);
            if (distanceTo(object, properties, *plan->chosen, false))
                return plan->chosen;
        }
        const LayoutData* layout = nullptr;
        const LayoutData* firstCandidate = nullptr;
        Distance least;
        unsigned numberThatMayDo = 0;
        forEachLayoutToMakeObjectsOf(type, [&](const LayoutData& candidate) {
            if (!firstCandidate)
                firstCandidate = &candidate;
            auto distance = distanceTo(object, properties, candidate, false);
            numberThatMayDo += !!distance;
            if (distance && (!layout || *distance < least)) {
                least = *distance;
                layout = &candidate;
            }
            return IterationStatus::Continue;
        });
        if (numberThatMayDo > 1 && !m_looksBelow) {
            SetForScope below(m_looksBelow, 3u);
            if (const LayoutData* chosenByWhatIsBelow = chooseLayout(object, properties, type, false))
                return chosenByWhatIsBelow;
        }
        if (!layout && firstCandidate && explains && m_explains)
            distanceTo(object, properties, *firstCandidate, true);
        return layout;
    }

    // Whether keep() can change anything. Only an object is ever made something else, and only for a type with views.
    bool mayConvertProperties(const RuntimeType& type) const
    {
        uintptr_t& asked = m_state.typeAskedAboutProperties(type);
        if ((asked & ~static_cast<uintptr_t>(1)) == std::bit_cast<uintptr_t>(&type))
            return asked & 1;
        bool result = false;
        forEachLayoutToMakeObjectsOf(type, [&](const LayoutData& layout) {
            for (uint32_t i = 0; i < layout.numberOfFields && !result; ++i)
                result = m_state.ownTypeOf(layout.fields()[i]).numberOfViews();
            return result ? IterationStatus::Done : IterationStatus::Continue;
        });
        asked = std::bit_cast<uintptr_t>(&type) | result;
        return result;
    }

    // Whether keep() could change anything: whether it ends well or everything is taken back, the object goes on as it is, so that is all there is to running it. It changes something only through a property whose field has a
    // type with views and which holds a plain object (copied, or walked in its turn) or an object of the program whose layout has specializations (retagged). A primitive is never converted here, and nothing is ever made of
    // any other object: they fit, or the whole fails. By the names of such fields, in every layout that the object could be made one of. Nothing is run, and nothing is kept.
    bool mayChangeSomethingBelow(JSObject* object, const RuntimeType& type)
    {
        // What is looked at besides the object: data that goes round in a circle, or that is reached in many ways, comes to an end so.
        unsigned objectsBelow = 32;
        return mayChangeSomethingBelow(object, type, objectsBelow);
    }

    bool mayChangeSomethingBelow(JSObject* object, const RuntimeType& type, unsigned& objectsBelow)
    {
        static constexpr unsigned maxLayouts = 4;
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        Structure* structure = object->structure();
        bool hasPlannedNothing = m_copies.isEmpty() && m_plannedShapes.isEmpty();
        auto mayChange = [&](JSValue value, const RuntimeType& ofField, bool leavesForeignObjects) {
            if (auto told = hasPlannedNothing ? m_state.mayBeChangedBelow(value, ofField, leavesForeignObjects) : std::nullopt)
                return *told;
            if (!value.isObject())
                return false;
            // From here on in the order of coerce().
            if (Object* compiled = m_state.compiledObject(value)) {
                if (!m_state.layoutOfShape(shapeOf(compiled)).numberOfSpecializations() || (fits(value, ofField) && !shouldSpecialize(compiled, ofField)))
                    return false;
                // It is tried, as a question of which nothing is left: it may be frozen, or have an extra property that cannot become a field.
                bool isSpecialized = false;
                orAsItIs(value, true, [&]() -> JSValue {
                    isSpecialized = !!specialize(compiled, ofField);
                    return { };
                });
                return isSpecialized || scope.exception();
            }
            JSObject* below = asObject(value);
            ClassPrototype::look(below);
            if (fits(value, ofField))
                return false;
            if (!ofField.admitsForeignObjectsByNature() || m_copies.contains(below))
                return isPlainObject(below);
            if (!mayConvertProperties(ofField) || !isPlainObject(below))
                return false;
            // It stays, and is walked in its turn: keep().
            return !objectsBelow || mayChangeSomethingBelow(below, ofField, --objectsBelow) || scope.exception();
        };
        for (unsigned part = 0; auto* entry = m_state.propertiesWithViews(m_vm, structure, type, part); ++part) {
            // A copy: a collection empties the table, and what hangs below may take the place.
            auto known = *entry;
            for (auto& property : known.span()) {
                if (mayChange(object->getDirect(property.offset), *m_state.data.at<RuntimeType>(property.type), property.leavesForeignObjects))
                    return true;
            }
            if (known.isLast)
                return false;
        }
        Vector<State::PropertiesWithViews::Property, State::PropertiesWithViews::maxSize> found;
        bool hasFoundAll = !structure->isDictionary();
        unsigned numberOfLayouts = 0;
        bool result = false;
        forEachLayoutToMakeObjectsOf(type, [&](const LayoutData& layout) {
            result = ++numberOfLayouts > maxLayouts;
            for (uint32_t i = 0; i < layout.numberOfFields && !result; ++i) {
                const FieldData& field = layout.fields()[i];
                const RuntimeType& ofField = m_state.ownTypeOf(field);
                if (!ofField.numberOfViews())
                    continue;
                unsigned attributes = 0;
                PropertyOffset offset = structure->get(m_vm, m_state.name(m_vm, field.name), attributes);
                if (!isValidOffset(offset) || attributes)
                    continue;
                hasFoundAll &= found.size() < State::PropertiesWithViews::maxSize * State::PropertiesWithViews::maxParts;
                // Where no entry is made it is asked if it comes to that.
                bool leavesForeignObjects = hasFoundAll && ofField.admitsForeignObjectsByNature() && !mayConvertProperties(ofField);
                if (hasFoundAll)
                    found.append({ offset, field.ownType(), leavesForeignObjects });
                result = mayChange(object->getDirect(offset), ofField, leavesForeignObjects);
            }
            return result ? IterationStatus::Done : IterationStatus::Continue;
        });
        // It has been through all of them.
        RELEASE_ASSERT(object->structure() == structure);
        if (!result && hasFoundAll) {
            std::span rest = found.span();
            uint8_t part = 0;
            do {
                auto ofPart = rest.first(std::min<size_t>(rest.size(), State::PropertiesWithViews::maxSize));
                rest = rest.subspan(ofPart.size());
                State::PropertiesWithViews entry { structure->id().bits(), static_cast<uint16_t>(ofPart.size()), part++, rest.empty(), &type, { } };
                std::ranges::copy(ofPart, entry.properties);
                m_state.setPropertiesWithViews(m_vm, structure, entry);
            } while (!rest.empty());
        }
        return result;
    }

    // The object stays what it is. What its properties hold becomes what the fields of the layout it would be copied into hold.
    JSValue keep(JSObject* object, const RuntimeType& type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        // An audit goes all the way.
        bool mayChange = m_state.data.isCompiledForAudit || mayChangeSomethingBelow(object, type);
        RETURN_IF_EXCEPTION(scope, { });
        if (!mayChange)
            return object;
        if (!m_kept.add({ object, &type }).isNewEntry)
            return object;
        m_keptInOrder.append({ object, &type });
        // All or nothing: where the property that it failed at the last time cannot be made what its field holds now either, the rest need not be looked at. Nothing is left of the question.
        Structure* structure = object->structure();
        if (auto& known = m_state.failureOfKeeping(m_vm, structure, type); known.structureID == structure->id().bits() && known.type == &type) {
            // By its name: a dictionary changes what it has and stays the Structure it is.
            const FieldData* field = known.field;
            unsigned attributes = 0;
            PropertyOffset offset = structure->get(m_vm, m_state.name(m_vm, field->name), attributes);
            bool stillFails = false;
            if (isValidOffset(offset) && !attributes) {
                orAsItIs(object, true, [&]() -> JSValue {
                    stillFails = !coerce(object->getDirect(offset), m_state.ownTypeOf(*field));
                    return { };
                });
                RETURN_IF_EXCEPTION(scope, { });
            }
            if (stillFails)
                return { };
            m_state.failureOfKeeping(m_vm, structure, type) = { };
        }
        Properties properties = propertiesOf(object);
        const LayoutData* layout = hasCustomProperties(properties) ? nullptr : chooseLayout(object, properties, type, false);
        if (!layout)
            return object;
        if (!m_state.planOfCoercion(object, type) && m_state.takesMorePlansOfCoercions()) {
            State::PlanOfCoercion::Discriminants discriminants;
            auto plan = planFor(object, structure, properties, type, *layout, 0, discriminants);
            plan->layout = nullptr;
            m_state.addPlanOfCoercion(object, structure, type, WTF::move(plan), discriminants);
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
                // Where there is one layout to choose from, which field a property goes with does not depend on what the object holds.
                unsigned numberOfLayouts = 0;
                forEachLayoutToMakeObjectsOf(type, [&](const LayoutData&) {
                    return ++numberOfLayouts > 1 ? IterationStatus::Done : IterationStatus::Continue;
                });
                if (numberOfLayouts == 1 && object->structure() == structure)
                    m_state.failureOfKeeping(m_vm, structure, type) = { structure->id().bits(), &type, field };
                return { };
            }
            m_path.removeLast();
            if (coerced != value)
                m_propertyStores.append({ object, property.name, coerced });
        }
        return object;
    }

    bool choiceGoesByNamesAndWhatIsHeldToLiterals(JSObject* object, const Properties& properties, const RuntimeType& type, const LayoutData& chosen, State::PlanOfCoercion::Discriminants& discriminants, bool& isExact, bool& hasRivals)
    {
        const LayoutData* first = nullptr;
        Distance least;
        BitVector isHeldToLiterals;
        Vector<std::pair<const LayoutData*, Distance>, 4> candidates;
        forEachLayoutToMakeObjectsOf(type, [&](const LayoutData& candidate) {
            Distance distance;
            Vector<const Property*, 4> heldHere;
            BitVector isPresent;
            for (auto& property : properties) {
                const FieldData* field = m_state.field(m_vm, candidate, property.name);
                JSValue value = object->getDirect(property.offset);
                if (!field) {
                    ++distance.extraProperties;
                    if (candidate.literalsWithoutFieldOffset()) {
                        heldHere.append(&property);
                        distance.otherLiterals += !m_state.isAmongLiteralsWithoutField(m_vm, candidate, property.name, value);
                    }
                    continue;
                }
                isPresent.set(field - candidate.fields());
                if (field->numberOfLiterals()) {
                    heldHere.append(&property);
                    distance.otherLiterals += !m_state.isAmongLiterals(*field, value);
                }
            }
            for (uint32_t i = 0; i < candidate.numberOfFields; ++i) {
                if (!(candidate.fields()[i].flags & fieldIsOptional) && !isPresent.get(i))
                    return IterationStatus::Continue;
            }
            for (const Property* property : heldHere)
                isHeldToLiterals.set(property - properties.begin());
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
            bool takesSameTypes = std::ranges::all_of(properties, [&, candidate = candidate](auto& property) {
                const FieldData* field = m_state.field(m_vm, *first, property.name);
                const FieldData* other = m_state.field(m_vm, *candidate, property.name);
                return field && other ? &m_state.ownTypeOf(*field) == &m_state.ownTypeOf(*other) : field == other;
            });
            if (!takesSameTypes)
                return false;
        }
        if (isHeldToLiterals.bitCount() > State::PlanOfCoercion::maxNumberOfDiscriminants)
            return false;
        for (size_t index : isHeldToLiterals)
            discriminants.append(properties[index].offset);
        isExact = !least.extraProperties && !least.otherLiterals;
        hasRivals = candidates.size() > 1;
        return first == &chosen;
    }

    std::unique_ptr<State::PlanOfCoercion> planFor(JSObject* object, Structure* structure, const Properties& properties, const RuntimeType& type, const LayoutData& layout, uint32_t orderAndCount, State::PlanOfCoercion::Discriminants& discriminants)
    {
        auto plan = makeUnique<State::PlanOfCoercion>();
        if (object->structure() != structure || structure->isDictionary() || structure->hasPolyProto() || hasIndexedProperties(structure->indexingType()))
            return plan;
        bool isExact = false;
        if (properties.containsIf([](auto& property) { return !!property.attributes; }) || !choiceGoesByNamesAndWhatIsHeldToLiterals(object, properties, type, layout, discriminants, isExact, plan->hasRivals))
            return plan;
        plan->chosen = &layout;
        if (!isExact)
            return plan;
        for (auto& property : properties)
            plan->fields.append({ property.offset, m_state.field(m_vm, layout, property.name), property.name, 0 });
        for (auto& planned : plan->fields)
            planned.kindsTaken = m_state.kindsTakenBy(m_state.ownTypeOf(*planned.field));
        plan->orderAndCount = orderAndCount;
        plan->layout = &layout;
        if (m_state.sizeWithRoomToSpecialize(layout.shape) <= State::PlanOfCoercion::maxSizeMadeOverWhereItIs && !plan->fields.containsIf([](auto& planned) { return !!(planned.field->flags & fieldIsOutOfLine); })) {
            plan->bodyBeforeStores.fill(0, m_state.sizeWithRoomToSpecialize(layout.shape) - sizeof(JSCell));
            uint8_t* cell = plan->bodyBeforeStores.mutableSpan().data() - sizeof(JSCell);
            for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
                if (const FieldData& field = layout.fields()[i]; !(field.flags & fieldIsOutOfLine))
                    clearSlot(cell + field.offset, field.store());
            }
            *reinterpret_cast<uint32_t*>(cell + offsetOfOrderWord) = (orderWordOf(keysAreInLayoutOrder, 0) & ~orderAndCountMask) | orderAndCount;
            *reinterpret_cast<uint32_t*>(cell + offsetOfShapeWord) = shapeWordOf(layout.shape);
        }
        return plan;
    }

    JSValue copyByPlan(JSObject* object, const State::PlanOfCoercion& plan)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const LayoutData& layout = *plan.layout;
        Structure* structure = m_state.structureFor(m_vm, layout.shape, object->getPrototypeDirect());
        Object* result = Object::create(m_vm, structure, layout, m_state.sizeWithRoomToSpecialize(layout.shape), orderWordOf(keysAreInLayoutOrder, 0));
        m_copiesKeptAlive.append(result);
        m_copiesKeptAlive.append(object);
        if (m_copiesKeptAlive.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(m_globalObject, scope);
            return { };
        }
        m_copies.add(object, result);
        m_copied.append(object);
        recordPathOfCopy(object);
        for (auto& planned : plan.fields) {
            const FieldData* field = planned.field;
            JSValue value = object->getDirect(planned.offset);
            m_path.append(planned.name);
            JSValue coerced = coerce(value, m_state.ownTypeOf(*field));
            RETURN_IF_EXCEPTION(scope, { });
            if (!coerced)
                return { };
            if (!result->store(m_vm, *field, coerced))
                return failToFit(value, m_state.ownTypeOf(*field));
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
        if (!isPlainDataWhateverItsIntegrity(object))
            return failToFit(object, type);
        Structure* structureOfSource = object->structure();
        auto integrity = integrityOfPlainData(m_vm, object);
        const State::PlanOfCoercion* plan = integrity ? nullptr : m_state.planOfCoercion(object, type);
        if (plan && plan->layout) {
            bool holdsWhatMayBeCoerced = std::ranges::all_of(plan->fields, [&](auto& planned) {
                return mayBeCoerced(object->getDirect(planned.offset), m_state.ownTypeOf(*planned.field));
            });
            if (holdsWhatMayBeCoerced)
                RELEASE_AND_RETURN(scope, copyByPlan(object, *plan));
        }
        // A getter and a setter are copied as what they are, which runs neither. No field holds them: see distanceTo().
        Properties properties = propertiesOf(object);
        if (hasCustomProperties(properties))
            return fail([] { return "has native getters and cannot be converted"_str; });
        bool mayChooseAgain = m_copiesObjectsOfProgram && m_mode == Mode::Cast;
        const LayoutData* layout = chooseLayout(object, properties, type, !mayChooseAgain);
        if (!layout && mayChooseAgain) {
            SetForScope reckonsWithCopies(m_reckonsWithCopiesOfObjectsOfProgram, true);
            layout = chooseLayout(object, properties, type, true);
        }
        if (!layout)
            return failToFit(object, type);

        Structure* structure = m_state.structureFor(m_vm, layout->shape, object->getPrototypeDirect());
        Object* result = Object::create(m_vm, structure, *layout, m_state.sizeWithRoomToSpecialize(layout->shape), orderWordOf(keysAreInLayoutOrder, 0));
        m_copiesKeptAlive.append(result);
        m_copiesKeptAlive.append(object);
        if (m_copiesKeptAlive.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(m_globalObject, scope);
            return { };
        }
        m_copies.add(object, result);
        m_copied.append(object);
        recordPathOfCopy(object);
        Vector<uint16_t, 16> order;
        // An extra property needs a place among the keys only if a field comes after it: see Object::didCreateField().
        Vector<UniquedStringImpl*, 8> extraKeysWithoutPlace;
        unsigned attributesOfAll = attributesOfDataWith(integrity);
        Vector<std::pair<const FieldData*, unsigned>, 2> attributesOfFields;
        for (auto& property : properties) {
            JSValue value = object->getDirect(property.offset);
            const FieldData* field = m_state.field(m_vm, *layout, property.name);
            if (!field) {
                result->willAddExtraProperty(m_vm, property.name);
                if (property.attributes & PropertyAttribute::Accessor)
                    result->ensureExtraProperties(m_vm)->putDirectAccessor(m_globalObject, property.name, uncheckedDowncast<GetterSetter>(value), property.attributes);
                else
                    result->ensureExtraProperties(m_vm)->putDirect(m_vm, property.name, value, property.attributes);
                if (!property.name->isSymbol())
                    extraKeysWithoutPlace.append(property.name);
                continue;
            }
            m_path.append(property.name);
            JSValue coerced = coerce(value, m_state.ownTypeOf(*field));
            RETURN_IF_EXCEPTION(scope, { });
            if (!coerced)
                return { };
            if (!result->store(m_vm, *field, coerced))
                return failToFit(value, m_state.ownTypeOf(*field));
            if (property.attributes != attributesOfAll)
                attributesOfFields.append({ field, property.attributes & ~attributesOfAll });
            m_path.removeLast();
            for (UniquedStringImpl* name : std::exchange(extraKeysWithoutPlace, { })) {
                if (auto entry = m_state.extraKeyEntry(*layout, name, true))
                    order.append(*entry);
            }
            order.append(field - layout->fields());
        }
        // Rather the fields in their order than nothing.
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
        uint32_t orderAndCount = m_state.orderStartingWith(*layout, order.span());
        result->setOrderAndCount(orderAndCount);
        m_state.countObject(result);
        for (auto [field, attributes] : attributesOfFields)
            result->setAttributesOf(m_vm, *field, attributes);
        if (integrity) {
            RELEASE_ASSERT(layout->mayBeFrozen);
            result->setIntegrity(m_globalObject, *integrity);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (integrity || !attributesOfFields.isEmpty())
            return result;
        if (!plan && m_state.takesMorePlansOfCoercions()) {
            State::PlanOfCoercion::Discriminants discriminants;
            auto made = planFor(object, structureOfSource, properties, type, *layout, orderAndCount, discriminants);
            m_state.addPlanOfCoercion(object, structureOfSource, type, WTF::move(made), discriminants);
        }
        return result;
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    State& m_state;
    Mode m_mode;
    Completeness m_completeness;
    bool m_explains;
    // See mayBecome().
    UncheckedKeyHashSet<Object*> m_asked;
    // What is reached twice is copied once, so that what was one object is one object, and a cycle a cycle.
    UncheckedKeyHashMap<JSObject*, Object*> m_copies;
    MarkedArgumentBuffer m_copiesKeptAlive;
    bool m_objectsMayBecomeTheirCopies { false };
    UncheckedKeyHashMap<JSCell*, JSObject*> m_becameTheirCopies;
    bool m_copiesObjectsOfProgram { false };
    bool m_refusesCopies { false };
    bool m_reckonsWithCopiesOfObjectsOfProgram { false };
    UncheckedKeyHashMap<Object*, JSObject*> m_ordinaryCopies;
    UncheckedKeyHashSet<JSObject*> m_isOrdinaryCopy;
    UncheckedKeyHashMap<JSObject*, String> m_pathsOfCopies;
    UncheckedKeyHashSet<Object*> m_lackedRoom;
    UncheckedKeyHashSet<JSObject*> m_isCopiedForLackOfRoom;
    JSObject* m_heldByNobodyElse { nullptr };
    UncheckedKeyHashMap<Object*, uint32_t> m_plannedShapes;
    UncheckedKeyHashSet<std::pair<JSObject*, const RuntimeType*>> m_kept;
    // The keys of m_copies and m_kept as they came, for orAsItIs().
    Vector<JSObject*> m_copied;
    Vector<std::pair<JSObject*, const RuntimeType*>> m_keptInOrder;
    Vector<Specialization> m_specializations;
    Vector<PropertyStore> m_propertyStores;
    Vector<UniquedStringImpl*, 8> m_path;
    String m_failure;
};

auto State::fieldsListedBy(VM& vm, const LayoutData& from, const LayoutData& to) -> const FieldsListed&
{
    RELEASE_ASSERT(&from != &to);
    return m_fieldsListed.ensure(static_cast<uint64_t>(from.shape) << 32 | to.shape, [&] {
        FieldsListed result;
        for (uint32_t i = 0; i < from.numberOfFields; ++i) {
            const FieldData* there = i == from.discriminant() ? nullptr : field(vm, to, name(vm, from.fields()[i].name).impl());
            if (there && there->numberOfLiterals())
                result.append({ &from.fields()[i], there });
        }
        return result;
    }).iterator->value;
}

bool State::holdsWhatIsListed(Object* object, const FieldsListed& listed) const
{
    return std::ranges::all_of(listed, [&](auto& fields) {
        JSValue held = object->read(*fields.first);
        return !held || isAmongLiterals(*fields.second, held);
    });
}

bool State::store(VM& vm, Object* object, const TransitionsOfStore& known, JSValue value)
{
    for (auto& transition : known.transitions) {
        uint32_t before = object->orderWord();
        if (!fits(ownTypeOf(*transition.created), value) || !holdsWhatIsListed(object, transition.listed) || !object->specialize(vm, *this, *transition.plan))
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
    if (object->orderWord() & ~orderAndCountMask)
        return false;
    Structure* structure = object->structure();
    const LayoutData& from = *structure->compiledLayout();
    EncodedJSValue discriminant = from.discriminant() != LayoutData::noBase ? JSValue::encode(object->read(from.fields()[from.discriminant()])) : 0;
    CachedStore& cached = cachedStore(structure, name, discriminant);
    if (cached.structure != structure || cached.name != name || cached.discriminant != discriminant || !cached.transitions->areReachedByTheChainAlone)
        return false;
    // Stricter than mayInterceptPut() and than the walk of putExtraWithoutRunningCode().
    for (JSValue next = structure->storedPrototype(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        Structure* ofHolder = asObject(next)->structure();
        TypeInfo typeInfo = ofHolder->typeInfo();
        if (ofHolder->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || typeInfo.overridesGetPrototype() || typeInfo.overridesGetOwnPropertySlot() || (typeInfo.overridesPut() && ofHolder->classInfoForCells() != PrototypeObject::info()) || asObject(next)->hasNonReifiedStaticProperties())
            return false;
    }
    return store(vm, object, *cached.transitions, value);
}

State::FailureOfKeeping& State::failureOfKeeping(VM& vm, Structure* structure, const RuntimeType& type)
{
    if (HeapVersion version = vm.heap.objectSpace().newlyAllocatedVersion(); version != m_versionOfFailuresOfKeeping) [[unlikely]] {
        zeroSpan(std::span { m_failuresOfKeeping });
        m_versionOfFailuresOfKeeping = version;
    }
    return m_failuresOfKeeping[placeOf<numberOfFailuresOfKeeping>(static_cast<uint64_t>(structure->id().bits()) << 32 ^ std::bit_cast<uintptr_t>(&type))];
}

std::span<State::PropertiesWithViews> State::placesOpenToPropertiesWithViews(VM& vm, Structure* structure, const RuntimeType& type, unsigned part)
{
    if (HeapVersion version = vm.heap.objectSpace().newlyAllocatedVersion(); version != m_versionOfPropertiesWithViews) [[unlikely]] {
        for (auto& entry : std::span { m_propertiesWithViews, numberOfPropertiesWithViews })
            entry.type = nullptr;
        m_versionOfPropertiesWithViews = version;
    }
    // Both are aligned addresses, out of one page as often as not.
    constexpr unsigned numberOfSets = numberOfPropertiesWithViews / numberOfPlacesOpenToPropertiesWithViews;
    unsigned set = (WTF::intHash(static_cast<uint64_t>(structure->id().bits()) << 32 ^ std::bit_cast<uintptr_t>(&type)) + part) % numberOfSets;
    return std::span { m_propertiesWithViews, numberOfPropertiesWithViews }.subspan(set * numberOfPlacesOpenToPropertiesWithViews, numberOfPlacesOpenToPropertiesWithViews);
}

const State::PropertiesWithViews* State::propertiesWithViews(VM& vm, Structure* structure, const RuntimeType& type, unsigned part)
{
    for (auto& entry : placesOpenToPropertiesWithViews(vm, structure, type, part)) {
        if (entry.isOf(structure, type, part))
            return &entry;
    }
    return nullptr;
}

void State::setPropertiesWithViews(VM& vm, Structure* structure, const PropertiesWithViews& entry)
{
    auto places = placesOpenToPropertiesWithViews(vm, structure, *entry.type, entry.part);
    // Its own, a free one, or each in its turn.
    auto* place = &places[m_nextPlaceTakenFromPropertiesWithViews % numberOfPlacesOpenToPropertiesWithViews];
    bool isTaken = true;
    for (auto& candidate : places) {
        if (candidate.isOf(structure, *entry.type, entry.part) || !candidate.type) {
            place = &candidate;
            isTaken = false;
            break;
        }
    }
    m_nextPlaceTakenFromPropertiesWithViews += isTaken;
    *place = entry;
}

bool State::staysAsItIsBelow(VM& vm, JSObject* object, const RuntimeType& type)
{
    Structure* structure = object->structure();
    for (unsigned part = 0; ; ++part) {
        auto* known = propertiesWithViews(vm, structure, type, part);
        if (!known)
            return false;
        for (auto& property : known->span()) {
            if (mayBeChangedBelow(object->getDirect(property.offset), *data.at<RuntimeType>(property.type), property.leavesForeignObjects) != std::optional(false))
                return false;
        }
        if (known->isLast)
            return true;
    }
}

bool State::entersAsItIs(JSObject* object, const RuntimeType& type) const
{
    return typeAskedAboutProperties(type) == std::bit_cast<uintptr_t>(&type) || !isPlainData(object);
}

static std::optional<StoreTransition::Cleared> clearedSlot(const TransitionPlan::Slot& slot)
{
    switch (slot.store) {
    case Store::Cell:
    case Store::JSValue:
        return StoreTransition::Cleared { slot.offset, 8, static_cast<uint64_t>(JSValue::encode(JSValue())) };
    case Store::Cell32:
    case Store::OptionalCell32:
        return StoreTransition::Cleared { slot.offset, 4, absentCell32 };
    case Store::OptionalBoolean:
        return StoreTransition::Cleared { slot.offset, 1, JSValue::ValueEmpty };
    case Store::OptionalInt64:
        return StoreTransition::Cleared { slot.offset, 8, static_cast<uint64_t>(absentInteger) };
    case Store::OptionalDouble:
        return StoreTransition::Cleared { slot.offset, 8, absentDouble };
    case Store::Int32OrElsewhere:
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
        // storeByKnownTransition() has just found nothing on the chain that could take or refuse a store.
        for (JSValue next = before->storedPrototype(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
            Structure* ofHolder = asObject(next)->structure();
            if (numberOfHolders == StoreTransition::maxHolders || ofHolder->isDictionary() || ofHolder->hasPolyProto())
                return nullptr;
            // Its ID never comes to mean another.
            if (m_structuresOfPlans.add(ofHolder).isNewEntry)
                didCreate(ofHolder);
            made->holders[numberOfHolders] = asObject(next);
            made->structureIDsOfHolders[numberOfHolders++] = ofHolder->id().bits();
        }
        for (auto& slot : transition.plan->addedSlots) {
            if (auto cleared = clearedSlot(slot))
                made->clearedSlots.append(*cleared);
        }
        made->cleared = made->clearedSlots.span().data();
        made->numberOfCleared = made->clearedSlots.size();
        forSites.append(WTF::move(made));
        return forSites.last().get();
    }
    return nullptr;
}

bool State::specializeToStore(JSGlobalObject* globalObject, Object* object, PropertyName name, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const LayoutData& from = object->layout();
    if (!from.numberOfSpecializations() || name.isSymbol())
        return false;
    bool goesByPlans = !(object->orderWord() & ~orderAndCountMask) && !data.isCompiledForAudit;
    JSValue discriminant = from.discriminant() != LayoutData::noBase ? object->read(from.fields()[from.discriminant()]) : JSValue();
    std::tuple<Structure*, RefPtr<UniquedStringImpl>, EncodedJSValue> keyOfPlans { object->structure(), name.uid(), JSValue::encode(discriminant) };
    auto storeByPlans = [&](const TransitionsOfStore& known) {
        cachedStore(object->structure(), name.uid(), JSValue::encode(discriminant)) = { object->structure(), name.uid(), JSValue::encode(discriminant), &known };
        return store(vm, object, known, value);
    };
    if (goesByPlans) {
        if (auto known = m_transitionsOfStores.find(keyOfPlans); known != m_transitionsOfStores.end()) [[likely]]
            return storeByPlans(*known->value);
    }
    // It says what the data say, so nothing is lost with it. A program that stores under ever new names does not keep them all.
    if (m_specializationsWithField.size() >= maxNumberOfOutcomes) [[unlikely]]
        m_specializationsWithField.clear();
    auto& shapes = m_specializationsWithField.ensure({ from.shape, name.uid() }, [&] {
        Vector<uint32_t, 1> result;
        // What the object can become over any number of transitions, nearest first: the lists are not transitive.
        Vector<uint32_t, 16> reached { from.shape };
        UncheckedKeyHashMap<uint32_t, unsigned> distance { { from.shape, 0u } };
        for (size_t at = 0; at < reached.size(); ++at) {
            const LayoutData& via = layoutOfShape(reached[at]);
            for (uint32_t shape : std::span { data.at<uint32_t>(via.specializationsOffset()), via.numberOfSpecializations() }) {
                if (distance.add(shape, distance.get(via.shape) + 1).isNewEntry)
                    reached.append(shape);
            }
        }
        // What goes by the two layouts and the name alone, and is for a store to ask. The rest is Coercion::mayBecome().
        for (uint32_t shape : reached.span().subspan(1)) {
            const LayoutData& to = layoutOfShape(shape);
            const FieldData* created = field(vm, to, name.uid());
            // Compiled code takes it that an immutable field is what it was when it first looked.
            if (to.isAbstract || !created || created->flags & fieldIsImmutable)
                continue;
            // What the layout has more is absent while the object becomes one of it: see Object::specialize().
            bool mayLackWhatIsAdded = true;
            for (uint32_t i = 0; i < to.numberOfFields && mayLackWhatIsAdded; ++i)
                mayLackWhatIsAdded = mayBeAbsent(to.fields()[i]) || field(vm, from, this->name(vm, to.fields()[i].name).impl());
            if (mayLackWhatIsAdded)
                result.append(shape);
        }
        // Then the least commitment. Not by size, which is for placement to decide: what an object becomes does not go by that.
        std::ranges::sort(result, [&](uint32_t a, uint32_t b) {
            return std::tuple { distance.get(a), layoutOfShape(a).numberOfFields, layoutOfShape(a).index } < std::tuple { distance.get(b), layoutOfShape(b).numberOfFields, layoutOfShape(b).index };
        });
        return result;
    }).iterator->value;
    if (goesByPlans) {
        // What the loop below and Coercion::mayBecome() ask of an object without extra properties, but for the value and the room.
        auto known = makeUnique<TransitionsOfStore>();
        auto& transitions = known->transitions;
        known->areReachedByTheChainAlone = !parseIndex(name) && name != vm.propertyNames->underscoreProto && !hasFixedMethod(from, name.uid());
        // The address stands for it in the table from here on, also where nothing can be made of the object.
        if (m_structuresOfPlans.add(object->structure()).isNewEntry)
            didCreate(object->structure());
        for (uint32_t shape : shapes) {
            const LayoutData& to = layoutOfShape(shape);
            bool requiresMore = requiredFieldsAdded(from, to).containsIf([&](uint16_t index) {
                return this->name(vm, to.fields()[index].name).impl() != name.uid();
            });
            if (requiresMore || hasPrototypeMembers(from) || to.kind != LayoutKind::Record || to.hasExtension != from.hasExtension)
                continue;
            if (discriminant && to.discriminant() != LayoutData::noBase && !isAmongLiterals(to.fields()[to.discriminant()], discriminant))
                continue;
            transitions.append({ field(vm, to, name.uid()), &planOfTransition(vm, object, shape), fieldsListedBy(vm, from, to) });
        }
        if (m_transitionsOfStores.size() >= maxNumberOfOutcomes) [[unlikely]] {
            m_transitionsOfStores.clear();
            zeroSpan(std::span { m_cachedStores, numberOfCachedStores });
        }
        return storeByPlans(*m_transitionsOfStores.add(keyOfPlans, WTF::move(known)).iterator->value);
    }
    for (uint32_t shape : shapes) {
        const LayoutData& to = layoutOfShape(shape);
        const FieldData& created = *field(vm, to, name.uid());
        if (!fits(ownTypeOf(created), value))
            continue;
        // Without extra properties the object has nothing of what the layout requires besides. Coercion::mayBecome() would say so at more expense.
        bool requiresMore = requiredFieldsAdded(from, to).containsIf([&](uint16_t index) {
            return this->name(vm, to.fields()[index].name).impl() != name.uid();
        });
        if (requiresMore && !object->extraProperties())
            continue;
        if (!holdsWhatIsListed(object, fieldsListedBy(vm, from, to)))
            continue;
        // Extra properties by the names of other fields of the layout become those fields, or it is not this layout.
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
    // The edges of the subtype relation that the checker takes for casts. Over every other a value is tested, and nothing else comes of it.
    bool isCoercion = reason == Format::CastReason::Coerce || reason == Format::CastReason::CoerceFreshObject || reason == Format::CastReason::CoerceFreshGraph;
    bool isCast = reason == Format::CastReason::As || isCoercion || isNarrowing;
    if (!isCast)
        return Coercion(globalObject, state, Coercion::Mode::Test, Coercion::Completeness::HasWhatIsAdded, explains);
    return Coercion(globalObject, state, isNarrowing ? Coercion::Mode::Narrowing : reason == Format::CastReason::Coerce ? Coercion::Mode::Coerce : reason == Format::CastReason::CoerceFreshObject ? Coercion::Mode::CoerceFreshObject : isCoercion ? Coercion::Mode::CoerceFreshGraph : Coercion::Mode::Cast, Coercion::Completeness::MayLackWhatIsAdded, explains);
}

TransitionPlan* State::transitionPlan(JSGlobalObject* globalObject, Object* object, const RuntimeType& type, Format::CastReason reason)
{
    const LayoutData& from = object->layout();
    EncodedJSValue discriminant = from.discriminant() != LayoutData::noBase ? JSValue::encode(object->read(from.fields()[from.discriminant()])) : 0;
    OutcomeOfCast outcome = outcomeOfCast(globalObject, object, type, reason, discriminant, false);
    m_lastPlanWentByStructureAlone = !outcome.decidedBy && from.discriminant() == LayoutData::noBase;
    if (!outcome.decidedBy) [[likely]]
        return outcome.plan;
    auto classOfWhatItHolds = classOfValue(*outcome.decidedBy, object->read(*outcome.decidedBy->field));
    return classOfWhatItHolds ? outcomeOfCast(globalObject, object, type, reason, *classOfWhatItHolds, true).plan : nullptr;
}

std::optional<EncodedJSValue> State::classOfValue(const DecidingField& deciding, JSValue value)
{
    // No value has such bits: a cell is aligned, a number has high bits.
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
    return codeOf(listed == deciding.strings.end() ? isUnlistedString : listed->value);
}

State::OutcomeOfCast State::outcomeOfCast(JSGlobalObject* globalObject, Object* object, const RuntimeType& type, Format::CastReason reason, EncodedJSValue discriminant, bool isClassOfValue)
{
    VM& vm = globalObject->vm();
    Structure* structure = object->structure();
    uint64_t key = std::bit_cast<uintptr_t>(structure) ^ std::rotl(static_cast<uint64_t>(std::bit_cast<uintptr_t>(&type)), 23) ^ static_cast<uint64_t>(discriminant) * 31 ^ static_cast<uint64_t>(reason) << 57;
    CachedTransition& cached = m_cachedTransitions[placeOf<numberOfCachedTransitions>(key)];
    if (cached.structure == structure && cached.type == &type && cached.discriminant == discriminant && cached.reason == reason) [[likely]]
        return cached.outcome;
    if (m_plansOfCasts.size() >= maxNumberOfOutcomes) [[unlikely]] {
        m_plansOfCasts.clear();
        zeroSpan(std::span { m_cachedTransitions, numberOfCachedTransitions });
        zeroSpan(std::span { m_transitionsMadeBefore, numberOfTransitionsMadeBefore });
        zeroSpan(std::span { m_castTransitions, static_cast<size_t>(1u << CastTransition::bitsOfPlace) });
        m_clearedSlotsOfPlans.clear();
        m_decidingFields.clear();
    }
    OutcomeOfCast outcome = m_plansOfCasts.ensure({ structure, &type, discriminant, static_cast<unsigned>(reason) }, [&]() -> OutcomeOfCast {
        // The address stands for it in the table from here on.
        if (m_structuresOfPlans.add(structure).isNewEntry)
            didCreate(structure);
        auto decides = makeUnique<DecidingField>();
        auto shape = coercionFor(globalObject, *this, reason, Coercion::Explains::No).outcomeByStructure(object, type, reason == Format::CastReason::Coerce, isClassOfValue, *decides);
        if (shape)
            return { *shape ? &planOfTransition(vm, object, *shape) : &m_planToStay, nullptr };
        if (!decides->field)
            return { };
        m_decidingFields.append(WTF::move(decides));
        return { nullptr, m_decidingFields.last().get() };
    }).iterator->value;
    cached = { structure, &type, discriminant, reason, outcome };
    return outcome;
}

const State::PlanOfCoercion* State::planOfCoercion(Structure* structure, const RuntimeType& type) const
{
    if (m_planOfCoercionLastAskedFor.structure == structure && m_planOfCoercionLastAskedFor.type == &type)
        return m_planOfCoercionLastAskedFor.plan;
    auto found = m_plansOfCoercions.find({ structure, &type });
    m_planOfCoercionLastAskedFor = { structure, &type, found == m_plansOfCoercions.end() ? nullptr : found->value.get() };
    return m_planOfCoercionLastAskedFor.plan;
}

const State::PlanOfCoercion* State::planOfCoercion(JSObject* object, const RuntimeType& type) const
{
    const PlanOfCoercion* found = planOfCoercion(object->structure(), type);
    while (found && found->discriminant != invalidOffset) {
        JSValue held = object->getDirect(found->discriminant);
        if (!held.isString() || asString(held)->isRope())
            return found;
        auto next = found->byDiscriminant.find(asString(held)->tryGetValue());
        if (next == found->byDiscriminant.end())
            return found->byDiscriminant.size() < PlanOfCoercion::maxNumberByDiscriminant && m_numberOfPlansOfCoercionsByDiscriminants < maxNumberOfPlansOfCoercionsByDiscriminants ? nullptr : found;
        found = next->value.get();
    }
    return found;
}

void State::addPlanOfCoercion(JSObject* object, Structure* structure, const RuntimeType& type, std::unique_ptr<PlanOfCoercion>&& plan, const PlanOfCoercion::Discriminants& discriminants)
{
    if (m_plansOfCoercions.size() >= maxNumberOfPlansOfCoercions)
        return;
    m_planOfCoercionLastAskedFor = { };
    m_hasAddedPlansOfCoercionsSinceLastCollection = true;
    if (discriminants.isEmpty()) {
        m_plansOfCoercions.add({ structure, &type }, WTF::move(plan));
        return;
    }
    auto goingBy = [](PropertyOffset discriminant) {
        auto made = makeUnique<PlanOfCoercion>();
        made->discriminant = discriminant;
        return made;
    };
    PlanOfCoercion* at = m_plansOfCoercions.ensure({ structure, &type }, [&] {
        return goingBy(discriminants[0]);
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
        if (at->byDiscriminant.size() >= PlanOfCoercion::maxNumberByDiscriminant || m_numberOfPlansOfCoercionsByDiscriminants >= maxNumberOfPlansOfCoercionsByDiscriminants)
            return;
        ++m_numberOfPlansOfCoercionsByDiscriminants;
        bool isLast = i + 1 == discriminants.size();
        at = at->byDiscriminant.add(text->isolatedCopy(), isLast ? WTF::move(plan) : goingBy(discriminants[i + 1])).iterator->value.get();
    }
}

uint8_t State::kindsTakenBy(const RuntimeType& type) const
{
    uint8_t kinds = 0;
    if (type.primitives & Primitive::Int && type.minInteger() <= INT32_MIN && type.maxInteger() >= INT32_MAX)
        kinds |= PlanOfCoercion::takesInt32;
    if (type.primitives & Primitive::String)
        kinds |= PlanOfCoercion::takesString;
    if (type.primitives & Primitive::True)
        kinds |= PlanOfCoercion::takesTrue;
    if (type.primitives & Primitive::False)
        kinds |= PlanOfCoercion::takesFalse;
    if (type.primitives & Primitive::Undefined)
        kinds |= PlanOfCoercion::takesUndefined;
    if (type.primitives & Primitive::Null)
        kinds |= PlanOfCoercion::takesNull;
    if ((type.primitives & Primitive::Number) == Primitive::Number && type.minInteger() <= -maxExactInteger && type.maxInteger() >= maxExactInteger)
        kinds |= PlanOfCoercion::takesDouble;
    return kinds;
}

bool State::hasRoomToBecome(JSObject* object, Object* copy)
{
    return !object->structure()->mayBePrototype() && object->cellSize() >= copy->layout().size;
}

void State::become(VM& vm, JSObject* object, Object* copy)
{
    Structure* structure = copy->structure();
    size_t sizeOfCell = object->cellSize();
    size_t size = std::min<size_t>(sizeWithRoomToSpecialize(copy->shape()), sizeOfCell);
    RELEASE_ASSERT(size >= sizeof(JSCell) && size >= copy->layout().size && size <= copy->cellSize());
    Object* itself = std::bit_cast<Object*>(object);
    JSObject* extra = copy->extraProperties();
    auto move = [&] {
        uint8_t* cell = std::bit_cast<uint8_t*>(object);
        memcpySpan(std::span { cell + sizeof(JSCell), size - sizeof(JSCell) }, std::span { std::bit_cast<const uint8_t*>(copy) + sizeof(JSCell), size - sizeof(JSCell) });
        zeroSpan(std::span { cell + size, sizeOfCell - size });
        object->JSCell::setStructure(vm, structure);
        if (extra) {
            extraProperties.remove(copy);
            extraProperties.set(itself, extra);
        }
    };
    if (vm.heap.mutatorShouldBeFenced()) [[unlikely]] {
        PreventCollectionScope preventCollection(vm.heap);
        move();
    } else
        move();
    if (auto* attributes = fieldAttributes.get(copy)) [[unlikely]] {
        FieldAttributesTable::Attributes moved { attributes->isFrozen, attributes->ofFields };
        fieldAttributes.remove(copy);
        auto& own = fieldAttributes.ensure(itself);
        own.isFrozen = moved.isFrozen;
        own.ofFields = WTF::move(moved.ofFields);
    }
    vm.writeBarrier(object);
    itself->copyNumberBoxes(*this);
    ensureStillAliveHere(copy);
    ensureStillAliveHere(extra);
}

Object* State::tryBecomeOrCopyByPlan(VM& vm, JSObject* object, const PlanOfCoercion& plan, bool mayBecomeIt)
{
    static constexpr size_t maxSize = PlanOfCoercion::maxSizeMadeOverWhereItIs;
    size_t size = plan.bodyBeforeStores.size() + sizeof(JSCell);
    if (plan.bodyBeforeStores.isEmpty())
        return nullptr;
    size_t sizeOfCell = object->cellSize();
    bool becomesIt = mayBecomeIt && !object->structure()->mayBePrototype() && sizeOfCell >= plan.layout->size;
    alignas(sizeof(EncodedJSValue)) uint8_t cellToBe[maxSize];
    memcpySpan(std::span { cellToBe + sizeof(JSCell), size - sizeof(JSCell) }, plan.bodyBeforeStores.span());
    for (auto& planned : plan.fields) {
        const FieldData& field = *planned.field;
        JSValue value = object->getDirect(planned.offset);
        void* slot = cellToBe + field.offset;
        if (field.store() != Store::Int32OrElsewhere) {
            if (!storeAs(field.store(), slot, value))
                return nullptr;
            continue;
        }
        if (value.isInt32() && value.asInt32() >= leastInt32InPlace)
            *static_cast<int32_t*>(slot) = value.asInt32();
        else if (value.isUndefinedOrNull())
            *static_cast<int32_t*>(slot) = value.isUndefined() ? undefinedInt32 : nullInt32;
        else
            return nullptr;
    }
    Structure* structure = structureFor(vm, plan.layout->shape, object->getPrototypeDirect());
    for (auto& planned : plan.fields) {
        if (planned.field->flags & fieldIsPinned) [[unlikely]]
            willAlter(*planned.field, object->getDirect(planned.offset));
    }
    if (!becomesIt) {
        Object* copy = Object::createWithBody(vm, structure, std::span { cellToBe + sizeof(JSCell), size - sizeof(JSCell) });
        ensureStillAliveHere(object);
        countObject(copy);
        return copy;
    }
    auto move = [&] {
        uint8_t* cell = std::bit_cast<uint8_t*>(object);
        size_t moved = std::min(size, sizeOfCell);
        memcpySpan(std::span { cell + sizeof(JSCell), moved - sizeof(JSCell) }, std::span { cellToBe + sizeof(JSCell), moved - sizeof(JSCell) });
        zeroSpan(std::span { cell + moved, sizeOfCell - moved });
        object->JSCell::setStructure(vm, structure);
    };
    if (vm.heap.mutatorShouldBeFenced()) [[unlikely]] {
        PreventCollectionScope preventCollection(vm.heap);
        move();
    } else
        move();
    vm.writeBarrier(object);
    countObject(std::bit_cast<Object*>(object));
    return std::bit_cast<Object*>(object);
}

bool State::tryBecomeByPlans(VM& vm, JSObject* top, const RuntimeType& typeOfTop)
{
    struct MadeOver {
        JSObject* object;
        const PlanOfCoercion* plan;
        size_t body;
        Structure* structure;
    };
    static constexpr unsigned maxNumber = 32;
    Vector<MadeOver, maxNumber> all;
    Vector<uint64_t, 64> bodies;
    auto lay = [&](auto& lay, JSObject* object, const RuntimeType& type) -> bool {
        for (auto& known : all) {
            if (known.object == object)
                return admitsShape(type, known.plan->layout->shape);
        }
        const PlanOfCoercion* plan = planOfCoercion(object, type);
        if (!plan || !plan->layout || plan->bodyBeforeStores.isEmpty() || all.size() >= maxNumber || object->structure()->mayBePrototype() || object->cellSize() < plan->layout->size)
            return false;
        size_t body = bodies.size();
        bodies.grow(body + WTF::roundUpToMultipleOf<sizeof(uint64_t)>(plan->bodyBeforeStores.size()) / sizeof(uint64_t));
        memcpySpan(asMutableByteSpan(bodies.mutableSpan().subspan(body)).first(plan->bodyBeforeStores.size()), plan->bodyBeforeStores.span());
        all.append({ object, plan, body, nullptr });
        for (auto& planned : plan->fields) {
            const FieldData& field = *planned.field;
            JSValue value = object->getDirect(planned.offset);
            if (!(planned.kindsTaken & PlanOfCoercion::kindOf(value))) {
                const RuntimeType& ofField = ownTypeOf(field);
                if (!value.isObject()) {
                    if (!fits(ofField, value))
                        return false;
                } else if (Object* compiled = compiledObject(value)) {
                    bool isOfType = ofField.allowsAnyObject() || admitsShape(ofField, compiled->shape());
                    if (!isOfType || (layoutOfShape(compiled->shape()).numberOfSpecializations() && !ofField.allowsAnyObject()))
                        return false;
                } else if (asObject(value)->classInfo() == ClassPrototype::info())
                    return false;
                else if (!fitsAsOneOfItsOwn(ofField, asObject(value)) && (ofField.admitsForeignObjectsByNature() || asObject(value)->type() != FinalObjectType || !lay(lay, asObject(value), ofField)))
                    return false;
            }
            void* slot = reinterpret_cast<uint8_t*>(bodies.mutableSpan().data() + body) + field.offset - sizeof(JSCell);
            if (field.store() != Store::Int32OrElsewhere) {
                if (!storeAs(field.store(), slot, value))
                    return false;
                continue;
            }
            if (value.isInt32() && value.asInt32() >= leastInt32InPlace)
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
                willAlter(*planned.field, made.object->getDirect(planned.offset));
        }
    }
    auto move = [&] {
        for (auto& made : all) {
            uint8_t* cell = std::bit_cast<uint8_t*>(made.object);
            size_t size = std::min(made.plan->bodyBeforeStores.size() + sizeof(JSCell), made.object->cellSize());
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
    }
    return true;
}

uint64_t State::wordOfDiscriminant(JSValue value)
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

auto State::quickConversion(uint32_t structureID, const RuntimeType& type, uint64_t discriminant, bool makesRoom) -> QuickConversion*
{
    uint32_t hash = structureID * 0x9e3779b1u ^ static_cast<uint32_t>(std::bit_cast<uintptr_t>(&type) >> 3) ^ static_cast<uint32_t>(discriminant * 0x9e3779b97f4a7c15ull >> 32);
    hash ^= hash >> 16;
    QuickConversion* entries = m_quickConversions->data();
    for (unsigned i = 0; i < 4; ++i) {
        QuickConversion* entry = entries + (hash + i) % numberOfQuickConversions;
        if (entry->structureID == structureID && entry->type == &type && entry->discriminant == discriminant)
            return entry;
        if (makesRoom && !entry->structureID)
            return entry;
    }
    return makesRoom ? entries + hash % numberOfQuickConversions : nullptr;
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
    case Store::Int32OrElsewhere:
    case Store::Cell32:
    case Store::OptionalCell32:
        return 4;
    case Store::Int64:
    case Store::Double:
    case Store::Cell:
    case Store::JSValue:
    case Store::OptionalInt64:
    case Store::OptionalDouble:
        return 8;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void State::rememberQuickConversion(Structure* structure, const RuntimeType& type, const PlanOfCoercion& root, const PlanOfCoercion& plan, uint64_t discriminant, Structure* made, size_t sizeOfCell)
{
    size_t sizeOfBody = plan.bodyBeforeStores.size();
    if (m_counts || data.isCompiledForAudit || !sizeOfBody || sizeOfCell % 16 || sizeOfCell < 32 || sizeOfCell > maxSizeOfQuickConversion || plan.fields.size() > PlanOfCoercion::maxNumberOfQuickMoves || structure->outOfLineCapacity())
        return;
    if (structures[plan.layout->shape] != made->id().bits())
        return;
    bool goesByDiscriminant = &root != &plan;
    if (goesByDiscriminant && (!discriminant || !isInlineOffset(root.discriminant)))
        return;
    for (auto& planned : plan.fields) {
        if (!isInlineOffset(planned.offset))
            return;
        if (planned.field->flags & fieldIsPinned && !(goesByDiscriminant && planned.offset == root.discriminant))
            return;
    }
    if (plan.quickBody.size() != sizeOfCell - sizeof(JSCell)) {
        using Move = PlanOfCoercion::QuickMove;
        plan.quickBody.fill(0, sizeOfCell - sizeof(JSCell));
        memcpySpan(plan.quickBody.mutableSpan().first(std::min(sizeOfBody, plan.quickBody.size())), plan.bodyBeforeStores.span().first(std::min(sizeOfBody, plan.quickBody.size())));
        plan.quickMoves.shrink(0);
        for (auto& planned : plan.fields) {
            Store store = planned.field->store();
            uint8_t kinds = planned.kindsTaken;
            Move::Kind kind = Move::Kind::Any;
            if (kinds == PlanOfCoercion::takesString && (store == Store::JSValue || store == Store::Cell))
                kind = Move::Kind::StringAsValue;
            else if (kinds == PlanOfCoercion::takesString && store == Store::Cell32)
                kind = Move::Kind::StringAsCell32;
            else if (kinds == (PlanOfCoercion::takesInt32 | PlanOfCoercion::takesDouble) && store == Store::Int32OrElsewhere)
                kind = Move::Kind::Int32InPlace;
            else if (kinds == (PlanOfCoercion::takesInt32 | PlanOfCoercion::takesDouble) && store == Store::Double)
                kind = Move::Kind::NumberAsDouble;
            else if (kinds == (PlanOfCoercion::takesTrue | PlanOfCoercion::takesFalse) && store == Store::Boolean)
                kind = Move::Kind::BooleanAsByte;
            plan.quickMoves.append({ static_cast<uint16_t>(JSObject::offsetOfInlineStorage() + offsetInInlineStorage(planned.offset) * sizeof(EncodedJSValue)), static_cast<uint16_t>(planned.field->offset), kind, static_cast<uint8_t>(widthOf(store)), kinds, store });
        }
    }
    if (!m_quickConversions)
        m_quickConversions = makeUniqueWithoutFastMallocCheck<std::array<QuickConversion, numberOfQuickConversions>>();
    uint32_t structureID = structure->id().bits();
    if (goesByDiscriminant)
        *quickConversion(structureID, type, 0, true) = { structureID, root.discriminant, &type, 0, nullptr, 0, 0 };
    uint64_t header = static_cast<uint64_t>(made->typeInfoBlob() & 0xffffff) << 32 | made->id().bits();
    *quickConversion(structureID, type, goesByDiscriminant ? discriminant : 0, true) = { structureID, invalidOffset, &type, goesByDiscriminant ? discriminant : 0, &plan, header, static_cast<uint32_t>(sizeOfCell) };
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
        uint64_t discriminant = wordOfDiscriminant(asObject(cell)->getDirect(entry->discriminantOffset));
        if (!discriminant)
            return false;
        entry = quickConversion(structureID, type, discriminant, false);
        if (!entry)
            return false;
    }
    if (!entry->plan || vm.heap.mutatorShouldBeFenced()) [[unlikely]]
        return false;
    using Move = PlanOfCoercion::QuickMove;
    const PlanOfCoercion& plan = *entry->plan;
    uint8_t* bytes = std::bit_cast<uint8_t*>(cell);
    std::span<const Move> moves = plan.quickMoves.span();
    uint64_t staged[PlanOfCoercion::maxNumberOfQuickMoves];
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
        case Move::Kind::Int32InPlace:
            if (!held.isInt32() || held.asInt32() < leastInt32InPlace)
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
            if (!(move.kindsTaken & PlanOfCoercion::kindOf(held)))
                return false;
            staged[i] = 0;
            if (move.store != Store::Int32OrElsewhere) {
                if (!storeAs(move.store, &staged[i], held))
                    return false;
            } else if (held.isInt32() && held.asInt32() >= leastInt32InPlace)
                staged[i] = static_cast<uint32_t>(held.asInt32());
            else if (held.isUndefinedOrNull())
                staged[i] = static_cast<uint32_t>(held.isUndefined() ? undefinedInt32 : nullInt32);
            else
                return false;
            break;
        }
    }
    const uint8_t* body = plan.quickBody.span().data();
    uint8_t* madeBody = bytes + sizeof(JSCell);
    switch (entry->sizeOfCell) {
    case 32:
        memcpy(madeBody, body, 24);
        break;
    case 48:
        memcpy(madeBody, body, 40);
        break;
    case 64:
        memcpy(madeBody, body, 56);
        break;
    case 80:
        memcpy(madeBody, body, 72);
        break;
    case 96:
        memcpy(madeBody, body, 88);
        break;
    case 112:
        memcpy(madeBody, body, 104);
        break;
    case 128:
        memcpy(madeBody, body, 120);
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
    constexpr uint64_t keptOfHeader = 0xffull << 56 | static_cast<uint64_t>(TypeInfoPerCellBit) << 48 | static_cast<uint64_t>(static_cast<uint8_t>(~AllArrayTypesAndHistory)) << 32;
    uint64_t* header = std::bit_cast<uint64_t*>(cell);
    *header = (*header & keptOfHeader) | entry->header;
    return true;
}

Object* State::copyWhatStaysAsItIsByPlan(VM& vm, JSObject* object, const RuntimeType& type, bool mayBecomeIt)
{
    const PlanOfCoercion* plan = planOfCoercion(object, type);
    if (!plan || !plan->layout)
        return nullptr;
    Structure* structureBefore = object->structure();
    const PlanOfCoercion* root = plan;
    uint64_t discriminant = 0;
    bool mayBeQuick = mayBecomeIt && plan->epochOfQuickConversion != m_epochOfQuickConversions;
    if (mayBeQuick) {
        plan->epochOfQuickConversion = m_epochOfQuickConversions;
        root = planOfCoercion(structureBefore, type);
        if (root != plan) {
            JSValue held = object->getDirect(root->discriminant);
            discriminant = wordOfDiscriminant(held);
            auto child = discriminant ? root->byDiscriminant.find(asString(held)->tryGetValue()) : root->byDiscriminant.end();
            mayBeQuick = child != root->byDiscriminant.end() && child->value.get() == plan;
        }
    }
    for (auto& planned : plan->fields) {
        JSValue value = object->getDirect(planned.offset);
        uint8_t kind = PlanOfCoercion::kindOf(value);
        if (planned.kindsTaken & kind) [[likely]]
            continue;
        const RuntimeType& ofField = ownTypeOf(*planned.field);
        if (!value.isObject()) {
            if (!fits(ofField, value))
                return nullptr;
            continue;
        }
        if (Object* compiled = compiledObject(value)) {
            bool isOfType = ofField.allowsAnyObject() || admitsShape(ofField, compiled->shape());
            if (!isOfType || (layoutOfShape(compiled->shape()).numberOfSpecializations() && !ofField.allowsAnyObject()))
                return nullptr;
            continue;
        }
        if (asObject(value)->classInfo() == ClassPrototype::info() || !fitsAsOneOfItsOwn(ofField, asObject(value)))
            return nullptr;
    }
    if (Object* made = tryBecomeOrCopyByPlan(vm, object, *plan, mayBecomeIt)) {
        if (mayBeQuick && JSValue(made) == JSValue(object))
            rememberQuickConversion(structureBefore, type, *root, *plan, discriminant, made->structure(), made->cellSize());
        return made;
    }
    const LayoutData& layout = *plan->layout;
    Object* result = Object::create(vm, structureFor(vm, layout.shape, object->getPrototypeDirect()), layout, sizeWithRoomToSpecialize(layout.shape), orderWordOf(keysAreInLayoutOrder, 0));
    for (auto& planned : plan->fields) {
        if (!result->store(vm, *planned.field, object->getDirect(planned.offset)))
            return nullptr;
    }
    result->setOrderAndCount(plan->orderAndCount);
    ensureStillAliveHere(object);
    countObject(result);
    return result;
}

static unsigned numberOfPlansBelow(const State::PlanOfCoercion& plan)
{
    unsigned number = plan.byDiscriminant.size();
    for (auto& below : plan.byDiscriminant.values())
        number += numberOfPlansBelow(*below);
    return number;
}

JSString* State::keyMadeOf(VM& vm, std::span<const Latin1Character> characters)
{
    if (characters.size() < 2)
        return characters.empty() ? jsEmptyString(vm) : jsSingleCharacterString(vm, characters[0]);
    unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    if (JSString* key = tryFindMadeKey(characters, hash))
        return key;
    return makeKey(vm, characters, hash);
}

JSString* State::textInCase(VM& vm, std::span<const Latin1Character> characters)
{
    ASSERT(characters.size() >= 2);
    unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    uint64_t& entry = m_textsInCase[hash % m_textsInCase.size()];
    if (entry && entry >> 32 == hash) {
        auto* candidate = uncheckedDowncast<JSString>(MarkedBlockCage::decode(static_cast<uint32_t>(entry)));
        if (CompactTable::hasCharacters<Latin1Character>(*candidate->tryGetValueImpl(), characters))
            return candidate;
    }
    JSString* made = jsNontrivialString(vm, String(characters));
    if (MarkedBlockCage::contains(made))
        entry = static_cast<uint64_t>(hash) << 32 | MarkedBlockCage::encode(made);
    return made;
}

JSString* State::makeKey(VM& vm, std::span<const Latin1Character> characters, unsigned hash)
{
    static constexpr size_t leastNumberOfPlaces = 1 << 8;
    static constexpr size_t mostNumberOfPlaces = 1 << 16;
    if (m_madeKeys.isEmpty()) [[unlikely]]
        m_madeKeys.fill(0, leastNumberOfPlaces);
    auto firstPlaceFor = [&](unsigned hash) {
        return (hash & (m_madeKeys.size() - 1)) & ~static_cast<size_t>(1);
    };
    JSString* made = jsNontrivialString(vm, String(characters));
    ASSERT(!made->tryGetValueImpl()->hasHash());
    *std::bit_cast<unsigned*>(std::bit_cast<uintptr_t>(made->tryGetValueImpl()) + StringImpl::flagsOffset()) |= hash << StringImpl::s_flagCount;
    ASSERT(made->tryGetValueImpl()->existingHash() == StringHasher::computeHashAndMaskTop8Bits(characters));
    if (!MarkedBlockCage::contains(made))
        return made;
    auto put = [&](uint64_t entry) {
        unsigned hash = entry >> 32;
        size_t place = firstPlaceFor(hash);
        if (m_madeKeys[place] && !m_madeKeys[place + 1])
            ++place;
        else if (m_madeKeys[place]) {
            place += hash >> 23 & 1;
            ++m_numberOfMadeKeysOverwritten;
        }
        m_madeKeys[place] = entry;
    };
    put(static_cast<uint64_t>(hash) << 32 | MarkedBlockCage::encode(made));
    if (m_numberOfMadeKeysOverwritten > m_madeKeys.size() / 4 && m_madeKeys.size() < mostNumberOfPlaces) {
        Vector<uint64_t> former = std::exchange(m_madeKeys, { });
        m_madeKeys.fill(0, former.size() * 2);
        for (uint64_t entry : former) {
            if (entry)
                put(entry);
        }
        m_numberOfMadeKeysOverwritten = 0;
    }
    return made;
}

void State::rememberFirstKeyByParts(const KeyByParts& taken, JSString* key)
{
    static constexpr size_t leastNumberOfPlaces = 1 << 8;
    if (data.usesWideReferences || !MarkedBlockCage::contains(key))
        return;
    m_keysByParts.fill({ }, leastNumberOfPlaces);
    rememberKeyByParts(taken, key);
}

void State::makeRoomForKeysByParts()
{
    static constexpr size_t mostNumberOfPlaces = 1 << 14;
    auto put = [&](KeyByParts entry, uint32_t hash) {
        size_t place = hash & (m_keysByParts.size() - 1) & ~static_cast<size_t>(1);
        if (m_keysByParts[place].countAndKinds && !m_keysByParts[place + 1].countAndKinds)
            ++place;
        else if (m_keysByParts[place].countAndKinds) {
            place += hash >> 31;
            ++m_numberOfKeysByPartsOverwritten;
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
    if (m_keysByParts.size() < mostNumberOfPlaces) {
        Vector<KeyByParts> former = std::exchange(m_keysByParts, { });
        m_keysByParts.fill({ }, former.size() * 2);
        for (auto& entry : former) {
            if (entry.countAndKinds)
                put(entry, hashOf(entry));
        }
    }
    m_numberOfKeysByPartsOverwritten = 0;
}

void State::noteExtraPropertyRead(Object* object, JSObject* extra, uint32_t name, PropertyOffset offset, unsigned attributes)
{
    Structure* structure = extra->structure();
    if (isCounting() || structure->isDictionary() || (isValidOffset(offset) && attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
        return;
    int32_t displacement = 0;
    if (isValidOffset(offset))
        displacement = isInlineOffset(offset) ? static_cast<int32_t>(JSObject::offsetOfInlineStorage() + offsetInInlineStorage(offset) * sizeof(EncodedJSValue)) : offsetInButterfly(offset) * static_cast<int32_t>(sizeof(EncodedJSValue));
    uint32_t structureID = structure->id().bits();
    uint32_t shapeWord = shapeWordOf(object->shape());
    m_hasNotedExtraPropertyReads = true;
    extraProperties.recentlyRead[ExtraPropertiesOfObject::placeOf(object)] = { object, extra };
    static_cast<PlaceOfExtraProperty*>(instance.typedPlacesOfExtraProperties)[PlaceOfExtraProperty::placeOf(structureID, name, shapeWord)] = { structureID, name, shapeWord, displacement };
}

void State::noteExtraPropertiesOf(Object* object, JSObject* extra)
{
    if (isCounting())
        return;
    m_hasNotedExtraPropertyReads = true;
    extraProperties.recentlyRead[ExtraPropertiesOfObject::placeOf(object)] = { object, extra };
}

void State::forgetExtraPropertyReads()
{
    if (!std::exchange(m_hasNotedExtraPropertyReads, false))
        return;
    zeroSpan(std::span { extraProperties.recentlyRead, static_cast<size_t>(1) << ExtraPropertiesOfObject::bitsOfPlace });
    zeroSpan(std::span { static_cast<PlaceOfExtraProperty*>(instance.typedPlacesOfExtraProperties), static_cast<size_t>(1) << PlaceOfExtraProperty::bitsOfPlace });
}

void State::forgetMadeKeysThatAreDead(VM& vm)
{
    for (uint64_t& entry : m_madeKeys) {
        if (entry && !vm.heap.isMarked(MarkedBlockCage::decode(static_cast<uint32_t>(entry))))
            entry = 0;
    }
    for (uint64_t& entry : m_textsInCase) {
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

void State::forgetPlansOfCoercionsOfDeadStructures(VM& vm, bool newOnly)
{
    m_planOfCoercionLastAskedFor = { };
    if (m_quickConversions) {
        m_quickConversions->fill({ });
        ++m_epochOfQuickConversions;
    }
    if (!std::exchange(m_hasAddedPlansOfCoercionsSinceLastCollection, false) && newOnly)
        return;
    m_plansOfCoercions.removeIf([&](auto& entry) {
        if (vm.heap.isMarked(entry.key.first))
            return false;
        m_numberOfPlansOfCoercionsByDiscriminants -= numberOfPlansBelow(*entry.value);
        return true;
    });
}

const State::PlanOfCopy* State::planOfCopy(uint32_t shape, uint32_t orderWord, uint32_t shapeOfCopy, std::span<UniquedStringImpl* const> excluded) const
{
    auto found = m_plansOfCopies.find({ shape, orderWord, shapeOfCopy });
    if (found == m_plansOfCopies.end())
        return nullptr;
    for (auto& plan : found->value) {
        if (std::ranges::equal(plan->excluded, excluded, [](const RefPtr<UniquedStringImpl>& a, UniquedStringImpl* b) { return a.get() == b; }))
            return plan.get();
    }
    return nullptr;
}

void State::addPlanOfCopy(uint32_t shape, uint32_t orderWord, uint32_t shapeOfCopy, std::unique_ptr<PlanOfCopy>&& plan)
{
    m_plansOfCopies.add({ shape, orderWord, shapeOfCopy }, Vector<std::unique_ptr<PlanOfCopy>, 1> { }).iterator->value.append(WTF::move(plan));
}

TransitionPlan& State::planOfTransition(VM& vm, Object* object, uint32_t shape)
{
    Structure* structure = object->structure();
    return *m_plansOfTransitions.ensure({ structure, shape }, [&] {
        const LayoutData& from = *structure->compiledLayout();
        const LayoutData& to = layoutOfShape(shape);
        RELEASE_ASSERT(from.hasExtension == to.hasExtension);
        auto plan = makeUnique<TransitionPlan>();
        plan->structure = structureFor(vm, shape, object->getPrototypeDirect(), object->hasExtraProperties());
        plan->shapeWord = shapeWordOf(shape);
        plan->size = to.size;
        plan->from = &from;
        plan->to = &to;
        for (uint32_t i = 0; i < from.numberOfFields; ++i) {
            const FieldData& old = from.fields()[i];
            const FieldData* kept = field(vm, to, name(vm, old.name).impl());
            RELEASE_ASSERT(kept && kept->offset == old.offset && kept->store() == old.store() && kept->type() == old.type() && (kept->flags & fieldIsOutOfLine) == (old.flags & fieldIsOutOfLine));
        }
        for (uint32_t i = 0; i < to.numberOfFields; ++i) {
            const FieldData& added = to.fields()[i];
            if (field(vm, from, name(vm, added.name).impl()))
                continue;
            RELEASE_ASSERT(mayBeAbsent(added));
            // Out of line, a slot that has never been stored to is empty, or beyond the size of the extension.
            if (!(added.flags & fieldIsOutOfLine))
                plan->addedSlots.append({ added.offset, added.store() });
        }
        // The address of either stands for it in the tables from here on.
        for (Structure* kept : { structure, plan->structure }) {
            if (m_structuresOfPlans.add(kept).isNewEntry)
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
    static inline bool s_isKeptForHost;

    static bool isWrittenIfAsked(bool isAskedByImage)
    {
        ViolationLog* log = singletonIfAsked(isAskedByImage);
        return log && log->m_file;
    }

    String placesThatThrew()
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

    static ViolationLog* singletonIfAsked(bool isAskedByImage)
    {
        static ViolationLog* log;
        static std::once_flag once;
        std::call_once(once, [&] {
            const char* where = getenv("BUN_AOT_TYPE_ERROR_LOG");
            if ((!where || !*where) && !isAskedByImage && s_isKeptForHost) {
                log = new ViolationLog(nullptr, false);
                return;
            }
            if (!where || !*where)
                where = isAskedByImage ? "stderr" : "off";
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

static JSObject* constructorOfLockedPrototype(VM& vm, JSObject* object)
{
    if (!object->mayBePrototype() || object->isCallable())
        return nullptr;
    unsigned attributes;
    JSValue constructor = object->getDirect(vm, vm.propertyNames->constructor, attributes);
    return constructor && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) && constructor.isCallable() ? asObject(constructor) : nullptr;
}

static String nameOfLockedObject(VM& vm, JSObject* object)
{
    if (object->isGlobalObject())
        return "globalThis"_s;
    if (object == object->globalObject()->functionPrototype())
        return "Function.prototype"_s;
    if (object->isCallable())
        return functionName(object);
    if (JSObject* constructor = constructorOfLockedPrototype(vm, object))
        return makeString(functionName(constructor), ".prototype"_s);
    return object->classInfo()->className;
}

static const void* returnAddressIntoTopCompiledFrame(VM& vm, bool ofItsCaller)
{
    CallFrame* frame = vm.topCallFrame;
    if (!frame)
        return nullptr;
    const void* address = returnAddressForFrame(frame, __builtin_frame_address(0));
    while (address && frame && kindOfAddress(address) == ImageAddressInfo::Stub) {
        address = removeCodePtrTag(frame->rawReturnPC());
        frame = frame->callerFrame();
    }
    if (ofItsCaller && address && frame && kindOfAddress(address) == ImageAddressInfo::Function)
        address = removeCodePtrTag(frame->rawReturnPC());
    return address && kindOfAddress(address) == ImageAddressInfo::Function ? address : nullptr;
}

std::optional<State::ListedSite> State::listedSite(const String& file, LineColumn position, const Violation& violation, ASCIILiteral kind) const
{
    std::span words { data.at<uint32_t>(data.sitesOffset), data.numberOfWordsOfSites };
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
    auto pathOfFile = file.utf8();
    for (uint32_t left = word(); left-- && isWhole;) {
        auto path = text();
        uint32_t numberOfRows = word();
        size_t start = at;
        at += static_cast<size_t>(numberOfRows) * 2;
        isWhole &= at <= words.size();
        if (!isWhole || !equalSpans(byteCast<uint8_t>(pathOfFile.span()), path))
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
            bool isOfKind = listed.kind == kind || (listed.kind == "as-convert"_s && kind == "as-refuse"_s) || (listed.kind == "field-redefine"_s && kind == "field-store-untyped"_s);
            if (kind != kindOfUnnamedViolation && !isOfKind)
                continue;
            if (violation.parameter && listed.kind == "enter-function"_s && listed.ordinal != std::min(*violation.parameter, 15u))
                continue;
            unsigned rankOfRow = 1 + (numberOfReason < reasons.size() && reasons[numberOfReason] == violation.what);
            if (rankOfRow > rank) {
                rank = rankOfRow;
                found = listed;
            }
        }
        if (found && kind != kindOfUnnamedViolation)
            found->kind = kind;
        if (found && violation.parameter && found->kind == "enter-function"_s)
            found->ordinal = *violation.parameter;
        return found;
    }
    return std::nullopt;
}

void State::noteViolation(VM& vm, const Violation& violation)
{
    ViolationLog* log = ViolationLog::singletonIfAsked(data.logsTypeErrors);
    if (!log) [[likely]]
        return;
    ASCIILiteral word = violation.got ? wordForKind(violation.got, violation.expected) : ASCIILiteral();
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
        return ViolationLog::isWrittenIfAsked(data.logsTypeErrors) && ViolationLog::isPowerOfTen(count);
    };
    auto find = [&](const void* address, bool isEntry, const Invocable<std::pair<String, LineColumn>()> auto& place, const Invocable<String()> auto& function) {
        ViolationLog::Address key { address, violation.kind.characters() };
        if (auto known = log->siteAt(key))
            return *known;
        ASCIILiteral kind = isEntry && violation.kind == kindOfUnnamedViolation ? "enter-function"_s : violation.kind;
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
    bool isBehindTheSite = false;
    StackVisitor::visit(vm.topCallFrame, vm, [&](StackVisitor& visitor) {
        if (visitor->isTailDeleted() || (visitor->aotFunction() && visitor->isHiddenAOTFrame(isAtTheTop)))
            return IterationStatus::Continue;
        bool wasAtTheTop = std::exchange(isAtTheTop, false);
        if (!visitor->isTypedAOTFrame())
            return IterationStatus::Continue;
        if (isBehindTheSite) {
            auto [file, position] = placeOf(visitor);
            callers.append(makeString(file, ':', position.line));
            return callers.size() < ViolationLog::maxNumberOfCallers ? IterationStatus::Continue : IterationStatus::Done;
        }
        isBehindTheSite = true;
        auto parameterCheck = violation.kind == kindOfUnnamedViolation ? ProgramData::get()->parameterCheckAt(visitor->aotFunction().index, visitor->bytecodeIndex().offset()) : std::nullopt;
        if (parameterCheck) {
            auto [file, position] = placeOf(visitor);
            callers.append(makeString(file, ':', position.line));
        }
        if (!isCounted) {
            uint32_t function = parameterCheck ? parameterCheck->typedFunction : visitor->typedAOTFunction();
            isCounted = true;
            isWritten = countAt(find(removeCodePtrTag(visitor->returnPC()), parameterCheck || (wasAtTheTop && typedFunctionIsHiddenInStackTraces(function)), [&] {
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
            SetForScope kindsAlone(s_describesKindsAlone, true);
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

void State::noteCastTransition(Structure* before, uint32_t typeAndReason, uint64_t wordsBefore, Object* object, const TransitionPlan& plan)
{
    Structure* after = object->structure();
    if (before->typeInfo().type() != after->typeInfo().type() || before->typeInfo().inlineTypeFlags() != after->typeInfo().inlineTypeFlags() || before->indexingModeIncludingHistory() != after->indexingModeIncludingHistory())
        return;
    CastTransition transition { before->id().bits(), typeAndReason, wordsBefore, object->orderAndShapeWords(), after->id().bits(), 0, nullptr, 0, 0, 0 };
    if (plan.structure) {
        using Cleared = StoreTransition::Cleared;
        auto& cleared = *m_clearedSlotsOfPlans.ensure(&plan, [&] {
            Vector<Cleared, 8> slots;
            for (auto& slot : plan.addedSlots) {
                if (auto clearedSlotOfPlan = clearedSlot(slot))
                    slots.append(*clearedSlotOfPlan);
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

bool State::tryTransitionMadeBefore(VM& vm, JSValue value, uint32_t typeAndReason)
{
    if (!value.isCell())
        return false;
    const RuntimeType& type = this->type(typeAndReason >> 4);
    auto reason = static_cast<Format::CastReason>(typeAndReason & 15);
    Structure* structure = value.asCell()->structure();
    auto& made = transitionMadeBefore(structure, type, reason);
    if (made.structure != structure || made.type != &type || made.reason != reason)
        return false;
    Object* object = uncheckedDowncast<Object>(value.asCell());
    if (object->orderWord() & ~orderAndCountMask)
        return false;
    uint64_t wordsBefore = object->orderAndShapeWords();
    if (made.plan->structure && !object->specializeAsBefore(vm, *made.plan))
        return false;
    noteCastTransition(structure, typeAndReason, wordsBefore, object, *made.plan);
    return true;
}

JSValue State::coerce(JSGlobalObject* globalObject, JSValue value, const RuntimeType& type, Format::CastReason reason, AuditSite site, HandsBack handsBack)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool hasObjects = type.allowsAnyObject() || type.allowsAnyFunction() || type.admitsForeignObjects() || type.numberOfViews() || type.numberOfJSTypes();
    bool isFreshObject = reason == Format::CastReason::AsFreshObject || reason == Format::CastReason::CoerceFreshObject;
    bool isMadeAlsoWhereAdmitted = reason == Format::CastReason::AsFreshObject;
    if (reason == Format::CastReason::AsFreshObject)
        reason = Format::CastReason::As;
    // A coercion like any other, but for what becomes of plain data.
    Format::CastReason asTold = reason;
    if (reason == Format::CastReason::CoerceFreshObject || reason == Format::CastReason::CoerceFreshGraph)
        reason = Format::CastReason::Coerce;
    if (reason == Format::CastReason::Coerce && !(value.isObject() && hasObjects)) {
        countCoercion(value, nullptr, type, reason, "converts"_s, value);
        JSValue converted = convert(globalObject, value, type, site);
        RETURN_IF_EXCEPTION(scope, { });
        if (converted != value)
            noteViolation(vm, { .kind = "as-convert"_s, .what = "Coerce"_s, .expected = &type, .got = value, .action = ActionOnViolation::Converted });
        return converted;
    }
    Object* compiled = compiledObject(value);
    // Nothing is made of an object that somebody else may hold, nor of an object of the engine or of a class that is not laid out, whatever the type: it stays what it is.
    if (!compiled && reason == Format::CastReason::Coerce && (asTold == reason || !isPlainData(asObject(value))) && !data.isCompiledForAudit) {
        countCoercion(value, nullptr, type, reason, "no data"_s, value);
        return value;
    }
    if (!compiled && type.admitsForeignObjectsByNature() && !isMadeAlsoWhereAdmitted && reason != Format::CastReason::Flow && value.isObject() && entersAsItIs(asObject(value), type)) {
        countCoercion(value, nullptr, type, reason, "admitted"_s, value);
        return value;
    }
    if (!compiled && type.admitsForeignObjectsByNature() && !isMadeAlsoWhereAdmitted && reason != Format::CastReason::Flow && value.isObject() && !data.isCompiledForAudit && staysAsItIsBelow(vm, asObject(value), type)) {
        countCoercion(value, nullptr, type, reason, "nothing below"_s, value);
        return value;
    }
    const LayoutData* before = compiled ? &compiled->layout() : nullptr;
    ASCIILiteral way = !compiled ? "foreign"_s : "flags"_s;
    if (Object* object = data.isCompiledForAudit ? nullptr : compiled; object && !(object->orderWord() & ~orderAndCountMask)) {
        Structure* structureBefore = object->structure();
        if (TransitionPlan* plan = transitionPlan(globalObject, object, type, reason); plan && (!plan->structure || object->specialize(vm, *this, *plan))) {
            if (m_lastPlanWentByStructureAlone && asTold == reason && !isFreshObject && !m_counts)
                transitionMadeBefore(structureBefore, type, reason) = { structureBefore, &type, plan, reason };
            countCoercion(value, before, type, reason, "plan"_s, value);
            return value;
        }
        way = "no-plan"_s;
    }
    if (!compiled && !data.isCompiledForAudit && value.isCell() && value.asCell()->type() == FinalObjectType && (!type.admitsForeignObjectsByNature() || isMadeAlsoWhereAdmitted) && (asTold == Format::CastReason::As || asTold == Format::CastReason::CoerceFreshObject || asTold == Format::CastReason::CoerceFreshGraph)) {
        bool mayBecomeIt = !isFreshObject && asTold != Format::CastReason::CoerceFreshGraph;
        if (Object* copy = copyWhatStaysAsItIsByPlan(vm, asObject(value), type, mayBecomeIt)) {
            bool hasBecomeIt = JSValue(copy) == value;
            bool hasRoom = hasBecomeIt || (mayBecomeIt && hasRoomToBecome(asObject(value), copy));
            bool copiesWhatOthersMayHold = asTold == Format::CastReason::As && !isFreshObject && !hasRoom;
            if (!copiesWhatOthersMayHold || !refusesCopiesOfWhatOthersMayHold()) {
                if (copiesWhatOthersMayHold)
                    noteViolation(vm, { .kind = "copy-on-enter"_s, .what = "As"_s, .expected = &type, .got = value, .action = ActionOnViolation::Copied });
                if (hasRoom && !hasBecomeIt)
                    become(vm, asObject(value), copy);
                countCoercion(value, nullptr, type, reason, "plan of a copy"_s, hasRoom ? value : JSValue(copy));
                return hasRoom ? value : JSValue(copy);
            }
        } else if (mayBecomeIt && tryBecomeByPlans(vm, asObject(value), type)) {
            countCoercion(value, nullptr, type, reason, "plans of a graph"_s, value);
            return value;
        }
    }
    Coercion attempt = coercionFor(globalObject, *this, asTold, Coercion::Explains::No);
    if (asTold != Format::CastReason::CoerceFreshGraph)
        attempt.allowObjectsToBecomeTheirCopies();
    if (isFreshObject && value.isObject())
        attempt.noteThatNobodyElseHolds(asObject(value));
    if (handsBack == HandsBack::WhatComesOfIt)
        attempt.allowCopiesOfObjectsOfProgram();
    JSValue result = attempt.run(value, type);
    RETURN_IF_EXCEPTION(scope, { });
    countCoercion(value, before, type, reason, way, result);
    if (result)
        return result;
    if ((reason == Format::CastReason::Flow || ((reason == Format::CastReason::Claim || reason == Format::CastReason::Store) && type.checksForeignObjects())) && !compiled && value.isObject() && !data.isCompiledForAudit) {
        bool hasBecomeIt = becomeWhereItIs(globalObject, asObject(value), type);
        RETURN_IF_EXCEPTION(scope, { });
        if (hasBecomeIt)
            return value;
    }
    // An object that nothing can be made of stays what it is.
    if (reason == Format::CastReason::Coerce && !data.isCompiledForAudit) [[likely]]
        return value;
    // Nothing is left of the attempt: once more, for the reason.
    Coercion coercion = coercionFor(globalObject, *this, asTold, Coercion::Explains::Yes);
    if (asTold != Format::CastReason::CoerceFreshGraph)
        coercion.allowObjectsToBecomeTheirCopies();
    if (isFreshObject && value.isObject())
        coercion.noteThatNobodyElseHolds(asObject(value));
    if (handsBack == HandsBack::WhatComesOfIt)
        coercion.allowCopiesOfObjectsOfProgram();
    {
        SetForScope kindsAlone(s_describesKindsAlone, ViolationLog::isWrittenIfAsked(data.logsTypeErrors));
        result = coercion.run(value, type);
    }
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_ASSERT(!result);
    if (reason == Format::CastReason::Coerce) {
        reportCoercion(site, makeString("'as "_s, describe(type), "' left "_s, describe(value), " unchanged"_s, coercion.failure().isEmpty() ? ""_s : ": "_s, coercion.failure()));
        return value;
    }
    // Behind a cast to `undefined` or `null` alone there is no room for the value, and `undefined` in its stead would be a false trail: as in any other image.
    bool hasRoomForNilAlone = !hasObjects && !(type.primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent));
    if (data.isCompiledForAudit) [[unlikely]] {
        JSValue goesOn = assertionFailed(value, type, static_cast<uint32_t>(reason), site, coercion.failure());
        if (!hasRoomForNilAlone)
            return goesOn;
    }
    bool goesOnAsStranger = type.isOpenedForLog() && value.isObject();
    noteViolation(vm, { .kind = reason == Format::CastReason::Flow ? "narrowing"_s : kindOfUnnamedViolation, .what = describeAssertion(static_cast<uint32_t>(reason)), .expected = &type, .got = value, .reason = &coercion.failure(), .action = goesOnAsStranger ? ActionOnViolation::Continued : ActionOnViolation::Threw });
    if (goesOnAsStranger)
        return value;
    throwTypeError(globalObject, scope, withAOTErrorNote(mismatchMessage(vm, value, type, coercion.failure())));
    return { };
}

bool State::accepts(JSGlobalObject* globalObject, const RuntimeType& type, JSValue value)
{
    if (fits(type, value))
        return true;
    if (!type.admitsOnlyOtherLayouts() || !compiledObject(value))
        return false;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = Coercion(globalObject, *this, Coercion::Mode::Test, Coercion::Completeness::HasWhatIsAdded, Coercion::Explains::No).run(value, type);
    RETURN_IF_EXCEPTION(scope, true);
    return !!result;
}

JSValue State::hold(JSGlobalObject* globalObject, JSValue value, const RuntimeType& type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Coercion attempt(globalObject, *this, Coercion::Mode::Cast, Coercion::Completeness::HasWhatIsAdded, Coercion::Explains::No);
    attempt.allowCopiesOfObjectsOfProgram();
    if (!type.admitsForeignObjectsByNature())
        attempt.allowObjectsToBecomeTheirCopies();
    JSValue result = attempt.run(value, type);
    RETURN_IF_EXCEPTION(scope, { });
    if (result)
        return result;
    Coercion coercion(globalObject, *this);
    coercion.allowCopiesOfObjectsOfProgram();
    if (!type.admitsForeignObjectsByNature())
        coercion.allowObjectsToBecomeTheirCopies();
    {
        SetForScope kindsAlone(s_describesKindsAlone, ViolationLog::isWrittenIfAsked(data.logsTypeErrors));
        result = coercion.run(value, type);
    }
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_ASSERT(!result);
    if (data.isCompiledForAudit) [[unlikely]]
        return assertionFailed(value, type, static_cast<uint32_t>(Format::CastReason::Store), { }, coercion.failure());
    bool goesOnAsStranger = type.isOpenedForLog() && value.isObject();
    noteViolation(vm, { .kind = kindOfUnnamedViolation, .what = "Store"_s, .expected = &type, .got = value, .reason = &coercion.failure(), .action = goesOnAsStranger ? ActionOnViolation::Continued : ActionOnViolation::Threw });
    if (goesOnAsStranger)
        return value;
    throwTypeError(globalObject, scope, withAOTErrorNote(mismatchMessage(vm, value, type, coercion.failure())));
    return { };
}

bool State::becomeWhereItIs(JSGlobalObject* globalObject, JSObject* object, const RuntimeType& type, String* whyNot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!data.isCompiledForAudit && object->type() == FinalObjectType && !type.admitsForeignObjectsByNature() && tryBecomeByPlans(vm, object, type)) {
        countCoercion(object, nullptr, type, Format::CastReason::Claim, "plans of a graph"_s, object);
        return true;
    }
    Object* compiled = compiledObject(object);
    const LayoutData* before = compiled ? &compiled->layout() : nullptr;
    Coercion attempt(globalObject, *this, Coercion::Mode::Cast, Coercion::Completeness::HasWhatIsAdded, Coercion::Explains::No);
    attempt.allowObjectsToBecomeTheirCopies();
    attempt.refuseCopies();
    attempt.allowTransitionsToMembers();
    JSValue result = attempt.run(object, type);
    RETURN_IF_EXCEPTION(scope, false);
    countCoercion(object, before, type, Format::CastReason::Claim, compiled ? "held"_s : "foreign"_s, result);
    RELEASE_ASSERT(!result || result == JSValue(object));
    if (!result && whyNot) {
        Coercion explaining(globalObject, *this, Coercion::Mode::Cast, Coercion::Completeness::HasWhatIsAdded, Coercion::Explains::Yes);
        explaining.allowObjectsToBecomeTheirCopies();
        explaining.refuseCopies();
        explaining.allowTransitionsToMembers();
        explaining.run(object, type);
        RETURN_IF_EXCEPTION(scope, false);
        *whyNot = explaining.failure();
    }
    return !!result;
}

// `value as T` where the value is no object, or T has none: see Format::CastReason::Coerce.
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
    bool hasSeveralKinds = hasNumber + hasString + hasBoolean + hasBigInt + !!(type.primitives & Primitive::Symbol) > 1;
    JSValue result = value;
    auto noteRefusal = [&] {
        if (scope.exception() && vm.isTerminationException(scope.exception()))
            return;
        noteViolation(vm, { .kind = "as-refuse"_s, .what = "Coerce"_s, .expected = &type, .got = value });
    };
    if (hasSeveralKinds) {
        result = value.toPrimitive(globalObject);
        if (scope.exception()) [[unlikely]] {
            noteRefusal();
            return { };
        }
        hasBigInt = false;
    }
    if (!fits(type, result)) {
        // What the language has no conversion for, and throws.
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
            noteRefusal();
            return { };
        }
    }
    // There is no conversion to a symbol or to an object, and a type may have room for some numbers only, or for one boolean.
    if (!fits(type, result)) {
        if (data.isCompiledForAudit) [[unlikely]]
            return assertionFailed(value, type, static_cast<uint32_t>(Format::CastReason::Coerce), site);
        noteRefusal();
        throwTypeError(globalObject, scope, withAOTErrorNote(makeString("Cannot convert "_s, describe(value), " to "_s, describe(type))));
        return { };
    }
    if (data.isCompiledForAudit) [[unlikely]]
        reportCoercion(site, makeString("'as "_s, describe(type), "' converted "_s, describe(value), " to "_s, describe(result)));
    return result;
}

static uint64_t keyOfPlace(uint32_t what, AuditSite site)
{
    return static_cast<uint64_t>(what & 0x7f) << 56 | static_cast<uint64_t>(site.function & 0xffffff) << 32 | site.lineAndColumn;
}

void State::reportCoercion(AuditSite site, const String& what)
{
    if (m_reportedPlaces.add(keyOfPlace(static_cast<uint32_t>(Format::CastReason::Coerce), site)).isNewEntry)
        dataLogLn("AOT: Coerce at ", describeSite(site), ": ", what, noteOnIgnoredRules());
}

void State::reportLackOfRoom(const LayoutData& layout, size_t cellSize, const LayoutData& wanted)
{
    if (data.isCompiledForAudit && m_reportedPlaces.add(3ULL << 62 | static_cast<uint64_t>(layout.shape) << 32 | wanted.shape).isNewEntry)
        dataLogLn(makeString("AOT: cannot convert "_s, nameOf(layout), " to "_s, nameOf(wanted), " in place: the object has "_s, cellSize, " bytes, "_s, nameOf(wanted), " needs "_s, wanted.size, noteOnIgnoredRules()));
}

bool State::reportOrThrow(JSGlobalObject* globalObject, const Violation& violation, const String& message, InAudit inAudit, ASCIILiteral departure)
{
    VM& vm = globalObject->vm();
    if (data.isCompiledForAudit) {
        // Most come here by a function of the engine, Object.freeze() say. What matters is who called that.
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
            dataLogLn("AOT: Rule at ", place, ": ", message, inAudit == InAudit::Ignored ? "; ignored" : "; carried out", departure.isNull() ? "" : "; ", departure.isNull() ? ""_s : departure, noteOnIgnoredRules());
        m_hasIgnoredRule |= inAudit == InAudit::Ignored;
        return true;
    }
    auto scope = DECLARE_THROW_SCOPE(vm);
    noteViolation(vm, violation);
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

// Of a new object. Where there are too many to keep their order (keysAreInLayoutOrder) a fresh allocation does not say what it is born with.
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

// How the key came: compiled code boxes a number it keeps as a double as one, whatever it is.
static ASCIILiteral describeKeyOfElement(JSValue key)
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
    String reason = makeString("key "_s, describeKeyOfElement(key), " shape "_s, static_cast<unsigned>(array->indexingType() & IndexingShapeMask), place, array->type() != ArrayType ? " changed"_s : ""_s);
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
        m_counts->typesOfCoercions.add(key, describe(type));
    ++added.iterator->value;
    if (Object* made = !before && result && value.isCell() ? compiledObject(result) : nullptr)
        ++m_counts->coercions.add(makeString("room: cell "_s, value.asCell()->cellSize(), " wanted "_s, sizeWithRoomToSpecialize(made->shape()), result == value ? " became"_s : " copied"_s, made->extraProperties() ? " superset"_s : " exact"_s, " at type "_s, static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(&type) - reinterpret_cast<const uint8_t*>(&data))), 0).iterator->value;
}

void State::countArrayPut(JSArray* array, JSValue key, IndexingType shapeBefore, unsigned vectorLengthBefore)
{
    if (!m_counts) [[likely]]
        return;
    if (shapeBefore < ArrayStorageShape && hasAnyArrayStorage(array->indexingType())) {
        ArrayStorage* storage = array->butterfly()->arrayStorage();
        // With the one that has just been stored.
        size_t elements = storage->m_numValuesInVector + (storage->m_sparseMap ? storage->m_sparseMap->size() : 0);
        String record = makeString("shape "_s, static_cast<unsigned>(shapeBefore), " index "_s, key.isNumber() ? String::number(key.asNumber()) : "?"_str, " vector "_s, vectorLengthBefore, " elements "_s, elements ? elements - 1 : 0);
        ++m_counts->arraysLeft.add(record, 0).iterator->value;
    }
    String reason = makeString("key "_s, describeKeyOfElement(key), " shape "_s, static_cast<unsigned>(shapeBefore), " to "_s, static_cast<unsigned>(array->indexingType() & IndexingShapeMask), array->type() != ArrayType ? " changed"_s : ""_s);
    ++m_counts->arrayPuts.add(reason, 0).iterator->value;
}

void State::countClosure(uint32_t function, unsigned numberOfCaptures)
{
    if (!m_counts) [[likely]]
        return;
    // From 1: no key is all zeros.
    ++m_counts->closures.add({ function + 1, numberOfCaptures }, 0).iterator->value;
}

void State::countPlainAllocation(JSCell* cell, uint32_t function, uint32_t kindAndValue)
{
    if (!m_counts) [[likely]]
        return;
    Structure* structure = cell->structure();
    // That is a cell of an allocation, which is counted as one.
    if (structure->compiledLayout() || !cell->isObject() || isSlimClosureType(cell->type()))
        return;
    size_t storage = 0;
    if (Butterfly* butterfly = asObject(cell)->butterfly(); butterfly && !isCopyOnWrite(structure->indexingMode())) {
        bool hasIndexingHeader = structure->hasIndexingHeader(cell);
        storage = Butterfly::totalSize(0, structure->outOfLineCapacity(), hasIndexingHeader, hasIndexingHeader ? butterfly->indexingHeader()->indexingPayloadSizeInBytes(structure) : 0);
    }
    // From 1: no key is all zeros.
    std::pair<uint32_t, uint32_t> key { function + 1, kindAndValue };
    ++m_counts->plainAllocations.add(key, 0).iterator->value;
    m_counts->bytesOfPlainCells.add(key, 0).iterator->value += cell->cellSize();
    m_counts->bytesOfPlainStorage.add(key, 0).iterator->value += storage;
}

void State::countExtraPropertyTable(const LayoutData& layout)
{
    if (!m_counts) [[likely]]
        return;
    ++m_counts->extraPropertyTables[layout.shape];
}

void State::countExtraProperty(const LayoutData& layout, PropertyName name)
{
    if (!m_counts) [[likely]]
        return;
    // All indexes count as one name, and a symbol goes by its description.
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
    // By cell size: auxiliary cells, and those of them whose owner has been seen.
    UncheckedKeyHashMap<size_t, uint64_t> auxiliaryCells;
    UncheckedKeyHashMap<size_t, uint64_t> ownedAuxiliaryCells;
    auto count = [&](LiveKind kind, const void* what, size_t cellSize) {
        ++live.cells.add({ static_cast<uint32_t>(kind), std::bit_cast<uintptr_t>(what), cellSize }, 0).iterator->value;
    };
    auto countStorage = [&](LiveKind kind, const void* what, JSObject* object) {
        Structure* structure = object->structure();
        Butterfly* butterfly = object->butterfly();
        // What arrays share until one of them is written to is a cell with a class.
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
            live.bytesOfStrings += sizeof(StringImpl);
        } else if (impl->isSubString()) {
            ++live.substrings;
            live.bytesOfStrings += sizeof(StringImpl);
        } else
            live.bytesOfStrings += impl->sizeInBytes();
    };
    auto sizesOfArray = [](JSArray* array) -> std::pair<uint64_t, uint64_t> {
        Butterfly* butterfly = array->butterfly();
        return { array->length(), butterfly ? butterfly->vectorLength() : 0 };
    };
    UncheckedKeyHashMap<JSCell*, uint32_t> holdersOfArrays;
    auto countSlots = [&](const LayoutData& layout, Object* object, size_t cellSize) {
        auto& slots = live.slots.add(&layout, Counts::LiveCells::Slots { }).iterator->value;
        if (isObjectLayout(layout.kind))
            ++live.orderStates.add({ layout.shape, object->orderWord() }, 0).iterator->value;
        ++slots.objects;
        slots.bytes += cellSize;
        if (layout.referencesOffset) {
            const uint32_t* references = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(&layout) + layout.referencesOffset);
            const uint32_t* offset = references + 3;
            auto holdsCell = [&](uint32_t offset) {
                JSValue value = JSValue::decode(object->at<EncodedJSValue>(offset));
                return value && value.isCell();
            };
            for (uint32_t i = 0; i < references[0]; ++i)
                slots.filledBoxed += holdsCell(*offset++);
            for (uint32_t i = 0; i < references[1]; ++i)
                slots.filledWide += holdsCell(*offset++);
            for (uint32_t i = 0; i < references[2]; ++i)
                slots.filledNarrow += object->at<uint32_t>(*offset++) > undefinedCell32;
        }
        auto& fields = live.fields.add(&layout, Vector<Counts::LiveCells::Field>()).iterator->value;
        if (fields.isEmpty())
            fields.grow(layout.numberOfFields);
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            const FieldData& field = layout.fields()[i];
            if (!holdsCells(field.store()))
                continue;
            const void* slot = object->slotOf(field);
            JSValue value = slot ? readAs(field.store(), slot) : JSValue();
            if (!value || !value.isCell())
                continue;
            auto& counts = fields[i];
            ++counts.filled;
            JSCell* held = value.asCell();
            const ClassInfo* classInfo = held->classInfo();
            if (classInfo == JSArray::info()) {
                auto [length, capacity] = sizesOfArray(uncheckedDowncast<JSArray>(held));
                ++counts.arrays;
                counts.emptyArrays += !length;
                counts.elements += length;
                counts.capacity += capacity;
                ++holdersOfArrays.add(held, 0).iterator->value;
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
        // Allocated, and the collection came before anything was written to it.
        if (heapCell->isZapped()) {
            count(LiveKind::Class, nullptr, cellSize);
            return;
        }
        JSCell* cell = static_cast<JSCell*>(heapCell);
        const ClassInfo* classInfo = cell->classInfo();
        // An object of the program has no butterfly: the word holds its order and shape.
        if (const LayoutData* layout = cell->structure()->compiledLayoutOf(cell)) {
            count(LiveKind::Layout, layout, cellSize);
            countSlots(*layout, uncheckedDowncast<Object>(cell), cellSize);
            if (Extension* extension = layout->hasExtension ? uncheckedDowncast<Object>(cell)->extension() : nullptr) {
                count(LiveKind::Extension, nullptr, extension->size());
                ++ownedAuxiliaryCells.add(extension->size(), 0).iterator->value;
            }
        } else if (extraPropertyTables.contains(cell)) {
            count(LiveKind::ExtraProperties, nullptr, cellSize);
            countStorage(LiveKind::StorageOfExtraProperties, nullptr, asObject(cell));
        } else {
            bool isClosure = classInfo == Closure::info() || classInfo == SlimClosure::info();
            count(isClosure ? LiveKind::Closure : LiveKind::Class, isClosure ? Closure::info() : classInfo, cellSize);
            if (classInfo == JSArray::info()) {
                auto [length, capacity] = sizesOfArray(uncheckedDowncast<JSArray>(cell));
                ++live.arrays[Counts::LiveCells::bucketOf(length)][Counts::LiveCells::bucketOf(capacity)];
            } else if (classInfo == JSMap::info())
                ++live.maps[Counts::LiveCells::bucketOf(uncheckedDowncast<JSMap>(cell)->size())];
            else if (classInfo == JSSet::info())
                ++live.sets[Counts::LiveCells::bucketOf(uncheckedDowncast<JSSet>(cell)->size())];
            if (classInfo == SlimClosure::info()) { }
            else if (cell->isObject())
                countStorage(LiveKind::StorageOfClass, classInfo, asObject(cell));
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
    for (auto& entry : holdersOfArrays)
        ++(entry.value == 1 ? live.arraysHeldOnce : live.arraysHeldByMore);
    m_counts->liveCells = WTF::move(live);
}

void State::printCounts() const
{
    auto nameOrDash = [&](uint32_t string) {
        String name = text(string);
        return name.isEmpty() ? "-"_str : name;
    };
    // Of a name that the running program has made up: a record is one line of words.
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
    // By guard: how many fields of the layouts of the program are pinned under it, and whether it is set.
    std::array<uint32_t, numberOfGuardsOfPinnedFields> pinnedFields { };
    for (uint32_t shape = 1; shape < data.numberOfLayouts; ++shape) {
        if (!hasLayoutOfShape(shape))
            continue;
        const LayoutData& layout = layoutOfShape(shape);
        for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
            if (layout.fields()[i].flags & fieldIsPinned)
                ++pinnedFields[layout.fields()[i].flags >> guardOfPinnedFieldShift];
        }
    }
    for (uint32_t guard = 0; guard < numberOfGuardsOfPinnedFields; ++guard) {
        if (pinnedFields[guard])
            dataLogLn("AOT count: guard ", guard, " pinned fields ", pinnedFields[guard], " set ", instance.typedAlteredLiterals[guard]);
    }
    for (uint32_t number = 0; number < m_counts->allocations.size(); ++number) {
        auto& counts = m_counts->allocations[number];
        if (!counts.objects)
            continue;
        const LayoutData& layout = layoutOfShape(allocation(number).shape);
        dataLogLn("AOT count: allocation ", number, " layout ", layout.index, " ", nameOrDash(layout.name), " size ", allocation(number).size, " cell ", counts.cellSize, " objects ", counts.objects);
    }
    for (auto [shape, cellSize, objects] : sorted(m_counts->objectsWithoutAllocation)) {
        const LayoutData& layout = layoutOfShape(shape);
        dataLogLn("AOT count: layout ", layout.index, " ", nameOrDash(layout.name), " cell ", cellSize, " objects ", objects);
    }
    for (auto [shape, word, times] : sorted(m_counts->orderStates)) {
        const LayoutData& layout = layoutOfShape(shape);
        dataLogLn("AOT count: order-state ", layout.index, " ", nameOrDash(layout.name), " order ", orderOf(word), " count ", countOf(word), " times ", times);
    }
    for (auto [fromShape, toShape, objects] : sorted(m_counts->transitions)) {
        const LayoutData& from = layoutOfShape(fromShape);
        const LayoutData& to = layoutOfShape(toShape);
        dataLogLn("AOT count: transition ", from.index, " ", nameOrDash(from.name), " ", to.index, " ", nameOrDash(to.name), " objects ", objects);
    }
    for (uint32_t shape = 0; shape < m_counts->fieldCreations.size(); ++shape) {
        auto& counts = m_counts->fieldCreations[shape];
        for (uint32_t i = 0; i < counts.size(); ++i) {
            if (!counts[i])
                continue;
            const LayoutData& layout = layoutOfShape(shape);
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
        dataLogLn("AOT count: coerce ", cast, " calls ", calls, " ", m_counts->typesOfCoercions.get(cast));
    Vector<std::pair<String, uint64_t>> arrayPuts;
    for (auto& entry : m_counts->arrayPuts)
        arrayPuts.append({ entry.key, entry.value });
    std::ranges::sort(arrayPuts, [](auto& a, auto& b) {
        return codePointCompareLessThan(a.first, b.first);
    });
    for (auto& [reason, stores] : arrayPuts)
        dataLogLn("AOT count: array-put ", reason, " stores ", stores);
    Vector<std::pair<String, uint64_t>> arraysLeft;
    for (auto& entry : m_counts->arraysLeft)
        arraysLeft.append({ entry.key, entry.value });
    std::ranges::sort(arraysLeft, [](auto& a, auto& b) {
        return codePointCompareLessThan(a.first, b.first);
    });
    for (auto& [record, arrays] : arraysLeft)
        dataLogLn("AOT count: array-left ", record, " arrays ", arrays);
    for (auto [function, numberOfCaptures, objects] : sorted(m_counts->closures)) {
        String name = nameOfFunction(function - 1);
        size_t cell = MarkedSpace::optimalSizeFor(makesSlimClosures(function - 1) ? SlimClosure::allocationSize(numberOfCaptures) : Closure::allocationSize(numberOfCaptures));
        dataLogLn("AOT count: closure f", function - 1, " ", name.isEmpty() ? "-"_str : name, " words ", numberOfCaptures, " cell ", cell, " objects ", objects);
    }
    for (auto [function, kindAndValue, objects] : sorted(m_counts->plainAllocations)) {
        static constexpr ASCIILiteral kinds[] = { "object"_s, "this"_s, "array"_s, "clone"_s };
        std::pair<uint32_t, uint32_t> key { function, kindAndValue };
        dataLogLn("AOT count: plain ", kinds[kindAndValue >> plainAllocationKindShift], " f", function - 1, " v", kindAndValue & ((1u << plainAllocationKindShift) - 1), " cell-bytes ", m_counts->bytesOfPlainCells.get(key), " storage-bytes ", m_counts->bytesOfPlainStorage.get(key), " objects ", objects);
    }
    for (uint32_t shape = 0; shape < m_counts->extraPropertyTables.size(); ++shape) {
        if (uint64_t objects = m_counts->extraPropertyTables[shape]) {
            const LayoutData& layout = layoutOfShape(shape);
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
            const LayoutData& layout = layoutOfShape(shape);
            dataLogLn("AOT count: extra ", layout.index, " ", nameOrDash(layout.name), " ", asWord(name), " created ", created);
        }
    }
    // The most frequent first.
    Vector<std::tuple<uint64_t, uintptr_t, uint32_t, uintptr_t, UniquedStringImpl*>> accesses;
    for (auto& entry : m_counts->accesses)
        accesses.append({ ~entry.value.count, std::get<0>(entry.key), std::get<1>(entry.key), std::get<2>(entry.key), entry.value.name.get() });
    std::ranges::sort(accesses);
    for (auto [invertedCount, address, kindAndPlace, what, name] : accesses) {
        static constexpr ASCIILiteral kinds[] = { "read"_s, "write"_s, "read-extra"_s, "write-extra"_s };
        static constexpr ASCIILiteral places[] = { "field"_s, "absent-field"_s, "extra"_s, "not-own"_s, "other-receiver"_s };
        auto place = static_cast<Counts::Place>(kindAndPlace & 0xff);
        auto location = FunctionRef::at(&instance, std::bit_cast<const void*>(address)).locationForReturnAddress(std::bit_cast<const void*>(address), nullptr);
        String function = location.typedFunction != FunctionRef::none ? typedFunctionName(location.typedFunction) : String();
        String where = "-"_s;
        if (auto position = location.function.reportedPositionFor(location.bytecodeIndex); position && position->source)
            where = makeString(ProgramData::get()->sourceName(position->source), ':', position->lineColumn.line, ':', position->lineColumn.column);
        String receiver = "- primitive"_s;
        if (place != Counts::Place::OtherReceiver) {
            const LayoutData& layout = layoutOfShape(static_cast<uint32_t>(what));
            receiver = makeString(layout.index, ' ', nameOrDash(layout.name));
        } else if (what)
            receiver = makeString("- "_s, std::bit_cast<const ClassInfo*>(what)->className);
        dataLogLn("AOT count: access ", kinds[kindAndPlace >> 8], " ", function.isEmpty() ? "-"_str : function, " ", where, " ", asWord(name), " ", receiver, " ", places[static_cast<unsigned>(place)], " ", ~invertedCount);
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
    // By LiveKind, the index of the layout, what the line starts with, cell size.
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
        case Counts::LiveKind::StorageOfExtraProperties:
            start = "extras-storage"_s;
            break;
        case Counts::LiveKind::Class:
            start = makeString("class "_s, className());
            break;
        case Counts::LiveKind::StorageOfClass:
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
        const LayoutData& layout = layoutOfShape(shape);
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
    dataLogLn("AOT live: arrays-held once ", live.arraysHeldOnce, " more ", live.arraysHeldByMore);
    for (unsigned size = 0; size < Counts::LiveCells::numberOfBuckets; ++size) {
        if (live.maps[size])
            dataLogLn("AOT live: map size ", Counts::LiveCells::boundOf(size), " objects ", live.maps[size]);
    }
    for (unsigned size = 0; size < Counts::LiveCells::numberOfBuckets; ++size) {
        if (live.sets[size])
            dataLogLn("AOT live: set size ", Counts::LiveCells::boundOf(size), " objects ", live.sets[size]);
    }
    dataLogLn("AOT live: extras-entries ", live.extraPropertyTables);
    dataLogLn("AOT live: strings ", live.strings, " bytes ", live.bytesOfStrings, " ropes ", live.ropes, " static ", live.staticStrings, " external ", live.externalStrings, " substrings ", live.substrings);
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

// As Reader::isReliedOn() has it, but for `never`, which nothing can be made of.
static bool isReliedOnInAudit(const RuntimeType& type)
{
    if (type.allowsAnyObject() || type.allowsAnyFunction() || type.admitsForeignObjects() || type.numberOfViews() || type.numberOfJSTypes() || !type.primitives)
        return false;
    return !(type.primitives & ~Primitive::Boolean) || !(type.primitives & ~(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent));
}

JSValue State::nearestOf(const RuntimeType& type, JSValue value) const
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
    // Adding zero makes zero of negative zero.
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
    // Of the operations that only the passes make, one is held to a type: what has taken the place of a read or a call.
    uint32_t operation = what & ~assertionIsOnResult;
    return operation < std::size(operations) ? operations[operation] : "CallBuiltin"_s;
}

JSValue State::assertionFailed(JSValue value, const RuntimeType& type, uint32_t what, AuditSite site, const String& failure, JSValue storedTo)
{
    // What has room for `undefined` and `null` alone: see State::coerce().
    bool isConverted = isReliedOnInAudit(type) && type.primitives & (Primitive::Number | Primitive::Boolean);
    JSValue goesOn = isReliedOnInAudit(type) ? nearestOf(type, value) : value;
    RELEASE_ASSERT(!isReliedOnInAudit(type) || fits(type, goesOn));
    // The store that follows throws whatever the value is, as it does in JavaScript. That is no contradiction of a type.
    if (Object* object = storedTo ? compiledObject(storedTo) : nullptr; object && object->isFrozen())
        return goesOn;
    if (!m_reportedPlaces.add(keyOfPlace(what & assertionIsOnResult ? 0x40 | what : what, site)).isNewEntry)
        return goesOn;
    bool isNonNull = what == static_cast<uint32_t>(Format::CastReason::NonNull) && value.isUndefinedOrNull();
    String message = isNonNull ? nonNullMessage(value) : makeString("expected "_s, describe(type), ", got "_s, describe(value));
    String reason = failure.isEmpty() ? explainMismatch(*instance.vm, value, type) : failure;
    dataLogLn(makeString("AOT: "_s, describeAssertion(what), " at "_s, describeSite(site), ": "_s, message, reason.isEmpty() ? ""_s : "; "_s, reason, isConverted ? "; converted to "_s : ""_s, isConverted ? describe(goesOn) : emptyString(), noteOnIgnoredRules()));
    return goesOn;
}

JSValue State::holdToTypeOf(JSGlobalObject* globalObject, const FieldData& field, JSValue value, String& failure)
{
    const RuntimeType& type = ownTypeOf(field);
    // Every store comes this way then, also that of typed code through a view. The field takes what its views take: see Reader. What it
    // does not take is reported and goes on, as it is if the slot has room for it.
    if (data.isCompiledForAudit && isReliedOnInAudit(type) && !fits(type, value)) [[unlikely]] {
        const RuntimeType& slot = typeOf(field);
        JSValue held = fits(slot, value) ? value : nearestOf(slot, value);
        if (m_reportedPlaces.add(1ULL << 63 | std::bit_cast<uintptr_t>(&field)).isNewEntry)
            dataLogLn("AOT: Store to '", text(field.name), "': expected ", describe(type), ", got ", describe(value), held == value ? "" : "; converted to ", held == value ? String() : describe(held), noteOnIgnoredRules());
        return held;
    }
    bool slotTakesMore = field.type() != field.ownType();
    if (!value.isObject())
        return fits(type, value) || (slotTakesMore && fits(typeOf(field), value)) ? value : JSValue();
    // Nothing is made of what enters a place whose type nobody wrote down.
    bool onlyTests = !(field.flags & fieldIsAnnotated);
    if (onlyTests && fitsAsOneOfItsOwn(type, asObject(value)))
        return value;
    if (Object* record = slotTakesMore ? compiledObject(value) : nullptr; record && !admitsShape(type, record->shape()) && admitsShape(typeOf(field), record->shape()))
        return value;
    bool mayBecomeIt = !onlyTests && storesMakeOverPlainData;
    if (mayBecomeIt && !type.admitsForeignObjectsByNature() && !data.isCompiledForAudit && value.asCell()->type() == FinalObjectType) {
        VM& vm = globalObject->vm();
        if (Object* copy = copyWhatStaysAsItIsByPlan(vm, asObject(value), type, true)) {
            bool hasBecomeIt = JSValue(copy) == value;
            bool hasRoom = hasBecomeIt || hasRoomToBecome(asObject(value), copy);
            if (hasRoom || !refusesCopiesOfWhatOthersMayHold()) {
                if (!hasRoom)
                    noteViolation(vm, { .kind = "copy-on-enter"_s, .what = "As"_s, .expected = &type, .got = value, .action = ActionOnViolation::Copied });
                if (hasRoom && !hasBecomeIt)
                    become(vm, asObject(value), copy);
                countCoercion(value, nullptr, type, Format::CastReason::Store, "plan of a copy"_s, hasRoom ? value : JSValue(copy));
                return hasRoom ? value : JSValue(copy);
            }
        } else if (tryBecomeByPlans(vm, asObject(value), type)) {
            countCoercion(value, nullptr, type, Format::CastReason::Store, "plans of a graph"_s, value);
            return value;
        }
    }
    Object* compiled = compiledObject(value);
    const LayoutData* before = compiled ? &compiled->layout() : nullptr;
    Coercion coercion(globalObject, *this, onlyTests ? Coercion::Mode::Test : Coercion::Mode::Cast);
    coercion.allowCopiesOfObjectsOfProgram();
    if (mayBecomeIt)
        coercion.allowObjectsToBecomeTheirCopies();
    JSValue result = coercion.run(value, type);
    countCoercion(value, before, type, Format::CastReason::Store, compiled ? "stored"_s : "foreign"_s, result);
    if (!result && onlyTests && !compiled && !data.isCompiledForAudit) {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        bool hasBecomeIt = becomeWhereItIs(globalObject, asObject(value), type);
        RETURN_IF_EXCEPTION(scope, { });
        if (hasBecomeIt)
            return value;
    }
    if (!result && type.isOpenedForLog()) [[unlikely]] {
        noteViolation(globalObject->vm(), { .kind = kindOfUnnamedViolation, .what = "Store"_s, .expected = &type, .got = value, .reason = &coercion.failure(), .action = ActionOnViolation::Continued });
        return value;
    }
    if (!result)
        failure = coercion.failure();
    return result;
}

JSCell* State::closureIfMade(uint32_t function) const
{
    if (data.usesWideReferences)
        return static_cast<JSCell**>(instance.typedClosures)[function];
    uint32_t row = static_cast<uint32_t*>(instance.typedClosures)[function];
    return row ? MarkedBlockCage::decode(row + 1) : nullptr;
}

void State::didCreateClosureFor(uint32_t function, JSCell* cell)
{
    if (!(kindAndDeclarationOf(function) & functionIsOfBuiltinModule))
        ensureStaticsOfProgramAreFilled();
    if (data.usesWideReferences) {
        static_cast<JSCell**>(instance.typedClosures)[function] = cell;
        didCreate(cell);
        return;
    }
    Locker locker { m_cellsLock };
    static_cast<uint32_t*>(instance.typedClosures)[function] = MarkedBlockCage::encode(cell) - 1;
    m_closuresMadeLately.cells.append(MarkedBlockCage::encode(cell));
    m_stretchesWithClosures.set(function >> closuresInStretchShift);
}

void State::makeTheOneThatConstructs(Closure* closure)
{
    uint32_t function = closure->functionIndex();
    if (closureIfMade(function) || closuresHaveCaptures(function))
        return;
    std::span allocationsByConstructor { data.at<std::array<uint32_t, 2>>(data.allocationsByConstructorOffset), data.numberOfAllocationsByConstructor };
    auto found = std::ranges::lower_bound(allocationsByConstructor, std::array<uint32_t, 2> { function, 0 });
    if (found == allocationsByConstructor.end() || (*found)[0] != function)
        return;
    didCreateClosureFor(function, closure);
}

JSObject* State::closureObjectFor(VM& vm, uint32_t function)
{
    if (!makesSlimClosures(function))
        return closure(vm, function);
    JSCell* cell = closureIfMade(function);
    if (!cell) {
        cell = SlimClosure::create(vm, slimClosureStructure(vm, function), function);
        didCreateClosureFor(function, cell);
    }
    return asObject(cell);
}

Closure* State::closure(VM& vm, uint32_t function)
{
    JSCell* cell = closureIfMade(function);
    if (!cell) {
        const LayoutData* ofClass = classConstructedBy(function);
        bool isMadeWhenChanged = ofClass && makesClassObjectWhenChanged(*ofClass) && hasPrototypeAheadOfClassObject(*ofClass);
        // It may make the class objects of what the class extends.
        Structure* structure = isMadeWhenChanged ? structureOfUnmadeClassObject(vm, *ofClass) : closureStructure(vm, function);
        RELEASE_ASSERT(!closureIfMade(function) && !makesSlimClosures(function));
        cell = Closure::create(vm, structure, function);
        didCreateClosureFor(function, cell);
        if (isMadeWhenChanged && !mayLeaveClassObjectUnmade(structure->storedPrototypeObject(), *ofClass))
            uncheckedDowncast<Closure>(cell)->ensureClassObject(vm);
        // The class object of a class that extends something foreign is made at the declaration, where that is evaluated.
        if (ofClass && !ofClass->hasForeignRoot && !isMadeWhenChanged) {
            RELEASE_ASSERT(ofClass->isEvaluatedOnce);
            makeClass(instance.globalObject, uncheckedDowncast<Closure>(cell), JSValue());
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
bool State::MadeLately::visit(VM& vm, Visitor& visitor, const Functor& handOver)
{
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        HeapVersion version = vm.heap.objectSpace().newlyAllocatedVersion();
        bool isFirstVisitOfCollection = version != versionOfLastVisit;
        if (isFirstVisitOfCollection) {
            versionOfLastVisit = version;
            earlier.append(cells.span().first(numberVisited));
            if (numberVisited == cells.size())
                cells.clear();
            else
                cells.removeAt(0, numberVisited);
        }
        size_t numberHandedOverInThisCollection = isFirstVisitOfCollection ? 0 : numberVisited;
        numberVisited = cells.size();
        // A full collection is handed everything at its first visit, and what has been made since is among these.
        if (vm.heap.collectionScope() != CollectionScope::Eden && isFirstVisitOfCollection) {
            for (uint32_t cell : earlier)
                handOver(visitor, MarkedBlockCage::decode(cell));
        }
        for (uint32_t cell : cells.span().subspan(numberHandedOverInThisCollection))
            handOver(visitor, MarkedBlockCage::decode(cell));
        return true;
    }
    return false;
}

template<typename Visitor>
void State::visitClosures(Visitor& visitor)
{
    Locker locker { m_cellsLock };
    if (data.usesWideReferences || m_closuresMadeLately.visit(*instance.vm, visitor, [](auto& visitor, JSCell* cell) {
        visitor.appendUnbarriered(cell);
    }))
        return;
    std::span rows { static_cast<const uint32_t*>(instance.typedClosures), data.numberOfFunctions };
    for (size_t stretch : m_stretchesWithClosures) {
        size_t first = stretch << closuresInStretchShift;
        for (uint32_t row : rows.subspan(first, std::min<size_t>(1 << closuresInStretchShift, rows.size() - first))) {
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
    bool wereHandedOver = m_stringsMadeLately.visit(*instance.vm, visitor, [](auto& visitor, JSCell* cell) {
        StringImpl* impl = uncheckedDowncast<JSString>(cell)->tryGetValueImpl();
        if (!impl) [[unlikely]] {
            visitor.appendUnbarriered(cell);
            return;
        }
        if (visitor.markLeafCell(cell))
            visitor.reportExtraMemoryVisitedOfLeafCell(impl->costDuringGC());
    });
    wereHandedOver &= m_stringsBackedByStaticDataMadeLately.visit(*instance.vm, visitor, [](auto& visitor, JSCell* cell) {
        visitor.markLeafCell(cell);
    });
    if (wereHandedOver)
        return;
    for (uint64_t slot : std::span { instance.typedStrings, capacityOfStringTable(instance.typedStrings) }.subspan(1)) {
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
        for (uint32_t card = m_sizeOfStaticsWithoutAccess >> Instance::typedCardShift; card < data.numberOfCards; ++card)
            visitCard(card);
        return;
    } else {
        JSC::Heap& heap = instance.vm->heap;
        bool all = false;
        if (heap.collectionScope() != CollectionScope::Eden && m_versionOfLastVisitOfAllCards != heap.objectSpace().markingVersion()) {
            m_versionOfLastVisitOfAllCards = heap.objectSpace().markingVersion();
            all = true;
        }
        bool worldIsStopped = heap.worldIsStopped();
        for (uint32_t first = 0; first < data.numberOfCards; first += sizeof(uint64_t)) {
            if (!*reinterpret_cast<const uint64_t*>(instance.typedCards + first))
                continue;
            for (uint32_t card = first; card < first + sizeof(uint64_t); ++card) {
                uint8_t state = instance.typedCards[card];
                if (state != Instance::typedCardIsStoredTo && !(all && state))
                    continue;
                if (worldIsStopped)
                    instance.typedCards[card] = Instance::typedCardWasLookedAt;
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
        // What an earlier collection has marked stays marked in a collection of the young.
        if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
            if (HeapVersion version = instance.vm->heap.objectSpace().newlyAllocatedVersion(); version != m_versionOfLastVisit) {
                m_versionOfLastVisit = version;
                m_numberOfCellsMarkedBefore = m_numberOfCellsVisited;
                if (instance.vm->heap.collectionScope() == CollectionScope::Eden)
                    first = m_numberOfCellsMarkedBefore;
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
    for (JSCell* cell : std::initializer_list<JSCell*> { moduleRecord, importMeta, m_closureStructures[0], m_closureStructures[1], m_closureStructures[2], m_closureStructures[3], m_closureStructures[4], m_liveBindingGetter, m_liveBindingAccessor }) {
        if (cell)
            visitor.appendUnbarriered(cell);
    }
    visitor.appendUnbarriered(newTarget);
    for (auto& [entryPoint, import] : importsOfEntryPoints) {
        visitor.appendUnbarriered(entryPoint);
        visitor.appendUnbarriered(import);
    }
    const uint32_t* roots = data.at<uint32_t>(data.rootsOffset);
    uint32_t sizeWithoutAccess = m_sizeOfStaticsWithoutAccess;
    WTF::loadLoadFence();
    for (uint32_t i = 0; i < data.numberOfRoots; ++i) {
        if (roots[i] >= sizeWithoutAccess)
            visitor.appendUnbarriered(JSValue::decode(*reinterpret_cast<EncodedJSValue*>(instance.typedStatics + roots[i])));
    }
    visitRootsWithCards(visitor);
}

template void State::visit(AbstractSlotVisitor&);
template void State::visit(SlotVisitor&);

// Stack frames. See AOTFunction.h.

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

static uint32_t ownerOfFunction(const Data& data, uint32_t function)
{
    const uint32_t* runs = data.at<uint32_t>(data.functionAliasesOffset);
    const uint16_t* lowHalves = reinterpret_cast<const uint16_t*>(runs + 2 * data.numberOfRunsOfFunctionAliases);
    const uint8_t* highBytes = reinterpret_cast<const uint8_t*>(lowHalves + data.numberOfFunctionAliases);
    uint32_t begin = 0;
    uint32_t end = data.numberOfRunsOfFunctionAliases;
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
    uint32_t behindRun = begin < data.numberOfRunsOfFunctionAliases ? runs[2 * begin + 1] : data.numberOfFunctionAliases;
    return index < behindRun ? static_cast<uint32_t>(highBytes[index]) << 16 | lowHalves[index] : function;
}

static JSString* textOfFunction(JSGlobalObject* globalObject, uint32_t function)
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
    if (!s_textOfFunctionInFile)
        return nullptr;
    const uint32_t* path = data.at<uint32_t>(texts.pathsOffset) + 2 * (*entry)[1];
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    String text = s_textOfFunctionInFile(globalObject, String::fromUTF8(std::span { data.at<char8_t>(path[0]), path[1] }), (*entry)[2]);
    scope.assertNoException();
    return text.isNull() ? nullptr : jsString(vm, WTF::move(text));
}

// By the function that owns it: see Closure::defineOwnProperty(). For every thread: a frame is reported without an Instance.
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
    uint32_t owner = ownerOfFunction(dataInImage(), function);
    if (isOnlyClosure)
        namesReportedForFrames().set(owner, name.isolatedCopy());
    else if (auto result = namesReportedForFrames().add(owner, String()); result.isNewEntry)
        result.iterator->value = name.isolatedCopy();
    else if (result.iterator->value != name)
        result.iterator->value = String();
    hasNamesReportedForFrames.store(true, std::memory_order_release);
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
    // Whatever it binds.
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

std::optional<bool> hasOwnPropertyOfCompiledObject(JSObject* object, UniquedStringImpl* name)
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

bool forEachOwnPropertyOfCompiledObject(JSObject* object, const ScopedLambda<bool(UniquedStringImpl&, JSValue)>& functor)
{
    const LayoutData* layout = object->structure()->compiledLayout();
    if (!layout || !isObjectLayout(layout->kind))
        return false;
    auto* typedObject = uncheckedDowncast<Object>(object);
    if (typedObject->forEachFieldAsProperty(functor))
        return true;
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

bool isOfBuiltinModule(JSCell* cell)
{
    auto* closure = dynamicDowncast<Closure>(cell);
    return closure && closure->isDeclaredAs(functionIsOfBuiltinModule);
}

void didChangePrototypeProperty(VM& vm, JSCell* function)
{
    if (auto* closure = dynamicDowncast<Closure>(function))
        closure->structure()->aotInstance()->typed->didChangePrototypeOf(vm, closure);
}

} // namespace Typed

String nameOfTypedClosure(const JSFunction* closure)
{
    const Typed::Data& data = Typed::dataInImage();
    return Typed::textOfConstant(data, data.at<uint32_t>(data.functionsOffset)[uncheckedDowncast<Typed::Closure>(closure)->functionIndex()]);
}

unsigned lengthOfTypedClosure(const JSFunction* closure)
{
    const Typed::Data& data = Typed::dataInImage();
    return data.lengthOfFunction(uncheckedDowncast<Typed::Closure>(closure)->functionIndex());
}

uint64_t closureWordOfTypedFunction(std::span<const uint8_t> image, uint32_t function, uint64_t relative)
{
    auto& header = *reinterpret_cast<const ImageHeader*>(image.data());
    RELEASE_ASSERT(header.typedDataSize);
    return Typed::closureWord(*reinterpret_cast<const Typed::Data*>(image.subspan(header.typedDataOffset).data()), function, relative);
}

RangeOfClosureWords rangeOfClosureWordsIn(std::span<const uint8_t> image)
{
    auto& header = *reinterpret_cast<const ImageHeader*>(image.data());
    RELEASE_ASSERT(header.typedDataSize);
    auto& data = *reinterpret_cast<const Typed::Data*>(image.subspan(header.typedDataOffset).data());
    return { data.firstFunctionClosuresGoBy, data.endOfFunctionsClosuresGoBy, data.rowsLeftOutAheadOfClosures };
}

static Typed::BuiltinID entryThatTypedFunctionStandsFor(uint32_t function)
{
    const Typed::Data& data = Typed::dataInImage();
    auto* functions = data.at<uint32_t>(data.preludeFunctionsOffset);
    for (unsigned which = 0; which < Typed::numberOfPreludeFunctions; ++which) {
        if (functions[which] == function)
            return Typed::Builtins::singleton().entryCarriedOutBy(static_cast<Typed::PreludeFunction>(which));
    }
    return { };
}

BuiltinInTrace builtinThatTypedFunctionStandsFor(uint32_t function)
{
    Typed::BuiltinID id = entryThatTypedFunctionStandsFor(function);
    if (!id)
        return BuiltinInTrace::None;
    auto& builtins = Typed::Builtins::singleton();
    if (builtins.isHostFunctionInEngine(id))
        return BuiltinInTrace::HostFunction;
    return builtins.at(id).kind == Typed::BuiltinKind::Constructor ? BuiltinInTrace::ConstructorWrittenInJavaScript : BuiltinInTrace::WrittenInJavaScript;
}

String typedFunctionName(uint32_t function, bool asWritten)
{
    const Typed::Data& data = Typed::dataInImage();
    RELEASE_ASSERT(function < data.numberOfFunctions);
    if (!asWritten && Typed::hasNamesReportedForFrames.load(std::memory_order_acquire)) [[unlikely]] {
        Locker locker { Typed::namesReportedForFramesLock };
        if (String reported = Typed::namesReportedForFrames().get(Typed::ownerOfFunction(data, function)); !reported.isNull())
            return reported.isolatedCopy();
    }
    if (static_cast<Typed::Format::FunctionKind>(Typed::kindByteOf(function) & Typed::functionKindMask) == Typed::Format::FunctionKind::Module)
        return emptyString();
    String name = Typed::textOfConstant(data, data.at<uint32_t>(data.functionsOffset)[function]);
    // The `name` property of an accessor says which of the two it is. A stack trace does not.
    bool isMethod = static_cast<Typed::Format::FunctionKind>(Typed::kindByteOf(function) & Typed::functionKindMask) == Typed::Format::FunctionKind::Method;
    if (isMethod && (name.startsWith("get "_s) || name.startsWith("set "_s)))
        return name.substring(4);
    return name;
}

CodeType typedFunctionCodeType(uint32_t function)
{
    uint8_t kindByte = Typed::kindByteOf(function);
    return static_cast<Typed::Format::FunctionKind>(kindByte & Typed::functionKindMask) == Typed::Format::FunctionKind::Module && !(kindByte & Typed::moduleIsCommonJS) ? ModuleCode : FunctionCode;
}

bool isEnteredByConstruction(const Instance* instance, const CallFrame* previousTopCallFrame)
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
    return (Typed::kindByteOf(function) & Typed::functionKindMask) == Typed::functionKindOfResumeFunctionsNamedByOthers;
}

bool typedFunctionIsHiddenInStackTraces(uint32_t function)
{
    return Typed::kindByteOf(function) & Typed::functionIsHiddenInStackTraces;
}

bool typedFunctionWraps(uint32_t wrapper, uint32_t function)
{
    return typedFunctionIsHiddenInStackTraces(wrapper) && Typed::ownerOfFunction(Typed::dataInImage(), wrapper) == Typed::ownerOfFunction(Typed::dataInImage(), function);
}

bool typedFunctionStandsForNoFunction(uint32_t function)
{
    return (Typed::kindByteOf(function) & Typed::functionKindMask) == Typed::functionKindOfBodiesOfSeveralSites;
}

Vector<uint8_t> symbolsOfImage(std::span<const uint8_t> bytes)
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

    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> classOfMethod;
    for (uint32_t shape = 1; shape < data.numberOfLayouts; ++shape) {
        if (!data.at<uint32_t>(data.layoutsOffset)[shape])
            continue;
        const Typed::LayoutData& layout = *data.at<Typed::LayoutData>(data.at<uint32_t>(data.layoutsOffset)[shape]);
        if (!layout.isClass())
            continue;
        for (auto methods : { layout.methods(), layout.staticMethods() }) {
            for (auto& method : methods)
                classOfMethod.add(Typed::ownerOfFunction(data, method.function), layout.name);
        }
    }

    struct Row {
        uint32_t offset;
        uint32_t size;
        uint32_t name;
    };
    Vector<Row> rows;
    Vector<uint8_t> names;
    UncheckedKeyHashMap<String, uint32_t> placesOfNames;
    auto placeOf = [&](const String& name) {
        return placesOfNames.ensure(name, [&] {
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
        uint32_t function = program->indexAndFlagsOfFunctions()[index] >> 3;
        auto kind = static_cast<Typed::Format::FunctionKind>(data.at<uint8_t>(data.functionKindsOffset)[function] & Typed::functionKindMask);
        String name = kind == Typed::Format::FunctionKind::Module ? String("(module)"_s) : Typed::textOfConstant(data, data.at<uint32_t>(data.functionsOffset)[function]);
        if (name.isEmpty())
            name = "(anonymous)"_s;
        if (auto found = classOfMethod.find(Typed::ownerOfFunction(data, function)); found != classOfMethod.end() && data.sizeOfString(found->value))
            name = makeString(Typed::textOfConstant(data, found->value), '.', name);
        if (const uint8_t* at = kind == Typed::Format::FunctionKind::Module ? program->positionsOfTypedFunction(index) : nullptr) {
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

std::optional<FunctionRef::ReportedPosition> definitionOfTypedFunction(uint32_t function)
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
    for (uint32_t asked : { function, Typed::ownerOfFunction(Typed::dataInImage(), function) }) {
        auto found = std::ranges::lower_bound(compiled.get(), asked, { }, &Compiled::first);
        if (found != compiled->end() && found->first == asked)
            return FunctionRef { nullptr, found->second }.reportedPositionFor(BytecodeIndex(0));
    }
    return std::nullopt;
}

uint32_t typedFunctionEnteredInsideFunctionAt(const void* pc)
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
        for (uint32_t function = program->firstFunctionClosuresGoBy; function - program->rowsLeftOutAheadOfClosures < program->numberOfTypedEntries; ++function) {
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

bool typedFunctionRejectsWhatItThrows(uint32_t function)
{
    // Where the function starts and where it goes on show in stack traces. What goes wrong while the parameters of a generator are bound is thrown to the caller, of an async generator too.
    constexpr uint8_t looksAt = Typed::functionIsAsync | Typed::functionIsGenerator | Typed::functionIsHiddenInStackTraces;
    return (Typed::kindByteOf(function) & looksAt) == (Typed::functionIsAsync | Typed::functionIsHiddenInStackTraces);
}

void keepTypeErrorsThrown()
{
    Typed::ViolationLog::s_isKeptForHost = true;
}

String typeErrorsThrown()
{
    Typed::ViolationLog* log = Typed::ViolationLog::singletonIfAsked(false);
    return log ? log->placesThatThrew() : String();
}

String nameOfSlimClosure(JSObject* closure)
{
    return uncheckedDowncast<Typed::SlimClosure>(closure)->nameOfFunction();
}

String writtenNameOfClosure(JSObject* object)
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
    String name = Typed::textOfConstant(data, data.at<uint32_t>(data.functionsOffset)[function]);
    if (static_cast<Typed::Format::FunctionKind>(Typed::kindByteOf(function) & Typed::functionKindMask) == Typed::Format::FunctionKind::Bound)
        return name.substring("bound "_s.length());
    return name;
}

String displayNameOfClosure(JSObject* object)
{
    uint32_t function;
    if (auto* slim = dynamicDowncast<Typed::SlimClosure>(object))
        function = slim->functionIndex();
    else if (auto* closure = dynamicDowncast<Typed::Closure>(object))
        function = closure->functionIndex();
    else
        return String();
    const Typed::Data& data = Typed::dataInImage();
    String name = Typed::textOfConstant(data, data.at<uint32_t>(data.functionsOffset)[function]);
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

unsigned lengthOfSlimClosure(JSObject* closure)
{
    const Typed::Data& data = Typed::dataInImage();
    return data.lengthOfFunction(uncheckedDowncast<Typed::SlimClosure>(closure)->functionIndex());
}

JSValue textOfSlimClosure(JSGlobalObject* globalObject, JSObject* closure)
{
    return uncheckedDowncast<Typed::SlimClosure>(closure)->toString(globalObject);
}

bool isClosureOfTypedFunction(JSCell* cell, uint32_t function)
{
    const Typed::Data& data = Typed::dataInImage();
    if (auto* slim = dynamicDowncast<Typed::SlimClosure>(cell))
        return Typed::ownerOfFunction(data, slim->functionIndex()) == Typed::ownerOfFunction(data, function);
    auto* closure = dynamicDowncast<Typed::Closure>(cell);
    if (!closure) {
        Typed::BuiltinID id = entryThatTypedFunctionStandsFor(function);
        JSObject* object = id ? cell->getObject() : nullptr;
        auto& builtins = Typed::Builtins::singleton();
        return object && builtins.findCell(object->realm(), builtins.hasCell(id) ? builtins.cellOf(id) : builtins.cellOf(builtins.at(id).nameName)) == cell;
    }
    return Typed::ownerOfFunction(data, closure->functionIndex()) == Typed::ownerOfFunction(data, function);
}

} // namespace AOT

JSValue JSObject::getDirectOfCompiledObject(VM& vm, PropertyName name, unsigned* attributes) const
{
    if (!structure()->compiledLayout())
        return { };
    return uncheckedDowncast<AOT::Typed::Object>(const_cast<JSObject*>(this))->getDirectFromNativeCode(vm, name, attributes);
}

bool JSObject::reifyIfSlimClosure(VM& vm)
{
    if (type() != SlimClosureType)
        return false;
    uncheckedDowncast<AOT::Typed::SlimClosure>(this)->reifyProperties(vm);
    return true;
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

bool JSObject::isRefusedByLockAlone(VM& vm, UniquedStringImpl* name, JSValue& held)
{
    held = { };
    if (vm.m_aotInstances.isEmpty() || !vm.m_aotInstances[0]->typed)
        return false;
    if (auto* proxy = dynamicDowncast<JSGlobalProxy>(this))
        return proxy->target()->isRefusedByLockAlone(vm, name, held);
    if (isGlobalObject()) {
        if (!vm.useImmutableIntrinsics || !name)
            return false;
        unsigned attributesOfBinding;
        PropertyOffset offset = structure()->get(vm, name, attributesOfBinding);
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

bool JSObject::noteIfRefusedByLockAlone(VM& vm, UniquedStringImpl* name, bool isAnswered, bool isDelete)
{
    if (auto* proxy = dynamicDowncast<JSGlobalProxy>(this))
        return proxy->target()->noteIfRefusedByLockAlone(vm, name, isAnswered, isDelete);
    JSValue held;
    if (!isLockedBuiltin())
        return false;
    bool isByLockAlone = isRefusedByLockAlone(vm, name, held) && !(isDelete && name == vm.propertyNames->length.impl() && dynamicDowncast<JSArray>(this));
    if (!isByLockAlone && isDelete && name && !isGlobalObject() && !vm.m_aotInstances.isEmpty() && vm.m_aotInstances[0]->typed && (name != vm.propertyNames->prototype.impl() || !isCallable())) {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        PropertySlot slot(this, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isOwn = methodTable()->getOwnPropertySlot(this, globalObject(), PropertyName(name), slot);
        if (scope.exception()) [[unlikely]] {
            scope.clearException();
            return false;
        }
        if (isOwn && name->isSymbol())
            isByLockAlone = this != globalObject()->functionPrototype() || name != vm.propertyNames->hasInstanceSymbol.impl();
        else if (isOwn && !slot.isValue())
            isByLockAlone = true;
        else if (isOwn) {
            JSValue found = slot.getValue(globalObject(), PropertyName(name));
            isByLockAlone = found.isCell() ? !found.isSymbol() : found.isNumber() && isCallable() && name == vm.propertyNames->length.impl();
        }
    }
    if (!isByLockAlone)
        return false;
    AOT::Typed::State* state = vm.m_aotInstances.isEmpty() ? nullptr : vm.m_aotInstances[0]->typed;
    JSObject* constructor = isCallable() || isGlobalObject() ? nullptr : AOT::Typed::constructorOfLockedPrototype(vm, this);
    bool isOfClassOfProgram = constructor ? !!dynamicDowncast<AOT::Typed::Closure>(constructor) : !!dynamicDowncast<AOT::Typed::Closure>(this);
    bool isThrownInstead = isAnswered && isOfClassOfProgram && state && !state->data.logsTypeErrors;
    noteRefusalByLock(vm, name, { }, isAnswered && !isThrownInstead);
    return !isAnswered || isThrownInstead;
}

String JSObject::wordsOfRefusalByLock(VM& vm, UniquedStringImpl* name, ASCIILiteral otherwise)
{
    if (auto* proxy = dynamicDowncast<JSGlobalProxy>(this))
        return proxy->target()->wordsOfRefusalByLock(vm, name, otherwise);
    JSValue held;
    if (!isRefusedByLockAlone(vm, name, held))
        return otherwise;
    JSObject* constructor = isCallable() || isGlobalObject() ? nullptr : AOT::Typed::constructorOfLockedPrototype(vm, this);
    ASCIILiteral why = (constructor ? dynamicDowncast<AOT::Typed::Closure>(constructor) : dynamicDowncast<AOT::Typed::Closure>(this)) ? "classes are fixed in a program compiled with --aot"_s : "built-ins are locked in a program compiled with --aot"_s;
    String holder = AOT::Typed::nameOfLockedObject(vm, this);
    if (!name || PropertyName(name).isPrivateName())
        return withAOTErrorNote(makeString("Cannot change "_s, holder, ": "_s, why));
    if (name->isSymbol())
        return withAOTErrorNote(makeString("Cannot change ["_s, StringView(name), "] of "_s, holder, ": "_s, why));
    return withAOTErrorNote(makeString("Cannot change '"_s, StringView(name), "' of "_s, holder, ": "_s, why));
}

void JSObject::noteRefusalByLock(VM& vm, UniquedStringImpl* name, ASCIILiteral kind, bool isAnswered)
{
    AOT::Typed::State* state = vm.m_aotInstances.isEmpty() ? nullptr : vm.m_aotInstances[0]->typed;
    if (!state)
        return;
    if (auto* proxy = dynamicDowncast<JSGlobalProxy>(this)) {
        proxy->target()->noteRefusalByLock(vm, name, kind, isAnswered);
        return;
    }
    if (isGlobalObject()) {
        if (!vm.useImmutableIntrinsics || !name)
            return;
        unsigned attributesOfBinding;
        PropertyOffset offset = structure()->get(vm, name, attributesOfBinding);
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
        JSObject* constructor = AOT::Typed::constructorOfLockedPrototype(vm, this);
        if (isGlobalObject())
            kind = "global-binding-write"_s;
        else if (!constructor)
            kind = mayBePrototype() && !isCallable() ? "generator-prototype-write"_s : "builtin-member-replace"_s;
        else
            kind = dynamicDowncast<AOT::Typed::Closure>(constructor) ? "class-prototype-write"_s : "builtin-prototype-write"_s;
    }
    unsigned attributes;
    bool isOwn = name && !PropertyName(name).isPrivateName() && (isValidOffset(structure()->get(vm, name, attributes)) || (structure()->hasNonReifiedStaticProperties() && structure()->findPropertyHashEntry(name)));
    state->noteViolation(vm, { .kind = kind, .onBuiltin = this, .member = isOwn ? name : nullptr, .action = isAnswered ? AOT::Typed::State::ActionOnViolation::Dropped : AOT::Typed::State::ActionOnViolation::Threw });
}

ASCIILiteral JSObject::putDirectToCompiledObject(VM& vm, PropertyName name, JSValue value, unsigned attributes)
{
    const auto* layout = structure()->compiledLayout();
    if (layout && uncheckedDowncast<AOT::Typed::Object>(this)->putDirectFromNativeCode(vm, name, value, attributes))
        return { };
    if (layout && layout->mayGiveUp) {
        uncheckedDowncast<AOT::Typed::Object>(this)->becomeOrdinary(vm);
        PutPropertySlot slot(this);
        return putDirectInternal<PutModeDefineOwnProperty>(vm, name, value, attributes, slot);
    }
    return throwImmutableShapeError(vm);
}

} // namespace JSC

#endif // ENABLE(AOT)
