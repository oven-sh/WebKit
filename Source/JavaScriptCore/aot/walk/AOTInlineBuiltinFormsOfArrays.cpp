/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCallPlan.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTIRProgram.h"
#include "AOTTypeArena.h"
#include "AOTTypedArrayKinds.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

#define AOT_CASE_IN_FORMS_OF_ARRAYS(name) case InlineBuiltin::name:

#define FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_FORMS_OF_ARRAYS(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_OWN_PROPERTY_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_OBJECT_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_HASH_TABLE_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_MAP_SET_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_PROMISE_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_BUFFER_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_MATH_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_DATE_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_FUNCTION_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_STRING_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_REG_EXP_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ERROR_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_INTL_BUILTIN(v)

static_assert(hasOptionalSlowPath(InlineBuiltin::ArrayIterator) && hasOptionalSlowPath(InlineBuiltin::ArrayLength) && hasOptionalSlowPath(InlineBuiltin::TypedArrayWithLength));
static_assert(!hasOptionalSlowPath(InlineBuiltin::ArrayAt) && !hasOptionalSlowPath(InlineBuiltin::ArrayPop) && !hasOptionalSlowPath(InlineBuiltin::ArrayPush) && !hasOptionalSlowPath(InlineBuiltin::ArrayIsArray));
static_assert(!hasOptionalSlowPath(InlineBuiltin::TypedArrayLength) && !hasOptionalSlowPath(InlineBuiltin::TypedArrayByteLength) && !hasOptionalSlowPath(InlineBuiltin::TypedArrayByteOffset) && !hasOptionalSlowPath(InlineBuiltin::TypedArraySet));

namespace {

class ArrayFormChooser {
public:
    explicit ArrayFormChooser(const InlineBuiltinQuery& query)
        : m_query(query)
        , m_types(query.context.types)
    {
    }

    InlineForm formOfArrayBuiltin() const;
    InlineForm formOfArrayStorageBuiltin() const;
    InlineForm formOfArrayBufferViewBuiltin() const;

private:
    static InlineForm wholeIf(bool takesOperands) { return takesOperands ? InlineForm::Whole : InlineForm::Call; }
    InlineForm twoWayIf(bool takesOperands) const { return takesOperands && m_query.hasSlowPath ? InlineForm::TwoWay : InlineForm::Call; }

    const BuiltinTable& table() const { return m_query.context.program.builtins(); }
    TypeID receiver() const { return m_query.receiver(); }
    TypeID argument(unsigned index) const { return m_query.argument(index); }
    unsigned numberOfArguments() const { return m_query.numberOfArguments(); }
    bool passesArgumentsOneByOne() const { return m_query.form == BuiltinCallForm::Whole; }

    bool isValue(TypeID type) const { return type && !isNeverType(type) && m_types.isSubset(type, TypeTable::anything); }
    bool isNumber(TypeID type) const { return type && m_types.at(type).isNumber(); }
    bool isString(TypeID type) const { return type && m_types.at(type).isAnyString(); }
    bool isInstanceOf(TypeID type, BuiltinClassID builtinClass) const { return type && !isNeverType(type) && m_types.isSubset(type, m_types.builtinClass(builtinClass)); }
    bool isArray(TypeID type) const { return isInstanceOf(type, BuiltinClasses::Array); }
    bool isPristineArrayWithStorageForm(TypeID) const;
    bool areNumbers(unsigned numberOfLeadingArguments) const;
    bool areValuesFrom(unsigned firstArgument) const;

    bool searchesForString() const { return numberOfArguments() == 1 && isString(argument(0)); }
    bool searchesForStringFromStart() const { return numberOfArguments() == 2 && isString(argument(0)) && isNumber(argument(1)); }
    bool canSearchInline() const;
    bool canJoinInline() const { return !numberOfArguments() || (numberOfArguments() == 1 && isString(argument(0))); }
    bool canConcatenateInline() const;
    bool canSliceInline() const { return numberOfArguments() <= 2 && areNumbers(numberOfArguments()); }
    bool canSpliceInline() const;
    bool canCopySplicedInline() const;

    bool isInstanceOfReceiverClass(TypeID) const;
    TypedArrayType elementTypeOfView(TypeID) const;
    bool constructsViewOfOneElementType() const;
    bool movesElementsOfOneType() const;

    InlineForm formOfPush() const;
    InlineForm formOfIsArray() const;

    const InlineBuiltinQuery& m_query;
    TypeArena& m_types;
};

} // anonymous namespace

