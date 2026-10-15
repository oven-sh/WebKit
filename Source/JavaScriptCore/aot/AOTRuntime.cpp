/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntime.h"
#include "CompilerHooks.h"
#include "HeapIterationScope.h"
#include "JSGlobalLexicalEnvironment.h"
#include "JSModuleEnvironment.h"
#include "JSModuleRecord.h"
#include "JSModuleLoader.h"


#include "ArrayConstructor.h"
#include "ArrayPrototype.h"
#include "MapPrototype.h"
#include "ModuleProgramCodeBlock.h"
#include "ModuleProgramExecutable.h"
#include "SamplingProfiler.h"
#include "SetPrototype.h"
#include "StringPrototype.h"

#include "DeferTermination.h"
#include "FrameTracers.h"
#include "FunctionCodeBlock.h"
#include "JSTemplateObjectDescriptor.h"
#include "JSWebAssemblyInstance.h"
#include "ParserError.h"

#if ENABLE(AOT)


#include "AOTImage.h"
#include "AOTInlineCaches.h"
#include "AOTOperationHelpers.h"
#include "AOTOperations.h"
#include "AOTProgram.h"
#include "AOTThunks.h"
#include "AOTTypedCompactTable.h"
#include "AOTTypedObject.h"
#include "CCallHelpers.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "DFGOperations.h"
#include "HeapAnalyzer.h"
#include "JITExceptions.h"
#include "JITOperations.h"
#include "JITThunks.h"
#include "JSAsyncFunction.h"
#include "JSAsyncGeneratorFunction.h"
#include "JSCInlines.h"
#include "JSGeneratorFunction.h"
#include "JSPromise.h"
#include "JSSentinel.h"
#include "LLIntData.h"
#include "LLIntEntrypoint.h"
#include "LLIntSlowPaths.h"
#include "LLIntThunks.h"
#include "LinkBuffer.h"
#include "MarkedBlockCage.h"
#include "MathObject.h"
#include "MicrotaskQueue.h"
#include "MicrotaskQueueInlines.h"
#include "ObjectConstructorInlines.h"
#include "StackFrame.h"
#include "ThunkGenerators.h"
#include "VMTrapsInlines.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include <mutex>
#include <wtf/FilePrintStream.h>
#include <wtf/HexNumber.h>
#include <wtf/ProcessID.h>
#include <wtf/StackTrace.h>
#include <wtf/StringPrintStream.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

#if OS(LINUX)
#include <errno.h>
#include <sys/mman.h>
#endif

#if OS(DARWIN)
#include <execinfo.h>
#endif

namespace JSC {

const ClassInfo JSFunctionWithCaptures::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSFunctionWithCaptures) };

JSFunctionWithCaptures* JSFunctionWithCaptures::create(VM& vm, JSScope* scope, Structure* structure, uintptr_t executableOrFunctionWord, std::span<const EncodedJSValue> values)
{
    static_assert(sizeof(JSFunctionWithCaptures) <= static_cast<size_t>(offsetOfCaptures()));
    auto* function = new (NotNull, allocateCell<JSFunctionWithCaptures>(vm, allocationSize(values.size()))) JSFunctionWithCaptures(vm, std::bit_cast<FunctionExecutable*>(executableOrFunctionWord), scope, structure, values.size());
    for (unsigned i = 0; i < values.size(); ++i)
        function->captures()[i].setWithoutWriteBarrier(JSValue::decode(values[i]));
    function->finishCreation(vm);
    return function;
}

template<typename Visitor>
void JSFunctionWithCaptures::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<JSFunctionWithCaptures>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.appendValuesHidden(thisObject->captures(), thisObject->count());
}

DEFINE_VISIT_CHILDREN(JSFunctionWithCaptures);

void JSFunctionWithCaptures::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    auto* thisObject = uncheckedDowncast<JSFunctionWithCaptures>(cell);
    Base::analyzeHeap(cell, analyzer);
    for (unsigned i = 0; i < thisObject->count(); ++i) {
        JSValue value = thisObject->captures()[i].get();
        if (value && value.isCell())
            analyzer.analyzeIndexEdge(thisObject, value.asCell(), i);
    }
}

} // namespace JSC

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RuntimeTable);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(Data);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(VirtualCallInfo);

void* stubAddress(Stub stub)
{
    const void* inImage = Image::stubAddress(stub);
    RELEASE_ASSERT_WITH_MESSAGE(inImage, "No AOT image with code is registered.");
    return const_cast<void*>(inImage);
}

void* catchThunk()
{
    return tagCodePtr<ExceptionHandlerPtrTag>(stubAddress(Stub::Catch));
}

void* rejectInFrameThunk()
{
    return tagCodePtr<ExceptionHandlerPtrTag>(stubAddress(Stub::RejectInFrame));
}


template<typename Wanted, typename Function> struct TakesFirst : std::false_type { };
template<typename Wanted, typename Result, typename First, typename... Rest> struct TakesFirst<Wanted, Result(First, Rest...)> : std::is_same<Wanted, First> { };
template<typename Wanted, typename Result, typename First, typename... Rest> struct TakesFirst<Wanted, Result(First, Rest...) noexcept> : std::is_same<Wanted, First> { };
#if OS(WINDOWS) && CPU(X86_64)
template<typename Wanted, typename Result, typename First, typename... Rest> struct TakesFirst<Wanted, Result SYSV_ABI(First, Rest...)> : std::is_same<Wanted, First> { };
template<typename Wanted, typename Result, typename First, typename... Rest> struct TakesFirst<Wanted, Result SYSV_ABI(First, Rest...) noexcept> : std::is_same<Wanted, First> { };
#endif
static_assert(TakesFirst<Instance*, std::remove_pointer_t<decltype(&operationAOTValueAdd)>>::value);
static_assert(TakesFirst<JSCell*, std::remove_pointer_t<decltype(&operationAOTGetPlainly)>>::value);

template<typename Wanted> static bool takesFirst(Entry entry)
{
    using namespace DFG;
    switch (entry) {
#define AOT_CASE_OF_OPERATION(name) \
    case Entry::name: \
        return TakesFirst<Wanted, std::remove_pointer_t<decltype(&name)>>::value;
    FOR_EACH_AOT_OPERATION(AOT_CASE_OF_OPERATION)
#undef AOT_CASE_OF_OPERATION
    default:
        return false;
    }
}

bool takesInstance(Entry entry) { return takesFirst<Instance*>(entry); }

#if HAVE_MUST_TAIL_CALL
template<Entry, auto operation> struct CountedOperation {
    static constexpr auto call = operation;
};

template<Entry entry, typename Result, typename... Rest, Result (*operation)(JSGlobalObject*, Rest...)>
struct CountedOperation<entry, operation> {
    static Result JIT_OPERATION_ATTRIBUTES call(JSGlobalObject* globalObject, Rest... rest)
    {
        runtimeTable(globalObject->vm()).countOperation(nameOf(entry).characters());
        MUST_TAIL_CALL return operation(globalObject, rest...);
    }
};

template<Entry entry, typename Result, typename... Rest, Result (*operation)(VM*, Rest...)>
struct CountedOperation<entry, operation> {
    static Result JIT_OPERATION_ATTRIBUTES call(VM* vm, Rest... rest)
    {
        runtimeTable(*vm).countOperation(nameOf(entry).characters());
        MUST_TAIL_CALL return operation(vm, rest...);
    }
};
#endif

RuntimeTable::RuntimeTable(VM& vm)
{
    using namespace DFG;
#define AOT_FILL_OPERATION(name) \
    m_entries[static_cast<unsigned>(Entry::name)] = tagCFunctionPtr<void*, OperationPtrTag>(name);
    FOR_EACH_AOT_OPERATION(AOT_FILL_OPERATION)
#undef AOT_FILL_OPERATION
#if HAVE_MUST_TAIL_CALL
    if (Options::useAOTOperationCounters()) [[unlikely]] {
#define AOT_COUNT_OPERATION(name) \
        m_entries[static_cast<unsigned>(Entry::name)] = tagCFunctionPtr<void*, OperationPtrTag>(CountedOperation<Entry::name, &name>::call);
        FOR_EACH_AOT_OPERATION_OF_THE_OTHER_TIERS(AOT_COUNT_OPERATION)
#undef AOT_COUNT_OPERATION
    }
#endif

    auto set = [&](Entry entry, void* pointer) {
        m_entries[static_cast<unsigned>(entry)] = pointer;
    };
    set(Entry::HandleException, tagCodePtr<JITThunkPtrTag>(stubAddress(Stub::HandleException)));
    set(Entry::VirtualCall, tagCodePtr<JITThunkPtrTag>(stubAddress(Stub::VirtualCall)));
    set(Entry::VirtualConstruct, tagCodePtr<JITThunkPtrTag>(stubAddress(Stub::VirtualConstruct)));
    set(Entry::VirtualTailCall, tagCodePtr<JITThunkPtrTag>(stubAddress(Stub::VirtualTailCall)));
    set(Entry::LookupExceptionHandler, tagCFunctionPtr<void*, OperationPtrTag>(operationAOTLookupExceptionHandler));
    set(Entry::ThrowStackOverflowError, tagCFunctionPtr<void*, OperationPtrTag>(operationAOTThrowStackOverflowError));
    set(Entry::NativeCallTrampoline, LLInt::getCodePtr<JSEntryPtrTag>(llint_native_call_trampoline).taggedPtr());
    set(Entry::InternalFunctionCallTrampoline, LLInt::getCodePtr<JSEntryPtrTag>(llint_internal_function_call_trampoline).taggedPtr());
    set(Entry::InternalFunctionConstructTrampoline, LLInt::getCodePtr<JSEntryPtrTag>(llint_internal_function_construct_trampoline).taggedPtr());

    auto addCallLinkInfo = [&](Entry entry, CallLinkInfo::CallType type) {
        auto info = makeUnique<VirtualCallInfo>();
        static_assert(!OBJECT_OFFSETOF(VirtualCallInfo, callLinkInfo));
        info->callLinkInfo.initialize(vm, nullptr, type, CodeOrigin { });
        info->callLinkInfo.setVirtualCall(vm);
        info->findTarget = tagCFunctionPtr<void*, OperationPtrTag>(findCallTarget);
        info->lookupExceptionHandler = tagCFunctionPtr<void*, OperationPtrTag>(operationLookupExceptionHandler);
        set(entry, &info->callLinkInfo);
        m_callLinkInfos.append(WTF::move(info));
    };
    addCallLinkInfo(Entry::CallLinkInfoForCall, CallLinkInfo::Call);
    addCallLinkInfo(Entry::CallLinkInfoForConstruct, CallLinkInfo::Construct);
    addCallLinkInfo(Entry::CallLinkInfoForTailCall, CallLinkInfo::TailCall);
    set(Entry::StructureIDBase, std::bit_cast<void*>(structureIDBase()));
    auto setHostFunction = [&](Entry entry, NativeFunction::Ptr function) {
        set(entry, TaggedNativeFunction(function).taggedPtr());
    };
    setHostFunction(Entry::HostMathSqrt, mathProtoFuncSqrt);
    setHostFunction(Entry::HostMathAbs, mathProtoFuncAbs);
    setHostFunction(Entry::HostMathFloor, mathProtoFuncFloor);
    setHostFunction(Entry::HostMathCeil, mathProtoFuncCeil);
    setHostFunction(Entry::HostMathTrunc, mathProtoFuncTrunc);
    setHostFunction(Entry::HostMathFround, mathProtoFuncFround);
    setHostFunction(Entry::HostMathMin, mathProtoFuncMin);
    setHostFunction(Entry::HostMathMax, mathProtoFuncMax);
    setHostFunction(Entry::HostMathIMul, mathProtoFuncIMul);
    setHostFunction(Entry::HostStringCharCodeAt, stringProtoFuncCharCodeAt);
    setHostFunction(Entry::HostArrayPush, arrayProtoFuncPush);
    setHostFunction(Entry::HostStringCodePointAt, stringProtoFuncCodePointAt);
    setHostFunction(Entry::HostStringCharAt, stringProtoFuncCharAt);
    setHostFunction(Entry::HostStringSlice, stringProtoFuncSlice);
    setHostFunction(Entry::HostArrayPop, arrayProtoFuncPop);
    setHostFunction(Entry::HostArrayIsArray, arrayConstructorIsArray);
    setHostFunction(Entry::HostMapGet, mapProtoFuncGet);
    setHostFunction(Entry::HostMapHas, mapProtoFuncHas);
    setHostFunction(Entry::HostMapSet, mapProtoFuncSet);
    setHostFunction(Entry::HostSetHas, setProtoFuncHas);
    setHostFunction(Entry::HostSetAdd, setProtoFuncAdd);

    installOperationFrontEnds(vm, m_entries);

    vm.getBoundFunction(true, SourceTaintedOrigin::Untainted)->setCallEntrypoint(CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::CallBoundFunction))));
}

RuntimeTable::~RuntimeTable()
{
    writeOperationCounts();
}

void RuntimeTable::countOperationBySlotState(const char* name, const Slot* slot)
{
    if (!slot) {
        countOperation(name, "no-slot");
        return;
    }
    bool isAbandoned = (slot->offset & Slot::attemptsMask) == Slot::attemptsMask;
    const char* state = "untried-slot";
    if (SharedData::contains(slot))
        state = "shared-slot";
    else if (slot->isPolymorphic())
        state = "polymorphic-slot";
    else if (slot->structureID)
        state = isAbandoned ? "abandoned-filled-slot" : "filled-slot";
    else if (isAbandoned)
        state = "abandoned-slot";
    else if (slot->offset & Slot::attemptsMask)
        state = "retried-slot";
    countOperation(name, state);
}

void RuntimeTable::countOperationAtSite(const char* name, uint32_t function, unsigned bytecodeOffset, unsigned line, unsigned column)
{
    UTF8CString& detail = m_detailsOfSites.add((static_cast<uint64_t>(function) + 1) << 32 | bytecodeOffset, UTF8CString()).iterator->value;
    if (detail.isNull())
        detail = toUTF8CString("at:", line, ":", column, ":bc", bytecodeOffset);
    countOperation(name, byteCast<char>(detail.data()));
}

void RuntimeTable::countAllocatedBytes(const char* kind, const Subspace* subspace, size_t cellSize, const ClassInfo* classOfOwner, size_t bytes)
{
    const char* name = "Heap::didAllocate";
    const void* source = subspace ? static_cast<const void*>(subspace) : static_cast<const void*>(classOfOwner);
    UTF8CString& detail = m_detailsOfAllocatedBytes.add(std::tuple { kind, source, cellSize }, UTF8CString()).iterator->value;
    if (detail.isNull()) {
        if (subspace && cellSize)
            detail = toUTF8CString(kind, ":", subspace->name(), ":", cellSize);
        else if (subspace)
            detail = toUTF8CString(kind, ":", subspace->name());
        else if (classOfOwner)
            detail = toUTF8CString(kind, ":", classOfOwner->className);
        else
            detail = toUTF8CString(kind);
    }
    countOperation(name, nullptr, bytes);
    countOperation(name, byteCast<char>(detail.data()), bytes);

    void* frames[20];
    int numberOfFrames = std::size(frames);
    WTFGetBacktrace(frames, &numberOfFrames);
    StringPrintStream stack;
    stack.print(byteCast<char>(detail.data()), "\t");
    for (int i = 0; i < numberOfFrames; ++i)
        stack.print(i ? " " : "", RawPointer(frames[i]));
    auto& entry = m_allocatedBytesByStack.add(stack.toUTF8CString(), std::pair<uint64_t, uint64_t> { }).iterator->value;
    entry.first += bytes;
    entry.second++;
}

void RuntimeTable::clearLiveBytes()
{
    for (auto& [operation, count] : m_operationCounts) {
        if (!strcmp(operation.first, "Heap::live"))
            count = 0;
    }
}

void RuntimeTable::setLiveBytes(const Subspace& subspace, size_t cellSize, size_t bytes)
{
    UTF8CString& detail = m_detailsOfAllocatedBytes.add(std::tuple { "live", static_cast<const void*>(&subspace), cellSize }, UTF8CString()).iterator->value;
    if (detail.isNull())
        detail = cellSize ? toUTF8CString("block:", subspace.name(), ":", cellSize) : toUTF8CString("precise:", subspace.name());
    countOperation("Heap::live", byteCast<char>(detail.data()), bytes);
}

uint64_t RuntimeTable::operationCount(StringView nameAndDetail) const
{
    uint64_t result = 0;
    for (auto& [operation, count] : m_operationCounts) {
        StringView name = StringView::fromLatin1(operation.first);
        if (!operation.second) {
            if (name == nameAndDetail)
                result += count;
            continue;
        }
        StringView detail = StringView::fromLatin1(operation.second);
        if (nameAndDetail.length() == name.length() + 1 + detail.length() && nameAndDetail.startsWith(name) && nameAndDetail[name.length()] == ':' && nameAndDetail.endsWith(detail))
            result += count;
    }
    return result;
}

void RuntimeTable::dumpOperationCounts(PrintStream& out) const
{
    Vector<std::tuple<const char*, const char*, uint64_t>> sorted;
    for (auto& [operation, count] : m_operationCounts)
        sorted.append({ operation.first, operation.second ? operation.second : "", count });
    std::ranges::sort(sorted, [](auto& a, auto& b) {
        if (int order = strcmp(std::get<0>(a), std::get<0>(b)))
            return order < 0;
        return strcmp(std::get<1>(a), std::get<1>(b)) < 0;
    });
    for (auto& [operation, detail, count] : sorted)
        out.println(count, "\t", operation, *detail ? ":" : "", detail);
    UncheckedKeyHashMap<const char*, uint64_t> distinctPairs;
    for (auto& pair : m_filledPairs)
        distinctPairs.add(std::get<0>(pair), 0).iterator->value++;
    for (auto& [kind, count] : distinctPairs)
        out.println(count, "\t", kind, ":distinct-pairs");
}

void RuntimeTable::forgetFilledPairsOfDeadStructures(VM& vm)
{
    auto isDead = [&](uint32_t structureID) { return structureID && !vm.heap.isMarked(std::bit_cast<StructureID>(structureID).decode()); };
    m_filledPairs.removeIf([&](auto& pair) {
        return isDead(static_cast<uint32_t>(std::get<2>(pair))) || isDead(static_cast<uint32_t>(static_cast<uint64_t>(std::get<2>(pair)) >> 32));
    });
}

void RuntimeTable::writeOperationCounts() const
{
    const char* path = byteCast<char>(Options::aotTypeCoverageCountsPath());
    if (!Options::useAOTOperationCounters() || !path)
        return;
    String name = makeStringByReplacingAll(String::fromUTF8(path), "%p"_s, String::number(getCurrentProcessID()));
    if (auto file = FilePrintStream::open(makeString(name, ".operations"_s).utf8(), "w"_s))
        dumpOperationCounts(*file);
    if (auto file = FilePrintStream::open(makeString(name, ".allocations"_s).utf8(), "w"_s)) {
        file->println("ANCHOR\t", RawPointer(reinterpret_cast<void*>(&WTFGetBacktrace)));
        for (auto& [stack, entry] : m_allocatedBytesByStack)
            file->println(entry.first, "\t", entry.second, "\t", byteCast<char>(stack.data()));
    }
    if (m_guests.isEmpty())
        return;
    if (auto file = FilePrintStream::open(makeString(name, ".guests"_s).utf8(), "w"_s)) {
        auto onOneLine = [](StringView text) {
            String result = text.toString();
            for (auto [character, written] : { std::pair { "\\"_s, "\\\\"_s }, std::pair { "\t"_s, "\\t"_s }, std::pair { "\n"_s, "\\n"_s }, std::pair { "\r"_s, "\\r"_s } })
                result = makeStringByReplacingAll(result, character, written);
            return result;
        };
        for (auto& [guest, entry] : m_guests) {
            size_t endOfKind = guest.find('\t');
            file->println(entry.first, "\t", StringView(guest).left(endOfKind), "\t", onOneLine(StringView(guest).substring(endOfKind + 1)), "\t", onOneLine(entry.second));
        }
    }
}

void RuntimeTable::noteGuest(const char* kind, StringView group, String&& sample)
{
    countOperation("guest", kind);
    auto& entry = m_guests.add(makeString(StringView::fromLatin1(kind), '\t', group), std::pair<uint64_t, String> { }).iterator->value;
    if (!entry.first++)
        entry.second = WTF::move(sample);
}

