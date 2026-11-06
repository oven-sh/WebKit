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

// The match statement. A pattern that fails jumps to whereToFail(). The names a pattern captures are kept in registers, and
// bound only once the whole pattern has matched.
//
// A pattern may be written over several lines, and a tool is told of them as the code comes by. In CPython's code there is more to come
// by: what puts a value on the stack and what takes it off again is of the line its pattern begins on. Where that has an instruction
// and there is none here, line() says so.

static bool isWildcard(Pattern* pattern)
{
    return pattern->kind == PatternKind::MatchAs && !pattern->as<MatchAs>()->name;
}

static bool isStarWildcard(Pattern* pattern)
{
    return pattern->kind == PatternKind::MatchStar && !pattern->as<MatchStar>()->name;
}

Label& Unit::whereToFail(PatternContext& context)
{
    if (context.stillToLookAt || !context.captures.isEmpty())
        return *context.failWithMoreToDrop;
    return context.failsByLine ? labelForLine(*context.failsByLine, m_markedLine) : *context.fail;
}

// The pattern fails unless the condition holds; `isNone`: unless the value is None. Where to fail is to go on with the loop, with nothing to drop first, the branch
// would go backwards: CPython has none that do, and turns it round to jump over a jump.
void Unit::failUnless(RegisterID* condition, PatternContext& context, bool isNone)
{
    auto branch = [&](Label& target, bool ifItHolds) {
        if (isNone)
            g.emitPyJumpIfNone(condition, target, ifItHolds);
        else if (ifItHolds)
            g.emitPyJumpIfTrue(condition, target);
        else
            g.emitPyJumpIfFalse(condition, target);
    };
    if (!context.loopToGoOnWith || context.stillToLookAt || !context.captures.isEmpty())
        return branch(whereToFail(context), false);
    Ref<Label> goesOn = g.newLabel();
    branch(goesOn.get(), true);
    emitImplicitContinue(*context.loopToGoOnWith);
    g.emitLabel(goesOn.get());
}

void Unit::dropWhatPatternLookedAt(const Range& range)
{
    if (m_markedPosition != g.instructions().size())
        m_markedLine = -1;
    line(range);
    m_whereSomethingIsDropped = g.instructions().size();
}

void Unit::capture(Name name, RegisterID* value, const Range& range, PatternContext& context)
{
    if (!name)
        return dropWhatPatternLookedAt(range);
    // It is put underneath whatever else there is.
    if (context.stillToLookAt || !context.captures.isEmpty())
        line(range);
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
    line(node->range);
    ++context.stillToLookAt;
    pattern(node->pattern, subject, context);
    --context.stillToLookAt;
    capture(node->name, subject, node->range, context);
}

void Unit::patternValue(MatchValue* node, RegisterID* subject, PatternContext& context)
{
    JSValue constantValue;
    if (!node->value->is<Attribute>() && !tryConstantValue(node->value, constantValue)) {
        fail(node->range, "patterns may only match literals and attribute lookups"_s);
        return;
    }
    Reg value;
    if (node->value->is<Attribute>())
        value = expr(node->value);
    else {
        // -1 and 1 + 2j are constants from the start here, as in CPython.
        line(node->value->range);
        noteConstant(constantValue);
        value = constant(constantValue);
    }
    Reg result = newTemporary();
    loc(node->range);
    g.emitPyCompare(result.get(), subject, value.get(), Python::CompareOperator::Eq);
    failUnless(result.get(), context);
}

