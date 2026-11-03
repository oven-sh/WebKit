/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCommandLine.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyCodeCache.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyExec.h"
#include "PyFunction.h"
#include "PyGenerator.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyStr.h"
#include "PyVirtualFiles.h"
#include "TopExceptionScope.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/FileSystem.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The command line of `python3`. (Python/initconfig.c, Python/preconfig.c, Python/getopt.c and Modules/main.c in CPython are the
// reference.)

Configuration& configuration()
{
    static NeverDestroyed<Configuration> configuration;
    return configuration.get();
}

// ---- Reading: nothing here needs an interpreter ----

// What is not UTF-8 comes out as surrogate escapes, and goes back to the system as the bytes it was.
static String textFromSystem(const char* text)
{
    return decodeFilesystem(byteCast<uint8_t>(unsafeSpan(text)));
}

static void printToStderr(const String& text)
{
    auto bytes = encodeFilesystem(text);
    fwrite(bytes.span().data(), 1, bytes.size(), stderr);
}

// A variable that is set to nothing counts as not set.
static const char* environmentVariable(const Configuration& configuration, const char* name)
{
    if (!configuration.useEnvironment)
        return nullptr;
    const char* value = getenv(name);
    return value && *value ? value : nullptr;
}

static std::optional<int> parseInt(const char* text)
{
    char* end = nullptr;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (*end || errno == ERANGE || value < INT_MIN || value > INT_MAX)
        return std::nullopt;
    return static_cast<int>(value);
}

static std::optional<int> parseInt(const String& text)
{
    return parseInt(byteCast<char>(text.utf8().data()));
}

// PYTHONVERBOSE=2 is -vv; PYTHONVERBOSE=yes is -v.
static void raiseToEnvironmentFlag(const Configuration& configuration, int& flag, const char* name)
{
    const char* text = environmentVariable(configuration, name);
    if (!text)
        return;
    int value = parseInt(text).value_or(1);
    if (value < 0)
        value = 1;
    flag = std::max(flag, value);
}

static bool hasEnvironmentFlag(const Configuration& configuration, const char* name)
{
    int flag = 0;
    raiseToEnvironmentFlag(configuration, flag, name);
    return flag;
}

// The last -X name or -X name=value. Null: there is none. Without a value: the empty string, and `hasValue` is false.
static String xOption(const Configuration& configuration, ASCIILiteral name, bool* hasValue = nullptr)
{
    String result;
    for (auto& option : configuration.xOptions) {
        size_t separator = option.find('=');
        if (StringView(option).left(separator == notFound ? option.length() : separator) != name)
            continue;
        result = separator == notFound ? emptyString() : option.substring(separator + 1);
        if (hasValue)
            *hasValue = separator != notFound;
    }
    return result;
}

static int fatalConfigurationError(ASCIILiteral function, ASCIILiteral message, ASCIILiteral state = "preinitialized"_s)
{
    printToStderr(makeString("Fatal Python error: "_s, function, function.isEmpty() ? ""_s : ": "_s, message, "\nPython runtime state: "_s, state, "\n\n"_s));
    return 1;
}

// name=0 or name=1, from the environment and then from -X, which wins.
static bool readSwitch(const Configuration& configuration, int& result, const char* variable, ASCIILiteral option)
{
    if (const char* text = environmentVariable(configuration, variable)) {
        auto value = parseInt(text);
        if (!value || *value < 0 || *value > 1)
            return false;
        result = *value;
    }
    bool hasValue = false;
    if (String text = xOption(configuration, option, &hasValue); !text.isNull()) {
        auto value = hasValue ? parseInt(text) : std::nullopt;
        if (!value || *value < 0 || *value > 1)
            return false;
        result = *value;
    }
    return true;
}

