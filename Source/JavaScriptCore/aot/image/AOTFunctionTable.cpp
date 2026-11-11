/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionTable.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallingConventions.h"
#include "AOTCodeLayout.h"
#include "AOTStubsCallAdapters.h"
#include "AOTFunctionTables.h"
#include "AOTImage.h"
#include "AOTLinkedFunctions.h"
#include "AOTNumbering.h"
#include "AOTPlan.h"
#include "AOTSectionWriter.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "RegisterAtOffsetList.h"
#include "RegisterSet.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

struct FrameShape {
    uint64_t calleeSaveRegisters;
    uint16_t calleeSavesStart;
    uint16_t frameSizeInUnits;
};

} // anonymous namespace

static bool checkFunctionLimits(const FunctionTableInputs& inputs, BuildContext& context)
{
    const FunctionNumbers& numbers = inputs.numbers.functions();
    auto fits = [&](LimitID limit, uint64_t count) {
        if (!context.exceeds(limit, count))
            return true;
        context.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::TooManyFunctions, .limit = limit, .count = clampTo<uint32_t>(count) });
        return false;
    };
    return fits(LimitID::ImageFunctions, std::max<uint64_t>(inputs.functions.size(), numbers.numberOfFunctions())) && fits(LimitID::ImageClosureFunctions, numbers.endOfClosureFunctions().index() - numbers.firstClosureFunction().index());
}

static bool checkClosureFunctionRange(const FunctionTableInputs& inputs, BuildContext& context)
{
    const ClosureFunctionRangeData& range = inputs.programTables.closureFunctions;
    const FunctionNumbers& numbers = inputs.numbers.functions();
    uint32_t first = numbers.firstClosureFunction().index();
    uint32_t end = numbers.endOfClosureFunctions().index();
    if (range.first == first && range.end == end && range.omittedWordCount <= range.first && range.end <= inputs.programTables.numberOfFunctions())
        return true;
    context.diagnostics().add(InternalError {
        .kind = InternalErrorKind::ProgramTableCheckFailed,
        .detail = makeString("the tables make closures of the functions "_s, range.first, " to "_s, range.end, " of "_s, inputs.programTables.numberOfFunctions(), " and leave out "_s, range.omittedWordCount, " words, and the functions "_s, first, " to "_s, end, " have a closure number"_s),
    });
    return false;
}

static uint64_t unwindableRegisterMask()
{
    uint64_t mask = 0;
    for (const RegisterAtOffset& entry : *RegisterSet::vmCalleeSaveRegisterOffsets()) {
        if (!RegisterSet::stackRegisters().contains(entry.reg(), IgnoreVectors))
            mask |= 1ULL << entry.reg().index();
    }
    return mask;
}

static FrameShape frameShapeOf(const FrameDescription& frame)
{
    static_assert(defaultLimitValue(LimitID::MaxFrameSizeInBytes, hostArchitecture()) / stackAlignmentBytes() <= UINT16_MAX);
    RELEASE_ASSERT(!(frame.frameSizeInBytes % stackAlignmentBytes()));
    FrameShape shape { 0, 0, safeCast<uint16_t>(frame.frameSizeInBytes / stackAlignmentBytes()) };
    for (unsigned i = 0; i < frame.calleeSaveRegisters.registerCount(); ++i) {
        const RegisterAtOffset& entry = frame.calleeSaveRegisters.at(i);
        RELEASE_ASSERT(entry.reg().index() < 64 && entry.offset() == frame.calleeSaveRegisters.at(0).offset() + static_cast<ptrdiff_t>(i * sizeof(CPURegister)) && (!i || entry.reg().index() > frame.calleeSaveRegisters.at(i - 1).reg().index()));
        shape.calleeSaveRegisters |= 1ULL << entry.reg().index();
    }
    if (frame.calleeSaveRegisters.registerCount()) {
        ptrdiff_t offset = frame.calleeSaveRegisters.at(0).offset();
        RELEASE_ASSERT((offset < 0 || (!offset && !frame.hasFrameRecord)) && !(offset % static_cast<ptrdiff_t>(sizeof(CPURegister))) && -offset / static_cast<ptrdiff_t>(sizeof(CPURegister)) <= static_cast<ptrdiff_t>(ImageFrame::maxCalleeSavesStart));
        shape.calleeSavesStart = static_cast<uint16_t>(-offset / static_cast<ptrdiff_t>(sizeof(CPURegister)));
    }
    RELEASE_ASSERT(!(frame.bytesAboveFrameRecord % stackAlignmentBytes()) && frame.bytesAboveFrameRecord / stackAlignmentBytes() <= ImageFrame::maxUnitsAboveFrameRecord && frame.bytesAboveFrameRecord <= frame.frameSizeInBytes);
    shape.calleeSavesStart |= static_cast<uint16_t>(frame.bytesAboveFrameRecord / stackAlignmentBytes() << ImageFrame::unitsAboveFrameRecordShift);
    if (!frame.hasFrameRecord)
        shape.calleeSavesStart |= ImageFrame::hasNoFrameRecordBit;
    return shape;
}

