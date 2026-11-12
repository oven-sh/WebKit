/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTSlotRepresentation.h"
#include <bit>
#include <iterator>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

template<typename ID>
constexpr std::optional<ID> optionalIDInImage(uint32_t word)
{
    return word == UINT32_MAX ? std::nullopt : std::optional<ID>(ID(word));
}

template<typename ID>
constexpr std::optional<ID> optionalIDInImage(Number24 number)
{
    return number == Number24::max ? std::nullopt : std::optional<ID>(ID(number));
}

constexpr Number24 optionalNumber24(uint32_t word)
{
    if (word == UINT32_MAX)
        return Number24(Number24::max);
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(word < Number24::max);
    return Number24(word);
}

enum class ModuleAliasKind : uint8_t;
constexpr uint32_t encodeModuleAlias(uint32_t module, ModuleAliasKind name) { return module << 2 | static_cast<uint32_t>(name); }

struct ModuleData {
    enum class Flag : uint16_t {
        HasTopLevelAwait = 1 << 0,
        RunsAgainAfterThrow = 1 << 0,
        IsSynchronous = 1 << 1,
        IsSimple = 1 << 2,
        NeedsLink = 1 << 3,
        ReachesUncompiledCode = 1 << 4,
        HasDynamicStarExport = 1 << 5,
        HasUnescapedExports = 1 << 5,
        IsEntryPoint = 1 << 6,
        CachesEvaluationError = 1 << 7,
        BodyMayRunAgain = 1 << 7,
        IsDataFoundByName = 1 << 7,
        HasDeferredRequest = 1 << 8,
        IsImportedAsESModule = 1 << 8,
        HasValue = 1 << 9,
        NeedsEvaluation = 1 << 10,
        IsLinkedWithEntryPoint = 1 << 11,
        EvaluatesItself = 1 << 12,
        IsLibraryCodeWithFilePath = 1 << 13,
        MayRunAsOriginal = 1 << 13,
        IsOfLibrary = 1 << 14,
        CapturesOnDemand = 1 << 15,
        OnlyPassesLinkOn = 1 << 15,
    };

    static constexpr uint8_t hasNoDisplayPath = UINT8_MAX - 1;
    static constexpr uint8_t displayPathIsListed = UINT8_MAX;

    uint32_t publicPathOrName;
    Number24 directory;
    uint8_t charactersInFrontOfDisplayPath;
    SlotData value;
    uint16_t flags;
    ModuleKind kind : 3;
    ExternalKind externalKind : 1;
    SyntheticExportRule exportRule : 2;
    RequireResult requireResult : 2;
    Number24 body;
    Number24 referrerRuntimeModuleOrLoadGroup;
    Number24 firstRequest;
    Number24 firstExport;
    Number24 firstSyntheticExport;
    Number24 firstRequiredName;
    Number24 firstLinkRequest;
    Number24 firstEvaluationRequest;
    uint8_t hasInstalledPositions : 1;
    uint8_t loadedByLoader : 1;
    uint8_t wrapsCommonJSModule : 1;
    uint8_t importedByModule : 1;
    uint8_t importedWhileItRunsOrAsNamespace : 1;
    uint8_t wrappedByFacade : 1;
    uint8_t evictableOrRejectsOverride : 1;
    uint8_t throwsAtLoadOrIsReloadable : 1 { 0 };

