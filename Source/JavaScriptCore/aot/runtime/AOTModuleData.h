/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTSlotRepresentation.h"
#include "AOTVarint.h"
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

enum class OtherNameOfModule : uint8_t;
// An entry of ProgramTables::modulesReachedUnderOtherNames(), which lists the modules that a test may mock. Ascending.
constexpr uint32_t moduleReachedUnder(uint32_t module, OtherNameOfModule name) { return module << 2 | static_cast<uint32_t>(name); }

struct ModuleData {
    enum class Flag : uint16_t {
        HasTopLevelAwait = 1 << 0,
        RunsAgainAfterThrow = 1 << 0,
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
    // Of a module of any other kind than External the bit says that a test may mock it.
    ExternalKind externalKind : 1;
    SyntheticExportRule exportRule : 2;
    RequireResult requireResult : 2;
    Number24 body;
    // Of a module outside the image: the module that names it. Of a module of the library: its number in the host's registry.
    // Of an ES module of the program: the head of its group, if that is another module. Of a head: the number of modules and the place of its list in ProgramTables::groupsLoadedWithGroups(), if it has one.
    Number24 referrerRuntimeModuleOrLoadGroup;
    Number24 firstRequest;
    Number24 firstExport;
    Number24 firstSyntheticExport;
    Number24 firstRequiredName;
    Number24 firstLinkRequest;
    Number24 firstEvaluationRequest;
    uint8_t hasInstalledPositions : 1;
    uint8_t loadedByLoader : 1;
    uint8_t faceOfFile : 1;
    uint8_t importedByModule : 1;
    // Of a data file the bit says that the loader answers a require() of it with its namespace object.
    // Of a module of the library without an export table: the word behind `value` answers a require() of the program.
    uint8_t importedWhileItRunsOrAsNamespace : 1;
    uint8_t beginsLoads : 1;
    uint8_t shownByFace : 1;
    // The code that asks for the file was compiled for a file that may be taken out of require.cache and load again.
    // Of a module of the library the bit says that a require() of the program that names it without `node:` was compiled for the module, whatever require.cache holds under that name.
    uint8_t evictableOrHeldUnderNameWithoutPrefix : 1;

    bool has(Flag flag) const { return OptionSet<Flag>::fromRaw(flags).contains(flag); }
    void add(Flag flag) { flags |= static_cast<uint16_t>(flag); }
    bool hasTopLevelAwait() const { return kind != ModuleKind::CommonJs && has(Flag::HasTopLevelAwait); }
    bool runsAgainAfterThrow() const { return kind == ModuleKind::CommonJs && has(Flag::RunsAgainAfterThrow); }
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
    bool hasFace() const { return shownByFace; }
    bool mayBeEvicted() const { return !isOfLibrary() && evictableOrHeldUnderNameWithoutPrefix; }
    bool isHeldUnderNameWithoutPrefix() const { return isOfLibrary() && evictableOrHeldUnderNameWithoutPrefix; }
    // Some module requests it with what it writes, and not by a require(): a graph of imports can come to it from above.
    bool isImportedByModule() const { return importedByModule; }
    // Something other than the import of a module may be the first to ask for it: the host, an import(), a require().
    bool mayBeginLoad() const { return beginsLoads; }
    // A CommonJS file that leads by a require() to an ES module that imports it. No code of the image takes what a require() of it answers for what the file exports.
    bool mayBeImportedWhileItRuns() const { return kind == ModuleKind::CommonJs && importedWhileItRunsOrAsNamespace; }
    // An ES module that another module imports with `import * as`. In plain Bun the link of that importer creates the namespace object.
    bool isImportedAsNamespace() const { return kind == ModuleKind::SourceText && importedWhileItRunsOrAsNamespace; }
    // A file that stands for a string. `bun build` puts the string where it links a require(), which is what requireResult says. Every other require() goes to the loader, as in `bun run`.
    bool answersLoaderWithNamespace() const { return kind == ModuleKind::Data && importedWhileItRunsOrAsNamespace; }
    bool hasRequireCacheValue() const { return (kind == ModuleKind::RuntimeSource || kind == ModuleKind::Runtime) && importedWhileItRunsOrAsNamespace; }
    SlotData requireCacheValue() const { return SlotData::create(value.offset + sizeof(EncodedJSValue), SlotRepresentation::JSValue); }
    // The helpers of the bundler, which have the form of a file of the program and answer to their name like a module of the library.
    bool isLibraryCodeWithFilePath() const { return has(Flag::IsLibraryCodeWithFilePath); }
    // Code of the host, whatever its form.
    bool isOfLibrary() const { return has(Flag::IsOfLibrary); }
    // Of the entry points, only the one that entryPointLinkPlan belongs to is linked with it.
    bool hasLinkPlan() const { return isEntryPoint() && isLinkedWithEntryPoint(); }

