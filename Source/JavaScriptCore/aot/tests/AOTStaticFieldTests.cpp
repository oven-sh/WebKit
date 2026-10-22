/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTImage.h"
#include "AOTImageLoading.h"
#include "AOTImageTestSupport.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTModules.h"
#include "AOTObjectModel.h"
#include "AOTStaticFields.h"
#include "JSCInlines.h"
#include "MarkedBlockCage.h"
#include "Options.h"
#include "PropertyNameArray.h"
#include "TopExceptionScope.h"
#include <array>
#include <optional>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/StringBuilder.h>

#if OS(WINDOWS)
#include <windows.h>
#else
#include <fcntl.h>
#endif

namespace JSC { namespace AOT {

std::span<const NativeTest> staticFieldTests();

static constexpr ASCIILiteral programText = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Counter" module=m0 constructor=f2 staticFields=[{key="tally" variable=g0}, {key="label" variable=g1}]
layout L1 kind=Instance name="Sub" module=m0 base=L0 constructor=f4 supertypes=[L0] flags=[HasHeritage]
variable g0 name="tally" module=m0 declared={type=number} owner=L0
variable g1 name="label" module=m0 declared={type=string} owner=L0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f5
    v4 = NewClosure function=f6
    v5 = NewClosure function=f7
    v6 = Call v1, -, v2, v3, v4, v5
    Return
}
function f1 name="define" results=[{type=anything}] {
  b0():
    v0 = NewClosure function=f2
    v1 = NewClass v0 layout=L0
    v2 = FinishClass v1 layout=L0
    v3 = CallDirect v2 function=f3
    Return v2
}
function f2 name="Counter" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f3 name="initializer" kind=Initializer flags=[HasThis] thisType={type=function} results=[{type=anything}] {
  b0(v0: function):
    v1 = Integer integer=1
    PropDefine v0, v1 key="tally"
    v3 = String string="one"
    PropDefine v0, v3 key="label"
    Return
}
function f4 name="Sub" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
  b0(v0: function, v1: anything):
    v2 = GetSuperBase v0
    v3 = SuperConstruct v2, v1 layout=L1
    Return v3
}
function f5 name="defineSub" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = ResolveHeritage v0
    v2 = NewClosure function=f4
    v3 = NewClass v2, v0, v1 layout=L1
    v4 = FinishClass v3 layout=L1
    Return v4
}
function f6 name="storeAndRead" parameters=[{type=anything}, {type=number}] results=[{type=anything}] {
  b0(v0: anything, v1: number):
    PropSet v0, v1 key="tally"
    v3 = PropGet v0 key="tally"
    v4 = PropGet v0 key="label"
    v5 = NewArray v3, v4 contents=anything holes=[]
    Return v5
}
function f7 name="mutate" parameters=[{type=anything}, {type=string}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: string, v2: anything):
    ElemSet v0, v1, v2
    v4 = ElemDelete v0, v1
    Return
}
)IR"_s;

static intptr_t openForMappingCode(const char* path)
{
#if OS(WINDOWS)
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    AOT_TEST_CHECK(file != INVALID_HANDLE_VALUE);
    return reinterpret_cast<intptr_t>(file);
#else
    int descriptor = open(path, O_RDONLY);
    AOT_TEST_CHECK(descriptor >= 0);
    return descriptor;
#endif
}

static void useImageWithCodeMappedFromFile(const char* path)
{
    static NeverDestroyed<ImageTests::AlignedBytes> image;
    std::optional<Vector<uint8_t>> contents = FileSystem::readEntireFile(String::fromUTF8(path));
    AOT_TEST_CHECK(contents);
    image.get() = ImageTests::AlignedBytes(contents->span());
    constexpr std::array<uint8_t, 16> identityOfLoadedPrograms { };
    AOT_TEST_CHECK(useImage(image->span(), ImageFile { openForMappingCode(path), 0 }, identityOfLoadedPrograms) == Wire::ImageVerdict::Accepted);
    AOT_TEST_CHECK(Image::current()->header().options.useWideReferences() || MarkedBlockCage::require());
}

