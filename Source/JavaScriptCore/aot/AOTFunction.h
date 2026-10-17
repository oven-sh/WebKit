/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "BytecodeIndex.h"
#include "CodeSpecializationKind.h"
#include "CodeType.h"
#include "Identifier.h"
#include "LineColumn.h"
#include "PropertySlot.h"
#include "WriteBarrier.h"
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>
#include <wtf/StackBounds.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

#if ENABLE(AOT)

namespace JSC {

class AbstractModuleRecord;
class CallFrame;
class CodeBlock;
class FunctionExecutable;
class JSFunction;
class JSGlobalObject;
class JSObject;
class JSValue;
class RegisterAtOffsetList;
class ScriptExecutable;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;
struct UnlinkedHandlerInfo;
struct UnlinkedStringJumpTable;

namespace AOT {

struct Data;
struct FunctionMetadata;
struct FunctionInfo;
struct ProgramData;
struct ImageFunction;
class InlineFrameReader;
struct Instance;
struct Slot;

using Compress = size_t (*)(const uint8_t* source, size_t sourceSize, uint8_t* destination, size_t capacity);
using Decompress = bool (*)(const uint8_t* source, size_t sourceSize, uint8_t* destination, size_t size);
JS_EXPORT_PRIVATE void setCodec(Compress, Decompress);
using FunctionSourceProvider = String (*)(JSGlobalObject*, const String& path, uint32_t place);
JS_EXPORT_PRIVATE void setFunctionSourceProvider(FunctionSourceProvider);

struct FunctionRef {
    JS_EXPORT_PRIVATE static FunctionRef at(Instance*, const void* address);
    struct Location;
    JS_EXPORT_PRIVATE Location locationForReturnAddress(const void* returnAddress, const void* frame, InlineFrameReader* = nullptr) const;
    JS_EXPORT_PRIVATE BytecodeIndex bytecodeIndexAt(const void* returnAddress, const void* frame) const;
    JS_EXPORT_PRIVATE Location inlineCallSiteLocation(unsigned inlineFrame, InlineFrameReader&) const;
    JS_EXPORT_PRIVATE static FunctionRef of(CodeBlock*);
    JS_EXPORT_PRIVATE static FunctionRef of(VM&, ScriptExecutable*, JSCell* instanceToken);
    explicit operator bool() const { return index != none; }

    JS_EXPORT_PRIVATE ScriptExecutable* executable() const;
    JS_EXPORT_PRIVATE ScriptExecutable* executableIfExists() const;
    JS_EXPORT_PRIVATE bool hasExecutable() const;

    const FunctionInfo& info() const;
    const ProgramData& programData() const;
    JS_EXPORT_PRIVATE Data* dataIfExists() const;
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* unlinkedCodeBlockIfExists() const;
    JS_EXPORT_PRIVATE CodeBlock* codeBlockIfExists() const;
    const FunctionMetadata* metadata() const;
    JS_EXPORT_PRIVATE CodeType codeType() const;
    JS_EXPORT_PRIVATE bool isBuiltinFunction() const;
    JS_EXPORT_PRIVATE unsigned instructionsSize() const;
    JS_EXPORT_PRIVATE const UnlinkedHandlerInfo* handlerFor(unsigned bytecodeOffset, UnlinkedHandlerInfo& scratchHandler) const;
    JS_EXPORT_PRIVATE void* catchEntrypointAddress(unsigned bytecodeOffset) const;
    const UnlinkedStringJumpTable& stringSwitchJumpTable(unsigned) const;
    JS_EXPORT_PRIVATE const IdentifierSet& constantIdentifierSet(unsigned) const;

