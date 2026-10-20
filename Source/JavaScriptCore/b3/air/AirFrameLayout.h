/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include <wtf/PrintStream.h>

namespace JSC { namespace B3 { namespace Air {

enum class SlotArea : uint8_t {
    BelowFrameRecord,
    AboveFrameRecord,
};

enum class CalleeSaveForm : uint8_t {
    Stored,
    PushedAtFrameBottom,
    PushedBelowFrameRecord,
};

enum class FrameCoverage : uint8_t {
    NoPath,
    SomePaths,
    EveryPathNeedsFrame,
    EveryPathBecauseShrinkWrappingIsOff,
    EveryPathBecauseOfSeveralEntrypoints,
    EveryPathBecauseFrameIsLarge,
};

struct FrameLayout {
    bool isPlanned { false };
    bool hasFrame { true };
    FrameCoverage coverage { FrameCoverage::EveryPathBecauseShrinkWrappingIsOff };
    SlotArea slotArea { SlotArea::BelowFrameRecord };
    CalleeSaveForm calleeSaveForm { CalleeSaveForm::Stored };
    unsigned bytesAboveFrameRecord { 0 };
    unsigned numberOfSavesClobberedAtSuspensions { 0 };
    unsigned numberOfFrameSetups { 0 };
    unsigned numberOfFrameTeardowns { 0 };
    unsigned numberOfSpillsPushedAtFrameSetup { 0 };

    JS_EXPORT_PRIVATE void dump(PrintStream&) const;
};

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
