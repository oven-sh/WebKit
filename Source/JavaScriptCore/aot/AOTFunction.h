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

// How to pack what is seldom wanted (the text that error messages quote), and unpack it. Without it nothing is packed, and an image in which something is cannot be quoted from.
// compress: how many bytes it came to, or zero. decompress: whether it came to exactly `size` bytes.
using Compress = size_t (*)(const uint8_t* source, size_t sourceSize, uint8_t* destination, size_t capacity);
using Decompress = bool (*)(const uint8_t* source, size_t sourceSize, uint8_t* destination, size_t size);
JS_EXPORT_PRIVATE void setCodec(Compress, Decompress);

// Where an object is made together with its first properties: which they are. It is what the bytecode says, for whoever makes the
// object the long way, without the bytecode. One word, and then one for each property.
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

// What a frame of code from the static compiler is a frame of: a function, in a realm. There need be nothing in memory that stands
// for it (SharedData).
struct FunctionRef {
    // What has that address in its code.
    JS_EXPORT_PRIVATE static FunctionRef at(Instance*, const void* address);
    // Where in its bytecode it is, if that is where it is going to be returned to. It may be in the middle of what a function does that
    // was made part of it: then that is the function, and this is where in that one's. inlineFrame: which call that was, or none.
    struct Place;
    JS_EXPORT_PRIVATE Place placeAt(const void* returnAddress) const;
    JS_EXPORT_PRIVATE BytecodeIndex bytecodeIndexAt(const void* returnAddress) const; // Place::bytecodeIndex
    // The call itself: where that was, and in what.
    JS_EXPORT_PRIVATE Place placeOfInlinedCall(unsigned inlineFrame) const;
    JS_EXPORT_PRIVATE static FunctionRef of(CodeBlock*); // None, unless its code is the static compiler's.
    // What has run as this executable's code of this kind. None, if its realm is no more.
    JS_EXPORT_PRIVATE static FunctionRef of(VM&, FunctionExecutable*, CodeSpecializationKind);
    explicit operator bool() const { return !!instance; }

    // These ask for nothing to be made, take no lock, and can be asked on any thread that has the VM's stopped.
    const FunctionInfo& info() const;
    JS_EXPORT_PRIVATE Data* dataIfItHasAny() const;
    JS_EXPORT_PRIVATE ScriptExecutable* executable() const;
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* unlinkedCodeBlockIfThereIsOne() const; // There is, if there are no facts().
    JS_EXPORT_PRIVATE CodeBlock* codeBlockIfThereIsOne() const;
    const FunctionMetadata* facts() const;
    // What the unlinked code says, whether it is there or not.
    JS_EXPORT_PRIVATE CodeType codeType() const;
    JS_EXPORT_PRIVATE bool isBuiltinFunction() const;
    JS_EXPORT_PRIVATE unsigned instructionsSize() const;
    JS_EXPORT_PRIVATE const UnlinkedHandlerInfo* handlerFor(unsigned bytecodeOffset) const;
    JS_EXPORT_PRIVATE void* addressOfCatchEntrypoint(unsigned bytecodeOffset) const; // Where the code is for the op_catch that is there.
    const UnlinkedStringJumpTable& stringSwitchJumpTable(unsigned) const;
    JS_EXPORT_PRIVATE const IdentifierSet& constantIdentifierSet(unsigned) const;
    // Of the body of an async function that is waiting in that state: where it goes on from. Nowhere in particular: the beginning.
    JS_EXPORT_PRIVATE BytecodeIndex resumePointOf(int32_t state) const;
    // The functions in it. (See UnlinkedCodeBlock::executableIn().)
    std::span<const WriteBarrier<UnlinkedFunctionExecutable>> functionDecls() const;
    std::span<const WriteBarrier<UnlinkedFunctionExecutable>> functionExprs() const;

    JS_EXPORT_PRIVATE LineColumn lineColumnFor(BytecodeIndex) const; // CodeBlock::lineColumnForBytecodeIndex()
    // Decoded now, if it is not there. For what nothing else will do for, which is little. As for ensureData().
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* ensureUnlinkedCodeBlock() const;
    UnlinkedCodeBlock* makeUnlinkedCodeBlockFromMetadata() const;
    JS_EXPORT_PRIVATE CodeBlock* ensureCodeBlock() const;
    // Its own, which it gets now if it has been doing without. Not while the collector is at work, and on no thread but the VM's.
    JS_EXPORT_PRIVATE Data* ensureData() const;
    // For the functions that the function makes closures of.
    FunctionExecutable* functionDecl(unsigned) const;
    FunctionExecutable* functionExpr(unsigned) const;
    // The number that goes with the slot (ImageFunction::siteConstants()), which is one of the function's.
    uint32_t siteConstantOf(const Slot*) const;
    // What the source says there, and whether it is exactly that: of a program that goes without its text (Image::quoteAt()).
    JS_EXPORT_PRIVATE std::optional<std::pair<String, bool>> quoteAt(BytecodeIndex) const;
    // Where a frame that is there is to be reported to be. If `source` is not zero that is a place in one of the sources that the
    // program was made from (StaticHeap::nameOfSource()), and otherwise in the text of the module. None: go by lineColumnFor().
    struct ReportedPosition {
        LineColumn lineColumn;
        uint32_t source { 0 };
    };
    enum class OfConstruction : bool { WhereItIs, WhereItStarts }; // The latter: at the `new`.
    JS_EXPORT_PRIVATE std::optional<ReportedPosition> reportedPositionFor(BytecodeIndex, OfConstruction = OfConstruction::WhereItIs) const;
    // Whether the instruction there is one of those that construct: for whoever would look at it, if it were there to look at.
    JS_EXPORT_PRIVATE bool constructsAt(BytecodeIndex) const;
    AllocationPlan planOf(const Slot* firstOfSite) const; // None, if the code is not from an image: then there is bytecode.

