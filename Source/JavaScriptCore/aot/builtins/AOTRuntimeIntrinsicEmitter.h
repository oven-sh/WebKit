/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class RuntimeIntrinsicEmitter final : public BuiltinFamilyEmitter {
public:
    explicit RuntimeIntrinsicEmitter(CodeGenerator&);

    void generateRequireBuiltinModule(const ValueContext&);
    void generateNativeBinding(const ValueContext&);
    void generateLinkTimeConstant(const ValueContext&);
    void generateHasJSType(const ValueContext&);
    void generateFindsDeclaredMember(const ValueContext&);
    void generateGetInternalField(const ValueContext&);
    void generatePutInternalField(const ValueContext&);

private:
    Condition hasJSType(const Value&, CellPointer);
    TypedPointer internalField(const Value&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