std::optional<FrameTable> collectFrames(const LinkedFunctions& functions, BuildContext& context)
{
    Vector<FrameShape> shapes(functions.size());
    forEachRangeOfIndicesInParallel(context.parallelLoops(), functions.size(), [&](size_t first, size_t end) {
        for (size_t position = first; position < end; ++position)
            shapes[position] = frameShapeOf(functions[position].frame);
    });

    const uint64_t unwindableRegisters = unwindableRegisterMask();
    FrameTable result;
    result.frameOf.reserveInitialCapacity(functions.size());
    UncheckedKeyHashMap<uint64_t, uint32_t, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> numbers;
    for (size_t position = 0; position < shapes.size(); ++position) {
        const FrameShape& shape = shapes[position];
        if (uint64_t unknownRegisters = shape.calleeSaveRegisters & ~unwindableRegisters) {
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::FrameSavesUnknownRegister, .function = functions[position].unit.function, .count = ctz(unknownRegisters) });
            return std::nullopt;
        }
        ImageFrame frame { ImageFrame::packRegisters(shape.calleeSaveRegisters), shape.calleeSavesStart, shape.frameSizeInUnits };
        result.frameOf.append(numbers.ensure(frame.bits(), [&] {
            result.frames.append(frame);
            return static_cast<uint32_t>(result.frames.size() - 1);
        }).iterator->value);
    }
    return result;
}

Vector<uint8_t> ImageFunctions::encode(std::span<const uint32_t> sideTables, std::span<const uint32_t> numbers)
{
    RELEASE_ASSERT(sideTables.size() == numbers.size());
    Vector<uint32_t> groups;
    Vector<Number24> listedNumbers;
    Vector<uint16_t> steps;
    Vector<uint32_t> listedPlaces;
    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> entryOfPlace;
    uint32_t firstPlaceBehind = 0;
    for (size_t index = 0; index < numbers.size(); ++index) {
        size_t entry = index % functionsInGroup;
        if (!entry) {
            groups.append(0);
            groups.append(static_cast<uint32_t>(listedNumbers.size()));
            groups.append(firstPlaceBehind);
        }
        if (entry && numbers[index] == numbers[index - 1] + 1)
            groups[groups.size() - wordsOfGroup] |= 1u << entry;
        else
            listedNumbers.append(Number24(numbers[index]));

        uint32_t place = sideTables[index];
        firstPlaceBehind = std::max(firstPlaceBehind, place + 1);
        if (uint32_t placeOfGroup = groups.last(); place >= placeOfGroup && place - placeOfGroup < isListedPlace)
            steps.append(static_cast<uint16_t>(place - placeOfGroup));
        else {
            uint32_t listed = entryOfPlace.ensure(place, [&] {
                listedPlaces.append(place);
                return static_cast<uint32_t>(listedPlaces.size() - 1);
            }).iterator->value;
            steps.append(static_cast<uint16_t>(isListedPlace | listed));
        }
    }

    bool needsWholePlaces = listedPlaces.size() > isListedPlace;
    uint32_t firstWord = needsWholePlaces ? hasWholePlaces : static_cast<uint32_t>(listedPlaces.size());
    size_t count = numbers.size();
    Vector<uint32_t> words(FillWith { }, firstListWord(firstWord, count) + numberOfListedPlaces(firstWord) + wordsOfNumbers(listedNumbers.size()), 0);
    std::span<uint32_t> span = words.mutableSpan();
    span[0] = firstWord;
    span[1] = static_cast<uint32_t>(listedNumbers.size());
    span[2] = static_cast<uint32_t>(count);
    if (needsWholePlaces)
        memcpySpan(span.subspan(wordsOfHead), sideTables);
    else {
        memcpySpan(asWritableBytes(span.subspan(wordsOfHead)), asBytes(steps.span()));
        memcpySpan(span.subspan(firstListWord(firstWord, count)), listedPlaces.span());
    }
    memcpySpan(span.subspan(firstGroupWord(firstWord, count)), groups.span());
    memcpySpan(asWritableBytes(span.subspan(firstListWord(firstWord, count) + numberOfListedPlaces(firstWord))), asBytes(listedNumbers.span()));
    return bytesOf(words);
}

