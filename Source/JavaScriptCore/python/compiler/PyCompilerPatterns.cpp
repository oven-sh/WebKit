/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCompilerUnit.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyRuntime.h"
#include "PyTuple.h"
#include "TopExceptionScope.h"
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

using namespace AST;

// The match statement. A pattern that fails jumps to context.fail. The names a pattern captures are kept in registers, and
// bound only once the whole pattern has matched.

static bool isWildcard(Pattern* pattern)
{
    return pattern->kind == PatternKind::MatchAs && !pattern->as<MatchAs>()->name;
}

static bool isStarWildcard(Pattern* pattern)
{
    return pattern->kind == PatternKind::MatchStar && !pattern->as<MatchStar>()->name;
}

void Unit::capture(Name name, RegisterID* value, const Range& range, PatternContext& context)
{
    if (!name)
        return;
    for (auto& existing : context.captures) {
        if (existing.name->impl() == name->impl()) {
            fail(range, makeString("multiple assignments to name '"_s, name->string(), "' in pattern"_s));
            return;
        }
    }
    Reg held = value;
    Reg reg = newTemporary();
    g.move(reg.get(), value);
    context.captures.append({ name, WTF::move(reg) });
}

void Unit::subpattern(Pattern* node, RegisterID* subject, PatternContext& context)
{
    bool saved = context.allowIrrefutable;
    context.allowIrrefutable = true;
    pattern(node, subject, context);
    context.allowIrrefutable = saved;
}

void Unit::patternAs(MatchAs* node, RegisterID* subject, PatternContext& context)
{
    if (!node->pattern) {
        if (!context.allowIrrefutable) {
            if (node->name)
                fail(node->range, makeString("name capture '"_s, node->name->string(), "' makes remaining patterns unreachable"_s));
            else
                fail(node->range, "wildcard makes remaining patterns unreachable"_s);
            return;
        }
        capture(node->name, subject, node->range, context);
        return;
    }
    pattern(node->pattern, subject, context);
    capture(node->name, subject, node->range, context);
}

void Unit::patternValue(MatchValue* node, RegisterID* subject, PatternContext& context)
{
    JSValue constantValue;
    if (!node->value->is<Attribute>() && !tryConstantValue(node->value, constantValue)) {
        fail(node->range, "patterns may only match literals and attribute lookups"_s);
        return;
    }
    Reg value = expr(node->value);
    Reg result = newTemporary();
    loc(node->range);
    g.emitPyCompare(result.get(), subject, value.get(), Python::CompareOperator::Eq);
    g.emitPyJumpIfFalse(result.get(), *context.fail);
}

void Unit::patternSingleton(MatchSingleton* node, RegisterID* subject, PatternContext& context)
{
    Reg result = newTemporary();
    if (node->value.kind == ConstantValue::Kind::None)
        g.emitPyIsNone(result.get(), subject);
    else
        g.emitPyCompare(result.get(), subject, constant(constantValue(node->value)), Python::CompareOperator::Is);
    g.emitPyJumpIfFalse(result.get(), *context.fail);
}

void Unit::patternSequence(MatchSequence* node, RegisterID* subject, PatternContext& context)
{
    Seq<Pattern*> patterns = node->patterns;
    unsigned size = patterns.size();
    int star = -1;
    bool onlyWildcards = true;
    bool starIsWildcard = false;
    for (unsigned i = 0; i < size; ++i) {
        Pattern* item = patterns[i];
        if (item->kind == PatternKind::MatchStar) {
            if (star >= 0) {
                fail(node->range, "multiple starred names in sequence pattern"_s);
                return;
            }
            starIsWildcard = isStarWildcard(item);
            onlyWildcards &= starIsWildcard;
            star = i;
            continue;
        }
        onlyWildcards &= isWildcard(item);
    }

    Reg held = subject;
    Reg scratch = newTemporary();
    loc(node->range);
    callRuntime(scratch.get(), RuntimeFunction::MatchSequence, { subject });
    g.emitPyJumpIfFalse(scratch.get(), *context.fail);
    if (star < 0 || size > 1) {
        callRuntime(scratch.get(), RuntimeFunction::GetLength, { subject });
        if (star < 0)
            g.emitPyCompare(scratch.get(), scratch.get(), constant(jsNumber(size)), Python::CompareOperator::Eq);
        else
            g.emitPyCompare(scratch.get(), scratch.get(), constant(jsNumber(size - 1)), Python::CompareOperator::GtE);
        g.emitPyJumpIfFalse(scratch.get(), *context.fail);
    }
    if (onlyWildcards)
        return;

    if (starIsWildcard) {
        // [first, *_, last]: only the items that matter are fetched.
        for (unsigned i = 0; i < size; ++i) {
            Pattern* item = patterns[i];
            if (isWildcard(item) || static_cast<int>(i) == star)
                continue;
            Reg value = newTemporary();
            loc(node->range);
            if (static_cast<int>(i) < star)
                g.emitPyGetItem(value.get(), subject, constant(jsNumber(i)));
            else {
                // The subject may not take negative indices.
                callRuntime(value.get(), RuntimeFunction::GetLength, { subject });
                g.emitPyBinary(value.get(), value.get(), constant(jsNumber(size - i)), BinaryOperator::Sub);
                g.emitPyGetItem(value.get(), subject, value.get());
            }
            subpattern(item, value.get(), context);
        }
        return;
    }

    if (star >= 0 && (star >= 256 || size - star - 1 >= static_cast<unsigned>(std::numeric_limits<int>::max() >> 8))) {
        fail(node->range, "too many expressions in star-unpacking sequence pattern"_s);
        return;
    }
    Arguments items(*this, size);
    loc(node->range);
    if (star < 0)
        g.emitPyUnpack(items.first(), subject, size, -1);
    else
        g.emitPyUnpack(items.first(), subject, star, size - star - 1);
    for (unsigned i = 0; i < size; ++i)
        subpattern(patterns[i], items[i], context);
}

