/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCollectionTestSupport.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTModules.h"
#include "AOTObjectModel.h"
#include "AOTProgramFile.h"
#include "AOTPropertyCaches.h"
#include "AOTRegExpRecords.h"
#include "AOTRuntimeType.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "AOTErrorMessages.h"
#include "AOTVector.h"
#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include <array>
#include <bit>
#include <initializer_list>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> sharingModeAndNamespaceTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Record" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
variable g0 name="a" module=m1 declared={type=anything} initialization=Undefined
variable g1 name="a" module=m2 declared={type=anything} initialization=Undefined
variable g2 name="chosen" module=m0 declared={type=anything} initialization=Undefined
module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1 completes=true}, {target=m2 completes=true}, {target=m1 phase=Defer}]
module m1 path="/first.js" displayPath="first.js" body=f1 exports=[{name="a" variable=g0}]
module m2 path="/second.js" displayPath="second.js" body=f2 exports=[{name="a" variable=g1}]
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="registry" [isFreeName]
    v2 = NewClosure function=f3
    PropSet v1, v2 key="isUnshared"
    v4 = NewClosure function=f4
    PropSet v1, v4 key="isShared"
    v6 = NewClosure function=f5
    PropSet v1, v6 key="isEither"
    v8 = NewClosure function=f6
    PropSet v1, v8 key="choose"
    v10 = NewClosure function=f7
    PropSet v1, v10 key="read"
    v12 = ModuleNamespace module=m1 phase=Defer
    PropSet v1, v12 key="deferred"
    Return
}
function f1 name="first" module=m1 kind=Module results=[{type=anything}] {
    v0 = String string="export of first"
    PutModuleVariable v0 variable=g0
    Return
}
function f2 name="second" module=m2 kind=Module results=[{type=anything}] {
    v0 = String string="export of second"
    PutModuleVariable v0 variable=g1
    Return
}
function f3 name="isUnshared" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = TypeTest v0 testedType=ArrayBuffer
    Return v1
}
function f4 name="isShared" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = TypeTest v0 testedType=SharedArrayBuffer
    Return v1
}
function f5 name="isEither" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = TypeTest v0 testedType=ArrayBuffer|SharedArrayBuffer
    Return v1
}
function f6 name="choose" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Integer integer=1
    v2 = StrictEq v0, v1
    Branch v2 -> taken b1(), notTaken b2()
  b1():
    v3 = ModuleNamespace module=m1
    PutModuleVariable v3 variable=g2
    Return
  b2():
    v5 = Integer integer=2
    v6 = StrictEq v0, v5
    Branch v6 -> taken b3(), notTaken b4()
  b3():
    v7 = ModuleNamespace module=m2
    PutModuleVariable v7 variable=g2
    Return
  b4():
    v9 = String string="field of a record"
    v10 = NewObject -, v9 layout=L0 fields=[0]
    PutModuleVariable v10 variable=g2
    Return
}
function f7 name="read" results=[{type=anything}] {
    v0 = GetModuleVariable variable=g2
    v1 = PropGet v0 key="a"
    Return v1
}
)IR"_s;

static constexpr ASCIILiteral programWithLiterals = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="registry" [isFreeName]
    v2 = NewClosure function=f1
    PropSet v1, v2 key="literals"
    Return
}
function f1 name="literals" results=[{type=anything}] {
    v0 = NewRegExp pattern="a+" regExpFlags=[]
    v1 = NewRegExp pattern="b+" regExpFlags=[]
    v2 = NewRegExp pattern="c+" regExpFlags=[]
    PropSet v0, v1 key="next"
    PropSet v1, v2 key="next"
    Return v0
}
)IR"_s;

static constexpr ModuleID firstModule(1u);
static constexpr ModuleID secondModule(2u);
static constexpr auto unshared = ArrayBufferSharingMode::Default;
static constexpr auto shared = ArrayBufferSharingMode::Shared;

namespace {

class TypeRecord {
public:
    TypeRecord(std::initializer_list<RuntimeCellTest> tests)
    {
        m_words.append(0);
        m_words.append(static_cast<uint32_t>(tests.size()));
        for (const RuntimeCellTest& test : tests) {
            auto words = std::bit_cast<std::array<uint32_t, 2>>(test);
            m_words.append(words[0]);
            m_words.append(words[1]);
        }
        AOT_TEST_CHECK(m_words.size() * sizeof(uint32_t) == RuntimeType::sizeInBytes(tests.size(), 0, 0));
    }

