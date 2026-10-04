/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntime.h"
#include "CompilerHooks.h"
#include "HeapIterationScope.h"
#include "JSModuleLoader.h"

#include "AOTBuiltins.h"
#include "AOTGraph.h"

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
#include "AOTOperationHelpers.h"
#include "AOTOperations.h"
#include "AOTProgram.h"
#include "AOTThunks.h"
#include "CCallHelpers.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "DFGOperations.h"
#include "JITOperations.h"
#include "JITThunks.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include "LLIntData.h"
#include "LLIntEntrypoint.h"
#include "LLIntSlowPaths.h"
#include "LLIntThunks.h"
#include "LinkBuffer.h"
#include "MathObject.h"
#include "MicrotaskQueue.h"
#include "ObjectConstructorInlines.h"
#include "StackFrame.h"
#include "ThunkGenerators.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include <wtf/FileHandle.h>
#include <wtf/FilePrintStream.h>
#include <wtf/FileSystem.h>
#include <wtf/MappedFileData.h>
#include <wtf/ProcessID.h>
#include <wtf/StringPrintStream.h>
#include <wtf/TZoneMallocInlines.h>

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
bool takesGlobalObject(Entry entry) { return takesFirst<JSGlobalObject*>(entry); }

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
    set(Entry::ThrowStackOverflowAtPrologue, tagCodePtr<JITThunkPtrTag>(stubAddress(Stub::ThrowStackOverflowAtPrologue)));
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
    for (unsigned slot = 0; slot < Structure::numberOfSlotsWithFieldIDs; ++slot)
        set(static_cast<Entry>(static_cast<unsigned>(Entry::FieldLayoutIDsInSlot0) + slot), const_cast<uint16_t*>(TypedLayoutTable::fieldLayoutIDsInSlot(slot)));
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
    CString& detail = m_detailsOfSites.add((static_cast<uint64_t>(function) + 1) << 32 | bytecodeOffset, CString()).iterator->value;
    if (detail.isNull())
        detail = toUTF8CString("at:", line, ":", column, ":bc", bytecodeOffset);
    countOperation(name, byteCast<char>(detail.data()));
}

void RuntimeTable::countAllocatedBytes(const char* kind, const Subspace* subspace, size_t cellSize, const ClassInfo* classOfOwner, size_t bytes)
{
    const char* name = "Heap::didAllocate";
    const void* source = subspace ? static_cast<const void*>(subspace) : static_cast<const void*>(classOfOwner);
    CString& detail = m_detailsOfAllocatedBytes.add(std::tuple { kind, source, cellSize }, CString()).iterator->value;
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
    CString& detail = m_detailsOfAllocatedBytes.add(std::tuple { "live", static_cast<const void*>(&subspace), cellSize }, CString()).iterator->value;
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
}

void RuntimeTable::writeOperationCounts() const
{
    const char* path = byteCast<char>(Options::aotTypeCoverageCountsPath());
    if (!Options::useAOTOperationCounters() || !path)
        return;
    String name = makeStringByReplacingAll(String::fromUTF8(path), "%p"_s, String::number(getCurrentProcessID()));
    if (auto file = FilePrintStream::open(byteCast<char>(makeString(name, ".operations"_s).utf8().data()), "w"))
        dumpOperationCounts(*file);
    if (auto file = FilePrintStream::open(byteCast<char>(makeString(name, ".allocations"_s).utf8().data()), "w")) {
        file->println("ANCHOR\t", RawPointer(reinterpret_cast<void*>(&WTFGetBacktrace)));
        for (auto& [stack, entry] : m_allocatedBytesByStack)
            file->println(entry.first, "\t", entry.second, "\t", byteCast<char>(stack.data()));
    }
    if (m_guests.isEmpty())
        return;
    if (auto file = FilePrintStream::open(byteCast<char>(makeString(name, ".guests"_s).utf8().data()), "w")) {
        for (auto& [guest, entry] : m_guests)
            file->println(entry.first, "\t", guest, "\t", entry.second);
    }
}

void RuntimeTable::noteGuest(const char* kind, StringView group, String&& sample)
{
    countOperation("guest", kind);
    auto& entry = m_guests.add(makeString(StringView::fromLatin1(kind), '\t', group), std::pair<uint64_t, String> { }).iterator->value;
    if (!entry.first++)
        entry.second = WTF::move(sample);
}

void Instance::collectCalleeCacheMisses()
{
    const char* const details[numberOfKindsOfCalleeCacheMiss] = { "same-code", "other-code", "other-kind-of-callee", "cache-in-shared-data" };
    for (unsigned i = 0; i < numberOfKindsOfCalleeCacheMiss; ++i) {
        if (uint64_t count = std::exchange(calleeCacheMisses[i], 0))
            AOT::runtimeTable(*vm).countOperation("CallCachedMiss", details[i], count);
    }
}

uint64_t operationCount(VM& vm, StringView name)
{
    for (Instance* instance : vm.m_aotInstances)
        instance->collectCalleeCacheMisses();
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
    for (Instance* instance : vm.m_aotInstances)
        instance->collectCalleeCacheMisses();
    runtimeTable(vm).writeOperationCounts();
    const char* path = byteCast<char>(Options::aotTypeCoverageCountsPath());
    if (!Options::useAOTOperationCounters() || !path)
        return;
    String name = makeStringByReplacingAll(String::fromUTF8(path), "%p"_s, String::number(getCurrentProcessID()));
    if (auto file = FilePrintStream::open(byteCast<char>(makeString(name, ".callees"_s).utf8().data()), "w")) {
        unsigned count = 0;
        for (Instance* instance : vm.m_aotInstances)
            count += instance->dumpCalleesSeen(*file);
        file->println("N\t", count);
    }
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
    Vector<Slot*> calleeCachesFilledSinceLastCollection;
    Vector<Slot*> abandonedCalleeCaches;
    UncheckedKeyHashMap<Slot*, std::pair<uint32_t, bool>> calleesSeen;
    Vector<JSCell*> cellsAddedSinceLastCollection;
    Vector<std::pair<Structure*, Structure*>> propertyRunTargetsAddedSinceLastCollection;
    bool hasVisitedEverythingInThisCollection { false };
    bool hasAddedWeakEntriesSinceLastCollection { false };
    bool hasFieldAdditions { false };
    AssumptionWatchpoint arraysLackIsConcatSpreadable;
    AssumptionWatchpoint arraysLackInheritedElements;
    Vector<Slot*> transitions;
    Vector<Slot*> transitionsSinceLastCollection;
    Vector<PolymorphicSlots*> allSiteSlots;
    UncheckedKeyHashMap<String, Structure*> shapes;
    UncheckedKeyHashMap<uint32_t, Structure*> knownShapes;
    UncheckedKeyHashMap<Structure*, Structure*> copyStructures;
    UncheckedKeyHashMap<std::pair<Structure*, uint16_t>, Structure*> convertedStructures;
    UncheckedKeyHashMap<std::pair<Structure*, const uint32_t*>, Instance::PropertyRunTarget> propertyRunTargets;
    UncheckedKeyHashMap<std::tuple<Structure*, Structure*, const IdentifierSet*>, Instance::CopiedProperties> copiedProperties;
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
    JSModuleLoader* loader { nullptr };
    Vector<std::pair<Structure*, Structure*>, 12> functionStructures;
    Vector<std::pair<Structure*, Structure*>, 6> functionStructuresWithCaptures;
    UncheckedKeyHashMap<uint32_t, ScriptExecutable*> topLevelExecutables;
    String retainedString;
    UncheckedKeyHashMap<uint32_t, JSArray*, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> templateObjects;
    JSCell* token { nullptr };
    FileSystem::MappedFileData typeCoverageCountersFile;
    Vector<uint32_t> typeCoverageCountersWithoutFile;
    bool loaderWasCleared { false };
    size_t environmentsSize { 0 };
    size_t sizeFromInstance { 0 };
    size_t numberOfFunctions { 0 };
    size_t dataStart { 0 };
    Vector<uint16_t> freeOwnData;
    unsigned numberOfOwnDataEverUsed { 0 };
    size_t usedDataEnd { 0 };
    size_t dataEnd { 0 };
    Vector<std::pair<size_t, size_t>> freeDataList;
};

