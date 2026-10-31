/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringUses.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTFieldTable.h"
#include "AOTFlowResults.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTProgramTables.h"
#include "AOTIdentifierTable.h"
#include "AOTImagesBelow.h"
#include "AOTModuleTable.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTOpcodesNamingRecords.h"
#include "AOTPipeline.h"
#include "AOTPlacement.h"
#include "AOTPropertyAccess.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRegExpRequests.h"
#include "AOTSchemaWalkers.h"
#include "AOTSlotPlanSteps.h"
#include "AOTSort.h"
#include "AOTTextEncoder.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "SmallStrings.h"
#include <algorithm>
#include <array>
#include <concepts>
#include <functional>
#include <limits>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/FastBitVector.h>
#include <wtf/HashSet.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static_assert(FunctionID(0u) < FunctionID(), "a minimum over functions starts at the null id");
static_assert(ModuleID(0u) < ModuleID(), "a function without a module sorts last");

static constexpr Typed::OpcodeSet keyOrComparisonOpcodes {
    Typed::Opcode::StrictEq, Typed::Opcode::Eq, Typed::Opcode::ElemGet, Typed::Opcode::ElemSet, Typed::Opcode::ElemHas, Typed::Opcode::ElemDelete, Typed::Opcode::ElemDefine, Typed::Opcode::SuperGet,
    Typed::Opcode::SuperSet, Typed::Opcode::GetByValWithThis, Typed::Opcode::ToPropertyKey, Typed::Opcode::CallBuiltin, Typed::Opcode::CallInline,
};
static constexpr Typed::TerminatorKindSet keyOrComparisonTerminators { Typed::TerminatorKind::TryCallInline };

static constexpr bool usesOperandsAsKeys(Typed::Opcode opcode) { return keyOrComparisonOpcodes.contains(opcode); }
static constexpr bool usesOperandsAsKeys(Typed::TerminatorKind kind) { return keyOrComparisonTerminators.contains(kind); }
static constexpr bool comparesWithWrittenText(Typed::Opcode opcode) { return opcode == Typed::Opcode::StringEqualsLiteral || opcode == Typed::Opcode::ValueEqualsLiteral; }
static constexpr bool comparesWithWrittenText(Typed::TerminatorKind) { return false; }
static constexpr bool comparesOperands(Typed::Opcode opcode) { return opcode == Typed::Opcode::StrictEq || opcode == Typed::Opcode::Eq; }
static constexpr bool comparesOperands(Typed::TerminatorKind) { return false; }

// The value of `list[i] = "text"` is only stored: nothing compares it or looks it up.
static constexpr bool isOnlyKept(Typed::OperandUses uses) { return !uses.isEmpty() && uses.containsOnly({ Typed::OperandUse::Retains, Typed::OperandUse::Returns }); }

template<typename Operation>
static constexpr bool looksUpKeysByName(Operation operation)
{
    return accessClassOf(operation) == Typed::AccessClass::ByName || familyOf(operation) == Typed::OperationFamily::FusedAccess;
}

static bool hasStringsForMessagesOnly(Typed::Opcode opcode)
{
    // An indirect call names its callee for the message of an error.
    if (accessClassOf(opcode) == Typed::AccessClass::IndirectCall)
        return true;
    switch (opcode) {
    case Typed::Opcode::InstanceOf:
    case Typed::Opcode::CheckInitialized:
    case Typed::Opcode::RequireObjectCoercible:
    case Typed::Opcode::ToObject:
    case Typed::Opcode::NewRuntimeError:
    case Typed::Opcode::NewFunctionThatThrows:
    case Typed::Opcode::ThrowNewError:
        return true;
    default:
        return false;
    }
}

static bool hasStringsForMessagesOnly(Typed::TerminatorKind)
{
    return false;
}

static void addRelaxed(uint32_t& counter)
{
    WTF::atomicExchangeAdd(&counter, 1u, std::memory_order_relaxed);
}

