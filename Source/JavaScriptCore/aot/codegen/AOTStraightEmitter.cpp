/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTCellFormat.h"
#include "AOTModuleEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubsArithmetic.h"
#include "AOTRepresentationRules.h"
#include "AOTSlotPlan.h"
#include "AOTStubsOperations.h"
#include "AOTStubsValues.h"
#include "AOTThrowEmitter.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "JSCJSValueInlines.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr TerminatorKindSet terminatorsWithStraightForm { TerminatorKind::Jump, TerminatorKind::Branch, TerminatorKind::Switch, TerminatorKind::Return, TerminatorKind::Throw, TerminatorKind::Rethrow, TerminatorKind::Unreachable };

static constexpr OpcodeSet opcodesWithoutCodeOfTheirOwn { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double };

static bool crossesEntryOfStraightUnit(Representation representation)
{
    switch (representation) {
    case Representation::JSValue:
    case Representation::Cell:
    case Representation::Int32:
    case Representation::Boolean:
    case Representation::Double:
        return true;
    default:
        return false;
    }
}

const OpcodeSet& StraightEmitter::opcodesWithStraightForm()
{
    static const OpcodeSet opcodes = opcodesOfConstants() | opcodesOfCasts() | opcodesOfOperators() | opcodesOfAccesses() | opcodesOfCalls() | opcodesOfBindings() | opcodesOfHints()
         | opcodesOfAllocations() | opcodesOfArrays() | opcodesOfArrayIntrinsics() | opcodesOfClasses() | opcodesOfCollectionIntrinsics() | opcodesOfEngineIntrinsics() | opcodesOfEvals() | opcodesOfExceptions() | opcodesOfForIns() | opcodesOfIterations() | opcodesOfModules() | opcodesOfPrivateAccesses() | opcodesOfPromiseIntrinsics() | opcodesOfRuntimeIntrinsics();
    return opcodes;
}

bool StraightEmitter::covers(const CodeGenerationInputs& inputs, CodeUnit unit, const BodyIndex& index)
{
    std::span<const BlockID> blocksNeverEntered = inputs.facts.summaries().at(unit.function).blocksNeverEntered.span();
    OpcodeSet opcodes = index.opcodes();
    if (!blocksNeverEntered.empty()) {
        opcodes = { };
        index.body().forEachBlock([&](BodyBlock block) {
            if (std::ranges::binary_search(blocksNeverEntered, block.id()))
                return;
            for (BodyValue value : block.values())
                opcodes.add(value.opcode());
        });
    }
    OpcodeSet covered = opcodes;
    covered &= opcodesWithStraightForm();
    if (!(covered == opcodes))
        return false;
    if (opcodes.contains(Opcode::NewClosure) && inputs.facts.linked().capturesWhatItOnlyCalls(unit.function))
        return false;

    const Function& function = inputs.program.at(unit.function);
    if (isClassConstructorOf(function.kind) && function.constructedClass && !fieldsHeldFilledInSlots(inputs.program, inputs.facts.linked(), inputs.plan.slots(), function.constructedClass).isEmpty())
        return false;

    const CallingConventions& conventions = inputs.plan.conventions();
    if (conventions.entryConventionOf(unit) != EntryConvention::Direct || conventions.numberOfResults(unit) != 1 || conventions.savesRegistersOfCallers(unit))
        return false;
    if (conventions.isCoroutine(unit.function))
        return false;
    if (inputs.plan.moduleEvaluation().moduleThatBeginsIn(unit.function))
        return false;
    if (!crossesEntryOfStraightUnit(conventions.resultRepresentation(unit)))
        return false;

    BodyBlock entry = index.block(entryBlock);
    for (uint32_t parameter = 0; parameter < entry.numberOfParameters(); ++parameter) {
        Representation representation = conventions.parameterRepresentation(inputs.program, unit, parameter);
        if (!crossesEntryOfStraightUnit(representation) && !index.isUnused(entry.parameter(parameter)))
            return false;
    }

    bool isCovered = true;
    index.body().forEachBlock([&](BodyBlock block) {
        if (std::ranges::binary_search(blocksNeverEntered, block.id()))
            return;
        for (BodyValue value : block.values()) {
            if (!opcodes.contains(Opcode::ModuleNamespace))
                break;
            if (value.opcode() == Opcode::ModuleNamespace && !value.ofModuleItself() && !value.asksAnew() && !ModuleEmitter::asksRunTimeForNamespaceAtEveryRead(inputs.program, inputs.plan, value.module(), value.phase()))
                isCovered = false;
        }
        TerminatorKind kind = block.terminator().kind();
        if (kind == TerminatorKind::ReturnCall && unit.role == CodeRole::Body && conventions.resultRepresentation(unit) != Representation::JSValue)
            isCovered = false;
        isCovered = isCovered && (terminatorsWithStraightForm.contains(kind) || (kind == TerminatorKind::ReturnCall && coversReturnCalls()) || (kind == TerminatorKind::SwitchString && coversSwitchesOnStrings()));
    });
    return isCovered;
}

StraightEmitter::StraightEmitter(BuildContext& build, TypeArena& types, const CodeGenerationInputs& inputs, CodeUnit unit, const BodyIndex& index, CodeRecords& records, Machine::Arena& arena)
    : m_build(build)
    , m_types(types)
    , m_inputs(inputs)
    , m_unit(unit)
    , m_index(index)
    , m_records(records)
    , m_arena(arena)
    , m_blocksNeverEntered(inputs.facts.summaries().at(unit.function).blocksNeverEntered.span())
{
    findValuesNobodyReads();
    measureFrame();
}

const CallingConventions& StraightEmitter::conventions() const
{
    return m_inputs.plan.conventions();
}

void StraightEmitter::findValuesNobodyReads()
{
    static constexpr OpcodeSet operationsThatOnlyMakeValues = inputIdentityOpcodes | opcodesWithoutCodeOfTheirOwn
        | OpcodeSet { Opcode::String, Opcode::BigInt, Opcode::Data, Opcode::NewClosure, Opcode::GetModuleVariable, Opcode::TypeTest };
    uint32_t numberOfValues = m_index.numberOfValues();
    Vector<uint32_t, 128> readsLeft;
    BitVector checksThatPass;
    auto countReads = [&] {
        if (!readsLeft.isEmpty())
            return;
        readsLeft.grow(numberOfValues);
        for (uint32_t other = 0; other < numberOfValues; ++other)
            readsLeft[other] = m_index.useCount(ValueID(other));
        m_valuesNobodyReads.ensureSize(numberOfValues);
    };
    for (uint32_t place = numberOfValues; place--;) {
        ValueID id(place);
        ValueID checkThatPasses;
        if (!m_index.isParameter(id) && m_index.opcodeOf(id) == Opcode::Call && mayCallOnlyCalleeDirectly(m_index.value(id), checkThatPasses)) {
            countReads();
            if (checkThatPasses) {
                checksThatPass.ensureSize(numberOfValues);
                checksThatPass.quickSet(checkThatPasses.index());
            }
            m_callsOfOnlyCallees.ensureSize(numberOfValues);
            m_callsOfOnlyCallees.quickSet(place);
            m_entersUnits = true;
            uint32_t callee = m_index.value(id).calleeOperand().index();
            if (readsLeft[callee] && readsLeft[callee] < BodyIndex::maxCountedUses)
                --readsLeft[callee];
            continue;
        }
        if (!m_index.isParameter(id) && m_index.opcodeOf(id) == Opcode::DefineMethod && classRecordCreatesMemberDefinedBy(m_index.value(id))) {
            countReads();
            uint32_t function = m_index.value(id).as<Opcode::DefineMethod>().functionOperand().index();
            if (readsLeft[function] && readsLeft[function] < BodyIndex::maxCountedUses)
                --readsLeft[function];
            continue;
        }
        if (m_index.isParameter(id) || !(operationsThatOnlyMakeValues.contains(m_index.opcodeOf(id)) || (place < checksThatPass.size() && checksThatPass.quickGet(place))))
            continue;
        if (readsLeft.isEmpty() ? !m_index.isUnused(id) : !!readsLeft[place])
            continue;
        BodyValue value = m_index.value(id);
        if (value.opcode() == Opcode::Convert && value.as<Opcode::Convert>().declared().has(PositionFlags::IsInt32Promise))
            continue;
        countReads();
        m_valuesNobodyReads.quickSet(place);
        value.forEachOperand([&](ValueID operand, uint32_t) {
            if (operand && readsLeft[operand.index()] && readsLeft[operand.index()] < BodyIndex::maxCountedUses)
                --readsLeft[operand.index()];
        });
    }
}

