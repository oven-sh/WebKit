/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCallPlan.h"

#if ENABLE(AOT)

#include "AOTPropertyAccessReceivers.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static std::optional<BuiltinDispatchTarget> dispatchRootOf(const OverloadQuery& query)
{
    const BuiltinTable& table = query.context.program.builtins();
    return table.dispatchOf(table.canonicalID(query.builtin), query.mode);
}

// A node on a rest parameter takes a branch when every argument passed from there on lies in the tested type.
static Vector<BuiltinOperand, 4> positionsTestedBy(const BuiltinDispatchNode& node, const BuiltinOperandTypes& operands)
{
    Vector<BuiltinOperand, 4> positions;
    switch (node.operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
    case BuiltinOperand::Kind::Argument:
        positions.append(node.operand);
        break;
    case BuiltinOperand::Kind::ArgumentsFrom:
        for (unsigned index = node.operand.argumentIndex(); index < operands.arguments.size(); ++index)
            positions.append(BuiltinOperand::argument(index));
        break;
    }
    return positions;
}

// An operand of the tested class that is not proven pristine may as well take a later branch.
static TypeFit provenFitOf(const OverloadQuery& query, const BuiltinDispatchNode& node, BuiltinTypeID tested)
{
    TypeFit fitOfEveryPosition = TypeFit::Inside;
    for (BuiltinOperand position : positionsTestedBy(node, query.proven)) {
        OperandFit fit = fitOf(query.context, query.proven.at(position), tested);
        switch (fit.unprovenPristine ? TypeFit::Unknown : fit.fit) {
        case TypeFit::Outside:
            return TypeFit::Outside;
        case TypeFit::Unknown:
            fitOfEveryPosition = TypeFit::Unknown;
            break;
        case TypeFit::Inside:
            break;
        }
    }
    return fitOfEveryPosition;
}

// A test establishes no proof: a predicted layout counts where the type narrowed so far lists layouts or objects of the layout keep their shape.
static TypeID predictedWithin(const OverloadQuery& query, BuiltinOperand operand, TypeID current)
{
    TypeID predicted = query.predicted.at(operand);
    if (!predicted || predicted == query.proven.at(operand))
        return { };
    const Program& program = query.context.program;
    TypeArena& types = query.context.types;
    const Type& prediction = types.at(predicted);
    const Type& currentType = types.at(current);
    bool canTestLayouts = currentType.layouts || layoutTestKindFor(program, membersOf(program, prediction)) == LayoutTestKind::Type;
    Type testable = canTestLayouts ? withoutClassProofs(prediction) : prediction.withoutProofs();
    TypeID met = types.intern(testable.intersectWith(currentType, types));
    return met == TypeTable::never ? TypeID() : met;
}

namespace {

class OperandNarrowing {
public:
    TypeID typeOf(const BuiltinOperandTypes& proven, BuiltinOperand operand) const
    {
        auto found = std::ranges::find(m_requirements, operand, &OperandRequirement::operand);
        if (found != m_requirements.end() && found->requiredType)
            return found->requiredType;
        return proven.at(operand);
    }

    void set(BuiltinOperand operand, TypeID type) { ensure(operand).requiredType = type; }

    void requirePristine(BuiltinOperand operand, const PristineRequirement& pristine)
    {
        if (!pristine)
            return;
        PristineRequirement& required = ensure(operand).pristine;
        ASSERT(!required || required.builtinClass == pristine.builtinClass);
        required.builtinClass = pristine.builtinClass;
        required.aspects.add(pristine.aspects);
    }

    bool needsTests() const
    {
        return std::ranges::any_of(m_requirements, [](const OperandRequirement& requirement) {
            return !!requirement.requiredType;
        });
    }

    Vector<OperandRequirement, 2> takeRequirements(TypeArena& types)
    {
        for (OperandRequirement& requirement : m_requirements) {
            if (requirement.requiredType)
                requirement.requiredType = types.intern(withoutClassProofs(types.at(requirement.requiredType)));
        }
        std::ranges::sort(m_requirements, precedes, &OperandRequirement::operand);
        return std::exchange(m_requirements, { });
    }

private:
    OperandRequirement& ensure(BuiltinOperand operand)
    {
        auto found = std::ranges::find(m_requirements, operand, &OperandRequirement::operand);
        if (found != m_requirements.end())
            return *found;
        m_requirements.append(OperandRequirement { operand, { }, { } });
        return m_requirements.last();
    }

