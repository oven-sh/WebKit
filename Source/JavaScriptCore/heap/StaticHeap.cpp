/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "StaticHeap.h"

#include "AOTImage.h"
#include "AOTRuntime.h"
#include "AbstractSlotVisitorInlines.h"
#include "BuiltinNames.h"
#include "CachedTypes.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "PreciseAllocation.h"
#include "SourceCodeKey.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include <wtf/BitVector.h>
#include <wtf/text/AtomStringTable.h>
#include <wtf/text/SymbolRegistry.h>

#if OS(DARWIN)
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace JSC {

using Region = bmalloc::StaticRegion;

bool StaticHeap::s_isBuilding = false;
VM* StaticHeap::s_vm = nullptr;
bool StaticHeap::s_hasNoCompilerThreads = false;
const StaticHeap::Header* StaticHeap::s_header = nullptr;
static constexpr size_t pageSizeOfImage = 16 * KB;

struct StaticHeapModule {
    uint32_t entryOffset; // What they are sorted by.
    // Of the key that the code is for, all that is not the same for every module, or the text itself.
    uint32_t keyHash;
    uint32_t keyLength;
    uint32_t keyFlags;
    uint64_t codeBlock;
};

struct StaticHeapTDZ {
    uint64_t executable; // What they are sorted by.
    uint64_t record;
    uint64_t moduleIndex;
};
static Vector<StaticHeapTDZ>* s_tdzBeingBuilt;
static JSString* s_emptyStringBeingBuilt;
static SymbolRegistry* s_symbolRegistriesBeingBuilt[2];
static size_t s_moduleBeingBuilt;

// The file: this, then each arena, on a page boundary.
struct StaticHeap::Header {
    static constexpr uint64_t expectedMagic = 0x3430504145485442ULL; // "BTHEAP04"
    static constexpr unsigned maxStructures = 32;

    uint64_t magic;
    uint64_t stamp; // Of the engine: what is in the arenas is its objects.
    uint64_t size; // Of everything.
    uint64_t arenaOffset[Region::numberOfArenasInFile];
    uint64_t arenaSize[Region::numberOfArenasInFile];

    // Addresses.
    uint64_t strings;
    uint64_t stringsSize;
    uint64_t stringSlots;
    uint64_t atomStringTable;
    uint64_t symbolRegistries[2]; // SymbolRegistry*: public, private.
    uint64_t payload;
    uint64_t payloadSize;
    uint64_t modules; // StaticHeapModule[]
    uint64_t numberOfModules;
    uint64_t tdz; // StaticHeapTDZ[]
    uint64_t numberOfTDZ;
    uint64_t infosOfFunctions; // AOT::FunctionInfo[], by AOT::CodeHeader::index.
    uint64_t factsOfFunctions; // uint32_t[], likewise. Zero: the unlinked code of functions is here instead.
    uint64_t numberOfFunctions;

    // What the cells say they are: which of the VM's own structures, and where that has to be.
    uint32_t numberOfStructures;
    struct {
        uint32_t indexInVM;
        uint32_t id;
    } structures[maxStructures];
};

// The VM's own structures, which it makes first, are members of it that follow one another.
static std::span<WriteBarrier<Structure>> structuresOf(VM& vm)
{
    return { &vm.structureStructure, static_cast<size_t>(&vm.bigIntStructure + 1 - &vm.structureStructure) };
}

// In Arena::Bss, a Decoder for each module.
static void* addressOfDecoder(size_t moduleIndex)
{
    constexpr size_t stride = roundUpToMultipleOf<64>(sizeof(Decoder));
    return reinterpret_cast<void*>(Region::startOf(Region::Arena::Bss) + Region::offsetOfDecodersInBss + moduleIndex * stride);
}

static VM* s_vmOfContainer = nullptr;

void StaticHeap::makeContainer(VM& vm)
{
    if (s_vmOfContainer == &vm)
        return;
    s_vmOfContainer = &vm;
    PreciseAllocation::setContainerOfStaticCells(PreciseAllocation::createForStaticCells(vm.heap, &vm.heap.cellSpace));
}

void StaticHeap::placeNextCell(VM& vm, void* address)
{
    RELEASE_ASSERT(!vm.heap.m_placeOfNextCell && !s_isBuilding && contains(address) && (std::bit_cast<uintptr_t>(address) & 15) == sizeOfCellHeader);
    vm.heap.m_placeOfNextCell = address;
}

bool StaticHeap::canPlaceCellsOf(VM& vm)
{
    static Lock lock;
    Locker locker { lock };
    if (!s_vmOfContainer && !s_isBuilding)
        makeContainer(vm);
    return s_vmOfContainer == &vm;
}

static Lock s_blocksLock;
static size_t s_blocksUsed WTF_GUARDED_BY_LOCK(s_blocksLock) = 0;
static Vector<std::pair<void*, size_t>>& freeBlocks() WTF_REQUIRES_LOCK(s_blocksLock)
{
    static NeverDestroyed<Vector<std::pair<void*, size_t>>> blocks;
    return blocks;
}

void* StaticHeap::allocateBlock(size_t size)
{
    RELEASE_ASSERT(!(size % WTF::pageSize()));
    Locker locker { s_blocksLock };
    auto& free = freeBlocks();
    for (unsigned i = 0; i < free.size(); ++i) {
        if (free[i].second == size) {
            void* result = free[i].first;
            free.removeAt(i);
            return result;
        }
    }
    size_t offset = Region::offsetOfBlocksInBss + s_blocksUsed;
    RELEASE_ASSERT(size <= Region::arenaReservation - offset);
    s_blocksUsed += size;
    return reinterpret_cast<void*>(Region::startOf(Region::Arena::Bss) + offset);
}

void StaticHeap::freeBlock(void* block, size_t size)
{
    // Zeroed again, and nobody's memory until it is written to.
    void* result = mmap(block, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
    RELEASE_ASSERT(result == block);
    Locker locker { s_blocksLock };
    freeBlocks().append({ block, size });
}

void StaticHeap::didPlaceCell(VM& vm, JSCell* cell)
{
    RELEASE_ASSERT(contains(cell) && !vm.heap.m_placeOfNextCell);
    // As after a collection that found it. What has been stored in it since it was allocated is found by the next one.
    cell->setCellState(CellState::PossiblyBlack);
    vm.writeBarrier(cell);
}

void* StaticHeap::tryAllocateCellSlow(VM& vm, size_t size)
{
    if (vm.heap.m_placeOfNextCell != placeOfEveryCellWhileBuilding) {
        void* place = std::exchange(vm.heap.m_placeOfNextCell, nullptr);
        *reinterpret_cast<size_t*>(static_cast<char*>(place) - sizeOfCellHeader) = size;
        return place;
    }
    if (!Region::isAllocatingOnThisThread())
        return nullptr;
    auto arena = Region::isAllocatingWhatIsMutable() ? Region::Arena::MutableCells : Region::Arena::Cells;
    auto* header = static_cast<char*>(Region::allocate(arena, sizeOfCellHeader + size, 16));
    *reinterpret_cast<size_t*>(header) = size;
    return header + sizeOfCellHeader;
}

JSString* StaticHeap::emptyStringWhileBuilding(VM& vm)
{
    if (!s_emptyStringBeingBuilt)
        s_emptyStringBeingBuilt = JSString::createHasOtherOwner(vm, *StringImpl::empty());
    return s_emptyStringBeingBuilt;
}

SymbolRegistry& StaticHeap::symbolRegistryWhileBuilding(bool isPrivate)
{
    return *s_symbolRegistriesBeingBuilt[isPrivate];
}

void StaticHeap::noteParentScopeTDZVariables(const UnlinkedFunctionExecutable& executable, const void* record)
{
    Region::AllocationScope notInRegion(false);
    s_tdzBeingBuilt->append({ std::bit_cast<uint64_t>(&executable), std::bit_cast<uint64_t>(record), s_moduleBeingBuilt });
}

namespace {

// Says of a cell whether everything the collector would go on to from it is in the region too.
class ClosureChecker final : public AbstractSlotVisitor {
public:
    ClosureChecker(VM& vm)
        : AbstractSlotVisitor(vm.heap, "StaticHeap"_s, m_opaqueRootStorage)
    {
    }

    unsigned numberOfEscapes { 0 };
    JSCell* current { nullptr };

    void check(const void* pointer, const char* what)
    {
        if (!pointer || StaticHeap::contains(pointer))
            return;
        numberOfEscapes++;
        if (m_reported.add(pointer).isNewEntry && m_reported.size() <= 40) {
            auto* cell = static_cast<const JSCell*>(pointer);
            dataLogLn("StaticHeap: a ", current->classInfo()->className, " refers to ", what, " outside: ", RawPointer(pointer), " ", !strcmp(what, "a cell") ? cell->classInfo()->className : ""_s);
        }
    }

    void append(const ConservativeRoots&) final { }
    void appendUnbarriered(JSCell* cell) final
    {
        // (Which are looked at on their own.)
        if (cell && cell->type() == StructureType)
            return;
        check(cell, "a cell");
    }
    void appendHiddenUnbarriered(JSCell* cell) final { appendUnbarriered(cell); }
    bool isFirstVisit() const final { return true; }
    bool isMarked(const void*) const final { return true; }
    bool isMarked(MarkedBlock&, HeapCell*) const final { return true; }
    bool isMarked(PreciseAllocation&, HeapCell*) const final { return true; }
    void markAuxiliary(const void* base) final { check(base, "storage"); }
    void reportExtraMemoryVisited(size_t) final { }
    void reportExternalMemoryVisited(size_t) final { }
    bool mutatorIsStopped() const final { return true; }
    void didRace(const VisitRaceKey&) final { }
    void visitAsConstraint(const JSCell*) final { }
    void addParallelConstraintTask(RefPtr<SharedTask<void(AbstractSlotVisitor&)>>) final { }
    void addParallelConstraintTask(RefPtr<SharedTask<void(SlotVisitor&)>>) final { }

private:
    UncheckedKeyHashSet<const void*> m_reported;
    ConcurrentPtrHashSet m_opaqueRootStorage;
};

} // anonymous namespace

#if OS(DARWIN)
// Every word that may be the address of something that is not in the region. Some are not addresses.
static void reportWhatMayPointOut()
{
    UncheckedKeyHashMap<uint64_t, bool> isMappedPage;
    auto looksLikePointerOut = [&](uint64_t word) {
        if ((word & 7) || word < 4 * GB || word >= (1ULL << 47) || Region::contains(reinterpret_cast<void*>(word)))
            return false;
        return isMappedPage.ensure(word >> 14, [&] {
            mach_vm_address_t address = word;
            mach_vm_size_t size = 0;
            vm_region_basic_info_data_64_t info;
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object;
            return mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object) == KERN_SUCCESS && address <= word && info.protection;
        }).iterator->value;
    };
    auto describeTarget = [&](uint64_t word) -> String {
        Dl_info info;
        if (dladdr(reinterpret_cast<void*>(word), &info) && info.dli_sname && reinterpret_cast<uint64_t>(info.dli_saddr) + 65536 > word)
            return makeString("symbol "_s, String::fromUTF8(info.dli_sname).left(90));
        return "heap"_s;
    };
    UncheckedKeyHashMap<String, unsigned> found;
    for (auto arena : { Region::Arena::Cells, Region::Arena::MutableCells }) {
        StaticHeap::forEachCell(arena, Region::used(arena), [&](void* pointer, size_t size) {
            auto* words = static_cast<uint64_t*>(pointer);
            for (size_t i = 0; i < size / 8; ++i) {
                if (looksLikePointerOut(words[i]))
                    found.add(makeString("cell "_s, String::fromLatin1(static_cast<JSCell*>(pointer)->classInfo()->className.characters()), " +"_s, i * 8, " -> "_s, describeTarget(words[i])), 0).iterator->value++;
            }
        });
    }
    for (auto arena : { Region::Arena::Malloc, Region::Arena::MutableMalloc }) {
        uintptr_t start = Region::startOf(arena);
        size_t used = Region::used(arena);
        // Each allocation is preceded by its size, and starts at a multiple of 16 (or more, which leaves a gap of zeros).
        for (size_t offset = 8; offset < used;) {
            size_t size = *reinterpret_cast<size_t*>(start + offset);
            if (!size) {
                offset += 16;
                continue;
            }
            auto* words = reinterpret_cast<uint64_t*>(start + offset + 8);
            for (size_t i = 0; i < size / 8; ++i) {
                if (looksLikePointerOut(words[i])) {
                    auto& count = found.add(makeString("malloc("_s, size > 512 ? 999999 : size, ") +"_s, i > 12 ? 999 : i * 8, " -> "_s, describeTarget(words[i])), 0).iterator->value;
                    if (++count <= 2) {
                        dataLog("StaticHeap: OUT example, ", size, " bytes at ", RawPointer(words), ":");
                        for (size_t j = 0; j < std::min<size_t>(size / 8, 8); ++j)
                            dataLog(" ", RawHex(words[j]));
                        auto* target = reinterpret_cast<uint64_t*>(words[i]);
                        dataLogLn("  target: ", RawHex(target[0]), " ", RawHex(target[1]), " ", RawHex(target[2]), " ", RawHex(target[3]));
                    }
                }
            }
            offset += (8 + size + 15) & ~static_cast<size_t>(15);
        }
    }
    Vector<std::pair<unsigned, String>> sorted;
    for (auto& entry : found)
        sorted.append({ entry.value, entry.key });
    std::ranges::sort(sorted, [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& entry : sorted)
        dataLogLn("StaticHeap: OUT ", entry.first, "  ", entry.second);
}
#endif

static void* addressOfSourceProvider(size_t moduleIndex)
{
    return reinterpret_cast<void*>(Region::startOf(Region::Arena::Bss) + Region::offsetOfSourceProvidersInBss + moduleIndex * StaticHeap::sizeOfPlaceForSourceProvider);
}

// AOT::FunctionFacts of a function whose code is not going to be here. What they refer to stays (UnlinkedCodeBlock::leaveToStaticHeap()).
static std::span<uint32_t> s_factsBeingBuilt;

static void fillFacts(uint32_t index, UnlinkedCodeBlock* codeBlock)
{
    if (s_factsBeingBuilt.empty() || s_factsBeingBuilt[index])
        return;
    using Facts = AOT::FunctionFacts;
    auto in = [](Region::Arena arena, const void* pointer) {
        uintptr_t offset = std::bit_cast<uintptr_t>(pointer) - Region::startOf(arena);
        RELEASE_ASSERT(offset && offset < Region::used(arena));
        return static_cast<uint32_t>(offset);
    };
    RELEASE_ASSERT(codeBlock->instructions().size() < (1u << (32 - Facts::shiftOfInstructionsSize)));
    Vector<uint32_t, 10> words { static_cast<uint32_t>(codeBlock->instructions().size()) << Facts::shiftOfInstructionsSize | (codeBlock->isBuiltinFunction() ? Facts::isBuiltinFunction : 0) };
    if (const void* record = codeBlock->cachedExpressionInfo()) {
        words[0] |= Facts::ExpressionInfo;
        words.append(in(Region::Arena::Data, record));
    }
    if (size_t count = codeBlock->numberOfExceptionHandlers()) {
        auto* handlers = static_cast<UnlinkedHandlerInfo*>(Region::allocate(Region::Arena::Data, count * sizeof(UnlinkedHandlerInfo), alignof(UnlinkedHandlerInfo)));
        for (size_t i = 0; i < count; ++i)
            handlers[i] = codeBlock->exceptionHandler(i);
        words[0] |= Facts::Handlers;
        words.append(in(Region::Arena::Data, handlers));
        words.append(count);
    }
    auto functions = [&](Facts::Fact fact, std::span<const WriteBarrier<UnlinkedFunctionExecutable>> all) {
        if (all.empty())
            return;
        words[0] |= fact;
        words.append(in(Region::Arena::Malloc, all.data()));
        words.append(all.size());
    };
    functions(Facts::FunctionDecls, codeBlock->functionDecls());
    functions(Facts::FunctionExprs, codeBlock->functionExprs());
    if (codeBlock->numberOfUnlinkedStringSwitchJumpTables()) {
        words[0] |= Facts::StringSwitchJumpTables;
        words.append(in(Region::Arena::Malloc, &codeBlock->unlinkedStringSwitchJumpTable(0)));
    }
    if (!AOT::constantsAreOfNoRealm(codeBlock, AOT::SymbolTablesWillDo::Yes)) {
        auto& representations = codeBlock->constantsSourceCodeRepresentation();
        Vector<uint32_t, 16> list { static_cast<uint32_t>(representations.size()), 0 };
        for (unsigned i = 0; i < representations.size(); ++i) {
            if (representations[i] == SourceCodeRepresentation::LinkTimeConstant)
                list.append(i);
        }
        list[1] = list.size() - 2;
        auto* copy = static_cast<uint32_t*>(Region::allocate(Region::Arena::Data, list.sizeInBytes(), alignof(uint32_t)));
        memcpySpan(std::span { copy, list.size() }, list.span());
        words[0] |= Facts::RealmConstants;
        words.append(in(Region::Arena::Data, copy));
    }
    auto* facts = static_cast<uint32_t*>(Region::allocate(Region::Arena::Data, words.sizeInBytes(), alignof(uint32_t)));
    memcpySpan(std::span { facts, words.size() }, words.span());
    s_factsBeingBuilt[index] = in(Region::Arena::Data, facts);
}

static void fillInfo(AOT::FunctionInfo& info, const AOT::ImageView::Function& function, UnlinkedCodeBlock* codeBlock, ScriptExecutable* executable, CodeSpecializationKind kind)
{
    // (Its SymbolTables are being made here and now.)
    bool constantsWillDo = AOT::constantsAreOfNoRealm(codeBlock, AOT::SymbolTablesWillDo::Yes);
    RELEASE_ASSERT(constantsWillDo || !function.startsCold);
    info.constants = codeBlock->constantRegisters().span().data();
    info.identifiers = codeBlock->identifiers().span().data();
    info.sites = function.sites;
    info.function = function.function;
    info.executableAndKind = executable ? std::bit_cast<uintptr_t>(executable) | !isCall(kind) : 0;
    info.numSlots = function.numSlots;
    // (It takes a Data to say which executable's the code is, if this does not.)
    info.flags = AOT::FunctionInfo::hasSiteConstants | (function.startsCold && executable ? AOT::FunctionInfo::startsCold : 0) | (isCall(kind) ? 0 : AOT::FunctionInfo::constructs) | (constantsWillDo ? AOT::FunctionInfo::constantsAreOfNoRealm : 0);
    if (codeBlock->codeType() == FunctionCode)
        fillFacts(function.index, codeBlock);
}

// The functions of `codeBlock`, whose source is `source`, and theirs.
static void makeExecutables(VM& vm, UnlinkedCodeBlock* codeBlock, const SourceCode& source, bool isInsideOrdinaryFunction, uint32_t moduleID, const AOT::ImageView& image, std::span<AOT::FunctionInfo> byIndex, uint64_t& made)
{
    auto make = [&](UnlinkedFunctionExecutable* unlinked) {
        if (unlinked->staticExecutable())
            return;
        // The source of a constructor that nobody wrote is one of the engine's own, so its executable is made when the program runs.
        bool isDefaultConstructor = unlinked->isBuiltinDefaultClassConstructor();
        auto functionKey = orderFunctionKey(*unlinked, isDefaultConstructor ? source : unlinked->linkedSourceCode(source));
        if (!functionKey)
            return;
        std::optional<AOT::ImageView::Function> code[2];
        for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
            if (!unlinked->codeBlockIfThereIsOne(kind))
                continue;
            AOT::ImageKey key;
            key.module = moduleID;
            key.start = functionKey->start;
            key.kind = static_cast<uint32_t>(functionKey->kind) << 1 | (kind == CodeSpecializationKind::CodeForConstruct);
            code[static_cast<unsigned>(kind)] = image.find(key);
        }
        if (!code[0] && !code[1])
            return;
        if (isDefaultConstructor) {
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (auto& function = code[static_cast<unsigned>(kind)]; function && !byIndex[function->index].sites)
                    fillInfo(byIndex[function->index], *function, unlinked->codeBlockIfThereIsOne(kind), nullptr, kind);
            }
            return;
        }
        // A function that is compiled both to be called and to construct has the functions inside it twice over. There is one piece
        // of code for each of those all the same, which is the code of one executable (AOT::FunctionInfo).
        FunctionExecutable* executable = nullptr;
        for (auto& function : code) {
            if (function && byIndex[function->index].executableAndKind)
                executable = uncheckedDowncast<FunctionExecutable>(byIndex[function->index].executable());
        }
        if (executable) {
            bool hasAllOfIt = true;
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (auto& function = code[static_cast<unsigned>(kind)])
                    hasAllOfIt &= byIndex[function->index].executable() == executable && byIndex[function->index].kind() == kind;
            }
            if (!hasAllOfIt)
                return;
            unlinked->setStaticExecutable(executable);
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (auto* nested = unlinked->codeBlockIfThereIsOne(kind))
                    makeExecutables(vm, nested, executable->source(), executable->isInsideOrdinaryFunction(), moduleID, image, byIndex, made);
            }
            return;
        }
        executable = unlinked->link(vm, nullptr, source, std::nullopt, NoIntrinsic, isInsideOrdinaryFunction);
        executable->becomeStatic(vm);
        for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
            if (auto& function = code[static_cast<unsigned>(kind)]) {
                    executable->setAOTCode(kind, image.addressOfStub(isCall(kind) ? AOT::Stub::EnterStaticFunctionForCall : AOT::Stub::EnterStaticFunctionForConstruct), function->entry, function->index);
                fillInfo(byIndex[function->index], *function, unlinked->codeBlockIfThereIsOne(kind), executable, kind);
            }
        }
        // (Whatever cannot be constructed with like that has code for it, or is not to be constructed with.)
        if (code[0] && !code[1] && unlinked->constructAbility() == ConstructAbility::CanConstruct && !unlinked->isClassConstructorFunction()) {
            void* stub = image.addressOfStub(AOT::Stub::ConstructByCalling);
            executable->setAOTCode(CodeSpecializationKind::CodeForConstruct, stub, stub, FunctionExecutable::aotIndexOfWhatConstructsByCalling);
        }
        unlinked->setStaticExecutable(executable);
        made++;
        for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
            if (auto* nested = unlinked->codeBlockIfThereIsOne(kind))
                makeExecutables(vm, nested, executable->source(), executable->isInsideOrdinaryFunction(), moduleID, image, byIndex, made);
        }
    };
    for (unsigned i = 0; i < codeBlock->numberOfFunctionDecls(); ++i)
        make(codeBlock->functionDecl(i));
    for (unsigned i = 0; i < codeBlock->numberOfFunctionExprs(); ++i)
        make(codeBlock->functionExpr(i));
}

