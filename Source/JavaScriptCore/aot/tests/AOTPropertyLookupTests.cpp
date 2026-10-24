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
#include "AOTRuntimeCounters.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSlowPaths.h"
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

static constexpr ASCIILiteral programText =
    "layout L0 name=\"Labeled\" module=m0 fields=[{key=\"label\" declared={type=anything} flags=[IsRequired]}, {key=\"count\" declared={type=number} flags=[IsRequired]}]\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "function f0 name=\"main\" module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"publish\" [isFreeName]\n"
    "    v2 = PropGet v0 key=\"input\" [isFreeName]\n"
    "    v3 = Check v2 castType=number\n"
    "    v4 = NewObject -, v2, v3 layout=L0 fields=[0, 1]\n"
    "    v5 = Call v1, -, v4\n"
    "    Return\n"
    "}\n"
    "entry module=m0\n"_s;

// A store under a key that nobody can name: the layout has a word for extra properties.
static constexpr ASCIILiteral programAddingPropertiesText =
    "layout L0 name=\"Labeled\" module=m0 fields=[{key=\"label\" declared={type=anything} flags=[IsRequired]}, {key=\"count\" declared={type=number} flags=[IsRequired]}]\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "function f0 name=\"main\" module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"publish\" [isFreeName]\n"
    "    v2 = PropGet v0 key=\"input\" [isFreeName]\n"
    "    v3 = Check v2 castType=number\n"
    "    v4 = NewObject -, v2, v3 layout=L0 fields=[0, 1]\n"
    "    v5 = Check v2 castType=string\n"
    "    ElemSet v4, v5, v2\n"
    "    v7 = Call v1, -, v4\n"
    "    Return\n"
    "}\n"
    "entry module=m0\n"_s;

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

static void testOwnDataProperty(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSObject* object = constructEmptyObject(program.realm());
    AOT_TEST_CHECK(object->type() == FinalObjectType);

    constexpr unsigned numberOfProperties = JSFinalObject::defaultInlineCapacity + 2;
    for (unsigned index = 0; index < numberOfProperties; ++index)
        object->putDirect(vm, Identifier::fromString(vm, makeString("field"_s, index)), jsNumber(index));
    for (unsigned index = 0; index < numberOfProperties; ++index) {
        Identifier name = Identifier::fromString(vm, makeString("field"_s, index));
        AOT_TEST_CHECK(findsOwnValue(program, object, name));
        AOT_TEST_CHECK(presenceAfterRead(program, object, name) == Presence::Present);
    }
    AOT_TEST_CHECK(isOutOfLineOffset(object->structure()->maxOffset()));

    Identifier hidden = Identifier::fromString(vm, "hidden"_s);
    object->putDirect(vm, hidden, jsNull(), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum);
    AOT_TEST_CHECK(findsOwnValue(program, object, hidden));
}

static void testAbsentName(LoadedProgram& program)
{
    VM& vm = program.vm();
    Identifier label = Identifier::fromString(vm, "label"_s);
    Identifier other = Identifier::fromString(vm, "other"_s);
    JSObject* object = constructEmptyObject(program.realm());
    AOT_TEST_CHECK(object->getPrototypeDirect() == JSValue(program.realm()->objectPrototype()));
    AOT_TEST_CHECK(program.realm()->objectPrototype()->structure()->hasImmutableProperties());
    auto isAbsentForGood = [&] {
        PropertyAnswer answer = readAndClassify(program, object, label);
        return isAbsent(answer) && !answer.dependsOnChain() && presenceAfterRead(program, object, label) == Presence::Absent;
    };

    AOT_TEST_CHECK(isAbsentForGood());
    object->putDirect(vm, other, jsNumber(7));
    AOT_TEST_CHECK(isAbsentForGood());
    object->putDirect(vm, label, jsUndefined());
    AOT_TEST_CHECK(findsOwnValue(program, object, label));
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

static void testNameOfObjectPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    const Identifier& toString = vm.propertyNames->toString;
    JSObject* objectPrototype = program.realm()->objectPrototype();
    JSObject* object = constructEmptyObject(program.realm());
    AOT_TEST_CHECK(objectPrototype->structure()->isImmutableProperty(toString.impl()));

    PropertyAnswer answer = readAndClassify(program, object, toString);
    AOT_TEST_CHECK(isInheritedFrom(program, answer, PropertyAnswerKind::Value, objectPrototype, toString) && !answer.dependsOnChain());
    AOT_TEST_CHECK(presenceAfterRead(program, object, toString) == Presence::Present);
    object->putDirect(vm, toString, jsNumber(7));
    AOT_TEST_CHECK(findsOwnValue(program, object, toString));
}

static void testUnderscoreProtoAndIndex(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    const Identifier& underscoreProto = vm.propertyNames->underscoreProto;
    Identifier index = Identifier::from(vm, 0u);
    JSObject* object = constructEmptyObject(realm);

    AOT_TEST_CHECK(isLastingRefusal(readAndClassify(program, object, underscoreProto)));
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

    // A cacheable dictionary says where a property is, and not that one is absent.
    object->convertToDictionary(vm);
    AOT_TEST_CHECK(PropertyWalk::hasPropertyOnChain(object->structure(), label.impl()) == Presence::Unknown);
    AOT_TEST_CHECK(isOwn(readAndClassify(program, object, label, ChainWalkMode::AllocatesNothing), PropertyAnswerKind::Value, object->getDirectOffset(vm, label)));
    AOT_TEST_CHECK(findsOwnValue(program, object, label));
    AOT_TEST_CHECK(isLastingRefusal(readAndClassify(program, object, other, ChainWalkMode::AllocatesNothing)));
    AOT_TEST_CHECK(object->structure()->isDictionary());

    PropertyAnswer answer = readAndClassify(program, object, other);
    AOT_TEST_CHECK(answer.kind == PropertyAnswerKind::Uncacheable && answer.didFlatten && !object->structure()->isDictionary());
    AOT_TEST_CHECK(findsOwnValue(program, object, label));
    AOT_TEST_CHECK(isAbsent(readAndClassify(program, object, other)));

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
        AOT_TEST_CHECK_FOR_SOURCE(slot.kind == Slot::Kind::TypedArrayLength && !slot.flags && !slot.payloadBits(), source);
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
    AOT_TEST_CHECK(readAndClassify(program, realms.object(program.realm(), "new Uint8Array(4)"_s), vm.propertyNames->byteOffset).kind == PropertyAnswerKind::Accessor);
}

static JSString* createKeyWithoutAtom(VM& vm)
{
    JSString* key = jsString(vm, makeString("la"_s, "bel"_s));
    AOT_TEST_CHECK(!key->tryGetValueImpl()->isAtom());
    return key;
}

static JSString* createRope(LoadedProgram& program, unsigned length)
{
    auto createHalf = [&](Latin1Character character) {
        Vector<Latin1Character> characters(FillWith { }, length / 2, character);
        return jsNontrivialString(program.vm(), String(characters.span()));
    };
    JSString* rope = jsString(program.realm(), createHalf('q'), createHalf('z'));
    AOT_TEST_CHECK(rope && rope->isRope() && rope->length() == length);
    return rope;
}

static bool isName(KeyName name, UniquedStringImpl* uid)
{
    return name.kind == KeyNameKind::Name && name.uid == uid;
}

