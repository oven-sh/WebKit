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

struct FunctionReportableSites {
    struct Construction {
        uint32_t offset;
        uint32_t linesUp;
        uint32_t columnOrColumnDelta;
    };
    Vector<uint32_t> offsets;
    Vector<Construction> constructions;
    Vector<uint32_t> identifierIndices;
    Vector<uint32_t> constantIndices;
};

namespace AOT {

struct FunctionInfo;
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
    static constexpr uint64_t expectedMagic = 0x3130415441445250ULL;

    struct RetainedPositions {
        const Vector<FunctionReportableSites>& sites;
        Function<bool(uint32_t moduleEntryOffset, LineColumn inModule, CString& sourceName, LineColumn& inSource)> find;
    };
    JS_EXPORT_PRIVATE static Vector<uint8_t> build(VM&, std::span<const uint8_t> strings, std::span<const uint8_t> payload, std::span<const uint32_t> moduleEntryOffsets, std::span<const uint8_t> codeImage, const RetainedPositions&, std::span<const FunctionReportableSites>, std::span<const std::optional<Vector<uint32_t>>> variablesExportedByModules);

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
    const uint32_t* functionMetadataOffsets() const { return at<uint32_t>(functionMetadataTableOffset); }
    const ExecutableRow& executableRow(uint32_t index) const { return at<ExecutableRow>(executableRowsOffset)[index]; }
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
    uint32_t numberOfFunctions;
    uint32_t functionMetadataTableOffset;
    uint32_t executableRowsOffset;
    uint32_t numberOfExecutables;
    uint32_t numberOfIdentifiers;
    uint32_t numberOfConstants;
    uint32_t numberOfUnlinkedFunctions;
    uint32_t numberOfTopLevelCodes;
    uint32_t sourceNamesOffset;
    uint32_t numberOfSources;
    uint32_t imageKeysOffset;
    uint32_t imageKeyCapacity;
    uint32_t stringConstantRecordsOffset;
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

    UniquedStringImpl* const* identifiers() const { return m_identifiers; }
    static constexpr uint32_t constantHashMultiplier = 2654435761u;
    static constexpr unsigned constantHashShift = 15;
    static uint32_t constantHash(uint32_t index)
    {
        uint32_t hash = index * constantHashMultiplier;
        return hash ^ hash >> constantHashShift;
    }
    static constexpr ptrdiff_t offsetOfConstantValues() { return OBJECT_OFFSETOF(VMProgram, m_constantValues); }
    static constexpr ptrdiff_t offsetOfConstantKeys() { return OBJECT_OFFSETOF(VMProgram, m_constantKeys); }
    static constexpr ptrdiff_t offsetOfConstantMask() { return OBJECT_OFFSETOF(VMProgram, m_constantMask); }

    JS_EXPORT_PRIVATE JSValue constant(uint32_t index);
    JSValue createTransientConstant(uint32_t index);
    JS_EXPORT_PRIVATE UniquedStringImpl* identifier(uint32_t index);
    const Identifier& identifierAsIdentifier(uint32_t index);
    String identifierWithoutGC(uint32_t index);
    const Identifier* tryGetIdentifierConcurrently(uint32_t index) const;
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

    JS_EXPORT_PRIVATE FunctionExecutable* builtinFunctionFor(uint32_t entryOffset, unsigned embedderStamp, const String& text, const SourceOrigin&, const String& sourceURL);
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
    static constexpr uint32_t executablesPerChunk = 64;
    FunctionExecutable*** m_executableChunks;
    std::unique_ptr<Impl> m_impl;
};

JS_EXPORT_PRIVATE uint16_t propertyNameIDIfKnown(VM&, UniquedStringImpl*);
JS_EXPORT_PRIVATE bool hasListOfPropertyNames();
void validatePropertyNameIDs(VM&);

} } // namespace JSC::AOT
