/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTClosure.h"
#include "AOTConvention.h"
#include "AOTEnvironment.h"
#include "AOTInstance.h"
#include "AOTModules.h"
#include "AOTObjectModel.h"
#include "AOTObjectWords.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeTable.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTUnwinding.h"
#include "DeferTermination.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "MarkedBlockCage.h"
#include "ObjectConstructor.h"
#include <wtf/DataLog.h>
#include <wtf/WTFProcess.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAllocateSlow, JSCell*, (Instance* instance, uint32_t allocation))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAllocateSlow);
    if (vm.traps().needsOutOfMemoryError()) [[unlikely]]
        vm.heap.cage().tryTakeBackOutOfMemoryError(vm);
    OPERATION_RETURN(scope, newCellForCompiledCode(vm, instance->state->objects->structures.allocateAsCompiledCodeDoes(vm, allocation)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAllocateSlowFromTemplate, JSCell*, (Instance* instance, uint32_t allocation, const uint8_t* body))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAllocateSlowFromTemplate);
    if (vm.traps().needsOutOfMemoryError()) [[unlikely]]
        vm.heap.cage().tryTakeBackOutOfMemoryError(vm);
    State& state = *instance->state;
    JSCell* cell = state.objects->structures.allocateAsCompiledCodeDoes(vm, allocation);
    const AllocationData row = state.tables.allocations()[allocation];
    size_t offsetOfBody = row.shape < state.tables.firstEnvironmentShape ? offsetOfFirstField : offsetOfFirstEnvironmentVariable;
    memcpySpan(std::span { reinterpret_cast<uint8_t*>(cell) + offsetOfBody, row.size() - offsetOfBody }, std::span { body, row.size() - offsetOfBody });
    OPERATION_RETURN(scope, newCellForCompiledCode(vm, cell));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetHeaderOfAllocation, uint64_t, (Instance* instance, uint32_t allocation))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetHeaderOfAllocation);
    OPERATION_RETURN(scope, instance->state->objects->structures.fillAllocationSite(vm, allocation).header);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewClosureSlow, JSCell*, (Instance* instance, uint64_t request, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewClosureSlow);
    DeferTerminationForAWhile deferTermination(vm);
    ObjectModel& objects = *instance->state->objects;
    uint64_t captures[] = { first, second, third, fourth };
    static_assert(std::size(captures) == ClosureRequest::maxSlimClosureCapturesInRegisters && ClosureRequest::maxNumberOfCaptures <= std::size(captures));
    RELEASE_ASSERT(ClosureRequest::countOf(request) <= (ClosureRequest::isSlim(request) ? ClosureRequest::maxSlimClosureCaptures : ClosureRequest::maxNumberOfCaptures));
    JSCell* cell;
    auto word = [&](ptrdiff_t offset) -> uint64_t& {
        return *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(cell) + offset);
    };
    if (ClosureRequest::isSlim(request)) {
        cell = SlimClosure::createForCompiledCode(vm, objects.closures.slimClosureStructure(kindOfClosureNumber(ClosureRequest::closureNumberOf(request))), SlimClosure::allocationSize(ClosureRequest::countOf(request)));
        for (unsigned slot = 0; slot < ClosureRequest::countOf(request); ++slot)
            word(SlimClosure::offsetOfCaptures() + slot * sizeof(uint64_t)) = slot < std::size(captures) ? captures[slot] : static_cast<uint64_t>(JSValue::encode(JSValue()));
        WTF::storeStoreFence();
        word(SlimClosure::offsetOfWord()) = instance->functionTables.closureWords[ClosureRequest::closureNumberOf(request)];
        WTF::storeStoreFence();
        OPERATION_RETURN(scope, cell);
    }
    cell = objects.structures.allocate(vm, ClosureRequest::allocationOf(request));
    word(JSObject::butterflyOffset()) = 0;
    word(JSCallee::offsetOfScopeChain()) = std::bit_cast<uint64_t>(instance->globalObject);
    word(JSFunction::offsetOfExecutableOrRareData()) = std::bit_cast<uint64_t>(instance->closures.executables[ClosureRequest::executableOf(request)]);
    for (unsigned slot = 0; slot < ClosureRequest::countOf(request); ++slot)
        word(Closure::offsetOfCaptures() + slot * sizeof(uint64_t)) = captures[slot];
    WTF::storeStoreFence();
    word(Closure::offsetOfEntry()) = instance->functionTables.closureWords[ClosureRequest::closureNumberOf(request)];
    WTF::storeStoreFence();
    OPERATION_RETURN(scope, cell);
}

static void markEscapedIfEnvironment(JSValue held)
{
    if (held && held.isCell() && held.asCell()->type() == CompiledEnvironmentType)
        uncheckedDowncast<Environment>(held.asCell())->markEscaped();
}

