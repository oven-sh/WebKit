/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeCounters.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTBuiltinCells.h"
#include "AOTChainDependents.h"
#include "AOTObjectWords.h"
#include "AOTCompiledCallee.h"
#include "AOTCompiledFrame.h"
#include "AOTConvention.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTJSONHooks.h"
#include "AOTModuleTable.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramTables.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSlimClosure.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTStubDescriptor.h"
#include "AOTVector.h"
#include "BlockDirectory.h"
#include "FreeList.h"
#include "FreeListInlines.h"
#include "JSCInlines.h"
#include "JSCJSValueInlines.h"
#include "LocalAllocator.h"
#include "MarkedSpace.h"
#include "Subspace.h"
#include "VM.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/HexNumber.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringView.h>

#if OS(LINUX)
#include <fcntl.h>
#include <unistd.h>
#endif

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT {

template<typename Function>
static void forEachInstance(VM& vm, const Function& function)
{
    VMState& state = vm.aotState();
    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances)
        function(*instance);
    for (Instance* instance : state.instancesToDestroy)
        function(*instance);
}

void RuntimeCounters::add(const CounterWords& words)
{
    std::span source { words.values };
    for (size_t index = 0; index < values.size(); ++index)
        values[index] += source[index];
}

void count(VM& vm, RuntimeCounterID counter)
{
    if (RuntimeCounters* counters = vm.aotState().counters.get())
        ++counters->values[static_cast<size_t>(counter)];
}

void count(Instance& instance, RuntimeCounterID counter)
{
    ++std::span { instance.runtime.counters.values }[static_cast<size_t>(counter)];
}

void countReasonForSlowStringify(VM& vm, ASCIILiteral reason)
{
    if (!Options::aotCountsPath()) [[likely]]
        return;
    AtomString name(reason);
    bool isCounted = false;
    forEachInstance(vm, [&](Instance& instance) {
        if (std::exchange(isCounted, true) || !instance.state)
            return;
        instance.state->propertyCaches->countNamed("JSON.stringify left to the general Stringifier"_s, name.impl());
    });
}

void countLimitHit(VM& vm, LimitID limit)
{
    if (RuntimeCounters* counters = vm.aotState().counters.get())
        ++counters->limitHits[static_cast<size_t>(limit)];
}

void countStackListingAfterGivenUpCapture(VM& vm)
{
    if (RuntimeCounters* counters = vm.aotState().counters.get())
        ++counters->stackListingsAfterGivenUpCaptures;
}

void countRopeFlattenedForPrefixTest(VM& vm)
{
    if (RuntimeCounters* counters = vm.aotState().counters.get())
        ++counters->ropesFlattenedForPrefixTests;
}

void countNativeTextHashed(VM& vm)
{
    if (RuntimeCounters* counters = vm.aotState().counters.get())
        ++counters->nativeTextsHashed;
}

void countModulesSoughtForRequireCache(VM& vm, size_t modules)
{
    if (RuntimeCounters* counters = vm.aotState().counters.get())
        counters->modulesSoughtForRequireCache += modules;
}

void countModulesSoughtForWhatHasRun(VM& vm, size_t modules)
{
    if (RuntimeCounters* counters = vm.aotState().counters.get())
        counters->modulesSoughtForWhatHasRun += modules;
}

uint64_t valueOf(VM& vm, RuntimeCounterID counter)
{
    size_t index = static_cast<size_t>(counter);
    RuntimeCounters* counters = vm.aotState().counters.get();
    uint64_t value = counters ? counters->values[index] : 0;
    forEachInstance(vm, [&](const Instance& instance) {
        value += std::span { instance.runtime.counters.values }[index];
    });
    return value;
}

uint64_t limitHitsOf(VM& vm, LimitID limit)
{
    RuntimeCounters* counters = vm.aotState().counters.get();
    return counters ? counters->limitHits[static_cast<size_t>(limit)] : 0;
}

uint64_t runtimeLimitValue(VM& vm, LimitID limit)
{
    return runtimeLimitValue(vm.aotState().image, limit);
}