void Unit::patternMapping(MatchMapping* node, RegisterID* subject, PatternContext& context)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm());
    unsigned size = node->keys.size();
    if (size != node->patterns.size()) {
        fail(node->range, makeString("keys ("_s, size, ") / patterns ("_s, node->patterns.size(), ") length mismatch in mapping pattern"_s));
        return;
    }

    Reg held = subject;
    Reg scratch = newTemporary();
    loc(node->range);
    callRuntime(scratch.get(), RuntimeFunction::MatchMapping, { subject });
    g.emitPyJumpIfFalse(scratch.get(), *context.fail);
    if (!size && !node->rest)
        return;
    if (size) {
        callRuntime(scratch.get(), RuntimeFunction::GetLength, { subject });
        g.emitPyCompare(scratch.get(), scratch.get(), constant(jsNumber(size)), Python::CompareOperator::GtE);
        g.emitPyJumpIfFalse(scratch.get(), *context.fail);
    }

    Reg keys = newTemporary();
    {
        Vector<JSValue, 8> seen;
        Arguments keyValues(*this, size);
        for (unsigned i = 0; i < size; ++i) {
            Expr* key = node->keys[i];
            if (!key) {
                fail(node->patterns[i]->range, "can't use NULL keys in MatchMapping (set 'rest' parameter instead)"_s);
                return;
            }
            JSValue value;
            if (tryConstantValue(key, value)) {
                for (JSValue other : seen) {
                    bool isDuplicate = isEqual(globalObject(), other, value);
                    if (scope.exception()) [[unlikely]]
                        return;
                    if (isDuplicate) {
                        fail(node->range, makeString("mapping pattern checks duplicate key ("_s, reprForMessage(globalObject(), value), ')'));
                        return;
                    }
                }
                seen.append(value);
            } else if (!key->is<Attribute>()) {
                fail(node->range, "mapping pattern keys may only match literals and attribute lookups"_s);
                return;
            }
            expr(key, keyValues[i]);
        }
        loc(node->range);
        callRuntime(keys.get(), RuntimeFunction::BuildTuple, keyValues);
    }

    Reg values = newTemporary();
    callRuntime(values.get(), RuntimeFunction::MatchKeys, { subject, keys.get() });
    g.emitPyIsNone(scratch.get(), values.get());
    g.emitPyJumpIfTrue(scratch.get(), *context.fail);

    if (size) {
        Arguments items(*this, size);
        g.emitPyUnpack(items.first(), values.get(), size, -1);
        for (unsigned i = 0; i < size; ++i)
            subpattern(node->patterns[i], items[i], context);
    }
    if (node->rest) {
        loc(node->range);
        callRuntime(values.get(), RuntimeFunction::CopyDictWithoutKeys, { subject, keys.get() });
        capture(node->rest, values.get(), node->range, context);
    }
}

void Unit::patternClass(MatchClass* node, RegisterID* subject, PatternContext& context)
{
    unsigned positionalCount = node->patterns.size();
    unsigned keywordCount = node->kwdAttrs.size();
    if (keywordCount != node->kwdPatterns.size()) {
        fail(node->range, makeString("kwd_attrs ("_s, keywordCount, ") / kwd_patterns ("_s, node->kwdPatterns.size(), ") length mismatch in class pattern"_s));
        return;
    }
    for (unsigned i = 0; i < keywordCount; ++i) {
        for (unsigned j = i + 1; j < keywordCount; ++j) {
            if (node->kwdAttrs[i]->impl() == node->kwdAttrs[j]->impl()) {
                fail(node->kwdPatterns[j]->range, makeString("attribute name repeated in class pattern: "_s, node->kwdAttrs[i]->string()));
                return;
            }
        }
    }

    Reg held = subject;
    Vector<JSValue, 8> names;
    for (Name name : node->kwdAttrs)
        names.append(g.addStringConstant(*name));

    Reg attributes = newTemporary();
    {
        Arguments arguments(*this, 4);
        g.move(arguments[0], subject);
        expr(node->cls, arguments[1]);
        load(arguments[2], jsNumber(positionalCount));
        load(arguments[3], PyTuple::create(globalObject(), names.span()));
        loc(node->range);
        callRuntime(attributes.get(), RuntimeFunction::MatchClass, arguments);
    }
    {
        Reg isNone = newTemporary();
        g.emitPyIsNone(isNone.get(), attributes.get());
        g.emitPyJumpIfTrue(isNone.get(), *context.fail);
    }

    unsigned total = positionalCount + keywordCount;
    if (!total)
        return;
    Arguments items(*this, total);
    g.emitPyUnpack(items.first(), attributes.get(), total, -1);
    for (unsigned i = 0; i < total; ++i) {
        Pattern* item = i < positionalCount ? node->patterns[i] : node->kwdPatterns[i - positionalCount];
        if (isWildcard(item))
            continue;
        subpattern(item, items[i], context);
    }
}