static void atomicMinRelaxed(FunctionID& minimum, FunctionID candidate)
{
    for (FunctionID current = WTF::atomicLoad(&minimum, std::memory_order_relaxed); candidate < current && !WTF::atomicCompareExchangeWeakRelaxed(&minimum, current, candidate);)
        current = WTF::atomicLoad(&minimum, std::memory_order_relaxed);
}

static BitVector toBitVector(const FastBitVector& bits)
{
    BitVector result(bits.numBits());
    bits.forEachSetBit([&](size_t index) {
        result.quickSet(index);
    });
    return result;
}

static StringID impliedNameOf(const Typed::Program& program, const Typed::Value& value)
{
    if (familyOf(value.opcode) != Typed::OperationFamily::PrivateAccess)
        return { };
    PropertyKeyID key = keyOfNamedAccess(program, value);
    return key ? program.at(key).text : StringID();
}

template<typename Operation>
static bool takesKeyAsOperand(Operation operation)
{
    uint32_t numberOfDeclaredOperands = Typed::numberOfNamedOperandsOf(operation) + (Typed::hasVariadicOperand(operation) ? 1 : 0);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        if (Typed::operandKindOf(operation, operandIndex) == Typed::OperandKind::PropertyKey)
            return true;
    }
    return false;
}

template<typename Operation, typename Set>
static Set withOperationsTakingKeys(Set operations)
{
    for (Operation operation : allMembersOf<Operation>) {
        if (takesKeyAsOperand(operation) || Typed::familyOf(operation) == Typed::OperationFamily::PrivateAccess)
            operations.add(operation);
    }
    return operations;
}

namespace {

struct SharedUses {
    size_t numberOfStrings;
    Vector<KeyUse> keys;
    FastBitVector isNamed { numberOfStrings };
    FastBitVector isNamedBesideCreation { numberOfStrings };
    FastBitVector isConstant { numberOfStrings };
    FastBitVector isCreatedRepeatedly { numberOfStrings };
    FastBitVector isAtom { numberOfStrings };
    FastBitVector isComparedRepeatedly { numberOfStrings };
};

struct FunctionWalk {
    bool runsAtMostOnce(Typed::BlockID block)
    {
        if (!functionRunsAtMostOnce)
            return false;
        if (!analyses)
            analyses.emplace(context, function);
        return analyses->executionCountOf(block) == Typed::ExecutionCount::AtMostOnce;
    }

    void nameString(StringID string)
    {
        if (!string || !namedStringSet.add(string).isNewEntry)
            return;
        if (hasCode)
            shared.isNamed.atomicSetAndCheck(string.index(), true);
        namedStrings.append(string.index());
    }

    void countKey(PropertyKeyID key, Typed::Access access, bool looksUpByName, bool looksUpInTables)
    {
        KeyUse& use = shared.keys[key.index()];
        addRelaxed(use.uses);
        if (looksUpByName)
            addRelaxed(use.lookupsByName);
        addRelaxed(use.usesByAccess[static_cast<size_t>(access)]);
        nameString(program.at(key).text);
        if (!looksUpInTables)
            return;
        addRelaxed(use.tableLookupUses);
        atomicMinRelaxed(use.firstTableLookupFunction, id);
    }

    void countCreationSite(StringID string, bool runsAtMostOnce)
    {
        bool isFirstSite = shared.isConstant.atomicSetAndCheck(string.index(), true);
        if (!isFirstSite || !runsAtMostOnce)
            shared.isCreatedRepeatedly.atomicSetAndCheck(string.index(), true);
    }

