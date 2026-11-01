/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationCallScope.h"
#include "AOTPropertyListCache.h"
#include "AOTPropertyWalkInlines.h"
#include "AOTRuntimeCounters.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTStringBuilderStorage.h"
#include "JSCInlines.h"
#include "JSONObject.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "TopExceptionScope.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> listedFieldsTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Item" fields=[{key="id" declared={type=number} flags=[IsRequired]}, {key="title" declared={type=string} flags=[IsRequired]}, {key="payload" declared={type=anything} flags=[IsRequired]}, {key="inStock" declared={type=boolean} flags=[IsRequired]}]
layout L1 name="Bag" fields=[{key="first" declared={type=number}}, {key="second" declared={type=number}}, {key="aKeyOfMoreThanSixteenCharacters" declared={type=number}}]
layout L2 name="Escaped" fields=[{key="caf\u{e9}" declared={type=number} flags=[IsRequired]}, {key="quo\"te" declared={type=number} flags=[IsRequired]}, {key="back\\slash" declared={type=number} flags=[IsRequired]}, {key="line\nbreak" declared={type=number} flags=[IsRequired]}, {key="" declared={type=number} flags=[IsRequired]}]
layout L3 name="Wide" fields=[{key="narrow" declared={type=number} flags=[IsRequired]}, {key="\u{43a}\u{43b}\u{44e}\u{447}" declared={type=number} flags=[IsRequired]}]
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
function f1 name="makeItem" parameters=[{type=number}, {type=string}, {type=anything}, {type=boolean}] results=[{type=anything}] {
    v4 = NewObject -, v0, v1, v2, v3 layout=L0 fields=[0, 1, 2, 3]
    v5 = FinishObject v4
    Return v5
}
function f2 name="makeBag" results=[{type=anything}] {
    v0 = NewObject layout=L1 fields=[]
    v1 = FinishObject v0
    Return v1
}
function f3 name="makeEscaped" parameters=[{type=number}] results=[{type=anything}] {
    v1 = NewObject -, v0, v0, v0, v0, v0 layout=L2 fields=[0, 1, 2, 3, 4]
    v2 = FinishObject v1
    Return v2
}
function f4 name="makeWide" parameters=[{type=number}] results=[{type=anything}] {
    v1 = NewObject -, v0, v0 layout=L3 fields=[0, 1]
    v2 = FinishObject v1
    Return v2
}
function f5 name="mutate" parameters=[{type=anything}, {type=string}, {type=anything}] results=[{type=anything}] {
    ElemSet v0, v1, v2
    v4 = ElemDelete v0, v1
    v5 = Builtin builtin=ObjectFreeze
    v6 = Call v5, -, v0
    Return
}
)IR"_s;

static Object* allocate(LoadedProgram& program, ASCIILiteral layoutName)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        if (!tables.hasLayoutForShape(tables.allocations()[number].shape))
            continue;
        const LayoutData& layout = tables.layoutForShape(tables.allocations()[number].shape);
        if (Image::current()->text().equals(StringID(layout.name), layoutName))
            return uncheckedDowncast<Object>(program.state().objects->structures.allocate(program.vm(), number));
    }
    failNativeTest(__FILE__, __LINE__, layoutName.characters());
}

static Identifier identifier(LoadedProgram& program, ASCIILiteral name)
{
    return Identifier::fromString(program.vm(), name);
}

static void put(LoadedProgram& program, JSObject* object, ASCIILiteral name, JSValue value)
{
    PutPropertySlot slot(object);
    AOT_TEST_CHECK(object->methodTable()->put(object, program.realm(), identifier(program, name), value, slot));
}

static void deleteProperty(LoadedProgram& program, JSObject* object, ASCIILiteral name)
{
    DeletePropertySlot slot;
    AOT_TEST_CHECK(object->methodTable()->deleteProperty(object, program.realm(), identifier(program, name), slot));
}

