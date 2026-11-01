// Cost of JSC::canonicalizeLocaleIDWithoutNullTerminator() per call: the function as it was (uloc_canonicalize alone) against
// the function as it is (with replaceLocaleAliases), in user-space instructions and in allocations.
#include "config.h"

#include "InitializeThreading.h"
#include "IntlObject.h"
#include <cstdio>
#include <cstring>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unicode/uclean.h>
#include <unicode/uloc.h>
#include <unistd.h>
#include <wtf/Vector.h>
#include <wtf/unicode/icu/ICUHelpers.h>

static uint64_t s_icuAllocations;
static uint64_t s_engineAllocations;
static int64_t s_icuLiveBytes;

static void* U_CALLCONV icuAllocate(const void*, size_t size)
{
    ++s_icuAllocations;
    size_t* block = static_cast<size_t*>(malloc(size + 16));
    block[0] = size;
    s_icuLiveBytes += size;
    return block + 2;
}
static void U_CALLCONV icuFree(const void*, void* pointer)
{
    if (!pointer)
        return;
    size_t* block = static_cast<size_t*>(pointer) - 2;
    s_icuLiveBytes -= block[0];
    free(block);
}
static void* U_CALLCONV icuReallocate(const void* context, void* pointer, size_t size)
{
    if (!pointer)
        return icuAllocate(context, size);
    size_t* block = static_cast<size_t*>(pointer) - 2;
    size_t old = block[0];
    void* replacement = icuAllocate(context, size);
    memcpy(replacement, pointer, old < size ? old : size);
    icuFree(context, pointer);
    return replacement;
}

// The link wraps the entry points of mimalloc that WTF::fastMalloc() and its relatives call.
extern "C" {
void* __real_mi_malloc(size_t);
void* __real_mi_zalloc(size_t);
void* __real_mi_realloc(void*, size_t);
void* __real_mi_malloc_aligned(size_t, size_t);
void* __real_mi_zalloc_aligned(size_t, size_t);
size_t mi_usable_size(const void*);
void __real_mi_free(void*);
static int64_t s_engineLiveBytes;
static bool s_trace;
static void* counted(void* pointer) { ++s_engineAllocations; s_engineLiveBytes += mi_usable_size(pointer); if (s_trace) fprintf(stderr, "  alloc %zu\n", mi_usable_size(pointer)); return pointer; }
void* __wrap_mi_malloc(size_t size) { return counted(__real_mi_malloc(size)); }
void* __wrap_mi_zalloc(size_t size) { return counted(__real_mi_zalloc(size)); }
void* __wrap_mi_realloc(void* pointer, size_t size) { s_engineLiveBytes -= pointer ? mi_usable_size(pointer) : 0; return counted(__real_mi_realloc(pointer, size)); }
void* __wrap_mi_malloc_aligned(size_t size, size_t alignment) { return counted(__real_mi_malloc_aligned(size, alignment)); }
void* __wrap_mi_zalloc_aligned(size_t size, size_t alignment) { return counted(__real_mi_zalloc_aligned(size, alignment)); }
void __wrap_mi_free(void* pointer) { s_engineLiveBytes -= pointer ? mi_usable_size(pointer) : 0; if (s_trace && pointer) fprintf(stderr, "  free %zu\n", mi_usable_size(pointer)); __real_mi_free(pointer); }
}

static int s_counter;
static void counterOpen()
{
    perf_event_attr attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.type = PERF_TYPE_HARDWARE;
    attributes.size = sizeof(attributes);
    attributes.config = PERF_COUNT_HW_INSTRUCTIONS;
    attributes.disabled = 1;
    attributes.exclude_kernel = 1;
    attributes.exclude_hv = 1;
    s_counter = static_cast<int>(syscall(SYS_perf_event_open, &attributes, 0, -1, -1, 0));
    if (s_counter < 0) {
        perror("perf_event_open");
        exit(1);
    }
}
static void counterStart()
{
    ioctl(s_counter, PERF_EVENT_IOC_RESET, 0);
    ioctl(s_counter, PERF_EVENT_IOC_ENABLE, 0);
}
static uint64_t counterStop()
{
    ioctl(s_counter, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t count = 0;
    if (read(s_counter, &count, sizeof(count)) != sizeof(count))
        exit(1);
    return count;
}

// The function before the change.
static NEVER_INLINE std::optional<Vector<char, 32>> before(const char* localeID)
{
    Vector<char, 32> buffer;
    auto status = callBufferProducingFunction(uloc_canonicalize, localeID, buffer);
    if (U_FAILURE(status))
        return std::nullopt;
    return buffer;
}

static NEVER_INLINE std::optional<Vector<char, 32>> after(const char* localeID)
{
    return JSC::canonicalizeLocaleIDWithoutNullTerminator(localeID);
}

int main(int argc, char** argv)
{
    UErrorCode status = U_ZERO_ERROR;
    u_setMemoryFunctions(nullptr, icuAllocate, icuReallocate, icuFree, &status);
    WTF::initialize();
    JSC::initialize();
    counterOpen();
    s_trace = !!getenv("TRACE");
    bool isAfter = argc > 1 && !strcmp(argv[1], "after");
    long iterations = argc > 2 ? atol(argv[2]) : 20000;
    auto function = isAfter ? after : before;
    for (int index = 3; index < argc; ++index) {
        const char* localeID = argv[index];
        uint64_t icu = s_icuAllocations, engine = s_engineAllocations;
        int64_t live = s_icuLiveBytes;
        int64_t engineLive = s_engineLiveBytes;
        counterStart();
        auto first = function(localeID);
        uint64_t firstInstructions = counterStop();
        uint64_t firstICU = s_icuAllocations - icu, firstEngine = s_engineAllocations - engine;
        int64_t firstLive = s_icuLiveBytes - live;
        int64_t firstEngineLive = s_engineLiveBytes - engineLive;
        icu = s_icuAllocations;
        engine = s_engineAllocations;
        size_t total = 0;
        counterStart();
        for (long iteration = 0; iteration < iterations; ++iteration) {
            auto result = function(localeID);
            total += result ? result->size() : 0;
        }
        uint64_t instructions = counterStop();
        char text[160] = "<fails>";
        if (first)
            snprintf(text, sizeof(text), "%.*s", static_cast<int>(first->size()), first->span().data());
        printf("%-6s %-40s -> %-40s | %8.1f instr/call  %5.2f ICU allocs  %5.2f engine allocs | first call: %9llu instr, %5llu ICU allocs (%lld bytes live), %llu engine allocs (%lld bytes live)\n",
            argv[1], localeID, text, static_cast<double>(instructions) / iterations, static_cast<double>(s_icuAllocations - icu) / iterations,
            static_cast<double>(s_engineAllocations - engine) / iterations, static_cast<unsigned long long>(firstInstructions),
            static_cast<unsigned long long>(firstICU), static_cast<long long>(firstLive), static_cast<unsigned long long>(firstEngine), static_cast<long long>(firstEngineLive));
        (void)total;
    }
    return 0;
}
