/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AirCode.h"
#include "AirGenerate.h"
#include "B3BasicBlockInlines.h"
#include "B3Generate.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include "JSCInlines.h"
#include "LinkBuffer.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

Lowering::Lowering(Procedure& proc, const Program& program, FunctionID id, LoweringContext& context)
    : Emitter(proc)
    , m_program(program)
    , m_function(program.at(id))
    , m_id(id)
    , m_isEntry(id == program.entry)
    , m_context(context)
    , m_builtins(Builtins::singleton())
    , m_length(program.find("length"_s.span8()).value_or(StringID()))
{
}

LType Lowering::b3TypeOf(Rep rep)
{
    switch (rep) {
    case Rep::Void:
        return Void;
    case Rep::Int32:
        return Int32;
    case Rep::Int64:
    case Rep::OptionalInt64:
    case Rep::Cell:
    case Rep::JSValue:
        return Int64;
    case Rep::Double:
    case Rep::OptionalDouble:
        return Double;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LBasicBlock Lowering::newColdBlock()
{
    LBasicBlock block = m_out.newBlock();
    block->setFrequency(0);
    return block;
}

Integer Lowering::integer(int64_t value)
{
    m_context.wideIntegerConstants.add(value);
    return Integer(m_out.constInt64(value));
}

// Where control goes from there. A condition whose type leaves one outcome is not tested.
std::span<const Target> Lowering::successorsOf(const Terminator& terminator) const
{
    std::span<const Target> targets = terminator.targets.span();
    if (terminator.kind != TerminatorKind::Branch)
        return targets;
    uint16_t primitives = typeOf(terminator.value).primitives;
    bool mayBeTrue = primitives & Primitive::True;
    bool mayBeFalse = primitives & Primitive::False;
    return targets.subspan(mayBeTrue ? 0 : 1, mayBeTrue + mayBeFalse);
}

Vector<BlockID> Lowering::reversePostorder() const
{
    Vector<BlockID> order;
    BitVector seen(m_function.blocks.size());
    Vector<std::pair<BlockID, unsigned>> stack;
    seen.set(0);
    stack.append({ BlockID(0u), 0 });
    while (!stack.isEmpty()) {
        auto& [block, next] = stack.last();
        auto targets = successorsOf(m_function.at(block).terminator);
        // From the last target to the first, so that the first comes right after the block.
        if (next < targets.size()) {
            BlockID successor = targets[targets.size() - 1 - next++].block;
            if (!seen.set(successor.index()))
                stack.append({ successor, 0 });
            continue;
        }
        order.append(block);
        stack.removeLast();
    }
    order.reverse();
    return order;
}

LBasicBlock Lowering::edgeTo(const Target& target, bool isBranch)
{
    auto& parameters = m_function.at(target.block).parameters;
    Vector<LValue, 4> arguments;
    for (unsigned i = 0; i < target.arguments.size(); ++i)
        arguments.append(in(target.arguments[i], m_function.at(parameters[i]).rep).value);
    auto& phis = m_phis[target.block.index()];
    LBasicBlock block = m_blocks[target.block.index()];
    auto anchorAll = [&] {
        for (unsigned i = 0; i < arguments.size(); ++i) {
            // The back end takes a phi for what comes in where that is one value, so it is a cell no earlier than that value is.
            if (!m_typeOfCell.contains(arguments[i]))
                m_typeOfCell.remove(phis[i]);
            m_out.addIncomingToPhi(phis[i], m_out.anchor(arguments[i]));
        }
    };
    if (!isBranch || arguments.isEmpty()) {
        anchorAll();
        return block;
    }
    LBasicBlock here = m_out.m_block;
    LBasicBlock edge = m_out.newBlock();
    edge->setFrequency(block->frequency());
    m_out.appendTo(edge);
    anchorAll();
    m_out.jump(block);
    m_out.appendTo(here);
    return edge;
}

void Lowering::lowerTerminator(const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Unreachable:
        m_out.unreachable();
        return;
    case TerminatorKind::Jump:
        m_out.jump(edgeTo(terminator.targets[0], false));
        return;
    case TerminatorKind::Branch: {
        auto successors = successorsOf(terminator);
        if (successors.empty()) {
            m_out.unreachable();
            return;
        }
        if (successors.size() == 1) {
            m_out.jump(edgeTo(successors[0], false));
            return;
        }
        Word32 condition = get(terminator.value).as<Word32>();
        LBasicBlock taken = edgeTo(terminator.targets[0], true);
        LBasicBlock notTaken = edgeTo(terminator.targets[1], true);
        m_out.branch(condition.value(), unsure(taken), unsure(notTaken));
        return;
    }
    case TerminatorKind::Return: {
        if (m_isEntry) {
            m_out.ret(undefined().value());
            return;
        }
        Rep rep = resultRepOf(m_program, m_id);
        if (rep == Rep::Void) {
            m_out.m_block->clearSuccessors();
            m_out.m_block->appendNew<B3::Value>(m_proc, B3::Return, m_out.origin());
            return;
        }
        if (terminator.value)
            m_out.ret(in(terminator.value, rep).value);
        else
            m_out.ret(convert(undefined(), Type::ofPrimitives(Primitive::Undefined), rep).value);
        return;
    }
    case TerminatorKind::Throw: {
        Boxed thrown = in<Boxed>(terminator.value);
        if (!terminator.targets.isEmpty())
            spillForCatch(terminator.targets[0]);
        AOT_TYPED_OPERATION(Throw, thrown);
        m_currentCatchEntrypoint = std::nullopt;
        m_out.unreachable();
        return;
    }
    case TerminatorKind::Invoke:
        // The other edge is taken by whoever unwinds the stack.
        m_out.jump(edgeTo(terminator.targets[0], false));
        return;
    case TerminatorKind::Switch: {
        Integer value = in<Integer>(terminator.value);
        Vector<FTL::SwitchCase> cases;
        for (unsigned i = 0; i < terminator.cases.size(); ++i)
            cases.append(FTL::SwitchCase(m_out.constInt64(terminator.cases[i]), edgeTo(terminator.targets[i], true), FTL::Weight()));
        m_out.switchInstruction(value.value(), cases, edgeTo(terminator.targets.last(), true), FTL::Weight());
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The blocks from which an edge that goes back can be reached without passing the block it goes back to.
void Lowering::computeLoopBlocks(const Vector<BlockID>& order)
{
    unsigned numberOfBlocks = m_function.blocks.size();
    Vector<unsigned> place(numberOfBlocks);
    for (unsigned i = 0; i < order.size(); ++i)
        place[order[i].index()] = i;
    Vector<Vector<BlockID, 2>> predecessors(numberOfBlocks);
    for (BlockID block : order) {
        for (auto& target : successorsOf(m_function.at(block).terminator))
            predecessors[target.block.index()].append(block);
    }
    for (BlockID block : order) {
        for (auto& target : successorsOf(m_function.at(block).terminator)) {
            if (place[target.block.index()] > place[block.index()])
                continue;
            BitVector seen(numberOfBlocks);
            seen.set(target.block.index());
            Vector<BlockID, 16> worklist;
            if (!seen.set(block.index()))
                worklist.append(block);
            while (!worklist.isEmpty()) {
                for (BlockID predecessor : predecessors[worklist.takeLast().index()]) {
                    if (!seen.set(predecessor.index()))
                        worklist.append(predecessor);
                }
            }
            m_loopBlocks.merge(seen);
        }
    }
}

void Lowering::lowerBlock(BlockID id)
{
    m_out.appendTo(m_blocks[id.index()]);
    m_isInLoop = m_loopBlocks.get(id.index());
    auto& block = m_function.at(id);
    if (!m_catchSpills.isEmpty()) {
        for (ValueID parameter : block.parameters)
            define(parameter, m_values[parameter.index()]);
    }
    m_currentBlock = &block;
    for (unsigned i = 0; i < block.values.size(); ++i) {
        ValueID valueID = block.values[i];
        auto& value = m_function.at(valueID);
        m_currentIndexInBlock = i;
        if (block.terminator.kind == TerminatorKind::Invoke && i + 1 == block.values.size())
            spillForCatch(block.terminator.targets[1]);
        Operand result = lower(valueID, value);
        m_currentCatchEntrypoint = std::nullopt;
        if (!m_whyFailed.isNull())
            return;
        if (result.rep != value.rep) [[unlikely]] {
            dataLogLn("AOT: f", m_id.index(), ": v", valueID.index(), " = ", nameOf(value.opcode), " is kept as ", nameOf(value.rep), " and was lowered as ", nameOf(result.rep));
            CRASH();
        }
        define(valueID, result);
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            validateFact(result, valueID, typeOf(valueID));
        // What a test has shown to be a cell, an operand a cast hands on, and what a variable is read back as were defined before that: a load from it may trap there.
        bool isDefinedHere = result.rep == Rep::Cell && result.value->opcode() != B3::Get && !m_takenForCells.contains(result.value);
        value.forEachChild([&](ValueID child) {
            isDefinedHere &= m_values[child.index()].value != result.value;
        });
        if (isDefinedHere)
            m_typeOfCell.add(result.value, value.type);
    }
    lowerTerminator(block.terminator);
}

bool Lowering::run()
{
    if (m_function.blocks.isEmpty()) {
        fail("a function without a body"_s);
        return false;
    }
    unsigned numberOfBlocks = m_function.blocks.size();
    m_values.grow(m_function.values.size());
    m_blocks.fill(nullptr, numberOfBlocks);
    m_phis.grow(numberOfBlocks);
    m_out.initialize(m_heaps);
    m_out.setFrequency(1);
    LBasicBlock prologue = m_out.newBlock();
    Vector<BlockID> order = reversePostorder();
    computeLoopBlocks(order);
    computeCatchEntrypoints(order);
    m_context.numberOfCatchEntrypoints = m_catchEntrypoints.size();
    m_proc.setNumEntrypoints(1 + m_catchEntrypoints.size());
    for (BlockID block : order) {
        m_blocks[block.index()] = m_out.newBlock();
        if (m_function.at(block).terminator.kind == TerminatorKind::Unreachable || m_function.at(block).terminator.kind == TerminatorKind::Throw)
            m_blocks[block.index()]->setFrequency(0);
    }
    for (auto& handler : m_catchEntrypoints)
        handler.entry = newColdBlock();
    m_out.appendTo(prologue);
    m_out.initializeConstants(m_proc, prologue);
    findPinnedRegisters();
    if (m_catchSpillSlotCount)
        m_catchSpillArea = m_out.lockedStackSlot(m_catchSpillSlotCount * sizeof(Register));
    m_data = m_proc.addVariable(Int64);
    loadICData();
    if (!lowerEntryParameters())
        return false;
    for (BlockID block : order) {
        if (!block.index())
            continue;
        m_out.appendTo(m_blocks[block.index()]);
        for (ValueID id : m_function.at(block).parameters) {
            Rep rep = m_function.at(id).rep;
            LValue phi = m_out.phi(b3TypeOf(rep));
            m_phis[block.index()].append(phi);
            m_values[id.index()] = Operand(rep, phi);
            if (rep == Rep::Cell)
                m_typeOfCell.add(phi, m_function.at(id).type);
        }
    }
    // What comes before the switch is run on every entry, also on that of a handler.
    LBasicBlock start = m_isEntry ? m_out.newBlock() : m_blocks[0];
    m_out.appendTo(prologue);
    if (m_catchEntrypoints.isEmpty())
        m_out.jump(start);
    else {
        Vector<LBasicBlock> entries { start };
        for (auto& handler : m_catchEntrypoints)
            entries.append(handler.entry);
        m_out.entrySwitch(entries);
    }
    if (m_isEntry) {
        m_out.appendTo(start);
        initializeModuleVariables();
        m_out.jump(m_blocks[0]);
    }
    for (BlockID block : order) {
        lowerBlock(block);
        if (!m_whyFailed.isNull())
            return false;
    }
    for (auto& handler : m_catchEntrypoints)
        lowerCatchEntrypoint(handler);
    m_heaps.computeRangesAndDecorateInstructions();
    m_proc.deleteOrphans();
    m_out.applyBlockOrder();
    return true;
}

// Whoever calls by name passes the environment, `this` and the arguments as the function declares them. The body may take them for more.
bool Lowering::lowerEntryParameters()
{
    auto& parameters = m_function.blocks[0].parameters;
    unsigned nextGPR = 0;
    if (m_function.has(FunctionUsesArgumentBuffer)) {
        m_argumentCount = Word32(m_out.castToInt32(registerOnEntry(argumentGPR(0))));
        m_argumentBuffer = Pointer(registerOnEntry(argumentGPR(1)));
    }
    if (m_function.isGenericEntrypoint()) {
        // The function value, `this` and every argument, each in its own register. All but the first may be anything.
        for (unsigned i = 0; i < parameters.size(); ++i) {
            Rep rep = m_function.at(parameters[i]).rep;
            RELEASE_ASSERT(i ? rep == Rep::JSValue : holdsCells(rep));
            GPRReg reg = i ? thisGPR : calleeGPR;
            if (i >= 2) {
                ++m_genericEntrypointParameterCount;
                if (nextGPR >= numberOfArgumentGPRs) {
                    fail("more parameters than there are registers"_s);
                    return false;
                }
                reg = argumentGPR(nextGPR++);
            }
            LValue value = registerOnEntry(reg);
            m_values[parameters[i].index()] = Operand(rep, value);
            if (rep == Rep::Cell)
                m_typeOfCell.add(value, m_function.at(parameters[i]).type);
        }
        return true;
    }

    unsigned nextFPR = 0;
    unsigned onStack = 0;
    for (unsigned i = 0; i < parameters.size(); ++i) {
        auto& parameter = m_function.at(parameters[i]);
        auto& type = typePassedAs(m_function, i);
        Rep rep = repOf(type);
        bool isDouble = b3TypeOf(rep) == Double;
        LValue value;
        if (isDouble && nextFPR < FPRInfo::numberOfArgumentRegisters)
            value = registerOnEntry(FPRInfo::toArgumentRegister(nextFPR++));
        else if (!isDouble && nextGPR < numberOfArgumentGPRs) {
            value = registerOnEntry(argumentGPR(nextGPR++));
            if (rep == Rep::Int32)
                value = m_out.castToInt32(value);
        } else {
            m_context.readsCallerFrame = true;
            TypedPointer pointer = m_out.address(m_heaps.root, m_out.framePointer(), sizeof(CallerFrameAndPC) + onStack++ * sizeof(Register));
            value = isDouble ? m_out.loadDouble(pointer) : rep == Rep::Int32 ? m_out.load32(pointer) : m_out.load64(pointer);
        }
        m_values[parameters[i].index()] = convert(Operand(rep, value), type, parameter.rep);
        if (rep == Rep::Cell && parameter.rep == Rep::Cell)
            m_typeOfCell.add(value, parameter.type);
    }
    return true;
}

TypedPointer Lowering::addressOf(VariableID id)
{
    return m_out.address(m_heaps.AOTTyped_variables[id.index()], fixedPointer(Instance::offsetOfTypedStatics()), m_program.variables[id.index()].offset);
}

// Before any code of the program runs. The memory is all zero bits, which in a boxed value stands for nothing.
void Lowering::initializeModuleVariables()
{
    struct Word {
        uint32_t offset;
        int64_t bits;
    };
    Vector<Word> words;
    for (auto& variable : m_program.variables) {
        // Only the collector looks at it before the first store, and to that no bits are nothing.
        if (variable.isNeverSeenUninitialized)
            continue;
        Rep rep = repOf(variable.store);
        bool hasMarkers = rep == Rep::JSValue || rep == Rep::OptionalInt64 || rep == Rep::OptionalDouble;
        RELEASE_ASSERT(hasMarkers || variable.startsUndefined);
        if (!hasMarkers)
            continue;
        if (int64_t bits = bitsOfMarker(variable.startsUndefined ? Primitive::Undefined : Primitive::Absent, rep))
            words.append({ variable.offset, bits });
    }
    std::ranges::sort(words, { }, &Word::offset);
    LValue statics = fixedPointer(Instance::offsetOfTypedStatics());
    for (unsigned first = 0; first < words.size();) {
        unsigned end = first + 1;
        while (end < words.size() && words[end].bits == words[first].bits && words[end].offset == words[end - 1].offset + sizeof(uint64_t))
            ++end;
        uint32_t firstWord = words[first].offset / sizeof(uint64_t);
        splatWords(statics, m_out.constInt32(firstWord), m_out.constInt32(firstWord + end - first), integer(words[first].bits).value(), m_heaps.AOTTyped_variables.atAnyNumber());
        first = end;
    }
}

// What was read from a variable that may hold nothing yet.
void Lowering::checkInitialized(Operand raw)
{
    require(logicalNot(isAbsent(raw)), [&] {
        AOT_TYPED_OPERATION(ThrowTDZError, word32(0));
    });
}

// The module variables and the variables in environments that this function stores to. One that is stored to once, and is never seen
// uninitialized, never changes while any other function runs: none that reads it is entered before the store.
void Lowering::findStoresToVariables()
{
    if (std::exchange(m_didFindStoresToVariables, true))
        return;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            if (value.opcode == Opcode::PutModuleVariable)
                m_variablesStoredTo.set(value.variable().index());
            else if (value.opcode == Opcode::PutField)
                m_fieldsStoredTo.add(static_cast<uint64_t>(value.layout().index()) << 32 | value.field());
        }
    }
}

Operand Lowering::lowerGetModuleVariable(ValueID result, const Value& value)
{
    auto& variable = m_program.variables[value.variable().index()];
    unsigned flags = IsSafe;
    if (variable.isStoredOnce) {
        findStoresToVariables();
        if (!m_variablesStoredTo.get(value.variable().index()))
            flags |= IsFixed;
    }
    Operand raw = load(variable.store, addressOf(value.variable()), flags);
    if (variable.mayBeSeenUninitialized())
        checkInitialized(raw);
    return ensure(raw, m_program.at(variable.type ? variable.type : m_program.types.anything), typeOf(result));
}

void Lowering::lowerPutModuleVariable(const Value& value)
{
    auto& variable = m_program.variables[value.variable().index()];
    didEmitWrite(Writes::Fields);
    store(variable.store, in(value.child(0), repOf(variable.store)), addressOf(value.variable()));
}

// Only compiled code gets to see an environment or the frame of a coroutine.
Operand Lowering::lowerGetEnvironmentField(ValueID result, const Value& value)
{
    auto& field = m_program.at(value.layout()).fields[value.field()];
    CellPointer cell = in<CellPointer>(value.child(0));
    unsigned flags = safeIfReaches(cell, field.offset + sizeOf(field.store));
    if (field.flags & fieldIsImmutable)
        flags |= IsFixed;
    else if (field.flags & fieldIsStoredOnce && field.flags & fieldIsNeverSeenUninitialized) {
        findStoresToVariables();
        if (!m_fieldsStoredTo.contains(static_cast<uint64_t>(value.layout().index()) << 32 | value.field()))
            flags |= IsFixed;
    }
    Operand raw = load(field.store, fieldAt(cell, field), flags);
    auto& stored = m_program.at(field.stored);
    if (stored.primitives & Primitive::Absent)
        checkInitialized(raw);
    return ensure(raw, stored.withoutPrimitives(Primitive::Absent), typeOf(result));
}

void Lowering::lowerPutEnvironmentField(const Value& value)
{
    auto& field = m_program.at(value.layout()).fields[value.field()];
    CellPointer cell = in<CellPointer>(value.child(0));
    ValueID stored = value.child(1);
    didEmitWrite(isLocalAllocation(m_function, value.child(0)) ? Writes::RuntimeState : Writes::Fields);
    store(field.store, in(stored, repOf(field.store)), fieldAt(cell, field));
    if (isLoweringInitializingStore())
        didLowerInitializingStore();
    else if (holdsCells(field.store) && typeOf(stored).hasCells())
        barrier(cell);
}

// What the language does and no function of the engine: the runtime carries it out.
Operand Lowering::lowerLanguageOperation(ValueID id, const Value& value)
{
    auto& builtin = m_builtins.at(value.builtin());
    RELEASE_ASSERT(builtin.implementation.kind == BuiltinImplementation::Operation);
    if (builtin.implementation.operation == LanguageOperation::ConstructWith)
        return lowerConstructWith(id, value);
    auto [count, list] = argumentBuffer(value.arguments);
    Boxed result = AOT_TYPED_OPERATION_WITH_EFFECTS(LanguageOperation, effectsOfBuiltinCall(value), word32(static_cast<int32_t>(builtin.implementation.operation)), count, list);
    auto& type = typeOf(id);
    return vouchesForItsType(m_program, m_function, value) ? convert(result, type, value.rep) : ensure(result, type);
}

Operand Lowering::lower(ValueID id, const Value& value)
{
    auto& type = typeOf(id);
    switch (value.opcode) {
    case Opcode::Parameter:
        RELEASE_ASSERT_NOT_REACHED();
    // A pass that puts a constant in the place of a value leaves it the type of that value.
    case Opcode::Undefined:
        return convert(undefined(), type, value.rep);
    case Opcode::Null:
        return convert(boxedConstant(jsNull()), type, value.rep);
    case Opcode::Boolean:
        return convert(boolean(value.boolean()), type, value.rep);
    case Opcode::Integer:
        return convert(integer(value.integer()), type, value.rep);
    case Opcode::Double:
        return convert(number(std::bit_cast<double>(value.bits())), type, value.rep);
    case Opcode::String:
        return convert(stringConstant(value.string()), type, value.rep);
    case Opcode::BigInt:
        return convert(AOT_TYPED_OPERATION(BigIntConstant, word32(value.string().index())), type, value.rep);
    case Opcode::FunctionReference:
        return convert(closureConstant(value.function()), type, value.rep);
    case Opcode::NewClosure:
        return convert(lowerNewClosure(value), type, value.rep);
    case Opcode::GetArgument: {
        uint32_t index = value.immediates[0];
        return ensure(either<Boxed>(below(word32(index), m_argumentCount), [&] {
            return Boxed(m_out.load64(m_out.address(m_heaps.AOTTyped_argumentBuffer, m_argumentBuffer.value(), index * sizeof(EncodedJSValue))));
        }, [&] {
            return undefined();
        }), type);
    }
    case Opcode::CreateRest: {
        Word32 first = word32(value.immediates[0]);
        Word32 count = select(below(first, m_argumentCount), Word32(m_out.sub(m_argumentCount.value(), first.value())), word32(0));
        return ensure(AOT_TYPED_OPERATION(NewArray, count, offsetBy(m_argumentBuffer, integer(value.immediates[0]), 3)), Type::engineClass(m_builtins.arrayClass()), type);
    }
    case Opcode::GetScope: {
        // It was found to be a closure, whatever its type says it may be.
        Operand closure = get(value.child(0));
        return scopeOfClosure(closure.rep == Rep::JSValue ? asCell(closure.as<Boxed>()) : closure.as<CellPointer>());
    }
    case Opcode::GetModuleVariable:
        return lowerGetModuleVariable(id, value);
    case Opcode::PutModuleVariable:
        lowerPutModuleVariable(value);
        return undefined();
    case Opcode::GetGlobal:
    case Opcode::GetGlobalForTypeOf:
        if (BuiltinName name = m_program.builtinName(value.string()); m_builtins.isGlobal(name) && !m_program.isExtended(name))
            return convert(builtinCell(m_builtins.cellOf(name)), type, value.rep);
        if (value.opcode == Opcode::GetGlobal)
            return ensure(AOT_TYPED_OPERATION(GetFree, word32(value.string().index())), type);
        return ensure(AOT_TYPED_OPERATION(GetFreeForTypeOf, word32(value.string().index())), type);
    case Opcode::PutGlobal:
        AOT_TYPED_OPERATION(SetFree, word32(value.string().index()), in<Boxed>(value.child(0)));
        return undefined();
    case Opcode::NewObjectWithShape:
        return lowerNewObject(id, value);
    case Opcode::GetField:
        if (!isObjectLayout(m_program.at(value.layout()).kind))
            return lowerGetEnvironmentField(id, value);
        return lowerGetField(id, value);
    case Opcode::PutField:
        if (!isObjectLayout(m_program.at(value.layout()).kind))
            lowerPutEnvironmentField(value);
        else
            lowerPutField(id, value);
        return undefined();
    case Opcode::GetById:
        return lowerGetById(id, value);
    case Opcode::PutById:
        lowerPutById(id, value);
        return undefined();
    case Opcode::InById:
        return lowerInById(value);
    case Opcode::DeleteById:
        return AOT_TYPED_OPERATION(DeleteByName, in<Boxed>(value.child(0)), word32(value.string().index()));
    case Opcode::InByVal:
        return AOT_TYPED_OPERATION(HasKey, in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
    case Opcode::DeleteByVal:
        return AOT_TYPED_OPERATION(DeleteByKey, in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
    case Opcode::GetByVal:
        return lowerGetByVal(id, value);
    case Opcode::PutByVal:
        lowerPutByVal(value);
        return undefined();
    case Opcode::NewArray: {
        auto [count, list] = argumentBuffer(value.arguments);
        return ensure(AOT_TYPED_OPERATION(NewArray, count, list), Type::engineClass(m_builtins.arrayClass()), type);
    }
    case Opcode::NewObject:
        return ensure(AOT_TYPED_OPERATION(NewObject), m_program.at(m_program.types.object), type);
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
    case Opcode::Pow:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
        return lowerBinary(id, value);
    case Opcode::Negate:
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
    case Opcode::BitNot:
        return lowerUnary(id, value);
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        return lowerRelational(value);
    case Opcode::StrictEq:
        return lowerStrictEq(value.child(0), value.child(1));
    case Opcode::Eq:
        return lowerLooseEq(value.child(0), value.child(1));
    case Opcode::LogicalNot:
        return logicalNot(get(value.child(0)).as<Word32>());
    case Opcode::ToBoolean:
        return lowerToBoolean(value.child(0));
    case Opcode::TypeOf:
        return convert(AOT_TYPED_OPERATION(TypeOf, in<Boxed>(value.child(0))), type, value.rep);
    case Opcode::InstanceOf:
        return AOT_TYPED_OPERATION(InstanceOf, in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
    case Opcode::ToString:
        return convert(toStringCell(value.child(0)), type, value.rep);
    case Opcode::ToPropertyKey:
        return ensure(AOT_TYPED_OPERATION(ToKey, in<Boxed>(value.child(0))), type);
    case Opcode::IsUndefinedOrNull:
        return lowerIsUndefinedOrNull(value.child(0));
    case Opcode::IsInstanceOfClass:
        return lowerIsInstanceOfClass(value);
    case Opcode::HasShapeOf:
        return lowerHasShapeOf(value);
    case Opcode::IsClosureOf:
        return lowerIsClosureOf(value);
    case Opcode::Strcat:
        return convert(lowerStrcat(value), type, value.rep);
    case Opcode::IteratorOpen:
        return lowerIteratorOpen(id, value);
    case Opcode::IteratorHasNext:
        return lowerIteratorHasNext(value);
    case Opcode::IteratorValue:
        return lowerIteratorValue(id, value);
    case Opcode::IteratorClose:
        lowerIteratorClose(value);
        return undefined();
    case Opcode::Call:
        return lowerCall(id, value);
    case Opcode::Construct:
        return lowerConstruct(id, value);
    case Opcode::CallMethod:
        return lowerCallMethod(id, value);
    case Opcode::CallDirect:
        return lowerCallDirect(id, value);
    case Opcode::CallBuiltin:
        if (m_builtins.at(value.builtin()).kind == BuiltinKind::Operation)
            return lowerLanguageOperation(id, value);
        return lowerCallBuiltin(id, value);
    case Opcode::GetExternalModule:
        return ensure(AOT_TYPED_OPERATION(GetExternalModule, word32(value.string().index())), type);
    // The passes have turned these into something else.
    case Opcode::Operation:
    case Opcode::Await:
    case Opcode::Yield:
    case Opcode::GetResumeMode:
        RELEASE_ASSERT_NOT_REACHED();
    case Opcode::Cast:
        return lowerCast(id, value);
    case Opcode::CatchValue:
        return ensure(AOT_TYPED_OPERATION(Catch), type);
    case Opcode::LoopHint:
        poll();
        loadICData();
        return undefined();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ImageKey keyOf(FunctionID function)
{
    ImageKey key { };
    key.module = 0;
    key.start = function.index();
    key.kind = ImageKey::typedFunction;
    return key;
}

bool compile(VM&, const Program& program, FunctionID id, CompiledCode& result)
{
    LoweringContext context;
    Procedure proc(/* usesSIMD = */ false);
    proc.setPositionIndependent();
    pinRegisters(proc);
#if CPU(X86_64)
    proc.code().setUnsavedCalleeSaves(RegisterSet { X86Registers::r12 });
#endif
    Lowering lowering(proc, program, id, context);
    if (!lowering.run()) {
        result.whyDeclined = lowering.whyFailed();
        return false;
    }
    estimateFrequencies(proc);
    if (Options::dumpAOTB3Graph() && result.mayBeDumped) [[unlikely]]
        dataLogLn("AOT B3:\n", proc);
    for (B3::Value* value : proc.values()) {
        if (!value->hasInt64())
            continue;
        uint64_t bits = value->asInt64();
        if (IntegersThatLookLikeAddresses::isOne(bits) && !context.wideIntegerConstants.contains(static_cast<int64_t>(bits))) {
            result.whyDeclined = "an address in the code"_s;
            return false;
        }
    }

    auto hasNoFrame = [&context](Air::Code& code) {
        return !context.numberOfCatchEntrypoints && !code.frameSize() && !code.calleeSaveRegisterAtOffsetList().registerCount() && !context.emitsCalls && !context.readsCallerFrame;
    };
    proc.code().setPrologueForEntrypoint(0, createSharedTask<Air::PrologueGeneratorFunction>([&](CCallHelpers& jit, Air::Code& code) {
        if (hasNoFrame(code))
            return;
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.emitFunctionPrologue();
        if (context.emitsCalls || code.frameSize() > 256) {
#if CPU(X86_64)
            constexpr GPRReg newStackPointer = CCallHelpers::s_scratchRegister;
            constexpr GPRReg vm = stubTemporaryGPRs[2];
            jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(code.frameSize())), GPRInfo::callFrameRegister, newStackPointer);
            jit.loadPtr(CCallHelpers::Address(instanceGPR, Instance::offsetOfVM()), vm);
            CCallHelpers::Jump fits = jit.branchPtr(CCallHelpers::BelowOrEqual, CCallHelpers::Address(vm, VM::offsetOfSoftStackLimit()), newStackPointer);
            context.stubCalls.tailCall(jit, Stub::ThrowStackOverflowAtPrologue);
            fits.link(&jit);
            jit.move(newStackPointer, CCallHelpers::stackPointerRegister);
#else
            context.stubCalls.call(jit, Stub::Prologue, code.frameSize(), CallSite { });
#endif
        } else if (code.frameSize())
            jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(code.frameSize()), CCallHelpers::stackPointerRegister);
        jit.emitSave(code.calleeSaveRegisterAtOffsetList());
    }));
    proc.code().setEpilogueGenerator(createSharedTask<Air::PrologueGeneratorFunction>([&](CCallHelpers& jit, Air::Code& code) {
        if (!hasNoFrame(code)) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            jit.emitRestore(code.calleeSaveRegisterAtOffsetList());
            if (code.frameSize())
                jit.emitFunctionEpilogue();
            else
                jit.emitFunctionEpilogueWithEmptyFrame();
        }
        jit.ret();
    }));

    for (unsigned i = 0; i < context.numberOfCatchEntrypoints; ++i) {
        proc.code().setPrologueForEntrypoint(i + 1, createSharedTask<Air::PrologueGeneratorFunction>([](CCallHelpers& jit, Air::Code& code) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            jit.addPtr(CCallHelpers::TrustedImm32(-static_cast<int32_t>(code.frameSize())), GPRInfo::callFrameRegister, CCallHelpers::stackPointerRegister);
        }));
    }

    generateToAir(proc);
    usePinnedRegistersDirectly(proc.code());
    Air::prepareForGeneration(proc.code());
    CCallHelpers jit;
    // The function starts where its code starts.
    CCallHelpers::Jump jumpToMainEntrypoint;
    if (context.numberOfCatchEntrypoints)
        jumpToMainEntrypoint = jit.jump();
    CCallHelpers::Label codeStart = jit.label();
    jit.setOopsEmitsBreakpointOnly();
    generate(proc, jit);
    if (jumpToMainEntrypoint.isSet())
        jumpToMainEntrypoint.linkTo(proc.code().entrypointLabel(0), &jit);
    else
        RELEASE_ASSERT(!CCallHelpers::differenceBetween(codeStart, proc.code().entrypointLabel(0)));
    jit.breakpoint();
    jit.padBeforePatch();
    Vector<uint32_t> storage(WTF::roundUpToMultipleOf<sizeof(uint32_t)>(jit.m_assembler.codeSize()) / sizeof(uint32_t));
    LinkBuffer linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(storage.mutableSpan().data()), storage.sizeInBytes(), LinkBuffer::Profile::FTL);

    CompiledFunctionInfo info;
    info.stubCalls = context.stubCalls.link(linkBuffer);
    info.codeSize = linkBuffer.size();
    void* start = linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr();
    info.convention = Convention { Signature::Registers, 0, false };
    if (program.at(id).isGenericEntrypoint())
        info.convention = Convention { program.at(id).has(FunctionUsesArgumentBuffer) ? Signature::List : Signature::Registers, lowering.genericEntrypointParameterCount(), true };
    info.frameSizeInBytes = proc.frameSize();
    info.knownCallees = WTF::move(context.knownCallees);
    info.indexReferences = context.indexReferences.link(linkBuffer);
    info.numSlots = context.sites.size();
    info.sites = WTF::move(context.sites);
    info.siteConstants.fill(0, info.numSlots);
    info.startsCold = info.numSlots;
    for (StringID name : context.identifiers) {
        info.typedIdentifierIndices.append(program.identifierNumbers[name.index()]);
        info.typedIdentifierNames.append(String::fromUTF8(program.string(name)));
    }
    info.calleeSaveRegisters = proc.calleeSaveRegisterAtOffsetList();
    for (unsigned i = 0; i < context.numberOfCatchEntrypoints; ++i)
        info.catchEntrypoints.append({ i, static_cast<unsigned>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(proc.code().entrypointLabel(i + 1)).untaggedPtr()) - static_cast<uint8_t*>(start)) });
    info.typedCatchEntrypointsForCallSites = WTF::move(context.catchEntrypointsForCallSites);
    MacroAssemblerCodeRef<JSEntryPtrTag> codeRef = FINALIZE_CODE_IF(Options::dumpAOTDisassembly() && result.mayBeDumped, linkBuffer, JSEntryPtrTag, nullptr, "AOT code");
    result.bytes.append(std::span { static_cast<const uint8_t*>(start), static_cast<size_t>(info.codeSize) });
    result.info = WTF::move(info);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
