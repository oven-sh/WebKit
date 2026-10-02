/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(AOT) && CPU(ARM64)

#include "AOTOperationsObjects.h"
#include "B3ValueInlines.h"
#include "BytecodeStructs.h"
#include "DirectArguments.h"
#include "JSCInlines.h"
#include "SymbolTable.h"
#include "UnlinkedCodeBlock.h"

namespace JSC { namespace AOT {

using namespace B3;

bool Lowering::tryLowerObjects(Node* node)
{
    return tryLowerAllocation(node) || tryLowerConversion(node) || tryLowerPropertyVariant(node);
}

LValue Lowering::allocateObjectWithProperties(unsigned slot, const Vector<LValue, 8>& values, LBasicBlock otherwise)
{
    auto orElse = [&](LValue condition) {
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition, usually(next), rarely(otherwise));
        m_out.appendTo(next);
    };
    LValue word = m_out.load64(slotWord(slot, 0));
    LValue structureID = m_out.castToInt32(word);
    orElse(m_out.notZero32(structureID));

    LValue allocator = m_out.loadPtr(slotWord(slot + 1, 1));
    PatchpointValue* allocation = m_out.patchpoint(pointerType());
    allocation->append(ConstrainedValue(allocator, ValueRep::SomeRegister));
    allocation->numGPScratchRegisters = 1;
    allocation->resultConstraints = { ValueRep::SomeEarlyRegister };
    allocation->clobber(RegisterSet::macroClobberedGPRs());
    allocation->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::JumpList outOfSpace;
        jit.emitAllocateWithNonNullAllocator(params[0].gpr(), JITAllocator::variable(), params[1].gpr(), params.gpScratch(0), outOfSpace, CCallHelpers::SlowAllocationResult::ClearToNull);
        outOfSpace.link(&jit);
    });
    LValue object = allocation;
    orElse(m_out.notNull(object));

    m_out.store64(m_out.bitOr(m_out.zeroExt(structureID, Int64), m_out.load64(slotWord(slot + 1, 0))), m_out.address(m_heaps.root, object, 0));
    m_out.storePtr(m_out.intPtrZero, object, m_heaps.JSObject_butterfly);
    for (unsigned i = 0; i < values.size(); ++i)
        m_out.store64(values[i], m_out.address(m_heaps.properties.atAnyNumber(), object, JSObject::offsetOfInlineStorage() + i * sizeof(EncodedJSValue)));

    LBasicBlock clear = m_out.newBlock();
    LBasicBlock clearLoop = m_out.newBlock();
    LBasicBlock cleared = m_out.newBlock();
    ValueFromBlock capacity = m_out.anchor(m_out.bitAnd(m_out.lShr(word, m_out.constInt32(32)), m_out.constInt64(Slot::offsetMask)));
    m_out.jump(clear);
    m_out.appendTo(clear, clearLoop);
    LValue remaining = m_out.phi(Int64, capacity);
    m_out.branch(m_out.above(remaining, m_out.constInt64(values.size())), rarely(clearLoop), usually(cleared));
    m_out.appendTo(clearLoop, cleared);
    LValue index = m_out.sub(remaining, m_out.constInt64(1));
    m_out.store64(m_out.int64Zero, TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(object, m_out.add(m_out.shl(index, m_out.constInt32(3)), m_out.constIntPtr(JSObject::offsetOfInlineStorage())))));
    m_out.addIncomingToPhi(remaining, m_out.anchor(index));
    m_out.jump(clear);
    m_out.appendTo(cleared);

    LBasicBlock fence = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    m_out.branch(m_out.load8ZeroExt32(m_vm, m_heaps.VM_heap_mutatorShouldBeFenced), rarely(fence), usually(done));
    m_out.appendTo(fence, done);
    m_out.fence(&m_heaps.root, nullptr);
    m_out.jump(done);
    m_out.appendTo(done);
    return object;
}

void Lowering::validateNewObject(Node* node, LValue object, uint32_t layout, const Vector<Node*, 8>& inSlots, const Vector<LValue, 8>& values, const Vector<TypeTable::FieldType, 8>* fieldTypesIfKnown)
{
    if (!Options::useAOTTypedFields() || !layout || !TypeTable::shared())
        return;
    auto fieldTypes = fieldTypesIfKnown ? *fieldTypesIfKnown : TypeTable::hasTypedFields() ? TypeTable::shared()->layoutFieldTypesBySlot(layout) : TypeTable::shared()->fieldTypesBySlot(layout);
    LBasicBlock mismatchCase = nullptr;
    for (unsigned slot = 0; slot < inSlots.size() && slot < fieldTypes.size(); ++slot) {
        if (!inSlots[slot] || !fieldTypes[slot].isConstrained())
            continue;
        if (!mismatchCase)
            mismatchCase = newColdBlock();
        branchUnlessAccepted(inSlots[slot], values[slot], fieldTypes[slot], mismatchCase);
    }
    if (!mismatchCase)
        return;
    LBasicBlock settled = m_out.newBlock();
    m_out.jump(settled);
    m_out.appendTo(mismatchCase);
    if (TypeTable::hasTypedFields())
        vmCall(node, Void, Entry::operationAOTValidateTypedObject, m_instance, object);
    else
        plainCall(Void, Entry::operationAOTValidateNewObject, m_instance, object);
    m_out.jump(settled);
    m_out.appendTo(settled);
}

