/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/Decimal128.h>
#include <JavaScriptCore/NativeFunction.h>
#include <JavaScriptCore/JSObject.h>

namespace JSC {

namespace DOMJIT {
class GetterSetter;
class Signature;
}

// An object with the [[DecimalData]] internal slot of https://tc39.es/proposal-decimal/.
class DecimalObject final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;

    static constexpr uint8_t numberOfLowerTierPreciseCells = 0;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.decimalObjectSpace<mode>();
    }

    static DecimalObject* create(VM& vm, Structure* structure, const Decimal128& value)
    {
        auto* object = new (NotNull, allocateCell<DecimalObject>(vm)) DecimalObject(vm, structure, value);
        object->finishCreation(vm);
        return object;
    }
    static constexpr ptrdiff_t offsetOfValue() { return OBJECT_OFFSETOF(DecimalObject, m_value); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    DECLARE_EXPORT_INFO;

    // Null if the value is something else. Cheap for primitives.
    static DecimalObject* tryCast(JSValue value) { return value.isObject() ? dynamicDowncast<DecimalObject>(asObject(value)) : nullptr; }

    const Decimal128& value() const { return m_value; }

#if ENABLE(JIT)
    // What optimized code has in place of a call of isNaN(), isFinite() and exponent().
    static const DOMJIT::GetterSetter* inlinedMethod(TaggedNativeFunction);
    // How it calls the others with the arguments of the signature, so toString() and toExponential() without options and round() with
    // an integer and no mode. Null for the rest.
    static const DOMJIT::Signature* directMethod(TaggedNativeFunction);
#endif

    // What Number(decimal) is.
    double toNumberValue() const;
    // What BigInt(decimal) is. Throws a RangeError if the value is not an integer.
    JSValue toBigIntValue(JSGlobalObject*) const;

    // For structured clone. The text is exact. The latter answers null if the text is not that of a Decimal, or useDecimal is off.
    JS_EXPORT_PRIVATE String textForClone() const;
    JS_EXPORT_PRIVATE static DecimalObject* tryCreateFromCloneText(JSGlobalObject*, StringView);

private:
    DecimalObject(VM& vm, Structure* structure, const Decimal128& value)
        : Base(vm, structure)
        , m_value(value)
    {
    }

    DECLARE_DEFAULT_FINISH_CREATION;

    Decimal128 m_value;
};

} // namespace JSC
