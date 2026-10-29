/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2006-2021 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#pragma once

#include "InternalFunction.h"

namespace JSC {

class FunctionPrototype final : public InternalFunction {
public:
    typedef InternalFunction Base;
#if ENABLE(AOT)
    // It lies above every function: a store to one of them must not find an object that overrides put on its way up. Its own members have the lock of the table.
    using JSObject::put;
    using JSObject::defineOwnProperty;
    using JSObject::setPrototype;
#endif

    static FunctionPrototype* create(VM& vm, Structure* structure)
    {
        FunctionPrototype* prototype = new (NotNull, allocateCell<FunctionPrototype>(vm)) FunctionPrototype(vm, structure);
        prototype->finishCreation(vm, String());
        return prototype;
    }

    void addFunctionProperties(VM&, JSGlobalObject*, JSFunction** callFunction, JSFunction** applyFunction, JSFunction** hasInstanceSymbolFunction);

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    DECLARE_INFO;

private:
    FunctionPrototype(VM&, Structure*);
    void finishCreation(VM&, const String& name);
};
STATIC_ASSERT_ISO_SUBSPACE_SHARABLE(FunctionPrototype, InternalFunction);

JSC_DECLARE_HOST_FUNCTION(functionProtoFuncToString);

} // namespace JSC
