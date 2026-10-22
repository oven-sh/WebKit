/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTChildProcess.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTImage.h"
#include "AOTImageLoading.h"
#include "AOTImageTestSupport.h"
#include "AOTInstance.h"
#include "AOTModules.h"
#include "AOTNativeTest.h"
#include "AOTObject.h"
#include "AOTProgramFile.h"
#include "AOTRealmTestSupport.h"
#include "AOTTemporaryFile.h"
#include "AOTTestSubcommands.h"
#include "AOTThreads.h"
#include "CustomGetterSetter.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "MarkedBlockCage.h"
#include "ObjectConstructor.h"
#include "Options.h"
#include "StrongInlines.h"
#include "TopExceptionScope.h"
#include <optional>
#include <wtf/DataLog.h>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

#if OS(WINDOWS)
#include <windows.h>
#else
#include <fcntl.h>
#endif

namespace JSC { namespace AOT {

static constexpr ASCIILiteral compiledObjectsTest = "native-stores/compiled-objects"_s;

static constexpr ASCIILiteral programText =
    "layout L0 name=\"Point\" module=m0 fields=[{key=\"x\" declared={type=anything} flags=[IsRequired]}]\n"
    "module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = NewClosure function=f1\n"
    "    v1 = Builtin builtin=GlobalThis\n"
    "    v2 = PropGet v1 key=\"registry\" [isFreeName]\n"
    "    PropSet v2, v0 key=\"createPoint\"\n"
    "    v4 = NewClosure function=f2\n"
    "    PropSet v2, v4 key=\"mutate\"\n"
    "    Return\n"
    "}\n"
    "function f1 name=\"createPoint\" results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Null\n"
    "    v1 = NewObject -, v0 layout=L0 fields=[0]\n"
    "    Return v1\n"
    "}\n"
    "function f2 name=\"mutate\" length=3 parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {\n"
    "  b0(v0: anything, v1: anything, v2: anything):\n"
    "    PropSet v0, v2 key=\"x\"\n"
    "    ElemSet v0, v1, v2\n"
    "    v5 = PropDelete v0 key=\"x\"\n"
    "    v6 = Builtin builtin=ObjectFreeze\n"
    "    v7 = Call v6, -, v0\n"
    "    v8 = Builtin builtin=ObjectDefineProperty\n"
    "    v9 = Call v8, -, v0, v1, v2\n"
    "    v10 = Builtin builtin=ObjectSetPrototypeOf\n"
    "    v11 = Call v10, -, v0, v2\n"
    "    Return\n"
    "}\n"_s;

static constexpr unsigned readOnly = static_cast<unsigned>(PropertyAttribute::ReadOnly);
static constexpr unsigned dontEnum = static_cast<unsigned>(PropertyAttribute::DontEnum);
static constexpr unsigned dontDelete = static_cast<unsigned>(PropertyAttribute::DontDelete);

static constexpr unsigned accessorAttribute = static_cast<unsigned>(PropertyAttribute::Accessor);

namespace {

class RunningProgram {
    WTF_MAKE_NONCOPYABLE(RunningProgram);
public:
    RunningProgram();

    VM& vm() { return m_realms.vm(); }
    JSGlobalObject* realm() { return m_realms.locked(); }
    const Identifier& x() const { return m_x; }
    const Identifier& added() const { return m_added; }

