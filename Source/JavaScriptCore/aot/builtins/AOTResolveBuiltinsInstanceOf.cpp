/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveBuiltinsInternal.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTResultRules.h"
#include "AOTTypeTestSite.h"
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

// The class whose prototype object the test looks for: the constructor finds the ordinary Symbol.hasInstance, and its prototype property holds that object for good.
static BuiltinClassID builtinClassTestedThrough(const BuiltinLookupContext& context, BuiltinID constructor)
{
    const Program& program = context.program;
    const BuiltinTable& table = program.builtins();

    BuiltinMemberAnswer hasInstance = findMemberFrom(context, constructor, program.keyOf(table.nameOf(WellKnownSymbol::HasInstance)));
    if (!hasInstance.isConstant() || hasInstance.member != table.canonicalID(Builtins::FunctionPrototypeSymbolHasInstance))
        return { };

    BuiltinClassID builtinClass = table.at(constructor).instanceClass;
    if (!builtinClass)
        return { };
    const BuiltinClass& instanceClass = table.at(builtinClass);
    if (!instanceClass.constructor || !instanceClass.prototype || table.canonicalID(instanceClass.constructor) != constructor)
        return { };

    BuiltinMemberAnswer prototype = findMemberFrom(context, constructor, program.keyOf(BuiltinNames::prototype));
    if (!prototype.isConstant() || prototype.member != table.canonicalID(instanceClass.prototype))
        return { };
    if (!tryMakeBuiltinConstant(table, instanceClass.prototype))
        return { };
    return builtinClass;
}