bool Lowering::tryLowerAllocation(Node* node)
{

    auto newFunction = [&](VirtualRegister scope, unsigned index, bool isExpression, FunctionKind kind) {
        m_graph.functionsCreated.append(isExpression ? code().codeBlock()->functionExpr(index) : code().codeBlock()->functionDecl(index));
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTNewFunction, m_instance, lowCell(node->use(scope)),
            m_out.constInt32(index), m_out.constInt32(isExpression | bytecodeOwner(node) << 1), m_out.constInt32(static_cast<uint32_t>(kind)), slotAddress(allocateSlots(2))));
        return true;
    };
    auto newInternalFieldObject = [&](InternalFieldObjectKind kind) {
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTNewInternalFieldObject, m_instance, m_out.constInt32(static_cast<uint32_t>(kind))));
        return true;
    };
    auto createInternalFieldObject = [&](VirtualRegister callee, InternalFieldObjectKind kind) {
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTCreateInternalFieldObject, m_instance, lowCell(node->use(callee)), m_out.constInt32(static_cast<uint32_t>(kind))));
        return true;
    };

    switch (node->opcode) {
    case op_new_object:
        if (unsigned count = node->numberOfLiteralProperties) {
            Vector<LValue, 8> values;
            Vector<Node*, 8> layoutSlotNodes;
            uint32_t layout = 0;
            for (unsigned i = 0; i < count; ++i) {
                values.append(lowJSValue(node->use(NewObjectPlan::registerOf(i))));
                layoutSlotNodes.append(node->use(NewObjectPlan::registerOf(i)));
            }
            auto thisShape = m_graph.literalShape(node);
            if (uint16_t layoutID = Graph::newObjectLayoutID(node); layoutID && (!thisShape || !thisShape->layoutID)) {
                LValue object = vmCall(node, pointerType(), Entry::operationAOTNewTypedObject, m_instance, m_out.constInt32(layoutID), slotAddress(allocateSlots(2)));
                auto& instructions = code().codeBlock()->instructions();
                auto& stores = code().literalStores(node->bytecodeIndex.offset());
                RELEASE_ASSERT(stores.size() >= count);
                for (unsigned i = 0; i < count; ++i) {
                    auto store = instructions.at(stores[i])->as<OpPutById>();
                    uint32_t flags = (store.m_flags.isDirect() ? 1 : 0) | (store.m_flags.ecmaMode().isStrict() ? 2 : 0);
                    vmCall(node, Void, Entry::operationAOTPutById, m_instance, object, values[i], m_out.constInt32(numberOf(store.m_property)), slotAddress(allocateSlot()), m_out.constInt32(flags));
                }
                setJSValue(node, object);
                return true;
            }
            unsigned slot = allocateSlots(2);
            Vector<TypeTable::FieldType, 8> fieldTypesBySlot;
            bool fieldTypesAreKnown = false;
            bool hasSlotsOutside = thisShape && thisShape->hasSlotsOutside();
            if (auto shape = WTF::move(thisShape)) {
                if (!shape->slots.isEmpty()) {
                    Vector<LValue, 8> inSlots;
                    Vector<Node*, 8> nodesInSlots;
                    inSlots.fill(m_out.int64Zero, shape->numberOfSlots());
                    nodesInSlots.fill(nullptr, shape->numberOfSlots());
                    auto fieldTypes = shape->layoutID ? TypeTable::shared()->layoutFieldTypesBySlot(shape->layoutID, shape->names.span(), shape->slots.span()) : Vector<TypeTable::FieldType, 8> { };
                    fieldTypesBySlot = fieldTypes;
                    fieldTypesAreKnown = shape->layoutID;
                    for (unsigned i = 0; i < count; ++i) {
                        inSlots[shape->slots[i]] = shape->layoutID && Options::useAOTTypedFields() && shape->slots[i] < fieldTypes.size() ? toFieldRepresentation(layoutSlotNodes[i], values[i], fieldTypes[shape->slots[i]]) : values[i];
                        nodesInSlots[shape->slots[i]] = layoutSlotNodes[i];
                    }
                    values = WTF::move(inSlots);
                    layoutSlotNodes = WTF::move(nodesInSlots);
                }
                layout = TypeTable::hasTypedFields() ? shape->layoutID : shape->number;
                m_graph.noteSiteShape(slot, WTF::move(*shape));
            }
            {
                auto& instructions = code().codeBlock()->instructions();
                auto& stores = code().literalStores(node->bytecodeIndex.offset());
                RELEASE_ASSERT(stores.size() >= count);
                Vector<uint32_t, 16> words { AllocationPlan::encode(node->as<OpNewObject>().m_inlineCapacity, count) };
                for (unsigned i = 0; i < count; ++i)
                    words.append(AllocationPlan::encode(numberOf(instructions.at(stores[i])->as<OpPutById>().m_property), true, true));
                m_graph.noteSitePlan(slot, WTF::move(words));
            }
            LBasicBlock slowCase = m_out.newBlock();
            LBasicBlock continuation = m_out.newBlock();
            Vector<ValueFromBlock, 2> results;
            if (!isCompact() && values.size() <= JSFinalObject::maxInlineCapacity && !hasSlotsOutside) {
                results.append(m_out.anchor(allocateObjectWithProperties(slot, values, slowCase)));
                m_out.jump(continuation);
            } else
                m_out.jump(slowCase);
            m_out.appendTo(slowCase, continuation);
            for (unsigned i = 0; i < values.size(); ++i)
                m_out.store64(values[i], scratchWord(i));
            results.append(m_out.anchor(vmCall(node, pointerType(), Entry::operationAOTNewObjectLiteral, m_instance, scratchAddress(), m_out.constInt32(values.size()), slotAddress(slot))));
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            LValue object = m_out.phi(pointerType(), results);
            validateNewObject(node, object, layout, layoutSlotNodes, values, fieldTypesAreKnown ? &fieldTypesBySlot : nullptr);
            setJSValue(node, object);
            return true;
        }
        if (uint16_t layoutID = Graph::newObjectLayoutID(node)) {
            setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTNewTypedObject, m_instance, m_out.constInt32(layoutID), slotAddress(allocateSlots(2))));
            return true;
        }
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTNewObject, m_instance, m_out.constInt32(node->as<OpNewObject>().m_inlineCapacity), slotAddress(allocateSlots(2))));
        return true;
    case op_create_this: {
        auto bytecode = node->as<OpCreateThis>();
        if (unsigned count = node->numberOfLiteralProperties) {
            Vector<LValue, 8> values;
            Vector<Node*, 8> layoutSlotNodes;
            uint32_t layout = 0;
            for (unsigned i = 0; i < count; ++i) {
                values.append(lowJSValue(node->use(NewObjectPlan::registerOf(i))));
                layoutSlotNodes.append(node->use(NewObjectPlan::registerOf(i)));
            }
            LValue callee = lowCell(node->use(bytecode.m_callee));
            unsigned slot = allocateSlots(3);
            {
                NewObjectPlan plan = NewObjectPlan::forCreateThis(code().codeBlock()->instructions(), node->bytecodeIndex.offset());
                RELEASE_ASSERT(plan.properties.size() == count);
                KnownShape shape;
                for (auto& property : plan.properties)
                    shape.names.append(code().codeBlock()->identifier(property.identifier).impl());
                if (uint32_t tag = node->graph->typeTagAt(plan.stores[0].offset); tag && (Options::aotShapeOptimizations() & 1) && TypeTable::shared()) {
                    if (auto layout = TypeTable::shared()->layoutOf(tag); layout && layout->properties.size() == count) {
                        bool matchesSourceOrder = true;
                        for (unsigned i = 0; i < count; ++i)
                            matchesSourceOrder &= layout->properties[i].first == shape.names[i] && layout->properties[i].second == i;
                        if (matchesSourceOrder)
                            shape.number = layout->number;
                    }
                }
                layout = shape.number;
                if (std::ranges::none_of(shape.names, [](UniquedStringImpl* name) { return name->isSymbol(); }))
                    m_graph.noteSiteShape(slot, WTF::move(shape));
                Vector<uint32_t, 16> words { AllocationPlan::encode(bytecode.m_inlineCapacity, count) };
                for (auto& property : plan.properties)
                    words.append(AllocationPlan::encode(numberOf(property.identifier), property.isDefined, property.isStrict));
                m_graph.noteSitePlan(slot, WTF::move(words));
            }
            LBasicBlock slowCase = m_out.newBlock();
            LBasicBlock continuation = m_out.newBlock();
            auto orElse = [&](LValue condition) {
                LBasicBlock next = m_out.newBlock();
                m_out.branch(condition, usually(next), rarely(slowCase));
                m_out.appendTo(next);
            };
            orElse(m_out.equal(m_out.loadPtr(slotWord(slot, 1)), callee));
            LValue rareData = m_out.loadPtr(callee, m_heaps.JSFunction_executableOrRareData);
            orElse(m_out.testNonZeroPtr(rareData, m_out.constIntPtr(JSFunction::rareDataTag)));
            LValue structure = m_out.loadPtr(m_out.address(m_heaps.FunctionRareData_structure, rareData, FunctionRareData::offsetOfObjectAllocationProfile() + ObjectAllocationProfileWithPrototype::offsetOfStructure() - JSFunction::rareDataTag));
            orElse(m_out.notNull(structure));
            orElse(m_out.equal(m_out.castToInt32(structure), m_out.castToInt32(m_out.load64(slotWord(slot + 2, 0)))));
            ValueFromBlock fastResult = m_out.anchor(allocateObjectWithProperties(slot, values, slowCase));
            m_out.jump(continuation);

            m_out.appendTo(slowCase, continuation);
            for (unsigned i = 0; i < count; ++i)
                m_out.store64(values[i], scratchWord(i));
            ValueFromBlock slowResult = m_out.anchor(vmCall(node, pointerType(), Entry::operationAOTCreateThisWithProperties, m_instance, callee, scratchAddress(), m_out.constInt32(count), slotAddress(slot)));
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            LValue object = m_out.phi(pointerType(), fastResult, slowResult);
            validateNewObject(node, object, layout, layoutSlotNodes, values);
            setJSValue(node, object);
            return true;
        }
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTCreateThis, m_instance, lowCell(node->use(bytecode.m_callee)), m_out.constInt32(bytecode.m_inlineCapacity)));
        return true;
    }
    case op_new_array: {
        auto bytecode = node->as<OpNewArray>();
        bool areInt32 = !!bytecode.m_argc;
        bool operandsAreInFrame = Graph::readsOperandsFromFrame(node);
        for (unsigned i = 0; i < bytecode.m_argc; ++i) {
            VirtualRegister reg(bytecode.m_argv.offset() - static_cast<int>(i));
            Type type = operandsAreInFrame ? m_graph.frameRegisterTypes[m_graph.registerIndex(reg)] : node->use(reg)->type;
            areInt32 &= type && isSubtype(type, TInt32);
        }
        LValue values = operandsAreInFrame ? addressFor(bytecode.m_argv).value() : bytecode.m_argc ? storeToScratch(node, bytecode.m_argv, bytecode.m_argc) : m_out.intPtrZero;
        setJSValue(node, withHelper(areInt32 ? Stub::HelperNewInt32Array : Stub::HelperNewArray, { values, m_out.constInt32(bytecode.m_argc) }, [&] {
            return vmCall(node, pointerType(), Entry::operationAOTNewArray, m_instance, values, m_out.constInt32(bytecode.m_argc), m_out.constInt32(areInt32 ? ArrayWithInt32 : ArrayWithContiguous));
        }));
        return true;
    }
    case op_new_array_with_size:
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTNewArrayWithSize, m_instance, lowJSValue(node->use(node->as<OpNewArrayWithSize>().m_length))));
        return true;
    case op_new_array_buffer: {
        LValue butterfly = lowCell(node->use(node->as<OpNewArrayBuffer>().m_immutableButterfly));
        setJSValue(node, withHelper(Stub::HelperNewArrayBuffer, { butterfly }, [&] {
            return vmCall(node, pointerType(), Entry::operationAOTNewArrayBuffer, m_instance, butterfly);
        }));
        return true;
    }
    case op_new_array_with_spread: {
        auto bytecode = node->as<OpNewArrayWithSpread>();
        uint32_t pendingSpreads = 0;
        bool hasSpreadElements = false;
        const BitVector& spreadMask = code().codeBlock()->bitVector(bytecode.m_bitVector);
        Vector<Node*, 8> elements;
        for (unsigned i = 0; i < bytecode.m_argc; ++i) {
            Node* element = node->use(VirtualRegister(bytecode.m_argv.offset() - static_cast<int>(i)));
            if (element->isBytecode(op_spread) && element->isElided) {
                RELEASE_ASSERT(i < 32);
                pendingSpreads |= 1u << i;
                element = element->use(element->as<OpSpread>().m_argument);
            } else
                hasSpreadElements |= spreadMask.get(i);
            elements.append(element);
        }
        for (unsigned i = 0; i < elements.size(); ++i)
            m_out.store64(lowJSValue(elements[i]), scratchWord(i));
        LValue values = m_scratch;
        auto slowCase = [&] {
            return vmCall(node, pointerType(), Entry::operationAOTNewArrayWithSpread, m_instance, values, m_out.constInt32(bytecode.m_argc), m_out.constInt32(pendingSpreads));
        };
        if (!hasSpreadElements)
            setJSValue(node, withHelper(Stub::HelperNewArrayWithSpread, { values, m_out.constInt32(bytecode.m_argc), m_out.constInt32(pendingSpreads) }, slowCase));
        else
            setJSValue(node, slowCase());
        return true;
    }
    case op_new_array_with_species: {
        auto bytecode = node->as<OpNewArrayWithSpecies>();
        LValue length = lowJSValue(node->use(bytecode.m_length));
        LValue array = lowCell(node->use(bytecode.m_array));
        auto slowCase = [&] { return vmCall(node, pointerType(), Entry::operationAOTNewArrayWithSpecies, m_instance, length, array); };
        setJSValue(node, withHelper(Stub::HelperNewArrayWithSpecies, { length, array }, slowCase));
        return true;
    }
    case op_spread:
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTSpread, m_instance, lowJSValue(node->use(node->as<OpSpread>().m_argument))));
        return true;
    case op_new_reg_exp:
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTNewRegExp, m_instance, lowConstantRegister(node->as<OpNewRegExp>().m_regexp)));
        return true;
    case op_new_reg_exp_shared: {
        auto bytecode = node->as<OpNewRegExpShared>();
        if (!Options::useSharedRegExpLiteralObjects()) {
            setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTNewRegExp, m_instance, lowConstantRegister(bytecode.m_regexp)));
            return true;
        }
        unsigned slot = allocateSlot();
        LBasicBlock make = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue cached = m_out.load64(slotWord(slot, 1));
        ValueFromBlock found = m_out.anchor(cached);
        m_out.branch(m_out.notZero64(cached), usually(continuation), rarely(make));
        m_out.appendTo(make);
        ValueFromBlock made = m_out.anchor(vmCall(node, pointerType(), Entry::operationAOTNewRegExpForReceiver, m_instance, lowConstantRegister(bytecode.m_regexp), m_out.constInt32(bytecode.m_forTest), slotAddress(slot)));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, found, made));
        return true;
    }

