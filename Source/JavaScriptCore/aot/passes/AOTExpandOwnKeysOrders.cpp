/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandOwnKeysOrders.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTJSONSupport.h"
#include <algorithm>
#include <limits>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ConstructorOrders {
    Vector<KeyOrderFields, 4> orders;
    bool isComplete { true };
};

} // anonymous namespace

static uint64_t saturateToUInt32(uint64_t count)
{
    return std::min<uint64_t>(count, std::numeric_limits<uint32_t>::max());
}

static Vector<LayoutID, 4> layoutsFromBaseDown(const Program& program, LayoutID id)
{
    Vector<LayoutID, 4> layouts;
    for (LayoutID current = id; current; current = program.at(current).flow.base) {
        RELEASE_ASSERT(layouts.size() < program.layouts.size());
        layouts.append(current);
    }
    layouts.reverse();
    return layouts;
}

static bool declaresFields(const Layout& layout)
{
    return std::ranges::any_of(layout.fields, [](const Field& field) {
        return !field.flags.contains(FieldFlags::IsInherited);
    });
}

static std::optional<KeyOrderFields> findFieldsIn(const Program& program, LayoutID layout, LayoutID declaringLayout, const FieldOrder& order)
{
    KeyOrderFields fields;
    fields.reserveInitialCapacity(order.fields.size());
    for (uint32_t declaredField : order.fields) {
        std::optional<uint32_t> field = program.at(layout).findField(fieldAt(program, { declaringLayout, declaredField }).key);
        if (!field)
            return std::nullopt;
        fields.append(*field);
    }
    return fields;
}

static ConstructorOrders constructorOrdersOf(const Program& program, LayoutID layout, LayoutID declaringLayout)
{
    const Layout& declaring = program.at(declaringLayout);
    ConstructorOrders result;
    for (const FieldOrder& order : declaring.flow.constructorOrders) {
        if (std::optional<KeyOrderFields> fields = findFieldsIn(program, layout, declaringLayout, order))
            result.orders.append(WTF::move(*fields));
        else
            result.isComplete = false;
    }

    // Flow lists orders for a layout that is allocated itself. A base that declares no field adds nothing to the order of a subclass.
    if (declaring.flow.constructorOrders.isEmpty() && declaringLayout != layout && !declaresFields(declaring))
        result.orders.append(KeyOrderFields());
    return result;
}

static KeyOrderFields appendCreatedFields(const KeyOrderFields& order, const KeyOrderFields& createdFields)
{
    KeyOrderFields result = order;
    for (uint32_t field : createdFields) {
        if (!order.contains(field))
            result.append(field);
    }
    return result;
}

bool isEnumerableStringField(const Program& program, const Field& field)
{
    return isJSONSerializedKey(program, field.key) && !field.computedKey && !field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsHidden });
}

ExpectedKeyOrders expectedKeyOrdersOf(const Program& program, LayoutID id, KeyOrderLimits limits)
{
    uint64_t numberOfOrdersLeftOut = 0;
    bool isComplete = true;
    Vector<KeyOrderFields, 4> orders { KeyOrderFields() };
    for (LayoutID declaringLayout : layoutsFromBaseDown(program, id)) {
        ConstructorOrders created = constructorOrdersOf(program, id, declaringLayout);
        isComplete &= created.isComplete;
        numberOfOrdersLeftOut = saturateToUInt32(numberOfOrdersLeftOut) * created.orders.size();

        Vector<KeyOrderFields, 4> extendedOrders;
        for (const KeyOrderFields& order : orders) {
            for (const KeyOrderFields& createdFields : created.orders) {
                KeyOrderFields extended = appendCreatedFields(order, createdFields);
                if (extendedOrders.contains(extended))
                    continue;
                if (extended.size() > limits.maxFields || extendedOrders.size() == limits.maxOrders) {
                    ++numberOfOrdersLeftOut;
                    continue;
                }
                extendedOrders.append(WTF::move(extended));
            }
        }
        orders = WTF::move(extendedOrders);
    }

    // The orders of a base are those of objects allocated as the base, so they are no proof about what its constructor does for a subclass.
    const LayoutFlow& flow = program.at(id).flow;
    bool coversEveryObject = isComplete && !numberOfOrdersLeftOut && !orders.isEmpty()
        && flow.baseState == BaseState::None && flow.fieldCreation == FieldCreation::OnlyAtConstruction;

    ExpectedKeyOrders result;
    result.layout = id;
    result.orders = WTF::move(orders);
    result.coverage = coversEveryObject ? KeyOrderCoverage::EveryObject : KeyOrderCoverage::Unknown;
    result.numberOfOrdersLeftOut = static_cast<uint32_t>(saturateToUInt32(numberOfOrdersLeftOut));
    return result;
}

std::optional<KeyOrderFields> keyOrderAtCreation(const Program& program, const Function& function, FunctionAnalyses& analyses, ValueID object)
{
    const Value& origin = function.at(analyses.sameObjectThroughCasts(object));
    if (origin.opcode != Opcode::NewObject)
        return std::nullopt;

    const Vector<Field>& fields = program.at(origin.layout()).fields;
    bool hasOnlyConstantKeys = std::ranges::all_of(origin.fields(), [&](uint32_t field) {
        return !!fields[field].key;
    });
    if (!hasOnlyConstantKeys)
        return std::nullopt;
    return KeyOrderFields(origin.fields());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
