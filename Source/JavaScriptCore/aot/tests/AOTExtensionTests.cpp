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
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTProgramFile.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "HeapCellInlines.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "Options.h"
#include "PropertyNameArray.h"
#include "StrongInlines.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <bit>
#include <wtf/DataLog.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> extensionTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Wide" fields=[{key="flags" declared={type=number} flags=[IsRequired]}, {key="late0" declared={type=undefined|L0}}, {key="late1" declared={type=undefined|L0}}, {key="late2" declared={type=undefined|L0}}, {key="late3" declared={type=undefined|L0}}, {key="late4" declared={type=undefined|L0}}, {key="late5" declared={type=undefined|L0}}, {key="late6" declared={type=undefined|L0}}, {key="late7" declared={type=undefined|L0}}, {key="late8" declared={type=undefined|L0}}, {key="late9" declared={type=undefined|L0}}, {key="late10" declared={type=undefined|L0}}, {key="late11" declared={type=undefined|L0}}, {key="late12" declared={type=undefined|L0}}, {key="late13" declared={type=undefined|L0}}, {key="late14" declared={type=undefined|L0}}, {key="late15" declared={type=undefined|L0}}, {key="late16" declared={type=undefined|L0}}, {key="late17" declared={type=undefined|L0}}, {key="late18" declared={type=undefined|L0}}, {key="late19" declared={type=undefined|L0}}, {key="late20" declared={type=undefined|L0}}, {key="late21" declared={type=undefined|L0}}, {key="late22" declared={type=undefined|L0}}, {key="late23" declared={type=undefined|L0}}, {key="wide0" declared={type=anything}}, {key="wide1" declared={type=anything}}, {key="wide2" declared={type=anything}}, {key="wide3" declared={type=anything}}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = Call v1, -, v2, v3, v4
    Return
}
function f1 name="make" parameters=[{type=number}, {type=anything}] results=[{type=anything}] {
  b0(v0: number, v1: anything):
    Jump -> next b1()
  b1():
    LoopHint
    v3 = NewObject -, v0 layout=L0 fields=[0]
    v4 = FinishObject v3
    v5 = Call v1, -, v4
    v6 = ToBoolean v5
    Branch v6 -> taken b1(), notTaken b2()
  b2():
    Return v4
}
function f2 name="fill" parameters=[{type=L0}] results=[{type=anything}] {
  b0(v0: L0):
    PropSet v0, v0 key="late0"
    PropSet v0, v0 key="late1"
    PropSet v0, v0 key="late2"
    PropSet v0, v0 key="late3"
    PropSet v0, v0 key="late4"
    PropSet v0, v0 key="late5"
    PropSet v0, v0 key="late6"
    PropSet v0, v0 key="late7"
    PropSet v0, v0 key="late8"
    PropSet v0, v0 key="late9"
    PropSet v0, v0 key="late10"
    PropSet v0, v0 key="late11"
    PropSet v0, v0 key="late12"
    PropSet v0, v0 key="late13"
    PropSet v0, v0 key="late14"
    PropSet v0, v0 key="late15"
    PropSet v0, v0 key="late16"
    PropSet v0, v0 key="late17"
    PropSet v0, v0 key="late18"
    PropSet v0, v0 key="late19"
    PropSet v0, v0 key="late20"
    PropSet v0, v0 key="late21"
    PropSet v0, v0 key="late22"
    PropSet v0, v0 key="late23"
    PropSet v0, v0 key="wide0"
    PropSet v0, v0 key="wide1"
    PropSet v0, v0 key="wide2"
    PropSet v0, v0 key="wide3"
    v29 = PropGet v0 key="late0"
    v30 = PropGet v0 key="late1"
    v31 = PropGet v0 key="late2"
    v32 = PropGet v0 key="late3"
    v33 = PropGet v0 key="late4"
    v34 = PropGet v0 key="late5"
    v35 = PropGet v0 key="late6"
    v36 = PropGet v0 key="late7"
    v37 = PropGet v0 key="late8"
    v38 = PropGet v0 key="late9"
    v39 = PropGet v0 key="late10"
    v40 = PropGet v0 key="late11"
    v41 = PropGet v0 key="late12"
    v42 = PropGet v0 key="late13"
    v43 = PropGet v0 key="late14"
    v44 = PropGet v0 key="late15"
    v45 = PropGet v0 key="late16"
    v46 = PropGet v0 key="late17"
    v47 = PropGet v0 key="late18"
    v48 = PropGet v0 key="late19"
    v49 = PropGet v0 key="late20"
    v50 = PropGet v0 key="late21"
    v51 = PropGet v0 key="late22"
    v52 = PropGet v0 key="late23"
    v53 = PropGet v0 key="wide0"
    v54 = PropGet v0 key="wide1"
    v55 = PropGet v0 key="wide2"
    v56 = PropGet v0 key="wide3"
    PropSet v0, v0 key="late0"
    PropSet v0, v0 key="late1"
    PropSet v0, v0 key="late2"
    PropSet v0, v0 key="late3"
    PropSet v0, v0 key="late0"
    PropSet v0, v0 key="late1"
    PropSet v0, v0 key="late2"
    PropSet v0, v0 key="late3"
    Return
}
function f3 name="clear" parameters=[{type=L0}] results=[{type=anything}] {
  b0(v0: L0):
    v1 = Undefined
    PropSet v0, v1 key="late5"
    PropSet v0, v1 key="late6"
    Return
}
)IR"_s;

