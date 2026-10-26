/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTEnvironment.h"
#include "AOTErrorMessages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTModules.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "BuiltinNames.h"
#include "CellSize.h"
#include "DirectArguments.h"
#include "DirectEvalCodeCache.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "Interpreter.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSDestructibleObject.h"
#include "JSLexicalEnvironmentInlines.h"
#include "JSNativeStdFunction.h"
#include "JSWithScope.h"
#include "ObjectConstructor.h"
#include "Symbol.h"
#include "SymbolTable.h"
#include "VariableEnvironment.h"
#include <algorithm>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <wtf/HashMap.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

enum class EvalBindingForm : uint8_t { ModuleVariable, EnvironmentSlot, MappedArgument, Operand };

static EvalBindingKind kindOf(const EvalBindingData& binding)
{
    RELEASE_ASSERT(binding.kind <= static_cast<uint32_t>(EvalBindingKind::SimpleCatchParameter));
    return static_cast<EvalBindingKind>(binding.kind);
}

static EvalBindingForm formOf(const EvalBindingData& binding)
{
    if (kindOf(binding) == EvalBindingKind::MappedArgument)
        return EvalBindingForm::MappedArgument;
    if (binding.operand == EvalBindingData::noOperand)
        return EvalBindingForm::ModuleVariable;
    return binding.slot.offset ? EvalBindingForm::EnvironmentSlot : EvalBindingForm::Operand;
}

static JSValue operandOrEmpty(JSCellButterfly* operands, uint32_t operand)
{
    if (operand == EvalBindingData::noOperand)
        return JSValue();
    RELEASE_ASSERT(operand < operands->length());
    return operands->get(operand);
}

template<typename CellType>
static CellType* cellOperandOf(JSCellButterfly* operands, const EvalBindingData& binding)
{
    JSValue value = operandOrEmpty(operands, binding.operand);
    auto* cell = value && value.isCell() ? dynamicDowncast<CellType>(value.asCell()) : nullptr;
    RELEASE_ASSERT(cell);
    return cell;
}

static DirectArguments* mappedArgumentsOf(JSCellButterfly* operands, const EvalBindingData& binding)
{
    auto* arguments = cellOperandOf<DirectArguments>(operands, binding);
    RELEASE_ASSERT(DirectArguments::offsetOfSlot(binding.slot.offset) < cellSize(arguments));
    return arguments;
}

