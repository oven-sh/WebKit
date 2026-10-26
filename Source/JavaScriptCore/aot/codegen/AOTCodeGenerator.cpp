/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBranchCounterNumbers.h"
#include "AOTCastEmitter.h"
#include "AOTCodeRecords.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTEdgeResults.h"
#include "AOTEntryEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTOperatorEmitter.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTPromisedResults.h"
#include "AOTRepresentationEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTStaticsEmitter.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include "B3UpsilonValue.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

const EffectsOnEdges& OperationContext::effects() const
{
    return m_valueID ? m_analyses.effectsOf(m_valueID) : m_analyses.effectsOfTerminator(m_block);
}

static void setOrigin(Output& out, ValueID id)
{
    out.setOrigin(b3OriginOf(id).dfgOrigin());
}

static bool hasParameterWithRegister(const Function& function, BlockID block)
{
    return std::ranges::any_of(function.at(block).parameters, [&](ValueID parameter) {
        return hasRegister(function.at(parameter).representation);
    });
}

// What the operation in the scope converts for itself, it alone finds. What is converted in front of an operation of the block that is being generated, every later one of the block finds.
class CodeGenerator::OperationScope {
    WTF_MAKE_NONCOPYABLE(OperationScope);
public:
    OperationScope(CodeGenerator& generator, const OperationContext& operation)
        : m_generator(generator)
        , m_outerOperation(std::exchange(generator.m_operation, &operation))
        , m_operandsOfOuterOperation(std::exchange(generator.m_convertedOperands, { }))
        , m_numberOfSharedForms(generator.m_sharedForms.size())
    {
    }

    ~OperationScope()
    {
        if (m_outerOperation)
            m_generator.dropSharedFormsSince(m_numberOfSharedForms);
        m_generator.m_operation = m_outerOperation;
        m_generator.m_convertedOperands = WTF::move(m_operandsOfOuterOperation);
    }

private:
    CodeGenerator& m_generator;
    const OperationContext* m_outerOperation;
    Vector<ConvertedOperand, 4> m_operandsOfOuterOperation;
    size_t m_numberOfSharedForms;
};

