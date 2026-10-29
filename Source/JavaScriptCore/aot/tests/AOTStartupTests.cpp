/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedObject.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTPropertyCaches.h"
#include "AOTRealmTestSupport.h"
#include "AOTRuntimeTable.h"
#include "AOTStubTestSupport.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsPropertyKeys.h"
#include "DeferGCInlines.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include <algorithm>
#include <array>
#include <wtf/StackBounds.h>

#if OS(LINUX)
#include <link.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace JSC::AOT {

std::span<const NativeTest> startupTests();

static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = Call v1, -, v2
    Return
}
function f1 name="read" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="shared"
    Return v1
}
)IR"_s;

static void testPropertyCachesAreCreatedAtFirstUse(LoadedProgram& program)
{
    State& state = program.state();
    AOT_TEST_CHECK(!state.propertyCaches.getIfCreated());

    PropertyCaches& caches = *state.propertyCaches;
    AOT_TEST_CHECK(state.propertyCaches.getIfCreated() == &caches);
    AOT_TEST_CHECK(&caches.siteCaches() == state.siteCaches.get());
    AOT_TEST_CHECK(&*state.propertyCaches == &caches && state.propertyCaches->siteCaches().size() == program.instance().siteCaches().size());
}

static PropertyAnswer answerOfKind(PropertyAnswerKind kind)
{
    PropertyAnswer answer;
    answer.kind = kind;
    return answer;
}

// A site is written at its second miss.
static void fileFreeNameTwice(PropertyCaches& caches, SiteCache site, const Identifier& name, const PropertyAnswer& answer)
{
    for (unsigned miss = 0; miss < 2; ++miss)
        caches.cacheFreeName(site, name.impl(), SlotAccess::Read, answer);
}

static void testGlobalObjectReportsChangesOnceAnAnswerDependsOnIt(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    JSGlobalObject* realm = program.realm();
    AOT_TEST_CHECK(!realm->mayBePrototype());

    PropertyCaches& caches = *program.state().propertyCaches;
    ChainDependents& dependents = *program.state().chainDependents;
    AOT_TEST_CHECK(caches.siteCaches().size());
    SiteCache site = caches.siteCaches().at(0);
    Identifier name = Identifier::fromString(vm, "shared"_s);
    AOT_TEST_CHECK(!realm->mayBePrototype());

    PropertyAnswer fixed = answerOfKind(PropertyAnswerKind::Constant);
    fixed.constant = jsNumber(7);
    fileFreeNameTwice(caches, site, name, fixed);
    AOT_TEST_CHECK(site->get().kind == Slot::Kind::Constant && !site->get().has(Slot::dependsOnChain));
    AOT_TEST_CHECK(!realm->mayBePrototype() && !dependents.hasDependents(realm));

    Structure* structureAtStart = realm->structure();
    fileFreeNameTwice(caches, site, name, answerOfKind(PropertyAnswerKind::Absent));
    AOT_TEST_CHECK(site->get().kind == Slot::Kind::Absent && site->get().has(Slot::dependsOnChain));
    AOT_TEST_CHECK(realm->mayBePrototype() && realm->structure() != structureAtStart && dependents.hasDependents(realm));

    realm->putDirect(vm, name, jsNumber(8));
    AOT_TEST_CHECK(site->get().isEmpty() && !dependents.hasDependents(realm));

    Structure* structureThatReports = realm->structure();
    fileFreeNameTwice(caches, site, Identifier::fromString(vm, "other"_s), answerOfKind(PropertyAnswerKind::Absent));
    AOT_TEST_CHECK(realm->structure() == structureThatReports && dependents.hasDependents(realm));
}

