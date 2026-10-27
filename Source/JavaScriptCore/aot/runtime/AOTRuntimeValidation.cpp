/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeValidation.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTBuiltinCells.h"
#include "AOTCellFormat.h"
#include "AOTClassTable.h"
#include "AOTCompiledCallee.h"
#include "AOTEffectsDeclared.h"
#include "AOTFunctionTables.h"
#include "AOTHeapHooks.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTNativeCallValidation.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "AOTStubDescriptor.h"
#include "AOTThreshold.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "MarkedBlockInlines.h"
#include "PreciseAllocation.h"
#include "Reg.h"
#include "SubspaceInlines.h"
#include <array>
#include <wtf/DataLog.h>
#include <wtf/HexNumber.h>
#include <wtf/Lock.h>
#include <wtf/RawHex.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

void willReadLegacyRegExpStatics(VM& vm)
{
    if (validatesRuntime(vm) && vm.aotState().image->programTables().flags & ProgramTables::neverReadsLegacyRegExpStatics)
        crashOnValidationFailure(vm, { ValidationFailureKind::LegacyRegExpStaticRead, { }, { }, JSValue() });
}

static constexpr unsigned maxNumberOfTestedElements = 64;
AOT_THRESHOLD(unsigned, maxNumberOfElementsTestedAgain, 8);

static void dumpClassOf(PrintStream& out, JSValue found)
{
    if (!found || !found.isCell()) {
        out.print(!found ? "the empty value" : found.isUndefined() ? "undefined" : found.isNull() ? "null" : found.isBoolean() ? "boolean" : found.isInt32() ? "int32" : found.isDouble() ? "double" : "invalid encoding");
        return;
    }
    JSCell* cell = found.asCell();
    JSType type = cell->type();
    if (const char* word = type == StringType ? "string" : type == SymbolType ? "symbol" : type == HeapBigIntType ? "bigint" : type == CompiledEnvironmentType ? "environment" : nullptr)
        out.print(word);
    else
        out.print(cell->isObject() ? "object " : "cell ", cell->classInfo()->className);
}

// Numbers and kinds only: what a program holds is not for a log.
static bool holdsType(Instance& instance, const RuntimeType& type, JSValue held, unsigned position)
{
    if (instance.state->objects->layouts.valueHasType(type, held))
        return true;
    dataLog("AOT: number ", position, " of what the container holds is outside a type of ", type.literalStrings().size(), " texts and ", type.cellTests().size(), " tests of cells: ");
    dumpClassOf(WTF::dataFile(), held);
    dataLogLn();
    return false;
}

template<typename Collection>
static bool entriesHaveType(Instance& instance, Collection* collection, Typed::ContentsSlot slot, const RuntimeType& type)
{
    JSCell* storage = collection->storage();
    typename Collection::Helper::Entry entry = 0;
    // Deleted entries stay in the table until it is rebuilt, and the walk passes them one by one.
    for (unsigned numberOfTestedEntries = 0; storage && entry < maxNumberOfTestedElements; ++numberOfTestedEntries) {
        auto next = Collection::Helper::transitAndNext(*instance.vm, *storage, entry);
        if (!next.storage)
            return true;
        if (!holdsType(instance, type, slot == Typed::ContentsSlot::Value ? next.value : next.key, numberOfTestedEntries))
            return false;
        storage = next.storage;
        entry = next.entry + 1;
    }
    return true;
}

static bool anyProperty(VM& vm, Structure* structure, const auto& predicate)
{
    bool isFound = false;
    structure->forEachProperty(vm, [&](const auto& entry) {
        isFound = predicate(entry);
        return !isFound;
    });
    return isFound;
}

