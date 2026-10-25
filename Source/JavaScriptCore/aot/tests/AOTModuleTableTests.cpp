/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleTable.h"
#include "AOTNativeTest.h"
#include "AOTVector.h"
#include "Identifier.h"
#include "JSCInlines.h"
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> moduleTableTests();

static constexpr ASCIILiteral programText = R"IR(variable g0 name="a" module=m3 declared={type=anything} initialization=Undefined
variable g1 name="b" module=m3 declared={type=anything} initialization=Undefined
variable g2 name="c" module=m3 declared={type=anything} initialization=Undefined
variable g3 name="d" module=m3 declared={type=anything} initialization=Undefined
variable g4 name="e" module=m3 declared={type=anything} initialization=Undefined
module m0 path="/app/main.js" displayPath="main.js" body=f0 requests=[{target=m1 completes=true}, {target=m2 completes=true}]
module m1 path="/app/left.js" displayPath="left.js" body=f1 requests=[{target=m3 completes=true}]
module m2 path="/app/right.js" displayPath="right.js" body=f2 requests=[{target=m3 completes=true}]
module m3 path="/app/shared.js" displayPath="shared.js" body=f3 requests=[{target=m4 completes=true}] exports=[{name="alpha" variable=g0}, {name="alphabet" variable=g1}, {name="beta" variable=g2}, {name="\u{1f600}" variable=g3}, {name="\u{ff5e}" variable=g4}]
module m4 path="/app/shared.js.map.js" displayPath="shared.js.map.js" body=f4 requests=[{target=m3 completes=true}]
module m5 path="/app/\u{e9}.js" displayPath="e.js" body=f5
module m6 path="/app/\u{ff5e}.js" displayPath="tilde.js" body=f6
module m7 path="/app/\u{1f600}.js" displayPath="face.js" body=f7
module m8 path="/a" displayPath="a" body=f8
module m9 path="/app/lib.js" displayPath="lib.js" body=f9
module m10 path="/app/other.js" displayPath="other.js" body=f10
module m11 path="/entry.js" displayPath="entry.js" body=f11 requests=[{target=m0 completes=true}, {target=m5 completes=true}, {target=m6 completes=true}, {target=m7 completes=true}, {target=m8 completes=true}]
entry module=m11

function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="use" [isFreeName]
    v2 = RequireModule module=m9 referrer=m0 specifier="./lib.js"
    v3 = RequireModule module=m10 referrer=m0 specifier="./other.js"
    v4 = RequireModule module=m9 referrer=m0 specifier="./lib.js"
    v5 = Call v1, -, v2, v3, v4
    Return
}
function f1 name="left" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="use" [isFreeName]
    v2 = RequireModule module=m9 referrer=m1 specifier="./lib.js"
    v3 = Call v1, -, v2
    Return
}
function f2 name="right" module=m2 kind=Module results=[{type=anything}] {
    Return
}
function f3 name="shared" module=m3 kind=Module results=[{type=anything}] {
    v0 = String string="alpha"
    v1 = Strcat v0, v0
    v2 = ModuleNamespace module=m3
    v3 = ElemGet v2, v1
    PutModuleVariable v3 variable=g0
    PutModuleVariable v3 variable=g1
    PutModuleVariable v3 variable=g2
    PutModuleVariable v3 variable=g3
    PutModuleVariable v3 variable=g4
    Return
}
function f4 name="map" module=m4 kind=Module results=[{type=anything}] {
    Return
}
function f5 name="accent" module=m5 kind=Module results=[{type=anything}] {
    Return
}
function f6 name="tilde" module=m6 kind=Module results=[{type=anything}] {
    Return
}
function f7 name="face" module=m7 kind=Module results=[{type=anything}] {
    Return
}
function f8 name="short" module=m8 kind=Module results=[{type=anything}] {
    Return
}
function f9 name="lib" module=m9 kind=Module results=[{type=anything}] {
    Return
}
function f10 name="other" module=m10 kind=Module results=[{type=anything}] {
    Return
}
function f11 name="entry" module=m11 kind=Module results=[{type=anything}] {
    Return
}
)IR"_s;

static constexpr ModuleID mainModule { 0u };
static constexpr ModuleID leftModule { 1u };
static constexpr ModuleID rightModule { 2u };
static constexpr ModuleID sharedModule { 3u };
static constexpr ModuleID mapModule { 4u };
static constexpr ModuleID accentModule { 5u };
static constexpr ModuleID tildeModule { 6u };
static constexpr ModuleID faceModule { 7u };
static constexpr ModuleID shortModule { 8u };
static constexpr ModuleID libraryModule { 9u };
static constexpr ModuleID otherModule { 10u };
static constexpr ModuleID entryModule { 11u };
static constexpr unsigned numberOfModules = 12;