static void testStubsFindTheirWordsBeforeFirstUse(LoadedProgram& program)
{
    VM& vm = program.vm();
    Instance& instance = program.instance();
    const Image& image = *vm.aotState().image;
    const PropertyCacheWords& words = instance.propertyCaches;
    AOT_TEST_CHECK(!program.state().propertyCaches.getIfCreated());

    unsigned numberOfHandlers = 0;
    for (SlotHandlerTable table : allSlotHandlerTables) {
        for (Slot::Kind kind : slotKindsWithHandlers) {
            const void* handler = instance.slotHandlers()[static_cast<unsigned>(table)][static_cast<unsigned>(kind)];
            AOT_TEST_CHECK(handler == image.stubAddress(slotHandlerFor(table, kind)));
            numberOfHandlers += !!handler;
        }
    }
    AOT_TEST_CHECK(numberOfHandlers);

    SiteCaches& sites = *program.state().siteCaches;
    AOT_TEST_CHECK(sites.size());
    uintptr_t addressOfFirstSite = std::bit_cast<uintptr_t>(sites.at(0));
    AOT_TEST_CHECK(std::bit_cast<uintptr_t>(words.siteCacheStates) + ((addressOfFirstSite >> SiteCacheState::slotAddressShift) & SiteCacheState::offsetMask) == std::bit_cast<uintptr_t>(&sites.stateOf(sites.at(0))));
    AOT_TEST_CHECK(&sites.stateOf(sites.at(0)) == &sites.stateOf(sites.at(0) + SiteCacheState::numberOfStates) && &sites.stateOf(sites.at(0)) != &sites.stateOf(sites.at(0) + 1));

    AOT_TEST_CHECK(words.hashedSlots && words.front[0].plane && words.front[0].rowsForStructureIDZero);
    AOT_TEST_CHECK(words.hasInstanceUID == vm.propertyNames->hasInstanceSymbol.impl() && words.prototypeUID == vm.propertyNames->prototype.impl());
    AOT_TEST_CHECK(words.defaultHasInstanceFunction == program.realm()->functionProtoHasInstanceSymbolFunction());
}

#if defined(BUN_HARDENED) && OS(UNIX)
static void testWordsWrittenOnceCannotBeWritten(LoadedProgram& program)
{
    Instance& instance = program.instance();
    auto write = [](auto& part) {
        return runInChild([&] {
            auto* byte = reinterpret_cast<volatile uint8_t*>(&part);
            *byte = *byte + 1;
        });
    };
#define AOT_CHECK_WRITE_FINISHES(member, Type) AOT_TEST_CHECK(write(instance.member) == ChildOutcome::Finished);
#define AOT_CHECK_WRITE_CRASHES(member, Type) AOT_TEST_CHECK(write(instance.member) == ChildOutcome::Crashed);
    FOR_EACH_AOT_INSTANCE_PART_BY_KIND(AOT_CHECK_WRITE_FINISHES, AOT_CHECK_WRITE_CRASHES)
#undef AOT_CHECK_WRITE_CRASHES
#undef AOT_CHECK_WRITE_FINISHES
    AOT_TEST_CHECK(write(instance.runtimeTable) == ChildOutcome::Crashed);
    AOT_TEST_CHECK(write(instance.slotHandlers()) == ChildOutcome::Crashed);
    AOT_TEST_CHECK(write(instance.builtinBindings.directFunctions) == ChildOutcome::Crashed);
    AOT_TEST_CHECK(write(instance.builtinBindings.nativeFunctionsOfHost) == ChildOutcome::Crashed);
    AOT_TEST_CHECK(write(instance.functionTables.closureWords) == ChildOutcome::Crashed);
}
#endif

static void fileAnswerUnder(PropertyCaches& caches, Structure* structure, const Identifier& name)
{
    caches.cache({ structure, { name.impl(), std::nullopt }, SlotAccess::Read }, PropertyAnswer::uncacheable(SlotAccess::Read));
}

