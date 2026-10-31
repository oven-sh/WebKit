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
#include "AOTBitVector.h"
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static constexpr size_t veneerAlignment = 4;
static constexpr uint64_t maxVectorSize = defaultLimitValue(LimitID::VectorBytes, hostArchitecture());

namespace {

struct UseRows {
    std::span<const CopyableUse> of(uint32_t user) const
    {
        return uses.span().subspan(firstUseByUser[user], firstUseByUser[user + 1] - firstUseByUser[user]);
    }

    Vector<CopyableUse> uses;
    Vector<uint32_t> firstUseByUser;
};

struct UnitGraph {
    bool isCopyable(uint32_t unit) const { return units[unit].sharing == UnitSharing::Copyable; }

    std::span<const LinkUnit> units;
    Vector<Vector<UnitTarget, 0>> targets;
    Vector<uint32_t> numberWithinSharing;
    Vector<uint32_t> uniqueUnits;
    Vector<uint32_t> copyableUnits;
    UseRows usesOfUniqueUnits;
    UseRows usesOfCopyableUnits;
    // The unique units from this one on are data.
    size_t firstDataUnit { 0 };
    size_t dataAlignment { 1 };
};

struct Veneer {
    UnitTarget target;
    uint32_t firstRelocation;
};

struct SectionLayout {
    UnitPlacement placement;
    Vector<Vector<Veneer, 0>> veneersByUniqueUnit;
    size_t dataOffset { 0 };
    size_t end { 0 };
};

} // anonymous namespace

static std::optional<size_t> findVeneer(std::span<const Veneer> veneers, const UnitTarget& target)
{
    auto found = std::ranges::find_if(veneers, [&](const Veneer& veneer) {
        return veneer.target.unit == target.unit && veneer.target.offset == target.offset;
    });
    if (found == veneers.end())
        return std::nullopt;
    return static_cast<size_t>(std::distance(veneers.begin(), found));
}

static size_t offsetOfVeneers(const LinkUnit& unit)
{
    return roundUpToMultipleOf<veneerAlignment>(unit.bytes.size());
}

static bool isWithinReach(RelocationKind kind, size_t site, size_t target)
{
    size_t distance = site < target ? target - site : site - target;
    return static_cast<int64_t>(distance) < MacroAssembler::relocationReach(kind);
}

static UseRows collectUses(const UnitGraph& graph, std::span<const uint32_t> users)
{
    UseRows rows;
    rows.firstUseByUser.reserveInitialCapacity(users.size() + 1);
    Vector<std::optional<uint32_t>> useByCopyable(FillWith { }, graph.copyableUnits.size(), std::nullopt);
    for (uint32_t user : users) {
        uint32_t firstUse = static_cast<uint32_t>(rows.uses.size());
        rows.firstUseByUser.append(firstUse);
        std::span<const Relocation> relocations = graph.units[user].relocations;
        for (size_t index = 0; index < relocations.size(); ++index) {
            uint32_t targetUnit = graph.targets[user][index].unit;
            if (!graph.isCopyable(targetUnit))
                continue;
            const Relocation& relocation = relocations[index];
            uint32_t copyable = graph.numberWithinSharing[targetUnit];
            uint32_t end = relocation.offset + MacroAssembler::relocationSize(relocation.kind);
            int64_t reach = MacroAssembler::relocationReach(relocation.kind);
            std::optional<uint32_t>& use = useByCopyable[copyable];
            if (!use) {
                use = static_cast<uint32_t>(rows.uses.size());
                rows.uses.append(CopyableUse { copyable, relocation.offset, end, reach });
                continue;
            }
            CopyableUse& existing = rows.uses[*use];
            existing.firstSite = std::min(existing.firstSite, relocation.offset);
            existing.lastSite = std::max(existing.lastSite, end);
            existing.reach = std::min(existing.reach, reach);
        }
        for (const CopyableUse& use : rows.uses.span().subspan(firstUse))
            useByCopyable[use.copyable] = std::nullopt;
    }
    rows.firstUseByUser.append(static_cast<uint32_t>(rows.uses.size()));
    return rows;
}