// A data property under a name like "-1" costs the structure its mark and leaves true what the storage aspect says.
static bool hasOrdinaryElements(VM& vm, JSValue value)
{
    auto* array = dynamicDowncast<JSArray>(value);
    if (!array)
        return false;

    Structure* structure = array->structure();
    if (!array->canDoFastIndexedAccess() || structure->realm()->isHavingABadTime() || hasSlowPutArrayStorage(structure->indexingType()) || !structure->isStructureExtensible() || !structure->hasMonoProto())
        return false;
    return !structure->hasAnyKindOfGetterSetterProperties() || !anyProperty(vm, structure, [](const auto& entry) {
        return (entry.attributes() & PropertyAttribute::AccessorOrCustomAccessorOrValue) && isCanonicalNumericIndexString(entry.key());
    });
}

// The array may have thousands of names and is asked at every use, so it is asked for the few names that would count: the members of its prototypes and what built-ins probe for.
static bool shadowsLockedPrototypeChain(VM& vm, Structure* structure)
{
    auto hasOwn = [&](PropertyName name) {
        return isValidOffset(structure->get(vm, name));
    };
    // Adding a symbol clears the permission, in a dictionary too.
    bool hasOwnSymbol = !structure->isQuickPropertyAccessAllowedForEnumeration() && anyProperty(vm, structure, [](const auto& entry) {
        return entry.key()->isSymbol();
    });
    if (hasOwn(vm.propertyNames->then) || hasOwn(vm.propertyNames->toJSON) || hasOwnSymbol)
        return true;

    for (JSObject* prototype = structure->storedPrototypeObject(); prototype; prototype = prototype->getPrototypeDirect().getObject()) {
        Structure* prototypeStructure = prototype->structure();
        if (!prototypeStructure->hasImmutablePrototype() || !prototypeStructure->hasImmutableIndexedProperties() || prototypeStructure->hasNonReifiedStaticProperties())
            return true;
        bool shadowsMember = anyProperty(vm, prototypeStructure, [&](const auto& entry) {
            return hasOwn(entry.key());
        });
        if (shadowsMember)
            return true;
    }
    return false;
}

// The bit of the cell is clear where the elements alone are not pristine. A structure that has been a dictionary carries no mark.
static bool hasPristineNames(VM& vm, JSValue value)
{
    if (!value.isCell())
        return false;
    Structure* structure = value.asCell()->structure();
    if (structure->isPristine())
        return true;
    if (!value.inherits<JSArray>() || !structure->hasBeenDictionary() || !structure->hasMonoProto() || structure->storedPrototype() != structure->realm()->arrayPrototype())
        return false;
    return !shadowsLockedPrototypeChain(vm, structure);
}

// The pointers are only compared.
struct SampledArray {
    const JSCell* array;
    const RuntimeType* type;
    uint64_t numberOfSamples;
};

struct SampledArrays {
    std::array<SampledArray, 4> arrays { };
    unsigned numberOfReplacements { 0 };
};

static SampledArrays& sampledArraysOfThread();

static uint64_t numberOfEarlierSamples(const JSCell* array, const RuntimeType& type)
{
    SampledArrays& earlier = sampledArraysOfThread();
    for (SampledArray& sampled : earlier.arrays) {
        if (sampled.array == array && sampled.type == &type)
            return ++sampled.numberOfSamples;
    }
    earlier.arrays[earlier.numberOfReplacements++ % earlier.arrays.size()] = { array, &type, 0 };
    return 0;
}