static void testClassesOfPrimitivesAreNotCreatedAtStart(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    JSGlobalObject* realm = program.realm();
    const PropertyCacheWords& words = program.instance().propertyCaches;
    uint32_t structureIDOfNoReceiver = vm.structureStructure->id().bits();
    AOT_TEST_CHECK(!realm->numberObjectStructureConcurrently() && !realm->booleanObjectStructureConcurrently());
    AOT_TEST_CHECK(words.numberStructureID == structureIDOfNoReceiver && words.booleanStructureID == structureIDOfNoReceiver);

    PropertyCaches& caches = *program.state().propertyCaches;
    AOT_TEST_CHECK(!realm->numberObjectStructureConcurrently() && !realm->booleanObjectStructureConcurrently());
    AOT_TEST_CHECK(!caches.tables().front().hasRow(vm.structureStructure->id()));

    Identifier name = Identifier::fromString(vm, "shared"_s);
    fileAnswerUnder(caches, constructEmptyObject(realm)->structure(), name);
    AOT_TEST_CHECK(words.numberStructureID == structureIDOfNoReceiver && words.booleanStructureID == structureIDOfNoReceiver);

    // While nothing is filed for numbers, a probe has nothing to find under either Structure.
    Structure* numberStructure = realm->numberObjectStructure();
    AOT_TEST_CHECK(words.numberStructureID == structureIDOfNoReceiver);

    fileAnswerUnder(caches, numberStructure, name);
    AOT_TEST_CHECK(words.numberStructureID == numberStructure->id().bits() && words.booleanStructureID == structureIDOfNoReceiver);

    Structure* booleanStructure = realm->booleanObjectStructure();
    fileAnswerUnder(caches, booleanStructure, name);
    AOT_TEST_CHECK(words.numberStructureID == numberStructure->id().bits() && words.booleanStructureID == booleanStructure->id().bits());

    fileAnswerUnder(caches, program.realms().plain()->numberObjectStructure(), name);
    AOT_TEST_CHECK(words.numberStructureID == numberStructure->id().bits());
}

