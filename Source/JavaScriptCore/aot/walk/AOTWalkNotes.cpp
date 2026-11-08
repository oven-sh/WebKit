/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNotes.h"

#if ENABLE(AOT)

#include "AOTCompileUnit.h"
#include "AOTOpcodeSets.h"
#include "AOTResultRules.h"
#include "AOTType.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet testsInWalkNotes = opcodesWhere([](Opcode opcode) {
    return familyOf(opcode) == OperationFamily::Guard && accessOf(opcode) == Access::None;
});
static constexpr OpcodeSet allocationsInWalkNotes = opcodesWhere([](Opcode opcode) {
    return interpretedOf(opcode) == Interpretation::Allocation;
});
static constexpr OpcodeSet operationsThatHandOnInWalkNotes = opcodesWhere([](Opcode opcode) {
    return handsOnOf(opcode) != HandedOn::Nothing || opcode == Opcode::ResultAt;
});
static constexpr OpcodeSet accessesOfElementsInWalkNotes = opcodesWhere([](Opcode opcode) {
    Opcode generic = genericOperationOf(opcode).value_or(opcode);
    return generic == Opcode::ElemGet || generic == Opcode::ElemSet || generic == Opcode::ElemHas || generic == Opcode::ElemDefine || generic == Opcode::ElemDelete;
});
static constexpr OpcodeSet buildersOfPlainObjectsInWalkNotes {
    Opcode::NewObject, Opcode::NewDictionary, Opcode::CopyObject, Opcode::CopyObjectAndDefine, Opcode::CopyTwoObjects
};
// What these prove is no type: it is that an object is as it was built.
static constexpr OpcodeSet testsOfStateInWalkNotes = opcodesWhere([](Opcode opcode) {
    switch (provesOf(opcode)) {
    case Proof::Pristine:
    case Proof::NoExtraProperties:
    case Proof::NoFieldOutsideSlot:
    case Proof::NotFrozen:
    case Proof::PrototypeLinkUnchanged:
    case Proof::KeyOrder:
    case Proof::MemberUnchanged:
    case Proof::HeldFieldsFilled:
    case Proof::SlotFilled:
    case Proof::UnmodifiedGenerator:
    case Proof::EnumeratorCurrent:
    case Proof::NotThenable:
        return true;
    default:
        return false;
    }
});

