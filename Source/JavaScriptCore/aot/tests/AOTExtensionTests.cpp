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
#include "AOTObjectModel.h"
#include "AOTProgramFile.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "HeapCellInlines.h"
#include "JSCInlines.h"
#include "Options.h"
#include "StrongInlines.h"
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
    v4 = Call v1, -, v2, v3
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
    Return
}
)IR"_s;

static Vector<uint8_t> compileWithNarrowReferences()
{
    Vector<uint8_t> image;
    runOnBuildThread([&] {
        Wire::CompileOptions options = makeDefaultCompileOptions();
        options.executableIdentity = { };
        options.target.referenceWidth = Wire::ReferenceWidth::Narrow;
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

static void runWithNarrowReferences(ASCIILiteral testName, LoadedProgramTest body)
{
    if (Options::aotImagePath()) {
        runWithLoadedProgram(testName, programText, body);
        return;
    }
    TemporaryFile file(".image"_s, compileWithNarrowReferences().span());
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
            runWithNarrowReferences(test.name, body);
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
        AOT_TEST_CHECK(object->store(vm, *field, object));
        checkHoldsExactly(fixture, object, singleElementSpan(field), object);
        AOT_TEST_CHECK(reinterpret_cast<HeapCell*>(object->extension())->cellSize() == extensionCellSizeFor(1));
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
            AOT_TEST_CHECK(object->store(vm, *field, object));
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
    auto bit = [](unsigned granule) {
        return 1ull << granule;
    };

    object->ensureExtensionGranules(vm, bit(5) | bit(7));
    Extension* first = object->extension();
    AOT_TEST_CHECK(first->granuleBits() == (bit(5) | bit(7)) && reinterpret_cast<HeapCell*>(first)->cellSize() == extensionCellSizeFor(3));
    first->granules()[0] = 55;
    first->granules()[1] = 77;

    object->ensureExtensionGranules(vm, bit(9));
    AOT_TEST_CHECK(object->extension() == first && first->granuleBits() == (bit(5) | bit(7) | bit(9)));
    AOT_TEST_CHECK(first->granules()[0] == 55 && first->granules()[1] == 77 && !first->granules()[2]);
    first->granules()[2] = 99;

    object->ensureExtensionGranules(vm, bit(11));
    Extension* second = object->extension();
    AOT_TEST_CHECK(second != first && second->granuleBits() == (bit(5) | bit(7) | bit(9) | bit(11)));
    AOT_TEST_CHECK(second->granules()[0] == 55 && second->granules()[1] == 77 && second->granules()[2] == 99 && !second->granules()[3]);

    object->ensureExtensionGranules(vm, bit(6));
    Extension* third = object->extension();
    AOT_TEST_CHECK(third != second && third->granuleBits() == (bit(5) | bit(6) | bit(7) | bit(9) | bit(11)));
    AOT_TEST_CHECK(third->granules()[0] == 55 && !third->granules()[1] && third->granules()[2] == 77 && third->granules()[3] == 99 && !third->granules()[4]);
    AOT_TEST_CHECK(first->granuleBits() == (bit(5) | bit(7) | bit(9)) && first->granules()[0] == 55 && first->granules()[1] == 77 && first->granules()[2] == 99);
    AOT_TEST_CHECK(second->granuleBits() == (bit(5) | bit(7) | bit(9) | bit(11)) && second->granules()[2] == 99);

    object->ensureExtensionGranules(vm, bit(0) | bit(63) | bit(7));
    Extension* last = object->extension();
    AOT_TEST_CHECK(last->granuleBits() == (bit(0) | bit(5) | bit(6) | bit(7) | bit(9) | bit(11) | bit(63)));
    AOT_TEST_CHECK(!last->granules()[0] && last->granules()[1] == 55 && last->granules()[3] == 77 && last->granules()[4] == 99 && !last->granules()[6]);
    object->ensureExtensionGranules(vm, last->granuleBits());
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
        AOT_TEST_CHECK(object->store(vm, *all[index], referent));
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
    AOT_TEST_CHECK(object->store(vm, *all[2], object) && object->store(vm, *all[6], object));
    Extension* extension = object->extension();
    uint64_t bits = extension->granuleBits();
    object->clear(*all[2]);
    object->clear(*all[4]);
    AOT_TEST_CHECK(object->extension() == extension && extension->granuleBits() == bits);
    AOT_TEST_CHECK(!object->isInSlot(*all[2]) && object->slotOf(*all[2]) && !object->isInSlot(*all[4]) && object->read(*all[6]) == JSValue(object));
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<extensionTests, programText, body>;

std::span<const NativeTest> extensionTests()
{
    static constexpr NativeTest tests[] = {
        { "extension/an-object-pays-for-the-fields-it-has"_s, withProgram<checkAnObjectPaysForTheFieldsItHas> },
        { "extension/fields-are-added-in-every-order"_s, withProgram<checkFieldsAreAddedInEveryOrder> },
        { "extension/a-granule-goes-in-place-only-behind-the-others"_s, withProgram<checkAGranuleGoesInPlaceOnlyBehindTheOthers> },
        { "extension/the-collector-finds-references-by-the-bits"_s, withProgram<checkTheCollectorFindsReferencesByTheBits> },
        { "extension/a-cleared-field-keeps-its-granule"_s, withProgram<checkAClearedFieldKeepsItsGranule> },
        { "extension/narrow-references/an-object-pays-for-the-fields-it-has"_s, runNativeTestWithNarrowReferences<checkAnObjectPaysForTheFieldsItHas> },
        { "extension/narrow-references/fields-are-added-in-every-order"_s, runNativeTestWithNarrowReferences<checkFieldsAreAddedInEveryOrder> },
        { "extension/narrow-references/a-granule-goes-in-place-only-behind-the-others"_s, runNativeTestWithNarrowReferences<checkAGranuleGoesInPlaceOnlyBehindTheOthers> },
        { "extension/narrow-references/the-collector-finds-references-by-the-bits"_s, runNativeTestWithNarrowReferences<checkTheCollectorFindsReferencesByTheBits> },
        { "extension/narrow-references/a-cleared-field-keeps-its-granule"_s, runNativeTestWithNarrowReferences<checkAClearedFieldKeepsItsGranule> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