static void testStubsOfFamilyLieSideBySide(LoadedProgram& program)
{
    const Image& image = *program.vm().aotState().image;
    constexpr unsigned numberOfCounts = BoxedCallEntry::maxArgumentCount - BoxedCallEntry::minArgumentCount + 1;
    std::span<const StubAddressInImage> addresses = image.stubAddressesOf(StubFamily::CallBoxedBuiltin);
    AOT_TEST_CHECK(addresses.size() == descriptorOf(StubFamily::CallBoxedBuiltin).numberOfForms * numberOfCounts);
    for (unsigned index = 0; index < addresses.size(); ++index) {
        StubKey key = callBoxedBuiltin(static_cast<BoxedCallForm>(index / numberOfCounts), BoxedCallEntry::minArgumentCount + index % numberOfCounts);
        AOT_TEST_CHECK(addresses[index].key == key.bits());
        AOT_TEST_CHECK(image.stubAddress(key) == image.sharedCodeCopy(0) + addresses[index].offset);
    }

    std::span<const StubAddressInImage> single = image.stubAddressesOf(StubFamily::EnterClosure);
    AOT_TEST_CHECK(single.size() == 1 && single[0].key == StubKey(StubFamily::EnterClosure).bits());
    AOT_TEST_CHECK(AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE || image.stubAddressesOf(StubFamily::WriteBarrier).empty());
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<startupTests, programText, body>;

#if OS(LINUX)
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
static uintptr_t endOfMappingThatHolds(const void* address)
{
    FILE* maps = fopen("/proc/self/maps", "r");
    AOT_TEST_CHECK(maps);
    char line[512];
    uintptr_t result = 0;
    while (!result && fgets(line, sizeof(line), maps)) {
        unsigned long begin = 0;
        unsigned long end = 0;
        if (sscanf(line, "%lx-%lx", &begin, &end) == 2 && begin <= reinterpret_cast<uintptr_t>(address) && reinterpret_cast<uintptr_t>(address) < end)
            result = end;
    }
    fclose(maps);
    return result;
}

// The collector scans the main thread's stack up to this address: too low and it misses the first frames, too high and it reads what is not mapped.
static void testMainThreadStackEndsAboveItsFirstFrame()
{
    char** environment = environ;
    // Pages below the first frame, whatever the size of a page.
    char planted[256 * KB] = "/path/that/a/tracer/keeps/below/the/first/frame";
    bool isMainThread = getpid() == static_cast<pid_t>(syscall(SYS_gettid));
    if (!isMainThread || reinterpret_cast<char*>(environment) < planted)
        return;
    uintptr_t endOfMapping = endOfMappingThatHolds(planted);
    AOT_TEST_CHECK(endOfMapping && endOfMapping == endOfMappingThatHolds(environment));

    auto isRight = [&](StackBounds bounds) {
        uintptr_t origin = reinterpret_cast<uintptr_t>(bounds.origin());
        return origin >= reinterpret_cast<uintptr_t>(environment) && origin <= endOfMapping;
    };
    AOT_TEST_CHECK(isRight(StackBounds::currentThreadStackBoundsForEmbedder()));

    char** end = environment;
    while (*end)
        ++end;
    auto* entry = reinterpret_cast<ElfW(auxv_t)*>(end + 1);
    while (entry->a_type != AT_NULL && entry->a_type != AT_EXECFN)
        ++entry;
    if (entry->a_type != AT_EXECFN || entry->a_un.a_val != getauxval(AT_EXECFN))
        return;
    auto original = entry->a_un.a_val;
    entry->a_un.a_val = reinterpret_cast<uintptr_t>(planted);
    bool isPlanted = getauxval(AT_EXECFN) == reinterpret_cast<uintptr_t>(planted);
    StackBounds boundsUnderTracer = StackBounds::currentThreadStackBoundsForEmbedder();
    entry->a_un.a_val = original;
    AOT_TEST_CHECK(!isPlanted || isRight(boundsUnderTracer));
}

static ProgramHeader segmentOfKind(unsigned kind, uintptr_t address, size_t sizeInFile, size_t sizeInMemory, unsigned flags)
{
    ProgramHeader header { };
    header.p_type = kind;
    header.p_flags = flags;
    header.p_offset = address;
    header.p_vaddr = address;
    header.p_filesz = sizeInFile;
    header.p_memsz = sizeInMemory;
    return header;
}

// Hands the objects over as dl_iterate_phdr() does.
static std::optional<LoadedObject> findObjectAmong(std::span<const LoadedObject> objects, uintptr_t address)
{
    LoadedObjectSearch search(address);
    for (const LoadedObject& object : objects) {
        if (search.visit(object))
            break;
    }
    return search.found();
}

static void testObjectIsFoundWhereverItIsListed()
{
    constexpr size_t page = 4 * KB;
    const ProgramHeader segmentsOfLinker[] = {
        segmentOfKind(PT_LOAD, 0, page, page, PF_R),
        segmentOfKind(PT_LOAD, page, page, page, PF_R | PF_X),
    };
    const ProgramHeader segmentsOfExecutable[] = {
        segmentOfKind(PT_NOTE, 5 * page, page, page, PF_R),
        segmentOfKind(PT_LOAD, 0, page, page, PF_R),
        segmentOfKind(PT_LOAD, page, 2 * page, 2 * page, PF_R | PF_X),
        segmentOfKind(PT_LOAD, 3 * page, page, 2 * page, PF_R | PF_W),
    };
    const ProgramHeader segmentsOfLibrary[] = {
        segmentOfKind(PT_LOAD, 0, 8 * page, 8 * page, PF_R | PF_X),
    };
    const LoadedObject linker { 0x7000000000, segmentsOfLinker };
    const LoadedObject executable { 0x5000000000, segmentsOfExecutable };
    const LoadedObject library { 0x6000000000, segmentsOfLibrary };
    const LoadedObject withoutHeaders;

    std::array<unsigned, 4> order { 0, 1, 2, 3 };
    do {
        const LoadedObject all[] = { executable, linker, library, withoutHeaders };
        const LoadedObject listed[] = { all[order[0]], all[order[1]], all[order[2]], all[order[3]] };
        std::optional<LoadedObject> found = findObjectAmong(listed, executable.loadBias + page + 16);
        AOT_TEST_CHECK(found && found->loadBias == executable.loadBias && found->programHeaders.data() == segmentsOfExecutable);
        AOT_TEST_CHECK(!findObjectAmong(listed, 0x4000000000));

        AOT_TEST_CHECK(found->findSegment(executable.loadBias) == &segmentsOfExecutable[1]);
        AOT_TEST_CHECK(found->findSegment(executable.loadBias + page) == &segmentsOfExecutable[2]);
        AOT_TEST_CHECK(found->findSegment(executable.loadBias + 3 * page - 1) == &segmentsOfExecutable[2]);
        AOT_TEST_CHECK(found->findSegment(executable.loadBias + 4 * page - 1) == &segmentsOfExecutable[3]);
        // Not from the file, not loaded, below the first segment.
        AOT_TEST_CHECK(!found->findSegment(executable.loadBias + 4 * page));
        AOT_TEST_CHECK(!found->findSegment(executable.loadBias + 5 * page));
        AOT_TEST_CHECK(!found->findSegment(executable.loadBias - 1));
        // What another object holds is in no segment of this one.
        AOT_TEST_CHECK(!found->findSegment(linker.loadBias + page + 16));
        AOT_TEST_CHECK(!found->findSegment(library.loadBias + page + 16));
    } while (std::ranges::next_permutation(order).found);
}

static void testLoadedExecutableIsThisProgramAlone()
{
    struct Others {
        const LoadedObject& executable;
        unsigned segmentsTakenForThoseOfExecutable { 0 };
    };
    Others others { loadedExecutable() };
    const ProgramHeader* code = others.executable.findSegment(reinterpret_cast<uintptr_t>(&testLoadedExecutableIsThisProgramAlone));
    AOT_TEST_CHECK(code && code->p_flags & PF_X);
    // The kernel describes the file that it was given. That is the dynamic linker where it was started with the program as its argument.
    if (others.executable.findSegment(getauxval(AT_ENTRY))) {
        AOT_TEST_CHECK(reinterpret_cast<uintptr_t>(others.executable.programHeaders.data()) == getauxval(AT_PHDR));
        AOT_TEST_CHECK(others.executable.programHeaders.size() == getauxval(AT_PHNUM));
    }

    dl_iterate_phdr([](dl_phdr_info* object, size_t, void* data) {
        auto& others = *static_cast<Others*>(data);
        if (!object->dlpi_phdr || object->dlpi_phdr == others.executable.programHeaders.data())
            return 0;
        for (const ProgramHeader& header : std::span { object->dlpi_phdr, object->dlpi_phnum }) {
            if (header.p_type == PT_LOAD && header.p_filesz && others.executable.findSegment(object->dlpi_addr + header.p_vaddr))
                ++others.segmentsTakenForThoseOfExecutable;
        }
        return 0;
    }, &others);
    AOT_TEST_CHECK(!others.segmentsTakenForThoseOfExecutable);
}
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#endif

// Without an image the collector keeps upstream's accounts: what a string borrows counts as memory that the heap holds on to, and so decides when the next collection starts.
static void testBorrowedTextOfStringIsCountedWithoutImage()
{
    if (Image::isRegistered())
        return;
    static Latin1Character text[MB];
    memsetSpan(std::span { text }, 'a');
    Realms realms;
    VM& vm = realms.vm();
    Strong<JSString> string(vm, jsString(vm, String(StringImpl::createWithoutCopying(std::span<const Latin1Character> { text }))));
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(vm.heap.extraMemorySize() >= sizeof(text));
    string.clear();
}

std::span<const NativeTest> startupTests()
{
    static constexpr NativeTest tests[] = {
        { "startup/borrowed-text-of-a-string-is-counted-without-an-image"_s, testBorrowedTextOfStringIsCountedWithoutImage },
#if OS(LINUX)
        { "startup/main-thread-stack-ends-above-its-first-frame"_s, testMainThreadStackEndsAboveItsFirstFrame },
        { "startup/an-object-is-found-wherever-the-process-lists-it"_s, testObjectIsFoundWhereverItIsListed },
        { "startup/the-loaded-executable-is-this-program-alone"_s, testLoadedExecutableIsThisProgramAlone },
#endif
        { "startup/property-caches-are-created-at-first-use"_s, withProgram<testPropertyCachesAreCreatedAtFirstUse> },
        { "startup/global-object-reports-changes-once-an-answer-depends-on-it"_s, withProgram<testGlobalObjectReportsChangesOnceAnAnswerDependsOnIt> },
        { "startup/stubs-find-their-words-before-first-use"_s, withProgram<testStubsFindTheirWordsBeforeFirstUse> },
#if defined(BUN_HARDENED) && OS(UNIX)
        { "startup/words-written-once-cannot-be-written"_s, withProgram<testWordsWrittenOnceCannotBeWritten> },
#endif
        { "startup/classes-of-primitives-are-not-created-at-start"_s, withProgram<testClassesOfPrimitivesAreNotCreatedAtStart> },
        { "startup/stubs-of-a-family-lie-side-by-side"_s, withProgram<testStubsOfFamilyLieSideBySide> },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