uint64_t runtimeLimitValue(const Image* image, LimitID limit)
{
    uint64_t usual = defaultLimitValue(limit, hostArchitecture());
    return image ? valueWhenLowered(limit, usual, image->header().options.loweredLimit, image->header().options.loweredLimitValue) : usual;
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(ArrivalCounts);

static constexpr size_t maxNamesShown = 12;

static constexpr std::pair<bool (JSValue::*)() const, ASCIILiteral> primitiveArrivals[] {
    { &JSValue::isUndefined, "undefined"_s }, { &JSValue::isNull, "null"_s }, { &JSValue::isBoolean, "boolean"_s }, { &JSValue::isNumber, "number"_s },
    { &JSValue::isString, "string"_s }, { &JSValue::isSymbol, "symbol"_s }, { &JSValue::isBigInt, "bigint"_s },
};

// A property gets the next free offset, so the offsets give the order in which the properties were added. Only a property that took the place of a deleted one is out of order.
static String describeNamesOf(Structure* structure)
{
    Vector<PropertyTableEntry> properties = structure->getPropertiesConcurrently();
    AOT::sort(properties, { }, [](const PropertyTableEntry& property) {
        return property.offset();
    });

    StringBuilder names;
    names.append("object{"_s);
    for (size_t index = 0; index < properties.size(); ++index) {
        if (index)
            names.append(',');
        if (index == maxNamesShown) {
            names.append(".."_s);
            break;
        }
        names.append(StringView(properties[index].key()));
    }
    names.append('}');
    // Whoever files an access to a cacheable dictionary that was never flat makes it flat.
    if (structure->isUncacheableDictionary() || (structure->isDictionary() && structure->hasBeenFlattenedBefore()))
        names.append("dict"_s);
    return names.toString();
}

static String describeArrival(JSValue value)
{
    for (auto& [isArrival, name] : primitiveArrivals) {
        if ((value.*isArrival)())
            return name;
    }
    if (Object* object = compiledObjectOrNull(value)) {
        bool hasLeftShape = object->hasFlagsInOrderWord() || object->compiledStructure().hasOtherPrototype();
        return makeString("layout:"_s, object->layout().index, hasLeftShape ? "+changed"_s : ""_s);
    }
    JSCell* cell = value.asCell();
    if (isFunctionType(cell->type())) {
        std::optional<FunctionID> function = functionOf(cell);
        return function ? makeString("function:"_s, function->index()) : "function:native"_str;
    }
    if (cell->type() == FinalObjectType)
        return describeNamesOf(cell->structure());
    return makeString("class:"_s, cell->classInfo()->className);
}

static ASCIILiteral describeStorageOfElements(JSValue receiver)
{
    if (!receiver.isObject())
        return "-"_s;
    switch (asObject(receiver)->indexingType() & IndexingShapeMask) {
    case NoIndexingShape:
        return "none"_s;
    case UndecidedShape:
        return "undecided"_s;
    case Int32Shape:
        return "int32"_s;
    case DoubleShape:
        return "double"_s;
    case ContiguousShape:
        return "contiguous"_s;
    case ArrayStorageShape:
        return "array storage"_s;
    case SlowPutArrayStorageShape:
        return "slow put array storage"_s;
    }
    return "?"_s;
}

static ASCIILiteral findElement(JSValue receiver, uint32_t index)
{
    if (receiver.isString())
        return index < asString(receiver)->length() ? "own"_s : "absent"_s;
    if (!receiver.isObject())
        return "?"_s;
    JSObject* object = asObject(receiver);
    if (object->canGetIndexQuickly(index))
        return "own"_s;
    if (isTypedArrayType(object->type()))
        return "absent"_s;
    if (auto* array = dynamicDowncast<JSArray>(object); array && index >= array->length())
        return "past the end"_s;
    return "?"_s;
}

using PropertyWalk::Presence;

static Presence hasOwnName(VM& vm, JSObject* object, UniquedStringImpl* uid)
{
    if (Presence presence = PropertyWalk::hasOwnProperty(object, uid); presence != Presence::Unknown)
        return presence;
    bool isArray = object->type() == ArrayType;
    if (object->structure()->typeInfo().overridesGetOwnPropertySlot() && !isArray)
        return Presence::Unknown;
    // Structure::get() may build the table of properties, which is a cell.
    if (isValidOffset(object->structure()->getConcurrently(uid)) || (isArray && uid == vm.propertyNames->length.impl()))
        return Presence::Present;
    return Presence::Absent;
}

static ASCIILiteral findName(JSGlobalObject* globalObject, JSValue receiver, UniquedStringImpl* uid)
{
    if (!receiver.isObject())
        return "?"_s;
    JSObject* object = asObject(receiver);
    switch (hasOwnName(globalObject->vm(), object, uid)) {
    case Presence::Present:
        return "own"_s;
    case Presence::Unknown:
        return "?"_s;
    case Presence::Absent:
        break;
    }
    // The walk begins above the receiver.
    switch (walkChain({ globalObject, object->structure(), object, ChainWalkMode::AllocatesNothing, ChainWalkUse::UsedAtOnce }, uid).result) {
    case ChainWalkResult::Unknown:
        return "?"_s;
    case ChainWalkResult::Absent:
        return "absent"_s;
    case ChainWalkResult::Found:
        return "inherited"_s;
    }
    return "?"_s;
}

static String describeKeyedArrival(JSGlobalObject* globalObject, JSValue receiver, JSValue key)
{
    ASCIILiteral kind = "other"_s;
    ASCIILiteral found = "?"_s;
    uint32_t mark = 0;
    auto isIndex = [&](uint32_t index, ASCIILiteral name) {
        kind = name;
        found = findElement(receiver, index);
        mark = index;
    };
    if (key.isInt32()) {
        if (key.asInt32() >= 0)
            isIndex(key.asInt32(), "index"_s);
        else {
            kind = "negative integer"_s;
            mark = key.asInt32();
        }
    } else if (key.isDouble()) {
        double number = key.asDouble();
        if (number >= 0 && number < MAX_ARRAY_INDEX && number == static_cast<uint32_t>(number))
            isIndex(static_cast<uint32_t>(number), "index as a double"_s);
        else
            kind = "number that is no index"_s;
    } else if (key.isSymbol()) {
        UniquedStringImpl* uid = &asSymbol(key)->uid();
        kind = "symbol"_s;
        found = findName(globalObject, receiver, uid);
        mark = static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid) >> 4);
    } else if (key.isString()) {
        const StringImpl* text = asString(key)->tryGetValueImpl();
        if (!text)
            kind = "rope"_s;
        else if (std::optional<uint32_t> index = parseIndex(*const_cast<StringImpl*>(text)))
            isIndex(*index, "text of an index"_s);
        else {
            mark = text->is8Bit() ? StringHasher::computeHashAndMaskTop8Bits(text->span8()) : StringHasher::computeHashAndMaskTop8Bits(text->span16());
            kind = text->isAtom() ? "name"_s : "text made anew"_s;
            RefPtr<AtomStringImpl> name = text->is8Bit() ? AtomStringImpl::lookUp(text->span8()) : AtomStringImpl::lookUp(text->span16());
            found = name ? findName(globalObject, receiver, name.get()) : receiver.isObject() ? "absent"_s : "?"_s;
        }
    }
    return makeString(describeArrival(receiver), "; "_s, describeStorageOfElements(receiver), "; key:"_s, kind, "; "_s, found, "; #"_s, hex(mark & 0xffff, 4, Lowercase));
}