void StraightEmitter::measureFrame()
{
    static constexpr OpcodeSet opcodesThatEnterUnits { Opcode::CallDirect, Opcode::EvaluateModule };
    size_t longestList = 0;
    uint32_t lasting = 0;
    uint32_t mostPassing = 0;
    m_index.body().forEachBlock([&](BodyBlock block) {
        if (isNeverEntered(block.id()))
            return;
        for (uint32_t index = 0; index < block.numberOfParameters(); ++index)
            lasting += !m_index.isUnused(block.parameter(index));
        uint32_t passing = 0;
        BodyTerminator terminator = block.terminator();
        ValueID returnedCall = terminator.kind() == TerminatorKind::ReturnCall ? terminator.callOperand() : ValueID();
        auto takesWord = [&](ValueID id) {
            return !m_index.isUnused(id) && !isReadByNobody(id) && !opcodesWithoutCodeOfTheirOwn.contains(m_index.opcodeOf(id));
        };
        for (BodyValue value : block.values()) {
            Opcode opcode = value.opcode();
            if (opcode != Opcode::NewObject)
                longestList = std::max(longestList, value.variadicOperands().size());
            m_entersUnits = m_entersUnits || opcodesThatEnterUnits.contains(opcode);
            if (takesWord(value.id())) {
                std::optional<BodyUser> user = m_index.onlyUser(value.id());
                bool isReadOnceInBlock = user && (user->kind == BodyUseKind::Operand ? block.holds(user->value) : user->kind == BodyUseKind::TerminatorOperand && user->block == block.id());
                if (!isReadOnceInBlock)
                    ++lasting;
                else
                    mostPassing = std::max(mostPassing, ++passing);
            }
            if (value.id() == returnedCall)
                continue;
            bool keepsWordOfWhatItHandsOn = inputPassThroughOpcodes.contains(opcode) && !takesWord(value.id());
            value.forEachOperand([&](ValueID operand, uint32_t) {
                if (!operand || m_index.isParameter(operand) || !block.holds(operand) || !m_index.hasOneUse(operand) || !takesWord(operand))
                    return;
                --passing;
                lasting += keepsWordOfWhatItHandsOn;
            });
        }
    });
    m_numberOfSlots = lasting + mostPassing;
    m_wordsOfScratchArea = static_cast<uint32_t>((longestList + 5) & ~static_cast<size_t>(1));
    if (m_numberOfSlots + m_wordsOfScratchArea > mostWordsOfFrame) {
        fail(Opcode::Undefined);
        m_numberOfSlots = 0;
        m_wordsOfScratchArea = 0;
    }
}

void StraightEmitter::fail(Opcode opcode)
{
    if (m_hasFailed)
        return;
    m_hasFailed = true;
    m_opcodeThatFailed = opcode;
}

std::optional<JSValue> StraightEmitter::constantOf(ValueID id) const
{
    if (!isConstant(id))
        return std::nullopt;
    BodyValue value = m_index.value(rootOf(id));
    switch (value.opcode()) {
    case Opcode::Undefined:
        return jsUndefined();
    case Opcode::Null:
        return jsNull();
    case Opcode::Boolean:
        return jsBoolean(value.boolean());
    case Opcode::Integer:
        return jsNumber(static_cast<double>(value.integer()));
    case Opcode::Double:
        return jsNumber(value.number());
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CCallHelpers::Address StraightEmitter::addressOfSlot(ValueID value)
{
    if (m_valueTakenOutOfRegister && rootOf(value) == m_valueTakenOutOfRegister)
        fail(m_index.opcodeOf(m_valueTakenOutOfRegister));
    ValueID root = rootOf(value);
    uint32_t& slot = m_slots[root.index()];
    if (slot == slotWasGivenBack) {
        fail(m_index.opcodeOf(root));
        return CCallHelpers::Address(GPRInfo::callFrameRegister, -static_cast<int32_t>(sizeof(EncodedJSValue)));
    }
    if (!slot) {
        slot = m_freeSlots.isEmpty() ? ++m_numberOfSlotsTaken : m_freeSlots.takeLast();
        if (slot > m_numberOfSlots)
            fail(m_index.isParameter(root) ? Opcode::Undefined : m_index.opcodeOf(root));
        if (std::optional<uint32_t> position = positionOfLastReadInBlock(root))
            giveBackSlotAt(*position, root);
    }
    return CCallHelpers::Address(GPRInfo::callFrameRegister, -static_cast<int32_t>(sizeof(EncodedJSValue)) * static_cast<int32_t>(slot));
}

std::optional<uint32_t> StraightEmitter::positionOfLastReadInBlock(ValueID value) const
{
    ValueID root = rootOf(value);
    if (!m_block || m_index.isParameter(value) || !m_block.holds(value))
        return std::nullopt;
    while (true) {
        std::optional<BodyUser> user = onlyReader(value);
        if (!user)
            return std::nullopt;
        if (user->kind == BodyUseKind::TerminatorOperand && user->block == m_block.id())
            return m_block.endOfValues().index();
        if (user->kind != BodyUseKind::Operand || !m_block.holds(user->value))
            return std::nullopt;
        if (user->value == m_returnedCall)
            return m_block.endOfValues().index();
        if (rootOf(user->value) != root)
            return user->value.index();
        value = user->value;
    }
}

void StraightEmitter::giveBackSlotAt(uint32_t position, ValueID root)
{
    m_nextValueGivenBack[root.index()] = std::exchange(m_firstValueGivenBackAt[position], root.index() + 1);
}

void StraightEmitter::giveBackSlotsReadLastAt(uint32_t position)
{
    for (uint32_t next = std::exchange(m_firstValueGivenBackAt[position], 0); next; next = m_nextValueGivenBack[next - 1])
        m_freeSlots.append(std::exchange(m_slots[next - 1], slotWasGivenBack));
}

CCallHelpers::Address StraightEmitter::slotOf(ValueID value)
{
    storeWhatWaitsInRegister();
    return addressOfSlot(value);
}

std::optional<BodyUser> StraightEmitter::onlyReader(ValueID value) const
{
    if (value != m_subjectOfProofsNobodyReads)
        return m_index.onlyUser(value);
    return m_index.useCount(value) == m_numberOfProofsNobodyReads + 1 ? m_index.firstUser(value) : std::nullopt;
}

void StraightEmitter::findProofsNobodyReads(const BodyTerminator& terminator)
{
    m_subjectOfProofsNobodyReads = { };
    m_numberOfProofsNobodyReads = 0;
    if (terminator.kind() != TerminatorKind::Branch)
        return;
    uint32_t targetIndex = 0;
    for (BodyTarget target : terminator.targets()) {
        uint32_t numberOfProofs = static_cast<uint32_t>(target.proofs().size());
        for (uint32_t proofIndex = 0; proofIndex < numberOfProofs; ++proofIndex) {
            std::optional<BodyEdgeResult> result = m_index.edgeResult(terminator, targetIndex, target, proofIndex);
            if (!result || !result->subject || !m_index.isUnused(result->parameter))
                continue;
            if (m_subjectOfProofsNobodyReads && m_subjectOfProofsNobodyReads != result->subject) {
                m_subjectOfProofsNobodyReads = { };
                return;
            }
            m_subjectOfProofsNobodyReads = result->subject;
            ++m_numberOfProofsNobodyReads;
        }
        ++targetIndex;
    }
}

bool StraightEmitter::isReadByBranchAlone(const BodyValue& value) const
{
    std::optional<BodyUser> user = m_index.onlyUser(value.id());
    return user && user->kind == BodyUseKind::TerminatorOperand && user->block == m_block.id() && m_block.terminator().kind() == TerminatorKind::Branch && isReadByNextCodeAlone(value);
}

bool StraightEmitter::isReadByNextCodeAlone(const BodyValue& value) const
{
    ValueID id = value.id();
    uint32_t end = m_block.endOfValues().index();
    while (true) {
        std::optional<BodyUser> user = onlyReader(id);
        if (!user)
            return false;
        uint32_t reader = end;
        if (user->kind == BodyUseKind::Operand) {
            if (!m_block.holds(user->value) || user->value == m_returnedCall)
                return false;
            if (isReadByNobody(user->value))
                return false;
            reader = user->value.index();
        } else if (user->kind != BodyUseKind::TerminatorOperand || user->block != m_block.id())
            return false;
        for (uint32_t between = value.id().index() + 1; between < reader; ++between) {
            Opcode opcode = m_index.opcodeOf(ValueID(between));
            if (!inputIdentityOpcodes.contains(opcode) && !opcodesWithoutCodeOfTheirOwn.contains(opcode))
                return false;
        }
        if (reader == end)
            return true;
        if (!inputIdentityOpcodes.contains(m_index.opcodeOf(user->value)))
            return true;
        id = user->value;
    }
}

void StraightEmitter::boxWhatWaitsInRegister()
{
    if (m_formInRegister == FormInRegister::Test) {
        if (m_testInRegister.mask)
            m_jit->and64(CCallHelpers::TrustedImm32(m_testInRegister.mask), resultGPR);
        m_jit->compare64(CCallHelpers::Equal, resultGPR, CCallHelpers::TrustedImm32(m_testInRegister.word), resultGPR);
        m_formInRegister = FormInRegister::ZeroOrOne;
    }
    if (m_formInRegister == FormInRegister::ZeroOrOne)
        boxInForm(Representation::Boolean, resultGPR);
    m_formInRegister = FormInRegister::Boxed;
}

void StraightEmitter::storeWhatWaitsInRegister()
{
    if (!m_valueInRegister)
        return;
    boxWhatWaitsInRegister();
    ValueID stored = std::exchange(m_valueInRegister, { });
    m_jit->store64(resultGPR, addressOfSlot(stored));
    noteCopyInRegister(stored);
}

void StraightEmitter::noteCopyInRegister(ValueID value)
{
    m_valueCopiedInRegister = rootOf(value);
    m_sizeOfCodeAtCopy = m_jit->m_assembler.codeSize();
}

bool StraightEmitter::isCopiedInRegister(ValueID value) const
{
    return m_valueCopiedInRegister && rootOf(value) == m_valueCopiedInRegister && m_jit->m_assembler.codeSize() == m_sizeOfCodeAtCopy;
}

CCallHelpers::Address StraightEmitter::wordOfScratchArea(uint32_t word) const
{
    RELEASE_ASSERT(word < m_wordsOfScratchArea);
    return CCallHelpers::Address(CCallHelpers::stackPointerRegister, static_cast<int32_t>(word * sizeof(EncodedJSValue)));
}

void StraightEmitter::load(ValueID value, GPRReg destination)
{
    if (waitsInRegister(value)) {
        boxWhatWaitsInRegister();
        m_valueTakenOutOfRegister = std::exchange(m_valueInRegister, { });
        m_jit->move(resultGPR, destination);
        return;
    }
    if (isCopiedInRegister(value)) {
        m_jit->move(resultGPR, destination);
        m_sizeOfCodeAtCopy = m_jit->m_assembler.codeSize();
        return;
    }
    if (isResultRegister(destination))
        storeWhatWaitsInRegister();
    if (std::optional<JSValue> constant = constantOf(value)) {
        m_jit->move(CCallHelpers::TrustedImm64(JSValue::encode(*constant)), destination);
        return;
    }
    m_jit->load64(addressOfSlot(value), destination);
}

bool StraightEmitter::leavesArgumentsToThunk(StubKey key, const StraightCallOptions& options) const
{
    return !(options.numberedImmediate && descriptorOf(key.family()).binding == ImmediateBinding::InThunk);
}

void StraightEmitter::bringAll(std::span<const StraightArgument> arguments, NOESCAPE const Invocable<GPRReg(size_t)> auto& registerOf, ArgumentMoves* leftToThunk)
{
    auto goesIntoThunk = [&](const StraightArgument& argument) {
        if (!leftToThunk)
            return false;
        switch (argument.kind) {
        case StraightArgument::Kind::Value:
            if (std::optional<JSValue> constant = constantOf(argument.value))
                return static_cast<uint64_t>(JSValue::encode(*constant)) <= maxConstantInArgumentThunk;
            return true;
        case StraightArgument::Kind::Word:
            return argument.bits <= maxConstantInArgumentThunk;
        case StraightArgument::Kind::Number:
        case StraightArgument::Kind::Register:
        case StraightArgument::Kind::AddressInScratchArea:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    size_t broughtFirst = arguments.size();
    for (size_t i = 0; i < arguments.size() && m_valueInRegister; ++i) {
        if (arguments[i].kind != StraightArgument::Kind::Value || !waitsInRegister(arguments[i].value))
            continue;
        bring(arguments[i], registerOf(i));
        broughtFirst = i;
    }
    for (size_t i = 0; i < arguments.size() && broughtFirst == arguments.size() && !m_valueInRegister; ++i) {
        if (arguments[i].kind != StraightArgument::Kind::Value || !isCopiedInRegister(arguments[i].value) || goesIntoThunk(arguments[i]))
            continue;
        bring(arguments[i], registerOf(i));
        broughtFirst = i;
    }
    storeWhatWaitsInRegister();
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i == broughtFirst)
            continue;
        const StraightArgument& argument = arguments[i];
        if (!goesIntoThunk(argument)) {
            for (size_t earlier = 0; argument.kind == StraightArgument::Kind::Register && earlier < arguments.size(); ++earlier)
                RELEASE_ASSERT(!(earlier == broughtFirst || (earlier < i && !goesIntoThunk(arguments[earlier]))) || registerOf(earlier) != static_cast<GPRReg>(argument.bits));
            bring(argument, registerOf(i));
            continue;
        }
        if (argument.kind == StraightArgument::Kind::Word)
            appendArgumentMove(*leftToThunk, registerOf(i), ArgumentSource::Constant, argument.bits);
        else if (std::optional<JSValue> constant = constantOf(argument.value))
            appendArgumentMove(*leftToThunk, registerOf(i), ArgumentSource::Constant, static_cast<uint64_t>(JSValue::encode(*constant)));
        else
            appendArgumentMove(*leftToThunk, registerOf(i), ArgumentSource::Stack64, static_cast<uint64_t>(static_cast<int64_t>(addressOfSlot(argument.value).offset)));
    }

    if (!m_build.options().recorded.validates) [[likely]]
        return;
    AllowMacroScratchRegisterUsage allowScratch(*m_jit);
    GPRReg again = m_jit->scratchRegister();
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (registerOf(i) == again)
            return;
    }
    CCallHelpers::JumpList differs;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const StraightArgument& argument = arguments[i];
        if (argument.kind != StraightArgument::Kind::Value || (i != broughtFirst && goesIntoThunk(argument)))
            continue;
        if (m_valueTakenOutOfRegister && rootOf(argument.value) == m_valueTakenOutOfRegister)
            continue;
        if (std::optional<JSValue> constant = constantOf(argument.value))
            m_jit->move(CCallHelpers::TrustedImm64(JSValue::encode(*constant)), again);
        else
            m_jit->load64(addressOfSlot(argument.value), again);
        differs.append(m_jit->branch64(CCallHelpers::NotEqual, again, registerOf(i)));
    }
    if (differs.empty())
        return;
    CCallHelpers::Jump isRight = m_jit->jump();
    differs.link(m_jit);
    m_jit->breakpoint();
    isRight.link(m_jit);
}

