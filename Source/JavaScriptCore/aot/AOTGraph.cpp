/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#include "AOTTypeTable.h"

#if ENABLE(AOT)

#include "AOTBuiltins.h"
#include "AOTProgram.h"
#include "AOTStubs.h"
#include "BuiltinNames.h"
#include "BytecodeStructs.h"
#include "BytecodeUseDef.h"
#include "ImmutableIntrinsics.h"
#include "JSCInlines.h"
#include "JSGenerator.h"
#include "JSTemplateObjectDescriptor.h"
#include "PreciseJumpTargetsInlines.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ScopedLambda.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static Type cellTypeForJSType(JSType type)
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
            return typeForTypedArray(type);
        return type >= ObjectType ? TOtherObject : TCellOther;
    }
}

Type valueType(JSValue value)
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
    if (value.isString()) {
        const StringImpl* impl = asString(value)->tryGetValueImpl();
        return impl && impl->isAtom() ? TAtomString : asString(value)->length() <= TypedLayoutTable::maxAtomizedStringLength ? TShortOtherString : TLongString;
    }
    return cellTypeForJSType(value.asCell()->type());
}

Type AtomicType::join(Type type)
{
    Type before = load();
    if ((before | type) == before)
        return before;
    static Lock locks[64];
    Locker locker { locks[(std::bit_cast<uintptr_t>(this) >> 4) % std::size(locks)] };
    before = load();
    store(before | type);
    return before;
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
    take(TAtomString, "AtomString"_s);
    take(TOtherString, "OtherString"_s);
    take(TShortOtherString, "ShortOtherString"_s);
    take(TLongString, "LongString"_s);
    take(TSymbol, "Symbol"_s);
    take(TBigInt, "BigInt"_s);
    take(TFunction, "Function"_s);
    if (type & TFunctionTag) {
        if (uint32_t function = functionNumberOf(type))
            out.print(bar, "Function#", function);
        else
            out.print(bar, "Function#?");
        type &= ~TFunction;
    }
    take(TArray, "Array"_s);
    take(TObject, "Object"_s);
    take(TOtherObject, "OtherObject"_s);
    take(TFinalObject, "FinalObject"_s);
    if (type & TFinalObjectTag) {
        auto layouts = layoutRangeOf(type);
        out.print(bar, "FinalObject#", layouts.lowest);
        if (!layouts.isOne())
            out.print("..", layouts.highest);
        type &= ~TFinalObject;
    }
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
    , m_convention(conventionOf(codeBlock))
{
    if (Options::useTypeTags()) [[unlikely]] {
        auto& instructions = codeBlock->instructions();
        for (unsigned offset = 0; offset < instructions.size(); offset += instructions.at(offset)->size()) {
            auto instruction = instructions.at(offset);
            if (instruction->opcodeID() == op_type_tag)
                m_typeTags.set(offset + instruction->size(), instruction->as<OpTypeTag>().m_tag);
        }
    }
}

Graph::~Graph() = default;

Node* Graph::addNode(NodeKind kind)
{
    Node& node = m_nodes.alloc();
    node.graph = this;
    node.kind = kind;
    node.index = m_nodes.size() - 1 + m_numberOfInlinedNodes;
    return &node;
}

BasicBlock* Graph::addBlock()
{
    blocks.append(makeUniqueWithoutFastMallocCheck<BasicBlock>());
    blocks.last()->index = blocks.size() - 1;
    blocks.last()->graph = this;
    return blocks.last().get();
}

Node* Graph::constant(JSValue value)
{
    ASSERT(!value || !value.isCell());
    if (!value) {
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
        node->type = valueType(value);
        node->range = IntegerRange::forNumber(value);
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
        case op_type_tag:
            continue;
        case op_check_type:
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
                plan.properties.append({ bytecode.m_property, bytecode.m_flags.isDirect(), bytecode.m_flags.ecmaMode().isStrict(), !bytecode.m_flags.isDirect() });
            } else if (!plan.properties[index].isDefined)
                return plan;
            else
                plan.properties[index].isAssigned |= !bytecode.m_flags.isDirect();
            plan.stores.append({ offset, static_cast<unsigned>(index) });
            continue;
        }
        default:
            return plan;
        }
    }
    return plan;
}

const Vector<unsigned, 4>& Graph::literalStores(unsigned offsetOfNewObject)
{
    if (!m_hasFoundLiteralStores)
        findLiteralStores();
    auto it = m_literalStores.find(offsetOfNewObject);
    RELEASE_ASSERT(it != m_literalStores.end());
    return it->value;
}

void Graph::findLiteralStores()
{
    m_hasFoundLiteralStores = true;
    constexpr unsigned maximumCount = KnownShape::maxProperties;
    UncheckedKeyHashMap<int, unsigned, DefaultHash<int>, WTF::UnsignedWithZeroKeyHashTraits<int>> open;
    for (const auto& instruction : m_codeBlock->instructions()) {
        OpcodeID opcode = instruction->opcodeID();
        VirtualRegister initialized;
        if (opcode == op_put_by_id) {
            auto bytecode = instruction->as<OpPutById>();
            if (auto it = open.find(bytecode.m_base.offset()); it != open.end()) {
                if (bytecode.m_value == bytecode.m_base || !bytecode.m_flags.isDirect())
                    open.remove(it);
                else {
                    auto& stores = m_literalStores.find(it->value)->value;
                    stores.append(instruction.offset());
                    if (stores.size() == maximumCount)
                        open.remove(it);
                    else
                        initialized = bytecode.m_base;
                }
            }
        }
        if (isBranch(opcode) || isTerminal(opcode) || isThrow(opcode) || opcode == op_loop_hint || opcode == op_catch) {
            open.clear();
            continue;
        }
        if (!open.isEmpty()) {
            auto close = [&](VirtualRegister reg) {
                if (reg != initialized)
                    open.remove(reg.offset());
            };
            for (unsigned checkpoint = 0; checkpoint < instruction->numberOfCheckpoints(); ++checkpoint) {
                computeUsesForBytecodeIndexImpl(instruction.ptr(), checkpoint, close);
                computeDefsForBytecodeIndexImpl(0, instruction.ptr(), checkpoint, close);
            }
        }
        if (opcode == op_new_object) {
            m_literalStores.add(instruction.offset(), Vector<unsigned, 4>());
            open.set(instruction->as<OpNewObject>().m_dst.offset(), instruction.offset());
        }
    }
}

Node* Graph::intrinsic(unsigned number)
{
    const ImmutableIntrinsics::Entry& entry = ImmutableIntrinsics::shared()->at(number);
    if (!entry.isCell)
        return constant(JSValue::decode(entry.primitive));
    return m_intrinsics.ensure(entry.canonical, [&] {
        Node* node = addNode(NodeKind::Intrinsic);
        node->intrinsic = entry.canonical;
        node->type = cellTypeForJSType(entry.type);
        node->range = IntegerRange::unknown();
        return node;
    }).iterator->value;
}

