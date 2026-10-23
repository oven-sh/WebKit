/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowResults.h"
#include "AOTOperationData.h"
#include "AOTReuseEnvironments.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <utility>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class VariableGroup : uint8_t {
    UndefinedValue,
    UndefinedInteger,
    UndefinedDouble,
    AbsentInteger,
    AbsentDouble,
    UndefinedReference4,
    UndefinedInteger4,
    AbsentInteger4,
    UndefinedByte,
    ZeroValue,
    ZeroFunction,
    ZeroWord8,
    ZeroReference4,
    ZeroWord4,
    ZeroWord2,
    ZeroWord1,
};

struct VariableOrder {
    Vector<uint32_t> indices;
    uint32_t numberFilledAtProgramEntry { 0 };
};

class StaticsAllocator {
public:
    uint32_t alignTo(unsigned alignment)
    {
        m_end = WTF::roundUpToMultipleOf(alignment, m_end);
        return static_cast<uint32_t>(m_end);
    }

    uint32_t allocate(uint64_t size, unsigned alignment)
    {
        uint32_t offset = alignTo(alignment);
        m_end += size;
        return offset;
    }

    uint64_t end() const { return m_end; }

private:
    uint64_t m_end { 0 };
};

struct StaticsBuilder {
    const PlacementContext& context;
    const PlacedSites& sites;
    GuardBitTable& guardBits;
    StaticsAllocator allocator { };
    StaticsPlan plan { };
    LiteralSites literals { };
    uint32_t firstClearedOffset { 0 };
};

using RegionStep = void (*)(StaticsBuilder&);

} // anonymous namespace

static constexpr unsigned numberOfFilledGroups = 9;
static constexpr unsigned numberOfVariableGroups = 16;
static constexpr unsigned numberOfBuckets = numberOfFilledGroups + numberOfVariableGroups;
static constexpr unsigned staticsWordSize = sizeof(uint64_t);

static_assert(static_cast<unsigned>(VariableGroup::ZeroValue) == numberOfFilledGroups);
static_assert(static_cast<unsigned>(VariableGroup::ZeroWord1) + 1 == numberOfVariableGroups);
static_assert(!traitsOf(SlotRepresentation::JSValue).absentBits && !traitsOf(SlotRepresentation::LiteralString).absentBits);
static_assert(!traitsOf(SlotRepresentation::OptionalCell32).absentBits && !traitsOf(SlotRepresentation::LiteralString32).absentBits && !traitsOf(SlotRepresentation::OptionalBoolean).absentBits);
static_assert(widthOf(SlotRepresentation::JSValue) == staticsWordSize && widthOf(SlotRepresentation::LiteralString) == staticsWordSize && widthOf(SlotRepresentation::OptionalInt64) == staticsWordSize && widthOf(SlotRepresentation::OptionalDouble) == staticsWordSize);
static_assert(defaultLimitValue(LimitID::MaxStaticsSize, Architecture::ARM64) <= UINT32_MAX && defaultLimitValue(LimitID::MaxStaticsSize, Architecture::X86_64) <= UINT32_MAX);
static_assert(sizeof(EncodedJSValue) == staticsWordSize && sizeof(JSCell*) == staticsWordSize);
static_assert(MemberGuardData::bitsPerWord == 8 * staticsWordSize);
static_assert(sizeof(ArrayAllocationProfile) == staticsWordSize && ArrayAllocationProfile::sizeOfLastArrayWord == staticsWordSize);

uint64_t initialWordOf(const Program& program, VariableID id, const PlannedVariable& planned)
{
    const Variable& variable = program.at(id);
    if (planned.storage != VariableStorage::Statics || planned.encoding != VariableEncoding::Plain)
        return 0;
    if (variable.flow.initialization != InitializationState::Unknown) {
        // The hooks of a class object ask the variable whether the static field is defined, at times no proof about compiled reads covers.
        bool isStaticField = !!variable.owner;
        return isStaticField ? traitsOf(planned.representation).absentBits : 0;
    }
    // A variable that is born as a definition says so with zero, which is no plain encoding.
    if (variable.initialization == VariableInitialization::Undefined)
        return traitsOf(planned.representation).undefinedBits;
    RELEASE_ASSERT(variable.initialization == VariableInitialization::Uninitialized);
    return traitsOf(planned.representation).absentBits;
}

