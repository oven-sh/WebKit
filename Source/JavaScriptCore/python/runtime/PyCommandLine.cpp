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
#include <wtf/HexNumber.h>
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

    if (const char* seed = environmentVariable(configuration, "PYTHONHASHSEED"); seed && strcmp(seed, "random") && !configuration.ignoresHashSeedOfEnvironment) {
        char* end = nullptr;
        errno = 0;
        unsigned long value = strtoul(seed, &end, 10);
        if (*end || value > 4294967295UL || (errno == ERANGE && value == ULONG_MAX))
            return fatalConfigurationError("config_init_hash_seed"_s, "PYTHONHASHSEED must be \"random\" or an integer in range [0; 4294967295]"_s);
        configuration.hashSeed = static_cast<uint32_t>(value);
    }

#if ASSERT_ENABLED
    // For tests: every dict of the process becomes keyed early, or at once.
    auto limitOfDicts = [&](ASCIILiteral name) -> std::optional<unsigned> {
        String text = xOption(configuration, name);
        auto value = text.isNull() ? std::nullopt : parseInt(text);
        return value && *value >= 0 ? std::optional<unsigned>(*value) : std::nullopt;
    };
    PyDict::setLimitsForTesting(limitOfDicts("dict_keyed_after_steps"_s), limitOfDicts("dict_keyed_from_log2_size"_s));
#endif

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

// What CPython prints, whether or not an option does anything here (DEFERRED.md says which do not).
static constexpr ASCIILiteral optionsHelp =
    "Options (and corresponding environment variables):\n"
    "-b     : issue warnings about converting bytes/bytearray to str and comparing\n"
    "         bytes/bytearray with str or bytes with int. (-bb: issue errors)\n"
    "         deprecated since 3.15 and will become no-op in 3.17.\n"
    "-B     : don't write .pyc files on import; also PYTHONDONTWRITEBYTECODE=x\n"
    "-c cmd : program passed in as string (terminates option list)\n"
    "-d     : turn on parser debugging output (for experts only, only works on\n"
    "         debug builds); also PYTHONDEBUG=x\n"
    "-E     : ignore PYTHON* environment variables (such as PYTHONPATH)\n"
    "-h     : print this help message and exit (also -? or --help)\n"
    "-i     : inspect interactively after running script; forces a prompt even\n"
    "         if stdin does not appear to be a terminal; also PYTHONINSPECT=x\n"
    "-I     : isolate Python from the user's environment (implies -E, -P and -s)\n"
    "-m mod : run library module as a script (terminates option list)\n"
    "-O     : remove assert and __debug__-dependent statements; add .opt-1 before\n"
    "         .pyc extension; also PYTHONOPTIMIZE=x\n"
    "-OO    : do -O changes and also discard docstrings; add .opt-2 before\n"
    "         .pyc extension\n"
    "-P     : don't prepend a potentially unsafe path to sys.path; also\n"
    "         PYTHONSAFEPATH\n"
    "-q     : don't print version and copyright messages on interactive startup\n"
    "-s     : don't add user site directory to sys.path; also PYTHONNOUSERSITE=x\n"
    "-S     : don't imply 'import site' on initialization\n"
    "-u     : force the stdout and stderr streams to be unbuffered;\n"
    "         this option has no effect on stdin; also PYTHONUNBUFFERED=x\n"
    "-v     : verbose (trace import statements); also PYTHONVERBOSE=x\n"
    "         can be supplied multiple times to increase verbosity\n"
    "-V     : print the Python version number and exit (also --version)\n"
    "         when given twice, print more information about the build\n"
    "-W arg : warning control; arg is action:message:category:module:lineno\n"
    "         also PYTHONWARNINGS=arg\n"
    "-x     : skip first line of source, allowing use of non-Unix forms of #!cmd\n"
    "-X opt : set implementation-specific option\n"
    "--check-hash-based-pycs always|default|never:\n"
    "         control how Python invalidates hash-based .pyc files\n"
    "--help-env: print help about Python environment variables and exit\n"
    "--help-xoptions: print help about implementation-specific -X options and exit\n"
    "--help-all: print complete help information and exit\n"
    "\n"
    "Arguments:\n"
    "file   : program read from script file\n"
    "-      : program read from stdin (default; interactive mode if a tty)\n"
    "arg ...: arguments passed to program in sys.argv[1:]\n"_s;