static void dumpArrivalLines(PrintStream& out, ASCIILiteral prefix, const ArrivalCounts::Counts& counts)
{
    Vector<const ArrivalCounts::Counts::KeyValuePairType*> lines;
    for (auto& line : counts)
        lines.append(&line);
    AOT::sort(lines, [](auto* a, auto* b) {
        if (a->key.first != b->key.first)
            return a->key.first < b->key.first;
        return a->value != b->value ? a->value > b->value : codePointCompareLessThan(a->key.second, b->key.second);
    });
    for (auto* line : lines)
        out.println(line->value, "\t", prefix, line->key.first, "\t", line->key.second);
}

ArrivalCounts::ArrivalCounts() = default;
ArrivalCounts::~ArrivalCounts() = default;

void ArrivalCounts::record(JSValue receiver, uint32_t site)
{
    ASSERT(receiver);
    ++m_arrivals.add({ site, describeArrival(receiver) }, 0).iterator->value;
    m_lastSite = site;
    m_hasCalledNothingSinceArrival = true;
}

void ArrivalCounts::record(JSGlobalObject* globalObject, JSValue receiver, JSValue key, uint32_t site)
{
    ASSERT(receiver && key);
    ++m_arrivals.add({ site, describeKeyedArrival(globalObject, receiver, key) }, 0).iterator->value;
    m_lastSite = site;
    m_hasCalledNothingSinceArrival = true;
}

NEVER_INLINE void ArrivalCounts::didCallOperation(Entry entry)
{
    if (!m_hasCalledNothingSinceArrival || entry == Entry::operationAOTRecordArrival)
        return;
    m_hasCalledNothingSinceArrival = false;
    ++m_operationsBehindArrivals.add({ m_lastSite, nameOf(entry) }, 0).iterator->value;
}

