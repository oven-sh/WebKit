/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CodeSpecializationKind.h"
#include "CollectionScope.h"
#include "Identifier.h"
#include "JSCJSValue.h"
#include "LineColumn.h"
#include <span>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/UniquedStringImpl.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class DecoderStringTable;
class FunctionExecutable;
class JSGlobalObject;
class ProgramObjectsDecoder;
class ScriptExecutable;
class SourceCode;
class SourceCodeKey;
class SourceOrigin;
class SourceProvider;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;
struct UnlinkedStringJumpTable;

// Where a call site of a function compiled from types is in the source. `source` indexes the function's own list of source names.
struct TypedSitePosition {
    uint32_t line;
    uint32_t column;
    uint32_t source;
    // The inline frame it is in. Of a call that was inlined: the frame the call stands in.
    uint32_t frame;
};

struct FunctionReportableSites {
    static constexpr uint32_t notTyped = UINT32_MAX;
    struct Construction {
        uint32_t offset;
        uint32_t linesUp;
        uint32_t columnOrColumnDelta;
    };
    Vector<uint32_t> offsets;
    Vector<Construction> constructions;
    Vector<uint32_t> identifierIndices;
    Vector<uint32_t> typedIdentifierIndices;
    Vector<String> typedIdentifierNames;
    Vector<std::pair<uint32_t, uint32_t>> typedCatchEntrypointsForCallSites;
    // Of a function compiled from types: its number in Typed::Data, and its positions by call-site number. Entry 0 is the function's start.
    uint32_t typedFunction { notTyped };
    uint32_t stateNumber { 0 };
    Vector<TypedSitePosition> typedPositions;
    uint32_t numberOfSitesWithoutCode { 0 };
    Vector<UTF8CString> typedSourceNames;
    Vector<uint32_t> constantIndices;
};

namespace AOT {

struct FunctionInfo;
namespace Typed {
struct FunctionTables;
void destroy(FunctionTables*);
}
struct ImageKey;

struct ProgramModule {
    static constexpr uint32_t invalidIndex = UINT32_MAX;

    bool hasExecutable() const { return executableIndex != invalidIndex; }
    bool hasTopLevelCode() const { return topLevelCodeIndex != invalidIndex; }
    bool isCompiledModule() const { return functionIndex != invalidIndex; }

    uint32_t entryOffset;
    uint32_t keyHash;
    uint32_t keyLength;
    uint32_t keyFlags;
    uint32_t isBuiltinFunction;
    uint32_t executableIndex { invalidIndex };
    uint32_t topLevelCodeIndex { invalidIndex };
    uint32_t functionIndex { invalidIndex };
    uint32_t environmentSymbolTable;
    uint32_t firstVarScopeOffset;
    uint32_t numberOfVarScopeOffsets;
    uint32_t functionDeclarationSlotsOffset;
    uint32_t numberOfFunctionDeclarationSlots;
    uint32_t features;
    uint32_t lexicallyScopedFeaturesAndFlags;
};

struct ExecutableRow {
    static constexpr unsigned moduleBits = 17;
    static constexpr unsigned parameterCountBits = 12;

    uint64_t entry[2];
    uint32_t index[2];
    uint32_t name;
    uint32_t unlinkedFunction;
    uint32_t module : moduleBits;
    uint32_t parameterCount : parameterCountBits;
    uint32_t isArrowFunctionContext : 1;
    uint32_t isInsideOrdinaryFunction : 1;
    uint32_t isShort : 1;
    uint32_t startOffset;
    uint32_t sourceLength;
    uint32_t functionStructureKind;
};
enum class FunctionStructureKind : uint32_t { None, Arrow, StrictFunction, StrictMethod, SloppyFunction, SloppyMethod };
static_assert(sizeof(ExecutableRow) == 48);

struct ProgramData {
    static constexpr uint64_t expectedMagic = 0x3430415441445250ULL;

    struct RetainedPositions {
        const Vector<FunctionReportableSites>& sites;
        Function<bool(uint32_t moduleEntryOffset, LineColumn inModule, UTF8CString& sourceName, LineColumn& inSource)> find;
    };
    JS_EXPORT_PRIVATE static Vector<uint8_t> build(VM&, std::span<const uint8_t> strings, std::span<const uint8_t> payload, std::span<const uint32_t> moduleEntryOffsets, std::span<const uint8_t> codeImage, const RetainedPositions&, std::span<const FunctionReportableSites>, std::span<const std::optional<Vector<uint32_t>>> variablesExportedByModules);
    JS_EXPORT_PRIVATE static void release(Vector<FunctionReportableSites>&);

    static const ProgramData* tryUse(std::span<const uint8_t>);
    JS_EXPORT_PRIVATE static const ProgramData* get();
    JS_EXPORT_PRIVATE static std::optional<std::pair<size_t, size_t>> stringTableIn(std::span<const uint8_t>);

