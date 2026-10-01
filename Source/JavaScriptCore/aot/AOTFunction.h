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
#include <wtf/text/WTFString.h>

namespace JSC {

class CallFrame;
class CodeBlock;
class FunctionExecutable;
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
struct ImageFunction;
struct Instance;
struct Slot;

// Compression hooks for rarely used data (the source text quoted in error messages). Without them nothing is compressed, and an
// image with compressed quotes cannot produce them.
// Compress returns the compressed size, or zero on failure. Decompress returns whether the output was exactly `size` bytes.
using Compress = size_t (*)(const uint8_t* source, size_t sourceSize, uint8_t* destination, size_t capacity);
using Decompress = bool (*)(const uint8_t* source, size_t sourceSize, uint8_t* destination, size_t size);
JS_EXPORT_PRIVATE void setCodec(Compress, Decompress);

// Describes an allocation site that creates an object together with its initial properties. It records what the bytecode says, for
// the slow path, which runs without bytecode. The layout is one header word followed by one word per property.
struct AllocationPlan {
    static uint32_t encode(unsigned inlineCapacity, unsigned count) { return inlineCapacity << 16 | count; }
    static uint32_t encode(unsigned identifier, bool isDefined, bool isStrict) { return identifier << 2 | isDefined << 1 | isStrict; }

    explicit operator bool() const { return !!words; }
    unsigned inlineCapacity() const { return words[0] >> 16; }
    unsigned count() const { return words[0] & 0xffff; }
    unsigned identifier(unsigned i) const { return words[i + 1] >> 2; }
    bool isDefined(unsigned i) const { return words[i + 1] & 2; } // As NewObjectPlan::Property.
    bool isStrict(unsigned i) const { return words[i + 1] & 1; }

    const uint32_t* words { nullptr };
};

// Identifies what a frame of ahead-of-time compiled code is executing: a function, in a realm. There may be no object in memory
// that represents it (SharedData).
struct FunctionRef {
    // The function whose code contains `address`.
    JS_EXPORT_PRIVATE static FunctionRef at(Instance*, const void* address);
    // The bytecode location corresponding to a return address in this function's code. If the address is inside an inlined callee,
    // the result names that callee and the location within it, and inlineFrame identifies the inlined call.
    struct Location;
    JS_EXPORT_PRIVATE Location locationForReturnAddress(const void* returnAddress) const;
    JS_EXPORT_PRIVATE BytecodeIndex bytecodeIndexAt(const void* returnAddress) const; // Location::bytecodeIndex
    // The location of the inlined call itself, in its caller.
    JS_EXPORT_PRIVATE Location inlineCallSiteLocation(unsigned inlineFrame) const;
    JS_EXPORT_PRIVATE static FunctionRef of(CodeBlock*); // Empty unless its code is ahead-of-time compiled.
    // The function that has run as this executable's code of this kind. Empty if its realm is gone.
    JS_EXPORT_PRIVATE static FunctionRef of(VM&, FunctionExecutable*, CodeSpecializationKind);
    explicit operator bool() const { return !!instance; }

    // These allocate nothing, take no locks, and may be called from any thread while the VM's thread is stopped.
    const FunctionInfo& info() const;
    JS_EXPORT_PRIVATE Data* dataIfExists() const;
    JS_EXPORT_PRIVATE ScriptExecutable* executable() const;
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* unlinkedCodeBlockIfExists() const; // Non-null if there is no metadata().
    JS_EXPORT_PRIVATE CodeBlock* codeBlockIfExists() const;
    const FunctionMetadata* metadata() const;
    // Properties of the unlinked code block, available whether or not it has been decoded.
    JS_EXPORT_PRIVATE CodeType codeType() const;
    JS_EXPORT_PRIVATE bool isBuiltinFunction() const;
    JS_EXPORT_PRIVATE unsigned instructionsSize() const;
    JS_EXPORT_PRIVATE const UnlinkedHandlerInfo* handlerFor(unsigned bytecodeOffset) const;
    JS_EXPORT_PRIVATE void* addressOfCatchEntrypoint(unsigned bytecodeOffset) const; // The code address for the op_catch at this offset.
    const UnlinkedStringJumpTable& stringSwitchJumpTable(unsigned) const;
    JS_EXPORT_PRIVATE const IdentifierSet& constantIdentifierSet(unsigned) const;
    // For an async function body suspended in `state`: where it resumes. Returns the start of the function for an unknown state.
    JS_EXPORT_PRIVATE BytecodeIndex resumePointOf(int32_t state) const;
    // Its inner functions. See UnlinkedCodeBlock::executableIn().
    std::span<const WriteBarrier<UnlinkedFunctionExecutable>> functionDecls() const;
    std::span<const WriteBarrier<UnlinkedFunctionExecutable>> functionExprs() const;

