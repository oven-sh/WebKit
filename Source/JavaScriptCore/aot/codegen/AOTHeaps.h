/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTEffects.h"
#include "AOTInstance.h"
#include "AOTMachine.h"
#include "AOTMemoryLocation.h"
#include "AOTVector.h"
#include "IndexingType.h"
#include "JSArrayBuffer.h"
#include "JSGlobalObject.h"
#include "JSPropertyNameEnumerator.h"
#include <array>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

using HeapList = Vector<const Machine::AbstractHeap*, 16>;

// Every list below is v(name, offset, traits) or v(name, offset, traits, parent). The offsets are evaluated in AOTHeaps.cpp alone.
// A heap is added to the tree behind its parent, so the constructor expands the lists in the order in which the struct below declares them, and within a list a parent stands ahead of its children.
// The traits are the constants of AOTHeaps::Traits.

// v(name, traits), below the root. To the IR a weak table is a collection like any other. A garbage collection only writes the weak ones, so they have heaps of their own:
// values read from a Map or a Set stay valid across it.
#define FOR_EACH_AOT_HEAP_OUTSIDE_IR(v) \
    v(collectorState, OutsideDeclaredEffects) \
    v(argumentBuffer, OutsideDeclaredEffects) \
    v(frameSlots, OutsideDeclaredEffects) \
    v(counters, OutsideDeclaredEffects) \
    v(weakTableStorage, Mutable) \
    v(weakTableEntries, Mutable)

// AOTObject_shapeWord is read of every cell. In one that is no compiled object other things lie there and change. None of them has compiledObjectShapeBit, so every test of the word fails for them all.
// A word of AOTInstance_fieldsFilledOnce is zero until it gets its value and keeps that value. Whoever reads zero goes the way that needs no word, so a read may stand anywhere.
// So it is with the one flag of AOTCompiledStructure_flags that is set behind the creation of the Structure: EveryFieldIsBornConfigurable.
#define FOR_EACH_AOT_FIELD_HEAP_UNDER_ROOT(v) \
    v(AOTInstance_vm, AOT_INSTANCE_OFFSET(vm), Immutable) \
    v(AOTCompiledStructure_flags, CompiledStructure::offsetOfFlags(), FilledOnce) \
    v(AOTInstance_fixedFields, 0, ImmutablePermanentCell) \
    v(AOTInstance_fieldsFilledOnce, 0, FilledOnce) \
    v(AOTClosure_entry, offsetOfClosureEntry, Immutable) \
    v(AOTObject_shapeWord, offsetOfShapeWord, Immutable) \
    v(ArrayBuffer_shared, ArrayBuffer::offsetOfShared(), Immutable) \
    v(JSArrayBuffer_impl, JSArrayBuffer::offsetOfImpl(), ImmutableInnerPointer) \
    v(JSGlobalObject_nullSetterStrictFunction, JSGlobalObject::offsetOfNullSetterStrictFunction(), Immutable) \
    v(JSPropertyNameEnumerator_cachedOrderWord, JSPropertyNameEnumerator::cachedOrderWordOffset(), Immutable) \
    v(JSPropertyNameEnumerator_cachedOrderWordMask, JSPropertyNameEnumerator::cachedOrderWordMaskOffset(), Immutable) \
    v(JSPropertyNameEnumerator_endOwnPropertyIndex, JSPropertyNameEnumerator::endOwnPropertyIndexOffset(), Immutable) \
    v(AOTImage_tables, 0, ImmutablePermanentCell)