    Instance* instance { nullptr };
    uint32_t index { 0 };
};

struct FunctionRef::Place {
    FunctionRef function;
    BytecodeIndex bytecodeIndex;
    unsigned inlineFrame { 0 };
    // placeOfInlinedCall(): the call was a tail call, so this is somewhere that nobody is any more.
    bool hasBeenLeft { false };
    // placeAt(): likewise the call that is being made here, once what is called runs (PackedSite::isTailCall).
    bool isTailCall { false };
};

// ---- Frames
//
// A frame of this compiler's code has nothing in it that says so. What it is a frame of is told from the address that is going to be
// returned to in it, which is in the frame of whatever it called.
struct ImageAddressInfo {
    enum Kind : uint8_t {
        NotInImage,
        Function, // In the code of a function: `index` says which, and `offset` how far in.
        // In a stub. Its frame, if it has one, is nobody's: whoever walks the stack goes on to the next, which is that of what
        // called the stub.
        Stub,
        // In a stub that was called the way the rest of the engine calls a function (adapt()): what is above its frame pointer is
        // what such a caller puts there, its caller is found the way such a frame's is, and it has saved registers.
        Adapter,
    };
    Kind kind { NotInImage };
    uint32_t index { 0 };
    uint32_t offset { 0 };
};
JS_EXPORT_PRIVATE ImageAddressInfo classifyAddress(const void* address); // Any thread.
// The address that is going to be returned to in `frame`: found by going from frame to frame, starting at one that is further in
// (that of a function of C++ that is running, or the EntryFrame of code that `frame` is waiting for). Null: it is not out from there.
JS_EXPORT_PRIVATE void* returnAddressInto(const void* frame, const void* startingFrom);
// Where in its bytecode a function is that is going to be returned to that far into its code (CallSiteIndex::bits()).
JS_EXPORT_PRIVATE uint32_t callSiteAt(const ImageFunction&, uint32_t offsetOfReturnAddress);
JS_EXPORT_PRIVATE const RegisterAtOffsetList& adapterSavedRegisters();
JS_EXPORT_PRIVATE bool hasCode(); // There is an image with code in it: otherwise none of this comes to anything.
// Nothing: nobody was to ask about what is called from there, or it is not where anything is going to return to.
JS_EXPORT_PRIVATE std::optional<uint32_t> tryCallSiteAt(const ImageFunction&, uint32_t offsetOfReturnAddress);
std::optional<uint32_t> siteOfSpread(const ImageFunction&, uint32_t callSite, unsigned item); // See SiteOfSpread.
// A call that was done away with (Graph::InlineFrame): what it was in (another such call, or none: the function whose code it is), where
// it was there, and which function it was of.
struct InlineFrameOfImage {
    uint32_t parent;
    uint32_t callSite;
    uint32_t function;
    bool isTailCall;
};
JS_EXPORT_PRIVATE InlineFrameOfImage inlineFrameOf(const ImageFunction&, unsigned frame);
// For as long as there is one of these, the function that is going to be returned to there is somewhere else as far as anybody can tell.
class CallSiteOverride {
    WTF_MAKE_NONCOPYABLE(CallSiteOverride);
public:
    CallSiteOverride(Instance&, const void* returnAddress, uint32_t site);
    ~CallSiteOverride();
private:
    Instance& m_instance;
};
// Of a frame of code from the static compiler, or of a stub that such code called: whose realm's. The adapter that let the code in says.
JS_EXPORT_PRIVATE Instance* instanceOfFrame(const void* frame);
// Whether that can be asked. It can of any frame of such code that anything is expected to look at; something that looks at the stack at
// any time at all (a profiler of allocations) may find a call on its way in, with nothing above it yet that says whose it is.
JS_EXPORT_PRIVATE bool canTellInstanceOfFrame(const void* frame);
// frame: the last that the VM was told of (VM::topCallFrame). Whether it is a stub's, or such code's: then there is nothing in it of what a
// frame has in the engine's own convention, and nothing is to be asked of it but by way of a StackVisitor.
JS_EXPORT_PRIVATE bool topFrameIsNotTheEnginesOwn(const void* frame);
// frame: one in the engine's own convention, of a host function, say. The function that made the call, if it is code from the static compiler.
// (The function whose bytecode it is that made the call: see FunctionRef::placeAt().)
JS_EXPORT_PRIVATE FunctionRef callerFunction(const CallFrame*);
// Its CodeBlock, made now if there is none, for whoever has to have one to report an error with. Null if it is not such code.
JS_EXPORT_PRIVATE CodeBlock* codeBlockOfCaller(const CallFrame*);

} } // namespace JSC::AOT
