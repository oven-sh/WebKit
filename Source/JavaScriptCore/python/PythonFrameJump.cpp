/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "config.h"
#include "PyFrame.h"

#include "BytecodeGenerator.h"
#include "BytecodeLivenessAnalysisInlines.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "Exception.h"
#include "JSCInlines.h"
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "PythonMonitoring.h"
#include "PythonOperations.h"
#include "PythonText.h"

// frame.f_lineno = n, which is how a debugger has a program go on from some other line. It is frame_lineno_set_impl() of CPython's Objects/frameobject.c.
//
// CPython works out, for each instruction, what kinds of thing are on its stack when that is come to: the iterator of each `for` that it is in, what was being handled before each `except`, and so on. It can go from
// one place to another if what the other needs is at the bottom of what there is, kind for kind, and it drops the rest. So it can leave anything, and go into nothing, but it can go from the body of one loop to that
// of the next, which then goes on with the iterator of the first. If there are several places on the line, it goes to the one that keeps the most, and of those that keep the same, to the first.
//
// Here those things are in registers, and each loop has a register of its own. The compiler says of each part of the code what CPython would have on its stack there and which registers that is in
// (CodeDetails::JumpBlock), and each op_py_line says which part it is in. Whether it can be done is decided as CPython decides it, and then what is needed is copied from the registers of where it is to those of
// where it is going.
//
// CPython has several copies of what comes after `finally`, and of the call of __exit__: one for each `return`, `break` and `continue` that leaves what came before, one for coming to the end of that, and one for
// an exception. They have different things on the stack, and it can go from one copy into another. Here there is one copy, and a register that says how it was come to, so to be in one copy or another is for that
// register to say one thing or another: a Way.

namespace JSC {

using namespace Python;

namespace {

using Block = CodeDetails::JumpBlock;

// The kinds of thing that CPython tells apart, and how it writes down a stack of them: three bits each, the top in the lowest.
enum class StackKind : uint8_t { Iterator = 1, Except = 2, Object = 3 };
constexpr int64_t overflowed = -1;
constexpr unsigned bitsPerEntry = 3;
constexpr unsigned maximumEntries = 63 / bitsPerEntry;

// How a part of the code was come to, if there is more than one way. 0 is by coming to the end of what is before it, 1 by an exception, and after that by one of Block::leaves.
using Way = unsigned;
constexpr Way fallenInto = 0;
constexpr Way thrown = 1;
constexpr Way firstLeave = 2;

struct Entry {
    StackKind kind;
    unsigned block; // Which it is of, counting from 1.
    unsigned index; // Which of those of the block it is.
    Way way;

