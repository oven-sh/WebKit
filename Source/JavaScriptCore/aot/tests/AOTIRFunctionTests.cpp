/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTIRTestSupport.h"
#include "AOTProvisionalFunctions.h"
#include "AOTResultRules.h"
#include "AOTSchemaWalkers.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

DeclaredType createDeclaredType(TypeID type, OptionSet<PositionFlags> flags = { })
{
    DeclaredType declared;
    declared.type = type;
    declared.flags = flags;
    return declared;
}

ConstructorForm constructorFormOf(FunctionKind kind, OptionSet<FunctionFlags> flags = { })
{
    Function function;
    function.kind = kind;
    function.flags = flags;
    return function.constructorForm();
}

bool isAt(Wire::SourceLocation location, ModuleID module, uint32_t position)
{
    return location.module == module.index() && location.position == position;
}

Opcode firstOpcodeOf(const Function& function, BlockID block)
{
    return function.at(function.at(block).values[0]).opcode;
}

struct RemapFixture {
    RemapFixture()
    {
        StringInterningPhase stringPhase(test.program.strings, 1);
        TypeInterningPhase typePhase(test.program.types, 1);
        {
            StringArena stringArena(stringPhase, 0);
            TypeArena typeArena(typePhase, 0);
            string = stringArena.intern(StringView("a string made by a pass"_s));
            key = typeArena.textKey(string);
            layouts = typeArena.completeLayoutSet(std::array<LayoutID, 1> { LayoutID(0u) });
            callees = typeArena.intern(CalleeSet { { function }, { }, true });
            firstType = typeArena.unionOf(TypeTable::string, TypeTable::boolean);
            secondType = typeArena.unionOf(TypeTable::string, TypeTable::null);
            thirdType = typeArena.unionOf(TypeTable::string, TypeTable::undefined);
        }
        AOT_TEST_CHECK(StringTable::isProvisional(string) && TypeTable::isProvisional(key) && TypeTable::isProvisional(layouts) && TypeTable::isProvisional(callees));
        AOT_TEST_CHECK(TypeTable::isProvisional(firstType) && TypeTable::isProvisional(secondType) && TypeTable::isProvisional(thirdType));
        strings = stringPhase.finish();
        types = typePhase.finish(std::array<unsigned, 1> { 0 }, functions, strings);
        AOT_TEST_CHECK(!strings.isIdentity() && !types.isIdentity() && !functions.isIdentity());
    }

    void remap(Function& remapped) const { remapIDs(remapped, types, strings, functions); }

    TestProgram test;
    FunctionID function { ProvisionalFunctions::idOf(0) };
    FunctionID finalFunction { 5u };
    FunctionRemap functions { Vector<FunctionID> { finalFunction } };
    StringID string;
    PropertyKeyID key;
    LayoutSetID layouts;
    CalleeSetID callees;
    TypeID firstType;
    TypeID secondType;
    TypeID thirdType;
    StringRemap strings;
    TypeRemap types;
};

} // anonymous namespace

static void constructorFormOfEachKind()
{
    AOT_TEST_CHECK(constructorFormOf(FunctionKind::Plain) == ConstructorForm::Base);
    AOT_TEST_CHECK(constructorFormOf(FunctionKind::BaseConstructor) == ConstructorForm::Base);
    AOT_TEST_CHECK(constructorFormOf(FunctionKind::DerivedConstructor) == ConstructorForm::Derived);
    for (FunctionKind kind : { FunctionKind::Arrow, FunctionKind::Method, FunctionKind::Getter, FunctionKind::Setter, FunctionKind::Initializer, FunctionKind::Module, FunctionKind::CommonJsModule })
        AOT_TEST_CHECK(constructorFormOf(kind) == ConstructorForm::NotConstructor);
}

