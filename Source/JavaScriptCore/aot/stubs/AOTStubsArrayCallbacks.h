/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"
#include "AOTStubsCompiledMethods.h"

namespace JSC { namespace AOT {

enum class CallbackSource : uint8_t { Element, Field };

enum class ElementTest : uint8_t { IsTrue, Equals, IsLess, IsLessOrEqual, StartsWith, HasType };
constexpr unsigned numberOfElementTests = static_cast<unsigned>(ElementTest::HasType) + 1;

struct ElementTypes {
    static constexpr unsigned bitOfObject = 15;
    static constexpr unsigned shiftOfEncoded = 16;
    static constexpr uint32_t object = 1u << bitOfObject;
    static constexpr uint32_t number = 1u << 31;
    static constexpr uint32_t cell(JSType type) { return 1u << type; }
    static constexpr uint32_t encoded(uint32_t primitive) { return 1u << (shiftOfEncoded + primitive); }
};
static_assert(SymbolType < ElementTypes::bitOfObject && HeapBigIntType < ElementTypes::bitOfObject && StringType < ElementTypes::bitOfObject && ElementTypes::bitOfObject < ObjectType);
static_assert(ElementTypes::shiftOfEncoded + static_cast<uint32_t>(JSValue::ValueUndefined) < 31 && ElementTypes::shiftOfEncoded + static_cast<uint32_t>(JSValue::ValueTrue) < 31);

enum class NumberFold : uint8_t { Sum, Greatest, Least };

enum class FieldMap : uint8_t { Field, LengthOfElement, LengthOfField, FieldOfField };

enum class NumberMap : uint8_t { Sum, Product, Quotient };
constexpr unsigned numberOfNumberMaps = static_cast<unsigned>(NumberMap::Quotient) + 1;

enum class FieldPairOperation : uint8_t { Sum, Difference, Product, FieldOfField };

enum class FieldPairTest : uint8_t { IsLess, IsLessOrEqual, Equals };

struct ArrayCallbackFlags {
    static constexpr uint32_t testIsNegated = 1;
    static constexpr uint32_t callbackIsLent = 2;
    static constexpr uint32_t hasNoInitialValue = 4;
    static constexpr uint32_t onlyTries = 8;
    static constexpr unsigned methodShift = 4;
    static constexpr uint32_t encode(CompiledMethodOfArray method, uint32_t flags) { return static_cast<uint32_t>(method) << methodShift | flags; }
};

constexpr StubKey searchArray(CallbackSource source, ElementTest test) { return StubKey(StubFamily::SearchArray).withForm(numberOfElementTests * static_cast<uint8_t>(source) + static_cast<uint8_t>(test)); }
constexpr StubKey foldArray(CallbackSource source, NumberFold fold) { return StubKey(StubFamily::FoldArray).withForm(3 * static_cast<uint8_t>(source) + static_cast<uint8_t>(fold)); }
constexpr StubKey filterArray(CallbackSource source, ElementTest test) { return StubKey(StubFamily::FilterArray).withForm(numberOfElementTests * static_cast<uint8_t>(source) + static_cast<uint8_t>(test)); }
constexpr StubKey mapArrayToTest(CallbackSource source, ElementTest test) { return StubKey(StubFamily::MapArrayToTest).withForm(numberOfElementTests * static_cast<uint8_t>(source) + static_cast<uint8_t>(test)); }
constexpr StubKey mapArrayToField(FieldMap map) { return StubKey(StubFamily::MapArrayToField).withForm(static_cast<uint8_t>(map)); }
constexpr StubKey foldFieldPairs(FieldPairOperation operation) { return StubKey(StubFamily::FoldFieldPairs).withForm(static_cast<uint8_t>(operation)); }
constexpr StubKey filterFieldPairs(FieldPairTest test) { return StubKey(StubFamily::FilterFieldPairs).withForm(static_cast<uint8_t>(test)); }
constexpr StubKey searchFieldPairs(FieldPairTest test) { return StubKey(StubFamily::SearchFieldPairs).withForm(static_cast<uint8_t>(test)); }
constexpr StubKey mapFieldPairs(FieldPairOperation operation) { return StubKey(StubFamily::MapFieldPairs).withForm(static_cast<uint8_t>(operation)); }
constexpr StubKey mapArrayToNumber(CallbackSource source, NumberMap map) { return StubKey(StubFamily::MapArrayToNumber).withForm(numberOfNumberMaps * static_cast<uint8_t>(source) + static_cast<uint8_t>(map)); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
