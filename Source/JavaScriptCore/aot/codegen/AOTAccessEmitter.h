/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTOperationEmitter.h"
#include "AOTStaleAnswers.h"
#include <optional>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

enum class PresenceQuestion : uint8_t;

namespace Typed {

struct MentionedAllocation;

bool namesSizeOfTables(const Program&, const Facts&, PropertyKeyID);

class AccessEmitter final : public OperationEmitter {
public:
    explicit AccessEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
        , m_functionComesAgain(!m_program.at(m_id).isModuleBody())
    {
    }

    void generatePropGet(const ValueContext&);
    void generatePropSet(const ValueContext&);
    void generatePropDefine(const ValueContext&);
    void generatePropHas(const ValueContext&);
    void generatePropDelete(const ValueContext&);
    void generateGetGlobalBinding(const ValueContext&);
    void generateSetGlobalBinding(const ValueContext&);
    void generateGetCachedGetter(const ValueContext&);
    void generateElemGetOrOwnAccessor(const ValueContext&);
    void generateGetGetterOfAccessorOrUndefined(const ValueContext&);
    void generateGetValueAtSiteOrAbsent(const ValueContext&);
    void generateHasPropertyOrAbsent(const ValueContext&);
    void generateGetOwnValueUnderKeyOrAbsent(const ValueContext&);
    void generateGetGlobalPropertyAddress(const ValueContext&);
    void generateIsPropertyFiled(const ValueContext&);
    void generateGetKeyedPropertyAddress(const ValueContext&);
    void generateGetKeyedProperty(const ValueContext&);
    void generatePutKeyedProperty(const ValueContext&);
    void generateGetKeyedValueOrAbsent(const ValueContext&);
    void generateGetTemplateObjectRaw(const ValueContext&);
    void generateFinishAccessThroughProxy(const ValueContext&);
    std::array<const Machine::AbstractHeap*, 2> heapsOfKeyedProperty() const;
    void generateGetGlobalProperty(const ValueContext&);
    void generatePutGlobalBinding(const ValueContext&);
    void generateGetValueUnderKeyOrAbsent(const ValueContext&);
    void generateGetPropertyOfHeir(const ValueContext&);
    void generatePutPropertyOfHeir(const ValueContext&);
    void generateElemGet(const ValueContext&);
    void generateElemGetByKeyParts(const ValueContext&);
    void generateElemSet(const ValueContext&);
    void generateElemHas(const ValueContext&);
    void generateElemDelete(const ValueContext&);
    void generateElemDefine(const ValueContext&);
    void generateDefineGetter(const ValueContext&);
    void generateDefineSetter(const ValueContext&);
    void generateTryDefineLiveBinding(const ValueContext&);
    void generateTryReExportFields(const ValueContext&);
    void generateSetFunctionName(const ValueContext&);
    void generateSetLiteralPrototype(const ValueContext&);
    void generateCopyDataProperties(const ValueContext&);
    void generateCopyObject(const ValueContext&);
    void generateCopyObjectAndDefine(const ValueContext&);
    void generateCopyTwoObjects(const ValueContext&);
    void generateTryCopyFields(const ValueContext&);
    Vector<FieldIndex, 8> fieldsThatTakeCopyFromOtherLayout(const Operation& copy);
    void copyFieldsFromOtherLayout(const Operation& copy, CellPointer source, CellPointer target);
    void noteCopyMadeAtAllocation(ValueID copy);
    void generateGetSuperBase(const ValueContext&);
    void generateSuperGet(const ValueContext&);
    void generateSuperSet(const ValueContext&);

