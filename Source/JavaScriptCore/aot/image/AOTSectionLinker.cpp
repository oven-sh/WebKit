/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSectionLinker.h"

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTUnitPlacement.h"
#include "AOTVector.h"
#include <algorithm>
#include <iterator>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static constexpr size_t veneerAlignment = 4;
static constexpr uint64_t maxVectorSize = defaultLimitValue(LimitID::VectorBytes, hostArchitecture());

namespace {

struct UnitGraph {
    bool isCopyable(uint32_t unit) const { return units[unit].sharing == UnitSharing::Copyable; }

    std::span<const LinkUnit> units;
    Vector<Vector<UnitTarget, 0>> targets;
    Vector<uint32_t> numberWithinSharing;
    Vector<uint32_t> uniqueUnits;
    Vector<uint32_t> copyableUnits;
};

struct UseRows {
    std::span<const CopyableUse> of(uint32_t user) const
    {
        return uses.span().subspan(firstUseByUser[user], firstUseByUser[user + 1] - firstUseByUser[user]);
    }

    Vector<CopyableUse> uses;
    Vector<uint32_t> firstUseByUser;
};

struct CopyableUses {
    UseRows ofUniqueUnits;
    UseRows ofCopyableUnits;
};

struct Veneer {
    UnitTarget target;
    uint32_t firstRelocation;
};

struct SectionLayout {
    UnitPlacement placement;
    Vector<Vector<Veneer, 0>> veneersByUniqueUnit;
};

class RelocationLinker {
public:
    RelocationLinker(const UnitGraph&, const CopyableUses&, const SectionLayout&, const CodeTemplates&, std::span<uint8_t> bytes);

    std::optional<SectionLinkFailure> run();

private:
    struct PlacedUser {
        uint32_t unit;
        size_t start;
        std::span<const CopyableUse> uses;
        std::span<const size_t> copyOffsetByUse;
        std::span<const Veneer> veneers;
    };

    size_t offsetOfUniqueTarget(const UnitTarget&) const;
    size_t offsetOfVeneer(const PlacedUser&, size_t veneer) const;
    std::optional<SectionLinkFailure> link(const PlacedUser&);
    std::optional<SectionLinkFailure> linkVeneers(const PlacedUser&);