std::pair<FunctionExecutable*, CodeSpecializationKind> StaticHeap::executableOfFunction(uint32_t index)
{
    RELEASE_ASSERT(s_header && index < s_header->numberOfFunctions);
    const AOT::FunctionInfo& info = std::bit_cast<const AOT::FunctionInfo*>(s_header->infosOfFunctions)[index];
    RELEASE_ASSERT(info.executableAndKind);
    return { uncheckedDowncast<FunctionExecutable>(info.executable()), info.kind() };
}

const uint32_t* StaticHeap::factsOfFunctions(VM& vm)
{
    return hasExecutablesOfFunctions(vm) ? std::bit_cast<const uint32_t*>(s_header->factsOfFunctions) : nullptr;
}

Ref<Decoder> StaticHeap::decoderOfWhatWasLeftInPayload(VM& vm, Decoder& placed)
{
    RELEASE_ASSERT(s_vm == &vm);
    static NeverDestroyed<UncheckedKeyHashMap<Decoder*, Ref<Decoder>>> decoders;
    return decoders->ensure(&placed, [&] {
        return Decoder::create(vm, placed.cachedBytecode(), placed.provider(), Decoder::RecoverableCode::No);
    }).iterator->value;
}

AOT::FunctionInfo* StaticHeap::infosOfFunctions(VM& vm)
{
    return hasExecutablesOfFunctions(vm) ? std::bit_cast<AOT::FunctionInfo*>(s_header->infosOfFunctions) : nullptr;
}

