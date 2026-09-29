/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(FTL_JIT)

#include "AOTProgram.h"
#include "AOTStubs.h"
#include "BytecodeStructs.h"
#include "BytecodeUseDef.h"
#include "JSCInlines.h"
#include "JSTemplateObjectDescriptor.h"
#include "PreciseJumpTargetsInlines.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "ImmutableIntrinsics.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

static Type typeOfCellOfType(JSType type)
{
    switch (type) {
    case StringType:
        return TString;
    case SymbolType:
        return TSymbol;
    case HeapBigIntType:
        return TBigInt;
    case JSFunctionType:
    case InternalFunctionType:
        return TFunction;
    case ArrayType:
    case DerivedArrayType:
        return TArray;
    case FinalObjectType:
        return TFinalObject;
    case JSMapType:
        return TMap;
    case JSSetType:
        return TSet;
    case JSWeakMapType:
        return TWeakMap;
    case JSWeakSetType:
        return TWeakSet;
    case RegExpObjectType:
        return TRegExp;
    case JSPromiseType:
        return TPromise;
    case JSDateType:
        return TDate;
    case ErrorInstanceType:
        return TError;
    case ArrayBufferType:
        return TArrayBuffer;
    case DataViewType:
        return TDataView;
    case StringObjectType:
        return TStringObject;
    default:
        if (isTypedArrayType(type))
            return typeOfTypedArray(type);
        return type >= ObjectType ? TOtherObject : TCellOther;
    }
}

Type typeOfValue(JSValue value)
{
    if (!value)
        return TEmpty;
    if (value.isInt32())
        return TInt32;
    if (value.isDouble())
        return TDouble;
    if (value.isBoolean())
        return TBoolean;
    if (value.isUndefined())
        return TUndefined;
    if (value.isNull())
        return TNull;
    if (!value.isCell())
        return TTop;
    return typeOfCellOfType(value.asCell()->type());
}

void dumpType(PrintStream& out, Type type)
{
    if (!type) {
        out.print("None");
        return;
    }
    if (type == TAll) {
        out.print("All");
        return;
    }
    if (type == TTop) {
        out.print("Top");
        return;
    }
    CommaPrinter bar("|"_s);
    auto take = [&](Type bits, ASCIILiteral name) {
        if ((type & bits) == bits) {
            out.print(bar, name);
            type &= ~bits;
        }
    };
    take(TNumber, "Number"_s);
    take(TAnyObject, "AnyObject"_s);
    take(TInt32, "Int32"_s);
    take(TDouble, "Double"_s);
    take(TBoolean, "Boolean"_s);
    take(TUndefined, "Undefined"_s);
    take(TNull, "Null"_s);
    take(TString, "String"_s);
    take(TSymbol, "Symbol"_s);
    take(TBigInt, "BigInt"_s);
    take(TFunction, "Function"_s);
    take(TArray, "Array"_s);
    take(TObject, "Object"_s);
    take(TOtherObject, "OtherObject"_s);
    take(TFinalObject, "FinalObject"_s);
    take(TMap, "Map"_s);
    take(TSet, "Set"_s);
    take(TWeakMap, "WeakMap"_s);
    take(TWeakSet, "WeakSet"_s);
    take(TRegExp, "RegExp"_s);
    take(TPromise, "Promise"_s);
    take(TDate, "Date"_s);
    take(TError, "Error"_s);
    take(TArrayBuffer, "ArrayBuffer"_s);
    take(TDataView, "DataView"_s);
    take(TStringObject, "StringObject"_s);
    take(TTypedArray, "TypedArray"_s);
    for (unsigned i = 0; i < NumberOfTypedArrayTypesExcludingDataView; ++i) {
        if (type & (1u << (firstTypedArrayBit + i))) {
            out.print(bar, "TypedArray", i);
            type &= ~(1u << (firstTypedArrayBit + i));
        }
    }
    take(TCellOther, "CellOther"_s);
    take(TEmpty, "Empty"_s);
}

Graph::Graph(VM& vm, UnlinkedCodeBlock* codeBlock, const ScopeChain& scopeChain)
    : m_vm(vm)
    , m_codeBlock(codeBlock)
    , m_scopeChain(scopeChain)
    , m_numArguments(codeBlock->numParameters())
    , m_numLocals(codeBlock->numCalleeLocals())
{
}

Graph::~Graph() = default;

Node* Graph::addNode(NodeKind kind)
{
    Node& node = m_nodes.alloc();
    node.kind = kind;
    node.index = m_nodes.size() - 1;
    return &node;
}

BasicBlock* Graph::addBlock()
{
    blocks.append(makeUniqueWithoutFastMallocCheck<BasicBlock>());
    blocks.last()->index = blocks.size() - 1;
    return blocks.last().get();
}

Node* Graph::constant(JSValue value)
{
    ASSERT(!value || !value.isCell());
    if (!value) {
        // Not a key the table can hold.
        if (!m_emptyConstant) {
            m_emptyConstant = addNode(NodeKind::Constant);
            m_emptyConstant->type = TEmpty;
            m_emptyConstant->range = IntegerRange::unknown();
        }
        return m_emptyConstant;
    }
    auto result = m_constants.add(JSValue::encode(value), nullptr);
    if (result.isNewEntry) {
        Node* node = addNode(NodeKind::Constant);
        node->constant = value;
        node->type = typeOfValue(value);
        node->range = IntegerRange::ofNumber(value);
        result.iterator->value = node;
    }
    return result.iterator->value;
}

NewObjectPlan NewObjectPlan::forCreateThis(const JSInstructionStream& instructions, unsigned offsetOfCreateThis)
{
    NewObjectPlan plan;
    auto createThis = instructions.at(offsetOfCreateThis);
    Vector<VirtualRegister, 4> aliases;
    aliases.append(createThis->as<OpCreateThis>().m_dst);
    constexpr unsigned maximumCount = 64;
    // Nothing gets in here from elsewhere: what a jump goes to is either after a jump or the top of a loop, and this stops at both.
    for (unsigned offset = offsetOfCreateThis + createThis->size(); offset < instructions.size(); offset += instructions.at(offset)->size()) {
        auto instruction = instructions.at(offset);
        switch (instruction->opcodeID()) {
        case op_mov: {
            auto bytecode = instruction->as<OpMov>();
            if (aliases.contains(bytecode.m_src)) {
                if (!aliases.contains(bytecode.m_dst))
                    aliases.append(bytecode.m_dst);
            } else
                aliases.removeAll(bytecode.m_dst);
            if (aliases.isEmpty())
                return plan;
            continue;
        }
        case op_check_type:
            // If it throws there is no object to be told anything by. That the object would have been made first can be told, if it
            // is made on behalf of a proxy: not a thing to hold every constructor back for.
            if (aliases.contains(instruction->as<OpCheckType>().m_value))
                return plan;
            continue;
        case op_put_by_id: {
            auto bytecode = instruction->as<OpPutById>();
            if (!aliases.contains(bytecode.m_base) || aliases.contains(bytecode.m_value))
                return plan;
            size_t index = plan.properties.findIf([&](auto& property) { return property.identifier == bytecode.m_property; });
            if (index == notFound) {
                if (plan.properties.size() == maximumCount)
                    return plan;
                index = plan.properties.size();
                plan.properties.append({ bytecode.m_property, bytecode.m_flags.isDirect(), bytecode.m_flags.ecmaMode().isStrict() });
            } else if (!plan.properties[index].isDefined) {
                // If a setter took the first it takes the second too, and only the last is kept.
                return plan;
            }
            plan.stores.append({ offset, static_cast<unsigned>(index) });
            continue;
        }
        default:
            return plan;
        }
    }
    return plan;
}

Vector<unsigned, 16> Graph::storesOfLiteral(const JSInstructionStream& instructions, unsigned offsetOfNewObject)
{
    Vector<unsigned, 16> stores;
    auto newObject = instructions.at(offsetOfNewObject);
    VirtualRegister object = newObject->as<OpNewObject>().m_dst;
    constexpr unsigned maximumCount = 2048; // Their values are all kept until the last.
    // Nothing gets in here from elsewhere: what a jump goes to is either after a jump or the top of a loop or a handler, and this
    // stops at all of them.
    for (unsigned offset = offsetOfNewObject + newObject->size(); offset < instructions.size() && stores.size() < maximumCount; offset += instructions.at(offset)->size()) {
        auto instruction = instructions.at(offset);
        OpcodeID opcode = instruction->opcodeID();
        if (opcode == op_put_by_id) {
            auto bytecode = instruction->as<OpPutById>();
            if (bytecode.m_base == object) {
                if (bytecode.m_value == object || !bytecode.m_flags.isDirect())
                    break;
                stores.append(offset);
                continue;
            }
        }
        if (isBranch(opcode) || isTerminal(opcode) || isThrow(opcode) || opcode == op_loop_hint || opcode == op_catch)
            break;
        bool knowsOfObject = false;
        for (unsigned checkpoint = 0; checkpoint < instruction->numberOfCheckpoints(); ++checkpoint) {
            computeUsesForBytecodeIndexImpl(instruction.ptr(), checkpoint, [&](VirtualRegister reg) {
                knowsOfObject |= reg == object;
            });
            computeDefsForBytecodeIndexImpl(0, instruction.ptr(), checkpoint, [&](VirtualRegister reg) {
                knowsOfObject |= reg == object;
            });
        }
        if (knowsOfObject)
            break;
    }
    return stores;
}

Node* Graph::intrinsic(unsigned number)
{
    const ImmutableIntrinsics::Entry& entry = ImmutableIntrinsics::shared()->at(number);
    if (!entry.isCell)
        return constant(JSValue::decode(entry.primitive));
    return m_intrinsics.ensure(entry.canonical, [&] {
        Node* node = addNode(NodeKind::Intrinsic);
        node->intrinsic = entry.canonical;
        node->type = typeOfCellOfType(entry.type);
        node->range = IntegerRange::unknown();
        return node;
    }).iterator->value;
}

Node* Graph::intrinsicReadBy(const JSInstruction* instruction, Node* base)
{
    if (!Options::useImmutableIntrinsics())
        return nullptr;
    const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
    if (!intrinsics)
        return nullptr;
    // No scope between here and the global object has the name, and none can be given it: a variable that cannot be reconfigured
    // cannot be shadowed by a declaration of a later script either.
    auto variable = [&](unsigned identifier, unsigned depth, ResolveType type) -> unsigned {
        if (isStaticClosureVarResolveType(type) || type == ResolvedClosureVar || type == ResolvedLazyClosureVar || !resolveStatically(identifier, depth, type).isGlobal)
            return 0;
        return intrinsics->find(ImmutableIntrinsics::globalObject, *m_codeBlock->identifier(identifier).impl());
    };
    unsigned number = 0;
    switch (instruction->opcodeID()) {
    case op_resolve_scope: {
        auto bytecode = instruction->as<OpResolveScope>();
        if (!variable(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType))
            return nullptr;
        return intrinsic(ImmutableIntrinsics::globalObject);
    }
    case op_get_from_scope: {
        auto bytecode = instruction->as<OpGetFromScope>();
        number = variable(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_getPutInfo.resolveType());
        break;
    }
    case op_get_by_id:
        if (base && base->kind == NodeKind::Intrinsic && base->intrinsic != ImmutableIntrinsics::globalObject)
            number = intrinsics->find(base->intrinsic, *m_codeBlock->identifier(instruction->as<OpGetById>().m_property).impl());
        break;
    default:
        break;
    }
    if (!number)
        return nullptr;
    numberOfIntrinsicReads++;
    return intrinsic(number);
}

CallIntrinsic callIntrinsicFor(UniquedStringImpl* name, unsigned argumentCountIncludingThis)
{
    static constexpr struct {
        ASCIILiteral name;
        unsigned argumentCount;
        CallIntrinsic intrinsic;
    } table[] = {
        { "sqrt"_s, 1, CallIntrinsic::MathSqrt }, { "abs"_s, 1, CallIntrinsic::MathAbs }, { "floor"_s, 1, CallIntrinsic::MathFloor },
        { "ceil"_s, 1, CallIntrinsic::MathCeil }, { "trunc"_s, 1, CallIntrinsic::MathTrunc }, { "fround"_s, 1, CallIntrinsic::MathFround },
        { "min"_s, 2, CallIntrinsic::MathMin }, { "max"_s, 2, CallIntrinsic::MathMax }, { "imul"_s, 2, CallIntrinsic::MathIMul },
        { "charCodeAt"_s, 1, CallIntrinsic::StringCharCodeAt }, { "push"_s, 1, CallIntrinsic::ArrayPush },
    };
    if (name->length() > 10 || name->isSymbol())
        return CallIntrinsic::None;
    for (auto& entry : table) {
        if (entry.argumentCount + 1 == argumentCountIncludingThis && WTF::equal(name, entry.name))
            return entry.intrinsic;
    }
    return CallIntrinsic::None;
}

Graph::CallOperands Graph::operandsOfCall(const JSInstruction* instruction)
{
    if (instruction->opcodeID() == op_call) {
        auto bytecode = instruction->as<OpCall>();
        return { bytecode.m_callee, bytecode.m_argc, bytecode.m_argv };
    }
    if (instruction->opcodeID() == op_tail_call) {
        auto bytecode = instruction->as<OpTailCall>();
        return { bytecode.m_callee, bytecode.m_argc, bytecode.m_argv };
    }
    auto bytecode = instruction->as<OpCallIgnoreResult>();
    return { bytecode.m_callee, bytecode.m_argc, bytecode.m_argv };
}

CallIntrinsic Graph::intrinsicOfCall(const Node* node) const
{
    if (node->opcode != op_call && node->opcode != op_call_ignore_result)
        return CallIntrinsic::None;
    CallOperands operands = operandsOfCall(node->instruction);
    Node* callee = node->use(operands.callee);
    if (callee->kind == NodeKind::Intrinsic) {
        // It is what it is, whatever it was found under.
        const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
        const ImmutableIntrinsics::Entry& entry = intrinsics->at(callee->intrinsic);
        const ImmutableIntrinsics::Entry& holder = intrinsics->at(entry.holder);
        if (holder.holder != ImmutableIntrinsics::globalObject || holder.name != "Math"_s)
            return CallIntrinsic::None;
        CallIntrinsic result = callIntrinsicFor(static_cast<UniquedStringImpl*>(entry.name.impl()), operands.argc);
        return result == CallIntrinsic::StringCharCodeAt || result == CallIntrinsic::ArrayPush ? CallIntrinsic::None : result;
    }
    if (!callee->isBytecode(op_get_by_id))
        return CallIntrinsic::None;
    return callIntrinsicFor(m_codeBlock->identifier(callee->as<OpGetById>().m_property).impl(), operands.argc);
}

