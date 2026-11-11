/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTOperationCallScope.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTPropertyWalk.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTRuntimeTable.h"
#include "AOTVector.h"
#include "DeferGCInlines.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "ObjectConstructor.h"
#include "ObjectPrototype.h"
#include "PrivateName.h"
#include "PropertyDescriptor.h"
#include "PropertySlot.h"
#include "ProxyObject.h"
#include "PutPropertySlot.h"
#include "Symbol.h"
#include <bit>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/text/MakeString.h>

namespace JSC::AOT {

std::span<const NativeTest> propertyLookupTests();

using Presence = PropertyWalk::Presence;

#define PROGRAM_CREATING_LABELED_OBJECT \
    "layout L0 name=\"Labeled\" module=m0 fields=[{key=\"label\" declared={type=anything} flags=[IsRequired]}, {key=\"count\" declared={type=number} flags=[IsRequired]}]\n" \
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n" \
    "function f0 name=\"main\" module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n" \
    "  b0():\n" \
    "    v0 = Builtin builtin=GlobalThis\n" \
    "    v1 = PropGet v0 key=\"publish\" [isFreeName]\n" \
    "    v2 = PropGet v0 key=\"input\" [isFreeName]\n" \
    "    v3 = ToNumber v2\n" \
    "    v4 = NewObject -, v2, v3 layout=L0 fields=[0, 1]\n"

static constexpr ASCIILiteral programText = PROGRAM_CREATING_LABELED_OBJECT
    "    v5 = Call v1, -, v4\n"
    "    Return\n"
    "}\n"
    "entry module=m0\n"_s;

static constexpr ASCIILiteral programAddingPropertiesText = PROGRAM_CREATING_LABELED_OBJECT
    "    v5 = Check v2 expectedType=string\n"
    "    ElemSet v4, v5, v2\n"
    "    v7 = Call v1, -, v4\n"
    "    v8 = PropGet v4 key=\"count\"\n"
    "    v9 = Call v1, -, v8\n"
    "    Return\n"
    "}\n"
    "entry module=m0\n"_s;

static constexpr ASCIILiteral programReadingCountText =
    "layout L0 name=\"Labeled\" module=m0 fields=[{key=\"label\" declared={type=anything} flags=[IsRequired]}, {key=\"count\" declared={type=number} flags=[IsRequired]}]\n"
    "variable g0 name=\"held\" module=m0 declared={type=anything} initialization=Undefined\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "function f0 name=\"main\" module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"publish\" [isFreeName]\n"
    "    v2 = NewClosure function=f1\n"
    "    v3 = Call v1, -, v2\n"
    "    Return\n"
    "}\n"
    "function f1 name=\"keep\" module=m0 parameters=[{type=anything}] results=[{type=anything}] {\n"
    "  b0(v0: anything):\n"
    "    v1 = ToNumber v0\n"
    "    v2 = NewObject -, v0, v1 layout=L0 fields=[0, 1]\n"
    "    v3 = FinishObject v2\n"
    "    v4 = GetModuleVariable variable=g0\n"
    "    PutModuleVariable v3 variable=g0\n"
    "    PropSet v4, v0 key=\"later\"\n"
    "    v7 = PropGet v4 key=\"label\"\n"
    "    v8 = PropGet v4 key=\"count\"\n"
    "    v9 = NewArray v7, v8 contents=anything holes=[]\n"
    "    Return v9\n"
    "}\n"
    "entry module=m0\n"_s;

#define PROGRAM_CREATING_SYMBOL \
    "layout L0 name=\"Tagged\" module=m0 fields=[{key=symbol(\"tag\")@m0:0 declared={type=anything} flags=[IsRequired]}]\n" \
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n" \
    "function f0 name=\"main\" module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n" \
    "  b0():\n" \
    "    v0 = Builtin builtin=GlobalThis\n" \
    "    v1 = PropGet v0 key=\"publish\" [isFreeName]\n" \
    "    v2 = PropGet v0 key=\"input\" [isFreeName]\n" \
    "    v3 = Builtin builtin=Symbol\n" \
    "    v4 = String string=\"tag\"\n" \
    "    v5 = Call v3, -, v4 calleeName=\"Symbol\" callNumber=0\n" \
    "    v6 = NewObject -, v2 layout=L0 fields=[0]\n" \
    "    v7 = Call v1, -, v6, v5\n" \
    "    Return\n" \
    "}\n" \
    "entry module=m0\n"

static constexpr ASCIILiteral programCreatingSymbolText = PROGRAM_CREATING_SYMBOL ""_s;

static PropertyAnswer readAndClassify(LoadedProgram& program, JSValue base, const Identifier& name, ChainWalkMode mode = ChainWalkMode::MayAllocate)
{
    JSGlobalObject* realm = program.realm();
    Structure* structureBefore = structureForPrimitive(realm, base);
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
    if (structureBefore)
        base.get(realm, name, slot);
    AOT_TEST_CHECK(!program.vm().exceptionForInspection());
    PropertyAnswer answer = classifyRead({ realm, base, structureBefore, name.impl(), slot, mode });
    AOT_TEST_CHECK(answer.access == SlotAccess::Read);
    return answer;
}

static bool isOwn(const PropertyAnswer& answer, PropertyAnswerKind kind, PropertyOffset offset)
{
    return answer.kind == kind && answer.isOwn && !answer.holder && answer.offset == offset && !answer.dependsOnChain();
}

static bool findsOwnValue(LoadedProgram& program, JSObject* object, const Identifier& name)
{
    DeferGC deferGC(program.vm());
    PropertyOffset offset = object->getDirectOffset(program.vm(), name);
    PropertyAnswer answer = readAndClassify(program, object, name);
    return isValidOffset(offset) && isOwn(answer, PropertyAnswerKind::Value, offset) && slotFor(answer, object->structure()).offsetInStorage() == offset;
}

static bool isInheritedFrom(LoadedProgram& program, const PropertyAnswer& answer, PropertyAnswerKind kind, JSObject* holder, const Identifier& name)
{
    return answer.kind == kind && !answer.isOwn && answer.holder == holder && answer.offset == holder->getDirectOffset(program.vm(), name);
}

static bool isAbsent(const PropertyAnswer& answer)
{
    return answer.kind == PropertyAnswerKind::Absent && !answer.isOwn && !answer.holder;
}

static bool isLastingRefusal(const PropertyAnswer& answer)
{
    return answer.kind == PropertyAnswerKind::Uncacheable && !answer.didFlatten;
}

static bool dependsOnlyOn(const PropertyAnswer& answer, JSObject* object)
{
    return answer.changeableObjects.size() == 1 && answer.changeableObjects[0] == object;
}

static Presence presenceAfterRead(LoadedProgram& program, JSObject* object, const Identifier& name)
{
    DeferGC deferGC(program.vm());
    readAndClassify(program, object, name);
    return PropertyWalk::hasPropertyOnChain(object->structure(), name.impl());
}

static void testOwnAccessor(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    Identifier label = Identifier::fromString(vm, "label"_s);
    JSObject* object = constructEmptyObject(realm);
    object->putDirectNonIndexAccessor(vm, label, GetterSetter::create(vm, realm, jsUndefined(), jsUndefined()), static_cast<unsigned>(PropertyAttribute::Accessor));

    AOT_TEST_CHECK(isOwn(readAndClassify(program, object, label), PropertyAnswerKind::Accessor, object->getDirectOffset(vm, label)));
    AOT_TEST_CHECK(slotFor(readAndClassify(program, object, label), object->structure()).kind == Slot::Kind::Getter);
    AOT_TEST_CHECK(presenceAfterRead(program, object, label) == Presence::Present);
    AOT_TEST_CHECK(isAbsent(readAndClassify(program, object, Identifier::fromString(vm, "other"_s))));
}

static void testUnderscoreProtoAndIndex(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    const Identifier& underscoreProto = vm.propertyNames->underscoreProto;
    Identifier index = Identifier::from(vm, 0u);
    JSObject* object = constructEmptyObject(realm);

    AOT_TEST_CHECK(isInheritedFrom(program, readAndClassify(program, object, underscoreProto), PropertyAnswerKind::Accessor, realm->objectPrototype(), underscoreProto));
    AOT_TEST_CHECK(isLastingRefusal(readAndClassify(program, constructEmptyObject(vm, realm->nullPrototypeObjectStructure()), underscoreProto)));
    AOT_TEST_CHECK(isLastingRefusal(readAndClassify(program, object, index)));
    AOT_TEST_CHECK(presenceAfterRead(program, object, index) == Presence::Unknown);

    object->putDirect(vm, underscoreProto, jsNumber(7));
    AOT_TEST_CHECK(object->putDirectIndex(realm, 0, jsNumber(7)));
    AOT_TEST_CHECK(object->type() == FinalObjectType);
    AOT_TEST_CHECK(isLastingRefusal(readAndClassify(program, object, underscoreProto)));
    AOT_TEST_CHECK(isLastingRefusal(readAndClassify(program, object, index)));
    AOT_TEST_CHECK(presenceAfterRead(program, object, index) == Presence::Unknown);
}

static void testDictionary(LoadedProgram& program)
{
    VM& vm = program.vm();
    Identifier label = Identifier::fromString(vm, "label"_s);
    Identifier other = Identifier::fromString(vm, "other"_s);
    JSObject* object = constructEmptyObject(program.realm());
    object->putDirect(vm, label, jsNumber(7));
    AOT_TEST_CHECK(findsOwnValue(program, object, label));
    AOT_TEST_CHECK(isAbsent(readAndClassify(program, object, other)));

    object->convertToDictionary(vm);
    AOT_TEST_CHECK(PropertyWalk::hasPropertyOnChain(object->structure(), label.impl()) == Presence::Unknown);
    AOT_TEST_CHECK(isOwn(readAndClassify(program, object, label, ChainWalkMode::AllocatesNothing), PropertyAnswerKind::Value, object->getDirectOffset(vm, label)));
    AOT_TEST_CHECK(isLastingRefusal(readAndClassify(program, object, other, ChainWalkMode::AllocatesNothing)));
    AOT_TEST_CHECK(object->structure()->isDictionary());

    AOT_TEST_CHECK(isAbsent(readAndClassify(program, object, other)) && !object->structure()->isDictionary());
    AOT_TEST_CHECK(findsOwnValue(program, object, label));
    AOT_TEST_CHECK(isAbsent(readAndClassify(program, object, other)));

    JSObject* readFirstForWhatItHolds = constructEmptyObject(program.realm());
    readFirstForWhatItHolds->putDirect(vm, label, jsNumber(7));
    readFirstForWhatItHolds->convertToDictionary(vm);
    AOT_TEST_CHECK(findsOwnValue(program, readFirstForWhatItHolds, label) && !readFirstForWhatItHolds->structure()->isDictionary());

    JSObject* givesNamesUp = constructEmptyObject(program.realm());
    givesNamesUp->putDirect(vm, label, jsNumber(7));
    givesNamesUp->convertToUncacheableDictionary(vm);
    PropertyAnswer answer = readAndClassify(program, givesNamesUp, label);
    AOT_TEST_CHECK(answer.kind == PropertyAnswerKind::Uncacheable && answer.didFlatten && !givesNamesUp->structure()->isDictionary());
    AOT_TEST_CHECK(findsOwnValue(program, givesNamesUp, label));

    object->convertToDictionary(vm);
    AOT_TEST_CHECK(findsOwnValue(program, object, label));
    AOT_TEST_CHECK(isLastingRefusal(readAndClassify(program, object, other)));
    AOT_TEST_CHECK(object->structure()->isDictionary());

    object->convertToUncacheableDictionary(vm);
    AOT_TEST_CHECK(isLastingRefusal(readAndClassify(program, object, label)));
    AOT_TEST_CHECK(isLastingRefusal(readAndClassify(program, object, other)));
    AOT_TEST_CHECK(object->structure()->isUncacheableDictionary());
}

static void testOtherPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    Identifier label = Identifier::fromString(vm, "label"_s);
    JSObject* prototype = constructEmptyObject(realm);
    JSObject* objectWithOtherPrototype = constructEmptyObject(realm, prototype);
    JSObject* objectWithNullPrototype = constructEmptyObject(vm, realm->nullPrototypeObjectStructure());
    AOT_TEST_CHECK(objectWithNullPrototype->getPrototypeDirect().isNull());

