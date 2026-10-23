/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAllocationCounts.h"

#if ENABLE(AOT)

#include "AOTAllocationSiteRecord.h"
#include "AOTCompiledFrame.h"
#include "AOTConvention.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleTable.h"
#include "AOTProgramTables.h"
#include "AOTSlimClosure.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTStubDescriptor.h"
#include "AOTVector.h"
#include "BlockDirectory.h"
#include "FreeList.h"
#include "JSCInlines.h"
#include "LocalAllocator.h"
#include "MarkedSpace.h"
#include "Subspace.h"
#include <algorithm>
#include <optional>
#include <wtf/HexNumber.h>
#include <wtf/PrintStream.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(AllocationCounts);

// No function of an image has the largest number, so no call site has this key, and the one above it stands for a deleted place in the table.
static constexpr uint64_t noCallSite = UINT64_MAX - 1;

static_assert(!FunctionID(AllocationData::notConstructed));

// Also right for the allocator without room of an empty allocation site record, which is a free list behind as many bytes.
static const FreeList& freeListOf(const void* allocator)
{
    return *reinterpret_cast<const FreeList*>(static_cast<const uint8_t*>(allocator) + LocalAllocator::offsetOfFreeList());
}

static String asWord(StringView text)
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

static String textOrEmpty(const Image& image, StringID string)
{
    if (image.isObfuscated() || !string || !image.text().hasText(string))
        return emptyString();
    return image.text().createString(string);
}

static uint64_t callSiteKeyOf(const CompiledFrame& frame)
{
    return static_cast<uint64_t>(frame.code.index()) << 32 | frame.callSite.bits();
}

static String describeFunction(VM& vm, FunctionID function)
{
    FunctionDescription description(function);
    return description.codeType() == ModuleCode ? "module"_str : asWord(description.name(vm));
}

static String describePosition(const Image& image, const CompiledFrame& frame)
{
    if (image.isObfuscated()) {
        LineColumn position = frame.lineColumn();
        return makeString(position.line, ':', position.column);
    }
    std::optional<SourcePosition> position = image.positionOf(frame.code.index(), frame.callSite);
    if (!position)
        return "-"_str;
    String path = position->source ? ModuleTable(image.programTables()).pathTextForLineColumns(position->source) : emptyString();
    return makeString(asWord(path), ':', position->lineColumn.line);
}

static String describePlace(VM& vm, const std::optional<CompiledFrame>& frame)
{
    if (!frame)
        return "- -"_str;
    return makeString(describeFunction(vm, frame->function), ' ', describePosition(*vm.aotState().image, *frame));
}

// The constructor of a class allocates the object, in its own frame or in one that was inlined.
static std::optional<CompiledFrame> findAllocatingFrame(VM& vm, FunctionID constructor)
{
    std::optional<CompiledFrameIterator> frames = CompiledFrameIterator::tryCreateAtTop(vm);
    // A function of the engine that compiled code has called: whoever reads the counts wants to know who called it.
    if (!frames && vm.topCallFrame && isCallerCompiled(vm, vm.topCallFrame))
        frames = CompiledFrameIterator::tryCreateAtCallerOf(vm, vm.topCallFrame, vm.topEntryFrame);
    if (!frames || frames->isAtEnd())
        return std::nullopt;
    CompiledFrame innermost = frames->frame();
    if (!constructor || FunctionDescription(innermost.function).sourceFunction() != constructor)
        return innermost;
    frames->advance();
    while (!frames->isAtEnd() && isEntrypointOf(frames->frame().function, constructor))
        frames->advance();
    // Code that is not compiled called the constructor.
    if (frames->isAtEnd())
        return innermost;
    return frames->frame();
}

AllocationCounts::AllocationCounts(Instance& instance)
    : m_instance(instance)
{
}

AllocationCounts::~AllocationCounts() = default;

AllocationCounts& AllocationCounts::ensure(Instance& instance)
{
    State& state = *instance.state;
    if (!state.allocationCounts) [[unlikely]]
        state.allocationCounts = makeUnique<AllocationCounts>(instance);
    return *state.allocationCounts;
}

void AllocationCounts::countFreeList(Instance& instance, const void* allocator, Allocated allocated)
{
    if (!allocator)
        return;
    const FreeList& freeList = freeListOf(allocator);
    uint64_t bytes = freeList.originalSize();
    if (!bytes || freeList.allocationWillSucceed())
        return;
    ensure(instance).add(allocated, bytes / freeList.cellSize(), bytes);
}