    operator const RuntimeType&() const { return *reinterpret_cast<const RuntimeType*>(m_words.span().data()); }

private:
    Vector<uint32_t, 8> m_words;
};

} // anonymous namespace

static JSValue evaluate(LoadedProgram& program, ASCIILiteral source)
{
    return program.realms().evaluate(program.realms().plain(), source);
}

static bool hasType(LoadedProgram& program, const RuntimeType& type, JSValue value)
{
    return program.state().objects->layouts.valueHasType(type, value);
}

static JSArrayBuffer* createBuffer(LoadedProgram& program, ArrayBufferSharingMode sharingMode, ArrayBufferSharingMode sharingModeOfStructure)
{
    RefPtr<ArrayBuffer> buffer = ArrayBuffer::tryCreate(8, 1);
    AOT_TEST_CHECK(buffer);
    buffer->setSharingMode(sharingMode);
    return JSArrayBuffer::create(program.vm(), program.realm()->arrayBufferStructure(sharingModeOfStructure), WTF::move(buffer));
}

static void testSharingModeOfValues(LoadedProgram& program)
{
    TypeRecord unsharedBuffers { RuntimeCellTest::sharingModeIs(unshared) };
    TypeRecord sharedBuffers { RuntimeCellTest::sharingModeIs(shared) };

    // Reflect.construct(SharedArrayBuffer, [8], ArrayBuffer) makes a shared buffer with the Structure of the other class.
    for (JSValue buffer : { JSValue(createBuffer(program, shared, shared)), JSValue(createBuffer(program, shared, unshared)) })
        AOT_TEST_CHECK(hasType(program, sharedBuffers, buffer) && !hasType(program, unsharedBuffers, buffer));

    JSArrayBuffer* detached = createBuffer(program, unshared, unshared);
    detached->impl()->detach(program.vm());
    AOT_TEST_CHECK(detached->impl()->isDetached());
    for (JSValue buffer : { JSValue(createBuffer(program, unshared, unshared)), JSValue(createBuffer(program, unshared, shared)), JSValue(detached), evaluate(program, "new ArrayBuffer(8)"_s), evaluate(program, "new (class extends ArrayBuffer { })(8)"_s) })
        AOT_TEST_CHECK(hasType(program, unsharedBuffers, buffer) && !hasType(program, sharedBuffers, buffer));

    for (JSValue other : { evaluate(program, "new Uint8Array(8)"_s), evaluate(program, "new DataView(new ArrayBuffer(8))"_s), evaluate(program, "Object.create(ArrayBuffer.prototype)"_s), evaluate(program, "ArrayBuffer"_s), evaluate(program, "'ArrayBuffer'"_s) })
        AOT_TEST_CHECK(!hasType(program, unsharedBuffers, other) && !hasType(program, sharedBuffers, other));
}

static void testModuleOfNamespaceObjects(LoadedProgram& program)
{
    using ModulePhase = AbstractModuleRecord::ModulePhase;
    Modules& modules = modulesOf(program.instance());
    JSObject* first = modules.moduleNamespace(firstModule, ModulePhase::Evaluation);
    JSObject* second = modules.moduleNamespace(secondModule, ModulePhase::Evaluation);
    JSObject* deferredFirst = modules.moduleNamespace(firstModule, ModulePhase::Defer);
    AOT_TEST_CHECK(first && second && deferredFirst && first != deferredFirst);

    TypeRecord namespacesOfFirst { RuntimeCellTest::isNamespaceOf(firstModule) };
    TypeRecord namespacesOfSecond { RuntimeCellTest::isNamespaceOf(secondModule) };
    AOT_TEST_CHECK(hasType(program, namespacesOfFirst, first) && !hasType(program, namespacesOfFirst, second));
    AOT_TEST_CHECK(hasType(program, namespacesOfSecond, second) && !hasType(program, namespacesOfSecond, first));
    AOT_TEST_CHECK(!hasType(program, namespacesOfFirst, deferredFirst));
    for (JSValue other : { evaluate(program, "({ a: 1 })"_s), evaluate(program, "Object.create(null)"_s), evaluate(program, "[]"_s), evaluate(program, "'first.js'"_s) })
        AOT_TEST_CHECK(!hasType(program, namespacesOfFirst, other));

    AOT_TEST_CHECK(operationAOTIsNamespaceOfModule(first, firstModule.index()) && !operationAOTIsNamespaceOfModule(first, secondModule.index()));
    AOT_TEST_CHECK(!operationAOTIsNamespaceOfModule(deferredFirst, firstModule.index()));
    AOT_TEST_CHECK(!operationAOTIsNamespaceOfModule(evaluate(program, "({ a: 1 })"_s).asCell(), firstModule.index()));
}

