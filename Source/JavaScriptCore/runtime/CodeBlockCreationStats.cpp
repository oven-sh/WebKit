/*
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "CodeBlockCreationStats.h"

#include "CodeBlock.h"
#include "FunctionExecutable.h"
#include "MetadataTable.h"
#include "Options.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include <algorithm>
#include <cstdlib>
#include <mutex>
#if OS(UNIX)
#include <time.h>
#include <unistd.h>
#endif
#include <wtf/DataLog.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StringPrintStream.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>

namespace JSC {
namespace CodeBlockCreationStats {

bool g_enabled = false;

namespace {

struct State {
    Lock lock;
    std::array<uint64_t, numberOfBuckets> ticks { };
    std::array<uint64_t, numberOfBuckets> counts { };

    Vector<std::unique_ptr<DecodeRecord>> decodes;
    UncheckedKeyHashMap<UnlinkedCodeBlock*, DecodeRecord*> liveDecodes;
    Vector<std::unique_ptr<LinkRecord>> links;
    UncheckedKeyHashMap<CodeBlock*, LinkRecord*> liveLinks;

    UncheckedKeyHashSet<UnlinkedFunctionExecutable*> unlinkedChildren; // decoded from cache, never link()ed yet
    UncheckedKeyHashSet<FunctionExecutable*> instantiatedExecutables; // got at least one JSFunction
    uint64_t functionsInstantiated { 0 };
    uint64_t childrenDecoded { 0 };
    uint64_t childrenDecodedWithName { 0 };
    uint64_t childrenDecodedWithTDZ { 0 };
    uint64_t childrenDecodedWithRareData { 0 };
    uint64_t childrenLinked { 0 };
    uint64_t executablesLinkedTotal { 0 };

    DecodeRecord* currentDecode { nullptr };

    uint64_t tsc0 { 0 };
    MonotonicTime mono0;
    MonotonicTime lastDump;
    uint64_t linksAtLastDump { 0 };
    unsigned dumps { 0 };
    unsigned intervalMs { 0 };
};

State& state()
{
    static LazyNeverDestroyed<State> s;
    static std::once_flag once;
    std::call_once(once, [] { s.construct(); });
    return s.get();
}

double nsPerTick()
{
    State& s = state();
    MonotonicTime t = MonotonicTime::now();
    uint64_t c = now();
    double ns = (t - s.mono0).nanoseconds();
    double dt = static_cast<double>(c - s.tsc0);
    if (dt <= 0 || ns <= 0)
        return 1;
    return ns / dt;
}

const char* bucketDescription(unsigned i)
{
    static const char* const descriptions[] = {
#define BUCKET_DESC(name, desc) desc,
        FOR_EACH_CODEBLOCK_CREATION_BUCKET(BUCKET_DESC)
#undef BUCKET_DESC
    };
    return descriptions[i];
}

const char* bucketName(unsigned i)
{
    static const char* const names[] = {
#define BUCKET_NAME(name, desc) #name,
        FOR_EACH_CODEBLOCK_CREATION_BUCKET(BUCKET_NAME)
#undef BUCKET_NAME
    };
    return names[i];
}

void exitHook()
{
    if (!g_enabled)
        return;
    State& s = state();
    if (s.dumps && s.links.size() == s.linksAtLastDump)
        return;
    dump("process-exit");
}

} // anonymous namespace

uint64_t nowSlow()
{
    return static_cast<uint64_t>(MonotonicTime::now().secondsSinceEpoch().nanoseconds());
}

void initialize()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (!Options::reportCodeBlockCreationCosts())
            return;
        State& s = state();
        s.tsc0 = now();
        s.mono0 = MonotonicTime::now();
        s.lastDump = s.mono0;
        s.intervalMs = Options::reportCodeBlockCreationCostsIntervalMs();
        g_enabled = true;
        std::atexit(exitHook);
#if OS(LINUX)
        at_quick_exit(exitHook);
#endif
    });
}

void add(Bucket bucket, uint64_t t, uint64_t count)
{
    State& s = state();
    // Main-thread dominated; tolerate rare races from compiler threads rather than paying for atomics.
    s.ticks[static_cast<unsigned>(bucket)] += t;
    s.counts[static_cast<unsigned>(bucket)] += count;
}

DecodeRecord* currentDecode() { return state().currentDecode; }
void setCurrentDecode(DecodeRecord* r) { state().currentDecode = r; }

DecodeRecord* beginDecode()
{
    State& s = state();
    auto record = makeUnique<DecodeRecord>();
    DecodeRecord* result = record.get();
    Locker locker { s.lock };
    s.decodes.append(WTF::move(record));
    return result;
}

void endDecode(DecodeRecord* record, DecodeRecord* previous, UnlinkedCodeBlock* codeBlock)
{
    State& s = state();
    s.currentDecode = previous;
    record->codeBlock = codeBlock;
    if (codeBlock) {
        Locker locker { s.lock };
        s.liveDecodes.set(codeBlock, record);
    } else
        record->dead = true;
}

void noteIdentifierTableCreated(UnlinkedCodeBlock* codeBlock, unsigned count)
{
    codeBlock->allocateIdentifierTouchBitsForStats(count);
}

void noteChildExecutableDecoded(UnlinkedFunctionExecutable* executable, bool hasName, bool hasTDZ, bool hasRareData)
{
    State& s = state();
    Locker locker { s.lock };
    s.childrenDecoded++;
    s.childrenDecodedWithName += hasName;
    s.childrenDecodedWithTDZ += hasTDZ;
    s.childrenDecodedWithRareData += hasRareData;
    s.unlinkedChildren.add(executable);
    if (DecodeRecord* r = s.currentDecode) {
        r->children++;
        r->childrenWithName += hasName;
        r->childrenWithTDZ += hasTDZ;
    }
}

void noteExecutableLinked(UnlinkedFunctionExecutable* executable)
{
    State& s = state();
    Locker locker { s.lock };
    s.executablesLinkedTotal++;
    if (s.unlinkedChildren.remove(executable))
        s.childrenLinked++;
}

void noteFunctionInstantiated(FunctionExecutable* executable)
{
    State& s = state();
    Locker locker { s.lock };
    s.functionsInstantiated++;
    s.instantiatedExecutables.add(executable);
}

void noteUnlinkedCodeBlockDestroyed(UnlinkedCodeBlock* codeBlock)
{
    State& s = state();
    Locker locker { s.lock };
    auto it = s.liveDecodes.find(codeBlock);
    if (it == s.liveDecodes.end())
        return;
    DecodeRecord* r = it->value;
    r->identifiersTouched = codeBlock->countTouchedIdentifiersForStats();
    r->dead = true;
    s.liveDecodes.remove(it);
}

LinkRecord* beginLink(CodeBlock* codeBlock, UnlinkedCodeBlock* unlinkedCodeBlock)
{
    State& s = state();
    auto record = makeUnique<LinkRecord>();
    LinkRecord* result = record.get();
    result->codeBlock = codeBlock;
    result->unlinkedCodeBlock = unlinkedCodeBlock;
    result->initialExecuteCount = unlinkedCodeBlock->llintExecuteCounter().count();
    result->codeType = static_cast<uint8_t>(unlinkedCodeBlock->codeType());
    result->instructionBytes = unlinkedCodeBlock->instructions().sizeInBytes();
    result->identifiers = unlinkedCodeBlock->numberOfIdentifiers();
    result->children = unlinkedCodeBlock->numberOfFunctionDecls() + unlinkedCodeBlock->numberOfFunctionExprs();
    result->constants = unlinkedCodeBlock->constantRegisters().size();
    result->handlers = unlinkedCodeBlock->numberOfExceptionHandlers();
    Locker locker { s.lock };
    s.links.append(WTF::move(record));
    s.liveLinks.set(codeBlock, result);
    if (auto it = s.liveDecodes.find(unlinkedCodeBlock); it != s.liveDecodes.end())
        it->value->everLinked = true;
    return result;
}

void endLink(LinkRecord* record)
{
    if (CodeBlock* codeBlock = record->codeBlock) {
        if (auto* metadata = codeBlock->metadataTable())
            record->metadataBytes = metadata->sizeInBytesForGC();
    }
    maybePeriodicDump();
}

void noteCodeBlockDestroyed(CodeBlock* codeBlock)
{
    State& s = state();
    Locker locker { s.lock };
    auto it = s.liveLinks.find(codeBlock);
    if (it == s.liveLinks.end())
        return;
    LinkRecord* r = it->value;
    r->finalExecuteCount = r->unlinkedCodeBlock->llintExecuteCounter().count();
    r->finalJITType = static_cast<uint8_t>(codeBlock->jitType());
    r->dead = true;
    s.liveLinks.remove(it);
}

void maybePeriodicDump()
{
    State& s = state();
    if (!s.intervalMs)
        return;
    MonotonicTime t = MonotonicTime::now();
    if ((t - s.lastDump).milliseconds() < s.intervalMs)
        return;
    dump("periodic");
}

static const char* jitTypeName(uint8_t t)
{
    switch (static_cast<JITType>(t)) {
    case JITType::None: return "none";
    case JITType::HostCallThunk: return "host";
    case JITType::InterpreterThunk: return "llint";
    case JITType::BaselineJIT: return "baseline";
    case JITType::DFGJIT: return "dfg";
    case JITType::FTLJIT: return "ftl";
    }
    return "?";
}

static const char* codeTypeName(uint8_t t)
{
    switch (static_cast<CodeType>(t)) {
    case GlobalCode: return "global";
    case EvalCode: return "eval";
    case FunctionCode: return "function";
    case ModuleCode: return "module";
    }
    return "?";
}

void dump(const char* reason)
{
    if (!g_enabled)
        return;
    State& s = state();
    double nsTick = nsPerTick();
    auto ms = [&](uint64_t t) { return static_cast<double>(t) * nsTick / 1e6; };

    Locker locker { s.lock };
    s.dumps++;
    s.lastDump = MonotonicTime::now();
    s.linksAtLastDump = s.links.size();

    // Refresh live records.
    for (auto& entry : s.liveLinks) {
        LinkRecord* r = entry.value;
        r->finalExecuteCount = r->unlinkedCodeBlock->llintExecuteCounter().count();
        r->finalJITType = static_cast<uint8_t>(entry.key->jitType());
    }
    for (auto& entry : s.liveDecodes)
        entry.value->identifiersTouched = entry.key->countTouchedIdentifiersForStats();

    StringPrintStream out;
    double threadCPUMs = 0, processCPUMs = 0;
    int pid = 0;
#if OS(UNIX)
    {
        struct timespec ts;
        if (!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts))
            threadCPUMs = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
        if (!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts))
            processCPUMs = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
    }
    pid = getpid();
#endif
    out.printf("\n=== CodeBlock creation cost report (%s) pid=%d dump#%u uptime(since first VM)=%.1fms dumping-thread-cpu=%.1fms process-cpu=%.1fms ns/tick=%.4f ===\n", reason, pid, s.dumps, (MonotonicTime::now() - s.mono0).milliseconds(), threadCPUMs, processCPUMs, nsTick);

    // ---- Totals ----
    out.printf("\n-- Totals (bucket, count, ms) --\n");
    for (unsigned i = 0; i < numberOfBuckets; ++i)
        out.printf("  %-30s %10llu %10.3f ms   %s\n", bucketName(i), static_cast<unsigned long long>(s.counts[i]), ms(s.ticks[i]), bucketDescription(i));
    double decodeTotalMs = ms(s.ticks[static_cast<unsigned>(Bucket::DecodeTotal)]);
    double linkTotalMs = ms(s.ticks[static_cast<unsigned>(Bucket::LinkTotal)]);
    // Lazy function-code-block decodes nest inside nothing else; root (program/module) decodes include eager children only.
    out.printf("  decode total %.3f ms + link total %.3f ms = %.3f ms creation cost\n", decodeTotalMs, linkTotalMs, decodeTotalMs + linkTotalMs);

    // ---- Decode summary ----
    {
        uint64_t blocks = s.decodes.size(), functionBlocks = 0, identifiers = 0, identifiersTouched = 0, identifiersInDead = 0, deadBlocks = 0, children = 0, withName = 0, withTDZ = 0, constants = 0, symbolTables = 0, symbolTableEntries = 0, stringConstants = 0, stringConstantCellsCreated = 0, neverLinked = 0, instructionBytes = 0;
        uint64_t identifiersNeverLinkedBlocks = 0;
        for (auto& d : s.decodes) {
            functionBlocks += d->isFunctionCode;
            identifiers += d->identifiers;
            identifiersTouched += d->identifiersTouched;
            if (d->dead) {
                deadBlocks++;
                identifiersInDead += d->identifiers;
            }
            children += d->children;
            withName += d->childrenWithName;
            withTDZ += d->childrenWithTDZ;
            constants += d->constants;
            symbolTables += d->symbolTableConstants;
            symbolTableEntries += d->symbolTableEntries;
            stringConstants += d->stringConstants;
            stringConstantCellsCreated += d->stringConstantCellsCreated;
            instructionBytes += d->instructionBytes;
            if (!d->everLinked) {
                neverLinked++;
                identifiersNeverLinkedBlocks += d->identifiers;
            }
        }
        out.printf("\n-- Bytecode-cache decode summary --\n");
        out.printf("  UnlinkedCodeBlocks decoded: %llu (function: %llu, other: %llu); destroyed before dump: %llu; never got a CodeBlock: %llu\n", (unsigned long long)blocks, (unsigned long long)functionBlocks, (unsigned long long)(blocks - functionBlocks), (unsigned long long)deadBlocks, (unsigned long long)neverLinked);
        out.printf("  instruction bytes decoded: %llu\n", (unsigned long long)instructionBytes);
        out.printf("  identifiers decoded: %llu; touched via identifier(i): %llu (%.1f%%); untouched: %llu (%.1f%%)  [identifier slots 8 B each => %.2f MB untouched slots]\n",
            (unsigned long long)identifiers, (unsigned long long)identifiersTouched, identifiers ? 100.0 * identifiersTouched / identifiers : 0.0,
            (unsigned long long)(identifiers - identifiersTouched), identifiers ? 100.0 * (identifiers - identifiersTouched) / identifiers : 0.0,
            (identifiers - identifiersTouched) * 8.0 / (1024 * 1024));
        out.printf("  constants decoded: %llu (SymbolTable constants: %llu with %llu entries; string constants: %llu, of which %llu created a shared-table cell)\n", (unsigned long long)constants, (unsigned long long)symbolTables, (unsigned long long)symbolTableEntries, (unsigned long long)stringConstants, (unsigned long long)stringConstantCellsCreated);
        out.printf("  child UnlinkedFunctionExecutables decoded eagerly: %llu (with name: %llu, with TDZ vars: %llu, with rare data: %llu)\n",
            (unsigned long long)s.childrenDecoded, (unsigned long long)s.childrenDecodedWithName, (unsigned long long)s.childrenDecodedWithTDZ, (unsigned long long)s.childrenDecodedWithRareData);
        uint64_t wasted = s.childrenDecoded - s.childrenLinked;
        out.printf("  ... of which later link()ed into a FunctionExecutable: %llu (%.1f%%); NEVER linked (wasted eager creation): %llu (%.1f%%) => %.2f MB of UnlinkedFunctionExecutable cells (sizeof=%zu)\n",
            (unsigned long long)s.childrenLinked, s.childrenDecoded ? 100.0 * s.childrenLinked / s.childrenDecoded : 0.0,
            (unsigned long long)wasted, s.childrenDecoded ? 100.0 * wasted / s.childrenDecoded : 0.0,
            wasted * static_cast<double>(sizeof(UnlinkedFunctionExecutable)) / (1024 * 1024), sizeof(UnlinkedFunctionExecutable));
        out.printf("  FunctionExecutables created via link() total: %llu\n", (unsigned long long)s.executablesLinkedTotal);
        double childMs = ms(s.ticks[static_cast<unsigned>(Bucket::DecodeChildren)]);
        out.printf("  eager child decode cost: %.3f ms total; pro-rata never-linked share: %.3f ms\n", childMs, s.childrenDecoded ? childMs * wasted / s.childrenDecoded : 0.0);
        double identMs = ms(s.ticks[static_cast<unsigned>(Bucket::DecodeIdentifiers)]);
        out.printf("  identifier decode cost: %.3f ms total; pro-rata untouched share: %.3f ms\n", identMs, identifiers ? identMs * (identifiers - identifiersTouched) / identifiers : 0.0);
    }

    // ---- Link summary + histogram ----
    struct Bin {
        const char* label;
        uint64_t count { 0 };
        uint64_t linkTicks { 0 };
        uint64_t decodeTicks { 0 };
        uint64_t metadataBytes { 0 };
        uint64_t instructionBytes { 0 };
        uint64_t identifiers { 0 };
        uint64_t children { 0 };
    };
    // Execution delta = LLInt counter delta since creation (prologue +5, epilogue +10 => one call = 15; loop back-edge +1) unless tiered up.
    Bin bins[] = {
        { "delta==0 (created, prologue never counted)" },
        { "0<delta<=15 (1 call, no loop iterations)" },
        { "15<delta<=30 (2 calls, or 1 call + <=15 loop iters)" },
        { "30<delta<=150 (~3-10 calls)" },
        { "150<delta<=1500 (~10-100 calls)" },
        { "delta>1500, still LLInt" },
        { "tiered: baseline" },
        { "tiered: DFG/FTL" },
    };
    constexpr unsigned numBins = sizeof(bins) / sizeof(bins[0]);
    UncheckedKeyHashSet<UnlinkedCodeBlock*> decodeCharged;
    auto binFor = [&](const LinkRecord& r) -> unsigned {
        JITType jt = static_cast<JITType>(r.finalJITType);
        if (jt == JITType::BaselineJIT)
            return 6;
        if (jt == JITType::DFGJIT || jt == JITType::FTLJIT)
            return 7;
        double delta = r.finalExecuteCount - r.initialExecuteCount;
        if (delta <= 0)
            return 0;
        if (delta <= 15)
            return 1;
        if (delta <= 30)
            return 2;
        if (delta <= 150)
            return 3;
        if (delta <= 1500)
            return 4;
        return 5;
    };
    UncheckedKeyHashMap<UnlinkedCodeBlock*, DecodeRecord*> decodeByUnlinked;
    for (auto& d : s.decodes) {
        if (!d->codeBlock)
            continue;
        // A dead record's cell address may have been reused by a later decode; prefer the live one.
        auto result = decodeByUnlinked.add(d->codeBlock, d.get());
        if (!result.isNewEntry && result.iterator->value->dead && !d->dead)
            result.iterator->value = d.get();
    }
    uint64_t totalMetadata = 0, totalLinkCount = s.links.size(), deadLinks = 0;
    struct Cold {
        unsigned maxBin;
        const char* label;
        std::array<uint64_t, numberOfBuckets> ticks { };
        uint64_t count { 0 }, metadata { 0 }, identifiers { 0 }, children { 0 }, instructionBytes { 0 }, constants { 0 }, symbolTablesCloned { 0 };
    };
    Cold colds[] = { { 1, "ran <=1x (delta<=15, never tiered)" }, { 2, "ran <=2x (delta<=30, never tiered)" }, { 3, "ran <=~10x (delta<=150, never tiered)" } };
    for (auto& lp : s.links) {
        LinkRecord& r = *lp;
        deadLinks += r.dead;
        unsigned b = binFor(r);
        Bin& bin = bins[b];
        bin.count++;
        bin.linkTicks += r.ticks[static_cast<unsigned>(Bucket::LinkTotal)];
        bin.metadataBytes += r.metadataBytes;
        bin.instructionBytes += r.instructionBytes;
        bin.identifiers += r.identifiers;
        bin.children += r.children;
        totalMetadata += r.metadataBytes;
        DecodeRecord* d = nullptr;
        if (auto it = decodeByUnlinked.find(r.unlinkedCodeBlock); it != decodeByUnlinked.end() && decodeCharged.add(r.unlinkedCodeBlock).isNewEntry) {
            d = it->value;
            bin.decodeTicks += d->ticks[static_cast<unsigned>(Bucket::DecodeTotal)];
        }
        for (Cold& cold : colds) {
            if (b > cold.maxBin)
                continue;
            cold.count++;
            cold.metadata += r.metadataBytes;
            cold.identifiers += r.identifiers;
            cold.children += r.children;
            cold.instructionBytes += r.instructionBytes;
            cold.constants += r.constants;
            cold.symbolTablesCloned += r.symbolTablesCloned;
            for (unsigned i = 0; i < numberOfBuckets; ++i)
                cold.ticks[i] += r.ticks[i] + (d ? d->ticks[i] : 0);
        }
    }
    out.printf("\n-- CodeBlock link summary --\n");
    out.printf("  CodeBlocks linked: %llu (destroyed before dump: %llu); MetadataTable bytes allocated: %.2f MB\n", (unsigned long long)totalLinkCount, (unsigned long long)deadLinks, totalMetadata / (1024.0 * 1024.0));
    {
        uint64_t instructions = 0, instructionBytes = 0, metadataOps = 0, coldInstructions = 0, coldMetadataOps = 0;
        for (auto& lp : s.links) {
            instructions += lp->instructionCount;
            instructionBytes += lp->instructionBytes;
            metadataOps += lp->profiledMetadataOps;
            if (binFor(*lp) <= 1) {
                coldInstructions += lp->instructionCount;
                coldMetadataOps += lp->profiledMetadataOps;
            }
        }
        out.printf("  bytecode instructions walked at link: %llu (%.2f MB); plain profiled-metadata entries initialized: %llu; scope-resolution ops: %llu; in <=1x blocks: %llu instructions, %llu metadata entries\n",
            (unsigned long long)instructions, instructionBytes / (1024.0 * 1024.0), (unsigned long long)metadataOps, (unsigned long long)s.counts[static_cast<unsigned>(Bucket::LinkScopeResolution)], (unsigned long long)coldInstructions, (unsigned long long)coldMetadataOps);
    }
    {
        // Fate of the FunctionExecutables that CodeBlock linking created eagerly for every child (live CodeBlocks only).
        UncheckedKeyHashSet<FunctionExecutable*> seen;
        UncheckedKeyHashSet<UnlinkedFunctionExecutable*> unlinkedSeen, unlinkedInstantiated, unlinkedRan;
        uint64_t total = 0, instantiated = 0, ran = 0, inColdBlocks = 0, inColdNeverInstantiated = 0;
        for (auto& entry : s.liveLinks) {
            CodeBlock* codeBlock = entry.key;
            LinkRecord& r = *entry.value;
            bool cold = binFor(r) <= 1;
            auto visit = [&](FunctionExecutable* executable) {
                if (!executable || !seen.add(executable).isNewEntry)
                    return;
                total++;
                bool gotFunction = s.instantiatedExecutables.contains(executable);
                bool didRun = executable->isGeneratedForCall() || executable->isGeneratedForConstruct();
                instantiated += gotFunction;
                ran += didRun;
                inColdBlocks += cold;
                inColdNeverInstantiated += cold && !gotFunction;
                UnlinkedFunctionExecutable* unlinked = executable->unlinkedExecutable();
                unlinkedSeen.add(unlinked);
                if (gotFunction)
                    unlinkedInstantiated.add(unlinked);
                if (didRun)
                    unlinkedRan.add(unlinked);
            };
            for (int i = 0, n = codeBlock->numberOfFunctionDecls(); i < n; ++i)
                visit(codeBlock->functionDecl(i));
            for (size_t i = 0, n = codeBlock->numberOfFunctionExprs(); i < n; ++i)
                visit(codeBlock->functionExpr(i));
        }
        out.printf("  FunctionExecutables created eagerly at link (children of live CodeBlocks): %llu (sizeof=%zu => %.2f MB); JSFunction allocations seen: %llu\n", (unsigned long long)total, sizeof(FunctionExecutable), total * static_cast<double>(sizeof(FunctionExecutable)) / (1024 * 1024), (unsigned long long)s.functionsInstantiated);
        out.printf("    got >=1 JSFunction (new_func ran): %llu (%.1f%%); NEVER instantiated: %llu (%.1f%%) => %.2f MB FunctionExecutable never used\n", (unsigned long long)instantiated, total ? 100.0 * instantiated / total : 0.0, (unsigned long long)(total - instantiated), total ? 100.0 * (total - instantiated) / total : 0.0, (total - instantiated) * static_cast<double>(sizeof(FunctionExecutable)) / (1024 * 1024));
        out.printf("    ever executed (has a CodeBlock): %llu (%.1f%%); instantiated but never called: %llu\n", (unsigned long long)ran, total ? 100.0 * ran / total : 0.0, (unsigned long long)(instantiated >= ran ? instantiated - ran : 0));
        out.printf("    in <=1x parent blocks: %llu, of which never instantiated: %llu\n", (unsigned long long)inColdBlocks, (unsigned long long)inColdNeverInstantiated);
        uint64_t u = unlinkedSeen.size();
        out.printf("  distinct child UnlinkedFunctionExecutables behind them: %llu; with >=1 JSFunction: %llu (%.1f%%); ever executed: %llu (%.1f%%); NEVER instantiated: %llu (%.1f%%) => %.2f MB UnlinkedFunctionExecutable + %.2f MB FunctionExecutable for functions that never became a closure\n",
            (unsigned long long)u, (unsigned long long)unlinkedInstantiated.size(), u ? 100.0 * unlinkedInstantiated.size() / u : 0.0, (unsigned long long)unlinkedRan.size(), u ? 100.0 * unlinkedRan.size() / u : 0.0,
            (unsigned long long)(u - unlinkedInstantiated.size()), u ? 100.0 * (u - unlinkedInstantiated.size()) / u : 0.0,
            (u - unlinkedInstantiated.size()) * static_cast<double>(sizeof(UnlinkedFunctionExecutable)) / (1024 * 1024), (total - instantiated) * static_cast<double>(sizeof(FunctionExecutable)) / (1024 * 1024));
    }
    out.printf("\n-- Histogram: creation cost vs executions (LLInt counter delta at dump; entry=+15, loop=+1) --\n");
    out.printf("  %-42s %8s %10s %10s %10s %10s %10s %10s\n", "bin", "blocks", "link ms", "decode ms", "meta MB", "instr KB", "idents", "children");
    for (unsigned i = 0; i < numBins; ++i) {
        Bin& b = bins[i];
        out.printf("  %-42s %8llu %10.3f %10.3f %10.2f %10.1f %10llu %10llu\n", b.label, (unsigned long long)b.count, ms(b.linkTicks), ms(b.decodeTicks), b.metadataBytes / (1024.0 * 1024.0), b.instructionBytes / 1024.0, (unsigned long long)b.identifiers, (unsigned long long)b.children);
    }
    for (Cold& cold : colds) {
        out.printf("\n-- Cost attributable to CodeBlocks that %s: %llu of %llu blocks (%.1f%%) --\n", cold.label, (unsigned long long)cold.count, (unsigned long long)totalLinkCount, totalLinkCount ? 100.0 * cold.count / totalLinkCount : 0.0);
        for (unsigned i = 0; i < numberOfBuckets; ++i) {
            if (cold.ticks[i])
                out.printf("  %-30s %10.3f ms (of %10.3f ms total = %.1f%%)\n", bucketName(i), ms(cold.ticks[i]), ms(s.ticks[i]), s.ticks[i] ? 100.0 * cold.ticks[i] / s.ticks[i] : 0.0);
        }
        out.printf("  MetadataTable bytes for these blocks: %.2f MB of %.2f MB (%.1f%%); identifiers: %llu; children: %llu; constants: %llu; SymbolTables cloned: %llu; instruction KB: %.1f\n",
            cold.metadata / (1024.0 * 1024.0), totalMetadata / (1024.0 * 1024.0), totalMetadata ? 100.0 * cold.metadata / totalMetadata : 0.0, (unsigned long long)cold.identifiers, (unsigned long long)cold.children, (unsigned long long)cold.constants, (unsigned long long)cold.symbolTablesCloned, cold.instructionBytes / 1024.0);
    }

    // ---- Top 20 by creation cost (link + own decode) ----
    struct Ranked {
        LinkRecord* link;
        DecodeRecord* decode;
        uint64_t ticks;
    };
    Vector<Ranked> ranked;
    ranked.reserveInitialCapacity(s.links.size());
    for (auto& lp : s.links) {
        DecodeRecord* d = nullptr;
        if (auto it = decodeByUnlinked.find(lp->unlinkedCodeBlock); it != decodeByUnlinked.end())
            d = it->value;
        ranked.append({ lp.get(), d, lp->ticks[static_cast<unsigned>(Bucket::LinkTotal)] + (d ? d->ticks[static_cast<unsigned>(Bucket::DecodeTotal)] : 0) });
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.ticks > b.ticks; });
    out.printf("\n-- Top 20 CodeBlocks by creation cost (link + decode of its UnlinkedCodeBlock) --\n");
    out.printf("  %9s %9s %9s %9s %9s %9s %8s %7s %6s %6s %8s %-8s %s\n", "total ms", "link", "decode", "walk", "consts", "funcs", "instrB", "metaB", "ident", "child", "delta", "tier", "type name");
    for (unsigned i = 0; i < ranked.size() && i < 20; ++i) {
        Ranked& r = ranked[i];
        LinkRecord& l = *r.link;
        CString name;
        if (!l.dead && l.codeBlock)
            name = l.codeBlock->inferredName();
        else
            name = "<destroyed>";
        out.printf("  %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %8u %7u %6u %6u %8.0f %-8s %s %s\n",
            ms(r.ticks), ms(l.ticks[static_cast<unsigned>(Bucket::LinkTotal)]), r.decode ? ms(r.decode->ticks[static_cast<unsigned>(Bucket::DecodeTotal)]) : 0.0,
            ms(l.ticks[static_cast<unsigned>(Bucket::LinkInstructionWalk)]), ms(l.ticks[static_cast<unsigned>(Bucket::LinkConstants)]), ms(l.ticks[static_cast<unsigned>(Bucket::LinkFunctions)]),
            l.instructionBytes, l.metadataBytes, l.identifiers, l.children, l.finalExecuteCount - l.initialExecuteCount, jitTypeName(l.finalJITType), codeTypeName(l.codeType), name.data() ? name.data() : "");
    }

    // ---- Top 10 decoded blocks by decode cost (typically the module/program roots with eager children) ----
    Vector<DecodeRecord*> decodesRanked;
    for (auto& d : s.decodes)
        decodesRanked.append(d.get());
    std::sort(decodesRanked.begin(), decodesRanked.end(), [](DecodeRecord* a, DecodeRecord* b) { return a->ticks[static_cast<unsigned>(Bucket::DecodeTotal)] > b->ticks[static_cast<unsigned>(Bucket::DecodeTotal)]; });
    out.printf("\n-- Top 10 UnlinkedCodeBlocks by decode cost --\n");
    out.printf("  %9s %9s %9s %9s %9s %9s %8s %7s %7s %7s %7s %s\n", "total ms", "fixed", "idents", "children", "consts", "symtab", "instrB", "ident", "touched", "child", "consts", "type");
    for (unsigned i = 0; i < decodesRanked.size() && i < 10; ++i) {
        DecodeRecord& d = *decodesRanked[i];
        auto t = [&](Bucket b) { return ms(d.ticks[static_cast<unsigned>(b)]); };
        out.printf("  %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %8u %7u %7u %7u %7u %s\n", t(Bucket::DecodeTotal), t(Bucket::DecodeFixed), t(Bucket::DecodeIdentifiers), t(Bucket::DecodeChildren), t(Bucket::DecodeConstants), t(Bucket::DecodeConstantSymbolTables),
            d.instructionBytes, d.identifiers, d.identifiersTouched, d.children, d.constants, d.isFunctionCode ? "function" : "program/module");
    }
    out.printf("=== end CodeBlock creation cost report ===\n\n");

    CString text = out.toCString();
#if OS(UNIX)
    // Straight to fd 2 so it survives late-exit ordering; one write keeps it contiguous with other stderr output.
    const char* p = text.data();
    size_t remaining = text.length();
    while (remaining) {
        ssize_t n = write(STDERR_FILENO, p, remaining);
        if (n <= 0)
            break;
        p += n;
        remaining -= n;
    }
#else
    fputs(text.data(), stderr);
    fflush(stderr);
#endif
}

} // namespace CodeBlockCreationStats
} // namespace JSC