static VariableGroup zeroGroupOf(const PlacedVariable& placed)
{
    switch (widthOf(placed.representation)) {
    case 8:
        if (!traitsOf(placed.representation).holdsCell)
            return VariableGroup::ZeroWord8;
        // The words that the collector visits lie together, so the image lists them as a few ranges.
        return placed.encoding == VariableEncoding::FunctionAsZero ? VariableGroup::ZeroFunction : VariableGroup::ZeroValue;
    case 4:
        return traitsOf(placed.representation).isNarrowReference ? VariableGroup::ZeroReference4 : VariableGroup::ZeroWord4;
    case 2:
        return VariableGroup::ZeroWord2;
    case 1:
        return VariableGroup::ZeroWord1;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static VariableGroup groupOf(VariableInitialization initialization, const PlacedVariable& placed)
{
    if (!placed.initialWord)
        return zeroGroupOf(placed);
    bool startsUndefined = initialization == VariableInitialization::Undefined;
    switch (placed.representation) {
    case SlotRepresentation::JSValue:
    case SlotRepresentation::LiteralString:
        return VariableGroup::UndefinedValue;
    case SlotRepresentation::OptionalInt64:
        return startsUndefined ? VariableGroup::UndefinedInteger : VariableGroup::AbsentInteger;
    case SlotRepresentation::OptionalDouble:
        return startsUndefined ? VariableGroup::UndefinedDouble : VariableGroup::AbsentDouble;
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::LiteralString32:
        return VariableGroup::UndefinedReference4;
    case SlotRepresentation::OptionalInt32:
        return startsUndefined ? VariableGroup::UndefinedInteger4 : VariableGroup::AbsentInteger4;
    case SlotRepresentation::OptionalBoolean:
        return VariableGroup::UndefinedByte;
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::Int32:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::Float:
    case SlotRepresentation::Double:
    case SlotRepresentation::Cell:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::DeferredClosure:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StaticsRegion regionOf(const PlannedVariable& planned, uint64_t initialWord)
{
    if (!initialWord || planned.isUsedBeforeProgramEntry)
        return StaticsRegion::FilledAtInstanceCreation;
    return StaticsRegion::FilledAtProgramEntry;
}

static Vector<PlacedVariable> createPlacedVariables(const PlacementContext& context, const PlacedSites& sites)
{
    const Program& program = context.program;
    BitVector described = findVariablesDescribedToRunTime(program, context.inputs.flow);
    return Vector<PlacedVariable>(program.variables.size(), [&](size_t index) {
        VariableID id(index);
        PlannedVariable planned = context.slots().variable(id);
        PlacedVariable placed;
        placed.storage = planned.storage;
        placed.encoding = planned.encoding;
        placed.representation = planned.representation;
        if (planned.encoding == VariableEncoding::FunctionAsZero)
            placed.function = program.at(id).function;
        if (planned.storage == VariableStorage::Statics && !sites.variablesNamedByCode.get(index) && !described.quickGet(index))
            placed.storage = VariableStorage::None;
        if (!placed.hasWordInStatics())
            return placed;
        placed.initialWord = initialWordOf(program, id, planned);
        placed.region = regionOf(planned, placed.initialWord);
        return placed;
    });
}

static unsigned bucketOf(const Program& program, VariableID id, const PlacedVariable& placed)
{
    unsigned group = static_cast<unsigned>(groupOf(program.at(id).initialization, placed));
    return placed.region == StaticsRegion::FilledAtProgramEntry ? group : numberOfFilledGroups + group;
}

static VariableOrder sortVariables(const Program& program, const Vector<PlacedVariable>& variables)
{
    std::array<uint32_t, numberOfBuckets> nextInBucket { };
    Vector<uint32_t> variablesWithWord;
    Vector<uint8_t> buckets;
    for (uint32_t index = 0; index < variables.size(); ++index) {
        if (!variables[index].hasWordInStatics())
            continue;
        unsigned bucket = bucketOf(program, VariableID(index), variables[index]);
        ++nextInBucket[bucket];
        variablesWithWord.append(index);
        buckets.append(static_cast<uint8_t>(bucket));
    }
    uint32_t precedingCount = 0;
    for (uint32_t& next : nextInBucket)
        precedingCount += std::exchange(next, precedingCount);
    VariableOrder order;
    order.numberFilledAtProgramEntry = nextInBucket[numberOfFilledGroups];
    order.indices.grow(variablesWithWord.size());
    for (size_t position = 0; position < variablesWithWord.size(); ++position)
        order.indices[nextInBucket[buckets[position]]++] = variablesWithWord[position];
    return order;
}

static void placeVariables(StaticsBuilder& builder)
{
    StaticsPlan& plan = builder.plan;
    plan.variables = createPlacedVariables(builder.context, builder.sites);
    VariableOrder order = sortVariables(builder.context.program, plan.variables);
    auto place = [&](std::span<const uint32_t> indices) {
        for (uint32_t index : indices) {
            auto& variable = plan.variables[index];
            unsigned width = widthOf(variable.representation);
            variable.offset = builder.allocator.allocate(width, width);
            // A root is visited as a word of eight bytes. The image lists the narrow references in a table of their own.
            SlotTraits traits = traitsOf(variable.representation);
            if (!traits.holdsCell || traits.isNarrowReference)
                continue;
            RELEASE_ASSERT(width == staticsWordSize && !(variable.offset % staticsWordSize));
            plan.roots.append(variable.offset);
        }
    };
    auto indices = order.indices.span();
    place(indices.first(order.numberFilledAtProgramEntry));
    plan.sizeFilledAtProgramEntry = builder.allocator.alignTo(staticsWordSize);
    place(indices.subspan(order.numberFilledAtProgramEntry));
}

static Vector<uint32_t> allocateWords(StaticsBuilder& builder, size_t count)
{
    return Vector<uint32_t>(count, [&](size_t) {
        return builder.allocator.allocate(staticsWordSize, staticsWordSize);
    });
}

static Vector<uint32_t> allocateRootWords(StaticsBuilder& builder, size_t count)
{
    Vector<uint32_t> offsets = allocateWords(builder, count);
    builder.plan.roots.appendVector(offsets);
    return offsets;
}

// What `module.exports` holds is a variable of the program. The namespace object of a module with an export table is made for an operation that wants an object.
static bool hasValueWord(const PlacementContext& context, ModuleID id)
{
    switch (context.program.at(id).kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
        return context.inputs.flow.module(id).namespaceUse != NamespaceUse::NeverCreated;
    case ModuleKind::CommonJs:
        return false;
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void placeModuleValues(StaticsBuilder& builder)
{
    builder.plan.moduleValueOffsets = Vector<std::optional<uint32_t>>(builder.context.program.modules.size(), [&](size_t index) -> std::optional<uint32_t> {
        if (!hasValueWord(builder.context, ModuleID(index)))
            return std::nullopt;
        uint32_t offset = builder.allocator.allocate(staticsWordSize, staticsWordSize);
        builder.plan.roots.append(offset);
        return offset;
    });
}

static void placeGuardWords(StaticsBuilder& builder)
{
    Vector<PlacedGuardBit>& bits = builder.guardBits.bits;
    if (bits.isEmpty())
        return;
    // ClassData::guardWordsOffset says "no guard words" with 0.
    if (!builder.allocator.end())
        builder.allocator.allocate(staticsWordSize, staticsWordSize);
    for (size_t first = 0; first < bits.size();) {
        uint32_t wordOffset = builder.allocator.allocate(staticsWordSize, staticsWordSize);
        size_t end = first;
        // The bits of one side of a class fill words that follow each other, and so do the bits of sets of conditions, which belong to no class: the first bit of a word starts the next one.
        while (end < bits.size() && bits[end].classLayout == bits[first].classLayout && bits[end].side == bits[first].side && (end == first || bits[end].bit % MemberGuardData::bitsPerWord))
            bits[end++].wordOffset = wordOffset;
        RELEASE_ASSERT(!bits[first].classLayout || !bits[first].bit || wordOffset == bits[first - 1].wordOffset + staticsWordSize);
        first = end;
    }
}

// A module runs at most once, so it has one module object, one require function and one `import.meta`.
static void placeModuleBuiltinCells(StaticsBuilder& builder)
{
    for (const ModuleBuiltinUse& use : builder.sites.moduleBuiltins) {
        uint32_t offset = builder.allocator.allocate(staticsWordSize, staticsWordSize);
        builder.plan.moduleBuiltinCells.append({ use.builtin, use.module, offset });
        builder.plan.roots.append(offset);
    }
}

static void placeSharedStorage(StaticsBuilder& builder)
{
    builder.literals.sharedStorageOffsets = allocateRootWords(builder, builder.sites.copyOnWriteArrays.size());
}

// The table of constant data begins with these nodes, in this order.
static void placeStorageInImage(StaticsBuilder& builder)
{
    uint64_t next = DataNodeHeader::firstNode;
    builder.literals.nodesOfStorageInImage = builder.sites.arraysWithStorageInImage.map([&](DataID data) {
        uint32_t node = safeCast<uint32_t>(next);
        next += DataNodeHeader::numberOfWords(DataNodeHeader::Kind::Array, builder.context.program.at(data).root.as<DataNode::Array>().elements.size());
        return node;
    });
}

static void placeRegExpCells(StaticsBuilder& builder)
{
    builder.literals.regExpCellOffsets = allocateRootWords(builder, builder.sites.regExpLiterals.size());
}

static void placeTemplateObjects(StaticsBuilder& builder)
{
    builder.literals.templateObjectOffsets = allocateRootWords(builder, builder.sites.templateObjects.size());
}

static void placeArrayProfiles(StaticsBuilder& builder)
{
    builder.literals.profiledLiteralOffsets = allocateWords(builder, builder.sites.profiledArrayLiterals.size());
}

static void placeLastArrays(StaticsBuilder& builder)
{
    uint64_t size = static_cast<uint64_t>(builder.sites.profiledArrayLiterals.size()) * staticsWordSize;
    builder.plan.distanceFromArrayProfileToLastArray = static_cast<uint32_t>(size);
    builder.firstClearedOffset = builder.allocator.allocate(size, staticsWordSize);
}

static void placeFreeEnvironmentCells(StaticsBuilder& builder)
{
    unsigned numberOfLayouts = builder.context.program.layouts.size();
    builder.plan.freeCellOffsets.fill(std::nullopt, numberOfLayouts);
    for (unsigned i = 0; i < numberOfLayouts; ++i) {
        if (!builder.context.inputs.environmentReuse.isReused(LayoutID(i)))
            continue;
        builder.plan.freeCellOffsets[i] = builder.allocator.allocate(staticsWordSize, staticsWordSize);
    }
    uint64_t clearedSize = builder.allocator.end() - builder.firstClearedOffset;
    if (clearedSize)
        builder.plan.clearedEachCollection.append({ builder.firstClearedOffset, static_cast<uint32_t>(clearedSize) });
}

static constexpr RegionStep regionSteps[] = {
    placeVariables,
    placeModuleValues,
    placeGuardWords,
    placeModuleBuiltinCells,
    placeSharedStorage,
    placeStorageInImage,
    placeRegExpCells,
    placeTemplateObjects,
    placeArrayProfiles,
    placeLastArrays,
    placeFreeEnvironmentCells,
};

std::pair<StaticsPlan, LiteralSites> placeStatics(const PlacementContext& context, const PlacedSites& sites, GuardBitTable& guardBits)
{
    StaticsBuilder builder { context, sites, guardBits };
    for (RegionStep placeRegion : regionSteps) {
        placeRegion(builder);
        uint64_t end = builder.allocator.end();
        if (end > context.build.limit(LimitID::MaxStaticsSize)) {
            context.build.reportLimitExceeded(LimitID::MaxStaticsSize, end);
            return { WTF::move(builder.plan), WTF::move(builder.literals) };
        }
    }
    AOT::sort(builder.plan.roots);
    builder.plan.size = static_cast<uint32_t>(WTF::roundUpToMultipleOf<staticsWordSize>(builder.allocator.end()));
    return { WTF::move(builder.plan), WTF::move(builder.literals) };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
