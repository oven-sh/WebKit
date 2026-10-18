/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTTypedCompactTable.h"
#include "AOTTypedData.h"
#include "AOTTypedObject.h"
#include "CellContainerInlines.h"
#include "Collector.h"
#include "DateInstance.h"
#include "GCSegmentedArrayInlines.h"
#include "GetterSetter.h"
#include "HeapCellInlines.h"
#include "JSCellButterfly.h"
#include "JSCellInlines.h"
#include "JSFunctionWithFields.h"
#include "JSMap.h"
#include "JSPromise.h"
#include "JSPromiseReaction.h"
#include "JSSet.h"
#include "MarkedBlockCage.h"
#include "SlotVisitorInlines.h"
#include "VM.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class CompiledLayoutVisitor {
    static NEVER_INLINE void appendOutOfLine(SlotVisitor& visitor, JSCell* cell) { visitor.appendUnbarriered(cell); }
    static NEVER_INLINE void appendHiddenOutOfLine(SlotVisitor& visitor, JSCell* cell) { visitor.appendHiddenUnbarriered(cell); }
    static NEVER_INLINE void visitThroughMethodTable(SlotVisitor& visitor, Structure* structure, const JSCell* cell) { structure->classInfoForCells()->methodTable.visitChildren(const_cast<JSCell*>(cell), visitor); }
    static NEVER_INLINE void visitOutOfLine(SlotVisitor& visitor, const JSCell* cell)
    {
        switch (cell->type()) {
        case SlimClosureType:
        case StructureType:
        case GetterSetterType:
        case JSCellButterflyType:
        case JSFunctionType:
        case JSPromiseType:
        case JSFullPromiseReactionType:
        case JSSlimPromiseReactionType:
        case JSDateType:
            return visitOutsideLoop(visitor, cell);
        default:
            return visitor.visitChildrenOfBlackCell(cell);
        }
    }
    static NEVER_INLINE void expand(MarkStackArray& stack) { stack.expand(); }

    class Appender {
    public:
        ALWAYS_INLINE Appender(SlotVisitor& visitor)
            : m_visitor(visitor)
            , m_stack(visitor.m_collectorStack)
            , m_markingVersion(visitor.m_markingVersion)
            , m_markingStamp(Structure::markingStampFor(visitor.m_markingVersion))
        {
            loadTop();
        }

        ALWAYS_INLINE ~Appender() { storeTop(); }

        uint8_t markingStamp() const { return m_markingStamp; }

        ALWAYS_INLINE void appendCell(JSCell* cell)
        {
            using Word = uint64_t;
            MarkedBlock& block = cell->markedBlock();
            HeapVersion version = block.markingVersion();
            uintptr_t dependency = 0;
#if CPU(ARM64)
            __asm__ volatile("eor %w[dependency], %w[version], %w[version]" : [dependency] "=r"(dependency), [version] "+r"(version));
#else
            WTF::loadLoadFence();
#endif
            if (version != m_markingVersion) [[unlikely]] {
                storeTop();
                appendOutOfLine(m_visitor, cell);
                loadTop();
                return;
            }
            uintptr_t atom = std::bit_cast<uintptr_t>(cell) / MarkedBlock::atomSize;
            auto* word = std::bit_cast<Atomic<Word>*>(std::bit_cast<uintptr_t>(block.marks().storage().data() + atom / 64 % (MarkedBlock::atomsPerBlock / 64)) + dependency);
            if (word->loadRelaxed() >> (atom % 64) & 1)
                return;
            Word mask = static_cast<Word>(1) << (atom % 64);
            if (word->exchangeOr(mask, std::memory_order_relaxed) & mask) [[unlikely]]
                return;
            cell->setCellState(CellState::PossiblyGrey);
            block.noteMarked();
            ++m_visitor.m_visitCount;
            m_visitor.m_bytesMarkedInHeap += block.cellSize();
            if (m_nextSlot == m_endOfSlots) [[unlikely]] {
                storeTop();
                expand(m_stack);
                loadTop();
            }
            *m_nextSlot++ = cell;
        }

        ALWAYS_INLINE void appendValue(JSValue value)
        {
            if ((std::rotr(static_cast<uint64_t>(JSValue::encode(value)), 3) - 1) >> 45)
                return;
            if (value.asCell()->isPreciseAllocation()) [[unlikely]] {
                storeTop();
                appendHiddenOutOfLine(m_visitor, value.asCell());
                loadTop();
            } else
                appendCell(value.asCell());
        }

        ALWAYS_INLINE void appendStructure(Structure* structure)
        {
            uint8_t& stampAndFlags = structure->markingStampAndFlags();
            if (stampAndFlags != m_markingStamp) [[unlikely]] {
                appendCell(structure);
                stampAndFlags = (stampAndFlags & ~Structure::markingStampMask) | m_markingStamp;
            }
        }

    private:
        ALWAYS_INLINE void storeTop()
        {
            m_stack.m_top = m_nextSlot - m_firstSlot;
#if ASSERT_ENABLED
            m_stack.m_segments.head()->m_top = m_nextSlot - m_firstSlot;
#endif
        }

        ALWAYS_INLINE void loadTop()
        {
            m_firstSlot = m_stack.m_segments.head()->data();
            m_nextSlot = m_firstSlot + m_stack.m_top;
            m_endOfSlots = m_firstSlot + MarkStackArray::s_segmentCapacity;
        }

        SlotVisitor& m_visitor;
        MarkStackArray& m_stack;
        const JSCell** m_firstSlot;
        const JSCell** m_nextSlot;
        const JSCell** m_endOfSlots;
        const HeapVersion m_markingVersion;
        const uint8_t m_markingStamp;
    };

    static NEVER_INLINE void visitOutsideLoop(SlotVisitor& visitor, const JSCell* cell)
    {
        if (!tryVisitOutsideLoop(visitor, cell)) [[unlikely]]
            visitor.visitChildrenOfBlackCell(cell);
    }

    static ALWAYS_INLINE bool tryAppendObjectWithoutElements(SlotVisitor& visitor, Appender& appender, const JSCell* cell, StructureID structureID, Structure* structure)
    {
        auto* object = static_cast<JSObjectWithButterfly*>(const_cast<JSCell*>(cell));
        PropertyOffset maxOffset = structure->maxOffset();
        IndexingType indexingMode;
        Dependency indexingModeDependency = structure->fencedIndexingMode(indexingMode);
        if (indexingMode != NonArray)
            return false;
        Butterfly* butterfly;
        Dependency butterflyDependency = indexingModeDependency.consume(object)->fencedButterfly(butterfly);
        if (butterflyDependency.consume(cell)->structureID() != structureID || butterflyDependency.consume(structure)->maxOffset() != maxOffset)
            return false;
        appender.appendStructure(structure);
        if (!butterfly)
            return true;
        visitor.markAuxiliary(butterfly->base(0, Structure::outOfLineCapacity(maxOffset)));
        const WriteBarrierBase<Unknown>* endOfProperties = butterfly->propertyStorage();
        for (const WriteBarrierBase<Unknown>* property = endOfProperties - Structure::outOfLineSize(maxOffset); property != endOfProperties; ++property)
            appender.appendValue(property->get());
        return true;
    }

    static ALWAYS_INLINE bool tryVisitOutsideLoop(SlotVisitor& visitor, const JSCell* cell)
    {
        using namespace AOT::Typed;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(cell);
        switch (cell->type()) {
        case SlimClosureType: {
            static_assert(SlimClosure::offsetOfCaptures() == sizeof(SlimClosure));
            StructureID structureID = cell->structureID();
            if (structureID.isNuked())
                return false;
            WTF::loadLoadFence();
            Structure* structure = structureID.decode();
            if (structure->typeInfo().type() != SlimClosureType)
                return false;
            uint64_t word = *reinterpret_cast<const uint64_t*>(bytes + SlimClosure::offsetOfWord());
            WTF::loadLoadFence();
            if (cell->structureID() != structureID)
                return false;
            Appender appender(visitor);
            appender.appendStructure(structure);
            if (!word)
                return true;
            AOT::Image* image = AOT::Image::withCode();
            const Data* data = image->at<Data>(image->header().typedDataOffset);
            const EncodedJSValue* capture = reinterpret_cast<const EncodedJSValue*>(bytes + SlimClosure::offsetOfCaptures());
            for (const EncodedJSValue* end = capture + data->at<std::array<uint8_t, 2>>(data->closureWordCountsOffset)[AOT::ClosureWord::functionOf(word)][0]; capture != end; ++capture)
                appender.appendValue(JSValue::decode(*capture));
            return true;
        }
        case StructureType: {
            auto* structure = static_cast<Structure*>(const_cast<JSCell*>(cell));
            if (structure->variant() != Structure::StructureVariant::Normal || structure->existingAOTFields().aotInstance) [[unlikely]]
                return false;
            Appender appender(visitor);
            appender.appendStructure(cell->structure());
            ConcurrentJSLocker locker(structure->m_lock);
            appender.appendValue(std::bit_cast<JSCell*>(structure->m_realm.get()));
            structure->markingStampAndFlags() = (structure->markingStampAndFlags() & ~Structure::markingStampMask) | appender.markingStamp();
            if (!structure->isObject())
                structure->m_cachedPrototypeChain.clear();
            else {
                appender.appendValue(structure->m_prototype.get());
                appender.appendValue(std::bit_cast<JSCell*>(structure->m_cachedPrototypeChain.get()));
            }
            appender.appendValue(structure->m_previousOrRareData.get());
            if (structure->isPinnedPropertyTable() || structure->protectPropertyTableWhileTransitioning())
                appender.appendValue(std::bit_cast<JSCell*>(structure->m_propertyTableUnsafe.get()));
            else if (structure->m_propertyTableUnsafe)
                structure->m_propertyTableUnsafe.clear();
            if (visitor.collectionScope() != CollectionScope::Full)
                appender.appendValue(structure->m_transitionTable.trySingleTransition());
            return true;
        }
        case GetterSetterType: {
            Appender appender(visitor);
            appender.appendStructure(cell->structure());
            appender.appendValue(*reinterpret_cast<JSCell* const*>(bytes + GetterSetter::offsetOfGetter()));
            appender.appendValue(*reinterpret_cast<JSCell* const*>(bytes + GetterSetter::offsetOfSetter()));
            return true;
        }
        case JSFunctionType:
        case JSPromiseType:
        case JSDateType: {
            StructureID structureID = cell->structureID();
            if (structureID.isNuked())
                return false;
            Structure* structure = structureID.decode();
            const ClassInfo* classInfo = structure->classInfoForCells();
            std::array<EncodedJSValue, 4> references;
            unsigned count = 0;
            uint64_t closureWord = 0;
            auto addValueAt = [&](ptrdiff_t offset) ALWAYS_INLINE_LAMBDA { references[count++] = *reinterpret_cast<const EncodedJSValue*>(bytes + offset); };
            if (classInfo == JSPromise::info()) {
                references[count++] = JSValue::encode(static_cast<const JSPromise*>(cell)->payloadCell());
                addValueAt(JSPromise::offsetOfSlot());
            } else if (classInfo == JSFunction::info() || classInfo == JSFunctionWithFields::info() || classInfo == Closure::info()) {
                addValueAt(JSCallee::offsetOfScopeChain());
                references[count++] = *reinterpret_cast<const EncodedJSValue*>(bytes + JSFunction::offsetOfExecutableOrRareData()) & ~static_cast<EncodedJSValue>(JSFunction::rareDataTag);
                if (classInfo == JSFunctionWithFields::info()) {
                    static_assert(JSFunctionWithFields::numberOfInternalFields == 2);
                    addValueAt(JSFunctionWithFields::offsetOfInternalFields());
                    addValueAt(JSFunctionWithFields::offsetOfInternalFields() + sizeof(EncodedJSValue));
                } else if (classInfo == Closure::info())
                    closureWord = static_cast<const Closure*>(cell)->entry();
            } else if (classInfo != DateInstance::info())
                return false;
            Appender appender(visitor);
            if (!tryAppendObjectWithoutElements(visitor, appender, cell, structureID, structure))
                return false;
            for (unsigned i = 0; i < count; ++i)
                appender.appendValue(JSValue::decode(references[i]));
            if (!closureWord)
                return true;
            AOT::Image* image = AOT::Image::withCode();
            const Data* data = image->at<Data>(image->header().typedDataOffset);
            const EncodedJSValue* capture = reinterpret_cast<const EncodedJSValue*>(bytes + Closure::offsetOfCaptures());
            for (const EncodedJSValue* end = capture + data->at<std::array<uint8_t, 2>>(data->closureWordCountsOffset)[AOT::ClosureWord::functionOf(closureWord)][0]; capture != end; ++capture)
                appender.appendValue(JSValue::decode(*capture));
            return true;
        }
        case JSFullPromiseReactionType: {
            auto* reaction = static_cast<const JSFullPromiseReaction*>(cell);
            Appender appender(visitor);
            appender.appendStructure(cell->structure());
            appender.appendValue(reaction->promise());
            appender.appendValue(reaction->next());
            appender.appendValue(reaction->onFulfilled());
            appender.appendValue(reaction->onRejected());
            appender.appendValue(reaction->context());
            return true;
        }
        case JSSlimPromiseReactionType: {
            auto* reaction = static_cast<const JSSlimPromiseReaction*>(cell);
            Appender appender(visitor);
            appender.appendStructure(cell->structure());
            appender.appendValue(reaction->promise());
            appender.appendValue(reaction->next());
            appender.appendValue(reaction->handlerOrContext());
            return true;
        }
        case JSCellButterflyType: {
            Appender appender(visitor);
            appender.appendStructure(cell->structure());
            if (!hasContiguous(cell->indexingType()))
                return true;
            Butterfly* butterfly = static_cast<const JSCellButterfly*>(cell)->toButterfly();
            const WriteBarrier<Unknown>* elements = butterfly->contiguous().data();
            for (unsigned i = 0, length = butterfly->publicLength(); i < length; ++i)
                appender.appendValue(elements[i].get());
            return true;
        }
        default:
            return false;
        }
    }