static constexpr ASCIILiteral xOptionsHelp =
    "The following implementation-specific options are available:\n"
    "-X context_aware_warnings=[0|1]: if true (1) then the warnings module will\n"
    "         use a context variables; if false (0) then the warnings module will\n"
    "         use module globals, which is not concurrent-safe; set to true for\n"
    "         free-threaded builds and false otherwise; also\n"
    "         PYTHON_CONTEXT_AWARE_WARNINGS\n"
    "-X cpu_count=N: override the return value of os.cpu_count();\n"
    "         -X cpu_count=default cancels overriding; also PYTHON_CPU_COUNT\n"
    "-X dev : enable Python Development Mode; also PYTHONDEVMODE\n"
    "-X disable-remote-debug: disable remote debugging; also PYTHON_DISABLE_REMOTE_DEBUG\n"
    "-X faulthandler: dump the Python traceback on fatal errors;\n"
    "         also PYTHONFAULTHANDLER\n"
    "-X frozen_modules=[on|off]: whether to use frozen modules; the default is \"on\"\n"
    "         for installed Python and \"off\" for a local build;\n"
    "         also PYTHON_FROZEN_MODULES\n"
    "-X importtime[=2]: show how long each import takes; use -X importtime=2 to\n"
    "         log imports of already-loaded modules; also PYTHONPROFILEIMPORTTIME\n"
    "-X int_max_str_digits=N: limit the size of int<->str conversions;\n"
    "         0 disables the limit; also PYTHONINTMAXSTRDIGITS\n"
    "-X lazy_imports=[all|normal]: control global lazy imports;\n"
    "         default is normal; also PYTHON_LAZY_IMPORTS\n"
    "-X no_debug_ranges: don't include extra location information in code objects;\n"
    "         also PYTHONNODEBUGRANGES\n"
    "-X pathconfig_warnings=[0|1]: if true (1) then path configuration is allowed\n"
    "         to log warnings into stderr; if false (0) suppress these warnings;\n"
    "         set to true by default; also PYTHON_PATHCONFIG_WARNINGS\n"
    "-X perf: support the Linux \"perf\" profiler; also PYTHONPERFSUPPORT=1\n"
    "-X perf_jit: support the Linux \"perf\" profiler with DWARF support;\n"
    "         also PYTHON_PERF_JIT_SUPPORT=1\n"
    "-X pycache_prefix=PATH: write .pyc files to a parallel tree instead of to the\n"
    "         code tree; also PYTHONPYCACHEPREFIX\n"
    "-X showrefcount: output the total reference count and number of used\n"
    "         memory blocks when the program finishes or after each statement in\n"
    "         the interactive interpreter; only works on debug builds\n"
    "-X thread_inherit_context=[0|1]: enable (1) or disable (0) threads inheriting\n"
    "         context vars by default; enabled by default in the free-threaded\n"
    "         build and disabled otherwise; also PYTHON_THREAD_INHERIT_CONTEXT\n"
    "-X tracemalloc[=N]: trace Python memory allocations; N sets a traceback limit\n"
    "         of N frames (default: 1); also PYTHONTRACEMALLOC=N\n"
    "-X utf8[=0|1]: enable (1) or disable (0) UTF-8 mode; also PYTHONUTF8\n"
    "-X warn_default_encoding: enable opt-in EncodingWarning for 'encoding=None';\n"
    "         also PYTHONWARNDEFAULTENCODING\n"_s;

