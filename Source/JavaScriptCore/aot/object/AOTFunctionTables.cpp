/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionTables.h"

#if ENABLE(AOT)

#include "AOTBuiltinModules.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCheckedFrameCells.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCallee.h"
#include "AOTConvention.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTMembersLeftOut.h"
#include "AOTObjectModel.h"
#include "AOTObjectWords.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTSlimClosure.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "AOTThreshold.h"
#include "AbstractSlotVisitorInlines.h"
#include "FunctionRareData.h"
#include "HeapUtil.h"
#include "MarkedBlockCage.h"
#include "JSCInlines.h"
#include "NativeExecutable.h"
#include "Options.h"
#include "SlotVisitorInlines.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/SymbolImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(FunctionTables::numberOfExecutables == ClosureWords::numberOfExecutables);
static_assert(FunctionTables::numberOfExecutables - 1 <= ResumeClosureRequest::maxExecutable);

namespace {

struct FunctionKindFields {
    CallableKind callableKind { CallableKind::Plain };
    bool isAsync { false };
    bool isGenerator { false };
    bool takesNewTarget { false };
    bool isConstructor { false };
    bool isDerivedConstructor { false };
};

} // anonymous namespace

static consteval FunctionKindData functionKindWith(FunctionKindFields fields)
{
    FunctionKindData kind { };
    kind.callableKind = static_cast<unsigned>(fields.callableKind);
    kind.isAsync = fields.isAsync;
    kind.isGenerator = fields.isGenerator;
    kind.takesNewTarget = fields.takesNewTarget;
    kind.isConstructor = fields.isConstructor;
    kind.isDerivedConstructor = fields.isDerivedConstructor;
    return kind;
}

static consteval bool usesNotConstructibleExecutable(FunctionKindFields fields)
{
    FunctionKindData kind = functionKindWith(fields);
    return FunctionTables::executableKind(kind) == FunctionTables::ExecutableKind::NotConstructible && !FunctionTables::canConstruct(kind);
}

static_assert(FunctionTables::executableKind(functionKindWith({ })) == FunctionTables::ExecutableKind::Constructible);
static_assert(FunctionTables::canConstruct(functionKindWith({ })));
static_assert(closureWordFlags(functionKindWith({ })) == ClosureWord::Flags { });

static_assert(FunctionTables::executableKind(functionKindWith({ .isConstructor = true })) == FunctionTables::ExecutableKind::Class);
static_assert(FunctionTables::canConstruct(functionKindWith({ .isConstructor = true })));
static_assert(closureWordFlags(functionKindWith({ .isConstructor = true })) == ClosureWord::Flags { .isNotCallable = true });

static_assert(FunctionTables::executableKind(functionKindWith({ .takesNewTarget = true, .isConstructor = true, .isDerivedConstructor = true })) == FunctionTables::ExecutableKind::Class);
static_assert(closureWordFlags(functionKindWith({ .takesNewTarget = true, .isConstructor = true, .isDerivedConstructor = true }))
    == ClosureWord::Flags { .isNotCallable = true, .takesNewTarget = true });

static_assert(usesNotConstructibleExecutable({ .isAsync = true }));
static_assert(usesNotConstructibleExecutable({ .isGenerator = true }));
static_assert(usesNotConstructibleExecutable({ .isAsync = true, .isGenerator = true }));
static_assert(usesNotConstructibleExecutable({ .callableKind = CallableKind::Arrow }));
static_assert(usesNotConstructibleExecutable({ .callableKind = CallableKind::Method }));

static_assert(FunctionTables::executableKind(functionKindWith({ .callableKind = CallableKind::Bound })) == FunctionTables::ExecutableKind::BoundClosure);
static_assert(!FunctionTables::canConstruct(functionKindWith({ .callableKind = CallableKind::Bound })));

static_assert(FunctionTables::executableKind(functionKindWith({ .takesNewTarget = true })) == FunctionTables::ExecutableKind::Constructible);
static_assert(closureWordFlags(functionKindWith({ .takesNewTarget = true })) == ClosureWord::Flags { .takesNewTarget = true });

uint32_t closureNumberOf(FunctionID function)
{
    return programTables().closureNumberOf(function.index());
}

FunctionID closureFunctionWithNumber(uint32_t number)
{
    return FunctionID(programTables().closureFunctionWithNumber(number));
}

unsigned functionLengthOf(FunctionID function)
{
    const ProgramTables& tables = programTables();
    ASSERT(function.index() >= tables.closureFunctions.first && function.index() < tables.closureFunctions.end);
    return tables.functionLengthOf(function.index());
}

FunctionID sourceFunctionOf(FunctionID function)
{
    return FunctionID(programTables().sourceFunctionOf(function.index()));
}

const BoundFunctionData& boundFunctionData(FunctionID function)
{
    auto boundFunctions = programTables().boundFunctions();
    auto found = std::ranges::lower_bound(boundFunctions, function.index(), { }, &BoundFunctionData::function);
    RELEASE_ASSERT(found != boundFunctions.end() && found->function == function.index());
    return *found;
}

static const uint64_t* closureWordsByNumber(const Image& image)
{
    const ClosureFunctionRangeData& range = image.programTables().closureFunctions;
    return image.section<uint64_t>(ImageSection::ClosureWords).subspan(range.first - range.omittedWordCount).data();
}

FunctionTables::FunctionTables(VM& vm)
    : words(closureWordsByNumber(*Image::current()))
{
    RELEASE_ASSERT(!Options::useJIT());
    for (unsigned index = 0; index < numberOfExecutables; ++index) {
        auto kind = static_cast<ExecutableKind>(index);
        bool isClass = kind == ExecutableKind::Class;
        bool constructs = isClass || kind == ExecutableKind::Constructible;
        NativeExecutable* executable = vm.getHostFunction(isClass ? callClassConstructor : callCompiledClosure, ImplementationVisibility::Private, constructs ? constructCompiledClosure : callHostFunctionAsConstructor, 0, String());
        executables[index] = Strong<JSCell>(vm, executable);
    }
}

