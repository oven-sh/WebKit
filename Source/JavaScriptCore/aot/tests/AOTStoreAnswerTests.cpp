/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTPropertyCachesAnswers.h"
#include "AOTRealmTestSupport.h"
#include "AOTRuntimeCounters.h"
#include "AOTVMState.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "ObjectConstructor.h"
#include "PutPropertySlot.h"
#include "VMInlines.h"
#include <optional>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

class Realm {
    WTF_MAKE_NONCOPYABLE(Realm);
public:
    Realm()
        : m_vm(VM::create())
    {
        m_lock.emplace(*m_vm);
        m_globalObject.set(*m_vm, JSGlobalObject::create(*m_vm, JSGlobalObject::createStructure(*m_vm, jsNull())));
    }

    ~Realm()
    {
        m_globalObject.clear();
        m_vm = nullptr;
        m_lock.reset();
    }

    VM& vm() { return *m_vm; }
    JSGlobalObject* globalObject() { return m_globalObject.get(); }
    JSObject* createObjectWithoutPrototype() { return constructEmptyObject(vm(), globalObject()->nullPrototypeObjectStructure()); }

private:
    RefPtr<VM> m_vm;
    std::optional<JSLockHolder> m_lock;
    Strong<JSGlobalObject> m_globalObject;
};

class ObjectWithOwnPut final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesPut;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        return &vm.cellSpace();
    }

    DECLARE_INFO;

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    }

    static ObjectWithOwnPut* create(VM& vm, JSGlobalObject* globalObject)
    {
        auto* object = new (NotNull, allocateCell<ObjectWithOwnPut>(vm)) ObjectWithOwnPut(vm, createStructure(vm, globalObject, globalObject->objectPrototype()));
        object->finishCreation(vm);
        return object;
    }

    static bool put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue, PutPropertySlot& slot)
    {
        return Base::put(cell, globalObject, name, jsNull(), slot);
    }

private:
    ObjectWithOwnPut(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }
};

const ClassInfo ObjectWithOwnPut::s_info = { "ObjectWithOwnPut"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ObjectWithOwnPut) };

} // namespace

static PropertyAnswer defineAndClassify(JSGlobalObject* globalObject, JSObject* object, const Identifier& name, JSValue value)
{
    Structure* structureBefore = object->structure();
    PutPropertySlot slot(object, true);
    object->putDirect(globalObject->vm(), name, value, slot);
    PropertyAnswer answer = classifyStore({ globalObject, object, structureBefore, name.impl(), slot, PutMode::Define });
    AOT_TEST_CHECK(answer.access == SlotAccess::Define);
    return answer;
}

static PropertyAnswer storeAndClassify(JSGlobalObject* globalObject, JSObject* object, const Identifier& name, JSValue value)
{
    Structure* structureBefore = object->structure();
    PutPropertySlot slot(object, true, PutPropertySlot::PutById);
    AOT_TEST_CHECK(object->methodTable()->put(object, globalObject, name, value, slot));
    PropertyAnswer answer = classifyStore({ globalObject, object, structureBefore, name.impl(), slot, PutMode::Strict });
    AOT_TEST_CHECK(answer.access == SlotAccess::Store);
    return answer;
}

static PropertyAnswer classifyRefusedSloppyStore(JSGlobalObject* globalObject, JSObject* object, const Identifier& name, JSValue value)
{
    Structure* structureBefore = object->structure();
    PutPropertySlot slot(object, false, PutPropertySlot::PutById);
    AOT_TEST_CHECK(!object->methodTable()->put(object, globalObject, name, value, slot));
    AOT_TEST_CHECK(!globalObject->vm().exceptionForInspection() && object->structure() == structureBefore);
    PropertyAnswer answer = classifyStore({ globalObject, object, structureBefore, name.impl(), slot, PutMode::Sloppy });
    AOT_TEST_CHECK(answer.access == SlotAccess::Store);
    return answer;
}

static bool isOwnValueAt(const PropertyAnswer& answer, PropertyOffset offset)
{
    return answer.kind == PropertyAnswerKind::Value && answer.isOwn && !answer.holder && answer.offset == offset && !answer.dependsOnChain();
}

static bool isLastingRefusal(const PropertyAnswer& answer)
{
    return answer.kind == PropertyAnswerKind::Uncacheable && !answer.didFlatten;
}

static bool dependsOnlyOn(const PropertyAnswer& answer, JSObject* object)
{
    return answer.changeableObjects.size() == 1 && answer.changeableObjects[0] == object;
}

