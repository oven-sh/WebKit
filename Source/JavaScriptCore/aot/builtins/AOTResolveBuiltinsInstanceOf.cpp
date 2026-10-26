/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveBuiltinsInternal.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTResultRules.h"
#include "AOTTypeTestSite.h"
#include "AOTVector.h"
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static bool findsOrdinaryHasInstance(const BuiltinLookupContext& context, BuiltinID constructor)
{
    const Program& program = context.program;
    const BuiltinTable& table = program.builtins();
    if (constructor == table.canonicalID(Builtins::File))
        return false;
    BuiltinMemberAnswer hasInstance = findMemberFrom(context, constructor, program.keyOf(table.nameOf(WellKnownSymbol::HasInstance)));
    return hasInstance.isConstant() && hasInstance.member == table.canonicalID(Builtins::FunctionPrototypeSymbolHasInstance);
}

static BuiltinClassID builtinClassTestedThrough(const BuiltinLookupContext& context, BuiltinID constructor)
{
    const Program& program = context.program;
    const BuiltinTable& table = program.builtins();

    if (!findsOrdinaryHasInstance(context, constructor))
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

static InstanceOfAnswer answerForLayouts(const BuiltinLookupContext& context, LayoutSetID layouts, BuiltinID soughtPrototype)
{
    const Program& program = context.program;
    bool areRecordsAsMade = std::ranges::all_of(program.at(layouts).members, [&](LayoutID layout) {
        return program.at(layout).kind == LayoutKind::Record && receiverStateOf(program, layout).cannot(ObjectMutation::ChangePrototype);
    });
    if (!areRecordsAsMade)
        return InstanceOfAnswer::Unknown;
    return soughtPrototype == program.builtins().canonicalID(Builtins::ObjectPrototype) ? InstanceOfAnswer::Yes : InstanceOfAnswer::No;
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
    if (subject.anyObject || subject.anyFunction || !subject.functions.isEmpty() || !subject.builtins.isEmpty())
        return InstanceOfAnswer::Unknown;
    if (!table.at(builtinClass).prototype)
        return InstanceOfAnswer::Unknown;

    BuiltinID soughtPrototype = table.canonicalID(table.at(builtinClass).prototype);
    std::optional<InstanceOfAnswer> answer;
    auto add = [&](InstanceOfAnswer ofPart) {
        answer = answer ? joinAnswers(*answer, ofPart) : ofPart;
    };
    if (subject.primitives)
        add(InstanceOfAnswer::No);
    if (subject.layouts)
        add(answerForLayouts(context, subject.layouts, soughtPrototype));
    for (const ClassPart& part : subject.classes)
        add(answerForClassPart(context, part, soughtPrototype));
    return *answer;
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
    context.forEachSiteIn(SiteList::InstanceOfTests, [&](const ValueSite& site) {
        const Value& value = function.at(site.value);
        BuiltinID constructor = singleBuiltinOf(program.typeOf(function, value.constructorOperand()));
        bool isProvenConstructor = !!constructor;
        if (!constructor)
            constructor = singleBuiltinOf(program.expectedOrTypeOf(function, value.constructorOperand()));
        if (!constructor)
            return;
        constructor = table.canonicalID(constructor);
        BuiltinClassID builtinClass = builtinClassTestedThrough(lookup, constructor);
        bool readsPrototype = !builtinClass && table.at(constructor).kind == BuiltinKind::Constructor && findsOrdinaryHasInstance(lookup, constructor);
        if (!builtinClass && !readsPrototype)
            return;
        if (!isProvenConstructor && !isWorthTestingConstructor(context, site, constructor))
            return;
        if (readsPrototype) {
            if (context.analyses.blockFrequencies().classOf(site.block) == BlockFrequency::Rare)
                return;
            sites.append(BuiltinInstanceOfSite { site, { }, InstanceOfAnswer::Unknown, isProvenConstructor ? BuiltinID() : constructor, false, true });
            return;
        }

        BuiltinInstanceOfSite plan { site, builtinClass, instanceOfAnswerFor(lookup, program.typeOf(function, value.valueOperand()), builtinClass), { }, false, false };
        if (!isProvenConstructor)
            plan.constructorToTest = constructor;
        plan.testsClass = plan.answer == InstanceOfAnswer::Unknown && isWorthTesting(context, site, builtinClass);
        sites.append(plan);
    });
    return sites;
}

static void becomeAnswer(FunctionPassContext& context, ValueID id, bool answer)
{
    Value& value = context.function.edit(id);
    value.become(Opcode::Boolean);
    value.setBoolean(answer);
    value.type = resultTypeOf(context.program, context.function, value, context.types);
    value.setExpected({ });
}

static void finishArmWithWalk(SiteExpansion& expansion, BuiltinClassID builtinClass, ValueID subject)
{
    FunctionPassContext& context = expansion.context();
    const BuiltinTable& table = context.program.builtins();
    InstanceOfAnswer answer = instanceOfAnswerFor({ context.program, context.inputs.flow }, context.program.typeOf(context.function, subject), builtinClass);
    if (answer != InstanceOfAnswer::Unknown) {
        expansion.finishArm(expansion.builder().appendBoolean(answer == InstanceOfAnswer::Yes));
        return;
    }
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

static void finishArmsWithPrototypeRead(SiteExpansion& expansion)
{
    FunctionPassContext& context = expansion.context();
    IRBuilder& builder = expansion.builder();
    Value read = IRBuilder::make(Opcode::PropGet, { expansion.original().constructorOperand() });
    read.setKey(context.program.keyOf(BuiltinNames::prototype));
    ValueID prototype = expansion.append(WTF::move(read));
    ValueID test = builder.appendTypeTest(prototype, Type::object());
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, testedOperandIndex, expansion.frequency(), BlockFrequency::Rare);
    builder.moveToEndOf(branch.notTaken);
    expansion.finishArm(expansion.appendOriginal());
    builder.moveToEndOf(branch.taken);
    expansion.finishArm(expansion.append(IRBuilder::make(Opcode::HasInPrototypeChain, { expansion.original().valueOperand(), branch.provenOnTaken })));
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
        if (site.readsPrototype)
            finishArmsWithPrototypeRead(expansion);
        else if (site.testsClass)
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