static bool liesInFrame(JSValue held)
{
    return held && held.isCell() && static_cast<uint8_t>(held.asCell()->cellState()) == frameCellState;
}

namespace {

struct ClosureInFrame {
    static_assert(SlimClosure::offsetOfCaptures() == SlimClosure::offsetOfWord() + static_cast<ptrdiff_t>(sizeof(uint64_t)) && Closure::offsetOfCaptures() == Closure::offsetOfEntry() + static_cast<ptrdiff_t>(sizeof(uint64_t)));

    explicit ClosureInFrame(JSCell* closure, bool standsInHeap = false)
        : cell(closure)
        , words(reinterpret_cast<const uint64_t*>(closure))
        , isFull(closure->type() == CompiledClosureType)
        , placeOfClosureWord((isFull ? Closure::offsetOfEntry() : SlimClosure::offsetOfWord()) / sizeof(uint64_t))
        , closureNumber(ClosureWord::closureNumberOf(words[placeOfClosureWord]))
        , counts(captureCountsOfClosureNumber(closureNumber))
    {
        RELEASE_ASSERT(standsInHeap || static_cast<uint8_t>(closure->cellState()) == frameCellState);
        RELEASE_ASSERT(isFull || closure->type() == SlimClosureType);
        RELEASE_ASSERT(counts.references <= counts.captures && counts.captures <= (isFull ? Closure::maxNumberOfCaptures : ClosureRequest::maxSlimClosureCaptures));
    }

    std::span<const uint64_t> captures() const { return { words + placeOfClosureWord + 1, counts.captures }; }
    JSCell*& copyInHeap() const { return *reinterpret_cast<JSCell**>(reinterpret_cast<uint8_t*>(cell) + offsetOfCopyInHeapOfClosureInFrame(isFull ? Closure::offsetOfCaptures() : SlimClosure::offsetOfCaptures(), counts.captures)); }

    JSCell* cell;
    const uint64_t* words;
    bool isFull;
    size_t placeOfClosureWord;
    uint32_t closureNumber;
    CaptureCounts counts;
};

} // anonymous namespace

static JSCell* makeRoomInHeapFor(VM& vm, Instance* instance, const ClosureInFrame& closure)
{
    if (closure.isFull)
        return Closure::createForCompiledCode(vm, closure.cell->structure(), Closure::allocationSize(closure.counts.captures));
    return SlimClosure::createForCompiledCode(vm, instance->state->objects->closures.slimClosureStructure(kindOfClosureNumber(closure.closureNumber)), SlimClosure::allocationSize(closure.counts.captures));
}

static JSCell* copyToHeap(VM& vm, Instance* instance, const ClosureInFrame& closure)
{
    JSCell* room = makeRoomInHeapFor(vm, instance, closure);
    uint64_t* target = reinterpret_cast<uint64_t*>(room);
    for (size_t place = 1; place < closure.placeOfClosureWord; ++place)
        target[place] = closure.words[place];
    std::span<const uint64_t> captures = closure.captures();
    for (unsigned slot = 0; slot < captures.size(); ++slot) {
        uint64_t word = captures[slot];
        if (slot < closure.counts.references) {
            JSValue held = JSValue::decode(word);
            if (liesInFrame(held)) {
                word = std::bit_cast<uint64_t>(ClosureInFrame(held.asCell()).copyInHeap());
                RELEASE_ASSERT(word);
            } else
                markEscapedIfEnvironment(held);
        }
        target[closure.placeOfClosureWord + 1 + slot] = word;
    }
    WTF::storeStoreFence();
    target[closure.placeOfClosureWord] = closure.words[closure.placeOfClosureWord];
    WTF::storeStoreFence();
    return room;
}

static JSCell* moveToHeap(VM& vm, Instance* instance, JSCell* closureInFrame)
{
    if (!liesInFrame(closureInFrame))
        return copyToHeap(vm, instance, ClosureInFrame(closureInFrame, true));
    Vector<JSCell*, 8> waiting;
    waiting.append(closureInFrame);
    while (true) {
        ClosureInFrame closure(waiting.last());
        if (!closure.copyInHeap()) {
            size_t numberWaiting = waiting.size();
            for (uint64_t reference : closure.captures().first(closure.counts.references)) {
                JSValue held = JSValue::decode(reference);
                if (liesInFrame(held) && !ClosureInFrame(held.asCell()).copyInHeap())
                    waiting.append(held.asCell());
            }
            if (waiting.size() != numberWaiting)
                continue;
            closure.copyInHeap() = copyToHeap(vm, instance, closure);
        }
        waiting.removeLast();
        if (waiting.isEmpty())
            return closure.copyInHeap();
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMoveClosureToHeap, JSCell*, (Instance* instance, JSCell* closureInFrame))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTMoveClosureToHeap);
    OPERATION_RETURN(scope, moveToHeap(vm, instance, closureInFrame));
}