static Object* createItem(LoadedProgram& program, JSValue payload)
{
    VM& vm = program.vm();
    Object* item = allocate(program, "Item"_s);
    std::span<const FieldData> fields = item->layout().fields();
    AOT_TEST_CHECK(item->store(vm, fields[0], AcceptedValue::provenAtBuildTime(jsNumber(7))));
    AOT_TEST_CHECK(item->store(vm, fields[1], AcceptedValue::provenAtBuildTime(jsNontrivialString(vm, "Product 7"_s))));
    AOT_TEST_CHECK(item->store(vm, fields[2], AcceptedValue::provenAtBuildTime(payload)));
    AOT_TEST_CHECK(item->store(vm, fields[3], AcceptedValue::provenAtBuildTime(jsBoolean(false))));
    return item;
}

static Object* createBag(LoadedProgram& program, std::initializer_list<ASCIILiteral> names)
{
    Object* bag = allocate(program, "Bag"_s);
    int32_t value = 0;
    for (ASCIILiteral name : names)
        put(program, bag, name, jsNumber(++value));
    AOT_TEST_CHECK(!bag->hasFlagsInOrderWord() && !bag->hasOutOfLineKeyOrder());
    return bag;
}

static String jsonKeysOf(const Object& object)
{
    auto fields = object.fieldsInKeyOrder();
    AOT_TEST_CHECK(fields);
    StringBuilder keys;
    for (auto& field : *fields)
        keys.append(field.jsonKey);
    return keys.toString();
}

static bool isListed(const Object& object)
{
    return object.compiledStructure().listedFields().orderAndCount == (object.orderWord() & ~orderWordIntegrityBits);
}

namespace {

enum class Path : bool { Fast, Slow };

}

static void checkStringify(LoadedProgram& program, JSValue value, Path expectedPath, const String& expectedText)
{
    VM& vm = program.vm();
    uint64_t slowPathsBefore = valueOf(vm, RuntimeCounterID::JSONStringifySlowPaths);
    String text = JSONStringify(program.realm(), value, 0);
    if (text != expectedText)
        failNativeTest(__FILE__, __LINE__, byteCast<char>(text.utf8().data()));
    AOT_TEST_CHECK(valueOf(vm, RuntimeCounterID::JSONStringifySlowPaths) - slowPathsBefore == (expectedPath == Path::Slow ? 1 : 0));
}

static void checkListIsFoundThroughStructure(LoadedProgram& program)
{
    Object* item = createItem(program, jsNull());
    AOT_TEST_CHECK(item->compiledStructure().listedFields().orderAndCount == CompiledStructure::ListedFields::noOrderWord);
    AOT_TEST_CHECK(jsonKeysOf(*item) == ",\"id\":,\"title\":,\"payload\":,\"inStock\":"_s);
    AOT_TEST_CHECK(isListed(*item));

    auto fields = *item->fieldsInKeyOrder();
    AOT_TEST_CHECK(fields.size() == 4 && fields.data() == item->compiledStructure().listedFields().fields);
    AOT_TEST_CHECK(fields.data() == createItem(program, jsNumber(1))->fieldsInKeyOrder()->data());
    for (unsigned index = 0; index < fields.size(); ++index)
        AOT_TEST_CHECK(fields[index].field == &item->layout().fields()[index] && fields[index].name == program.state().objects->layouts.nameOf(program.vm(), *fields[index].field));
}

static void checkEachKeyOrderHasItsList(LoadedProgram& program)
{
    Object* forward = createBag(program, { "first"_s, "second"_s, "aKeyOfMoreThanSixteenCharacters"_s });
    Object* backward = createBag(program, { "aKeyOfMoreThanSixteenCharacters"_s, "second"_s, "first"_s });
    Object* partial = createBag(program, { "second"_s });
    Object* empty = createBag(program, { });
    AOT_TEST_CHECK(forward->structure() == backward->structure() && forward->structure() == partial->structure());

    for (unsigned round = 0; round < 2; ++round) {
        AOT_TEST_CHECK(jsonKeysOf(*forward) == ",\"first\":,\"second\":,\"aKeyOfMoreThanSixteenCharacters\":"_s);
        AOT_TEST_CHECK(isListed(*forward) && !isListed(*backward));
        AOT_TEST_CHECK(jsonKeysOf(*backward) == ",\"aKeyOfMoreThanSixteenCharacters\":,\"second\":,\"first\":"_s);
        AOT_TEST_CHECK(isListed(*backward) && !isListed(*forward));
        AOT_TEST_CHECK(jsonKeysOf(*partial) == ",\"second\":"_s);
        AOT_TEST_CHECK(jsonKeysOf(*empty).isEmpty() && isListed(*empty));
    }

    checkStringify(program, forward, Path::Fast, "{\"first\":1,\"second\":2,\"aKeyOfMoreThanSixteenCharacters\":3}"_s);
    checkStringify(program, backward, Path::Fast, "{\"aKeyOfMoreThanSixteenCharacters\":1,\"second\":2,\"first\":3}"_s);
    checkStringify(program, partial, Path::Fast, "{\"second\":1}"_s);
    checkStringify(program, empty, Path::Fast, "{}"_s);
}

