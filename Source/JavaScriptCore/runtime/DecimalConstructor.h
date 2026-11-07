/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/InternalFunction.h>

namespace JSC {

class DecimalPrototype;

class DecimalConstructor final : public InternalFunction {
public:
    using Base = InternalFunction;

    static DecimalConstructor* create(VM&, Structure*, DecimalPrototype*);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    DECLARE_INFO;

private:
    DecimalConstructor(VM&, Structure*);
    void finishCreation(VM&, DecimalPrototype*);
};
STATIC_ASSERT_ISO_SUBSPACE_SHARABLE(DecimalConstructor, InternalFunction);

} // namespace JSC
