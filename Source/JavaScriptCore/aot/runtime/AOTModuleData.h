/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTSlotRepresentation.h"
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

// None is the largest number.
constexpr Number24 optionalNumber24(uint32_t word)
{
    if (word == UINT32_MAX)
        return Number24(Number24::max);
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(word < Number24::max);
    return Number24(word);
}

struct ModuleData {
    enum class Flag : uint16_t {
        HasTopLevelAwait = 1 << 0,
        IsSynchronous = 1 << 1,
        IsSimple = 1 << 2,
        NeedsLink = 1 << 3,
        ReachesUncompiledCode = 1 << 4,
        HasDynamicStarExport = 1 << 5,
        IsEntryPoint = 1 << 6,
        KeepsError = 1 << 7,
        HasDeferredRequest = 1 << 8,
        HasValue = 1 << 9,
        NeedsEvaluation = 1 << 10,
        IsLinkedWithEntryPoint = 1 << 11,
        EvaluatesItself = 1 << 12,
        IsLibraryCodeWithFilePath = 1 << 13,
        IsOfLibrary = 1 << 14,
        CapturesOnDemand = 1 << 15,
    };

    static constexpr uint8_t hasNoDisplayPath = UINT8_MAX - 1;
    static constexpr uint8_t displayPathIsListed = UINT8_MAX;

    // The public path. Of a module that has a directory: what stands behind the last slash of it.
    uint32_t publicPathOrName;
    // In ProgramTables::moduleDirectories(). The largest number: none.
    Number24 directory;
    // The display path is the public path without as many characters at its start. Where it is not, it stands in ProgramTables::moduleDisplayPaths().
    uint8_t charactersInFrontOfDisplayPath;
    // With HasValue.
    SlotData value;
    uint16_t flags;
    ModuleKind kind : 3;
    ExternalKind externalKind : 1;
    SyntheticExportRule exportRule : 2;
    RequireResult requireResult : 2;
    Number24 body;
    // Of a module outside the image: the module that names it. Of a module of the library: its number in the host's registry. No other module has either.
    Number24 referrerOrRuntimeModule;
    Number24 firstRequest;
    Number24 firstExport;
    Number24 firstSyntheticExport;
    Number24 firstRequiredName;
    Number24 firstLinkRequest;
    Number24 firstEvaluationRequest;
    uint8_t hasInstalledPositions : 1;
    uint8_t loadedByLoader : 1;
    uint8_t faceOfFile : 1;
    uint8_t unusedBits : 5;

    bool has(Flag flag) const { return OptionSet<Flag>::fromRaw(flags).contains(flag); }
    void add(Flag flag) { flags |= static_cast<uint16_t>(flag); }
    bool hasTopLevelAwait() const { return has(Flag::HasTopLevelAwait); }
    bool isSynchronous() const { return has(Flag::IsSynchronous); }
    bool isSimple() const { return has(Flag::IsSimple); }
    bool needsLink() const { return has(Flag::NeedsLink); }
    bool reachesUncompiledCode() const { return has(Flag::ReachesUncompiledCode); }
    bool hasDynamicStarExport() const { return has(Flag::HasDynamicStarExport); }
    bool isEntryPoint() const { return has(Flag::IsEntryPoint); }
    // Whoever asks for the module after its body threw gets the same error. Without it the module counts as evaluated, with what it had exported by then.
    bool keepsError() const { return has(Flag::KeepsError); }
    bool hasDeferredRequest() const { return has(Flag::HasDeferredRequest); }
    bool hasValue() const { return has(Flag::HasValue); }
    bool needsEvaluation() const { return has(Flag::NeedsEvaluation); }
    bool isLinkedWithEntryPoint() const { return has(Flag::IsLinkedWithEntryPoint); }
    // Its body evaluates what the module requests, runs the code of the module and stores the status, all of it once.
    bool evaluatesItself() const { return has(Flag::EvaluatesItself); }
    bool isLoadedByLoader() const { return loadedByLoader; }
    // Its first request is the CommonJS file that it stands for where ES modules import the file.
    bool isFaceOfFile() const { return faceOfFile; }
    // The helpers of the bundler, which have the form of a file of the program and answer to their name like a module of the library.
    bool isLibraryCodeWithFilePath() const { return has(Flag::IsLibraryCodeWithFilePath); }
    // Code of the host, whatever its form.
    bool isOfLibrary() const { return has(Flag::IsOfLibrary); }
    // Of the entry points, only the one that entryPointLinkPlan belongs to is linked with it.
    bool hasLinkPlan() const { return isEntryPoint() && isLinkedWithEntryPoint(); }