static void checkChangedObjectsLeaveTheList(LoadedProgram& program)
{
    Object* unchanged = createItem(program, jsNull());
    AOT_TEST_CHECK(unchanged->fieldsInKeyOrder() && isListed(*unchanged));

    Object* extended = createItem(program, jsNull());
    put(program, extended, "added"_s, jsNumber(1));
    AOT_TEST_CHECK(extended->hasExtraProperties() && !extended->fieldsInKeyOrder());
    checkStringify(program, extended, Path::Fast, "{\"id\":7,\"title\":\"Product 7\",\"payload\":null,\"inStock\":false,\"added\":1}"_s);

    Object* indexed = createItem(program, jsNull());
    AOT_TEST_CHECK(indexed->methodTable()->putByIndex(indexed, program.realm(), 3, jsNumber(1), false));
    AOT_TEST_CHECK(!isCompiledObject(indexed) || !indexed->fieldsInKeyOrder());
    checkStringify(program, indexed, Path::Slow, "{\"3\":1,\"id\":7,\"title\":\"Product 7\",\"payload\":null,\"inStock\":false}"_s);

    Object* shortened = createItem(program, jsNull());
    deleteProperty(program, shortened, "title"_s);
    checkStringify(program, shortened, Path::Fast, "{\"id\":7,\"payload\":null,\"inStock\":false}"_s);
    put(program, shortened, "title"_s, jsNontrivialString(program.vm(), "again"_s));
    checkStringify(program, shortened, Path::Fast, "{\"id\":7,\"payload\":null,\"inStock\":false,\"title\":\"again\"}"_s);

    Object* hidden = createItem(program, jsNull());
    PropertyDescriptor notEnumerable;
    notEnumerable.setEnumerable(false);
    AOT_TEST_CHECK(hidden->methodTable()->defineOwnProperty(hidden, program.realm(), identifier(program, "title"_s), notEnumerable, false));
    AOT_TEST_CHECK(!hidden->fieldsInKeyOrder());
    checkStringify(program, hidden, Path::Fast, "{\"id\":7,\"payload\":null,\"inStock\":false}"_s);

    Object* frozen = createItem(program, jsNull());
    frozen->setIntegrity(program.realm(), Object::Integrity::Frozen);
    AOT_TEST_CHECK(frozen->isFrozen() && frozen->fieldsInKeyOrder()->data() == unchanged->fieldsInKeyOrder()->data());
    checkStringify(program, frozen, Path::Fast, "{\"id\":7,\"title\":\"Product 7\",\"payload\":null,\"inStock\":false}"_s);
}

static Object* createFilledWithOnes(LoadedProgram& program, ASCIILiteral layoutName)
{
    Object* object = allocate(program, layoutName);
    for (const FieldData& field : object->layout().fields())
        AOT_TEST_CHECK(object->store(program.vm(), field, AcceptedValue::provenAtBuildTime(jsNumber(1))));
    return object;
}

static void checkKeyTextIsEscapedOnce(LoadedProgram& program)
{
    Object* escaped = createFilledWithOnes(program, "Escaped"_s);
    AOT_TEST_CHECK(jsonKeysOf(*escaped) == String::fromUTF8(",\"caf\xc3\xa9\":,\"quo\\\"te\":,\"back\\\\slash\":,\"line\\nbreak\":,\"\":"));
    checkStringify(program, escaped, Path::Fast, String::fromUTF8("{\"caf\xc3\xa9\":1,\"quo\\\"te\":1,\"back\\\\slash\":1,\"line\\nbreak\":1,\"\":1}"));
}

