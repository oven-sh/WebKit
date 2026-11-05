/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkJSONWrites.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinIDs.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTStringBuilderWords.h"
#include "AOTTypeArena.h"
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static Reference inFormForLoopOfSerializers(Walker& walker, Reference reference, Representation wanted, Origin origin)
{
    Representation held = walker.representationOf(reference);
    if (!reference || !hasRegister(wanted) || held == wanted)
        return reference;

    TypeArena& types = walker.types();
    if (reference.isConstant()) {
        const Operation& record = walker.code().at(reference.id());
        bool hasCopyInForm = constantsInFormOfUses.contains(record.opcode) && isLegalRepresentation(types.at(record.type), wanted)
            && wanted != Representation::Cell32;
        if (hasCopyInForm) {
            Draft copy(record.opcode, record.origin());
            copy.flags = static_cast<uint8_t>(record.flags);
            copy.setPayloadListFrom(0, record.payload());
            copy.representation = wanted;
            return walker.constant(WTF::move(copy));
        }
    }

    std::optional<ConversionPath> path;
    if (held != Representation::Unchosen)
        path = findConversion(types, walker.typeOf(reference), held, wanted);
    if (!path) {
        walker.fail("a value of a serializer has no conversion into the form in which it goes round"_s);
        return { };
    }
    origin.isTailCall = false;
    for (ConversionStep step : *path) {
        Draft conversion(step.opcode, origin);
        conversion.setOperand(0, reference);
        conversion.representation = step.to;
        reference = walker.build(WTF::move(conversion));
        if (!reference)
            return { };
    }
    return reference;
}

BuiltLoopOfSerializers::BuiltLoopOfSerializers(Walker& walker, std::span<const Reference> entering, std::span<const TypeID> typesOfParameters)
    : m_walker(walker)
{
    RELEASE_ASSERT(entering.size() == typesOfParameters.size());
    if (walker.isUnreachable())
        return;

    Code& code = walker.code();
    Origin origin = walker.originOfOperation();
    m_standsInLoopAround = code.at(walker.currentBlock()).isInLoop;
    m_head = code.addBlock(BlockRole::Ordinary, code.at(walker.currentBlock()).frequency);
    code.setLoopMarks(m_head, true, true);
    Vector<ValueID, 2> parameters;
    for (TypeID type : typesOfParameters) {
        parameters.append(code.appendParameter(m_head, type, walker.formOfParameter(type, { }), origin));
        m_parameters.append(Reference::value(parameters.last()));
    }
    if (!parameters.isEmpty())
        code.setParameters(m_head, parameters.span());

    if (!leadToHead(entering)) {
        m_head = { };
        return;
    }
    walker.continueIn(m_head);
    Draft poll(Opcode::LoopHint, origin);
    walker.chooseConservativeFields(poll);
    if (Reference built = walker.build(WTF::move(poll)))
        m_poll = built.id();
}

bool BuiltLoopOfSerializers::leadToHead(std::span<const Reference> values)
{
    RELEASE_ASSERT(values.size() == m_parameters.size());
    Code& code = m_walker.code();
    Origin origin = m_walker.originOfOperation();

    Vector<ValueID, 2> arguments;
    for (size_t index = 0; index < values.size(); ++index) {
        TypeID typeOfParameter = code.at(m_parameters[index].id()).type;
        Representation form = code.at(m_parameters[index].id()).representation;
        Reference value = values[index];
        if (!value || !value.isRecord() || !m_walker.types().isSubset(code.at(value.id()).type, typeOfParameter)) {
            m_walker.fail("what is handed to the head of a loop of a serializer lies outside the type of its parameter"_s);
            return false;
        }
        if (!isSameWord(m_walker.representationOf(value), form))
            value = inFormForLoopOfSerializers(m_walker, value, form, origin);
        if (!value || m_walker.isUnreachable())
            return false;
        arguments.append(value.id());
    }

    BlockID from = m_walker.currentBlock();
    code.end(from, TerminatorDraft(TerminatorKind::Jump, origin), 1);
    code.closeEdge(code.at(from).terminator.next(), m_head, arguments.span());
    return true;
}

void BuiltLoopOfSerializers::goRound(std::span<const Reference> values)
{
    if (!m_head || m_walker.isUnreachable())
        return;
    if (leadToHead(values))
        m_hasBackEdge = true;
}

void BuiltLoopOfSerializers::close()
{
    if (!m_head || m_walker.hasFailed())
        return;
    if (!m_walker.isUnreachable()) {
        m_walker.fail("a loop of a serializer is closed and the block that the walk stands in has no end"_s);
        return;
    }
    if (m_hasBackEdge)
        return;
    Code& code = m_walker.code();
    code.setLoopMarks(m_head, m_standsInLoopAround, true);
    if (m_poll)
        code.addFateOfPoll(m_poll, FateOfPoll::IsDropped);
}

void JSONTextWriter::appendListByRunTime(Reference array, Reference levelsAround, const PositionOfJSONText& position)
{
    appendKeyOf(position);
    Draft list = draft(Opcode::JSONAppendValue);
    list.setJsonPosition(JSONPosition::Element);
    list.setValueOperand(array);
    if (levelsAround)
        list.setLevelsOperand(levelsAround);
    append(WTF::move(list));
    takeResultsOfJSONAppendValue(true);
    appendCommaOf(position);
}

