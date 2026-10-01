/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#include "AOTTypeTable.h"

#if ENABLE(FTL_JIT)

#include "AOTBuiltins.h"
#include "AOTProgram.h"
#include "AOTStubs.h"
#include "BuiltinNames.h"
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
    if (value.isString()) {
        const StringImpl* impl = asString(value)->tryGetValueImpl();
        return impl && impl->isAtom() ? TAtomString : asString(value)->length() <= TypedLayoutTable::maxLengthOfAtomizedString ? TShortOtherString : TLongString;
    }
    return typeOfCellOfType(value.asCell()->type());
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
        // The empty value is encoded as zero, which the hash table reserves for empty buckets.
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
    // No jump can land in the middle of this sequence: a jump target either follows a branch or is a loop header, and the scan
    // stops at both.
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
            // If the check throws, the object is unreachable, so it does not matter that it has not been allocated yet. One
            // difference is observable: with a proxy as new.target, allocating reads `prototype` through the proxy, and that read
            // now comes after the check. That is accepted, rather than giving up on every constructor that checks a parameter.
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
                // A second ordinary store to the same property. If a setter on the prototype chain receives the first store it must
                // receive this one too, but a plan keeps only the last value.
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

const Vector<unsigned, 4>& Graph::storesOfLiteral(unsigned offsetOfNewObject)
{
    if (!m_hasFoundStoresOfLiterals)
        findStoresOfLiterals();
    auto it = m_storesOfLiterals.find(offsetOfNewObject);
    RELEASE_ASSERT(it != m_storesOfLiterals.end());
    return it->value;
}

// One pass over the function for all of its literals. Looking ahead from each op_new_object instead is quadratic: in [{...}, {...}, ...]
// nothing mentions an element again until the op_new_array at the end.
void Graph::findStoresOfLiterals()
{
    m_hasFoundStoresOfLiterals = true;
    constexpr unsigned maximumCount = 2048; // The values stored are all kept live until the last store.
    // By register: the offset of the op_new_object whose object is still being initialized there.
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
                    auto& stores = m_storesOfLiterals.find(it->value)->value;
                    stores.append(instruction.offset());
                    if (stores.size() == maximumCount)
                        open.remove(it);
                    else
                        initialized = bytecode.m_base;
                }
            }
        }
        // Control cannot enter between the stores from elsewhere: a jump target follows a jump, or is a loop header or a handler.
        if (isBranch(opcode) || isTerminal(opcode) || isThrow(opcode) || opcode == op_loop_hint || opcode == op_catch) {
            open.clear();
            continue;
        }
        if (!open.isEmpty()) {
            // Anything else that reads or writes the register may observe the object.
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
            m_storesOfLiterals.add(instruction.offset(), Vector<unsigned, 4>());
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
        node->type = typeOfCellOfType(entry.type);
        node->range = IntegerRange::unknown();
        return node;
    }).iterator->value;
}