static Vector<uint8_t> encodeFunctions(const FunctionTableInputs& inputs)
{
    Vector<uint32_t> numbers(inputs.functions.size(), [&](size_t position) {
        return inputs.functions[position].function.index();
    });
    return ImageFunctions::encode(inputs.sideTableOffsets, numbers.span());
}

Vector<uint8_t> FunctionStarts::encode(std::span<const uint32_t> starts)
{
    constexpr uint32_t endOfStarts = std::numeric_limits<uint32_t>::max();
    size_t numberOfEntries = starts.size() + 1;
    auto startAt = [&](size_t index) {
        return index < starts.size() ? starts[index] : endOfStarts;
    };
    size_t firstGroup = numberOfWordsOfSteps(starts.size());
    size_t numberOfGroups = numberOfGroupsFor(starts.size());
    Vector<uint32_t> words;
    words.fill(0, firstGroup + numberOfGroups);
    for (size_t group = 0; group < numberOfGroups; ++group) {
        size_t first = group * functionsInGroup;
        size_t last = std::min<size_t>(first + functionsInGroup, numberOfEntries) - 1;
        uint32_t start = startAt(first);
        if (start < wideGroup && startAt(last) - start <= std::numeric_limits<uint16_t>::max()) {
            words[firstGroup + group] = start;
            auto* steps = reinterpret_cast<uint16_t*>(words.mutableSpan().data());
            for (size_t index = first; index <= last; ++index)
                steps[index] = static_cast<uint16_t>(startAt(index) - start);
            continue;
        }
        words[firstGroup + group] = wideGroup | safeCast<uint32_t>(words.size() - firstGroup);
        for (size_t index = first; index < first + functionsInGroup; ++index)
            words.append(startAt(index));
    }
    return Vector<uint8_t>(asBytes(words.span()));
}

static Vector<uint32_t> encodeCodeGranules(const CodeLayout& layout)
{
    const Vector<uint32_t>& bodyStarts = layout.bodyStarts;
    size_t granuleSize = static_cast<size_t>(1) << layout.codeGranuleShift;
    Vector<uint32_t> codeGranules;
    codeGranules.reserveInitialCapacity((layout.functionsEnd + granuleSize - 1) / granuleSize);
    uint32_t function = 0;
    for (size_t start = 0; start < layout.functionsEnd; start += granuleSize) {
        while (function + 1 < bodyStarts.size() && bodyStarts[function + 1] <= start)
            ++function;
        codeGranules.append(function);
    }
    return codeGranules;
}

