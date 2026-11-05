/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class Value;

namespace Air {

struct StackColoring {
    static constexpr unsigned bytesPerUnit = 4;
    static constexpr unsigned bytesPerWord = 8;
    static constexpr unsigned unitsPerWord = bytesPerWord / bytesPerUnit;

    struct UnitRange {
        unsigned first { 0 };
        unsigned last { 0 };
    };

    unsigned numberOfWords() const { return (numberOfUnits + unitsPerWord - 1) / unitsPerWord; }

    Vector<std::optional<UnitRange>> unitsOfSlot;
    Vector<unsigned> useCountOfSlot;
    bool hasSuspensions { false };
    BitVector spillSlotsLiveAcrossSuspensions;
    // By the origin of the suspension. It is empty where the slots are too many for liveness.
    UncheckedKeyHashMap<Value*, BitVector> spillSlotsLiveAcrossEachSuspension;
    unsigned numberOfUnits { 0 };
};

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
