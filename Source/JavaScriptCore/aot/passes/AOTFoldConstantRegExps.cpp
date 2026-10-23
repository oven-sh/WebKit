/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsDescription.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTKnownValue.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTResultRules.h"
#include "AOTRuleSupport.h"
#include "AOTStringArena.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
#include "YarrFlags.h"
#include <optional>
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(numberOfEnumerators<RegExpFlags> == Yarr::numberOfFlags);

static OptionSet<RegExpFlags> toRegExpFlags(OptionSet<Yarr::Flags> flags)
{
    OptionSet<RegExpFlags> result;
    for (RegExpFlags flag : allMembersOf<RegExpFlags>) {
        if (flags.contains(toYarrFlag(flag)))
            result.add(flag);
    }
    return result;
}

namespace {

class RegExpFolder {
public:
    explicit RegExpFolder(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
    {
    }

    PassOutcome run()
    {
        Vector<ValueID, 8> foldedValues;
        for (BlockID blockID : m_context.analyses.cfg().reversePostOrder()) {
            const Block& block = m_function.at(blockID);
            ValueID tailCall = block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.callOperand() : ValueID();
            for (ValueID id : block.values) {
                if (id != tailCall && foldValue(id))
                    foldedValues.append(id);
            }
        }
        if (foldedValues.isEmpty())
            return PassOutcome::Unchanged;

        m_context.analyses.didChangeValues();
        narrowValueTypes(TypePropagationInputs { m_program, m_context.id, m_function, m_context.analyses, m_context.types, m_context.build, &m_context.typingMemo }, foldedValues.span());
        m_context.keptAnalyses = allAnalyses;
        return PassOutcome::Changed;
    }

private:
    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    KnownValue knownValueOf(ValueID id) const { return Typed::knownValueOf(m_program, m_function, id); }

    bool foldValue(ValueID id)
    {
        const Value& call = m_function.at(id);
        if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole)
            return false;

        const BuiltinTable& table = m_program.builtins();
        const BuiltinOverload& overload = table.at(call.overload());
        BuiltinID builtin = table.canonicalID(overload.builtin);
        switch (overload.mode) {
        case BuiltinCallMode::Call:
        case BuiltinCallMode::Construct:
            return builtin == Builtins::RegExp && foldCreation(id);
        case BuiltinCallMode::Get:
            return foldGetter(id, builtin, overload);
        case BuiltinCallMode::Set:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool foldCreation(ValueID id)
    {
        const Value& call = m_function.at(id);
        std::span<const ValueID> arguments = call.argumentsOperands();
        if (arguments.size() > 2)
            return false;

        KnownValue flags = arguments.size() == 2 ? knownValueOf(arguments[1]) : KnownValue::undefined();
        std::optional<RegExpLiteralInType> literal = createdLiteralOf(arguments.empty() ? ValueID() : arguments[0], flags);
        if (!literal || mayRunJavaScript(call) || !isValid(*literal))
            return false;

        Value creation(Opcode::NewRegExp);
        creation.setPattern(literal->pattern);
        creation.setRegExpFlags(literal->flags());
        creation.setAllocationSite(call.allocationSite());
        return replace(id, WTF::move(creation));
    }

    std::optional<RegExpLiteralInType> createdLiteralOf(ValueID pattern, KnownValue flags) const
    {
        bool passesFlags = flags.kind() != KnownValue::Kind::Undefined;
        OptionSet<RegExpFlags> passedFlags;
        if (passesFlags) {
            std::optional<OptionSet<RegExpFlags>> parsedFlags = parseFlags(flags);
            if (!parsedFlags)
                return std::nullopt;
            passedFlags = *parsedFlags;
        }

        if (StringID text = patternTextOf(pattern))
            return RegExpLiteralInType { text, passedFlags.toRaw() };

        std::optional<RegExpLiteralInType> copied = singleRegExpLiteralAsCreatedOf(typeOf(pattern));
        if (!copied || !passesFlags)
            return std::nullopt;
        return RegExpLiteralInType { copied->pattern, passedFlags.toRaw() };
    }

    StringID patternTextOf(ValueID pattern) const
    {
        if (!pattern || knownValueOf(pattern).kind() == KnownValue::Kind::Undefined)
            return m_program.strings.emptyString();

        KnownValue text = evaluateStringOperation(m_program, m_context.strings, m_function, IRBuilder::make(Opcode::ToString, { pattern }), [&](ValueID operand) {
            return knownValueOf(operand);
        });
        return text.kind() == KnownValue::Kind::String ? text.asString() : StringID();
    }

    std::optional<OptionSet<RegExpFlags>> parseFlags(KnownValue flags) const
    {
        if (flags.kind() != KnownValue::Kind::String)
            return std::nullopt;
        Vector<char16_t> buffer;
        std::optional<OptionSet<Yarr::Flags>> parsedFlags = Yarr::parseFlags(m_program.strings.viewOf(flags.asString(), buffer));
        if (!parsedFlags)
            return std::nullopt;
        return toRegExpFlags(*parsedFlags);
    }

    bool foldGetter(ValueID id, BuiltinID getter, const BuiltinOverload& overload)
    {
        const Value& call = m_function.at(id);
        ValueID receiver = call.receiverOperand();
        if (!receiver)
            return false;
        std::optional<RegExpLiteralInType> literal = singleRegExpLiteralAsCreatedOf(typeOf(receiver));
        if (!literal || mayRunJavaScript(call))
            return false;

        const BuiltinImplementation& implementation = overload.code.implementation;
        if (implementation.inlineBuiltin == InlineBuiltin::RegExpFlag)
            return becomeBoolean(id, literal->flags().contains(static_cast<RegExpFlags>(implementation.inlineParameters[0])));
        if (getter == Builtins::RegExpPrototypeFlags) {
            Yarr::FlagsString flags = Yarr::flagsString(toYarrFlags(literal->flags()));
            return becomeString(id, m_context.strings.intern(StringView { flags.span() }));
        }
        if (getter == Builtins::RegExpPrototypeSource) {
            StringID source = sourceOf(*literal);
            return source && becomeString(id, source);
        }
        return false;
    }

    bool mayRunJavaScript(const Value& call) const
    {
        EffectDescription description = describeEffects(m_program, m_function, call);
        return description.callsUnknownCode || !description.functions.isEmpty();
    }

    const RegExpSummary& summaryOf(RegExpLiteralInType literal) const
    {
        Vector<char16_t> buffer;
        StringView pattern = m_program.strings.viewOf(literal.pattern, buffer);
        return m_context.build.regExpSummaries().summaryOf(literal.pattern, toYarrFlags(literal.flags()), pattern);
    }

    bool isValid(RegExpLiteralInType literal) const
    {
        return summaryOf(literal).isValid();
    }

    StringID sourceOf(RegExpLiteralInType literal) const
    {
        const RegExpSummary& summary = summaryOf(literal);
        if (!summary.isValid())
            return { };
        return summary.sourceIsPattern ? literal.pattern : m_context.strings.intern(StringView(summary.source));
    }

    bool becomeBoolean(ValueID id, bool boolean)
    {
        Value constant(Opcode::Boolean);
        constant.setBoolean(boolean);
        return replace(id, WTF::move(constant));
    }

    bool becomeString(ValueID id, StringID string)
    {
        Value constant(Opcode::String);
        constant.setString(string);
        return replace(id, WTF::move(constant));
    }

    bool replace(ValueID id, Value&& replacement)
    {
        TypeID type = resultTypeOf(m_program, m_function, replacement, m_context.types);
        if (!m_context.types.at(type).isSubsetOf(typeOf(id), m_program))
            return false;

        Value& value = m_function.edit(id);
        replacement.setOrigin(value.origin());
        replacement.type = type;
        value = WTF::move(replacement);
        return true;
    }

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
};

} // anonymous namespace

PassOutcome foldConstantRegExps(FunctionPassContext& context)
{
    return RegExpFolder(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