Node* Graph::intrinsicReadBy(const JSInstruction* instruction, Node* base)
{
    const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
    if (!intrinsics)
        return nullptr;
    // The number of the intrinsic that a global variable holds, or zero. The name has to resolve to the global object: no scope in
    // between declares it, and none can start to, because a later script cannot declare a lexical variable that shadows a
    // non-configurable global property.
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

std::optional<LinkTimeConstant> Graph::linkTimeConstantOf(const Node* node)
{
    if (node->kind != NodeKind::LinkTimeConstant)
        return std::nullopt;
    return static_cast<LinkTimeConstant>(node->intrinsic);
}

CallIntrinsic Graph::intrinsicOfCall(const Node* node) const
{
    if (node->graph != this)
        return node->graph->intrinsicOfCall(node);
    if (node->opcode != op_call && node->opcode != op_call_ignore_result)
        return CallIntrinsic::None;
    CallOperands operands = operandsOfCall(node->instruction);
    Node* callee = node->use(operands.callee);
    if (callee->kind == NodeKind::Intrinsic) {
        // The callee is a known intrinsic, whichever name the program read it through.
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
    if (intrinsicOfCall(node) == CallIntrinsic::ArrayPush) {
        auto operands = operandsOfCall(node->instruction);
        return { node->use(operands.argument(0)), node->use(operands.argument(1)) };
    }
    return { nullptr, nullptr };
}

std::optional<uint32_t> Graph::distanceOfEnvironmentAccessed(const Node* node)
{
    if (node->graph != this)
        return node->graph->distanceOfEnvironmentAccessed(node);
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
        // As in knownFunctionReadBy(): the variable has to be the module's, not a local of this function or of a scope in between.
        auto resolution = m_declaredNames->resolve(m_codeBlock->identifier(identifier).impl());
        if (resolution.kind != DeclaredNamesLink::Resolution::Slot || !resolution.isInOutermostEnvironment || resolution.offset != offset)
            return std::nullopt;
        if (isScopeAtDepth(node->use(scopeRegister), resolution.hops))
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

// Whether `scope` is `hops` scopes out from the scope the function was created in.
bool Graph::isScopeAtDepth(const Node* scope, unsigned hops)
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
    if (node->graph != this)
        return node->graph->distanceOfEnvironmentResolvedTo(node);
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
    if ((!known || !proven) && functionsOfProgram() && (node->opcode == op_call || node->opcode == op_call_ignore_result || node->opcode == op_tail_call)) {
        VirtualRegister calleeRegister = node->opcode == op_tail_call ? node->as<OpTailCall>().m_callee : operandsOfCall(node->instruction).callee;
        if (const KnownFunction* method = functionsOfProgram()->function(closedMethodReadBy(node->use(calleeRegister))); method && method->forCall) {
            known = method;
            proven = true;
        }
    }
    // The type of the callee may identify a single function, however the value got there. (The type may also allow values that are
    // not callable. The lowering of the call checks for those.)
    if ((!known || !proven) && node->opcode != op_construct) {
        VirtualRegister calleeRegister;
        switch (node->opcode) {
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
        if (calleeRegister.isValid() && functionsOfProgram()) {
            Type type = node->use(calleeRegister)->type;
            if (const KnownFunction* function = mayBe(type, TOtherObject) ? nullptr : functionsOfProgram()->function(functionNumberOf(type)); function && function->forCall) {
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
    // In the generic copy of a split loop, the value read reaches the call through phis that merge that copy with the fast copy.
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
    // (A callee found through a phi is only a hint, never exact.)
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
        // Is this the module's variable, and not a local of this function or of a scope in between?
        const void* scope = const_cast<Graph*>(this)->identityOfScope(callee->use(bytecode.m_scope));
        // If the scope cannot be identified, the variable may still be the module's. That proves nothing, but it is a useful hint.
        if (!scope)
            return probablyFunctionInVariableOfModule(bytecode.m_var, bytecode.m_offset);
        if (scope != m_hints->scopeOfVariables())
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
        // An import that this code does not read from a static location (see resolveStatically()). The imported function is still a
        // useful hint.
        if (const StaticImport* import = m_linkage->findImport(name); import && import->function)
            return import->function;
    }
    // A read whose scope is resolved when the code is linked, because BytecodeOptimizerAccess::resolveScopesStatically() did not
    // resolve it. The name still says which variable is likely to be read: a hint, unless the scope can be identified below.
    if (m_declaredNames) {
        auto resolution = m_declaredNames->resolve(name);
        switch (resolution.kind) {
        case DeclaredNamesLink::Resolution::Slot: {
            if (!resolution.isInOutermostEnvironment)
                return nullptr;
            const KnownFunction* known = m_hints->find(name, resolution.offset);
            // Exact only if the scope that is read from is the one that declares the name.
            if (known && known->isExact && isExact)
                *isExact = const_cast<Graph*>(this)->identityOfScope(callee->use(bytecode.m_scope)) == m_hints->scopeOfVariables();
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
    // Lowered like a call to an unknown callee, which needs the function object: Lowering::lowerCallToKnownFunction().
    if (node->isBytecode(op_tail_call) && known->conventionForCall.signature == Signature::List)
        return false;
    if (closedMethodReadBy(node->use(calleeRegister))) {
        usesStaticImports = true;
        return true;
    }
    if (!node->use(calleeRegister)->isBytecode(op_get_from_scope))
        return false;
    // The callee finds its module's environment at a static distance from the Instance, without checking, so this code must depend
    // on that distance too. distanceOfEnvironmentAccessed() records the dependency (usesStaticImports).
    return !!distanceOfEnvironmentAccessed(node->use(calleeRegister));
}

uint32_t Graph::distanceOfEnvironmentOfModule()
{
    RELEASE_ASSERT(m_scopeIsEnvironmentOfModule);
    usesStaticImports = true;
    return m_linkage->distanceOfEnvironment;
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
            // For example Math.floor(): the callee is already known to be an intrinsic.
            if (callee->kind == NodeKind::Intrinsic) {
                if (builtinWithNumber(callee->intrinsic) != Builtin::None)
                    node->builtinCalled = callee->intrinsic;
                continue;
            }
            if (!callee->isBytecode(op_get_by_id) || callee->guard || callee->guarded || callee->isElided || callee->isReadOnlyToBeCalled)
                continue;
            auto read = callee->as<OpGetById>();
            Node* base = callee->use(read.m_base);
            int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
            if (node->use(VirtualRegister(firstArgument)) != base || !base->type)
                continue;
            const StringImpl& name = *callee->graph->codeBlock()->identifier(read.m_property).impl();
            Receiver receiver = receiverOfType(base->type);
            if (receiver == Receiver::None) {
                // Use the receiver type from the type table if there is one. Otherwise guess from which built-in types have a
                // method with this name.
                uint32_t tag = TypeTable::shared() ? typeTagOf(callee) : 0;
                if (tag && TypeTable::shared()->isArray(tag) && mayBe(base->type, TArray))
                    receiver = Receiver::Array;
                else if (Receiver hinted = tag ? static_cast<Receiver>(TypeTable::shared()->receiverHintOf(tag)) : Receiver::None; hinted != Receiver::None && hinted != Receiver::Number && mayBe(base->type, typeOf(hinted)))
                    receiver = hinted;
                else if (!tag || !TypeTable::shared()->isShape(tag))
                    receiver = receiverLikelyToHave(base->type, name);
            }
            if (receiver == Receiver::None)
                continue;
            unsigned number = intrinsicFoundOn(receiver, name);
            if (!number || builtinWithNumber(number) == Builtin::None)
                continue;
            node->builtinCalled = number;
            node->receiverOfBuiltin = static_cast<uint8_t>(receiver);
            callee->builtinCalled = number;
            callee->receiverOfBuiltin = static_cast<uint8_t>(receiver);
        }
    }
}

bool Graph::isReadOfIteratorMethodOfArray(const Node* node)
{
    if (!node->isBytecode(op_get_by_id) || node->guard)
        return false;
    auto bytecode = node->as<OpGetById>();
    Type base = node->use(bytecode.m_base)->type;
    return base && isSubtype(base, TArray) && node->graph->codeBlock()->identifier(bytecode.m_property).impl() == node->graph->vm().propertyNames->iteratorSymbol.impl();
}

bool Graph::isIteratorMethodOfAnyArray(const Node* node)
{
    if (node->kind == NodeKind::LinkTimeConstant)
        return static_cast<LinkTimeConstant>(node->intrinsic) == LinkTimeConstant::arrayProtoValues;
    if (node->kind != NodeKind::Intrinsic)
        return false;
    auto number = intrinsicForLinkTimeConstant(jsNumber(static_cast<int32_t>(LinkTimeConstant::arrayProtoValues)));
    return number && node->intrinsic == *number;
}

void Graph::elideReadsOfIteratorMethodsOfArrays()
{
    UncheckedKeyHashMap<Node*, unsigned> otherUses;
    for (BasicBlock* block : m_rpo) {
        for (Node* node : block->nodes) {
            if (isReadOfIteratorMethodOfArray(node))
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
                compares &= other.node == use.node ? &other == &use : isIteratorMethodOfAnyArray(other.node);
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

void Graph::elideReadsOfCalleesNotPassed()
{
    // For each read, the number of uses that need the value.
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
        // The read has no observable effect: the variable holds the function, or is a lazily instantiated declaration, which is
        // only created when it is first read.
        if (read->isBytecode(op_get_by_id)) {
            // (Reading a method from a value that is not an object throws, so that check stays. Reading it from an object has no
            // observable effect.)
            if (closedMethodReadBy(read))
                read->isReadOnlyToBeCalled = true;
            else
                read->isElided = true;
            continue;
        }
        bool isExact = false;
        read->isElided = knownFunctionReadBy(read, &isExact) && isExact;
    }
}

const void* Graph::identityOfScope(const Node* scope, unsigned depth)
{
    if (scope->graph != this)
        return scope->graph->identityOfScope(scope, depth);
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
    // A scope that arrives through phis or stack slots: identified only if every source is the same scope.
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
                // A register that is kept in its stack slot may hold the value of any store to that slot.
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
                // The initial value of the register, before a scope is stored in it. No variable is read through it.
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
    if (node->graph != this)
        return node->graph->variableAccessedBy(node);
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
        // op_put_to_scope names the scope it stores to: its operand is the constant that holds the scope's symbol table, and that
        // cell is how a scope is identified here.
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
    // An access whose scope is resolved when the code is linked. The variable is not a local of this function.
    if (m_declaredNames) {
        auto resolution = m_declaredNames->resolve(m_codeBlock->identifier(identifier).impl());
        if (resolution.kind == DeclaredNamesLink::Resolution::Slot)
            return { resolution.scope, resolution.offset };
    }
    return { };
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
                // Can the store be to a variable in an environment record at all?
                auto bytecode = node->as<OpPutToScope>();
                UniquedStringImpl* name = m_codeBlock->identifier(bytecode.m_var).impl();
                ResolveType type = bytecode.m_getPutInfo.resolveType();
                bool isOnHolder = false;
                if (type != ResolvedClosureVar && type != ResolvedLazyClosureVar && type != Dynamic && m_declaredNames) {
                    auto kind = m_declaredNames->resolve(name).kind;
                    // (Assigning to an import throws.)
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
                    summaries.recordDynamicReadOfName(name);
                break;
            }
            case op_create_scoped_arguments:
                // A scoped arguments object aliases the parameters in the environment record, so they can be written through it.
                if (const void* scope = identityOfScope(node->use(node->as<OpCreateScopedArguments>().m_scope)))
                    summaries.giveUpOnScope(scope);
                else {
                    for (auto& identifier : m_codeBlock->identifiers())
                        summaries.giveUpOnName(identifier.impl());
                }
                break;
            case op_call_direct_eval:
                // Direct eval runs code the compiler has not seen, which can write any variable in scope.
                if (m_declaredNames)
                    m_declaredNames->forEachScope([&](const void* scope) { summaries.giveUpOnScope(scope); });
                for (BasicBlock* other : m_rpo) {
                    for (Node* made : other->nodes) {
                        if (made->isBytecode(op_create_lexical_environment) || made->isBytecode(op_create_generator_frame_environment)) {
                            if (const void* scope = identityOfScope(made))
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

void Graph::recordUsesOfKnownFunctions(const FunctionSummaryMap& summariesByExecutable)
{
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
    auto note = [&](Node* user, const Use& use) {
        // At its creation site, the only use that does not count is the store that initializes its variable. Any other use lets the
        // function object escape before it reaches the variable.
        if (auto* executable = executableCreatedBy(use.node)) {
            if (auto* summary = summariesByExecutable.get(executable); summary && !(user->isBytecode(op_put_to_scope) && use.reg == user->as<OpPutToScope>().m_value))
                summary->valueIsUsed.store(true, std::memory_order_relaxed);
            return;
        }
        if (!use.node->isBytecode(op_get_from_scope))
            return;
        bool readIsExact = false;
        const KnownFunction* known = knownFunctionReadBy(use.node, &readIsExact);
        if (!known || !known->summary)
            return;
        // (`f?.()` tests the callee for undefined and null.)
        if (user->isBytecode(op_check_tdz) || user->isBytecode(op_jundefined_or_null) || user->isBytecode(op_jnundefined_or_null))
            return;
        bool isCallee = false;
        if (user->isBytecode(op_call))
            isCallee = use.reg == user->as<OpCall>().m_callee;
        else if (user->isBytecode(op_call_ignore_result))
            isCallee = use.reg == user->as<OpCallIgnoreResult>().m_callee;
        else if (user->isBytecode(op_tail_call))
            isCallee = use.reg == user->as<OpTailCall>().m_callee;
        // (An inexact read may be of another variable with the same name, so it does not count as a direct call.)
        if (isCallee && readIsExact && known->forCall && knownCallee(user) == known && calleeIsExact(user)) {
            known->summary->directCalls.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        known->summary->valueIsUsed.store(true, std::memory_order_relaxed);
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

void Graph::noteFieldsComparedWithStrings()
{
    if (!Options::useAOTTypedFields() || !TypeTable::typedFieldsAreEnforced())
        return;
    auto isStringOfProgram = [](Node* node) {
        if (node->kind != NodeKind::ConstantCell || !node->reg.isConstant())
            return false;
        JSValue constant = node->graph->codeBlock()->getConstant(node->reg);
        return constant && constant.isString();
    };
    // Follows the value back to the property reads it may have come from.
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
        if (isStringOfProgram(right))
            note(note, left, 0);
        else if (isStringOfProgram(left))
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

    // For a rest array or an arguments object: the number of uses by calls that can pass the arguments on without materializing it.
    UncheckedKeyHashMap<Node*, unsigned> numberOfAliasingUses;
    for (BasicBlock* block : m_rpo) {
        for (unsigned index = 0; index < block->nodes.size(); ++index) {
            Node* list = listOfArgumentsOf(block->nodes[index]);
            if (!list)
                continue;
            if (list->isBytecode(op_create_cloned_arguments)) {
                ++numberOfAliasingUses.add(list, 0).iterator->value;
                continue;
            }
            // In reverse order. They must come immediately before the call, with nothing in between that could observe when they
            // run.
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
                // (Spreading an array yields its elements, as long as array iteration has not been modified.)
                if (spread->isBytecode(op_create_rest))
                    ++numberOfAliasingUses.add(spread, 0).iterator->value;
            }
        }
    }

    // (The arguments passed to this function are in the caller's frame, and nothing writes to them.)
    for (auto& [node, uses] : numberOfAliasingUses)
        node->isElided = uses == numberOfUses.get(node);

    // An array built from other arrays: [...a, ...b]. The spreads immediately before it are elided in the same way.
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
                bool isOfThisArray = false;
                for (auto& use : array->uses)
                    isOfThisArray |= use.node == spread;
                if (!isOfThisArray)
                    break;
                spread->isElided = true;
            }
        }
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

bool Graph::isThisOfEscapingFunction(const Node* node)
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
    if (!Options::useAOTTypedFields() || Options::useAOTFunctionSplitting() || !TypeTable::typedFieldsAreEnforced() || node->guard)
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
    return fieldOfTypedBase(node->use(base), name);
}

std::optional<TypeTable::Field> Graph::fieldOfTypedBase(const Node* base, UniquedStringImpl* name)
{
    if (!Options::useAOTTypedFields() || !TypeTable::typedFieldsAreEnforced() || !base->type || !isSubtype(base->type, TFinalObject))
        return std::nullopt;
    auto layoutIDs = layoutRangeOf(base->type);
    if (!layoutIDs.lowest || layoutIDs.lowest != layoutIDs.highest)
        return std::nullopt;
    return TypeTable::shared()->fieldOfLayout(layoutIDs.lowest, name);
}

uint32_t Graph::classNotedBy(const Node* node)
{
    if (!node->isBytecode(op_call_ignore_result) || !Options::useAOTTypedFields() || !TypeTable::hasTypedFields())
        return 0;
    CallOperands operands = operandsOfCall(node->instruction);
    if (operands.argc != 2 || linkTimeConstantOf(node->use(operands.callee)) != LinkTimeConstant::noteClass)
        return 0;
    uint32_t tag = typeTagOf(node);
    return tag && TypeTable::shared()->isClass(tag) ? tag : 0;
}

static UnlinkedFunctionExecutable* functionMadeBy(const Node* node)
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

void Graph::noteClassesDefined()
{
    ClassesOfProgram* classes = classesOfProgram();
    const FunctionsOfProgram* functions = functionsOfProgram();
    if (!classes || !functions || !TypeTable::typedFieldsAreEnforced())
        return;
    for (BasicBlock* block : m_rpo) {
        for (Node* note : block->nodes) {
            uint32_t classType = classNotedBy(note);
            if (!classType)
                continue;
            CallOperands operands = operandsOfCall(note->instruction);
            Node* constructor = note->use(operands.argument(0));
            Node* prototype = note->use(operands.argument(1));
            uint16_t layoutID = TypeTable::shared()->layoutIDOfInstancesOf(classType);
            auto noteThisInCodeOf = [&](Node* value) -> uint32_t {
                UnlinkedFunctionExecutable* made = functionMadeBy(value);
                uint32_t number = made ? functions->numberOf(made) : 0;
                if (const KnownFunction* function = functions->function(number)) {
                    classes->noteThisIn(function->forCall, layoutID);
                    classes->noteThisIn(function->forConstruct, layoutID);
                }
                return number;
            };
            noteThisInCodeOf(constructor);
            for (BasicBlock* otherBlock : m_rpo) {
                for (Node* node : otherBlock->nodes) {
                    if (node->isBytecode(op_define_data_property)) {
                        auto bytecode = node->as<OpDefineDataProperty>();
                        if (node->use(bytecode.m_base) != prototype)
                            continue;
                        uint32_t number = noteThisInCodeOf(node->use(bytecode.m_value));
                        Node* property = node->use(bytecode.m_property);
                        if (!number || property->kind != NodeKind::ConstantCell || !property->reg.isConstant())
                            continue;
                        JSValue name = property->graph->codeBlock()->getConstant(property->reg);
                        const StringImpl* impl = name && name.isString() ? asString(name)->tryGetValueImpl() : nullptr;
                        if (!impl || !impl->isAtom())
                            continue;
                        auto* uid = static_cast<UniquedStringImpl*>(const_cast<StringImpl*>(impl));
                        if (TypeTable::shared()->isNonEscapingMethod(classType, uid))
                            classes->recordNonEscapingMethod(classType, uid, number);
                    } else if (node->isBytecode(op_put_by_id)) {
                        auto bytecode = node->as<OpPutById>();
                        if (node->use(bytecode.m_base) == constructor && node->graph->codeBlock()->identifier(bytecode.m_property) == m_vm.propertyNames->builtinNames().instanceFieldInitializerPrivateName())
                            noteThisInCodeOf(node->use(bytecode.m_value));
                    }
                }
            }
        }
    }
}

uint32_t Graph::closedMethodReadBy(const Node* node)
{
    if (!node->isBytecode(op_get_by_id) || !Options::useAOTTypedFields() || !TypeTable::typedFieldsAreEnforced() || !classesOfProgram())
        return 0;
    uint32_t tag = typeTagOf(node);
    if (!tag)
        return 0;
    UniquedStringImpl* name = node->graph->codeBlock()->identifier(node->as<OpGetById>().m_property).impl();
    uint32_t classType = TypeTable::shared()->classOfMethodReadBy(tag, name);
    return classType ? classesOfProgram()->closedMethod(classType, name) : 0;
}

Type Graph::typeOfThisOnEntry() const
{
    // A class field initializer is only called by the constructor, on the instance that was just allocated.
    if (m_codeBlock->parseMode() == SourceParseMode::ClassFieldInitializerMode) {
        if (uint16_t layoutID = layoutIDOfThis())
            return typeOfObjectWithLayout(layoutID);
    }
    return typeOfArgumentOnEntry(0);
}

uint16_t Graph::layoutIDOfThis() const
{
    if (!Options::useAOTTypedFields() || !TypeTable::typedFieldsAreEnforced() || !classesOfProgram())
        return 0;
    return classesOfProgram()->layoutIDOfThisIn(m_codeBlock);
}

uint16_t Graph::layoutIDOfNewObject(const Node* node)
{
    if (!Options::useAOTTypedFields() || !TypeTable::hasTypedFields() || !(Options::aotShapeOptimizations() & 1))
        return 0;
    uint32_t tag = typeTagOf(node);
    return tag ? TypeTable::shared()->layoutIDOfAllocation(tag) : 0;
}

std::optional<KnownShape> Graph::shapeOfLiteral(const Node* node) const
{
    if (node->graph != this)
        return node->graph->shapeOfLiteral(node);
    unsigned count = node->numberOfLiteralProperties;
    std::optional<TypeTable::Layout> layout;
    if (uint32_t tag = typeTagOf(node); tag && (Options::aotShapeOptimizations() & 1) && TypeTable::shared())
        layout = TypeTable::shared()->layoutOf(tag);
    if ((!count && !layout && !layoutIDOfNewObject(node)) || count > KnownShape::maxProperties)
        return std::nullopt;
    KnownShape shape;
    shape.inlineCapacity = KnownShape::inlineCapacityFor(count);
    // Collects the property names the same way operationAOTNewObjectLiteral() does.
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
    // Use the layout from the type table only if it describes this literal: the same names in the same order. All properties must
    // fit inline, with the ones that have no slot in the typed layout after the ones that do. A literal that does not fit is
    // initialized one property at a time.
    if (layout && layout->properties.size() == count && layout->capacity <= JSFinalObject::maxInlineCapacity) {
        bool isAsWritten = true;
        for (unsigned i = 0; i < count; ++i)
            isAsWritten &= layout->properties[i].first == shape.names[i];
        if (isAsWritten) {
            shape.number = layout->number;
            shape.layoutID = layout->layoutID;
            shape.reserved = layout->layoutID ? layout->capacity : 0;
            shape.inlineSlots = layout->inlineSlots;
            for (auto& property : layout->properties)
                shape.slots.append(property.second);
            // (An object with a closed typed layout gets room for the properties it can ever have, which may be fewer than the
            // layout has names for.)
            shape.inlineCapacity = KnownShape::inlineCapacityFor(layout->layoutID ? std::min<unsigned>(layout->inlineSlots, layout->capacity) : layout->capacity);
            return shape;
        }
    }
    // Otherwise, if the allocation has a typed layout, each property goes in the slot the layout assigns to its name.
    if (uint16_t number = layoutIDOfNewObject(node)) {
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
    // (The allocation has no typed layout. The object may later be used as a type that needs more inline slots than the literal has
    // properties, so reserve them now.)
    if (Options::useAOTTypedFields() && TypeTable::hasTypedFields()) {
        if (unsigned wanted = TypeTable::shared()->inlineSlotsNeededFor(shape.names.span()); wanted > count)
            shape.inlineCapacity = std::max(shape.inlineCapacity, KnownShape::inlineCapacityFor(wanted));
    }
    if (count < 2 && shape.inlineCapacity == KnownShape::inlineCapacityFor(count))
        return std::nullopt;
    return shape;
}

unsigned Graph::indexOfKnownCallee(const ImageKey& key)
{
    if (!isOutermost())
        return outermost().indexOfKnownCallee(key);
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
                // An import or a global. Which one, and where it is, is only known once the modules are linked.
                result.kind = StaticVariable::Unresolved;
                return result;
            }
            break;
        }
        case ScopeChainEntry::Unknown:
            if (m_declaredNames && type == GlobalProperty) {
                auto resolution = m_declaredNames->resolve(uid);
                if (resolution.kind == DeclaredNamesLink::Resolution::Slot) {
                    // (Not a local of this function: the bytecode generator would have resolved that itself.)
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
                    // Assigning to an import throws. That is left to the importing module's environment, so a name that is assigned
                    // to is not accessed at a static location.
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
            // Resolved at run time. The operations that fill the inline caches only cache bindings whose location cannot change.
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
    // A depth that would collide with Site::resolvesInGlobalScopes does not fit in the field.
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

void Graph::computeOrderOfBlocks()
{
    // The same algorithm as Parser::computeReversePostOrder(), which has already set isInLoop for the loops of each function.
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
            out.print(" .", graph->codeBlock()->identifier(as<OpGetById>().m_property).impl(), Graph::closedMethodReadBy(this) ? " (a closed method)" : "", isReadOnlyToBeCalled ? " (only to be called)" : "");
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
        case GuardKind::Field:
            out.print("GuardField(", opcode, " bc#", bytecodeIndex.offset(), ", slot ", slotOfField, " of #", firstLayout, "..", lastLayout, ")");
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
        case GuardKind::TypedArrayStorage:
            out.print("GuardTypedArrayStorage");
            break;
        case GuardKind::IsIntrinsicOfArray:
            out.print("IsIntrinsicOfArray ", intrinsic, " ");
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
        // The registers the instruction reads as a whole. A register that one of its own checkpoints wrote earlier does not count.
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

    // Whether the instruction has a fast path that is worth a guard: when the guard fails, execution leaves the fast copy of the
    // loop.
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
        case op_check_type:
            return true;
        default:
            return false;
        }
    }

    // A function that only computes a value from its arguments, in straight-line code, after checking their types. In the fast copy
    // of a loop, a call to one is replaced by that computation. If the callee turns out to be another function, or a check fails,
    // nothing observable has happened yet, and the generic copy makes the call.
    //
    // Every operation must be one that the types make simple. The lowering of anything else calls the runtime, on behalf of an
    // instruction that does not belong to the caller.
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

    // Appends the inlined body of the call and its guards to the block, and returns the result. Returns null if the call cannot be
    // inlined after all.
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

    // What the fast copy of a loop does better than code that uses inline caches: unboxed numbers, indexed element accesses, and
    // calls that are inlined. A loop with none of these gains nothing from being split.
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

    // The typed field that the instruction accesses, according to the type of its base (GuardKind::Field).
    std::optional<TypeTable::Field> fieldReadBy(unsigned offset)
    {
        if (!Options::useAOTFunctionSplitting() || !TypeTable::shared())
            return std::nullopt;
        uint32_t tag = m_graph.typeTagAt(offset);
        if (!tag)
            return std::nullopt;
        const JSInstruction* instruction = m_instructions.at(offset).ptr();
        unsigned identifier;
        if (instruction->opcodeID() == op_get_by_id && (Options::aotShapeOptimizations() & 2))
            identifier = instruction->as<OpGetById>().m_property;
        else if (instruction->opcodeID() == op_put_by_id && (Options::aotShapeOptimizations() & 4) && !instruction->as<OpPutById>().m_flags.isDirect())
            identifier = instruction->as<OpPutById>().m_property;
        else
            return std::nullopt;
        return TypeTable::shared()->fieldOf(tag, m_codeBlock->identifier(identifier).impl());
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

    // With typed fields: the typed layout of the base of the access that follows the op_type_tag at `offset`, and the register that
    // holds the base.
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

    // Likewise: the register that holds the base of the access that follows the op_type_tag at `offset`, if the type table says it
    // is an array.
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

    // With Options::useAOTTypedFields() and without function splitting, an access that relies on the type of its base does so
    // inside loops as well as outside, and needs no guard.
    bool usesTypedAccessWithoutGuard(unsigned offset)
    {
        if (!Options::useAOTTypedFields() || Options::useAOTFunctionSplitting() || !TypeTable::shared())
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

    // Chooses where the guards go, once the blocks and loops are known. Returns false if there are none.
    bool chooseGuards()
    {
        if (!Options::useAOTLoopSplitting() || !Options::aotLoopSplittingPolicy() || !usesStubs || m_graph.loopsAreNotSplit)
            return false;
        unsigned size = m_instructions.size();
        // With Options::useAOTFunctionSplitting() the whole function has two copies, not only its loops.
        BitVector fieldAccesses;
        if (Options::useAOTFunctionSplitting() && TypeTable::shared()) {
            BitVector partOfAllocation;
            unsigned count = 0;
            for (const auto& instruction : m_instructions) {
                unsigned offset = instruction.offset();
                if (instruction->opcodeID() == op_new_object) {
                    for (unsigned store : m_graph.storesOfLiteral(offset))
                        partOfAllocation.set(store);
                } else if (instruction->opcodeID() == op_create_this) {
                    for (auto& store : NewObjectPlan::forCreateThis(m_instructions, offset).stores)
                        partOfAllocation.set(store.offset);
                }
                if (!partOfAllocation.get(offset) && fieldReadBy(offset)) {
                    fieldAccesses.set(offset);
                    ++count;
                }
            }
            if (count < std::max(1u, Options::minimumTypedAccessesForAOTFunctionSplitting()))
                fieldAccesses.clearAll();
        }
        bool hasTwoCopiesOfAll = !fieldAccesses.isEmpty();
        m_graph.hasTwoCopiesOfAll = hasTwoCopiesOfAll;
        struct OfBlock {
            Vector<unsigned, 8> guards;
            bool benefitsFromFastCopy { false };
            bool hasRealCall { false };
            // An inlined call or an indexed element access: each iteration makes one call fewer.
            bool benefitsGreatlyFromFastCopy { false };
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
                    for (unsigned store : m_graph.storesOfLiteral(offset))
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
                bool isGuarded = canBeGuarded(instruction) && !usesTypedAccessWithoutGuard(offset) && !isCheckSubsumedByAssertion(offset, block->bytecodeEnd);
                if (isGuarded)
                    ofBlock.guards.append(offset);
                OpcodeID opcode = instruction->opcodeID();
                if (isCallOrTheLike(opcode)) {
                    if (isGuarded) {
                        ofBlock.benefitsFromFastCopy = true;
                        ofBlock.benefitsGreatlyFromFastCopy = true;
                    } else
                        ofBlock.hasRealCall = true;
                } else if (opcodeBenefitsFromFastCopy(opcode)) {
                    ofBlock.benefitsFromFastCopy = true;
                    if (opcode == op_get_by_val || opcode == op_put_by_val)
                        ofBlock.benefitsGreatlyFromFastCopy = true;
                }
            }
        }

        // The body of a loop is every block that reaches a back edge to its header without passing through the header. A loop
        // nested in a split loop is split too.
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
            bool hasRealCall = false;
            for (unsigned index : body) {
                hasWhatIsBetter |= ofBlocks[index].benefitsFromFastCopy;
                hasWhatIsMuchBetter |= ofBlocks[index].benefitsGreatlyFromFastCopy;
                hasRealCall |= ofBlocks[index].hasRealCall;
            }
            bool isProfitable = true;
            switch (Options::aotLoopSplittingPolicy()) {
            case 2:
                isProfitable = hasWhatIsBetter;
                break;
            case 3:
                isProfitable = !hasRealCall;
                break;
            case 4:
                isProfitable = hasWhatIsBetter && !hasRealCall;
                break;
            case 5:
                isProfitable = hasWhatIsMuchBetter || !hasRealCall;
                break;
            default:
                break;
            }
            if (isProfitable)
                hasTwoCopies.merge(body);
        }

        bool found = false;
        for (BasicBlock* block : m_graph.m_rpo) {
            bool isInLoopWithTwoCopies = block->isInLoop && hasTwoCopies.get(block->index);
            if (!isInLoopWithTwoCopies && (!hasTwoCopiesOfAll || block == m_graph.root))
                continue;
            if (m_inLoop.isEmpty()) {
                m_inLoop.ensureSize(size + 1);
                m_guards.ensureSize(size + 1);
                m_loopHeaders.ensureSize(size + 1);
            }
            if (block->isLoopHeader && isInLoopWithTwoCopies)
                m_loopHeaders.set(block->bytecodeBegin);
            for (unsigned offset = block->bytecodeBegin; offset < block->bytecodeEnd; offset += m_instructions.at(offset)->size()) {
                m_inLoop.set(offset);
                if (fieldAccesses.get(offset)) {
                    m_guards.set(offset);
                    found = true;
                }
            }
            if (!isInLoopWithTwoCopies)
                continue;
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
                    // The header of a split loop gets three blocks: a reentry block, a pre-header and the header. The first two are
                    // empty and end with a guard, and each is immediately followed by the block it falls through to.
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
                // A guard ends its block. The next block is the one it falls through to.
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
        // Visit the handlers first. They then come last in the reverse post order, which keeps the root at the front.
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

        // The body of a loop: every block that reaches the back edge without passing through the header.
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

    // The instruction after the block counts too, because inlineCall() may make it part of the block.
    template<typename Handler>
    static bool isCoveredBy(BasicBlock* block, const Handler& handler)
    {
        return block->bytecodeBegin < handler.end && handler.start <= block->bytecodeEnd;
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
        // Any instruction of a block that a handler covers may jump to the handler. A block that ends in op_throw has no other successor.
        Vector<Vector<BasicBlock*, 2>> handlersOfBlocks(m_graph.blocks.size());
        for (unsigned i = 0; i < m_codeBlock->numberOfExceptionHandlers(); ++i) {
            auto& handler = m_codeBlock->exceptionHandler(i);
            BasicBlock* target = m_graph.blockForOffset[handler.target];
            for (BasicBlock* block : m_graph.m_rpo) {
                if (isCoveredBy(block, handler))
                    handlersOfBlocks[block->index].append(target);
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
                // A definition in the block does not end this liveness, because the jump may happen before it.
                for (BasicBlock* handler : handlersOfBlocks[block->index])
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
        unsigned index; // The index in the block's nodes where the store would have been.
        VirtualRegister reg;
        Node* value;
    };
    Vector<SkippedStore> m_skippedStores;
    bool m_needsEveryStore { false };
    Vector<BitVector> m_defsOfBlocks;

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

        // Registers are kept in stack slots for exception handlers, which can only read values from memory. A register number is
        // reused for unrelated values, so a store is only needed where a handler that reads the register can still be reached. This
        // computes, for each block, the registers read by the handlers that cover it and by handlers reachable after it.
        m_skippedStores.clear();
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
                if (isCoveredBy(block, handler))
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

    // Emits the stores that were skipped, for when a register turns out to be read from its stack slot somewhere other than on
    // entry to a handler.
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
            // (A JSTemplateObjectDescriptor constant is replaced by the template object, an array, before the code runs.)
            node->type = value.asCell()->inherits<JSTemplateObjectDescriptor>() ? TArray : typeOfValue(value);
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
            node->type = TTop; // The constant is known, but its value depends on the realm.
            return append(block, node);
        }
        if (reg.isConstant())
            return constantFor(reg);
        if (reg == VirtualRegister(CallFrameSlot::callee)) {
            // Nothing writes the callee slot, so it is read at each use.
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
        if (!value) {
            // Bytecode liveness says nothing reads this register. It is only read on a path where it was never written, so it holds
            // the undefined that op_enter stored.
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
        if (m_graph.livesInFrame(reg)) {
            unsigned index = m_graph.registerIndex(reg);
            if (m_needsEveryStore || m_graph.m_arrayOperandRegisters.get(index) || block->readByHandlersOfBlock.get(index) || block->readByHandlersAfterBlock.get(index)) {
                Node* node = m_graph.addNode(NodeKind::SetStack);
                node->reg = reg;
                node->uses.append({ VirtualRegister(), value });
                append(block, node);
                // The store is for handlers, which can only read the value from memory. Other uses get the value directly.
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
                node->type = i ? m_graph.typeOfArgumentOnEntry(i) : m_graph.typeOfThisOnEntry();
                append(block, node);
                set(block, node->reg, node);
            }
            // Every local starts as undefined, as op_enter will store again. A register kept in a stack slot needs the slot to hold
            // a value from the start.
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
            // (See Graph::typeTagAt().)
            if (opcode == op_type_tag) {
                // With typed layouts, op_type_tag is also a check: the base of the next instruction has the typed layout, or this
                // throws. Later uses of the value can rely on the layout, so the node is a definition, as op_check_type is.
                auto [layoutID, baseRegister] = layoutCheckedAt(offset);
                bool isArray = false;
                if (!layoutID) {
                    baseRegister = arrayAssertedAt(offset);
                    isArray = baseRegister.isValid();
                }
                if ((!layoutID && !isArray) || !m_graph.isTracked(baseRegister))
                    continue;
                Node* base = get(block, baseRegister);
                Node* node = m_graph.addNode(NodeKind::Bytecode);
                node->opcode = opcode;
                node->instruction = instruction;
                // (If it throws, it does so on behalf of the next instruction, with the error that instruction would have thrown.)
                node->bytecodeIndex = BytecodeIndex(offset + instruction->size());
                node->uses.append({ baseRegister, base });
                node->firstLayout = node->lastLayout = layoutID;
                // (The `this` of a function that escapes can be any value its callers choose.)
                if (layoutID)
                    node->isTrusted = TypeTable::shared()->isTrusted(m_graph.typeTagAt(offset + instruction->size())) && !(TypeTable::shared()->isOpen(layoutID) && Graph::isThisOfEscapingFunction(base));
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
            // The op_type_tag that follows checks for a narrower type.
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
                // One of the stores whose values the object is going to be allocated with.
                auto& stores = m_planOfObject.stores;
                RELEASE_ASSERT(m_nextStoreOfPlan < stores.size() && stores[m_nextStoreOfPlan].offset == offset);
                VirtualRegister reg = NewObjectPlan::registerOf(stores[m_nextStoreOfPlan].property);
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
                if (++m_nextStoreOfPlan == stores.size()) {
                    append(block, m_pendingCreateThis);
                    m_pendingCreateThis = nullptr;
                }
                continue;
            }

            switch (opcode) {
            case op_new_object: {
                VirtualRegister reg = instruction->as<OpNewObject>().m_dst;
                // A register that a handler reads from its stack slot has to be written at the instruction itself.
                if (!m_graph.isTracked(reg) || m_graph.isLiveIntoHandler(reg))
                    break;
                auto stores = m_graph.storesOfLiteral(offset);
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
                // A register that is kept in its stack slot has to be written at the instruction itself.
                // (An instance with a typed layout has each field in the slot the layout assigns to its name, not in the order of
                // the stores, so its fields are stored one at a time.)
                if (m_graph.hasFrameRegisters() || block->isInLoop || m_graph.layoutIDOfThis())
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
                m_pendingCreateThis = node;
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
                // op_enter also puts the callee's scope in the scope register.
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
                // This is a branch that also writes a register, which one node cannot represent: a terminal has to come last, and
                // its definition has to be available to the phis of the successors and to the store to a stack slot. So it becomes
                // two nodes. The first defines the iterator as the instruction leaves it. The second is the branch, which only
                // needs that value to choose its target (see Lowering::lowerIteratorCloseCheck()).
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

            if (comesAfterGuard && block->predecessors[0]->terminal()->guardKind == GuardKind::Field) {
                // The base passed the guard, so later uses of the value can rely on its typed layout.
                Node* guard = block->predecessors[0]->terminal();
                VirtualRegister baseRegister = opcode == op_get_by_id ? instruction->as<OpGetById>().m_base : instruction->as<OpPutById>().m_base;
                Node* base = get(block, baseRegister);
                Node* narrow = m_graph.addNode(NodeKind::Narrow);
                narrow->reg = baseRegister;
                narrow->bytecodeIndex = BytecodeIndex(offset);
                narrow->uses.append({ VirtualRegister(), base });
                // (An object whose layout lacks the property passes the guard too.)
                narrow->firstLayout = guard->firstWithout ? std::min(guard->firstLayout, guard->firstWithout) : guard->firstLayout;
                narrow->lastLayout = std::max(guard->lastLayout, guard->lastWithout);
                narrow->narrowedTo = typeOfObjectWithLayoutInRange(narrow->firstLayout, narrow->lastLayout);
                append(block, narrow);
                for (unsigned index = 0; index < block->valuesAtTail.size(); ++index) {
                    if (block->valuesAtTail[index] == base && !m_graph.m_frameRegisters.get(index))
                        block->valuesAtTail[index] = narrow;
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
                VirtualRegister thisRegister = Graph::operandsOfCall(instruction).argument(0);
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
                // op_check_type does not change the register, but afterwards more is known about the value. So the check is a
                // definition of the same value with a narrower type.
                VirtualRegister reg = instruction->as<OpCheckType>().m_value;
                if (m_graph.isTracked(reg)) {
                    node->reg = reg;
                    // (The stack slot already holds the same value.)
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
            if (auto field = fieldReadBy(block->bytecodeEnd)) {
                guard->guardKind = GuardKind::Field;
                guard->slotOfField = field->slot;
                guard->firstLayout = field->first;
                guard->lastLayout = field->last;
                if (guard->opcode == op_get_by_id && (Options::aotShapeOptimizations() & 8)) {
                    guard->firstWithout = field->firstWithout;
                    guard->lastWithout = field->lastWithout;
                }
                if (Options::useAOTTypedFields() && field->fieldType.isConstrained()) {
                    guard->fieldTypeKinds = safeCast<uint16_t>(field->fieldType.kinds);
                    guard->fieldTypeFirst = field->fieldType.first;
                    guard->fieldTypeLast = field->fieldType.last;
                }
            }
            append(block, guard);
            return;
        }

        // A terminal has to be the last node of its block, so it cannot be followed by a SetStack or a Proj.
        if (Node* last = block->terminal(); last && last->kind != NodeKind::Bytecode) {
            for (Node* node : block->nodes) {
                if (node->kind == NodeKind::Bytecode && (isBranch(node->opcode) || isTerminal(node->opcode) || isThrow(node->opcode))) {
                    m_graph.fail("terminal defines a register"_s, node->opcode);
                    return;
                }
            }
        }
    }

    // The value of the register on the edge from one block to the other.
    Node* valueLeaving(BasicBlock* from, BasicBlock* to, unsigned index)
    {
        Node* value = from->valuesAtTail[index];
        // On the edge taken when the guard fails, the value keeps the type it had before.
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
        // A phi that only one value reaches is replaced by that value. The phis of a variable that a loop does not modify have each
        // other as inputs, all the way around the loop and through both of its copies. So this starts by assuming that no value
        // reaches any phi, and iterates to a fixpoint.
        UncheckedKeyHashMap<Node*, Node*> origins; // Missing: no value reaches it so far. Itself: more than one value does.
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
                // The target of a variable that the loop does not modify is the pre-header's phi, if it still exists.
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
    // Indexed by bytecode offset. Set once the loops are known.
    bool m_hasGuards { false };
    BitVector m_leaders;
    BitVector m_inLoop;
    BitVector m_guards;
    BitVector m_loopHeaders;
    struct PendingLiteral {
        Node* node; // The op_new_object, which is moved to where the last of its stores is.
        Vector<unsigned, 4> stores;
        unsigned next;
    };
    Vector<PendingLiteral, 2> m_pendingLiterals; // Innermost last.
    Node* m_pendingCreateThis { nullptr }; // An op_create_this that is moved to where the last of its stores is.
    NewObjectPlan m_planOfObject;
    unsigned m_nextStoreOfPlan { 0 };
    Vector<std::pair<BasicBlock*, BasicBlock*>> m_backEdges;
    Vector<std::pair<VirtualRegister, unsigned>, 8> m_recentProperties; // In the current block: the property that op_get_by_id read into each register.
    Vector<std::pair<VirtualRegister, const KnownFunction*>, 8> m_recentFunctions; // Likewise, the function that op_get_from_scope probably read.
    UncheckedKeyHashMap<UnlinkedFunctionCodeBlock*, bool> m_canBeInlined;
    UncheckedKeyHashMap<unsigned, Node*, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_resultsOfInlinedCalls; // Keyed by the bytecode offset of the call.
};

} // anonymous namespace

bool parseBytecode(Graph& graph)
{
    Parser parser(graph);
    return parser.run();
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