static void testRedefinitionOfOrdinaryObjectReplaces()
{
    Realm realm;
    VM& vm = realm.vm();
    Identifier name = Identifier::fromString(vm, "x"_s);
    JSObject* object = constructEmptyObject(realm.globalObject());
    Structure* emptyStructure = object->structure();

    PropertyAnswer definition = defineAndClassify(realm.globalObject(), object, name, jsNumber(1));
    AOT_TEST_CHECK(definition.kind == PropertyAnswerKind::Addition && definition.isOwn && !definition.dependsOnChain());
    AOT_TEST_CHECK(definition.newStructure == object->structure() && definition.newStructure != emptyStructure);
    AOT_TEST_CHECK(definition.offset == object->structure()->get(vm, name) && !definition.newStorageCapacity);
    AOT_TEST_CHECK(isOwnValueAt(defineAndClassify(realm.globalObject(), object, name, jsNumber(2)), object->structure()->get(vm, name)));
    AOT_TEST_CHECK(isOwnValueAt(storeAndClassify(realm.globalObject(), object, name, jsNumber(3)), object->structure()->get(vm, name)));
    AOT_TEST_CHECK(object->getDirect(vm, name) == jsNumber(3));
}

static void testRedefinitionOfObjectWithOwnPutServesDefinitionsOnly()
{
    Realm realm;
    VM& vm = realm.vm();
    Identifier name = Identifier::fromString(vm, "x"_s);
    JSObject* object = ObjectWithOwnPut::create(vm, realm.globalObject());
    AOT_TEST_CHECK(isHostObjectOverridingPut(object->structure()));

    AOT_TEST_CHECK(hasOrdinaryDefineForName(vm, object, object->structure(), name.impl()));
    AOT_TEST_CHECK(defineAndClassify(realm.globalObject(), object, name, jsNumber(1)).kind == PropertyAnswerKind::Addition);
    AOT_TEST_CHECK(isOwnValueAt(defineAndClassify(realm.globalObject(), object, name, jsNumber(2)), object->structure()->get(vm, name)));
    AOT_TEST_CHECK(object->getDirect(vm, name) == jsNumber(2));
    AOT_TEST_CHECK(isLastingRefusal(storeAndClassify(realm.globalObject(), object, name, jsNumber(3))));
    AOT_TEST_CHECK(object->getDirect(vm, name).isNull());
}

static void testStoreToObjectWithOwnPutIsNotCached()
{
    Realm realm;
    VM& vm = realm.vm();
    Identifier name = Identifier::fromString(vm, "x"_s);
    JSObject* object = ObjectWithOwnPut::create(vm, realm.globalObject());

    AOT_TEST_CHECK(isLastingRefusal(storeAndClassify(realm.globalObject(), object, name, jsNumber(1))));
    AOT_TEST_CHECK(isLastingRefusal(storeAndClassify(realm.globalObject(), object, name, jsNumber(2))));
    AOT_TEST_CHECK(object->getDirect(vm, name).isNull());
}

static void testShadowingNameOnPristineArrayIsNotCached()
{
    struct AddedName {
        ASCIILiteral text;
        bool shadowsArrayPrototype;
    };
    static constexpr AddedName addedNames[] = {
        { "other"_s, false },
        { "push"_s, true },
    };

    Realms realms;
    VM& vm = realms.vm();
    Identifier tag = Identifier::fromString(vm, "tag"_s);
    for (JSGlobalObject* realm : { realms.locked(), realms.plain() }) {
        bool hasPristineArrays = realm == realms.locked();
        for (const AddedName& addedName : addedNames) {
            JSArray* array = realms.array(realm, "[1, 2]"_s);
            AOT_TEST_CHECK(array->isPristine() == hasPristineArrays);
            AOT_TEST_CHECK(isLastingRefusal(defineAndClassify(realm, array, tag, jsNumber(1))));

            PropertyAnswer answer = defineAndClassify(realm, array, Identifier::fromString(vm, addedName.text), jsNumber(2));
            if (hasPristineArrays && addedName.shadowsArrayPrototype)
                AOT_TEST_CHECK(isLastingRefusal(answer));
            else
                AOT_TEST_CHECK(answer.kind == PropertyAnswerKind::Addition && !answer.newStorageCapacity);
            AOT_TEST_CHECK(array->isPristine() == (hasPristineArrays && !addedName.shadowsArrayPrototype));
        }
    }
}