static JSValue readBinding(Instance* instance, JSCellButterfly* operands, const EvalBindingData& binding)
{
    switch (formOf(binding)) {
    case EvalBindingForm::ModuleVariable:
        return ProgramStatics::readVariable(*instance, binding.slot);
    case EvalBindingForm::EnvironmentSlot:
        return readSlot(binding.slot.representation(), &cellOperandOf<Environment>(operands, binding)->at<uint8_t>(binding.slot.offset), *instance->state->strings);
    case EvalBindingForm::MappedArgument:
        return mappedArgumentsOf(operands, binding)->argument(DirectArgumentsOffset(binding.slot.offset)).get();
    case EvalBindingForm::Operand:
        return operandOrEmpty(operands, binding.operand);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue boundThisOrEmpty(JSValue thisValue, bool isInDerivedConstructor)
{
    return isInDerivedConstructor && thisValue.isUndefined() ? JSValue() : thisValue;
}

// `this` of a derived constructor names the constructor, and lies where compiled code reads it again.
static bool canBeBoundByText(const EvalBindingData& thisBinding)
{
    return thisBinding.function != EvalBindingData::noOperand && formOf(thisBinding) == EvalBindingForm::EnvironmentSlot;
}

static const EvalBindingData* findBinding(std::span<const EvalBindingData> bindings, EvalBindingKind kind)
{
    for (auto& binding : bindings) {
        if (kindOf(binding) == kind)
            return &binding;
    }
    return nullptr;
}

static bool isAssignableBinding(EvalBindingKind kind)
{
    return kind == EvalBindingKind::Variable || kind == EvalBindingKind::Arguments || kind == EvalBindingKind::MappedArgument || kind == EvalBindingKind::SimpleCatchParameter;
}

static bool isNamedBinding(EvalBindingKind kind)
{
    return isAssignableBinding(kind) || kind == EvalBindingKind::Constant || kind == EvalBindingKind::FunctionName
        || kind == EvalBindingKind::PrivateName || kind == EvalBindingKind::PrivateMethod || kind == EvalBindingKind::PrivateAccessor;
}

static unsigned attributesOf(EvalBindingKind kind)
{
    RELEASE_ASSERT(isNamedBinding(kind) || kind == EvalBindingKind::This || kind == EvalBindingKind::HomeObject);
    return isAssignableBinding(kind) ? static_cast<unsigned>(PropertyAttribute::DontDelete) : PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly;
}

static bool isStaticPrivateField(const EvalBindingData& binding)
{
    return kindOf(binding) == EvalBindingKind::PrivateName && !binding.privateKey;
}

static std::optional<PrivateNameEntry> privateNameEntryOf(const EvalBindingData& binding)
{
    using Traits = PrivateNameEntry::Traits;
    uint16_t staticTrait = binding.privateKey ? Traits::None : Traits::IsStatic;
    switch (kindOf(binding)) {
    case EvalBindingKind::PrivateName:
        return PrivateNameEntry(isStaticPrivateField(binding) ? Traits::IsStatic | Traits::IsGetter | Traits::IsSetter : Traits::None);
    case EvalBindingKind::PrivateMethod:
        return PrivateNameEntry(staticTrait | Traits::IsMethod);
    case EvalBindingKind::PrivateAccessor: {
        uint16_t getterTrait = binding.function == EvalBindingData::noOperand ? Traits::None : Traits::IsGetter;
        uint16_t setterTrait = binding.setter == EvalBindingData::noOperand ? Traits::None : Traits::IsSetter;
        return PrivateNameEntry(staticTrait | getterTrait | setterTrait);
    }
    case EvalBindingKind::Variable:
    case EvalBindingKind::Constant:
    case EvalBindingKind::FunctionName:
    case EvalBindingKind::This:
    case EvalBindingKind::NewTarget:
    case EvalBindingKind::HomeObject:
    case EvalBindingKind::ClassObject:
    case EvalBindingKind::Arguments:
    case EvalBindingKind::WithObject:
    case EvalBindingKind::EvalVariables:
    case EvalBindingKind::MappedArgument:
    case EvalBindingKind::SimpleCatchParameter:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool needsPrivateBrand(const EvalBindingData& binding)
{
    std::optional<PrivateNameEntry> entry = privateNameEntryOf(binding);
    return entry && entry->isPrivateMethodOrAccessor();
}

static Symbol* privateSymbolAt(Instance* instance, uint32_t privateKey)
{
    UniquedStringImpl* uid = identifierAt(instance, privateKey).impl();
    RELEASE_ASSERT(uid && uid->isSymbol() && static_cast<SymbolImpl*>(uid)->isPrivate());
    return Symbol::create(*instance->vm, static_cast<SymbolImpl&>(*uid));
}

static JSObject* createAccessorPair(VM& vm, JSGlobalObject* globalObject, JSValue getter, JSValue setter)
{
    auto& builtinNames = vm.propertyNames->builtinNames();
    JSObject* pair = constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure());
    if (getter)
        pair->putDirect(vm, builtinNames.getPrivateName(), getter);
    if (setter)
        pair->putDirect(vm, builtinNames.setPrivateName(), setter);
    return pair;
}

// What evaluated text finds as the derived constructor. `super()` constructs its prototype: that of the class at that moment.
class DerivedConstructorStandIn final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetPrototype;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject) { return Structure::create(vm, globalObject, jsNull(), TypeInfo(ObjectType, StructureFlags), info()); }
    static DerivedConstructorStandIn* create(VM&, Structure*, JSObject* constructor);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static JSValue getPrototype(JSObject*, JSGlobalObject*);

private:
    DerivedConstructorStandIn(VM& vm, Structure* structure, JSObject* constructor)
        : Base(vm, structure)
        , m_constructor(constructor, WriteBarrierEarlyInit)
    {
    }

    WriteBarrier<JSObject> m_constructor;
};

const ClassInfo DerivedConstructorStandIn::s_info = { "DerivedConstructorStandIn"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(DerivedConstructorStandIn) };

DerivedConstructorStandIn* DerivedConstructorStandIn::create(VM& vm, Structure* structure, JSObject* constructor)
{
    auto* result = new (NotNull, allocateCell<DerivedConstructorStandIn>(vm)) DerivedConstructorStandIn(vm, structure, constructor);
    result->finishCreation(vm);
    return result;
}

template<typename Visitor>
void DerivedConstructorStandIn::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<DerivedConstructorStandIn>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_constructor);
}

DEFINE_VISIT_CHILDREN(DerivedConstructorStandIn);

JSValue DerivedConstructorStandIn::getPrototype(JSObject* object, JSGlobalObject* globalObject)
{
    return uncheckedDowncast<DerivedConstructorStandIn>(object)->m_constructor->getPrototype(globalObject);
}