    JSObject* createPoint();

private:
    Realms m_realms;
    Strong<JSObject> m_createPoint;
    Identifier m_x;
    Identifier m_added;
};

} // anonymous namespace

static JSC_DECLARE_CUSTOM_GETTER(getSeven);

JSC_DEFINE_CUSTOM_GETTER(getSeven, (JSGlobalObject*, EncodedJSValue, PropertyName))
{
    return JSValue::encode(jsNumber(7));
}

static Vector<uint8_t> compileOnThisThread()
{
    Wire::CompileOptions options = ImageTests::wireOptions();
    options.target.referenceWidth = Wire::ReferenceWidth::Narrow;
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
    return WTF::move(output->image);
}

static Vector<uint8_t> compileWithNarrowReferences()
{
    Vector<uint8_t> image;
    runOnBuildThread([&] {
        image = compileOnThisThread();
    });
    return image;
}

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

static void useImageAt(const char* path)
{
    static NeverDestroyed<ImageTests::AlignedBytes> image;
    std::optional<Vector<uint8_t>> contents = FileSystem::readEntireFile(String::fromUTF8(path));
    AOT_TEST_CHECK(contents);
    image.get() = ImageTests::AlignedBytes(contents->span());
    Wire::ImageVerdict verdict = useImage(image->span(), ImageFile { openForMappingCode(path), 0 }, ImageTests::testIdentity);
    if (verdict != Wire::ImageVerdict::Accepted)
        dataLogLn("Cannot use ", path, ": ", nameOf(verdict));
    AOT_TEST_CHECK(verdict == Wire::ImageVerdict::Accepted);
    AOT_TEST_CHECK(!Image::current()->header().options.useWideReferences());
    AOT_TEST_CHECK(MarkedBlockCage::require());
}

RunningProgram::RunningProgram()
{
    VM& vm = this->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    AOT_TEST_CHECK(vm.hasCompiledCode() && Instance::of(realm()));

    JSObject* registry = constructEmptyObject(realm());
    realm()->putDirect(vm, Identifier::fromString(vm, "registry"_s), registry);
    std::optional<ModuleID> module = findModule("main.js"_s);
    AOT_TEST_CHECK(module);
    evaluateModuleSynchronously(realm(), *module);
    AOT_TEST_CHECK(!scope.exception() && modulesOf(realm())->status(*module) == ModuleStatus::Evaluated);

    JSValue createPoint = registry->getDirect(vm, Identifier::fromString(vm, "createPoint"_s));
    AOT_TEST_CHECK(createPoint && createPoint.isCallable());
    m_createPoint.set(vm, asObject(createPoint));
    m_x = Identifier::fromString(vm, "x"_s);
    m_added = Identifier::fromString(vm, "added"_s);
}

JSObject* RunningProgram::createPoint()
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm());
    JSValue point = call(realm(), m_createPoint.get(), ArgList(), "createPoint is not a function"_s);
    AOT_TEST_CHECK(!scope.exception() && point.isObject());
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(asObject(point)));
    return asObject(point);
}

static bool hasOwnProperty(VM& vm, JSObject* object, const Identifier& name, JSValue value, unsigned attributes)
{
    unsigned currentAttributes = 0;
    return object->getDirect(vm, name, currentAttributes) == value && currentAttributes == attributes;
}

static bool isFieldInSlot(JSObject* compiledObject, const Identifier& name)
{
    Object* object = uncheckedDowncast<Object>(compiledObject);
    const FieldData* field = object->fieldNamed(object->state(), name);
    AOT_TEST_CHECK(field);
    return object->isInSlot(*field);
}

static bool putOwnDataProperty(VM& vm, JSObject* compiledObject, const Identifier& name, JSValue value)
{
    PutPropertySlot slot(compiledObject);
    bool isStored = compiledObject->putOwnDataProperty(vm, name, value, slot);
    AOT_TEST_CHECK(!slot.isCacheablePut());
    return isStored;
}

static void testStoreToField(RunningProgram& program)
{
    VM& vm = program.vm();
    JSObject* point = program.createPoint();
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.x(), jsNull(), 0));

    AOT_TEST_CHECK(putOwnDataProperty(vm, point, program.x(), jsNumber(7)));
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.x(), jsNumber(7), 0));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(point));
}

static void testStoreToFieldOfSealedObject(RunningProgram& program)
{
    VM& vm = program.vm();
    JSObject* point = program.createPoint();
    point->seal(vm);
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.x(), jsNull(), dontDelete));

    AOT_TEST_CHECK(putOwnDataProperty(vm, point, program.x(), jsNumber(7)));
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.x(), jsNumber(7), dontDelete));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(point));
}

static void testStoreToFieldOfFrozenObject(RunningProgram& program)
{
    VM& vm = program.vm();
    JSObject* point = program.createPoint();
    point->freeze(vm);
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.x(), jsNull(), readOnly | dontDelete));

    AOT_TEST_CHECK(!putOwnDataProperty(vm, point, program.x(), jsNumber(7)));
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.x(), jsNull(), readOnly | dontDelete));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(point));
}