// The collector reads the stack for what looks like a reference, so the tests below keep the cells they ask about in another form and off the stack.
using HiddenCells = std::array<uintptr_t, 3>;

static NEVER_INLINE uintptr_t hide(JSCell* cell)
{
    return ~std::bit_cast<uintptr_t>(cell);
}

static NEVER_INLINE unsigned numberOfMarkedCells(VM& vm, const HiddenCells& hidden, unsigned count)
{
    unsigned marked = 0;
    for (unsigned i = 0; i < count; ++i)
        marked += hasSurvivedCollection(vm, std::bit_cast<JSCell*>(~hidden[i]));
    return marked;
}

static void checkCellsMadeBetweenCollectionsLast(LoadedProgram& program, uintptr_t (*makeOrFind)(LoadedProgram&, unsigned index))
{
    VM& vm = program.vm();
    auto hiddenCell = [&](unsigned index) {
        return makeOrFind(program, index);
    };
    auto hiddenOffTheStack = makeUniqueWithoutFastMallocCheck<HiddenCells>();
    HiddenCells& hidden = *hiddenOffTheStack;
    auto collect = [&](CollectionScope scope) {
        vm.heap.collectNow(Sync, scope);
    };
    // After this the roots have been visited and nothing has been stored since.
    collect(CollectionScope::Full);
    collect(CollectionScope::Eden);

    hidden[0] = hiddenCell(0);
    collect(CollectionScope::Eden);
    AOT_TEST_CHECK(numberOfMarkedCells(vm, hidden, 1) == 1);
    hidden[1] = hiddenCell(1);
    collect(CollectionScope::Eden);
    AOT_TEST_CHECK(numberOfMarkedCells(vm, hidden, 2) == 2);
    hidden[2] = hiddenCell(2);
    collect(CollectionScope::Full);
    AOT_TEST_CHECK(numberOfMarkedCells(vm, hidden, 3) == 3);
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full, CollectionScope::Full, CollectionScope::Eden }) {
        collect(scope);
        AOT_TEST_CHECK(numberOfMarkedCells(vm, hidden, 3) == 3);
    }
    for (unsigned i = 0; i < hidden.size(); ++i)
        AOT_TEST_CHECK(hiddenCell(i) == hidden[i]);
}

// Nothing but the list of loaded modules holds a namespace object between two requests for it.
static NEVER_INLINE uintptr_t hiddenNamespaceObject(LoadedProgram& program, unsigned index)
{
    using ModulePhase = AbstractModuleRecord::ModulePhase;
    Modules& modules = modulesOf(program.instance());
    switch (index) {
    case 0:
        return hide(modules.moduleNamespace(firstModule, ModulePhase::Evaluation));
    case 1:
        return hide(modules.moduleNamespace(secondModule, ModulePhase::Evaluation));
    default:
        return hide(modules.moduleNamespace(firstModule, ModulePhase::Defer));
    }
}

static void testNamespaceObjectsMadeBetweenCollectionsLast(LoadedProgram& program)
{
    checkCellsMadeBetweenCollectionsLast(program, hiddenNamespaceObject);
}

// Nor anything but the set of literals the compiled pattern of a literal between two evaluations.
static NEVER_INLINE uintptr_t hiddenLiteral(LoadedProgram& program, unsigned index)
{
    VM& vm = program.vm();
    return hide(vm.aotState().regExpLiterals->regExpFor(vm, RegExpRecordID(index), { }));
}

static void testLiteralsMadeBetweenCollectionsLast(LoadedProgram& program)
{
    checkCellsMadeBetweenCollectionsLast(program, hiddenLiteral);
}

static NEVER_INLINE void addTwoPropertiesInARow(LoadedProgram& program, HiddenCells& hidden)
{
    VM& vm = program.vm();
    PropertyOffset offset;
    Structure* old = program.realm()->objectStructureForObjectConstructor();
    Structure* first = Structure::addPropertyTransition(vm, old, Identifier::fromString(vm, "firstAddition"_s), 0, offset);
    Structure* second = Structure::addPropertyTransition(vm, first, Identifier::fromString(vm, "secondAddition"_s), 0, offset);
    PropertyCaches& caches = *program.state().propertyCaches;
    caches.didAddProperty(first, second);
    caches.didAddProperty(old, first);
    hidden[0] = hide(first);
    hidden[1] = hide(second);
}

