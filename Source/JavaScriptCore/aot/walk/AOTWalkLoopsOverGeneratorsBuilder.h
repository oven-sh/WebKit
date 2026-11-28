/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTIRProgram.h"
#include "AOTIterationHelpers.h"
#include "AOTWalk.h"
#include <array>
#include <initializer_list>
#include <optional>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

bool canCallCompilerHelper(Walker&, FunctionID helper);
Reference callCompilerHelper(Walker&, FunctionID helper, std::span<const Reference> arguments, Origin);
OperatorForm operatorFormFor(Walker&, const Draft&);
Reference boxNumberIfNeeded(Walker&, Reference, Origin);

class BuilderForGenerators {
    WTF_MAKE_NONCOPYABLE(BuilderForGenerators);
public:
    struct MeetingOfStep {
        BlockID block;
        Reference done;
        Reference value;
    };

    BuilderForGenerators(Walker& walker, Origin origin)
        : m_walker(walker)
        , m_origin(origin)
    {
        m_origin.isTailCall = false;
    }

    Origin origin() const { return m_origin; }
    Draft draft(Opcode opcode) const { return Draft(opcode, m_origin); }

    Reference build(Draft&& operation)
    {
        if (m_walker.isUnreachable())
            return { };
        m_walker.chooseConservativeFields(operation);
        return m_walker.build(WTF::move(operation));
    }

    Reference buildAsItIs(Draft&& operation)
    {
        return m_walker.isUnreachable() ? Reference() : m_walker.build(WTF::move(operation));
    }

    Reference constant(Draft&& written)
    {
        written.representation = Representation::JSValue;
        return m_walker.constant(WTF::move(written));
    }

    Reference undefined() { return constant(draft(Opcode::Undefined)); }

    Reference boolean(bool value)
    {
        Draft written = draft(Opcode::Boolean);
        written.setBoolean(value);
        return constant(WTF::move(written));
    }

    Reference integer(int64_t value)
    {
        Draft written = draft(Opcode::Integer);
        written.setInteger(value);
        return m_walker.constant(WTF::move(written));
    }

    Reference read(Reference object, PropertyKeyID key)
    {
        Draft access = draft(Opcode::PropGet);
        access.setObjectOperand(object);
        access.setKey(key);
        return build(WTF::move(access));
    }

    Reference read(Reference object, BuiltinName name) { return read(object, m_walker.program().keyOf(name)); }

    Reference call(Reference method, Reference receiver)
    {
        Draft invocation = draft(Opcode::Call);
        invocation.setCalleeOperand(method);
        invocation.setThisValueOperand(receiver);
        return build(WTF::move(invocation));
    }

    FunctionID findHelper(IterationHelper helper)
    {
        if (m_walker.isUnreachable())
            return { };
        FunctionID function = functionOfIterationHelper(m_walker.program(), helper);
        return canCallCompilerHelper(m_walker, function) ? function : FunctionID();
    }

    Reference callHelper(FunctionID helper, std::initializer_list<Reference> arguments)
    {
        return m_walker.isUnreachable() ? Reference() : callCompilerHelper(m_walker, helper, std::span<const Reference>(arguments.begin(), arguments.size()), m_origin);
    }

    Reference testType(Reference value, TypeID tested)
    {
        Draft test = draft(Opcode::TypeTest);
        test.setValueOperand(value);
        test.setTestedType(tested);
        return build(WTF::move(test));
    }

    Reference requireObject(Reference value, RuntimeErrorKind error)
    {
        Draft check = draft(Opcode::RequireObject);
        check.setValueOperand(value);
        check.setRuntimeError(error);
        return build(WTF::move(check));
    }

    Reference resultAt(Reference results, uint32_t index)
    {
        Draft result = draft(Opcode::ResultAt);
        result.setResultsOperand(results);
        result.setIndex(index);
        return build(WTF::move(result));
    }

    Reference resultsOf(Reference done, Reference value)
    {
        Draft results = draft(Opcode::Results);
        results.setValuesOperands(std::array { done, value });
        return build(WTF::move(results));
    }

    Reference isInteger(Reference number, int64_t asked)
    {
        if (m_walker.isUnreachable())
            return { };
        Draft operation = draft(Opcode::StrictEq);
        m_walker.chooseConservativeFields(operation);
        operation.setOperand(0, number);
        operation.setOperand(1, integer(asked));
        operation.setForm(operatorFormFor(m_walker, operation));
        return boxNumberIfNeeded(m_walker, m_walker.build(WTF::move(operation)), m_origin);
    }