NativeExecutable* FunctionTables::executableOf(FunctionID function) const
{
    return uncheckedDowncast<NativeExecutable>(executables[static_cast<unsigned>(executableKind(functionKindData(function)))].get());
}

FunctionTables& FunctionTables::of(VM& vm)
{
    RELEASE_ASSERT(vm.hasCompiledCode());
    auto& tables = vm.aotState().functionTables;
    if (!tables)
        tables = makeUnique<FunctionTables>(vm);
    return *tables;
}

static ClosureTable& closureTableOf(JSObject* callee)
{
    return State::of(callee->structure()->realm()).objects->closures;
}

static ASCIILiteral initialNamePrefix(FunctionKindData kind)
{
    if (kind.isBound())
        return "bound "_s;
    bool isGetter = kind.accessor() == MethodKind::Getter;
    RELEASE_ASSERT(isGetter || kind.accessor() == MethodKind::Setter);
    return isGetter ? "get "_s : "set "_s;
}

String writtenFunctionName(FunctionID function)
{
    const Image& image = *Image::current();
    ASSERT(function.index() < image.programTables().numberOfFunctions());
    StringID name(image.programTables().nameOfFunction(function.index()));
    return !image.isObfuscated() && image.text().hasText(name) ? image.text().createString(name) : emptyString();
}

String initialNameProperty(FunctionID function)
{
    FunctionKindData kind = functionKindData(function);
    String written = writtenFunctionName(function);
    return kind.hasInitialNamePrefix() ? makeString(initialNamePrefix(kind), written) : written;
}

JSString* initialNamePropertyString(VM& vm, State& state, FunctionID function)
{
    if (functionKindData(function).hasInitialNamePrefix())
        return jsString(vm, initialNameProperty(function));
    return state.strings->writtenNameOfFunction(vm, function);
}

// The frames of a generator run under a function object of their own.
// So is a frame without a record of a function that records its callees only once one of them was renamed.
static bool isNamedInFramesByFunction(FunctionID closureFunction)
{
    FunctionDescription description(closureFunction);
    return !functionKindData(closureFunction).isBound() && !functionKindData(sourceFunctionOf(closureFunction)).isGenerator && !description.recordsCallee() && !description.recordsCalleeOnceRenamed();
}

// functionName() can run at the end of a GC, where of() cannot create the tables.
static FunctionTables* existingTables(VM& vm)
{
    return vm.aotState().functionTables.get();
}

static String nameSetAtRunTime(VM& vm, FunctionID function)
{
    FunctionTables* tables = existingTables(vm);
    if (!tables || !tables->hasNamesSetAtRunTime.load(std::memory_order_acquire)) [[likely]]
        return String();
    FunctionID sourceFunction = sourceFunctionOf(function);
    Locker locker { tables->namesLock };
    if (String name = tables->namesShownUntilLookedFor.get(sourceFunction); !name.isNull())
        return name;
    return tables->namesSetAtRunTime.get(sourceFunction);
}

static String nameShownUntilLookedFor(VM& vm, FunctionID function)
{
    FunctionTables* tables = existingTables(vm);
    if (!tables || !tables->hasNamesSetAtRunTime.load(std::memory_order_acquire)) [[likely]]
        return String();
    Locker locker { tables->namesLock };
    return tables->namesShownUntilLookedFor.get(sourceFunctionOf(function));
}

static String nameLookedFor(VM& vm, FunctionID function)
{
    FunctionTables* tables = existingTables(vm);
    if (!tables || !tables->hasNamesLookedFor.load(std::memory_order_acquire)) [[likely]]
        return String();
    FunctionID sourceFunction = sourceFunctionOf(function);
    {
        Locker locker { tables->namesLock };
        if (!tables->namesLookedFor.contains(sourceFunction.index()))
            return String();
    }
    String name = initialNameProperty(sourceFunction);
    return name.isEmpty() ? String() : name;
}

// Every call is made from further in than its one entry says.
static constexpr RecordedCallee emptyTable[2] { { 0, nullptr }, { std::numeric_limits<uintptr_t>::max(), nullptr } };
AOT_THRESHOLD(size_t, leastNumberOfRecordedCallees, 256);

const RecordedCallee* FunctionTables::recordedCalleesOf(const FunctionTables* tables)
{
    return (tables && tables->hasRecordedCallees() ? tables->recordedCalleeStorage.span() : std::span { emptyTable }).subspan(1).data();
}

static RecordedCallee& entryOfCallFrom(std::span<RecordedCallee> storage, uintptr_t stackPointerOfCaller)
{
    return storage[1 + (stackPointerOfCaller & storage[0].stackPointerOfCaller) / sizeof(RecordedCallee)];
}

static void setRecordedCalleeStorage(VM& vm, FunctionTables& tables, Vector<RecordedCallee>&& storage)
{
    tables.recordedCalleeStorage = WTF::move(storage);
    tables.phaseOfLastWalkOfRecordedCallees = std::nullopt;
    VMState& state = vm.aotState();
    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances)
        instance->heap.recordedCallees = FunctionTables::recordedCalleesOf(&tables);
}

