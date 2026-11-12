/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCodeRole.h"
#include "AOTID.h"
#include "AOTImageForward.h"
#include "AOTInstanceLayout.h"
#include "AOTOperationData.h"
#include "AOTSlotPlan.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include "YarrFlags.h"
#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

class BuildContext;
class IdentifierNumbering;
class ParallelLoops;
class LinkedFunctions;
class RegExpRequests;
class StringConstantNumbering;
struct CodeBody;
struct NamesAndStringsToNumber;

namespace Typed {
class Facts;
class Plan;
class Program;
class TypeArena;
}

using Typed::CodeUnit;
using Typed::DataID;
using Typed::FieldIndex;
using Typed::LayoutID;
using Typed::PropertyKeyID;
using Typed::TypeID;
using Typed::VariableID;

enum class AskerOfClassGuardOfName : uint8_t { Reads, StoresAlone, ReadsBehindLookAtObject, AssignedMember };
constexpr uint32_t sideWordOfClassGuardOfName(ClassSide side, AskerOfClassGuardOfName asker) { return static_cast<uint32_t>(side) + static_cast<uint32_t>(asker) * numberOfEnumerators<ClassSide>; }
inline ClassSide sideOfClassGuardKey(std::span<const uint32_t> key) { return static_cast<ClassSide>(key[1] % numberOfEnumerators<ClassSide>); }
inline AskerOfClassGuardOfName askerOfClassGuardKey(std::span<const uint32_t> key) { return static_cast<AskerOfClassGuardOfName>(key[1] / numberOfEnumerators<ClassSide>); }
inline bool isClassGuardOfNamesKey(std::span<const uint32_t> key) { return key.size() > 2; }
bool guardsOfNamesAwaitBeginningOf(const Typed::Program&, const Typed::Plan&, LayoutID classLayout);

using ClassGuardKey = Vector<uint32_t, 4>;

inline constexpr uint32_t firstWordOfClassGuardCountKey = UINT32_MAX;
inline bool isClassGuardCountKey(std::span<const uint32_t> key) { return !key.empty() && key[0] == firstWordOfClassGuardCountKey; }

template<typename Functor>
bool forEachGuardInClassGuardCountKey(std::span<const uint32_t> key, NOESCAPE const Functor& functor)
{
    if (!isClassGuardCountKey(key))
        return false;
    size_t numberOfGuards = 0;
    for (std::span<const uint32_t> rest = key.subspan(1); !rest.empty(); ++numberOfGuards) {
        size_t numberOfWords = rest[0];
        if (numberOfWords < 2 || numberOfWords >= rest.size() || rest[1] == firstWordOfClassGuardCountKey)
            return false;
        functor(rest.subspan(1, numberOfWords));
        rest = rest.subspan(1 + numberOfWords);
    }
    return numberOfGuards > 1;
}

inline constexpr uint32_t firstWordOfRowOfClassGuardCountsKey = UINT32_MAX - 1;
inline bool isRowOfClassGuardCountsKey(std::span<const uint32_t> key) { return !key.empty() && key[0] == firstWordOfRowOfClassGuardCountsKey; }

template<typename Functor>
bool forEachSlotInRowOfClassGuardCountsKey(std::span<const uint32_t> key, NOESCAPE const Functor& functor)
{
    if (!isRowOfClassGuardCountsKey(key))
        return false;
    uint32_t slot = 0;
    for (std::span<const uint32_t> rest = key.subspan(1); !rest.empty(); ++slot) {
        size_t numberOfWords = rest[0];
        if (numberOfWords >= rest.size())
            return false;
        functor(slot, rest.subspan(1, numberOfWords));
        rest = rest.subspan(1 + numberOfWords);
    }
    return !!slot;
}

template<typename Functor>
bool forEachGuardInSlotOfRowOfClassGuardCounts(std::span<const uint32_t> slot, NOESCAPE const Functor& functor)
{
    for (std::span<const uint32_t> rest = slot; !rest.empty();) {
        size_t numberOfWords = rest[0];
        if (numberOfWords < 2 || numberOfWords >= rest.size() || rest[1] >= firstWordOfRowOfClassGuardCountsKey)
            return false;
        functor(rest.subspan(1, numberOfWords));
        rest = rest.subspan(1 + numberOfWords);
    }
    return true;
}

