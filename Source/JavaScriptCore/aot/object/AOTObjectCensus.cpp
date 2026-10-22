/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObjectCensus.h"

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTEnvironment.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionNames.h"
#include "AOTFunctionTables.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTCompactTable.h"
#include "AOTObjectVisiting.h"
#include "AOTPendingStars.h"
#include "AOTRuntimeValidation.h"
#include "AOTSlimClosure.h"
#include "AOTSlotAccess.h"
#include "AOTStructureCensus.h"
#include "AOTTableCensus.h"
#include "AOTVMState.h"
#include "BlockDirectoryInlines.h"
#include "ButterflyInlines.h"
#include "DeferGCInlines.h"
#include "HeapCellInlines.h"
#include "HeapIterationScope.h"
#include "JSCInlines.h"
#include "JSMap.h"
#include "JSSet.h"
#include "MarkedBlockInlines.h"
#include "MarkedSpaceInlines.h"
#include "Options.h"
#include "PreventCollectionScope.h"
#include "SubspaceInlines.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/DataLog.h>
#include <wtf/FilePrintStream.h>
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/HexNumber.h>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr unsigned orderWordFlagsShift = std::countr_zero(orderWordHasExtraProperties);

static String asWord(StringView name)
{
    if (name.isEmpty())
        return "-"_str;
    StringBuilder builder;
    for (char16_t unit : name.codeUnits()) {
        if (unit == ' ')
            builder.append('_');
        else if (unit > ' ' && unit != 0x7f && unit != '\\')
            builder.append(unit);
        else
            builder.append("\\u"_s, hex(unit, 4, Lowercase));
    }
    return builder.toString();
}

static String nameOrDash(StringID string)
{
    const ImageText& text = Image::current()->text();
    if (!string || !text.hasText(string))
        return "-"_str;
    return asWord(text.view(string));
}

static String nameAsWord(const LayoutData& layout)
{
    return asWord(nameOf(programTables(), layout));
}

static String nameAsWord(const EnvironmentData& environment)
{
    return nameOrDash(StringID(environment.name));
}

static String nameAsWord(const FieldData& field)
{
    return nameOrDash(Image::current()->text().identifier(field.name).text());
}

namespace {

class ObjectCensus {
    WTF_MAKE_NONCOPYABLE(ObjectCensus);
public:
    explicit ObjectCensus(VM&);

    void collect();
    void print(PrintStream&) const;

private:
    enum class LiveCellKind : uint32_t { Layout = 1, Environment, Closure, Extension, ExtraProperties, ExtraPropertiesStorage, Class, ClassStorage, OtherAuxiliary };
    enum class ClosureClass : uint8_t { Full, Slim, Reified };
    struct FreeCells {
        uint64_t cells { 0 };
        uint64_t blocks { 0 };
        uint64_t emptyBlocks { 0 };
    };
    struct Strings {
        UncheckedKeyHashSet<const StringImpl*> impls;
        uint64_t strings { 0 };
        uint64_t bytes { 0 };
        uint64_t ropes { 0 };
        uint64_t staticStrings { 0 };
        uint64_t externalStrings { 0 };
        uint64_t substrings { 0 };
    };
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
        uint64_t outside { 0 };
        uint64_t deferred { 0 };
        uint64_t withoutStorage { 0 };
        uint64_t absent { 0 };
        uint64_t undefined { 0 };
        uint64_t values { 0 };
    };
    struct KeySet {
        const Object* example { nullptr };
        uint64_t objects { 0 };
    };
    // What the objects of a layout hold beside their cells.
    struct Beside {
        uint64_t extensions { 0 };
        uint64_t extensionBytes { 0 };
        uint64_t extras { 0 };
        uint64_t extrasWithoutProperties { 0 };
        uint64_t extrasBytes { 0 };
        uint64_t extrasStorages { 0 };
        uint64_t extrasStorageBytes { 0 };
    };
    using CountsByPair = UncheckedKeyHashMap<std::pair<uint32_t, uint32_t>, uint64_t>;
    using KeySetsByState = UncheckedKeyHashMap<std::tuple<uintptr_t, uint32_t, uintptr_t>, KeySet>;
    using CountsBySize = UncheckedKeyHashMap<size_t, uint64_t>;
    using ClosureCounts = std::array<uint64_t, 3>;
    static constexpr size_t indexOf(ClosureClass closureClass) { return static_cast<size_t>(closureClass); }
    static constexpr unsigned numberOfBuckets = 50;
    static unsigned bucketOf(uint64_t number) { return number <= 16 ? number : 12 + WTF::fastLog2(number - 1) + 1; }
    static uint64_t boundOf(unsigned bucket) { return bucket <= 16 ? bucket : 1ull << (bucket - 12); }

    template<typename CellFunctor, typename BlockFunctor> void forEachLiveCell(const CellFunctor&, const BlockFunctor&);
    void collectExtraProperties(HeapCell*, HeapCell::Kind);
    void visit(HeapCell*, HeapCell::Kind, size_t cellSize);
    void count(LiveCellKind, const void* descriptor, size_t cellSize);
    void countObject(const LayoutData&, const Object&, size_t cellSize);
    void countEnvironment(const Environment&, size_t cellSize);
    void countClosure(JSCell*, ClosureClass, size_t cellSize);
    void countOrdinaryCell(JSCell*, size_t cellSize);
    void countFields(const LayoutData&, const Object&);
    void countFieldStates(const LayoutData&, const Object&);
    void countKeySet(const LayoutData&, const Object&);
    void countBeside(const LayoutData&, const Object&);
    void countFieldsOutsideSlots();
    void countStorage(LiveCellKind, const void* descriptor, JSObject*);
    void countString(JSString*);
    void countFreeCells(MarkedBlock::Handle&, uint64_t liveCellsInBlock);
    template<typename CellType> static void countReferenceSlots(std::span<const uint32_t> references, const CellType&, Slots&);
    static std::pair<uint64_t, uint64_t> arraySizes(JSArray*);
    static std::optional<ClosureClass> closureClassOf(JSType);
    static bool isInitializedClosure(JSCell*, ClosureClass);