uint64_t operationCount(VM& vm, StringView name)
{
    return runtimeTable(vm).operationCount(name);
}

void countAllocatedBytes(VM& vm, const char* kind, const Subspace* subspace, size_t cellSize, const JSCell* owner, size_t bytes)
{
    if (!bytes || !vm.m_aotRuntimeTable)
        return;
    vm.m_aotRuntimeTable->countAllocatedBytes(kind, subspace, cellSize, owner ? owner->classInfo() : nullptr, bytes);
    if (vm.m_aotRuntimeTable->shouldWriteOperationCountsAfterAllocating(bytes))
        vm.m_aotRuntimeTable->writeOperationCounts();
}

void writeOperationCounts(VM& vm)
{
    runtimeTable(vm).writeOperationCounts();
}

void countAwait(VM& vm, const char* where, JSValue operand)
{
    if (!vm.m_aotRuntimeTable)
        return;
    static constexpr const char* details[2][5] = {
        { "queue-not-empty:not-an-object", "queue-not-empty:pending", "queue-not-empty:fulfilled", "queue-not-empty:rejected", "queue-not-empty:other-object" },
        { "queue-empty:not-an-object", "queue-empty:pending", "queue-empty:fulfilled", "queue-empty:rejected", "queue-empty:other-object" },
    };
    unsigned kind = 0;
    if (auto* promise = dynamicDowncast<JSPromise>(operand))
        kind = 1 + static_cast<unsigned>(promise->status());
    else if (operand.isObject())
        kind = 4;
    runtimeTable(vm).countOperation(where, details[vm.defaultMicrotaskQueue().isEmpty()][kind]);
}

void countJobCall(VM& vm)
{
    if (!vm.m_aotRuntimeTable)
        return;
    runtimeTable(vm).countOperation("job-call", "other");
}

void noteGuest(JSGlobalObject* globalObject, const char* kind, StringView group, StringView text)
{
    VM& vm = globalObject->vm();
    if (!vm.m_aotRuntimeTable)
        return;
    String sample = makeStringByReplacingAll(makeString(text.length(), " \""_s, text.left(80), '"'), "\n"_s, " "_s);
    if (!group.isNull()) {
        vm.m_aotRuntimeTable->noteGuest(kind, group, WTF::move(sample));
        return;
    }
    Vector<StackFrame> frames;
    vm.interpreter.getStackTrace(globalObject, frames, 0, 4);
    String by = makeStringByReplacingAll(Interpreter::stackTraceAsString(vm, frames), "\n"_s, " <- "_s);
    vm.m_aotRuntimeTable->noteGuest(kind, by, WTF::move(sample));
}

RuntimeTable& runtimeTable(VM& vm)
{
    if (!vm.m_aotRuntimeTable)
        vm.m_aotRuntimeTable = makeUnique<RuntimeTable>(vm);
    return *vm.m_aotRuntimeTable;
}

struct Instance::Collections {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Collections);
    Vector<Data*> all;
    Vector<Data*> filledSinceLastCollection;
    Vector<JSCell*> cellsAddedSinceLastCollection;
    Vector<std::pair<Structure*, Structure*>> propertyRunTargetsAddedSinceLastCollection;
    bool hasVisitedEverythingInThisCollection { false };
    bool hasAddedWeakEntriesSinceLastCollection { false };
    AssumptionWatchpoint arraysLackIsConcatSpreadable;
    AssumptionWatchpoint arraysLackInheritedElements;
    std::unique_ptr<std::array<BufferAccessorWatchpoint, Instance::numberOfBufferAccessors>> bufferAccessorWatchpoints;
    Vector<Slot*> transitions;
    Vector<Slot*> transitionsSinceLastCollection;
    Vector<PolymorphicSlots*> allSiteSlots;
    // Let go by whoever notes the first place behind the collection that has emptied the table: the collector lets go of no name.
    Vector<RefPtr<UniquedStringImpl>> namesOfOwnPropertyPlaces;
    UncheckedKeyHashMap<uint64_t, Structure*> shapes;
    Vector<uint32_t> filledRows;
    UncheckedKeyHashMap<uint32_t, Structure*> emptyStructuresWithoutPrototype;
    UncheckedKeyHashMap<Structure*, Structure*> copyStructures;
    UncheckedKeyHashMap<std::pair<Structure*, uint64_t>, Instance::CopyForMore> copiesForMore;
    UncheckedKeyHashMap<std::tuple<Structure*, UniquedStringImpl*, uintptr_t>, Instance::CopyExcluding> copiesExcluding;
    Vector<RefPtr<UniquedStringImpl>> namesOfCopiesExcludingThatWent;
    std::pair<Structure*, uint64_t> keyOfLastCopyForMore { nullptr, 0 };
    const Instance::CopyForMore* lastCopyForMore { nullptr };
    UncheckedKeyHashMap<std::tuple<const void*, Structure*, Structure*, Structure*>, std::unique_ptr<Instance::SpreadLiteralPlan>> spreadLiteralPlans;
    const Instance::SpreadLiteralPlan* lastSpreadLiteralPlan { nullptr };
    UncheckedKeyHashMap<std::pair<Structure*, const uint32_t*>, Instance::PropertyRunTarget> propertyRunTargets;
    UncheckedKeyHashMap<String, std::unique_ptr<uint32_t>> canonicalPropertyRuns;
    UncheckedKeyHashMap<std::tuple<Structure*, Structure*, const IdentifierSet*>, Instance::CopiedProperties> copiedProperties;
    UncheckedKeyHashMap<std::tuple<uint32_t, JSObject*, JSCell*>, Instance::StructuresOfClass> structuresOfClasses;
    UncheckedKeyHashMap<Structure*, JSFunction*> constructorsByFirstStructure;
    void setConstructorByFirstStructure(Structure* first, JSFunction* constructor)
    {
        auto result = constructorsByFirstStructure.add(first, constructor);
        if (!result.isNewEntry && result.iterator->value == constructor)
            return;
        result.iterator->value = constructor;
        hasAddedWeakEntriesSinceLastCollection = true;
    }
    UncheckedKeyHashMap<JSFunction*, unsigned> learnedInlineCapacities;
    UncheckedKeyHashMap<uint32_t, uint8_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> learnedRoomOfRows;
    JSModuleLoader* loader { nullptr };
    UncheckedKeyHashMap<uint32_t, ScriptExecutable*> topLevelExecutables;
    UncheckedKeyHashMap<uint32_t, JSArray*, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> templateObjects;
    JSCell* token { nullptr };
    bool loaderWasCleared { false };
    size_t environmentsSize { 0 };
    size_t sizeFromInstance { 0 };
    size_t numberOfFunctions { 0 };
    size_t dataStart { 0 };
    Vector<uint16_t> freeOwnData;
    unsigned numberOfOwnDataEverUsed { 0 };
    size_t usedDataEnd { 0 };
    size_t dataEnd { 0 };
    uint64_t* startsOfData { nullptr };
    size_t sizeOfStartsOfData { 0 };
    Vector<std::pair<size_t, size_t>> freeDataList;
    struct SlotNoLongerWatched {
        Data* data;
        Slot* slot;
        StructureID structureID;
        uint32_t offset;
        void* pointer;
        bool wasSeenAtCollection;
        std::unique_ptr<StackTrace> stack;
    };
    Vector<SlotNoLongerWatched> slotsNoLongerWatched;
};

static_assert(Instance::offsetOfVM() == 16, "JSWebAssemblyInstance::offsetOfVM()");
static_assert(!Instance::offsetOfSharedData());
static_assert(!(Instance::offsetOfOwnData() % sizeof(Data*)) && Instance::offsetOfOwnData() / sizeof(Data*) + Instance::maxNumberOfOwnData <= std::numeric_limits<uint16_t>::max() + 1);
static_assert(Instance::stateOfLinkedWithoutData() + 1 == Instance::offsetOfOwnData() / sizeof(Data*));
static_assert(Instance::cacheMissLimitFor(FunctionInfo::maxEncodedSlots) < std::numeric_limits<uint16_t>::max());

static_assert(!(sizeof(Data) % sizeof(uint64_t)) && OBJECT_OFFSETOF(Data, slots) == sizeof(Data));
static constexpr auto s_sharedData = [] {
    std::array<uint64_t, SharedData::size / sizeof(uint64_t)> words { };
    static_assert(!OBJECT_OFFSETOF(Slot, structureID) && OBJECT_OFFSETOF(Slot, offset) == 4 && sizeof(Slot) == 16);
    for (size_t i = sizeof(Data) / sizeof(uint64_t); i < words.size(); i += 2)
        words[i] = static_cast<uint64_t>(Slot::attemptsMask) << 32;
    return words;
}();

Data* SharedData::get()
{
    return std::bit_cast<Data*>(s_sharedData.data());
}

JSCell* ownerOf(Instance* instance) { return instance->loader(); }
JSCell* tokenOf(Instance* instance) { return instance->collections->token; }

JSGlobalObject* globalObjectForInstanceToken(VM& vm, JSCell* token)
{
    for (Instance* instance : vm.m_aotInstances) {
        if (tokenOf(instance) == token)
            return instance->globalObject;
    }
    return nullptr;
}

void didClearLoaderOf(Instance* instance)
{
    if (instance->globalObject->aotInstance() != instance)
        instance->collections->loaderWasCleared = true;
}

bool Instance::loaderWasCleared() const { return collections->loaderWasCleared; }

void Instance::destroyUnneededInstances(VM& vm)
{
    while (!vm.m_aotInstancesToDestroy.isEmpty())
        destroy(vm.m_aotInstancesToDestroy.last());
}
JSModuleLoader* Instance::loader() const { return collections->loader; }

Instance& Instance::ensure(JSGlobalObject* globalObject)
{
    if (Instance* instance = globalObject->aotInstance())
        return *instance;
    Instance& instance = ensure(globalObject->moduleLoader());
    globalObject->setAOTInstance(&instance);
    if (instance.typed && !globalObject->hasImmutableIntrinsics())
        instance.typed->captureBuiltinsOfRealm();
    return instance;
}

Instance* Instance::of(JSFunction* function)
{
    Structure* structure = function->structure();
    if (Instance* instance = structure->aotInstance())
        return instance;
    return &ensure(structure->realm());
}

#if CPU(X86_64) && OS(LINUX)
static Lock s_loopPollPagesLock;

static Vector<std::pair<VM*, void*>>& loopPollPages()
{
    static NeverDestroyed<Vector<std::pair<VM*, void*>>> pages;
    return pages;
}

static void setLoopPollPageProtected(void* page, bool isProtected)
{
    RELEASE_ASSERT(!mprotect(page, WTF::pageSize(), isProtected ? PROT_NONE : PROT_READ | PROT_WRITE));
}

void setLoopPollPagesProtected(VM& vm, bool isProtected)
{
    Locker locker { s_loopPollPagesLock };
    for (auto& [owner, page] : loopPollPages()) {
        if (owner == &vm)
            setLoopPollPageProtected(page, isProtected);
    }
}
#else
void setLoopPollPagesProtected(VM&, bool)
{
}
#endif

Instance& Instance::ensure(JSModuleLoader* loader)
{
    if (Instance* instance = loader->aotInstance())
        return *instance;
#if ENABLE(WEBASSEMBLY)
    RELEASE_ASSERT(Instance::offsetOfVM() == JSWebAssemblyInstance::offsetOfVM());
#endif
    JSGlobalObject* globalObject = loader->moduleScope()->realm();
    VM& vm = globalObject->vm();
    RELEASE_ASSERT(vm.useImmutableIntrinsics);
    // The collector does not visit an Instance before it is registered, which is the last thing done here.
    DeferGCForAWhile deferGC(vm);
    auto dataStartFor = [](size_t numberOfStates) {
        return std::max<size_t>(roundUpToMultipleOf(WTF::pageSize(), sizeof(Instance) + numberOfStates * sizeof(uint16_t)), static_cast<size_t>(minStateWithData) << stateWithDataShift);
    };
    VMProgram* program = VMProgram::of(vm);
    RELEASE_ASSERT(program);
    destroyUnneededInstances(vm);
    size_t environmentsSize = roundUpToMultipleOf(WTF::pageSize(), Image::environmentsSize());
    if (Image* image = Image::withCode(); image && image->header().typedDataSize) {
        RELEASE_ASSERT(environmentsSize <= roomForEnvironments && !(granuleBelowInstance % WTF::pageSize()));
        environmentsSize = roomBelowInstance(image->at<Typed::Data>(image->header().typedDataOffset)->sizeOfStatics);
    }
    size_t numberOfFunctions = Image::numberOfFunctions();
    size_t numberOfStates = program->data().numberOfStates;
    size_t size = dataStartFor(numberOfStates) + roundUpToMultipleOf(WTF::pageSize(), Image::totalDataSize());
    Instance* instance = reinterpret_cast<Instance*>(static_cast<char*>(OSAllocator::reserveAndCommit(environmentsSize + size, OSAllocator::FastMallocPages)) + environmentsSize);
#if OS(LINUX)
    while (madvise(reinterpret_cast<char*>(instance) - environmentsSize, environmentsSize + size, MADV_NOHUGEPAGE) == -1 && errno == EAGAIN) { }
#endif
    instance->runtimeTable = AOT::runtimeTable(vm).entries();
    instance->globalObject = globalObject;
    instance->vm = &vm;
    instance->powersOfTen = AOT::powersOfTen.data();
    instance->collections = new Collections;
    instance->keyedReads = static_cast<KeyedRead*>(OSAllocator::reserveAndCommit(sizeOfKeyedTables, OSAllocator::FastMallocPages));
    instance->keyedAdditions = reinterpret_cast<KeyedAddition*>(instance->keyedReads + numberOfKeyedReads);
    instance->keyedHas = reinterpret_cast<Slot*>(instance->keyedAdditions + numberOfKeyedAdditions);
    static_assert(!(numberOfNamedFields * sizeof(NamedField) % (16 * KB)));
    instance->namedFieldReads = static_cast<NamedField*>(OSAllocator::reserveAndCommit(numberOfNamedFields * sizeof(NamedField), OSAllocator::FastMallocPages));
    instance->namedFieldWrites = static_cast<NamedField*>(OSAllocator::reserveAndCommit(numberOfNamedFields * sizeof(NamedField), OSAllocator::FastMallocPages));
    instance->collections->loader = loader;
    instance->collections->environmentsSize = environmentsSize;
    instance->collections->sizeFromInstance = size;
    instance->collections->numberOfFunctions = numberOfFunctions;
    instance->collections->dataStart = dataStartFor(numberOfStates) >> stateWithDataShift;
    instance->collections->usedDataEnd = instance->collections->dataStart;
    instance->collections->dataEnd = size >> stateWithDataShift;
    instance->collections->sizeOfStartsOfData = roundUpToMultipleOf(WTF::pageSize(), (instance->collections->dataEnd - instance->collections->dataStart) / 8 + sizeof(uint64_t));
    instance->collections->startsOfData = static_cast<uint64_t*>(OSAllocator::reserveAndCommit(instance->collections->sizeOfStartsOfData, OSAllocator::FastMallocPages));
    instance->program = program;
    instance->programData = &program->data();
    instance->infos = program->data().infos();
    instance->addressOfStates = instance->states;
    instance->functionMetadataOffsets = program->data().functionMetadataOffsets();
    instance->programIdentifiers = program->identifiers();
    instance->programIdentifierPlaces = program->identifierPlaces();
    instance->sharedData = SharedData::get();
    instance->sharedDataOfLinkedFunctions = instance->sharedData;
    if (Image* image = Image::withCode()) {
        instance->image = image->at<uint8_t>(0);
        instance->code = static_cast<const uint8_t*>(image->code());
        instance->codeGranules = image->at<uint32_t>(image->header().codeGranulesOffset);
        instance->subsequentFunctionStarts = image->at<uint32_t>(image->header().functionStartsOffset) + 1;
        for (uint32_t inAdapter : image->header().returnsIntoAdapters) {
            const uint8_t* address = instance->code + image->header().stubCopies[0] + inAdapter;
            if (address > stubAddress(Stub::EnterSavedFrame) && address <= stubAddress(Stub::LaySavedFrameOnStack))
                instance->returnAddressOfSavedFrames = address;
        }
        RELEASE_ASSERT(instance->returnAddressOfSavedFrames);
    }
    instance->structureIDBase = JSC::structureIDBase();
    instance->cellCageBase = MarkedBlockCage::base();
    instance->numberTag = JSValue::NumberTag;
    instance->notCellMask = JSValue::NotCellMask;
    instance->numberBoxBase = Typed::NumberBoxTable::base();
    instance->boxedNaN = JSValue::encode(jsNaN());
    for (int integer = Instance::leastIntegerKeptAsDouble; integer <= Instance::greatestIntegerKeptAsDouble; ++integer)
        instance->integersKeptAsDoubles[integer - Instance::leastIntegerKeptAsDouble] = integer;
    for (size_t i = 0; i < std::size(Instance::bitsOfOtherDoublesKept); ++i)
        instance->otherDoubles[i] = Instance::bitsOfOtherDoublesKept[i];
    instance->fieldIDAtDirectLocationBase = instance->structureIDBase + Structure::offsetOfFieldIDInSlot() - JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) * sizeof(uint16_t);
    {
        auto idOf = [](Structure* structure) { return structure->id().bits(); };
        auto headerOf = [](Structure* structure) { return static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits(); };
        auto receiverStructureID = [&](Receiver receiver) -> uint32_t& { return instance->receiverStructureIDs[static_cast<unsigned>(receiver)]; };
        receiverStructureID(Receiver::Map) = idOf(globalObject->mapStructure());
        receiverStructureID(Receiver::Set) = idOf(globalObject->setStructure());
        receiverStructureID(Receiver::WeakMap) = idOf(globalObject->weakMapStructure());
        receiverStructureID(Receiver::WeakSet) = idOf(globalObject->weakSetStructure());
        receiverStructureID(Receiver::RegExp) = idOf(globalObject->regExpStructure());
        receiverStructureID(Receiver::Date) = idOf(globalObject->dateStructure());
        instance->boundFunctionStructureID = idOf(globalObject->boundFunctionStructure());
        instance->hostFunctionStructureID = idOf(globalObject->hostFunctionStructure());
        NativeExecutable* boundFunctionExecutable = vm.getBoundFunction(true, SourceTaintedOrigin::Untainted);
        boundFunctionExecutable->setCallEntrypoint(CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::CallBoundFunction))));
        instance->boundFunctionExecutable = boundFunctionExecutable;
        instance->boundFunctionClassInfo = JSBoundFunction::info();
        for (IndexingType type : { ArrayWithUndecided, ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous, ArrayWithArrayStorage, CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithDouble, CopyOnWriteArrayWithContiguous })
            instance->originalArrayStructureIDs[(type & (IndexingShapeMask | CopyOnWrite)) >> Instance::arrayKindShift] = idOf(globalObject->originalArrayStructureForIndexingType(type));
        if (!globalObject->hasImmutableIntrinsics()) {
            zeroSpan(std::span { instance->receiverStructureIDs });
            zeroSpan(std::span { instance->originalArrayStructureIDs });
        }
        for (unsigned vectorLength = 1; vectorLength <= Instance::maxVectorLengthGrownByStub; ++vectorLength) {
            unsigned next = Butterfly::optimalContiguousVectorLength(static_cast<size_t>(0), static_cast<unsigned>(nextLength(vectorLength + 1)));
            instance->nextVectorLengths[vectorLength] = next <= UINT8_MAX && next & 1 ? next : 0;
        }
        if (!globalObject->isHavingABadTime()) {
            instance->newArrayWithInt32Header = headerOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithInt32));
            instance->newArrayWithDoubleHeader = headerOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithDouble));
            instance->newArrayWithContiguousHeader = headerOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous));
            instance->newCopyOnWriteArrayHeaders[0] = headerOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithInt32));
            instance->newCopyOnWriteArrayHeaders[1] = headerOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithDouble));
            instance->newCopyOnWriteArrayHeaders[2] = headerOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithContiguous));
        }
        instance->collections->arraysLackIsConcatSpreadable.install(globalObject->arrayIsConcatSpreadableWatchpointSet(), instance->arraysLackIsConcatSpreadable);
        instance->collections->arraysLackInheritedElements.install(globalObject->arrayPrototypeChainIsSaneWatchpointSet(), instance->arraysLackInheritedElements);
        instance->auxiliarySpace = &vm.auxiliarySpace();
        instance->cellSpace = &vm.cellSpace();
        instance->knownGrowthsOfCompactTables = Typed::CompactTable::knownGrowths();
        instance->arrayAllocator = subspaceFor<JSArray>(vm)->allocatorFor(sizeof(JSArray), AllocatorForMode::EnsureAllocator).localAllocator();
        instance->ropeStringAllocator = subspaceFor<JSRopeString>(vm)->allocatorFor(sizeof(JSRopeString), AllocatorForMode::EnsureAllocator).localAllocator();
        RELEASE_ASSERT(static_cast<LocalAllocator*>(instance->arrayAllocator)->cellSize() == sizeof(JSArray));
        RELEASE_ASSERT(static_cast<LocalAllocator*>(instance->ropeStringAllocator)->cellSize() == sizeof(JSRopeString));
        instance->singleCharacterStrings = vm.smallStrings.singleCharacterStrings();
        instance->emptyString = vm.smallStrings.emptyString();
        instance->sentinelString = vm.smallStrings.sentinelString();
        instance->stringHeader = headerOf(vm.stringStructure.get());
        instance->smallIntStrings = vm.numericStrings.smallIntCache();
        instance->intStrings = vm.numericStrings.intCache();
        instance->hashSecrets[0] = 0x2d358dccaa6c78a5ULL;
        instance->hashSecrets[1] = 0x8bb84b93962eacc9ULL;
        prepareInlineAllocation<JSPromise>(instance, InlineAllocation::Promise, globalObject->promiseStructure());
    }
    if (Image* image = Image::withCode()) {
        MonotonicTime before = MonotonicTime::now();
        unsigned count = 0;
        for (unsigned which = 0; which < numberOfLinkTimeConstants; ++which) {
            if (!(image->header().linkTimeConstantsUsed[which / 64] >> which % 64 & 1))
                continue;
            instance->linkTimeConstants[which] = JSValue::encode(globalObject->linkTimeConstant(static_cast<LinkTimeConstant>(which)));
            ++count;
        }
        if (Options::verboseAOTCompilation()) [[unlikely]]
            dataLogLn("AOT: ", count, " link-time constants made ready in ", (MonotonicTime::now() - before).microseconds(), " us");
    }
    memcpySpan(std::span { instance->intrinsics }, globalObject->immutableIntrinsics());
    for (unsigned number = 1; number < globalObject->immutableIntrinsics().size(); ++number) {
        const ImmutableIntrinsics::Entry& entry = ImmutableIntrinsics::shared()->at(number);
        if (entry.holder != ImmutableIntrinsics::globalObject)
            break;
        RELEASE_ASSERT(JSValue::encode(globalObject->getDirect(vm, Identifier::fromString(vm, entry.name))) == instance->intrinsics[number]);
    }
    instance->objectPrototype = globalObject->objectPrototype();
    if (JSValue call = globalObject->linkTimeConstant(LinkTimeConstant::callFunction); call.isCell())
        instance->functionPrototypeCall = call.asCell();
    instance->collections->token = Symbol::create(vm);
    if (Image* image = Image::withCode(); image && image->header().typedDataSize)
        instance->typed = new Typed::State(vm, *instance, *image->at<Typed::Data>(image->header().typedDataOffset));
    loader->setAOTInstance(instance);
    if (instance->typed)
        instance->typed->adoptClosuresOfRealm(vm);
    vm.traps().registerMirror(instance->stackMirror);
