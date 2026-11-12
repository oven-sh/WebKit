/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCallPlan.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

#define FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_OBJECT_FORM_RULES(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_STORAGE_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ARRAY_BUFFER_VIEW_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_PROMISE_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_BUFFER_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_MATH_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_DATE_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_FUNCTION_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_STRING_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_REG_EXP_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_ERROR_BUILTIN(v) \
    FOR_EACH_AOT_INLINE_BUILTIN_IN_INTL_BUILTIN(v)

#define AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES(name) case InlineBuiltin::name:

namespace {

class ObjectFormRules {
public:
    explicit ObjectFormRules(const InlineBuiltinQuery& query)
        : m_query(query)
    {
    }

    InlineForm call() const { return InlineForm::Call; }
    InlineForm twoWay() const { return m_query.hasSlowPath ? InlineForm::TwoWay : InlineForm::Call; }

    InlineForm wholeIfArgumentCountIsAtLeast(unsigned numberOfArguments) const
    {
        return m_query.numberOfArguments() >= numberOfArguments ? InlineForm::Whole : call();
    }

    InlineForm formWhereInlineCodeHandlesEveryObject(TypeID operand) const
    {
        if (!mayBeObject(operand))
            return call();
        return typeOf(operand).isObject() ? InlineForm::Whole : twoWay();
    }

    InlineForm formWhereInlineCodeHandlesSomeObjects(TypeID operand) const
    {
        return mayBeObject(operand) ? twoWay() : call();
    }

    InlineForm forGetPrototypeOf() const
    {
        TypeID operand = m_query.argument(0);
        if (!mayBeObject(operand))
            return call();
        Type type = typeOf(operand);
        bool areObjectsOfProgram = type.isJavaScriptValue() && type.isObject() && type.hasOnlyLayouts() && !type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) && std::ranges::none_of(membersOf(m_query.context.types, type), [&](LayoutID id) {
            return liesBelowBuiltinClass(id) || !isFoundByShapeTest(m_query.context.program.at(id));
        });
        return areObjectsOfProgram ? InlineForm::Whole : twoWay();
    }

    InlineForm forObjectCreate() const
    {
        TypeID prototype = m_query.argument(0);
        if (!prototype || !passesPrototypeAlone())
            return call();
        Type type = typeOf(prototype);
        if (!type.hasObjects() && !(type.primitives & Primitive::Null))
            return call();
        return type.isObject() ? InlineForm::Whole : twoWay();
    }

    InlineForm forSetPrototypeOf() const
    {
        TypeID object = m_query.argument(0);
        if (m_query.numberOfArguments() < 2 || !mayBeObject(object))
            return call();
        bool mayHaveNoOrderWord = std::ranges::any_of(membersOf(m_query.context.types, typeOf(object)), [&](LayoutID id) {
            return liesBelowBuiltinClass(id);
        });
        return mayHaveNoOrderWord ? call() : twoWay();
    }

    InlineForm forObjectIs() const
    {
        if (m_query.numberOfArguments() != 2)
            return call();
        Type left = typeOf(m_query.argument(0));
        Type right = typeOf(m_query.argument(1));
        bool mayBothBeStrings = (left.primitives & Primitive::AnyString) && (right.primitives & Primitive::AnyString);
        bool mayBothBeBigInts = left.primitives & right.primitives & Primitive::BigInt;
        return mayBothBeStrings || mayBothBeBigInts ? twoWay() : InlineForm::Whole;
    }

    InlineForm forStringOfSymbol() const
    {
        if (!m_query.receiver())
            return call();
        Type receiver = typeOf(m_query.receiver());
        bool isSymbol = receiver.isJavaScriptValue() && !receiver.hasObjects() && receiver.primitives && !(receiver.primitives & ~Primitive::AnySymbol);
        return isSymbol ? InlineForm::Whole : call();
    }

    InlineForm forCollectionFromIterable() const
    {
        return m_query.numberOfArguments() == 1 && mayBeListedWithoutIterator(m_query.argument(0)) ? InlineForm::Whole : call();
    }

private:
    Type typeOf(TypeID type) const { return m_query.context.types.at(type); }
    bool mayBeObject(TypeID operandOrNotPassed) const { return operandOrNotPassed && typeOf(operandOrNotPassed).hasObjects(); }
    bool liesBelowBuiltinClass(LayoutID id) const { return !!builtinClassWrittenAbove(m_query.context.types, id); }

    bool passesPrototypeAlone() const
    {
        unsigned numberOfArguments = m_query.numberOfArguments();
        return numberOfArguments == 1 || (numberOfArguments == 2 && hasOnlyPrimitivesIn(m_query.argument(1), Primitive::Undefined));
    }

    bool mayBeListedWithoutIterator(TypeID iterable) const
    {
        TypeArena& types = m_query.context.types;
        Type type = types.at(iterable);
        if (type.anyObject || findClassPart(types, type, BuiltinClasses::Array))
            return true;
        if (m_query.builtin != InlineBuiltin::SetFromIterable)
            return false;
        return type.primitives & Primitive::AnyString || findClassPart(types, type, BuiltinClasses::Arguments);
    }

    const InlineBuiltinQuery& m_query;
};

} // anonymous namespace