// A layout that is made seldom has no Extension.
static Vector<uint8_t> compileWithFieldsOutOfLine(std::optional<Wire::ReferenceWidth> referenceWidth)
{
    Vector<uint8_t> image;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        options.executableIdentity = { };
        options.testing = Wire::TestingSwitch::NoLayoutIsMadeSeldom;
        if (referenceWidth)
            options.target.referenceWidth = *referenceWidth;
        BuildContext build(options);
        std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(String(programText)), Typed::IRForm::AsRead, build);
        AOT_TEST_CHECK(program);
        std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
        if (output->image.isEmpty())
            Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
        AOT_TEST_CHECK(!output->image.isEmpty());
        image = WTF::move(output->image);
    });
    return image;
}

static void runWithFieldsOutOfLine(ASCIILiteral testName, std::optional<Wire::ReferenceWidth> referenceWidth, LoadedProgramTest body)
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(testName, programText, body);
        return;
    }
    TemporaryFile file(".image"_s, compileWithFieldsOutOfLine(referenceWidth).span());
    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(testName), makeString("--aotImagePath="_s, file.path()) };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0))
        dataLogLn(result.output);
    AOT_TEST_CHECK(result.exitedWith(0));
}

template<LoadedProgramTest body>
static void runNativeTestWithNarrowReferences()
{
    for (const NativeTest& test : extensionTests()) {
        if (test.run == &runNativeTestWithNarrowReferences<body>) {
            runWithFieldsOutOfLine(test.name, Wire::ReferenceWidth::Narrow, body);
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<LoadedProgramTest body>
static void runNativeTestWithDefaultReferences()
{
    for (const NativeTest& test : extensionTests()) {
        if (test.run == &runNativeTestWithDefaultReferences<body>) {
            runWithFieldsOutOfLine(test.name, std::nullopt, body);
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

class Fixture {
    WTF_MAKE_NONCOPYABLE(Fixture);
public:
    explicit Fixture(LoadedProgram&);

    VM& vm() { return m_program.vm(); }
    const LayoutData& layout() const { return *m_layout; }
    // In ascending order of their offsets.
    std::span<const FieldData* const> fieldsOutOfLine() const { return m_fieldsOutOfLine.span(); }
    Object* create() { return uncheckedDowncast<Object>(m_program.state().objects->structures.allocate(vm(), m_allocation)); }

private:
    LoadedProgram& m_program;
    const LayoutData* m_layout { nullptr };
    uint32_t m_allocation { 0 };
    Vector<const FieldData*> m_fieldsOutOfLine;
};

} // anonymous namespace

Fixture::Fixture(LoadedProgram& program)
    : m_program(program)
{
    const ProgramTables& tables = program.state().tables;
    std::span allocations = tables.allocations();
    for (uint32_t number = 0; !m_layout && number < allocations.size(); ++number) {
        uint32_t shape = allocations[number].shape;
        if (!tables.hasLayoutForShape(shape) || !tables.layoutForShape(shape).hasExtension())
            continue;
        m_layout = &tables.layoutForShape(shape);
        m_allocation = number;
    }
    AOT_TEST_CHECK(m_layout);
    for (const FieldData& field : m_layout->fields()) {
        if (field.isOutOfLine())
            m_fieldsOutOfLine.append(&field);
    }
    std::ranges::sort(m_fieldsOutOfLine, { }, [](const FieldData* field) {
        return field->slot.offset;
    });
    AOT_TEST_CHECK(m_fieldsOutOfLine.size() >= 12);
}

static uint64_t granuleBitsOf(std::span<const FieldData* const> fields)
{
    uint64_t bits = 0;
    for (const FieldData* field : fields)
        bits |= granuleBitOf(field->slot.offset);
    return bits;
}

static void checkHoldsExactly(Fixture& fixture, Object* object, std::span<const FieldData* const> stored, JSValue value)
{
    Extension* extension = object->extension();
    AOT_TEST_CHECK(extension && extension->granuleBits() == granuleBitsOf(stored));
    AOT_TEST_CHECK(extension->sizeInBytes() <= reinterpret_cast<HeapCell*>(extension)->cellSize());
    for (const FieldData* field : fixture.fieldsOutOfLine()) {
        bool isStored = std::ranges::find(stored, field) != stored.end();
        AOT_TEST_CHECK(object->isInSlot(*field) == isStored);
        AOT_TEST_CHECK(isStored ? object->read(*field) == value : !object->read(*field));
        AOT_TEST_CHECK(!!object->slotOf(*field) == extension->holds(field->slot.offset));
    }
}

static void checkAnObjectPaysForTheFieldsItHas(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    AOT_TEST_CHECK(!fixture.create()->extension());
    for (const FieldData* field : fixture.fieldsOutOfLine()) {
        Object* object = fixture.create();
        AOT_TEST_CHECK(!object->slotOf(*field) && !object->read(*field));
        AOT_TEST_CHECK(object->tryStore(vm, *field, object));
        checkHoldsExactly(fixture, object, singleElementSpan(field), object);
        AOT_TEST_CHECK(reinterpret_cast<HeapCell*>(object->extension())->cellSize() == minExtensionSize);
    }
}

static void checkFieldsAreAddedInEveryOrder(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    std::span all = fixture.fieldsOutOfLine();
    Vector<const FieldData*> chosen { all[0], all[3], all[all.size() / 2], all[all.size() - 1] };
    std::ranges::sort(chosen);
    do {
        Object* object = fixture.create();
        Vector<const FieldData*> stored;
        for (const FieldData* field : chosen) {
            AOT_TEST_CHECK(object->tryStore(vm, *field, object));
            stored.append(field);
            checkHoldsExactly(fixture, object, stored.span(), object);
        }
    } while (std::ranges::next_permutation(chosen).found);
}

static void checkAGranuleGoesInPlaceOnlyBehindTheOthers(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    Object* object = fixture.create();
    constexpr bool collectorMayVisit = true;
    auto bit = [](unsigned granule) {
        return 1ull << granule;
    };

    Object* single = fixture.create();
    single->ensureExtensionGranules(vm, bit(5), collectorMayVisit);
    Extension* smallest = single->extension();
    AOT_TEST_CHECK(smallest->granuleBits() == bit(5) && reinterpret_cast<HeapCell*>(smallest)->cellSize() == minExtensionSize);
    smallest->granules()[0] = 55;
    single->ensureExtensionGranules(vm, bit(7), collectorMayVisit);
    Extension* grown = single->extension();
    AOT_TEST_CHECK(grown != smallest && grown->granuleBits() == (bit(5) | bit(7)) && grown->granules()[0] == 55 && !grown->granules()[1]);
    AOT_TEST_CHECK(smallest->granuleBits() == bit(5) && smallest->granules()[0] == 55);
    grown->granules()[0] = 0;

    object->ensureExtensionGranules(vm, bit(5) | bit(7), collectorMayVisit);
    Extension* first = object->extension();
    AOT_TEST_CHECK(first->granuleBits() == (bit(5) | bit(7)) && reinterpret_cast<HeapCell*>(first)->cellSize() == extensionCellSizeFor(3));
    first->granules()[0] = 55;
    first->granules()[1] = 77;

    object->ensureExtensionGranules(vm, bit(9), collectorMayVisit);
    AOT_TEST_CHECK(object->extension() == first && first->granuleBits() == (bit(5) | bit(7) | bit(9)));
    AOT_TEST_CHECK(first->granules()[0] == 55 && first->granules()[1] == 77 && !first->granules()[2]);
    first->granules()[2] = 99;

    object->ensureExtensionGranules(vm, bit(11), collectorMayVisit);
    Extension* second = object->extension();
    AOT_TEST_CHECK(second != first && second->granuleBits() == (bit(5) | bit(7) | bit(9) | bit(11)));
    AOT_TEST_CHECK(second->granules()[0] == 55 && second->granules()[1] == 77 && second->granules()[2] == 99 && !second->granules()[3]);

    object->ensureExtensionGranules(vm, bit(6), collectorMayVisit);
    Extension* third = object->extension();
    AOT_TEST_CHECK(third != second && third->granuleBits() == (bit(5) | bit(6) | bit(7) | bit(9) | bit(11)));
    AOT_TEST_CHECK(third->granules()[0] == 55 && !third->granules()[1] && third->granules()[2] == 77 && third->granules()[3] == 99 && !third->granules()[4]);
    AOT_TEST_CHECK(first->granuleBits() == (bit(5) | bit(7) | bit(9)) && first->granules()[0] == 55 && first->granules()[1] == 77 && first->granules()[2] == 99);
    AOT_TEST_CHECK(second->granuleBits() == (bit(5) | bit(7) | bit(9) | bit(11)) && second->granules()[2] == 99);

    object->ensureExtensionGranules(vm, bit(0) | bit(63) | bit(7), collectorMayVisit);
    Extension* last = object->extension();
    AOT_TEST_CHECK(last->granuleBits() == (bit(0) | bit(5) | bit(6) | bit(7) | bit(9) | bit(11) | bit(63)));
    AOT_TEST_CHECK(!last->granules()[0] && last->granules()[1] == 55 && last->granules()[3] == 77 && last->granules()[4] == 99 && !last->granules()[6]);
    object->ensureExtensionGranules(vm, last->granuleBits(), collectorMayVisit);
    AOT_TEST_CHECK(object->extension() == last);
    for (unsigned index = 0; index < 7; ++index)
        last->granules()[index] = 0;

    // The room follows from the number of granules. A size class may be wider than the size asked for, and whoever allocates an Extension clears that size and no more.
    constexpr unsigned count = 10;
    constexpr unsigned capacity = extensionCapacityFor(count);
    static_assert(capacity > count && extensionCapacityFor(capacity) == capacity && extensionCapacityFor(capacity + 1) > capacity);
    Object* other = fixture.create();
    other->ensureExtensionGranules(vm, bit(count) - 1);
    Extension* roomy = other->extension();
    size_t cellSize = reinterpret_cast<HeapCell*>(roomy)->cellSize();
    AOT_TEST_CHECK(cellSize >= extensionCellSizeFor(count));
    memsetSpan(std::span { reinterpret_cast<uint8_t*>(roomy), cellSize }.subspan(extensionCellSizeFor(count)), 0xab);
    for (unsigned granule = count; granule < capacity; ++granule) {
        other->ensureExtensionGranules(vm, bit(2 * granule));
        AOT_TEST_CHECK(other->extension() == roomy && !roomy->granules()[granule]);
    }
    AOT_TEST_CHECK(roomy->sizeInBytes() == extensionCellSizeFor(count));
    other->ensureExtensionGranules(vm, bit(2 * capacity));
    AOT_TEST_CHECK(other->extension() != roomy && static_cast<unsigned>(std::popcount(other->extension()->granuleBits())) == capacity + 1 && !other->extension()->granules()[capacity]);
}

static void checkAGranuleGoesBetweenTheOthersWhileTheCollectorVisitsNothing(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    constexpr bool collectorMayVisit = false;
    auto bit = [](unsigned granule) {
        return 1ull << granule;
    };

    Object* object = fixture.create();
    object->ensureExtensionGranules(vm, bit(5) | bit(7) | bit(9) | bit(11), collectorMayVisit);
    Extension* extension = object->extension();
    static_assert(extensionCapacityFor(4) == 5 && extensionCapacityFor(5) == 5 && extensionCapacityFor(6) > 5);
    extension->granules()[0] = 55;
    extension->granules()[1] = 77;
    extension->granules()[2] = 99;
    extension->granules()[3] = 111;

    object->ensureExtensionGranules(vm, bit(6), collectorMayVisit);
    AOT_TEST_CHECK(object->extension() == extension && extension->granuleBits() == (bit(5) | bit(6) | bit(7) | bit(9) | bit(11)));
    AOT_TEST_CHECK(extension->granules()[0] == 55 && !extension->granules()[1] && extension->granules()[2] == 77 && extension->granules()[3] == 99 && extension->granules()[4] == 111);

    // It is full now, so the next granule goes into a copy wherever it belongs.
    object->ensureExtensionGranules(vm, bit(0), collectorMayVisit);
    Extension* larger = object->extension();
    AOT_TEST_CHECK(larger != extension && larger->granuleBits() == (bit(0) | bit(5) | bit(6) | bit(7) | bit(9) | bit(11)));
    AOT_TEST_CHECK(!larger->granules()[0] && larger->granules()[1] == 55 && !larger->granules()[2] && larger->granules()[3] == 77 && larger->granules()[4] == 99 && larger->granules()[5] == 111);
    AOT_TEST_CHECK(extension->granuleBits() == (bit(5) | bit(6) | bit(7) | bit(9) | bit(11)) && extension->granules()[4] == 111);

    // Several at once: in front of all, between two, and behind all.
    static_assert(extensionCapacityFor(6) == 9);
    object->ensureExtensionGranules(vm, bit(3) | bit(8) | bit(63), collectorMayVisit);
    AOT_TEST_CHECK(object->extension() == larger && larger->granuleBits() == (bit(0) | bit(3) | bit(5) | bit(6) | bit(7) | bit(8) | bit(9) | bit(11) | bit(63)));
    const uint64_t expected[] = { 0, 0, 55, 0, 77, 0, 99, 111, 0 };
    for (unsigned index = 0; index < std::size(expected); ++index)
        AOT_TEST_CHECK(larger->granules()[index] == expected[index]);
    for (unsigned index = 0; index < std::size(expected); ++index)
        larger->granules()[index] = 0;
    for (unsigned index = 0; index < 5; ++index)
        extension->granules()[index] = 0;

    // Fields keep their values through every order of creation, whichever way their granules came.
    std::span<const FieldData* const> fields = fixture.fieldsOutOfLine();
    Object* filled = fixture.create();
    for (size_t index = fields.size(); index--;) {
        filled->ensureExtensionGranules(vm, granuleBitOf(fields[index]->slot.offset), collectorMayVisit);
        AOT_TEST_CHECK(filled->tryStore(vm, *fields[index], filled));
        for (size_t stored = index; stored < fields.size(); ++stored)
            AOT_TEST_CHECK(filled->read(*fields[stored]) == JSValue(filled));
    }
}

static void checkTheCollectorFindsReferencesByTheBits(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    std::span all = fixture.fieldsOutOfLine();
    Strong<JSObject> owner(vm, fixture.create());
    Object* object = uncheckedDowncast<Object>(owner.get());
    Vector<Object*> referents;
    for (size_t index = all.size(); index--;) {
        if (index % 3 == 1)
            continue;
        Object* referent = fixture.create();
        AOT_TEST_CHECK(object->tryStore(vm, *all[index], referent));
        referents.append(referent);
    }
    Object* unreferenced = fixture.create();
    vm.heap.collectSync(CollectionScope::Full);
    for (Object* referent : referents)
        AOT_TEST_CHECK(vm.heap.isMarked(referent));
    AOT_TEST_CHECK(vm.heap.isMarked(reinterpret_cast<HeapCell*>(object->extension())));
    UNUSED_PARAM(unreferenced);

    size_t next = 0;
    for (size_t index = all.size(); index--;) {
        if (index % 3 != 1)
            AOT_TEST_CHECK(object->read(*all[index]) == JSValue(referents[next++]));
    }

    unsigned numberOfReferences = 0;
    forEachReference(object, referenceTableOf(fixture.layout()), [&](Reference reference) {
        numberOfReferences += reference.kind != Reference::Kind::Auxiliary && reference.bits;
    });
    AOT_TEST_CHECK(numberOfReferences == referents.size());
}

static void checkAClearedFieldKeepsItsGranule(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    std::span all = fixture.fieldsOutOfLine();
    Object* object = fixture.create();
    AOT_TEST_CHECK(object->tryStore(vm, *all[2], object) && object->tryStore(vm, *all[6], object));
    Extension* extension = object->extension();
    uint64_t bits = extension->granuleBits();
    object->clear(*all[2]);
    object->clear(*all[4]);
    AOT_TEST_CHECK(object->extension() == extension && extension->granuleBits() == bits);
    AOT_TEST_CHECK(!object->isInSlot(*all[2]) && object->slotOf(*all[2]) && !object->isInSlot(*all[4]) && object->read(*all[6]) == JSValue(object));
}

static const FieldData& fieldThatMayExistWithoutStorage(Fixture& fixture)
{
    for (const FieldData* field : fixture.fieldsOutOfLine()) {
        if (field->flags & fieldMayExistWithoutStorage)
            return *field;
    }
    failNativeTest(__FILE__, __LINE__, "a field that a store of undefined creates may exist without storage");
}

static bool isAmongKeys(LoadedProgram& program, Object* object, PropertyName name)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    PropertyNameArrayBuilder names(program.vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, program.realm(), names, DontEnumPropertiesMode::Exclude);
    AOT_TEST_CHECK(!scope.exception());
    return std::ranges::any_of(names, [&](const Identifier& key) {
        return key.impl() == name.uid();
    });
}

static bool hasPropertyNamed(LoadedProgram& program, Object* object, PropertyName name)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    bool result = object->hasProperty(program.realm(), name);
    AOT_TEST_CHECK(!scope.exception());
    return result;
}

static JSValue getByName(LoadedProgram& program, Object* object, PropertyName name)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(program.vm());
    JSValue result = object->get(program.realm(), name);
    AOT_TEST_CHECK(!scope.exception() && result);
    return result;
}

static void checkAStoreOfUndefinedNeedsNoStorage(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    State& state = program.state();
    const FieldData& field = fieldThatMayExistWithoutStorage(fixture);
    PropertyName name = state.objects->layouts.nameOf(vm, field);
    uint64_t granuleBit = granuleBitOf(field.slot.offset);

    Object* object = fixture.create();
    auto holdsUndefined = [&] {
        return object->isInKeyOrder(state, field) && object->read(field).isUndefined() && findFieldProperty(vm, *object, field).isInSlot
            && hasPropertyNamed(program, object, name) && isAmongKeys(program, object, name) && getByName(program, object, name).isUndefined();
    };
    AOT_TEST_CHECK(!object->isInKeyOrder(state, field) && !object->read(field) && !findFieldProperty(vm, *object, field));
    AOT_TEST_CHECK(!hasPropertyNamed(program, object, name) && !isAmongKeys(program, object, name));

    AOT_TEST_CHECK(tryCreateField(vm, state, *object, field, jsUndefined()));
    AOT_TEST_CHECK(holdsUndefined() && !object->extension() && !object->isInSlot(field));
    AOT_TEST_CHECK(object->tryStore(vm, field, jsUndefined()));
    AOT_TEST_CHECK(holdsUndefined() && !object->extension());

    AOT_TEST_CHECK(object->tryStore(vm, field, object));
    Extension* extension = object->extension();
    AOT_TEST_CHECK(extension && extension->granuleBits() == granuleBit && object->isInSlot(field));
    AOT_TEST_CHECK(object->read(field) == JSValue(object) && getByName(program, object, name) == JSValue(object) && isAmongKeys(program, object, name));

    AOT_TEST_CHECK(object->tryStore(vm, field, jsUndefined()));
    AOT_TEST_CHECK(holdsUndefined() && object->isInSlot(field));
    AOT_TEST_CHECK(object->extension() == extension && extension->granuleBits() == granuleBit);

    // What compiled code does where it stores undefined and finds no granule: it has the run time list the field, and that is all.
    Object* listed = fixture.create();
    operationAOTDidCreateFieldAt(&program.instance(), listed, slotKeyFor(field.slot.offset, true));
    AOT_TEST_CHECK(!listed->extension() && listed->isInKeyOrder(state, field) && listed->read(field).isUndefined());
    AOT_TEST_CHECK(hasPropertyNamed(program, listed, name) && isAmongKeys(program, listed, name));
}

static PropertyAnswer classifyReadOf(LoadedProgram& program, Object* object, PropertyName name)
{
    Structure* structureBefore = object->structure();
    PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
    JSValue(object).get(program.realm(), name, slot);
    return classifyRead({ program.realm(), object, structureBefore, name.uid(), slot });
}

// Listed or not, the field reads as undefined while the objects above answer undefined, so such a read is cached and asks no key order. Whether the property exists, the slot for reads does not say.
static void checkAReadOfAnEmptySlotIsCached(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    State& state = program.state();
    const FieldData& field = fieldThatMayExistWithoutStorage(fixture);
    PropertyName name = state.objects->layouts.nameOf(vm, field);
    Object* unlisted = fixture.create();
    Object* listed = fixture.create();
    JSObject* prototype = constructEmptyObject(program.realm());
    for (Object* object : { unlisted, listed })
        AOT_TEST_CHECK(object->methodTable()->setPrototype(object, program.realm(), prototype, false));
    AOT_TEST_CHECK(tryCreateField(vm, state, *listed, field, jsUndefined()) && !listed->isInSlot(field) && !unlisted->isInKeyOrder(state, field));
    // What answers where no slot does: a read that is not cached asks it first.
    auto readByLeafLookup = [&](Object* object) {
        return JSValue::decode(operationAOTGetGeneric(&program.instance(), object, name.uid()));
    };

    for (Object* object : { unlisted, listed }) {
        PropertyAnswer answer = classifyReadOf(program, object, name);
        AOT_TEST_CHECK(answer.kind == PropertyAnswerKind::Field && answer.field == &field);
        AOT_TEST_CHECK(answer.emptySlotMayBeUndefined && !answer.absentIsUndefined);
        AOT_TEST_CHECK(answer.dependsOnChain());
        Slot slot = slotFor(answer, object->structure());
        AOT_TEST_CHECK(slot.kind == Slot::Kind::Field && slot.has(Slot::emptySlotMayBeUndefined) && slot.has(Slot::dependsOnChain) && !slot.has(Slot::absentIsUndefined));
        AOT_TEST_CHECK(getByName(program, object, name).isUndefined() && readByLeafLookup(object).isUndefined());
    }

    prototype->putDirect(vm, name, jsNumber(3));
    for (Object* object : { unlisted, listed }) {
        PropertyAnswer answer = classifyReadOf(program, object, name);
        AOT_TEST_CHECK(answer.kind == PropertyAnswerKind::FieldOrInheritedValue && answer.field == &field && answer.holder == prototype && answer.emptySlotMayBeUndefined && !answer.absentIsUndefined);
        Slot slot = slotFor(answer, object->structure());
        AOT_TEST_CHECK(slot.kind == Slot::Kind::FieldOrInherited && slot.has(Slot::emptySlotMayBeUndefined) && !slot.has(Slot::inheritsGetter));
    }
    AOT_TEST_CHECK(getByName(program, unlisted, name) == jsNumber(3) && getByName(program, listed, name).isUndefined());
    AOT_TEST_CHECK(readByLeafLookup(unlisted) == jsNumber(3) && readByLeafLookup(listed).isUndefined());
    AOT_TEST_CHECK(listed->tryStore(vm, field, listed) && readByLeafLookup(listed) == JSValue(listed));
}

static void checkWhetherTheOrderListsAFieldIsRememberedBesideExtraProperties(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    State& state = program.state();
    const FieldData& field = fieldThatMayExistWithoutStorage(fixture);
    PropertyName name = state.objects->layouts.nameOf(vm, field);
    auto readsAsValue = [&](Object* object) {
        return !!operationAOTHasExtraProperty(&program.instance(), object, field.name);
    };
    auto remembered = [&](Object* object) {
        return state.objects->keyOrders.rememberedWhetherOrderLists(object->shape(), object->orderWord() & orderAndCountMask, field.name);
    };

    Object* unlisted = fixture.create();
    Object* listed = fixture.create();
    AOT_TEST_CHECK(tryCreateField(vm, state, *listed, field, jsUndefined()) && !listed->isInSlot(field));
    for (Object* object : { unlisted, listed }) {
        PutPropertySlot slot(object, true);
        AOT_TEST_CHECK(object->methodTable()->put(object, program.realm(), Identifier::fromString(vm, "no field of the layout"_s), jsNumber(1), slot));
        AOT_TEST_CHECK(object->hasExtraProperties() && !remembered(object));
    }
    for (unsigned round = 0; round < 2; ++round) {
        AOT_TEST_CHECK(readsAsValue(listed) && !readsAsValue(unlisted));
        AOT_TEST_CHECK(remembered(listed) == std::optional<bool>(true) && remembered(unlisted) == std::optional<bool>(false));
    }

    PropertyDescriptor hidden(jsNumber(2), static_cast<unsigned>(PropertyAttribute::DontEnum));
    AOT_TEST_CHECK(listed->methodTable()->defineOwnProperty(listed, program.realm(), name, hidden, false));
    AOT_TEST_CHECK(livesInExtraProperties(vm, *listed, field) && !readsAsValue(listed) && getByName(program, listed, name) == jsNumber(2));
}

// Whichever other question is asked in between, the answer is still there: two questions that fall on one place of the memory do not throw each other out.
static void checkTwoAnswersAboutTheOrderStayRememberedSideBySide(LoadedProgram& program)
{
    KeyOrderTable& orders = program.state().objects->keyOrders;
    for (uint32_t other = 1; other < 4096; ++other) {
        orders.rememberWhetherOrderLists(7, 3, 11, true);
        orders.rememberWhetherOrderLists(7 + other, 3, 11, false);
        AOT_TEST_CHECK(orders.rememberedWhetherOrderLists(7, 3, 11) == std::optional<bool>(true));
        AOT_TEST_CHECK(orders.rememberedWhetherOrderLists(7 + other, 3, 11) == std::optional<bool>(false));
    }
}

// Two narrow references share a granule. The one that holds undefined without storage has storage once the other gets a value, and nothing was ever written to it.
static void checkTheGranuleOfANeighborTakesNoFieldAway(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    State& state = program.state();
    unsigned numberOfPairs = 0;
    for (const FieldData* field : fixture.fieldsOutOfLine()) {
        if (!(field->flags & fieldMayExistWithoutStorage))
            continue;
        PropertyName name = state.objects->layouts.nameOf(vm, *field);
        for (const FieldData* neighbor : fixture.fieldsOutOfLine()) {
            if (neighbor == field || granuleBitOf(neighbor->slot.offset) != granuleBitOf(field->slot.offset))
                continue;
            ++numberOfPairs;
            Object* object = fixture.create();
            AOT_TEST_CHECK(tryCreateField(vm, state, *object, *field, jsUndefined()) && !object->extension());
            AOT_TEST_CHECK(tryCreateField(vm, state, *object, *neighbor, object) && object->slotOf(*field));
            AOT_TEST_CHECK(object->isInKeyOrder(state, *field) && object->read(*field).isUndefined() && findFieldProperty(vm, *object, *field).isInSlot);
            AOT_TEST_CHECK(hasPropertyNamed(program, object, name) && isAmongKeys(program, object, name) && getByName(program, object, name).isUndefined());
        }
    }
    AOT_TEST_CHECK(numberOfPairs);
}

static void checkTheFirstGrowthReservesWhatTheConstructorFills(LoadedProgram& program)
{
    Fixture fixture(program);
    uint64_t reserved = fixture.layout().granulesFilledByConstructor();
    for (const FieldData* field : fixture.fieldsOutOfLine()) {
        uint64_t granuleBit = granuleBitOf(field->slot.offset);
        Object* object = fixture.create();
        operationAOTEnsureExtensionGranules(&program.instance(), object, granuleBit);
        AOT_TEST_CHECK(object->extension() && object->extension()->granuleBits() == (granuleBit | reserved));
    }
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithDefaultReferences<body>;

std::span<const NativeTest> extensionTests()
{
    static constexpr NativeTest tests[] = {
        { "extension/an-object-pays-for-the-fields-it-has"_s, withProgram<checkAnObjectPaysForTheFieldsItHas> },
        { "extension/fields-are-added-in-every-order"_s, withProgram<checkFieldsAreAddedInEveryOrder> },
        { "extension/a-granule-goes-in-place-only-behind-the-others"_s, withProgram<checkAGranuleGoesInPlaceOnlyBehindTheOthers> },
        { "extension/a-granule-goes-between-the-others-while-the-collector-visits-nothing"_s, withProgram<checkAGranuleGoesBetweenTheOthersWhileTheCollectorVisitsNothing> },
        { "extension/the-collector-finds-references-by-the-bits"_s, withProgram<checkTheCollectorFindsReferencesByTheBits> },
        { "extension/a-cleared-field-keeps-its-granule"_s, withProgram<checkAClearedFieldKeepsItsGranule> },
        { "extension/a-store-of-undefined-needs-no-storage"_s, withProgram<checkAStoreOfUndefinedNeedsNoStorage> },
        { "extension/a-read-of-an-empty-slot-is-cached"_s, withProgram<checkAReadOfAnEmptySlotIsCached> },
        { "extension/whether-the-order-lists-a-field-is-remembered-beside-extra-properties"_s, withProgram<checkWhetherTheOrderListsAFieldIsRememberedBesideExtraProperties> },
        { "extension/two-answers-about-the-order-stay-remembered-side-by-side"_s, withProgram<checkTwoAnswersAboutTheOrderStayRememberedSideBySide> },
        { "extension/the-first-growth-reserves-what-the-constructor-fills"_s, withProgram<checkTheFirstGrowthReservesWhatTheConstructorFills> },
        { "extension/narrow-references/an-object-pays-for-the-fields-it-has"_s, runNativeTestWithNarrowReferences<checkAnObjectPaysForTheFieldsItHas> },
        { "extension/narrow-references/fields-are-added-in-every-order"_s, runNativeTestWithNarrowReferences<checkFieldsAreAddedInEveryOrder> },
        { "extension/narrow-references/a-granule-goes-in-place-only-behind-the-others"_s, runNativeTestWithNarrowReferences<checkAGranuleGoesInPlaceOnlyBehindTheOthers> },
        { "extension/narrow-references/a-granule-goes-between-the-others-while-the-collector-visits-nothing"_s, runNativeTestWithNarrowReferences<checkAGranuleGoesBetweenTheOthersWhileTheCollectorVisitsNothing> },
        { "extension/narrow-references/the-collector-finds-references-by-the-bits"_s, runNativeTestWithNarrowReferences<checkTheCollectorFindsReferencesByTheBits> },
        { "extension/narrow-references/a-cleared-field-keeps-its-granule"_s, runNativeTestWithNarrowReferences<checkAClearedFieldKeepsItsGranule> },
        { "extension/narrow-references/a-store-of-undefined-needs-no-storage"_s, runNativeTestWithNarrowReferences<checkAStoreOfUndefinedNeedsNoStorage> },
        { "extension/narrow-references/the-granule-of-a-neighbor-takes-no-field-away"_s, runNativeTestWithNarrowReferences<checkTheGranuleOfANeighborTakesNoFieldAway> },
        { "extension/narrow-references/the-first-growth-reserves-what-the-constructor-fills"_s, runNativeTestWithNarrowReferences<checkTheFirstGrowthReservesWhatTheConstructorFills> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