class EvalBindings final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesPut | GetOwnPropertySlotIsImpureForPropertyAbsence;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject) { return Structure::create(vm, globalObject, jsNull(), TypeInfo(ObjectType, StructureFlags), info()); }
    static EvalBindings* create(VM&, Structure*, Instance*, const DirectEvalRequest&, JSCellButterfly* operands, JSObject* homeObjectHolder, std::span<const EvalBindingData>);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);

    const EvalBindingData* find(VM&, PropertyName) const;
    JSValue read(const EvalBindingData&) const;

private:
    EvalBindings(VM& vm, Structure* structure, Instance* instance, const DirectEvalRequest& request, JSCellButterfly* operands, JSObject* homeObjectHolder, std::span<const EvalBindingData> bindings)
        : Base(vm, structure)
        , m_instance(instance)
        , m_request(&request)
        , m_first(bindings.empty() ? 0 : static_cast<uint32_t>(bindings.data() - request.bindings))
        , m_count(bindings.size())
        , m_operands(operands, WriteBarrierEarlyInit)
        , m_homeObjectHolder(homeObjectHolder, WriteBarrierEarlyInit)
    {
    }

    std::span<const EvalBindingData> bindings() const { return unsafeMakeSpan(m_request->bindings + m_first, m_count); }
    uint32_t privateValueIndexOf(const EvalBindingData& binding) const { return m_operands->length() - m_request->numberOfBindings + static_cast<uint32_t>(&binding - m_request->bindings); }
    JSCell* createPrivateValue(VM&, const EvalBindingData&);
    JSObject* createStaticFieldAccessors(VM&, const EvalBindingData&);
    JSValue readStaticField(JSGlobalObject*, const EvalBindingData&) const;
    void storeStaticField(JSGlobalObject*, const EvalBindingData&, JSValue);
    bool storeVariable(JSGlobalObject*, const EvalBindingData&, PropertyName, JSValue);
    void writeBinding(VM&, const EvalBindingData&, JSValue);

    Instance* m_instance;
    const DirectEvalRequest* m_request;
    uint32_t m_first;
    uint32_t m_count;
    WriteBarrier<JSCellButterfly> m_operands;
    WriteBarrier<JSObject> m_homeObjectHolder;
};

const ClassInfo EvalBindings::s_info = { "EvalBindings"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(EvalBindings) };

JSCell* EvalBindings::createPrivateValue(VM& vm, const EvalBindingData& binding)
{
    if (isStaticPrivateField(binding))
        return createStaticFieldAccessors(vm, binding);
    if (kindOf(binding) == EvalBindingKind::PrivateName)
        return privateSymbolAt(m_instance, binding.privateKey);
    if (kindOf(binding) != EvalBindingKind::PrivateAccessor)
        return nullptr;
    JSCellButterfly* operands = m_operands.get();
    return createAccessorPair(vm, realm(), operandOrEmpty(operands, binding.function), operandOrEmpty(operands, binding.setter));
}

JSObject* EvalBindings::createStaticFieldAccessors(VM& vm, const EvalBindingData& binding)
{
    const EvalBindingData* field = &binding;
    JSObject* getter = JSNativeStdFunction::create(vm, realm(), 0, String(), [this, field](JSGlobalObject* lexicalGlobalObject, CallFrame*) {
        return JSValue::encode(readStaticField(lexicalGlobalObject, *field));
    }, JSValue(this));
    JSObject* setter = JSNativeStdFunction::create(vm, realm(), 1, String(), [this, field](JSGlobalObject* lexicalGlobalObject, CallFrame* callFrame) {
        storeStaticField(lexicalGlobalObject, *field, callFrame->argument(0));
        return JSValue::encode(jsUndefined());
    }, JSValue(this));
    return createAccessorPair(vm, realm(), getter, setter);
}

JSValue EvalBindings::readStaticField(JSGlobalObject* globalObject, const EvalBindingData& binding) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = readBinding(m_instance, m_operands.get(), binding);
    if (!value)
        throwException(globalObject, scope, createInvalidPrivateNameError(globalObject));
    return value;
}

void EvalBindings::storeStaticField(JSGlobalObject* globalObject, const EvalBindingData& binding, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    readStaticField(globalObject, binding);
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    storeVariable(globalObject, binding, identifierAt(m_instance, binding.name), value);
}

EvalBindings* EvalBindings::create(VM& vm, Structure* structure, Instance* instance, const DirectEvalRequest& request, JSCellButterfly* operands, JSObject* homeObjectHolder, std::span<const EvalBindingData> bindings)
{
    auto* result = new (NotNull, allocateCell<EvalBindings>(vm)) EvalBindings(vm, structure, instance, request, operands, homeObjectHolder, bindings);
    result->finishCreation(vm);
    for (auto& binding : bindings) {
        if (JSCell* value = result->createPrivateValue(vm, binding))
            operands->setIndex(vm, result->privateValueIndexOf(binding), value);
    }
    return result;
}