// AOTObject_hiddenWords and AOTInstance_classGuards lie beside the buckets of their parents, so every mask that names one of those buckets names them too:
// whatever adds a field may give the object another Extension, and what ends the guard of a class ends the guards of the classes below it.
// No program can tell whether the closure table has made a closure yet, or whether the value word of a module is filled. Both heaps stay below RuntimeState, so whatever writes that ends a numbered load.
// Nor can it tell whether the cell of a constant has been made. AOTInstance_stringCells has the words of strings and keys, AOTInstance_constantCells those of every other constant.
// Both stay below AllocatorState and change: code fills some of these words itself, and a zero that was read ahead of the store must not be found again behind it.
// Nor can it tell where a class token has found a private name before. A word of AOTClassToken_knownStructures or AOTClassToken_records is zero until it gets its value and keeps that value, and the number of the Structure is the last word of a record to get one.
#define FOR_EACH_AOT_FIELD_HEAP_UNDER_IR_HEAP(v) \
    v(AOTInstance_mutableFields, 0, Mutable, RuntimeState) \
    v(AOTInstance_stringCells, 0, PermanentCellOutsideDeclaredEffects, AllocatorState) \
    v(AOTInstance_constantCells, 0, PermanentCellOutsideDeclaredEffects, AllocatorState) \
    v(AOTInstance_closureCells, 0, PermanentCellOutsideDeclaredEffects, RuntimeState) \
    v(AOTInstance_moduleValues, 0, OutsideDeclaredEffects, RuntimeState) \
    v(AOTInstance_globalBindings, 0, Mutable, RuntimeState) \
    v(AOTInstance_siteCaches, 0, OutsideDeclaredEffects, RuntimeState) \
    v(AOTPropertyCaches_front, 0, Mutable, RuntimeState) \
    v(AOTClassToken_knownStructures, JSCellButterfly::offsetOfData() + ClassToken::knownStructuresSlot * sizeof(EncodedJSValue), OutsideDeclaredEffects, RuntimeState) \
    v(AOTClassToken_records, 0, OutsideDeclaredEffects, RuntimeState) \
    v(AOTCompactTable_lookupHeader, 0, Mutable, CollectionStorage) \
    v(AOTStringBuilderStorage_fields, 0, Mutable, StringBuilderContents) \
    v(AOTStackStringBuilder_fields, 0, Mutable, StringBuilderContents) \
    v(stringBuilderBytes, 0, Mutable, StringBuilderContents) \
    v(AOTObject_orderWord, offsetOfOrderWord, Mutable, OrderWords) \
    v(AOTSlimClosure_word, offsetOfSlimClosureWord, Mutable, ShapeWords) \
    v(AOTExtension_granuleBits, offsetOfExtensionGranuleBits, Mutable, ObjectFields) \
    v(AOTObject_hiddenWords, 0, Mutable, ObjectFields) \
    v(AOTObject_scopeAndClosuresOfSlots, 0, OutsideDeclaredEffects, ObjectFields) \
    v(AOTInstance_classGuards, 0, Mutable, MemberGuards) \
    v(JSGlobalObject_globalLexicalBindingEpoch, JSGlobalObject::globalLexicalBindingEpochOffset(), Mutable, RuntimeState) \
    v(TypedArrayProperties, 0, Mutable, TypedArrayElements)

// JSCell_typeInfoType does change in place, between the types of a row of changesOfCellTypeInPlace: PrimitiveEmitter::hasCellTypeIn() keeps every test under this heap to ranges that cannot tell.
// Nothing writes a field of a JSPropertyNameEnumerator once it is created.
#define FOR_EACH_JSC_FIELD_HEAP_UNDER_ROOT(v) \
    v(ClassInfo_parentClass, ClassInfo::offsetOfParentClass(), Immutable) \
    v(JSCell_typeInfoType, JSCell::typeInfoTypeOffset(), Immutable) \
    v(JSPropertyNameEnumerator_cachedInlineCapacity, JSPropertyNameEnumerator::cachedInlineCapacityOffset(), Immutable) \
    v(JSPropertyNameEnumerator_cachedPropertyNamesVector, JSPropertyNameEnumerator::cachedPropertyNamesVectorOffset(), ImmutableInnerPointer) \
    v(JSPropertyNameEnumerator_cachedStructureID, JSPropertyNameEnumerator::cachedStructureIDOffset(), Immutable) \
    v(JSPropertyNameEnumerator_endGenericPropertyIndex, JSPropertyNameEnumerator::endGenericPropertyIndexOffset(), Immutable) \
    v(JSPropertyNameEnumerator_endStructurePropertyIndex, JSPropertyNameEnumerator::endStructurePropertyIndexOffset(), Immutable) \
    v(JSPropertyNameEnumerator_indexLength, JSPropertyNameEnumerator::indexedLengthOffset(), Immutable) \
    v(Structure_classInfo, Structure::classInfoOffset(), Immutable) \
    v(Structure_realm, Structure::realmOffset(), Immutable)