static void checkOnlyWideNameHasNoKeyText(LoadedProgram& program)
{
    Object* wide = createFilledWithOnes(program, "Wide"_s);
    auto fields = *wide->fieldsInKeyOrder();
    AOT_TEST_CHECK(fields.size() == 2 && String(fields[0].jsonKey) == ",\"narrow\":"_s && fields[1].jsonKey.empty());
    checkStringify(program, wide, Path::Slow, String::fromUTF8("{\"narrow\":1,\"\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\":1}"));
}

static void checkValuesOfEveryKind(LoadedProgram& program)
{
    VM& vm = program.vm();
    Realms& realms = program.realms();
    JSGlobalObject* realm = program.realm();
    auto check = [&](JSValue payload, Path path, ASCIILiteral member) {
        checkStringify(program, createItem(program, payload), path, makeString("{\"id\":7,\"title\":\"Product 7\""_s, member, ",\"inStock\":false}"_s));
    };

    check(jsUndefined(), Path::Fast, ""_s);
    check(jsNull(), Path::Fast, ",\"payload\":null"_s);
    check(jsBoolean(true), Path::Fast, ",\"payload\":true"_s);
    check(jsNumber(-2147483647 - 1), Path::Fast, ",\"payload\":-2147483648"_s);
    check(jsNumber(-0.0), Path::Fast, ",\"payload\":0"_s);
    check(jsNumber(1.5), Path::Fast, ",\"payload\":1.5"_s);
    check(jsNaN(), Path::Fast, ",\"payload\":null"_s);
    check(jsNumber(std::numeric_limits<double>::infinity()), Path::Fast, ",\"payload\":null"_s);
    check(jsEmptyString(vm), Path::Fast, ",\"payload\":\"\""_s);
    check(jsNontrivialString(vm, "say \"hi\"\n"_s), Path::Fast, ",\"payload\":\"say \\\"hi\\\"\\n\""_s);
    check(realms.evaluate(realm, "'left' + String(Math.max(1, 2)) + 'right, and long enough to be a rope'"_s), Path::Fast, ",\"payload\":\"left2right, and long enough to be a rope\""_s);
    check(realms.evaluate(realm, "'\\ud800'"_s), Path::Fast, ",\"payload\":\"\\ud800\""_s);
    checkStringify(program, createItem(program, realms.evaluate(realm, "'\\u4e2d\\u6587'"_s)), Path::Fast, String::fromUTF8("{\"id\":7,\"title\":\"Product 7\",\"payload\":\"\xe4\xb8\xad\xe6\x96\x87\",\"inStock\":false}"));
    checkStringify(program, createItem(program, createItem(program, realms.evaluate(realm, "['\\u20ac', 1.5, () => 1]"_s))), Path::Slow, String::fromUTF8("{\"id\":7,\"title\":\"Product 7\",\"payload\":{\"id\":7,\"title\":\"Product 7\",\"payload\":[\"\xe2\x82\xac\",1.5,null],\"inStock\":false},\"inStock\":false}"));
    check(jsNumber(10000), Path::Fast, ",\"payload\":10000"_s);
    check(jsNumber(17699), Path::Fast, ",\"payload\":17699"_s);
    check(jsNumber(99999999), Path::Fast, ",\"payload\":99999999"_s);
    check(jsNumber(100000000), Path::Fast, ",\"payload\":100000000"_s);
    check(realms.evaluate(realm, "Symbol('s')"_s), Path::Fast, ""_s);
    check(realms.evaluate(realm, "(function () { })"_s), Path::Fast, ""_s);
    check(realms.evaluate(realm, "() => 1"_s), Path::Fast, ""_s);
    check(realms.evaluate(realm, "Math.max"_s), Path::Fast, ""_s);
    check(realms.evaluate(realm, "(class { })"_s), Path::Fast, ""_s);
    check(realms.evaluate(realm, "Object.assign(() => 1, { toJSON() { return 'from a function'; } })"_s), Path::Slow, ",\"payload\":\"from a function\""_s);
    check(realms.evaluate(realm, "Object.setPrototypeOf(() => 1, { toJSON() { return 'inherited'; } })"_s), Path::Slow, ",\"payload\":\"inherited\""_s);
    check(realms.evaluate(realm, "[1, undefined, () => 1, Symbol('s'), 'x']"_s), Path::Slow, ",\"payload\":[1,null,null,null,\"x\"]"_s);
    check(realms.evaluate(realm, "[1, 'x', null]"_s), Path::Fast, ",\"payload\":[1,\"x\",null]"_s);
    check(realms.evaluate(realm, "({ a: 1 })"_s), Path::Fast, ",\"payload\":{\"a\":1}"_s);
    check(realms.evaluate(realm, "({ toJSON() { return 5; } })"_s), Path::Slow, ",\"payload\":5"_s);
    check(realms.evaluate(realm, "({ get a() { return 1; } })"_s), Path::Slow, ",\"payload\":{\"a\":1}"_s);
    check(realms.evaluate(realm, "new Proxy({ a: 1 }, { })"_s), Path::Slow, ",\"payload\":{\"a\":1}"_s);
    check(realms.evaluate(realm, "new Number(3)"_s), Path::Slow, ",\"payload\":3"_s);
    check(realms.evaluate(realm, "new String('boxed')"_s), Path::Slow, ",\"payload\":\"boxed\""_s);
    check(realms.evaluate(realm, "new Boolean(false)"_s), Path::Slow, ",\"payload\":false"_s);
    check(createItem(program, createBag(program, { "second"_s })), Path::Fast, ",\"payload\":{\"id\":7,\"title\":\"Product 7\",\"payload\":{\"second\":1},\"inStock\":false}"_s);
}