Node* Graph::intrinsicReadBy(const JSInstruction* instruction, Node* base)
{
    auto isGlobal = [&](unsigned identifier, unsigned depth, ResolveType type) {
        return !isStaticClosureVarResolveType(type) && type != ResolvedClosureVar && type != ResolvedLazyClosureVar && resolveStatically(identifier, depth, type).isGlobal;
    };
    if (instruction->opcodeID() == op_get_from_scope) {
        auto bytecode = instruction->as<OpGetFromScope>();
        const Identifier& name = m_codeBlock->identifier(bytecode.m_var);
        bool isUndefined = name == m_vm.propertyNames->undefinedKeyword;
        bool isNaN = name == m_vm.propertyNames->NaN;
        if ((isUndefined || isNaN || name == m_vm.propertyNames->Infinity) && isGlobal(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_getPutInfo.resolveType()))
            return constant(isUndefined ? jsUndefined() : isNaN ? jsNaN() : jsNumber(std::numeric_limits<double>::infinity()));
    }
    const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
    if (!intrinsics)
        return nullptr;
    auto variable = [&](unsigned identifier, unsigned depth, ResolveType type) -> unsigned {
        if (!isGlobal(identifier, depth, type))
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

Graph::CallOperands Graph::callOperands(const JSInstruction* instruction)
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

std::optional<LinkTimeConstant> Graph::linkTimeConstantOf(const Node* node)
{
    if (node->kind != NodeKind::LinkTimeConstant)
        return std::nullopt;
    return static_cast<LinkTimeConstant>(node->intrinsic);
}

CallIntrinsic Graph::callIntrinsic(const Node* node) const
{
    if (node->graph != this)
        return node->graph->callIntrinsic(node);
    if (node->opcode != op_call && node->opcode != op_call_ignore_result)
        return CallIntrinsic::None;
    CallOperands operands = callOperands(node->instruction);
    Node* callee = node->use(operands.callee);
    if (callee->kind == NodeKind::Intrinsic) {
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
    return callIntrinsicFor(callee->graph->codeBlock()->identifier(callee->as<OpGetById>().m_property).impl(), operands.argc);
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
    if (node->graph != this)
        return node->graph->arrayAndElementStored(node);
    if (node->kind != NodeKind::Bytecode)
        return { nullptr, nullptr };
    if (node->opcode == op_put_by_val) {
        auto bytecode = node->as<OpPutByVal>();
        return { node->use(bytecode.m_base), node->use(bytecode.m_value) };
    }
    if (callIntrinsic(node) == CallIntrinsic::ArrayPush) {
        auto operands = callOperands(node->instruction);
        return { node->use(operands.argument(0)), node->use(operands.argument(1)) };
    }
    return { nullptr, nullptr };
}

std::optional<uint32_t> Graph::accessedEnvironmentDepth(const Node* node)
{
    if (node->graph != this)
        return node->graph->accessedEnvironmentDepth(node);
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
        auto resolution = m_declaredNames->resolve(m_codeBlock->identifier(identifier).impl());
        if (resolution.kind != DeclaredNamesLink::Resolution::Slot || !resolution.isInOutermostEnvironment || resolution.offset != offset)
            return std::nullopt;
        if (isScopeAtDepth(node->use(scopeRegister), resolution.hops))
            return found(m_linkage->environmentDepth);
        return std::nullopt;
    }
    if (type == Dynamic)
        return std::nullopt;
    auto variable = resolveStatically(identifier, depth, type);
    if (variable.kind == StaticVariable::Closure && variable.isInOutermostEnvironment)
        return found(m_linkage->environmentDepth);
    if (variable.kind == StaticVariable::Import && node->isBytecode(op_get_from_scope))
        return found(variable.import.environmentDepth);
    return std::nullopt;
}

bool Graph::isScopeUsedAsImplicitThis(const Node* node)
{
    if (node->graph != this)
        return node->graph->isScopeUsedAsImplicitThis(node);
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

bool Graph::isScopeAtDepth(const Node* scope, unsigned hops)
{
    if (scope->isBytecode(op_get_scope))
        return !hops;
    if (!scope->isBytecode(op_resolve_scope))
        return false;
    ResolveType type = scope->as<OpResolveScope>().m_resolveType;
    return isStaticClosureVarResolveType(type) && staticClosureVarHops(type) == hops;
}

std::optional<uint32_t> Graph::resolvedEnvironmentDepth(const Node* node)
{
    if (node->graph != this)
        return node->graph->resolvedEnvironmentDepth(node);
    if (!m_linkage)
        return std::nullopt;
    auto bytecode = node->as<OpResolveScope>();
    uint32_t distance = 0;
    if (isStaticClosureVarResolveType(bytecode.m_resolveType)) {
        if (!m_declaredNames)
            return std::nullopt;
        auto resolution = m_declaredNames->resolve(m_codeBlock->identifier(bytecode.m_var).impl());
        if (resolution.kind == DeclaredNamesLink::Resolution::Slot && resolution.isInOutermostEnvironment && resolution.hops == staticClosureVarHops(bytecode.m_resolveType))
            distance = m_linkage->environmentDepth;
    } else if (bytecode.m_resolveType != Dynamic) {
        if (auto variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType); variable.kind == StaticVariable::Import)
            distance = variable.import.environmentDepth;
        else if (variable.kind == StaticVariable::Closure && variable.isInOutermostEnvironment)
            distance = m_linkage->environmentDepth;
    }
    if (!distance)
        return std::nullopt;
    usesStaticImports = true;
    return distance;
}

bool Graph::calleeIsExact(const Node* node) const
{
    if (node->graph != this)
        return node->graph->calleeIsExact(node);
    bool isExact = false;
    return knownCallee(node, &isExact) && isExact;
}

const KnownFunction* Graph::knownCallee(const Node* node, bool* isExact) const
{
    if (node->graph != this)
        return node->graph->knownCallee(node, isExact);
    bool proven = false;
    const KnownFunction* known = knownCalleeIgnoringSummaries(node, &proven);
    if ((!known || !proven) && programFunctions() && (node->opcode == op_call || node->opcode == op_call_ignore_result || node->opcode == op_tail_call)) {
        VirtualRegister calleeRegister = node->opcode == op_tail_call ? node->as<OpTailCall>().m_callee : callOperands(node->instruction).callee;
        if (const KnownFunction* method = programFunctions()->function(closedMethodReadBy(node->use(calleeRegister))); method && method->forCall) {
            known = method;
            proven = true;
        }
    }
    if (!known || !proven) {
        VirtualRegister calleeRegister;
        switch (node->opcode) {
        case op_construct:
            calleeRegister = node->as<OpConstruct>().m_callee;
            break;
        case op_call:
            calleeRegister = node->as<OpCall>().m_callee;
            break;
        case op_call_ignore_result:
            calleeRegister = node->as<OpCallIgnoreResult>().m_callee;
            break;
        case op_tail_call:
            calleeRegister = node->as<OpTailCall>().m_callee;
            break;
        default:
            break;
        }
        if (calleeRegister.isValid() && programFunctions()) {
            Node* callee = node->use(calleeRegister);
            Type type = callee->type;
            if (callee->isBytecode(op_get_from_scope) && m_variableSummaries) {
                if (Variable variable = const_cast<Graph*>(this)->variableAccessedBy(callee))
                    type = m_variableSummaries->read(variable, callee->graph->codeBlock()->identifier(callee->as<OpGetFromScope>().m_var).impl(), summaryReader());
            }
            if (const KnownFunction* function = mayBe(type, TOtherObject) ? nullptr : programFunctions()->function(functionNumberOf(type)); function && (node->opcode == op_construct ? function->forConstruct && !function->executable->isBuiltinDefaultClassConstructor() : !!function->forCall)) {
                known = function;
                proven = true;
            }
        }
    }
    if (isExact)
        *isExact = proven;
    return known;
}

const KnownFunction* Graph::knownCalleeIgnoringSummaries(const Node* node, bool* isExact) const
{
    if (node->graph != this)
        return node->graph->knownCalleeIgnoringSummaries(node, isExact);
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
    return knownFunctionReadBy(callee, callee == node->use(calleeRegister) ? isExact : nullptr);
}

const KnownFunction* Graph::knownFunctionReadBy(const Node* callee, bool* isExact) const
{
    if (callee->graph != this)
        return callee->graph->knownFunctionReadBy(callee, isExact);
    if (!m_hints)
        return nullptr;
    auto bytecode = callee->as<OpGetFromScope>();
    UniquedStringImpl* name = m_codeBlock->identifier(bytecode.m_var).impl();
    ResolveType type = bytecode.m_getPutInfo.resolveType();
    if (type == ResolvedClosureVar || type == ResolvedLazyClosureVar) {
        const void* scope = const_cast<Graph*>(this)->scopeIdentity(callee->use(bytecode.m_scope));
        if (!scope)
            return likelyFunctionInModuleVariable(bytecode.m_var, bytecode.m_offset);
        if (scope != m_hints->variableScope())
            return nullptr;
        const KnownFunction* known = m_hints->find(name, bytecode.m_offset);
        if (known && isExact)
            *isExact = known->isExact;
        return known;
    }
    if (type == Dynamic)
        return nullptr;
    if (auto variable = const_cast<Graph*>(this)->resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type); variable.kind == StaticVariable::Import) {
        if (isExact && variable.import.function)
            *isExact = variable.import.function->isExact;
        return variable.import.function;
    } else if (variable.kind == StaticVariable::ModuleImport && m_linkage) {
        if (const StaticImport* import = m_linkage->findImport(name); import && import->function)
            return import->function;
    }
    if (m_declaredNames) {
        auto resolution = m_declaredNames->resolve(name);
        switch (resolution.kind) {
        case DeclaredNamesLink::Resolution::Slot: {
            if (!resolution.isInOutermostEnvironment)
                return nullptr;
            const KnownFunction* known = m_hints->find(name, resolution.offset);
            if (known && known->isExact && isExact)
                *isExact = const_cast<Graph*>(this)->scopeIdentity(callee->use(bytecode.m_scope)) == m_hints->variableScope();
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

const KnownFunction* Graph::likelyFunctionInModuleVariable(unsigned identifier, unsigned scopeOffset) const
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
    if (node->graph != this)
        return node->graph->passesNoFunctionObject(node);
    VirtualRegister calleeRegister;
    if (node->isBytecode(op_call))
        calleeRegister = node->as<OpCall>().m_callee;
    else if (node->isBytecode(op_call_ignore_result))
        calleeRegister = node->as<OpCallIgnoreResult>().m_callee;
    else if (node->isBytecode(op_tail_call))
        calleeRegister = node->as<OpTailCall>().m_callee;
    else
        return false;
    bool isExact = false;
    const KnownFunction* known = knownCallee(node, &isExact);
    if (!known || !isExact || !known->needsNoFunctionObject.load(std::memory_order_relaxed))
        return false;
    if (node->isBytecode(op_tail_call) && known->conventionForCall.signature == Signature::List)
        return false;
    if (closedMethodReadBy(node->use(calleeRegister))) {
        usesStaticImports = true;
        return true;
    }
    if (!node->use(calleeRegister)->isBytecode(op_get_from_scope))
        return false;
    return !!accessedEnvironmentDepth(node->use(calleeRegister));
}

uint32_t Graph::moduleEnvironmentDepth()
{
    RELEASE_ASSERT(m_scopeIsModuleEnvironment);
    usesStaticImports = true;
    return m_linkage->environmentDepth;
}

void Graph::findBuiltinsCalled()
{
    if (!ImmutableIntrinsics::shared())
        return;
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            if (node->kind != NodeKind::Bytecode || node->guard || node->guarded)
                continue;
            if (node->opcode != op_call && node->opcode != op_call_ignore_result && node->opcode != op_tail_call)
                continue;
            VirtualRegister calleeRegister;
            unsigned argv;
            if (node->opcode == op_call) {
                auto bytecode = node->as<OpCall>();
                calleeRegister = bytecode.m_callee, argv = bytecode.m_argv;
            } else if (node->opcode == op_call_ignore_result) {
                auto bytecode = node->as<OpCallIgnoreResult>();
                calleeRegister = bytecode.m_callee, argv = bytecode.m_argv;
            } else {
                auto bytecode = node->as<OpTailCall>();
                calleeRegister = bytecode.m_callee, argv = bytecode.m_argv;
            }
            Node* callee = node->use(calleeRegister);
            if (callee->kind == NodeKind::Intrinsic) {
                if (builtinAtIndex(callee->intrinsic) != Builtin::None)
                    node->builtinCalled = callee->intrinsic;
                continue;
            }
            if (!callee->isBytecode(op_get_by_id) || callee->guard || callee->guarded || callee->isElided || callee->isReadOnlyForCall)
                continue;
            auto read = callee->as<OpGetById>();
            Node* base = callee->use(read.m_base);
            int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
            if (node->use(VirtualRegister(firstArgument)) != base || !base->type)
                continue;
            const StringImpl& name = *callee->graph->codeBlock()->identifier(read.m_property).impl();
            Receiver receiver = receiverWithType(base->type);
            if (receiver == Receiver::None) {
                uint32_t tag = TypeTable::shared() ? typeTagOf(callee) : 0;
                if (tag && TypeTable::shared()->isArray(tag) && mayBe(base->type, TArray))
                    receiver = Receiver::Array;
                else if (Receiver hinted = tag ? static_cast<Receiver>(TypeTable::shared()->receiverHintOf(tag)) : Receiver::None; hinted != Receiver::None && hinted != Receiver::Number && mayBe(base->type, typeOf(hinted)))
                    receiver = hinted;
                else if (!tag || !TypeTable::shared()->isShape(tag))
                    receiver = likelyReceiverWith(base->type, name);
            }
            if (receiver == Receiver::None)
                continue;
            unsigned number = intrinsicFoundOn(receiver, name);
            if (!number || builtinAtIndex(number) == Builtin::None)
                continue;
            node->builtinCalled = number;
            node->builtinReceiver = static_cast<uint8_t>(receiver);
            callee->builtinCalled = number;
            callee->builtinReceiver = static_cast<uint8_t>(receiver);
        }
    }
}

bool Graph::isArrayIteratorMethodRead(const Node* node)
{
    if (!node->isBytecode(op_get_by_id) || node->guard)
        return false;
    auto bytecode = node->as<OpGetById>();
    Type base = node->use(bytecode.m_base)->type;
    return base && isSubtype(base, TArray) && node->graph->codeBlock()->identifier(bytecode.m_property).impl() == node->graph->vm().propertyNames->iteratorSymbol.impl();
}

bool Graph::isAnyArrayIteratorMethod(const Node* node)
{
    if (node->kind == NodeKind::LinkTimeConstant)
        return static_cast<LinkTimeConstant>(node->intrinsic) == LinkTimeConstant::arrayProtoValues;
    if (node->kind != NodeKind::Intrinsic)
        return false;
    auto number = intrinsicForLinkTimeConstant(jsNumber(static_cast<int32_t>(LinkTimeConstant::arrayProtoValues)));
    return number && node->intrinsic == *number;
}

void Graph::elideArrayIteratorMethodReads()
{
    UncheckedKeyHashMap<Node*, unsigned> otherUses;
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            if (isArrayIteratorMethodRead(node))
                otherUses.add(node, 0);
        }
    }
    if (otherUses.isEmpty())
        return;
    auto note = [&](Node* user, const Use& use) {
        auto found = otherUses.find(use.node);
        if (found == otherUses.end())
            return;
        bool compares = user->isBytecode(op_jstricteq) || user->isBytecode(op_jnstricteq) || user->isBytecode(op_stricteq) || user->isBytecode(op_nstricteq);
        if (compares) {
            for (auto& other : user->uses)
                compares &= other.node == use.node ? &other == &use : isAnyArrayIteratorMethod(other.node);
        }
        found->value += !compares;
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
    for (auto& [read, count] : otherUses) {
        if (!count)
            read->isElided = true;
    }
}

void Graph::sinkIteratorMethodReads()
{
    if (!Options::useUnboxedFastArrayIteration())
        return;
    Vector<Node*, 4> candidates;
    for (BasicBlock* block : m_rpo) {
        if (block->isGeneric)
            continue;
        Node* previous = nullptr;
        for (Node* node : block->nodes) {
            if (node->isElided)
                continue;
            if (node->isBytecode(op_iterator_open) && previous && previous->isBytecode(op_get_by_id) && !node->guard && !node->guarded && !previous->guard && !previous->guarded) {
                auto open = node->as<OpIteratorOpen>();
                auto read = previous->as<OpGetById>();
                Node* iterable = node->use(open.m_iterable);
                if (node->use(open.m_symbolIterator) == previous && previous->use(read.m_base) == iterable && mayBe(iterable->type, TArray)
                    && previous->graph->codeBlock()->identifier(read.m_property) == m_vm.propertyNames->iteratorSymbol) {
                    node->iteratorMethodRead = previous;
                    candidates.append(node);
                }
            }
            previous = node;
        }
    }
    if (candidates.isEmpty())
        return;
    UncheckedKeyHashMap<Node*, unsigned> uses;
    for (Node* open : candidates)
        uses.add(open->iteratorMethodRead, 0);
    auto note = [&](Node* user) {
        for (auto& use : user->uses) {
            if (auto found = uses.find(use.node); found != uses.end())
                found->value++;
        }
    };
    for (BasicBlock* block : m_rpo) {
        for (Node* phi : block->phis)
            note(phi);
        for (Node* node : block->nodes)
            note(node);
    }
    for (Node* open : candidates) {
        if (uses.get(open->iteratorMethodRead) == 1) {
            open->iteratorMethodRead->isElided = true;
            remark("sunk-iterator-method-read"_s);
        } else
            open->iteratorMethodRead = nullptr;
    }
}

void Graph::elideUnpassedCalleeReads()
{
    UncheckedKeyHashMap<Node*, unsigned> wanted;
    Vector<Node*, 16> reads;
    auto note = [&](Node* user, const Use& use) {
        bool isMethod = closedMethodReadBy(use.node);
        if (!use.node->isBytecode(op_get_from_scope) && !isMethod)
            return;
        bool wantsValue = true;
        if (user->isBytecode(op_check_tdz))
            wantsValue = mayBe(use.node->type, TEmpty);
        else if (user->isBytecode(op_call))
            wantsValue = use.reg != user->as<OpCall>().m_callee || !passesNoFunctionObject(user) || !(isMethod || knownCallee(user)->isDeclaration);
        else if (user->isBytecode(op_call_ignore_result))
            wantsValue = use.reg != user->as<OpCallIgnoreResult>().m_callee || !passesNoFunctionObject(user) || !(isMethod || knownCallee(user)->isDeclaration);
        else if (user->isBytecode(op_tail_call))
            wantsValue = use.reg != user->as<OpTailCall>().m_callee || !passesNoFunctionObject(user) || !(isMethod || knownCallee(user)->isDeclaration);
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
        if (read->isBytecode(op_get_by_id)) {
            if (closedMethodReadBy(read))
                read->isReadOnlyForCall = true;
            else
                read->isElided = true;
            continue;
        }
        bool isExact = false;
        read->isElided = knownFunctionReadBy(read, &isExact) && isExact;
    }
}

unsigned Graph::dissolvedScopesOutside(unsigned hops) const
{
    if (!m_variableSummaries || !m_declaredNames)
        return 0;
    unsigned count = 0;
    for (unsigned i = 0; i < hops; ++i) {
        if (const void* scope = m_declaredNames->scopeIdentity(i); scope && m_variableSummaries->isDissolved(scope))
            ++count;
    }
    return count;
}

const void* Graph::generatorFrameIdentity()
{
    if (m_generatorFrameIdentity)
        return *m_generatorFrameIdentity;
    m_generatorFrameIdentity = nullptr;
    if (!isGeneratorOrAsyncFunctionBodyParseMode(m_codeBlock->parseMode()))
        return nullptr;
    VirtualRegister frame = virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::Frame));
    const void* result = nullptr;
    for (const auto& instruction : m_codeBlock->instructions()) {
        VirtualRegister table;
        if (instruction->opcodeID() == op_create_lexical_environment && instruction->as<OpCreateLexicalEnvironment>().m_dst == frame)
            table = instruction->as<OpCreateLexicalEnvironment>().m_symbolTable;
        else if (instruction->opcodeID() == op_create_generator_frame_environment && instruction->as<OpCreateGeneratorFrameEnvironment>().m_dst == frame)
            table = instruction->as<OpCreateGeneratorFrameEnvironment>().m_symbolTable;
        else
            continue;
        JSValue constant = table.isConstant() ? m_codeBlock->getConstant(table) : JSValue();
        if (result || !constant || !constant.isCell())
            return nullptr;
        result = constant.asCell();
    }
    m_generatorFrameIdentity = result;
    return result;
}

unsigned Graph::dissolvedScopesAbove(const Node* scope, unsigned hops)
{
    if (scope->graph != this)
        return scope->graph->dissolvedScopesAbove(scope, hops);
    if (!m_variableSummaries || !hops)
        return 0;
    const void* identity = scopeIdentity(scope);
    unsigned count = 0;
    std::optional<unsigned> frame;
    for (unsigned i = 0; i < hops && identity; ++i) {
        const void* parent = nullptr;
        if (!frame) {
            for (BasicBlock* block : outermost().m_rpo) {
                for (Node* node : block->nodes) {
                    if (node->graph == this && node->isBytecode(op_create_lexical_environment) && scopeIdentity(node) == identity) {
                        Node* above = node->use(node->as<OpCreateLexicalEnvironment>().m_scope);
                        parent = scopeIdentity(above);
                        break;
                    }
                }
                if (parent)
                    break;
            }
        }
        if (!parent && m_declaredNames) {
            if (!frame) {
                for (unsigned index = 0; index < 64; ++index) {
                    const void* candidate = m_declaredNames->scopeIdentity(index);
                    if (!candidate)
                        break;
                    if (candidate == identity) {
                        frame = index;
                        break;
                    }
                }
            }
            if (frame) {
                frame = *frame + 1;
                parent = m_declaredNames->scopeIdentity(*frame);
            }
        }
        identity = parent;
        count += identity && m_variableSummaries->isDissolved(identity);
    }
    return count;
}