    JS_EXPORT_PRIVATE LineColumn lineColumnFor(BytecodeIndex) const; // As CodeBlock::lineColumnForBytecodeIndex().
    // Decodes the unlinked code block if necessary. Rarely needed. Same threading restrictions as ensureData().
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* ensureUnlinkedCodeBlock() const;
    UnlinkedCodeBlock* makeUnlinkedCodeBlockFromMetadata() const;
    JS_EXPORT_PRIVATE CodeBlock* ensureCodeBlock() const;
    // Returns the function's own Data, allocating it if the function has been using SharedData. Main thread only, and not during
    // GC.
    JS_EXPORT_PRIVATE Data* ensureData() const;
    // Executables for the function's inner functions.
    FunctionExecutable* functionDecl(unsigned) const;
    FunctionExecutable* functionExpr(unsigned) const;
    // The site constant (ImageFunction::siteConstants()) for one of this function's slots.
    uint32_t siteConstantOf(const Slot*) const;
    // The source text at this location and whether it is exact, for programs built without source text (Image::quoteAt()).
    JS_EXPORT_PRIVATE std::optional<std::pair<String, bool>> quoteAt(BytecodeIndex) const;
    // The position to report for a frame at this location. If `source` is nonzero, the position is in one of the original sources
    // (StaticHeap::nameOfSource()); otherwise it is in the bundled module text. Returns nullopt if lineColumnFor() should be used.
    struct ReportedPosition {
        LineColumn lineColumn;
        uint32_t source { 0 };
    };
    enum class OfConstruction : bool { WhereItIs, WhereItStarts }; // WhereItStarts: at the `new` keyword.
    JS_EXPORT_PRIVATE std::optional<ReportedPosition> reportedPositionFor(BytecodeIndex, OfConstruction = OfConstruction::WhereItIs) const;
    // Whether the instruction at this location is a construct, for callers that cannot inspect the bytecode.
    JS_EXPORT_PRIVATE bool constructsAt(BytecodeIndex) const;
    AllocationPlan planOf(const Slot* firstOfSite) const; // Empty if the code is not from an image, in which case the bytecode is available.

    Instance* instance { nullptr };
    uint32_t index { 0 };
};

struct FunctionRef::Location {
    FunctionRef function;
    BytecodeIndex bytecodeIndex;
    unsigned inlineFrame { 0 };
    // Set by inlineCallSiteLocation(): the call was a tail call, so this frame no longer logically exists.
    bool isTailDeleted { false };
    // Set by locationForReturnAddress(): the call being made here is a tail call (PackedSite::isTailCall).
    bool isTailCall { false };
};

// ---- Frames
//
// A frame of ahead-of-time compiled code carries no marker and no CodeBlock. Its function is identified from the return address
// into it, which is stored in its callee's frame.
struct ImageAddressInfo {
    enum Kind : uint8_t {
        NotInImage,
        Function, // In a function's code. `index` is the function and `offset` the offset within it.
        // In a stub. Its frame, if it has one, is skipped by stack walkers, which continue with the stub's caller.
        Stub,
        // In an entry adapter: a stub that is called with the engine's standard calling convention (adapt()). It has a standard
        // frame header, its caller is found as for any standard frame, and it has saved callee-saved registers.
        Adapter,
    };
    Kind kind { NotInImage };
    uint32_t index { 0 };
    uint32_t offset { 0 };
};
JS_EXPORT_PRIVATE ImageAddressInfo classifyAddress(const void* address); // Any thread.
// The return address into `frame`, found by walking outward from a more recent frame (that of a running C++ function, or the
// EntryFrame of code that `frame` is waiting on). Returns null if `frame` is not reachable from there.
JS_EXPORT_PRIVATE void* returnAddressForFrame(const void* frame, const void* startingFrom);
// The call site (CallSiteIndex::bits()) for a return address at this offset in the function's code.
JS_EXPORT_PRIVATE uint32_t callSiteAt(const ImageFunction&, uint32_t offsetOfReturnAddress);
JS_EXPORT_PRIVATE const RegisterAtOffsetList& adapterSavedRegisters();
JS_EXPORT_PRIVATE bool hasCode(); // Whether an image with code is loaded. If not, none of these functions apply.
// Returns nullopt if the offset is not a return address, or if its call site was not recorded.
JS_EXPORT_PRIVATE std::optional<uint32_t> tryCallSiteAt(const ImageFunction&, uint32_t offsetOfReturnAddress);
std::optional<uint32_t> siteOfSpread(const ImageFunction&, uint32_t callSite, unsigned item); // See SiteOfSpread.
// A call that was inlined (Graph::InlineFrame): its parent (another inline frame, or none for the outermost function), its call
// site in the parent, and the callee.
struct ImageInlineFrame {
    uint32_t parent;
    uint32_t callSite;
    uint32_t function;
    bool isTailCall;
};
JS_EXPORT_PRIVATE ImageInlineFrame inlineFrameOf(const ImageFunction&, unsigned frame);
// While this object is alive, the function that will be returned to at returnAddress reports `site` as its current call site.
class CallSiteOverride {
    WTF_MAKE_NONCOPYABLE(CallSiteOverride);
public:
    CallSiteOverride(Instance&, const void* returnAddress, uint32_t site);
    ~CallSiteOverride();
private:
    Instance& m_instance;
};
// The Instance for a frame of ahead-of-time compiled code, or of a stub called by such code. It is recovered from the entry
// adapter's frame.
JS_EXPORT_PRIVATE Instance* instanceForFrame(const void* frame);
// Whether instanceForFrame() can be called. It can for any frame that stack walkers are expected to visit. An asynchronous observer
// (such as an allocation profiler) may see a call that is still being set up, with no adapter frame above it yet.
JS_EXPORT_PRIVATE bool canFindInstanceForFrame(const void* frame);
// `frame` is VM::topCallFrame. Returns whether it belongs to a stub or to ahead-of-time compiled code. Such a frame has no standard
// frame header and must only be inspected through a StackVisitor.
JS_EXPORT_PRIVATE bool topCallFrameIsAOTFrame(const void* frame);
// `frame` uses the standard calling convention (a host function's frame, for example). Returns the calling function if it is
// ahead-of-time compiled. With inlining, this is the function whose bytecode made the call (see FunctionRef::locationForReturnAddress()).
JS_EXPORT_PRIVATE FunctionRef callerFunction(const CallFrame*);
// The caller's CodeBlock, created if necessary, for error reporting paths that require one. Null if the caller is not ahead-of-time
// compiled.
JS_EXPORT_PRIVATE CodeBlock* codeBlockOfCaller(const CallFrame*);

} } // namespace JSC::AOT