#if CPU(X86_64) && OS(LINUX)
    if (instance->typed) {
        Locker locker { s_loopPollPagesLock };
        void* page = reinterpret_cast<char*>(instance) + offsetOfLoopPollPage();
        loopPollPages().append({ &vm, page });
        if (instance->stackMirror.trapAwareSoftStackLimit() == reinterpret_cast<void*>(StackManager::StopRequestMarkerValue))
            setLoopPollPageProtected(page, true);
    }
#endif
    instance->barrierThreshold = vm.heap.barrierThreshold();
    instance->mutatorShouldBeFenced = vm.heap.mutatorShouldBeFenced();
    vm.m_aotInstances.append(instance);
    PlaceTable::of(vm).didCreate(instance);
    for (unsigned kind = 0; kind < PlaceTable::numberOfKinds; ++kind) {
        for (unsigned form = 0; form < std::size(instance->handlersOfReadsByName); ++form)
            instance->handlersOfReadsByName[form][kind] = stubAddress(handlerOfReadByName(form, static_cast<PlaceTable::Kind>(kind)));
        for (unsigned variant = 0; variant < std::size(instance->handlersOfStoresByName); ++variant)
            instance->handlersOfStoresByName[variant][kind] = stubAddress(handlerOfStoreByName(variant, static_cast<PlaceTable::Kind>(kind)));
    }
    microtaskQueueingDidChange(vm);
    if (instance->typed && !globalObject->hasImmutableIntrinsics() && globalObject->aotInstance())
        instance->typed->captureBuiltinsOfRealm();
    return *instance;
}

void microtaskQueueingDidChange(VM& vm)
{
    for (Instance* instance : vm.m_aotInstances) {
        instance->microtaskQueue = &instance->globalObject->microtaskQueue();
        instance->awaitMustSuspend = !instance->globalObject->canFastQueueMicrotask() || vm.hostObservesMicrotaskBoundaries();
    }
}

ScriptExecutable* Instance::topLevelExecutableOf(uint32_t moduleID)
{
    return collections->topLevelExecutables.get(moduleID);
}

JSValue Instance::didRunModuleCode(AbstractModuleRecord* record, ModuleProgramExecutable* executable, JSValue yielded)
{
    if (!typed || !typed->data.isEntryPoint(executable->programModule()->entryOffset))
        return yielded;
    if (JSObject* import = yielded.getObject())
        typed->importsOfEntryPoints.append(std::pair { record, import });
    return jsUndefined();
}

JSObject* Instance::importOfCompiledEntryPoint(AbstractModuleRecord* record)
{
    for (Instance* instance : record->vm().m_aotInstances) {
        if (!instance->typed)
            continue;
        for (auto& [entryPoint, import] : instance->typed->importsOfEntryPoints) {
            if (entryPoint == record)
                return import;
        }
    }
    return nullptr;
}

uint64_t Instance::prepareModuleCode(ModuleProgramExecutable* executable, JSScope* scope)
{
    VM& vm = *this->vm;
    auto throwScope = DECLARE_THROW_SCOPE(vm);
    uint32_t index = executable->programModule()->functionIndex;
    if (typed && !globalObject->hasImmutableIntrinsics()) [[unlikely]] {
        throwSyntaxError(globalObject, throwScope, withAOTErrorNote(makeString("The module "_s, executable->source().provider()->sourceURL(), " was compiled ahead of time and cannot be loaded in a realm that was created at run time (ShadowRealm, node:vm)"_s)));
        return 0;
    }
    if (typed)
        typed->ensureStaticsOfProgramAreFilled();
    if (typed && !typed->moduleRecord && scope->type() == ModuleEnvironmentType && typed->data.isEntryPoint(executable->programModule()->entryOffset)) {
        // An object of the program says what it is laid out as, not whose it is, and compiled code takes the variables of the modules from the Instance it runs in.
        for (Instance* other : vm.m_aotInstances) {
            if (other == this || !other->typed || !other->typed->moduleRecord || (typed->data.mayRunInSeveralInstances && other->globalObject == globalObject))
                continue;
            throwSyntaxError(globalObject, throwScope, withAOTErrorNote(makeString("The module "_s, executable->source().provider()->sourceURL(), " was compiled ahead of time and has been loaded already: another realm or loader cannot load it again"_s)));
            return 0;
        }
        JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
        for (JSScope* overlay = loader()->moduleScope(); overlay != lexical; overlay = overlay->next()) {
            Vector<Identifier> names;
            {
                SymbolTable* table = uncheckedDowncast<JSLexicalEnvironment>(overlay)->symbolTable();
                ConcurrentJSLocker locker(table->m_lock);
                for (auto entry = table->begin(locker); entry != table->end(locker); ++entry) {
                    if (!entry->key->isSymbol())
                        names.append(Identifier::fromUid(vm, entry->key.get()));
                }
            }
            for (auto& name : names) {
                bool shadowsGlobal = lexical->symbolTable()->contains(name.impl()) || globalObject->hasProperty(globalObject, name);
                RETURN_IF_EXCEPTION(throwScope, 0);
                if (!shadowsGlobal)
                    continue;
                throwTypeError(globalObject, throwScope, withAOTErrorNote(makeString("The `globals` of a Bun.ModuleGraph cannot replace the global \""_s, name.string(), "\" for a module that was compiled ahead of time"_s)));
                return 0;
            }
        }
        typed->moduleRecord = uncheckedDowncast<JSModuleEnvironment>(scope)->moduleRecord();
        typed->importMeta = nullptr;
    }
    if (Data* data = dataIfExists(index); data && data->executable() == executable)
        return data->code()->entry();
    DeferGCForAWhile deferGC(vm);
    ImageCode found = findInImage(executable, scope);
    if (!found) {
        throwSyntaxError(globalObject, throwScope, withAOTErrorNote(makeString("The module "_s, executable->source().provider()->sourceURL(), " was compiled ahead of time and cannot be run the way it has been loaded"_s)));
        return 0;
    }
    Ref<JITCode> code = jitCodeForImageFunction(found, CodeSpecializationKind::CodeForCall);
    code->setInstance(*this);
    return Data::create(*this, executable, nullptr, code.get(), nullptr)->code()->entry();
}

void Instance::didFinishModuleEvaluation(ModuleProgramExecutable* executable)
{
    if (vm->heap.collectionScope())
        return;
    Data* data = dataIfExists(executable->programModule()->functionIndex);
    if (!data || data->executable() != executable)
        return;
#if ENABLE(SAMPLING_PROFILER)
    if (SamplingProfiler* profiler = vm->samplingProfiler()) [[unlikely]] {
        DeferGCForAWhile deferGC(*vm);
        Locker locker { profiler->getLock() };
        HeapIterationScope heapIterationScope(vm->heap);
        profiler->processUnverifiedStackTraces();
    }
#endif
    if (CodeBlock* codeBlock = data->codeBlock())
        codeBlock->releaseAOTData();
    else
        Data::destroy(data);
}

CodePtr<JSEntryPtrTag> moduleCodeEntrypoint()
{
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::EnterModule)));
}

CodePtr<JSEntryPtrTag> closureEntrypoint()
{
    // The image stays where it is. To ask for it takes a lock.
    static void* entrypoint = tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::EnterClosure));
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(entrypoint);
}

CodePtr<JSEntryPtrTag> slimClosureEntrypoint()
{
    static void* entrypoint = tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::EnterSlimClosure));
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(entrypoint);
}

void* taggedSlimClosureEntrypoint()
{
    return slimClosureEntrypoint().taggedPtr();
}

CodePtr<JSEntryPtrTag> resumeEntrypoint()
{
    static void* entrypoint = tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::EnterResume));
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(entrypoint);
}

CodePtr<JSEntryPtrTag> savedFrameEntrypoint()
{
    static void* entrypoint = tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::EnterSavedFrame));
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(entrypoint);
}

void Instance::setTopLevelExecutableOf(uint32_t moduleID, ScriptExecutable* executable)
{
    RELEASE_ASSERT(moduleID);
    collections->topLevelExecutables.set(moduleID, executable);
    noteCellAdded(executable);
}

void* Instance::allocateForData(size_t size)
{
    size_t units = roundUpToMultipleOf<1 << stateWithDataShift>(size) >> stateWithDataShift;
    auto at = [&](size_t where) {
        size_t bit = where - collections->dataStart;
        collections->startsOfData[bit / 64] |= 1ull << bit % 64;
        return std::bit_cast<void*>(std::bit_cast<uintptr_t>(this) + (where << stateWithDataShift));
    };
    auto& free = collections->freeDataList;
    for (unsigned i = 0; i < free.size(); ++i) {
        if (free[i].second < units)
            continue;
        size_t where = free[i].first;
        if (free[i].second == units)
            free.removeAt(i);
        else {
            free[i].first += units;
            free[i].second -= units;
        }
        return memset(at(where), 0, units << stateWithDataShift);
    }
    size_t where = collections->usedDataEnd;
    RELEASE_ASSERT(units <= collections->dataEnd - where);
    collections->usedDataEnd += units;
    return at(where);
}

Data* Instance::dataWithSlot(const Slot* slot, const void* returnAddress)
{
    size_t bit = ((std::bit_cast<uintptr_t>(slot) - std::bit_cast<uintptr_t>(this)) >> stateWithDataShift) - collections->dataStart;
    if (bit >= collections->usedDataEnd - collections->dataStart)
        return nullptr;
    size_t word = bit / 64;
    uint64_t starts = collections->startsOfData[word] & ~0ull >> (63 - bit % 64);
    while (!starts) {
        RELEASE_ASSERT(word);
        starts = collections->startsOfData[--word];
    }
    Data* data = std::bit_cast<Data*>(std::bit_cast<uintptr_t>(this) + ((collections->dataStart + word * 64 + 63 - clz(starts)) << stateWithDataShift));
    RELEASE_ASSERT(slot >= data->slots && slot < data->slots + data->numSlots);
    cachedAddressInfo(returnAddress) = { returnAddress, data->imageFunction()->index, CachedAddressInfo::siteNotResolved };
    return data;
}

void Instance::didStopWatching(Data* data, Slot* slot, bool wasWatched)
{
    validateSlotsNoLongerWatched(slot, false);
    if (wasWatched && slot->structureID)
        collections->slotsNoLongerWatched.append({ data, slot, slot->structureID, slot->offset & ~Slot::attemptsMask, slot->pointer, false, StackTrace::captureStackTrace(12, 2) });
}

void Instance::validateSlotsNoLongerWatched(const Slot* beingFilled, bool isAtCollection)
{
    collections->slotsNoLongerWatched.removeAllMatching([&](auto& noted) {
        if (noted.slot == beingFilled)
            return false;
        const Slot& slot = *noted.slot;
        if (slot.structureID != noted.structureID || (slot.offset & ~Slot::attemptsMask) != noted.offset || slot.pointer != noted.pointer)
            return true;
        if (noted.data->watchpoints && noted.data->watchpoints->contains(noted.slot))
            return true;
        if (isAtCollection && !std::exchange(noted.wasSeenAtCollection, true))
            return false;
        dataLogLn("AOT: slot ", RawPointer(noted.slot), " is filled as it was when its watchpoints were taken away: structure id ", noted.structureID.bits(), " offset and flags ", RawHex(noted.offset), " pointer ", RawPointer(noted.pointer), ". They were taken away at:");
        dataLogLn(StackTracePrinter { *noted.stack, "    " });
        CRASH();
    });
}

void Instance::forgetSlotsNoLongerWatched(Data* data)
{
    collections->slotsNoLongerWatched.removeAllMatching([&](auto& noted) {
        return noted.data == data;
    });
}

void Instance::freeDataMemory(void* pointer, size_t size)
{
    size_t units = roundUpToMultipleOf<1 << stateWithDataShift>(size) >> stateWithDataShift;
    size_t where = (std::bit_cast<uintptr_t>(pointer) - std::bit_cast<uintptr_t>(this)) >> stateWithDataShift;
    collections->startsOfData[(where - collections->dataStart) / 64] &= ~(1ull << (where - collections->dataStart) % 64);
    auto& free = collections->freeDataList;
    unsigned i = 0;
    while (i < free.size() && free[i].first < where)
        ++i;
    free.insert(i, std::pair<size_t, size_t> { where, units });
    if (i + 1 < free.size() && free[i].first + free[i].second == free[i + 1].first) {
        free[i].second += free[i + 1].second;
        free.removeAt(i + 1);
    }
    if (i && free[i - 1].first + free[i - 1].second == free[i].first) {
        free[i - 1].second += free[i].second;
        free.removeAt(i);
    }
}

void Instance::destroy(Instance* instance)
{
    delete instance->typed;
#if CPU(X86_64) && OS(LINUX)
    {
        Locker locker { s_loopPollPagesLock };
        loopPollPages().removeFirstMatching([&](auto& entry) {
            return entry.second == reinterpret_cast<char*>(instance) + offsetOfLoopPollPage();
        });
    }
#endif
    instance->vm->traps().unregisterMirror(instance->stackMirror);
    instance->vm->m_aotInstances.removeFirst(instance);
    instance->vm->m_aotInstancesToDestroy.removeFirst(instance);
    if (PlaceTable* table = instance->vm->m_aotPlaceTable)
        table->willDestroy(instance);
    while (!instance->collections->all.isEmpty())
        Data::destroy(instance->collections->all.last());
    size_t environmentsSize = instance->collections->environmentsSize;
    size_t size = instance->collections->sizeFromInstance;
    OSAllocator::decommitAndRelease(instance->collections->startsOfData, instance->collections->sizeOfStartsOfData);
    fastFree(instance->cachedReturnAddresses);
    delete instance->collections;
    fastFree(instance->customGetters);
    fastFree(instance->propertyRuns);
    OSAllocator::decommitAndRelease(instance->namedFieldReads, numberOfNamedFields * sizeof(NamedField));
    OSAllocator::decommitAndRelease(instance->namedFieldWrites, numberOfNamedFields * sizeof(NamedField));
    OSAllocator::decommitAndRelease(instance->keyedReads, sizeOfKeyedTables);
    OSAllocator::decommitAndRelease(reinterpret_cast<char*>(instance) - environmentsSize, environmentsSize + size);
}

JSCell** Instance::environmentSlot(ImageEnvironment environment) const
{
    if (!environment.distance || environment.distance > Image::environmentsSize())
        return nullptr;
    return reinterpret_cast<JSCell**>(const_cast<char*>(reinterpret_cast<const char*>(this)) - environment.distance);
}

SUPPRESS_ASAN void* returnAddressForFrame(const void* frame, const void* startingFrom)
{
    struct Record {
        const Record* previous;
        void* returnAddress;
    };
    auto* record = static_cast<const Record*>(startingFrom);
    while (record && record < frame && !(std::bit_cast<uintptr_t>(record) % sizeof(void*))) {
        const Record* previous = record->previous;
        if (previous == frame)
            return removeCodePtrTag(record->returnAddress);
        if (previous <= record)
            break;
        record = previous;
    }
    return nullptr;
}

namespace {
struct FrameRecord {
    const FrameRecord* previous;
    void* returnAddress;
};
}

SUPPRESS_ASAN std::optional<FrameAndPC> innermostFrame(void* machineFrame, void* machinePC, void* machineLinkRegister, void* topCallFrame, const StackBounds& stack)
{
    auto isValid = [&](const FrameRecord* record) {
        return stack.contains(const_cast<FrameRecord*>(record)) && !(std::bit_cast<uintptr_t>(record) % sizeof(void*));
    };
    auto* record = static_cast<const FrameRecord*>(machineFrame);
    if (machineLinkRegister && isValid(record) && removeCodePtrTag(record->returnAddress) != machineLinkRegister
        && kindOfAddress(machineLinkRegister) != ImageAddressInfo::NotInImage)
        return FrameAndPC { machineFrame, machineLinkRegister };
    void* pc = machinePC;
    while (isValid(record)) {
        if (record == topCallFrame)
            return FrameAndPC { topCallFrame, pc };
        pc = removeCodePtrTag(record->returnAddress);
        if (kindOfAddress(pc) != ImageAddressInfo::NotInImage)
            return FrameAndPC { const_cast<FrameRecord*>(record->previous), pc };
        if (record->previous <= record)
            break;
        record = record->previous;
    }
    return std::nullopt;
}