void FunctionTables::recordCallee(VM& vm, const void* stackPointer, JSObject* closure)
{
    Locker locker { recordedCalleesLock };
    RecordedCallee added { std::bit_cast<uintptr_t>(stackPointer), closure };
    if (hasRecordedCallees()) {
        RecordedCallee& entry = entryOfCallFrom(recordedCalleeStorage.mutableSpan(), added.stackPointerOfCaller);
        if (entry.stackPointerOfCaller <= added.stackPointerOfCaller) {
            entry = added;
            return;
        }
    }

    const StackBounds& stack = Thread::currentSingleton().stack();
    Vector<RecordedCallee, 16> recorded { added };
    for (const RecordedCallee& entry : recordedCallees()) {
        if (entry.stackPointerOfCaller > added.stackPointerOfCaller && stack.contains(std::bit_cast<void*>(entry.stackPointerOfCaller)))
            recorded.append(entry);
    }
    for (size_t numberOfEntries = std::max(leastNumberOfRecordedCallees, recordedCallees().size());; numberOfEntries *= 2) {
        // Two places on one stack are less than the stack apart.
        RELEASE_ASSERT(numberOfEntries * sizeof(RecordedCallee) <= 2 * roundUpToPowerOfTwo(stack.size()));
        Vector<RecordedCallee> storage;
        storage.fill({ 0, nullptr }, 1 + numberOfEntries);
        storage[0].stackPointerOfCaller = (numberOfEntries - 1) * sizeof(RecordedCallee);
        bool eachHasItsPlace = std::ranges::all_of(recorded, [&](const RecordedCallee& entry) {
            return !std::exchange(entryOfCallFrom(storage.mutableSpan(), entry.stackPointerOfCaller), entry).closure;
        });
        if (eachHasItsPlace) {
            setRecordedCalleeStorage(vm, *this, WTF::move(storage));
            return;
        }
    }
}

bool FunctionTables::framesAreNamedByCallee(FunctionID function)
{
    return recordsCalleesOf(function) || FunctionDescription(function).recordsCallee();
}

void FunctionTables::recordCalleesOf(VM& vm, FunctionID closureFunction)
{
    if (recordsCalleesOf(closureFunction))
        return;
    {
        Locker locker { namesLock };
        const ProgramTables& tables = programTables();
        if (!functionsRenamedAtRunTime)
            functionsRenamedAtRunTime = MallocSpan<uint8_t>::zeroedMalloc(tables.numberOfFunctions());
        std::span<uint8_t> isRenamed = functionsRenamedAtRunTime.mutableSpan();
        uint32_t sourceFunction = tables.sourceFunctionOf(closureFunction.index());
        isRenamed[closureFunction.index()] = 1;
        for (Number24 form : tables.closureFormsOf(sourceFunction))
            isRenamed[form] = 1;
        isRenamed[sourceFunction] = 1;
        if (isRenamedAtRunTime.exchange(isRenamed.data(), std::memory_order_release))
            return;
    }
    VMState& state = vm.aotState();
    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances)
        makeTailCallsByValuePoll(*instance, isRenamedAtRunTime.load(std::memory_order_acquire));
}

// In the lowest bit of the function object: a collection has left the entry in the table. The call that writes the entry anew takes the mark away.
static constexpr uintptr_t markOfCollection = 1;
// In the next bit: the function object lies in a frame. The collector visits what is recorded, so the function as it is written stands above the two bits in its place.
static constexpr uintptr_t markOfClosureInFrame = 2;
static constexpr unsigned numberOfMarks = 2;

static bool liesInFrame(const RecordedCallee& entry)
{
    return std::bit_cast<uintptr_t>(entry.closure) & markOfClosureInFrame;
}

static JSObject* closureOnHeapOf(const RecordedCallee& entry)
{
    return liesInFrame(entry) ? nullptr : std::bit_cast<JSObject*>(std::bit_cast<uintptr_t>(entry.closure) & ~markOfCollection);
}

static FunctionID sourceFunctionOf(const RecordedCallee& entry)
{
    if (liesInFrame(entry))
        return FunctionID(safeCast<uint32_t>(std::bit_cast<uintptr_t>(entry.closure) >> numberOfMarks));
    return sourceFunctionOf(*functionOf(closureOnHeapOf(entry)));
}

std::optional<JSObject*> FunctionTables::recordedCalleeOfCallFrom(const void* stackPointer, FunctionID sourceFunction)
{
    if (!hasRecordedCallees())
        return std::nullopt;
    uintptr_t stackPointerOfCaller = std::bit_cast<uintptr_t>(stackPointer);
    const RecordedCallee& entry = entryOfCallFrom(recordedCalleeStorage.mutableSpan(), stackPointerOfCaller);
    if (entry.stackPointerOfCaller != stackPointerOfCaller || sourceFunctionOf(entry) != sourceFunction)
        return std::nullopt;
    return closureOnHeapOf(entry);
}

SUPPRESS_ASAN std::optional<JSObject*> FunctionTables::calleeKeptInFrame(VM& vm, const void* frameRecordOfCaller, CalleeOfSite site, FunctionID sourceFunction)
{
    if (!framesAreNamedByCallee(sourceFunction))
        return std::nullopt;
    auto isFunctionObjectOfFunction = [&](JSCell* cell) {
        std::optional<FunctionID> function = functionOf(cell);
        return function && sourceFunctionOf(*function) == sourceFunction;
    };
    auto isCellOnHeap = [&](JSCell* cell) {
        return cell && !cell->isPreciseAllocation() && HeapUtil::isJSCellStorage(vm.heap, vm.heap.objectSpace().blocks().filter(), cell);
    };
    auto* kept = static_cast<JSCell* const*>(frameRecordOfCaller)[site.wordInFrame()];
    bool liesInFrame = Thread::currentSingleton().stack().contains(kept) || CheckedFrameCells::isInRange(kept);
    if (!liesInFrame && !isCellOnHeap(kept))
        return std::nullopt;
    // An environment in a frame has no header that says what it is. What its slot holds is asked in its place.
    if (std::optional<uint32_t> field = site.fieldInEnvironment(); field && (liesInFrame || kept->type() == CompiledEnvironmentType)) {
        SlotData slot = SlotData::decode(*field);
        JSCell* closure = cellInSlot(slot.representation(), reinterpret_cast<const uint8_t*>(kept) + slot.offset, vm.heap.cage().base());
        if (liesInFrame && !isCellOnHeap(closure))
            return nullptr;
        return closure && isFunctionObjectOfFunction(closure) ? asObject(closure) : nullptr;
    }
    if (liesInFrame)
        return nullptr;
    return isFunctionObjectOfFunction(kept) ? std::optional(asObject(kept)) : std::nullopt;
}

