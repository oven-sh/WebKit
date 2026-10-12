/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 */

#include "config.h"

#if USE(BUN_JSC_ADDITIONS)

#include "BlockDirectoryInlines.h"
#include "CodeBlock.h"
#include "FunctionExecutable.h"
#include "HeapIterationScope.h"
#include "JSArrayBufferView.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSMap.h"
#include "JSSet.h"
#include "MarkedBlockInlines.h"
#include "MarkedSpaceInlines.h"
#include "NativeExecutable.h"
#include "PreventCollectionScope.h"
#include "VMManager.h"
#include <map>
#include <set>
#include <tuple>
#include <unistd.h>
#include <wtf/FilePrintStream.h>

#if ENABLE(AOT)
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#endif

namespace JSC {

namespace {

// Numbers, and names of the engine's own: classes (ClassInfo), subspaces. Nothing of the program: no string, no name of a property, function or type.
class HeapCensus {
public:
    HeapCensus(VM& vm, PrintStream& out)
        : m_vm(vm)
        , m_out(out)
    {
    }

    void run(bool collectFirst);

private:
    struct Cells {
        uint64_t cells { 0 };
        uint64_t estimatedBytes { 0 };
    };
    struct Directory {
        uint64_t blocks { 0 };
        uint64_t emptyBlocks { 0 };
        uint64_t sparseBlocks { 0 };
        uint64_t liveCells { 0 };
        uint64_t freeCells { 0 };
    };
    struct Strings {
        uint64_t strings { 0 };
        uint64_t bytes { 0 };
    };
#if ENABLE(AOT)
    struct Slots {
        uint64_t objects { 0 };
        uint64_t bytes { 0 };
        uint64_t filledBoxed { 0 };
        uint64_t filledWide { 0 };
        uint64_t filledNarrow { 0 };
    };
    void visitLaidOut(const AOT::Typed::LayoutData&, JSCell*, size_t cellSize);
#endif

    static uint64_t boundOf(uint64_t number)
    {
        if (number <= 16)
            return number;
        uint64_t bound = 32;
        while (bound < number)
            bound <<= 1;
        return bound;
    }

    void visit(HeapCell*, HeapCell::Kind, size_t cellSize);
    void visitString(JSString*);
    void visitStructure(Structure*);
    void visitFunction(JSFunction*);
    void visitStorage(JSObject*, const ClassInfo*);
    void visitSource(const SourceCode&);
    void print(bool collected);