    bool hasExportTable() const;
    bool isESModuleOfProgram() const { return kind == ModuleKind::SourceText && !isOfLibrary(); }
    bool isUncompiledCode() const { return kind == ModuleKind::External && externalKind == ExternalKind::UncompiledCode; }
    bool isUncompiledCodeImportedAsNamespace() const { return isUncompiledCode() && importedWhileItRunsOrAsNamespace; }
    bool isHostValue() const { return kind == ModuleKind::External && externalKind == ExternalKind::HostValue; }
    static constexpr ExternalKind mayBeMockedBit = static_cast<ExternalKind>(1);
    bool mayBeMocked() const { return kind != ModuleKind::External && externalKind == mayBeMockedBit; }
    bool capturesExports() const;
    // A file whose names an ES importer finds only in the object that the file exports. They are captured behind its evaluation, which no link starts.
    bool capturesExportsBehindEvaluation() const { return capturesExports() && (kind == ModuleKind::SourceText || kind == ModuleKind::CommonJs); }
    bool holdsNamespaceInValue() const { return hasValue() && (hasExportTable() || isUncompiledCode()); }

    std::optional<FunctionID> bodyFunction() const { return optionalIDInImage<FunctionID>(body); }
    std::optional<ModuleID> referrerModule() const { return kind == ModuleKind::External ? optionalIDInImage<ModuleID>(referrerRuntimeModuleOrLoadGroup) : std::nullopt; }
    // Of a text that the program evaluates: the file that evaluates it.
    std::optional<ModuleID> evaluatorModule() const { return kind == ModuleKind::CommonJs ? optionalIDInImage<ModuleID>(referrerRuntimeModuleOrLoadGroup) : std::nullopt; }
    std::optional<uint32_t> registryID() const { return kind == ModuleKind::External || kind == ModuleKind::CommonJs || isESModuleOfProgram() ? std::nullopt : optionalIDInImage<uint32_t>(referrerRuntimeModuleOrLoadGroup); }
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

// In the order of the modules.
struct UncompiledModuleData {
    static constexpr uint32_t noTypeAttribute = UINT32_MAX;

    uint32_t module;
    // `import data from "./data.txt" with { type: "toml" }`: what the statement that names the module says of its type. The loader of the host is asked with it.
    uint32_t typeAttribute;
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
    uint32_t kindBits : 3;
    // A TypeScript module exports a name of a CommonJS file, and was linked whether or not the file has it. The variable holds what the file had. While it is empty the name is no export.
    uint32_t isDroppedWhileEmpty : 1;
    // ModuleFlow::exportsClosedToOutside
    uint32_t isClosedToOutside : 1;
    uint32_t isReplaceable : 1;
    uint32_t isDeferred : 1;
    uint32_t isFromStarExport : 1;
    union {
        SlotData variable;
        uint32_t module;
        uint32_t indirectExport;
    };

    // Of a variable that the build let go of. The entry is there for its name, and isClosedToOutside.
    static constexpr SlotData noSlot { SlotData::maxOffset, 0 };
    // No export: the last entry of a module that a test may mock ahead of its load. Its variable holds what stands in place of the module, and is empty while nothing does.
    static constexpr uint32_t kindBitsOfWordOfMock = 3;

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
    // Of a module of the library, and not of a name that only a mock takes away: every module that reads the variable without a test has the name among its required names. Where the host defers the export, the link of the first of them reads it.
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

// `export { name as exportName } from "specifier"` asks for a required name: what the error of a link that does not find the name quotes. Sorted by the number of the required name.
struct ExportStatementData {
    uint32_t requiredName;
    uint32_t exportName : 31;
    // A link that does not find the required name goes on, and the name that the statement exports is no export.
    uint32_t dropsExportIfAbsent : 1;
    uint32_t specifier;

