/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirFrameLayout.h"

#if ENABLE(B3_JIT)

#include <wtf/CommaPrinter.h>

namespace JSC { namespace B3 { namespace Air {

void FrameLayout::dump(PrintStream& out) const
{
    if (!isPlanned) {
        out.print("upstream's");
        return;
    }

    CommaPrinter comma;
    switch (coverage) {
    case FrameCoverage::NoPath:
        out.print(comma, "NoPath");
        break;
    case FrameCoverage::SomePaths:
        out.print(comma, "SomePaths");
        break;
    case FrameCoverage::EveryPathNeedsFrame:
        out.print(comma, "EveryPathNeedsFrame");
        break;
    case FrameCoverage::EveryPathBecauseShrinkWrappingIsOff:
        out.print(comma, "EveryPathBecauseShrinkWrappingIsOff");
        break;
    case FrameCoverage::EveryPathBecauseOfSeveralEntrypoints:
        out.print(comma, "EveryPathBecauseOfSeveralEntrypoints");
        break;
    case FrameCoverage::EveryPathBecauseFrameIsLarge:
        out.print(comma, "EveryPathBecauseFrameIsLarge");
        break;
    }

    if (hasFrame && !hasFrameRecord)
        out.print(comma, "no frame record");

    if (slotArea == SlotArea::AboveFrameRecord)
        out.print(comma, "slots above frame record: ", bytesAboveFrameRecord, " bytes");

    switch (calleeSaveForm) {
    case CalleeSaveForm::Stored:
        break;
    case CalleeSaveForm::PushedAtFrameBottom:
        out.print(comma, "PushedAtFrameBottom");
        break;
    case CalleeSaveForm::PushedBelowFrameRecord:
        out.print(comma, "PushedBelowFrameRecord");
        break;
    }

    if (numberOfSavesClobberedAtSuspensions)
        out.print(comma, "saves clobbered at suspensions: ", numberOfSavesClobberedAtSuspensions);
    if (numberOfFrameSetups)
        out.print(comma, "setups: ", numberOfFrameSetups);
    if (numberOfFrameTeardowns)
        out.print(comma, "teardowns: ", numberOfFrameTeardowns);
    if (numberOfSpillsPushedAtFrameSetup)
        out.print(comma, "spills pushed at setup: ", numberOfSpillsPushedAtFrameSetup);
    if (numberOfReloadsPoppedAtFrameTeardown)
        out.print(comma, "reloads popped at teardown: ", numberOfReloadsPoppedAtFrameTeardown);
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
