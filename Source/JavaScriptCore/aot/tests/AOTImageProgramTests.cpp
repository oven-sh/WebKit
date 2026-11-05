/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBodyFactsForTables.h"
#include "AOTCompileQueue.h"
#include "AOTConvention.h"
#include "AOTIRParser.h"
#include "AOTImage.h"
#include "AOTImageBuilder.h"
#include "AOTImageTestSupport.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
#include "AOTNumbering.h"
#include "AOTProgramFile.h"
#include "AOTProgramTables.h"
#include "AOTRegExpCompiler.h"
#include "AOTRegExpRequests.h"
#include "AOTStages.h"
#include "AOTVector.h"
#include "AOTWasmInterface.h"
#include "ErrorInstance.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "TopExceptionScope.h"
#include "VM.h"
#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdio.h>
#include <utility>
#include <wtf/BitSet.h>
#include <wtf/DataLog.h>
#include <wtf/HexNumber.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

using RegExpFlags = OptionSet<Yarr::Flags>;

namespace {

struct ParsedProgram {
    BuildContext context;
    std::unique_ptr<Typed::Program> program;

    ParsedProgram(const String& text, const TestOptions& = { });
};

struct CompiledWholeProgram {
    PreparedProgram prepared;
    CompiledProgram units;

    explicit CompiledWholeProgram(ParsedProgram&);
};

struct PatternUse {
    UTF8CString bytes;
    RegExpFlags flags { };
};

struct CompiledRequests {
    ParsedProgram parsed;
    CompiledWholeProgram code;
    ImageNumbers numbers;
    const RegExpRequests& requests;
    CompiledRegExps regExps;

    CompiledRequests(const Vector<PatternUse>&, const TestOptions& = { });

    const Typed::Program& program() const { return *code.prepared.program; }
    String textOf(const RegExpRequest& request) const { return program().strings.toWTFString(request.pattern); }
};

} // anonymous namespace

ParsedProgram::ParsedProgram(const String& text, const TestOptions& options)
    : context(wireOptions(options))
    , program(Typed::parseProgram(text, context))
{
    AOT_TEST_CHECK(context.threadCount() == options.threadCount);
    AOT_TEST_CHECK(program);
}

static void checkBuildHasNoErrors(BuildContext& context, const Typed::Program* program)
{
    if (!context.diagnostics().hasErrors())
        return;
    context.diagnostics().finish(program);
    Typed::printDiagnostics(context.diagnostics(), WTF::dataFile());
    failNativeTest(__FILE__, __LINE__, "the build reports no error");
}

CompiledWholeProgram::CompiledWholeProgram(ParsedProgram& parsed)
{
    BuildContext& context = parsed.context;
    prepared.program = WTF::move(parsed.program);
    prepared.mechanisms = Typed::mechanismsOfProcess();
    bool isPrepared = prepareProgram(prepared, context);
    checkBuildHasNoErrors(context, prepared.program.get());
    AOT_TEST_CHECK(isPrepared);

    units = compileQueuedUnits(*prepared.compileInputs, prepared.program->types, context);
    checkBuildHasNoErrors(context, prepared.program.get());
    AOT_TEST_CHECK(units.succeeded && !units.functions.isEmpty());
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
    text.append("    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    for (size_t i = 0; i < uses.size(); ++i) {
        text.append("    v"_s, 2 * i + 2, " = NewRegExp pattern="_s);
        appendQuoted(text, bytesOf(uses[i].bytes));
        appendFlags(text, uses[i].flags);
        text.append("\n    v"_s, 2 * i + 3, " = Call v1, -, v"_s, 2 * i + 2, '\n');
    }
    text.append("    Return\n}\n"_s, runtimeFunctionsText(1));
    return text.toString();
}

static const RegExpRequests& collectRegExpRequests(CompiledWholeProgram& code, ImageNumbers& numbers, BuildContext& context)
{
    NumberingInputs inputs { *code.prepared.program, *code.prepared.facts, *code.prepared.plan, code.units.functions };
    GatheredMentions mentions = gatherMentions(inputs, context);
    NamesOfTables tables(*code.prepared.program);
    ImageNumbersBuilder builder { inputs, mentions, tables, numbers, context };
    builder.collectRegExpRequests();
    checkBuildHasNoErrors(context, code.prepared.program.get());
    return numbers.regExpRequests();
}

