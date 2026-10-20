/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTChildProcess.h"

#if ENABLE(AOT)

#include <array>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Seconds.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringView.h>

#if OS(WINDOWS)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr Seconds silenceLimit = 300_s;

static Lock spawnLock;

using OutputBuffer = std::array<uint8_t, 16 * KB>;

#if OS(WINDOWS)

static constexpr Seconds outputPollInterval = 20_ms;

static ASCIILiteral signalName(int)
{
    return { };
}

static void appendQuotedArgument(StringBuilder& commandLine, StringView argument)
{
    if (!commandLine.isEmpty())
        commandLine.append(' ');
    commandLine.append('"');
    unsigned backslashCount = 0;
    auto appendBackslashes = [&](unsigned count) {
        for (unsigned i = 0; i < count; ++i)
            commandLine.append('\\');
    };
    for (char16_t character : argument.codeUnits()) {
        if (character == '\\') {
            ++backslashCount;
            continue;
        }
        appendBackslashes(character == '"' ? 2 * backslashCount + 1 : backslashCount);
        backslashCount = 0;
        commandLine.append(character);
    }
    appendBackslashes(2 * backslashCount);
    commandLine.append('"');
}

static bool readOutput(HANDLE pipe, HANDLE process, Vector<uint8_t>& output)
{
    MonotonicTime lastOutputTime = MonotonicTime::now();
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            return true;
        if (available) {
            OutputBuffer buffer;
            DWORD count = 0;
            if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
                return true;
            output.append(std::span(buffer).first(count));
            lastOutputTime = MonotonicTime::now();
            continue;
        }
        if (MonotonicTime::now() - lastOutputTime > silenceLimit)
            return false;
        WaitForSingleObject(process, outputPollInterval.millisecondsAs<DWORD>());
    }
}

static void disableCrashReports()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

static ChildProcessResult runProcess(const Vector<StringView>& commandLine)
{
    StringBuilder quotedCommandLine;
    for (StringView argument : commandLine)
        appendQuotedArgument(quotedCommandLine, argument);
    Vector<wchar_t> commandLineCharacters = quotedCommandLine.toString().wideCharacters();

    HANDLE readEnd = nullptr;
    PROCESS_INFORMATION process { };
    {
        Locker locker { spawnLock };
        SECURITY_ATTRIBUTES inheritable { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0))
            return { };
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup { };
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writeEnd;
        startup.hStdError = writeEnd;
        BOOL hasStarted = CreateProcessW(nullptr, commandLineCharacters.mutableSpan().data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process);
        CloseHandle(writeEnd);
        if (!hasStarted) {
            CloseHandle(readEnd);
            return { };
        }
    }

    Vector<uint8_t> output;
    bool hasHung = !readOutput(readEnd, process.hProcess, output);
    if (hasHung)
        TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(readEnd);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    constexpr DWORD exceptionSeverityBit = 0x80000000;
    ChildProcessOutcome outcome = ChildProcessOutcome::Exited;
    if (hasHung)
        outcome = ChildProcessOutcome::Hung;
    else if (exitCode & exceptionSeverityBit)
        outcome = ChildProcessOutcome::Crashed;
    return { outcome, static_cast<int>(exitCode), String::fromUTF8ReplacingInvalidSequences(output.span()) };
}

#else // OS(WINDOWS)

static ASCIILiteral signalName(int signal)
{
    switch (signal) {
    case SIGABRT:
        return "SIGABRT"_s;
    case SIGBUS:
        return "SIGBUS"_s;
    case SIGFPE:
        return "SIGFPE"_s;
    case SIGILL:
        return "SIGILL"_s;
    case SIGKILL:
        return "SIGKILL"_s;
    case SIGSEGV:
        return "SIGSEGV"_s;
    case SIGTRAP:
        return "SIGTRAP"_s;
    default:
        return { };
    }
}