    PropertyAnswer answerBelowPrototype = readAndClassify(program, objectWithOtherPrototype, label);
    AOT_TEST_CHECK(isAbsent(answerBelowPrototype) && dependsOnlyOn(answerBelowPrototype, prototype));
    AOT_TEST_CHECK(slotFor(answerBelowPrototype, objectWithOtherPrototype->structure()).has(Slot::dependsOnChain));
    AOT_TEST_CHECK(!program.state().chainDependents->hasDependents(prototype));
    AOT_TEST_CHECK(presenceAfterRead(program, objectWithOtherPrototype, label) == Presence::Absent);
    AOT_TEST_CHECK(program.state().chainDependents->hasDependents(prototype));
    PropertyAnswer answerWithoutPrototype = readAndClassify(program, objectWithNullPrototype, label);
    AOT_TEST_CHECK(isAbsent(answerWithoutPrototype) && !answerWithoutPrototype.dependsOnChain());

    for (JSObject* object : { objectWithOtherPrototype, objectWithNullPrototype }) {
        AOT_TEST_CHECK(object->type() == FinalObjectType);
        object->putDirect(vm, label, jsNumber(7));
        AOT_TEST_CHECK(findsOwnValue(program, object, label));
    }

    prototype->putDirect(vm, label, jsNumber(8));
    PropertyAnswer inherited = readAndClassify(program, constructEmptyObject(realm, prototype), label);
    AOT_TEST_CHECK(isInheritedFrom(program, inherited, PropertyAnswerKind::Value, prototype, label) && dependsOnlyOn(inherited, prototype));
}

