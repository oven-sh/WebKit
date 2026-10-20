/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONTestSupport.h"

#if ENABLE(AOT)

#include "AOTJSONValueParser.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTRuntimeCounters.h"
#include "AOTSchemaRuntimeOperations.h"
#include "PropertyNameArray.h"
#include <algorithm>
#include <initializer_list>
#include <wtf/Noncopyable.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

using Typed::JSONOutcome;

std::span<const NativeTest> jsonKeyOrderTests();

static constexpr unsigned timesParsed = 3;
static constexpr unsigned numberOfRowFields = 20;

#define AOT_ROW_FIELD(number) "{key=\"k" #number "\" declared={type=anything} flags=[IsRequired]}"

static constexpr ASCIILiteral rowProgramText =
    "layout L0 name=\"Row\" module=m0 fields=["
    AOT_ROW_FIELD(0) ", " AOT_ROW_FIELD(1) ", " AOT_ROW_FIELD(2) ", " AOT_ROW_FIELD(3) ", " AOT_ROW_FIELD(4) ", "
    AOT_ROW_FIELD(5) ", " AOT_ROW_FIELD(6) ", " AOT_ROW_FIELD(7) ", " AOT_ROW_FIELD(8) ", " AOT_ROW_FIELD(9) ", "
    AOT_ROW_FIELD(10) ", " AOT_ROW_FIELD(11) ", " AOT_ROW_FIELD(12) ", " AOT_ROW_FIELD(13) ", " AOT_ROW_FIELD(14) ", "
    AOT_ROW_FIELD(15) ", " AOT_ROW_FIELD(16) ", " AOT_ROW_FIELD(17) ", " AOT_ROW_FIELD(18) ", " AOT_ROW_FIELD(19) "]\n"
    "layout L1 name=\"Unlisted\" module=m0 fields=[{key=\"a\" declared={type=anything} flags=[IsRequired]}, {key=\"b\" declared={type=anything} flags=[IsRequired]}]\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Integer integer=1\n"
    "    v1 = NewObject -, v0, v0, v0, v0, v0, v0, v0, v0, v0, v0, v0, v0, v0, v0, v0, v0, v0, v0, v0, v0 layout=L0 fields=[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19]\n"
    "    v2 = Builtin builtin=GlobalThis\n"
    "    PropSet v2, v1 key=\"row\"\n"
    "    v4 = NewObject -, v0, v0 layout=L1 fields=[0, 1]\n"
    "    v5 = PropGet v4 key=\"a\"\n"
    "    v6 = PropGet v4 key=\"b\"\n"
    "    v7 = Builtin builtin=MathRandom\n"
    "    v8 = Call v7\n"
    "    v9 = ToString v8\n"
    "    ElemSet v1, v9, v0 mode=Strict\n"
    "    Return v4\n"
    "}\n"_s;

#undef AOT_ROW_FIELD

namespace {

class Row {
    WTF_MAKE_NONCOPYABLE(Row);
public:
    explicit Row(LoadedProgram&);

    VM& vm() { return m_program.vm(); }
    JSGlobalObject* realm() { return m_program.realm(); }
    Instance& instance() { return m_program.instance(); }
    ObjectModel& objects() { return *m_program.state().objects; }
    const LayoutData& layout() const { return *m_layout; }
    uint32_t orderWordOfAllocation() const { return m_allocation->orderWord; }
    std::span<const uint16_t> fields() const { return m_fields.span(); }
    Vector<uint16_t> fieldsStartingWith(std::initializer_list<unsigned> declaredIndices) const;

    JSONKeyOrder advance(JSONKeyOrder, uint32_t field);
    JSONKeyOrder orderAfter(std::span<const uint16_t> fields);
    std::span<const uint16_t> entriesOf(JSONKeyOrder);

