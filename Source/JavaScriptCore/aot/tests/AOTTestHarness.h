/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeRole.h"
#include "AOTCompileOptionWords.h"
#include "AOTIR.h"
#include "AOTMechanisms.h"
#include "AOTTestFile.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

class CompileOutput;
struct CompiledCode;
struct PreparedProgram;

namespace Machine {
class Procedure;
}

using OptionWords = std::initializer_list<ASCIILiteral>;

// A native test of a whole program compiles it in each of these ways and expects the same of both. Typed::mechanismsOfWay() reads the word.
inline constexpr ASCIILiteral waysToCompilePrograms[] { "mechanisms=all"_s, "mechanisms=none"_s };

// What every command and every test file starts from for its program: what `bun build --compile --aot` passes where nothing else is asked for (fill_record() in src/aot/engine.rs), so that testaot shows the Code that ships.
// `references=wide` is what a huge heap asks for. Bun also passes exclude=CodeOutsideImage unless code from disk is allowed: no word takes an exclusion back, so that one is left to the command line.
inline Wire::CompileOptions makeOptionsOfTestedProgram()
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    options.target.referenceWidth = Wire::ReferenceWidth::Narrow;
    return options;
}

namespace Typed {

class Code;
class Facts;
class Plan;
class TypeArena;
// facts/AOTLinkTables.h. As a build has it where nothing is said.
enum class OrderOfWorklist : uint8_t;

// ---- The command line. testaot.cpp
//
//   testaot all <folder>
//   testaot run <file or folder>.. [--filter=<text>] [--jobs=<n>] [--show-output] [--single-process] [--option=<option word>].. [--only-diagnostic=<kind>]
//   testaot native [<filter>] [--after=<name>] [--part=<i>/<n>] [--single-process]
//   testaot list commands|options|mechanisms|statistics|diagnostics|image-format
//   testaot <command> <program> { argument } { option word } [mechanisms=<list>] [--program-of-tag]      any name that `list commands` prints: `testaot emit f.ir f3`
//   testaot print <program> --wire
//   testaot compile <program> --output=<image> { option word | statistic } [mechanisms=<list>] [--program-of-tag]
//   testaot write-out <file>
//   testaot fuzz-allocation [--seed=<n>] [--count=<n>] [--blocks=<n>] [--show=<seed>]
//
// <program> is IR text, or the bytes that `bun build --aot-out=<file>` writes. --program-of-tag takes bytes that the binary of the tag wrote: SchemaHashCheck::Skipped.
// Each takes --mechanisms=<list>: what the list lacks is withheld in every build that the process and its children make, whatever a native test or a word of the command line asks for.
// A `RUN` of a file that asks for what the list lacks is left out.
// Each takes --show-refused-bodies: the process and its children print each body that the verifier of bodies refuses to stderr.

inline constexpr int exitCodeForSuccess = 0;
inline constexpr int exitCodeForFailure = 1;
inline constexpr int exitCodeForInvalidCommandLine = 2;

// AOTProgramSubcommands.cpp
int runCommandSubcommand(StringView command, std::span<const String> arguments);
int runPrintSubcommand(std::span<const String> arguments);
int runCompileSubcommand(std::span<const String> arguments);
// AOTMachineFuzzer.cpp
int runFuzzAllocationSubcommand(std::span<const String> arguments);

// The declared types of the parameters of the function, of f0 without a `TYPES` line. AOTStagedBuild.cpp
Vector<TypeID> typesNamedBy(const Program&, std::optional<FunctionID> functionOfTypesLine);

// ---- Which mechanisms of the middle a build has. AOTStagedBuild.cpp
// driver/AOTMechanismWords.h has the word `mechanisms=<list>` and its grammar: isMechanismsWord(), applyMechanismsWord().

inline constexpr ASCIILiteral mechanismsFlag = "--mechanisms"_s;

// `all`, `none`, or the names with commas between them, in the order of the enumerators.
void printMechanisms(PrintStream&, Mechanisms);
// All, until --mechanisms has said otherwise.
Mechanisms mechanismsOfProcess();
void setMechanismsOfProcess(Mechanisms);
// applyMechanismsWord(). Then what the process withholds goes, with all that leans on it.
std::optional<String> applyMechanismsWordOfTest(Mechanisms&, StringView word);
// applyMechanismsWord() alone, for whoever compares what is asked for with mechanismsOfProcess(). Nothing: no word has been applied yet, and the first starts from all.
std::optional<String> applyMechanismsWordAskedFor(std::optional<Mechanisms>&, StringView word);
// One of waysToCompilePrograms, or no word at all: mechanismsOfProcess().
Mechanisms mechanismsOfWay(ASCIILiteral way);

// compileParsedProgramWithMechanisms() of driver/AOTStages.h, for whoever includes this header alone.
std::unique_ptr<CompileOutput> compileWithMechanisms(std::unique_ptr<Program>, BuildContext&, Mechanisms);

// ---- One program taken through the stages, each at its first request and once. AOTStagedBuild.cpp

enum class MachineState : uint8_t {
    // As CodeGenerator::run() left them.
    Appended,
    // Behind Machine::prepareForGeneration(), with the allocation checked.
    Prepared,
};

// It rests on the driver's own pieces, so that a test stops between the stages of the real build: PreparedProgram and prepareProgram() of driver/AOTStages.h, and what a thread of Compile owns.
class StagedBuild {
    WTF_MAKE_NONCOPYABLE(StagedBuild);
public:
    StagedBuild(std::unique_ptr<Program>&&, BuildContext&, Mechanisms = mechanismsOfProcess(), OrderOfWorklist = { });
    ~StagedBuild();