    const UnitGraph& m_graph;
    const CopyableUses& m_uses;
    const SectionLayout& m_layout;
    const CodeTemplates& m_templates;
    std::span<uint8_t> m_bytes;
    Vector<size_t> m_copyOffsetByCopyable;
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

static std::expected<Vector<Vector<UnitTarget, 0>>, SectionLinkFailure> resolveTargets(const SectionLinkInputs& inputs)
{
    std::span<const LinkUnit> units = inputs.units;
    Vector<Vector<UnitTarget, 0>> targets(units.size());
    for (uint32_t unit = 0; unit < units.size(); ++unit) {
        std::span<const uint8_t> bytes = units[unit].bytes;
        std::span<const Relocation> relocations = units[unit].relocations;
        targets[unit].reserveInitialCapacity(relocations.size());
        for (uint32_t index = 0; index < relocations.size(); ++index) {
            const Relocation& relocation = relocations[index];
            size_t size = MacroAssembler::relocationSize(relocation.kind);
            RELEASE_ASSERT(size && size <= bytes.size() && relocation.offset <= bytes.size() - size);
            std::optional<UnitTarget> target = inputs.resolve(unit, relocation);
            if (!target || target->unit >= units.size() || target->offset > units[target->unit].bytes.size())
                return std::unexpected(SectionLinkFailure { SectionLinkError::SymbolWithoutUnit, unit, index });
            targets[unit].append(*target);
        }
    }
    return targets;
}

static std::expected<UnitGraph, SectionLinkFailure> createGraph(const SectionLinkInputs& inputs)
{
    RELEASE_ASSERT(isRepresentableAs<uint32_t>(inputs.units.size()));
    auto targets = resolveTargets(inputs);
    if (!targets)
        return std::unexpected(targets.error());

    UnitGraph graph;
    graph.units = inputs.units;
    graph.targets = WTF::move(*targets);
    graph.numberWithinSharing.reserveInitialCapacity(inputs.units.size());
    for (uint32_t unit = 0; unit < inputs.units.size(); ++unit) {
        Vector<uint32_t>& unitsWithSameSharing = graph.isCopyable(unit) ? graph.copyableUnits : graph.uniqueUnits;
        graph.numberWithinSharing.append(static_cast<uint32_t>(unitsWithSameSharing.size()));
        unitsWithSameSharing.append(unit);
    }
    return graph;
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

static SectionLayout layOutSection(const UnitGraph& graph, const CopyableUses& uses, const CodeTemplates& templates)
{
    size_t numberOfUniqueUnits = graph.uniqueUnits.size();
    Vector<size_t> alignments(numberOfUniqueUnits, [&](size_t number) -> size_t {
        return graph.units[graph.uniqueUnits[number]].alignment;
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
            .uses = uses.ofUniqueUnits.uses.span(),
            .firstUseByUnit = uses.ofUniqueUnits.firstUseByUser.span(),
            .usesOfCopyables = uses.ofCopyableUnits.uses.span(),
            .firstUseByCopyable = uses.ofCopyableUnits.firstUseByUser.span(),
            .alignmentAfterCopies = 1,
            .start = 0,
            .shiftOfFirstUnit = 0,
            .policy = CopyPolicy::BeforeUser,
        });
    } while (addVeneers(graph, layout));
    return layout;
}

static LinkedSection copyUnits(const UnitGraph& graph, const SectionLayout& layout, const CodeTemplates& templates)
{
    LinkedSection section;
    section.bytes = Vector<uint8_t>(FillWith { }, layout.placement.end, 0);
    section.uniqueUnitOffsets = Vector<uint32_t>(FillWith { }, graph.units.size(), 0);
    std::span<uint8_t> bytes = section.bytes.mutableSpan();
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

RelocationLinker::RelocationLinker(const UnitGraph& graph, const CopyableUses& uses, const SectionLayout& layout, const CodeTemplates& templates, std::span<uint8_t> bytes)
    : m_graph(graph)
    , m_uses(uses)
    , m_layout(layout)
    , m_templates(templates)
    , m_bytes(bytes)
    , m_copyOffsetByCopyable(FillWith { }, graph.copyableUnits.size(), 0)
{
}

std::optional<SectionLinkFailure> RelocationLinker::run()
{
    const UnitPlacement& placement = m_layout.placement;
    for (uint32_t number = 0; number < m_graph.uniqueUnits.size(); ++number) {
        std::span<const CopyableUse> uses = m_uses.ofUniqueUnits.of(number);
        auto failure = link({
            .unit = m_graph.uniqueUnits[number],
            .start = placement.unitStarts[number],
            .uses = uses,
            .copyOffsetByUse = placement.copyOffsetByUse.span().subspan(m_uses.ofUniqueUnits.firstUseByUser[number], uses.size()),
            .veneers = m_layout.veneersByUniqueUnit[number].span(),
        });
        if (failure)
            return failure;
    }
    for (const PlacedCopy& copy : placement.copies) {
        auto failure = link({
            .unit = m_graph.copyableUnits[copy.copyable],
            .start = copy.offset,
            .uses = m_uses.ofCopyableUnits.of(copy.copyable),
            .copyOffsetByUse = copy.copyOffsetByUse.span(),
            .veneers = { },
        });
        if (failure)
            return failure;
    }
    return std::nullopt;
}

size_t RelocationLinker::offsetOfUniqueTarget(const UnitTarget& target) const
{
    return m_layout.placement.unitStarts[m_graph.numberWithinSharing[target.unit]] + target.offset;
}

size_t RelocationLinker::offsetOfVeneer(const PlacedUser& user, size_t veneer) const
{
    return user.start + offsetOfVeneers(m_graph.units[user.unit]) + veneer * m_templates.sizeOfVeneer();
}

std::optional<SectionLinkFailure> RelocationLinker::link(const PlacedUser& user)
{
    RELEASE_ASSERT(user.uses.size() == user.copyOffsetByUse.size());
    for (size_t use = 0; use < user.uses.size(); ++use)
        m_copyOffsetByCopyable[user.uses[use].copyable] = user.copyOffsetByUse[use];

    std::span<const Relocation> relocations = m_graph.units[user.unit].relocations;
    const Vector<UnitTarget, 0>& targets = m_graph.targets[user.unit];
    auto indexOf = [&](const Relocation& relocation) {
        return static_cast<uint32_t>(std::distance(relocations.data(), &relocation));
    };
    std::optional<SectionLinkFailure> failure;
    applyRelocations(m_bytes, user.start, relocations, [&](const Relocation& relocation) -> std::optional<size_t> {
        const UnitTarget& target = targets[indexOf(relocation)];
        if (m_graph.isCopyable(target.unit))
            return m_copyOffsetByCopyable[m_graph.numberWithinSharing[target.unit]] + target.offset;
        if (transfersControl(relocation.kind)) {
            if (auto veneer = findVeneer(user.veneers, target))
                return offsetOfVeneer(user, *veneer);
        }
        return offsetOfUniqueTarget(target);
    }, [&](const Relocation& relocation) {
        if (!failure)
            failure = SectionLinkFailure { SectionLinkError::OutOfPCRelativeReach, user.unit, indexOf(relocation) };
    });
    if (failure)
        return failure;
    return linkVeneers(user);
}

std::optional<SectionLinkFailure> RelocationLinker::linkVeneers(const PlacedUser& user)
{
    for (size_t index = 0; index < user.veneers.size(); ++index) {
        const Veneer& veneer = user.veneers[index];
        if (!applyRelocation(m_bytes, offsetOfVeneer(user, index), m_templates.veneer.relocations[0], offsetOfUniqueTarget(veneer.target)))
            return SectionLinkFailure { SectionLinkError::OutOfPCRelativeReach, user.unit, veneer.firstRelocation };
    }
    return std::nullopt;
}

std::expected<LinkedSection, SectionLinkFailure> linkSection(const SectionLinkInputs& inputs)
{
    auto graph = createGraph(inputs);
    if (!graph)
        return std::unexpected(graph.error());
    CopyableUses uses { collectUses(*graph, graph->uniqueUnits.span()), collectUses(*graph, graph->copyableUnits.span()) };

    SectionLayout layout = layOutSection(*graph, uses, inputs.templates);
    if (layout.placement.end > std::min(inputs.maxSize, maxVectorSize))
        return std::unexpected(SectionLinkFailure { SectionLinkError::SectionTooLarge, 0, 0 });

    LinkedSection section = copyUnits(*graph, layout, inputs.templates);
    if (auto failure = RelocationLinker(*graph, uses, layout, inputs.templates, section.bytes.mutableSpan()).run())
        return std::unexpected(*failure);
    return section;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
