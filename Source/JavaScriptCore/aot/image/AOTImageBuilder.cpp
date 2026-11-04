/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallSiteTable.h"
#include "AOTCodeTemplates.h"
#include "AOTFacts.h"
#include "AOTFaultTable.h"
#include "AOTFunctionTable.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImage.h"
#include "AOTPhaseTimes.h"
#include "AOTPlan.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeTable.h"
#include "AOTSectionWriter.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTStubCatalog.h"
#include "AOTStubDescriptor.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTestRunReportSizes.h"
#include "AOTThreads.h"
#include "MarkedBlockCage.h"
#include <algorithm>
#include <array>
#include <expected>
#include <limits>
#include <optional>
#include <type_traits>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

void reportLinkFailure(LinkFailure failure, FunctionID function, BuildContext& context)
{
    switch (failure) {
    case LinkFailure::ProcessAddressInCode:
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::ProcessAddressInImageCode, .function = function });
        return;
    case LinkFailure::OutOfPCRelativeReach:
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .function = function });
        return;
    case LinkFailure::OutOfMemory:
        context.reportLimitExceeded(LimitID::VectorBytes, 0, { .function = function });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void reportCalleesWithoutCode(const CompiledCode& code, std::span<const OptionalIndex> resolved, BuildContext& context)
{
    BitVector needsCode(resolved.size());
    for (const Relocation& relocation : code.body.relocations) {
        // A relocation that names no callee at all is the linker's to report.
        if (relocation.space == SymbolSpace::Function && relocation.symbol < resolved.size())
            needsCode.quickSet(relocation.symbol);
    }
    for (size_t index : needsCode) {
        if (resolved[index])
            continue;
        CodeUnit callee = code.body.callees[index];
        context.diagnostics().add(InternalError {
            .kind = InternalErrorKind::CallToFunctionWithoutCode,
            .function = code.unit.function,
            .count = callee.function.index(),
            .detail = makeString("its "_s, nameOf(code.unit.role), " calls the "_s, nameOf(callee.role), " of function f"_s, callee.function.index()),
        });
    }
}

ResolvedCallees resolveCallees(const LinkedFunctions& functions, const Typed::Plan& plan, BuildContext& context)
{
    const Typed::CallingConventions& conventions = plan.conventions();
    ResolvedCallees result;
    result.firstCallee.grow(functions.size() + 1);
    uint64_t numberOfCallees = 0;
    for (size_t position = 0; position < functions.size(); ++position) {
        result.firstCallee[position] = static_cast<uint32_t>(numberOfCallees);
        numberOfCallees += functions[position].body.callees.size();
    }
    static_assert(defaultLimitValue(LimitID::VectorBytes, hostArchitecture()) <= std::numeric_limits<uint32_t>::max());
    if (numberOfCallees * sizeof(OptionalIndex) > context.limit(LimitID::VectorBytes)) {
        context.reportLimitExceeded(LimitID::VectorBytes, numberOfCallees * sizeof(OptionalIndex));
        return { };
    }
    result.firstCallee[functions.size()] = static_cast<uint32_t>(numberOfCallees);
    result.positions.fill(OptionalIndex { }, numberOfCallees);
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        const CompiledCode& code = functions[position];
        std::span<OptionalIndex> resolved = result.positions.mutableSpan().subspan(result.firstCallee[position], code.body.callees.size());
        bool hasCalleeWithoutCode = false;
        for (size_t index = 0; index < resolved.size(); ++index) {
            CodeUnit callee = code.body.callees[index];
            // A relocation names the primary entry of a position. Only the word of a closure names a fused call entry.
            bool isFusedCallEntry = callee.role == Typed::CodeRole::GenericCallEntry && conventions.hasFusedCallEntry(callee.function);
            if (std::optional<uint32_t> positionOfCallee = isFusedCallEntry ? std::nullopt : functions.positionOfUnit(callee, conventions.indexOf(callee)))
                resolved[index] = { *positionOfCallee };
            else
                hasCalleeWithoutCode = true;
        }
        if (hasCalleeWithoutCode)
            reportCalleesWithoutCode(code, resolved, context);
    });
    return result;
}