#define AOT_DEFINE_NEW_COLLECTION(operation, name, structureOfRealm) \
    JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operation, JSCell*, (Instance* instance)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::operation); \
        DeferTerminationForAWhile deferTermination(vm); \
        Structure* structure = globalObject->structureOfRealm(); \
        prepareInlineAllocation<JS ## name>(instance, InlineAllocation::name, structure); \
        OPERATION_RETURN(scope, JS ## name::create(vm, structure)); \
    }
AOT_DEFINE_NEW_COLLECTION(operationAOTNewMap, Map, mapStructure)
AOT_DEFINE_NEW_COLLECTION(operationAOTNewSet, Set, setStructure)
#undef AOT_DEFINE_NEW_COLLECTION

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBarrierSlow, void, (Instance* instance, JSCell* cell))
{
    instance->vm->writeBarrierSlowPath(cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRememberElement, void, (Instance* instance, JSCell* array, EncodedJSValue element))
{
    JSObject* owner = cellArgument<JSObject>(array, Entry::operationAOTRememberElement);
    JSValue value = JSValue::decode(element);
    if (value && value.isCell())
        instance->vm->heap.writeBarrierSlowPath(owner, value.asCell());
    else
        instance->vm->writeBarrierSlowPath(owner);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringConstant, JSCell*, (Instance* instance, uint32_t index))
{
    LeafOperationScope leaf(*instance->vm);
    return instance->state->strings->constant(*instance->vm, index);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTKeyConstant, JSCell*, (Instance* instance, uint32_t index))
{
    VM& vm = *instance->vm;
    LeafOperationScope leaf(vm);
    DeferGCForAWhile deferGC(vm);
    return instance->state->strings->keyCell(vm, index);
}

static void exitIfRealmLacksHostOf(State& state, uint32_t cellNumber)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(state.tables.builtinCells()[cellNumber], table);
    RELEASE_ASSERT(key);
    if (BuiltinBindings::provides(table.at(key->builtin()).origin))
        return;
    dataLogLn("AOT: the program needs the host's ", table.textOf(key->builtin()), ", and this realm has no host");
    exitProcess(4);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBuiltinCell, JSCell*, (Instance* instance, uint32_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTBuiltinCell);
    JSCell* cell = instance->state->builtinCells->cellReadWithoutTest(index);
    if (!cell) [[unlikely]]
        exitIfRealmLacksHostOf(*instance->state, index);
    OPERATION_RETURN(scope, cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMethodCalledBoxed, JSCell*, (Instance* instance, uint32_t builtin))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTMethodCalledBoxed);
    JSCell* method = instance->state->builtinCells->findObject(BuiltinID(builtin));
    RELEASE_ASSERT(method);
    OPERATION_RETURN(scope, method);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFunctionOfBuiltinAsBuilt, JSCell*, (Instance* instance, uint32_t form))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTFunctionOfBuiltinAsBuilt);
    OPERATION_RETURN(scope, instance->state->builtinCells->findFunctionAsBuilt(static_cast<BuiltinBoxedStub>(form)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBigIntConstant, EncodedJSValue, (Instance* instance, uint32_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTBigIntConstant);
    DeferTerminationForAWhile deferTermination(vm);
    OPERATION_RETURN(scope, JSValue::encode(instance->state->strings->bigIntConstant(globalObject, index)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSingletonClosure, JSCell*, (Instance* instance, uint32_t closureNumber))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTSingletonClosure);
    OPERATION_RETURN(scope, instance->state->objects->closures.closure(vm, closureFunctionWithNumber(closureNumber)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewTemplateObject, JSCell*, (Instance* instance, uint32_t request, uint32_t module, uint32_t offsetOfCell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewTemplateObject);
    ProgramStrings& strings = *instance->state->strings;
    const auto& templateObject = requestAt<TemplateObjectRequest>(instance, request);
    MarkedArgumentBuffer cookedStrings;
    MarkedArgumentBuffer rawStrings;
    for (const auto& part : unsafeMakeSpan(templateObject.parts, templateObject.count)) {
        cookedStrings.append(part.cookedPlusOne ? JSValue(strings.constant(vm, part.cookedPlusOne - 1)) : jsUndefined());
        rawStrings.append(strings.constant(vm, part.raw));
    }
    if (cookedStrings.hasOverflowed() || rawStrings.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    JSArray* cooked = constructArray(globalObject, static_cast<JSC::ArrayAllocationProfile*>(nullptr), cookedStrings);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* raw = constructArray(globalObject, static_cast<JSC::ArrayAllocationProfile*>(nullptr), rawStrings);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    objectConstructorFreeze(globalObject, raw);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    cooked->putDirect(vm, vm.propertyNames->raw, raw, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    objectConstructorFreeze(globalObject, cooked);
    instance->state->modules->didMakeTemplateObject(ModuleID(module), offsetOfCell);
    OPERATION_RETURN(scope, cooked);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