// No program can tell whether JSRopeString_fiber0 holds a fiber or a StringImpl.
#define FOR_EACH_JSC_FIELD_HEAP_UNDER_IR_HEAP(v) \
    v(ArrayBuffer_data, ArrayBuffer::offsetOfData(), InnerPointer, TypedArrayStorage) \
    v(ArrayBuffer_sizeInBytes, ArrayBuffer::offsetOfSizeInBytes(), Mutable, TypedArrayStorage) \
    v(ArrayStorage_numValuesInVector, ArrayStorage::numValuesInVectorOffset(), Mutable, ArrayStorage) \
    v(Butterfly_arrayBuffer, Butterfly::offsetOfArrayBuffer(), Mutable, TypedArrayStorage) \
    v(Butterfly_publicLength, Butterfly::offsetOfPublicLength(), Mutable, ArrayStorage) \
    v(Butterfly_vectorLength, Butterfly::offsetOfVectorLength(), Mutable, ArrayStorage) \
    v(DateInstance_internalNumber, DateInstance::offsetOfInternalNumber(), Mutable, InternalFields) \
    v(DateInstance_cachedGregorianDateTime, DateInstance::offsetOfCachedGregorianDateTime(), Mutable, InternalFields) \
    v(DateInstance_cachedGregorianDateTimeUTC, DateInstance::offsetOfCachedGregorianDateTimeUTC(), Mutable, InternalFields) \
    v(InlineWatchpointSet_data, InlineWatchpointSet::offsetOfData(), Mutable, RuntimeState) \
    v(JSArrayBufferView_byteOffset, JSArrayBufferView::offsetOfByteOffset(), Mutable, TypedArrayStorage) \
    v(JSArrayBufferView_length, JSArrayBufferView::offsetOfLength(), Mutable, TypedArrayStorage) \
    v(JSArrayBufferView_mode, JSArrayBufferView::offsetOfMode(), Mutable, TypedArrayStorage) \
    v(JSArrayBufferView_vector, JSArrayBufferView::offsetOfVector(), InnerPointer, TypedArrayStorage) \
    v(JSBigInt_length, JSBigInt::offsetOfLength(), Immutable, RuntimeState) \
    v(JSBigInt_data, JSBigInt::offsetOfData(), Immutable, RuntimeState) \
    v(JSBigInt_sign, JSCell::typeInfoFlagsOffset(), Immutable, RuntimeState) \
    v(JSBoundFunction_targetFunction, JSBoundFunction::offsetOfTargetFunction(), Mutable, RuntimeState) \
    v(JSBoundFunction_boundThis, JSBoundFunction::offsetOfBoundThis(), Mutable, RuntimeState) \
    v(JSBoundFunction_boundArg0, JSBoundFunction::offsetOfBoundArgs() + sizeof(WriteBarrier<Unknown>) * 0, Mutable, RuntimeState) \
    v(JSBoundFunction_boundArg1, JSBoundFunction::offsetOfBoundArgs() + sizeof(WriteBarrier<Unknown>) * 1, Mutable, RuntimeState) \
    v(JSBoundFunction_boundArg2, JSBoundFunction::offsetOfBoundArgs() + sizeof(WriteBarrier<Unknown>) * 2, Mutable, RuntimeState) \
    v(JSBoundFunction_nameMayBeNull, JSBoundFunction::offsetOfNameMayBeNull(), Mutable, RuntimeState) \
    v(JSBoundFunction_length, JSBoundFunction::offsetOfLength(), Mutable, RuntimeState) \
    v(JSBoundFunction_boundArgsLength, JSBoundFunction::offsetOfBoundArgsLength(), Mutable, RuntimeState) \
    v(JSBoundFunction_canConstruct, JSBoundFunction::offsetOfCanConstruct(), Mutable, RuntimeState) \
    v(JSCallee_scope, JSCallee::offsetOfScopeChain(), Mutable, RuntimeState) \
    v(JSCell_header, 0, Mutable, ShapeWords) \
    v(JSFunction_executableOrRareData, JSFunction::offsetOfExecutableOrRareData(), Mutable, RuntimeState) \
    v(JSGlobalObject_regExpGlobalData_cachedResult_lastRegExp, JSGlobalObject::regExpGlobalDataOffset() + RegExpGlobalData::offsetOfCachedResult() + RegExpCachedResult::offsetOfLastRegExp(), Mutable, RegExpState) \
    v(JSGlobalObject_regExpGlobalData_cachedResult_lastInput, JSGlobalObject::regExpGlobalDataOffset() + RegExpGlobalData::offsetOfCachedResult() + RegExpCachedResult::offsetOfLastInput(), Mutable, RegExpState) \
    v(JSGlobalObject_regExpGlobalData_cachedResult_result_start, JSGlobalObject::regExpGlobalDataOffset() + RegExpGlobalData::offsetOfCachedResult() + RegExpCachedResult::offsetOfResult() + OBJECT_OFFSETOF(MatchResult, start), Mutable, RegExpState) \
    v(JSGlobalObject_regExpGlobalData_cachedResult_result_end, JSGlobalObject::regExpGlobalDataOffset() + RegExpGlobalData::offsetOfCachedResult() + RegExpCachedResult::offsetOfResult() + OBJECT_OFFSETOF(MatchResult, end), Mutable, RegExpState) \
    v(JSGlobalObject_regExpGlobalData_cachedResult_reified, JSGlobalObject::regExpGlobalDataOffset() + RegExpGlobalData::offsetOfCachedResult() + RegExpCachedResult::offsetOfReified(), Mutable, RegExpState) \
    v(JSGlobalObject_regExpGlobalData_cachedResult_oneCharacterMatch, JSGlobalObject::regExpGlobalDataOffset() + RegExpGlobalData::offsetOfCachedResult() + RegExpCachedResult::offsetOfOneCharacterMatch(), Mutable, RegExpState) \
    v(JSObject_butterfly, JSObject::butterflyOffset(), InnerPointer, ArrayStorage) \
    v(JSRopeString_fiber0, JSRopeString::offsetOfFiber0(), InnerPointerOutsideDeclaredEffects, StringStorage) \
    v(JSRopeString_fiber1, JSRopeString::offsetOfFiber1(), Mutable, StringStorage) \
    v(JSRopeString_fiber2, JSRopeString::offsetOfFiber2(), Mutable, StringStorage) \
    v(RegExp_flags, RegExp::offsetOfFlags(), Mutable, RegExpState) \
    v(RegExp_minimumSize, RegExp::offsetOfMinimumSize(), Mutable, RegExpState) \
    v(RegExpObject_regExpAndFlags, RegExpObject::offsetOfRegExpAndFlags(), Mutable, RegExpState) \
    v(RegExpObject_lastIndex, RegExpObject::offsetOfLastIndex(), Mutable, RegExpState) \
    v(StringImpl_data, StringImpl::dataOffset(), ImmutableInnerPointer, StringStorage) \
    v(StringImpl_hashAndFlags, StringImpl::flagsOffset(), Mutable, StringStorage) \
    v(StringImpl_length, StringImpl::lengthMemoryOffset(), Immutable, StringStorage) \
    v(Structure_bitField, Structure::bitFieldOffset(), Mutable, RuntimeState) \
    v(Structure_indexingModeIncludingHistory, Structure::indexingModeIncludingHistoryOffset(), Immutable, RuntimeState) \
    v(Structure_previousOrRareData, Structure::previousOrRareDataOffset(), Mutable, RuntimeState) \
    v(Structure_prototype, Structure::prototypeOffset(), Immutable, PrototypeLinks) \
    v(Structure_seenProperties, Structure::seenPropertiesOffset(), Mutable, RuntimeState) \
    v(StructureRareData_cachedEnumerableStrings, StructureRareData::offsetOfCachedPropertyNames(CachedPropertyNamesKind::EnumerableStrings), Mutable, RuntimeState) \
    v(StructureRareData_cachedStrings, StructureRareData::offsetOfCachedPropertyNames(CachedPropertyNamesKind::Strings), Mutable, RuntimeState) \
    v(StructureRareData_cachedSymbols, StructureRareData::offsetOfCachedPropertyNames(CachedPropertyNamesKind::Symbols), Mutable, RuntimeState) \
    v(StructureRareData_cachedStringsAndSymbols, StructureRareData::offsetOfCachedPropertyNames(CachedPropertyNamesKind::StringsAndSymbols), Mutable, RuntimeState) \
    v(StructureRareData_cachedPropertyNameEnumeratorAndFlag, StructureRareData::offsetOfCachedPropertyNameEnumeratorAndFlag(), Mutable, RuntimeState) \
    v(StructureRareData_specialPropertyCache, StructureRareData::offsetOfSpecialPropertyCache(), Mutable, RuntimeState) \
    v(SpecialPropertyCache_cachedToStringTagValue, SpecialPropertyCache::offsetOfCache(CachedSpecialPropertyKey::ToStringTag) + SpecialPropertyCacheEntry::offsetOfValue(), Mutable, RuntimeState) \
    v(JSMap_storage, (JSMap::offsetOfStorage()), Mutable, CollectionStorage) \
    v(JSSet_storage, (JSSet::offsetOfStorage()), Mutable, CollectionStorage) \
    v(JSPromise_packed, JSPromise::offsetOfPacked(), Mutable, PromiseState) \
    v(JSPromise_slot, JSPromise::offsetOfSlot(), Mutable, PromiseState) \
    v(WatchpointSet_state, WatchpointSet::offsetOfState(), Mutable, RuntimeState)

