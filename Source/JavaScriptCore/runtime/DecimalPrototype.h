/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/JSObject.h>

namespace JSC {

// decimalOperator(DecimalOperator, left, right). undefined if an operand is not a Decimal. Both operands of a unary operator are the same.
JSC_DECLARE_HOST_FUNCTION(decimalOperator);

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