    StringID exportNameString() const { return StringID(exportName); }
    StringID specifierString() const { return StringID(specifier); }
};

// `export * from "./a"; export * from "./b"`, and both export the name: the module does not export it, and whoever outside the image asks for it is told why. Sorted by the module.
struct AmbiguousExportData {
    uint32_t module;
    uint32_t name;

    StringID nameString() const { return StringID(name); }
};

// What ModuleHost::stampOfFile() said of the file when the build read it. Of a file that the run may read again from disk. Sorted by the module.
struct SourceStampData {
    uint32_t module;
    uint32_t low;
    uint32_t high;

    uint64_t stamp() const { return static_cast<uint64_t>(high) << 32 | low; }
};

// A specifier that an importer could not resolve at the build, or a name that a module turned out to lack: then the module stands in the place of the importer and the name in that of the specifier.
// Where `export { .. as name } from "specifier"` asks for what the module lacks: what the statement is written from stands in the place of the importer, and the name that it exports in that of the specifier.
// Behind the failures, which code names by their place in the table, stand the modules that cannot load, sorted: each in the place of the importer, and in that of the specifier the number of the failure that its load ends with.
struct ModuleFailureData {
    static constexpr uint32_t isAbsentExportBit = 1u << 31;
    static constexpr uint32_t isAskedByExportStatementBit = 1u << 30;
    static constexpr uint32_t isLoadOfModuleBit = 1u << 29;
    static constexpr uint32_t bits = isAbsentExportBit | isAskedByExportStatementBit | isLoadOfModuleBit;

    static ModuleFailureData ofAbsentExport(ModuleID exporter, uint32_t name) { return { exporter.index() | isAbsentExportBit, name }; }
    static ModuleFailureData ofExportStatement(uint32_t exportSpecifier, uint32_t exportName)
    {
        RELEASE_ASSERT(!(exportSpecifier & bits));
        return { exportSpecifier | isAbsentExportBit | isAskedByExportStatementBit, exportName };
    }
    static ModuleFailureData ofLoad(ModuleID module, ModuleFailureID failure) { return { module.index() | isLoadOfModuleBit, failure.index() }; }

    uint32_t importer;
    uint32_t specifier;

    bool isAbsentExport() const { return importer & isAbsentExportBit; }
    bool isAskedByExportStatement() const { return importer & isAskedByExportStatementBit; }
    bool isLoadOfModule() const { return importer & isLoadOfModuleBit; }
    ModuleFailureID failureOfLoad() const { ASSERT(isLoadOfModule()); return ModuleFailureID(specifier); }
    ModuleID importerModule() const { ASSERT(!isAskedByExportStatement()); return ModuleID(importer & ~bits); }
    StringID exportSpecifierString() const { ASSERT(isAskedByExportStatement()); return StringID(importer & ~bits); }
    StringID specifierString() const { return StringID(specifier); }
};

// An ES module that imports a file whose require() answers depending on whether it is imported.
struct ImportOfHeldFileData {
    uint64_t key() const { return static_cast<uint64_t>(file) << 32 | importer; }

    uint32_t file;
    uint32_t importer;
};

// What a heap snapshot shows under the name of a module's variable. The image holds these as bytes (forEachNamedVariable).
struct NamedVariableData {
    enum class Place : uint32_t { Word, NarrowWord, Closure };
    static constexpr unsigned placeBits = 2;

    static constexpr NamedVariableData inStatics(uint32_t name, uint32_t offset, bool isNarrow) { return { name, offset | static_cast<uint32_t>(isNarrow ? Place::NarrowWord : Place::Word) }; }
    static constexpr NamedVariableData closureOf(uint32_t name, uint32_t function) { return { name, function << placeBits | static_cast<uint32_t>(Place::Closure) }; }