static ModuleTable tableOfImage()
{
    return ModuleTable(Image::current()->programTables());
}

static String fromUTF8(const char* bytes)
{
    String text = String::fromUTF8(bytes);
    AOT_TEST_CHECK(!text.isNull());
    return text;
}

static String as16Bit(ASCIILiteral text)
{
    Vector<char16_t> characters;
    for (char character : text.span())
        characters.append(static_cast<char16_t>(character));
    String result(characters.span());
    AOT_TEST_CHECK(characters.isEmpty() || !result.is8Bit());
    return result;
}

static void checkLists(LoadedProgram&)
{
    ModuleTable table = tableOfImage();
    // Behind the modules of the program comes the one that withStandInLibrary() adds.
    AOT_TEST_CHECK(table.size() == numberOfModules + 1 && &table.programTables() == &Image::current()->programTables());

    auto hasTargets = [&](ModuleID module, std::initializer_list<ModuleID> targets) {
        return std::ranges::equal(table.requests(module), targets, { }, &ModuleRequestData::targetModule);
    };
    AOT_TEST_CHECK(hasTargets(mainModule, { leftModule, rightModule }) && hasTargets(leftModule, { sharedModule }) && hasTargets(rightModule, { sharedModule }));
    AOT_TEST_CHECK(hasTargets(sharedModule, { mapModule }) && hasTargets(mapModule, { sharedModule }) && hasTargets(accentModule, { }));
    AOT_TEST_CHECK(hasTargets(entryModule, { mainModule, accentModule, tildeModule, faceModule, shortModule }));

    // Only the walk of the entry point comes to the other modules, so its body calls their code and the loader has nothing to visit. The lists of a walk that is left to the loader: AOTModuleRequestListTests.cpp.
    for (unsigned index = 0; index < numberOfModules; ++index)
        AOT_TEST_CHECK(table.evaluationRequests(ModuleID(index)).empty());
    AOT_TEST_CHECK(table.allEvaluationRequests().empty() && !table.endOfEvaluationRequests(entryModule));
    AOT_TEST_CHECK(table.allLinkRequests().empty() && table.entryPointLinkPlan().empty());

    for (unsigned index = 0; index < numberOfModules; ++index) {
        ModuleID module(index);
        AOT_TEST_CHECK(table.exports(module).size() == (module == sharedModule ? 5 : 0));
        AOT_TEST_CHECK(table.syntheticExports(module).empty() && table.requiredNames(module).empty());
        AOT_TEST_CHECK(table.data(module).kind == ModuleKind::SourceText && table.hasFilePath(module));
        AOT_TEST_CHECK(!!table.data(module).bodyFunction() == (module == entryModule) && table.data(module).evaluatesItself() == (module == entryModule));
        AOT_TEST_CHECK(table.data(module).isSynchronous() && !table.data(module).needsLink() && !table.data(module).reachesUncompiledCode());
        AOT_TEST_CHECK(table.data(module).isSimple() == (module != sharedModule && module != mapModule));
        AOT_TEST_CHECK(table.data(module).needsEvaluation() == (module == entryModule));
        AOT_TEST_CHECK(table.data(module).isEntryPoint() == (module == entryModule) && !table.data(module).isLinkedWithEntryPoint() && table.linkRequests(module).empty());
    }
}

static void testModuleNumbersInOneAndTwoWords()
{
    constexpr uint16_t words[] { 7, 0, 65535, 1, 3, 2 };
    ModuleNumbers narrow(words, 1u << 16);
    AOT_TEST_CHECK(narrow.size() == 6 && narrow[2] == 65535 && narrow.contains(3) && !narrow.contains(4));
    AOT_TEST_CHECK(narrow.subspan(2, 2).size() == 2 && narrow.subspan(2, 2)[1] == 1 && narrow.subspan(6, 0).empty());

    ModuleNumbers wide(words, (1u << 16) + 1);
    AOT_TEST_CHECK(wide.size() == 3 && wide[0] == 7 && wide[1] == 65535 + (1u << 16) && wide[2] == 3 + (2u << 16));
    AOT_TEST_CHECK(wide.contains(7) && !wide.contains(65535) && wide.subspan(1, 2)[1] == wide[2]);
    uint64_t sum = 0;
    for (uint32_t module : wide)
        sum += module;
    AOT_TEST_CHECK(sum == 7 + 65535 + (1u << 16) + 3 + (2u << 16));
    AOT_TEST_CHECK(ModuleNumbers().empty() && !ModuleNumbers().size());
}

