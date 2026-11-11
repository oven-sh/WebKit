/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCallPlan.h"

#if ENABLE(AOT)

#include "AOTEmitterList.h"
#include "AOTIRProgram.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTTypedArrayKinds.h"
#include "AOTWalkCoroutinePromises.h"
#include "JSBoundFunction.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

#define AOT_DECLARE_EMITTER_OF_INLINE_BUILTIN_FAMILY(family) class family##Emitter;
FOR_EACH_AOT_INLINE_BUILTIN_FAMILY(AOT_DECLARE_EMITTER_OF_INLINE_BUILTIN_FAMILY)
#undef AOT_DECLARE_EMITTER_OF_INLINE_BUILTIN_FAMILY

template<typename Emitter> inline constexpr bool isListedInlineBuiltinEmitter = false;
#define AOT_LIST_INLINE_BUILTIN_EMITTER(Emitter) template<> inline constexpr bool isListedInlineBuiltinEmitter<Emitter> = true;
FOR_EACH_AOT_INLINE_BUILTIN_EMITTER(AOT_LIST_INLINE_BUILTIN_EMITTER)
#undef AOT_LIST_INLINE_BUILTIN_EMITTER

static constexpr bool isFamilyWithInlineBuiltinEmitter(InlineBuiltinFamily family)
{
    switch (family) {
#define AOT_ANSWER_FOR_INLINE_BUILTIN_FAMILY(name) \
    case InlineBuiltinFamily::name: \
        return isListedInlineBuiltinEmitter<name##Emitter>;
    FOR_EACH_AOT_INLINE_BUILTIN_FAMILY(AOT_ANSWER_FOR_INLINE_BUILTIN_FAMILY)
#undef AOT_ANSWER_FOR_INLINE_BUILTIN_FAMILY
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr bool hasGeneratorOfInlineBuiltin(InlineBuiltin builtin)
{
    return isFamilyWithInlineBuiltinEmitter(familyOf(builtin));
}

bool hasEmitter(InlineBuiltin builtin)
{
    return hasGeneratorOfInlineBuiltin(builtin);
}

#define AOT_COUNT_INLINE_BUILTIN_WITH_GENERATOR(name) + hasGeneratorOfInlineBuiltin(InlineBuiltin::name)
static_assert(0 FOR_EACH_AOT_INLINE_BUILTIN_IN_FUNCTION_BUILTIN(AOT_COUNT_INLINE_BUILTIN_WITH_GENERATOR) == 4, "chooseFormOfFunctionBuiltin names each member of the family that has code in line");
static_assert(0 FOR_EACH_AOT_INLINE_BUILTIN_IN_PROMISE_BUILTIN(AOT_COUNT_INLINE_BUILTIN_WITH_GENERATOR) == 3, "chooseFormOfPromiseBuiltin names each member of the family that has code in line");
static_assert(0 FOR_EACH_AOT_INLINE_BUILTIN_IN_DATE_BUILTIN(AOT_COUNT_INLINE_BUILTIN_WITH_GENERATOR) == 6, "chooseFormOfDateBuiltin names each member of the family that has code in line");
static_assert(0 FOR_EACH_AOT_INLINE_BUILTIN_IN_BUFFER_BUILTIN(AOT_COUNT_INLINE_BUILTIN_WITH_GENERATOR) == 7, "chooseFormOfBufferBuiltin names each member of the family that has code in line");
static_assert(0 FOR_EACH_AOT_INLINE_BUILTIN_IN_ERROR_BUILTIN(AOT_COUNT_INLINE_BUILTIN_WITH_GENERATOR) == 1, "chooseFormOfErrorBuiltin names each member of the family that has code in line");
static_assert(0 FOR_EACH_AOT_INLINE_BUILTIN_IN_INTL_BUILTIN(AOT_COUNT_INLINE_BUILTIN_WITH_GENERATOR) == 2, "chooseFormOfIntlBuiltin names each member of the family that has code in line");
#undef AOT_COUNT_INLINE_BUILTIN_WITH_GENERATOR

#define AOT_COUNT_INLINE_BUILTIN(name) + 1
static_assert(0 FOR_EACH_AOT_INLINE_BUILTIN_IN_DATE_BUILTIN(AOT_COUNT_INLINE_BUILTIN) == 6, "chooseFormOfDateBuiltin names each member of the family");
#undef AOT_COUNT_INLINE_BUILTIN

static bool formOfCallMayBecomeInlineBuiltin(BuiltinCallForm form)
{
    switch (form) {
    case BuiltinCallForm::Whole:
        return true;
    case BuiltinCallForm::AnyReceiver:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool passesMoreArgumentsThanInlineBuiltinTakes(const InlineBuiltinQuery& query)
{
    std::optional<uint8_t> maximum = query.overload.code.implementation.maximumArgumentCount;
    return query.form == BuiltinCallForm::Whole && maximum && query.numberOfArguments() > *maximum;
}

static InlineForm chooseFormByFamilyOfInlineBuiltin(const InlineBuiltinQuery& query)
{
    switch (familyOf(query.builtin)) {
#define AOT_CHOOSE_FORM_OF_INLINE_BUILTIN_FAMILY(family) \
    case InlineBuiltinFamily::family: \
        return chooseFormOf##family(query);
    FOR_EACH_AOT_INLINE_BUILTIN_FAMILY(AOT_CHOOSE_FORM_OF_INLINE_BUILTIN_FAMILY)
#undef AOT_CHOOSE_FORM_OF_INLINE_BUILTIN_FAMILY
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool takesOperandsAsWritten(const InlineBuiltinQuery& query)
{
    switch (query.builtin) {
    case InlineBuiltin::ObjectHasOwn: {
        TypeID key = query.argument(0);
        return !query.overload.receiver || (key && !query.context.types.at(key).hasObjects());
    }
    case InlineBuiltin::OwnPropertyKeys:
    case InlineBuiltin::ObjectValues:
    case InlineBuiltin::ObjectEntries:
        return true;
    case InlineBuiltin::ObjectCreate:
        return true;
    case InlineBuiltin::CallNumberConstructor:
        return query.standsInLoop;
    default:
        return false;
    }
}

InlineForm chooseInlineForm(const InlineBuiltinQuery& query)
{
    if (!hasEmitter(query.builtin) || !formOfCallMayBecomeInlineBuiltin(query.form) || passesMoreArgumentsThanInlineBuiltinTakes(query))
        return InlineForm::Call;
    if (query.operandsAreAsWritten && !takesOperandsAsWritten(query))
        return InlineForm::Call;

    InlineForm form = chooseFormByFamilyOfInlineBuiltin(query);
    RELEASE_ASSERT(form != InlineForm::TwoWay || query.hasSlowPath);
    return form;
}

static InlineForm chooseFormOfFunctionBind(const InlineBuiltinQuery& query)
{
    if (!query.hasSlowPath || !query.receiver() || query.numberOfArguments() > 1 + JSBoundFunction::maxEmbeddedArgs)
        return InlineForm::Call;

    TypeArena& types = query.context.types;
    Type target = types.at(query.receiver());
    bool mayBeClosure = target.traitsOfNamed.contains(ObjectTrait::IsClosure) || mayContainAnyClosure(types, target);
    return mayBeClosure ? InlineForm::TwoWay : InlineForm::Call;
}

static bool standsInFunctionBehindCallOfInlineBuiltin(const InlineBuiltinQuery& query)
{
    RuntimeFunctionID slowPath = query.overload.slowPathFunction();
    const RuntimeFunctionEntry* entry = slowPath ? findRuntimeFunction(query.context.program, slowPath) : nullptr;
    return entry && entry->function == query.context.writtenIn;
}

static InlineForm chooseFormOfFunctionApply(const InlineBuiltinQuery& query)
{
    if (query.isDoneInPlace && query.receiver())
        return InlineForm::Whole;
    RELEASE_ASSERT(!standsInFunctionBehindCallOfInlineBuiltin(query));
    return InlineForm::Call;
}

static bool everyArgumentButThisMayBeObjectForReflect(const InlineBuiltinQuery& query, std::optional<unsigned> indexOfThis)
{
    for (unsigned index = 0; index < query.numberOfArguments(); ++index) {
        TypeID argument = query.argument(index);
        if (index != indexOfThis && (!argument || !query.context.types.at(argument).hasObjects()))
            return false;
    }
    return true;
}

static InlineForm chooseFormOfReflectApply(const InlineBuiltinQuery& query)
{
    constexpr unsigned indexOfThisArgument = 1;
    bool takesOperands = query.numberOfArguments() == 3 && everyArgumentButThisMayBeObjectForReflect(query, indexOfThisArgument);
    return query.hasSlowPath && takesOperands ? InlineForm::TwoWay : InlineForm::Call;
}

static InlineForm chooseFormOfReflectConstruct(const InlineBuiltinQuery& query)
{
    bool takesOperands = (query.numberOfArguments() == 2 || query.numberOfArguments() == 3) && everyArgumentButThisMayBeObjectForReflect(query, std::nullopt);
    return query.hasSlowPath && takesOperands ? InlineForm::TwoWay : InlineForm::Call;
}

InlineForm chooseFormOfFunctionBuiltin(const InlineBuiltinQuery& query)
{
    RELEASE_ASSERT(familyOf(query.builtin) == InlineBuiltinFamily::FunctionBuiltin);
    if (query.builtin == InlineBuiltin::FunctionBind)
        return chooseFormOfFunctionBind(query);
    if (query.builtin == InlineBuiltin::FunctionApply)
        return chooseFormOfFunctionApply(query);
    if (query.builtin == InlineBuiltin::ReflectApply)
        return chooseFormOfReflectApply(query);
    if (query.builtin == InlineBuiltin::ReflectConstruct)
        return chooseFormOfReflectConstruct(query);
    return InlineForm::Call;
}

static InlineForm chooseFormOfPromiseResolve(const InlineBuiltinQuery& query)
{
    if (!query.numberOfArguments() || holdsOnlyPrimitives(query.argument(0)))
        return InlineForm::Whole;
    return query.hasSlowPath ? InlineForm::TwoWay : InlineForm::Call;
}

InlineForm chooseFormOfPromiseBuiltin(const InlineBuiltinQuery& query)
{
    RELEASE_ASSERT(familyOf(query.builtin) == InlineBuiltinFamily::PromiseBuiltin);
    if (query.builtin == InlineBuiltin::PromiseResolve)
        return chooseFormOfPromiseResolve(query);
    if (query.builtin == InlineBuiltin::PromiseThen || query.builtin == InlineBuiltin::PromiseCatch)
        return InlineForm::Whole;
    return InlineForm::Call;
}

InlineForm chooseFormOfDateBuiltin(const InlineBuiltinQuery& query)
{
    RELEASE_ASSERT(familyOf(query.builtin) == InlineBuiltinFamily::DateBuiltin);
    switch (query.builtin) {
    case InlineBuiltin::DateNow:
    case InlineBuiltin::DateGetTime:
    case InlineBuiltin::DateGetMilliseconds:
    case InlineBuiltin::DateSetTime:
        return InlineForm::Whole;
    case InlineBuiltin::DateGetField:
    case InlineBuiltin::DateToISOString:
        return query.hasSlowPath ? InlineForm::TwoWay : InlineForm::Call;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool hostReadsMembersFirstIn(const BuiltinTable& table, const BuiltinOverload& overload)
{
    return table.canonicalID(overload.builtin) == Builtins::DatePrototypeToJSON;
}

template<typename Parameter>
static Parameter parameterOfBufferBuiltin(const InlineBuiltinQuery& query, unsigned index)
{
    const BuiltinImplementation& implementation = query.overload.code.implementation;
    RELEASE_ASSERT(implementation.inlineBuiltin == query.builtin && index < numberOfParametersOf(query.builtin));
    return static_cast<Parameter>(implementation.inlineParameters[index]);
}

static bool isNumberForBufferBuiltin(const InlineBuiltinQuery& query, TypeID type)
{
    return type && query.context.types.at(type).isNumber();
}

static bool hasOnlyNumbersBehindForBufferBuiltin(const InlineBuiltinQuery& query, unsigned firstIndex)
{
    for (unsigned index = firstIndex; index < query.numberOfArguments(); ++index) {
        if (!isNumberForBufferBuiltin(query, query.argument(index)))
            return false;
    }
    return true;
}

static bool isInstanceForBufferBuiltin(const InlineBuiltinQuery& query, TypeID type, BuiltinClassID builtinClass)
{
    TypeArena& types = query.context.types;
    return type && !isNeverType(type) && types.isSubset(type, types.builtinClass(builtinClass));
}

static TypedArrayType elementsOfViewForBufferBuiltin(const InlineBuiltinQuery& query, TypeID view)
{
    if (!view || !query.context.types.at(view).isObject())
        return NotTypedArray;
    TypedArrayType elements = typedArrayTypeOf(builtinTypeContextOf(query.context), view);
    return isTypedView(elements) ? elements : NotTypedArray;
}

static bool storesElementInLineForBufferBuiltin(const InlineBuiltinQuery& query, TypeID stored, TypedArrayType elements)
{
    if (!stored)
        return false;
    Type type = query.context.types.at(stored);
    if (isBigInt(elements))
        return !type.isNever() && type.hasOnlyPrimitives(Primitive::BigInt);
    return type.isNumber();
}

static bool accessesDataViewInLine(const InlineBuiltinQuery& query)
{
    return parameterOfBufferBuiltin<TypedArrayType>(query, 0) != TypeFloat16
        && isInstanceForBufferBuiltin(query, query.receiver(), BuiltinClasses::DataView)
        && isNumberForBufferBuiltin(query, query.argument(0));
}

static bool accessesElementAtomicallyInLine(const InlineBuiltinQuery& query)
{
    if (query.numberOfArguments() != 2 + numberOfValuesTakenBy(parameterOfBufferBuiltin<AtomicsOperation>(query, 0)))
        return false;
    return hasIntegerElements(elementsOfViewForBufferBuiltin(query, query.argument(0))) && hasOnlyNumbersBehindForBufferBuiltin(query, 1);
}

static bool accessesBufferInLine(const InlineBuiltinQuery& query)
{
    std::optional<TypedArrayType> elements = elementsMovedBy(parameterOfBufferBuiltin<unsigned>(query, 1), parameterOfBufferBuiltin<NumberKind>(query, 2));
    if (!elements || elementsOfViewForBufferBuiltin(query, query.receiver()) == NotTypedArray)
        return false;
    unsigned numberOfStoredValues = parameterOfBufferBuiltin<AccessDirection>(query, 0) == AccessDirection::Write;
    if (numberOfStoredValues && !storesElementInLineForBufferBuiltin(query, query.argument(0), *elements))
        return false;
    return hasOnlyNumbersBehindForBufferBuiltin(query, numberOfStoredValues);
}

static bool copiesBufferInLine(const InlineBuiltinQuery& query)
{
    constexpr unsigned mostArgumentsOfCopy = 4;
    if (!query.numberOfArguments() || query.numberOfArguments() > mostArgumentsOfCopy)
        return false;
    if (elementsOfViewForBufferBuiltin(query, query.receiver()) == NotTypedArray || elementsOfViewForBufferBuiltin(query, query.argument(0)) == NotTypedArray)
        return false;
    return hasOnlyNumbersBehindForBufferBuiltin(query, 1);
}

InlineForm chooseFormOfBufferBuiltin(const InlineBuiltinQuery& query)
{
    RELEASE_ASSERT(familyOf(query.builtin) == InlineBuiltinFamily::BufferBuiltin);
    if (query.form != BuiltinCallForm::Whole)
        return InlineForm::Call;

    auto twoWayIf = [&](bool takesOperands) {
        return takesOperands && query.hasSlowPath ? InlineForm::TwoWay : InlineForm::Call;
    };
    switch (query.builtin) {
    case InlineBuiltin::DataViewByteLength:
        return twoWayIf(isInstanceForBufferBuiltin(query, query.receiver(), BuiltinClasses::DataView));
    case InlineBuiltin::ArrayBufferByteLength:
        return isInstanceForBufferBuiltin(query, query.receiver(), BuiltinClasses::ArrayBuffer) ? InlineForm::Whole : InlineForm::Call;
    case InlineBuiltin::DataViewGet:
        return twoWayIf(accessesDataViewInLine(query));
    case InlineBuiltin::DataViewSet:
        return twoWayIf(accessesDataViewInLine(query) && storesElementInLineForBufferBuiltin(query, query.argument(1), parameterOfBufferBuiltin<TypedArrayType>(query, 0)));
    case InlineBuiltin::Atomics:
        return twoWayIf(accessesElementAtomicallyInLine(query));
    case InlineBuiltin::BufferAccess:
        return twoWayIf(accessesBufferInLine(query));
    case InlineBuiltin::BufferCopy:
        return twoWayIf(copiesBufferInLine(query));
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

InlineForm chooseFormOfErrorBuiltin(const InlineBuiltinQuery& query)
{
    RELEASE_ASSERT(query.builtin == InlineBuiltin::NewError);
    constexpr unsigned mostArgumentsOfNewError = 2;
    if (query.form != BuiltinCallForm::Whole || query.isHiddenInStackTraces || query.numberOfArguments() > mostArgumentsOfNewError)
        return InlineForm::Call;
    const BuiltinImplementation& implementation = query.overload.code.implementation;
    RELEASE_ASSERT(implementation.inlineBuiltin == query.builtin);
    return static_cast<ErrorKind>(implementation.inlineParameters[0]) == ErrorKind::AggregateError ? InlineForm::Call : InlineForm::Whole;
}

InlineForm chooseFormOfIntlBuiltin(const InlineBuiltinQuery& query)
{
    RELEASE_ASSERT(familyOf(query.builtin) == InlineBuiltinFamily::IntlBuiltin);
    TypeID formatted = query.argument(0);
    if (query.form != BuiltinCallForm::Whole || !query.hasSlowPath || !query.receiver() || !formatted)
        return InlineForm::Call;

    TypeArena& types = query.context.types;
    bool isNumber = types.at(formatted).isNumber();
    switch (query.builtin) {
    case InlineBuiltin::FormatNumber:
        return isNumber ? InlineForm::TwoWay : InlineForm::Call;
    case InlineBuiltin::FormatDateTime: {
        bool isDate = !isNeverType(formatted) && types.isSubset(formatted, types.builtinClass(BuiltinClasses::Date));
        return isNumber || isDate ? InlineForm::TwoWay : InlineForm::Call;
    }
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