static_assert(Instance::offsetOfVM() == 16, "JSWebAssemblyInstance::offsetOfVM()");
static_assert(!Instance::offsetOfSharedData());
static_assert(!(Instance::offsetOfOwnData() % sizeof(Data*)) && Instance::offsetOfOwnData() / sizeof(Data*) + Instance::maxNumberOfOwnData <= Instance::dataNumberMask + 1);

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
    auto dataStartFor = [](size_t numberOfFunctions) {
        return std::max<size_t>(roundUpToMultipleOf(WTF::pageSize(), sizeof(Instance) + numberOfFunctions * sizeof(uint32_t)), static_cast<size_t>(minStateWithData) << stateWithDataShift);
    };
    VMProgram* program = VMProgram::of(vm);
    RELEASE_ASSERT(program);
    destroyUnneededInstances(vm);
    size_t environmentsSize = roundUpToMultipleOf(WTF::pageSize(), Image::environmentsSize());
    size_t numberOfFunctions = Image::numberOfFunctions();
    size_t size = dataStartFor(numberOfFunctions) + roundUpToMultipleOf(WTF::pageSize(), Image::totalDataSize());
    Instance* instance = reinterpret_cast<Instance*>(static_cast<char*>(OSAllocator::reserveAndCommit(environmentsSize + size, OSAllocator::FastMallocPages)) + environmentsSize);
    instance->runtimeTable = AOT::runtimeTable(vm).entries();
    instance->globalObject = globalObject;
    instance->vm = &vm;
    instance->collections = new Collections;
    instance->collections->loader = loader;
    instance->collections->environmentsSize = environmentsSize;
    instance->collections->sizeFromInstance = size;
    instance->collections->numberOfFunctions = numberOfFunctions;
    instance->collections->dataStart = dataStartFor(numberOfFunctions) >> stateWithDataShift;
    instance->collections->usedDataEnd = instance->collections->dataStart;
    instance->collections->dataEnd = size >> stateWithDataShift;
    instance->program = program;
    instance->programData = &program->data();
    instance->stringConstantRecords = program->data().at<uint32_t>(program->data().stringConstantRecordsOffset);
    instance->infos = program->data().infos();
    instance->functionMetadataOffsets = program->data().functionMetadataOffsets();
    instance->programIdentifiers = program->identifiers();
    instance->sharedData = SharedData::get();
    instance->fieldsWithObservableReads = static_cast<uint8_t*>(OSAllocator::reserveAndCommit(sizeOfFieldsWithObservableReads, OSAllocator::FastMallocPages));
    if (Image* image = Image::withCode()) {
        instance->image = image->at<uint8_t>(0);
        instance->code = static_cast<const uint8_t*>(image->code());
        instance->codeGranules = image->at<uint32_t>(image->header().codeGranulesOffset);
        instance->subsequentFunctionStarts = image->at<uint32_t>(image->header().functionStartsOffset) + 1;
    }
    if (uint32_t numberOfCounters = Image::numberOfTypeCoverageCounters()) [[unlikely]] {
        if (const char* path = byteCast<char>(Options::aotTypeCoverageCountsPath())) {
            String name = makeStringByReplacingAll(String::fromUTF8(path), "%p"_s, String::number(getCurrentProcessID()));
            auto file = FileSystem::openFile(name, FileSystem::FileOpenMode::ReadWrite);
            if (file && file.truncate(static_cast<int64_t>(numberOfCounters) * sizeof(uint32_t))) {
                if (auto mapped = file.map(FileSystem::MappedFileMode::Shared, FileSystem::FileOpenMode::ReadWrite)) {
                    instance->collections->typeCoverageCountersFile = WTF::move(*mapped);
                    instance->typeCoverageCounters = reinterpret_cast<uint32_t*>(instance->collections->typeCoverageCountersFile.mutableSpan().data());
                }
            }
            if (!instance->typeCoverageCounters)
                dataLogLn("AOT: cannot map ", name, " for the type coverage counters");
        }
        if (!instance->typeCoverageCounters) {
            instance->collections->typeCoverageCountersWithoutFile.fill(0, numberOfCounters);
            instance->typeCoverageCounters = instance->collections->typeCoverageCountersWithoutFile.mutableSpan().data();
        }
    }
    instance->missLimitPerEightSlots = 8;
    instance->remainingMissBudget = 4;
    instance->structureIDBase = JSC::structureIDBase();
    {
        auto idOf = [](Structure* structure) { return structure->id().bits(); };
        auto receiverStructureID = [&](Receiver receiver) -> uint32_t& { return instance->receiverStructureIDs[static_cast<unsigned>(receiver)]; };
        receiverStructureID(Receiver::Map) = idOf(globalObject->mapStructure());
        receiverStructureID(Receiver::Set) = idOf(globalObject->setStructure());
        receiverStructureID(Receiver::WeakMap) = idOf(globalObject->weakMapStructure());
        receiverStructureID(Receiver::WeakSet) = idOf(globalObject->weakSetStructure());
        receiverStructureID(Receiver::RegExp) = idOf(globalObject->regExpStructure());
        receiverStructureID(Receiver::Date) = idOf(globalObject->dateStructure());
        instance->boundFunctionStructureID = idOf(globalObject->boundFunctionStructure());
        NativeExecutable* boundFunctionExecutable = vm.getBoundFunction(true, SourceTaintedOrigin::Untainted);
        if (usesStubs)
            boundFunctionExecutable->setCallEntrypoint(CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::CallBoundFunction))));
        instance->boundFunctionExecutable = boundFunctionExecutable;
        for (IndexingType type : { ArrayWithUndecided, ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous, ArrayWithArrayStorage, CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithDouble, CopyOnWriteArrayWithContiguous })
            instance->originalArrayStructureIDs[(type & (IndexingShapeMask | CopyOnWrite)) >> Instance::arrayKindShift] = idOf(globalObject->originalArrayStructureForIndexingType(type));
        if (!globalObject->isHavingABadTime()) {
            instance->regExpMatchesArrayStructureIDs[0] = idOf(globalObject->regExpMatchesArrayStructure());
            instance->regExpMatchesArrayStructureIDs[1] = idOf(globalObject->regExpMatchesArrayWithIndicesStructure());
            instance->newArrayWithInt32StructureID = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithInt32));
            instance->newArrayWithContiguousStructureID = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous));
            instance->newCopyOnWriteArrayStructureIDs[0] = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithInt32));
            instance->newCopyOnWriteArrayStructureIDs[1] = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithDouble));
            instance->newCopyOnWriteArrayStructureIDs[2] = idOf(globalObject->arrayStructureForIndexingTypeDuringAllocation(CopyOnWriteArrayWithContiguous));
        }
        instance->collections->arraysLackIsConcatSpreadable.install(globalObject->arrayIsConcatSpreadableWatchpointSet(), instance->arraysLackIsConcatSpreadable);
        instance->collections->arraysLackInheritedElements.install(globalObject->arrayPrototypeChainIsSaneWatchpointSet(), instance->arraysLackInheritedElements);
        instance->activationStructureID = idOf(globalObject->activationStructure());
        instance->auxiliarySpace = &vm.auxiliarySpace();
        instance->activationSpace = subspaceFor<JSLexicalEnvironment>(vm);
        instance->arrayAllocator = subspaceFor<JSArray>(vm)->allocatorFor(sizeof(JSArray), AllocatorForMode::EnsureAllocator).localAllocator();
        instance->ropeStringAllocator = subspaceFor<JSRopeString>(vm)->allocatorFor(sizeof(JSRopeString), AllocatorForMode::EnsureAllocator).localAllocator();
        instance->singleCharacterStrings = vm.smallStrings.singleCharacterStrings();
        instance->emptyString = vm.smallStrings.emptyString();
        instance->sentinelString = vm.smallStrings.sentinelString();
        instance->arrayIterationSentinel = vm.fastArrayUnboxedSentinel();
        instance->stringStructureID = idOf(vm.stringStructure.get());
        instance->smallIntStrings = vm.numericStrings.smallIntCache();
        instance->intStrings = vm.numericStrings.intCache();
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
    if (Image* image = Image::withShapes()) {
        instance->dispatch = image->at<uint32_t>(image->header().dispatchOffset);
        instance->selectorRows = image->at<uint32_t>(image->header().selectorRowsOffset);
        RELEASE_ASSERT(!image->header().intrinsicHash || image->header().intrinsicHash == ImmutableIntrinsics::shared()->hash());
        instance->objectPrototype = globalObject->objectPrototype();
        if (JSValue call = globalObject->linkTimeConstant(LinkTimeConstant::callFunction); call.isCell()) {
            instance->functionPrototypeCall = call.asCell();
            instance->boundFunctionStructureID = globalObject->boundFunctionStructure()->id().bits();
        }
        instance->selectorsOnObjectPrototype = static_cast<uint8_t*>(fastZeroedMalloc(image->header().numberOfSelectors / 8 + 1));
    }
    instance->collections->token = Symbol::create(vm);
    loader->setAOTInstance(instance);
    vm.m_aotInstances.append(instance);
    return *instance;
}

