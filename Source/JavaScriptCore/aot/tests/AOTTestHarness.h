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

inline constexpr ASCIILiteral waysToCompilePrograms[] { "mechanisms=all"_s, "mechanisms=none"_s };

inline Wire::CompileOptions makeOptionsOfTestedProgram()
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    options.target.referenceWidth = Wire::ReferenceWidth::Narrow;
    return options;
}

namespace Typed {

class Code;
struct CompileInputs;
class Facts;
class Plan;
class TypeArena;
enum class OrderOfWorklist : uint8_t;

inline constexpr int exitCodeForSuccess = 0;
inline constexpr int exitCodeForFailure = 1;
inline constexpr int exitCodeForInvalidCommandLine = 2;

int runCommandSubcommand(StringView command, std::span<const String> arguments);
int runPrintSubcommand(std::span<const String> arguments);
int runCompileSubcommand(std::span<const String> arguments);
int runFuzzAllocationSubcommand(std::span<const String> arguments);
int runFuzzExecutionSubcommand(std::span<const String> arguments);

Vector<TypeID> typesNamedBy(const Program&, std::optional<FunctionID> functionOfTypesLine);

inline constexpr ASCIILiteral mechanismsFlag = "--mechanisms"_s;

void printMechanisms(PrintStream&, Mechanisms);
Mechanisms mechanismsOfProcess();
void setMechanismsOfProcess(Mechanisms);
std::optional<String> applyMechanismsWordOfTest(Mechanisms&, StringView word);
std::optional<String> applyMechanismsWordAskedFor(std::optional<Mechanisms>&, StringView word);
Mechanisms mechanismsOfWay(ASCIILiteral way);

std::unique_ptr<CompileOutput> compileWithMechanisms(std::unique_ptr<Program>, BuildContext&, Mechanisms);

enum class MachineState : uint8_t {
    Appended,
    Prepared,
};

class ScopeOfTestThatRunsAsUsersBuild {
    WTF_MAKE_NONCOPYABLE(ScopeOfTestThatRunsAsUsersBuild);
public:
    explicit ScopeOfTestThatRunsAsUsersBuild(bool);
    ~ScopeOfTestThatRunsAsUsersBuild();

private:
    bool m_before;
};

class StagedBuild {
    WTF_MAKE_NONCOPYABLE(StagedBuild);
public:
    StagedBuild(std::unique_ptr<Program>&&, BuildContext&, Mechanisms = mechanismsOfProcess(), OrderOfWorklist = { });
    ~StagedBuild();

    Program& program();
    BuildContext& build() { return m_build; }
    Mechanisms mechanisms() const;

    const Facts* facts();
    const Plan* plan();
    const PreparedProgram* prepared();
    std::unique_ptr<CompileInputs> prepareInputsForVerification();

    TypeArena& types();
    const Code* code(CodeUnit);
    Machine::Procedure* machineOperations(CodeUnit, MachineState);
    Machine::Procedure* machineOperationsOf(const Code&, MachineState);
    const Machine::Procedure* refusedMachineOperations() const;
    const CompiledCode* compiledCode(CodeUnit);

    Vector<CodeUnit> unitsToCompile();

private:
    struct State;

    Machine::Procedure* emitMachineOperations(const Code&, MachineState);

    BuildContext& m_build;
    std::unique_ptr<State> m_state;
};

std::optional<CodeUnit> parseCodeUnit(StringView);
void printCodeUnit(PrintStream&, CodeUnit);

struct TestCommandContext {
    std::unique_ptr<Program> program;
    std::span<const String> arguments;
    PrintStream& out;
    BuildContext& build;
    const Wire::CompileOptions* options { nullptr };
    std::span<const TestSection> sections { };
    std::optional<FunctionID> functionOfTypesLine { };
    StringView programText { };
    Mechanisms mechanisms { mechanismsOfProcess() };
    Vector<String> notesForFailure { };

    const TestSection* findSection(ASCIILiteral name) const
    {
        for (const TestSection& section : sections) {
            if (section.name == name)
                return &section;
        }
        return nullptr;
    }
    StagedBuild stage(OrderOfWorklist order = { }) { return StagedBuild(WTF::move(program), build, mechanisms, order); }
};

enum class ProgramOfCommand : uint8_t {
    None,
    AsWritten,
    WithLibrary,
};

struct TestCommand {
    ASCIILiteral name;
    void (*run)(TestCommandContext&);
    ProgramOfCommand program { ProgramOfCommand::AsWritten };
    ASCIILiteral synopsis { ""_s };
};

#define FOR_EACH_AOT_TEST_COMMAND_PROVIDER(v) \
    v(irTestCommands) \
    v(typeTestCommands) \
    v(supportTestCommands) \
    v(factsTestCommands) \
    v(testRunsTestCommands) \
    v(planTestCommands) \
    v(walkTestCommands) \
    v(machineTestCommands) \
    v(programTestCommands)

#define AOT_DECLARE_TEST_COMMAND_PROVIDER(name) std::span<const TestCommand> name();
FOR_EACH_AOT_TEST_COMMAND_PROVIDER(AOT_DECLARE_TEST_COMMAND_PROVIDER)
#undef AOT_DECLARE_TEST_COMMAND_PROVIDER

const TestCommand* findTestCommand(StringView name);
void checkTestCommandNames();
void listTestCommands(PrintStream&);

std::optional<Vector<FunctionID>> functionsNamedBy(const Program&, std::span<const String> arguments, PrintStream&);
std::optional<Vector<CodeUnit>> unitsNamedBy(const Program&, std::span<const String> arguments, PrintStream&);
std::optional<Vector<CodeUnit>> unitsInOrderOfWords(const Program&, std::span<const String> arguments, PrintStream&);
std::optional<String> takeMechanismsWords(Vector<String>& words, std::optional<Mechanisms>& askedFor);

unsigned listBrokenLawsOfTypes(PrintStream&, Program&, unsigned mostTypes);

template<typename Range, typename T>
bool hasElements(const Range& range, std::initializer_list<T> expected)
{
    return std::ranges::equal(range, expected);
}

std::unique_ptr<Program> createEmptyProgram(BuildContext&);

void printDiagnosticsOf(const CompileOutput&, PrintStream&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