    template<typename InstructionType, typename Operation>
    void visit(const InstructionType& instruction, Operation operation, bool runsAtMostOnce)
    {
        bool createsStringConstant = runtimeOf(operation) == Typed::RuntimeOperation::StringConstant;
        bool looksUpByName = looksUpKeysByName(operation);
        bool looksUpInTables = !runsAtMostOnce && looksUpByName;
        bool isComparedRepeatedly = !runsAtMostOnce && hasCode;
        forEachReference(instruction, [&]<typename Reference>(const Reference& reference) {
            if constexpr (std::same_as<Reference, StringID>) {
                nameString(reference);
                if (reference && isComparedRepeatedly && comparesWithWrittenText(operation))
                    shared.isComparedRepeatedly.atomicSetAndCheck(reference.index(), true);
                if (createsStringConstant)
                    countCreationSite(reference, runsAtMostOnce);
                else if (reference && !hasStringsForMessagesOnly(operation))
                    shared.isNamedBesideCreation.atomicSetAndCheck(reference.index(), true);
            } else if constexpr (std::same_as<Reference, PropertyKeyID>)
                countKey(reference, accessOf(operation), looksUpByName, looksUpInTables);
        });
        bool usesEveryOperandAsKey = usesOperandsAsKeys(operation);
        instruction.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!usesEveryOperandAsKey && instruction.operandKind(operandIndex) != Typed::OperandKind::PropertyKey)
                return;
            if (isOnlyKept(instruction.operandUses(operandIndex)))
                return;
            const Typed::Value& definition = function.at(sameObjectThroughCasts(function, operand));
            if (definition.opcode != Typed::Opcode::String)
                return;
            shared.isAtom.atomicSetAndCheck(definition.string().index(), true);
            if (isComparedRepeatedly && comparesOperands(operation))
                shared.isComparedRepeatedly.atomicSetAndCheck(definition.string().index(), true);
        });
    }

    void visit(const Typed::Value& value, bool runsAtMostOnce)
    {
        visit(value, value.opcode, runsAtMostOnce);
        nameString(impliedNameOf(program, value));
    }

    Vector<uint32_t> run()
    {
        static const Typed::OpcodeSet opcodesWithUses = withOperationsTakingKeys<Typed::Opcode>(Typed::findOpcodesNaming<StringID, PropertyKeyID>() | keyOrComparisonOpcodes);
        static const Typed::TerminatorKindSet terminatorKindsWithUses = withOperationsTakingKeys<Typed::TerminatorKind>(Typed::findTerminatorKindsNaming<StringID, PropertyKeyID>() | keyOrComparisonTerminators);
        if (!mayEndBlockWith(program, id, terminatorKindsWithUses)) {
            Typed::BlockID current;
            bool currentRunsAtMostOnce = false;
            forEachListedValue(program, id, opcodesWithUses, [&](Typed::BlockID block, uint32_t, ValueID, const Typed::Value& value) {
                if (block != current) {
                    current = block;
                    currentRunsAtMostOnce = runsAtMostOnce(block);
                }
                visit(value, currentRunsAtMostOnce);
            });
            return WTF::move(namedStrings);
        }

        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            Typed::BlockID blockID(blockIndex);
            const Typed::Block& block = function.at(blockID);
            bool blockRunsAtMostOnce = runsAtMostOnce(blockID);
            for (ValueID valueID : block.values)
                visit(function.at(valueID), blockRunsAtMostOnce);
            visit(block.terminator, block.terminator.kind, blockRunsAtMostOnce);
        }
        return WTF::move(namedStrings);
    }

    const Typed::Program& program;
    FunctionID id;
    const Typed::Function& function;
    Typed::AnalysisContext context;
    bool hasCode;
    bool functionRunsAtMostOnce;
    SharedUses& shared;
    std::optional<Typed::FunctionAnalyses> analyses { };
    Vector<uint32_t> namedStrings { };
    UncheckedKeyHashSet<StringID> namedStringSet { };
};

} // anonymous namespace

static Vector<uint32_t> sortFunctionsBySourcePosition(const Typed::Program& program, const ParallelLoops& loops)
{
    // Of functions that begin at one place the outer one comes first, and ahead of it what has no text of its own, as the body of a module: a function may begin with the first character of its file.
    using PositionAndIndex = std::array<uint32_t, 4>;
    Vector<PositionAndIndex> positionsAndIndices(program.functions.size(), [&](size_t index) {
        const Typed::Function& function = program.functions[index];
        uint32_t charactersBehindEnd = function.text.end ? std::numeric_limits<uint32_t>::max() - function.text.end : 0;
        return PositionAndIndex { function.module.index(), function.position, charactersBehindEnd, static_cast<uint32_t>(index) };
    });
    parallelSort(loops, positionsAndIndices.mutableSpan(), std::less<PositionAndIndex> { });
    return Vector<uint32_t>(program.functions.size(), [&](size_t index) {
        return positionsAndIndices[index][3];
    });
}

