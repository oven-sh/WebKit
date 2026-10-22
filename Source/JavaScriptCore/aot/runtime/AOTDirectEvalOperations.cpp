/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTEnvironment.h"
#include "AOTErrorMessages.h"
#include "AOTEvalScope.h"
#include "AOTImmortalCells.h"
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

static JSValue operandOf(JSCellButterfly* operands, const EvalBindingData& binding)
{
    RELEASE_ASSERT(binding.operand < operands->length());
    return operands->get(binding.operand);
}

static JSValue functionOperandOrEmpty(JSCellButterfly* operands, uint32_t operand)
{
    if (operand == EvalBindingData::noOperand)
        return JSValue();
    RELEASE_ASSERT(operand < operands->length());
    return operands->get(operand);
}

static Environment* environmentOf(JSCellButterfly* operands, const EvalBindingData& binding)
{
    JSValue value = operandOf(operands, binding);
    auto* environment = value && value.isCell() ? dynamicDowncast<Environment>(value.asCell()) : nullptr;
    RELEASE_ASSERT(environment);
    return environment;
}

static DirectArguments* mappedArgumentsOf(JSCellButterfly* operands, const EvalBindingData& binding)
{
    JSValue value = operandOf(operands, binding);
    auto* arguments = value && value.isCell() ? dynamicDowncast<DirectArguments>(value.asCell()) : nullptr;
    RELEASE_ASSERT(arguments && DirectArguments::offsetOfSlot(binding.slot.offset) < cellSize(arguments));
    return arguments;
}

