/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestEvents.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool mayWriteDependenciesOf(const TestCondition& condition, const Program& program, const Effects& effects)
{
    return effects.maySuspend || std::ranges::any_of(condition.dependsOn, [&](const MemoryLocation& location) {
        return effects.writes.overlaps(program, location);
    });
}

// A use that is about to be rewritten comes with the effects of its fast form. The operation as it stands bounds the objects that form reaches only if it writes at least as much.
EndingsAtWriter::EndingsAtWriter(FunctionAnalyses& analyses, ValueID writer, const EffectsOnEdges& effectsOfWriter)
    : m_analyses(analyses)
    , m_writer(writer)
    , m_effects(effectsOfWriter.normal)
    , m_isBoundedByWriter(!m_effects.maySuspend && m_effects.writes.isSubsetOf(analyses.effectsOf(writer).normal.writes))
{
}

ProofEnding EndingsAtWriter::endingOf(const TestCondition& condition, const std::optional<GuardDependencies>& dependenciesOfTest)
{
    if (condition.lastsForever())
        return ProofEnding::DoesNotEnd;
    if (!mayWriteDependenciesOf(condition, m_analyses.program(), m_effects))
        return ProofEnding::DoesNotEnd;
    if (dependenciesOfTest && leavesSlotsAsTested(m_analyses.program(), m_analyses.function(), m_analyses.function().at(m_writer), m_effects, dependenciesOfTest->slotState))
        return ProofEnding::DoesNotEnd;
    if (!m_isBoundedByWriter)
        return ProofEnding::MayEnd;
    if (!dependenciesOfTest)
        return ProofEnding::DoesNotEnd;

    if (!m_guardWriter)
        m_guardWriter.emplace(m_analyses, m_writer);
    return m_guardWriter->mayEnd(*dependenciesOfTest) ? ProofEnding::MayEnd : ProofEnding::DoesNotEnd;
}

GuardWriter::WrittenDependencies EndingsAtWriter::writtenOf(const TestCondition& condition, const GuardDependencies& dependencies)
{
    using EndedProofs = GuardWriter::EndedProofs;
    if (condition.lastsForever())
        return { EndedProofs::None, 0 };
    if (!mayWriteDependenciesOf(condition, m_analyses.program(), m_effects))
        return { EndedProofs::None, 0 };
    if (leavesSlotsAsTested(m_analyses.program(), m_analyses.function(), m_analyses.function().at(m_writer), m_effects, dependencies.slotState))
        return { EndedProofs::None, 0 };
    if (!m_isBoundedByWriter)
        return { EndedProofs::All, 0 };

    if (!m_guardWriter)
        m_guardWriter.emplace(m_analyses, m_writer);
    return m_guardWriter->writtenOf(dependencies);
}

ProofEnding EndingsAtWriter::endingOf(ValueID subject, bool subjectComesFromOutside, const GuardWriter::WrittenDependencies& written)
{
    switch (written.endedProofs) {
    case GuardWriter::EndedProofs::None:
        return ProofEnding::DoesNotEnd;
    case GuardWriter::EndedProofs::All:
        return ProofEnding::MayEnd;
    case GuardWriter::EndedProofs::AboutSomeObjects:
        break;
    }
    return m_guardWriter->mayEndProofAbout(subject, subjectComesFromOutside, written) ? ProofEnding::MayEnd : ProofEnding::DoesNotEnd;
}

ProofEnding endingBetweenTestAndBranch(FunctionAnalyses& analyses, const TestCondition& condition, ValueID writer, const EffectsOnEdges& effectsOfWriter)
{
    if (condition.lastsForever())
        return ProofEnding::DoesNotEnd;
    std::optional<GuardDependencies> dependencies = dependenciesOfGuardOf(analyses, condition.test);
    if (dependencies)
        dependencies->slotState = TestedSlotState::None;
    return EndingsAtWriter(analyses, writer, effectsOfWriter).endingOf(condition, dependencies);
}

ProofEnding endingOfTerminator(FunctionAnalyses& analyses, const TestCondition& condition, const Effects& effectsOfTerminator)
{
    if (condition.lastsForever())
        return ProofEnding::DoesNotEnd;
    return mayWriteDependenciesOf(condition, analyses.program(), effectsOfTerminator) ? ProofEnding::MayEnd : ProofEnding::DoesNotEnd;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