ProgramUses collectProgramUses(const Typed::PipelineOutput& pipeline, const BitVector& bodiesWithoutCode, BuildContext& context)
{
    const Typed::Program& program = *pipeline.program;
    const ParallelLoops& loops = context.parallelLoops();
    unsigned numberOfFunctions = program.functions.size();

    SharedUses shared { program.strings.size(), Vector<KeyUse>(program.types.numberOfPropertyKeys()) };
    ProgramUses uses;
    StringUses& strings = uses.strings;
    strings.namedByFunction.grow(numberOfFunctions);
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        FunctionID id(index);
        if (program.at(id).hasBody())
            strings.namedByFunction[index] = FunctionWalk { program, id, program.at(id), { context, program, pipeline.flow.get(), nullptr, id }, !bodiesWithoutCode.get(index), pipeline.flow->runsAtMostOnce(program, id), shared }.run();
    });

    strings.isNamed = toBitVector(shared.isNamed);
    strings.isNamedBesideCreation = toBitVector(shared.isNamedBesideCreation);
    strings.isConstant = toBitVector(shared.isConstant);
    strings.isCreatedRepeatedly = toBitVector(shared.isCreatedRepeatedly);
    strings.isAtom = toBitVector(shared.isAtom);
    auto nameBesideCreation = [&](StringID string) {
        strings.isNamed.quickSet(string.index());
        strings.isNamedBesideCreation.quickSet(string.index());
    };
    pipeline.regExpRequests->forEachString(nameBesideCreation);
    for (const RegExpRequest& request : pipeline.regExpRequests->all()) {
        for (StringID name : request.captureGroupNames) {
            if (name)
                strings.isAtom.quickSet(name.index());
        }
        for (const RegExpNamedGroupStrings& group : request.namedGroups)
            strings.isAtom.quickSet(group.name.index());
    }
    for (StringID string : pipeline.literalStrings->stringsWithoutWord())
        nameBesideCreation(string);
    strings.isConstantInTables = pipeline.placement->stringConstantsInTables();
    strings.isNamed.merge(strings.isConstantInTables);
    strings.isConstant.merge(strings.isConstantInTables);
    strings.isCreatedRepeatedly.merge(strings.isConstantInTables);
    strings.functionsInSourceOrder = sortFunctionsBySourcePosition(program, loops);

    uses.keys.byKey = WTF::move(shared.keys);
    uses.keys.textsComparedRepeatedly = toBitVector(shared.isComparedRepeatedly);
    return uses;
}

static_assert(!RowDisplacementTable::rowMarkerColumn);
static_assert(defaultLimitValue(LimitID::LookupTableNames, hostArchitecture()) == RowDisplacementTable::maxColumn);
static_assert(StringID(0u) < StringID(), "a symbol without a description sorts last");
static_assert(PropertyKeyID(0u) < PropertyKeyID(), "a name without a key sorts last");

namespace {

struct NameCandidate {
    NumberedIdentifier identifier;
    bool hasPromisedColumn { false };
    bool isEngineSymbol { false };
    // Nothing looks it up, so it takes no column and comes behind every other name.
    bool isOnlyCompared { false };
    uint32_t uses { 0 };
    uint32_t tableLookupUses { 0 };
    uint32_t lookupsByName { 0 };
    FunctionID firstTableLookupFunction { };
};

} // anonymous namespace

static bool isFoundByText(const Typed::Program& program, const NumberedIdentifier& identifier)
{
    return !identifier.key || program.at(identifier.key).kind == PropertyKeyKind::Text;
}

