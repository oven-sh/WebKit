/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTPropertyCachesAnswers.h"
#include "AOTRealmTestSupport.h"
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
        Structure* structure = createStructure(vm, globalObject, globalObject->objectPrototype());
        auto* object = new (NotNull, allocateCell<ObjectWithOwnPut>(vm)) ObjectWithOwnPut(vm, structure);
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

static bool isOwnValueAt(const PropertyAnswer& answer, PropertyOffset offset)
{
    return answer.kind == PropertyAnswerKind::Value && answer.isOwn && !answer.holder && answer.offset == offset && !answer.dependsOnChain();
}

static bool isLastingRefusal(const PropertyAnswer& answer)
{
    return answer.kind == PropertyAnswerKind::Uncacheable && !answer.didFlatten;
}

static void testReplacementOnPrototypeIsNotCached()
{
    Realm realm;
    VM& vm = realm.vm();
    Identifier name = Identifier::fromString(vm, "x"_s);
    JSObject* object = realm.createObjectWithoutPrototype();
    AOT_TEST_CHECK(storeAndClassify(realm.globalObject(), object, name, jsNumber(1)).kind == PropertyAnswerKind::Addition);
    AOT_TEST_CHECK(isOwnValueAt(storeAndClassify(realm.globalObject(), object, name, jsNumber(2)), 0));
    AOT_TEST_CHECK(isOwnValueAt(defineAndClassify(realm.globalObject(), object, name, jsNumber(3)), 0));

    object->didBecomePrototype(vm);
    AOT_TEST_CHECK(object->structure()->mayBePrototype() && object->structure()->get(vm, name) == 0);
    AOT_TEST_CHECK(isLastingRefusal(storeAndClassify(realm.globalObject(), object, name, jsNumber(4))));
    AOT_TEST_CHECK(isLastingRefusal(defineAndClassify(realm.globalObject(), object, name, jsNumber(5))));
    AOT_TEST_CHECK(object->getDirect(vm, name) == jsNumber(5));
}

static void testAdditionNamesTheCapacityItGrowsTo()
{
    constexpr unsigned maxCapacityGrown = 32;
    Realm realm;
    VM& vm = realm.vm();
    JSObject* object = realm.createObjectWithoutPrototype();
    bool didGrowFromFourToEight = false;
    for (unsigned index = 0;; ++index) {
        AOT_TEST_CHECK(index < JSFinalObject::defaultInlineCapacity + 4 * maxCapacityGrown);
        unsigned capacityBefore = object->structure()->outOfLineCapacity();
        PropertyAnswer answer = storeAndClassify(realm.globalObject(), object, Identifier::fromString(vm, makeString("field"_s, index)), jsNumber(index));
        unsigned capacityAfter = object->structure()->outOfLineCapacity();
        if (capacityAfter != capacityBefore && capacityBefore > maxCapacityGrown) {
            AOT_TEST_CHECK(isLastingRefusal(answer));
            break;
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
        { "store-answer/replacement-on-prototype-is-not-cached"_s, testReplacementOnPrototypeIsNotCached },
        { "store-answer/addition-names-the-capacity-it-grows-to"_s, testAdditionNamesTheCapacityItGrowsTo },
        { "store-answer/addition-beside-elements-names-the-indexing-header"_s, testAdditionBesideElementsNamesTheIndexingHeader },
        { "store-answer/dictionary-is-flattened-once"_s, testDictionaryIsFlattenedOnce },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
