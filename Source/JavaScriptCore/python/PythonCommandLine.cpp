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
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "config.h"
#include "PythonConfiguration.h"

#include "PythonPlatform.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/unicode/UTF8Conversion.h>

// The command line of `python`, and what it looks at in its environment: Python/getopt.c, and what has to do with either in Python/preconfig.c and Python/initconfig.c, of CPython. None of it needs anything of the engine's, and it
// is all done before there is an engine to need.

namespace JSC { namespace Python {

#include "PythonUsageText.h"

namespace {

// Py_DecodeLocale(), which here is always from UTF-8: a byte that is not part of anything is the low half of a surrogate pair, from which it can be got back.
String decodeArgument(const char* argument)
{
    auto bytes = byteCast<char8_t>(unsafeSpan(argument));
    if (String decoded = String::fromUTF8(bytes); !decoded.isNull())
        return decoded;
    StringBuilder builder;
    size_t i = 0;
    while (i < bytes.size()) {
        // The longest run from here that is well formed
        size_t offset = i;
        char32_t character;
        U8_NEXT(bytes.data(), offset, bytes.size(), character);
        if (static_cast<int32_t>(character) < 0) {
            builder.append(static_cast<char16_t>(0xDC00 + bytes[i]));
            ++i;
            continue;
        }
        builder.append(character);
        i = offset;
    }
    return builder.toString();
}

// PyStatus: nothing if all is well. Otherwise the process is to end, with a status, or with something said of what is wrong and by what.
struct Status {
    static Status ok() { return { }; }
    static Status exit(int status) { return { status, { }, { }, { } }; }
    static Status error(ASCIILiteral function, ASCIILiteral message, ASCIILiteral runtimeState = "preinitialized"_s) { return { std::nullopt, function, message, runtimeState }; }
    bool isException() const { return exitStatus || !message.isNull(); }

    std::optional<int> exitStatus;
    ASCIILiteral function;
    ASCIILiteral message;
    ASCIILiteral runtimeState; // How far Python is said to have got
};

#define RETURN_IF_EXCEPTION_STATUS(expression) \
    do { \
        Status status = (expression); \
        if (status.isException()) \
            return status; \
    } while (false)

// ---- Python/getopt.c

constexpr auto shortOptions = "bBc:dEhiIm:OPqRsStuvVW:xX:?"_s;

struct LongOption {
    ASCIILiteral name;
    bool hasArgument;
    int value;
};
constexpr LongOption longOptions[] = {
    { "check-hash-based-pycs"_s, true, 0 },
    { "help-all"_s, false, 1 },
    { "help-env"_s, false, 2 },
    { "help-xoptions"_s, false, 3 },
};

class OptionReader {
public:
    static constexpr int end = -1;

    OptionReader(const Vector<String>& arguments, bool reportsErrors)
        : m_arguments(arguments)
        , m_reportsErrors(reportsErrors)
    {
    }

    unsigned index() const { return m_index; }
    void stepBack() { --m_index; }
    const String& argument() const { return m_argument; }