// The run time creates the identifiers of all of these in one step, before it looks up the first uid.
static bool isEngineSymbol(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
        return true;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<NameCandidate> collectNumberedNames(const Typed::Program& program, const KeyUses& uses, const BitVector& keysWithColumn)
{
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    RELEASE_ASSERT(uses.byKey.size() == numberOfKeys);

    Vector<NameCandidate> names;
    BitVector isTextOfNumberedKey(program.strings.size());
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        PropertyKeyID key(index);
        const KeyUse& use = uses.byKey[index];
        if (!use.uses && !uses.keysNamedByTables.get(index) && !program.builtinNameOf(key))
            continue;
        NumberedIdentifier identifier { key, program.at(key).text };
        if (isFoundByText(program, identifier))
            isTextOfNumberedKey.quickSet(identifier.text.index());
        names.append(NameCandidate {
            .identifier = identifier,
            .hasPromisedColumn = keysWithColumn.get(index),
            .isEngineSymbol = isEngineSymbol(program.at(key).kind),
            .uses = use.uses,
            .tableLookupUses = use.tableLookupUses,
            .lookupsByName = use.lookupsByName,
            .firstTableLookupFunction = use.firstTableLookupFunction,
        });
    }
    for (size_t index : uses.stringsNamedAsKeysByTables) {
        if (!isTextOfNumberedKey.get(index))
            names.append(NameCandidate { .identifier = { PropertyKeyID(), StringID(index) } });
    }
    for (size_t index : uses.textsComparedRepeatedly) {
        if (!isTextOfNumberedKey.get(index) && !uses.stringsNamedAsKeysByTables.get(index) && !program.strings[StringID(index)].empty())
            names.append(NameCandidate { .identifier = { PropertyKeyID(), StringID(index) }, .isOnlyCompared = true });
    }
    return names;
}

static void sortByUse(const ParallelLoops& loops, std::span<NameCandidate> names)
{
    parallelSort(loops, names, [](const NameCandidate& a, const NameCandidate& b) {
        if (a.isOnlyCompared != b.isOnlyCompared)
            return b.isOnlyCompared;
        if (a.hasPromisedColumn != b.hasPromisedColumn)
            return a.hasPromisedColumn;
        if (a.tableLookupUses != b.tableLookupUses)
            return a.tableLookupUses > b.tableLookupUses;
        if (a.uses != b.uses)
            return a.uses > b.uses;
        if (a.identifier.key != b.identifier.key)
            return a.identifier.key < b.identifier.key;
        return a.identifier.text < b.identifier.text;
    });
}

static void groupColumnsByFunction(std::span<NameCandidate> namesWithColumn)
{
    constexpr size_t hotNameCount = RowDisplacementTable::numberOfHotNames - 1;
    if (namesWithColumn.size() <= hotNameCount)
        return;
    auto remainingNamesWithColumn = namesWithColumn.subspan(hotNameCount);
    AOT::stableSortInLine(remainingNamesWithColumn, [](const NameCandidate& a, const NameCandidate& b) {
        return a.firstTableLookupFunction < b.firstTableLookupFunction;
    });
}

static void reportNamesWithoutColumn(std::span<const NameCandidate> namesWithoutColumn, BuildContext& context)
{
    auto isLookedUp = [](const NameCandidate& name) {
        return name.tableLookupUses > 0;
    };
    auto mostLookedUp = std::ranges::find_if(namesWithoutColumn, isLookedUp);
    if (mostLookedUp == namesWithoutColumn.end())
        return;
    auto lookedUpCount = static_cast<uint64_t>(std::ranges::count_if(namesWithoutColumn, isLookedUp));
    context.gaveUp(LimitID::LookupTableNames, { .function = mostLookedUp->firstTableLookupFunction });
    context.statistics().count(hitsOf(LimitID::LookupTableNames), lookedUpCount - 1);
}

static void sortByFirstMention(const ParallelLoops& loops, std::span<NameCandidate> namesWithoutColumn)
{
    parallelSort(loops, namesWithoutColumn, [](const NameCandidate& a, const NameCandidate& b) {
        // The table of fields takes more names than the front table. The names that code looks up get the low numbers, which are the ones it takes.
        if (!a.lookupsByName != !b.lookupsByName)
            return !!a.lookupsByName;
        if (a.isOnlyCompared != b.isOnlyCompared)
            return b.isOnlyCompared;
        if (a.isEngineSymbol != b.isEngineSymbol)
            return a.isEngineSymbol;
        if (a.identifier.text != b.identifier.text)
            return a.identifier.text < b.identifier.text;
        return a.identifier.key < b.identifier.key;
    });
}