// A collection takes every word of the stack above it for a reference. What a function that has returned left there lies under the frames of the collection, which do not write every word of theirs,
// and which frames those are depends on which thread collects.
static NEVER_INLINE void overwriteFramesOfFunctionsThatReturned()
{
    volatile uint8_t bytes[16 * KB];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = 0;
}

static void testAdditionThatWaitsForItsFirstStructure(LoadedProgram& program)
{
    if (framesAreThoseOfAddressSanitizer)
        return;
    VM& vm = program.vm();
    auto hiddenOffTheStack = makeUniqueWithoutFastMallocCheck<HiddenCells>();
    vm.heap.collectNow(Sync, CollectionScope::Full);
    addTwoPropertiesInARow(program, *hiddenOffTheStack);
    overwriteFramesOfFunctionsThatReturned();
    vm.heap.collectNow(Sync, CollectionScope::Eden);
    AOT_TEST_CHECK(numberOfMarkedCells(vm, *hiddenOffTheStack, 2) == 2);
    // A full collection ends every Structure that no object has. The young collection may have run on this thread, with both in its hands.
    overwriteFramesOfFunctionsThatReturned();
    vm.heap.collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(!numberOfMarkedCells(vm, *hiddenOffTheStack, 2));
}

static void testNoShapePasses(LoadedProgram& program)
{
    const RuntimeLayoutTable& layouts = program.state().objects->layouts;
    constexpr uint32_t firstShape = 1;
    AOT_TEST_CHECK(layouts.shapeHasType(TypeRecord { RuntimeCellTest::isObject() }, firstShape));
    for (RuntimeCellTest test : { RuntimeCellTest::sharingModeIs(unshared), RuntimeCellTest::sharingModeIs(shared), RuntimeCellTest::isNamespaceOf(firstModule) })
        AOT_TEST_CHECK(!layouts.shapeHasType(TypeRecord { test }, firstShape));
}

static bool isDescribedAs(LoadedProgram& program, const RuntimeType& type, StringView expected)
{
    String description = describe(program.state().tables, type);
    if (description == expected)
        return true;
    dataLogLn("described as ", description, ", expected ", expected);
    return false;
}

static bool hasMessage(LoadedProgram& program, JSValue value, const RuntimeType& type, StringView expected)
{
    String message = makeString("expected "_s, describe(program.state().tables, type), ", got "_s, describe(value));
    if (message == expected)
        return true;
    dataLogLn("the message is ", message, ", expected ", expected);
    return false;
}

static void testMessages(LoadedProgram& program)
{
    TypeRecord unsharedBuffers { RuntimeCellTest::sharingModeIs(unshared) };
    TypeRecord sharedBuffers { RuntimeCellTest::sharingModeIs(shared) };
    AOT_TEST_CHECK(isDescribedAs(program, unsharedBuffers, "ArrayBuffer"_s));
    AOT_TEST_CHECK(isDescribedAs(program, sharedBuffers, "SharedArrayBuffer"_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord { RuntimeCellTest::cellTypeIs(ArrayBufferType, ArrayBufferType) }, "ArrayBuffer | SharedArrayBuffer"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "new ArrayBuffer(1)"_s), sharedBuffers, "expected SharedArrayBuffer, got built-in ArrayBuffer"_s));
    AOT_TEST_CHECK(hasMessage(program, evaluate(program, "new Uint8Array(1)"_s), unsharedBuffers, "expected ArrayBuffer, got built-in Uint8Array"_s));

    TypeRecord namespacesOfFirst { RuntimeCellTest::isNamespaceOf(firstModule) };
    AOT_TEST_CHECK(isDescribedAs(program, namespacesOfFirst, "typeof import(\"first.js\")"_s));
    AOT_TEST_CHECK(isDescribedAs(program, TypeRecord { RuntimeCellTest::isNamespaceOf(firstModule), RuntimeCellTest::isNamespaceOf(secondModule) }, "typeof import(\"first.js\") | typeof import(\"second.js\")"_s));
    AOT_TEST_CHECK(hasMessage(program, jsNumber(3), namespacesOfFirst, "expected typeof import(\"first.js\"), got number"_s));
}

static constexpr ASCIILiteral scriptStart = R"js(
globalThis.registry = { };
function describe(value)
{
    return registry.isUnshared(value) + " " + registry.isShared(value) + " " + registry.isEither(value);
}
)js"_s;