static void checkFindByPath(LoadedProgram&)
{
    ModuleTable table = tableOfImage();
    for (unsigned index = 0; index < numberOfModules; ++index) {
        ModuleID module(index);
        AOT_TEST_CHECK(table.findByPath(table.pathText(module)) == module);
    }
    // The files of one directory share it, and the path is put together again. A module of a cycle gets a record with its path for a key: that path is whole.
    AOT_TEST_CHECK(table.data(mainModule).directory != Number24::max && table.data(mainModule).directory == table.data(leftModule).directory && table.data(mainModule).directory != table.data(entryModule).directory);
    AOT_TEST_CHECK(table.data(sharedModule).directory == Number24::max && table.pathText(sharedModule) == "/app/shared.js"_s);
    AOT_TEST_CHECK(table.pathText(mainModule) == "/app/main.js"_s && table.pathText(entryModule) == "/entry.js"_s && table.pathText(shortModule) == "/a"_s);
    AOT_TEST_CHECK(table.pathText(faceModule) == fromUTF8("/app/\xf0\x9f\x98\x80.js"));
    // The end of the public path, and a text of its own.
    AOT_TEST_CHECK(table.displayPathText(mainModule) == "main.js"_s && table.displayPathText(entryModule) == "entry.js"_s && table.displayPathText(mapModule) == "shared.js.map.js"_s);
    AOT_TEST_CHECK(table.displayPathText(accentModule) == "e.js"_s && table.displayPathText(tildeModule) == "tilde.js"_s && table.displayPathText(faceModule) == "face.js"_s);

    AOT_TEST_CHECK(table.findByPath("/a"_s) == shortModule);
    AOT_TEST_CHECK(table.findByPath("/entry.js"_s) == entryModule);
    AOT_TEST_CHECK(table.findByPath("/app/main.js"_s) == mainModule && table.findByPath(as16Bit("/app/main.js"_s)) == mainModule);
    AOT_TEST_CHECK(table.findByPath("/app/shared.js"_s) == sharedModule && table.findByPath("/app/shared.js.map.js"_s) == mapModule);

    for (ASCIILiteral absentPath : { ""_s, "/"_s, "/app"_s, "/app/"_s, "/app/shared.j"_s, "/app/shared.js."_s, "/app/shared.js.map"_s, "/app/shared.js.map.js "_s, "/app/none.js"_s, "/app/Main.js"_s, "/aa"_s, "/entry.js/"_s, "/zzz"_s, "main.js"_s, "app/main.js"_s }) {
        AOT_TEST_CHECK(!table.findByPath(absentPath));
        AOT_TEST_CHECK(!table.findByPath(as16Bit(absentPath)) || absentPath.isEmpty());
    }
}

static void checkFindByPathOutsideASCII(LoadedProgram&)
{
    ModuleTable table = tableOfImage();
    AOT_TEST_CHECK(table.findByPath(fromUTF8("/app/\xc3\xa9.js")) == accentModule);
    AOT_TEST_CHECK(table.findByPath(fromUTF8("/app/\xef\xbd\x9e.js")) == tildeModule);
    AOT_TEST_CHECK(table.findByPath(fromUTF8("/app/\xf0\x9f\x98\x80.js")) == faceModule);

    AOT_TEST_CHECK(!table.findByPath("/app/e.js"_s));
    AOT_TEST_CHECK(!table.findByPath(fromUTF8("/app/\xc3\xa8.js")) && !table.findByPath(fromUTF8("/app/\xc3\xa9")) && !table.findByPath(fromUTF8("/app/\xc3\xa9.jsx")));
    AOT_TEST_CHECK(!table.findByPath(fromUTF8("/app/\xef\xbd\x9d.js")) && !table.findByPath(fromUTF8("/app/\xef\xbd\x9f.js")));
    AOT_TEST_CHECK(!table.findByPath(fromUTF8("/app/\xf0\x9f\x98\x81.js")) && !table.findByPath(fromUTF8("/app/\xf0\x9f\x98\x80")));

    constexpr char16_t leadSurrogateOnly[] = { '/', 'a', 'p', 'p', '/', 0xd83d, '.', 'j', 's' };
    AOT_TEST_CHECK(!table.findByPath(StringView(std::span<const char16_t>(leadSurrogateOnly))));
}

static std::optional<unsigned> findExport(LoadedProgram& program, ModuleID module, const String& name)
{
    return tableOfImage().findExport(module, Identifier::fromString(program.vm(), name).impl());
}

