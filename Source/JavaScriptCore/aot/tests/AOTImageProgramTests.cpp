/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTIRParser.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImageTestSupport.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
#include "AOTProgramTableEncoder.h"
#include "AOTProgramTables.h"
#include "AOTRegExpCompiler.h"
#include "AOTRegExpRequests.h"
#include "AOTStringArena.h"
#include "AOTVector.h"
#include "AOTWasmModules.h"
#include "ErrorInstance.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "TopExceptionScope.h"
#include "VM.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <span>
#include <stdio.h>
#include <utility>
#include <wtf/BitSet.h>
#include <wtf/HexNumber.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

using RegExpFlags = OptionSet<Yarr::Flags>;
using Typed::ValueID;

namespace {

struct ParsedProgram {
    BuildContext context;
    std::unique_ptr<Typed::Program> program;

    ParsedProgram(const String& text, const TestOptions& = { });

    Vector<UserDiagnostic> finishAndTake(UserDiagnosticKind);
};

struct PatternUse {
    UTF8CString bytes;
    RegExpFlags flags { };
};

struct CompiledRequests {
    ParsedProgram parsed;
    RegExpRequests requests;
    CompiledRegExps regExps;

    CompiledRequests(const Vector<PatternUse>&, const TestOptions& = { });

    String textOf(const RegExpRequest& request) const { return parsed.program->strings.toWTFString(request.pattern); }
};

} // anonymous namespace

ParsedProgram::ParsedProgram(const String& text, const TestOptions& options)
    : context(wireOptions(options))
    , program(Typed::parseProgram(text, Typed::IRForm::AsRead, context))
{
    AOT_TEST_CHECK(context.threadCount() == options.threadCount);
    AOT_TEST_CHECK(program);
}

Vector<UserDiagnostic> ParsedProgram::finishAndTake(UserDiagnosticKind kind)
{
    context.diagnostics().finish(program.get());
    Vector<UserDiagnostic> diagnostics = context.diagnostics().userDiagnostics();
    diagnostics.removeAllMatching([&](const UserDiagnostic& diagnostic) {
        return diagnostic.kind != kind;
    });
    return diagnostics;
}

static TestOptions withLoweredLimit(LimitID limit, uint64_t value)
{
    return { .loweredLimit = limit, .loweredLimitValue = value };
}

static TestOptions withThreads(unsigned threadCount)
{
    return { .threadCount = threadCount };
}

static std::span<const uint8_t> bytesOf(const UTF8CString& bytes)
{
    return byteCast<uint8_t>(bytes.span());
}

static void appendQuoted(StringBuilder& text, std::span<const uint8_t> bytes)
{
    text.append('"');
    for (uint8_t byte : bytes)
        text.append("\\x"_s, hex(byte, 2));
    text.append('"');
}

static void appendFlags(StringBuilder& text, RegExpFlags flags)
{
    static constexpr std::array<std::pair<Yarr::Flags, ASCIILiteral>, 8> names { {
        { Yarr::Flags::HasIndices, "HasIndices"_s }, { Yarr::Flags::Global, "Global"_s }, { Yarr::Flags::IgnoreCase, "IgnoreCase"_s }, { Yarr::Flags::Multiline, "Multiline"_s },
        { Yarr::Flags::DotAll, "DotAll"_s }, { Yarr::Flags::Unicode, "Unicode"_s }, { Yarr::Flags::UnicodeSets, "UnicodeSets"_s }, { Yarr::Flags::Sticky, "Sticky"_s },
    } };
    if (flags.isEmpty())
        return;
    text.append(" regExpFlags=["_s);
    ASCIILiteral separator = ""_s;
    for (auto [flag, name] : names) {
        if (!flags.contains(flag))
            continue;
        text.append(separator, name);
        separator = ", "_s;
    }
    text.append(']');
}

static String patternsText(const Vector<PatternUse>& uses)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry\n"_s);
    text.append("function f0 name=\"main\" kind=Module results=[{type=anything}] {\n  b0():\n"_s);
    for (size_t i = 0; i < uses.size(); ++i) {
        text.append("    v"_s, i, " = NewRegExp pattern="_s);
        appendQuoted(text, bytesOf(uses[i].bytes));
        appendFlags(text, uses[i].flags);
        text.append('\n');
    }
    text.append("    Return\n}\n"_s);
    return text.toString();
}

