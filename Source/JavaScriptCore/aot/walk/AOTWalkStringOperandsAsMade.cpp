/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTypes.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr PristineAspectSet aspectsThatLookAtCellProvesForStringOperands { PristineAspects::Storage, PristineAspects::Names, PristineAspects::InheritedNamesKept, PristineAspects::Prototype };

bool goesByOneRowOnOperandAsMade(Walker& walker, const BodyValue& call, BuiltinID builtin, Reference value, TypeID behindTest, BuiltinOverloadID* onlyRow)
{
    TypeArena& types = walker.types();
    const BuiltinTable& table = walker.program().builtins();
    Type typeBehindTest = types.at(behindTest);
    const ClassPart* onlyClass = walker.has(Mechanism::EscapeAnalysis) && typeBehindTest.isObject() ? singleBuiltinClassOf(types, typeBehindTest) : nullptr;
    if (!onlyClass || pristineTestOf(table, onlyClass->builtinClass) != PristineTest::MayPass)
        return false;

    BuiltinOperandTypes assumed;
    bool isEachBuilt = true;
    auto describe = [&](ValueID input, TypeID& type, PristineAspectSet& aspects) {
        Reference operand = walker.referenceOf(input);
        isEachBuilt &= !!operand;
        if (!operand)
            return;
        type = operand == value ? behindTest : walker.typeOf(operand);
        if (operand == value || isKnownPristine(walker, operand))
            aspects = aspectsThatLookAtCellProvesForStringOperands;
    };
    if (ValueID receiver = call.thisValueOperand())
        describe(receiver, assumed.receiver, assumed.aspectsOfReceiver);
    for (ValueID argument : call.argumentsOperands()) {
        TypeID type;
        PristineAspectSet aspects;
        describe(argument, type, aspects);
        assumed.arguments.append(type);
        assumed.aspectsOfArguments.append(aspects);
    }

    BuiltinTypeContext context { walker.program(), types };
    if (!isEachBuilt || !rowsSpeakOfCallOnReceiver(context, builtin, assumed.receiver))
        return false;
    Vector<BuiltinOverloadID, 4> reached = overloadsReachedBy(context, builtin, BuiltinCallMode::Call, assumed);
    if (reached.size() != 1 || table.at(reached[0]).isUndeclared())
        return false;
    if (onlyRow)
        *onlyRow = reached[0];
    return true;
}

static ValueID findLastCallInRowOfCallsOfMethodsOfTable(Walker& walker, const BodyValue& firstRead, const BodyValue& firstCall, BuiltinClassID builtinClass, Vector<ValueID, 4>& readsOffHolders)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(firstRead.id()));
    ValueID receiver = firstRead.objectOperand();
    uint32_t behindStretch = block.firstValue().index() + std::min<uint32_t>(block.numberOfValues(), walker.endOfStretchBeingWalked());
    auto argumentsRunNoCode = [&](ValueID read, ValueID call) {
        for (uint32_t number = read.index() + 1; number < call.index(); ++number) {
            BodyValue within = index.value(ValueID(number));
            ValueID user = index.onlyUsingValue(within.id());
            if (!user || call < user)
                return false;
            if (within.opcode() == Opcode::Convert)
                continue;
            if (within.opcode() != Opcode::PropGet || within.isFreeName() || within.objectOperand() == receiver || !(within.objectOperand() < firstRead.id()) || !guessAtShapeOfHolderReadByName(walker, within))
                return false;
            readsOffHolders.append(within.id());
        }
        return true;
    };
    if (!argumentsRunNoCode(firstRead.id(), firstCall.id()))
        return firstCall.id();
    ValueID last = firstCall.id();
    for (;;) {
        ValueID result = last;
        uint32_t next = result.index() + 1;
        while (next < behindStretch && index.opcodeOf(ValueID(next)) == Opcode::Convert && index.value(ValueID(next)).operand(0) == result && index.onlyUsingValue(result) == ValueID(next))
            result = ValueID(next++);
        if (!index.isUnused(result) || next >= behindStretch || index.opcodeOf(ValueID(next)) != Opcode::PropGet)
            return last;
        BodyValue read = index.value(ValueID(next));
        BuiltinID member = !read.isFreeName() && read.key() && read.objectOperand() == receiver ? findOwnMember(program, table.at(builtinClass).prototype, read.key()) : BuiltinID();
        ValueID callID = member && table.boxedCallStubOf(table.canonicalID(member)) ? index.onlyUsingValue(read.id()) : ValueID();
        if (!callID || index.isParameter(callID) || index.blockOf(callID) != block.id() || index.opcodeOf(callID) != Opcode::Call || callID.index() >= behindStretch)
            return last;
        BodyValue call = index.value(callID);
        size_t readsBefore = readsOffHolders.size();
        if (call.calleeOperand() != read.id() || call.thisValueOperand() != receiver || !argumentsRunNoCode(read.id(), callID)) {
            readsOffHolders.shrink(readsBefore);
            return last;
        }
        last = callID;
    }
}