static Vector<uint64_t> encodeClosureWords(const FunctionTableInputs& inputs, BuildContext& context)
{
    const ClosureFunctionRangeData& range = inputs.programTables.closureFunctions;
    const FunctionNumbers& numbers = inputs.numbers.functions();
    Vector<uint64_t> words(FillWith { }, range.end - range.omittedWordCount, 0);
    std::span<const FusedCallEntry> fusedCallEntries = inputs.functions.fusedCallEntries();
    forEachIndexInParallel(context.parallelLoops(), range.end - range.first, [&](unsigned closureNumber) {
        FunctionID function(range.first + closureNumber);
        uint32_t codeOffset = 0;
        EntryDescription entry;
        if (std::optional<uint32_t> position = inputs.index.indexOf(function)) {
            const CompiledCode& code = inputs.functions[*position];
            codeOffset = inputs.layout.bodyStarts[*position] + code.body.primaryEntryOffset();
            RELEASE_ASSERT(codeOffset && !(codeOffset % imageFunctionAlignment));
            entry = code.entry;
        } else if (auto fused = std::ranges::lower_bound(fusedCallEntries, numbers.unitOf(function), { }, &FusedCallEntry::unitIndex); fused != fusedCallEntries.end() && fused->unitIndex == numbers.unitOf(function)) {
            bool startsModule = inputs.plan.conventions().callEntryIsNumberOfModuleStart(fused->unit.function);
            if (startsModule)
                codeOffset = inputs.layout.sharedCodeCopies[0] + inputs.sharedCode.blob.offsetOf(startModuleThatRunTimeNames()).value();
            std::optional<uint32_t> positionOfTarget = startsModule ? std::nullopt : fused->unit.role == Typed::CodeRole::GenericCallEntry ? inputs.functions.positionOfCodeAt(fused->placeOfTarget) : inputs.functions.positionOfUnit(fused->unit, fused->unitIndex);
            std::optional<uint32_t> offsetInTarget = positionOfTarget ? inputs.functions[*positionOfTarget].body.offsetOfEntry(Typed::fusedCallEntry) : std::nullopt;
            if (offsetInTarget)
                codeOffset = inputs.layout.bodyStarts[*positionOfTarget] + *offsetInTarget;
            entry = fused->entry;
        }
        if (!codeOffset) {
            context.diagnostics().add(InternalError {
                .kind = InternalErrorKind::FunctionWithoutCode,
                .function = FunctionID(inputs.programTables.sourceFunctionOf(function.index())),
                .detail = makeString("function "_s, function.index(), " of the image, which is unit "_s, numbers.unitOf(function), ", has the closure number "_s, closureNumber),
            });
            return;
        }
        ClosureWord::Flags flags = closureWordFlags(inputs.programTables.kindOfFunction(function.index()));
        flags.borrowedArguments = entry.borrowedArguments;
        flags.endsItsTailCalls = entry.endsItsTailCalls;
        words[function.index() - range.omittedWordCount] = ClosureWord::encode({ codeOffset, entry.numberOfParameters }, numbers.closureNumberOf(function), flags);
    });
    return words;
}

static Vector<uint32_t> encodeMethodTable(const FunctionTableInputs& inputs, BuildContext& context)
{
    const Typed::CallingConventions& conventions = inputs.plan.conventions();
    const Typed::ShapeTable& shapes = inputs.plan.shapes();
    std::span<const Typed::MethodSelector> selectors = conventions.methodSelectors();
    uint32_t numberOfWords = inputs.programTables.instanceSizes.sizeOfMethodTable / sizeof(uint32_t);
    if (!numberOfWords)
        return { };

    auto codeOffsetOf = [&](CodeUnit unit) -> uint32_t {
        std::optional<uint32_t> position = inputs.functions.positionOfUnit(unit, conventions.indexOf(unit));
        if (!position) {
            context.diagnostics().add(InternalError {
                .kind = InternalErrorKind::FunctionWithoutCode,
                .function = unit.function,
                .detail = "the method table holds a unit of the function"_s,
            });
            return 0;
        }
        uint32_t codeOffset = inputs.layout.bodyStarts[*position] + inputs.functions[*position].body.primaryEntryOffset();
        RELEASE_ASSERT(codeOffset && !(codeOffset % codeOffsetUnit));
        return codeOffset / codeOffsetUnit;
    };

    Vector<uint32_t> words(FillWith { }, numberOfWords + selectors.size(), 0);
    Vector<uint32_t, 16> codeOffsetsOfMethods;
    for (uint32_t place = 0; place < selectors.size(); ++place) {
        std::optional<uint32_t> origin = inputs.numbers.numberOf(NumberSpace::MethodSelector, std::span<const uint32_t>(&place, 1));
        if (!origin)
            continue;
        const Typed::MethodSelector& selector = selectors[place];
        const Typed::MethodFamily& family = conventions.familyOf(selector);
        if (family.entersLookup())
            words[numberOfWords + place] = codeOffsetOf(conventions.tableLookupOf(selector));

        codeOffsetsOfMethods.shrink(0);
        for (FunctionID method : family.methods)
            codeOffsetsOfMethods.append(codeOffsetOf(conventions.unitEnteredThroughTable(inputs.program, selector, method)));
        for (const Typed::MethodTableRow& row : family.rows) {
            size_t placeOfMethod = std::ranges::lower_bound(family.methods, row.method) - family.methods.begin();
            Typed::ShapeRange shapesOfRow = shapes.shapesOf(row.layout);
            for (uint32_t shape = shapesOfRow.first; shape < shapesOfRow.end(); ++shape) {
                uint32_t word = methodTableEntryOf(*origin, shape);
                RELEASE_ASSERT(word < numberOfWords && !words[word]);
                words[word] = codeOffsetsOfMethods[placeOfMethod];
            }
        }
    }
    return words;
}