std::optional<JSType> Graph::typedArrayAccessed(const Node* node)
{
    Node* base;
    if (node->opcode == op_get_by_val)
        base = node->use(node->as<OpGetByVal>().m_base);
    else if (node->opcode == op_put_by_val)
        base = node->use(node->as<OpPutByVal>().m_base);
    else
        return std::nullopt;
    auto type = typedArrayTypeOf(base->type);
    if (!type)
        return std::nullopt;
    switch (*type) {
    case Int8ArrayType:
    case Uint8ArrayType:
    case Int16ArrayType:
    case Uint16ArrayType:
    case Int32ArrayType:
    case Uint32ArrayType:
    case Float32ArrayType:
    case Float64ArrayType:
        return type;
    default:
        return std::nullopt;
    }
}

std::pair<Node*, Node*> Graph::arrayAndElementStored(const Node* node) const
{
    if (node->kind != NodeKind::Bytecode)
        return { nullptr, nullptr };
    if (node->opcode == op_put_by_val) {
        auto bytecode = node->as<OpPutByVal>();
        return { node->use(bytecode.m_base), node->use(bytecode.m_value) };
    }
    if (intrinsicOfCall(node) == CallIntrinsic::ArrayPush) {
        auto operands = operandsOfCall(node->instruction);
        return { node->use(operands.argument(0)), node->use(operands.argument(1)) };
    }
    return { nullptr, nullptr };
}

std::optional<uint32_t> Graph::distanceOfEnvironmentAccessed(const Node* node)
{
    if (!m_linkage)
        return std::nullopt;
    unsigned identifier;
    unsigned offset;
    unsigned depth;
    VirtualRegister scopeRegister;
    ResolveType type;
    if (node->isBytecode(op_get_from_scope)) {
        auto bytecode = node->as<OpGetFromScope>();
        identifier = bytecode.m_var;
        offset = bytecode.m_offset;
        depth = bytecode.m_localScopeDepth;
        scopeRegister = bytecode.m_scope;
        type = bytecode.m_getPutInfo.resolveType();
    } else {
        auto bytecode = node->as<OpPutToScope>();
        identifier = bytecode.m_var;
        offset = bytecode.m_offset;
        depth = bytecode.m_symbolTableOrScopeDepth.scopeDepth();
        scopeRegister = bytecode.m_scope;
        type = bytecode.m_getPutInfo.resolveType();
    }
    auto found = [&](uint32_t distance) -> std::optional<uint32_t> {
        if (!distance)
            return std::nullopt;
        usesStaticImports = true;
        return distance;
    };
    if (type == ResolvedClosureVar || type == ResolvedLazyClosureVar) {
        if (!m_declaredNames)
            return std::nullopt;
        // As for Graph::knownCallee(): not a variable of the function's own, and not one of a scope in between.
        auto resolution = m_declaredNames->resolve(m_codeBlock->identifier(identifier).impl());
        if (resolution.kind != DeclaredNamesLink::Resolution::Slot || !resolution.isInOutermostEnvironment || resolution.offset != offset)
            return std::nullopt;
        if (isThatManyScopesOut(node->use(scopeRegister), resolution.hops))
            return found(m_linkage->distanceOfEnvironment);
        return std::nullopt;
    }
    if (type == Dynamic)
        return std::nullopt;
    auto variable = resolveStatically(identifier, depth, type);
    if (variable.kind == StaticVariable::Closure && variable.isInOutermostEnvironment)
        return found(m_linkage->distanceOfEnvironment);
    if (variable.kind == StaticVariable::Import && node->isBytecode(op_get_from_scope))
        return found(variable.import.distanceOfEnvironment);
    return std::nullopt;
}

bool Graph::isScopeThatStandsForNoThis(const Node* node)
{
    if (node->isBytecode(op_get_scope))
        return true;
    if (node->kind == NodeKind::Intrinsic)
        return node->intrinsic == ImmutableIntrinsics::globalObject;
    if (!node->isBytecode(op_resolve_scope))
        return false;
    auto bytecode = node->as<OpResolveScope>();
    if (isStaticClosureVarResolveType(bytecode.m_resolveType))
        return true;
    if (bytecode.m_resolveType == Dynamic)
        return false;
    auto kind = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType).kind;
    return kind == StaticVariable::Import || kind == StaticVariable::Closure;
}

// Counting from the scope that the function was made in.
bool Graph::isThatManyScopesOut(const Node* scope, unsigned hops)
{
    if (scope->isBytecode(op_get_scope))
        return !hops;
    if (!scope->isBytecode(op_resolve_scope))
        return false;
    ResolveType type = scope->as<OpResolveScope>().m_resolveType;
    return isStaticClosureVarResolveType(type) && staticClosureVarHops(type) == hops;
}

std::optional<uint32_t> Graph::distanceOfEnvironmentResolvedTo(const Node* node)
{
    if (!m_linkage)
        return std::nullopt;
    auto bytecode = node->as<OpResolveScope>();
    uint32_t distance = 0;
    if (isStaticClosureVarResolveType(bytecode.m_resolveType)) {
        if (!m_declaredNames)
            return std::nullopt;
        auto resolution = m_declaredNames->resolve(m_codeBlock->identifier(bytecode.m_var).impl());
        if (resolution.kind == DeclaredNamesLink::Resolution::Slot && resolution.isInOutermostEnvironment && resolution.hops == staticClosureVarHops(bytecode.m_resolveType))
            distance = m_linkage->distanceOfEnvironment;
    } else if (bytecode.m_resolveType != Dynamic) {
        if (auto variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType); variable.kind == StaticVariable::Import)
            distance = variable.import.distanceOfEnvironment;
        else if (variable.kind == StaticVariable::Closure && variable.isInOutermostEnvironment)
            distance = m_linkage->distanceOfEnvironment;
    }
    if (!distance)
        return std::nullopt;
    usesStaticImports = true;
    return distance;
}

bool Graph::calleeIsProven(const Node* node) const
{
    bool isProven = false;
    return knownCallee(node, &isProven) && isProven;
}

bool Graph::isSiblingCall(const Node* node) const
{
    if (!usesStubs || !Options::aotSiblingCalls() || (Options::aotDisableFastPaths() & 128) || !node->isBytecode(op_tail_call) || m_codeBlock->codeType() != FunctionCode)
        return false;
    unsigned argumentCountIncludingThis = node->as<OpTailCall>().m_argc;
    if (argumentCountIncludingThis > m_codeBlock->numParameters() || argumentCountIncludingThis > mostArgumentsOfSiblingCall)
        return false;
    bool isProven = false;
    const KnownFunction* known = knownCallee(node, &isProven);
    // (A function that is passed too few moves its frame down to make room for the rest. One that did that every time round would
    // run out of stack, which is what a call in tail position is not to do.)
    return known && isProven && known->forCall && argumentCountIncludingThis >= known->forCall->numParameters();
}

const KnownFunction* Graph::knownCallee(const Node* node, bool* isProven) const
{
    bool proven = false;
    const KnownFunction* known = knownCalleeWithoutFacts(node, &proven);
    // Only where nothing better is known: what is known of a closed function is known from the calls that are proven the other way.
    if ((!known || !proven) && node->hasFact(FactDirect, 4)) {
        if (const KnownFunction* body = bodyOfFact(node->fact & 0xfffffff)) {
            known = body;
            proven = true;
        }
    }
    if (isProven)
        *isProven = proven;
    return known;
}

unsigned Graph::iteratedFactOf(const Node* node)
{
    if (!(Options::aotFacts() & 16))
        return 0;
    VirtualRegister iterable;
    switch (node->opcode) {
    case op_iterator_open:
        iterable = node->as<OpIteratorOpen>().m_iterable;
        break;
    case op_iterator_next:
        iterable = node->as<OpIteratorNext>().m_iterable;
        break;
    case op_iterator_close_check:
        iterable = node->as<OpIteratorCloseCheck>().m_iterable;
        break;
    default:
        return 0;
    }
    for (auto& use : node->uses) {
        if (use.reg != iterable)
            continue;
        const Node* value = use.node;
        while (value->isBytecode(op_check_type))
            value = value->use(value->as<OpCheckType>().m_value);
        return value->iteratedFact;
    }
    // The half of an op_iterator_close_check that is the branch: it has the other half for its iterator, and nothing else.
    if (node->opcode == op_iterator_close_check && node->uses.size() == 1 && node->uses[0].node->isBytecode(op_iterator_close_check))
        return iteratedFactOf(node->uses[0].node);
    return 0;
}

const KnownFunction* Graph::knownCalleeWithoutFacts(const Node* node, bool* isProven) const
{
    if (!m_hints)
        return nullptr;
    VirtualRegister calleeRegister;
    switch (node->opcode) {
    case op_call:
        calleeRegister = node->as<OpCall>().m_callee;
        break;
    case op_call_ignore_result:
        calleeRegister = node->as<OpCallIgnoreResult>().m_callee;
        break;
    case op_construct:
        calleeRegister = node->as<OpConstruct>().m_callee;
        break;
    case op_tail_call:
        calleeRegister = node->as<OpTailCall>().m_callee;
        break;
    default:
        return nullptr;
    }
    // In the generic copy of a loop what was read comes by way of phis, from that copy and from the other.
    Node* callee = node->use(calleeRegister);
    for (unsigned depth = 0; callee->kind == NodeKind::Phi && depth < 4; ++depth) {
        Node* source = nullptr;
        for (auto& use : callee->uses) {
            Node* input = use.node;
            if (input == callee || input == source)
                continue;
            if (source && source->isBytecode(op_get_from_scope) && input->isBytecode(op_get_from_scope) && source->instruction == input->instruction)
                continue;
            if (source)
                return nullptr;
            source = input;
        }
        if (!source)
            return nullptr;
        callee = source;
    }
    if (!callee->isBytecode(op_get_from_scope))
        return nullptr;
    // (Through a phi it is a hint like any other.)
    return knownFunctionReadBy(callee, callee == node->use(calleeRegister) ? isProven : nullptr);
}

const KnownFunction* Graph::knownFunctionReadBy(const Node* callee, bool* isProven) const
{
    if (!m_hints)
        return nullptr;
    auto bytecode = callee->as<OpGetFromScope>();
    UniquedStringImpl* name = m_codeBlock->identifier(bytecode.m_var).impl();
    ResolveType type = bytecode.m_getPutInfo.resolveType();
    if (type == ResolvedClosureVar || type == ResolvedLazyClosureVar) {
        // Is it the module's variable? Not one of the function's own, and not one of a scope in between.
        const void* scope = const_cast<Graph*>(this)->identityOfScope(callee->use(bytecode.m_scope));
        // If there is no telling which scope it is read from, it may be: which is no proof of anything, but is not to be overlooked.
        if (!scope)
            return probablyFunctionInVariableOfModule(bytecode.m_var, bytecode.m_offset);
        if (scope != m_hints->scopeOfVariables())
            return nullptr;
        const KnownFunction* known = m_hints->find(name, bytecode.m_offset);
        if (known && isProven)
            *isProven = known->isProven;
        return known;
    }
    if (type == Dynamic)
        return nullptr;
    if (auto variable = const_cast<Graph*>(this)->resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type); variable.kind == StaticVariable::Import) {
        if (isProven && variable.import.function)
            *isProven = variable.import.function->isProven;
        return variable.import.function;
    } else if (variable.kind == StaticVariable::ModuleImport && m_linkage) {
        // An import that this code is not compiled to read from where it is (resolveStatically()). It is read all the same.
        if (const StaticImport* import = m_linkage->findImport(name); import && import->function)
            return import->function;
    }
    // A read that was left to be resolved when the code is linked (BytecodeOptimizerAccess::resolveScopesStatically() only goes so
    // far). It reads what it reads all the same: which is no proof of anything, but is not to be overlooked either.
    if (m_declaredNames) {
        auto resolution = m_declaredNames->resolve(name);
        switch (resolution.kind) {
        case DeclaredNamesLink::Resolution::Slot: {
            if (!resolution.isInOutermostEnvironment)
                return nullptr;
            const KnownFunction* known = m_hints->find(name, resolution.offset);
            // Is the scope that it is read from the one that the name is found in?
            if (known && known->isProven && isProven && Options::aotResolvesScopesItself())
                *isProven = const_cast<Graph*>(this)->identityOfScope(callee->use(bytecode.m_scope)) == m_hints->scopeOfVariables();
            return known;
        }
        case DeclaredNamesLink::Resolution::Stable:
        case DeclaredNamesLink::Resolution::Global:
            return m_hints->find(name, std::nullopt);
        case DeclaredNamesLink::Resolution::Dynamic:
            break;
        }
    }
    return m_hints->find(name, std::nullopt);
}

const KnownFunction* Graph::probablyFunctionInVariableOfModule(unsigned identifier, unsigned scopeOffset) const
{
    if (!m_hints || !m_declaredNames)
        return nullptr;
    UniquedStringImpl* name = m_codeBlock->identifier(identifier).impl();
    auto resolution = m_declaredNames->resolve(name);
    if (resolution.kind != DeclaredNamesLink::Resolution::Slot || !resolution.isInOutermostEnvironment || resolution.offset != scopeOffset)
        return nullptr;
    return m_hints->find(name, scopeOffset);
}

bool Graph::passesNoFunctionObject(const Node* node)
{
    VirtualRegister calleeRegister;
    if (node->isBytecode(op_call))
        calleeRegister = node->as<OpCall>().m_callee;
    else if (node->isBytecode(op_call_ignore_result))
        calleeRegister = node->as<OpCallIgnoreResult>().m_callee;
    else if (isSiblingCall(node))
        calleeRegister = node->as<OpTailCall>().m_callee;
    else
        return false;
    bool isProven = false;
    const KnownFunction* known = knownCallee(node, &isProven);
    if (!known || !isProven || !known->needsNoFunctionObject.load(std::memory_order_relaxed))
        return false;
    if (!node->use(calleeRegister)->isBytecode(op_get_from_scope))
        return node->hasFact(FactDirect, 4);
    // The function goes by where the environment of its module is, so that had better be known here too. (This says that the code
    // rests on it: what is called does not look.)
    return !!distanceOfEnvironmentAccessed(node->use(calleeRegister));
}

