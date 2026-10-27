/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRTestSupport.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include <algorithm>
#include <array>
#include <initializer_list>

namespace JSC { namespace AOT { namespace Typed {

namespace {

DeclaredType createDeclaredType(TypeID type)
{
    return DeclaredType(Proven::byPosition(type));
}

CreatorRecord::Coroutine createCoroutineRecord(CoroutineKind kind, TypeID promiseType = { })
{
    CreatorRecord::Coroutine record;
    record.kind = kind;
    record.promiseType = promiseType;
    return record;
}

Function createFunctionWithRole(FunctionRole role, CreatorRecord&& creator = { })
{
    Function function;
    function.role = role;
    function.creator = WTF::move(creator);
    function.thisType = createDeclaredType(TypeTable::anything);
    function.results = { createDeclaredType(TypeTable::anything) };
    function.capabilities = initialCapabilitiesOf(function);
    return function;
}

RuntimeFunctionID findRuntimeFunctionWithRole(RuntimeFunctionRole role)
{
    std::span<const RuntimeFunctionData> functions = BuiltinTable::singleton().runtimeFunctions();
    auto function = std::ranges::find(functions, role, &RuntimeFunctionData::role);
    AOT_TEST_CHECK(function != functions.end());
    return function->id;
}

Vector<InternalErrorKind> verifyAndCollectErrors(const Program& program, IRForm form)
{
    BuildContext build(makeDefaultCompileOptions());
    bool isValid = verifyProgram(program, form, build);
    build.diagnostics().finish(&program);
    AOT_TEST_CHECK(build.diagnostics().userDiagnostics().isEmpty());
    Vector<InternalErrorKind> kinds = WTF::map(build.diagnostics().internalErrors(), [](const InternalError& error) {
        return error.kind;
    });
    AOT_TEST_CHECK(isValid == kinds.isEmpty());
    return kinds;
}

} // anonymous namespace

static void verifierRejectsRecordOfAnotherRole()
{
    TestProgram test;
    FunctionID id = test.addFunction(createFunctionWithRole(FunctionRole::Source));
    AOT_TEST_CHECK(verifyAndCollectErrors(test.program, IRForm::Prepared).isEmpty());
    test.program.at(id).creator = CreatorRecord::ChainStep { };
    AOT_TEST_CHECK(hasElements(verifyAndCollectErrors(test.program, IRForm::Prepared), { InternalErrorKind::InvalidCreatorRecord }));
    test.program.at(id) = createFunctionWithRole(FunctionRole::ChainStep, CreatorRecord::ChainStep { });
    AOT_TEST_CHECK(verifyAndCollectErrors(test.program, IRForm::Prepared).isEmpty());
    test.program.at(id).capabilities.add(FunctionCapabilities::MayChangeSignature);
    AOT_TEST_CHECK(hasElements(verifyAndCollectErrors(test.program, IRForm::Prepared), { InternalErrorKind::InvalidCreatorRecord }));
}

static void enclosingRuntimeFunctionMustBeRegistered()
{
    TestProgram test;
    RuntimeFunctionID runtimeFunction = findRuntimeFunctionWithRole(RuntimeFunctionRole::HostBuiltin);
    FunctionID registered = test.addFunction(createFunctionWithRole(FunctionRole::Library));
    FunctionID enclosed = test.addFunction(createFunctionWithRole(FunctionRole::Library));
    test.program.at(registered).enclosingRuntimeFunction = runtimeFunction;
    test.program.at(enclosed).enclosingRuntimeFunction = runtimeFunction;
    AOT_TEST_CHECK(!findRuntimeFunction(test.program, runtimeFunction) && !findRuntimeFunction(test.program, { }));
    AOT_TEST_CHECK(hasElements(verifyAndCollectErrors(test.program, IRForm::Prepared), { InternalErrorKind::InvalidCreatorRecord, InternalErrorKind::InvalidCreatorRecord }));

    test.program.runtimeFunctions.append(RuntimeFunctionEntry { runtimeFunction, registered, FunctionVisibility::Public });
    const RuntimeFunctionEntry* entry = findRuntimeFunction(test.program, runtimeFunction);
    AOT_TEST_CHECK(entry && entry->function == registered);
    AOT_TEST_CHECK(verifyAndCollectErrors(test.program, IRForm::Prepared).isEmpty());
}

static void editorRecordsChangedFunctions()
{
    TestProgram test;
    test.program.functions.grow(3);
    ProgramEditor editor(test.program);
    AOT_TEST_CHECK(!editor.program().at(FunctionID(1u)).hasBody());
    AOT_TEST_CHECK(editor.changedFunctions().isEmpty());
    editor.at(FunctionID(2u)).length = 1;
    AOT_TEST_CHECK(!editor.at(FunctionID(0u)).hasBody());
    AOT_TEST_CHECK(hasElements(editor.changedFunctions(), { FunctionID(0u), FunctionID(2u) }));
}

static void editorTellsFunctionsGivenOutOnlyForStorage()
{
    TestProgram test;
    test.program.functions.grow(4);
    ProgramEditor editor(test.program);
    editor.atForStorage(FunctionID(0u));
    editor.atForStorage(FunctionID(1u));
    editor.atForLowering(FunctionID(1u));
    editor.atForStorage(FunctionID(2u));
    editor.at(FunctionID(2u));
    editor.atForLowering(FunctionID(3u));
    AOT_TEST_CHECK(hasElements(editor.changedFunctions(), { FunctionID(0u), FunctionID(1u), FunctionID(2u), FunctionID(3u) }));
    AOT_TEST_CHECK(hasElements(editor.functionsWithChangedOperations(), { FunctionID(0u), FunctionID(1u), FunctionID(2u) }));
    AOT_TEST_CHECK(hasElements(editor.functionsThatWereOnlyLowered(), { FunctionID(3u) }));
    AOT_TEST_CHECK(editor.wasGivenOutOnlyForStorage(FunctionID(0u)));
    AOT_TEST_CHECK(!editor.wasGivenOutOnlyForStorage(FunctionID(1u)) && !editor.wasGivenOutOnlyForStorage(FunctionID(2u)) && !editor.wasGivenOutOnlyForStorage(FunctionID(3u)));
}

static void editorAddFunctionLikeFollowsDerivationSource()
{
    TestProgram test;
    FunctionID original = test.addFunction({ });
    test.program.at(original).module = ModuleID(3u);
    ProgramEditor editor(test.program);
    FunctionID copy = editor.addFunctionLike(original, { });
    FunctionID copyOfCopy = editor.addFunctionLike(copy, { });
    AOT_TEST_CHECK(!editor.program().at(original).derivation.source);
    AOT_TEST_CHECK(editor.program().at(copy).derivation.source == original && editor.program().at(copy).module == ModuleID(3u));
    AOT_TEST_CHECK(editor.program().at(copyOfCopy).derivation.source == original && editor.program().at(copyOfCopy).module == ModuleID(3u));
    AOT_TEST_CHECK(hasElements(editor.changedFunctions(), { copy, copyOfCopy }));
}

static void editorRemoveBodyKeepsSignature()
{
    TestProgram test;
    Function function = createFunction({ Opcode::Undefined });
    function.inlineFrames.grow(1);
    function.parameters = { createDeclaredType(TypeTable::number) };
    function.results = { createDeclaredType(TypeTable::string) };
    function.role = FunctionRole::GenericVersion;
    function.creator = CreatorRecord::Version { FunctionID(5u) };
    FunctionID id = test.addFunction(WTF::move(function));

    ProgramEditor editor(test.program);
    editor.removeBody(id);
    const Function& removed = editor.program().at(id);
    AOT_TEST_CHECK(!removed.hasBody() && removed.values.isEmpty() && removed.inlineFrames.isEmpty());
    AOT_TEST_CHECK(removed.parameters[0].written() == TypeTable::number && removed.result().written() == TypeTable::string);
    AOT_TEST_CHECK(removed.role == FunctionRole::GenericVersion && removed.creator.as<CreatorRecord::Version>().provenVersion == FunctionID(5u));
    AOT_TEST_CHECK(hasElements(editor.changedFunctions(), { id }));
}

static void editorReservesConsecutiveFunctions()
{
    TestProgram test;
    test.program.functions.grow(2);
    ProgramEditor editor(test.program);
    AOT_TEST_CHECK(editor.reserveFunctions(3) == FunctionID(2u) && editor.program().functions.size() == 5);
    AOT_TEST_CHECK(hasElements(editor.changedFunctions(), { FunctionID(2u), FunctionID(3u), FunctionID(4u) }));
    for (FunctionID id : editor.changedFunctions())
        AOT_TEST_CHECK(!editor.program().at(id).hasBody());
}

static void editorAddedLayoutIsItsOnlyDeclaredMember()
{
    TestProgram test;
    test.program.layouts.grow(1);
    ProgramEditor editor(test.program);
    Layout layout;
    layout.fields.grow(2);
    LayoutID id = editor.addLayout(WTF::move(layout));
    AOT_TEST_CHECK(id == LayoutID(1u) && editor.program().at(id).ownFieldCount == 2);
    const LayoutSet& members = editor.program().at(editor.program().declaredMembersOf(id));
    AOT_TEST_CHECK(members.listsEveryLayout() && hasElements(members.written(), { id }));
    AOT_TEST_CHECK(editor.changedFunctions().isEmpty());
}

static void expectedTypeOfValueWinsOverItsType()
{
    TestProgram test;
    Function function;
    ValueID value = addValue(function, addBlock(function), Opcode::Undefined);
    function.edit(value).type = TypeTable::number;
    AOT_TEST_CHECK(&test.program.typeOf(function, value) == &test.program.at(TypeTable::number) && &test.program.expectedOrTypeOf(function, value) == &test.program.at(TypeTable::number));
    function.edit(value).setExpected(ExpectedType(TypeTable::int32));
    AOT_TEST_CHECK(&test.program.typeOf(function, value) == &test.program.at(TypeTable::number) && &test.program.expectedOrTypeOf(function, value) == &test.program.at(TypeTable::int32));
}

static void propertyKeysAreUnique()
{
    TestProgram test;
    TypeTable& types = test.program.types;
    StringID description = test.program.strings.intern(StringView("a description"_s));
    TypeArena arena(types);
    PropertyKeyID registered = arena.registeredSymbolKey(description);
    PropertyKeyID text = arena.textKey(description);
    PropertyKeyID iterator = arena.wellKnownSymbolKey(WellKnownSymbol::Iterator);
    AOT_TEST_CHECK(types.numberOfPropertyKeys() == 3 && registered != text && text != iterator);
    AOT_TEST_CHECK(arena.registeredSymbolKey(description) == registered && arena.textKey(description) == text && arena.wellKnownSymbolKey(WellKnownSymbol::Iterator) == iterator);
    AOT_TEST_CHECK(arena.wellKnownSymbolKey(WellKnownSymbol::AsyncIterator) != iterator && types.numberOfPropertyKeys() == 4);

    PropertyKey created;
    created.kind = PropertyKeyKind::CreatedSymbol;
    created.text = description;
    created.module = ModuleID(0u);
    created.callNumber = 120;
    PropertyKey createdElsewhere = created;
    createdElsewhere.callNumber = 130;
    AOT_TEST_CHECK(!types.find(created));
    PropertyKeyID createdKey = arena.intern(created);
    AOT_TEST_CHECK(arena.intern(created) == createdKey && types.find(created) == createdKey && arena.intern(createdElsewhere) != createdKey);

    PropertyKey privateName;
    privateName.kind = PropertyKeyKind::PrivateName;
    privateName.text = description;
    privateName.owner = LayoutID(0u);
    PropertyKey privateNameOfOtherClass = privateName;
    privateNameOfOtherClass.owner = LayoutID(1u);
    AOT_TEST_CHECK(arena.intern(privateName) != arena.intern(privateNameOfOtherClass) && arena.intern(privateName) != text);
    AOT_TEST_CHECK(types.numberOfPropertyKeys() == 8 && types.at(registered).kind == PropertyKeyKind::RegisteredSymbol && types.at(registered).text == description);

    TypeArena laterArena(types);
    AOT_TEST_CHECK(laterArena.registeredSymbolKey(description) == registered && types.numberOfPropertyKeys() == 8);
}

static void wellKnownTypeIDs()
{
    TypeTable types(BuiltinTable::singleton());
    struct WellKnownType {
        TypeID id;
        Type type;
    };
    const WellKnownType wellKnownTypes[] = {
        { TypeTable::never, Type::never() },
        { TypeTable::undefined, Type::fromPrimitives(Primitive::Undefined) },
        { TypeTable::boolean, Type::fromPrimitives(Primitive::Boolean) },
        { TypeTable::int32, Type::integer(Range::int32()) },
        { TypeTable::integer, Type::integer(Range::full()) },
        { TypeTable::number, Type::fromPrimitives(Primitive::Number) },
        { TypeTable::string, Type::fromPrimitives(Primitive::String) },
        { TypeTable::function, Type::function() },
        { TypeTable::object, Type::object() },
        { TypeTable::anything, Type::anything() },
        { TypeTable::null, Type::fromPrimitives(Primitive::Null) },
        { TypeTable::nullish, Type::fromPrimitives(Primitive::UndefinedOrNull) },
        { TypeTable::absent, Type::fromPrimitives(Primitive::Absent) },
        { TypeTable::guard, Type::forOpaque(OpaqueKind::Guard) },
        { TypeTable::length, Type::integer(Range::length()) },
    };
    // Behind them stands one type without a name: what a boxed location with a dead zone holds.
    constexpr size_t numberOfTypesInNewTable = std::size(wellKnownTypes) + 1;
    AOT_TEST_CHECK(types.size() == numberOfTypesInNewTable);
    for (unsigned index = 0; index < std::size(wellKnownTypes); ++index) {
        const WellKnownType& expected = wellKnownTypes[index];
        AOT_TEST_CHECK(expected.id == TypeID(index) && types.intern(expected.type) == expected.id && types.find(types.at(expected.id)) == expected.id);
    }
    AOT_TEST_CHECK(types.intern(Type::anything().withPrimitives(Primitive::Absent)) == TypeID(static_cast<unsigned>(std::size(wellKnownTypes))));
    AOT_TEST_CHECK(types.size() == numberOfTypesInNewTable);
    AOT_TEST_CHECK(types.at(TypeTable::never).isNever() && types.at(TypeTable::anything).isAnything() && types.at(TypeTable::guard).isOpaque(OpaqueKind::Guard));
}

static void equalLayoutSetsGetOneID()
{
    TypeTable types(BuiltinTable::singleton());
    LayoutSetID set = types.intern(LayoutSet::complete({ LayoutID(2u), LayoutID(0u), LayoutID(1u) }));
    AOT_TEST_CHECK(types.intern(LayoutSet::complete({ LayoutID(0u), LayoutID(1u), LayoutID(2u) })) == set);
    AOT_TEST_CHECK(types.intern(LayoutSet::complete({ LayoutID(1u), LayoutID(2u), LayoutID(1u), LayoutID(0u) })) == set);
    AOT_TEST_CHECK(types.numberOfLayoutSets() == 1 && hasElements(types.at(set).written(), { LayoutID(0u), LayoutID(1u), LayoutID(2u) }));
    AOT_TEST_CHECK(types.intern(LayoutSet::open({ LayoutID(0u), LayoutID(1u), LayoutID(2u) })) != set);
    AOT_TEST_CHECK(types.intern(LayoutSet::complete({ LayoutID(0u), LayoutID(1u) })) != set && types.numberOfLayoutSets() == 3);
    AOT_TEST_CHECK(types.intern(LayoutSet::complete({ LayoutID(0u), LayoutID(1u), LayoutID(2u) }, true)) != set && types.numberOfLayoutSets() == 4);

    TypeArena arena(types);
    AOT_TEST_CHECK(arena.completeLayoutSet(std::array<LayoutID, 3> { LayoutID(1u), LayoutID(0u), LayoutID(2u) }) == set);
    TypeID type = arena.intern(Type::forLayouts(std::array<LayoutID, 3> { LayoutID(2u), LayoutID(1u), LayoutID(0u) }, arena));
    AOT_TEST_CHECK(types.at(type).layouts == set && arena.intern(Type::forLayoutSet(set)) == type);
}

static void onlyCompleteLayoutSetsListAllLayouts()
{
    static_assert(!std::is_convertible_v<Expected<std::span<const LayoutID>>, std::span<const LayoutID>>);
    static_assert(std::is_convertible_v<ProvenValue<std::span<const LayoutID>>, std::span<const LayoutID>>);
    static_assert(!std::is_constructible_v<ProvenValue<std::span<const LayoutID>>, std::span<const LayoutID>>);

    LayoutSet complete = LayoutSet::complete({ LayoutID(0u), LayoutID(1u) });
    AOT_TEST_CHECK(complete.all() && std::ranges::equal(*complete.all(), complete.written()) && std::ranges::equal(complete.completeMembers(), complete.written()));
    AOT_TEST_CHECK(complete.listsEveryLayout() && !complete.mayHoldObjectsWithoutLayout());

    LayoutSet besideObjectsWithoutLayout = LayoutSet::complete({ LayoutID(0u), LayoutID(1u) }, true);
    AOT_TEST_CHECK(!besideObjectsWithoutLayout.all() && std::ranges::equal(besideObjectsWithoutLayout.completeMembers(), complete.written()));
    AOT_TEST_CHECK(besideObjectsWithoutLayout.listsEveryLayout() && besideObjectsWithoutLayout.mayHoldObjectsWithoutLayout());

    LayoutSet open = LayoutSet::open({ LayoutID(0u), LayoutID(1u) });
    AOT_TEST_CHECK(!open.all() && !open.listsEveryLayout() && open.mayHoldObjectsWithoutLayout());
    AOT_TEST_CHECK(std::ranges::equal(open.seen().predicted(), complete.written()));
    AOT_TEST_CHECK(isSubsetOf(complete, open) && !isSubsetOf(open, complete) && overlaps(open, LayoutSet::complete({ LayoutID(7u) })));
}

static void builtinLayoutsAppendedOnce()
{
    TestProgram test;
    const BuiltinTable& table = test.program.builtins();
    unsigned count = table.layoutCount();
    test.program.layouts.grow(1);
    test.program.internBuiltinNameKeys();
    test.program.appendBuiltinLayouts();
    AOT_TEST_CHECK(count && test.program.layouts.size() == 1 + count);
    AOT_TEST_CHECK(test.program.layouts[0].kind == LayoutKind::Record && !test.program.layouts[0].builtinLayout);
    TypeArena arena(test.program.types);
    for (unsigned index = 0; index < count; ++index) {
        const Layout& layout = test.program.at(test.program.layoutOf(BuiltinLayoutID(index)));
        AOT_TEST_CHECK(layout.kind == LayoutKind::BuiltinResult && layout.builtinLayout == BuiltinLayoutID(index));
        std::span<const BuiltinLayoutField> expectedFields = table.at(BuiltinLayoutID(index)).fields;
        AOT_TEST_CHECK(layout.fields.size() == expectedFields.size() && layout.ownFieldCount == expectedFields.size());
        for (size_t position = 0; position < expectedFields.size(); ++position) {
            const BuiltinLayoutField& expected = expectedFields[position];
            const Field& field = layout.fields[position];
            AOT_TEST_CHECK(field.key == (expected.name ? test.program.keyOf(expected.name) : PropertyKeyID()));
            AOT_TEST_CHECK(field.declared.written() && field.declared.written() == arena.intern(typeOfBuiltinLayoutField(test.program, expected, arena)));
            AOT_TEST_CHECK(field.flags.contains(FieldFlags::IsRequired) == !expected.isOptional);
        }
    }
}

static void derivedLayoutIDsFollowLayoutsAsRead()
{
    TestProgram test;
    Program& program = test.program;
    unsigned count = program.builtins().layoutCount();
    program.layouts.grow(2);
    program.modules.grow(3);
    program.modules[1].kind = ModuleKind::CommonJs;
    program.recordCountsAsRead();
    program.internBuiltinNameKeys();
    program.appendBuiltinLayouts();
    program.appendNamespaceLayouts();

    AOT_TEST_CHECK(program.numberOfLayoutsAsRead() == 2 && program.layouts.size() == 2 + count + 2);
    for (unsigned index = 0; index < count; ++index)
        AOT_TEST_CHECK(program.layoutOf(BuiltinLayoutID(index)) == LayoutID(2 + index));
    AOT_TEST_CHECK(program.modules[0].namespaceLayout == LayoutID(2 + count) && !program.modules[1].namespaceLayout && program.modules[2].namespaceLayout == LayoutID(3 + count));
    for (ModuleID module : { ModuleID(0u), ModuleID(2u) }) {
        LayoutID id = program.at(module).namespaceLayout;
        AOT_TEST_CHECK(program.at(id).kind == LayoutKind::ModuleNamespace && program.at(id).namespaceOf == module && program.at(id).module == module);
    }
    for (unsigned index = 2; index < program.layouts.size(); ++index) {
        const LayoutSet& members = program.at(program.declaredMembersOf(LayoutID(index)));
        AOT_TEST_CHECK(members.listsEveryLayout() && hasElements(members.written(), { LayoutID(index) }));
    }
}

static void builtinNameKeysAreInverse()
{
    TestProgram test;
    const BuiltinTable& table = test.program.builtins();
    test.program.internBuiltinNameKeys();
    unsigned numberOfSymbolNames = 0;
    for (unsigned index = 0; index < table.nameCount(); ++index) {
        BuiltinName name(index);
        PropertyKeyID key = test.program.keyOf(name);
        AOT_TEST_CHECK(key && test.program.builtinNameOf(key) == name);
        PropertyKeyKind kind = test.program.at(key).kind;
        if (table.isRegisteredSymbolName(name))
            AOT_TEST_CHECK(kind == PropertyKeyKind::RegisteredSymbol);
        else
            AOT_TEST_CHECK(kind == (table.isSymbolName(name) ? PropertyKeyKind::WellKnownSymbol : PropertyKeyKind::Text));
        numberOfSymbolNames += kind != PropertyKeyKind::Text;
    }
    AOT_TEST_CHECK(numberOfSymbolNames && numberOfSymbolNames < table.nameCount());
    AOT_TEST_CHECK(test.program.types.numberOfPropertyKeys() == table.nameCount());

    TypeArena arena(test.program.types);
    AOT_TEST_CHECK(!test.program.builtinNameOf(arena.textKey(test.program.strings.intern(StringView("no name of the table"_s)))));
}

static void lineColumnWithoutTable()
{
    TestProgram test;
    test.program.modules.grow(1);
    for (ModuleID module : { ModuleID(0u), ModuleID(1u), ModuleID() })
        AOT_TEST_CHECK((test.program.lineColumnFor(module, 25) == LineColumn { 1, 1 }));
}

static void lineColumnAfterNonASCII()
{
    static constexpr uint32_t lineStarts[] { 0, 10, 20 };
    TestProgram test;
    test.program.modules.grow(1);
    SourcePositions& positions = test.program.modules[0].positions;
    positions.lineStarts = lineStarts;
    positions.columnAdjustments.append(ColumnAdjustment { 17, 1, 5 });
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 12) == LineColumn { 2, 3 }));
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 14) == LineColumn { 2, 5 }));
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 17) == LineColumn { 2, 6 }));
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 19) == LineColumn { 2, 8 }));
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 22) == LineColumn { 3, 3 }));
}