const void* Graph::scopeIdentity(const Node* scope, unsigned depth)
{
    if (scope->graph != this)
        return scope->graph->scopeIdentity(scope, depth);
    if (depth > 6)
        return nullptr;
    auto fromOutside = [&](unsigned hops) -> const void* {
        return m_declaredNames ? m_declaredNames->scopeIdentity(hops) : nullptr;
    };
    auto tableFor = [&](VirtualRegister reg) -> const void* {
        if (!reg.isConstant())
            return nullptr;
        JSValue table = m_codeBlock->getConstant(reg);
        return table && table.isCell() ? table.asCell() : nullptr;
    };
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
                if (!std::exchange(m_hasStoresToFrameRegisters, true)) {
                    for (BasicBlock* block : m_rpo) {
                        for (Node* store : block->nodes) {
                            if (store->kind == NodeKind::SetStack)
                                m_storesToFrameRegisters.add(store->reg.offset(), Vector<Node*>()).iterator->value.append(store);
                        }
                    }
                }
                auto it = m_storesToFrameRegisters.find(node->reg.offset());
                if (it == m_storesToFrameRegisters.end())
                    return nullptr;
                for (Node* store : it->value)
                    worklist.append(store->uses[0].node);
                break;
            }
            case NodeKind::Constant:
                break;
            case NodeKind::Argument: {
                const void* identity = node->reg == virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::Frame)) ? generatorFrameIdentity() : nullptr;
                if (!identity || (result && result != identity))
                    return nullptr;
                result = identity;
                break;
            }
            case NodeKind::Bytecode: {
                const void* identity = scopeIdentity(node, depth + 1);
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
            return tableFor(scope->as<OpCreateLexicalEnvironment>().m_symbolTable);
        case op_create_generator_frame_environment:
            return tableFor(scope->as<OpCreateGeneratorFrameEnvironment>().m_symbolTable);
        case op_get_scope:
            return fromOutside(0);
        case op_get_from_scope: {
            Variable restored = variableAccessedBy(scope);
            if (!restored || restored.scope != generatorFrameIdentity())
                return nullptr;
            const void* result = nullptr;
            for (BasicBlock* block : outermost().m_rpo) {
                for (Node* store : block->nodes) {
                    if (!store->isBytecode(op_put_to_scope))
                        continue;
                    Variable saved = variableAccessedBy(store);
                    if (saved.scope != restored.scope || saved.offset != restored.offset)
                        continue;
                    Node* value = store->use(store->as<OpPutToScope>().m_value);
                    if (value == scope || value->kind == NodeKind::Constant)
                        continue;
                    const void* identity = scopeIdentity(value, depth + 1);
                    if (!identity || (result && result != identity))
                        return nullptr;
                    result = identity;
                }
            }
            return result;
        }
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
                return scopeIdentity(inner->use(inner->as<OpCreateLexicalEnvironment>().m_scope), depth + 1);
            if (inner->isBytecode(op_create_generator_frame_environment))
                return scopeIdentity(inner->use(inner->as<OpCreateGeneratorFrameEnvironment>().m_scope), depth + 1);
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
    if (node->graph != this)
        return node->graph->variableAccessedBy(node);
    VirtualRegister scope;
    unsigned identifier;
    unsigned offset;
    unsigned localScopeDepth = 0;
    VirtualRegister scopeTable;
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
            scopeTable = bytecode.m_symbolTableOrScopeDepth.symbolTable();
    }
    switch (type) {
    case ResolvedClosureVar:
    case ResolvedLazyClosureVar:
        if (scopeTable.isValid() && scopeTable.isConstant()) {
            if (JSValue table = m_codeBlock->getConstant(scopeTable); table && table.isCell())
                return { table.asCell(), offset };
        }
        return { scopeIdentity(node->use(scope)), offset };
    case Dynamic:
        return { };
    default:
        break;
    }
    if (auto variable = resolveStatically(identifier, localScopeDepth, type); variable.kind == StaticVariable::Import)
        return { variable.import.scope, variable.import.scopeOffset };
    if (m_declaredNames) {
        auto resolution = m_declaredNames->resolve(m_codeBlock->identifier(identifier).impl());
        if (resolution.kind == DeclaredNamesLink::Resolution::Slot)
            return { resolution.scope, resolution.offset };
    }
    return { };
}

Node* Graph::constantCellOf(UnlinkedCodeBlock* owner, VirtualRegister reg)
{
    return m_constantCellsOfOtherCode.ensure({ owner, reg.offset() }, [&] {
        Node* node = addNode(NodeKind::ConstantCell);
        node->range = IntegerRange::unknown();
        node->reg = reg;
        node->ownerOfConstant = owner;
        JSValue value = owner->getConstant(reg);
        node->type = valueType(value);
        if (value.isString() && owner->codeType() == ModuleCode)
            node->type |= asString(value)->length() <= TypedLayoutTable::maxAtomizedStringLength ? TShortOtherString : TLongString;
        return node;
    }).iterator->value;
}

void Graph::recordObjectsInVariables(VariableSummaries& summaries)
{
    auto isAlias = [](const Node* node) {
        return node->kind == NodeKind::Narrow || node->isBytecode(op_check_type) || node->isBytecode(op_check_tdz) || node->isBytecode(op_type_tag);
    };
    auto skipAliases = [&](Node* node) {
        while (isAlias(node))
            node = node->uses[0].node;
        return node;
    };
    enum class UseKind : uint8_t { ReadsName, OnlyTests, NeedsObject, Escapes };
    auto onlyReadsArguments = [&](Node* call, VirtualRegister calleeRegister, unsigned argv, VirtualRegister argument) {
        Node* callee = call->use(calleeRegister);
        int position = argument.offset() - (-static_cast<int>(argv) + CallFrame::thisArgumentOffset());
        if (argument == calleeRegister)
            return false;
        if (callee->kind == NodeKind::LinkTimeConstant) {
            auto constant = static_cast<LinkTimeConstant>(callee->intrinsic);
            return (position == 2 && constant == LinkTimeConstant::copyDataProperties) || (!position && constant == LinkTimeConstant::cloneObject);
        }
        if (callee->kind != NodeKind::Intrinsic || position != 1)
            return false;
        auto* intrinsics = ImmutableIntrinsics::shared();
        auto& entry = intrinsics->at(callee->intrinsic);
        auto& holder = intrinsics->at(entry.holder);
        if (holder.holder != ImmutableIntrinsics::globalObject || holder.name != "Object"_s)
            return false;
        return entry.name == "keys"_s || entry.name == "values"_s || entry.name == "entries"_s || entry.name == "hasOwn"_s || entry.name == "getOwnPropertyNames"_s || entry.name == "isFrozen"_s;
    };
    auto kindOf = [&](Node* user, VirtualRegister reg) {
        if (user->kind != NodeKind::Bytecode || user->guard)
            return UseKind::Escapes;
        auto needsObjectIf = [](bool condition) { return condition ? UseKind::NeedsObject : UseKind::Escapes; };
        switch (user->opcode) {
        case op_get_by_id:
            return UseKind::ReadsName;
        case op_jundefined_or_null:
        case op_jnundefined_or_null:
        case op_jeq_null:
        case op_jneq_null:
        case op_eq_null:
        case op_neq_null:
        case op_is_undefined_or_null:
        case op_jtrue:
        case op_jfalse:
        case op_not:
            return UseKind::OnlyTests;
        case op_stricteq:
        case op_nstricteq:
        case op_jstricteq:
        case op_jnstricteq:
        case op_typeof:
            return UseKind::NeedsObject;
        case op_get_by_val:
            return needsObjectIf(reg == user->as<OpGetByVal>().m_base);
        case op_in_by_val:
            return needsObjectIf(reg == user->as<OpInByVal>().m_base);
        case op_in_by_id:
            return needsObjectIf(reg == user->as<OpInById>().m_base);
        case op_get_property_enumerator:
            return needsObjectIf(reg == user->as<OpGetPropertyEnumerator>().m_base);
        case op_enumerator_next:
            return needsObjectIf(reg == user->as<OpEnumeratorNext>().m_base);
        case op_enumerator_get_by_val:
            return needsObjectIf(reg == user->as<OpEnumeratorGetByVal>().m_base);
        case op_enumerator_in_by_val:
            return needsObjectIf(reg == user->as<OpEnumeratorInByVal>().m_base);
        case op_enumerator_has_own_property:
            return needsObjectIf(reg == user->as<OpEnumeratorHasOwnProperty>().m_base);
        case op_call:
            return needsObjectIf(onlyReadsArguments(user, user->as<OpCall>().m_callee, user->as<OpCall>().m_argv, reg));
        case op_call_ignore_result:
            return needsObjectIf(onlyReadsArguments(user, user->as<OpCallIgnoreResult>().m_callee, user->as<OpCallIgnoreResult>().m_argv, reg));
        default:
            return UseKind::Escapes;
        }
    };
    VariableSummaries::EscapingVariables escaping;
    VariableSummaries::NamesReadOfVariables namesRead;
    Vector<Variable, 4> needingObject;
    Vector<UniquedStringImpl*, 4> lookedUpFromUnknownScopes;
    UncheckedKeyHashMap<Node*, Vector<Node*, 2>> usersOfLiterals;
    auto nameOfUser = [](const Node* user) {
        return user->kind == NodeKind::Bytecode ? ASCIILiteral::fromLiteralUnsafe(opcodeNames[user->opcode]) : user->kind == NodeKind::Phi ? "meets-another-value"_s : "is-kept-in-the-frame"_s;
    };
    std::optional<UncheckedKeyHashSet<const Node*>> storesNeverReadFromFrame;
    auto isNeverReadFromFrame = [&](const Node* store) {
        if (!storesNeverReadFromFrame) {
            storesNeverReadFromFrame.emplace();
            for (BasicBlock* block : m_rpo) {
                BitVector readLater = block->readByHandlersAfterBlock;
                readLater.ensureSize(numRegisters());
                for (BasicBlock* successor : block->successors)
                    readLater.merge(successor->liveIn);
                for (unsigned i = block->nodes.size(); i--;) {
                    const Node* node = block->nodes[i];
                    if (node->kind == NodeKind::GetStack)
                        readLater.set(registerIndex(node->reg));
                    else if (node->kind == NodeKind::SetStack) {
                        unsigned index = registerIndex(node->reg);
                        if (!readLater.get(index) && !block->readByBlockHandlers.get(index))
                            storesNeverReadFromFrame->add(node);
                        readLater.clear(index);
                    } else if (readsOperandsFromFrame(node)) {
                        auto bytecode = node->as<OpNewArray>();
                        for (unsigned operand = 0; operand < bytecode.m_argc; ++operand)
                            readLater.set(registerIndex(VirtualRegister(bytecode.m_argv.offset() - static_cast<int>(operand))));
                    }
                }
            }
        }
        return storesNeverReadFromFrame->contains(store);
    };
    auto visit = [&](Node* user) {
        if (isAlias(user) || (user->kind == NodeKind::SetStack && isNeverReadFromFrame(user)))
            return;
        for (auto& use : user->uses) {
            Node* source = skipAliases(use.node);
            if (source->isBytecode(op_new_object)) {
                usersOfLiterals.add(source, Vector<Node*, 2> { }).iterator->value.append(user);
                continue;
            }
            if (!source->isBytecode(op_get_from_scope))
                continue;
            Variable variable = variableAccessedBy(source);
            if (!variable)
                continue;
            switch (kindOf(user, use.reg)) {
            case UseKind::ReadsName:
                namesRead.append({ variable, user->graph->codeBlock()->identifier(user->as<OpGetById>().m_property).impl() });
                break;
            case UseKind::OnlyTests:
                break;
            case UseKind::NeedsObject:
                needingObject.append(variable);
                break;
            case UseKind::Escapes:
                escaping.append({ variable, nameOfUser(user) });
                break;
            }
        }
    };
    std::optional<Vector<const void*, 4>> scopesMadeHere;
    auto noteLookupByName = [&](Node* node, unsigned identifier, unsigned offset, ResolveType type) {
        UniquedStringImpl* name = node->graph->codeBlock()->identifier(identifier).impl();
        const DeclaredNamesLink* declaredNames = node->graph->m_declaredNames;
        if (!declaredNames) {
            lookedUpFromUnknownScopes.append(name);
            return;
        }
        if (type == ResolvedClosureVar || type == ResolvedLazyClosureVar) {
            if (!scopesMadeHere) {
                scopesMadeHere.emplace();
                if (auto* module = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(m_codeBlock))
                    scopesMadeHere->append(module->getConstant(VirtualRegister(module->moduleEnvironmentSymbolTableConstantRegisterOffset())).asCell());
                for (BasicBlock* block : m_rpo) {
                    for (Node* made : block->nodes) {
                        if (made->isBytecode(op_create_lexical_environment) || made->isBytecode(op_create_generator_frame_environment))
                            scopesMadeHere->append(scopeIdentity(made));
                    }
                }
            }
            for (const void* scope : *scopesMadeHere) {
                if (scope)
                    escaping.append({ Variable { scope, offset }, "is-accessed-through-a-scope-that-cannot-be-told"_s });
                else
                    lookedUpFromUnknownScopes.append(name);
            }
            declaredNames->forEachSlotNamed(name, [&](const void* scope, unsigned slot) {
                escaping.append({ Variable { scope, slot }, "is-accessed-through-a-scope-that-cannot-be-told"_s });
            });
            return;
        }
        auto kind = declaredNames->resolve(name).kind;
        if (type != Dynamic && (kind == DeclaredNamesLink::Resolution::Global || kind == DeclaredNamesLink::Resolution::Stable))
            return;
        declaredNames->forEachSlotNamed(name, [&](const void* scope, unsigned offset) {
            escaping.append({ Variable { scope, offset }, "may-be-looked-up-by-name"_s });
        });
        if (type == Dynamic)
            lookedUpFromUnknownScopes.append(name);
    };
    Vector<Node*, 4> stores;
    for (BasicBlock* block : m_rpo) {
        for (Node* phi : block->phis)
            visit(phi);
        for (Node* node : block->nodes) {
            visit(node);
            if (node->isBytecode(op_put_to_scope)) {
                if (variableAccessedBy(node))
                    stores.append(node);
                else
                    noteLookupByName(node, node->as<OpPutToScope>().m_var, node->as<OpPutToScope>().m_offset, node->as<OpPutToScope>().m_getPutInfo.resolveType());
            } else if (node->isBytecode(op_get_from_scope)) {
                Variable variable = variableAccessedBy(node);
                if (!variable)
                    noteLookupByName(node, node->as<OpGetFromScope>().m_var, node->as<OpGetFromScope>().m_offset, node->as<OpGetFromScope>().m_getPutInfo.resolveType());
                else if (node->as<OpGetFromScope>().m_getPutInfo.resolveType() == ResolvedLazyClosureVar)
                    escaping.append({ variable, "may-hold-a-function-declaration"_s });
            }
        }
    }
    VariableSummaries::StoresToVariables storesToVariables;
    for (Node* store : stores) {
        Variable variable = variableAccessedBy(store);
        auto bytecode = store->as<OpPutToScope>();
        Node* object = skipAliases(store->use(bytecode.m_value));
        std::unique_ptr<VariableSummaries::ObjectLiteral> literal;
        if (object->isBytecode(op_new_object)) {
            UnlinkedCodeBlock* codeBlock = object->graph->codeBlock();
            auto& instructions = codeBlock->instructions();
            literal = makeUnique<VariableSummaries::ObjectLiteral>();
            literal->codeBlock = codeBlock;
            literal->variableName = store->graph->codeBlock()->identifier(bytecode.m_var).impl();
            bool hasPlainNames = true;
            auto add = [&](const OpPutById& property, Node* value) {
                UniquedStringImpl* name = codeBlock->identifier(property.m_property).impl();
                hasPlainNames &= !name->isSymbol() && !literal->names.contains(name);
                literal->names.append(name);
                VariableSummaries::ObjectLiteral::Value known;
                if (value->kind == NodeKind::Constant && value->constant)
                    known.immediate = value->constant;
                else if (value->kind == NodeKind::ConstantCell && value->reg.isConstant() && !value->ownerOfConstant && value->graph == object->graph && codeBlock->getConstant(value->reg).isString())
                    known.constantCell = value->reg;
                literal->values.append(known);
            };
            if (object->numberOfLiteralProperties) {
                auto& absorbed = object->graph->literalStores(object->bytecodeIndex.offset());
                for (unsigned i = 0; i < object->numberOfLiteralProperties; ++i)
                    add(instructions.at(absorbed[i])->as<OpPutById>(), object->use(NewObjectPlan::registerOf(i)));
            }
            if (object->numberOfLiteralProperties) {
                if (auto shape = literalShape(object); shape && !shape->layoutID && shape->slots.isEmpty() && shape->inlineCapacity <= std::numeric_limits<uint8_t>::max())
                    literal->inlineCapacity = shape->inlineCapacity;
            }
            for (Node* user : usersOfLiterals.find(object)->value) {
                if (user != store)
                    escaping.append({ variable, nameOfUser(user) });
            }
            if (!hasPlainNames)
                escaping.append({ variable, "the-literal-has-a-symbol-or-the-same-name-twice"_s });
        }
        storesToVariables.append({ variable, WTF::move(literal) });
    }
    if (!storesToVariables.isEmpty() || !escaping.isEmpty() || !namesRead.isEmpty() || !needingObject.isEmpty() || !lookedUpFromUnknownScopes.isEmpty())
        summaries.noteObjectsInVariables(WTF::move(storesToVariables), escaping, namesRead, needingObject.span(), lookedUpFromUnknownScopes.span());
}