static constexpr ASCIILiteral statements = R"js(
    print("ArrayBuffer: " + describe(new ArrayBuffer(8)));
    print("SharedArrayBuffer: " + describe(new SharedArrayBuffer(8)));
    print("shared with the prototype of ArrayBuffer: " + describe(Reflect.construct(SharedArrayBuffer, [8], ArrayBuffer)));
    print("unshared with the prototype of SharedArrayBuffer: " + describe(Reflect.construct(ArrayBuffer, [8], SharedArrayBuffer)));
    print("subclass of ArrayBuffer: " + describe(new (class extends ArrayBuffer { })(8)));
    print("detached: " + describe((buffer => (buffer.transfer(), buffer))(new ArrayBuffer(8))));
    print("Uint8Array: " + describe(new Uint8Array(8)));
    print("object with the prototype of ArrayBuffer: " + describe(Object.create(ArrayBuffer.prototype)));
    print("number: " + describe(8));
    print("string: " + describe("ArrayBuffer"));
    print("undefined: " + describe(undefined));
    for (let choice of [1, 2, 3, 1]) {
        registry.choose(choice);
        print("choice " + choice + ": " + registry.read());
    }
)js"_s;

static constexpr ASCIILiteral expectedOutput = R"(ArrayBuffer: true false true
SharedArrayBuffer: false true true
shared with the prototype of ArrayBuffer: false true true
unshared with the prototype of SharedArrayBuffer: true false true
subclass of ArrayBuffer: true false true
detached: true false true
Uint8Array: false false false
object with the prototype of ArrayBuffer: false false false
number: false false false
string: false false false
undefined: false false false
choice 1: export of first
choice 2: export of second
choice 3: field of a record
choice 1: export of first
)"_s;

static std::unique_ptr<CompileOutput> compileOnThisThread(std::initializer_list<ASCIILiteral> optionWords)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), Typed::IRForm::AsRead, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(program);

    std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
    if (output->image.isEmpty())
        Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
    AOT_TEST_CHECK(!output->image.isEmpty());
    AOT_TEST_CHECK(output->entryModuleIDs.size() == 1);
    return output;
}

static String runInShell(std::initializer_list<ASCIILiteral> optionWords)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileOnThisThread(optionWords);
    });

    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments {
        String("--useSharedArrayBuffer=1"_s),
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(scriptStart, "runCompiledEntryPoint("_s, output->entryModuleIDs[0], ").then(() => {"_s, statements, "}).catch(error => print(\"the script threw \" + error));"_s),
    };

    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

static void checkCompiledTests(std::initializer_list<ASCIILiteral> optionWords)
{
    String output = runInShell(optionWords);
    if (output != expectedOutput)
        dataLog(output);
    AOT_TEST_CHECK(output == expectedOutput);
}

static void testCompiledTests()
{
    checkCompiledTests({ });
    checkCompiledTests({ "references=narrow"_s });
}

static void testCompiledTestsUnderValidation()
{
    checkCompiledTests({ "validate"_s });
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<sharingModeAndNamespaceTests, programText, body>;

std::span<const NativeTest> sharingModeAndNamespaceTests()
{
    static constexpr NativeTest tests[] = {
        { "sharing-mode-and-namespace/sharing-mode-of-values"_s, withProgram<testSharingModeOfValues> },
        { "sharing-mode-and-namespace/module-of-namespace-objects"_s, withProgram<testModuleOfNamespaceObjects> },
        { "sharing-mode-and-namespace/namespace-objects-made-between-collections-last"_s, withProgram<testNamespaceObjectsMadeBetweenCollectionsLast> },
        { "sharing-mode-and-namespace/literals-made-between-collections-last"_s, runNativeTestWithLoadedProgram<sharingModeAndNamespaceTests, programWithLiterals, testLiteralsMadeBetweenCollectionsLast> },
        { "sharing-mode-and-namespace/addition-that-waits-for-its-first-structure"_s, withProgram<testAdditionThatWaitsForItsFirstStructure> },
        { "sharing-mode-and-namespace/no-shape-passes"_s, withProgram<testNoShapePasses> },
        { "sharing-mode-and-namespace/messages"_s, withProgram<testMessages> },
        { "sharing-mode-and-namespace/compiled-tests"_s, testCompiledTests },
        { "sharing-mode-and-namespace/compiled-tests-under-validation"_s, testCompiledTestsUnderValidation },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