    VM& m_vm;
    PrintStream& m_out;
    // The name of a ClassInfo is what JavaScript sees ("Function" for six classes): the subspace tells them apart.
    std::map<std::tuple<const ClassInfo*, Subspace*, size_t>, Cells> m_classes;
    std::map<std::tuple<const ClassInfo*, Subspace*, size_t>, uint64_t> m_storage;
    std::map<size_t, uint64_t> m_auxiliaryCells;
    std::map<size_t, uint64_t> m_ownedAuxiliaryCells;
    std::map<std::pair<Subspace*, size_t>, Directory> m_directories;
    std::map<Structure*, std::pair<uint64_t, Subspace*>> m_cellsOfStructure;
    Vector<Structure*> m_structures;
    std::set<const StringImpl*> m_stringImpls;
    // rope, substring rope | 8 or 16 bit x atom or not | static, external, substring
    std::map<ASCIILiteral, Strings, bool (*)(ASCIILiteral, ASCIILiteral)> m_strings { [](ASCIILiteral a, ASCIILiteral b) { return strcmp(a.characters(), b.characters()) < 0; } };
    // kind of executable, has rare data, has storage
    std::map<std::tuple<Subspace*, unsigned, bool, bool>, uint64_t> m_functions;
    std::set<ExecutableBase*> m_executablesOfFunctions;
    Vector<ExecutableBase*> m_nativeExecutables;
    // number of bound arguments, subspace of the target
    std::map<std::pair<unsigned, Subspace*>, uint64_t> m_boundFunctions;
    // length, capacity, indexing type
    std::map<std::tuple<uint64_t, uint64_t, unsigned>, uint64_t> m_arrays;
    std::map<uint64_t, uint64_t> m_maps;
    std::map<uint64_t, uint64_t> m_sets;
    // mode of a typed array
    std::map<unsigned, Strings> m_views;
    std::set<SourceProvider*> m_sources;
    uint64_t m_finalObjects { 0 };
    uint64_t m_inlineCapacity { 0 };
    uint64_t m_inlineSize { 0 };
    uint64_t m_outOfLineCapacity { 0 };
    uint64_t m_outOfLineSize { 0 };
    uint64_t m_zapped { 0 };
#if ENABLE(AOT)
    std::map<std::pair<const AOT::Typed::LayoutData*, size_t>, uint64_t> m_layouts;
    std::map<const AOT::Typed::LayoutData*, Slots> m_slots;
#endif
};

void HeapCensus::visitString(JSString* string)
{
    auto add = [&](ASCIILiteral kind, uint64_t bytes) {
        auto& entry = m_strings[kind];
        ++entry.strings;
        entry.bytes += bytes;
    };
    if (string->isRope()) {
        add(string->isSubstring() ? "substring-rope"_s : "rope"_s, 0);
        return;
    }
    const StringImpl* impl = string->tryGetValueImpl();
    if (!impl || !m_stringImpls.insert(impl).second) {
        add("shared"_s, 0);
        return;
    }
    if (impl->isStatic())
        add("static"_s, 0);
    else if (impl->isExternal())
        add(impl->is8Bit() ? "external-8"_s : "external-16"_s, sizeof(StringImpl));
    else if (impl->isSubString())
        add("substring"_s, sizeof(StringImpl));
    else if (impl->isAtom())
        add(impl->is8Bit() ? "atom-8"_s : "atom-16"_s, impl->sizeInBytes());
    else
        add(impl->is8Bit() ? "flat-8"_s : "flat-16"_s, impl->sizeInBytes());
}

void HeapCensus::visitStorage(JSObject* object, const ClassInfo* classInfo)
{
    Structure* structure = object->structure();
    Butterfly* butterfly = object->butterfly();
    if (!butterfly || isCopyOnWrite(structure->indexingMode()))
        return;
    size_t cellSize = static_cast<HeapCell*>(butterfly->base(structure))->cellSize();
    ++m_storage[{ classInfo, object->subspace(), cellSize }];
    ++m_ownedAuxiliaryCells[cellSize];
}

void HeapCensus::visitSource(const SourceCode& source)
{
    if (SourceProvider* provider = source.provider())
        m_sources.insert(provider);
}

void HeapCensus::visitFunction(JSFunction* function)
{
    ExecutableBase* executable = function->executable();
    // 0 none, 1 native, 2 a builtin in JavaScript, 3 of the program
    unsigned kind = 0;
    if (executable) {
        m_executablesOfFunctions.insert(executable);
        if (executable->isHostFunction())
            kind = 1;
        else
            kind = uncheckedDowncast<FunctionExecutable>(executable)->isBuiltinFunction() ? 2 : 3;
    }
    ++m_functions[{ function->subspace(), kind, !!function->rareData(), !!function->butterfly() }];
}

#if ENABLE(AOT)
void HeapCensus::visitLaidOut(const AOT::Typed::LayoutData& layout, JSCell* cell, size_t cellSize)
{
    using namespace AOT::Typed;
    ++m_layouts[{ &layout, cellSize }];
    auto& slots = m_slots[&layout];
    ++slots.objects;
    slots.bytes += cellSize;
    if (!layout.referencesOffset)
        return;
    auto* bytes = reinterpret_cast<const uint8_t*>(cell);
    const uint32_t* references = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(&layout) + layout.referencesOffset);
    const uint32_t* offset = references + 3;
    auto holdsCell = [&](uint32_t offset) {
        JSValue value = JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(bytes + offset));
        return value && value.isCell();
    };
    for (uint32_t i = 0; i < references[0]; ++i)
        slots.filledBoxed += holdsCell(*offset++);
    for (uint32_t i = 0; i < references[1]; ++i)
        slots.filledWide += holdsCell(*offset++);
    for (uint32_t i = 0; i < references[2]; ++i)
        slots.filledNarrow += *reinterpret_cast<const uint32_t*>(bytes + *offset++) > undefinedCell32;
}
#endif