void AllocationCounts::willAllocate(Instance& instance, uint32_t allocation)
{
    const AllocationSiteRecord* record = instance.objects.allocationSite(allocation);
    countFreeList(instance, record ? record->allocator : nullptr, { Kind::Allocation, allocation });

    auto reason = [&] {
        if (!record || !record->header)
            return ReasonForNoRoom::FirstAllocationAtSite;
        const FreeList& freeList = freeListOf(record->allocator);
        uint64_t cells = freeList.originalSize() ? freeList.originalSize() / freeList.cellSize() : 0;
        if (!cells)
            return ReasonForNoRoom::NoList;
        if (cells == 1)
            return ReasonForNoRoom::ListOfOneCell;
        if (cells <= 4)
            return ReasonForNoRoom::ListOfAtMost4Cells;
        if (cells <= 16)
            return ReasonForNoRoom::ListOfAtMost16Cells;
        return cells <= 64 ? ReasonForNoRoom::ListOfAtMost64Cells : ReasonForNoRoom::LongerList;
    };
    ++ensure(instance).m_slowAllocations[static_cast<unsigned>(reason())];
}

void AllocationCounts::willAllocateClosure(Instance& instance, uint64_t request)
{
    const void* allocator = nullptr;
    if (ClosureRequest::isSlim(request))
        allocator = instance.closures.slimClosureAllocators[MarkedSpace::sizeClassToIndex(SlimClosure::allocationSize(ClosureRequest::countOf(request))) - 1];
    else if (const AllocationSiteRecord* record = instance.objects.allocationSite(ClosureRequest::allocationOf(request)))
        allocator = record->allocator;
    countFreeList(instance, allocator, { Kind::Closure, ClosureRequest::functionOf(request) });
}

void AllocationCounts::willAllocateMap(Instance& instance)
{
    countFreeList(instance, instance.allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Map)].allocator, { Kind::Map });
}

void AllocationCounts::willAllocateSet(Instance& instance)
{
    countFreeList(instance, instance.allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Set)].allocator, { Kind::Set });
}

void AllocationCounts::willRefill(VM& vm, const LocalAllocator& allocator)
{
    const FreeList& freeList = freeListOf(&allocator);
    uint64_t bytes = freeList.originalSize();
    VMState& vmState = vm.aotState();
    if (!bytes || vmState.instances.isEmpty())
        return;
    Instance& instance = *vmState.instances.first();
    if (!instance.state)
        return;
    State& state = *instance.state;
    if (!state.allocationCounts) [[unlikely]]
        state.allocationCounts = makeUnique<AllocationCounts>(instance);
    state.allocationCounts->addRefill(allocator, bytes / freeList.cellSize(), bytes);
}

void AllocationCounts::didMake(VM& vm, ASCIILiteral what, const char* detailOrNull)
{
    VMState& vmState = vm.aotState();
    if (vmState.instances.isEmpty() || !vmState.instances.first()->state)
        return;
    AllocationCounts& counts = ensure(*vmState.instances.first());
    String words = makeString(describePlace(vm, findAllocatingFrame(vm, FunctionID())), ' ', what, detailOrNull ? ":"_s : ""_s, detailOrNull ? asWord(String::fromLatin1(detailOrNull)) : emptyString());
    uint32_t number = counts.m_madeByWords.ensure(words, [&] {
        counts.m_made.append(Line { words });
        return static_cast<uint32_t>(counts.m_made.size() - 1);
    }).iterator->value;
    ++counts.m_made[number].cells;
}

void AllocationCounts::addRefill(const LocalAllocator& allocator, uint64_t cells, uint64_t bytes)
{
    VM& vm = *m_instance.vm;
    const ASCIICString& subspace = allocator.directory().subspace()->name();
    String words = makeString(describePlace(vm, findAllocatingFrame(vm, FunctionID())), ' ', asWord(String::fromLatin1(subspace.data())), ':', allocator.cellSize());
    uint32_t number = m_refillsByWords.ensure(words, [&] {
        m_refills.append(Line { words });
        return static_cast<uint32_t>(m_refills.size() - 1);
    }).iterator->value;
    Line& line = m_refills[number];
    line.cells += cells;
    line.bytes += bytes;
}

static String describeLayoutOfAllocation(const Instance& instance, uint32_t allocation)
{
    const Image& image = *instance.vm->aotState().image;
    const ProgramTables& tables = instance.tables();
    uint32_t shape = tables.allocations()[allocation].shape;
    if (shape < tables.firstEnvironmentShape) {
        const LayoutData& layout = tables.layoutForShape(shape);
        return makeString('L', layout.index, ':', asWord(nameOf(tables, layout)));
    }
    const EnvironmentData& environment = tables.environmentForShape(shape);
    if (environment.kind == LayoutKind::Closure)
        return "closure"_str;
    return makeString('L', environment.index, ':', asWord(textOrEmpty(image, StringID(environment.name))));
}

