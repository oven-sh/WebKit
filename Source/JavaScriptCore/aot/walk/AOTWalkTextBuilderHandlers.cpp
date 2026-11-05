/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class HandlerOfTextInBuilder : uint8_t {
    LetsTextGo,
    TakesString,
    GoesOnInBuilder,
};

} // anonymous namespace

static const NameOfGrowingText* findNameInTextBuilderHandlers(const GrowingText& web, ValueID value)
{
    auto found = std::ranges::lower_bound(web.names, value, { }, &NameOfGrowingText::value);
    return found != web.names.end() && found->value == value ? &*found : nullptr;
}

static HandlerOfTextInBuilder classifyHandlerInTextBuilderHandlers(const BodyIndex& index, const GrowingText& web, BodyTarget handler)
{
    if (index.isInLoop(handler.block(), index.loopOf(web.loopHead)))
        return HandlerOfTextInBuilder::GoesOnInBuilder;
    for (ValueID argument : handler.arguments()) {
        if (findNameInTextBuilderHandlers(web, argument))
            return HandlerOfTextInBuilder::TakesString;
    }
    return HandlerOfTextInBuilder::LetsTextGo;
}

bool everyHandlerLetsTextGo(Walker& walker, const GrowingText& web)
{
    const BodyIndex& index = walker.currentBodyIndex();
    for (BlockID member : index.membersOf(index.loopOf(web.loopHead))) {
        BodyBlock block = index.block(member);
        if (!block.hasHandler())
            continue;
        BodyTarget handler = block.handler();
        if (classifyHandlerInTextBuilderHandlers(index, web, handler) != HandlerOfTextInBuilder::LetsTextGo)
            return false;
        for (const NameOfGrowingText& name : web.names) {
            if (walker.isLiveAtHeadOf(name.value, handler.block()))
                return false;
        }
    }
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