// Everything but the options that have a letter of their own. -E, -I and -X have been seen to.
static std::optional<int> readEnvironmentAndXOptions(Configuration& configuration)
{
    if (configuration.isolated) {
        configuration.safePath = true;
        configuration.useEnvironment = false;
        configuration.userSiteDirectory = false;
    }

    if (!xOption(configuration, "no_debug_ranges"_s).isNull() || environmentVariable(configuration, "PYTHONNODEBUGRANGES"))
        configuration.codeDebugRanges = false;
    if (!xOption(configuration, "dev"_s).isNull() || environmentVariable(configuration, "PYTHONDEVMODE"))
        configuration.devMode = true;
    if (!xOption(configuration, "warn_default_encoding"_s).isNull() || environmentVariable(configuration, "PYTHONWARNDEFAULTENCODING"))
        configuration.warnDefaultEncoding = true;

    bool hasValue = false;
    if (String text = xOption(configuration, "utf8"_s, &hasValue); !text.isNull()) {
        if (!hasValue || text == "1"_s)
            configuration.utf8Mode = true;
        else if (text == "0"_s)
            configuration.utf8Mode = false;
        else
            return fatalConfigurationError("preconfig_init_utf8_mode"_s, "invalid -X utf8 option value"_s, "preinitializing"_s);
    } else if (const char* value = environmentVariable(configuration, "PYTHONUTF8")) {
        if (!strcmp(value, "1"))
            configuration.utf8Mode = true;
        else if (!strcmp(value, "0"))
            configuration.utf8Mode = false;
        else
            return fatalConfigurationError("preconfig_init_utf8_mode"_s, "invalid PYTHONUTF8 environment variable value"_s, "preinitializing"_s);
    }

    raiseToEnvironmentFlag(configuration, configuration.parserDebug, "PYTHONDEBUG");
    raiseToEnvironmentFlag(configuration, configuration.verbose, "PYTHONVERBOSE");
    raiseToEnvironmentFlag(configuration, configuration.optimizationLevel, "PYTHONOPTIMIZE");
    if (!configuration.inspect && environmentVariable(configuration, "PYTHONINSPECT"))
        configuration.inspect = 1;
    if (hasEnvironmentFlag(configuration, "PYTHONDONTWRITEBYTECODE"))
        configuration.writeBytecode = false;
    if (hasEnvironmentFlag(configuration, "PYTHONNOUSERSITE"))
        configuration.userSiteDirectory = false;
    if (hasEnvironmentFlag(configuration, "PYTHONUNBUFFERED"))
        configuration.bufferedStdio = false;
    if (environmentVariable(configuration, "PYTHONSAFEPATH"))
        configuration.safePath = true;
    if (const char* value = environmentVariable(configuration, "PYTHONPATH"))
        configuration.pythonPath = textFromSystem(value);
    if (const char* value = environmentVariable(configuration, "PYTHONHOME"))
        configuration.home = textFromSystem(value);
    if (const char* value = environmentVariable(configuration, "PYTHONPLATLIBDIR"))
        configuration.platlibdir = textFromSystem(value);

    if (const char* seed = environmentVariable(configuration, "PYTHONHASHSEED"); seed && strcmp(seed, "random")) {
        char* end = nullptr;
        errno = 0;
        unsigned long value = strtoul(seed, &end, 10);
        if (*end || value > 4294967295UL || (errno == ERANGE && value == ULONG_MAX))
            return fatalConfigurationError("config_init_hash_seed"_s, "PYTHONHASHSEED must be \"random\" or an integer in range [0; 4294967295]"_s);
        configuration.hashRandomization = !!value;
    }

    auto readGIL = [&](StringView value) -> std::optional<int> {
        if (value == "0"_s)
            return fatalConfigurationError("config_read_gil"_s, "Disabling the GIL is not supported by this build"_s);
        if (value != "1"_s)
            return fatalConfigurationError("config_read_gil"_s, "PYTHON_GIL / -X gil must be \"0\" or \"1\""_s);
        return std::nullopt;
    };
    if (const char* value = environmentVariable(configuration, "PYTHON_GIL")) {
        if (auto status = readGIL(StringView::fromLatin1(value)))
            return status;
    }
    if (String value = xOption(configuration, "gil"_s); !value.isNull()) {
        if (auto status = readGIL(value))
            return status;
    }

    if (!xOption(configuration, "faulthandler"_s).isNull() || environmentVariable(configuration, "PYTHONFAULTHANDLER") || configuration.devMode)
        configuration.faulthandler = true;

    if (const char* text = environmentVariable(configuration, "PYTHONPROFILEIMPORTTIME")) {
        configuration.importTime = parseInt(text).value_or(1);
        if (configuration.importTime < 0 || configuration.importTime > 2)
            return fatalConfigurationError("config_init_import_time"_s, "PYTHONPROFILEIMPORTTIME: numeric values other than 1 and 2 are reserved for future use."_s);
    }
    if (String text = xOption(configuration, "importtime"_s); !text.isNull()) {
        configuration.importTime = text.isEmpty() ? 1 : parseInt(text).value_or(1);
        if (configuration.importTime < 0 || configuration.importTime > 2)
            return fatalConfigurationError("config_init_import_time"_s, "-X importtime: values other than 1 and 2 are reserved for future use."_s);
    }

    auto readLazyImports = [&](StringView value) {
        if (value == "all"_s)
            configuration.lazyImports = 1;
        else if (value == "normal"_s)
            configuration.lazyImports = -1;
        else
            return false;
        return true;
    };
    if (const char* value = environmentVariable(configuration, "PYTHON_LAZY_IMPORTS"); value && !readLazyImports(StringView::fromLatin1(value)))
        return fatalConfigurationError("config_init_lazy_imports"_s, "PYTHON_LAZY_IMPORTS: invalid value; expected 'all' or 'normal'"_s);
    if (String value = xOption(configuration, "lazy_imports"_s); !value.isNull() && !readLazyImports(value))
        return fatalConfigurationError("config_init_lazy_imports"_s, "-X lazy_imports: invalid value; expected 'all' or 'normal'"_s);

    if (const char* text = environmentVariable(configuration, "PYTHONTRACEMALLOC")) {
        auto frames = parseInt(text);
        if (!frames || *frames < 0 || *frames > 65535)
            return fatalConfigurationError("config_init_tracemalloc"_s, "PYTHONTRACEMALLOC: invalid number of frames"_s);
        configuration.tracemalloc = *frames;
    }
    if (String text = xOption(configuration, "tracemalloc"_s, &hasValue); !text.isNull()) {
        auto frames = hasValue ? parseInt(text) : std::optional<int>(1);
        if (!frames || *frames < 0 || *frames > 65535)
            return fatalConfigurationError("config_init_tracemalloc"_s, "-X tracemalloc=NFRAME: invalid number of frames"_s);
        configuration.tracemalloc = *frames;
    }

    auto isDigitLimit = [](std::optional<int> limit) { return limit && (!*limit || *limit >= 640); };
    if (const char* text = environmentVariable(configuration, "PYTHONINTMAXSTRDIGITS")) {
        auto limit = parseInt(text);
        if (!isDigitLimit(limit))
            return fatalConfigurationError("config_init_int_max_str_digits"_s, "PYTHONINTMAXSTRDIGITS: invalid limit; must be >= 640 or 0 for unlimited."_s);
        configuration.intMaxStrDigits = *limit;
    }
    if (String text = xOption(configuration, "int_max_str_digits"_s, &hasValue); !text.isNull()) {
        auto limit = hasValue ? parseInt(text) : std::nullopt;
        if (!isDigitLimit(limit))
            return fatalConfigurationError("config_init_int_max_str_digits"_s, "-X int_max_str_digits: invalid limit; must be >= 640 or 0 for unlimited."_s);
        configuration.intMaxStrDigits = *limit;
    }

    auto readCPUCount = [&](const String& text) {
        if (text == "default"_s) {
            configuration.cpuCount = -1;
            return true;
        }
        auto count = parseInt(text);
        if (!count || *count < 1)
            return false;
        configuration.cpuCount = *count;
        return true;
    };
    constexpr auto cpuCountMessage = "-X cpu_count=n option: n is missing or an invalid number, n must be greater than 0"_s;
    if (const char* text = environmentVariable(configuration, "PYTHON_CPU_COUNT"); text && !readCPUCount(textFromSystem(text)))
        return fatalConfigurationError("config_init_cpu_count"_s, cpuCountMessage);
    if (String text = xOption(configuration, "cpu_count"_s, &hasValue); !text.isNull() && (!hasValue || !readCPUCount(text)))
        return fatalConfigurationError("config_init_cpu_count"_s, cpuCountMessage);

    if (String text = xOption(configuration, "pycache_prefix"_s); !text.isNull())
        configuration.pycachePrefix = text.isEmpty() ? String() : text;
    else if (const char* value = environmentVariable(configuration, "PYTHONPYCACHEPREFIX"))
        configuration.pycachePrefix = textFromSystem(value);

    if (!readSwitch(configuration, configuration.threadInheritContext, "PYTHON_THREAD_INHERIT_CONTEXT", "thread_inherit_context"_s))
        return fatalConfigurationError("config_init_thread_inherit_context"_s, "-X thread_inherit_context=n: n is missing or invalid"_s);
    if (!readSwitch(configuration, configuration.contextAwareWarnings, "PYTHON_CONTEXT_AWARE_WARNINGS", "context_aware_warnings"_s))
        return fatalConfigurationError("config_init_context_aware_warnings"_s, "-X context_aware_warnings=n: n is missing or invalid"_s);
    int ignored = 1;
    if (!readSwitch(configuration, ignored, "PYTHON_PATHCONFIG_WARNINGS", "pathconfig_warnings"_s))
        return fatalConfigurationError("config_init_pathconfig_warnings"_s, "-X pathconfig_warnings=n: n is missing or invalid"_s);

    auto isOnOrOff = [](StringView value) { return value.isEmpty() || value == "on"_s || value == "off"_s; };
    if (const char* value = environmentVariable(configuration, "PYTHON_FROZEN_MODULES")) {
        if (!isOnOrOff(StringView::fromLatin1(value)))
            return fatalConfigurationError(""_s, "bad value for PYTHON_FROZEN_MODULES (expected \"on\" or \"off\")"_s);
        configuration.useFrozenModules = StringView::fromLatin1(value) != "off"_s;
    }
    if (String value = xOption(configuration, "frozen_modules"_s); !value.isNull()) {
        if (!isOnOrOff(value))
            return fatalConfigurationError(""_s, "bad value for option -X frozen_modules (expected \"on\" or \"off\")"_s);
        configuration.useFrozenModules = value != "off"_s;
    }

    // encoding:errors, either of which may be left out.
    if (const char* value = environmentVariable(configuration, "PYTHONIOENCODING")) {
        String text = textFromSystem(value);
        size_t colon = text.find(':');
        String encoding = colon == notFound ? text : text.left(colon);
        String errors = colon == notFound ? String() : text.substring(colon + 1);
        if (!encoding.isEmpty()) {
            configuration.stdioEncoding = encoding;
            if (errors.isEmpty())
                errors = "strict"_s;
        }
        if (!errors.isEmpty())
            configuration.stdioErrors = errors;
    }
    return std::nullopt;
}