static bool contentsHaveType(Instance& instance, JSCell* container, Typed::ContentsSlot slot, const RuntimeType& type)
{
    if (auto* map = dynamicDowncast<JSMap>(container))
        return slot == Typed::ContentsSlot::Element || entriesHaveType(instance, map, slot, type);
    if (auto* set = dynamicDowncast<JSSet>(container))
        return slot != Typed::ContentsSlot::Element || entriesHaveType(instance, set, slot, type);

    auto* array = dynamicDowncast<JSArray>(container);
    if (!array || slot != Typed::ContentsSlot::Element)
        return true;

    // A loop checks the same long array on every iteration. Samples after the first are small, and each starts one element later, so together they cover the array.
    uint64_t length = array->length();
    uint64_t numberOfEarlier = length > maxNumberOfTestedElements ? numberOfEarlierSamples(array, type) : 0;
    uint64_t stride = std::max<uint64_t>(1, length / (numberOfEarlier ? maxNumberOfElementsTestedAgain : maxNumberOfTestedElements));
    for (uint64_t position = numberOfEarlier % stride; position < length; position += stride) {
        unsigned index = static_cast<unsigned>(position);
        JSValue element = array->tryGetIndexQuickly(index);
        if (element && !holdsType(instance, type, element, index))
            return false;
    }
    return true;
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

static void dumpCalledCode(PrintStream& out, JSValue found)
{
    uint32_t bits = static_cast<uint32_t>(found.asInt32());
    uint32_t number = bits & CalledCode::maxNumber;
    switch (static_cast<CalledCode::Kind>(bits >> CalledCode::kindShift)) {
    case CalledCode::Kind::Operation:
        out.print(nameOf(static_cast<Entry>(number)));
        return;
    case CalledCode::Kind::Stub:
        out.print("stub ", descriptorOf(static_cast<StubFamily>(number)).name);
        return;
    case CalledCode::Kind::Function:
        out.print("f", number);
        return;
    case CalledCode::Kind::Address:
        out.print("a function value");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void crashOnValidationFailure(VM&, const ValidationFailure& failure)
{
    // Never released: another thread that fails waits here until this one has crashed.
    static Lock lock;
    lock.lock();

    // Describing a damaged cell can fault, so the kind and the ids are printed first.
    auto numberOrDash = [](auto id) {
        return id ? String::number(id.index()) : "-"_str;
    };
    dataLog("AOT validation: ", nameOf(failure.kind), " f", numberOrDash(failure.function), " v", numberOrDash(failure.value), " ");
    if (failure.kind == ValidationFailureKind::CallChangedKeptRegister || failure.kind == ValidationFailureKind::UnwindingChangedKeptRegister)
        dumpChangedRegister(WTF::dataFile(), failure.found);
    else if (failure.kind == ValidationFailureKind::CallCollected)
        dumpCalledCode(WTF::dataFile(), failure.found);
    else
        dumpClassOf(WTF::dataFile(), failure.found);
    dataLogLn();
    if (ASCIILiteral word = disableOf(failure.kind); !word.isEmpty())
        dataLogLn("AOT: the check belongs to --aot-disable=", word);
    writeCheckedNativeCalls();
    CRASH_WITH_INFO(static_cast<uint64_t>(failure.kind), failure.function.index(), failure.value.index());
}

struct Photograph {
    JSObject* object { nullptr };
    AbstractHeaps written;
    OperandSnapshot before;
};

struct ValidationOfThread {
    std::array<Photograph, PhotographedCall::maxOperands> photographs;
    SampledArrays sampledArrays;
};

static ValidationOfThread& validationOfThread()
{
    static thread_local auto* validation = new ValidationOfThread;
    return *validation;
}

// One call of a thread is photographed at a time: it runs no other compiled code. A call that throws leaves its photographs behind, and the next one takes their places.
static std::span<Photograph, PhotographedCall::maxOperands> photographsOfThread()
{
    return validationOfThread().photographs;
}

static SampledArrays& sampledArraysOfThread()
{
    return validationOfThread().sampledArrays;
}

void photographOperand(VM& vm, JSValue operand, AbstractHeaps written, unsigned position)
{
    DeferGCForAWhile deferGC(vm);
    auto photographs = photographsOfThread();
    for (Photograph& later : photographs.subspan(position))
        later.object = nullptr;
    if (canCapture(operand))
        photographs[position] = { asObject(operand), written, capture(asObject(operand)) };
}

void compareOperandWithPhotograph(VM& vm, JSValue operand, uint32_t positionAndCallee, FunctionID function, Typed::ValueID value)
{
    DeferGCForAWhile deferGC(vm);
    unsigned position = positionAndCallee & ((1u << PhotographedCall::calleeShift) - 1);
    auto photographs = photographsOfThread();
    const Photograph& photograph = photographs[position];
    JSObject* object = photograph.object;
    if (!object || operand != JSValue(object))
        return;
    AbstractHeaps written;
    for (const Photograph& other : photographs) {
        if (other.object == object)
            written.add(other.written);
    }

    bool isArray = isJSArray(object);
    for (const OperandDifference& difference : differencesBetween(vm, object, photograph.before, capture(object))) {
        AbstractHeaps heaps = difference.change == OperandChange::InternalStateChanged ? heapsOfClassOf(object) : rowOf(difference.change).heaps;
        // `{ 0: x }` defines a property under the name "0". Whoever reads an element of an object that is no array reads its named properties too.
        if (!isArray && (difference.change == OperandChange::ElementsChanged || difference.change == OperandChange::ArrayStorageChanged))
            heaps.add(AbstractHeap::NamedProperties);
        // Only an array has a storage that is read apart from its elements.
        if (!isArray && difference.change == OperandChange::ArrayStorageChanged)
            heaps.add(AbstractHeap::Elements);
        if (heaps.isEmpty() || written.containsAny(heaps))
            continue;
        String message = makeString("operand "_s, position, ": "_s, describe(difference), ", the call writes "_s, describeMembers(written), " through it, not "_s, describeMembers(heaps), " (f"_s, function.index(), " v"_s, value.index(), ')');
        uint32_t callee = positionAndCallee >> PhotographedCall::calleeShift;
        String nameOfCallee = callee & PhotographedCall::calleeIsStub ? makeString("stub "_s, descriptorOf(static_cast<StubFamily>(callee & ~PhotographedCall::calleeIsStub)).name) : String(nameOf(static_cast<Entry>(callee)));
        reportOrCrash(vm, { ValidationFailureKind::OperationWroteHeap, function, value, object }, "operation"_s, nameOfCallee, message);
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPhotographOperand, void, (Instance* instance, EncodedJSValue operand, uint64_t writtenHeaps, uint32_t position))
{
    photographOperand(*instance->vm, JSValue::decode(operand), AbstractHeaps::fromRaw(writtenHeaps), position);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCompareOperandWithPhotograph, void, (Instance* instance, EncodedJSValue operand, uint32_t positionAndCallee, uint32_t function, uint32_t valueIndex))
{
    compareOperandWithPhotograph(*instance->vm, JSValue::decode(operand), positionAndCallee, FunctionID(function), Typed::ValueID(valueIndex));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNoteFailedGuard, void, (Instance*, uint32_t function, uint32_t opcode, uint32_t ordinal))
{
    static Lock lock;
    Locker locker { lock };
    static auto* noted = new HashSet<uint64_t>;
    constexpr unsigned bitsOfOrdinal = 20;
    constexpr unsigned bitsOfOpcode = 12;
    uint64_t guard = (static_cast<uint64_t>(function) << (bitsOfOpcode + bitsOfOrdinal)) | (static_cast<uint64_t>(opcode) << bitsOfOrdinal) | (ordinal & ((1u << bitsOfOrdinal) - 1));
    if (noted->add(guard + 1).isNewEntry)
        dataLogLn("AOT guard failed: f", function, " ", nameOf(static_cast<Typed::Opcode>(opcode)), " ", ordinal);
}

#if ENABLE(DFG_DOES_GC_VALIDATION)
thread_local uint64_t placesThatMayCollect;
#endif

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCountStopsForCollector, uint64_t, (Instance* instance))
{
#if ENABLE(DFG_DOES_GC_VALIDATION)
    return instance->vm->heap.mutatorExecutionVersion() + placesThatMayCollect;
#else
    return instance->vm->heap.mutatorExecutionVersion();
#endif
}

#if ASSERT_ENABLED
thread_local CallerFrameAndPC* NoCollectionGuard::s_frameRecord;
#endif

OperationPermissions permissionsOf(Entry entry)
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    if (!operation)
        return { };
    Typed::Effects effects = Typed::declaredBoundOf(*operation);
    return { .mayEnterVM = effects.mayCallUnknownCode, .mayCollect = effects.mayCollect };
}

CollectionVersions collectionVersionsOf(VM& vm)
{
    MarkedSpace& space = vm.heap.objectSpace();
    return { space.markingVersion(), space.edenVersion(), space.newlyAllocatedVersion() };
}

NEVER_INLINE void OperationValidationScope::open(Entry entry)
{
    OperationPermissions permissions = permissionsOf(entry);
    m_entry = entry;
    m_mayNotEnterVM = !permissions.mayEnterVM;
    m_mayNotCollect = !permissions.mayCollect;
    m_collectionVersions = collectionVersionsOf(m_vm);
    m_didEnterVMBefore = std::exchange(m_vm.didEnterVM, false);
    m_isOpen = true;
}

NEVER_INLINE void OperationValidationScope::close()
{
    bool didEnterVM = m_mayNotEnterVM && m_vm.didEnterVM;
    // An operation that throws allocates what it throws, and the head of every handler counts on a collection.
    bool didCollect = m_mayNotCollect && !m_vm.exceptionForInspection() && collectionVersionsOf(m_vm) != m_collectionVersions;
    if (didEnterVM || didCollect) {
        dataLogLn("AOT: ", nameOf(m_entry), " is declared not to ", didEnterVM ? "enter the VM" : "collect");
        crashOnValidationFailure(m_vm, { didEnterVM ? ValidationFailureKind::OperationEnteredVM : ValidationFailureKind::OperationCollected, { }, { }, { } });
    }
    m_vm.didEnterVM |= m_didEnterVMBefore;
}

static bool stillPassesTest(State& state, const RuntimeCellTest& test, JSCell* cell)
{
    // What was pristine when it was stored need not be so now.
    if (test.kind == CellTestKind::IsPristine)
        return true;
    // To look up a built-in that nothing has asked for yet may create it.
    if (test.kind == CellTestKind::IsBuiltinCell) {
        JSCell* builtin = unsafeMakeSpan(state.instance.prefix().builtinCells, state.tables.builtinCells().size())[test.builtinCellNumber()];
        return !builtin || builtin == cell;
    }
    return state.objects->layouts.cellPassesTest(test, cell);
}

static bool slotMayHold(State& state, const RuntimeType& type, JSValue value)
{
    OptionSet<PrimitiveSet> primitives = type.primitiveSet();
    JSCell* cell = value.isCell() ? value.asCell() : nullptr;
    // To compare a rope with the literals of a type is to flatten it.
    if (cell && cell->isString() && asString(cell)->isRope())
        return primitives.containsAny({ PrimitiveSet::String, PrimitiveSet::LiteralString });
    if (!cell || cell->isHeapBigInt() || cell->isString())
        return state.objects->layouts.valueHasType(type, value);
    if (cell->isSymbol() && primitives.containsAny({ PrimitiveSet::Symbol, PrimitiveSet::LiteralSymbol }))
        return true;
    if (!cell->isSymbol() && !cell->isObject())
        return false;
    return std::ranges::any_of(type.cellTests(), [&](const RuntimeCellTest& test) {
        return stillPassesTest(state, test, cell);
    });
}

// The cell of a literal string is created when something reads the slot.
static bool slotMayHoldLiteralString(const Image& image, const RuntimeType& type, uint64_t word)
{
    std::span<const uint32_t> literals = type.literalStrings();
    if (literals.empty())
        return type.primitiveSet().contains(PrimitiveSet::String);
    if (!type.primitiveSet().containsAny({ PrimitiveSet::String, PrimitiveSet::LiteralString }))
        return false;
    const ImageText& text = image.text();
    StringID held(image.programTables().literalStrings()[literalStringIndex(static_cast<uint32_t>(word))]);
    if (std::ranges::find(literals, held.index()) != literals.end())
        return true;
    // Only an image that validates has the text of every word.
    if (!text.hasText(held))
        return !image.header().options.validates;
    StringView characters = text.view(held);
    return std::ranges::any_of(literals, [&](uint32_t literal) {
        return text.equals(StringID(literal), characters);
    });
}

// What lies at the address can be read.
static bool isCellOfHeap(VM& vm, const void* address)
{
    MarkedSpace& space = vm.heap.objectSpace();
    if (PreciseAllocation::isPreciseAllocation(static_cast<HeapCell*>(const_cast<void*>(address)))) {
        return space.preciseAllocations().containsIf([&](PreciseAllocation* allocation) {
            return allocation->cell() == address;
        });
    }
    MarkedBlock* block = MarkedBlock::blockFor(address);
    return block && MarkedBlock::isAtomAligned(address) && space.blocks().set().contains(block);
}

// Of bits that are no marker of the representation and no word of a literal string.
static bool isFormOfValue(VM& vm, SlotRepresentation representation, uint64_t bits, uintptr_t cageBase)
{
    SlotTraits traits = traitsOf(representation);
    switch (traits.valueKind) {
    case SlotValueKind::Boolean:
        return traits.hasAbsent ? bits == JSValue::ValueFalse || bits == JSValue::ValueTrue : bits <= 1;
    case SlotValueKind::SignedInteger:
        return representation != SlotRepresentation::OptionalInt32 || static_cast<int32_t>(bits) >= minInlineInt32;
    case SlotValueKind::UnsignedInteger:
    case SlotValueKind::Float:
    case SlotValueKind::Double:
        return true;
    // A slot that defers a closure holds no cell until something asks for the closure.
    case SlotValueKind::DeferredClosure:
        if (bits < minCell32)
            return true;
        break;
    case SlotValueKind::Boxed:
        if (!JSValue::decode(bits).isCell())
            return true;
        break;
    case SlotValueKind::Cell:
    case SlotValueKind::LiteralString:
        break;
    }
    if (traits.isNarrowReference)
        return bits >= minCell32 && isCellOfHeap(vm, MarkedBlockCage::decode(cageBase, static_cast<uint32_t>(bits)));
    return JSValue::decode(bits).isCell() && isCellOfHeap(vm, JSValue::decode(bits).asCell());
}

std::optional<SlotOutsideType> findSlotOutsideType(VM& vm, const Object& object)
{
    State& state = object.state();
    const LayoutData& layout = object.layout();
    for (const FieldData& field : layout.fields()) {
        // A brand is a class token, and what the slot of an accessor holds is not the value of the property. No type describes either.
        if (field.isBrand() || field.isAccessor())
            continue;
        const void* slot = object.slotOf(field);
        if (!slot)
            continue;

        SlotRepresentation representation = field.slot.representation();
        SlotTraits traits = traitsOf(representation);
        uint64_t bits = readSlotBits(representation, slot);
        // A slot that cannot say "absent" holds zero until its first store.
        if (bits == (traits.hasAbsent ? traits.absentBits : 0))
            continue;
        if ((traits.hasUndefined && bits == traits.undefinedBits) || (traits.hasNull && bits == traits.nullBits)) {
            JSValue marker = bits == traits.undefinedBits ? jsUndefined() : jsNull();
            if (!state.objects->layouts.slotAdmits(field, marker))
                return SlotOutsideType { &field, bits, true, marker };
            continue;
        }

        const RuntimeType& type = state.objects->layouts.typeOf(field);
        if (traits.valueKind == SlotValueKind::LiteralString && isLiteralStringWord(bits)) {
            if (!slotMayHoldLiteralString(*vm.aotState().image, type, bits))
                return SlotOutsideType { &field, bits, true, { } };
            continue;
        }
        if (!isFormOfValue(vm, representation, bits, cageBaseOf(&object)))
            return SlotOutsideType { &field, bits, false, { } };

        bool defersClosure = traits.valueKind == SlotValueKind::DeferredClosure;
        JSValue value = defersClosure ? JSValue(cellInSlot(representation, slot, cageBaseOf(&object))) : readSlot(representation, slot, *state.strings);
        if (!value)
            continue;
        // A hidden field holds the environment that the deferred closures of the object capture. It is no value of the language, so no type describes it.
        if (value.isCell() && value.asCell()->type() == CompiledEnvironmentType)
            continue;
        if (!slotMayHold(state, type, value))
            return SlotOutsideType { &field, bits, true, value };
    }
    return std::nullopt;
}

void validateSlotTypes(VM& vm, const Object& object)
{
    std::optional<SlotOutsideType> outside = findSlotOutsideType(vm, object);
    if (!outside) [[likely]]
        return;

    const LayoutData& layout = object.layout();
    const FieldData& field = *outside->field;
    const ImageText& text = vm.aotState().image->text();
    StringID name = text.identifier(field.name).text();
    dataLog("AOT: field ", layout.indexOf(field), " (", name && text.hasText(name) ? text.view(name) : StringView("-"_s), ") of layout ", layout.index, ", a slot of the kind ", nameOf(field.slot.representation()), " at ", static_cast<uint32_t>(field.slot.offset));
    if (outside->isFormOfValue)
        dataLogLn(", holds a value outside its type");
    else
        dataLogLn(", holds ", RawHex(outside->bits), ", which is neither a marker nor a value there");
    dataLog("AOT: the object has shape ", object.shape(), " and order word ", RawHex(object.orderWord()), "; the allocations of that shape:");
    std::span<const AllocationData> allocations = object.state().tables.allocations();
    for (uint32_t number = 0; number < allocations.size(); ++number) {
        if (allocations[number].shape == object.shape())
            dataLog(" ", number);
    }
    dataLogLn();
    crashOnValidationFailure(vm, { ValidationFailureKind::HeapSlotType, { }, { }, outside->found });
}

void CollectionValidator::didFinishCollection(VM& vm, CollectionScope scope)
{
    for (Instance* instance : vm.aotState().instances) {
        for (JSObject* object : instance->state->builtinCells->objectsWithEveryPropertyLocked()) {
            if (!object->structure()->hasImmutableProperties())
                crashOnValidationFailure(vm, { ValidationFailureKind::BuiltinNotImmutable, { }, { }, object });
        }
    }

    // Every compiled object lies in the space of cells.
    if (scope != CollectionScope::Full) {
        vm.cellSpace().forEachMarkedCell([&](HeapCell* heapCell, HeapCell::Kind) {
            if (Object* object = compiledObjectOrNull(static_cast<JSCell*>(heapCell)))
                validateSlotTypes(vm, *object);
        });
        return;
    }

    vm.heap.objectSpace().forEachSubspace([&](Subspace& subspace) {
        subspace.forEachMarkedCell([&](HeapCell* heapCell, HeapCell::Kind kind) {
            if (!isJSCellKind(kind))
                return;
            JSCell* cell = static_cast<JSCell*>(heapCell);
            validateShapeWord(cell);
            if (cell->type() == CompiledEnvironmentType)
                return;
            if (Object* object = compiledObjectOrNull(cell))
                validateSlotTypes(vm, *object);
            else if (auto* array = dynamicDowncast<JSArray>(cell))
                array->validatePristine(vm);
        });
        return IterationStatus::Continue;
    });
}

// An inquiry runs no code of the program.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTValidateLookup, size_t, (Instance* instance, JSCell* receiver, uint32_t key, JSCell* expected))
{
    VM& vm = *instance->vm;
    LeafOperationScope leaf(vm);
    DeferGCForAWhile deferGC(vm);
    DeferTerminationForAWhile deferTermination(vm);
    JSGlobalObject* globalObject = instance->globalObject;
    const Identifier& name = identifierAt(instance, key);
    JSObject* object = asObject(receiver);
    JSObject* firstObjectAsked = object;
    JSValue found;
    if (auto throughTable = instance->state->objects->classes.findThroughLazyPrototypes(vm, object, name.impl(), false)) {
        if (throughTable->member) {
            if (throughTable->member->isAccessor)
                return false;
            found = throughTable->member->holder->getDirect(throughTable->member->offset);
        } else if (!(firstObjectAsked = throughTable->firstObjectAbove))
            return false;
    }
    if (!found) {
        PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
        if (!firstObjectAsked->getPropertySlot(globalObject, name, slot) || !slot.isValue())
            return false;
        found = slot.getValue(globalObject, name);
    }
    if (!found.isCell())
        return false;
    if (found.asCell() == expected)
        return true;
    // Each evaluation of a class makes its own closures of one definition, and a definition has several compiled forms.
    std::optional<FunctionID> foundFunction = functionOf(found.asCell());
    std::optional<FunctionID> expectedFunction = functionOf(expected);
    return foundFunction && expectedFunction && sourceFunctionOf(*foundFunction) == sourceFunctionOf(*expectedFunction);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReportValidationFailure, void, (Instance* instance, EncodedJSValue value, uint32_t kind, uint32_t function, uint32_t valueIndex))
{
    RELEASE_ASSERT(kind < numberOfEnumerators<ValidationFailureKind>);
    DeferGCForAWhile deferGC(*instance->vm);
    auto failureKind = static_cast<ValidationFailureKind>(kind);
    if (failureKind == ValidationFailureKind::ArrayElementsNotOrdinary && hasOrdinaryElements(*instance->vm, JSValue::decode(value)))
        return;
    if (failureKind == ValidationFailureKind::ArrayNotPristine && hasPristineNames(*instance->vm, JSValue::decode(value)))
        return;
    crashOnValidationFailure(*instance->vm, { failureKind, Typed::FunctionID(function), Typed::ValueID(valueIndex), JSValue::decode(value) });
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTContentsHaveType, size_t, (Instance* instance, JSCell* container, uint32_t contentsSlot, uint32_t type))
{
    DeferGCForAWhile deferGC(*instance->vm);
    const RuntimeType* contents = instance->state->objects->layouts.typeWithIndex(type);
    RELEASE_ASSERT(contents);
    return contentsHaveType(*instance, container, static_cast<Typed::ContentsSlot>(contentsSlot), *contents);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReportBrokenClaim, void, (uint32_t claimAndOffset, uint32_t function, uint32_t valueIndex, const uint64_t* foundAndGiven))
{
    static constexpr ASCIILiteral names[] { "StackSlotIsWrittenBeforeRead"_s, "WordNeverChanges"_s, "LoadMayMove"_s, "ValueIsInRange"_s, "ComparisonIsDecided"_s };
    static_assert(std::size(names) == static_cast<size_t>(BrokenClaim::ComparisonIsDecided) + 1);
    uint32_t number = claimAndOffset & 0xff;
    RELEASE_ASSERT(number < std::size(names));
    auto claim = static_cast<BrokenClaim>(number);
    static Lock lock;
    lock.lock();
    dataLog("AOT claim check: ", names[number], " f", function, " v", static_cast<int32_t>(valueIndex));
    if (claim == BrokenClaim::ComparisonIsDecided)
        dataLog(" of 0x", hex(foundAndGiven[0], Lowercase), " and 0x", hex(foundAndGiven[1], Lowercase));
    else if (claim != BrokenClaim::ValueIsInRange)
        dataLog(" at +", static_cast<int32_t>(claimAndOffset) >> 8);
    if (claim == BrokenClaim::WordNeverChanges || claim == BrokenClaim::LoadMayMove)
        dataLog(" holds 0x", hex(foundAndGiven[0], Lowercase), ", not 0x", hex(foundAndGiven[1], Lowercase));
    dataLogLn();
    CRASH_WITH_INFO(number, function, valueIndex);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
