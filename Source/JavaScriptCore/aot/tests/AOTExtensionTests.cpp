/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTOperationCallScope.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTVector.h"
#include "HeapCellInlines.h"
#include "JSCInlines.h"
#include "MarkedBlockCage.h"
#include "ObjectConstructor.h"
#include "PropertyNameArray.h"
#include "StrongInlines.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> extensionTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Wide" fields=[{key="flags" declared={type=number} flags=[IsRequired]}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = NewClosure function=f5
    v7 = Call v1, -, v2, v3, v4, v5, v6
    Return
}
function f1 name="make" parameters=[{type=number}, {type=anything}] results=[{type=anything}] {
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
    PropSet v0, v0 key="late24"
    PropSet v0, v0 key="late25"
    PropSet v0, v0 key="late26"
    PropSet v0, v0 key="late27"
    PropSet v0, v0 key="late28"
    PropSet v0, v0 key="late29"
    PropSet v0, v0 key="late30"
    PropSet v0, v0 key="late31"
    PropSet v0, v0 key="late32"
    PropSet v0, v0 key="late33"
    PropSet v0, v0 key="late34"
    PropSet v0, v0 key="late35"
    PropSet v0, v0 key="late36"
    PropSet v0, v0 key="late37"
    PropSet v0, v0 key="late38"
    PropSet v0, v0 key="late39"
    PropSet v0, v0 key="late40"
    PropSet v0, v0 key="late41"
    PropSet v0, v0 key="late42"
    PropSet v0, v0 key="late43"
    PropSet v0, v0 key="late44"
    PropSet v0, v0 key="late45"
    PropSet v0, v0 key="late46"
    PropSet v0, v0 key="late47"
    PropSet v0, v0 key="wide0"
    PropSet v0, v0 key="wide1"
    PropSet v0, v0 key="wide2"
    PropSet v0, v0 key="wide3"
    v53 = PropGet v0 key="late0"
    v54 = PropGet v0 key="late1"
    v55 = PropGet v0 key="late2"
    v56 = PropGet v0 key="late3"
    v57 = PropGet v0 key="late4"
    v58 = PropGet v0 key="late5"
    v59 = PropGet v0 key="late6"
    v60 = PropGet v0 key="late7"
    v61 = PropGet v0 key="late8"
    v62 = PropGet v0 key="late9"
    v63 = PropGet v0 key="late10"
    v64 = PropGet v0 key="late11"
    v65 = PropGet v0 key="late12"
    v66 = PropGet v0 key="late13"
    v67 = PropGet v0 key="late14"
    v68 = PropGet v0 key="late15"
    v69 = PropGet v0 key="late16"
    v70 = PropGet v0 key="late17"
    v71 = PropGet v0 key="late18"
    v72 = PropGet v0 key="late19"
    v73 = PropGet v0 key="late20"
    v74 = PropGet v0 key="late21"
    v75 = PropGet v0 key="late22"
    v76 = PropGet v0 key="late23"
    v77 = PropGet v0 key="late24"
    v78 = PropGet v0 key="late25"
    v79 = PropGet v0 key="late26"
    v80 = PropGet v0 key="late27"
    v81 = PropGet v0 key="late28"
    v82 = PropGet v0 key="late29"
    v83 = PropGet v0 key="late30"
    v84 = PropGet v0 key="late31"
    v85 = PropGet v0 key="late32"
    v86 = PropGet v0 key="late33"
    v87 = PropGet v0 key="late34"
    v88 = PropGet v0 key="late35"
    v89 = PropGet v0 key="late36"
    v90 = PropGet v0 key="late37"
    v91 = PropGet v0 key="late38"
    v92 = PropGet v0 key="late39"
    v93 = PropGet v0 key="late40"
    v94 = PropGet v0 key="late41"
    v95 = PropGet v0 key="late42"
    v96 = PropGet v0 key="late43"
    v97 = PropGet v0 key="late44"
    v98 = PropGet v0 key="late45"
    v99 = PropGet v0 key="late46"
    v100 = PropGet v0 key="late47"
    v101 = PropGet v0 key="wide0"
    v102 = PropGet v0 key="wide1"
    v103 = PropGet v0 key="wide2"
    v104 = PropGet v0 key="wide3"
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
    v1 = Undefined
    PropSet v0, v1 key="late0"
    PropSet v0, v1 key="late1"
    PropSet v0, v1 key="late2"
    PropSet v0, v1 key="late3"
    PropSet v0, v1 key="late4"
    PropSet v0, v1 key="late5"
    PropSet v0, v1 key="late6"
    PropSet v0, v1 key="late7"
    PropSet v0, v1 key="late8"
    PropSet v0, v1 key="late9"
    PropSet v0, v1 key="late10"
    PropSet v0, v1 key="late11"
    PropSet v0, v1 key="late12"
    PropSet v0, v1 key="late13"
    PropSet v0, v1 key="late14"
    PropSet v0, v1 key="late15"
    PropSet v0, v1 key="late16"
    PropSet v0, v1 key="late17"
    PropSet v0, v1 key="late18"
    PropSet v0, v1 key="late19"
    PropSet v0, v1 key="late20"
    PropSet v0, v1 key="late21"
    PropSet v0, v1 key="late22"
    PropSet v0, v1 key="late23"
    PropSet v0, v1 key="late24"
    PropSet v0, v1 key="late25"
    PropSet v0, v1 key="late26"
    PropSet v0, v1 key="late27"
    PropSet v0, v1 key="late28"
    PropSet v0, v1 key="late29"
    PropSet v0, v1 key="late30"
    PropSet v0, v1 key="late31"
    PropSet v0, v1 key="late32"
    PropSet v0, v1 key="late33"
    PropSet v0, v1 key="late34"
    PropSet v0, v1 key="late35"
    PropSet v0, v1 key="late36"
    PropSet v0, v1 key="late37"
    PropSet v0, v1 key="late38"
    PropSet v0, v1 key="late39"
    PropSet v0, v1 key="late40"
    PropSet v0, v1 key="late41"
    PropSet v0, v1 key="late42"
    PropSet v0, v1 key="late43"
    PropSet v0, v1 key="late44"
    PropSet v0, v1 key="late45"
    PropSet v0, v1 key="late46"
    PropSet v0, v1 key="late47"
    Return
}
function f4 name="seed" parameters=[{type=number}] results=[{type=anything}] {
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    PropSet v2, v2 key="late0"
    PropSet v2, v2 key="late1"
    PropSet v2, v2 key="late2"
    PropSet v2, v2 key="late3"
    PropSet v2, v2 key="late4"
    PropSet v2, v2 key="late5"
    PropSet v2, v2 key="late6"
    PropSet v2, v2 key="late7"
    PropSet v2, v2 key="late8"
    PropSet v2, v2 key="late9"
    PropSet v2, v2 key="late10"
    PropSet v2, v2 key="late11"
    PropSet v2, v2 key="late12"
    PropSet v2, v2 key="late13"
    PropSet v2, v2 key="late14"
    PropSet v2, v2 key="late15"
    PropSet v2, v2 key="late16"
    PropSet v2, v2 key="late17"
    PropSet v2, v2 key="late18"
    PropSet v2, v2 key="late19"
    PropSet v2, v2 key="late20"
    PropSet v2, v2 key="late21"
    PropSet v2, v2 key="late22"
    PropSet v2, v2 key="late23"
    PropSet v2, v2 key="late24"
    PropSet v2, v2 key="late25"
    PropSet v2, v2 key="late26"
    PropSet v2, v2 key="late27"
    PropSet v2, v2 key="late28"
    PropSet v2, v2 key="late29"
    PropSet v2, v2 key="late30"
    PropSet v2, v2 key="late31"
    PropSet v2, v2 key="late32"
    PropSet v2, v2 key="late33"
    PropSet v2, v2 key="late34"
    PropSet v2, v2 key="late35"
    PropSet v2, v2 key="late36"
    PropSet v2, v2 key="late37"
    PropSet v2, v2 key="late38"
    PropSet v2, v2 key="late39"
    PropSet v2, v2 key="late40"
    PropSet v2, v2 key="late41"
    PropSet v2, v2 key="late42"
    PropSet v2, v2 key="late43"
    PropSet v2, v2 key="late44"
    PropSet v2, v2 key="late45"
    PropSet v2, v2 key="late46"
    PropSet v2, v2 key="late47"
    PropSet v2, v2 key="wide0"
    PropSet v2, v2 key="wide1"
    PropSet v2, v2 key="wide2"
    PropSet v2, v2 key="wide3"
    Return v2
}
function f5 name="empty" parameters=[{type=number}] results=[{type=anything}] {
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    v3 = Undefined
    PropSet v2, v3 key="late0"
    PropSet v2, v3 key="late1"
    PropSet v2, v3 key="late2"
    PropSet v2, v3 key="late3"
    PropSet v2, v3 key="late4"
    PropSet v2, v3 key="late5"
    PropSet v2, v3 key="late6"
    PropSet v2, v3 key="late7"
    PropSet v2, v3 key="late8"
    PropSet v2, v3 key="late9"
    PropSet v2, v3 key="late10"
    PropSet v2, v3 key="late11"
    PropSet v2, v3 key="late12"
    PropSet v2, v3 key="late13"
    PropSet v2, v3 key="late14"
    PropSet v2, v3 key="late15"
    PropSet v2, v3 key="late16"
    PropSet v2, v3 key="late17"
    PropSet v2, v3 key="late18"
    PropSet v2, v3 key="late19"
    PropSet v2, v3 key="late20"
    PropSet v2, v3 key="late21"
    PropSet v2, v3 key="late22"
    PropSet v2, v3 key="late23"
    PropSet v2, v3 key="late24"
    PropSet v2, v3 key="late25"
    PropSet v2, v3 key="late26"
    PropSet v2, v3 key="late27"
    PropSet v2, v3 key="late28"
    PropSet v2, v3 key="late29"
    PropSet v2, v3 key="late30"
    PropSet v2, v3 key="late31"
    PropSet v2, v3 key="late32"
    PropSet v2, v3 key="late33"
    PropSet v2, v3 key="late34"
    PropSet v2, v3 key="late35"
    PropSet v2, v3 key="late36"
    PropSet v2, v3 key="late37"
    PropSet v2, v3 key="late38"
    PropSet v2, v3 key="late39"
    PropSet v2, v3 key="late40"
    PropSet v2, v3 key="late41"
    PropSet v2, v3 key="late42"
    PropSet v2, v3 key="late43"
    PropSet v2, v3 key="late44"
    PropSet v2, v3 key="late45"
    PropSet v2, v3 key="late46"
    PropSet v2, v3 key="late47"
    PropSet v2, v3 key="wide0"
    PropSet v2, v3 key="wide1"
    PropSet v2, v3 key="wide2"
    PropSet v2, v3 key="wide3"
    Return v2
}
)IR"_s;