// sys.warnoptions. The filter that is added last is asked first, so what counts most comes last.
static void collectWarningOptions(Configuration& configuration, const Vector<String>& fromCommandLine)
{
    Vector<String> options;
    auto add = [&](const String& option) {
        if (!options.contains(option))
            options.append(option);
    };
    if (configuration.devMode)
        add("default"_s);
    if (const char* value = environmentVariable(configuration, "PYTHONWARNINGS")) {
        for (auto part : textFromSystem(value).split(','))
            add(part);
    }
    for (auto& option : fromCommandLine)
        add(option);
    if (configuration.bytesWarning)
        add(configuration.bytesWarning > 1 ? "error::BytesWarning"_s : "default::BytesWarning"_s);
    configuration.warnOptions = WTF::move(options);
}

std::optional<int> readEnvironment(Configuration& configuration)
{
    if (auto status = readEnvironmentAndXOptions(configuration))
        return status;
    collectWarningOptions(configuration, { });
    return std::nullopt;
}

namespace {

// getopt(), as far as python3 goes.
class Options {
public:
    static constexpr int end = -1;
    static constexpr int mistake = '_';
    enum Long : int { CheckHashBasedPycs = 0, HelpAll = 1, HelpEnvironment = 2, HelpXOptions = 3 };

    Options(std::span<const char* const> arguments, bool reportsMistakes)
        : m_arguments(arguments)
        , m_reportsMistakes(reportsMistakes)
    {
    }

    size_t index() const { return m_index; }
    const char* argument() const { return m_argument; }

    int next()
    {
        if (!*m_rest) {
            if (m_index >= m_arguments.size())
                return end;
            const char* current = m_arguments[m_index];
            if (current[0] != '-' || !current[1])
                return end;
            if (!strcmp(current, "--")) {
                ++m_index;
                return end;
            }
            if (!strcmp(current, "--help")) {
                ++m_index;
                return 'h';
            }
            if (!strcmp(current, "--version")) {
                ++m_index;
                return 'V';
            }
            m_rest = current + 1;
            ++m_index;
        }

        char option = *m_rest++;
        if (!option)
            return end;

        if (option == '-') {
            static constexpr struct {
                const char* name;
                bool takesArgument;
                int value;
            } longOptions[] = {
                { "check-hash-based-pycs", true, CheckHashBasedPycs },
                { "help-all", false, HelpAll },
                { "help-env", false, HelpEnvironment },
                { "help-xoptions", false, HelpXOptions },
            };
            const char* name = std::exchange(m_rest, "");
            for (auto& candidate : longOptions) {
                if (strcmp(candidate.name, name))
                    continue;
                if (!candidate.takesArgument)
                    return candidate.value;
                if (m_index >= m_arguments.size()) {
                    report("Argument expected for the "_s, m_arguments[m_index - 1], " options\n"_s);
                    return mistake;
                }
                m_argument = m_arguments[m_index++];
                return candidate.value;
            }
            report("Unknown option: "_s, m_arguments[m_index - 1], "\n"_s);
            return mistake;
        }

        const char* known = strchr("bBc:dEhiIm:OPqRsStuvVW:xX:?", option);
        if (!known || option == ':') {
            char text[] = { '-', option, 0 };
            report("Unknown option: "_s, text, "\n"_s);
            return mistake;
        }
        if (known[1] == ':') {
            if (*m_rest)
                m_argument = std::exchange(m_rest, "");
            else if (m_index < m_arguments.size())
                m_argument = m_arguments[m_index++];
            else {
                char text[] = { '-', option, 0 };
                report("Argument expected for the "_s, text, " option\n"_s);
                return mistake;
            }
        }
        return option;
    }

private:
    void report(ASCIILiteral before, const char* what, ASCIILiteral after)
    {
        if (m_reportsMistakes)
            printToStderr(makeString(before, textFromSystem(what), after));
    }

    std::span<const char* const> m_arguments;
    bool m_reportsMistakes;
    size_t m_index { 1 };
    const char* m_rest { "" };
    const char* m_argument { nullptr };
};

} // namespace

static String usageLine(const String& program)
{
    return makeString("usage: "_s, program, " [option] ... [-c cmd | -m mod | file | -] [arg] ...\n"_s);
}

static constexpr ASCIILiteral optionsHelp =
    "Options (and the environment variables that do the same):\n"
    "-b     : warn when bytes are compared with or converted to str (-bb: raise)\n"
    "-B     : sys.dont_write_bytecode is true; also PYTHONDONTWRITEBYTECODE=x\n"
    "-c cmd : run the program given as a string (the last option)\n"
    "-d     : accepted; there is no parser debugging output; also PYTHONDEBUG=x\n"
    "-E     : ignore the PYTHON* environment variables\n"
    "-h     : print this and exit (also -? or --help)\n"
    "-i     : go on interactively when the program has run; also PYTHONINSPECT=x\n"
    "-I     : keep the user's environment out (implies -E, -P and -s)\n"
    "-m mod : run a module of the library as the program (the last option)\n"
    "-O     : leave out assert statements and code under `if __debug__`;\n"
    "         also PYTHONOPTIMIZE=x\n"
    "-OO    : as -O, and leave out docstrings\n"
    "-P     : do not put the directory of the program first on sys.path;\n"
    "         also PYTHONSAFEPATH\n"
    "-q     : no version and copyright lines when starting interactively\n"
    "-s     : do not add the user's site directory to sys.path;\n"
    "         also PYTHONNOUSERSITE=x\n"
    "-S     : do not import site at the start\n"
    "-u     : do not buffer stdout and stderr; also PYTHONUNBUFFERED=x\n"
    "-v     : say what is imported, and from where; also PYTHONVERBOSE=x\n"
    "-V     : print the version of Python and exit (also --version);\n"
    "         twice, say more about the build\n"
    "-W arg : filter warnings; arg is action:message:category:module:lineno;\n"
    "         also PYTHONWARNINGS=arg\n"
    "-x     : skip the first line of the program\n"
    "-X opt : an option of the implementation; see --help-xoptions\n"
    "--check-hash-based-pycs always|default|never:\n"
    "         accepted; there are no .pyc files\n"
    "--help-env: print the environment variables that matter and exit\n"
    "--help-xoptions: print the -X options and exit\n"
    "--help-all: print all of the help and exit\n"
    "\n"
    "Arguments:\n"
    "file   : the program is read from this file\n"
    "-      : the program is read from stdin (the default; interactive on a terminal)\n"
    "arg ...: what the program finds in sys.argv[1:]\n"_s;