namespace {

class RunningProgram {
    WTF_MAKE_NONCOPYABLE(RunningProgram);
public:
    explicit RunningProgram(LoadedProgram&);

    VM& vm() { return m_program.vm(); }
    JSGlobalObject* realm() { return m_program.realm(); }
    ClassTable& classes() { return m_program.state().objects->classes; }
    JSValue evaluate(ASCIILiteral source) { return m_program.realms().evaluate(realm(), source); }
    bool isTrue(ASCIILiteral source) { return evaluate(source).isTrue(); }
    Closure& classObject(ASCIILiteral expression);
    Identifier name(ASCIILiteral text) { return Identifier::fromString(vm(), text); }
    const FieldData& ownField(Closure&, ASCIILiteral name);
    std::optional<unsigned> attributesInStructure(JSObject&, ASCIILiteral name);
    String keysOf(JSObject&);

private:
    LoadedProgram& m_program;
};

} // anonymous namespace

RunningProgram::RunningProgram(LoadedProgram& program)
    : m_program(program)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm());
    evaluate("globalThis.publish = function (define, defineSub, storeAndRead) { globalThis.define = define; globalThis.defineSub = defineSub; globalThis.storeAndRead = storeAndRead }"_s);
    std::optional<ModuleID> module = findModule("/main.js"_s);
    AOT_TEST_CHECK(module);
    evaluateModuleSynchronously(realm(), *module);
    AOT_TEST_CHECK(!scope.exception() && modulesOf(realm())->status(*module) == ModuleStatus::Evaluated);
    evaluate("globalThis.First = define(); globalThis.Second = define()"_s);
}

Closure& RunningProgram::classObject(ASCIILiteral expression)
{
    JSValue value = evaluate(expression);
    AOT_TEST_CHECK_FOR_SOURCE(value.isCell() && isCompiledClosure(value.asCell()) && uncheckedDowncast<Closure>(value.asCell())->classData(), expression);
    return *uncheckedDowncast<Closure>(value.asCell());
}

const FieldData& RunningProgram::ownField(Closure& classObject, ASCIILiteral fieldName)
{
    const FieldData* field = StaticFields::findOwnField(classObject, name(fieldName).impl());
    AOT_TEST_CHECK_FOR_SOURCE(field, fieldName);
    return *field;
}

std::optional<unsigned> RunningProgram::attributesInStructure(JSObject& object, ASCIILiteral propertyName)
{
    unsigned attributes = 0;
    if (!isValidOffset(object.structure()->get(vm(), name(propertyName), attributes)))
        return std::nullopt;
    return attributes;
}

