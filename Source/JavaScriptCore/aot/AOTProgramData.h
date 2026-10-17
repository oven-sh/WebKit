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

struct TypedSitePosition {
    uint32_t line;
    uint32_t column;
    uint32_t source;
    uint32_t frame;
};

struct ParameterCheckSite {
    uint32_t site;
    uint32_t typedFunction;
    TypedSitePosition definition;
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
    static constexpr uint32_t handlerTakesException = 1u << 31;
    static constexpr uint32_t handlerIgnoresValue = 1u << 30;
    static constexpr uint32_t handlerFlags = handlerTakesException | handlerIgnoresValue;
    Vector<std::pair<uint32_t, uint32_t>> typedCatchEntrypointsForCallSites;
    uint32_t typedFunction { notTyped };
    uint32_t stateNumber { 0 };
    Vector<TypedSitePosition> typedPositions;
    Vector<ParameterCheckSite> parameterCheckSites;
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

    bool hasTopLevelCode() const { return topLevelCodeIndex != invalidIndex; }
    bool isCompiledModule() const { return functionIndex != invalidIndex; }

    uint32_t entryOffset;
    uint32_t keyHash;
    uint32_t keyLength;
    uint32_t keyFlags;
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
    inline const FunctionInfo* infos() const;
    const uint32_t* functionIndexAndFlags() const { return at<uint32_t>(functionIndexAndFlagsOffset); }
    const uint32_t* functionMetadataOffsets() const { return at<uint32_t>(functionMetadataTableOffset); }
    std::span<const uint8_t> typedFunctionHandlers(uint32_t functionIndex) const;
    const uint8_t* typedFunctionPositions(uint32_t functionIndex) const;
    struct ParameterCheck {
        uint32_t typedFunction;
        uint32_t source;
        uint32_t line;
        uint32_t column;
    };
    std::optional<ParameterCheck> parameterCheckAt(uint32_t functionIndex, uint32_t site) const;
    const uint64_t* closureWords() const { return reinterpret_cast<const uint64_t*>(reinterpret_cast<uintptr_t>(at<uint64_t>(typedEntriesOffset)) - omittedClosureWordCount * sizeof(uint64_t)); }
    uint64_t closureWordOf(uint32_t function) const { return function >= firstClosureFunction && function - omittedClosureWordCount < numberOfTypedEntries ? closureWords()[function] : 0; }
    static constexpr char8_t relativeSourceNameMarker = 1;
    JS_EXPORT_PRIVATE String sourceName(uint32_t) const;
    JS_EXPORT_PRIVATE String sourceNameInFrames(uint32_t) const;
    JS_EXPORT_PRIVATE bool isSourceNameEmpty(uint32_t) const;
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
    uint32_t functionIndexAndFlagsOffset;
    uint32_t numberOfFunctions;
    uint32_t numberOfStates;
    uint32_t functionMetadataTableOffset;
    uint32_t typedEntriesOffset;
    uint32_t numberOfTypedEntries;
    uint32_t firstClosureFunction;
    uint32_t omittedClosureWordCount;
    uint32_t numberOfIdentifiers;
    uint32_t numberOfConstants;
    uint32_t numberOfTopLevelCodes;
    uint32_t sourceNamesOffset;
    uint32_t numberOfSources;
    uint32_t imageKeysOffset;
    uint32_t imageKeyCapacity;
    uint32_t parameterChecksOffset;
    uint32_t numberOfParameterChecks;
};

class VMProgram {
    WTF_MAKE_NONCOPYABLE(VMProgram);
    WTF_MAKE_TZONE_ALLOCATED(VMProgram);
public:
    JS_EXPORT_PRIVATE static VMProgram* of(VM&);
    explicit VMProgram(VM&);
    ~VMProgram();

    const ProgramData& data() const { return m_data; }
    Typed::FunctionTables* typedFunctionTables { nullptr };

    UniquedStringImpl* const* identifiers() const { return m_identifiers; }
    const uint16_t* identifierPositions() const { return m_identifierPositions; }
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
        if (uint16_t place = m_identifierPositions[index]; isIdentifierPosition(place)) [[likely]]
            return m_identifiers[place];
        return identifierSlow(index);
    }
    ALWAYS_INLINE const Identifier& identifierAsIdentifier(uint32_t index)
    {
        static_assert(sizeof(Identifier) == sizeof(UniquedStringImpl*));
        if (!index) [[unlikely]]
            return *reinterpret_cast<const Identifier*>(&m_identifiers[0]);
        RELEASE_ASSERT(index < m_data.numberOfIdentifiers);
        if (uint16_t place = m_identifierPositions[index]; isIdentifierPosition(place)) [[likely]]
            return *reinterpret_cast<const Identifier*>(&m_identifiers[place]);
        return identifierAsIdentifierSlow(index);
    }
    ALWAYS_INLINE const Identifier* identifierIfCreated(uint32_t index) const
    {
        RELEASE_ASSERT(index < m_data.numberOfIdentifiers);
        uint16_t place = m_identifierPositions[index];
        return isIdentifierPosition(place) ? reinterpret_cast<const Identifier*>(&m_identifiers[place]) : nullptr;
    }
    const Identifier* tryGetIdentifierConcurrently(uint32_t index) const;
    bool isIdentifierRetained(uint32_t index) const { return m_identifierPositions[index]; }
    JS_EXPORT_PRIVATE const Identifier& retainIdentifier(uint32_t index, Identifier&&);
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* topLevelCodeFor(const SourceCodeKey&);
    JS_EXPORT_PRIVATE const ProgramModule* moduleFor(SourceProvider&);
    UnlinkedCodeBlock* topLevelCode(uint32_t index);

    JS_EXPORT_PRIVATE void didLoadModule(SourceProvider&);

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
    uint16_t* m_identifierPositions;
    static constexpr uint16_t identifierStoredElsewhere = UINT16_MAX;
    static constexpr bool isIdentifierPosition(uint16_t place) { return static_cast<uint16_t>(place - 1) < identifierStoredElsewhere - 1; }
    JS_EXPORT_PRIVATE UniquedStringImpl* identifierSlow(uint32_t index);
    JS_EXPORT_PRIVATE const Identifier& identifierAsIdentifierSlow(uint32_t index);
    uint32_t m_positionedIdentifierCount { 0 };
    std::unique_ptr<Impl> m_impl;
};

} } // namespace JSC::AOT
