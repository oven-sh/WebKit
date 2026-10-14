/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntime.h"
#include "CompilerHooks.h"
#include "HeapIterationScope.h"
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
#include "AOTTypedObject.h"
#include "CCallHelpers.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "DFGOperations.h"
#include "HeapAnalyzer.h"
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
#include <wtf/ProcessID.h>
#include <wtf/StackTrace.h>
#include <wtf/StringPrintStream.h>
#include <wtf/TZoneMallocInlines.h>

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

extern "C" void* g_aotStaticFunctionEntrypoints[3];

template<typename Wanted, typename Function> struct TakesFirst : std::false_type { };
template<typename Wanted, typename Result, typename First, typename... Rest> struct TakesFirst<Wanted, Result(First, Rest...)> : std::is_same<Wanted, First> { };
template<typename Wanted, typename Result, typename First, typename... Rest> struct TakesFirst<Wanted, Result(First, Rest...) noexcept> : std::is_same<Wanted, First> { };

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
    set(Entry::LookupExceptionHandler, tagCFunctionPtr<void*, OperationPtrTag>(operationLookupExceptionHandler));
    set(Entry::ThrowStackOverflowError, tagCFunctionPtr<void*, OperationPtrTag>(operationAOTThrowStackOverflowError));
    set(Entry::ThrowCalledIndirectlyError, tagCFunctionPtr<void*, OperationPtrTag>(operationAOTThrowCalledIndirectlyError));
    set(Entry::NativeCallTrampoline, LLInt::getCodePtr<JSEntryPtrTag>(llint_native_call_trampoline).taggedPtr());
    set(Entry::InternalFunctionCallTrampoline, LLInt::getCodePtr<JSEntryPtrTag>(llint_internal_function_call_trampoline).taggedPtr());
    set(Entry::InternalFunctionConstructTrampoline, LLInt::getCodePtr<JSEntryPtrTag>(llint_internal_function_construct_trampoline).taggedPtr());
    set(Entry::EnterStaticFunctionForCall, tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::EnterStaticFunctionForCall)));
    set(Entry::EnterStaticFunctionForConstruct, tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::EnterStaticFunctionForConstruct)));
    g_aotStaticFunctionEntrypoints[0] = m_entries[static_cast<unsigned>(Entry::EnterStaticFunctionForCall)];
    set(Entry::ConstructViaCall, tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::ConstructViaCall)));
    g_aotStaticFunctionEntrypoints[1] = m_entries[static_cast<unsigned>(Entry::EnterStaticFunctionForConstruct)];
    g_aotStaticFunctionEntrypoints[2] = m_entries[static_cast<unsigned>(Entry::ConstructViaCall)];

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

    if (usesStubs)
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