void ArrivalCounts::dump(PrintStream& out) const
{
    dumpArrivalLines(out, "arrival:"_s, m_arrivals);
    dumpArrivalLines(out, "left:"_s, m_operationsBehindArrivals);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRecordArrival, void, (Instance* instance, EncodedJSValue receiver, EncodedJSValue keyOrEmpty, uint32_t site))
{
    State& state = *instance->state;
    if (!state.arrivalCounts) [[unlikely]]
        state.arrivalCounts = makeUnique<ArrivalCounts>();
    if (JSValue key = JSValue::decode(keyOrEmpty))
        state.arrivalCounts->record(instance->globalObject, JSValue::decode(receiver), key, site);
    else
        state.arrivalCounts->record(JSValue::decode(receiver), site);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

WTF_MAKE_TZONE_ALLOCATED_IMPL(AllocationCounts);

static_assert(!FunctionID(AllocationData::notConstructed));

static constexpr std::array<ASCIILiteral, 7> reasonsForSlowAllocation {
    "the_first_allocation_at_the_site"_s, "the_allocator_had_no_list"_s, "a_list_of_1_cell_is_used_up"_s, "a_list_of_2_to_4_cells_is_used_up"_s,
    "a_list_of_5_to_16_cells_is_used_up"_s, "a_list_of_17_to_64_cells_is_used_up"_s, "a_longer_list_is_used_up"_s,
};
static constexpr std::array<uint64_t, 5> longestListOfReason { 0, 1, 4, 16, 64 };

// Also right for the allocator without room of an empty allocation site record, which is a free list behind as many bytes.
static const FreeList& freeListOf(const void* allocator)
{
    return *reinterpret_cast<const FreeList*>(static_cast<const uint8_t*>(allocator) + LocalAllocator::offsetOfFreeList());
}

static CountedCells listOf(const void* allocator)
{
    uint64_t bytes = freeListOf(allocator).originalSize();
    return { bytes, bytes ? bytes / freeListOf(allocator).cellSize() : 0 };
}

static std::optional<CountedCells> usedUpListOf(const void* allocator)
{
    if (!allocator || !listOf(allocator).bytes || freeListOf(allocator).allocationWillSucceed())
        return std::nullopt;
    return listOf(allocator);
}

static void addLine(AllocationCounts::Lines& lines, const String& words, CountedCells counted)
{
    CountedCells& line = lines.add(words, CountedCells { }).iterator->value;
    line.bytes += counted.bytes;
    line.cells += counted.cells;
}

static void dumpLines(PrintStream& out, ASCIILiteral firstWord, const AllocationCounts::Lines& unsorted)
{
    Vector<const AllocationCounts::Lines::KeyValuePairType*> lines;
    for (auto& line : unsorted)
        lines.append(&line);
    AOT::sort(lines, [](auto* a, auto* b) {
        return a->value != b->value ? a->value > b->value : codePointCompareLessThan(a->key, b->key);
    });
    for (auto* line : lines)
        out.println(firstWord, " ", line->key, " ", line->value.cells, " ", line->value.bytes);
}

static String countedAsWord(StringView text)
{
    if (text.isEmpty())
        return "-"_str;
    StringBuilder builder;
    for (char16_t unit : text.codeUnits()) {
        if (unit == ' ')
            builder.append('_');
        else if (unit > ' ' && unit != 0x7f && unit != '\\')
            builder.append(unit);
        else
            builder.append("\\u"_s, hex(unit, 4, Lowercase));
    }
    return builder.toString();
}

// The constructor of a class allocates the object, in its own frame or in one that was inlined.
static std::optional<CompiledFrame> findAllocatingFrame(VM& vm, FunctionID constructor)
{
    std::optional<CompiledFrameIterator> frames = CompiledFrameIterator::tryCreateAtTop(vm);
    if (!frames && vm.topCallFrame && isCallerCompiled(vm, vm.topCallFrame, vm.topEntryFrame))
        frames = CompiledFrameIterator::tryCreateAtCallerOf(vm, vm.topCallFrame, vm.topEntryFrame);
    if (!frames || frames->isAtEnd())
        return std::nullopt;
    CompiledFrame innermost = frames->frame();
    if (!constructor || FunctionDescription(innermost.function).sourceFunction() != constructor)
        return innermost;
    frames->advance();
    while (!frames->isAtEnd() && isEntrypointOf(frames->frame().function, constructor))
        frames->advance();
    return frames->isAtEnd() ? innermost : frames->frame();
}

static const Image& imageOf(VM& vm, const std::optional<CompiledFrame>& frame)
{
    return frame && frame->instance ? *frame->instance->program : *vm.aotState().image;
}

static String describePlace(VM& vm, const std::optional<CompiledFrame>& frame)
{
    if (!frame)
        return "- -"_str;
    // The numbers of a frame are those of its image. Another one may run: native code that goes through the instances of a realm allocates under each of them.
    const Image& image = imageOf(vm, frame);
    RunningImageScope imageOfFrame(vm.aotState(), &image);
    FunctionDescription description(frame->function);
    String function = description.codeType() == ModuleCode ? "module"_str : countedAsWord(description.name(vm));
    if (image.isObfuscated()) {
        LineColumn position = frame->lineColumn();
        return makeString(function, ' ', position.line, ':', position.column);
    }
    std::optional<SourcePosition> position = image.positionOf(frame->code.index(), frame->callSite);
    if (!position)
        return makeString(function, " -"_s);
    String path = position->source ? ModuleTable(image.programTables()).pathTextForLineColumns(position->source) : emptyString();
    return makeString(function, ' ', countedAsWord(path), ':', position->lineColumn.line);
}

static String describeLayoutOfAllocation(const Instance& instance, uint32_t allocation)
{
    const Image& image = *instance.program;
    const ProgramTables& tables = instance.tables();
    uint32_t shape = tables.allocations()[allocation].shape;
    if (shape < tables.firstEnvironmentShape) {
        const LayoutData& layout = tables.layoutForShape(shape);
        return makeString('L', layout.index, ':', countedAsWord(nameOf(tables, layout)));
    }
    const EnvironmentData& environment = tables.environmentForShape(shape);
    if (environment.kind == LayoutKind::Closure)
        return "closure"_str;
    StringID name(environment.name);
    bool hasName = !image.isObfuscated() && name && image.text().hasText(name);
    return makeString('L', environment.index, ':', countedAsWord(hasName ? image.text().createString(name) : emptyString()));
}

template<typename Functor>
const String& AllocationCounts::wordsAtAllocatingFrame(VM& vm, FunctionID constructor, Counted kind, uint64_t detail, const Functor& describeCounted)
{
    ASSERT(!(detail >> 56));
    uint64_t counted = static_cast<uint64_t>(kind) << 56 | detail;
    const void* returnAddress = constructor ? nullptr : CompiledFrameIterator::findReturnAddressInFunctionAtTop(vm);
    if (returnAddress) {
        if (auto known = m_wordsOfReturnAddresses.find({ returnAddress, counted }); known != m_wordsOfReturnAddresses.end())
            return known->value;
    }
    std::optional<CompiledFrame> frame = findAllocatingFrame(vm, constructor);
    // A call site lies in one function, inlined or not.
    uint64_t site = frame ? (static_cast<uint64_t>(frame->code.index()) + 1) << 32 | frame->callSite.bits() : 0;
    const String& words = m_wordsOfLines.ensure({ site, counted, imageOf(vm, frame).position() }, [&] {
        return makeString(describePlace(vm, frame), ' ', describeCounted());
    }).iterator->value;
    if (returnAddress)
        m_wordsOfReturnAddresses.add({ returnAddress, counted }, words);
    return words;
}

AllocationCounts& AllocationCounts::ensure(Instance& instance)
{
    State& state = *instance.state;
    if (!state.allocationCounts) [[unlikely]]
        state.allocationCounts = makeUnique<AllocationCounts>();
    return *state.allocationCounts;
}

AllocationCounts* AllocationCounts::ensureForFirstInstance(VM& vm)
{
    VMState& vmState = vm.aotState();
    if (vmState.instances.isEmpty() || !vmState.instances.first()->state)
        return nullptr;
    return &ensure(*vmState.instances.first());
}

void AllocationCounts::willAllocate(Instance& instance, uint32_t allocation)
{
    const AllocationSiteRecord* record = instance.objects.allocationSite(allocation);
    if (auto list = usedUpListOf(record ? record->allocator : nullptr)) {
        FunctionID constructor { instance.tables().constructorOfAllocation(allocation) };
        if (constructor)
            constructor = FunctionDescription(constructor).sourceFunction();
        AllocationCounts& counts = ensure(instance);
        addLine(counts.m_allocations, counts.wordsAtAllocatingFrame(*instance.vm, constructor, Counted::Layout, allocation, [&] {
            return describeLayoutOfAllocation(instance, allocation);
        }), *list);
    }
    size_t reason = 0;
    if (record && record->header)
        reason = 1 + (std::ranges::lower_bound(longestListOfReason, listOf(record->allocator).cells) - longestListOfReason.begin());
    ++ensure(instance).m_slowAllocations[reason];
}

void AllocationCounts::willAllocateClosure(Instance& instance, uint64_t request)
{
    const void* allocator = nullptr;
    if (ClosureRequest::isSlim(request))
        allocator = instance.closures.slimClosureAllocators[MarkedSpace::sizeClassToIndex(SlimClosure::allocationSize(ClosureRequest::countOf(request))) - 1];
    else if (const AllocationSiteRecord* record = instance.objects.allocationSite(ClosureRequest::allocationOf(request)))
        allocator = record->allocator;
    if (auto list = usedUpListOf(allocator)) {
        AllocationCounts& counts = ensure(instance);
        addLine(counts.m_allocations, counts.wordsAtAllocatingFrame(*instance.vm, { }, Counted::Closure, ClosureRequest::closureNumberOf(request), [&] {
            return makeString("closure:"_s, countedAsWord(writtenFunctionName(closureFunctionWithNumber(ClosureRequest::closureNumberOf(request)))));
        }), *list);
    }
}

void AllocationCounts::willAllocateMap(Instance& instance)
{
    if (auto list = usedUpListOf(instance.allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Map)].allocator))
        addLine(ensure(instance).m_allocations, ensure(instance).wordsAtAllocatingFrame(*instance.vm, { }, Counted::Map, 0, [] {
            return "Map"_s;
        }), *list);
}

