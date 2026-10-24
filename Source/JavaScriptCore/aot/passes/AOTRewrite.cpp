/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRewrite.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTResultRules.h"
#include "AOTTypePropagation.h"

namespace JSC { namespace AOT { namespace Typed {

IRBuilder& Rewrite::builder()
{
    if (!m_builder)
        m_builder.emplace(analyses.context(), function, context.types, &analyses);
    return *m_builder;
}

Block& Rewrite::edit(BlockID id, OptionSet<BlockEdit> edits)
{
    m_didEditEdges |= edits.containsAny({ BlockEdit::Edges, BlockEdit::Rest });
    return function.edit(id, edits);
}

void Rewrite::replaceUse(const Use& use, ValueID replacement)
{
    Typed::replaceUse(function, use, replacement);
}

ValueID Rewrite::insertBefore(ProgramPoint point, Origin origin, Value&& value)
{
    builder().setOrigin(origin);
    return builder().insertBefore(point, WTF::move(value));
}

void Rewrite::become(ValueID id, Value&& replacement)
{
    Value& value = edit(id);
    replacement.setOrigin(value.origin());
    replacement.type = resultTypeOf(program, function, replacement, context.types);
    value = WTF::move(replacement);
    retype(id);
}

void Rewrite::erase(BlockID block, ValueID id)
{
    m_erasedValues.set(id.index());
    m_blocksOfErasedValues.appendIfNotContains(block);
}

void Rewrite::removeParameter(BlockID block, uint32_t parameterIndex)
{
    m_removedParameters.append({ block, parameterIndex });
}

void Rewrite::replaceUsesOf(ValueID value, ValueID replacement)
{
    if (!m_substitution)
        m_substitution.emplace(function);
    m_substitution->replace(value, replacement);
}

void Rewrite::count(StatisticID statistic, uint64_t amount)
{
    context.build.statistics().count(statistic, amount);
}

PassOutcome Rewrite::commit()
{
    for (unsigned rule = 0; !m_edits.isEmpty() && rule <= m_rule; ++rule) {
        for (Edit& edit : m_edits) {
            if (edit.rule == rule)
                edit.run();
        }
    }
    if (function.numberOfEdits() == m_numberOfEditsAtStart)
        return PassOutcome::Unchanged;

    builder().applyInsertions();
    for (BlockID block : m_blocksOfErasedValues) {
        const BlockValues& values = at(block).values;
        BitVector indexes;
        for (size_t position = 0; position < values.size(); ++position) {
            if (m_erasedValues.get(values[position].index()))
                indexes.set(position);
        }
        builder().eraseValues(block, indexes);
    }
    for (auto [block, parameterIndex] : m_removedParameters) {
        BitVector indexes;
        indexes.set(parameterIndex);
        builder().removeParameters(block, indexes);
    }
    if (m_substitution)
        m_substitution->apply(function, &analyses);
    if (m_didEditEdges)
        analyses.didChangeControlFlow();
    else
        analyses.didChangeValues();
    if (!m_retypedValues.isEmpty())
        narrowValueTypes(TypePropagationInputs { program, context.id, function, analyses, context.types, context.build, &context.typingMemo }, m_retypedValues.span());
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
