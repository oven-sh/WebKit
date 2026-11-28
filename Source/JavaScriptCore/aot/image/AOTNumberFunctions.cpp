/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNumbering.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallingConventions.h"
#include "AOTIRProgram.h"
#include "AOTImageBuilder.h"
#include "AOTObjectWords.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTStubKey.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThreads.h"
#include <bit>
#include <compare>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/Scope.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static_assert(defaultLimitValue(LimitID::ImageClosureFunctions, hostArchitecture()) < Number24::max);

namespace {

struct PlaceOfClosureCell {
    friend auto operator<=>(const PlaceOfClosureCell&, const PlaceOfClosureCell&) = default;

    bool isRarelyFilled;
    uint32_t module;
    uint32_t function;
};

} // anonymous namespace

static bool countOfImageFunctionsFits(BuildContext& context, LimitID limit, uint64_t count)
{
    if (!context.exceeds(limit, count))
        return true;
    context.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::TooManyFunctions, .limit = limit, .count = clampTo<uint32_t>(count) });
    return false;
}

void ImageNumbersBuilder::numberFunctions(const NumberingInputs& inputs, ImageNumbers& numbers, BuildContext& context)
{
    const Typed::CallingConventions& conventions = inputs.plan.conventions();
    const uint32_t firstDerived = static_cast<uint32_t>(inputs.program.functions.size());
    const uint32_t numberOfUnits = conventions.numberOfUnits();

    BitVector closureEntries(numberOfUnits);
    BitVector otherDerivedUnits(numberOfUnits);
    BitVector unitsShownInFrames(numberOfUnits);
    for (const CompiledCode& code : inputs.functions) {
        ASSERT(code.unitIndex == conventions.indexOf(code.unit));
        RELEASE_ASSERT(code.unitIndex < numberOfUnits);
        for (size_t frame = 1; code.sites.hasEncodedInlineFrames() && frame < code.sites.inlineFrames.size(); ++frame) {
            Typed::CodeUnit shown = code.body.callees[code.sites.inlineFrames[frame].callee];
            if (shown.role != Typed::CodeRole::Body)
                unitsShownInFrames.quickSet(conventions.indexOf(shown));
        }
        if (code.unit.role == Typed::CodeRole::Body)
            continue;
        bool isListedTwice = (Typed::isGenericEntry(code.unit.role) ? closureEntries : otherDerivedUnits).quickSet(code.unitIndex);
        RELEASE_ASSERT(!isListedTwice);
    }
    otherDerivedUnits.merge(unitsShownInFrames);
    for (const FusedCallEntry& fused : inputs.functions.fusedCallEntries()) {
        RELEASE_ASSERT(fused.unitIndex < numberOfUnits);
        bool isListedTwice = closureEntries.quickSet(fused.unitIndex);
        RELEASE_ASSERT(!isListedTwice);
    }

    size_t numberOfClosureFunctions = closureEntries.bitCount();
    size_t numberOfDerivedUnits = numberOfClosureFunctions + otherDerivedUnits.bitCount();
    bool everyNumberFits = countOfImageFunctionsFits(context, LimitID::ImageFunctions, static_cast<uint64_t>(firstDerived) + numberOfDerivedUnits);
    everyNumberFits &= countOfImageFunctionsFits(context, LimitID::ImageClosureFunctions, numberOfClosureFunctions);
    if (!everyNumberFits)
        return;

    numbers.m_functions.m_firstDerived = firstDerived;
    numbers.m_functions.m_numberOfClosureFunctions = static_cast<uint32_t>(numberOfClosureFunctions);
    Vector<FunctionID>& idOfUnit = numbers.m_functions.m_idOfUnit;
    Vector<uint32_t>& unitOfDerived = numbers.m_functions.m_unitOfDerived;

    Vector<FunctionNumbers::IDOfUnit>& idsOfUnitsBehindFunctions = numbers.m_functions.m_idsOfUnitsBehindFunctions;
    idOfUnit.fill(FunctionID(), static_cast<size_t>(std::min<uint64_t>(numberOfUnits, static_cast<uint64_t>(firstDerived) * Typed::numberOfCodeRolesOfOneFunction)));
    auto setIDOfUnit = [&](uint32_t unitIndex, FunctionID id) {
        if (unitIndex < idOfUnit.size())
            idOfUnit[unitIndex] = id;
        else
            idsOfUnitsBehindFunctions.append({ unitIndex, id });
    };
    for (uint32_t index = 0; index < firstDerived; ++index)
        setIDOfUnit(conventions.indexOf({ FunctionID(index), Typed::CodeRole::Body }), FunctionID(index));

    unitOfDerived.reserveInitialCapacity(numberOfDerivedUnits);
    auto numberUnitsInAscendingOrder = [&](const BitVector& units) {
        for (size_t unitIndex : units) {
            setIDOfUnit(static_cast<uint32_t>(unitIndex), FunctionID(firstDerived + unitOfDerived.size()));
            unitOfDerived.append(static_cast<uint32_t>(unitIndex));
        }
    };
    numberUnitsInAscendingOrder(closureEntries);
    numberUnitsInAscendingOrder(otherDerivedUnits);
    if (hasFlag(context.options().injectedFaults, Wire::FaultInjection::SwapClosureNumbers) && numberOfClosureFunctions > 1) {
        std::swap(unitOfDerived[0], unitOfDerived[1]);
        setIDOfUnit(unitOfDerived[0], FunctionID(firstDerived));
        setIDOfUnit(unitOfDerived[1], FunctionID(firstDerived + 1));
    }
    AOT::sortInLine(idsOfUnitsBehindFunctions, { }, &FunctionNumbers::IDOfUnit::unit);
}