ASCIILiteral nameOf(SourceOfNotedValue source)
{
    switch (source) {
    case SourceOfNotedValue::NotSaid:
        return "NotSaid"_s;
    case SourceOfNotedValue::EntryOfFunction:
        return "EntryOfFunction"_s;
    case SourceOfNotedValue::CaughtValue:
        return "CaughtValue"_s;
    case SourceOfNotedValue::EvaluatedText:
        return "EvaluatedText"_s;
    case SourceOfNotedValue::BuiltinResult:
        return "BuiltinResult"_s;
    case SourceOfNotedValue::ResultOfCallByValue:
        return "ResultOfCallByValue"_s;
    case SourceOfNotedValue::ResultOfDirectCall:
        return "ResultOfDirectCall"_s;
    case SourceOfNotedValue::ElementOfCollection:
        return "ElementOfCollection"_s;
    case SourceOfNotedValue::PropertyOrVariable:
        return "PropertyOrVariable"_s;
    case SourceOfNotedValue::ObjectBuiltWithoutLayout:
        return "ObjectBuiltWithoutLayout"_s;
    case SourceOfNotedValue::LostByWalk:
        return "LostByWalk"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral nameOf(AskedOfNotedValue asked)
{
    switch (asked) {
    case AskedOfNotedValue::NotSaid:
        return "NotSaid"_s;
    case AskedOfNotedValue::StateOfObject:
        return "StateOfObject"_s;
    case AskedOfNotedValue::PlaceOfElement:
        return "PlaceOfElement"_s;
    case AskedOfNotedValue::ElementThatMayBeAbsent:
        return "ElementThatMayBeAbsent"_s;
    case AskedOfNotedValue::IndexThatMayBeUndefined:
        return "IndexThatMayBeUndefined"_s;
    case AskedOfNotedValue::PropertyOfObjectWithoutLayout:
        return "PropertyOfObjectWithoutLayout"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static AccessKind kindOfCallInWalkNote(const Operation& call)
{
    bool constructs = entersByValueOf(call.opcode) == EntryByValue::Construction || (call.opcode == Opcode::CallDirect && call.isConstruction());
    return constructs ? AccessKind::Construct : AccessKind::Call;
}

static AccessKind kindOfAccessWithoutColumnInWalkNote(const Operation& access)
{
    if (access.numberOfNamedOperands() && access.operandUses(0).contains(OperandUse::DeletesByKey))
        return AccessKind::Delete;
    switch (interpretedOf(access.opcode)) {
    case Interpretation::Read:
        return AccessKind::Read;
    case Interpretation::Store:
        return AccessKind::Store;
    case Interpretation::Test:
        return AccessKind::Has;
    case Interpretation::None:
    case Interpretation::Allocation:
    case Interpretation::Cast:
    case Interpretation::List:
    case Interpretation::Guards:
        return AccessKind::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static AccessKind accessKindInWalkNote(const Operation& operation)
{
    Opcode opcode = operation.opcode;
    switch (accessOf(opcode)) {
    case Access::Read:
        return AccessKind::Read;
    case Access::Store:
        return AccessKind::Store;
    case Access::Define:
        return AccessKind::Define;
    case Access::Has:
        return AccessKind::Has;
    case Access::None:
        break;
    }

    switch (accessClassOf(opcode)) {
    case AccessClass::DirectCall:
    case AccessClass::IndirectCall:
        return kindOfCallInWalkNote(operation);
    case AccessClass::Slot:
    case AccessClass::ByName:
        return kindOfAccessWithoutColumnInWalkNote(operation);
    case AccessClass::None:
        break;
    }

    switch (operatorKindOf(opcode)) {
    case OperatorKind::Additive:
    case OperatorKind::Multiplicative:
    case OperatorKind::Bitwise:
        return AccessKind::Arithmetic;
    case OperatorKind::Relation:
        return AccessKind::Comparison;
    case OperatorKind::Equality:
        return AccessKind::Equality;
    case OperatorKind::NumberConversion:
        return AccessKind::Conversion;
    case OperatorKind::None:
        return AccessKind::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static WalkNoteKind kindOfOperationInWalkNote(const Operation& operation)
{
    if (testsItsOwnGuess(operation))
        return WalkNoteKind::TestThatRejoins;

    Opcode opcode = operation.opcode;
    switch (accessClassOf(opcode)) {
    case AccessClass::Slot:
    case AccessClass::DirectCall:
        return WalkNoteKind::ResolvedOperation;
    case AccessClass::ByName:
    case AccessClass::IndirectCall:
        return WalkNoteKind::GenericOperation;
    case AccessClass::None:
        break;
    }
    if (PayloadWord::form(opcode))
        return operation.form() == OperatorForm::Generic ? WalkNoteKind::GenericOperation : WalkNoteKind::ResolvedOperation;
    return formOf(opcode) == Form::Open ? WalkNoteKind::ResolvedOperation : WalkNoteKind::GenericOperation;
}

static AccessCost costOfWalkNoteKind(WalkNoteKind kind)
{
    switch (kind) {
    case WalkNoteKind::ResolvedOperation:
        return AccessCost::Free;
    case WalkNoteKind::TestThatRejoins:
    case WalkNoteKind::TestThatLeaves:
    case WalkNoteKind::TestThatThrows:
        return AccessCost::TestsReceiver;
    case WalkNoteKind::GenericOperation:
    case WalkNoteKind::CallNotInlined:
    case WalkNoteKind::ObjectBuilt:
    case WalkNoteKind::ObjectNotInFrame:
    case WalkNoteKind::ObjectStayedVirtual:
    case WalkNoteKind::LoopWalkedAgain:
    case WalkNoteKind::WayEnds:
        return AccessCost::ByName;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool placeWalkNote(const Code& code, const Operation& operation, Origin& origin, FunctionID& writtenIn)
{
    std::span<const InlineFrame> frames = code.inlineFrames();
    if (!operation.hasPosition() || operation.inlineFrame >= frames.size())
        return false;
    origin = operation.origin();
    writtenIn = frames[operation.inlineFrame].callee;
    return true;
}

// Nothing proves the type of a value whose type holds every object, or holds a seed. A seed marks what is believed and not proven:
// the declared type of a parameter whose callers do not prove it, what a read by name gives.
static bool isUnprovenInWalkNotes(TypeReader types, const Code& code, ValueID value)
{
    TypeID type = code.at(value).type;
    if (!type)
        return false;
    const Type& record = types.at(type);
    return record.hasUnboundedUnknown() || !record.seeds.isEmpty();
}

static ValueID makerBehindHandingOnInWalkNotes(const Code& code, ValueID value)
{
    for (;;) {
        const Operation& operation = code.at(value);
        if (!operationsThatHandOnInWalkNotes.contains(operation.opcode) || !operation.numberOfOperands() || !operation.operand(0))
            return value;
        value = operation.operand(0);
    }
}

static bool isEntryParameterInWalkNotes(const Code& code, const Operation& maker)
{
    if (maker.opcode != Opcode::Parameter)
        return false;
    const Block& block = code.at(maker.block());
    return block.isEntry && !block.isHandlerBlock();
}

static SourceOfNotedValue sourceOfMakerInWalkNotes(const Code& code, const Operation& maker)
{
    Opcode opcode = maker.opcode;
    if (isEntryParameterInWalkNotes(code, maker))
        return SourceOfNotedValue::EntryOfFunction;
    OperationFamily family = familyOf(opcode);
    if (opcode == Opcode::Parameter || family == OperationFamily::Constant)
        return SourceOfNotedValue::NotSaid;
    if (raisesOf(opcode) == ProgramEvent::EvaluatesText)
        return SourceOfNotedValue::EvaluatedText;
    if (family == OperationFamily::Exception)
        return SourceOfNotedValue::CaughtValue;
    if (family == OperationFamily::Arguments)
        return SourceOfNotedValue::EntryOfFunction;
    if (family == OperationFamily::BuiltinCall || family == OperationFamily::InlineBuiltin)
        return SourceOfNotedValue::BuiltinResult;
    if (accessClassOf(opcode) == AccessClass::IndirectCall)
        return SourceOfNotedValue::ResultOfCallByValue;
    if (accessClassOf(opcode) == AccessClass::DirectCall)
        return SourceOfNotedValue::ResultOfDirectCall;
    if (buildersOfPlainObjectsInWalkNotes.contains(opcode))
        return SourceOfNotedValue::ObjectBuiltWithoutLayout;
    if (maker.numberOfNamedOperands() && maker.operandUses(0).contains(OperandUse::ReadsContents))
        return SourceOfNotedValue::ElementOfCollection;
    bool reads = accessOf(opcode) == Access::Read || interpretedOf(opcode) == Interpretation::Read || accessClassOf(opcode) != AccessClass::None;
    if (reads || family == OperationFamily::Binding || family == OperationFamily::Environment)
        return SourceOfNotedValue::PropertyOrVariable;
    return SourceOfNotedValue::NotSaid;
}

// The operand whose type decides how the operation is compiled: the object of an access, and of an operator the first operand
// whose type is not proven.
static ValueID valueThatOperationHangsOnInWalkNotes(TypeReader types, const Code& code, const Operation& operation)
{
    if (std::optional<uint8_t> receiver = receiverOf(operation.opcode))
        return operation.child(*receiver);
    if (accessesOfElementsInWalkNotes.contains(operation.opcode))
        return operation.child(0);
    if (operatorKindOf(operation.opcode) == OperatorKind::None)
        return { };
    for (uint32_t operandIndex = 0; operandIndex < operation.numberOfNamedOperands(); ++operandIndex) {
        ValueID operand = operation.operand(operandIndex);
        if (operand && isUnprovenInWalkNotes(types, code, operand))
            return operand;
    }
    return { };
}

static bool elementMayBeAbsentInWalkNotes(const Operation& access)
{
    return access.opcode == Opcode::GetArrayElement && (!access.isInBounds() || (!access.isPresent() && !access.readsHoleAsNaN()));
}

// The test that has to fail for the block to run: the block has one edge into it, and that edge is the one a branch takes where
// its condition does not hold.
static ValueID testThatFailsInFrontOfInWalkNotes(const Code& code, BlockID block)
{
    EdgeID only = code.at(block).firstEdgeIn();
    if (!only || code.at(only).nextInto())
        return { };
    const Terminator& terminator = code.at(code.at(only).from).terminator;
    if (terminator.kind != TerminatorKind::Branch || terminator.notTaken() != only)
        return { };
    return terminator.child(0);
}

struct SaidOfNotedValue {
    SourceOfNotedValue source { SourceOfNotedValue::NotSaid };
    AskedOfNotedValue asked { AskedOfNotedValue::NotSaid };
    // Nothing is said, and only because the value is made out of a join that is asked about now: it is a variable of a loop.
    bool comesBackToJoin { false };

    bool saysSomething() const { return source != SourceOfNotedValue::NotSaid || asked != AskedOfNotedValue::NotSaid; }
    bool saysTheSameAs(const SaidOfNotedValue& other) const { return source == other.source && asked == other.asked; }
};

// What is asked for one note. A value is followed through reads and joins, and a join has many edges: the steps bound the work.
struct QuestionInWalkNotes {
    TypeReader types;
    const Code& code;
    unsigned stepsLeft { 64 };
    Vector<ValueID, 8> joinsAsked { };
};

static SaidOfNotedValue saidOfValueInWalkNotes(QuestionInWalkNotes&, ValueID);

// The type is proven, and every object that it holds is one without a layout.
static bool holdsObjectsWithoutLayoutInWalkNotes(TypeReader types, const Code& code, ValueID value)
{
    TypeID type = code.at(value).type;
    if (!type)
        return false;
    const Type& record = types.at(type);
    return record.isJavaScriptValue() && (record.hasOnlyClassObjects() || record.hasOnlyClosures() || record.hasOnlyBuiltins());
}

enum class AskedFor : uint8_t { Operation, ValueThatOperationMakes };

// What an operation that is no resolved one says: of the value that it hangs on where the type of that value is not proven, and
// else of the test that it runs, or that it runs behind. That an access by name asks an object without a layout is said of the
// access alone: of the value that the access makes it says nothing.
static SaidOfNotedValue saidOfOperationInWalkNotes(QuestionInWalkNotes& question, const Operation& operation, AskedFor askedFor)
{
    const Code& code = question.code;
    ValueID hangsOn = valueThatOperationHangsOnInWalkNotes(question.types, code, operation);
    if (hangsOn && isUnprovenInWalkNotes(question.types, code, hangsOn))
        return saidOfValueInWalkNotes(question, hangsOn);

    bool isOfElement = accessesOfElementsInWalkNotes.contains(operation.opcode);
    ValueID index = isOfElement ? operation.child(1) : ValueID();
    if (index && isUnprovenInWalkNotes(question.types, code, index))
        return saidOfValueInWalkNotes(question, index);

    if (testsItsOwnGuess(operation)) {
        if (!isOfElement || !hangsOn)
            return { };
        return { SourceOfNotedValue::NotSaid, elementMayBeAbsentInWalkNotes(operation) ? AskedOfNotedValue::ElementThatMayBeAbsent : AskedOfNotedValue::PlaceOfElement };
    }

    if (ValueID failed = testThatFailsInFrontOfInWalkNotes(code, operation.block())) {
        Opcode test = code.at(failed).opcode;
        if (testsOfStateInWalkNotes.contains(test))
            return { SourceOfNotedValue::NotSaid, AskedOfNotedValue::StateOfObject };
        if (test == Opcode::TypeTest && index && code.at(index).opcode == Opcode::Undefined)
            return { SourceOfNotedValue::NotSaid, AskedOfNotedValue::IndexThatMayBeUndefined };
    }

    bool isByName = accessClassOf(operation.opcode) == AccessClass::ByName && !isOfElement;
    if (askedFor == AskedFor::Operation && isByName && hangsOn && holdsObjectsWithoutLayoutInWalkNotes(question.types, code, hangsOn))
        return { SourceOfNotedValue::NotSaid, AskedOfNotedValue::PropertyOfObjectWithoutLayout };
    return { };
}

// What a join holds whose type is not proven comes from the values that its edges bring. Where those say one thing the join says
// it. Where they do not, and their makers are of one kind, the join is of that kind: a read that is resolved on one way and runs by
// name on the other is a read on both.
static SaidOfNotedValue saidOfJoinInWalkNotes(QuestionInWalkNotes& question, ValueID join)
{
    const Code& code = question.code;
    UnknownSeeds seeds = question.types.at(code.at(join).type).seeds;
    if (!seeds.isEmpty() && seeds.containsOnly({ UnknownSeed::CaughtException }))
        return { SourceOfNotedValue::CaughtValue };
    if (!seeds.isEmpty() && seeds.containsOnly({ UnknownSeed::LostByWalk }))
        return { SourceOfNotedValue::LostByWalk };

    BlockID block = code.blockOf(join);
    std::span<const ValueID> parameters = code.at(block).parameters();
    size_t place = 0;
    while (place < parameters.size() && parameters[place] != join)
        ++place;
    if (place == parameters.size())
        return { };
    if (question.joinsAsked.contains(join))
        return { SourceOfNotedValue::NotSaid, AskedOfNotedValue::NotSaid, true };

    question.joinsAsked.append(join);
    SaidOfNotedValue agreed;
    SourceOfNotedValue agreedKind = SourceOfNotedValue::NotSaid;
    bool saidDiffers = false;
    bool kindsDiffer = false;
    code.forEachEdgeInto(block, [&](const Edge& edge) {
        if (place >= edge.arguments().size())
            return;
        ValueID brought = edge.arguments()[place];
        if (!brought || !isUnprovenInWalkNotes(question.types, code, brought))
            return;
        SaidOfNotedValue said = saidOfValueInWalkNotes(question, brought);
        if (!said.saysSomething() && said.comesBackToJoin)
            return;
        saidDiffers |= !said.saysSomething() || (agreed.saysSomething() && !said.saysTheSameAs(agreed));
        agreed = said;

        SourceOfNotedValue kind = sourceOfMakerInWalkNotes(code, code.at(makerBehindHandingOnInWalkNotes(code, brought)));
        kindsDiffer |= kind == SourceOfNotedValue::NotSaid || (agreedKind != SourceOfNotedValue::NotSaid && kind != agreedKind);
        agreedKind = kind;
    });
    question.joinsAsked.removeLast();

    if (!saidDiffers)
        return { agreed.source, agreed.asked };
    if (!kindsDiffer)
        return { agreedKind };
    return { };
}

static SaidOfNotedValue saidOfValueInWalkNotes(QuestionInWalkNotes& question, ValueID value)
{
    const Code& code = question.code;
    if (!question.stepsLeft || !isUnprovenInWalkNotes(question.types, code, value))
        return { };
    --question.stepsLeft;

    ValueID made = makerBehindHandingOnInWalkNotes(code, value);
    const Operation& maker = code.at(made);
    if (maker.opcode == Opcode::Parameter && !isEntryParameterInWalkNotes(code, maker))
        return saidOfJoinInWalkNotes(question, made);

    // What a read that is no resolved one gives is no more proven than what the read hangs on.
    bool isRead = accessKindInWalkNote(maker) == AccessKind::Read && operatorKindOf(maker.opcode) == OperatorKind::None;
    if (isRead && kindOfOperationInWalkNote(maker) != WalkNoteKind::ResolvedOperation) {
        SaidOfNotedValue said = saidOfOperationInWalkNotes(question, maker, AskedFor::ValueThatOperationMakes);
        if (said.saysSomething() || said.comesBackToJoin)
            return said;
    }
    return { sourceOfMakerInWalkNotes(code, maker) };
}

static void noteWhatOperationSaysInWalkNotes(TypeReader types, const Code& code, const Operation& operation, WalkNote& note)
{
    QuestionInWalkNotes question { types, code };
    SaidOfNotedValue said = saidOfOperationInWalkNotes(question, operation, AskedFor::Operation);
    note.source = said.source;
    note.asked = said.asked;
    if (note.kind != WalkNoteKind::TestThatRejoins)
        return;
    if (ValueID hangsOn = valueThatOperationHangsOnInWalkNotes(types, code, operation))
        placeWalkNote(code, code.at(makerBehindHandingOnInWalkNotes(code, hangsOn)), note.neededAt, note.neededIn);
}

static void noteWhatTestSaysInWalkNotes(TypeReader types, const Code& code, const Operation& test, WalkNote& note)
{
    ValueID tested = test.child(0);
    if (tested)
        placeWalkNote(code, code.at(makerBehindHandingOnInWalkNotes(code, tested)), note.neededAt, note.neededIn);

    if (testsOfStateInWalkNotes.contains(test.opcode)) {
        note.asked = AskedOfNotedValue::StateOfObject;
        return;
    }
    switch (provesOf(test.opcode)) {
    case Proof::ArrayStorage:
        note.asked = AskedOfNotedValue::PlaceOfElement;
        return;
    case Proof::InBounds:
        note.asked = AskedOfNotedValue::ElementThatMayBeAbsent;
        return;
    default:
        break;
    }
    if (!tested)
        return;
    QuestionInWalkNotes question { types, code };
    SaidOfNotedValue said = saidOfValueInWalkNotes(question, tested);
    note.source = said.source;
    note.asked = said.asked;
}

static ValueID userThatNeedsCellInWalkNotes(const Code& code, ValueID object)
{
    ValueID user = code.onlyUserOf(object);
    while (user && conversionsOfForm.contains(code.at(user).opcode))
        user = code.onlyUserOf(user);
    return user;
}

static LayoutID layoutOfValueInWalkNote(TypeReader types, const Code& code, ValueID value)
{
    return singleLayoutOf(types, types.at(code.at(value).type));
}

WalkNotes notesOfSweptCode(TypeReader types, const Code& code, bool notesResolvedOperations, bool looksForSources)
{
    RELEASE_ASSERT(code.isSwept());
    WalkNotes result { code.unit(), { }, 0 };

    for (uint32_t index = 0; index < code.numberOfValues(); ++index) {
        ValueID id(index);
        const Operation& operation = code.at(id);
        BlockID block = operation.block();
        if (!block || !code.isReached(block))
            continue;
        result.numberOfValuesInDouble += code.isNeeded(id) && (operation.representation == Representation::Double || operation.representation == Representation::OptionalDouble);

        WalkNote note;
        if (!placeWalkNote(code, operation, note.origin, note.writtenIn))
            continue;
        note.loopDepth = code.at(block).isInLoop;

        if (allocationsInWalkNotes.contains(operation.opcode)) {
            note.kind = code.isNeeded(id) ? WalkNoteKind::ObjectBuilt : WalkNoteKind::ObjectStayedVirtual;
            note.cost = costOfWalkNoteKind(note.kind);
            note.layout = layoutOfValueInWalkNote(types, code, id);
            if (note.kind == WalkNoteKind::ObjectBuilt) {
                if (ValueID user = userThatNeedsCellInWalkNotes(code, id); user && placeWalkNote(code, code.at(user), note.neededAt, note.neededIn))
                    note.opcode = code.at(user).opcode;
            }
            result.notes.append(note);
            continue;
        }
        if (!code.isNeeded(id))
            continue;

        note.opcode = operation.opcode;
        if (code.unit().role == CodeRole::ParameterGuard) {
            if (operation.opcode != Opcode::TypeTest)
                continue;
            note.kind = WalkNoteKind::TestThatLeaves;
            note.cost = costOfWalkNoteKind(note.kind);
            note.layout = singleLayoutOf(types, types.at(operation.testedType()));
            if (looksForSources)
                noteWhatTestSaysInWalkNotes(types, code, operation, note);
            result.notes.append(note);
            continue;
        }
        if (testsInWalkNotes.contains(operation.opcode)) {
            note.kind = WalkNoteKind::TestThatRejoins;
            note.cost = costOfWalkNoteKind(note.kind);
            if (ValueID tested = operation.child(0))
                note.layout = layoutOfValueInWalkNote(types, code, tested);
            if (looksForSources)
                noteWhatTestSaysInWalkNotes(types, code, operation, note);
            result.notes.append(note);
            continue;
        }

        note.access = accessKindInWalkNote(operation);
        if (note.access == AccessKind::Unknown)
            continue;
        note.kind = kindOfOperationInWalkNote(operation);
        if (note.kind == WalkNoteKind::ResolvedOperation && !notesResolvedOperations)
            continue;
        note.cost = costOfWalkNoteKind(note.kind);
        if (std::optional<uint8_t> receiver = receiverOf(operation.opcode)) {
            if (ValueID object = operation.child(*receiver))
                note.layout = layoutOfValueInWalkNote(types, code, object);
        }
        if (note.kind != WalkNoteKind::ResolvedOperation && looksForSources)
            noteWhatOperationSaysInWalkNotes(types, code, operation, note);
        result.notes.append(note);
    }
    for (const Code::TestOfGuardInFrontOfCopy& noted : code.testsOfGuardsInFrontOfCopies()) {
        const Operation& operation = code.at(noted.test);
        BlockID block = operation.block();
        if (operation.opcode != Opcode::TypeTest || !block || !code.isReached(block) || !code.isNeeded(noted.test))
            continue;
        WalkNote note;
        note.kind = WalkNoteKind::TestThatLeaves;
        note.opcode = operation.opcode;
        note.cost = costOfWalkNoteKind(note.kind);
        note.loopDepth = code.at(block).isInLoop;
        note.origin = Origin { noted.positionOfCallee, 0, false, false };
        note.writtenIn = noted.callee;
        note.layout = singleLayoutOf(types, types.at(operation.testedType()));
        if (looksForSources)
            noteWhatTestSaysInWalkNotes(types, code, operation, note);
        result.notes.append(note);
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