static void testOtherBase(LoadedProgram& program)
{
    VM& vm = program.vm();
    Identifier label = Identifier::fromString(vm, "label"_s);
    JSArray* array = constructEmptyArray(program.realm(), nullptr);
    array->putDirect(vm, label, jsNumber(7));

    AOT_TEST_CHECK(findsOwnValue(program, array, label));
    AOT_TEST_CHECK(isAbsent(readAndClassify(program, array, Identifier::fromString(vm, "other"_s))));
    AOT_TEST_CHECK(isOwn(readAndClassify(program, array, vm.propertyNames->length), PropertyAnswerKind::ArrayLength, invalidOffset));

    auto* sharedStorage = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, 2);
    sharedStorage->setIndex(vm, 0, jsNumber(1));
    sharedStorage->setIndex(vm, 1, jsNumber(2));
    JSArray* sharing = JSArray::createWithButterfly(vm, nullptr, program.realm()->originalArrayStructureForIndexingType(CopyOnWriteArrayWithContiguous), sharedStorage->toButterfly());
    AOT_TEST_CHECK(isOwn(readAndClassify(program, sharing, vm.propertyNames->length), PropertyAnswerKind::ArrayLength, invalidOffset));
    AOT_TEST_CHECK(PropertyWalk::hasPropertyOnChain(array->structure(), label.impl()) == Presence::Unknown);

    JSString* text = jsNontrivialString(vm, "text"_s);
    AOT_TEST_CHECK(isAbsent(readAndClassify(program, text, label)));
    AOT_TEST_CHECK(isOwn(readAndClassify(program, text, vm.propertyNames->length), PropertyAnswerKind::StringLength, invalidOffset));
    AOT_TEST_CHECK(isAbsent(readAndClassify(program, jsNumber(7), label)));
    AOT_TEST_CHECK(isLastingRefusal(readAndClassify(program, jsUndefined(), label)));
}

static void testLengthOfTypedArrays(LoadedProgram& program)
{
    VM& vm = program.vm();
    Realms& realms = program.realms();
    const Identifier& length = vm.propertyNames->length;

    for (ASCIILiteral source : { "new Uint8Array(4)"_s, "new Float64Array(4)"_s, "new BigInt64Array(4)"_s, "new Uint8Array(new ArrayBuffer(8, { maxByteLength: 16 }))"_s, "Object.assign(new Uint8Array(4), { extra: 1 })"_s }) {
        JSObject* view = realms.object(program.realm(), source);
        PropertyAnswer answer = readAndClassify(program, view, length);
        AOT_TEST_CHECK_FOR_SOURCE(answer.kind == PropertyAnswerKind::TypedArrayLength && !answer.isOwn && !answer.dependsOnChain(), source);
        Slot slot = slotFor(answer, view->structure());
        AOT_TEST_CHECK_FOR_SOURCE(slot.kind == Slot::Kind::TypedArrayLength && slot.flags == Slot::needsHandler && !slot.payloadBits(), source);
    }

    JSObject* instanceOfSubclass = realms.object(program.realm(), "new (class extends Uint8Array { })(4)"_s);
    PropertyAnswer belowSubclass = readAndClassify(program, instanceOfSubclass, length);
    AOT_TEST_CHECK(belowSubclass.kind == PropertyAnswerKind::TypedArrayLength && dependsOnlyOn(belowSubclass, instanceOfSubclass->getPrototypeDirect().getObject()));

    static constexpr ASCIILiteral objectsWithOtherLength[] = {
        "Object.defineProperty(new Uint8Array(4), 'length', { value: 1 })"_s,
        "new (class extends Uint8Array { get length() { return 1; } })(4)"_s,
        "Object.setPrototypeOf(new Uint8Array(4), null)"_s,
        "Object.setPrototypeOf(new Uint8Array(4), { get length() { return 1; } })"_s,
        "({ get length() { return 1; } })"_s,
        "[1, 2, 3, 4]"_s,
    };
    for (ASCIILiteral source : objectsWithOtherLength)
        AOT_TEST_CHECK_FOR_SOURCE(readAndClassify(program, realms.object(program.realm(), source), length).kind != PropertyAnswerKind::TypedArrayLength, source);
    for (ASCIILiteral source : { "new Uint8Array(4)"_s, "new Int8Array(4)"_s, "new Uint8ClampedArray(4)"_s, "new Uint8Array(new ArrayBuffer(8, { maxByteLength: 16 }))"_s })
        AOT_TEST_CHECK_FOR_SOURCE(readAndClassify(program, realms.object(program.realm(), source), vm.propertyNames->byteLength).kind == PropertyAnswerKind::TypedArrayLength, source);
    for (ASCIILiteral source : { "new Uint16Array(4)"_s, "new Float64Array(4)"_s, "new DataView(new ArrayBuffer(8))"_s, "new (class extends Uint8Array { get byteLength() { return 1; } })(4)"_s })
        AOT_TEST_CHECK_FOR_SOURCE(readAndClassify(program, realms.object(program.realm(), source), vm.propertyNames->byteLength).kind == PropertyAnswerKind::Accessor, source);
    for (ASCIILiteral source : { "new Uint8Array(4)"_s, "new Float64Array(new ArrayBuffer(32), 8, 2)"_s }) {
        JSObject* view = realms.object(program.realm(), source);
        PropertyAnswer answer = readAndClassify(program, view, vm.propertyNames->byteOffset);
        AOT_TEST_CHECK_FOR_SOURCE(answer.kind == PropertyAnswerKind::TypedArrayLength && answer.readsByteOffset, source);
        AOT_TEST_CHECK_FOR_SOURCE(slotFor(answer, view->structure()).flags == (Slot::needsHandler | Slot::readsByteOffset), source);
    }
    for (ASCIILiteral source : { "new DataView(new ArrayBuffer(8))"_s, "new (class extends Uint8Array { get byteOffset() { return 1; } })(4)"_s })
        AOT_TEST_CHECK_FOR_SOURCE(readAndClassify(program, realms.object(program.realm(), source), vm.propertyNames->byteOffset).kind != PropertyAnswerKind::TypedArrayLength, source);
}

