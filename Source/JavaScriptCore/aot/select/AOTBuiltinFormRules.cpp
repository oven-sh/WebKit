/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinFormRules.h"

#if ENABLE(AOT)

#include "AOTForwardedCalls.h"
#include "AOTLayout.h"
#include "AOTPropertyAccessElements.h"
#include "AOTPropertyAccessReceivers.h"
#include "JSBoundFunction.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

InlineForm wholeOrTwoWay(const InlineBuiltinSite& site, bool handlesEveryValue)
{
    if (handlesEveryValue && hasOptionalSlowPath(site.builtin))
        return InlineForm::Whole;
    RELEASE_ASSERT(site.hasSlowPath);
    return InlineForm::TwoWay;
}

const Type& receiverTypeOf(const InlineBuiltinSite& site)
{
    ValueID receiver = site.receiver();
    RELEASE_ASSERT(receiver);
    return site.typeOf(receiver);
}

const Type* argumentTypeOf(const InlineBuiltinSite& site, unsigned index)
{
    std::span<const ValueID> arguments = site.arguments();
    return index < arguments.size() ? &site.typeOf(arguments[index]) : nullptr;
}

static InlineForm callIf(const InlineBuiltinSite& site, bool condition, InlineForm otherwise)
{
    if (!condition)
        return otherwise;
    RELEASE_ASSERT(site.hasSlowPath);
    return InlineForm::Call;
}

static bool hasObjectPart(const Type* type)
{
    return type && type->hasObjects();
}

static bool isInRareBlock(const InlineBuiltinSite& site)
{
    BlockID block = site.context.analyses().useLists().blockOf(site.call);
    return site.context.function().at(block).frequency.predicted() == BlockFrequency::Rare;
}

InlineForm chooseFormOfOwnPropertyBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::OwnPropertyKeys:
        // The list that is remembered for the shape of the object is found in line. That is worth its bytes wherever the code may well run.
        if (site.shape != CodeShape::Fast && isInRareBlock(site))
            return InlineForm::Whole;
        return callIf(site, !hasObjectPart(argumentTypeOf(site, 0)), InlineForm::TwoWay);
    case InlineBuiltin::ObjectGetOwnPropertyDescriptor:
        return callIf(site, !hasObjectPart(argumentTypeOf(site, 0)), InlineForm::TwoWay);
    case InlineBuiltin::ObjectHasOwn:
    case InlineBuiltin::ObjectPropertyIsEnumerable:
    case InlineBuiltin::OwnPropertyDescriptorFields:
    case InlineBuiltin::ObjectValues:
    case InlineBuiltin::ObjectEntries:
        return InlineForm::Whole;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static bool mayBeObjectOrNull(const Type& type)
{
    return type.hasObjects() || type.primitives & Primitive::Null;
}

// The constructor of a built-in class makes the objects of a class below it: cells of that class, which have no order word.
static bool mayBeMadeByBuiltinClass(const InlineBuiltinSite& site, const Type* type)
{
    const Program& program = site.context.program();
    return type && std::ranges::any_of(membersOf(program, *type), [&](LayoutID member) {
        return !!builtinBaseClassOf(program, member);
    });
}

static bool canFreezeInPlace(const InlineBuiltinSite& site)
{
    const Type* object = argumentTypeOf(site, 0);
    if (!object)
        return false;
    const Program& program = site.context.program();
    std::span<const LayoutID> members = membersOf(program, *object);
    // A module namespace object cannot be frozen: its exports stay writable, and the attempt throws.
    bool areOrdinaryObjects = std::ranges::all_of(members, [&](LayoutID member) {
        const Layout& layout = program.at(member);
        return (layout.kind == LayoutKind::Record || layout.kind == LayoutKind::Instance) && !hasFieldUnderSymbolCreatedByLibrary(program, layout) && !builtinBaseClassOf(program, member);
    });
    return !members.empty() && areOrdinaryObjects && !receiverStateOf(program, members).cannot(ObjectMutation::ChangeIntegrity);
}