static void testStoreToExtraProperty(RunningProgram& program)
{
    VM& vm = program.vm();
    JSObject* point = program.createPoint();
    AOT_TEST_CHECK(point->putDirect(vm, program.added(), jsNumber(1), dontEnum));
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.added(), jsNumber(1), dontEnum));

    AOT_TEST_CHECK(putOwnDataProperty(vm, point, program.added(), jsNumber(2)));
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.added(), jsNumber(2), dontEnum));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(point));
}

static void testStoreToReadOnlyExtraProperty(RunningProgram& program)
{
    VM& vm = program.vm();
    JSObject* point = program.createPoint();
    AOT_TEST_CHECK(point->putDirect(vm, program.added(), jsNumber(1), readOnly));

    AOT_TEST_CHECK(!putOwnDataProperty(vm, point, program.added(), jsNumber(2)));
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.added(), jsNumber(1), readOnly));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(point));
}

static void testStoreOfAbsentName(RunningProgram& program)
{
    VM& vm = program.vm();
    JSObject* extensible = program.createPoint();
    AOT_TEST_CHECK(putOwnDataProperty(vm, extensible, program.added(), jsNumber(1)));
    AOT_TEST_CHECK(hasOwnProperty(vm, extensible, program.added(), jsNumber(1), 0));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(extensible));

    JSObject* nonExtensible = program.createPoint();
    AOT_TEST_CHECK(nonExtensible->methodTable()->preventExtensions(nonExtensible, program.realm()));
    AOT_TEST_CHECK(!putOwnDataProperty(vm, nonExtensible, program.added(), jsNumber(1)));
    AOT_TEST_CHECK(!nonExtensible->getDirect(vm, program.added()));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(nonExtensible));
}

static void testDefinitionOfField(RunningProgram& program)
{
    VM& vm = program.vm();
    JSObject* unchanged = program.createPoint();
    AOT_TEST_CHECK(unchanged->putDirect(vm, program.x(), jsNumber(3)));
    AOT_TEST_CHECK(hasOwnProperty(vm, unchanged, program.x(), jsNumber(3), 0));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(unchanged));
    AOT_TEST_CHECK(isFieldInSlot(unchanged, program.x()));

    JSObject* redefined = program.createPoint();
    AOT_TEST_CHECK(redefined->putDirect(vm, program.x(), jsNumber(3), dontEnum));
    AOT_TEST_CHECK(hasOwnProperty(vm, redefined, program.x(), jsNumber(3), dontEnum));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(redefined));
    AOT_TEST_CHECK(!isFieldInSlot(redefined, program.x()));

    JSObject* sealed = program.createPoint();
    sealed->seal(vm);
    AOT_TEST_CHECK(sealed->putDirect(vm, program.x(), jsNumber(3)));
    AOT_TEST_CHECK(hasOwnProperty(vm, sealed, program.x(), jsNumber(3), 0));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(sealed));
    AOT_TEST_CHECK(!isFieldInSlot(sealed, program.x()));

    JSObject* withAccessor = program.createPoint();
    GetterSetter* accessor = GetterSetter::create(vm, program.realm(), nullptr, nullptr);
    AOT_TEST_CHECK(withAccessor->putDirectAccessor(program.realm(), program.x(), accessor, accessorAttribute));
    AOT_TEST_CHECK(hasOwnProperty(vm, withAccessor, program.x(), accessor, accessorAttribute));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(withAccessor));
    AOT_TEST_CHECK(!isFieldInSlot(withAccessor, program.x()));
}

static void testStoreToFieldOutsideSlot(RunningProgram& program)
{
    VM& vm = program.vm();
    JSObject* point = program.createPoint();
    AOT_TEST_CHECK(point->putDirect(vm, program.x(), jsNumber(3), dontEnum));
    AOT_TEST_CHECK(putOwnDataProperty(vm, point, program.x(), jsNumber(4)));
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.x(), jsNumber(4), dontEnum));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(point));
    AOT_TEST_CHECK(!isFieldInSlot(point, program.x()));

    JSObject* readOnlyPoint = program.createPoint();
    AOT_TEST_CHECK(readOnlyPoint->putDirect(vm, program.x(), jsNumber(3), readOnly));
    AOT_TEST_CHECK(!putOwnDataProperty(vm, readOnlyPoint, program.x(), jsNumber(4)));
    AOT_TEST_CHECK(hasOwnProperty(vm, readOnlyPoint, program.x(), jsNumber(3), readOnly));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(readOnlyPoint));
    AOT_TEST_CHECK(!isFieldInSlot(readOnlyPoint, program.x()));
}