static constexpr ASCIILiteral xOptionsHelp =
    "The -X options:\n"
    "-X context_aware_warnings=[0|1]: sys.flags.context_aware_warnings;\n"
    "         also PYTHON_CONTEXT_AWARE_WARNINGS\n"
    "-X cpu_count=N: what os.cpu_count() returns; `default` is what the system says;\n"
    "         also PYTHON_CPU_COUNT\n"
    "-X dev : development mode; also PYTHONDEVMODE\n"
    "-X faulthandler: enable faulthandler at the start; also PYTHONFAULTHANDLER\n"
    "-X importtime[=2]: say how long each import takes;\n"
    "         also PYTHONPROFILEIMPORTTIME\n"
    "-X int_max_str_digits=N: the limit on conversions between int and str;\n"
    "         0 is no limit; also PYTHONINTMAXSTRDIGITS\n"
    "-X lazy_imports=[all|normal]: whether every import is lazy;\n"
    "         also PYTHON_LAZY_IMPORTS\n"
    "-X pycache_prefix=PATH: sys.pycache_prefix; also PYTHONPYCACHEPREFIX\n"
    "-X thread_inherit_context=[0|1]: sys.flags.thread_inherit_context;\n"
    "         also PYTHON_THREAD_INHERIT_CONTEXT\n"
    "-X tracemalloc[=N]: start tracemalloc; also PYTHONTRACEMALLOC=N\n"
    "-X utf8[=0|1]: sys.flags.utf8_mode; also PYTHONUTF8\n"
    "-X warn_default_encoding: sys.flags.warn_default_encoding;\n"
    "         also PYTHONWARNDEFAULTENCODING\n"
    "Any other is accepted, and is in sys._xoptions.\n"_s;

static constexpr ASCIILiteral environmentHelp =
    "Environment variables:\n"
    "PYTHONASYNCIODEBUG: the debug mode of asyncio\n"
    "PYTHON_BASIC_REPL: use the plain interactive loop\n"
    "PYTHONBREAKPOINT: what breakpoint() calls; 0 makes it do nothing\n"
    "PYTHON_COLORS   : 1 for colors in what the interpreter prints, 0 for none\n"
    "PYTHONHASHSEED  : `random`, or an integer in [0,4294967295]\n"
    "PYTHON_HISTORY  : where the history of the interactive interpreter is kept\n"
    "PYTHONHOME      : the directory whose lib/python3.15 is the standard library\n"
    "PYTHONIOENCODING: encoding[:errors] of stdin, stdout and stderr\n"
    "PYTHONPATH      : directories, separated by ':', that come first on sys.path\n"
    "PYTHONPLATLIBDIR: sys.platlibdir\n"
    "PYTHONSTARTUP   : a file that is run before the first interactive prompt\n"
    "PYTHONUSERBASE  : site.USER_BASE\n"
    "\n"
    "Those that do what an option does are listed with the options (--help,\n"
    "--help-xoptions).\n"_s;

static int usageMistake(const String& program)
{
    printToStderr(makeString(usageLine(program), "Try `python -h' for more information.\n"_s));
    return 2;
}

static void printToStdout(const String& text)
{
    auto utf8 = text.utf8();
    fwrite(utf8.data(), 1, utf8.length(), stdout);
    fflush(stdout);
}

std::optional<int> readCommandLine(Configuration& configuration, std::span<const char* const> arguments)
{
    configuration.originalArguments.clear();
    for (const char* argument : arguments)
        configuration.originalArguments.append(textFromSystem(argument));
    configuration.programName = !arguments.empty() && *arguments[0] ? configuration.originalArguments[0] : "python3"_str;
    const String& program = configuration.programName;

    // What decides how the rest is read comes first.
    {
        Options options(arguments, false);
        while (true) {
            int option = options.next();
            if (option == Options::end || option == 'c' || option == 'm')
                break;
            if (option == 'E')
                configuration.useEnvironment = false;
            else if (option == 'I')
                configuration.isolated = true;
            else if (option == 'X')
                configuration.xOptions.append(textFromSystem(options.argument()));
        }
    }

    Vector<String> warnOptions;
    int printsVersion = 0;
    Options options(arguments, true);
    while (true) {
        int option = options.next();
        if (option == Options::end)
            break;
        // The last option: what follows is the program's, whatever it looks like.
        if (option == 'c') {
            configuration.runCommand = makeString(textFromSystem(options.argument()), '\n');
            break;
        }
        if (option == 'm') {
            configuration.runModule = textFromSystem(options.argument());
            break;
        }
        switch (option) {
        case Options::CheckHashBasedPycs:
            configuration.checkHashPycsMode = textFromSystem(options.argument());
            if (configuration.checkHashPycsMode != "always"_s && configuration.checkHashPycsMode != "never"_s && configuration.checkHashPycsMode != "default"_s) {
                printToStderr("--check-hash-based-pycs must be one of 'default', 'always', or 'never'\n"_s);
                return usageMistake(program);
            }
            break;
        case Options::HelpAll:
            printToStdout(makeString(usageLine(program), optionsHelp, '\n', environmentHelp, '\n', xOptionsHelp));
            return 0;
        case Options::HelpEnvironment:
            printToStdout(environmentHelp);
            return 0;
        case Options::HelpXOptions:
            printToStdout(xOptionsHelp);
            return 0;
        case 'h':
        case '?':
            printToStdout(makeString(usageLine(program), optionsHelp));
            return 0;
        case 'b':
            ++configuration.bytesWarning;
            break;
        case 'd':
            ++configuration.parserDebug;
            break;
        case 'i':
            ++configuration.inspect;
            ++configuration.interactive;
            break;
        case 'E':
        case 'I':
        case 'X':
        case 't':
            break;
        case 'O':
            ++configuration.optimizationLevel;
            break;
        case 'P':
            configuration.safePath = true;
            break;
        case 'B':
            configuration.writeBytecode = false;
            break;
        case 's':
            configuration.userSiteDirectory = false;
            break;
        case 'S':
            configuration.siteImport = false;
            break;
        case 'u':
            configuration.bufferedStdio = false;
            break;
        case 'v':
            ++configuration.verbose;
            break;
        case 'x':
            configuration.skipSourceFirstLine = true;
            break;
        case 'V':
            ++printsVersion;
            break;
        case 'W':
            warnOptions.append(textFromSystem(options.argument()));
            break;
        case 'q':
            ++configuration.quiet;
            break;
        case 'R':
            configuration.hashRandomization = true;
            break;
        default:
            return usageMistake(program);
        }
    }

    if (printsVersion) {
        printToStdout(makeString("Python "_s, printsVersion >= 2 ? versionText() : shortVersionText(), '\n'));
        return 0;
    }

    size_t index = options.index();
    if (configuration.runCommand.isNull() && configuration.runModule.isNull() && index < arguments.size() && strcmp(arguments[index], "-"))
        configuration.runFilename = configuration.originalArguments[index];

    // sys.argv starts with the file as it was given, or with what stands for it.
    configuration.arguments.clear();
    if (!configuration.runCommand.isNull() || !configuration.runModule.isNull())
        --index;
    for (size_t i = index; i < arguments.size(); ++i)
        configuration.arguments.append(configuration.originalArguments[i]);
    if (configuration.arguments.isEmpty())
        configuration.arguments.append(emptyString());
    if (!configuration.runCommand.isNull())
        configuration.arguments[0] = "-c"_s;
    else if (!configuration.runModule.isNull())
        configuration.arguments[0] = "-m"_s;

    if (auto status = readEnvironmentAndXOptions(configuration))
        return status;
    collectWarningOptions(configuration, warnOptions);
    return std::nullopt;
}

