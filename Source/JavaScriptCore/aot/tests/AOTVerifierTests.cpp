/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsCheck.h"
#include "AOTIR.h"
#include "AOTIRTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTReaderFixture.h"
#include "AOTTypeRelations.h"
#include "AOTTypeTestPlan.h"
#include "AOTVerifier.h"
#include "AOTVerifierInternal.h"
#include <memory>
#include <span>
#include <wtf/DataLog.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

namespace {

using namespace Typed;
using Kind = InternalErrorKind;
using FunctionChange = void (*)(Typed::Function&);
using ProgramChange = void (*)(Program&);

constexpr FunctionID f7 = readerFixtureStreamFunction;
constexpr FunctionID f8 = readerFixtureHandlerFunction;
constexpr LayoutID L0 { 0u };
constexpr LayoutID L2 { 2u };
constexpr BlockID b0 { 0u };
constexpr BlockID b1 { 1u };
constexpr BlockID b3 { 3u };
constexpr ValueID v0 { 0u };
constexpr ValueID v1 { 1u };
constexpr ValueID v2 { 2u };
constexpr ValueID v4 { 4u };
constexpr ValueID v5 { 5u };
constexpr ValueID v7 { 7u };
constexpr ValueID v9 { 9u };
constexpr ValueID v11 { 11u };
constexpr ValueID v13 { 13u };
constexpr ValueID v14 { 14u };
constexpr ValueID v15 { 15u };
constexpr ValueID v16 { 16u };

constexpr uint32_t idPastEveryTable = 1000000;
constexpr VerifierInputs whileOptimizing { .isOptimizing = true };

struct VerifierTest {
    VerifierTest()
        : program(fixture.read())
    {
        typeProgramAsRead(*program, fixture.build);
    }

    BuildContext& build() { return fixture.build; }

    void expectErrors(std::span<const Kind> expected)
    {
        Diagnostics& diagnostics = build().diagnostics();
        diagnostics.finish(program.get());
        bool hasExpectedErrors = diagnostics.userDiagnostics().isEmpty() && std::ranges::equal(diagnostics.internalErrors(), expected, { }, &InternalError::kind);
        if (!hasExpectedErrors)
            printDiagnostics(diagnostics, WTF::dataFile());
        AOT_TEST_CHECK(hasExpectedErrors);
    }

    void expectNoError() { expectErrors({ }); }
    void expectOnly(Kind expected) { expectErrors(singleElementSpan(expected)); }

