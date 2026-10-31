/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTChildProcess.h"

#if ENABLE(AOT)

#include "AOTCompile.h"
#include "AOTVector.h"
#include <array>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/FileSystem.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ProcessID.h>
#include <wtf/Seconds.h>
#include <wtf/Threading.h>
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
#if OS(LINUX)
#include <sys/prctl.h>
#include <sys/uio.h>
#endif

extern char** environ;
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// Shorter than the five minutes that Bun's runner gives a file, so that a test that never ends is named.
#if ASAN_ENABLED || ASSERT_ENABLED
static constexpr Seconds silenceLimit = 300_s;
#else
static constexpr Seconds silenceLimit = 100_s;
#endif

static Lock spawnLock;
static bool printsLineAfterEachChildProcess;

using OutputBuffer = std::array<uint8_t, 16 * KB>;

static String temporaryFolderOf(ProcessID process)
{
    return FileSystem::pathByAppendingComponent(temporaryFolder(), makeString("testaot-"_s, process));
}

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

// Windows ends the processes of a job when the last handle to it closes: when this process ends, however it ends.
static HANDLE jobOfChildren()
{
    static HANDLE job = [] {
        HANDLE created = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits { };
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        return created;
    }();
    return job;
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
        AssignProcessToJobObject(jobOfChildren(), process.hProcess);
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
    FileSystem::deleteNonEmptyDirectory(temporaryFolderOf(process.dwProcessId));

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
    // Where core_pattern names a program, Linux starts it whatever the limit is, except for a limit of 1. No file is written under a limit of less than a page.
    struct rlimit noCoreFile { 1, 1 };
    setrlimit(RLIMIT_CORE, &noCoreFile);
}

struct StartedProcess {
    pid_t process;
    int output;
};

// A test that never ends must not outlive the process that would have ended it.
#if OS(LINUX)

static constexpr int exitCodeWithoutProgram = 127;

// posix_spawn() cannot ask for the signal, which the kernel sends when the thread that started the child ends, however it ends.
static int spawn(pid_t& process, char* const* arguments, int output)
{
    pid_t parent = getpid();
    process = vfork();
    if (process)
        return process < 0 ? errno : 0;

    // This is the parent's memory until exec: system calls only.
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    close(STDIN_FILENO);
    if (getppid() == parent && open("/dev/null", O_RDONLY) == STDIN_FILENO && dup2(output, STDOUT_FILENO) >= 0 && dup2(output, STDERR_FILENO) >= 0)
        execvp(arguments[0], arguments);
    _exit(exitCodeWithoutProgram);
}

static void killHung(pid_t process)
{
    kill(process, SIGKILL);
}

#else

// There is no such signal here. A child leads a group, what it starts stays in the group, and a child that hangs ends with its group.
static bool startsGroups()
{
    return getpgrp() != getpid();
}

static int spawn(pid_t& process, char* const* arguments, int output)
{
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output, STDERR_FILENO);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    if (startsGroups())
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    int error = posix_spawnp(&process, arguments[0], &actions, &attributes, arguments, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    return error;
}

static void killHung(pid_t process)
{
    kill(startsGroups() ? -process : process, SIGKILL);
}

#endif

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

    pid_t process = 0;
    int error = spawn(process, argumentPointers.mutableSpan().data(), writeEnd);
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
        killHung(started->process);

    int status = 0;
    while (waitpid(started->process, &status, 0) < 0 && errno == EINTR) { }
    FileSystem::deleteNonEmptyDirectory(temporaryFolderOf(started->process));
#if OS(LINUX)
    if (WIFEXITED(status) && WEXITSTATUS(status) == exitCodeWithoutProgram && output.isEmpty())
        return { };
#endif

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

// Where WTF::FileSystem puts temporary files.
String temporaryFolder()
{
#if OS(WINDOWS)
    std::array<wchar_t, MAX_PATH + 1> path;
    DWORD length = GetTempPathW(path.size(), path.data());
    RELEASE_ASSERT(length && length < path.size());
    return String(path.data(), length);
#else
    const char* path = getenv("TMPDIR");
    return String::fromUTF8(path ? path : "/tmp");
#endif
}

void enterTemporaryFolder()
{
    static NeverDestroyed<String> folder;
    folder.get() = temporaryFolderOf(getCurrentProcessID());
    // Of an earlier process with this number, whose parent was killed.
    FileSystem::deleteNonEmptyDirectory(folder);
    RELEASE_ASSERT(FileSystem::makeAllDirectories(folder));
#if OS(WINDOWS)
    RELEASE_ASSERT(SetEnvironmentVariableW(L"TMP", folder->wideCharacters().span().data()));
#else
    RELEASE_ASSERT(!setenv("TMPDIR", byteCast<char>(folder->utf8().spanIncludingNullTerminator().data()), 1));
#endif
    atexit([] {
        FileSystem::deleteNonEmptyDirectory(folder);
    });
}

#if OS(LINUX) && (CPU(X86_64) || CPU(ARM64))

#if CPU(X86_64)
static constexpr int trapSignal = SIGILL;
static constexpr std::array registerNames { "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rdi", "rsi", "rbp", "rbx", "rdx", "rax", "rcx", "rsp", "rip" };
#else
static constexpr int trapSignal = SIGTRAP;
#endif

static void printToStandardError(const char* format, ...) WTF_ATTRIBUTE_PRINTF(1, 2);
static void printToStandardError(const char* format, ...)
{
    std::array<char, 256> text;
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(text.data(), text.size(), format, arguments);
    va_end(arguments);
    if (length <= 0)
        return;
    ssize_t written = write(STDERR_FILENO, text.data(), std::min<size_t>(length, text.size() - 1));
    UNUSED_VARIABLE(written);
}