    bool hasExportTable() const;
    bool isUncompiledCode() const { return kind == ModuleKind::External && externalKind == ExternalKind::UncompiledCode; }
    bool isHostValue() const { return kind == ModuleKind::External && externalKind == ExternalKind::HostValue; }
    bool capturesExports() const;
    // A file whose names an ES importer finds only in the object that the file exports. They are captured behind its evaluation, which no link starts.
    bool capturesExportsBehindEvaluation() const { return capturesExports() && (kind == ModuleKind::SourceText || kind == ModuleKind::CommonJs); }
    bool holdsNamespaceInValue() const { return hasValue() && (hasExportTable() || isUncompiledCode()); }

    std::optional<FunctionID> bodyFunction() const { return optionalIDInImage<FunctionID>(body); }
    std::optional<ModuleID> referrerModule() const { return kind == ModuleKind::External ? optionalIDInImage<ModuleID>(referrerOrRuntimeModule) : std::nullopt; }
    // Of a text that the program evaluates: the file that evaluates it.
    std::optional<ModuleID> evaluatorModule() const { return kind == ModuleKind::CommonJs ? optionalIDInImage<ModuleID>(referrerOrRuntimeModule) : std::nullopt; }
    std::optional<uint32_t> registryID() const { return kind == ModuleKind::External || kind == ModuleKind::CommonJs ? std::nullopt : optionalIDInImage<uint32_t>(referrerOrRuntimeModule); }
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

// No importer binds a name of a CommonJS file with CapturesOnDemand at run time. Its rule says what its namespace object shows, for whoever asks for that object.
inline bool ModuleData::capturesExports() const
{
    if (has(Flag::CapturesOnDemand))
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

// The files of a large program lie in a few thousand directories, and each of those in another: a directory is the one above it and its own name, which ends with the slash.
struct ModuleDirectoryData {
    Number24 parent;
    Number24 name;

    std::optional<uint32_t> parentDirectory() const { return optionalIDInImage<uint32_t>(parent); }
};

// In the order of the modules.
struct ModuleDisplayPathData {
    Number24 module;
    Number24 path;
};

struct ModuleRequestData {
    static constexpr uint32_t maxTarget = (1u << 20) - 1;

    uint8_t targetBytes[2];
    uint8_t highBitsOfTarget : 4;
    // The build put the request in front of those that the module writes: something below the module asks for the target, which runs while the graph is loaded.
    uint8_t isHoisted : 1;
    uint8_t isDeferred : 1;
    uint8_t isDynamicStarExport : 1;
    // A require() that the code of the requesting module answers by itself: it evaluates the target where it stands, so no walk of an evaluation follows it.
    uint8_t isRequire : 1;

    static constexpr ModuleRequestData create(uint32_t target, bool isDeferred, bool isDynamicStarExport, bool isRequire = false, bool isHoisted = false)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(target <= maxTarget);
        return { { static_cast<uint8_t>(target), static_cast<uint8_t>(target >> 8) }, static_cast<uint8_t>(target >> 16), isHoisted, isDeferred, isDynamicStarExport, isRequire };
    }
    constexpr uint32_t target() const { return targetBytes[0] | targetBytes[1] << 8 | highBitsOfTarget << 16; }
    ModuleID targetModule() const { return ModuleID(target()); }
};

// The numbers of modules in a table of 16-bit words: one word each while every module of the program has a number under 2^16, and two words, the low one first, in a larger program.
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
    // The place of the first number from `start` on that has no bit in the set, or `end`.
    size_t findFirstOutside(std::span<const uint64_t> set, size_t start, size_t end) const
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

// What a walk of all the requests of the entry point does while no module is linked, in the order of that walk.
struct LinkPlanStepData {
    uint32_t module : 31;
    // Otherwise the module captures exports.
    uint32_t resolvesNames : 1;

    ModuleID targetModule() const { return ModuleID(static_cast<uint32_t>(module)); }
};

// A link also starts at a module that an import() names, or that no module requests.
struct LinkPlanRootData {
    uint32_t module;
    uint32_t firstStep;
    uint32_t memberOfCycle;
};

struct ModuleExportData {
    uint32_t name : 24;
    uint32_t kindBits : 6;
    uint32_t isDeferred : 1;
    uint32_t isFromStarExport : 1;
    union {
        SlotData variable;
        uint32_t module;
        uint32_t indirectExport;
    };

    ModuleExportKind kind() const { return static_cast<ModuleExportKind>(kindBits); }
    ModuleID targetModule() const { return ModuleID(module); }
};

struct IndirectExportData {
    uint32_t module;
    uint32_t importName;