static constexpr unsigned numberOfRangeArgumentsOfSplice = 2;

bool ArrayFormChooser::areNumbers(unsigned numberOfLeadingArguments) const
{
    for (unsigned index = 0; index < numberOfLeadingArguments; ++index) {
        if (!isNumber(argument(index)))
            return false;
    }
    return true;
}

bool ArrayFormChooser::areValuesFrom(unsigned firstArgument) const
{
    for (unsigned index = firstArgument; index < numberOfArguments(); ++index) {
        if (!isValue(argument(index)))
            return false;
    }
    return true;
}

bool ArrayFormChooser::canSearchInline() const
{
    constexpr uint16_t comparedByValue = Primitive::Number | Primitive::AnyString | Primitive::BigInt;
    if (numberOfArguments() != 1 || !isValue(argument(0)))
        return false;
    Type searched = m_types.at(argument(0));
    if (searched.isNumber() || !(searched.primitives & comparedByValue))
        return true;
    return searched.hasObjects() || searched.primitives & ~comparedByValue;
}

bool ArrayFormChooser::canConcatenateInline() const
{
    if (numberOfArguments() > maxInlineConcatOperands)
        return false;
    for (unsigned index = 0; index < numberOfArguments(); ++index) {
        if (!isValue(argument(index)) || (m_types.at(argument(index)).hasObjects() && !isArray(argument(index))))
            return false;
    }
    return true;
}

bool ArrayFormChooser::canSpliceInline() const
{
    if (numberOfArguments() < numberOfRangeArgumentsOfSplice || !areNumbers(numberOfRangeArgumentsOfSplice))
        return false;
    return numberOfArguments() - numberOfRangeArgumentsOfSplice <= maxInlineSpliceInsertions && areValuesFrom(numberOfRangeArgumentsOfSplice);
}

bool ArrayFormChooser::canCopySplicedInline() const
{
    if (!areNumbers(std::min(numberOfArguments(), numberOfRangeArgumentsOfSplice)))
        return false;
    return numberOfArguments() <= numberOfRangeArgumentsOfSplice + maxInlineSpliceInsertions && areValuesFrom(numberOfRangeArgumentsOfSplice);
}

