/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTCallSiteTable.h"
#include "AOTCodedTexts.h"
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
#include "AOTSort.h"
#include "AOTStringUses.h"
#include "AOTStubCatalog.h"
#include "AOTStubDescriptor.h"
#include "AOTStubsCallAdapters.h"
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
#include <wtf/Atomics.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/ParkingLot.h>
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
    forEachRangeOfIndicesInParallel(context.parallelLoops(), functions.size(), [&](size_t first, size_t end) {
        for (size_t position = first; position < end; ++position) {
            const CompiledCode& code = functions[position];
            std::span<const CodeUnit> callees = code.body.callees.span();
            std::span<OptionalIndex> resolved = result.positions.mutableSpan().subspan(result.firstCallee[position], callees.size());
            bool hasCalleeWithoutCode = false;
            for (size_t index = 0; index < resolved.size(); ++index) {
                CodeUnit callee = callees[index];
                bool isFusedCallEntry = callee.role == Typed::CodeRole::GenericCallEntry && conventions.hasFusedCallEntry(callee.function);
                if (std::optional<uint32_t> positionOfCallee = isFusedCallEntry ? std::nullopt : functions.positionOfUnit(callee, conventions.indexOf(callee)))
                    resolved[index] = { *positionOfCallee };
                else
                    hasCalleeWithoutCode = true;
            }
            if (hasCalleeWithoutCode)
                reportCalleesWithoutCode(code, resolved, context);
        }
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
    offsets.reserveInitialCapacity(sizeof(SlotHandlers) / sizeof(void*));
    for (SlotHandlerTable table : allSlotHandlerTables) {
        for (Slot::Kind kind : slotKindsWithHandlers)
            offsets.append(blob.offsetOf(slotHandlerFor(table, kind)).value_or(offsetOfAbsentStub));
    }
    for (AnswerHandlerTable table : allAnswerHandlerTables) {
        const std::array handlers = answerHandlerKeys(table);
        for (unsigned bits = 0; bits < numberOfSlotKindsWithHandlers; ++bits)
            offsets.append(bits < handlers.size() ? blob.offsetOf(handlers[bits]).value_or(offsetOfAbsentStub) : offsetOfAbsentStub);
    }
    return bytesOf(offsets);
}

static Vector<uint8_t> encodeNamesOfSites(const ImageNumbers& numbers)
{
    const NumberedKeys& sites = numbers.siteCaches();
    const IdentifierNumbering& identifiers = numbers.identifiers();
    std::span<const uint32_t> numbersOfSites = numbers.numbersOfSites();
    Vector<uint16_t> names(FillWith { }, numbersOfSites.back(), 0);
    for (uint32_t place = 0, numberOfPlaces = sites.size(); place < numberOfPlaces; ++place) {
        std::span<const uint32_t> key = sites.keyAt(place);
        if (!ImageNumbers::isPlaceWithSiteAnswers(key))
            continue;
        uint32_t name = identifiers.numberOf(ImageNumbers::nameOfSite(key)).value();
        uint32_t number = numbersOfSites[place];
        if (name < InstanceLayout::nameOfSiteStandsBehind) {
            names[number] = static_cast<uint16_t>(name);
            continue;
        }
        RELEASE_ASSERT(numbersOfSites[place + 1] - number == InstanceLayout::numbersOfSiteWhoseNameStandsBehind);
        names[number] = InstanceLayout::nameOfSiteStandsBehind;
        names[number + 1] = static_cast<uint16_t>(name);
        names[number + 2] = static_cast<uint16_t>(name >> 16);
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
        return { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::AllowsLibrarySource };
    case Wire::UnseenCodeCause::OptionAdmitsCode:
        return { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::AllowsLibrarySource, ImageFlag::LoadsModulesByName };
    case Wire::UnseenCodeCause::LoadsCodeByName:
        return { ImageFlag::AdmitsUnknownJavaScript, ImageFlag::AllowsLibrarySource, ImageFlag::LoadsModulesByName, ImageFlag::LoadsCodeOutsideImage };
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

OptionSet<ImageFlag> flagsForUnseenCode(const Typed::Facts& facts, const CompileOptions& options)
{
    OptionSet<ImageFlag> flags;
    if (options.excludesLibraryText())
        flags.add(ImageFlag::IsBuiltWithLibrary);
    for (const auto& record : facts.unseenCode())
        flags.add(imageFlagsAdmittedBy(record.cause));
    RELEASE_ASSERT(facts.unseenCodeCanRun() || !flags.contains(ImageFlag::AdmitsUnknownJavaScript));
    RELEASE_ASSERT(!facts.unseenCodeCanRun() || facts.linked().unseenCodeCanRun);
    if (facts.unseenCodeCanRun() || facts.unseenScriptsCanRun())
        flags.add(ImageFlag::AdmitsUnknownJavaScript);
    if (facts.setsMainModulePath())
        flags.add(ImageFlag::SetsMainModulePath);
    const Typed::Program& program = facts.program();
    if (facts.isLetGo(program.builtins().canonicalID(Builtins::GlobalThis)))
        flags.add(ImageFlag::LetsGlobalObjectGo);
    bool holdsMockOfFile = false;
    bool originalMayRun = false;
    for (const Typed::Variable& variable : program.variables) {
        if (!variable.flags.contains(Typed::VariableFlags::HoldsMock) || program.at(variable.module).runtimeModule)
            continue;
        holdsMockOfFile = true;
        originalMayRun |= variable.flags.contains(Typed::VariableFlags::OriginalMayRun);
    }
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
    uint32_t numberOfStringWords() const { return ofStrings->numberOfWords() - endOfIdentifiers; }

    ImageForm form { ImageForm::Absolute };
    const StringNumbers* ofStrings { nullptr };
    const StringConstantNumbering* constants { nullptr };
    Vector<uint32_t> byString;
    Vector<uint32_t> listedWordIndexes;
    Vector<uint8_t> codedTexts;
    Vector<ImageOtherIdentifier> otherIdentifiers;
    uint32_t endOfIdentifiers { 0 };
    uint32_t count { 0 };
    uint32_t numberOfListedWords { 0 };
    uint64_t largestNumberNeeded { 0 };
};

struct ImageWriterInputs {
    const Typed::Program& program;
    const Typed::Facts& facts;
    const Typed::Plan& plan;
    const CodeLinkerInputs& linker;
    const ImageSections& sections;
    const StringHeaderNumbers& stringHeaders;
    const WebAssemblyImageInputs& webAssembly;
    const FFIStubs& ffiStubs;
    const WTF::BitSet<numberOfEntries>& entriesUsed;
    ImageCounts counts;
    std::span<const StubReturnSite> stubReturnSites;
};

class AnswerAwaitedByLane {
    WTF_MAKE_NONCOPYABLE(AnswerAwaitedByLane);
public:
    AnswerAwaitedByLane() = default;

    void give(bool answer)
    {
        m_state.store(answer ? isYes : isNo);
        ParkingLot::unparkAll(&m_state);
    }

    bool wait()
    {
        for (;;) {
            uint32_t state = m_state.load();
            if (state != isNotGiven)
                return state == isYes;
            ParkingLot::compareAndPark(&m_state, isNotGiven);
        }
    }

private:
    static constexpr uint32_t isNotGiven = 0;
    static constexpr uint32_t isYes = 1;
    static constexpr uint32_t isNo = 2;

    Atomic<uint32_t> m_state { isNotGiven };
};

} // anonymous namespace

static constexpr ASCIILiteral nameOfLaneSteps = "image: lanes"_s;

static bool checksImageWhileBuilding(const BuildContext& context)
{
    return ASSERT_ENABLED || ASAN_ENABLED || context.options().verifiesMachineCode;
}

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

static size_t countFunctionsWithCode(const Typed::Program& program, const LinkedFunctions& functions)
{
    BitVector hasCode(program.functions.size());
    size_t count = 0;
    for (const CompiledCode& code : functions)
        count += hasCode.add(code.unit.function.index());
    return count;
}

static Vector<uint32_t> measureSideTablesOfFunctions(const SideTables& sideTables, bool hasPositions, const ParallelLoops& loops)
{
    size_t numberOfFunctions = sideTables.sizes.size();
    Vector<uint32_t> sizes = sideTables.sizes;
    if (!hasPositions)
        return sizes;
    const uint8_t* positions = sideTables.positions.span().data();
    size_t numberOfGroups = PositionIndex::numberOfEntriesFor(numberOfFunctions);
    forEachIndexInParallel(loops, static_cast<unsigned>(numberOfGroups), [&](unsigned group) {
        const uint8_t* cursor = PositionIndex::firstEntry(positions, group, numberOfFunctions);
        size_t first = static_cast<size_t>(group) * PositionIndex::functionsBetween;
        size_t end = std::min(numberOfFunctions, first + PositionIndex::functionsBetween);
        for (size_t position = first; position < end; ++position) {
            const uint8_t* entry = cursor;
            PositionsHead head = PositionsHead::read(cursor);
            cursor += head.size;
            sizes[position] += static_cast<uint32_t>(cursor - entry);
        }
    });
    return sizes;
}

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

static TextRecords textRecordsIn(const Vector<uint8_t>& strings, const Vector<uint8_t>& longStrings, const Vector<uint8_t>& identifiers)
{
    return {
        spanReinterpretCast<const ImageString>(strings.span()),
        spanReinterpretCast<const ImageLongString>(longStrings.span()),
        spanReinterpretCast<const ImageIdentifier>(identifiers.span()),
    };
}

static StringHeaderNumbers numberStringHeaders(const TextRecords& text, const ImageNumbers& imageNumbers, ImageForm form)
{
    StringHeaderNumbers numbers;
    numbers.form = form;
    numbers.ofStrings = &imageNumbers.stringNumbers();
    numbers.constants = &imageNumbers.stringConstants();
    numbers.byString.fill(0, text.strings.size());
    if (numbers.hasRecords())
        numbers.listedWordIndexes.fill(0, text.strings.size());
    uint64_t count = text.identifiers.size();
    numbers.endOfIdentifiers = static_cast<uint32_t>(text.identifiers.size());
    RELEASE_ASSERT(numbers.endOfIdentifiers <= numbers.ofStrings->numberOfWords());
    for (uint32_t number = 0; number < text.identifiers.size(); ++number) {
        StringID textOfIdentifier = text.identifiers[number].text();
        bool isOther = text.identifiers[number].kind() != PropertyKeyKind::Text;
        if (textOfIdentifier && isOther)
            numbers.otherIdentifiers.append({ number, numbers.ofStrings->requiredNumberOf(textOfIdentifier) + 1 });
        RELEASE_ASSERT(isOther || !number || numbers.ofStrings->requiredNumberOf(textOfIdentifier) == number);
    }
    uint64_t numberOfListedWords = 0;
    for (size_t index : listStringsInOrderOfText(text.strings.size(), imageNumbers.stringsInOrderOfText())) {
        const ImageString& string = text.strings[index];
        bool needsRecord = numbers.hasRecords() && string.hasHeader() && string.range(text.longStrings).length;
        if (needsRecord && !ImageListedString::fitsOneWord(string.range(text.longStrings))) {
            numbers.listedWordIndexes[index] = static_cast<uint32_t>(numberOfListedWords);
            numberOfListedWords += ImageListedString::numberOfWords(string.range(text.longStrings));
        }
        if (!string.hasText() || string.hasIdentifierNumber())
            continue;
        if (string.hasHeader()) {
            numbers.byString[index] = static_cast<uint32_t>(count++);
            continue;
        }
        if (numbers.constants->isPlaced(StringID(static_cast<uint32_t>(index))))
            continue;
        numbers.byString[index] = static_cast<uint32_t>(numberOfListedWords);
        numberOfListedWords += ImageListedString::numberOfWords(string.range(text.longStrings)) + string.hasListedHash();
    }
    numbers.count = static_cast<uint32_t>(count);
    numbers.numberOfListedWords = static_cast<uint32_t>(numberOfListedWords);
    numbers.largestNumberNeeded = std::max<uint64_t>({ count, numberOfListedWords, imageNumbers.stringNumbers().coded().size() });
    return numbers;
}

static Vector<uint8_t> encodeCodedTexts(const Typed::StringTable& strings, const StringNumbers& numbers, BuildContext& context)
{
    Vector<std::span<const uint8_t>> texts(numbers.coded().size(), [&](size_t index) {
        return strings[numbers.coded()[index]];
    });
    Vector<uint8_t> bytes = CodedTexts::encode(texts.span(), context.parallelLoops());
    if (checksImageWhileBuilding(context)) {
        CodedTexts coded(bytes.span(), static_cast<uint32_t>(texts.size()));
        for (uint32_t number = 0; number < texts.size(); ++number)
            RELEASE_ASSERT(equalSpans(std::span<const uint8_t>(coded.at(number).span()), texts[number]));
    }
    return bytes;
}

[[nodiscard]] static bool checkStringHeaderNumbers(const StringHeaderNumbers& numbers, BuildContext& context)
{
    static_assert(defaultLimitValue(LimitID::MaxListedStrings, hostArchitecture()) == ImageString::hashMask + 1);
    if (numbers.largestNumberNeeded <= ImageString::hashMask + 1)
        return true;
    context.reportLimitExceeded(LimitID::MaxListedStrings, numbers.largestNumberNeeded);
    return false;
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
            size = static_cast<uint64_t>(stringHeaders.numberOfStringWords()) * sizeof(ImageStringWord) + stringHeaders.codedTexts.size();
        else if (static_cast<ImageSection>(index) == ImageSection::LongStrings)
            size = stringHeaders.sizeOfList();
        else if (static_cast<ImageSection>(index) == ImageSection::Identifiers)
            size = ImageOtherIdentifier::offsetFor(size / sizeof(ImageIdentifier)) + stringHeaders.otherIdentifiers.sizeInBytes();
        bool isClonedByPages = static_cast<ImageSection>(index) == ImageSection::MethodTable && sections.programTables().instanceSizes.sizeOfMethodTable;
        tablesEnd = isClonedByPages ? WTF::roundUpToMultipleOf<imagePageSize>(tablesEnd) : WTF::roundUpToMultipleOf<imageSectionAlignment>(tablesEnd);
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
        .numberOfStringWords = stringHeaders.numberOfStringWords(),
        .numberOfCodedTexts = static_cast<uint32_t>(stringHeaders.ofStrings->coded().size()),
        .numberOfPlacedConstants = stringHeaders.constants->numberOfPlaced(),
    };
    if (Typed::reachesModuleObjects(parts.program))
        flags.add(ImageFlag::ShowsWaysBetweenModuleObjects);
    if (std::ranges::any_of(parts.program.modules, [](const Typed::Module& module) { return module.kind != Typed::ModuleKind::CommonJs && module.flags.contains(Typed::ModuleFlags::MayBeReevaluated); }))
        flags.add(ImageFlag::RunsEntryPointsAgain);
    if (!parts.program.moduleInstances.isEmpty() || std::ranges::any_of(parts.program.modules, [](const Typed::Module& module) { return module.flags.contains(Typed::ModuleFlags::IsReloadable); }))
        flags.add(ImageFlag::HasReloadableModules);
    if (std::ranges::any_of(parts.program.modules, [](const Typed::Module& module) { return module.flags.contains(Typed::ModuleFlags::IsExposedByRequireCache); }))
        flags.add(ImageFlag::ShowsEntriesOfRequireCache);
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

static ImageStringWord wordOfString(const TextRecords& text, const StringHeaderNumbers& numbers, size_t index)
{
    const ImageString& string = text.strings[index];
    ImageStringWord word = string.inImage(numbers.byString[index]);
    if (string.hasText() && string.range(text.longStrings).length < 2)
        word.bits |= ImageString::isShorterThanTwoFlag;
    return word;
}

static void writeStrings(std::span<uint8_t> image, const TextRecords& text, const StringHeaderNumbers& numbers, const ImageExtent& extent, const ParallelLoops& loops)
{
    std::span<ImageStringRecord> records;
    std::span<ImageStringHeader> headers;
    if (numbers.hasRecords())
        records = sectionIn<ImageStringRecord>(image, extent, ImageSection::StringHeaders);
    else
        headers = sectionIn<ImageStringHeader>(image, extent, ImageSection::StringHeaders);
    std::span stringsInImage = sectionIn<ImageStringWord>(image, extent, ImageSection::Strings).first(numbers.numberOfStringWords());
    memcpySpan(image.subspan(extent[ImageSection::Strings].offset + stringsInImage.size_bytes()), numbers.codedTexts.span());
    std::span listedStrings = sectionIn<uint32_t>(image, extent, ImageSection::LongStrings);
    std::span identifiersInImage = sectionIn<uint8_t>(image, extent, ImageSection::Identifiers);
    for (size_t number = 0; number < text.identifiers.size(); ++number)
        identifiersInImage[number] = text.identifiers[number].word(0).byteInImage();
    memcpySpan(identifiersInImage.subspan(ImageOtherIdentifier::offsetFor(text.identifiers.size())), asBytes(numbers.otherIdentifiers.span()));
    forEachRangeOfIndicesInParallel(loops, text.strings.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            const ImageString& string = text.strings[index];
            if (std::optional<uint32_t> number = numbers.ofStrings->numberOf(StringID(static_cast<uint32_t>(index))); number && *number < numbers.ofStrings->numberOfWords()) {
                ImageStringWord word = wordOfString(text, numbers, index);
                if (*number >= numbers.endOfIdentifiers)
                    stringsInImage[*number - numbers.endOfIdentifiers] = word;
                else
                    RELEASE_ASSERT((word.bits & ~(ImageString::is16BitFlag | ImageString::isShorterThanTwoFlag)) == ImageStringWord::ofIdentifier(*number).bits);
            }
            if (!string.hasText())
                continue;
            ImageLongString range = string.range(text.longStrings);
            if (!string.hasHeader()) {
                if (numbers.constants->isPlaced(StringID(static_cast<uint32_t>(index))))
                    continue;
                ImageListedString::write(listedStrings.subspan(numbers.byString[index]), range);
                if (string.hasListedHash())
                    listedStrings[numbers.byString[index] + ImageListedString::numberOfWords(range)] = string.hashOrIdentifierNumber();
                continue;
            }
            bool isIdentifier = string.hasIdentifierNumber();
            uint32_t number = isIdentifier ? string.hashOrIdentifierNumber() : numbers.byString[index];
            if (isIdentifier)
                RELEASE_ASSERT(text.identifiers[number].kind() == PropertyKeyKind::Text && text.identifiers[number].text() == StringID(index));
            uint32_t hash = isIdentifier ? text.identifiers[number].hashOrSymbol() : string.hashOrIdentifierNumber();
            if (!numbers.hasRecords()) {
                if (range.length || !isIdentifier)
                    headers[number] = ImageStringHeader::create(static_cast<uint64_t>(extent[ImageSection::Text].offset) + range.offset, range.length, hash, string.is16Bit(), isIdentifier ? StringImpl::StringAtom : StringImpl::StringNormal);
                continue;
            }
            if (!range.length)
                continue;
            uint32_t rangeOrIndex = numbers.listedWordIndexes[index] << ImageString::lengthBits;
            if (ImageListedString::fitsOneWord(range))
                ImageListedString::write(std::span { &rangeOrIndex, 1 }, range);
            else {
                std::span words = listedStrings.subspan(numbers.listedWordIndexes[index]);
                ImageListedString::write(words, range);
            }
            records[number] = ImageStringRecord::create(rangeOrIndex, hash, string.is16Bit(), isIdentifier);
        }
    });
}

