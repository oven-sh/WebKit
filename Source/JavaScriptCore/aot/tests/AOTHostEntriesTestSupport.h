/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTNativeTest.h"

namespace JSC { namespace AOT {

// The engine has a constant for what it declares and for the roles of the host. A test finds anything else of the host in the table that it is linked with.
inline BuiltinID hostBuiltin(ASCIILiteral token)
{
    BuiltinID id = BuiltinTable::singleton().builtinFromToken(token);
    AOT_TEST_CHECK(id);
    return id;
}

inline BuiltinClassID hostClass(ASCIILiteral token)
{
    BuiltinClassID id = BuiltinTable::singleton().classFromToken(token);
    AOT_TEST_CHECK(id);
    return id;
}

inline RuntimeModuleID hostModule(ASCIILiteral token)
{
    RuntimeModuleID id = BuiltinTable::singleton().moduleFromToken(token);
    AOT_TEST_CHECK(id);
    return id;
}

inline BuiltinName hostName(ASCIILiteral text)
{
    BuiltinName name = BuiltinTable::singleton().nameOf(byteCast<uint8_t>(text.span()));
    AOT_TEST_CHECK(name);
    return name;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