void StraightEmitter::bring(const StraightArgument& argument, GPRReg destination)
{
    switch (argument.kind) {
    case StraightArgument::Kind::Value:
        load(argument.value, destination);
        return;
    case StraightArgument::Kind::Word:
        if (isResultRegister(destination))
            storeWhatWaitsInRegister();
        m_jit->move(CCallHelpers::TrustedImm64(static_cast<int64_t>(argument.bits)), destination);
        return;
    case StraightArgument::Kind::Number:
        if (isResultRegister(destination))
            storeWhatWaitsInRegister();
        m_records.relocations.moveNumber({ SymbolSpace::Number, static_cast<uint32_t>(argument.bits) }, destination);
        return;
    case StraightArgument::Kind::Register:
        storeWhatWaitsInRegister();
        m_jit->move(static_cast<GPRReg>(argument.bits), destination);
        return;
    case StraightArgument::Kind::AddressInScratchArea:
        if (isResultRegister(destination))
            storeWhatWaitsInRegister();
        m_jit->addPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(argument.bits * sizeof(EncodedJSValue))), CCallHelpers::stackPointerRegister, destination);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StraightEmitter::storeInScratchArea(ValueID value, uint32_t word)
{
    if (std::optional<JSValue> constant = constantOf(value)) {
        m_jit->store64(CCallHelpers::TrustedImm64(JSValue::encode(*constant)), wordOfScratchArea(word));
        return;
    }
    m_jit->store64(loadIntoResultRegister(value), wordOfScratchArea(word));
}

void StraightEmitter::define(const BodyValue& value, GPRReg boxed)
{
    if (m_valueInRegister && isResultRegister(boxed)) {
        fail(value);
        return;
    }
    storeWhatWaitsInRegister();
    if (m_index.isUnused(value.id()))
        return;
    if (isResultRegister(boxed) && isReadByNextCodeAlone(value)) {
        m_valueInRegister = value.id();
        m_formInRegister = FormInRegister::Boxed;
        return;
    }
    m_jit->store64(boxed, addressOfSlot(value.id()));
    if (isResultRegister(boxed))
        noteCopyInRegister(value.id());
}

void StraightEmitter::defineTest(const BodyValue& value, StraightTest test)
{
    RELEASE_ASSERT(!m_valueInRegister && isReadByBranchAlone(value));
    m_valueInRegister = value.id();
    m_formInRegister = FormInRegister::Test;
    m_testInRegister = test;
}

void StraightEmitter::throwIfEmpty(const BodyValue& check, ValueID checked, const ThrowSite& throwSite)
{
    bool waits = waitsInRegister(checked);
    if (waits)
        boxWhatWaitsInRegister();
    bool liesInRegister = waits || isCopiedInRegister(checked);
    CCallHelpers::Jump isFilled = liesInRegister ? m_jit->branchTest64(CCallHelpers::NonZero, resultGPR) : m_jit->branchTest64(CCallHelpers::NonZero, addressOfSlot(checked));
    ValueID waiting = std::exchange(m_valueInRegister, { });
    throwAt(check.origin(), throwSite);
    m_valueInRegister = waiting;
    isFilled.link(m_jit);
    defineSameAs(check, checked);
    if (waits && !isReadByNextCodeAlone(check))
        storeWhatWaitsInRegister();
    else if (liesInRegister && !waits)
        noteCopyInRegister(checked);
}

void StraightEmitter::defineBoolean(const BodyValue& value, GPRReg zeroOrOne)
{
    if (m_valueInRegister && isResultRegister(zeroOrOne)) {
        fail(value);
        return;
    }
    storeWhatWaitsInRegister();
    if (m_index.isUnused(value.id()))
        return;
    if (isResultRegister(zeroOrOne) && isReadByNextCodeAlone(value)) {
        m_valueInRegister = value.id();
        m_formInRegister = FormInRegister::ZeroOrOne;
        return;
    }
    boxInForm(Representation::Boolean, zeroOrOne);
    m_jit->store64(zeroOrOne, addressOfSlot(value.id()));
}

void StraightEmitter::defineInt32(const BodyValue& value, GPRReg word)
{
    boxInForm(Representation::Int32, word);
    define(value, word);
}

void StraightEmitter::defineDouble(const BodyValue& value, FPRReg number)
{
    GPRReg result = takeResultRegister();
    boxDouble(number, result);
    define(value, result);
}

ValueID StraightEmitter::resultAt(const BodyValue& producer, uint32_t index) const
{
    for (uint32_t next = producer.id().index() + 1; next < m_block.endOfValues().index(); ++next) {
        BodyValue value = m_index.value(ValueID(next));
        if (value.opcode() != Opcode::ResultAt || value.resultsOperand() != producer.id())
            break;
        if (value.index() == index)
            return value.id();
    }
    return { };
}

