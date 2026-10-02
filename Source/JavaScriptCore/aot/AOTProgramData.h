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
class ObjectsOfProgramDecoder;
class ScriptExecutable;
class SourceCode;
class SourceCodeKey;
class SourceOrigin;
class SourceProvider;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;
struct UnlinkedStringJumpTable;

// Where a frame of a function that was compiled ahead of time can say it is.
struct ReportableSitesOfFunction {
    struct Construction {
        uint32_t offset;
        // Where the expression starts (the `new`), from where the frame says it is: that many lines up, and then at this column,
        // counting from one; or, on the same line, this many columns to the left.
        uint32_t linesUp;
        uint32_t columnOrColumnsLeft;
    };
    Vector<uint32_t> offsets; // In the bytecode. In order.
    Vector<Construction> constructions; // Those of them that construct something, if it is known where the `new` is.
    Vector<uint32_t> numbersOfIdentifiers; // What the code says for each of the function's identifiers (AOT::NumbersOfIdentifiers).
    Vector<uint32_t> numbersOfConstants; // Likewise (AOT::NumbersOfConstants). None: the function has its own.
};

namespace AOT {

struct FunctionInfo;
struct ImageKey;

struct ModuleOfProgram {
    uint32_t entryOffset; // In the payload that the program was built from. One less than SourceProvider::aotModuleID().
    uint32_t keyHash; // Of a builtin function: the embedder's stamp.
    uint32_t keyLength;
    uint32_t keyFlags;
    uint32_t isBuiltinFunction;
    uint32_t number; // One more than the number of its top-level code, or of a builtin function's executable. Zero: it has none.
};

struct RowOfExecutable {
    static constexpr unsigned bitsOfModule = 17;
    static constexpr unsigned bitsOfParameterCount = 12;

    uint64_t entry[2]; // FunctionExecutable::aotEntryFor()
    uint32_t index[2]; // FunctionExecutable::aotIndexFor()
    uint32_t name; // The number of an identifier. Zero: none.
    uint32_t unlinkedFunction; // Its number. In the short form, functions that only differ in what is in this row share one.
    uint32_t module : bitsOfModule;
    uint32_t parameterCount : bitsOfParameterCount;
    uint32_t isArrowFunctionContext : 1;
    uint32_t isInsideOrdinaryFunction : 1;
    uint32_t isShort : 1;
    uint32_t startOffset; // In the module's text, which there is none of, for an executable in full.
    uint32_t sourceLength;
    uint32_t unused;
};
static_assert(sizeof(RowOfExecutable) == 48);

// What a program that was compiled ahead of time consists of besides its code (AOT::Image), which it follows in the file. It is read
// where it is, wherever that is: it has offsets from its own start, and numbers, and no addresses.
struct ProgramData {
    static constexpr uint64_t expectedMagic = 0x3130415441445250ULL; // "PRDATA01"

    // `positions.find` maps a module (by its entry offset in the payload) and a position in the bundled text to a source name and
    // position. If it returns false, the position in the bundled text is kept.
    struct PositionsToKeep {
        const Vector<ReportableSitesOfFunction>& sites; // Indexed by function index in the code image.
        Function<bool(uint32_t entryOffsetOfModule, LineColumn inModule, CString& nameOfSource, LineColumn& inSource)> find;
    };
    // `strings` is the result of EncoderStringTable::serialize(); the payload and its module entry offsets come from
    // BytecodeLinkEncoder::finish().
    JS_EXPORT_PRIVATE static Vector<uint8_t> build(VM&, std::span<const uint8_t> strings, std::span<const uint8_t> payload, std::span<const uint32_t> entryOffsetsOfModules, std::span<const uint8_t> imageOfCode, const PositionsToKeep&, std::span<const ReportableSitesOfFunction> reportableSites, std::span<const std::optional<Vector<uint32_t>>> variablesExportedByModules);

    static const ProgramData* tryUse(std::span<const uint8_t>); // Null if they were built by a different engine build.
    JS_EXPORT_PRIVATE static const ProgramData* get(); // That of the process's image (Image::registerImage()). Null if it has none.
    // The offset and size of its copy of the string table, so that a container file does not need to store it twice.
    JS_EXPORT_PRIVATE static std::optional<std::pair<size_t, size_t>> stringTableIn(std::span<const uint8_t>);