bool StaticHeap::hasExecutablesOfFunctions(VM& vm)
{
    return s_header && s_vm == &vm && s_header->numberOfFunctions;
}

void* StaticHeap::takePlaceForSourceProvider(VM& vm, size_t entryOffset, size_t sizeOfProvider)
{
    RELEASE_ASSERT(sizeOfProvider <= sizeOfPlaceForSourceProvider);
    if (!s_header || s_vm != &vm)
        return nullptr;
    std::span<const StaticHeapModule> modules { std::bit_cast<const StaticHeapModule*>(s_header->modules), static_cast<size_t>(s_header->numberOfModules) };
    size_t index = std::ranges::lower_bound(modules, entryOffset, { }, &StaticHeapModule::entryOffset) - modules.begin();
    if (index == modules.size() || modules[index].entryOffset != entryOffset || !modules[index].codeBlock)
        return nullptr;
    void* place = addressOfSourceProvider(index);
    if (*static_cast<uintptr_t*>(place)) // What is there starts with the address of a table of virtual functions.
        return nullptr;
    return place;
}

Vector<uint8_t> StaticHeap::build(VM& vm, std::span<const uint8_t> strings, std::span<const uint8_t> payload, std::span<const uint32_t> entryOffsetsOfModules, std::span<const uint8_t> imageOfCode)
{
    if (!Region::beginBuilding())
        return { };
    PreciseAllocation* containerBefore = PreciseAllocation::containerOfStaticCells();
    VM* vmOfContainerBefore = s_vmOfContainer;
    VM* vmBefore = s_vm;
    makeContainer(vm);
    s_vm = &vm;

    Header header { };
    header.magic = Header::expectedMagic;
    header.stamp = AOT::imageStamp();

    auto copy = [&](std::span<const uint8_t> bytes) {
        auto* result = static_cast<uint8_t*>(Region::allocate(Region::Arena::Data, bytes.size(), pageSizeOfImage));
        memcpy(result, bytes.data(), bytes.size());
        return std::span<uint8_t> { result, bytes.size() };
    };
    auto stringsCopy = copy(strings);
    header.strings = std::bit_cast<uint64_t>(stringsCopy.data());
    header.stringsSize = strings.size();

    bool ok = true;
    unsigned numberOfCodeBlocksFailed = 0;
    {
        DeferGC deferGC(vm);
        uint32_t count = *reinterpret_cast<const uint32_t*>(strings.data());
        auto* slots = static_cast<uintptr_t*>(Region::allocate(Region::Arena::Data, static_cast<size_t>(count) * sizeof(uintptr_t), pageSizeOfImage));
        header.stringSlots = std::bit_cast<uint64_t>(slots);
        DecoderStringTable table(stringsCopy, slots);

        // Every string is an atom, of a table that has nothing else in it: whatever is equal to one of them, when the program runs,
        // is that one.
        auto* atoms = new (NotNull, Region::allocate(Region::Arena::Data, sizeof(AtomStringTable), 16)) AtomStringTable;
        header.atomStringTable = std::bit_cast<uint64_t>(atoms);
        for (bool isPrivate : { false, true }) {
            s_symbolRegistriesBeingBuilt[isPrivate] = new (NotNull, Region::allocate(Region::Arena::Data, sizeof(SymbolRegistry), 16)) SymbolRegistry(isPrivate ? SymbolRegistry::Type::PrivateSymbol : SymbolRegistry::Type::PublicSymbol);
            header.symbolRegistries[isPrivate] = std::bit_cast<uint64_t>(s_symbolRegistriesBeingBuilt[isPrivate]);
        }
        AtomStringTable* usualAtoms = Thread::currentSingleton().setCurrentAtomStringTable(atoms);
        {
            Region::AllocationScope allocationScope;
            s_isBuilding = true;
            vm.heap.m_placeOfNextCell = placeOfEveryCellWhileBuilding;
            atoms->table().reserveInitialCapacity(count);
            for (uint32_t ordinal = 0; ordinal < count; ++ordinal) {
                table.atomFor(vm, ordinal);
                table.jsStringFor(vm, ordinal);
            }
            unsigned numberOfAtomsOfStrings = atoms->table().size();

            if (!payload.empty() && !entryOffsetsOfModules.empty()) {
                auto payloadCopy = copy(payload);
                header.payload = std::bit_cast<uint64_t>(payloadCopy.data());
                header.payloadSize = payload.size();
                Vector<uint32_t> sortedOffsets(entryOffsetsOfModules);
                std::ranges::sort(sortedOffsets);
                auto* modules = static_cast<StaticHeapModule*>(Region::allocate(Region::Arena::Data, sortedOffsets.size() * sizeof(StaticHeapModule), 16));
                header.modules = std::bit_cast<uint64_t>(modules);
                header.numberOfModules = sortedOffsets.size();
                auto imageView = AOT::ImageView::tryCreate(imageOfCode, reinterpret_cast<const void*>(Region::startOf(Region::Arena::Image)));
                std::span<AOT::FunctionInfo> infosOfFunctions;
                if (imageView) {
                    // (Where it can be written to: what is left blank is filled in if it turns out to be wanted. See AOT::Data::create().)
                    size_t size = imageView->numberOfFunctions() * sizeof(AOT::FunctionInfo);
                    infosOfFunctions = { static_cast<AOT::FunctionInfo*>(Region::allocate(Region::Arena::MutableMalloc, size, pageSizeOfImage)), imageView->numberOfFunctions() };
                    memset(static_cast<void*>(infosOfFunctions.data()), 0, size);
                    header.infosOfFunctions = std::bit_cast<uint64_t>(infosOfFunctions.data());
                    if (!Options::staticHeapKeepsFunctionCode()) {
                        s_factsBeingBuilt = { static_cast<uint32_t*>(Region::allocate(Region::Arena::Data, imageView->numberOfFunctions() * sizeof(uint32_t), pageSizeOfImage)), imageView->numberOfFunctions() };
                        zeroSpan(s_factsBeingBuilt);
                        header.factsOfFunctions = std::bit_cast<uint64_t>(s_factsBeingBuilt.data());
                    }
                    header.numberOfFunctions = infosOfFunctions.size();
                }
                uint64_t numberOfExecutables = 0;
                auto reportExecutables = makeScopeExit([&] {
                    if (Options::aotReportStats()) [[unlikely]] {
                        size_t known = 0, cold = 0;
                        for (auto& info : infosOfFunctions) {
                            known += !!info.sites;
                            cold += !!(info.flags & AOT::FunctionInfo::startsCold);
                        }
                        dataLogLn("StaticHeap: ", numberOfExecutables, " executables of functions; of the image's ", infosOfFunctions.size(), " functions ", known, " are known, and ", cold, " start cold");
                    }
                });
                Vector<StaticHeapTDZ> tdz;
                s_tdzBeingBuilt = &tdz;
                s_emptyStringBeingBuilt = nullptr;
                for (size_t i = 0; i < sortedOffsets.size(); ++i) {
                    s_moduleBeingBuilt = i;
                    RefPtr<CachedBytecode> cachedBytecode;
                    {
                        Region::AllocationScope notInRegion(false);
                        cachedBytecode = CachedBytecode::create(payloadCopy, [](const void*) { }, { });
                    }
                    cachedBytecode->setPayloadIsPersistent();
                    cachedBytecode->setEntryOffset(sortedOffsets[i]);
                    void* address = addressOfDecoder(i);
                    Decoder& decoder = Decoder::createForStaticHeap(address, vm, *cachedBytecode, nullptr);
                    decoder.setExternalStrings(table);
                    {
                        SourceCodeKey key;
                        Vector<std::pair<UnlinkedFunctionExecutable*, std::pair<int32_t, int32_t>>> functions;
                        auto forgetFunctions = makeScopeExit([&] {
                            Region::AllocationScope notInRegion(false);
                            functions = { };
                        });
                        UnlinkedCodeBlock* codeBlock = decodeAllForStaticHeap(decoder, key, functions);
                        if (codeBlock && (!key.name().isEmpty() || key.functionConstructorParametersEndPosition() != -1))
                            codeBlock = nullptr;
                        if (!codeBlock)
                            numberOfCodeBlocksFailed++;
                        else if (imageView) {
                            // Nothing is asked of the provider here, which is not there. It is counted as referred to, in memory that is
                            // not kept: once more than it is, or it would be destroyed if the module turns out to have no functions.
                            static_cast<SourceProvider*>(addressOfSourceProvider(i))->ref();
                            SourceCode source { RefPtr { static_cast<SourceProvider*>(addressOfSourceProvider(i)) }, 0, static_cast<int>(key.length()), 1, 1 };
                            makeExecutables(vm, codeBlock, source, false, sortedOffsets[i] + 1, *imageView, infosOfFunctions, numberOfExecutables);
                            // The code of the module itself, but for its executable, which is made when the program runs.
                            if (auto function = imageView->find(AOT::imageKeyForTopLevelCode(sortedOffsets[i] + 1)))
                                fillInfo(infosOfFunctions[function->index], *function, codeBlock, nullptr, CodeSpecializationKind::CodeForCall);
                        }
                        if (decoder.leavesFunctionCodeInPayload()) {
                            for (auto& [function, offsets] : functions) {
                                for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                                    if (auto* code = function->codeBlockIfThereIsOne(kind))
                                        code->leaveToStaticHeap(!!code->numberOfUnlinkedStringSwitchJumpTables());
                                }
                                function->leaveCodeInPayload(decoder, offsets);
                            }
                        }
                        modules[i] = { sortedOffsets[i], key.hash(), static_cast<uint32_t>(key.length()), key.flagsBits(), std::bit_cast<uint64_t>(codeBlock) };
                    }
                    // (Not destroyed: it is referred to. It has one reference to what is let go of right after.)
                    decoder.forgetWhatWasDecoded();
                    cachedBytecode->deref();
                    memset(address, 0, sizeof(Decoder));
                }
                s_tdzBeingBuilt = nullptr;
                s_factsBeingBuilt = { };
                std::ranges::sort(tdz, { }, &StaticHeapTDZ::executable);
                header.tdz = std::bit_cast<uint64_t>(copy(asByteSpan(tdz.span())).data());
                header.numberOfTDZ = tdz.size();
                {
                    Region::AllocationScope notInRegion(false);
                    tdz = { };
                }
            }
            if (Options::aotReportStats()) [[unlikely]]
                dataLogLn("StaticHeap: ", atoms->table().size() - numberOfAtomsOfStrings, " atoms that are not in the table of strings");
            s_isBuilding = false;
            vm.heap.m_placeOfNextCell = nullptr;
        }
        Thread::currentSingleton().setCurrentAtomStringTable(usualAtoms);
        for (auto& atom : atoms->table())
            atom->becomeStatic();
        for (auto* registry : s_symbolRegistriesBeingBuilt)
            registry->becomeStatic();
        if (Options::aotReportStats()) [[unlikely]]
            dataLogLn("StaticHeap: ", s_symbolRegistriesBeingBuilt[0]->size(), " registered symbols, ", s_symbolRegistriesBeingBuilt[1]->size(), " private ones");
    }

    // What the collector never looks at must not be all that keeps something alive.
    {
        ClosureChecker checker(vm);
        for (auto arena : { Region::Arena::Cells, Region::Arena::MutableCells }) {
            forEachCell(arena, Region::used(arena), [&](void* pointer, size_t) {
                checker.current = static_cast<JSCell*>(pointer);
                checker.current->methodTable()->visitChildren(checker.current, checker);
            });
        }
        if (checker.numberOfEscapes) {
            dataLogLn("StaticHeap: ", checker.numberOfEscapes, " references out of it");
            ok = false;
        }
    }