static bool hasAddressInImage(StubLinkage linkage)
{
    switch (linkage) {
    case StubLinkage::EnteredByRuntime:
    case StubLinkage::FrontEnd:
    case StubLinkage::Handler:
        return true;
    case StubLinkage::CalledByCode:
    case StubLinkage::InternalEntry:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

static Vector<uint8_t> encodeStubAddresses(const StubBlob& blob)
{
    Vector<StubAddressInImage> addresses;
    for (auto& placement : blob.placements) {
        StubLinkage linkage = descriptorOf(placement.key.family()).linkage;
        // Only needed where a call pushes its return address: there, walking a stopped thread's stack has to look up the target of a call.
        bool usesCallConvention = isX86_64() && linkage == StubLinkage::CalledByCode && signatureOf(placement.key).usesCallConvention;
        if (usesCallConvention || hasAddressInImage(linkage))
            addresses.append({ placement.key.bits(), placement.offset, usesCallConvention });
    }
    AOT::sortInLine(addresses, [](const StubAddressInImage& a, const StubAddressInImage& b) {
        bool isFirstPart = isFoundByKey(StubKey::fromBits(a.key).family());
        if (isFirstPart != isFoundByKey(StubKey::fromBits(b.key).family()))
            return isFirstPart;
        return a.key < b.key;
    });
    return bytesOf(addresses);
}

static Vector<uint8_t> encodeSlotHandlers(const StubBlob& blob)
{
    Vector<uint32_t> offsets;
    offsets.reserveInitialCapacity(numberOfSlotHandlerTables * numberOfSlotKindsWithHandlers);
    for (SlotHandlerTable table : allSlotHandlerTables) {
        for (Slot::Kind kind : slotKindsWithHandlers)
            offsets.append(blob.offsetOf(slotHandlerFor(table, kind)).value_or(offsetOfAbsentStub));
    }
    return bytesOf(offsets);
}

// A stub that is handed a place and finds no answer there goes on under the name. A site with a Slot is handed its name by the code, so its two bytes say nothing.
static Vector<uint8_t> encodeNamesOfSites(const ImageNumbers& numbers)
{
    const NumberedKeys& sites = numbers.siteCaches();
    std::span<const uint32_t> numbersOfSites = numbers.numbersOfSites();
    Vector<uint16_t> names(FillWith { }, numbersOfSites.back(), 0);
    for (uint32_t place = 0; place < sites.size(); ++place) {
        std::span<const uint32_t> key = sites.keyAt(place);
        if (!ImageNumbers::isPlaceWithSiteAnswers(key))
            continue;
        uint32_t name = numbers.numberOf(NumberSpace::Name, key.subspan(key.size() - 2, 1)).value();
        uint32_t number = numbersOfSites[place];
        names[number] = static_cast<uint16_t>(name);
        if (numbersOfSites[place + 1] - number == 2)
            names[number + 1] = static_cast<uint16_t>(name >> 16);
        else
            RELEASE_ASSERT(name <= UINT16_MAX);
    }
    return bytesOf(names);
}

OptionSet<ImageFlag> imageFlagsAdmittedBy(Wire::UnseenCodeCause cause)
{
    switch (cause) {
    case Wire::UnseenCodeCause::EvaluatesText:
    case Wire::UnseenCodeCause::RunsUnseenCode:
        return { ImageFlag::AdmitsUnknownJavaScript };
    case Wire::UnseenCodeCause::ImportsUncompiledModule:
        return { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::AdmitsLibraryText };
    case Wire::UnseenCodeCause::OptionAdmitsCode:
        return { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::AdmitsLibraryText, ImageFlag::LoadsModulesByName };
    case Wire::UnseenCodeCause::LoadsCodeByName:
        return { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::AdmitsLibraryText, ImageFlag::LoadsModulesByName, ImageFlag::LoadsCodeOutsideImage };
    case Wire::UnseenCodeCause::LoadsModuleByName:
        return { ImageFlag::LoadsModulesByName };
    case Wire::UnseenCodeCause::Unknown:
    case Wire::UnseenCodeCause::FlowResultDisabled:
    case Wire::UnseenCodeCause::RunsMachineCode:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

OptionSet<ImageFlag> flagsForUnseenCode(const Typed::Facts& facts, const CompileOptions&)
{
    OptionSet<ImageFlag> flags;
    for (const auto& record : facts.unseenCode())
        flags.add(imageFlagsAdmittedBy(record.cause));
    // What is proven of the whole program rests on unseenCodeCanRun(). A record that admits code where that answer is no would let in what those proofs rule out.
    RELEASE_ASSERT(facts.unseenCodeCanRun() || !flags.contains(ImageFlag::AdmitsUnknownJavaScript));
    if (facts.unseenCodeCanRun() || facts.unseenScriptsCanRun())
        flags.add(ImageFlag::AdmitsUnknownJavaScript);
    if (facts.findsFilesOfProgramByName())
        flags.add(ImageFlag::FindsDataFilesByName);
    if (facts.storesPathOfMainModule())
        flags.add(ImageFlag::StoresPathOfMainModule);
    if (facts.reachedFunctionsCountUnreadCode())
        flags.add(ImageFlag::RunsUnreadCode);
    const Typed::Program& program = facts.program();
    bool holdsMockOfFile = false;
    bool originalMayRun = false;
    for (const Typed::Variable& variable : program.variables) {
        if (!variable.flags.contains(Typed::VariableFlags::HoldsMock) || program.at(variable.module).runtimeModule)
            continue;
        holdsMockOfFile = true;
        originalMayRun |= variable.flags.contains(Typed::VariableFlags::OriginalMayRun);
    }
    // An executable hands bun:test to a text too, which mocks what the build never saw. The doors that an original opens stay shut there unless a variable says that one may run.
    if (!originalMayRun && (holdsMockOfFile || !program.admitsReplacedBuiltins))
        flags.add(ImageFlag::RefusesOriginalsBesideMocks);
    return flags;
}

namespace {

struct ShapeSetsOfTables {
    Vector<uint8_t> bytes;
    Vector<ConstantRange> parts;
};

struct ImageSections {
    Vector<uint8_t>& operator[](ImageSection section) { return bytes[static_cast<size_t>(section)]; }
    const Vector<uint8_t>& operator[](ImageSection section) const { return bytes[static_cast<size_t>(section)]; }
    const ProgramTables& programTables() const
    {
        RELEASE_ASSERT((*this)[ImageSection::ProgramTables].size() >= sizeof(ProgramTables));
        return *reinterpret_cast<const ProgramTables*>((*this)[ImageSection::ProgramTables].span().data());
    }

    std::array<Vector<uint8_t>, numberOfImageSections> bytes;
};

struct ImageExtent {
    ImageSectionRange operator[](ImageSection section) const { return sections[static_cast<size_t>(section)]; }

    std::array<ImageSectionRange, numberOfImageSections> sections;
    uint64_t tablesEnd { 0 };
    uint64_t codeOffset { 0 };
    uint64_t size { 0 };
};

struct TextRecords {
    std::span<const ImageString> strings;
    std::span<const ImageLongString> longStrings;
    std::span<const ImageIdentifier> identifiers;
};

struct StringHeaderNumbers {
    bool hasRecords() const { return form == ImageForm::PositionIndependent; }
    uint64_t sizeOfHeaders() const { return static_cast<uint64_t>(count) * (hasRecords() ? sizeof(ImageStringRecord) : sizeof(ImageStringHeader)); }
    uint64_t sizeOfList() const { return static_cast<uint64_t>(numberOfListedWords) * sizeof(uint32_t); }

    ImageForm form { ImageForm::Absolute };
    // The number of the header of a string that is no identifier's text, or the first word in the image's list of a string without a header.
    Vector<uint32_t> byString;
    Vector<uint32_t> listedWordIndexes;
    Vector<uint32_t> literalStringWords;
    uint32_t count { 0 };
    uint32_t numberOfListedWords { 0 };
};

struct ImageWriterInputs {
    const Typed::Program& program;
    const Typed::Facts& facts;
    const Typed::Plan& plan;
    const CodeLinkerInputs& linker;
    const ImageSections& sections;
    const WebAssemblyImageInputs& webAssembly;
    const FFIStubs& ffiStubs;
    const WTF::BitSet<numberOfEntries>& entriesUsed;
    ImageCounts counts;
    std::span<const StubReturnSite> stubReturnSites;
};

} // anonymous namespace

// A step in a lane has a line by the clock alone, under this name. The line of the stage, which the sum of the stage counts, is that of all lanes together.
static constexpr ASCIILiteral nameOfLaneSteps = "image: lanes"_s;

static bool checksImageWhileBuilding(const BuildContext& context)
{
    return ASSERT_ENABLED || ASAN_ENABLED || context.options().verifiesMachineCode;
}

// The sets of shapes that stubs and C++ read are the last thing in the program tables: TablePartID::ShapeSets.
static ShapeSetsOfTables takeShapeSets(Vector<uint8_t>& programTables)
{
    RELEASE_ASSERT(programTables.size() >= sizeof(ProgramTables));
    auto& tables = *reinterpret_cast<ProgramTables*>(programTables.mutableSpan().data());
    TableRange& range = tables.ranges[static_cast<unsigned>(ProgramTableIndex::shapeSetBits)];
    ShapeSetsOfTables sets;
    if (!range.count)
        return sets;
    std::span<const uint8_t> bytes = asBytes(tables.shapeSetBits());
    if (range.offset + bytes.size() != programTables.size())
        return sets;
    sets.bytes = Vector<uint8_t>(bytes);
    for (const LayoutSetData& set : tables.layoutSets()) {
        uint32_t location = set.members.bitSetOffset;
        if (location == noBitSet || PagedBitSet::entriesAt(location) != PagedBitSet::Entries::None)
            continue;
        uint64_t offset = static_cast<uint64_t>(PagedBitSet::firstWordAt(location)) * sizeof(uint64_t);
        uint64_t size = Typed::numberOfPagesInShapeSet(set.members.count) * sizeof(uint64_t);
        RELEASE_ASSERT(isSumSmallerThanOrEqual<uint64_t>(offset, size, bytes.size()));
        if (!(offset % alignmentOfConstant(size)))
            sets.parts.append({ static_cast<uint32_t>(offset), static_cast<uint32_t>(size) });
    }
    AOT::sortInLine(sets.parts, [](const ConstantRange& a, const ConstantRange& b) {
        return a.offset != b.offset ? a.offset < b.offset : a.size < b.size;
    });
    removeRepeatedElements(sets.parts, [](const ConstantRange& a, const ConstantRange& b) {
        return a.offset == b.offset && a.size == b.size;
    });
    uint32_t offset = range.offset;
    range = { 0, 0 };
    programTables.shrink(offset);
    return sets;
}

// FunctionNumbers::numberOfFunctions(), which follows from the units that have code.
static size_t countFunctionsInImage(const Typed::Program& program, const LinkedFunctions& functions)
{
    size_t count = program.functions.size() + functions.fusedCallEntries().size();
    for (const CompiledCode& code : functions)
        count += code.unit.role != Typed::CodeRole::Body;
    return count;
}

static Vector<uint32_t> measureSideTablesOfFunctions(const SideTables& sideTables, std::span<const Vector<uint8_t>> callSites, bool hasPositions, const ParallelLoops& loops)
{
    size_t numberOfFunctions = callSites.size();
    RELEASE_ASSERT(sideTables.offsets.size() == numberOfFunctions && sideTables.flags.size() == numberOfFunctions);
    Vector<uint32_t> sizes(FillWith { }, numberOfFunctions, 0);
    const uint8_t* streams = sideTables.bytes.span().data();
    const uint8_t* positions = sideTables.positions.span().data();
    size_t numberOfGroups = PositionIndex::numberOfEntriesFor(numberOfFunctions);
    forEachIndexInParallel(loops, static_cast<unsigned>(numberOfGroups), [&](unsigned group) {
        const uint8_t* cursor = nullptr;
        if (hasPositions) {
            uint32_t placeOfFirstEntry;
            memcpy(&placeOfFirstEntry, positions + group * sizeof(uint32_t), sizeof(uint32_t));
            cursor = positions + numberOfGroups * sizeof(uint32_t) + placeOfFirstEntry;
        }
        size_t first = static_cast<size_t>(group) * PositionIndex::functionsBetween;
        size_t end = std::min(numberOfFunctions, first + PositionIndex::functionsBetween);
        for (size_t position = first; position < end; ++position) {
            SideTableParts parts = SideTableParts::read(ImageFunction::create(sideTables.offsets[position], 0, sideTables.flags[position]), streams);
            uint64_t size = parts.frameRanges.size() + parts.handlers.size() + callSites[position].size();
            if (cursor) {
                const uint8_t* entry = cursor;
                PositionsHead head = PositionsHead::read(cursor);
                cursor += head.size;
                size += cursor - entry;
            }
            sizes[position] = clampTo<uint32_t>(size);
        }
    });
    return sizes;
}

// The strings in the order in which their text lies, which is by the first function that names each, and behind them every other id.
static Vector<uint32_t> listStringsInOrderOfText(size_t numberOfStrings, std::span<const StringID> stringsInOrderOfText)
{
    Vector<uint32_t> order;
    order.reserveInitialCapacity(numberOfStrings);
    BitVector isListed(numberOfStrings);
    for (StringID string : stringsInOrderOfText) {
        isListed.quickSet(string.index());
        order.append(string.index());
    }
    for (uint32_t index = 0; index < numberOfStrings; ++index) {
        if (!isListed.quickGet(index))
            order.append(index);
    }
    return order;
}

// THE HEADERS AND THE LIST LIE IN THE ORDER OF THE TEXT. The ids of strings follow the hashes of their bytes, so in the order of the ids what one function names lies all over both,
// and each string that a program makes at its start waits for a line of memory of its own.
static std::optional<StringHeaderNumbers> numberStringHeaders(const TextRecords& text, std::span<const StringID> stringsInOrderOfText, std::span<const uint32_t> literalStrings, ImageForm form, BuildContext& context)
{
    StringHeaderNumbers numbers;
    numbers.form = form;
    numbers.byString.fill(0, text.strings.size());
    if (numbers.hasRecords()) {
        numbers.listedWordIndexes.fill(0, text.strings.size());
        numbers.literalStringWords.fill(0, text.strings.size());
        for (uint32_t number = 0; number < literalStrings.size(); ++number)
            numbers.literalStringWords[literalStrings[number]] = literalStringWord(number);
    }
    uint64_t count = text.identifiers.size();
    uint64_t numberOfListedWords = 0;
    for (size_t index : listStringsInOrderOfText(text.strings.size(), stringsInOrderOfText)) {
        const ImageString& string = text.strings[index];
        bool needsRecord = numbers.hasRecords() && string.hasHeader() && string.range(text.longStrings).length;
        if (needsRecord && (numbers.literalStringWords[index] || !ImageListedString::fitsOneWord(string.range(text.longStrings)))) {
            numbers.listedWordIndexes[index] = static_cast<uint32_t>(numberOfListedWords);
            numberOfListedWords += ImageListedString::numberOfWords(string.range(text.longStrings)) + !!numbers.literalStringWords[index];
        }
        if (!string.hasText() || string.hasIdentifierNumber())
            continue;
        if (string.hasHeader()) {
            numbers.byString[index] = static_cast<uint32_t>(count++);
            continue;
        }
        numbers.byString[index] = static_cast<uint32_t>(numberOfListedWords);
        numberOfListedWords += ImageListedString::numberOfWords(string.range(text.longStrings)) + string.hasListedHash();
    }
    static_assert(defaultLimitValue(LimitID::MaxListedStrings, hostArchitecture()) == ImageString::hashMask + 1);
    if (std::max(count, numberOfListedWords) > ImageString::hashMask + 1) {
        context.reportLimitExceeded(LimitID::MaxListedStrings, std::max(count, numberOfListedWords));
        return std::nullopt;
    }
    numbers.count = static_cast<uint32_t>(count);
    numbers.numberOfListedWords = static_cast<uint32_t>(numberOfListedWords);
    return numbers;
}

static std::optional<ImageExtent> computeImageExtent(const ImageSections& sections, const StringHeaderNumbers& stringHeaders, uint64_t codeSize, BuildContext& context)
{
    RELEASE_ASSERT(sections[ImageSection::StringHeaders].isEmpty());
    ImageExtent extent;
    uint64_t tablesEnd = sizeof(ImageHeader);
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        uint64_t size = sections.bytes[index].size();
        if (static_cast<ImageSection>(index) == ImageSection::StringHeaders)
            size = stringHeaders.sizeOfHeaders();
        else if (static_cast<ImageSection>(index) == ImageSection::Strings)
            size = size / sizeof(ImageString) * sizeof(ImageStringWord);
        else if (static_cast<ImageSection>(index) == ImageSection::LongStrings)
            size = stringHeaders.sizeOfList();
        else if (static_cast<ImageSection>(index) == ImageSection::Identifiers)
            size = size / sizeof(ImageIdentifier) * sizeof(ImageIdentifierWord);
        // An instance has the pages of the method table for its own until it writes to one: mapPrivateCloneOfImagePages().
        bool isClonedByPages = static_cast<ImageSection>(index) == ImageSection::MethodTable && sections.programTables().instanceSizes.sizeOfMethodTable;
        tablesEnd = isClonedByPages ? WTF::roundUpToMultipleOf<imagePageSize>(tablesEnd) : WTF::roundUpToMultipleOf<imageSectionAlignment>(tablesEnd);
        // An offset that does not fit is never read: the image is then too large for one of the two tests below.
        extent.sections[index] = { static_cast<uint32_t>(tablesEnd), static_cast<uint32_t>(size) };
        tablesEnd += size;
    }
    if (tablesEnd > context.limit(LimitID::TableBytes)) {
        context.reportLimitExceeded(LimitID::TableBytes, tablesEnd);
        return std::nullopt;
    }
    extent.tablesEnd = tablesEnd;
    extent.codeOffset = WTF::roundUpToMultipleOf<imagePageSize>(tablesEnd);
    extent.size = WTF::roundUpToMultipleOf<imagePageSize>(extent.codeOffset + codeSize);
    static_assert(WTF::isValidCapacityForVector<uint8_t>(defaultLimitValue(LimitID::VectorBytes, hostArchitecture())));
    if (extent.size > context.limit(LimitID::VectorBytes)) {
        context.reportLimitExceeded(LimitID::VectorBytes, extent.size);
        return std::nullopt;
    }
    return extent;
}

static constexpr size_t bytesInChunkOfImage = MB;

static Vector<uint8_t> allocateImage(const ImageExtent& extent, size_t executableSize, const CodeTemplates& templates, const ParallelLoops& loops)
{
    static_assert(!(bytesInChunkOfImage % codeOffsetUnit) && !(imagePageSize % codeOffsetUnit));
    Vector<uint8_t> image;
    image.grow(extent.size);
    forEachIndexInParallel(loops, static_cast<unsigned>((image.size() + bytesInChunkOfImage - 1) / bytesInChunkOfImage), [&](unsigned i) {
        size_t start = i * bytesInChunkOfImage;
        size_t end = std::min(start + bytesInChunkOfImage, image.size());
        size_t codeStart = std::clamp<size_t>(extent.codeOffset, start, end);
        size_t codeEnd = std::clamp<size_t>(extent.codeOffset + executableSize, start, end);
        zeroSpan(image.mutableSpan().subspan(start, codeStart - start));
        templates.fillWithBreakpoints(image.mutableSpan().subspan(codeStart, codeEnd - codeStart));
        zeroSpan(image.mutableSpan().subspan(codeEnd, end - codeEnd));
    });
    return image;
}

template<typename T>
static constexpr bool hasNoPadding = std::has_unique_object_representations_v<T>;

static void writeHeader(ImageHeader& header, const ImageWriterInputs& parts, const ImageExtent& extent, const StringHeaderNumbers& stringHeaders, uint32_t endOfIdentifiers, BuildContext& context)
{
    static_assert(hasNoPadding<RecordedOptions> && hasNoPadding<ImageSectionRange> && hasNoPadding<ImageCounts> && hasNoPadding<ImageCodeRanges>, "padding in a member assigned as a whole would make the bytes of the image differ from build to build");
    static_assert(sizeof(ImageHeapOptions) == sizeof(double) + 2 * sizeof(uint32_t), "a double has no unique object representation, so the sizes say that there is no padding");
    const CodeLayout& layout = parts.linker.layout;
    const FunctionNumbers& functionNumbers = parts.linker.numbers.functions();
    header.magic = imageMagic;
    header.identity = context.options().executableIdentity;
    header.stamp = imageStamp(context.targetFeatures(), header.identity, context.recordedOptions().form());
    header.size = extent.size;
    header.codeOffset = extent.codeOffset;
    header.codeSize = layout.codeSize;
    header.address = 0;
    header.executableSize = layout.executableSize;
    header.formatVersion = imageFormatVersion;
    header.options = context.recordedOptions();
    header.heap = { context.options().sizeClassProgression, MarkedBlockCage::shift, 0 };
    std::ranges::copy(extent.sections, header.sections);
    header.counts = parts.counts;
    header.code = {
        .functionsEnd = layout.functionsEnd,
        .codeGranuleShift = layout.codeGranuleShift,
        .sharedCodeSize = layout.sharedCodeSize,
        .webAssemblyCodeOffset = layout.webAssemblyCodeOffset,
        .webAssemblyCodeSize = safeCast<uint32_t>(parts.webAssembly.code.size()),
        .ffiCodeOffset = layout.ffiCodeOffset,
        .ffiCodeSize = safeCast<uint32_t>(parts.ffiStubs.code.size()),
        .regExpCodeOffset = layout.regExpCodeOffset,
        .constantsOffset = layout.constantsOffset,
        .constantsSize = safeCast<uint32_t>(parts.linker.constants.bytes.size()),
    };
    OptionSet<ImageFlag> flags = flagsForUnseenCode(parts.facts, context.options());
    if (context.options().obfuscates())
        flags.add(ImageFlag::IsObfuscated);
    header.numbers = {
        .lastShape = parts.plan.shapes().numberOfShapes(),
        .endOfClosureNumbers = functionNumbers.endOfClosureFunctions().index() - functionNumbers.firstClosureFunction().index(),
        .endOfIdentifiers = endOfIdentifiers,
        .endOfStringHeaders = stringHeaders.count,
    };
    if (Typed::namesWaysToModuleObjects(parts.program))
        flags.add(ImageFlag::ShowsWaysBetweenModuleObjects);
    if (std::ranges::any_of(parts.program.modules, [](const Typed::Module& module) { return module.kind != Typed::ModuleKind::CommonJs && module.flags.contains(Typed::ModuleFlags::RunsAgain); }))
        flags.add(ImageFlag::RunsEntryPointsAgain);
    header.flags = flags.toRaw();
    for (size_t entry : parts.entriesUsed)
        header.entriesUsed[entry / 32] |= 1u << entry % 32;
    for (const StubReturnSite& site : parts.stubReturnSites) {
        if (!hasSingleReturnSite(site.kind))
            continue;
        uint32_t& offset = header.singleReturnSites[static_cast<unsigned>(site.kind)];
        RELEASE_ASSERT(!offset && site.offset);
        offset = site.offset;
    }
    header.numberOfStubsFoundByKey = static_cast<uint32_t>(std::ranges::count_if(spanReinterpretCast<const StubAddressInImage>(parts.sections[ImageSection::StubAddresses].span()), [](const StubAddressInImage& address) {
        return isFoundByKey(StubKey::fromBits(address.key).family());
    }));
}

static void copySections(std::span<uint8_t> image, const ImageWriterInputs& parts, const ImageExtent& extent, const ParallelLoops& loops)
{
    struct CopyRange {
        size_t offset;
        std::span<const uint8_t> source;
    };
    const CodeLayout& layout = parts.linker.layout;
    Vector<CopyRange> chunks;
    auto addChunks = [&](uint64_t offset, std::span<const uint8_t> source) {
        RELEASE_ASSERT(isSumSmallerThanOrEqual<uint64_t>(offset, source.size(), image.size()));
        for (size_t start = 0; start < source.size(); start += bytesInChunkOfImage)
            chunks.append({ static_cast<size_t>(offset) + start, source.subspan(start, std::min(bytesInChunkOfImage, source.size() - start)) });
    };
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        // writeStrings() makes the three of the encoder's records.
        if (static_cast<ImageSection>(index) != ImageSection::Strings && static_cast<ImageSection>(index) != ImageSection::LongStrings && static_cast<ImageSection>(index) != ImageSection::Identifiers)
            addChunks(extent.sections[index].offset, parts.sections.bytes[index].span());
    }
    addChunks(extent.codeOffset + layout.regExpCodeOffset, parts.linker.layoutInputs.regExpSection.bytes.span());
    addChunks(extent.codeOffset + layout.webAssemblyCodeOffset, parts.webAssembly.code.span());
    addChunks(extent.codeOffset + layout.ffiCodeOffset, parts.ffiStubs.code.span());
    addChunks(extent.codeOffset + layout.ffiDirectoryOffset, parts.ffiStubs.directory.span());
    forEachIndexInParallel(loops, chunks.size(), [&](unsigned index) {
        memcpySpan(image.subspan(chunks[index].offset), chunks[index].source);
    });
}

template<typename T>
static std::span<T> sectionIn(std::span<uint8_t> image, const ImageExtent& extent, ImageSection section)
{
    return spanReinterpretCast<T>(image.subspan(extent[section].offset, extent[section].size));
}

static void writeStrings(std::span<uint8_t> image, const TextRecords& text, const StringHeaderNumbers& numbers, const ImageExtent& extent, const ParallelLoops& loops)
{
    std::span<ImageStringRecord> records;
    std::span<ImageStringHeader> headers;
    if (numbers.hasRecords())
        records = sectionIn<ImageStringRecord>(image, extent, ImageSection::StringHeaders);
    else
        headers = sectionIn<ImageStringHeader>(image, extent, ImageSection::StringHeaders);
    std::span stringsInImage = sectionIn<ImageStringWord>(image, extent, ImageSection::Strings);
    std::span listedStrings = sectionIn<uint32_t>(image, extent, ImageSection::LongStrings);
    std::span identifiersInImage = sectionIn<ImageIdentifierWord>(image, extent, ImageSection::Identifiers);
    for (size_t number = 0; number < text.identifiers.size(); ++number)
        identifiersInImage[number] = text.identifiers[number].inImage();
    forEachIndexInParallel(loops, static_cast<unsigned>(text.strings.size()), [&](unsigned index) {
        const ImageString& string = text.strings[index];
        stringsInImage[index] = string.inImage(numbers.byString[index]);
        if (!string.hasText())
            return;
        ImageLongString range = string.range(text.longStrings);
        if (range.length < 2)
            stringsInImage[index].bits |= ImageString::isShorterThanTwoFlag;
        if (!string.hasHeader()) {
            ImageListedString::write(listedStrings.subspan(numbers.byString[index]), range);
            if (string.hasListedHash())
                listedStrings[numbers.byString[index] + ImageListedString::numberOfWords(range)] = string.hashOrIdentifierNumber();
            return;
        }
        bool isIdentifier = string.hasIdentifierNumber();
        uint32_t number = isIdentifier ? string.hashOrIdentifierNumber() : numbers.byString[index];
        if (isIdentifier)
            RELEASE_ASSERT(text.identifiers[number].kind() == PropertyKeyKind::Text && text.identifiers[number].text() == StringID(index));
        uint32_t hash = isIdentifier ? text.identifiers[number].hashOrSymbol() : string.hashOrIdentifierNumber();
        if (!numbers.hasRecords()) {
            // The atom with no characters is the engine's own.
            if (range.length || !isIdentifier)
                headers[number] = ImageStringHeader::create(static_cast<uint64_t>(extent[ImageSection::Text].offset) + range.offset, range.length, hash, string.is16Bit(), isIdentifier ? StringImpl::StringAtom : StringImpl::StringNormal);
            return;
        }
        if (!range.length)
            return;
        uint32_t literalStringWord = numbers.literalStringWords[index];
        uint32_t rangeOrIndex = numbers.listedWordIndexes[index] << ImageString::lengthBits;
        if (!literalStringWord && ImageListedString::fitsOneWord(range))
            ImageListedString::write(std::span { &rangeOrIndex, 1 }, range);
        else {
            std::span words = listedStrings.subspan(numbers.listedWordIndexes[index]);
            ImageListedString::write(words, range);
            if (literalStringWord)
                words[ImageListedString::numberOfWords(range)] = literalStringWord;
        }
        records[number] = ImageStringRecord::create(rangeOrIndex, hash, string.is16Bit(), isIdentifier);
        if (literalStringWord)
            records[number].hashAndFlags |= ImageStringRecord::isLiteralStringFlag;
    });
}

// ImageSection::Strings goes by the ids of the strings, which follow the hashes of their bytes. The constants are numbered by the first function that names each, so here what one function names lies side by side.
static void writeWordsOfStringConstants(std::span<uint8_t> image, const ImageExtent& extent)
{
    std::span stringsInImage = sectionIn<ImageStringWord>(image, extent, ImageSection::Strings);
    std::span constants = sectionIn<uint32_t>(image, extent, ImageSection::StringConstants);
    size_t count = constants.size() / 2;
    for (size_t number = 0; number < count; ++number)
        constants[number] = stringsInImage[constants[count + number]].bits;
}

static void writeLiteralStringWords(std::span<ImageStringHeader> headers, const TextRecords& text, const StringHeaderNumbers& numbers, std::span<const uint32_t> literalStrings)
{
    RELEASE_ASSERT(!headers.empty() && !headers[0].length);
    headers[0].literalStringWord = static_cast<uint32_t>(numbers.count * sizeof(ImageStringHeader));
    for (uint32_t number = 0; number < literalStrings.size(); ++number) {
        const ImageString& string = text.strings[literalStrings[number]];
        if (!string.hasText() || !string.hasHeader())
            continue;
        uint32_t numberOfHeader = string.hasIdentifierNumber() ? string.hashOrIdentifierNumber() : numbers.byString[literalStrings[number]];
        ImageStringHeader& header = headers[numberOfHeader];
        if (header.length)
            header.literalStringWord = literalStringWord(number);
    }
}

static void writeRangeOfShapeSets(std::span<uint8_t> image, const CodeLinkerInputs& linker, const ImageExtent& extent)
{
    uint32_t size = linker.constants.sizeOfSharedConstants;
    if (!size)
        return;
    ImageSectionRange section = extent[ImageSection::ProgramTables];
    RELEASE_ASSERT(section.size >= sizeof(ProgramTables));
    auto& tables = *reinterpret_cast<ProgramTables*>(image.subspan(section.offset).data());
    uint64_t distance = extent.codeOffset + linker.layout.constantsOffset - section.offset;
    RELEASE_ASSERT(distance <= std::numeric_limits<uint32_t>::max() && !(distance % sizeof(uint64_t)));
    tables.ranges[static_cast<unsigned>(ProgramTableIndex::shapeSetBits)] = { static_cast<uint32_t>(distance), static_cast<uint32_t>(size / sizeof(uint64_t)) };
}

// The pages of an image are shared between processes: none is written, and none but those of the string headers holds an address. The run time maps memory of its own over the callback context.
static void checkImageIsReadOnly(std::span<const uint8_t> code, const FFIStubs& ffiStubs, const CodeLayout& layout, BuildContext& context)
{
    uint64_t offset = static_cast<uint64_t>(layout.ffiCodeOffset) + ffiStubs.callbackContextOffset;
    uint64_t size = ffiStubs.callbackContextSize;
    if (!size)
        return;
    bool isEmptyPageRange = !(offset % imagePageSize) && !(size % imagePageSize) && isSumSmallerThanOrEqual<uint64_t>(offset, size, code.size())
        && std::ranges::all_of(code.subspan(offset, size), [](uint8_t byte) { return !byte; });
    if (!isEmptyPageRange)
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::WritableSectionInImage, .count = clampTo<uint32_t>(offset) });
}