    void printCells(PrintStream&) const;
    void printClosures(PrintStream&) const;
    void printOrderStates(PrintStream&) const;
    void printKeySets(PrintStream&) const;
    static String keysOf(const Object&);
    void printLayouts(PrintStream&) const;
    void printExtensions(PrintStream&) const;
    void printBeside(PrintStream&) const;
    void printEnvironments(PrintStream&) const;
    void printContainers(PrintStream&) const;
    void printStrings(PrintStream&) const;
    void printFreeCells(PrintStream&) const;
    template<typename Record> static void printSlots(PrintStream&, const Record&, const String& name, const Slots&);
    static Vector<std::tuple<uint32_t, uint32_t, uint64_t>> sorted(const CountsByPair&);

    VM& m_vm;
    UncheckedKeyHashMap<std::tuple<uint32_t, uintptr_t, size_t>, uint64_t> m_cells;
    UncheckedKeyHashMap<std::pair<Subspace*, size_t>, FreeCells> m_freeCells;
    UncheckedKeyHashSet<JSCell*> m_extraPropertyObjects;
    Vector<const Object*> m_objectsWithExtraProperties;
    Strings m_strings;
    UncheckedKeyHashMap<const LayoutData*, Slots> m_slots;
    UncheckedKeyHashMap<const EnvironmentData*, Slots> m_environmentSlots;
    CountsByPair m_orderStates;
    KeySetsByState m_keySets;
    UncheckedKeyHashMap<String, KeySet> m_unorderedKeySets;
    UncheckedKeyHashMap<const LayoutData*, Vector<Field>> m_fields;
    UncheckedKeyHashMap<std::tuple<uintptr_t, uint32_t, uint32_t>, uint64_t> m_extensions;
    UncheckedKeyHashMap<const LayoutData*, Beside> m_beside;
    UncheckedKeyHashMap<FunctionID, ClosureCounts> m_closuresByFunction;
    std::array<std::array<uint64_t, numberOfBuckets>, numberOfBuckets> m_arrays { };
    std::array<std::pair<uint64_t, uint64_t>, 4> m_arrayForms { };
    UncheckedKeyHashMap<std::tuple<uint32_t, uint32_t, uint32_t>, uint64_t> m_arraysThatLeftTheirRoom;
    std::array<uint64_t, numberOfBuckets> m_maps { };
    std::array<uint64_t, numberOfBuckets> m_sets { };
    void findHoldersOfFirstTables();

    struct FirstTable {
        CompactTable* table { nullptr };
        uint64_t lengthOfChain { 0 };
        Vector<String> holders;
    };
    Vector<FirstTable> m_firstTables;
    Vector<CompactTable*> m_obsoleteTables;
    UncheckedKeyHashSet<JSCell*> m_successorsOfTables;
    UncheckedKeyHashMap<std::pair<uint32_t, uint32_t>, uint64_t> m_tables;
    UncheckedKeyHashMap<JSCell*, uint32_t> m_fieldReferencesByArray;
    uint64_t m_singlyReferencedArrays { 0 };
    uint64_t m_multiplyReferencedArrays { 0 };
    CountsBySize m_auxiliaryCells;
    CountsBySize m_ownedAuxiliaryCells;
};

ObjectCensus::ObjectCensus(VM& vm)
    : m_vm(vm)
{
}

template<typename CellFunctor, typename BlockFunctor>
void ObjectCensus::forEachLiveCell(const CellFunctor& visitCell, const BlockFunctor& didVisitBlock)
{
    MarkedSpace& space = m_vm.heap.objectSpace();
    space.forEachBlock([&](MarkedBlock::Handle* block) {
        size_t cellSize = block->cellSize();
        uint64_t liveCellsInBlock = 0;
        block->forEachLiveCell([&](size_t, HeapCell* cell, HeapCell::Kind kind) {
            ++liveCellsInBlock;
            visitCell(cell, kind, cellSize);
            return IterationStatus::Continue;
        });
        didVisitBlock(*block, liveCellsInBlock);
    });
    for (PreciseAllocation* allocation : space.preciseAllocations()) {
        if (allocation->isLive())
            visitCell(allocation->cell(), allocation->attributes().cellKind, allocation->cellSize());
    }
}

void ObjectCensus::collect()
{
    PreventCollectionScope preventCollection(m_vm.heap);
    DeferGCForAWhile deferGC(m_vm);
    {
        HeapIterationScope iterationScope(m_vm.heap);
        forEachLiveCell([&](HeapCell* cell, HeapCell::Kind kind, size_t) {
            collectExtraProperties(cell, kind);
        }, [](MarkedBlock::Handle&, uint64_t) { });
        forEachLiveCell([&](HeapCell* cell, HeapCell::Kind kind, size_t cellSize) {
            visit(cell, kind, cellSize);
        }, [&](MarkedBlock::Handle& block, uint64_t liveCellsInBlock) {
            countFreeCells(block, liveCellsInBlock);
        });
    }
    findHoldersOfFirstTables();
    // A property lookup can allocate the property table of a Structure, and nothing may allocate during the walk.
    countFieldsOutsideSlots();
    for (auto& entry : m_auxiliaryCells) {
        uint64_t owned = m_ownedAuxiliaryCells.get(entry.key);
        if (entry.value > owned)
            m_cells.add({ static_cast<uint32_t>(LiveCellKind::OtherAuxiliary), 0, entry.key }, entry.value - owned);
    }
    for (auto& entry : m_fieldReferencesByArray)
        ++(entry.value == 1 ? m_singlyReferencedArrays : m_multiplyReferencedArrays);
}