    ReaderFixture fixture;
    std::unique_ptr<Program> program;
};

void expectFailureIn(FunctionID id, Kind expected, FunctionChange change, IRForm form = IRForm::AsRead, const VerifierInputs& inputs = { })
{
    VerifierTest test;
    Typed::Function copy = test.program->at(id);
    change(copy);
    AOT_TEST_CHECK(!verifyFunction(*test.program, id, copy, form, test.build(), inputs));
    test.expectOnly(expected);
}

void expectFailure(Kind expected, FunctionChange change, IRForm form = IRForm::AsRead, const VerifierInputs& inputs = { })
{
    expectFailureIn(f7, expected, change, form, inputs);
}

void expectSuccessIn(FunctionID id, FunctionChange change, const VerifierInputs& inputs = { })
{
    VerifierTest test;
    Typed::Function copy = test.program->at(id);
    change(copy);
    bool isValid = verifyFunction(*test.program, id, copy, IRForm::AsRead, test.build(), inputs);
    test.expectNoError();
    AOT_TEST_CHECK(isValid);
}

void expectSuccessAfterTyping(FunctionChange change, IRForm form, const VerifierInputs& inputs = { })
{
    VerifierTest test;
    Typed::Function copy = test.program->at(f7);
    change(copy);
    // SelectTypedResults leaves no call that still names a result to validate.
    if (form != IRForm::AsRead && !inputs.isOptimizing)
        copy.at(v4).setExpectedResult({ });
    typeFunctionAsRead(*test.program, f7, copy, test.build());
    bool isValid = verifyFunction(*test.program, f7, copy, form, test.build(), inputs);
    test.expectNoError();
    AOT_TEST_CHECK(isValid);
}

void expectProgramFailure(Kind expected, ProgramChange change)
{
    VerifierTest test;
    change(*test.program);
    AOT_TEST_CHECK(!verifyProgram(*test.program, IRForm::AsRead, test.build()));
    test.expectOnly(expected);
}

void testAcceptsProgram()
{
    VerifierTest test;
    bool isValid = verifyProgram(*test.program, IRForm::AsRead, test.build());
    test.expectNoError();
    AOT_TEST_CHECK(isValid);
}

void testAcceptsCopies()
{
    VerifierTest test;
    unsigned numberOfBodies = 0;
    bool areAllValid = true;
    for (uint32_t index = 0; index < test.program->functions.size(); ++index) {
        Typed::Function copy = test.program->functions[index];
        numberOfBodies += copy.hasBody();
        areAllValid &= verifyFunction(*test.program, FunctionID(index), copy, IRForm::AsRead, test.build());
    }
    test.expectNoError();
    AOT_TEST_CHECK(areAllValid && numberOfBodies == 3);
}

void testValueListedTwice()
{
    expectFailure(Kind::ValueListedTwice, [](Typed::Function& copy) {
        copy.at(b1).values.append(v2);
    });
    expectFailure(Kind::ValueListedTwice, [](Typed::Function& copy) {
        copy.at(b0).values.append(v2);
    });
    expectFailure(Kind::ValueListedTwice, [](Typed::Function& copy) {
        copy.at(b1).parameters.append(v0);
    });
}

void testParameterNotInList()
{
    expectFailure(Kind::ParameterNotInList, [](Typed::Function& copy) {
        copy.at(b3).values.clear();
        copy.at(b3).parameters.append(v16);
    });
    expectFailure(Kind::ParameterNotInList, [](Typed::Function& copy) {
        copy.at(b3).parameters.clear();
        copy.at(b3).values.append(v15);
    });
}

void testListedIDOutOfRange()
{
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(b1).values.append(ValueID(1000u));
    });
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(b1).values.append(ValueID());
    });
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(b1).parameters.append(ValueID(1000u));
    });
}

void addDetachedValue(Typed::Function& function)
{
    addUnlistedValue(function, Opcode::Null);
}

void testValueNotListed()
{
    expectFailure(Kind::ValueNotListed, addDetachedValue);
    expectSuccessAfterTyping(addDetachedValue, IRForm::AsRead, whileOptimizing);
    expectSuccessAfterTyping(addDetachedValue, IRForm::Prepared);
}

void testOperandIDOutOfRange()
{
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(v5).setOperand(0, ValueID(1000u));
    });
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(v4).mutableVariadicOperands()[0] = ValueID(1000u);
    });
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(b0).terminator.setOperand(0, ValueID(1000u));
    });
}

void testTargetIDOutOfRange()
{
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(b0).terminator.taken().block = BlockID(9u);
    });
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(b0).terminator.taken().block = BlockID();
    });
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(b1).terminator.targets[0].arguments[0] = ValueID(1000u);
    });
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(b1).handler = Target { BlockID(9u), { }, { } };
    });
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(b1).subject = ValueID(1000u);
    });
}

void testTargetCount()
{
    expectFailure(Kind::TargetCountMismatch, [](Typed::Function& copy) {
        copy.at(b0).terminator.targets.append(Target { b3, { v2 }, { } });
    });
    expectFailure(Kind::TargetCountMismatch, [](Typed::Function& copy) {
        copy.at(b3).terminator.targets.append(Target { b3, { v2 }, { } });
    });
}

void testOperandMissing()
{
    expectFailure(Kind::OperandMissing, [](Typed::Function& copy) {
        copy.at(v4).mutableVariadicOperands()[0] = ValueID();
    });
    expectFailure(Kind::OperandMissing, [](Typed::Function& copy) {
        copy.at(v9).setOperand(0, ValueID());
    });
    expectFailure(Kind::OperandMissing, [](Typed::Function& copy) {
        copy.at(b0).terminator.setOperand(0, ValueID());
    });
}