    BlockID addBlock(BlockFrequency frequency = BlockFrequency::Normal) { return m_walker.addBlock(frequency); }
    void branch(Reference condition, BlockID taken, BlockID notTaken) { m_walker.branch(condition, taken, notTaken); }

    void continueIn(BlockID block)
    {
        m_walker.continueIn(block);
        standUnderHandler();
    }

    void jumpTo(BlockID block, std::initializer_list<Reference> arguments = { })
    {
        m_walker.jumpTo(block, std::span<const Reference>(arguments.begin(), arguments.size()));
        standUnderHandler();
    }

    Reference narrowed(Reference value, Reference test, uint32_t operandIndex)
    {
        Draft proof = draft(Opcode::Narrowed);
        proof.setValueOperand(value);
        proof.setTestOperand(test);
        proof.setOperandIndex(operandIndex);
        proof.setIsOnTakenEdge(true);
        return buildAsItIs(WTF::move(proof));
    }

    std::optional<PassedTest> continueIfCheckPasses(Reference test, BlockID otherwise)
    {
        std::optional<PassedTest> passed = m_walker.continueIfCheckPasses(test, otherwise);
        standUnderHandler();
        return passed;
    }

    Reference continueIfTypeMatches(Reference value, TypeID tested, BlockID otherwise)
    {
        Reference test = testType(value, tested);
        BlockID passed = addBlock();
        branch(test, passed, otherwise);
        continueIn(passed);
        return narrowed(value, test, OperandIndex::TypeTest::value);
    }

    void buildThrowingOperation(Draft&& throwing)
    {
        build(WTF::move(throwing));
        if (!m_walker.isUnreachable())
            m_walker.code().end(m_walker.currentBlock(), TerminatorDraft(TerminatorKind::Unreachable, m_origin), 0);
    }

    void throwError(RuntimeErrorKind error)
    {
        Draft check = draft(Opcode::RequireObject);
        check.setValueOperand(undefined());
        check.setRuntimeError(error);
        buildThrowingOperation(WTF::move(check));
    }

    void beginToDropWhatIsThrown()
    {
        Code& code = m_walker.code();
        m_handler = code.addBlock(BlockRole::CatchHandler, BlockFrequency::Rare);
        code.setLoopMarks(m_handler, m_walker.isInLoop(), false);
        jumpTo(addBlock());
    }

    void dropWhatWasThrown(BlockID after)
    {
        BlockID handler = std::exchange(m_handler, BlockID());
        jumpTo(after);
        continueIn(handler);
        m_walker.state().clear();
        build(draft(Opcode::CatchValue));
        jumpTo(after);
    }

    MeetingOfStep openMeetingOfStep()
    {
        MeetingOfStep meeting;
        meeting.block = addBlock();
        meeting.done = m_walker.addParameter(meeting.block, { }, m_walker.formOfParameter(TypeTable::boolean, { }));
        meeting.value = m_walker.addParameter(meeting.block, { }, Representation::JSValue);
        return meeting;
    }

    void endStep(Reference done, Reference value, const MeetingOfStep& after)
    {
        BlockID last = addBlock();
        BlockID further = addBlock();
        branch(done, last, further);
        continueIn(last);
        jumpTo(after.block, { boolean(true), undefined() });
        continueIn(further);
        jumpTo(after.block, { boolean(false), value });
    }

    void readAnswerOfStep(Reference answer, const MeetingOfStep& after)
    {
        Draft conversion = draft(Opcode::ToBoolean);
        conversion.setValueOperand(read(answer, BuiltinNames::done));
        Reference done = build(WTF::move(conversion));

        BlockID last = addBlock();
        BlockID further = addBlock();
        branch(done, last, further);
        continueIn(last);
        jumpTo(after.block, { boolean(true), undefined() });
        continueIn(further);
        Reference value = read(answer, BuiltinNames::value);
        jumpTo(after.block, { boolean(false), value });
    }

    Reference openByProtocol(Reference iterable)
    {
        Draft check = draft(Opcode::RequireObjectCoercible);
        check.setValueOperand(iterable);
        check.setNullishAccess(NullishAccess::Iterate);
        iterable = build(WTF::move(check));

        Reference iterator;
        if (FunctionID opens = findHelper(IterationHelper::OpenIterator)) {
            iterator = callHelper(opens, { iterable });
            BlockID opened = addBlock();
            BlockID lacksMethod = addBlock(BlockFrequency::Rare);
            branch(testType(iterator, TypeTable::undefined), lacksMethod, opened);
            continueIn(lacksMethod);
            throwNotIterable(iterable);
            continueIn(opened);
        } else {
            Reference method = read(iterable, m_walker.types().wellKnownSymbolKey(WellKnownSymbol::Iterator));
            BlockID callable = addBlock();
            BlockID missing = addBlock(BlockFrequency::Rare);
            branch(testType(method, TypeTable::function), callable, missing);
            continueIn(missing);
            throwNotIterable(iterable);
            continueIn(callable);
            iterator = requireObject(call(method, iterable), RuntimeErrorKind::IteratorNotObject);
        }
        Reference next = read(iterator, BuiltinNames::next);

        Draft words = draft(Opcode::NewArray);
        words.standsForNoValueOfInput = true;
        words.setContents(TypeTable::anything);
        words.setElementsOperands(std::array { iterator, next });
        return build(WTF::move(words));
    }

