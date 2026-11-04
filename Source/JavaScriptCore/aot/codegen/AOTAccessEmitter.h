/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include <optional>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

struct MentionedAllocation;

class AccessEmitter final : public OperationEmitter {
public:
    explicit AccessEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generatePropGet(const ValueContext&);
    void generatePropSet(const ValueContext&);
    void generatePropDefine(const ValueContext&);
    void generatePropHas(const ValueContext&);
    void generatePropDelete(const ValueContext&);
    void generateGetGlobalBinding(const ValueContext&);
    void generateSetGlobalBinding(const ValueContext&);
    void generateGetCachedGetter(const ValueContext&);
    void generateGetAccessorOfMember(const ValueContext&);
    void generateGetCachedValue(const ValueContext&);
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
    void generateGetSuperBase(const ValueContext&);
    void generateSuperGet(const ValueContext&);
    void generateSuperSet(const ValueContext&);
    void generateMayBindAnswersOfGetAndSetTraps(const ValueContext&);
    void generateMayBindAnswersOfHasTraps(const ValueContext&);
    void generateValidateAnswerOfGetTrap(const ValueContext&);
    void generateValidateDenialOfHasTrap(const ValueContext&);
    void generateValidateConsentOfDeleteTrap(const ValueContext&);
    void generateHasOwnByAnswerOfDescriptorTrap(const ValueContext&);
    void generateFinishStoreByTrap(const ValueContext&);

    // `site` is what siteOf() gave for the access. `receiverID` is the operand that holds the receiver, where one does.
    Boxed getById(const OperationContext&, Boxed receiver, PropertyKeyID, std::optional<Pointer> site = std::nullopt, ValueID receiverID = { });
    void putById(const OperationContext&, Boxed receiver, PropertyKeyID, Boxed value, PutMode, std::optional<Pointer> site = std::nullopt);
    // For an operation with `siteCaches` in the schema: the Slot that remembers what the access found last, where the access takes one.
    std::optional<Pointer> siteOf(const OperationContext&, PropertyKeyID, SlotAccess);
    // For a closed read or call under a name: the SiteAnswers of the place, where the place comes again.
    std::optional<NumberedImmediate> answersOf(const OperationContext&, PropertyKeyID, SiteUse);
    StubKey stubThatReadsAtSite(PropertyKeyID, ValueID receiver);
    Condition hasById(const OperationContext&, Boxed receiver, PropertyKeyID);
    Boxed getByVal(const OperationContext&, Boxed receiver, Boxed key);
    // Where the program runs in several realms, each has its own symbol, and the tables that are asked by the number of a name are shared by all of them. Such a key goes as its cell.
    bool isSymbolOfRealm(PropertyKeyID) const;
    Boxed cellOfKey(const OperationContext&, PropertyKeyID);
    void putByVal(const OperationContext&, Boxed receiver, Boxed key, Boxed value, PutMode);
    Condition hasByVal(const OperationContext&, Boxed receiver, Boxed key);
    // The Structure that the same change of prototype led to before. Storing it is the whole change only for an object that is extensible and is no prototype itself.
    Word32 findPrototypeTransition(CellPointer object, Boxed prototype, LBasicBlock isUnknown);
    void storeStructureAfterPrototypeTransition(CellPointer object, Word32 structureID);
    // For generateNewObject(). `copy` is the TryCopyFields that the block generates right behind the allocation of a literal without fields, and that fills the literal.
    // A source that can be copied is copied as the cell is made, and `allocateEmpty` makes the cell for any other. generateTryCopyFields() then answers with what was done here and appends nothing.
    // Nothing: nothing was appended, and the two are generated one behind the other.
    std::optional<CellPointer> tryAllocateCopyOfFields(const ValueContext& allocation, const MentionedAllocation&, ValueID copy, const ScopedLambda<CellPointer()>& allocateEmpty);
    // For generateCall(), in front of everything else. `object.name(a, b)`: where generatePropGet() has left the read of the callee to the call, one stub reads and calls, the call is defined, and the answer is true.
    bool tryCallMethodByName(const ValueContext& call);
    // Of a NewRegExp that stands between a read that is left to its call and the call: the call takes the object that all evaluations share.
    bool isPatternLeftToCall(ValueID pattern) const { return m_readLeftToCall && m_readLeftToCall->pattern == pattern; }
    static PutMode putModeOf(ECMAMode mode) { return forMode(mode, PutMode::Strict, PutMode::Sloppy); }

private:
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