// ---- Between the start of the import system and the program ----

static JSValue callWithKeywords(JSGlobalObject* globalObject, JSValue callable, std::initializer_list<std::pair<ASCIILiteral, JSValue>> keywords)
{
    Runtime& rt = runtime(globalObject);
    PyDict* dict = PyDict::create(globalObject);
    for (auto& [name, value] : keywords)
        dict->set(rt.vm(), internString(rt.vm(), name), value);
    return callWithTupleAndDict(globalObject, callable, rt.emptyTuple(), dict);
}

// ---- -X importtime ----

static bool s_importTimeHeaderIsDue = true;
static int s_importLevel;
static double s_importTimeAccumulated; // In what the module that is being imported has imported so far.

static void printImportTimeHeader()
{
    if (std::exchange(s_importTimeHeaderIsDue, false))
        printToStderr("import time: self [us] | cumulative | imported package\n"_s);
}

void ImportTimeReport::start(const String& moduleName)
{
    printImportTimeHeader();
    m_name = moduleName;
    ++s_importLevel;
    m_accumulatedBefore = std::exchange(s_importTimeAccumulated, 0);
    m_start = MonotonicTime::now().secondsSinceEpoch().microseconds();
}

void ImportTimeReport::finish()
{
    double cumulative = MonotonicTime::now().secondsSinceEpoch().microseconds() - m_start;
    --s_importLevel;
    char numbers[64];
    snprintf(numbers, sizeof(numbers), "import time: %9ld | %10ld | ", static_cast<long>(std::ceil(cumulative - s_importTimeAccumulated)), static_cast<long>(std::ceil(cumulative)));
    StrBuilder line;
    line.append(String::fromLatin1(numbers));
    for (int i = 0; i < s_importLevel * 2; ++i)
        line.append(' ');
    line.append(m_name, '\n');
    printToStderr(line.toString());
    s_importTimeAccumulated = m_accumulatedBefore + cumulative;
}

void ImportTimeReport::reportCached(const String& moduleName)
{
    printImportTimeHeader();
    StrBuilder line;
    line.append("import time: cached    | cached     | "_s);
    for (int i = moduleName.length(); i < s_importLevel * 2; ++i)
        line.append(' ');
    line.append(moduleName, '\n');
    printToStderr(line.toString());
}

PyModule* ensureMainModule(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (auto* existing = PyModule::tryFrom(rt.modules()->get(rt.names.dMain())))
        return existing;
    PyModule* main = PyModule::create(globalObject, rt.names.dMain());
    rt.modules()->set(vm, rt.names.dMain(), main);
    main->dict()->set(vm, rt.names.dBuiltins(), rt.builtinsModule());
    if (auto* bootstrap = PyModule::tryFrom(rt.modules()->get(internString(vm, "_frozen_importlib"_s)))) {
        if (JSValue importer = bootstrap->dict()->get(internString(vm, "BuiltinImporter"_s)))
            main->dict()->set(vm, rt.names.dLoader(), importer);
    }
    return main;
}

void startMainInterpreter(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Configuration& configuration = Python::configuration();

    if (configuration.faulthandler) {
        JSValue module = importModule(globalObject, "faulthandler"_s);
        RETURN_IF_EXCEPTION(scope, void());
        callMethod(globalObject, module, internString(vm, "enable"_s));
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (configuration.tracemalloc) {
        JSValue module = importModule(globalObject, "_tracemalloc"_s);
        RETURN_IF_EXCEPTION(scope, void());
        callMethod(globalObject, module, internString(vm, "start"_s), jsNumber(configuration.tracemalloc));
        RETURN_IF_EXCEPTION(scope, void());
    }

    // The streams were made before there was a way to find any codec but those written in C++.
    if (!configuration.stdioEncoding.isNull() || !configuration.stdioErrors.isNull()) {
        JSValue encoding = jsUndefined();
        if (!configuration.stdioEncoding.isNull()) {
            // Under the name the codec gives itself.
            JSValue codecs = importModule(globalObject, "_codecs"_s);
            RETURN_IF_EXCEPTION(scope, void());
            JSValue info = callMethod(globalObject, codecs, internString(vm, "lookup"_s), makeStr(vm, configuration.stdioEncoding));
            RETURN_IF_EXCEPTION(scope, void());
            encoding = getAttr(globalObject, info, rt.names.name());
            RETURN_IF_EXCEPTION(scope, void());
        }
        for (JSString* name : { rt.names.stdin_(), rt.names.stdout_(), rt.names.stderr_() }) {
            JSValue stream = rt.sysDict()->get(name);
            if (!stream || isNone(stream))
                continue;
            JSValue reconfigure = getAttr(globalObject, stream, internString(vm, "reconfigure"_s));
            RETURN_IF_EXCEPTION(scope, void());
            JSValue errors = name == rt.names.stderr_() ? JSValue(makeStr(vm, "backslashreplace"_s)) : makeStr(vm, configuration.stdioErrors.isNull() ? "surrogateescape"_str : configuration.stdioErrors);
            callWithKeywords(globalObject, reconfigure, { { "encoding"_s, encoding }, { "errors"_s, errors } });
            RETURN_IF_EXCEPTION(scope, void());
        }
    }

    ensureMainModule(globalObject);

    // It reads sys.warnoptions when it is imported.
    if (!configuration.warnOptions.isEmpty()) {
        importModule(globalObject, "warnings"_s);
        RETURN_IF_EXCEPTION(scope, void());
    }

    if (configuration.siteImport) {
        scope.release();
        importModule(globalObject, "site"_s);
    }
}

// ---- Running ----

static bool stdinIsInteractive()
{
    return isatty(STDIN_FILENO) || configuration().interactive;
}

static bool runsCode()
{
    Configuration& configuration = Python::configuration();
    return !configuration.runCommand.isNull() || !configuration.runFilename.isNull() || !configuration.runModule.isNull();
}

static String currentDirectory()
{
    char buffer[PATH_MAX];
    return getcwd(buffer, sizeof(buffer)) ? textFromSystem(buffer) : String();
}

// What comes first on sys.path, going by sys.argv[0]. Null: nothing.
static String computeFirstSearchPath(const Vector<String>& arguments)
{
    if (arguments.isEmpty())
        return { };
    const String& first = arguments[0];
    if (first == "-m"_s)
        return currentDirectory();
    if (first == "-c"_s)
        return emptyString();
    // The directory the script is really in.
    String path = realPath(first);
    if (path.isNull())
        path = first;
    size_t slash = path.reverseFind('/');
    if (slash == notFound)
        return emptyString();
    return path.left(std::max<size_t>(slash, 1));
}

// sys.path_importer_cache[path], which is filled in from sys.path_hooks. None: nothing can import from there.
static JSValue importerFor(JSGlobalObject* globalObject, JSValue path)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* cache = PyDict::tryFrom(rt.sysDict()->get(internString(vm, "path_importer_cache"_s)));
    JSValue hooks = rt.sysDict()->get(internString(vm, "path_hooks"_s));
    if (!cache || !hooks)
        return jsUndefined();
    JSValue cached = cache->get(globalObject, path);
    RETURN_IF_EXCEPTION(scope, { });
    if (cached)
        return cached;
    cache->set(globalObject, path, jsUndefined());
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* list = listFromIterable(globalObject, hooks);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; i < listLength(list); ++i) {
        JSValue importer = call(globalObject, listGet(list, i), path);
        if (scope.exception()) {
            if (!clearPendingExceptionIfMatches(globalObject, rt.ImportError()))
                return { };
            continue;
        }
        cache->set(globalObject, path, importer);
        RETURN_IF_EXCEPTION(scope, { });
        return importer;
    }
    return jsUndefined();
}