uint32_t Graph::distanceOfEnvironmentOfModule()
{
    RELEASE_ASSERT(m_scopeIsEnvironmentOfModule);
    usesStaticImports = true;
    return m_linkage->distanceOfEnvironment;
}

void Graph::elideReadsOfCalleesNotPassed()
{
    // How many of a read's uses want the value.
    UncheckedKeyHashMap<Node*, unsigned> wanted;
    Vector<Node*, 16> reads;
    auto note = [&](Node* user, const Use& use) {
        if (!use.node->isBytecode(op_get_from_scope) && !((Options::aotFacts() & 4) && use.node->isBytecode(op_get_by_id)))
            return;
        bool wantsValue = true;
        if (user->isBytecode(op_check_tdz))
            wantsValue = mayBe(use.node->type, TEmpty);
        else if (user->isBytecode(op_call))
            wantsValue = use.reg != user->as<OpCall>().m_callee || !passesNoFunctionObject(user) || !knownCallee(user)->isDeclaration;
        else if (user->isBytecode(op_call_ignore_result))
            wantsValue = use.reg != user->as<OpCallIgnoreResult>().m_callee || !passesNoFunctionObject(user) || !knownCallee(user)->isDeclaration;
        else if (user->isBytecode(op_tail_call))
            wantsValue = use.reg != user->as<OpTailCall>().m_callee || !passesNoFunctionObject(user) || !knownCallee(user)->isDeclaration;
        auto result = wanted.add(use.node, 0);
        if (result.isNewEntry)
            reads.append(use.node);
        result.iterator->value += wantsValue;
    };
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            for (auto& use : node->uses)
                note(node, use);
        }
        for (Node* phi : block->phis) {
            for (auto& use : phi->uses)
                note(phi, use);
        }
    }
    for (Node* read : reads) {
        if (wanted.get(read))
            continue;
        // Reading it does nothing that anybody can see: it holds the function, or is about to be given it (a declaration's is made
        // when it is first read).
        if (read->isBytecode(op_get_by_id)) {
            read->isElided = true;
            continue;
        }
        bool isProven = false;
        read->isElided = knownFunctionReadBy(read, &isProven) && isProven;
    }
}

const void* Graph::identityOfScope(const Node* scope, unsigned depth)
{
    if (depth > 6)
        return nullptr;
    auto fromOutside = [&](unsigned hops) -> const void* {
        return m_declaredNames ? m_declaredNames->identityOfScope(hops) : nullptr;
    };
    auto ofTable = [&](VirtualRegister reg) -> const void* {
        if (!reg.isConstant())
            return nullptr;
        JSValue table = m_codeBlock->getConstant(reg);
        return table && table.isCell() ? table.asCell() : nullptr;
    };
    // What comes by way of phis and of memory: all of it the same one.
    if (scope->kind != NodeKind::Bytecode) {
        Vector<const Node*, 16> worklist { scope };
        UncheckedKeyHashSet<const Node*> seen;
        const void* result = nullptr;
        while (!worklist.isEmpty()) {
            const Node* node = worklist.takeLast();
            if (!seen.add(node).isNewEntry)
                continue;
            switch (node->kind) {
            case NodeKind::Phi:
                for (auto& use : node->uses)
                    worklist.append(use.node);
                break;
            case NodeKind::Narrow:
                worklist.append(node->uses[0].node);
                break;
            case NodeKind::GetStack: {
                // A register that lives in memory holds whatever was last put there.
                if (!std::exchange(m_hasStoresToHomed, true)) {
                    for (BasicBlock* block : m_rpo) {
                        for (Node* store : block->nodes) {
                            if (store->kind == NodeKind::SetStack)
                                m_storesToHomed.add(store->reg.offset(), Vector<Node*>()).iterator->value.append(store);
                        }
                    }
                }
                auto it = m_storesToHomed.find(node->reg.offset());
                if (it == m_storesToHomed.end())
                    return nullptr;
                for (Node* store : it->value)
                    worklist.append(store->uses[0].node);
                break;
            }
            case NodeKind::Constant:
                // What a register holds until it is given a scope. Nothing is read from that.
                break;
            case NodeKind::Bytecode: {
                const void* identity = identityOfScope(node, depth + 1);
                if (!identity || (result && result != identity))
                    return nullptr;
                result = identity;
                break;
            }
            default:
                return nullptr;
            }
        }
        return result;
    }
    switch (scope->kind) {
    case NodeKind::Bytecode:
        switch (scope->opcode) {
        case op_create_lexical_environment:
            return ofTable(scope->as<OpCreateLexicalEnvironment>().m_symbolTable);
        case op_create_generator_frame_environment:
            return ofTable(scope->as<OpCreateGeneratorFrameEnvironment>().m_symbolTable);
        case op_get_scope:
            return fromOutside(0);
        case op_resolve_scope: {
            ResolveType type = scope->as<OpResolveScope>().m_resolveType;
            if (isStaticClosureVarResolveType(type))
                return fromOutside(staticClosureVarHops(type));
            if (type == GlobalProperty && m_declaredNames) {
                auto resolution = m_declaredNames->resolve(m_codeBlock->identifier(scope->as<OpResolveScope>().m_var).impl());
                if (resolution.kind == DeclaredNamesLink::Resolution::Slot)
                    return resolution.scope;
            }
            return nullptr;
        }
        case op_get_parent_scope: {
            const Node* inner = scope->use(scope->as<OpGetParentScope>().m_scope);
            if (inner->isBytecode(op_create_lexical_environment))
                return identityOfScope(inner->use(inner->as<OpCreateLexicalEnvironment>().m_scope), depth + 1);
            if (inner->isBytecode(op_create_generator_frame_environment))
                return identityOfScope(inner->use(inner->as<OpCreateGeneratorFrameEnvironment>().m_scope), depth + 1);
            if (inner->isBytecode(op_get_scope))
                return fromOutside(1);
            return nullptr;
        }
        default:
            return nullptr;
        }
    default:
        return nullptr;
    }
}

Variable Graph::variableAccessedBy(const Node* node)
{
    VirtualRegister scope;
    unsigned identifier;
    unsigned offset;
    unsigned localScopeDepth = 0;
    VirtualRegister tableOfScope;
    ResolveType type;
    if (node->isBytecode(op_get_from_scope)) {
        auto bytecode = node->as<OpGetFromScope>();
        scope = bytecode.m_scope;
        identifier = bytecode.m_var;
        offset = bytecode.m_offset;
        localScopeDepth = bytecode.m_localScopeDepth;
        type = bytecode.m_getPutInfo.resolveType();
    } else {
        auto bytecode = node->as<OpPutToScope>();
        scope = bytecode.m_scope;
        identifier = bytecode.m_var;
        offset = bytecode.m_offset;
        type = bytecode.m_getPutInfo.resolveType();
        if (type != ResolvedClosureVar && type != ResolvedLazyClosureVar)
            localScopeDepth = bytecode.m_symbolTableOrScopeDepth.scopeDepth();
        else
            tableOfScope = bytecode.m_symbolTableOrScopeDepth.symbolTable();
    }
    switch (type) {
    case ResolvedClosureVar:
    case ResolvedLazyClosureVar:
        // A store says which scope it is to: the constant that has the symbol table, which is what a scope goes by here.
        if (tableOfScope.isValid() && tableOfScope.isConstant()) {
            if (JSValue table = m_codeBlock->getConstant(tableOfScope); table && table.isCell())
                return { table.asCell(), offset };
        }
        return { identityOfScope(node->use(scope)), offset };
    case Dynamic:
        return { };
    default:
        break;
    }
    if (auto variable = resolveStatically(identifier, localScopeDepth, type); variable.kind == StaticVariable::Import)
        return { variable.import.scope, variable.import.scopeOffset };
    // One that was left to be looked for when the code is linked. It is not one of the function's own.
    if (m_declaredNames) {
        auto resolution = m_declaredNames->resolve(m_codeBlock->identifier(identifier).impl());
        if (resolution.kind == DeclaredNamesLink::Resolution::Slot)
            return { resolution.scope, resolution.offset };
    }
    return { };
}

void Graph::noteWhatCannotBeToldOfVariables(VariableFacts& facts)
{
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            if (node->kind != NodeKind::Bytecode)
                continue;
            switch (node->opcode) {
            case op_put_to_scope: {
                if (variableAccessedBy(node))
                    break;
                // Is it in an environment record at all?
                auto bytecode = node->as<OpPutToScope>();
                UniquedStringImpl* name = m_codeBlock->identifier(bytecode.m_var).impl();
                ResolveType type = bytecode.m_getPutInfo.resolveType();
                bool isElsewhere = false;
                if (type != ResolvedClosureVar && type != ResolvedLazyClosureVar && type != Dynamic && m_declaredNames) {
                    auto kind = m_declaredNames->resolve(name).kind;
                    // (Assigning to an import throws.)
                    isElsewhere = kind == DeclaredNamesLink::Resolution::Global || kind == DeclaredNamesLink::Resolution::Stable;
                }
                if (!isElsewhere)
                    facts.giveUpOnName(name);
                break;
            }
            case op_create_scoped_arguments:
                // An object by way of which the parameters that are in the record can be written.
                if (const void* scope = identityOfScope(node->use(node->as<OpCreateScopedArguments>().m_scope)))
                    facts.giveUpOnScope(scope);
                else {
                    for (auto& identifier : m_codeBlock->identifiers())
                        facts.giveUpOnName(identifier.impl());
                }
                break;
            case op_call_direct_eval:
                // Code that nobody has seen, which can write whatever is in sight of it.
                if (m_declaredNames)
                    m_declaredNames->forEachScope([&](const void* scope) { facts.giveUpOnScope(scope); });
                for (BasicBlock* other : m_rpo) {
                    for (Node* made : other->nodes) {
                        if (made->isBytecode(op_create_lexical_environment) || made->isBytecode(op_create_generator_frame_environment)) {
                            if (const void* scope = identityOfScope(made))
                                facts.giveUpOnScope(scope);
                        }
                    }
                }
                for (auto& identifier : m_codeBlock->identifiers())
                    facts.giveUpOnName(identifier.impl());
                break;
            default:
                break;
            }
        }
    }
}

void Graph::noteUsesOfProvenFunctions(const FactsOfExecutables& factsOfExecutables)
{
    auto executableMadeBy = [&](Node* node) -> UnlinkedFunctionExecutable* {
        if (node->kind != NodeKind::Bytecode)
            return nullptr;
        switch (node->opcode) {
        case op_new_func:
            return m_codeBlock->functionDecl(node->as<OpNewFunc>().m_functionDecl);
        case op_new_func_exp:
            return m_codeBlock->functionExpr(node->as<OpNewFuncExp>().m_functionDecl);
        default:
            return nullptr;
        }
    };
    auto valueIsUsed = [&](ProgramFacts* facts, ProgramFacts::WhyValueIsUsed why, Node* user) {
        facts->valueIsUsed.store(true, std::memory_order_relaxed);
        uint32_t nothing = 0;
        facts->whyValueIsUsed.compare_exchange_strong(nothing, why | (user->kind == NodeKind::Bytecode ? static_cast<uint32_t>(user->opcode) : 1000 + static_cast<uint32_t>(user->kind)) << 8, std::memory_order_relaxed);
    };
    auto note = [&](Node* user, const Use& use) {
        // Where it is made, it is on its way to the variable. Anywhere else it goes, it has got out before it got there.
        if (auto* executable = executableMadeBy(use.node)) {
            if (auto* facts = factsOfExecutables.get(executable); facts && !(user->isBytecode(op_put_to_scope) && use.reg == user->as<OpPutToScope>().m_value))
                valueIsUsed(facts, ProgramFacts::WhereItIsMade, user);
            return;
        }
        if (!use.node->isBytecode(op_get_from_scope))
            return;
        bool readIsProven = false;
        const KnownFunction* known = knownFunctionReadBy(use.node, &readIsProven);
        if (!known || !known->facts)
            return;
        // (`f?.()` asks whether there is anything to call.)
        if (user->isBytecode(op_check_tdz) || user->isBytecode(op_jundefined_or_null) || user->isBytecode(op_jnundefined_or_null))
            return;
        bool isCallee = false;
        if (user->isBytecode(op_call))
            isCallee = use.reg == user->as<OpCall>().m_callee;
        else if (user->isBytecode(op_call_ignore_result))
            isCallee = use.reg == user->as<OpCallIgnoreResult>().m_callee;
        else if (user->isBytecode(op_tail_call))
            isCallee = use.reg == user->as<OpTailCall>().m_callee;
        // (What may be a read of some other variable of that name is no proof of anything, either way.)
        if (isCallee && readIsProven && known->forCall && knownCallee(user) == known && calleeIsProven(user)) {
            known->facts->directCalls.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        valueIsUsed(known->facts, !isCallee ? ProgramFacts::Operand : !readIsProven ? ProgramFacts::CalleeButReadIsNotProven : ProgramFacts::CalleeButCallIsNotProven, user);
    };
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            for (auto& use : node->uses)
                note(node, use);
        }
        for (Node* phi : block->phis) {
            for (auto& use : phi->uses)
                note(phi, use);
        }
    }
}

Node* Graph::listOfArgumentsOf(const Node* node)
{
    if (node->kind != NodeKind::Bytecode)
        return nullptr;
    auto ofAll = [&](auto bytecode) -> Node* {
        return bytecode.m_firstVarArg || !bytecode.m_arguments.isValid() ? nullptr : node->use(bytecode.m_arguments);
    };
    switch (node->opcode) {
    case op_call_varargs:
        return ofAll(node->as<OpCallVarargs>());
    case op_tail_call_varargs:
        return ofAll(node->as<OpTailCallVarargs>());
    case op_construct_varargs:
        return ofAll(node->as<OpConstructVarargs>());
    case op_super_construct_varargs:
        return ofAll(node->as<OpSuperConstructVarargs>());
    default:
        return nullptr;
    }
}

