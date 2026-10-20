/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class OwnPropertyBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit OwnPropertyBuiltinEmitter(CodeGenerator&);

    void generateOwnPropertyKeys(const InlineCall&);
    void generateObjectGetOwnPropertyDescriptor(const InlineCall&);
    void generateObjectHasOwn(const InlineCall&);
    void generateObjectPropertyIsEnumerable(const InlineCall&);
    void generateOwnPropertyDescriptorFields(const InlineCall&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