    Object* createObject();
    Object* createObjectWithEveryField();
    bool hasEveryField(Object*);
    JSONOutcome finish(Object*, JSValue extra, JSONKeyOrder);
    void addExtraMember(const String& name, JSValue, uint64_t* extraAndOrder);
    const uint8_t* parseExtraMembers(const uint8_t* quote, uint64_t* extraAndOrder);
    const uint8_t* parseMember(Object*, const uint8_t* quote, uint64_t* extraAndOrder);
    String stringifyAfterParsing(const String& text);
    Vector<uint16_t> fieldsInOrderOfKeys(Object*);

private:
    LoadedProgram& m_program;
    const AllocationData* m_allocation { nullptr };
    const LayoutData* m_layout { nullptr };
    Vector<uint16_t> m_fields;
};

Row::Row(LoadedProgram& program)
    : m_program(program)
{
    const ProgramTables& tables = instance().tables();
    for (const AllocationData& allocation : tables.allocations()) {
        if (!tables.hasLayoutForShape(allocation.shape) || tables.layoutForShape(allocation.shape).numberOfFields != numberOfRowFields)
            continue;
        AOT_TEST_CHECK(!m_allocation);
        m_allocation = &allocation;
        m_layout = &tables.layoutForShape(allocation.shape);
    }
    AOT_TEST_CHECK(m_allocation);
    AOT_TEST_CHECK(!m_layout->keysAreNeverEnumerated);
    AOT_TEST_CHECK(!(m_allocation->orderWord & ~orderAndCountMask) && countOf(m_allocation->orderWord) == numberOfRowFields);

    for (unsigned i = 0; i < numberOfRowFields; ++i) {
        const FieldData* field = objects().layouts.field(vm(), layout(), Identifier::fromString(vm(), makeString('k', i)).impl());
        AOT_TEST_CHECK(field);
        m_fields.append(layout().indexOf(*field));
    }
}

Vector<uint16_t> Row::fieldsStartingWith(std::initializer_list<unsigned> declaredIndices) const
{
    Vector<uint16_t> result;
    for (unsigned index : declaredIndices)
        result.append(m_fields[index]);
    for (uint16_t field : m_fields) {
        if (!result.contains(field))
            result.append(field);
    }
    return result;
}

JSONKeyOrder Row::advance(JSONKeyOrder order, uint32_t field)
{
    return JSONKeyOrder::fromWord(operationAOTJSONObjectKey(&instance(), order.word(), layout().shape, field));
}

JSONKeyOrder Row::orderAfter(std::span<const uint16_t> fields)
{
    JSONKeyOrder order;
    for (uint16_t field : fields)
        order = advance(order, field);
    return order;
}

std::span<const uint16_t> Row::entriesOf(JSONKeyOrder order)
{
    switch (order.kind()) {
    case JSONKeyOrder::Kind::Numbered:
        return objects().keyOrders.entriesInOrder(layout(), orderOf(order.orderAndCount())).first(countOf(order.orderAndCount()));
    case JSONKeyOrder::Kind::Listed:
        return instance().json.runtime->keyOrderLists().entries(order.list()).first(order.countInList());
    case JSONKeyOrder::Kind::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Object* Row::createObject()
{
    return Object::create(vm(), objects().structures.objectStructure(vm(), layout().shape), layout(), m_allocation->size, unorderedOrderWord);
}

Object* Row::createObjectWithEveryField()
{
    Object* object = createObject();
    for (uint16_t field : m_fields)
        AOT_TEST_CHECK(object->store(vm(), layout().fields()[field], jsNumber(field + 100)));
    return object;
}

bool Row::hasEveryField(Object* object)
{
    return std::ranges::all_of(m_fields, [&](uint16_t field) {
        return object->read(layout().fields()[field]) == jsNumber(field + 100);
    });
}

JSONOutcome Row::finish(Object* object, JSValue extra, JSONKeyOrder order)
{
    OperationCallScope scope(vm());
    return static_cast<JSONOutcome>(operationAOTJSONObjectFinish(&instance(), object, JSValue::encode(extra), order.word()));
}

void Row::addExtraMember(const String& name, JSValue value, uint64_t* extraAndOrder)
{
    JSString* nameCell = jsString(vm(), name);
    OperationCallScope scope(vm());
    operationAOTJSONAddExtraMember(&instance(), layout().shape, nameCell, JSValue::encode(value), extraAndOrder);
}

const uint8_t* Row::parseExtraMembers(const uint8_t* quote, uint64_t* extraAndOrder)
{
    OperationCallScope scope(vm());
    return operationAOTJSONParseExtraMembers(&instance(), layout().shape, quote, extraAndOrder);
}

const uint8_t* Row::parseMember(Object* object, const uint8_t* quote, uint64_t* extraAndOrder)
{
    JSONValueParser parser(instance(), vm(), realm());
    JSONValueParser::ParsedName name = parser.parseNameAndColon(quote);
    AOT_TEST_CHECK(name.afterColon);
    const FieldData* field = objects().layouts.field(vm(), layout(), name.name.impl());
    if (!field)
        return parseExtraMembers(quote, extraAndOrder);
    ParsedJSONValue value = parser.parse(name.afterColon);
    AOT_TEST_CHECK(value.end && object->store(vm(), *field, value.value));
    extraAndOrder[1] = advance(JSONKeyOrder::fromWord(extraAndOrder[1]), layout().indexOf(*field)).word();
    return value.end;
}

String Row::stringifyAfterParsing(const String& text)
{
    JSONSourceScope source(instance(), StringView(text));
    AOT_TEST_CHECK(!!source && *source.start() == '{');
    Object* object = createObject();
    uint64_t extraAndOrder[2] = { static_cast<uint64_t>(JSValue::encode(jsUndefined())), JSONKeyOrder().word() };
    const uint8_t* cursor = source.start();
    do {
        cursor = parseMember(object, cursor + 1, extraAndOrder);
        AOT_TEST_CHECK(cursor && (*cursor == ',' || *cursor == '}'));
    } while (*cursor == ',');
    AOT_TEST_CHECK(finish(object, JSValue::decode(extraAndOrder[0]), JSONKeyOrder::fromWord(extraAndOrder[1])) == JSONOutcome::Parsed);
    return JSONStringify(realm(), object, 0u);
}

Vector<uint16_t> Row::fieldsInOrderOfKeys(Object* object)
{
    PropertyNameArrayBuilder names(vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, realm(), names, DontEnumPropertiesMode::Exclude);
    Vector<uint16_t> result;
    for (const Identifier& name : names) {
        const FieldData* field = objects().layouts.field(vm(), layout(), name.impl());
        AOT_TEST_CHECK(field);
        result.append(layout().indexOf(*field));
    }
    return result;
}

} // anonymous namespace

static void testDeclaredOrder(LoadedProgram& program)
{
    Row row(program);
    JSONKeyOrder order;
    for (unsigned i = 0; i < numberOfRowFields; ++i) {
        order = row.advance(order, row.fields()[i]);
        AOT_TEST_CHECK(order.kind() == JSONKeyOrder::Kind::Numbered);
        AOT_TEST_CHECK(equalSpans(row.entriesOf(order), row.fields().first(i + 1)));
    }
    AOT_TEST_CHECK(order.word() == row.orderWordOfAllocation());
    AOT_TEST_CHECK(row.orderAfter(row.fields()) == order);

    Object* object = row.createObjectWithEveryField();
    AOT_TEST_CHECK(row.finish(object, jsUndefined(), order) == JSONOutcome::Parsed);
    AOT_TEST_CHECK(object->orderWord() == row.orderWordOfAllocation());
    AOT_TEST_CHECK(row.hasEveryField(object));
    AOT_TEST_CHECK(equalSpans(row.fieldsInOrderOfKeys(object).span(), row.fields()));
}

static void testOtherOrders(LoadedProgram& program)
{
    Row row(program);
    Vector<uint16_t> reversed(row.fields());
    reversed.reverse();
    Vector<Vector<uint16_t>> sequences { reversed, row.fieldsStartingWith({ 1, 0 }), row.fieldsStartingWith({ 0, 1, 2, 19 }), row.fieldsStartingWith({ 7 }) };
    Vector<uint64_t> words { row.orderWordOfAllocation() };
    for (const Vector<uint16_t>& sequence : sequences) {
        JSONKeyOrder order = row.orderAfter(sequence.span());
        AOT_TEST_CHECK(order.kind() == JSONKeyOrder::Kind::Numbered);
        AOT_TEST_CHECK(equalSpans(row.entriesOf(order), sequence.span()));
        AOT_TEST_CHECK(!words.contains(order.word()));
        words.append(order.word());
    }
    for (size_t i = 0; i < sequences.size(); ++i)
        AOT_TEST_CHECK(row.orderAfter(sequences[i].span()).word() == words[i + 1]);
    AOT_TEST_CHECK(row.orderAfter(row.fields()).word() == row.orderWordOfAllocation());

    Object* object = row.createObjectWithEveryField();
    AOT_TEST_CHECK(row.finish(object, jsUndefined(), row.orderAfter(reversed.span())) == JSONOutcome::Parsed);
    AOT_TEST_CHECK(!object->hasOutOfLineKeyOrder() && row.hasEveryField(object));
    AOT_TEST_CHECK(equalSpans(row.fieldsInOrderOfKeys(object).span(), reversed.span()));

    Vector<uint16_t> someFields { row.fields()[5], row.fields()[2] };
    Object* partialObject = row.createObject();
    for (uint16_t field : someFields)
        AOT_TEST_CHECK(partialObject->store(row.vm(), row.layout().fields()[field], jsNumber(1)));
    AOT_TEST_CHECK(row.finish(partialObject, jsUndefined(), row.orderAfter(someFields.span())) == JSONOutcome::Parsed);
    AOT_TEST_CHECK(equalSpans(row.fieldsInOrderOfKeys(partialObject).span(), someFields.span()));
}

static void testUnknownOrder(LoadedProgram& program)
{
    Row row(program);
    JSONKeyOrder order = row.advance(row.advance({ }, row.fields()[0]), numberOfRowFields);
    AOT_TEST_CHECK(order.kind() == JSONKeyOrder::Kind::Unknown);
    AOT_TEST_CHECK(row.advance(order, row.fields()[1]) == order);

    uint64_t fallbacksBefore = valueOf(row.vm(), RuntimeCounterID::TypedParsesFellBackPerValue);
    Object* object = row.createObjectWithEveryField();
    AOT_TEST_CHECK(row.finish(object, jsUndefined(), order) == JSONOutcome::Unknown);
    AOT_TEST_CHECK(valueOf(row.vm(), RuntimeCounterID::TypedParsesFellBackPerValue) == fallbacksBefore + 1);
    AOT_TEST_CHECK(!row.vm().exceptionForInspection());
}

static void testExtraMembers(LoadedProgram& program)
{
    static constexpr ASCIILiteral texts[] = {
        "{\"k0\":0,\"x\":1,\"y\":2,\"k1\":3,\"z\":4,\"k2\":5}"_s,
        "{\"x\":1,\"k0\":0}"_s,
        "{\"k0\":0,\"x\":1}"_s,
        "{\"x\":1,\"y\":2}"_s,
        "{\"y\":1,\"k3\":2,\"x\":3}"_s,
        "{\"k1\":1,\"x\":[1,{\"a\":2}],\"k0\":0,\"y\":\"s\",\"w\":null}"_s,
        "{\"x\":1,\"k0\":0,\"x\":2}"_s,
        "{\"k0\":0,\"7\":1,\"k1\":2,\"3\":4}"_s,
        "{\"k0\":0,\"__proto__\":1,\"k1\":2}"_s,
        "{\"k0\":0,\"\\u0078\":1,\"k1\":2}"_s,
    };
    Row row(program);
    auto checkText = [&](const String& text) {
        String expected = JSONStringify(row.realm(), JSONParse(row.realm(), text), 0u);
        AOT_TEST_CHECK(!expected.isNull());
        for (unsigned i = 0; i < timesParsed; ++i)
            AOT_TEST_CHECK(row.stringifyAfterParsing(text) == expected);
    };
    for (unsigned round = 0; round < 2; ++round) {
        for (ASCIILiteral text : texts)
            checkText(String(text));
    }

    StringBuilder everyFieldAndExtraMembers;
    for (unsigned i = 0; i < numberOfRowFields; ++i) {
        everyFieldAndExtraMembers.append(i ? ","_s : "{"_s, "\"k"_s, numberOfRowFields - 1 - i, "\":"_s, i);
        if (!(i % 3))
            everyFieldAndExtraMembers.append(",\"e"_s, i, "\":"_s, i, ",\"f"_s, i, "\":true"_s);
    }
    everyFieldAndExtraMembers.append('}');
    checkText(everyFieldAndExtraMembers.toString());
}

static void testAddExtraMember(LoadedProgram& program)
{
    Row row(program);
    Object* object = row.createObject();
    uint64_t extraAndOrder[2] = { static_cast<uint64_t>(JSValue::encode(jsUndefined())), JSONKeyOrder().word() };
    auto addField = [&](unsigned declaredIndex, int32_t value) {
        uint16_t field = row.fields()[declaredIndex];
        AOT_TEST_CHECK(object->store(row.vm(), row.layout().fields()[field], jsNumber(value)));
        extraAndOrder[1] = row.advance(JSONKeyOrder::fromWord(extraAndOrder[1]), field).word();
    };
    addField(1, 10);
    row.addExtraMember("x"_s, jsNumber(11), extraAndOrder);
    AOT_TEST_CHECK(JSValue::decode(extraAndOrder[0]).isObject());
    row.addExtraMember("7"_s, jsNumber(12), extraAndOrder);
    addField(0, 13);
    row.addExtraMember("__proto__"_s, jsNull(), extraAndOrder);
    row.addExtraMember("x"_s, jsNumber(14), extraAndOrder);
    AOT_TEST_CHECK(row.finish(object, JSValue::decode(extraAndOrder[0]), JSONKeyOrder::fromWord(extraAndOrder[1])) == JSONOutcome::Parsed);
    JSValue expected = JSONParse(row.realm(), "{\"k1\":10,\"x\":11,\"7\":12,\"k0\":13,\"__proto__\":null,\"x\":14}"_s);
    AOT_TEST_CHECK(JSONStringify(row.realm(), object, 0u) == JSONStringify(row.realm(), expected, 0u));

    uint64_t declaredName[2] = { static_cast<uint64_t>(JSValue::encode(jsUndefined())), JSONKeyOrder().word() };
    row.addExtraMember("k2"_s, jsNumber(1), declaredName);
    AOT_TEST_CHECK(JSONKeyOrder::fromWord(declaredName[1]).kind() == JSONKeyOrder::Kind::Unknown);

    uint64_t unknownOrder[2] = { static_cast<uint64_t>(JSValue::encode(jsUndefined())), JSONKeyOrder::unknown().word() };
    row.addExtraMember("x"_s, jsNumber(1), unknownOrder);
    AOT_TEST_CHECK(JSValue::decode(unknownOrder[0]).isUndefined() && unknownOrder[1] == JSONKeyOrder::unknown().word());
    AOT_TEST_CHECK(!row.vm().exceptionForInspection());
}

static void testDeclaredNameAmongExtraMembers(LoadedProgram& program)
{
    Row row(program);
    uint64_t fallbacksBefore = valueOf(row.vm(), RuntimeCounterID::TypedParsesFellBackPerValue);
    for (ASCIILiteral text : { "\"\\u006b0\":1}"_s, "\"x\":}"_s, "\"x\" 1}"_s, "x:1}"_s }) {
        JSONSourceScope source(row.instance(), StringView(text));
        AOT_TEST_CHECK(!!source);
        uint64_t extraAndOrder[2] = { static_cast<uint64_t>(JSValue::encode(jsUndefined())), JSONKeyOrder().word() };
        AOT_TEST_CHECK(!row.parseExtraMembers(source.start(), extraAndOrder));
        AOT_TEST_CHECK(!row.vm().exceptionForInspection());
    }
    AOT_TEST_CHECK(valueOf(row.vm(), RuntimeCounterID::TypedParsesFellBackPerValue) == fallbacksBefore + 4);
}

static Vector<uint16_t> fillOrderTable(Row& row)
{
    std::span<const uint16_t> fields = row.fields();
    for (uint16_t first : fields) {
        for (uint16_t second : fields) {
            for (uint16_t third : fields) {
                for (uint16_t fourth : fields) {
                    if (first == second || first == third || first == fourth || second == third || second == fourth || third == fourth)
                        continue;
                    Vector<uint16_t> sequence { first, second, third, fourth };
                    if (row.orderAfter(sequence.span()).kind() == JSONKeyOrder::Kind::Listed)
                        return sequence;
                }
            }
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void testFullOrderTable(LoadedProgram& program)
{
    Row row(program);
    VM& vm = row.vm();
    Vector<uint16_t> reversed(row.fields());
    reversed.reverse();
    JSONKeyOrder reversedOrder = row.orderAfter(reversed.span());
    AOT_TEST_CHECK(reversedOrder.kind() == JSONKeyOrder::Kind::Numbered);
    AOT_TEST_CHECK(!limitHitsOf(vm, LimitID::KeyOrdersPerLayout));

    Vector<uint16_t> sequence = fillOrderTable(row);
    AOT_TEST_CHECK(limitHitsOf(vm, LimitID::KeyOrdersPerLayout));
    for (uint16_t field : row.fields()) {
        if (!sequence.contains(field))
            sequence.append(field);
    }
    JSONKeyOrder order = row.orderAfter(sequence.span());
    AOT_TEST_CHECK(order.kind() == JSONKeyOrder::Kind::Listed);
    AOT_TEST_CHECK(equalSpans(row.entriesOf(order), sequence.span()));
    AOT_TEST_CHECK(row.orderAfter(reversed.span()) == reversedOrder);
    AOT_TEST_CHECK(row.orderAfter(row.fields()).word() == row.orderWordOfAllocation());

    uint64_t ordersStoredOutOfLine = valueOf(vm, RuntimeCounterID::KeyOrdersStoredOutOfLine);
    Object* object = row.createObjectWithEveryField();
    AOT_TEST_CHECK(row.finish(object, jsUndefined(), order) == JSONOutcome::Parsed);
    AOT_TEST_CHECK(object->hasOutOfLineKeyOrder());
    AOT_TEST_CHECK(valueOf(vm, RuntimeCounterID::KeyOrdersStoredOutOfLine) == ordersStoredOutOfLine + 1);
    AOT_TEST_CHECK(object->shape() == row.layout().shape && row.hasEveryField(object));
    AOT_TEST_CHECK(equalSpans(row.fieldsInOrderOfKeys(object).span(), sequence.span()));
}

static void testKeysNeverEnumerated(LoadedProgram& program)
{
    Instance& instance = program.instance();
    const ProgramTables& tables = instance.tables();
    const AllocationData* allocation = nullptr;
    for (const AllocationData& candidate : tables.allocations()) {
        if (tables.hasLayoutForShape(candidate.shape) && tables.layoutForShape(candidate.shape).keysAreNeverEnumerated && tables.layoutForShape(candidate.shape).numberOfFields)
            allocation = &candidate;
    }
    AOT_TEST_CHECK(allocation && allocation->orderWord == unorderedOrderWord);
    const LayoutData& layout = tables.layoutForShape(allocation->shape);

    for (JSONKeyOrder order : { JSONKeyOrder(), JSONKeyOrder::numbered(orderWordOf(3, 1)), JSONKeyOrder::listed(0, 0), JSONKeyOrder::unknown() }) {
        for (uint32_t field = 0; field < layout.numberOfFields; ++field)
            AOT_TEST_CHECK(operationAOTJSONObjectKey(&instance, order.word(), layout.shape, field) == order.word());
    }
    AOT_TEST_CHECK(!instance.json.runtime->keyOrderLists().size());

    VM& vm = program.vm();
    Object* object = Object::create(vm, program.state().objects->structures.objectStructure(vm, layout.shape), layout, allocation->size, unorderedOrderWord);
    for (const FieldData& field : layout.fields())
        AOT_TEST_CHECK(object->store(vm, field, jsNumber(1)));
    OperationCallScope scope(vm);
    AOT_TEST_CHECK(static_cast<JSONOutcome>(operationAOTJSONObjectFinish(&instance, object, JSValue::encode(jsUndefined()), JSONKeyOrder().word())) == JSONOutcome::Parsed);
    AOT_TEST_CHECK(object->orderWord() == unorderedOrderWord);
    for (const FieldData& field : layout.fields())
        AOT_TEST_CHECK(object->read(field) == jsNumber(1));
}

template<LoadedProgramTest body>
static constexpr void (*withRowProgram)() = runNativeTestWithLoadedProgram<jsonKeyOrderTests, rowProgramText, body>;

std::span<const NativeTest> jsonKeyOrderTests()
{
    static constexpr NativeTest tests[] = {
        { "json-parser/key-orders/declared-order"_s, withRowProgram<testDeclaredOrder> },
        { "json-parser/key-orders/other-orders"_s, withRowProgram<testOtherOrders> },
        { "json-parser/key-orders/unknown-order"_s, withRowProgram<testUnknownOrder> },
        { "json-parser/key-orders/extra-members"_s, withRowProgram<testExtraMembers> },
        { "json-parser/key-orders/add-extra-member"_s, withRowProgram<testAddExtraMember> },
        { "json-parser/key-orders/declared-name-among-extra-members"_s, withRowProgram<testDeclaredNameAmongExtraMembers> },
        { "json-parser/key-orders/full-order-table"_s, withRowProgram<testFullOrderTable> },
        { "json-parser/key-orders/keys-never-enumerated"_s, withRowProgram<testKeysNeverEnumerated> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
