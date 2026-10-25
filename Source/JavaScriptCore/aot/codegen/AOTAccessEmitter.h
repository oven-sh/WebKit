/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellAllocationEmitter.h"
#include "AOTOperationEmitter.h"
#include "AOTRuntimeRecords.h"
#include <optional>
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

    Boxed getById(const OperationContext&, Boxed receiver, PropertyKeyID, SiteCacheID, std::optional<Origin> = std::nullopt);
    void putById(const OperationContext&, Boxed receiver, PropertyKeyID, Boxed value, PutMode, SiteCacheID);
    Condition hasById(const OperationContext&, Boxed receiver, PropertyKeyID);
    Boxed getByVal(const OperationContext&, Boxed receiver, Boxed key);
    void putByVal(const OperationContext&, Boxed receiver, Boxed key, Boxed value, PutMode);
    Condition hasByVal(const OperationContext&, Boxed receiver, Boxed key);
    void recordArrival(const OperationContext&, Boxed receiver, uint32_t part = 0);
    // The Structure that the same change of prototype led to before. Storing it is the whole change only for an object that is extensible and is no prototype itself.
    Word32 findPrototypeTransition(CellPointer object, Boxed prototype, LBasicBlock isUnknown);
    void storeStructureAfterPrototypeTransition(CellPointer object, Word32 structureID);
    static PutMode putModeOf(ECMAMode mode) { return forMode(mode, PutMode::Strict, PutMode::Sloppy); }
    std::optional<CellPointer> tryAllocateCopyOfFields(const ValueContext& allocation, ValueID copy, const ScopedLambda<CellPointer()>& allocateEmpty);

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

    std::optional<uint32_t> findIdentifierNumber(const OperationContext&, PropertyKeyID);
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
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