String AllocationCounts::describeLayout(Allocated allocated) const
{
    switch (allocated.kind) {
    case Kind::Allocation:
        return describeLayoutOfAllocation(m_instance, allocated.id);
    case Kind::Closure:
        return makeString("closure:"_s, asWord(writtenFunctionName(FunctionID(allocated.id))));
    case Kind::Map:
        return "Map"_str;
    case Kind::Set:
        return "Set"_str;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t AllocationCounts::numberOfLine(const String& words)
{
    return m_linesByWords.ensure(words, [&] {
        m_lines.append(Line { words });
        return static_cast<uint32_t>(m_lines.size() - 1);
    }).iterator->value;
}

void AllocationCounts::add(Allocated allocated, uint64_t cells, uint64_t bytes)
{
    VM& vm = *m_instance.vm;
    FunctionID constructor;
    if (allocated.kind == Kind::Allocation) {
        if (FunctionID constructedBy { m_instance.tables().constructorOfAllocation(allocated.id) })
            constructor = FunctionDescription(constructedBy).sourceFunction();
    }
    std::optional<CompiledFrame> frame = findAllocatingFrame(vm, constructor);
    Key key { frame ? callSiteKeyOf(*frame) : noCallSite, static_cast<uint32_t>(allocated.kind), allocated.id };
    uint32_t number = m_linesByCallSite.ensure(key, [&] {
        return numberOfLine(makeString(describePlace(vm, frame), ' ', describeLayout(allocated)));
    }).iterator->value;
    Line& line = m_lines[number];
    line.cells += cells;
    line.bytes += bytes;
}

template<typename Line>
static void dumpLines(PrintStream& out, ASCIILiteral firstWord, const Vector<Line>& unsorted)
{
    Vector<const Line*> lines;
    lines.reserveInitialCapacity(unsorted.size());
    for (const Line& line : unsorted)
        lines.append(&line);
    AOT::sort(lines, [](const Line* a, const Line* b) {
        if (a->bytes != b->bytes)
            return a->bytes > b->bytes;
        if (a->cells != b->cells)
            return a->cells > b->cells;
        return codePointCompareLessThan(a->words, b->words);
    });
    for (const Line* line : lines)
        out.println(firstWord, " ", line->words, " ", line->cells, " ", line->bytes);
}

size_t ExactAllocationCounts::numberOfKinds()
{
    return numberOfStubFamilies + 1;
}

void ExactAllocationCounts::dump(const Instance& instance, PrintStream& out)
{
    const uint64_t* words = instance.runtime.counters.allocationCounts;
    if (!words)
        return;
    struct Counted {
        String what;
        uint64_t cells;
        uint64_t bytes;
    };
    Vector<Counted> lines;
    UncheckedKeyHashMap<String, size_t> lineOfLayout;
    std::span allocations = instance.tables().allocations();
    const uint64_t* cellsOfAllocations = words + offsetOfCellsOfAllocations() / sizeof(uint64_t);
    for (size_t allocation = 0; allocation < allocations.size(); ++allocation) {
        uint64_t cells = cellsOfAllocations[allocation];
        if (!cells)
            continue;
        uint64_t bytes = cells * MarkedSpace::optimalSizeFor(allocations[allocation].size());
        size_t line = lineOfLayout.ensure(describeLayoutOfAllocation(instance, static_cast<uint32_t>(allocation)), [&] {
            return lines.size();
        }).iterator->value;
        if (line == lines.size())
            lines.append({ describeLayoutOfAllocation(instance, static_cast<uint32_t>(allocation)), 0, 0 });
        lines[line].cells += cells;
        lines[line].bytes += bytes;
    }
    for (size_t kind = 0; kind < numberOfKinds(); ++kind) {
        uint64_t cells = words[kind * wordsOfKind];
        uint64_t bytes = words[kind * wordsOfKind + 1];
        if (!cells)
            continue;
        lines.append({ kind == kindOfCompiledCode() ? "code"_str : makeString("stub:"_s, descriptorOf(static_cast<StubFamily>(kind)).name), cells, bytes });
    }
    AOT::sort(lines, [](const Counted& a, const Counted& b) {
        if (a.bytes != b.bytes)
            return a.bytes > b.bytes;
        if (a.cells != b.cells)
            return a.cells > b.cells;
        return codePointCompareLessThan(a.what, b.what);
    });
    for (const Counted& line : lines)
        out.println("allocation - - "_s, line.what, " "_s, line.cells, " "_s, line.bytes);
}

void AllocationCounts::dump(PrintStream& out) const
{
    if (!m_instance.runtime.counters.allocationCounts)
        dumpLines(out, "allocation"_s, m_lines);
    dumpLines(out, "refill"_s, m_refills);
    dumpLines(out, "made"_s, m_made);
    constexpr std::array<ASCIILiteral, numberOfReasonsForNoRoom> reasons {
        "the_first_allocation_at_the_site"_s, "the_allocator_had_no_list"_s, "a_list_of_1_cell_is_used_up"_s, "a_list_of_2_to_4_cells_is_used_up"_s,
        "a_list_of_5_to_16_cells_is_used_up"_s, "a_list_of_17_to_64_cells_is_used_up"_s, "a_longer_list_is_used_up"_s,
    };
    for (unsigned reason = 0; reason < numberOfReasonsForNoRoom; ++reason)
        out.println("slow-allocation "_s, reasons[reason], " "_s, m_slowAllocations[reason]);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