    Reference takeWordsOfProtocol(Reference parameter, Reference listAsMade)
    {
        m_listOfWordsAsMade = listAsMade;
        Reference test = testType(parameter, m_walker.types().builtinClass(BuiltinClasses::Array));
        BlockID holdsList = addBlock();
        BlockID never = addBlock(BlockFrequency::Rare);
        branch(test, holdsList, never);
        continueIn(never);
        if (!m_walker.isUnreachable())
            m_walker.code().end(never, TerminatorDraft(TerminatorKind::Unreachable, m_origin), 0);
        continueIn(holdsList);
        return narrowed(parameter, test, OperandIndex::TypeTest::value);
    }

    Reference iteratorIn(Reference words) { return readWord(words, 0); }
    Reference nextIn(Reference words) { return readWord(words, 1); }

    void stepByProtocol(Reference iterator, Reference next, const MeetingOfStep& after)
    {
        if (FunctionID steps = findHelper(IterationHelper::StepIterator)) {
            Reference stepValue = callHelper(steps, { iterator, next });
            Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
            isClosed.setTableOperand(stepValue);
            endStep(build(WTF::move(isClosed)), stepValue, after);
            return;
        }
        readAnswerOfStep(requireObject(call(next, iterator), RuntimeErrorKind::IteratorResultNotObject), after);
    }

    void closeByProtocol(Reference iterator, IteratorCompletion completion)
    {
        if (m_walker.isUnreachable())
            return;
        switch (completion) {
        case IteratorCompletion::Normal:
            if (FunctionID closes = findHelper(IterationHelper::CloseIterator))
                callHelper(closes, { iterator });
            else
                callReturn(iterator, true);
            return;
        case IteratorCompletion::Throw:
            break;
        }
        if (FunctionID closes = findHelper(IterationHelper::CloseIteratorAfterThrow)) {
            callHelper(closes, { iterator });
            return;
        }
        BlockID after = addBlock();
        beginToDropWhatIsThrown();
        callReturn(iterator, false);
        dropWhatWasThrown(after);
    }

private:
    void standUnderHandler()
    {
        if (!m_handler || m_walker.isUnreachable())
            return;
        Code& code = m_walker.code();
        BlockID block = m_walker.currentBlock();
        if (!code.at(block).handler)
            code.closeEdge(code.addHandlerEdge(block), m_handler, { });
    }

    void throwNotIterable(Reference iterable)
    {
        Draft error = draft(Opcode::ThrowNotIterable);
        error.setIterableOperand(iterable);
        error.setIsSpread(false);
        buildThrowingOperation(WTF::move(error));
    }

    Reference readWord(Reference words, uint32_t word)
    {
        Draft element = draft(Opcode::GetArrayElement);
        element.setArrayOperand(words);
        element.setPositionOperand(integer(word));
        element.setArrayStorage(ArrayStorage::Contiguous);
        element.setLevel(KnowledgeLevel::Proven);
        element.setKeyIsIntegral(true);
        if (std::optional<PassedTest> inside = m_walker.findWhatHoldsWordInsideListMadeByWalk(words, m_listOfWordsAsMade, integer(word))) {
            element.setIsInBounds(*inside);
            element.setIsPresent(*inside);
        }
        return buildAsItIs(WTF::move(element));
    }

    void callReturn(Reference iterator, bool resultMustBeObject)
    {
        Reference method = read(iterator, BuiltinNames::returnKeyword);
        BlockID absent = addBlock();
        BlockID present = addBlock();
        BlockID after = addBlock();
        branch(testType(method, TypeTable::nullish), absent, present);
        continueIn(absent);
        jumpTo(after);

        continueIn(present);
        Reference result = call(method, iterator);
        if (resultMustBeObject)
            requireObject(result, RuntimeErrorKind::IteratorResultNotObject);
        jumpTo(after);
    }

    Walker& m_walker;
    Origin m_origin;
    Reference m_listOfWordsAsMade;
    BlockID m_handler;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