static String keyOfEngineSymbol(PropertyKeyKind kind, uint32_t wellKnownSymbol, const String& text)
{
    return makeString(static_cast<unsigned>(kind), ':', wellKnownSymbol, ':', text);
}

static bool isSameSymbolInEveryImage(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
        return true;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::unique_ptr<ImagesBelow> ImagesBelow::tryCreate(std::span<const std::span<const uint8_t>> images)
{
    std::unique_ptr<ImagesBelow> result(new ImagesBelow);
    for (std::span<const uint8_t> bytes : images) {
        std::optional<OptionSet<ImageFlag>> flags = Image::flags(bytes);
        if (!flags || !flags->contains(ImageFlag::SharesRealms))
            return nullptr;
        auto& header = *reinterpret_cast<const ImageHeader*>(bytes.data());
        if (header.formatVersion != imageFormatVersion || header.size > bytes.size() || header.codeOffset > header.size)
            return nullptr;
        std::span<const uint8_t> image = bytes.first(header.size);
        result->m_images.append(Image::create(image, image.data() + header.codeOffset));
    }
    if (result->m_images.isEmpty())
        return nullptr;
    for (size_t position = 0; position < result->m_images.size(); ++position) {
        const std::unique_ptr<Image>& image = result->m_images[position];
        const ImageText& text = image->text();
        const ImageNumberRanges& numbers = image->header().numbers;
        for (uint32_t number : image->programTables().identifiersOfEngineSymbols()) {
            if (number < numbers.firstStringHeader)
                continue;
            const ImageIdentifierWord& identifier = text.identifier(number);
            if (!isSameSymbolInEveryImage(identifier.kind()))
                continue;
            bool isWellKnown = identifier.kind() == PropertyKeyKind::WellKnownSymbol;
            String name = !isWellKnown && identifier.text() ? result->viewOf(position, identifier.text()).toString() : String();
            result->m_engineSymbols.add(keyOfEngineSymbol(identifier.kind(), isWellKnown ? static_cast<uint32_t>(identifier.wellKnownSymbol()) : 0, name), number);
        }
    }
    return result;
}

// These images are bytes that nothing has loaded: each has the headers of its own numbers alone. A text that is a name of an image before it lies there.
bool ImagesBelow::holdsFile(StringView path) const
{
    for (size_t position = 0; position < m_images.size(); ++position) {
        auto viewInImage = [&](StringID string) {
            return viewOf(position, string);
        };
        ModuleTable::TextOfString text(viewInImage);
        ModuleTable table(m_images[position]->programTables(), text);
        if (table.mayHoldPathStartingWith(path) && table.holdsFileAt(path))
            return true;
    }
    return false;
}

StringView ImagesBelow::viewOf(size_t position, StringID string) const
{
    const ImageText& text = m_images[position]->text();
    std::optional<uint32_t> number = text.identifierNumber(string);
    if (!number || *number >= m_images[position]->header().numbers.firstStringHeader)
        return text.view(string);
    for (size_t before = 0; before < position; ++before) {
        const ImageNumberRanges& numbers = m_images[before]->header().numbers;
        if (*number >= numbers.firstStringHeader && *number < numbers.endOfStringHeaders) {
            const ImageText& holder = m_images[before]->text();
            return holder.view(holder.identifier(*number).text());
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<uint32_t> ImagesBelow::numberOfName(const DecodedString& name) const
{
    StringView characters = name.is16Bit() ? StringView(name.units16.span()) : StringView(byteCast<Latin1Character>(name.units8.span()));
    for (size_t position = 0; position < m_images.size(); ++position) {
        const std::unique_ptr<Image>& image = m_images[position];
        const ImageText& text = image->text();
        std::optional<uint32_t> number = IdentifierTable(image->programTables().identifiersByHash()).find(name.hash, [&](uint32_t candidate) {
            StringID string = text.identifier(candidate).text();
            return string ? viewOf(position, string) == characters : characters.isEmpty();
        });
        if (number)
            return number;
    }
    return std::nullopt;
}

StringView ImagesBelow::textOfName(uint32_t number) const
{
    for (size_t position = 0; position < m_images.size(); ++position) {
        const ImageNumberRanges& numbers = m_images[position]->header().numbers;
        if (number < numbers.firstStringHeader || number >= numbers.endOfStringHeaders)
            continue;
        const ImageIdentifierWord& identifier = m_images[position]->text().identifier(number);
        if (identifier.kind() != PropertyKeyKind::Text || !identifier.text())
            return { };
        return viewOf(position, identifier.text());
    }
    return { };
}

std::optional<uint32_t> ImagesBelow::numberOfEngineSymbol(PropertyKeyKind kind, uint32_t wellKnownSymbol, const String& text) const
{
    if (!isSameSymbolInEveryImage(kind))
        return std::nullopt;
    auto found = m_engineSymbols.find(keyOfEngineSymbol(kind, wellKnownSymbol, text));
    if (found == m_engineSymbols.end())
        return std::nullopt;
    return found->value;
}

std::optional<uint32_t> numberInImagesBelow(const Typed::Program& program, PropertyKeyID key, StringID text, const ImagesBelow& below)
{
    PropertyKeyKind kind = key ? program.at(key).kind : PropertyKeyKind::Text;
    if (kind == PropertyKeyKind::Text)
        return text ? below.numberOfName(decodeForImage(program.strings[text])) : std::nullopt;
    if (kind == PropertyKeyKind::WellKnownSymbol)
        return below.numberOfEngineSymbol(kind, static_cast<uint32_t>(*program.at(key).symbol), String());
    if (!text)
        return std::nullopt;
    DecodedString decoded = decodeForImage(program.strings[text]);
    return below.numberOfEngineSymbol(kind, 0, decoded.is16Bit() ? String(decoded.units16.span()) : String(byteCast<Latin1Character>(decoded.units8.span())));
}

IdentifierNumbering numberIdentifiers(const Typed::Program& program, const KeyUses& uses, const BitVector& keysWithColumn, BuildContext& context)
{
    Vector<NameCandidate> names = collectNumberedNames(program, uses, keysWithColumn);
    // A name that an image below has keeps its number there, and with it its column or its lack of one. The others go on where the headers of those images end.
    if (const ImagesBelow* below = context.imagesBelow()) {
        sortByFirstMention(context.parallelLoops(), names.mutableSpan());
        Vector<std::optional<uint32_t>> numbersBelow(names.size());
        forEachIndexInParallel(context.parallelLoops(), static_cast<unsigned>(names.size()), [&](unsigned index) {
            numbersBelow[index] = numberInImagesBelow(program, names[index].identifier.key, names[index].identifier.text, *below);
        });
        // The first number stands for nothing, as 0 does in the first image: its header says how many headers the image has.
        uint32_t next = below->numbersOfLast().endOfStringHeaders;

        IdentifierNumbering numbering;
        numbering.m_first = next++;
        numbering.m_numbersByKey.fill(0, program.types.numberOfPropertyKeys());
        numbering.m_numbersByText.fill(0, program.strings.size());
        numbering.m_identifiers.grow(next + static_cast<size_t>(std::ranges::count_if(numbersBelow, [](const std::optional<uint32_t>& number) { return !number; })));
        for (uint32_t index = 0; index < names.size(); ++index) {
            const NumberedIdentifier& identifier = names[index].identifier;
            uint32_t number = numbersBelow[index] ? *numbersBelow[index] : next++;
            numbering.m_identifiers[number] = identifier;
            if (identifier.key)
                numbering.m_numbersByKey[identifier.key.index()] = number;
            if (isFoundByText(program, identifier))
                numbering.m_numbersByText[identifier.text.index()] = number;
        }
        numbering.m_countWithLookupTableColumn = std::min(numbering.count(), RowDisplacementTable::maxColumn + 1);
        numbering.m_countWithFieldTableColumn = std::min(numbering.count(), FieldTableEntry::maxName + 1);
        context.statistics().count(StatisticID::IdentifiersNumbered, names.size());
        if (context.exceeds(LimitID::MaxIdentifiers, numbering.count()))
            context.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::TooManyIdentifiers, .limit = LimitID::MaxIdentifiers, .count = numbering.count() });
        return numbering;
    }
    sortByUse(context.parallelLoops(), names.mutableSpan());
    auto numberOfNamesLookedUp = static_cast<uint64_t>(std::ranges::count_if(names, [](const NameCandidate& name) { return !name.isOnlyCompared; }));
    auto columnCount = static_cast<uint32_t>(std::min<uint64_t>(numberOfNamesLookedUp, context.limit(LimitID::LookupTableNames)));
    groupColumnsByFunction(names.mutableSpan().first(columnCount));
    reportNamesWithoutColumn(names.span().subspan(columnCount), context);
    sortByFirstMention(context.parallelLoops(), names.mutableSpan().subspan(columnCount));

    IdentifierNumbering numbering;
    numbering.m_numbersByKey.fill(0, program.types.numberOfPropertyKeys());
    numbering.m_numbersByText.fill(0, program.strings.size());
    numbering.m_identifiers.grow(names.size() + 1);
    for (uint32_t index = 0; index < names.size(); ++index) {
        const NumberedIdentifier& identifier = names[index].identifier;
        uint32_t number = RowDisplacementTable::numberForRank(index + 1, columnCount);
        numbering.m_identifiers[number] = identifier;
        if (identifier.key)
            numbering.m_numbersByKey[identifier.key.index()] = number;
        if (isFoundByText(program, identifier))
            numbering.m_numbersByText[identifier.text.index()] = number;
    }
    numbering.m_countWithLookupTableColumn = columnCount + 1;
    auto lookedUpWithoutColumn = static_cast<uint64_t>(std::ranges::count_if(names.span().subspan(columnCount), [](const NameCandidate& name) {
        return name.lookupsByName > 0;
    }));
    numbering.m_countWithFieldTableColumn = static_cast<uint32_t>(std::min<uint64_t>(columnCount + lookedUpWithoutColumn, FieldTableEntry::maxName) + 1);

    context.statistics().count(StatisticID::IdentifiersNumbered, names.size());
    if (context.exceeds(LimitID::MaxIdentifiers, numbering.count()))
        context.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::TooManyIdentifiers, .limit = LimitID::MaxIdentifiers, .count = numbering.count() });
    return numbering;
}