static bool passesCallbackToCopy(Walker& walker, const BodyValue& call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    auto arguments = call.as<Opcode::Call>().argumentsOperands();
    ValueID callback = arguments.empty() ? ValueID() : index.stripCasts(arguments[0]);
    FunctionID function;
    if (callback && !index.isParameter(callback) && index.opcodeOf(callback) == Opcode::NewClosure)
        function = index.value(callback).function();
    else if (callback && !index.isParameter(callback) && index.opcodeOf(callback) == Opcode::GetModuleVariable) {
        const Variable& variable = walker.program().at(index.value(callback).as<Opcode::GetModuleVariable>().variable());
        if (variable.initialization == VariableInitialization::Function && variable.flags.contains(VariableFlags::IsImmutable))
            function = variable.function;
    }
    return function && walker.facts().linked().function(function).weightAsLeaf != LinkedFunction::notLeaf;
}

static bool argumentsStayInCall(const BodyIndex& index, ValueID read, ValueID call)
{
    for (uint32_t number = read.index() + 1; number < call.index(); ++number) {
        ValueID user = index.onlyUsingValue(ValueID(number));
        if (!user || call < user)
            return false;
    }
    return true;
}

ValueID findLastCallInMethodChain(Walker& walker, const BodyValue& firstCall, uint32_t behindStretch)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(firstCall.id()));
    ValueID last = firstCall.id();
    if (!passesCallbackToCopy(walker, firstCall))
        return last;
    for (;;) {
        ValueID result = last;
        uint32_t next = result.index() + 1;
        while (next < behindStretch && index.opcodeOf(ValueID(next)) == Opcode::Convert && index.value(ValueID(next)).operand(0) == result && index.onlyUsingValue(result) == ValueID(next))
            result = ValueID(next++);
        if (next >= behindStretch || index.opcodeOf(ValueID(next)) != Opcode::PropGet || index.useCount(result) != 2)
            return last;
        BodyValue read = index.value(ValueID(next));
        BuiltinID member = !read.isFreeName() && read.key() && read.objectOperand() == result ? findOwnMember(program, table.at(BuiltinClassID(BuiltinClasses::Array)).prototype, read.key()) : BuiltinID();
        bool hasLibraryImplementation = member && findLibraryFunctionsBehindBuiltin(walker, walker.types().forBuiltin(table.canonicalID(member))).enteredByValue;
        ValueID callID = hasLibraryImplementation ? index.onlyUsingValue(read.id()) : ValueID();
        if (!callID || index.isParameter(callID) || !block.holds(callID) || index.opcodeOf(callID) != Opcode::Call || callID.index() >= behindStretch)
            return last;
        BodyValue call = index.value(callID);
        if (call.calleeOperand() != read.id() || call.thisValueOperand() != result || !passesCallbackToCopy(walker, call) || !argumentsStayInCall(index, read.id(), callID))
            return last;
        last = callID;
    }
}

bool openTestOfListAtReadOfMethodOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (read.isFreeName() || !walker.isInLoop() || walker.isUnreachable() || !walker.has(Mechanism::Inlining) || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::EscapeAnalysis))
        return false;
    Walker::MethodChain& chain = walker.currentMethodChain();
    if (walker.walksStretchInsideAnother() && (chain.depthOfCopies != walker.depthOfCopies() || !(chain.openRead < read.id()) || chain.lastCall < read.id()))
        return false;
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    TypeArena& types = walker.types();
    Reference receiver = walker.referenceOf(read.objectOperand());
    TypeID proven = receiver && receiver.isValue() ? walker.typeOf(receiver) : TypeID();
    const ClassPart* onlyClass = proven && !isNeverType(proven) && types.at(proven).isObject() ? singleBuiltinClassOf(types, types.at(proven)) : nullptr;
    if (!onlyClass || !read.key() || pristineTestOf(table, onlyClass->builtinClass) != PristineTest::MayPass)
        return false;
    BuiltinClassID builtinClass = onlyClass->builtinClass;
    BuiltinID member = findOwnMember(program, table.at(builtinClass).prototype, read.key());
    if (!member)
        return false;
    bool hasLibraryImplementation = !!findLibraryFunctionsBehindBuiltin(walker, types.forBuiltin(table.canonicalID(member))).enteredByValue;
    if (!hasLibraryImplementation && ((builtinClass != BuiltinClasses::Map && builtinClass != BuiltinClasses::Set) || !table.boxedCallStubOf(table.canonicalID(member))))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    ValueID callID = index.onlyUsingValue(read.id());
    if (!callID || index.isParameter(callID) || index.blockOf(callID) != block.id() || index.opcodeOf(callID) != Opcode::Call || callID.index() - block.firstValue().index() >= walker.endOfStretchBeingWalked())
        return false;
    BodyValue call = index.value(callID);
    ValueID receiverID = read.objectOperand();
    if (call.calleeOperand() != read.id() || call.thisValueOperand() != receiverID)
        return false;
    if ((hasLibraryImplementation && !passesCallbackToCopy(walker, call)) || !argumentsStayInCall(index, read.id(), callID))
        return false;

    TypeID instances = types.builtinClass(builtinClass);
    if (!types.isSubset(proven, instances) || isKnownPristine(walker, receiver) || receiverStaysAsMadeByLink(walker, read))
        return false;
    Vector<ValueID, 4> readsOffHolders;
    uint32_t behindStretch = block.firstValue().index() + std::min<uint32_t>(block.numberOfValues(), walker.endOfStretchBeingWalked());
    ValueID last = hasLibraryImplementation ? findLastCallInMethodChain(walker, call, behindStretch) : findLastCallInRowOfCallsOfMethodsOfTable(walker, read, call, builtinClass, readsOffHolders);
    if (last == callID)
        readsOffHolders.shrink(0);
    else
        walker.count(hasLibraryImplementation ? StatisticID::MethodChainsKeptInBranches : StatisticID::CollectionCallRunsGuardedOnce);
    SetForScope walksChain(chain, Walker::MethodChain { walker.depthOfCopies(), read.id(), last == callID ? read.id() : last });
    if (hasLibraryImplementation && index.opcodeOf(index.stripCasts(call.as<Opcode::Call>().argumentsOperands()[0])) == Opcode::GetModuleVariable)
        walker.count(StatisticID::ReceiversSpeculatedForNamedCallbacks);
    openGuessAtOperandTakenAsMade(walker, index.value(last), receiver, instances, read.id().index() - block.firstValue().index(), readsOffHolders.span());
    return true;
}