    template<typename T> const T* at(uint32_t offset) const { return reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(this) + offset); }
    template<typename T> std::span<const T> spanAt(uint32_t offset, size_t count) const { return { at<T>(offset), count }; }

    std::span<const uint8_t> strings() const { return spanAt<uint8_t>(stringsOffset, stringsSize); }
    std::span<const uint8_t> objects() const { return spanAt<uint8_t>(objectsOffset, objectsSize); }
    std::span<const ProgramModule> modules() const { return spanAt<ProgramModule>(modulesOffset, numberOfModules); }
    const ProgramModule* moduleWithEntryOffset(uint32_t) const;
    std::span<const uint32_t> functionDeclarationSlots(const ProgramModule& module) const { return spanAt<uint32_t>(module.functionDeclarationSlotsOffset, module.numberOfFunctionDeclarationSlots); }
    JS_EXPORT_PRIVATE std::span<const uint32_t> functionDeclarationListEntries(const ProgramModule&) const;
    inline const FunctionInfo* infos() const;
    const uint32_t* indexAndFlagsOfFunctions() const { return at<uint32_t>(indexAndFlagsOfFunctionsOffset); }
    const uint32_t* functionMetadataOffsets() const { return at<uint32_t>(functionMetadataTableOffset); }
    std::span<const uint8_t> handlersOfTypedFunction(uint32_t functionIndex) const;
    const uint8_t* positionsOfTypedFunction(uint32_t functionIndex) const;
    const ExecutableRow& executableRow(uint32_t index) const { return at<ExecutableRow>(executableRowsOffset)[index]; }
    // By function of the program that was compiled from types: its ClosureWord. Zero: it has no code. Only from firstFunctionClosuresGoBy on; ahead of those stand the rows of the first functions of the program, which Stub::CallModuleFunction calls by their number plus rowsLeftOutAheadOfClosures. There is nothing at an index under rowsLeftOutAheadOfClosures.
    const uint64_t* closureWords() const { return reinterpret_cast<const uint64_t*>(reinterpret_cast<uintptr_t>(at<uint64_t>(typedEntriesOffset)) - rowsLeftOutAheadOfClosures * sizeof(uint64_t)); }
    uint64_t closureWordOf(uint32_t function) const { return function >= firstFunctionClosuresGoBy && function - rowsLeftOutAheadOfClosures < numberOfTypedEntries ? closureWords()[function] : 0; }
    JS_EXPORT_PRIVATE const ExecutableRow& executableRowForFunction(uint32_t functionIndex) const;
    JS_EXPORT_PRIVATE uint32_t executableIndexForFunction(uint32_t functionIndex) const;
    JS_EXPORT_PRIVATE LineColumn functionStartPosition(uint32_t functionIndex) const;
    JS_EXPORT_PRIVATE String sourceName(uint32_t) const;
    JS_EXPORT_PRIVATE Vector<uint32_t> moduleEntryOffsets() const;
    std::span<const ImageKey> imageKeys() const;

    uint64_t magic;
    uint64_t stamp;
    uint32_t size;
    uint32_t stringsOffset;
    uint32_t stringsSize;
    uint32_t objectsOffset;
    uint32_t objectsSize;
    uint32_t modulesOffset;
    uint32_t numberOfModules;
    uint32_t infosOffset;
    uint32_t indexAndFlagsOfFunctionsOffset;
    uint32_t numberOfFunctions;
    uint32_t numberOfStates;
    uint32_t functionMetadataTableOffset;
    uint32_t executableRowsOffset;
    uint32_t numberOfExecutables;
    uint32_t typedEntriesOffset;
    uint32_t numberOfTypedEntries;
    uint32_t firstFunctionClosuresGoBy;
    uint32_t rowsLeftOutAheadOfClosures;
    uint32_t numberOfIdentifiers;
    uint32_t numberOfConstants;
    uint32_t numberOfUnlinkedFunctions;
    uint32_t numberOfTopLevelCodes;
    uint32_t sourceNamesOffset;
    uint32_t numberOfSources;
    uint32_t imageKeysOffset;
    uint32_t imageKeyCapacity;
    uint32_t engineBuiltinModulesOffset;
    uint32_t numberOfEngineBuiltins;
};

class VMProgram {
    WTF_MAKE_NONCOPYABLE(VMProgram);
    WTF_MAKE_TZONE_ALLOCATED(VMProgram);
public:
    JS_EXPORT_PRIVATE static VMProgram* of(VM&);
    explicit VMProgram(VM&);
    ~VMProgram();

    const ProgramData& data() const { return m_data; }
    // Made when first asked for: see Typed::FunctionTables::of().
    Typed::FunctionTables* typedFunctionTables { nullptr };