void AllocationCounts::willAllocateSet(Instance& instance)
{
    if (auto list = usedUpListOf(instance.allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Set)].allocator))
        addLine(ensure(instance).m_allocations, ensure(instance).wordsAtAllocatingFrame(*instance.vm, { }, Counted::Set, 0, [] {
            return "Set"_s;
        }), *list);
}

// Each VM's allocators that have requested a free list. An allocator is registered by willRefill() before it gets its first list.
using AllocatorsOfVMs = UncheckedKeyHashMap<const VMState*, UncheckedKeyHashSet<const LocalAllocator*>>;
static Lock allocatorsOfVMsLock;
static AllocatorsOfVMs& allocatorsOfVMs() WTF_REQUIRES_LOCK(allocatorsOfVMsLock)
{
    static NeverDestroyed<AllocatorsOfVMs> allocators;
    return allocators;
}

static String describeCellsOf(const LocalAllocator& allocator)
{
    return makeString(countedAsWord(String::fromLatin1(allocator.directory().subspace()->name().data())), ':', allocator.cellSize());
}

static void addListInUse(const LocalAllocator& allocator, AllocationCounts::Lines& refills)
{
    const FreeList& list = freeListOf(&allocator);
    uint64_t bytesLeft = 0;
    list.forEachInterval([&](char* start, char* end) {
        bytesLeft += end - start;
    });
    if (uint64_t bytesUsed = list.originalSize() - bytesLeft)
        addLine(refills, makeString("- list_in_use "_s, describeCellsOf(allocator)), { bytesUsed, bytesUsed / list.cellSize() });
}