template<typename Visitor>
void EvalBindings::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<EvalBindings>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_operands);
    visitor.append(thisObject->m_homeObjectHolder);
}

DEFINE_VISIT_CHILDREN(EvalBindings);

const EvalBindingData* EvalBindings::find(VM& vm, PropertyName name) const
{
    auto& builtinNames = vm.propertyNames->builtinNames();
    if (name == builtinNames.thisPrivateName())
        return findBinding(bindings(), EvalBindingKind::This);
    if (name == builtinNames.derivedConstructorPrivateName())
        return findBinding(bindings(), EvalBindingKind::HomeObject);

    std::optional<uint32_t> number = vm.aotState().identifiers->numberOf(name.uid());
    if (!number)
        return nullptr;
    auto bindingsOfRequest = unsafeMakeSpan(m_request->bindings, m_request->numberOfBindings);
    auto bindingsWithName = std::ranges::equal_range(m_request->bindingsByName(), *number, { }, [&](uint32_t index) {
        return bindingsOfRequest[index].name;
    });
    for (uint32_t index : bindingsWithName) {
        bool isAmongOwnBindings = index - m_first < m_count;
        if (isAmongOwnBindings && isNamedBinding(kindOf(bindingsOfRequest[index])))
            return &bindingsOfRequest[index];
    }
    return nullptr;
}

JSValue EvalBindings::read(const EvalBindingData& binding) const
{
    EvalBindingKind kind = kindOf(binding);
    if (kind == EvalBindingKind::HomeObject)
        return m_homeObjectHolder.get();
    if (kind == EvalBindingKind::PrivateName || kind == EvalBindingKind::PrivateAccessor)
        return m_operands->get(privateValueIndexOf(binding));
    if (kind == EvalBindingKind::PrivateMethod)
        return operandOrEmpty(m_operands.get(), binding.function);
    JSValue value = readBinding(m_instance, m_operands.get(), binding);
    if (kind == EvalBindingKind::This)
        return boundThisOrEmpty(value, m_request->isInDerivedConstructor);
    return value;
}

bool EvalBindings::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<EvalBindings>(object);
    const EvalBindingData* binding = thisObject->find(vm, name);
    if (!binding)
        return false;
    EvalBindingKind kind = kindOf(*binding);
    if (slot.internalMethodType() != PropertySlot::InternalMethodType::Get) {
        slot.setValue(object, attributesOf(kind), jsUndefined());
        return true;
    }
    JSValue value = thisObject->read(*binding);
    if (!value && kind != EvalBindingKind::This) {
        throwException(globalObject, scope, createTDZError(globalObject, StringView(name.uid())));
        return false;
    }
    slot.setValue(object, attributesOf(kind), value);
    return true;
}

void EvalBindings::writeBinding(VM& vm, const EvalBindingData& binding, JSValue value)
{
    ProgramStrings& strings = *m_instance->state->strings;
    SlotRepresentation representation = binding.slot.representation();
    uint32_t offset = binding.slot.offset;
    switch (formOf(binding)) {
    case EvalBindingForm::ModuleVariable: {
        RELEASE_ASSERT(representation != SlotRepresentation::DeferredClosure);
        ProgramStatics& statics = *m_instance->state->statics;
        statics.ensureProgramStaticsInitialized();
        RELEASE_ASSERT(tryWriteSlot(strings, representation, m_instance->statics() + offset, value));
        if (traitsOf(representation).holdsCell)
            statics.didStoreRootInStatics(offset);
        return;
    }
    case EvalBindingForm::EnvironmentSlot: {
        auto* environment = cellOperandOf<Environment>(m_operands.get(), binding);
        RELEASE_ASSERT(tryWriteSlot(strings, representation, &environment->at<uint8_t>(offset), value));
        vm.writeBarrier(environment, value);
        return;
    }
    case EvalBindingForm::MappedArgument: {
        DirectArguments* arguments = mappedArgumentsOf(m_operands.get(), binding);
        arguments->argument(DirectArgumentsOffset(offset)).set(vm, arguments, value);
        return;
    }
    case EvalBindingForm::Operand:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool EvalBindings::storeVariable(JSGlobalObject* globalObject, const EvalBindingData& binding, PropertyName name, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!read(binding)) {
        throwException(globalObject, scope, createTDZError(globalObject, StringView(name.uid())));
        return false;
    }
    if (formOf(binding) == EvalBindingForm::Operand) {
        throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
        return false;
    }
    writeBinding(vm, binding, value);
    return true;
}

bool EvalBindings::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<EvalBindings>(cell);
    const EvalBindingData* binding = thisObject->find(vm, name);
    if (!binding)
        return false;
    EvalBindingKind kind = kindOf(*binding);
    if (isAssignableBinding(kind))
        RELEASE_AND_RETURN(scope, thisObject->storeVariable(globalObject, *binding, name, value));
    if (kind == EvalBindingKind::FunctionName && !slot.isStrictMode())
        return true;
    if (kind == EvalBindingKind::This) {
        if (!canBeBoundByText(*binding)) {
            throwException(globalObject, scope, createSuperCallInDirectEvalError(globalObject));
            return false;
        }
        thisObject->writeBinding(vm, *binding, value);
        return true;
    }
    RELEASE_ASSERT(isNamedBinding(kind));
    throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
    return false;
}