    ModuleID targetModule() const { return ModuleID(module); }
};

struct SyntheticExportData {
    uint32_t name : 24;
    uint32_t absentIsUndefined : 1;
    uint32_t hasVariable : 1;
    // Of a module of the library: every module that binds the name has it among its required names. Where the host defers the export, the link of the first of them reads it.
    uint32_t isBoundByRequiredName : 1;
    uint32_t unused : 5;
    SlotData variable;
};

struct RequiredNameData {
    uint32_t exporter : 31;
    uint32_t mayBeAbsent : 1;
    uint32_t name;

    ModuleID exporterModule() const { return ModuleID(static_cast<uint32_t>(exporter)); }
};

// A specifier that an importer could not resolve at the build, or a name that a module turned out to lack: then the module stands in the place of the importer and the name in that of the specifier.
struct ModuleFailureData {
    static constexpr uint32_t isAbsentExportBit = 1u << 31;

    static ModuleFailureData ofAbsentExport(ModuleID exporter, uint32_t name) { return { exporter.index() | isAbsentExportBit, name }; }

    uint32_t importer;
    uint32_t specifier;

    bool isAbsentExport() const { return importer & isAbsentExportBit; }
    ModuleID importerModule() const { return ModuleID(importer & ~isAbsentExportBit); }
    StringID specifierString() const { return StringID(specifier); }
};

struct CompiledRuntimeModuleData {
    uint32_t registryID;
    uint32_t module;
};

struct CompiledRuntimeFunctionData {
    uint32_t id;
    uint32_t function;
    uint32_t visibility;
};

static_assert(sizeof(ModuleData) == 40 && isImageRecord<ModuleData>);
static_assert(sizeof(ModuleDirectoryData) == 6 && isImageRecord<ModuleDirectoryData> && sizeof(ModuleDisplayPathData) == 6 && isImageRecord<ModuleDisplayPathData>);
static_assert(sizeof(ModuleRequestData) == 3 && alignof(ModuleRequestData) == 1 && isImageRecord<ModuleRequestData>);
static_assert(ModuleRequestData::create(0xbcdef, true, false, true).target() == 0xbcdef);
static_assert(sizeof(LinkPlanStepData) == 4 && isImageRecord<LinkPlanStepData>);
static_assert(sizeof(LinkPlanRootData) == 12 && isImageRecord<LinkPlanRootData>);
static_assert(sizeof(ModuleExportData) == 8 && isImageRecord<ModuleExportData>);
static_assert(sizeof(IndirectExportData) == 8 && isImageRecord<IndirectExportData>);
static_assert(sizeof(SyntheticExportData) == 8 && isImageRecord<SyntheticExportData>);
static_assert(sizeof(RequiredNameData) == 8 && isImageRecord<RequiredNameData>);
static_assert(sizeof(ModuleFailureData) == 8 && isImageRecord<ModuleFailureData>);
static_assert(sizeof(CompiledRuntimeModuleData) == 8 && isImageRecord<CompiledRuntimeModuleData>);
static_assert(sizeof(CompiledRuntimeFunctionData) == 12 && isImageRecord<CompiledRuntimeFunctionData>);
static_assert(sizeof(ModuleData::Flag) == sizeof(ModuleData::flags));
static_assert(sizeof(ModuleKind) == 1 && sizeof(ExternalKind) == 1 && sizeof(SyntheticExportRule) == 1 && sizeof(RequireResult) == 1);
static_assert(static_cast<unsigned>(ModuleKind::External) < 8 && static_cast<unsigned>(ExternalKind::HostValue) < 2 && static_cast<unsigned>(SyntheticExportRule::RuntimeModule) < 4 && static_cast<unsigned>(RequireResult::DefaultExport) < 4);

#define FOR_EACH_AOT_MODULE_PROGRAM_TABLE(v) \
    v(modules, ModuleData) \
    v(moduleDirectories, ModuleDirectoryData) \
    v(moduleDisplayPaths, ModuleDisplayPathData) \
    v(moduleRequests, ModuleRequestData) \
    v(linkRequests, uint16_t) \
    v(evaluationRequests, uint16_t) \
    v(entryPointLinkPlan, LinkPlanStepData) \
    v(linkPlanRoots, LinkPlanRootData) \
    v(linkPlanStepsOfRoots, LinkPlanStepData) \
    v(moduleExports, ModuleExportData) \
    v(indirectExports, IndirectExportData) \
    v(syntheticExports, SyntheticExportData) \
    v(requiredNames, RequiredNameData) \
    v(moduleFailures, ModuleFailureData) \
    v(modulesByPath, uint16_t) \
    v(modulesOfUncompiledCode, uint32_t) \
    v(compiledRuntimeModules, CompiledRuntimeModuleData) \
    v(compiledRuntimeFunctions, CompiledRuntimeFunctionData)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