static void testHiddenPropertyIsStoredButNotRedefined()
{
    Realm realm;
    VM& vm = realm.vm();
    Identifier name = Identifier::fromString(vm, "x"_s);
    JSObject* object = constructEmptyObject(realm.globalObject());
    object->putDirect(vm, name, jsNumber(1), static_cast<unsigned>(PropertyAttribute::DontEnum));

    AOT_TEST_CHECK(isOwnValueAt(storeAndClassify(realm.globalObject(), object, name, jsNumber(2)), object->structure()->get(vm, name)));
    AOT_TEST_CHECK(defineAndClassify(realm.globalObject(), object, name, jsNumber(3)).kind == PropertyAnswerKind::Uncacheable);
    AOT_TEST_CHECK(object->getDirect(vm, name) == jsNumber(3));
}

static void testAdditionDependsOnObjectsAbove()
{
    Realms realms;
    VM& vm = realms.vm();
    JSGlobalObject* realm = realms.plain();
    Identifier name = Identifier::fromString(vm, "x"_s);
    JSObject* prototype = realms.object(realm, "Object.create(null, { other: { set(value) { }, configurable: true } })"_s);
    JSObject* object = constructEmptyObject(realm, prototype);
    Structure* emptyStructure = object->structure();

    PropertyAnswer store = storeAndClassify(realm, object, name, jsNumber(1));
    AOT_TEST_CHECK(store.kind == PropertyAnswerKind::Addition && store.isOwn && dependsOnlyOn(store, prototype));
    AOT_TEST_CHECK(store.newStructure == object->structure() && store.newStructure->previousID() == emptyStructure);
    AOT_TEST_CHECK(store.offset == object->structure()->get(vm, name));

    PropertyAnswer definition = defineAndClassify(realm, constructEmptyObject(realm, prototype), name, jsNumber(1));
    AOT_TEST_CHECK(definition.kind == PropertyAnswerKind::Addition && !definition.dependsOnChain());

    AOT_TEST_CHECK(prototype->makePropertiesImmutable(vm));
    PropertyAnswer storeBelowImmutablePrototype = storeAndClassify(realm, constructEmptyObject(realm, prototype), name, jsNumber(1));
    AOT_TEST_CHECK(storeBelowImmutablePrototype.kind == PropertyAnswerKind::Addition && !storeBelowImmutablePrototype.dependsOnChain());
}

static void testSetterAboveIsCalled()
{
    Realms realms;
    VM& vm = realms.vm();
    JSGlobalObject* realm = realms.plain();
    Identifier name = Identifier::fromString(vm, "x"_s);
    JSObject* prototype = realms.object(realm, "Object.create(null, { x: { set(value) { }, configurable: true } })"_s);
    JSObject* object = constructEmptyObject(realm, prototype);
    Structure* emptyStructure = object->structure();

    PropertyAnswer answer = storeAndClassify(realm, object, name, jsNumber(1));

    AOT_TEST_CHECK(answer.kind == PropertyAnswerKind::Accessor && !answer.isOwn && answer.holder == prototype);
    AOT_TEST_CHECK(answer.offset == prototype->structure()->get(vm, name) && dependsOnlyOn(answer, prototype));
    AOT_TEST_CHECK(object->structure() == emptyStructure);

    PropertyAnswer ownSetter = storeAndClassify(realm, prototype, name, jsNumber(1));
    AOT_TEST_CHECK(ownSetter.kind == PropertyAnswerKind::Accessor && ownSetter.isOwn && !ownSetter.holder && !ownSetter.dependsOnChain());
    AOT_TEST_CHECK(ownSetter.offset == prototype->structure()->get(vm, name));
}

static void testReadOnlyPropertyAboveRefusesUntilItChanges()
{
    Realms realms;
    VM& vm = realms.vm();
    JSGlobalObject* realm = realms.plain();
    Identifier name = Identifier::fromString(vm, "x"_s);
    JSObject* prototype = realms.object(realm, "Object.create(null, { x: { value: 1, writable: false, configurable: true } })"_s);
    JSObject* object = constructEmptyObject(realm, prototype);

    PropertyAnswer answer = classifyRefusedSloppyStore(realm, object, name, jsNumber(2));

    AOT_TEST_CHECK(isLastingRefusal(answer) && dependsOnlyOn(answer, prototype));
    AOT_TEST_CHECK(defineAndClassify(realm, object, name, jsNumber(2)).kind == PropertyAnswerKind::Addition);
}