    Vector<OperandRequirement, 2> m_requirements;
};

struct OperandDecision {
    BuiltinOperand operand;
    TypeFit fit;
    TypeID narrowedType;
    PristineRequirement unprovenPristine;
};

class OverloadWalk {
public:
    explicit OverloadWalk(const OverloadQuery& query)
        : m_query(query)
        , m_table(query.context.program.builtins())
    {
    }

    OverloadMatch run(BuiltinDispatchTarget);

private:
    std::optional<BuiltinDispatchTarget> chooseBranch(const BuiltinDispatchNode&);
    bool canPassBy(const BuiltinDispatchNode&, const BuiltinDispatchBranch&) const;
    void collectOverloadsBehind(BuiltinDispatchTarget, Vector<BuiltinOverloadID, 4>&) const;
    TypeFit decideBranch(std::span<const BuiltinOperand> positions, BuiltinTypeID tested);
    OperandDecision decideOperand(BuiltinOperand, BuiltinTypeID tested) const;
    TypeID chooseNarrowedType(const Type& widest, TypeID predicted, BuiltinTypeID tested, TypeFit decision) const;
    void record(const OperandDecision&);
    BuiltinOverloadID firstReachableOverload(BuiltinDispatchTarget) const;

    const OverloadQuery& m_query;
    const BuiltinTable& m_table;
    OperandNarrowing m_narrowing;
};

OverloadMatch OverloadWalk::run(BuiltinDispatchTarget target)
{
    while (!target.isOverload()) {
        std::optional<BuiltinDispatchTarget> next = chooseBranch(m_table.at(target.node));
        if (!next)
            return { BuiltinMatchState::Unknown, firstReachableOverload(target), { } };
        target = *next;
    }
    BuiltinMatchState state = m_narrowing.needsTests() ? BuiltinMatchState::NeedsTests : BuiltinMatchState::Proven;
    return { state, target.overload, m_narrowing.takeRequirements(m_query.context.types) };
}

std::optional<BuiltinDispatchTarget> OverloadWalk::chooseBranch(const BuiltinDispatchNode& node)
{
    Vector<BuiltinOperand, 4> positions = positionsTestedBy(node, m_query.proven);
    for (const BuiltinDispatchBranch& branch : node.branches) {
        switch (decideBranch(positions.span(), branch.tested)) {
        case TypeFit::Outside:
            continue;
        case TypeFit::Inside:
            return branch.target;
        case TypeFit::Unknown:
            if (canPassBy(node, branch))
                continue;
            return std::nullopt;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void OverloadWalk::collectOverloadsBehind(BuiltinDispatchTarget target, Vector<BuiltinOverloadID, 4>& overloads) const
{
    if (target.isOverload()) {
        if (!overloads.contains(target.overload))
            overloads.append(target.overload);
        return;
    }
    for (const BuiltinDispatchBranch& branch : m_table.at(target.node).branches)
        collectOverloadsBehind(branch.target, overloads);
}

// A special case is a faster form of a later overload, which is right for its operands too. A walk that cannot tell whether an operand is the special one passes the branch by,
// as long as the branch leads to nothing else that the last branch, which takes everything, does not lead to.
bool OverloadWalk::canPassBy(const BuiltinDispatchNode& node, const BuiltinDispatchBranch& branch) const
{
    const BuiltinDispatchBranch& last = node.branches.back();
    if (&branch == &last)
        return false;

    Vector<BuiltinOverloadID, 4> behindLast;
    collectOverloadsBehind(last.target, behindLast);
    Vector<BuiltinOverloadID, 4> behindBranch;
    collectOverloadsBehind(branch.target, behindBranch);
    return std::ranges::all_of(behindBranch, [&](BuiltinOverloadID overload) {
        return m_table.at(overload).flags.contains(BuiltinOverloadFlag::IsSpecialCase) || behindLast.contains(overload);
    });
}

TypeFit OverloadWalk::decideBranch(std::span<const BuiltinOperand> positions, BuiltinTypeID tested)
{
    Vector<OperandDecision, 4> decisions;
    for (BuiltinOperand position : positions)
        decisions.append(decideOperand(position, tested));

    auto excluding = std::ranges::find_if(decisions, [](const OperandDecision& decision) {
        return decision.fit == TypeFit::Outside && !decision.narrowedType;
    });
    if (excluding == decisions.end())
        excluding = std::ranges::find(decisions, TypeFit::Outside, &OperandDecision::fit);
    if (excluding != decisions.end()) {
        record(*excluding);
        return TypeFit::Outside;
    }

    if (std::ranges::contains(decisions, TypeFit::Unknown, &OperandDecision::fit))
        return TypeFit::Unknown;
    for (const OperandDecision& decision : decisions)
        record(decision);
    return TypeFit::Inside;
}

OperandDecision OverloadWalk::decideOperand(BuiltinOperand operand, BuiltinTypeID tested) const
{
    const TypingContext& context = m_query.context;
    TypeArena& types = context.types;
    TypeID current = m_narrowing.typeOf(m_query.proven, operand);
    OperandFit proven = fitOf(context, current, tested);
    if (proven.fit != TypeFit::Unknown)
        return { operand, proven.fit, { }, proven.unprovenPristine };

    OperandDecision undecided { operand, TypeFit::Unknown, { }, { } };
    TypeID predicted = predictedWithin(m_query, operand, current);
    if (!predicted)
        return undecided;

    Type testedType = typeOfBuiltinType({ context, nullptr }, tested);
    switch (fitOf(context, predicted, tested).fit) {
    case TypeFit::Unknown:
        return undecided;
    case TypeFit::Inside: {
        TypeID narrowed = chooseNarrowedType(types.at(current).intersectWith(testedType, types), predicted, tested, TypeFit::Inside);
        return { operand, TypeFit::Inside, narrowed, fitOf(context, narrowed, tested).unprovenPristine };
    }
    case TypeFit::Outside:
        return { operand, TypeFit::Outside, chooseNarrowedType(types.at(current).without(testedType, types), predicted, tested, TypeFit::Outside), { } };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A table type without an exact IR type (a class with subclasses, plain data) leaves only the prediction itself to test.
TypeID OverloadWalk::chooseNarrowedType(const Type& widest, TypeID predicted, BuiltinTypeID tested, TypeFit decision) const
{
    const TypingContext& context = m_query.context;
    if (!context.types.at(predicted).isSubsetOf(widest, context.program))
        return predicted;
    TypeID widestID = context.types.intern(widest);
    return fitOf(context, widestID, tested).fit == decision ? widestID : predicted;
}

void OverloadWalk::record(const OperandDecision& decision)
{
    if (decision.narrowedType)
        m_narrowing.set(decision.operand, decision.narrowedType);
    m_narrowing.requirePristine(decision.operand, decision.unprovenPristine);
}

BuiltinOverloadID OverloadWalk::firstReachableOverload(BuiltinDispatchTarget target) const
{
    while (!target.isOverload()) {
        const BuiltinDispatchNode& node = m_table.at(target.node);
        auto branch = std::ranges::find_if(node.branches, [&](const BuiltinDispatchBranch& candidate) {
            return provenFitOf(m_query, node, candidate.tested) != TypeFit::Outside;
        });
        RELEASE_ASSERT(branch != node.branches.end());
        target = branch->target;
    }
    return target.overload;
}

} // anonymous namespace

OverloadMatch matchOverload(const OverloadQuery& query)
{
    std::optional<BuiltinDispatchTarget> root = dispatchRootOf(query);
    if (!root)
        return { };
    return OverloadWalk(query).run(*root);
}

static void visitReachable(const OverloadQuery& query, BuiltinDispatchTarget target, Vector<BuiltinOverloadID, 4>& seen, const ScopedLambda<void(BuiltinOverloadID)>& functor)
{
    if (target.isOverload()) {
        if (seen.contains(target.overload))
            return;
        seen.append(target.overload);
        functor(target.overload);
        return;
    }

    const BuiltinDispatchNode& node = query.context.program.builtins().at(target.node);
    for (const BuiltinDispatchBranch& branch : node.branches) {
        switch (provenFitOf(query, node, branch.tested)) {
        case TypeFit::Outside:
            continue;
        case TypeFit::Unknown:
            visitReachable(query, branch.target, seen, functor);
            continue;
        case TypeFit::Inside:
            visitReachable(query, branch.target, seen, functor);
            return;
        }
    }
}

void forEachReachableOverload(const OverloadQuery& query, NOESCAPE const ScopedLambda<void(BuiltinOverloadID)>& functor)
{
    std::optional<BuiltinDispatchTarget> root = dispatchRootOf(query);
    if (!root)
        return;
    Vector<BuiltinOverloadID, 4> seen;
    visitReachable(query, *root, seen, functor);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