static void testOnlyKeysWithAtomsAreNames(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    Identifier label = Identifier::fromString(vm, "label"_s);
    JSString* keyWithAtom = jsString(vm, label.string().string());
    JSString* keyWithoutAtom = createKeyWithoutAtom(vm);
    JSString* ropeOfAtoms = jsString(realm, jsNontrivialString(vm, "la"_s), jsNontrivialString(vm, "bel"_s));
    AOT_TEST_CHECK(ropeOfAtoms->isRope());

    AOT_TEST_CHECK(isName(nameOfKeyWithoutAllocating(realm, keyWithAtom, KeyAtomization::OnlyExisting), label.impl()));
    AOT_TEST_CHECK(isName(nameOfKeyWithoutAllocating(realm, keyWithoutAtom, KeyAtomization::OnlyExisting), label.impl()));
    AOT_TEST_CHECK(keyWithoutAtom->tryGetValueImpl() == label.impl() && atomOfKey(keyWithoutAtom) == label.impl());
    AOT_TEST_CHECK(isName(nameOfKeyWithoutAllocating(realm, ropeOfAtoms, KeyAtomization::OnlyExisting), label.impl()));
    AOT_TEST_CHECK(ropeOfAtoms->tryGetValueImpl() == label.impl());

    JSString* keyOfNoProperty = jsNontrivialString(vm, makeString("a name that "_s, "no property has"_s));
    AOT_TEST_CHECK(nameOfKeyWithoutAllocating(realm, keyOfNoProperty, KeyAtomization::OnlyExisting).kind == KeyNameKind::HasNoAtom);
    AOT_TEST_CHECK(!keyOfNoProperty->tryGetValueImpl()->isAtom() && !atomOfKey(keyOfNoProperty));
    KeyName createdName = nameOfKeyWithoutAllocating(realm, keyOfNoProperty, KeyAtomization::Atomize);
    AOT_TEST_CHECK(createdName.uid && isName(createdName, atomOfKey(keyOfNoProperty)));

    AOT_TEST_CHECK(nameOfKeyWithoutAllocating(realm, createRope(program, 8), KeyAtomization::OnlyExisting).kind == KeyNameKind::NotAName);
    JSString* longRope = createRope(program, 300);
    AOT_TEST_CHECK(nameOfKeyWithoutAllocating(realm, longRope, KeyAtomization::OnlyExisting).kind == KeyNameKind::HasNoAtom);
    AOT_TEST_CHECK(nameOfKeyWithoutAllocating(realm, longRope, KeyAtomization::Atomize).kind == KeyNameKind::Name);
    for (KeyAtomization atomization : { KeyAtomization::OnlyExisting, KeyAtomization::Atomize })
        AOT_TEST_CHECK(nameOfKeyWithoutAllocating(realm, createRope(program, JSString::maxLengthForOnStackResolve + 2), atomization).kind == KeyNameKind::NotAName);

    Symbol* symbol = Symbol::create(vm);
    PrivateName privateName(PrivateName::PrivateSymbol, "secret"_s);
    AOT_TEST_CHECK(isName(nameOfKeyWithoutAllocating(realm, symbol, KeyAtomization::OnlyExisting), &symbol->uid()) && uidOfKey(symbol) == &symbol->uid());
    AOT_TEST_CHECK(nameOfKeyWithoutAllocating(realm, Symbol::create(vm, privateName.uid()), KeyAtomization::OnlyExisting).kind == KeyNameKind::NotAName);
    AOT_TEST_CHECK(nameOfKeyWithoutAllocating(realm, constructEmptyObject(realm), KeyAtomization::OnlyExisting).kind == KeyNameKind::NotAName);
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

static void testPresenceIsCachedUnderName(LoadedProgram& program)
{
    VM& vm = program.vm();
    HashedSlotTable& table = program.state().propertyCaches->tables().hashedTable();
    Identifier label = Identifier::fromString(vm, "label"_s);
    Identifier other = Identifier::fromString(vm, "other"_s);
    AOT_TEST_CHECK(vm.aotState().identifiers->numberOf(label.impl()));
    AOT_TEST_CHECK(!vm.aotState().identifiers->numberOf(other.impl()));

    Structure* emptyStructure = program.realm()->nullPrototypeObjectStructure();
    JSObject* objectWithLabel = constructEmptyObject(vm, emptyStructure);
    objectWithLabel->putDirect(vm, label, jsNumber(7));
    JSObject* objectWithOther = constructEmptyObject(vm, emptyStructure);
    objectWithOther->putDirect(vm, other, jsNumber(7));
    auto hasLabel = [&](JSObject* object) {
        return static_cast<LeafPresence>(operationAOTInByValGeneric(&program.instance(), object, createKeyWithoutAtom(vm)));
    };
    auto findLabel = [&](JSObject* object) {
        return table.find(object->structure()->id(), label.impl(), SlotAccess::Read);
    };

    AOT_TEST_CHECK(!findLabel(objectWithLabel) && !findLabel(objectWithOther));
    // A pair is filed at its third miss.
    for (unsigned miss = 0; miss < 2; ++miss)
        AOT_TEST_CHECK(hasLabel(objectWithLabel) == LeafPresence::Own && hasLabel(objectWithOther) == LeafPresence::Absent && !findLabel(objectWithLabel) && !findLabel(objectWithOther));
    AOT_TEST_CHECK(hasLabel(objectWithLabel) == LeafPresence::Own);
    const Slot* present = findLabel(objectWithLabel);
    AOT_TEST_CHECK(present && present->kind == Slot::Kind::Value && present->has(Slot::isOwn) && Slot::presenceOf(present->kind) == SlotPresence::Present);
    AOT_TEST_CHECK(present->offsetInStorage() == objectWithLabel->getDirectOffset(vm, label));

    AOT_TEST_CHECK(hasLabel(objectWithOther) == LeafPresence::Absent);
    const Slot* absent = findLabel(objectWithOther);
    AOT_TEST_CHECK(absent && absent->kind == Slot::Kind::Absent && Slot::presenceOf(absent->kind) == SlotPresence::Absent);
    AOT_TEST_CHECK(hasLabel(objectWithLabel) == LeafPresence::Own && hasLabel(objectWithOther) == LeafPresence::Absent);

    JSObject* heir = constructEmptyObject(program.realm(), objectWithLabel);
    for (unsigned miss = 0; miss < 2; ++miss)
        AOT_TEST_CHECK(hasLabel(heir) == LeafPresence::Inherited && !findLabel(heir));
    AOT_TEST_CHECK(hasLabel(heir) == LeafPresence::Inherited);
    const Slot* inherited = findLabel(heir);
    AOT_TEST_CHECK(inherited && Slot::presenceOf(inherited->kind) == SlotPresence::Present && !inherited->has(Slot::isOwn));
    AOT_TEST_CHECK(hasLabel(heir) == LeafPresence::Inherited);
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

static void testLeafOperationsAnswerByName(LoadedProgram& program)
{
    VM& vm = program.vm();
    Instance* instance = &program.instance();
    HashedSlotTable& table = program.state().propertyCaches->tables().hashedTable();
    Identifier nameWithDigit = Identifier::fromString(vm, "3d"_s);
    Identifier addedName = Identifier::fromString(vm, "4e"_s);
    Identifier absentName = Identifier::fromString(vm, "5f"_s);
    JSObject* object = constructEmptyObject(vm, program.realm()->nullPrototypeObjectStructure());
    object->putDirect(vm, nameWithDigit, jsNumber(7));
    Structure* structure = object->structure();
    auto keyFor = [&](const Identifier& name) {
        JSString* key = jsNontrivialString(vm, makeString(name.string().string().left(1), name.string().string().substring(1)));
        AOT_TEST_CHECK(!key->tryGetValueImpl()->isAtom());
        return key;
    };
    auto get = [&](JSCell* key) {
        return JSValue::decode(operationAOTGetByValGeneric(instance, object, key));
    };
    auto has = [&](JSCell* key) {
        return static_cast<LeafPresence>(operationAOTInByValGeneric(instance, object, key));
    };

    AOT_TEST_CHECK(JSValue::decode(operationAOTGetGeneric(instance, object, nameWithDigit.impl())) == jsNumber(7));
    AOT_TEST_CHECK(JSValue::decode(operationAOTGetGeneric(instance, object, absentName.impl())).isUndefined());
    AOT_TEST_CHECK(!table.find(structure->id(), nameWithDigit.impl(), SlotAccess::Read) && !table.find(structure->id(), absentName.impl(), SlotAccess::Read));

    AOT_TEST_CHECK(get(keyFor(nameWithDigit)) == jsNumber(7) && has(keyFor(nameWithDigit)) == LeafPresence::Own);
    AOT_TEST_CHECK(get(keyFor(absentName)).isUndefined() && has(keyFor(absentName)) == LeafPresence::Absent);
    // A pair is filed at its third miss.
    AOT_TEST_CHECK(!table.find(structure->id(), nameWithDigit.impl(), SlotAccess::Read) && !table.find(structure->id(), absentName.impl(), SlotAccess::Read));
    AOT_TEST_CHECK(get(keyFor(nameWithDigit)) == jsNumber(7) && get(keyFor(absentName)).isUndefined());
    AOT_TEST_CHECK(table.find(structure->id(), nameWithDigit.impl(), SlotAccess::Read) && table.find(structure->id(), absentName.impl(), SlotAccess::Read));
    JSString* index = jsString(vm, String("0"_s));
    AOT_TEST_CHECK(!get(index) && has(index) == LeafPresence::TakeSlowPath);
    JSString* keyOfNoProperty = jsNontrivialString(vm, makeString("a name that "_s, "no object has"_s));
    AOT_TEST_CHECK(get(keyOfNoProperty).isUndefined() && has(keyOfNoProperty) == LeafPresence::Absent && !atomOfKey(keyOfNoProperty));

    auto put = [&](JSCell* key, JSValue value) {
        return static_cast<LeafAnswer>(operationAOTPutByValGeneric(instance, object, key, JSValue::encode(value)));
    };
    AOT_TEST_CHECK(put(keyFor(nameWithDigit), jsNumber(6)) == LeafAnswer::Yes && object->getDirect(vm, nameWithDigit) == jsNumber(6));
    // The first store of a Structure and a key files nothing.
    AOT_TEST_CHECK(!table.find(structure->id(), nameWithDigit.impl(), SlotAccess::Store));
    AOT_TEST_CHECK(put(keyFor(nameWithDigit), jsNumber(8)) == LeafAnswer::Yes && object->getDirect(vm, nameWithDigit) == jsNumber(8));
    const Slot* replacement = table.find(structure->id(), nameWithDigit.impl(), SlotAccess::Store);
    AOT_TEST_CHECK(replacement && replacement->kind == Slot::Kind::Value && replacement->has(Slot::isOwn));
    AOT_TEST_CHECK(put(keyFor(addedName), jsNumber(9)) == LeafAnswer::Yes && object->getDirect(vm, addedName) == jsNumber(9));
    // The first object that is given a name becomes a dictionary, the second gets a Structure for it, and the third addition is filed.
    AOT_TEST_CHECK(object->structure()->isDictionary() && !table.find(structure->id(), addedName.impl(), SlotAccess::Store));
    auto makeNextObject = [&] {
        JSObject* nextObject = constructEmptyObject(vm, program.realm()->nullPrototypeObjectStructure());
        nextObject->putDirect(vm, nameWithDigit, jsNumber(7));
        AOT_TEST_CHECK(nextObject->structure() == structure);
        AOT_TEST_CHECK(static_cast<LeafAnswer>(operationAOTPutByValGeneric(instance, nextObject, keyFor(addedName), JSValue::encode(jsNumber(9)))) == LeafAnswer::Yes && nextObject->getDirect(vm, addedName) == jsNumber(9));
        return nextObject;
    };
    JSObject* secondObject = makeNextObject();
    AOT_TEST_CHECK(!secondObject->structure()->isDictionary() && !table.find(structure->id(), addedName.impl(), SlotAccess::Store));
    JSObject* thirdObject = makeNextObject();
    const Slot* addition = table.find(structure->id(), addedName.impl(), SlotAccess::Store);
    AOT_TEST_CHECK(addition && addition->kind == Slot::Kind::Transition && addition->transition.newStructureID == secondObject->structure()->id() && thirdObject->structure() == secondObject->structure());
    AOT_TEST_CHECK(put(index, jsNumber(1)) == LeafAnswer::TakeSlowPath);
}

static void testLeafOperationsAnswerByIndex(LoadedProgram& program)
{
    VM& vm = program.vm();
    Instance* instance = &program.instance();
    Realms& realms = program.realms();
    HashedSlotTable& table = program.state().propertyCaches->tables().hashedTable();
    auto keyFor = [&](unsigned index) {
        return jsString(vm, String::number(index));
    };
    auto get = [&](JSCell* base, unsigned index) {
        return JSValue::decode(operationAOTGetByValGeneric(instance, base, keyFor(index)));
    };
    auto has = [&](JSCell* base, unsigned index) {
        return static_cast<LeafPresence>(operationAOTInByValGeneric(instance, base, keyFor(index)));
    };
    auto put = [&](JSCell* base, unsigned index, JSValue value) {
        return static_cast<LeafAnswer>(operationAOTPutByValGeneric(instance, base, keyFor(index), JSValue::encode(value)));
    };
    auto isNumber = [](JSValue value, double number) {
        return value && value.isNumber() && value.asNumber() == number;
    };

    for (ASCIILiteral source : { "new Array(10, 11, 12)"_s, "new Array(10, 11.5, 12)"_s, "new Array(10, 'b', 12)"_s, "Object.assign({ }, [10, 11, 12])"_s, "new Int32Array([10, 11, 12])"_s }) {
        JSObject* object = realms.object(program.realm(), source);
        Structure* structure = object->structure();
        AOT_TEST_CHECK_FOR_SOURCE(isNumber(get(object, 0), 10) && isNumber(get(object, 2), 12) && has(object, 2) == LeafPresence::Own, source);
        AOT_TEST_CHECK_FOR_SOURCE(!get(object, 3) && has(object, 3) == LeafPresence::TakeSlowPath, source);
        AOT_TEST_CHECK_FOR_SOURCE(put(object, 2, jsNumber(13)) == LeafAnswer::Yes && isNumber(object->getIndexQuickly(2), 13), source);
        AOT_TEST_CHECK_FOR_SOURCE(put(object, 100000, jsNumber(1)) == LeafAnswer::TakeSlowPath && !object->canGetIndexQuickly(100000u), source);
        for (SlotAccess access : { SlotAccess::Read, SlotAccess::Store })
            AOT_TEST_CHECK_FOR_SOURCE(!table.find(structure->id(), Identifier::fromString(vm, "2"_s).impl(), access), source);
    }

    JSObject* arrayWithHole = realms.object(program.realm(), "[10, , 12]"_s);
    AOT_TEST_CHECK(isNumber(get(arrayWithHole, 0), 10) && !get(arrayWithHole, 1) && has(arrayWithHole, 1) == LeafPresence::TakeSlowPath);

    struct RefusedStore {
        ASCIILiteral source;
        unsigned index;
    };
    static constexpr RefusedStore refusedStores[] = {
        { "Object.freeze(new Array(10, 11))"_s, 1 },
        { "Object.freeze([10, 11])"_s, 1 },
        { "Object.preventExtensions(new Array(10, 11))"_s, 2 },
    };
    for (const RefusedStore& refused : refusedStores) {
        JSObject* object = realms.object(program.realm(), refused.source);
        AOT_TEST_CHECK_FOR_SOURCE(put(object, refused.index, jsNumber(5)) == LeafAnswer::TakeSlowPath, refused.source);
        AOT_TEST_CHECK_FOR_SOURCE(isNumber(object->get(program.realm(), 1u), 11) && object->get(program.realm(), 2u).isUndefined(), refused.source);
    }

    JSObject* proxy = realms.object(program.realm(), "new Proxy([10, 11], { })"_s);
    AOT_TEST_CHECK(!get(proxy, 0) && has(proxy, 0) == LeafPresence::TakeSlowPath && put(proxy, 0, jsNumber(5)) == LeafAnswer::TakeSlowPath);
    JSString* text = jsNontrivialString(vm, "text"_s);
    AOT_TEST_CHECK(!get(text, 0) && has(text, 0) == LeafPresence::TakeSlowPath && put(text, 0, jsNumber(5)) == LeafAnswer::TakeSlowPath);
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

static void testNamedChangeDropsOnlyThatName(LoadedProgram& program)
{
    VM& vm = program.vm();
    ObjectBelowPrototype fixture(program);
    ChainDependents& dependents = *program.state().chainDependents;
    uint64_t numberDroppedBefore = valueOf(vm, RuntimeCounterID::ChainDependentsDropped);
    fixture.file(fixture.label);
    fixture.file(fixture.other);
    AOT_TEST_CHECK(fixture.isFiledAsAbsent(fixture.label) && fixture.isFiledAsAbsent(fixture.other));
    AOT_TEST_CHECK(fixture.caches.tables().front().hasRow(fixture.object->structure()->id()));
    AOT_TEST_CHECK(fixture.caches.tables().hashedTable().find(fixture.object->structure()->id(), fixture.other.impl(), SlotAccess::Read));
    AOT_TEST_CHECK(dependents.numberOfDependents(fixture.prototype) == 2);

    fixture.prototype->putDirect(vm, fixture.label, jsNumber(1));

    AOT_TEST_CHECK(!fixture.find(fixture.label) && fixture.isFiledAsAbsent(fixture.other));
    AOT_TEST_CHECK(dependents.numberOfDependents(fixture.prototype) == 1);
    AOT_TEST_CHECK(valueOf(vm, RuntimeCounterID::ChainDependentsDropped) == numberDroppedBefore + 1);

    fixture.file(fixture.label);
    std::optional<Slot> inherited = fixture.find(fixture.label);
    AOT_TEST_CHECK(inherited && inherited->kind == Slot::Kind::Value && inherited->holder == fixture.prototype && inherited->has(Slot::dependsOnChain));
    AOT_TEST_CHECK(inherited->offsetInStorage() == fixture.prototype->getDirectOffset(vm, fixture.label));
    bool isInFront = !!fixture.caches.tables().front().find(fixture.object->structure()->id(), *fixture.caches.nameOf(fixture.label.impl()).identifierNumber, SlotAccess::Read);
    AOT_TEST_CHECK(isInFront == vm.heap.cage().contains(fixture.prototype));
}

// Nobody tells the table when an owner evicts what depends on a chain. A full collection asks the owners, but only when as many dependents were added as were left the last time.
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
    // It does no harm: the change finds nothing to drop.
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
        uint64_t numberDroppedBefore = valueOf(vm, RuntimeCounterID::ChainDependentsDropped);
        fixture.prototype->putDirect(vm, nameAt(index), jsNumber(index));
        AOT_TEST_CHECK(valueOf(vm, RuntimeCounterID::ChainDependentsDropped) == numberDroppedBefore + 1);
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

static void testNamelessChangeDropsEveryName(LoadedProgram& program)
{
    VM& vm = program.vm();
    ObjectBelowPrototype fixture(program);
    ChainDependents& dependents = *program.state().chainDependents;
    uint64_t numberDroppedBefore = valueOf(vm, RuntimeCounterID::ChainDependentsDropped);
    JSObject* objectWithoutPrototype = constructEmptyObject(vm, program.realm()->nullPrototypeObjectStructure());
    fixture.file(fixture.label);
    fixture.file(fixture.other);
    fixture.caches.cache({ objectWithoutPrototype->structure(), fixture.caches.nameOf(fixture.label.impl()), SlotAccess::Read }, readAndClassify(program, objectWithoutPrototype, fixture.label));
    AOT_TEST_CHECK(fixture.isFiledAsAbsent(fixture.label) && fixture.isFiledAsAbsent(fixture.other));

    didChangeChainObject(vm, fixture.prototype, nullptr);

    AOT_TEST_CHECK(!fixture.find(fixture.label) && !fixture.find(fixture.other));
    AOT_TEST_CHECK(!dependents.hasDependents(fixture.prototype));
    AOT_TEST_CHECK(valueOf(vm, RuntimeCounterID::ChainDependentsDropped) == numberDroppedBefore + 2);
    std::optional<Slot> independent = fixture.caches.tables().find(objectWithoutPrototype->structure()->id(), fixture.caches.nameOf(fixture.label.impl()), SlotAccess::Read);
    AOT_TEST_CHECK(independent && independent->kind == Slot::Kind::Absent && !independent->has(Slot::dependsOnChain));
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

// What compiled code does: nobody hears of it.
static bool storeInSlot(LoadedProgram& program, Object* object, const FieldData& field, JSValue value)
{
    std::optional<AcceptedValue> accepted = program.state().objects->layouts.accept(field, value);
    return accepted && object->store(program.vm(), field, *accepted);
}

static void putByName(LoadedProgram& program, JSObject* object, const Identifier& name, JSValue value)
{
    PutPropertySlot slot(object, true);
    AOT_TEST_CHECK(object->methodTable()->put(object, program.realm(), name, value, slot) && !program.vm().exceptionForInspection());
}

static void testFieldsOfCompiledPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    PropertyCaches& caches = *program.state().propertyCaches;
    Identifier label = Identifier::fromString(vm, "label"_s);
    Identifier count = Identifier::fromString(vm, "count"_s);
    Object* prototype = allocateLabeledObject(program);
    JSObject* object = constructEmptyObject(realm, prototype);
    Structure* structure = object->structure();
    const FieldData* labelField = prototype->fieldNamed(program.state(), label);
    AOT_TEST_CHECK(labelField && prototype->structure()->mayBePrototype());
    auto find = [&](const Identifier& name) {
        return caches.tables().find(structure->id(), caches.nameOf(name.impl()), SlotAccess::Read);
    };
    auto getByKey = [&](const Identifier& name) {
        return JSValue::decode(operationAOTGetByValGeneric(&program.instance(), object, jsNontrivialString(vm, name.string())));
    };

    // Compiled code fills an empty slot without a report. What it puts there is a writable data property, so a store below finds the same before and after.
    prototype->clear(*labelField);
    // A read asks the prototype each time.
    PropertyAnswer belowEmptySlot = readAndClassify(program, object, label);
    AOT_TEST_CHECK(belowEmptySlot.kind == PropertyAnswerKind::InheritedField && belowEmptySlot.holder == prototype && belowEmptySlot.changeableObjects.contains(prototype));
    AOT_TEST_CHECK(storeCreatesOwnProperty(realm, structure, object, label.impl()));

    putByName(program, prototype, label, jsNumber(7));
    PropertyAnswer inherited = readAndClassify(program, object, label);
    AOT_TEST_CHECK(inherited.kind == PropertyAnswerKind::Value && !inherited.isOwn && inherited.holder == prototype && dependsOnlyOn(inherited, prototype));
    AOT_TEST_CHECK(readThroughAnswer(realm, object, inherited) == jsNumber(7));
    // The leaf operation goes up to a compiled object only for a pair that it files, and it files a pair at its third miss.
    AOT_TEST_CHECK(!getByKey(label) && !getByKey(label));
    AOT_TEST_CHECK(getByKey(label) == jsNumber(7));
    AOT_TEST_CHECK(storeCreatesOwnProperty(realm, structure, object, label.impl()));

    // The answer names the word of the slot, so it holds after a store that nobody hears of.
    AOT_TEST_CHECK(storeInSlot(program, prototype, *labelField, jsNontrivialString(vm, "text"_s)));
    AOT_TEST_CHECK(readThroughAnswer(realm, object, inherited).isString());

    // A slot with an unboxed number has no word that a read by offset could take: the prototype is asked for its field. A store below it still meets a writable data property.
    putByName(program, prototype, count, jsNumber(1.5));
    PropertyAnswer belowUnboxedSlot = readAndClassify(program, object, count);
    AOT_TEST_CHECK(belowUnboxedSlot.kind == PropertyAnswerKind::InheritedField && !belowUnboxedSlot.isOwn && belowUnboxedSlot.holder == prototype && dependsOnlyOn(belowUnboxedSlot, prototype));
    AOT_TEST_CHECK(!getByKey(count));
    AOT_TEST_CHECK(storeCreatesOwnProperty(realm, structure, object, count.impl()));

    caches.cache({ structure, caches.nameOf(label.impl()), SlotAccess::Read }, inherited);
    std::optional<Slot> filed = find(label);
    AOT_TEST_CHECK(filed && filed->kind == Slot::Kind::Value && filed->holder == prototype && filed->has(Slot::dependsOnChain));
    DeletePropertySlot deleteSlot;
    AOT_TEST_CHECK(prototype->methodTable()->deleteProperty(prototype, realm, label, deleteSlot));
    AOT_TEST_CHECK(!find(label) && readAndClassify(program, object, label).kind == PropertyAnswerKind::InheritedField);

    putByName(program, prototype, label, jsNumber(9));
    caches.cache({ structure, caches.nameOf(label.impl()), SlotAccess::Read }, readAndClassify(program, object, label));
    AOT_TEST_CHECK(find(label));
    prototype->setIntegrity(realm, Object::Integrity::Frozen);
    AOT_TEST_CHECK(!find(label));
    AOT_TEST_CHECK(!storeCreatesOwnProperty(realm, structure, object, label.impl()) && !storeCreatesOwnProperty(realm, structure, object, count.impl()));
    AOT_TEST_CHECK(readAndClassify(program, object, label).kind == PropertyAnswerKind::Value);
}

static void defineAccessor(LoadedProgram&, JSObject*, const Identifier&);
static PropertyAnswer storeAndClassify(LoadedProgram&, JSObject*, const Identifier&, JSValue);

static void testUnboxedFieldsOfCompiledPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    PropertyCaches& caches = *program.state().propertyCaches;
    Identifier count = Identifier::fromString(vm, "count"_s);
    auto isFiledFor = [&](JSObject* object) {
        std::optional<Slot> slot = caches.tables().find(object->structure()->id(), caches.nameOf(count.impl()), SlotAccess::Read);
        return slot && slot->kind == Slot::Kind::InheritedField && slot->has(Slot::dependsOnChain) && !slot->has(Slot::isOwn);
    };
    auto createObjectBelow = [&](Object* prototype) {
        putByName(program, prototype, count, jsNumber(1.5));
        JSObject* object = constructEmptyObject(realm, prototype);
        PropertyAnswer answer = readAndClassify(program, object, count);
        AOT_TEST_CHECK(answer.kind == PropertyAnswerKind::InheritedField && answer.holder == prototype);
        caches.cache({ object->structure(), caches.nameOf(count.impl()), SlotAccess::Read }, answer);
        AOT_TEST_CHECK(isFiledFor(object) && caches.tables().find(object->structure()->id(), caches.nameOf(count.impl()), SlotAccess::Read)->holder == prototype);
        return object;
    };

    // The record names the holder and nothing of the slot, so it holds after a store that nobody hears of.
    Object* prototype = allocateLabeledObject(program);
    JSObject* object = createObjectBelow(prototype);
    AOT_TEST_CHECK(storeInSlot(program, prototype, *prototype->fieldNamed(program.state(), count), jsNumber(2.5)) && isFiledFor(object));

    // The prototype reports what takes the property out of its slot, and the record goes.
    DeletePropertySlot deleteSlot;
    AOT_TEST_CHECK(prototype->methodTable()->deleteProperty(prototype, realm, count, deleteSlot));
    AOT_TEST_CHECK(!isFiledFor(object) && !prototype->isInSlot(*prototype->fieldNamed(program.state(), count)));

    Object* prototypeWithAccessor = allocateLabeledObject(program);
    JSObject* objectBelowAccessor = createObjectBelow(prototypeWithAccessor);
    defineAccessor(program, prototypeWithAccessor, count);
    AOT_TEST_CHECK(!isFiledFor(objectBelowAccessor) && readAndClassify(program, objectBelowAccessor, count).kind != PropertyAnswerKind::InheritedField);

    // A value of another type moves the property to the extra properties.
    Object* prototypeWithText = allocateLabeledObject(program);
    JSObject* objectBelowText = createObjectBelow(prototypeWithText);
    putByName(program, prototypeWithText, count, jsNontrivialString(vm, "text"_s));
    AOT_TEST_CHECK(!isFiledFor(objectBelowText) && readAndClassify(program, objectBelowText, count).kind == PropertyAnswerKind::Value);

    // An object between the two that gets the name hides the field.
    Object* farPrototype = allocateLabeledObject(program);
    putByName(program, farPrototype, count, jsNumber(1.5));
    JSObject* middle = constructEmptyObject(realm, farPrototype);
    JSObject* objectBelowBoth = constructEmptyObject(realm, middle);
    PropertyAnswer throughMiddle = readAndClassify(program, objectBelowBoth, count);
    AOT_TEST_CHECK(throughMiddle.kind == PropertyAnswerKind::InheritedField && throughMiddle.holder == farPrototype && throughMiddle.changeableObjects.contains(middle));
    caches.cache({ objectBelowBoth->structure(), caches.nameOf(count.impl()), SlotAccess::Read }, throughMiddle);
    AOT_TEST_CHECK(isFiledFor(objectBelowBoth));
    putByName(program, middle, count, jsNumber(4));
    AOT_TEST_CHECK(!isFiledFor(objectBelowBoth));
}

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

    // Nothing above: the object asks the lower prototype, which asks the upper one, whose empty slot reads as undefined.
    PropertyAnswer fromObject = file(object);
    AOT_TEST_CHECK(fromObject.kind == PropertyAnswerKind::InheritedField && fromObject.holder == lower);
    for (JSObject* onChain : { static_cast<JSObject*>(lower), static_cast<JSObject*>(upper), top })
        AOT_TEST_CHECK(fromObject.changeableObjects.contains(onChain));
    PropertyAnswer fromLower = file(lower);
    AOT_TEST_CHECK(fromLower.kind == PropertyAnswerKind::FieldOrInheritedField && fromLower.field == labelField && fromLower.holder == upper);
    PropertyAnswer fromUpper = file(upper);
    AOT_TEST_CHECK(fromUpper.kind == PropertyAnswerKind::Field && fromUpper.absentIsUndefined);

    // What compiled code does: nobody hears of it, and every record stays right.
    AOT_TEST_CHECK(storeInSlot(program, upper, *labelField, jsNumber(2)));
    AOT_TEST_CHECK(find(object) && find(lower) && find(upper));

    // A data property at the top changes what the upper prototype answers while its slot is empty, and nothing about whom the others ask.
    upper->clear(*labelField);
    top->putDirect(vm, label, jsNumber(5));
    AOT_TEST_CHECK(!find(object) && !find(lower) && !find(upper));
    AOT_TEST_CHECK(file(object).holder == lower && file(lower).holder == upper);
    PropertyAnswer aboveUpper = file(upper);
    AOT_TEST_CHECK(aboveUpper.kind == PropertyAnswerKind::FieldOrInheritedValue && aboveUpper.holder == top);

    // An accessor at the top needs the receiver.
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

    // A store below a writable data property creates the receiver's own, whatever the slot above holds.
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

static void testStoresBelowEmptySlotOfCompiledPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    PropertyCaches& caches = *program.state().propertyCaches;
    Identifier label = Identifier::fromString(vm, "label"_s);
    Object* prototype = allocateLabeledObject(program);
    const FieldData* labelField = prototype->fieldNamed(program.state(), label);
    prototype->clear(*labelField);
    Structure* structure = constructEmptyObject(realm, prototype)->structure();
    auto find = [&] {
        return caches.tables().find(structure->id(), caches.nameOf(label.impl()), SlotAccess::Store);
    };
    auto storeAndFile = [&] {
        PropertyAnswer answer = storeAndClassify(program, constructEmptyObject(realm, prototype), label, jsNumber(1));
        caches.cache({ structure, caches.nameOf(label.impl()), SlotAccess::Store }, answer);
        return answer;
    };

    AOT_TEST_CHECK(!prototype->hasExtraProperties());
    PropertyAnswer addition = storeAndFile();
    AOT_TEST_CHECK(addition.kind == PropertyAnswerKind::Addition && dependsOnlyOn(addition, prototype));
    AOT_TEST_CHECK(find() && find()->kind == Slot::Kind::Transition && find()->has(Slot::dependsOnChain));

    // What compiled code does: nobody hears of it, and the answer is the same.
    AOT_TEST_CHECK(storeInSlot(program, prototype, *labelField, jsNumber(2)));
    AOT_TEST_CHECK(find() && storeAndFile().kind == PropertyAnswerKind::Addition);

    // A frozen field refuses the store below it.
    prototype->setIntegrity(realm, Object::Integrity::Frozen);
    AOT_TEST_CHECK(!find() && !storeCreatesOwnProperty(realm, structure, nullptr, label.impl()));
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

    // Compiled code that fills the slot hides the setter, and nobody hears of it: the answer holds now and cannot be kept.
    AOT_TEST_CHECK(storeThroughChain(walk(ChainWalkUse::UsedAtOnce)) == StoreThroughChain::CallsSetter && walk(ChainWalkUse::UsedAtOnce).holder == top);
    AOT_TEST_CHECK(walk(ChainWalkUse::KeptForStores).result == ChainWalkResult::Unknown && walk(ChainWalkUse::KeptForReads).result == ChainWalkResult::Unknown);
    // The prototype would call the getter with itself as the receiver.
    AOT_TEST_CHECK(walk(ChainWalkUse::KeptForReadsThatAskObjects).result == ChainWalkResult::Unknown && isLastingRefusal(readAndClassify(program, object, label)));

    // An object with extra properties leaves the creation of a field to the run time, which reports it.
    putByName(program, prototype, other, jsNumber(1));
    AOT_TEST_CHECK(prototype->hasExtraProperties());
    AOT_TEST_CHECK(storeThroughChain(walk(ChainWalkUse::KeptForStores)) == StoreThroughChain::CallsSetter && walk(ChainWalkUse::KeptForReads).holder == top);
    AOT_TEST_CHECK(walk(ChainWalkUse::KeptForStores).changeableObjects.contains(prototype) && walk(ChainWalkUse::KeptForStores).changeableObjects.contains(top));
}

static void testReadsThroughEmptySlotOfCompiledPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    PropertyCaches& caches = *program.state().propertyCaches;
    Identifier label = Identifier::fromString(vm, "label"_s);
    Identifier other = Identifier::fromString(vm, "other"_s);
    Object* prototype = allocateLabeledObject(program);
    const FieldData* labelField = prototype->fieldNamed(program.state(), label);
    prototype->clear(*labelField);
    removeFieldFromKeyOrder(vm, program.state(), *prototype, *labelField);
    putByName(program, prototype, other, jsNumber(1));
    JSObject* object = constructEmptyObject(realm, prototype);
    Structure* structure = object->structure();
    auto find = [&] {
        return caches.tables().find(structure->id(), caches.nameOf(label.impl()), SlotAccess::Read);
    };

    PropertyAnswer absent = readAndClassify(program, object, label);
    AOT_TEST_CHECK(isAbsent(absent) && absent.changeableObjects.contains(prototype));
    caches.cache({ structure, caches.nameOf(label.impl()), SlotAccess::Read }, absent);
    AOT_TEST_CHECK(find() && find()->kind == Slot::Kind::Absent);

    // What compiled code does with such an object: it has the run time list the field, then fills the slot.
    callOperationFromTest(program.vm(), operationAOTDidCreateFieldAt, &program.instance(), prototype, slotKeyFor(labelField->slot.offset, labelField->isOutOfLine()));
    AOT_TEST_CHECK(!find());
    AOT_TEST_CHECK(storeInSlot(program, prototype, *labelField, jsNumber(3)));
    PropertyAnswer inherited = readAndClassify(program, object, label);
    AOT_TEST_CHECK(inherited.kind == PropertyAnswerKind::Value && inherited.holder == prototype && readThroughAnswer(realm, object, inherited) == jsNumber(3));
}

static void testExtraPropertiesOfCompiledPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    PropertyCaches& caches = *program.state().propertyCaches;
    Identifier other = Identifier::fromString(vm, "other"_s);
    Identifier absentName = Identifier::fromString(vm, "absentName"_s);
    Object* prototype = allocateLabeledObject(program);
    putByName(program, prototype, other, jsNumber(1));
    JSObject* extra = prototype->extraProperties();
    JSObject* object = constructEmptyObject(realm, prototype);
    Structure* structure = object->structure();
    auto find = [&](const Identifier& name, SlotAccess access) {
        return caches.tables().find(structure->id(), caches.nameOf(name.impl()), access);
    };
    auto fileRead = [&](const Identifier& name) {
        PropertyAnswer answer = readAndClassify(program, object, name);
        caches.cache({ structure, caches.nameOf(name.impl()), SlotAccess::Read }, answer);
        return answer;
    };

    PropertyAnswer inherited = fileRead(other);
    AOT_TEST_CHECK(extra && isInheritedFrom(program, inherited, PropertyAnswerKind::Value, extra, other) && dependsOnlyOn(inherited, prototype));
    putByName(program, prototype, other, jsNumber(2));
    AOT_TEST_CHECK(find(other, SlotAccess::Read) && readThroughAnswer(realm, object, inherited) == jsNumber(2));
    AOT_TEST_CHECK(storeCreatesOwnProperty(realm, structure, object, other.impl()));

    AOT_TEST_CHECK(isAbsent(fileRead(absentName)) && find(absentName, SlotAccess::Read));
    putByName(program, prototype, absentName, jsNumber(3));
    AOT_TEST_CHECK(!find(absentName, SlotAccess::Read) && find(other, SlotAccess::Read));
    AOT_TEST_CHECK(isInheritedFrom(program, readAndClassify(program, object, absentName), PropertyAnswerKind::Value, extra, absentName));

    defineAccessor(program, prototype, other);
    AOT_TEST_CHECK(!find(other, SlotAccess::Read));
    AOT_TEST_CHECK(isInheritedFrom(program, readAndClassify(program, object, other), PropertyAnswerKind::Accessor, extra, other));
    ChainWalk store = walkChain({ realm, structure, object, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores }, other.impl());
    AOT_TEST_CHECK(storeThroughChain(store) == StoreThroughChain::CallsSetter && store.holder == extra && store.owner == prototype);

    DeletePropertySlot deleteSlot;
    fileRead(other);
    AOT_TEST_CHECK(find(other, SlotAccess::Read) && prototype->methodTable()->deleteProperty(prototype, realm, other, deleteSlot));
    AOT_TEST_CHECK(!find(other, SlotAccess::Read) && isAbsent(readAndClassify(program, object, other)));
}