static JSString* createKeyWithoutAtom(VM& vm)
{
    JSString* key = jsString(vm, makeString("la"_s, "bel"_s));
    AOT_TEST_CHECK(!key->tryGetValueImpl()->isAtom());
    return key;
}

static void testLastIndexOfRegExps(LoadedProgram& program)
{
    VM& vm = program.vm();
    Realms& realms = program.realms();
    const Identifier& lastIndex = vm.propertyNames->lastIndex;

    for (ASCIILiteral source : { "/a/g"_s, "/a/"_s, "new RegExp('a', 'y')"_s, "Object.assign(/a/g, { extra: 1 })"_s, "Object.freeze(/a/g)"_s, "new (class extends RegExp { get lastIndex() { return 7; } })('a')"_s }) {
        JSObject* regExp = realms.object(program.realm(), source);
        PropertyAnswer answer = readAndClassify(program, regExp, lastIndex);
        AOT_TEST_CHECK_FOR_SOURCE(isOwn(answer, PropertyAnswerKind::LastIndexOfRegExp, invalidOffset), source);
        Slot slot = slotFor(answer, regExp->structure());
        AOT_TEST_CHECK_FOR_SOURCE(slot.kind == Slot::Kind::ArrayLength && slot.has(Slot::isLastIndexOfRegExp) && !slot.payloadBits(), source);
    }
    Slot lengthOfList = slotFor(readAndClassify(program, realms.object(program.realm(), "[1, 2]"_s), vm.propertyNames->length), realms.object(program.realm(), "[1, 2]"_s)->structure());
    AOT_TEST_CHECK(lengthOfList.kind == Slot::Kind::ArrayLength && !lengthOfList.has(Slot::isLastIndexOfRegExp));

    for (ASCIILiteral source : { "Object.create(/a/g)"_s, "({ lastIndex: 3 })"_s, "({ })"_s, "[1]"_s, "(function () { })"_s })
        AOT_TEST_CHECK_FOR_SOURCE(readAndClassify(program, realms.object(program.realm(), source), lastIndex).kind != PropertyAnswerKind::LastIndexOfRegExp, source);
    AOT_TEST_CHECK(readAndClassify(program, realms.object(program.realm(), "/a/g"_s), vm.propertyNames->length).kind != PropertyAnswerKind::LastIndexOfRegExp);
}

static void testEachPlaceOfAnswersWaitsByStateOfItsOwn()
{
    BitVector taken(SiteCacheState::numberOfStates);
    for (uint32_t place = 0; place < SiteCacheState::numberOfStates; ++place) {
        uint32_t offset = SiteCacheState::offsetOfStateOf(SiteCacheState::keyOfPlaceOfAnswers(place));
        AOT_TEST_CHECK(!(offset % sizeof(SiteCacheState)) && offset / sizeof(SiteCacheState) < SiteCacheState::numberOfStates);
        AOT_TEST_CHECK(!taken.quickSet(offset / sizeof(SiteCacheState)));
    }
}

static void testIndexesAreNotNames(LoadedProgram& program)
{
    struct KeyText {
        ASCIILiteral text;
        KeyNameKind kind;
        uint32_t index { 0 };
    };
    static constexpr KeyText keyTexts[] = {
        { "3d"_s, KeyNameKind::Name },
        { "10px"_s, KeyNameKind::Name },
        { "0"_s, KeyNameKind::Index, 0 },
        { "00"_s, KeyNameKind::Name },
        { "4294967294"_s, KeyNameKind::Index, 4294967294u },
        { "4294967295"_s, KeyNameKind::Name },
        { ""_s, KeyNameKind::Name },
        { "__proto__"_s, KeyNameKind::NotAName },
    };
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    for (const KeyText& keyText : keyTexts) {
        bool isIndex = keyText.kind == KeyNameKind::Index;
        for (KeyAtomization atomization : { KeyAtomization::OnlyExisting, KeyAtomization::Atomize }) {
            JSString* keyWithoutAtom = jsString(vm, String(keyText.text));
            if (isIndex && keyText.text.length() > 1) {
                AOT_TEST_CHECK_FOR_SOURCE(nameOfKeyWithoutAllocating(realm, keyWithoutAtom, atomization).kind == KeyNameKind::Index, keyText.text);
                AOT_TEST_CHECK_FOR_SOURCE(!keyWithoutAtom->tryGetValueImpl()->isAtom(), keyText.text);
            }
            Identifier atom = Identifier::fromString(vm, String(keyText.text));
            for (JSString* key : { keyWithoutAtom, jsString(vm, atom.string().string()) }) {
                KeyName name = nameOfKeyWithoutAllocating(realm, key, atomization);
                AOT_TEST_CHECK_FOR_SOURCE(name.kind == keyText.kind && name.index == keyText.index, keyText.text);
                AOT_TEST_CHECK_FOR_SOURCE(name.uid == (keyText.kind == KeyNameKind::Name ? atom.impl() : nullptr), keyText.text);
                AOT_TEST_CHECK_FOR_SOURCE(indexOfKey(key) == (isIndex ? std::optional<uint32_t>(keyText.index) : std::nullopt), keyText.text);
            }
        }
    }

    auto createRopeOfDigits = [&] {
        JSString* rope = jsString(realm, jsNontrivialString(vm, "12"_s), jsNontrivialString(vm, "34"_s));
        AOT_TEST_CHECK(rope->isRope());
        return rope;
    };
    AOT_TEST_CHECK(nameOfKeyWithoutAllocating(realm, createRopeOfDigits(), KeyAtomization::OnlyExisting).kind == KeyNameKind::NotAName);
    AOT_TEST_CHECK(!indexOfKey(createRopeOfDigits()));
    KeyName nameOfRope = nameOfKeyWithoutAllocating(realm, createRopeOfDigits(), KeyAtomization::Atomize);
    AOT_TEST_CHECK(nameOfRope.kind == KeyNameKind::Index && nameOfRope.index == 1234 && !nameOfRope.uid);

    AOT_TEST_CHECK(indexOfKey(jsNumber(7)) == 7u && indexOfKey(jsNumber(7.0)) == 7u);
    AOT_TEST_CHECK(!indexOfKey(jsNumber(-1)) && !indexOfKey(jsNumber(7.5)) && !indexOfKey(jsUndefined()) && !indexOfKey(Symbol::create(vm)));
}

