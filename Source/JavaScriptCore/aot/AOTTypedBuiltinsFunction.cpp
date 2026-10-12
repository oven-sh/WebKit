/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedPrepareBoundFunctionAllocation, void, (Instance* instance))
{
    prepareInlineAllocation<JSBoundFunction>(instance, Instance::InlineAllocation::BoundFunction, instance->globalObject->boundFunctionStructure());
    RELEASE_ASSERT(static_cast<LocalAllocator*>(instance->inlineAllocations[static_cast<unsigned>(Instance::InlineAllocation::BoundFunction)].allocator)->cellSize() == sizeof(JSBoundFunction));
}

namespace Typed {

using namespace B3;

// The family aot/builtins/function.def.

std::optional<Writes> writesOfInlineFunction(InlineBuiltin, std::span<const Type>)
{
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<Operand> Lowering::lowerInlineFunction(InlineBuiltin, ValueID, const Value&, Writes)
{
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinID Lowering::bindIfNamed(StringID name) const
{
    static const BuiltinName bind = Builtins::singleton().nameOf("bind"_s.span8());
    return m_program.builtinName(name) == bind ? m_builtins.find(BuiltinKind::Method, m_builtins.functionClass().name, bind) : BuiltinID();
}

// A closure with the structure it was allocated with has no property of its own and inherits from Function.prototype, which is immutable: it finds the built-in function, and its `name` and
// `length` are what it was compiled with. So the bound function is made as FTL::LowerDFGToB3::compileNewBoundFunction() makes it, and works out its own two when it is asked:
// JSBoundFunction::canSkipNameAndLengthMaterialization(). A class may declare `static bind`, which does not show in its structure.
std::optional<Operand> Lowering::lowerBind(ValueID function, StringID name, std::optional<Boxed> method, const Arguments& arguments, const Type& type)
{
    BuiltinID bind = bindIfNamed(name);
    auto& functionType = typeOf(function);
    if (!bind || !(functionType.anyFunction || functionType.anyObject) || arguments.size() > 1 + JSBoundFunction::maxEmbeddedArgs)
        return std::nullopt;
    static_assert(sizeof(JSBoundFunction) == 96 && JSBoundFunction::offsetOfCanConstruct() == JSBoundFunction::offsetOfBoundArgsLength() + sizeof(uint32_t) && JSBoundFunction::offsetOfCanConstruct() + sizeof(uint32_t) == sizeof(JSBoundFunction));
    constexpr auto kind = Instance::InlineAllocation::BoundFunction;

    Boxed callee = in<Boxed>(function);
    Boxed boundThis = arguments.isEmpty() ? undefined() : in<Boxed>(arguments[0]);
    Boxed boundArguments[JSBoundFunction::maxEmbeddedArgs];
    unsigned numberOfBoundArguments = arguments.isEmpty() ? 0 : arguments.size() - 1;
    for (unsigned i = 0; i < JSBoundFunction::maxEmbeddedArgs; ++i)
        boundArguments[i] = i < numberOfBoundArguments ? in<Boxed>(arguments[i + 1]) : boxedConstant(JSValue());

    Rep rep = repOf(type);
    LBasicBlock generally = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    if (method)
        orElse(equal(*method, box(builtinCell(m_builtins.cellOf(bind)))), generally);
    if (functionType.primitives & ~(Primitive::String | Primitive::Symbol | Primitive::BigInt))
        orElse(isCell(callee, functionType), generally);
    CellPointer target = asCell(callee);
    const Function* definedAs = functionDefining(function);
    auto hasStructureIDAt = [&](ptrdiff_t offset) {
        return equal(loadField<Word32>(target, m_heaps.JSCell_structureID), Word32(loadRuntimeState32(offset)));
    };
    auto isFullClosureThatIsCalledOrElse = [&] {
        orElse(hasStructureIDAt(Instance::offsetOfTypedClosureStructureID()), generally);
        if (!definedAs || definedAs->kind == FunctionKind::Constructor)
            orElse(isZero(bitAnd(loadField<Integer>(target, m_heaps.AOTTypedClosure_entry), integer(static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit)))), generally);
    };
    if (definedAs && m_program.makesSlimClosures(*definedAs))
        orElse(hasStructureIDAt(Instance::offsetOfTypedSlimClosureHeader(0)), generally);
    else if (definedAs)
        isFullClosureThatIsCalledOrElse();
    else {
        LBasicBlock isClosureThatIsCalled = m_out.newBlock();
        LBasicBlock isNotSlim = m_out.newBlock();
        m_out.branch(hasStructureIDAt(Instance::offsetOfTypedSlimClosureHeader(0)).value(), usually(isClosureThatIsCalled), rarely(isNotSlim));
        m_out.appendTo(isNotSlim);
        isFullClosureThatIsCalledOrElse();
        m_out.jump(isClosureThatIsCalled);
        m_out.appendTo(isClosureThatIsCalled);
    }
    // Null until the general path has been taken once.
    LValue allocator = m_out.loadPtr(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfAllocatorFor(kind)));
    orElse(m_out.notNull(allocator), generally);
    CellPointer bound = allocateCell(allocator, sizeof(JSBoundFunction), generally);
    m_out.store64(m_out.load64(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfHeaderFor(kind))), bound.value(), m_heaps.JSCell_header);
    m_out.storePtr(m_out.intPtrZero, bound.value(), m_heaps.JSObject_butterfly);
    m_out.storePtr(fixedPointer(Instance::offsetOfGlobalObject()), bound.value(), m_heaps.JSCallee_scope);
    m_out.storePtr(fixedPointer(Instance::offsetOfBoundFunctionExecutable()), bound.value(), m_heaps.JSFunction_executableOrRareData);
    m_out.storePtr(target.value(), bound.value(), m_heaps.JSBoundFunction_targetFunction);
    m_out.store64(boundThis.value(), bound.value(), m_heaps.JSBoundFunction_boundThis);
    m_out.store64(boundArguments[0].value(), bound.value(), m_heaps.JSBoundFunction_boundArg0);
    m_out.store64(boundArguments[1].value(), bound.value(), m_heaps.JSBoundFunction_boundArg1);
    m_out.store64(boundArguments[2].value(), bound.value(), m_heaps.JSBoundFunction_boundArg2);
    m_out.storePtr(m_out.intPtrZero, bound.value(), m_heaps.JSBoundFunction_nameMayBeNull);
    m_out.store64(integer(std::bit_cast<int64_t>(PNaN)).value(), bound.value(), m_heaps.JSBoundFunction_length);
    m_out.store32(m_out.constInt32(numberOfBoundArguments), bound.value(), m_heaps.JSBoundFunction_boundArgsLength);
    // With it: not tainted.
    m_out.store32(m_out.constInt32(static_cast<uint8_t>(TriState::Indeterminate)), bound.value(), m_heaps.JSBoundFunction_canConstruct);
    fenceAfterInitialization();
    join.add(ensure(bound, m_program.at(m_program.types.function), type));

    m_out.appendTo(generally);
    AOT_TYPED_OPERATION(PrepareBoundFunctionAllocation);
    join.add(ensure(method ? callGenerally(*method, callee, arguments) : callMethodGenerally(callee, name, arguments), type));
    return join.finish();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