// How far the stack of the program reaches is not asked during a collection. A call has returned for sure once the program has returned to whoever entered it from outside.
static bool hasReturned(VM& vm, const FunctionTables& tables, const RecordedCallee& entry)
{
    if (!vm.isEntered())
        return true;
    return std::bit_cast<uintptr_t>(entry.closure) & markOfCollection && vm.entryCountFromOutside() != tables.entryCountAtLastCollection;
}

template<typename Visitor>
void FunctionTables::visitRecordedCallees(VM& vm, Visitor& visitor, uint64_t phaseOfCollector)
{
    UNUSED_PARAM(phaseOfCollector);
    if (!hasRecordedCallees() || !vm.isEntered())
        return;
    Locker locker { recordedCalleesLock };
    std::span<const RecordedCallee> entries = recordedCallees();
    auto visit = [&](const RecordedCallee& entry) {
        if (JSObject* closure = closureOnHeapOf(entry); closure && !hasReturned(vm, *this, entry))
            visitor.appendUnbarriered(closure);
    };
    // The GC verifier marks on its own and leaves the state of the real collection alone.
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        phaseOfLastWalkOfRecordedCallees = phaseOfCollector;
        placesFilledAtLastWalk.shrink(0);
        for (size_t place = 0; place < entries.size(); ++place) {
            if (entries[place].closure)
                placesFilledAtLastWalk.append(static_cast<uint32_t>(place));
        }
        for (uint32_t place : placesFilledAtLastWalk)
            visit(entries[place]);
    } else
        std::ranges::for_each(entries, visit);
}
template void FunctionTables::visitRecordedCallees(VM&, AbstractSlotVisitor&, uint64_t);
template void FunctionTables::visitRecordedCallees(VM&, SlotVisitor&, uint64_t);

void FunctionTables::finalizeRecordedCallees(VM& vm, uint64_t phaseOfCollector)
{
    if (!hasRecordedCallees())
        return;
    Locker locker { recordedCalleesLock };
    bool isInUse = false;
    auto finalize = [&](RecordedCallee& entry) {
        if (!entry.closure)
            return;
        if (hasReturned(vm, *this, entry) || (!liesInFrame(entry) && !vm.heap.isMarked(closureOnHeapOf(entry)))) {
            entry = { 0, nullptr };
            return;
        }
        entry.closure = std::bit_cast<JSObject*>(std::bit_cast<uintptr_t>(entry.closure) | markOfCollection);
        isInUse = true;
    };
    std::span<RecordedCallee> entries = recordedCallees();
    if (!vm.isEntered()) {
        if (entries.size() <= leastNumberOfRecordedCallees)
            zeroSpan(entries);
    } else if (phaseOfLastWalkOfRecordedCallees && *phaseOfLastWalkOfRecordedCallees + 1 == phaseOfCollector) {
        for (uint32_t place : placesFilledAtLastWalk)
            finalize(entries[place]);
    } else
        std::ranges::for_each(entries, finalize);
    entryCountAtLastCollection = vm.entryCountFromOutside();
    if (!isInUse && recordedCallees().size() > leastNumberOfRecordedCallees)
        setRecordedCalleeStorage(vm, *this, { });
}


static String nullIfEmpty(const String& text)
{
    return text.isEmpty() ? String() : text;
}

static String ownStringProperty(JSObject* closure, const Identifier& name)
{
    if (auto* classObject = dynamicDowncast<Closure>(closure)) {
        const FieldData* field = StaticFields::findOwnField(*classObject, name.impl());
        if (String string = field ? ProgramStatics::readStringWithoutAllocating(classObject->state().instance, field->slot) : String(); !string.isNull())
            return string;
    }
    unsigned attributes;
    PropertyOffset offset = closure->structure()->getConcurrently(name.impl(), attributes);
    if (offset == invalidOffset || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessorOrValue))
        return String();
    JSValue value = closure->getDirect(offset);
    return value && value.isString() ? asString(value)->tryGetValueWithoutGC() : String();
}

static String ownTextProperty(JSObject* closure, const Identifier& name)
{
    return nullIfEmpty(ownStringProperty(closure, name));
}

// The look was noted in the cell, and nothing has asked for more than the value of the property since.
static bool hasInitialNameWithoutProperty(JSObject* callee)
{
    if (auto* slim = dynamicDowncast<SlimClosure>(callee))
        return slim->nameWasLookedFor();
    auto* closure = dynamicDowncast<Closure>(callee);
    if (!closure || !closure->nameWasLookedFor())
        return false;
    FunctionRareData* rareData = closure->rareData();
    return !rareData || !rareData->hasReifiedName();
}

static void markRenamed(JSObject* closure)
{
    if (auto* full = dynamicDowncast<Closure>(closure))
        full->markRenamed();
    else
        uncheckedDowncast<ReifiedSlimClosure>(closure)->markRenamed();
}