static constexpr ASCIILiteral environmentHelp =
    "Environment variables that change behavior:\n"
    "PYTHONASYNCIODEBUG: enable asyncio debug mode\n"
    "PYTHON_BASIC_REPL: use the traditional parser-based REPL\n"
    "PYTHONBREAKPOINT: if this variable is set to 0, it disables the default\n"
    "                  debugger.  It can be set to the callable of your debugger of\n"
    "                  choice.\n"
    "PYTHONCASEOK    : ignore case in 'import' statements (Windows)\n"
    "PYTHONCOERCECLOCALE: if this variable is set to 0, it disables the locale\n"
    "                  coercion behavior.  Use PYTHONCOERCECLOCALE=warn to request\n"
    "                  display of locale coercion and locale compatibility warnings\n"
    "                  on stderr.\n"
    "PYTHON_COLORS   : if this variable is set to 1, the interpreter will colorize\n"
    "                  various kinds of output.  Setting it to 0 deactivates\n"
    "                  this behavior.\n"
    "PYTHONHASHSEED  : if this variable is set to 'random', a random value is used\n"
    "                  to seed the hashes of str and bytes objects.  It can also be\n"
    "                  set to an integer in the range [0,4294967295] to get hash\n"
    "                  values with a predictable seed.\n"
    "PYTHON_HISTORY  : the location of a .python_history file.\n"
    "PYTHONHOME      : alternate <prefix> directory (or <prefix>:<exec_prefix>).\n"
    "                  The default module search path uses <prefix>/lib/pythonX.X.\n"
    "PYTHONIOENCODING: encoding[:errors] used for stdin/stdout/stderr\n"
    "PYTHONMALLOC    : set the Python memory allocators and/or install debug hooks\n"
    "                  on Python memory allocators.  Use PYTHONMALLOC=debug to\n"
    "                  install debug hooks.\n"
    "PYTHONMALLOCSTATS: print memory allocator statistics\n"
    "PYTHONPATH      : ':'-separated list of directories prefixed to the\n"
    "                  default module search path.  The result is sys.path.\n"
    "PYTHONPLATLIBDIR: override sys.platlibdir\n"
    "PYTHONSTARTUP   : file executed on interactive startup (no default)\n"
    "PYTHONUSERBASE  : defines the user base directory (site.USER_BASE)\n"
    "\n"
    "These variables have equivalent command-line options (see --help for details):\n"
    "PYTHON_CONTEXT_AWARE_WARNINGS: if true (1), enable thread-safe warnings\n"
    "                  module behaviour (-X context_aware_warnings)\n"
    "PYTHON_CPU_COUNT: override the return value of os.cpu_count() (-X cpu_count)\n"
    "PYTHONDEBUG     : enable parser debug mode (-d)\n"
    "PYTHONDEVMODE   : enable Python Development Mode (-X dev)\n"
    "PYTHONDONTWRITEBYTECODE: don't write .pyc files (-B)\n"
    "PYTHONFAULTHANDLER: dump the Python traceback on fatal errors (-X faulthandler)\n"
    "PYTHON_FROZEN_MODULES: whether to use frozen modules; the default is \"on\"\n"
    "                  for installed Python and \"off\" for a local build\n"
    "                  (-X frozen_modules)\n"
    "PYTHONINSPECT   : inspect interactively after running script (-i)\n"
    "PYTHONINTMAXSTRDIGITS: limit the size of int<->str conversions;\n"
    "                  0 disables the limit (-X int_max_str_digits=N)\n"
    "PYTHON_LAZY_IMPORTS: control global lazy imports (-X lazy_imports)\n"
    "PYTHONNODEBUGRANGES: don't include extra location information in code objects\n"
    "                  (-X no_debug_ranges)\n"
    "PYTHONNOUSERSITE: disable user site directory (-s)\n"
    "PYTHONOPTIMIZE  : enable level 1 optimizations (-O)\n"
    "PYTHON_PERF_JIT_SUPPORT: enable Linux \"perf\" profiler support with JIT\n"
    "                  (-X perf_jit)\n"
    "PYTHONPERFSUPPORT: support the Linux \"perf\" profiler (-X perf)\n"
    "PYTHONPROFILEIMPORTTIME: show how long each import takes (-X importtime)\n"
    "PYTHONPYCACHEPREFIX: root directory for bytecode cache (pyc) files\n"
    "                  (-X pycache_prefix)\n"
    "PYTHONSAFEPATH  : don't prepend a potentially unsafe path to sys.path.\n"
    "PYTHON_THREAD_INHERIT_CONTEXT: if true (1), threads inherit context vars\n"
    "                  (-X thread_inherit_context)\n"
    "PYTHONTRACEMALLOC: trace Python memory allocations (-X tracemalloc)\n"
    "PYTHONUNBUFFERED: disable stdout/stderr buffering (-u)\n"
    "PYTHONUTF8      : control the UTF-8 mode (-X utf8)\n"
    "PYTHONVERBOSE   : trace import statements (-v)\n"
    "PYTHONWARNDEFAULTENCODING: enable opt-in EncodingWarning for 'encoding=None'\n"
    "                  (-X warn_default_encoding)\n"
    "PYTHONWARNINGS  : warning control (-W)\n"_s;

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
            configuration.ignoresHashSeedOfEnvironment = true;
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

