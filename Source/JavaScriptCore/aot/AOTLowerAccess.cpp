/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(FTL_JIT)

#include "B3PatchpointValue.h"
#include "BytecodeStructs.h"
#include "JSCInlines.h"
#include "JSLexicalEnvironment.h"
#include "SymbolTable.h"
#include "UnlinkedCodeBlock.h"

namespace JSC { namespace AOT {

using namespace B3;

// The slot's first word is { structureID, offset }, low half first.
static LValue lowHalf(FTL::Output& out, LValue word) { return out.castToInt32(word); }
static LValue highHalf(FTL::Output& out, LValue word) { return out.castToInt32(out.lShr(word, out.constInt32(32))); }

LValue Lowering::loadProperty(LValue object, LValue offset)
{
    LBasicBlock inlineCase = m_out.newBlock();
    LBasicBlock outOfLineCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.lessThan(offset, m_out.constInt32(firstOutOfLineOffset)), unsure(inlineCase), unsure(outOfLineCase));

    m_out.appendTo(inlineCase, outOfLineCase);
    LValue inlineAddress = m_out.add(object, m_out.add(m_out.shl(m_out.signExt32ToPtr(offset), m_out.constInt32(3)), m_out.constIntPtr(JSObject::offsetOfInlineStorage())));
    ValueFromBlock inlineResult = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), inlineAddress)));
    m_out.jump(continuation);

    m_out.appendTo(outOfLineCase, continuation);
    LValue butterfly = m_out.loadPtr(object, m_heaps.JSObject_butterfly);
    // Out of line property n is at butterfly[-(n - firstOutOfLineOffset) - 2].
    LValue index = m_out.sub(m_out.constIntPtr(firstOutOfLineOffset - 2), m_out.signExt32ToPtr(offset));
    LValue outOfLineAddress = m_out.add(butterfly, m_out.shl(index, m_out.constInt32(3)));
    ValueFromBlock outOfLineResult = m_out.anchor(m_out.load64(TypedPointer(m_heaps.properties.atAnyNumber(), outOfLineAddress)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int64, inlineResult, outOfLineResult);
}

// word: the first of a slot whose offset is the location of a property (locationOfProperty()).
TypedPointer Lowering::cachedPropertyAddress(LValue object, LValue word, const AbstractHeap* heap)
{
    LValue location = m_out.aShr(m_out.shl(word, m_out.constInt32(32 - Slot::offsetBits)), m_out.constInt32(64 - Slot::offsetBits));
    LValue storage = m_out.select(m_out.lessThan(location, m_out.int64Zero), m_out.loadPtr(object, m_heaps.JSObject_butterfly), object);
    return TypedPointer(heap ? *heap : m_heaps.properties.atAnyNumber(), m_out.add(storage, m_out.shl(location, m_out.constInt32(3))));
}

void Lowering::lowerGetById(Node* node)
{
    auto bytecode = node->as<OpGetById>();
    Node* baseNode = node->use(bytecode.m_base);
    setJSValue(node, getByIdCached(node, lowJSValue(baseNode), baseNode->type, Entry::operationAOTGetById, bytecode.m_property));
}

// operation: takes the global object, the base, identifier (whatever that means to it) and the cache, which it fills.
LValue Lowering::getByIdCached(Node* node, LValue base, Type baseType, Entry operation, unsigned identifier)
{
    std::optional<Stub> stub;
    if (usesStubs && Site::fits(identifier, 0)) {
        if (operation == Entry::operationAOTGetById)
            stub = Stub::GetById;
        else if (operation == Entry::operationAOTGetByIdWellKnown)
            stub = Stub::GetByIdWellKnown;
    }
    unsigned slot = stub ? sharedSite(node, identifier) : allocateSlot();
    auto throughStub = [&]() -> LValue {
        return callStub(*stub, Int64, { { base, GPRInfo::argumentGPR0 }, { slotAddress(slot), GPRInfo::argumentGPR1 } }, { });
    };
    if (stub && isCompact())
        return throughStub();

    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock rightStructure = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    if (isSubtype(baseType, TCell))
        m_out.jump(cellCase);
    else
        m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

    m_out.appendTo(cellCase, rightStructure);
    LValue word = m_out.load64(slotWord(slot, 0));
    m_out.branch(m_out.equal(m_out.load32(base, m_heaps.JSCell_structureID), lowHalf(m_out, word)), usually(rightStructure), rarely(slowCase));

    // Calling a getter is the stub's business.
    m_out.appendTo(rightStructure, hit);
    m_out.branch(m_out.testIsZero64(word, m_out.constInt64(static_cast<int64_t>(Slot::isGetter) << 32)), usually(hit), rarely(slowCase));

    // In the base, or in an object that every base of this structure inherits it from (see cacheGetById()).
    m_out.appendTo(hit, slowCase);
    LValue holder = m_out.loadPtr(slotWord(slot, 1));
    ValueFromBlock fastResult = m_out.anchor(m_out.load64(cachedPropertyAddress(m_out.select(m_out.notNull(holder), holder, base), word)));
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    ValueFromBlock slowResult = m_out.anchor(stub ? throughStub() : vmCall(node, Int64, operation, m_globalObject, base, m_out.constInt32(identifier), slotAddress(slot)));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return m_out.phi(Int64, fastResult, slowResult);
}