    JS_EXPORT_PRIVATE LineColumn lineColumnFor(BytecodeIndex) const;
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* ensureUnlinkedCodeBlock() const;
    UnlinkedCodeBlock* createUnlinkedCodeBlockFromMetadata() const;
    JS_EXPORT_PRIVATE CodeBlock* ensureCodeBlock() const;
    JS_EXPORT_PRIVATE Data* ensureData() const;
    uint32_t siteConstantOf(const Slot*) const;
    JS_EXPORT_PRIVATE std::optional<std::pair<String, bool>> quoteAt(BytecodeIndex) const;
    struct ReportedPosition {
        LineColumn lineColumn;
        uint32_t source { 0 };
    };
    enum class ConstructPosition : bool { AtDivot, AtStart };
    JS_EXPORT_PRIVATE std::optional<ReportedPosition> reportedPositionFor(BytecodeIndex, ConstructPosition = ConstructPosition::AtDivot) const;
    std::optional<ReportedPosition> decodeReportedPositionFor(BytecodeIndex, ConstructPosition) const;
    JS_EXPORT_PRIVATE bool constructsAt(BytecodeIndex) const;

    static constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
    Instance* instance { nullptr };
    uint32_t index { none };
};

struct FunctionRef::Location {
    FunctionRef function;
    BytecodeIndex bytecodeIndex;
    unsigned inlineFrame { 0 };
    bool isTailDeleted { false };
    bool isTailCall { false };
    bool isConstructed { false };
    uint32_t typedFunction { FunctionRef::none };
};

JS_EXPORT_PRIVATE String typedFunctionName(uint32_t function, bool asWritten = false);
JS_EXPORT_PRIVATE String typedFunctionName(VM&, uint32_t function, bool asWritten = false);
JS_EXPORT_PRIVATE CodeType typedFunctionCodeType(uint32_t function);
JS_EXPORT_PRIVATE bool typedFunctionIsConstructor(uint32_t function);
JS_EXPORT_PRIVATE bool isCompiledClassObject(JSFunction*);
JS_EXPORT_PRIVATE bool typedFunctionHasGivenName(uint32_t function);
JS_EXPORT_PRIVATE bool typedFunctionIsHiddenInStackTraces(uint32_t function);
JS_EXPORT_PRIVATE bool typedFunctionWraps(uint32_t wrapper, uint32_t function);
JS_EXPORT_PRIVATE bool typedFunctionIsSynthetic(uint32_t function);
JS_EXPORT_PRIVATE bool wasEnteredByConstruct(const Instance*, const CallFrame* previousTopCallFrame);
enum class StackTraceBuiltinKind : uint8_t { None, JSBuiltin, JSBuiltinConstructor, HostFunction };
JS_EXPORT_PRIVATE StackTraceBuiltinKind builtinKindOfTypedFunction(uint32_t function);
JS_EXPORT_PRIVATE bool typedFunctionRejectsOnThrow(uint32_t function);
JS_EXPORT_PRIVATE String typedClosureName(const JSFunction*);
JS_EXPORT_PRIVATE unsigned typedClosureLength(const JSFunction*);
JS_EXPORT_PRIVATE String slimClosureName(JSObject*);
JS_EXPORT_PRIVATE String closureDeclaredName(JSObject*);
JS_EXPORT_PRIVATE String closureDisplayName(JSObject*);
JS_EXPORT_PRIVATE void startRecordingThrownTypeErrors();
JS_EXPORT_PRIVATE String recordedThrownTypeErrors();
JS_EXPORT_PRIVATE bool isSloppyPlainClosure(JSCell*);
JS_EXPORT_PRIVATE unsigned slimClosureLength(JSObject*);
JS_EXPORT_PRIVATE Structure* cachedBoundFunctionStructure(JSObject*);
JS_EXPORT_PRIVATE void setCachedBoundFunctionStructure(JSObject*, Structure*);
JS_EXPORT_PRIVATE void* taggedSlimClosureEntrypoint();
JS_EXPORT_PRIVATE JSValue slimClosureSourceText(JSGlobalObject*, JSObject*);
uint64_t typedFunctionClosureWord(std::span<const uint8_t> image, uint32_t function, uint64_t relative);
struct ClosureWordRange {
    uint32_t firstClosureFunction;
    uint32_t endOfClosureFunctions;
    uint32_t omittedClosureWordCount;
};
ClosureWordRange closureWordRangeIn(std::span<const uint8_t> image);
JS_EXPORT_PRIVATE bool isTypedFunctionClosure(JSCell*, uint32_t function);
JS_EXPORT_PRIVATE JSObject* withAllPropertiesInline(JSGlobalObject*, JSObject*);

namespace Typed {

JS_EXPORT_PRIVATE bool isEntryPoint(uint32_t module);
JS_EXPORT_PRIVATE JSValue requireCompiledEntryPoint(JSGlobalObject*, AbstractModuleRecord*);
JS_EXPORT_PRIVATE bool isModuleNamespace(JSCell*);
JS_EXPORT_PRIVATE void detachModuleNamespace(JSGlobalObject*, JSObject*);
Identifier tryCreateIdentifier(VM&, uint32_t identifier);
JS_EXPORT_PRIVATE bool hasBuiltinModule(JSGlobalObject*, uint32_t id);
JS_EXPORT_PRIVATE JSValue evaluateBuiltinModule(JSGlobalObject*, uint32_t id);
JS_EXPORT_PRIVATE void willRequestFileFromProgram(JSGlobalObject*, const String& path);
JS_EXPORT_PRIVATE bool hasBuiltinFunction(VM&, uint32_t id);
JS_EXPORT_PRIVATE JSFunction* builtinFunction(JSGlobalObject*, uint32_t id, JSObject* prototype = nullptr);
void didFinishCreation(JSGlobalObject*);
enum class DeclaredAs : uint8_t { NotCompiled, Function, AsyncFunction, GeneratorFunction, AsyncGeneratorFunction, Class };
JS_EXPORT_PRIVATE DeclaredAs declaredAs(JSCell*);
JS_EXPORT_PRIVATE bool belongsToBuiltinModule(JSCell*);
JS_EXPORT_PRIVATE bool isGeneratorObject(JSCell*);
JS_EXPORT_PRIVATE void reifyClassPrototypeIfNeeded(JSObject*);
JS_EXPORT_PRIVATE bool forEachCompiledObjectOwnProperty(JSObject*, const ScopedLambda<bool(UniquedStringImpl&, JSValue)>&);
std::optional<bool> compiledObjectHasOwnProperty(JSObject*, UniquedStringImpl*);
JSValue tryGetCompiledObjectOwnData(VM&, JSCell*, UniquedStringImpl*);
std::optional<std::pair<uint32_t, uint32_t>> fieldOffsetAndKindForInterpreter(JSCell*, UniquedStringImpl*);
bool absentFieldReadsAsUndefined(JSGlobalObject*, JSCell*, UniquedStringImpl*);
bool structureGuaranteesUndefinedRead(JSGlobalObject*, JSCell*, UniquedStringImpl*);
bool compiledObjectStructureGuaranteesAbsence(Structure*, UniquedStringImpl*);
void countIfCompiledObject(VM&, JSValue, const char* operation);
bool tryStorePrimitiveToCompiledObjectField(VM&, JSCell*, UniquedStringImpl*, JSValue);
std::optional<std::pair<uint32_t, uint32_t>> fieldStoreOffsetAndKindForInterpreter(VM&, JSCell*, const Identifier&);
bool storeCompiledObjectExtraProperty(JSCell*, int offset, JSValue);
JSValue compiledObjectExtraProperty(JSCell*, int offset);
std::optional<std::pair<uint16_t, uint16_t>> extraPropertyLocationForInterpreter(VM&, JSCell*, const Identifier&);
bool tryAssignPrimitivesToCompiledObjectFields(VM&, JSObject* target, JSObject* source);
JS_EXPORT_PRIVATE void didChangePrototypeProperty(VM&, JSCell* function);
JS_EXPORT_PRIVATE void didCreateReplaceableGlobal(JSGlobalObject*, ASCIILiteral name, JSObject*);
struct HostClassCachedGetter {
    GetValueFunc::Ptr getter;
    uint32_t offsetOfCachedValue;
};
JS_EXPORT_PRIVATE void setHostClassCachedGetters(std::span<const HostClassCachedGetter>);
static constexpr unsigned maxNumberOfHostClasses = 128;
JS_EXPORT_PRIVATE void didCreateHostClassStructure(JSGlobalObject*, unsigned number, Structure*);

} // namespace Typed

void destroyJSONBuffers(Instance&);
void destroyJSONSource(Instance&);
void destroyStringAppender(Instance&);
void destroyFormatPatternState(Instance&);
void releaseLargeJSONSource(Instance&);
String takePreparedJSON(Instance&, JSValue);
JS_EXPORT_PRIVATE JSGlobalObject* globalObjectForInstanceToken(VM&, JSCell* token);
JS_EXPORT_PRIVATE void registerAsyncLocalStorage(JSGlobalObject*, JSObject* storage, JSObject* frame, JSCell* getStore);

struct StackTraceFrame {
    uint32_t function;
    uint32_t typedFunction;
    uint64_t callSiteAndConstructedRoot;
    static StackTraceFrame create(uint32_t function, uint32_t typedFunction, BytecodeIndex callSite, bool isConstructed = false)
    {
        return { function, typedFunction, callSite.asBits() | static_cast<uint64_t>(isConstructed ? typedFunction + 1 : 0) << 32 };
    }
    BytecodeIndex callSite() const { return BytecodeIndex::fromBits(static_cast<uint32_t>(callSiteAndConstructedRoot)); }
    void setConstructedRoot(uint32_t root)
    {
        if (!(callSiteAndConstructedRoot >> 32))
            callSiteAndConstructedRoot |= static_cast<uint64_t>(root + 1) << 32;
    }
    bool isConstructed() const
    {
        uint32_t root = callSiteAndConstructedRoot >> 32;
        return root && (root - 1 == typedFunction || typedFunctionWraps(root - 1, typedFunction));
    }
};
JS_EXPORT_PRIVATE void appendProfileFrames(uint32_t function, uint32_t site, bool& noFrameShownYet, Vector<StackTraceFrame, 4>&);
JS_EXPORT_PRIVATE String describeCodeAddresses(std::span<const void* const>);
JS_EXPORT_PRIVATE std::optional<FunctionRef::ReportedPosition> typedFunctionDefinition(uint32_t typedFunction);
JS_EXPORT_PRIVATE Vector<uint8_t> imageSymbols(std::span<const uint8_t> imageAndProgramData);
JS_EXPORT_PRIVATE uint32_t typedFunctionWithInnerEntryAt(const void* pc);

struct ReturnAddressTrace {
    WTF_ALLOW_STRUCT_COMPACT_POINTERS;
    Instance* instance;
    static constexpr unsigned limitBits = 24;
    static constexpr unsigned maxOmittedFrames = 255;
    uint32_t limit : limitBits;
    uint32_t omittedFrameCount : 8;
    uint32_t numberOfReturnAddresses : 30;
    uint32_t topIsAtStackTraceItemSite : 1;
    uint32_t hasHostFunctionFrameOnTop : 1;
    static size_t allocationSize(size_t numberOfReturnAddresses) { return sizeof(ReturnAddressTrace) + numberOfReturnAddresses * sizeof(uint32_t); }
    size_t sizeInBytes() const { return allocationSize(numberOfReturnAddresses); }
    std::span<uint32_t> returnAddresses() { return { reinterpret_cast<uint32_t*>(this + 1), numberOfReturnAddresses }; }
    std::span<const uint32_t> returnAddresses() const { return { reinterpret_cast<const uint32_t*>(this + 1), numberOfReturnAddresses }; }
    static constexpr uint32_t isConstructedRoot = 1u << 31;
    static uint32_t keep(const void* address, uintptr_t imageCodeStart) { return static_cast<uint32_t>(std::bit_cast<uintptr_t>(address) - imageCodeStart); }
    static const void* addressOf(uint32_t kept, uintptr_t imageCodeStart) { return std::bit_cast<const void*>(imageCodeStart + (kept & ~isConstructedRoot)); }
};
JS_EXPORT_PRIVATE uintptr_t imageCodeStart();
void writePerfMap();

struct ImageAddressInfo {
    enum Kind : uint8_t {
        NotInImage,
        Function,
        Stub,
        Adapter,
    };
    static constexpr uint32_t frameMayBeHostFunction = 1;
    Kind kind { NotInImage };
    uint32_t index { 0 };
    uint32_t offset { 0 };
};
JS_EXPORT_PRIVATE ImageAddressInfo classifyAddress(const void* address);
JS_EXPORT_PRIVATE ImageAddressInfo::Kind addressKind(const void* address);
JS_EXPORT_PRIVATE std::optional<uint32_t> imageCodeOffset(const void* address);
JS_EXPORT_PRIVATE void* returnAddressForFrame(const void* frame, const void* startingFrom);
struct FrameAndPC {
    void* frame { nullptr };
    void* pc { nullptr };
};
JS_EXPORT_PRIVATE std::optional<FrameAndPC> innermostFrame(void* machineFrame, void* machinePC, void* machineLinkRegister, void* topCallFrame, const StackBounds&);
JS_EXPORT_PRIVATE void* returnAddressOfFramelessTop(void* machineFrame, void* machinePC, void* machineLinkRegister, void* machineStackPointer, const StackBounds&);
JS_EXPORT_PRIVATE const RegisterAtOffsetList& adapterSavedRegisters();
struct SavedRegisters {
    uint64_t mask;
    ptrdiff_t first;
};
JS_EXPORT_PRIVATE SavedRegisters savedRegistersOf(const ImageFunction&);
JS_EXPORT_PRIVATE bool hasCode();
JS_EXPORT_PRIVATE std::optional<uint32_t> tryCallSiteAt(const ImageFunction&, uint32_t offsetOfReturnAddress, bool* constructs = nullptr);
std::optional<uint32_t> spreadSite(const ImageFunction&, uint32_t callSite, unsigned item);
struct ImageInlineFrame {
    uint32_t parent;
    uint32_t callSite;
    uint32_t function;
    uint8_t flags;
    bool isTailCall() const { return flags & 1; }
    bool isConstruction() const { return flags & 2; }
};
JS_EXPORT_PRIVATE ImageInlineFrame inlineFrameOf(const ImageFunction&, unsigned frame);
class InlineFrameReader {
public:
    const ImageInlineFrame& at(const ImageFunction& function, unsigned frame)
    {
        if (m_function != &function || frame > m_frames.size())
            read(function, frame);
        return m_frames[frame - 1];
    }

private:
    JS_EXPORT_PRIVATE void read(const ImageFunction&, unsigned last);