void didChangeDisplayName(VM& vm, JSObject* closure)
{
    FunctionID closureFunction = *functionOf(closure);
    FunctionTables& tables = FunctionTables::of(vm);
    FunctionID sourceFunction = sourceFunctionOf(closureFunction);
    if (FunctionDescription(closureFunction).recordsCalleeOnceRenamed()) {
        tables.recordCalleesOf(vm, closureFunction);
        markRenamed(closure);
    }
    if (!isNamedInFramesByFunction(closureFunction))
        return;
    if (hasInitialNameWithoutProperty(closure))
        didLookForNameProperty(vm, closure);
    auto* classObject = dynamicDowncast<Closure>(closure);
    bool isClassObject = classObject && classObject->classData();
    String text = isClassObject ? String() : ownStringProperty(closure, vm.propertyNames->displayName);
    bool mayHaveDisplayName = isClassObject || !text.isNull();
    State* state = &State::of(closure->structure()->realm());
    Locker locker { tables.namesLock };
    if (!mayHaveDisplayName && !tables.displayNames.contains(sourceFunction))
        return;
    auto& entry = tables.displayNames.add(sourceFunction, FunctionTables::DisplayName { }).iterator->value;
    if (entry.isOneOfSeveral)
        return;
    if (entry.functionObject && entry.functionObject != closure) {
        if (mayHaveDisplayName)
            entry = { .state = entry.state == state ? state : nullptr, .isOneOfSeveral = true };
        return;
    }
    entry.functionObject = closure;
    entry.text = WTF::move(text);
    entry.state = state;
    if (isClassObject)
        entry.constructorOfClass = closureFunction;
    tables.hasDisplayNames.store(true, std::memory_order_relaxed);
}

void FunctionTables::forgetDisplayNamesOf(const State& state)
{
    Locker locker { namesLock };
    displayNames.removeIf([&](auto& entry) {
        return entry.value.state == &state;
    });
}

static String displayNameInFrames(VM& vm, FunctionID function)
{
    FunctionTables* tables = existingTables(vm);
    if (!tables || !tables->hasDisplayNames.load(std::memory_order_relaxed)) [[likely]]
        return String();
    FunctionID sourceFunction = sourceFunctionOf(function);
    Locker locker { tables->namesLock };
    auto entry = tables->displayNames.find(sourceFunction);
    if (entry == tables->displayNames.end() || entry->value.isOneOfSeveral)
        return String();
    if (!entry->value.constructorOfClass)
        return entry->value.text;
    JSCell* classObject = entry->value.state->objects->closures.closureIfCreated(entry->value.constructorOfClass);
    return classObject == entry->value.functionObject ? ownStringProperty(asObject(classObject), vm.propertyNames->displayName) : String();
}

static String functionNameAsWritten(const FunctionDescription& description)
{
    FunctionKindData kind = functionKindData(description.function());
    if (description.codeType() == ModuleCode || kind.isCommonJSModule)
        return emptyString();
    String written = writtenFunctionName(description.function());
    return kind.isBound() ? makeString("bound "_s, written) : written;
}

// The engine names a frame by the property `name` that the function object got when the literal was evaluated, "get " and "set " included. The frame of a generator has no function object to ask.
static String nameInFramesOfFunctionNamedByComputedKey(FunctionID function)
{
    FunctionID source = sourceFunctionOf(function);
    return functionKindData(source).isGenerator ? emptyString() : initialNameProperty(source);
}

// The engine shows the property `name` once the function object has it as a property, which is when it is defined.
static String nameGivenTo(VM& vm, JSObject* callee)
{
    FunctionID function = *functionOf(callee);
    if (String name = nameShownUntilLookedFor(vm, function); !name.isNull())
        return name;
    // Such a closure has the property from the first look for it on. A slim closure of any other kind has it from its first property on, whichever that is.
    bool hasNameOnceLookedFor = functionKindData(function).notesLooksForName();
    String name = hasInitialNameWithoutProperty(callee) ? nullIfEmpty(initialNameProperty(function)) : ownTextProperty(callee, vm.propertyNames->name);
    if (!name.isNull() && (hasNameOnceLookedFor || name != initialNameProperty(function)))
        return name;
    if (!hasNameOnceLookedFor) {
        if (String name = nameLookedFor(vm, function); !name.isNull())
            return name;
    }
    return ownTextProperty(callee, vm.propertyNames->displayName);
}

// The engine runs the body of a generator, and of an async function that awaits, as a function of its own. It has the name that is written in the function and none that the function takes from a variable or a property.
static bool runsInBodyWithoutName(const FunctionDescription& description)
{
    FunctionKindData kind = functionKindData(sourceFunctionOf(description.function()));
    return kind.hasInferredName && (kind.isGenerator || (kind.isAsync && description.isNamedByOwner()));
}

// So does the body of an async function that may wait: what the function object is called says nothing about it.
static bool runsInBodyThatMayWait(const FunctionDescription& description)
{
    return description.isAsync() && description.rejectsOnThrow() && description.visibility() == TraceVisibility::Visible;
}