SUPPRESS_ASAN void* returnAddressOfTopWithoutFrame(void* machineFrame, void* machinePC, void* machineLinkRegister, void* machineStackPointer, const StackBounds& stack)
{
    auto isValid = [&](const void* address) {
        return stack.contains(const_cast<void*>(address)) && !(std::bit_cast<uintptr_t>(address) % sizeof(void*));
    };
    ImageAddressInfo top = classifyAddress(machinePC);
    auto* record = static_cast<const FrameRecord*>(machineFrame);
    if (top.kind == ImageAddressInfo::NotInImage || !isValid(record))
        return nullptr;
    void* leftByCall = machineLinkRegister;
    if (!isARM64())
        leftByCall = isValid(machineStackPointer) ? *static_cast<void* const*>(machineStackPointer) : nullptr;
    if (!leftByCall || leftByCall == removeCodePtrTag(record->returnAddress))
        return nullptr;
    ImageAddressInfo caller = classifyAddress(leftByCall);
    if (caller.kind == ImageAddressInfo::NotInImage)
        return nullptr;
    if (top.kind == ImageAddressInfo::Function)
        return caller.kind == ImageAddressInfo::Function && caller.index == top.index ? nullptr : leftByCall;
    return caller.kind == ImageAddressInfo::Function ? leftByCall : nullptr;
}

SUPPRESS_ASAN Instance* instanceForFrame(const void* frame)
{
    for (auto* record = static_cast<const FrameRecord*>(frame);; record = record->previous) {
        ImageAddressInfo::Kind kind = kindOfAddress(removeCodePtrTag(record->returnAddress));
        RELEASE_ASSERT(kind != ImageAddressInfo::NotInImage);
        if (kind == ImageAddressInfo::Adapter)
            return *reinterpret_cast<Instance* const*>(reinterpret_cast<const char*>(record->previous) + offsetOfInstanceInAdapter);
    }
}

SUPPRESS_ASAN Instance* tryInstanceForFrame(const void* frame)
{
    for (auto* record = static_cast<const FrameRecord*>(frame); record; record = record->previous) {
        ImageAddressInfo::Kind kind = kindOfAddress(removeCodePtrTag(record->returnAddress));
        if (kind == ImageAddressInfo::NotInImage)
            return nullptr;
        if (kind == ImageAddressInfo::Adapter)
            return *reinterpret_cast<Instance* const*>(reinterpret_cast<const char*>(record->previous) + offsetOfInstanceInAdapter);
    }
    return nullptr;
}

NEVER_INLINE bool topCallFrameIsAOTFrame(const void* frame)
{
    if (!hasCode())
        return false;
    void* returnAddress = returnAddressForFrame(frame, __builtin_frame_address(0));
    return returnAddress && kindOfAddress(returnAddress) != ImageAddressInfo::NotInImage;
}

SUPPRESS_ASAN FunctionRef callerFunction(const CallFrame* callFrame)
{
    if (!hasCode())
        return { };
    for (auto* record = reinterpret_cast<const FrameRecord*>(callFrame);; record = record->previous) {
        ImageAddressInfo what = classifyAddress(removeCodePtrTag(record->returnAddress));
        if (what.kind == ImageAddressInfo::Function) {
            FunctionRef function { instanceForFrame(record->previous), what.index };
            if (function.info().hasInlineFrames) [[unlikely]]
                return function.locationForReturnAddress(removeCodePtrTag(record->returnAddress), record->previous).function;
            return function;
        }
        if (what.kind != ImageAddressInfo::Stub)
            return { };
    }
}

JSCell* callerOwnerCell(const CallFrame* callFrame)
{
    FunctionRef function = callerFunction(callFrame);
    if (!function) {
        if (!hasCode() || kindOfAddress(removeCodePtrTag(reinterpret_cast<const FrameRecord*>(callFrame)->returnAddress)) != ImageAddressInfo::Stub)
            return nullptr;
        Instance* instance = tryInstanceForFrame(callFrame);
        return instance ? instance->globalObject : nullptr;
    }
    // What was compiled from types has no bytecode to make a code block of.
    if (!function.unlinkedCodeBlockIfExists() && !(function.metadata() && function.metadata()->find(FunctionMetadata::Scalars)))
        return function.instance->globalObject;
    return function.ensureCodeBlock();
}

const RegisterAtOffsetList& adapterSavedRegisters()
{
    static LazyNeverDestroyed<RegisterAtOffsetList> list;
    static std::once_flag once;
    std::call_once(once, [] {
        RegisterSet registers;
        registers.add(instanceGPR, IgnoreVectors);
        registers.add(GPRInfo::numberTagRegister, IgnoreVectors);
        registers.add(GPRInfo::notCellMaskRegister, IgnoreVectors);
#if CPU(ARM64)
        registers.add(cellCageBaseGPR, IgnoreVectors);
#endif
#if CPU(X86_64)
        registers.add(X86Registers::ebx, IgnoreVectors);
        registers.add(X86Registers::r12, IgnoreVectors);
#endif
        list.construct(registers);
        list->adjustOffsets(offsetOfInstanceRegisterInAdapter - list->find(instanceGPR)->offset());
#if CPU(ARM64)
        RELEASE_ASSERT(list->find(cellCageBaseGPR)->offset() == offsetOfCellCageBaseRegisterInAdapter);
#endif
#if CPU(X86_64)
        RELEASE_ASSERT(list->find(X86Registers::ebx)->offset() == offsetOfRBXInAdapter && list->find(X86Registers::r12)->offset() == offsetOfR12InAdapter);
#endif
        RELEASE_ASSERT(list->find(GPRInfo::numberTagRegister)->offset() == offsetOfNumberTagRegisterInAdapter && list->find(GPRInfo::notCellMaskRegister)->offset() == offsetOfNotCellMaskRegisterInAdapter);
    });
    return list.get();
}

Data* Data::create(Instance& instance, ScriptExecutable* executable, UnlinkedCodeBlock* unlinkedCodeBlock, JITCode& code, CodeBlock* codeBlock)
{
    return create(instance, executable, unlinkedCodeBlock, *code.imageFunction(), &code, codeBlock);
}

Data* Data::create(Instance& instance, const ImageFunction& function)
{
    return create(instance, nullptr, nullptr, function, nullptr, nullptr);
}

const ImageFunction* Data::imageFunction() const
{
    return reinterpret_cast<const ImageFunction*>(sites) - 1;
}

Data* Data::create(Instance& instance, ScriptExecutable* executable, UnlinkedCodeBlock* unlinkedCodeBlock, const ImageFunction& function, JITCode* code, CodeBlock* codeBlock)
{
    VM& vm = *instance.vm;
    DeferGCForAWhile deferGC(vm);
    unsigned numSlots = function.numSlots;
    bool hasLinkage = executable || unlinkedCodeBlock || code || codeBlock;
    Data* data = static_cast<Data*>(instance.allocateForData(allocationSize(numSlots, hasLinkage)));
    data->instance = &instance;
    const FunctionInfo& info = instance.infos[function.index];
    data->sites = function.sites();
    data->hasSiteConstants = function.hasSiteConstants;
    data->hasSitesInMegamorphicCache = false;
    data->hasLinkage = hasLinkage;
    data->numSlots = numSlots;
    data->slotEpoch = 1;
    if (code)
        code->ref();
    if (hasLinkage)
        *data->linkage() = { codeBlock, executable, unlinkedCodeBlock, code };

    RELEASE_ASSERT(function.index < instance.collections->numberOfFunctions);
    RELEASE_ASSERT(!instance.dataIfExists(function.index));
    instance.setData(function.index, data);
    data->indexInAllList = instance.collections->all.size();
    instance.collections->all.append(data);
    data->noteFilled();

    RELEASE_ASSERT(info.sitesInImage() == data->sites && info.numberOfSlotsForMisses() == std::min<uint32_t>(numSlots, FunctionInfo::maxEncodedSlots));
    return data;
}

bool Instance::hasRoomForData() const
{
    return !collections->freeOwnData.isEmpty() || collections->numberOfOwnDataEverUsed < maxNumberOfOwnData;
}

void Instance::ensureDataIfThereIsRoom(uint32_t index)
{
    missCounters[index % numberOfMissCounters] = 0;
    if (hasRoomForData())
        ensureData(index);
}

void Instance::setData(uint32_t index, Data* data)
{
    RELEASE_ASSERT(hasRoomForData() && !dataIfExists(index) && infos[index].stateNumber);
    unsigned which = collections->freeOwnData.isEmpty() ? collections->numberOfOwnDataEverUsed++ : collections->freeOwnData.takeLast();
    ownData[which] = data;
    states[infos[index].stateNumber] = static_cast<uint16_t>(offsetOfOwnData() / sizeof(Data*) + which);
}

void Instance::setNotLinked(uint32_t index)
{
    RELEASE_ASSERT(infos[index].stateNumber);
    uint16_t& state = states[infos[index].stateNumber];
    if (state > stateOfLinkedWithoutData()) {
        unsigned which = state - offsetOfOwnData() / sizeof(Data*);
        ownData[which] = nullptr;
        collections->freeOwnData.append(static_cast<uint16_t>(which));
    }
    state = 0;
}

Data* Instance::ensureData(uint32_t index)
{
    Data* data = dataIfExists(index);
    if (data)
        return data;
    const FunctionInfo& info = infos[index];
    RELEASE_ASSERT(!info.isTopLevelCode());
    return Data::create(*this, *info.function());
}

FunctionRef FunctionRef::at(Instance* instance, const void* address)
{
    auto& asked = instance->cachedAddressInfo(address);
    if (asked.address == address) [[likely]]
        return { instance, asked.function };
    ImageAddressInfo what = classifyAddress(address);
    RELEASE_ASSERT(what.kind == ImageAddressInfo::Function);
    asked = { address, what.index, Instance::CachedAddressInfo::siteNotResolved };
    return { instance, what.index };
}

CallSiteOverride::CallSiteOverride(Instance& instance, const void* frame)
    : m_instance(instance)
    , m_previous(instance.callSiteOverrides)
    , m_frame(frame)
{
    instance.callSiteOverrides = this;
}

CallSiteOverride::~CallSiteOverride()
{
    RELEASE_ASSERT(m_instance.callSiteOverrides == this);
    m_instance.callSiteOverrides = m_previous;
}

static ALWAYS_INLINE FunctionRef::Location locationForSite(FunctionRef function, uint32_t site, InlineFramesRead* read = nullptr)
{
    uint32_t typedFunction = function.info().isTyped ? function.info().typedFunction() : FunctionRef::none;
    if (!function.info().hasInlineFrames) [[likely]]
        return { function, CallSiteIndex(site).bytecodeIndex(), 0, false, false, false, typedFunction };
    const ImageFunction& record = *function.info().function();
    unsigned frame = PackedSite::inlineFrame(site);
    BytecodeIndex bytecodeIndex = CallSiteIndex(PackedSite::bits(site)).bytecodeIndex();
    if (!frame)
        return { function, bytecodeIndex, 0, false, false, false, typedFunction };
    ImageInlineFrame inlineFrame = read ? read->at(record, frame) : inlineFrameOf(record, frame);
    uint32_t inlined = inlineFrame.function;
    if (function.info().isTyped)
        return { function, bytecodeIndex, frame, false, false, inlineFrame.isConstruction(), inlined };
    return { FunctionRef { function.instance, inlined }, bytecodeIndex, frame, false, PackedSite::isTailCallSite(site) };
}

static const Instance::CachedAddressInfo& resolve(FunctionRef function, const void* returnAddress, uint32_t offset)
{
    using Asked = Instance::CachedAddressInfo;
    using Place = Instance::PlaceInTrace;
    Instance& instance = *function.instance;
    Asked* all = instance.ensureCachedReturnAddresses();
    size_t index = Instance::firstPlaceOfReturnAddress(returnAddress);
    if (all[index].address) {
        size_t second = Instance::secondPlaceOfReturnAddress(returnAddress);
        if (!all[second].address)
            index = second;
        else {
            instance.evictsSecondPlaceOfReturnAddress = !instance.evictsSecondPlaceOfReturnAddress;
            if (instance.evictsSecondPlaceOfReturnAddress)
                index = second;
        }
    }
    Asked& asked = all[index];
    auto& info = function.info();
    bool constructs = false;
    auto site = tryCallSiteAt(*info.function(), offset, &constructs);
    RELEASE_ASSERT(!site || *site < Asked::hasNoSite);
    asked = { returnAddress, function.index, site.value_or(Asked::hasNoSite) };
    auto& place = instance.placesInTraces()[index];
    place = { };
    instance.placesInUnwinding()[index] = { };
    if (!site || !info.isTyped)
        return asked;
    place.typedFunction = info.typedFunction();
    if (info.hasInlineFrames && PackedSite::inlineFrame(*site))
        place.kind = Place::IsInInlinedBody;
    else if (uint32_t bits = info.hasInlineFrames ? PackedSite::bits(*site) : *site; !(bits >> (constructs ? Place::numberOfBitsWithConstructedCallee : 30))) {
        place.callSiteBits = bits;
        place.kind = typedFunctionIsHiddenInStackTraces(place.typedFunction) ? Place::IsHidden : Place::IsShown;
    }
    if (constructs)
        place = place.withConstructedCallee();
    return asked;
}

Instance::CachedAddressInfo* Instance::allocateCachedReturnAddresses()
{
    cachedReturnAddresses = static_cast<CachedAddressInfo*>(fastZeroedMalloc(numberOfCachedAddressInfos * (sizeof(CachedAddressInfo) + sizeof(PlaceInTrace) + sizeof(PlaceInUnwinding))));
    return cachedReturnAddresses;
}

const Instance::PlaceInUnwinding& Instance::findPlaceInUnwinding(const CachedAddressInfo& asked)
{
    auto& place = placesInUnwinding()[&asked - cachedReturnAddresses];
    FunctionRef function { this, asked.function };
    auto& info = function.info();
    place.state = PlaceInUnwinding::IsNotLeft;
    if (!info.isTyped)
        return place;
    if (!info.hasNoHandlers) {
        if (asked.site >= CachedAddressInfo::hasNoSite)
            return place;
        UnlinkedHandlerInfo madeHere;
        uint32_t callSiteBits = info.hasInlineFrames ? PackedSite::bits(asked.site) : asked.site;
        if (auto* handler = function.handlerFor(CallSiteIndex(callSiteBits).bytecodeIndex().offset(), madeHere)) {
            place.offsetOfHandlerInImage = static_cast<uint32_t>(std::bit_cast<uintptr_t>(function.catchEntrypointAddress(handler->target)) - startOfImageCode());
            place.state = handler->type() == HandlerType::Finally ? PlaceInUnwinding::HasHandlerThatTakesException : handler->type() == HandlerType::SynthesizedCatch ? PlaceInUnwinding::HasHandlerThatIgnoresValue : PlaceInUnwinding::HasHandler;
            return place;
        }
    }
    const ImageFrame& frame = Image::of(*info.function()).frameOf(*info.function());
    for (uint64_t mask = ImageFunction::unpackRegisters(frame.calleeSaveRegisters); mask; mask &= mask - 1) {
        unsigned index = WTF::ctz(mask);
        if (RegisterSet::stackRegisters().contains(Reg::fromIndex(index), IgnoreVectors) || RegisterSet::vmCalleeSaveBufferSlotsByRegIndex()[index] < 0)
            return place;
    }
    place.savedRegisters = frame.calleeSaveRegisters;
    place.savedRegistersStart = frame.calleeSavesStart;
    place.state = typedFunctionRejectsWhatItThrows(info.typedFunction()) ? PlaceInUnwinding::RejectsPromise : PlaceInUnwinding::IsLeftByWhateverIsThrown;
    return place;
}

struct HandlerInCompiledCode {
    CallFrame* frame { nullptr };
    void* code { nullptr };
    bool takesException { false };
};

static ALWAYS_INLINE HandlerInCompiledCode findHandlerInCompiledCallers(VM& vm, Instance& instance, CallFrame* frameOfStub)
{
    using Place = Instance::PlaceInUnwinding;
    if (instance.callSiteOverrides || frameOfStub->callerFrameOrEntryFrame() == vm.topEntryFrame) [[unlikely]]
        return { };
    const Instance::CachedAddressInfo* cachedReturnAddresses = instance.ensureCachedReturnAddresses();
    const Place* places = instance.placesInUnwinding();
    CPURegister* calleeSaveRegistersBuffer = vmEntryRecord(vm.topEntryFrame)->calleeSaveRegistersBuffer;
    std::span<const int8_t> bufferSlotsByRegIndex = RegisterSet::vmCalleeSaveBufferSlotsByRegIndex();
    void* returnPC = removeCodePtrTag(frameOfStub->rawReturnPC());
    CallFrame* callFrame = frameOfStub->callerFrame();
    CallFrame* frameLeft = nullptr;
    static void* const rejectInFrame = removeCodePtrTag(rejectInFrameThunk());
    for (;; returnPC = removeCodePtrTag(callFrame->rawReturnPC()), frameLeft = callFrame, callFrame = callFrame->callerFrame()) {
        const Instance::CachedAddressInfo* found = instance.findCachedReturnAddress(returnPC);
        if (!found) [[unlikely]] {
            if (returnPC == instance.returnAddressOfSavedFrames && frameLeft)
                return { frameLeft, rejectInFrame, false };
            ImageAddressInfo what = classifyAddress(returnPC);
            if (what.kind == ImageAddressInfo::Stub && what.index != ImageAddressInfo::frameMayBeOfHostFunction && callFrame->callerFrameOrEntryFrame() != vm.topEntryFrame)
                continue;
            if (what.kind != ImageAddressInfo::Function)
                return { };
            found = &resolve(FunctionRef { &instance, what.index }, returnPC, what.offset);
        }
        const Place* place = &places[found - cachedReturnAddresses];
        if (place->state == Place::IsNotKnown) [[unlikely]]
            place = &instance.findPlaceInUnwinding(*found);
        if (place->state == Place::HasHandler || place->state == Place::HasHandlerThatTakesException || place->state == Place::HasHandlerThatIgnoresValue)
            return { callFrame, std::bit_cast<void*>(startOfImageCode() + place->offsetOfHandlerInImage), place->state == Place::HasHandlerThatTakesException };
        if (place->state == Place::IsNotLeft)
            return { };
        const CPURegister* slot = reinterpret_cast<const CPURegister*>(callFrame->registers()) - place->savedRegistersStart;
        for (uint64_t mask = ImageFunction::unpackRegisters(place->savedRegisters); mask; mask &= mask - 1, ++slot)
            calleeSaveRegistersBuffer[bufferSlotsByRegIndex[WTF::ctz(mask)]] = *slot;
        if (place->state == Place::RejectsPromise)
            return { callFrame, rejectInFrame, false };
    }
}

static NEVER_INLINE bool valueIsTakenByCompiledCallerOf(VM& vm, Instance& instance, CallFrame* callFrame)
{
    using Place = Instance::PlaceInUnwinding;
    const Instance::CachedAddressInfo* cachedReturnAddresses = instance.ensureCachedReturnAddresses();
    const Place* places = instance.placesInUnwinding();
    for (;;) {
        if (callFrame->callerFrameOrEntryFrame() == vm.topEntryFrame)
            return false;
        void* returnPC = removeCodePtrTag(callFrame->rawReturnPC());
        if (returnPC == instance.returnAddressOfSavedFrames)
            return true;
        callFrame = callFrame->callerFrame();
        const Instance::CachedAddressInfo* found = instance.findCachedReturnAddress(returnPC);
        if (!found) [[unlikely]] {
            ImageAddressInfo what = classifyAddress(returnPC);
            if (what.kind == ImageAddressInfo::Stub && what.index != ImageAddressInfo::frameMayBeOfHostFunction)
                continue;
            if (what.kind != ImageAddressInfo::Function)
                return false;
            found = &resolve(FunctionRef { &instance, what.index }, returnPC, what.offset);
        }
        const Place* place = &places[found - cachedReturnAddresses];
        if (place->state == Place::IsNotKnown) [[unlikely]]
            place = &instance.findPlaceInUnwinding(*found);
        if (place->state == Place::HasHandler || place->state == Place::HasHandlerThatIgnoresValue || place->state == Place::RejectsPromise)
            return true;
        if (place->state == Place::IsNotLeft)
            return false;
    }
}