#if OS(DARWIN)
    if (getenv("BUN_STATIC_HEAP_AUDIT")) [[unlikely]]
        reportWhatMayPointOut();
    Region::dumpMallocAudit(); // TEMPORARY-MALLOC-AUDIT
#endif

    // Cells are as they would be in the VM that is going to have them, after a collection that found them.
    auto structures = structuresOf(vm);
    uint32_t blockOfStructures = vm.structureStructure->id().bits() & ~static_cast<uint32_t>(MarkedBlock::blockSize - 1);
    UncheckedKeyHashMap<uint32_t, uint32_t> idInFirstVM;
    UncheckedKeyHashMap<String, std::pair<uint64_t, uint64_t>> byClass;
    for (auto arena : { Region::Arena::Cells, Region::Arena::MutableCells }) {
        forEachCell(arena, Region::used(arena), [&](void* pointer, size_t size) {
            auto* cell = static_cast<JSCell*>(pointer);
            uint32_t id = cell->structureID().bits();
            if (Options::aotReportStats()) [[unlikely]] {
                auto& tally = byClass.add(String::fromLatin1(cell->classInfo()->className.characters()), std::pair<uint64_t, uint64_t> { }).iterator->value;
                tally.first++;
                tally.second += size;
            }
            uint32_t translated = idInFirstVM.ensure(id, [&]() -> uint32_t {
                if ((id & ~static_cast<uint32_t>(MarkedBlock::blockSize - 1)) != blockOfStructures || header.numberOfStructures == Header::maxStructures)
                    return 0;
                for (size_t i = 0; i < structures.size(); ++i) {
                    if (structures[i] && structures[i]->id().bits() == id) {
                        uint32_t result = offsetOfFirstStructureBlock + (id & (MarkedBlock::blockSize - 1));
                        header.structures[header.numberOfStructures++] = { static_cast<uint32_t>(i), result };
                        return result;
                    }
                }
                return 0;
            }).iterator->value;
            if (!translated) {
                if (ok)
                    dataLogLn("StaticHeap: a ", cell->classInfo()->className, " has a structure that is not one of the VM's own");
                ok = false;
                return;
            }
            *reinterpret_cast<uint32_t*>(cell) = translated;
            cell->setCellState(CellState::PossiblyBlack);
        });
    }

    Region::forgetWhatIsFree();
    Vector<uint8_t> image;
    if (ok) {
        size_t size = pageSizeOfImage;
        for (unsigned i = 0; i < Region::numberOfArenasInFile; ++i) {
            header.arenaOffset[i] = size;
            header.arenaSize[i] = roundUpToMultipleOf<pageSizeOfImage>(Region::used(static_cast<Region::Arena>(i)));
            size += header.arenaSize[i];
        }
        header.size = size;
        image.fill(0, size);
        memcpy(image.mutableSpan().data(), &header, sizeof(header));
        for (unsigned i = 0; i < Region::numberOfArenasInFile; ++i)
            memcpy(image.mutableSpan().data() + header.arenaOffset[i], reinterpret_cast<void*>(Region::startOf(static_cast<Region::Arena>(i))), Region::used(static_cast<Region::Arena>(i)));
    }
    if (Options::aotReportStats()) [[unlikely]] {
        dataLogLn("StaticHeap: ", ok ? "" : "FAILED ", image.size(), " bytes: data ", Region::used(Region::Arena::Data), ", malloc ", Region::used(Region::Arena::Malloc), ", cells ", Region::used(Region::Arena::Cells), ", mutable cells ", Region::used(Region::Arena::MutableCells), ", mutable malloc ", Region::used(Region::Arena::MutableMalloc), "; ", header.numberOfModules, " modules, ", numberOfCodeBlocksFailed, " not decoded");
        for (auto& entry : byClass)
            dataLogLn("StaticHeap:     ", entry.key, ": ", entry.value.first, " cells, ", entry.value.second, " bytes");
    }

    PreciseAllocation::setContainerOfStaticCells(containerBefore);
    s_vmOfContainer = vmOfContainerBefore;
    s_vm = vmBefore;
    Region::endBuilding();
    return image;
}