    Program& program();
    BuildContext& build() { return m_build; }
    Mechanisms mechanisms() const;

    // All below answer null once a stage has reported an error. The build's diagnostics hold it.

    const Facts* facts();
    const Plan* plan();
    // Facts, Plan, the stub catalog, the heaps and the inputs of code generation.
    const PreparedProgram* prepared();

    // The arena of the one thread that walks. It is reset in front of each unit.
    TypeArena& types();
    // Walked and swept. Valid until the next request for a unit. With Mechanism::Arrivals the whole stage runs once in front of the first unit, so a callee takes what all its callers have left (walk/AOTArrivals.h), as it does in a build, in whatever order units are asked for.
    const Code* code(CodeUnit);
    // Valid until the next request for a unit.
    Machine::Procedure* machineOperations(CodeUnit, MachineState);
    // Of Code that no walk of this build has made: it is swept, its types lie in types(), and it outlives the answer.
    Machine::Procedure* machineOperationsOf(const Code&, MachineState);
    // What the last call of machineOperations() refused, with the products of the steps ahead of the failure. Null where it refused nothing.
    const Machine::Procedure* refusedMachineOperations() const;
    // From the input body to bytes, as a thread of Compile does it.
    const CompiledCode* compiledCode(CodeUnit);

    // The units that compileQueuedUnits() gives code, ascending.
    Vector<CodeUnit> unitsToCompile();

private:
    struct State;

    Machine::Procedure* emitMachineOperations(const Code&, MachineState);

    BuildContext& m_build;
    std::unique_ptr<State> m_state;
};

// `f3`, `f3:GenericCallEntry`, `f3:Serializer:2`: a function, a CodeRole by its C++ name, the layout of a serializer. Without a role: CodeRole::Body.
std::optional<CodeUnit> parseCodeUnit(StringView);
void printCodeUnit(PrintStream&, CodeUnit);

// ---- Commands: what a `RUN:` line or the command line names

struct TestCommandContext {
    // Read and verified. Null for a command that takes no program.
    std::unique_ptr<Program> program;
    std::span<const String> arguments;
    PrintStream& out;
    BuildContext& build;
    const Wire::CompileOptions* options { nullptr };
    std::span<const TestSection> sections { };
    std::optional<FunctionID> functionOfTypesLine { };
    // The text of the program, for a command that reads it a second time.
    StringView programText { };
    // Of the last `mechanisms=` word: of the `RUN` line or the command line, else of the `OPTIONS`. The word is no argument.
    Mechanisms mechanisms { mechanismsOfProcess() };

