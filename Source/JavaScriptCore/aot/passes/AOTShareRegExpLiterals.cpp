/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRewrite.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEscapeAnalysis.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTKnownValue.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTResultRules.h"
#include "AOTRuleSupport.h"
#include "AOTStringArena.h"
#include "YarrFlags.h"
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(numberOfEnumerators<RegExpFlags> == Yarr::numberOfFlags);

static const RegExpSummary& regExpSummaryOf(FunctionPassContext& context, StringID pattern, OptionSet<RegExpFlags> flags)
{
    Vector<char16_t> buffer;
    return context.build.regExpSummaries().summaryOf(pattern, toYarrFlags(flags), context.program.strings.viewOf(pattern, buffer));
}

PassOutcome foldConstantRegExps(FunctionPassContext& context)
{
    Rewrite rewrite(context);
    const Program& program = context.program;
    const Function& function = context.function;

    auto known = [&](ValueID id) { return knownValueOf(program, function, id); };
    auto mayRunJavaScript = [&](const Value& call) {
        EffectDescription description = describeEffects(program, function, call);
        return description.callsUnknownCode || !description.functions.isEmpty();
    };
    auto replace = [&](ValueID id, Value&& replacement) {
        replacement.type = resultTypeOf(program, function, replacement, context.types);
        if (!context.types.at(replacement.type).isSubsetOf(rewrite.typeOf(id), program))
            return;
        replacement.setOrigin(function.at(id).origin());
        rewrite.edit(id) = WTF::move(replacement);
        rewrite.retype(id);
    };
    auto parseFlags = [&](KnownValue flags) -> std::optional<OptionSet<RegExpFlags>> {
        if (flags.kind() != KnownValue::Kind::String)
            return std::nullopt;
        Vector<char16_t> buffer;
        std::optional<OptionSet<Yarr::Flags>> parsedFlags = Yarr::parseFlags(program.strings.viewOf(flags.asString(), buffer));
        if (!parsedFlags)
            return std::nullopt;
        OptionSet<RegExpFlags> result;
        for (RegExpFlags flag : allMembersOf<RegExpFlags>) {
            if (parsedFlags->contains(toYarrFlag(flag)))
                result.add(flag);
        }
        return result;
    };
    auto patternTextOf = [&](ValueID pattern) {
        if (!pattern || known(pattern).kind() == KnownValue::Kind::Undefined)
            return program.strings.emptyString();
        KnownValue text = evaluateStringOperation(program, context.strings, function, IRBuilder::make(Opcode::ToString, { pattern }), known);
        return text.kind() == KnownValue::Kind::String ? text.asString() : StringID();
    };
    auto createdLiteralOf = [&](ValueID pattern, KnownValue flags) -> std::optional<RegExpLiteralInType> {
        bool passesFlags = flags.kind() != KnownValue::Kind::Undefined;
        std::optional<OptionSet<RegExpFlags>> passedFlags = passesFlags ? parseFlags(flags) : OptionSet<RegExpFlags> { };
        if (!passedFlags)
            return std::nullopt;
        if (StringID text = patternTextOf(pattern))
            return RegExpLiteralInType { text, passedFlags->toRaw() };
        std::optional<RegExpLiteralInType> copied = singleRegExpLiteralAsCreatedOf(rewrite.typeOf(pattern));
        if (!copied || !passesFlags)
            return std::nullopt;
        return RegExpLiteralInType { copied->pattern, passedFlags->toRaw() };
    };
    auto foldCreation = [&](ValueID id) {
        const Value& call = function.at(id);
        std::span<const ValueID> arguments = call.argumentsOperands();
        if (arguments.size() > 2)
            return;
        KnownValue flags = arguments.size() == 2 ? known(arguments[1]) : KnownValue::undefined();
        std::optional<RegExpLiteralInType> literal = createdLiteralOf(arguments.empty() ? ValueID() : arguments[0], flags);
        if (!literal || mayRunJavaScript(call) || !regExpSummaryOf(context, literal->pattern, literal->flags()).isValid())
            return;
        Value creation(Opcode::NewRegExp);
        creation.setPattern(literal->pattern);
        creation.setRegExpFlags(literal->flags());
        creation.setAllocationSite(call.allocationSite());
        replace(id, WTF::move(creation));
    };
    auto foldGetter = [&](ValueID id, BuiltinID getter, const BuiltinImplementation& implementation) {
        const Value& call = function.at(id);
        ValueID receiver = call.receiverOperand();
        std::optional<RegExpLiteralInType> literal = receiver ? singleRegExpLiteralAsCreatedOf(rewrite.typeOf(receiver)) : std::nullopt;
        if (!literal || mayRunJavaScript(call))
            return;
        if (implementation.inlineBuiltin == InlineBuiltin::RegExpFlag) {
            Value constant(Opcode::Boolean);
            constant.setBoolean(literal->flags().contains(static_cast<RegExpFlags>(implementation.inlineParameters[0])));
            replace(id, WTF::move(constant));
            return;
        }
        StringID string;
        if (getter == Builtins::RegExpPrototypeFlags) {
            Yarr::FlagsString flags = Yarr::flagsString(toYarrFlags(literal->flags()));
            string = context.strings.intern(StringView { flags.span() });
        } else if (getter == Builtins::RegExpPrototypeSource) {
            const RegExpSummary& summary = regExpSummaryOf(context, literal->pattern, literal->flags());
            if (summary.isValid())
                string = summary.sourceIsPattern ? literal->pattern : context.strings.intern(StringView(summary.source));
        }
        if (!string)
            return;
        Value constant(Opcode::String);
        constant.setString(string);
        replace(id, WTF::move(constant));
    };

    // A fold is made at once: the getter behind a creation reads the literal out of the type of the new value.
    const BuiltinTable& table = program.builtins();
    for (BlockID blockID : rewrite.blocks()) {
        const Block& block = function.at(blockID);
        ValueID tailCall = block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.callOperand() : ValueID();
        for (ValueID id : block.values) {
            const Value& call = function.at(id);
            if (id == tailCall || call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole)
                continue;
            const BuiltinOverload& overload = table.at(call.overload());
            BuiltinID builtin = table.canonicalID(overload.builtin);
            switch (overload.mode) {
            case BuiltinCallMode::Call:
            case BuiltinCallMode::Construct:
                if (builtin == Builtins::RegExp)
                    foldCreation(id);
                break;
            case BuiltinCallMode::Get:
                foldGetter(id, builtin, overload.code.implementation);
                break;
            case BuiltinCallMode::Set:
                break;
            }
        }
    }
    return rewrite.commit();
}