static void checkToJSONOfTheObjectItself(LoadedProgram& program)
{
    Realms& realms = program.realms();
    JSGlobalObject* realm = program.realm();
    Object* warm = createItem(program, jsNull());
    checkStringify(program, warm, Path::Fast, "{\"id\":7,\"title\":\"Product 7\",\"payload\":null,\"inStock\":false}"_s);

    Object* own = createItem(program, jsNull());
    put(program, own, "toJSON"_s, realms.evaluate(realm, "(function () { return this.id; })"_s));
    checkStringify(program, own, Path::Slow, "7"_s);

    Object* inherited = createItem(program, jsNull());
    AOT_TEST_CHECK(inherited->methodTable()->setPrototype(inherited, realm, realms.evaluate(realm, "({ toJSON() { return this.title; } })"_s), false));
    checkStringify(program, inherited, Path::Slow, "\"Product 7\""_s);

    checkStringify(program, warm, Path::Fast, "{\"id\":7,\"title\":\"Product 7\",\"payload\":null,\"inStock\":false}"_s);
}

static void checkCyclesAndDepth(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Object* cyclic = createItem(program, jsNull());
    AOT_TEST_CHECK(cyclic->store(vm, cyclic->layout().fields()[2], AcceptedValue::provenAtBuildTime(cyclic)));
    String text = JSONStringify(program.realm(), cyclic, 0);
    AOT_TEST_CHECK(text.isNull() && scope.exception());
    JSValue thrown = scope.exception()->value();
    scope.clearException();
    AOT_TEST_CHECK(thrown.toWTFString(program.realm()).startsWith("TypeError: JSON.stringify cannot serialize cyclic structures."_s));

    JSValue nested = jsNull();
    StringBuilder expected;
    constexpr unsigned depth = 300;
    for (unsigned level = 0; level < depth; ++level) {
        nested = createItem(program, nested);
        expected.append("{\"id\":7,\"title\":\"Product 7\",\"payload\":"_s);
    }
    expected.append("null"_s);
    for (unsigned level = 0; level < depth; ++level)
        expected.append(",\"inStock\":false}"_s);
    checkStringify(program, nested, Path::Fast, expected.toString());
}