static std::expected<UnitGraph, SectionLinkFailure> createGraph(const SectionLinkInputs& inputs)
{
    std::span<const LinkUnit> units = inputs.units;
    RELEASE_ASSERT(isRepresentableAs<uint32_t>(units.size()));
    UnitGraph graph;
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
            std::optional<UnitTarget> target = inputs.resolve(unit, relocation);
            if (!target || target->unit >= units.size() || target->offset > units[target->unit].bytes.size())
                return std::unexpected(SectionLinkFailure { SectionLinkError::SymbolWithoutUnit, unit, index });
            graph.targets[unit].append(*target);
        }
    }

    graph.numberWithinSharing.fill(0, units.size());
    auto append = [&](Vector<uint32_t>& unitsWithSameSharing, uint32_t unit) {
        graph.numberWithinSharing[unit] = static_cast<uint32_t>(unitsWithSameSharing.size());
        unitsWithSameSharing.append(unit);
    };
    for (uint32_t unit = 0; unit < units.size(); ++unit) {
        if (graph.isCopyable(unit))
            append(graph.copyableUnits, unit);
        else if (!units[unit].isData)
            append(graph.uniqueUnits, unit);
    }
    graph.firstDataUnit = graph.uniqueUnits.size();
    for (uint32_t unit = 0; unit < units.size(); ++unit) {
        if (!graph.isCopyable(unit) && units[unit].isData)
            append(graph.uniqueUnits, unit);
    }
    graph.usesOfUniqueUnits = collectUses(graph, graph.uniqueUnits.span());
    graph.usesOfCopyableUnits = collectUses(graph, graph.copyableUnits.span());
    return graph;
}

static bool addVeneers(const UnitGraph& graph, SectionLayout& layout)
{
    const Vector<size_t>& unitStarts = layout.placement.unitStarts;
    bool didAdd = false;
    for (size_t number = 0; number < graph.uniqueUnits.size(); ++number) {
        uint32_t unit = graph.uniqueUnits[number];
        std::span<const Relocation> relocations = graph.units[unit].relocations;
        Vector<Veneer, 0>& veneers = layout.veneersByUniqueUnit[number];
        for (uint32_t index = 0; index < relocations.size(); ++index) {
            const Relocation& relocation = relocations[index];
            const UnitTarget& target = graph.targets[unit][index];
            if (!transfersControl(relocation.kind) || graph.isCopyable(target.unit))
                continue;
            size_t site = unitStarts[number] + relocation.offset;
            size_t targetOffset = unitStarts[graph.numberWithinSharing[target.unit]] + target.offset;
            if (isWithinReach(relocation.kind, site, targetOffset) || findVeneer(veneers.span(), target))
                continue;
            veneers.append(Veneer { target, index });
            didAdd = true;
        }
    }
    return didAdd;
}