void RuntimeTable::countForFamily(const char* name, const char* nameByFamily, uint16_t family, const char* detail)
{
    countOperation(name, detail);
    UTF8CString& detailByFamily = m_detailsByFamily.add(std::pair { detail, static_cast<unsigned>(family) }, UTF8CString()).iterator->value;
    if (detailByFamily.isNull())
        detailByFamily = toUTF8CString(static_cast<unsigned>(family), ":", detail);
    countOperation(nameByFamily, byteCast<char>(detailByFamily.data()));
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

void countJobCall(VM& vm, bool entersStaticCode)
{
    if (!vm.m_aotRuntimeTable)
        return;
    runtimeTable(vm).countOperation("job-call", entersStaticCode ? "static-code" : "other");
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
    UncheckedKeyHashMap<uint32_t, Structure*> knownShapes;
    UncheckedKeyHashMap<uint32_t, Structure*> emptyStructuresWithoutPrototype;
    UncheckedKeyHashMap<Structure*, Structure*> copyStructures;
    UncheckedKeyHashMap<std::pair<Structure*, uint64_t>, Instance::CopyForMore> copiesForMore;
    UncheckedKeyHashMap<std::tuple<Structure*, UniquedStringImpl*, uintptr_t>, Instance::CopyExcluding> copiesExcluding;
    Vector<RefPtr<UniquedStringImpl>> namesOfCopiesExcludingThatWent;
    std::pair<Structure*, uint64_t> keyOfLastCopyForMore { nullptr, 0 };
    const Instance::CopyForMore* lastCopyForMore { nullptr };
    UncheckedKeyHashMap<std::pair<Structure*, uint16_t>, Structure*> convertedStructures;
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
    struct LayoutConversionPlan {
        Vector<std::pair<PropertyOffset, uint16_t>> moves;
        Vector<const TypedLayoutTable::Field*> fields;
    };
    UncheckedKeyHashMap<std::pair<Structure*, uint16_t>, LayoutConversionPlan> conversionPlans;
    UncheckedKeyHashMap<std::pair<Structure*, uint16_t>, ASCIILiteral> rejectedConversions;
    UncheckedKeyHashMap<uint32_t, Structure*> emptyStructures;
    UncheckedKeyHashMap<uint32_t, JSFunction*> constructorsOfLayouts;
    JSModuleLoader* loader { nullptr };
    Vector<std::pair<Structure*, Structure*>, 12> functionStructures;
    Vector<std::pair<Structure*, Structure*>, 6> functionStructuresWithCaptures;
    UncheckedKeyHashMap<uint32_t, ScriptExecutable*> topLevelExecutables;
    String retainedString;
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
static_assert(Instance::offsetOfDepartedFamily(Instance::numberOfFamiliesWithByte) <= 4096);
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

void noteDepartureFromFamily(VM& vm, uint16_t family, const char* road)
{
#if ASSERT_ENABLED
    vm.m_aotFamilyWhoseDepartureWasNoted = family;
#endif
    Instance::noteDepartedFamily(vm, family);
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countForFamily("Family::departed", "Family::departed-from", family, road);
}

bool hasNotedDepartureFromFamily(VM& vm, uint16_t family)
{
#if ASSERT_ENABLED
    if (std::exchange(vm.m_aotFamilyWhoseDepartureWasNoted, 0) != family)
        return false;
#endif
    if (!Instance::hasByteForFamily(family))
        return true;
    for (Instance* instance : vm.m_aotInstances) {
        if (!instance->departedFamilies[family])
            return false;
    }
    return true;
}

void didClearLoaderOf(Instance* instance)
{
    if (instance->globalObject->aotInstance() != instance)
        instance->collections->loaderWasCleared = true;
}

bool Instance::loaderWasCleared() const { return collections->loaderWasCleared; }

void Instance::noteDepartedFamily(VM& vm, uint16_t family)
{
    if (!hasByteForFamily(family))
        return;
    for (Instance* instance : vm.m_aotInstances)
        instance->departedFamilies[family] = 1;
}

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
    return instance;
}

Instance* Instance::of(JSFunction* function)
{
    Structure* structure = function->structure();
    if (Instance* instance = structure->aotInstance())
        return instance;
    return &ensure(structure->realm());
}

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
    instance->keyedStores = instance->keyedHas + numberOfKeyedHas;
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
    instance->stringConstantRecords = program->data().at<uint32_t>(program->data().stringConstantRecordsOffset);
    instance->infos = program->data().infos();
    instance->addressOfStates = instance->states;
    instance->functionMetadataOffsets = program->data().functionMetadataOffsets();
    instance->programIdentifiers = program->identifiers();
    instance->programIdentifierPlaces = program->identifierPlaces();
    instance->sharedData = SharedData::get();
    instance->sharedDataOfLinkedFunctions = instance->sharedData;
    instance->fieldsWithObservableReads = static_cast<uint8_t*>(OSAllocator::reserveAndCommit(sizeOfFieldsWithObservableReads, OSAllocator::FastMallocPages));
    if (Image* image = Image::withCode()) {
        instance->image = image->at<uint8_t>(0);
        instance->code = static_cast<const uint8_t*>(image->code());
        instance->codeGranules = image->at<uint32_t>(image->header().codeGranulesOffset);
        instance->subsequentFunctionStarts = image->at<uint32_t>(image->header().functionStartsOffset) + 1;
    }
    instance->structureIDBase = JSC::structureIDBase();
    instance->cellCageBase = MarkedBlockCage::base();
    instance->numberBoxBase = Typed::NumberBoxTable::base();
    instance->familyBase = instance->structureIDBase + Structure::offsetOfFamily();
    instance->fieldIDAtDirectLocationBase = instance->structureIDBase + Structure::offsetOfFieldIDInSlot() - JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) * sizeof(uint16_t);
    for (unsigned slot = 0; slot < Structure::numberOfSlotsWithFieldIDs; ++slot)
        instance->fieldIDInSlotBases[slot] = instance->structureIDBase + Structure::offsetOfFieldIDInSlot() + slot * sizeof(uint16_t);
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
        if (usesStubs)
            boundFunctionExecutable->setCallEntrypoint(CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::CallBoundFunction))));
        instance->boundFunctionExecutable = boundFunctionExecutable;
        instance->boundFunctionClassInfo = JSBoundFunction::info();
        for (IndexingType type : { ArrayWithUndecided, ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous, ArrayWithArrayStorage, CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithDouble, CopyOnWriteArrayWithContiguous })
            instance->originalArrayStructureIDs[(type & (IndexingShapeMask | CopyOnWrite)) >> Instance::arrayKindShift] = idOf(globalObject->originalArrayStructureForIndexingType(type));
        for (unsigned vectorLength = 1; vectorLength <= Instance::maxVectorLengthGrownByStub; ++vectorLength) {
            unsigned next = Butterfly::optimalContiguousVectorLength(static_cast<size_t>(0), static_cast<unsigned>(nextLength(vectorLength + 1)));
            instance->nextVectorLengths[vectorLength] = next <= UINT8_MAX && next & 1 ? next : 0;
        }
        if (!globalObject->isHavingABadTime()) {
            instance->regExpMatchesArrayStructureIDs[0] = idOf(globalObject->regExpMatchesArrayStructure());
            instance->regExpMatchesArrayStructureIDs[1] = idOf(globalObject->regExpMatchesArrayWithIndicesStructure());
            instance->newArrayWithInt32Header = headerOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithInt32));
            instance->newArrayWithDoubleHeader = headerOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithDouble));
            instance->newArrayWithContiguousHeader = headerOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous));
            instance->newCopyOnWriteArrayHeaders[0] = headerOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithInt32));
            instance->newCopyOnWriteArrayHeaders[1] = headerOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithDouble));
            instance->newCopyOnWriteArrayHeaders[2] = headerOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithContiguous));
        }
        instance->collections->arraysLackIsConcatSpreadable.install(globalObject->arrayIsConcatSpreadableWatchpointSet(), instance->arraysLackIsConcatSpreadable);
        instance->collections->arraysLackInheritedElements.install(globalObject->arrayPrototypeChainIsSaneWatchpointSet(), instance->arraysLackInheritedElements);
        instance->activationHeader = headerOf(globalObject->activationStructure());
        instance->auxiliarySpace = &vm.auxiliarySpace();
        instance->cellSpace = &vm.cellSpace();
        instance->activationSpace = subspaceFor<JSLexicalEnvironment>(vm);
        instance->arrayAllocator = subspaceFor<JSArray>(vm)->allocatorFor(sizeof(JSArray), AllocatorForMode::EnsureAllocator).localAllocator();
        instance->ropeStringAllocator = subspaceFor<JSRopeString>(vm)->allocatorFor(sizeof(JSRopeString), AllocatorForMode::EnsureAllocator).localAllocator();
        RELEASE_ASSERT(static_cast<LocalAllocator*>(instance->arrayAllocator)->cellSize() == sizeof(JSArray));
        RELEASE_ASSERT(static_cast<LocalAllocator*>(instance->ropeStringAllocator)->cellSize() == sizeof(JSRopeString));
        instance->singleCharacterStrings = vm.smallStrings.singleCharacterStrings();
        instance->emptyString = vm.smallStrings.emptyString();
        instance->sentinelString = vm.smallStrings.sentinelString();
        instance->arrayIterationSentinel = vm.fastArraySentinel();
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
    if (Image* image = Image::withShapes()) {
        RELEASE_ASSERT(!image->header().intrinsicHash || image->header().intrinsicHash == ImmutableIntrinsics::shared()->hash());
    }
    if (JSValue call = globalObject->linkTimeConstant(LinkTimeConstant::callFunction); call.isCell())
        instance->functionPrototypeCall = call.asCell();
    instance->collections->token = Symbol::create(vm);
    if (Image* image = Image::withCode(); image && image->header().typedDataSize)
        instance->typed = new Typed::State(vm, *instance, *image->at<Typed::Data>(image->header().typedDataOffset));
    loader->setAOTInstance(instance);
    if (instance->typed)
        instance->typed->adoptClosuresOfRealm(vm);
    if (!vm.m_aotInstances.isEmpty())
        memcpySpan(std::span { instance->departedFamilies }, std::span { vm.m_aotInstances[0]->departedFamilies });
    vm.traps().registerMirror(instance->stackMirror);
    instance->barrierThreshold = vm.heap.barrierThreshold();
    instance->mutatorShouldBeFenced = vm.heap.mutatorShouldBeFenced();
    vm.m_aotInstances.append(instance);
    microtaskQueueingDidChange(vm);
    return *instance;
}