#define FOR_EACH_JSC_FIELD_HEAP_UNDER_FIELD_HEAP(v) \
    v(JSCell_structureID, JSCell::structureIDOffset(), Mutable, JSCell_header) \
    v(JSCell_usefulBytes, JSCell::indexingTypeAndMiscOffset(), Mutable, JSCell_header) \
    v(DirectArguments_callee, DirectArguments::offsetOfCallee(), Mutable, elementsOfArguments) \
    v(DirectArguments_length, DirectArguments::offsetOfLength(), Mutable, elementsOfArguments) \
    v(DirectArguments_minCapacity, DirectArguments::offsetOfMinCapacity(), Mutable, elementsOfArguments) \
    v(DirectArguments_mappedArguments, DirectArguments::offsetOfMappedArguments(), Mutable, elementsOfArguments) \
    v(DirectArguments_modifiedArgumentsDescriptor, DirectArguments::offsetOfModifiedArgumentsDescriptor(), Mutable, elementsOfArguments) \
    v(JSCell_cellState, JSCell::cellStateOffset(), OutsideDeclaredEffects, collectorState) \
    v(JSCell_indexingTypeAndMisc, JSCell::indexingTypeAndMiscOffset(), Mutable, JSCell_usefulBytes) \
    v(JSCell_typeInfoFlags, JSCell::typeInfoFlagsOffset(), Mutable, JSCell_usefulBytes) \
    v(JSRopeString_length, JSRopeString::offsetOfLength(), Immutable, JSRopeString_fiber1) \
    v(ScopedArguments_overrodeThings, ScopedArguments::offsetOfOverrodeThings(), Mutable, elementsOfArguments) \
    v(ScopedArguments_scope, ScopedArguments::offsetOfScope(), Mutable, elementsOfArguments) \
    v(ScopedArguments_storage, ScopedArguments::offsetOfStorage(), Mutable, elementsOfArguments) \
    v(ScopedArguments_table, ScopedArguments::offsetOfTable(), Mutable, elementsOfArguments) \
    v(ScopedArguments_totalLength, ScopedArguments::offsetOfTotalLength(), Mutable, elementsOfArguments) \
    v(ScopedArgumentsTable_arguments, ScopedArgumentsTable::offsetOfArguments(), Mutable, elementsOfArguments) \
    v(ScopedArgumentsTable_length, ScopedArgumentsTable::offsetOfLength(), Mutable, elementsOfArguments) \
    v(WeakMapImpl_capacity, WeakMapImpl<WeakMapBucket<WeakMapBucketDataKey>>::offsetOfCapacity(), Mutable, weakTableStorage) \
    v(WeakMapImpl_buffer, WeakMapImpl<WeakMapBucket<WeakMapBucketDataKey>>::offsetOfBuffer(), InnerPointer, weakTableStorage) \
    v(WeakMapImpl_keyCount, WeakMapImpl<WeakMapBucket<WeakMapBucketDataKey>>::offsetOfKeyCount(), Mutable, weakTableStorage) \
    v(WeakMapImpl_deleteCount, WeakMapImpl<WeakMapBucket<WeakMapBucketDataKey>>::offsetOfDeleteCount(), Mutable, weakTableStorage) \
    v(WeakMapBucket_value, WeakMapBucket<WeakMapBucketDataKeyValue>::offsetOfValue(), Mutable, weakTableEntries) \
    v(WeakMapBucket_key, WeakMapBucket<WeakMapBucketDataKeyValue>::offsetOfKey(), Mutable, weakTableEntries) \
    v(JSRopeString_flags, JSRopeString::offsetOfFlags(), Mutable, JSRopeString_fiber0) \
    v(VM_heap_barrierThreshold, VM::offsetOfHeapBarrierThreshold(), OutsideDeclaredEffects, VM_heapState) \
    v(VM_heap_mutatorShouldBeFenced, VM::offsetOfHeapMutatorShouldBeFenced(), OutsideDeclaredEffects, VM_heapState)

