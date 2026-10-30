/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectVisiting.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeValidation.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "AOTTableInImage.h"
#include "AOTUnwinding.h"
#include "AOTVector.h"
#include "ButterflyInlines.h"
#include "CommonSlowPaths.h"
#include "CompleteSubspaceInlines.h"
#include "DeferGCInlines.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "FreeListInlines.h"
#include "LocalAllocatorInlines.h"
#include "ObjectConstructor.h"
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
    bool usesWideReferences { state.instance.options().useWideReferences() };
    // Tables that are not part of the image of the process lie in memory that the collector takes for its own.
    bool dataIsOutsideHeap { JSCellButterfly::isOutsideHeap(std::bit_cast<const Butterfly*>(data.data())) };
    // The rows of a table are objects of one allocation.
    uint32_t allocationOfReferences { UINT32_MAX };
    const uint32_t* references { nullptr };
};

struct ArrayNode {
    bool holdsRawDoubles() const { return indexingType == ArrayWithDouble; }
    Butterfly* storage() const { return std::bit_cast<Butterfly*>(words.data()); }

    IndexingType indexingType;
    bool isStorage;
    std::span<const uint64_t> words;
};

static JSObject* buildNode(Builder&, uint32_t first);

ALWAYS_INLINE static ArrayNode arrayNodeAt(const uint64_t* node, DataNodeHeader header)
{
    return { static_cast<IndexingType>(header.operand & DataNodeHeader::indexingTypeMask), !!(header.operand & DataNodeHeader::isStorage), std::span { node + DataNodeHeader::wordsBeforeElements, header.count } };
}

static ArrayNode arrayNodeAt(std::span<const uint64_t> data, uint32_t first)
{
    DataNodeHeader header = DataNodeHeader::decode(data[first]);
    RELEASE_ASSERT(header.kind() == DataNodeHeader::Kind::Array);
    return { static_cast<IndexingType>(header.operand & DataNodeHeader::indexingTypeMask), !!(header.operand & DataNodeHeader::isStorage), data.subspan(first + DataNodeHeader::wordsBeforeElements, header.count) };
}

// As compiled code makes it. A header is zero where the realm gives new arrays another kind of storage.
ALWAYS_INLINE static JSArray* createArrayCell(Builder& builder, uint64_t header, Butterfly* butterfly)
{
    static_assert(sizeof(JSArray) == 2 * sizeof(uint64_t) && JSObject::butterflyOffset() == sizeof(uint64_t));
    ASSERT(header);
    auto* allocator = static_cast<LocalAllocator*>(builder.state.instance.prefix().arrayAllocator);
    auto* words = static_cast<uint64_t*>(allocator->allocate(builder.vm.heap, sizeof(JSArray), nullptr, AllocationFailureMode::Assert));
    words[0] = header;
    words[1] = std::bit_cast<uint64_t>(butterfly);
    return reinterpret_cast<JSArray*>(words);
}

static uint64_t headerOfCopyOnWriteArray(const Instance& instance, IndexingType indexingType)
{
    return *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(&instance) + Instance::offsetOfNewCopyOnWriteArrayHeader(indexingType));
}

