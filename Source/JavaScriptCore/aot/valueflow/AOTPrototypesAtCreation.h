/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowPrototypes.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueFlowBuiltins.h"

namespace JSC { namespace AOT { namespace Typed {

// Who should have listed what lies above an object and has not: the table, for one of its own objects or classes, or nobody at all, because the object stands for others.
enum class SilentAboutPrototype : uint8_t { Table, Everybody };

inline BuiltinClassID wrapperOf(PrimitiveSet kind)
{
    uint16_t bit = static_cast<uint16_t>(kind);
    if (bit & Primitive::AnyString)
        return BuiltinClasses::String;
    if (bit & Primitive::Number)
        return BuiltinClasses::Number;
    if (bit & Primitive::Boolean)
        return BuiltinClasses::Boolean;
    if (bit & Primitive::AnySymbol)
        return BuiltinClasses::Symbol;
    if (bit & Primitive::BigInt)
        return BuiltinClasses::BigInt;
    return { };
}

// What lies directly above an object as it is made: two objects at the most. The rules and whoever asks behind the solve walk the same links.
// An allocation that names its prototype writes the link, so nothing is listed for it here.
void forEachPrototypeAtCreation(const Program& program, const ValueFlowUniverse& universe, ValueFlowObjectID object, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor, NOESCAPE const Invocable<void(SilentAboutPrototype)> auto& nobodyLists)
{
    const BuiltinTable& builtins = program.builtins();
    auto visitPrototypeOf = [&](BuiltinClassID builtinClass) {
        if (!builtinClass)
            return;
        if (BuiltinID prototype = builtins.at(builtinClass).prototype)
            functor(universe.objectOf(prototype));
        else
            nobodyLists(SilentAboutPrototype::Table);
    };
    auto hasHeritage = [&](LayoutID layout) {
        return layout && program.at(layout).has(LayoutFlags::HasHeritage);
    };

    switch (universe.kindOf(object)) {
    case ValueFlowObjectKind::Primitive:
        visitPrototypeOf(wrapperOf(static_cast<PrimitiveSet>(1u << object.index())));
        return;
    case ValueFlowObjectKind::Key:
        visitPrototypeOf(program.at(universe.keyOf(object)).kind == PropertyKeyKind::Text ? BuiltinClasses::String : BuiltinClasses::Symbol);
        return;
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::ResultOfBuiltin:
        nobodyLists(SilentAboutPrototype::Everybody);
        return;
    case ValueFlowObjectKind::Namespace:
    case ValueFlowObjectKind::ConstructedObject:
        return;
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin: {
        const Builtin& entry = builtins.at(universe.builtinOf(object));
        switch (entry.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            nobodyLists(SilentAboutPrototype::Table);
            return;
        case BuiltinPrototypeLink::Null:
            return;
        case BuiltinPrototypeLink::Builtin:
            functor(universe.objectOf(entry.inheritsFrom));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    case ValueFlowObjectKind::Closure: {
        const Function& function = program.at(universe.closureFunction(object));
        if (!hasHeritage(function.constructedClass))
            visitPrototypeOf(classOfFunctionObject(function));
        return;
    }
    case ValueFlowObjectKind::Instance:
        functor(universe.prototypeOf(universe.layoutOf(object)));
        return;
    case ValueFlowObjectKind::CoroutineResult:
    case ValueFlowObjectKind::RestArray:
    case ValueFlowObjectKind::FunctionPrototype:
        visitPrototypeOf(universe.classOf(object));
        return;
    case ValueFlowObjectKind::Prototype: {
        LayoutID layout = universe.classLayoutOf(object);
        if (hasHeritage(layout))
            return;
        FunctionID constructor = program.at(layout).constructor;
        visitPrototypeOf(constructor ? classAbovePrototypeProperty(program.at(constructor)) : BuiltinClasses::Object);
        return;
    }
    case ValueFlowObjectKind::Site: {
        const ValueFlowSite& site = *universe.siteOf(object);
        if (site.builtinClass)
            visitPrototypeOf(site.builtinClass == BuiltinClasses::Arguments ? BuiltinClasses::Object : site.builtinClass);
        if (!site.layout) {
            if (!site.builtinClass)
                nobodyLists(SilentAboutPrototype::Everybody);
            return;
        }
        switch (program.at(site.layout).kind) {
        case LayoutKind::Record:
        case LayoutKind::Container:
        case LayoutKind::BuiltinResult:
            if (!site.namesPrototype)
                visitPrototypeOf(BuiltinClasses::Object);
            return;
        case LayoutKind::Instance:
            if (!site.namesPrototype)
                functor(universe.prototypeOf(site.layout));
            return;
        case LayoutKind::Environment:
        case LayoutKind::Closure:
        case LayoutKind::CoroutineFrame:
        case LayoutKind::ModuleNamespace:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