void ImageNumbersBuilder::checkEntriesOfMentionedClosures()
{
    const Typed::CallingConventions& conventions = inputs.plan.conventions();
    BitVector functionsOfMentionedClosures = mentions.byID(NumberSpace::ClosureFunction).isMentioned;
    functionsOfMentionedClosures.merge(mentions.byID(NumberSpace::ClosureCell).isMentioned);
    for (size_t index : functionsOfMentionedClosures) {
        FunctionID id(index);
        if (numbers.m_functions.idOf(conventions.indexOf(conventions.closureEntryOf(id))))
            continue;
        context.diagnostics().add(InternalError {
            .kind = InternalErrorKind::FunctionWithoutCode,
            .function = id,
            .detail = "code mentions its closures, and the entry that the word of a closure names was not compiled"_s,
        });
    }
}

static BitVector findFunctionsWhoseClosuresAreMadeWhileModulesLoad(const NumberingInputs& inputs, const NamesOfTables& tables)
{
    BitVector functions = tables.functionsWithClosureCell;
    uint32_t numberOfVariables = inputs.plan.slots().numberOfVariables();
    for (uint32_t index = 0; index < numberOfVariables; ++index) {
        Typed::PlannedVariable variable = inputs.plan.variable(VariableID(index));
        if (variable.encoding != Typed::VariableEncoding::FunctionAsZero)
            continue;
        RELEASE_ASSERT(variable.function);
        functions.set(variable.function.index());
    }
    for (const Typed::RuntimeFunctionEntry& entry : inputs.program.runtimeFunctions) {
        if (entry.function)
            functions.set(entry.function.index());
    }
    return functions;
}

void ImageNumbersBuilder::numberClosureCellsAndClassRanks()
{
    const Typed::Program& program = inputs.program;
    const Typed::Plan& plan = inputs.plan;
    const Typed::CallingConventions& conventions = plan.conventions();

    const BitVector functionsMadeWhileModulesLoad = findFunctionsWhoseClosuresAreMadeWhileModulesLoad(inputs, tables);
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    Vector<PlaceOfClosureCell> cells;
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        FunctionID id(index);
        if (!plan.hasClosureCell(id) || !isClosureCellNamed(id, tables, mentions))
            continue;
        if (!numbers.m_functions.idOf(conventions.indexOf(conventions.closureEntryOf(id))))
            continue;
        ModuleID module = program.functions[index].module;
        cells.append({ !functionsMadeWhileModulesLoad.get(index), module ? module.index() : UINT32_MAX, index });
    }
    AOT::sort(cells);

    numbers.m_closureCellNumbers.fill(0, numberOfFunctions);
    uint32_t numberOfCells = 1;
    for (const PlaceOfClosureCell& cell : cells)
        numbers.m_closureCellNumbers[cell.function] = numberOfCells++;
    numbers.m_numberOfClosureCells = numberOfCells;

    numbers.m_classRanks.fill(UINT32_MAX, program.layouts.size());
    uint32_t numberOfClassRecords = 0;
    for (size_t index : plan.layoutsWithClassRecord()) {
        if (program.layouts[index].kind == LayoutKind::Instance)
            numbers.m_classRanks[index] = numberOfClassRecords++;
    }
}