static void testAdditionToPrototypeIsNotCached()
{
    Realm realm;
    VM& vm = realm.vm();
    JSObject* object = realm.createObjectWithoutPrototype();
    object->didBecomePrototype(vm);
    AOT_TEST_CHECK(object->structure()->mayBePrototype());

    AOT_TEST_CHECK(isLastingRefusal(storeAndClassify(realm.globalObject(), object, Identifier::fromString(vm, "x"_s), jsNumber(1))));
    AOT_TEST_CHECK(isLastingRefusal(defineAndClassify(realm.globalObject(), object, Identifier::fromString(vm, "y"_s), jsNumber(1))));
    AOT_TEST_CHECK(isOwnValueAt(storeAndClassify(realm.globalObject(), object, Identifier::fromString(vm, "x"_s), jsNumber(2)), 0));
}

static void testAdditionNamesTheCapacityItGrowsTo()
{
    Realm realm;
    VM& vm = realm.vm();
    vm.aotState().counters = makeUniqueWithoutFastMallocCheck<RuntimeCounters>();
    uint64_t maxCapacityGrown = runtimeLimitValue(vm, LimitID::MaxCapacityGrownByStub);
    JSObject* object = realm.createObjectWithoutPrototype();
    bool didGrowFromFourToEight = false;
    unsigned index = 0;
    for (; !limitHitsOf(vm, LimitID::MaxCapacityGrownByStub); ++index) {
        AOT_TEST_CHECK(index < JSFinalObject::defaultInlineCapacity + 4 * maxCapacityGrown);
        unsigned capacityBefore = object->structure()->outOfLineCapacity();
        PropertyAnswer answer = storeAndClassify(realm.globalObject(), object, Identifier::fromString(vm, makeString("field"_s, index)), jsNumber(index));
        unsigned capacityAfter = object->structure()->outOfLineCapacity();
        if (capacityAfter != capacityBefore && capacityBefore > maxCapacityGrown) {
            AOT_TEST_CHECK(isLastingRefusal(answer) && limitHitsOf(vm, LimitID::MaxCapacityGrownByStub) == 1);
            continue;
        }
        AOT_TEST_CHECK(answer.kind == PropertyAnswerKind::Addition && !answer.dependsOnChain() && !answer.storageHasIndexingHeader);
        AOT_TEST_CHECK(answer.newStorageCapacity == (capacityAfter == capacityBefore ? 0 : capacityAfter));
        didGrowFromFourToEight |= capacityBefore == 4 && answer.newStorageCapacity == 8;
    }
    AOT_TEST_CHECK(didGrowFromFourToEight && object->structure()->outOfLineCapacity() == 4 * maxCapacityGrown);
}

static void testAdditionBesideElementsNamesTheIndexingHeader()
{
    Realms realms;
    VM& vm = realms.vm();
    JSGlobalObject* realm = realms.plain();
    for (ASCIILiteral source : { "[1, 2].map(x => x)"_s, "[1.5, 2.5].map(x => x)"_s, "['a', 'b'].map(x => x)"_s, "Object.assign({ }, [1, 2])"_s }) {
        JSObject* object = realms.object(realm, source);
        JSValue firstElement = object->getIndexQuickly(0);
        AOT_TEST_CHECK(object->structure()->couldHaveIndexingHeader() && !isCopyOnWrite(object->indexingMode()));
        unsigned numberOfGrowths = 0;
        for (unsigned index = 0; index < JSFinalObject::defaultInlineCapacity + 6; ++index) {
            Structure* before = object->structure();
            PropertyAnswer answer = storeAndClassify(realm, object, Identifier::fromString(vm, makeString("field"_s, index)), jsNumber(index));
            unsigned capacityAfter = object->structure()->outOfLineCapacity();
            bool grows = capacityAfter != before->outOfLineCapacity();
            AOT_TEST_CHECK(answer.kind == PropertyAnswerKind::Addition && answer.newStructure == object->structure());
            AOT_TEST_CHECK(answer.storageHasIndexingHeader == grows && answer.newStorageCapacity == (grows ? capacityAfter : 0));
            Slot slot = slotFor(answer, before);
            AOT_TEST_CHECK(slot.kind == Slot::Kind::Transition && slot.has(Slot::growsStorage) == grows && slot.has(Slot::storageHasIndexingHeader) == grows);
            numberOfGrowths += grows;
        }
        AOT_TEST_CHECK(numberOfGrowths >= 2 && object->getIndexQuickly(0) == firstElement);
    }

    // A store copies shared elements first, so the array leaves the Structure it had by two steps. A definition takes one step, which changes the indexing mode.
    Identifier field = Identifier::fromString(vm, "field"_s);
    JSArray* storedTo = realms.array(realm, "[1, 2]"_s);
    JSArray* definedOn = realms.array(realm, "[1, 2]"_s);
    AOT_TEST_CHECK(isCopyOnWrite(storedTo->indexingMode()) && isCopyOnWrite(definedOn->indexingMode()));
    AOT_TEST_CHECK(!storeAndClassify(realm, storedTo, field, jsNumber(1)).isCacheable());
    AOT_TEST_CHECK(isLastingRefusal(defineAndClassify(realm, definedOn, field, jsNumber(1))));
}