void microtaskQueueingDidChange(VM& vm)
{
    for (Instance* instance : vm.m_aotInstances) {
        instance->microtaskQueue = &instance->globalObject->microtaskQueue();
        instance->awaitMustSuspend = !instance->globalObject->canFastQueueMicrotask() || vm.hostObservesMicrotaskBoundaries();
    }
}

static ScriptExecutable* topLevelExecutableOf(Data& data)
{
    if (auto* function = dynamicDowncast<FunctionExecutable>(data.executable()); function && function->hasAOTEntry()) {
        uint32_t moduleID = function->sourceProvider()->aotModuleID();
        if (ScriptExecutable* result = data.instance->topLevelExecutableOf(moduleID))
            return result;
        const ProgramModule* module = ProgramData::get()->moduleWithEntryOffset(moduleID - 1);
        RELEASE_ASSERT(module && module->isBuiltinFunction && module->hasExecutable());
        return data.instance->program->executable(module->executableIndex);
    }
    return data.executable()->topLevelExecutable();
}

Structure* Instance::functionStructure(Structure* realmStructure, FunctionExecutable* executable, JSScope* scope)
{
    if (executable->isBuiltinFunction() && !(executable->unlinkedExecutable()->isBuiltinDefaultClassConstructor() && &instanceOf(scope) == this))
        return realmStructure;
    return functionStructure(realmStructure);
}

Structure* Instance::functionStructure(Structure* realmStructure)
{
    for (auto& [from, to] : collections->functionStructures) {
        if (from == realmStructure)
            return to;
    }
    RELEASE_ASSERT(!realmStructure->didTransition() && !realmStructure->aotInstance());
    DeferGC deferGC(*vm);
    Structure* result = Structure::create(*vm, globalObject, realmStructure->storedPrototype(), realmStructure->typeInfo(), realmStructure->classInfoForCells(), realmStructure->indexingModeIncludingHistory(), realmStructure->inlineCapacity());
    result->setAOTInstance(this);
    collections->functionStructures.append({ realmStructure, result });
    return result;
}