    UniquedStringImpl* const* identifiers() const { return m_identifiers; }
    const uint16_t* identifierPlaces() const { return m_identifierPlaces; }
    static constexpr uint32_t constantHashMultiplier = 2654435761u;
    static constexpr unsigned constantHashShift = 15;
    static uint32_t constantHash(uint32_t index)
    {
        uint32_t hash = index * constantHashMultiplier;
        return hash ^ hash >> constantHashShift;
    }

    JS_EXPORT_PRIVATE JSValue constant(uint32_t index);
    ALWAYS_INLINE UniquedStringImpl* identifier(uint32_t index)
    {
        RELEASE_ASSERT(index < m_data.numberOfIdentifiers);
        if (uint16_t place = m_identifierPlaces[index]; isPlaceOfIdentifier(place)) [[likely]]
            return m_identifiers[place];
        return identifierSlow(index);
    }
    ALWAYS_INLINE const Identifier& identifierAsIdentifier(uint32_t index)
    {
        static_assert(sizeof(Identifier) == sizeof(UniquedStringImpl*));
        if (!index) [[unlikely]]
            return *reinterpret_cast<const Identifier*>(&m_identifiers[0]);
        RELEASE_ASSERT(index < m_data.numberOfIdentifiers);
        if (uint16_t place = m_identifierPlaces[index]; isPlaceOfIdentifier(place)) [[likely]]
            return *reinterpret_cast<const Identifier*>(&m_identifiers[place]);
        return identifierAsIdentifierSlow(index);
    }
    ALWAYS_INLINE const Identifier* identifierIfMade(uint32_t index) const
    {
        RELEASE_ASSERT(index < m_data.numberOfIdentifiers);
        uint16_t place = m_identifierPlaces[index];
        return isPlaceOfIdentifier(place) ? reinterpret_cast<const Identifier*>(&m_identifiers[place]) : nullptr;
    }
    String identifierWithoutGC(uint32_t index);
    const Identifier* tryGetIdentifierConcurrently(uint32_t index) const;
    bool isIdentifierKept(uint32_t index) const { return m_identifierPlaces[index]; }
    JS_EXPORT_PRIVATE const Identifier& keepIdentifier(uint32_t index, Identifier&&);
    JS_EXPORT_PRIVATE FunctionExecutable* executable(uint32_t index);
    FunctionExecutable* executableIfExists(uint32_t index) const
    {
        FunctionExecutable** chunk = m_executableChunks[index / executablesPerChunk];
        return chunk ? chunk[index % executablesPerChunk] : nullptr;
    }
    FunctionExecutable* executableForFunction(uint32_t functionIndex) { return executable(m_data.executableIndexForFunction(functionIndex)); }
    JS_EXPORT_PRIVATE UnlinkedFunctionExecutable* unlinkedFunction(uint32_t index, bool isShared);
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* topLevelCodeFor(const SourceCodeKey&);
    JS_EXPORT_PRIVATE const ProgramModule* moduleFor(SourceProvider&);
    UnlinkedCodeBlock* topLevelCode(uint32_t index);

    JS_EXPORT_PRIVATE SourceProvider* moduleProvider(uint32_t moduleIndex);
    JS_EXPORT_PRIVATE void didLoadModule(SourceProvider&);
    const SourceCode& shortExecutableSource(uint32_t executableIndex);

    FunctionExecutable* engineBuiltinFor(unsigned index, std::span<const Latin1Character> text);

    const UnlinkedStringJumpTable& stringSwitchJumpTable(uint32_t offsetOfTables, unsigned which);
    const IdentifierSet& identifierSet(uint32_t offsetOfSets, unsigned which);

    template<typename Visitor> void visit(Visitor&, CollectionScope);
    void didFinishCollection();

private:
    struct Impl;
    void didMaterialize(JSCell*);
    DecoderStringTable& strings();

    VM& m_vm;
    const ProgramData& m_data;
    void addConstant(uint32_t index, JSValue);
    EncodedJSValue* m_constantValues { nullptr };
    uint32_t* m_constantKeys { nullptr };
    uint32_t m_constantMask { 0 };
    uint32_t m_numberOfMaterializedConstants { 0 };
    UniquedStringImpl** m_identifiers;
    uint16_t* m_identifierPlaces;
    static constexpr uint16_t placeOfIdentifiersKeptElsewhere = UINT16_MAX;
    static constexpr bool isPlaceOfIdentifier(uint16_t place) { return static_cast<uint16_t>(place - 1) < placeOfIdentifiersKeptElsewhere - 1; }
    JS_EXPORT_PRIVATE UniquedStringImpl* identifierSlow(uint32_t index);
    JS_EXPORT_PRIVATE const Identifier& identifierAsIdentifierSlow(uint32_t index);
    uint32_t m_numberOfIdentifiersInPlaces { 0 };
    static constexpr uint32_t executablesPerChunk = 64;
    FunctionExecutable*** m_executableChunks;
    std::unique_ptr<Impl> m_impl;
};

} } // namespace JSC::AOT