void testUnexpectedOperand()
{
    expectFailure(Kind::UnexpectedOperand, [](Typed::Function& copy) {
        copy.at(v9).appendVariadicOperand(v0);
    });
}

void testFlagNotDeclared()
{
    expectFailure(Kind::FlagNotDeclared, [](Typed::Function& copy) {
        copy.at(v2).flags = 1;
    });
    expectFailure(Kind::FlagNotDeclared, [](Typed::Function& copy) {
        copy.at(v9).flags = 1 << 2;
    });
    expectFailure(Kind::FlagNotDeclared, [](Typed::Function& copy) {
        copy.at(b0).terminator.flags = 1;
    });
}

void testOpcodeOutOfRange()
{
    expectFailure(Kind::OpcodeNotInForm, [](Typed::Function& copy) {
        copy.at(v2).opcode = static_cast<Opcode>(numberOfEnumerators<Opcode>);
    });
    expectFailure(Kind::OpcodeNotInForm, [](Typed::Function& copy) {
        copy.at(b0).terminator.kind = static_cast<TerminatorKind>(numberOfEnumerators<TerminatorKind>);
    });
}

void testStorageOfAnotherOperation()
{
    expectFailure(Kind::OperandMissing, [](Typed::Function& copy) {
        copy.at(v2).opcode = Opcode::PropGet;
    });
    expectFailure(Kind::PayloadOutOfRange, [](Typed::Function& copy) {
        copy.at(v2).opcode = Opcode::NewClosure;
    });
    expectFailure(Kind::OperandMissing, [](Typed::Function& copy) {
        copy.at(b3).terminator.kind = TerminatorKind::Unreachable;
    });
}

void testPayloadOutOfRange()
{
    expectFailure(Kind::PayloadOutOfRange, [](Typed::Function& copy) {
        copy.at(v9).setKey(PropertyKeyID(idPastEveryTable));
    });
    expectFailure(Kind::PayloadOutOfRange, [](Typed::Function& copy) {
        copy.at(v9).setKey(PropertyKeyID());
    });
    expectFailure(Kind::PayloadOutOfRange, [](Typed::Function& copy) {
        copy.at(v14).setData(DataID(7u));
    });
    expectFailure(Kind::PayloadOutOfRange, [](Typed::Function& copy) {
        copy.at(v4).setExpectedResult(ExpectedResultID(7u));
    });
    expectFailure(Kind::PayloadOutOfRange, [](Typed::Function& copy) {
        copy.at(v13).setTestedType(TypeID(idPastEveryTable));
    });
    expectFailure(Kind::PayloadOutOfRange, [](Typed::Function& copy) {
        copy.at(v11).setBuiltin(BuiltinID(60000));
    });
    expectFailure(Kind::PayloadOutOfRange, [](Typed::Function& copy) {
        copy.at(v7).setMode(static_cast<ECMAMode>(9));
    });
}

void testTypeIDOutOfRange()
{
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(v2).type = { };
    });
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(v2).type = TypeID(idPastEveryTable);
    });
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(v9).setWritten(TypeID(idPastEveryTable));
    });
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(v9).setPrediction(TypeID(idPastEveryTable));
    });
    expectFailure(Kind::IdOutOfRange, [](Typed::Function& copy) {
        copy.at(v15).type = { };
    });
}

void testUntypedFunction()
{
    ReaderFixture fixture;
    auto program = fixture.read();
    AOT_TEST_CHECK(!verifyFunction(*program, f7, program->at(f7), IRForm::AsRead, fixture.build));
    Diagnostics& diagnostics = fixture.build.diagnostics();
    diagnostics.finish(program.get());
    AOT_TEST_CHECK(diagnostics.internalErrors().size() == 1 && diagnostics.internalErrors()[0].kind == Kind::IdOutOfRange);
}

void testWrongRepresentation()
{
    expectFailure(Kind::WrongRepresentation, [](Typed::Function& copy) {
        copy.at(v2).representation = Representation::Double;
    });
    expectFailure(Kind::WrongRepresentation, [](Typed::Function& copy) {
        copy.at(v0).representation = Representation::JSValue;
    });
}