bool Instance::nearestHandlerIgnoresValue(VM& vm, CallFrame* callFrame, void* returnPC)
{
    const PlaceInUnwinding* places = placesInUnwinding();
    for (;; returnPC = removeCodePtrTag(callFrame->rawReturnPC()), callFrame = callFrame->callerFrame()) {
        const CachedAddressInfo* found = findCachedReturnAddress(returnPC);
        if (!found) [[unlikely]] {
            ImageAddressInfo what = classifyAddress(returnPC);
            if (what.kind == ImageAddressInfo::Stub && what.index != ImageAddressInfo::frameMayBeOfHostFunction && callFrame->callerFrameOrEntryFrame() != vm.topEntryFrame)
                continue;
            if (what.kind != ImageAddressInfo::Function)
                return false;
            found = &resolve(FunctionRef { this, what.index }, returnPC, what.offset);
        }
        const PlaceInUnwinding* place = &places[found - cachedReturnAddresses];
        if (place->state == PlaceInUnwinding::IsNotKnown) [[unlikely]]
            place = &findPlaceInUnwinding(*found);
        if (place->state != PlaceInUnwinding::IsLeftByWhateverIsThrown)
            return place->state == PlaceInUnwinding::HasHandlerThatIgnoresValue;
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLookupExceptionHandler, UGPRPair, (VM* vmPointer, Instance* instance))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    Exception* exception = vm.exceptionForInspection();
    bool isAtSiteOfItsItem = std::exchange(instance->thrownValueIsAtSiteOfItsItem, false);
    bool needsException = false;
    Exception* passedOn = nullptr;
    if (exception)
        instance->thrownValue = 0;
    else {
        JSValue thrown = JSValue::decode(instance->thrownValue);
        needsException = instance->globalObject->debugger() || Options::breakOnThrow();
        if (thrown.isCell() && thrown.asCell()->type() == CellType)
            passedOn = uncheckedDowncast<Exception>(thrown.asCell());
    }
    HandlerInCompiledCode handler;
    if (!exception || !vm.isTerminationException(exception)) [[likely]]
        handler = findHandlerInCompiledCallers(vm, *instance, callFrame);
    if (passedOn && handler.code && !needsException) {
        if (!handler.takesException)
            instance->thrownValue = JSValue::encode(passedOn->value());
        return makeUGPRPair(std::bit_cast<UCPURegister>(handler.code), std::bit_cast<UCPURegister>(handler.frame));
    }
    bool stackIsNotCapturedYet = (!exception && !passedOn) || (exception && vm.deferredStackCapture.exception == exception);
    bool nobodyReadsStack = stackIsNotCapturedYet && handler.code && !needsException && (!handler.takesException || valueIsTakenByCompiledCallerOf(vm, *instance, handler.frame));
    if (vm.deferredStackCapture.exception) {
        if (nobodyReadsStack && vm.deferredStackCapture.exception == exception)
            vm.dropDeferredStackCapture();
        else {
            JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
            vm.finishDeferredStackCapture();
        }
    }
    if (handler.code && (exception || !(handler.takesException || needsException))) [[likely]]
        return makeUGPRPair(std::bit_cast<UCPURegister>(handler.code), std::bit_cast<UCPURegister>(handler.frame));
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    if (!exception) {
        JSValue thrown = JSValue::decode(std::exchange(instance->thrownValue, 0));
        RELEASE_ASSERT(thrown);
        auto scope = DECLARE_THROW_SCOPE(vm);
        Exception* made = throwException(instance->globalObject, scope, passedOn ? thrown : JSValue(Exception::create(vm, thrown, nobodyReadsStack ? Exception::StackCaptureAction::DoNotCaptureStack : Exception::StackCaptureAction::CaptureStack)));
        if (isAtSiteOfItsItem)
            made->noteThatTopFrameIsAtSiteOfItsItem();
        if (handler.code && handler.takesException && !needsException) {
            auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            instance->thrownValue = JSValue::encode(made);
            topScope.clearException();
        }
    }
    if (handler.code)
        return makeUGPRPair(std::bit_cast<UCPURegister>(handler.code), std::bit_cast<UCPURegister>(handler.frame));
    genericUnwind(vm, callFrame);
    return makeUGPRPair(0, 0);
}

const Instance::CachedAddressInfo* Instance::resolveReturnAddress(const void* returnAddress)
{
    if (auto* found = findCachedReturnAddress(returnAddress))
        return found;
    ImageAddressInfo what = classifyAddress(returnAddress);
    if (what.kind != ImageAddressInfo::Function)
        return nullptr;
    return &resolve(FunctionRef { this, what.index }, returnAddress, what.offset);
}

FunctionRef::Location FunctionRef::locationForReturnAddress(const void* returnAddress, const void* frame, InlineFramesRead* read) const
{
    using Asked = Instance::CachedAddressInfo;
    const Asked* found = instance->findCachedReturnAddress(returnAddress);
    if (!found) [[unlikely]] {
        ImageAddressInfo what = classifyAddress(returnAddress);
        RELEASE_ASSERT(what.kind == ImageAddressInfo::Function && what.index == index);
        found = &resolve(*this, returnAddress, what.offset);
    }
    auto& asked = *found;
    ASSERT(asked.function == index);
    if (asked.site == Asked::hasNoSite) [[unlikely]]
        return { *this, BytecodeIndex(), 0, false, false, false, info().isTyped ? info().typedFunction() : none };
    for (const CallSiteOverride* candidate = instance->callSiteOverrides; candidate; candidate = candidate->previous()) {
        if (candidate->frame() != frame)
            continue;
        if (auto site = spreadSite(*info().function(), asked.site, candidate->item()))
            return locationForSite(*this, *site, read);
        break;
    }
    return locationForSite(*this, asked.site, read);
}

FunctionRef::Location FunctionRef::inlineCallSiteLocation(unsigned inlineFrame, InlineFramesRead& read) const
{
    ImageInlineFrame frame = read.at(*info().function(), inlineFrame);
    Location location = locationForSite(*this, PackedSite::pack(frame.parent, frame.callSite), &read);
    location.isTailDeleted = frame.isTailCall();
    return location;
}

void appendFramesInProfile(uint32_t index, uint32_t site, bool& nothingIsShownYet, Vector<FrameInTrace, 4>& results)
{
    FunctionRef function { nullptr, index };
    if (!function.info().isTyped)
        return;
    InlineFramesRead read;
    for (auto location = locationForSite(function, site, &read);; location = function.inlineCallSiteLocation(location.inlineFrame, read)) {
        bool isHidden = nothingIsShownYet ? typedFunctionStandsForNoFunction(location.typedFunction) : typedFunctionIsHiddenInStackTraces(location.typedFunction);
        if (!location.isTailDeleted && !isHidden) {
            nothingIsShownYet = false;
            results.append(FrameInTrace::create(index, location.typedFunction, location.bytecodeIndex));
        }
        if (!location.inlineFrame)
            break;
    }
}

String describeCodeAddresses(std::span<const void* const> addresses)
{
    const ProgramData* program = ProgramData::get();
    if (!program || !Image::withCode())
        return { };
    StringBuilder result;
    bool nothingIsShownYet = true;
    for (const void* address : addresses) {
        ImageAddressInfo what = classifyAddress(address);
        if (what.kind != ImageAddressInfo::Function)
            continue;
        FunctionRef function { nullptr, what.index };
        if (!function.info().isTyped)
            continue;
        Vector<FrameInTrace, 4> frames;
        appendFramesInProfile(what.index, tryCallSiteAt(*function.info().function(), what.offset).value_or(0), nothingIsShownYet, frames);
        for (auto& frame : frames) {
            bool isModule = typedFunctionCodeType(frame.typedFunction) == ModuleCode;
            String name = isModule ? String() : typedFunctionName(frame.typedFunction);
            auto position = function.reportedPositionFor(frame.callSite());
            String place = position && position->source ? makeString(program->sourceName(position->source), ':', position->lineColumn.line, ':', position->lineColumn.column) : String();
            if (isModule)
                result.append("    at "_s, place, '\n');
            else if (place.isEmpty())
                result.append("    at "_s, name.isEmpty() ? StringView("<anonymous>"_s) : StringView(name), '\n');
            else
                result.append("    at "_s, name.isEmpty() ? StringView("<anonymous>"_s) : StringView(name), " ("_s, place, ")\n"_s);
        }
    }
    return result.toString();
}

BytecodeIndex FunctionRef::bytecodeIndexAt(const void* returnAddress, const void* frame) const
{
    return locationForReturnAddress(returnAddress, frame).bytecodeIndex;
}

void writePerfMap()
{
    const ProgramData* program = ProgramData::get();
    Image* image = Image::withCode();
    auto file = program && image ? FilePrintStream::open(makeString("/tmp/perf-"_s, getCurrentProcessID(), ".map"_s).utf8(), "w"_s) : nullptr;
    if (!file)
        return;
    auto& header = image->header();
    uint64_t code = startOfImageCode();
    const uint32_t* starts = image->at<uint32_t>(header.functionStartsOffset);
    Vector<std::pair<uint32_t, String>> rows;
    for (uint32_t index = 0; index < header.numberOfFunctions; ++index) {
        uint32_t end = index + 1 < header.numberOfFunctions ? starts[index + 1] : header.functionsEnd;
        FunctionRef function { nullptr, index };
        if (end == starts[index])
            continue;
        if (!function.info().isTyped) {
            rows.append({ starts[index], String("(compiled code)"_s) });
            continue;
        }
        uint32_t typedFunction = function.info().typedFunction();
        String name = typedFunctionCodeType(typedFunction) == ModuleCode ? String("(module)"_s) : typedFunctionName(typedFunction);
        auto position = function.reportedPositionFor(BytecodeIndex(0));
        String place = position && position->source ? makeString(' ', program->sourceName(position->source), ':', position->lineColumn.line) : emptyString();
        rows.append({ starts[index], makeString(name.isEmpty() ? String("(anonymous)"_s) : name, place) });
    }
    for (unsigned i = 0; i < header.numberOfStubCopies; ++i)
        rows.append({ header.stubCopies[i], String("(stubs of compiled code)"_s) });
    std::ranges::sort(rows, { }, &std::pair<uint32_t, String>::first);
    for (size_t i = 0; i < rows.size(); ++i) {
        uint32_t end = i + 1 < rows.size() ? rows[i + 1].first : header.functionsEnd;
        file->println(hex(code + rows[i].first, Lowercase), " ", hex(end - rows[i].first, Lowercase), " ", rows[i].second);
    }
}

FunctionRef FunctionRef::of(VM& vm, ScriptExecutable* scriptExecutable, JSCell* instanceToken)
{
    auto instanceWithToken = [&]() -> Instance* {
        for (Instance* instance : vm.m_aotInstances) {
            if (tokenOf(instance) == instanceToken)
                return instance;
        }
        return nullptr;
    };
    if (auto* moduleValue = dynamicDowncast<ModuleProgramExecutable>(scriptExecutable))
        return { instanceWithToken(), moduleValue->programModule()->functionIndex };
    return { };
}

CodeBlock* FunctionRef::ensureCodeBlock() const
{
    return ensureData()->ensureCodeBlock();
}

Data* FunctionRef::dataIfExists() const
{
    return instance ? instance->dataIfExists(index) : nullptr;
}

Data* FunctionRef::ensureData() const
{
    return instance->ensureData(index);
}

ScriptExecutable* FunctionRef::executable() const
{
    if (Data* data = dataIfExists())
        return data->executable();
    return nullptr;
}

ScriptExecutable* FunctionRef::executableIfExists() const
{
    if (Data* data = dataIfExists())
        return data->executable();
    return nullptr;
}

bool FunctionRef::hasExecutable() const
{
    if (Data* data = dataIfExists())
        return !!data->executable();
    return false;
}

CodeBlock* FunctionRef::codeBlockIfExists() const
{
    Data* data = dataIfExists();
    return data ? data->codeBlock() : nullptr;
}

uint32_t FunctionRef::siteConstantOf(const Slot* slot) const
{
    const FunctionInfo& info = this->info();
    size_t which = slot - (SharedData::contains(slot) ? instance->sharedData : instance->dataIfExists(index))->slots;
    if (!info.hasCodeInImage())
        return 0;
    if (!info.hasSiteConstants())
        return info.sitesInImage()[which].identifierAndExtra;
    return info.function()->siteConstants()[which];
}

std::optional<std::pair<String, bool>> FunctionRef::quoteAt(BytecodeIndex bytecodeIndex) const
{
    const ImageFunction* function = info().function();
    if (!function)
        return std::nullopt;
    return Image::of(*function).quoteAt(*function, bytecodeIndex.offset());
}

FunctionRef FunctionRef::of(CodeBlock* codeBlock)
{
    if (codeBlock->jitType() != JITType::AOTJIT)
        return { };
    RefPtr generated = codeBlock->jitCode();
    auto* code = static_cast<JITCode*>(generated.get());
    return code->instance() ? FunctionRef { code->instance(), code->index() } : FunctionRef { };
}

bool FunctionRef::constructsAt(BytecodeIndex bytecodeIndex) const
{
    const ImageFunction* function = info().function();
    return function && Image::of(*function).constructsAt(*function, bytecodeIndex.offset());
}

UnlinkedCodeBlock* FunctionRef::unlinkedCodeBlockIfExists() const
{
    Data* data = dataIfExists();
    return data ? data->unlinkedCodeBlock() : nullptr;
}

UnlinkedCodeBlock* FunctionRef::createUnlinkedCodeBlockFromMetadata() const
{
    auto* metadata = this->metadata();
    const uint32_t* scalars = metadata ? metadata->find(FunctionMetadata::Scalars) : nullptr;
    if (!scalars)
        return nullptr;
    CodeBlockParts parts { };
    parts.scalars = programData().at<uint8_t>(*scalars);
    parts.instructions = absentInstructions(metadata->instructionsSize());
    if (const uint32_t* words = metadata->find(FunctionMetadata::Handlers))
        parts.handlers = { programData().at<UnlinkedHandlerInfo>(words[0]), words[1] };
    if (codeType() == ModuleCode)
        return createModuleCodeBlockFromParts(*instance->vm, parts);
    return createFunctionCodeBlockFromParts(*instance->vm, parts);
}

UnlinkedCodeBlock* FunctionRef::ensureUnlinkedCodeBlock() const
{
    if (UnlinkedCodeBlock* existing = unlinkedCodeBlockIfExists())
        return existing;
    VM& vm = *instance->vm;
    DeferGCForAWhile deferGC(vm);
    DeferTerminationForAWhile deferTermination(vm);
    SuspendExceptionScope suspendExceptions(vm);
    Data* data = ensureData();
    UnlinkedCodeBlock* result = createUnlinkedCodeBlockFromMetadata();
    RELEASE_ASSERT(result && data->hasLinkage);
    data->linkage()->unlinkedCodeBlock = result;
    if (!data->hasBeenFilledSinceLastCollection)
        data->noteFilled();
    return result;
}

const FunctionMetadata* FunctionRef::metadata() const
{
    const ProgramData& data = programData();
    uint32_t at = data.functionMetadataOffsets()[index];
    static constexpr FunctionMetadata ofTypedFunctionWithoutHandlers { 0 };
    if (at & FunctionMetadata::isPositionsOfTypedFunction || FunctionMetadata::isOfTypedFunctionWithHandlers(at))
        return &ofTypedFunctionWithoutHandlers;
    return at ? data.at<FunctionMetadata>(at) : nullptr;
}

static uint64_t readVarint(const uint8_t*& at)
{
    uint64_t value = 0;
    for (unsigned shift = 0;; shift += 7) {
        uint8_t byte = *at++;
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return value;
    }
}

auto FunctionRef::reportedPositionFor(BytecodeIndex bytecodeIndex, ConstructPosition constructPosition) const -> std::optional<ReportedPosition>
{
    struct Remembered {
        uint32_t functionPlusOne;
        uint32_t offsetAndConstructPosition;
        ReportedPosition position;
    };
    static thread_local std::array<Remembered, 16> allRemembered;
    uint32_t offsetAndConstructPosition = bytecodeIndex.offset() << 1 | static_cast<uint32_t>(constructPosition);
    Remembered& remembered = allRemembered[(index * 31 + bytecodeIndex.offset()) % allRemembered.size()];
    if (remembered.functionPlusOne == index + 1 && remembered.offsetAndConstructPosition == offsetAndConstructPosition)
        return remembered.position;
    auto position = decodeReportedPositionFor(bytecodeIndex, constructPosition);
    if (position && !(bytecodeIndex.offset() >> 31))
        remembered = { index + 1, offsetAndConstructPosition, *position };
    return position;
}

auto FunctionRef::decodeReportedPositionFor(BytecodeIndex bytecodeIndex, ConstructPosition constructPosition) const -> std::optional<ReportedPosition>
{
    const uint8_t* at = programData().positionsOfTypedFunction(index);
    if (auto* metadata = at ? nullptr : this->metadata()) {
        if (const uint32_t* where = metadata->find(FunctionMetadata::ExpressionInfo))
            at = programData().at<uint8_t>(*where);
    }
    if (!at)
        return std::nullopt;
    ReportedPosition result;
    result.lineColumn.line = static_cast<unsigned>(readVarint(at));
    result.lineColumn.column = static_cast<unsigned>(readVarint(at));
    uint64_t offset = 0;
    int64_t line = 0;
    int64_t column = 0;
    uint32_t source = 0;
    auto zigZagDecode = [](uint64_t value) {
        return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1);
    };
    auto readPosition = [&]() -> ReportedPosition {
        uint64_t word = readVarint(at);
        if (word & 1)
            column += zigZagDecode(word >> 1);
        else {
            line += zigZagDecode(word >> 2);
            if (word & 2)
                source = static_cast<uint32_t>(readVarint(at));
            column = static_cast<int64_t>(readVarint(at));
        }
        return { { static_cast<unsigned>(line), static_cast<unsigned>(column) }, source };
    };
    bool hasEntryForEveryOffset = info().isTyped;
    // Of a typed function each is relative to the last one of the same inline frame.
    struct State {
        int64_t line { 0 };
        int64_t column { 0 };
        uint32_t source { 0 };
    };
    Vector<State, 8> states;
    std::optional<FramesOfSites> frames;
    uint64_t count = readVarint(at);
    if (hasEntryForEveryOffset) {
        frames.emplace(*info().function());
        line = result.lineColumn.line;
        column = result.lineColumn.column;
        source = static_cast<uint32_t>(readVarint(at));
        result.source = source;
    }
    const State start { line, column, source };
    uint64_t first = 0;
    if (hasEntryForEveryOffset) {
        states.append(start);
        first = 1;
        constexpr uint64_t sitesBetween = FunctionMetadata::sitesBetweenPositionCheckpoints;
        uint64_t numberOfCheckpoints = count ? (count - 1) / sitesBetween : 0;
        const uint8_t* checkpoints = at;
        at += numberOfCheckpoints * sizeof(uint16_t);
        if (uint64_t numberAhead = std::min<uint64_t>(bytecodeIndex.offset() / sitesBetween, numberOfCheckpoints)) {
            for (uint64_t i = 0; i < numberAhead; ++i)
                at += checkpoints[2 * i] | checkpoints[2 * i + 1] << 8;
            first = numberAhead * sitesBetween;
            offset = first - 1;
            frames->skip(first - 1);
        }
    }
    unsigned frame = 0;
    for (uint64_t i = first; i < count; ++i) {
        uint64_t word = hasEntryForEveryOffset ? 2 : readVarint(at);
        offset += word >> 1;
        if (offset > bytecodeIndex.offset())
            break;
        if (hasEntryForEveryOffset) {
            states[frame] = { line, column, source };
            frame = frames->next();
            while (frame >= states.size())
                states.append(start);
            line = states[frame].line;
            column = states[frame].column;
            source = states[frame].source;
        }
        result = readPosition();
        if (word & 1) {
            ReportedPosition start = readPosition();
            if (constructPosition == ConstructPosition::AtStart)
                result = start;
        }
    }
    return result;
}

CodeType FunctionRef::codeType() const
{
    return info().codeType();
}

bool FunctionRef::isBuiltinFunction() const
{
    return metadata()->flagsAndInstructionsSize & FunctionMetadata::isBuiltinFunction;
}

unsigned FunctionRef::instructionsSize() const
{
    return metadata()->instructionsSize();
}

void* FunctionRef::catchEntrypointAddress(unsigned bytecodeOffset) const
{
    const ImageFunction& function = *info().function();
    return const_cast<uint8_t*>(Image::of(function).codeFor(function)) + bytecodeOffset;
}