// An array stays an array, so an array's butterfly word and another object's are separate heaps, and so are the bytes that name the kind of storage. The parent is for a cell that may be either.
#define FOR_EACH_AOT_FIELD_HEAP_UNDER_FIELD_HEAP(v) \
    v(cards, 0, OutsideDeclaredEffects, collectorState) \
    v(VM_heapState, 0, OutsideDeclaredEffects, collectorState) \
    v(JSArray_butterfly, JSObject::butterflyOffset(), InnerPointer, JSObject_butterfly) \
    v(JSNonArray_butterfly, JSObject::butterflyOffset(), InnerPointer, JSObject_butterfly)

// Behind FOR_EACH_JSC_FIELD_HEAP_UNDER_FIELD_HEAP.
#define FOR_EACH_AOT_FIELD_HEAP_UNDER_JSC_FIELD_HEAP(v) \
    v(JSArray_indexingTypeAndMisc, JSCell::indexingTypeAndMiscOffset(), Mutable, JSCell_indexingTypeAndMisc) \
    v(JSNonArray_indexingTypeAndMisc, JSCell::indexingTypeAndMiscOffset(), Mutable, JSCell_indexingTypeAndMisc)

// v(name, offset, size of an element, traits, parent), the parent being an expression in the constructor.
#define FOR_EACH_AOT_INDEXED_HEAP(v) \
    v(AOTClosure_captures, offsetOfClosureCaptures, sizeof(EncodedJSValue), Immutable, entire(Typed::AbstractHeap::EnvironmentFields)) \
    v(AOTSlimClosure_captures, offsetOfSlimClosureCaptures, sizeof(EncodedJSValue), Immutable, entire(Typed::AbstractHeap::EnvironmentFields)) \
    v(ArrayStorage_vector, ArrayStorage::vectorOffset(), sizeof(WriteBarrier<Unknown>), Mutable, elementsOfArrays) \
    v(indexedInt32Properties, 0, sizeof(EncodedJSValue), Mutable, elementsOfArrays) \
    v(indexedDoubleProperties, 0, sizeof(double), Mutable, elementsOfArrays) \
    v(indexedContiguousProperties, 0, sizeof(EncodedJSValue), Mutable, elementsOfArrays) \
    v(DirectArguments_storage, DirectArguments::storageOffset(), sizeof(EncodedJSValue), Mutable, elementsOfArguments) \
    v(ScopedArguments_Storage_storage, 0, sizeof(EncodedJSValue), Mutable, elementsOfArguments) \
    v(scopedArgumentsTableArguments, 0, sizeof(int32_t), Mutable, elementsOfArguments) \
    v(CompleteSubspace_allocatorForSizeStep, CompleteSubspace::offsetOfAllocatorForSizeStep(), sizeof(Allocator), Mutable, entire(Typed::AbstractHeap::AllocatorState)) \
    v(JSInternalFieldObjectImpl_internalFields, JSInternalFieldObjectImpl<>::offsetOfInternalFields(), sizeof(WriteBarrier<Unknown>), Mutable, entire(Typed::AbstractHeap::InternalFields)) \
    v(JSPropertyNameEnumerator_cachedPropertyNamesVectorContents, 0, sizeof(WriteBarrier<JSString>), Immutable, root) \
    v(WriteBarrierBuffer_bufferContents, 0, sizeof(JSCell*), OutsideDeclaredEffects, collectorState) \
    v(characters8, 0, sizeof(Latin1Character), Mutable, entire(Typed::AbstractHeap::StringStorage)) \
    v(characters16, 0, sizeof(char16_t), Mutable, entire(Typed::AbstractHeap::StringStorage)) \
    v(singleCharacterStrings, 0, sizeof(JSString*), ImmutablePermanentCell, root)