static LocationProof prototypeLinkProofOf(const BuiltinLookupContext& context, BuiltinID holder)
{
    if (context.program.builtins().hasLockedPrototypeLink(holder))
        return LocationProof::Constant;

    switch (context.flow.builtinHolder(holder)) {
    case BuiltinHolderState::Unknown:
        return LocationProof::Unknown;
    case BuiltinHolderState::Intact:
        return LocationProof::Constant;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static InstanceOfAnswer findInPrototypeChain(const BuiltinLookupContext& context, BuiltinClassID instanceClass, BuiltinID soughtPrototype)
{
    const BuiltinTable& table = context.program.builtins();
    BuiltinID holder = table.at(instanceClass).prototype;
    while (holder) {
        holder = table.canonicalID(holder);
        if (holder == soughtPrototype)
            return InstanceOfAnswer::Yes;
        if (prototypeLinkProofOf(context, holder) != LocationProof::Constant)
            return InstanceOfAnswer::Unknown;

        const Builtin& object = table.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return InstanceOfAnswer::Unknown;
        case BuiltinPrototypeLink::Null:
            return InstanceOfAnswer::No;
        case BuiltinPrototypeLink::Builtin:
            holder = object.inheritsFrom;
            break;
        }
    }
    return InstanceOfAnswer::Unknown;
}

static InstanceOfAnswer answerForClassPart(const BuiltinLookupContext& context, const ClassPart& part, BuiltinID soughtPrototype)
{
    if (!part.provenPristine.contains(PristineAspects::Names))
        return InstanceOfAnswer::Unknown;
    return findInPrototypeChain(context, part.builtinClass, soughtPrototype);
}

static InstanceOfAnswer joinAnswers(InstanceOfAnswer a, InstanceOfAnswer b)
{
    return a == b ? a : InstanceOfAnswer::Unknown;
}

InstanceOfAnswer instanceOfAnswerFor(const BuiltinLookupContext& context, const Type& subject, BuiltinClassID builtinClass)
{
    const BuiltinTable& table = context.program.builtins();
    if (!subject.hasObjects())
        return InstanceOfAnswer::No;
    if (subject.anyObject || subject.anyFunction || subject.layouts || !subject.functions.isEmpty() || !subject.builtins.isEmpty())
        return InstanceOfAnswer::Unknown;
    if (!table.at(builtinClass).prototype)
        return InstanceOfAnswer::Unknown;

    BuiltinID soughtPrototype = table.canonicalID(table.at(builtinClass).prototype);
    InstanceOfAnswer answer = subject.primitives ? InstanceOfAnswer::No : answerForClassPart(context, subject.classes.first(), soughtPrototype);
    for (const ClassPart& part : subject.classes)
        answer = joinAnswers(answer, answerForClassPart(context, part, soughtPrototype));
    return answer;
}

static bool isWorthTesting(FunctionPassContext& context, const ValueSite& site, BuiltinClassID builtinClass)
{
    const Program& program = context.program;
    const BuiltinTable& table = program.builtins();
    FunctionAnalyses& analyses = context.analyses;
    if (pristineTestOf(table, builtinClass) != PristineTest::MayPass)
        return false;
    if (analyses.blockFrequencies().classOf(site.block) == BlockFrequency::Rare)
        return false;

    ValueID subject = context.function.at(site.value).valueOperand();
    const Type& subjectType = program.typeOf(context.function, subject);
    Type instances = Type::builtinClass(builtinClass);
    if (!subjectType.overlaps(instances, program))
        return false;

    bool needsTypeTest = !subjectType.isSubsetOf(instances, program);
    if (needsTypeTest && table.at(builtinClass).instanceTest == BuiltinInstanceTest::None)
        return false;
    if (needsTypeTest && analyses.findEarlierTest(makeTypeTest(subject, context.types.intern(instances)), site.block) == EarlierTest::Failed)
        return false;
    return analyses.findEarlierTest(makePristineTest(subject, builtinClass, PristineAspects::Names), site.block) != EarlierTest::Failed;
}

static bool isWorthTestingConstructor(FunctionPassContext& context, const ValueSite& site, BuiltinID constructor)
{
    FunctionAnalyses& analyses = context.analyses;
    if (analyses.blockFrequencies().classOf(site.block) == BlockFrequency::Rare)
        return false;
    ValueID operand = context.function.at(site.value).constructorOperand();
    return analyses.findEarlierTest(makeTypeTest(operand, context.types.intern(Type::forBuiltin(constructor))), site.block) != EarlierTest::Failed;
}

Vector<BuiltinInstanceOfSite> planBuiltinInstanceOfSites(FunctionPassContext& context)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const BuiltinTable& table = program.builtins();
    BuiltinLookupContext lookup { program, context.inputs.flow };

    Vector<BuiltinInstanceOfSite> sites;
    forEachValueSite(context.analyses, function, [&](const ValueSite& site) {
        const Value& value = function.at(site.value);
        if (value.opcode != Opcode::InstanceOf)
            return;
        BuiltinID constructor = singleBuiltinOf(program.typeOf(function, value.constructorOperand()));
        bool isProvenConstructor = !!constructor;
        if (!constructor)
            constructor = singleBuiltinOf(program.predictionOrTypeOf(function, value.constructorOperand()));
        if (!constructor)
            return;
        constructor = table.canonicalID(constructor);
        BuiltinClassID builtinClass = builtinClassTestedThrough(lookup, constructor);
        if (!builtinClass)
            return;
        if (!isProvenConstructor && !isWorthTestingConstructor(context, site, constructor))
            return;

        BuiltinInstanceOfSite plan { site, builtinClass, instanceOfAnswerFor(lookup, program.typeOf(function, value.valueOperand()), builtinClass), { }, false };
        if (!isProvenConstructor)
            plan.constructorToTest = constructor;
        plan.testsClass = plan.answer == InstanceOfAnswer::Unknown && isWorthTesting(context, site, builtinClass);
        sites.append(plan);
    });
    return sites;
}

