/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEscapeAnalysis.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTStringArena.h"
#include "AOTValueIdentity.h"
#include "YarrFlags.h"
#include <optional>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses contentsUses { OperandUse::ReadsContents, OperandUse::WritesContents };

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

static bool isOnlyInspected(const ObjectUse& objectUse)
{
    return !objectUse.isEscape && objectUse.uses.containsOnly({ OperandUse::Inspects });
}

static bool mayBeCopiesOfOneSite(const Value& literal, const Value& other)
{
    return other.opcode == Opcode::NewRegExp
        && other.position == literal.position
        && other.pattern() == literal.pattern()
        && other.regExpFlags() == literal.regExpFlags();
}

namespace {

enum class LastIndexUse : uint8_t { Unknown, Ignores, ResetsIfGlobal, AdvancesIfGlobalOrSticky };

class LiteralUses {
public:
    explicit LiteralUses(FunctionPassContext& context)
        : m_program(context.program)
        , m_function(context.function)
        , m_escape(context.analyses.escape())
        , m_identities(context.analyses.identities())
    {
    }

    bool allowSharing(ValueID literal) const
    {
        std::optional<ObjectID> object = m_escape.objectDefinedBy(literal);
        if (!object || m_escape.at(*object).reasons.contains(EscapeReason::LimitReached))
            return false;
        for (ValueID alias : m_escape.aliasesOf(*object)) {
            if (m_identities.identityOf(alias) != literal)
                return false;
        }
        for (const ObjectUse& objectUse : m_escape.usesOf(*object)) {
            if (!isHarmless(objectUse, literal))
                return false;
        }
        return true;
    }

private:
    bool isHarmless(const ObjectUse& objectUse, ValueID literal) const
    {
        const Use& use = objectUse.use;
        if (use.kind != Use::Kind::Operand)
            return isOnlyInspected(objectUse);

        const Value& user = m_function.at(use.user);
        if (takesObjectOfAnotherCopy(user, literal))
            return false;
        switch (effectRuleOf(user.opcode)) {
        case EffectRule::Declared:
        case EffectRule::Operator:
        case EffectRule::Access:
            return isOnlyInspected(objectUse);
        case EffectRule::Call:
            // A callee may evaluate the site of the literal again, and would find the two objects identical.
            return false;
        case EffectRule::Builtin:
            return isHarmlessInBuiltin(user, objectUse, m_function.at(literal).regExpFlags());
        case EffectRule::Inline:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The copies of a site that inlining and cloning make share the object of the site.
    bool takesObjectOfAnotherCopy(const Value& user, ValueID literal) const
    {
        bool result = false;
        user.forEachChild([&](ValueID operand) {
            for (ObjectID object : m_escape.pointsTo(operand).objects) {
                ValueID definition = m_escape.at(object).definition;
                if (definition != literal && mayBeCopiesOfOneSite(m_function.at(literal), m_function.at(definition)))
                    result = true;
            }
        });
        return result;
    }

    bool isHarmlessInBuiltin(const Value& call, const ObjectUse& objectUse, OptionSet<RegExpFlags> flags) const
    {
        switch (lastIndexUseOf(call, objectUse)) {
        case LastIndexUse::Unknown:
            return false;
        case LastIndexUse::Ignores:
            return isOnlyInspected(objectUse);
        case LastIndexUse::ResetsIfGlobal:
            return !flags.contains(RegExpFlags::Sticky);
        case LastIndexUse::AdvancesIfGlobalOrSticky:
            return !flags.containsAny({ RegExpFlags::Global, RegExpFlags::Sticky });
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    LastIndexUse lastIndexUseOf(const Value& call, const ObjectUse& objectUse) const
    {
        if (!objectUse.uses.containsAny(contentsUses))
            return LastIndexUse::Ignores;
        if (!objectUse.uses.containsOnly(usesOfMatching))
            return LastIndexUse::Unknown;

        const BuiltinOverload& overload = m_program.builtins().at(call.overload());
        OperandBehavior behavior = declaredBehaviorOf(overload, builtinOperandAt(call, objectUse.use.operandIndex));
        if (!behavior.protocols.containsOnly({ Protocol::ToPrimitive }))
            return LastIndexUse::Unknown;

        std::optional<InlineBuiltin> builtin = inlineImplementationOf(m_program, call);
        if (!builtin)
            return LastIndexUse::Unknown;

        // A match stores lastIndex only under g or y. With g, these String methods store 0 first and stop at the failed match that stores 0 again.
        switch (*builtin) {
        case InlineBuiltin::RegExpExec:
        case InlineBuiltin::RegExpExecPrefix:
        case InlineBuiltin::RegExpTest:
            return LastIndexUse::AdvancesIfGlobalOrSticky;
        case InlineBuiltin::StringMatch:
        case InlineBuiltin::StringMatchPrefix:
        case InlineBuiltin::StringReplace:
        case InlineBuiltin::StringReplaceAll:
            return LastIndexUse::ResetsIfGlobal;
        default:
            return LastIndexUse::Unknown;
        }
    }

    const Program& m_program;
    const Function& m_function;
    const EscapeAnalysis& m_escape;
    const ValueIdentities& m_identities;
};

} // anonymous namespace

// The operation that creates a shared literal cannot throw, and a pattern may fail to compile unless the build has its metadata.
static bool isCreatedWithoutError(FunctionPassContext& context, const Value& literal)
{
    Vector<char16_t> buffer;
    StringView pattern = context.program.strings.viewOf(literal.pattern(), buffer);
    const RegExpSummary& summary = context.build.regExpSummaries().summaryOf(literal.pattern(), toYarrFlags(literal.regExpFlags()), pattern);
    return summary.isValid() && summary.metadata == RegExpMetadata::Known;
}

static Vector<ValueID, 4> collectLiterals(FunctionPassContext& context)
{
    Vector<ValueID, 4> literals;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : context.function.at(block).values) {
            if (context.function.at(id).opcode == Opcode::NewRegExp)
                literals.append(id);
        }
    }
    return literals;
}

PassOutcome shareRegExpLiterals(FunctionPassContext& context)
{
    Vector<ValueID, 4> literals = collectLiterals(context);
    if (literals.isEmpty())
        return PassOutcome::Unchanged;

    LiteralUses uses(context);
    literals.removeAllMatching([&](ValueID literal) {
        return !uses.allowSharing(literal) || !isCreatedWithoutError(context, context.function.at(literal));
    });
    if (literals.isEmpty())
        return PassOutcome::Unchanged;

    for (ValueID literal : literals) {
        Value& value = context.function.edit(literal);
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