static SectionLayout layOutSection(const UnitGraph& graph, const CodeTemplates& templates)
{
    size_t numberOfUniqueUnits = graph.uniqueUnits.size();
    Vector<size_t> alignments(numberOfUniqueUnits, [&](size_t number) -> size_t {
        size_t alignment = graph.units[graph.uniqueUnits[number]].alignment;
        return number == graph.firstDataUnit ? std::max(alignment, graph.dataAlignment) : alignment;
    });
    if (alignments.isEmpty())
        alignments.append(1);
    Vector<CopyableUnit> copyables(graph.copyableUnits.size(), [&](size_t number) {
        const LinkUnit& unit = graph.units[graph.copyableUnits[number]];
        return CopyableUnit { unit.bytes.size(), unit.alignment };
    });
    Vector<size_t> alignedOffsets(FillWith { }, numberOfUniqueUnits, 0);
    Vector<size_t> sizes(FillWith { }, numberOfUniqueUnits, 0);
    BitVector fallsIntoNext;

    SectionLayout layout;
    layout.veneersByUniqueUnit.grow(numberOfUniqueUnits);
    do {
        for (size_t number = 0; number < numberOfUniqueUnits; ++number) {
            const LinkUnit& unit = graph.units[graph.uniqueUnits[number]];
            size_t numberOfVeneers = layout.veneersByUniqueUnit[number].size();
            sizes[number] = numberOfVeneers ? offsetOfVeneers(unit) + numberOfVeneers * templates.sizeOfVeneer() : unit.bytes.size();
        }
        layout.placement = placeUnits({
            .sizes = sizes.span(),
            .alignedOffsets = alignedOffsets.span(),
            .alignments = alignments.span(),
            .fallsIntoNext = fallsIntoNext,
            .copyables = copyables.span(),
            .uses = graph.usesOfUniqueUnits.uses.span(),
            .firstUseByUnit = graph.usesOfUniqueUnits.firstUseByUser.span(),
            .usesOfCopyables = graph.usesOfCopyableUnits.uses.span(),
            .firstUseByCopyable = graph.usesOfCopyableUnits.firstUseByUser.span(),
            .alignmentAfterCopies = 1,
            .start = 0,
            .shiftOfFirstUnit = 0,
            .policy = CopyPolicy::BeforeUser,
        });
    } while (addVeneers(graph, layout));
    bool hasData = graph.firstDataUnit < numberOfUniqueUnits;
    layout.dataOffset = hasData ? layout.placement.unitStarts[graph.firstDataUnit] : roundUpToMultipleOf(graph.dataAlignment, layout.placement.end);
    layout.end = std::max(layout.placement.end, layout.dataOffset);
    return layout;
}

static LinkedSection copyUnits(const UnitGraph& graph, const SectionLayout& layout, const CodeTemplates& templates)
{
    LinkedSection section;
    section.bytes = Vector<uint8_t>(FillWith { }, layout.end, 0);
    section.dataOffset = static_cast<uint32_t>(layout.dataOffset);
    section.uniqueUnitOffsets = Vector<uint32_t>(FillWith { }, graph.units.size(), 0);
    std::span<uint8_t> bytes = section.bytes.mutableSpan();
    templates.fillWithBreakpoints(bytes.first(layout.dataOffset - layout.dataOffset % templates.breakpoint.size()));
    for (size_t number = 0; number < graph.uniqueUnits.size(); ++number) {
        uint32_t unitIndex = graph.uniqueUnits[number];
        const LinkUnit& unit = graph.units[unitIndex];
        size_t start = layout.placement.unitStarts[number];
        section.uniqueUnitOffsets[unitIndex] = static_cast<uint32_t>(start);
        memcpySpan(bytes.subspan(start), unit.bytes);
        size_t numberOfVeneers = layout.veneersByUniqueUnit[number].size();
        for (size_t veneer = 0; veneer < numberOfVeneers; ++veneer)
            memcpySpan(bytes.subspan(start + offsetOfVeneers(unit) + veneer * templates.sizeOfVeneer()), templates.veneer.bytes.span());
        section.numberOfVeneers += static_cast<uint32_t>(numberOfVeneers);
    }
    section.copies.reserveInitialCapacity(layout.placement.copies.size());
    for (const PlacedCopy& copy : layout.placement.copies) {
        uint32_t unitIndex = graph.copyableUnits[copy.copyable];
        memcpySpan(bytes.subspan(copy.offset), graph.units[unitIndex].bytes);
        section.copies.append(PlacedUnit { unitIndex, static_cast<uint32_t>(copy.offset) });
    }
    ASSERT(std::ranges::is_sorted(section.copies, { }, &PlacedUnit::offset));
    return section;
}