CodeGenerator::IncomingOperand CodeGenerator::takeIncomingOperand(Edge edge, uint32_t parameterIndex, std::span<const Operand>& declaredResults) const
{
    IncomingValue incoming = incomingValueOf(function(), edge, parameterIndex);
    switch (incoming.kind) {
    case IncomingValue::Kind::Argument:
        return { getAsDefined(incoming.argument), incoming.argument };
    case IncomingValue::Kind::EdgeResult:
        break;
    }

    EdgeResult result = edgeResultAt(function(), incoming.result);
    switch (result.kind) {
    case EdgeResultKind::NarrowedOperand:
        return { getAsDefined(result.subject), result.subject, incoming.result };
    case EdgeResultKind::Guard:
        return { };
    case EdgeResultKind::DeclaredResult: {
        RELEASE_ASSERT(!declaredResults.empty());
        Operand operand = declaredResults.front();
        declaredResults = declaredResults.subspan(1);
        return { operand, { }, incoming.result };
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

GenerationResult CodeGenerator::run()
{
    RELEASE_ASSERT(function().representationsAreChosen);
    if (!recordSitesWithoutCode())
        return GenerationResult::Failed;

    m_values.fill(nullptr, function().values.size());
    m_resultRanges.grow(function().values.size());
    m_newestSharedFormOf.fill(0, function().values.size());
    m_blockOfParameter.grow(function().values.size());
    m_valuesOutliveNoSuspension = m_inputs.frameMaps.numberOfSuspensions() || function().role == FunctionRole::CoroutineBody;
    createBlocks();

    m_out.initializeConstants(m_proc, m_out.m_block);
    definePinnedValues(m_out, m_proc, m_inputs.program.instanceSizes, m_pinned);
    emitter<EntryEmitter>().generateEntry();
    if (m_hasFailed)
        return GenerationResult::Failed;
    createPhis();
    if (std::optional<uint32_t> number = m_inputs.program.branchCounters.counterOfEntries(m_inputs.id)) {
        TypedPointer counter = emitter<StaticsEmitter>().branchCounter(*number);
        m_out.store64(m_out.add(m_out.load64(counter), m_out.constInt64(1)), counter);
    }
    m_out.jump(blockFor(entryBlock));

    for (BlockID block : analyses().cfg().reversePostOrder()) {
        generateBlock(block);
        if (m_hasFailed)
            return GenerationResult::Failed;
    }
    publishInductionVariables();
    finishProcedure();
    return GenerationResult::Generated;
}

bool CodeGenerator::recordSitesWithoutCode()
{
    Subject subject { .function = m_inputs.id };
    size_t numberOfInlineFrames = function().inlineFrames.size();
    if (numberOfInlineFrames > build().limit(LimitID::InlineFramesPerFunction)) {
        failAtLimit(LimitID::InlineFramesPerFunction, numberOfInlineFrames, subject);
        return false;
    }

    SiteRecords& sites = m_records.sites;
    for (auto& frame : function().inlineFrames) {
        bool isSelfFunction = sites.inlineCallSites.isEmpty();
        if (isSelfFunction) {
            sites.inlineCallSites.append(0);
            continue;
        }
        sites.inlineCallSites.append(sites.addSite({ .origin = { .position = frame.callPosition, .inlineFrame = frame.parent, .isPartOfConstruction = frame.callIsPartOfConstruction || frame.isConstruction } }));
    }
    sites.numberOfSitesWithoutCode = sites.positions.size();

    size_t numberOfPositions = sites.positions.size() + 1;
    if (numberOfPositions > build().limit(LimitID::SitesPerFunction)) {
        failAtLimit(LimitID::SitesPerFunction, numberOfPositions, subject);
        return false;
    }
    return true;
}

void CodeGenerator::createBlocks()
{
    m_out.initialize(m_heaps);
    LBasicBlock root = m_out.newBlock();
    RELEASE_ASSERT(!root->index());
    m_blocks.fill(nullptr, function().blocks.size());
    for (BlockID block : analyses().cfg().reversePostOrder())
        m_blocks[block.index()] = m_out.newBlock();
    m_out.appendTo(root);
}

void CodeGenerator::createPhis()
{
    for (BlockID id : analyses().cfg().reversePostOrder()) {
        if (id == entryBlock)
            continue;
        LBasicBlock block = blockFor(id);
        for (ValueID parameter : function().at(id).parameters) {
            m_blockOfParameter[parameter.index()] = id;
            Representation representation = representationOf(parameter);
            if (!hasRegister(representation))
                continue;
            LValue& lowered = m_values[parameter.index()];
            RELEASE_ASSERT(!lowered);
            lowered = block->appendNew<B3::Value>(m_proc, B3::Phi, b3TypeOf(representation), b3OriginOf(parameter));
            publishKnownProperties(parameter, lowered);
        }
    }
}

// The step of a counter. A read at the old count may be folded into an instruction behind the step, and then both counts live at once and the back edge has a move.
bool CodeGenerator::isHandedOnByJumpAlone(BlockID id, const Block& block, ValueID value)
{
    Opcode opcode = function().at(value).opcode;
    if (block.terminator.kind != TerminatorKind::Jump || (opcode != Opcode::Add && opcode != Opcode::Sub))
        return false;
    std::optional<Use> use = analyses().useLists().singleUse(value);
    return use && use->kind == Use::Kind::EdgeArgument && use->block == id && analyses().hasNoEffects(value);
}

void CodeGenerator::generateBlock(BlockID id)
{
    m_out.appendTo(blockFor(id));
    setOrigin(m_out, { });
    forgetSharedForms();
    if (m_blocksGeneratedElsewhere.get(id.index())) {
        m_out.unreachable();
        return;
    }
    if (m_validates)
        emitter<RuntimeValidationEmitter>().validateParameters(id, function().at(id));
    generateContentsOf(id);
}

void CodeGenerator::generateInPlaceOfBlocks(std::span<const BlockID> blocks)
{
    for (BlockID block : blocks)
        m_blocksGeneratedElsewhere.set(block.index());
    auto begin = m_heaps.markDecorations();
    generateContentsOf(blocks[0]);
    // The terminator that is being generated declares none of the stores of these blocks.
    m_exemptStores.append({ begin, m_heaps.markDecorations() });
}

void CodeGenerator::generateContentsOf(BlockID id)
{
    const Block& block = function().at(id);
    std::span<const ValueID> values = block.values.span();
    if (ValueID owned = valueOwnedByTerminator(block.terminator)) {
        RELEASE_ASSERT(!values.empty() && values.back() == owned);
        values = values.first(values.size() - 1);
    }
    Vector<ValueID, 2> generatedLast;
    for (ValueID value : values) {
        if (isHandedOnByJumpAlone(id, block, value)) {
            generatedLast.append(value);
            continue;
        }
        // What is alive across a suspension is saved with the frame: the value, not what it is computed from.
        if (maySuspend(function().at(value).opcode)) {
            for (ValueID earlier : std::exchange(generatedLast, { })) {
                generateValue(id, block, earlier);
                if (m_hasFailed)
                    return;
            }
        }
        generateValue(id, block, value);
        if (m_hasFailed)
            return;
        if (m_valuesOutliveNoSuspension && maySuspend(function().at(value).opcode))
            forgetSharedForms();
        if (typeOf(value).isNever()) {
            if (currentBlockIsOpen()) {
                if (validatesOrChecksProofs())
                    emitter<RuntimeValidationEmitter>().reportUnreachable(value);
                m_out.unreachable();
            }
            return;
        }
    }
    for (ValueID value : generatedLast) {
        generateValue(id, block, value);
        if (m_hasFailed)
            return;
    }
    generateTerminator(id, block);
}

void CodeGenerator::generateValue(BlockID blockID, const Block& block, ValueID id)
{
    // The one user of a deferred closure is a store that writes nothing: the slot already names the function.
    if (isDeferredClosure(function(), id))
        return;
    // The one user of such a prototype fetches it where it walks the chain, behind its test for a cell.
    if (function().at(id).opcode == Opcode::Builtin && emitter<OperatorEmitter>().fetchesPrototypeItself(id))
        return;
    if (function().at(id).opcode == Opcode::Mod && emitter<OperatorEmitter>().leavesRemainderToTest(id))
        return;
    ValueContext context(blockID, block, id, function().at(id), analyses());
    generate(context);
}

void CodeGenerator::generate(const ValueContext& context)
{
    setOrigin(m_out, context.id());
    OperationScope scope(*this, context);
    convertOperandsOf(context, context.value());
    if (m_validates)
        emitter<RuntimeValidationEmitter>().validateOperands(context);
    else if (m_checksProofs)
        emitter<RuntimeValidationEmitter>().checkSlotAccessWithoutGuard(context);

    auto begin = m_heaps.markDecorations();
    size_t numberOfEnclosingExemptions = m_exemptStores.size();
    dispatch(context);
    checkInlineStores(context, begin);
    m_exemptStores.shrink(numberOfEnclosingExemptions);

    if (context.isBranchCondition() || context.returnCall()) {
        // The terminator that owns this value declares none of its stores.
        m_exemptStores.append({ begin, m_heaps.markDecorations() });
        return;
    }
    if (m_hasFailed || typeOf(context.id()).isNever())
        return;

    size_t index = context.id().index();
    RELEASE_ASSERT(context.representation() == Representation::Void || m_values[index] || m_resultRanges[index]);
    if (m_validates)
        emitter<RuntimeValidationEmitter>().validateResult(context);
}

void CodeGenerator::generateTerminator(BlockID blockID, const Block& block)
{
    setOrigin(m_out, { });
    TerminatorContext context(blockID, block, analyses());
    OperationScope scope(*this, context);
    convertOperandsOf(context, context.terminator());
    if (m_validates)
        emitter<RuntimeValidationEmitter>().validateExit(context);
    else if (m_checksProofs && context.kind() == TerminatorKind::Unreachable)
        emitter<RuntimeValidationEmitter>().reportUnreachable({ });

    auto begin = m_heaps.markDecorations();
    dispatch(context);
    checkInlineStores(context, begin);
    m_exemptStores.shrink(0);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

void CodeGenerator::generateBranchCondition(const TerminatorContext& terminator, ValueID condition, BranchTargets targets)
{
    ValueContext context(terminator.block(), function().at(terminator.block()), condition, function().at(condition), analyses());
    context.m_branchTargets = targets;
    generate(context);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

void CodeGenerator::generateReturnedCall(const TerminatorContext& terminator, ValueID call)
{
    ValueContext context(terminator.block(), function().at(terminator.block()), call, function().at(call), analyses());
    context.m_returnCall = &terminator;
    generate(context);
    RELEASE_ASSERT(m_hasFailed || !currentBlockIsOpen());
}

bool CodeGenerator::calleeTestsResultOf(const ValueContext& call) const
{
    if (m_checksKeptRegisters || m_droppedGuard)
        return false;
    return m_inputs.program.promisedResults.findCallThatStandsForTest(program(), function(), function().at(call.block())) == call.id();
}

void CodeGenerator::setLandingOfMisfit(ValueID call, const LandingOfMisfit& landing)
{
    RELEASE_ASSERT(!m_callWithLandingOfMisfit && call);
    m_callWithLandingOfMisfit = call;
    m_landingOfMisfit = landing;
}

std::optional<CodeGenerator::LandingOfMisfit> CodeGenerator::takeLandingOfMisfit(ValueID call)
{
    if (!call || m_callWithLandingOfMisfit != call)
        return std::nullopt;
    m_callWithLandingOfMisfit = { };
    return m_landingOfMisfit;
}

// A test for a type that holds neither undefined nor null fails for both, so the check right in front of it is made for the values that failed, and those that pass never meet it.
ValueID CodeGenerator::nullishCheckBehindFailedTest(BlockID id) const
{
    const Block& block = function().at(id);
    if (m_validates || m_droppedGuard || block.terminator.kind != TerminatorKind::TestAndBranch || block.values.size() < 2)
        return { };
    ValueID check = block.values[block.values.size() - 2];
    const Value& test = function().at(block.values.last());
    const Value& checked = function().at(check);
    if (test.opcode != Opcode::TypeTest || test.valueOperand() != check || checked.opcode != Opcode::RequireObjectCoercible)
        return { };
    ValueID unchecked = checked.valueOperand();
    if (representationOf(check) != Representation::JSValue || representationOf(unchecked) != Representation::JSValue)
        return { };
    if (!(typeOf(unchecked).primitives & ~(cellPrimitives | Primitive::UndefinedOrNull)) || program().at(test.testedType()).mayBeUndefinedOrNull())
        return { };
    return check;
}

void CodeGenerator::generateNullishCheckBehindFailedTest(BlockID id, ValueID check)
{
    ValueContext context(id, function().at(id), check, function().at(check), analyses());
    setOrigin(m_out, check);
    emitter<CastEmitter>().requireObjectCoercible(context);
}

void CodeGenerator::finishProcedure()
{
    m_heaps.computeRangesAndDecorateInstructions();
    if (build().recordedOptions().isSabotaged(Sabotage::FieldsNeverChange)) {
        B3::HeapRange fields = m_heaps.rangeOf(m_heaps.entire(AbstractHeap::ObjectFields));
        for (B3::Value* value : m_proc.values()) {
            if (auto* load = value->as<B3::MemoryValue>(); load && load->isLoad() && load->range() && fields.begin() <= load->range().begin() && load->range().end() <= fields.end())
                load->setReadsMutability(B3::Mutability::Immutable);
        }
    }
    m_proc.deleteOrphans();
    m_out.applyBlockOrder();
}

LBasicBlock CodeGenerator::blockFor(BlockID id) const
{
    LBasicBlock block = m_blocks[id.index()];
    RELEASE_ASSERT(block);
    return block;
}

B3::FrequentedBlock CodeGenerator::edgeTo(const TerminatorContext& terminator, uint32_t targetIndex, std::span<const Operand> declaredResults)
{
    const Target& target = terminator->targets[targetIndex];
    Edge edge { terminator.block(), targetIndex };
    const BlockFrequencies& frequencies = analyses().blockFrequencies();
    // Code that runs a few times pays for a side that lies out of the way with a line more each time the side runs.
    B3::FrequencyClass frequency = b3FrequencyClassOf(terminator.shape() == CodeShape::Fast ? frequencies.classForBlockOrder(edge) : frequencies.classOf(edge));
    if (build().recordedOptions().has(TestingSwitch::IgnoreBranchHints))
        frequency = B3::FrequencyClass::Normal;

    LBasicBlock block = blockFor(target.block);
    if (!hasParameterWithRegister(function(), target.block))
        return { block, frequency };
    if (terminator->targets.size() == 1) {
        passArguments(edge, target, declaredResults);
        return { block, frequency };
    }

    LBasicBlock here = m_out.m_block;
    LBasicBlock edgeBlock = m_out.newBlock();
    m_out.appendTo(edgeBlock);
    passArguments(edge, target, declaredResults);
    m_out.jump(block);
    m_out.appendTo(here);
    return { edgeBlock, frequency };
}

void CodeGenerator::jumpTo(const TerminatorContext& terminator, uint32_t targetIndex, std::span<const Operand> declaredResults)
{
    const Target& target = terminator->targets[targetIndex];
    passArguments(Edge { terminator.block(), targetIndex }, target, declaredResults);
    m_out.jump(blockFor(target.block));
}

void CodeGenerator::passArguments(Edge edge, const Target& target, std::span<const Operand> declaredResults)
{
    // What is converted for an edge lies on the edge.
    size_t numberOfSharedForms = m_sharedForms.size();
    const Block& from = function().at(edge.from);
    bool liesBetweenBlocks = from.terminator.kind != TerminatorKind::Jump;
    std::optional<ConversionEmitter::PlaceOnEdge> outerPlace;
    if (liesBetweenBlocks) {
        const Block& to = function().at(target.block);
        outerPlace = emitter<ConversionEmitter>().setPlaceOnEdge(ConversionEmitter::PlaceOnEdge { rarerOf(from.frequency, to.frequency), std::min(from.shape, to.shape) });
    }
    auto& parameters = function().at(target.block).parameters;
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        if (hasRegister(representationOf(parameters[index])))
            passArgument(parameters[index], takeIncomingOperand(edge, index, declaredResults));
    }
    RELEASE_ASSERT(declaredResults.empty());
    dropSharedFormsSince(numberOfSharedForms);
    if (liesBetweenBlocks)
        emitter<ConversionEmitter>().setPlaceOnEdge(outerPlace);
}

void CodeGenerator::passHandlerArguments(const Target& handler)
{
    // A landing pad is entered from every block that hands the handler these arguments: of what was made in the block that is being generated nothing is known to be there.
    size_t numberOfSharedForms = m_sharedForms.size();
    size_t firstSharedFormInReach = std::exchange(m_firstSharedFormInReach, numberOfSharedForms);
    std::optional<ConversionEmitter::PlaceOnEdge> outerPlace = emitter<ConversionEmitter>().setPlaceOnEdge(ConversionEmitter::PlaceOnEdge { BlockFrequency::Rare, CodeShape::Compact });
    auto& parameters = function().at(handler.block).parameters;
    RELEASE_ASSERT(handler.arguments.size() == parameters.size());
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (hasRegister(representationOf(parameters[index])))
            passArgument(parameters[index], { getAsDefined(handler.arguments[index]), handler.arguments[index] });
    }
    dropSharedFormsSince(numberOfSharedForms);
    m_firstSharedFormInReach = firstSharedFormInReach;
    emitter<ConversionEmitter>().setPlaceOnEdge(outerPlace);
}

void CodeGenerator::passArgument(ValueID parameter, const IncomingOperand& incoming)
{
    Operand argument = incoming.operand;
    Representation wanted = function().at(parameter).representation;
    if (!isSameWord(argument.representation, wanted)) {
        RELEASE_ASSERT(m_operation);
        if (!incoming.result)
            argument = convert(*m_operation, incoming.source, argument, typeOf(incoming.source), wanted);
        else {
            // An edge that would pass a value of type never is not taken. An earlier value in the wanted form would have to stay alive for this edge alone.
            Type type = computeTypeOnEdge({ program(), function(), m_inputs.types, TypeLevel::Proven }, *incoming.result);
            argument = type.isNever() ? emitter<ConstantEmitter>().zeroOf(wanted) : convert(*m_operation, incoming.source, argument, type, wanted, EarlierValue::IsNotLookedFor);
        }
    }
    LValue phi = m_values[parameter.index()];
    RELEASE_ASSERT(phi && phi->opcode() == B3::Phi);
    m_out.m_block->appendNew<B3::UpsilonValue>(m_proc, m_out.origin(), argument.value, phi);
}

Representation CodeGenerator::representationOf(ValueID id) const
{
    for (const ConvertedOperand& converted : m_convertedOperands) {
        if (converted.id == id)
            return converted.operand.representation;
    }
    return function().at(id).representation;
}

Operand CodeGenerator::get(ValueID id) const
{
    for (const ConvertedOperand& converted : m_convertedOperands) {
        if (converted.id == id)
            return converted.operand;
    }
    return getAsDefined(id);
}

Operand CodeGenerator::getAsDefined(ValueID id) const
{
    Representation representation = function().at(id).representation;
    if (representation == Representation::Void)
        return { };
    LValue lowered = m_values[id.index()];
    RELEASE_ASSERT(lowered);
    return { representation, lowered };
}

Operand CodeGenerator::getIn(ValueID id, Representation wanted)
{
    Operand operand = get(id);
    if (operand.representation == wanted) [[likely]]
        return operand;
    // The code may lie in an arm of the operation, so nobody else finds the result.
    RELEASE_ASSERT(m_operation);
    size_t numberOfSharedForms = m_sharedForms.size();
    Operand converted = convert(*m_operation, id, getAsDefined(id), typeOf(id), wanted);
    dropSharedFormsSince(numberOfSharedForms);
    return converted;
}

template<typename Operation>
void CodeGenerator::convertOperandsOf(const OperationContext& context, const Operation& operation)
{
    RepresentationInputs inputs { program(), m_inputs.id, function(), m_inputs.program.placement, m_inputs.program.conventions };
    uint32_t numberOfOperands = operation.numberOfOperands();
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        ValueID operand = operation.operand(operandIndex);
        if (!operand)
            continue;
        Representation wanted = wantedRepresentationOf(inputs, operation, operandIndex);
        Representation defined = function().at(operand).representation;
        // An operation that takes a double with any NaN computes with it as with any number.
        bool isTakenAsNumber = holdsAnyNaN(wanted);
        if (!hasRegister(wanted) || (defined == wanted && !isTakenAsNumber))
            continue;
        // The operation makes such an operand itself.
        if (hasRegister(defined) && !m_values[operand.index()])
            continue;
        Representation taken = isTakenAsNumber ? Representation::Double : wanted;
        bool isConverted = m_convertedOperands.containsIf([&](const ConvertedOperand& converted) {
            RELEASE_ASSERT(converted.id != operand || converted.operand.representation == taken);
            return converted.id == operand;
        });
        if (isConverted)
            continue;
        Operand converted = convert(context, operand, getAsDefined(operand), typeOf(operand), wanted);
        m_convertedOperands.append({ operand, Operand(taken, converted.value) });
    }
}

void CodeGenerator::takeOperandsOf(ValueID operationLeftToUser)
{
    convertOperandsOf(*m_operation, function().at(operationLeftToUser));
}

std::optional<Operand> CodeGenerator::onlyValueOf(const Type& type, Representation representation)
{
    if (!isLegalRepresentation(type, representation))
        return std::nullopt;
    if (type.isInteger() && type.integers.isSingleValue()) {
        if (representation == Representation::Int64)
            return Operand(Integer(m_out.constInt64(type.integers.minimum)));
        return emitter<ConstantEmitter>().constantOf(jsNumber(static_cast<double>(type.integers.minimum)), representation);
    }
    if (type.isBoolean() && type.isSingleValue())
        return emitter<ConstantEmitter>().constantOf(jsBoolean(!!(type.primitives & Primitive::True)), representation);
    return std::nullopt;
}

// A constant costs nothing in any form, and a conversion of one may cost a call.
Operand CodeGenerator::makeConstantIn(const OperationContext& user, ValueID constant, Representation representation)
{
    Value inForm = function().at(constant);
    inForm.representation = representation;
    ValueContext context(user.block(), function().at(user.block()), constant, inForm, analyses());
    Operand made;
    Operand* outer = std::exchange(m_definitionTaken, &made);
    dispatch(context);
    m_definitionTaken = outer;
    RELEASE_ASSERT(m_hasFailed || made.representation == representation);
    return made;
}

ValueID CodeGenerator::onlyValueArrivingAt(ValueID parameter)
{
    if (function().at(parameter).opcode != Opcode::Parameter)
        return { };
    BlockID block = m_blockOfParameter[parameter.index()];
    if (!block || block == entryBlock)
        return { };
    std::span<const Edge> incomingEdges = analyses().cfg().incomingEdges(block);
    if (incomingEdges.size() != 1 || incomingEdges[0].isHandlerEdge())
        return { };

    size_t index = function().at(block).parameters.find(parameter);
    RELEASE_ASSERT(index != notFound);
    IncomingValue incoming = incomingValueOf(function(), incomingEdges[0], index);
    if (incoming.kind == IncomingValue::Kind::Argument)
        return incoming.argument;
    EdgeResult result = edgeResultAt(function(), incoming.result);
    return result.kind == EdgeResultKind::NarrowedOperand ? result.subject : ValueID();
}

ValueID CodeGenerator::findSameValueMadeBefore(ValueID source, Representation wanted)
{
    if (m_valuesOutliveNoSuspension)
        return { };
    ValueID value = source;
    while (true) {
        if (isCastOrConversion(function().at(value).opcode))
            value = function().at(value).operand(0);
        else if (wanted == Representation::Cell32)
            return { };
        else
            value = onlyValueArrivingAt(value);
        if (!value)
            return { };
        if (function().at(value).representation == wanted)
            break;
    }
    if (!m_values[value.index()] || m_inputs.types.intersectionOf(function().at(value).type, function().at(source).type) != function().at(source).type)
        return { };
    return value;
}

std::optional<Operand> CodeGenerator::findSharedForm(ValueID source, Representation representation) const
{
    for (uint32_t next = m_newestSharedFormOf[source.index()]; next > m_firstSharedFormInReach; next = m_sharedForms[next - 1].previousOfSource) {
        const SharedForm& form = m_sharedForms[next - 1];
        if (form.operand.representation == representation)
            return form.operand;
    }
    return std::nullopt;
}

void CodeGenerator::shareForm(ValueID source, Operand operand)
{
    m_sharedForms.append({ source, operand, m_newestSharedFormOf[source.index()] });
    m_newestSharedFormOf[source.index()] = m_sharedForms.size();
}

void CodeGenerator::dropSharedFormsSince(size_t mark)
{
    while (m_sharedForms.size() > mark) {
        SharedForm form = m_sharedForms.takeLast();
        m_newestSharedFormOf[form.source.index()] = form.previousOfSource;
    }
    m_firstSharedFormInReach = std::min(m_firstSharedFormInReach, m_sharedForms.size());
}

Operand CodeGenerator::convert(const OperationContext& context, ValueID source, Operand operand, const Type& type, Representation wanted, EarlierValue earlierValue)
{
    if (operand.representation == wanted)
        return operand;
    bool hasTypeOfSource = source && &type == &typeOf(source);
    auto share = [&](Operand form) {
        if (source)
            shareForm(source, form);
        if (hasTypeOfSource && form.value)
            publishKnownPropertiesOfConvertedValue(source, form);
        return form;
    };
    if (source) {
        if (std::optional<Operand> shared = findSharedForm(source, wanted))
            return *shared;
        bool isConstant = constantsInFormOfUses.contains(function().at(source).opcode) && isLegalRepresentation(type, wanted) && !(wanted == Representation::Cell32 && type.hasCells()) && !isSameWord(operand.representation, wanted);
        if (isConstant)
            return share(makeConstantIn(context, source, wanted));
        if (ValueID sameValue = earlierValue == EarlierValue::MayStandIn ? findSameValueMadeBefore(source, wanted) : ValueID())
            return share(getAsDefined(sameValue));
    }
    if (std::optional<Operand> constant = onlyValueOf(type, wanted))
        return share(*constant);

    std::optional<ConversionPath> path = findConversion(type, operand.representation, wanted);
    if (!path) {
        fail(context, InternalErrorKind::UnsupportedConversion);
        return emitter<ConstantEmitter>().zeroOf(wanted);
    }
    ValueID sourceOfStep = source;
    for (ConversionStep step : *path) {
        std::optional<Operand> shared = source ? findSharedForm(source, step.to) : std::nullopt;
        // The back end asks the origin of a decoded reference which markers it may hold.
        bool decodesNarrowReference = !shared && sourceOfStep && operand.representation == Representation::Cell32;
        auto* originOfUser = m_out.origin().dfgOrigin();
        if (decodesNarrowReference)
            setOrigin(m_out, sourceOfStep);
        operand = shared ? *shared : share(emitter<RepresentationEmitter>().convert(context, step.opcode, sourceOfStep, operand, type, step.to));
        if (decodesNarrowReference)
            m_out.setOrigin(originOfUser);
        if (step.to == Representation::Cell32 && context.valueID())
            m_records.operationsWithCompressedOperand.set(context.valueID().index());
        sourceOfStep = { };
    }
    return operand;
}

void CodeGenerator::defineValue(ValueID id, Operand operand)
{
    if (m_definitionTaken) [[unlikely]] {
        *m_definitionTaken = operand;
        return;
    }
    if (operand.representation == Representation::Double && holdsAnyNaN(function().at(id).representation))
        operand.representation = function().at(id).representation;
    RELEASE_ASSERT(operand.representation == function().at(id).representation);
    if (operand.representation == Representation::Void)
        return;
    LValue& lowered = m_values[id.index()];
    RELEASE_ASSERT(!lowered && operand.value && operand.value->type() == b3TypeOf(operand.representation));
    lowered = operand.value;
    publishKnownProperties(id, lowered);
}

void CodeGenerator::define(const ValueContext& value, Operand result)
{
    RELEASE_ASSERT(!value.isBranchCondition());
    defineValue(value.id(), result);
}

void CodeGenerator::defineTest(const ValueContext& value, Condition result)
{
    std::optional<GuardUnderTest> guard = takeGuardUnderTest(value);
    if (!value.isBranchCondition()) {
        if (guard) {
            LBasicBlock failed = m_out.newBlock();
            LBasicBlock continuation = m_out.newBlock();
            m_out.branch(result.value(), { normalEdge(continuation), rareEdge(failed) });
            m_out.appendTo(failed);
            emitter<RuntimeValidationEmitter>().noteFailedGuard(value.opcode(), guard->ordinal);
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            if (guard->isDropped)
                result = Condition(m_out.constInt32(1));
        }
        define(value, result);
        return;
    }
    LValue& lowered = m_values[value.id().index()];
    RELEASE_ASSERT(!lowered);
    lowered = result.value();
    m_out.branch(result.value(), guard ? targetsOf(value, *guard) : value.branchTargets());
}

void CodeGenerator::defineResults(const ValueContext& value, std::span<const Operand> results)
{
    std::optional<ResultRange>& range = m_resultRanges[value.id().index()];
    RELEASE_ASSERT(!range);
    range = ResultRange { static_cast<uint32_t>(m_results.size()), static_cast<uint32_t>(results.size()) };
    m_results.append(results);
}

Operand CodeGenerator::resultAt(ValueID producer, uint32_t index) const
{
    const std::optional<ResultRange>& range = m_resultRanges[producer.index()];
    RELEASE_ASSERT(range && index < range->count);
    return m_results[range->first + index];
}

void CodeGenerator::defineParameter(ValueID parameter, Operand operand)
{
    RELEASE_ASSERT(function().at(parameter).opcode == Opcode::Parameter);
    defineValue(parameter, operand);
}

Subject CodeGenerator::subjectOf(const OperationContext& context) const
{
    return { .function = m_inputs.id, .block = context.block(), .value = context.valueID(), .location = function().locationOf(context.origin()) };
}

void CodeGenerator::fail(const OperationContext& context, InternalErrorKind kind)
{
    if (m_hasFailed)
        return;
    m_hasFailed = true;
    Subject subject = subjectOf(context);
    build().diagnostics().add(InternalError { .kind = kind, .function = subject.function, .block = subject.block, .value = subject.value, .location = subject.location });
}

void CodeGenerator::failAtLimit(const OperationContext& context, LimitID limit, uint64_t needed)
{
    failAtLimit(limit, needed, subjectOf(context));
}

void CodeGenerator::failAtLimit(LimitID limit, uint64_t needed, const Subject& subject)
{
    if (m_hasFailed)
        return;
    m_hasFailed = true;
    build().reportLimitExceeded(limit, needed, subject);
}

std::optional<DroppedGuard> droppedGuardIn(const RecordedOptions& options, FunctionID id)
{
    const RecordedSwitches& switches = options.switches;
    if (id.index() < switches.firstID || (switches.idCount && id.index() - switches.firstID >= switches.idCount))
        return std::nullopt;
    return options.droppedGuard();
}

std::optional<CodeGenerator::GuardUnderTest> CodeGenerator::takeGuardUnderTest(const ValueContext& value)
{
    bool isGuard = value.opcode() == Opcode::TypeTest || familyOf(value.opcode()) == OperationFamily::Guard;
    if (!m_droppedGuard || !isGuard)
        return std::nullopt;
    if (m_numberOfGuardsSeen.isEmpty())
        m_numberOfGuardsSeen.fill(0, numberOfEnumerators<Opcode>);
    uint32_t ordinal = m_numberOfGuardsSeen[static_cast<size_t>(value.opcode())]++;
    return GuardUnderTest { .ordinal = ordinal, .isDropped = m_droppedGuard->opcode == value.opcode() && m_droppedGuard->ordinal == ordinal };
}

BranchTargets CodeGenerator::targetsOf(const ValueContext& value, const GuardUnderTest& guard)
{
    const BranchTargets& targets = value.branchTargets();
    LBasicBlock here = m_out.m_block;
    LBasicBlock failed = m_out.newBlock();
    m_out.appendTo(failed);
    emitter<RuntimeValidationEmitter>().noteFailedGuard(value.opcode(), guard.ordinal);
    m_out.jump((guard.isDropped ? targets.taken : targets.notTaken).block());
    m_out.appendTo(here);
    return { targets.taken, B3::FrequentedBlock(failed, targets.notTaken.frequency()) };
}

bool CodeGenerator::currentBlockIsOpen() const
{
    LBasicBlock block = m_out.m_block;
    return !block->size() || !block->last()->effects().terminal;
}

ValueID valueOwnedByTerminator(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::TestAndBranch:
        return terminator.conditionOperand();
    case TerminatorKind::ReturnCall:
        return terminator.callOperand();
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Return:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CodeGenerator::InitializationScope::InitializationScope(CodeGenerator& generator)
    : m_generator(generator)
    , m_begin(generator.m_heaps.markDecorations())
{
    ++generator.m_initializationDepth;
}

CodeGenerator::InitializationScope::~InitializationScope()
{
    --m_generator.m_initializationDepth;
    m_generator.m_exemptStores.append({ m_begin, m_generator.m_heaps.markDecorations() });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