static NEVER_INLINE void reportMentionWithoutNumber(const CompiledCode& code, const Mention& mention, ASCIILiteral holder, BuildContext& context)
{
    context.diagnostics().add(InternalError {
        .kind = InternalErrorKind::RelocationNotApplied,
        .function = code.unit.function,
        .count = static_cast<uint32_t>(mention.space),
        .detail = makeString(holder, " in its "_s, nameOf(code.unit.role), " mentions a thing of number space "_s, static_cast<unsigned>(mention.space), " that has no number"_s),
    });
}

static ALWAYS_INLINE std::optional<uint32_t> numberToPutInPlaceOfMention(const CompiledCode& code, uint32_t mentionIndex, ASCIILiteral holder, const ImageNumbers& numbers, BuildContext& context)
{
    const Mention& mention = code.body.mentions[mentionIndex];
    std::optional<uint32_t> number = numbers.valueOf(mention, code.body.mentionWords.span(), false);
    if (!number)
        reportMentionWithoutNumber(code, mention, holder, context);
    return number;
}

static ALWAYS_INLINE bool mentionIsOfNameWithColumn(const CompiledCode& code, uint32_t mentionIndex, const ImageNumbers& numbers)
{
    const Mention& mention = code.body.mentions[mentionIndex];
    std::span<const uint32_t> key = keyOf(mention, code.body.mentionWords.span());
    PropertyKeyID name;
    if (mention.space == NumberSpace::Name)
        name = PropertyKeyID(key[0]);
    else if (mention.space == NumberSpace::SiteCache && ImageNumbers::isPlaceWithSiteAnswers(key))
        name = ImageNumbers::nameOfSite(key);
    else
        return false;
    std::optional<uint32_t> number = numbers.identifiers().numberOf(name);
    return number && *number && *number < InstanceLayout::nameOfSiteStandsBehind;
}

static void putNumberInThrowWordOfThunk(const CompiledCode& code, std::span<uint64_t> moves, size_t index, const ImageNumbers& numbers, BuildContext& context)
{
    uint32_t firstWord = static_cast<uint32_t>(moves[index + 1]);
    uint32_t mention = static_cast<uint32_t>(moves[index + 1] >> 32);
    std::optional<uint32_t> number = numberToPutInPlaceOfMention(code, mention, "the throw word that an argument thunk moves"_s, numbers, context);
    if (!number)
        return;
    std::optional<uint32_t> narrowWord = ThrowSiteWord::withNarrowA(firstWord, *number);
    setSourceOfArgumentMove(moves, index, ArgumentSource::Constant, narrowWord ? *narrowWord : ThrowSiteWord::inRegister(firstWord, *number));
}

static void putNumberAboveConstantOfThunk(const CompiledCode& code, std::span<uint64_t> moves, size_t index, const ImageNumbers& numbers, BuildContext& context)
{
    uint32_t constant = static_cast<uint32_t>(moves[index + 1]);
    uint32_t mention = static_cast<uint32_t>(moves[index + 1] >> 32);
    if (std::optional<uint32_t> number = numberToPutInPlaceOfMention(code, mention, "the number that an argument thunk moves above a constant"_s, numbers, context))
        setSourceOfArgumentMove(moves, index, ArgumentSource::Constant, constant | static_cast<uint64_t>(*number) << 32);
}

static void putNumbersInThrowWordsOfAccess(const CompiledCode& code, FaultingAccess& access, const ImageNumbers& numbers, BuildContext& context)
{
    constexpr unsigned wordOfWideA = 1;
    unsigned wordsThatWereMentions = std::exchange(access.wordsThatAreMentions, 0);
    for (unsigned wordsLeft = wordsThatWereMentions; wordsLeft; wordsLeft &= wordsLeft - 1) {
        uint32_t& word = access.throwSite[WTF::ctz(wordsLeft)];
        std::optional<uint32_t> number = numberToPutInPlaceOfMention(code, word, "a word that says what a faulting access throws"_s, numbers, context);
        if (!number)
            return;
        word = *number;
    }
    if (!(wordsThatWereMentions & 1 << wordOfWideA) || !std::bit_cast<ThrowSiteWord>(access.throwSite[0]).hasWideA)
        return;
    if (std::optional<uint32_t> narrowWord = ThrowSiteWord::withNarrowA(access.throwSite[0], access.throwSite[wordOfWideA])) {
        access.throwSite[0] = *narrowWord;
        access.throwSite.removeAt(wordOfWideA);
    }
}

