/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBlock.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// The cases of a switch over integers that lie within 64 values of each other and lead to few places: one test of the range and one test of a bit for each place.
struct SwitchWindow {
    static constexpr int64_t numberOfValues = 64;
    AOT_CLASS_THRESHOLD(unsigned, minCases, 4);
    static constexpr unsigned maxPlaces = 3;

    struct Place {
        uint32_t firstCase;
        uint64_t values;
    };

    uint64_t allValues() const { return length == numberOfValues ? ~0ull : (1ull << length) - 1; }
    bool hasNoGap() const
    {
        uint64_t listed = 0;
        for (const Place& place : places)
            listed |= place.values;
        return listed == allValues();
    }

    int64_t first { 0 };
    int64_t length { 0 };
    Vector<Place, maxPlaces> places;
};

inline std::optional<SwitchWindow> findSwitchWindow(const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::Switch)
        return std::nullopt;
    auto cases = terminator.cases();
    if (cases.size() < SwitchWindow::minCases)
        return std::nullopt;
    int64_t least = cases[0];
    int64_t greatest = cases[0];
    for (uint32_t index = 1; index < cases.size(); ++index) {
        least = std::min<int64_t>(least, cases[index]);
        greatest = std::max<int64_t>(greatest, cases[index]);
    }
    if (least < INT32_MIN || greatest > INT32_MAX || greatest - least >= SwitchWindow::numberOfValues)
        return std::nullopt;

    SwitchWindow window { least, greatest - least + 1, { } };
    for (uint32_t index = 0; index < cases.size(); ++index) {
        uint64_t bit = 1ull << (cases[index] - window.first);
        size_t place = window.places.findIf([&](const SwitchWindow::Place& known) {
            return terminator.targets[known.firstCase] == terminator.targets[index];
        });
        if (place != notFound) {
            window.places[place].values |= bit;
            continue;
        }
        if (window.places.size() == SwitchWindow::maxPlaces)
            return std::nullopt;
        window.places.append({ index, bit });
    }
    return window;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