static bool mayCompareCellContents(const Type& left, const Type& right)
{
    bool mayBothBeStrings = (left.primitives & Primitive::AnyString) && (right.primitives & Primitive::AnyString);
    bool mayBothBeBigInts = left.primitives & right.primitives & Primitive::BigInt;
    return mayBothBeStrings || mayBothBeBigInts;
}

InlineForm chooseFormOfObjectBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::ObjectCreate:
        return callIf(site, site.arguments().size() != 1 || !mayBeObjectOrNull(site.typeOf(site.arguments()[0])), InlineForm::TwoWay);
    case InlineBuiltin::GetPrototypeOf:
    case InlineBuiltin::CallObjectConstructor:
        return callIf(site, !hasObjectPart(argumentTypeOf(site, 0)), InlineForm::TwoWay);
    case InlineBuiltin::SetPrototypeOf:
        return callIf(site, site.arguments().size() < 2 || !hasObjectPart(argumentTypeOf(site, 0)) || mayBeMadeByBuiltinClass(site, argumentTypeOf(site, 0)) || site.shape != CodeShape::Fast, InlineForm::TwoWay);
    case InlineBuiltin::ObjectPrototypeToString:
        return callIf(site, !receiverTypeOf(site).hasObjects(), InlineForm::TwoWay);
    case InlineBuiltin::ObjectFreeze:
        return callIf(site, !canFreezeInPlace(site), InlineForm::TwoWay);
    case InlineBuiltin::ObjectAssign:
        return callIf(site, site.arguments().size() != 2 || !hasObjectPart(argumentTypeOf(site, 0)), InlineForm::TwoWay);
    case InlineBuiltin::ObjectIs: {
        std::span<const ValueID> arguments = site.arguments();
        RELEASE_ASSERT(arguments.size() == 2);
        return wholeOrTwoWay(site, !mayCompareCellContents(site.typeOf(arguments[0]), site.typeOf(arguments[1])));
    }
    case InlineBuiltin::ReturnThis:
        return InlineForm::Whole;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

InlineForm chooseFormOfFunctionBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::FunctionBind:
        return callIf(site, site.arguments().size() > 1 + JSBoundFunction::maxEmbeddedArgs || !receiverTypeOf(site).mayBeFunction(), InlineForm::TwoWay);
    case InlineBuiltin::ReflectApply:
        return callIf(site, site.arguments().size() != 3, InlineForm::TwoWay);
    case InlineBuiltin::ReflectConstruct:
        // Bytecode constructs in place only where the call is written so. Every other call has a frame of the host function.
        return callIf(site, !site.value().isDoneInPlace() || site.arguments().size() < 2 || site.arguments().size() > 3, InlineForm::TwoWay);
    case InlineBuiltin::FunctionApply:
        // Bytecode applies in place only where the call is written `f.apply(..)`. Every other call has a frame of Function.prototype.apply.
        return callIf(site, !forwardsInPlace(site.value()), InlineForm::Whole);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

InlineForm chooseFormOfErrorBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::NewError: {
        const Function& sourceFunction = site.context.program().sourceFunctionOf(site.context.function(), site.value());
        return callIf(site, sourceFunction.isHiddenInStackTraces() || site.arguments().size() > 2, InlineForm::TwoWay);
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

InlineForm chooseFormOfIntlBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::FormatNumber:
    case InlineBuiltin::FormatDateTime: {
        const Type* formatted = argumentTypeOf(site, 0);
        return callIf(site, !formatted || !formatted->isNumber(), InlineForm::TwoWay);
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

InlineForm chooseFormOfDateBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::DateGetField:
    case InlineBuiltin::DateToISOString:
        return InlineForm::TwoWay;
    case InlineBuiltin::DateNow:
    case InlineBuiltin::DateGetTime:
    case InlineBuiltin::DateGetMilliseconds:
    case InlineBuiltin::DateSetTime:
        return InlineForm::Whole;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