void StraightEmitter::defineResult(const BodyValue& producer, uint32_t index, GPRReg boxed)
{
    if (ValueID part = resultAt(producer, index); part && !m_index.isUnused(part))
        m_jit->store64(boxed, addressOfSlot(part));
}

void StraightEmitter::boxDouble(FPRReg number, GPRReg boxed)
{
    StubKey key = boxNumber(BoxedNumberForm::Double);
    const StubSignature& signature = m_inputs.stubCatalog.summaryOf(key).signature;
    if (isResultRegister(signature.firstResult.gpr()))
        takeResultRegister();
    m_jit->moveDouble(number, signature.operands[0].fpr());
    m_records.stubCalls.call(*m_jit, key, OptionalIndex { });
    m_jit->move(signature.firstResult.gpr(), boxed);
}

void StraightEmitter::unboxNumber(GPRReg boxedNumber, FPRReg number)
{
    CCallHelpers::Jump isInt32 = m_jit->branchIfInt32(boxedNumber);
    m_jit->unboxDoubleWithoutAssertions(boxedNumber, boxedNumber, number);
    CCallHelpers::Jump done = m_jit->jump();
    isInt32.link(m_jit);
    m_jit->convertInt32ToDouble(boxedNumber, number);
    done.link(m_jit);
}

void StraightEmitter::defineSameAs(const BodyValue& value, ValueID other)
{
    m_roots[value.id().index()] = m_roots[other.index()];
    uint32_t root = rootOf(other).index();
    for (uint32_t* link = &m_firstValueGivenBackAt[value.id().index()]; *link; link = &m_nextValueGivenBack[*link - 1]) {
        if (*link != root + 1)
            continue;
        *link = m_nextValueGivenBack[root];
        if (std::optional<uint32_t> position = positionOfLastReadInBlock(value.id()))
            giveBackSlotAt(*position, ValueID(root));
        return;
    }
}

void StraightEmitter::defineConstant(const BodyValue& value)
{
    m_roots[value.id().index()] = value.id().index() | isConstantBit;
}