static void testEmptySlotOfReceiverDefersToChain(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    PropertyCaches& caches = *program.state().propertyCaches;
    Identifier label = Identifier::fromString(vm, "label"_s);
    JSObject* prototype = constructEmptyObject(realm);
    Object* object = allocateLabeledObject(program);
    const FieldData* labelField = object->fieldNamed(program.state(), label);
    object->clear(*labelField);
    object->setPrototypeKeepingShape(vm, prototype);
    Structure* structure = object->structure();
    auto file = [&](SlotAccess access, const PropertyAnswer& answer) {
        caches.cache({ structure, caches.nameOf(label.impl()), access }, answer);
        return caches.tables().find(structure->id(), caches.nameOf(label.impl()), access);
    };
    auto namesProperty = [&](const std::optional<Slot>& slot, Slot::Kind kind) {
        return slot && slot->kind == kind && slot->has(Slot::dependsOnChain) && slot->fieldSlot() == labelField->slot
            && MarkedBlockCage::decode(vm.heap.cage().base(), slot->fieldAndHolder.holder) == prototype && slot->offsetInStorage() == prototype->getDirectOffset(vm, label);
    };

    // Nothing above: an empty slot reads as undefined, and a store fills it.
    PropertyAnswer absent = readAndClassify(program, object, label);
    AOT_TEST_CHECK(absent.kind == PropertyAnswerKind::Field && absent.absentIsUndefined && absent.changeableObjects.contains(prototype));

    prototype->putDirect(vm, label, jsNumber(5));
    PropertyAnswer value = readAndClassify(program, object, label);
    AOT_TEST_CHECK(value.kind == PropertyAnswerKind::FieldOrInheritedValue && value.field == labelField && value.holder == prototype && !value.absentIsUndefined);
    std::optional<Slot> valueSlot = file(SlotAccess::Read, value);
    AOT_TEST_CHECK(namesProperty(valueSlot, Slot::Kind::FieldOrInherited) && !valueSlot->has(Slot::inheritsGetter));

    // The prototype reports what happens to the property, and the record goes.
    DeletePropertySlot deleteSlot;
    AOT_TEST_CHECK(JSObject::deleteProperty(prototype, realm, label, deleteSlot));
    AOT_TEST_CHECK(!caches.tables().find(structure->id(), caches.nameOf(label.impl()), SlotAccess::Read));

    defineAccessor(program, prototype, label);
    PropertyAnswer getter = readAndClassify(program, object, label);
    AOT_TEST_CHECK(getter.kind == PropertyAnswerKind::FieldOrInheritedAccessor && getter.holder == prototype);
    std::optional<Slot> getterSlot = file(SlotAccess::Read, getter);
    AOT_TEST_CHECK(namesProperty(getterSlot, Slot::Kind::FieldOrInherited) && getterSlot->has(Slot::inheritsGetter));

    ChainWalk walk = walkChain({ realm, structure, object, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores }, label.impl());
    PutPropertySlot putSlot(object, true, PutPropertySlot::PutById);
    putSlot.setCacheableSetter(walk.holder, walk.offset);
    PropertyAnswer setter = classifyStore({ realm, object, structure, label.impl(), putSlot, PutMode::Strict });
    AOT_TEST_CHECK(setter.kind == PropertyAnswerKind::FieldOrInheritedAccessor && setter.access == SlotAccess::Store && setter.holder == prototype);
    AOT_TEST_CHECK(namesProperty(file(SlotAccess::Store, setter), Slot::Kind::FieldOrInheritedSetter));

    // A definition asks nobody above.
    PropertyAnswer definition = classifyStore({ realm, object, structure, label.impl(), putSlot, PutMode::Define });
    AOT_TEST_CHECK(definition.kind == PropertyAnswerKind::FieldCreation && !definition.holder);
}