CompiledRequests::CompiledRequests(const Vector<PatternUse>& uses, const TestOptions& options)
    : parsed(patternsText(uses), options)
{
    Typed::FlowResults flow;
    Typed::StringArena strings(parsed.program->strings);
    requests = collectRegExpRequests(*parsed.program, flow, strings, parsed.context);
    regExps = compileRegExps(*parsed.program, requests, parsed.context);
    AOT_TEST_CHECK(requests.candidates().empty());
    AOT_TEST_CHECK(regExps.results.size() == requests.all().size());
}

static const RegExpRequest& requestFor(const CompiledRequests& compiled, const String& pattern, RegExpFlags flags = { })
{
    std::span<const RegExpRequest> requests = compiled.requests.all();
    auto request = std::ranges::find_if(requests, [&](const RegExpRequest& candidate) {
        return compiled.textOf(candidate) == pattern && candidate.flags == flags;
    });
    AOT_TEST_CHECK(request != requests.end());
    return *request;
}

static bool isSiteInMain(const RegExpSite& site, uint32_t value)
{
    return site == RegExpSite { FunctionID(0u), ValueID(value) };
}

static void testInsignificantFlags()
{
    CompiledRequests compiled({ { "a"_s }, { "a"_s, Yarr::Flags::Global }, { "a"_s, Yarr::Flags::IgnoreCase } });
    AOT_TEST_CHECK(compiled.requests.all().size() == 2);
    const RegExpRequest& plain = requestFor(compiled, "a"_s);
    AOT_TEST_CHECK(plain.sites.size() == 2);
    AOT_TEST_CHECK(isSiteInMain(plain.sites[0], 0));
    AOT_TEST_CHECK(isSiteInMain(plain.sites[1], 1));
    for (const RegExpRequest& request : compiled.requests.all())
        AOT_TEST_CHECK(!request.flags.contains(Yarr::Flags::Global));

    Typed::RegExpRecordID record = compiled.requests.recordOf(plain.pattern, { });
    AOT_TEST_CHECK(&compiled.requests.at(record) == &plain);
    AOT_TEST_CHECK(compiled.requests.recordOf(plain.pattern, Yarr::Flags::Global) == record);
    AOT_TEST_CHECK(compiled.requests.recordOf(plain.pattern, Yarr::Flags::IgnoreCase) != record);
}

static void testHasIndicesFlag()
{
    CompiledRequests compiled({ { "a"_s, Yarr::Flags::HasIndices }, { "a"_s, RegExpFlags { Yarr::Flags::Global, Yarr::Flags::HasIndices } } });
    AOT_TEST_CHECK(compiled.requests.all().size() == 1);
    AOT_TEST_CHECK(compiled.requests.all()[0].sites.size() == 2);
    AOT_TEST_CHECK(compiled.requests.all()[0].flags.isEmpty());
}

static void testEmptyAndSurrogate()
{
    static constexpr char loneSurrogate[] = { '\xed', '\xa0', '\x80' };
    CompiledRequests compiled({ { ""_s }, { UTF8CString::fromUTF8(loneSurrogate) } });
    AOT_TEST_CHECK(compiled.requests.all().size() == 2);

    unsigned numberOfEmpty = 0;
    unsigned numberOfSurrogates = 0;
    for (const RegExpRequest& request : compiled.requests.all()) {
        AOT_TEST_CHECK(request.pattern);
        String pattern = compiled.textOf(request);
        numberOfEmpty += pattern.isEmpty();
        numberOfSurrogates += pattern.length() == 1 && pattern[0] == 0xd800;
    }
    AOT_TEST_CHECK(numberOfEmpty == 1);
    AOT_TEST_CHECK(numberOfSurrogates == 1);
}