bool EvalBindings::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot&)
{
    return !uncheckedDowncast<EvalBindings>(cell)->find(globalObject->vm(), name);
}

struct EvalVariableScopeContents {
    EvalBindings* ownBindings { nullptr };
    JSObject* variables { nullptr };
    JSValue privateClassBrand { };
    JSValue privateBrand { };
};

class EvalVariableScope final : public JSLexicalEnvironment {
public:
    using Base = JSLexicalEnvironment;

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject) { return Structure::create(vm, globalObject, jsNull(), TypeInfo(LexicalEnvironmentType, StructureFlags), info()); }
    static EvalVariableScope* create(VM&, Structure*, JSScope* next, SymbolTable::ScopeType, const EvalVariableScopeContents&);

    DECLARE_INFO;

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);

private:
    static constexpr unsigned numberOfScopeOffsets = 4;
    static constexpr ScopeOffset privateClassBrandScopeOffset() { return ScopeOffset(PrivateNameEntry::privateClassBrandOffset); }
    static constexpr ScopeOffset privateBrandScopeOffset() { return ScopeOffset(PrivateNameEntry::privateBrandOffset); }
    static constexpr ScopeOffset ownBindingsScopeOffset() { return ScopeOffset(2); }
    static constexpr ScopeOffset variablesScopeOffset() { return ScopeOffset(3); }
    static_assert(PrivateNameEntry::privateClassBrandOffset < 2 && PrivateNameEntry::privateBrandOffset < 2);

    EvalVariableScope(VM& vm, Structure* structure, JSScope* next, SymbolTable* symbolTable)
        : Base(vm, structure, next, symbolTable, jsUndefined())
    {
    }

    EvalBindings* ownBindings()
    {
        JSValue value = variableAt(ownBindingsScopeOffset()).get();
        return value.isUndefined() ? nullptr : uncheckedDowncast<EvalBindings>(value.asCell());
    }

    JSObject* variablesObject()
    {
        JSValue value = variableAt(variablesScopeOffset()).get();
        return value.isUndefined() ? nullptr : asObject(value);
    }
};

const ClassInfo EvalVariableScope::s_info = { "EvalVariableScope"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(EvalVariableScope) };

EvalVariableScope* EvalVariableScope::create(VM& vm, Structure* structure, JSScope* next, SymbolTable::ScopeType scopeType, const EvalVariableScopeContents& contents)
{
    bool isVarScope = scopeType == SymbolTable::ScopeType::VarScope;
    RELEASE_ASSERT(isVarScope == !!contents.variables && (contents.ownBindings || contents.variables));
    SymbolTable* symbolTable = SymbolTable::create(vm);
    symbolTable->setScopeType(scopeType);
    symbolTable->setUsesSloppyEval(isVarScope);
    for (unsigned i = 0; i < numberOfScopeOffsets; ++i)
        symbolTable->takeNextScopeOffset();
    auto* result = new (NotNull, allocateCell<EvalVariableScope>(vm, allocationSize(symbolTable))) EvalVariableScope(vm, structure, next, symbolTable);
    result->finishCreation(vm);
    result->variableAt(privateClassBrandScopeOffset()).set(vm, result, contents.privateClassBrand);
    result->variableAt(privateBrandScopeOffset()).set(vm, result, contents.privateBrand);
    if (contents.ownBindings)
        result->variableAt(ownBindingsScopeOffset()).set(vm, result, contents.ownBindings);
    if (contents.variables)
        result->variableAt(variablesScopeOffset()).set(vm, result, contents.variables);
    return result;
}

bool EvalVariableScope::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<EvalVariableScope>(object);
    if (EvalBindings* ownBindings = thisObject->ownBindings()) {
        bool isFound = EvalBindings::getOwnPropertySlot(ownBindings, globalObject, name, slot);
        RETURN_IF_EXCEPTION(scope, false);
        if (isFound)
            return true;
    }
    JSObject* variables = thisObject->variablesObject();
    if (!variables)
        return false;
    RELEASE_AND_RETURN(scope, variables->methodTable()->getOwnPropertySlot(variables, globalObject, name, slot));
}

