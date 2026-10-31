/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTIRParser.h"
#include "AOTReaderTestSupport.h"
#include "AOTVector.h"
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

namespace {

using namespace ReaderTests;

void testLayouts()
{
    ReaderFixture fixture;
    uint32_t firstStaticField = fixture.spanAt(layoutOffset(fixture, L2, offsetof(Wire::Layout, staticFields))).start;
    expectEach(fixture, {
        { "Layout.kind"_s, layoutOffset(fixture, L0, offsetof(Wire::Layout, kind)), numberOfEnumerators<LayoutKind>, Kind::UnknownEnumerator },
        { "layout kind of the engine"_s, layoutOffset(fixture, L1, offsetof(Wire::Layout, kind)), static_cast<uint32_t>(LayoutKind::Closure), Kind::EngineOnlyOnWire },
        { "Layout.flags"_s, layoutOffset(fixture, L0, offsetof(Wire::Layout, flags)), highBit, Kind::UnknownFlag },
        { "derived layout as base"_s, layoutOffset(fixture, L3, offsetof(Wire::Layout, base)), fixture.countOf(List::Layouts), Kind::IdOutOfRange },
        { "derived layout as supertype"_s, wordOffset(fixture, fixture.spanAt(layoutOffset(fixture, L3, offsetof(Wire::Layout, supertypes))).start + 1), fixture.countOf(List::Layouts), Kind::IdOutOfRange },
        { "Field.flags"_s, fieldOffset(fixture, L0, 0, offsetof(Wire::Field, flags)), highBit, Kind::UnknownFlag },
        { "field flag of the engine"_s, fieldOffset(fixture, L0, 0, offsetof(Wire::Field, flags)), static_cast<uint32_t>(FieldFlags::IsInherited), Kind::EngineOnlyOnWire },
        { "DeclaredType.flags"_s, fieldOffset(fixture, L0, 0, offsetof(Wire::Field, declared.flags)), highBit, Kind::UnknownFlag },
        { "two classes with one constructor"_s, layoutOffset(fixture, L3, offsetof(Wire::Layout, constructor)), f1.index(), Kind::InvalidClass },
        { "StaticField.variable"_s, fixture.offsetOf<Wire::StaticField>(List::StaticFields, firstStaticField, offsetof(Wire::StaticField, variable)), Wire::none, Kind::IdOutOfRange },
    });
    expectOrderChecked(fixture, "supertypes"_s, wordOffset(fixture, fixture.spanAt(layoutOffset(fixture, L3, offsetof(Wire::Layout, supertypes))).start), wordSize);
    expectAccepted("a class that extends itself"_s, readBytes(damagedCopy(fixture, layoutOffset(fixture, L3, offsetof(Wire::Layout, base)), L3.index()).span()));
}

void testDerivedLayouts()
{
    ReaderFixture fixture;
    auto program = fixture.read();
    const BuiltinTable& builtins = BuiltinTable::singleton();
    AOT_TEST_CHECK(builtins.layoutCount());
    for (unsigned index = 0; index < builtins.layoutCount(); ++index) {
        LayoutID id = program->layoutOf(BuiltinLayoutID(index));
        const Layout& layout = program->at(id);
        AOT_TEST_CHECK(id == LayoutID(program->numberOfLayoutsAsRead() + index));
        AOT_TEST_CHECK(layout.kind == LayoutKind::BuiltinResult && layout.builtinLayout == BuiltinLayoutID(index));
        AOT_TEST_CHECK(layout.fields.size() == builtins.at(BuiltinLayoutID(index)).fields.size() && layout.ownFieldCount == layout.fields.size());
        AOT_TEST_CHECK(hasElements(program->at(program->declaredMembersOf(id)).written(), { id }));
    }

    LayoutID namespaceLayout = program->at(m0).namespaceLayout;
    AOT_TEST_CHECK(namespaceLayout == LayoutID(program->layouts.size() - 1) && !program->at(m1).namespaceLayout);
    AOT_TEST_CHECK(program->at(namespaceLayout).kind == LayoutKind::ModuleNamespace && program->at(namespaceLayout).namespaceOf == m0);
    AOT_TEST_CHECK(program->at(namespaceLayout).fields.size() == program->at(m0).exports.size());

    AOT_TEST_CHECK(hasElements(program->at(program->declaredMembersOf(L0)).written(), { L0, L2, L3 }));
    AOT_TEST_CHECK(hasElements(program->at(program->declaredMembersOf(L1)).written(), { L1 }));
    AOT_TEST_CHECK(hasElements(program->at(program->declaredMembersOf(L2)).written(), { L2, L3 }));
    AOT_TEST_CHECK(hasElements(program->at(program->declaredMembersOf(L3)).written(), { L3 }));
}

constexpr ASCIILiteral privateNamesText = R"IR(layout L0 kind=Instance name="C" module=m0 fields=[{key=#"p"@L0 declared={type=number} flags=[IsRequired, IsPrivate]}, {key="p" declared={type=number}}]
layout L1 kind=Instance name="D" module=m0 base=L0 fields=[{key=#"p"@L1 declared={type=number} flags=[IsRequired, IsPrivate]}] flags=[HasHeritage]
module m0 path="/a.ts" displayPath="a.ts"
entry module=m0
)IR"_s;

void testPrivateNames()
{
    ReaderFixture fixture(privateNamesText);
    auto program = fixture.read();
    PropertyKeyID nameInBase = program->at(L0).fields[0].key;
    PropertyKeyID publicName = program->at(L0).fields[1].key;
    PropertyKeyID nameInSubclass = program->at(L1).fields[0].key;
    AOT_TEST_CHECK(nameInBase != nameInSubclass && nameInBase != publicName && nameInSubclass != publicName);
    AOT_TEST_CHECK(program->at(nameInBase).kind == PropertyKeyKind::PrivateName && program->at(nameInBase).owner == L0);
    AOT_TEST_CHECK(program->at(nameInSubclass).kind == PropertyKeyKind::PrivateName && program->at(nameInSubclass).owner == L1);
    AOT_TEST_CHECK(program->at(publicName).kind == PropertyKeyKind::Text && !program->at(publicName).owner);
    AOT_TEST_CHECK(program->at(nameInBase).text == program->at(nameInSubclass).text && program->at(nameInBase).text == program->at(publicName).text);
    AOT_TEST_CHECK(program->at(L0).findField(nameInBase) == 0u && program->at(L0).findField(publicName) == 1u && !program->at(L0).findField(nameInSubclass));
    expectAccepted("private names"_s, readBytes(fixture.bytes.span()));
}

void testLayoutRecords()
{
    ReaderFixture fixture;
    RecordDamages damages(fixture);
    auto layout = offsetsOf<Wire::Layout>(fixture, List::Layouts, L2.index());
    damages.optionalID("Layout.name"_s, layout(offsetof(Wire::Layout, name)), List::Strings);
    damages.optionalID("Layout.module"_s, layout(offsetof(Wire::Layout, module)), List::Modules);
    damages.optionalID("Layout.base"_s, layout(offsetof(Wire::Layout, base)), numberOfLayoutIDs(fixture));
    damages.optionalID("Layout.constructor"_s, layout(offsetof(Wire::Layout, constructor)), List::Functions);
    damages.span("Layout.fields"_s, layout(offsetof(Wire::Layout, fields)));
    damages.span("Layout.staticFields"_s, layout(offsetof(Wire::Layout, staticFields)));
    damages.span("Layout.supertypes"_s, layout(offsetof(Wire::Layout, supertypes)));

    damages.optionalID("Field.key"_s, fieldOffset(fixture, L2, 0, offsetof(Wire::Field, key)), List::PropertyKeys);
    damages.flags("Field.form"_s, fieldOffset(fixture, L2, 0, offsetof(Wire::Field, form)));
    damages.optionalID("Field.originLayout"_s, fieldOffset(fixture, L2, 0, offsetof(Wire::Field, originLayout)), numberOfLayoutIDs(fixture));
    addDeclaredTypeDamages(damages, fieldOffset(fixture, L0, 0, offsetof(Wire::Field, declared)));

    auto staticField = offsetsOf<Wire::StaticField>(fixture, List::StaticFields, 0);
    damages.optionalID("StaticField.key"_s, staticField(offsetof(Wire::StaticField, key)), List::PropertyKeys);
    damages.id("StaticField.variable"_s, staticField(offsetof(Wire::StaticField, variable)), List::Variables);

    for (uint32_t index = 0; index < fixture.countOf(List::Variables); ++index) {
        auto variable = offsetsOf<Wire::Variable>(fixture, List::Variables, index);
        damages.optionalID("Variable.name"_s, variable(offsetof(Wire::Variable, name)), List::Strings);
        damages.id("Variable.module"_s, variable(offsetof(Wire::Variable, module)), List::Modules);
        addDeclaredTypeDamages(damages, variable(offsetof(Wire::Variable, declared)));
        damages.enumerator<VariableInitialization>("Variable.initialization"_s, variable(offsetof(Wire::Variable, initialization)));
        damages.optionalID("Variable.function"_s, variable(offsetof(Wire::Variable, function)), List::Functions);
        damages.optionalID("Variable.owner"_s, variable(offsetof(Wire::Variable, owner)), numberOfLayoutIDs(fixture));
        damages.flags("Variable.flags"_s, variable(offsetof(Wire::Variable, flags)));
    }
    expectEach(fixture, damages.damages());
}

void addModuleListDamages(const ReaderFixture& fixture, RecordDamages& damages)
{
    auto request = offsetsOf<Wire::ModuleRequest>(fixture, List::ModuleRequests, 0);
    damages.id("ModuleRequest.target"_s, request(offsetof(Wire::ModuleRequest, target)), List::Modules);
    damages.enumerator<ImportPhase>("ModuleRequest.phase"_s, request(offsetof(Wire::ModuleRequest, phase)));
    damages.boolean("ModuleRequest.isDynamicStarExport"_s, request(offsetof(Wire::ModuleRequest, isDynamicStarExport)));
    damages.enumerator<ModuleRequestKind>("ModuleRequest.kind"_s, request(offsetof(Wire::ModuleRequest, kind)));
    damages.boolean("ModuleRequest.completes"_s, request(offsetof(Wire::ModuleRequest, completes)));
    damages.boolean("ModuleRequest.isHoisted"_s, request(offsetof(Wire::ModuleRequest, isHoisted)));

    for (uint32_t index = 0; index < fixture.countOf(List::ModuleExports); ++index) {
        auto entry = offsetsOf<Wire::ModuleExport>(fixture, List::ModuleExports, index);
        damages.id("ModuleExport.name"_s, entry(offsetof(Wire::ModuleExport, name)), List::Strings);
        damages.enumerator<ModuleExportKind>("ModuleExport.kind"_s, entry(offsetof(Wire::ModuleExport, kind)));
        damages.optionalID("ModuleExport.variable"_s, entry(offsetof(Wire::ModuleExport, variable)), List::Variables);
        damages.optionalID("ModuleExport.module"_s, entry(offsetof(Wire::ModuleExport, module)), List::Modules);
        damages.optionalID("ModuleExport.importName"_s, entry(offsetof(Wire::ModuleExport, importName)), List::Strings);
        damages.enumerator<ImportPhase>("ModuleExport.phase"_s, entry(offsetof(Wire::ModuleExport, phase)));
        damages.boolean("ModuleExport.isFromStarExport"_s, entry(offsetof(Wire::ModuleExport, isFromStarExport)));
    }
    for (uint32_t index = 0; index < fixture.countOf(List::SyntheticExports); ++index) {
        auto entry = offsetsOf<Wire::SyntheticExport>(fixture, List::SyntheticExports, index);
        damages.id("SyntheticExport.name"_s, entry(offsetof(Wire::SyntheticExport, name)), List::Strings);
        damages.optionalID("SyntheticExport.variable"_s, entry(offsetof(Wire::SyntheticExport, variable)), List::Variables);
        damages.boolean("SyntheticExport.isKnownMember"_s, entry(offsetof(Wire::SyntheticExport, isKnownMember)));
    }
    auto requiredName = offsetsOf<Wire::RequiredName>(fixture, List::RequiredNames, 0);
    damages.id("RequiredName.module"_s, requiredName(offsetof(Wire::RequiredName, module)), List::Modules);
    damages.id("RequiredName.name"_s, requiredName(offsetof(Wire::RequiredName, name)), List::Strings);
    damages.optionalID("RequiredName.exportName"_s, requiredName(offsetof(Wire::RequiredName, exportName)), List::Strings);
    damages.optionalID("RequiredName.specifier"_s, requiredName(offsetof(Wire::RequiredName, specifier)), List::Strings);
}

void testModuleRecords()
{
    ReaderFixture fixture;
    RecordDamages damages(fixture);
    for (ModuleID id : { m0, m1 }) {
        auto module = offsetsOf<Wire::Module>(fixture, List::Modules, id.index());
        damages.id("Module.path"_s, module(offsetof(Wire::Module, path)), List::Strings);
        damages.id("Module.displayPath"_s, module(offsetof(Wire::Module, displayPath)), List::Strings);
        damages.enumerator<ModuleKind>("Module.kind"_s, module(offsetof(Wire::Module, kind)));
        damages.flags("Module.flags"_s, module(offsetof(Wire::Module, flags)));
        damages.enumerator<SyntheticExportRule>("Module.exportRule"_s, module(offsetof(Wire::Module, exportRule)));
        damages.enumerator<RequireResult>("Module.requireResult"_s, module(offsetof(Wire::Module, requireResult)));
        damages.enumerator<ExternalKind>("Module.externalKind"_s, module(offsetof(Wire::Module, externalKind)));
        damages.optionalID("Module.typeAttribute"_s, module(offsetof(Wire::Module, typeAttribute)), List::Strings);
        damages.external("Module.runtimeModule"_s, module(offsetof(Wire::Module, runtimeModule)));
        damages.optionalID("Module.referrer"_s, module(offsetof(Wire::Module, referrer)), List::Modules);
        damages.optionalID("Module.body"_s, module(offsetof(Wire::Module, body)), List::Functions);
        damages.optionalID("Module.exportsVariable"_s, module(offsetof(Wire::Module, exportsVariable)), List::Variables);
        damages.optionalID("Module.data"_s, module(offsetof(Wire::Module, data)), List::Data);
        damages.span("Module.requests"_s, module(offsetof(Wire::Module, requests)));
        damages.span("Module.exports"_s, module(offsetof(Wire::Module, exports)));
        damages.span("Module.syntheticExports"_s, module(offsetof(Wire::Module, syntheticExports)));
        damages.span("Module.requiredNames"_s, module(offsetof(Wire::Module, requiredNames)));
        damages.span("SourcePositions.lineStarts"_s, module(offsetof(Wire::Module, positions.lineStarts)));
        damages.span("SourcePositions.columnAdjustments"_s, module(offsetof(Wire::Module, positions.columnAdjustments)));
        damages.span("SourcePositions.textPrintedInPlace"_s, module(offsetof(Wire::Module, positions.textPrintedInPlace)));
        damages.span("SourcePositions.textOfBody"_s, module(offsetof(Wire::Module, positions.textOfBody)));
        damages.span("SourcePositions.whereThisIsMade"_s, module(offsetof(Wire::Module, positions.whereThisIsMade)));
        damages.span("Module.functionText"_s, module(offsetof(Wire::Module, functionText)));
        damages.span("Module.typesCutFromFunctionText"_s, module(offsetof(Wire::Module, typesCutFromFunctionText)));
    }
    addModuleListDamages(fixture, damages);
    expectEach(fixture, damages.damages());

    expectOrderChecked(fixture, "exports"_s, fixture.offsetOf<Wire::ModuleExport>(List::ModuleExports, 0), sizeof(Wire::ModuleExport));
    expectOrderChecked(fixture, "synthetic exports"_s, fixture.offsetOf<Wire::SyntheticExport>(List::SyntheticExports, 0), sizeof(Wire::SyntheticExport));
}

void addExpectedResultDamages(const ReaderFixture& fixture, RecordDamages& damages)
{
    auto expectedResult = offsetsOf<Wire::ExpectedResult>(fixture, List::ExpectedResults, 0);
    damages.id("ExpectedResult.type"_s, expectedResult(offsetof(Wire::ExpectedResult, type)), List::Types);
    damages.span("ExpectedResult.layouts"_s, expectedResult(offsetof(Wire::ExpectedResult, layouts)));
    damages.id("layout of an expected result"_s, wordOffset(fixture, fixture.spanAt(expectedResult(offsetof(Wire::ExpectedResult, layouts))).start), numberOfLayoutIDs(fixture));
}

void addDataDamages(const ReaderFixture& fixture, RecordDamages& damages)
{
    auto data = offsetsOf<Wire::Data>(fixture, List::Data, 0);
    size_t rootWords = data(offsetof(Wire::Data, root.words));
    damages.variant("DataNode.kind"_s, data(offsetof(Wire::Data, root.kind)));
    damages.id("DataNode.Object.layout"_s, rootWords, numberOfLayoutIDs(fixture));
    damages.span("DataNode.Object.members"_s, rootWords + wordSize);
    damages.optionalID("Data.module"_s, data(offsetof(Wire::Data, module)), List::Modules);

    auto arrayMember = offsetsOf<Wire::DataMember>(fixture, List::DataMembers, 1);
    size_t arrayWords = arrayMember(offsetof(Wire::DataMember, value.words));
    damages.id("DataMember.key"_s, arrayMember(offsetof(Wire::DataMember, key)), List::PropertyKeys);
    damages.variant("kind of a member"_s, arrayMember(offsetof(Wire::DataMember, value.kind)));
    damages.id("DataNode.Array.contents"_s, arrayWords, List::Types);
    damages.span("DataNode.Array.elements"_s, arrayWords + wordSize);
    damages.variant("kind of an element"_s, fixture.offsetOf<Wire::DataNode>(List::DataNodes, 0, offsetof(Wire::DataNode, kind)));
}

void testSmallRecords()
{
    ReaderFixture fixture;
    RecordDamages damages(fixture);
    auto typeSite = offsetsOf<Wire::TypeSite>(fixture, List::TypeSites, 0);
    damages.id("TypeSite.module"_s, typeSite(offsetof(Wire::TypeSite, module)), List::Modules);
    damages.enumerator<TypeSiteKind>("TypeSite.kind"_s, typeSite(offsetof(Wire::TypeSite, kind)));
    damages.optionalID("TypeSite.text"_s, typeSite(offsetof(Wire::TypeSite, text)), List::Strings);
    addExpectedResultDamages(fixture, damages);

    auto failure = offsetsOf<Wire::ModuleFailure>(fixture, List::ModuleFailures, 0);
    damages.id("ModuleFailure.importer"_s, failure(offsetof(Wire::ModuleFailure, importer)), List::Modules);
    damages.id("ModuleFailure.specifier"_s, failure(offsetof(Wire::ModuleFailure, specifier)), List::Strings);

    auto evalScope = offsetsOf<Wire::EvalScope>(fixture, List::EvalScopes, 0);
    damages.enumerator<ECMAMode>("EvalScope.mode"_s, evalScope(offsetof(Wire::EvalScope, mode)));
    damages.flags("EvalScope.flags"_s, evalScope(offsetof(Wire::EvalScope, flags)));
    damages.span("EvalScope.bindings"_s, evalScope(offsetof(Wire::EvalScope, bindings)));
    auto binding = offsetsOf<Wire::EvalBinding>(fixture, List::EvalBindings, 0);
    damages.optionalID("EvalBinding.name"_s, binding(offsetof(Wire::EvalBinding, name)), List::Strings);
    damages.enumerator<EvalBindingKind>("EvalBinding.kind"_s, binding(offsetof(Wire::EvalBinding, kind)));
    damages.optionalID("EvalBinding.layout"_s, binding(offsetof(Wire::EvalBinding, layout)), numberOfLayoutIDs(fixture));
    damages.optionalID("EvalBinding.variable"_s, binding(offsetof(Wire::EvalBinding, variable)), List::Variables);
    addDataDamages(fixture, damages);

    auto entry = offsetsOf<Wire::Entry>(fixture, List::Entries, 0);
    damages.id("Entry.module"_s, entry(offsetof(Wire::Entry, module)), List::Modules);
    damages.span("Entry.hostReads"_s, entry(offsetof(Wire::Entry, hostReads)));
    auto hostRead = offsetsOf<Wire::HostRead>(fixture, List::HostReads, 0);
    damages.id("HostRead.exportName"_s, hostRead(offsetof(Wire::HostRead, exportName)), List::Strings);
    damages.external("HostRead.builtin"_s, hostRead(offsetof(Wire::HostRead, builtin)));

    auto runtimeFunction = offsetsOf<Wire::RuntimeFunctionEntry>(fixture, List::RuntimeFunctions, 0);
    damages.external("RuntimeFunctionEntry.id"_s, runtimeFunction(offsetof(Wire::RuntimeFunctionEntry, id)));
    damages.id("RuntimeFunctionEntry.function"_s, runtimeFunction(offsetof(Wire::RuntimeFunctionEntry, function)), List::Functions);
    damages.enumerator<FunctionVisibility>("RuntimeFunctionEntry.visibility"_s, runtimeFunction(offsetof(Wire::RuntimeFunctionEntry, visibility)));

    auto webAssemblyModule = offsetsOf<Wire::WebAssemblyModule>(fixture, List::WebAssemblyModules, 0);
    damages.id("WebAssemblyModule.module"_s, webAssemblyModule(offsetof(Wire::WebAssemblyModule, module)), List::Modules);
    damages.span("WebAssemblyModule.bytes"_s, webAssemblyModule(offsetof(Wire::WebAssemblyModule, bytes)));
    expectEach(fixture, damages.damages());

    expectOrderChecked(fixture, "runtime functions"_s, runtimeFunction(0), sizeof(Wire::RuntimeFunctionEntry));
}

void testDataTreeThatNamesItself()
{
    ReaderFixture fixture;
    size_t arrayNode = fixture.offsetOf<Wire::DataMember>(List::DataMembers, 1, offsetof(Wire::DataMember, value));
    size_t firstElement = fixture.offsetOf<Wire::DataNode>(List::DataNodes, 0);
    expectAfter(fixture, "an array that is its own element"_s, Kind::DuplicateListEntry, [&](Vector<uint8_t>& bytes) {
        memcpySpan(bytes.mutableSpan().subspan(firstElement, sizeof(Wire::DataNode)), fixture.bytes.subspan(arrayNode, sizeof(Wire::DataNode)));
    });
}

void testRecordsRefusedByVerifier()
{
    ReaderFixture fixture;
    WireTypes types(fixture);
    expectEachRejectedByVerifier(fixture, {
        { "no entries"_s, ReaderFixture::offsetOfSection(List::Entries, offsetof(Wire::Section, count)), 0, Kind::NoEntryPoint },
        { "parameter of no built-in"_s, fixture.offsetOf<Wire::HostRead>(List::HostReads, 0, offsetof(Wire::HostRead, parameter)), 1, Kind::InvalidEntry },
        { "hole as a member"_s, fixture.offsetOf<Wire::DataMember>(List::DataMembers, 0, offsetof(Wire::DataMember, value.kind)), static_cast<uint32_t>(DataNode::Kind::Hole), Kind::InvalidData },
        { "hole as a root"_s, fixture.offsetOf<Wire::Data>(List::Data, 0, offsetof(Wire::Data, root.kind)), static_cast<uint32_t>(DataNode::Kind::Hole), Kind::InvalidData },
        { "environment as the layout of data"_s, fixture.offsetOf<Wire::Data>(List::Data, 0, offsetof(Wire::Data, root.words)), L1.index(), Kind::InvalidData },
        { "enclosing function not registered"_s, functionOffset(fixture, f5, offsetof(Wire::Function, enclosingRuntimeFunction)), 2, Kind::InvalidCreatorRecord },
        { "type of this without HasThis"_s, functionOffset(fixture, f7, offsetof(Wire::Function, thisType.type)), types.number, Kind::InvalidCreatorRecord },
        { "private name owned by an environment"_s, keyOffset(fixture, fixture.word(fieldOffset(fixture, L2, 1, offsetof(Wire::Field, key))), offsetof(Wire::PropertyKey, owner)), L1.index(), Kind::InvalidPropertyKey },
    });
}

void testFunctionRecords()
{
    ReaderFixture fixture;
    RecordDamages damages(fixture);
    auto function = offsetsOf<Wire::Function>(fixture, List::Functions, f7.index());
    damages.optionalID("Function.name"_s, function(offsetof(Wire::Function, name)), List::Strings);
    damages.id("Function.module"_s, function(offsetof(Wire::Function, module)), List::Modules);
    damages.enumerator<FunctionKind>("Function.kind"_s, function(offsetof(Wire::Function, kind)));
    damages.enumerator<FunctionRole>("Function.role"_s, function(offsetof(Wire::Function, role)));
    damages.flags("Function.flags"_s, function(offsetof(Wire::Function, flags)));
    damages.external("Function.enclosingRuntimeFunction"_s, function(offsetof(Wire::Function, enclosingRuntimeFunction)));
    damages.span("Function.captures"_s, function(offsetof(Wire::Function, captures)));
    addDeclaredTypeDamages(damages, function(offsetof(Wire::Function, thisType)));
    damages.span("Function.parameters"_s, function(offsetof(Wire::Function, parameters)));
    damages.span("Function.results"_s, function(offsetof(Wire::Function, results)));
    damages.span("Body.code"_s, function(offsetof(Wire::Function, body.code)));
    addDeclaredTypeDamages(damages, fixture.offsetOf<Wire::DeclaredType>(List::DeclaredTypes, fixture.spanAt(function(offsetof(Wire::Function, parameters))).start));
    expectEach(fixture, damages.damages());

    size_t codeCount = function(offsetof(Wire::Function, body.code.count));
    size_t valueCount = function(offsetof(Wire::Function, body.valueCount));
    size_t blockCount = function(offsetof(Wire::Function, body.blockCount));
    expectEach(fixture, {
        { "role of the engine"_s, function(offsetof(Wire::Function, role)), static_cast<uint32_t>(FunctionRole::GenericEntrypoint), Kind::EngineOnlyOnWire },
        { "no results"_s, function(offsetof(Wire::Function, results.count)), 0, Kind::ListOutOfBounds },
        { "code one word longer"_s, codeCount, fixture.word(codeCount) + 1, Kind::ListOutOfBounds },
        { "code one word shorter"_s, codeCount, fixture.word(codeCount) - 1, Kind::ListOutOfBounds },
        { "no code"_s, codeCount, 0, Kind::ListOutOfBounds },
        { "one value too many"_s, valueCount, fixture.word(valueCount) + 1, Kind::ListOutOfBounds },
        { "one value too few"_s, valueCount, fixture.word(valueCount) - 1, Kind::IdOutOfRange },
        { "huge valueCount"_s, valueCount, Wire::none, Kind::ListOutOfBounds },
        { "one block too many"_s, blockCount, fixture.word(blockCount) + 1, Kind::ListOutOfBounds },
        { "one block too few"_s, blockCount, fixture.word(blockCount) - 1, Kind::IdOutOfRange },
        { "huge blockCount"_s, blockCount, Wire::none, Kind::ListOutOfBounds },
    });
}

void testTooManyCaptures()
{
    auto readWithCaptures = [](uint64_t numberOfCaptures) {
        StringBuilder text;
        text.append("module m0 path=\"/a.ts\" displayPath=\"a.ts\"\nfunction f0 name=\"f\" module=m0 captures=["_s);
        for (uint64_t index = 0; index < numberOfCaptures; ++index)
            text.append(index ? ", {type=anything}"_s : "{type=anything}"_s);
        text.append("] results=[{type=anything}]\nentry module=m0\n"_s);

        BuildContext build(createOptions());
        auto program = parseProgram(text.toString(), IRForm::AsRead, build);
        return finishRead(build, program.get());
    };
    uint64_t limit = BuildContext(createOptions()).limit(LimitID::MaxCaptures);
    expectAccepted("as many captures as a closure has"_s, readWithCaptures(limit));
    expectRejected("one capture too many"_s, readWithCaptures(limit + 1), Kind::ListOutOfBounds);
}

} // anonymous namespace

std::span<const NativeTest> readerRecordTests()
{
    static constexpr NativeTest tests[] = {
        { "reader/layouts"_s, testLayouts },
        { "reader/derived-layouts"_s, testDerivedLayouts },
        { "reader/private-names"_s, testPrivateNames },
        { "reader/layout-records"_s, testLayoutRecords },
        { "reader/module-records"_s, testModuleRecords },
        { "reader/small-records"_s, testSmallRecords },
        { "reader/data-tree-that-names-itself"_s, testDataTreeThatNamesItself },
        { "reader/records-refused-by-verifier"_s, testRecordsRefusedByVerifier },
        { "reader/function-records"_s, testFunctionRecords },
        { "reader/too-many-captures"_s, testTooManyCaptures },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