    bool has(Flag flag) const { return OptionSet<Flag>::fromRaw(flags).contains(flag); }
    void add(Flag flag) { flags |= static_cast<uint16_t>(flag); }
    bool hasTopLevelAwait() const { return kind != ModuleKind::CommonJs && has(Flag::HasTopLevelAwait); }
    bool bodyThrowsAtLoad() const { return kind != ModuleKind::CommonJs && throwsAtLoadOrIsReloadable; }
    bool isReloadable() const { return kind == ModuleKind::CommonJs && throwsAtLoadOrIsReloadable; }
    bool runsAgainAfterThrow() const { return kind == ModuleKind::CommonJs && has(Flag::RunsAgainAfterThrow); }
    bool isSynchronous() const { return has(Flag::IsSynchronous); }
    bool isSimple() const { return has(Flag::IsSimple); }
    bool needsLink() const { return has(Flag::NeedsLink); }
    bool onlyPassesLinkOn() const { return has(Flag::OnlyPassesLinkOn) && kind != ModuleKind::CommonJs; }
    bool reachesUncompiledCode() const { return has(Flag::ReachesUncompiledCode); }
    bool hasDynamicStarExport() const { return kind != ModuleKind::CommonJs && has(Flag::HasDynamicStarExport); }
    bool hasUnescapedExports() const { return kind == ModuleKind::CommonJs && has(Flag::HasUnescapedExports); }
    bool isEntryPoint() const { return has(Flag::IsEntryPoint); }
    bool cachesEvaluationError() const { return kind != ModuleKind::CommonJs || has(Flag::CachesEvaluationError); }
    bool bodyOfESModuleMayRunAgain() const { return kind == ModuleKind::SourceText && has(Flag::BodyMayRunAgain); }
    bool isDataFoundByName() const { return kind == ModuleKind::Data && has(Flag::IsDataFoundByName); }
    bool hasDeferredRequest() const { return kind != ModuleKind::CommonJs && has(Flag::HasDeferredRequest); }
    bool isImportedAsESModule() const { return kind == ModuleKind::CommonJs && has(Flag::IsImportedAsESModule); }
    bool hasValue() const { return has(Flag::HasValue); }
    bool needsEvaluation() const { return has(Flag::NeedsEvaluation); }
    bool isLinkedWithEntryPoint() const { return has(Flag::IsLinkedWithEntryPoint); }
    bool evaluatesItself() const { return has(Flag::EvaluatesItself); }
    bool isLoadedByLoader() const { return loadedByLoader; }
    bool isFacade() const { return wrapsCommonJSModule; }
    bool hasFacade() const { return wrappedByFacade; }
    bool mayBeEvicted() const { return !isOfLibrary() && evictableOrRejectsOverride; }
    bool rejectsRequireCacheOverride() const { return isOfLibrary() && evictableOrRejectsOverride; }
    bool isImportedByModule() const { return importedByModule; }
    bool isImportableDuringEvaluation() const { return kind == ModuleKind::CommonJs && importedWhileItRunsOrAsNamespace; }
    bool isImportedAsNamespace() const { return kind == ModuleKind::SourceText && importedWhileItRunsOrAsNamespace; }
    bool loaderReturnsNamespace() const { return kind == ModuleKind::Data && importedWhileItRunsOrAsNamespace; }
    bool hasRequireCacheValue() const { return (kind == ModuleKind::RuntimeSource || kind == ModuleKind::Runtime) && importedWhileItRunsOrAsNamespace; }
    SlotData requireCacheValue() const { return SlotData::create(value.offset + sizeof(EncodedJSValue), SlotRepresentation::JSValue); }
    bool isLibraryCodeWithFilePath() const { return kind != ModuleKind::CommonJs && has(Flag::IsLibraryCodeWithFilePath); }
    bool mayRunAsOriginal() const { return kind == ModuleKind::CommonJs && has(Flag::MayRunAsOriginal); }
    bool isOfLibrary() const { return has(Flag::IsOfLibrary); }
    bool hasLinkPlan() const { return isEntryPoint() && isLinkedWithEntryPoint(); }

    bool hasExportTable() const;
    bool isESModuleOfProgram() const { return kind == ModuleKind::SourceText && !isOfLibrary(); }
    bool isUncompiledCode() const { return kind == ModuleKind::External && externalKind == ExternalKind::UncompiledCode; }
    bool isUncompiledCodeImportedAsNamespace() const { return isUncompiledCode() && importedWhileItRunsOrAsNamespace; }
    bool isHostValue() const { return kind == ModuleKind::External && externalKind == ExternalKind::HostValue; }
    static constexpr ExternalKind mayBeMockedBit = static_cast<ExternalKind>(1);
    bool mayBeMocked() const { return kind != ModuleKind::External && externalKind == mayBeMockedBit; }
    bool capturesExports() const;
    bool capturesExportsBehindEvaluation() const { return capturesExports() && (kind == ModuleKind::SourceText || kind == ModuleKind::CommonJs); }
    bool linkFollowsRequests() const { return capturesExportsBehindEvaluation() || (!capturesExports() && !isUncompiledCode()); }
    bool holdsNamespaceInValue() const { return hasValue() && (hasExportTable() || isUncompiledCode()); }

