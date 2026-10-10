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
#include "WriteBarrier.h"
#include <optional>
#include <span>
#include <wtf/StackBounds.h>
#include <wtf/text/WTFString.h>

#if ENABLE(AOT)

namespace JSC {

class CallFrame;
class CodeBlock;
class FunctionExecutable;
class JSFunction;
class JSGlobalObject;
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
struct Instance;
struct Slot;

using Compress = size_t (*)(const uint8_t* source, size_t sourceSize, uint8_t* destination, size_t capacity);
using Decompress = bool (*)(const uint8_t* source, size_t sourceSize, uint8_t* destination, size_t size);
JS_EXPORT_PRIVATE void setCodec(Compress, Decompress);

struct AllocationPlan {
    static uint32_t encode(unsigned inlineCapacity, unsigned count)
    {
        RELEASE_ASSERT(inlineCapacity <= 0xffff && count <= 0xffff);
        return inlineCapacity << 16 | count;
    }
    static uint32_t encode(unsigned identifier, bool isDefined, bool isStrict, bool isAssigned) { return identifier << 3 | isAssigned << 2 | isDefined << 1 | isStrict; }

    explicit operator bool() const { return !!words; }
    unsigned inlineCapacity() const { return words[0] >> 16; }
    unsigned count() const { return words[0] & 0xffff; }
    unsigned identifier(unsigned i) const { return words[i + 1] >> 3; }
    bool isAssigned(unsigned i) const { return words[i + 1] & 4; }
    bool isDefined(unsigned i) const { return words[i + 1] & 2; }
    bool isStrict(unsigned i) const { return words[i + 1] & 1; }
    uint16_t thisLayoutID() const { return static_cast<uint16_t>(words[count() + 1]); }
    uint16_t thisFamily() const { return static_cast<uint16_t>(words[count() + 1] >> 16); }

    const uint32_t* words { nullptr };
};

struct FunctionRef {
    JS_EXPORT_PRIVATE static FunctionRef at(Instance*, const void* address);
    struct Location;
    JS_EXPORT_PRIVATE Location locationForReturnAddress(const void* returnAddress, const void* frame) const;
    JS_EXPORT_PRIVATE BytecodeIndex bytecodeIndexAt(const void* returnAddress, const void* frame) const;
    JS_EXPORT_PRIVATE Location inlineCallSiteLocation(unsigned inlineFrame) const;
    JS_EXPORT_PRIVATE static FunctionRef of(CodeBlock*);
    JS_EXPORT_PRIVATE static FunctionRef of(VM&, ScriptExecutable*, CodeSpecializationKind, JSCell* instanceToken);
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
    JS_EXPORT_PRIVATE const UnlinkedHandlerInfo* handlerFor(unsigned bytecodeOffset) const;
    JS_EXPORT_PRIVATE void* catchEntrypointAddress(unsigned bytecodeOffset) const;
    JS_EXPORT_PRIVATE JSCell* calleeInFrame(const void* frame) const;
    const UnlinkedStringJumpTable& stringSwitchJumpTable(unsigned) const;
    JS_EXPORT_PRIVATE const IdentifierSet& constantIdentifierSet(unsigned) const;
    JS_EXPORT_PRIVATE BytecodeIndex resumePointOf(int32_t state) const;