    uint32_t name;
    uint32_t placeAndNumber;

    StringID nameString() const { return StringID(name); }
    Place place() const { return static_cast<Place>(placeAndNumber & ((1u << placeBits) - 1)); }
    uint32_t offset() const { return placeAndNumber & ~((1u << placeBits) - 1); }
    FunctionID function() const { return FunctionID(placeAndNumber >> placeBits); }
};

// The table namedVariables, for each module that has such variables: how many, then each of them, in the order of their names: its name less
// the one before, then its place less the last place of its kind, in any module, above two bits for the kind of place. Only a heap snapshot reads it.
class NamedVariablesWriter {
public:
    void beginModule(size_t numberOfVariables) { appendVarint(m_bytes, numberOfVariables); m_lastName = 0; }
    void append(const NamedVariableData& variable)
    {
        uint32_t number = variable.placeAndNumber >> NamedVariableData::placeBits;
        uint32_t& last = m_lastNumber[variable.place() == NamedVariableData::Place::Closure];
        appendVarint(m_bytes, variable.name - m_lastName);
        appendVarint(m_bytes, zigZagEncode(static_cast<int64_t>(number) - last) << NamedVariableData::placeBits | static_cast<uint32_t>(variable.place()));
        m_lastName = variable.name;
        last = number;
    }
    const Vector<uint8_t>& bytes() const { return m_bytes; }

private:
    Vector<uint8_t> m_bytes;
    uint32_t m_lastName { 0 };
    uint32_t m_lastNumber[2] { };
};

// False if the bytes do not end where a module ends.
bool forEachNamedVariable(std::span<const uint8_t> bytes, NOESCAPE const auto& beginModule, NOESCAPE const auto& visit)
{
    const uint8_t* cursor = bytes.data();
    const uint8_t* end = cursor + bytes.size();
    uint32_t lastNumber[2] { };
    while (cursor < end) {
        size_t numberOfVariables = readVarint(cursor);
        beginModule(numberOfVariables);
        uint32_t name = 0;
        for (size_t index = 0; index < numberOfVariables; ++index) {
            if (cursor >= end)
                return false;
            name += static_cast<uint32_t>(readVarint(cursor));
            uint64_t word = readVarint(cursor);
            uint32_t place = static_cast<uint32_t>(word & ((1u << NamedVariableData::placeBits) - 1));
            uint32_t& last = lastNumber[place == static_cast<uint32_t>(NamedVariableData::Place::Closure)];
            last += static_cast<uint32_t>(zigZagDecode(word >> NamedVariableData::placeBits));
            visit(NamedVariableData { name, last << NamedVariableData::placeBits | place });
        }
    }
    return cursor == end;
}

// What a heap snapshot shows under the name of a variable that a closure holds a copy of.
struct NamedCaptureData {
    uint32_t function;
    uint32_t slot;
    uint32_t name;