String functionName(VM& vm, const FunctionDescription& description, NameForm form, JSObject* callee, bool calleeLiesInFrame)
{
    FunctionID function = description.function();
    switch (form) {
    case NameForm::AsWritten:
        return functionNameAsWritten(description);
    case NameForm::InFrames:
        if (runsInBodyWithoutName(description))
            return emptyString();
        if (callee) [[unlikely]] {
            if (String name = nameGivenTo(vm, callee); !name.isNull())
                return name;
        } else if (!runsInBodyThatMayWait(description)) {
            String nameSet = calleeLiesInFrame ? String() : nameSetAtRunTime(vm, function);
            if (!nameSet.isEmpty()) [[unlikely]]
                return nameSet;
            // A text without characters: the function object has no such property any more, or one that names nothing.
            if (String name = nameSet.isNull() ? nameLookedFor(vm, function) : String(); !name.isNull()) [[unlikely]]
                return name;
            if (String displayName = calleeLiesInFrame ? String() : displayNameInFrames(vm, function); !displayName.isEmpty()) [[unlikely]]
                return displayName;
        }
        if (description.isNamedByComputedKey()) [[unlikely]]
            return nameInFramesOfFunctionNamedByComputedKey(function);
        return functionNameAsWritten(description);
    case NameForm::CalculatedDisplayName: {
        if (runsInBodyWithoutName(description))
            return emptyString();
        String displayName = callee ? ownStringProperty(callee, vm.propertyNames->displayName) : calleeLiesInFrame || runsInBodyThatMayWait(description) ? String() : displayNameInFrames(vm, function);
        if (!displayName.isNull()) [[unlikely]]
            return displayName;
        return description.isNamedByComputedKey() ? emptyString() : functionNameAsWritten(description);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static String computedFunctionName(JSGlobalObject* globalObject, const Identifier& key, MethodKind kind)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = key.string();
    if (UniquedStringImpl* uid = key.impl(); uid->isSymbol()) {
        auto& symbol = static_cast<SymbolImpl&>(*uid);
        if (symbol.isPrivate())
            name = StringImpl::createSubstringSharingImpl(symbol, 0, symbol.length());
        else
            name = symbol.isNullSymbol() ? emptyString() : tryMakeString('[', StringView(uid), ']');
        if (!name) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope, "Function name is too long"_s);
            return { };
        }
    }
    if (kind == MethodKind::Method)
        return name;
    bool isGetter = kind == MethodKind::Getter;
    name = tryMakeString(isGetter ? "get "_s : "set "_s, name);
    if (!name) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope, isGetter ? "Getter name is too long"_s : "Setter name is too long"_s);
    return name;
}

static void nameFramesOfFunction(VM& vm, JSObject* closure, const String& name)
{
    FunctionID function = *functionOf(closure);
    if (!isNamedInFramesByFunction(function))
        return;
    FunctionTables& tables = FunctionTables::of(vm);
    Locker locker { tables.namesLock };
    tables.namesSetAtRunTime.set(sourceFunctionOf(function), name);
    tables.hasNamesSetAtRunTime.store(true, std::memory_order_release);
}

void didDefineNameProperty(VM& vm, JSObject* closure, const String& name)
{
    FunctionID function = *functionOf(closure);
    if (FunctionDescription(function).recordsCalleeOnceRenamed()) {
        FunctionTables::of(vm).recordCalleesOf(vm, function);
        markRenamed(closure);
    }
    nameFramesOfFunction(vm, closure, name);
}

void didDeleteNameProperty(VM& vm, JSObject* closure)
{
    didDefineNameProperty(vm, closure, emptyString());
}

void didLookForNameProperty(VM& vm, JSObject* closure)
{
    FunctionID function = *functionOf(closure);
    FunctionID sourceFunction = sourceFunctionOf(function);
    if (functionKindData(function).isBound() || functionKindData(sourceFunction).isGenerator)
        return;
    FunctionTables& tables = FunctionTables::of(vm);
    // Other function objects of the accessor keep the name without "get " or "set " in their frames.
    if (functionKindData(function).accessor() != MethodKind::Method && FunctionDescription(function).recordsCalleeOnceRenamed() && !dynamicDowncast<SlimClosure>(closure)) {
        tables.recordCalleesOf(vm, function);
        markRenamed(closure);
        return;
    }
    Locker locker { tables.namesLock };
    if (!tables.namesLookedFor.add(sourceFunction.index()))
        return;
    tables.hasNamesLookedFor.store(true, std::memory_order_release);
    tables.namesShownUntilLookedFor.remove(sourceFunction);
}

void showNameUntilLookedFor(VM& vm, JSObject* closure, const String& name)
{
    FunctionID function = *functionOf(closure);
    FunctionID sourceFunction = sourceFunctionOf(function);
    if (functionKindData(function).isBound() || functionKindData(sourceFunction).isGenerator)
        return;
    FunctionTables& tables = FunctionTables::of(vm);
    Locker locker { tables.namesLock };
    if (tables.namesLookedFor.contains(sourceFunction.index()))
        return;
    tables.namesShownUntilLookedFor.set(sourceFunction, name);
    tables.hasNamesSetAtRunTime.store(true, std::memory_order_release);
}

void setComputedFunctionName(VM& vm, JSObject* closure, const Identifier& key, MethodKind kind)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    String name = computedFunctionName(closure->realm(), key, kind);
    RETURN_IF_EXCEPTION(scope, void());
    CompiledCallee::reifyProperties(vm, closure);
    closure->putDirect(vm, vm.propertyNames->name, jsString(vm, name), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
    if (isCompiledClosure(closure)) {
        FunctionRareData* rareData = uncheckedDowncast<Closure>(closure)->ensureRareData(vm);
        rareData->setHasReifiedName();
        rareData->setHasModifiedNameForBoundOrNonHostFunction();
    }
    nameFramesOfFunction(vm, closure, name);
}

// The entries of the functions that implement the closures of the source.
static std::span<const FunctionTextEntry> functionTextEntriesOfSource(const ProgramTables& tables, uint32_t source)
{
    auto texts = tables.functionTexts();
    if (texts.empty())
        return { };
    std::span entries { tables.at<FunctionTextEntry>(texts[0].entriesOffset), texts[0].numberOfEntries };
    auto found = std::ranges::equal_range(entries, source, { }, [&](const FunctionTextEntry& entry) { return tables.sourceFunctionOf(entry.function); });
    return { found.begin(), found.end() };
}

static std::span<const Latin1Character> bytesOf(const ProgramTables& tables, const FunctionTextEntry& entry)
{
    return { tables.at<Latin1Character>(tables.functionTexts()[0].textOffset) + entry.start, entry.end - entry.start };
}