static void testTooLong()
{
    CompiledRequests compiled({ { "abcdef+"_s }, { "abcdef+"_s, Yarr::Flags::Global }, { "ab+"_s } }, withLoweredLimit(LimitID::RegExpPatternLengthWithCode, 4));
    const RegExpRequest& tooLong = requestFor(compiled, "abcdef+"_s);
    AOT_TEST_CHECK(tooLong.sites.size() == 2);
    const CompiledRegExp& result = compiled.regExps.results[&tooLong - compiled.requests.all().data()];
    AOT_TEST_CHECK(result.form == RegExpRecordForm::NoCode);
    AOT_TEST_CHECK(result.reason == RegExpDeclineReason::PatternTooLong);
    AOT_TEST_CHECK(compiled.regExps.numberWithForm(RegExpRecordForm::Code) == 1);
    AOT_TEST_CHECK(compiled.regExps.numberWithForm(RegExpRecordForm::NoCode) == 1);
    AOT_TEST_CHECK(!compiled.parsed.context.diagnostics().hasErrors());

    Vector<UserDiagnostic> diagnostics = compiled.parsed.finishAndTake(UserDiagnosticKind::RegExpWithoutCode);
    AOT_TEST_CHECK(diagnostics.size() == 2);
    for (size_t i = 0; i < diagnostics.size(); ++i) {
        AOT_TEST_CHECK(diagnostics[i].function == tooLong.sites[i].function);
        AOT_TEST_CHECK(diagnostics[i].value == tooLong.sites[i].value);
        AOT_TEST_CHECK(diagnostics[i].limit == LimitID::RegExpPatternLengthWithCode);
        AOT_TEST_CHECK(diagnostics[i].count == static_cast<uint32_t>(RegExpDeclineReason::PatternTooLong));
    }
}

static void testResultsBelongToRequests()
{
    Vector<PatternUse> uses;
    for (unsigned k = 1; k <= 40; ++k)
        uses.append({ makeString("a{"_s, k, "}b+"_s).utf8() });

    CompiledRequests serial(uses, withThreads(1));
    CompiledRequests parallel(uses, withThreads(8));
    AOT_TEST_CHECK(serial.requests.all().size() == uses.size());
    AOT_TEST_CHECK(parallel.regExps.results.size() == serial.regExps.results.size());
    for (size_t i = 0; i < serial.regExps.results.size(); ++i) {
        AOT_TEST_CHECK(serial.textOf(serial.requests.all()[i]) == parallel.textOf(parallel.requests.all()[i]));
        AOT_TEST_CHECK(serial.regExps.results[i].form == parallel.regExps.results[i].form);
        AOT_TEST_CHECK(serial.regExps.results[i].reason == parallel.regExps.results[i].reason);
        AOT_TEST_CHECK(serial.regExps.results[i].codeSize() == parallel.regExps.results[i].codeSize());
    }
}

static void testUnicodeProperties()
{
    CompiledRequests compiled({ { "\\p{L}+"_s, Yarr::Flags::Unicode }, { "[\\P{Lu}a]"_s, Yarr::Flags::Unicode }, { "[\\p{L}--a]"_s, Yarr::Flags::UnicodeSets }, { "ab+"_s } });
    AOT_TEST_CHECK(compiled.requests.all().size() == 4);
    AOT_TEST_CHECK(compiled.regExps.numberWithUnicodePropertyLookup == 3);
}

static void testUnicodePropertiesWithAddedFlag()
{
    CompiledRequests compiled({ { "\\p{L}+"_s }, { "\\p{NoSuchProperty}"_s }, { "[a-\\p{L}]"_s }, { "[\\q{a}]\\P{RGI_Emoji}"_s } });
    AOT_TEST_CHECK(compiled.requests.all().size() == 4);
    AOT_TEST_CHECK(compiled.regExps.numberWithUnicodePropertyLookup == 4);
}

static void testNoUnicodeProperties()
{
    CompiledRequests compiled({ { "a|b"_s }, { "ab+"_s, Yarr::Flags::Unicode }, { "[\\w-.]"_s }, { "a\\-\\p{L}"_s }, { "\\\\p{L}"_s } });
    AOT_TEST_CHECK(compiled.requests.all().size() == 5);
    AOT_TEST_CHECK(!compiled.regExps.numberWithUnicodePropertyLookup);
}