// A free list is counted when it is used up. Adding the used part of the current lists makes the lines sum to the number of cells handed out.
static void addListsInUse(const VMState& state, AllocationCounts::Lines& refills)
{
    Locker locker { allocatorsOfVMsLock };
    auto allocators = allocatorsOfVMs().find(&state);
    if (allocators == allocatorsOfVMs().end())
        return;
    for (const LocalAllocator* allocator : allocators->value)
        addListInUse(*allocator, refills);
}

void AllocationCounts::willRefill(VM& vm, const LocalAllocator& allocator)
{
    AllocationCounts* counts = ensureForFirstInstance(vm);
    if (!counts || counts->m_listedAllocators.add(&allocator).isNewEntry) {
        Locker locker { allocatorsOfVMsLock };
        allocatorsOfVMs().add(&vm.aotState(), UncheckedKeyHashSet<const LocalAllocator*> { }).iterator->value.add(&allocator);
    }
    if (RuntimeTable* table = vm.aotState().runtimeTable.get())
        table->willRefillAllocator();
    CountedCells list = listOf(&allocator);
    if (counts && list.bytes)
        addLine(counts->m_refills, counts->wordsAtAllocatingFrame(vm, { }, Counted::CellsOfAllocator, std::bit_cast<uintptr_t>(&allocator), [&] {
            return describeCellsOf(allocator);
        }), list);
}

void AllocationCounts::willStopAllocating(VM& vm, const LocalAllocator& allocator)
{
    if (AllocationCounts* counts = ensureForFirstInstance(vm))
        addListInUse(allocator, counts->m_refills);
}

void AllocationCounts::willShutDownHeap(VMState& state)
{
    if (!state.instances.isEmpty() && state.instances.first()->state)
        addListsInUse(state, ensure(*state.instances.first()).m_refills);
    Locker locker { allocatorsOfVMsLock };
    allocatorsOfVMs().remove(&state);
}

void AllocationCounts::didMake(VM& vm, ASCIILiteral what, const char* detailOrNull)
{
    if (AllocationCounts* counts = ensureForFirstInstance(vm))
        addLine(counts->m_made, makeString(describePlace(vm, findAllocatingFrame(vm, { })), ' ', what, detailOrNull ? ":"_s : ""_s, detailOrNull ? countedAsWord(String::fromLatin1(detailOrNull)) : emptyString()), { 0, 1 });
}

size_t ExactAllocationCounts::numberOfKinds()
{
    return numberOfStubFamilies + 1;
}