static std::optional<SectionLinkFailure> linkUnits(const UnitGraph& graph, const SectionLayout& layout, const CodeTemplates& templates, std::span<uint8_t> bytes)
{
    const UnitPlacement& placement = layout.placement;
    Vector<size_t> copyOffsetByCopyable(FillWith { }, graph.copyableUnits.size(), 0);
    auto offsetOfUniqueTarget = [&](const UnitTarget& target) {
        return placement.unitStarts[graph.numberWithinSharing[target.unit]] + target.offset;
    };
    auto link = [&](uint32_t unit, size_t start, std::span<const CopyableUse> uses, std::span<const size_t> copyOffsetByUse, std::span<const Veneer> veneers) {
        RELEASE_ASSERT(uses.size() == copyOffsetByUse.size());
        for (size_t use = 0; use < uses.size(); ++use)
            copyOffsetByCopyable[uses[use].copyable] = copyOffsetByUse[use];

        std::span<const Relocation> relocations = graph.units[unit].relocations;
        auto indexOf = [&](const Relocation& relocation) {
            return static_cast<uint32_t>(std::distance(relocations.data(), &relocation));
        };
        auto offsetOfVeneer = [&](size_t veneer) {
            return start + offsetOfVeneers(graph.units[unit]) + veneer * templates.sizeOfVeneer();
        };
        std::optional<SectionLinkFailure> failure;
        applyRelocations(bytes, start, relocations, [&](const Relocation& relocation) -> std::optional<size_t> {
            const UnitTarget& target = graph.targets[unit][indexOf(relocation)];
            if (graph.isCopyable(target.unit))
                return copyOffsetByCopyable[graph.numberWithinSharing[target.unit]] + target.offset;
            if (transfersControl(relocation.kind)) {
                if (auto veneer = findVeneer(veneers, target))
                    return offsetOfVeneer(*veneer);
            }
            return offsetOfUniqueTarget(target);
        }, [&](const Relocation& relocation) {
            if (!failure)
                failure = SectionLinkFailure { SectionLinkError::OutOfPCRelativeReach, unit, indexOf(relocation) };
        });
        for (size_t index = 0; !failure && index < veneers.size(); ++index) {
            if (!applyRelocation(bytes, offsetOfVeneer(index), templates.veneer.relocations[0], offsetOfUniqueTarget(veneers[index].target)))
                failure = SectionLinkFailure { SectionLinkError::OutOfPCRelativeReach, unit, veneers[index].firstRelocation };
        }
        return failure;
    };

    for (uint32_t number = 0; number < graph.uniqueUnits.size(); ++number) {
        std::span<const CopyableUse> uses = graph.usesOfUniqueUnits.of(number);
        std::span<const size_t> copyOffsetByUse = placement.copyOffsetByUse.span().subspan(graph.usesOfUniqueUnits.firstUseByUser[number], uses.size());
        if (auto failure = link(graph.uniqueUnits[number], placement.unitStarts[number], uses, copyOffsetByUse, layout.veneersByUniqueUnit[number].span()))
            return failure;
    }
    for (const PlacedCopy& copy : placement.copies) {
        if (auto failure = link(graph.copyableUnits[copy.copyable], copy.offset, graph.usesOfCopyableUnits.of(copy.copyable), copy.copyOffsetByUse.span(), { }))
            return failure;
    }
    return std::nullopt;
}

std::expected<LinkedSection, SectionLinkFailure> linkSection(const SectionLinkInputs& inputs)
{
    auto graph = createGraph(inputs);
    if (!graph)
        return std::unexpected(graph.error());
    SectionLayout layout = layOutSection(*graph, inputs.templates);
    if (layout.end > std::min(inputs.maxSize, maxVectorSize))
        return std::unexpected(SectionLinkFailure { SectionLinkError::SectionTooLarge, 0, 0 });

    LinkedSection section = copyUnits(*graph, layout, inputs.templates);
    if (auto failure = linkUnits(*graph, layout, inputs.templates, section.bytes.mutableSpan()))
        return std::unexpected(*failure);
    return section;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