std::span<const Latin1Character> functionSourceBytes(FunctionID function)
{
    const ProgramTables& tables = programTables();
    std::span entries = functionTextEntriesOfSource(tables, tables.sourceFunctionOf(function.index()));
    auto entry = std::ranges::find(entries, function.index(), &FunctionTextEntry::function);
    if (entry == entries.end())
        return { };
    return bytesOf(tables, *entry);
}

std::span<const Latin1Character> functionSourceBytesOfSource(FunctionID source)
{
    const ProgramTables& tables = programTables();
    std::span entries = functionTextEntriesOfSource(tables, source.index());
    if (entries.empty())
        return { };
    return bytesOf(tables, entries.front());
}

JSString* functionSourceText(JSGlobalObject* globalObject, FunctionID function)
{
    VM& vm = globalObject->vm();
    FunctionTables::TextsByFunction& answered = FunctionTables::of(vm).sourceTexts;
    if (auto found = answered.find(function); found != answered.end()) {
        if (JSString* text = found->value.get())
            return text;
    }
    std::span bytes = functionSourceBytes(function);
    if (!bytes.data())
        return nullptr;
    JSString* text = charactersAreAllASCII(bytes) ? jsOwnedString(vm, StringImpl::createWithoutCopying(bytes)) : jsString(vm, String::fromUTF8ReplacingInvalidSequences(bytes));
    answered.set(function, Weak<JSString>(text));
    return text;
}

std::optional<uint32_t> closureNumberOf(const JSCell* cell)
{
    switch (cell->type()) {
    case SlimClosureType:
        return uncheckedDowncast<SlimClosure>(cell)->closureNumber();
    case SlimClosureWithPropertiesType:
        return uncheckedDowncast<ReifiedSlimClosure>(cell)->closureNumber();
    case CompiledClosureType:
        return uncheckedDowncast<Closure>(cell)->closureNumber();
    default:
        return std::nullopt;
    }
}

std::optional<FunctionID> functionOf(const JSCell* cell)
{
    std::optional<uint32_t> number = closureNumberOf(cell);
    return number ? std::optional(closureFunctionWithNumber(*number)) : std::nullopt;
}

static FunctionID checkedFunctionOf(const JSCell* callee)
{
    std::optional<FunctionID> function = functionOf(callee);
    RELEASE_ASSERT(function);
    return *function;
}

namespace CompiledCallee {

bool isCompiledCallee(const JSCell* cell)
{
    return isCompiledCalleeType(cell->type());
}

String name(JSObject* callee)
{
    return initialNameProperty(checkedFunctionOf(callee));
}

unsigned length(JSObject* callee)
{
    return functionLengthOf(checkedFunctionOf(callee));
}

bool canAssumeNameAndLengthAreOriginal(VM& vm, JSObject* callee)
{
    ASSERT(isSlimClosureType(callee->type()));
    if (callee->type() == SlimClosureType)
        return true;
    FunctionID function = checkedFunctionOf(callee);
    constexpr unsigned attributesAtCreation = PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum;
    unsigned attributes = 0;
    JSValue length = callee->getDirect(vm, vm.propertyNames->length, attributes);
    if (!length || attributes != attributesAtCreation || length != jsNumber(functionLengthOf(function)))
        return false;
    JSValue name = callee->getDirect(vm, vm.propertyNames->name, attributes);
    if (!name)
        return false;
    if (attributes == (attributesAtCreation | PropertyAttribute::CustomValue))
        return true;
    return attributes == attributesAtCreation && name.isString() && asString(name)->tryGetValueWithoutGC() == initialNameProperty(function);
}

JSValue sourceText(JSGlobalObject* globalObject, JSObject* callee)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    FunctionID function = checkedFunctionOf(callee);
    FunctionKindData kindData = functionKindData(function);
    CallableKind callableKind = kindData.callable();
    if (callableKind == CallableKind::Bound)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, "function "_s, writtenFunctionName(function), "() { [native code] }"_s));
    ASCIILiteral async = kindData.isAsync ? "async "_s : ""_s;
    bool isProgramFunction = !kindData.isInRuntimeModule;
    JSString* text = isProgramFunction ? functionSourceText(globalObject, function) : nullptr;
    RETURN_IF_EXCEPTION(scope, { });
    if (text)
        return text;
    if (isProgramFunction && callableKind == CallableKind::Arrow)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, "() => { [native code] }"_s));
    bool isWrittenWithoutName = callableKind == CallableKind::Arrow || (!isProgramFunction && kindData.hasInferredName);
    String name = isWrittenWithoutName ? emptyString() : initialNameProperty(function);
    if (kindData.isConstructor)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, "class "_s, name, name.isEmpty() ? ""_s : " "_s, "{ [native code] }"_s));
    if (isProgramFunction && callableKind == CallableKind::Method)
        RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, kindData.isGenerator ? "*"_s : ""_s, name, "() { [native code] }"_s));
    RELEASE_AND_RETURN(scope, jsMakeNontrivialString(globalObject, async, kindData.isGenerator ? "function* "_s : "function "_s, name, "() { [native code] }"_s));
}

JSString* sourceStringIfCreated(VM& vm, JSObject* callee)
{
    FunctionID function = checkedFunctionOf(callee);
    FunctionKindData kindData = functionKindData(function);
    if (kindData.isBound() || kindData.isInRuntimeModule)
        return nullptr;
    const FunctionTables::TextsByFunction& answered = FunctionTables::of(vm).sourceTexts;
    auto found = answered.find(function);
    return found == answered.end() ? nullptr : found->value.get();
}

bool isBoundFunction(JSObject* callee)
{
    return functionKindData(checkedFunctionOf(callee)).isBound();
}