// An obsolete table names the table that took its place, so whoever holds the first of a chain holds them all. The longest chains are listed with the cells that have a word, wide or narrow,
// that may name the first table. A chain without such a cell is held from outside the heap: by the stack, a register or a table of the run time.
void ObjectCensus::findHoldersOfFirstTables()
{
    constexpr size_t maxListedChains = 8;
    for (CompactTable* table : m_obsoleteTables) {
        if (m_successorsOfTables.contains(table))
            continue;
        FirstTable first { table, 0, { } };
        for (JSCell* next = table; next->classInfo() == CompactTable::info() && !uncheckedDowncast<CompactTable>(next)->isLive(); next = uncheckedDowncast<CompactTable>(next)->successorOfObsoleteTable())
            ++first.lengthOfChain;
        m_firstTables.append(WTF::move(first));
    }
    std::ranges::sort(m_firstTables, std::ranges::greater { }, &FirstTable::lengthOfChain);
    size_t numberOfListedChains = std::min(m_firstTables.size(), maxListedChains);
    if (!numberOfListedChains)
        return;

    HeapIterationScope iterationScope(m_vm.heap);
    forEachLiveCell([&](HeapCell* heapCell, HeapCell::Kind kind, size_t cellSize) {
        if (isJSCellKind(kind) && heapCell->isZapped())
            return;
        const uint32_t* words = reinterpret_cast<const uint32_t*>(heapCell);
        for (size_t index = 0; index < cellSize / sizeof(uint32_t); ++index) {
            for (size_t chain = 0; chain < numberOfListedChains; ++chain) {
                FirstTable& first = m_firstTables[chain];
                bool isNamedWide = !(index % 2) && *reinterpret_cast<const uint64_t*>(words + index) == std::bit_cast<uint64_t>(first.table);
                bool isNamedNarrow = words[index] == MarkedBlockCage::encode(cageBaseOf(first.table), first.table);
                if ((!isNamedWide && !isNamedNarrow) || heapCell == static_cast<HeapCell*>(first.table))
                    continue;
                String holder = "auxiliary"_s;
                if (isJSCellKind(kind)) {
                    JSCell* cell = static_cast<JSCell*>(heapCell);
                    if (const LayoutData* layout = isCompiledObject(cell) ? compiledLayoutOrNull(cell->structure()) : nullptr)
                        holder = makeString("layout:"_s, nameAsWord(*layout));
                    else if (cell->type() == CompiledEnvironmentType)
                        holder = makeString("environment:"_s, nameAsWord(uncheckedDowncast<Environment>(cell)->layout()));
                    else
                        holder = makeString("class:"_s, cell->classInfo()->className);
                }
                first.holders.append(makeString(holder, '+', index * sizeof(uint32_t), isNamedWide ? ":wide"_s : ":narrow"_s));
            }
        }
    }, [](MarkedBlock::Handle&, uint64_t) { });
}

void ObjectCensus::collectExtraProperties(HeapCell* heapCell, HeapCell::Kind kind)
{
    if (!isJSCellKind(kind) || heapCell->isZapped())
        return;
    JSCell* cell = static_cast<JSCell*>(heapCell);
    if (!isCompiledObject(cell))
        return;
    const Object* object = uncheckedDowncast<Object>(cell);
    JSObject* extraProperties = extraPropertiesIfMade(*object);
    if (!extraProperties)
        return;
    m_extraPropertyObjects.add(extraProperties);
    m_objectsWithExtraProperties.append(object);
}

void ObjectCensus::visit(HeapCell* heapCell, HeapCell::Kind kind, size_t cellSize)
{
    if (!isJSCellKind(kind)) {
        ++m_auxiliaryCells.add(cellSize, 0).iterator->value;
        return;
    }
    if (heapCell->isZapped()) {
        count(LiveCellKind::Class, nullptr, cellSize);
        return;
    }
    JSCell* cell = static_cast<JSCell*>(heapCell);
    if (const LayoutData* layout = isCompiledObject(cell) ? compiledLayoutOrNull(cell->structure()) : nullptr)
        countObject(*layout, *uncheckedDowncast<Object>(cell), cellSize);
    else if (cell->type() == CompiledEnvironmentType)
        countEnvironment(*uncheckedDowncast<Environment>(cell), cellSize);
    else if (m_extraPropertyObjects.contains(cell)) {
        count(LiveCellKind::ExtraProperties, nullptr, cellSize);
        countStorage(LiveCellKind::ExtraPropertiesStorage, nullptr, asObject(cell));
    } else if (std::optional<ClosureClass> closureClass = closureClassOf(cell->type()))
        countClosure(cell, *closureClass, cellSize);
    else
        countOrdinaryCell(cell, cellSize);
}

void ObjectCensus::count(LiveCellKind kind, const void* descriptor, size_t cellSize)
{
    ++m_cells.add({ static_cast<uint32_t>(kind), std::bit_cast<uintptr_t>(descriptor), cellSize }, 0).iterator->value;
}

void ObjectCensus::countObject(const LayoutData& layout, const Object& object, size_t cellSize)
{
    count(LiveCellKind::Layout, &layout, cellSize);
    ++m_orderStates.add({ layout.shape, object.orderWord() }, 0).iterator->value;
    auto& slots = m_slots.add(&layout, Slots { }).iterator->value;
    ++slots.objects;
    slots.bytes += cellSize;
    countReferenceSlots(referenceTableOf(layout), object, slots);
    countFields(layout, object);
    countKeySet(layout, object);
    if (Extension* extension = object.extension()) {
        size_t extensionCellSize = reinterpret_cast<HeapCell*>(extension)->cellSize();
        count(LiveCellKind::Extension, nullptr, extensionCellSize);
        ++m_ownedAuxiliaryCells.add(extensionCellSize, 0).iterator->value;
    }
    countFieldStates(layout, object);
    countBeside(layout, object);
}

void ObjectCensus::countBeside(const LayoutData& layout, const Object& object)
{
    Extension* extension = object.extension();
    JSObject* extraProperties = extraPropertiesIfMade(object);
    if (!extension && !extraProperties)
        return;
    Beside& beside = m_beside.add(&layout, Beside { }).iterator->value;
    if (extension) {
        ++beside.extensions;
        beside.extensionBytes += reinterpret_cast<HeapCell*>(extension)->cellSize();
    }
    if (!extraProperties)
        return;
    ++beside.extras;
    beside.extrasBytes += extraProperties->cellSize();
    Structure* structure = extraProperties->structure();
    beside.extrasWithoutProperties += structure->maxOffset() == invalidOffset;
    Butterfly* butterfly = extraProperties->butterfly();
    if (!butterfly || isCopyOnWrite(structure->indexingMode()))
        return;
    ++beside.extrasStorages;
    beside.extrasStorageBytes += static_cast<HeapCell*>(butterfly->base(structure))->cellSize();
}