public:
    template<bool drainsCollectorStack, bool mutatorIsStopped>
    static NEVER_INLINE void drain(SlotVisitor& visitor, MarkStackArray& stack, unsigned countdown)
    {
        using namespace AOT::Typed;
        using Word = uint64_t;
        const uintptr_t structureBase = structureIDBase();
        const uintptr_t cageBase = MarkedBlockCage::base();
        const HeapVersion markingVersion = visitor.m_markingVersion;
        visitor.noteBytesMarked(visitor.collector().heap(), 0);
        MarkStackArray& collectorStack = visitor.m_collectorStack;
        const JSCell** firstSlot = collectorStack.m_segments.head()->data();
        const JSCell** nextSlot = firstSlot + collectorStack.m_top;
        const JSCell** endOfSlots = firstSlot + MarkStackArray::s_segmentCapacity;
#if CPU(X86_64)
        size_t& visitCount = visitor.m_visitCount;
        size_t& bytesVisited = visitor.m_bytesMarkedInHeap;
        size_t& nonCellVisitCount = visitor.m_nonCellVisitCount;
#else
        size_t visitCount = 0;
        size_t bytesVisited = 0;
        size_t nonCellVisitCount = 0;
#endif
        const uint8_t markingStamp = Structure::markingStampFor(markingVersion);

        auto storeTop = [&] ALWAYS_INLINE_LAMBDA {
            collectorStack.m_top = nextSlot - firstSlot;
#if ASSERT_ENABLED
            collectorStack.m_segments.head()->m_top = nextSlot - firstSlot;
#endif
        };

        auto loadTop = [&] ALWAYS_INLINE_LAMBDA {
            firstSlot = collectorStack.m_segments.head()->data();
            nextSlot = firstSlot + collectorStack.m_top;
            endOfSlots = firstSlot + MarkStackArray::s_segmentCapacity;
        };

        auto appendCell = [&](JSCell* cell) ALWAYS_INLINE_LAMBDA {
            MarkedBlock& block = cell->markedBlock();
            HeapVersion version = block.markingVersion();
            uintptr_t dependency = 0;
#if CPU(ARM64)
            __asm__ volatile("eor %w[dependency], %w[version], %w[version]" : [dependency] "=r"(dependency), [version] "+r"(version));
#else
            WTF::loadLoadFence();
#endif
            if (version != markingVersion) [[unlikely]] {
                storeTop();
                appendOutOfLine(visitor, cell);
                loadTop();
                return;
            }
            static_assert(std::is_same_v<std::remove_cvref_t<decltype(block.marks().storage()[0])>, Word>);
            static_assert(MarkedBlock::atomSize == 16 && MarkedBlock::atomsPerBlock % 64 == 0);
            uintptr_t atom = std::bit_cast<uintptr_t>(cell) / MarkedBlock::atomSize;
            auto* word = std::bit_cast<Atomic<Word>*>(std::bit_cast<uintptr_t>(block.marks().storage().data() + atom / 64 % (MarkedBlock::atomsPerBlock / 64)) + dependency);
            if (word->loadRelaxed() >> (atom % 64) & 1)
                return;
            Word mask = static_cast<Word>(1) << (atom % 64);
            if (word->exchangeOr(mask, std::memory_order_relaxed) & mask) [[unlikely]]
                return;
            cell->setCellState(CellState::PossiblyGrey);
            block.noteMarked();
            ++visitCount;
            bytesVisited += block.cellSize();
            if (nextSlot == endOfSlots) [[unlikely]] {
                storeTop();
                expand(collectorStack);
                loadTop();
            }
            *nextSlot++ = cell;
        };

        auto appendValue = [&](JSValue value) ALWAYS_INLINE_LAMBDA {
            static_assert(!(JSValue::NotCellMask & 0x0001fffffffffff8ull) && (JSValue::NotCellMask & 7) && !JSValue::ValueEmpty);
            if ((std::rotr(static_cast<uint64_t>(JSValue::encode(value)), 3) - 1) >> 45)
                return;
            if (value.asCell()->isPreciseAllocation()) [[unlikely]] {
                storeTop();
                appendHiddenOutOfLine(visitor, value.asCell());
                loadTop();
            } else
                appendCell(value.asCell());
        };

        auto appendNarrowReference = [&](uint32_t word) ALWAYS_INLINE_LAMBDA {
            if (word >= minCell32)
                appendCell(std::bit_cast<JSCell*>(cageBase + (static_cast<uintptr_t>(word) << MarkedBlockCage::shift)));
        };

        auto appendStructure = [&](Structure* structure) ALWAYS_INLINE_LAMBDA -> bool {
            uint8_t& stampAndFlags = structure->markingStampAndFlags();
            if (stampAndFlags != markingStamp) [[unlikely]] {
                if (stampAndFlags & Structure::isStructureWithExtraProperties)
                    return false;
                if ((stampAndFlags & Structure::markingStampMask) != markingStamp) {
                    appendCell(structure);
                    stampAndFlags = (stampAndFlags & ~Structure::markingStampMask) | markingStamp;
                }
            }
            return true;
        };

        auto markAuxiliary = [&](HeapCell* cell) ALWAYS_INLINE_LAMBDA {
            if (cell->isPreciseAllocation()) [[unlikely]] {
                visitor.markAuxiliary(cell);
                return;
            }
            MarkedBlock& block = cell->markedBlock();
            Dependency dependency = block.aboutToMark(markingVersion, cell);
            if (block.testAndSetMarked(cell, dependency))
                return;
            block.noteMarked();
            ++visitCount;
            size_t size = block.cellSize();
            bytesVisited += size;
            nonCellVisitCount += size;
        };

        auto visitExtension = [&](uint32_t extensionWord, const uint32_t* offset) ALWAYS_INLINE_LAMBDA {
            const uint8_t* extension = std::bit_cast<const uint8_t*>(cageBase + (static_cast<uintptr_t>(extensionWord) << MarkedBlockCage::shift));
            markAuxiliary(std::bit_cast<HeapCell*>(extension));
            uint32_t size = *reinterpret_cast<const uint32_t*>(extension + offsetOfExtensionSize);
            uint32_t numberOfBoxed = offset[0];
            uint32_t numberOfNarrow = offset[1];
            offset += 2;
            const uint32_t* endOfBoxed = offset + numberOfBoxed;
            for (; offset != endOfBoxed && *offset + sizeof(EncodedJSValue) <= size; ++offset)
                appendValue(JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(extension + *offset)));
            offset = endOfBoxed;
            for (const uint32_t* end = offset + numberOfNarrow; offset != end && *offset + sizeof(uint32_t) <= size; ++offset)
                appendNarrowReference(*reinterpret_cast<const uint32_t*>(extension + *offset));
        };

        auto tryVisitArray = [&](const JSCell* cell) ALWAYS_INLINE_LAMBDA -> bool {
            auto* array = static_cast<JSObjectWithButterfly*>(const_cast<JSCell*>(cell));
            StructureID structureID = cell->structureID();
            if (structureID.isNuked())
                return false;
            Structure* structure = std::bit_cast<Structure*>(structureBase + structureID.bits());
            PropertyOffset maxOffset = structure->maxOffset();
            IndexingType indexingMode;
            Dependency indexingModeDependency = structure->fencedIndexingMode(indexingMode);
            if (indexingMode != ArrayWithContiguous && indexingMode != ArrayWithInt32 && indexingMode != ArrayWithDouble && indexingMode != ArrayWithUndecided)
                return false;
            Butterfly* butterfly;
            Dependency butterflyDependency = indexingModeDependency.consume(array)->fencedButterfly(butterfly);
            if (butterfly) {
                if (butterflyDependency.consume(cell)->structureID() != structureID || butterflyDependency.consume(structure)->maxOffset() != maxOffset)
                    return false;
            }
            appendStructure(structure);
            if (!butterfly)
                return true;
            markAuxiliary(static_cast<HeapCell*>(butterfly->base(0, Structure::outOfLineCapacity(maxOffset))));
            const WriteBarrierBase<Unknown>* endOfProperties = butterfly->propertyStorage();
            for (const WriteBarrierBase<Unknown>* property = endOfProperties - Structure::outOfLineSize(maxOffset); property != endOfProperties; ++property)
                appendValue(property->get());
            if (indexingMode != ArrayWithContiguous)
                return true;
            const WriteBarrier<Unknown>* elements = butterfly->contiguous().data();
            for (unsigned i = 0, length = butterfly->publicLength(); i < length; ++i)
                appendValue(elements[i].get());
            return true;
        };

        auto tryVisitString = [&](const JSCell* cell, Structure* structure) ALWAYS_INLINE_LAMBDA -> bool {
            appendStructure(structure);
            uintptr_t pointer = static_cast<const JSString*>(cell)->fiberConcurrently();
            if (pointer & JSString::isRopeInPointer) {
                auto& fibers = *static_cast<const JSRopeString*>(cell);
                if (pointer & JSRopeString::isSubstringInPointer) {
                    appendCell(fibers.fiber1());
                    return true;
                }
                JSString* fiber = std::bit_cast<JSString*>(pointer & JSRopeString::stringMask);
                if (!fiber)
                    return true;
                appendCell(fiber);
                fiber = fibers.fiber1();
                if (!fiber)
                    return true;
                appendCell(fiber);
                fiber = fibers.fiber2();
                if (fiber)
                    appendCell(fiber);
                return true;
            }
            if (StringImpl* impl = std::bit_cast<StringImpl*>(pointer)) {
                if constexpr (drainsCollectorStack) {
                    if (!AOT::Image::isBackedByStaticData(*impl)) [[likely]]
                        visitor.reportLeafCellExtraMemoryVisited(cell->vm().heap, impl->costDuringGC());
                }
            }
            return true;
        };

        auto tryVisitObjectWithoutElements = [&](const JSCell* cell, bool hasInlineProperties) ALWAYS_INLINE_LAMBDA -> bool {
            auto* object = static_cast<JSObjectWithButterfly*>(const_cast<JSCell*>(cell));
            StructureID structureID = cell->structureID();
            if (structureID.isNuked())
                return false;
            Structure* structure = std::bit_cast<Structure*>(structureBase + structureID.bits());
            PropertyOffset maxOffset = structure->maxOffset();
            IndexingType indexingMode;
            Dependency indexingModeDependency = structure->fencedIndexingMode(indexingMode);
            if (indexingMode != NonArray)
                return false;
            Butterfly* butterfly;
            Dependency butterflyDependency = indexingModeDependency.consume(object)->fencedButterfly(butterfly);
            if (butterflyDependency.consume(cell)->structureID() != structureID || butterflyDependency.consume(structure)->maxOffset() != maxOffset)
                return false;
            appendStructure(structure);
            if (butterfly) {
                markAuxiliary(static_cast<HeapCell*>(butterfly->base(0, Structure::outOfLineCapacity(maxOffset))));
                const WriteBarrierBase<Unknown>* endOfProperties = butterfly->propertyStorage();
                for (const WriteBarrierBase<Unknown>* property = endOfProperties - Structure::outOfLineSize(maxOffset); property != endOfProperties; ++property)
                    appendValue(property->get());
            }
            if (hasInlineProperties) {
                const WriteBarrierBase<Unknown>* property = object->inlineStorageUnsafe();
                for (const WriteBarrierBase<Unknown>* end = property + std::min<unsigned>(maxOffset + 1, structure->inlineCapacity()); property != end; ++property)
                    appendValue(property->get());
            }
            return true;
        };

        auto tryVisitMapOrSet = [&](const JSCell* cell) ALWAYS_INLINE_LAMBDA -> bool {
            if (!tryVisitObjectWithoutElements(cell, false))
                return false;
            static_assert(sizeof(JSMap) == sizeof(JSSet));
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(cell);
            appendValue(*reinterpret_cast<JSCell* const*>(bytes + JSMap::offsetOfStorage()));
            appendValue(*reinterpret_cast<JSCell* const*>(bytes + JSMap::offsetOfCompactTable()));
            return true;
        };

        auto tryVisitCompactTable = [&](const JSCell* cell, Structure* structure) ALWAYS_INLINE_LAMBDA -> bool {
            using AOT::Typed::CompactTable;
            auto* table = static_cast<const CompactTable*>(cell);
            Locker locker { const_cast<JSCell*>(cell)->cellLock() };
            if (!table->isLive() || table->keyWidth() != CompactTable::Width::Narrow || table->valueWidth() == CompactTable::Width::Wide || table->isInImage())
                return false;
            appendStructure(structure);
            std::span keys = table->narrowKeys();
            WTF::loadLoadFence();
            if (table->valueWidth() == CompactTable::Width::None) {
                for (uint32_t key : keys)
                    appendNarrowReference(key);
                return true;
            }
            const uint32_t* values = keys.data() + table->capacity();
            for (size_t entry = 0; entry < keys.size(); ++entry) {
                appendNarrowReference(keys[entry]);
                appendNarrowReference(values[entry]);
            }
            return true;
        };

        auto tryVisit = [&](const JSCell* cell) ALWAYS_INLINE_LAMBDA -> bool {
            Structure* structure = std::bit_cast<Structure*>(structureBase + cell->structureID().decontaminate().bits());
            auto& aotFields = structure->existingAOTFields();
            if (!aotFields.isCompiledCellStructure()) {
                JSType type = cell->type();
                if (type == ArrayType)
                    return tryVisitArray(cell);
                if (type == StringType)
                    return tryVisitString(cell, structure);
                if (type == FinalObjectType)
                    return tryVisitObjectWithoutElements(cell, true);
                if (type == JSMapType || type == JSSetType)
                    return tryVisitMapOrSet(cell);
                if (type == CellType && structure->classInfoForCells() == AOT::Typed::CompactTable::info())
                    return tryVisitCompactTable(cell, structure);
                return false;
            }
            const LayoutData* layout = aotFields.compiledLayout();
            if (!layout)
                layout = &environmentLayout(*aotFields.compiledEnvironmentData(), structure->firstCompiledEnvironmentShape(), cell);
            const uint8_t* fields = reinterpret_cast<const uint8_t*>(cell);
            if (!appendStructure(structure)) [[unlikely]] {
                uint32_t offsetOfExtraProperties = layout->offsetOfExtraProperties();
                if (!offsetOfExtraProperties) {
                    storeTop();
                    visitThroughMethodTable(visitor, structure, cell);
                    loadTop();
                    return true;
                }
                appendCell(structure);
                appendNarrowReference(*reinterpret_cast<const uint32_t*>(fields + offsetOfExtraProperties));
            }
            if (!layout->referencesOffset)
                return true;
            const uint32_t* references = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(layout) + layout->referencesOffset);
            const uint32_t* offset = references + 3;
            for (const uint32_t* end = offset + references[0] + references[1]; offset != end; ++offset)
                appendValue(JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(fields + *offset)));
            for (const uint32_t* end = offset + references[2]; offset != end; ++offset)
                appendNarrowReference(*reinterpret_cast<const uint32_t*>(fields + *offset));
            if (layout->hasExtension) {
                if (uint32_t extensionWord = *reinterpret_cast<const uint32_t*>(fields + offsetOfExtension))
                    visitExtension(extensionWord, offset);
            }
            return true;
        };