std::optional<StaticHeap::Copies> StaticHeap::copiesIn(std::span<const uint8_t> image)
{
    if (image.size() < sizeof(Header))
        return std::nullopt;
    auto& header = *reinterpret_cast<const Header*>(image.data());
    if (header.magic != Header::expectedMagic || header.size > image.size())
        return std::nullopt;
    constexpr unsigned data = static_cast<unsigned>(Region::Arena::Data);
    auto offsetOf = [&](uint64_t address, uint64_t size) -> std::optional<size_t> {
        uint64_t inArena = address - Region::startOf(Region::Arena::Data);
        if (!address || inArena > header.arenaSize[data] || size > header.arenaSize[data] - inArena)
            return std::nullopt;
        return header.arenaOffset[data] + inArena;
    };
    auto strings = offsetOf(header.strings, header.stringsSize);
    auto payload = offsetOf(header.payload, header.payloadSize);
    if (!strings || !payload)
        return std::nullopt;
    return Copies { *strings, static_cast<size_t>(header.stringsSize), *payload, static_cast<size_t>(header.payloadSize) };
}

bool StaticHeap::map(std::span<const uint8_t> image, int fileDescriptor, off_t offsetInFile)
{
    if (s_header || image.size() < sizeof(Header))
        return false;
    auto& header = *reinterpret_cast<const Header*>(image.data());
    if (header.magic != Header::expectedMagic || header.stamp != AOT::imageStamp() || header.size > image.size())
        return false;
    // An atom that the thread has already may be equal to one of these, and there can be only one.
    AtomStringTable* atoms = Thread::currentSingleton().atomStringTable();
    if (!atoms->table().isEmpty())
        return false;
    for (unsigned i = 0; i < Region::numberOfArenasInFile; ++i) {
        if (header.arenaOffset[i] % pageSizeOfImage || header.arenaOffset[i] > header.size || header.arenaSize[i] > header.size - header.arenaOffset[i])
            return false;
    }
    for (unsigned i = 0; i < Region::numberOfArenasInFile; ++i) {
        // (Some of it mapped and the rest not is nothing that anybody gets to see.)
        if (!Region::map(static_cast<Region::Arena>(i), fileDescriptor, offsetInFile + header.arenaOffset[i], header.arenaSize[i]))
            return false;
    }
    s_header = &header;
    atoms->setStaticTable(&std::bit_cast<AtomStringTable*>(header.atomStringTable)->table());
    return true;
}