static JSValue readBinding(Instance* instance, JSCellButterfly* operands, const EvalBindingData& binding)
{
    switch (formOf(binding)) {
    case EvalBindingForm::ModuleVariable:
        return ProgramStatics::readVariable(*instance, binding.slot);
    case EvalBindingForm::EnvironmentSlot:
        return readSlot(binding.slot.representation(), &environmentOf(operands, binding)->at<uint8_t>(binding.slot.offset), *instance->state->strings);
    case EvalBindingForm::MappedArgument:
        return mappedArgumentsOf(operands, binding)->argument(DirectArgumentsOffset(binding.slot.offset)).get();
    case EvalBindingForm::Operand:
        return operandOf(operands, binding);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue boundThisOrEmpty(JSValue thisValue, bool isInDerivedConstructor)
{
    return isInDerivedConstructor && thisValue.isUndefined() ? JSValue() : thisValue;
}

static const EvalBindingData* findBinding(std::span<const EvalBindingData> bindings, EvalBindingKind kind)
{
    for (auto& binding : bindings) {
        if (kindOf(binding) == kind)
            return &binding;
    }
    return nullptr;
}

static bool isNamedBinding(EvalBindingKind kind)
{
    switch (kind) {
    case EvalBindingKind::Variable:
    case EvalBindingKind::Constant:
    case EvalBindingKind::FunctionName:
    case EvalBindingKind::Arguments:
    case EvalBindingKind::PrivateName:
    case EvalBindingKind::PrivateMethod:
    case EvalBindingKind::PrivateAccessor:
    case EvalBindingKind::MappedArgument:
    case EvalBindingKind::SimpleCatchParameter:
        return true;
    case EvalBindingKind::This:
    case EvalBindingKind::NewTarget:
    case EvalBindingKind::HomeObject:
    case EvalBindingKind::ClassObject:
    case EvalBindingKind::WithObject:
    case EvalBindingKind::EvalVariables:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static unsigned attributesOf(EvalBindingKind kind)
{
    switch (kind) {
    case EvalBindingKind::Variable:
    case EvalBindingKind::Arguments:
    case EvalBindingKind::MappedArgument:
    case EvalBindingKind::SimpleCatchParameter:
        return static_cast<unsigned>(PropertyAttribute::DontDelete);
    case EvalBindingKind::Constant:
    case EvalBindingKind::FunctionName:
    case EvalBindingKind::PrivateName:
    case EvalBindingKind::PrivateMethod:
    case EvalBindingKind::PrivateAccessor:
    case EvalBindingKind::This:
    case EvalBindingKind::HomeObject:
        return PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly;
    case EvalBindingKind::NewTarget:
    case EvalBindingKind::ClassObject:
    case EvalBindingKind::WithObject:
    case EvalBindingKind::EvalVariables:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
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

static bool isNameInquiry(const PropertySlot& slot)
{
    switch (slot.internalMethodType()) {
    case PropertySlot::InternalMethodType::HasProperty:
    case PropertySlot::InternalMethodType::GetOwnProperty:
    case PropertySlot::InternalMethodType::VMInquiry:
        return true;
    case PropertySlot::InternalMethodType::Get:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct EvalBindingContext {
    Instance* instance;
    JSCellButterfly* operands;
    JSObject* homeObjectHolder;
    const DirectEvalRequest* request;
    uint32_t firstPrivateValue;
    bool isInDerivedConstructor;
};

class EvalBindings final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesPut | GetOwnPropertySlotIsImpureForPropertyAbsence;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*);
    static EvalBindings* create(VM&, Structure*, const EvalBindingContext&, std::span<const EvalBindingData>);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);

    const EvalBindingData* find(VM&, PropertyName) const;
    JSValue read(const EvalBindingData&) const;

private:
    EvalBindings(VM&, Structure*, const EvalBindingContext&, std::span<const EvalBindingData>);
    void finishCreation(VM&);

    std::span<const EvalBindingData> bindings() const { return unsafeMakeSpan(m_request->bindings + m_first, m_count); }
    uint32_t privateValueIndexOf(const EvalBindingData&) const;
    JSCell* createPrivateValue(VM&, const EvalBindingData&);
    JSObject* createStaticFieldAccessors(VM&, const EvalBindingData&);
    JSValue readStaticField(JSGlobalObject*, const EvalBindingData&) const;
    void storeStaticField(JSGlobalObject*, const EvalBindingData&, JSValue);
    bool storeVariable(JSGlobalObject*, const EvalBindingData&, PropertyName, JSValue);
    bool tryWriteBinding(VM&, const EvalBindingData&, JSValue);

    Instance* m_instance;
    const DirectEvalRequest* m_request;
    uint32_t m_first;
    uint32_t m_count;
    uint32_t m_firstPrivateValue;
    bool m_isInDerivedConstructor;
    WriteBarrier<JSCellButterfly> m_operands;
    WriteBarrier<JSObject> m_homeObjectHolder;
};

const ClassInfo EvalBindings::s_info = { "EvalBindings"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(EvalBindings) };

EvalBindings::EvalBindings(VM& vm, Structure* structure, const EvalBindingContext& context, std::span<const EvalBindingData> bindings)
    : Base(vm, structure)
    , m_instance(context.instance)
    , m_request(context.request)
    , m_first(bindings.empty() ? 0 : static_cast<uint32_t>(bindings.data() - context.request->bindings))
    , m_count(bindings.size())
    , m_firstPrivateValue(bindings.empty() ? 0 : context.firstPrivateValue + m_first)
    , m_isInDerivedConstructor(context.isInDerivedConstructor)
    , m_operands(context.operands, WriteBarrierEarlyInit)
    , m_homeObjectHolder(context.homeObjectHolder, WriteBarrierEarlyInit)
{
}

void EvalBindings::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    for (auto& binding : bindings()) {
        if (JSCell* value = createPrivateValue(vm, binding))
            m_operands->setIndex(vm, privateValueIndexOf(binding), value);
    }
}

uint32_t EvalBindings::privateValueIndexOf(const EvalBindingData& binding) const
{
    return m_firstPrivateValue + static_cast<uint32_t>(&binding - bindings().data());
}

JSCell* EvalBindings::createPrivateValue(VM& vm, const EvalBindingData& binding)
{
    if (isStaticPrivateField(binding))
        return createStaticFieldAccessors(vm, binding);
    if (kindOf(binding) == EvalBindingKind::PrivateName)
        return privateSymbolAt(m_instance, binding.privateKey);
    if (kindOf(binding) != EvalBindingKind::PrivateAccessor)
        return nullptr;
    JSCellButterfly* operands = m_operands.get();
    return createAccessorPair(vm, realm(), functionOperandOrEmpty(operands, binding.function), functionOperandOrEmpty(operands, binding.setter));
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

Structure* EvalBindings::createStructure(VM& vm, JSGlobalObject* globalObject)
{
    return Structure::create(vm, globalObject, jsNull(), TypeInfo(ObjectType, StructureFlags), info());
}

EvalBindings* EvalBindings::create(VM& vm, Structure* structure, const EvalBindingContext& context, std::span<const EvalBindingData> bindings)
{
    auto* result = new (NotNull, allocateCell<EvalBindings>(vm)) EvalBindings(vm, structure, context, bindings);
    result->finishCreation(vm);
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
        return functionOperandOrEmpty(m_operands.get(), binding.function);
    JSValue value = readBinding(m_instance, m_operands.get(), binding);
    if (kind == EvalBindingKind::This)
        return boundThisOrEmpty(value, m_isInDerivedConstructor);
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
    if (isNameInquiry(slot)) {
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

bool EvalBindings::tryWriteBinding(VM& vm, const EvalBindingData& binding, JSValue value)
{
    ProgramStrings& strings = *m_instance->state->strings;
    SlotRepresentation representation = binding.slot.representation();
    uint32_t offset = binding.slot.offset;
    switch (formOf(binding)) {
    case EvalBindingForm::ModuleVariable: {
        RELEASE_ASSERT(representation != SlotRepresentation::DeferredClosure);
        ProgramStatics& statics = *m_instance->state->statics;
        statics.ensureProgramStaticsInitialized();
        if (!tryWriteSlot(strings, representation, m_instance->statics() + offset, value))
            return false;
        if (traitsOf(representation).holdsCell)
            statics.didStoreRootInStatics(offset);
        return true;
    }
    case EvalBindingForm::EnvironmentSlot: {
        Environment* environment = environmentOf(m_operands.get(), binding);
        if (!tryWriteSlot(strings, representation, &environment->at<uint8_t>(offset), value))
            return false;
        vm.writeBarrier(environment, value);
        return true;
    }
    case EvalBindingForm::MappedArgument: {
        DirectArguments* arguments = mappedArgumentsOf(m_operands.get(), binding);
        arguments->argument(DirectArgumentsOffset(offset)).set(vm, arguments, value);
        return true;
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
    RELEASE_ASSERT(tryWriteBinding(vm, binding, value));
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
    switch (kindOf(*binding)) {
    case EvalBindingKind::Constant:
    case EvalBindingKind::PrivateName:
    case EvalBindingKind::PrivateMethod:
    case EvalBindingKind::PrivateAccessor:
        throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
        return false;
    case EvalBindingKind::FunctionName:
        if (!slot.isStrictMode())
            return true;
        throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
        return false;
    case EvalBindingKind::This:
        throwException(globalObject, scope, createSuperCallInDirectEvalError(globalObject));
        return false;
    case EvalBindingKind::Variable:
    case EvalBindingKind::Arguments:
    case EvalBindingKind::MappedArgument:
    case EvalBindingKind::SimpleCatchParameter:
        RELEASE_AND_RETURN(scope, thisObject->storeVariable(globalObject, *binding, name, value));
    case EvalBindingKind::NewTarget:
    case EvalBindingKind::HomeObject:
    case EvalBindingKind::ClassObject:
    case EvalBindingKind::WithObject:
    case EvalBindingKind::EvalVariables:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
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

    static Structure* createStructure(VM&, JSGlobalObject*);
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

Structure* EvalVariableScope::createStructure(VM& vm, JSGlobalObject* globalObject)
{
    return Structure::create(vm, globalObject, jsNull(), TypeInfo(LexicalEnvironmentType, StructureFlags), info());
}

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

class EvalScopeChainBuilder {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(EvalScopeChainBuilder);
public:
    EvalScopeChainBuilder(JSGlobalObject* globalObject, VM& vm, const EvalBindingContext& context, JSValue newTarget)
        : m_globalObject(globalObject)
        , m_vm(vm)
        , m_context(context)
        , m_newTarget(newTarget)
        , m_scope(globalObject->globalScope())
    {
    }

    JSScope* innermost() const { return m_scope; }

    void addWithScope(const EvalBindingData&);
    void addSimpleCatchParameterScope(std::span<const EvalBindingData> parameter);
    void addBindingsRun(std::span<const EvalBindingData>);
    void addVariableScope(std::span<const EvalBindingData> ownBindings, const EvalBindingData& variables);
    void addEmptyVariableScope();
    void addClassScope(std::span<const EvalBindingData> ownBindings);

private:
    EvalBindings* createBindings(std::span<const EvalBindingData>);
    void pushEvalScope(EvalBindings*);
    void pushVariableScope(SymbolTable::ScopeType, const EvalVariableScopeContents&);

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    EvalBindingContext m_context;
    JSValue m_newTarget;
    JSScope* m_scope;
};

using StructureCreator = Structure* (*)(VM&, JSGlobalObject*);

static Structure* ensureStructure(VM& vm, Instance& instance, Structure* DirectEvalStructures::* member, StructureCreator create)
{
    Structure*& structure = instance.state->objects->caches->directEvalStructures.*member;
    if (!structure) {
        structure = create(vm, instance.globalObject);
        instance.state->immortalCells->add(structure);
    }
    return structure;
}

void EvalScopeChainBuilder::addWithScope(const EvalBindingData& binding)
{
    JSValue object = readBinding(m_context.instance, m_context.operands, binding);
    RELEASE_ASSERT(object.isObject());
    m_scope = JSWithScope::create(m_vm, m_globalObject, m_scope, asObject(object));
}

EvalBindings* EvalScopeChainBuilder::createBindings(std::span<const EvalBindingData> bindings)
{
    Structure* structure = ensureStructure(m_vm, *m_context.instance, &DirectEvalStructures::bindings, EvalBindings::createStructure);
    return EvalBindings::create(m_vm, structure, m_context, bindings);
}

void EvalScopeChainBuilder::pushEvalScope(EvalBindings* bindings)
{
    Structure* structure = ensureStructure(m_vm, *m_context.instance, &DirectEvalStructures::scope, EvalScope::createStructure);
    m_scope = EvalScope::create(m_vm, structure, m_scope, bindings, m_newTarget);
}

void EvalScopeChainBuilder::addBindingsRun(std::span<const EvalBindingData> bindings)
{
    if (!bindings.empty())
        pushEvalScope(createBindings(bindings));
}

void EvalScopeChainBuilder::pushVariableScope(SymbolTable::ScopeType scopeType, const EvalVariableScopeContents& contents)
{
    if (m_scope == m_globalObject->globalScope())
        pushEvalScope(createBindings({ }));
    Structure* structure = ensureStructure(m_vm, *m_context.instance, &DirectEvalStructures::variableScope, EvalVariableScope::createStructure);
    m_scope = EvalVariableScope::create(m_vm, structure, m_scope, scopeType, contents);
}

void EvalScopeChainBuilder::addSimpleCatchParameterScope(std::span<const EvalBindingData> parameter)
{
    pushVariableScope(SymbolTable::ScopeType::CatchScopeWithSimpleParameter, { .ownBindings = createBindings(parameter) });
}

void EvalScopeChainBuilder::addVariableScope(std::span<const EvalBindingData> ownBindings, const EvalBindingData& variables)
{
    JSValue object = readBinding(m_context.instance, m_context.operands, variables);
    RELEASE_ASSERT(object.isObject() && std::ranges::none_of(ownBindings, needsPrivateBrand));
    pushVariableScope(SymbolTable::ScopeType::VarScope, { .ownBindings = createBindings(ownBindings), .variables = asObject(object) });
}

void EvalScopeChainBuilder::addEmptyVariableScope()
{
    pushVariableScope(SymbolTable::ScopeType::VarScope, { .variables = constructEmptyObject(m_vm, m_globalObject->nullPrototypeObjectStructure()) });
}

void EvalScopeChainBuilder::addClassScope(std::span<const EvalBindingData> ownBindings)
{
    EvalVariableScopeContents contents { .ownBindings = createBindings(ownBindings) };
    if (const EvalBindingData* classObject = findBinding(ownBindings, EvalBindingKind::ClassObject))
        contents.privateClassBrand = readBinding(m_context.instance, m_context.operands, *classObject);
    auto instanceMember = std::ranges::find_if(ownBindings, [](const EvalBindingData& binding) {
        return binding.privateKey && needsPrivateBrand(binding);
    });
    if (instanceMember != ownBindings.end())
        contents.privateBrand = privateSymbolAt(m_context.instance, instanceMember->privateKey);
    pushVariableScope(SymbolTable::ScopeType::LexicalScope, contents);
}

static bool hasOwnScope(const EvalBindingData& binding)
{
    return kindOf(binding) == EvalBindingKind::WithObject || kindOf(binding) == EvalBindingKind::SimpleCatchParameter;
}

static void addBindingsToChain(EvalScopeChainBuilder& builder, std::span<const EvalBindingData> bindings)
{
    size_t index = bindings.size();
    size_t runEnd = index;
    while (index) {
        const EvalBindingData& outermost = bindings[index - 1];
        if (hasOwnScope(outermost)) {
            builder.addBindingsRun(bindings.subspan(index, runEnd - index));
            if (kindOf(outermost) == EvalBindingKind::WithObject)
                builder.addWithScope(outermost);
            else
                builder.addSimpleCatchParameterScope(bindings.subspan(index - 1, 1));
            runEnd = --index;
            continue;
        }
        size_t groupStart = index - 1;
        while (groupStart && bindings[groupStart - 1].depth == outermost.depth && !hasOwnScope(bindings[groupStart - 1]))
            --groupStart;
        auto group = bindings.subspan(groupStart, index - groupStart);
        if (const EvalBindingData* variables = findBinding(group, EvalBindingKind::EvalVariables)) {
            builder.addBindingsRun(bindings.subspan(index, runEnd - index));
            builder.addVariableScope(group, *variables);
            runEnd = groupStart;
        } else if (std::ranges::any_of(group, needsPrivateBrand)) {
            builder.addBindingsRun(bindings.subspan(index, runEnd - index));
            builder.addClassScope(group);
            runEnd = groupStart;
        }
        index = groupStart;
    }
    builder.addBindingsRun(bindings.first(runEnd));
}

static JSCellButterfly* copyOperands(VM& vm, std::span<const EncodedJSValue> operands, size_t numberOfPrivateValues)
{
    auto* copy = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, operands.size() + numberOfPrivateValues);
    for (size_t i = 0; i < operands.size(); ++i)
        copy->setIndex(vm, i, JSValue::decode(operands[i]));
    return copy;
}

static JSObject* createHomeObjectHolder(Instance* instance, JSGlobalObject* globalObject, VM& vm, std::span<const EvalBindingData> bindings, JSCellButterfly* operands)
{
    const EvalBindingData* homeObject = findBinding(bindings, EvalBindingKind::HomeObject);
    if (!homeObject)
        return nullptr;
    JSObject* holder = constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure());
    holder->putDirect(vm, vm.propertyNames->builtinNames().homeObjectPrivateName(), readBinding(instance, operands, *homeObject));
    return holder;
}

static JSValue computeNewTarget(Instance* instance, const DirectEvalRequest& request, std::span<const EvalBindingData> bindings, JSCellButterfly* operands)
{
    if (!request.allowsNewTarget)
        return JSValue();
    const EvalBindingData* binding = findBinding(bindings, EvalBindingKind::NewTarget);
    JSValue newTarget = binding ? readBinding(instance, operands, *binding) : operands->get(1);
    return newTarget ? newTarget : jsUndefined();
}

struct EvalScopeChain {
    JSScope* innermost;
    JSValue thisValue;
    PrivateNameEnvironment privateNames;
};

static EvalScopeChain buildScopeChain(Instance* instance, JSGlobalObject* globalObject, VM& vm, const DirectEvalRequest& request, std::span<const EncodedJSValue> operands)
{
    auto bindings = unsafeMakeSpan(request.bindings, request.numberOfBindings);
    JSCellButterfly* operandCopy = copyOperands(vm, operands, bindings.size());
    JSObject* holder = createHomeObjectHolder(instance, globalObject, vm, bindings, operandCopy);
    EvalBindingContext context { instance, operandCopy, holder, &request, static_cast<uint32_t>(operands.size()), !!request.isInDerivedConstructor };
    EvalScopeChainBuilder builder(globalObject, vm, context, computeNewTarget(instance, request, bindings, operandCopy));
    addBindingsToChain(builder, bindings);
    if (!request.isStrict && request.isInFunction && !findBinding(bindings, EvalBindingKind::EvalVariables))
        builder.addEmptyVariableScope();

    const EvalBindingData* thisBinding = findBinding(bindings, EvalBindingKind::This);
    JSValue thisValue = thisBinding ? readBinding(instance, operandCopy, *thisBinding) : operandCopy->get(0);
    EvalScopeChain chain { builder.innermost(), boundThisOrEmpty(thisValue, context.isInDerivedConstructor), { } };
    for (auto& binding : bindings) {
        if (std::optional<PrivateNameEntry> entry = privateNameEntryOf(binding))
            chain.privateNames.add(RefPtr<UniquedStringImpl> { identifierAt(instance, binding.name).impl() }, *entry);
    }
    return chain;
}

static DerivedContextType derivedContextTypeOf(const DirectEvalRequest& request)
{
    if (request.isInDerivedConstructor)
        return DerivedContextType::DerivedConstructorContext;
    return request.allowsSuperProperty ? DerivedContextType::DerivedMethodContext : DerivedContextType::None;
}

static EvalContextType evalContextTypeOf(const DirectEvalRequest& request)
{
    if (request.isInFieldInitializer)
        return EvalContextType::InstanceFieldEvalContext;
    return request.isInFunction ? EvalContextType::FunctionEvalContext : EvalContextType::None;
}

class DirectEvalCaches final : public JSDestructibleObject {
public:
    using Base = JSDestructibleObject;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.destructibleObjectSpace(); }

    static DirectEvalCaches* create(VM&, JSGlobalObject*);
    static void destroy(JSCell*);

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

void DirectEvalCaches::destroy(JSCell* cell)
{
    static_cast<DirectEvalCaches*>(cell)->~DirectEvalCaches();
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

static DirectEvalCaches& ensureCodeCaches(VM& vm, Instance& instance)
{
    DirectEvalCaches*& caches = instance.state->objects->caches->directEvalCaches;
    if (!caches) {
        caches = DirectEvalCaches::create(vm, instance.globalObject);
        instance.state->immortalCells->add(caches);
    }
    return *caches;
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

    bool hasThisBinding = findBinding(unsafeMakeSpan(request.bindings, request.numberOfBindings), EvalBindingKind::This);
    RELEASE_ASSERT(!request.isInDerivedConstructor || hasThisBinding);
    DirectEvalCaches& caches = ensureCodeCaches(vm, *instance);
    DirectEvalCaller caller {
        .callFrame = std::bit_cast<CallFrame*>(frameRecord),
        .scope = chain.innermost,
        .thisValue = chain.thisValue,
        .sourceOrigin = instance->state->modules->sourceOrigin(ModuleID(request.module)),
        .lexicallyScopedFeatures = request.isStrict ? StrictModeLexicallyScopedFeature : NoLexicallyScopedFeatures,
        .derivedContextType = derivedContextTypeOf(request),
        .evalContextType = evalContextTypeOf(request),
        .needsClassFieldInitializer = NeedsClassFieldInitializer::No,
        .privateBrandRequirement = PrivateBrandRequirement::None,
        .isArrowFunctionContext = hasThisBinding,
        .isInsideOrdinaryFunction = !!request.allowsNewTarget,
        .privateNames = &chain.privateNames,
        .codeBlockWithCache = nullptr,
        .cache = &caches.ensureCacheOf(requestOffset),
        .cacheOwner = &caches,
        .callSite = BytecodeIndex::fromBits(requestOffset),
    };
    OPERATION_RETURN(scope, JSValue::encode(evaluateDirectEval(globalObject, program, caller)));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