static constexpr OperandUses contentsReadOrWritten { OperandUse::ReadsContents, OperandUse::WritesContents };

static constexpr AnalysisSet analysesUnaffectedBySharing {
    FunctionAnalysis::UseLists,
    FunctionAnalysis::Identities,
    FunctionAnalysis::Liveness,
    FunctionAnalysis::Frequencies,
    FunctionAnalysis::FailedTests,
};

// While every use of the object is one that this pass accepts, lastIndex holds a number, and converting it runs no user code.
static constexpr OperandUses usesOfMatching {
    OperandUse::Inspects,
    OperandUse::ReadsContents,
    OperandUse::WritesContents,
    OperandUse::AppliesTransitively,
    OperandUse::AppliesToContents,
    OperandUse::PassesToUserCode,
};

PassOutcome shareRegExpLiterals(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    const BuildContext& build = context.build;
    Vector<ValueID, 4> literals;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (function.at(id).opcode == Opcode::NewRegExp)
                literals.append(id);
        }
    }
    if (literals.isEmpty())
        return PassOutcome::Unchanged;

    const EscapeAnalysis& escape = context.analyses.escape();
    const ValueIdentities& identities = context.analyses.identities();
    auto isOnlyInspected = [](const ObjectUse& objectUse) { return !objectUse.isEscape && objectUse.uses.containsOnly({ OperandUse::Inspects }); };
    // The copies of a site that inlining and cloning make share the object of the site.
    auto takesObjectOfAnotherCopy = [&](const Value& user, ValueID id) {
        const Value& literal = function.at(id);
        bool result = false;
        user.forEachChild([&](ValueID operand) {
            for (ObjectID object : escape.pointsTo(operand).objects) {
                const Value& other = function.at(escape.at(object).definition);
                if (escape.at(object).definition != id && other.opcode == Opcode::NewRegExp && other.position == literal.position && other.pattern() == literal.pattern() && other.regExpFlags() == literal.regExpFlags())
                    result = true;
            }
        });
        return result;
    };
    auto isHarmlessInBuiltin = [&](const Value& call, const ObjectUse& objectUse, OptionSet<RegExpFlags> flags) {
        if (!objectUse.uses.containsAny(contentsReadOrWritten))
            return isOnlyInspected(objectUse);
        std::optional<InlineBuiltin> builtin;
        if (objectUse.uses.containsOnly(usesOfMatching) && declaredBehaviorOf(program.builtins().at(call.overload()), builtinOperandAt(call, objectUse.use.operandIndex)).protocols.containsOnly({ Protocol::ToPrimitive }))
            builtin = inlineImplementationOf(program, call);
        if (!builtin)
            return build.isSabotaged(Sabotage::SharedPatternsAreUsedInUnknownWays);

        // A match stores lastIndex only under g or y. With g, these String methods store 0 first and stop at the failed match that stores 0 again.
        switch (*builtin) {
        case InlineBuiltin::RegExpExec:
        case InlineBuiltin::RegExpExecPrefix:
        case InlineBuiltin::RegExpTest:
            return !flags.containsAny({ RegExpFlags::Global, RegExpFlags::Sticky }) || build.isSabotaged(Sabotage::SharedPatternsAdvance);
        case InlineBuiltin::StringMatch:
        case InlineBuiltin::StringMatchPrefix:
        case InlineBuiltin::StringReplace:
        case InlineBuiltin::StringReplaceAll:
            return !flags.contains(RegExpFlags::Sticky) || build.isSabotaged(Sabotage::SharedPatternsAreSticky);
        default:
            return build.isSabotaged(Sabotage::SharedPatternsAreUsedInUnknownWays);
        }
    };
    auto isHarmless = [&](const ObjectUse& objectUse, ValueID literal) {
        if (objectUse.use.kind != Use::Kind::Operand)
            return isOnlyInspected(objectUse);
        const Value& user = function.at(objectUse.use.user);
        if (takesObjectOfAnotherCopy(user, literal))
            return false;
        switch (effectRuleOf(user.opcode)) {
        case EffectRule::Declared:
        case EffectRule::Operator:
        case EffectRule::Access:
            return isOnlyInspected(objectUse);
        case EffectRule::Call:
            // A callee may evaluate the site of the literal again, and would find the two objects identical.
            return build.isSabotaged(Sabotage::SharedPatternsReachCalls);
        case EffectRule::Builtin:
            return isHarmlessInBuiltin(user, objectUse, function.at(literal).regExpFlags());
        case EffectRule::Inline:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto canShare = [&](ValueID literal) {
        std::optional<ObjectID> object = escape.objectDefinedBy(literal);
        if (!object || escape.at(*object).reasons.contains(EscapeReason::LimitReached))
            return false;
        if (!std::ranges::all_of(escape.aliasesOf(*object), [&](ValueID alias) { return identities.identityOf(alias) == literal; }))
            return false;
        if (!std::ranges::all_of(escape.usesOf(*object), [&](const ObjectUse& objectUse) { return isHarmless(objectUse, literal); }))
            return false;
        // The operation that creates a shared literal cannot throw, and a pattern may fail to compile unless the build has its metadata.
        const RegExpSummary& summary = regExpSummaryOf(context, function.at(literal).pattern(), function.at(literal).regExpFlags());
        return summary.isValid() && summary.metadata == RegExpMetadata::Known;
    };
    literals.removeAllMatching([&](ValueID literal) { return !canShare(literal); });
    if (literals.isEmpty())
        return PassOutcome::Unchanged;

    for (ValueID literal : literals) {
        Value& value = function.edit(literal);
        StringID pattern = value.pattern();
        OptionSet<RegExpFlags> flags = value.regExpFlags();
        value.become(Opcode::SharedRegExpLiteral);
        value.setPattern(pattern);
        value.setRegExpFlags(flags);
    }
    context.analyses.invalidate(valueAnalyses - analysesUnaffectedBySharing);
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