static ScriptExecutable* topLevelExecutableOf(Data& data)
{
    if (auto* function = dynamicDowncast<FunctionExecutable>(data.executable); function && function->hasAOTEntry()) {
        uint32_t moduleID = function->sourceProvider()->aotModuleID();
        if (ScriptExecutable* result = data.instance->topLevelExecutableOf(moduleID))
            return result;
        const ProgramModule* module = ProgramData::get()->moduleWithEntryOffset(moduleID - 1);
        RELEASE_ASSERT(module && module->isBuiltinFunction && module->hasExecutable());
        return data.instance->program->executable(module->executableIndex);
    }
    return data.executable->topLevelExecutable();
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

Structure* Instance::functionStructureWithCaptures(Structure* realmStructure)
{
    for (auto& [from, to] : collections->functionStructuresWithCaptures) {
        if (from == realmStructure)
            return to;
    }
    DeferGC deferGC(*vm);
    Structure* result = Structure::create(*vm, globalObject, realmStructure->storedPrototype(), realmStructure->typeInfo(), JSFunctionWithCaptures::info());
    result->setAOTInstance(this);
    collections->functionStructuresWithCaptures.append({ realmStructure, result });
    return result;
}

JSFunctionWithCaptures* Instance::tryMakeFunctionWithoutExecutable(uint32_t executableIndex, JSScope* scope, std::span<const EncodedJSValue> captures)
{
    const ExecutableRow& row = program->data().executableRow(executableIndex);
    Structure* realmStructure = nullptr;
    switch (static_cast<FunctionStructureKind>(row.functionStructureKind)) {
    case FunctionStructureKind::None:
        return nullptr;
    case FunctionStructureKind::Arrow:
        realmStructure = globalObject->arrowFunctionStructure(false);
        break;
    case FunctionStructureKind::StrictFunction:
        realmStructure = globalObject->strictFunctionStructure(false);
        break;
    case FunctionStructureKind::StrictMethod:
        realmStructure = globalObject->strictMethodStructure(false);
        break;
    case FunctionStructureKind::SloppyFunction:
        realmStructure = globalObject->sloppyFunctionStructure(false);
        break;
    case FunctionStructureKind::SloppyMethod:
        realmStructure = globalObject->sloppyMethodStructure(false);
        break;
    }
    auto word = JSFunction::tryEncodeAOTFunctionWord(row.entry[0], row.index[0]);
    if (!word)
        return nullptr;
    return JSFunctionWithCaptures::create(*vm, scope, functionStructureWithCaptures(realmStructure), *word, captures);
}

JSFunction* Instance::tryMakeFunctionWithoutExecutable(uint32_t executableIndex, JSScope* scope)
{
    const ExecutableRow& row = program->data().executableRow(executableIndex);
    Structure* realmStructure = nullptr;
    switch (static_cast<FunctionStructureKind>(row.functionStructureKind)) {
    case FunctionStructureKind::None:
        return nullptr;
    case FunctionStructureKind::Arrow:
        realmStructure = globalObject->arrowFunctionStructure(false);
        break;
    case FunctionStructureKind::StrictFunction:
        realmStructure = globalObject->strictFunctionStructure(false);
        break;
    case FunctionStructureKind::StrictMethod:
        realmStructure = globalObject->strictMethodStructure(false);
        break;
    case FunctionStructureKind::SloppyFunction:
        realmStructure = globalObject->sloppyFunctionStructure(false);
        break;
    case FunctionStructureKind::SloppyMethod:
        realmStructure = globalObject->sloppyMethodStructure(false);
        break;
    }
    auto word = JSFunction::tryEncodeAOTFunctionWord(row.entry[0], row.index[0]);
    if (!word)
        return nullptr;
    return JSFunction::createWithAOTFunctionWord(*vm, scope, functionStructure(realmStructure), *word);
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

JSArray* Instance::templateObjectFor(uint32_t numberOfDescriptor)
{
    if (auto it = collections->templateObjects.find(numberOfDescriptor); it != collections->templateObjects.end())
        return it->value;
    JSArray* result = uncheckedDowncast<JSTemplateObjectDescriptor>(program->constant(numberOfDescriptor).asCell())->createTemplateObject(globalObject);
    RELEASE_ASSERT(result);
    collections->templateObjects.add(numberOfDescriptor, result);
    noteCellAdded(result);
    return result;
}

StringImpl* Instance::retainUntilNextCall(String&& string)
{
    collections->retainedString = WTF::move(string);
    return collections->retainedString.impl();
}

ScriptExecutable* Instance::topLevelExecutableOf(uint32_t moduleID)
{
    return collections->topLevelExecutables.get(moduleID);
}

uint64_t Instance::prepareModuleCode(ModuleProgramExecutable* executable, JSScope* scope)
{
    VM& vm = *this->vm;
    auto throwScope = DECLARE_THROW_SCOPE(vm);
    uint32_t index = executable->programModule()->functionIndex;
    if (Data* data = dataIfExists(index); data && data->executable == executable)
        return data->code->entry();
    DeferGCForAWhile deferGC(vm);
    ImageCode found = findInImage(executable, CodeSpecializationKind::CodeForCall, nullptr, scope);
    if (!found) {
        throwSyntaxError(globalObject, throwScope, makeString("The module "_s, executable->source().provider()->sourceURL(), " was compiled ahead of time and cannot be run the way it has been loaded"_s));
        return 0;
    }
    Ref<JITCode> code = jitCodeForImageFunction(found, CodeSpecializationKind::CodeForCall);
    code->setInstance(*this);
    return Data::create(*this, executable, nullptr, code.get(), nullptr)->code->entry();
}

void Instance::didFinishModuleEvaluation(ModuleProgramExecutable* executable)
{
    if (vm->heap.collectionScope())
        return;
    Data* data = dataIfExists(executable->programModule()->functionIndex);
    if (!data || data->executable != executable)
        return;
#if ENABLE(SAMPLING_PROFILER)
    if (SamplingProfiler* profiler = vm->samplingProfiler()) [[unlikely]] {
        DeferGCForAWhile deferGC(*vm);
        Locker locker { profiler->getLock() };
        HeapIterationScope heapIterationScope(vm->heap);
        profiler->processUnverifiedStackTraces();
    }
#endif
    if (CodeBlock* codeBlock = data->codeBlock)
        codeBlock->releaseAOTData();
    else
        Data::destroy(data);
}

CodePtr<JSEntryPtrTag> moduleCodeEntrypoint()
{
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(tagCodePtr<JSEntryPtrTag>(stubAddress(Stub::EnterModule)));
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
    auto at = [&](size_t where) { return std::bit_cast<void*>(std::bit_cast<uintptr_t>(this) + (where << stateWithDataShift)); };
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
        memset(at(where), 0, units << stateWithDataShift);
        return at(where);
    }
    size_t where = collections->usedDataEnd;
    RELEASE_ASSERT(units <= collections->dataEnd - where);
    collections->usedDataEnd += units;
    return at(where);
}

void Instance::freeDataMemory(void* pointer, size_t size)
{
    size_t units = roundUpToMultipleOf<1 << stateWithDataShift>(size) >> stateWithDataShift;
    size_t where = (std::bit_cast<uintptr_t>(pointer) - std::bit_cast<uintptr_t>(this)) >> stateWithDataShift;
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
    instance->collectCalleeCacheMisses();
    instance->vm->m_aotInstances.removeFirst(instance);
    instance->vm->m_aotInstancesToDestroy.removeFirst(instance);
    while (!instance->collections->all.isEmpty())
        Data::destroy(instance->collections->all.last());
    size_t environmentsSize = instance->collections->environmentsSize;
    size_t size = instance->collections->sizeFromInstance;
    delete instance->collections;
    OSAllocator::decommitAndRelease(instance->fieldsWithObservableReads, sizeOfFieldsWithObservableReads);
    fastFree(instance->selectorsOnObjectPrototype);
    OSAllocator::decommitAndRelease(reinterpret_cast<char*>(instance) - environmentsSize, environmentsSize + size);
}

JSCell** Instance::environmentSlot(ImageEnvironment environment) const
{
    if (!environment.distance || environment.distance > collections->environmentsSize)
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
        && classifyAddress(machineLinkRegister).kind != ImageAddressInfo::NotInImage)
        return FrameAndPC { machineFrame, machineLinkRegister };
    void* pc = machinePC;
    while (isValid(record)) {
        if (record == topCallFrame)
            return FrameAndPC { topCallFrame, pc };
        pc = removeCodePtrTag(record->returnAddress);
        if (classifyAddress(pc).kind != ImageAddressInfo::NotInImage)
            return FrameAndPC { const_cast<FrameRecord*>(record->previous), pc };
        if (record->previous <= record)
            break;
        record = record->previous;
    }
    return std::nullopt;
}