#define AOT_NEW_FUNCTION(Struct, opcodeName, isExpression, kind) \
    case opcodeName: { \
        auto bytecode = node->as<Struct>(); \
        return newFunction(bytecode.m_scope, bytecode.m_functionDecl, isExpression, FunctionKind::kind); \
    }
    AOT_NEW_FUNCTION(OpNewFunc, op_new_func, false, Normal)
    AOT_NEW_FUNCTION(OpNewFuncExp, op_new_func_exp, true, Normal)
    AOT_NEW_FUNCTION(OpNewGeneratorFunc, op_new_generator_func, false, Generator)
    AOT_NEW_FUNCTION(OpNewGeneratorFuncExp, op_new_generator_func_exp, true, Generator)
    AOT_NEW_FUNCTION(OpNewAsyncFunc, op_new_async_func, false, Async)
    AOT_NEW_FUNCTION(OpNewAsyncFuncExp, op_new_async_func_exp, true, Async)
    AOT_NEW_FUNCTION(OpNewAsyncGeneratorFunc, op_new_async_generator_func, false, AsyncGenerator)
    AOT_NEW_FUNCTION(OpNewAsyncGeneratorFuncExp, op_new_async_generator_func_exp, true, AsyncGenerator)
#undef AOT_NEW_FUNCTION

    case op_set_function_name: {
        auto bytecode = node->as<OpSetFunctionName>();
        vmCall(node, Void, Entry::operationAOTSetFunctionName, m_instance, lowCell(node->use(bytecode.m_function)), lowJSValue(node->use(bytecode.m_name)));
        return true;
    }
    case op_new_promise:
        return newInternalFieldObject(InternalFieldObjectKind::Promise);
    case op_new_generator:
        return newInternalFieldObject(InternalFieldObjectKind::Generator);
    case op_new_async_function_generator:
        return newInternalFieldObject(InternalFieldObjectKind::AsyncFunctionGenerator);
    case op_create_promise:
        return createInternalFieldObject(node->as<OpCreatePromise>().m_callee, InternalFieldObjectKind::Promise);
    case op_create_generator:
        return createInternalFieldObject(node->as<OpCreateGenerator>().m_callee, InternalFieldObjectKind::Generator);
    case op_create_async_generator:
        return createInternalFieldObject(node->as<OpCreateAsyncGenerator>().m_callee, InternalFieldObjectKind::AsyncGenerator);
    case op_create_lexical_environment: {
        auto bytecode = node->as<OpCreateLexicalEnvironment>();
        if (node->isPromoted) {
            LValue initialValue = lowJSValue(node->use(bytecode.m_initialValue));
            JSValue table = code().codeBlock()->getConstant(bytecode.m_symbolTable);
            for (unsigned i = uncheckedDowncast<SymbolTable>(table.asCell())->scopeSize(); i--;)
                m_out.m_block->appendNew<B3::VariableValue>(m_proc, B3::Set, m_out.origin(), environmentVariable(node, i), initialValue);
            return true;
        }
        LValue scope = lowCell(node->use(bytecode.m_scope));
        LValue symbolTable = lowCell(node->use(bytecode.m_symbolTable));
        LValue initialValue = lowJSValue(node->use(bytecode.m_initialValue));
        unsigned scopeSize = uncheckedDowncast<SymbolTable>(code().codeBlock()->getConstant(bytecode.m_symbolTable).asCell())->scopeSize();
        setJSValue(node, withHelper(Stub::HelperNewActivation, { scope, symbolTable, initialValue, m_out.constInt32(scopeSize) }, [&] {
            return vmCall(node, pointerType(), Entry::operationAOTCreateLexicalEnvironment, m_instance, scope, symbolTable, initialValue, m_out.constInt32(scopeSize));
        }));
        return true;
    }
    case op_push_with_scope: {
        auto bytecode = node->as<OpPushWithScope>();
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTPushWithScope, m_instance, lowCell(node->use(bytecode.m_currentScope)), lowJSValue(node->use(bytecode.m_newScope))));
        return true;
    }
    case op_resolve_scope_for_hoisting_func_decl_in_eval: {
        auto bytecode = node->as<OpResolveScopeForHoistingFuncDeclInEval>();
        setJSValue(node, vmCall(node, Int64, Entry::operationAOTResolveScopeForHoistingFuncDeclInEval, m_instance, lowCell(node->use(bytecode.m_scope)), m_out.constInt32(numberOf(bytecode.m_property))));
        return true;
    }
    case op_create_direct_arguments:
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTCreateDirectArguments, m_instance, callee(), numberOfArgumentsPassed(), argumentsPassed(), m_out.constInt32(code().codeBlock()->numParameters() - 1)));
        return true;
    case op_create_scoped_arguments:
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTCreateScopedArguments, m_instance, lowCell(node->use(node->as<OpCreateScopedArguments>().m_scope)), callee(), numberOfArgumentsPassed(), argumentsPassed()));
        return true;
    case op_create_cloned_arguments:
        setJSValue(node, vmCall(node, pointerType(), Entry::operationAOTCreateClonedArguments, m_instance, callee(), numberOfArgumentsPassed(), argumentsPassed()));
        return true;
    case op_create_rest: {
        unsigned skipped = node->as<OpCreateRest>().m_numParametersToSkip;
        LValue passed = numberOfArgumentsPassed();
        LValue count = m_out.select(m_out.above(passed, m_out.constInt32(skipped)), m_out.sub(passed, m_out.constInt32(skipped)), m_out.int32Zero);
        setJSValue(node, withHelper(Stub::HelperNewArray, { m_out.add(argumentsPassed(), m_out.constIntPtr(skipped * sizeof(EncodedJSValue))), count }, [&] {
            return vmCall(node, pointerType(), Entry::operationAOTCreateRest, m_instance, passed, argumentsPassed(), m_out.constInt32(skipped));
        }));
        return true;
    }
    default:
        return false;
    }
}