static void testSlotFiledByNumberIsCopiedForKeys(LoadedProgram& program)
{
    VM& vm = program.vm();
    PropertyCaches& caches = *program.state().propertyCaches;
    Identifier label = Identifier::fromString(vm, "label"_s);
    CachedName name = caches.nameOf(label.impl());
    AOT_TEST_CHECK(name.identifierNumber && RowDisplacementTable::hasColumn(*name.identifierNumber));
    AOT_TEST_CHECK(caches.nameOf(*name.identifierNumber).uid == label.impl());
    JSObject* object = constructEmptyObject(vm, program.realm()->nullPrototypeObjectStructure());
    object->putDirect(vm, label, jsNumber(7));
    Structure* structure = object->structure();

    caches.cache({ structure, name, SlotAccess::Read }, readAndClassify(program, object, label));
    std::optional<Slot> filed = caches.tables().front().find(structure->id(), *name.identifierNumber, SlotAccess::Read);
    AOT_TEST_CHECK(filed && filed->kind == Slot::Kind::Value && !caches.tables().hashedTable().find(structure->id(), label.impl(), SlotAccess::Read));

    AOT_TEST_CHECK(JSValue::decode(operationAOTGetByValGeneric(&program.instance(), object, createKeyWithoutAtom(vm))) == jsNumber(7));

    const Slot* copy = caches.tables().hashedTable().find(structure->id(), label.impl(), SlotAccess::Read);
    AOT_TEST_CHECK(copy && copy->headerBits() == filed->headerBits() && copy->payloadBits() == filed->payloadBits());
}

namespace {

struct ObjectBelowPrototype {
    explicit ObjectBelowPrototype(LoadedProgram& program)
        : program(program)
        , caches(*program.state().propertyCaches)
        , label(Identifier::fromString(program.vm(), "label"_s))
        , other(Identifier::fromString(program.vm(), "other"_s))
        , prototype(constructEmptyObject(program.realm()))
        , object(constructEmptyObject(program.realm(), prototype))
    {
    }

    void file(const Identifier& name)
    {
        caches.cache({ object->structure(), caches.nameOf(name.impl()), SlotAccess::Read }, readAndClassify(program, object, name));
    }

    std::optional<Slot> find(const Identifier& name)
    {
        return caches.tables().find(object->structure()->id(), caches.nameOf(name.impl()), SlotAccess::Read);
    }

    bool isFiledAsAbsent(const Identifier& name)
    {
        std::optional<Slot> slot = find(name);
        return slot && slot->kind == Slot::Kind::Absent && slot->has(Slot::dependsOnChain);
    }

    LoadedProgram& program;
    PropertyCaches& caches;
    Identifier label;
    Identifier other;
    JSObject* prototype;
    JSObject* object;
};

} // namespace

static void testEvictedDependentsAreDroppedWhenTheTableHasDoubled(LoadedProgram& program)
{
    VM& vm = program.vm();
    ObjectBelowPrototype fixture(program);
    ChainDependents& dependents = *program.state().chainDependents;
    auto collect = [&] {
        vm.heap.collectNow(Sync, CollectionScope::Full);
    };
    fixture.file(fixture.label);
    fixture.file(fixture.other);
    collect();
    AOT_TEST_CHECK(fixture.isFiledAsAbsent(fixture.label) && fixture.isFiledAsAbsent(fixture.other) && dependents.numberOfDependents(fixture.prototype) == 2);

    fixture.caches.tables().remove(fixture.object->structure()->id(), fixture.caches.nameOf(fixture.other.impl()), SlotAccess::Read);
    AOT_TEST_CHECK(!fixture.find(fixture.other) && dependents.numberOfDependents(fixture.prototype) == 2);
    collect();
    AOT_TEST_CHECK(dependents.numberOfDependents(fixture.prototype) == 2);
    fixture.prototype->putDirect(vm, fixture.other, jsNumber(1));
    AOT_TEST_CHECK(dependents.numberOfDependents(fixture.prototype) == 1 && fixture.isFiledAsAbsent(fixture.label));

    fixture.caches.tables().remove(fixture.object->structure()->id(), fixture.caches.nameOf(fixture.label.impl()), SlotAccess::Read);
    constexpr unsigned numberOfNames = 4000;
    auto nameAt = [&](unsigned index) {
        return Identifier::fromString(vm, makeString("added"_s, index));
    };
    for (unsigned index = 0; index < numberOfNames; ++index)
        fixture.file(nameAt(index));
    size_t numberStillFiled = 0;
    for (unsigned index = 0; index < numberOfNames; ++index)
        numberStillFiled += fixture.isFiledAsAbsent(nameAt(index));
    size_t numberBefore = dependents.numberOfDependents(fixture.prototype);
    AOT_TEST_CHECK(numberStillFiled && numberBefore > numberStillFiled);
    collect();
    AOT_TEST_CHECK(dependents.numberOfDependents(fixture.prototype) == numberStillFiled);
}

static void testNamedChangesOfAnObjectWithManyDependents(LoadedProgram& program)
{
    VM& vm = program.vm();
    ObjectBelowPrototype fixture(program);
    ChainDependents& dependents = *program.state().chainDependents;
    constexpr unsigned numberOfNames = 500;
    auto nameAt = [&](unsigned index) {
        return Identifier::fromString(vm, makeString("indexed"_s, index));
    };
    for (unsigned index = 0; index < numberOfNames; ++index)
        fixture.file(nameAt(index));
    size_t numberLeft = dependents.numberOfDependents(fixture.prototype);
    AOT_TEST_CHECK(numberLeft == numberOfNames);

    for (unsigned index = 0; index < numberOfNames; index += 2) {
        fixture.prototype->putDirect(vm, nameAt(index), jsNumber(index));
        AOT_TEST_CHECK(dependents.numberOfDependents(fixture.prototype) == --numberLeft);
        AOT_TEST_CHECK(!fixture.find(nameAt(index)));
    }
    vm.heap.collectNow(Sync, CollectionScope::Full);
    for (unsigned index = 1; index < numberOfNames; index += 2) {
        size_t numberBefore = dependents.numberOfDependents(fixture.prototype);
        bool wasFiled = fixture.isFiledAsAbsent(nameAt(index));
        fixture.prototype->putDirect(vm, nameAt(index), jsNumber(index));
        AOT_TEST_CHECK(!fixture.find(nameAt(index)));
        AOT_TEST_CHECK(dependents.numberOfDependents(fixture.prototype) + wasFiled <= numberBefore);
    }
    didChangeChainObject(vm, fixture.prototype, nullptr);
    AOT_TEST_CHECK(!dependents.hasDependents(fixture.prototype));
}

static Object* allocateLabeledObject(LoadedProgram& program)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t number = 0; number < tables.allocations().size(); ++number) {
        if (tables.allocations()[number].shape < tables.firstEnvironmentShape)
            return uncheckedDowncast<Object>(program.state().objects->structures.allocate(program.vm(), number));
    }
    failNativeTest(__FILE__, __LINE__, "the image allocates an object");
}

