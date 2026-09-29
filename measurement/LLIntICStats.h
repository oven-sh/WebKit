// TEMPORARY measurement instrumentation. Not part of the change. Compiled only with -DJSC_LLINT_IC_STATS=1.
// Enabled at run time with the environment variable LLINT_IC_STATS=1 (dump to stderr at exit) or
// LLINT_IC_STATS=<path> (append one JSON line to that file at exit).

#pragma once

#if defined(JSC_LLINT_IC_STATS)

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace JSC { namespace LLIntICStats {

enum Class : unsigned {
    OwnValue,       // own data property, cacheable
    OwnUncacheable, // own data property, structure not cacheable (dictionary etc.)
    ProtoValue,     // data property on the prototype chain, cacheable
    Absent,         // property not found
    Getter,         // JS getter
    Custom,         // custom getter / custom value
    StringLength,   // "length" on a JSString
    ArrayLength,    // "length" on a JSArray
    NonCell,        // receiver is not a cell
    Opaque,         // proxy / module namespace / other uncacheable value
    Threw,
    NumClasses
};

static constexpr const char* classNames[NumClasses] = {
    "own", "own-uncacheable", "proto", "absent", "getter", "custom", "string-length", "array-length", "non-cell", "opaque", "threw"
};

struct Site {
    uint64_t calls { 0 };
    std::array<uint64_t, NumClasses> byClass { };
    std::array<uint32_t, 8> structures { };
    unsigned numStructures { 0 }; // saturates at 9 (= more than 8)
};

struct Key {
    const void* owner;
    uint32_t index;
    bool operator==(const Key& other) const { return owner == other.owner && index == other.index; }
};
struct KeyHash {
    size_t operator()(const Key& key) const { return std::hash<const void*>()(key.owner) * 31 + key.index; }
};

struct State {
    bool enabled { false };
    const char* path { nullptr };
    std::mutex lock;
    std::unordered_map<Key, Site, KeyHash> getSites;
    std::unordered_map<Key, Site, KeyHash> putSites;
    std::atomic<uint64_t> getCalls { 0 };
    std::atomic<uint64_t> putCalls { 0 };
    std::atomic<uint64_t> baselineCompiles { 0 };
    std::atomic<uint64_t> tierUpTriggers { 0 };
    std::atomic<uint64_t> protoCacheSetups { 0 };
    std::atomic<uint64_t> unsetCacheSetups { 0 };
    std::atomic<uint64_t> watchpointClears { 0 };
};

inline void dump();

inline State& state()
{
    static State* s = [] {
        auto* result = new State;
        const char* env = getenv("LLINT_IC_STATS");
        if (env && *env && strcmp(env, "0")) {
            result->enabled = true;
            if (strcmp(env, "1"))
                result->path = strdup(env);
            atexit(dump);
        }
        return result;
    }();
    return *s;
}

inline bool enabled() { return state().enabled; }

inline void recordSite(std::unordered_map<Key, Site, KeyHash>& map, const void* owner, uint32_t index, Class klass, uint32_t structureID)
{
    State& s = state();
    std::lock_guard<std::mutex> locker(s.lock);
    Site& site = map[Key { owner, index }];
    site.calls++;
    site.byClass[klass]++;
    if (structureID && site.numStructures <= 8) {
        bool found = false;
        for (unsigned i = 0; i < std::min(site.numStructures, 8u); ++i) {
            if (site.structures[i] == structureID) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (site.numStructures < 8)
                site.structures[site.numStructures] = structureID;
            site.numStructures++;
        }
    }
}

inline void recordGet(const void* owner, uint32_t index, Class klass, uint32_t structureID)
{
    State& s = state();
    if (!s.enabled)
        return;
    s.getCalls.fetch_add(1, std::memory_order_relaxed);
    recordSite(s.getSites, owner, index, klass, structureID);
}

inline void recordPut(const void* owner, uint32_t index, uint32_t structureID)
{
    State& s = state();
    if (!s.enabled)
        return;
    s.putCalls.fetch_add(1, std::memory_order_relaxed);
    recordSite(s.putSites, owner, index, OwnValue, structureID);
}

inline void count(std::atomic<uint64_t> State::* field)
{
    State& s = state();
    if (!s.enabled)
        return;
    (s.*field).fetch_add(1, std::memory_order_relaxed);
}

inline void summarize(FILE* out, const char* name, const std::unordered_map<Key, Site, KeyHash>& sites, uint64_t totalCalls, bool json)
{
    // Buckets by calls per site: 1, 2-10, 11-100, >100.
    uint64_t bucketSites[4] = { }, bucketCalls[4] = { };
    std::array<uint64_t, NumClasses> hotByClass { };
    std::array<uint64_t, NumClasses> warmByClass { }; // sites with 11-100 calls
    std::array<uint64_t, NumClasses> allByClass { };
    uint64_t hotCalls = 0;
    uint64_t hotOwnByStructures[4] = { }; // 1, 2-4, 5-8, >8 structures
    for (auto& entry : sites) {
        const Site& site = entry.second;
        unsigned bucket = site.calls <= 1 ? 0 : site.calls <= 10 ? 1 : site.calls <= 100 ? 2 : 3;
        bucketSites[bucket]++;
        bucketCalls[bucket] += site.calls;
        for (unsigned i = 0; i < NumClasses; ++i)
            allByClass[i] += site.byClass[i];
        if (bucket == 2) {
            for (unsigned i = 0; i < NumClasses; ++i)
                warmByClass[i] += site.byClass[i];
        }
        if (bucket == 3) {
            hotCalls += site.calls;
            for (unsigned i = 0; i < NumClasses; ++i)
                hotByClass[i] += site.byClass[i];
            unsigned sb = site.numStructures <= 1 ? 0 : site.numStructures <= 4 ? 1 : site.numStructures <= 8 ? 2 : 3;
            hotOwnByStructures[sb] += site.byClass[OwnValue];
        }
    }
    if (json) {
        fprintf(out, "\"%s\":{\"calls\":%llu,\"sites\":%zu,\"sites_by_calls\":[%llu,%llu,%llu,%llu],\"calls_by_bucket\":[%llu,%llu,%llu,%llu],\"hot_calls\":%llu,\"hot_by_class\":{",
            name, (unsigned long long)totalCalls, sites.size(),
            (unsigned long long)bucketSites[0], (unsigned long long)bucketSites[1], (unsigned long long)bucketSites[2], (unsigned long long)bucketSites[3],
            (unsigned long long)bucketCalls[0], (unsigned long long)bucketCalls[1], (unsigned long long)bucketCalls[2], (unsigned long long)bucketCalls[3],
            (unsigned long long)hotCalls);
        for (unsigned i = 0; i < NumClasses; ++i)
            fprintf(out, "%s\"%s\":%llu", i ? "," : "", classNames[i], (unsigned long long)hotByClass[i]);
        fprintf(out, "},\"warm_by_class\":{");
        for (unsigned i = 0; i < NumClasses; ++i)
            fprintf(out, "%s\"%s\":%llu", i ? "," : "", classNames[i], (unsigned long long)warmByClass[i]);
        fprintf(out, "},\"all_by_class\":{");
        for (unsigned i = 0; i < NumClasses; ++i)
            fprintf(out, "%s\"%s\":%llu", i ? "," : "", classNames[i], (unsigned long long)allByClass[i]);
        fprintf(out, "},\"hot_own_by_structures\":[%llu,%llu,%llu,%llu]}",
            (unsigned long long)hotOwnByStructures[0], (unsigned long long)hotOwnByStructures[1], (unsigned long long)hotOwnByStructures[2], (unsigned long long)hotOwnByStructures[3]);
        return;
    }
    fprintf(out, "[llint-ic-stats] %s: %llu slow path calls at %zu sites\n", name, (unsigned long long)totalCalls, sites.size());
    static const char* bucketNames[4] = { "1", "2-10", "11-100", ">100" };
    for (unsigned i = 0; i < 4; ++i)
        fprintf(out, "[llint-ic-stats]   sites with %s calls: %llu sites, %llu calls (%.1f%%)\n", bucketNames[i],
            (unsigned long long)bucketSites[i], (unsigned long long)bucketCalls[i], totalCalls ? 100.0 * bucketCalls[i] / totalCalls : 0.0);
    fprintf(out, "[llint-ic-stats]   classes at sites with >100 calls (share of %llu calls):\n", (unsigned long long)hotCalls);
    for (unsigned i = 0; i < NumClasses; ++i) {
        if (hotByClass[i])
            fprintf(out, "[llint-ic-stats]     %-16s %10llu  %5.1f%%\n", classNames[i], (unsigned long long)hotByClass[i], hotCalls ? 100.0 * hotByClass[i] / hotCalls : 0.0);
    }
    fprintf(out, "[llint-ic-stats]   classes at sites with 11-100 calls (share of %llu calls):\n", (unsigned long long)bucketCalls[2]);
    for (unsigned i = 0; i < NumClasses; ++i) {
        if (warmByClass[i])
            fprintf(out, "[llint-ic-stats]     %-16s %10llu  %5.1f%%\n", classNames[i], (unsigned long long)warmByClass[i], bucketCalls[2] ? 100.0 * warmByClass[i] / bucketCalls[2] : 0.0);
    }
    fprintf(out, "[llint-ic-stats]   own-value calls at hot sites by structures seen at the site (1 / 2-4 / 5-8 / >8): %llu / %llu / %llu / %llu\n",
        (unsigned long long)hotOwnByStructures[0], (unsigned long long)hotOwnByStructures[1], (unsigned long long)hotOwnByStructures[2], (unsigned long long)hotOwnByStructures[3]);
}

inline void dump()
{
    State& s = state();
    std::lock_guard<std::mutex> locker(s.lock);
    FILE* out = stderr;
    bool json = false;
    if (s.path) {
        out = fopen(s.path, "a");
        if (!out)
            out = stderr;
        else
            json = true;
    }
    if (json) {
        fprintf(out, "{");
        summarize(out, "get_by_id", s.getSites, s.getCalls.load(), true);
        fprintf(out, ",");
        summarize(out, "put_by_id", s.putSites, s.putCalls.load(), true);
        fprintf(out, ",\"baseline_compiles\":%llu,\"tier_up_triggers\":%llu,\"proto_cache_setups\":%llu,\"unset_cache_setups\":%llu,\"watchpoint_clears\":%llu}\n",
            (unsigned long long)s.baselineCompiles.load(), (unsigned long long)s.tierUpTriggers.load(), (unsigned long long)s.protoCacheSetups.load(),
            (unsigned long long)s.unsetCacheSetups.load(), (unsigned long long)s.watchpointClears.load());
        fclose(out);
        return;
    }
    summarize(out, "get_by_id", s.getSites, s.getCalls.load(), false);
    summarize(out, "put_by_id", s.putSites, s.putCalls.load(), false);
    fprintf(out, "[llint-ic-stats] baseline compiles: %llu, tier-up triggers from misses: %llu, proto cache setups: %llu, unset cache setups: %llu, watchpoint clears: %llu\n",
        (unsigned long long)s.baselineCompiles.load(), (unsigned long long)s.tierUpTriggers.load(), (unsigned long long)s.protoCacheSetups.load(),
        (unsigned long long)s.unsetCacheSetups.load(), (unsigned long long)s.watchpointClears.load());
}

} } // namespace JSC::LLIntICStats

#define LLINT_IC_STATS(...) __VA_ARGS__

#else

#define LLINT_IC_STATS(...)

#endif