void Lowering::throwTDZError(Node* node)
{
    bool isThis = node->as<OpCheckTdz>().m_targetVirtualRegister == code().codeBlock()->thisRegister();
    coldCall(node, isThis ? Entry::operationAOTThrowThisTDZError : Entry::operationAOTThrowTDZError);
    m_out.unreachable();
}

void Lowering::throwStaticError(Node* node)
{
    auto bytecode = node->as<OpThrowStaticError>();
    vmCall(node, Void, Entry::operationAOTThrowStaticError, m_instance, lowJSValue(node->use(bytecode.m_message)), m_out.constInt32(static_cast<uint32_t>(bytecode.m_errorType)));
    m_out.unreachable();
}

void Lowering::lowerToThis(Node* node)
{
    auto bytecode = node->as<OpToThis>();
    Node* valueNode = node->use(bytecode.m_srcDst);
    bool isStrict = bytecode.m_ecmaMode.isStrict();
    if (isStrict ? !mayBe(valueNode->type, TOtherObject) : isSubtype(valueNode->type, TAnyObject & ~TOtherObject)) {
        setResult(node, lowRaw(valueNode), valueNode->rep());
        return;
    }

    LValue value = lowJSValue(valueNode);
    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock objectCase = m_out.newBlock();
    LBasicBlock slowCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    LBasicBlock notObjectCase = isStrict ? continuation : slowCase;
    Vector<ValueFromBlock, 4> results;

    if (isStrict)
        results.append(m_out.anchor(value));
    m_out.branch(isCell(value), usually(cellCase), rarely(notObjectCase));

    m_out.appendTo(cellCase, objectCase);
    LValue type = cellType(value);
    if (isStrict)
        results.append(m_out.anchor(value));
    m_out.branch(m_out.aboveOrEqual(type, m_out.constInt32(ObjectType)), usually(objectCase), rarely(notObjectCase));

    m_out.appendTo(objectCase, slowCase);
    results.append(m_out.anchor(value));
    LValue isScope = m_out.belowOrEqual(m_out.sub(type, m_out.constInt32(FirstScopeType)), m_out.constInt32(LastScopeType - FirstScopeType));
    m_out.branch(isScope, rarely(slowCase), usually(continuation));

    m_out.appendTo(slowCase, continuation);
    results.append(m_out.anchor(vmCall(node, Int64, Entry::operationAOTToThis, m_instance, value, m_out.constInt32(isStrict))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

bool Lowering::tryLowerConversion(Node* node)
{
    auto identityOrSlow = [&](Node* valueNode, Type identity, auto&& isIdentity, auto&& slow) {
        if (isSubtype(valueNode->type, identity)) {
            setResult(node, lowRaw(valueNode), valueNode->rep());
            return true;
        }
        LValue value = lowJSValue(valueNode);
        if (!mayBe(valueNode->type, identity)) {
            setJSValue(node, slow(value));
            return true;
        }
        LBasicBlock slowCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock fastResult = m_out.anchor(value);
        m_out.branch(isIdentity(value), usually(continuation), rarely(slowCase));
        m_out.appendTo(slowCase, continuation);
        ValueFromBlock slowResult = m_out.anchor(slow(value));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, fastResult, slowResult));
        return true;
    };
    auto isStringOrSymbol = [&](Node* valueNode, LValue value) {
        return isCellAnd(valueNode, value, [&](LValue cell) {
            LValue type = cellType(cell);
            return m_out.bitOr(m_out.equal(type, m_out.constInt32(StringType)), m_out.equal(type, m_out.constInt32(SymbolType)));
        });
    };
    auto test = [&](Node* valueNode, Type yes, Type no, auto&& slow) {
        if (isSubtype(valueNode->type, yes))
            setBoolean(node, m_out.booleanTrue);
        else if (isSubtype(valueNode->type, no))
            setBoolean(node, m_out.booleanFalse);
        else
            setBoolean(node, m_out.notZero64(slow(lowJSValue(valueNode))));
        return true;
    };

    switch (node->opcode) {
    case op_to_this:
        lowerToThis(node);
        return true;
    case op_to_object: {
        auto bytecode = node->as<OpToObject>();
        Node* valueNode = node->use(bytecode.m_operand);
        return identityOrSlow(valueNode, TAnyObject, [&](LValue value) {
            return isCellAnd(valueNode, value, [&](LValue cell) { return isObjectCell(cell); });
        }, [&](LValue value) {
            return vmCall(node, pointerType(), Entry::operationAOTToObject, m_instance, value, m_out.constInt32(numberOf(bytecode.m_message)));
        });
    }
    case op_to_primitive: {
        Node* valueNode = node->use(node->as<OpToPrimitive>().m_src);
        return identityOrSlow(valueNode, TPrimitive, [&](LValue value) {
            return m_out.logicalNot(isCellAnd(valueNode, value, [&](LValue cell) { return isObjectCell(cell); }));
        }, [&](LValue value) {
            return vmCall(node, Int64, Entry::operationAOTToPrimitive, m_instance, value);
        });
    }
    case op_to_property_key: {
        Node* valueNode = node->use(node->as<OpToPropertyKey>().m_src);
        return identityOrSlow(valueNode, TString | TSymbol, [&](LValue value) {
            return isStringOrSymbol(valueNode, value);
        }, [&](LValue value) {
            return vmCall(node, Int64, Entry::operationAOTToPropertyKey, m_instance, value);
        });
    }
    case op_to_property_key_or_number: {
        Node* valueNode = node->use(node->as<OpToPropertyKeyOrNumber>().m_src);
        return identityOrSlow(valueNode, TString | TSymbol | TNumber, [&](LValue value) {
            return m_out.bitOr(isNumber(value), isStringOrSymbol(valueNode, value));
        }, [&](LValue value) {
            return vmCall(node, Int64, Entry::operationAOTToPropertyKey, m_instance, value);
        });
    }
    case op_typeof:
        setJSValue(node, plainCall(pointerType(), Entry::operationAOTTypeof, m_instance, lowJSValue(node->use(node->as<OpTypeof>().m_value))));
        return true;
    case op_typeof_is_object:
    case op_typeof_is_function: {
        bool wantsObject = node->opcode == op_typeof_is_object;
        Node* valueNode = node->use(wantsObject ? node->as<OpTypeofIsObject>().m_operand : node->as<OpTypeofIsFunction>().m_operand);
        Type yes = wantsObject ? TNull | TArray : TFunction;
        Type no = wantsObject ? (TPrimitive & ~TNull) | TFunction : TPrimitive | TArray;
        if (isSubtype(valueNode->type, yes)) {
            setBoolean(node, m_out.booleanTrue);
            return true;
        }
        if (isSubtype(valueNode->type, no)) {
            setBoolean(node, m_out.booleanFalse);
            return true;
        }
        LValue value = lowJSValue(valueNode);
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock notFunctionCase = m_out.newBlock();
        LBasicBlock slowPath = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        Vector<ValueFromBlock, 5> results;
        results.append(m_out.anchor(wantsObject ? m_out.equal(value, m_out.constInt64(JSValue::ValueNull)) : m_out.booleanFalse));
        m_out.branch(isCell(value), unsure(cellCase), unsure(continuation));

        m_out.appendTo(cellCase);
        LValue type = cellType(value);
        if (wantsObject) {
            LBasicBlock objectCase = m_out.newBlock();
            results.append(m_out.anchor(m_out.booleanFalse));
            m_out.branch(m_out.aboveOrEqual(type, m_out.constInt32(ObjectType)), unsure(objectCase), unsure(continuation));
            m_out.appendTo(objectCase);
        }
        results.append(m_out.anchor(wantsObject ? m_out.booleanFalse : m_out.booleanTrue));
        m_out.branch(m_out.equal(type, m_out.constInt32(JSFunctionType)), unsure(continuation), unsure(notFunctionCase));

        m_out.appendTo(notFunctionCase);
        results.append(m_out.anchor(wantsObject ? m_out.booleanTrue : m_out.booleanFalse));
        m_out.branch(m_out.testNonZero32(m_out.load8ZeroExt32(value, m_heaps.JSCell_typeInfoFlags), m_out.constInt32(MasqueradesAsUndefined | OverridesGetCallData)), rarely(slowPath), usually(continuation));

        m_out.appendTo(slowPath);
        results.append(m_out.anchor(m_out.notZero64(plainCall(Int64, wantsObject ? Entry::operationAOTTypeofIsObject : Entry::operationAOTTypeofIsFunction, m_instance, value))));
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        setBoolean(node, m_out.phi(Int32, results));
        return true;
    }
    case op_is_callable:
        return test(node->use(node->as<OpIsCallable>().m_operand), TFunction, TPrimitive | TArray, [&](LValue value) {
            return plainCall(Int64, Entry::operationAOTIsCallable, value);
        });
    case op_is_constructor:
        return test(node->use(node->as<OpIsConstructor>().m_operand), TNone, TPrimitive | TArray, [&](LValue value) {
            return plainCall(Int64, Entry::operationAOTIsConstructor, value);
        });
    case op_strcat: {
        auto bytecode = node->as<OpStrcat>();
        bool areStrings = bytecode.m_count >= 2 && bytecode.m_count <= 5;
        for (unsigned i = 0; i < bytecode.m_count; ++i)
            areStrings &= isSubtype(node->use(VirtualRegister(bytecode.m_src.offset() - static_cast<int>(i)))->type, TString);
        if (areStrings) {
            auto at = [&](unsigned i) { return lowCell(node->use(VirtualRegister(bytecode.m_src.offset() - static_cast<int>(i)))); };
            LValue soFar = at(0);
            for (unsigned i = 1; i < bytecode.m_count;) {
                LValue first = soFar;
                LValue second = at(i++);
                if (i < bytecode.m_count) {
                    LValue third = at(i++);
                    soFar = withHelper(Stub::HelperMakeRope3, { first, second, third }, [&] { return vmCall(node, pointerType(), Entry::operationMakeRope3, m_globalObject, first, second, third); });
                } else
                    soFar = withHelper(Stub::HelperMakeRope2, { first, second }, [&] { return vmCall(node, pointerType(), Entry::operationMakeRope2, m_globalObject, first, second); });
            }
            setJSValue(node, soFar);
            return true;
        }
        LValue values = storeToScratch(node, bytecode.m_src, bytecode.m_count);
        setJSValue(node, vmCall(node, Int64, Entry::operationAOTStrcat, m_instance, values, m_out.constInt32(bytecode.m_count)));
        return true;
    }
    case op_get_prototype_of:
        setJSValue(node, vmCall(node, Int64, Entry::operationAOTGetPrototypeOf, m_instance, lowJSValue(node->use(node->as<OpGetPrototypeOf>().m_value))));
        return true;
    case op_instanceof: {
        auto bytecode = node->as<OpInstanceof>();
        Node* valueNode = node->use(bytecode.m_value);
        Node* constructorNode = node->use(bytecode.m_constructor);
        LValue value = lowJSValue(valueNode);
        LValue constructor = lowJSValue(constructorNode);
        LBasicBlock constructorIsObject = m_out.newBlock();
        LBasicBlock constructorIsNotObject = newColdBlock();
        LBasicBlock isCustom = newColdBlock();
        LBasicBlock isDefault = m_out.newBlock();
        LBasicBlock valueIsObject = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        Vector<ValueFromBlock, 4> results;

        m_out.branch(isCellAnd(constructorNode, constructor, [&](LValue cell) { return isObjectCell(cell); }), usually(constructorIsObject), rarely(constructorIsNotObject));

        m_out.appendTo(constructorIsNotObject);
        results.append(m_out.anchor(vmCall(node, Int64, Entry::operationAOTInstanceof, m_instance, value, constructor)));
        m_out.jump(continuation);

        m_out.appendTo(constructorIsObject);
        LValue hasInstance = getByIdCached(node, constructor, TAnyObject, Entry::operationAOTGetByIdWellKnown, static_cast<unsigned>(WellKnownIdentifier::HasInstanceSymbol));
        LValue defaultHasInstance = m_out.load64(m_out.address(m_heaps.root, m_globalObject, JSGlobalObject::offsetOfFunctionProtoHasInstanceSymbolFunction()));
        LValue implementsDefault = m_out.testNonZero32(m_out.load8ZeroExt32(constructor, m_heaps.JSCell_typeInfoFlags), m_out.constInt32(ImplementsDefaultHasInstance));
        m_out.branch(m_out.bitAnd(m_out.equal(hasInstance, defaultHasInstance), implementsDefault), usually(isDefault), rarely(isCustom));

        m_out.appendTo(isCustom);
        results.append(m_out.anchor(vmCall(node, Int64, Entry::operationAOTInstanceofCustom, m_instance, value, constructor, hasInstance)));
        m_out.jump(continuation);

        m_out.appendTo(isDefault);
        results.append(m_out.anchor(m_out.int64Zero));
        m_out.branch(isCellAnd(valueNode, value, [&](LValue cell) { return isObjectCell(cell); }), unsure(valueIsObject), unsure(continuation));

        m_out.appendTo(valueIsObject);
        LValue prototype = getByIdCached(node, constructor, TAnyObject, Entry::operationAOTGetByIdWellKnown, static_cast<unsigned>(WellKnownIdentifier::Prototype));
        results.append(m_out.anchor(callStub(Stub::InstanceOf, Int64, { { value, GPRInfo::argumentGPR0 }, { prototype, GPRInfo::argumentGPR1 } }, { })));
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        setProj(node, bytecode.m_dst, m_out.notZero64(m_out.phi(Int64, results)), Rep::Boolean);
        setProj(node, bytecode.m_hasInstanceOrPrototype, m_out.constInt64(JSValue::encode(jsUndefined())));
        return true;
    }
    case op_has_structure_with_flags: {
        auto bytecode = node->as<OpHasStructureWithFlags>();
        LValue structure = structureOf(lowCell(node->use(bytecode.m_operand)));
        setBoolean(node, m_out.testNonZero32(m_out.load32(structure, m_heaps.Structure_bitField), m_out.constInt32(bytecode.m_flags)));
        return true;
    }
    default:
        return false;
    }
}

void Lowering::lowerGetLength(Node* node)
{
    Node* baseNode = node->use(node->as<OpGetLength>().m_base);
    LValue base = lowJSValue(baseNode);

    if (isSubtype(baseNode->type, TArray) && baseNode->type) {
        if (auto* view = viewOf(node, baseNode)) {
            setInt64(node, view->length);
            return;
        }
        LBasicBlock hasStorage = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
        ValueFromBlock none = m_out.anchor(m_out.int64Zero);
        m_out.branch(m_out.notNull(butterfly), usually(hasStorage), rarely(continuation));
        m_out.appendTo(hasStorage);
        ValueFromBlock some = m_out.anchor(m_out.zeroExt(m_out.load32(butterfly, m_heaps.Butterfly_publicLength), Int64));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setInt64(node, m_out.phi(Int64, none, some));
        return;
    }

    if (isCompact() && !isSubtype(baseNode->type, TString)) {
        setJSValue(node, callStub(Stub::GetLength, Int64, { { base, GPRInfo::argumentGPR0 }, { slotAddress(allocateSite(node, static_cast<unsigned>(WellKnownIdentifier::Length))), GPRInfo::argumentGPR1 } }, { }));
        return;
    }

    LBasicBlock cellCase = m_out.newBlock();
    LBasicBlock arrayCase = m_out.newBlock();
    LBasicBlock notArrayCase = m_out.newBlock();
    LBasicBlock stringCase = m_out.newBlock();
    LBasicBlock ropeCase = m_out.newBlock();
    LBasicBlock notRopeCase = m_out.newBlock();
    LBasicBlock genericCase = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 4> results;

    if (isSubtype(baseNode->type, TCell))
        m_out.jump(cellCase);
    else
        m_out.branch(isCell(base), usually(cellCase), rarely(genericCase));

    m_out.appendTo(cellCase, arrayCase);
    if (mayBe(baseNode->type, TArray)) {
        LValue indexingType = m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc);
        LValue isArrayWithStorage = m_out.bitAnd(m_out.testNonZero32(indexingType, m_out.constInt32(IsArray)), m_out.testNonZero32(indexingType, m_out.constInt32(IndexingShapeMask)));
        m_out.branch(isArrayWithStorage, unsure(arrayCase), unsure(notArrayCase));
    } else
        m_out.jump(notArrayCase);

    m_out.appendTo(arrayCase, notArrayCase);
    LValue arrayLength = m_out.load32(m_out.loadPtr(base, m_heaps.JSObject_butterfly), m_heaps.Butterfly_publicLength);
    results.append(m_out.anchor(boxInt32(arrayLength)));
    m_out.branch(m_out.greaterThanOrEqual(arrayLength, m_out.int32Zero), usually(continuation), rarely(genericCase));

    m_out.appendTo(notArrayCase, stringCase);
    if (isSubtype(baseNode->type, TString))
        m_out.jump(stringCase);
    else if (mayBe(baseNode->type, TString))
        m_out.branch(isCellOfType(base, StringType), unsure(stringCase), unsure(genericCase));
    else
        m_out.jump(genericCase);

    m_out.appendTo(stringCase, ropeCase);
    LValue fiber = m_out.loadPtr(base, m_heaps.JSRopeString_fiber0);
    m_out.branch(m_out.testNonZeroPtr(fiber, m_out.constIntPtr(JSString::isRopeInPointer)), rarely(ropeCase), usually(notRopeCase));

    m_out.appendTo(ropeCase, notRopeCase);
    results.append(m_out.anchor(boxInt32(m_out.load32(base, m_heaps.JSRopeString_length))));
    m_out.jump(continuation);

    m_out.appendTo(notRopeCase, genericCase);
    results.append(m_out.anchor(boxInt32(m_out.load32(fiber, m_heaps.StringImpl_length))));
    m_out.jump(continuation);

    m_out.appendTo(genericCase, continuation);
    results.append(m_out.anchor(getByIdCached(node, base, baseNode->type, Entry::operationAOTGetByIdWellKnown, static_cast<unsigned>(WellKnownIdentifier::Length))));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, results));
}