CompiledRequests::CompiledRequests(const Vector<PatternUse>& uses, const TestOptions& options)
    : parsed(patternsText(uses), options)
    , code(parsed)
    , requests(collectRegExpRequests(code, numbers, parsed.context))
    , regExps(compileRegExps(program(), requests, parsed.context))
{
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

static void testInsignificantFlags()
{
    CompiledRequests compiled({ { "a"_s }, { "a"_s, Yarr::Flags::Global }, { "a"_s, Yarr::Flags::IgnoreCase } });
    AOT_TEST_CHECK(compiled.requests.all().size() == 2);
    const RegExpRequest& plain = requestFor(compiled, "a"_s);
    for (const RegExpRequest& request : compiled.requests.all())
        AOT_TEST_CHECK(!request.flags.contains(Yarr::Flags::Global));

    Typed::RegExpRecordID record = compiled.requests.recordOf(plain.pattern, { });
    AOT_TEST_CHECK(record);
    AOT_TEST_CHECK(&compiled.requests.at(record) == &plain);
    AOT_TEST_CHECK(compiled.requests.recordOf(plain.pattern, Yarr::Flags::Global) == record);
    Typed::RegExpRecordID recordIgnoringCase = compiled.requests.recordOf(plain.pattern, Yarr::Flags::IgnoreCase);
    AOT_TEST_CHECK(recordIgnoringCase && recordIgnoringCase != record);
    AOT_TEST_CHECK(!compiled.requests.recordOf(plain.pattern, Yarr::Flags::Multiline));
}

#if ENABLE(WEBASSEMBLY_OMGJIT)

namespace {

} // anonymous namespace

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

static constexpr ASCIILiteral functionsWithOwnSpaces =
    "function f1 name=\"addsConstant\" parameters=[{type=number}] results=[{type=number}] {\n"
    "  b0(v0: number):\n"
    "    v1 = Double number=0.1\n"
    "    v2 = Add v0, v1\n"
    "    Return v2\n"
    "}\n"
    "function f2 name=\"makesRegExp\" results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = NewRegExp pattern=\"ab+c\"\n"
    "    Return v0\n"
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

static void appendFunctionThatCallsItself(StringBuilder& text, unsigned function)
{
    text.append("function f"_s, function, " name=\"callsItself\" parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n"_s);
    text.append("    v1 = ToBoolean v0\n    Branch v1 -> taken b1(), notTaken b2()\n  b1():\n    v2 = PropGet v0 key=\"next\"\n"_s);
    text.append("    v3 = CallDirect v2 function=f"_s, function, "\n    v4 = CallDirect v3 function=f"_s, function, "\n    Return v4\n  b2():\n    Return v0\n}\n"_s);
}

static void appendFunctionWithArrayLiteral(StringBuilder& text, unsigned function)
{
    constexpr unsigned numberOfElements = maxArrayElementsInRegisters + 1;
    static_assert(numberOfElements <= maxArrayLengthInStubKey);
    text.append("function f"_s, function, " name=\"listsValues\" parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    v1 = NewArray "_s);
    for (unsigned i = 0; i < numberOfElements; ++i)
        text.append(i ? ", v0"_s : "v0"_s);
    text.append(" contents=anything\n    Return v1\n}\n"_s);
}

static String programUsingSymbolSpaces()
{
    constexpr unsigned functionWithSwitch = 3;
    constexpr unsigned functionThatCallsItself = 4;
    constexpr unsigned functionWithArrayLiteral = 5;
    constexpr unsigned lastFunction = functionWithArrayLiteral + 40;
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
    appendFunctionWithArrayLiteral(text, functionWithArrayLiteral);
    for (unsigned function = functionWithArrayLiteral + 1; function <= lastFunction; ++function)
        appendFunctionWithAccesses(text, function);
    text.append(runtimeFunctionsText(lastFunction + 1));
    return text.toString();
}

static BuiltImage buildWholeProgram(ParsedProgram& parsed)
{
    BuildContext& context = parsed.context;
    CompiledWholeProgram code(parsed);
    Typed::Program& program = *code.prepared.program;
    BodyFactsForTables bodyFacts = collectBodyFactsForTables(program, *code.prepared.plan, code.units.functions, context);
    checkBuildHasNoErrors(context, &program);

    BuiltImage built = buildImage(ImageInputs {
        .program = program,
        .facts = *code.prepared.facts,
        .plan = *code.prepared.plan,
        .stubCatalog = *code.prepared.stubCatalog,
        .functions = WTF::move(code.units.functions),
        .bodyFacts = WTF::move(bodyFacts),
        .entriesUsedByFunctions = code.units.entriesUsed,
    }, context);
    checkBuildHasNoErrors(context, &program);
    AOT_TEST_CHECK(Image::check(AlignedBytes(built.image.span()).span(), testIdentity) == Wire::ImageVerdict::Accepted);
    return built;
}

static void testSymbolSpacesOfProgram()
{
    using enum SymbolSpace;
    WTF::BitSet<numberOfSymbolSpaces> usedSpaces;
    auto addSpacesOf = [&](std::span<const Relocation> relocations) {
        for (const Relocation& relocation : relocations)
            usedSpaces.set(static_cast<size_t>(relocation.space));
    };
    ParsedProgram parsed(programUsingSymbolSpaces());
    BuiltImage built = buildWholeProgram(parsed);
    bool mentionsRegExp = false;
    for (const CompiledCode& function : built.functions) {
        addSpacesOf(function.body.relocations);
        mentionsRegExp |= std::ranges::any_of(function.body.mentions, [](const Mention& mention) {
            return mention.space == NumberSpace::RegExp;
        });
    }
    addSpacesOf(built.sharedCode.blob.code.relocations);
    addSpacesOf(built.sharedCode.argumentThunks.code.relocations);
    for (SymbolSpace space : { Function, Stub, ArgumentThunk, Constant, JumpTable, Number, InstanceWord }) {
        if (!usedSpaces.get(static_cast<size_t>(space)))
            dataLogLn("no relocation names symbol space ", static_cast<size_t>(space));
        AOT_TEST_CHECK(usedSpaces.get(static_cast<size_t>(space)));
    }
    AOT_TEST_CHECK(mentionsRegExp);
}

static constexpr ASCIILiteral programForOneInstance = "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
    "entry\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
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

static void testRealmAfterRetiredRealm(LoadedProgram& program)
{
    AOT_TEST_CHECK(!(Image::current()->programTables().flags & ProgramTables::mayRunInMultipleInstances));
    program.realm()->setMicrotaskRunnability(QueuedTaskResult::Discard);
    Instance* second = Instance::of(createRealmWithHostOf(program));
    AOT_TEST_CHECK(second);
    AOT_TEST_CHECK(second != &program.instance());
    AOT_TEST_CHECK(second->state);
    AOT_TEST_CHECK(second->state != program.instance().state);
    AOT_TEST_CHECK(!Instance::of(createRealmWithHostOf(program)));
}

#if OS(LINUX)
namespace {

} // anonymous namespace

#endif

std::span<const NativeTest> programInputTests();

std::span<const NativeTest> programInputTests()
{
    static constexpr NativeTest tests[] = {
        { "image/regexp-queue/insignificant-flags"_s, testInsignificantFlags },
        { "image/programs/symbol-spaces"_s, testSymbolSpacesOfProgram },
        { "image/instances/second-realm-has-no-instance"_s, runNativeTestWithLoadedProgram<programInputTests, programForOneInstance, testSecondRealmHasNoInstance> },
        { "image/instances/realm-after-collected-realm"_s, runNativeTestWithLoadedProgram<programInputTests, programForOneInstance, testRealmAfterCollectedRealm> },
        { "image/instances/realm-after-retired-realm"_s, runNativeTestWithLoadedProgram<programInputTests, programForOneInstance, testRealmAfterRetiredRealm> },
#if OS(LINUX)
#endif
#if ENABLE(WEBASSEMBLY_OMGJIT)
#endif
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