static void testDictionaryIsFlattenedOnce()
{
    Realm realm;
    VM& vm = realm.vm();
    Identifier name = Identifier::fromString(vm, "x"_s);
    Identifier added = Identifier::fromString(vm, "y"_s);
    Identifier addedLater = Identifier::fromString(vm, "z"_s);
    JSObject* object = realm.createObjectWithoutPrototype();
    object->putDirect(vm, name, jsNumber(1));
    object->convertToDictionary(vm);
    AOT_TEST_CHECK(object->structure()->isDictionary());

    // A cacheable dictionary says where a property is. A property it gets leaves its Structure as it is.
    AOT_TEST_CHECK(isOwnValueAt(storeAndClassify(realm.globalObject(), object, name, jsNumber(2)), object->structure()->get(vm, name)));
    AOT_TEST_CHECK(object->structure()->isDictionary());
    PropertyAnswer first = storeAndClassify(realm.globalObject(), object, added, jsNumber(2));
    AOT_TEST_CHECK(first.kind == PropertyAnswerKind::Uncacheable && first.didFlatten && !object->structure()->isDictionary());
    AOT_TEST_CHECK(isOwnValueAt(storeAndClassify(realm.globalObject(), object, name, jsNumber(3)), object->structure()->get(vm, name)));

    object->convertToDictionary(vm);
    AOT_TEST_CHECK(isLastingRefusal(storeAndClassify(realm.globalObject(), object, addedLater, jsNumber(4))));
    AOT_TEST_CHECK(object->structure()->isDictionary() && object->getDirect(vm, addedLater) == jsNumber(4));

    object->convertToUncacheableDictionary(vm);
    AOT_TEST_CHECK(isLastingRefusal(storeAndClassify(realm.globalObject(), object, name, jsNumber(5))));
    AOT_TEST_CHECK(object->structure()->isUncacheableDictionary() && object->getDirect(vm, name) == jsNumber(5));
}

std::span<const NativeTest> storeAnswerTests()
{
    static constexpr NativeTest tests[] = {
        { "store-answer/redefinition-of-ordinary-object-replaces"_s, testRedefinitionOfOrdinaryObjectReplaces },
        { "store-answer/redefinition-of-object-with-own-put-serves-definitions-only"_s, testRedefinitionOfObjectWithOwnPutServesDefinitionsOnly },
        { "store-answer/store-to-object-with-own-put-is-not-cached"_s, testStoreToObjectWithOwnPutIsNotCached },
        { "store-answer/shadowing-name-on-pristine-array-is-not-cached"_s, testShadowingNameOnPristineArrayIsNotCached },
        { "store-answer/hidden-property-is-stored-but-not-redefined"_s, testHiddenPropertyIsStoredButNotRedefined },
        { "store-answer/addition-depends-on-objects-above"_s, testAdditionDependsOnObjectsAbove },
        { "store-answer/setter-above-is-called"_s, testSetterAboveIsCalled },
        { "store-answer/read-only-property-above-refuses-until-it-changes"_s, testReadOnlyPropertyAboveRefusesUntilItChanges },
        { "store-answer/addition-to-prototype-is-not-cached"_s, testAdditionToPrototypeIsNotCached },
        { "store-answer/addition-names-the-capacity-it-grows-to"_s, testAdditionNamesTheCapacityItGrowsTo },
        { "store-answer/addition-beside-elements-names-the-indexing-header"_s, testAdditionBesideElementsNamesTheIndexingHeader },
        { "store-answer/dictionary-is-flattened-once"_s, testDictionaryIsFlattenedOnce },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