void Unit::patternSingleton(MatchSingleton* node, RegisterID* subject, PatternContext& context)
{
    loc(node->range);
    // (CPython's compiler makes one instruction of a test for None, which needs no constant.)
    noteConstant(constantValue(node->value), node->value.kind != ConstantValue::Kind::None);
    if (node->value.kind == ConstantValue::Kind::None)
        return failUnless(subject, context, true);
    Reg result = newTemporary();
    g.emitPyCompare(result.get(), subject, constant(constantValue(node->value)), Python::CompareOperator::Is);
    failUnless(result.get(), context);
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
    ++context.stillToLookAt;
    callRuntime(scratch.get(), RuntimeFunction::MatchSequence, { subject });
    g.emitPyJumpIfFalse(scratch.get(), whereToFail(context));
    if (star < 0 || size > 1) {
        callRuntime(scratch.get(), RuntimeFunction::GetLength, { subject });
        noteConstant(jsNumber(star < 0 ? size : size - 1));
        if (star < 0)
            g.emitPyCompare(scratch.get(), scratch.get(), constant(jsNumber(size)), Python::CompareOperator::Eq);
        else
            g.emitPyCompare(scratch.get(), scratch.get(), constant(jsNumber(size - 1)), Python::CompareOperator::GtE);
        g.emitPyJumpIfFalse(scratch.get(), whereToFail(context));
    }
    --context.stillToLookAt;
    if (onlyWildcards)
        return dropWhatPatternLookedAt(node->range);

    if (starIsWildcard) {
        // [first, *_, last]: only the items that matter are fetched.
        ++context.stillToLookAt;
        for (unsigned i = 0; i < size; ++i) {
            Pattern* item = patterns[i];
            if (isWildcard(item) || static_cast<int>(i) == star)
                continue;
            Reg value = newTemporary();
            loc(node->range);
            noteConstant(jsNumber(static_cast<int>(i) < star ? i : size - i));
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
        --context.stillToLookAt;
        return dropWhatPatternLookedAt(node->range);
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
    context.stillToLookAt += size;
    for (unsigned i = 0; i < size; ++i) {
        --context.stillToLookAt;
        subpattern(patterns[i], items[i], context);
    }
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
    ++context.stillToLookAt;
    callRuntime(scratch.get(), RuntimeFunction::MatchMapping, { subject });
    g.emitPyJumpIfFalse(scratch.get(), whereToFail(context));
    if (!size && !node->rest) {
        --context.stillToLookAt;
        return dropWhatPatternLookedAt(node->range);
    }
    if (size) {
        callRuntime(scratch.get(), RuntimeFunction::GetLength, { subject });
        noteConstant(jsNumber(size));
        g.emitPyCompare(scratch.get(), scratch.get(), constant(jsNumber(size)), Python::CompareOperator::GtE);
        g.emitPyJumpIfFalse(scratch.get(), whereToFail(context));
    }

    Reg keys = newTemporary();
    {
        Vector<JSValue, 8> seen;
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
        }
        if (PyTuple* tuple = tupleOfConstants(node->keys)) {
            for (unsigned i = 0; i < tuple->length(); ++i)
                noteConstant(tuple->at(i), false);
            noteConstantMade(tuple);
            load(keys.get(), tuple);
        } else {
            Arguments keyValues(*this, size);
            for (unsigned i = 0; i < size; ++i)
                expr(node->keys[i], keyValues[i]);
            loc(node->range);
            callRuntime(keys.get(), RuntimeFunction::BuildTuple, keyValues);
        }
    }

    // The subject, the keys and the values.
    context.stillToLookAt += 2;
    Reg values = newTemporary();
    callRuntime(values.get(), RuntimeFunction::MatchKeys, { subject, keys.get() });
    noteConstant(jsUndefined(), false);
    g.emitPyJumpIfNone(values.get(), whereToFail(context));
    --context.stillToLookAt;

    if (size) {
        Arguments items(*this, size);
        g.emitPyUnpack(items.first(), values.get(), size, -1);
        context.stillToLookAt += size;
        for (unsigned i = 0; i < size; ++i) {
            --context.stillToLookAt;
            subpattern(node->patterns[i], items[i], context);
        }
    }
    context.stillToLookAt -= 2;
    if (!node->rest)
        return dropWhatPatternLookedAt(node->range);
    loc(node->range);
    callRuntime(values.get(), RuntimeFunction::CopyDictWithoutKeys, { subject, keys.get() });
    capture(node->rest, values.get(), node->range, context);
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
        expr(node->cls, arguments[1]);
        g.move(arguments[0], subject);
        load(arguments[2], jsNumber(positionalCount));
        PyTuple* namesTuple = PyTuple::create(globalObject(), names.span());
        noteConstant(namesTuple);
        load(arguments[3], namesTuple);
        loc(node->range);
        callRuntime(attributes.get(), RuntimeFunction::MatchClass, arguments);
    }
    noteConstant(jsUndefined(), false);
    ++context.stillToLookAt;
    g.emitPyJumpIfNone(attributes.get(), whereToFail(context));
    --context.stillToLookAt;

    unsigned total = positionalCount + keywordCount;
    if (!total)
        return dropWhatPatternLookedAt(node->range);
    Arguments items(*this, total);
    g.emitPyUnpack(items.first(), attributes.get(), total, -1);
    context.stillToLookAt += total;
    for (unsigned i = 0; i < total; ++i) {
        --context.stillToLookAt;
        Pattern* item = i < positionalCount ? node->patterns[i] : node->kwdPatterns[i - positionalCount];
        if (isWildcard(item)) {
            dropWhatPatternLookedAt(node->range);
            continue;
        }
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
        Ref<Label> nextWithMoreToDrop = g.newLabel();
        PatternContext inner;
        inner.fail = next.ptr();
        inner.failWithMoreToDrop = nextWithMoreToDrop.ptr();
        // One that always matches must come last, if it is allowed at all.
        inner.allowIrrefutable = i == size - 1 && context.allowIrrefutable;
        line(alternative->range);
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
        line(alternative->range);
        jump(end.get());
        emitFailLabels(alternative->range, *inner.failWithMoreToDrop, *inner.fail);
    }
    dropWhatPatternLookedAt(node->range);
    if (Label& target = whereToFail(context); context.functionEndsWhereItFails && &target != context.failWithMoreToDrop)
        hiddenJump(target);
    else
        jump(target);
    g.emitLabel(end.get());
    dropWhatPatternLookedAt(node->range);

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

void Unit::emitFailLabels(const Range& range, Label& failWithMoreToDrop, Label& fail)
{
    if (failWithMoreToDrop.isBound()) {
        g.emitLabel(failWithMoreToDrop);
        dropWhatPatternLookedAt(range);
    }
    if (fail.isBound())
        g.emitLabel(fail);
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

void Unit::matchStatement(Match* node, const Last& isLast)
{
    Reg subject = newTemporary();
    line(node->subject->range);
    expr(node->subject, subject.get());
    Ref<Label> end = g.newLabel();

    unsigned count = node->cases.size();
    for (unsigned i = 0; i < count; ++i) {
        MatchCase* matchCase = node->cases[i];
        Ref<Label> next = g.newLabel();
        Ref<Label> nextWithMoreToDrop = g.newLabel();
        PatternContext context;
        context.failWithMoreToDrop = nextWithMoreToDrop.ptr();
        bool isLastCase = i + 1 == count;
        // Like a test that fails: see jumpIfOrLeave().
        bool failingBranchesToTarget = isLastCase && isLast.kind == Last::BeforeJump && !isReplacedByCopy(isLast) && canBranchTo(isLast);
        context.fail = failingBranchesToTarget ? isLast.target : next.ptr();
        ReturnsByLine failsByLine;
        if (isLast && isLastCase && !failingBranchesToTarget && matchCase->pattern->range.lineno != (matchCase->guard ? matchCase->guard->range : matchCase->pattern->range).endLineno)
            context.failsByLine = &failsByLine;
        bool failingGoesOnWithLoop = isLastCase && isLast.kind == Last::OfLoopBody && !isLast.isBehindJump;
        if (failingGoesOnWithLoop)
            context.loopToGoOnWith = isLast.loop;
        context.functionEndsWhereItFails = isLastCase && isLast.kind == Last::OfFunction;
        // A case that always matches must be guarded, or the last.
        context.allowIrrefutable = matchCase->guard || i == count - 1;
        pushBlock({ .kind = BlockKind::MatchSubject, .tryDepth = g.tryDepth() });
        m_statementStart = g.instructions().size();
        line(matchCase->pattern->range);
        {
            SetForScope replacedByCopy(m_targetReplacedByCopy, isLastCase ? targetReplacedByCopy(next.get(), isLast) : nullptr);
            pattern(matchCase->pattern, subject.get(), context);
        }
        if (!context.captures.isEmpty())
            line(matchCase->pattern->range);
        for (auto& captured : context.captures)
            storeName(captured.name, captured.reg.get(), matchCase->pattern->range);
        context.captures.clear();
        if (matchCase->guard) {
            SetForScope branchTarget(m_branchTarget, failingGoesOnWithLoop ? BranchTarget { next.ptr(), { }, nullptr, isLast.loop, nullptr }
                : context.failsByLine ? BranchTarget { next.ptr(), { }, context.failsByLine, { }, nullptr } : m_branchTarget);
            jumpIf(matchCase->guard, *context.fail, false);
        }
        popBlock();
        // In CPython's code a jump to the end comes behind every body but that of a `case _:` that is the last of several. After the last case it has
        // nothing to jump over but what drops something where the pattern has failed.
        bool isDefault = isLastCase && count > 1 && isWildcard(matchCase->pattern);
        bool endsWhereStatementEnds = isDefault && !next->isBound();
        m_statementsEndWhereStatementEnds = endsWhereStatementEnds;
        statements(matchCase->body, 0, isDefault ? isLast : lastBeforeJump(end.get(), isLast, JumpIs::ToldOf));
        if (endsWhereStatementEnds)
            continue;
        if (isLast || !isLastCase || nextWithMoreToDrop->isBound())
            leaveStatement(end.get(), isLast, JumpIs::ToldOf);
        else if (m_whereLineOfBranchIsMarked == g.instructions().size()) {
            // What is left of the jump where a copy of the end of the function has taken its place is of the line of the only way there.
            OpNop::emit(&g);
        }
        for (auto& [lineno, label, place] : failsByLine) {
            g.emitLabel(label.get());
            if (isLast.kind != Last::BeforeJump || isLast.jumpIs >= JumpIs::ToldOf)
                line(lineno);
            leave(isLast);
        }
        // (A pattern that always matches.)
        if (!next->isBound() && !nextWithMoreToDrop->isBound())
            continue;
        emitFailLabels(matchCase->pattern->range, nextWithMoreToDrop.get(), next.get());
        if (isLast.kind == Last::BeforeJump && isLastCase) {
            // (Unless other ways meet here: see endOfStatement(). In CPython the body jumps here, over what drops something, and then a copy of the end of the
            // function, which has lines of its own, does not take the line of either way.)
            m_whereLineOfBranchIsMarked = g.instructions().size();
            if (nextWithMoreToDrop->isBound() && isReplacedByCopy(isLast))
                noLine();
            else
                silentLine(matchCase->pattern->range.lineno);
        }
        else if (isLast && isLastCase)
            line(matchCase->pattern->range);
    }
    endOfStatement(end.get());
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