void AllocationCounts::dump(const Instance& instance, PrintStream& out)
{
    const uint64_t* exactCounts = instance.runtime.counters.allocationCounts;
    if (exactCounts) {
        Lines lines;
        std::span allocations = instance.tables().allocations();
        const uint64_t* cellsOfAllocations = exactCounts + ExactAllocationCounts::offsetOfCellsOfAllocations() / sizeof(uint64_t);
        for (size_t allocation = 0; allocation < allocations.size(); ++allocation) {
            if (uint64_t cells = cellsOfAllocations[allocation])
                addLine(lines, makeString("- - "_s, describeLayoutOfAllocation(instance, static_cast<uint32_t>(allocation))), { cells * MarkedSpace::optimalSizeFor(allocations[allocation].size()), cells });
        }
        for (size_t kind = 0; kind < ExactAllocationCounts::numberOfKinds(); ++kind) {
            const uint64_t* counted = exactCounts + kind * ExactAllocationCounts::wordsOfKind;
            if (counted[0])
                addLine(lines, kind == ExactAllocationCounts::kindOfCompiledCode() ? "- - code"_str : makeString("- - stub:"_s, descriptorOf(static_cast<StubFamily>(kind)).name), { counted[1], counted[0] });
        }
        dumpLines(out, "allocation"_s, lines);
    }
    const AllocationCounts* counts = instance.state->allocationCounts.get();
    if (counts && !exactCounts)
        dumpLines(out, "allocation"_s, counts->m_allocations);
    Lines refills;
    if (counts)
        refills = counts->m_refills;
    // A VM's free lists are counted for its first instance.
    if (instance.vm->aotState().instances.first() == &instance)
        addListsInUse(instance.vm->aotState(), refills);
    dumpLines(out, "refill"_s, refills);
    if (!counts)
        return;
    dumpLines(out, "made"_s, counts->m_made);
    for (size_t reason = 0; reason < reasonsForSlowAllocation.size(); ++reason)
        out.println("slow-allocation "_s, reasonsForSlowAllocation[reason], " "_s, counts->m_slowAllocations[reason]);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

static void dumpOperationCalls(VM& vm, PrintStream& out)
{
    const RuntimeTable* table = vm.aotState().runtimeTable.get();
    if (!table)
        return;
    Vector<Entry> calledOperations;
    for (unsigned index = 0; index < numberOfEntries; ++index) {
        Entry entry = static_cast<Entry>(index);
        if (isOperation(entry) && table->callCount(entry))
            calledOperations.append(entry);
    }
    AOT::sort(calledOperations, [](Entry a, Entry b) {
        return codePointCompareLessThan(nameOf(a), nameOf(b));
    });
    for (Entry entry : calledOperations)
        out.println(table->callCount(entry), "\toperation:", nameOf(entry));
    for (Entry entry : calledOperations) {
        if (uint64_t calls = table->numberOfCallsThatRefilled(entry))
            out.println(calls, "\trefilled:", nameOf(entry));
    }
}


namespace {

// The pages of a range that are the process's own: present, not of a file, and mapped by nobody else. A page that was only ever read is
// the kernel's page of zeros: it makes a page fault and takes no memory.
class PagesHeld {
public:
#if OS(LINUX)
    PagesHeld()
        : m_pagemap(open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC))
    {
    }
    ~PagesHeld()
    {
        if (m_pagemap >= 0)
            close(m_pagemap);
    }

    size_t kilobytesIn(const void* start, size_t bytes) const
    {
        constexpr uint64_t isPresent = 1ull << 63;
        constexpr uint64_t isOfFileOrShared = 1ull << 61;
        constexpr uint64_t isMappedExclusively = 1ull << 56;
        if (m_pagemap < 0)
            return 0;
        size_t page = std::bit_cast<uintptr_t>(start) / pageSize();
        size_t end = (std::bit_cast<uintptr_t>(start) + bytes + pageSize() - 1) / pageSize();
        size_t held = 0;
        std::array<uint64_t, 512> entries;
        while (page < end) {
            size_t wanted = std::min(entries.size(), end - page);
            ssize_t got = pread(m_pagemap, entries.data(), wanted * sizeof(uint64_t), page * sizeof(uint64_t));
            if (got <= 0)
                break;
            size_t count = static_cast<size_t>(got) / sizeof(uint64_t);
            for (uint64_t entry : std::span(entries).first(count))
                held += (entry & (isPresent | isOfFileOrShared | isMappedExclusively)) == (isPresent | isMappedExclusively);
            page += count;
        }
        return held * pageSize() / KB;
    }

private:
    int m_pagemap;
#else
    size_t kilobytesIn(const void*, size_t) const { return 0; }
#endif
};

} // anonymous namespace

// In KB: what each part of the run time's own pages holds, and the room it has.
static void dumpPagesHeld(VM& vm, PrintStream& out)
{
    struct Part {
        ASCIILiteral name;
        size_t held { 0 };
        size_t room { 0 };
    };
    enum : size_t { Cards, Statics, PrefixAndPollPage, FixedTables, StringCells, SiteCaches, MethodTable, TableArena, numberOfParts };
    std::array<Part, numberOfParts> parts { { { "Cards"_s }, { "Statics"_s }, { "PrefixAndPollPage"_s }, { "FixedTables"_s }, { "StringCells"_s }, { "SiteCaches"_s }, { "MethodTable"_s }, { "TableArena"_s } } };
    PagesHeld pages;
    auto add = [&](size_t part, const void* start, size_t bytes) {
        parts[part].held += pages.kilobytesIn(start, bytes);
        parts[part].room += bytes / KB;
    };
    forEachInstance(vm, [&](const Instance& instance) {
        InstanceSizes sizes = instance.tables().instanceSizes;
        auto* base = reinterpret_cast<const char*>(&instance);
        auto between = [&](size_t part, ptrdiff_t from, ptrdiff_t to) { add(part, base + from, static_cast<size_t>(to - from)); };
        between(Cards, -static_cast<ptrdiff_t>(InstanceLayout::bytesReservedBelowInstance(sizes)), InstanceLayout::offsetOfStatics(sizes));
        between(Statics, InstanceLayout::offsetOfStatics(sizes), InstanceLayout::offsetOfLoopPollPage());
        between(PrefixAndPollPage, InstanceLayout::offsetOfLoopPollPage(), 0);
        between(FixedTables, 0, InstanceLayout::offsetOfStringCells);
        between(StringCells, InstanceLayout::offsetOfStringCells, InstanceLayout::offsetOfSiteCaches(sizes));
        between(SiteCaches, InstanceLayout::offsetOfSiteCaches(sizes), InstanceLayout::offsetOfMethodTable(sizes));
        between(MethodTable, InstanceLayout::offsetOfMethodTable(sizes), InstanceLayout::bytesReservedFromInstance(sizes));
        if (instance.state)
            add(TableArena, instance.state->tableArena.handedOut().data(), instance.state->tableArena.handedOut().size());
    });
    Part all { "AllPages"_s };
    if (vm.aotState().pages) {
        std::span<const uint8_t> handedOut = vm.aotState().pages->handedOut();
        all.held = pages.kilobytesIn(handedOut.data(), handedOut.size());
        all.room = handedOut.size() / KB;
    }
    out.println(all.held, "\theld:", all.name);
    for (const Part& part : parts)
        out.println(part.held, "\theld:", part.name);
    out.println(all.room, "\troom:", all.name);
    for (const Part& part : parts)
        out.println(part.room, "\troom:", part.name);
}