template<typename Number>
static const UnlinkedHandlerInfo* handlerInRow(std::span<const uint8_t> row, unsigned site, UnlinkedHandlerInfo& madeHere)
{
    constexpr size_t sizeOfHandler = 3 * sizeof(Number);
    bool someHandlerIgnoresValue = row[0] & FunctionMetadata::someHandlerIgnoresValue;
    size_t numberOfHandlers = someHandlerIgnoresValue ? (row.size() - 1) * 8 / (sizeOfHandler * 8 + 1) : (row.size() - 1) / sizeOfHandler;
    const uint8_t* ignoresValue = row.data() + 1 + numberOfHandlers * sizeOfHandler;
    for (size_t i = 0; i < numberOfHandlers; ++i) {
        std::array<Number, 3> handler;
        memcpy(handler.data(), row.data() + 1 + i * sizeOfHandler, sizeof(handler));
        bool takesException = handler[0] > handler[1];
        if (takesException)
            std::swap(handler[0], handler[1]);
        if (site >= handler[1])
            continue;
        if (site < handler[0])
            return nullptr;
        bool ignores = someHandlerIgnoresValue && ignoresValue[i / 8] >> i % 8 & 1;
        madeHere = UnlinkedHandlerInfo(handler[0], handler[1], handler[2], takesException ? HandlerType::Finally : ignores ? HandlerType::SynthesizedCatch : HandlerType::Catch);
        return &madeHere;
    }
    return nullptr;
}

const UnlinkedHandlerInfo* FunctionRef::handlerFor(unsigned bytecodeOffset, UnlinkedHandlerInfo& madeHere) const
{
    if (auto row = programData().handlersOfTypedFunction(index); !row.empty()) {
        unsigned width = row[0] & ~FunctionMetadata::someHandlerIgnoresValue;
        if (width == 1)
            return handlerInRow<uint8_t>(row, bytecodeOffset, madeHere);
        if (width == 2)
            return handlerInRow<uint16_t>(row, bytecodeOffset, madeHere);
        return handlerInRow<uint32_t>(row, bytecodeOffset, madeHere);
    }
    const uint32_t* words = metadata()->find(FunctionMetadata::Handlers);
    if (!words)
        return nullptr;
    std::span<const UnlinkedHandlerInfo> handlers { programData().at<UnlinkedHandlerInfo>(words[0]), words[1] };
    return UnlinkedHandlerInfo::handlerForIndex<const UnlinkedHandlerInfo>(handlers, bytecodeOffset, RequiredHandler::AnyHandler);
}

const UnlinkedStringJumpTable& FunctionRef::stringSwitchJumpTable(unsigned tableIndex) const
{
    return instance->program->stringSwitchJumpTable(*metadata()->find(FunctionMetadata::StringSwitchJumpTables), tableIndex);
}

const IdentifierSet& FunctionRef::constantIdentifierSet(unsigned index) const
{
    return instance->program->identifierSet(*metadata()->find(FunctionMetadata::ConstantIdentifierSets), index);
}

void Data::destroy(Data* data)
{
    Instance& instance = *data->instance;
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        instance.forgetSlotsNoLongerWatched(data);
    if (data->hasSitesInMegamorphicCache)
        instance.vm->invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change);
    RELEASE_ASSERT(instance.dataIfExists(data->imageFunction()->index) == data);
    instance.setNotLinked(data->imageFunction()->index);
    auto belongsToThisData = [&](Slot* slot) { return slot >= data->slots && slot < data->slots + data->numSlots; };
    instance.collections->transitions.removeAllMatching(belongsToThisData);
    instance.collections->transitionsSinceLastCollection.removeAllMatching(belongsToThisData);
    auto removeFrom = [&](Vector<Data*>& list, unsigned Data::*index) {
        RELEASE_ASSERT(list[data->*index] == data);
        Data* last = list.takeLast();
        if (last != data) {
            list[data->*index] = last;
            last->*index = data->*index;
        }
    };
    removeFrom(instance.collections->all, &Data::indexInAllList);
    if (data->hasBeenFilledSinceLastCollection)
        removeFrom(instance.collections->filledSinceLastCollection, &Data::indexInFilledList);
    delete data->watchpoints;
    instance.collections->allSiteSlots.removeAllMatching([&](PolymorphicSlots* several) {
        if (several->owner != data)
            return false;
        fastFree(several);
        return true;
    });
    if (Linkage* linkage = data->linkage()) {
        if (linkage->code)
            linkage->code->deref();
    }
    instance.freeDataMemory(data, allocationSize(data->numSlots, data->hasLinkage));
}

FunctionRef Data::function() const
{
    return { instance, imageFunction()->index };
}

CodeBlock* Data::ensureCodeBlock()
{
    RELEASE_ASSERT(hasLinkage);
    auto& [codeBlock, executable, unlinkedCodeBlock, code] = *linkage();
    if (codeBlock)
        return codeBlock;
    VM& vm = *instance->vm;
    if (!unlinkedCodeBlock)
        function().ensureUnlinkedCodeBlock();
    DeferGCForAWhile deferGC(vm);
    DeferTerminationForAWhile deferTermination(vm);
    SuspendExceptionScope suspendExceptions(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    CodeBlock* result;
    if (auto* moduleValue = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(unlinkedCodeBlock))
        result = ModuleProgramCodeBlock::create(vm, uncheckedDowncast<ModuleProgramExecutable>(executable), moduleValue, instance->globalObject, CodeBlock::LinkMode::ForCodeFromImage);
    else
        result = FunctionCodeBlock::create(vm, uncheckedDowncast<FunctionExecutable>(executable), uncheckedDowncast<UnlinkedFunctionCodeBlock>(unlinkedCodeBlock), instance->globalObject, CodeBlock::LinkMode::ForCodeFromImage);
    scope.releaseAssertNoException();
    RELEASE_ASSERT(result);
    result->adoptAOTCode(*code, this);
    codeBlock = result;
    if (!hasBeenFilledSinceLastCollection)
        noteFilled();
    return result;
}

LineColumn FunctionRef::lineColumnFor(BytecodeIndex bytecodeIndex) const
{
    if (auto position = reportedPositionFor(bytecodeIndex))
        return position->lineColumn;
    ScriptExecutable* executable = this->executable();
    RELEASE_ASSERT(bytecodeIndex.offset() < instructionsSize());
    SourceProvider& provider = *executable->source().provider();
    unsigned sourceOffset = executable->source().startOffset();
    return provider.documentLineColumn(provider.lineColumnInTextForOffset(sourceOffset));
}

ASCIILiteral nameOf(Stub stub)
{
    switch (stub) {
#define AOT_NAME_OF_STUB(name) case Stub::name: return #name ""_s;
    FOR_EACH_AOT_STUB(AOT_NAME_OF_STUB)
#undef AOT_NAME_OF_STUB
    case Stub::NumberOfStubs:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(Entry entry)
{
    switch (entry) {
#define AOT_NAME_OF_ENTRY(name) case Entry::name: return #name ""_s;
    FOR_EACH_AOT_OPERATION(AOT_NAME_OF_ENTRY)
    FOR_EACH_AOT_THUNK(AOT_NAME_OF_ENTRY)
    FOR_EACH_AOT_POINTER(AOT_NAME_OF_ENTRY)
#undef AOT_NAME_OF_ENTRY
    case Entry::NumberOfEntries:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Data::noteFilled()
{
    hasBeenFilledSinceLastCollection = true;
    indexInFilledList = instance->collections->filledSinceLastCollection.size();
    instance->collections->filledSinceLastCollection.append(this);
}

template<typename Visitor>
void Data::visit(Visitor& visitor)
{
    if (!hasLinkage)
        return;
    auto& [codeBlock, executable, unlinkedCodeBlock, code] = *linkage();
    if (executable)
        visitor.appendUnbarriered(executable);
    if (unlinkedCodeBlock)
        visitor.appendUnbarriered(unlinkedCodeBlock);
    if (codeBlock)
        visitor.appendUnbarriered(codeBlock);
}

static ALWAYS_INLINE bool hasTransition(const Slot& slot)
{
    return slot.structureID && (slot.offset & Slot::isIndirect) && slot.newStructureID && !slot.hasPointer();
}

PolymorphicSlots* Instance::makeSiteSlots(Data* owner, const Slot* slot, UniquedStringImpl* name)
{
    auto* several = static_cast<PolymorphicSlots*>(fastZeroedMalloc(sizeof(PolymorphicSlots)));
    several->name = name;
    static_assert(Site::identifierBits < 31);
    several->identifierIndex = (owner->sites[slot - owner->slots].identifierAndExtra & ((1u << Site::identifierBits) - 1)) | PolymorphicSlots::hasNotReadCompiledField;
    several->owner = owner;
    several->remainingBulkLearnAttempts = PolymorphicSlots::maxBulkLearnAttempts;
    several->missesUntilLesson = PolymorphicSlots::missesPerEarlyLesson;
    several->owesItsLessons = !parseIndex(*name) && name != vm->propertyNames->underscoreProto.impl();
    several->byName = PolymorphicSlots::initialByName;
    several->remainingNameTableFillFailures = PolymorphicSlots::maxNameTableFillFailures;
    collections->allSiteSlots.append(several);
    return several;
}

void Instance::noteTransitionCached(Slot* slot)
{
    collections->transitionsSinceLastCollection.append(slot);
}

void Instance::noteCellAdded(JSCell* cell)
{
    if (cell)
        collections->cellsAddedSinceLastCollection.append(cell);
}

template<typename Visitor>
void Instance::visit(Visitor& visitor, bool newOnly)
{
    bool visitsEverything = !std::is_same_v<Visitor, SlotVisitor>;
    if (visitsEverything)
        newOnly = false;
    else if (!newOnly)
        visitsEverything = !std::exchange(collections->hasVisitedEverythingInThisCollection, true);
    for (Data* data : visitsEverything ? collections->all : collections->filledSinceLastCollection)
        data->visit(visitor);
    auto visitTransitions = [&](const Vector<Slot*>& slots) {
        for (Slot* slot : slots) {
            if (hasTransition(*slot) && visitor.isMarked(slot->structureID.decode()))
                visitor.appendUnbarriered(slot->newStructureID.decode());
        }
    };
    visitTransitions(collections->transitionsSinceLastCollection);
    if (newOnly) {
        for (auto& [from, last] : collections->propertyRunTargetsAddedSinceLastCollection) {
            if (last && visitor.isMarked(from))
                visitor.appendUnbarriered(last);
        }
    } else {
        visitTransitions(collections->transitions);
        for (auto& [from, to] : collections->propertyRunTargets) {
            if (to.last && visitor.isMarked(from.first))
                visitor.appendUnbarriered(to.last);
        }
    }
    visitor.appendUnbarriered(globalObject);
    for (Structure* structure : collections->emptyStructuresWithoutPrototype.values())
        visitor.appendUnbarriered(structure);
    for (auto& ofType : typedArraysWithBuiltinLength) {
        for (auto& typedArray : ofType) {
            visitor.appendUnbarriered(typedArray.prototype);
            visitor.appendUnbarriered(typedArray.secondPrototype);
        }
    }
    visitor.appendUnbarriered(typedArrayViewPrototype);
    visitor.appendUnbarriered(typedArrayLengthAccessor);
    visitor.appendUnbarriered(typedArrayLengthGetter);
    visitor.appendUnbarriered(boundFunctionExecutable);
    visitor.appendUnbarriered(argumentsStructure);
    for (Structure* structure : emptyStructuresFilledByKey)
        visitor.appendUnbarriered(structure);
    for (Structure* structure : arrayInTheMakingStructures)
        visitor.appendUnbarriered(structure);
    visitor.appendUnbarriered(structureOfTypedContiguousArrays);
    visitor.appendUnbarriered(bufferStructure);
    visitor.appendUnbarriered(bufferAccessorHolder);
    visitor.appendUnbarriered(collections->token);
    if (!visitsEverything) {
        for (JSCell* cell : collections->cellsAddedSinceLastCollection)
            visitor.appendUnbarriered(cell);
        return;
    }
    if (collections->environmentsSize) {
        for (uint32_t distance = sizeof(void*); distance <= Image::environmentsSize(); distance += sizeof(void*)) {
            if (JSCell* environment = *reinterpret_cast<JSCell**>(reinterpret_cast<char*>(this) - distance))
                visitor.appendUnbarriered(environment);
        }
    }
    for (ScriptExecutable* executable : collections->topLevelExecutables.values())
        visitor.appendUnbarriered(executable);
    for (JSArray* templateObject : collections->templateObjects.values())
        visitor.appendUnbarriered(templateObject);
}

void Instance::watchBufferAccessor(unsigned row, const ObjectPropertyCondition& condition)
{
    if (!collections->bufferAccessorWatchpoints)
        collections->bufferAccessorWatchpoints = makeUniqueWithoutFastMallocCheck<std::array<BufferAccessorWatchpoint, numberOfBufferAccessors>>();
    collections->bufferAccessorWatchpoints->at(row).watch(*vm, condition, *this, row);
}

// The names are not kept alive. Where one has gone and another is at its address, the row stands for other names than it did: whoever goes by a target holds it against the names at hand.
const uint32_t* Instance::canonicalPropertyRun(std::span<UniquedStringImpl* const> names)
{
    // The null String is no key.
    RELEASE_ASSERT(!names.empty());
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.canonicalPropertyRun(names);
    std::span bytes { reinterpret_cast<const Latin1Character*>(names.data()), names.size_bytes() };
    if (auto found = collections->canonicalPropertyRuns.find<StringViewHashTranslator>(StringView { bytes }); found != collections->canonicalPropertyRuns.end())
        return found->value.get();
    return collections->canonicalPropertyRuns.ensure(String { bytes }, [] {
        return makeUniqueWithoutFastMallocCheck<uint32_t>(0u);
    }).iterator->value.get();
}

bool Instance::hasPropertyRunTarget(Structure* structure, std::span<UniquedStringImpl* const> names)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.hasPropertyRunTarget(structure, names);
    std::span bytes { reinterpret_cast<const Latin1Character*>(names.data()), names.size_bytes() };
    auto found = collections->canonicalPropertyRuns.find<StringViewHashTranslator>(StringView { bytes });
    return found != collections->canonicalPropertyRuns.end() && collections->propertyRunTargets.contains({ structure, found->value.get() });
}

const Instance::PropertyRunTarget& Instance::propertyRunTarget(Structure* structure, const uint32_t* run, const ScopedLambda<void(Vector<UniquedStringImpl*, 16>&)>& collectNames)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.propertyRunTarget(structure, run, collectNames);
    if (auto it = collections->propertyRunTargets.find({ structure, run }); it != collections->propertyRunTargets.end())
        return it->value;
    Vector<UniquedStringImpl*, 16> names;
    collectNames(names);
    PropertyRunTarget target;
    bool mayBeIntercepted = names.contains(vm->propertyNames->underscoreProto.impl());
    for (JSValue next = structure->storedPrototype(); next.isObject() && !mayBeIntercepted;) {
        JSObject* prototype = asObject(next);
        // The walk reads what the objects have past their hooks. What is found out here is kept for the Structure of the receiver, which does not change when the prototype becomes ordinary.
        Typed::ClassPrototype::look(prototype);
        Structure* prototypeStructure = prototype->structure();
        if (prototypeStructure->isDictionary() && !prototypeStructure->isUncacheableDictionary() && !prototypeStructure->hasBeenFlattenedBefore())
            prototypeStructure = prototypeStructure->flattenDictionaryStructure(*vm, prototype);
        TypeInfo info = prototypeStructure->typeInfo();
        // The prototype of a class looks only at what is stored to itself: see Typed::PrototypeObject::put().
        bool overridesPut = info.overridesPut() && !prototype->inherits<Typed::PrototypeObject>();
        if (prototypeStructure->isDictionary() || prototypeStructure->hasPolyProto() || overridesPut || info.overridesGetPrototype() || info.overridesGetOwnPropertySlot() || prototype->hasNonReifiedStaticProperties()) {
            mayBeIntercepted = true;
            break;
        }
        if (prototypeStructure->hasReadOnlyOrGetterSetterPropertiesExcludingProto()) {
            for (UniquedStringImpl* name : names) {
                unsigned attributes;
                if (isValidOffset(prototypeStructure->get(*vm, name, attributes)))
                    mayBeIntercepted |= !!(attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue);
            }
        }
        target.prototypeStructures.append(prototypeStructure->id());
        next = prototypeStructure->storedPrototype();
    }
    Structure* last = mayBeIntercepted ? nullptr : structure;
    unsigned followed = 0;
    for (; last && followed < names.size(); ++followed) {
        PropertyOffset offset;
        Structure* next = Structure::addPropertyTransitionToExistingStructure(last, names[followed], 0, offset);
        if (!next)
            break;
        last = next;
    }
    if (last && followed < names.size()) {
        if (last->isDictionary())
            last = nullptr;
        else {
            DeferredStructureTransitionWatchpointFire deferred(*vm, last);
            last = Structure::addPropertiesTransition(*vm, last, names.subspan(followed), &deferred);
        }
    }
    if (last)
        noteRunOfProperties(structure, last);
    PropertyOffset offset = structure->maxOffset();
    for (unsigned i = 0; last && i < names.size(); ++i) {
        offset = offsetAfter(offset, structure->inlineCapacity());
        if (last->get(*vm, names[i]) != offset)
            last = nullptr;
    }
    if (last && (last->maxOffset() != offset || last->isDictionary()))
        last = nullptr;
    target.last = last;
    collections->propertyRunTargetsAddedSinceLastCollection.append({ structure, last });
    collections->hasAddedWeakEntriesSinceLastCollection = true;
    return collections->propertyRunTargets.add({ structure, run }, WTF::move(target)).iterator->value;
}

Structure* Instance::emptyStructureWithoutPrototype(unsigned inlineCapacity)
{
    if (inlineCapacity <= JSFinalObject::defaultInlineCapacity)
        return globalObject->nullPrototypeObjectStructure();
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.emptyStructureWithoutPrototype(inlineCapacity);
    inlineCapacity = std::min(inlineCapacity, JSFinalObject::maxInlineCapacity);
    if (Structure* structure = collections->emptyStructuresWithoutPrototype.get(inlineCapacity))
        return structure;
    Structure* structure = JSFinalObject::createStructure(*vm, globalObject, jsNull(), inlineCapacity);
    collections->emptyStructuresWithoutPrototype.set(inlineCapacity, structure);
    return structure;
}

void Instance::fillRow(uint32_t number, Structure* structure, uint32_t placesToClear, LocalAllocator* allocator)
{
    Slot& row = rows[number];
    RELEASE_ASSERT(!row.structureID);
    row.offset = placesToClear;
    row.pointer = allocator;
    WTF::storeStoreFence();
    row.structureID = structure->id();
    collections->filledRows.append(number);
}

Structure* Instance::literalStructure(Structure* empty, std::span<UniquedStringImpl* const> names, uint32_t numberOfRowPlusOne)
{
    if (numberOfRowPlusOne > UINT16_MAX || names.empty() || names.size() > empty->inlineCapacity())
        numberOfRowPlusOne = 0;
    if (Instance& realmStructure = ensure(globalObject); &realmStructure != this)
        return realmStructure.literalStructure(empty, names);
    ASSERT(empty->storedPrototype() == globalObject->objectPrototype());
    unsigned inlineCapacity = empty->inlineCapacity();
    uint64_t key = inlineCapacity;
    for (UniquedStringImpl* name : names) {
        key = (key + std::bit_cast<uintptr_t>(name)) * 0x9e3779b97f4a7c15ull;
        key ^= key >> 32;
    }
    key = key >> 1 | 1;
    auto hasTheNames = [&](Structure* structure) {
        if (structure->inlineCapacity() != inlineCapacity || structure->inlineSize() + structure->outOfLineSize() != names.size())
            return false;
        if (numberOfRowPlusOne && structure->numberOfRowPlusOne() == numberOfRowPlusOne)
            return true;
        for (unsigned i = 0; i < names.size(); ++i) {
            if (structure->get(*vm, names[i]) != offsetForPropertyNumber(i, inlineCapacity))
                return false;
        }
        return true;
    };
    for (size_t i = 0; numberOfRowPlusOne && i < names.size(); ++i) {
        if (!(typed->data.stringFacts(typed->data.row(numberOfRowPlusOne - 1)[1 + i]) & Typed::Data::stringHasIdentifierNumber))
            numberOfRowPlusOne = 0;
    }
    if (Structure* known = collections->shapes.get(key); known && hasTheNames(known)) {
        if (numberOfRowPlusOne && !known->numberOfRowPlusOne())
            known->setNumberOfRowPlusOne(numberOfRowPlusOne);
        return known;
    }
    Structure* result = numberOfRowPlusOne ? Structure::createOfRow(*vm, empty, names, numberOfRowPlusOne) : Structure::createWithProperties(*vm, empty, names);
    if (!result)
        return nullptr;
    collections->shapes.set(key, result);
    collections->hasAddedWeakEntriesSinceLastCollection = true;
    noteCellAdded(result);
    return result;
}
template void Instance::visit(AbstractSlotVisitor&, bool);
template void Instance::visit(SlotVisitor&, bool);

void Instance::noteOutOfLineProperty(Structure* structure)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.noteOutOfLineProperty(structure);
    if (structure->inlineCapacity() >= maxLearnedInlineCapacity)
        return;
    if (JSFunction* constructor = constructorOfObjectsWith(structure)) {
        learnInlineCapacity(constructor, structure);
        return;
    }
    Structure* first = structure;
    while (Structure* previous = first->previousID())
        first = previous;
    size_t sizeOfCell = MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(std::min<unsigned>(maxLearnedInlineCapacity, structure->inlineSize() + structure->outOfLineSize())));
    unsigned wanted = (sizeOfCell - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue);
    uint32_t numberPlusOne = first->numberOfRowPlusOne();
    if (!numberPlusOne || rows[numberPlusOne - 1].structureID != first->id())
        return;
    uint32_t number = numberPlusOne - 1;
    collections->learnedRoomOfRows.set(number, wanted);
    zeroBytes(rows[number]);
    collections->filledRows.removeFirst(number);
}

unsigned Instance::learnedRoomOfRow(uint32_t number)
{
    return collections->learnedRoomOfRows.isEmpty() ? 0 : collections->learnedRoomOfRows.get(number);
}

void Instance::noteRunOfProperties(Structure* from, Structure* to)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.noteRunOfProperties(from, to);
    if (to->inlineCapacity() >= maxLearnedInlineCapacity)
        return;
    JSFunction* constructor = constructorOfObjectsWith(from);
    if (!constructor)
        return;
    if (to->outOfLineSize())
        learnInlineCapacity(constructor, to);
    else if (!to->previousID())
        collections->setConstructorByFirstStructure(to, constructor);
}

