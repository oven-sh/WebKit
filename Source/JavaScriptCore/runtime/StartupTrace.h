#pragma once

#include <wtf/DataLog.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Vector.h>

namespace JSC {

// Usable inside member-initializer lists: returns 0, side-effect is a timed log.
// Usage: , someField((lap("label"), realInit))
inline int lap(const char* label)
{
    static thread_local WTF::MonotonicTime last;
    if (!getenv("BUN_startupTrace"))
        return 0;
    auto now = WTF::MonotonicTime::now();
    if (last)
        dataLogLn("    lap[", label, "] ", (now - last).microseconds(), "us");
    else
        dataLogLn("    lap[", label, "] (start)");
    last = now;
    return 0;
}


struct StartupTrace {
    struct Entry { const char* label; double us; };
    WTF::Vector<Entry, 64> entries;
    WTF::MonotonicTime t0;
    WTF::MonotonicTime last;
    const char* name;

    explicit StartupTrace(const char* n)
        : t0(WTF::MonotonicTime::now()), last(t0), name(n) { }

    void mark(const char* label)
    {
        auto now = WTF::MonotonicTime::now();
        entries.append({ label, (now - last).microseconds() });
        last = now;
    }

    ~StartupTrace()
    {
        if (!getenv("BUN_startupTrace"))
            return;
        auto total = (last - t0).microseconds();
        dataLogLn("[", name, "] total ", total, "us");
        for (auto& e : entries)
            dataLogLn("  ", e.us, "us  ", e.label, "  (", (e.us * 100.0 / total), "%)");
    }
};

} // namespace JSC