void HeapCensus::visit(HeapCell* heapCell, HeapCell::Kind kind, size_t cellSize)
{
    if (!isJSCellKind(kind)) {
        ++m_auxiliaryCells[cellSize];
        return;
    }
    if (heapCell->isZapped()) {
        ++m_zapped;
        return;
    }
    JSCell* cell = static_cast<JSCell*>(heapCell);
    Structure* structure = cell->structure();
    const ClassInfo* classInfo = structure->classInfoForCells();
    Subspace* subspace = heapCell->subspace();
    auto& cellsOfStructure = m_cellsOfStructure[structure];
    ++cellsOfStructure.first;
    cellsOfStructure.second = subspace;
#if ENABLE(AOT)
    // The second word of such a cell is no butterfly.
    if (const AOT::Typed::LayoutData* layout = structure->compiledLayoutOf(cell)) {
        visitLaidOut(*layout, cell, cellSize);
        return;
    }
#endif
    auto& cells = m_classes[{ classInfo, subspace, cellSize }];
    ++cells.cells;
    if (cell->isString()) {
        visitString(static_cast<JSString*>(cell));
        return;
    }
    if (!cell->isObject()) {
        cells.estimatedBytes += cell->estimatedSizeInBytes(m_vm);
        if (classInfo == Structure::info())
            m_structures.append(static_cast<Structure*>(cell));
        else if (classInfo == NativeExecutable::info())
            m_nativeExecutables.append(static_cast<ExecutableBase*>(cell));
        else if (auto* executable = dynamicDowncast<ScriptExecutable>(cell))
            visitSource(executable->source());
        return;
    }
    JSObject* object = asObject(cell);
#if ENABLE(AOT)
    if (isSlimClosureType(cell->type()))
        return;
#endif
    visitStorage(object, classInfo);
    if (auto* bound = dynamicDowncast<JSBoundFunction>(cell))
        ++m_boundFunctions[{ bound->boundArgsLength(), bound->targetFunction()->subspace() }];
    if (auto* function = dynamicDowncast<JSFunction>(cell)) {
        visitFunction(function);
        return;
    }
    if (classInfo == JSArray::info()) {
        Butterfly* butterfly = object->butterfly();
        ++m_arrays[{ boundOf(uncheckedDowncast<JSArray>(cell)->length()), boundOf(butterfly ? butterfly->vectorLength() : 0), structure->indexingMode() }];
    } else if (classInfo == JSMap::info())
        ++m_maps[boundOf(uncheckedDowncast<JSMap>(cell)->size())];
    else if (classInfo == JSSet::info())
        ++m_sets[boundOf(uncheckedDowncast<JSSet>(cell)->size())];
    else if (classInfo == JSFinalObject::info()) {
        ++m_finalObjects;
        m_inlineCapacity += structure->inlineCapacity();
        m_inlineSize += structure->inlineSize();
        m_outOfLineCapacity += structure->outOfLineCapacity();
        m_outOfLineSize += structure->outOfLineSize();
    } else if (auto* view = dynamicDowncast<JSArrayBufferView>(cell)) {
        auto& entry = m_views[static_cast<unsigned>(view->mode())];
        ++entry.strings;
        entry.bytes += view->byteLength();
    }
}

void HeapCensus::run(bool collectFirst)
{
    Heap& heap = m_vm.heap;
    if (collectFirst)
        heap.collectNow(Sync, CollectionScope::Full);
    PreventCollectionScope preventCollection(heap);
    MarkedSpace& space = heap.objectSpace();
    {
        HeapIterationScope iterationScope(heap);
        space.forEachBlock([&](MarkedBlock::Handle* block) {
            size_t cellSize = block->cellSize();
            uint64_t liveCells = 0;
            block->forEachLiveCell([&](size_t, HeapCell* cell, HeapCell::Kind kind) {
                ++liveCells;
                visit(cell, kind, cellSize);
                return IterationStatus::Continue;
            });
            auto& directory = m_directories[{ block->subspace(), cellSize }];
            ++directory.blocks;
            directory.emptyBlocks += !liveCells;
            directory.sparseBlocks += liveCells && liveCells * 4 < block->cellsPerBlock();
            directory.liveCells += liveCells;
            directory.freeCells += block->cellsPerBlock() - liveCells;
        });
        for (PreciseAllocation* allocation : space.preciseAllocations()) {
            if (allocation->isLive())
                visit(allocation->cell(), allocation->attributes().cellKind, allocation->cellSize());
        }
    }
    print(collectFirst);
}