void JSONTextWriter::appendArray(Reference array, const JSONValuePlan& plan, const PositionOfJSONText& position)
{
    LevelScope level(*this);
    if (hasStopped())
        return;
    if (!m_buildsLoops) {
        appendListByRunTime(array, integer(m_levelsOpen - 1), position);
        return;
    }

    JSONListAround list { m_walker.typeOf(array), plan.element };
    if (!list.list || repeatsJSONListAround(m_listsAround.span().subspan(m_firstListDirectlyAround), list)) {
        takeSlowPath();
        return;
    }
    m_listsAround.append(list);
    auto leaveList = makeScopeExit([&] {
        m_listsAround.removeLast();
    });
    requirePristine(array, BuiltinClasses::Array, aspectsOfArrayWrittenAsJSON);
    if (hasStopped())
        return;

    Draft readOfLength = draft(Opcode::GetArrayLength);
    readOfLength.setArrayOperand(array);
    Reference length = build(WTF::move(readOfLength));
    if (hasStopped())
        return;
    if (!length) {
        takeSlowPath();
        return;
    }

    BlockID done = openMerge();
    ArmOfJSONText isEmpty = armOfComparison(Opcode::StrictEq, length, 0);
    if (isEmpty.matches != AnswerOfJSONTest::No) {
        appendKeyOf(position);
        appendToken(JSONToken::EmptyArray);
        appendCommaOf(position);
        jumpTo(done);
        if (!continueWithOtherValues(isEmpty)) {
            continueAt(done);
            return;
        }
    }

    for (const ArrayStorageForm& form : arrayStorageForms) {
        if (hasStopped())
            break;
        if (!types().overlap(types().withoutNullish(typeOfElementRead(form.kind)), plan.element))
            continue;
        Draft storageTest = draft(Opcode::HasArrayStorage);
        storageTest.setArrayOperand(array);
        storageTest.setArrayStorage(form.kind);
        ArmOfJSONText hasStorage = armOfTest(build(WTF::move(storageTest)));
        if (hasStorage.matches == AnswerOfJSONTest::No)
            continue;

        if (!form.holdsCells) {
            appendListByRunTime(array, { }, position);
        } else {
            appendKeyOf(position);
            appendToken(JSONToken::OpenArray);
            appendArrayElements(array, length, form.kind, plan.element);
            appendToken(JSONToken::CloseArray);
            appendCommaOf(position);
        }
        jumpTo(done);
        if (!continueWithOtherValues(hasStorage)) {
            continueAt(done);
            return;
        }
    }
    takeSlowPath();
    continueAt(done);
}

void JSONTextWriter::appendArrayElement(Reference array, Reference index, ArrayStorage storage, TypeID writtenElementType)
{
    if (hasStopped())
        return;
    Draft read = draft(Opcode::GetArrayElement);
    read.setArrayOperand(array);
    read.setPositionOperand(index);
    read.setArrayStorage(storage);
    read.setLevel(KnowledgeLevel::Proven);
    read.setKeyIsIntegral(true);
    read.setIsInBounds(true);
    Reference element = build(WTF::move(read));
    if (hasStopped())
        return;
    if (!element) {
        takeSlowPath();
        return;
    }

    appendValue(element, planJSONValue(m_context, m_walker.typeOf(element), writtenElementType), PositionOfJSONText { JSONPosition::Element, { }, afterSeparator });
}

void JSONTextWriter::appendArrayElements(Reference array, Reference length, ArrayStorage storage, TypeID writtenElementType)
{
    if (hasStopped())
        return;
    Type lengthType = typeOf(length);
    if (!lengthType.isInteger()) {
        takeSlowPath();
        return;
    }
    if (lengthType.integers.maximum < 2) {
        appendArrayElement(array, integer(0), storage, writtenElementType);
        return;
    }

    commit();
    BlockID behindLastElement = openMerge();
    Reference firstIndex = integer(0);
    TypeID indexType = types().integer(Range { 0, lengthType.integers.maximum - 1 });
    BuiltLoopOfSerializers loop(m_walker, singleElementSpan(firstIndex), singleElementSpan(indexType));
    ++m_numberOfOpenLoops;
    auto appendElementOfTurn = [&] {
        Reference index = loop.parameter(0);
        appendArrayElement(array, index, storage, writtenElementType);

        Reference next = buildOperator(Opcode::Add, index, integer(1));
        Reference hasNext = buildOperator(Opcode::Less, next, length);
        ensureCursor(jsonTokenBytes);
        if (hasStopped())
            return;
        if (!hasNext) {
            m_walker.fail("nothing ends the loop over the elements of a list that is written as JSON"_s);
            return;
        }

        CursorOfJSONText afterElement = m_cursor;
        BlockID nextTurn = addBlock();
        BlockID lastTurn = addBlock();
        m_walker.branch(hasNext, nextTurn, lastTurn);
        continueIn(nextTurn);
        Reference comingRound = narrowed(next, hasNext, OperandIndex::Less::left, true);
        m_cursor = afterElement;
        appendToken(JSONToken::Comma);
        commit();
        if (!hasStopped())
            loop.goRound(singleElementSpan(comingRound));

        continueIn(lastTurn);
        m_cursor = afterElement;
        jumpTo(behindLastElement);
    };
    if (!hasStopped())
        appendElementOfTurn();
    --m_numberOfOpenLoops;
    loop.close();
    continueAt(behindLastElement);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