static bool readOutput(int pipe, Vector<uint8_t>& output)
{
    for (;;) {
        struct pollfd poller { pipe, POLLIN, 0 };
        int readyCount = poll(&poller, 1, silenceLimit.millisecondsAs<int>());
        if (readyCount < 0 && errno == EINTR)
            continue;
        if (readyCount <= 0)
            return false;

        OutputBuffer buffer;
        ssize_t count = read(pipe, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return true;
        output.append(std::span(buffer).first(static_cast<size_t>(count)));
    }
}

static void disableCrashReports()
{
    struct rlimit noCoreFile { 0, 0 };
    setrlimit(RLIMIT_CORE, &noCoreFile);
}

struct StartedProcess {
    pid_t process;
    int output;
};

static std::optional<StartedProcess> startProcess(const Vector<StringView>& commandLine)
{
    Vector<UTF8CString> arguments = commandLine.map([](StringView argument) {
        return argument.utf8();
    });
    Vector<char*> argumentPointers;
    for (UTF8CString& argument : arguments)
        argumentPointers.append(byteCast<char>(argument.mutableSpanIncludingNullTerminator().data()));
    argumentPointers.append(nullptr);

    Locker locker { spawnLock };
    std::array<int, 2> pipeEnds;
    if (pipe(pipeEnds.data()))
        return std::nullopt;
    auto [readEnd, writeEnd] = pipeEnds;
    fcntl(readEnd, F_SETFD, FD_CLOEXEC);
    fcntl(writeEnd, F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, writeEnd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, writeEnd, STDERR_FILENO);
    pid_t process = 0;
    int error = posix_spawnp(&process, argumentPointers[0], &actions, nullptr, argumentPointers.mutableSpan().data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(writeEnd);
    if (error) {
        close(readEnd);
        return std::nullopt;
    }
    return StartedProcess { process, readEnd };
}

static ChildProcessResult runProcess(const Vector<StringView>& commandLine)
{
    std::optional<StartedProcess> started = startProcess(commandLine);
    if (!started)
        return { };

    Vector<uint8_t> output;
    bool hasHung = !readOutput(started->output, output);
    close(started->output);
    if (hasHung)
        kill(started->process, SIGKILL);

    int status = 0;
    while (waitpid(started->process, &status, 0) < 0 && errno == EINTR) { }

    String text = String::fromUTF8ReplacingInvalidSequences(output.span());
    if (hasHung)
        return { ChildProcessOutcome::Hung, 0, WTF::move(text) };
    if (WIFSIGNALED(status))
        return { ChildProcessOutcome::Crashed, WTERMSIG(status), WTF::move(text) };
    return { ChildProcessOutcome::Exited, WEXITSTATUS(status), WTF::move(text) };
}

#endif // OS(WINDOWS)

static String& mutableProgramPath()
{
    static NeverDestroyed<String> path;
    return path;
}

void setProgramPath(String&& path)
{
    mutableProgramPath() = WTF::move(path);
}

const String& programPath()
{
    return mutableProgramPath();
}

String ChildProcessResult::description() const
{
    switch (outcome) {
    case ChildProcessOutcome::Exited:
        return makeString("exited with "_s, code);
    case ChildProcessOutcome::Crashed:
        if (ASCIILiteral name = signalName(code); !name.isNull())
            return makeString("crashed with "_s, name);
        return makeString("crashed with "_s, static_cast<unsigned>(code));
    case ChildProcessOutcome::Hung:
        return makeString("printed nothing for "_s, silenceLimit.secondsAs<unsigned>(), " seconds"_s);
    case ChildProcessOutcome::NotStarted:
        return "could not be started in a process of its own"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ChildProcessLauncher::ChildProcessLauncher(String&& program, Vector<String>&& sharedArguments)
    : m_program(WTF::move(program))
    , m_sharedArguments(WTF::move(sharedArguments))
{
    disableCrashReports();
}

ChildProcessResult ChildProcessLauncher::run(std::span<const String> arguments) const
{
    Vector<StringView> commandLine;
    commandLine.append(m_program);
    for (const String& argument : arguments)
        commandLine.append(argument);
    for (const String& argument : m_sharedArguments)
        commandLine.append(argument);
    return runProcess(commandLine);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
