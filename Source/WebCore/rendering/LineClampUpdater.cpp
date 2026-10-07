/**
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "LineClampUpdater.h"

#include "LayoutIntegrationLineLayout.h"
#include "LocalFrameView.h"
#include "LocalFrameViewInlines.h"
#include "RenderBlockFlow.h"
#include "RenderBlockFlowInlines.h"
#include "RenderBoxInlines.h"
#include "RenderObjectInlines.h"
#include "RenderView.h"
#include "StyleDisplay.h"
#include "StyleMaximumLines.h"

namespace WebCore {

LineClampUpdater::LineClampUpdater(const RenderBlock& blockContainer)
    : m_blockContainer(blockContainer)
{
    auto* layoutState = m_blockContainer->view().frameView().layoutContext().layoutState();
    if (!layoutState)
        return;

    m_previousLineClamp = layoutState->lineClamp();
    auto maximumLinesForBlockContainer = m_blockContainer->style().maxLines().tryValue();
    // "If the box is a multicol container, the behavior is the same as continue: auto."
    // https://drafts.csswg.org/css-overflow-4/#continue
    if (CheckedPtr blockFlow = dynamicDowncast<RenderBlockFlow>(blockContainer); blockFlow && blockFlow->multiColumnFlow())
        maximumLinesForBlockContainer = { };
    if (blockContainer.isFieldset() || (layoutState->legacyLineClamp() && blockContainer.isNonReplacedAtomicInlineLevelBox()) || blockContainer.isFloatingOrOutOfFlowPositioned()) {
        // Legacy line clamp does not cross into the interior of an atomic inline-level box.
        layoutState->setLineClamp({ });

        m_skippedLegacyLineClampToRestore = layoutState->legacyLineClamp();
        layoutState->setLegacyLineClamp({ });
        // The box may still clamp its own content.
        if (!maximumLinesForBlockContainer)
            return;
    }

    if (maximumLinesForBlockContainer) {
        // Ignore top level legacy line clamp for now.
        if (m_blockContainer->style().overflowContinue() == OverflowContinue::WebkitLegacy)
            return;
        // New, top level line clamp.
        m_isLineClampRoot = true;
        layoutState->setLineClamp(RenderLayoutState::LineClamp { static_cast<size_t>(maximumLinesForBlockContainer->value), m_blockContainer->style().overflowContinue() == OverflowContinue::Discard });
        return;
    }

    if (m_previousLineClamp) {
        // Propagated line clamp.
        if (blockContainer.establishesIndependentFormattingContext() || blockContainer.style().display() == Style::DisplayType::RubyText) {
            // Contents of descendants that establish independent formatting contexts are skipped over while counting line boxes,
            // and a ruby annotation belongs to the line of its base: it is clamped with that line, not line by line on its own.
            layoutState->setLineClamp({ });
            return;
        }
        auto effectiveShouldDiscard = m_previousLineClamp->shouldDiscardOverflow  || m_blockContainer->style().overflowContinue() == OverflowContinue::Discard;
        layoutState->setLineClamp(RenderLayoutState::LineClamp { m_previousLineClamp->maximumLines, effectiveShouldDiscard });
        return;
    }
}

LineClampUpdater::~LineClampUpdater()
{
    auto* layoutState = m_blockContainer->view().frameView().layoutContext().layoutState();
    if (!layoutState)
        return;

    if (m_skippedLegacyLineClampToRestore)
        layoutState->setLegacyLineClamp(m_skippedLegacyLineClampToRestore);

    if (!m_previousLineClamp) {
        layoutState->setLineClamp({ });
        return;
    }

    auto lineClamp = layoutState->lineClamp();
    if (!lineClamp || m_blockContainer->establishesIndependentFormattingContext()) {
        // "Only line boxes in the same block formatting context are counted: the contents of descendants
        // that establish independent formatting contexts are skipped over while counting line boxes."
        // https://drafts.csswg.org/css-overflow-4/#max-lines
        layoutState->setLineClamp(m_previousLineClamp);
        return;
    }

    size_t lineCount = m_isLineClampRoot ? 0 : m_previousLineClamp->maximumLines - std::min(m_previousLineClamp->maximumLines, lineClamp->maximumLines);
    if (CheckedPtr blockFlow = dynamicDowncast<RenderBlockFlow>(m_blockContainer.get()); blockFlow && blockFlow->childrenInline())
        lineCount = blockFlow->lineCount();
    layoutState->setLineClamp(RenderLayoutState::LineClamp { m_previousLineClamp->maximumLines - std::min(m_previousLineClamp->maximumLines, lineCount), m_previousLineClamp->shouldDiscardOverflow });
}

void LineClampUpdater::setMaximumLines(size_t maximumLines)
{
    auto* layoutState = m_blockContainer->view().frameView().layoutContext().layoutState();
    if (!layoutState)
        return;
    m_isLineClampRoot = true;
    layoutState->setLineClamp(RenderLayoutState::LineClamp { maximumLines, m_blockContainer->style().overflowContinue() == OverflowContinue::Discard });
}

void LineClampUpdater::resetLineClamp()
{
    ASSERT(m_isLineClampRoot);
    auto* layoutState = m_blockContainer->view().frameView().layoutContext().layoutState();
    if (!layoutState)
        return;
    layoutState->setLineClamp({ });
}

std::optional<size_t> LineClampUpdater::maximumLinesForAutoClampPoint() const
{
    auto& lineClampContainer = downcast<RenderBlockFlow>(m_blockContainer.get());
    // line-clamp: auto (or just a block-ellipsis value) leaves max-lines at auto on a line-clamp container.
    CheckedRef style = lineClampContainer.style();
    if (!style->maxLines().isAuto() || style->overflowContinue() != OverflowContinue::Discard || lineClampContainer.multiColumnFlow())
        return { };
    // "the block size the box would have if its automatic block size were infinite"
    auto maximumContentHeight = lineClampContainer.constrainContentBoxLogicalHeightByMinMax(lineClampContainer.computeContentLogicalHeight(style->logicalHeight(), std::nullopt).value_or(LayoutUnit::max()), std::nullopt);
    if (maximumContentHeight == LayoutUnit::max())
        return { };
    auto blockSizeLimit = lineClampContainer.borderAndPaddingBefore() + maximumContentHeight;

    // "The auto clamp point will be set to the last possible clamp point such that, for it and all previous possible clamp points,
    // the line-clamp container's automatic block size (as determined below) is not greater than the block size the box would have
    // if its automatic block size were infinite."
    // https://drafts.csswg.org/css-overflow-4/#line-clamp-containers
    // Lines only go down in the block formatting context, so the lines that fit are the ones before the auto clamp point.
    size_t inlineLineCount = 0;
    auto hasLineAfterClampPoint = false;
    for (CheckedPtr<const RenderObject> descendant = &lineClampContainer; descendant;) {
        CheckedPtr blockFlow = dynamicDowncast<RenderBlockFlow>(*descendant);
        auto isSkippedOver = descendant != &lineClampContainer && (descendant->isFloatingOrOutOfFlowPositioned() || (blockFlow && (blockFlow->establishesIndependentFormattingContext() || blockFlow->style().display() == Style::DisplayType::RubyText)));
        if (isSkippedOver) {
            descendant = descendant->nextInPreOrderAfterChildren(&lineClampContainer);
            continue;
        }
        if (blockFlow && blockFlow->inlineLayout()) {
            // The line-clamp container's block size limit in this block's coordinates, less the border and padding (of the block and its ancestors) still following its lines.
            auto availableHeightForLines = blockSizeLimit;
            for (CheckedPtr<const RenderBlock> ancestor = blockFlow; ancestor && ancestor != &lineClampContainer; ancestor = ancestor->containingBlock())
                availableHeightForLines -= ancestor->logicalTop() + ancestor->borderAndPaddingAfter();
            auto [lineCount, hasLineAfter] = blockFlow->inlineLayout()->lineCountForHeight(availableHeightForLines);
            inlineLineCount += lineCount;
            hasLineAfterClampPoint = hasLineAfterClampPoint || hasLineAfter;
        }
        descendant = descendant->nextInPreOrder(&lineClampContainer);
    }
    if (!hasLineAfterClampPoint)
        return { };
    return inlineLineCount;
}

} // namespace WebCore