static void testNoRegExps()
{
    CompiledRequests compiled({ });
    AOT_TEST_CHECK(compiled.requests.all().empty());
    AOT_TEST_CHECK(compiled.regExps.results.isEmpty());
    for (RegExpRecordForm form : { RegExpRecordForm::Code, RegExpRecordForm::Atom, RegExpRecordForm::NoCode, RegExpRecordForm::NoMetadata })
        AOT_TEST_CHECK(!compiled.regExps.numberWithForm(form));
    AOT_TEST_CHECK(!compiled.regExps.numberWithUnicodePropertyLookup);
}

#if ENABLE(WEBASSEMBLY_OMGJIT)

static constexpr uint8_t moduleReturning42[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x05, 0x01, 0x01, 0x66, 0x00, 0x00,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b,
};

static constexpr uint8_t moduleReturning43[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x05, 0x01, 0x01, 0x66, 0x00, 0x00,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2b, 0x0b,
};

static constexpr uint8_t invalidModule[] = { 0xde, 0xad, 0xbe, 0xef };

static String webAssemblyText(size_t numberOfEntries)
{
    StringBuilder text;
    for (size_t k = 0; k < std::max<size_t>(numberOfEntries, 1); ++k)
        text.append("module m"_s, k, " path=\"m"_s, k, ".js\" displayPath=\"m"_s, k, ".js\"\n"_s);
    text.append("entry\n"_s);
    for (size_t k = 0; k < numberOfEntries; ++k)
        text.append("webassemblymodule module=m"_s, k, '\n');
    return text.toString();
}

namespace {

struct CompiledWebAssembly {
    ParsedProgram parsed;
    WebAssemblyImageInputs modules;
    Vector<UserDiagnostic> invalidModules;

    CompiledWebAssembly(std::initializer_list<std::span<const uint8_t>>);
};

} // anonymous namespace

CompiledWebAssembly::CompiledWebAssembly(std::initializer_list<std::span<const uint8_t>> entries)
    : parsed(webAssemblyText(entries.size()))
{
    size_t index = 0;
    for (std::span<const uint8_t> bytes : entries)
        parsed.program->webAssemblyModules[index++].bytes = bytes;

    std::expected<CodeTemplates, LinkFailure> templates = CodeTemplates::create(parsed.context.assemblerPolicy(), &parsed.context);
    AOT_TEST_CHECK(templates);
    {
        RefPtr<VM> vm = VM::create(HeapType::Small);
        JSLockHolder locker(*vm);
        modules = compileWebAssemblyModules(*vm, *parsed.program, *templates, parsed.context);
        vm = nullptr;
    }
    AOT_TEST_CHECK(!parsed.context.diagnostics().hasErrors());
    invalidModules = parsed.finishAndTake(UserDiagnosticKind::WasmInvalidModule);
}

static void checkHasCode(const CompiledWebAssembly& compiled, uint32_t numberOfModules)
{
    AOT_TEST_CHECK(compiled.modules.numberOfModules == numberOfModules);
    AOT_TEST_CHECK(!compiled.modules.code.isEmpty());
    AOT_TEST_CHECK(!compiled.modules.directory.isEmpty());
    AOT_TEST_CHECK(compiled.invalidModules.isEmpty());
}

static void testOneModule()
{
    checkHasCode(CompiledWebAssembly({ moduleReturning42 }), 1);
}

static void testSameModuleTwice()
{
    CompiledWebAssembly once({ moduleReturning42 });
    CompiledWebAssembly twice({ moduleReturning42, moduleReturning42 });
    checkHasCode(twice, 1);
    AOT_TEST_CHECK(twice.modules.code == once.modules.code);
    AOT_TEST_CHECK(twice.modules.directory == once.modules.directory);
}

static void testTwoModules()
{
    checkHasCode(CompiledWebAssembly({ moduleReturning42, moduleReturning43 }), 2);
}

static void testInvalidModule()
{
    CompiledWebAssembly compiled({ invalidModule });
    AOT_TEST_CHECK(compiled.invalidModules.size() == 1);
    AOT_TEST_CHECK(compiled.invalidModules[0].location.module == 0);
    AOT_TEST_CHECK(!compiled.modules.numberOfModules);
}