    JS_EXPORT_PRIVATE LineColumn lineColumnFor(BytecodeIndex) const;
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* ensureUnlinkedCodeBlock() const;
    UnlinkedCodeBlock* createUnlinkedCodeBlockFromMetadata() const;
    JS_EXPORT_PRIVATE CodeBlock* ensureCodeBlock() const;
    JS_EXPORT_PRIVATE Data* ensureData() const;
    FunctionExecutable* functionDecl(unsigned) const;
    FunctionExecutable* functionExpr(unsigned) const;
    std::optional<uint32_t> nestedExecutableIndex(bool isExpression, unsigned index) const;
    uint32_t siteConstantOf(const Slot*) const;
    JS_EXPORT_PRIVATE std::optional<std::pair<String, bool>> quoteAt(BytecodeIndex) const;
    struct ReportedPosition {
        LineColumn lineColumn;
        uint32_t source { 0 };
    };
    enum class ConstructPosition : bool { AtDivot, AtStart };
    JS_EXPORT_PRIVATE std::optional<ReportedPosition> reportedPositionFor(BytecodeIndex, ConstructPosition = ConstructPosition::AtDivot) const;
    JS_EXPORT_PRIVATE bool constructsAt(BytecodeIndex) const;
    AllocationPlan planOf(const Slot* firstSiteSlot) const;

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
    // With FunctionInfo::isTyped: the function to report, by its number in Typed::Data. `function` stays the one whose code is running,
    // also for an inlined frame: positions and handlers are its own.
    uint32_t typedFunction { FunctionRef::none };
};

// Functions compiled from types have no executable. Stack frames report them from Typed::Data, which lives in the image: no cells, no
// Instance, usable on any thread.
JS_EXPORT_PRIVATE String typedFunctionName(uint32_t function);
JS_EXPORT_PRIVATE CodeType typedFunctionCodeType(uint32_t function);
JS_EXPORT_PRIVATE bool typedFunctionIsConstructor(uint32_t function);
// Generic entrypoints and coroutine ramps: another frame of the same name stands next to them.
JS_EXPORT_PRIVATE bool typedFunctionIsHiddenInStackTraces(uint32_t function);
// The general entry or the ramp of an async function, or what is bound to one: see UnwindFunctor::rejectsInFrame().
JS_EXPORT_PRIVATE bool typedFunctionRejectsWhatItThrows(uint32_t function);
// Of a closure, whose executable is NativeExecutable::isSharedByAOTTypedClosures(): what the executable of any other host function has.
JS_EXPORT_PRIVATE String nameOfTypedClosure(const JSFunction*);
JS_EXPORT_PRIVATE unsigned lengthOfTypedClosure(const JSFunction*);
// The ClosureWord of the function. `relative`: the EntryWord of its code, with the distance from the start of the code for an address.
uint64_t closureWordOfTypedFunction(std::span<const uint8_t> image, uint32_t function, uint64_t relative);
// Whether the cell is a closure of the function, whichever of its entrypoints each of the two goes by.
JS_EXPORT_PRIVATE bool isClosureOfTypedFunction(JSCell*, uint32_t function);

namespace Typed {

// Whether an entry function of a program that was compiled from types runs in the place of the top-level code of the module, which is named
// as the program data names it (ProgramModule::entryOffset: SourceProvider::aotModuleID() less one). False where there is no image.
JS_EXPORT_PRIVATE bool isEntryPoint(uint32_t module);
// For the host's registry of its own modules. Whether the image has the module of this number: false where there is no image.
JS_EXPORT_PRIVATE bool hasBuiltinModule(JSGlobalObject*, uint32_t id);
// Runs the module, which the image has, and yields what it exports. Empty: it has thrown. Its top level is code that runs once: asked again,
// this yields the same, or throws the same, without running anything, as for a module of the language.
JS_EXPORT_PRIVATE JSValue evaluateBuiltinModule(JSGlobalObject*, uint32_t id);
// For whoever makes the functions that the engine or the host implements in JavaScript. Whether the image has the function of this number.
JS_EXPORT_PRIVATE bool hasBuiltinFunction(VM&, uint32_t id);
// A new function object for it, which the image has. With another prototype than its kind of function has, if one is given. It can be made
// while the global object is being created: until that is done it may be stored, and stored to with putDirect(), and nothing else.
JS_EXPORT_PRIVATE JSFunction* builtinFunction(JSGlobalObject*, uint32_t id, JSObject* prototype = nullptr);
// The global object is complete.
void didFinishCreation(JSGlobalObject*);
// A function that is compiled from types is a JSFunction with a NativeExecutable, which says nothing of what it was written as.
enum class DeclaredAs : uint8_t { NotCompiled, Function, AsyncFunction, GeneratorFunction, AsyncGeneratorFunction, Class };
JS_EXPORT_PRIVATE DeclaredAs declaredAs(JSCell*);
// Whether it is such a function, and was written in one of the host's own modules.
JS_EXPORT_PRIVATE bool isOfBuiltinModule(JSCell*);
// What a compiled generator function or async generator function returns. It is no JSGenerator.
JS_EXPORT_PRIVATE bool isGeneratorObject(JSCell*);
// After `prototype` has been stored to a function with putDirect(), which goes past the function's own methods. Any cell will do.
JS_EXPORT_PRIVATE void didChangePrototypeProperty(VM&, JSCell* function);

} // namespace Typed
JS_EXPORT_PRIVATE JSGlobalObject* globalObjectForInstanceToken(VM&, JSCell* token);

struct ImageAddressInfo {
    enum Kind : uint8_t {
        NotInImage,
        Function,
        Stub,
        Adapter,
    };
    Kind kind { NotInImage };
    uint32_t index { 0 };
    uint32_t offset { 0 };
};
JS_EXPORT_PRIVATE ImageAddressInfo classifyAddress(const void* address);
JS_EXPORT_PRIVATE std::optional<uint32_t> imageCodeOffset(const void* address);
JS_EXPORT_PRIVATE void* returnAddressForFrame(const void* frame, const void* startingFrom);
struct FrameAndPC {
    void* frame { nullptr };
    void* pc { nullptr };
};
JS_EXPORT_PRIVATE std::optional<FrameAndPC> innermostFrame(void* machineFrame, void* machinePC, void* machineLinkRegister, void* topCallFrame, const StackBounds&);
JS_EXPORT_PRIVATE uint32_t callSiteAt(const ImageFunction&, uint32_t offsetOfReturnAddress);
JS_EXPORT_PRIVATE const RegisterAtOffsetList& adapterSavedRegisters();
// What calleeSaveRegistersOf() says, without a list: by Reg::index(); they are saved in that order, a word each, the first that many words from the frame pointer.
struct SavedRegisters {
    uint64_t mask;
    ptrdiff_t first;
};
JS_EXPORT_PRIVATE SavedRegisters savedRegistersOf(const ImageFunction&);
JS_EXPORT_PRIVATE bool hasCode();
JS_EXPORT_PRIVATE std::optional<uint32_t> tryCallSiteAt(const ImageFunction&, uint32_t offsetOfReturnAddress);
std::optional<uint32_t> spreadSite(const ImageFunction&, uint32_t callSite, unsigned item);
struct ImageInlineFrame {
    uint32_t parent;
    uint32_t callSite;
    uint32_t function;
    bool isTailCall;
};
JS_EXPORT_PRIVATE ImageInlineFrame inlineFrameOf(const ImageFunction&, unsigned frame);
// Of a function whose sites are numbered in the order of the code: the inline frame of number 1, 2, .. A number without code: a call that was
// inlined is in the frame it stands in; whatever else has a number of that kind is in the function itself.
class FramesOfSites {
public:
    explicit FramesOfSites(const ImageFunction&);
    unsigned next();

private:
    const uint8_t* m_inlineFrames { nullptr };
    const uint8_t* m_sites { nullptr };
    uint64_t m_inFunctionItself { 0 };
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
JS_EXPORT_PRIVATE bool canFindInstanceForFrame(const void* frame);
JS_EXPORT_PRIVATE bool topCallFrameIsAOTFrame(const void* frame);
JS_EXPORT_PRIVATE FunctionRef callerFunction(const CallFrame*);
JS_EXPORT_PRIVATE JSCell* callerOwnerCell(const CallFrame*);
JS_EXPORT_PRIVATE bool hasCompiledCode(VM&, JSFunction*);
void countAwait(VM&, const char* where, JSValue operand);
void countJobCall(VM&, bool entersStaticCode);
void noteGuest(JSGlobalObject*, const char* kind, StringView group, StringView text);

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