// Made once for a build, ahead of Compile. Every thread reads it, and nothing changes it.
struct AOTHeaps {
    WTF_MAKE_NONCOPYABLE(AOTHeaps);

    struct Traits {
        using Set = OptionSet<Machine::HeapTrait>;
        static constexpr Set Mutable { };
        static constexpr Set Immutable { Machine::HeapTrait::Immutable };
        static constexpr Set InnerPointer { Machine::HeapTrait::HoldsInnerPointer };
        static constexpr Set ImmutableInnerPointer { Machine::HeapTrait::Immutable, Machine::HeapTrait::HoldsInnerPointer };
        static constexpr Set PermanentCell { Machine::HeapTrait::HoldsPermanentCell };
        static constexpr Set ImmutablePermanentCell { Machine::HeapTrait::Immutable, Machine::HeapTrait::HoldsPermanentCell };
        static constexpr Set FilledOnce { Machine::HeapTrait::Immutable, Machine::HeapTrait::IsFilledOnce };
        static constexpr Set OutsideDeclaredEffects { Machine::HeapTrait::IsOutsideDeclaredEffects };
        static constexpr Set InnerPointerOutsideDeclaredEffects { Machine::HeapTrait::HoldsInnerPointer, Machine::HeapTrait::IsOutsideDeclaredEffects };
        static constexpr Set PermanentCellOutsideDeclaredEffects { Machine::HeapTrait::HoldsPermanentCell, Machine::HeapTrait::IsOutsideDeclaredEffects };
    };