void StaticHeap::install(VM& vm)
{
    if (!s_header || s_vm || &vm != addressOfVM())
        return;
    // Otherwise the strings are still good, as strings; the cells are not looked at again.
    auto structures = structuresOf(vm);
    for (unsigned i = 0; i < s_header->numberOfStructures; ++i) {
        auto& expected = s_header->structures[i];
        if (expected.indexInVM >= structures.size() || !structures[expected.indexInVM] || structures[expected.indexInVM]->id().bits() != expected.id)
            return;
    }
    s_hasNoCompilerThreads = !Options::useJIT();
    vm.symbolRegistry().setStaticRegistry(std::bit_cast<const SymbolRegistry*>(s_header->symbolRegistries[0]));
    vm.privateSymbolRegistry().setStaticRegistry(std::bit_cast<const SymbolRegistry*>(s_header->symbolRegistries[1]));
    makeContainer(vm);
    s_vm = &vm;
}

std::unique_ptr<DecoderStringTable> StaticHeap::tryCreateStringTable(VM& vm, std::span<const uint8_t> strings)
{
    if (!s_header || s_vm != &vm || strings.size() != s_header->stringsSize)
        return nullptr;
    auto* copy = std::bit_cast<const uint8_t*>(s_header->strings);
    // They come out of the same file. This is against a mistake, not against malice.
    size_t sample = std::min<size_t>(strings.size(), 4 * KB);
    if (memcmp(copy, strings.data(), sample) || memcmp(copy + strings.size() - sample, strings.data() + strings.size() - sample, sample))
        return nullptr;
    return makeUnique<DecoderStringTable>(std::span<const uint8_t> { copy, strings.size() }, std::bit_cast<uintptr_t*>(s_header->stringSlots));
}