    // False: the unit runs at most once, or is the body of a module, and the operation stands in no loop of it.
    bool comesAgain(const OperationContext&) const;
    struct StoreOfKnownField {
        StubKey stub;
        uint32_t offsetOfRecord;
    };
    // Where every receiver is a compiled object with a plain field of the name at one place of its layout's record: the stub that is told that place.
    std::optional<StoreOfKnownField> findStoreOfKnownField(const Type& receiver, PropertyKeyID, PutMode);
    void putNamedProperty(const ValueContext&, Boxed object, PutMode);
    Boxed getNamedProperty(const ValueContext&);
    // The block generates the one operation that uses the read right behind it: a call of what is read, on the object that it is read from, which the stub CallMethodById can make.
    bool isReadOfMethodCalledNext(const ValueContext& read);
    ValueID findPatternLeftToCall(const ValueContext& read);
    // `return table.get(key)`: where the receiver is as its class makes it, the built-in answers and the unit returns. Else nothing has happened, and the read and the tail call follow as they are written.
    void returnWhatMethodOfBuiltinClassGives(const ValueContext& read);
    // What the slot at the site finds for every object with its Structure, where the object has that Structure and the slot is of the kind.
    Boxed loadBySlotAtSite(CellPointer object, Pointer site, Slot::Kind, LBasicBlock whenSlotIsOfOtherKind, LBasicBlock whenSlotIsOfOtherStructure);
    Boxed getMemberAboveLayouts(const ValueContext&, Boxed receiver, Pointer site);
    Boxed getMemberInSlotOrByName(const ValueContext&, Boxed receiver, const PlacedSlot&);
    // The one place in the cell where every receiver keeps the field, where the bytes there say whether the object has it.
    std::optional<PlacedSlot> findSlotInCellOfEveryReceiver(const Type& receiver, PropertyKeyID);
    Boxed getFreeName(const ValueContext&);
    // The GetterSetter. The key is no symbol of a realm.
    CellPointer accessorAboveObjectByFrontCell(CellPointer object, PropertyKeyID, LBasicBlock findsNoGetter);
    void putFreeName(const ValueContext&);
    CellPointer globalObject();
    // The record of the binding, by NumberSpace::GlobalBinding.
    Pointer globalBindingAddress(PropertyKeyID, OptionSet<GlobalBindingFlags>);
    TypedPointer globalBindingLocationOrElse(Pointer record, bool seesScriptBindings, LBasicBlock miss);
    void defineAccessor(const ValueContext&, uint32_t accessorFlags);
    Condition mayBindAnswersOfTraps(ValueID target, uint32_t bitsOfStructureThatMayBind);
    // An index in a register, or any value.
    struct ElementKey {
        LValue value;
        bool isInteger;
    };
    std::optional<ElementKey> elementKeyOf(const ValueContext&);
    Boxed keyOperand(const OperationContext&, PropertyKeyID key, ValueID computedKey);
    struct ExcludedKeys {
        Word32 run;
        Word32 count;
        Pointer keys;
    };
    ExcludedKeys excludedKeysOf(const ValueContext&);
    ExcludedKeys noExcludedKeys();
    CellPointer copyObject(const ValueContext&, Boxed source, ExcludedKeys);
    // What the source runs or throws is shown at the origin. `excludedKeys` is asked where the operation is called.
    void copyDataProperties(const ValueContext&, CellPointer target, ValueID source, Origin, NOESCAPE const Invocable<ExcludedKeys()> auto& excludedKeys);

    struct CopyAtAllocation {
        ValueID copy;
        Condition didCopy;
    };
    // From tryAllocateCopyOfFields() until the TryCopyFields is generated.
    std::optional<CopyAtAllocation> m_copyAtAllocation;

    struct ReadLeftToCall {
        ValueID read;
        PropertyKeyID key;
        Origin origin;
        ValueID pattern;
    };
    // From generatePropGet() until the call is generated.
    std::optional<ReadLeftToCall> m_readLeftToCall;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