static void writeWordsOfStringConstants(std::span<uint8_t> image, const TextRecords& text, const StringHeaderNumbers& numbers, const ImageExtent& extent)
{
    std::span constants = sectionIn<uint32_t>(image, extent, ImageSection::StringConstants);
    const uint32_t numberOfConstants = numbers.constants->count();
    const uint32_t numberOfPlaced = numbers.constants->numberOfPlaced();
    RELEASE_ASSERT(constants.size() == static_cast<size_t>(numberOfConstants) + numberOfPlaced);
    for (uint32_t number = 0; number < numberOfPlaced; ++number) {
        const ImageString& string = text.strings[constants[number]];
        ImageLongString range = string.range(text.longStrings);
        RELEASE_ASSERT(!string.hasHeader() && string.hasListedHash() && !string.is16Bit() && ImagePlacedString::fits(range));
        constants[number] = ImagePlacedString::wordFor(range);
        constants[numberOfConstants + number] = string.hashOrIdentifierNumber();
    }
    for (uint32_t& constant : constants.subspan(numberOfPlaced, numberOfConstants - numberOfPlaced))
        constant = wordOfString(text, numbers, constant).bits;
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
    TextRecords text = textRecordsIn(sections[ImageSection::Strings], sections[ImageSection::LongStrings], sections[ImageSection::Identifiers]);
    const StringHeaderNumbers& stringHeaders = parts.stringHeaders;
    if (!checkStringHeaderNumbers(stringHeaders, context))
        return { };
    std::optional<ImageExtent> extent = computeImageExtent(sections, stringHeaders, layout.codeSize, context);
    if (!extent)
        return { };

    const ParallelLoops& loops = context.parallelLoops();
    Vector<uint8_t> image = allocateImage(*extent, layout.executableSize, parts.linker.layoutInputs.templates, loops);
    writeHeader(*reinterpret_cast<ImageHeader*>(image.mutableSpan().data()), parts, *extent, stringHeaders, static_cast<uint32_t>(text.identifiers.size()), context);
    times.didFinish("allocate image, write header"_s);

    copySections(image.mutableSpan(), parts, *extent, loops);
    times.didFinish("copy sections"_s);

    writeStrings(image.mutableSpan(), text, stringHeaders, *extent, loops);
    writeWordsOfStringConstants(image.mutableSpan(), text, stringHeaders, *extent);
    writeRangeOfShapeSets(image.mutableSpan(), parts.linker, *extent);
    times.didFinish("write strings"_s);

    CodeLinkerInputs linker = parts.linker;
    linker.strings = text.strings;
    linker.longStrings = text.longStrings;
    linkCode(linker, image.mutableSubspan(extent->codeOffset, layout.codeSize), context);
    times.didFinish("link code"_s);

    checkImageIsReadOnly(image.subspan(extent->codeOffset, layout.codeSize), parts.ffiStubs, layout, context);
    if (context.diagnostics().hasErrors())
        return { };
    context.statistics().count(StatisticID::TableBytesEmitted, extent->tablesEnd);
    context.statistics().count(StatisticID::StringBytesEmitted, (*extent)[ImageSection::Text].size);
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

    TableInputs tableInputs { program, inputs.facts, inputs.plan, functions, inputs.bodyFacts, context.options(), &context, inputs.functionSources.get() };
    result.numbers = makeUniqueWithoutFastMallocCheck<ImageNumbers>();
    ImageNumbers& numbers = *result.numbers;

    ResolvedCallees callees;
    NumberingInputs numberingInputs { program, inputs.facts, inputs.plan, functions };
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
            ImageNumbersBuilder::numberFunctions(numberingInputs, numbers, context);
            steps.didFinish("number functions"_s);
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
    StringHeaderNumbers stringHeaders;
    Vector<uint8_t> namesOfSites;
    Vector<Vector<PrefixSymbol>> callSites;
    SideTables sideTables;
    const bool hasPositions = !context.options().obfuscates();

    auto compileRegExpsOfImage = [&] {
        StepTimes steps(context, nameOfLaneSteps);
        compiledRegExps = compileRegExps(program, numbers.regExpRequests(), context);
        steps.didFinish("compile regular expressions"_s);
    };
    auto layOutRegExps = [&] {
        StepTimes steps(context, nameOfLaneSteps);
        std::optional<RegExpSection> section = layOutRegExpSection(compiledRegExps.results.span(), context.assemblerPolicy(), templates, &context);
        if (!section)
            return;
        regExpSection = WTF::move(*section);
        steps.didFinish("lay out regular expressions"_s);
    };

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

            collectRequestsForRegExps(numberingInputs, *mentions, *namesOfTables, numbers, context);
            times.didFinish("collect the requests for regular expressions"_s);
            numberAllButIdentifiersAndStrings(numberingInputs, *mentions, *namesOfTables, numbers, context);
            times.didFinish("number all but identifiers and strings"_s);
        },
        [&] {
            StepTimes steps(context, nameOfLaneSteps);
            callSites = encodeCallSites(functions, numbers.functions(), inputs.plan.conventions(), context);
            steps.didFinish("encode call sites"_s);
            if (diagnostics.hasErrors())
                return;
            sideTables = encodeSideTables(functions, callSites.span(), context);
            steps.didFinish("encode side tables"_s);
            if (diagnostics.hasErrors())
                return;
            result.sideTableBytes = measureSideTablesOfFunctions(sideTables, hasPositions, context.parallelLoops());
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

    auto runLanesBehindNumbersInFunctions = [&] {
    runLanes(context.parallelLoops(), {
        [&] {
            StepTimes steps(context, nameOfLaneSteps);
            index.emplace(functions, &context.parallelLoops());
            steps.didFinish("index functions"_s);
            if (mayUseFFI(inputs.facts, context.options()))
                ffiStubs = generateFFIStubs(context);
            steps.didFinish("generate FFI stubs"_s);
            webAssembly = compileWebAssemblyModules(program, templates, context, &numbers.otherStringsWithText(), WTF::move(inputs.earlyWasmModules), &functions);
            steps.didFinish("compile WebAssembly modules"_s);
            farInstanceWords = findFarInstanceWords(functions, numbers, context);
            steps.didFinish("find far words of the instance"_s);
            namesOfSites = encodeNamesOfSites(numbers);
            steps.didFinish("encode names of sites"_s);
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
            ShapeSetsOfTables shapeSets = takeShapeSets(programTables);
            constants = buildConstantPool(functions, context, { shapeSets.bytes.span(), shapeSets.parts.span() });
            steps.didFinish("build constant pool"_s);
        },
        [&] {
            if (diagnostics.hasErrors())
                return;
            StepTimes steps(context, nameOfLaneSteps);
            StubOptions stubOptions = inputs.stubCatalog.options();
            stubOptions.maxNameInFieldTable = numbers.identifiers().countWithFieldTableColumn() - 1;
            StubPolicy stubPolicy;
            stubPolicy.shapesFitHalfword = inputs.plan.shapes().numberOfShapes() <= 1u << 16;
            const StubCatalog stubCatalogOfImage(stubOptions, WTF::move(stubPolicy));
            steps.didFinish("create stub catalog"_s);
            std::optional<CalledSharedCode> calledSharedCode = collectCalledSharedCode(functions, context);
            steps.didFinish("collect called shared code"_s);
            if (!calledSharedCode || diagnostics.hasErrors())
                return;
            bool someWordLeadsToStartOfModule = std::ranges::any_of(functions.fusedCallEntries(), [&](const FusedCallEntry& fused) {
                return inputs.plan.conventions().callEntryIsNumberOfModuleStart(fused.unit.function);
            });
            if (someWordLeadsToStartOfModule)
                calledSharedCode->stubKeys.append(startModuleThatRunTimeNames());
            result.sharedCode = generateSharedCode(WTF::move(*calledSharedCode), stubCatalogOfImage, inputs.entriesUsedByFunctions, context);
            steps.didFinish("generate shared code"_s);
        },
    });
    };
    AnswerAwaitedByLane stringsAreNumbered;
    runLanes(context.parallelLoops(), {
        [&] {
            numberIdentifiersAndStrings(numberingInputs, *mentions, *namesOfTables, numbers, context);
            times.didFinish("number identifiers and strings"_s);
            bool isNumbered = !diagnostics.hasErrors();
            stringsAreNumbered.give(isNumbered);
            if (!isNumbered)
                return;
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
            text = encodeText({ .program = program, .numbers = numbers }, context, scopedLambda<bool()>([&] {
                return stringsAreNumbered.wait();
            }));
            steps.didFinish("encode text"_s);
            if (diagnostics.hasErrors())
                return;
            stringHeaders = numberStringHeaders(textRecordsIn(text.strings, text.longStrings, text.identifiers), numbers, context.recordedOptions().form());
            stringHeaders.codedTexts = encodeCodedTexts(program.strings, numbers.stringNumbers(), context);
            steps.didFinish("number string headers"_s);
        },
        [&] {
            compileRegExpsOfImage();
            if (!diagnostics.hasErrors())
                layOutRegExps();
        },
    });
    times.didFinish("lanes in front of the layout of code"_s);
    if (diagnostics.hasErrors())
        return result;

    regExps = encodeRegExpRecords({ numbers.regExpRequests(), compiledRegExps, regExpSection, numbers.stringNumbers() }, context);
    times.didFinish("describe regular expressions"_s);
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
    if (sections.programTables().numberOfFunctions() != numbers.functions().numberOfFunctions()) {
        diagnostics.add(InternalError { .kind = InternalErrorKind::ProgramTableCheckFailed, .detail = makeString("the tables list "_s, sections.programTables().numberOfFunctions(), " functions, and "_s, numbers.functions().numberOfFunctions(), " have a number"_s) });
        return result;
    }

    EncodedFunctionTables functionTables;
    runLanes(context.parallelLoops(), {
        [&] {
            StepTimes steps(context, nameOfLaneSteps);
            removeFinalJumps(result.functions, result.layout.fallsIntoNext, finalJumps.span(), context);
            steps.didFinish("remove final jumps"_s);
        },
        [&] {
            StepTimes steps(context, nameOfLaneSteps);
            functionTables = encodeFunctionTables({
                .functions = functions,
                .numbers = numbers,
                .layout = result.layout,
                .sharedCode = result.sharedCode,
                .index = *index,
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
    sections[ImageSection::NamesOfSites] = WTF::move(namesOfSites);
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
        .stringHeaders = stringHeaders,
        .webAssembly = webAssembly,
        .ffiStubs = ffiStubs,
        .entriesUsed = result.entriesUsed,
        .counts = { static_cast<uint32_t>(functions.size()), regExps.maxOutputSize },
        .stubReturnSites = stubs.returnSites.span(),
    }, times, context);
    if (diagnostics.hasErrors())
        result.image.clear();
    else
        context.statistics().count(StatisticID::ImageFunctionsEmitted, countFunctionsWithCode(program, functions));
    logSizesOfImageForTestRunReport(result.image.span(), result.bytesOfTableParts, context);
    times.didFinish("check image"_s);

    destroyInBackground(context.backgroundJobs(), WTF::move(callSites));
    destroyInBackground(context.backgroundJobs(), WTF::move(constants));
    destroyInBackground(context.backgroundJobs(), WTF::move(sections));
    times.didFinish("hand on what nobody reads any more"_s);

    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