void Graph::recordUntrackableVariableAccesses(VariableSummaries& summaries)
{
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            if (node->kind != NodeKind::Bytecode)
                continue;
            switch (node->opcode) {
            case op_put_to_scope: {
                if (variableAccessedBy(node))
                    break;
                auto bytecode = node->as<OpPutToScope>();
                UniquedStringImpl* name = m_codeBlock->identifier(bytecode.m_var).impl();
                ResolveType type = bytecode.m_getPutInfo.resolveType();
                bool isOnHolder = false;
                if (type != ResolvedClosureVar && type != ResolvedLazyClosureVar && type != Dynamic && m_declaredNames) {
                    auto kind = m_declaredNames->resolve(name).kind;
                    isOnHolder = kind == DeclaredNamesLink::Resolution::Global || kind == DeclaredNamesLink::Resolution::Stable;
                }
                if (!isOnHolder)
                    summaries.giveUpOnName(name);
                break;
            }
            case op_get_from_scope: {
                if (variableAccessedBy(node))
                    break;
                auto bytecode = node->as<OpGetFromScope>();
                UniquedStringImpl* name = m_codeBlock->identifier(bytecode.m_var).impl();
                ResolveType type = bytecode.m_getPutInfo.resolveType();
                bool isOnHolder = false;
                if (type != ResolvedClosureVar && type != ResolvedLazyClosureVar && type != Dynamic && m_declaredNames)
                    isOnHolder = m_declaredNames->resolve(name).kind == DeclaredNamesLink::Resolution::Global;
                if (!isOnHolder)
                    summaries.recordDynamicNameRead(name);
                break;
            }
            case op_create_scoped_arguments:
                if (const void* scope = scopeIdentity(node->use(node->as<OpCreateScopedArguments>().m_scope)))
                    summaries.giveUpOnScope(scope);
                else {
                    for (auto& identifier : m_codeBlock->identifiers())
                        summaries.giveUpOnName(identifier.impl());
                }
                break;
            case op_call_direct_eval:
                if (m_declaredNames) {
                    m_declaredNames->forEachScope([&](const void* scope) {
                        dataLogLnIf(Options::logAOTTypeInference(), "AOT inference: direct eval gives up on scope ", RawPointer(scope));
                        summaries.giveUpOnScope(scope);
                    });
                } else {
                    dataLogLnIf(Options::logAOTTypeInference(), "AOT inference: direct eval in code that does not know what is declared around it gives up on every scope");
                    summaries.giveUpOnAllScopes();
                }
                for (BasicBlock* other : m_rpo) {
                    for (Node* made : other->nodes) {
                        if (made->isBytecode(op_create_lexical_environment) || made->isBytecode(op_create_generator_frame_environment)) {
                            if (const void* scope = scopeIdentity(made))
                                summaries.giveUpOnScope(scope);
                        }
                    }
                }
                for (auto& identifier : m_codeBlock->identifiers())
                    summaries.giveUpOnName(identifier.impl());
                break;
            default:
                break;
            }
        }
    }
}

bool Graph::hasOverriddenMethodInfo()
{
    return !!Options::aotOverriddenMethodsPath();
}

bool Graph::methodMayBeOverridden(ASCIILiteral className, Node* read)
{
    static const NeverDestroyed<std::optional<UncheckedKeyHashSet<String>>> all = [] () -> std::optional<UncheckedKeyHashSet<String>> {
        if (!Options::aotOverriddenMethodsPath())
            return std::nullopt;
        String path { Options::aotOverriddenMethodsPath() };
        auto contents = FileSystem::readEntireFile(path);
        if (!contents) {
            dataLogLn("AOT: ", path, " cannot be read");
            return std::nullopt;
        }
        UncheckedKeyHashSet<String> result;
        for (auto line : String::fromUTF8(contents->span()).split('\n'))
            result.add(line);
        return result;
    }();
    if (!all.get() || !read || !read->isBytecode(op_get_by_id))
        return true;
    UniquedStringImpl* name = read->graph->codeBlock()->identifier(read->as<OpGetById>().m_property).impl();
    StringView text(name);
    if (name->isSymbol()) {
        if (!text.startsWith("Symbol."_s))
            return true;
        return all.get()->contains(makeString(className, ".@@"_s, text.substring(7))) || all.get()->contains(makeString(className, ".*"_s));
    }
    return all.get()->contains(makeString(className, '.', text)) || all.get()->contains(makeString(className, ".*"_s));
}

PropertyEffect Graph::propertyEffectOf(const Node* node)
{
    switch (node->kind) {
    case NodeKind::Constant:
    case NodeKind::ConstantCell:
    case NodeKind::Intrinsic:
    case NodeKind::LinkTimeConstant:
    case NodeKind::Argument:
    case NodeKind::Phi:
    case NodeKind::Proj:
    case NodeKind::GetStack:
    case NodeKind::SetStack:
    case NodeKind::Narrow:
        return PropertyEffect::None;
    case NodeKind::Guard:
        return PropertyEffect::Arbitrary;
    case NodeKind::Bytecode:
        break;
    }
    if (node->guard)
        return PropertyEffect::Arbitrary;
    auto isVariable = [](ResolveType type) {
        return type == ClosureVar || type == ResolvedClosureVar || type == LazyClosureVar || type == ResolvedLazyClosureVar || type == ModuleVar || type == GlobalLexicalVar;
    };
    auto unlessOperandMayBeObject = [&] {
        for (auto& use : node->uses) {
            if (!use.node->type || mayBe(use.node->type, TAnyObject))
                return PropertyEffect::OnSlowPathOnly;
        }
        return PropertyEffect::None;
    };
    switch (node->opcode) {
    case op_enter:
    case op_nop:
    case op_mov:
    case op_type_tag:
    case op_check_type:
    case op_check_tdz:
    case op_check_traps:
    case op_loop_hint:
    case op_get_scope:
    case op_get_parent_scope:
    case op_argument_count:
    case op_get_argument:
    case op_to_this:
    case op_typeof:
    case op_typeof_is_undefined:
    case op_typeof_is_object:
    case op_typeof_is_function:
    case op_not:
    case op_unsigned:
    case op_stricteq:
    case op_nstricteq:
    case op_jstricteq:
    case op_jnstricteq:
    case op_below:
    case op_beloweq:
    case op_jbelow:
    case op_jbeloweq:
    case op_jmp:
    case op_jtrue:
    case op_jfalse:
    case op_jeq_null:
    case op_jneq_null:
    case op_jundefined_or_null:
    case op_jnundefined_or_null:
    case op_jeq_ptr:
    case op_jneq_ptr:
    case op_eq_null:
    case op_neq_null:
    case op_is_undefined_or_null:
    case op_is_boolean:
    case op_is_number:
    case op_is_big_int:
    case op_is_object:
    case op_is_callable:
    case op_is_constructor:
    case op_is_cell_with_type:
    case op_is_empty:
    case op_switch_imm:
    case op_switch_char:
    case op_switch_string:
    case op_new_object:
    case op_new_array:
    case op_new_array_buffer:
    case op_new_func:
    case op_new_func_exp:
    case op_new_reg_exp:
    case op_create_lexical_environment:
    case op_create_rest:
    case op_throw:
    case op_throw_static_error:
    case op_ret:
        return PropertyEffect::None;
    case op_resolve_scope:
        return isVariable(node->as<OpResolveScope>().m_resolveType) ? PropertyEffect::None : PropertyEffect::OnSlowPathOnly;
    case op_get_from_scope:
        return isVariable(node->as<OpGetFromScope>().m_getPutInfo.resolveType()) ? PropertyEffect::None : PropertyEffect::OnSlowPathOnly;
    case op_put_to_scope:
        return isVariable(node->as<OpPutToScope>().m_getPutInfo.resolveType()) ? PropertyEffect::None : PropertyEffect::Arbitrary;
    case op_add:
    case op_sub:
    case op_mul:
    case op_div:
    case op_mod:
    case op_pow:
    case op_negate:
    case op_inc:
    case op_dec:
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
    case op_eq:
    case op_neq:
    case op_jeq:
    case op_jneq:
        return unlessOperandMayBeObject();
    case op_get_by_id:
    case op_get_length:
    case op_get_by_val:
    case op_in_by_id:
    case op_in_by_val:
    case op_instanceof:
    case op_create_this:
        return PropertyEffect::OnSlowPathOnly;
    case op_put_by_val: {
        Type key = node->use(node->as<OpPutByVal>().m_property)->type;
        return key && isSubtype(key, TInt32) ? PropertyEffect::OnSlowPathOnly : PropertyEffect::Arbitrary;
    }
    case op_put_by_id:
        return PropertyEffect::StoresNamedProperty;
    case op_call:
    case op_call_ignore_result:
    case op_tail_call:
        return PropertyEffect::Call;
    default:
        return PropertyEffect::Arbitrary;
    }
}

const FunctionSummary::PropertyEffects* Graph::propertyEffectsOfCall(const Node* node) const
{
    bool isExact = false;
    const KnownFunction* known = knownCallee(node, &isExact);
    if (!known || !isExact || !known->forCall || !known->summary || known->summary->propertyEffects.isArbitrary)
        return nullptr;
    return &known->summary->propertyEffects;
}

void Graph::recordPropertyEffects() const
{
    if (!m_summary || m_codeBlock->isConstructor())
        return;
    FunctionSummary::PropertyEffects effects;
    effects.isArbitrary = false;
    const Node* culprit = nullptr;
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            if (effects.isArbitrary)
                break;
            culprit = node;
            switch (propertyEffectOf(node)) {
            case PropertyEffect::None:
            case PropertyEffect::OnSlowPathOnly:
                break;
            case PropertyEffect::StoresNamedProperty: {
                UniquedStringImpl* name = node->graph->codeBlock()->identifier(node->as<OpPutById>().m_property).impl();
                if (!effects.storedNames.contains(name))
                    effects.storedNames.append(name);
                break;
            }
            case PropertyEffect::Call: {
                bool isExact = false;
                const KnownFunction* known = knownCallee(node, &isExact);
                if (!known || !isExact || !known->forCall || !known->summary)
                    effects.isArbitrary = true;
                else if (known->summary != m_summary && !effects.callees.contains(known->summary))
                    effects.callees.append(known->summary);
                break;
            }
            case PropertyEffect::Arbitrary:
                effects.isArbitrary = true;
                break;
            }
        }
    }
    if (effects.storedNames.size() > FunctionSummary::PropertyEffects::maxStoredNames)
        effects.isArbitrary = true;
    if (Options::logAOTTypeInference()) [[unlikely]] {
        if (effects.isArbitrary && culprit)
            dataLogLn("AOT inference: `", m_nameForLog, "` may change any property: ", culprit->kind == NodeKind::Bytecode ? opcodeNames[culprit->opcode] : "a guard"_s, " bc#", culprit->bytecodeIndex.offset());
        else
            dataLogLn("AOT inference: `", m_nameForLog, "` itself stores to ", effects.storedNames.size(), " names and calls ", effects.callees.size(), " functions");
    }
    m_summary->propertyEffects = WTF::move(effects);
}