void Graph::findListsOfArguments()
{
    if (!usesStubs || !Options::aotCallsWithLists())
        return;

    UncheckedKeyHashMap<Node*, unsigned> numberOfUses;
    bool writesToItsArguments = false;
    auto note = [&](Node* user) {
        // What a handler is to find a parameter to be is put where the parameter was passed.
        if (user->kind == NodeKind::SetStack && user->reg.isArgument())
            writesToItsArguments = true;
        for (auto& use : user->uses) {
            if (use.node->isBytecode(op_spread) || use.node->isBytecode(op_new_array_with_spread) || use.node->isBytecode(op_create_rest) || use.node->isBytecode(op_create_cloned_arguments))
                ++numberOfUses.add(use.node, 0).iterator->value;
        }
    };
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes)
            note(node);
        for (Node* phi : block->phis)
            note(phi);
    }
    if (numberOfUses.isEmpty())
        return;

    // Of an array of the rest of the arguments or an arguments object: by calls that do not need there to be one.
    UncheckedKeyHashMap<Node*, unsigned> numberOfUsesThatPassItOn;
    for (BasicBlock* block : m_rpo) {
        for (unsigned index = 0; index < block->nodes.size(); ++index) {
            Node* list = listOfArgumentsOf(block->nodes[index]);
            if (!list)
                continue;
            if (list->isBytecode(op_create_cloned_arguments)) {
                ++numberOfUsesThatPassItOn.add(list, 0).iterator->value;
                continue;
            }
            // The last first. Nothing is between them and the call, so nobody can tell when they are done.
            Vector<Node*, 4> parts;
            if (list->isBytecode(op_spread))
                parts.append(list);
            else if (list->isBytecode(op_new_array_with_spread)) {
                auto bytecode = list->as<OpNewArrayWithSpread>();
                if (bytecode.m_argc > mostItemsInList)
                    continue;
                parts.append(list);
                for (unsigned i = bytecode.m_argc; i--;) {
                    Node* element = list->use(VirtualRegister(bytecode.m_argv.offset() - static_cast<int>(i)));
                    if (element->isBytecode(op_spread))
                        parts.append(element);
                }
            } else
                continue;
            bool areRightBeforeTheCall = parts.size() <= index;
            for (unsigned i = 0; areRightBeforeTheCall && i < parts.size(); ++i)
                areRightBeforeTheCall = block->nodes[index - 1 - i] == parts[i] && numberOfUses.get(parts[i]) == 1;
            if (!areRightBeforeTheCall)
                continue;
            for (Node* part : parts) {
                part->isElided = true;
                if (!part->isBytecode(op_spread))
                    continue;
                Node* spread = part->use(part->as<OpSpread>().m_argument);
                if (spread->isBytecode(op_create_rest))
                    ++numberOfUsesThatPassItOn.add(spread, 0).iterator->value;
            }
        }
    }

    for (auto& [node, uses] : numberOfUsesThatPassItOn) {
        // Whoever else uses it finds out first whether it has been made. An array is likely to be used where that would tell.
        if (node->isBytecode(op_create_rest))
            node->isMadeWhenWanted = uses == numberOfUses.get(node) && node->as<OpCreateRest>().m_numParametersToSkip <= listSkipMask;
        else
            node->isMadeWhenWanted = !writesToItsArguments;
    }
}

void Graph::noteSelectorOfSite(unsigned slot, UniquedStringImpl* name)
{
    size_t index = selectors.find(name);
    if (index == notFound) {
        index = selectors.size();
        selectors.append(name);
    }
    while (siteConstants.size() <= slot)
        siteConstants.append(0);
    siteConstants[slot] = index + 1;
}

void Graph::noteShapeOfSite(unsigned slot, KnownShape&& shape)
{
    while (siteConstants.size() <= slot)
        siteConstants.append(0);
    shapes.append(WTF::move(shape));
    siteConstants[slot] = shapes.size() | CompiledFunctionInfo::siteConstantIsShape;
}

void Graph::notePlanOfSite(unsigned firstSlot, Vector<uint32_t, 16>&& words)
{
    while (siteConstants.size() <= firstSlot + 1)
        siteConstants.append(0);
    siteConstants[firstSlot + 1] = (plans.size() + 1) | CompiledFunctionInfo::siteConstantIsPlan;
    plans.appendVector(words);
}

std::optional<KnownShape> Graph::shapeOfLiteral(const Node* node) const
{
    unsigned count = node->numberOfLiteralProperties;
    if (count < 2 || count > KnownShape::maxProperties)
        return std::nullopt;
    KnownShape shape;
    shape.inlineCapacity = KnownShape::inlineCapacityFor(count);
    // As operationAOTNewObjectLiteral() finds them.
    auto& instructions = m_codeBlock->instructions();
    VirtualRegister object = node->as<OpNewObject>().m_dst;
    for (unsigned offset = node->bytecodeIndex.offset() + node->instruction->size(); shape.names.size() < count; offset += instructions.at(offset)->size()) {
        auto next = instructions.at(offset);
        if (next->opcodeID() != op_put_by_id)
            continue;
        auto bytecode = next->as<OpPutById>();
        if (bytecode.m_base != object)
            continue;
        UniquedStringImpl* name = m_codeBlock->identifier(bytecode.m_property).impl();
        if (shape.names.contains(name) || name->isSymbol())
            return std::nullopt;
        shape.names.append(name);
    }
    return shape;
}

unsigned Graph::indexOfKnownCallee(const ImageKey& key)
{
    for (unsigned i = 0; i < knownCallees.size(); ++i) {
        if (knownCallees[i].sameFunction(key))
            return i;
    }
    knownCallees.append(key);
    return knownCallees.size() - 1;
}

Graph::StaticVariable Graph::resolveStatically(unsigned identifierIndex, unsigned localScopeDepth, ResolveType type)
{
    StaticVariable result;
    if (type == Dynamic || needsVarInjectionChecks(type))
        return result;
    UniquedStringImpl* uid = m_codeBlock->identifier(identifierIndex).impl();
    unsigned depth = localScopeDepth;
    for (const ScopeChainEntry& entry : m_scopeChain) {
        switch (entry.kind) {
        case ScopeChainEntry::Lexical: {
            SymbolTable* symbolTable = entry.symbolTable;
            ConcurrentJSLocker locker(symbolTable->m_lock);
            auto iter = symbolTable->find(locker, uid);
            if (iter != symbolTable->end(locker)) {
                result.kind = StaticVariable::Closure;
                result.depth = depth;
                result.offset = iter->value.scopeOffset();
                result.inModule = entry.isModule;
                result.isReadOnly = iter->value.isReadOnly();
                return result;
            }
            if (symbolTable->usesSloppyEval())
                return result;
            if (entry.isModule) {
                // An import, or a global: which, and where, is for when the modules have been linked.
                result.kind = StaticVariable::Unresolved;
                return result;
            }
            break;
        }
        case ScopeChainEntry::Unknown:
            if (m_declaredNames && type == GlobalProperty) {
                auto resolution = m_declaredNames->resolve(uid);
                if (resolution.kind == DeclaredNamesLink::Resolution::Slot && Options::aotResolvesScopesItself()) {
                    // (It is none of the function's own: whatever made the bytecode would have said so.)
                    result.kind = StaticVariable::Closure;
                    result.depth = depth + resolution.hops;
                    result.offset = ScopeOffset(resolution.offset);
                    result.inModule = resolution.isLazyFunctionSlot;
                    result.isReadOnly = resolution.isReadOnly;
                    result.isInOutermostEnvironment = resolution.isInOutermostEnvironment;
                    return result;
                }
                if (resolution.kind == DeclaredNamesLink::Resolution::Global) {
                    result.kind = StaticVariable::Unresolved;
                    result.isInGlobalScopes = true;
                    result.isGlobal = true;
                    return result;
                }
                if (resolution.kind == DeclaredNamesLink::Resolution::Stable) {
                    result.kind = StaticVariable::ModuleImport;
                    result.depth = depth + resolution.hops;
                    // Assigning to an import is an error that the environment of the module that imports it knows to throw.
                    const StaticImport* import = m_linkage && !m_namesAssignedTo.get(identifierIndex) ? m_linkage->findImport(uid) : nullptr;
                    if (import) {
                        result.kind = StaticVariable::Import;
                        result.import = *import;
                        result.offset = ScopeOffset(import->scopeOffset);
                        usesStaticImports = true;
                    }
                    return result;
                }
            }
            result.kind = StaticVariable::Unresolved;
            return result;
        case ScopeChainEntry::GlobalLexical:
        case ScopeChainEntry::Global:
            // The operations that fill the caches look at what the name turned out to be, and only cache what stays put.
            result.kind = StaticVariable::Unresolved;
            result.isGlobal = true;
            return result;
        case ScopeChainEntry::Opaque:
            return result;
        }
        ++depth;
    }
    return result;
}

unsigned Graph::extraOfResolveScope(const OpResolveScope& bytecode)
{
    if (resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType).isInGlobalScopes)
        return Site::resolvesInGlobalScopes;
    // Too deep to be told from that is too deep to fit.
    return bytecode.m_localScopeDepth < Site::resolvesInGlobalScopes ? bytecode.m_localScopeDepth : std::numeric_limits<unsigned>::max();
}

unsigned Graph::extraOfGetFromScope(const OpGetFromScope& bytecode)
{
    unsigned result = bytecode.m_getPutInfo.resolveMode() == ThrowIfNotFound ? Site::throwsIfNotFound : 0;
    if (resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_getPutInfo.resolveType()).kind == StaticVariable::ModuleImport)
        result |= Site::isImport;
    return result;
}

void Graph::setLinkage(const ModuleLinkage* linkage, const DeclaredNamesLink* declaredNames)
{
    m_linkage = linkage;
    m_declaredNames = declaredNames;
    if (!linkage)
        return;
    m_scopeIsEnvironmentOfModule = linkage->distanceOfEnvironment && m_codeBlock->codeType() == FunctionCode && declaredNames && declaredNames->scopeIsOutermostEnvironment();
    m_needsFunctionObject = !m_scopeIsEnvironmentOfModule || AOT::needsFunctionObject(m_codeBlock);
    for (const auto& instruction : m_codeBlock->instructions()) {
        if (instruction->opcodeID() != op_put_to_scope)
            continue;
        auto bytecode = instruction->as<OpPutToScope>();
        if (bytecode.m_getPutInfo.resolveType() != ResolvedClosureVar)
            m_namesAssignedTo.set(bytecode.m_var);
    }
}

void Graph::fail(ASCIILiteral reason, OpcodeID opcode)
{
    if (failed())
        return;
    m_failureReason = reason;
    m_failureOpcode = opcode;
}

void Node::dump(PrintStream& out) const
{
    out.print("@", index, "<");
    dumpType(out, type);
    if (range.isKnown())
        out.print(" ", range.min, "..", range.max);
    out.print("> = ");
    switch (kind) {
    case NodeKind::Bytecode:
        out.print(opcode, " bc#", bytecodeIndex.offset());
        break;
    case NodeKind::Constant:
        out.print("Constant(", constant, ")");
        break;
    case NodeKind::ConstantCell:
        out.print("ConstantCell(", reg, ")");
        break;
    case NodeKind::Intrinsic:
        out.print("Intrinsic(", intrinsic, " ", ImmutableIntrinsics::shared()->at(ImmutableIntrinsics::shared()->at(intrinsic).holder).name, ".", ImmutableIntrinsics::shared()->at(intrinsic).name, ")");
        break;
    case NodeKind::Argument:
        out.print("Argument(", reg, ")");
        break;
    case NodeKind::Phi:
        out.print("Phi(", reg, ")");
        break;
    case NodeKind::Proj:
        out.print("Proj(", reg, ")");
        break;
    case NodeKind::GetStack:
        out.print("GetStack(", reg, ")");
        break;
    case NodeKind::SetStack:
        out.print("SetStack(", reg, ")");
        break;
    case NodeKind::Narrow:
        out.print("Narrow(", reg, ")");
        break;
    case NodeKind::Guard:
        switch (guardKind) {
        case GuardKind::Whole:
            out.print("Guard(", opcode, " bc#", bytecodeIndex.offset(), structureIsChecked ? ", structure checked" : "", slotIsPlain ? ", plain" : "", calleeIsChecked ? ", callee checked" : "", ")");
            break;
        case GuardKind::Nothing:
            out.print("Guard()");
            break;
        case GuardKind::Reentry:
            out.print("GuardReentry");
            break;
        case GuardKind::Structure:
            out.print("GuardStructure(@", site->index, ")");
            break;
        case GuardKind::SlotsAgree:
            out.print("GuardSlotsAgree(@", site->index, ", @", otherSite->index, ")");
            break;
        case GuardKind::SlotIsPlain:
            out.print("GuardSlotIsPlain(@", site->index, ")");
            break;
        case GuardKind::BeginSlotChecks:
            out.print("BeginSlotChecks");
            break;
        case GuardKind::EndSlotChecks:
            out.print("EndSlotChecks");
            break;
        case GuardKind::Callee:
            out.print("GuardCallee(bc#", bytecodeIndex.offset(), ")");
            break;
        case GuardKind::KnownCallee:
            out.print("GuardKnownCallee(bc#", bytecodeIndex.offset(), ")");
            break;
        case GuardKind::TypedArrayStorage:
            out.print("GuardTypedArrayStorage");
            break;
        }
        break;
    }
    if (!uses.isEmpty()) {
        out.print(" [");
        CommaPrinter comma;
        for (auto& use : uses) {
            out.print(comma);
            if (use.reg.isValid())
                out.print(use.reg, ":");
            out.print("@", use.node ? use.node->index : UINT_MAX);
        }
        out.print("]");
    }
}

void Graph::dump(PrintStream& out) const
{
    for (BasicBlock* block : m_rpo) {
        out.print("Block #", block->index, " bc#[", block->bytecodeBegin, ", ", block->bytecodeEnd, ")");
        if (block->isCatchEntrypoint)
            out.print(" catch");
        if (block->isLoopHeader)
            out.print(" loop");
        if (block->isGeneric)
            out.print(" generic");
        if (block->isPreHeader)
            out.print(" pre-header");
        if (block->isReentry)
            out.print(" re-entry");
        out.print(" preds:");
        for (auto* predecessor : block->predecessors)
            out.print(" #", predecessor->index);
        out.print("\n");
        for (Node* phi : block->phis)
            out.print("    ", *phi, "\n");
        for (Node* node : block->nodes)
            out.print("    ", *node, "\n");
        out.print("  succs:");
        for (auto* successor : block->successors)
            out.print(" #", successor->index);
        out.print("\n");
    }
}

namespace {

class Parser {
public:
    Parser(Graph& graph)
        : m_graph(graph)
        , m_codeBlock(graph.codeBlock())
        , m_instructions(m_codeBlock->instructions())
    {
    }

