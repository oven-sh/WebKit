/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <memory>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {

// One time zone as ICU has it: its UTC offsets, its transitions and its display names.
//
// ICU's C API has no time zone object. A UCalendar is the only thing there that holds a zone, and the ICU that Bun
// bundles loads its likely-subtags data to open a calendar of any locale (about 18 million instructions, once per
// process). So where Bun links that ICU this is an icu::TimeZone, which loads nothing but the zone. Everywhere else
// it is a UCalendar: on Darwin Bun links the system's ICU, of which only the C API is stable.
//
// No ICU type is named here, so that only ICUTimeZone.cpp sees ICU's C++ headers.
class ICUTimeZone {
    WTF_MAKE_TZONE_ALLOCATED(ICUTimeZone);
    WTF_MAKE_NONCOPYABLE(ICUTimeZone);
public:
    // Milliseconds. The UTC offset of the zone at a time is the sum of the two.
    struct Offsets {
        int32_t raw { 0 };
        int32_t dst { 0 };
    };

    // Which offset a local time gets when a transition skips it or repeats it: the one before the transition, or the
    // one after it (UCAL_TZ_LOCAL_FORMER and UCAL_TZ_LOCAL_LATTER, for both cases).
    enum class LocalTime : bool { Former, Latter };
    enum class Transition : uint8_t { Next, Previous, PreviousInclusive };
    enum class DisplayName : bool { Standard, Daylight };

    // `icuIdentifier` is what TimeZone::toICUString() returns. As with ucal_open, an identifier that ICU does not have
    // is GMT. Null only when ICU cannot make the zone at all.
    static std::unique_ptr<ICUTimeZone> create(StringView icuIdentifier);
    ~ICUTimeZone();

    // A query changes the UCalendar when that is what holds the zone, so a zone that threads share needs a lock around
    // its queries. All of them return nothing when ICU refuses the time (NaN).
    std::optional<Offsets> offsetAt(double epochMilliseconds);
    std::optional<Offsets> offsetFromLocal(double localMilliseconds, LocalTime);
    // The inner value is empty when the zone has no transition on that side of the time.
    std::optional<std::optional<double>> transition(double epochMilliseconds, Transition);
    // The long name in `locale`. A null String when ICU fails.
    String displayName(DisplayName, const char* locale);

private:
    explicit ICUTimeZone(void* zone)
        : m_zone(zone)
    {
    }

    void* m_zone;
};

} // namespace JSC
