/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRemovableFeature.h"
#include "AOTSchemaRuntimeOperations.h"
#include "CallLinkInfo.h"
#include <memory>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class JSGlobalObject;
class Structure;

namespace AOT {

enum class LeafAnswer : size_t { TakeSlowPath, No, Yes };
enum class LeafPresence : size_t { TakeSlowPath, Absent, Inherited, Own };

// What the code generator told the back end, and a run has found untrue. --aot-check=ClaimsToBackEnd, --aot-check=MovedLoads
enum class BrokenClaim : uint32_t {
    StackSlotIsWrittenBeforeRead,
    WordNeverChanges,
    LoadMayMove,
    ValueIsInRange,
    ComparisonIsDecided,
    CounterStepsAsTold,
};

#if ASAN_ENABLED && CPU(X86_64) && OS(LINUX)
#define AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE 1
#define FOR_EACH_AOT_OPERATION_FOR_ASAN(v) \
    v(operationAOTReportPoisonedAccess)
#else
#define AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE 0
#define FOR_EACH_AOT_OPERATION_FOR_ASAN(v)
#endif

// No compiled code calls the reserved ones. The entries behind them keep their numbers.
#define FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(v, reserved) \
    v(operationAOTPrepareProxyCall) \
    FOR_EACH_AOT_OPERATION_FOR_ASAN(v) \
    v(operationAOTCallEngineCallee) \
    v(operationAOTReportBrokenClaim) \
    reserved(operationArrayIndexOfValueInt32OrContiguous) \
    reserved(operationArrayIncludesValueInt32OrContiguous) \
    v(operationAOTLookupExceptionHandler) \
    v(operationAOTFindLandingOfMisfit) \
    v(operationAOTBytesAllocatedAtPrologueCheck) \
    v(operationAOTCopyDataPropertiesByFieldMap)

#define FOR_EACH_AOT_THUNK(v) \
    v(HandleException)

#define FOR_EACH_AOT_POINTER(v) \
    v(CallLinkInfoForCall) \
    v(VirtualCallTrampoline) \
    v(ErrorConstructorClassInfo) \
    v(NonConstructorHostFunction) \
    v(AsyncGeneratorClassInfo) \
    v(GeneratorClassInfo) \
    v(WebAssemblyFunctionClassInfo) \
    v(CallNativeFunctionUnderValidation) \
    v(ConstructNativeFunctionUnderValidation) \
    v(CheckThatThrowingCallHasSite)

enum class Entry : uint16_t {
#define AOT_DEFINE_SCHEMA_ENTRY(name) operationAOT##name,
#define AOT_DEFINE_ENTRY(name) name,
    FOR_EACH_AOT_RUNTIME_OPERATION(AOT_DEFINE_SCHEMA_ENTRY)
    FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(AOT_DEFINE_ENTRY, AOT_DEFINE_ENTRY)
    FOR_EACH_AOT_THUNK(AOT_DEFINE_ENTRY)
    FOR_EACH_AOT_POINTER(AOT_DEFINE_ENTRY)
#undef AOT_DEFINE_ENTRY
#undef AOT_DEFINE_SCHEMA_ENTRY
#define AOT_STUB_FRONT_END(Operation, Family) Raw##Operation,
#include "AOTStubFamilies.def"
#undef AOT_STUB_FRONT_END
    NumberOfEntries
};

constexpr unsigned numberOfEntries = static_cast<unsigned>(Entry::NumberOfEntries);
constexpr unsigned numberOfRuntimeOperations = numberOfEnumerators<RuntimeOperation>;
static_assert(numberOfEntries <= 4096, "An ARM64 load from the table reaches 32 KB.");

constexpr uint32_t offsetInRuntimeTable(Entry entry)
{
    return static_cast<uint32_t>(entry) * sizeof(void*);
}

constexpr Entry entryAtOffset(uint32_t offset)
{
    uint32_t index = offset / sizeof(void*);
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(index < numberOfEntries);
    Entry entry = static_cast<Entry>(index);
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(offsetInRuntimeTable(entry) == offset);
    return entry;
}

constexpr Entry entryOf(RuntimeOperation operation)
{
    return static_cast<Entry>(operation);
}

constexpr std::optional<RuntimeOperation> runtimeOperationOf(Entry entry)
{
    if (static_cast<unsigned>(entry) >= numberOfRuntimeOperations)
        return std::nullopt;
    return static_cast<RuntimeOperation>(entry);
}

constexpr bool isOperation(Entry entry)
{
    if (runtimeOperationOf(entry))
        return true;
    switch (entry) {
#define AOT_CASE_OF_OPERATION(name) case Entry::name:
    FOR_EACH_AOT_OPERATION_OUTSIDE_SCHEMA(AOT_CASE_OF_OPERATION, AOT_CASE_OF_OPERATION)
#undef AOT_CASE_OF_OPERATION
        return true;
    default:
        return false;
    }
}

constexpr bool isRawOperation(Entry entry)
{
    switch (entry) {
#define AOT_STUB_FRONT_END(Operation, Family) case Entry::Raw##Operation:
#include "AOTStubFamilies.def"
#undef AOT_STUB_FRONT_END
        return true;
    default:
        return false;
    }
}

constexpr ArgumentBufferAccess argumentBufferAccessOf(Entry entry)
{
    if (std::optional<RuntimeOperation> operation = runtimeOperationOf(entry))
        return argumentBufferAccessOf(*operation);
    return ArgumentBufferAccess::ReadsAndWrites;
}

constexpr std::optional<RemovableFeature> featureOf(Entry entry)
{
    switch (entry) {
    case Entry::operationAOTFormatNumber:
        return RemovableFeature::IntlNumberFormat;
    case Entry::operationAOTFormatDateTime:
        return RemovableFeature::IntlDateTimeFormat;
    case Entry::operationAOTRegExpExec:
    case Entry::operationAOTRegExpTest:
    case Entry::operationAOTRegExpFind:
    case Entry::operationAOTRegExpTestBoxed:
    case Entry::operationAOTRegExpMatchLiteral:
    case Entry::operationAOTRegExpMatchLiteralWithoutObject:
    case Entry::operationAOTRegExpMatchFlatProgram8:
    case Entry::operationAOTNewRegExpLiteral:
    case Entry::operationAOTSharedRegExpLiteral:
    case Entry::operationAOTNewRegExp:
    case Entry::operationAOTRegExpFlags:
    case Entry::operationAOTRegExpSource:
    case Entry::operationAOTStringReplaceRegExp:
    case Entry::operationAOTStringReplaceAllRegExp:
    case Entry::operationAOTStringReplaceRegExpString:
    case Entry::operationAOTStringReplaceRegExpWithFunction:
    case Entry::operationAOTStringSplitRegExp:
    case Entry::operationAOTStringMatchRegExp:
    case Entry::operationAOTStringSearchRegExp:
    case Entry::operationAOTRegExpExecPrefix:
    case Entry::operationAOTStringMatchPrefix:
        return RemovableFeature::RegExp;
    default:
        return std::nullopt;
    }
}

JS_EXPORT_PRIVATE ASCIILiteral nameOf(Entry);

static_assert(entryAtOffset(offsetInRuntimeTable(Entry::HandleException)) == Entry::HandleException);
static_assert(entryOf(RuntimeOperation::ValueAdd) == Entry::operationAOTValueAdd);
static_assert(runtimeOperationOf(Entry::operationAOTValueAdd) == RuntimeOperation::ValueAdd);
static_assert(!runtimeOperationOf(Entry::operationAOTLookupExceptionHandler));
static_assert(!runtimeOperationOf(Entry::HandleException));
static_assert(argumentBufferAccessOf(Entry::operationAOTValueAdd) == ArgumentBufferAccess::None);
static_assert(argumentBufferAccessOf(Entry::operationAOTNewArray) == ArgumentBufferAccess::Reads);
static_assert(argumentBufferAccessOf(Entry::operationArrayIndexOfValueInt32OrContiguous) == ArgumentBufferAccess::ReadsAndWrites);
static_assert(featureOf(Entry::operationAOTFormatNumber) == RemovableFeature::IntlNumberFormat);
static_assert(featureOf(Entry::operationAOTRegExpTest) == RemovableFeature::RegExp);
static_assert(!featureOf(Entry::operationAOTValueAdd));
static_assert(isOperation(Entry::operationArrayIndexOfValueInt32OrContiguous) && !isOperation(Entry::VirtualCallTrampoline));

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTReportPoisonedAccess, void, (Instance*, void* address, uint32_t sizeAndIsStore, void* const* stackPointer));
#endif
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTLookupExceptionHandler, UGPRPair, (VM*, Instance*));
// Stands in its place in the table of an image that shares realms.
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTLookupExceptionHandlerAmongImages, UGPRPair, (VM*, Instance*));
// How far the stack pointer at the stack check of a frame without a record is below the return address of the frame. Zero for a frame that has a record.
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTBytesAllocatedAtPrologueCheck, UGPRPair, (Instance*, const void* returnAddress));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTFindLandingOfMisfit, void*, (Instance*, void* returnAddress, EncodedJSValue misfit));
// What is to be called in place of the Proxy that is the callee of the frame, and, where that is the trap, the list of arguments, or one where the caller makes it.
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTPrepareProxyCall, UGPRPair, (Instance*, CallFrame* hostCallFrame, uint32_t makesList));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationAOTReportBrokenClaim, void, (uint32_t claimAndOffset, uint32_t function, uint32_t valueIndex, const uint64_t* foundAndGiven));
JSC_DECLARE_JIT_OPERATION(operationAOTCallEngineCallee, EncodedJSValue, (Instance*, EncodedJSValue callee, EncodedJSValue thisValue, uint32_t count, const EncodedJSValue* arguments));
JSC_DECLARE_JIT_OPERATION(operationAOTCopyDataPropertiesByFieldMap, void, (Instance*, JSCell* target, EncodedJSValue source, uint32_t excludedRun, uint32_t count, const EncodedJSValue* excludedKeys));