    // _PyOS_GetOpt()
    int next()
    {
        if (m_position >= m_current.length()) {
            if (m_index >= m_arguments.size())
                return end;
            const String& argument = m_arguments[m_index];
            if (argument.length() < 2 || argument[0] != '-')
                return end;
            if (argument == "--"_s) {
                ++m_index;
                return end;
            }
            if (argument == "--help"_s) {
                ++m_index;
                return 'h';
            }
            if (argument == "--version"_s) {
                ++m_index;
                return 'V';
            }
            m_current = m_arguments[m_index++];
            m_position = 1;
        }
        char16_t option = m_current[m_position++];
        if (option == '-') {
            if (m_position >= m_current.length()) {
                report("Expected long option\n"_s);
                return end;
            }
            String name = m_current.substring(m_position);
            m_position = m_current.length();
            for (auto& candidate : longOptions) {
                if (name != candidate.name)
                    continue;
                if (!candidate.hasArgument)
                    return candidate.value;
                if (m_index >= m_arguments.size()) {
                    report(makeString("Argument expected for the "_s, m_arguments[m_index - 1], " options\n"_s));
                    return '_';
                }
                m_argument = m_arguments[m_index++];
                return candidate.value;
            }
            report(makeString("Unknown option: "_s, m_arguments[m_index - 1], '\n'));
            return '_';
        }
        // A colon is found there too, and is then an option that nothing is done for.
        size_t found = option < 0x80 ? StringView(shortOptions).find(option) : notFound;
        if (found == notFound) {
            // As much of it as fits in a char, whatever that makes
            if (m_reportsErrors)
                fprintf(stderr, "Unknown option: -%c\n", static_cast<char>(option));
            return '_';
        }
        if (found + 1 < shortOptions.length() && shortOptions[found + 1] == ':') {
            if (m_position < m_current.length()) {
                m_argument = m_current.substring(m_position);
                m_position = m_current.length();
            } else {
                if (m_index >= m_arguments.size()) {
                    report(makeString("Argument expected for the -"_s, static_cast<char>(option), " option\n"_s));
                    return '_';
                }
                m_argument = m_arguments[m_index++];
            }
        }
        return option;
    }

private:
    void report(const String& message)
    {
        if (!m_reportsErrors)
            return;
        CString encoded = message.utf8();
        fputs(encoded.data(), stderr);
    }