void Graph::recordKnownFunctionUses(const FunctionSummaryMap& summariesByExecutable, const FunctionSummary* currentSummary)
{
    auto calleeRegisterOf = [](Node* user) -> std::optional<VirtualRegister> {
        if (user->isBytecode(op_call))
            return user->as<OpCall>().m_callee;
        if (user->isBytecode(op_call_ignore_result))
            return user->as<OpCallIgnoreResult>().m_callee;
        if (user->isBytecode(op_tail_call))
            return user->as<OpTailCall>().m_callee;
        return std::nullopt;
    };
    auto isPerElementCallback = [&](Node* user, const Use& use) {
        auto calleeRegister = calleeRegisterOf(user);
        if (!calleeRegister || use.reg == *calleeRegister)
            return false;
        Node* callee = user->use(*calleeRegister);
        if (!callee->isBytecode(op_get_by_id))
            return false;
        StringView name { m_codeBlock->identifier(callee->as<OpGetById>().m_property).impl() };
        for (ASCIILiteral method : { "map"_s, "filter"_s, "some"_s, "every"_s, "find"_s, "findIndex"_s, "findLast"_s, "findLastIndex"_s, "forEach"_s, "reduce"_s, "reduceRight"_s, "flatMap"_s, "sort"_s, "toSorted"_s }) {
            if (name == method)
                return true;
        }
        return false;
    };
    auto executableCreatedBy = [&](Node* node) -> UnlinkedFunctionExecutable* {
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
    auto note = [&](BasicBlock* block, Node* user, const Use& use) {
        if (auto* executable = executableCreatedBy(use.node)) {
            auto* summary = summariesByExecutable.get(executable);
            if (!summary)
                return;
            if (!(user->isBytecode(op_put_to_scope) && use.reg == user->as<OpPutToScope>().m_value))
                summary->valueIsUsed.store(true, std::memory_order_relaxed);
            if (isPerElementCallback(user, use) || (block->isInLoop && calleeRegisterOf(user) == use.reg))
                summary->isCalledRepeatedly.store(true, std::memory_order_relaxed);
            return;
        }
        if (!use.node->isBytecode(op_get_from_scope))
            return;
        bool readIsExact = false;
        const KnownFunction* known = knownFunctionReadBy(use.node, &readIsExact);
        if (!known || !known->summary)
            return;
        if (user->isBytecode(op_check_tdz) || user->isBytecode(op_jundefined_or_null) || user->isBytecode(op_jnundefined_or_null))
            return;
        bool isCallee = calleeRegisterOf(user) == use.reg;
        if (isCallee) {
            if (block->isInLoop)
                known->summary->isCalledRepeatedly.store(true, std::memory_order_relaxed);
            else if (currentSummary) {
                Locker locker { currentSummary->directCalleesLock };
                currentSummary->directCallees.append(known->summary);
            }
        } else if (isPerElementCallback(user, use))
            known->summary->isCalledRepeatedly.store(true, std::memory_order_relaxed);
        if (isCallee && readIsExact && known->forCall && knownCallee(user) == known && calleeIsExact(user)) {
            known->summary->directCalls.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        known->summary->valueIsUsed.store(true, std::memory_order_relaxed);
    };
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            for (auto& use : node->uses)
                note(block, node, use);
        }
        for (Node* phi : block->phis) {
            for (auto& use : phi->uses)
                note(block, phi, use);
        }
    }
}

void Graph::noteFieldsComparedWithStrings()
{
    if (!Options::useAOTTypedFields() || !TypeTable::typedFieldsAreEnforced())
        return;
    auto isProgramString = [](Node* node) {
        if (node->kind != NodeKind::ConstantCell || !node->reg.isConstant())
            return false;
        JSValue constant = node->codeBlockOfConstant()->getConstant(node->reg);
        return constant && constant.isString();
    };
    auto note = [&](auto& self, Node* node, unsigned depth) -> void {
        if (depth > 4)
            return;
        switch (node->kind) {
        case NodeKind::Narrow:
            self(self, node->uses[0].node, depth + 1);
            return;
        case NodeKind::Phi:
            for (auto& use : node->uses) {
                if (use.node != node)
                    self(self, use.node, depth + 1);
            }
            return;
        case NodeKind::Bytecode:
            if (node->opcode == op_check_type)
                self(self, node->use(node->as<OpCheckType>().m_value), depth + 1);
            else if (node->opcode == op_check_tdz)
                self(self, node->uses[0].node, depth + 1);
            else if (node->opcode == op_get_by_id) {
                if (auto field = typedFieldAccessedBy(node))
                    TypeTable::shared()->noteComparedWithString(*field);
            }
            return;
        default:
            return;
        }
    };
    auto compared = [&](Node* node, VirtualRegister lhs, VirtualRegister rhs) {
        Node* left = node->use(lhs);
        Node* right = node->use(rhs);
        if (isProgramString(right))
            note(note, left, 0);
        else if (isProgramString(left))
            note(note, right, 0);
    };
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            if (node->kind != NodeKind::Bytecode)
                continue;
            switch (node->opcode) {
#define AOT_COMPARISON(Struct, opcodeName) \
            case opcodeName: \
                compared(node, node->as<Struct>().m_lhs, node->as<Struct>().m_rhs); \
                break;
            AOT_COMPARISON(OpStricteq, op_stricteq)
            AOT_COMPARISON(OpNstricteq, op_nstricteq)
            AOT_COMPARISON(OpEq, op_eq)
            AOT_COMPARISON(OpNeq, op_neq)
            AOT_COMPARISON(OpJstricteq, op_jstricteq)
            AOT_COMPARISON(OpJnstricteq, op_jnstricteq)
            AOT_COMPARISON(OpJeq, op_jeq)
            AOT_COMPARISON(OpJneq, op_jneq)
#undef AOT_COMPARISON
            case op_switch_string:
                note(note, node->use(node->as<OpSwitchString>().m_scrutinee), 0);
                break;
            default:
                break;
            }
        }
    }
}

Node* Graph::argumentListFor(const Node* node)
{
    if (node->kind != NodeKind::Bytecode)
        return nullptr;
    auto forAllOperands = [&](auto bytecode) -> Node* {
        return bytecode.m_firstVarArg || !bytecode.m_arguments.isValid() ? nullptr : node->use(bytecode.m_arguments);
    };
    switch (node->opcode) {
    case op_call_varargs:
        return forAllOperands(node->as<OpCallVarargs>());
    case op_tail_call_varargs:
        return forAllOperands(node->as<OpTailCallVarargs>());
    case op_construct_varargs:
        return forAllOperands(node->as<OpConstructVarargs>());
    case op_super_construct_varargs:
        return forAllOperands(node->as<OpSuperConstructVarargs>());
    default:
        return nullptr;
    }
}

