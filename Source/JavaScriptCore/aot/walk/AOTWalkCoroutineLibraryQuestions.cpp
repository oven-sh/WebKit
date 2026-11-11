/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkCoroutinePromises.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTObjects.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypeNarrowing.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

bool answerLibraryQuestionOfCoroutines(Walker&, const BodyValue&);

static constexpr OpcodeSet makersOfArraysOfLibraryQuestions {
    Opcode::NewArray, Opcode::Data, Opcode::NewTemplateObject, Opcode::CreateRest,
    Opcode::NewContiguousArray, Opcode::NewArrayLike,
};

static void bindAnswerOfLibraryQuestion(Walker& walker, const BodyValue& question, bool answer)
{
    Draft constant(Opcode::Boolean);
    constant.setBoolean(answer);
    walker.bind(question.id(), walker.constant(WTF::move(constant)));
}

static bool mayBeArrayOfLibraryQuestions(TypeArena& types, TypeID id)
{
    return types.overlap(id, types.builtinClass(BuiltinClasses::Array)) || mayContainBuiltin(types, types.at(id), Builtins::ArrayPrototype);
}

static bool holdsOnlyArraysOfLibraryQuestions(TypeArena& types, TypeID id)
{
    Type type = types.at(id);
    if (!type.isObject() || !type.hasOnlyClassObjects())
        return false;
    const ClassPart* part = singleBuiltinClassOf(types, type);
    return part && part->builtinClass == BuiltinClasses::Array;
}

static bool answerWhetherIteratorIsReadUnseenOfLibraryQuestions(Walker& walker, const BodyValue& question)
{
    Reference value = walker.referenceOf(question.as<Opcode::ReadsIteratorOfListUnseen>().valueOperand());
    TypeID type = value ? walker.typeOf(value) : TypeID();
    if (!type || isNeverType(type) || walker.types().at(type).mayBeUnspecified())
        return false;
    TypeArena& types = walker.types();
    if (!mayBeArrayOfLibraryQuestions(types, type)) {
        bindAnswerOfLibraryQuestion(walker, question, false);
        return true;
    }

    if (!holdsOnlyArraysOfLibraryQuestions(types, type) || !walker.program().builtins().mayHavePristineInstances(BuiltinClasses::Array))
        return false;
    if (!isKnownPristine(walker, value))
        return false;
    if (!isArrayIteratorProtocolIntact(walker))
        return false;
    walker.verifyPristineObject(value);
    bindAnswerOfLibraryQuestion(walker, question, true);
    return true;
}

static TypeID typeOfCellsWithKindsOfLibraryQuestions(Walker& walker, JSType first, JSType last)
{
    unsigned numberOfKinds = static_cast<unsigned>(last) - static_cast<unsigned>(first) + 1;
    if (numberOfKinds > 64)
        return { };

    const BuiltinTable& table = walker.program().builtins();
    TypeArena& types = walker.types();
    TypeID tested = TypeTable::never;
    uint64_t kindsWithClass = 0;
    for (uint32_t index = 0; index < table.classCount(); ++index) {
        const BuiltinClass& candidate = table.at(BuiltinClassID(index));
        if (candidate.origin != BuiltinOrigin::Engine || candidate.instanceTest != BuiltinInstanceTest::JSTypeEquals || candidate.type < first || candidate.type > last)
            continue;
        tested = types.unionOf(tested, types.builtinClass(candidate.id));
        kindsWithClass |= 1ull << (static_cast<unsigned>(candidate.type) - static_cast<unsigned>(first));
    }
    uint64_t everyKind = numberOfKinds == 64 ? UINT64_MAX : (1ull << numberOfKinds) - 1;
    return kindsWithClass == everyKind ? tested : TypeID();
}

static bool mayBeInstanceOfDerivedClassOfLibraryQuestions(Walker& walker, const Type& type)
{
    const Program& program = walker.program();
    return std::ranges::any_of(membersOf(walker.types(), type), [&](LayoutID layout) {
        return program.at(layout).isDerived();
    });
}