static void testStoreIsFiledOneMissEarlierThanRead(LoadedProgram& program)
{
    PropertyCaches& caches = *program.state().propertyCaches;
    Identifier label = Identifier::fromString(program.vm(), "label"_s);
    StructureID structure = program.realm()->nullPrototypeObjectStructure()->id();
    auto missesUntilFiled = [&](SlotAccess access) {
        unsigned misses = 1;
        while (!caches.shouldFile(structure, label.impl(), access))
            ++misses;
        return misses;
    };
    AOT_TEST_CHECK(missesUntilFiled(SlotAccess::Read) == 3);
    AOT_TEST_CHECK(missesUntilFiled(SlotAccess::Store) == 2);
    AOT_TEST_CHECK(missesUntilFiled(SlotAccess::Define) == 2);
}

static void testPairsThatMissInTurnAreAllFiled(LoadedProgram& program)
{
    PropertyCaches& caches = *program.state().propertyCaches;
    StructureID structure = program.realm()->nullPrototypeObjectStructure()->id();
    constexpr unsigned numberOfPairs = 1024;
    constexpr unsigned maxTurns = 32;
    BitVector isFiled(numberOfPairs);
    for (unsigned turn = 0; turn < maxTurns; ++turn) {
        for (unsigned pair = 0; pair < numberOfPairs; ++pair) {
            auto* uid = std::bit_cast<UniquedStringImpl*>(static_cast<uintptr_t>(0x10000 + 32 * pair));
            if (!isFiled.get(pair) && caches.shouldFile(structure, uid, SlotAccess::Read))
                isFiled.set(pair);
        }
    }
    AOT_TEST_CHECK(isFiled.bitCount() == numberOfPairs);
}