JSFunction* Instance::constructorOfObjectsWith(Structure* structure)
{
    if (collections->constructorsByFirstStructure.isEmpty())
        return nullptr;
    while (Structure* previous = structure->previousID())
        structure = previous;
    return collections->constructorsByFirstStructure.get(structure);
}

void Instance::learnInlineCapacity(JSFunction* constructor, Structure* structure)
{
    unsigned inlineCapacity = structure->inlineCapacity();
    unsigned wanted = std::min(maxLearnedInlineCapacity, std::max(2 * inlineCapacity, inlineCapacity + structure->outOfLineSize() + 2));
    auto addResult = collections->learnedInlineCapacities.add(constructor, 0);
    collections->hasAddedWeakEntriesSinceLastCollection |= addResult.isNewEntry;
    unsigned& learned = addResult.iterator->value;
    if (wanted <= learned)
        return;
    learned = wanted;
    if (FunctionRareData* rareData = constructor->rareData())
        rareData->clear("Objects get properties outside their inline storage");
}

Instance::StructuresOfClass Instance::structuresOfClass(uint32_t classIndex, JSObject* parentOfClassObject, JSCell* parentOfPrototype)
{
    return collections->structuresOfClasses.get({ classIndex + 1, parentOfClassObject, parentOfPrototype });
}

void Instance::noteStructuresOfClass(uint32_t classIndex, JSObject* parentOfClassObject, JSCell* parentOfPrototype, StructuresOfClass structures)
{
    collections->structuresOfClasses.set({ classIndex + 1, parentOfClassObject, parentOfPrototype }, structures);
    collections->hasAddedWeakEntriesSinceLastCollection = true;
}

const Instance::CopiedProperties& Instance::copiedProperties(Structure* target, Structure* source, const IdentifierSet* excluded)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.copiedProperties(target, source, excluded);
    std::tuple key { target, source, excluded };
    if (auto it = collections->copiedProperties.find(key); it != collections->copiedProperties.end())
        return it->value;
    constexpr unsigned maxSize = 4096;
    if (collections->copiedProperties.size() >= maxSize)
        collections->copiedProperties.clear();

    CopiedProperties result;
    Vector<UniquedStringImpl*, 16> names;
    Structure* last = target;
    source->forEachProperty(*vm, [&](const PropertyTableEntry& entry) {
        if (PropertyName(entry.key()).isPrivateName() || (entry.attributes() & PropertyAttribute::DontEnum))
            return true;
        if (excluded && excluded->contains(entry.key()))
            return true;
        names.append(entry.key());
        result.offsets.append({ entry.offset(), invalidOffset });
        return true;
    });
    for (unsigned i = 0; last && i < names.size(); ++i) {
        unsigned attributes;
        PropertyOffset offset = last->get(*vm, names[i], attributes);
        if (isValidOffset(offset)) {
            if (attributes)
                last = nullptr;
            continue;
        }
        Structure* next = Structure::addPropertyTransitionToExistingStructure(last, names[i], 0, offset);
        if (!next) {
            DeferredStructureTransitionWatchpointFire deferred(*vm, last);
            next = Structure::addNewPropertyTransition(*vm, last, names[i], 0, offset, PutPropertySlot::UnknownContext, &deferred);
        }
        last = next->isDictionary() ? nullptr : next;
    }
    for (unsigned i = 0; last && i < names.size(); ++i)
        result.offsets[i].second = last->get(*vm, names[i]);
    result.last = last;
    collections->hasAddedWeakEntriesSinceLastCollection = true;
    return collections->copiedProperties.add(key, WTF::move(result)).iterator->value;
}

static std::tuple<Structure*, UniquedStringImpl*, uintptr_t> keyOfCopyExcluding(Structure* source, std::span<UniquedStringImpl* const> excluded)
{
    uintptr_t behindFirst = 0;
    for (UniquedStringImpl* name : excluded.subspan(1))
        behindFirst = behindFirst * 0x9e3779b97f4a7c15ull + std::bit_cast<uintptr_t>(name);
    return { source, excluded[0], behindFirst };
}

const Instance::CopyExcluding* Instance::copyExcluding(Structure* source, std::span<UniquedStringImpl* const> excluded)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.copyExcluding(source, excluded);
    auto found = collections->copiesExcluding.find(keyOfCopyExcluding(source, excluded));
    if (found == collections->copiesExcluding.end() || found->value.excluded.size() != excluded.size())
        return nullptr;
    for (size_t i = 0; i < excluded.size(); ++i) {
        if (found->value.excluded[i].get() != excluded[i])
            return nullptr;
    }
    return &found->value;
}

const Instance::CopyExcluding* Instance::setCopyExcluding(Structure* source, CopyExcluding&& copy)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.setCopyExcluding(source, WTF::move(copy));
    collections->namesOfCopiesExcludingThatWent.shrink(0);
    collections->hasAddedWeakEntriesSinceLastCollection = true;
    noteCellAdded(source);
    noteCellAdded(copy.structure);
    Vector<UniquedStringImpl*, 4> excluded(copy.excluded.size(), [&](size_t i) {
        return copy.excluded[i].get();
    });
    return &collections->copiesExcluding.set(keyOfCopyExcluding(source, excluded.span()), WTF::move(copy)).iterator->value;
}

const Instance::CopyForMore* Instance::copyForMore(Structure* source, uint64_t more)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.copyForMore(source, more);
    std::pair key { source, more };
    if (collections->keyOfLastCopyForMore == key)
        return collections->lastCopyForMore;
    auto found = collections->copiesForMore.find(key);
    if (found == collections->copiesForMore.end())
        return nullptr;
    collections->keyOfLastCopyForMore = key;
    collections->lastCopyForMore = &found->value;
    return &found->value;
}

const Instance::CopyForMore* Instance::setCopyForMore(Structure* source, uint64_t more, CopyForMore copy)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.setCopyForMore(source, more, copy);
    collections->hasAddedWeakEntriesSinceLastCollection = true;
    collections->keyOfLastCopyForMore = { nullptr, 0 };
    noteCellAdded(source);
    noteCellAdded(copy.structure);
    return &collections->copiesForMore.set(std::pair { source, more }, copy).iterator->value;
}

const Instance::SpreadLiteralPlan* Instance::spreadLiteralPlan(const void* site, const SpreadLiteralPlan::Sources& sources)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.spreadLiteralPlan(site, sources);
    if (auto* last = collections->lastSpreadLiteralPlan; last && last->site == site && last->sources == sources)
        return last;
    auto found = collections->spreadLiteralPlans.find(std::tuple { site, sources[0], sources[1], sources[2] });
    if (found == collections->spreadLiteralPlans.end())
        return nullptr;
    collections->lastSpreadLiteralPlan = found->value.get();
    return found->value.get();
}

const Instance::SpreadLiteralPlan* Instance::addSpreadLiteralPlan(SpreadLiteralPlan&& plan)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.addSpreadLiteralPlan(WTF::move(plan));
    constexpr unsigned maxSize = 4096;
    if (collections->spreadLiteralPlans.size() >= maxSize)
        collections->spreadLiteralPlans.clear();
    collections->lastSpreadLiteralPlan = nullptr;
    collections->hasAddedWeakEntriesSinceLastCollection = true;
    for (Structure* source : plan.sources)
        noteCellAdded(source);
    noteCellAdded(plan.structure);
    std::tuple key { plan.site, plan.sources[0], plan.sources[1], plan.sources[2] };
    return collections->spreadLiteralPlans.set(key, makeUniqueWithoutFastMallocCheck<SpreadLiteralPlan>(WTF::move(plan))).iterator->value.get();
}

bool Instance::isArrayInTheMaking(JSCell* cell) const
{
    Structure* structure = cell->structure();
    return structure == arrayInTheMakingStructures[0] || structure == arrayInTheMakingStructures[1] || structure == arrayInTheMakingStructures[2];
}

Structure* Instance::arrayInTheMakingStructure(IndexingType indexingType)
{
    if (!arrayInTheMakingHeaders[indexOfArrayInTheMaking(ArrayWithContiguous)]) {
        for (IndexingType type : { ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous }) {
            Structure* structure = JSArray::createStructure(*vm, globalObject, globalObject->arrayPrototype(), type);
            RELEASE_ASSERT(structure->typeInfoBlob() == globalObject->originalArrayStructureForIndexingType(type)->typeInfoBlob());
            arrayInTheMakingStructures[indexOfArrayInTheMaking(type)] = structure;
            arrayInTheMakingHeaders[indexOfArrayInTheMaking(type)] = static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
        }
    }
    return arrayInTheMakingStructures[indexOfArrayInTheMaking(indexingType)];
}

Structure* Instance::typedContiguousArrayStructure()
{
    if (!structureOfTypedContiguousArrays && !globalObject->isHavingABadTime() && MarkedBlockCage::isEnabled()) {
        Structure* structure = Structure::nonPropertyTransition(*vm, globalObject->originalArrayStructureForIndexingType(ArrayWithContiguous), TransitionKind::AllocateTypedContiguous, nullptr);
        structureOfTypedContiguousArrays = structure;
        newArrayWithTypedContiguousHeader = static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
        vm->writeBarrier(globalObject);
    }
    return newArrayWithTypedContiguousHeader ? structureOfTypedContiguousArrays : nullptr;
}

void Instance::didHaveBadTime()
{
    newArrayWithTypedContiguousHeader = 0;
    newArrayWithInt32Header = 0;
    newArrayWithDoubleHeader = 0;
    newArrayWithContiguousHeader = 0;
    zeroSpan(std::span { newCopyOnWriteArrayHeaders });
    inlineAllocations[static_cast<unsigned>(InlineAllocation::Arguments)].allocator = nullptr;
}

void Instance::noteOwnPropertyPlace(uint32_t structureID, UniquedStringImpl* uid, uint32_t place)
{
    auto& names = collections->namesOfOwnPropertyPlaces;
    if (names.size() >= 4 * numberOfOwnPropertyPlaces) [[unlikely]] {
        Vector<RefPtr<UniquedStringImpl>> namesInTable;
        for (auto& entry : ownPropertyPlaces) {
            if (entry.structureID)
                namesInTable.append(entry.uid);
        }
        names = WTF::move(namesInTable);
    }
    if (!hasOwnPropertyPlaces)
        names.shrink(0);
    names.append(uid);
    ownPropertyPlaces[ownPropertyPlaceIndex(structureID, std::bit_cast<uintptr_t>(uid))] = { uid, structureID, place };
    hasOwnPropertyPlaces = true;
}

