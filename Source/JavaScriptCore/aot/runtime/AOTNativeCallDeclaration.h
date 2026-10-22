/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTNativeCallValidation.h"
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

enum class NativeCallDeclarationSource : uint8_t {
    Compiler,
    CalleeInRealm,
    NativeCodeOfCallee,
    NameOfModuleMember,
    NotInTable,
};

struct NativeCallDeclaration {
    NativeCallDeclarationSource source { NativeCallDeclarationSource::NotInTable };
    BuiltinID builtin;
    Vector<const BuiltinOverload*, 4> overloads;
};

String nameOfNativeFunction(JSObject* callee);
// May create objects of the realm that are created on demand.
NativeCallDeclaration findDeclaration(const NativeCall&, const NativeCallExpectation&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