    bool run()
    {
        if (!findBlocks())
            return false;
        computeReversePostOrder();
        if (chooseGuards()) {
            m_graph.blocks.clear();
            m_graph.m_rpo.clear();
            m_graph.catchEntrypoints.clear();
            if (!findBlocks())
                return false;
            computeReversePostOrder();
        }
        computeLiveness();
        chooseHomedRegisters();
        for (BasicBlock* block : m_graph.m_rpo) {
            parseBlock(block);
            if (m_graph.failed())
                return false;
        }
        if (m_needsEveryStore)
            makeSkippedStores();
        fillPhis();
        simplifyPhis();
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (node->kind != NodeKind::Bytecode)
                    continue;
                switch (node->opcode) {
                case op_call:
                case op_call_ignore_result:
                case op_construct:
                    if (auto* known = m_graph.knownCallee(node); known && (known->forCall == m_codeBlock || known->forConstruct == m_codeBlock))
                        m_graph.callsItself = true;
                    [[fallthrough]];
                case op_tail_call:
                case op_call_direct_eval:
                case op_call_varargs:
                case op_tail_call_varargs:
                case op_construct_varargs:
                case op_super_construct:
                case op_super_construct_varargs:
                case op_iterator_open:
                case op_iterator_next:
                case op_async_iterator_open:
                case op_async_iterator_next:
                case op_iterator_close_check:
                case op_instanceof:
                    m_graph.makesCalls = true;
                    break;
                default:
                    break;
                }
            }
        }
        return !m_graph.failed();
    }