static void lineColumnAtEveryBoundary()
{
    static constexpr uint32_t lineStarts[] { 0, 3 };
    static_assert(sizeof("ab\n\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80x") - 1 == 13);
    TestProgram test;
    test.program.modules.grow(1);
    SourcePositions& positions = test.program.modules[0].positions;
    positions.lineStarts = lineStarts;
    positions.columnAdjustments = { ColumnAdjustment { 5, 1, 1 }, ColumnAdjustment { 8, 1, 2 }, ColumnAdjustment { 12, 1, 4 } };
    struct Boundary {
        uint32_t position;
        unsigned column;
    };
    for (Boundary boundary : { Boundary { 3, 1 }, Boundary { 5, 2 }, Boundary { 8, 3 }, Boundary { 12, 5 }, Boundary { 13, 6 } })
        AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), boundary.position) == LineColumn { 2, boundary.column }));
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 1) == LineColumn { 1, 2 }));
}

static void lineColumnAfterLineSeparator()
{
    static constexpr uint32_t lineStarts[] { 0, 4 };
    static_assert(sizeof("a\xe2\x80\xa8" "b") - 1 == 5);
    TestProgram test;
    test.program.modules.grow(1);
    SourcePositions& positions = test.program.modules[0].positions;
    positions.lineStarts = lineStarts;
    positions.columnAdjustments.append(ColumnAdjustment { 4, 0, 2 });
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 1) == LineColumn { 1, 2 }));
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 4) == LineColumn { 2, 1 }));
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 5) == LineColumn { 2, 2 }));
}