// Whether it is worth asking the path hooks, which means importing zipimport: a file that is no archive is a script.
static bool mayBeImportPath(const String& path)
{
    if (kindOfFile(path) != VirtualFileKind::File)
        return true;
    auto contents = readFile(path);
    if (!contents)
        return true;
    // An archive ends with a record that starts with this, and a comment of less than 64K.
    static constexpr uint8_t signature[] = { 'P', 'K', 5, 6 };
    auto bytes = contents->span();
    if (bytes.size() > 65536 + 22)
        bytes = bytes.last(65536 + 22);
    return !!memmem(bytes.data(), bytes.size(), signature, sizeof(signature));
}

// textwrap.dedent(), more or less: what all lines that are not blank start with is taken off, and blank lines are emptied.
static String dedent(const String& text)
{
    Vector<String> lines = text.splitAllowingEmptyEntries('\n');
    auto isBlank = [](const String& line) {
        for (unsigned i = 0; i < line.length(); ++i) {
            if (line[i] != ' ' && line[i] != '\t')
                return false;
        }
        return true;
    };
    std::optional<String> margin;
    for (auto& line : lines) {
        if (isBlank(line))
            continue;
        unsigned indentation = 0;
        while (indentation < line.length() && (line[indentation] == ' ' || line[indentation] == '\t'))
            ++indentation;
        if (!margin) {
            margin = line.left(indentation);
            continue;
        }
        unsigned common = 0;
        while (common < indentation && common < margin->length() && line[common] == (*margin)[common])
            ++common;
        margin = margin->left(common);
    }
    if (!margin || margin->isEmpty())
        return text;
    StrBuilder result;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i)
            result.append('\n');
        if (!isBlank(lines[i]))
            result.append(StringView(lines[i]).substring(margin->length()));
    }
    return result.toString();
}

static void runCommand(JSGlobalObject* globalObject, const String& command, JSValue* unfinished)
{
    auto utf8 = dedent(command).utf8();
    runSourceInMain(globalObject, byteCast<uint8_t>(utf8.span()), "<string>"_s, CompileIgnoreCookie | CompileAllowTopLevelAwait | CompileSourceIsShown, unfinished);
}

// runpy._run_module_as_main(name, changesFirstArgument)
static void runModule(JSGlobalObject* globalObject, const String& name, bool changesFirstArgument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue runpy = importModule(globalObject, "runpy"_s);
    JSValue function = runpy ? getAttr(globalObject, runpy, internString(vm, "_run_module_as_main"_s)) : JSValue();
    if (scope.exception()) [[unlikely]] {
        printToStderr("Could not import runpy._run_module_as_main\n"_s);
        return;
    }
    scope.release();
    call(globalObject, function, makeStr(vm, name), jsBoolean(changesFirstArgument));
}

static void setMainLoader(JSGlobalObject* globalObject, PyDict* globals, const String& path, ASCIILiteral type = "SourceFileLoader"_s)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue external = rt.modules()->get(internString(vm, "_frozen_importlib_external"_s));
    if (!external)
        return;
    JSValue loader = callMethod(globalObject, external, internString(vm, type), rt.names.dMain(), makeStr(vm, path));
    RETURN_IF_EXCEPTION(scope, void());
    globals->set(vm, rt.names.dLoader(), loader);
}

void runFileInMain(JSGlobalObject* globalObject, const String& path, std::span<const uint8_t> source, JSValue* unfinished)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* globals = ensureMainModule(globalObject)->dict();
    globals->set(vm, rt.names.dFile(), makeStr(vm, path));
    setMainLoader(globalObject, globals, path);
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    runSourceInMain(globalObject, source, path, CompileAllowTopLevelAwait | CompileSourceIsFile, unfinished);
}

// A file of compiled code is a program too.
static void runPycFileInMain(JSGlobalObject* globalObject, const String& path, std::span<const uint8_t> contents, JSValue* unfinished)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* globals = ensureMainModule(globalObject)->dict();
    globals->set(vm, rt.names.dFile(), makeStr(vm, path));
    setMainLoader(globalObject, globals, path, "SourcelessFileLoader"_s);
    RETURN_IF_EXCEPTION(scope, void());
    if (!hasPycMagicNumber(contents)) {
        raiseRuntimeError(globalObject, scope, "Bad magic number in .pyc file"_s);
        return;
    }
    PyCode* code = codeInPycFile(globalObject, contents);
    if (!code) {
        raiseRuntimeError(globalObject, scope, "Bad code object in .pyc file"_s);
        return;
    }
    RELEASE_AND_RETURN(scope, runCodeInMain(globalObject, code, unfinished));
}

