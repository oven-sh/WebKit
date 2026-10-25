/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3RangeReductions.h"

#if ENABLE(B3_JIT)

#include "B3IntRange.h"
#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace B3 {

static RangeReduction reduceToConstant(int64_t constant)
{
    return { RangeReduction::Kind::Constant, constant, nullptr };
}

static RangeReduction reduceToValue(Value* value)
{
    return { RangeReduction::Kind::Value, 0, value };
}

enum class ExtensionKind : uint8_t { Sign, Zero, Either };

static std::optional<ExtensionKind> extensionKindOf(const Procedure& proc, Value* value)
{
    if (value->opcode() != SExt32 && value->opcode() != ZExt32)
        return std::nullopt;
    if (rangeFor(proc, value->child(0)).min() >= 0)
        return ExtensionKind::Either;
    return value->opcode() == SExt32 ? ExtensionKind::Sign : ExtensionKind::Zero;
}

static Opcode unsignedComparison(Opcode opcode)
{
    switch (opcode) {
    case LessThan:
        return Below;
    case GreaterThan:
        return Above;
    case LessEqual:
        return BelowEqual;
    case GreaterEqual:
        return AboveEqual;
    default:
        return opcode;
    }
}

// Sign extension keeps the signed and the unsigned order. Zero extension keeps the unsigned order and makes it the signed order of the wide values.
static RangeReduction compareBeforeExtension(const Procedure& proc, Value* comparison)
{
    std::optional<ExtensionKind> left = extensionKindOf(proc, comparison->child(0));
    std::optional<ExtensionKind> right = extensionKindOf(proc, comparison->child(1));
    if (!left || !right)
        return { };
    ExtensionKind kind = *left == ExtensionKind::Either ? *right : *left;
    if (*right != ExtensionKind::Either && *right != kind)
        return { };
    Opcode opcode = kind == ExtensionKind::Zero ? unsignedComparison(comparison->opcode()) : comparison->opcode();
    return { RangeReduction::Kind::Comparison, 0, comparison->child(0)->child(0), comparison->child(1)->child(0), opcode };
}

static Value* truncatedInt64(Value* extension)
{
    Value* truncation = extension->child(0);
    if (truncation->opcode() != Trunc || truncation->child(0)->type() != Int64)
        return nullptr;
    return truncation->child(0);
}

static bool isLowBitMask(int64_t mask)
{
    return mask > 0 && !(static_cast<uint64_t>(mask) & (static_cast<uint64_t>(mask) + 1));
}

RangeReduction findRangeReduction(const Procedure& proc, Value* value)
{
    if (isIntegerComparison(value->opcode())) {
        switch (evaluateComparison(proc, value)) {
        case TriState::True:
            return reduceToConstant(1);
        case TriState::False:
            return reduceToConstant(0);
        case TriState::Indeterminate:
            return compareBeforeExtension(proc, value);
        }
    }

    switch (value->opcode()) {
    case SExt32:
        if (Value* wide = truncatedInt64(value); wide && rangeFor(proc, wide).isSubsetOf(IntRange::top<int32_t>()))
            return reduceToValue(wide);
        if (rangeFor(proc, value->child(0)).min() >= 0)
            return { RangeReduction::Kind::ZeroExtension, 0, value->child(0) };
        return { };
    case ZExt32:
        if (Value* wide = truncatedInt64(value); wide && rangeFor(proc, wide).isSubsetOf(IntRange(0, std::numeric_limits<uint32_t>::max())))
            return reduceToValue(wide);
        return { };
    case BitAnd:
        if (value->child(1)->hasInt() && isLowBitMask(value->child(1)->asInt()) && rangeFor(proc, value->child(0)).isSubsetOf(IntRange(0, value->child(1)->asInt())))
            return reduceToValue(value->child(0));
        return { };
    default:
        return { };
    }
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