static void lineColumnWithoutLineStarts()
{
    TestProgram test;
    test.program.modules.grow(1);
    SourcePositions& positions = test.program.modules[0].positions;
    positions.columnAdjustments = { ColumnAdjustment { 40, 0, 7 }, ColumnAdjustment { 90, 2, 0 } };
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 0) == LineColumn { 1, 1 }));
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 40) == LineColumn { 1, 8 }));
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 43) == LineColumn { 1, 11 }));
    AOT_TEST_CHECK((test.program.lineColumnFor(ModuleID(0u), 90) == LineColumn { 3, 1 }));
}

static void functionsCalledByHostAscending()
{
    TestProgram test;
    Program& program = test.program;
    FunctionID source = test.addFunction(createFunctionWithRole(FunctionRole::Source));
    FunctionID coroutineBody = test.addFunction(createFunctionWithRole(FunctionRole::CoroutineBody, createCoroutineRecord(CoroutineKind::AsyncFunction)));
    FunctionID hostBuiltin = test.addFunction(createFunctionWithRole(FunctionRole::Library));
    FunctionID compilerFunction = test.addFunction(createFunctionWithRole(FunctionRole::Library));
    FunctionID model = test.addFunction(createFunctionWithRole(FunctionRole::Model));
    FunctionID unregistered = test.addFunction(createFunctionWithRole(FunctionRole::Library));
    FunctionID entryBody = test.addFunction(createFunctionWithRole(FunctionRole::Source));
    FunctionID importedBody = test.addFunction(createFunctionWithRole(FunctionRole::Source));
    program.modules.grow(3);
    program.modules[0].body = importedBody;
    program.modules[1].body = entryBody;
    program.entries.append(Entry { ModuleID(1u), { } });
    program.runtimeFunctions.append(RuntimeFunctionEntry { findRuntimeFunctionWithRole(RuntimeFunctionRole::HostBuiltin), hostBuiltin, FunctionVisibility::Public });
    program.runtimeFunctions.append(RuntimeFunctionEntry { findRuntimeFunctionWithRole(RuntimeFunctionRole::CompilerFunction), compilerFunction, FunctionVisibility::Public });
    program.runtimeFunctions.append(RuntimeFunctionEntry { findRuntimeFunctionWithRole(RuntimeFunctionRole::Model), model, FunctionVisibility::Public });
    std::ranges::sort(program.runtimeFunctions, { }, &RuntimeFunctionEntry::id);

    Vector<FunctionID> visited;
    forEachFunctionCalledByHost(program, [&](FunctionID id) { visited.append(id); });
    AOT_TEST_CHECK(hasElements(visited, { coroutineBody, hostBuiltin, entryBody, importedBody }));
    for (FunctionID id : { source, compilerFunction, model, unregistered })
        AOT_TEST_CHECK(!visited.contains(id));
    AOT_TEST_CHECK(program.isEntry(entryBody) && !program.isEntry(importedBody) && !program.isEntry({ }));
}