enum class NumberSpace : uint8_t {
    Name,
    NameOfText,
    String,
    StringCell,
    KeyCell,
    BuiltinCell,
    BigInt,
    GlobalBinding,
    SiteCache,
    RequireEdge,
    Variable,
    CardOfVariable,
    CardOfWord,
    ModuleValue,
    ModuleNamespace,
    ModuleBuiltinCell,
    DataStorage,
    DataElements,
    RegExpLiteralCell,
    TemplateObjectCell,
    ArrayProfile,
    ReadGroupCache,
    EnvironmentHandedBack,
    ClassGuard,
    ClassRank,
    HeirLookedAt,
    ClosureFunction,
    ClosureCell,
    Allocation,
    Request,
    PropertyRun,
    ShownString,
    AtomOfString,
    NameHeader,
    StringCharacters,
    Type,
    MethodSelector,
    RegExp,
};
inline constexpr unsigned numberOfNumberSpaces = 38;
static_assert(static_cast<unsigned>(NumberSpace::RegExp) + 1 == numberOfNumberSpaces);

enum class GlobalBindingFlags : uint8_t {
    SeesScriptBindings = 1 << 0,
    IsStore = 1 << 1,
    IsForTypeOf = 1 << 2,
};

enum class RequestKind : uint32_t {
    NewClass,
    TemplateObject,
    DirectEval,
    Data,
    CollectionData,
    FormatPattern,
    ImportChecks,
};

struct Mention {
    NumberSpace space;
    uint8_t numberOfWords;
    static constexpr uint8_t longKey = UINT8_MAX;
    uint16_t reserved { 0 };
    uint32_t firstWord;
    int32_t addend;
};
static_assert(sizeof(Mention) == 12);

inline constexpr size_t layoutWordOfAllocationKey = 0;
inline constexpr size_t constructorWordOfAllocationKey = 4;

class MentionList {
    WTF_MAKE_NONCOPYABLE(MentionList);
public:
    MentionList() = default;

    JS_EXPORT_PRIVATE uint32_t add(NumberSpace, std::span<const uint32_t> key, int32_t addend = 0);
    JS_EXPORT_PRIVATE uint32_t add(NumberSpace, uint32_t key, int32_t addend = 0);
    JS_EXPORT_PRIVATE uint32_t addPair(NumberSpace, uint32_t first, uint32_t second, int32_t addend = 0);
    template<typename Tag> uint32_t add(NumberSpace space, Typed::ID<Tag> id, int32_t addend = 0) { return add(space, id.index(), addend); }

    static ClassGuardKey keyOfClassGuard(LayoutID classLayout, ClassSide side) { return { classLayout.index(), static_cast<uint32_t>(side) }; }
    static ClassGuardKey keyOfClassGuardOfName(LayoutID classLayout, ClassSide side, NumberSpace spaceOfName, uint32_t name, AskerOfClassGuardOfName asker = AskerOfClassGuardOfName::Reads)
    {
        return { classLayout.index(), sideWordOfClassGuardOfName(side, asker), static_cast<uint32_t>(spaceOfName), name };
    }
    static ClassGuardKey keyOfClassGuardOfNames(LayoutID classLayout, ClassSide side, std::span<const PropertyKeyID> names)
    {
        ClassGuardKey key { classLayout.index(), static_cast<uint32_t>(side) };
        for (PropertyKeyID name : names) {
            key.append(static_cast<uint32_t>(NumberSpace::Name));
            key.append(name.index());
        }
        return key;
    }
    uint32_t classGuard(const ClassGuardKey& key) { return add(NumberSpace::ClassGuard, key.span()); }
    JS_EXPORT_PRIVATE uint32_t classGuardCount(std::span<const ClassGuardKey> guards);
    JS_EXPORT_PRIVATE uint32_t rowOfClassGuardCounts(std::span<const Vector<ClassGuardKey, 4>> slots);
    uint32_t builtinCell(BuiltinCellKey key) { return add(NumberSpace::BuiltinCell, key.encoded()); }
    uint32_t globalBinding(PropertyKeyID key, OptionSet<GlobalBindingFlags> flags) { return addPair(NumberSpace::GlobalBinding, key.index(), flags.toRaw()); }
    static constexpr uint32_t firstUseOfSiteAnswers = 8;
    static constexpr uint32_t useOfPresenceTests = 12;
    template<size_t inlineCapacity>
    uint32_t siteCache(Vector<uint32_t, inlineCapacity>&& modulesAndPositions, PropertyKeyID key, uint32_t access)
    {
        modulesAndPositions.append(key.index());
        modulesAndPositions.append(access);
        return add(NumberSpace::SiteCache, modulesAndPositions.span());
    }
    uint32_t regExp(StringID pattern, OptionSet<Yarr::Flags> flags) { return addPair(NumberSpace::RegExp, pattern.index(), flags.toRaw()); }
    static constexpr OptionSet<Yarr::Flags> flagsOfMatcher { Yarr::Flags::Global, Yarr::Flags::HasIndices };
    uint32_t regExpMatcher(StringID pattern, OptionSet<Yarr::Flags> flags) { return regExp(pattern, flags | flagsOfMatcher); }
    uint32_t regExpLiteralCell(Wire::SourceLocation written, StringID pattern, uint32_t flags)
    {
        uint32_t key[] { written.module, written.position, pattern.index(), flags };
        return add(NumberSpace::RegExpLiteralCell, std::span<const uint32_t>(key));
    }
    uint32_t templateObjectCell(Wire::SourceLocation written) { return addPair(NumberSpace::TemplateObjectCell, written.module, written.position); }
    uint32_t cardOfWord(NumberSpace space, std::span<const uint32_t> key)
    {
        Vector<uint32_t, 4> words;
        words.append(static_cast<uint32_t>(space));
        words.append(key);
        return add(NumberSpace::CardOfWord, words.span());
    }
    JS_EXPORT_PRIVATE uint32_t allocation(const Typed::PlacedAllocation&, std::span<const FieldIndex> initialFields, int32_t addend = 0);
    JS_EXPORT_PRIVATE uint32_t request(RequestKind, std::span<const uint32_t> words);

