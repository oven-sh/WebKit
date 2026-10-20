/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeValidation.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTCellFormat.h"
#include "AOTEffectsDeclared.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTStubDescriptor.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "MarkedBlockInlines.h"
#include "Reg.h"
#include "SubspaceInlines.h"
#include <wtf/DataLog.h>
#include <wtf/Lock.h>

namespace JSC { namespace AOT {

static_assert(static_cast<unsigned>(ValidationFailureKind::BindingReadBeforeStore) + 1 == numberOfValidationFailureKinds);

ASCIILiteral nameOf(ValidationFailureKind kind)
{
    switch (kind) {
    case ValidationFailureKind::ValueType:
        return "ValueType"_s;
    case ValidationFailureKind::UnreachableValue:
        return "UnreachableValue"_s;
    case ValidationFailureKind::FrameCellEscaped:
        return "FrameCellEscaped"_s;
    case ValidationFailureKind::SavedFrameRootMissing:
        return "SavedFrameRootMissing"_s;
    case ValidationFailureKind::VisitedCellsDiffer:
        return "VisitedCellsDiffer"_s;
    case ValidationFailureKind::OperationEnteredVM:
        return "OperationEnteredVM"_s;
    case ValidationFailureKind::OperationCollected:
        return "OperationCollected"_s;
    case ValidationFailureKind::CacheHitDiffers:
        return "CacheHitDiffers"_s;
    case ValidationFailureKind::CacheEntryDropped:
        return "CacheEntryDropped"_s;
    case ValidationFailureKind::ImpossibleMutation:
        return "ImpossibleMutation"_s;
    case ValidationFailureKind::TypeTestsDiffer:
        return "TypeTestsDiffer"_s;
    case ValidationFailureKind::BarrierWasNeeded:
        return "BarrierWasNeeded"_s;
    case ValidationFailureKind::BuiltinNotImmutable:
        return "BuiltinNotImmutable"_s;
    case ValidationFailureKind::ArrayNotPristine:
        return "ArrayNotPristine"_s;
    case ValidationFailureKind::ShapeWordInvalid:
        return "ShapeWordInvalid"_s;
    case ValidationFailureKind::StackTraceDiffers:
        return "StackTraceDiffers"_s;
    case ValidationFailureKind::FastPathResultDiffers:
        return "FastPathResultDiffers"_s;
    case ValidationFailureKind::OperationPreconditionBroken:
        return "OperationPreconditionBroken"_s;
    case ValidationFailureKind::NativeCallChangedOperand:
        return "NativeCallChangedOperand"_s;
    case ValidationFailureKind::NativeCallWroteHeap:
        return "NativeCallWroteHeap"_s;
    case ValidationFailureKind::NativeCallRanCode:
        return "NativeCallRanCode"_s;
    case ValidationFailureKind::NativeCallThrew:
        return "NativeCallThrew"_s;
    case ValidationFailureKind::NativeCallCollected:
        return "NativeCallCollected"_s;
    case ValidationFailureKind::NativeCallKeptOperand:
        return "NativeCallKeptOperand"_s;
    case ValidationFailureKind::AssumedType:
        return "AssumedType"_s;
    case ValidationFailureKind::UnlistedCallee:
        return "UnlistedCallee"_s;
    case ValidationFailureKind::SecondRun:
        return "SecondRun"_s;
    case ValidationFailureKind::ReadsDiffer:
        return "ReadsDiffer"_s;
    case ValidationFailureKind::ContainerContents:
        return "ContainerContents"_s;
    case ValidationFailureKind::ArrayStorageKind:
        return "ArrayStorageKind"_s;
    case ValidationFailureKind::RemovedFieldNamed:
        return "RemovedFieldNamed"_s;
    case ValidationFailureKind::ReceiverShape:
        return "ReceiverShape"_s;
    case ValidationFailureKind::CallChangedKeptRegister:
        return "CallChangedKeptRegister"_s;
    case ValidationFailureKind::UnwindingChangedKeptRegister:
        return "UnwindingChangedKeptRegister"_s;
    case ValidationFailureKind::ArrayElementsNotOrdinary:
        return "ArrayElementsNotOrdinary"_s;
    case ValidationFailureKind::KeysListed:
        return "KeysListed"_s;
    case ValidationFailureKind::LegacyRegExpStaticRead:
        return "LegacyRegExpStaticRead"_s;
    case ValidationFailureKind::GuardNoLongerHolds:
        return "GuardNoLongerHolds"_s;
    case ValidationFailureKind::BindingReadBeforeStore:
        return "BindingReadBeforeStore"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void willReadLegacyRegExpStatics(VM& vm)
{
    if (validatesRuntime(vm) && vm.aotState().image->programTables().flags & ProgramTables::neverReadsLegacyRegExpStatics)
        crashOnValidationFailure(vm, { ValidationFailureKind::LegacyRegExpStaticRead, { }, { }, JSValue() });
}

static constexpr unsigned maxNumberOfTestedElements = 64;

template<typename Collection>
static bool entriesHaveType(Instance& instance, Collection* collection, Typed::ContentsSlot slot, const RuntimeType& type)
{
    const RuntimeLayoutTable& layouts = instance.state->objects->layouts;
    JSCell* storage = collection->storage();
    typename Collection::Helper::Entry entry = 0;
    for (unsigned numberOfTestedEntries = 0; storage && numberOfTestedEntries < maxNumberOfTestedElements; ++numberOfTestedEntries) {
        auto next = Collection::Helper::transitAndNext(*instance.vm, *storage, entry);
        if (!next.storage)
            return true;
        if (!layouts.valueHasType(type, slot == Typed::ContentsSlot::Value ? next.value : next.key))
            return false;
        storage = next.storage;
        entry = next.entry + 1;
    }
    return true;
}

// A data property under a name like "-1" costs the structure its mark and leaves true what the storage aspect says.
bool hasOrdinaryElements(VM& vm, JSValue value)
{
    auto* array = dynamicDowncast<JSArray>(value);
    if (!array)
        return false;

    Structure* structure = array->structure();
    JSGlobalObject* realm = structure->realm();
    if (structure->mayInterceptIndexedAccesses() || hasSlowPutArrayStorage(structure->indexingType()) || !structure->isStructureExtensible())
        return false;
    if (realm->isHavingABadTime() || !realm->arrayPrototypeChainIsSaneWatchpointSet().isStillValid())
        return false;
    if (!structure->hasMonoProto() || structure->storedPrototype() != realm->arrayPrototype())
        return false;

    bool hasAccessorUnderNumericName = false;
    structure->forEachProperty(vm, [&](const auto& entry) {
        hasAccessorUnderNumericName |= isCanonicalNumericIndexString(entry.key()) && (entry.attributes() & PropertyAttribute::AccessorOrCustomAccessorOrValue);
        return !hasAccessorUnderNumericName;
    });
    return !hasAccessorUnderNumericName;
}

bool contentsHaveType(Instance& instance, JSCell* container, Typed::ContentsSlot slot, const RuntimeType& type)
{
    if (auto* map = dynamicDowncast<JSMap>(container))
        return slot == Typed::ContentsSlot::Element || entriesHaveType(instance, map, slot, type);
    if (auto* set = dynamicDowncast<JSSet>(container))
        return slot != Typed::ContentsSlot::Element || entriesHaveType(instance, set, slot, type);

    auto* array = dynamicDowncast<JSArray>(container);
    if (!array || slot != Typed::ContentsSlot::Element)
        return true;

    const RuntimeLayoutTable& layouts = instance.state->objects->layouts;
    unsigned length = array->length();
    unsigned stride = std::max(1u, length / maxNumberOfTestedElements);
    for (unsigned index = 0; index < length; index += stride) {
        JSValue element = array->tryGetIndexQuickly(index);
        if (element && !layouts.valueHasType(type, element))
            return false;
    }
    return true;
}

static void dumpClassOf(PrintStream& out, JSValue found)
{
    if (!found) {
        out.print("the empty value");
        return;
    }
    if (found.isUndefined()) {
        out.print("undefined");
        return;
    }
    if (found.isNull()) {
        out.print("null");
        return;
    }
    if (found.isBoolean()) {
        out.print("boolean");
        return;
    }
    if (found.isInt32()) {
        out.print("int32");
        return;
    }
    if (found.isDouble()) {
        out.print("double");
        return;
    }
    if (!found.isCell()) {
        out.print("invalid encoding");
        return;
    }

    JSCell* cell = found.asCell();
    switch (cell->type()) {
    case StringType:
        out.print("string");
        return;
    case SymbolType:
        out.print("symbol");
        return;
    case HeapBigIntType:
        out.print("bigint");
        return;
    case CompiledEnvironmentType:
        out.print("environment");
        return;
    default:
        out.print(cell->isObject() ? "object " : "cell ", cell->classInfo()->className);
        return;
    }
}

static void dumpChangedRegister(PrintStream& out, JSValue found)
{
    uint32_t bits = static_cast<uint32_t>(found.asInt32());
    unsigned registerIndex = bits & ChangedRegister::unknownRegister;
    if (registerIndex == ChangedRegister::unknownRegister)
        out.print("some register");
    else
        out.print(Reg::fromIndex(registerIndex));
    uint32_t callee = bits >> ChangedRegister::calleeShift;
    if (callee == ChangedRegister::unknownCallee)
        return;
    if (callee & ChangedRegister::calleeIsStub)
        out.print(" callee stub ", descriptorOf(static_cast<StubFamily>(callee & ~ChangedRegister::calleeIsStub)).name);
    else
        out.print(" callee f", callee);
}

namespace {

MAKE_PRINT_ADAPTOR(ClassDump, JSValue, dumpClassOf);
MAKE_PRINT_ADAPTOR(ChangedRegisterDump, JSValue, dumpChangedRegister);

class IndexDump {
public:
    template<typename Tag>
    explicit IndexDump(Typed::ID<Tag> id)
        : m_index(id ? std::optional<uint32_t>(id.index()) : std::nullopt)
    {
    }

    void dump(PrintStream& out) const
    {
        if (m_index)
            out.print(*m_index);
        else
            out.print("-");
    }

private:
    std::optional<uint32_t> m_index;
};

} // anonymous namespace

void crashOnValidationFailure(VM&, const ValidationFailure& failure)
{
    // Never released: another thread that fails waits here until this one has crashed.
    static Lock lock;
    lock.lock();

    // Describing a damaged cell can fault, so the kind and the ids are printed first.
    dataLog("AOT validation: ", nameOf(failure.kind), " f", IndexDump(failure.function), " v", IndexDump(failure.value), " ");
    if (failure.kind == ValidationFailureKind::CallChangedKeptRegister || failure.kind == ValidationFailureKind::UnwindingChangedKeptRegister)
        dataLogLn(ChangedRegisterDump(failure.found));
    else
        dataLogLn(ClassDump(failure.found));
    CRASH_WITH_INFO(static_cast<uint64_t>(failure.kind), failure.function.index(), failure.value.index());
}

OperationPermissions permissionsOf(Entry entry)
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    if (!operation)
        return { .mayEnterVM = true, .mayCollect = true };
    Typed::Effects effects = Typed::declaredBoundOf(*operation);
    return { .mayEnterVM = effects.mayCallUnknownCode, .mayCollect = effects.mayCollect };
}

OperationValidationScope::CollectionVersions OperationValidationScope::collectionVersions() const
{
    MarkedSpace& space = m_vm.heap.objectSpace();
    return { space.markingVersion(), space.edenVersion(), space.newlyAllocatedVersion() };
}

NEVER_INLINE void OperationValidationScope::open(Entry entry)
{
    OperationPermissions permissions = permissionsOf(entry);
    m_entry = entry;
    m_mayNotEnterVM = !permissions.mayEnterVM;
    m_mayNotCollect = !permissions.mayCollect;
    m_collectionVersions = collectionVersions();
    m_didEnterVMBefore = std::exchange(m_vm.didEnterVM, false);
    m_isOpen = true;
}

NEVER_INLINE void OperationValidationScope::close()
{
    bool didEnterVM = m_mayNotEnterVM && m_vm.didEnterVM;
    bool didCollect = m_mayNotCollect && collectionVersions() != m_collectionVersions;
    if (didEnterVM || didCollect)
        dataLogLn("AOT: ", nameOf(m_entry), " is declared not to ", didEnterVM ? "enter the VM" : "collect");
    if (didEnterVM)
        crashOnValidationFailure(m_vm, { ValidationFailureKind::OperationEnteredVM, { }, { }, { } });
    if (didCollect)
        crashOnValidationFailure(m_vm, { ValidationFailureKind::OperationCollected, { }, { }, { } });
    m_vm.didEnterVM |= m_didEnterVMBefore;
}

void CollectionValidator::didFinishCollection(VM& vm, CollectionScope scope)
{
    for (Instance* instance : vm.aotState().instances) {
        for (JSObject* object : instance->state->builtinCells->objectsWithEveryPropertyLocked()) {
            if (!object->structure()->hasImmutableProperties())
                crashOnValidationFailure(vm, { ValidationFailureKind::BuiltinNotImmutable, { }, { }, object });
        }
    }

    if (scope != CollectionScope::Full)
        return;

    vm.heap.objectSpace().forEachSubspace([&](Subspace& subspace) {
        subspace.forEachMarkedCell([&](HeapCell* heapCell, HeapCell::Kind kind) {
            if (!isJSCellKind(kind))
                return;
            JSCell* cell = static_cast<JSCell*>(heapCell);
            validateShapeWord(cell);
            if (cell->type() == CompiledEnvironmentType)
                return;
            if (auto* array = dynamicDowncast<JSArray>(cell))
                array->validatePristine(vm);
        });
        return IterationStatus::Continue;
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