void Lowering::lowerPutById(Node* node)
{
    auto bytecode = node->as<OpPutById>();
    Node* baseNode = node->use(bytecode.m_base);
    Node* valueNode = node->use(bytecode.m_value);
    LValue base = lowJSValue(baseNode);
    LValue value = lowJSValue(valueNode);
    uint32_t flags = (bytecode.m_flags.isDirect() ? 1 : 0) | (bytecode.m_flags.ecmaMode().isStrict() ? 2 : 0);
    if (isCompact() && Site::fits(bytecode.m_property, flags)) {
        callStub(Stub::PutById, Void, { { base, GPRInfo::argumentGPR0 }, { value, GPRInfo::argumentGPR1 }, { slotAddress(sharedSite(node, bytecode.m_property, flags)), GPRInfo::argumentGPR2 } }, { });
        return;
    }
    unsigned slot = allocateSlot();

    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock hit = m_out.newBlock();
    LBasicBlock transition = m_out.newBlock();
    LBasicBlock stored = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    if (isSubtype(baseNode->type, TCell))
        m_out.jump(cellCase);
    else
        m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

    m_out.appendTo(cellCase, hit);
    LValue word = m_out.load64(slotWord(slot, 0));
    m_out.branch(m_out.equal(m_out.load32(base, m_heaps.JSCell_structureID), lowHalf(m_out, word)), usually(hit), rarely(slowCase));

    m_out.appendTo(hit, transition);
    m_out.store64(value, cachedPropertyAddress(base, word));
    LValue newStructureID = lowHalf(m_out, m_out.load64(slotWord(slot, 1)));
    m_out.branch(m_out.notZero32(newStructureID), unsure(transition), unsure(stored));

    m_out.appendTo(transition, stored);
    m_out.store32(newStructureID, base, m_heaps.JSCell_structureID);
    m_out.jump(stored);

    m_out.appendTo(stored, slowCase);
    if (mayBe(valueNode->type, TCell))
        storeBarrier(base);
    m_out.jump(continuation);

    m_out.appendTo(slowCase, continuation);
    vmCall(node, Void, Entry::operationAOTPutById, m_globalObject, base, value, m_out.constInt32(bytecode.m_property), slotAddress(slot), m_out.constInt32(flags));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// The property as an array index, on the way to haveIndex. Everything that is not one goes to notIndex.
LValue Lowering::lowIndex(Node* propertyNode, LBasicBlock haveIndex, LBasicBlock notIndex)
{
    if (propertyNode->rep() == Rep::Int32) {
        LValue index = lowInt32(propertyNode);
        m_out.jump(haveIndex);
        return index;
    }
    if (propertyNode->rep() == Rep::Int64) {
        LValue wide = lowRaw(propertyNode);
        LValue index = m_out.castToInt32(wide);
        m_out.branch(m_out.belowOrEqual(wide, m_out.constInt64(INT32_MAX)), usually(haveIndex), rarely(notIndex));
        return index;
    }
    if (propertyNode->rep() == Rep::Double) {
        LValue asDouble = lowDouble(propertyNode);
        LValue index = m_out.doubleToInt32(asDouble);
        m_out.branch(m_out.doubleEqual(m_out.intToDouble(index), asDouble), usually(haveIndex), rarely(notIndex));
        return index;
    }
    LValue property = lowJSValue(propertyNode);
    LValue index = unboxInt32(property);
    m_out.branch(isInt32(property), usually(haveIndex), unsure(notIndex));
    return index;
}

void Lowering::lowerGetByVal(Node* node)
{
    auto bytecode = node->as<OpGetByVal>();
    Node* baseNode = node->use(bytecode.m_base);
    Node* propertyNode = node->use(bytecode.m_property);
    LValue base = lowJSValue(baseNode);

    if (isCompact()) {
        setJSValue(node, callBinaryStub(node, Stub::GetByVal, Int64, base, lowJSValue(propertyNode)));
        return;
    }

    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;

    // An in-bounds, non-hole element of an array-like with contiguous or int32 storage.
    if (!(Options::aotDisableFastPaths() & 8) && mayBe(baseNode->type, TAnyObject) && mayBe(propertyNode->type, TNumber)) {
        LBasicBlock haveIndex = m_out.newBlock();
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock rightShape = m_out.newBlock();
        LBasicBlock inBounds = m_out.newBlock();

        LValue index = lowIndex(propertyNode, haveIndex, slowCase);

        m_out.appendTo(haveIndex, cellCase);
        if (isSubtype(baseNode->type, TCell))
            m_out.jump(cellCase);
        else
            m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

        m_out.appendTo(cellCase, rightShape);
        LValue shape = m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask));
        // Int32Shape and ContiguousShape hold JSValues; DoubleShape sits between them.
        LValue isJSValueShape = m_out.bitOr(m_out.equal(shape, m_out.constInt32(Int32Shape)), m_out.equal(shape, m_out.constInt32(ContiguousShape)));
        m_out.branch(isJSValueShape, usually(rightShape), rarely(slowCase));

        m_out.appendTo(rightShape, inBounds);
        LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), usually(inBounds), rarely(slowCase));

        m_out.appendTo(inBounds, slowCase);
        LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index)));
        results.append(m_out.anchor(element));
        m_out.branch(m_out.notZero64(element), usually(continuation), rarely(slowCase));
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    if constexpr (usesStubs)
        results.append(m_out.anchor(callBinaryStub(node, Stub::GetByVal, Int64, base, lowJSValue(propertyNode))));
    else
        results.append(m_out.anchor(vmCall(node, Int64, Entry::operationAOTGetByVal, m_globalObject, base, lowJSValue(propertyNode))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

void Lowering::lowerPutByVal(Node* node)
{
    auto bytecode = node->as<OpPutByVal>();
    Node* baseNode = node->use(bytecode.m_base);
    Node* propertyNode = node->use(bytecode.m_property);
    Node* valueNode = node->use(bytecode.m_value);
    LValue base = lowJSValue(baseNode);
    LValue value = lowJSValue(valueNode);

    auto throughStub = [&] {
        callStub(Stub::PutByVal, Void, { { base, GPRInfo::argumentGPR0 }, { lowJSValue(propertyNode), GPRInfo::argumentGPR1 }, { value, GPRInfo::argumentGPR2 } },
            { { GPRInfo::argumentGPR3, bytecode.m_ecmaMode.isStrict() }, { GPRInfo::regT10, CallSiteIndex(node->bytecodeIndex).bits() } });
    };
    if (isCompact()) {
        throughStub();
        return;
    }

    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();

    // An element for which there is room in contiguous storage. Growing that and every other shape are the runtime's.
    if (!(Options::aotDisableFastPaths() & 16) && mayBe(baseNode->type, TAnyObject) && mayBe(propertyNode->type, TNumber)) {
        LBasicBlock haveIndex = m_out.newBlock();
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock rightShape = m_out.newBlock();
        LBasicBlock beyondLength = m_out.newBlock();
        LBasicBlock lengthen = m_out.newBlock();
        LBasicBlock inBounds = m_out.newBlock();

        LValue index = lowIndex(propertyNode, haveIndex, slowCase);

        m_out.appendTo(haveIndex, cellCase);
        if (isSubtype(baseNode->type, TCell))
            m_out.jump(cellCase);
        else
            m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

        m_out.appendTo(cellCase, rightShape);
        // Copy on write storage has the shape bits of what it holds and one more bit.
        LValue indexingMode = m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask | CopyOnWrite));
        m_out.branch(m_out.equal(indexingMode, m_out.constInt32(ContiguousShape)), usually(rightShape), rarely(slowCase));

        m_out.appendTo(rightShape, beyondLength);
        LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), usually(inBounds), unsure(beyondLength));

        m_out.appendTo(beyondLength, lengthen);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_vectorLength)), unsure(lengthen), unsure(slowCase));

        // What is between the old length and here are holes already.
        m_out.appendTo(lengthen, inBounds);
        m_out.store32(m_out.add(index, m_out.int32One), butterfly, m_heaps.Butterfly_publicLength);
        m_out.jump(inBounds);

        m_out.appendTo(inBounds, slowCase);
        m_out.store64(value, m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index)));
        if (mayBe(valueNode->type, TCell))
            storeBarrier(base);
        m_out.jump(continuation);
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    if constexpr (usesStubs)
        throughStub();
    else
        vmCall(node, Void, Entry::operationAOTPutByVal, m_globalObject, base, lowJSValue(propertyNode), value, m_out.constInt32(bytecode.m_ecmaMode.isStrict()));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// ---- Scopes