    std::optional<FunctionID> bodyFunction() const { return optionalIDInImage<FunctionID>(body); }
    std::optional<ModuleID> referrerModule() const { return kind == ModuleKind::External ? optionalIDInImage<ModuleID>(referrerRuntimeModuleOrLoadGroup) : std::nullopt; }
    std::optional<ModuleID> evaluatorModule() const { return kind == ModuleKind::CommonJs ? optionalIDInImage<ModuleID>(referrerRuntimeModuleOrLoadGroup) : std::nullopt; }
    std::optional<uint32_t> registryID() const { return kind == ModuleKind::RuntimeSource || kind == ModuleKind::Runtime ? optionalIDInImage<uint32_t>(referrerRuntimeModuleOrLoadGroup) : std::nullopt; }

    enum class PlaceInLoadGroup : uint8_t { BelowHead, InCycleWithHead, Head, HeadThatRequestsUncompiledCode };
    static constexpr unsigned bitsOfNumberInLoadGroup = 22;
    static constexpr uint32_t endOfNumbersInLoadGroups = 1u << bitsOfNumberInLoadGroup;
    static Number24 encodeLoadGroup(PlaceInLoadGroup place, uint32_t number)
    {
        RELEASE_ASSERT(number < endOfNumbersInLoadGroups);
        return Number24(static_cast<uint32_t>(place) << bitsOfNumberInLoadGroup | number);
    }
    PlaceInLoadGroup placeInLoadGroup() const { return static_cast<PlaceInLoadGroup>(static_cast<uint32_t>(referrerRuntimeModuleOrLoadGroup) >> bitsOfNumberInLoadGroup); }
    uint32_t numberInLoadGroup() const { return static_cast<uint32_t>(referrerRuntimeModuleOrLoadGroup) & ((1u << bitsOfNumberInLoadGroup) - 1); }
    bool isHeadOfLoadGroup() const { return placeInLoadGroup() >= PlaceInLoadGroup::Head; }
};