    const Vector<String>& m_arguments;
    bool m_reportsErrors;
    unsigned m_index { 1 };
    String m_current;
    unsigned m_position { 0 };
    String m_argument;
};

// ---- The environment, and -X

// _Py_GetEnv(): one that is empty is as good as not there.
const char* environmentVariable(bool usesEnvironment, const char* name)
{
    if (!usesEnvironment)
        return nullptr;
    const char* value = getenv(name);
    return value && value[0] ? value : nullptr;
}

// _Py_str_to_int()
std::optional<int> toInt(const char* text)
{
    char* end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (*end || errno == ERANGE || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        return std::nullopt;
    return static_cast<int>(value);
}

// config_wstr_to_int()
std::optional<int> toInt(const String& text)
{
    CString encoded = text.utf8();
    return toInt(encoded.data());
}

// _Py_get_env_flag(). What is not a number, or is less than nothing, is 1.
void raiseToEnvironmentFlag(bool usesEnvironment, int& flag, const char* name)
{
    const char* variable = environmentVariable(usesEnvironment, name);
    if (!variable)
        return;
    int value = toInt(variable).value_or(1);
    if (value < 0)
        value = 1;
    flag = std::max(flag, value);
}

// _Py_get_xoption(): the whole of it, with what it is set to. Null if it is not there.
const String* findExtraOption(const Vector<String>& options, ASCIILiteral name)
{
    for (auto& option : options) {
        size_t separator = option.find('=');
        if (StringView(option).left(separator == notFound ? option.length() : separator) == name)
            return &option;
    }
    return nullptr;
}

// What comes after the "=", if there is one
std::optional<String> afterSeparator(const String& option)
{
    size_t separator = option.find('=');
    if (separator == notFound)
        return std::nullopt;
    return option.substring(separator + 1);
}

class Reader {
public:
    Reader(Configuration& configuration)
        : config(configuration)
    {
    }

    Status read(std::span<const char* const>);

private:
    const char* environment(const char* name) const { return environmentVariable(config.usesEnvironment, name); }
    const String* extraOption(ASCIILiteral name) const { return findExtraOption(config.extraOptions, name); }
    // config_get_xoption_value(): empty if it is set to nothing. Nothing if it is not there.
    std::optional<String> extraOptionValue(ASCIILiteral name) const
    {
        const String* option = extraOption(name);
        if (!option)
            return std::nullopt;
        return afterSeparator(*option).value_or(emptyString());
    }

    void readBeforeCommandLine();
    Status readUTF8Mode();
    Status parseCommandLine(Vector<String>& warningOptions, unsigned& optionIndex);
    void makeFilenameAbsolute();
    void updateArguments(unsigned optionIndex);
    void initializeWarningOptions(const Vector<String>& fromCommandLine, const Vector<String>& fromEnvironment);
    Status readEnvironment();
    Status readComplexOptions();
    Status readSwitch(bool& target, const char* variable, ASCIILiteral option, ASCIILiteral function, ASCIILiteral ifVariableIsWrong, ASCIILiteral ifOptionIsWrong);
    Status readFrozenModules();
    void readStandardStreamEncoding();
    void usage(bool isError) const;

    Configuration& config;
    Vector<String> arguments;
};

// config_usage()
void Reader::usage(bool isError) const
{
    FILE* file = isError ? stderr : stdout;
    CString program = config.programName.utf8();
    ALLOW_NONLITERAL_FORMAT_BEGIN
    fprintf(file, usage_line, program.data());
    ALLOW_NONLITERAL_FORMAT_END
    fputs(isError ? "Try `python -h' for more information.\n" : usage_help, file);
}

void printEnvironmentUsage()
{
    ALLOW_NONLITERAL_FORMAT_BEGIN
    printf(usage_envvars, ':', PYTHONHOMEHELP, ':');
    ALLOW_NONLITERAL_FORMAT_END
}

// _PyPreCmdline_Read(): what has to be known before anything in the environment is looked at
void Reader::readBeforeCommandLine()
{
    OptionReader options(arguments, false);
    for (;;) {
        int c = options.next();
        if (c == OptionReader::end || c == 'c' || c == 'm')
            break;
        if (c == 'E')
            config.usesEnvironment = false;
        else if (c == 'I')
            config.isIsolated = true;
        else if (c == 'X')
            config.extraOptions.append(options.argument());
    }
    if (config.isIsolated)
        config.usesEnvironment = false;
    if (extraOption("dev"_s) || environment("PYTHONDEVMODE"))
        config.isDevelopmentMode = true;
    if (extraOption("warn_default_encoding"_s) || environment("PYTHONWARNDEFAULTENCODING"))
        config.warnsOfDefaultEncoding = true;
}

// preconfig_init_utf8_mode()
Status Reader::readUTF8Mode()
{
    constexpr auto function = "preconfig_init_utf8_mode"_s;
    if (const String* option = extraOption("utf8"_s)) {
        auto value = afterSeparator(*option);
        if (!value || *value == "1"_s)
            config.usesUTF8Mode = true;
        else if (*value == "0"_s)
            config.usesUTF8Mode = false;
        else
            return Status::error(function, "invalid -X utf8 option value"_s, "preinitializing"_s);
        return Status::ok();
    }
    if (const char* variable = environment("PYTHONUTF8")) {
        if (!strcmp(variable, "1"))
            config.usesUTF8Mode = true;
        else if (!strcmp(variable, "0"))
            config.usesUTF8Mode = false;
        else
            return Status::error(function, "invalid PYTHONUTF8 environment variable value"_s, "preinitializing"_s);
    }
    return Status::ok();
}

// config_parse_cmdline()
Status Reader::parseCommandLine(Vector<String>& warningOptions, unsigned& optionIndex)
{
    unsigned printsVersion = 0;
    OptionReader options(arguments, true);
    for (;;) {
        int c = options.next();
        if (c == OptionReader::end)
            break;
        // Either is the last of the options. What follows and looks like one is for what is run to make what it will of.
        if (c == 'c') {
            config.runCommand = makeString(options.argument(), '\n');
            break;
        }
        if (c == 'm') {
            config.runModule = options.argument();
            break;
        }
        switch (c) {
        case 0:
            if (options.argument() != "always"_s && options.argument() != "never"_s && options.argument() != "default"_s) {
                fputs("--check-hash-based-pycs must be one of 'default', 'always', or 'never'\n", stderr);
                usage(true);
                return Status::exit(2);
            }
            break;
        case 1:
            usage(false);
            putchar('\n');
            printEnvironmentUsage();
            putchar('\n');
            puts(usage_xoptions);
            return Status::exit(0);
        case 2:
            printEnvironmentUsage();
            return Status::exit(0);
        case 3:
            puts(usage_xoptions);
            return Status::exit(0);
        case 'b':
            ++config.bytesWarning;
            break;
        case 'd':
            ++config.parserDebug;
            break;
        case 'i':
            ++config.inspect;
            ++config.interactive;
            break;
        case 'E':
        case 'I':
        case 'X':
            // Seen to already
            break;
        case 'O':
            ++config.optimizationLevel;
            break;
        case 'P':
            config.hasSafePath = true;
            break;
        case 'B':
            config.writesBytecode = false;
            break;
        case 's':
            config.usesUserSiteDirectory = false;
            break;
        case 'S':
            config.importsSite = false;
            break;
        case 't':
            // It once meant something.
            break;
        case 'u':
            config.buffersStandardStreams = false;
            break;
        case 'v':
            ++config.verbose;
            break;
        case 'x':
            config.skipsFirstLineOfSource = true;
            break;
        case 'h':
        case '?':
            usage(false);
            return Status::exit(0);
        case 'V':
            ++printsVersion;
            break;
        case 'W':
            warningOptions.append(options.argument());
            break;
        case 'q':
            ++config.quiet;
            break;
        case 'R':
            // What a string hashes to is the same from one run to the next in any case.
            break;
        default:
            usage(true);
            return Status::exit(2);
        }
    }

    if (printsVersion) {
        printf("Python %s\n", printsVersion >= 2 ? PYTHON_FULL_VERSION_STRING : PYTHON_VERSION_STRING);
        return Status::exit(0);
    }

    if (config.runCommand.isNull() && config.runModule.isNull() && options.index() < arguments.size() && arguments[options.index()] != "-"_s)
        config.runFilename = arguments[options.index()];
    if (!config.runCommand.isNull() || !config.runModule.isNull())
        options.stepBack();
    optionIndex = options.index();
    return Status::ok();
}

// config_run_filename_abspath(). If there is no telling where the process is, it is left as it is.
void Reader::makeFilenameAbsolute()
{
    if (config.runFilename.isNull() || config.runFilename.startsWith('/'))
        return;
    char directory[PATH_MAX];
    if (!getcwd(directory, sizeof(directory)))
        return;
    // _Py_abspath(): the one after the other, as they are
    if (config.runFilename.isEmpty() || config.runFilename == "."_s)
        config.runFilename = decodeArgument(directory);
    else
        config.runFilename = makeString(decodeArgument(directory), '/', config.runFilename);
}

// config_update_argv()
void Reader::updateArguments(unsigned optionIndex)
{
    config.arguments.clear();
    if (arguments.size() <= optionIndex)
        config.arguments.append(emptyString());
    else
        config.arguments.append(arguments.subspan(optionIndex));
    if (!config.runCommand.isNull())
        config.arguments[0] = "-c"_s;
    else if (!config.runModule.isNull())
        config.arguments[0] = "-m"_s;
}

// config_init_warnoptions(). What is added last is looked at first, so what is to give way to the rest comes first.
void Reader::initializeWarningOptions(const Vector<String>& fromCommandLine, const Vector<String>& fromEnvironment)
{
    Vector<String> options;
    auto append = [&] (const String& option) {
        if (!config.warningOptions.contains(option) && !options.contains(option))
            options.append(option);
    };
    if (config.isDevelopmentMode)
        append("default"_s);
    for (auto& option : fromEnvironment)
        append(option);
    for (auto& option : fromCommandLine)
        append(option);
    if (config.bytesWarning)
        append(config.bytesWarning > 1 ? "error::BytesWarning"_s : "default::BytesWarning"_s);
    options.appendVector(config.warningOptions);
    config.warningOptions = WTF::move(options);
}

// config_read_env_vars()
Status Reader::readEnvironment()
{
    bool uses = config.usesEnvironment;
    raiseToEnvironmentFlag(uses, config.parserDebug, "PYTHONDEBUG");
    raiseToEnvironmentFlag(uses, config.verbose, "PYTHONVERBOSE");
    raiseToEnvironmentFlag(uses, config.optimizationLevel, "PYTHONOPTIMIZE");
    if (!config.inspect && environment("PYTHONINSPECT"))
        config.inspect = 1;
    auto isSet = [&] (const char* name) {
        int flag = 0;
        raiseToEnvironmentFlag(uses, flag, name);
        return !!flag;
    };
    if (isSet("PYTHONDONTWRITEBYTECODE"))
        config.writesBytecode = false;
    if (isSet("PYTHONNOUSERSITE"))
        config.usesUserSiteDirectory = false;
    if (isSet("PYTHONUNBUFFERED"))
        config.buffersStandardStreams = false;
    if (const char* path = environment("PYTHONPATH"); path && config.searchPathFromEnvironment.isNull())
        config.searchPathFromEnvironment = decodeArgument(path);
    if (const char* directory = environment("PYTHONPLATLIBDIR"))
        config.platformLibraryDirectory = decodeArgument(directory);

    // config_init_hash_seed(). It is looked at for the sake of saying what is wrong with it.
    if (const char* seed = environment("PYTHONHASHSEED"); seed && strcmp(seed, "random")) {
        char* end;
        errno = 0;
        unsigned long value = strtoul(seed, &end, 10);
        if (*end || value > 4294967295UL || (errno == ERANGE && value == ULONG_MAX))
            return Status::error("config_init_hash_seed"_s, "PYTHONHASHSEED must be \"random\" or an integer in range [0; 4294967295]"_s);
    }
    if (environment("PYTHONSAFEPATH"))
        config.hasSafePath = true;
    if (const char* gil = environment("PYTHON_GIL")) {
        if (!strcmp(gil, "0"))
            return Status::error("config_read_gil"_s, "Disabling the GIL is not supported by this build"_s);
        if (strcmp(gil, "1"))
            return Status::error("config_read_gil"_s, "PYTHON_GIL / -X gil must be \"0\" or \"1\""_s);
    }
    return Status::ok();
}

// config_init_thread_inherit_context() and config_init_context_aware_warnings()
Status Reader::readSwitch(bool& target, const char* variable, ASCIILiteral option, ASCIILiteral function, ASCIILiteral ifVariableIsWrong, ASCIILiteral ifOptionIsWrong)
{
    auto isSwitch = [] (std::optional<int> value) { return value && *value >= 0 && *value <= 1; };
    if (const char* value = environment(variable)) {
        auto number = toInt(value);
        if (!isSwitch(number))
            return Status::error(function, ifVariableIsWrong);
        target = *number;
    }
    if (const String* given = extraOption(option)) {
        auto value = afterSeparator(*given);
        auto number = value ? toInt(*value) : std::nullopt;
        if (!isSwitch(number))
            return Status::error(function, ifOptionIsWrong);
        target = *number;
    }
    return Status::ok();
}

// config_read_complex_options(). What there is nothing here to do anything with is looked at for the sake of saying what is wrong with it.
Status Reader::readComplexOptions()
{
    if (environment("PYTHONNODEBUGRANGES") || extraOption("no_debug_ranges"_s))
        config.hasDebugRanges = false;

    // config_init_import_time()
    {
        constexpr auto function = "config_init_import_time"_s;
        int importTime = 0;
        if (const char* variable = environment("PYTHONPROFILEIMPORTTIME")) {
            importTime = toInt(variable).value_or(1);
            if (importTime < 0 || importTime > 2)
                return Status::error(function, "PYTHONPROFILEIMPORTTIME: numeric values other than 1 and 2 are reserved for future use."_s);
        }
        if (auto value = extraOptionValue("importtime"_s)) {
            importTime = value->isEmpty() ? 1 : toInt(*value).value_or(1);
            if (importTime < 0 || importTime > 2)
                return Status::error(function, "-X importtime: values other than 1 and 2 are reserved for future use."_s);
        }
        config.importTime = importTime;
    }

    // config_init_tracemalloc()
    {
        constexpr auto function = "config_init_tracemalloc"_s;
        if (const char* variable = environment("PYTHONTRACEMALLOC")) {
            if (toInt(variable).value_or(-1) < 0)
                return Status::error(function, "PYTHONTRACEMALLOC: invalid number of frames"_s);
        }
        if (const String* option = extraOption("tracemalloc"_s)) {
            if (auto value = afterSeparator(*option); value && toInt(*value).value_or(-1) < 0)
                return Status::error(function, "-X tracemalloc=NFRAME: invalid number of frames"_s);
        }
    }

    // config_init_int_max_str_digits()
    {
        constexpr auto function = "config_init_int_max_str_digits"_s;
        constexpr int threshold = 640; // _PY_LONG_MAX_STR_DIGITS_THRESHOLD
        auto isValid = [] (std::optional<int> digits) { return digits && (!*digits || *digits >= threshold); };
        if (const char* variable = environment("PYTHONINTMAXSTRDIGITS")) {
            auto digits = toInt(variable);
            if (!isValid(digits))
                return Status::error(function, "PYTHONINTMAXSTRDIGITS: invalid limit; must be >= 640 or 0 for unlimited."_s);
            config.maximumDigitsOfIntAsString = *digits;
        }
        if (const String* option = extraOption("int_max_str_digits"_s)) {
            auto value = afterSeparator(*option);
            auto digits = value ? toInt(*value) : std::nullopt;
            if (!isValid(digits))
                return Status::error(function, "-X int_max_str_digits: invalid limit; must be >= 640 or 0 for unlimited."_s);
            config.maximumDigitsOfIntAsString = *digits;
        }
    }

    // config_init_cpu_count()
    {
        auto wrong = [] { return Status::error("config_init_cpu_count"_s, "-X cpu_count=n option: n is missing or an invalid number, n must be greater than 0"_s); };
        if (const char* variable = environment("PYTHON_CPU_COUNT")) {
            int count = -1;
            if (strcmp(variable, "default")) {
                count = toInt(variable).value_or(0);
                if (count < 1)
                    return wrong();
            }
            config.cpuCount = count;
        }
        if (const String* option = extraOption("cpu_count"_s)) {
            auto value = afterSeparator(*option);
            if (!value)
                return wrong();
            int count = -1;
            if (*value != "default"_s) {
                count = toInt(*value).value_or(0);
                if (count < 1)
                    return wrong();
            }
            config.cpuCount = count;
        }
    }

    // config_init_pycache_prefix(). If the option is there the environment is not looked at, even if the option is set to nothing.
    if (const String* option = extraOption("pycache_prefix"_s)) {
        if (auto value = afterSeparator(*option); value && !value->isEmpty())
            config.bytecodeCachePrefix = *value;
    } else if (const char* variable = environment("PYTHONPYCACHEPREFIX"))
        config.bytecodeCachePrefix = decodeArgument(variable);

    RETURN_IF_EXCEPTION_STATUS(readSwitch(config.threadsInheritContext, "PYTHON_THREAD_INHERIT_CONTEXT", "thread_inherit_context"_s, "config_init_thread_inherit_context"_s,
        "PYTHON_THREAD_INHERIT_CONTEXT=N: N is missing or invalid"_s, "-X thread_inherit_context=n: n is missing or invalid"_s));
    RETURN_IF_EXCEPTION_STATUS(readSwitch(config.hasContextAwareWarnings, "PYTHON_CONTEXT_AWARE_WARNINGS", "context_aware_warnings"_s, "config_init_context_aware_warnings"_s,
        "PYTHON_CONTEXT_AWARE_WARNINGS=N: N is missing or invalid"_s, "-X context_aware_warnings=n: n is missing or invalid"_s));
    return Status::ok();
}

// What config_init_import() does besides finding where things are
Status Reader::readFrozenModules()
{
    if (const char* variable = environment("PYTHON_FROZEN_MODULES")) {
        if (!strcmp(variable, "on"))
            config.usesFrozenModules = true;
        else if (!strcmp(variable, "off"))
            config.usesFrozenModules = false;
        else
            return Status::error({ }, "bad value for PYTHON_FROZEN_MODULES (expected \"on\" or \"off\")"_s);
    }
    if (auto value = extraOptionValue("frozen_modules"_s)) {
        if (*value == "on"_s || value->isEmpty())
            config.usesFrozenModules = true;
        else if (*value == "off"_s)
            config.usesFrozenModules = false;
        else
            return Status::error({ }, "bad value for option -X frozen_modules (expected \"on\" or \"off\")"_s);
    }
    return Status::ok();
}

// config_init_stdio_encoding()
void Reader::readStandardStreamEncoding()
{
    const char* variable = environment("PYTHONIOENCODING");
    if (!variable)
        return;
    String text = decodeArgument(variable);
    size_t separator = text.find(':');
    String encoding = text.left(separator == notFound ? text.length() : separator);
    String errors = separator == notFound ? String() : text.substring(separator + 1);
    if (errors.isEmpty())
        errors = String();
    if (!encoding.isEmpty()) {
        config.standardStreamEncoding = encoding;
        // An encoding by itself is that encoding, strictly.
        if (errors.isNull())
            errors = "strict"_s;
    }
    if (!errors.isNull())
        config.standardStreamErrors = errors;
}

// _PyConfig_Read()
Status Reader::read(std::span<const char* const> given)
{
    for (const char* argument : given)
        arguments.append(decodeArgument(argument));
    config.originalArguments = arguments;
    if (config.programName.isNull() && !arguments.isEmpty())
        config.programName = arguments[0];
    // What `python` does that a program that only imports something written in Python does not have done to it
    config.buffersStandardStreams = true;
    config.installsSignalHandlers = true;

    readBeforeCommandLine();
    RETURN_IF_EXCEPTION_STATUS(readUTF8Mode());
    if (config.isIsolated) {
        config.hasSafePath = true;
        config.usesEnvironment = false;
        config.usesUserSiteDirectory = false;
    }

    // config_read_cmdline()
    Vector<String> warningOptionsOfCommandLine;
    unsigned optionIndex = 0;
    RETURN_IF_EXCEPTION_STATUS(parseCommandLine(warningOptionsOfCommandLine, optionIndex));
    makeFilenameAbsolute();
    updateArguments(optionIndex);
    Vector<String> warningOptionsOfEnvironment;
    if (const char* variable = environment("PYTHONWARNINGS")) {
        for (auto option : StringView(decodeArgument(variable)).split(','))
            warningOptionsOfEnvironment.append(option.toString());
    }
    initializeWarningOptions(warningOptionsOfCommandLine, warningOptionsOfEnvironment);

    // config_read()
    if (config.usesEnvironment)
        RETURN_IF_EXCEPTION_STATUS(readEnvironment());
    if (auto gil = extraOptionValue("gil"_s)) {
        if (*gil == "0"_s)
            return Status::error("config_read_gil"_s, "Disabling the GIL is not supported by this build"_s);
        if (*gil != "1"_s)
            return Status::error("config_read_gil"_s, "PYTHON_GIL / -X gil must be \"0\" or \"1\""_s);
    }
    RETURN_IF_EXCEPTION_STATUS(readComplexOptions());
    if (const char* home = environment("PYTHONHOME"))
        config.home = decodeArgument(home);
    RETURN_IF_EXCEPTION_STATUS(readFrozenModules());
    readStandardStreamEncoding();
    // These are counted, and are then only yes or no. In CPython that comes of all this being made into a dict, for what works out where things are, and back: PyConfig_MEMBER_BOOL
    for (int* flag : { &config.parserDebug, &config.inspect, &config.interactive, &config.quiet })
        *flag = !!*flag;
    return Status::ok();
}

} // anonymous namespace

std::optional<int> readCommandLine(Configuration& configuration, std::span<const char* const> arguments)
{
    Status status = Reader(configuration).read(arguments);
    if (status.exitStatus) {
        fflush(stdout);
        return status.exitStatus;
    }
    if (status.message.isNull())
        return std::nullopt;
    // Py_ExitStatusException(), and fatal_error()
    fflush(stdout);
    if (status.function.isNull())
        fprintf(stderr, "Fatal Python error: %s\n", status.message.characters());
    else
        fprintf(stderr, "Fatal Python error: %s: %s\n", status.function.characters(), status.message.characters());
    fprintf(stderr, "Python runtime state: %s\n\n", status.runtimeState.characters());
    return 1;
}

} } // namespace JSC::Python