void Graph::findArgumentLists()
{
    UncheckedKeyHashMap<Node*, unsigned> numberOfUses;
    auto note = [&](Node* user) {
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

    UncheckedKeyHashMap<Node*, unsigned> numberOfAliasingUses;
    for (BasicBlock* block : m_rpo) {
        for (unsigned index = 0; index < block->nodes.size(); ++index) {
            Node* list = argumentListFor(block->nodes[index]);
            if (!list)
                continue;
            if (list->isBytecode(op_create_cloned_arguments)) {
                ++numberOfAliasingUses.add(list, 0).iterator->value;
                continue;
            }
            Vector<Node*, 4> parts;
            if (list->isBytecode(op_spread))
                parts.append(list);
            else if (list->isBytecode(op_new_array_with_spread)) {
                auto bytecode = list->as<OpNewArrayWithSpread>();
                if (bytecode.m_argc > maxListItems)
                    continue;
                parts.append(list);
                for (unsigned i = bytecode.m_argc; i--;) {
                    Node* element = list->use(VirtualRegister(bytecode.m_argv.offset() - static_cast<int>(i)));
                    if (element->isBytecode(op_spread))
                        parts.append(element);
                }
            } else
                continue;
            bool immediatelyPrecedeCall = parts.size() <= index;
            for (unsigned i = 0; immediatelyPrecedeCall && i < parts.size(); ++i)
                immediatelyPrecedeCall = block->nodes[index - 1 - i] == parts[i] && numberOfUses.get(parts[i]) == 1;
            if (!immediatelyPrecedeCall)
                continue;
            for (Node* part : parts) {
                part->isElided = true;
                if (!part->isBytecode(op_spread))
                    continue;
                Node* spread = part->use(part->as<OpSpread>().m_argument);
                if (spread->isBytecode(op_create_rest))
                    ++numberOfAliasingUses.add(spread, 0).iterator->value;
            }
        }
    }

    for (auto& [node, uses] : numberOfAliasingUses)
        node->isElided = uses == numberOfUses.get(node);

    for (BasicBlock* block : m_rpo) {
        for (unsigned index = 0; index < block->nodes.size(); ++index) {
            Node* array = block->nodes[index];
            if (!array->isBytecode(op_new_array_with_spread) || array->isElided)
                continue;
            auto bytecode = array->as<OpNewArrayWithSpread>();
            if (bytecode.m_argc < 2 || bytecode.m_argc > 32)
                continue;
            for (unsigned before = index; before--;) {
                Node* spread = block->nodes[before];
                if (!spread->isBytecode(op_spread) || spread->isElided || numberOfUses.get(spread) != 1)
                    break;
                bool belongsToThisArray = false;
                for (auto& use : array->uses)
                    belongsToThisArray |= use.node == spread;
                if (!belongsToThisArray)
                    break;
                spread->isElided = true;
            }
        }
    }
}

void Graph::noteSiteSelector(unsigned slot, UniquedStringImpl* name)
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

void Graph::noteSiteShape(unsigned slot, KnownShape&& shape)
{
    while (siteConstants.size() <= slot)
        siteConstants.append(0);
    shapes.append(WTF::move(shape));
    siteConstants[slot] = shapes.size() | CompiledFunctionInfo::siteConstantIsShape;
}

void Graph::noteSitePlan(unsigned firstSlot, Vector<uint32_t, 16>&& words)
{
    while (siteConstants.size() <= firstSlot + 1)
        siteConstants.append(0);
    siteConstants[firstSlot + 1] = (plans.size() + 1) | CompiledFunctionInfo::siteConstantIsPlan;
    plans.appendVector(words);
}

bool Graph::isEscapingFunctionThis(const Node* node)
{
    while (node->kind == NodeKind::Narrow || node->isBytecode(op_to_this) || node->isBytecode(op_check_type) || node->isBytecode(op_type_tag))
        node = node->uses[0].node;
    if (node->kind != NodeKind::Argument || node->reg != virtualRegisterForArgumentIncludingThis(0))
        return false;
    const FunctionSummary* summary = node->graph->summary();
    return !summary || !summary->isNonEscaping;
}

std::optional<TypeTable::Field> Graph::typedFieldAccessedBy(const Node* node)
{
    if (!Options::useAOTTypedFields() || !TypeTable::typedFieldsAreEnforced() || node->guard)
        return std::nullopt;
    if (node->onlyChecksConstantObject || node->slotInConstantObjectPlusOne)
        return std::nullopt;
    uint32_t tag = typeTagOf(node);
    unsigned identifier;
    VirtualRegister base;
    if (node->isBytecode(op_get_by_id) && (Options::aotShapeOptimizations() & 2)) {
        identifier = node->as<OpGetById>().m_property;
        base = node->as<OpGetById>().m_base;
    } else if (node->isBytecode(op_put_by_id) && (Options::aotShapeOptimizations() & 4)) {
        identifier = node->as<OpPutById>().m_property;
        base = node->as<OpPutById>().m_base;
    } else
        return std::nullopt;
    UniquedStringImpl* name = node->graph->codeBlock()->identifier(identifier).impl();
    if (tag) {
        if (auto field = TypeTable::shared()->fieldOf(tag, name))
            return field;
    }
    return typedBaseField(node->use(base), name);
}

std::optional<TypeTable::Field> Graph::typedBaseField(const Node* base, UniquedStringImpl* name)
{
    if (!Options::useAOTTypedFields() || !TypeTable::typedFieldsAreEnforced() || !base->type || !isSubtype(base->type, TFinalObject))
        return std::nullopt;
    auto layoutIDs = layoutRangeOf(base->type);
    if (!layoutIDs.lowest || layoutIDs.lowest != layoutIDs.highest)
        return std::nullopt;
    return TypeTable::shared()->layoutField(layoutIDs.lowest, name);
}

uint32_t Graph::classRecordedBy(const Node* node)
{
    if (!node->isBytecode(op_call_ignore_result) || !Options::useAOTTypedFields() || !TypeTable::hasTypedFields())
        return 0;
    CallOperands operands = callOperands(node->instruction);
    if (operands.argc != 2 || linkTimeConstantOf(node->use(operands.callee)) != LinkTimeConstant::noteClass)
        return 0;
    uint32_t tag = typeTagOf(node);
    return tag && TypeTable::shared()->isClass(tag) ? tag : 0;
}

static UnlinkedFunctionExecutable* functionCreatedBy(const Node* node)
{
    if (node->kind != NodeKind::Bytecode)
        return nullptr;
    UnlinkedCodeBlock* code = node->graph->codeBlock();
    switch (node->opcode) {
    case op_new_func_exp:
        return code->functionExpr(node->as<OpNewFuncExp>().m_functionDecl);
    case op_new_generator_func_exp:
        return code->functionExpr(node->as<OpNewGeneratorFuncExp>().m_functionDecl);
    case op_new_async_func_exp:
        return code->functionExpr(node->as<OpNewAsyncFuncExp>().m_functionDecl);
    case op_new_async_generator_func_exp:
        return code->functionExpr(node->as<OpNewAsyncGeneratorFuncExp>().m_functionDecl);
    default:
        return nullptr;
    }
}

const KnownFunction* Graph::functionMadeBy(const Node* node)
{
    if (!programFunctions() || node->kind != NodeKind::Bytecode)
        return nullptr;
    UnlinkedCodeBlock* code = node->graph->codeBlock();
    UnlinkedFunctionExecutable* made = nullptr;
    switch (node->opcode) {
    case op_new_func:
        made = code->functionDecl(node->as<OpNewFunc>().m_functionDecl);
        break;
    case op_new_generator_func:
        made = code->functionDecl(node->as<OpNewGeneratorFunc>().m_functionDecl);
        break;
    case op_new_async_func:
        made = code->functionDecl(node->as<OpNewAsyncFunc>().m_functionDecl);
        break;
    case op_new_async_generator_func:
        made = code->functionDecl(node->as<OpNewAsyncGeneratorFunc>().m_functionDecl);
        break;
    default:
        made = functionCreatedBy(node);
        break;
    }
    return made ? programFunctions()->function(programFunctions()->numberOf(made)) : nullptr;
}

void Graph::noteClassesDefined()
{
    ProgramClasses* classes = programClasses();
    const ProgramFunctions* functions = programFunctions();
    if (!classes || !functions || !TypeTable::typedFieldsAreEnforced())
        return;
    for (BasicBlock* block : m_rpo) {
        for (Node* note : block->nodes) {
            uint32_t classType = classRecordedBy(note);
            if (!classType)
                continue;
            CallOperands operands = callOperands(note->instruction);
            Node* constructor = note->use(operands.argument(0));
            Node* prototype = note->use(operands.argument(1));
            uint16_t layoutID = TypeTable::shared()->instanceLayoutIDFor(classType);
            auto recordThisUseIn = [&](Node* value) -> uint32_t {
                UnlinkedFunctionExecutable* made = functionCreatedBy(value);
                uint32_t number = made ? functions->numberOf(made) : 0;
                if (const KnownFunction* function = functions->function(number)) {
                    classes->noteThisIn(function->forCall, layoutID);
                    classes->noteThisIn(function->forConstruct, layoutID);
                }
                return number;
            };
            recordThisUseIn(constructor);
            for (BasicBlock* otherBlock : m_rpo) {
                for (Node* node : otherBlock->nodes) {
                    if (node->isBytecode(op_define_data_property)) {
                        auto bytecode = node->as<OpDefineDataProperty>();
                        if (node->use(bytecode.m_base) != prototype)
                            continue;
                        uint32_t number = recordThisUseIn(node->use(bytecode.m_value));
                        Node* property = node->use(bytecode.m_property);
                        if (!number || property->kind != NodeKind::ConstantCell || !property->reg.isConstant())
                            continue;
                        JSValue name = property->codeBlockOfConstant()->getConstant(property->reg);
                        const StringImpl* impl = name && name.isString() ? asString(name)->tryGetValueImpl() : nullptr;
                        if (!impl || !impl->isAtom())
                            continue;
                        auto* uid = static_cast<UniquedStringImpl*>(const_cast<StringImpl*>(impl));
                        if (TypeTable::shared()->isNonEscapingMethod(classType, uid))
                            classes->recordNonEscapingMethod(classType, uid, number);
                    } else if (node->isBytecode(op_put_by_id)) {
                        auto bytecode = node->as<OpPutById>();
                        if (node->use(bytecode.m_base) == constructor && node->graph->codeBlock()->identifier(bytecode.m_property) == m_vm.propertyNames->builtinNames().instanceFieldInitializerPrivateName())
                            recordThisUseIn(node->use(bytecode.m_value));
                    }
                }
            }
        }
    }
}

void Graph::addRemark(ASCIILiteral what, StringView detail, bool isOffUsualPath)
{
    String text = detail.isNull() ? String(what) : makeString(what, ':', detail);
    if (m_isCoveringOperation) {
        auto& outcomes = coverage.last().outcomes;
        String outcome = isOffUsualPath ? makeString("rarely-"_s, text) : text;
        if (!outcomes.contains(outcome))
            outcomes.append(WTF::move(outcome));
    }
    if (!remarks.contains(text))
        remarks.append(WTF::move(text));
}

void Graph::beginCoveredOperation(const Node* node, const BasicBlock* block, bool isElided)
{
    m_isCoveringOperation = false;
    if (!node || node->kind != NodeKind::Bytecode || !node->instruction || node->instruction->opcodeID() != node->opcode)
        return;
    auto& instructions = node->graph->codeBlock()->instructions();
    size_t ownOffset = reinterpret_cast<uintptr_t>(node->instruction) - reinterpret_cast<uintptr_t>(instructions.at(0).ptr());
    if (ownOffset >= instructions.size())
        return;
    uint8_t flags = 0;
    if (block->isGeneric)
        flags |= CoveredOperation::isInGenericCopy;
    if (block->isRarelyExecuted)
        flags |= CoveredOperation::isRarelyExecuted;
    if (block->isInLoop)
        flags |= CoveredOperation::isInLoop;
    if (node->graph != this)
        flags |= CoveredOperation::isInlined;
    if (isElided)
        flags |= CoveredOperation::isElided;
    auto append = [&](unsigned offset) -> CoveredOperation& {
        coverage.append(node->graph->coveredOperationAt(offset, flags));
        return coverage.last();
    };
    if (node->numberOfLiteralProperties) {
        if (node->opcode == op_new_object) {
            for (unsigned offset : node->graph->literalStores(node->bytecodeIndex.offset()))
                append(offset).outcomes.append("absorbed-into-allocation"_s);
        } else if (node->opcode == op_create_this) {
            for (auto& store : NewObjectPlan::forCreateThis(node->graph->codeBlock()->instructions(), node->bytecodeIndex.offset()).stores)
                append(store.offset).outcomes.append("absorbed-into-allocation"_s);
        }
    }
    append(ownOffset);
    m_isCoveringOperation = !isElided;
}

CoveredOperation Graph::coveredOperationAt(unsigned offset, uint8_t flags)
{
    auto instruction = codeBlock()->instructions().at(offset);
    CoveredOperation operation;
    operation.codeBlock = codeBlock();
    operation.bytecodeOffset = offset;
    operation.opcode = instruction->opcodeID();
    operation.flags = flags;
    if (Options::useAOTTypeCoverageCounters())
        operation.counter = (firstTypeCoverageCounter + offset) * CoveredOperation::countersPerOperation;
    if (uint32_t tag = typeTagAt(offset)) {
        operation.flags |= CoveredOperation::hasTypeTag;
        if (TypeTable::shared()) {
            if (auto reason = TypeTable::shared()->reasonForNoType(tag))
                operation.reason = *reason;
        }
    }
    std::optional<unsigned> identifier;
    switch (instruction->opcodeID()) {
    case op_get_by_id:
        identifier = instruction->as<OpGetById>().m_property;
        break;
    case op_put_by_id:
        identifier = instruction->as<OpPutById>().m_property;
        break;
    case op_in_by_id:
        identifier = instruction->as<OpInById>().m_property;
        break;
    default:
        break;
    }
    if (identifier)
        operation.property = codeBlock()->identifier(*identifier).string();
    return operation;
}

uint32_t Graph::closedMethodReadBy(const Node* node)
{
    if (!node->isBytecode(op_get_by_id) || !Options::useAOTTypedFields() || !TypeTable::typedFieldsAreEnforced() || !programClasses())
        return 0;
    uint32_t tag = typeTagOf(node);
    if (!tag)
        return 0;
    UniquedStringImpl* name = node->graph->codeBlock()->identifier(node->as<OpGetById>().m_property).impl();
    uint32_t classType = TypeTable::shared()->methodClassReadBy(tag, name);
    return classType ? programClasses()->closedMethod(classType, name) : 0;
}

Type Graph::thisTypeOnEntry() const
{
    if (m_codeBlock->parseMode() == SourceParseMode::ClassFieldInitializerMode) {
        if (uint16_t layoutID = thisLayoutID())
            return objectTypeForLayout(layoutID);
    }
    return argumentTypeOnEntry(0);
}

uint16_t Graph::thisLayoutID() const
{
    if (!Options::useAOTTypedFields() || !TypeTable::typedFieldsAreEnforced() || !programClasses())
        return 0;
    return programClasses()->thisLayoutIDIn(m_codeBlock);
}

uint16_t Graph::newObjectLayoutID(const Node* node)
{
    if (!Options::useAOTTypedFields() || !TypeTable::hasTypedFields() || !(Options::aotShapeOptimizations() & 1))
        return 0;
    uint32_t tag = typeTagOf(node);
    return tag ? TypeTable::shared()->allocationLayoutID(tag) : 0;
}

std::optional<KnownShape> Graph::literalShape(const Node* node) const
{
    if (node->graph != this)
        return node->graph->literalShape(node);
    unsigned count = node->numberOfLiteralProperties;
    std::optional<TypeTable::Layout> layout;
    if (uint32_t tag = typeTagOf(node); tag && (Options::aotShapeOptimizations() & 1) && TypeTable::shared())
        layout = TypeTable::shared()->layoutOf(tag);
    if ((!count && !layout && !newObjectLayoutID(node)) || count > KnownShape::maxProperties)
        return std::nullopt;
    KnownShape shape;
    shape.inlineCapacity = KnownShape::inlineCapacityFor(count);
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
    if (layout && layout->properties.size() == count && layout->capacity <= JSFinalObject::maxInlineCapacity) {
        bool matchesSourceOrder = true;
        for (unsigned i = 0; i < count; ++i)
            matchesSourceOrder &= layout->properties[i].first == shape.names[i];
        if (matchesSourceOrder) {
            shape.number = layout->number;
            shape.layoutID = layout->layoutID;
            shape.reserved = layout->layoutID ? layout->capacity : 0;
            shape.inlineSlots = layout->inlineSlots;
            for (auto& property : layout->properties)
                shape.slots.append(property.second);
            shape.inlineCapacity = KnownShape::inlineCapacityFor(layout->layoutID ? std::min<unsigned>(layout->inlineSlots, layout->capacity) : layout->capacity);
            return shape;
        }
    }
    if (uint16_t number = newObjectLayoutID(node)) {
        auto layout = TypeTable::shared()->typedLayout(number);
        BitVector taken;
        for (UniquedStringImpl* name : shape.names) {
            auto* found = layout.fields.findIf([&](auto& entry) { return entry.name == name; }) != notFound ? &layout.fields[layout.fields.findIf([&](auto& entry) { return entry.name == name; })] : nullptr;
            if (!found || taken.get(found->slot))
                return std::nullopt;
            taken.set(found->slot);
            shape.slots.append(found->slot);
        }
        shape.layoutID = number;
        shape.reserved = layout.capacity;
        shape.inlineSlots = layout.inlineSlots;
        shape.inlineCapacity = KnownShape::inlineCapacityFor(layout.inlineSlots);
        return shape;
    }
    if (Options::useAOTTypedFields() && TypeTable::hasTypedFields()) {
        if (unsigned wanted = TypeTable::shared()->inlineSlotsNeededFor(shape.names.span()); wanted > count)
            shape.inlineCapacity = std::max(shape.inlineCapacity, KnownShape::inlineCapacityFor(wanted));
    }
    if (count < 2 && shape.inlineCapacity == KnownShape::inlineCapacityFor(count))
        return std::nullopt;
    return shape;
}

unsigned Graph::knownCalleeIndex(const ImageKey& key)
{
    if (!isOutermost())
        return outermost().knownCalleeIndex(key);
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
                result.kind = StaticVariable::Unresolved;
                return result;
            }
            break;
        }
        case ScopeChainEntry::Unknown:
            if (m_declaredNames && type == GlobalProperty) {
                auto resolution = m_declaredNames->resolve(uid);
                if (resolution.kind == DeclaredNamesLink::Resolution::Slot) {
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

unsigned Graph::resolveScopeExtra(const OpResolveScope& bytecode)
{
    if (resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType).isInGlobalScopes)
        return Site::resolvesInGlobalScopes;
    return bytecode.m_localScopeDepth < Site::resolvesInGlobalScopes ? bytecode.m_localScopeDepth : std::numeric_limits<unsigned>::max();
}

unsigned Graph::getFromScopeExtra(const OpGetFromScope& bytecode)
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
    m_scopeIsModuleEnvironment = linkage->environmentDepth && m_codeBlock->codeType() == FunctionCode && declaredNames && declaredNames->scopeIsOutermostEnvironment();
    m_needsFunctionObject = !m_scopeIsModuleEnvironment || AOT::needsFunctionObject(m_codeBlock);
    for (const auto& instruction : m_codeBlock->instructions()) {
        if (instruction->opcodeID() != op_put_to_scope)
            continue;
        auto bytecode = instruction->as<OpPutToScope>();
        if (bytecode.m_getPutInfo.resolveType() != ResolvedClosureVar)
            m_namesAssignedTo.set(bytecode.m_var);
    }
}

void Graph::adoptInlinee(std::unique_ptr<Graph>&& other, InlineFrame frame)
{
    RELEASE_ASSERT(isOutermost() && other->isOutermost() && other->m_inlinees.isEmpty());
    if (inlineFrames.isEmpty())
        inlineFrames.append({ });
    other->m_inlineFrame = inlineFrames.size();
    inlineFrames.append(frame);
    other->m_outermost = this;
    for (auto& node : other->m_nodes)
        node.index += m_nodes.size() + m_numberOfInlinedNodes;
    m_numberOfInlinedNodes += other->m_nodes.size();
    for (auto& block : other->blocks) {
        block->index = blocks.size();
        blocks.append(WTF::move(block));
    }
    other->blocks.clear();
    other->m_rpo.clear();
    callsItself |= other->callsItself;
    makesCalls |= other->makesCalls;
    usesStaticImports |= other->usesStaticImports;
    numberOfIntrinsicReads += other->numberOfIntrinsicReads;
    m_inlinees.append(WTF::move(other));
}

void Graph::computeBlockOrder()
{
    for (auto& block : blocks)
        block->isReachable = false;
    m_rpo.shrink(0);
    Vector<BasicBlock*> postOrder;
    BitVector visited(blocks.size());
    struct Frame {
        BasicBlock* block;
        unsigned next;
    };
    auto visitFrom = [&](BasicBlock* start) {
        if (visited.get(start->index))
            return;
        Vector<Frame> stack;
        visited.set(start->index);
        stack.append({ start, 0 });
        while (!stack.isEmpty()) {
            Frame& frame = stack.last();
            if (frame.next < frame.block->successors.size()) {
                BasicBlock* successor = frame.block->successors[frame.next++];
                if (!visited.get(successor->index)) {
                    visited.set(successor->index);
                    stack.append({ successor, 0 });
                }
                continue;
            }
            postOrder.append(frame.block);
            stack.removeLast();
        }
    };
    for (BasicBlock* entrypoint : catchEntrypoints)
        visitFrom(entrypoint);
    visitFrom(root);
    for (unsigned i = postOrder.size(); i--;) {
        postOrder[i]->isReachable = true;
        m_rpo.append(postOrder[i]);
    }
}