void ObjectCensus::countFieldStates(const LayoutData& layout, const Object& object)
{
    uint64_t bitsOfFields = 0;
    uint64_t bitsWithValue = 0;
    auto& fields = m_fields.find(&layout)->value;
    for (const FieldData& field : layout.fields()) {
        auto& counts = fields[layout.indexOf(field)];
        const void* slot = object.slotOf(field);
        if (!slot) {
            ++counts.withoutStorage;
            continue;
        }
        uint64_t bit = field.isOutOfLine() ? granuleBitOf(field.slot.offset) : 0;
        bitsOfFields |= bit;
        SlotRepresentation representation = field.slot.representation();
        SlotTraits traits = traitsOf(representation);
        uint64_t bits = readSlotBits(representation, slot);
        if (traits.hasAbsent && bits == traits.absentBits) {
            ++counts.absent;
            continue;
        }
        if (traits.hasUndefined && bits == traits.undefinedBits) {
            ++counts.undefined;
            continue;
        }
        ++counts.values;
        bitsWithValue |= bit;
    }
    Extension* extension = object.extension();
    if (!extension)
        return;
    bitsWithValue |= extension->granuleBits() & ~bitsOfFields;
    ++m_extensions.add({ std::bit_cast<uintptr_t>(&layout), static_cast<uint32_t>(std::popcount(extension->granuleBits())), static_cast<uint32_t>(std::popcount(bitsWithValue)) }, 0).iterator->value;
}

void ObjectCensus::countEnvironment(const Environment& environment, size_t cellSize)
{
    const EnvironmentData& layout = environment.layout();
    count(LiveCellKind::Environment, &layout, cellSize);
    auto& slots = m_environmentSlots.add(&layout, Slots { }).iterator->value;
    ++slots.objects;
    slots.bytes += cellSize;
    countReferenceSlots(referenceTableOf(layout), environment, slots);
}

void ObjectCensus::countClosure(JSCell* cell, ClosureClass closureClass, size_t cellSize)
{
    count(LiveCellKind::Closure, Closure::info(), cellSize);
    if (closureClass != ClosureClass::Slim)
        countStorage(LiveCellKind::ClassStorage, cell->classInfo(), asObject(cell));
    if (!isInitializedClosure(cell, closureClass))
        return;
    auto& counts = m_closuresByFunction.add(*functionOf(cell), ClosureCounts { }).iterator->value;
    ++counts[indexOf(closureClass)];
}

void ObjectCensus::countOrdinaryCell(JSCell* cell, size_t cellSize)
{
    const ClassInfo* classInfo = cell->classInfo();
    count(LiveCellKind::Class, classInfo, cellSize);
    if (classInfo == JSArray::info()) {
        auto [length, capacity] = arraySizes(uncheckedDowncast<JSArray>(cell));
        ++m_arrays[bucketOf(length)][bucketOf(capacity)];
        // 0: the cell has no room for storage. 1: the storage lies in the cell. 2: the array has left the room in its cell for other storage.
        Butterfly* butterfly = uncheckedDowncast<JSArray>(cell)->butterfly();
        bool isInOwnCell = static_cast<size_t>(std::bit_cast<char*>(butterfly) - std::bit_cast<char*>(cell)) < cellSize;
        unsigned form = cellSize == sizeof(JSArray) ? 0 : isInOwnCell ? 1 : cell->structure()->outOfLineCapacity() ? 3 : 2;
        if (form >= 2)
            ++m_arraysThatLeftTheirRoom.add({ static_cast<uint32_t>(form), static_cast<uint32_t>(cellSize), static_cast<uint32_t>(std::min<uint64_t>(length, 40)) }, 0).iterator->value;
        ++m_arrayForms[form].first;
        m_arrayForms[form].second += cellSize;
    } else if (classInfo == JSMap::info())
        ++m_maps[bucketOf(uncheckedDowncast<JSMap>(cell)->size())];
    else if (classInfo == JSSet::info())
        ++m_sets[bucketOf(uncheckedDowncast<JSSet>(cell)->size())];
    else if (classInfo == CompactTable::info()) {
        auto* table = uncheckedDowncast<CompactTable>(cell);
        // 0: obsolete. 1: without an entry, nothing was ever added. 2: without an entry, all were deleted. 3: with entries.
        uint32_t state = !table->isLive() ? 0 : table->size() ? 3 : table->used() ? 2 : 1;
        ++m_tables.add({ state << 2 | static_cast<uint32_t>(table->status()), static_cast<uint32_t>(cellSize) }, 0).iterator->value;
        if (!table->isLive()) {
            m_obsoleteTables.append(table);
            m_successorsOfTables.add(table->successorOfObsoleteTable());
        }
    }
    if (cell->isObject())
        countStorage(LiveCellKind::ClassStorage, classInfo, asObject(cell));
    else if (cell->isString())
        countString(static_cast<JSString*>(cell));
}

template<typename CellType>
void ObjectCensus::countReferenceSlots(std::span<const uint32_t> references, const CellType& cell, Slots& slots)
{
    if (references.empty())
        return;
    const uint32_t* offset = references.data() + numberOfInlineReferenceCounts;
    auto containsCell = [&](uint32_t slotOffset) {
        JSValue value = JSValue::decode(cell.template at<EncodedJSValue>(slotOffset));
        return value && value.isCell();
    };
    for (uint32_t i = 0; i < references[0]; ++i)
        slots.filledBoxed += containsCell(*offset++);
    for (uint32_t i = 0; i < references[1]; ++i)
        slots.filledWide += containsCell(*offset++);
    for (uint32_t i = 0; i < references[2]; ++i)
        slots.filledNarrow += cell.template at<uint32_t>(*offset++) >= minCell32;
}

