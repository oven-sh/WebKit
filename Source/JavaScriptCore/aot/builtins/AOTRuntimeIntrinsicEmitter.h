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
    void generateIsPlainInstanceOf(const ValueContext&);
    void generateIsPlainHeirOf(const ValueContext&);
    void generateIsAsBorn(const ValueContext&);
    void generateIsAsBornExceptForAddedNames(const ValueContext&);
    void generateIsPrototypeChainPlain(const ValueContext&);
    void generateFindsNoMember(const ValueContext&);
    void generateGetInternalField(const ValueContext&);
    void generatePutInternalField(const ValueContext&);

private:
    Condition hasJSType(const Operation&, CellPointer);
    TypedPointer internalField(const Operation&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