static void testInvalidModuleTwice()
{
    CompiledWebAssembly compiled({ invalidModule, invalidModule });
    AOT_TEST_CHECK(compiled.invalidModules.size() == 2);
    AOT_TEST_CHECK(compiled.invalidModules[0].location.module == 0);
    AOT_TEST_CHECK(compiled.invalidModules[1].location.module == 1);
    AOT_TEST_CHECK(!compiled.modules.numberOfModules);
}

static void testValidBesideInvalid()
{
    CompiledWebAssembly compiled({ invalidModule, moduleReturning42 });
    AOT_TEST_CHECK(compiled.modules.numberOfModules == 1);
    AOT_TEST_CHECK(compiled.invalidModules.size() == 1);
    AOT_TEST_CHECK(compiled.invalidModules[0].location.module == 0);
}

static void testNoWebAssemblyModules()
{
    CompiledWebAssembly compiled({ });
    AOT_TEST_CHECK(!compiled.modules.numberOfModules);
    AOT_TEST_CHECK(compiled.modules.code.isEmpty());
    AOT_TEST_CHECK(compiled.modules.directory.isEmpty());
}

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

static constexpr ASCIILiteral functionsWithOwnSpaces =
    "function f1 name=\"addsConstant\" parameters=[{type=number}] results=[{type=number}] {\n"
    "  b0(v0: number):\n"
    "    v1 = Double number=0.1\n"
    "    v2 = Add v0, v1\n"
    "    Return v2\n"
    "}\n"
    "function f2 name=\"matches\" parameters=[{type=string}] results=[{type=anything}] {\n"
    "  b0(v0: string):\n"
    "    v1 = NewRegExp pattern=\"ab+c\"\n"
    "    v2 = PropGet v1 key=\"test\"\n"
    "    v3 = Call v2, v1, v0\n"
    "    Return v3\n"
    "}\n"_s;

static void appendFunctionWithSwitch(StringBuilder& text, unsigned function)
{
    constexpr unsigned numberOfCases = 8;
    text.append("function f"_s, function, " name=\"switches\" parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    Switch v0 cases=["_s);
    for (unsigned i = 0; i < numberOfCases; ++i)
        text.append(i ? ", "_s : ""_s, i);
    text.append("] -> "_s);
    for (unsigned i = 0; i < numberOfCases; ++i)
        text.append("case b"_s, i + 1, "(), "_s);
    text.append("otherwise b"_s, numberOfCases + 1, "()\n"_s);
    for (unsigned i = 0; i < numberOfCases; ++i)
        text.append("  b"_s, i + 1, "():\n    v"_s, i + 1, " = Integer integer="_s, 11 * i + 10, "\n    Return v"_s, i + 1, '\n');
    text.append("  b"_s, numberOfCases + 1, "():\n    Return v0\n}\n"_s);
}