bool Lowering::tryLowerPropertyVariant(Node* node)
{
    auto low = [&](VirtualRegister reg) { return lowJSValue(node->use(reg)); };
    auto strictness = [&](ECMAMode mode) { return m_out.constInt32(mode.isStrict()); };
    auto setBooleanResult = [&](LValue result) {
        setBoolean(node, m_out.notZero64(result));
        return true;
    };

    switch (node->opcode) {
    case op_get_length:
        lowerGetLength(node);
        return true;
    case op_get_by_id_direct: {
        auto bytecode = node->as<OpGetByIdDirect>();
        Node* baseNode = node->use(bytecode.m_base);
        setJSValue(node, getByIdCached(node, lowJSValue(baseNode), baseNode->type, Entry::operationAOTGetByIdDirect, bytecode.m_property));
        return true;
    }
    case op_get_by_id_with_this: {
        auto bytecode = node->as<OpGetByIdWithThis>();
        setJSValue(node, vmCall(node, Int64, Entry::operationAOTGetByIdWithThis, m_instance, low(bytecode.m_base), low(bytecode.m_thisValue), m_out.constInt32(numberOf(bytecode.m_property))));
        return true;
    }
    case op_get_by_val_with_this: {
        auto bytecode = node->as<OpGetByValWithThis>();
        setJSValue(node, vmCall(node, Int64, Entry::operationAOTGetByValWithThis, m_instance, low(bytecode.m_base), low(bytecode.m_thisValue), low(bytecode.m_property)));
        return true;
    }
    case op_put_by_id_with_this: {
        auto bytecode = node->as<OpPutByIdWithThis>();
        vmCall(node, Void, Entry::operationAOTPutByIdWithThis, m_instance, low(bytecode.m_base), low(bytecode.m_thisValue), low(bytecode.m_value), m_out.constInt32(numberOf(bytecode.m_property)), strictness(bytecode.m_ecmaMode));
        return true;
    }
    case op_put_by_val_with_this: {
        auto bytecode = node->as<OpPutByValWithThis>();
        vmCall(node, Void, Entry::operationAOTPutByValWithThis, m_instance, low(bytecode.m_base), low(bytecode.m_thisValue), low(bytecode.m_property), low(bytecode.m_value), strictness(bytecode.m_ecmaMode));
        return true;
    }
    case op_put_by_val_direct: {
        auto bytecode = node->as<OpPutByValDirect>();
        Node* baseNode = node->use(bytecode.m_base);
        Node* propertyNode = node->use(bytecode.m_property);
        Node* valueNode = node->use(bytecode.m_value);
        LValue base = lowJSValue(baseNode);
        LValue property = lowJSValue(propertyNode);
        LValue value = lowJSValue(valueNode);
        LBasicBlock slowCase = nullptr;
        LBasicBlock continuation = nullptr;
        if (usesStubs && mayBe(baseNode->type, TArray) && mayBe(propertyNode->type, TInt32)) {
            callStub(Stub::PutByValDirect, Void, { { base, GPRInfo::argumentGPR0 }, { property, GPRInfo::argumentGPR1 }, { value, GPRInfo::argumentGPR2 } },
                { { GPRInfo::argumentGPR3, bytecode.m_ecmaMode.isStrict() } });
            return true;
        }
        if (mayBe(baseNode->type, TArray) && mayBe(propertyNode->type, TInt32)) {
            slowCase = m_out.newBlock();
            continuation = m_out.newBlock();
            if (!isSubtype(propertyNode->type, TInt32))
                orElse(isInt32(property), slowCase);
            LValue index = unboxInt32(property);
            LValue mode = m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc);
            orElse(m_out.equal(m_out.bitAnd(mode, m_out.constInt32(IsArray | IndexingShapeMask | CopyOnWrite)), m_out.constInt32(ArrayWithContiguous)), slowCase);
            LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
            orElse(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_vectorLength)), slowCase);
            m_out.store64(value, m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index)));
            LBasicBlock isPastEnd = m_out.newBlock();
            LBasicBlock stored = m_out.newBlock();
            m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), unsure(stored), unsure(isPastEnd));
            m_out.appendTo(isPastEnd);
            m_out.store32(m_out.add(index, m_out.int32One), butterfly, m_heaps.Butterfly_publicLength);
            m_out.jump(stored);
            m_out.appendTo(stored);
            if (mayBe(valueNode->type, TCell))
                storeBarrier(base);
            m_out.jump(continuation);
            m_out.appendTo(slowCase);
        }
        vmCall(node, Void, Entry::operationAOTPutByValDirect, m_instance, base, property, value, strictness(bytecode.m_ecmaMode));
        if (continuation) {
            m_out.jump(continuation);
            m_out.appendTo(continuation);
        }
        return true;
    }
    case op_in_by_id: {
        auto bytecode = node->as<OpInById>();
        return setBooleanResult(vmCall(node, Int64, Entry::operationAOTInById, m_instance, low(bytecode.m_base), m_out.constInt32(numberOf(bytecode.m_property))));
    }
    case op_in_by_val: {
        auto bytecode = node->as<OpInByVal>();
        Node* baseNode = node->use(bytecode.m_base);
        Node* propertyNode = node->use(bytecode.m_property);
        bool isInLoop = m_block->isInLoop && !m_block->isGeneric;
        if ((isCompact() && !isInLoop) || !mayBe(baseNode->type, TAnyObject) || !mayBe(propertyNode->type, TNumber))
            return setBooleanResult(vmCall(node, Int64, Entry::operationAOTInByVal, m_instance, low(bytecode.m_base), low(bytecode.m_property)));

        LValue base = lowJSValue(baseNode);
        LBasicBlock indexReady = m_out.newBlock();
        LBasicBlock cellCase = m_out.newBlock();
        LBasicBlock rightShape = m_out.newBlock();
        LBasicBlock inBounds = m_out.newBlock();
        LBasicBlock slowCase = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue index = lowIndex(propertyNode, indexReady, slowCase);

        m_out.appendTo(indexReady, cellCase);
        if (isSubtype(baseNode->type, TCell))
            m_out.jump(cellCase);
        else
            m_out.branch(isCell(base), usually(cellCase), rarely(slowCase));

        m_out.appendTo(cellCase, rightShape);
        LValue shape = m_out.bitAnd(m_out.load8ZeroExt32(base, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask));
        m_out.branch(m_out.bitOr(m_out.equal(shape, m_out.constInt32(Int32Shape)), m_out.equal(shape, m_out.constInt32(ContiguousShape))), usually(rightShape), rarely(slowCase));

        m_out.appendTo(rightShape, inBounds);
        LValue butterfly = m_out.loadPtr(base, m_heaps.JSObject_butterfly);
        m_out.branch(m_out.below(index, m_out.load32(butterfly, m_heaps.Butterfly_publicLength)), usually(inBounds), rarely(slowCase));

        m_out.appendTo(inBounds, slowCase);
        ValueFromBlock isThere = m_out.anchor(m_out.booleanTrue);
        m_out.branch(m_out.notZero64(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(index)))), usually(continuation), rarely(slowCase));

        m_out.appendTo(slowCase, continuation);
        ValueFromBlock found = m_out.anchor(m_out.notZero64(vmCall(node, Int64, Entry::operationAOTInByVal, m_instance, base, lowJSValue(propertyNode))));
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        setBoolean(node, m_out.phi(Int32, isThere, found));
        return true;
    }
    case op_del_by_id: {
        auto bytecode = node->as<OpDelById>();
        return setBooleanResult(vmCall(node, Int64, Entry::operationAOTDelById, m_instance, low(bytecode.m_base), m_out.constInt32(numberOf(bytecode.m_property)), strictness(bytecode.m_ecmaMode)));
    }
    case op_del_by_val: {
        auto bytecode = node->as<OpDelByVal>();
        return setBooleanResult(vmCall(node, Int64, Entry::operationAOTDelByVal, m_instance, low(bytecode.m_base), low(bytecode.m_property), strictness(bytecode.m_ecmaMode)));
    }
    case op_get_private_name: {
        auto bytecode = node->as<OpGetPrivateName>();
        if constexpr (usesStubs)
            setJSValue(node, callStub(Stub::GetPrivateName, Int64, { { low(bytecode.m_base), GPRInfo::argumentGPR0 }, { low(bytecode.m_property), GPRInfo::argumentGPR1 }, { slotAddress(allocateSite(node, 0)), GPRInfo::argumentGPR2 } }, { }));
        else
            setJSValue(node, vmCall(node, Int64, Entry::operationAOTGetPrivateName, m_instance, low(bytecode.m_base), low(bytecode.m_property), m_out.int32Zero, slotAddress(allocateSlot()), m_out.int32Zero));
        return true;
    }
    case op_put_private_name: {
        auto bytecode = node->as<OpPutPrivateName>();
        if constexpr (usesStubs) {
            callStub(Stub::PutPrivateName, Void, { { low(bytecode.m_base), GPRInfo::argumentGPR0 }, { low(bytecode.m_property), GPRInfo::argumentGPR1 }, { low(bytecode.m_value), GPRInfo::argumentGPR2 },
                { slotAddress(allocateSite(node, 0, bytecode.m_putKind.isDefine())), GPRInfo::argumentGPR3 } }, { });
        } else
            vmCall(node, Void, Entry::operationAOTPutPrivateName, m_instance, low(bytecode.m_base), low(bytecode.m_property), low(bytecode.m_value), m_out.int32Zero, slotAddress(allocateSlot()), m_out.constInt32(bytecode.m_putKind.isDefine()));
        return true;
    }
    case op_has_private_name: {
        auto bytecode = node->as<OpHasPrivateName>();
        return setBooleanResult(vmCall(node, Int64, Entry::operationAOTHasPrivateName, m_instance, low(bytecode.m_base), low(bytecode.m_property)));
    }
    case op_has_private_brand: {
        auto bytecode = node->as<OpHasPrivateBrand>();
        return setBooleanResult(vmCall(node, Int64, Entry::operationAOTHasPrivateBrand, m_instance, low(bytecode.m_base), low(bytecode.m_brand)));
    }
    case op_check_private_brand: {
        auto bytecode = node->as<OpCheckPrivateBrand>();
        if constexpr (usesStubs)
            callStub(Stub::CheckPrivateBrand, Void, { { low(bytecode.m_base), GPRInfo::argumentGPR0 }, { low(bytecode.m_brand), GPRInfo::argumentGPR1 }, { slotAddress(allocateSite(node, 0)), GPRInfo::argumentGPR2 } }, { });
        else
            vmCall(node, Void, Entry::operationAOTCheckPrivateBrand, m_instance, low(bytecode.m_base), low(bytecode.m_brand), m_out.int32Zero, slotAddress(allocateSlot()), m_out.int32Zero);
        return true;
    }
    case op_set_private_brand: {
        auto bytecode = node->as<OpSetPrivateBrand>();
        vmCall(node, Void, Entry::operationAOTSetPrivateBrand, m_instance, low(bytecode.m_base), low(bytecode.m_brand));
        return true;
    }
    case op_put_getter_by_id: {
        auto bytecode = node->as<OpPutGetterById>();
        vmCall(node, Void, Entry::operationAOTPutAccessorById, m_instance, low(bytecode.m_base), m_out.constInt32(numberOf(bytecode.m_property)), m_out.constInt32(bytecode.m_attributes), low(bytecode.m_accessor), m_out.constInt32(false));
        return true;
    }
    case op_put_setter_by_id: {
        auto bytecode = node->as<OpPutSetterById>();
        vmCall(node, Void, Entry::operationAOTPutAccessorById, m_instance, low(bytecode.m_base), m_out.constInt32(numberOf(bytecode.m_property)), m_out.constInt32(bytecode.m_attributes), low(bytecode.m_accessor), m_out.constInt32(true));
        return true;
    }
    case op_put_getter_setter_by_id: {
        auto bytecode = node->as<OpPutGetterSetterById>();
        vmCall(node, Void, Entry::operationAOTPutGetterSetterById, m_instance, low(bytecode.m_base), m_out.constInt32(numberOf(bytecode.m_property)), m_out.constInt32(bytecode.m_attributes), low(bytecode.m_getter), low(bytecode.m_setter));
        return true;
    }
    case op_put_getter_by_val: {
        auto bytecode = node->as<OpPutGetterByVal>();
        vmCall(node, Void, Entry::operationAOTPutAccessorByVal, m_instance, low(bytecode.m_base), low(bytecode.m_property), m_out.constInt32(bytecode.m_attributes), low(bytecode.m_accessor), m_out.constInt32(false));
        return true;
    }
    case op_put_setter_by_val: {
        auto bytecode = node->as<OpPutSetterByVal>();
        vmCall(node, Void, Entry::operationAOTPutAccessorByVal, m_instance, low(bytecode.m_base), low(bytecode.m_property), m_out.constInt32(bytecode.m_attributes), low(bytecode.m_accessor), m_out.constInt32(true));
        return true;
    }
    case op_define_data_property: {
        auto bytecode = node->as<OpDefineDataProperty>();
        vmCall(node, Void, isInRunOnceCode() ? Entry::operationAOTDefineDataPropertyOnSingleton : Entry::operationAOTDefineDataProperty, m_instance, low(bytecode.m_base), low(bytecode.m_property), low(bytecode.m_value), unboxInt32(low(bytecode.m_attributes)));
        return true;
    }
    case op_define_accessor_property: {
        auto bytecode = node->as<OpDefineAccessorProperty>();
        vmCall(node, Void, Entry::operationAOTDefineAccessorProperty, m_instance, low(bytecode.m_base), low(bytecode.m_property), low(bytecode.m_getter), low(bytecode.m_setter), unboxInt32(low(bytecode.m_attributes)));
        return true;
    }
    case op_get_from_arguments: {
        auto bytecode = node->as<OpGetFromArguments>();
        setJSValue(node, m_out.load64(lowCell(node->use(bytecode.m_arguments)), m_heaps.DirectArguments_storage[bytecode.m_index]));
        return true;
    }
    case op_put_to_arguments: {
        auto bytecode = node->as<OpPutToArguments>();
        LValue arguments = lowCell(node->use(bytecode.m_arguments));
        m_out.store64(low(bytecode.m_value), arguments, m_heaps.DirectArguments_storage[bytecode.m_index]);
        if (mayBe(node->use(bytecode.m_value)->type, TCell))
            storeBarrier(arguments);
        return true;
    }
    default:
        return false;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && CPU(ARM64)