SUPPRESS_ASAN Instance* instanceForFrame(const void* frame)
{
    for (auto* record = static_cast<const FrameRecord*>(frame);; record = record->previous) {
        ImageAddressInfo::Kind kind = classifyAddress(removeCodePtrTag(record->returnAddress)).kind;
        RELEASE_ASSERT(kind != ImageAddressInfo::NotInImage);
        if (kind == ImageAddressInfo::Adapter)
            return *reinterpret_cast<Instance* const*>(reinterpret_cast<const char*>(record->previous) + offsetOfInstanceInAdapter);
    }
}

SUPPRESS_ASAN bool canFindInstanceForFrame(const void* frame)
{
    for (auto* record = static_cast<const FrameRecord*>(frame); record; record = record->previous) {
        ImageAddressInfo::Kind kind = classifyAddress(removeCodePtrTag(record->returnAddress)).kind;
        if (kind == ImageAddressInfo::NotInImage)
            return false;
        if (kind == ImageAddressInfo::Adapter)
            return true;
    }
    return false;
}

NEVER_INLINE bool topCallFrameIsAOTFrame(const void* frame)
{
    if (!hasCode())
        return false;
    void* returnAddress = returnAddressForFrame(frame, __builtin_frame_address(0));
    return returnAddress && classifyAddress(returnAddress).kind != ImageAddressInfo::NotInImage;
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

CodeBlock* callerCodeBlock(const CallFrame* callFrame)
{
    FunctionRef function = callerFunction(callFrame);
    return function ? function.ensureCodeBlock() : nullptr;
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
    VM& vm = *instance.vm;
    DeferGCForAWhile deferGC(vm);
    unsigned numSlots = code.numSlots();
    Data* data = static_cast<Data*>(instance.allocateForData(sizeof(Data) + numSlots * sizeof(Slot)));
    data->codeBlock = codeBlock;
    data->instance = &instance;
    data->executable = executable;
    data->unlinkedCodeBlock = unlinkedCodeBlock;
    code.ref();
    data->code = &code;
    const FunctionInfo& info = instance.infos[code.index()];
    data->sites = code.sites();
    data->hasSiteConstants = code.imageFunction()->hasSiteConstants;
    data->hasSitesInMegamorphicCache = false;
    data->numSlots = numSlots;
    data->slotEpoch = 1;

    RELEASE_ASSERT(code.index() < instance.collections->numberOfFunctions);
    RELEASE_ASSERT(!instance.dataIfExists(code.index()));
    instance.setData(code.index(), data);
    data->indexInAllList = instance.collections->all.size();
    instance.collections->all.append(data);
    data->noteFilled();

    RELEASE_ASSERT(info.sitesInImage() == data->sites && (info.flags >> FunctionInfo::numberOfFlagBits) == std::min<uint32_t>(numSlots, FunctionInfo::maxEncodedSlots) && (!info.hasExecutable() || instance.program->executable(info.indexPlusOne() - 1) == executable));
    return data;
}

bool Instance::hasRoomForData() const
{
    return !collections->freeOwnData.isEmpty() || collections->numberOfOwnDataEverUsed < maxNumberOfOwnData;
}

void Instance::ensureDataIfThereIsRoom(uint32_t index)
{
    if (hasRoomForData()) {
        ensureData(index);
        return;
    }
    if (!(states[index] & dataNumberMask))
        states[index] &= ~(maxMisses << missesShift);
}

void Instance::setData(uint32_t index, Data* data)
{
    RELEASE_ASSERT(hasRoomForData() && !(states[index] & dataNumberMask));
    unsigned which = collections->freeOwnData.isEmpty() ? collections->numberOfOwnDataEverUsed++ : collections->freeOwnData.takeLast();
    ownData[which] = data;
    states[index] = isLinkedWithoutData | static_cast<uint32_t>(offsetOfOwnData() / sizeof(Data*) + which);
}

void Instance::setNotLinked(uint32_t index)
{
    if (uint32_t number = states[index] & dataNumberMask) {
        unsigned which = number - offsetOfOwnData() / sizeof(Data*);
        ownData[which] = nullptr;
        collections->freeOwnData.append(static_cast<uint16_t>(which));
    }
    states[index] = 0;
}

Data* Instance::ensureData(uint32_t index)
{
    Data* data = dataIfExists(index);
    if (data)
        return data;
    const FunctionInfo& info = infos[index];
    RELEASE_ASSERT(info.hasExecutable());
    FunctionExecutable* executable = program->executable(info.indexPlusOne() - 1);
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

static FunctionRef::Location locationForSite(FunctionRef function, uint32_t site)
{
    if (!function.info().hasInlineFrames) [[likely]]
        return { function, CallSiteIndex(site).bytecodeIndex(), 0 };
    const ImageFunction& record = *function.info().function();
    unsigned frame = PackedSite::inlineFrame(site);
    BytecodeIndex bytecodeIndex = CallSiteIndex(PackedSite::bits(site)).bytecodeIndex();
    if (!frame)
        return { function, bytecodeIndex, 0 };
    return { FunctionRef { function.instance, inlineFrameOf(record, frame).function }, bytecodeIndex, frame, false, PackedSite::isTailCallSite(site) };
}

FunctionRef::Location FunctionRef::locationForReturnAddress(const void* returnAddress, const void* frame) const
{
    using Asked = Instance::CachedAddressInfo;
    auto& asked = instance->cachedAddressInfo(returnAddress);
    if (asked.address != returnAddress || asked.site == Asked::siteNotResolved) [[unlikely]] {
        ImageAddressInfo what = classifyAddress(returnAddress);
        RELEASE_ASSERT(what.kind == ImageAddressInfo::Function && what.index == index);
        auto site = tryCallSiteAt(*info().function(), what.offset);
        RELEASE_ASSERT(!site || *site < Asked::hasNoSite);
        asked = { returnAddress, index, site.value_or(Asked::hasNoSite) };
    }
    ASSERT(asked.function == index);
    if (asked.site == Asked::hasNoSite) [[unlikely]]
        return { *this, BytecodeIndex(), 0 };
    for (const CallSiteOverride* candidate = instance->callSiteOverrides; candidate; candidate = candidate->previous()) {
        if (candidate->frame() != frame)
            continue;
        if (auto site = spreadSite(*info().function(), asked.site, candidate->item()))
            return locationForSite(*this, *site);
        break;
    }
    return locationForSite(*this, asked.site);
}

FunctionRef::Location FunctionRef::inlineCallSiteLocation(unsigned inlineFrame) const
{
    ImageInlineFrame frame = inlineFrameOf(*info().function(), inlineFrame);
    Location location = locationForSite(*this, PackedSite::pack(frame.parent, frame.callSite));
    location.isTailDeleted = frame.isTailCall;
    return location;
}

BytecodeIndex FunctionRef::bytecodeIndexAt(const void* returnAddress, const void* frame) const
{
    return locationForReturnAddress(returnAddress, frame).bytecodeIndex;
}

void Instance::noteCalleeSeen(Slot* cache, uint32_t function)
{
    auto& seen = collections->calleesSeen.add(cache, std::pair { function, false }).iterator->value;
    seen.second |= seen.first != function;
}

unsigned Instance::dumpCalleesSeen(PrintStream& out) const
{
    unsigned count = 0;
    for (Data* data : collections->all) {
        for (unsigned slot = 0; data->sites && slot + 1 < data->numSlots; ++slot) {
            if (data->sites[slot + 1].identifierAndExtra != Site::isCalleeCache)
                continue;
            auto seen = collections->calleesSeen.find(&data->slots[slot]);
            if (seen == collections->calleesSeen.end())
                continue;
            FunctionRef::Location site = locationForSite(data->function(), data->sites[slot].identifierAndExtra);
            out.println(site.function.index, "\t", site.bytecodeIndex.offset(), "\t", static_cast<int32_t>(seen->value.first), "\t", static_cast<unsigned>(seen->value.second));
            ++count;
        }
    }
    return count;
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
        return data->executable;
    const FunctionInfo& info = this->info();
    return info.hasExecutable() && instance ? instance->program->executable(info.indexPlusOne() - 1) : nullptr;
}

ScriptExecutable* FunctionRef::executableIfExists() const
{
    if (Data* data = dataIfExists())
        return data->executable;
    const FunctionInfo& info = this->info();
    return info.hasExecutable() && instance ? instance->program->executableIfExists(info.indexPlusOne() - 1) : nullptr;
}

bool FunctionRef::hasExecutable() const
{
    return dataIfExists() || (info().hasExecutable() && instance);
}

CodeBlock* FunctionRef::codeBlockIfExists() const
{
    Data* data = dataIfExists();
    return data ? data->codeBlock : nullptr;
}

uint32_t FunctionRef::siteConstantOf(const Slot* slot) const
{
    const FunctionInfo& info = this->info();
    size_t which = slot - (SharedData::contains(slot) ? instance->sharedData : instance->dataIfExists(index))->slots;
    if (info.flags & FunctionInfo::sitesHaveInlineConstants)
        return info.sitesInImage()[which].identifierAndExtra;
    if (!(info.flags & FunctionInfo::hasSiteConstants))
        return 0;
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

AllocationPlan FunctionRef::planOf(const Slot* firstSiteSlot) const
{
    uint32_t constant = siteConstantOf(firstSiteSlot + 1);
    if (!constant)
        return { };
    return { info().function()->plans() + constant - 1 };
}

UnlinkedCodeBlock* FunctionRef::unlinkedCodeBlockIfExists() const
{
    if (Data* data = dataIfExists(); data && data->unlinkedCodeBlock)
        return data->unlinkedCodeBlock;
    return nullptr;
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
    RELEASE_ASSERT(result);
    data->unlinkedCodeBlock = result;
    if (!data->hasBeenFilledSinceLastCollection)
        data->noteFilled();
    return result;
}

const FunctionMetadata* FunctionRef::metadata() const
{
    const ProgramData& data = programData();
    uint32_t at = data.functionMetadataOffsets()[index];
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
    const uint8_t* at = nullptr;
    if (auto* metadata = this->metadata()) {
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
    for (uint64_t count = readVarint(at); count--;) {
        uint64_t word = readVarint(at);
        offset += word >> 1;
        if (offset > bytecodeIndex.offset())
            break;
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
    for (unsigned i = 0; i < function.numberOfCatchEntrypoints; ++i) {
        if (function.catchEntrypoints()[i].bytecodeOffset == bytecodeOffset)
            return const_cast<uint8_t*>(Image::of(function).codeFor(function)) + function.catchEntrypoints()[i].codeOffset;
    }
    return nullptr;
}

SUPPRESS_ASAN JSCell* FunctionRef::calleeInFrame(const void* frame) const
{
    unsigned start = info().calleeStart;
    return start ? *(static_cast<JSCell* const*>(frame) - start) : nullptr;
}

const UnlinkedHandlerInfo* FunctionRef::handlerFor(unsigned bytecodeOffset) const
{
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

BytecodeIndex FunctionRef::resumePointOf(int32_t state) const
{
    if (state <= 0)
        return BytecodeIndex(0);
    int32_t offset = 0;
    if (const uint32_t* word = metadata()->find(FunctionMetadata::ResumePoints)) {
        const int32_t* table = programData().at<int32_t>(*word);
        if (state >= table[0] && static_cast<uint32_t>(state - table[0]) < static_cast<uint32_t>(table[1]))
            offset = table[2 + state - table[0]];
    }
    return BytecodeIndex(std::max(offset, 0));
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
    if (data->hasSitesInMegamorphicCache)
        instance.vm->invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change);
    RELEASE_ASSERT(instance.dataIfExists(data->code->index()) == data);
    instance.setNotLinked(data->code->index());
    auto belongsToThisData = [&](Slot* slot) { return slot >= data->slots && slot < data->slots + data->numSlots; };
    instance.collections->transitions.removeAllMatching(belongsToThisData);
    instance.collections->transitionsSinceLastCollection.removeAllMatching(belongsToThisData);
    instance.collections->calleeCachesFilledSinceLastCollection.removeAllMatching(belongsToThisData);
    instance.collections->abandonedCalleeCaches.removeAllMatching(belongsToThisData);
    instance.collections->calleesSeen.removeIf([&](auto& entry) { return belongsToThisData(entry.key); });
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
    if (data->functions)
        fastFree(data->functions);
    data->code->deref();
    instance.freeDataMemory(data, sizeof(Data) + data->numSlots * sizeof(Slot));
}

FunctionRef Data::function() const
{
    return { instance, code->index() };
}

CodeBlock* Data::ensureCodeBlock()
{
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
    if (!data.functions) {
        const FunctionMetadata& metadata = *data.function().metadata();
        data.functions = static_cast<FunctionExecutable**>(fastZeroedMalloc((functionsIn(metadata, FunctionMetadata::FunctionDecls).size() + functionsIn(metadata, FunctionMetadata::FunctionExprs).size()) * sizeof(FunctionExecutable*)));
    }
    FunctionExecutable*& function = data.functions[index];
    if (!function) {
        ScriptExecutable* executable = data.executable;
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

std::optional<uint32_t> FunctionRef::nestedExecutableIndex(bool isExpression, unsigned index) const
{
    uint32_t entry = functionsIn(*metadata(), isExpression ? FunctionMetadata::FunctionExprs : FunctionMetadata::FunctionDecls)[index];
    if (entry & 1)
        return std::nullopt;
    return (entry >> 1) - 1;
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
    if (!(info.flags & FunctionInfo::startsCold) || (info.function()->usesStaticImports && !moduleIsLinkedAsCompiled(scope)))
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
    if (const FunctionInfo& info = instance->infos[index]; info.flags & FunctionInfo::startsCold) {
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
    return slot.structureID && slot.newStructureID && (!slot.fieldType || (slot.offset & Slot::hasFieldType)) && !slot.hasPointer();
}

PolymorphicSlots* Instance::makeSiteSlots(Data* owner, UniquedStringImpl* name)
{
    auto* several = static_cast<PolymorphicSlots*>(fastZeroedMalloc(sizeof(PolymorphicSlots)));
    several->name = name;
    several->owner = owner;
    several->remainingBulkLearnAttempts = PolymorphicSlots::maxBulkLearnAttempts;
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
    visitor.appendUnbarriered(collections->token);
    if (!visitsEverything) {
        for (JSCell* cell : collections->cellsAddedSinceLastCollection)
            visitor.appendUnbarriered(cell);
        return;
    }
    for (Structure* structure : collections->shapes.values()) {
        if (structure)
            visitor.appendUnbarriered(structure);
    }
    for (Structure* structure : collections->knownShapes.values())
        visitor.appendUnbarriered(structure);
    for (auto& [from, to] : collections->copyStructures) {
        visitor.appendUnbarriered(from);
        if (to)
            visitor.appendUnbarriered(to);
    }
    for (Structure* structure : collections->emptyStructures.values())
        visitor.appendUnbarriered(structure);
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

const Instance::PropertyRunTarget& Instance::propertyRunTarget(Structure* structure, const uint32_t* run, const ScopedLambda<void(Vector<UniquedStringImpl*, 16>&)>& collectNames)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.propertyRunTarget(structure, run, collectNames);
    if (auto it = collections->propertyRunTargets.find({ structure, run }); it != collections->propertyRunTargets.end())
        return it->value;
    Vector<UniquedStringImpl*, 16> names;
    collectNames(names);
    PropertyRunTarget target;
    bool mayBeIntercepted = false;
    for (JSValue next = structure->storedPrototype(); next.isObject() && !mayBeIntercepted;) {
        JSObject* prototype = asObject(next);
        Structure* prototypeStructure = prototype->structure();
        if (prototypeStructure->isDictionary() && !prototypeStructure->isUncacheableDictionary() && !prototypeStructure->hasBeenFlattenedBefore())
            prototypeStructure = prototypeStructure->flattenDictionaryStructure(*vm, prototype);
        TypeInfo info = prototypeStructure->typeInfo();
        if (prototypeStructure->isDictionary() || prototypeStructure->hasPolyProto() || info.overridesPut() || info.overridesGetPrototype() || info.overridesGetOwnPropertySlot() || prototype->hasNonReifiedStaticProperties()) {
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

Structure* Instance::knownShapeStructureIfExists(uint32_t shape)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.knownShapeStructureIfExists(shape);
    return collections->knownShapes.get(shape);
}

Structure* Instance::knownShapeStructure(uint32_t shape, std::span<UniquedStringImpl* const> names)
{
    if (Instance& realmStructure = ensure(globalObject); &realmStructure != this)
        return realmStructure.knownShapeStructure(shape, names);
    if (auto it = collections->knownShapes.find(shape); it != collections->knownShapes.end())
        return it->value;
    Image* image = Image::withShapes();
    RELEASE_ASSERT(shape && shape < image->header().numberOfShapes);
    const ImageShape& description = image->at<ImageShape>(image->header().shapesOffset)[shape];
    RELEASE_ASSERT(names.size() == description.numberOfProperties);
    DeferGC deferGC(*vm);
    Structure* empty = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, globalObject->objectPrototype(), description.inlineCapacity);
    RELEASE_ASSERT(empty->inlineCapacity() == description.inlineCapacity);
    auto slots = knownShapeSlots(shape);
    Structure* result = slots.empty() ? Structure::createWithProperties(*vm, empty, names) : Structure::createWithProperties(*vm, empty, names, slots, description.reserved, description.layoutID ? description.inlineSlots : std::numeric_limits<unsigned>::max());
    RELEASE_ASSERT(result);
    result->setKnownShape(*vm, safeCast<uint16_t>(shape));
    if (description.hasIds) {
        uint16_t fieldIDInSlot[Structure::numberOfSlotsWithFieldIDs] { };
        const uint16_t* ids = slots.data() + slots.size();
        for (unsigned i = 0; i < slots.size(); ++i) {
            if (ids[i])
                fieldIDInSlot[slots[i]] = ids[i];
        }
        result->setTypedLayoutID(description.layoutID, fieldIDInSlot);
    } else if (TypedLayoutTable::hasTypedFields())
        result->setTypedLayoutID(description.layoutID);
    collections->knownShapes.add(shape, result);
    noteCellAdded(result);
    return result;
}

Structure* Instance::emptyStructureForLayout(uint16_t layoutID)
{
    if (Instance& realmStructure = ensure(globalObject); &realmStructure != this)
        return realmStructure.emptyStructureForLayout(layoutID);
    if (auto it = collections->emptyStructures.find(layoutID); it != collections->emptyStructures.end())
        return it->value;
    unsigned capacity = TypedLayoutTable::numberOfSlots(layoutID);
    RELEASE_ASSERT(capacity);
    DeferGC deferGC(*vm);
    unsigned inlineSlots = TypedLayoutTable::inlineSlots(layoutID);
    Structure* empty = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, globalObject->objectPrototype(), KnownShape::inlineCapacityFor(inlineSlots));
    RELEASE_ASSERT(empty->inlineCapacity() >= inlineSlots);
    Structure* result = Structure::createWithProperties(*vm, empty, { }, std::span<const uint16_t> { }, capacity, inlineSlots);
    RELEASE_ASSERT(result);
    result->setTypedLayoutID(layoutID);
    collections->emptyStructures.add(layoutID, result);
    noteCellAdded(result);
    return result;
}

Structure* Instance::emptyStructureForLayout(uint16_t layoutID, JSObject* prototype)
{
    unsigned capacity = TypedLayoutTable::numberOfSlots(layoutID);
    RELEASE_ASSERT(capacity);
    DeferGC deferGC(*vm);
    unsigned inlineSlots = TypedLayoutTable::inlineSlots(layoutID);
    Structure* empty = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, prototype, KnownShape::inlineCapacityFor(inlineSlots));
    RELEASE_ASSERT(empty->inlineCapacity() >= inlineSlots);
    Structure* result = Structure::createWithProperties(*vm, empty, { }, std::span<const uint16_t> { }, capacity, inlineSlots);
    RELEASE_ASSERT(result);
    result->setTypedLayoutID(layoutID);
    return result;
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
    auto no = [](ASCIILiteral why) {
        TypedLayoutTable::s_lastConversionFailure = why;
        return false;
    };
    if (object->type() != FinalObjectType)
        return no("it is not a plain object"_s);
    if (old->typedLayoutID())
        return no("it already has the layout of another type"_s);
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
    return true;
}

std::span<const uint16_t> Instance::knownShapeSlots(uint32_t shape) const
{
    Image* image = Image::withShapes();
    const ImageShape& description = image->at<ImageShape>(image->header().shapesOffset)[shape];
    if (!description.slots)
        return { };
    return { image->at<uint16_t>(image->header().shapeSlotsOffset) + description.slots - 1, description.numberOfProperties };
}

void Instance::inspectObjectPrototype()
{
    if (!objectPrototype)
        return;
    Structure* structure = objectPrototype->structure();
    if (structure->id().bits() == objectPrototypeStructureID)
        return;
    objectPrototypeStructureID = 0;
    if (structure->isDictionary() || !structure->propertyAccessesAreCacheable() || structure->typeInfo().overridesGetOwnPropertySlot()
        || structure->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence() || !structure->storedPrototype().isNull()
        || (structure->typeInfo().hasStaticPropertyTable() && !structure->staticPropertiesReified()))
        return;
    Image* image = Image::withShapes();
    memset(selectorsOnObjectPrototype, 0, image->header().numberOfSelectors / 8 + 1);
    structure->forEachProperty(*vm, [&](const PropertyTableEntry& entry) {
        if (uint32_t selector = image->selectorNamed(*vm, *entry.key()))
            selectorsOnObjectPrototype[selector / 8] |= 1 << (selector % 8);
        return true;
    });
    objectPrototypeStructureID = structure->id().bits();
}

Structure* Instance::literalStructure(Structure* empty, std::span<UniquedStringImpl* const> names)
{
    if (Instance& realmStructure = ensure(globalObject); &realmStructure != this)
        return realmStructure.literalStructure(empty, names);
    ASSERT(empty->storedPrototype() == globalObject->objectPrototype());
    Vector<uintptr_t, 32> words;
    words.append(empty->inlineCapacity());
    for (UniquedStringImpl* name : names)
        words.append(std::bit_cast<uintptr_t>(name));
    String key { std::span { reinterpret_cast<const Latin1Character*>(words.span().data()), words.size() * sizeof(void*) } };
    if (auto it = collections->shapes.find(key); it != collections->shapes.end())
        return it->value;
    Structure* result = Structure::createWithProperties(*vm, empty, names);
    collections->shapes.add(WTF::move(key), result);
    noteCellAdded(result);
    return result;
}
template void Instance::visit(AbstractSlotVisitor&, bool);
template void Instance::visit(SlotVisitor&, bool);

unsigned Instance::inlineCapacityFor(JSFunction* constructor, unsigned inlineCapacityInBytecode)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.inlineCapacityFor(constructor, inlineCapacityInBytecode);
    auto it = collections->learnedInlineCapacities.find(constructor);
    return it == collections->learnedInlineCapacities.end() ? inlineCapacityInBytecode : std::max(inlineCapacityInBytecode, it->value);
}

void Instance::noteFirstStructure(Structure* first, JSFunction* constructor)
{
    if (Instance& realmInstance = ensure(globalObject); &realmInstance != this)
        return realmInstance.noteFirstStructure(first, constructor);
    if (first->inlineCapacity() < maxLearnedInlineCapacity && !first->previousID())
        collections->setConstructorByFirstStructure(first, constructor);
}

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
        if (isValidOffset(last->get(*vm, names[i], attributes))) {
            if (attributes)
                last = nullptr;
            continue;
        }
        PropertyOffset offset;
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

JSObject* Instance::tryCopySlotsForSpread(JSObject* source)
{
    if (Instance& realmStructure = ensure(globalObject); &realmStructure != this)
        return realmStructure.tryCopySlotsForSpread(source);
    Structure* sourceStructure = source->structure();
    Structure* copyStructure = nullptr;
    if (auto it = collections->copyStructures.find(sourceStructure); it != collections->copyStructures.end())
        copyStructure = it->value;
    else {
        if (sourceStructure->canPerformFastPropertyEnumerationCommon() && checkStructureForClone(sourceStructure) && !sourceStructure->outOfLineCapacity()) {
            Vector<UniquedStringImpl*, 32> names;
            bool isInOrder = true;
            sourceStructure->forEachProperty(*vm, [&](const PropertyTableEntry& entry) {
                isInOrder &= !entry.attributes() && static_cast<size_t>(entry.offset()) == names.size();
                names.append(entry.key());
                return isInOrder;
            });
            if (isInOrder && !names.isEmpty()) {
                DeferGC deferGC(*vm);
                unsigned inlineCapacity = std::max<unsigned>(sourceStructure->inlineCapacity(), JSFinalObject::defaultInlineCapacity);
                Structure* empty = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, globalObject->objectPrototype(), inlineCapacity);
                if (empty->inlineCapacity() >= names.size())
                    copyStructure = literalStructure(empty, names.span());
            }
        }
        collections->copyStructures.add(sourceStructure, copyStructure);
        noteCellAdded(sourceStructure);
        noteCellAdded(copyStructure);
    }
    if (!copyStructure)
        return nullptr;
    JSFinalObject* copy = JSFinalObject::create(*vm, copyStructure);
    for (PropertyOffset offset = 0; offset <= copyStructure->maxOffset(); ++offset)
        copy->putDirectOffset(*vm, offset, source->getDirect(offset));
    return copy;
}

void Instance::didHaveBadTime()
{
    newArrayWithInt32StructureID = 0;
    newArrayWithContiguousStructureID = 0;
    zeroSpan(std::span { newCopyOnWriteArrayStructureIDs });
    zeroSpan(std::span { regExpMatchesArrayStructureIDs });
}

void Instance::noteFieldAddition(Structure* before, unsigned slot, Structure* afterwards)
{
    FieldAddition& entry = fieldAdditions[fieldAdditionIndex(before->id().bits(), slot)];
    entry.structureID = before->id().bits();
    entry.slot = slot;
    entry.structureIDAfterAddition = afterwards->id().bits();
    collections->hasFieldAdditions = true;
}

void Instance::noteCalleeCacheFilled(Slot* cache)
{
    collections->calleeCachesFilledSinceLastCollection.append(cache);
}

void Instance::noteCalleeCacheAbandoned(Slot* cache)
{
    collections->abandonedCalleeCaches.append(cache);
}

void Instance::clearCachesValidatedByMegamorphicCacheEpoch()
{
    zeroSpan(std::span { customGetters });
    zeroSpan(std::span { inheritedSetters });
}

void Instance::finalizeUnconditionally(bool newOnly)
{
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        AOT::runtimeTable(*vm).countOperation("Heap::collection", newOnly ? "eden" : "full");
        AOT::writeOperationCounts(*vm);
    }
    for (Slot* cache : collections->calleeCachesFilledSinceLastCollection) {
        if (cache->structureID && !vm->heap.isMarked(static_cast<JSCell*>(cache->pointer)))
            cache->clear();
    }
    collections->calleeCachesFilledSinceLastCollection.shrink(0);
    if (std::exchange(collections->hasFieldAdditions, false))
        zeroSpan(std::span { fieldAdditions });
    clearCachesValidatedByMegamorphicCacheEpoch();
    for (PolymorphicSlots* several : collections->allSiteSlots) {
        if (newOnly && !several->owner->hasBeenFilledSinceLastCollection)
            continue;
        for (Slot& slot : several->slots)
            several->owner->finalizeSlot(*vm, slot);
    }
    for (Data* data : newOnly ? collections->filledSinceLastCollection : collections->all)
        data->finalizeUnconditionally(*vm);
    for (Data* data : collections->filledSinceLastCollection)
        data->hasBeenFilledSinceLastCollection = false;
    collections->filledSinceLastCollection.shrink(0);
    auto isDead = [&](uint32_t structureID) { return !vm->heap.isMarked(std::bit_cast<StructureID>(structureID).decode()); };
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
        collections->rejectedConversions.removeIf([&](auto& entry) {
            return !vm->heap.isMarked(entry.key.first);
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
        for (Slot* cache : collections->abandonedCalleeCaches)
            cache[1].offset = (CalleeCache::maxAttempts - 1) * CalleeCache::attempt;
        collections->abandonedCalleeCaches.shrink(0);
        auto& transitions = collections->transitions;
        std::ranges::sort(transitions);
        transitions.shrink(std::ranges::unique(transitions).begin() - transitions.begin());
        transitions.removeAllMatching([](Slot* slot) { return !hasTransition(*slot); });
    }
    if (Options::verboseAOTCompilation()) [[unlikely]] {
        dataLogLn("AOT: instance ", RawPointer(this), " after ", newOnly ? "an eden" : "a full", " collection: ", collections->all.size(), " of ", collections->numberOfFunctions, " functions have slots, in ", collections->usedDataEnd - collections->dataStart, " bytes; ",
            collections->allSiteSlots.size(), " sites with several slots (", collections->allSiteSlots.size() * sizeof(PolymorphicSlots), " bytes); ", collections->transitions.size(), " transitions; structures of literals ", collections->shapes.size(), " + ", collections->knownShapes.size(),
            ", of copies ", collections->copyStructures.size(), ", of layouts ", collections->emptyStructures.size(), " + ", collections->convertedStructures.size(), "; property runs ", collections->propertyRunTargets.size(), "; copied properties ", collections->copiedProperties.size(),
            "; constructors ", collections->constructorsByFirstStructure.size(), " + ", collections->learnedInlineCapacities.size(), "; modules ", collections->topLevelExecutables.size(), "; template objects ", collections->templateObjects.size(),
            "; environments ", Image::environmentsSize() / sizeof(void*), "; unlinked functions in the program ", program->data().numberOfUnlinkedFunctions);
    }
}

void Instance::forgetResolvedGlobalScopes()
{
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
        else if (!slot.hasPointer() && slot.newStructureID && (!slot.fieldType || (slot.offset & Slot::hasFieldType)))
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