static void checkInlineWalkMatchesTheWalkByCallbacks(LoadedProgram& program)
{
    VM& vm = program.vm();
    auto describe = [&](JSObject* object, auto&& walk) {
        StringBuilder text;
        auto result = walk(vm, object, PropertyWalk::Filter::EnumerableStrings, [&](const PropertyWalk::OwnProperty& property) {
            text.append(StringView(property.name), '=', property.value.toWTFString(program.realm()), '/', property.attributes, ' ');
            return IterationStatus::Continue;
        });
        text.append(static_cast<unsigned>(result));
        return text.toString();
    };
    auto walkInline = [](auto&&... arguments) { return PropertyWalk::forEachOwnPropertyInline(arguments...); };
    auto walkByCallbacks = [](auto&&... arguments) { return PropertyWalk::forEachOwnProperty(arguments...); };

    Object* frozen = createBag(program, { "second"_s, "first"_s });
    frozen->setIntegrity(program.realm(), Object::Integrity::Frozen);
    Object* extended = createItem(program, jsNumber(4));
    put(program, extended, "added"_s, jsNumber(1));
    for (JSObject* object : std::initializer_list<JSObject*> { createItem(program, jsUndefined()), createBag(program, { "second"_s, "first"_s }), frozen, extended, constructEmptyObject(program.realm()) })
        AOT_TEST_CHECK(describe(object, walkInline) == describe(object, walkByCallbacks));

    unsigned visited = 0;
    auto result = PropertyWalk::forEachOwnPropertyInline(vm, createItem(program, jsNull()), PropertyWalk::Filter::EnumerableStrings, [&](const PropertyWalk::OwnProperty&) {
        return ++visited == 2 ? IterationStatus::Done : IterationStatus::Continue;
    });
    AOT_TEST_CHECK(visited == 2 && result == PropertyWalk::Result::Stopped);
}

static void checkLongTextsTakeTheirOwnLengthInABuilder(LoadedProgram& program)
{
    VM& vm = program.vm();
    constexpr unsigned length = 1 << 20;
    constexpr unsigned lengthOfTheRest = 1024;
    String longText { Vector<Latin1Character>(FillWith { }, length, static_cast<Latin1Character>('t')).span() };
    auto append = [&](StringBuilderStorage* builder, Object* item) {
        OperationCallScope scope(vm);
        auto appended = operationAOTStringBuilderAppendJSON(&program.instance(), builder, JSValue::encode(item), JSValue::encode(jsNumber(0)), JSValue::encode(jsUndefined()));
        AOT_TEST_CHECK(!appended.exception && appended.value);
    };
    auto limitOf = [](StringBuilderStorage* builder) {
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(builder) + StringBuilderStorage::offsetOfLimit());
    };
    auto bufferOf = [](StringBuilderStorage* builder) {
        return *reinterpret_cast<Latin1Character**>(reinterpret_cast<uint8_t*>(builder) + StringBuilderStorage::offsetOfCharacters());
    };
    auto createExtended = [&](JSValue payload, JSValue added) {
        Object* item = createItem(program, payload);
        put(program, item, "added"_s, added);
        AOT_TEST_CHECK(item->hasExtraProperties());
        return item;
    };
    Object* shortItem = createExtended(jsNull(), jsNumber(1));
    String shortText = "{\"id\":7,\"title\":\"Product 7\",\"payload\":null,\"inStock\":false,\"added\":1}"_s;
    checkStringify(program, shortItem, Path::Fast, shortText);

    struct Case {
        JSValue payload;
        JSValue added;
        unsigned longTexts;
        bool hasEscape;
    };
    const Case cases[] = {
        { jsString(vm, longText), jsNumber(1), 1, false },
        { jsNull(), jsString(vm, longText), 1, false },
        { jsString(vm, longText), jsString(vm, longText), 2, false },
        { jsString(vm, makeString(longText, '"')), jsNumber(1), 1, true },
        { jsString(vm, makeString('\n', longText)), jsString(vm, longText), 2, true },
        { jsString(vm, longText), jsString(vm, makeString(longText, '\\')), 2, true },
        { jsNontrivialString(vm, "a\nb"_s), jsNontrivialString(vm, "\"c\""_s), 0, true },
    };
    for (const Case& oneCase : cases) {
        Object* item = createExtended(oneCase.payload, oneCase.added);
        String expected = JSONStringify(program.realm(), item, 0);
        AOT_TEST_CHECK(expected.length() >= oneCase.longTexts * length && expected.length() < oneCase.longTexts * length + lengthOfTheRest);

        StringBuilderStorage* builder = StringBuilderStorage::acquire(program.instance());
        append(builder, item);
        AOT_TEST_CHECK(builder->length() == expected.length());
        if (!oneCase.hasEscape) {
            AOT_TEST_CHECK(limitOf(builder) < expected.length() + expected.length() / 8 + lengthOfTheRest);
            Latin1Character* buffer = bufferOf(builder);
            for (unsigned i = 0; i < 100; ++i)
                append(builder, shortItem);
            AOT_TEST_CHECK(bufferOf(builder) == buffer);
        }
        JSString* written = builder->finish(program.realm(), program.instance(), StringBuilderStorage::StringUse::Unknown);
        AOT_TEST_CHECK(written);
        String text = written->value(program.realm()).data;
        AOT_TEST_CHECK(text.startsWith(expected) && (oneCase.hasEscape ? text.length() == expected.length() : text.length() == expected.length() + 100 * shortText.length() && text.endsWith(shortText)));
    }
}

