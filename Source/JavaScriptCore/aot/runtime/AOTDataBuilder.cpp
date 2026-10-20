/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDataBuilder.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "AOTTableInImage.h"
#include "CommonSlowPaths.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "ObjectInitializationScope.h"
#include <bit>
#include <span>

namespace JSC { namespace AOT {

struct Builder {
    JSGlobalObject* realm;
    VM& vm;
    State& state;
    std::span<const uint64_t> data;
    MarkedArgumentBuffer& roots;
};

struct ArrayNode {
    bool holdsRawDoubles() const { return indexingType == ArrayWithDouble; }

    IndexingType indexingType;
    std::span<const uint64_t> words;
};

static JSObject* buildNode(Builder&, uint32_t first);

static ArrayNode arrayNodeAt(std::span<const uint64_t> data, uint32_t first)
{
    DataNodeHeader header = DataNodeHeader::decode(data[first]);
    RELEASE_ASSERT(header.kind() == DataNodeHeader::Kind::Array);
    return { static_cast<IndexingType>(header.operand), data.subspan(first + 1, header.count) };
}

static JSValue numberOrHole(uint64_t bits)
{
    double number = std::bit_cast<double>(bits);
    if (number != number)
        return { };
    return jsNumber(number);
}

JSValue constantOfDataWord(State& state, uint64_t word)
{
    switch (DataWord::kindOf(word)) {
    case DataWord::Kind::Immediate:
        return JSValue::decode(static_cast<EncodedJSValue>(word));
    case DataWord::Kind::StringConstant:
        return state.strings->constant(*state.instance.vm, DataWord::operandOf(word));
    case DataWord::Kind::BigIntConstant:
        return state.strings->bigIntConstant(state.instance.globalObject, DataWord::operandOf(word));
    case DataWord::Kind::Node:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue buildValue(Builder& builder, uint64_t word)
{
    if (DataWord::kindOf(word) == DataWord::Kind::Node)
        return buildNode(builder, DataWord::operandOf(word));
    return constantOfDataWord(builder.state, word);
}

static JSValue buildElement(Builder& builder, const ArrayNode& node, uint32_t index)
{
    uint64_t word = node.words[index];
    JSValue element = node.holdsRawDoubles() ? numberOrHole(word) : buildValue(builder, word);
    return element ? element : jsUndefined();
}

static void appendValues(Builder& builder, std::span<const uint64_t> words)
{
    auto scope = DECLARE_THROW_SCOPE(builder.vm);
    for (uint64_t word : words) {
        JSValue value = buildValue(builder, word);
        RETURN_IF_EXCEPTION(scope, void());
        builder.roots.append(value);
    }
    if (builder.roots.hasOverflowed()) [[unlikely]]
        throwOutOfMemoryError(builder.realm, scope);
}

static JSArray* buildArray(Builder& builder, const ArrayNode& node)
{
    VM& vm = builder.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t count = static_cast<uint32_t>(node.words.size());
    size_t firstRoot = builder.roots.size();
    if (!node.holdsRawDoubles()) {
        appendValues(builder, node.words);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    ObjectInitializationScope initializationScope(vm);
    JSArray* array = JSArray::tryCreateUninitializedRestricted(initializationScope, nullptr, builder.realm->arrayStructureForIndexingTypeDuringAllocation(node.indexingType), count);
    if (!array) {
        throwOutOfMemoryError(builder.realm, scope);
        return nullptr;
    }
    IndexingType createdType = array->indexingType();
    if (hasDouble(createdType)) {
        ASSERT(node.holdsRawDoubles());
        memcpySpan(unsafeMakeSpan(array->butterfly()->contiguousDouble().data(), count), spanReinterpretCast<const double>(node.words));
        return array;
    }
    uint32_t numberOfHoles = 0;
    for (uint32_t index = 0; index < count; ++index) {
        JSValue element = node.holdsRawDoubles() ? numberOrHole(node.words[index]) : builder.roots.at(firstRoot + index);
        numberOfHoles += !element;
        array->initializeIndex(initializationScope, index, element, createdType);
    }
    if (hasAnyArrayStorage(createdType))
        array->butterfly()->arrayStorage()->m_numValuesInVector -= numberOfHoles;
    builder.roots.shrink(firstRoot);
    return array;
}

static JSObject* buildObject(Builder& builder, uint32_t first)
{
    VM& vm = builder.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    DataNodeHeader header = DataNodeHeader::decode(builder.data[first]);
    uint32_t count = header.count;
    std::span<const uint64_t> words = builder.data.subspan(first + 1, DataNodeHeader::numberOfWords(DataNodeHeader::Kind::Object, count) - 1);
    std::span<const uint16_t> fieldIndices = spanReinterpretCast<const uint16_t>(words.subspan(count)).first(count);

    size_t firstRoot = builder.roots.size();
    appendValues(builder, words.first(count));
    RETURN_IF_EXCEPTION(scope, nullptr);

    ObjectModel& objects = *builder.state.objects;
    RELEASE_ASSERT(objects.layouts.hasLayoutForShape(objects.layouts.allocation(header.operand).shape));
    Object* object = uncheckedDowncast<Object>(objects.structures.allocate(vm, header.operand));
    std::span<const FieldData> fields = object->layout().fields();
    for (uint32_t index = 0; index < count; ++index) {
        bool isStored = object->store(vm, fields[fieldIndices[index]], builder.roots.at(firstRoot + index));
        RELEASE_ASSERT(isStored);
    }
    builder.roots.shrink(firstRoot);
    return object;
}

static JSObject* buildNode(Builder& builder, uint32_t first)
{
    VM& vm = builder.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!vm.isSafeToRecurse()) [[unlikely]] {
        throwStackOverflowError(builder.realm, scope);
        return nullptr;
    }
    switch (DataNodeHeader::decode(builder.data[first]).kind()) {
    case DataNodeHeader::Kind::Array:
        RELEASE_AND_RETURN(scope, buildArray(builder, arrayNodeAt(builder.data, first)));
    case DataNodeHeader::Kind::Object:
        RELEASE_AND_RETURN(scope, buildObject(builder, first));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSCellButterfly* tryCreateSharedStorage(JSGlobalObject* realm, State& state, const ArrayNode& node)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t count = static_cast<uint32_t>(node.words.size());
    JSCellButterfly* storage = JSCellButterfly::tryCreate(vm, node.indexingType | CopyOnWrite, count);
    if (!storage) {
        throwOutOfMemoryError(realm, scope);
        return nullptr;
    }
    for (uint32_t index = 0; index < count; ++index) {
        uint64_t word = node.words[index];
        storage->setIndex(vm, index, node.holdsRawDoubles() ? jsDoubleNumber(std::bit_cast<double>(word)) : constantOfDataWord(state, word));
    }
    return storage;
}

JSObject* tryBuildData(JSGlobalObject* realm, State& state, const DataRequest& request)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (request.sharedStorageOffset == DataRequest::noSharedStorage) {
        MarkedArgumentBuffer roots;
        Builder builder { realm, vm, state, state.tables.data(), roots };
        RELEASE_AND_RETURN(scope, buildNode(builder, request.root));
    }

    JSCellButterfly* shared = state.statics->at<JSCellButterfly*>(request.sharedStorageOffset);
    if (!shared) {
        shared = tryCreateSharedStorage(realm, state, arrayNodeAt(state.tables.data(), request.root));
        RETURN_IF_EXCEPTION(scope, nullptr);
        vm.mutatorFence();
        state.statics->setCell(request.sharedStorageOffset, shared);
    }
    return CommonSlowPaths::allocateNewArrayBuffer(vm, realm->arrayStructureForIndexingTypeDuringAllocation(shared->indexingMode()), shared);
}

JSSet* tryBuildSet(JSGlobalObject* realm, State& state, const CollectionDataRequest& request)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (request.tableInImage)
        return CompactTable::createSetInImage(realm, *state.tables.at<TableInImage>(request.tableInImage));

    MarkedArgumentBuffer roots;
    Builder builder { realm, vm, state, state.tables.data(), roots };
    ArrayNode keys = arrayNodeAt(builder.data, request.elements);
    JSSet* set = JSSet::create(vm, realm->setStructure());
    for (uint32_t index = 0; index < keys.words.size(); ++index) {
        JSValue key = buildElement(builder, keys, index);
        RETURN_IF_EXCEPTION(scope, nullptr);
        set->add(realm, key);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return set;
}

JSMap* tryBuildMap(JSGlobalObject* realm, State& state, const CollectionDataRequest& request)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (request.tableInImage)
        return CompactTable::createMapInImage(realm, *state.tables.at<TableInImage>(request.tableInImage));

    MarkedArgumentBuffer roots;
    Builder builder { realm, vm, state, state.tables.data(), roots };
    ArrayNode entries = arrayNodeAt(builder.data, request.elements);
    RELEASE_ASSERT(!entries.holdsRawDoubles());
    JSMap* map = JSMap::create(vm, realm->mapStructure());
    for (uint64_t word : entries.words) {
        RELEASE_ASSERT(DataWord::kindOf(word) == DataWord::Kind::Node);
        ArrayNode entry = arrayNodeAt(builder.data, DataWord::operandOf(word));
        RELEASE_ASSERT(entry.words.size() == 2);
        JSValue key = buildElement(builder, entry, 0);
        RETURN_IF_EXCEPTION(scope, nullptr);
        JSValue value = buildElement(builder, entry, 1);
        RETURN_IF_EXCEPTION(scope, nullptr);
        map->set(realm, key, value);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return map;
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewFromData, JSCell*, (Instance* instance, uint32_t request))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewFromData);
    OPERATION_RETURN(scope, tryBuildData(globalObject, *instance->state, requestAt<DataRequest>(instance, request)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewSetFromData, JSCell*, (Instance* instance, uint32_t request))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewSetFromData);
    OPERATION_RETURN(scope, tryBuildSet(globalObject, *instance->state, requestAt<CollectionDataRequest>(instance, request)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewMapFromData, JSCell*, (Instance* instance, uint32_t request))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewMapFromData);
    OPERATION_RETURN(scope, tryBuildMap(globalObject, *instance->state, requestAt<CollectionDataRequest>(instance, request)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