    const TestSection* findSection(ASCIILiteral name) const
    {
        for (const TestSection& section : sections) {
            if (section.name == name)
                return &section;
        }
        return nullptr;
    }
    // Takes the program.
    StagedBuild stage(OrderOfWorklist order = { }) { return StagedBuild(WTF::move(program), build, mechanisms, order); }
};

enum class ProgramOfCommand : uint8_t {
    None,
    // As the text has it.
    AsWritten,
    // With the stand-in library behind it: withStandInLibrary().
    WithLibrary,
};

struct TestCommand {
    ASCIILiteral name;
    void (*run)(TestCommandContext&);
    ProgramOfCommand program { ProgramOfCommand::AsWritten };
    // `{ argument }` as `list commands` prints it.
    ASCIILiteral synopsis { ""_s };
};

// Each provider is one source of tests/, named AOT<Stage>TestCommands.cpp. The first two belong to the areas whose code they run.
//
//   irTestCommands          print | round-trip | verify | body-index f | edge-results f | liveness f
//   typeTestCommands        subset | union | intersect | cast | canonical | rep | members | find-field L key [append-inherited] | layout-set L,L.. | truthy | edges | laws-of-types | aliases | memos-at-rest | memos-at-rest-untold
//   supportTestCommands     diagnostics | parallel-loops | build-context | hashed-index | crash
//   factsTestCommands       locks | summary f.. | link [part..] | closure
//   testRunsTestCommands    test-run [oldest-first|scattered]
//   planTestCommands        plan [part..]
//   jsonPlanTestCommands    json-plan
//   walkTestCommands        walk unit.. | code-round-trip | verify-code | sweep-code
//   machineTestCommands    emit unit.. | emit-code | order unit.. | liveness-of-operations unit.. | allocate unit.. | frame unit.. | bytes unit.. | bytes-by-form | named-forms operation.. | bytes-outside-operations | ranges unit..
//   programTestCommands     compile | run-in-shell { --<option> } { word } | image [section..] | same-bytes-alone unit.. | units-with-code unit..
#define FOR_EACH_AOT_TEST_COMMAND_PROVIDER(v) \
    v(irTestCommands) \
    v(typeTestCommands) \
    v(supportTestCommands) \
    v(factsTestCommands) \
    v(testRunsTestCommands) \
    v(planTestCommands) \
    v(jsonPlanTestCommands) \
    v(walkTestCommands) \
    v(machineTestCommands) \
    v(programTestCommands)

#define AOT_DECLARE_TEST_COMMAND_PROVIDER(name) std::span<const TestCommand> name();
FOR_EACH_AOT_TEST_COMMAND_PROVIDER(AOT_DECLARE_TEST_COMMAND_PROVIDER)
#undef AOT_DECLARE_TEST_COMMAND_PROVIDER

// AOTTestCommandRegistry.cpp
const TestCommand* findTestCommand(StringView name);
void checkTestCommandNames();
void listTestCommands(PrintStream&);

// The functions that the arguments name, ascending, or every function with a body where there is none. Prints `bad argument <word>` and answers nothing for a word that names no function.
std::optional<Vector<FunctionID>> functionsNamedBy(const Program&, std::span<const String> arguments, PrintStream&);
// Without arguments: the Body of every function with a body.
std::optional<Vector<CodeUnit>> unitsNamedBy(const Program&, std::span<const String> arguments, PrintStream&);
// The same in the order of the words, each as often as it is named.
std::optional<Vector<CodeUnit>> unitsInOrderOfWords(const Program&, std::span<const String> arguments, PrintStream&);
// Takes every `mechanisms=` word out of the words and applies it, from left to right: applyMechanismsWordAskedFor(). The answer is the sentence of the first word that is wrong.
std::optional<String> takeMechanismsWords(Vector<String>& words, std::optional<Mechanisms>& askedFor);

// Every pair of the program's types that breaks a law of the algebra of types, one line for each shape of pair. Answers how many shapes. AOTTypeTestCommands.cpp
unsigned listBrokenLawsOfTypes(PrintStream&, Program&, unsigned mostTypes);

// ---- For native tests

template<typename Range, typename T>
bool hasElements(const Range& range, std::initializer_list<T> expected)
{
    return std::ranges::equal(range, expected);
}

// A program with the built-ins' strings and nothing else. AOTStagedBuild.cpp
std::unique_ptr<Program> createEmptyProgram(BuildContext&);

// One line `<Kind>: <message>` for each record of UserDiagnostics and InternalErrors. AOTStagedBuild.cpp
void printDiagnosticsOf(const CompileOutput&, PrintStream&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