static void printMemoryBehind(uintptr_t address)
{
    std::array<uintptr_t, 8> words;
    address &= ~(sizeof(uintptr_t) - 1);
    struct iovec target { words.data(), sizeof(words) };
    struct iovec source { reinterpret_cast<void*>(address), sizeof(words) };
    // Fails where a load would fault.
    if (process_vm_readv(getpid(), &target, 1, &source, 1, 0) != static_cast<ssize_t>(sizeof(words)))
        return;
    printToStandardError("0x%llx:", static_cast<unsigned long long>(address));
    for (uintptr_t word : words)
        printToStandardError(" %016llx", static_cast<unsigned long long>(word));
    printToStandardError("\n");
}

static void printMappings()
{
    int file = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (file < 0)
        return;
    std::array<char, 4 * KB> text;
    for (ssize_t count; (count = read(file, text.data(), text.size())) > 0;) {
        ssize_t written = write(STDERR_FILENO, text.data(), count);
        UNUSED_VARIABLE(written);
    }
    close(file);
}

static void reportTrap(int signal, siginfo_t*, void* context)
{
    constexpr uintptr_t redZoneSize = 128;
    constexpr uintptr_t dumpedStackSize = 4 * KB;
    constexpr uintptr_t wordsPerLine = 4;
    constexpr unsigned maxFrames = 64;

    const mcontext_t& machineContext = static_cast<ucontext_t*>(context)->uc_mcontext;
#if CPU(X86_64)
    uintptr_t pc = machineContext.gregs[REG_RIP];
    uintptr_t sp = machineContext.gregs[REG_RSP];
    uintptr_t fp = machineContext.gregs[REG_RBP];
#else
    uintptr_t pc = machineContext.pc;
    uintptr_t sp = machineContext.sp;
    uintptr_t fp = machineContext.regs[29];
#endif

    std::array<char, 16> threadName { };
    prctl(PR_GET_NAME, threadName.data());
    printToStandardError("%s in thread \"%s\" at 0x%llx\n", signalName(signal).characters(), threadName.data(), static_cast<unsigned long long>(pc));

    std::array<char, 1024> work;
    if (size_t size = describeWhereThisThreadStopped(work)) {
        ssize_t written = write(STDERR_FILENO, work.data(), size);
        UNUSED_VARIABLE(written);
    }

    printToStandardError("registers:");
#if CPU(X86_64)
    for (size_t index = 0; index < registerNames.size(); ++index)
        printToStandardError(" %s=%llx", registerNames[index], static_cast<unsigned long long>(machineContext.gregs[index]));
#else
    for (size_t index = 0; index < std::size(machineContext.regs); ++index)
        printToStandardError(" x%zu=%llx", index, static_cast<unsigned long long>(machineContext.regs[index]));
#endif

    const StackBounds& stack = Thread::currentSingleton().stack();
    auto isOnStack = [&](uintptr_t address) {
        return !(address % sizeof(uintptr_t)) && stack.contains(reinterpret_cast<void*>(address));
    };

    printToStandardError("\nreturn addresses:");
    for (unsigned count = 0; count < maxFrames && isOnStack(fp) && isOnStack(fp + sizeof(uintptr_t)); ++count) {
        auto* frame = reinterpret_cast<const uintptr_t*>(fp);
        printToStandardError(" 0x%llx", static_cast<unsigned long long>(frame[1]));
        if (frame[0] <= fp)
            break;
        fp = frame[0];
    }

    printToStandardError("\nstack:\n");
    constexpr uintptr_t lineSize = wordsPerLine * sizeof(uintptr_t);
    for (uintptr_t address = (sp - redZoneSize) & ~(lineSize - 1); address < sp + dumpedStackSize && isOnStack(address) && isOnStack(address + lineSize - sizeof(uintptr_t)); address += lineSize) {
        auto* words = reinterpret_cast<const uintptr_t*>(address);
        printToStandardError("0x%llx: %016llx %016llx %016llx %016llx\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(words[0]), static_cast<unsigned long long>(words[1]), static_cast<unsigned long long>(words[2]), static_cast<unsigned long long>(words[3]));
    }

    printToStandardError("memory behind the registers and the words of the stack:\n");
#if CPU(X86_64)
    for (size_t index = 0; index < registerNames.size(); ++index)
        printMemoryBehind(machineContext.gregs[index]);
#else
    for (uintptr_t value : machineContext.regs)
        printMemoryBehind(value);
#endif
    for (uintptr_t address = (sp - redZoneSize) & ~(sizeof(uintptr_t) - 1); address < sp + dumpedStackSize && isOnStack(address); address += sizeof(uintptr_t)) {
        uintptr_t word = *reinterpret_cast<const uintptr_t*>(address);
        if (!stack.contains(reinterpret_cast<void*>(word)))
            printMemoryBehind(word);
    }

    printToStandardError("mappings:\n");
    printMappings();

    struct sigaction defaultAction { };
    defaultAction.sa_handler = SIG_DFL;
    sigaction(signal, &defaultAction, nullptr);
    raise(signal);
}

void installTrapReporter()
{
    struct sigaction action { };
    action.sa_sigaction = reportTrap;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(trapSignal, &action, nullptr);
}

#else

void installTrapReporter()
{
}

#endif

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
    ChildProcessResult result = runProcess(commandLine);
    if (printsLineAfterEachChildProcess)
        dataLogLn(lineAfterChildProcess);
    return result;
}

void printLineAfterEachChildProcess()
{
    printsLineAfterEachChildProcess = true;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