template<LoadedProgramTest body>
static void runNativeTestWithNarrowReferences()
{
    for (const NativeTest& test : extensionTests()) {
        if (test.run == &runNativeTestWithNarrowReferences<body>) {
            runWithLoadedProgram(test.name, programText, body, { "references=narrow"_s });
            return;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<LoadedProgramTest body>
static void runNativeTestWithWideReferences()
{
    for (const NativeTest& test : extensionTests()) {
        if (test.run == &runNativeTestWithWideReferences<body>) {
            runWithLoadedProgram(test.name, programText, body, { "references=wide"_s });
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
    std::span<const FieldData* const> fieldsOutOfLine() const { return m_fieldsOutOfLine.span(); }
    Object* create() { return uncheckedDowncast<Object>(m_program.state().objects->structures.allocate(vm(), m_allocation)); }
    void letFieldsExistWithoutStorage();
    bool admitsUndefined(const FieldData& field) const { return m_program.state().objects->layouts.slotAdmits(field, jsUndefined()); }
    void letFirstExtensionHold(uint64_t granuleBits);

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

void Fixture::letFieldsExistWithoutStorage()
{
    constexpr size_t wordOfFlags = offsetof(FieldData, flags) / sizeof(uint32_t);
    const RuntimeLayoutTable& layouts = m_program.state().objects->layouts;
    for (const FieldData* field : m_fieldsOutOfLine) {
        if (!layouts.slotAdmits(*field, jsUndefined()))
            continue;
        FieldData marked = *field;
        marked.flags |= fieldMayExistWithoutStorage;
        overwriteWordInImage(reinterpret_cast<const uint32_t*>(field)[wordOfFlags], std::bit_cast<std::array<uint32_t, sizeof(FieldData) / sizeof(uint32_t)>>(marked)[wordOfFlags]);
    }
}

void Fixture::letFirstExtensionHold(uint64_t granuleBits)
{
    const ExtensionLayoutData& data = m_layout->extensionData();
    overwriteWordInImage(data.granulesOfFirstExtensionLow, static_cast<uint32_t>(granuleBits));
    overwriteWordInImage(data.granulesOfFirstExtensionHigh, static_cast<uint32_t>(granuleBits >> 32));
    AOT_TEST_CHECK(m_layout->granulesOfFirstExtension() == granuleBits);
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

    object->ensureExtensionGranules(vm, bit(0), collectorMayVisit);
    Extension* larger = object->extension();
    AOT_TEST_CHECK(larger != extension && larger->granuleBits() == (bit(0) | bit(5) | bit(6) | bit(7) | bit(9) | bit(11)));
    AOT_TEST_CHECK(!larger->granules()[0] && larger->granules()[1] == 55 && !larger->granules()[2] && larger->granules()[3] == 77 && larger->granules()[4] == 99 && larger->granules()[5] == 111);
    AOT_TEST_CHECK(extension->granuleBits() == (bit(5) | bit(6) | bit(7) | bit(9) | bit(11)) && extension->granules()[4] == 111);

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
    fixture.letFieldsExistWithoutStorage();
    bool referencesAreNarrow = !Image::current()->header().options.useWideReferences();
    for (const FieldData* field : fixture.fieldsOutOfLine()) {
        if (field->flags & fieldMayExistWithoutStorage && fixture.admitsUndefined(*field) && traitsOf(field->slot.representation()).isNarrowReference == referencesAreNarrow)
            return *field;
    }
    failNativeTest(__FILE__, __LINE__, "a field out of line admits undefined in a slot as wide as the references of the program");
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

    Object* listed = fixture.create();
    callOperationFromTest(vm, operationAOTDidCreateFieldAt, &program.instance(), listed, slotKeyFor(field.slot.offset, true));
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

static void checkTheGranuleOfANeighborTakesNoFieldAway(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    State& state = program.state();
    fixture.letFieldsExistWithoutStorage();
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

namespace {

struct FieldsOfMakersAndStranger {
    explicit FieldsOfMakersAndStranger(Fixture&);

    Vector<const FieldData*> ofMakers;
    const FieldData* ofStranger { nullptr };
    uint64_t granulesOfMakers { 0 };
    uint64_t granuleOfStranger { 0 };
};

} // anonymous namespace

FieldsOfMakersAndStranger::FieldsOfMakersAndStranger(Fixture& fixture)
{
    AOT_TEST_CHECK(!fixture.layout().granulesOfFirstExtension());
    for (const FieldData* field : fixture.fieldsOutOfLine()) {
        uint64_t granuleBit = granuleBitOf(field->slot.offset);
        if ((granulesOfMakers | granuleOfStranger) & granuleBit || ofMakers.size() == 5)
            continue;
        if (ofMakers.size() == 2 && !ofStranger) {
            ofStranger = field;
            granuleOfStranger = granuleBit;
            continue;
        }
        ofMakers.append(field);
        granulesOfMakers |= granuleBit;
    }
    AOT_TEST_CHECK(ofMakers.size() == 5 && ofStranger);
    fixture.letFirstExtensionHold(granulesOfMakers);
}

static void checkTheFirstExtensionHoldsWhatTheMakersFill(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    FieldsOfMakersAndStranger fields(fixture);
    static_assert(extensionCapacityFor(1) == 1 && extensionCapacityFor(5) == 5);

    Strong<JSObject> owner(vm, fixture.create());
    Object* object = uncheckedDowncast<Object>(owner.get());
    AOT_TEST_CHECK(!object->extension() && object->tryStore(vm, *fields.ofMakers[3], object));
    Extension* first = object->extension();
    AOT_TEST_CHECK(first && first->granuleBits() == fields.granulesOfMakers);
    AOT_TEST_CHECK(first->sizeInBytes() == extensionCellSizeFor(5) && reinterpret_cast<HeapCell*>(first)->cellSize() >= extensionCellSizeFor(5));
    for (const FieldData* field : fields.ofMakers) {
        bool isStored = field == fields.ofMakers[3];
        AOT_TEST_CHECK(object->slotOf(*field) && object->isInSlot(*field) == isStored);
        AOT_TEST_CHECK(isStored ? object->read(*field) == JSValue(object) : !object->read(*field));
    }
    AOT_TEST_CHECK(!object->slotOf(*fields.ofStranger) && !object->read(*fields.ofStranger));
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(object->extension() == first && vm.heap.isMarked(reinterpret_cast<HeapCell*>(first)));

    for (unsigned index : { 1u, 4u, 0u, 2u }) {
        AOT_TEST_CHECK(object->tryStore(vm, *fields.ofMakers[index], object));
        AOT_TEST_CHECK(object->extension() == first && first->granuleBits() == fields.granulesOfMakers);
    }
    for (const FieldData* field : fields.ofMakers)
        AOT_TEST_CHECK(object->read(*field) == JSValue(object));

    AOT_TEST_CHECK(object->tryStore(vm, *fields.ofStranger, object));
    Extension* grown = object->extension();
    AOT_TEST_CHECK(grown != first && grown->granuleBits() == (fields.granulesOfMakers | fields.granuleOfStranger));
    AOT_TEST_CHECK(object->read(*fields.ofStranger) == JSValue(object));
    for (const FieldData* field : fields.ofMakers)
        AOT_TEST_CHECK(object->read(*field) == JSValue(object));
}

static void checkWhoeverAsksFirstMakesTheSameExtension(LoadedProgram& program)
{
    Fixture fixture(program);
    VM& vm = fixture.vm();
    FieldsOfMakersAndStranger fields(fixture);
    for (const FieldData* field : fixture.fieldsOutOfLine()) {
        uint64_t granuleBit = granuleBitOf(field->slot.offset);
        Object* filledByOperation = fixture.create();
        callOperationFromTest(vm, operationAOTEnsureExtensionGranules, &program.instance(), filledByOperation, granuleBit);
        Object* filledByStore = fixture.create();
        AOT_TEST_CHECK(filledByStore->tryStore(vm, *field, filledByStore));
        Object* filledByCreation = fixture.create();
        AOT_TEST_CHECK(tryCreateField(vm, program.state(), *filledByCreation, *field, filledByCreation));
        for (Object* object : { filledByOperation, filledByStore, filledByCreation })
            AOT_TEST_CHECK(object->extension() && object->extension()->granuleBits() == (granuleBit | fields.granulesOfMakers));
    }
    AOT_TEST_CHECK(!fixture.create()->extension());
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithWideReferences<body>;

static constexpr ASCIILiteral noNarrowReferences = "plan/AOTSlotPlan.cpp: narrow references in slots: no field lies in a Cell32 or an OptionalCell32"_s;

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
        { "extension/the-first-extension-holds-what-the-makers-fill"_s, withProgram<checkTheFirstExtensionHoldsWhatTheMakersFill> },
        { "extension/whoever-asks-first-makes-the-same-extension"_s, withProgram<checkWhoeverAsksFirstMakesTheSameExtension> },
        { "extension/narrow-references/an-object-pays-for-the-fields-it-has"_s, runNativeTestWithNarrowReferences<checkAnObjectPaysForTheFieldsItHas> },
        { "extension/narrow-references/fields-are-added-in-every-order"_s, runNativeTestWithNarrowReferences<checkFieldsAreAddedInEveryOrder> },
        { "extension/narrow-references/a-granule-goes-in-place-only-behind-the-others"_s, runNativeTestWithNarrowReferences<checkAGranuleGoesInPlaceOnlyBehindTheOthers> },
        { "extension/narrow-references/a-granule-goes-between-the-others-while-the-collector-visits-nothing"_s, runNativeTestWithNarrowReferences<checkAGranuleGoesBetweenTheOthersWhileTheCollectorVisitsNothing> },
        { "extension/narrow-references/the-collector-finds-references-by-the-bits"_s, runNativeTestWithNarrowReferences<checkTheCollectorFindsReferencesByTheBits> },
        { "extension/narrow-references/a-cleared-field-keeps-its-granule"_s, runNativeTestWithNarrowReferences<checkAClearedFieldKeepsItsGranule> },
        { "extension/narrow-references/a-store-of-undefined-needs-no-storage"_s, runNativeTestWithNarrowReferences<checkAStoreOfUndefinedNeedsNoStorage>, noNarrowReferences },
        { "extension/narrow-references/a-read-of-an-empty-slot-is-cached"_s, runNativeTestWithNarrowReferences<checkAReadOfAnEmptySlotIsCached>, noNarrowReferences },
        { "extension/narrow-references/the-granule-of-a-neighbor-takes-no-field-away"_s, runNativeTestWithNarrowReferences<checkTheGranuleOfANeighborTakesNoFieldAway>, noNarrowReferences },
        { "extension/narrow-references/the-first-extension-holds-what-the-makers-fill"_s, runNativeTestWithNarrowReferences<checkTheFirstExtensionHoldsWhatTheMakersFill> },
        { "extension/narrow-references/whoever-asks-first-makes-the-same-extension"_s, runNativeTestWithNarrowReferences<checkWhoeverAsksFirstMakesTheSameExtension> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