void addRequest(Typed::Function& function)
{
    ValueID request = addValue(function, b3, Opcode::ClaimDeclared, { v0 });
    DeclaredType declared;
    declared.type = TypeTable::anything;
    function.at(request).setDeclared(declared);
}

void addPreparedOperation(Typed::Function& function)
{
    addValue(function, b3, Opcode::GetShapeEpoch);
}

void addOperationsOfBothForms(Typed::Function& function)
{
    addRequest(function);
    addPreparedOperation(function);
}

void addPlacedOperation(Typed::Function& function)
{
    addValue(function, b3, Opcode::Box, { v0 });
}

void testOptimizingAcceptsBothForms()
{
    expectFailure(Kind::OpcodeNotInForm, addOperationsOfBothForms, IRForm::AsRead);
    expectFailure(Kind::RequiredPassLeftOperation, addOperationsOfBothForms, IRForm::Prepared);
    expectSuccessAfterTyping(addOperationsOfBothForms, IRForm::AsRead, whileOptimizing);
    expectSuccessAfterTyping(addOperationsOfBothForms, IRForm::Prepared, whileOptimizing);
    expectSuccessAfterTyping(addPreparedOperation, IRForm::Prepared);
    expectFailure(Kind::RequiredPassLeftOperation, [](Typed::Function&) { }, IRForm::Prepared);

    expectFailure(Kind::OpcodeNotInForm, addPlacedOperation, IRForm::AsRead, whileOptimizing);
    expectFailure(Kind::OpcodeNotInForm, addPlacedOperation, IRForm::Prepared, whileOptimizing);
}

void testRequestAfterTyping()
{
    VerifierTest test;
    Typed::Function copy = test.program->at(f7);
    addRequest(copy);
    typeFunctionAsRead(*test.program, f7, copy, test.build());
    AOT_TEST_CHECK(!verifyFunction(*test.program, f7, copy, IRForm::AsRead, test.build()));
    test.expectOnly(Kind::RequestAfterTyping);
}

void widenUnusedValue(Typed::Function& function)
{
    function.at(v13).type = TypeTable::anything;
}

void testOptimizingAcceptsWiderType()
{
    expectFailure(Kind::TypeOutOfDate, widenUnusedValue);
    expectFailure(Kind::TypeOutOfDate, [](Typed::Function& copy) {
        copy.at(v13).type = TypeTable::undefined;
    }, IRForm::AsRead, whileOptimizing);

    expectSuccessIn(f7, widenUnusedValue, whileOptimizing);
}

void testEntryParameterTypes()
{
    expectFailure(Kind::EntryParametersMismatch, [](Typed::Function& copy) {
        copy.at(v1).type = TypeTable::anything;
    });
    expectFailure(Kind::EntryParametersMismatch, [](Typed::Function& copy) {
        copy.at(v1).type = TypeTable::never;
    });
    expectFailure(Kind::EntryParametersMismatch, [](Typed::Function& copy) {
        copy.at(v0).type = TypeTable::number;
    });
    expectFailure(Kind::EntryParametersMismatch, [](Typed::Function& copy) {
        copy.at(v1).type = TypeTable::anything;
    }, IRForm::AsRead, whileOptimizing);
}

void testEdgeArgumentTypes()
{
    expectFailure(Kind::EdgeArgumentMismatch, [](Typed::Function& copy) {
        copy.at(v15).type = TypeTable::undefined;
    });
    expectFailure(Kind::EdgeArgumentMismatch, [](Typed::Function& copy) {
        copy.at(v15).type = TypeTable::never;
    });
    expectSuccessIn(f7, [](Typed::Function& copy) {
        copy.at(v15).type = TypeTable::anything;
    });
}

void testEdgeResultTypes()
{
    VerifierTest test;
    AOT_TEST_CHECK(test.program->at(f8).at(v2).type == TypeTable::number);

    expectFailureIn(f8, Kind::EdgeArgumentMismatch, [](Typed::Function& copy) {
        copy.at(v2).type = TypeTable::undefined;
    });
    expectFailureIn(f8, Kind::EdgeArgumentMismatch, [](Typed::Function& copy) {
        copy.at(v2).type = TypeTable::never;
    });
    expectSuccessIn(f8, [](Typed::Function& copy) {
        copy.at(v2).type = TypeTable::anything;
    });
}