static bool storeInSlot(LoadedProgram& program, Object* object, const FieldData& field, JSValue value)
{
    std::optional<AcceptedValue> accepted = program.state().objects->layouts.accept(field, value);
    return accepted && object->store(program.vm(), field, *accepted);
}

static void checkWhereSlotsUnderCreatedSymbolAreFiled(LoadedProgram& program, bool eachRealmCreatesItsSymbol)
{
    VM& vm = program.vm();
    PropertyCaches& caches = *program.state().propertyCaches;
    AOT_TEST_CHECK(!!(Image::current()->programTables().flags & ProgramTables::mayRunInMultipleInstances) == eachRealmCreatesItsSymbol);
    CachedName name = caches.nameOf(allocateLabeledObject(program)->layout().fields()[0].name);
    AOT_TEST_CHECK(name.uid->isSymbol() && RowDisplacementTable::hasColumn(*name.identifierNumber));
    AOT_TEST_CHECK(identifiersOf(program.instance()).isSymbolOfRealm(*name.identifierNumber) == eachRealmCreatesItsSymbol);

    Identifier tag = Identifier::fromUid(vm, name.uid);
    PropertyOffset offset;
    Structure* structure = Structure::addPropertyTransition(vm, program.realm()->nullPrototypeObjectStructure(), tag, 0, offset);
    AOT_TEST_CHECK(!structure->isDictionary());
    Slot slot = Slot::createInStorage(structure->id(), Slot::Kind::Value, Slot::isOwn, nullptr, offset);
    for (SlotAccess access : { SlotAccess::Read, SlotAccess::Store }) {
        caches.tables().set(structure, name, access, slot);
        AOT_TEST_CHECK(!!caches.tables().front().find(structure->id(), *name.identifierNumber, access) == !eachRealmCreatesItsSymbol);
        AOT_TEST_CHECK(!!caches.tables().hashedTable().find(structure->id(), name.uid, access) == eachRealmCreatesItsSymbol);
        AOT_TEST_CHECK(caches.findByKey(structure, name.uid, access));
    }
}

static void testSymbolOfOneRealmIsFiledInFront(LoadedProgram& program)
{
    checkWhereSlotsUnderCreatedSymbolAreFiled(program, false);
}

static void testSymbolOfEachRealmIsNeverFiledInFront(LoadedProgram& program)
{
    checkWhereSlotsUnderCreatedSymbolAreFiled(program, true);
}

static void putByName(LoadedProgram& program, JSObject* object, const Identifier& name, JSValue value)
{
    PutPropertySlot slot(object, true);
    AOT_TEST_CHECK(object->methodTable()->put(object, program.realm(), name, value, slot) && !program.vm().exceptionForInspection());
}

static bool holdsUnboxedNumber(const FieldData* field)
{
    return field && traitsOf(field->slot.representation()).valueKind == SlotValueKind::Double;
}

static void defineAccessor(LoadedProgram&, JSObject*, const Identifier&);
static PropertyAnswer storeAndClassify(LoadedProgram&, JSObject*, const Identifier&, JSValue);

static void testReadsBelowEmptySlotsAskTheFirstObjectThatHasOne(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    PropertyCaches& caches = *program.state().propertyCaches;
    Identifier label = Identifier::fromString(vm, "label"_s);
    JSObject* top = constructEmptyObject(realm);
    Object* upper = allocateLabeledObject(program);
    Object* lower = allocateLabeledObject(program);
    const FieldData* labelField = upper->fieldNamed(program.state(), label);
    upper->clear(*labelField);
    lower->clear(*labelField);
    upper->setPrototypeKeepingShape(vm, top);
    lower->setPrototypeKeepingShape(vm, upper);
    JSObject* object = constructEmptyObject(realm, lower);
    AOT_TEST_CHECK(!upper->hasExtraProperties() && !lower->hasExtraProperties());
    auto find = [&](JSObject* receiver) {
        return caches.tables().find(receiver->structure()->id(), caches.nameOf(label.impl()), SlotAccess::Read);
    };
    auto file = [&](JSObject* receiver) {
        PropertyAnswer answer = readAndClassify(program, receiver, label);
        caches.cache({ receiver->structure(), caches.nameOf(label.impl()), SlotAccess::Read }, answer);
        return answer;
    };

    PropertyAnswer fromObject = file(object);
    AOT_TEST_CHECK(fromObject.kind == PropertyAnswerKind::InheritedField && fromObject.holder == lower);
    for (JSObject* onChain : { static_cast<JSObject*>(lower), static_cast<JSObject*>(upper), top })
        AOT_TEST_CHECK(fromObject.changeableObjects.contains(onChain));
    PropertyAnswer fromLower = file(lower);
    AOT_TEST_CHECK(fromLower.kind == PropertyAnswerKind::FieldOrInheritedField && fromLower.field == labelField && fromLower.holder == upper);
    PropertyAnswer fromUpper = file(upper);
    AOT_TEST_CHECK(fromUpper.kind == PropertyAnswerKind::Field && fromUpper.absentIsUndefined);

    AOT_TEST_CHECK(storeInSlot(program, upper, *labelField, jsNumber(2)));
    AOT_TEST_CHECK(find(object) && find(lower) && find(upper));

    upper->clear(*labelField);
    top->putDirect(vm, label, jsNumber(5));
    AOT_TEST_CHECK(!find(object) && !find(lower) && !find(upper));
    AOT_TEST_CHECK(file(object).holder == lower && file(lower).holder == upper);
    PropertyAnswer aboveUpper = file(upper);
    AOT_TEST_CHECK(aboveUpper.kind == PropertyAnswerKind::FieldOrInheritedValue && aboveUpper.holder == top);

    DeletePropertySlot deleteSlot;
    AOT_TEST_CHECK(JSObject::deleteProperty(top, realm, label, deleteSlot));
    defineAccessor(program, top, label);
    AOT_TEST_CHECK(!find(object) && !find(lower) && !find(upper));
    AOT_TEST_CHECK(isLastingRefusal(readAndClassify(program, object, label)));
    PropertyAnswer lowerBelowAccessor = readAndClassify(program, lower, label);
    AOT_TEST_CHECK(lowerBelowAccessor.kind == PropertyAnswerKind::Field && !lowerBelowAccessor.absentIsUndefined);
}

