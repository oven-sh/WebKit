/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTState.h"
#include "AOTWatchpoints.h"
#include "DecimalConstructor.h"
#include "DecimalObject.h"
#include "DecimalPrototype.h"
#include "DecimalStringCache.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

static void forgetDecimalClass(DecimalWords& words)
{
    words.structureID = 0;
    words.constructor = DecimalWords::noValue;
    std::ranges::fill(words.methods, DecimalWords::noValue);
}

void prepareDecimalWords(Instance& instance)
{
    RELEASE_ASSERT(Decimal128::hasLayoutOfGeneratedCode());
    VM& vm = *instance.vm;
    JSGlobalObject* realm = instance.globalObject;
    DecimalWords& words = instance.decimals;
    instance.decimalTables.tables = &Decimal128::tables();
    instance.state->caches->addClearedEachCollection("Object.prototype without roundingMode"_s, asMutableByteSpan(words.structureIDOfObjectPrototypeWithoutRoundingMode));

    Structure* structure = realm->decimalStructure();
    JSObject* prototype = structure->storedPrototypeObject();
    // All are made before the first is watched: each one changes the structure of the prototype.
    auto names = WTF::map(namesOfDecimalOperations, [&](ASCIILiteral name) {
        Identifier identifier = Identifier::fromString(vm, name);
        PropertySlot slot(prototype, PropertySlot::InternalMethodType::VMInquiry, &vm);
        prototype->methodTable()->getOwnPropertySlot(prototype, realm, identifier, slot);
        return identifier;
    });
    words.structureID = structure->id().bits();
    words.constructor = JSValue::encode(realm->decimalConstructor());
    for (unsigned index = 0; index < numberOfDecimalOperations; ++index) {
        auto isAsMade = [&](JSValue held) {
            auto* function = dynamicDowncast<JSFunction>(held);
            words.methods[index] = JSValue::encode(held);
            return function && function->isHostFunction() && function->realm() == realm && decimalOperationOf(function->nativeFunction()) == static_cast<DecimalOperation>(index);
        };
        auto watch = PropertyValueWatchpoint::tryWatch(vm, prototype, names[index].impl(), isAsMade, [](PropertyValueWatchpoint& fired) {
            forgetDecimalClass(fired.owner<DecimalWords>());
        }, &words);
        if (!watch) {
            forgetDecimalClass(words);
            return;
        }
        instance.state->watchesOfDecimalMethods.append(WTF::move(watch));
    }
}

static ALWAYS_INLINE UGPRPair pairOfDecimal(Decimal128 value)
{
    auto [low, upper] = value.words();
    return makeUGPRPair(low, upper);
}