#if CPU(X86_64)
        for (; countdown; --countdown) {
            const JSCell* cell;
            if constexpr (drainsCollectorStack) {
                if (nextSlot == firstSlot)
                    break;
                cell = *--nextSlot;
            } else {
                if (!stack.canRemoveLast())
                    break;
                cell = stack.removeLast();
            }
            cell->setCellState(CellState::PossiblyBlack);
            if constexpr (!mutatorIsStopped)
                WTF::storeLoadFence();
            if (tryVisit(cell))
                continue;
            storeTop();
            visitOutOfLine(visitor, cell);
            loadTop();
        }
#else
        auto popAndPrefetch = [&] ALWAYS_INLINE_LAMBDA -> const JSCell* {
            if (!countdown)
                return nullptr;
            if constexpr (drainsCollectorStack) {
                if (nextSlot == firstSlot)
                    return nullptr;
                --countdown;
                const JSCell* cell = *--nextSlot;
                __builtin_prefetch(cell);
                return cell;
            } else {
                if (!stack.canRemoveLast())
                    return nullptr;
                --countdown;
                return stack.popAndPrefetch();
            }
        };

        for (const JSCell* next = popAndPrefetch(); next;) {
            const JSCell* cell = next;
            next = popAndPrefetch();
            cell->setCellState(CellState::PossiblyBlack);
            if constexpr (!mutatorIsStopped)
                WTF::storeLoadFence();
            if (tryVisit(cell))
                continue;
            storeTop();
            visitOutOfLine(visitor, cell);
            loadTop();
        }
#endif

        storeTop();
#if !CPU(X86_64)
        visitor.m_visitCount += visitCount;
        visitor.m_bytesMarkedInHeap += bytesVisited;
        visitor.m_nonCellVisitCount += nonCellVisitCount;
#endif
    }
};

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