    bool isEmpty() const { return m_mentions.isEmpty(); }
    JS_EXPORT_PRIVATE void moveTo(CodeBody&);

private:
    static constexpr size_t smallestTableSize = 64;

    template<size_t extent> uint32_t addKey(NumberSpace, std::span<const uint32_t, extent> key, int32_t addend);
    void growTable();

    Vector<Mention> m_mentions;
    Vector<uint32_t> m_words;
    Vector<uint32_t, smallestTableSize> m_table;
};

inline std::span<const uint32_t> keyOf(const Mention& mention LIFETIME_BOUND, std::span<const uint32_t> mentionWords)
{
    if (mention.numberOfWords == 1)
        return { &mention.firstWord, 1 };
    if (mention.numberOfWords != Mention::longKey)
        return mentionWords.subspan(mention.firstWord, mention.numberOfWords);
    return mentionWords.subspan(mention.firstWord + 1, mentionWords[mention.firstWord]);
}

JS_EXPORT_PRIVATE void copyMadeTextsMentionedBy(CodeBody&, const Typed::TypeArena&);
void internTextsMadeByFunctions(Typed::Program&, LinkedFunctions&, BuildContext&);

struct AllocationKey {
    Typed::PlacedAllocation allocation;
    std::span<const FieldIndex> initialFields;
};
JS_EXPORT_PRIVATE AllocationKey decodeAllocationKey(std::span<const uint32_t> key);

bool canBePropertyRun(const Typed::Program&, std::span<const uint32_t> keys);