// The exit status, if the file could not even be run.
static std::optional<int> runFile(JSGlobalObject* globalObject, const String& path, JSValue* unfinished)
{
    Configuration& configuration = Python::configuration();
    auto contents = readFile(path);
    if (!contents) {
        int error = errno;
        if (error == EISDIR) {
            printToStderr(makeString(configuration.programName, ": "_s, strRepr(path), " is a directory, cannot continue\n"_s));
            return 1;
        }
        printToStderr(makeString(configuration.programName, ": can't open file "_s, strRepr(path), ": [Errno "_s, error, "] "_s, textFromSystem(strerror(error)), '\n'));
        return 2;
    }
    std::span<const uint8_t> source = contents->span();
    if (path.endsWith(".pyc"_s) || hasPycMagicNumber(source)) {
        runPycFileInMain(globalObject, path, source, unfinished);
        return std::nullopt;
    }
    if (configuration.skipSourceFirstLine) {
        // The end of the line stays, so that the numbers of the lines do.
        size_t end = 0;
        while (end < source.size() && source[end] != '\n')
            ++end;
        source = source.subspan(end);
    }
    runFileInMain(globalObject, path, source, unfinished);
    return std::nullopt;
}

static Vector<uint8_t> readAllOfStdin()
{
    Vector<uint8_t> bytes;
    uint8_t buffer[16384];
    while (true) {
        ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        bytes.append(std::span { buffer, static_cast<size_t>(count) });
    }
    return bytes;
}

// ---- The interactive interpreter ----

// One line from standard input, with its end. Empty: there is no more.
static Vector<uint8_t> readLineFromStdin(const String& prompt)
{
    printToStderr(prompt);
    fflush(stderr);
    Vector<uint8_t> line;
    while (true) {
        uint8_t byte;
        ssize_t count = read(STDIN_FILENO, &byte, 1);
        if (count < 0 && errno == EINTR) {
            // Ctrl-C: whoever called sees to it.
            line.clear();
            line.append('\x03');
            return line;
        }
        if (count <= 0)
            return line;
        line.append(byte);
        if (byte == '\n')
            return line;
    }
}

static String promptText(JSGlobalObject* globalObject, ASCIILiteral name)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue prompt = rt.sysDict()->get(internString(vm, name));
    if (!prompt)
        return emptyString();
    JSString* text = str(globalObject, prompt);
    String result = text ? String(text->value(globalObject)) : emptyString();
    scope.clearException();
    return result;
}

static void flushStandardStreams(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(rt.vm());
    for (JSString* name : { rt.names.stderr_(), rt.names.stdout_() }) {
        JSValue stream = rt.sysDict()->get(name);
        if (stream && !isNone(stream))
            callMethod(globalObject, stream, rt.names.flush());
        scope.clearException();
    }
}

// Reads statements from standard input and runs each as it is complete, until there are no more. No line editing.
// The exit status, if the program asked to exit.
std::optional<int> runBasicInteractiveLoop(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyDict* globals = ensureMainModule(globalObject)->dict();

    for (auto [name, text] : { std::pair { "ps1"_s, ">>> "_s }, std::pair { "ps2"_s, "... "_s } }) {
        JSString* key = internString(vm, name);
        if (!rt.sysDict()->get(key))
            rt.sysDict()->set(vm, key, makeStr(vm, text));
    }

    // codeop tells a statement that is not finished from one that is wrong.
    JSValue compiler;
    if (JSValue codeop = importModule(globalObject, "codeop"_s)) {
        compiler = callMethod(globalObject, codeop, internString(vm, "CommandCompiler"_s));
        if (compiler) {
            if (JSValue inner = getAttr(globalObject, compiler, internString(vm, "compiler"_s))) {
                JSValue flags = getAttr(globalObject, inner, internString(vm, "flags"_s));
                if (flags && flags.isInt32())
                    setAttr(globalObject, inner, internString(vm, "flags"_s), jsNumber(flags.asInt32() | CompileAllowTopLevelAwait));
            }
        }
    }
    if (scope.exception())
        return handleUncaughtException(globalObject);

    JSValue filename = makeStr(vm, "<stdin>"_s);
    JSValue mode = makeStr(vm, "single"_s);
    JSValue runner;
    Vector<uint8_t> pending;
    // A `from __future__ import` holds for the rest of the session.
    int compileFlags = CompileAllowTopLevelAwait | CompileSourceIsShown;
    while (true) {
        Vector<uint8_t> line = readLineFromStdin(promptText(globalObject, pending.isEmpty() ? "ps1"_s : "ps2"_s));
        bool isAtEnd = false;
        if (line.size() == 1 && line[0] == '\x03') {
            pending.clear();
            makeCallsOwed(vm);
            if (!scope.exception())
                continue;
        } else {
            isAtEnd = line.isEmpty();
            if (isAtEnd)
                printToStderr("\n"_s);
            if (isAtEnd && pending.isEmpty())
                break;
            pending.appendVector(line);
        }

        JSValue code;
        if (!scope.exception()) {
            if (isAtEnd)
                code = compileSource(globalObject, pending.span(), "<stdin>"_s, "single"_s, compileFlags, -1, globals->get(rt.names.dName()));
            else {
                // It takes the lines without the end of the last: that a block is over shows in the empty line after it.
                auto text = pending.span();
                if (!text.empty() && text.back() == '\n')
                    text = text.first(text.size() - 1);
                code = call(globalObject, compiler, makeStr(vm, String::fromUTF8ReplacingInvalidSequences(text)), filename, mode);
                if (code && isNone(code))
                    continue;
                // Once more, so that tracebacks can show the lines.
                if (code)
                    code = compileSource(globalObject, pending.span(), "<stdin>"_s, "single"_s, compileFlags, -1, globals->get(rt.names.dName()));
            }
            // Where the compiler was called from is of no interest.
            if (auto* exception = PyException::tryFrom(pendingException(globalObject)))
                exception->setTraceback(vm, JSValue());
        }
        if (code) {
            registerSourceWithLinecache(globalObject, code, pending.span(), "<stdin>"_s);
            compileFlags |= static_cast<int>((PyCode::from(code)->flags() & OptionSet<CodeFlag> { CodeFlag::FutureAnnotations, CodeFlag::FutureBarryAsBDFL }).toRaw());
        }
        pending.clear();

        if (code && !scope.exception()) {
            JSValue result = runCode(globalObject, code, globals, globals);
            if (result && PyCode::from(code)->flags().contains(CodeFlag::Coroutine)) {
                // One loop for the session, so that what one statement starts the next finds running.
                if (!runner) {
                    if (JSValue asyncio = importModule(globalObject, "asyncio"_s))
                        runner = callMethod(globalObject, asyncio, internString(vm, "Runner"_s));
                }
                if (runner)
                    callMethod(globalObject, runner, internString(vm, "run"_s), result);
            }
        }
        if (scope.exception()) {
            bool exits = pendingExceptionMatches(globalObject, rt.SystemExit());
            int status = handleUncaughtException(globalObject);
            if (exits)
                return status;
        }
        flushStandardStreams(globalObject);
    }
    if (runner) {
        callMethod(globalObject, runner, internString(vm, "close"_s));
        scope.clearException();
    }
    return std::nullopt;
}