void HeapCensus::print(bool collected)
{
    Heap& heap = m_vm.heap;
    MarkedSpace& space = heap.objectSpace();
    size_t bytesInPreciseAllocations = 0;
    for (PreciseAllocation* allocation : space.preciseAllocations())
        bytesInPreciseAllocations += allocation->isLive() ? allocation->cellSize() : 0;
    m_out.println("census: heap size ", heap.size(), " capacity ", heap.capacity(), " extra ", heap.extraMemorySize(), " blocks ", space.blocks().set().size(), " precise ", bytesInPreciseAllocations, " collected ", collected ? 1 : 0, " zapped ", m_zapped);
    for (auto& [key, cells] : m_classes) {
        auto [classInfo, subspace, cellSize] = key;
        m_out.println("census: class ", classInfo->className, " cell ", cellSize, " cells ", cells.cells, " estimated ", cells.estimatedBytes, " subspace ", subspace->name());
    }
    for (auto& [key, cells] : m_storage) {
        auto [classInfo, subspace, cellSize] = key;
        m_out.println("census: storage ", classInfo->className, " cell ", cellSize, " cells ", cells, " subspace ", subspace->name());
    }
    for (auto& [cellSize, cells] : m_auxiliaryCells)
        m_out.println("census: auxiliary cell ", cellSize, " cells ", cells, " owned ", m_ownedAuxiliaryCells[cellSize]);
    for (auto& [key, directory] : m_directories)
        m_out.println("census: directory cell ", key.second, " blocks ", directory.blocks, " empty ", directory.emptyBlocks, " sparse ", directory.sparseBlocks, " live ", directory.liveCells, " free ", directory.freeCells, " subspace ", key.first->name());
    for (auto& [kind, strings] : m_strings)
        m_out.println("census: strings ", kind, " strings ", strings.strings, " bytes ", strings.bytes);

    // class, JSType and subspace of the cells (the last only where one is alive), kind, has a table, has rare data, cells alive (0, 1, more)
    struct OfStructures {
        uint64_t structures { 0 };
        uint64_t properties { 0 };
    };
    std::map<std::tuple<const ClassInfo*, unsigned, Subspace*, unsigned, bool, bool, unsigned>, OfStructures> structures;
    for (Structure* structure : m_structures) {
        // 0 root, 1 transition, 2 dictionary, 3 of a compiled layout
        unsigned kind = structure->isDictionary() ? 2 : structure->previousID() ? 1 : 0;
#if ENABLE(AOT)
        if (structure->isOfCompiledCells())
            kind = 3;
#endif
        auto found = m_cellsOfStructure.find(structure);
        uint64_t cells = found == m_cellsOfStructure.end() ? 0 : found->second.first;
        Subspace* subspace = found == m_cellsOfStructure.end() ? nullptr : found->second.second;
        auto& entry = structures[{ structure->classInfoForCells(), static_cast<unsigned>(structure->typeInfo().type()), subspace, kind, !!*reinterpret_cast<PropertyTable* const*>(reinterpret_cast<const uint8_t*>(structure) + Structure::propertyTableUnsafeOffset()), structure->hasRareData(), cells > 1 ? 2 : cells }];
        ++entry.structures;
        entry.properties += structure->inlineSize() + structure->outOfLineSize();
    }
    for (auto& [key, entry] : structures) {
        auto [classInfo, type, subspace, kind, hasTable, hasRareData, cells] = key;
        m_out.print("census: structures of ", classInfo->className, " kind ", kind, " table ", hasTable ? 1 : 0, " rare ", hasRareData ? 1 : 0, " cells ", cells, " structures ", entry.structures, " properties ", entry.properties, " type ", type, " subspace ");
        if (subspace)
            m_out.println(subspace->name());
        else
            m_out.println("-");
    }

    for (auto& [key, cells] : m_functions) {
        auto [subspace, kind, hasRareData, hasStorage] = key;
        m_out.println("census: functions executable ", kind, " rare ", hasRareData ? 1 : 0, " storage ", hasStorage ? 1 : 0, " cells ", cells, " subspace ", subspace->name());
    }
    uint64_t heldNativeExecutables = 0;
    for (ExecutableBase* executable : m_nativeExecutables)
        heldNativeExecutables += m_executablesOfFunctions.count(executable);
    m_out.println("census: native-executables ", m_nativeExecutables.size(), " of-a-live-function ", heldNativeExecutables);
    for (auto& [key, cells] : m_boundFunctions)
        m_out.println("census: bound arguments ", key.first, " cells ", cells, " target ", key.second->name());
    for (auto& [key, cells] : m_arrays) {
        auto [length, capacity, indexingMode] = key;
        m_out.println("census: array length ", length, " capacity ", capacity, " indexing ", indexingMode, " cells ", cells);
    }
    for (auto& [size, cells] : m_maps)
        m_out.println("census: map size ", size, " cells ", cells);
    for (auto& [size, cells] : m_sets)
        m_out.println("census: set size ", size, " cells ", cells);
    for (auto& [mode, views] : m_views)
        m_out.println("census: views mode ", mode, " cells ", views.strings, " bytes ", views.bytes);
    m_out.println("census: final-objects ", m_finalObjects, " inline-capacity ", m_inlineCapacity, " inline-used ", m_inlineSize, " out-of-line-capacity ", m_outOfLineCapacity, " out-of-line-used ", m_outOfLineSize);

    uint64_t bytesOf8BitSources = 0;
    uint64_t bytesOf16BitSources = 0;
    for (SourceProvider* provider : m_sources) {
        StringView source = provider->source();
        (source.is8Bit() ? bytesOf8BitSources : bytesOf16BitSources) += source.length() * (source.is8Bit() ? 1 : 2);
    }
    m_out.println("census: sources ", m_sources.size(), " bytes-8 ", bytesOf8BitSources, " bytes-16 ", bytesOf16BitSources);

    // tier (0 interpreter, 1 baseline, 2 DFG, 3 FTL)
    struct OfCodeBlocks {
        uint64_t codeBlocks { 0 };
        uint64_t metadataBytes { 0 };
        uint64_t machineCodeBytes { 0 };
    };
    std::array<OfCodeBlocks, 4> tiers;
    heap.forEachCodeBlock([&](CodeBlock* codeBlock) {
        unsigned tier = 0;
        switch (codeBlock->jitType()) {
        case JITType::BaselineJIT:
            tier = 1;
            break;
        case JITType::DFGJIT:
            tier = 2;
            break;
        case JITType::FTLJIT:
            tier = 3;
            break;
        default:
            break;
        }
        auto& entry = tiers[tier];
        ++entry.codeBlocks;
        if (auto* table = codeBlock->metadataTable())
            entry.metadataBytes += table->sizeInBytesForGC();
        if (JITCode::isJIT(codeBlock->jitType())) {
            if (auto jitCode = codeBlock->jitCode(); jitCode && !jitCode->isShared())
                entry.machineCodeBytes += jitCode->size();
        }
    });
    for (unsigned tier = 0; tier < tiers.size(); ++tier)
        m_out.println("census: code-blocks tier ", tier, " cells ", tiers[tier].codeBlocks, " metadata ", tiers[tier].metadataBytes, " machine-code ", tiers[tier].machineCodeBytes);

#if ENABLE(AOT)
    using namespace AOT::Typed;
    for (AOT::Instance* instance : m_vm.m_aotInstances) {
        if (instance->typed)
            m_out.println("census: strings-made ", instance->typed->numberOfStringsMade(), " slots ", (instance->typedStrings[0] >> 32) + 1);
    }
    for (auto& [key, cells] : m_layouts)
        m_out.println("census: layout ", key.first->index, " kind ", static_cast<unsigned>(key.first->kind), " cell ", key.second, " cells ", cells);
    for (auto& [layout, slots] : m_slots) {
        static constexpr uint32_t none[3] = { 0, 0, 0 };
        const uint32_t* references = layout->referencesOffset ? reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(layout) + layout->referencesOffset) : none;
        m_out.println("census: slots ", layout->index, " size ", layout->size, " objects ", slots.objects, " bytes ", slots.bytes, " boxed ", references[0], " wide ", references[1], " narrow ", references[2],
            " filled-boxed ", slots.filledBoxed, " filled-wide ", slots.filledWide, " filled-narrow ", slots.filledNarrow);
        m_out.print("census: placed ", layout->index, " size ", layout->size, " extension ", layout->hasExtension ? 1 : 0, " :");
        for (uint32_t i = 0; i < layout->numberOfFields; ++i) {
            const FieldData& field = layout->fields()[i];
            if (!(field.flags & fieldIsOutOfLine))
                m_out.print(" ", field.offset, ":", static_cast<unsigned>(field.store()));
        }
        m_out.println();
    }
#endif
}

} // namespace

} // namespace JSC

// For a tool outside the process's own code (a debugger, an inserted library). To be called on a thread that holds the lock of the VM, at a point where it runs no JavaScript.
extern "C" __attribute__((used)) JS_EXPORT_PRIVATE void JSC__writeHeapCensus(int fileDescriptor, bool collectFirst);
void JSC__writeHeapCensus(int fileDescriptor, bool collectFirst)
{
    using namespace JSC;
    FILE* file = fdopen(dup(fileDescriptor), "a");
    if (!file)
        return;
    FilePrintStream out(file, FilePrintStream::Adopt);
    unsigned number = 0;
    VMManager::forEachVM([&](VM& vm) {
        bool isOurs = vm.currentThreadIsHoldingAPILock();
        out.println("census: vm ", number++, " walked ", isOurs ? 1 : 0);
        if (isOurs)
            HeapCensus(vm, out).run(collectFirst);
        return IterationStatus::Continue;
    });
    out.flush();
}

#endif // USE(BUN_JSC_ADDITIONS)