bool EvalVariableScope::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    auto* thisObject = uncheckedDowncast<EvalVariableScope>(cell);
    EvalBindings* ownBindings = thisObject->ownBindings();
    if (ownBindings && ownBindings->find(globalObject->vm(), name))
        return EvalBindings::put(ownBindings, globalObject, name, value, slot);
    JSObject* variables = thisObject->variablesObject();
    if (!variables)
        return false;
    PutPropertySlot variablesSlot(variables, false);
    return variables->methodTable()->put(variables, globalObject, name, value, variablesSlot);
}

bool EvalVariableScope::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    auto* thisObject = uncheckedDowncast<EvalVariableScope>(cell);
    EvalBindings* ownBindings = thisObject->ownBindings();
    if (ownBindings && ownBindings->find(globalObject->vm(), name))
        return false;
    JSObject* variables = thisObject->variablesObject();
    if (!variables)
        return true;
    return variables->methodTable()->deleteProperty(variables, globalObject, name, slot);
}

static Structure* ensureStructure(VM& vm, Instance& instance, Structure* DirectEvalStructures::* member, Structure* (*create)(VM&, JSGlobalObject*))
{
    Structure*& structure = instance.state->objects->caches->directEvalStructures.*member;
    if (!structure) {
        structure = create(vm, instance.globalObject);
        instance.state->immortalCells->add(structure);
    }
    return structure;
}

static bool hasOwnScope(const EvalBindingData& binding)
{
    return kindOf(binding) == EvalBindingKind::WithObject || kindOf(binding) == EvalBindingKind::SimpleCatchParameter;
}

static JSC_DECLARE_HOST_FUNCTION(throwSuperCallInDirectEvalError);

JSC_DEFINE_HOST_FUNCTION(throwSuperCallInDirectEvalError, (JSGlobalObject* globalObject, CallFrame*))
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    return throwVMError(globalObject, scope, createSuperCallInDirectEvalError(globalObject));
}

struct EvalScopeChain {
    JSScope* innermost;
    JSValue thisValue;
    PrivateNameEnvironment privateNames;
};