    template<typename T> const T* at(uint32_t offset) const { return reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(this) + offset); }
    template<typename T> std::span<const T> spanAt(uint32_t offset, size_t count) const { return { at<T>(offset), count }; }

    std::span<const uint8_t> strings() const { return spanAt<uint8_t>(offsetOfStrings, sizeOfStrings); }
    std::span<const uint8_t> objects() const { return spanAt<uint8_t>(offsetOfObjects, sizeOfObjects); }
    std::span<const ModuleOfProgram> modules() const { return spanAt<ModuleOfProgram>(offsetOfModules, numberOfModules); }
    const ModuleOfProgram* moduleWithEntryOffset(uint32_t) const;
    inline const FunctionInfo* infos() const;
    const uint32_t* functionMetadataOffsets() const { return at<uint32_t>(offsetOfFunctionMetadataOffsets); }
    const RowOfExecutable& rowOfExecutable(uint32_t number) const { return at<RowOfExecutable>(offsetOfRowsOfExecutables)[number]; }
    JS_EXPORT_PRIVATE const RowOfExecutable& rowOfExecutableOfFunction(uint32_t indexOfFunction) const;
    JS_EXPORT_PRIVATE uint32_t numberOfExecutableOfFunction(uint32_t indexOfFunction) const;
    JS_EXPORT_PRIVATE LineColumn whereFunctionStarts(uint32_t indexOfFunction) const;
    JS_EXPORT_PRIVATE String nameOfSource(uint32_t) const; // Source numbers start at one.
    JS_EXPORT_PRIVATE Vector<uint32_t> entryOffsetsOfModules() const; // Not of builtins.
    std::span<const ImageKey> keysOfImage() const;

    uint64_t magic;
    uint64_t stamp; // AOT::imageStamp()
    uint32_t size;
    uint32_t offsetOfStrings;
    uint32_t sizeOfStrings;
    uint32_t offsetOfObjects; // encodeObjectsOfProgram()
    uint32_t sizeOfObjects;
    uint32_t offsetOfModules; // In the order of their entry offsets.
    uint32_t numberOfModules;
    uint32_t offsetOfInfos; // FunctionInfo[], by function index.
    uint32_t numberOfFunctions;
    uint32_t offsetOfFunctionMetadataOffsets; // uint32_t[], likewise. See FunctionMetadata.
    uint32_t offsetOfRowsOfExecutables;
    uint32_t numberOfExecutables;
    uint32_t numberOfIdentifiers;
    uint32_t numberOfConstants;
    uint32_t numberOfUnlinkedFunctions;
    uint32_t numberOfTopLevelCodes;
    uint32_t offsetOfNamesOfSources; // uint32_t[numberOfSources + 1]: the offset of each name in the UTF-8 text that follows the array.
    uint32_t numberOfSources;
    uint32_t offsetOfKeysOfImage; // ImageKey[]: of the functions whose executables are made from their source at run time.
    uint32_t capacityOfKeysOfImage;
    // uint32_t[], by the number of a constant: for a string, the offset of its record in the string table: its length, with the top bit
    // set if it is 8-bit; its hash; its characters. Zero for anything else.
    uint32_t offsetOfRecordsOfStringConstants;
    uint32_t offsetOfModulesOfEngineBuiltins; // uint32_t[], by BuiltinCodeIndex: one more than the index of its module. Zero: it has none.
    uint32_t numberOfEngineBuiltins;
};

// The engine's objects for what ProgramData describes. Each is made when it is first asked for, in the VM's heap like any other, and
// kept for as long as the VM lives. They are shared by every instance of the program in the VM, so they are what cannot be told
// apart and does not change: one refers only to primitives and to others of them, never to a JSObject.
class ProgramOfVM {
    WTF_MAKE_NONCOPYABLE(ProgramOfVM);
    WTF_MAKE_TZONE_ALLOCATED(ProgramOfVM);
public:
    JS_EXPORT_PRIVATE static ProgramOfVM* of(VM&); // Null if the process has no ProgramData.
    explicit ProgramOfVM(VM&);
    ~ProgramOfVM();

