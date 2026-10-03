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
    static uint32_t encode(unsigned inlineCapacity, unsigned count) { return inlineCapacity << 16 | count; }
    static uint32_t encode(unsigned identifier, bool isDefined, bool isStrict, bool isAssigned) { return identifier << 3 | isAssigned << 2 | isDefined << 1 | isStrict; }

    explicit operator bool() const { return !!words; }
    unsigned inlineCapacity() const { return words[0] >> 16; }
    unsigned count() const { return words[0] & 0xffff; }
    unsigned identifier(unsigned i) const { return words[i + 1] >> 3; }
    bool isAssigned(unsigned i) const { return words[i + 1] & 4; }
    bool isDefined(unsigned i) const { return words[i + 1] & 2; }
    bool isStrict(unsigned i) const { return words[i + 1] & 1; }

    const uint32_t* words { nullptr };
};

struct FunctionRef {
    JS_EXPORT_PRIVATE static FunctionRef at(Instance*, const void* address);
    struct Location;
    JS_EXPORT_PRIVATE Location locationForReturnAddress(const void* returnAddress) const;
    JS_EXPORT_PRIVATE BytecodeIndex bytecodeIndexAt(const void* returnAddress) const;
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
};

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
class CallSiteOverride {
    WTF_MAKE_NONCOPYABLE(CallSiteOverride);
public:
    CallSiteOverride(Instance&, const void* returnAddress, uint32_t site);
    ~CallSiteOverride();
private:
    Instance& m_instance;
};
JS_EXPORT_PRIVATE Instance* instanceForFrame(const void* frame);
JS_EXPORT_PRIVATE bool canFindInstanceForFrame(const void* frame);
JS_EXPORT_PRIVATE bool topCallFrameIsAOTFrame(const void* frame);
JS_EXPORT_PRIVATE FunctionRef callerFunction(const CallFrame*);
JS_EXPORT_PRIVATE CodeBlock* callerCodeBlock(const CallFrame*);

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