static void appendFunctionWithAccesses(StringBuilder& text, unsigned function)
{
    text.append("function f"_s, function, " name=\"f"_s, function, "\" parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
    text.append("    v1 = PropGet v0 key=\"a\"\n    v2 = PropGet v1 key=\"b\"\n    v3 = PropGet v2 key=\"c\"\n"_s);
    text.append("    v4 = Integer integer="_s, function, "\n    v5 = Call v3, v2, v4, v1, v1, v1, v1, v1, v1, v1, v1\n    Return v5\n}\n"_s);
}

// A function that calls itself is never wholly in line, so a call that names a function stays.
static void appendFunctionThatCallsItself(StringBuilder& text, unsigned function)
{
    text.append("function f"_s, function, " name=\"callsItself\" parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
    text.append("    v1 = ToBoolean v0\n    Branch v1 -> taken b1(), notTaken b2()\n  b1():\n    v2 = PropGet v0 key=\"next\"\n"_s);
    text.append("    v3 = CallDirect v2 function=f"_s, function, "\n    v4 = CallDirect v3 function=f"_s, function, "\n    Return v4\n  b2():\n    Return v0\n}\n"_s);
}

static String programUsingSymbolSpaces()
{
    constexpr unsigned functionWithSwitch = 3;
    constexpr unsigned functionThatCallsItself = 4;
    constexpr unsigned lastFunction = functionThatCallsItself + 40;
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\nentry\n"_s);
    text.append("function f0 name=\"main\" kind=Module results=[{type=anything}] {\n  b0():\n"_s);
    text.append("    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    for (unsigned function = 1; function <= lastFunction; ++function) {
        text.append("    v"_s, 2 * function, " = NewClosure function=f"_s, function, '\n');
        text.append("    v"_s, 2 * function + 1, " = Call v1, -, v"_s, 2 * function, '\n');
    }
    text.append("    Return\n}\n"_s, functionsWithOwnSpaces);
    appendFunctionWithSwitch(text, functionWithSwitch);
    appendFunctionThatCallsItself(text, functionThatCallsItself);
    for (unsigned function = functionThatCallsItself + 1; function <= lastFunction; ++function)
        appendFunctionWithAccesses(text, function);
    text.append(runtimeFunctionsText(lastFunction + 1));
    return text.toString();
}

static BuiltImage buildWholeProgram(ParsedProgram& parsed)
{
    BuildContext& context = parsed.context;
    std::unique_ptr<Typed::PipelineOutput> pipeline = Typed::runPipeline(WTF::move(parsed.program), context);
    AOT_TEST_CHECK(pipeline && pipeline->isComplete);
    CodeGenerationPlan plan = prepareCodeGeneration(*pipeline, context);
    GeneratedCode generated = generateAllCode(createCodeGenerationInputs(*pipeline, plan), *pipeline->program, context);
    for (const CompiledCode& code : generated.functions) {
        const Typed::Function& function = pipeline->program->at(code.function);
        AOT_TEST_CHECK(function.bodyIsReleased && function.hasBody() && function.blocks.isEmpty() && function.values.isEmpty());
    }
    Typed::EncodedProgramTables tables = Typed::encodeProgramTables({ *pipeline, plan.instanceSizes, plan.stringConstants, plan.bodyFactsForTables }, context);
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());

    BuiltImage built = buildImage(ImageInputs {
        .pipeline = *pipeline,
        .stubCatalog = *plan.stubCatalog,
        .instanceSizes = plan.instanceSizes,
        .functions = WTF::move(generated.functions),
        .entriesUsedByFunctions = generated.entriesUsed,
        .programTables = WTF::move(tables.bytes),
        .text = encodeText({ *pipeline->program, plan.uses.strings, context.identifierNumbering(), plan.stringConstants, tables.namedStrings }, context),
        .regExpRequests = *pipeline->regExpRequests,
        .regExps = compileRegExps(*pipeline->program, *pipeline->regExpRequests, context),
        .webAssembly = { },
        .ffiStubs = { },
    }, context);
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
    AOT_TEST_CHECK(Image::check(AlignedBytes(built.image.span()).span(), testIdentity) == Wire::ImageVerdict::Accepted);
    return built;
}

static void testSymbolSpacesOfProgram()
{
    using enum SymbolSpace;
    WTF::BitSet<numberOfSymbolSpaces> usedSpaces;
    auto addSpacesOf = [&](const Vector<Relocation>& relocations) {
        for (const Relocation& relocation : relocations)
            usedSpaces.set(static_cast<size_t>(relocation.space));
    };
    // Cold code is outlined and asks a shared stub, which needs no cache of the site. Hot code names the cache of its site.
    for (Wire::HeatOverride heat : { Wire::HeatOverride::EverythingIsCold, Wire::HeatOverride::EverythingIsHot }) {
        ParsedProgram parsed(programUsingSymbolSpaces(), { .heat = heat });
        BuiltImage built = buildWholeProgram(parsed);
        for (const CompiledCode& function : built.functions)
            addSpacesOf(function.body.relocations);
        addSpacesOf(built.sharedCode.blob.code.relocations);
        addSpacesOf(built.sharedCode.argumentThunks.code.relocations);
        addSpacesOf(built.outlinedCode.sharedBodies.relocations);
        addSpacesOf(built.outlinedCode.ownBodies.relocations);
    }
    for (SymbolSpace space : { Function, Stub, ArgumentThunk, OutlinedBody, Constant, JumpTable, SiteCache, RegExp }) {
        if (!usedSpaces.get(static_cast<size_t>(space)))
            dataLogLn("no relocation names symbol space ", static_cast<size_t>(space));
        AOT_TEST_CHECK(usedSpaces.get(static_cast<size_t>(space)));
    }
}

static constexpr ASCIILiteral programForOneInstance = "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
    "entry\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

static constexpr ASCIILiteral programForManyInstances = "variable g0 name=\"worker\" declared={type=anything}\n"
    "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0 exports=[{name=\"worker\" variable=g0}]\n"
    "entry hostReads=[{exportName=\"worker\"}]\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=Worker\n"
    "    PutModuleVariable v0 variable=g0\n"
    "    Return\n"
    "}\n"_s;

static JSGlobalObject* createRealmWithHostOf(LoadedProgram& program, VM& vm)
{
    JSGlobalObject* realm = JSGlobalObject::createWithCustomMethodTable(vm, JSGlobalObject::createStructure(vm, jsNull()), program.realm()->globalObjectMethodTable());
    AOT_TEST_CHECK(realm->globalObjectMethodTable()->moduleHost);
    return realm;
}

static JSGlobalObject* createRealmWithHostOf(LoadedProgram& program)
{
    return createRealmWithHostOf(program, program.vm());
}

static void testSecondRealmHasNoInstance(LoadedProgram& program)
{
    AOT_TEST_CHECK(!(Image::current()->programTables().flags & ProgramTables::mayRunInMultipleInstances));
    JSGlobalObject* second = createRealmWithHostOf(program);
    AOT_TEST_CHECK(!Instance::of(second));
    AOT_TEST_CHECK(Instance::of(program.realm()) == &program.instance());

    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    AOT_TEST_CHECK(!AOT::importModule(second, ModuleID(0u), std::nullopt));
    AOT_TEST_CHECK(scope.exception());
    auto* error = dynamicDowncast<ErrorInstance>(scope.exception()->value());
    AOT_TEST_CHECK(error && error->errorType() == ErrorType::SyntaxError);
    scope.clearException();
}

static void testRealmAfterCollectedRealm(LoadedProgram& program)
{
    RefPtr<VM> vm = VM::create();
    Thread::create("AOT realm test"_s, [&] {
        JSLockHolder locker(*vm);
        AOT_TEST_CHECK(Instance::of(createRealmWithHostOf(program, *vm)));
        AOT_TEST_CHECK(!Instance::of(createRealmWithHostOf(program, *vm)));
    })->waitForCompletion();

    JSLockHolder locker(*vm);
    vm->heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(Instance::of(createRealmWithHostOf(program, *vm)));
    vm = nullptr;
}

static void testTwoRealmsHaveInstances(LoadedProgram& program)
{
    // No built-in of Bun's starts a second instance in the VM of the program, so the compiler writes the flag for no program. The run time keeps the instances of one VM apart for a host that has such a built-in.
    const ProgramTables& tables = Image::current()->programTables();
    AOT_TEST_CHECK(!(tables.flags & ProgramTables::mayRunInMultipleInstances));
    overwriteWordInImage(tables.flags, tables.flags | ProgramTables::mayRunInMultipleInstances);
    Instance* second = Instance::of(createRealmWithHostOf(program));
    AOT_TEST_CHECK(second);
    AOT_TEST_CHECK(second != &program.instance());
    AOT_TEST_CHECK(second->state);
    AOT_TEST_CHECK(second->state != program.instance().state);
    AOT_TEST_CHECK(!Instance::of(program.realms().plain()));
}

#if OS(LINUX)
namespace {

struct MappedPages {
    uintptr_t start;
    uintptr_t end;
    bool isWritable;
    bool isExecutable;
};

} // anonymous namespace

static Vector<MappedPages> readMappedPages()
{
    FILE* file = fopen("/proc/self/maps", "r");
    AOT_TEST_CHECK(file);
    Vector<MappedPages> result;
    unsigned long start;
    unsigned long end;
    char permissions[5];
    while (fscanf(file, "%lx-%lx %4s%*[^\n]", &start, &end, permissions) == 3)
        result.append({ start, end, permissions[1] == 'w', permissions[2] == 'x' });
    fclose(file);
    return result;
}

static void testLoadedImageHasNoWritablePage(LoadedProgram&)
{
    const Image& image = *Image::current();
    uintptr_t start = std::bit_cast<uintptr_t>(&image.header());
    uintptr_t end = start + image.header().size;
    uintptr_t codeStart = std::bit_cast<uintptr_t>(image.code().data());
    uintptr_t codeEnd = codeStart + image.header().size - image.header().codeOffset;
    AOT_TEST_CHECK(codeEnd <= start || codeStart >= end);

    uintptr_t checkedEnd = start;
    uintptr_t checkedCodeEnd = codeStart;
    bool isWritableAfterImage = false;
    for (const MappedPages& pages : readMappedPages()) {
        AOT_TEST_CHECK(!pages.isWritable || !pages.isExecutable);
        if (pages.start <= end && end < pages.end)
            isWritableAfterImage = pages.isWritable;
        if (pages.end > codeStart && pages.start < codeEnd) {
            AOT_TEST_CHECK(pages.start <= checkedCodeEnd);
            AOT_TEST_CHECK(pages.isExecutable);
            checkedCodeEnd = pages.end;
        }
        if (pages.end <= start || pages.start >= end)
            continue;
        AOT_TEST_CHECK(pages.start <= checkedEnd);
        AOT_TEST_CHECK(!pages.isWritable);
        AOT_TEST_CHECK(!pages.isExecutable);
        checkedEnd = pages.end;
    }
    AOT_TEST_CHECK(checkedEnd == end);
    AOT_TEST_CHECK(checkedCodeEnd >= codeEnd);

    AOT_TEST_CHECK(isWritableAfterImage);
    auto* byteAfterImage = std::bit_cast<volatile uint8_t*>(end);
    *byteAfterImage = 1;
    AOT_TEST_CHECK(*byteAfterImage == 1);
}
#endif

std::span<const NativeTest> programInputTests();

std::span<const NativeTest> programInputTests()
{
    static constexpr NativeTest tests[] = {
        { "image/regexp-queue/insignificant-flags"_s, testInsignificantFlags },
        { "image/regexp-queue/has-indices-flag"_s, testHasIndicesFlag },
        { "image/regexp-queue/empty-and-surrogate"_s, testEmptyAndSurrogate },
        { "image/regexp-queue/too-long"_s, testTooLong },
        { "image/regexp-queue/results-belong-to-requests"_s, testResultsBelongToRequests },
        { "image/regexp-queue/unicode-properties"_s, testUnicodeProperties },
        { "image/regexp-queue/unicode-properties-with-added-flag"_s, testUnicodePropertiesWithAddedFlag },
        { "image/regexp-queue/no-unicode-properties"_s, testNoUnicodeProperties },
        { "image/regexp-queue/none"_s, testNoRegExps },
        { "image/programs/symbol-spaces"_s, testSymbolSpacesOfProgram },
        { "image/instances/second-realm-has-no-instance"_s, runNativeTestWithLoadedProgram<programInputTests, programForOneInstance, testSecondRealmHasNoInstance> },
        { "image/instances/realm-after-collected-realm"_s, runNativeTestWithLoadedProgram<programInputTests, programForOneInstance, testRealmAfterCollectedRealm> },
        { "image/instances/two-realms-have-instances"_s, runNativeTestWithLoadedProgram<programInputTests, programForManyInstances, testTwoRealmsHaveInstances> },
#if OS(LINUX)
        { "image/loading/loaded-image-has-no-writable-page"_s, runNativeTestWithLoadedProgram<programInputTests, programForOneInstance, testLoadedImageHasNoWritablePage> },
#endif
#if ENABLE(WEBASSEMBLY_OMGJIT)
        { "image/wasm-modules/one"_s, testOneModule },
        { "image/wasm-modules/same-module-twice"_s, testSameModuleTwice },
        { "image/wasm-modules/two-modules"_s, testTwoModules },
        { "image/wasm-modules/invalid"_s, testInvalidModule },
        { "image/wasm-modules/invalid-twice"_s, testInvalidModuleTwice },
        { "image/wasm-modules/valid-beside-invalid"_s, testValidBesideInvalid },
        { "image/wasm-modules/none"_s, testNoWebAssemblyModules },
#endif
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