void StraightEmitter::boxInForm(Representation representation, GPRReg word)
{
    switch (representation) {
    case Representation::JSValue:
    case Representation::Cell:
        return;
    case Representation::Int32:
        m_jit->zeroExtend32ToWord(word, word);
        m_jit->withNumberTag([&](auto tag) {
            m_jit->or64(tag, word);
        });
        return;
    case Representation::Boolean:
        m_jit->or32(CCallHelpers::TrustedImm32(JSValue::ValueFalse), word);
        return;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StraightEmitter::unboxIntoForm(Representation representation, GPRReg word)
{
    switch (representation) {
    case Representation::JSValue:
    case Representation::Cell:
        return;
    case Representation::Int32: {
        CCallHelpers::Jump isInt32 = m_jit->branchIfInt32(word);
        m_jit->unboxDoubleWithoutAssertions(word, word, conversionFPR);
        m_jit->truncateDoubleToInt32(conversionFPR, word);
        isInt32.link(m_jit);
        m_jit->zeroExtend32ToWord(word, word);
        return;
    }
    case Representation::Boolean:
        m_jit->and32(CCallHelpers::TrustedImm32(1), word);
        return;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t StraightEmitter::mentionOfName(PropertyKeyID key)
{
    RELEASE_ASSERT(key && !liesInThread(key));
    return m_records.mentions.add(NumberSpace::Name, key);
}

uint32_t StraightEmitter::addSite(Origin origin, const StraightCallOptions& options, Opcode opcode)
{
    BuiltinID calledBuiltin = options.calledBuiltin;
    if (calledBuiltin && BuiltinBindings::linkTimeConstantOf(calledBuiltin))
        calledBuiltin = { };
    bool standsForCallee = calledBuiltin ? origin.isTailCall || options.isTailCall : origin.isTailCall && !options.isTailCall && !options.constructs;
    bool isInsideCallee = options.callerHasLeft || (standsForCallee && (calledBuiltin || !runsBeforeCalleeOf(opcode)));
    std::optional<CallSiteKind> kind = findCallSiteKind({ .isConstruct = options.constructs, .isTailCall = options.isTailCall || origin.isTailCall || options.callerHasLeft, .isInsideCallee = isInsideCallee, .namesBuiltin = !!calledBuiltin });
    RELEASE_ASSERT(kind);
    uint32_t number = m_records.sites.addSite({ .origin = origin, .kind = *kind, .calledBuiltin = calledBuiltin, .constructedCallee = options.constructedCallee });
    if (uint64_t { number } + 1 > m_build.limit(LimitID::SitesPerFunction))
        fail(opcode);
    return number;
}

HandlerType StraightEmitter::handlerTypeOf(BlockID handler) const
{
    HandlerType type = HandlerType::SynthesizedCatch;
    for (BodyValue value : m_index.block(handler).values()) {
        if (value.opcode() == Opcode::CatchException)
            return HandlerType::Finally;
        if (value.opcode() != Opcode::CatchValue)
            break;
        type = HandlerType::Catch;
    }
    return type;
}

void StraightEmitter::noteSiteMayThrow(uint32_t site)
{
    if (!m_block.hasHandler())
        return;
    if (!m_landingPadOfBlockPlusOne) {
        m_landingPads.append({ m_block.id(), m_arena.create<MacroAssembler::Label>() });
        m_records.exceptions.landingPads.append(m_landingPads.last().label);
        m_landingPadOfBlockPlusOne = static_cast<uint32_t>(m_records.exceptions.landingPads.size());
    }
    m_records.exceptions.sites.append(SiteLandingPad { site, m_landingPadOfBlockPlusOne - 1, handlerTypeOf(m_block.handler().block()) });
}

void StraightEmitter::generateLandingPads(unsigned bytesBelowFrameRecord)
{
    for (size_t index = 0; index < m_landingPads.size(); ++index) {
        LandingPad pad = m_landingPads[index];
        m_landingPadOfBlockPlusOne = 0;
        m_block = m_index.block(pad.from);
        BodyTarget handler = m_block.handler();
        *pad.label = m_jit->label();
        if (!isX86_64())
            m_jit->addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(bytesBelowFrameRecord)), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
        BodyBlock handlerBlock = m_index.block(handler.block());
        m_block = handlerBlock;
        if (handlerTypeOf(handler.block()) == HandlerType::SynthesizedCatch)
            callOperation(Origin { }, RuntimeOperation::Catch, { }, { .followsThrow = true });
        Vector<std::pair<ValueID, ValueID>, 8> moves;
        std::span<const ValueID> arguments = handler.arguments();
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (!m_index.isUnused(handlerBlock.parameter(static_cast<uint32_t>(i))))
                moves.append({ arguments[i], handlerBlock.parameter(static_cast<uint32_t>(i)) });
        }
        moveBetweenSlots(moves.span());
        m_jumps.append({ m_jit->jump(), handler.block() });
    }
}

ThrowSite StraightEmitter::throwSiteWithName(ThrowKind kind, StringID name)
{
    if (!name)
        return { kind, ThrowSiteWord::none, std::nullopt };
    return { .kind = kind, .a = m_records.mentions.add(NumberSpace::ShownString, name), .b = std::nullopt, .aIsMention = true };
}

void emitStraightThrowSite(CCallHelpers& jit, CodeRecords& records, const ThrowSite& site, std::optional<GPRReg> operand, unsigned registerNumberWithoutOperand, uint32_t callSite)
{
    Vector<uint32_t, 3> words = throwSiteWords(site, operand ? Representation::JSValue : Representation::Void, operand ? static_cast<unsigned>(*operand) : registerNumberWithoutOperand);
    uint8_t wordsThatAreMentions = mentionsAmongThrowSiteWords(site, words.size());
    ThrowSiteSource source = !site.b ? ThrowSiteSource::WordInRegister : ThrowSiteSource::WordsAfterCall;
    StubKey key = throwAtSite(source, CallerFrame::HasFrame);
    if (source == ThrowSiteSource::WordInRegister) {
        RELEASE_ASSERT(words.size() <= 2);
        ArgumentMoves moves;
        appendArgumentMove(moves, throwWordGPR, wordsThatAreMentions ? ArgumentSource::ThrowWordWithMention : ArgumentSource::Constant, words.size() == 2 ? ThrowSiteWord::inRegister(words[0], words[1]) : words[0]);
        records.stubCalls.call(jit, key, std::nullopt, OptionalIndex { callSite }, moves, { });
        return;
    }
    records.stubCalls.callForTwoReads(jit, key, { }, OptionalIndex { callSite });
    for (size_t index = 0; index < words.size(); ++index) {
#if CPU(X86_64)
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(ThrowSiteWord::opcodeInFrontOfWordBehindCall));
#endif
        if (wordsThatAreMentions & (1 << index))
            records.relocations.emitNumber32(CodeSymbol { SymbolSpace::Number, words[index] });
        else
            jit.m_assembler.buffer().putInt(words[index]);
    }
}

static ThrowSite checkedStraightThrowSite(const ThrowSite& site)
{
    std::optional<uint32_t> b = site.b == ThrowSiteWord::none ? std::nullopt : site.b;
    return { .kind = site.kind, .a = site.a, .b = b, .aIsMention = site.aIsMention, .bIsMention = b && site.bIsMention };
}

void StraightEmitter::throwAt(Origin origin, const ThrowSite& site, std::optional<GPRReg> boxedOperand)
{
    AllowMacroScratchRegisterUsage allowScratch(*m_jit);
    RELEASE_ASSERT(!boxedOperand || *boxedOperand != throwWordGPR);
    storeWhatWaitsInRegister();
    uint32_t callSite = addSite(origin, { });
    noteSiteMayThrow(callSite);
    emitStraightThrowSite(*m_jit, m_records, checkedStraightThrowSite(site), boxedOperand, 0, callSite);
}

void StraightEmitter::throwError(Origin origin, ErrorKind kind, StringID message)
{
    AllowMacroScratchRegisterUsage allowScratch(*m_jit);
    uint32_t callSite = addSite(origin, { });
    noteSiteMayThrow(callSite);
    emitStraightThrowSite(*m_jit, m_records, checkedStraightThrowSite(throwSiteWithName(ThrowKind::ErrorWithMessage, message)), std::nullopt, static_cast<unsigned>(kind), callSite);
}

void StraightEmitter::emitCallInstruction(StubKey key, const StubSignature& signature, OptionalIndex site, OptionalIndex firstRead, const StraightCallOptions& options, const ArgumentMoves& moves, ImmediateOfThunk immediateOfThunk)
{
    StubCalls& stubCalls = m_records.stubCalls;
    RELEASE_ASSERT(!options.immediate || !options.numberedImmediate);
    uint32_t returnSite;
    if (moves.isEmpty()) {
        returnSite = options.numberedImmediate ? stubCalls.call(*m_jit, key, *options.numberedImmediate, site, firstRead)
            : options.immediate ? stubCalls.call(*m_jit, key, *options.immediate, site, firstRead)
            : stubCalls.call(*m_jit, key, site);
    } else if (options.numberedImmediate)
        returnSite = stubCalls.call(*m_jit, key, *options.numberedImmediate, site, moves, firstRead);
    else if (immediateOfThunk == ImmediateOfThunk::IsMovedByThunk || !options.immediate)
        returnSite = stubCalls.call(*m_jit, key, options.immediate, site, moves, firstRead);
    else {
        RELEASE_ASSERT(!firstRead);
        returnSite = stubCalls.callWithImmediateOfSite(*m_jit, key, options.immediate, site, moves);
    }
    if (options.throwSite) {
        bool namesOperand = !signature.usesCallConvention;
        Vector<uint32_t, 3> words = throwSiteWords(*options.throwSite, namesOperand ? Representation::JSValue : Representation::Void, namesOperand ? signature.operands[0].gpr() : 0);
        uint8_t wordsThatAreMentions = mentionsAmongThrowSiteWords(*options.throwSite, words.size());
        m_records.traps.callThrowSites.append(CallThrowSite { returnSite, WTF::move(words), wordsThatAreMentions });
    }
    if (options.neverReturns)
        m_jit->breakpoint();
}

void StraightEmitter::callStub(const BodyValue& value, StubKey key, std::initializer_list<StraightArgument> arguments, const StraightCallOptions& options)
{
    StraightCallOptions withOpcode = options;
    callStub(options.origin.value_or(value.origin()), key, std::span<const StraightArgument>(arguments.begin(), arguments.size()), withOpcode);
}

void StraightEmitter::callStub(Origin origin, StubKey key, std::span<const StraightArgument> arguments, const StraightCallOptions& options)
{
    AllowMacroScratchRegisterUsage allowScratch(*m_jit);
    const StubCatalog& catalog = m_inputs.stubCatalog;
    const StubSummary& summary = catalog.summaryOf(key);
    const StubSignature& signature = summary.signature;
    RELEASE_ASSERT(!signature.usesCallConvention && arguments.size() == signature.numberOfOperands);
    ArgumentMoves moves;
    bool firstReadNeedsNoThunk = options.readBeforeCall && options.immediate;
    bringAll(arguments, [&](size_t i) {
        return signature.operands[i].gpr();
    }, leavesArgumentsToThunk(key, options) && !firstReadNeedsNoThunk ? &moves : nullptr);

    StubKey boundKey = options.immediate && catalog.bindsImmediate(key) ? key.withImmediate(*options.immediate) : key;
    Effects effects = catalog.effectsOf(boundKey);
    OptionalIndex site;
    OptionalIndex firstRead;
    if (effects.mayExitSideways() || options.capturesStackTrace || (effects.mayCollect && m_hasHandlers && !options.followsThrow)) {
        if (options.readBeforeCall)
            firstRead = { addSite(*options.readBeforeCall, { }) };
        site = { addSite(origin, options) };
        noteSiteMayThrow(*site);
    }

    StubKey called = key;
    if (StubKey form = key.withCallerFrame(CallerFrame::KeepsNoFPRs); key.callerFrame() == CallerFrame::HasFrame && exists(form) && !options.neverReturns && !options.throwSite && !firstRead) {
        StubKey boundForm = options.immediate && catalog.bindsImmediate(form) ? form.withImmediate(*options.immediate) : form;
        if (catalog.summaryOf(boundForm).clobbered != catalog.summaryOf(boundKey).clobbered)
            called = form;
    }
    emitCallInstruction(called, signature, site, firstRead, options, moves);
}

static OperationCall::Returns returnsOfStraightCall(RuntimeValueKind result)
{
    switch (result) {
    case RuntimeValueKind::Void:
        return OperationCall::Returns::Void;
    case RuntimeValueKind::Double:
        return OperationCall::Returns::Double;
    case RuntimeValueKind::Pair:
        return OperationCall::Returns::Pair;
    default:
        return OperationCall::Returns::Value;
    }
}

void StraightEmitter::callOperation(const BodyValue& value, RuntimeOperation operation, std::initializer_list<StraightArgument> arguments, const StraightCallOptions& options)
{
    callOperation(options.origin.value_or(value.origin()), operation, std::span<const StraightArgument>(arguments.begin(), arguments.size()), options);
}

void StraightEmitter::callOperation(Origin origin, RuntimeOperation operation, std::span<const StraightArgument> arguments, const StraightCallOptions& options)
{
    AllowMacroScratchRegisterUsage allowScratch(*m_jit);
    RuntimeSignature signature = signatureOf(operation);
    AOT::Entry entry = entryOf(operation);
    RELEASE_ASSERT(arguments.size() == signature.parameters.size() && !options.immediate && !options.numberedImmediate);
    OperationCall::FirstArgument first = isLeaf(operation) ? OperationCall::FirstArgument::None : OperationCall::FirstArgument::Instance;
    m_records.totals.entriesUsed.set(static_cast<size_t>(entry));

    unsigned firstGPR = first != OperationCall::FirstArgument::None;
    for (size_t i = 0; i < arguments.size(); ++i)
        RELEASE_ASSERT(signature.parameters[i] != RuntimeValueKind::Double && firstGPR + i < numberOfOperationArgumentGPRs);
    ArgumentMoves moves;
    bringAll(arguments, [&](size_t i) {
        return operationArgumentGPR(firstGPR + static_cast<unsigned>(i));
    }, leavesArgumentsToThunk(callOperationFor(entry, returnsOfStraightCall(signature.result), first), options) ? &moves : nullptr);

    Effects effects = AOT::effectsOf(entry);
    OptionalIndex site;
    if (effects.mayExitSideways() || options.capturesStackTrace || (effects.mayCollect && m_hasHandlers && !options.followsThrow)) {
        site = { addSite(origin, options) };
        noteSiteMayThrow(*site);
    }
    StubKey key = callOperationFor(entry, returnsOfStraightCall(signature.result), first);
    StraightCallOptions withImmediate = options;
    withImmediate.immediate = offsetInRuntimeTable(entry);
    emitCallInstruction(key, m_inputs.stubCatalog.summaryOf(key).signature, site, { }, withImmediate, moves, ImmediateOfThunk::IsMovedByThunk);
}

void StraightEmitter::callByConvention(const BodyValue& value, StubKey stub, std::span<const StraightArgument> stubOperands, std::optional<StraightArgument> callee, std::optional<StraightArgument> thisValue, std::span<const ValueID> arguments, const StraightCallOptions& options)
{
    AllowMacroScratchRegisterUsage allowScratch(*m_jit);
    const StubSignature& signature = m_inputs.stubCatalog.summaryOf(stub).signature;
    RELEASE_ASSERT(signature.usesCallConvention && stubOperands.size() == signature.numberOfOperands);
    for (size_t i = numberOfArgumentGPRs; i < arguments.size(); ++i)
        storeInScratchArea(arguments[i], static_cast<uint32_t>(i - numberOfArgumentGPRs));
    Vector<StraightArgument, 12> passed;
    Vector<GPRReg, 12> registers;
    for (size_t i = 0; i < arguments.size() && i < numberOfArgumentGPRs; ++i) {
        passed.append(StraightArgument(arguments[i]));
        registers.append(argumentGPR(static_cast<unsigned>(i)));
    }
    for (size_t i = 0; i < stubOperands.size(); ++i) {
        passed.append(stubOperands[i]);
        registers.append(signature.operands[i].gpr());
    }
    if (callee) {
        passed.append(*callee);
        registers.append(calleeGPR);
    }
    if (thisValue) {
        passed.append(*thisValue);
        registers.append(thisGPR);
    }
    ArgumentMoves moves;
    bool firstReadNeedsNoThunk = options.readBeforeCall && options.immediate;
    bringAll(passed.span(), [&](size_t i) {
        return registers[i];
    }, leavesArgumentsToThunk(stub, options) && !firstReadNeedsNoThunk ? &moves : nullptr);

    OptionalIndex firstRead;
    if (options.readBeforeCall)
        firstRead = { addSite(*options.readBeforeCall, { }) };
    uint32_t site = addSite(options.origin.value_or(value.origin()), options, value.opcode());
    noteSiteMayThrow(site);
    emitCallInstruction(stub, signature, { site }, firstRead, options, moves);
}

void StraightEmitter::takeParameters(const ArgumentLocations& locations)
{
    BodyBlock entry = m_index.block(entryBlock);
    RELEASE_ASSERT(locations.parameters.size() == entry.numberOfParameters());
    for (bool readsStackWords : { false, true }) {
        for (uint32_t index = 0; index < entry.numberOfParameters(); ++index) {
            const ArgumentLocation& location = locations.parameters[index];
            ValueID parameter = entry.parameter(index);
            if (m_index.isUnused(parameter) || (location.kind == ArgumentLocation::Kind::Stack) != readsStackWords)
                continue;
            switch (location.kind) {
            case ArgumentLocation::Kind::None:
                break;
            case ArgumentLocation::Kind::GPR:
                boxInForm(location.representation, location.reg.gpr());
                m_jit->store64(location.reg.gpr(), addressOfSlot(parameter));
                break;
            case ArgumentLocation::Kind::FPR:
                RELEASE_ASSERT(location.representation == Representation::Double);
                m_jit->storeDouble(location.reg.fpr(), addressOfSlot(parameter));
                break;
            case ArgumentLocation::Kind::Stack: {
                GPRReg result = takeResultRegister();
                if (location.representation == Representation::Double) {
                    FPRReg number = takeResultFPR();
                    m_jit->loadDouble(CCallHelpers::Address(GPRInfo::callFrameRegister, offsetOfStackWordFromFrame(location.stackIndex)), number);
                    boxDouble(number, result);
                } else {
                    m_jit->load64(CCallHelpers::Address(GPRInfo::callFrameRegister, offsetOfStackWordFromFrame(location.stackIndex)), result);
                    boxInForm(location.representation, result);
                }
                m_jit->store64(result, addressOfSlot(parameter));
                break;
            }
            }
        }
    }
    for (uint32_t index = 0; index < entry.numberOfParameters(); ++index) {
        ValueID parameter = entry.parameter(index);
        TypeID type = entry.parameterType(index);
        if (m_index.isUnused(parameter) || !type || holdsOnlyPrimitives(type))
            continue;
        const Type& declared = m_types.at(type);
        if (!declared.environment)
            continue;
        const PlannedLayout& planned = m_inputs.plan.layout(declared.environment);
        if (!planned.cellsAreHandedBack)
            continue;
        GPRReg environment = takeResultRegister();
        m_jit->load64(addressOfSlot(parameter), environment);
        CCallHelpers::Jump isNoCell;
        if (declared.primitives)
            isNoCell = m_jit->branchIfNotCell(environment);
        uint32_t shape = m_inputs.plan.environmentAllocation(declared.environment).shape;
        m_jit->store8(CCallHelpers::TrustedImm32(highByteOfEnvironmentShape(shape) | environmentIsEscapedBit), CCallHelpers::Address(environment, offsetOfEnvironmentShapeHighByte));
        if (isNoCell.isSet())
            isNoCell.link(m_jit);
    }
    for (uint32_t index = 0; index < entry.numberOfParameters(); ++index) {
        ValueID parameter = entry.parameter(index);
        if (locations.parameters[index].kind != ArgumentLocation::Kind::FPR || m_index.isUnused(parameter))
            continue;
        GPRReg result = takeResultRegister();
        FPRReg number = takeResultFPR();
        m_jit->loadDouble(addressOfSlot(parameter), number);
        boxDouble(number, result);
        m_jit->store64(result, addressOfSlot(parameter));
    }
}

void StraightEmitter::generate(CCallHelpers& jit, const ArgumentLocations& locations, unsigned bytesBelowFrameRecord)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    m_jit = &jit;
    if (m_hasFailed)
        return;
    uint32_t numberOfValues = m_index.numberOfValues();
    m_roots.grow(numberOfValues);
    m_slots.fill(0, numberOfValues);
    m_firstValueGivenBackAt.fill(0, numberOfValues + 1);
    m_nextValueGivenBack.fill(0, numberOfValues);
    for (uint32_t value = 0; value < numberOfValues; ++value)
        m_roots[value] = value;
    m_labels.grow(m_index.numberOfBlocks());
    m_blocksGenerated.ensureSize(m_index.numberOfBlocks());

    m_index.body().forEachBlock([&](BodyBlock block) {
        m_hasHandlers = m_hasHandlers || block.hasHandler();
    });

    takeParameters(locations);
    if (facts().isTakenToRunOnce(m_unit.function)) {
        StraightArgument function = StraightArgument::word(m_unit.function.index());
        callOperation(Origin { }, RuntimeOperation::EnterBodyThatRunsOnce, singleElementSpan(function), { });
    }
    uint32_t endOfImportChecks = 0;
    m_index.body().forEachBlock([&](BodyBlock block) {
        if (m_hasFailed || isNeverEntered(block.id()) || block.id().index() < endOfImportChecks)
            return;
        if (Vector<ImportCheck, 8> checks = importChecksFrom(block); checks.size() >= fewestImportChecksInList) {
            endOfImportChecks = block.id().index() + 2 * static_cast<uint32_t>(checks.size());
            generateImportChecks(block, WTF::move(checks));
            return;
        }
        generateBlock(block);
    });
    if (!m_hasFailed)
        generateLandingPads(bytesBelowFrameRecord);
    if (m_hasFailed)
        return;
    for (JumpToBlock& jump : m_jumps)
        jump.jump.linkTo(m_labels[jump.block.index()], &jit);
    writeListsOfImportChecks();
}