static void testAbsenceFilterNamesWhatStopsStores(LoadedProgram& program)
{
    VM& vm = program.vm();
    JSGlobalObject* realm = program.realm();
    PropertyCaches& caches = *program.state().propertyCaches;
    VMPropertyCaches& tables = caches.tables();
    Identifier label = Identifier::fromString(vm, "label"_s);
    std::optional<uint32_t> number = caches.nameOf(label.impl()).identifierNumber;
    AOT_TEST_CHECK(number);
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
    AOT_TEST_CHECK(ofBelow && ofBelow->letsStoresPass(*number));

    // A data property that can be written stops no store, and the filters for stores outlive its arrival.
    above->putDirect(vm, label, jsNumber(1));
    AOT_TEST_CHECK(validFilterOf(below) == ofBelow && ofBelow->letsStoresPass(*number) && !tables.wasChangedOnSomeChain(*number));

    // A filter lasts as long as its Structure: nothing files it anew for a store that the tables answer.
    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        vm.heap.collectSync(scope);
        ensureStillAliveHere(below);
        AOT_TEST_CHECK(validFilterOf(below) == ofBelow && ofBelow->letsStoresPass(*number));
    }

    // What the lowest object has itself is nothing to a store that has found that the object lacks the name.
    JSObject* owner = constructEmptyObject(realm);
    owner->putDirect(vm, label, jsNumber(1), static_cast<unsigned>(PropertyAttribute::ReadOnly));
    const AbsenceFilter* ofOwner = filterOf(owner);
    AOT_TEST_CHECK(ofOwner && ofOwner->letsStoresPass(*number));

    // The filters stay, and the name is marked for all of them.
    above->putDirect(vm, label, jsNumber(1), static_cast<unsigned>(PropertyAttribute::ReadOnly));
    AOT_TEST_CHECK(validFilterOf(below) == ofBelow && tables.wasChangedOnSomeChain(*number));
    JSObject* twoBelow = makeBelow(below);
    const AbsenceFilter* ofTwoBelow = filterOf(twoBelow);
    AOT_TEST_CHECK(ofTwoBelow && !ofTwoBelow->letsStoresPass(*number));

    JSObject* withAccessor = constructEmptyObject(realm);
    withAccessor->putDirectAccessor(realm, label, GetterSetter::create(vm, realm, nullptr, nullptr), static_cast<unsigned>(PropertyAttribute::Accessor));
    const AbsenceFilter* ofBelowAccessor = filterOf(makeBelow(withAccessor));
    AOT_TEST_CHECK(ofBelowAccessor && !ofBelowAccessor->letsStoresPass(*number));

    // A change that names nothing ends every filter for stores.
    above->setPrototypeDirect(vm, constructEmptyObject(realm));
    AOT_TEST_CHECK(!validFilterOf(below) && !validFilterOf(owner));

    // No list of names speaks for an object that has a say of its own in a store that comes past it.
    AOT_TEST_CHECK(!filterOf(makeBelow(ProxyObject::create(realm, constructEmptyObject(realm), constructEmptyObject(realm)))));
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<propertyLookupTests, programText, body>;