static void verifierRejectsTooManyCaptures()
{
    TestProgram test;
    test.program.modules.grow(1);
    BuildContext build(makeDefaultCompileOptions());
    Function function = createFunctionWithRole(FunctionRole::Source);
    function.module = ModuleID(0u);
    function.captures.fill(createDeclaredType(TypeTable::anything), build.limit(LimitID::MaxCaptures));
    FunctionID id = test.addFunction(WTF::move(function));
    AOT_TEST_CHECK(verifyProgram(test.program, IRForm::Prepared, build));

    test.program.at(id).captures.append(createDeclaredType(TypeTable::anything));
    AOT_TEST_CHECK(!verifyProgram(test.program, IRForm::Prepared, build));
    build.diagnostics().finish(&test.program);
    const Vector<InternalError>& errors = build.diagnostics().internalErrors();
    AOT_TEST_CHECK(errors.size() == 1 && errors[0].kind == InternalErrorKind::ListOutOfBounds && errors[0].limit == LimitID::MaxCaptures);
    AOT_TEST_CHECK(build.diagnostics().userDiagnostics().isEmpty());
}

#define FOR_EACH_AOT_IR_PROGRAM_TEST(v) \
    v(verifierRejectsRecordOfAnotherRole) v(enclosingRuntimeFunctionMustBeRegistered) \
    v(editorRecordsChangedFunctions) v(editorTellsFunctionsGivenOutOnlyForStorage) v(editorAddFunctionLikeFollowsDerivationSource) v(editorRemoveBodyKeepsSignature) v(editorReservesConsecutiveFunctions) \
    v(editorAddedLayoutIsItsOnlyDeclaredMember) \
    v(expectedTypeOfValueWinsOverItsType) v(propertyKeysAreUnique) v(wellKnownTypeIDs) v(equalLayoutSetsGetOneID) v(onlyCompleteLayoutSetsListAllLayouts) \
    v(builtinLayoutsAppendedOnce) v(derivedLayoutIDsFollowLayoutsAsRead) v(builtinNameKeysAreInverse) \
    v(lineColumnWithoutTable) v(lineColumnAfterNonASCII) v(lineColumnAtEveryBoundary) v(lineColumnAfterLineSeparator) v(lineColumnWithoutLineStarts) v(functionsCalledByHostAscending) \
    v(verifierRejectsTooManyCaptures)

std::span<const NativeTest> irProgramTests()
{
#define AOT_IR_PROGRAM_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_IR_PROGRAM_TEST(AOT_IR_PROGRAM_TEST_ENTRY) };
#undef AOT_IR_PROGRAM_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