void Graph::computeDominators()
{
    auto& rpo = m_rpo;
    for (unsigned i = 0; i < rpo.size(); ++i) {
        rpo[i]->rpoIndex = i;
        rpo[i]->immediateDominator = nullptr;
    }
    auto isEntry = [&](BasicBlock* block) { return block == root || block->isCatchEntrypoint; };
    BitVector processed(rpo.size());
    auto intersect = [&](BasicBlock* a, BasicBlock* b) -> BasicBlock* {
        while (a != b) {
            if (!a || !b)
                return nullptr;
            while (a && b && a->rpoIndex > b->rpoIndex)
                a = a->immediateDominator;
            while (a && b && b->rpoIndex > a->rpoIndex)
                b = b->immediateDominator;
        }
        return a;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (BasicBlock* block : rpo) {
            if (isEntry(block)) {
                processed.set(block->rpoIndex);
                continue;
            }
            BasicBlock* dominator = nullptr;
            bool first = true;
            for (BasicBlock* predecessor : block->predecessors) {
                if (!processed.get(predecessor->rpoIndex))
                    continue;
                dominator = first ? predecessor : intersect(dominator, predecessor);
                first = false;
            }
            if (!processed.get(block->rpoIndex) || block->immediateDominator != dominator) {
                processed.set(block->rpoIndex);
                block->immediateDominator = dominator;
                changed = true;
            }
        }
    }

    Vector<Vector<BasicBlock*, 2>> children(blocks.size());
    Vector<BasicBlock*, 4> roots;
    for (BasicBlock* block : rpo) {
        if (block->immediateDominator)
            children[block->immediateDominator->index].append(block);
        else
            roots.append(block);
    }
    unsigned number = 0;
    struct Frame {
        BasicBlock* block;
        unsigned next;
    };
    Vector<Frame> stack;
    for (BasicBlock* treeRoot : roots) {
        treeRoot->dominatorPreNumber = number++;
        stack.append({ treeRoot, 0 });
        while (!stack.isEmpty()) {
            Frame& frame = stack.last();
            auto& list = children[frame.block->index];
            if (frame.next < list.size()) {
                BasicBlock* child = list[frame.next++];
                child->dominatorPreNumber = number++;
                stack.append({ child, 0 });
                continue;
            }
            frame.block->dominatorPostNumber = number++;
            stack.removeLast();
        }
    }
}