class RuntimeTable {
    WTF_MAKE_TZONE_ALLOCATED(RuntimeTable);
    WTF_MAKE_NONCOPYABLE(RuntimeTable);
public:
    explicit RuntimeTable(VM&);
    ~RuntimeTable();

    void** entries() { return m_entries; }

    void finishInitialization(VM&);

    bool countsCalls() const { return m_countsCalls; }
    uint64_t callCount(Entry entry) const { return m_callCounts[static_cast<unsigned>(entry)]; }
    void didCall(Entry entry)
    {
        ++m_callCounts[static_cast<unsigned>(entry)];
        m_lastCalled = entry;
    }
    // Compiled code leaves its work to an operation where an allocator has no cell at hand. Such a call refills the allocator. Only the first refill behind a call is counted.
    void willRefillAllocator()
    {
        if (m_lastCalled == Entry::NumberOfEntries)
            return;
        if (m_callsThatRefilled.isEmpty())
            m_callsThatRefilled.fill(0, numberOfEntries);
        ++m_callsThatRefilled[static_cast<unsigned>(std::exchange(m_lastCalled, Entry::NumberOfEntries))];
    }
    uint64_t numberOfCallsThatRefilled(Entry entry) const { return m_callsThatRefilled.isEmpty() ? 0 : m_callsThatRefilled[static_cast<unsigned>(entry)]; }

    Structure* compactTableStructure() const { return m_compactTableStructure; }
    uint32_t compactTableIterationEntry() const { return m_compactTableIterationEntry; }
    void setCompactTableIterationEntry(uint32_t entry) { m_compactTableIterationEntry = entry; }

    const Identifier& nameOfApplyTrap(VM&);

private:
#if defined(BUN_HARDENED)
    void** m_entries;
#else
    void* m_entries[numberOfEntries] { };
#endif
    uint64_t m_callCounts[numberOfEntries] { };
    WTF::Vector<uint64_t> m_callsThatRefilled;
    Entry m_lastCalled { Entry::NumberOfEntries };
    DataOnlyCallLinkInfo m_callLinkInfoForCall;
    Structure* m_compactTableStructure { nullptr };
    uint32_t m_compactTableIterationEntry { 0 };
    Identifier m_nameOfApplyTrap;
    bool m_countsCalls { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