std::optional<StraightEmitter::ImportCheck> StraightEmitter::importCheckIn(BodyBlock block) const
{
    uint32_t place = block.id().index();
    if (block.hasHandler() || block.numberOfValues() != 2 || place + 2 >= m_index.numberOfBlocks())
        return std::nullopt;
    BodyValue read = m_index.value(block.firstValue());
    BodyValue test = m_index.value(ValueID(block.firstValue().index() + 1));
    BodyTerminator terminator = block.terminator();
    if (read.opcode() != Opcode::GetModuleVariable || test.opcode() != Opcode::TypeTest || terminator.kind() != TerminatorKind::Branch)
        return std::nullopt;
    if (test.valueOperand() != read.id() || test.testedType() != TypeTable::absent || terminator.conditionOperand() != test.id() || !m_index.hasOneUse(test.id()))
        return std::nullopt;
    BodyTarget taken = terminator.taken();
    BodyTarget notTaken = terminator.notTaken();
    if (taken.block().index() != place + 1 || notTaken.block().index() != place + 2 || !taken.arguments().empty() || !notTaken.arguments().empty())
        return std::nullopt;
    if (isNeverEntered(taken.block()) || isNeverEntered(notTaken.block()) || !m_index.onlyIncomingEdge(taken.block()) || !m_index.onlyIncomingEdge(notTaken.block()))
        return std::nullopt;
    BodyBlock failing = m_index.block(taken.block());
    BodyBlock next = m_index.block(notTaken.block());
    uint32_t numberOfProofs = static_cast<uint32_t>(taken.proofs().size() + notTaken.proofs().size());
    if (m_index.useCount(read.id()) != numberOfProofs + 1 || failing.numberOfParameters() != taken.proofs().size() || next.numberOfParameters() != notTaken.proofs().size())
        return std::nullopt;
    for (BodyBlock successor : { failing, next }) {
        for (uint32_t index = 0; index < successor.numberOfParameters(); ++index) {
            if (!m_index.isUnused(successor.parameter(index)))
                return std::nullopt;
        }
    }
    if (failing.hasHandler() || failing.numberOfValues() != 1 || failing.terminator().kind() != TerminatorKind::Unreachable)
        return std::nullopt;
    BodyValue thrown = m_index.value(failing.firstValue());
    if (thrown.opcode() != Opcode::ThrowModuleFailure || thrown.isForRequire())
        return std::nullopt;
    PlannedVariable planned = m_inputs.plan.variable(read.variable());
    if (!planned.hasWordInStatics() || planned.encoding != VariableEncoding::Plain || planned.representation != SlotRepresentation::JSValue || m_inputs.plan.deferredDefinitions().findDeferred(read.variable()))
        return std::nullopt;
    return ImportCheck { read.variable(), thrown.failure() };
}

Vector<StraightEmitter::ImportCheck, 8> StraightEmitter::importChecksFrom(BodyBlock block) const
{
    Vector<ImportCheck, 8> checks;
    while (std::optional<ImportCheck> check = importCheckIn(block)) {
        checks.append(*check);
        block = m_index.block(BlockID(block.id().index() + 2));
    }
    return checks;
}

void StraightEmitter::generateImportChecks(BodyBlock first, Vector<ImportCheck, 8>&& checks)
{
    uint32_t numberOfChecks = static_cast<uint32_t>(checks.size());
    m_block = first;
    m_landingPadOfBlockPlusOne = 0;
    m_valueCopiedInRegister = { };
    m_labels[first.id().index()] = m_jit->label();
    ListOfImportChecks list;
    list.entry = static_cast<uint16_t>(firstEntryOfListOfImportChecks + m_listsOfImportChecks.size());
    list.checks = WTF::move(checks);
    Origin origin;
    for (uint32_t index = 0; index < numberOfChecks; ++index) {
        BodyBlock block = m_index.block(BlockID(first.id().index() + 2 * index));
        m_blocksGenerated.quickSet(block.id().index());
        m_blocksGenerated.quickSet(block.id().index() + 1);
        if (!index)
            origin = m_index.value(m_index.block(BlockID(block.id().index() + 1)).firstValue()).origin();
    }
    GPRReg pairs = operationArgumentGPR(1);
    m_records.stubCalls.moveAddressOfFunction(m_unit, list.entry, pairs);
    StraightArgument arguments[] { StraightArgument::inRegister(pairs), StraightArgument::word(numberOfChecks) };
    callOperation(origin, RuntimeOperation::ThrowIfImportIsMissing, arguments, { });
    m_listsOfImportChecks.append(WTF::move(list));
}