static void coroutinesAreNotConstructors()
{
    AOT_TEST_CHECK(constructorFormOf(FunctionKind::Plain, FunctionFlags::IsAsync) == ConstructorForm::NotConstructor);
    AOT_TEST_CHECK(constructorFormOf(FunctionKind::Plain, FunctionFlags::IsGenerator) == ConstructorForm::NotConstructor);
    AOT_TEST_CHECK(constructorFormOf(FunctionKind::Plain, { FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) == ConstructorForm::NotConstructor);
    AOT_TEST_CHECK(constructorFormOf(FunctionKind::Plain, { FunctionFlags::IsSloppy, FunctionFlags::HasThis, FunctionFlags::HasNewTarget }) == ConstructorForm::Base);

    Function ramp;
    ramp.role = FunctionRole::CoroutineRamp;
    ramp.creator = CreatorRecord::Coroutine { };
    ramp.flags = { FunctionFlags::IsAsync };
    AOT_TEST_CHECK(ramp.constructorForm() == ConstructorForm::NotConstructor && !ramp.isUnsplitCoroutine());
}

static void moduleBodiesByKind()
{
    for (FunctionKind kind : allMembersOf<FunctionKind>) {
        Function function;
        function.kind = kind;
        AOT_TEST_CHECK(function.isModuleBody() == (kind == FunctionKind::Module || kind == FunctionKind::CommonJsModule));
    }
}

static void entryParameterIndexesOfEveryFlagCombination()
{
    static constexpr std::pair<FunctionFlags, EntryParameter> implicitParameters[] = {
        { FunctionFlags::HasThis, EntryParameter::This },
        { FunctionFlags::TakesCallee, EntryParameter::Callee },
        { FunctionFlags::HasNewTarget, EntryParameter::NewTarget },
        { FunctionFlags::HasArgumentCount, EntryParameter::ArgumentCount },
        { FunctionFlags::HasHiddenRest, EntryParameter::HiddenRest },
    };
    for (unsigned combination = 0; combination < 1u << std::size(implicitParameters); ++combination) {
        Function function;
        function.captures.fill(createDeclaredType(TypeTable::anything), 2);
        function.parameters.fill(createDeclaredType(TypeTable::anything), 3);
        Vector<EntryParameter> expectedKinds { EntryParameter::Capture, EntryParameter::Capture };
        for (unsigned bit = 0; bit < std::size(implicitParameters); ++bit) {
            auto [flag, kind] = implicitParameters[bit];
            if (kind == EntryParameter::HiddenRest)
                expectedKinds.appendList({ EntryParameter::Parameter, EntryParameter::Parameter, EntryParameter::Parameter });
            if (!(combination & 1u << bit))
                continue;
            function.flags.add(flag);
            expectedKinds.append(kind);
        }
        auto expectedIndexOf = [&](EntryParameter kind) -> std::optional<unsigned> {
            size_t index = expectedKinds.find(kind);
            return index == notFound ? std::nullopt : std::optional<unsigned>(index);
        };

        AOT_TEST_CHECK(function.numberOfEntryParameters() == expectedKinds.size());
        for (unsigned index = 0; index < expectedKinds.size(); ++index)
            AOT_TEST_CHECK(function.entryParameterKind(index) == expectedKinds[index]);
        AOT_TEST_CHECK(function.thisParameterIndex() == expectedIndexOf(EntryParameter::This));
        AOT_TEST_CHECK(function.calleeParameterIndex() == expectedIndexOf(EntryParameter::Callee));
        AOT_TEST_CHECK(function.newTargetParameterIndex() == expectedIndexOf(EntryParameter::NewTarget));
        AOT_TEST_CHECK(function.argumentCountParameterIndex() == expectedIndexOf(EntryParameter::ArgumentCount));
        AOT_TEST_CHECK(function.firstParameterIndex() == expectedIndexOf(EntryParameter::Parameter));
        AOT_TEST_CHECK(function.hiddenRestParameterIndex() == expectedIndexOf(EntryParameter::HiddenRest));
    }
}

static void implicitEntryParametersHaveNoDeclaredPosition()
{
    Function function;
    function.flags = { FunctionFlags::HasThis, FunctionFlags::TakesCallee, FunctionFlags::HasNewTarget, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest };
    function.captures = { createDeclaredType(TypeTable::string) };
    function.parameters = { createDeclaredType(TypeTable::boolean), createDeclaredType(TypeTable::number) };
    AOT_TEST_CHECK(function.declaredEntryParameter(0) == &function.captures[0] && function.declaredEntryParameter(1) == &function.thisType);
    AOT_TEST_CHECK(function.declaredEntryParameter(5) == &function.parameters[0] && function.declaredEntryParameter(6) == &function.parameters[1]);
    for (unsigned index : { 2u, 3u, 4u, 7u })
        AOT_TEST_CHECK(!function.declaredEntryParameter(index) && !std::as_const(function).declaredEntryParameter(index));

    function.flags = { FunctionFlags::HasThis };
    AOT_TEST_CHECK(function.numberOfEntryParameters() == 4);
    for (unsigned index = 0; index < 4; ++index)
        AOT_TEST_CHECK(function.declaredEntryParameter(index));
}

static void declaredEntryTypeOfThisFollowsLevel()
{
    TestProgram test;
    TypeArena arena(test.program.types);
    Function function;
    function.flags = { FunctionFlags::HasThis };
    function.thisType = createDeclaredType(TypeTable::function, PositionFlags::IsExpected);
    function.thisType.site = TypeSiteID(4u);
    TypingContext typing { test.program, function, arena };
    AOT_TEST_CHECK(declaredEntryTypeOf(typing, function, 0) == TypeTable::anything);
    AOT_TEST_CHECK(declaredEntryTypeOf(typing.atLevel(TypeLevel::Expected), function, 0) == TypeTable::function);
    AOT_TEST_CHECK(declaredEntryTypeOf(typing.expecting(), function, 0) == TypeTable::function);

    function.thisType.flags = { };
    AOT_TEST_CHECK(declaredEntryTypeOf(typing, function, 0) == TypeTable::function);
    AOT_TEST_CHECK(declaredEntryTypeOf(typing.expecting(), function, 0) == TypeTable::function);
}

static void forEachTypeVisitsThisTypeOnce()
{
    for (OptionSet<PositionFlags> flags : { OptionSet<PositionFlags> { }, OptionSet<PositionFlags> { PositionFlags::IsExpected } }) {
        Function function;
        function.thisType = createDeclaredType(TypeID(101u), flags);
        Vector<TypeID> visited;
        forEachType(static_cast<const FunctionRecord&>(function), [&](TypeID id) { visited.append(id); });
        AOT_TEST_CHECK(hasElements(visited, { TypeID(101u) }));
    }
}

static void restParameterIsNotPositional()
{
    Function function;
    function.parameters.fill(createDeclaredType(TypeTable::anything), 3);
    AOT_TEST_CHECK(function.numberOfPositionalParameters() == 3);
    function.flags = { FunctionFlags::HasRestParameter };
    AOT_TEST_CHECK(function.numberOfPositionalParameters() == 2 && function.numberOfEntryParameters() == 3);
}

static void presenceFlagsCoverImplicitParameters()
{
    OptionSet<FunctionFlags> presenceFlags;
    for (EntryParameter kind : allMembersOf<EntryParameter>)
        presenceFlags.add(presentWithOf(kind));
    AOT_TEST_CHECK(presenceFlags == OptionSet<FunctionFlags>({ FunctionFlags::HasThis, FunctionFlags::TakesCallee, FunctionFlags::HasNewTarget, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }));
    AOT_TEST_CHECK(presentWithOf(EntryParameter::Capture).isEmpty() && presentWithOf(EntryParameter::Parameter).isEmpty());

    Function function;
    function.flags = presenceFlags | OptionSet<FunctionFlags>({ FunctionFlags::IsAsync, FunctionFlags::IsSloppy });
    AOT_TEST_CHECK(function.firstParameterIndex() == 4 && function.numberOfEntryParameters() == 5);
    function.flags = function.flags - presenceFlags;
    AOT_TEST_CHECK(function.flags == OptionSet<FunctionFlags>({ FunctionFlags::IsAsync, FunctionFlags::IsSloppy }));
    AOT_TEST_CHECK(!function.firstParameterIndex() && !function.numberOfEntryParameters());
}

static void bodyIsBlocksAndValues()
{
    Function function;
    AOT_TEST_CHECK(!function.hasBody());
    BlockID entry = addBlock(function);
    AOT_TEST_CHECK(function.hasBody() && function.at(entry).terminator.kind == TerminatorKind::Unreachable && function.at(entry).values.isEmpty());
    ValueID parameter = addParameter(function, entry, TypeTable::number);
    ValueID value = addValue(function, entry, Opcode::Negate, { parameter });
    AOT_TEST_CHECK(&function.at(entry) == &function.blocks[0] && &function.at(value) == &function.values[1] && &std::as_const(function).at(value) == &function.values[1]);
    AOT_TEST_CHECK(hasElements(function.at(entry).parameters, { parameter }) && hasElements(function.at(entry).values, { value }));
    AOT_TEST_CHECK(function.at(parameter).opcode == Opcode::Parameter && function.at(parameter).type == TypeTable::number && function.at(value).operand(0) == parameter);

    function.results = { createDeclaredType(TypeTable::string) };
    AOT_TEST_CHECK(&function.result() == &function.results[0]);
}

static void copyOfFunctionSharesNothing()
{
    Function function;
    BlockID entry = addBlock(function);
    BlockID exit = addBlock(function);
    ValueID element = addValue(function, entry, Opcode::Undefined);
    ValueID array = addValue(function, entry, Opcode::NewArray, { ValueID(), element });
    function.edit(array).setHoles(std::array<uint32_t, 2> { 1, 2 });
    jump(function, entry, exit, { element });
    function.edit(entry).handler = Target { exit, { array }, { } };
    function.parameters = { createDeclaredType(TypeTable::number) };

    Function copy = function;
    copy.edit(array).setHoles(std::array<uint32_t, 1> { 7 });
    copy.edit(array).appendVariadicOperand(array);
    copy.edit(entry).terminator.next().arguments.append(array);
    copy.edit(entry).handler->arguments.clear();
    copy.edit(entry).values.clear();
    copy.parameters[0].type = TypeTable::string;
    addBlock(copy);

    AOT_TEST_CHECK(function.blocks.size() == 2 && hasElements(function.at(entry).values, { element, array }));
    AOT_TEST_CHECK(hasElements(function.at(array).holes(), { 1u, 2u }) && hasElements(function.at(array).variadicOperands(), { element }));
    AOT_TEST_CHECK(hasElements(function.at(entry).terminator.next().arguments, { element }) && hasElements(function.at(entry).handler->arguments, { array }));
    AOT_TEST_CHECK(function.parameters[0].type == TypeTable::number);
}

static void declarationLocationNeedsModule()
{
    Function function;
    function.position = 12;
    AOT_TEST_CHECK(isAt(function.declarationLocation(), ModuleID(), 0));
    function.module = ModuleID(3u);
    AOT_TEST_CHECK(isAt(function.declarationLocation(), ModuleID(3u), 12));

    ValueID value = addValue(function, addBlock(function), Opcode::Undefined);
    AOT_TEST_CHECK(!function.at(value).hasPosition() && isAt(function.locationOf(value), ModuleID(), 0));
    function.edit(value).position = 40;
    AOT_TEST_CHECK(isAt(function.locationOf(value), ModuleID(3u), 40) && isAt(function.locationOf(Origin { 41, 0 }), ModuleID(3u), 41));
}

static void compactKeepsBlockReachedThroughHandlerOnly()
{
    Function function;
    BlockID entry = addBlock(function);
    BlockID exit = addBlock(function);
    BlockID handler = addBlock(function);
    BlockID unreachable = addBlock(function);
    addUnlistedValue(function, Opcode::Call);
    ValueID kept = addValue(function, entry, Opcode::Integer);
    ValueID subject = addValue(function, entry, Opcode::Null);
    jump(function, entry, exit);
    function.edit(entry).handler = Target { handler, { kept }, { } };
    function.edit(handler).role = BlockRole::IterationCleanup;
    function.edit(handler).subject = subject;
    addParameter(function, handler);
    addValue(function, handler, Opcode::CatchException);
    addValue(function, unreachable, Opcode::Undefined);
    jump(function, unreachable, handler, { kept });

    AOT_TEST_CHECK(compactFunction(function));
    AOT_TEST_CHECK(function.blocks.size() == 3 && function.values.size() == 4);
    AOT_TEST_CHECK(function.at(entry).terminator.next().block == exit && function.at(entry).handler->block == handler);
    AOT_TEST_CHECK(function.at(handler).role == BlockRole::IterationCleanup && firstOpcodeOf(function, handler) == Opcode::CatchException);
    AOT_TEST_CHECK(hasElements(function.at(entry).values, { ValueID(0u), ValueID(1u) }) && hasElements(function.at(entry).handler->arguments, { ValueID(0u) }));
    AOT_TEST_CHECK(function.at(ValueID(0u)).opcode == Opcode::Integer && function.at(handler).subject == ValueID(1u) && function.at(ValueID(1u)).opcode == Opcode::Null);
}

static void compactSortsBlocksWithoutEarlierPredecessor()
{
    Function function;
    BlockID entry = addBlock(function);
    BlockID last = addBlock(function);
    BlockID second = addBlock(function);
    BlockID handler = addBlock(function);
    addValue(function, last, Opcode::Integer);
    addValue(function, second, Opcode::Undefined);
    addValue(function, handler, Opcode::CatchValue);
    jump(function, entry, second);
    jump(function, second, last);
    function.edit(second).handler = Target { handler, { }, { } };
    function.edit(handler).role = BlockRole::CatchHandler;
    jump(function, handler, last);
    function.edit(last).terminator.become(TerminatorKind::Return);

    AOT_TEST_CHECK(compactFunction(function));
    AOT_TEST_CHECK(function.blocks.size() == 4 && function.values.size() == 3);
    AOT_TEST_CHECK(firstOpcodeOf(function, BlockID(1u)) == Opcode::Undefined && firstOpcodeOf(function, BlockID(2u)) == Opcode::CatchValue && firstOpcodeOf(function, BlockID(3u)) == Opcode::Integer);
    AOT_TEST_CHECK(function.blocks[0].terminator.next().block == BlockID(1u));
    AOT_TEST_CHECK(function.blocks[1].terminator.next().block == BlockID(3u) && function.blocks[1].handler->block == BlockID(2u));
    AOT_TEST_CHECK(function.blocks[2].role == BlockRole::CatchHandler && function.blocks[2].terminator.next().block == BlockID(3u) && !function.blocks[2].handler);
    AOT_TEST_CHECK(function.blocks[3].terminator.kind == TerminatorKind::Return);
    for (unsigned index = 1; index < 4; ++index)
        AOT_TEST_CHECK(hasElements(function.blocks[index].values, { ValueID(index - 1) }));
    AOT_TEST_CHECK(!compactFunction(function));
}

static void compactKeepsValidBlockOrder()
{
    Function function;
    BlockID entry = addBlock(function);
    BlockID exit = addBlock(function);
    BlockID header = addBlock(function);
    BlockID latch = addBlock(function);
    ValueID condition = addValue(function, entry, Opcode::Boolean);
    addUnlistedValue(function, Opcode::Call);
    addValue(function, exit, Opcode::Integer);
    addValue(function, header, Opcode::Undefined);
    addValue(function, latch, Opcode::Null);
    branch(function, entry, condition, header, exit);
    jump(function, header, latch);
    jump(function, latch, header);
    function.edit(exit).terminator.become(TerminatorKind::Return);

    AOT_TEST_CHECK(compactFunction(function));
    AOT_TEST_CHECK(function.blocks.size() == 4 && function.values.size() == 4);
    AOT_TEST_CHECK(function.at(entry).terminator.taken().block == header && function.at(entry).terminator.notTaken().block == exit);
    AOT_TEST_CHECK(function.at(header).terminator.next().block == latch && function.at(latch).terminator.next().block == header);
    AOT_TEST_CHECK(firstOpcodeOf(function, exit) == Opcode::Integer && firstOpcodeOf(function, header) == Opcode::Undefined && firstOpcodeOf(function, latch) == Opcode::Null);
    AOT_TEST_CHECK(function.at(entry).terminator.conditionOperand() == ValueID(0u) && function.at(ValueID(0u)).opcode == Opcode::Boolean);
}

static void remapIDsRewritesTypesOfValues()
{
    RemapFixture fixture;
    Function function;
    ValueID typed = addValue(function, addBlock(function), Opcode::Undefined);
    ValueID unlisted = addUnlistedValue(function, Opcode::Undefined);
    ValueID untouched = addUnlistedValue(function, Opcode::Undefined);
    function.edit(typed).type = fixture.firstType;
    function.edit(typed).setExpected(fixture.secondType);
    function.edit(unlisted).type = fixture.thirdType;
    function.edit(untouched).setExpected(TypeTable::string);

    fixture.remap(function);
    AOT_TEST_CHECK(function.at(typed).type == fixture.types(fixture.firstType) && !TypeTable::isProvisional(function.at(typed).type));
    AOT_TEST_CHECK(function.at(typed).expected() == fixture.types(fixture.secondType) && !TypeTable::isProvisional(function.at(typed).expected()));
    AOT_TEST_CHECK(function.at(typed).type != function.at(typed).expected());
    AOT_TEST_CHECK(function.at(unlisted).type == fixture.types(fixture.thirdType) && !function.at(unlisted).expected());
    AOT_TEST_CHECK(function.at(untouched).type == TypeTable::anything && function.at(untouched).expected() == TypeTable::string);
    AOT_TEST_CHECK(fixture.test.program.at(function.at(typed).type).primitives == (Primitive::String | Primitive::Boolean));
}

// Two arenas give one type two provisional ids, so an expected type may turn out to be the type itself. An expected type is set where it says more than the type does.
static void remapIDsClearsExpectedTypeThatBecomesTheType()
{
    TestProgram test;
    StringInterningPhase stringPhase(test.program.strings, 1);
    TypeInterningPhase typePhase(test.program.types, 2);
    TypeID inFirstArena;
    TypeID inSecondArena;
    TypeID narrower;
    {
        TypeArena first(typePhase, 0);
        TypeArena second(typePhase, 1);
        inFirstArena = first.unionOf(TypeTable::string, TypeTable::boolean);
        inSecondArena = second.unionOf(TypeTable::string, TypeTable::boolean);
        narrower = TypeTable::string;
    }
    AOT_TEST_CHECK(TypeTable::isProvisional(inFirstArena) && TypeTable::isProvisional(inSecondArena) && inFirstArena != inSecondArena);
    FunctionRemap functions;
    StringRemap strings = stringPhase.finish();
    TypeRemap types = typePhase.finish(std::array<unsigned, 2> { 0, 1 }, functions, strings);
    AOT_TEST_CHECK(types(inFirstArena) == types(inSecondArena));

    Function function;
    BlockID entry = addBlock(function);
    ValueID sameType = addValue(function, entry, Opcode::Undefined);
    ValueID narrowerType = addValue(function, entry, Opcode::Undefined);
    function.edit(sameType).type = inFirstArena;
    function.edit(sameType).setExpected(inSecondArena);
    function.edit(narrowerType).type = inFirstArena;
    function.edit(narrowerType).setExpected(narrower);

    remapIDs(function, types, strings, functions);
    AOT_TEST_CHECK(function.at(sameType).type == types(inFirstArena) && !function.at(sameType).expected());
    AOT_TEST_CHECK(function.at(narrowerType).type == types(inFirstArena) && function.at(narrowerType).expected() == narrower);
}

static void adoptIDsGivesBodyBuiltInAnotherArenaTheIDsOfThisOne()
{
    TestProgram test;
    TypeInterningPhase typePhase(test.program.types, 2);
    {
        TypeArena builder(typePhase, 0);
        TypeArena optimizer(typePhase, 1);
        Function function;
        BlockID entry = addBlock(function);
        ValueID typed = addValue(function, entry, Opcode::Undefined);
        ValueID typeTest = addValue(function, entry, Opcode::TypeTest);
        ValueID untouched = addValue(function, entry, Opcode::Undefined);
        function.edit(typed).type = builder.unionOf(TypeTable::string, TypeTable::boolean);
        function.edit(typed).setExpected(builder.unionOf(TypeTable::string, TypeTable::undefined));
        function.edit(typeTest).type = TypeTable::boolean;
        function.edit(typeTest).setTestedType(builder.unionOf(TypeTable::string, TypeTable::boolean));
        function.edit(untouched).type = TypeTable::anything;

        TypeID computed = optimizer.unionOf(TypeTable::string, TypeTable::boolean);
        AOT_TEST_CHECK(TypeTable::isProvisional(computed) && computed != function.at(typed).type);
        AOT_TEST_CHECK(numberOfForeignIDs(function, optimizer) == 3 && !numberOfForeignIDs(function, builder));

        adoptIDs(function, optimizer);
        AOT_TEST_CHECK(!numberOfForeignIDs(function, optimizer) && numberOfForeignIDs(function, builder) == 3);
        AOT_TEST_CHECK(function.at(typed).type == computed);
        AOT_TEST_CHECK(function.at(typed).expected() == optimizer.unionOf(TypeTable::string, TypeTable::undefined));
        AOT_TEST_CHECK(function.at(typeTest).testedType() == computed && function.at(typeTest).type == TypeTable::boolean);
        AOT_TEST_CHECK(function.at(untouched).type == TypeTable::anything && !function.at(untouched).expected());
    }
    typePhase.discard();
}

static void remapIDsRewritesPayloadsOfValues()
{
    RemapFixture fixture;
    Function function;
    BlockID entry = addBlock(function);
    ValueID copy = addValue(function, entry, Opcode::CopyDataProperties);
    ValueID string = addValue(function, entry, Opcode::String);
    ValueID shapeTest = addValue(function, entry, Opcode::ShapeTest);
    ValueID call = addValue(function, entry, Opcode::Call);
    ValueID closure = addValue(function, entry, Opcode::NewClosure);
    ValueID typeTest = addValue(function, entry, Opcode::TypeTest);
    function.edit(copy).setExcludedKeys(std::array<PropertyKeyID, 3> { PropertyKeyID(3u), fixture.key, PropertyKeyID(1u) });
    function.edit(string).setString(fixture.string);
    function.edit(shapeTest).setLayouts(fixture.layouts);
    function.edit(call).setCallees(fixture.callees);
    function.edit(closure).setFunction(fixture.function);
    function.edit(typeTest).setTestedType(fixture.firstType);

    fixture.remap(function);
    AOT_TEST_CHECK(hasElements(function.at(copy).excludedKeys(), { PropertyKeyID(3u), fixture.types(fixture.key), PropertyKeyID(1u) }));
    AOT_TEST_CHECK(!TypeTable::isProvisional(function.at(copy).excludedKeys()[1]));
    AOT_TEST_CHECK(function.at(string).string() == fixture.strings(fixture.string) && !StringTable::isProvisional(function.at(string).string()));
    AOT_TEST_CHECK(function.at(shapeTest).layouts() == fixture.types(fixture.layouts) && !TypeTable::isProvisional(function.at(shapeTest).layouts()));
    AOT_TEST_CHECK(function.at(call).callees() == fixture.types(fixture.callees) && !TypeTable::isProvisional(function.at(call).callees()));
    AOT_TEST_CHECK(function.at(closure).function() == fixture.finalFunction && function.at(typeTest).testedType() == fixture.types(fixture.firstType));

    const Program& program = fixture.test.program;
    AOT_TEST_CHECK(program.at(function.at(copy).excludedKeys()[1]).text == function.at(string).string());
    AOT_TEST_CHECK(hasElements(program.at(function.at(call).callees()).functions, { fixture.finalFunction }));
}

static void remapIDsRewritesPayloadsOfTerminators()
{
    RemapFixture fixture;
    Function function;
    BlockID returnCall = addBlock(function);
    BlockID switchString = addBlock(function);
    BlockID switchOnShape = addBlock(function);
    TailCast cast;
    cast.declared = createDeclaredType(fixture.firstType, PositionFlags::IsExpected);
    TailCast finalCast;
    finalCast.declared = createDeclaredType(TypeTable::number);
    function.edit(returnCall).terminator.become(TerminatorKind::ReturnCall);
    function.edit(returnCall).terminator.setCasts(std::array<TailCast, 2> { finalCast, cast });
    function.edit(switchString).terminator.become(TerminatorKind::SwitchString);
    function.edit(switchString).terminator.setStrings(std::array<StringID, 2> { StringID(0u), fixture.string });
    function.edit(switchOnShape).terminator.become(TerminatorKind::SwitchOnShape);
    function.edit(switchOnShape).terminator.setLayoutSets(std::array<LayoutSetID, 1> { fixture.layouts });

    fixture.remap(function);
    auto casts = function.at(returnCall).terminator.casts();
    AOT_TEST_CHECK(casts.size() == 2 && casts[0] == finalCast);
    AOT_TEST_CHECK(casts[1].declared == createDeclaredType(fixture.types(fixture.firstType), PositionFlags::IsExpected));
    AOT_TEST_CHECK(hasElements(function.at(switchString).terminator.strings(), { StringID(0u), fixture.strings(fixture.string) }));
    AOT_TEST_CHECK(hasElements(function.at(switchOnShape).terminator.layoutSets(), { fixture.types(fixture.layouts) }));
}

static void remapIDsRewritesHeader()
{
    RemapFixture fixture;
    Function function;
    function.name = fixture.string;
    function.captures = { createDeclaredType(fixture.firstType) };
    function.thisType = createDeclaredType(fixture.secondType, PositionFlags::IsExpected);
    function.parameters = { createDeclaredType(TypeTable::string), createDeclaredType(fixture.thirdType) };
    function.results = { createDeclaredType(fixture.thirdType) };
    function.role = FunctionRole::GenericVersion;
    function.creator = CreatorRecord::Version { fixture.function };
    function.derivation.source = fixture.function;
    function.inlineFrames.grow(2);
    function.inlineFrames[1].callee = fixture.function;

    fixture.remap(function);
    AOT_TEST_CHECK(function.name == fixture.strings(fixture.string) && function.captures[0].type == fixture.types(fixture.firstType));
    AOT_TEST_CHECK(function.thisType == createDeclaredType(fixture.types(fixture.secondType), PositionFlags::IsExpected));
    AOT_TEST_CHECK(function.parameters[0].type == TypeTable::string && function.parameters[1].type == fixture.types(fixture.thirdType));
    AOT_TEST_CHECK(function.results[0].type == fixture.types(fixture.thirdType));
    AOT_TEST_CHECK(function.creator.as<CreatorRecord::Version>().provenVersion == fixture.finalFunction && function.derivation.source == fixture.finalFunction);
    AOT_TEST_CHECK(!function.inlineFrames[0].callee && function.inlineFrames[1].callee == fixture.finalFunction);
}

static void remapIDsWithIdentityRemapsChangesNothing()
{
    RemapFixture fixture;
    Function function;
    ValueID value = addValue(function, addBlock(function), Opcode::TypeTest);
    function.edit(value).type = fixture.firstType;
    function.edit(value).setTestedType(fixture.secondType);
    function.captures = { createDeclaredType(fixture.thirdType) };
    function.derivation.source = fixture.function;

    TypeRemap types;
    StringRemap strings;
    FunctionRemap functions;
    AOT_TEST_CHECK(types.isIdentity() && strings.isIdentity() && functions.isIdentity());
    remapIDs(function, types, strings, functions);
    AOT_TEST_CHECK(function.at(value).type == fixture.firstType && function.at(value).testedType() == fixture.secondType);
    AOT_TEST_CHECK(function.captures[0].type == fixture.thirdType && function.derivation.source == fixture.function);
}

#define FOR_EACH_AOT_IR_FUNCTION_TEST(v) \
    v(constructorFormOfEachKind) v(coroutinesAreNotConstructors) v(moduleBodiesByKind) \
    v(entryParameterIndexesOfEveryFlagCombination) v(implicitEntryParametersHaveNoDeclaredPosition) \
    v(declaredEntryTypeOfThisFollowsLevel) v(forEachTypeVisitsThisTypeOnce) \
    v(restParameterIsNotPositional) v(presenceFlagsCoverImplicitParameters) \
    v(bodyIsBlocksAndValues) v(copyOfFunctionSharesNothing) v(declarationLocationNeedsModule) \
    v(compactKeepsBlockReachedThroughHandlerOnly) v(compactSortsBlocksWithoutEarlierPredecessor) v(compactKeepsValidBlockOrder) \
    v(remapIDsRewritesTypesOfValues) v(remapIDsClearsExpectedTypeThatBecomesTheType) v(remapIDsRewritesPayloadsOfValues) v(remapIDsRewritesPayloadsOfTerminators) v(remapIDsRewritesHeader) \
    v(remapIDsWithIdentityRemapsChangesNothing) v(adoptIDsGivesBodyBuiltInAnotherArenaTheIDsOfThisOne)

std::span<const NativeTest> irFunctionTests()
{
#define AOT_IR_FUNCTION_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_IR_FUNCTION_TEST(AOT_IR_FUNCTION_TEST_ENTRY) };
#undef AOT_IR_FUNCTION_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