static void testEmptySlotOfReceiverDefersToUnboxedFieldOfCompiledPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    PropertyCaches& caches = *program.state().propertyCaches;
    Identifier count = Identifier::fromString(vm, "count"_s);
    Object* prototype = allocateLabeledObject(program);
    putByName(program, prototype, count, jsNumber(1.5));
    Object* object = allocateLabeledObject(program);
    const FieldData* countField = object->fieldNamed(program.state(), count);
    AOT_TEST_CHECK(holdsUnboxedNumber(countField));
    DeletePropertySlot deleteSlot;
    AOT_TEST_CHECK(object->methodTable()->deleteProperty(object, realm, count, deleteSlot) && isCompiledObject(object) && !object->isInSlot(*countField));
    object->setPrototypeKeepingShape(vm, prototype);
    Structure* structure = object->structure();
    auto find = [&] {
        return caches.tables().find(structure->id(), caches.nameOf(count.impl()), SlotAccess::Read);
    };

    PropertyAnswer inherited = readAndClassify(program, object, count);
    AOT_TEST_CHECK(inherited.kind == PropertyAnswerKind::FieldOrInheritedField && inherited.field == countField && inherited.holder == prototype && !inherited.absentIsUndefined);
    AOT_TEST_CHECK(inherited.changeableObjects.contains(prototype));
    caches.cache({ structure, caches.nameOf(count.impl()), SlotAccess::Read }, inherited);
    std::optional<Slot> slot = find();
    AOT_TEST_CHECK(slot && slot->kind == Slot::Kind::FieldOrInheritedField && slot->has(Slot::dependsOnChain) && slot->fieldSlot() == countField->slot);
    AOT_TEST_CHECK(MarkedBlockCage::decode(vm.heap.cage().base(), slot->fieldAndHolder.holder) == prototype);

    PropertyAnswer store = storeAndClassify(program, object, count, jsNumber(3));
    AOT_TEST_CHECK(store.kind != PropertyAnswerKind::FieldOrInheritedField && object->isInSlot(*countField));

    AOT_TEST_CHECK(prototype->methodTable()->deleteProperty(prototype, realm, count, deleteSlot));
    AOT_TEST_CHECK(!find());
}

static PropertyAnswer storeAndClassify(LoadedProgram& program, JSObject* object, const Identifier& name, JSValue value)
{
    Structure* structureBefore = object->structure();
    PutPropertySlot slot(object, true, PutPropertySlot::PutById);
    AOT_TEST_CHECK(object->methodTable()->put(object, program.realm(), name, value, slot) && !program.vm().exceptionForInspection());
    PropertyAnswer answer = classifyStore({ program.realm(), object, structureBefore, name.impl(), slot, PutMode::Strict });
    AOT_TEST_CHECK(answer.access == SlotAccess::Store);
    return answer;
}

static void defineAccessor(LoadedProgram& program, JSObject* object, const Identifier& name)
{
    PropertyDescriptor descriptor;
    descriptor.setAccessorDescriptor(GetterSetter::create(program.vm(), program.realm(), nullptr, program.realm()->throwTypeErrorFunction()), PropertyAttribute::Accessor | 0u);
    AOT_TEST_CHECK(object->methodTable()->defineOwnProperty(object, program.realm(), name, descriptor, true) && !program.vm().exceptionForInspection());
}

static void testSetterAboveEmptySlotOfCompiledPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    Identifier label = Identifier::fromString(vm, "label"_s);
    Identifier other = Identifier::fromString(vm, "other"_s);
    JSObject* top = constructEmptyObject(realm);
    defineAccessor(program, top, label);
    Object* prototype = allocateLabeledObject(program);
    const FieldData* labelField = prototype->fieldNamed(program.state(), label);
    prototype->clear(*labelField);
    prototype->setPrototypeKeepingShape(vm, top);
    JSObject* object = constructEmptyObject(realm, prototype);
    Structure* structure = object->structure();
    auto walk = [&](ChainWalkUse use) {
        return walkChain({ realm, structure, object, ChainWalkMode::MayAllocate, use }, label.impl());
    };

    AOT_TEST_CHECK(storeThroughChain(walk(ChainWalkUse::UsedAtOnce)) == StoreThroughChain::CallsSetter && walk(ChainWalkUse::UsedAtOnce).holder == top);
    AOT_TEST_CHECK(walk(ChainWalkUse::KeptForStores).result == ChainWalkResult::Unknown && walk(ChainWalkUse::KeptForReads).result == ChainWalkResult::Unknown);
    AOT_TEST_CHECK(walk(ChainWalkUse::KeptForReadsThatAskObjects).result == ChainWalkResult::Unknown && isLastingRefusal(readAndClassify(program, object, label)));

    putByName(program, prototype, other, jsNumber(1));
    AOT_TEST_CHECK(prototype->hasExtraProperties());
    AOT_TEST_CHECK(storeThroughChain(walk(ChainWalkUse::KeptForStores)) == StoreThroughChain::CallsSetter && walk(ChainWalkUse::KeptForReads).holder == top);
    AOT_TEST_CHECK(walk(ChainWalkUse::KeptForStores).changeableObjects.contains(prototype) && walk(ChainWalkUse::KeptForStores).changeableObjects.contains(top));
}