constexpr bool mayHaveUnitsBehind(NumberSpace space)
{
    switch (space) {
    case NumberSpace::ClosureFunction:
    case NumberSpace::ClosureCell:
    case NumberSpace::ClassRank:
    case NumberSpace::Request:
    case NumberSpace::ModuleValue:
    case NumberSpace::RequireEdge:
    case NumberSpace::Allocation:
    case NumberSpace::Variable:
    case NumberSpace::CardOfVariable:
    case NumberSpace::MethodSelector:
        return true;
    case NumberSpace::Name:
    case NumberSpace::NameOfText:
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::KeyCell:
    case NumberSpace::BuiltinCell:
    case NumberSpace::BigInt:
    case NumberSpace::GlobalBinding:
    case NumberSpace::SiteCache:
    case NumberSpace::CardOfWord:
    case NumberSpace::ModuleNamespace:
    case NumberSpace::ModuleBuiltinCell:
    case NumberSpace::DataStorage:
    case NumberSpace::DataElements:
    case NumberSpace::RegExpLiteralCell:
    case NumberSpace::TemplateObjectCell:
    case NumberSpace::ArrayProfile:
    case NumberSpace::ReadGroupCache:
    case NumberSpace::EnvironmentHandedBack:
    case NumberSpace::ClassGuard:
    case NumberSpace::HeirLookedAt:
    case NumberSpace::PropertyRun:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::NameHeader:
    case NumberSpace::StringCharacters:
    case NumberSpace::Type:
    case NumberSpace::RegExp:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

JS_EXPORT_PRIVATE void forEachUnitBehind(const Mention&, std::span<const uint32_t> mentionWords, const Typed::Program&, const Typed::Facts&, const Typed::Plan&, NOESCAPE const ScopedLambda<void(CodeUnit)>&);
enum class MentionKind : uint8_t { Call, ClosureWord };
using AsksForUnitsOfClass = ScopedLambda<bool(LayoutID classOrNoneBehindLast)>;
JS_EXPORT_PRIVATE void forEachUnitBehindSlowCase(const Mention&, std::span<const uint32_t> mentionWords, const Typed::Program&, const Typed::Facts&, const Typed::Plan&, NOESCAPE const ScopedLambda<void(CodeUnit, MentionKind)>&, const AsksForUnitsOfClass*);
inline void forEachUnitBehind(const Mention& mention, std::span<const uint32_t> mentionWords, const Typed::Program& program, const Typed::Facts& facts, const Typed::Plan& plan, NOESCAPE const ScopedLambda<void(CodeUnit, MentionKind)>& functor, const AsksForUnitsOfClass* asksForUnitsOfClass = nullptr)
{
    if (mayHaveUnitsBehind(mention.space))
        forEachUnitBehindSlowCase(mention, mentionWords, program, facts, plan, functor, asksForUnitsOfClass);
}

class FunctionNumbers {
public:
    FunctionID idOf(uint32_t unitIndex) const
    {
        if (unitIndex < m_idOfUnit.size())
            return m_idOfUnit[unitIndex];
        auto found = std::ranges::lower_bound(m_idsOfUnitsBehindFunctions, unitIndex, { }, &IDOfUnit::unit);
        return found != m_idsOfUnitsBehindFunctions.end() && found->unit == unitIndex ? found->id : FunctionID();
    }
    uint32_t unitOf(FunctionID id) const { return m_unitOfDerived[id.index() - m_firstDerived]; }
    uint32_t firstDerived() const { return m_firstDerived; }
    uint32_t numberOfFunctions() const { return m_firstDerived + m_unitOfDerived.size(); }
    FunctionID firstClosureFunction() const { return FunctionID(m_firstDerived); }
    FunctionID endOfClosureFunctions() const { return FunctionID(m_firstDerived + m_numberOfClosureFunctions); }
    uint32_t closureNumberOf(FunctionID closureEntry) const { return closureEntry.index() - m_firstDerived; }
    bool hasAscendingClosureNumbers() const
    {
        for (uint32_t number = 0; number < m_numberOfClosureFunctions; ++number) {
            if (number && m_unitOfDerived[number - 1] >= m_unitOfDerived[number])
                return false;
            if (idOf(m_unitOfDerived[number]).index() != m_firstDerived + number)
                return false;
        }
        return true;
    }

private:
    friend struct ImageNumbersBuilder;

    struct IDOfUnit {
        uint32_t unit;
        FunctionID id;
    };

    Vector<FunctionID> m_idOfUnit;
    Vector<IDOfUnit> m_idsOfUnitsBehindFunctions;
    Vector<uint32_t> m_unitOfDerived;
    uint32_t m_firstDerived { 0 };
    uint32_t m_numberOfClosureFunctions { 0 };
};

struct PlacedVariable {
    bool hasWordInStatics() const { return hasWord; }

    Typed::PlannedVariable plan;
    bool hasWord { false };
    uint32_t offset { 0 };
    uint64_t initialWord { 0 };
    FunctionID function;
};

struct ModuleBuiltinCell {
    BuiltinID builtin;
    ModuleID module;
    uint32_t offset { 0 };
};

struct PlacedClassGuardOfName {
    uint32_t sideWord() const { return sideWordOfClassGuardOfName(side, asker); }
    std::array<uint32_t, 4> key() const { return { classLayout.index(), sideWord(), static_cast<uint32_t>(spaceOfName), name }; }
    friend auto operator<=>(const PlacedClassGuardOfName& first, const PlacedClassGuardOfName& second) { return first.key() <=> second.key(); }
    friend bool operator==(const PlacedClassGuardOfName& first, const PlacedClassGuardOfName& second) { return first.key() == second.key(); }

    LayoutID classLayout;
    ClassSide side { ClassSide::Prototype };
    NumberSpace spaceOfName { NumberSpace::Name };
    uint32_t name { 0 };
    uint32_t bit { 0 };
    AskerOfClassGuardOfName asker { AskerOfClassGuardOfName::Reads };
};

struct PlacedClassGuardCount {
    uint32_t offset { 0 };
    uint32_t firstValue { 0 };
};

struct PlacedGuardOfClassGuardCount {
    friend auto operator<=>(const PlacedGuardOfClassGuardCount&, const PlacedGuardOfClassGuardCount&) = default;

    uint32_t bit { 0 };
    uint32_t offsetOfCount { 0 };
};

struct PlacedStatics {
    static constexpr uint32_t noOffset = UINT32_MAX;

    Vector<PlacedVariable> variables;
    Vector<uint32_t> moduleValueOffsets;
    Vector<uint32_t> moduleNamespaceOffsets;
    Vector<uint32_t> dataStorageOffsets;
    Vector<uint32_t> regExpLiteralCellOffsets;
    Vector<uint32_t> templateObjectCellOffsets;
    Vector<uint32_t> arrayProfileOffsets;
    uint32_t distanceFromArrayProfileToLastArray { 0 };
    Vector<uint32_t> readGroupCacheOffsets;
    Vector<ModuleBuiltinCell> moduleBuiltinCells;
    Vector<uint32_t> classGuardBits;
    Vector<PlacedClassGuardOfName> classGuardsOfNames;
    Vector<PlacedClassGuardOfName> namesOfClassGuardsOfSeveralNames;
    Vector<PlacedClassGuardOfName> guardsOfAssignedMembers;
    Vector<uint32_t> bitsOfClassGuards;
    Vector<PlacedClassGuardCount> classGuardCounts;
    Vector<PlacedGuardOfClassGuardCount> guardsOfClassGuardCounts;
    Vector<uint32_t> environmentHandedBackOffsets;
    Vector<uint32_t> subclassStructuresOffsets;
    Vector<uint32_t> roots;
    Vector<StaticsRange> clearedEachCollection;
    Vector<StaticsRange> variablesWithViewMirror;
    uint32_t size { 0 };
};

struct PlacedGlobalBinding {
    PropertyKeyID key;
    OptionSet<GlobalBindingFlags> flags;
    bool isStoredByName { false };
    bool isStoredThroughBinding { false };
};

struct RequireEdge {
    ModuleID referrer;
    ModuleID target;
    StringID specifier;

    friend auto operator<=>(const RequireEdge&, const RequireEdge&) = default;
};

struct NumberedKeys {
    std::span<const uint32_t> keyAt(uint32_t index) const { return words.subspan(firstWord[index], firstWord[index + 1] - firstWord[index]); }
    uint32_t size() const { return firstWord.isEmpty() ? 0 : firstWord.size() - 1; }
    JS_EXPORT_PRIVATE std::optional<uint32_t> find(std::span<const uint32_t> key) const;
    JS_EXPORT_PRIVATE void buildHashIndex(const ParallelLoops&);

    Vector<uint32_t> words;
    Vector<uint32_t> firstWord;
    Vector<uint32_t> hashSlots;
};

class StringNumbers {
public:
    StringNumbers() = default;
    JS_EXPORT_PRIVATE StringNumbers(const BitVector& named, Vector<StringID>&& sortedCodedStrings, size_t numberOfStrings, const IdentifierNumbering* = nullptr);

    uint32_t numberOfWords() const { return m_numberOfWords; }
    std::span<const StringID> coded() const { return m_coded.span(); }
    std::optional<uint32_t> numberOf(StringID id) const
    {
        if (id.index() >= m_numbers.size() || m_numbers[id.index()] == noNumber)
            return std::nullopt;
        return m_numbers[id.index()];
    }
    uint32_t requiredNumberOf(StringID id) const
    {
        std::optional<uint32_t> number = numberOf(id);
        RELEASE_ASSERT(number);
        return *number;
    }

private:
    static constexpr uint32_t noNumber = UINT32_MAX;

    Vector<uint32_t> m_numbers;
    uint32_t m_numberOfWords { 0 };
    Vector<StringID> m_coded;
};

class ImageNumbers {
    WTF_MAKE_NONCOPYABLE(ImageNumbers);
public:
    ImageNumbers();
    ~ImageNumbers();

    JS_EXPORT_PRIVATE std::optional<uint32_t> valueOf(const Mention&, std::span<const uint32_t> mentionWords, bool asInstanceWord) const;
    JS_EXPORT_PRIVATE std::optional<uint32_t> numberOf(NumberSpace, std::span<const uint32_t> key) const;

    const FunctionNumbers& functions() const { return m_functions; }
    const IdentifierNumbering& identifiers() const { return *m_identifiers; }
    const StringConstantNumbering& stringConstants() const { return *m_stringConstants; }
    const RegExpRequests& regExpRequests() const { return *m_regExpRequests; }
    const PlacedStatics& statics() const { return m_statics; }
    InstanceSizes instanceSizes() const { return m_instanceSizes; }

    const BitVector& namedStrings() const { return m_namedStrings; }
    const StringNumbers& stringNumbers() const { return m_stringNumbers; }
    const BitVector& textsOfNames() const { return m_textsOfNames; }
    const BitVector& otherStringsWithText() const { return m_otherStringsWithText; }
    std::span<const StringID> stringsInOrderOfText() const { return m_stringsInOrderOfText.span(); }
    const BitVector& stringsOnlyShown() const { return m_stringsOnlyShown; }
    const BitVector& atomsOfStrings() const { return m_atomsOfStrings; }

    std::span<const PropertyKeyID> keyCells() const { return m_keyCells.span(); }
    std::span<const BuiltinCellKey> builtinCells() const { return m_builtinCells.span(); }
    std::span<const uint32_t> eagerBuiltinCells() const { return m_eagerBuiltinCells.span(); }
    std::span<const BuiltinID> membersAskedByByteAlone() const { return m_membersAskedByByteAlone.span(); }
    std::span<const StringID> bigIntConstants() const { return m_bigIntConstants.span(); }
    std::span<const PlacedGlobalBinding> globalBindings() const { return m_globalBindings.span(); }
    std::span<const RequireEdge> requireEdges() const { return m_requireEdges.span(); }

    std::optional<uint32_t> rankOfClass(LayoutID id) const { return id.index() < m_classRanks.size() && m_classRanks[id.index()] != UINT32_MAX ? std::optional(m_classRanks[id.index()]) : std::nullopt; }
    uint32_t closureCellNumber(FunctionID id) const { return id.index() < m_closureCellNumbers.size() ? m_closureCellNumbers[id.index()] : 0; }
    uint32_t numberOfClosureCells() const { return m_numberOfClosureCells; }

    const NumberedKeys& allocations() const { return m_allocations; }
    const NumberedKeys& requests() const { return m_requests; }
    std::span<const uint32_t> offsetsOfRequests() const { return m_offsetsOfRequests.span(); }
    uint32_t sizeOfRequests() const { return m_sizeOfRequests; }
    const NumberedKeys& propertyRuns() const { return m_propertyRuns; }
    const NumberedKeys& regExpLiteralCells() const { return m_regExpLiteralCells; }
    const NumberedKeys& templateObjectCells() const { return m_templateObjectCells; }
    const NumberedKeys& arrayProfiles() const { return m_arrayProfiles; }
    const NumberedKeys& readGroupCaches() const { return m_readGroupCaches; }
    const NumberedKeys& siteCaches() const { return m_siteCaches; }
    std::span<const uint32_t> numbersOfSites() const { return m_numbersOfSites.span(); }
    static bool isPlaceWithSiteAnswers(std::span<const uint32_t> keyOfSite) { return keyOfSite.back() >= MentionList::firstUseOfSiteAnswers; }
    static PropertyKeyID nameOfSite(std::span<const uint32_t> keyOfSite) { return PropertyKeyID(keyOfSite[keyOfSite.size() - 2]); }
    const Vector<uint32_t>& originsOfMethodSelectors() const { return m_originsOfMethodSelectors; }

    std::span<const TypeID> typesNeededAtRunTime() const { return m_typesNeededAtRunTime.span(); }
    std::optional<uint32_t> placeOfType(TypeID type) const
    {
        std::span<const PlaceOfType> slots = m_placesOfTypes.span();
        if (slots.empty())
            return std::nullopt;
        for (size_t slot = firstSlotOfType(type.bits(), slots.size());; slot = (slot + 1) & (slots.size() - 1)) {
            if (!slots[slot].placePlusOne)
                return std::nullopt;
            if (slots[slot].bits == type.bits())
                return slots[slot].placePlusOne - 1;
        }
    }
    void setOffsetsOfTypes(Vector<uint32_t>&& offsets) { m_offsetsOfTypes = WTF::move(offsets); }
    void setNodesOfData(Vector<uint32_t>&& nodes) { m_nodesOfData = WTF::move(nodes); }

private:
    friend struct ImageNumbersBuilder;

    struct PlaceOfType {
        uint32_t bits;
        uint32_t placePlusOne;
    };

    static size_t firstSlotOfType(uint32_t bits, size_t numberOfSlots) { return (bits * 0x9e3779b1u) >> std::countl_zero(static_cast<uint32_t>(numberOfSlots - 1)); }

    std::optional<uint32_t> numberOfInLine(NumberSpace, std::span<const uint32_t> key) const;
    std::optional<uint32_t> valueOfSlowCase(const Mention&, std::span<const uint32_t> mentionWords, bool asInstanceWord) const;

    FunctionNumbers m_functions;
    std::unique_ptr<IdentifierNumbering> m_identifiers;
    std::unique_ptr<StringConstantNumbering> m_stringConstants;
    std::unique_ptr<RegExpRequests> m_regExpRequests;
    std::unique_ptr<NamesAndStringsToNumber> m_namesAndStringsToNumber;
    PlacedStatics m_statics;
    InstanceSizes m_instanceSizes { };
    BitVector m_namedStrings;
    StringNumbers m_stringNumbers;
    BitVector m_textsOfNames;
    BitVector m_otherStringsWithText;
    Vector<StringID> m_stringsInOrderOfText;
    BitVector m_stringsOnlyShown;
    BitVector m_atomsOfStrings;
    uint32_t m_bytesBetweenNameHeaders { 0 };
    Vector<PropertyKeyID> m_keyCells;
    Vector<uint32_t> m_keyCellNumbersPlusOne;
    Vector<BuiltinCellKey> m_builtinCells;
    Vector<uint32_t> m_eagerBuiltinCells;
    Vector<BuiltinID> m_membersAskedByByteAlone;
    Vector<StringID> m_bigIntConstants;
    Vector<PlacedGlobalBinding> m_globalBindings;
    Vector<RequireEdge> m_requireEdges;
    Vector<uint32_t> m_classRanks;
    Vector<uint32_t> m_closureCellNumbers;
    uint32_t m_numberOfClosureCells { 0 };
    NumberedKeys m_allocations;
    NumberedKeys m_requests;
    Vector<uint32_t> m_offsetsOfRequests;
    uint32_t m_sizeOfRequests { 0 };
    NumberedKeys m_propertyRuns;
    NumberedKeys m_regExpLiteralCells;
    NumberedKeys m_templateObjectCells;
    NumberedKeys m_arrayProfiles;
    NumberedKeys m_readGroupCaches;
    NumberedKeys m_classGuards;
    NumberedKeys m_siteCaches;
    Vector<uint32_t> m_numbersOfSites { 0 };
    Vector<uint32_t> m_originsOfMethodSelectors;
    Vector<TypeID> m_typesNeededAtRunTime;
    Vector<PlaceOfType> m_placesOfTypes;
    Vector<uint32_t> m_offsetsOfTypes;
    Vector<uint32_t> m_nodesOfData;
};

struct FunctionByModule {
    static FunctionByModule forFunction(const Typed::Program&, FunctionID);
    friend auto operator<=>(const FunctionByModule&, const FunctionByModule&) = default;

    ModuleID module;
    uint32_t position { UINT32_MAX };
    FunctionID function;
};

struct GatheredMentions {
    struct ByID {
        BitVector isMentioned;
        Vector<uint32_t> numberOfFunctions;
        Vector<FunctionByModule> firstFunction;
    };

    ByID& byID(NumberSpace space) { return idSpaces[static_cast<unsigned>(space)]; }
    const ByID& byID(NumberSpace space) const { return idSpaces[static_cast<unsigned>(space)]; }
    const NumberedKeys& keys(NumberSpace space) const { return keySpaces[static_cast<unsigned>(space)]; }
    NumberedKeys takeKeys(NumberSpace space) { return std::exchange(keySpaces[static_cast<unsigned>(space)], { }); }
    bool isMentioned(NumberSpace space, uint32_t id) const { return byID(space).isMentioned.get(id); }
    FunctionByModule firstFunctionMentioning(NumberSpace space, uint32_t id) const
    {
        const Vector<FunctionByModule>& functions = byID(space).firstFunction;
        return id < functions.size() ? functions[id] : FunctionByModule { };
    }

    std::array<ByID, numberOfNumberSpaces> idSpaces;
    std::array<NumberedKeys, numberOfNumberSpaces> keySpaces;
};

constexpr bool hasOneIDAsKey(NumberSpace space)
{
    switch (space) {
    case NumberSpace::Name:
    case NumberSpace::NameOfText:
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::KeyCell:
    case NumberSpace::BuiltinCell:
    case NumberSpace::BigInt:
    case NumberSpace::Variable:
    case NumberSpace::CardOfVariable:
    case NumberSpace::ModuleValue:
    case NumberSpace::ModuleNamespace:
    case NumberSpace::DataStorage:
    case NumberSpace::DataElements:
    case NumberSpace::EnvironmentHandedBack:
    case NumberSpace::ClassRank:
    case NumberSpace::HeirLookedAt:
    case NumberSpace::ClosureFunction:
    case NumberSpace::ClosureCell:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::NameHeader:
    case NumberSpace::StringCharacters:
    case NumberSpace::MethodSelector:
        return true;
    case NumberSpace::CardOfWord:
    case NumberSpace::Type:
    case NumberSpace::GlobalBinding:
    case NumberSpace::SiteCache:
    case NumberSpace::RequireEdge:
    case NumberSpace::ModuleBuiltinCell:
    case NumberSpace::ClassGuard:
    case NumberSpace::RegExpLiteralCell:
    case NumberSpace::TemplateObjectCell:
    case NumberSpace::ArrayProfile:
    case NumberSpace::ReadGroupCache:
    case NumberSpace::Allocation:
    case NumberSpace::Request:
    case NumberSpace::PropertyRun:
    case NumberSpace::RegExp:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct NumberingInputs {
    Typed::Program& program;
    const Typed::Facts& facts;
    const Typed::Plan& plan;
    const LinkedFunctions& functions;
};

GatheredMentions gatherMentions(const NumberingInputs&, BuildContext&);

struct NamesOfTables {
    explicit NamesOfTables(const Typed::Program&);
    void merge(const NamesOfTables&);

    BitVector keys;
    BitVector textsNamedAsKeys;
    BitVector strings;
    BitVector stringsWithOtherUsesThanShowing;
    BitVector modulePathStrings;
    BitVector stringConstants;
    BitVector bigInts;
    Vector<uint32_t> allocations;
    Vector<TypeID> types;
    BitVector variables;
    BitVector functionsWithClosureCell;
};

inline bool isVariableOfStaticsNamed(const Typed::PlannedVariable& planned, VariableID id, const NamesOfTables& tables, const GatheredMentions& mentions)
{
    return planned.isDescribedToRunTime
        || tables.variables.get(id.index())
        || mentions.isMentioned(NumberSpace::Variable, id.index())
        || mentions.isMentioned(NumberSpace::CardOfVariable, id.index());
}

inline bool isClosureCellNamed(FunctionID function, const NamesOfTables& tables, const GatheredMentions& mentions)
{
    return tables.functionsWithClosureCell.get(function.index()) || mentions.isMentioned(NumberSpace::ClosureCell, function.index());
}

struct ImageNumbersBuilder {
    const NumberingInputs& inputs;
    GatheredMentions& mentions;
    const NamesOfTables& tables;
    ImageNumbers& numbers;
    BuildContext& context;

    static void numberFunctions(const NumberingInputs&, ImageNumbers&, BuildContext&);
    void checkEntriesOfMentionedClosures();
    void numberClosureCellsAndClassRanks();
    void collectNamesAndStringsWithText();
    void numberIdentifiersAndStrings();
    void numberConstants();
    void numberRows();
    void numberSites();
    void numberTypes();
    void collectRegExpRequests();
    void layOutStatics();
};

void collectRequestsForRegExps(const NumberingInputs&, GatheredMentions&, const NamesOfTables&, ImageNumbers&, BuildContext&);
void numberAllButIdentifiersAndStrings(const NumberingInputs&, GatheredMentions&, const NamesOfTables&, ImageNumbers&, BuildContext&);
void numberIdentifiersAndStrings(const NumberingInputs&, GatheredMentions&, const NamesOfTables&, ImageNumbers&, BuildContext&);
void layOutStaticsOfMentions(const NumberingInputs&, GatheredMentions&, const NamesOfTables&, ImageNumbers&, BuildContext&);
void numberMentions(const NumberingInputs&, GatheredMentions&, const NamesOfTables&, ImageNumbers&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