void testHandlerArgumentTypes()
{
    VerifierTest test;
    AOT_TEST_CHECK(test.program->at(f8).at(v4).type == TypeTable::anything);

    expectFailureIn(f8, Kind::EdgeArgumentMismatch, [](Typed::Function& copy) {
        copy.at(v4).type = TypeTable::number;
    });
    expectFailureIn(f8, Kind::EdgeArgumentMismatch, [](Typed::Function& copy) {
        copy.at(v4).type = TypeTable::never;
    });
}

constexpr ValueID placedRegExp { 0u };
constexpr ValueID placedObject { 1u };
constexpr ValueID placedGuard { 2u };
constexpr ValueID placedRead { 3u };
constexpr uint32_t placedID = 4;

Typed::Function createPlacedFunction(Program& program)
{
    Typed::Function function = createFunction({ Opcode::NewRegExp, Opcode::NewObject, Opcode::IsClassEvaluatedOnce, Opcode::GetSlot });
    function.module = ModuleID(0u);

    function.at(placedRegExp).setPattern(program.strings.emptyString());
    function.at(placedRegExp).setRegExpRecord(RegExpRecordID(placedID));
    function.at(placedObject).setLayout(L0);
    function.at(placedObject).setAllocation(AllocationID(placedID));
    function.at(placedGuard).setLayout(L2);
    function.at(placedGuard).setGuardBit(GuardBitID(placedID));

    Value& read = function.at(placedRead);
    read.setOperand(0, placedObject);
    read.setKey(program.at(L0).fields[0].key);
    read.setReceivers(program.types.intern(LayoutSet { { L0 }, true }));
    read.setFieldAccess(FieldAccessID(placedID));
    return function;
}

Vector<InternalError> verifyPlacedIDs(IRForm form, const PlacedTableSizes& sizes)
{
    VerifierTest test;
    Typed::Function function = createPlacedFunction(*test.program);
    FunctionVerifier verifier(*test.program, f7, function, form, test.build(), VerifierInputs { .placed = &sizes });
    bool isValid = verifier.runStructure();
    Vector<InternalError> errors = verifier.takeErrors();
    AOT_TEST_CHECK(isValid == errors.isEmpty());
    return errors;
}

void expectPlacedIDRefused(IRForm form, const PlacedTableSizes& sizes, ValueID holder)
{
    Vector<InternalError> errors = verifyPlacedIDs(form, sizes);
    AOT_TEST_CHECK(errors.size() == 1);
    AOT_TEST_CHECK(errors[0].kind == Kind::PayloadOutOfRange && errors[0].value == holder);
}

void testPlacedTableSizes()
{
    constexpr unsigned enough = placedID + 1;
    AOT_TEST_CHECK(verifyPlacedIDs(IRForm::Placed, { enough, enough, enough, enough }).isEmpty());
    expectPlacedIDRefused(IRForm::Placed, { placedID, enough, enough, enough }, placedRead);
    expectPlacedIDRefused(IRForm::Placed, { enough, placedID, enough, enough }, placedObject);
    expectPlacedIDRefused(IRForm::Placed, { enough, enough, placedID, enough }, placedRegExp);
    expectPlacedIDRefused(IRForm::Placed, { enough, enough, enough, placedID }, placedGuard);
    expectPlacedIDRefused(IRForm::Placed, { enough, enough, 0, enough }, placedRegExp);
    expectPlacedIDRefused(IRForm::Selected, { enough, enough, enough, enough }, placedRegExp);
}

void testInvalidModuleFailure()
{
    expectProgramFailure(Kind::InvalidModuleFailure, [](Program& program) {
        program.moduleFailures[0].importer = { };
    });
    expectProgramFailure(Kind::InvalidModuleFailure, [](Program& program) {
        program.moduleFailures[0].specifier = { };
    });
}