void Unit::patternOr(MatchOr* node, RegisterID* subject, PatternContext& context)
{
    Reg held = subject;
    Ref<Label> end = g.newLabel();
    unsigned size = node->patterns.size();

    // The alternatives bind the same names, which end up in the registers the first one put them in.
    Vector<PatternContext::Capture, 4> control;
    for (unsigned i = 0; i < size; ++i) {
        Pattern* alternative = node->patterns[i];
        Ref<Label> next = g.newLabel();
        PatternContext inner;
        inner.fail = next.ptr();
        // One that always matches must come last, if it is allowed at all.
        inner.allowIrrefutable = i == size - 1 && context.allowIrrefutable;
        pattern(alternative, subject, inner);
        if (c.failed())
            return;

        if (!i)
            control = WTF::move(inner.captures);
        else {
            bool same = inner.captures.size() == control.size();
            for (auto& wanted : control) {
                if (!same)
                    break;
                size_t index = inner.captures.findIf([&](auto& capture) { return capture.name->impl() == wanted.name->impl(); });
                if (index == notFound) {
                    same = false;
                    break;
                }
                g.move(wanted.reg.get(), inner.captures[index].reg.get());
            }
            if (!same) {
                fail(node->range, "alternative patterns bind different names"_s);
                return;
            }
        }
        g.emitJump(end.get());
        g.emitLabel(next.get());
    }
    g.emitJump(*context.fail);
    g.emitLabel(end.get());

    for (auto& captured : control) {
        for (auto& existing : context.captures) {
            if (existing.name->impl() == captured.name->impl()) {
                fail(node->range, makeString("multiple assignments to name '"_s, captured.name->string(), "' in pattern"_s));
                return;
            }
        }
        context.captures.append(WTF::move(captured));
    }
}

void Unit::pattern(Pattern* node, RegisterID* subject, PatternContext& context)
{
    if (!isSafeToRecurse())
        return;
    switch (node->kind) {
    case PatternKind::MatchValue:
        return patternValue(node->as<MatchValue>(), subject, context);
    case PatternKind::MatchSingleton:
        return patternSingleton(node->as<MatchSingleton>(), subject, context);
    case PatternKind::MatchSequence:
        return patternSequence(node->as<MatchSequence>(), subject, context);
    case PatternKind::MatchMapping:
        return patternMapping(node->as<MatchMapping>(), subject, context);
    case PatternKind::MatchClass:
        return patternClass(node->as<MatchClass>(), subject, context);
    case PatternKind::MatchStar:
        return capture(node->as<MatchStar>()->name, subject, node->range, context);
    case PatternKind::MatchAs:
        return patternAs(node->as<MatchAs>(), subject, context);
    case PatternKind::MatchOr:
        return patternOr(node->as<MatchOr>(), subject, context);
    }
}

void Unit::matchStatement(Match* node, Last isLast)
{
    Reg subject = newTemporary();
    line(node->subject->range.lineno);
    expr(node->subject, subject.get());
    Ref<Label> end = g.newLabel();

    unsigned count = node->cases.size();
    for (unsigned i = 0; i < count; ++i) {
        MatchCase* matchCase = node->cases[i];
        Ref<Label> next = g.newLabel();
        PatternContext context;
        context.fail = next.ptr();
        // A case that always matches must be guarded, or the last.
        context.allowIrrefutable = matchCase->guard || i == count - 1;
        pushBlock({ .kind = BlockKind::MatchSubject, .tryDepth = g.tryDepth() });
        m_statementStart = g.instructions().size();
        line(matchCase->pattern->range.lineno);
        pattern(matchCase->pattern, subject.get(), context);
        for (auto& captured : context.captures)
            storeName(captured.name, captured.reg.get(), matchCase->pattern->range);
        context.captures.clear();
        if (matchCase->guard)
            jumpIf(matchCase->guard, next.get(), false);
        popBlock();
        statements(matchCase->body, 0, isLast);
        leaveStatement(end.get(), isLast, JumpIs::OfTheProgram);
        g.emitLabel(next.get());
        lineOfImplicitReturn(matchCase->pattern->range.lineno, isLast.onlyIf(i + 1 == count));
    }
    endOfStatement(end.get());
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