EncodedFunctionTables encodeFunctionTables(const FunctionTableInputs& inputs, BuildContext& context)
{
    const LinkedFunctions& functions = inputs.functions;
    const CodeLayout& layout = inputs.layout;
    const ParallelLoops& loops = context.parallelLoops();
    RELEASE_ASSERT(layout.bodyStarts.size() == functions.size());
    RELEASE_ASSERT(inputs.sideTableOffsets.size() == functions.size());

    if (!checkFunctionLimits(inputs, context) || !checkClosureFunctionRange(inputs, context))
        return { };

    EncodedFunctionTables tables;
    tables.functions = encodeFunctions(inputs);
    tables.closureWords = bytesOf(encodeClosureWords(inputs, context));
    runTasksInParallel(loops, {
        [&] { tables.functionStarts = FunctionStarts::encode(layout.bodyStarts.span()); },
        [&] { tables.codeGranules = bytesOf(encodeCodeGranules(layout)); },
        [&] { tables.methodTable = bytesOf(encodeMethodTable(inputs, context)); },
    });
    return tables;
}

std::optional<uint32_t> Image::imageFunctionFromClosureWord(FunctionID id) const
{
    uint64_t word = closureWord(id);
    if (!word)
        return std::nullopt;
    CodeLocation entry = locate(std::bit_cast<const void*>(ClosureWord::addressOf(word, std::bit_cast<uintptr_t>(code().data()))));
    if (entry.kind != CodeKind::Function || rootFunctionOf(entry.function) != id)
        return std::nullopt;
    return entry.function;
}

std::optional<uint32_t> Image::imageFunctionOf(FunctionID id) const
{
    const ClosureFunctionRangeData& closures = programTables().closureFunctions;
    auto hasClosureWord = [&](uint32_t function) {
        return function >= closures.first && function < closures.end;
    };
    if (hasClosureWord(id.index()))
        return imageFunctionFromClosureWord(id);
    std::call_once(m_didSortFunctionsByID, [&] {
        for (uint32_t function = 0; function < header().counts.functions; ++function) {
            if (!hasClosureWord(rootFunctionOf(function).index()))
                m_functionsByID.append(function);
        }
        AOT::sort(m_functionsByID, { }, [&](uint32_t function) {
            return rootFunctionOf(function).index();
        });
    });
    std::span<const uint32_t> functionsByID = m_functionsByID.span();
    auto found = std::ranges::lower_bound(functionsByID, id.index(), { }, [&](uint32_t function) -> uint32_t {
        return rootFunctionOf(function).index();
    });
    if (found == functionsByID.end() || rootFunctionOf(*found) != id)
        return std::nullopt;
    return *found;
}

std::span<const uint32_t> Image::frameMapWords(uint32_t function) const
{
    const ImageFunction& record = this->function(function);
    if (!record.has(ImageFunctionFlag::HasFrameMaps))
        return { };
    SideTableParts parts = SideTableParts::read(record, section<uint8_t>(ImageSection::SideTables));
    return section<uint32_t>(ImageSection::FrameMaps).subspan(parts.firstFrameMapWord, parts.numberOfFrameMapWords);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
