/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinFormRules.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTTypedArrayKinds.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

template<typename Parameter>
static Parameter inlineParameterOf(const InlineBuiltinSite& site, unsigned index)
{
    ASSERT(index < numberOfParametersOf(site.builtin));
    return static_cast<Parameter>(site.overload.code.implementation.inlineParameters[index]);
}

static bool canParseIntInline(const InlineBuiltinSite& site)
{
    const Type* string = argumentTypeOf(site, 0);
    const Type* radix = argumentTypeOf(site, 1);
    return string && string->isAnyString() && (!radix || radix->isInt32());
}

static bool canStringifyNumberInline(const InlineBuiltinSite& site)
{
    const Type* radix = argumentTypeOf(site, 0);
    return receiverTypeOf(site).isNumber() && (!radix || radix->isInt32());
}

InlineForm chooseFormOfMathBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::MathAbs:
    case InlineBuiltin::MathFloor:
    case InlineBuiltin::MathCeil:
    case InlineBuiltin::MathTrunc:
    case InlineBuiltin::MathRound:
    case InlineBuiltin::MathFround:
    case InlineBuiltin::MathF16Round:
    case InlineBuiltin::MathSqrt:
    case InlineBuiltin::MathPow:
    case InlineBuiltin::MathMin:
    case InlineBuiltin::MathMax:
    case InlineBuiltin::MathImul:
    case InlineBuiltin::MathClz32:
    case InlineBuiltin::MathSign:
    case InlineBuiltin::MathFunction:
    case InlineBuiltin::MathATan2:
    case InlineBuiltin::MathHypot:
    case InlineBuiltin::IsNaN:
    case InlineBuiltin::IsFinite:
    case InlineBuiltin::NumberIsNaN:
    case InlineBuiltin::NumberIsFinite:
    case InlineBuiltin::NumberIsInteger:
    case InlineBuiltin::NumberIsSafeInteger:
    case InlineBuiltin::MathRandom:
        return InlineForm::Whole;
    case InlineBuiltin::MathMinOfArray:
    case InlineBuiltin::MathMaxOfArray:
        return InlineForm::TwoWay;
    case InlineBuiltin::CallNumberConstructor:
        return twoWayIf(site, site.arguments().size() == 1);
    case InlineBuiltin::ParseInt:
        return twoWayIf(site, canParseIntInline(site));
    case InlineBuiltin::NumberToString:
        return twoWayIf(site, canStringifyNumberInline(site));
    case InlineBuiltin::NumberToStringAppend:
        return InlineForm::Call;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static unsigned numberOfOperandsOfAtomics(AtomicsOperation operation)
{
    switch (operation) {
    case AtomicsOperation::Load:
        return 0;
    case AtomicsOperation::Add:
    case AtomicsOperation::And:
    case AtomicsOperation::Exchange:
    case AtomicsOperation::Or:
    case AtomicsOperation::Store:
    case AtomicsOperation::Sub:
    case AtomicsOperation::Xor:
        return 1;
    case AtomicsOperation::CompareExchange:
        return 2;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canStoreElementInline(const Type* stored, TypedArrayType elements)
{
    if (!stored)
        return false;
    if (isBigInt(elements))
        return !stored->isNever() && stored->hasOnlyPrimitives(Primitive::BigInt);
    return stored->isNumber();
}

static bool canAccessDataViewInline(const InlineBuiltinSite& site)
{
    return inlineParameterOf<TypedArrayType>(site, 0) != TypeFloat16 && isNumberArgument(site, 0);
}

static bool canAccessAtomicallyInline(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    if (arguments.size() != 2 + numberOfOperandsOfAtomics(inlineParameterOf<AtomicsOperation>(site, 0)))
        return false;
    if (!hasIntegerElements(typedArrayTypeOf(site.context.program().builtins(), site.typeOf(arguments[0]))))
        return false;
    return std::ranges::all_of(arguments.subspan(1), [&](ValueID argument) {
        return site.typeOf(argument).isNumber();
    });
}

static bool canAccessBufferInline(const InlineBuiltinSite& site)
{
    std::optional<TypedArrayType> elements = elementsMovedBy(inlineParameterOf<unsigned>(site, 1), inlineParameterOf<NumberKind>(site, 2));
    if (!elements)
        return false;
    unsigned numberOfStoredValues = inlineParameterOf<AccessDirection>(site, 0) == AccessDirection::Write;
    if (numberOfStoredValues && !canStoreElementInline(argumentTypeOf(site, 0), *elements))
        return false;
    const Type* offset = argumentTypeOf(site, numberOfStoredValues);
    return !offset || offset->isNumber();
}

static TypedArrayType elementTypeOfView(const InlineBuiltinSite& site, const Type& view)
{
    TypedArrayType elements = view.isObject() ? typedArrayTypeOf(site.context.program().builtins(), view) : NotTypedArray;
    return isTypedView(elements) ? elements : NotTypedArray;
}

// Copies bytes between typed arrays of known types, at numeric positions. The inline checks are larger than a call, so this is only used in hot code.
static bool canCopyBufferInline(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    if (site.shape != CodeShape::Fast || arguments.empty() || arguments.size() > 4 || !elementTypeOfView(site, receiverTypeOf(site)) || !elementTypeOfView(site, site.typeOf(arguments[0])))
        return false;
    return std::ranges::all_of(arguments.subspan(1), [&](ValueID position) {
        return site.typeOf(position).isNumber();
    });
}

InlineForm chooseFormOfBufferBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::DataViewByteLength:
        return InlineForm::TwoWay;
    case InlineBuiltin::ArrayBufferByteLength:
        return InlineForm::Whole;
    case InlineBuiltin::DataViewGet:
        return twoWayIf(site, canAccessDataViewInline(site));
    case InlineBuiltin::DataViewSet:
        return twoWayIf(site, canAccessDataViewInline(site) && canStoreElementInline(argumentTypeOf(site, 1), inlineParameterOf<TypedArrayType>(site, 0)));
    case InlineBuiltin::Atomics:
        return twoWayIf(site, canAccessAtomicallyInline(site));
    case InlineBuiltin::BufferAccess:
        return twoWayIf(site, canAccessBufferInline(site));
    case InlineBuiltin::BufferCopy:
        return twoWayIf(site, canCopyBufferInline(site));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static bool canAllocateTypedArrayInline(const InlineBuiltinSite& site)
{
    const BuiltinTable& table = site.context.program().builtins();
    BuiltinClassID constructedClass = table.at(site.overload.builtin).instanceClass;
    return constructedClass && table.typedArrayClassOf(constructedClass) == constructedClass && isNumberArgument(site, 0);
}

static bool canCopyTypedArrayInline(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    return arguments.size() == 1 && hasInlineElementAccess(typedArrayTypeOf(site.context.program().builtins(), site.typeOf(arguments[0])));
}

// Elements copied into a typed array of the same type are copied as raw bytes.
static bool canSetTypedArrayInline(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    if (site.shape != CodeShape::Fast || arguments.empty() || arguments.size() > 2)
        return false;
    TypedArrayType elements = elementTypeOfView(site, receiverTypeOf(site));
    return elements && elementTypeOfView(site, site.typeOf(arguments[0])) == elements && (arguments.size() == 1 || site.typeOf(arguments[1]).isNumber());
}

InlineForm chooseFormOfArrayBufferViewBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::TypedArrayLength:
    case InlineBuiltin::TypedArrayByteOffset:
        return InlineForm::TwoWay;
    case InlineBuiltin::TypedArrayByteLength:
        return twoWayIf(site, typedArrayTypeOf(site.context.program().builtins(), receiverTypeOf(site)) != NotTypedArray);
    case InlineBuiltin::TypedArrayWithLength:
        return canAllocateTypedArrayInline(site) ? InlineForm::Whole : InlineForm::Call;
    case InlineBuiltin::ArrayFrom:
        return twoWayIf(site, canCopyTypedArrayInline(site));
    case InlineBuiltin::TypedArraySet:
        return twoWayIf(site, canSetTypedArrayInline(site));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
