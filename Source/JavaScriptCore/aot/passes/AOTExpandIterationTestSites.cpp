/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestSite.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsCheck.h"
#include "AOTEffectsDeclared.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class KeyUseKind : uint8_t { Read, PresenceTest };

struct KeyedOperation {
    KeyUseKind kind;
    ValueID object;
    ValueID key;
};

struct IterationOwnPropertyTest {
    BuiltinOperand object;
    BuiltinOperand key;
};

} // anonymous namespace

static bool isResultRead(const Value& value, ForInStepResult result)
{
    return value.opcode == Opcode::ResultAt && value.index() == static_cast<uint32_t>(result);
}

static std::optional<IterationOwnPropertyTest> ownPropertyTestOf(BuiltinID builtin)
{
    if (builtin == Builtins::ObjectHasOwn)
        return IterationOwnPropertyTest { BuiltinOperand::argument(0), BuiltinOperand::argument(1) };
    if (builtin == Builtins::ObjectPrototypeHasOwnProperty)
        return IterationOwnPropertyTest { BuiltinOperand::receiver(), BuiltinOperand::argument(0) };
    return std::nullopt;
}

static std::optional<KeyedOperation> keyedOperationOf(const Program& program, const Value& value)
{
    if (value.opcode == Opcode::ElemGet)
        return KeyedOperation { KeyUseKind::Read, value.objectOperand(), value.propertyOperand() };
    if (value.opcode == Opcode::ElemHas)
        return KeyedOperation { KeyUseKind::PresenceTest, value.objectOperand(), value.propertyOperand() };
    if (value.opcode != Opcode::CallBuiltin)
        return std::nullopt;

    BuiltinCall call = builtinCallOf(program, value);
    if (call.form != BuiltinCallForm::Whole)
        return std::nullopt;
    std::optional<IterationOwnPropertyTest> test = ownPropertyTestOf(program.builtins().at(call.overload).builtin);
    if (!test)
        return std::nullopt;
    ValueID object = call.operandAt(test->object);
    ValueID key = call.operandAt(test->key);
    if (!object || !key)
        return std::nullopt;
    return KeyedOperation { KeyUseKind::PresenceTest, object, key };
}

static ValueID findStepOfKey(const TestSiteReporter& reporter, ValueID key)
{
    const Function& function = reporter.function();
    const Value& origin = function.at(reporter.analyses().identityOf(key));
    if (!isResultRead(origin, ForInStepResult::Key))
        return { };
    ValueID producer = origin.resultsOperand();
    return function.at(producer).opcode == Opcode::ForInStep ? producer : ValueID();
}

static ValueID findPositionRead(const TestSiteReporter& reporter, ValueID step)
{
    for (const Use& use : reporter.analyses().useLists().usesOf(step)) {
        if (use.kind != Use::Kind::Operand || !isResultRead(reporter.function().at(use.user), ForInStepResult::Position))
            continue;
        if (reporter.analyses().dominates(use.user, reporter.point()))
            return use.user;
    }
    return { };
}

static bool canReadByPosition(const TestSiteReporter& reporter, ValueID object)
{
    return !reporter.program().expectedOrTypeOf(reporter.function(), object).hasOnlyLayouts(reporter.program()) && reporter.typeOf(reporter.use()).isAnything();
}

static Opcode opcodeWhenCurrent(KeyUseKind kind)
{
    switch (kind) {
    case KeyUseKind::Read:
        return Opcode::EnumeratorGetByVal;
    case KeyUseKind::PresenceTest:
        return Opcode::Boolean;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void reportEnumeratedKeyTests(TestSiteReporter& reporter)
{
    bool hasSteps = reporter.function().values.containsIf([](const Value& value) {
        return value.opcode == Opcode::ForInStep;
    });
    if (!hasSteps)
        return;

    reporter.forEachCandidate([&](const Value& use) {
        std::optional<KeyedOperation> operation = keyedOperationOf(reporter.program(), use);
        if (!operation)
            return;

        ValueID step = findStepOfKey(reporter, operation->key);
        if (!step || !reporter.typeOf(operation->object).hasObjects())
            return;
        if (operation->kind == KeyUseKind::Read && !canReadByPosition(reporter, operation->object))
            return;

        ValueID position = findPositionRead(reporter, step);
        if (!position)
            return;

        ValueID enumerator = reporter.function().at(step).enumeratorOperand();
        reporter.report({ IRBuilder::make(Opcode::IsEnumeratorCurrent, { enumerator, operation->object, position }) }, declaredBoundOf(opcodeWhenCurrent(operation->kind)));
    });
}

void rewriteTestedEnumeratedKeyUse(TestSiteRewriter& rewriter)
{
    FunctionPassContext& context = rewriter.context();
    const EffectContext& effectContext = context.analyses.effectContext();
    Value& use = rewriter.useValue();
    RELEASE_ASSERT(rewriter.tests().size() == 1 && rewriter.guard());
    const Value& test = rewriter.tests().front();
    KeyedOperation operation = *keyedOperationOf(rewriter.program(), use);

    ReplacedEffects replaced;
    replaced.add(effectContext, rewriter.function(), use);

    use.become(opcodeWhenCurrent(operation.kind));
    switch (operation.kind) {
    case KeyUseKind::Read:
        use.setObjectOperand(operation.object);
        use.setPropertyOperand(operation.key);
        use.setEnumeratorOperand(test.enumeratorOperand());
        use.setPositionOperand(test.positionOperand());
        use.setGuardOperand(rewriter.guard());
        break;
    case KeyUseKind::PresenceTest:
        use.setBoolean(true);
        break;
    }

    checkReplacement(effectContext, rewriter.function(), ReplacementSite { context.id, rewriter.point().block, rewriter.use() }, use, replaced, context.build.diagnostics());
    context.build.statistics().count(StatisticID::EnumeratedKeyUsesResolved);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