// $PYTHONSTARTUP. The exit status, if it asked to exit.
static std::optional<int> runStartupFile(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    const char* value = environmentVariable(configuration(), "PYTHONSTARTUP");
    if (!value)
        return std::nullopt;
    String path = textFromSystem(value);
    auto contents = readFile(path);
    if (!contents) {
        int error = errno;
        printToStderr("Could not open PYTHONSTARTUP\n"_s);
        auto throwScope = DECLARE_THROW_SCOPE(vm);
        raiseOSError(globalObject, throwScope, error, makeStr(vm, path));
    } else
        runSourceInMain(globalObject, contents->span(), path, CompileSourceIsFile, nullptr);
    if (!scope.exception())
        return std::nullopt;
    bool exits = pendingExceptionMatches(globalObject, runtime(globalObject).SystemExit());
    int status = handleUncaughtException(globalObject);
    return exits ? std::optional<int>(status) : std::nullopt;
}

// sys.__interactivehook__(), which `site` sets.
static std::optional<int> runInteractiveHook(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue hook = rt.sysDict()->get(internString(vm, "__interactivehook__"_s));
    if (!hook)
        return std::nullopt;
    call(globalObject, hook);
    if (!scope.exception())
        return std::nullopt;
    printToStderr("Failed calling sys.__interactivehook__\n"_s);
    bool exits = pendingExceptionMatches(globalObject, rt.SystemExit());
    int status = handleUncaughtException(globalObject);
    return exits ? std::optional<int>(status) : std::nullopt;
}

// The exit status.
static int runInteractively(JSGlobalObject* globalObject, bool runsStartupFile)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (isatty(STDIN_FILENO) && !environmentVariable(configuration(), "PYTHON_BASIC_REPL")) {
        importModule(globalObject, "_pyrepl"_s);
        if (!scope.exception()) {
            JSValue module = importModule(globalObject, internString(vm, configuration().interactiveModule));
            JSValue console = module ? getAttr(globalObject, module, internString(vm, "interactive_console"_s)) : JSValue();
            if (console) {
                callWithKeywords(globalObject, console, { { "mainmodule"_s, ensureMainModule(globalObject) }, { "pythonstartup"_s, jsBoolean(runsStartupFile) } });
            } else
                printToStderr("Could not import _pyrepl.main\n"_s);
            return scope.exception() ? handleUncaughtException(globalObject) : 0;
        }
        if (!clearPendingExceptionIfMatches(globalObject, rt.ModuleNotFoundError())) {
            printToStderr("Could not import _pyrepl.main\n"_s);
            return handleUncaughtException(globalObject);
        }
    }
    return runBasicInteractiveLoop(globalObject).value_or(0);
}

static void setInspect(JSGlobalObject* globalObject, int value)
{
    configuration().inspect = value;
    updateSysFlags(globalObject);
}

// -i: when the program has run, go on interactively.
static void inspectIfAsked(JSGlobalObject* globalObject, std::optional<int>& status)
{
    Configuration& configuration = Python::configuration();
    // The program may have set it.
    if (!configuration.inspect && environmentVariable(configuration, "PYTHONINSPECT"))
        setInspect(globalObject, 1);
    if (!configuration.inspect || !stdinIsInteractive() || !runsCode())
        return;
    setInspect(globalObject, 0);
    if (auto exitStatus = runInteractiveHook(globalObject)) {
        status = exitStatus;
        return;
    }
    status = runInteractively(globalObject, true);
}

std::optional<int> commandLineProgramEnded(JSGlobalObject* globalObject, JSValue exception)
{
    std::optional<int> status = mainModuleEnded(globalObject, exception);
    inspectIfAsked(globalObject, status);
    return status;
}

std::optional<int> runCommandLine(JSGlobalObject* globalObject, JSValue* unfinished)
{
    Runtime& rt = Runtime::ensure(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Configuration& configuration = Python::configuration();
    std::optional<int> status;

    installSignalHandlers(globalObject);
    ensureMainModule(globalObject);

    String filename = configuration.runFilename;
    if (!filename.isNull() && !filename.startsWith('/'))
        filename = absolutePath(filename);

    // A directory or an archive with a __main__.py in it is a program too.
    bool isImportPath = false;
    if (!filename.isNull() && mayBeImportPath(filename)) {
        JSValue importer = importerFor(globalObject, makeStr(vm, filename));
        if (scope.exception()) {
            printToStderr("Failed checking if argv[0] is an import path entry\n"_s);
            return handleUncaughtException(globalObject);
        }
        isImportPath = !isNone(importer);
    }

    String first;
    if (isImportPath)
        first = filename;
    else if (!configuration.safePath)
        first = computeFirstSearchPath(configuration.arguments);
    if (!first.isNull()) {
        configuration.firstSearchPath = first;
        if (auto* searchPath = tryAsList(rt.sysDict()->get(rt.names.path())))
            listInsert(globalObject, searchPath, 0, makeStr(vm, first));
    }

    if (!configuration.quiet && (configuration.verbose || (!runsCode() && stdinIsInteractive()))) {
        printToStderr(makeString("Python "_s, versionText(), " on "_s, platformText(), '\n'));
        if (configuration.siteImport)
            printToStderr("Type \"help\", \"copyright\", \"credits\" or \"license\" for more information.\n"_s);
    }

    makeCallsOwed(vm);
    if (!scope.exception()) {
        if (!configuration.runCommand.isNull())
            runCommand(globalObject, configuration.runCommand, unfinished);
        else if (!configuration.runModule.isNull())
            runModule(globalObject, configuration.runModule, true);
        else if (isImportPath)
            runModule(globalObject, "__main__"_s, false);
        else if (!filename.isNull())
            status = runFile(globalObject, filename, unfinished);
        else if (stdinIsInteractive()) {
            // SystemExit is to exit.
            setInspect(globalObject, 0);
            status = runStartupFile(globalObject);
            if (!status)
                status = runInteractiveHook(globalObject);
            if (!status)
                status = runInteractively(globalObject, false);
        } else {
            auto source = readAllOfStdin();
            ensureMainModule(globalObject)->dict()->set(vm, rt.names.dFile(), makeStr(vm, "<stdin>"_s));
            runSourceInMain(globalObject, source.span(), "<stdin>"_s, CompileAllowTopLevelAwait, unfinished);
        }
    }
    if (unfinished && *unfinished)
        return std::nullopt;

    // Flushing looks for signals, and drops what is raised.
    if (!scope.exception())
        checkSignals(globalObject);
    if (scope.exception())
        status = handleUncaughtException(globalObject);
    flushStandardStreams(globalObject);
    inspectIfAsked(globalObject, status);
    return status;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