    const ImageFunction* m_function { nullptr };
    Vector<ImageInlineFrame, 32> m_frames;
};
class SiteFrames {
public:
    explicit SiteFrames(const ImageFunction&);
    unsigned next();
    void skip(uint64_t numberOfSites);

private:
    const uint8_t* m_inlineFrames { nullptr };
    const uint8_t* m_sites { nullptr };
    uint64_t m_sitesInFunction { 0 };
    uint64_t m_inlinedCalls { 0 };
    uint64_t m_withCode { 0 };
    unsigned m_frame { 0 };
};
class CallSiteOverride {
    WTF_MAKE_NONCOPYABLE(CallSiteOverride);
public:
    CallSiteOverride(Instance&, const void* frame);
    ~CallSiteOverride();

    void setItem(unsigned item) { m_item = item; }

    const CallSiteOverride* previous() const { return m_previous; }
    const void* frame() const { return m_frame; }
    unsigned item() const { return m_item; }

private:
    Instance& m_instance;
    const CallSiteOverride* m_previous;
    const void* m_frame;
    unsigned m_item { 0 };
};
JS_EXPORT_PRIVATE Instance* instanceForFrame(const void* frame);
JS_EXPORT_PRIVATE Instance* tryInstanceForFrame(const void* frame);
JS_EXPORT_PRIVATE bool topCallFrameIsAOTFrame(const void* frame);
JS_EXPORT_PRIVATE FunctionRef callerFunction(const CallFrame*);
JS_EXPORT_PRIVATE JSCell* callerOwnerCell(const CallFrame*);
void countAwait(VM&, const char* where, JSValue operand);
void countJobCall(VM&);
void recordSample(JSGlobalObject*, const char* kind, StringView group, StringView text);

} } // namespace JSC::AOT

#else // ENABLE(AOT)

namespace JSC { namespace AOT {

struct Instance;

struct FunctionRef {
    enum class ConstructPosition : uint8_t { AtDivot, AtStart };
    struct ReportedPosition {
        LineColumn lineColumn;
        uint32_t source { 0 };
    };
    explicit operator bool() const { return false; }
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