InlineForm chooseFormOfOwnPropertyBuiltin(const InlineBuiltinQuery& query)
{
    ObjectFormRules rules(query);
    switch (query.builtin) {
    case InlineBuiltin::OwnPropertyKeys:
        return query.operandsAreAsWritten ? InlineForm::Whole : rules.formWhereInlineCodeHandlesEveryObject(query.argument(0));
    case InlineBuiltin::ObjectGetOwnPropertyDescriptor:
        return query.numberOfArguments() >= 2 ? rules.formWhereInlineCodeHandlesEveryObject(query.argument(0)) : rules.call();
    case InlineBuiltin::ObjectHasOwn:
        return rules.wholeIfArgumentCountIsAtLeast(query.overload.receiver ? 1 : 2);
    case InlineBuiltin::ObjectIsPrototypeOf:
    case InlineBuiltin::ObjectPropertyIsEnumerable:
        return rules.wholeIfArgumentCountIsAtLeast(1);
    case InlineBuiltin::ReflectSet:
        return rules.wholeIfArgumentCountIsAtLeast(3);
    case InlineBuiltin::ObjectValues:
    case InlineBuiltin::ObjectEntries:
        return InlineForm::Whole;
    FOR_EACH_AOT_INLINE_BUILTIN_IN_OBJECT_BUILTIN(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_HASH_TABLE_BUILTIN(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_MAP_SET_BUILTIN(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
    FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_OBJECT_FORM_RULES(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

InlineForm chooseFormOfObjectBuiltin(const InlineBuiltinQuery& query)
{
    ObjectFormRules rules(query);
    switch (query.builtin) {
    case InlineBuiltin::ObjectCreate:
        return rules.forObjectCreate();
    case InlineBuiltin::CallObjectConstructor:
        return rules.formWhereInlineCodeHandlesEveryObject(query.argument(0));
    case InlineBuiltin::GetPrototypeOf:
        return rules.forGetPrototypeOf();
    case InlineBuiltin::ObjectPrototypeToString:
        return rules.formWhereInlineCodeHandlesSomeObjects(query.receiver());
    case InlineBuiltin::SetPrototypeOf:
        return rules.forSetPrototypeOf();
    case InlineBuiltin::ObjectAssign:
        return query.numberOfArguments() >= 2 ? rules.formWhereInlineCodeHandlesSomeObjects(query.argument(0)) : rules.call();
    case InlineBuiltin::ObjectIs:
        return rules.forObjectIs();
    case InlineBuiltin::ReturnThis:
        return InlineForm::Whole;
    case InlineBuiltin::SymbolDescription:
    case InlineBuiltin::SymbolToString:
        return rules.forStringOfSymbol();
    FOR_EACH_AOT_INLINE_BUILTIN_IN_OWN_PROPERTY_BUILTIN(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_HASH_TABLE_BUILTIN(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_MAP_SET_BUILTIN(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
    FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_OBJECT_FORM_RULES(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

InlineForm chooseFormOfHashTableBuiltin(const InlineBuiltinQuery& query)
{
    ObjectFormRules rules(query);
    switch (query.builtin) {
    case InlineBuiltin::NewMap:
    case InlineBuiltin::NewSet:
    case InlineBuiltin::MapSize:
    case InlineBuiltin::SetSize:
        return InlineForm::Whole;
    case InlineBuiltin::MapGet:
    case InlineBuiltin::MapHas:
    case InlineBuiltin::SetHas:
    case InlineBuiltin::SetAdd:
        return rules.wholeIfArgumentCountIsAtLeast(1);
    case InlineBuiltin::MapSet:
        return rules.wholeIfArgumentCountIsAtLeast(2);
    FOR_EACH_AOT_INLINE_BUILTIN_IN_OWN_PROPERTY_BUILTIN(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_OBJECT_BUILTIN(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_MAP_SET_BUILTIN(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
    FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_OBJECT_FORM_RULES(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

InlineForm chooseFormOfMapSetBuiltin(const InlineBuiltinQuery& query)
{
    ObjectFormRules rules(query);
    switch (query.builtin) {
    case InlineBuiltin::SetFromIterable:
    case InlineBuiltin::MapFromEntries:
        return rules.forCollectionFromIterable();
    case InlineBuiltin::WeakMapGet:
    case InlineBuiltin::WeakMapHas:
    case InlineBuiltin::WeakSetHas:
    case InlineBuiltin::MapDelete:
    case InlineBuiltin::SetDelete:
        return rules.wholeIfArgumentCountIsAtLeast(1);
    case InlineBuiltin::MapClear:
    case InlineBuiltin::SetClear:
    case InlineBuiltin::MapIterator:
    case InlineBuiltin::SetIterator:
        return InlineForm::Whole;
    FOR_EACH_AOT_INLINE_BUILTIN_IN_OWN_PROPERTY_BUILTIN(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_OBJECT_BUILTIN(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
    FOR_EACH_AOT_INLINE_BUILTIN_IN_HASH_TABLE_BUILTIN(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
    FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_OBJECT_FORM_RULES(AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES)
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#undef AOT_CASE_OUTSIDE_FAMILY_OF_OBJECT_FORM_RULES
#undef FOR_EACH_AOT_INLINE_BUILTIN_OUTSIDE_OBJECT_FORM_RULES

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