static Vector<uint8_t> writeImage(const ImageWriterInputs& parts, PhaseTimes& times, BuildContext& context)
{
    const CodeLayout& layout = parts.linker.layout;
    const ImageSections& sections = parts.sections;
    TextRecords text {
        spanReinterpretCast<const ImageString>(sections[ImageSection::Strings].span()),
        spanReinterpretCast<const ImageLongString>(sections[ImageSection::LongStrings].span()),
        spanReinterpretCast<const ImageIdentifier>(sections[ImageSection::Identifiers].span()),
    };
    std::span<const uint32_t> literalStrings = sections.programTables().literalStrings();
    std::optional<StringHeaderNumbers> stringHeaders = numberStringHeaders(text, parts.linker.numbers.stringsInOrderOfText(), literalStrings, context.recordedOptions().form(), context);
    if (!stringHeaders)
        return { };
    std::optional<ImageExtent> extent = computeImageExtent(sections, *stringHeaders, layout.codeSize, context);
    if (!extent)
        return { };

    const ParallelLoops& loops = context.parallelLoops();
    Vector<uint8_t> image = allocateImage(*extent, layout.executableSize, parts.linker.layoutInputs.templates, loops);
    writeHeader(*reinterpret_cast<ImageHeader*>(image.mutableSpan().data()), parts, *extent, *stringHeaders, static_cast<uint32_t>(text.identifiers.size()), context);
    times.didFinish("allocate image, write header"_s);

    copySections(image.mutableSpan(), parts, *extent, loops);
    times.didFinish("copy sections"_s);

    writeStrings(image.mutableSpan(), text, *stringHeaders, *extent, loops);
    writeWordsOfStringConstants(image.mutableSpan(), *extent);
    if (!stringHeaders->hasRecords())
        writeLiteralStringWords(sectionIn<ImageStringHeader>(image.mutableSpan(), *extent, ImageSection::StringHeaders), text, *stringHeaders, literalStrings);
    writeRangeOfShapeSets(image.mutableSpan(), parts.linker, *extent);
    times.didFinish("write strings"_s);

    linkCode(parts.linker, image.mutableSubspan(extent->codeOffset, layout.codeSize), context);
    times.didFinish("link code"_s);

    checkImageIsReadOnly(image.subspan(extent->codeOffset, layout.codeSize), parts.ffiStubs, layout, context);
    if (context.diagnostics().hasErrors())
        return { };
    // Without the padding up to the page where the code begins.
    context.statistics().count(StatisticID::TableBytesEmitted, extent->tablesEnd);
    context.statistics().count(StatisticID::TextBytesEmitted, (*extent)[ImageSection::Text].size);
    return image;
}