void applyNumbersToFunctions(LinkedFunctions& functions, const ImageNumbers& numbers, BuildContext& context)
{
    if (!numbers.functions().hasAscendingClosureNumbers()) {
        context.diagnostics().add(InternalError {
            .kind = InternalErrorKind::PlacementCheckFailed,
            .detail = "closure numbers do not rise with the numbers of the functions, as the tests of closures in compiled code expect"_s,
        });
        return;
    }
    forEachRangeOfIndicesInParallel(context.parallelLoops(), functions.size(), [&](size_t first, size_t end) {
        uint64_t columnStubVariantsSelected = 0;
        auto countAtEnd = makeScopeExit([&] {
            context.statistics().count(StatisticID::ColumnStubVariantsSelected, columnStubVariantsSelected);
        });
        for (size_t position = first; position < end; ++position) {
            CompiledCode& code = functions[position];
            code.function = numbers.functions().idOf(code.unitIndex);
            if (!code.function) {
                context.diagnostics().add(InternalError {
                    .kind = InternalErrorKind::PlacementCheckFailed,
                    .function = code.unit.function,
                    .detail = makeString("its "_s, nameOf(code.unit.role), " has code and no number among the functions of the image"_s),
                });
                continue;
            }

            for (const NumberedStubKey& numbered : code.body.rareLists->numberedStubKeys) {
                StubKey& key = code.body.stubKeys[numbered.stubKey];
                if (!key.immediate()) {
                    if (mentionIsOfNameWithColumn(code, numbered.mention, numbers)) {
                        key = siblingUnderNameWithColumn(key).value();
                        ++columnStubVariantsSelected;
                    }
                    continue;
                }
                ASSERT(key.immediate() == std::optional<uint32_t>(0));
                if (std::optional<uint32_t> immediate = numberToPutInPlaceOfMention(code, numbered.mention, "the key of a stub"_s, numbers, context))
                    key = key.withImmediate(*immediate);
            }

            size_t numberOfLists = code.body.argumentMoves.size();
            for (size_t list = 0; list < numberOfLists; ++list) {
                std::span<uint64_t> moves = code.body.argumentMoves[list];
                RELEASE_ASSERT(moves.size() >= numberOfHeaderWords && !((moves.size() - numberOfHeaderWords) % 2));
                if (moves[1] & (immediateIsMention | mentionNamesSiblingOfCallee)) {
                    if (std::optional<StubKey> sibling = siblingUnderNameWithColumn(StubKey::fromBits(moves[0])); sibling && mentionIsOfNameWithColumn(code, static_cast<uint32_t>(moves[1]), numbers)) {
                        moves[0] = sibling->bits();
                        ++columnStubVariantsSelected;
                    }
                    if (moves[1] & mentionNamesSiblingOfCallee)
                        moves[1] = 0;
                }
                if (moves[1] & immediateIsMention) {
                    if (std::optional<uint32_t> immediate = numberToPutInPlaceOfMention(code, static_cast<uint32_t>(moves[1]), "the immediate that an argument thunk moves"_s, numbers, context))
                        moves[1] = hasImmediate | *immediate;
                }
                for (size_t index = numberOfHeaderWords; index < moves.size(); index += 2) {
                    if (sourceOfArgumentMove(moves, index) == ArgumentSource::ThrowWordWithMention)
                        putNumberInThrowWordOfThunk(code, moves, index, numbers, context);
                    else if (sourceOfArgumentMove(moves, index) == ArgumentSource::ConstantBelowMention)
                        putNumberAboveConstantOfThunk(code, moves, index, numbers, context);
                }
            }

            for (FaultingAccess& access : code.sites.faultingAccesses)
                putNumbersInThrowWordsOfAccess(code, access, numbers, context);
        }
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