    const ProgramData& data() const { return m_data; }

    // Stubs read this table themselves. An entry is zero until the thing has been asked for.
    UniquedStringImpl* const* identifiers() const { return m_identifiers; }
    // And this one: few of a program's constants are ever asked for, so those that have been made are in a hash table, by one more
    // than their numbers. Open addressing, the next place being the next entry. See Stub::Constant.
    static constexpr uint32_t multiplierOfHashOfConstant = 2654435761u;
    static constexpr unsigned shiftOfHashOfConstant = 15;
    static uint32_t hashOfConstant(uint32_t number)
    {
        uint32_t hash = number * multiplierOfHashOfConstant;
        return hash ^ hash >> shiftOfHashOfConstant;
    }
    static constexpr ptrdiff_t offsetOfValuesOfConstants() { return OBJECT_OFFSETOF(ProgramOfVM, m_valuesOfConstants); }
    static constexpr ptrdiff_t offsetOfKeysOfConstants() { return OBJECT_OFFSETOF(ProgramOfVM, m_keysOfConstants); }
    static constexpr ptrdiff_t offsetOfMaskOfConstants() { return OBJECT_OFFSETOF(ProgramOfVM, m_maskOfConstants); }

    JS_EXPORT_PRIVATE JSValue constant(uint32_t number);
    JS_EXPORT_PRIVATE UniquedStringImpl* identifier(uint32_t number);
    const Identifier& identifierAsIdentifier(uint32_t number);
    JS_EXPORT_PRIVATE FunctionExecutable* executable(uint32_t number);
    FunctionExecutable* executableOfFunction(uint32_t indexOfFunction) { return executable(m_data.numberOfExecutableOfFunction(indexOfFunction)); }
    JS_EXPORT_PRIVATE UnlinkedFunctionExecutable* unlinkedFunction(uint32_t number, bool isShared);
    // The top-level code of the module whose provider says so (SourceProvider::aotModuleID()).
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* topLevelCodeFor(const SourceCodeKey&);
    UnlinkedCodeBlock* topLevelCode(uint32_t number);

    // What the executables of a module's functions say their source is. The first provider that the module is loaded with in this
    // VM, which has no text.
    JS_EXPORT_PRIVATE SourceProvider* providerOfModule(uint32_t indexOfModule);
    JS_EXPORT_PRIVATE void didLoadModule(SourceProvider&);
    const SourceCode& sourceOfShortExecutable(uint32_t numberOfExecutable);

    // What linking the result of decodeBuiltinFunction() would produce, for a builtin with this payload entry offset and source
    // text. Its source() equals what makeSource() would have returned.
    JS_EXPORT_PRIVATE FunctionExecutable* builtinFunctionFor(uint32_t entryOffset, unsigned embedderStamp, const String& text, const SourceOrigin&, const String& sourceURL);
    // The same for one of JSC's own builtins (BuiltinExecutables::stampOf()).
    FunctionExecutable* engineBuiltinFor(unsigned index, std::span<const Latin1Character> text);

    // See FunctionMetadata::StringSwitchJumpTables and ConstantIdentifierSets.
    const UnlinkedStringJumpTable& stringSwitchJumpTable(uint32_t offsetOfTables, unsigned which);
    const IdentifierSet& identifierSet(uint32_t offsetOfSets, unsigned which);

    template<typename Visitor> void visit(Visitor&, CollectionScope);
    void didFinishCollection();

private:
    struct Rest;
    void didMake(JSCell*);
    DecoderStringTable& strings();

    VM& m_vm;
    const ProgramData& m_data;
    void addConstant(uint32_t number, JSValue);
    EncodedJSValue* m_valuesOfConstants { nullptr };
    uint32_t* m_keysOfConstants { nullptr };
    uint32_t m_maskOfConstants { 0 };
    uint32_t m_numberOfConstantsMade { 0 };
    UniquedStringImpl** m_identifiers;
    FunctionExecutable** m_executables;
    std::unique_ptr<Rest> m_rest;
};

} } // namespace JSC::AOT
