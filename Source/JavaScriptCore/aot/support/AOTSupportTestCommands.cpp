/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSupportTestCommands.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTDiagnostics.h"
#include "AOTLimits.h"
#include "AOTSchemaEnums.h"
#include "AOTWire.h"
#include <utility>
#include <wtf/ListDump.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class DestructionLog {
public:
    void append(unsigned number)
    {
        Locker locker { m_lock };
        m_numbers.append(number);
    }

    bool contains(unsigned number)
    {
        Locker locker { m_lock };
        return m_numbers.contains(number);
    }

    Vector<unsigned> take()
    {
        Locker locker { m_lock };
        return std::exchange(m_numbers, { });
    }

private:
    Lock m_lock;
    Vector<unsigned> m_numbers WTF_GUARDED_BY_LOCK(m_lock);
};

class Recorder {
    WTF_MAKE_NONCOPYABLE(Recorder);
public:
    Recorder(BuildContext& build, DestructionLog& log, unsigned number, std::optional<unsigned> successor)
        : m_build(build)
        , m_log(&log)
        , m_number(number)
        , m_successor(successor)
    {
    }

    Recorder(Recorder&& other)
        : m_build(other.m_build)
        , m_log(std::exchange(other.m_log, nullptr))
        , m_number(other.m_number)
        , m_successor(other.m_successor)
    {
    }

    ~Recorder()
    {
        if (!m_log)
            return;
        m_log->append(m_number);
        if (m_successor)
            m_build.destroyLater(Recorder(m_build, *m_log, *m_successor, std::nullopt));
    }

private:
    BuildContext& m_build;
    DestructionLog* m_log;
    unsigned m_number;
    std::optional<unsigned> m_successor;
};

struct BuildContextMode {
    ASCIILiteral name;
    void (*run)(TestCommandContext&, std::span<const String> words);
};

} // anonymous namespace

void printBadArgument(PrintStream& out, StringView word)
{
    out.println("error: bad argument "_s, word);
}

void printMissingArgument(PrintStream& out)
{
    out.println("error: missing argument"_s);
}

void printCheck(PrintStream& out, ASCIILiteral name, bool passed)
{
    out.println(name, ": "_s, passed ? "ok"_s : "FAILED"_s);
}

ASCIILiteral yesOrNo(bool value)
{
    return value ? "yes"_s : "no"_s;
}

bool isOnlyWord(std::span<const String> words, ASCIILiteral word)
{
    return words.size() == 1 && words[0] == word;
}

std::optional<unsigned> parseThreadCount(StringView text)
{
    auto threads = parseTestNumber<unsigned>(text);
    return threads && *threads && *threads <= Limits::maxWorkers ? threads : std::nullopt;
}

bool takeCommonTestWord(StringView word, CommonTestWords& words)
{
    constexpr ASCIILiteral threadsPrefix = "threads="_s;
    constexpr ASCIILiteral seedPrefix = "seed="_s;
    if (word.startsWith(threadsPrefix)) {
        auto threads = parseThreadCount(word.substring(threadsPrefix.length()));
        if (!threads)
            return false;
        words.threads = *threads;
        return true;
    }
    if (word.startsWith(seedPrefix)) {
        auto seed = parseTestNumber<unsigned>(word.substring(seedPrefix.length()));
        if (!seed)
            return false;
        words.seed = *seed;
        return true;
    }
    return false;
}

std::optional<CommonTestWords> parseCommonTestWords(PrintStream& out, std::span<const String> words)
{
    CommonTestWords commonWords;
    for (const String& word : words) {
        if (!takeCommonTestWord(word, commonWords)) {
            printBadArgument(out, word);
            return std::nullopt;
        }
    }
    return commonWords;
}

static bool formatsLimitExceeded(LimitID limit)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    options.threadCount = 1;
    BuildContext build(options);
    build.reportLimitExceeded(limit, 1);

    Diagnostics& diagnostics = build.diagnostics();
    diagnostics.finish(nullptr);
    if (diagnostics.userDiagnostics().size() != 1)
        return false;
    const String& message = diagnostics.messageOfUserDiagnostic(0);
    return !message.isEmpty() && !message.contains('{') && !message.contains('}');
}

static bool hasValidColumns(LimitID limit)
{
    if (purposeOf(limit).isEmpty())
        return false;
    if (makeString(nameOf(limit), "Hits"_s) != nameOf(hitsOf(limit)))
        return false;
    return !failsBuild(limit) || formatsLimitExceeded(limit);
}