JSFunction* Instance::makeFunction(FunctionExecutable* executable, JSScope* scope)
{
    if (isAsyncGeneratorWrapperParseMode(executable->parseMode()))
        return JSAsyncGeneratorFunction::create(*vm, globalObject, executable, scope, functionStructure(globalObject->asyncGeneratorFunctionStructure(), executable, scope));
    if (isGeneratorWrapperParseMode(executable->parseMode()))
        return JSGeneratorFunction::create(*vm, globalObject, executable, scope, functionStructure(globalObject->generatorFunctionStructure(), executable, scope));
    if (isAsyncFunctionWrapperParseMode(executable->parseMode()))
        return JSAsyncFunction::create(*vm, globalObject, executable, scope, functionStructure(globalObject->asyncFunctionStructure(), executable, scope));
    return JSFunction::create(*vm, globalObject, executable, scope, functionStructure(JSFunction::selectStructureForNewFuncExp(globalObject, executable), executable, scope));
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
    if (typed)
        typed->ensureStaticsOfProgramAreFilled();
    if (typed && !typed->moduleRecord && scope->type() == ModuleEnvironmentType && typed->data.isEntryPoint(executable->programModule()->entryOffset)) {
        // An object of the program says what it is laid out as, not whose it is, and compiled code takes the variables of the modules from the Instance it runs in.
        for (Instance* other : vm.m_aotInstances) {
            if (other == this || !other->typed || !other->typed->moduleRecord)
                continue;
            throwSyntaxError(globalObject, throwScope, makeString("The module "_s, executable->source().provider()->sourceURL(), " was compiled ahead of time and has been loaded already: another realm or loader cannot load it again"_s));
            return 0;
        }
        typed->moduleRecord = uncheckedDowncast<JSModuleEnvironment>(scope)->moduleRecord();
    }
    if (Data* data = dataIfExists(index); data && data->executable() == executable)
        return data->code()->entry();
    DeferGCForAWhile deferGC(vm);
    ImageCode found = findInImage(executable, CodeSpecializationKind::CodeForCall, nullptr, scope);
    if (!found) {
        throwSyntaxError(globalObject, throwScope, makeString("The module "_s, executable->source().provider()->sourceURL(), " was compiled ahead of time and cannot be run the way it has been loaded"_s));
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
    instance->vm->traps().unregisterMirror(instance->stackMirror);
    instance->vm->m_aotInstances.removeFirst(instance);
    instance->vm->m_aotInstancesToDestroy.removeFirst(instance);
    while (!instance->collections->all.isEmpty())
        Data::destroy(instance->collections->all.last());
    size_t environmentsSize = instance->collections->environmentsSize;
    size_t size = instance->collections->sizeFromInstance;
    OSAllocator::decommitAndRelease(instance->collections->startsOfData, instance->collections->sizeOfStartsOfData);
    fastFree(instance->cachedReturnAddresses);
    delete instance->collections;
    fastFree(instance->customGetters);
    OSAllocator::decommitAndRelease(instance->fieldsWithObservableReads, sizeOfFieldsWithObservableReads);
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
    if (!function)
        return nullptr;
    // What was compiled from types has no bytecode to make a code block of.
    if (!function.unlinkedCodeBlockIfExists() && !(function.metadata() && function.metadata()->find(FunctionMetadata::Scalars)))
        return function.instance->globalObject;
    return function.ensureCodeBlock();
}

bool hasCompiledCode(VM& vm, JSFunction* function)
{
    if (function->hasAOTFunctionWord())
        return true;
    if (function->isHostFunction())
        return false;
    FunctionExecutable* executable = function->jsExecutable();
    return executable->hasAOTEntry() || FunctionRef::of(vm, executable, CodeSpecializationKind::CodeForCall, nullptr) || FunctionRef::of(vm, executable, CodeSpecializationKind::CodeForConstruct, nullptr);
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
#if CPU(X86_64)
        registers.add(X86Registers::ebx, IgnoreVectors);
        registers.add(X86Registers::r12, IgnoreVectors);
#endif
        list.construct(registers);
        list->adjustOffsets(offsetOfInstanceRegisterInAdapter - list->find(instanceGPR)->offset());
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
        *data->linkage() = { codeBlock, executable, unlinkedCodeBlock, code, nullptr };

    RELEASE_ASSERT(function.index < instance.collections->numberOfFunctions);
    RELEASE_ASSERT(!instance.dataIfExists(function.index));
    instance.setData(function.index, data);
    data->indexInAllList = instance.collections->all.size();
    instance.collections->all.append(data);
    data->noteFilled();

    RELEASE_ASSERT(info.sitesInImage() == data->sites && info.numberOfSlotsForMisses() == std::min<uint32_t>(numSlots, FunctionInfo::maxEncodedSlots) && (!info.hasExecutable() || instance.program->executable(info.indexPlusOne() - 1) == executable));
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
    // What was compiled from types has no executable: only its caches live here.
    FunctionExecutable* executable = info.hasExecutable() ? program->executable(info.indexPlusOne() - 1) : nullptr;
    RELEASE_ASSERT(executable || !info.isTopLevelCode());
    if (!executable)
        return Data::create(*this, *info.function());
    UnlinkedFunctionCodeBlock* unlinkedCodeBlock = nullptr;
    Ref<JITCode> code = executable->aotIndexFor(info.kind()) == index && executable->hasJITCodeFor(info.kind()) ? Ref { static_cast<JITCode&>(executable->generatedJITCodeFor(info.kind()).get()) } : jitCodeForImageFunction({ &Image::of(*info.function()), info.function() }, info.kind());
    RELEASE_ASSERT(code->index() == index);
    code->setInstance(*this);
    data = Data::create(*this, executable, unlinkedCodeBlock, code.get());
    RELEASE_ASSERT(data);
    return data;
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
    auto& info = FunctionRef { this, asked.function }.info();
    place.state = PlaceInUnwinding::IsNotLeft;
    if (!info.isTyped || !info.hasNoHandlers || typedFunctionRejectsWhatItThrows(info.typedFunction()))
        return place;
    const ImageFrame& frame = Image::of(*info.function()).frameOf(*info.function());
    for (uint64_t mask = ImageFunction::unpackRegisters(frame.calleeSaveRegisters); mask; mask &= mask - 1) {
        unsigned index = WTF::ctz(mask);
        if (RegisterSet::stackRegisters().contains(Reg::fromIndex(index), IgnoreVectors) || RegisterSet::vmCalleeSaveBufferSlotsByRegIndex()[index] < 0)
            return place;
    }
    place.savedRegisters = frame.calleeSaveRegisters;
    place.savedRegistersStart = frame.calleeSavesStart;
    place.state = PlaceInUnwinding::IsLeftByWhateverIsThrown;
    return place;
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

BytecodeIndex FunctionRef::bytecodeIndexAt(const void* returnAddress, const void* frame) const
{
    return locationForReturnAddress(returnAddress, frame).bytecodeIndex;
}

FunctionRef FunctionRef::of(VM& vm, ScriptExecutable* scriptExecutable, CodeSpecializationKind kind, JSCell* instanceToken)
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
    auto* executable = uncheckedDowncast<FunctionExecutable>(scriptExecutable);
    if (executable->hasAOTEntry()) {
        if (!executable->aotEntryFor(kind) || executable->aotIndexFor(kind) == FunctionExecutable::aotConstructViaCallIndex)
            return { };
        return { instanceWithToken(), executable->aotIndexFor(kind) };
    }
    if (!executable->hasJITCodeFor(kind) || executable->generatedJITCodeFor(kind)->jitType() != JITType::AOTJIT)
        return { };
    Ref generated = executable->generatedJITCodeFor(kind);
    auto& code = static_cast<JITCode&>(generated.get());
    return { code.instance(), code.index() };
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
    const FunctionInfo& info = this->info();
    return info.hasExecutable() && instance ? instance->program->executable(info.indexPlusOne() - 1) : nullptr;
}

ScriptExecutable* FunctionRef::executableIfExists() const
{
    if (Data* data = dataIfExists())
        return data->executable();
    const FunctionInfo& info = this->info();
    return info.hasExecutable() && instance ? instance->program->executableIfExists(info.indexPlusOne() - 1) : nullptr;
}

bool FunctionRef::hasExecutable() const
{
    if (Data* data = dataIfExists())
        return !!data->executable();
    return info().hasExecutable() && instance;
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
    if (hasEntryForEveryOffset)
        states.append(start);
    unsigned frame = 0;
    for (uint64_t i = hasEntryForEveryOffset ? 1 : 0; i < count; ++i) {
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

SUPPRESS_ASAN JSCell* FunctionRef::calleeInFrame(const void* frame) const
{
    const ImageFunction* function = info().function();
    unsigned start = function ? Image::of(*function).frameOf(*function).calleeStart : 0;
    return start ? *(static_cast<JSCell* const*>(frame) - start) : nullptr;
}

template<typename Number>
static const UnlinkedHandlerInfo* handlerInRow(std::span<const uint8_t> row, unsigned site, UnlinkedHandlerInfo& madeHere)
{
    const uint8_t* endOfRow = row.data() + row.size();
    for (const uint8_t* at = row.data() + 1; at < endOfRow; at += 3 * sizeof(Number)) {
        Number behindLast;
        memcpy(&behindLast, at + sizeof(Number), sizeof(Number));
        if (site >= behindLast)
            continue;
        std::array<Number, 3> handler;
        memcpy(handler.data(), at, sizeof(handler));
        if (site < handler[0])
            return nullptr;
        madeHere = UnlinkedHandlerInfo(handler[0], handler[1], handler[2], HandlerType::Catch);
        return &madeHere;
    }
    return nullptr;
}

const UnlinkedHandlerInfo* FunctionRef::handlerFor(unsigned bytecodeOffset, UnlinkedHandlerInfo& madeHere) const
{
    if (auto row = programData().handlersOfTypedFunction(index); !row.empty()) {
        if (row[0] == 1)
            return handlerInRow<uint8_t>(row, bytecodeOffset, madeHere);
        if (row[0] == 2)
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

static std::span<const uint32_t> functionsIn(const FunctionMetadata& metadata, FunctionMetadata::Section which)
{
    const uint32_t* words = metadata.find(which);
    if (!words)
        return { };
    return { ProgramData::get()->at<uint32_t>(words[0]), words[1] };
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
        if (linkage->functions)
            fastFree(linkage->functions);
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
    auto& [codeBlock, executable, unlinkedCodeBlock, code, functions] = *linkage();
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
    SourceProvider& provider = *executable->sourceProvider();
    unsigned sourceOffset = executable->source().startOffset();
    return provider.documentLineColumn(provider.lineColumnInTextForOffset(sourceOffset));
}

static FunctionExecutable* functionOf(Data& data, unsigned index, uint32_t entry)
{
    RELEASE_ASSERT(entry);
    VMProgram& program = *data.instance->program;
    if (!(entry & 1))
        return program.executable((entry >> 1) - 1);
    RELEASE_ASSERT(data.hasLinkage);
    FunctionExecutable**& functions = data.linkage()->functions;
    if (!functions) {
        const FunctionMetadata& metadata = *data.function().metadata();
        functions = static_cast<FunctionExecutable**>(fastZeroedMalloc((functionsIn(metadata, FunctionMetadata::FunctionDecls).size() + functionsIn(metadata, FunctionMetadata::FunctionExprs).size()) * sizeof(FunctionExecutable*)));
    }
    FunctionExecutable*& function = functions[index];
    if (!function) {
        ScriptExecutable* executable = data.executable();
        function = program.unlinkedFunction((entry >> 1) - 1, false)->link(*data.instance->vm, topLevelExecutableOf(data), executable->source(), std::nullopt, NoIntrinsic, executable->isInsideOrdinaryFunction());
        if (!data.hasBeenFilledSinceLastCollection)
            data.noteFilled();
    }
    return function;
}

FunctionExecutable* Data::functionDecl(unsigned index)
{
    return functionOf(*this, index, functionsIn(*function().metadata(), FunctionMetadata::FunctionDecls)[index]);
}

FunctionExecutable* Data::functionExpr(unsigned index)
{
    const FunctionMetadata& metadata = *function().metadata();
    return functionOf(*this, functionsIn(metadata, FunctionMetadata::FunctionDecls).size() + index, functionsIn(metadata, FunctionMetadata::FunctionExprs)[index]);
}

FunctionExecutable* FunctionRef::functionDecl(unsigned index) const
{
    if (uint32_t entry = functionsIn(*metadata(), FunctionMetadata::FunctionDecls)[index]; !(entry & 1))
        return instance->program->executable((entry >> 1) - 1);
    return ensureData()->functionDecl(index);
}

FunctionExecutable* FunctionRef::functionExpr(unsigned index) const
{
    if (uint32_t entry = functionsIn(*metadata(), FunctionMetadata::FunctionExprs)[index]; !(entry & 1))
        return instance->program->executable((entry >> 1) - 1);
    return ensureData()->functionExpr(index);
}

bool install(VM& vm, FunctionExecutable* executable, CodeSpecializationKind kind, UnlinkedCodeBlock* unlinkedCodeBlock, JSScope* scope, Ref<JITCode>&& code)
{
    Instance& instance = instanceOf(scope);
    code->setInstance(instance);
    uint32_t index = code->index();
    if (instance.isLinked(index))
        RELEASE_ASSERT((FunctionRef { &instance, index }.executable() == executable));
    else if (!Data::create(instance, executable, unlinkedCodeBlock, code.get()))
        return false;
    executable->installAOTCode(vm, kind, WTF::move(code));
    return true;
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

bool linkColdStaticFunction(Instance* instance, uint32_t index, JSScope* scope)
{
    if (scope->realm() != instance->globalObject)
        return false;
    if (instance->isLinked(index))
        return true;
    const FunctionInfo& info = instance->infos[index];
    if (!info.startsCold() || (info.function()->usesStaticImports && !moduleIsLinkedAsCompiled(scope)))
        return false;
    instance->setLinkedWithoutData(index);
    return true;
}

bool linkStaticFunction(Instance* instance, FunctionExecutable* executable, CodeSpecializationKind kind, JSScope* scope)
{
    if (scope->realm() != instance->globalObject)
        return false;
    uint32_t index = executable->aotIndexFor(kind);
    if (index == FunctionExecutable::aotConstructViaCallIndex)
        return true;
    if (instance->isLinked(index))
        return true;
    if (const FunctionInfo& info = instance->infos[index]; info.startsCold()) {
        RELEASE_ASSERT(info.hasExecutable() && info.kind() == kind);
        if (info.function()->usesStaticImports && !moduleIsLinkedAsCompiled(scope))
            return false;
        instance->setLinkedWithoutData(index);
        return true;
    }
    UnlinkedFunctionCodeBlock* unlinkedCodeBlock = nullptr;
    ImageCode found = findInImage(executable, kind, unlinkedCodeBlock, scope);
    if (!found)
        return false;
    Ref<JITCode> code = jitCodeForImageFunction(found, kind);
    RELEASE_ASSERT(code->index() == executable->aotIndexFor(kind));
    code->setInstance(*instance);
    return !!Data::create(*instance, executable, unlinkedCodeBlock, code.get());
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
    auto& [codeBlock, executable, unlinkedCodeBlock, code, functions] = *linkage();
    if (executable)
        visitor.appendUnbarriered(executable);
    if (unlinkedCodeBlock)
        visitor.appendUnbarriered(unlinkedCodeBlock);
    if (codeBlock)
        visitor.appendUnbarriered(codeBlock);
    if (functions) {
        const FunctionMetadata& metadata = *function().metadata();
        for (unsigned i = functionsIn(metadata, FunctionMetadata::FunctionDecls).size() + functionsIn(metadata, FunctionMetadata::FunctionExprs).size(); i--;) {
            if (functions[i])
                visitor.appendUnbarriered(functions[i]);
        }
    }
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
    for (auto& [from, to] : collections->functionStructures)
        visitor.appendUnbarriered(to);
    for (auto& [from, to] : collections->functionStructuresWithCaptures)
        visitor.appendUnbarriered(to);
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
    for (Structure* structure : arrayInTheMakingStructures)
        visitor.appendUnbarriered(structure);
    visitor.appendUnbarriered(bufferStructure);
    visitor.appendUnbarriered(bufferAccessorHolder);
    visitor.appendUnbarriered(collections->token);
    if (!visitsEverything) {
        for (JSCell* cell : collections->cellsAddedSinceLastCollection)
            visitor.appendUnbarriered(cell);
        return;
    }
    for (Structure* structure : collections->knownShapes.values())
        visitor.appendUnbarriered(structure);
    for (Structure* structure : collections->emptyStructures.values())
        visitor.appendUnbarriered(structure);
    for (JSFunction* constructor : collections->constructorsOfLayouts.values())
        visitor.appendUnbarriered(constructor);
    for (auto& [from, to] : collections->convertedStructures) {
        visitor.appendUnbarriered(from.first);
        if (to)
            visitor.appendUnbarriered(to);
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

JSObject* Instance::newObjectOf(VM& vm, Structure* structure)
{
    unsigned outOfLineCapacity = structure->outOfLineCapacity();
    if (!outOfLineCapacity)
        return constructEmptyObject(vm, structure);
    DeferGC deferGC(vm);
    Butterfly* butterfly = Butterfly::create(vm, nullptr, 0, outOfLineCapacity, false, IndexingHeader(), 0);
    gcSafeZeroMemory(std::bit_cast<EncodedJSValue*>(butterfly->propertyStorage() - outOfLineCapacity), outOfLineCapacity * sizeof(EncodedJSValue));
    return JSFinalObject::createWithButterfly(vm, structure, butterfly);
}

bool Instance::convertToTypedLayout(VM& vm, JSObject* object, uint16_t layoutID)
{
    Structure* old = object->structure();
    unsigned capacity = TypedLayoutTable::numberOfSlots(layoutID);
    auto no = [&](ASCIILiteral why) {
        TypedLayoutTable::s_lastConversionFailure = why;
        if (Options::useAOTOperationCounters()) [[unlikely]]
            AOT::runtimeTable(vm).countOperation("Instance::convertToTypedLayout", why.characters());
        return false;
    };
    if (object->type() != FinalObjectType)
        return no("it is not a plain object"_s);
    if (old->typedLayoutID())
        return no("it already has the layout of another type"_s);
    if (old->hasStableFields())
        return no("its fields may not move"_s);
    if (TypedLayoutTable::isGivenOnlyAtBirth(layoutID))
        return no("the layout is given only at birth"_s);
    if (!capacity)
        return no("the type has no layout"_s);
    unsigned inlineSlots = TypedLayoutTable::inlineSlots(layoutID);
    bool usesFieldIDs = TypedLayoutTable::usesFieldIDs(layoutID);
    if (!usesFieldIDs && TypedLayoutTable::isInstanceLayout(layoutID))
        return no("it was not made by the constructor of the class"_s);
    if (usesFieldIDs) {
        if (old->cannotConvertToTypedLayout())
            return no("objects with its structure cannot be converted"_s);
        capacity = inlineSlots = std::min<unsigned>(inlineSlots, old->inlineCapacity());
        if (!capacity) {
            old->setCannotConvertToTypedLayout();
            return no("it does not have enough inline capacity"_s);
        }
    }
    if (old->hasPolyProto())
        return no("its prototype is not supported"_s);
    if (old->mayBePrototype())
        return no("it is used as a prototype"_s);
    if (old->isBrandedStructure())
        return no("it has private methods"_s);
    if (old->inlineCapacity() < inlineSlots)
        return no("it does not have enough inline capacity"_s);
    if (!old->isStructureExtensible())
        return no("it is not extensible"_s);
    if (old->isDictionary() && old->isUncacheableDictionary())
        return no("it is a dictionary"_s);
    Instance& instance = ensure(old->globalObject());
    if (auto it = instance.collections->rejectedConversions.find({ old, layoutID }); it != instance.collections->rejectedConversions.end())
        return no(it->value);
    auto rejectStructure = [&](ASCIILiteral why) {
        if (!old->isDictionary() && instance.collections->rejectedConversions.size() < 4096) {
            instance.collections->rejectedConversions.add({ old, layoutID }, why);
            instance.collections->hasAddedWeakEntriesSinceLastCollection = true;
        }
        return no(why);
    };
    Vector<std::pair<PropertyOffset, uint16_t>, 16> moves;
    Vector<UniquedStringImpl*, 16> names;
    Vector<uint16_t, 16> slots;
    Vector<const TypedLayoutTable::Field*, 16> fields;
    Vector<unsigned, 16> attributes;
    Vector<const TypedLayoutTable::Field*, 4> accessors;
    if (auto plan = instance.collections->conversionPlans.find({ old, layoutID }); plan != instance.collections->conversionPlans.end()) {
        ASSERT(instance.collections->convertedStructures.contains({ old, layoutID }));
        moves = plan->value.moves;
        fields = plan->value.fields;
    } else {
        BitVector taken;
        unsigned next = capacity;
        bool isPlain = true;
        bool hasSlotConflict = false;
        old->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            auto* field = TypedLayoutTable::findField(vm, layoutID, entry.key());
            if (usesFieldIDs) {
                if (entry.attributes() & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue)) {
                    if (field)
                        accessors.append(field);
                    field = nullptr;
                }
            } else
                isPlain &= !entry.attributes();
            attributes.append(entry.attributes());
            hasSlotConflict |= field && !usesFieldIDs && taken.get(field->slot);
            unsigned slot = field && field->slot < capacity && !taken.get(field->slot) ? field->slot : next++;
            taken.set(slot);
            fields.append(field);
            names.append(entry.key());
            slots.append(safeCast<uint16_t>(slot));
            moves.append({ entry.offset(), safeCast<uint16_t>(slot) });
            return true;
        });
        if (!isPlain)
            return rejectStructure("it has a property that is not a plain data property"_s);
        if (hasSlotConflict)
            return rejectStructure("it has two properties that share a slot"_s);
        for (auto& field : TypedLayoutTable::fieldsOf(layoutID)) {
            if (!usesFieldIDs && !field.mayBeAbsent && !taken.get(field.slot))
                return rejectStructure("it lacks a required property"_s);
        }
        if (JSValue prototype = old->storedPrototype(); prototype.isObject() && asObject(prototype) != old->globalObject()->objectPrototype()) {
            if (usesFieldIDs) {
                if (!old->isDictionary())
                    old->setCannotConvertToTypedLayout();
                return no("its prototype is not supported"_s);
            }
            if (!TypedLayoutTable::tryToInheritFrom(vm, layoutID, prototype))
                return no("it inherits a property that the type declares, or its prototype is not supported"_s);
        }
    }
    Vector<JSValue, 16> values;
    for (unsigned i = 0; i < moves.size(); ++i) {
        JSValue value = object->getDirect(moves[i].first);
        if (fields[i] && TypedLayoutTable::checkStore(*fields[i], value) == TypedLayoutTable::StoreCheck::Rejected)
            return no("the value of a property does not match its declared type"_s);
        values.append(fields[i] ? TypedLayoutTable::toFieldRepresentation(*fields[i], value) : value);
    }
    if (object->structure() != old)
        return object->structure()->typedLayoutID() == layoutID;
    Structure* converted;
    if (auto it = instance.collections->convertedStructures.find({ old, layoutID }); it != instance.collections->convertedStructures.end())
        converted = it->value;
    else {
        DeferGC deferGC(vm);
        Structure* empty = old->storedPrototype().isObject()
            ? old->globalObject()->structureCache().emptyObjectStructureForPrototype(old->globalObject(), asObject(old->storedPrototype()), old->inlineCapacity())
            : Structure::create(vm, old->globalObject(), jsNull(), old->typeInfo(), old->classInfoForCells(), NonArray, old->inlineCapacity());
        converted = empty->inlineCapacity() == old->inlineCapacity() && empty->indexingType() == old->indexingType() ? Structure::createWithProperties(vm, empty, names.span(), slots.span(), capacity, inlineSlots, attributes.span()) : nullptr;
        if (converted)
            converted->copyAccessorAndReadOnlyFlagsFrom(*old);
        if (converted && usesFieldIDs) {
            uint16_t fieldIDInSlot[Structure::numberOfSlotsWithFieldIDs] { };
            for (auto* field : accessors) {
                if (field->slot < Structure::numberOfSlotsWithFieldIDs)
                    fieldIDInSlot[field->slot] = Structure::ambiguousFieldID;
            }
            for (unsigned i = 0; i < fields.size(); ++i) {
                if (auto* field = fields[i]; field && field->slot < Structure::numberOfSlotsWithFieldIDs)
                    fieldIDInSlot[field->slot] = !fieldIDInSlot[field->slot] && field->slot < capacity && slots[i] == field->slot ? field->id : Structure::ambiguousFieldID;
            }
            converted->setTypedLayoutID(layoutID, fieldIDInSlot);
        } else if (converted)
            converted->setTypedLayoutID(layoutID);
        if (!old->isDictionary()) {
            instance.collections->convertedStructures.add({ old, layoutID }, converted);
            instance.noteCellAdded(old);
            instance.noteCellAdded(converted);
            if (converted) {
                Collections::LayoutConversionPlan plan;
                plan.moves = moves;
                plan.fields = fields;
                instance.collections->conversionPlans.add({ old, layoutID }, WTF::move(plan));
            }
        }
    }
    if (!converted)
        return no("it has indexed elements, or no structure could be created"_s);
    if (old->family()) [[unlikely]]
        noteDepartureFromFamily(vm, old->family(), "typed-layout");
    {
        DeferGC deferGC(vm);
        unsigned oldOutside = old->outOfLineCapacity();
        unsigned newOutside = converted->outOfLineCapacity();
        if (newOutside != oldOutside) {
            Butterfly* butterfly = nullptr;
            if (newOutside) {
                butterfly = Butterfly::create(vm, nullptr, 0, newOutside, false, IndexingHeader(), 0);
                gcSafeZeroMemory(std::bit_cast<EncodedJSValue*>(butterfly->propertyStorage() - newOutside), newOutside * sizeof(EncodedJSValue));
            }
            object->nukeStructureAndSetButterfly(vm, object->structureID(), butterfly);
        } else {
            for (unsigned i = 0; i < newOutside; ++i)
                object->locationForOffset(firstOutOfLineOffset + i)->clear();
        }
        for (unsigned offset = 0; offset < old->inlineCapacity(); ++offset)
            object->locationForOffset(offset)->clear();
        for (unsigned i = 0; i < moves.size(); ++i)
            object->putDirectOffset(vm, TypedLayoutTable::offsetOfSlot(moves[i].second, inlineSlots), values[i]);
        object->setStructure(vm, converted);
        vm.writeBarrier(object);
    }
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        const char* birth = old->knownShape() ? "a-literal-of-known-shape" : "something-else";
        for (auto& entry : instance.collections->copiedProperties) {
            if (entry.value.last == old)
                birth = "a-copy";
        }
        AOT::runtimeTable(vm).countOperation("Instance::convertToTypedLayout:born-as", birth);
    }
    return true;
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
    if (JSFunction* constructor = constructorOfObjectsWith(structure))
        learnInlineCapacity(constructor, structure);
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

static const TypedLayoutTable::Field* fieldStoredAt(VM& vm, Structure* structure, UniquedStringImpl* name, PropertyOffset offset)
{
    uint16_t layoutID = structure->typedLayoutID();
    if (!layoutID)
        return nullptr;
    if (!TypedLayoutTable::usesFieldIDs(layoutID))
        return TypedLayoutTable::findField(vm, layoutID, name);
    if (static_cast<unsigned>(offset) >= Structure::numberOfSlotsWithFieldIDs)
        return nullptr;
    uint16_t id = structure->fieldIDInSlot(offset);
    return id && id != Structure::ambiguousFieldID ? &TypedLayoutTable::fieldWithID(offset, id) : nullptr;
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
    bool checksFields = target->typedLayoutID() && TypedLayoutTable::hasTypedFields();
    auto noteStore = [&](unsigned i, PropertyOffset offset, bool isOverwritten) {
        if (!checksFields)
            return;
        auto* field = fieldStoredAt(*vm, last, names[i], offset);
        if (!field && isOverwritten && isInlineOffset(offset) && TypedLayoutTable::fieldTypeInSlot(last->typedLayoutID(), offset))
            last = nullptr;
        else if (field && source->typedLayoutID() == last->typedLayoutID() && field == fieldStoredAt(*vm, source, names[i], result.offsets[i].first))
            field = nullptr;
        result.fieldsToCheck.append(field);
    };
    for (unsigned i = 0; last && i < names.size(); ++i) {
        unsigned attributes;
        PropertyOffset offset = last->get(*vm, names[i], attributes);
        if (isValidOffset(offset)) {
            if (attributes)
                last = nullptr;
            else
                noteStore(i, offset, true);
            continue;
        }
        Structure* next = Structure::addPropertyTransitionToExistingStructure(last, names[i], 0, offset);
        if (!next) {
            DeferredStructureTransitionWatchpointFire deferred(*vm, last);
            next = Structure::addNewPropertyTransition(*vm, last, names[i], 0, offset, PutPropertySlot::UnknownContext, &deferred);
        }
        last = next->isDictionary() ? nullptr : next;
        if (last)
            noteStore(i, offset, false);
    }
    if (!result.fieldsToCheck.containsIf([](auto* field) { return !!field; }))
        result.fieldsToCheck.clear();
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

void Instance::didHaveBadTime()
{
    newArrayWithInt32Header = 0;
    newArrayWithDoubleHeader = 0;
    newArrayWithContiguousHeader = 0;
    zeroSpan(std::span { newCopyOnWriteArrayHeaders });
    zeroSpan(std::span { regExpMatchesArrayStructureIDs });
}

void Instance::noteOwnPropertyPlace(uint32_t structureID, UniquedStringImpl* uid, uint32_t place)
{
    auto& names = collections->namesOfOwnPropertyPlaces;
    if (names.size() >= 4 * numberOfOwnPropertyPlaces) [[unlikely]] {
        zeroSpan(std::span { ownPropertyPlaces });
        hasOwnPropertyPlaces = false;
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
        return !structure->typeInfo().overridesGetOwnPropertySlot() && !TypeInfo::hasStaticPropertyTable(structure->typeInfo().inlineTypeFlags()) && !structure->compiledLayout() && !structure->typedLayoutID() && structure->hasMonoProto();
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
        if (flags != (Slot::isGetter | Slot::pointerIsCell))
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
    for (unsigned i = 0; i < numberOfKeyedStores; ++i) {
        auto& entry = keyedStores[i];
        if (!entry.structureID || entry.offset & Slot::flagsMask & ~Slot::isIndirect)
            continue;
        Structure* structure = entry.structureID.decode();
        JSCell* key = static_cast<JSCell*>(entry.pointer);
        if (!isOrdinary(structure))
            continue;
        auto uid = uidOf(key);
        unsigned attributes;
        PropertyOffset own = placeIn(structure, uid.get(), attributes);
        if (!isAt(entry.offset, own) || attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue))
            fail("keyedStores"_s, i, "the object has no plain value that may be written where the entry says"_s, structure, key);
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

void Instance::clearCachesValidatedByMegamorphicCacheEpoch(bool isAtEndOfCollection)
{
    if (customGetters) {
        for (auto& record : std::span { customGetters, numberOfCustomGetters }) {
            uint32_t lastSerial = record.lastSerial;
            record = { };
            record.lastSerial = lastSerial;
        }
    }
    zeroSpan(std::span { inheritedSetters });
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
    }
    if (std::exchange(hasOwnPropertyAnswers, false))
        zeroSpan(std::span { ownPropertyAnswers });
    if (std::exchange(hasOwnPropertyPlaces, false))
        zeroSpan(std::span { ownPropertyPlaces });
    if (std::exchange(hasHeirsOfPrototypes, false))
        zeroSpan(std::span { heirsOfPrototypes });
    if (std::exchange(hasThisAllocations, false))
        zeroSpan(std::span { thisAllocations });
    zeroSpan(std::span { emptyStructuresOfLiterals });
    collections->filledRows.removeAllMatching([&](uint32_t number) {
        if (vm->heap.isMarked(rows[number].structureID.decode()))
            return false;
        zeroBytes(rows[number]);
        return true;
    });
    clearCachesValidatedByMegamorphicCacheEpoch(true);
    takeOutOfKeyedTable(*vm, std::span { keyedStores, numberOfKeyedStores }, true);
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
        collections->rejectedConversions.removeIf([&](auto& entry) {
            return !vm->heap.isMarked(entry.key.first);
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
            collections->allSiteSlots.size(), " sites with several slots (", collections->allSiteSlots.size() * sizeof(PolymorphicSlots), " bytes); ", collections->transitions.size(), " transitions; structures of literals ", collections->shapes.size(), " + ", collections->knownShapes.size(),
            ", of copies ", collections->copyStructures.size(), ", of layouts ", collections->emptyStructures.size(), " + ", collections->convertedStructures.size(), "; property runs ", collections->propertyRunTargets.size(), "; copied properties ", collections->copiedProperties.size(),
            "; constructors ", collections->constructorsByFirstStructure.size(), " + ", collections->learnedInlineCapacities.size(), "; modules ", collections->topLevelExecutables.size(), "; template objects ", collections->templateObjects.size(),
            "; environments ", Image::environmentsSize() / sizeof(void*), "; unlinked functions in the program ", program->data().numberOfUnlinkedFunctions);
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
    , m_entry(function.hasNoGeneralBody ? EntryWord::encode(stubAddress(Stub::ThrowCalledIndirectly), Convention { }) : EntryWord::encode(code, function.convention()))
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

#endif // ENABLE(AOT)