ALWAYS_INLINE static JSArray* tryCreateArrayAroundStorage(Builder& builder, const ArrayNode& node)
{
    if (!node.isStorage || !builder.dataIsOutsideHeap)
        return nullptr;
    uint64_t header = headerOfCopyOnWriteArray(builder.state.instance, node.indexingType);
    return header ? createArrayCell(builder, header, node.storage()) : nullptr;
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

// The word of a string constant in the instance is what a narrow slot holds.
ALWAYS_INLINE static uint32_t narrowStringConstant(Builder& builder, uint32_t number)
{
    Instance& instance = builder.state.instance;
    if (uint32_t word = instance.stringCell(number).get()) [[likely]]
        return word;
    return MarkedBlockCage::encode(instance.cellCageBase, builder.state.strings->constant(builder.vm, number));
}

ALWAYS_INLINE static JSValue buildValue(Builder& builder, uint64_t word)
{
    switch (DataWord::kindOf(word)) {
    case DataWord::Kind::Node:
        return buildNode(builder, DataWord::operandOf(word));
    case DataWord::Kind::StringConstant:
        if (!builder.usesWideReferences) [[likely]]
            return MarkedBlockCage::decode(builder.state.instance.cellCageBase, narrowStringConstant(builder, DataWord::operandOf(word)));
        break;
    case DataWord::Kind::Immediate:
    case DataWord::Kind::BigIntConstant:
        break;
    }
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

ALWAYS_INLINE static const uint32_t* referencesOfAllocation(Builder& builder, uint32_t allocation)
{
    if (builder.allocationOfReferences != allocation) [[unlikely]] {
        const ProgramTables& tables = builder.state.tables;
        builder.references = inlineReferenceTableOf(tables.layoutForShape(tables.allocations()[allocation].shape)).data();
        builder.allocationOfReferences = allocation;
    }
    return builder.references;
}

// As compiled code allocates it: the record has the first two words, and an allocator that may be one without room.
ALWAYS_INLINE static uint64_t* allocateObjectCell(Builder& builder, uint32_t allocation)
{
    StructureTable& structures = builder.state.objects->structures;
    const AllocationSiteRecord* record = builder.state.instance.objects.allocationSite(allocation);
    if (!record || !record->header) [[unlikely]]
        record = &structures.fillAllocationSite(builder.vm, allocation);
    uint64_t header = record->header;
    uint64_t orderAndShapeWords = record->orderAndShapeWords;
    FreeList& freeList = *reinterpret_cast<FreeList*>(static_cast<uint8_t*>(record->allocator) + LocalAllocator::offsetOfFreeList());
    auto* words = reinterpret_cast<uint64_t*>(freeList.allocateWithCellSize([&]() -> HeapCell* {
        return structures.allocateAsCompiledCodeDoes(builder.vm, allocation);
    }, freeList.cellSize()));
    words[0] = header;
    words[1] = orderAndShapeWords;
    return words;
}

// No collection begins or ends while a tree is built, and nothing leads to the cell yet. Where a member cannot be made, the cell is left without a word of the table.
ALWAYS_INLINE static JSObject* buildObjectFromBody(Builder& builder, const uint64_t* node, DataNodeHeader header)
{
    VM& vm = builder.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const uint32_t* counts = referencesOfAllocation(builder, header.operand);
    uint64_t* words = allocateObjectCell(builder, header.operand);
    uint32_t count = header.count;
    for (uint32_t index = 0; index < count; ++index)
        words[offsetOfFirstField / sizeof(uint64_t) + index] = node[1 + index];
    if (counts) {
        uint8_t* fields = reinterpret_cast<uint8_t*>(words);
        auto abandon = [&] {
            zeroSpan(std::span { fields + offsetOfFirstField, count * sizeof(uint64_t) });
            return nullptr;
        };
        const uint32_t* next = counts + numberOfInlineReferenceCounts;
        for (const uint32_t* end = next + counts[0] + counts[1]; next != end; ++next) {
            uint64_t& word = *reinterpret_cast<uint64_t*>(fields + *next);
            if (DataWord::kindOf(word) == DataWord::Kind::Immediate)
                continue;
            JSValue value = buildValue(builder, word);
            EXCEPTION_ASSERT(!!scope.exception() == !value);
            if (!value) [[unlikely]]
                return abandon();
            word = JSValue::encode(value);
        }
        for (const uint32_t* end = next + counts[2]; next != end; ++next) {
            uint32_t& word = *reinterpret_cast<uint32_t*>(fields + *next);
            if (NarrowDataWord::isFinal(word))
                continue;
            if (NarrowDataWord::kindOf(word) == DataWord::Kind::StringConstant) {
                word = narrowStringConstant(builder, NarrowDataWord::operandOf(word));
                continue;
            }
            JSValue value = buildValue(builder, NarrowDataWord::widen(word));
            EXCEPTION_ASSERT(!!scope.exception() == !value);
            if (!value) [[unlikely]]
                return abandon();
            word = MarkedBlockCage::encode(builder.state.instance.cellCageBase, value.asCell());
        }
    }
    Object* object = reinterpret_cast<Object*>(words);
    if (checksClaimsAboutObjects(vm)) [[unlikely]]
        validateSlotTypes(vm, *object);
    return object;
}

// The storage is filled before an array points at it, so the collector never finds a word of the table in it.
static JSArray* tryBuildArrayOfValues(Builder& builder, const ArrayNode& node)
{
    VM& vm = builder.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint64_t header = builder.state.instance.prefix().newArrayWithContiguousHeader;
    uint32_t count = static_cast<uint32_t>(node.words.size());
    if (node.indexingType != ArrayWithContiguous || !header || !count || count >= MAX_STORAGE_VECTOR_LENGTH)
        return nullptr;
    uint32_t vectorLength = count | 1;
    void* memory = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
    if (!memory) [[unlikely]]
        return nullptr;
    Butterfly* butterfly = Butterfly::fromBase(memory, 0, 0);
    butterfly->setVectorLength(vectorLength);
    butterfly->setPublicLength(count);
    auto* elements = reinterpret_cast<uint64_t*>(butterfly->contiguous().data());
    elements[vectorLength - 1] = JSValue::encode(JSValue());
    const uint64_t* words = node.words.data();
    for (uint32_t index = 0; index < count; ++index) {
        uint64_t word = words[index];
        if (DataWord::kindOf(word) != DataWord::Kind::Immediate) {
            // The rows of a table.
            const uint64_t* row = DataWord::kindOf(word) == DataWord::Kind::Node ? builder.data.data() + DataWord::operandOf(word) : nullptr;
            if (row && DataNodeHeader::decode(*row).kind() == DataNodeHeader::Kind::ObjectBody)
                word = JSValue::encode(buildObjectFromBody(builder, row, DataNodeHeader::decode(*row)));
            else
                word = JSValue::encode(buildValue(builder, word));
            EXCEPTION_ASSERT(!!scope.exception() == !word);
            if (!word) [[unlikely]]
                return nullptr;
        }
        elements[index] = word;
    }
    return createArrayCell(builder, header, butterfly);
}

static NEVER_INLINE JSArray* buildArrayAsRealmDoes(Builder& builder, const ArrayNode& node)
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

struct OtherMembersNode {
    std::span<const uint64_t> values;
    std::span<const uint32_t> identifierNumbers;
};

static OtherMembersNode membersOfNodeAt(std::span<const uint64_t> data, uint32_t first, DataNodeHeader::Kind kind)
{
    DataNodeHeader header = DataNodeHeader::decode(data[first]);
    RELEASE_ASSERT(header.kind() == kind);
    std::span<const uint64_t> words = data.subspan(first + 1, DataNodeHeader::numberOfWords(header.kind(), header.count) - 1);
    return { words.first(header.count), spanReinterpretCast<const uint32_t>(words.subspan(header.count)).first(header.count) };
}

static void defineOtherMembers(Builder& builder, Object& object, std::span<const uint32_t> identifierNumbers, size_t firstRoot)
{
    VM& vm = builder.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Instance* instance = &builder.state.instance;
    Vector<Object::ExtraProperty, 16> properties(identifierNumbers.size(), [&](size_t index) {
        return Object::ExtraProperty { identifierAt(instance, identifierNumbers[index]).impl(), builder.roots.at(firstRoot + index) };
    });
    if (object.tryCreateExtraPropertiesOfNewObject(vm, properties.span()))
        return;
    for (auto& property : properties) {
        PutPropertySlot slot(&object, true);
        defineDataProperty(instance, &object, property.name, property.value, slot);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

static NEVER_INLINE JSObject* buildObject(Builder& builder, uint32_t first)
{
    VM& vm = builder.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    DataNodeHeader header = DataNodeHeader::decode(builder.data[first]);
    uint32_t count = header.count;
    std::span<const uint64_t> words = builder.data.subspan(first + 1, DataNodeHeader::numberOfWords(header.kind(), count) - 1);
    std::span<const uint16_t> fieldIndices = spanReinterpretCast<const uint16_t>(words.subspan(count)).first(count);
    OtherMembersNode otherMembers;
    if (header.kind() == DataNodeHeader::Kind::ObjectWithOtherMembers) {
        RELEASE_ASSERT(DataWord::kindOf(words.back()) == DataWord::Kind::Node);
        otherMembers = membersOfNodeAt(builder.data, DataWord::operandOf(words.back()), DataNodeHeader::Kind::OtherMembers);
    }

    size_t firstRoot = builder.roots.size();
    appendValues(builder, words.first(count));
    RETURN_IF_EXCEPTION(scope, nullptr);
    appendValues(builder, otherMembers.values);
    RETURN_IF_EXCEPTION(scope, nullptr);

    ObjectModel& objects = *builder.state.objects;
    RELEASE_ASSERT(objects.layouts.hasLayoutForShape(objects.layouts.allocation(header.operand).shape));
    Object* object = uncheckedDowncast<Object>(objects.structures.allocate(vm, header.operand));
    std::span<const FieldData> fields = object->layout().fields();
    for (uint32_t index = 0; index < count; ++index) {
        bool isStored = object->store(vm, fields[fieldIndices[index]], AcceptedValue::provenAtBuildTime(builder.roots.at(firstRoot + index)));
        RELEASE_ASSERT(isStored);
    }
    if (checksClaimsAboutObjects(vm)) [[unlikely]]
        validateSlotTypes(vm, *object);
    if (!otherMembers.values.empty()) {
        defineOtherMembers(builder, *object, otherMembers.identifierNumbers, firstRoot + count);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    builder.roots.shrink(firstRoot);
    return object;
}

// What NewDictionary makes, with its properties.
static NEVER_INLINE JSObject* buildOrdinaryObject(Builder& builder, uint32_t first)
{
    VM& vm = builder.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    OtherMembersNode members = membersOfNodeAt(builder.data, first, DataNodeHeader::Kind::OrdinaryObject);
    int count = static_cast<int>(members.values.size());

    ProgramIdentifiers& identifiers = *vm.aotState().identifiers;
    Vector<UniquedStringImpl*, 16> names(count, [&](size_t index) {
        return identifiers.uid(members.identifierNumbers[index]);
    });
    Structure* structure = Structure::createForOneObject(vm, builder.realm->objectStructureForObjectConstructor(), names.span());

    // No collection begins or ends while a tree is built: the storage is filled before an object points at it, and the cell before anything leads to it.
    int inlineCapacity = structure->inlineCapacity();
    Butterfly* butterfly = structure->outOfLineCapacity() ? Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0) : nullptr;
    for (int index = inlineCapacity; index < count; ++index) {
        JSValue value = buildValue(builder, members.values[index]);
        RETURN_IF_EXCEPTION(scope, nullptr);
        butterfly->propertyStorage()[offsetInOutOfLineStorage(offsetForPropertyNumber(index, inlineCapacity))].setWithoutWriteBarrier(value);
    }
    std::array<JSValue, JSFinalObject::maxInlineCapacity> inlineValues;
    int inlineCount = std::min(count, inlineCapacity);
    for (int index = 0; index < inlineCount; ++index) {
        inlineValues[index] = buildValue(builder, members.values[index]);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    JSFinalObject* object = JSFinalObject::createWithButterfly(vm, structure, butterfly);
    for (int index = 0; index < inlineCount; ++index)
        object->putDirectOffset(vm, offsetForPropertyNumber(index, inlineCapacity), inlineValues[index]);
    return object;
}

static JSObject* buildNode(Builder& builder, uint32_t first)
{
    VM& vm = builder.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    RELEASE_ASSERT(first < builder.data.size());
    const uint64_t* node = builder.data.data() + first;
    DataNodeHeader header = DataNodeHeader::decode(*node);
    if (header.kind() == DataNodeHeader::Kind::Array) {
        ArrayNode array = arrayNodeAt(node, header);
        if (JSArray* created = tryCreateArrayAroundStorage(builder, array))
            return created;
    }
    if (!vm.isSafeToRecurse()) [[unlikely]] {
        throwStackOverflowError(builder.realm, scope);
        return nullptr;
    }
    switch (header.kind()) {
    case DataNodeHeader::Kind::Array: {
        ArrayNode array = arrayNodeAt(node, header);
        JSArray* created = tryBuildArrayOfValues(builder, array);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (created)
            return created;
        RELEASE_AND_RETURN(scope, buildArrayAsRealmDoes(builder, array));
    }
    case DataNodeHeader::Kind::Object:
    case DataNodeHeader::Kind::ObjectWithOtherMembers:
        RELEASE_AND_RETURN(scope, buildObject(builder, first));
    case DataNodeHeader::Kind::ObjectBody:
        RELEASE_AND_RETURN(scope, buildObjectFromBody(builder, node, header));
    case DataNodeHeader::Kind::OrdinaryObject:
        RELEASE_AND_RETURN(scope, buildOrdinaryObject(builder, first));
    case DataNodeHeader::Kind::OtherMembers:
        break;
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
        DeferGC deferGC(vm);
        MarkedArgumentBuffer roots;
        Builder builder { realm, vm, state, state.tables.data(), roots };
        JSObject* root = buildNode(builder, request.root);
        vm.mutatorFence();
        RELEASE_AND_RETURN(scope, root);
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

    DeferGC deferGC(vm);
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

    DeferGC deferGC(vm);
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