static EvalScopeChain buildScopeChain(Instance* instance, JSGlobalObject* globalObject, VM& vm, const DirectEvalRequest& request, std::span<const EncodedJSValue> operands)
{
    using ScopeType = SymbolTable::ScopeType;
    auto bindings = unsafeMakeSpan(request.bindings, request.numberOfBindings);
    auto* operandCopy = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, operands.size() + bindings.size());
    for (size_t i = 0; i < operands.size(); ++i)
        operandCopy->setIndex(vm, i, JSValue::decode(operands[i]));
    auto read = [&](const EvalBindingData& binding) {
        return readBinding(instance, operandCopy, binding);
    };
    auto readObject = [&](const EvalBindingData& binding) {
        JSValue object = read(binding);
        RELEASE_ASSERT(object.isObject());
        return asObject(object);
    };

    // The bindings of the class follow those of its constructor.
    auto parentOfClass = [&](const EvalBindingData& bindingOfConstructor) {
        const EvalBindingData* classObject = findBinding(bindings.subspan(&bindingOfConstructor - bindings.data()), EvalBindingKind::ClassObject);
        RELEASE_ASSERT(classObject);
        return readObject(*classObject)->getPrototypeDirect().getObject();
    };

    JSObject* homeObjectHolder = nullptr;
    if (const EvalBindingData* homeObject = findBinding(bindings, EvalBindingKind::HomeObject)) {
        const EvalBindingData* thisBinding = request.isInDerivedConstructor ? findBinding(bindings, EvalBindingKind::This) : nullptr;
        if (thisBinding && canBeBoundByText(*thisBinding)) {
            homeObjectHolder = DerivedConstructorStandIn::create(vm, ensureStructure(vm, *instance, &DirectEvalStructures::derivedConstructorStandIn, DerivedConstructorStandIn::createStructure), asObject(operandCopy->get(thisBinding->function)));
            if (JSValue initializer = operandOrEmpty(operandCopy, thisBinding->setter))
                homeObjectHolder->putDirect(vm, vm.propertyNames->builtinNames().instanceFieldInitializerPrivateName(), initializer);
        } else if (request.isInDerivedConstructor)
            homeObjectHolder = constructEmptyObject(globalObject, JSFunction::create(vm, globalObject, 0, String(), throwSuperCallInDirectEvalError, ImplementationVisibility::Private, NoIntrinsic, throwSuperCallInDirectEvalError));
        else if (JSObject* parent = request.allowsSuperCall ? parentOfClass(*homeObject) : nullptr)
            homeObjectHolder = constructEmptyObject(globalObject, parent);
        else
            homeObjectHolder = constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure());
        homeObjectHolder->putDirect(vm, vm.propertyNames->builtinNames().homeObjectPrivateName(), read(*homeObject));
    }
    JSValue newTarget;
    if (request.allowsNewTarget) {
        const EvalBindingData* binding = findBinding(bindings, EvalBindingKind::NewTarget);
        newTarget = binding ? read(*binding) : operandCopy->get(1);
        if (!newTarget)
            newTarget = jsUndefined();
    }

    JSScope* innermost = globalObject->globalScope();
    auto createBindings = [&](std::span<const EvalBindingData> own) {
        return EvalBindings::create(vm, ensureStructure(vm, *instance, &DirectEvalStructures::bindings, EvalBindings::createStructure), instance, request, operandCopy, homeObjectHolder, own);
    };
    auto pushBindings = [&](std::span<const EvalBindingData> run) {
        EvalBindings* own = createBindings(run);
        innermost = EvalScope::create(vm, ensureStructure(vm, *instance, &DirectEvalStructures::scope, EvalScope::createStructure), innermost, own, newTarget);
    };
    auto pushRun = [&](std::span<const EvalBindingData> run) {
        if (!run.empty())
            pushBindings(run);
    };
    auto pushVariableScope = [&](ScopeType scopeType, const EvalVariableScopeContents& contents) {
        if (innermost == globalObject->globalScope())
            pushBindings({ });
        innermost = EvalVariableScope::create(vm, ensureStructure(vm, *instance, &DirectEvalStructures::variableScope, EvalVariableScope::createStructure), innermost, scopeType, contents);
    };

    size_t index = bindings.size();
    size_t runEnd = index;
    while (index) {
        const EvalBindingData& outermost = bindings[index - 1];
        auto run = bindings.subspan(index, runEnd - index);
        if (hasOwnScope(outermost)) {
            pushRun(run);
            if (kindOf(outermost) == EvalBindingKind::WithObject)
                innermost = JSWithScope::create(vm, globalObject, innermost, readObject(outermost));
            else
                pushVariableScope(ScopeType::CatchScopeWithSimpleParameter, { .ownBindings = createBindings(bindings.subspan(index - 1, 1)) });
            runEnd = --index;
            continue;
        }
        size_t groupStart = index - 1;
        while (groupStart && bindings[groupStart - 1].depth == outermost.depth && !hasOwnScope(bindings[groupStart - 1]))
            --groupStart;
        auto group = bindings.subspan(groupStart, index - groupStart);
        if (const EvalBindingData* variables = findBinding(group, EvalBindingKind::EvalVariables)) {
            pushRun(run);
            JSObject* variablesObject = readObject(*variables);
            RELEASE_ASSERT(std::ranges::none_of(group, needsPrivateBrand));
            pushVariableScope(ScopeType::VarScope, { .ownBindings = createBindings(group), .variables = variablesObject });
            runEnd = groupStart;
        } else if (std::ranges::any_of(group, needsPrivateBrand)) {
            pushRun(run);
            EvalVariableScopeContents contents { .ownBindings = createBindings(group) };
            if (const EvalBindingData* classObject = findBinding(group, EvalBindingKind::ClassObject))
                contents.privateClassBrand = read(*classObject);
            auto instanceMember = std::ranges::find_if(group, [](const EvalBindingData& binding) {
                return binding.privateKey && needsPrivateBrand(binding);
            });
            if (instanceMember != group.end())
                contents.privateBrand = privateSymbolAt(instance, instanceMember->privateKey);
            pushVariableScope(ScopeType::LexicalScope, contents);
            runEnd = groupStart;
        }
        index = groupStart;
    }
    pushRun(bindings.first(runEnd));
    if (!request.isStrict && request.isInFunction && !findBinding(bindings, EvalBindingKind::EvalVariables))
        pushVariableScope(ScopeType::VarScope, { .variables = constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure()) });

    const EvalBindingData* thisBinding = findBinding(bindings, EvalBindingKind::This);
    EvalScopeChain chain { innermost, boundThisOrEmpty(thisBinding ? read(*thisBinding) : operandCopy->get(0), request.isInDerivedConstructor), { } };
    for (auto& binding : bindings) {
        if (std::optional<PrivateNameEntry> entry = privateNameEntryOf(binding))
            chain.privateNames.add(RefPtr<UniquedStringImpl> { identifierAt(instance, binding.name).impl() }, *entry);
    }
    return chain;
}