void StraightEmitter::writeListsOfImportChecks()
{
    for (const ListOfImportChecks& list : m_listsOfImportChecks) {
        m_records.entries.append(RecordedEntry { list.entry, m_jit->label() });
        for (const ImportCheck& check : list.checks) {
            m_records.relocations.emitNumber32({ SymbolSpace::Number, m_records.mentions.add(NumberSpace::Variable, check.variable) });
            m_jit->m_assembler.buffer().putInt(static_cast<int32_t>(check.failure.index()));
        }
    }
}

void StraightEmitter::generateBlock(BodyBlock block)
{
    m_block = block;
    m_landingPadOfBlockPlusOne = 0;
    m_labels[block.id().index()] = m_jit->label();
    m_blocksGenerated.quickSet(block.id().index());
    BodyTerminator terminator = block.terminator();
    m_returnedCall = terminator.kind() == TerminatorKind::ReturnCall ? terminator.callOperand() : ValueID();
    m_valueCopiedInRegister = { };
    findProofsNobodyReads(terminator);
    for (uint32_t index = 0; index < block.numberOfParameters(); ++index) {
        if (!m_index.isUnused(block.parameter(index)))
            addressOfSlot(block.parameter(index));
    }
    for (BodyValue value : block.values()) {
        if (value.id() != m_returnedCall)
            generateOperation(value);
        if (m_hasFailed)
            return;
        giveBackSlotsReadLastAt(value.id().index());
    }
    generateTerminator(block, terminator);
    RELEASE_ASSERT(m_hasFailed || !m_valueInRegister);
    giveBackSlotsReadLastAt(block.endOfValues().index());
}

bool StraightEmitter::leavesDefinitionToFirstRead(const BodyValue& value)
{
    static constexpr OpcodeSet opcodesOfDefinitions = inputPassThroughOpcodes | OpcodeSet { Opcode::NewRegExp, Opcode::Data, Opcode::PutModuleVariable };
    const DeferredDefinitions& definitions = m_inputs.plan.deferredDefinitions();
    if (!opcodesOfDefinitions.contains(value.opcode()) || !definitions.bodiesThatDefine.get(m_unit.function.index()))
        return false;
    ValueID made = value.opcode() == Opcode::PutModuleVariable ? value.valueOperand() : value.id();
    while (!m_index.isParameter(made) && inputPassThroughOpcodes.contains(m_index.opcodeOf(made)))
        made = m_index.value(made).operand(0);
    const DeferredVariable* deferred = m_index.isParameter(made) ? nullptr : definitions.findMadeBy(m_unit.function, made);
    if (!deferred)
        return false;
    if (value.opcode() == Opcode::PutModuleVariable && deferred->isMarkedAtDeclaration) {
        JSValue mark = deferred->mark == Primitive::Undefined ? jsUndefined() : deferred->mark == Primitive::Null ? jsNull() : JSValue();
        GPRReg result = takeResultRegister();
        m_jit->move(CCallHelpers::TrustedImm64(JSValue::encode(mark)), result);
        storeVariable(deferred->variable, result);
    }
    return true;
}

template<typename OperationOrTerminator>
bool StraightEmitter::readsWhatWaitsInRegister(const OperationOrTerminator& reader) const
{
    bool reads = false;
    reader.forEachOperand([&](ValueID operand, uint32_t) {
        reads = reads || (operand && waitsInRegister(operand));
    });
    return reads;
}

void StraightEmitter::generateOperation(const BodyValue& value)
{
    if (isReadByNobody(value.id()) || leavesDefinitionToFirstRead(value))
        return;
    if (m_valueInRegister && !inputIdentityOpcodes.contains(value.opcode()) && !opcodesWithoutCodeOfTheirOwn.contains(value.opcode()) && !readsWhatWaitsInRegister(value))
        storeWhatWaitsInRegister();
    switch (familyOf(value.opcode())) {
    case OperationFamily::Constant:
        generateConstant(value);
        return;
    case OperationFamily::Cast:
        generateCast(value);
        return;
    case OperationFamily::Operator:
        generateOperator(value);
        return;
    case OperationFamily::Access:
        generateAccess(value);
        return;
    case OperationFamily::Call:
        generateCall(value);
        return;
    case OperationFamily::Binding:
        generateBinding(value);
        return;
    case OperationFamily::Hint:
        generateHint(value);
        return;
    case OperationFamily::Allocation:
        if (opcodesOfArrays().contains(value.opcode()))
            generateArray(value);
        else
            generateAllocation(value);
        return;
    case OperationFamily::ArrayIntrinsic:
        generateArrayIntrinsic(value);
        return;
    case OperationFamily::Class:
        generateClass(value);
        return;
    case OperationFamily::CollectionIntrinsic:
        generateCollectionIntrinsic(value);
        return;
    case OperationFamily::EngineIntrinsic:
        generateEngineIntrinsic(value);
        return;
    case OperationFamily::Eval:
        generateEval(value);
        return;
    case OperationFamily::Exception:
        generateException(value);
        return;
    case OperationFamily::ForIn:
        generateForIn(value);
        return;
    case OperationFamily::Iteration:
        generateIteration(value);
        return;
    case OperationFamily::Module:
        generateModule(value);
        return;
    case OperationFamily::PrivateAccess:
        generatePrivateAccess(value);
        return;
    case OperationFamily::PromiseIntrinsic:
        generatePromiseIntrinsic(value);
        return;
    case OperationFamily::RuntimeIntrinsic:
        generateRuntimeIntrinsic(value);
        return;
    default:
        fail(value);
        return;
    }
}

bool StraightEmitter::edgeMovesNothing(const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target)
{
    BodyBlock successor = m_index.block(target.block());
    for (size_t i = 0; i < target.arguments().size(); ++i) {
        if (!m_index.isUnused(successor.parameter(static_cast<uint32_t>(i))))
            return false;
    }
    uint32_t numberOfProofs = static_cast<uint32_t>(target.proofs().size());
    if (!numberOfProofs || (!m_blocksGenerated.quickGet(target.block().index()) && m_index.onlyIncomingEdge(target.block())))
        return true;
    for (uint32_t proof = 0; proof < numberOfProofs; ++proof) {
        std::optional<BodyEdgeResult> result = m_index.edgeResult(terminator, targetIndex, target, proof);
        if (result && !m_index.isUnused(result->parameter))
            return false;
    }
    return true;
}

void StraightEmitter::passArguments(const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target)
{
    BodyBlock successor = m_index.block(target.block());
    std::span<const ValueID> arguments = target.arguments();
    uint32_t numberOfProofs = static_cast<uint32_t>(target.proofs().size());
    bool namesSubjects = numberOfProofs && !m_blocksGenerated.quickGet(target.block().index()) && m_index.onlyIncomingEdge(target.block());

    Vector<std::pair<ValueID, ValueID>, 8> moves;
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (!m_index.isUnused(successor.parameter(static_cast<uint32_t>(i))))
            moves.append({ arguments[i], successor.parameter(static_cast<uint32_t>(i)) });
    }
    for (uint32_t proof = 0; proof < numberOfProofs; ++proof) {
        std::optional<BodyEdgeResult> result = m_index.edgeResult(terminator, targetIndex, target, proof);
        if (!result || m_index.isUnused(result->parameter))
            continue;
        if (namesSubjects)
            m_roots[result->parameter.index()] = m_roots[result->subject.index()];
        else
            moves.append({ result->subject, result->parameter });
    }
    moveBetweenSlots(moves.span());
}

void StraightEmitter::moveBetweenSlots(std::span<const std::pair<ValueID, ValueID>> moves)
{
    storeWhatWaitsInRegister();
    for (size_t first = 0; first < moves.size(); first += numberOfTemporaries) {
        size_t end = std::min<size_t>(moves.size(), first + numberOfTemporaries);
        if (end < moves.size()) {
            for (size_t later = end; later < moves.size(); ++later) {
                for (size_t earlier = first; earlier < end; ++earlier) {
                    if (!isConstant(moves[later].first) && rootOf(moves[later].first) == rootOf(moves[earlier].second)) {
                        fail(Opcode::Undefined);
                        return;
                    }
                }
            }
        }
        for (size_t i = first; i < end; ++i)
            load(moves[i].first, temporary(static_cast<unsigned>(i - first)));
        for (size_t i = first; i < end; ++i)
            m_jit->store64(temporary(static_cast<unsigned>(i - first)), addressOfSlot(moves[i].second));
    }
}

void StraightEmitter::jumpTo(BlockID to)
{
    if (to.index() == m_block.id().index() + 1)
        return;
    m_jumps.append({ m_jit->jump(), to });
}

void StraightEmitter::jumpTo(CCallHelpers::Jump jump, BlockID to)
{
    m_jumps.append({ jump, to });
}

void StraightEmitter::tearDownFrame()
{
    RELEASE_ASSERT(!m_valueInRegister);
    m_jit->emitFunctionEpilogue();
}