void ObjectCensus::countFields(const LayoutData& layout, const Object& object)
{
    auto& fields = m_fields.add(&layout, Vector<Field>()).iterator->value;
    if (fields.isEmpty())
        fields.grow(layout.numberOfFields);
    for (const FieldData& field : layout.fields()) {
        SlotTraits traits = traitsOf(field.slot.representation());
        if (!traits.holdsCell)
            continue;
        const void* slot = object.slotOf(field);
        if (!slot)
            continue;
        auto& counts = fields[layout.indexOf(field)];
        JSCell* valueCell = cellInSlot(field.slot.representation(), slot, cageBaseOf(&object));
        if (!valueCell) {
            counts.deferred += traits.valueKind == SlotValueKind::DeferredClosure && !slotHoldsAbsent(field.slot.representation(), slot);
            continue;
        }
        ++counts.filled;
        const ClassInfo* classInfo = valueCell->classInfo();
        if (classInfo == JSArray::info()) {
            auto [length, capacity] = arraySizes(uncheckedDowncast<JSArray>(valueCell));
            ++counts.arrays;
            counts.emptyArrays += !length;
            counts.elements += length;
            counts.capacity += capacity;
            ++m_fieldReferencesByArray.add(valueCell, 0).iterator->value;
        } else if (classInfo == JSMap::info()) {
            ++counts.maps;
            counts.emptyMaps += !uncheckedDowncast<JSMap>(valueCell)->size();
        }
    }
}

// The Structure, the order word and the Structure of the extra properties name the keys of an ordered object. An unordered object has what its slots hold.
void ObjectCensus::countKeySet(const LayoutData& layout, const Object& object)
{
    JSObject* extraProperties = extraPropertiesIfMade(object);
    uintptr_t extraPropertiesStructure = extraProperties ? std::bit_cast<uintptr_t>(extraProperties->structure()) : 0;
    KeySet* keySet;
    if (!layout.keysAreNeverEnumerated)
        keySet = &m_keySets.add({ std::bit_cast<uintptr_t>(object.structure()), object.orderWord() & orderAndCountMask, extraPropertiesStructure }, KeySet { }).iterator->value;
    else {
        StringBuilder filledSlots;
        filledSlots.append(layout.shape, ' ', hex(extraPropertiesStructure));
        for (const FieldData& field : layout.fields()) {
            if (object.isInSlot(field))
                filledSlots.append(' ', layout.indexOf(field));
        }
        keySet = &m_unorderedKeySets.add(filledSlots.toString(), KeySet { }).iterator->value;
    }
    keySet->example = &object;
    ++keySet->objects;
}

void ObjectCensus::countFieldsOutsideSlots()
{
    for (const Object* object : m_objectsWithExtraProperties) {
        const LayoutData& layout = object->layout();
        auto& fields = m_fields.find(&layout)->value;
        for (const FieldData& field : layout.fields()) {
            if (!object->isInSlot(field) && livesInExtraProperties(m_vm, *object, field))
                ++fields[layout.indexOf(field)].outside;
        }
    }
}

void ObjectCensus::countStorage(LiveCellKind kind, const void* descriptor, JSObject* object)
{
    Structure* structure = object->structure();
    Butterfly* butterfly = object->butterfly();
    if (!butterfly || isCopyOnWrite(structure->indexingMode()))
        return;
    void* base = butterfly->base(structure);
    if (static_cast<size_t>(static_cast<char*>(base) - std::bit_cast<char*>(object)) < object->cellSize())
        return;
    size_t cellSize = static_cast<HeapCell*>(base)->cellSize();
    count(kind, descriptor, cellSize);
    ++m_ownedAuxiliaryCells.add(cellSize, 0).iterator->value;
}

void ObjectCensus::countString(JSString* string)
{
    if (string->isRope()) {
        ++m_strings.ropes;
        return;
    }
    const StringImpl* impl = string->tryGetValueImpl();
    if (!impl || !m_strings.impls.add(impl).isNewEntry)
        return;
    ++m_strings.strings;
    if (impl->isStatic())
        ++m_strings.staticStrings;
    else if (impl->isExternal()) {
        ++m_strings.externalStrings;
        m_strings.bytes += sizeof(StringImpl);
    } else if (impl->isSubString()) {
        ++m_strings.substrings;
        m_strings.bytes += sizeof(StringImpl);
    } else
        m_strings.bytes += impl->sizeInBytes();
}

void ObjectCensus::countFreeCells(MarkedBlock::Handle& block, uint64_t liveCellsInBlock)
{
    auto& freeCells = m_freeCells.add({ block.subspace(), block.cellSize() }, FreeCells { }).iterator->value;
    freeCells.cells += block.cellsPerBlock() - liveCellsInBlock;
    ++freeCells.blocks;
    freeCells.emptyBlocks += !liveCellsInBlock;
}

std::pair<uint64_t, uint64_t> ObjectCensus::arraySizes(JSArray* array)
{
    Butterfly* butterfly = array->butterfly();
    return { array->length(), butterfly ? butterfly->vectorLength() : 0 };
}

std::optional<ObjectCensus::ClosureClass> ObjectCensus::closureClassOf(JSType type)
{
    if (type == CompiledClosureType)
        return ClosureClass::Full;
    if (type == SlimClosureType)
        return ClosureClass::Slim;
    if (type == SlimClosureWithPropertiesType)
        return ClosureClass::Reified;
    return std::nullopt;
}