    Boxed getById(const OperationContext&, Boxed receiver, PropertyKeyID, ValueID receiverID = { });
    Boxed getByIdThroughStub(const OperationContext&, Boxed receiver, PropertyKeyID, ValueID receiverID);
    bool isErrorMessageRead(const OperationContext&) const;
    Boxed loadErrorMessage(Boxed receiver, ValueID receiverID, LBasicBlock whenNotKept);
    bool verifiesFastPathAnswers(PropertyKeyID);
    Word32 slowPathReadCount() { return loadMutable32(AOT_INSTANCE_OFFSET(propertyCaches.slowPathReadCount)); }
    Word32 recordPendingRead(Boxed receiver, PropertyKeyID);
    void verifyFastPathAnswer(const OperationContext&, Boxed receiver, PropertyKeyID, Boxed answer, StubThatAnswers, Word32 countBeforeRead);
    void putById(const OperationContext&, Boxed receiver, PropertyKeyID, Boxed value, PutMode, std::optional<Pointer> site = std::nullopt);
    std::optional<Pointer> siteOf(const OperationContext&, PropertyKeyID, SlotAccess);
    std::optional<Pointer> siteOfStore(const OperationContext&, PropertyKeyID, SlotAccess);
    std::optional<NumberedImmediate> answersOf(const OperationContext&, PropertyKeyID, SiteUse);
    struct ReadOfGroup {
        ValueID id;
        PropertyKeyID key;
        NumberedImmediate answers;
    };
    std::optional<NumberedImmediate> answersOfReadOutOfRow(BlockID, const Operation&);
    void readGroupOffOneValue(const ValueContext&, Boxed receiver, bool mayBeNoCell, NumberedImmediate answersOfFirst);
    StubKey stubThatReadsAtSite(PropertyKeyID, ValueID receiver);
    ValueID readAfterStore(const ValueContext& store);
    LValue newestAnswerOfSite(Boxed receiver, ValueID receiverID, NumberedImmediate answers, LBasicBlock asksStub, LBasicBlock hasOtherKey = nullptr);
    std::optional<NumberedImmediate> inlineReadAnswers(const ValueContext&);
    Boxed readWithInlineSiteCheck(const ValueContext&, Boxed receiver, ValueID receiverID, NumberedImmediate answers);
    StubKey stubOfEffectFreeRead(const Operation&);
    Condition hasById(const OperationContext&, Boxed receiver, PropertyKeyID);
    Condition findsName(const OperationContext&, Boxed receiver, PropertyKeyID, PresenceQuestion);
    Boxed getByVal(const OperationContext&, Boxed receiver, Boxed key);
    LValue tryUnboxConstantIndex(Boxed key);
    bool isSymbolOfRealm(PropertyKeyID key) const { return !m_plan.hasOneInstance() && !liesInThread(key) && m_program.at(key).kind == PropertyKeyKind::CreatedSymbol; }
    Boxed cellOfKey(const OperationContext&, PropertyKeyID);
    void putByVal(const OperationContext&, Boxed receiver, Boxed key, Boxed value, PutMode);
    Condition hasByVal(const OperationContext&, Boxed receiver, Boxed key);
    Word32 findPrototypeTransition(CellPointer object, Boxed prototype, LBasicBlock isUnknown);
    void storeStructureAfterPrototypeTransition(CellPointer object, Word32 structureID);
    std::optional<CellPointer> tryAllocateCopyOfFields(const ValueContext& allocation, const MentionedAllocation&, ValueID copy, const ScopedLambda<CellPointer()>& allocateEmpty);
    bool tryCallMethodByName(const ValueContext& call);
    bool readsTruthAtSite(BlockID, ValueID read) const;
    std::optional<unsigned> integerThatLengthIsAskedFor(BlockID, ValueID read, ValueID test);
    bool isPatternLeftToCall(ValueID pattern) const { return m_readLeftToCall && m_readLeftToCall->pattern == pattern; }
    bool methodMayBeUnread(ValueID read) const { return m_methodsThatMayBeUnread.get(read.index()); }
    static PutMode putModeOf(ECMAMode mode) { return forMode(mode, PutMode::Strict, PutMode::Sloppy); }

private:
    HashMap<uint32_t, LValue, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_valuesReadInFront;
    HashMap<uint32_t, LValue, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_valuesStoredBeforeLoop;
    template<typename T> static T forMode(ECMAMode mode, T whenStrict, T whenSloppy)
    {
        switch (mode) {
        case ECMAMode::Strict:
            return whenStrict;
        case ECMAMode::Sloppy:
            return whenSloppy;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool comesAgain(const OperationContext&) const;
    bool keepsSite(const OperationContext&) const;
    std::optional<NumberedImmediate> answersOfKeyThatIsNoSymbolOfRealm(const OperationContext&, PropertyKeyID, SiteUse);
    struct StoreOfKnownField {
        StubKey stub;
        uint32_t offsetOfRecord;
    };
    std::optional<StoreOfKnownField> findStoreOfKnownField(const Type& receiver, PropertyKeyID, PutMode);
    void putNamedProperty(const ValueContext&, Boxed object, PutMode);
    Boxed getNamedProperty(const ValueContext&);
    void branchOnTruthAtSite(const ValueContext& read);
    void branchOnWhetherLengthIs(const ValueContext& read, unsigned integer);
    std::optional<Boxed> lookWhetherReceiverIsAsMadeInPlaceOfRead(const ValueContext&);
    bool isReadOfMethodCalledNext(const ValueContext& read);
    ValueID findPatternLeftToCall(const ValueContext& read);
    void returnWhatMethodOfBuiltinClassGives(const ValueContext& read);
    Boxed loadBySlotAtSite(CellPointer object, Pointer site, Slot::Kind, LBasicBlock whenSlotIsOfOtherKind, LBasicBlock whenSlotIsOfOtherStructure);
    Boxed getMemberAboveLayouts(const ValueContext&, Boxed receiver, Pointer site);
    Boxed getMemberInSlotOrByName(const ValueContext&, Boxed receiver, const PlacedSlot&);
    bool storeLastIndexInLine(const ValueContext&, Boxed object, PutMode);
    struct ShapesOfStoreAtSite {
        Type known;
        bool asksTableOfFields { false };
        LBasicBlock hasOtherStructure { nullptr };
        LBasicBlock asksStub { nullptr };
    };
    template<typename UsePlace> void checkStoreSiteInline(Boxed receiver, Pointer site, const ShapesOfStoreAtSite&, const UsePlace&);
    bool storeAskingSiteInLine(const ValueContext&, Boxed receiver, Boxed stored, PutMode, Pointer site);
    std::optional<PlacedSlot> findSlotInCellOfEveryReceiver(const Type& receiver, PropertyKeyID);
    Boxed getFreeName(const ValueContext&);
    void putFreeName(const ValueContext&);
    CellPointer globalObject();
    Pointer globalBindingAddress(PropertyKeyID, OptionSet<GlobalBindingFlags>);
    Pointer propertyAddressOrElse(Pointer record, bool seesScriptBindings, LBasicBlock miss);
    TypedPointer globalBindingLocationOrElse(Pointer record, bool seesScriptBindings, LBasicBlock miss);
    bool lookVouchesForPropertyOfHeir(const ValueContext&) const;
    TypedPointer locationOfPropertyOfHeir(const ValueContext&, CellPointer object);
    void defineAccessor(const ValueContext&, uint32_t accessorFlags);
    struct ElementKey {
        LValue value;
        bool isInteger;
    };
    std::optional<ElementKey> elementKeyOf(const ValueContext&);
    bool readsElementsRepeatedly(const ValueContext&, ValueID object);
    Boxed presentElementOrElse(const ValueContext&, ValueID objectOperand, const ElementKey&, LBasicBlock outOfLine);
    void overwriteElementOrElse(const ValueContext&, StubKey, Boxed object, const ElementKey&, Boxed stored);
    Boxed keyOperand(const OperationContext&, PropertyKeyID key, ValueID computedKey);
    struct ExcludedKeys {
        Word32 run;
        Word32 count;
        Pointer keys;
    };
    ExcludedKeys excludedKeysOf(const ValueContext&);
    ExcludedKeys noExcludedKeys();
    CellPointer copyObject(const ValueContext&, Boxed source, ExcludedKeys);
    void copyDataProperties(const ValueContext&, CellPointer target, ValueID source, Origin, NOESCAPE const Invocable<ExcludedKeys()> auto& excludedKeys);

    struct CopyAtAllocation {
        ValueID copy;
        Condition didCopy;
    };
    std::optional<CopyAtAllocation> m_copyAtAllocation;

    struct ReadLeftToCall {
        ValueID read;
        PropertyKeyID key;
        Origin origin;
        ValueID pattern;
    };
    std::optional<ReadLeftToCall> m_readLeftToCall;
    ValueID m_baseOfSuperLeftToRead;
    BitVector m_methodsThatMayBeUnread;
    const bool m_functionComesAgain;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