static bool hasCellInVM(std::span<const uint8_t> text)
{
    constexpr size_t maxEncodedSizeOfSingleCharacterString = 2;
    if (text.size() > maxEncodedSizeOfSingleCharacterString)
        return false;
    Vector<char16_t> units = Typed::decodeProgramString(text);
    return units.isEmpty() || (units.size() == 1 && units[0] <= maxSingleCharacterString);
}

StringConstantNumbering numberStringConstants(const Typed::Program& program, const StringUses& uses, BuildContext& context)
{
    BitVector needsNumber(program.strings.size());
    needsNumber.merge(uses.isConstantInTables);
    for (size_t string : uses.isCreatedRepeatedly) {
        if (uses.isConstant.get(string) && !hasCellInVM(program.strings[StringID(string)]))
            needsNumber.quickSet(string);
    }

    StringConstantNumbering numbering;
    numbering.m_numbersPlusOne.fill(0, program.strings.size());
    numbering.m_strings.reserveInitialCapacity(needsNumber.bitCount());
    auto numberIfNeeded = [&](size_t string) {
        if (!needsNumber.quickGet(string) || numbering.m_numbersPlusOne[string])
            return;
        numbering.m_strings.append(StringID(string));
        numbering.m_numbersPlusOne[string] = numbering.count();
    };
    for (uint32_t function : uses.functionsInSourceOrder) {
        for (uint32_t string : uses.namedByFunction[function])
            numberIfNeeded(string);
    }
    for (size_t string : needsNumber)
        numberIfNeeded(string);

    context.statistics().count(StatisticID::StringConstantsNumbered, numbering.count());
    return numbering;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