bool ObjectCensus::isInitializedClosure(JSCell* cell, ClosureClass closureClass)
{
    switch (closureClass) {
    case ClosureClass::Full:
        return !!uncheckedDowncast<Closure>(cell)->entry();
    case ClosureClass::Slim:
        return !!*reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(cell) + SlimClosure::offsetOfWord());
    case ClosureClass::Reified:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ObjectCensus::print(PrintStream& out) const
{
    printCells(out);
    printClosures(out);
    printOrderStates(out);
    printKeySets(out);
    printLayouts(out);
    printExtensions(out);
    printBeside(out);
    printEnvironments(out);
    printContainers(out);
    printStrings(out);
    printFreeCells(out);
}

void ObjectCensus::printCells(PrintStream& out) const
{
    Vector<std::tuple<uint32_t, uint32_t, String, size_t, uint64_t>> lines;
    for (auto& entry : m_cells) {
        auto [kind, descriptor, cellSize] = entry.key;
        auto className = [&] {
            return descriptor ? String(std::bit_cast<const ClassInfo*>(descriptor)->className) : "-"_str;
        };
        uint32_t index = 0;
        String start;
        switch (static_cast<LiveCellKind>(kind)) {
        case LiveCellKind::Layout: {
            const LayoutData& layout = *std::bit_cast<const LayoutData*>(descriptor);
            index = layout.index;
            start = makeString("layout "_s, layout.index, ' ', nameAsWord(layout));
            break;
        }
        case LiveCellKind::Environment: {
            const EnvironmentData& environment = *std::bit_cast<const EnvironmentData*>(descriptor);
            index = environment.index;
            start = makeString("environment "_s, environment.index, ' ', nameAsWord(environment));
            break;
        }
        case LiveCellKind::Closure:
            start = "closure"_s;
            break;
        case LiveCellKind::Extension:
            start = "extension"_s;
            break;
        case LiveCellKind::ExtraProperties:
            start = "extras"_s;
            break;
        case LiveCellKind::ExtraPropertiesStorage:
            start = "extras-storage"_s;
            break;
        case LiveCellKind::Class:
            start = makeString("class "_s, className());
            break;
        case LiveCellKind::ClassStorage:
            start = makeString("auxiliary-of "_s, className());
            break;
        case LiveCellKind::OtherAuxiliary:
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
        if (std::get<3>(a) != std::get<3>(b))
            return std::get<3>(a) < std::get<3>(b);
        return std::get<4>(a) < std::get<4>(b);
    });
    for (auto& [kind, index, start, cellSize, objects] : lines)
        out.println("AOT live: ", start, " cell ", cellSize, " objects ", objects);
}

static ASCIILiteral callableKindName(FunctionKindData kind)
{
    switch (kind.accessor()) {
    case MethodKind::Getter:
        return "getter"_s;
    case MethodKind::Setter:
        return "setter"_s;
    case MethodKind::Method:
        break;
    }
    switch (kind.callable()) {
    case CallableKind::Plain:
        return "plain"_s;
    case CallableKind::Arrow:
        return "arrow"_s;
    case CallableKind::Method:
        return "method"_s;
    case CallableKind::Bound:
        return "bound"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ObjectCensus::printClosures(PrintStream& out) const
{
    Vector<std::pair<FunctionID, ClosureCounts>> functions;
    for (auto& entry : m_closuresByFunction)
        functions.append({ entry.key, entry.value });
    std::ranges::sort(functions);
    for (auto& [function, counts] : functions) {
        FunctionKindData kind = programTables().kindOfFunction(function.index());
        ASCIILiteral role = kind.closureStructure == classObjectClosureStructure ? "class"_s : kind.isConstructor ? "constructor"_s : "not-constructor"_s;
        out.println("AOT live: closures-of ", function.index(), " ", asWord(writtenFunctionName(function)), " full ", counts[indexOf(ClosureClass::Full)], " slim ", counts[indexOf(ClosureClass::Slim)], " reified ", counts[indexOf(ClosureClass::Reified)], " role ", role, " in ", kind.isInRuntimeModule ? "runtime"_s : "program"_s,
            " kind ", callableKindName(kind), " captures ", captureCountsOf(function).captures);
    }
}

Vector<std::tuple<uint32_t, uint32_t, uint64_t>> ObjectCensus::sorted(const CountsByPair& counts)
{
    Vector<std::tuple<uint32_t, uint32_t, uint64_t>> result;
    for (auto& entry : counts)
        result.append({ entry.key.first, entry.key.second, entry.value });
    std::ranges::sort(result);
    return result;
}

void ObjectCensus::printOrderStates(PrintStream& out) const
{
    const ProgramTables& tables = programTables();
    auto states = sorted(m_orderStates);
    for (size_t i = 0; i < states.size();) {
        uint32_t shape = std::get<0>(states[i]);
        const LayoutData& layout = tables.layoutForShape(shape);
        String name = nameAsWord(layout);
        uint64_t outOfLine = 0;
        uint64_t unordered = 0;
        for (; i < states.size() && std::get<0>(states[i]) == shape; ++i) {
            uint32_t word = std::get<1>(states[i]);
            uint64_t objects = std::get<2>(states[i]);
            if (layout.keysAreNeverEnumerated)
                unordered += objects;
            else if (isOrderStoredOutOfLine(word))
                outOfLine += objects;
            else
                out.println("AOT live: order-state ", layout.index, " ", name, " order ", orderOf(word), " count ", countOf(word), " flags ", word >> orderWordFlagsShift, " objects ", objects);
        }
        if (outOfLine)
            out.println("AOT live: order-state ", layout.index, " ", name, " order out-of-line objects ", outOfLine);
        if (unordered)
            out.println("AOT live: order-state ", layout.index, " ", name, " order unordered objects ", unordered);
    }
}

String ObjectCensus::keysOf(const Object& object)
{
    const LayoutData& layout = object.layout();
    StringBuilder keys;
    auto append = [&](const String& name) {
        keys.append(keys.isEmpty() ? ""_s : ","_s, name);
    };
    if (layout.keysAreNeverEnumerated) {
        for (const FieldData& field : layout.fields()) {
            if (object.isInSlot(field))
                append(nameAsWord(field));
        }
    } else {
        object.forEachFieldInKeyOrder(object.state(), [&](uint16_t index) {
            append(nameAsWord(layout.fields()[index]));
        });
    }
    keys.append(" extra "_s);
    bool isFirst = true;
    for (UniquedStringImpl* name : object.extraKeyNames()) {
        keys.append(isFirst ? ""_s : ","_s, asWord(StringView(name)));
        isFirst = false;
    }
    return keys.toString();
}

void ObjectCensus::printKeySets(PrintStream& out) const
{
    Vector<std::tuple<uint32_t, uint64_t, String>> lines;
    auto add = [&](const KeySet& keySet) {
        const LayoutData& layout = keySet.example->layout();
        lines.append({ layout.index, keySet.objects, makeString(layout.index, ' ', nameAsWord(layout), " objects "_s, keySet.objects, " keys "_s, keysOf(*keySet.example)) });
    };
    for (auto& entry : m_keySets)
        add(entry.value);
    for (auto& entry : m_unorderedKeySets)
        add(entry.value);
    std::ranges::sort(lines, [](auto& a, auto& b) {
        if (std::get<0>(a) != std::get<0>(b))
            return std::get<0>(a) < std::get<0>(b);
        if (std::get<1>(a) != std::get<1>(b))
            return std::get<1>(a) > std::get<1>(b);
        return codePointCompare(std::get<2>(a), std::get<2>(b)) == std::strong_ordering::less;
    });
    for (auto& line : lines)
        out.println("AOT live: key-set ", std::get<2>(line));
}

template<typename Record>
void ObjectCensus::printSlots(PrintStream& out, const Record& record, const String& name, const Slots& slots)
{
    std::array<uint32_t, numberOfInlineReferenceCounts> references { };
    if (std::span table = referenceTableOf(record); !table.empty())
        std::ranges::copy(table.first(numberOfInlineReferenceCounts), references.begin());
    out.println("AOT live: slots ", record.index, " ", name, " size ", record.size, " objects ", slots.objects, " bytes ", slots.bytes, " boxed ", references[0], " wide ", references[1], " narrow ", references[2], " filled-boxed ", slots.filledBoxed, " filled-wide ", slots.filledWide, " filled-narrow ", slots.filledNarrow);
}

void ObjectCensus::printLayouts(PrintStream& out) const
{
    Vector<const LayoutData*> layoutsWithSlots;
    for (auto& entry : m_slots)
        layoutsWithSlots.append(entry.key);
    std::ranges::sort(layoutsWithSlots, [](auto* a, auto* b) {
        return std::tie(a->index, a->shape) < std::tie(b->index, b->shape);
    });
    for (const LayoutData* layout : layoutsWithSlots) {
        String name = nameAsWord(*layout);
        printSlots(out, *layout, name, m_slots.find(layout)->value);
        StringBuilder placed;
        for (const FieldData& field : layout->fields()) {
            if (!field.isOutOfLine())
                placed.append(' ', field.slot.offset, ':', static_cast<unsigned>(field.slot.representation()));
        }
        out.println("AOT live: placed ", layout->index, " ", name, " size ", layout->size, " extension ", layout->hasExtension() ? 1 : 0, " :", placed.toString());
        auto& fields = m_fields.find(layout)->value;
        for (const FieldData& field : layout->fields()) {
            auto& counts = fields[layout->indexOf(field)];
            if (!traitsOf(field.slot.representation()).holdsCell && !counts.outside)
                continue;
            out.println("AOT live: field ", layout->index, " ", name, " ", nameAsWord(field), field.isOutOfLine() ? " out-of-line" : " in-line", " store ", static_cast<unsigned>(field.slot.representation()), " filled ", counts.filled, " arrays ", counts.arrays, " empty ", counts.emptyArrays, " elements ", counts.elements, " capacity ", counts.capacity, " maps ", counts.maps, " empty-maps ", counts.emptyMaps, " outside ", counts.outside, " deferred ", counts.deferred);
        }
        for (const FieldData& field : layout->fields()) {
            auto& counts = fields[layout->indexOf(field)];
            out.println("AOT live: states ", layout->index, " ", name, " ", nameAsWord(field), field.isOutOfLine() ? " out-of-line" : " in-line", " offset ", field.slot.offset, " width ", widthOf(field.slot.representation()), " store ", static_cast<unsigned>(field.slot.representation()), " without-storage ", counts.withoutStorage, " absent ", counts.absent, " undefined ", counts.undefined, " values ", counts.values);
        }
    }
}

void ObjectCensus::printExtensions(PrintStream& out) const
{
    Vector<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, const LayoutData*, uint64_t>> lines;
    for (auto& entry : m_extensions) {
        auto [descriptor, granulesHeld, granulesWithValue] = entry.key;
        const LayoutData* layout = std::bit_cast<const LayoutData*>(descriptor);
        lines.append({ layout->index, layout->shape, granulesHeld, granulesWithValue, layout, entry.value });
    }
    std::ranges::sort(lines);
    for (auto& [index, shape, granulesHeld, granulesWithValue, layout, objects] : lines)
        out.println("AOT live: granules ", index, " ", nameAsWord(*layout), " held ", granulesHeld, " with-value ", granulesWithValue, " objects ", objects);
}

void ObjectCensus::printBeside(PrintStream& out) const
{
    Vector<const LayoutData*> layouts;
    for (auto& entry : m_beside)
        layouts.append(entry.key);
    std::ranges::sort(layouts, [](auto* a, auto* b) {
        return std::tie(a->index, a->shape) < std::tie(b->index, b->shape);
    });
    for (const LayoutData* layout : layouts) {
        const Beside& beside = m_beside.find(layout)->value;
        out.println("AOT live: beside ", layout->index, " ", nameAsWord(*layout), " extensions ", beside.extensions, " bytes ", beside.extensionBytes, " extras ", beside.extras, " bytes ", beside.extrasBytes, " without-properties ", beside.extrasWithoutProperties, " extras-storages ", beside.extrasStorages, " bytes ", beside.extrasStorageBytes);
    }
}

void ObjectCensus::printEnvironments(PrintStream& out) const
{
    Vector<const EnvironmentData*> environments;
    for (auto& entry : m_environmentSlots)
        environments.append(entry.key);
    std::ranges::sort(environments, [](auto* a, auto* b) {
        return std::tie(a->index, a->shape) < std::tie(b->index, b->shape);
    });
    for (const EnvironmentData* environment : environments)
        printSlots(out, *environment, nameAsWord(*environment), m_environmentSlots.find(environment)->value);
}

void ObjectCensus::printContainers(PrintStream& out) const
{
    for (unsigned length = 0; length < numberOfBuckets; ++length) {
        for (unsigned capacity = 0; capacity < numberOfBuckets; ++capacity) {
            if (uint64_t arrays = m_arrays[length][capacity])
                out.println("AOT live: array length ", boundOf(length), " capacity ", boundOf(capacity), " arrays ", arrays);
        }
    }
    out.println("AOT live: array-cells without-room ", m_arrayForms[0].first, " bytes ", m_arrayForms[0].second, " storage-in-cell ", m_arrayForms[1].first, " bytes ", m_arrayForms[1].second, " room-left-behind-by-growth ", m_arrayForms[2].first, " bytes ", m_arrayForms[2].second, " room-left-behind-for-named-properties ", m_arrayForms[3].first, " bytes ", m_arrayForms[3].second);
    for (auto& entry : m_arraysThatLeftTheirRoom)
        out.println("AOT live: array-left-its-room ", std::get<0>(entry.key) == 2 ? "by-growth" : "for-named-properties", " cell ", std::get<1>(entry.key), " length ", std::get<2>(entry.key), " arrays ", entry.value);
    out.println("AOT live: arrays-held once ", m_singlyReferencedArrays, " more ", m_multiplyReferencedArrays);
    for (Instance* instance : m_vm.aotState().instances) {
        if (JSCell* list = instance->realm.emptyArgumentList)
            out.println("AOT live: empty-argument-list length ", uncheckedDowncast<JSArray>(list)->length(), " pristine ", list->isPristine() ? 1 : 0);
    }
    for (unsigned size = 0; size < numberOfBuckets; ++size) {
        if (m_maps[size])
            out.println("AOT live: map size ", boundOf(size), " objects ", m_maps[size]);
    }
    for (unsigned size = 0; size < numberOfBuckets; ++size) {
        if (m_sets[size])
            out.println("AOT live: set size ", boundOf(size), " objects ", m_sets[size]);
    }
    static constexpr ASCIILiteral statesOfTables[] = { "obsolete"_s, "never-filled"_s, "emptied"_s, "with-entries"_s };
    for (auto& entry : m_tables)
        out.println("AOT live: compact-table ", statesOfTables[entry.key.first >> 2], " status ", entry.key.first & 3, " cell ", entry.key.second, " objects ", entry.value);
    out.println("AOT live: compact-table chains ", m_firstTables.size(), " obsolete ", m_obsoleteTables.size());
    for (const FirstTable& first : m_firstTables.span().first(std::min<size_t>(m_firstTables.size(), 8))) {
        out.print("AOT live: compact-table chain of ", first.lengthOfChain, " status ", static_cast<unsigned>(first.table->status()), " held-by");
        for (const String& holder : first.holders)
            out.print(" ", holder);
        out.println(first.holders.isEmpty() ? " nothing-in-the-heap" : "");
    }
    out.println("AOT live: extras-entries ", m_extraPropertyObjects.size());
}

void ObjectCensus::printStrings(PrintStream& out) const
{
    out.println("AOT live: strings ", m_strings.strings, " bytes ", m_strings.bytes, " ropes ", m_strings.ropes, " static ", m_strings.staticStrings, " external ", m_strings.externalStrings, " substrings ", m_strings.substrings);
}

void ObjectCensus::printFreeCells(PrintStream& out) const
{
    Vector<std::tuple<size_t, String, FreeCells>> freeCells;
    for (auto& entry : m_freeCells)
        freeCells.append({ entry.key.second, String::fromLatin1(entry.key.first->name().data()), entry.value });
    std::ranges::sort(freeCells, [](auto& a, auto& b) {
        if (std::get<0>(a) != std::get<0>(b))
            return std::get<0>(a) < std::get<0>(b);
        if (auto order = codePointCompare(std::get<1>(a), std::get<1>(b)); order != std::strong_ordering::equal)
            return order == std::strong_ordering::less;
        auto& countsA = std::get<2>(a);
        auto& countsB = std::get<2>(b);
        return std::tie(countsA.cells, countsA.blocks, countsA.emptyBlocks) < std::tie(countsB.cells, countsB.blocks, countsB.emptyBlocks);
    });
    for (auto& [cellSize, subspace, counts] : freeCells)
        out.println("AOT live: free cell ", cellSize, " cells ", counts.cells, " blocks ", counts.blocks, " empty ", counts.emptyBlocks, " subspace ", subspace);
}

} // anonymous namespace

void dumpObjectCensus(VM& vm, PrintStream& out)
{
    if (!vm.hasCompiledCode())
        return;
    RELEASE_ASSERT(!vm.heap.currentThreadIsDoingGCWork());
    ObjectCensus census(vm);
    census.collect();
    census.print(out);
    dumpStructureCensus(vm, out);
}

void validateSlotTypesOfLiveObjects(VM& vm)
{
    RELEASE_ASSERT(!vm.heap.currentThreadIsDoingGCWork());
    PreventCollectionScope preventCollection(vm.heap);
    DeferGCForAWhile deferGC(vm);
    HeapIterationScope iterationScope(vm.heap);
    vm.cellSpace().forEachLiveCell([&](HeapCell* heapCell, HeapCell::Kind) {
        if (heapCell->isZapped())
            return;
        if (Object* object = compiledObjectOrNull(static_cast<JSCell*>(heapCell)))
            validateSlotTypes(vm, *object);
    });
}

void dumpObjectCensusIfRequested(VM& vm)
{
    if (!vm.hasCompiledCode())
        return;
    if (validatesRuntime(vm)) [[unlikely]]
        validateSlotTypesOfLiveObjects(vm);
    if (!Options::dumpAOTObjectCensusAtExit()) [[likely]]
        return;
    // A collection empties caches that name dead cells.
    dumpTableCensus(vm, WTF::dataFile());
    vm.heap.collectNow(Sync, CollectionScope::Full);
    dumpObjectCensus(vm, WTF::dataFile());
}

void answerObjectCensusRequest(VM& vm)
{
    const char* path = byteCast<char>(Options::aotObjectCensusRequestPath());
    if (!path || !vm.hasCompiledCode()) [[likely]]
        return;
    String request = String::fromUTF8(path);
    if (!FileSystem::deleteFile(request))
        return;
    static unsigned numberOfAnswers;
    auto out = FilePrintStream::open(makeString(request, '.', ++numberOfAnswers).utf8(), "w"_s);
    if (!out)
        return;
    dumpTableCensus(vm, *out);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    dumpObjectCensus(vm, *out);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