    AOTHeaps();

    // What Machine::Procedure takes.
    const Machine::HeapTree& tree() const LIFETIME_BOUND { return m_tree; }

    Machine::AbstractHeap root;
    // Two buckets of Typed::AbstractHeap::Elements, under other names.
    Machine::AbstractHeap elementsOfArrays;
    Machine::AbstractHeap elementsOfArguments;

#define AOT_DECLARE_HEAP(name, ...) Machine::AbstractHeap name;
    FOR_EACH_AOT_HEAP_OUTSIDE_IR(AOT_DECLARE_HEAP)
    FOR_EACH_AOT_FIELD_HEAP_UNDER_ROOT(AOT_DECLARE_HEAP)
    FOR_EACH_AOT_FIELD_HEAP_UNDER_IR_HEAP(AOT_DECLARE_HEAP)
    FOR_EACH_JSC_FIELD_HEAP_UNDER_ROOT(AOT_DECLARE_HEAP)
    FOR_EACH_JSC_FIELD_HEAP_UNDER_IR_HEAP(AOT_DECLARE_HEAP)
    FOR_EACH_AOT_FIELD_HEAP_UNDER_FIELD_HEAP(AOT_DECLARE_HEAP)
    FOR_EACH_JSC_FIELD_HEAP_UNDER_FIELD_HEAP(AOT_DECLARE_HEAP)
    FOR_EACH_AOT_FIELD_HEAP_UNDER_JSC_FIELD_HEAP(AOT_DECLARE_HEAP)
#undef AOT_DECLARE_HEAP

#define AOT_DECLARE_INDEXED_HEAP(name, ...) Machine::IndexedAbstractHeap name;
    FOR_EACH_AOT_INDEXED_HEAP(AOT_DECLARE_INDEXED_HEAP)
#undef AOT_DECLARE_INDEXED_HEAP

