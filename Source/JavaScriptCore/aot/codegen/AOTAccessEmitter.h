/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTRuntimeRecords.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class AccessEmitter final : public OperationEmitter {
public:
    explicit AccessEmitter(CodeGenerator&);

    void generatePropGet(const ValueContext&);
    void generatePropSet(const ValueContext&);
    void generatePropDefine(const ValueContext&);
    void generatePropHas(const ValueContext&);
    void generatePropDelete(const ValueContext&);
    void generateElemGet(const ValueContext&);
    void generateElemSet(const ValueContext&);
    void generateElemHas(const ValueContext&);
    void generateElemDelete(const ValueContext&);
    void generateElemDefine(const ValueContext&);
    void generateDefineGetter(const ValueContext&);
    void generateDefineSetter(const ValueContext&);
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
    // The Structure that the same change of prototype led to before. Storing it is the whole change only for an object that is extensible and is no prototype itself.
    Word32 findPrototypeTransition(CellPointer object, Boxed prototype, LBasicBlock isUnknown);
    static PutMode putModeOf(ECMAMode);

private:
    std::optional<uint32_t> findIdentifierNumber(const OperationContext&, PropertyKeyID);
    Boxed getByIdThroughSiteCache(const OperationContext&, Boxed receiver, uint32_t identifier, SiteCacheID, std::optional<Origin>);
    Boxed getByIdThroughTables(const OperationContext&, Boxed receiver, uint32_t identifier, std::optional<Origin>);
    void putByIdThroughSiteCache(const OperationContext&, Boxed receiver, uint32_t identifier, Boxed value, PutMode, SiteCacheID);
    void putByIdThroughTables(const OperationContext&, Boxed receiver, uint32_t identifier, Boxed value, PutMode);
    Boxed getFreeName(const ValueContext&);
    void putFreeName(const ValueContext&);
    void defineAccessor(const ValueContext&, uint32_t accessorFlags);
    Boxed keyOperand(const OperationContext&, PropertyKeyID key, ValueID computedKey);
    Boxed identifierOperand(const OperationContext&, PropertyKeyID);
    struct ExcludedKeys {
        Word32 run;
        Word32 count;
        Pointer keys;
    };
    ExcludedKeys excludedKeysOf(const ValueContext&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