void Graph::fail(ASCIILiteral reason, OpcodeID opcode)
{
    if (!isOutermost()) {
        outermost().fail(reason, opcode);
        return;
    }
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
        if (uint32_t tag = Graph::typeTagOf(this))
            out.print(" type#", tag);
        if (isBytecode(op_get_by_id))
            out.print(" .", graph->codeBlock()->identifier(as<OpGetById>().m_property).impl(), Graph::closedMethodReadBy(this) ? " (a closed method)" : "", isReadOnlyForCall ? " (only to be called)" : "");
        else if (isBytecode(op_put_by_id))
            out.print(" .", graph->codeBlock()->identifier(as<OpPutById>().m_property).impl());
        if (wasInferredUnreachable)
            out.print(" (taken never to be reached)");
        break;
    case NodeKind::Constant:
        out.print("Constant(", constant, ")");
        break;
    case NodeKind::ConstantCell:
        out.print("ConstantCell(", reg, ")");
        break;
    case NodeKind::LinkTimeConstant:
        out.print("LinkTimeConstant(", intrinsic, ")");
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
            out.print("Guard(", opcode, " bc#", bytecodeIndex.offset(), structureIsChecked ? ", structure checked" : "", slotIsDirect ? ", plain" : "", calleeIsChecked ? ", callee checked" : "", ")");
            break;
        case GuardKind::Nothing:
            out.print("Guard()");
            break;
        case GuardKind::Entry:
            out.print("GuardEntry");
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
        case GuardKind::SlotIsDirect:
            out.print("GuardSlotIsDirect(@", site->index, ")");
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
        case GuardKind::Uint8ArrayStorageIfAny:
            out.print("Uint8ArrayStorageIfAny");
            break;
        case GuardKind::TypedArrayStorage:
            out.print("GuardTypedArrayStorage");
            break;
        case GuardKind::IsLikelyFunction:
            out.print("GuardIsLikelyFunction#", likelyFunction);
            break;
        case GuardKind::IsArrayIntrinsic:
            out.print("IsArrayIntrinsic ", intrinsic, " ");
            break;
        case GuardKind::IsIntrinsic:
            out.print("IsIntrinsic ", intrinsic, " ");
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
        chooseFrameRegisters();
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

    bool canBeGuarded(const JSInstruction* instruction)
    {
        switch (instruction->opcodeID()) {
        case op_resolve_scope:
            return !isStaticClosureVarResolveType(instruction->as<OpResolveScope>().m_resolveType);
        case op_get_from_scope:
            return instruction->as<OpGetFromScope>().m_getPutInfo.resolveType() != ResolvedClosureVar;
        case op_call:
        case op_call_ignore_result: {
            auto operands = Graph::callOperands(instruction);
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
        case op_check_type:
            return true;
        default:
            return false;
        }
    }

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
                return valueType(value);
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
                Type type = typeOf(bytecode.m_value) & typeAcceptedByMask(bytecode.m_mask);
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
        bool isExact = false;
        const KnownFunction* known = m_graph.knownCallee(guard, &isExact);
        if (!known || !isExact || !canBeInlined(known, bytecode.m_argc))
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

    static bool opcodeBenefitsFromFastCopy(OpcodeID opcode)
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

    static bool isCallLike(OpcodeID opcode)
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

    bool isCheckSubsumedByAssertion(unsigned offset, unsigned end)
    {
        const JSInstruction* instruction = m_instructions.at(offset).ptr();
        if (instruction->opcodeID() != op_check_type || !(instruction->as<OpCheckType>().m_mask & MaskOtherObject) || Options::auditAOTTypedFields())
            return false;
        unsigned next = offset + instruction->size();
        if (next >= end || m_instructions.at(next)->opcodeID() != op_type_tag)
            return false;
        auto [layoutID, base] = layoutCheckedAt(next);
        return layoutID && base == instruction->as<OpCheckType>().m_value && m_graph.isTracked(base);
    }

    std::pair<uint16_t, VirtualRegister> layoutCheckedAt(unsigned offset)
    {
        if (!Options::useAOTTypedFields() || !TypeTable::hasTypedFields())
            return { };
        unsigned next = offset + m_instructions.at(offset)->size();
        if (next >= m_instructions.size() || !usesTypedAccessWithoutGuard(next))
            return { };
        const JSInstruction* access = m_instructions.at(next).ptr();
        VirtualRegister base = access->opcodeID() == op_get_by_id ? access->as<OpGetById>().m_base : access->as<OpPutById>().m_base;
        if (base.isConstant())
            return { };
        return { TypeTable::shared()->layoutIDOf(m_graph.typeTagAt(next)), base };
    }

    VirtualRegister arrayAssertedAt(unsigned offset)
    {
        if (!Options::useAOTTypedFields() || !TypeTable::typedFieldsAreEnforced())
            return { };
        unsigned next = offset + m_instructions.at(offset)->size();
        if (next >= m_instructions.size())
            return { };
        uint32_t tag = m_graph.typeTagAt(next);
        if (!tag || !TypeTable::shared()->isArray(tag))
            return { };
        const JSInstruction* access = m_instructions.at(next).ptr();
        VirtualRegister base;
        if (access->opcodeID() == op_get_by_id)
            base = access->as<OpGetById>().m_base;
        else if (access->opcodeID() == op_get_length)
            base = access->as<OpGetLength>().m_base;
        return base.isValid() && !base.isConstant() ? base : VirtualRegister();
    }

    bool usesTypedAccessWithoutGuard(unsigned offset)
    {
        if (!Options::useAOTTypedFields() || !TypeTable::shared())
            return false;
        uint32_t tag = m_graph.typeTagAt(offset);
        if (!tag)
            return false;
        const JSInstruction* instruction = m_instructions.at(offset).ptr();
        unsigned identifier;
        if (instruction->opcodeID() == op_get_by_id && (Options::aotShapeOptimizations() & 2))
            identifier = instruction->as<OpGetById>().m_property;
        else if (instruction->opcodeID() == op_put_by_id && (Options::aotShapeOptimizations() & 4))
            identifier = instruction->as<OpPutById>().m_property;
        else
            return false;
        return !!TypeTable::shared()->fieldOf(tag, m_codeBlock->identifier(identifier).impl());
    }

    bool chooseGuards()
    {
        if (!Options::useAOTLoopSplitting() || !Options::aotLoopSplittingPolicy() || !usesDataStubs())
            return false;
        unsigned size = m_instructions.size();
        struct BlockInfo {
            Vector<unsigned, 8> guards;
            bool benefitsFromFastCopy { false };
            bool hasRealCall { false };
            bool benefitsGreatlyFromFastCopy { false };
        };
        Vector<BlockInfo> blockInfos(m_graph.blocks.size());
        for (BasicBlock* block : m_graph.m_rpo) {
            if (!block->isInLoop || block == m_graph.root)
                continue;
            BlockInfo& blockInfo = blockInfos[block->index];
            m_recentProperties.shrink(0);
            m_recentFunctions.shrink(0);
            BitVector literalPart;
            for (unsigned offset = block->bytecodeBegin; offset < block->bytecodeEnd; offset += m_instructions.at(offset)->size()) {
                const JSInstruction* instruction = m_instructions.at(offset).ptr();
                if (literalPart.get(offset))
                    continue;
                if (instruction->opcodeID() == op_new_object) {
                    for (unsigned store : m_graph.literalStores(offset))
                        literalPart.set(store);
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
                        if (auto* known = m_graph.likelyFunctionInModuleVariable(bytecode.m_var, bytecode.m_offset))
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
                bool isGuarded = canBeGuarded(instruction) && !usesTypedAccessWithoutGuard(offset) && !isCheckSubsumedByAssertion(offset, block->bytecodeEnd);
                if (isGuarded)
                    blockInfo.guards.append(offset);
                OpcodeID opcode = instruction->opcodeID();
                if (isCallLike(opcode)) {
                    if (isGuarded) {
                        blockInfo.benefitsFromFastCopy = true;
                        blockInfo.benefitsGreatlyFromFastCopy = true;
                    } else
                        blockInfo.hasRealCall = true;
                } else if (opcodeBenefitsFromFastCopy(opcode)) {
                    blockInfo.benefitsFromFastCopy = true;
                    if (opcode == op_get_by_val || opcode == op_put_by_val)
                        blockInfo.benefitsGreatlyFromFastCopy = true;
                }
            }
        }

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
            bool hasBenefit = false;
            bool hasLargeBenefit = false;
            bool hasRealCall = false;
            for (unsigned index : body) {
                hasBenefit |= blockInfos[index].benefitsFromFastCopy;
                hasLargeBenefit |= blockInfos[index].benefitsGreatlyFromFastCopy;
                hasRealCall |= blockInfos[index].hasRealCall;
            }
            bool isProfitable = true;
            switch (Options::aotLoopSplittingPolicy()) {
            case 2:
                isProfitable = hasBenefit;
                break;
            case 3:
                isProfitable = !hasRealCall;
                break;
            case 4:
                isProfitable = hasBenefit && !hasRealCall;
                break;
            case 5:
                isProfitable = hasLargeBenefit || !hasRealCall;
                break;
            default:
                break;
            }
            if (isProfitable)
                hasTwoCopies.merge(body);
        }
        for (BasicBlock* block : m_graph.m_rpo) {
            block->isInProfitableLoop = block->isInLoop && hasTwoCopies.get(block->index);
            if (block->isInProfitableLoop)
                m_graph.remark("profitable-loop"_s);
        }
        if (m_graph.loopSplittingIsDisabled)
            return false;

        bool found = false;
        for (BasicBlock* block : m_graph.m_rpo) {
            bool isInSplitLoop = block->isInLoop && hasTwoCopies.get(block->index);
            if (!isInSplitLoop)
                continue;
            if (m_inLoop.isEmpty()) {
                m_inLoop.ensureSize(size + 1);
                m_guards.ensureSize(size + 1);
                m_loopHeaders.ensureSize(size + 1);
            }
            if (block->isLoopHeader) {
                m_graph.remark("split-loop"_s);
                m_loopHeaders.set(block->bytecodeBegin);
            }
            for (unsigned offset = block->bytecodeBegin; offset < block->bytecodeEnd; offset += m_instructions.at(offset)->size())
                m_inLoop.set(offset);
            for (unsigned offset : blockInfos[block->index].guards) {
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
            unsigned lastOffset = block->bytecodeBegin;
            for (unsigned offset = block->bytecodeBegin; offset < block->bytecodeEnd; offset += m_instructions.at(offset)->size())
                lastOffset = offset;
            auto instruction = m_instructions.at(lastOffset);
            OpcodeID opcode = instruction->opcodeID();
            if (isBranch(opcode)) {
                Vector<int32_t, 8> relativeOffsets;
                extractStoredJumpTargetsForInstruction(m_codeBlock, instruction, [&](int32_t relativeOffset) {
                    relativeOffsets.append(relativeOffset);
                });
                if (opcode == op_switch_string)
                    std::ranges::sort(relativeOffsets);
                for (int32_t relativeOffset : relativeOffsets)
                    link(block, m_graph.targetFrom(block, lastOffset + relativeOffset));
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
        Vector<std::pair<BasicBlock*, BasicBlock*>>& backEdges = m_backEdges;
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

    template<typename Handler>
    static bool isCoveredBy(BasicBlock* block, const Handler& handler)
    {
        return block->bytecodeBegin < handler.end && handler.start <= block->bytecodeEnd;
    }

    void computeLiveness()
    {
        unsigned numRegisters = m_graph.numRegisters();
        Vector<BitVector> uses(m_graph.blocks.size());
        Vector<BitVector>& defs = m_blockDefs;
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
        Vector<Vector<BasicBlock*, 2>> blockHandlers(m_graph.blocks.size());
        for (unsigned i = 0; i < m_codeBlock->numberOfExceptionHandlers(); ++i) {
            auto& handler = m_codeBlock->exceptionHandler(i);
            BasicBlock* target = m_graph.blockForOffset[handler.target];
            for (BasicBlock* block : m_graph.m_rpo) {
                if (isCoveredBy(block, handler))
                    blockHandlers[block->index].append(target);
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
                for (BasicBlock* handler : blockHandlers[block->index])
                    live.merge(handler->liveIn);
                if (live != block->liveIn) {
                    block->liveIn = WTF::move(live);
                    changed = true;
                }
            }
        }
    }

    struct SkippedStore {
        BasicBlock* block;
        unsigned index;
        VirtualRegister reg;
        Node* value;
    };
    Vector<SkippedStore> m_skippedStores;
    bool m_needsEveryStore { false };
    Vector<BitVector> m_blockDefs;

    void chooseFrameRegisters()
    {
        m_graph.m_registersLiveIntoHandlers.ensureSize(m_graph.numRegisters());
        for (BasicBlock* entrypoint : m_graph.catchEntrypoints)
            m_graph.m_registersLiveIntoHandlers.merge(entrypoint->liveIn);
        m_graph.m_arrayOperandRegisters.ensureSize(m_graph.numRegisters());
        for (const auto& instruction : m_instructions) {
            if (!Graph::readsOperandsFromFrame(instruction.ptr()))
                continue;
            auto bytecode = instruction->as<OpNewArray>();
            for (unsigned i = 0; i < bytecode.m_argc; ++i)
                m_graph.m_arrayOperandRegisters.set(m_graph.registerIndex(VirtualRegister(bytecode.m_argv.offset() - static_cast<int>(i))));
        }
        m_graph.m_frameRegisters = m_graph.m_registersLiveIntoHandlers;
        m_graph.m_frameRegisters.merge(m_graph.m_arrayOperandRegisters);
        m_graph.m_frameRegisterIndices.fill(0, m_graph.numRegisters());
        unsigned numberOfFrameRegisters = 0;
        for (unsigned index : m_graph.m_frameRegisters)
            m_graph.m_frameRegisterIndices[index] = numberOfFrameRegisters++;
        m_graph.frameRegisterTypes.fill(TNone, m_graph.numRegisters());

        m_skippedStores.clear();
        if (m_graph.catchEntrypoints.isEmpty() || m_needsEveryStore)
            return;
        unsigned numRegisters = m_graph.numRegisters();
        Vector<BitVector> atStart(m_graph.blocks.size());
        for (BasicBlock* block : m_graph.m_rpo) {
            block->readByBlockHandlers.ensureSize(numRegisters);
            block->readByHandlersAfterBlock.ensureSize(numRegisters);
            atStart[block->index].ensureSize(numRegisters);
        }
        for (unsigned i = 0; i < m_codeBlock->numberOfExceptionHandlers(); ++i) {
            auto& handler = m_codeBlock->exceptionHandler(i);
            BasicBlock* target = m_graph.blockForOffset[handler.target];
            for (BasicBlock* block : m_graph.m_rpo) {
                if (isCoveredBy(block, handler))
                    block->readByBlockHandlers.merge(target->liveIn);
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
                start.exclude(m_blockDefs[block->index]);
                start.merge(block->readByBlockHandlers);
                if (after != block->readByHandlersAfterBlock || start != atStart[block->index]) {
                    block->readByHandlersAfterBlock = WTF::move(after);
                    atStart[block->index] = WTF::move(start);
                    changed = true;
                }
            }
        }
    }

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
        RELEASE_ASSERT(m_codeBlock->constantSourceCodeRepresentation(reg) != SourceCodeRepresentation::LinkTimeConstant);
        JSValue value = m_codeBlock->getConstant(reg);
        if (!value || !value.isCell())
            return m_graph.constant(value);
        if (!m_constantCells[index]) {
            Node* node = m_graph.addNode(NodeKind::ConstantCell);
            node->range = IntegerRange::unknown();
            node->reg = reg;
            node->type = value.asCell()->inherits<JSTemplateObjectDescriptor>() ? TArray : valueType(value);
            if (value.isString() && m_codeBlock->codeType() == ModuleCode)
                node->type |= asString(value)->length() <= TypedLayoutTable::maxAtomizedStringLength ? TShortOtherString : TLongString;
            m_constantCells[index] = node;
        }
        return m_constantCells[index];
    }

    Node* get(BasicBlock* block, VirtualRegister reg)
    {
        if (reg.isConstant() && m_codeBlock->constantSourceCodeRepresentation(reg) == SourceCodeRepresentation::LinkTimeConstant) {
            JSValue which = m_codeBlock->getConstant(reg);
            if (auto number = intrinsicForLinkTimeConstant(which))
                return m_graph.intrinsic(*number);
            Node* node = m_graph.addNode(NodeKind::LinkTimeConstant);
            node->range = IntegerRange::unknown();
            node->intrinsic = safeCast<uint16_t>(which.asInt32AsAnyInt());
            node->type = TTop;
            return append(block, node);
        }
        if (reg.isConstant())
            return constantFor(reg);
        if (reg == VirtualRegister(CallFrameSlot::callee)) {
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
        if (!value && m_graph.livesInFrame(reg)) {
            m_needsEveryStore = true;
            Node* node = m_graph.addNode(NodeKind::GetStack);
            node->reg = reg;
            return append(block, node);
        }
        if (!value)
            return m_graph.constant(jsUndefined());
        return value;
    }

    void set(BasicBlock* block, VirtualRegister reg, Node* value)
    {
        if (!m_graph.isTracked(reg)) {
            m_graph.fail("writes a register outside the frame"_s);
            return;
        }
        if (m_graph.livesInFrame(reg)) {
            unsigned index = m_graph.registerIndex(reg);
            if (m_needsEveryStore || m_graph.m_arrayOperandRegisters.get(index) || block->readByBlockHandlers.get(index) || block->readByHandlersAfterBlock.get(index)) {
                Node* node = m_graph.addNode(NodeKind::SetStack);
                node->reg = reg;
                node->uses.append({ VirtualRegister(), value });
                append(block, node);
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
                node->type = i ? m_graph.argumentTypeOnEntry(i) : m_graph.thisTypeOnEntry();
                append(block, node);
                set(block, node->reg, node);
            }
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

        RELEASE_ASSERT(m_pendingLiterals.isEmpty());
        for (unsigned offset = block->bytecodeBegin; offset < block->bytecodeEnd; offset += m_instructions.at(offset)->size()) {
            const JSInstruction* instruction = m_instructions.at(offset).ptr();
            OpcodeID opcode = instruction->opcodeID();
            if (opcode == op_type_tag) {
                auto [layoutID, baseRegister] = layoutCheckedAt(offset);
                bool isArray = false;
                if (!layoutID) {
                    baseRegister = arrayAssertedAt(offset);
                    isArray = baseRegister.isValid();
                }
                if ((!layoutID && !isArray) || !m_graph.isTracked(baseRegister))
                    continue;
                Node* base = get(block, baseRegister);
                if (base == m_pendingCreateThis)
                    continue;
                Node* node = m_graph.addNode(NodeKind::Bytecode);
                node->opcode = opcode;
                node->instruction = instruction;
                node->bytecodeIndex = BytecodeIndex(offset + instruction->size());
                node->uses.append({ baseRegister, base });
                node->firstLayout = node->lastLayout = layoutID;
                if (layoutID)
                    node->isTrusted = TypeTable::shared()->isTrusted(m_graph.typeTagAt(offset + instruction->size())) && !(TypeTable::shared()->isOpen(layoutID) && Graph::isEscapingFunctionThis(base));
                if (isArray)
                    node->narrowedTo = TArray;
                node->reg = baseRegister;
                append(block, node);
                for (unsigned index = 0; index < block->valuesAtTail.size(); ++index) {
                    if (block->valuesAtTail[index] == base && !m_graph.m_frameRegisters.get(index))
                        block->valuesAtTail[index] = node;
                }
                continue;
            }
            if (isCheckSubsumedByAssertion(offset, block->bytecodeEnd))
                continue;
            if (opcode == op_put_by_id && !m_pendingLiterals.isEmpty() && m_pendingLiterals.last().stores[m_pendingLiterals.last().next] == offset) {
                auto& literal = m_pendingLiterals.last();
                literal.node->uses.append({ NewObjectPlan::registerOf(literal.next), get(block, instruction->as<OpPutById>().m_value) });
                if (++literal.next == literal.stores.size()) {
                    Node* node = literal.node;
                    m_pendingLiterals.removeLast();
                    append(block, node);
                    set(block, node->reg, node);
                }
                continue;
            }
            if (m_pendingCreateThis && opcode == op_put_by_id) {
                auto& stores = m_objectPlan.stores;
                RELEASE_ASSERT(m_nextPlanStore < stores.size() && stores[m_nextPlanStore].offset == offset);
                VirtualRegister reg = NewObjectPlan::registerOf(stores[m_nextPlanStore].property);
                Node* value = get(block, instruction->as<OpPutById>().m_value);
                bool found = false;
                for (auto& use : m_pendingCreateThis->uses) {
                    if (use.reg == reg) {
                        use.node = value;
                        found = true;
                    }
                }
                if (!found)
                    m_pendingCreateThis->uses.append({ reg, value });
                if (++m_nextPlanStore == stores.size()) {
                    append(block, m_pendingCreateThis);
                    m_pendingCreateThis = nullptr;
                }
                continue;
            }

            switch (opcode) {
            case op_new_object: {
                VirtualRegister reg = instruction->as<OpNewObject>().m_dst;
                if (!m_graph.isTracked(reg) || m_graph.isLiveIntoHandler(reg))
                    break;
                auto stores = m_graph.literalStores(offset);
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
                m_pendingLiterals.append({ node, WTF::move(stores), 0 });
                continue;
            }
            case op_create_this: {
                if (m_graph.hasFrameRegisters() || block->isInLoop)
                    break;
                m_objectPlan = NewObjectPlan::forCreateThis(m_instructions, offset);
                if (m_objectPlan.stores.isEmpty() || m_objectPlan.stores.last().offset >= block->bytecodeEnd)
                    break;
                if (uint16_t layoutID = m_graph.thisLayoutID()) {
                    bool hasFieldForEachProperty = TypeTable::hasTypedFields() && (Options::aotShapeOptimizations() & 4) && !Options::auditAOTTypedFields();
                    for (auto& property : m_objectPlan.properties) {
                        auto field = hasFieldForEachProperty ? TypeTable::shared()->layoutField(layoutID, m_codeBlock->identifier(property.identifier).impl()) : std::nullopt;
                        hasFieldForEachProperty = field && field->isInObject();
                    }
                    if (!hasFieldForEachProperty)
                        break;
                }
                auto bytecode = instruction->as<OpCreateThis>();
                Node* node = m_graph.addNode(NodeKind::Bytecode);
                node->opcode = opcode;
                node->instruction = instruction;
                node->bytecodeIndex = BytecodeIndex(offset);
                node->uses.append({ bytecode.m_callee, get(block, bytecode.m_callee) });
                node->numberOfLiteralProperties = m_objectPlan.properties.size();
                node->reg = bytecode.m_dst;
                node->block = block;
                set(block, bytecode.m_dst, node);
                m_pendingCreateThis = node;
                m_nextPlanStore = 0;
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
            case op_identity_with_profile:
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
                if (auto it = m_inlinedCallResults.find(offset); it != m_inlinedCallResults.end()) {
                    set(block, instruction->as<OpCall>().m_dst, it->value);
                    continue;
                }
            }

            Node* node = m_graph.addNode(NodeKind::Bytecode);
            node->opcode = opcode;
            node->instruction = instruction;
            node->bytecodeIndex = BytecodeIndex(offset);
            if (!Graph::readsOperandsFromFrame(instruction)) {
                forEachUse(instruction, [&](VirtualRegister reg) {
                    node->uses.append({ reg, get(block, reg) });
                });
            }
            if (opcode == op_call || opcode == op_call_ignore_result || opcode == op_tail_call) {
                VirtualRegister thisRegister = Graph::callOperands(instruction).argument(0);
                for (auto& use : node->uses) {
                    if (use.reg == thisRegister && m_graph.isScopeUsedAsImplicitThis(use.node))
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
                    if (check.m_value == instruction->as<OpGetByVal>().m_dst)
                        node->expectedMask = check.m_mask;
                }
            }

            Vector<VirtualRegister, 4> defs;
            forEachDef(instruction, [&](VirtualRegister reg) {
                defs.append(reg);
            });
            if (opcode == op_check_type) {
                VirtualRegister reg = instruction->as<OpCheckType>().m_value;
                if (m_graph.isTracked(reg)) {
                    node->reg = reg;
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
                    if (m_graph.m_frameRegisters.get(index) || !value)
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
                bool speculates = false;
                for (const auto& inLoop : m_instructions) {
                    if (inLoop.offset() < block->bytecodeEnd || !m_inLoop.get(inLoop.offset()))
                        continue;
                    VirtualRegister counter;
                    if (inLoop->is<OpInc>())
                        counter = inLoop->as<OpInc>().m_srcDst;
                    else if (inLoop->is<OpDec>())
                        counter = inLoop->as<OpDec>().m_srcDst;
                    else
                        continue;
                    unsigned index = m_graph.registerIndex(counter);
                    Node* value = block->valuesAtTail[index];
                    if (!block->liveIn.get(index) || m_graph.m_frameRegisters.get(index) || !value || (value->kind == NodeKind::Narrow && value->block == block))
                        continue;
                    Node* narrow = m_graph.addNode(NodeKind::Narrow);
                    narrow->reg = counter;
                    narrow->speculatedType = TInt32;
                    narrow->uses.append({ VirtualRegister(), value });
                    append(block, narrow);
                    block->valuesAtTail[index] = narrow;
                    speculates = true;
                }
                if (speculates) {
                    Node* entry = m_graph.addNode(NodeKind::Guard);
                    entry->guardKind = GuardKind::Entry;
                    entry->bytecodeIndex = BytecodeIndex(block->bytecodeEnd);
                    append(block, entry);
                }
                guard->guardKind = GuardKind::Nothing;
                guard->bytecodeIndex = BytecodeIndex(block->bytecodeEnd);
                append(block, guard);
                return;
            }
            if (Node* result = inlineCall(block, instruction, block->bytecodeEnd)) {
                m_inlinedCallResults.set(block->bytecodeEnd, result);
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

        if (Node* last = block->terminal(); last && last->kind != NodeKind::Bytecode) {
            for (Node* node : block->nodes) {
                if (node->kind == NodeKind::Bytecode && (isBranch(node->opcode) || isTerminal(node->opcode) || isThrow(node->opcode))) {
                    m_graph.fail("terminal defines a register"_s, node->opcode);
                    return;
                }
            }
        }
    }

    Node* valueLeaving(BasicBlock* from, BasicBlock* to, unsigned index)
    {
        Node* value = from->valuesAtTail[index];
        if (value && (from->isReentry || from->isPreHeader) && to != from->successors[0] && value->kind == NodeKind::Narrow && value->block == from)
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
        UncheckedKeyHashMap<Node*, Node*> origins;
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
    bool m_hasGuards { false };
    BitVector m_leaders;
    BitVector m_inLoop;
    BitVector m_guards;
    BitVector m_loopHeaders;
    struct PendingLiteral {
        Node* node;
        Vector<unsigned, 4> stores;
        unsigned next;
    };
    Vector<PendingLiteral, 2> m_pendingLiterals;
    Node* m_pendingCreateThis { nullptr };
    NewObjectPlan m_objectPlan;
    unsigned m_nextPlanStore { 0 };
    Vector<std::pair<BasicBlock*, BasicBlock*>> m_backEdges;
    Vector<std::pair<VirtualRegister, unsigned>, 8> m_recentProperties;
    Vector<std::pair<VirtualRegister, const KnownFunction*>, 8> m_recentFunctions;
    UncheckedKeyHashMap<UnlinkedFunctionCodeBlock*, bool> m_canBeInlined;
    UncheckedKeyHashMap<unsigned, Node*, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_inlinedCallResults;
};

} // anonymous namespace

bool parseBytecode(Graph& graph)
{
    Parser parser(graph);
    return parser.run();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