void testRequiredIDsOfRecords()
{
    expectProgramFailure(Kind::InvalidModule, [](Program& program) {
        program.modules[0].path = { };
    });
    expectProgramFailure(Kind::InvalidModule, [](Program& program) {
        program.modules[0].requests[0].target = { };
    });
    expectProgramFailure(Kind::InvalidEntry, [](Program& program) {
        program.entries[0].module = { };
    });
    expectProgramFailure(Kind::InvalidEntry, [](Program& program) {
        program.entries[0].hostReads[0].exportName = { };
    });
    expectProgramFailure(Kind::InvalidVariable, [](Program& program) {
        program.variables[0].module = { };
    });
}

void testRecordIDOutOfRange()
{
    expectProgramFailure(Kind::IdOutOfRange, [](Program& program) {
        program.variables[0].module = ModuleID(9u);
    });
    expectProgramFailure(Kind::IdOutOfRange, [](Program& program) {
        program.at(L2).constructor = FunctionID(99u);
    });
    expectProgramFailure(Kind::IdOutOfRange, [](Program& program) {
        program.at(f7).name = StringID(idPastEveryTable);
    });
    expectProgramFailure(Kind::IdOutOfRange, [](Program& program) {
        program.entries[0].module = ModuleID(9u);
    });
}

void testEffectsExceedReplacedGroup()
{
    VerifierTest test;
    Program& program = *test.program;
    Typed::Function copy = program.at(f7);
    ValueID comparisonOfIntegers = addValue(copy, b0, Opcode::StrictEq, { v2, v2 });
    ValueID comparisonOfAnything = addValue(copy, b0, Opcode::StrictEq, { v0, v0 });
    ValueID replacement = addValue(copy, b0, Opcode::StringEqualsLiteral, { v0 });
    copy.at(replacement).setLiteral(program.strings.emptyString());
    typeFunctionAsRead(program, f7, copy, test.build());

    EffectSummaries summaries;
    EffectContext context { program, summaries };
    ReplacementSite site { f7, b0, replacement };
    Diagnostics& diagnostics = test.build().diagnostics();

    ReplacedEffects groupThatReadsStrings;
    groupThatReadsStrings.add(context, copy, copy.at(comparisonOfAnything));
    checkReplacement(context, copy, site, copy.at(replacement), groupThatReadsStrings, diagnostics);
    AOT_TEST_CHECK(!diagnostics.hasErrors());

    ReplacedEffects groupWithoutEffects;
    groupWithoutEffects.add(context, copy, copy.at(comparisonOfIntegers));
    checkReplacement(context, copy, site, copy.at(replacement), groupWithoutEffects, diagnostics);
    test.expectOnly(Kind::EffectsExceedReplacedGroup);
}

void testEffectsExceedReplacedGroupInProvisionalFunction()
{
    VerifierTest test;
    Program& program = *test.program;
    Typed::Function copy = program.at(f7);
    copy.derivation.source = f7;
    ValueID comparisonOfIntegers = addValue(copy, b0, Opcode::StrictEq, { v2, v2 });
    ValueID replacement = addValue(copy, b0, Opcode::StringEqualsLiteral, { v0 });
    copy.at(replacement).setLiteral(program.strings.emptyString());
    typeFunctionAsRead(program, f7, copy, test.build());

    EffectSummaries summaries;
    EffectContext context { program, summaries };
    ReplacedEffects groupWithoutEffects;
    groupWithoutEffects.add(context, copy, copy.at(comparisonOfIntegers));
    checkReplacement(context, copy, { ProvisionalFunctions::idOf(0), b0, replacement }, copy.at(replacement), groupWithoutEffects, test.build().diagnostics());
    test.expectOnly(Kind::EffectsExceedReplacedGroup);

    const InternalError& error = test.build().diagnostics().internalErrors()[0];
    AOT_TEST_CHECK(error.function == f7 && !error.block && !error.value);
}

