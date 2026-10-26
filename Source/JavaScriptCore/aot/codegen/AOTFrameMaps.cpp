/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameMaps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffects.h"
#include "AOTEffectsDeclared.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeldOperands.h"
#include "AOTIR.h"
#include "AOTRepresentation.h"
#include "AOTResultRules.h"
#include "AOTSavedFrameMap.h"
#include "AOTSort.h"
#include <algorithm>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::all_of(allMembersOf<Representation>, [](Representation representation) {
    return frameSlotKindOf(representation).has_value() == holdsCells(representation);
}));

const SuspensionRoots& FrameMaps::rootsAt(ValueID suspension) const
{
    auto roots = std::ranges::lower_bound(m_suspensions, suspension, { }, &SuspensionRoots::suspension);
    RELEASE_ASSERT(roots != m_suspensions.end() && roots->suspension == suspension);
    return *roots;
}

BitVector FrameMaps::suspensionValues() const
{
    BitVector values;
    if (m_suspensions.isEmpty())
        return values;
    values.ensureSize(m_suspensions.last().suspension.index() + 1);
    for (const SuspensionRoots& roots : m_suspensions)
        values.quickSet(roots.suspension.index());
    return values;
}

static ASCIILiteral labelOf(FrameSlotKind kind)
{
    switch (kind) {
    case FrameSlotKind::Value:
        return "value"_s;
    case FrameSlotKind::Cell:
        return "cell"_s;
    case FrameSlotKind::NarrowReference:
        return "narrow"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FrameMaps::dump(PrintStream& out, const Function&) const
{
    for (const SuspensionRoots& roots : m_suspensions) {
        out.print("v", roots.suspension.index(), ":");
        for (const FrameRoot& root : roots.roots)
            out.print(" v", root.value.index(), ":", labelOf(root.kind));
        for (const FrameResultRoot& root : roots.resultRoots)
            out.print(" v", root.producer.index(), ".", root.index, ":", labelOf(root.kind));
        out.print("\n");
    }
}

static void appendResultRoots(const Function& function, const UseLists& useLists, ValueID producer, uint32_t numberOfResults, SuspensionRoots& roots)
{
    Vector<std::optional<FrameSlotKind>, 4> kinds(numberOfResults);
    for (const Use& use : useLists.usesOf(producer)) {
        RELEASE_ASSERT(use.kind == Use::Kind::Operand);
        const Value& reader = function.at(use.user);
        RELEASE_ASSERT(reader.opcode == Opcode::ResultAt);
        kinds[reader.index()] = frameSlotKindOf(reader.representation);
    }
    for (uint32_t index = 0; index < numberOfResults; ++index) {
        if (kinds[index])
            roots.resultRoots.append({ producer, index, *kinds[index] });
    }
}

static SuspensionRoots collectRoots(const Program& program, const Function& function, FunctionAnalyses& analyses, const Liveness::LocalCalc& liveAfterSuspension, ValueID suspension)
{
    const UseLists& useLists = analyses.useLists();
    SuspensionRoots roots { suspension, { }, { } };
    auto addRoot = [&](ValueID live) {
        if (live == suspension || isDeferredClosure(function, live))
            return;
        const Value& value = function.at(live);
        if (std::optional<uint32_t> numberOfResults = numberOfResultsOf(program, function, value)) {
            appendResultRoots(function, useLists, live, *numberOfResults, roots);
            return;
        }
        if (std::optional<FrameSlotKind> kind = frameSlotKindOf(value.representation))
            roots.roots.append({ live, *kind });
    };
    liveAfterSuspension.forEachLive(addRoot);
    BitVector held;
    for (ValueID operand : heldOperandsOf(function.at(suspension))) {
        if (!liveAfterSuspension.isLive(operand) && !held.set(operand.index()))
            addRoot(operand);
    }

    BuildContext& build = analyses.context().build;
    if (roots.size() > build.limit(LimitID::MaxFrameMapRoots))
        build.reportLimitExceeded(LimitID::MaxFrameMapRoots, roots.size(), analyses.subject(suspension));
    return roots;
}

FrameMaps computeFrameMaps(const Program& program, const Function& function, FunctionAnalyses& analyses)
{
    FrameMaps maps;
    if (function.role != FunctionRole::CoroutineBody)
        return maps;
    RELEASE_ASSERT(function.representationsAreChosen);

    uint64_t numberOfRoots = 0;
    Vector<uint32_t, 4> suspensionIndexes;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (maySuspend(function.at(values[index]).opcode) && analyses.maySuspend(values[index]))
                suspensionIndexes.append(index);
        }
        if (suspensionIndexes.isEmpty())
            continue;

        Liveness::LocalCalc localCalc(analyses.liveness(), block);
        uint32_t pointIndex = values.size();
        while (!suspensionIndexes.isEmpty()) {
            uint32_t suspensionIndex = suspensionIndexes.takeLast();
            while (pointIndex > suspensionIndex + 1)
                localCalc.execute(--pointIndex);
            SuspensionRoots roots = collectRoots(program, function, analyses, localCalc, values[suspensionIndex]);
            numberOfRoots += roots.size();
            maps.m_suspensions.append(WTF::move(roots));
        }
    }

    AOT::sort(maps.m_suspensions, { }, &SuspensionRoots::suspension);
    analyses.context().build.statistics().count(StatisticID::SuspensionRoots, numberOfRoots);
    return maps;
}

uint32_t FrameMapRecords::addMap(std::span<const uint32_t> map)
{
    RELEASE_ASSERT(SavedFrameMap::tryCreate(map, 0)->numberOfWords() == map.size());
    for (uint32_t offset : m_offsets) {
        if (spanHasPrefix(words.subspan(offset), map))
            return offset;
    }
    uint32_t offset = words.size();
    m_offsets.append(offset);
    words.append(map);
    return offset;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