static void testAbsenceFilterNamesWhatStopsStores(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    PropertyCaches& caches = *program.state().propertyCaches;
    VMPropertyCaches& tables = caches.tables();
    auto validFilterOf = [&](JSObject* object) -> const AbsenceFilter* {
        const AbsenceFilter& filter = tables.absenceFilterFor(object->structureID());
        return filter.structureID == object->structureID().bits() && filter.generationForStores == tables.absenceFilterGenerationForStores() ? &filter : nullptr;
    };
    auto filterOf = [&](JSObject* object) {
        tables.fileAbsenceFilterForStores(realm, object);
        return validFilterOf(object);
    };
    auto makeBelow = [&](JSObject* prototype) {
        JSObject* made = constructEmptyObject(realm);
        made->setPrototypeDirect(vm, prototype);
        return made;
    };

    JSObject* above = constructEmptyObject(realm);
    JSObject* below = makeBelow(above);
    const AbsenceFilter* ofBelow = filterOf(below);
    AOT_TEST_CHECK(ofBelow);

    Identifier label;
    std::optional<uint32_t> number;
    for (ASCIILiteral name : { "label"_s, "count"_s, "publish"_s, "input"_s }) {
        label = Identifier::fromString(vm, name);
        number = caches.nameOf(label.impl()).identifierNumber;
        if (number && ofBelow->letsStoresPass(*number))
            break;
    }
    AOT_TEST_CHECK(number && ofBelow->letsStoresPass(*number));

    above->putDirect(vm, label, jsNumber(1));
    AOT_TEST_CHECK(validFilterOf(below) == ofBelow && ofBelow->letsStoresPass(*number) && !tables.wasChangedOnSomeChain(*number));

    AOT_TEST_CHECK(above->structure()->isDictionary() && flattenOnce(vm, above) && !above->structure()->isDictionary());
    AOT_TEST_CHECK(validFilterOf(below) == ofBelow && ofBelow->letsStoresPass(*number) && !tables.wasChangedOnSomeChain(*number));

    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        vm.heap.collectSync(scope);
        ensureStillAliveHere(below);
        AOT_TEST_CHECK(validFilterOf(below) == ofBelow && ofBelow->letsStoresPass(*number));
    }

    JSObject* owner = nullptr;
    unsigned numberOfOwners = 0;
    do {
        owner = constructEmptyObject(realm);
        owner->putDirect(vm, Identifier::fromString(vm, makeString("other"_s, numberOfOwners++)), jsNumber(0));
        owner->putDirect(vm, label, jsNumber(1), static_cast<unsigned>(PropertyAttribute::ReadOnly));
    } while (AbsenceFilter::indexFor(owner->structureID().bits()) == AbsenceFilter::indexFor(below->structureID().bits()));
    const AbsenceFilter* ofOwner = filterOf(owner);
    AOT_TEST_CHECK(ofOwner && ofOwner->letsStoresPass(*number));

    above->putDirect(vm, label, jsNumber(1), static_cast<unsigned>(PropertyAttribute::ReadOnly));
    AOT_TEST_CHECK(validFilterOf(below) == ofBelow && tables.wasChangedOnSomeChain(*number));
    JSObject* twoBelow = makeBelow(below);
    const AbsenceFilter* ofTwoBelow = filterOf(twoBelow);
    AOT_TEST_CHECK(ofTwoBelow && !ofTwoBelow->letsStoresPass(*number));

    JSObject* withAccessor = constructEmptyObject(realm);
    withAccessor->putDirectAccessor(realm, label, GetterSetter::create(vm, realm, nullptr, nullptr), static_cast<unsigned>(PropertyAttribute::Accessor));
    const AbsenceFilter* ofBelowAccessor = filterOf(makeBelow(withAccessor));
    AOT_TEST_CHECK(ofBelowAccessor && !ofBelowAccessor->letsStoresPass(*number));

    above->setPrototypeDirect(vm, constructEmptyObject(realm));
    AOT_TEST_CHECK(!validFilterOf(below) && !validFilterOf(owner));

    AOT_TEST_CHECK(!filterOf(makeBelow(ProxyObject::create(realm, constructEmptyObject(realm), constructEmptyObject(realm)))));

    JSObject* earlier = makeBelow(constructEmptyObject(realm));
    const AbsenceFilter* place = filterOf(earlier);
    JSObject* later = nullptr;
    do {
        later = makeBelow(constructEmptyObject(realm));
    } while (AbsenceFilter::indexFor(later->structureID().bits()) != AbsenceFilter::indexFor(earlier->structureID().bits()));
    AOT_TEST_CHECK(place && validFilterOf(earlier) == place && !validFilterOf(later));
    AOT_TEST_CHECK(filterOf(later) == place && !validFilterOf(earlier));
    AOT_TEST_CHECK(filterOf(earlier) == place && !validFilterOf(later));
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<propertyLookupTests, programText, body>;

template<LoadedProgramTest body>
static constexpr void (*withProgramAddingProperties)() = runNativeTestWithLoadedProgram<propertyLookupTests, programAddingPropertiesText, body>;

template<LoadedProgramTest body>
static void changeObjectsUnseen(LoadedProgram& program)
{
    program.vm().aotState().nativeTestChangesObjectsUnseen = true;
    body(program);
}

template<LoadedProgramTest body>
static constexpr void (*withProgramReadingCount)() = runNativeTestWithLoadedProgram<propertyLookupTests, programReadingCountText, changeObjectsUnseen<body>>;

template<LoadedProgramTest body>
static constexpr void (*withProgramCreatingSymbol)() = runNativeTestWithLoadedProgram<propertyLookupTests, programCreatingSymbolText, body>;

static void runSymbolOfEachRealmIsNeverFiledInFront()
{
    runWithLoadedProgram("property-lookup/symbol-of-each-realm-is-never-filed-in-front"_s, programCreatingSymbolText, testSymbolOfEachRealmIsNeverFiledInFront, { "several-realms"_s });
}

std::span<const NativeTest> propertyLookupTests()
{
    static constexpr NativeTest tests[] = {
        { "property-lookup/own-accessor"_s, withProgram<testOwnAccessor> },
        { "property-lookup/underscore-proto-and-index"_s, withProgram<testUnderscoreProtoAndIndex> },
        { "property-lookup/dictionary"_s, withProgram<testDictionary> },
        { "property-lookup/other-prototype"_s, withProgram<testOtherPrototype> },
        { "property-lookup/other-base"_s, withProgram<testOtherBase> },
        { "property-lookup/length-of-typed-arrays"_s, withProgram<testLengthOfTypedArrays> },
        { "property-lookup/last-index-of-regexps"_s, withProgram<testLastIndexOfRegExps> },
        { "property-lookup/each-place-of-answers-waits-by-a-state-of-its-own"_s, testEachPlaceOfAnswersWaitsByStateOfItsOwn },
        { "property-lookup/indexes-are-not-names"_s, withProgram<testIndexesAreNotNames> },
        { "property-lookup/absence-filter-names-what-stops-stores"_s, withProgram<testAbsenceFilterNamesWhatStopsStores> },
        { "property-lookup/slot-filed-by-number-is-copied-for-keys"_s, withProgram<testSlotFiledByNumberIsCopiedForKeys> },
        { "property-lookup/symbol-of-one-realm-is-filed-in-front"_s, withProgramCreatingSymbol<testSymbolOfOneRealmIsFiledInFront> },
        { "property-lookup/symbol-of-each-realm-is-never-filed-in-front"_s, runSymbolOfEachRealmIsNeverFiledInFront },
        { "property-lookup/evicted-dependents-are-dropped-when-the-table-has-doubled"_s, withProgram<testEvictedDependentsAreDroppedWhenTheTableHasDoubled> },
        { "property-lookup/named-changes-of-an-object-with-many-dependents"_s, withProgram<testNamedChangesOfAnObjectWithManyDependents> },
        { "property-lookup/reads-below-empty-slots-ask-the-first-object-that-has-one"_s, withProgram<testReadsBelowEmptySlotsAskTheFirstObjectThatHasOne> },
        { "property-lookup/empty-slot-of-receiver-defers-to-unboxed-field-of-compiled-prototype"_s, withProgramReadingCount<testEmptySlotOfReceiverDefersToUnboxedFieldOfCompiledPrototype> },
        { "property-lookup/setter-above-empty-slot-of-compiled-prototype"_s, withProgramAddingProperties<testSetterAboveEmptySlotOfCompiledPrototype> },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