void Instance::validateKeyedTables()
{
    VM& vm = *this->vm;
    DeferGC deferGC(vm);
    uint32_t epoch = vm.ensureMegamorphicCache().epoch();
    auto uidOf = [](JSCell* key) -> RefPtr<UniquedStringImpl> {
        if (key->isSymbol())
            return &asSymbol(key)->privateName().uid();
        StringImpl* text = key->isString() ? asString(key)->tryGetValueImpl() : nullptr;
        if (!text)
            return nullptr;
        if (text->isAtom())
            return static_cast<UniquedStringImpl*>(text);
        return AtomStringImpl::lookUp(text);
    };
    auto isOrdinary = [](Structure* structure) {
        return !structure->typeInfo().overridesGetOwnPropertySlot() && !TypeInfo::hasStaticPropertyTable(structure->typeInfo().inlineTypeFlags()) && !structure->compiledLayout() && structure->hasMonoProto();
    };
    auto fail = [](ASCIILiteral table, unsigned index, ASCIILiteral what, Structure* structure, JSCell* key) {
        dataLogLn("AOT: ", table, "[", index, "]: ", what, ": structure ", RawPointer(structure), " ", *structure, ", key ", JSValue(key));
        CRASH();
    };
    auto placeIn = [&](Structure* structure, UniquedStringImpl* uid, unsigned& attributes) {
        attributes = 0;
        return uid ? structure->get(vm, uid, attributes) : invalidOffset;
    };
    enum class Found : uint8_t { Nowhere, There, Unknown };
    auto findAbove = [&](Structure* structure, UniquedStringImpl* uid, JSObject*& holder, PropertyOffset& offset, unsigned& attributes) {
        for (JSValue prototype = structure->storedPrototype(); prototype.isObject(); prototype = holder->getPrototypeDirect()) {
            holder = asObject(prototype);
            if (!isOrdinary(holder->structure()))
                return Found::Unknown;
            offset = placeIn(holder->structure(), uid, attributes);
            if (isValidOffset(offset))
                return Found::There;
        }
        return Found::Nowhere;
    };
    constexpr uint32_t placeMask = Slot::offsetMask | Slot::isIndirect;
    auto isAt = [&](uint32_t said, PropertyOffset offset) {
        auto location = isValidOffset(offset) ? propertyLocation(offset) : std::nullopt;
        return location && *location == (said & placeMask);
    };
    for (unsigned i = 0; i < numberOfKeyedReads; ++i) {
        auto& read = keyedReads[i];
        if (!read.slot.structureID)
            continue;
        Structure* structure = read.slot.structureID.decode();
        JSCell* key = static_cast<JSCell*>(read.slot.pointer);
        if (!isOrdinary(structure))
            continue;
        auto uid = uidOf(key);
        unsigned attributes;
        PropertyOffset own = placeIn(structure, uid.get(), attributes);
        uint32_t flags = read.slot.offset & Slot::flagsMask;
        if (!(flags & Slot::isGetter)) {
            if (!isAt(read.slot.offset, own) || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
                fail("keyedReads"_s, i, "the object has no plain value where the entry says"_s, structure, key);
            continue;
        }
        if (flags != (Slot::isGetter | Slot::pointerIsCell) && flags != keyIsAbsentForGood)
            continue;
        bool isMemo = (read.epoch & ~epochIsValidated) == (epoch | epochIsOfDictionary);
        if (isMemo ? !structure->isDictionary() || std::bit_cast<uintptr_t>(read.holder) != structure->numberOfPropertiesOfDictionary() : read.epoch != epoch && read.epoch != MegamorphicCache::permanentEpoch)
            continue;
        if (isValidOffset(own))
            fail("keyedReads"_s, i, "the entry says it is not the object's own, and it is"_s, structure, key);
        JSObject* holder = nullptr;
        PropertyOffset above = invalidOffset;
        Found found = findAbove(structure, uid.get(), holder, above, attributes);
        if (found == Found::Unknown)
            continue;
        if (isMemo || !read.holder) {
            if (found == Found::There)
                fail("keyedReads"_s, i, "the entry says nowhere, and an object above has it"_s, structure, key);
            continue;
        }
        if (found == Found::Nowhere || holder != read.holder || !isAt(read.slot.offset, above))
            fail("keyedReads"_s, i, "it is not on the holder, or not at the place, that the entry says"_s, structure, key);
    }
    for (unsigned i = 0; i < numberOfKeyedHas; ++i) {
        auto& entry = keyedHas[i];
        if (uint32_t epochOfEntry = entry.offset & ((1u << keyedHasAnswerShift) - 1); !entry.structureID || entry.offset & Slot::isGetter || (epochOfEntry != epoch && epochOfEntry != MegamorphicCache::permanentEpoch))
            continue;
        Structure* structure = entry.structureID.decode();
        JSCell* key = static_cast<JSCell*>(entry.pointer);
        if (!isOrdinary(structure))
            continue;
        auto uid = uidOf(key);
        unsigned attributes;
        bool has = isValidOffset(placeIn(structure, uid.get(), attributes));
        if (!has) {
            JSObject* holder = nullptr;
            PropertyOffset above = invalidOffset;
            Found found = findAbove(structure, uid.get(), holder, above, attributes);
            if (found == Found::Unknown)
                continue;
            has = found == Found::There;
        }
        if (has != !!(entry.offset >> keyedHasAnswerShift & 1))
            fail("keyedHas"_s, i, "the answer is the other one"_s, structure, key);
    }
    for (unsigned i = 0; i < numberOfKeyedReads; ++i) {
        auto& entry = keyedReads[i].slot;
        if (!entry.structureID || entry.offset & Slot::flagsMask & ~Slot::isIndirect || entry.offset & keyMayNotBeReplaced)
            continue;
        Structure* structure = entry.structureID.decode();
        JSCell* key = static_cast<JSCell*>(entry.pointer);
        if (!isOrdinary(structure))
            continue;
        auto uid = uidOf(key);
        unsigned attributes;
        PropertyOffset own = placeIn(structure, uid.get(), attributes);
        if (!isAt(entry.offset, own) || attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
            fail("keyedReads"_s, i, "the object has no plain value that may be written where the entry says"_s, structure, key);
    }
    for (unsigned i = 0; i < numberOfKeyedAdditions; ++i) {
        auto& addition = keyedAdditions[i];
        if (!addition.slot.structureID || (addition.epoch != MegamorphicCache::invalidEpoch && addition.epoch != epoch))
            continue;
        Structure* structure = addition.slot.structureID.decode();
        Structure* afterwards = addition.newStructureID.decode();
        JSCell* key = static_cast<JSCell*>(addition.slot.pointer);
        if (!isOrdinary(structure) || !isOrdinary(afterwards))
            continue;
        auto uid = uidOf(key);
        unsigned attributes;
        if (isValidOffset(placeIn(structure, uid.get(), attributes)))
            fail("keyedAdditions"_s, i, "the object has the property already"_s, structure, key);
        if (!isAt(addition.slot.offset, placeIn(afterwards, uid.get(), attributes)) || afterwards->previousID() != structure)
            fail("keyedAdditions"_s, i, "the Structure afterwards is no heir, or has the property elsewhere"_s, structure, key);
    }
    for (unsigned i = 0; hasOwnPropertyPlaces && i < numberOfOwnPropertyPlaces; ++i) {
        auto& entry = ownPropertyPlaces[i];
        if (!entry.structureID || entry.place & OwnPropertyPlace::isCompiledField)
            continue;
        Structure* structure = std::bit_cast<StructureID>(entry.structureID).decode();
        if (!isOrdinary(structure))
            continue;
        unsigned attributes;
        PropertyOffset own = placeIn(structure, entry.uid, attributes);
        bool saysValue = entry.place & OwnPropertyPlace::isPresent && !(entry.place & OwnPropertyPlace::isNoValue);
        if (isValidOffset(own) != !!(entry.place & OwnPropertyPlace::isPresent) || (saysValue && (attributes & OwnPropertyPlace::attributes) != (entry.place & OwnPropertyPlace::attributes))) {
            dataLogLn("AOT: ownPropertyPlaces[", i, "]: the object has it, or has it not, or with other attributes, than the entry says: structure ", RawPointer(structure), " ", *structure, ", name ", entry.uid);
            CRASH();
        }
    }
}

template<typename Entry>
static void takeOutOfKeyedTable(VM& vm, std::span<Entry> table, bool isAtEndOfCollection, bool mayStay = true)
{
    static_assert(!OBJECT_OFFSETOF(Instance::KeyedRead, slot) && !OBJECT_OFFSETOF(Instance::KeyedAddition, slot));
    for (Entry& entry : table) {
        Slot& slot = *std::bit_cast<Slot*>(&entry);
        if (!slot.structureID)
            continue;
        uint32_t flags = slot.offset & Slot::flagsMask;
        bool holdsWhileBothLive = mayStay && (!(flags & Slot::isGetter) || flags == Instance::keyIsCompiledField || flags == Instance::keyIsAbsent);
        if (!holdsWhileBothLive || (isAtEndOfCollection && (!vm.heap.isMarked(slot.structureID.decode()) || !vm.heap.isMarked(static_cast<JSCell*>(slot.pointer)))))
            zeroBytes(entry);
    }
}

void Instance::forgetNewTarget()
{
    if (typed)
        typed->newTarget = { };
}

void Instance::sweepKeyedTables()
{
    keyedScratchEntry.fill(0);
    Heap& heap = vm->heap;
    MegamorphicCache* cache = vm->megamorphicCache();
    uint32_t currentEpoch = cache ? cache->epoch() : MegamorphicCache::invalidEpoch;
    auto isLive = [&](StructureID structureID) {
        return heap.isMarked(structureID.decode());
    };
    auto holds = [&](uint32_t epoch) {
        return epoch == currentEpoch || epoch == MegamorphicCache::permanentEpoch;
    };
    auto namesLiveCells = [&](const Slot& slot) {
        return isLive(slot.structureID) && heap.isMarked(static_cast<JSCell*>(slot.pointer));
    };
    for (KeyedRead& read : std::span { keyedReads, numberOfKeyedReads }) {
        if (!read.slot.structureID)
            continue;
        uint32_t flags = read.slot.offset & Slot::flagsMask;
        bool stays = namesLiveCells(read.slot) && flags != Slot::isGetter;
        if (stays && flags & Slot::isGetter && flags & Slot::pointerIsCell) {
            uint32_t epoch = read.epoch & ~epochIsValidated;
            if (epoch & epochIsOfDictionary)
                stays = (epoch & ~epochIsOfDictionary) == currentEpoch;
            else
                stays = holds(epoch) && (!read.holder || heap.isMarked(read.holder));
        }
        if (!stays)
            zeroBytes(read);
    }
    for (Slot& entry : std::span { keyedHas, numberOfKeyedHas }) {
        if (!entry.structureID)
            continue;
        uint32_t flags = entry.offset & Slot::flagsMask;
        if (!namesLiveCells(entry) || (flags ? flags != keyedHasIsLookedUp : !holds(entry.offset & ((1u << keyedHasAnswerShift) - 1))))
            zeroBytes(entry);
    }
    for (KeyedAddition& addition : std::span { keyedAdditions, numberOfKeyedAdditions }) {
        if (!addition.slot.structureID)
            continue;
        if (!namesLiveCells(addition.slot) || !isLive(addition.newStructureID) || (addition.epoch != currentEpoch && addition.epoch != MegamorphicCache::invalidEpoch))
            zeroBytes(addition);
    }
    if (hasOwnPropertyAnswers) {
        hasOwnPropertyAnswers = false;
        for (uint64_t& answer : ownPropertyAnswers) {
            if (!answer)
                continue;
            if (isLive(std::bit_cast<StructureID>(static_cast<uint32_t>(answer))))
                hasOwnPropertyAnswers = true;
            else
                answer = 0;
        }
    }
    if (hasOwnPropertyPlaces) {
        hasOwnPropertyPlaces = false;
        for (OwnPropertyPlace& entry : ownPropertyPlaces) {
            if (!entry.structureID)
                continue;
            if (isLive(std::bit_cast<StructureID>(entry.structureID)))
                hasOwnPropertyPlaces = true;
            else
                zeroBytes(entry);
        }
    }
}

void Instance::clearCachesValidatedByMegamorphicCacheEpoch(bool isAtEndOfCollection)
{
    if (propertyRuns)
        zeroSpan(std::span { propertyRuns, numberOfPropertyRuns });
    if (customGetters) {
        auto isMarked = [&](JSCell* cell) { return !cell || vm->heap.isMarked(cell); };
        for (auto& record : std::span { customGetters, numberOfCustomGetters }) {
            if (!record.structureID)
                continue;
            if (isAtEndOfCollection && isMarked(std::bit_cast<StructureID>(record.structureID).decode()) && isMarked(record.passedForReceiver) && isMarked(record.globalObject))
                continue;
            uint32_t lastSerial = record.lastSerial;
            record = { };
            record.lastSerial = lastSerial;
        }
    }
    zeroSpan(std::span { inheritedSetters });
    if (isAtEndOfCollection) {
        sweepKeyedTables();
        return;
    }
    takeOutOfKeyedTable(*vm, std::span { keyedReads, numberOfKeyedReads }, isAtEndOfCollection);
    takeOutOfKeyedTable(*vm, std::span { keyedHas, numberOfKeyedHas }, false, false);
    takeOutOfKeyedTable(*vm, std::span { keyedAdditions, numberOfKeyedAdditions }, false, false);
}

static void validateStructureOfSlotIsLive(VM& vm, const Data* data, const Slot& slot)
{
    if (!slot.structureID || vm.heap.isMarked(slot.structureID.decode()))
        return;
    dataLogLn("AOT: slot ", RawPointer(&slot), " of function ", data->imageFunction()->index, " names a Structure that has died, and the collector was not told that it was filled: structure id ", slot.structureID.bits(), " offset and flags ", RawHex(slot.offset), " pointer ", RawPointer(slot.pointer));
    CRASH();
}

void Instance::finalizeUnconditionally(bool newOnly)
{
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        validateSlotsNoLongerWatched(nullptr, true);
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        AOT::runtimeTable(*vm).countOperation("Heap::collection", newOnly ? "eden" : "full");
        AOT::runtimeTable(*vm).forgetFilledPairsOfDeadStructures(*vm);
        AOT::writeOperationCounts(*vm);
    }
    if (typed) {
        typed->forgetPlansOfCoercionsOfDeadStructures(*vm, newOnly);
        typed->forgetMadeKeysThatAreDead(*vm);
        typed->forgetExtraPropertyReads();
    }
    if (std::exchange(hasHeirsOfPrototypes, false))
        zeroSpan(std::span { heirsOfPrototypes });
    if (std::exchange(hasThisAllocations, false))
        zeroSpan(std::span { thisAllocations });
    if (std::exchange(hasCaseConversions, false))
        zeroBytes(caseConversions);
    zeroSpan(std::span { emptyStructuresOfLiterals });
    collections->filledRows.removeAllMatching([&](uint32_t number) {
        if (vm->heap.isMarked(rows[number].structureID.decode()))
            return false;
        zeroBytes(rows[number]);
        return true;
    });
    clearCachesValidatedByMegamorphicCacheEpoch(true);
    for (PolymorphicSlots* several : collections->allSiteSlots) {
        if (newOnly && !several->owner->hasBeenFilledSinceLastCollection) {
            if (Options::validateAOTTypedFacts()) [[unlikely]] {
                for (Slot& slot : several->slots)
                    validateStructureOfSlotIsLive(*vm, several->owner, slot);
            }
            continue;
        }
        for (Slot& slot : several->slots)
            several->owner->finalizeSlot(*vm, slot);
    }
    if (newOnly && Options::validateAOTTypedFacts()) [[unlikely]] {
        for (Data* data : collections->all) {
            if (data->hasBeenFilledSinceLastCollection)
                continue;
            for (unsigned i = 0; i < data->numSlots; ++i)
                validateStructureOfSlotIsLive(*vm, data, data->slots[i]);
        }
    }
    for (Data* data : newOnly ? collections->filledSinceLastCollection : collections->all)
        data->finalizeUnconditionally(*vm);
    for (Data* data : collections->filledSinceLastCollection)
        data->hasBeenFilledSinceLastCollection = false;
    collections->filledSinceLastCollection.shrink(0);
    auto isDead = [&](uint32_t structureID) { return !vm->heap.isMarked(std::bit_cast<StructureID>(structureID).decode()); };
    if (typed) {
        for (auto& cache : typed->globalPropertyCaches()) {
            if (cache.structureID && isDead(cache.structureID))
                cache = { };
        }
    }
    bool viewPrototypeHasChanged = typedArrayViewPrototypeStructureID && isDead(typedArrayViewPrototypeStructureID);
    for (auto& ofType : typedArraysWithBuiltinLength) {
        for (auto& entry : ofType) {
            if (entry.structureID && (viewPrototypeHasChanged || isDead(entry.structureID) || isDead(entry.prototypeStructureID) || isDead(entry.secondPrototypeStructureID)))
                entry.structureID = 0;
        }
    }
    if (std::exchange(collections->hasAddedWeakEntriesSinceLastCollection, false) || !newOnly) {
        collections->propertyRunTargets.removeIf([&](auto& entry) {
            if (!vm->heap.isMarked(entry.key.first) || (entry.value.last && !vm->heap.isMarked(entry.value.last)))
                return true;
            return std::ranges::any_of(entry.value.prototypeStructures, [&](StructureID id) { return !vm->heap.isMarked(id.decode()); });
        });
        collections->constructorsByFirstStructure.removeIf([&](auto& entry) {
            return !vm->heap.isMarked(entry.key) || !vm->heap.isMarked(entry.value);
        });
        collections->learnedInlineCapacities.removeIf([&](auto& entry) {
            return !vm->heap.isMarked(entry.key);
        });
        collections->copiedProperties.removeIf([&](auto& entry) {
            auto& [target, source, excluded] = entry.key;
            return !vm->heap.isMarked(target) || !vm->heap.isMarked(source) || (entry.value.last && !vm->heap.isMarked(entry.value.last));
        });
        collections->structuresOfClasses.removeIf([&](auto& entry) {
            return !vm->heap.isMarked(entry.value.ofClassObject) || !vm->heap.isMarked(entry.value.ofPrototype);
        });
        collections->shapes.removeIf([&](auto& entry) {
            return !vm->heap.isMarked(entry.value);
        });
        collections->copiesExcluding.removeIf([&](auto& entry) {
            if (vm->heap.isMarked(std::get<0>(entry.key)) && (!entry.value.structure || vm->heap.isMarked(entry.value.structure)))
                return false;
            for (auto& name : entry.value.excluded)
                collections->namesOfCopiesExcludingThatWent.append(WTF::move(name));
            return true;
        });
        collections->keyOfLastCopyForMore = { nullptr, 0 };
        collections->copiesForMore.removeIf([&](auto& entry) {
            return !vm->heap.isMarked(entry.key.first) || (entry.value.structure && !vm->heap.isMarked(entry.value.structure));
        });
        collections->lastSpreadLiteralPlan = nullptr;
        collections->spreadLiteralPlans.removeIf([&](auto& entry) {
            return (entry.value->structure && !vm->heap.isMarked(entry.value->structure)) || std::ranges::any_of(entry.value->sources, [&](Structure* source) { return source && !vm->heap.isMarked(source); });
        });
        collections->copyStructures.removeIf([&](auto& entry) {
            return !vm->heap.isMarked(entry.key) || (entry.value && !vm->heap.isMarked(entry.value));
        });
    }
    collections->cellsAddedSinceLastCollection.shrink(0);
    collections->propertyRunTargetsAddedSinceLastCollection.shrink(0);
    collections->hasVisitedEverythingInThisCollection = false;
    collections->transitions.appendVector(collections->transitionsSinceLastCollection);
    collections->transitionsSinceLastCollection.shrink(0);
    if (!newOnly) {
        for (Data* data : collections->all)
            data->retryAbandonedSlots();
        auto& transitions = collections->transitions;
        std::ranges::sort(transitions);
        transitions.shrink(std::ranges::unique(transitions).begin() - transitions.begin());
        transitions.removeAllMatching([](Slot* slot) { return !hasTransition(*slot); });
    }
    if (Options::verboseAOTCompilation()) [[unlikely]] {
        dataLogLn("AOT: instance ", RawPointer(this), " after ", newOnly ? "an eden" : "a full", " collection: ", collections->all.size(), " of ", collections->numberOfFunctions, " functions have slots, in ", (collections->usedDataEnd - collections->dataStart) << stateWithDataShift, " bytes; ",
            collections->allSiteSlots.size(), " sites with several slots (", collections->allSiteSlots.size() * sizeof(PolymorphicSlots), " bytes); ", collections->transitions.size(), " transitions; structures of literals ", collections->shapes.size(),
            ", of copies ", collections->copyStructures.size(), "; property runs ", collections->propertyRunTargets.size(), "; copied properties ", collections->copiedProperties.size(),
            "; constructors ", collections->constructorsByFirstStructure.size(), " + ", collections->learnedInlineCapacities.size(), "; modules ", collections->topLevelExecutables.size(), "; template objects ", collections->templateObjects.size(),
            "; environments ", Image::environmentsSize() / sizeof(void*));
    }
}

void Instance::forgetValuesOfGlobalProperties()
{
    if (typed)
        zeroSpan(typed->globalPropertyCaches());
}

void Instance::forgetResolvedGlobalScopes()
{
    if (typed)
        zeroSpan(typed->globalPropertyCaches());
    for (Data* data : collections->all) {
        for (unsigned i = 0; i < data->numSlots; ++i) {
            Slot& slot = data->slots[i];
            if (slot.structureID || (slot.pointer != globalObject && slot.pointer != globalObject->globalLexicalEnvironment()))
                continue;
            slot.clear();
            data->slotEpoch++;
        }
    }
}

void Data::finalizeSlot(VM& vm, Slot& slot)
{
    if (!slot.structureID)
        return;
    bool dead = !vm.heap.isMarked(slot.structureID.decode());
    if (!dead) {
        if (slot.offset & Slot::pointerIsCell)
            dead = !vm.heap.isMarked(static_cast<JSCell*>(slot.pointer));
        else if (hasTransition(slot))
            dead = !vm.heap.isMarked(slot.newStructureID.decode());
        else if (slot.readsExtraProperty())
            dead = !vm.heap.isMarked(slot.structureIDOfExtraProperties().decode());
    }
    if (dead) {
        if (Options::useAOTOperationCounters()) [[unlikely]]
            runtimeTable(vm).countOperation("Data::finalizeSlot", "dead");
        slot.clearAndForgetAttempts();
        slotEpoch++;
    }
}

void Data::retryAbandonedSlots()
{
    for (unsigned i = 0; i < numSlots; ++i) {
        Slot& slot = slots[i];
        if ((slot.offset & Slot::attemptsMask) != Slot::attemptsMask || (!slot.structureID && slot.pointer))
            continue;
        uint32_t attempts = slot.structureID ? Slot::maxAttempts - 1 : Slot::maxAttemptsForInheritedProperty - 1;
        slot.offset = (slot.offset & ~Slot::attemptsMask) | attempts << Slot::attemptsShift;
    }
}

void Data::finalizeUnconditionally(VM& vm)
{
    for (unsigned i = 0; i < numSlots; ++i)
        finalizeSlot(vm, slots[i]);

    if (watchpoints) {
        watchpoints->removeIf([&](auto& entry) {
            Slot& slot = *entry.key;
            if (!slot.structureID)
                return true;
            for (const SlotWatchpoint& watchpoint : entry.value) {
                if (!watchpoint.key().isStillLive(vm)) {
                    slot.clearAndForgetAttempts();
                    slotEpoch++;
                    return true;
                }
            }
            return false;
        });
    }
}

static Stub stubFor(JITCode::Way way)
{
    switch (way) {
    case JITCode::Way::TopLevel:
        return Stub::Enter;
    case JITCode::Way::Call:
        return Stub::EnterFunctionForCall;
    case JITCode::Way::Construct:
        return Stub::EnterFunctionForConstruct;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JITCode::Way JITCode::entryBlockFor(UnlinkedCodeBlock* codeBlock)
{
    if (codeBlock->codeType() != FunctionCode)
        return Way::TopLevel;
    return codeBlock->isConstructor() ? Way::Construct : Way::Call;
}

SavedRegisters savedRegistersOf(const ImageFunction& function)
{
    const ImageFrame& frame = Image::of(function).frameOf(function);
    return { ImageFunction::unpackRegisters(frame.calleeSaveRegisters), -static_cast<ptrdiff_t>(frame.calleeSavesStart) };
}

const RegisterAtOffsetList* calleeSaveRegistersOf(const ImageFunction& function)
{
    static Lock lock;
    static NeverDestroyed<UncheckedKeyHashMap<uint64_t, Vector<std::unique_ptr<RegisterAtOffsetList>, 1>>> lists;

    const ImageFrame& frame = Image::of(function).frameOf(function);
    uint64_t mask = ImageFunction::unpackRegisters(frame.calleeSaveRegisters);
    ptrdiff_t offsetOfFirst = -static_cast<ptrdiff_t>(frame.calleeSavesStart * sizeof(CPURegister));
    uint64_t key = (static_cast<uint64_t>(frame.calleeSavesStart) << 32 | frame.calleeSaveRegisters) * 2 + 1;
    auto matchCase = [&](const RegisterAtOffsetList& list) {
        uint64_t registers = 0;
        for (unsigned i = 0; i < list.registerCount(); ++i)
            registers |= 1ULL << list.at(i).reg().index();
        return registers == mask && (!mask || list.at(0).offset() == offsetOfFirst);
    };

    Locker locker { lock };
    auto& candidates = lists->add(key, Vector<std::unique_ptr<RegisterAtOffsetList>, 1>()).iterator->value;
    for (auto& candidate : candidates) {
        if (matchCase(*candidate))
            return candidate.get();
    }
    RegisterSet registers;
    for (unsigned index = 0; index < 64; ++index) {
        if (mask >> index & 1)
            registers.add(Reg::fromIndex(index), IgnoreVectors);
    }
    auto list = makeUnique<RegisterAtOffsetList>(registers);
    if (mask)
        list->adjustOffsets(offsetOfFirst - list->at(0).offset());
    RELEASE_ASSERT(matchCase(*list));
    candidates.append(WTF::move(list));
    return candidates.last().get();
}

JITCode::JITCode(void* code, const ImageFunction& function, Way way)
    : JSC::JITCode(JITType::AOTJIT, CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(stubAddress(stubFor(way)))), ShareAttribute::Shared)
    , m_code(code)
    , m_entry(EntryWord::encode(code, function.convention()))
    , m_function(&function)
    , m_calleeSaveRegisters(calleeSaveRegistersOf(function))
{
}

JITCode::~JITCode() = default;

CodePtr<JSEntryPtrTag> JITCode::addressForCall(ArityCheckMode)
{
    return m_addressForCall;
}

void* JITCode::executableAddressAtOffset(size_t offset) { return static_cast<uint8_t*>(m_code) + offset; }
void* JITCode::dataAddressAtOffset(size_t offset) { return static_cast<uint8_t*>(m_code) + offset; }
unsigned JITCode::offsetOf(void* pointer) { return static_cast<uint8_t*>(pointer) - static_cast<uint8_t*>(m_code); }
unsigned JITCode::codeSize() const { return Image::of(*m_function).sizeOfCodeOf(*m_function); }
unsigned JITCode::frameSizeInBytes() const { return Image::of(*m_function).frameOf(*m_function).frameSizeInBytes(); }
size_t JITCode::size() { return codeSize(); }

bool JITCode::contains(void* address)
{
    return address >= m_code && address < static_cast<uint8_t*>(m_code) + codeSize();
}

} } // namespace JSC::AOT

extern "C" JS_EXPORT_PRIVATE bool JSC__AOT__entryPointIsCommonJS(JSC::JSGlobalObject*);
extern "C" bool JSC__AOT__entryPointIsCommonJS(JSC::JSGlobalObject* globalObject)
{
    JSC::AOT::Instance* instance = globalObject->aotInstance();
    return instance && instance->typed && instance->typed->entryPointIsCommonJS;
}

#endif // ENABLE(AOT)