    StringID nameString() const { return StringID(name); }
};

// The slots and names of one function, the slots rising.
using NamedCaptureList = Vector<std::pair<uint32_t, uint32_t>, 8>;

// The lists that were written or read last, each once, the latest first. Closures made side by side hold the same variables.
class RecentNamedCaptureLists {
public:
    size_t size() const { return m_lists.size(); }
    size_t find(const NamedCaptureList& list) const { return m_lists.find(list); }
    const NamedCaptureList& moveToFront(size_t index)
    {
        NamedCaptureList list = WTF::move(m_lists[index]);
        m_lists.removeAt(index);
        m_lists.insert(0, WTF::move(list));
        return m_lists[0];
    }
    void add(const NamedCaptureList& list)
    {
        if (m_lists.size() == 64)
            m_lists.removeLast();
        m_lists.insert(0, list);
    }

private:
    Vector<NamedCaptureList> m_lists;
};

// The table namedCaptures, for each function whose closures hold such copies, in the order of the functions: the function less the one before, then a word.
// Odd: above the bit, which of the recent lists is the list of this function too. Even: above the bit, the slots as bits, then a name for each;
// zero: how many, then the slot and the name of each. Only a heap snapshot reads it.
class NamedCapturesWriter {
public:
    void append(uint32_t function, const NamedCaptureList& list)
    {
        appendVarint(m_bytes, function - m_lastFunction);
        m_lastFunction = function;
        if (size_t index = m_recent.find(list); index != notFound) {
            appendVarint(m_bytes, index << 1 | 1);
            m_recent.moveToFront(index);
            return;
        }
        m_recent.add(list);
        if (list.last().first < 62) {
            uint64_t slots = 0;
            for (auto [slot, name] : list)
                slots |= 1ull << slot;
            appendVarint(m_bytes, slots << 1);
            for (auto [slot, name] : list)
                appendVarint(m_bytes, name);
            return;
        }
        appendVarint(m_bytes, 0);
        appendVarint(m_bytes, list.size());
        for (auto [slot, name] : list) {
            appendVarint(m_bytes, slot);
            appendVarint(m_bytes, name);
        }
    }
    const Vector<uint8_t>& bytes() const { return m_bytes; }

private:
    Vector<uint8_t> m_bytes;
    RecentNamedCaptureLists m_recent;
    uint32_t m_lastFunction { 0 };
};

// False if the bytes do not end where a function ends, or name a list that is not there.
bool forEachNamedCapture(std::span<const uint8_t> bytes, NOESCAPE const auto& visit)
{
    const uint8_t* cursor = bytes.data();
    const uint8_t* end = cursor + bytes.size();
    RecentNamedCaptureLists recent;
    uint32_t function = 0;
    auto visitAll = [&](const NamedCaptureList& list) {
        for (auto [slot, name] : list)
            visit(NamedCaptureData { function, slot, name });
    };
    while (cursor < end) {
        function += static_cast<uint32_t>(readVarint(cursor));
        if (cursor >= end)
            return false;
        uint64_t word = readVarint(cursor);
        if (word & 1) {
            if (word >> 1 >= recent.size())
                return false;
            visitAll(recent.moveToFront(word >> 1));
            continue;
        }
        NamedCaptureList list;
        if (uint64_t slots = word >> 1) {
            for (uint32_t slot = 0; slots; ++slot, slots >>= 1) {
                if (!(slots & 1))
                    continue;
                if (cursor >= end)
                    return false;
                list.append({ slot, static_cast<uint32_t>(readVarint(cursor)) });
            }
        } else {
            if (cursor >= end)
                return false;
            size_t numberOfCaptures = readVarint(cursor);
            for (size_t index = 0; index < numberOfCaptures; ++index) {
                if (cursor >= end)
                    return false;
                uint32_t slot = static_cast<uint32_t>(readVarint(cursor));
                if (cursor >= end)
                    return false;
                list.append({ slot, static_cast<uint32_t>(readVarint(cursor)) });
            }
        }
        visitAll(list);
        recent.add(list);
    }
    return cursor == end;
}

struct CompiledRuntimeModuleData {
    uint32_t registryID;
    uint32_t module;
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

static_assert(sizeof(ModuleData) == 40 && isImageRecord<ModuleData>);
static_assert(sizeof(ModuleDirectoryData) == 6 && isImageRecord<ModuleDirectoryData> && sizeof(ModuleDisplayPathData) == 6 && isImageRecord<ModuleDisplayPathData>);
static_assert(sizeof(UncompiledModuleData) == 8 && isImageRecord<UncompiledModuleData>);
static_assert(sizeof(ModuleRequestData) == 3 && alignof(ModuleRequestData) == 1 && isImageRecord<ModuleRequestData>);
static_assert(ModuleRequestData::create(0xbcdef, true, false, true).target() == 0xbcdef);
static_assert(sizeof(LinkPlanStepData) == 4 && isImageRecord<LinkPlanStepData>);
static_assert(sizeof(LinkPlanRootData) == 12 && isImageRecord<LinkPlanRootData>);
static_assert(sizeof(ModuleExportData) == 8 && isImageRecord<ModuleExportData>);
static_assert(sizeof(IndirectExportData) == 8 && isImageRecord<IndirectExportData>);
static_assert(sizeof(SyntheticExportData) == 8 && isImageRecord<SyntheticExportData>);
static_assert(sizeof(RequiredNameData) == 8 && isImageRecord<RequiredNameData>);
static_assert(sizeof(ExportStatementData) == 12 && isImageRecord<ExportStatementData>);
static_assert(sizeof(AmbiguousExportData) == 8 && isImageRecord<AmbiguousExportData>);
static_assert(sizeof(SourceStampData) == 12 && isImageRecord<SourceStampData>);
static_assert(sizeof(ModuleFailureData) == 8 && isImageRecord<ModuleFailureData>);
static_assert(sizeof(ImportOfHeldFileData) == 8 && isImageRecord<ImportOfHeldFileData>);
static_assert(sizeof(StateOfLibraryModuleData) == 12 && isImageRecord<StateOfLibraryModuleData>);
static_assert(sizeof(CompiledRuntimeModuleData) == 8 && isImageRecord<CompiledRuntimeModuleData>);
static_assert(sizeof(CompiledRuntimeFunctionData) == 12 && isImageRecord<CompiledRuntimeFunctionData>);
static_assert(sizeof(ModuleData::Flag) == sizeof(ModuleData::flags));
static_assert(sizeof(ModuleKind) == 1 && sizeof(ExternalKind) == 1 && sizeof(SyntheticExportRule) == 1 && sizeof(RequireResult) == 1);
static_assert(static_cast<unsigned>(ModuleKind::External) < 8 && static_cast<unsigned>(ExternalKind::HostValue) < 2 && static_cast<unsigned>(SyntheticExportRule::RuntimeModule) < 4 && static_cast<unsigned>(RequireResult::NamespaceIfImported) < 4);

// ProgramTables::directoriesOfModulesByPath() has a bit for each text that ends with a slash and starts the path of a module listed by path, at the hash of the UTF-16 text.
struct HashOfDirectory {
    static constexpr uint32_t ofNoText = 2166136261u;
    static constexpr unsigned bitsPerDirectory = 16;
    static constexpr uint32_t with(uint32_t hash, char16_t character) { return (hash ^ character) * 16777619u; }
    static constexpr size_t bit(uint32_t hash, size_t numberOfBits) { return (hash ^ hash >> 15) & (numberOfBits - 1); }
};

// ProgramTables::modulesByHashOfPath() lists the modules in modulesByPath() by StringView::hash() of their paths, multiplied so that every bit of it counts: no place is empty.
// The high bits choose one of the buckets, of which there is one for two modules. ProgramTables::firstModulesByHashOfPath() says where each bucket begins, and behind the last where it ends.
// An entry holds the number of its module, in as many bits as the largest number takes, and the low bits of the hash in the rest. So a path is compared with a module that has another only where the hashes are as good as equal.
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
    // The number of the module, if the entry has the hash. A larger number otherwise.
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
    v(linkPlanRoots, LinkPlanRootData) \
    v(linkPlanStepsOfRoots, LinkPlanStepData) \
    v(moduleExports, ModuleExportData) \
    v(indirectExports, IndirectExportData) \
    v(syntheticExports, SyntheticExportData) \
    v(requiredNames, RequiredNameData) \
    v(exportStatements, ExportStatementData) \
    v(ambiguousExports, AmbiguousExportData) \
    v(sourceStamps, SourceStampData) \
    v(moduleFailures, ModuleFailureData) \
    v(modulesByPath, uint16_t) \
    v(modulesByHashOfPath, uint32_t) \
    v(firstModulesByHashOfPath, uint16_t) \
    v(directoriesOfModulesByPath, uint32_t) \
    v(modulesOfUncompiledCode, UncompiledModuleData) \
    v(modulesThatLoadCompiledFilesAlone, uint32_t) \
    v(modulesThatFindBuiltinsReadOnly, uint32_t) \
    v(modulesReachedUnderOtherNames, uint32_t) \
    v(importsOfHeldFiles, ImportOfHeldFileData) \
    v(namedVariables, uint8_t) \
    v(namedCaptures, uint8_t) \
    v(compiledRuntimeModules, CompiledRuntimeModuleData) \
    v(stateOfLibraryModules, StateOfLibraryModuleData) \
    v(compiledRuntimeFunctions, CompiledRuntimeFunctionData)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