static void checkFindExport(LoadedProgram& program)
{
    AOT_TEST_CHECK(findExport(program, sharedModule, "alpha"_s) == 0u);
    AOT_TEST_CHECK(findExport(program, sharedModule, "alphabet"_s) == 1u);
    AOT_TEST_CHECK(findExport(program, sharedModule, "beta"_s) == 2u);
    AOT_TEST_CHECK(findExport(program, sharedModule, as16Bit("beta"_s)) == 2u);

    for (ASCIILiteral absentName : { ""_s, "a"_s, "alph"_s, "alphab"_s, "alphabets"_s, "Alpha"_s, "bet"_s, "betas"_s, "gamma"_s, "default"_s })
        AOT_TEST_CHECK(!findExport(program, sharedModule, absentName));
    for (ModuleID moduleWithoutExports : { mainModule, mapModule, entryModule })
        AOT_TEST_CHECK(!findExport(program, moduleWithoutExports, "alpha"_s));
}

static void checkFindExportOfSymbol(LoadedProgram& program)
{
    ModuleTable table = tableOfImage();
    VM& vm = program.vm();
    AOT_TEST_CHECK(!table.findExport(sharedModule, vm.propertyNames->iteratorSymbol.impl()));
    AOT_TEST_CHECK(!table.findExport(sharedModule, vm.propertyNames->toStringTagSymbol.impl()));

    Ref<SymbolImpl> symbolNamedLikeAnExport = SymbolImpl::create(*String("alpha"_s).impl());
    AOT_TEST_CHECK(!table.findExport(sharedModule, symbolNamedLikeAnExport.ptr()));
    AOT_TEST_CHECK(table.findExport(sharedModule, Identifier::fromString(vm, "alpha"_s).impl()) == 0u);
}

static void checkFindExportWithSurrogatePair(LoadedProgram& program)
{
    String face = fromUTF8("\xf0\x9f\x98\x80");
    String tilde = fromUTF8("\xef\xbd\x9e");
    std::optional<unsigned> indexOfFace = findExport(program, sharedModule, face);
    std::optional<unsigned> indexOfTilde = findExport(program, sharedModule, tilde);
    AOT_TEST_CHECK(indexOfFace && indexOfTilde && *indexOfFace != *indexOfTilde && *indexOfFace >= 3 && *indexOfTilde >= 3);

    AOT_TEST_CHECK(!findExport(program, sharedModule, fromUTF8("\xf0\x9f\x98\x81")) && !findExport(program, sharedModule, fromUTF8("\xef\xbd\x9d")));
    AOT_TEST_CHECK(!findExport(program, sharedModule, makeString(face, face)) && !findExport(program, sharedModule, makeString(tilde, 'a')));
    constexpr char16_t leadSurrogateOnly[] = { 0xd83d };
    AOT_TEST_CHECK(!findExport(program, sharedModule, String(std::span<const char16_t>(leadSurrogateOnly))));
}

// The loader answers these calls, so it links what they name when they run: the tables have no request for them.
static void checkRequireOfLoaderIsNoRequest(LoadedProgram&)
{
    ModuleTable table = tableOfImage();
    for (ModuleID requirer : { mainModule, leftModule }) {
        for (const ModuleRequestData& request : table.requests(requirer))
            AOT_TEST_CHECK(ModuleTable::isImport(request) && request.targetModule() != libraryModule && request.targetModule() != otherModule);
    }
}

static void testRequestOfRequire()
{
    ModuleRequestData imported = ModuleRequestData::create(7, true, false);
    ModuleRequestData required = ModuleRequestData::create(ModuleRequestData::maxTarget, false, false, true);
    AOT_TEST_CHECK(ModuleTable::isImport(imported) && imported.target() == 7 && imported.isDeferred && !imported.isDynamicStarExport);
    AOT_TEST_CHECK(!ModuleTable::isImport(required) && required.target() == ModuleRequestData::maxTarget && !required.isDeferred && !required.isDynamicStarExport);
}

template<LoadedProgramTest body>
static constexpr void (*withModules)() = runNativeTestWithLoadedProgram<moduleTableTests, programText, body>;

std::span<const NativeTest> moduleTableTests()
{
    static constexpr NativeTest tests[] = {
        { "module-table/lists"_s, withModules<checkLists> },
        { "module-table/module-numbers-in-one-and-two-words"_s, testModuleNumbersInOneAndTwoWords },
        { "module-table/find-by-path"_s, withModules<checkFindByPath> },
        { "module-table/path-outside-ascii"_s, withModules<checkFindByPathOutsideASCII> },
        { "module-table/find-export"_s, withModules<checkFindExport> },
        { "module-table/export-of-symbol"_s, withModules<checkFindExportOfSymbol> },
        { "module-table/export-with-surrogate-pair"_s, withModules<checkFindExportWithSurrogatePair> },
        { "module-table/require-of-the-loader-is-no-request"_s, withModules<checkRequireOfLoaderIsNoRequest> },
        { "module-table/request-of-a-require"_s, testRequestOfRequire },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
