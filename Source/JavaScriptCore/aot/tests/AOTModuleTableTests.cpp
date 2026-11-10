/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTModuleHost.h"
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
    AOT_TEST_CHECK(table.size() == numberOfModules + 1 && &table.programTables() == &Image::current()->programTables());

    auto hasTargets = [&](ModuleID module, std::initializer_list<ModuleID> targets) {
        return std::ranges::equal(table.requests(module), targets, { }, &ModuleRequestData::targetModule);
    };
    AOT_TEST_CHECK(hasTargets(mainModule, { leftModule, rightModule }) && hasTargets(leftModule, { sharedModule }) && hasTargets(rightModule, { sharedModule }));
    AOT_TEST_CHECK(hasTargets(sharedModule, { mapModule }) && hasTargets(mapModule, { sharedModule }) && hasTargets(accentModule, { }));
    AOT_TEST_CHECK(hasTargets(entryModule, { mainModule, accentModule, tildeModule, faceModule, shortModule }));

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
        AOT_TEST_CHECK(table.findByPath(table.fullPath(module)) == module);
    }
    AOT_TEST_CHECK(table.data(mainModule).directory != Number24::max && table.data(mainModule).directory == table.data(leftModule).directory && table.data(mainModule).directory != table.data(entryModule).directory);
    AOT_TEST_CHECK(table.data(sharedModule).directory == Number24::max && table.fullPath(sharedModule) == "/app/shared.js"_s);
    AOT_TEST_CHECK(table.fullPath(mainModule) == "/app/main.js"_s && table.fullPath(entryModule) == "/entry.js"_s && table.fullPath(shortModule) == "/a"_s);
    AOT_TEST_CHECK(table.fullPath(faceModule) == fromUTF8("/app/\xf0\x9f\x98\x80.js"));
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

static std::optional<unsigned> findExport(LoadedProgram& program, ModuleID module, const String& name)
{
    return tableOfImage().findExport(module, Identifier::fromString(program.vm(), name).impl());
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

static void testPathBesideFile()
{
    AOT_TEST_CHECK(pathBesideFile("/app/main.js"_s, "./x.sqlite"_s) == "/app/x.sqlite"_s);
    AOT_TEST_CHECK(pathBesideFile("/app/lib/main.js"_s, "../data/./x.sqlite"_s) == "/app/data/x.sqlite"_s);
    AOT_TEST_CHECK(pathBesideFile("/main.js"_s, "../../x.sqlite"_s) == "/x.sqlite"_s);
    AOT_TEST_CHECK(pathBesideFile("/app/main.js"_s, "./a\\b.sqlite"_s) == "/app/a\\b.sqlite"_s);
    AOT_TEST_CHECK(pathBesideFile("/$bunfs/root/main.js"_s, "./x.sqlite"_s) == "/$bunfs/root/x.sqlite"_s);

    AOT_TEST_CHECK(pathBesideFile("C:\\a\\main.js"_s, "./x.sqlite"_s) == "C:\\a\\x.sqlite"_s);
    AOT_TEST_CHECK(pathBesideFile("C:\\a\\b\\main.js"_s, "../data/x.sqlite"_s) == "C:\\a\\data\\x.sqlite"_s);
    AOT_TEST_CHECK(pathBesideFile("C:\\main.js"_s, "../../x.sqlite"_s) == "C:\\x.sqlite"_s);

    AOT_TEST_CHECK(pathBesideFile("C:/a/main.js"_s, "./x.sqlite"_s) == "C:/a/x.sqlite"_s);
    AOT_TEST_CHECK(pathBesideFile("B:/~BUN/root/main.js"_s, "./data\\x.sqlite"_s) == "B:/~BUN/root/data/x.sqlite"_s);
    AOT_TEST_CHECK(pathBesideFile("B:/~BUN/root/main.js"_s, "../../../x.sqlite"_s) == "B:/x.sqlite"_s);

    AOT_TEST_CHECK(pathBesideFile("\\\\server\\share\\a\\main.js"_s, "../x.sqlite"_s) == "\\\\server\\share\\x.sqlite"_s);
    AOT_TEST_CHECK(pathBesideFile("\\\\server\\share\\main.js"_s, "../../x.sqlite"_s) == "\\\\server\\share\\x.sqlite"_s);
    AOT_TEST_CHECK(pathBesideFile("//server/share/main.js"_s, "../x.sqlite"_s) == "//server/share/x.sqlite"_s);
}

template<LoadedProgramTest body>
static constexpr void (*withModules)() = runNativeTestWithLoadedProgram<moduleTableTests, programText, body>;

std::span<const NativeTest> moduleTableTests()
{
    static constexpr NativeTest tests[] = {
        { "module-table/lists"_s, withModules<checkLists> },
        { "module-table/module-numbers-in-one-and-two-words"_s, testModuleNumbersInOneAndTwoWords },
        { "module-table/find-by-path"_s, withModules<checkFindByPath> },
        { "module-table/a-path-beside-a-file-keeps-the-root-and-the-separator-of-that-file"_s, testPathBesideFile },
        { "module-table/export-of-symbol"_s, withModules<checkFindExportOfSymbol> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
