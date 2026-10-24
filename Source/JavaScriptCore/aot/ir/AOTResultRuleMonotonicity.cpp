/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRuleMonotonicity.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIR.h"
#include "AOTResultRulesInternal.h"
#include "AOTVector.h"
#include <algorithm>
#include <ranges>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

static uint64_t shapeOf(const Function& function, const Value& value)
{
    WordHasher hasher;
    hasher.add(static_cast<uint32_t>(value.opcode), value.numberOfOperands());
    hasher.addPayloadOf(value);
    hasher.addForEachOperandOf(value, [&](ValueID operand) {
        return operand ? function.at(operand).type.index() : UINT32_MAX;
    });
    return hasher.hash();
}

static Vector<Type, 32> narrowerTypesOf(const Program& program, const Type& type, TypeArena& types)
{
    Vector<Type, 32> narrower;
    auto add = [&](Type&& candidate) {
        candidate.canonicalize();
        if (candidate == type || !candidate.isSubsetOf(type, program) || narrower.contains(candidate))
            return;
        narrower.append(WTF::move(candidate));
    };

    add(Type::never());
    for (TypeID part : { TypeTable::undefined, TypeTable::null, TypeTable::nullish, TypeTable::boolean, TypeTable::int32, TypeTable::integer, TypeTable::number, TypeTable::string, TypeTable::function, TypeTable::object }) {
        const Type& sample = types.at(part);
        add(type.intersectWith(sample, types));
        if (!sample.hasObjects())
            add(type.withoutPrimitives(sample.primitives));
    }
    add(type.withoutObjects());
    add(type.intersectWith(Type::integer(Range::one(0)), types));
    add(type.intersectWith(Type::builtinClass(BuiltinClasses::Array), types));
    if (type.layouts) {
        auto isAllocated = [&](LayoutID member) {
            return program.at(member).flow.allocation != AllocationState::NeverAllocated;
        };
        std::span<const LayoutID> members = membersOf(program, type);
        auto first = std::ranges::find_if(members, isAllocated);
        auto last = std::ranges::find_if(members | std::views::reverse, isAllocated);
        if (first != members.end()) {
            add(type.withoutObjects().unionWith(Type::forLayout(*first, types), types));
            add(Type::forLayout(*first, types));
            add(Type::forLayout(*last, types));
        }
    } else if (type.anyObject && !program.layouts.isEmpty()) {
        for (size_t index = 0; index < program.layouts.size(); ++index) {
            if (program.layouts[index].isEnvironment() || program.layouts[index].flow.allocation == AllocationState::NeverAllocated)
                continue;
            add(Type::forLayout(LayoutID(index), types));
            add(types.at(TypeTable::undefined).unionWith(Type::forLayout(LayoutID(index), types), types));
            break;
        }
    }
    return narrower;
}

static constexpr OpcodeSet opcodesThatKeepCodeBehindFailedCheck { Opcode::Check, Opcode::Convert, Opcode::NarrowByField };

namespace {

class ResultRuleCheck {
    WTF_MAKE_NONCOPYABLE(ResultRuleCheck);
public:
    ResultRuleCheck(const Program& program, Function& function, TypeArena& types)
        : m_program(program)
        , m_function(function)
        , m_types(types)
    {
    }

    std::optional<BrokenResultRule> check(ValueID);

private:
    struct Results {
        Type byRule;
        Type byTypeIDs;
    };

    bool isInside(const Type& narrow, const Type& wide) const;
    Results resultsWithOperandTypedAs(ValueID, ValueID operand, const Type&);
    std::optional<BrokenResultRule> compare(ValueID, uint32_t operandIndex, const Type& narrow, const Type& wide);

    const Program& m_program;
    Function& m_function;
    TypeArena& m_types;
};

bool ResultRuleCheck::isInside(const Type& narrow, const Type& wide) const
{
    if (narrow.isSubsetOf(wide, m_program))
        return true;
    const BuiltinTable& builtins = m_program.builtins();
    Type withoutExtendingClasses = narrow;
    withoutExtendingClasses.classes.removeAllMatching([&](const ClassPart& part) {
        return !wide.findClassPart(part.builtinClass) && std::ranges::any_of(wide.classes, [&](const ClassPart& extended) {
            return builtins.isSameOrSubclassOf(part.builtinClass, extended.builtinClass);
        });
    });
    return withoutExtendingClasses.classes.size() != narrow.classes.size() && withoutExtendingClasses.isSubsetOf(wide, m_program);
}

auto ResultRuleCheck::resultsWithOperandTypedAs(ValueID id, ValueID operand, const Type& type) -> Results
{
    Value& definition = m_function.edit(operand);
    TypeID storedType = definition.type;
    auto restore = makeScopeExit([&] {
        definition.type = storedType;
    });
    definition.type = m_types.intern(type);

    TypingContext typing { m_program, m_function, m_types };
    const Value& value = m_function.at(id);
    return { m_types.at(m_types.intern(m_types.adopt(computeResultType(typing, value)))), m_types.at(m_types.adopt(computeResultTypeID(typing, value))) };
}

std::optional<BrokenResultRule> ResultRuleCheck::compare(ValueID id, uint32_t operandIndex, const Type& narrow, const Type& wide)
{
    ValueID operand = m_function.at(id).operand(operandIndex);
    Results ofNarrow = resultsWithOperandTypedAs(id, operand, narrow);
    if (ofNarrow.byTypeIDs != ofNarrow.byRule)
        return BrokenResultRule { BrokenResultRule::Law::BothPathsAgree, id, operandIndex, narrow, WTF::move(ofNarrow.byTypeIDs), { }, WTF::move(ofNarrow.byRule) };
    if (opcodesThatKeepCodeBehindFailedCheck.contains(m_function.at(id).opcode))
        return std::nullopt;
    Results ofWide = resultsWithOperandTypedAs(id, operand, wide);
    if (!isInside(ofNarrow.byRule, ofWide.byRule))
        return BrokenResultRule { BrokenResultRule::Law::NarrowerOperandGivesNarrowerResult, id, operandIndex, narrow, WTF::move(ofNarrow.byRule), wide, WTF::move(ofWide.byRule) };
    return std::nullopt;
}

std::optional<BrokenResultRule> ResultRuleCheck::check(ValueID id)
{
    const Value& value = m_function.at(id);
    for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
        ValueID operand = value.operand(operandIndex);
        if (!operand || operand == id)
            continue;
        Type type = m_types.at(m_function.at(operand).type);
        if (!type.isJavaScriptValue())
            continue;

        for (const Type& narrower : narrowerTypesOf(m_program, type, m_types)) {
            if (std::optional<BrokenResultRule> found = compare(id, operandIndex, narrower, type))
                return found;
        }
        Type anyValue = Type::anything().withPrimitives(type.primitives & Primitive::Absent);
        if (value.operandKind(operandIndex) == OperandKind::Any && type != anyValue) {
            if (std::optional<BrokenResultRule> found = compare(id, operandIndex, type, anyValue))
                return found;
        }
    }
    return std::nullopt;
}

} // anonymous namespace

std::optional<BrokenResultRule> findBrokenResultRule(const Program& program, Function& function, TypeArena& types, ShapesWithMonotoneResult& checkedShapes)
{
    ResultRuleCheck check(program, function, types);
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (resultRuleOf(value.opcode) == ResultRule::None || numberOfResultsOf(program, function, value))
                continue;
            if (!checkedShapes.add(shapeOf(function, value)).isNewEntry)
                continue;
            if (std::optional<BrokenResultRule> found = check.check(id))
                return found;
        }
    }
    return std::nullopt;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