    friend bool operator==(const Entry&, const Entry&) = default;
};
using Stack = Vector<Entry, 8>;

int64_t encode(const Stack& stack)
{
    if (stack.size() > maximumEntries)
        return overflowed;
    int64_t result = 0;
    for (auto& entry : stack)
        result = result << bitsPerEntry | static_cast<int64_t>(entry.kind);
    return result;
}

bool isCompatibleKind(int64_t from, int64_t to)
{
    if (!to)
        return false;
    if (to == static_cast<int64_t>(StackKind::Object))
        return true;
    return from == to;
}

// compatible_stack()
bool isCompatible(int64_t from, int64_t to)
{
    if (from < 0 || to < 0)
        return false;
    constexpr int64_t top = (1 << bitsPerEntry) - 1;
    while (from > to)
        from >>= bitsPerEntry;
    while (from) {
        if (!isCompatibleKind(from & top, to & top))
            return false;
        from >>= bitsPerEntry;
        to >>= bitsPerEntry;
    }
    return !to;
}

// explain_incompatible_stack()
ASCIILiteral explain(const Stack& to)
{
    if (to.size() > maximumEntries)
        return "stack is too deep to analyze"_s;
    switch (to.last().kind) {
    case StackKind::Except:
        return "can't jump into an 'except' block as there's no exception"_s;
    case StackKind::Object:
        return "incompatible stacks"_s;
    case StackKind::Iterator:
        return "can't jump into the body of a for loop"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool hasWays(const Block& block) { return block.kind == Block::Kind::Finally || block.kind == Block::Kind::WithExit; }
bool isWith(const Block& block) { return block.kind == Block::Kind::With || block.kind == Block::Kind::WithExit; }
bool keepsValue(const Block& block, Way way) { return way >= firstLeave && block.leaves[way - firstLeave].keepsValue; }

void appendEntries(Stack& stack, const Vector<Block>& blocks, unsigned index, Way way)
{
    const Block& block = blocks[index - 1];
    auto append = [&] (StackKind kind, unsigned which) { stack.append({ kind, index, which, way }); };
    switch (block.kind) {
    case Block::Kind::Loop:
    case Block::Kind::ComprehensionLoop:
        append(StackKind::Iterator, 0);
        return;
    case Block::Kind::LoopEnd:
        append(StackKind::Iterator, 0);
        append(StackKind::Object, 1);
        return;
    case Block::Kind::Comprehension:
    case Block::Kind::With:
    case Block::Kind::WithExit:
        // The method, and what it is a method of.
        append(StackKind::Object, 0);
        append(StackKind::Object, 1);
        if (keepsValue(block, way))
            append(StackKind::Object, 2);
        return;
    case Block::Kind::Handler:
    case Block::Kind::Matching:
        append(StackKind::Except, 0);
        return;
    case Block::Kind::Finally:
        if (way == thrown) {
            append(StackKind::Except, 0);
            append(StackKind::Except, 1);
        } else if (keepsValue(block, way))
            append(StackKind::Object, 0);
        return;
    case Block::Kind::Protected:
    case Block::Kind::Scope:
        return;
    case Block::Kind::Subject:
        append(StackKind::Object, 0);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// From the outermost to the innermost.
using Chain = Vector<unsigned, 8>;
Chain chainOf(const Vector<Block>& blocks, unsigned block)
{
    Chain chain;
    for (; block; block = blocks[block - 1].parent)
        chain.append(block);
    chain.reverse();
    return chain;
}

// The ways that there are to be in a part of the code, in the order that CPython has its copies in: those that have to do with exceptions are after everything else.
Vector<Way, 4> waysOf(const Block& block)
{
    Vector<Way, 4> ways;
    if (!hasWays(block)) {
        ways.append(fallenInto);
        return ways;
    }
    for (bool isInHandler : { false, true }) {
        for (unsigned i = 0; i < block.leaves.size(); ++i) {
            if (block.leaves[i].isInHandler == isInHandler)
                ways.append(firstLeave + i);
        }
        if (!isInHandler && block.isFallenInto)
            ways.append(fallenInto);
    }
    if (block.kind == Block::Kind::Finally)
        ways.append(thrown);
    return ways;
}

struct Place {
    unsigned line;
    unsigned offset; // Of the op_py_line, or the op_py_enter.
    unsigned next; // Of what comes after it.
    unsigned block;
    bool canBeGoneOnFrom;
};

} // anonymous namespace

void PyFrame::goOnFromLine(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    MonitoringState& monitoring = realm->monitoring();

    if (!monitoring.eventBeingTold) {
        raiseValueError(globalObject, scope, "f_lineno can only be set in a trace function"_s);
        return;
    }
    switch (*monitoring.eventBeingTold) {
    case MonitoringEvent::Line:
    case MonitoringEvent::Jump:
        break;
    case MonitoringEvent::PyStart:
        raiseValueError(globalObject, scope, "can't jump from the 'call' trace event of a new frame"_s);
        return;
    case MonitoringEvent::Call:
    case MonitoringEvent::CReturn:
        raiseValueError(globalObject, scope, "can't jump during a call"_s);
        return;
    case MonitoringEvent::Reraise:
    case MonitoringEvent::StopIteration:
    case MonitoringEvent::Branch:
        raise(globalObject, scope, BuiltinType::SystemError, "unexpected event type"_s);
        return;
    default:
        // CPython can do it as a generator yields or is resumed, and at a branch.
        raiseValueError(globalObject, scope, "can only jump from a 'line' trace event"_s);
        return;
    }
    // CPython asks no more than that something is being told. But a frame further out is in the middle of a call, and there is nowhere for it to go on from but where that returns to.
    if (state() != State::Running || callFrame(vm) != monitoring.frameBeingToldOf || !monitoring.isToldFromLine) {
        raiseValueError(globalObject, scope, "can't jump during a call"_s);
        return;
    }
    CallFrame* callFrame = monitoring.frameBeingToldOf;

    if (!value.isInt32()) {
        raiseValueError(globalObject, scope, "lineno out of range"_s);
        return;
    }
    int given = value.asInt32();
    const FunctionInfo& info = functionInfo();
    unsigned firstLine = info.firstLine + info.lineDelta;
    if (given < static_cast<int>(firstLine)) {
        raiseValueError(globalObject, scope, concatenate("line "_s, String::number(given), " comes before the current code block"_s));
        return;
    }

    // Where each line begins.
    CodeBlock* codeBlock = callFrame->codeBlock();
    const Vector<Block>& blocks = details().jumpBlocks;
    Vector<Place, 32> places;
    for (const auto& instruction : codeBlock->instructions()) {
        unsigned offset = instruction.offset();
        unsigned next = offset + instruction->size();
        if (instruction->is<OpPyEnter>()) {
            if (!instruction->as<OpPyEnter>().m_isResume)
                places.append({ firstLine, offset, next, 0, true });
            continue;
        }
        if (!instruction->is<OpPyLine>())
            continue;
        auto bytecode = instruction->as<OpPyLine>();
        auto kind = static_cast<LineKind>(bytecode.m_kind);
        if (kind != LineKind::Line && kind != LineKind::WhereItCanBeGoneOnFrom)
            continue;
        places.append({ lineAt(vm, BytecodeIndex(offset)), offset, next, bytecode.m_block, kind == LineKind::WhereItCanBeGoneOnFrom });
    }

    // first_line_not_before()
    unsigned line = std::numeric_limits<unsigned>::max();
    for (auto& place : places) {
        if (place.line >= static_cast<unsigned>(given))
            line = std::min(line, place.line);
    }
    if (line == std::numeric_limits<unsigned>::max()) {
        raiseValueError(globalObject, scope, concatenate("line "_s, String::number(given), " comes after the current code block"_s));
        return;
    }

    // What there is where it is, which is where it was last sent, if it has been.
    unsigned fromOffset = m_goesOnFrom ? m_goesOnFrom->offset : monitoring.offsetBeingToldOf;
    auto fromInstruction = codeBlock->instructions().at(fromOffset);
    Chain fromChain = chainOf(blocks, fromInstruction->is<OpPyLine>() ? fromInstruction->as<OpPyLine>().m_block : 0);
    auto read = [&] (VirtualRegister location) { return callFrame->r(location).jsValue(); };
    Vector<Way, 8> fromWays;
    Stack from;
    for (unsigned index : fromChain) {
        const Block& block = blocks[index - 1];
        Way way = fallenInto;
        if (hasWays(block)) {
            bool isFinally = block.kind == Block::Kind::Finally;
            JSValue completion = read(isFinally ? block.first : block.third);
            JSValue completionValue = read(isFinally ? block.second : block.fourth);
            int type = completion.isInt32() ? completion.asInt32() : static_cast<int>(CompletionType::Normal);
            if (type == static_cast<int>(CompletionType::Throw))
                way = isFinally ? thrown : fallenInto;
            else if (type != static_cast<int>(CompletionType::Normal)) {
                // Which `return` it was is not kept. It is one that returns what is being returned.
                std::optional<Way> found;
                for (unsigned i = 0; i < block.leaves.size(); ++i) {
                    auto& leave = block.leaves[i];
                    if (leave.completionType != type)
                        continue;
                    if (!found || (!leave.keepsValue && leave.value.isValid() && read(leave.value) == completionValue && keepsValue(block, *found)))
                        found = firstLeave + i;
                }
                way = found.value_or(fallenInto);
            }
        }
        fromWays.append(way);
        appendEntries(from, blocks, index, way);
    }
    int64_t fromStack = encode(from);

    // What each thing that CPython would have on its stack is.
    JSValue marker = realm->boundArgumentsMarker();
    auto valueOf = [&] (const Entry& entry) -> JSValue {
        const Block& block = blocks[entry.block - 1];
        switch (block.kind) {
        case Block::Kind::Loop:
        case Block::Kind::ComprehensionLoop:
        case Block::Kind::Handler:
        case Block::Kind::Subject:
            return read(block.first);
        case Block::Kind::With:
        case Block::Kind::WithExit:
            return !entry.index ? read(block.first) : entry.index == 2 ? read(block.fourth) : JSValue();
        case Block::Kind::Matching:
            return read(blocks[block.parent - 1].second);
        case Block::Kind::Finally:
            return read(entry.index || entry.kind == StackKind::Object ? block.second : block.third);
        case Block::Kind::LoopEnd:
        case Block::Kind::Comprehension:
        case Block::Kind::Protected:
        case Block::Kind::Scope:
            return { };
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    // CPython goes by the kind alone, and will take an iterator for the __exit__ of a `with`. Here what is of one part of the code can be given only to another part of the same sort.
    auto canBeGiven = [&] (const Entry& given, const Entry& to) {
        if (given == to)
            return true;
        const Block& block = blocks[to.block - 1];
        // Nothing is made of what there is here.
        if (block.kind == Block::Kind::LoopEnd)
            return true;
        switch (to.kind) {
        case StackKind::Iterator:
            return block.kind == Block::Kind::Loop;
        case StackKind::Except:
            // What is being handled has to be something, where what was being handled before need not be.
            return block.kind == Block::Kind::Handler || (block.kind == Block::Kind::Finally && !to.index) || valueOf(given) != marker;
        case StackKind::Object:
            if (block.kind == Block::Kind::Subject)
                return blocks[given.block - 1].kind == Block::Kind::Subject;
            return isWith(block) && isWith(blocks[given.block - 1]) && given.index == to.index && to.index < 2;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    // The best of the places on that line: the one that keeps the most, and the first of those that keep as much.
    struct Choice {
        const Place* place { nullptr };
        Stack stack;
        Chain chain;
        Vector<Way, 8> ways;
        int64_t encoded { overflowed };
    } best;
    bool isFound = false;
    std::optional<ASCIILiteral> objection;
    for (auto& place : places) {
        if (place.line != line)
            continue;
        if (!place.canBeGoneOnFrom) {
            // It is in the middle of something, of which only the compiler knows what has been worked out so far. It can stay where it is.
            if (place.offset == fromOffset) {
                isFound = true;
                if (fromStack > best.encoded)
                    best = { &place, from, fromChain, fromWays, fromStack };
            } else if (!isFound && !objection) {
                // CPython goes by what is on top.
                Block::Kind innermost = place.block ? blocks[place.block - 1].kind : Block::Kind::Subject;
                objection = innermost == Block::Kind::ComprehensionLoop || innermost == Block::Kind::Comprehension ? "can't jump into the body of a for loop"_s
                    : innermost == Block::Kind::Matching ? "can't jump into an 'except' block as there's no exception"_s : "incompatible stacks"_s;
            }
            continue;
        }
        Chain chain = chainOf(blocks, place.block);
        bool isComeTo = true;
        for (unsigned index : chain)
            isComeTo = isComeTo && (blocks[index - 1].kind != Block::Kind::Handler || blocks[index - 1].isFallenInto);
        if (!isComeTo) {
            if (!isFound && !objection)
                objection = "can't jump into an exception handler, or code may be unreachable"_s;
            continue;
        }
        // Each way of being in each part of the code that it is in, but that as soon as there is more than there is to give it, or something else, there is no going on with that.
        Vector<Way, 8> ways;
        Stack to;
        bool isFirst = true;
        auto consider = [&] (auto& consider, unsigned depth) -> void {
            if (depth == chain.size()) {
                int64_t encoded = encode(to);
                bool can = isCompatible(fromStack, encoded);
                for (unsigned i = 0; can && i < to.size(); ++i)
                    can = canBeGiven(from[i], to[i]);
                if (can) {
                    isFound = true;
                    if (encoded > best.encoded)
                        best = { &place, to, chain, ways, encoded };
                } else if (isFirst && !isFound && !objection)
                    objection = fromStack == overflowed ? "stack to deep to analyze"_s : explain(to);
                isFirst = false;
                return;
            }
            const Block& block = blocks[chain[depth] - 1];
            bool hasTriedWithNothing = false;
            for (Way way : waysOf(block)) {
                unsigned size = to.size();
                appendEntries(to, blocks, chain[depth], way);
                // Of those that have nothing on the stack, the first is the one.
                bool isWorthTrying = to.size() > size || !hasWays(block) || !std::exchange(hasTriedWithNothing, true);
                // The first of all is gone through with whatever comes of it, since what is wrong with it is what is said if nothing can be done.
                for (unsigned i = size; isWorthTrying && !isFirst && i < to.size(); ++i)
                    isWorthTrying = i < from.size() && isCompatibleKind(static_cast<int64_t>(from[i].kind), static_cast<int64_t>(to[i].kind));
                if (isWorthTrying) {
                    ways.append(way);
                    consider(consider, depth + 1);
                    ways.removeLast();
                }
                to.shrink(size);
            }
        };
        consider(consider, 0);
    }
    if (!isFound) {
        raiseValueError(globalObject, scope, objection.value_or("cannot find bytecode for specified line"_s));
        return;
    }

    // What is to be done is all worked out before any of it is done, so that if it is not to be done after all nothing has been.
    Vector<std::pair<VirtualRegister, JSValue>, 16> writes;
    auto write = [&] (VirtualRegister location, JSValue written) {
        if (location.isValid())
            writes.append({ location, written });
    };
    Vector<JSValue, 8> kept;
    for (unsigned i = 0; i < best.stack.size(); ++i)
        kept.append(valueOf(from[i]));
    // What is dropped, from the top. What was being handled before an `except` is being handled again.
    JSValue handled = realm->ownHandledException() ? JSValue(realm->ownHandledException()) : marker;
    for (unsigned i = from.size(); i-- > best.stack.size();) {
        if (from[i].kind == StackKind::Except)
            handled = valueOf(from[i]);
    }
    auto setThrown = [&] (const Block& handler, JSValue exception) {
        write(handler.second, exception);
        write(handler.third, uncheckedDowncast<Exception>(exception.asCell())->value());
    };
    for (unsigned i = 0; i < best.stack.size(); ++i) {
        const Entry& to = best.stack[i];
        if (to == from[i])
            continue;
        const Block& block = blocks[to.block - 1];
        switch (block.kind) {
        case Block::Kind::Loop:
        case Block::Kind::Subject:
            write(block.first, kept[i]);
            break;
        case Block::Kind::With:
        case Block::Kind::WithExit:
            if (!to.index)
                write(block.first, kept[i]);
            break;
        case Block::Kind::Handler:
            write(block.first, kept[i]);
            if (handled != marker)
                setThrown(block, handled);
            break;
        case Block::Kind::Matching:
            setThrown(blocks[block.parent - 1], kept[i]);
            break;
        case Block::Kind::Finally:
            write(to.index ? block.second : block.third, kept[i]);
            break;
        case Block::Kind::LoopEnd:
        case Block::Kind::Comprehension:
        case Block::Kind::ComprehensionLoop:
        case Block::Kind::Protected:
        case Block::Kind::Scope:
            break;
        }
    }
    // And what says how each part of the code was come to.
    unsigned common = 0;
    while (common < best.chain.size() && common < fromChain.size() && best.chain[common] == fromChain[common])
        ++common;
    unsigned entriesSoFar = 0;
    for (unsigned i = 0; i < best.chain.size(); ++i) {
        const Block& block = blocks[best.chain[i] - 1];
        Way way = best.ways[i];
        while (entriesSoFar < best.stack.size() && best.stack[entriesSoFar].block == best.chain[i])
            ++entriesSoFar;
        if (i < common && way == fromWays[i])
            continue;
        auto completion = [&] (VirtualRegister type, VirtualRegister completionValue) {
            if (way == fallenInto) {
                write(type, jsNumber(static_cast<int>(CompletionType::Normal)));
                write(completionValue, JSValue());
                return;
            }
            auto& leave = block.leaves[way - firstLeave];
            write(type, jsNumber(leave.completionType));
            write(completionValue, leave.value.isValid() ? read(leave.value) : JSValue());
        };
        switch (block.kind) {
        case Block::Kind::WithExit:
            completion(block.third, block.fourth);
            [[fallthrough]];
        case Block::Kind::With:
            write(block.second, jsBoolean(true));
            break;
        case Block::Kind::Finally: {
            if (way == thrown) {
                write(block.first, jsNumber(static_cast<int>(CompletionType::Throw)));
                break;
            }
            completion(block.first, block.second);
            // When it is over, what is being handled is to be what it will be by then: what was being handled before the first `except` that this is going into, in here.
            JSValue afterwards = handled;
            for (unsigned entry = entriesSoFar; entry < best.stack.size(); ++entry) {
                if (best.stack[entry].kind == StackKind::Except) {
                    afterwards = kept[entry];
                    break;
                }
            }
            write(block.third, afterwards);
            break;
        }
        case Block::Kind::Protected:
            write(block.first, jsNumber(static_cast<int>(CompletionType::Normal)));
            write(block.second, JSValue());
            break;
        default:
            break;
        }
    }

    // An environment that it is no longer in.
    {
        JSScope* environment = callFrame->scope(details().scopeRegister.offset());
        bool isLeft = false;
        for (unsigned i = fromChain.size(); i-- > common;) {
            if (blocks[fromChain[i] - 1].kind != Block::Kind::Scope)
                continue;
            environment = environment->next();
            isLeft = true;
        }
        if (isLeft)
            write(details().scopeRegister, environment);
    }

    // A generator that is resumed is given back only what could have been made use of from where it left off, as the code is written. So what is found once, when it begins, is found again. As for the rest,
    // what is made use of from where this is going has to be something that could be made use of from here as well, which is then as it should be, or something that has just been seen to.
    if (info.isGeneratorBody) {
        write(details().globalsRegister, globalsOfScope(vm, m_function->scope()));
        write(details().builtinsRegister, builtinsOfScope(vm, m_function->scope()));
        write(details().namespaceRegister, details().namespaceRegister.isValid() ? namespaceArgument(vm, callFrame) : JSValue());
        auto& liveness = codeBlock->livenessAnalysis();
        FastBitVector isNeeded = liveness.getLivenessInfoAtInstruction(codeBlock, BytecodeIndex(best.place->next));
        FastBitVector isThere = liveness.getLivenessInfoAtInstruction(codeBlock, BytecodeIndex(fromOffset));
        for (auto& [location, written] : writes) {
            if (location.isLocal() && static_cast<size_t>(location.toLocal()) < isThere.numBits())
                isThere[location.toLocal()] = true;
        }
        // These are kept whatever comes of them.
        for (auto& variable : details().frameVariables) {
            if (variable.location.isValid() && variable.location.isLocal())
                isThere[variable.location.toLocal()] = true;
        }
        isThere[details().scopeRegister.toLocal()] = true;
        bool isAllThere = true;
        isNeeded.forEachSetBit([&] (size_t local) {
            isAllThere = isAllThere && isThere[local];
        });
        if (!isAllThere) {
            raiseValueError(globalObject, scope, "incompatible stacks"_s);
            return;
        }
    }

    // The compiler may have made sure that a variable has a value where this is going, and left out looking. So they are all given one.
    unsigned unbound = 0;
    for (auto& variable : details().frameVariables) {
        if (variable.location.isValid() && !read(variable.location)) {
            write(variable.location, jsUndefined());
            ++unbound;
        }
    }
    if (unbound) {
        warn(globalObject, BuiltinType::RuntimeWarning, concatenate("assigning None to "_s, String::number(unbound), " unbound local"_s, unbound == 1 ? ""_s : "s"_s), 0);
        RETURN_IF_EXCEPTION(scope, void());
    }

    realm->setOwnHandledException(vm, handled == marker ? nullptr : uncheckedDowncast<Exception>(handled.asCell()));
    for (auto& [location, written] : writes)
        callFrame->uncheckedR(location) = written;
    m_goesOnFrom = GoesOnFrom { best.place->offset, best.place->next, line };
}

} // namespace JSC