static void runAllLimits(PrintStream& out)
{
    unsigned numberOfCheckedLimits = 0;
    unsigned numberOfBadLimits = 0;
    for (LimitID limit : allMembersOf<LimitID>) {
        if (limit == LimitID::None)
            continue;
        ++numberOfCheckedLimits;
        if (hasValidColumns(limit))
            continue;
        ++numberOfBadLimits;
        out.println("bad limit "_s, nameOf(limit));
    }
    out.println("limits: "_s, numberOfCheckedLimits, ", "_s, numberOfBadLimits, " bad"_s);
}

static void runLimits(TestCommandContext& context)
{
    if (context.arguments.empty())
        return printMissingArgument(context.out);
    if (isOnlyWord(context.arguments, "all"_s))
        return runAllLimits(context.out);

    auto limit = parseEnum<LimitID>(context.arguments[0]);
    if (!limit || *limit == LimitID::None)
        return printBadArgument(context.out, context.arguments[0]);
    Vector<uint64_t> amounts;
    for (const String& argument : context.arguments.subspan(1)) {
        auto amount = parseTestNumber<uint64_t>(argument);
        if (!amount)
            return printBadArgument(context.out, argument);
        amounts.append(*amount);
    }

    context.out.println("limit "_s, nameOf(*limit), " = "_s, context.build.limit(*limit), " (default "_s, defaultLimitValue(*limit, hostArchitecture()), ')');
    context.out.println("can be lowered: "_s, yesOrNo(canBeLowered(*limit)));
    for (uint64_t amount : amounts)
        context.out.println("exceeds "_s, amount, ": "_s, yesOrNo(context.build.exceeds(*limit, amount)));
    context.out.println("gave up: "_s, nameOf(context.build.gaveUp(*limit).limit));
}

static void runThreadCount(TestCommandContext& context, std::span<const String> words)
{
    if (!words.empty())
        return printBadArgument(context.out, words[0]);
    context.out.println("thread count ["_s, context.build.threadCount(), ']');
}

static void runDestroyLater(TestCommandContext& context, std::span<const String> words)
{
    constexpr unsigned numberOfValues = 4;

    if (words.empty())
        return printMissingArgument(context.out);
    auto threads = parseThreadCount(words[0]);
    if (!threads)
        return printBadArgument(context.out, words[0]);
    if (words.size() > 1)
        return printBadArgument(context.out, words[1]);

    DestructionLog log;
    bool wereDestroyedAtOnce = true;
    {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        options.threadCount = *threads;
        BuildContext build(options);
        for (unsigned number = 0; number < numberOfValues; ++number) {
            std::optional<unsigned> successor;
            if (number + 1 == numberOfValues)
                successor = numberOfValues;
            build.destroyLater(Recorder(build, log, number, successor));
            if (!log.contains(number))
                wereDestroyedAtOnce = false;
        }
    }

    Vector<unsigned> destroyed = log.take();
    context.out.println("destroyed ["_s, listDump(destroyed, " "_s), ']');
    context.out.println("destroyed at once: "_s, yesOrNo(wereDestroyedAtOnce));
}

static void runScratch(TestCommandContext& context, std::span<const String> words)
{
    if (!words.empty())
        return printBadArgument(context.out, words[0]);

    BuildContext& build = context.build;
    auto* scratch = &build.backEndScratch(0);
    printCheck(context.out, "scratch is reused"_s, &build.backEndScratch(0) == scratch);
    printCheck(context.out, "workers have separate scratches"_s, build.threadCount() == 1 || &build.backEndScratch(1) != scratch);
}

static void runBuildContext(TestCommandContext& context)
{
    static constexpr BuildContextMode modes[] = {
        { "thread-count"_s, runThreadCount },
        { "destroy-later"_s, runDestroyLater },
        { "scratch"_s, runScratch },
    };

    if (context.arguments.empty())
        return printMissingArgument(context.out);
    for (const BuildContextMode& mode : modes) {
        if (context.arguments[0] == mode.name)
            return mode.run(context, context.arguments.subspan(1));
    }
    printBadArgument(context.out, context.arguments[0]);
}

std::span<const TestCommand> supportTestCommands()
{
    static constexpr TestCommand commands[] = {
        { "arena-merge"_s, std::nullopt, runArenaMergeTestCommand },
        { "hashed-index"_s, std::nullopt, runHashedIndexTestCommand },
        { "diagnostics"_s, IRForm::Prepared, runDiagnosticsTestCommand },
        { "losses"_s, IRForm::Prepared, runLossesTestCommand },
        { "limits"_s, std::nullopt, runLimits },
        { "parallel-loops"_s, std::nullopt, runParallelLoopsTestCommand },
        { "build-context"_s, std::nullopt, runBuildContext },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
