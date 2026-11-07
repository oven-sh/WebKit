/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSectionLinker.h"

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTUnitPlacement.h"
#include <algorithm>
#include <iterator>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static constexpr size_t sectionVeneerAlignment = 4;
static constexpr uint64_t maxLinkedVectorSize = defaultLimitValue(LimitID::VectorBytes, hostArchitecture());

namespace {

struct SectionUnitGraph {
    std::span<const LinkUnit> units;
    Vector<Vector<UnitTarget, 0>> targets;
    Vector<uint32_t> unitsByPlace;
    Vector<uint32_t> placeOfUnit;
    size_t firstPlaceOfData { 0 };
    size_t dataAlignment { 1 };
};

struct SectionVeneer {
    UnitTarget target;
    uint32_t firstRelocation;
};

struct SectionLayout {
    UnitPlacement placement;
    Vector<Vector<SectionVeneer, 0>> veneersByPlace;
    size_t dataOffset { 0 };
    size_t end { 0 };
};

} // anonymous namespace

static constexpr bool namesPlaceInSection(RelocationKind kind)
{
    switch (kind) {
    case RelocationKind::Call:
    case RelocationKind::Jump:
    case RelocationKind::ShortJump:
    case RelocationKind::ConditionalJump:
    case RelocationKind::ShortConditionalJump:
    case RelocationKind::ConditionalJumpThenNop:
    case RelocationKind::TestBitJump:
    case RelocationKind::TestBitJumpThenNop:
    case RelocationKind::InvertedJumpThenJump:
    case RelocationKind::AddressNear:
    case RelocationKind::AddressFar:
    case RelocationKind::Load64:
    case RelocationKind::Distance32:
        return true;
    case RelocationKind::InstanceOffset:
    case RelocationKind::Number32:
    case RelocationKind::InstanceAccess:
    case RelocationKind::NumberWord:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static std::optional<size_t> findSectionVeneer(std::span<const SectionVeneer> veneers, const UnitTarget& target)
{
    auto found = std::ranges::find_if(veneers, [&](const SectionVeneer& veneer) {
        return veneer.target.unit == target.unit && veneer.target.offset == target.offset;
    });
    if (found == veneers.end())
        return std::nullopt;
    return static_cast<size_t>(std::distance(veneers.begin(), found));
}

static size_t offsetOfSectionVeneers(const LinkUnit& unit)
{
    return roundUpToMultipleOf<sectionVeneerAlignment>(unit.bytes.size());
}

static bool isWithinReachInSection(RelocationKind kind, size_t site, size_t target)
{
    size_t distance = site < target ? target - site : site - target;
    return static_cast<int64_t>(distance) < MacroAssembler::relocationReach(kind);
}

static std::expected<SectionUnitGraph, SectionLinkFailure> createSectionUnitGraph(const SectionLinkInputs& inputs)
{
    std::span<const LinkUnit> units = inputs.units;
    RELEASE_ASSERT(isRepresentableAs<uint32_t>(units.size()));
    SectionUnitGraph graph;
    graph.units = units;
    graph.dataAlignment = inputs.dataAlignment;
    graph.targets.grow(units.size());
    for (uint32_t unit = 0; unit < units.size(); ++unit) {
        std::span<const uint8_t> bytes = units[unit].bytes;
        std::span<const Relocation> relocations = units[unit].relocations;
        graph.targets[unit].reserveInitialCapacity(relocations.size());
        for (uint32_t index = 0; index < relocations.size(); ++index) {
            const Relocation& relocation = relocations[index];
            size_t size = MacroAssembler::relocationSize(relocation.kind);
            RELEASE_ASSERT(size && size <= bytes.size() && relocation.offset <= bytes.size() - size);
            std::optional<UnitTarget> target;
            if (namesPlaceInSection(relocation.kind))
                target = inputs.resolve(unit, relocation);
            if (!target || target->unit >= units.size() || target->offset > units[target->unit].bytes.size())
                return std::unexpected(SectionLinkFailure { SectionLinkError::SymbolWithoutUnit, unit, index });
            graph.targets[unit].append(*target);
        }
    }

    graph.placeOfUnit.fill(0, units.size());
    graph.unitsByPlace.reserveInitialCapacity(units.size());
    auto append = [&](uint32_t unit) {
        graph.placeOfUnit[unit] = static_cast<uint32_t>(graph.unitsByPlace.size());
        graph.unitsByPlace.append(unit);
    };
    for (uint32_t unit = 0; unit < units.size(); ++unit) {
        if (!units[unit].isData)
            append(unit);
    }
    graph.firstPlaceOfData = graph.unitsByPlace.size();
    for (uint32_t unit = 0; unit < units.size(); ++unit) {
        if (units[unit].isData)
            append(unit);
    }
    return graph;
}

static bool addSectionVeneers(const SectionUnitGraph& graph, SectionLayout& layout)
{
    const Vector<size_t>& unitStarts = layout.placement.unitStarts;
    bool didAdd = false;
    for (size_t place = 0; place < graph.unitsByPlace.size(); ++place) {
        uint32_t unit = graph.unitsByPlace[place];
        std::span<const Relocation> relocations = graph.units[unit].relocations;
        Vector<SectionVeneer, 0>& veneers = layout.veneersByPlace[place];
        for (uint32_t index = 0; index < relocations.size(); ++index) {
            const Relocation& relocation = relocations[index];
            const UnitTarget& target = graph.targets[unit][index];
            if (!transfersControl(relocation.kind))
                continue;
            size_t site = unitStarts[place] + relocation.offset;
            size_t targetOffset = unitStarts[graph.placeOfUnit[target.unit]] + target.offset;
            if (isWithinReachInSection(relocation.kind, site, targetOffset) || findSectionVeneer(veneers.span(), target))
                continue;
            veneers.append(SectionVeneer { target, index });
            didAdd = true;
        }
    }
    return didAdd;
}

static SectionLayout layOutSection(const SectionUnitGraph& graph, const CodeTemplates& templates)
{
    size_t numberOfUnits = graph.unitsByPlace.size();
    Vector<size_t> alignments(numberOfUnits, [&](size_t place) -> size_t {
        size_t alignment = graph.units[graph.unitsByPlace[place]].alignment;
        return place == graph.firstPlaceOfData ? std::max(alignment, graph.dataAlignment) : alignment;
    });
    if (alignments.isEmpty())
        alignments.append(1);
    Vector<size_t> alignedOffsets(FillWith { }, numberOfUnits, 0);
    Vector<size_t> sizes(FillWith { }, numberOfUnits, 0);
    BitVector fallsIntoNext;

    SectionLayout layout;
    layout.veneersByPlace.grow(numberOfUnits);
    do {
        for (size_t place = 0; place < numberOfUnits; ++place) {
            const LinkUnit& unit = graph.units[graph.unitsByPlace[place]];
            size_t numberOfVeneers = layout.veneersByPlace[place].size();
            sizes[place] = numberOfVeneers ? offsetOfSectionVeneers(unit) + numberOfVeneers * templates.sizeOfVeneer() : unit.bytes.size();
        }
        layout.placement = placeUnits({
            .sizes = sizes.span(),
            .alignedOffsets = alignedOffsets.span(),
            .alignments = alignments.span(),
            .fallsIntoNext = fallsIntoNext,
            .copyables = { },
            .uses = { },
            .firstUseByUnit = { },
            .alignmentAfterCopies = 1,
            .start = 0,
            .policy = CopyPolicy::BeforeUser,
        });
    } while (addSectionVeneers(graph, layout));
    bool hasData = graph.firstPlaceOfData < numberOfUnits;
    layout.dataOffset = hasData ? layout.placement.unitStarts[graph.firstPlaceOfData] : roundUpToMultipleOf(graph.dataAlignment, layout.placement.end);
    layout.end = std::max(layout.placement.end, layout.dataOffset);
    return layout;
}

static LinkedSection copyUnitsIntoSection(const SectionUnitGraph& graph, const SectionLayout& layout, const CodeTemplates& templates)
{
    LinkedSection section;
    section.bytes = Vector<uint8_t>(FillWith { }, layout.end, 0);
    section.dataOffset = static_cast<uint32_t>(layout.dataOffset);
    section.uniqueUnitOffsets = Vector<uint32_t>(FillWith { }, graph.units.size(), 0);
    std::span<uint8_t> bytes = section.bytes.mutableSpan();
    templates.fillWithBreakpoints(bytes.first(layout.dataOffset - layout.dataOffset % templates.breakpoint.size()));
    for (size_t place = 0; place < graph.unitsByPlace.size(); ++place) {
        uint32_t unitIndex = graph.unitsByPlace[place];
        const LinkUnit& unit = graph.units[unitIndex];
        size_t start = layout.placement.unitStarts[place];
        section.uniqueUnitOffsets[unitIndex] = static_cast<uint32_t>(start);
        memcpySpan(bytes.subspan(start), unit.bytes);
        size_t numberOfVeneers = layout.veneersByPlace[place].size();
        for (size_t veneer = 0; veneer < numberOfVeneers; ++veneer)
            memcpySpan(bytes.subspan(start + offsetOfSectionVeneers(unit) + veneer * templates.sizeOfVeneer()), templates.veneer.bytes.span());
        section.numberOfVeneers += static_cast<uint32_t>(numberOfVeneers);
    }
    return section;
}

static std::optional<SectionLinkFailure> linkUnitsOfSection(const SectionUnitGraph& graph, const SectionLayout& layout, const CodeTemplates& templates, std::span<uint8_t> bytes)
{
    const Vector<size_t>& unitStarts = layout.placement.unitStarts;
    auto offsetOfTarget = [&](const UnitTarget& target) {
        return unitStarts[graph.placeOfUnit[target.unit]] + target.offset;
    };
    for (size_t place = 0; place < graph.unitsByPlace.size(); ++place) {
        uint32_t unit = graph.unitsByPlace[place];
        size_t start = unitStarts[place];
        std::span<const Relocation> relocations = graph.units[unit].relocations;
        std::span<const SectionVeneer> veneers = layout.veneersByPlace[place].span();
        auto indexOf = [&](const Relocation& relocation) {
            return static_cast<uint32_t>(std::distance(relocations.data(), &relocation));
        };
        auto offsetOfVeneer = [&](size_t veneer) {
            return start + offsetOfSectionVeneers(graph.units[unit]) + veneer * templates.sizeOfVeneer();
        };
        std::optional<SectionLinkFailure> failure;
        applyRelocations(bytes, start, relocations, [&](const Relocation& relocation) -> std::optional<size_t> {
            const UnitTarget& target = graph.targets[unit][indexOf(relocation)];
            if (transfersControl(relocation.kind)) {
                if (auto veneer = findSectionVeneer(veneers, target))
                    return offsetOfVeneer(*veneer);
            }
            return offsetOfTarget(target);
        }, [&](const Relocation& relocation) {
            if (!failure)
                failure = SectionLinkFailure { SectionLinkError::OutOfPCRelativeReach, unit, indexOf(relocation) };
        });
        for (size_t index = 0; !failure && index < veneers.size(); ++index) {
            if (!applyRelocation(bytes, offsetOfVeneer(index), templates.veneer.relocations[0], offsetOfTarget(veneers[index].target)))
                failure = SectionLinkFailure { SectionLinkError::OutOfPCRelativeReach, unit, veneers[index].firstRelocation };
        }
        if (failure)
            return failure;
    }
    return std::nullopt;
}

std::expected<LinkedSection, SectionLinkFailure> linkSection(const SectionLinkInputs& inputs)
{
    auto graph = createSectionUnitGraph(inputs);
    if (!graph)
        return std::unexpected(graph.error());
    SectionLayout layout = layOutSection(*graph, inputs.templates);
    if (layout.end > std::min(inputs.maxSize, maxLinkedVectorSize))
        return std::unexpected(SectionLinkFailure { SectionLinkError::SectionTooLarge, 0, 0 });

    LinkedSection section = copyUnitsIntoSection(*graph, layout, inputs.templates);
    if (auto failure = linkUnitsOfSection(*graph, layout, inputs.templates, section.bytes.mutableSpan()))
        return std::unexpected(*failure);
    return section;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