RefPtr<TDZEnvironmentLink> StaticHeap::parentScopeTDZVariablesOf(const UnlinkedFunctionExecutable& executable)
{
    std::span<const StaticHeapTDZ> all { std::bit_cast<const StaticHeapTDZ*>(s_header->tdz), static_cast<size_t>(s_header->numberOfTDZ) };
    auto found = std::ranges::lower_bound(all, std::bit_cast<uint64_t>(&executable), { }, &StaticHeapTDZ::executable);
    if (found == all.end() || found->executable != std::bit_cast<uint64_t>(&executable))
        return nullptr;
    // (Which is there: the executable came out of a code block that codeFor() returned.)
    return decodeParentScopeTDZVariablesForStaticHeap(*static_cast<Decoder*>(addressOfDecoder(found->moduleIndex)), std::bit_cast<const void*>(found->record));
}

UnlinkedCodeBlock* StaticHeap::codeFor(VM& vm, const SourceCodeKey& key, const CachedBytecode& cachedBytecode)
{
    ASSERT(s_vm == &vm);
    if (!s_header->numberOfModules || !cachedBytecode.payloadIsPersistent() || cachedBytecode.size() < s_header->payloadSize)
        return nullptr;
    // A recording is of what is read out of the payload.
    if (BytecodeOrderRecorder::ofVM(vm)) [[unlikely]]
        return nullptr;
    std::span<uint8_t> payload { std::bit_cast<uint8_t*>(s_header->payload), static_cast<size_t>(s_header->payloadSize) };
    static const uint8_t* s_samePayload = nullptr;
    if (cachedBytecode.span().data() != s_samePayload) {
        size_t sample = std::min<size_t>(payload.size(), 4 * KB);
        if (memcmp(payload.data(), cachedBytecode.span().data(), sample))
            return nullptr;
        s_samePayload = cachedBytecode.span().data();
    }
    std::span<const StaticHeapModule> modules { std::bit_cast<const StaticHeapModule*>(s_header->modules), static_cast<size_t>(s_header->numberOfModules) };
    uint32_t entryOffset = static_cast<uint32_t>(cachedBytecode.entryOffset());
    size_t index = std::ranges::lower_bound(modules, entryOffset, { }, &StaticHeapModule::entryOffset) - modules.begin();
    if (index == modules.size() || modules[index].entryOffset != entryOffset || !modules[index].codeBlock)
        return nullptr;
    auto* module = &modules[index];
    if (key.hash() != module->keyHash || key.length() != module->keyLength || key.flagsBits() != module->keyFlags || !key.name().isEmpty() || key.functionConstructorParametersEndPosition() != -1)
        return nullptr;

    static NeverDestroyed<BitVector> s_hasDecoder;
    auto* decoder = static_cast<Decoder*>(addressOfDecoder(index));
    if (!s_hasDecoder->get(index)) {
        Ref ownBytecode = CachedBytecode::create(payload, [](const void*) { }, { });
        ownBytecode->setPayloadIsPersistent();
        ownBytecode->setEntryOffset(entryOffset);
        Decoder::createForStaticHeap(decoder, vm, WTF::move(ownBytecode), &key.source().provider());
        s_hasDecoder->set(index);
    }
    return std::bit_cast<UnlinkedCodeBlock*>(module->codeBlock);
}

} // namespace JSC