String RunningProgram::keysOf(JSObject& object)
{
    PropertyNameArrayBuilder names(vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object.methodTable()->getOwnPropertyNames(&object, realm(), names, DontEnumPropertiesMode::Include);
    StringBuilder keys;
    for (auto& key : names)
        keys.append(keys.isEmpty() ? ""_s : ","_s, key.string());
    return keys.toString();
}

static constexpr unsigned customValue = static_cast<unsigned>(PropertyAttribute::CustomValue);

static void checkFirstEvaluationOwnsTheVariables(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    Closure& first = program.classObject("First"_s);
    Closure& second = program.classObject("Second"_s);
    AOT_TEST_CHECK(&first != &second && first.classData() == second.classData() && first.classData()->staticFields().size() == 2);
    AOT_TEST_CHECK(StaticFields::ownsVariables(first) && !StaticFields::ownsVariables(second));

    for (ASCIILiteral name : { "tally"_s, "label"_s }) {
        AOT_TEST_CHECK(StaticFields::isInVariable(first, program.ownField(first, name)));
        AOT_TEST_CHECK(!StaticFields::findOwnField(second, program.name(name).impl()));
    }
    AOT_TEST_CHECK(!StaticFields::findOwnField(first, program.name("prototype"_s).impl()) && !StaticFields::findOwnField(first, program.name("absent"_s).impl()));
    AOT_TEST_CHECK(program.isTrue("First.tally === 1 && First.label === 'one' && Second.tally === 1 && Second.label === 'one'"_s));
}

static void checkOwnerHasItsOwnStructure(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    Closure& first = program.classObject("First"_s);
    Closure& second = program.classObject("Second"_s);
    Structure* structureOfSecond = second.structure();
    StaticFields::ensureProperties(program.vm(), first);
    StaticFields::ensureProperties(program.vm(), second);
    AOT_TEST_CHECK(second.structure() == structureOfSecond && first.structure() != second.structure());

    for (ASCIILiteral name : { "tally"_s, "label"_s }) {
        AOT_TEST_CHECK(program.attributesInStructure(first, name) == customValue);
        AOT_TEST_CHECK(program.attributesInStructure(second, name) == 0u);
    }
    AOT_TEST_CHECK(program.keysOf(first) == program.keysOf(second) && program.keysOf(first).endsWith(",tally,label"_s));

    Structure* structureOfFirst = first.structure();
    StaticFields::ensureProperties(program.vm(), first);
    AOT_TEST_CHECK(first.structure() == structureOfFirst);

    AOT_TEST_CHECK(program.isTrue("(d => d.value === 1 && d.writable && d.enumerable && d.configurable && !('get' in d))(Object.getOwnPropertyDescriptor(First, 'tally'))"_s));
    AOT_TEST_CHECK(program.isTrue("(d => d.value === 1 && d.writable && d.enumerable && d.configurable && !('get' in d))(Object.getOwnPropertyDescriptor(Second, 'tally'))"_s));
}

static void checkReadCreatesNoProperty(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    Closure& first = program.classObject("First"_s);
    Structure* structure = first.structure();
    AOT_TEST_CHECK(!program.attributesInStructure(first, "tally"_s));
    AOT_TEST_CHECK(program.isTrue("First.tally === 1 && First.label === 'one' && Object.hasOwn(First, 'tally') && !Object.hasOwn(First, 'absent')"_s));
    AOT_TEST_CHECK(first.structure() == structure && !program.attributesInStructure(first, "tally"_s));
}

static void checkGenericCodeReachesTheVariable(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    Closure& first = program.classObject("First"_s);
    const FieldData& tally = program.ownField(first, "tally"_s);
    auto variableHolds = [&](int32_t number) {
        return StaticFields::isInVariable(first, tally) && program.classes().readStaticField(tally) == jsNumber(number);
    };

    AOT_TEST_CHECK(variableHolds(1) && program.classes().tryStoreStaticField(program.vm(), tally, jsNumber(41)));
    AOT_TEST_CHECK(program.isTrue("First.tally === 41"_s));
    AOT_TEST_CHECK(program.isTrue("(First.tally = 42) === 42 && First.tally === 42"_s) && variableHolds(42));
    AOT_TEST_CHECK(program.isTrue("Object.defineProperty(First, 'tally', { value: 43 }) === First && First.tally === 43"_s) && variableHolds(43));
    AOT_TEST_CHECK(program.isTrue("Reflect.set(First, 'tally', 44) && First.tally === 44"_s) && variableHolds(44));
    AOT_TEST_CHECK(program.isTrue("storeAndRead(First, 45)[0] === 45 && First.tally === 45"_s) && variableHolds(45));

    StaticFields::ensureProperties(program.vm(), first);
    AOT_TEST_CHECK(program.isTrue("(First.tally = 46) === 46 && First.tally === 46"_s) && variableHolds(46));
    AOT_TEST_CHECK(program.attributesInStructure(first, "tally"_s) == customValue);

    AOT_TEST_CHECK(program.isTrue("(Second.tally = 7) === 7 && Second.tally === 7 && First.tally === 46"_s) && variableHolds(46));
    AOT_TEST_CHECK(program.isTrue("storeAndRead(Second, 8)[0] === 8 && Second.tally === 8 && First.tally === 46"_s) && variableHolds(46));
}

static void checkStoreOfAnotherTypeLeavesTheVariable(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    Closure& first = program.classObject("First"_s);
    const FieldData& tally = program.ownField(first, "tally"_s);
    const FieldData& label = program.ownField(first, "label"_s);
    AOT_TEST_CHECK(!program.classes().tryStoreStaticField(program.vm(), tally, jsNull()) && StaticFields::isInVariable(first, tally));

    AOT_TEST_CHECK(program.isTrue("(First.tally = 'text') === 'text' && First.tally === 'text'"_s));
    AOT_TEST_CHECK(!StaticFields::isInVariable(first, tally) && !program.classes().readStaticField(tally));
    AOT_TEST_CHECK(program.attributesInStructure(first, "tally"_s) == 0u && program.keysOf(first).endsWith(",tally,label"_s));
    AOT_TEST_CHECK(StaticFields::isInVariable(first, label) && program.attributesInStructure(first, "label"_s) == customValue);
    AOT_TEST_CHECK(program.isTrue("storeAndRead(First, 5)[0] === 5 && First.tally === 5 && First.label === 'one'"_s));
}

static void checkDeletedField(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    Closure& first = program.classObject("First"_s);
    const FieldData& tally = program.ownField(first, "tally"_s);
    const FieldData& label = program.ownField(first, "label"_s);

    AOT_TEST_CHECK(program.isTrue("delete First.tally && !('tally' in First) && First.tally === undefined"_s));
    AOT_TEST_CHECK(!StaticFields::isInVariable(first, tally) && !program.classes().readStaticField(tally) && !program.attributesInStructure(first, "tally"_s));
    AOT_TEST_CHECK(program.keysOf(first).endsWith(",label"_s) && !program.keysOf(first).contains("tally"_s));
    AOT_TEST_CHECK(StaticFields::isInVariable(first, label) && program.isTrue("First.label === 'one' && Second.tally === 1"_s));
}

static void checkRedefinedField(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    Closure& first = program.classObject("First"_s);
    const FieldData& tally = program.ownField(first, "tally"_s);

    AOT_TEST_CHECK(program.isTrue("Object.defineProperty(First, 'tally', { enumerable: false }) === First && First.tally === 1"_s));
    AOT_TEST_CHECK(!StaticFields::isInVariable(first, tally));
    AOT_TEST_CHECK(program.attributesInStructure(first, "tally"_s) == static_cast<unsigned>(PropertyAttribute::DontEnum) && program.keysOf(first).endsWith(",tally,label"_s));

    AOT_TEST_CHECK(program.isTrue("Object.defineProperty(First, 'label', { get() { return this } }) === First && First.label === First"_s));
    AOT_TEST_CHECK(!StaticFields::isInVariable(first, program.ownField(first, "label"_s)) && program.keysOf(first).endsWith(",tally,label"_s));
}

static void checkAddedNameComesAfterTheFields(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    Closure& first = program.classObject("First"_s);
    AOT_TEST_CHECK(!program.attributesInStructure(first, "tally"_s));
    AOT_TEST_CHECK(program.isTrue("(First.extra = 1) === 1"_s));
    AOT_TEST_CHECK(program.keysOf(first).endsWith(",tally,label,extra"_s));
    AOT_TEST_CHECK(StaticFields::isInVariable(first, program.ownField(first, "tally"_s)) && program.attributesInStructure(first, "tally"_s) == customValue);
    AOT_TEST_CHECK(program.attributesInStructure(first, "extra"_s) == 0u);
}

static void checkSubclassReadsAndShadows(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    Closure& first = program.classObject("First"_s);
    const FieldData& tally = program.ownField(first, "tally"_s);
    program.evaluate("globalThis.Sub = defineSub(First)"_s);
    Closure& sub = program.classObject("Sub"_s);
    AOT_TEST_CHECK(!StaticFields::findOwnField(sub, program.name("tally"_s).impl()));
    AOT_TEST_CHECK(program.isTrue("Object.getPrototypeOf(Sub) === First && Sub.tally === 1 && !Object.hasOwn(Sub, 'tally')"_s));

    AOT_TEST_CHECK(program.isTrue("(First.tally = 2) === 2 && Sub.tally === 2"_s));
    AOT_TEST_CHECK(program.isTrue("(Sub.tally = 5) === 5 && Object.hasOwn(Sub, 'tally') && Sub.tally === 5 && First.tally === 2"_s));
    AOT_TEST_CHECK(StaticFields::isInVariable(first, tally) && program.classes().readStaticField(tally) == jsNumber(2));
}

static void checkFieldForCache(LoadedProgram& loaded)
{
    RunningProgram program(loaded);
    VM& vm = program.vm();
    Closure& first = program.classObject("First"_s);
    Closure& second = program.classObject("Second"_s);
    UniquedStringImpl* tally = program.name("tally"_s).impl();
    AOT_TEST_CHECK(!program.attributesInStructure(first, "tally"_s));
    AOT_TEST_CHECK(StaticFields::findFieldForCache(vm, &first, tally) == &program.ownField(first, "tally"_s));
    AOT_TEST_CHECK(program.attributesInStructure(first, "tally"_s) == customValue);

    AOT_TEST_CHECK(!StaticFields::findFieldForCache(vm, &second, tally));
    AOT_TEST_CHECK(!StaticFields::findFieldForCache(vm, &first, program.name("absent"_s).impl()));
    AOT_TEST_CHECK(!StaticFields::findFieldForCache(vm, program.realm(), tally));
    AOT_TEST_CHECK(!StaticFields::findFieldForCache(vm, program.evaluate("define"_s).asCell(), tally));

    program.evaluate("First.tally = 'text'"_s);
    AOT_TEST_CHECK(!StaticFields::findFieldForCache(vm, &first, tally));
}

template<LoadedProgramTest body>
static void runNativeTestWithCompiledCode()
{
    for (const NativeTest& test : staticFieldTests()) {
        if (test.run != &runNativeTestWithCompiledCode<body>)
            continue;
        if (const char* imagePath = byteCast<char>(Options::aotImagePath()))
            useImageWithCodeMappedFromFile(imagePath);
        runWithLoadedProgram(test.name, programText, body);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::span<const NativeTest> staticFieldTests()
{
    static constexpr NativeTest tests[] = {
        { "static-fields/first-evaluation-owns-the-variables"_s, runNativeTestWithCompiledCode<checkFirstEvaluationOwnsTheVariables> },
        { "static-fields/owner-has-its-own-structure"_s, runNativeTestWithCompiledCode<checkOwnerHasItsOwnStructure> },
        { "static-fields/read-creates-no-property"_s, runNativeTestWithCompiledCode<checkReadCreatesNoProperty> },
        { "static-fields/generic-code-reaches-the-variable"_s, runNativeTestWithCompiledCode<checkGenericCodeReachesTheVariable> },
        { "static-fields/store-of-another-type-leaves-the-variable"_s, runNativeTestWithCompiledCode<checkStoreOfAnotherTypeLeavesTheVariable> },
        { "static-fields/deleted-field"_s, runNativeTestWithCompiledCode<checkDeletedField> },
        { "static-fields/redefined-field"_s, runNativeTestWithCompiledCode<checkRedefinedField> },
        { "static-fields/added-name-comes-after-the-fields"_s, runNativeTestWithCompiledCode<checkAddedNameComesAfterTheFields> },
        { "static-fields/subclass-reads-and-shadows"_s, runNativeTestWithCompiledCode<checkSubclassReadsAndShadows> },
        { "static-fields/field-for-cache"_s, runNativeTestWithCompiledCode<checkFieldForCache> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