// As sys.stderr would show the name: what cannot be encoded is escaped.
static String nameForImportTime(const String& name)
{
    if (name.is8Bit())
        return name;
    StrBuilder result;
    for (char32_t character : StringView(name).codePoints()) {
        if (U_IS_SURROGATE(character))
            result.append("\\u"_s, WTF::hex(static_cast<unsigned>(character), 4, WTF::Lowercase));
        else
            result.append(character);
    }
    return result.toString();
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
    line.append(nameForImportTime(m_name), '\n');
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
    line.append(nameForImportTime(moduleName), '\n');
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
    if (auto* bootstrap = PyModule::tryFrom(rt.modules()->get(rt.names.uFrozenImportlib()))) {
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

    registerInteropTypes(globalObject);
    RETURN_IF_EXCEPTION(scope, void());

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
    String text = dedent(command);
    auto utf8 = tryEncodeUTF8(text);
    if (!utf8) {
        // There were bytes in it that are not UTF-8.
        printToStderr("Unable to decode the command from the command line:\n"_s);
        VM& vm = getVM(globalObject);
        strEncode(globalObject, makeStr(vm, text), makeStr(vm, "utf-8"_s), makeStr(vm, "strict"_s));
        return;
    }
    runSourceInMain(globalObject, utf8->span(), "<string>"_s, CompileIgnoreCookie | CompileAllowTopLevelAwait | CompileSourceIsShown, unfinished);
}

// runpy._run_module_as_main(name, changesFirstArgument)
static void runModule(JSGlobalObject* globalObject, const String& name, bool changesFirstArgument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue runpy = importModule(globalObject, "runpy"_s);
    JSValue function = !scope.exception() ? getAttr(globalObject, runpy, internString(vm, "_run_module_as_main"_s)) : JSValue();
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

// One line from standard input, with its end. Empty: there is no more. Nothing: the handler of a signal has raised an exception.
static std::optional<Vector<uint8_t>> readLineFromStdin(JSGlobalObject* globalObject, const String& prompt)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        JSValue edited = readLineWithEditor(globalObject, makeStr(vm, prompt));
        if (scope.exception() && clearPendingExceptionIfMatches(globalObject, runtime(globalObject).EOFError()))
            return Vector<uint8_t> { };
        if (scope.exception()) {
            printToStderr("\n"_s);
            return std::nullopt;
        }
        if (edited) {
            String text = strValue(edited)->value(globalObject);
            Vector<uint8_t> line = *tryEncodeUTF8(text, true);
            line.append('\n');
            return line;
        }
    }
    printToStderr(prompt);
    fflush(stderr);
    Vector<uint8_t> line;
    while (true) {
        uint8_t byte;
        ssize_t count = read(STDIN_FILENO, &byte, 1);
        if (count < 0 && errno == EINTR) {
            makeCallsOwed(vm);
            if (!scope.exception())
                continue;
            printToStderr("\n"_s);
            return std::nullopt;
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
    dropPendingException(globalObject, scope);
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
        dropPendingException(globalObject, scope);
    }
}

static bool hasOnlyBlankLinesAndComments(std::span<const uint8_t> text)
{
    bool isInComment = false;
    for (uint8_t character : text) {
        if (character == '\n')
            isInComment = false;
        else if (character == '#')
            isInComment = true;
        else if (!isInComment && character != ' ' && character != '\t' && character != '\f' && character != '\r')
            return false;
    }
    return true;
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
    if (JSValue codeop = importModule(globalObject, "codeop"_s); !failed(scope, codeop)) {
        compiler = callMethod(globalObject, codeop, internString(vm, "CommandCompiler"_s));
        if (!failed(scope, compiler)) {
            if (JSValue inner = getAttr(globalObject, compiler, internString(vm, "compiler"_s)); !failed(scope, inner)) {
                JSValue flags = getAttr(globalObject, inner, internString(vm, "flags"_s));
                if (!failed(scope, flags) && flags.isInt32())
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
    bool wasInterrupted = false;
    while (true) {
        auto line = readLineFromStdin(globalObject, promptText(globalObject, pending.isEmpty() ? "ps1"_s : "ps2"_s));
        bool isAtEnd = false;
        if (!line)
            pending.clear();
        else {
            isAtEnd = line->isEmpty();
            if (isAtEnd)
                printToStderr("\n"_s);
            if (isAtEnd && pending.isEmpty())
                break;
            pending.appendVector(*line);
            // Nothing to run, and no mistake either.
            if (hasOnlyBlankLinesAndComments(pending.span())) {
                pending.clear();
                if (isAtEnd)
                    break;
                continue;
            }
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
                if (!failed(scope, code) && isNone(code))
                    continue;
                // Once more, so that tracebacks can show the lines, and a mistake is found in the lines as they were typed.
                if (code || pendingExceptionMatches(globalObject, rt.SyntaxError())) {
                    scope.clearException();
                    code = compileSource(globalObject, pending.span(), "<stdin>"_s, "single"_s, compileFlags, -1, globals->get(rt.names.dName()));
                }
            }
            // Where the compiler was called from is of no interest.
            if (auto* exception = PyException::tryFrom(pendingException(globalObject)))
                exception->setTraceback(vm, JSValue());
        }
        if (!failed(scope, code)) {
            registerSourceWithLinecache(globalObject, code, pending.span(), "<stdin>"_s);
            compileFlags |= static_cast<int>((PyCode::from(code)->flags() & OptionSet<CodeFlag> { CodeFlag::FutureAnnotations, CodeFlag::FutureBarryAsBDFL }).toRaw());
        }
        pending.clear();

        if (code && !scope.exception()) {
            JSValue result = runCode(globalObject, code, globals, globals);
            if (!scope.exception() && PyCode::from(code)->flags().contains(CodeFlag::Coroutine)) {
                // One loop for the session, so that what one statement starts the next finds running.
                if (!runner) {
                    JSValue asyncio = importModule(globalObject, "asyncio"_s);
                    if (!scope.exception())
                        runner = callMethod(globalObject, asyncio, internString(vm, "Runner"_s));
                }
                if (!scope.exception())
                    callMethod(globalObject, runner, internString(vm, "run"_s), result);
            }
        }
        if (vm.hasPendingTerminationException()) [[unlikely]]
            return 1;
        if (scope.exception()) {
            bool exits = pendingExceptionMatches(globalObject, rt.SystemExit());
            int status = handleUncaughtException(globalObject);
            if (exits || vm.hasPendingTerminationException())
                return status;
            wasInterrupted |= status == -SIGINT;
        }
        flushStandardStreams(globalObject);
    }
    if (runner) {
        callMethod(globalObject, runner, internString(vm, "close"_s));
        dropPendingException(globalObject, scope);
    }
    // As in CPython: a KeyboardInterrupt that was reported here is what the process dies of in the end.
    if (wasInterrupted)
        return -SIGINT;
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
    int status = handleUncaughtExceptionOfFile(globalObject);
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
            JSValue console = !scope.exception() ? getAttr(globalObject, module, internString(vm, "interactive_console"_s)) : JSValue();
            if (!scope.exception()) {
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
    Runtime::EventLoopPositionScope position(runtime(globalObject), false);
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

String standardLibraryDirectory(); // PySys.cpp

// Nothing works without the standard library. Where it was looked for, and why there, in CPython's words.
static void reportMissingStandardLibrary(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    Configuration& configuration = Python::configuration();
    auto shown = [](const String& text) { return text.isNull() ? "(not set)"_str : asciiEscape(strRepr(text)); };
    StrBuilder text;
    if (!configuration.home.isNull())
        text.append("WARN: Could not find the standard library directory! The Python 'home' directory was set to "_s, strRepr(configuration.home), ", is this correct?\n"_s);
    text.append("Python path configuration:\n  PYTHONHOME = "_s, shown(configuration.home), "\n  PYTHONPATH = "_s, shown(configuration.pythonPath), "\n  program name = "_s, shown(configuration.programName));
    text.append("\n  isolated = "_s, configuration.isolated ? 1 : 0, "\n  environment = "_s, configuration.useEnvironment ? 1 : 0, "\n  user site = "_s, configuration.userSiteDirectory ? 1 : 0);
    text.append("\n  safe_path = "_s, configuration.safePath ? 1 : 0, "\n  import site = "_s, configuration.siteImport ? 1 : 0, "\n  is in build tree = 0\n  stdlib dir = "_s, shown(standardLibraryDirectory()));
    text.append("\n  sys.path[0] = "_s, shown(configuration.firstSearchPath), '\n');
    for (ASCIILiteral name : { "_base_executable"_s, "base_prefix"_s, "base_exec_prefix"_s, "platlibdir"_s, "executable"_s, "prefix"_s, "exec_prefix"_s }) {
        JSValue value = rt.sysDict()->get(internString(vm, name));
        text.append("  sys."_s, name, " = "_s, value ? asciiEscape(reprForMessage(globalObject, value)) : "(not set)"_str, '\n');
    }
    if (auto* searchPath = tryAsList(rt.sysDict()->get(rt.names.path()))) {
        text.append("  sys.path = [\n"_s);
        for (unsigned i = 0; i < listLength(searchPath); ++i)
            text.append("    "_s, asciiEscape(reprForMessage(globalObject, listGet(searchPath, i))), ",\n"_s);
        text.append("  ]\n"_s);
    }
    text.append("Fatal Python error: Failed to import encodings module\nPython runtime state: core initialized\nModuleNotFoundError: No module named 'encodings'\n"_s);
    printToStderr(text.toString());
}

std::optional<int> runCommandLine(JSGlobalObject* globalObject, JSValue* unfinished)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Runtime& rt = Runtime::ensure(globalObject);
    bool hasStarted = !scope.exception();
    Configuration& configuration = Python::configuration();
    std::optional<int> status;

    if (!rt.slot(Runtime::Slot::FindAndLoad)) {
        reportMissingStandardLibrary(globalObject);
        return 1;
    }

    bool runsFile = false;
    auto finish = [&] {
        // As in CPython, what a file has written comes before the traceback. After -c and -m it stays in the buffers until the interpreter ends.
        if (runsFile) {
            ExceptionSetAside suspended(globalObject);
            flushStandardStreams(globalObject);
        }
        if (scope.exception())
            status = handleUncaughtException(globalObject);
        inspectIfAsked(globalObject, status);
        return status;
    };

    if (hasStarted)
        installSignalHandlers(globalObject);
    if (scope.exception())
        return finish();
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
        if (auto* searchPath = tryAsList(rt.sysDict()->get(rt.names.path()))) {
            listInsert(globalObject, searchPath, 0, makeStr(vm, first));
            if (scope.exception())
                return finish();
        }
    }

    if (!configuration.quiet && (configuration.verbose || (!runsCode() && stdinIsInteractive()))) {
        printToStderr(makeString("Python "_s, versionText(), " on "_s, platformText(), '\n'));
        if (configuration.siteImport)
            printToStderr("Type \"help\", \"copyright\", \"credits\" or \"license\" for more information.\n"_s);
    }

    makeCallsOwed(vm);
    if (!scope.exception()) {
        Runtime::EventLoopPositionScope position(rt, false);
        if (!configuration.runCommand.isNull())
            runCommand(globalObject, configuration.runCommand, unfinished);
        else if (!configuration.runModule.isNull())
            runModule(globalObject, configuration.runModule, true);
        else if (isImportPath)
            runModule(globalObject, "__main__"_s, false);
        else if (!filename.isNull()) {
            runsFile = true;
            status = runFile(globalObject, filename, unfinished);
        } else if (stdinIsInteractive()) {
            // SystemExit is to exit.
            setInspect(globalObject, 0);
            status = runStartupFile(globalObject);
            if (!status)
                status = runInteractiveHook(globalObject);
            if (!status)
                status = runInteractively(globalObject, false);
        } else {
            runsFile = true;
            auto source = readAllOfStdin();
            ensureMainModule(globalObject)->dict()->set(vm, rt.names.dFile(), makeStr(vm, "<stdin>"_s));
            runSourceInMain(globalObject, source.span(), "<stdin>"_s, CompileAllowTopLevelAwait, unfinished);
        }
    }
    if (!scope.exception() && unfinished && *unfinished)
        return std::nullopt;

    // Flushing looks for signals, and drops what is raised.
    if (!scope.exception())
        checkOnlySignals(globalObject);
    return finish();
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