static void testFieldReturnsToSlot(RunningProgram& program)
{
    VM& vm = program.vm();
    JSObject* point = program.createPoint();
    AOT_TEST_CHECK(point->putDirect(vm, program.x(), jsNumber(3), dontEnum | readOnly));
    AOT_TEST_CHECK(!isFieldInSlot(point, program.x()));

    AOT_TEST_CHECK(point->putDirect(vm, program.x(), jsNumber(4)));
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.x(), jsNumber(4), 0));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(point));
    AOT_TEST_CHECK(isFieldInSlot(point, program.x()));
}

static void testDefinitionOfCustomAccessor(RunningProgram& program)
{
    VM& vm = program.vm();
    JSObject* point = program.createPoint();
    AOT_TEST_CHECK(putOwnDataProperty(vm, point, program.x(), jsNumber(3)));
    CustomGetterSetter* accessor = CustomGetterSetter::create(vm, getSeven, nullptr);
    AOT_TEST_CHECK(point->putDirectCustomAccessor(vm, program.added(), accessor, PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly));
    // It is an extra property like any other.
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(point) && isFieldInSlot(point, program.x()));
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.x(), jsNumber(3), 0));
    AOT_TEST_CHECK(point->get(program.realm(), program.added()) == jsNumber(7));
}

static void testDefinitionOfExtraProperty(RunningProgram& program)
{
    VM& vm = program.vm();
    JSObject* point = program.createPoint();
    AOT_TEST_CHECK(point->putDirect(vm, program.added(), jsNumber(1), dontEnum | readOnly));

    AOT_TEST_CHECK(point->putDirect(vm, program.added(), jsNumber(2)));
    AOT_TEST_CHECK(hasOwnProperty(vm, point, program.added(), jsNumber(2), 0));
    AOT_TEST_CHECK(PropertyWalk::isCompiledObject(point));
}

namespace {

struct ProgramCase {
    ASCIILiteral name;
    void (*run)(RunningProgram&);
};

} // anonymous namespace

static constexpr ProgramCase programCases[] = {
    { "store to field"_s, testStoreToField },
    { "store to field of sealed object"_s, testStoreToFieldOfSealedObject },
    { "store to field of frozen object"_s, testStoreToFieldOfFrozenObject },
    { "store to extra property"_s, testStoreToExtraProperty },
    { "store to read-only extra property"_s, testStoreToReadOnlyExtraProperty },
    { "store of absent name"_s, testStoreOfAbsentName },
    { "definition of field"_s, testDefinitionOfField },
    { "store to field outside slot"_s, testStoreToFieldOutsideSlot },
    { "field returns to slot"_s, testFieldReturnsToSlot },
    { "definition of custom accessor"_s, testDefinitionOfCustomAccessor },
    { "definition of extra property"_s, testDefinitionOfExtraProperty },
};

static void runInProcessWithImage(const char* imagePath)
{
    useImageAt(imagePath);
    RunningProgram program;
    for (const ProgramCase& programCase : programCases) {
        dataLogLn(programCase.name);
        programCase.run(program);
    }
}

static void testCompiledObjects()
{
    if (const char* imagePath = byteCast<char>(Options::aotImagePath())) {
        runInProcessWithImage(imagePath);
        return;
    }

    TemporaryFile file(".image"_s, compileWithNarrowReferences().span());
    ChildProcessLauncher launcher(String(programPath()), { makeString("--aotImagePath="_s, file.path()) });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(compiledObjectsTest) };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(Typed::exitCodeForSuccess))
        dataLog(result.output);
    AOT_TEST_CHECK(result.exitedWith(Typed::exitCodeForSuccess));
}

std::span<const NativeTest> nativeStoreTests()
{
    static constexpr NativeTest tests[] = {
        { compiledObjectsTest, testCompiledObjects },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