template<LoadedProgramTest body>
static constexpr void (*withProgramAddingProperties)() = runNativeTestWithLoadedProgram<propertyLookupTests, programAddingPropertiesText, body>;

std::span<const NativeTest> propertyLookupTests()
{
    static constexpr NativeTest tests[] = {
        { "property-lookup/own-data-property"_s, withProgram<testOwnDataProperty> },
        { "property-lookup/absent-name"_s, withProgram<testAbsentName> },
        { "property-lookup/own-accessor"_s, withProgram<testOwnAccessor> },
        { "property-lookup/name-of-object-prototype"_s, withProgram<testNameOfObjectPrototype> },
        { "property-lookup/underscore-proto-and-index"_s, withProgram<testUnderscoreProtoAndIndex> },
        { "property-lookup/dictionary"_s, withProgram<testDictionary> },
        { "property-lookup/other-prototype"_s, withProgram<testOtherPrototype> },
        { "property-lookup/other-base"_s, withProgram<testOtherBase> },
        { "property-lookup/length-of-typed-arrays"_s, withProgram<testLengthOfTypedArrays> },
        { "property-lookup/only-keys-with-atoms-are-names"_s, withProgram<testOnlyKeysWithAtomsAreNames> },
        { "property-lookup/indexes-are-not-names"_s, withProgram<testIndexesAreNotNames> },
        { "property-lookup/presence-is-cached-under-name"_s, withProgram<testPresenceIsCachedUnderName> },
        { "property-lookup/store-is-filed-one-miss-earlier-than-read"_s, withProgram<testStoreIsFiledOneMissEarlierThanRead> },
        { "property-lookup/pairs-that-miss-in-turn-are-all-filed"_s, withProgram<testPairsThatMissInTurnAreAllFiled> },
        { "property-lookup/absence-filter-names-what-stops-stores"_s, withProgram<testAbsenceFilterNamesWhatStopsStores> },
        { "property-lookup/slot-filed-by-number-is-copied-for-keys"_s, withProgram<testSlotFiledByNumberIsCopiedForKeys> },
        { "property-lookup/leaf-operations-answer-by-name"_s, withProgram<testLeafOperationsAnswerByName> },
        { "property-lookup/leaf-operations-answer-by-index"_s, withProgram<testLeafOperationsAnswerByIndex> },
        { "property-lookup/named-change-drops-only-that-name"_s, withProgram<testNamedChangeDropsOnlyThatName> },
        { "property-lookup/evicted-dependents-are-dropped-when-the-table-has-doubled"_s, withProgram<testEvictedDependentsAreDroppedWhenTheTableHasDoubled> },
        { "property-lookup/named-changes-of-an-object-with-many-dependents"_s, withProgram<testNamedChangesOfAnObjectWithManyDependents> },
        { "property-lookup/nameless-change-drops-every-name"_s, withProgram<testNamelessChangeDropsEveryName> },
        { "property-lookup/fields-of-compiled-prototype"_s, withProgram<testFieldsOfCompiledPrototype> },
        { "property-lookup/unboxed-fields-of-compiled-prototype"_s, withProgramAddingProperties<testUnboxedFieldsOfCompiledPrototype> },
        { "property-lookup/reads-below-empty-slots-ask-the-first-object-that-has-one"_s, withProgram<testReadsBelowEmptySlotsAskTheFirstObjectThatHasOne> },
        { "property-lookup/empty-slot-of-receiver-defers-to-unboxed-field-of-compiled-prototype"_s, withProgramAddingProperties<testEmptySlotOfReceiverDefersToUnboxedFieldOfCompiledPrototype> },
        { "property-lookup/empty-slot-of-receiver-defers-to-chain"_s, withProgramAddingProperties<testEmptySlotOfReceiverDefersToChain> },
        { "property-lookup/stores-below-empty-slot-of-compiled-prototype"_s, withProgram<testStoresBelowEmptySlotOfCompiledPrototype> },
        { "property-lookup/setter-above-empty-slot-of-compiled-prototype"_s, withProgramAddingProperties<testSetterAboveEmptySlotOfCompiledPrototype> },
        { "property-lookup/reads-through-empty-slot-of-compiled-prototype"_s, withProgramAddingProperties<testReadsThroughEmptySlotOfCompiledPrototype> },
        { "property-lookup/extra-properties-of-compiled-prototype"_s, withProgramAddingProperties<testExtraPropertiesOfCompiledPrototype> },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