void StraightEmitter::branchOnCondition(ValueID condition, bool jumpsIfTrue, BlockID target)
{
    if (waitsInRegister(condition) && m_formInRegister == FormInRegister::Test) {
        GPRReg word = registerOfWaitingValue(condition);
        m_valueTakenOutOfRegister = std::exchange(m_valueInRegister, { });
        if (m_testInRegister.mask)
            m_jit->and64(CCallHelpers::TrustedImm32(m_testInRegister.mask), word);
        m_jumps.append({ m_jit->branch64(jumpsIfTrue ? CCallHelpers::Equal : CCallHelpers::NotEqual, word, CCallHelpers::TrustedImm32(m_testInRegister.word)), target });
        return;
    }
    if (waitsInRegister(condition) && m_formInRegister == FormInRegister::ZeroOrOne) {
        GPRReg zeroOrOne = registerOfWaitingValue(condition);
        m_valueTakenOutOfRegister = std::exchange(m_valueInRegister, { });
        m_jumps.append({ m_jit->branchTest32(jumpsIfTrue ? CCallHelpers::NonZero : CCallHelpers::Zero, zeroOrOne), target });
        return;
    }
    if (!givesBoolean(condition)) {
        StraightArgument tested(condition);
        callStub(Origin { }, AOT::toBoolean(), { &tested, 1 }, { });
        m_jumps.append({ m_jit->branchTest32(jumpsIfTrue ? CCallHelpers::NonZero : CCallHelpers::Zero, resultOfCall()), target });
        return;
    }
    GPRReg boxed = loadIntoResultRegister(condition);
    m_jumps.append({ m_jit->branch32(jumpsIfTrue ? CCallHelpers::Equal : CCallHelpers::NotEqual, boxed, CCallHelpers::TrustedImm32(JSValue::ValueTrue)), target });
}

bool StraightEmitter::givesBoolean(ValueID value) const
{
    static const OpcodeSet opcodesThatGiveBooleans = opcodesWhere([](Opcode opcode) {
        return formOfResultOfEvery(opcode) == Representation::Boolean || opcode == Opcode::Boolean;
    });
    ValueID root = rootOf(value);
    return !m_index.isParameter(root) && opcodesThatGiveBooleans.contains(m_index.opcodeOf(root));
}

void StraightEmitter::generateReturn(const BodyTerminator& terminator)
{
    returnToCaller(terminator.valueOperand(), terminator.origin());
}

void StraightEmitter::returnToCaller(ValueID returned, Origin origin)
{
    if (TypeID held = plan().typeHeldOfResult(program(), facts().linked(), m_unit); held && m_types.at(held).isJavaScriptValue()) {
        trapUnlessOfType(returned, m_types.testablePartOf(held), ProofSource::ResultOfCall, origin);
        if (m_hasFailed)
            return;
    }
    if (returned)
        loadIntoResultRegister(returned);
    else
        m_jit->move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), takeResultRegister());
    Representation representation = conventions().resultRepresentation(m_unit);
    if (representation == Representation::Double)
        unboxNumber(resultGPR, resultFPR);
    else
        unboxIntoForm(representation, resultGPR);
    tearDownFrame();
    m_jit->ret();
}

void StraightEmitter::generateTerminator(BodyBlock block, const BodyTerminator& terminator)
{
    if (m_valueInRegister && !readsWhatWaitsInRegister(terminator) && !(m_returnedCall && readsWhatWaitsInRegister(m_index.value(m_returnedCall))))
        storeWhatWaitsInRegister();
    switch (terminator.kind()) {
    case TerminatorKind::Jump:
        passArguments(terminator, 0, terminator.next());
        jumpTo(terminator.next().block());
        return;
    case TerminatorKind::Branch: {
        BodyTarget taken = terminator.taken();
        BodyTarget notTaken = terminator.notTaken();
        ValueID condition = terminator.conditionOperand();
        if (std::optional<JSValue> constant = constantOf(condition); constant && constant->isBoolean()) {
            bool isTaken = constant->isTrue();
            passArguments(terminator, isTaken ? 0 : 1, isTaken ? taken : notTaken);
            jumpTo((isTaken ? taken : notTaken).block());
            return;
        }
        if (isNeverEntered(taken.block()) || isNeverEntered(notTaken.block())) {
            bool isTaken = isNeverEntered(notTaken.block());
            if (waitsInRegister(condition))
                m_valueInRegister = { };
            passArguments(terminator, isTaken ? 0 : 1, isTaken ? taken : notTaken);
            jumpTo((isTaken ? taken : notTaken).block());
            return;
        }
        bool takenMovesNothing = edgeMovesNothing(terminator, 0, taken);
        bool notTakenMovesNothing = edgeMovesNothing(terminator, 1, notTaken);
        bool takenIsNext = taken.block().index() == block.id().index() + 1;
        if (notTakenMovesNothing && (takenIsNext || !takenMovesNothing)) {
            branchOnCondition(condition, false, notTaken.block());
            passArguments(terminator, 1, notTaken);
            passArguments(terminator, 0, taken);
            jumpTo(taken.block());
            return;
        }
        if (takenMovesNothing) {
            branchOnCondition(condition, true, taken.block());
            passArguments(terminator, 0, taken);
            passArguments(terminator, 1, notTaken);
            jumpTo(notTaken.block());
            return;
        }
        CCallHelpers::Jump isFalse;
        if (givesBoolean(condition))
            isFalse = m_jit->branch32(CCallHelpers::NotEqual, loadIntoResultRegister(condition), CCallHelpers::TrustedImm32(JSValue::ValueTrue));
        else {
            StraightArgument tested(condition);
            callStub(Origin { }, AOT::toBoolean(), { &tested, 1 }, { });
            isFalse = m_jit->branchTest32(CCallHelpers::Zero, resultOfCall());
        }
        passArguments(terminator, 0, taken);
        m_jumps.append({ m_jit->jump(), taken.block() });
        isFalse.link(m_jit);
        passArguments(terminator, 1, notTaken);
        jumpTo(notTaken.block());
        return;
    }
    case TerminatorKind::Return:
        generateReturn(terminator);
        return;
    case TerminatorKind::Switch:
        generateSwitch(block, terminator);
        return;
    case TerminatorKind::SwitchString:
        generateSwitchOnString(terminator);
        return;
    case TerminatorKind::ReturnCall:
        generateReturnCall(terminator);
        return;
    case TerminatorKind::Throw: {
        StraightArgument thrown(terminator.valueOperand());
        callOperation(terminator.origin(), RuntimeOperation::Throw, { &thrown, 1 }, { .neverReturns = true });
        return;
    }
    case TerminatorKind::Rethrow: {
        StraightArgument thrown(terminator.exceptionOperand());
        callOperation(terminator.origin(), RuntimeOperation::Throw, { &thrown, 1 }, { .neverReturns = true });
        return;
    }
    case TerminatorKind::Unreachable:
        m_jit->breakpoint();
        return;
    default:
        fail(Opcode::Undefined);
        return;
    }
}

void StraightEmitter::generateSwitch(BodyBlock block, const BodyTerminator& terminator)
{
    auto cases = terminator.cases();
    uint32_t numberOfCases = static_cast<uint32_t>(cases.size());
    Vector<BodyTarget, 16> targets;
    for (BodyTarget target : terminator.targets())
        targets.append(target);
    RELEASE_ASSERT(targets.size() == numberOfCases + 1);

    std::optional<uint32_t> onlyTargetEntered;
    uint32_t numberOfTargetsEntered = 0;
    for (uint32_t index = 0; index <= numberOfCases; ++index) {
        if (isNeverEntered(targets[index].block()))
            continue;
        ++numberOfTargetsEntered;
        onlyTargetEntered = index;
    }
    if (numberOfTargetsEntered <= 1) {
        if (waitsInRegister(terminator.subjectOperand()))
            m_valueInRegister = { };
        if (!numberOfTargetsEntered) {
            m_jit->breakpoint();
            return;
        }
        passArguments(terminator, *onlyTargetEntered, targets[*onlyTargetEntered]);
        jumpTo(targets[*onlyTargetEntered].block());
        return;
    }

    GPRReg subject = temporary(0);
    load(terminator.subjectOperand(), subject);
    GPRReg integer = takeResultRegister();
    CCallHelpers::JumpList goesToOtherwise;
    CCallHelpers::Jump isInt32 = m_jit->branchIfInt32(subject);
    goesToOtherwise.append(m_jit->branchIfNotNumber(subject));
    StraightArgument passed = StraightArgument::inRegister(subject);
    callOperation(terminator.origin(), RuntimeOperation::TryConvertToInteger, { &passed, 1 }, { });
    CCallHelpers::Jump isConverted = m_jit->jump();
    isInt32.link(m_jit);
    m_jit->signExtend32ToPtr(subject, integer);
    isConverted.link(m_jit);

    for (uint32_t index = 0; index < numberOfCases; ++index) {
        if (isNeverEntered(targets[index].block()))
            continue;
        int64_t caseValue = cases[index];
        if (caseValue != static_cast<int32_t>(caseValue)) {
            fail(Opcode::Undefined);
            return;
        }
        if (edgeMovesNothing(terminator, index, targets[index])) {
            passArguments(terminator, index, targets[index]);
            m_jumps.append({ m_jit->branch64(CCallHelpers::Equal, integer, CCallHelpers::TrustedImm32(static_cast<int32_t>(caseValue))), targets[index].block() });
            continue;
        }
        CCallHelpers::Jump isOther = m_jit->branch64(CCallHelpers::NotEqual, integer, CCallHelpers::TrustedImm32(static_cast<int32_t>(caseValue)));
        passArguments(terminator, index, targets[index]);
        m_jumps.append({ m_jit->jump(), targets[index].block() });
        isOther.link(m_jit);
    }
    goesToOtherwise.link(m_jit);
    if (isNeverEntered(targets[numberOfCases].block())) {
        m_jit->breakpoint();
        return;
    }
    passArguments(terminator, numberOfCases, targets[numberOfCases]);
    jumpTo(targets[numberOfCases].block());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