    // Below Typed::AbstractHeap::NamedProperties.
    Machine::NumberedAbstractHeap properties;

    // Other names of heaps above. Set last.
    Machine::AbstractHeap JSCell_freeListNext;
    Machine::AbstractHeap ArrayStorage_publicLength;
    Machine::AbstractHeap ArrayStorage_vectorLength;
    Machine::AbstractHeap JSString_value;
    Machine::AbstractHeap JSWrapperObject_internalValue;

    const Machine::IndexedAbstractHeap* forIndexingType(IndexingType) const;

    const Machine::AbstractHeap& entire(Typed::AbstractHeap heap) const LIFETIME_BOUND { return m_entireHeaps[static_cast<size_t>(heap)]; }
    // The bucket of the location where it has one, else the entire heap.
    const Machine::AbstractHeap& at(const Typed::Program&, const Typed::MemoryLocation&) const LIFETIME_BOUND;

    // For a mask that names some buckets of a heap: those buckets, and every field heap that lies below the heap beside its buckets. And each heap that several heaps of the IR own, where the masks name one of the owners.
    void appendHeaps(const Typed::LocationMasks&, HeapList&) const;
    void appendHeapsWrittenByCollection(HeapList&) const;
    static bool mayConvertCells(const Typed::Effects& effectsOfCall);
    static bool mayReplaceStorageOfArrays(const Typed::Effects& effectsOfCall);
    enum class ConvertedCells : uint8_t { AllButArrays, All };
    void appendHeapsOfConvertedCells(HeapList&, ConvertedCells = ConvertedCells::All) const;

    void decoratePatchpointRead(std::span<const Machine::AbstractHeap* const> heaps, Machine::PatchOperation* patch) const { patch->setReads(heaps); }
    void decoratePatchpointWrite(std::span<const Machine::AbstractHeap* const> heaps, Machine::PatchOperation* patch) const { patch->setWrites(heaps); }
    void decoratePatchpointRead(const Machine::AbstractHeap* heap, Machine::PatchOperation* patch) const { patch->setReads(std::span<const Machine::AbstractHeap* const>(&heap, 1)); }
    void decoratePatchpointWrite(const Machine::AbstractHeap* heap, Machine::PatchOperation* patch) const { patch->setWrites(std::span<const Machine::AbstractHeap* const>(&heap, 1)); }

private:
    static constexpr unsigned numberOfBuckets = 64;
    using Buckets = std::array<Machine::AbstractHeap, numberOfBuckets>;

    Machine::AbstractHeap addHeap(const Machine::AbstractHeap& parent, ASCIILiteral name, ptrdiff_t offset, Traits::Set);
    const Machine::AbstractHeap& bucketOf(Typed::AbstractHeap heap, unsigned bit) const LIFETIME_BOUND { return m_buckets[Typed::maskIndexOf(heap)][bit]; }

    Machine::HeapTree m_tree;
    std::array<Machine::AbstractHeap, Typed::numberOfHeaps> m_entireHeaps;
    std::array<Buckets, Typed::numberOfIndexedHeaps> m_buckets;
    // By the heap of the IR: the members of this struct whose parent is that heap itself.
    std::array<Vector<const Machine::AbstractHeap*>, Typed::numberOfHeaps> m_fieldHeapsBelow;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