BuiltImage buildImage(ImageInputs&& inputs, BuildContext& context)
{
    Diagnostics& diagnostics = context.diagnostics();
    PhaseTimes times(context, Stage::BuildImage);
    Typed::Program& program = inputs.program;
    BuiltImage result;
    result.functions = WTF::move(inputs.functions);
    const LinkedFunctions& functions = result.functions;

    std::expected<CodeTemplates, LinkFailure> createdTemplates = CodeTemplates::create(context.assemblerPolicy(), &context);
    if (!createdTemplates) {
        reportLinkFailure(createdTemplates.error(), { }, context);
        return result;
    }
    const CodeTemplates& templates = *createdTemplates;
    times.didFinish("create code templates"_s);

    TableInputs tableInputs { program, inputs.facts, inputs.plan, functions, inputs.bodyFacts, context.options(), &context };
    result.numbers = makeUniqueWithoutFastMallocCheck<ImageNumbers>();
    ImageNumbers& numbers = *result.numbers;

    // Both steps of the first lane add strings to the program, so one follows the other. Of a body they write the words of its mentions and the texts it made. The second lane reads its callees and relocations, and no string.
    ResolvedCallees callees;
    runLanes(context.parallelLoops(), {
        [&] {
            StepTimes steps(context, nameOfLaneSteps);
            if (context.options().obfuscates())
                hidePathsOfModules(program, inputs.facts, inputs.plan, context);
            internPiecesOfModulePaths(program);
            steps.didFinish("hide paths of modules, intern pieces of paths"_s);
            internTextsMadeByFunctions(program, result.functions, context);
            steps.didFinish("intern texts made by functions"_s);
        },
        [&] {
            StepTimes steps(context, nameOfLaneSteps);
            callees = resolveCallees(functions, inputs.plan, context);
            steps.didFinish("resolve callees"_s);
        },
    });
    times.didFinish("lanes in front of the mentions"_s);
    if (diagnostics.hasErrors())
        return result;

    std::optional<CodeIndex> index;
    unsigned codeGranuleShift = 0;
    FFIStubs ffiStubs;
    WebAssemblyImageInputs webAssembly;
    Vector<FinalJump> finalJumps;
    Vector<FarInstanceWords> farInstanceWords;
    Vector<uint8_t> programTables;
    ConstantPool constants;
    CompiledRegExps compiledRegExps;
    RegExpSection regExpSection;
    RegExpSections regExps;
    EncodedText text;
    Vector<Vector<uint8_t>> callSites;
    SideTables sideTables;
    const bool hasPositions = !context.options().obfuscates();
    const size_t numberOfFunctionsInImage = countFunctionsInImage(program, functions);

    auto compileRegExpsOfImage = [&] {
        StepTimes steps(context, nameOfLaneSteps);
        compiledRegExps = compileRegExps(program, numbers.regExpRequests(), context);
        steps.didFinish("compile regular expressions"_s);
    };
    auto layOutAndDescribeRegExps = [&] {
        StepTimes steps(context, nameOfLaneSteps);
        std::optional<RegExpSection> section = layOutRegExpSection(compiledRegExps.results.span(), context.assemblerPolicy(), templates, &context);
        if (!section)
            return;
        regExpSection = WTF::move(*section);
        regExps = encodeRegExpRecords({ numbers.regExpRequests(), compiledRegExps, regExpSection }, context);
        steps.didFinish("lay out and describe regular expressions"_s);
    };

    // The numbering leaves most threads idle. What asks for no number, and for nothing of a function that applyNumbersToFunctions() writes, runs beside it.
    NumberingInputs numberingInputs { program, inputs.facts, inputs.plan, functions };
    std::optional<GatheredMentions> mentions;
    std::optional<NamesOfTables> namesOfTables;
    runLanes(context.parallelLoops(), {
        [&] {
            mentions = gatherMentions(numberingInputs, context);
            times.didFinish("gather mentions"_s);
            if (diagnostics.hasErrors())
                return;

            namesOfTables = nameWhatTablesName(tableInputs, *mentions, context);
            times.didFinish("name what the tables name"_s);
            if (diagnostics.hasErrors())
                return;

            numberWhatStaticsWaitFor(numberingInputs, *mentions, *namesOfTables, numbers, context, scopedLambda<void()>(compileRegExpsOfImage));
            times.didFinish("number"_s);
        },
        [&] {
            StepTimes steps(context, nameOfLaneSteps);
            callSites = encodeCallSites(functions, numberOfFunctionsInImage, context);
            steps.didFinish("encode call sites"_s);
            if (diagnostics.hasErrors())
                return;
            // Of a body they read neither the bytes nor the relocations, which is all that removeFinalJumps() changes. So they do not wait for the layout.
            sideTables = encodeSideTables(functions, callSites.span(), context);
            steps.didFinish("encode side tables"_s);
            if (diagnostics.hasErrors())
                return;
            result.sideTableBytes = measureSideTablesOfFunctions(sideTables, callSites.span(), hasPositions, context.parallelLoops());
            steps.didFinish("measure side tables"_s);
        },
        [&] {
            StepTimes steps(context, nameOfLaneSteps);
            codeGranuleShift = chooseCodeGranuleShift(functions, context);
            steps.didFinish("choose code granules"_s);
            finalJumps = findFinalJumps(functions, callees, context);
            steps.didFinish("find final jumps"_s);
        },
    });
    times.didFinish("lanes beside the numbering"_s);
    if (diagnostics.hasErrors())
        return result;
    RELEASE_ASSERT(numbers.functions().numberOfFunctions() == numberOfFunctionsInImage);

    // The text asks which strings the image holds and in which order, and the regular expressions ask for their requests. Neither waits for the statics or for the numbers in the functions.
    auto runLanesBehindNumbersInFunctions = [&] {
    // Each lane reads the functions, the program and the numbers as they stand here, and nothing that another lane makes.
    // The lane of the tables alone writes to the numbers: the offsets of types and the nodes of data, which no other lane asks for.
    // The first lane makes a VM, so it is the one that stays on this thread, and it is short, so this thread is soon free for a lane that the pool has not begun. Behind it the longest come first.
    runLanes(context.parallelLoops(), {
        [&] {
            StepTimes steps(context, nameOfLaneSteps);
            index.emplace(functions, &context.parallelLoops());
            steps.didFinish("index functions"_s);
            if (mayUseFFI(inputs.facts, context.options()))
                ffiStubs = generateFFIStubs(context);
            steps.didFinish("generate FFI stubs"_s);
            webAssembly = compileWebAssemblyModules(program, templates, context, &numbers.namedStrings());
            steps.didFinish("compile WebAssembly modules"_s);
            farInstanceWords = findFarInstanceWords(functions, numbers, context);
            steps.didFinish("find far words of the instance"_s);
            if (checksImageWhileBuilding(context)) {
                checkUncheckedStackUsage(functions, callees, context);
                steps.didFinish("check unchecked stack usage"_s);
            }
        },
        [&] {
            if (diagnostics.hasErrors())
                return;
            StepTimes steps(context, nameOfLaneSteps);
            programTables = encodeProgramTables(tableInputs, numbers, result.bytesOfTableParts, context);
            steps.didFinish("encode program tables"_s);
            if (diagnostics.hasErrors())
                return;
            // The pool begins with the sets of shapes, so it waits for the tables.
            ShapeSetsOfTables shapeSets = takeShapeSets(programTables);
            constants = buildConstantPool(functions, context, { shapeSets.bytes.span(), shapeSets.parts.span() });
            steps.didFinish("build constant pool"_s);
        },
        [&] {
            if (diagnostics.hasErrors())
                return;
            StepTimes steps(context, nameOfLaneSteps);
            // The stubs compare a name with the last column of the table of fields, which only the numbering knows.
            StubOptions stubOptions = inputs.stubCatalog.options();
            stubOptions.maxNameInFieldTable = numbers.identifiers().countWithFieldTableColumn() - 1;
            StubPolicy stubPolicy = stubPolicyOf(inputs.plan.slots());
            const StubCatalog stubCatalogOfImage(stubOptions, WTF::move(stubPolicy));
            steps.didFinish("create stub catalog"_s);
            std::optional<CalledSharedCode> calledSharedCode = collectCalledSharedCode(functions, context);
            steps.didFinish("collect called shared code"_s);
            if (!calledSharedCode || diagnostics.hasErrors())
                return;
            result.sharedCode = generateSharedCode(WTF::move(*calledSharedCode), stubCatalogOfImage, inputs.entriesUsedByFunctions, context);
            steps.didFinish("generate shared code"_s);
        },
    });
    };
    runLanes(context.parallelLoops(), {
        [&] {
            layOutStaticsOfMentions(numberingInputs, *mentions, *namesOfTables, numbers, context);
            mentions.reset();
            namesOfTables.reset();
            times.didFinish("lay out statics"_s);
            if (diagnostics.hasErrors())
                return;
            applyNumbersToFunctions(result.functions, numbers, context);
            times.didFinish("apply numbers to functions"_s);
            if (diagnostics.hasErrors())
                return;
            runLanesBehindNumbersInFunctions();
        },
        [&] {
            StepTimes steps(context, nameOfLaneSteps);
            text = encodeText({ .program = program, .numbers = numbers }, context);
            steps.didFinish("encode text"_s);
        },
        [&] { layOutAndDescribeRegExps(); },
    });
    times.didFinish("lanes in front of the layout of code"_s);
    if (diagnostics.hasErrors())
        return result;

    CodeLayoutInputs layoutInputs {
        .functions = functions,
        .callees = callees,
        .sharedCode = result.sharedCode,
        .regExpSection = regExpSection,
        .templates = templates,
        .finalJumps = finalJumps.span(),
        .codeGranuleShift = codeGranuleShift,
        .webAssemblyCodeSize = webAssembly.code.size(),
        .webAssemblyCodeAlignment = webAssembly.codeAlignment,
        .constantsSize = constants.bytes.size(),
        .ffiCodeSize = ffiStubs.code.size(),
        .ffiCodeAlignment = ffiStubs.codeAlignment,
        .ffiDirectorySize = ffiStubs.directory.size(),
        .farInstanceWords = farInstanceWords.span(),
    };
    result.layout = layOutCode(layoutInputs, context);
    times.didFinish("lay out code"_s);
    if (diagnostics.hasErrors())
        return result;

    ImageSections sections;
    sections[ImageSection::ProgramTables] = WTF::move(programTables);
    // Inline frames number the built-in functions behind the functions, and their reader counts the functions by the tables.
    if (sections.programTables().numberOfFunctions() != numbers.functions().numberOfFunctions()) {
        diagnostics.add(InternalError { .kind = InternalErrorKind::ProgramTableCheckFailed, .detail = makeString("the tables list "_s, sections.programTables().numberOfFunctions(), " functions, and "_s, numbers.functions().numberOfFunctions(), " have a number"_s) });
        return result;
    }

    // All three ask for the layout. The first writes the bytes and the relocations of bodies, which neither of the others reads.
    EncodedFunctionTables functionTables;
    runLanes(context.parallelLoops(), {
        [&] {
            StepTimes steps(context, nameOfLaneSteps);
            removeFinalJumps(result.functions, result.layout.fallsIntoNext, finalJumps.span(), context);
            steps.didFinish("remove final jumps"_s);
        },
        [&] {
            StepTimes steps(context, nameOfLaneSteps);
            // The record of a function says where its side tables lie.
            functionTables = encodeFunctionTables({
                .functions = functions,
                .numbers = numbers,
                .layout = result.layout,
                .index = *index,
                .sideTableFlags = sideTables.flags.span(),
                .sideTableOffsets = sideTables.offsets.span(),
                .programTables = sections.programTables(),
                .hasPositions = hasPositions,
                .program = program,
                .plan = inputs.plan,
            }, context);
            steps.didFinish("encode function tables"_s);
        },
        [&] {
            if (diagnostics.hasErrors())
                return;
            StepTimes steps(context, nameOfLaneSteps);
            sections[ImageSection::FaultSites] = encodeFaultSites(functions, result.layout.bodyStarts.span(), context);
            steps.didFinish("encode fault sites"_s);
        },
    });
    times.didFinish("lanes behind the layout of code"_s);
    if (diagnostics.hasErrors())
        return result;

    const StubBlob& stubs = result.sharedCode.blob;
    sections[ImageSection::Functions] = WTF::move(functionTables.functions);
    sections[ImageSection::FunctionStarts] = WTF::move(functionTables.functionStarts);
    sections[ImageSection::CodeGranules] = WTF::move(functionTables.codeGranules);
    sections[ImageSection::Frames] = bytesOf(sideTables.frames);
    sections[ImageSection::FrameMaps] = bytesOf(sideTables.frameMapWords);
    context.statistics().count(StatisticID::FrameMapBytesEmitted, sections[ImageSection::FrameMaps].size());
    sections[ImageSection::ClosureWords] = WTF::move(functionTables.closureWords);
    sections[ImageSection::MethodTable] = WTF::move(functionTables.methodTable);
    sections[ImageSection::Positions] = WTF::move(sideTables.positions);
    sections[ImageSection::SideTables] = WTF::move(sideTables.bytes);
    sections[ImageSection::SharedCodeCopies] = bytesOf(Vector<uint32_t>(result.layout.sharedCodeCopies));
    sections[ImageSection::Strings] = WTF::move(text.strings);
    sections[ImageSection::LongStrings] = WTF::move(text.longStrings);
    sections[ImageSection::Text] = WTF::move(text.text);
    sections[ImageSection::Identifiers] = WTF::move(text.identifiers);
    sections[ImageSection::StringConstants] = WTF::move(text.stringConstants);
    sections[ImageSection::NamesOfSites] = encodeNamesOfSites(numbers);
    sections[ImageSection::RegExps] = WTF::move(regExps.records);
    sections[ImageSection::RegExpHashes] = WTF::move(regExps.hashes);
    sections[ImageSection::RegExpDetails] = WTF::move(regExps.details);
    sections[ImageSection::WebAssemblyDirectory] = WTF::move(webAssembly.directory);
    sections[ImageSection::StubAddresses] = encodeStubAddresses(stubs);
    sections[ImageSection::StubReturnSites] = bytesOf(stubs.returnSites);
    sections[ImageSection::SlotHandlers] = encodeSlotHandlers(stubs);
    result.entriesUsed = inputs.entriesUsedByFunctions;
    result.entriesUsed.merge(stubs.entriesUsed);
    uint32_t numberOfRegExpsWithCode = compiledRegExps.numberWithForm(RegExpRecordForm::Code);
    result.contents = {
        .numberOfRegExpsWithCode = numberOfRegExpsWithCode,
        .numberOfRegExpsWithoutCode = safeCast<uint32_t>(compiledRegExps.results.size()) - numberOfRegExpsWithCode,
        .numberOfRegExpsWithUnicodePropertyLookup = compiledRegExps.numberWithUnicodePropertyLookup,
        .numberOfWebAssemblyModules = webAssembly.numberOfModules,
    };
    times.didFinish("collect sections"_s);

    CodeLinkerInputs linker { layoutInputs, result.layout, constants, numbers };
    result.image = writeImage({
        .program = program,
        .facts = inputs.facts,
        .plan = inputs.plan,
        .linker = linker,
        .sections = sections,
        .webAssembly = webAssembly,
        .ffiStubs = ffiStubs,
        .entriesUsed = result.entriesUsed,
        .counts = { static_cast<uint32_t>(functions.size()), regExps.maxOutputSize },
        .stubReturnSites = stubs.returnSites.span(),
    }, times, context);
    if (diagnostics.hasErrors())
        result.image.clear();
    else
        context.statistics().count(StatisticID::ImageFunctionsEmitted, functions.size());
    logSizesOfImageForTestRunReport(result.image.span(), result.bytesOfTableParts, context);
    times.didFinish("check image"_s);

    // The image holds what they held. Two of them are a list or two for each unit, and the third is as large as the tables: the build does not wait for their destructors, which stood in no line of the log.
    destroyInBackground(context.backgroundJobs(), WTF::move(callSites));
    destroyInBackground(context.backgroundJobs(), WTF::move(constants));
    destroyInBackground(context.backgroundJobs(), WTF::move(sections));
    times.didFinish("hand on what nobody reads any more"_s);

    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