#define AOT_DEFINE_BINARY_DECIMAL_OPERATION(name, function) \
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDecimal##name, UGPRPair, (uint64_t low, uint64_t upper, uint64_t otherLow, uint64_t otherUpper)) \
{ \
    return pairOfDecimal(Decimal128::function(Decimal128::fromWords(low, upper), Decimal128::fromWords(otherLow, otherUpper))); \
}
AOT_DEFINE_BINARY_DECIMAL_OPERATION(Add, add)
AOT_DEFINE_BINARY_DECIMAL_OPERATION(Subtract, subtract)
AOT_DEFINE_BINARY_DECIMAL_OPERATION(Multiply, multiply)
AOT_DEFINE_BINARY_DECIMAL_OPERATION(Divide, divide)
AOT_DEFINE_BINARY_DECIMAL_OPERATION(Remainder, remainder)
#undef AOT_DEFINE_BINARY_DECIMAL_OPERATION

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDecimalMantissa, UGPRPair, (uint64_t low, uint64_t upper))
{
    return pairOfDecimal(Decimal128::fromWords(low, upper).mantissa());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDecimalRound, UGPRPair, (uint64_t low, uint64_t upper, EncodedJSValue digits, uint32_t mode))
{
    return pairOfDecimal(roundOrNoValue(Decimal128::fromWords(low, upper), JSValue::decode(digits), static_cast<Decimal128::RoundingMode>(mode)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDecimalDivideAndRound, UGPRPair, (uint64_t low, uint64_t upper, uint64_t otherLow, uint64_t otherUpper, EncodedJSValue digits, uint32_t mode))
{
    auto dividend = Decimal128::fromWords(low, upper);
    auto divisor = Decimal128::fromWords(otherLow, otherUpper);
    if (JSValue::decode(digits).isInt32()) [[likely]]
        return pairOfDecimal(Decimal128::divideAndRound(dividend, divisor, JSValue::decode(digits).asInt32(), static_cast<Decimal128::RoundingMode>(mode)));
    return pairOfDecimal(roundOrNoValue(Decimal128::divide(dividend, divisor), JSValue::decode(digits), static_cast<Decimal128::RoundingMode>(mode)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDecimalScale10, UGPRPair, (uint64_t low, uint64_t upper, EncodedJSValue power))
{
    return pairOfDecimal(scale10OrNoValue(Decimal128::fromWords(low, upper), JSValue::decode(power)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDecimalCompare, int64_t, (uint64_t low, uint64_t upper, uint64_t otherLow, uint64_t otherUpper))
{
    return orderOfDecimals(Decimal128::compare(Decimal128::fromWords(low, upper), Decimal128::fromWords(otherLow, otherUpper)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDecimalExponent, EncodedJSValue, (uint64_t low, uint64_t upper))
{
    return JSValue::encode(jsNumber(Decimal128::fromWords(low, upper).exponent()));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDecimalOfInteger, UGPRPair, (int64_t integer))
{
    return pairOfDecimal(Decimal128::fromInteger(integer));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDecimalOfPrimitive, UGPRPair, (EncodedJSValue value))
{
    return pairOfDecimal(toDecimal128OrNoValue(JSValue::decode(value)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDecimalOfRopeOrBigInt, UGPRPair, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDecimalOfRopeOrBigInt);
    OPERATION_RETURN(scope, pairOfDecimal(tryToDecimal128(globalObject, JSValue::decode(value)).value_or(Decimal128::noValue())));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewDecimal, JSCell*, (Instance* instance, uint64_t low, uint64_t upper))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewDecimal);
    Structure* structure = globalObject->decimalStructure();
    prepareInlineAllocation<DecimalObject>(instance, InlineAllocation::Decimal, structure);
    OPERATION_RETURN(scope, DecimalObject::create(vm, structure, Decimal128::fromWords(low, upper)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDecimalToString, JSCell*, (Instance* instance, uint64_t low, uint64_t upper))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDecimalToString);
    OPERATION_RETURN(scope, vm.decimalStringCache().toString(vm, Decimal128::fromWords(low, upper)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDecimalToExponential, JSCell*, (Instance* instance, uint64_t low, uint64_t upper))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDecimalToExponential);
    OPERATION_RETURN(scope, vm.decimalStringCache().toExponential(vm, Decimal128::fromWords(low, upper)));
}

static bool objectPrototypeLacksRoundingMode(VM& vm, Instance& instance)
{
    Structure* structure = instance.globalObject->objectPrototype()->structure();
    uint32_t& known = instance.decimals.structureIDOfObjectPrototypeWithoutRoundingMode;
    if (known == structure->id().bits()) [[likely]]
        return true;
    if (isValidOffset(structure->get(vm, vm.propertyNames->roundingMode)))
        return false;
    if (!structure->isDictionary())
        known = structure->id().bits();
    return true;
}

// The mode is empty if the options are a literal that does not name it. Null where the method has more to do.
JSC_DEFINE_JIT_OPERATION(operationAOTDecimalToFixed, JSCell*, (Instance* instance, uint64_t low, uint64_t upper, EncodedJSValue digits, EncodedJSValue encodedMode))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDecimalToFixed);
    JSValue mode = JSValue::decode(encodedMode);
    if (!mode) {
        if (!objectPrototypeLacksRoundingMode(vm, *instance)) [[unlikely]]
            OPERATION_RETURN(scope, nullptr);
        mode = jsUndefined();
    }
    OPERATION_RETURN(scope, tryDecimalToFixed(globalObject, Decimal128::fromWords(low, upper), JSValue::decode(digits), mode));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