void Lowering::lowerResolveScope(Node* node)
{
    auto bytecode = node->as<OpResolveScope>();
    if (auto distance = m_graph.distanceOfEnvironmentResolvedTo(node)) {
        setJSValue(node, environmentAt(*distance));
        return;
    }
    LValue scope = lowCell(node->use(bytecode.m_scope));

    auto walk = [&](unsigned depth) {
        LValue current = scope;
        for (unsigned i = 0; i < depth; ++i)
            current = m_out.loadPtr(current, m_heaps.JSScope_next);
        return current;
    };

    if (isStaticClosureVarResolveType(bytecode.m_resolveType)) {
        setJSValue(node, walk(bytecode.m_localScopeDepth + staticClosureVarHops(bytecode.m_resolveType)));
        return;
    }

    StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, bytecode.m_resolveType);
    if (variable.isAtStaticDepth()) {
        setJSValue(node, walk(variable.depth));
        return;
    }
    if (variable.kind == StaticVariable::Import) {
        // The environment of the module that has the variable, which the environment of this one is given the first time it is
        // asked for.
        LValue importer = walk(variable.depth);
        LValue exporter = m_out.load64(importer, m_heaps.JSLexicalEnvironment_variables[variable.import.scopeOffsetOfSlot]);
        LBasicBlock slowCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fastResult = m_out.anchor(exporter);
        m_out.branch(m_out.isZero64(exporter), rarely(slowCase), usually(continuation));
        m_out.appendTo(slowCase, continuation);
        ValueFromBlock slowResult = m_out.anchor(vmCall(node, Int64, Entry::operationAOTFillImportSlot, m_globalObject, importer, m_out.constInt32(variable.import.slot)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, fastResult, slowResult));
        return;
    }

    unsigned extra = m_graph.extraOfResolveScope(bytecode);
    if (isFusedWithGetFromScope(node))
        return;
    if (usesStubs && variable.kind == StaticVariable::Unresolved && Site::fits(bytecode.m_var, extra)) {
        setJSValue(node, callStub(Stub::ResolveScope, Int64, { { scope, GPRInfo::argumentGPR0 }, { slotAddress(sharedSite(node, bytecode.m_var, extra)), GPRInfo::argumentGPR1 } }, { }));
        return;
    }

    unsigned slot = allocateSlot();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 2> results;
    if (variable.kind == StaticVariable::Unresolved) {
        LValue cached = m_out.loadPtr(slotWord(slot, 1));
        LValue epochPlusOne = highHalf(m_out, m_out.load64(slotWord(slot, 0)));
        LValue epoch = m_out.load32(m_out.address(m_heaps.root, m_globalObject, JSGlobalObject::offsetOfGlobalLexicalBindingEpoch()));
        results.append(m_out.anchor(cached));
        m_out.branch(m_out.equal(epochPlusOne, m_out.add(epoch, m_out.constInt32(1))), usually(continuation), rarely(slowCase));
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(vmCall(node, pointerType(), Entry::operationAOTResolveScope, m_globalObject, scope, m_out.constInt32(bytecode.m_var), slotAddress(slot), m_out.constInt32(variable.isInGlobalScopes ? Site::resolvesInGlobalScopes : 0))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

bool Lowering::isFusedWithGetFromScope(Node* node)
{
    if (!usesStubs || !node->isBytecode(op_resolve_scope) || node->block != m_block || node->useCount != 1)
        return false;
    auto resolve = node->as<OpResolveScope>();
    if (isStaticClosureVarResolveType(resolve.m_resolveType) || !Site::fits(resolve.m_var, m_graph.extraOfResolveScope(resolve)))
        return false;
    if (resolveStatically(resolve.m_var, resolve.m_localScopeDepth, resolve.m_resolveType).kind != StaticVariable::Unresolved)
        return false;

    // Asked when lowering either of the two.
    unsigned index = m_block->nodes[m_nodeIndex] == node ? m_nodeIndex : m_nodeIndex - 1;
    if (index >= m_block->nodes.size() || m_block->nodes[index] != node || index + 1 >= m_block->nodes.size())
        return false;
    Node* next = m_block->nodes[index + 1];
    if (!next->isBytecode(op_get_from_scope))
        return false;
    auto get = next->as<OpGetFromScope>();
    ResolveType type = get.m_getPutInfo.resolveType();
    return next->use(get.m_scope) == node && get.m_var == resolve.m_var
        && type != ResolvedClosureVar && type != ResolvedLazyClosureVar
        && resolveStatically(get.m_var, get.m_localScopeDepth, type).kind == StaticVariable::Unresolved;
}

void Lowering::lowerGetFromScope(Node* node)
{
    auto bytecode = node->as<OpGetFromScope>();
    if (Node* resolveNode = node->use(bytecode.m_scope); isFusedWithGetFromScope(resolveNode)) {
        auto resolve = resolveNode->as<OpResolveScope>();
        LValue scope = lowCell(resolveNode->use(resolve.m_scope));
        unsigned site = allocateSite(resolveNode, resolve.m_var, m_graph.extraOfResolveScope(resolve));
        unsigned siteOfGet = allocateSite(node, bytecode.m_var, m_graph.extraOfGetFromScope(bytecode));
        RELEASE_ASSERT(siteOfGet == site + 1);
        setJSValue(node, callStub(Stub::GetGlobal, Int64, { { scope, GPRInfo::argumentGPR0 }, { slotAddress(site), GPRInfo::argumentGPR1 } }, { }));
        return;
    }
    auto distance = m_graph.distanceOfEnvironmentAccessed(node);
    LValue scope = distance ? environmentAt(*distance) : lowCell(node->use(bytecode.m_scope));
    ResolveType type = bytecode.m_getPutInfo.resolveType();

    auto loadClosureVariable = [&](unsigned offset, bool mayBeLazy) {
        LValue value = m_out.load64(scope, m_heaps.JSLexicalEnvironment_variables[offset]);
        if (!mayBeLazy) {
            setJSValue(node, value);
            return;
        }
        LBasicBlock slowCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fastResult = m_out.anchor(value);
        m_out.branch(m_out.isZero64(value), rarely(slowCase), usually(continuation));
        m_out.appendTo(slowCase, continuation);
        ValueFromBlock slowResult = m_out.anchor(vmCall(node, Int64, Entry::operationAOTReadLazyClosureVar, m_globalObject, distance ? environmentAt(*distance) : scope, m_out.constInt32(offset)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, fastResult, slowResult));
    };

    if (type == ResolvedClosureVar) {
        loadClosureVariable(bytecode.m_offset, false);
        return;
    }
    if (type == ResolvedLazyClosureVar) {
        loadClosureVariable(bytecode.m_offset, true);
        return;
    }

    StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_localScopeDepth, type);
    if (variable.kind == StaticVariable::Closure) {
        loadClosureVariable(variable.offset.offset(), variable.inModule);
        return;
    }
    if (variable.kind == StaticVariable::Import) {
        // The scope is the environment of the module that has it (lowerResolveScope()).
        loadClosureVariable(variable.offset.offset(), true);
        return;
    }

    unsigned throwIfNotFound = m_graph.extraOfGetFromScope(bytecode);
    if (usesStubs && variable.isCachedInSlot() && Site::fits(bytecode.m_var, throwIfNotFound)) {
        setJSValue(node, callStub(Stub::GetFromScope, Int64, { { scope, GPRInfo::argumentGPR0 }, { slotAddress(sharedSite(node, bytecode.m_var, throwIfNotFound)), GPRInfo::argumentGPR1 } }, { }));
        return;
    }

    unsigned slot = allocateSlot();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;
    if (variable.isCachedInSlot()) {
        LBasicBlock hasAddress = m_out.newBlock();
        LBasicBlock noAddress = m_out.newBlock();
        LBasicBlock structureHit = m_out.newBlock();

        // Either way what is cached is only good for the kind of scope it was found in: the name may resolve to another.
        LValue word = m_out.load64(slotWord(slot, 0));
        m_out.branch(m_out.equal(m_out.load32(scope, m_heaps.JSCell_structureID), lowHalf(m_out, word)), usually(structureHit), rarely(slowCase));

        m_out.appendTo(structureHit, hasAddress);
        LValue address = m_out.loadPtr(slotWord(slot, 1));
        m_out.branch(m_out.notNull(address), unsure(hasAddress), unsure(noAddress));

        m_out.appendTo(hasAddress, noAddress);
        LValue value = m_out.load64(TypedPointer(m_heaps.root, address));
        results.append(m_out.anchor(value));
        m_out.branch(m_out.notZero64(value), usually(continuation), rarely(slowCase));

        m_out.appendTo(noAddress, slowCase);
        results.append(m_out.anchor(loadProperty(scope, highHalf(m_out, word))));
        m_out.jump(continuation);
    } else
        m_out.jump(slowCase);

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(vmCall(node, Int64, Entry::operationAOTGetFromScope, m_globalObject, scope, m_out.constInt32(bytecode.m_var), slotAddress(slot), m_out.constInt32(throwIfNotFound))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

void Lowering::lowerPutToScope(Node* node)
{
    auto bytecode = node->as<OpPutToScope>();
    auto distance = m_graph.distanceOfEnvironmentAccessed(node);
    LValue scope = distance ? environmentAt(*distance) : lowCell(node->use(bytecode.m_scope));
    Node* valueNode = node->use(bytecode.m_value);
    LValue value = lowJSValue(valueNode);
    ResolveType type = bytecode.m_getPutInfo.resolveType();

    std::optional<unsigned> closureOffset;
    if (type == ResolvedClosureVar)
        closureOffset = bytecode.m_offset;
    else {
        StaticVariable variable = resolveStatically(bytecode.m_var, bytecode.m_symbolTableOrScopeDepth.scopeDepth(), type);
        if (variable.kind == StaticVariable::Closure && (!variable.isReadOnly || isInitialization(bytecode.m_getPutInfo.initializationMode())))
            closureOffset = variable.offset.offset();
    }

    if (closureOffset) {
        // Nobody is watching it: where there is code from the static compiler, the optimizing JITs take no closure variable for a
        // constant (DFG::Graph::tryGetConstantClosureVar()).
        m_out.store64(value, scope, m_heaps.JSLexicalEnvironment_variables[*closureOffset]);
        if (mayBe(valueNode->type, TCell))
            storeBarrier(scope);
        return;
    }

    // What the operation has to know about the instruction, small enough for a Site.
    GetPutInfo info = bytecode.m_getPutInfo;
    static_assert(static_cast<unsigned>(ThrowIfNotFound) <= 1 && static_cast<unsigned>(DoNotThrowIfNotFound) <= 1);
    unsigned how = static_cast<unsigned>(info.resolveMode()) | static_cast<unsigned>(info.initializationMode()) << 1 | info.ecmaMode().isStrict() << 3;
    RELEASE_ASSERT(static_cast<unsigned>(info.initializationMode()) <= 3);

    if (usesStubs && Site::fits(bytecode.m_var, how)) {
        callStub(Stub::PutToScope, Void, { { scope, GPRInfo::argumentGPR0 }, { value, GPRInfo::argumentGPR1 }, { slotAddress(allocateSite(node, bytecode.m_var, how)), GPRInfo::argumentGPR2 } }, { });
        return;
    }
    vmCall(node, Void, Entry::operationAOTPutToScope, m_globalObject, scope, value, m_out.constInt32(bytecode.m_var), slotAddress(allocateSlot()), m_out.constInt32(how));
}

bool Lowering::tryLowerAccess(Node* node)
{
    switch (node->opcode) {
    case op_get_by_id:
        lowerGetById(node);
        return true;
    case op_put_by_id:
        lowerPutById(node);
        return true;
    case op_get_by_val:
        lowerGetByVal(node);
        return true;
    case op_put_by_val:
        lowerPutByVal(node);
        return true;
    case op_resolve_scope:
        lowerResolveScope(node);
        return true;
    case op_get_from_scope:
        lowerGetFromScope(node);
        return true;
    case op_put_to_scope:
        lowerPutToScope(node);
        return true;
    default:
        return false;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