void dumpCounts(VM& vm, PrintStream& out)
{
    for (bool isStubExit : { false, true }) {
        for (RuntimeCounterID counter : allMembersOf<RuntimeCounterID>) {
            if (counter != RuntimeCounterID::None && countsStubExit(counter) == isStubExit)
                out.println(valueOf(vm, counter), "\t", isStubExit ? "exit:" : "counter:", Wire::nameOf(counter));
        }
    }
    // The run time also meets limits of the build, such as the number of key orders of a layout.
    for (LimitID limit : allMembersOf<LimitID>) {
        uint64_t hits = limitHitsOf(vm, limit);
        if (stageOf(limit) == Stage::RunTime || hits)
            out.println(hits, "\tlimit:", Wire::nameOf(limit));
    }
    dumpOperationCalls(vm, out);
    out.println(vm.hasBytecodeIntrinsicRegistry() ? 1 : 0, "\tmade:BytecodeIntrinsicRegistry");
    RuntimeCounters* counters = vm.aotState().counters.get();
    out.println(counters ? counters->stackListingsAfterGivenUpCaptures : 0, "\tmade:StackListingsAfterGivenUpCaptures");
    out.println(counters ? counters->ropesFlattenedForPrefixTests : 0, "\tflattened:RopesForPrefixTests");
    out.println(counters ? counters->nativeTextsHashed : 0, "\thashed:NativeTexts");
    uint64_t inlineFramesDecoded = 0;
    for (const VMState::PartsOfImage& parts : vm.aotState().parts) {
        if (parts.callSiteCache)
            inlineFramesDecoded += parts.callSiteCache->numberOfInlineFramesDecoded();
    }
    out.println(inlineFramesDecoded, "\tdecoded:InlineFrames");
    uint64_t builtinsSoughtNotMade = 0;
    forEachInstance(vm, [&](const Instance& instance) {
        if (instance.state)
            builtinsSoughtNotMade += instance.state->builtinCells->numberOfBuiltinsSoughtNotMade();
    });
    out.println(builtinsSoughtNotMade, "\tsought:BuiltinsNotMade");
    out.println(counters ? counters->modulesSoughtForRequireCache : 0, "\tsought:ModulesForRequireCache");
    out.println(counters ? counters->modulesSoughtForWhatHasRun : 0, "\tsought:ModulesForWhatHasRun");
    out.println(ModuleTable::modulesComparedWithPath(), "\tsought:ModulesByPath");
    dumpPagesHeld(vm, out);

    forEachInstance(vm, [&](const Instance& instance) {
        if (!instance.runtime.counters.branchCounts)
            return;
        auto branchCounts = unsafeMakeSpan(instance.runtime.counters.branchCounts, static_cast<size_t>(runtimeLimitValue(vm, LimitID::BranchCounters)));
        for (size_t index = 0; index < branchCounts.size(); ++index) {
            if (uint64_t branchCount = branchCounts[index])
                out.println(branchCount, "\tbranch:", index);
        }
    });
    auto forEachState = [&](const auto& dump) {
        forEachInstance(vm, [&](const Instance& instance) {
            if (instance.state)
                dump(instance, *instance.state);
        });
    };
    forEachState([&](const Instance&, const State& state) {
        if (!state.objects)
            return;
        state.objects->structures.dumpStructuresMade(out);
        if (state.objects->caches)
            state.objects->caches->assignmentPlans.dumpCopies(out);
    });
    forEachState([&](const Instance&, const State& state) {
        if (state.arrivalCounts)
            state.arrivalCounts->dump(out);
    });
    forEachState([&](const Instance& instance, const State&) {
        AllocationCounts::dump(instance, out);
    });
    forEachState([&](const Instance&, const State& state) {
        if (const PropertyCaches* caches = state.propertyCaches.getIfCreated())
            caches->dumpNamedCounts(out);
    });
}

} } // namespace JSC::AOT

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