// The list of an object with extra properties serves Object.keys, copies and for-in too. The text of its keys is for JSON.stringify alone, which writes it when it first asks.
static void checkKeyTextOfAListWithExtraPropertiesWaitsForStringify(LoadedProgram& program)
{
    VM& vm = program.vm();
    Object* extended = createItem(program, jsNull());
    put(program, extended, "added"_s, jsNumber(1));
    AOT_TEST_CHECK(extended->hasExtraProperties());
    CachedPropertyList* list = nullptr;
    {
        DeferGC deferGC(vm);
        list = cachedPropertyList(vm, *extended);
        AOT_TEST_CHECK(list && list->properties.size() == 5 && list->jsonKeys.isEmpty() && list->endsOfJSONKeys.isEmpty());
    }
    checkStringify(program, extended, Path::Fast, "{\"id\":7,\"title\":\"Product 7\",\"payload\":null,\"inStock\":false,\"added\":1}"_s);
    {
        DeferGC deferGC(vm);
        AOT_TEST_CHECK(cachedPropertyList(vm, *extended) == list && list->endsOfJSONKeys.size() == 5 && !list->jsonKeys.isEmpty());
    }
    checkStringify(program, extended, Path::Fast, "{\"id\":7,\"title\":\"Product 7\",\"payload\":null,\"inStock\":false,\"added\":1}"_s);
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<listedFieldsTests, programText, body>;

std::span<const NativeTest> listedFieldsTests()
{
    static constexpr NativeTest tests[] = {
        { "listed-fields/list-is-found-through-structure"_s, withProgram<checkListIsFoundThroughStructure> },
        { "listed-fields/each-key-order-has-its-list"_s, withProgram<checkEachKeyOrderHasItsList> },
        { "listed-fields/changed-objects-leave-the-list"_s, withProgram<checkChangedObjectsLeaveTheList> },
        { "listed-fields/key-text-is-escaped-once"_s, withProgram<checkKeyTextIsEscapedOnce> },
        { "listed-fields/key-text-of-a-list-with-extra-properties-waits-for-stringify"_s, withProgram<checkKeyTextOfAListWithExtraPropertiesWaitsForStringify> },
        { "listed-fields/only-wide-name-has-no-key-text"_s, withProgram<checkOnlyWideNameHasNoKeyText> },
        { "listed-fields/stringify-values-of-every-kind"_s, withProgram<checkValuesOfEveryKind> },
        { "listed-fields/stringify-to-json-of-the-object"_s, withProgram<checkToJSONOfTheObjectItself> },
        { "listed-fields/stringify-cycles-and-depth"_s, withProgram<checkCyclesAndDepth> },
        { "listed-fields/inline-walk-matches-walk-by-callbacks"_s, withProgram<checkInlineWalkMatchesTheWalkByCallbacks> },
        { "listed-fields/long-texts-take-their-own-length-in-a-builder"_s, withProgram<checkLongTextsTakeTheirOwnLengthInABuilder> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