class DirectEvalCaches final : public JSDestructibleObject {
public:
    using Base = JSDestructibleObject;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.destructibleObjectSpace(); }

    static DirectEvalCaches* create(VM&, JSGlobalObject*);
    static void destroy(JSCell* cell) { static_cast<DirectEvalCaches*>(cell)->~DirectEvalCaches(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    DirectEvalCodeCache& ensureCacheOf(uint32_t requestOffset);

private:
    DirectEvalCaches(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    UncheckedKeyHashMap<uint32_t, std::unique_ptr<DirectEvalCodeCache>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_caches;
};

const ClassInfo DirectEvalCaches::s_info = { "DirectEvalCaches"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(DirectEvalCaches) };

DirectEvalCaches* DirectEvalCaches::create(VM& vm, JSGlobalObject* globalObject)
{
    Structure* structure = Structure::create(vm, globalObject, jsNull(), TypeInfo(ObjectType, StructureFlags), info());
    auto* result = new (NotNull, allocateCell<DirectEvalCaches>(vm)) DirectEvalCaches(vm, structure);
    result->finishCreation(vm);
    return result;
}

template<typename Visitor>
void DirectEvalCaches::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<DirectEvalCaches>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    Locker locker { thisObject->cellLock() };
    for (auto& cache : thisObject->m_caches.values())
        cache->visitAggregate(visitor);
}

DEFINE_VISIT_CHILDREN(DirectEvalCaches);

DirectEvalCodeCache& DirectEvalCaches::ensureCacheOf(uint32_t requestOffset)
{
    if (DirectEvalCodeCache* cache = m_caches.get(requestOffset))
        return *cache;
    auto created = makeUniqueWithoutFastMallocCheck<DirectEvalCodeCache>();
    DirectEvalCodeCache& cache = *created;
    Locker locker { cellLock() };
    m_caches.add(requestOffset, WTF::move(created));
    return cache;
}

JSC_DEFINE_JIT_OPERATION(operationAOTDirectEval, EncodedJSValue, (Instance* instance, EncodedJSValue text, uint32_t requestOffset, uint32_t count, const EncodedJSValue* operands))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDirectEval);
    RELEASE_ASSERT(count >= 2);
    JSValue program = JSValue::decode(text);
    if (!program.isString() && !program.isObject())
        OPERATION_RETURN(scope, text);

    const auto& request = requestAt<DirectEvalRequest>(instance, requestOffset);
    EvalScopeChain chain = buildScopeChain(instance, globalObject, vm, request, unsafeMakeSpan(operands, count));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());

    const EvalBindingData* thisBinding = findBinding(unsafeMakeSpan(request.bindings, request.numberOfBindings), EvalBindingKind::This);
    bool hasThisBinding = thisBinding;
    RELEASE_ASSERT(!request.isInDerivedConstructor || hasThisBinding);
    bool initializesInstances = request.isInDerivedConstructor && canBeBoundByText(*thisBinding) && thisBinding->setter != EvalBindingData::noOperand;
    DirectEvalCaches*& caches = instance->state->objects->caches->directEvalCaches;
    if (!caches) {
        caches = DirectEvalCaches::create(vm, instance->globalObject);
        instance->state->immortalCells->add(caches);
    }
    DirectEvalCaller caller {
        .callFrame = std::bit_cast<CallFrame*>(frameRecord),
        .scope = chain.innermost,
        .thisValue = chain.thisValue,
        .sourceOrigin = instance->state->modules->sourceOrigin(ModuleID(request.module)),
        .lexicallyScopedFeatures = request.isStrict ? StrictModeLexicallyScopedFeature : NoLexicallyScopedFeatures,
        .derivedContextType = request.allowsSuperCall ? DerivedContextType::DerivedConstructorContext : request.allowsSuperProperty ? DerivedContextType::DerivedMethodContext : DerivedContextType::None,
        .evalContextType = request.isInFieldInitializer ? EvalContextType::InstanceFieldEvalContext : request.isInFunction ? EvalContextType::FunctionEvalContext : EvalContextType::None,
        .needsClassFieldInitializer = initializesInstances ? NeedsClassFieldInitializer::Yes : NeedsClassFieldInitializer::No,
        .privateBrandRequirement = PrivateBrandRequirement::None,
        .isArrowFunctionContext = hasThisBinding,
        .isInsideOrdinaryFunction = !!request.allowsNewTarget,
        .privateNames = &chain.privateNames,
        .codeBlockWithCache = nullptr,
        .cache = &caches->ensureCacheOf(requestOffset),
        .cacheOwner = caches,
        .callSite = BytecodeIndex::fromBits(requestOffset),
    };
    OPERATION_RETURN(scope, JSValue::encode(evaluateDirectEval(globalObject, program, caller)));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