static void becomeAnswer(FunctionPassContext& context, ValueID id, bool answer)
{
    Value& value = context.function.at(id);
    value.become(Opcode::Boolean);
    value.setBoolean(answer);
    value.type = resultTypeOf(context.program, context.function, value, context.types);
    value.setWritten(TypeID());
    value.setPrediction(TypeID());
}

// Both lookups on the constructor are answered, so what is left of the operation is the walk up the chain of the value.
static void finishArmWithWalk(SiteExpansion& expansion, BuiltinClassID builtinClass, ValueID subject)
{
    const BuiltinTable& table = expansion.context().program.builtins();
    ValueID prototype = expansion.builder().append(*tryMakeBuiltinConstant(table, table.at(builtinClass).prototype));
    expansion.finishArm(expansion.append(IRBuilder::make(Opcode::HasInPrototypeChain, { subject, prototype })));
}

static void finishArmsWithClassTests(SiteExpansion& expansion, const BuiltinInstanceOfSite& site)
{
    FunctionPassContext& context = expansion.context();
    IRBuilder& builder = expansion.builder();
    Type instances = Type::builtinClass(site.builtinClass);
    BlockFrequency frequency = expansion.frequency();
    ValueID subject = expansion.original().valueOperand();

    if (!context.program.typeOf(context.function, subject).isSubsetOf(instances, context.program)) {
        ValueID test = builder.appendTypeTest(subject, instances);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, testedOperandIndex, frequency, frequency);
        builder.moveToEndOf(branch.notTaken);
        finishArmWithWalk(expansion, site.builtinClass, branch.provenOnNotTaken);
        builder.moveToEndOf(branch.taken);
        subject = branch.provenOnTaken;
    }

    ValueID pristine = builder.append(makePristineTest(subject, site.builtinClass, PristineAspects::Names));
    IRBuilder::BranchBlocks arms = builder.emitBranch(pristine, frequency, BlockFrequency::Unlikely);
    expansion.finishArm(builder.appendBoolean(true));
    builder.moveToEndOf(arms.notTaken);
    finishArmWithWalk(expansion, site.builtinClass, subject);
}

static void expand(SiteExpansion& expansion, const BuiltinInstanceOfSite& site)
{
    IRBuilder& builder = expansion.builder();
    expansion.begin(site.site);

    if (site.constructorToTest) {
        ValueID test = builder.appendTypeTest(expansion.original().constructorOperand(), Type::forBuiltin(site.constructorToTest));
        IRBuilder::BranchBlocks arms = builder.emitBranch(test, expansion.frequency(), BlockFrequency::Unlikely);
        builder.moveToEndOf(arms.notTaken);
        expansion.finishArm(expansion.appendOriginal());
        builder.moveToEndOf(arms.taken);
    }

    switch (site.answer) {
    case InstanceOfAnswer::Unknown:
        if (site.testsClass)
            finishArmsWithClassTests(expansion, site);
        else
            finishArmWithWalk(expansion, site.builtinClass, expansion.original().valueOperand());
        break;
    case InstanceOfAnswer::No:
        expansion.finishArm(builder.appendBoolean(false));
        break;
    case InstanceOfAnswer::Yes:
        expansion.finishArm(builder.appendBoolean(true));
        break;
    }
    expansion.end();
}

void rewriteBuiltinInstanceOfSites(FunctionPassContext& context, std::span<const BuiltinInstanceOfSite> sites)
{
    if (sites.empty())
        return;

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    SiteExpansion expansion(context, builder);
    for (const BuiltinInstanceOfSite& site : sites | std::views::reverse) {
        if (site.constructorToTest) {
            expand(expansion, site);
            continue;
        }
        switch (site.answer) {
        case InstanceOfAnswer::Unknown:
            expand(expansion, site);
            break;
        case InstanceOfAnswer::No:
            becomeAnswer(context, site.site.value, false);
            break;
        case InstanceOfAnswer::Yes:
            becomeAnswer(context, site.site.value, true);
            break;
        }
    }
    context.analyses.didChangeValues();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