FunctionKind kind(JSObject* callee)
{
    FunctionID function = checkedFunctionOf(callee);
    FunctionKindData kindData = functionKindData(function);
    if (kindData.isInRuntimeModule)
        return FunctionKind::Builtin;
    if (kindData.isConstructor)
        return FunctionKind::Class;
    CallableKind callableKind = kindData.callable();
    if (callableKind == CallableKind::Bound)
        return FunctionKind::Strict;
    if (kindData.isGenerator)
        return kindData.isAsync ? FunctionKind::AsyncGenerator : FunctionKind::Generator;
    if (kindData.isAsync)
        return FunctionKind::Async;
    switch (callableKind) {
    case CallableKind::Arrow:
        return FunctionKind::Arrow;
    case CallableKind::Method:
        return FunctionKind::Method;
    case CallableKind::Plain:
        return FunctionDescription(function).isStrict() ? FunctionKind::Strict : FunctionKind::Sloppy;
    case CallableKind::Bound:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

String ownDisplayName(JSObject* object)
{
    return ownStringProperty(object, object->vm().propertyNames->displayName);
}

String displayName(JSObject* object)
{
    std::optional<FunctionID> function = functionOf(object);
    if (!function)
        return String();
    if (FunctionDescription(*function).isNamedByComputedKey())
        return emptyString();
    // The engine shows a built-in function by the name in its own text alone.
    FunctionKindData kindData = functionKindData(*function);
    bool takesNameFromOwner = kindData.hasInferredName || kindData.accessor() != MethodKind::Method;
    if (kindData.isInRuntimeModule && takesNameFromOwner && !kindData.isBound())
        return emptyString();
    return writtenFunctionName(*function);
}

bool reifyProperties(VM& vm, JSObject* object)
{
    if (object->type() != SlimClosureType)
        return false;
    uncheckedDowncast<SlimClosure>(object)->reify(vm);
    return true;
}

void setPrototypeProperty(VM& vm, JSObject* callee, JSObject* prototype, unsigned attributes)
{
    RELEASE_ASSERT(isCompiledCallee(callee));
    if (callee->type() == CompiledClosureType) {
        uncheckedDowncast<Closure>(callee)->setPrototypeProperty(vm, prototype, attributes);
        return;
    }
    reifyProperties(vm, callee);
    callee->putDirect(vm, vm.propertyNames->prototype, prototype, attributes);
}

void setNameProperty(JSGlobalObject* globalObject, JSObject* callee, JSString* name, unsigned attributes)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    RELEASE_ASSERT(isCompiledCallee(callee));
    String text = name->value(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    if (callee->type() == CompiledClosureType)
        uncheckedDowncast<Closure>(callee)->setNameProperty(vm, name, attributes);
    else {
        reifyProperties(vm, callee);
        callee->putDirect(vm, vm.propertyNames->name, name, attributes);
    }
    didDefineNameProperty(vm, callee, text);
}

void showNameUntilLookedFor(VM& vm, JSObject* callee, const String& name)
{
    RELEASE_ASSERT(isCompiledCallee(callee));
    AOT::showNameUntilLookedFor(vm, callee, name);
}

Structure* cachedBoundFunctionStructure(JSObject* callee)
{
    if (!isSlimClosureType(callee->type()))
        return nullptr;
    return closureTableOf(callee).asyncBoundFunctionStructure();
}

void setCachedBoundFunctionStructure(VM&, JSObject* callee, Structure* structure)
{
    if (isSlimClosureType(callee->type()))
        closureTableOf(callee).setAsyncBoundFunctionStructure(structure);
}

} // namespace CompiledCallee

DeclaredAs declaredAs(JSCell* cell)
{
    std::optional<FunctionID> function = functionOf(cell);
    if (!function)
        function = compiledBuiltinFunctionID(cell);
    if (!function)
        return standsInForFunction(cell) ? DeclaredAs::Function : DeclaredAs::NotCompiled;
    FunctionKindData kindData = functionKindData(*function);
    if (kindData.isConstructor)
        return FunctionDescription(*function).isNakedConstructor() ? DeclaredAs::Function : DeclaredAs::Class;
    if (kindData.isBound())
        return DeclaredAs::Function;
    if (kindData.isGenerator)
        return kindData.isAsync ? DeclaredAs::AsyncGeneratorFunction : DeclaredAs::GeneratorFunction;
    return kindData.isAsync ? DeclaredAs::AsyncFunction : DeclaredAs::Function;
}

String closureDeclaredName(JSObject* object)
{
    std::optional<FunctionID> function = functionOf(object);
    if (!function)
        return String();
    FunctionKindData kindData = functionKindData(*function);
    bool takesNameFromOwner = kindData.hasInferredName || kindData.accessor() != MethodKind::Method;
    if (takesNameFromOwner && !kindData.isBound())
        return emptyString();
    return writtenFunctionName(*function);
}

bool belongsToBuiltinModule(JSCell* cell)
{
    std::optional<FunctionID> function = functionOf(cell);
    return function && functionKindData(*function).isInRuntimeModule;
}

bool isCompiledClassObject(JSFunction* candidate)
{
    std::optional<FunctionID> function = functionOf(candidate);
    return function && functionKindData(*function).isConstructor;
}

bool isClosureOf(JSCell* cell, FunctionID function)
{
    BuiltinID calledBuiltin = builtinOfFunctionInFrames(function);
    if (std::optional<FunctionID> closureFunction = functionOf(cell))
        return !calledBuiltin && sourceFunctionOf(*closureFunction) == sourceFunctionOf(function);
    std::optional<uint32_t> implemented = calledBuiltin ? std::optional<uint32_t>(calledBuiltin.index()) : programTables().builtinImplementedBy(function.index());
    JSObject* object = cell->getObject();
    if (!implemented || !object)
        return false;
    // A stack walk asks, and allocates nothing. A function object that is yet to be created is not this one.
    JSGlobalObject* realm = object->realmMayBeNull();
    return realm && findExistingBuiltin(realm, BuiltinID(*implemented)) == object;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