static std::optional<bool> hasCellKindOfArraysOfLibraryQuestions(Walker& walker, Reference value, TypeID id)
{
    TypeArena& types = walker.types();
    if (!mayBeArrayOfLibraryQuestions(types, id))
        return false;

    const Code& code = walker.code();
    if (value.isValue() && holdsOnlyArraysOfLibraryQuestions(types, id)) {
        ValueID made = underlyingCellOf(code, value.id());
        if (!code.isConstant(made) && makersOfArraysOfLibraryQuestions.contains(code.at(made).opcode))
            return true;
    }

    Type type = types.at(id);
    if (!type.isObject() || !type.hasOnlyNamedObjects() || type.namesMergedObjects())
        return std::nullopt;
    bool areAllMadeAsArrays = types.at(type.objects).allOf([&](ObjectID object) {
        const ObjectRecord& record = types.at(object);
        return record.kind == ObjectKind::RestArray && record.builtinClass == BuiltinClasses::Array && !record.layout && !record.has(ObjectTrait::MayHaveLayoutBelow);
    });
    return areAllMadeAsArrays ? std::optional(true) : std::nullopt;
}

static bool answerWhetherCellHasKindOfLibraryQuestions(Walker& walker, const BodyValue& question)
{
    auto written = question.as<Opcode::HasJSType>();
    JSType first = toJSType(written.first());
    JSType last = toJSType(written.last());
    if (first > last) {
        bindAnswerOfLibraryQuestion(walker, question, false);
        return true;
    }

    Reference value = walker.referenceOf(written.valueOperand());
    TypeID id = value ? walker.typeOf(value) : TypeID();
    if (!id || isNeverType(id))
        return false;
    TypeArena& types = walker.types();
    Type type = types.at(id);
    if (!type.isJavaScriptValue() || type.mayBeUnspecified())
        return false;
    if (!type.hasCells()) {
        bindAnswerOfLibraryQuestion(walker, question, false);
        return true;
    }

    JSType kindOfArrays = walker.program().builtins().at(BuiltinClasses::Array).type;
    if (first <= kindOfArrays && kindOfArrays <= last) {
        std::optional<bool> answer = first == last ? hasCellKindOfArraysOfLibraryQuestions(walker, value, id) : std::nullopt;
        if (answer)
            bindAnswerOfLibraryQuestion(walker, question, *answer);
        return answer.has_value();
    }

    if (mayBeInstanceOfDerivedClassOfLibraryQuestions(walker, type))
        return false;
    TypeID tested = typeOfCellsWithKindsOfLibraryQuestions(walker, first, last);
    if (!tested)
        return false;

    TypingContext typing = walker.typing();
    if (isNeverType(narrowByTest(typing, id, tested, EdgeSide::Taken))) {
        bindAnswerOfLibraryQuestion(walker, question, false);
        return true;
    }
    if (isNeverType(narrowByTest(typing, id, tested, EdgeSide::NotTaken))) {
        bindAnswerOfLibraryQuestion(walker, question, true);
        return true;
    }

    Draft test(Opcode::TypeTest, walker.originOf(question));
    walker.chooseConservativeFields(test);
    test.inputValueGiven = question.id();
    test.setValueOperand(value);
    test.setTestedType(tested);
    if (Reference answer = walker.build(WTF::move(test)))
        walker.bind(question.id(), answer);
    return true;
}

bool answerLibraryQuestionOfCoroutines(Walker& walker, const BodyValue& question)
{
    if (!walker.has(Mechanism::Folding))
        return false;
    if (question.opcode() == Opcode::HasJSType)
        return answerWhetherCellHasKindOfLibraryQuestions(walker, question);
    return question.opcode() == Opcode::ReadsIteratorOfListUnseen && answerWhetherIteratorIsReadUnseenOfLibraryQuestions(walker, question);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