InlineForm ArrayFormChooser::formOfArrayBuiltin() const
{
    if (!passesArgumentsOneByOne() || !isArray(receiver()))
        return InlineForm::Call;

    switch (m_query.builtin) {
    case InlineBuiltin::ArrayAt:
        return twoWayIf(isNumber(argument(0)));
    case InlineBuiltin::ArrayPop:
        return twoWayIf(true);
    case InlineBuiltin::ArrayIterator:
        return InlineForm::Whole;
    case InlineBuiltin::ArrayIndexOf:
    case InlineBuiltin::ArrayIncludes:
        if (searchesForString())
            return InlineForm::Whole;
        return twoWayIf(searchesForStringFromStart() || canSearchInline());
    case InlineBuiltin::ArrayJoin:
        return twoWayIf(canJoinInline());
    case InlineBuiltin::ArrayConcat:
        return twoWayIf(canConcatenateInline());
    case InlineBuiltin::ArrayShift:
        return twoWayIf(true);
    case InlineBuiltin::ArraySlice:
        return twoWayIf(canSliceInline());
    case InlineBuiltin::ArraySplice:
        return twoWayIf(canSpliceInline());
    case InlineBuiltin::ArrayWith:
        return twoWayIf(numberOfArguments() == 2 && isNumber(argument(0)) && isValue(argument(1)));
    case InlineBuiltin::ArrayToReversed:
        return twoWayIf(!numberOfArguments());
    case InlineBuiltin::ArrayToSpliced:
        return twoWayIf(canCopySplicedInline());
    case InlineBuiltin::ArrayUnshift:
        return twoWayIf(numberOfArguments() == 1 && isValue(argument(0)));
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_STORAGE_BUILTIN(AOT_CASE_IN_FORMS_OF_ARRAYS)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_BUFFER_VIEW_BUILTIN(AOT_CASE_IN_FORMS_OF_ARRAYS)
    FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_FORMS_OF_ARRAYS(AOT_CASE_IN_FORMS_OF_ARRAYS)
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ArrayFormChooser::isPristineArrayWithStorageForm(TypeID) const
{
    return false;
}

InlineForm ArrayFormChooser::formOfPush() const
{
    if (numberOfArguments() != 1 || !isArray(receiver()) || !isValue(argument(0)))
        return InlineForm::Call;
    return isPristineArrayWithStorageForm(receiver()) ? InlineForm::Whole : twoWayIf(true);
}

InlineForm ArrayFormChooser::formOfIsArray() const
{
    TypeID tested = argument(0);
    if (!isValue(tested))
        return InlineForm::Call;
    return m_types.at(tested).hasObjects() ? twoWayIf(true) : InlineForm::Whole;
}

InlineForm ArrayFormChooser::formOfArrayStorageBuiltin() const
{
    if (!passesArgumentsOneByOne())
        return InlineForm::Call;

    switch (m_query.builtin) {
    case InlineBuiltin::ArrayPush:
        return formOfPush();
    case InlineBuiltin::ArrayIsArray:
        return formOfIsArray();
    case InlineBuiltin::ArrayLength:
        return wholeIf(isArray(receiver()));
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_BUILTIN(AOT_CASE_IN_FORMS_OF_ARRAYS)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_BUFFER_VIEW_BUILTIN(AOT_CASE_IN_FORMS_OF_ARRAYS)
    FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_FORMS_OF_ARRAYS(AOT_CASE_IN_FORMS_OF_ARRAYS)
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ArrayFormChooser::isInstanceOfReceiverClass(TypeID view) const
{
    BuiltinClassID receiverClass = table().receiverClassOf(m_query.overload.builtin);
    if (!receiverClass || !view)
        return false;
    Type type = m_types.at(view);
    if (!type.isObject())
        return false;
    if (!type.hasOnlyClassObjects())
        return isInstanceOf(view, receiverClass);
    return std::ranges::all_of(classPartsOf(m_types, type), [&](const ClassPart& part) {
        return table().isSameOrSubclassOf(part.builtinClass, receiverClass);
    });
}

TypedArrayType ArrayFormChooser::elementTypeOfView(TypeID view) const
{
    if (!view || !m_types.at(view).isObject())
        return NotTypedArray;
    TypedArrayType elements = typedArrayTypeOf(builtinTypeContextOf(m_query.context), view);
    return isTypedView(elements) ? elements : NotTypedArray;
}

bool ArrayFormChooser::constructsViewOfOneElementType() const
{
    BuiltinClassID constructed = table().at(m_query.overload.builtin).instanceClass;
    return constructed && isTypedView(table().typedArrayTypeOf(constructed)) && table().typedArrayClassOf(constructed) == constructed;
}

bool ArrayFormChooser::movesElementsOfOneType() const
{
    if (!numberOfArguments() || numberOfArguments() > 2)
        return false;
    TypedArrayType elements = elementTypeOfView(receiver());
    if (elements == NotTypedArray || elementTypeOfView(argument(0)) != elements)
        return false;
    return numberOfArguments() == 1 || isNumber(argument(1));
}

InlineForm ArrayFormChooser::formOfArrayBufferViewBuiltin() const
{
    if (!passesArgumentsOneByOne())
        return InlineForm::Call;

    switch (m_query.builtin) {
    case InlineBuiltin::TypedArrayLength:
    case InlineBuiltin::TypedArrayByteOffset:
        return twoWayIf(isInstanceOfReceiverClass(receiver()));
    case InlineBuiltin::TypedArrayByteLength:
        return twoWayIf(elementTypeOfView(receiver()) != NotTypedArray);
    case InlineBuiltin::TypedArrayWithLength:
        return wholeIf(constructsViewOfOneElementType() && isNumber(argument(0)));
    case InlineBuiltin::TypedArraySet:
        return twoWayIf(movesElementsOfOneType());
    case InlineBuiltin::ByteLiteral:
        return InlineForm::Call;
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_BUILTIN(AOT_CASE_IN_FORMS_OF_ARRAYS)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_STORAGE_BUILTIN(AOT_CASE_IN_FORMS_OF_ARRAYS)
    FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_FORMS_OF_ARRAYS(AOT_CASE_IN_FORMS_OF_ARRAYS)
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

InlineForm chooseFormOfArrayBuiltin(const InlineBuiltinQuery& query)
{
    return ArrayFormChooser(query).formOfArrayBuiltin();
}

InlineForm chooseFormOfArrayStorageBuiltin(const InlineBuiltinQuery& query)
{
    return ArrayFormChooser(query).formOfArrayStorageBuiltin();
}

InlineForm chooseFormOfArrayBufferViewBuiltin(const InlineBuiltinQuery& query)
{
    return ArrayFormChooser(query).formOfArrayBufferViewBuiltin();
}

#undef FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_FORMS_OF_ARRAYS
#undef AOT_CASE_IN_FORMS_OF_ARRAYS

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