void openGuessAtOperandTakenAsMade(Walker& walker, const BodyValue& call, Reference value, TypeID tested, std::optional<uint32_t> firstPlace, std::span<const ValueID> readsOffHolders)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BlockID inputBlock = index.blockOf(call.id());
    uint32_t place = call.id().index() - index.block(inputBlock).firstValue().index();
    BodyTerminator end = index.terminator(inputBlock);
    bool isReturnedAtOnce = end.kind() == TerminatorKind::ReturnCall && end.operand(0) == call.id();
    bool isReadBehind = !index.isUnused(call.id());

    Vector<ValueID, 2> names;
    call.forEachOperand([&](ValueID operand, uint32_t) {
        if (walker.referenceOf(operand) == value && !names.contains(operand))
            names.append(operand);
    });

    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    BlockID closedBlock;
    auto continueIfCheckPasses = [&](Reference test) {
        BlockID passed = walker.addBlock();
        if (!closedBlock)
            closedBlock = walker.addBlock(BlockFrequency::Rare);
        walker.branch(test, passed, closedBlock);
        walker.continueIn(passed);
    };

    Reference proven = value;
    if (!walker.types().isSubset(walker.typeOf(value), tested)) {
        Reference isOfClass = buildTypeTestForStrings(walker, value, tested, origin);
        if (!isOfClass)
            return;
        continueIfCheckPasses(isOfClass);
        proven = buildProofBehindTestForStrings(walker, value, isOfClass, OperandIndex::TypeTest::value, origin);
    }
    TestOfObjectAsMade asked = walker.program().admitsReplacedBuiltins ? TestOfObjectAsMade::AsksObjectAlone : TestOfObjectAsMade::AsksWhatWasWrittenAboveToo;
    if (Reference isAsMade = proven ? buildTestOfObjectAsMade(walker, proven, origin, asked) : Reference()) {
        continueIfCheckPasses(isAsMade);
        markObjectPristine(walker, proven);
    }
    struct HolderShaped {
        ValueID name;
        HoistedSpeculation guess;
        Reference shaped;
    };
    Vector<HolderShaped, 2> holdersShaped;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> placesOfHolders;
    UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> slotsRead;
    for (ValueID readOffHolder : readsOffHolders) {
        if (walker.isUnreachable() || walker.hasFailed())
            break;
        walker.releaseEndedPendingBlocks();
        BodyValue read = index.value(readOffHolder);
        ValueID name = read.objectOperand();
        auto place = placesOfHolders.find(name.index());
        if (place == placesOfHolders.end()) {
            std::optional<HoistedSpeculation> guess = guessAtShapeOfHolderReadByName(walker, read);
            Reference hasShape = guess ? buildTestInFrontOfLoop(walker, *guess, origin) : Reference();
            if (!hasShape || !hasShape.isValue())
                continue;
            continueIfCheckPasses(hasShape);
            Reference shaped = walker.isUnreachable() ? Reference() : buildProofInFrontOfLoop(walker, *guess, hasShape, origin);
            if (!shaped)
                continue;
            place = placesOfHolders.add(name.index(), static_cast<uint32_t>(holdersShaped.size())).iterator;
            holdersShaped.append(HolderShaped { name, WTF::move(*guess), shaped });
        }
        const HolderShaped& holder = holdersShaped[place->value];
        if (!slotsRead.add(static_cast<uint64_t>(name.index()) << 32 | read.key().bits()).isNewEntry)
            continue;
        Reference isFilled = buildTestOfSlotFilledBehindShape(walker, holder.guess, holder.shaped, read);
        if (!isFilled)
            continue;
        if (isFilled.isValue())
            continueIfCheckPasses(isFilled);
        if (!walker.isUnreachable())
            readSlotFilledBehindShape(walker, holder.guess, holder.shaped, read);
    }
    walker.letGuessFailUnasked(closedBlock, origin);
    if (walker.hasFailed())
        return;
    BlockID typedBlock = walker.isUnreachable() ? BlockID() : walker.currentBlock();
    BlockID meetingBlock = walker.addBlock();
    walker.recordSpeculation();

    Vector<Walker::JoinArm, 2> arms;
    auto walkCallToEndOfArm = [&] {
        walker.walkStretch(inputBlock, firstPlace.value_or(place), place + 1);
        if (isReturnedAtOnce)
            walker.threadReturnOf(inputBlock);
        if (walker.isUnreachable())
            return;
        Reference result = isReadBehind ? walker.referenceOf(call.id()) : Reference();
        if (isReadBehind && (!result || !result.isRecord()))
            return walker.fail("an arm of a guess at an operand has no value for what is read behind it"_s);
        arms.append(Walker::JoinArm { walker.currentBlock(), { result } });
    };

    walker.continueIn(closedBlock);
    {
        Walker::ClosedArm closedArm(walker);
        walkCallToEndOfArm();
    }
    if (walker.hasFailed())
        return;

    if (typedBlock) {
        walker.continueIn(typedBlock);
        Vector<Reference, 2> boundBefore;
        for (ValueID name : names) {
            boundBefore.append(walker.referenceBoundTo(name));
            walker.bind(name, proven);
        }
        Vector<Reference, 2> holdersBefore;
        for (const HolderShaped& holder : holdersShaped) {
            holdersBefore.append(walker.referenceBoundTo(holder.name));
            walker.bind(holder.name, holder.shaped);
        }
        walkCallToEndOfArm();
        for (size_t position = 0; position < holdersShaped.size(); ++position)
            walker.bind(holdersShaped[position].name, holdersBefore[position]);
        for (size_t position = 0; position < names.size(); ++position)
            walker.bind(names[position], boundBefore[position]);
    }
    if (walker.hasFailed())
        return;

    Reference met;
    if (walker.joinArms(arms.span(), meetingBlock, singleElementSpan(met).first(isReadBehind)) && isReadBehind)
        walker.bind(call.id(), met);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
