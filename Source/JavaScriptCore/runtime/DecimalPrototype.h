/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/Decimal128.h>
#include <JavaScriptCore/JSObject.h>

namespace JSC {

// decimalLiteral("1.5") is 1.5m.
JSC_DECLARE_HOST_FUNCTION(decimalLiteral);
// decimalOperator(DecimalOperator, left, right). undefined if an operand is not a Decimal. Both operands of a unary operator are the same.
JSC_DECLARE_HOST_FUNCTION(decimalOperator);

// What toFixed() answers for the options that it has read, or null where it throws. This throws only for want of memory.
JSString* tryDecimalToFixed(JSGlobalObject*, Decimal128, JSValue digits, JSValue roundingMode);

class DecimalPrototype final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | HasStaticPropertyTable;

    template<typename CellType, SubspaceAccess>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        STATIC_ASSERT_ISO_SUBSPACE_SHARABLE(DecimalPrototype, Base);
        return &vm.plainObjectSpace();
    }

    static DecimalPrototype* create(VM&, Structure*);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    DECLARE_INFO;

private:
    DecimalPrototype(VM&, Structure*);
    void finishCreation(VM&);
};

} // namespace JSC