inline bool ModuleData::hasExportTable() const
{
    switch (kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
        return true;
    case ModuleKind::CommonJs:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline bool ModuleData::capturesExports() const
{
    if (kind == ModuleKind::CommonJs && has(Flag::CapturesOnDemand))
        return false;
    switch (exportRule) {
    case SyntheticExportRule::CommonJs:
    case SyntheticExportRule::CommonJsIgnoringAnnotation:
    case SyntheticExportRule::RuntimeModule:
        return true;
    case SyntheticExportRule::None:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct ModuleDirectoryData {
    Number24 parent;
    Number24 name;

    std::optional<uint32_t> parentDirectory() const { return optionalIDInImage<uint32_t>(parent); }
};

struct ModuleDisplayPathData {
    Number24 module;
    Number24 path;
};

struct UncompiledModuleData {
    static constexpr uint32_t noTypeAttribute = UINT32_MAX;

    uint32_t module;
    uint32_t typeAttribute;
};

struct ModuleRequestData {
    static constexpr uint32_t maxTarget = (1u << 20) - 1;

    uint8_t targetBytes[2];
    uint8_t highBitsOfTarget : 4;
    uint8_t isHoisted : 1;
    uint8_t isDeferred : 1;
    uint8_t isDynamicStarExport : 1;
    uint8_t isRequire : 1;

    static constexpr ModuleRequestData create(uint32_t target, bool isDeferred, bool isDynamicStarExport, bool isRequire = false, bool isHoisted = false)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(target <= maxTarget);
        return { { static_cast<uint8_t>(target), static_cast<uint8_t>(target >> 8) }, static_cast<uint8_t>(target >> 16), isHoisted, isDeferred, isDynamicStarExport, isRequire };
    }
    constexpr uint32_t target() const { return targetBytes[0] | targetBytes[1] << 8 | highBitsOfTarget << 16; }
    ModuleID targetModule() const { return ModuleID(target()); }
};

class ModuleNumbers {
public:
    static constexpr bool needsTwoWords(size_t numberOfModules) { return numberOfModules > (1u << 16); }

    class Iterator {
    public:
        using value_type = uint32_t;
        using difference_type = std::ptrdiff_t;

        constexpr Iterator() = default;
        constexpr Iterator(const ModuleNumbers* numbers, size_t index)
            : m_numbers(numbers)
            , m_index(index)
        {
        }

        constexpr uint32_t operator*() const { return (*m_numbers)[m_index]; }
        constexpr Iterator& operator++() { ++m_index; return *this; }
        constexpr Iterator operator++(int) { Iterator before = *this; ++m_index; return before; }
        friend constexpr bool operator==(const Iterator&, const Iterator&) = default;

    private:
        const ModuleNumbers* m_numbers { nullptr };
        size_t m_index { 0 };
    };

    constexpr ModuleNumbers() = default;
    constexpr ModuleNumbers(std::span<const uint16_t> words, size_t numberOfModules)
        : m_words(words)
        , m_hasTwoWords(needsTwoWords(numberOfModules))
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(!m_hasTwoWords || !(words.size() % 2));
    }

    constexpr size_t size() const { return m_hasTwoWords ? m_words.size() / 2 : m_words.size(); }
    constexpr bool empty() const { return m_words.empty(); }
    constexpr uint32_t operator[](size_t index) const
    {
        if (!m_hasTwoWords) [[likely]]
            return m_words[index];
        return m_words[2 * index] | static_cast<uint32_t>(m_words[2 * index + 1]) << 16;
    }
    constexpr ModuleNumbers subspan(size_t start, size_t count) const
    {
        ModuleNumbers part;
        part.m_words = m_hasTwoWords ? m_words.subspan(2 * start, 2 * count) : m_words.subspan(start, count);
        part.m_hasTwoWords = m_hasTwoWords;
        return part;
    }
    constexpr bool contains(uint32_t module) const
    {
        for (size_t index = 0; index < size(); ++index) {
            if ((*this)[index] == module)
                return true;
        }
        return false;
    }
    ALWAYS_INLINE size_t findFirstOutside(std::span<const uint64_t> set, size_t start, size_t end) const
    {
        auto isOutside = [&](uint32_t number) ALWAYS_INLINE_LAMBDA {
            return !(set[number / 64] >> (number % 64) & 1);
        };
        if (m_hasTwoWords) [[unlikely]] {
            while (start != end && !isOutside((*this)[start]))
                ++start;
            return start;
        }
        for (uint16_t number : m_words.subspan(start, end - start)) {
            if (isOutside(number))
                break;
            ++start;
        }
        return start;
    }
    constexpr Iterator begin() const LIFETIME_BOUND { return { this, 0 }; }
    constexpr Iterator end() const LIFETIME_BOUND { return { this, size() }; }

private:
    std::span<const uint16_t> m_words;
    bool m_hasTwoWords { false };
};

struct LinkPlanStepData {
    uint32_t module : 31;
    uint32_t resolvesNames : 1;

    ModuleID targetModule() const { return ModuleID(static_cast<uint32_t>(module)); }
};

struct ModuleExportData {
    uint32_t name : 24;
    uint32_t kindBits : 3;
    uint32_t isHiddenUntilInitialized : 1;
    uint32_t staysIn : 1;
    uint32_t isReplaceable : 1;
    uint32_t isDeferred : 1;
    uint32_t isFromStarExport : 1;
    union {
        SlotData variable;
        uint32_t module;
        uint32_t indirectExport;
        uint32_t exportedField;
    };

    static constexpr uint32_t instanceAllocatorKindBits = 4;
    static constexpr uint32_t instanceGuardKindBits = 5;
    static constexpr uint32_t kindBitsOfWordOfMock = 6;
    static constexpr uint32_t instanceVariableKindBits = 7;
    static constexpr uint32_t severalInstancesKindBits = instanceAllocatorKindBits | 8;
    static_assert(numberOfEnumerators<ModuleExportKind> <= instanceAllocatorKindBits);

    bool isHidden() const { return kindBits >= instanceAllocatorKindBits; }
    uint32_t hiddenKindBits() const { return kindBits | isFromStarExport << 3; }
    void setHiddenKindBits(uint32_t bits)
    {
        kindBits = bits & 7;
        isFromStarExport = bits >> 3;
    }
    ModuleExportKind kind() const { return static_cast<ModuleExportKind>(kindBits); }
    ModuleID targetModule() const { return ModuleID(module); }
};

struct IndirectExportData {
    uint32_t module;
    uint32_t importName;

    ModuleID targetModule() const { return ModuleID(module); }
};

struct ExportedFieldData {
    uint32_t module;
    SlotData slot;

    ModuleID declaringModule() const { return ModuleID(module); }
};

static constexpr IndirectExportData endOfModuleInstances { std::numeric_limits<uint32_t>::max(), 0 };

struct ModuleInstanceData {
    uint32_t numberOfLinks;
    uint32_t reexportCount;
    SlotData namespaceField;
    SlotData deferredNamespaceField;
};

struct ModuleLinkData {
    uint32_t target : 29;
    uint32_t hasLink : 1;
    uint32_t hasMock : 1;
    uint32_t isResolved : 1;
    SlotData link;
    SlotData mock;
    uint32_t firstStep;

    ModuleID targetModule() const { return ModuleID(static_cast<uint32_t>(target)); }
};

struct LinkPathStepData {
    uint32_t module;
    uint32_t nextStep;

    static constexpr uint32_t noStep = std::numeric_limits<uint32_t>::max();

    ModuleID moduleOfStep() const { return ModuleID(module); }
};

struct ReexportedFieldData {
    uint32_t exportIndex;
    SlotData link;
    uint32_t module : 31;
    uint32_t hasLink : 1;
    uint32_t importName;

    ModuleID namedModule() const { return ModuleID(static_cast<uint32_t>(module)); }
};

struct SyntheticExportData {
    uint32_t name : 24;
    uint32_t absentIsUndefined : 1;
    uint32_t hasVariable : 1;
    uint32_t isBoundByRequiredName : 1;
    uint32_t isReplaceable : 1;
    uint32_t unused : 4;
    SlotData variable;
};

struct RequiredNameData {
    uint32_t exporter : 31;
    uint32_t mayBeAbsent : 1;
    uint32_t name;

    ModuleID exporterModule() const { return ModuleID(static_cast<uint32_t>(exporter)); }
};

struct ExportStatementData {
    uint32_t requiredName;
    uint32_t exportName : 31;
    uint32_t dropsExportIfAbsent : 1;
    uint32_t specifier : 31;
    uint32_t isOfStar : 1;

    StringID exportNameString() const { return StringID(exportName); }
    StringID specifierString() const { return StringID(specifier); }
};

struct AmbiguousExportData {
    uint32_t module;
    uint32_t name;

    StringID nameString() const { return StringID(name); }
};

struct SourceStampData {
    uint32_t module;
    uint32_t low;
    uint32_t high;

    uint64_t stamp() const { return static_cast<uint64_t>(high) << 32 | low; }
};

struct NameOfStatementData {
    uint32_t module;
    uint32_t name;

    StringID nameString() const { return StringID(name); }
};

struct RequestedOriginalData {
    uint32_t function;
    uint32_t specifier;
};

struct StarAsWrittenData {
    uint32_t module;
    uint32_t target;
    uint32_t place : 31;
    uint32_t isFollowed : 1;
};

struct WriterOfExportData {
    uint32_t module;
    uint32_t place;
    uint32_t writer;
};

struct ModuleFailureData {
    static constexpr uint32_t isAbsentExportBit = 1u << 31;
    static constexpr uint32_t isFromExportStatementBit = 1u << 30;
    static constexpr uint32_t isLoadOfModuleBit = 1u << 29;
    static constexpr uint32_t bits = isAbsentExportBit | isFromExportStatementBit | isLoadOfModuleBit;

    static ModuleFailureData ofAbsentExport(ModuleID referrer, ModuleID exporter, uint32_t name) { return { exporter.index() | isAbsentExportBit, name, referrer.index() }; }
    static ModuleFailureData ofExportStatement(ModuleID referrer, uint32_t exportSpecifier, uint32_t exportName)
    {
        RELEASE_ASSERT(!(exportSpecifier & bits));
        return { exportSpecifier | isAbsentExportBit | isFromExportStatementBit, exportName, referrer.index() };
    }
    static ModuleFailureData ofLoad(ModuleID module, ModuleFailureID failure) { return { module.index() | isLoadOfModuleBit, failure.index(), module.index() }; }

    uint32_t importer;
    uint32_t specifier;
    uint32_t referrer;

    bool isAbsentExport() const { return importer & isAbsentExportBit; }
    bool isFromExportStatement() const { return importer & isFromExportStatementBit; }
    bool isLoadOfModule() const { return importer & isLoadOfModuleBit; }
    ModuleFailureID failureOfLoad() const { ASSERT(isLoadOfModule()); return ModuleFailureID(specifier); }
    ModuleID importerModule() const { ASSERT(!isFromExportStatement()); return ModuleID(importer & ~bits); }
    ModuleID referrerModule() const { return ModuleID(referrer); }
    StringID exportSpecifierString() const { ASSERT(isFromExportStatement()); return StringID(importer & ~bits); }
    StringID specifierString() const { return StringID(specifier); }
};

struct ImportOfHeldFileData {
    uint64_t key() const { return static_cast<uint64_t>(file) << 32 | importer; }

    uint32_t file;
    uint32_t importer;
};

struct CompiledRuntimeModuleData {
    uint32_t registryID;
    uint32_t module : 30;
    uint32_t valueStaysWithCompiledCode : 1;
    uint32_t recordIsHandedOutFrozen : 1;
};

struct StateOfLibraryModuleData {
    enum class Kind : uint32_t { Classes, Closures, Variables };
    static constexpr unsigned kindBits = 2;

    static constexpr StateOfLibraryModuleData create(uint32_t module, Kind kind, uint32_t first, uint32_t count) { return { module << kindBits | static_cast<uint32_t>(kind), first, count }; }

    uint32_t moduleAndKind;
    uint32_t first;
    uint32_t count;

    uint32_t module() const { return moduleAndKind >> kindBits; }
    Kind kind() const { return static_cast<Kind>(moduleAndKind & ((1u << kindBits) - 1)); }
};

struct CompiledRuntimeFunctionData {
    uint32_t id;
    uint32_t function;
    uint32_t visibility;
};

struct ModuleStartStepData {
    enum class Operation : uint32_t {
        Trap,
        CallStart,
        CallBody,
        Begin,
        BeginOrReturn,
        Finish,
        ReturnIfLoadingStops,
        AbandonOnThrow,
        Return,
        FinishAndReturn,
        CallStartUnlessStopped,
    };

    static constexpr unsigned numberOfOperationBits = 4;
    static constexpr uint32_t operationMask = (1u << numberOfOperationBits) - 1;
    static constexpr uint32_t flagBit = 1u << numberOfOperationBits;
    static constexpr unsigned operandShift = numberOfOperationBits + 1;
    static constexpr uint32_t maxOperand = UINT32_MAX >> operandShift;

    static constexpr ModuleStartStepData create(Operation operation, uint32_t operand = 0, bool hasFlag = false)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(operand <= maxOperand);
        return { static_cast<uint32_t>(operation) | (hasFlag ? flagBit : 0) | operand << operandShift };
    }
    constexpr Operation operation() const { return static_cast<Operation>(bits & operationMask); }
    constexpr bool hasFlag() const { return bits & flagBit; }
    constexpr uint32_t operand() const { return bits >> operandShift; }
    constexpr bool callsStart() const { return operation() == Operation::CallStart || operation() == Operation::CallStartUnlessStopped; }
    constexpr bool endsRow() const { return operation() == Operation::Trap || operation() == Operation::Return || operation() == Operation::FinishAndReturn; }

    uint32_t bits;
};
static_assert(sizeof(ModuleStartStepData) == 4 && isImageRecord<ModuleStartStepData>);
static_assert(static_cast<uint32_t>(ModuleStartStepData::Operation::CallStartUnlessStopped) <= ModuleStartStepData::operationMask && Number24::max <= ModuleStartStepData::maxOperand);
static_assert(!ModuleStartStepData::create(ModuleStartStepData::Operation::Trap).bits);
static_assert(ModuleStartStepData::create(ModuleStartStepData::Operation::Finish, 0xabcdef, true).operand() == 0xabcdef && ModuleStartStepData::create(ModuleStartStepData::Operation::Finish, 0xabcdef, true).hasFlag());
static_assert(ModuleStartStepData::create(ModuleStartStepData::Operation::CallBody, ModuleStartStepData::maxOperand).operation() == ModuleStartStepData::Operation::CallBody && !ModuleStartStepData::create(ModuleStartStepData::Operation::CallBody, ModuleStartStepData::maxOperand).hasFlag());

static_assert(sizeof(ModuleData) == 40 && isImageRecord<ModuleData>);
static_assert(sizeof(ModuleDirectoryData) == 6 && isImageRecord<ModuleDirectoryData> && sizeof(ModuleDisplayPathData) == 6 && isImageRecord<ModuleDisplayPathData>);
static_assert(sizeof(UncompiledModuleData) == 8 && isImageRecord<UncompiledModuleData>);
static_assert(sizeof(ModuleRequestData) == 3 && alignof(ModuleRequestData) == 1 && isImageRecord<ModuleRequestData>);
static_assert(ModuleRequestData::create(0xbcdef, true, false, true).target() == 0xbcdef);
static_assert(sizeof(LinkPlanStepData) == 4 && isImageRecord<LinkPlanStepData>);
static_assert(sizeof(ModuleExportData) == 8 && isImageRecord<ModuleExportData>);
static_assert(sizeof(IndirectExportData) == 8 && isImageRecord<IndirectExportData>);
static_assert(sizeof(ModuleInstanceData) == 2 * sizeof(IndirectExportData) && sizeof(ModuleLinkData) == sizeof(ModuleInstanceData) && sizeof(ReexportedFieldData) == sizeof(ModuleInstanceData) && alignof(ModuleInstanceData) == alignof(IndirectExportData)
    && alignof(ModuleLinkData) == alignof(IndirectExportData) && alignof(ReexportedFieldData) == alignof(IndirectExportData) && isImageRecord<ModuleInstanceData> && isImageRecord<ModuleLinkData> && isImageRecord<ReexportedFieldData>);
static_assert(sizeof(LinkPathStepData) == sizeof(IndirectExportData) && alignof(LinkPathStepData) == alignof(IndirectExportData) && isImageRecord<LinkPathStepData>);
static_assert(sizeof(ExportedFieldData) == sizeof(IndirectExportData) && alignof(ExportedFieldData) == alignof(IndirectExportData) && isImageRecord<ExportedFieldData>);
static_assert(sizeof(SyntheticExportData) == 8 && isImageRecord<SyntheticExportData>);
static_assert(sizeof(RequiredNameData) == 8 && isImageRecord<RequiredNameData>);
static_assert(sizeof(ExportStatementData) == 12 && isImageRecord<ExportStatementData>);
static_assert(sizeof(AmbiguousExportData) == 8 && isImageRecord<AmbiguousExportData>);
static_assert(sizeof(SourceStampData) == 12 && isImageRecord<SourceStampData>);
static_assert(sizeof(NameOfStatementData) == 8 && isImageRecord<NameOfStatementData>);
static_assert(sizeof(RequestedOriginalData) == 8 && isImageRecord<RequestedOriginalData>);
static_assert(sizeof(StarAsWrittenData) == 12 && isImageRecord<StarAsWrittenData>);
static_assert(sizeof(WriterOfExportData) == 12 && isImageRecord<WriterOfExportData>);
static_assert(sizeof(ModuleFailureData) == 12 && isImageRecord<ModuleFailureData>);
static_assert(sizeof(ImportOfHeldFileData) == 8 && isImageRecord<ImportOfHeldFileData>);
static_assert(sizeof(StateOfLibraryModuleData) == 12 && isImageRecord<StateOfLibraryModuleData>);
static_assert(sizeof(CompiledRuntimeModuleData) == 8 && isImageRecord<CompiledRuntimeModuleData>);
static_assert(sizeof(CompiledRuntimeFunctionData) == 12 && isImageRecord<CompiledRuntimeFunctionData>);
static_assert(sizeof(ModuleData::Flag) == sizeof(ModuleData::flags));
static_assert(sizeof(ModuleKind) == 1 && sizeof(ExternalKind) == 1 && sizeof(SyntheticExportRule) == 1 && sizeof(RequireResult) == 1);
static_assert(static_cast<unsigned>(ModuleKind::External) < 8 && static_cast<unsigned>(ExternalKind::HostValue) < 2 && static_cast<unsigned>(SyntheticExportRule::RuntimeModule) < 4 && static_cast<unsigned>(RequireResult::NamespaceIfImported) < 4);

struct HashOfDirectory {
    static constexpr uint32_t ofNoText = 2166136261u;
    static constexpr unsigned bitsPerDirectory = 16;
    static constexpr uint32_t with(uint32_t hash, char16_t character) { return (hash ^ character) * 16777619u; }
    static constexpr size_t bit(uint32_t hash, size_t numberOfBits) { return (hash ^ hash >> 15) & (numberOfBits - 1); }
};

static constexpr uint32_t hashOfAnyQuery = HashOfDirectory::with(HashOfDirectory::ofNoText, '?');

class ModuleByHashOfPath {
public:
    constexpr ModuleByHashOfPath(size_t numberOfModuleRecords, unsigned hashOfPath)
        : m_bitsOfModule(std::bit_width(numberOfModuleRecords))
        , m_hash(hashOfPath * 0x9e3779b1u)
        , m_hashInEntry(m_hash << m_bitsOfModule)
    {
    }

    static constexpr size_t numberOfBuckets(size_t numberOfModules) { return (numberOfModules + 1) / 2; }
    constexpr size_t bucket(size_t numberOfBuckets) const { return static_cast<uint64_t>(m_hash) * numberOfBuckets >> 32; }
    constexpr uint32_t hash() const { return m_hash; }
    constexpr uint32_t entryFor(uint32_t module) const { return m_hashInEntry | module; }
    constexpr uint32_t moduleOrMore(uint32_t entry) const { return entry ^ m_hashInEntry; }
    constexpr bool isModule(uint32_t moduleOrMore) const { return !(moduleOrMore >> m_bitsOfModule); }

private:
    unsigned m_bitsOfModule;
    uint32_t m_hash;
    uint32_t m_hashInEntry;
};

#define FOR_EACH_AOT_MODULE_PROGRAM_TABLE(v) \
    v(modules, ModuleData) \
    v(moduleDirectories, ModuleDirectoryData) \
    v(moduleDisplayPaths, ModuleDisplayPathData) \
    v(moduleRequests, ModuleRequestData) \
    v(linkRequests, uint16_t) \
    v(evaluationRequests, uint16_t) \
    v(groupsLoadedWithGroups, uint16_t) \
    v(entryPointLinkPlan, LinkPlanStepData) \
    v(moduleExports, ModuleExportData) \
    v(indirectExports, IndirectExportData) \
    v(syntheticExports, SyntheticExportData) \
    v(requiredNames, RequiredNameData) \
    v(exportStatements, ExportStatementData) \
    v(ambiguousExports, AmbiguousExportData) \
    v(sourceStamps, SourceStampData) \
    v(moduleFailures, ModuleFailureData) \
    v(namesOfStatements, NameOfStatementData) \
    v(starsAsWritten, StarAsWrittenData) \
    v(writersOfExports, WriterOfExportData) \
    v(modulesByPath, uint16_t) \
    v(modulesByHashOfPath, uint32_t) \
    v(firstModulesByHashOfPath, uint16_t) \
    v(directoriesOfModulesByPath, uint32_t) \
    v(modulesOfUncompiledCode, UncompiledModuleData) \
    v(modulesRejectingUncompiledCode, uint32_t) \
    v(modulesWithReadOnlyBuiltins, uint32_t) \
    v(importedModuleAliases, uint32_t) \
    v(modulesThatDependOnMoreThanSource, uint32_t) \
    v(asyncFunctionsWithoutAwait, uint32_t) \
    v(functionsImportingOwnOriginal, uint32_t) \
    v(requestedOriginals, RequestedOriginalData) \
    v(importsOfHeldFiles, ImportOfHeldFileData) \
    v(compiledRuntimeModules, CompiledRuntimeModuleData) \
    v(stateOfLibraryModules, StateOfLibraryModuleData) \
    v(compiledRuntimeFunctions, CompiledRuntimeFunctionData) \
    v(moduleStarts, uint32_t) \
    v(moduleStartSteps, ModuleStartStepData)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