private:
    template<typename Functor>
    void forEachUse(const JSInstruction* instruction, const Functor& functor)
    {
        // What the instruction as a whole reads: a register that one of its checkpoints wrote first is its own business.
        Vector<VirtualRegister, 4> defined;
        Vector<VirtualRegister, 8> seen;
        unsigned checkpoints = instruction->numberOfCheckpoints();
        for (unsigned checkpoint = 0; checkpoint < checkpoints; ++checkpoint) {
            computeUsesForBytecodeIndexImpl(instruction, checkpoint, [&](VirtualRegister reg) {
                if (defined.contains(reg) || seen.contains(reg))
                    return;
                seen.append(reg);
                functor(reg);
            });
            if (checkpoints > 1) {
                computeDefsForBytecodeIndexImpl(m_codeBlock->numVars(), instruction, checkpoint, [&](VirtualRegister reg) {
                    defined.append(reg);
                });
            }
        }
    }

    template<typename Functor>
    void forEachDef(const JSInstruction* instruction, const Functor& functor)
    {
        Vector<VirtualRegister, 4> seen;
        unsigned checkpoints = instruction->numberOfCheckpoints();
        for (unsigned checkpoint = 0; checkpoint < checkpoints; ++checkpoint) {
            computeDefsForBytecodeIndexImpl(m_codeBlock->numVars(), instruction, checkpoint, [&](VirtualRegister reg) {
                if (seen.contains(reg))
                    return;
                seen.append(reg);
                functor(reg);
            });
        }
    }

    std::pair<Node*, VirtualRegister> intrinsicReadBy(BasicBlock* block, const JSInstruction* instruction)
    {
        if (!Options::useImmutableIntrinsics())
            return { };
        switch (instruction->opcodeID()) {
        case op_resolve_scope:
            return { m_graph.intrinsicReadBy(instruction, nullptr), instruction->as<OpResolveScope>().m_dst };
        case op_get_from_scope:
            return { m_graph.intrinsicReadBy(instruction, nullptr), instruction->as<OpGetFromScope>().m_dst };
        case op_get_by_id: {
            auto bytecode = instruction->as<OpGetById>();
            return { m_graph.intrinsicReadBy(instruction, get(block, bytecode.m_base)), bytecode.m_dst };
        }
        default:
            return { };
        }
    }

    // What there is a short way of doing that is worth leaving the fast copy of a loop for when it does not apply.
    bool canBeGuarded(const JSInstruction* instruction)
    {
        switch (instruction->opcodeID()) {
        case op_resolve_scope:
            return !isStaticClosureVarResolveType(instruction->as<OpResolveScope>().m_resolveType);
        case op_get_from_scope:
            return instruction->as<OpGetFromScope>().m_getPutInfo.resolveType() != ResolvedClosureVar;
        case op_call:
        case op_call_ignore_result: {
            auto operands = Graph::operandsOfCall(instruction);
            for (auto [reg, identifier] : m_recentProperties) {
                if (reg == operands.callee)
                    return callIntrinsicFor(m_codeBlock->identifier(identifier).impl(), operands.argc) != CallIntrinsic::None;
            }
            if (instruction->opcodeID() != op_call)
                return false;
            for (auto& [reg, known] : m_recentFunctions) {
                if (reg == operands.callee)
                    return canBeInlined(known, operands.argc);
            }
            return false;
        }
        case op_get_by_id:
        case op_put_by_id:
        case op_get_by_val:
        case op_put_by_val:
        case op_get_length:
        case op_check_traps:
            return true;
        case op_check_type:
            return !isFact(instruction->as<OpCheckType>().m_mask);
        default:
            return false;
        }
    }

    // A function that does nothing but work something out from its arguments, in a straight line, once it has checked that they are
    // what it takes them for. In the fast copy of a loop a call of one is replaced by what it works out: if the callee turns out to
    // be another function, or a check fails, nothing has happened yet, and the generic copy makes the call.
    //
    // Every operation has to be one that the types make simple. The lowering of anything else calls the runtime, on behalf of an
    // instruction that is not the caller's.
    bool canBeInlined(const KnownFunction* known, unsigned argumentCountIncludingThis)
    {
        UnlinkedFunctionCodeBlock* callee = known->forCall;
        if (!callee || argumentCountIncludingThis < callee->numParameters())
            return false;
        return m_canBeInlined.ensure(callee, [&] {
            return computeCanBeInlined(callee);
        }).iterator->value;
    }

    static bool computeCanBeInlined(UnlinkedFunctionCodeBlock* callee)
    {
        if (callee->instructionsSize() > 256 || callee->numberOfExceptionHandlers() || callee->isConstructor())
            return false;
        unsigned numParameters = callee->numParameters();
        Vector<Type, 16> types;
        types.fill(TTop, numParameters + callee->numCalleeLocals());
        auto slotOf = [&](VirtualRegister reg) -> Type* {
            if (reg.isArgument() && static_cast<unsigned>(reg.toArgument()) < numParameters)
                return &types[reg.toArgument()];
            if (reg.isLocal() && static_cast<unsigned>(reg.toLocal()) < callee->numCalleeLocals())
                return &types[numParameters + reg.toLocal()];
            return nullptr;
        };
        bool ok = true;
        auto typeOf = [&](VirtualRegister reg) -> Type {
            if (reg.isConstant()) {
                if (callee->constantSourceCodeRepresentation(reg) == SourceCodeRepresentation::LinkTimeConstant) {
                    ok = false;
                    return TTop;
                }
                JSValue value = callee->getConstant(reg);
                if (!value || value.isCell())
                    ok = false;
                return typeOfValue(value);
            }
            if (Type* slot = slotOf(reg))
                return *slot;
            ok = false;
            return TTop;
        };
        auto define = [&](VirtualRegister reg, Type type) {
            if (Type* slot = slotOf(reg))
                *slot = type;
            else
                ok = false;
        };
        auto numbers = [&](VirtualRegister lhs, VirtualRegister rhs) { return isSubtype(typeOf(lhs) | typeOf(rhs), TNumber); };

        bool returned = false;
        for (const auto& instruction : callee->instructions()) {
            if (returned || !ok)
                return false;
            switch (instruction->opcodeID()) {
            case op_enter:
                break;
            case op_mov: {
                auto bytecode = instruction->as<OpMov>();
                define(bytecode.m_dst, typeOf(bytecode.m_src));
                break;
            }
            case op_check_type: {
                auto bytecode = instruction->as<OpCheckType>();
                if (isFact(bytecode.m_mask))
                    break;
                Type type = typeOf(bytecode.m_value) & typeAdmittedByMask(bytecode.m_mask);
                if (!bytecode.m_value.isConstant())
                    define(bytecode.m_value, type);
                break;
            }
#define AOT_ARITHMETIC(Struct, opcodeName, result) \
            case opcodeName: { \
                auto bytecode = instruction->as<Struct>(); \
                if (!numbers(bytecode.m_lhs, bytecode.m_rhs)) \
                    return false; \
                define(bytecode.m_dst, result); \
                break; \
            }
            AOT_ARITHMETIC(OpAdd, op_add, TNumber)
            AOT_ARITHMETIC(OpSub, op_sub, TNumber)
            AOT_ARITHMETIC(OpMul, op_mul, TNumber)
            AOT_ARITHMETIC(OpDiv, op_div, TNumber)
            AOT_ARITHMETIC(OpBitand, op_bitand, TInt32)
            AOT_ARITHMETIC(OpBitor, op_bitor, TInt32)
            AOT_ARITHMETIC(OpBitxor, op_bitxor, TInt32)
            AOT_ARITHMETIC(OpLshift, op_lshift, TInt32)
            AOT_ARITHMETIC(OpRshift, op_rshift, TInt32)
            AOT_ARITHMETIC(OpLess, op_less, TBoolean)
            AOT_ARITHMETIC(OpLesseq, op_lesseq, TBoolean)
            AOT_ARITHMETIC(OpGreater, op_greater, TBoolean)
            AOT_ARITHMETIC(OpGreatereq, op_greatereq, TBoolean)
            AOT_ARITHMETIC(OpStricteq, op_stricteq, TBoolean)
            AOT_ARITHMETIC(OpNstricteq, op_nstricteq, TBoolean)
#undef AOT_ARITHMETIC
            case op_negate: {
                auto bytecode = instruction->as<OpNegate>();
                if (!isSubtype(typeOf(bytecode.m_operand), TNumber))
                    return false;
                define(bytecode.m_dst, TNumber);
                break;
            }
            case op_inc: {
                auto bytecode = instruction->as<OpInc>();
                if (!isSubtype(typeOf(bytecode.m_srcDst), TNumber))
                    return false;
                break;
            }
            case op_dec: {
                auto bytecode = instruction->as<OpDec>();
                if (!isSubtype(typeOf(bytecode.m_srcDst), TNumber))
                    return false;
                break;
            }
            case op_ret:
                typeOf(instruction->as<OpRet>().m_value);
                returned = true;
                break;
            default:
                return false;
            }
        }
        return returned && ok;
    }

    // What the call comes to, its guards and all, at the end of the block. Null if the call is not one to do this to after all.
    Node* inlineCall(BasicBlock* block, const JSInstruction* call, unsigned offset)
    {
        if (call->opcodeID() != op_call)
            return nullptr;
        auto bytecode = call->as<OpCall>();
        Node* guard = m_graph.addNode(NodeKind::Guard);
        guard->guardKind = GuardKind::KnownCallee;
        guard->opcode = op_call;
        guard->instruction = call;
        guard->bytecodeIndex = BytecodeIndex(offset);
        guard->uses.append({ bytecode.m_callee, get(block, bytecode.m_callee) });
        const KnownFunction* known = m_graph.knownCallee(guard);
        if (!known || !canBeInlined(known, bytecode.m_argc))
            return nullptr;
        append(block, guard);

        UnlinkedFunctionCodeBlock* callee = known->forCall;
        unsigned numParameters = callee->numParameters();
        Vector<Node*, 16> values;
        values.fill(m_graph.constant(jsUndefined()), numParameters + callee->numCalleeLocals());
        int firstArgument = -static_cast<int>(bytecode.m_argv) + CallFrame::thisArgumentOffset();
        for (unsigned i = 0; i < numParameters; ++i)
            values[i] = get(block, VirtualRegister(firstArgument + i));
        auto slotOf = [&](VirtualRegister reg) -> Node*& {
            if (reg.isArgument())
                return values[reg.toArgument()];
            return values[numParameters + reg.toLocal()];
        };
        auto valueOf = [&](VirtualRegister reg) -> Node* {
            if (reg.isConstant())
                return m_graph.constant(callee->getConstant(reg));
            return slotOf(reg);
        };

        for (const auto& instruction : callee->instructions()) {
            OpcodeID opcode = instruction->opcodeID();
            switch (opcode) {
            case op_enter:
                continue;
            case op_mov: {
                auto mov = instruction->as<OpMov>();
                slotOf(mov.m_dst) = valueOf(mov.m_src);
                continue;
            }
            case op_ret:
                return valueOf(instruction->as<OpRet>().m_value);
            case op_check_type:
                if (isFact(instruction->as<OpCheckType>().m_mask))
                    continue;
                break;
            default:
                break;
            }
            auto make = [&](NodeKind kind) {
                Node* node = m_graph.addNode(kind);
                node->opcode = opcode;
                node->instruction = instruction.ptr();
                node->bytecodeIndex = BytecodeIndex(offset);
                computeUsesForBytecodeIndexImpl(instruction.ptr(), noCheckpoints, [&](VirtualRegister reg) {
                    for (auto& use : node->uses) {
                        if (use.reg == reg)
                            return;
                    }
                    node->uses.append({ reg, valueOf(reg) });
                });
                return append(block, node);
            };
            if (opcode == op_check_type) {
                Node* check = make(NodeKind::Guard);
                Node* node = make(NodeKind::Bytecode);
                node->guard = check;
                check->guarded = node;
                VirtualRegister reg = instruction->as<OpCheckType>().m_value;
                if (!reg.isConstant())
                    slotOf(reg) = node;
                continue;
            }
            Node* node = make(NodeKind::Bytecode);
            computeDefsForBytecodeIndexImpl(callee->numVars(), instruction.ptr(), noCheckpoints, [&](VirtualRegister reg) {
                slotOf(reg) = node;
            });
        }
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }

    // { a: x, b: y }: an op_new_object and, right after it, an op_put_by_id for each property (Options::evaluateObjectLiteralValuesFirst()).
    // How many of those there are.
    // What the fast copy of a loop does better than code that has caches to go by: numbers that are not boxed, elements got at by
    // their index, a call that is not made. A loop that has none of it gains nothing by there being two of it.
    static bool isBetterInFastCopy(OpcodeID opcode)
    {
        switch (opcode) {
        case op_add:
        case op_sub:
        case op_mul:
        case op_div:
        case op_mod:
        case op_pow:
        case op_inc:
        case op_dec:
        case op_negate:
        case op_bitand:
        case op_bitor:
        case op_bitxor:
        case op_bitnot:
        case op_lshift:
        case op_rshift:
        case op_urshift:
        case op_less:
        case op_lesseq:
        case op_greater:
        case op_greatereq:
        case op_jless:
        case op_jlesseq:
        case op_jgreater:
        case op_jgreatereq:
        case op_jnless:
        case op_jnlesseq:
        case op_jngreater:
        case op_jngreatereq:
        case op_get_by_val:
        case op_put_by_val:
            return true;
        default:
            return false;
        }
    }

    static bool isCallOrTheLike(OpcodeID opcode)
    {
        switch (opcode) {
        case op_call:
        case op_call_ignore_result:
        case op_tail_call:
        case op_construct:
        case op_call_varargs:
        case op_tail_call_varargs:
        case op_construct_varargs:
        case op_super_construct:
        case op_super_construct_varargs:
        case op_call_direct_eval:
        case op_iterator_open:
        case op_iterator_next:
            return true;
        default:
            return false;
        }
    }

    // With the blocks and the loops known: where the guards go. False if nowhere.
    bool chooseGuards()
    {
        if (!Options::aotSplitLoops() || !Options::aotLoopsToSplit() || !usesStubs)
            return false;
        unsigned size = m_instructions.size();
        struct OfBlock {
            Vector<unsigned, 8> guards;
            bool hasWhatIsBetterInFastCopy { false };
            bool hasCallThatIsMade { false };
            // A call that the fast copy does not make, or an element got at by its index: each time round, that is a call less.
            bool hasWhatIsMuchBetterInFastCopy { false };
        };
        Vector<OfBlock> ofBlocks(m_graph.blocks.size());
        for (BasicBlock* block : m_graph.m_rpo) {
            if (!block->isInLoop)
                continue;
            OfBlock& ofBlock = ofBlocks[block->index];
            m_recentProperties.shrink(0);
            m_recentFunctions.shrink(0);
            BitVector partOfLiteral;
            for (unsigned offset = block->bytecodeBegin; offset < block->bytecodeEnd; offset += m_instructions.at(offset)->size()) {
                const JSInstruction* instruction = m_instructions.at(offset).ptr();
                if (partOfLiteral.get(offset))
                    continue;
                if (instruction->opcodeID() == op_new_object) {
                    for (unsigned store : Graph::storesOfLiteral(m_instructions, offset))
                        partOfLiteral.set(store);
                }
                if (instruction->opcodeID() == op_get_by_id) {
                    auto bytecode = instruction->as<OpGetById>();
                    m_recentProperties.removeAllMatching([&](auto& entry) { return entry.first == bytecode.m_dst; });
                    m_recentProperties.append({ bytecode.m_dst, bytecode.m_property });
                    m_recentFunctions.removeAllMatching([&](auto& entry) { return entry.first == bytecode.m_dst; });
                }
                if (instruction->opcodeID() == op_get_from_scope && m_graph.calleeHints()) {
                    auto bytecode = instruction->as<OpGetFromScope>();
                    m_recentFunctions.removeAllMatching([&](auto& entry) { return entry.first == bytecode.m_dst; });
                    m_recentProperties.removeAllMatching([&](auto& entry) { return entry.first == bytecode.m_dst; });
                    ResolveType type = bytecode.m_getPutInfo.resolveType();
                    if (type == ResolvedClosureVar || type == ResolvedLazyClosureVar) {
                        if (auto* known = m_graph.probablyFunctionInVariableOfModule(bytecode.m_var, bytecode.m_offset))
                            m_recentFunctions.append({ bytecode.m_dst, known });
                    } else if (type != Dynamic) {
                        const KnownFunction* known;
                        if (auto variable = m_graph.resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type); variable.kind == Graph::StaticVariable::Import)
                            known = variable.import.function;
                        else
                            known = m_graph.calleeHints()->find(m_codeBlock->identifier(bytecode.m_var).impl(), std::nullopt);
                        if (known)
                            m_recentFunctions.append({ bytecode.m_dst, known });
                    }
                }
                bool isGuarded = canBeGuarded(instruction);
                if (isGuarded)
                    ofBlock.guards.append(offset);
                OpcodeID opcode = instruction->opcodeID();
                if (isCallOrTheLike(opcode)) {
                    if (isGuarded) {
                        ofBlock.hasWhatIsBetterInFastCopy = true;
                        ofBlock.hasWhatIsMuchBetterInFastCopy = true;
                    } else
                        ofBlock.hasCallThatIsMade = true;
                } else if (isBetterInFastCopy(opcode)) {
                    ofBlock.hasWhatIsBetterInFastCopy = true;
                    if (opcode == op_get_by_val || opcode == op_put_by_val)
                        ofBlock.hasWhatIsMuchBetterInFastCopy = true;
                }
            }
        }

        // The body of a loop is whatever gets to a jump back to its header without going through the header. A loop inside one that
        // has two copies has two.
        UncheckedKeyHashMap<BasicBlock*, BitVector> bodies;
        for (auto [from, header] : m_backEdges) {
            BitVector& body = bodies.ensure(header, [&] {
                BitVector result(m_graph.blocks.size());
                result.set(header->index);
                return result;
            }).iterator->value;
            Vector<BasicBlock*> worklist { from };
            while (!worklist.isEmpty()) {
                BasicBlock* block = worklist.takeLast();
                if (body.get(block->index))
                    continue;
                body.set(block->index);
                for (BasicBlock* predecessor : block->predecessors)
                    worklist.append(predecessor);
            }
        }
        BitVector hasTwoCopies(m_graph.blocks.size());
        for (auto& [header, body] : bodies) {
            bool hasWhatIsBetter = false;
            bool hasWhatIsMuchBetter = false;
            bool hasCallThatIsMade = false;
            for (unsigned index : body) {
                hasWhatIsBetter |= ofBlocks[index].hasWhatIsBetterInFastCopy;
                hasWhatIsMuchBetter |= ofBlocks[index].hasWhatIsMuchBetterInFastCopy;
                hasCallThatIsMade |= ofBlocks[index].hasCallThatIsMade;
            }
            bool isWorthIt = true;
            switch (Options::aotLoopsToSplit()) {
            case 2:
                isWorthIt = hasWhatIsBetter;
                break;
            case 3:
                isWorthIt = !hasCallThatIsMade;
                break;
            case 4:
                isWorthIt = hasWhatIsBetter && !hasCallThatIsMade;
                break;
            case 5:
                isWorthIt = hasWhatIsMuchBetter || !hasCallThatIsMade;
                break;
            default:
                break;
            }
            if (isWorthIt)
                hasTwoCopies.merge(body);
        }

        bool found = false;
        for (BasicBlock* block : m_graph.m_rpo) {
            if (!block->isInLoop || !hasTwoCopies.get(block->index))
                continue;
            if (m_inLoop.isEmpty()) {
                m_inLoop.ensureSize(size + 1);
                m_guards.ensureSize(size + 1);
                m_loopHeaders.ensureSize(size + 1);
            }
            if (block->isLoopHeader)
                m_loopHeaders.set(block->bytecodeBegin);
            for (unsigned offset = block->bytecodeBegin; offset < block->bytecodeEnd; offset += m_instructions.at(offset)->size())
                m_inLoop.set(offset);
            for (unsigned offset : ofBlocks[block->index].guards) {
                m_guards.set(offset);
                found = true;
            }
        }
        m_hasGuards = found;
        if (found) {
            for (auto [from, header] : m_backEdges) {
                if (hasTwoCopies.get(header->index))
                    m_graph.jumpsBack.add(static_cast<uint64_t>(header->bytecodeBegin) << 32 | from->bytecodeEnd);
            }
        }
        return found;
    }

    bool findBlocks()
    {
        unsigned size = m_instructions.size();
        BitVector& leaders = m_leaders;
        leaders.clearAll();
        leaders.ensureSize(size + 1);
        leaders.set(0);
        for (const auto& instruction : m_instructions) {
            OpcodeID opcode = instruction->opcodeID();
            unsigned next = instruction.offset() + instruction->size();
            if (isBranch(opcode)) {
                extractStoredJumpTargetsForInstruction(m_codeBlock, instruction, [&](int32_t relativeOffset) {
                    leaders.set(instruction.offset() + relativeOffset);
                });
                leaders.set(next);
            } else if (isTerminal(opcode) || isThrow(opcode))
                leaders.set(next);
        }
        for (unsigned i = 0; i < m_codeBlock->numberOfExceptionHandlers(); ++i)
            leaders.set(m_codeBlock->exceptionHandler(i).target);

        // The root has no bytecode: it is where the arguments and the initial values of the locals are defined, so that
        // bytecode offset 0 can be a jump target like any other.
        m_graph.root = m_graph.addBlock();

        m_graph.blockForOffset.fill(nullptr, size);
        if (m_hasGuards)
            m_graph.headerForOffset.fill(nullptr, size);
        BasicBlock* current = nullptr;
        Vector<BasicBlock*, 4> reentries;
        for (const auto& instruction : m_instructions) {
            unsigned offset = instruction.offset();
            if (leaders.get(offset)) {
                if (current)
                    current->bytecodeEnd = offset;
                if (m_hasGuards && m_loopHeaders.get(offset)) {
                    // Each of the three is followed by what it goes on to.
                    BasicBlock* reentry = m_graph.addBlock();
                    reentry->bytecodeBegin = offset;
                    reentry->bytecodeEnd = offset;
                    reentry->endsWithGuard = true;
                    reentry->isReentry = true;
                    reentry->isGeneric = true;
                    reentries.append(reentry);
                }
                current = m_graph.addBlock();
                current->bytecodeBegin = offset;
                m_graph.blockForOffset[offset] = current;
                if (m_hasGuards && m_loopHeaders.get(offset)) {
                    current->bytecodeEnd = offset;
                    current->endsWithGuard = true;
                    current->isPreHeader = true;
                    current = m_graph.addBlock();
                    current->bytecodeBegin = offset;
                    m_graph.headerForOffset[offset] = current;
                }
            }
            if (m_hasGuards && m_guards.get(offset)) {
                // The block that comes right after is the one that goes on.
                current->bytecodeEnd = offset;
                current->endsWithGuard = true;
                current = m_graph.addBlock();
                current->bytecodeBegin = offset;
            }
        }
        if (!current) {
            m_graph.fail("no bytecode"_s);
            return false;
        }
        current->bytecodeEnd = size;

        Vector<BasicBlock*> genericBlockForOffset;
        if (m_hasGuards) {
            genericBlockForOffset.fill(nullptr, size);
            m_graph.genericTargetForOffset.fill(nullptr, size);
            current = nullptr;
            for (const auto& instruction : m_instructions) {
                unsigned offset = instruction.offset();
                bool inLoop = m_inLoop.get(offset);
                if (current && (!inLoop || leaders.get(offset) || m_guards.get(offset))) {
                    current->bytecodeEnd = offset;
                    current = nullptr;
                }
                if (!inLoop || current)
                    continue;
                current = m_graph.addBlock();
                current->isGeneric = true;
                current->bytecodeBegin = offset;
                genericBlockForOffset[offset] = current;
                if (!m_loopHeaders.get(offset))
                    m_graph.genericTargetForOffset[offset] = current;
            }
            if (current)
                current->bytecodeEnd = size;
            for (BasicBlock* reentry : reentries)
                m_graph.genericTargetForOffset[reentry->bytecodeBegin] = reentry;
        }

        auto link = [&](BasicBlock* from, BasicBlock* to) {
            from->successors.append(to);
            if (!to->predecessors.contains(from))
                to->predecessors.append(from);
        };

        link(m_graph.root, m_graph.blockForOffset[0]);
        for (auto& blockPtr : m_graph.blocks) {
            BasicBlock* block = blockPtr.get();
            if (block == m_graph.root)
                continue;
            if (block->endsWithGuard) {
                link(block, m_graph.blocks[block->index + 1].get());
                link(block, genericBlockForOffset[block->bytecodeEnd]);
                continue;
            }
            // Find the last instruction.
            unsigned lastOffset = block->bytecodeBegin;
            for (unsigned offset = block->bytecodeBegin; offset < block->bytecodeEnd; offset += m_instructions.at(offset)->size())
                lastOffset = offset;
            auto instruction = m_instructions.at(lastOffset);
            OpcodeID opcode = instruction->opcodeID();
            if (isBranch(opcode)) {
                extractStoredJumpTargetsForInstruction(m_codeBlock, instruction, [&](int32_t relativeOffset) {
                    link(block, m_graph.targetFrom(block, lastOffset + relativeOffset));
                });
                if (!isUnconditionalBranch(opcode) && opcode != op_switch_imm && opcode != op_switch_char && opcode != op_switch_string)
                    link(block, m_graph.targetFrom(block, block->bytecodeEnd));
            } else if (!isTerminal(opcode) && !isThrow(opcode)) {
                if (block->bytecodeEnd >= size) {
                    m_graph.fail("falls off the end"_s);
                    return false;
                }
                link(block, m_graph.targetFrom(block, block->bytecodeEnd));
            }
        }

        for (unsigned i = 0; i < m_codeBlock->numberOfExceptionHandlers(); ++i) {
            BasicBlock* target = m_graph.blockForOffset[m_codeBlock->exceptionHandler(i).target];
            if (!target->isCatchEntrypoint) {
                target->isCatchEntrypoint = true;
                m_graph.catchEntrypoints.append(target);
            }
            if (m_instructions.at(target->bytecodeBegin)->opcodeID() != op_catch) {
                m_graph.fail("handler does not start with op_catch"_s);
                return false;
            }
            if (!target->predecessors.isEmpty()) {
                m_graph.fail("handler is also a jump target"_s);
                return false;
            }
        }
        return true;
    }

    void computeReversePostOrder()
    {
        Vector<BasicBlock*> postOrder;
        BitVector visited(m_graph.blocks.size());
        BitVector onStack(m_graph.blocks.size());
        Vector<std::pair<BasicBlock*, BasicBlock*>>& backEdges = m_backEdges; // From, to.
        backEdges.shrink(0);
        struct Frame {
            BasicBlock* block;
            unsigned next;
        };
        auto visitFrom = [&](BasicBlock* start) {
            if (visited.get(start->index))
                return;
            Vector<Frame> stack;
            visited.set(start->index);
            onStack.set(start->index);
            stack.append({ start, 0 });
            while (!stack.isEmpty()) {
                Frame& frame = stack.last();
                if (frame.next < frame.block->successors.size()) {
                    BasicBlock* successor = frame.block->successors[frame.next++];
                    if (onStack.get(successor->index)) {
                        successor->isLoopHeader = true;
                        backEdges.append({ frame.block, successor });
                    }
                    if (!visited.get(successor->index)) {
                        visited.set(successor->index);
                        onStack.set(successor->index);
                        stack.append({ successor, 0 });
                    }
                    continue;
                }
                onStack.clear(frame.block->index);
                postOrder.append(frame.block);
                stack.removeLast();
            }
        };
        // Handlers last in post order means first in the reverse; visiting them first keeps the root at the front.
        for (BasicBlock* entrypoint : m_graph.catchEntrypoints)
            visitFrom(entrypoint);
        visitFrom(m_graph.root);
        for (unsigned i = postOrder.size(); i--;) {
            postOrder[i]->isReachable = true;
            m_graph.m_rpo.append(postOrder[i]);
        }
        for (BasicBlock* block : m_graph.m_rpo) {
            block->predecessors.removeAllMatching([](BasicBlock* predecessor) {
                return !predecessor->isReachable;
            });
        }

        // The body of a loop: whatever gets to the jump back without going through the header.
        for (auto [from, header] : backEdges) {
            header->isInLoop = true;
            Vector<BasicBlock*> worklist;
            worklist.append(from);
            BitVector seen(m_graph.blocks.size());
            seen.set(header->index);
            while (!worklist.isEmpty()) {
                BasicBlock* block = worklist.takeLast();
                if (seen.get(block->index))
                    continue;
                seen.set(block->index);
                block->isInLoop = true;
                for (BasicBlock* predecessor : block->predecessors)
                    worklist.append(predecessor);
            }
        }
    }

    void computeLiveness()
    {
        unsigned numRegisters = m_graph.numRegisters();
        Vector<BitVector> uses(m_graph.blocks.size());
        Vector<BitVector>& defs = m_defsOfBlocks;
        defs.clear();
        defs.grow(m_graph.blocks.size());
        for (BasicBlock* block : m_graph.m_rpo) {
            BitVector& use = uses[block->index];
            BitVector& def = defs[block->index];
            use.ensureSize(numRegisters);
            def.ensureSize(numRegisters);
            block->liveIn.ensureSize(numRegisters);
            for (unsigned offset = block->bytecodeBegin; offset < block->bytecodeEnd; offset += m_instructions.at(offset)->size()) {
                const JSInstruction* instruction = m_instructions.at(offset).ptr();
                forEachUse(instruction, [&](VirtualRegister reg) {
                    if (!m_graph.isTracked(reg))
                        return;
                    unsigned index = m_graph.registerIndex(reg);
                    if (!def.get(index))
                        use.set(index);
                });
                forEachDef(instruction, [&](VirtualRegister reg) {
                    if (m_graph.isTracked(reg))
                        def.set(m_graph.registerIndex(reg));
                });
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (unsigned i = m_graph.m_rpo.size(); i--;) {
                BasicBlock* block = m_graph.m_rpo[i];
                BitVector live(numRegisters);
                for (BasicBlock* successor : block->successors)
                    live.merge(successor->liveIn);
                live.exclude(defs[block->index]);
                live.merge(uses[block->index]);
                if (live != block->liveIn) {
                    block->liveIn = WTF::move(live);
                    changed = true;
                }
            }
        }
    }

    struct SkippedStore {
        BasicBlock* block;
        unsigned index; // In the nodes of the block: where it would have been.
        VirtualRegister reg;
        Node* value;
    };
    Vector<SkippedStore> m_skippedStores;
    bool m_needsEveryStore { false };
    Vector<BitVector> m_defsOfBlocks;

    void chooseHomedRegisters()
    {
        m_graph.m_homed.ensureSize(m_graph.numRegisters());
        for (BasicBlock* entrypoint : m_graph.catchEntrypoints)
            m_graph.m_homed.merge(entrypoint->liveIn);
        m_graph.homedTypes.fill(TNone, m_graph.numRegisters());

        // It is for the sake of a handler that they do, which has nothing to go by but what is in memory. The number of a register is
        // put to one use after another: what is in it is only anybody's business where a handler that reads it is still to come.
        m_skippedStores.clear();
        m_needsEveryStore = !Options::aotStoresHomedRegistersOnlyWhereRead();
        if (m_graph.catchEntrypoints.isEmpty() || m_needsEveryStore)
            return;
        unsigned numRegisters = m_graph.numRegisters();
        Vector<BitVector> atStart(m_graph.blocks.size());
        for (BasicBlock* block : m_graph.m_rpo) {
            block->readByHandlersOfBlock.ensureSize(numRegisters);
            block->readByHandlersAfterBlock.ensureSize(numRegisters);
            atStart[block->index].ensureSize(numRegisters);
        }
        for (unsigned i = 0; i < m_codeBlock->numberOfExceptionHandlers(); ++i) {
            auto& handler = m_codeBlock->exceptionHandler(i);
            BasicBlock* target = m_graph.blockForOffset[handler.target];
            for (BasicBlock* block : m_graph.m_rpo) {
                // (What a block ends in may be done as part of it: see inlineCall().)
                if (block->bytecodeBegin < handler.end && handler.start <= block->bytecodeEnd)
                    block->readByHandlersOfBlock.merge(target->liveIn);
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned i = m_graph.m_rpo.size(); i--;) {
                BasicBlock* block = m_graph.m_rpo[i];
                BitVector after(numRegisters);
                for (BasicBlock* successor : block->successors)
                    after.merge(atStart[successor->index]);
                BitVector start = after;
                start.exclude(m_defsOfBlocks[block->index]);
                start.merge(block->readByHandlersOfBlock);
                if (after != block->readByHandlersAfterBlock || start != atStart[block->index]) {
                    block->readByHandlersAfterBlock = WTF::move(after);
                    atStart[block->index] = WTF::move(start);
                    changed = true;
                }
            }
        }
    }

    // If it turns out that a register is read from memory somewhere other than on the way in to a handler.
    void makeSkippedStores()
    {
        for (unsigned i = m_skippedStores.size(); i--;) {
            auto& skipped = m_skippedStores[i];
            Node* node = m_graph.addNode(NodeKind::SetStack);
            node->reg = skipped.reg;
            node->block = skipped.block;
            node->uses.append({ VirtualRegister(), skipped.value });
            skipped.block->nodes.insert(skipped.index, node);
        }
        m_skippedStores.clear();
    }

    Node* append(BasicBlock* block, Node* node)
    {
        node->block = block;
        block->nodes.append(node);
        return node;
    }

    Node* constantFor(VirtualRegister reg)
    {
        ASSERT(reg.isConstant());
        unsigned index = reg.toConstantIndex();
        while (m_constantCells.size() <= index)
            m_constantCells.append(nullptr);
        if (m_codeBlock->constantSourceCodeRepresentation(reg) == SourceCodeRepresentation::LinkTimeConstant) {
            if (!m_constantCells[index]) {
                Node* node = m_graph.addNode(NodeKind::ConstantCell);
                node->range = IntegerRange::unknown();
                node->reg = reg;
                node->type = TTop; // Which one it is is known, but not what the realm makes of it.
                m_constantCells[index] = node;
            }
            return m_constantCells[index];
        }
        JSValue value = m_codeBlock->getConstant(reg);
        if (!value || !value.isCell())
            return m_graph.constant(value);
        if (!m_constantCells[index]) {
            Node* node = m_graph.addNode(NodeKind::ConstantCell);
            node->range = IntegerRange::unknown();
            node->reg = reg;
            // (What says how to make the array that a tagged template is passed has that array in its place by the time the code runs.)
            node->type = value.asCell()->inherits<JSTemplateObjectDescriptor>() ? TArray : typeOfValue(value);
            m_constantCells[index] = node;
        }
        return m_constantCells[index];
    }

    Node* get(BasicBlock* block, VirtualRegister reg)
    {
        if (reg.isConstant())
            return constantFor(reg);
        if (reg == VirtualRegister(CallFrameSlot::callee)) {
            // Nothing writes it: read where it is wanted.
            Node* node = m_graph.addNode(NodeKind::Argument);
            node->reg = reg;
            node->type = TAnyObject;
            return append(block, node);
        }
        if (!m_graph.isTracked(reg)) {
            m_graph.fail("reads a register outside the frame"_s);
            return m_graph.constant(jsUndefined());
        }
        Node* value = block->valuesAtTail[m_graph.registerIndex(reg)];
        if (!value && m_graph.isHomed(reg)) {
            m_needsEveryStore = true;
            Node* node = m_graph.addNode(NodeKind::GetStack);
            node->reg = reg;
            return append(block, node);
        }
        if (!value) {
            // Bytecode liveness says nothing reads this, and yet: it is a register that is only read on a path where it was
            // never written, which is to say that op_enter's undefined is what it holds.
            return m_graph.constant(jsUndefined());
        }
        return value;
    }

    void set(BasicBlock* block, VirtualRegister reg, Node* value)
    {
        if (!m_graph.isTracked(reg)) {
            m_graph.fail("writes a register outside the frame"_s);
            return;
        }
        if (m_graph.isHomed(reg)) {
            unsigned index = m_graph.registerIndex(reg);
            if (m_needsEveryStore || block->readByHandlersOfBlock.get(index) || block->readByHandlersAfterBlock.get(index)) {
                Node* node = m_graph.addNode(NodeKind::SetStack);
                node->reg = reg;
                node->uses.append({ VirtualRegister(), value });
                append(block, node);
                // That is for whoever gets here by way of a handler, with nothing to go by but what is in memory. Everybody else
                // knows what was put there.
            } else
                m_skippedStores.append({ block, static_cast<unsigned>(block->nodes.size()), reg, value });
        }
        block->valuesAtTail[m_graph.registerIndex(reg)] = value;
    }

    void parseBlock(BasicBlock* block)
    {
        unsigned numRegisters = m_graph.numRegisters();
        block->valuesAtTail.fill(nullptr, numRegisters);

        if (block == m_graph.root) {
            for (unsigned i = 0; i < m_graph.numArguments(); ++i) {
                Node* node = m_graph.addNode(NodeKind::Argument);
                node->reg = virtualRegisterForArgumentIncludingThis(i);
                node->type = m_graph.typeOfArgumentOnEntry(i);
                append(block, node);
                set(block, node->reg, node);
            }
            // Every local is undefined until op_enter says so again; a homed one needs its slot to hold a value from the start.
            for (unsigned i = 0; i < m_graph.numLocals(); ++i)
                set(block, virtualRegisterForLocal(i), m_graph.constant(jsUndefined()));
            return;
        }

        bool needsPhis = block->predecessors.size() != 1 || block->isLoopHeader || block->predecessors[0]->valuesAtTail.isEmpty();
        if (block->isCatchEntrypoint) {
            for (unsigned index : block->liveIn) {
                Node* node = m_graph.addNode(NodeKind::GetStack);
                node->reg = m_graph.registerForIndex(index);
                block->valuesAtTail[index] = append(block, node);
            }
        } else {
            for (unsigned index : block->liveIn) {
                if (!needsPhis) {
                    block->valuesAtTail[index] = valueLeaving(block->predecessors[0], block, index);
                    continue;
                }
                Node* phi = m_graph.addNode(NodeKind::Phi);
                phi->reg = m_graph.registerForIndex(index);
                phi->block = block;
                block->phis.append(phi);
                block->valuesAtTail[index] = phi;
            }
        }

        RELEASE_ASSERT(m_literalsBeingMade.isEmpty());
        for (unsigned offset = block->bytecodeBegin; offset < block->bytecodeEnd; offset += m_instructions.at(offset)->size()) {
            const JSInstruction* instruction = m_instructions.at(offset).ptr();
            OpcodeID opcode = instruction->opcodeID();
            if (opcode == op_check_type && isFact(instruction->as<OpCheckType>().m_mask)) {
                // It is taken out here, and nothing further on knows of it but by what it leaves on the node.
                auto bytecode = instruction->as<OpCheckType>();
                unsigned kind = bytecode.m_mask >> 28;
                if (kind == FactBody || bytecode.m_value.isConstant())
                    continue;
                Node* value = get(block, bytecode.m_value);
                while (value->isBytecode(op_check_type))
                    value = value->use(value->as<OpCheckType>().m_value);
                if (kind == FactArray)
                    value->iteratedFact = (bytecode.m_mask & 15) + 1;
                else if ((kind == FactField && value->isBytecode(op_get_by_id)) || (kind == FactElement && value->isBytecode(op_get_by_val)))
                    value->fact = bytecode.m_mask;
                else if ((kind == FactDirect || kind == FactBuiltin) && value->isBytecode(op_call)) {
                    value->fact = bytecode.m_mask;
                    Node* callee = value->use(value->as<OpCall>().m_callee);
                    if (kind == FactBuiltin && callee->isBytecode(op_get_by_id) && !callee->fact)
                        callee->fact = bytecode.m_mask;
                }
                continue;
            }
            if (opcode == op_put_by_id && !m_literalsBeingMade.isEmpty() && m_literalsBeingMade.last().stores[m_literalsBeingMade.last().next] == offset) {
                auto& literal = m_literalsBeingMade.last();
                literal.node->uses.append({ NewObjectPlan::registerOf(literal.next), get(block, instruction->as<OpPutById>().m_value) });
                if (++literal.next == literal.stores.size()) {
                    Node* node = literal.node;
                    m_literalsBeingMade.removeLast();
                    append(block, node);
                    set(block, node->reg, node);
                }
                continue;
            }
            if (m_objectBeingPlanned && opcode == op_put_by_id) {
                // One of the stores that the object is going to be made with the outcome of.
                auto& stores = m_planOfObject.stores;
                RELEASE_ASSERT(m_nextStoreOfPlan < stores.size() && stores[m_nextStoreOfPlan].offset == offset);
                VirtualRegister reg = NewObjectPlan::registerOf(stores[m_nextStoreOfPlan].property);
                Node* value = get(block, instruction->as<OpPutById>().m_value);
                bool found = false;
                for (auto& use : m_objectBeingPlanned->uses) {
                    if (use.reg == reg) {
                        use.node = value;
                        found = true;
                    }
                }
                if (!found)
                    m_objectBeingPlanned->uses.append({ reg, value });
                if (++m_nextStoreOfPlan == stores.size()) {
                    append(block, m_objectBeingPlanned);
                    m_objectBeingPlanned = nullptr;
                }
                continue;
            }

            switch (opcode) {
            case op_new_object: {
                VirtualRegister reg = instruction->as<OpNewObject>().m_dst;
                // A register that lives in memory is written where the instruction is.
                if (!m_graph.isTracked(reg) || m_graph.isHomed(reg))
                    break;
                auto stores = Graph::storesOfLiteral(m_instructions, offset);
                while (!stores.isEmpty() && stores.last() >= block->bytecodeEnd)
                    stores.removeLast();
                if (stores.isEmpty())
                    break;
                Node* node = m_graph.addNode(NodeKind::Bytecode);
                node->opcode = opcode;
                node->instruction = instruction;
                node->bytecodeIndex = BytecodeIndex(offset);
                node->numberOfLiteralProperties = stores.size();
                node->reg = reg;
                m_literalsBeingMade.append({ node, WTF::move(stores), 0 });
                continue;
            }
            case op_create_this: {
                // A register that lives in memory is written where the instruction is.
                if (m_graph.hasHomedRegisters() || block->isInLoop)
                    break;
                m_planOfObject = NewObjectPlan::forCreateThis(m_instructions, offset);
                if (m_planOfObject.stores.isEmpty() || m_planOfObject.stores.last().offset >= block->bytecodeEnd)
                    break;
                auto bytecode = instruction->as<OpCreateThis>();
                Node* node = m_graph.addNode(NodeKind::Bytecode);
                node->opcode = opcode;
                node->instruction = instruction;
                node->bytecodeIndex = BytecodeIndex(offset);
                node->uses.append({ bytecode.m_callee, get(block, bytecode.m_callee) });
                node->numberOfLiteralProperties = m_planOfObject.properties.size();
                node->reg = bytecode.m_dst;
                node->block = block;
                set(block, bytecode.m_dst, node);
                m_objectBeingPlanned = node;
                m_nextStoreOfPlan = 0;
                continue;
            }
            case op_mov: {
                auto bytecode = instruction->as<OpMov>();
                set(block, bytecode.m_dst, get(block, bytecode.m_src));
                continue;
            }
            case op_enter:
                for (unsigned i = m_codeBlock->numVars(); i--;)
                    set(block, virtualRegisterForLocal(i), m_graph.constant(jsUndefined()));
                // It also puts the callee's scope in the scope register.
                if (m_codeBlock->scopeRegister().isValid()) {
                    Node* scope = m_graph.addNode(NodeKind::Bytecode);
                    scope->opcode = op_get_scope;
                    scope->bytecodeIndex = BytecodeIndex(offset);
                    scope->reg = m_codeBlock->scopeRegister();
                    append(block, scope);
                    set(block, scope->reg, scope);
                }
                continue;
            case op_iterator_close_check: {
                // A branch that also writes a register, which is one thing too many for a node: a terminal has to come last,
                // and what it defines has to be there for the phis of the successors and for the store of a homed register.
                // So it is two nodes. The first is the iterator as the instruction leaves it. The second is the branch, and
                // needs nothing but that to tell where to go (see Lowering::lowerIteratorCloseCheck()).
                auto bytecode = instruction->as<OpIteratorCloseCheck>();
                auto addPart = [&] {
                    Node* node = m_graph.addNode(NodeKind::Bytecode);
                    node->opcode = opcode;
                    node->instruction = instruction;
                    node->bytecodeIndex = BytecodeIndex(offset);
                    return node;
                };
                Node* iterator = addPart();
                forEachUse(instruction, [&](VirtualRegister reg) {
                    iterator->uses.append({ reg, get(block, reg) });
                });
                iterator->reg = bytecode.m_iterator;
                append(block, iterator);
                set(block, bytecode.m_iterator, iterator);
                Node* branch = addPart();
                branch->uses.append({ bytecode.m_iterator, iterator });
                append(block, branch);
                continue;
            }
            case op_nop:
            case op_super_sampler_begin:
            case op_super_sampler_end:
            case op_identity_with_profile: // Leaves its operand as it is.
                continue;
            case op_resolve_scope:
            case op_get_from_scope:
            case op_get_by_id:
                if (auto [known, dst] = intrinsicReadBy(block, instruction); known) {
                    set(block, dst, known);
                    continue;
                }
                break;
            default:
                break;
            }

            bool comesAfterGuard = offset == block->bytecodeBegin && !block->isGeneric && block->predecessors.size() == 1 && block->predecessors[0]->endsWithGuard && !block->predecessors[0]->isPreHeader;
            if (comesAfterGuard && opcode == op_call) {
                if (auto it = m_resultsOfInlinedCalls.find(offset); it != m_resultsOfInlinedCalls.end()) {
                    set(block, instruction->as<OpCall>().m_dst, it->value);
                    continue;
                }
            }

            Node* node = m_graph.addNode(NodeKind::Bytecode);
            node->opcode = opcode;
            node->instruction = instruction;
            node->bytecodeIndex = BytecodeIndex(offset);
            forEachUse(instruction, [&](VirtualRegister reg) {
                node->uses.append({ reg, get(block, reg) });
            });
            if (opcode == op_call || opcode == op_call_ignore_result || opcode == op_tail_call) {
                VirtualRegister thisRegister = Graph::operandsOfCall(instruction).argument(0);
                for (auto& use : node->uses) {
                    if (use.reg == thisRegister && m_graph.isScopeThatStandsForNoThis(use.node))
                        use.node = m_graph.constant(jsUndefined());
                }
            }
            append(block, node);
            if (offset == block->bytecodeBegin && !block->isGeneric && block->predecessors.size() == 1 && block->predecessors[0]->endsWithGuard && !block->predecessors[0]->isPreHeader) {
                node->guard = block->predecessors[0]->terminal();
                node->guard->guarded = node;
            }
            if (opcode == op_get_by_val) {
                unsigned next = offset + instruction->size();
                if (next < m_instructions.size() && m_instructions.at(next)->opcodeID() == op_check_type) {
                    auto check = m_instructions.at(next)->as<OpCheckType>();
                    if (check.m_value == instruction->as<OpGetByVal>().m_dst && !isFact(check.m_mask))
                        node->expectedMask = check.m_mask;
                }
            }

            Vector<VirtualRegister, 4> defs;
            forEachDef(instruction, [&](VirtualRegister reg) {
                defs.append(reg);
            });
            if (opcode == op_check_type) {
                // It leaves the register as it is, but from here on more is known about what is in it. That is what all of this
                // is for: the check is a definition, of the same value with a smaller type.
                VirtualRegister reg = instruction->as<OpCheckType>().m_value;
                if (m_graph.isTracked(reg)) {
                    node->reg = reg;
                    // (What is in memory is the same value as before.)
                    block->valuesAtTail[m_graph.registerIndex(reg)] = node;
                }
            } else if (defs.size() == 1) {
                node->reg = defs[0];
                set(block, defs[0], node);
            } else {
                for (VirtualRegister reg : defs) {
                    Node* proj = m_graph.addNode(NodeKind::Proj);
                    proj->reg = reg;
                    proj->bytecodeIndex = node->bytecodeIndex;
                    proj->uses.append({ VirtualRegister(), node });
                    append(block, proj);
                    set(block, reg, proj);
                }
            }
            if (m_graph.failed())
                return;
        }

        if (block->endsWithGuard) {
            const JSInstruction* instruction = m_instructions.at(block->bytecodeEnd).ptr();
            Node* guard = m_graph.addNode(NodeKind::Guard);
            if (block->isReentry) {
                for (unsigned index : block->liveIn) {
                    Node* value = block->valuesAtTail[index];
                    if (m_graph.m_homed.get(index) || !value)
                        continue;
                    Node* narrow = m_graph.addNode(NodeKind::Narrow);
                    narrow->reg = m_graph.registerForIndex(index);
                    narrow->uses.append({ VirtualRegister(), value });
                    append(block, narrow);
                    block->valuesAtTail[index] = narrow;
                }
                guard->guardKind = GuardKind::Reentry;
                guard->bytecodeIndex = BytecodeIndex(block->bytecodeEnd);
                append(block, guard);
                return;
            }
            if (block->isPreHeader) {
                guard->guardKind = GuardKind::Nothing;
                guard->bytecodeIndex = BytecodeIndex(block->bytecodeEnd);
                append(block, guard);
                return;
            }
            if (Node* result = inlineCall(block, instruction, block->bytecodeEnd)) {
                m_resultsOfInlinedCalls.set(block->bytecodeEnd, result);
                guard->guardKind = GuardKind::Nothing;
                guard->bytecodeIndex = BytecodeIndex(block->bytecodeEnd);
                append(block, guard);
                return;
            }
            if (intrinsicReadBy(block, instruction).first) {
                guard->guardKind = GuardKind::Nothing;
                guard->bytecodeIndex = BytecodeIndex(block->bytecodeEnd);
                append(block, guard);
                return;
            }
            guard->opcode = instruction->opcodeID();
            guard->instruction = instruction;
            guard->bytecodeIndex = BytecodeIndex(block->bytecodeEnd);
            forEachUse(instruction, [&](VirtualRegister reg) {
                guard->uses.append({ reg, get(block, reg) });
            });
            append(block, guard);
            return;
        }

        // A terminal has to be the last node, and a SetStack or a Proj after it would not be.
        if (Node* last = block->terminal(); last && last->kind != NodeKind::Bytecode) {
            for (Node* node : block->nodes) {
                if (node->kind == NodeKind::Bytecode && (isBranch(node->opcode) || isTerminal(node->opcode) || isThrow(node->opcode))) {
                    m_graph.fail("terminal defines a register"_s, node->opcode);
                    return;
                }
            }
        }
    }

    // What is in the register on the way from one block to the other.
    Node* valueLeaving(BasicBlock* from, BasicBlock* to, unsigned index)
    {
        Node* value = from->valuesAtTail[index];
        // What did not pass the test goes on as what it was.
        if (value && from->isReentry && to != from->successors[0] && value->kind == NodeKind::Narrow && value->block == from)
            return value->uses[0].node;
        return value;
    }

    void fillPhis()
    {
        for (BasicBlock* block : m_graph.m_rpo) {
            if (block->isReentry) {
                BasicBlock* header = block->successors[0]->successors[0];
                for (Node* node : block->nodes) {
                    if (node->kind != NodeKind::Narrow)
                        continue;
                    for (Node* phi : header->phis) {
                        if (phi->reg == node->reg)
                            node->target = phi;
                    }
                }
            }
            for (Node* phi : block->phis) {
                unsigned index = m_graph.registerIndex(phi->reg);
                for (BasicBlock* predecessor : block->predecessors) {
                    Node* value = valueLeaving(predecessor, block, index);
                    if (!value)
                        value = m_graph.constant(jsUndefined());
                    phi->uses.append({ VirtualRegister(), value });
                }
            }
        }
    }

    static Node* resolve(Node* node)
    {
        while (node->replacement)
            node = node->replacement;
        return node;
    }

    void simplifyPhis()
    {
        // A phi that only one value ever gets to is that value. The phis of a variable that a loop leaves alone have each other for
        // inputs, all the way around the loop and through both copies of it, so this starts from the assumption that they are all
        // nothing and looks for what contradicts it.
        UncheckedKeyHashMap<Node*, Node*> origins; // Missing: nothing gets to it, so far. Itself: more than one value does.
        auto originOf = [&](Node* node) -> Node* {
            while (node->kind == NodeKind::Narrow)
                node = node->uses[0].node;
            return node->kind == NodeKind::Phi ? origins.get(node) : node;
        };
        bool changed = true;
        while (changed) {
            changed = false;
            for (BasicBlock* block : m_graph.m_rpo) {
                for (Node* phi : block->phis) {
                    Node* before = origins.get(phi);
                    if (before == phi)
                        continue;
                    Node* origin = nullptr;
                    for (auto& use : phi->uses) {
                        Node* value = originOf(use.node);
                        if (!value || value == origin)
                            continue;
                        if (origin) {
                            origin = phi;
                            break;
                        }
                        origin = value;
                    }
                    if (origin == before)
                        continue;
                    origins.set(phi, origin);
                    changed = true;
                }
            }
        }
        for (auto& [phi, origin] : origins) {
            if (origin != phi)
                phi->replacement = origin;
        }
        for (BasicBlock* block : m_graph.m_rpo) {
            block->phis.removeAllMatching([](Node* phi) {
                return !!phi->replacement;
            });
            for (Node* phi : block->phis) {
                for (auto& use : phi->uses)
                    use.node = resolve(use.node);
            }
            for (Node* node : block->nodes) {
                for (auto& use : node->uses)
                    use.node = resolve(use.node);
                // A variable that the loop leaves alone is on its way to the pre-header's phi, if to any.
                if (node->target) {
                    node->target = resolve(node->target);
                    if (node->target->kind != NodeKind::Phi)
                        node->target = nullptr;
                }
            }
            for (auto& value : block->valuesAtTail) {
                if (value)
                    value = resolve(value);
            }
        }
    }

    Graph& m_graph;
    UnlinkedCodeBlock* m_codeBlock;
    const JSInstructionStream& m_instructions;
    Vector<Node*> m_constantCells;
    // By bytecode offset, once the loops are known.
    bool m_hasGuards { false };
    BitVector m_leaders;
    BitVector m_inLoop;
    BitVector m_guards;
    BitVector m_loopHeaders;
    struct LiteralBeingMade {
        Node* node; // The op_new_object, which is going to be put where the last of its stores is.
        Vector<unsigned, 16> stores;
        unsigned next;
    };
    Vector<LiteralBeingMade, 2> m_literalsBeingMade; // One inside the other.
    Node* m_objectBeingPlanned { nullptr }; // An op_create_this that is going to be put where the last of its stores is.
    NewObjectPlan m_planOfObject;
    unsigned m_nextStoreOfPlan { 0 };
    Vector<std::pair<BasicBlock*, BasicBlock*>> m_backEdges;
    Vector<std::pair<VirtualRegister, unsigned>, 8> m_recentProperties; // In the block being looked at: what op_get_by_id put where.
    Vector<std::pair<VirtualRegister, const KnownFunction*>, 8> m_recentFunctions; // And what op_get_from_scope probably did.
    UncheckedKeyHashMap<UnlinkedFunctionCodeBlock*, bool> m_canBeInlined;
    UncheckedKeyHashMap<unsigned, Node*, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_resultsOfInlinedCalls; // By the offset of the call.
};

} // anonymous namespace

bool parseBytecode(Graph& graph)
{
    Parser parser(graph);
    return parser.run();
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
