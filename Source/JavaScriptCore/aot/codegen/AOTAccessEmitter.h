/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellAllocationEmitter.h"
#include "AOTOperationEmitter.h"
#include "AOTRuntimeRecords.h"
#include "Structure.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

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
    void generateMayBindAnswersOfGetAndSetTraps(const ValueContext& value) { defineTest(value, mayBindAnswersOfTraps(value->targetOperand(), Structure::s_hasNonConfigurableReadOnlyOrGetterSetterPropertiesBits)); }
    void generateMayBindAnswersOfHasTraps(const ValueContext& value) { defineTest(value, mayBindAnswersOfTraps(value->targetOperand(), Structure::s_didPreventExtensionsBits | Structure::s_hasNonConfigurablePropertiesBits)); }
    void generateValidateAnswerOfGetTrap(const ValueContext&);
    void generateValidateDenialOfHasTrap(const ValueContext&);
    void generateValidateConsentOfDeleteTrap(const ValueContext&);
    void generateHasOwnByAnswerOfDescriptorTrap(const ValueContext&);
    void generateFinishStoreByTrap(const ValueContext&);
    void generateElemGet(const ValueContext&);
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
    void generateTryCopyFields(const ValueContext&);
    void generateGetSuperBase(const ValueContext&);
    void generateSuperGet(const ValueContext&);
    void generateSuperSet(const ValueContext&);

    // dict["on" + type]: the one user of the sum, or of a cast of it, is the read that stands right behind it. That read looks the property up by the two parts.
    bool leavesKeyToRead(ValueID sumOrCast) { return !!findReadByPartsOf(sumOrCast); }

    Boxed getById(const OperationContext&, Boxed receiver, PropertyKeyID, SiteCacheID, std::optional<Origin> = std::nullopt, ValueID receiverID = { });
    StubKey stubThatReadsAtSite(uint32_t identifierNumber, ValueID receiver);
    Boxed getMemberAboveLayouts(const ValueContext&);
    Boxed getByTablesOrNothing(const ValueContext&);
    Boxed getMemberAboveEmptySlot(const ValueContext&);
    void generateReadAboveFreshObject(const ValueContext&);
    void generateDidReadAbove(const ValueContext&);
    void checkAbsentFieldWasNotRemoved(const ValueContext& read, Boxed result);
    // What the slot at the site finds for every object with its Structure, where the object has that Structure and the slot is of the kind.
    Boxed loadBySlotAtSite(CellPointer object, Pointer site, Slot::Kind, LBasicBlock whenSlotIsOfOtherKind, LBasicBlock whenSlotIsOfOtherStructure);
    void putById(const OperationContext&, Boxed receiver, PropertyKeyID, Boxed value, PutMode, SiteCacheID);
    Condition hasById(const OperationContext&, Boxed receiver, PropertyKeyID);
    Boxed getByVal(const OperationContext&, Boxed receiver, Boxed key);
    // Where the program runs in several realms, each has its own symbol, and the tables that are asked by the number of a name are shared by all of them. Such a key goes as its cell.
    bool isSymbolOfRealm(PropertyKeyID) const;
    Boxed cellOfKey(const OperationContext&, PropertyKeyID);
    void putByVal(const OperationContext&, Boxed receiver, Boxed key, Boxed value, PutMode);
    Condition hasByVal(const OperationContext&, Boxed receiver, Boxed key);
    void recordArrival(const OperationContext&, Boxed receiver, uint32_t part = 0, ValueID key = { });
    // What countSiteCacheMiss() does in a stub: whether the site has missed often enough to be filled again.
    Condition countMissAtSite(Pointer site);
    // The Structure that the same change of prototype led to before. Storing it is the whole change only for an object that is extensible and is no prototype itself.
    Word32 findPrototypeTransition(CellPointer object, Boxed prototype, LBasicBlock isUnknown);
    void storeStructureAfterPrototypeTransition(CellPointer object, Word32 structureID);
    static PutMode putModeOf(ECMAMode mode) { return forMode(mode, PutMode::Strict, PutMode::Sloppy); }
    std::optional<CellPointer> tryAllocateCopyOfFields(const ValueContext& allocation, ValueID copy, const ScopedLambda<CellPointer()>& allocateEmpty);

private:
    Condition mayBindAnswersOfTraps(ValueID target, uint32_t bitsOfStructureThatMayBind);
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

    std::optional<uint32_t> findIdentifierNumber(const OperationContext&, PropertyKeyID);
    // By the read: whether the tables had an answer, for the test that stands behind the read.
    UncheckedKeyHashMap<uint32_t, LValue, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_didTablesAnswer;
    bool comesAgain(const OperationContext&);
    void putNamedProperty(const ValueContext&, Boxed object, PutMode);
    Boxed getFreeName(const ValueContext&);
    void putFreeName(const ValueContext&);
    CellPointer globalObject();
    Pointer globalBindingAddress(GlobalBindingID);
    TypedPointer globalBindingLocationOrElse(Pointer record, bool seesScriptBindings, LBasicBlock miss);
    void validateGlobalBinding(const ValueContext&, Boxed read);
    void defineAccessor(const ValueContext&, uint32_t accessorFlags);
    // An index in a register, or any value.
    struct ElementKey {
        LValue value;
        bool isInteger;
    };
    std::optional<ElementKey> elementKeyOf(const ValueContext&);
    struct ReadByParts {
        ValueID read;
        ValueID first;
        ValueID second;
    };
    std::optional<ReadByParts> findReadByPartsOf(ValueID sumOrCast);
    Boxed keyOperand(const OperationContext&, PropertyKeyID key, ValueID computedKey);
    struct ExcludedKeys {
        Word32 run;
        Word32 count;
        Pointer keys;
    };
    ExcludedKeys excludedKeysOf(const ValueContext&);
    void copyDataProperties(const ValueContext&);

    // Between a CopyObject and the CopyDataProperties right behind it: the object that holds what both copy, or null where each has to do its own.
    struct CopyOfTwo {
        ValueID second;
        CellPointer copy;
    };
    std::optional<CopyOfTwo> m_copyOfTwo;

    struct CopyAtAllocation {
        ValueID copy;
        Condition didCopy;
    };
    std::optional<CopyAtAllocation> m_copyAtAllocation;

    // `{ ...source, name, other }`: the definitions by name that come first behind a CopyObject whose copy is an ordinary object, with values that are there at the copy.
    // One call copies and defines, when it knows how. The code behind it asks whether it did: nothing that can run other code stands between them.
    struct DefinitionWithCopy {
        ValueID definition;
        uint32_t name;
        Boxed value;
    };
    Vector<DefinitionWithCopy, 4> findDefinitionsWithCopy(const ValueContext& copy);
    bool isThereAtCopy(ValueID copy, ValueID);
    Boxed valueAtCopy(const ValueContext& copy, ValueID);
    bool canTakeNames(ValueID);
    std::optional<Vector<ValueID, 4>> copiesThatMeet(const Target&, size_t index);
    BitVector m_definitionsWithCopy;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