// The record of a type is planned from the type without its proofs, and the tables hold the cells of the realm alone.
void testRecordOfEveryBuiltinNamesOnlyCellsOfRealm()
{
    VerifierTest test;
    const Program& program = *test.program;
    const BuiltinTable& builtins = program.builtins();
    unsigned numberOfBuiltinsWithoutCell = 0;
    for (unsigned index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID id(index);
        bool hasCell = BuiltinCellKey::object(id).hasCell(builtins);
        numberOfBuiltinsWithoutCell += !hasCell;

        Type type = Type::forBuiltin(id);
        AOT_TEST_CHECK(isTestableBuiltin(builtins, id) == hasCell);
        AOT_TEST_CHECK(type.isSingleValue() == hasCell);
        AOT_TEST_CHECK((type.withUntestableBuiltinsWidened() == type) == hasCell);
        AOT_TEST_CHECK((type.withoutProofs() == type) == hasCell);

        TypeTestPlan plan = computeTypeTestPlanFromAnything(program, type.withoutProofs());
        AOT_TEST_CHECK(plan.someCellPasses());
        for (const CellTest& cellTest : plan.cellTests)
            AOT_TEST_CHECK(cellTest.kind != CellTestKind::IsBuiltinCell || BuiltinCellKey::object(cellTest.builtin).hasCell(builtins));
    }
    AOT_TEST_CHECK(numberOfBuiltinsWithoutCell);
}

} // anonymous namespace

std::span<const NativeTest> verifierTests()
{
    static constexpr NativeTest tests[] = {
        { "verifier/accepts-program"_s, testAcceptsProgram },
        { "verifier/accepts-copies"_s, testAcceptsCopies },
        { "verifier/value-listed-twice"_s, testValueListedTwice },
        { "verifier/parameter-not-in-list"_s, testParameterNotInList },
        { "verifier/listed-id-out-of-range"_s, testListedIDOutOfRange },
        { "verifier/value-not-listed"_s, testValueNotListed },
        { "verifier/operand-id-out-of-range"_s, testOperandIDOutOfRange },
        { "verifier/target-id-out-of-range"_s, testTargetIDOutOfRange },
        { "verifier/target-count"_s, testTargetCount },
        { "verifier/operand-missing"_s, testOperandMissing },
        { "verifier/unexpected-operand"_s, testUnexpectedOperand },
        { "verifier/flag-not-declared"_s, testFlagNotDeclared },
        { "verifier/opcode-out-of-range"_s, testOpcodeOutOfRange },
        { "verifier/storage-of-another-operation"_s, testStorageOfAnotherOperation },
        { "verifier/payload-out-of-range"_s, testPayloadOutOfRange },
        { "verifier/type-id-out-of-range"_s, testTypeIDOutOfRange },
        { "verifier/untyped-function"_s, testUntypedFunction },
        { "verifier/wrong-representation"_s, testWrongRepresentation },
        { "verifier/optimizing-accepts-both-forms"_s, testOptimizingAcceptsBothForms },
        { "verifier/request-after-typing"_s, testRequestAfterTyping },
        { "verifier/optimizing-accepts-wider-type"_s, testOptimizingAcceptsWiderType },
        { "verifier/entry-parameter-types"_s, testEntryParameterTypes },
        { "verifier/edge-argument-types"_s, testEdgeArgumentTypes },
        { "verifier/edge-result-types"_s, testEdgeResultTypes },
        { "verifier/handler-argument-types"_s, testHandlerArgumentTypes },
        { "verifier/placed-table-sizes"_s, testPlacedTableSizes },
        { "verifier/invalid-module-failure"_s, testInvalidModuleFailure },
        { "verifier/required-ids-of-records"_s, testRequiredIDsOfRecords },
        { "verifier/record-id-out-of-range"_s, testRecordIDOutOfRange },
        { "verifier/effects-exceed-replaced-group"_s, testEffectsExceedReplacedGroup },
        { "verifier/effects-exceed-replaced-group-in-provisional-function"_s, testEffectsExceedReplacedGroupInProvisionalFunction },
        { "verifier/record-of-every-builtin-names-only-cells-of-realm"_s, testRecordOfEveryBuiltinNamesOnlyCellsOfRealm },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
