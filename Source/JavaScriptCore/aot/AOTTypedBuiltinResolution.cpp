/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static bool accepts(ArgumentConversion conversion, const Type& argument)
{
    switch (conversion) {
    case ArgumentConversion::None:
        return true;
    case ArgumentConversion::RequiresNumber:
        return argument.isNumber();
    case ArgumentConversion::RequiresInteger:
        return argument.isInteger();
    case ArgumentConversion::RequiresTypedArray:
        return argument.singleEngineClass() && argument.singleEngineClass()->typedArrayType != NotTypedArray;
    case ArgumentConversion::ToNumber:
        return !argument.hasObjects() && !(argument.primitives & (Primitive::Symbol | Primitive::BigInt));
    case ArgumentConversion::ToString:
        return !argument.hasObjects() && !(argument.primitives & Primitive::Symbol);
    case ArgumentConversion::ToPropertyKey:
        return !argument.hasObjects();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinID applicableEntry(BuiltinID first, std::span<const Type> arguments)
{
    auto& builtins = Builtins::singleton();
    for (BuiltinID id = first; id; id = builtins.next(id)) {
        auto& builtin = builtins.at(id);
        if (!builtin.takes(arguments.size()))
            continue;
        bool applies = true;
        for (unsigned i = 0; i < arguments.size() && applies; ++i) {
            auto& parameter = builtin.parameters[std::min<size_t>(i, builtin.parameters.size() - 1)];
            if (parameter.mode == ParameterMode::Optional) {
                Type given = arguments[i].withoutPrimitives(Primitive::Undefined);
                applies = given.isNever() || accepts(parameter.conversion, given);
            } else
                applies = accepts(parameter.conversion, arguments[i]);
        }
        if (applies)
            return id;
    }
    return { };
}

Type elementTypeOf(TypedArrayType type)
{
    switch (type) {
    case TypeInt8:
        return Type::integer({ -128, 127 });
    case TypeUint8:
    case TypeUint8Clamped:
        return Type::integer({ 0, 255 });
    case TypeInt16:
        return Type::integer({ -32768, 32767 });
    case TypeUint16:
        return Type::integer({ 0, 65535 });
    case TypeInt32:
        return Type::integer(Range::int32());
    case TypeUint32:
        return Type::integer(Range::uint32());
    case TypeFloat16:
    case TypeFloat32:
    case TypeFloat64:
        return Type::fromPrimitives(Primitive::Number);
    case TypeBigInt64:
    case TypeBigUint64:
        return Type::fromPrimitives(Primitive::BigInt);
    case NotTypedArray:
    case TypeDataView:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

KeyedAccess keyedAccessOf(const Program& program, const Function& function, const Value& value)
{
    auto isArray = [&](ValueID id) {
        auto* engineClass = program.typeOf(function, id).singleEngineClass();
        return engineClass && engineClass->type == ArrayType;
    };
    auto isTypedArray = [&](ValueID id) {
        auto* engineClass = program.typeOf(function, id).singleEngineClass();
        if (!engineClass)
            return false;
        switch (engineClass->typedArrayType) {
        case TypeInt8:
        case TypeUint8:
        case TypeUint8Clamped:
        case TypeInt16:
        case TypeUint16:
        case TypeInt32:
        case TypeUint32:
        case TypeFloat32:
        case TypeFloat64:
            return true;
        default:
            return false;
        }
    };
    auto accessForIterable = [&](ValueID iterable) {
        return isArray(iterable) ? KeyedAccess::Array : isTypedArray(iterable) ? KeyedAccess::TypedArray : KeyedAccess::Generic;
    };
    switch (value.opcode) {
    case Opcode::IteratorOpen:
        return accessForIterable(value.child(0));
    case Opcode::IteratorHasNext:
    case Opcode::IteratorValue:
    case Opcode::IteratorClose: {
        auto& iterator = function.at(value.child(0));
        return iterator.opcode == Opcode::IteratorOpen ? accessForIterable(iterator.child(0)) : KeyedAccess::Generic;
    }
    case Opcode::GetByVal:
    case Opcode::PutByVal:
        break;
    default:
        return KeyedAccess::Generic;
    }
    auto& key = program.typeOf(function, value.child(1));
    auto& receiver = program.typeOf(function, value.child(0));
    if (!key.isNumber()) {
        auto* engineClass = receiver.singleEngineClass();
        bool isNumberOrNullish = key.primitives & Primitive::Number && key.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
        return value.opcode == Opcode::GetByVal && isNumberOrNullish && engineClass && engineClass->type == ArrayType ? KeyedAccess::Array : KeyedAccess::Generic;
    }
    if (auto* engineClass = receiver.singleEngineClass()) {
        if (engineClass->type == ArrayType)
            return KeyedAccess::Array;
        switch (engineClass->typedArrayType) {
        case TypeInt8:
        case TypeUint8:
        case TypeInt16:
        case TypeUint16:
        case TypeInt32:
        case TypeUint32:
        case TypeFloat32:
        case TypeFloat64:
            return value.opcode == Opcode::GetByVal || program.typeOf(function, value.child(2)).isNumber() ? KeyedAccess::TypedArray : KeyedAccess::Generic;
        case TypeUint8Clamped:
            return value.opcode == Opcode::GetByVal ? KeyedAccess::TypedArray : KeyedAccess::Generic;
        default:
            return KeyedAccess::Generic;
        }
    }
    bool isString = receiver.primitives & Primitive::String && receiver.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
    return isString && value.opcode == Opcode::GetByVal ? KeyedAccess::String : KeyedAccess::Generic;
}

static bool fieldGuaranteesType(const Program& program, const Layout& layout, const Field& field, const Type& type)
{
    if (!field.stored)
        return !isObjectLayout(layout.kind) && field.type && program.at(field.type).isSubsetOf(type, program);
    auto& stored = program.at(field.stored);
    if (!stored.withoutPrimitives(Primitive::Absent).isSubsetOf(type, program))
        return false;
    bool mayBeAbsent = isObjectLayout(layout.kind) && (stored.primitives & Primitive::Absent || field.isMissingInSomeSubtype);
    return !mayBeAbsent || type.primitives & Primitive::Undefined;
}

static bool builtinTableGuaranteesType(const Program& program, const Function& function, const Value& value, const Type& type)
{
    auto& builtins = Builtins::singleton();
    auto& result = builtins.at(value.builtin()).result;
    if (result.isAnything())
        return false;
    static constexpr std::pair<uint16_t, uint16_t> primitives[] = {
        { BuiltinResult::Undefined, Primitive::Undefined },
        { BuiltinResult::Null, Primitive::Null },
        { BuiltinResult::Boolean, Primitive::Boolean },
        { BuiltinResult::Number, Primitive::Number },
        { BuiltinResult::String, Primitive::String },
        { BuiltinResult::Symbol, Primitive::Symbol },
        { BuiltinResult::BigInt, Primitive::BigInt },
    };
    uint16_t bits = 0;
    for (auto [kind, primitive] : primitives) {
        if (result.kinds & kind)
            bits |= primitive;
    }
    if (!Type::fromPrimitives(bits).isSubsetOf(type, program))
        return false;
    if (result.kinds & BuiltinResult::Integer && !Type::integer({ result.minimum, result.maximum }).isSubsetOf(type, program))
        return false;
    if (result.kinds & BuiltinResult::Instance && !Type::engineClass(builtins.classes()[result.engineClass]).isSubsetOf(type, program))
        return false;
    if (result.kinds & BuiltinResult::InstanceOfSameClass && !Type::engineClass(builtins.classes()[value.engineClass()]).isSubsetOf(type, program))
        return false;
    if (result.kinds & BuiltinResult::This && !program.typeOf(function, value.child(0)).withoutPrimitives(Primitive::UndefinedOrNull).isSubsetOf(type, program))
        return false;
    for (unsigned i = 0; i < 8; ++i) {
        if (!(result.arguments & 1 << i))
            continue;
        auto& argument = i < value.arguments.size() ? program.typeOf(function, value.arguments[i]) : program.at(program.types.undefined);
        if (!argument.isSubsetOf(type, program))
            return false;
    }
    return true;
}

bool guaranteesResultType(const Program& program, const Function& function, const Value& value)
{
    auto& type = program.at(value.type);
    if (type.isAnything())
        return true;
    switch (value.opcode) {
    case Opcode::Parameter:
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::Boolean:
    case Opcode::Integer:
    case Opcode::Double:
    case Opcode::String:
    case Opcode::BigInt:
    case Opcode::ConstantSymbol:
    case Opcode::FunctionReference:
    case Opcode::PutModuleVariable:
    case Opcode::GetStaticField:
    case Opcode::PutStaticField:
    case Opcode::PutGlobal:
    case Opcode::NewObjectWithShape:
    case Opcode::PutField:
    case Opcode::NewClosure:
    case Opcode::NewClosureOnce:
    case Opcode::PutById:
    case Opcode::InById:
    case Opcode::DeleteById:
    case Opcode::PutByVal:
    case Opcode::InByVal:
    case Opcode::DeleteByVal:
    case Opcode::NewArray:
    case Opcode::NewObject:
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
    case Opcode::Negate:
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
    case Opcode::BitNot:
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
    case Opcode::StrictEq:
    case Opcode::Eq:
    case Opcode::LogicalNot:
    case Opcode::ToBoolean:
    case Opcode::TypeOf:
    case Opcode::InstanceOf:
    case Opcode::ToString:
    case Opcode::IsUndefinedOrNull:
    case Opcode::IsInstanceOfClass:
    case Opcode::Strcat:
    case Opcode::IteratorHasNext:
    case Opcode::IteratorClose:
    case Opcode::LoopHint:
    case Opcode::UnlikelyHint:
    case Opcode::GetResumeMode:
    case Opcode::GetCapture:
    case Opcode::CreateRest:
    case Opcode::GetRestLength:
    case Opcode::CreateThis:
    case Opcode::HasShapeOf:
    case Opcode::IsPlainRecord:
    case Opcode::GetPlainRecordField:
    case Opcode::IsClassObjectUnmodified:
    case Opcode::IsClosureOf:
    case Opcode::IsBuiltinFunction:
    case Opcode::BuiltinFunction:
    case Opcode::TypeOfIs:
    case Opcode::AssertNotAllocated:
    case Opcode::MaterializeEnvironment:
    case Opcode::Cast:
        return true;

    case Opcode::GetModuleVariable: {
        TypeID held = program.variables[value.variable().index()].type;
        return held && program.at(held).isSubsetOf(type, program);
    }
    case Opcode::GetField: {
        auto& layout = program.at(value.layout());
        return fieldGuaranteesType(program, layout, layout.fields[value.field()], type);
    }
    case Opcode::GetById: {
        auto& receiver = program.typeOf(function, value.child(0));
        if (receiver.acceptsUntypedObjects(program))
            return false;
        if (auto access = fieldOf(program, receiver, value.string())) {
            auto& layout = program.at(access->layout);
            return fieldGuaranteesType(program, layout, layout.fields[access->field], type);
        }
        LayoutID supertype = receiver.singleLayout();
        return supertype && findMethod(program, supertype, value.string(), MethodKind::Method) && Type::function().isSubsetOf(type, program);
    }
    case Opcode::CallDirect: {
        TypeID returnedValue = program.at(value.function()).resultType;
        return returnedValue && program.at(returnedValue).isSubsetOf(type, program);
    }
    case Opcode::CallBuiltin:
        return builtinTableGuaranteesType(program, function, value, type);
    case Opcode::Construct:
    case Opcode::ConstructSuper:
    case Opcode::ConstructSuperForwardVarargs:
        return Type::object().isSubsetOf(type, program);

    case Opcode::GetGlobal:
    case Opcode::GetGlobalForTypeOf:
    case Opcode::GetByVal:
    case Opcode::ToPropertyKey:
    case Opcode::IteratorOpen:
    case Opcode::IteratorValue:
    case Opcode::Call:
    case Opcode::CallMethod:
    case Opcode::CatchValue:
    case Opcode::Operation:
    case Opcode::Await:
    case Opcode::GetExternalModule:
    case Opcode::Yield:
    case Opcode::GetArgument:
    case Opcode::GetMyArgumentByVal:
    case Opcode::CallForwardVarargs:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool hasGenericFallback(const Program& program, const Function& function, const Value& value)
{
    auto& builtin = Builtins::singleton().at(value.builtin());
    if (!builtin.hasReceiver() || !program.typeOf(function, value.child(0)).hasObjects())
        return false;
    if (value.methodGetById() || builtin.implementation.kind != BuiltinImplementation::Inline || !isExactForEveryReceiver(builtin.implementation.inlined))
        return true;
    Vector<Type, 4> arguments;
    for (ValueID argument : value.arguments)
        arguments.append(program.typeOf(function, argument));
    return !writesOfInlineBuiltin(builtin.implementation.inlined, arguments.span());
}

namespace {

class BuiltinResolution {
public:
    BuiltinResolution(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
        , m_builtins(Builtins::singleton())
    {
    }

    void run()
    {
        countUses();
        retypePrototypeReads();
        retypeClaimedElements();
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values)
                resolve(m_function.at(id));
        }
        foldMethodReads();
        countUses();
        foldLiteralJoins();
        removeUnusedBuiltinReads();
        markMethodReads();
    }

private:
    using Types = Vector<Type, 4>;

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }
    BuiltinName nameOf(const Value& value) const { return m_program.builtinName(value.string()); }

    Types typesOf(std::span<const ValueID> values) const
    {
        Types types;
        for (ValueID id : values)
            types.append(typeOf(id));
        return types;
    }

    const Value& definitionOf(ValueID id) const
    {
        return m_function.at(m_program.originOf(m_function, id));
    }

    BuiltinName globalNameOf(ValueID id) const
    {
        auto& value = definitionOf(id);
        if (value.opcode != Opcode::GetGlobal)
            return { };
        BuiltinName name = nameOf(value);
        return m_builtins.isGlobal(name) ? name : BuiltinName();
    }

    const EngineClass* classOfPrototypeRead(ValueID id) const
    {
        auto& value = definitionOf(id);
        if (value.opcode != Opcode::GetById || nameOf(value) != m_builtins.prototypeName())
            return nullptr;
        return m_builtins.classWithFixedPrototypeNamed(globalNameOf(value.child(0)));
    }

    const EngineClass* classOfSingleUseEmptyLiteral(ValueID id) const
    {
        ValueID origin = m_program.originOf(m_function, id);
        auto& value = m_function.at(origin);
        if (m_uses[origin.index()] != 1 || !value.arguments.isEmpty())
            return nullptr;
        if (value.opcode == Opcode::NewObject)
            return &m_builtins.objectClass();
        return value.opcode == Opcode::NewArray ? &m_builtins.arrayClass() : nullptr;
    }

    const EngineClass* classOfMethodHolder(ValueID id) const
    {
        const EngineClass* holder = classOfPrototypeRead(id);
        return holder ? holder : classOfSingleUseEmptyLiteral(id);
    }

    bool isObjectHasOwnProperty(BuiltinName holder, BuiltinName name) const
    {
        return holder == m_builtins.objectClass().name && m_builtins.hasOwnProperty(name);
    }

    bool isBuiltinTableFunction(ValueID id) const
    {
        auto& value = definitionOf(id);
        if (value.opcode == Opcode::BuiltinFunction)
            return true;
        if (value.opcode != Opcode::GetById)
            return false;
        if (BuiltinName holder = globalNameOf(value.child(0)))
            return m_builtins.findStatic(BuiltinKind::StaticFunction, holder, nameOf(value)) || isObjectHasOwnProperty(holder, nameOf(value));
        const EngineClass* holder = classOfMethodHolder(value.child(0));
        return holder && m_builtins.findForInstance(BuiltinKind::Method, *holder, nameOf(value));
    }

    const EngineClass* classOf(const Type& type) const
    {
        if (auto* engineClass = type.singleEngineClass())
            return engineClass;
        if (type.hasObjects())
            return nullptr;
        auto isOnly = [&](uint16_t kind, uint16_t besides) {
            return type.primitives & kind && !(type.primitives & ~(kind | besides));
        };
        if (isOnly(Primitive::String, Primitive::UndefinedOrNull))
            return &m_builtins.stringClass();
        if (isOnly(Primitive::Symbol, Primitive::UndefinedOrNull))
            return &m_builtins.symbolClass();
        if (isOnly(Primitive::BigInt, Primitive::UndefinedOrNull))
            return &m_builtins.bigIntClass();
        if (isOnly(Primitive::Number, 0))
            return &m_builtins.numberClass();
        if (isOnly(Primitive::Boolean, 0))
            return &m_builtins.booleanClass();
        return nullptr;
    }

    bool returnsPrototypeArgument(const Value& value) const
    {
        BuiltinID first;
        if (value.opcode == Opcode::CallMethod)
            first = m_builtins.findStatic(BuiltinKind::StaticFunction, globalNameOf(value.child(0)), nameOf(value));
        else if (value.opcode == Opcode::Call) {
            auto& read = m_function.at(value.child(0));
            if (read.opcode == Opcode::GetById && &definitionOf(read.child(0)) == &definitionOf(value.child(1)))
                first = m_builtins.findStatic(BuiltinKind::StaticFunction, globalNameOf(read.child(0)), nameOf(read));
        }
        BuiltinID entry = applicableEntry(first, typesOf(value.arguments.span()).span());
        if (!entry)
            return false;
        auto& result = m_builtins.at(entry).result;
        if (result.kinds || !hasOneBitSet(result.arguments))
            return false;
        unsigned argument = WTF::ctz(static_cast<unsigned>(result.arguments));
        return argument < value.arguments.size() && classOfPrototypeRead(value.arguments[argument]);
    }

    void retypeClaimedElements()
    {
        if (Options::auditAOTTypedAssertions())
            return;
        for (auto& block : m_function.blocks) {
            for (size_t index = 0; index + 1 < block.values.size(); ++index) {
                ValueID id = block.values[index];
                auto& read = m_function.at(id);
                auto& cast = m_function.at(block.values[index + 1]);
                if (read.opcode != Opcode::IteratorValue || keyedAccessOf(m_program, m_function, read) != KeyedAccess::Array || m_uses[id.index()] != 1)
                    continue;
                if (cast.opcode != Opcode::Cast || cast.child(0) != id || cast.castReason() != CastReason::Claim || cast.string() || cast.accessedName())
                    continue;
                auto& claimed = m_program.at(cast.type);
                if (claimed.hasObjects() || !claimed.isSubsetOf(m_program.at(read.type), m_program))
                    continue;
                read.type = cast.type;
                read.rep = repOf(claimed);
                cast.setCastReason(CastReason::Upcast);
            }
        }
    }

    void retypePrototypeReads()
    {
        Vector<ValueID, 4> reads;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                if (typeOf(id).engineClasses.isEmpty())
                    continue;
                auto& value = m_function.at(id);
                if ((value.opcode == Opcode::GetById && classOfPrototypeRead(id)) || returnsPrototypeArgument(value))
                    reads.append(id);
            }
        }
        if (reads.isEmpty())
            return;
        Vector<unsigned, 4> untypedUses;
        untypedUses.fill(0, reads.size());
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                const Value& user = m_function.at(id);
                switch (user.opcode) {
                case Opcode::Cast:
                    if (user.castReason() == CastReason::Upcast)
                        break;
                    [[fallthrough]];
                case Opcode::GetById:
                case Opcode::InById:
                case Opcode::DeleteById:
                case Opcode::GetByVal:
                case Opcode::InByVal:
                case Opcode::DeleteByVal:
                case Opcode::CallMethod:
                case Opcode::Call:
                case Opcode::Construct:
                case Opcode::Operation:
                case Opcode::StrictEq:
                case Opcode::Eq:
                case Opcode::InstanceOf:
                case Opcode::TypeOf:
                case Opcode::TypeOfIs:
                case Opcode::IsBuiltinFunction:
                case Opcode::ToBoolean:
                case Opcode::IsUndefinedOrNull:
                    user.forEachChild([&](ValueID child) {
                        if (size_t at = reads.find(child); at != notFound)
                            ++untypedUses[at];
                    });
                    break;
                default:
                    break;
                }
            }
        }
        for (unsigned i = 0; i < reads.size(); ++i) {
            if (untypedUses[i] != m_uses[reads[i].index()])
                continue;
            m_function.at(reads[i]).type = m_program.types.anything;
            m_function.at(reads[i]).rep = Rep::JSValue;
        }
    }

    bool isOnlyComparedOrBitTested(ValueID id) const
    {
        for (auto& block : m_function.blocks) {
            for (ValueID userID : block.values) {
                auto& user = m_function.at(userID);
                bool uses = false;
                user.forEachChild([&](ValueID child) {
                    uses |= child == id;
                });
                if (!uses)
                    continue;
                switch (user.opcode) {
                case Opcode::BitAnd:
                case Opcode::BitOr:
                case Opcode::BitXor:
                case Opcode::LShift:
                case Opcode::RShift:
                case Opcode::URShift:
                case Opcode::Less:
                case Opcode::LessEq:
                case Opcode::Greater:
                case Opcode::GreaterEq:
                case Opcode::StrictEq:
                    break;
                default:
                    return false;
                }
            }
            bool isForwarded = false;
            const_cast<Terminator&>(block.terminator).forEachChild([&](ValueID& child) {
                isForwarded |= child == id;
            });
            if (isForwarded)
                return false;
        }
        return true;
    }

    void become(Value& value, BuiltinID entry, const EngineClass* engineClass, ValueID receiver, Arguments&& arguments, ValueID methodGetById = { })
    {
        auto& builtin = m_builtins.at(entry);
        if (builtin.implementation.kind == BuiltinImplementation::Number && !receiver) {
            if (!m_program.at(m_program.types.number).isSubsetOf(m_program.at(value.type), m_program))
                return;
            value.opcode = Opcode::Double;
            value.type = m_program.types.number;
            value.rep = Rep::Double;
            value.children = { };
            value.arguments.clear();
            value.setBits(std::bit_cast<uint64_t>(m_builtins.numberOf(entry)));
            return;
        }
        value.opcode = Opcode::CallBuiltin;
        value.children = { receiver, methodGetById, ValueID() };
        value.immediates = { entry.index(), engineClass ? engineClass->index : Format::none };
        value.arguments = WTF::move(arguments);

        TypeID returnedValue = m_program.builtinIntegerResultTypes[entry.index()];
        if (!returnedValue && builtin.result.kinds == BuiltinResult::Boolean && !builtin.result.arguments)
            returnedValue = m_program.types.boolean;
        if (!returnedValue && builtin.implementation.kind == BuiltinImplementation::Inline && builtin.implementation.inlined == InlineBuiltin::StringCharCodeAt && isOnlyComparedOrBitTested(ValueID(static_cast<uint32_t>(&value - m_function.values.span().data()))))
            returnedValue = m_program.types.number;
        if (!returnedValue || !m_program.at(returnedValue).isSubsetOf(m_program.at(value.type), m_program) || m_program.at(value.type).isSubsetOf(m_program.at(returnedValue), m_program))
            return;
        if (builtin.hasReceiver() && typeOf(receiver).hasObjects() && (builtin.kind != BuiltinKind::Getter || hasGenericFallback(m_program, m_function, value)))
            return;
        value.type = returnedValue;
        value.rep = repOf(m_program.at(returnedValue));
    }

    bool isResolvable(BuiltinID entry) const
    {
        return entry && m_builtins.at(entry).implementation.kind != BuiltinImplementation::Generic;
    }

    const EngineClass* receiverClass(const Type& type) const
    {
        const EngineClass* engineClass = classOf(type);
        if (!engineClass || (type.hasObjects() && !engineClass->canCheckOriginalStructure()))
            return nullptr;
        return engineClass;
    }

    bool resolveForReceiver(Value& value, BuiltinKind kind, ValueID receiver, BuiltinName name, Arguments&& arguments, ValueID methodGetById = { })
    {
        const EngineClass* engineClass = receiverClass(typeOf(receiver));
        if (!engineClass && !typeOf(receiver).hasObjects() && typeOf(receiver).mayBeUndefinedOrNull())
            engineClass = classOf(typeOf(receiver).withoutPrimitives(Primitive::UndefinedOrNull));
        if (!engineClass)
            return false;
        BuiltinID entry = applicableEntry(m_builtins.findForInstance(kind, *engineClass, name), typesOf(arguments.span()).span());
        if (!isResolvable(entry))
            return false;
        become(value, entry, engineClass, receiver, WTF::move(arguments), methodGetById);
        return true;
    }

    bool resolveForHolder(Value& value, BuiltinName holder, BuiltinName name)
    {
        BuiltinID entry = applicableEntry(m_builtins.findStatic(BuiltinKind::StaticFunction, holder, name), typesOf(value.arguments.span()).span());
        if (!isResolvable(entry))
            return false;
        become(value, entry, m_builtins.classNamed(holder), { }, Arguments(value.arguments));
        return true;
    }

    bool resolveCallOfGetById(Value& value)
    {
        ValueID readID = value.child(0);
        auto& read = m_function.at(readID);
        if (read.opcode != Opcode::GetById || &definitionOf(read.child(0)) != &definitionOf(value.child(1)))
            return false;
        if (nameOf(read) == m_builtins.callName() && isBuiltinTableFunction(read.child(0)) && !value.arguments.isEmpty()) {
            if (!resolvePrototypeMethodCall(value, read.child(0))) {
                value.children = { read.child(0), value.arguments[0], ValueID() };
                value.arguments.removeAt(0);
            }
            return true;
        }
        if (BuiltinName holder = globalNameOf(read.child(0)))
            return resolveForHolder(value, holder, nameOf(read));
        ValueID receiver = read.child(0);
        if (typeOf(receiver).hasObjects())
            return resolveForReceiver(value, BuiltinKind::Method, receiver, nameOf(read), Arguments(value.arguments), readID);
        if (!resolveForReceiver(value, BuiltinKind::Method, receiver, nameOf(read), Arguments(value.arguments)))
            return false;
        if (m_uses[readID.index()] == 1 && typeOf(receiver).mayBeUndefinedOrNull()) {
            read.opcode = Opcode::Cast;
            read.type = m_function.at(receiver).type;
            read.rep = m_function.at(receiver).rep;
            read.immediates[1] = static_cast<uint32_t>(CastReason::Flow);
        }
        return true;
    }

    bool resolvePrototypeMethodCall(Value& value, ValueID function)
    {
        if (value.arguments.isEmpty())
            return false;
        auto& method = definitionOf(function);
        if (method.opcode != Opcode::GetById)
            return false;
        const EngineClass* holder = classOfMethodHolder(method.child(0));
        if (isObjectHasOwnProperty(globalNameOf(method.child(0)), nameOf(method)))
            holder = &m_builtins.objectClass();
        if (BuiltinID found = m_builtins.hasOwnProperty(nameOf(method)); found && holder && m_builtins.findForInstance(BuiltinKind::Method, *holder, nameOf(method)) == found) {
            BuiltinID entry = applicableEntry(m_builtins.hasOwn(), typesOf(value.arguments.span()).span());
            if (!isResolvable(entry))
                return false;
            become(value, entry, holder, { }, Arguments(value.arguments));
            return true;
        }
        if (BuiltinID found = m_builtins.propertyIsEnumerable(nameOf(method)); found && holder && value.arguments.size() == 2 && m_builtins.findForInstance(BuiltinKind::Method, *holder, nameOf(method)) == found) {
            become(value, m_builtins.propertyIsEnumerableOperation(), nullptr, { }, Arguments(value.arguments));
            return true;
        }
        const EngineClass* engineClass = classOf(typeOf(value.arguments[0]));
        if (!holder || !engineClass || typeOf(value.arguments[0]).hasObjects())
            return false;
        Arguments arguments;
        arguments.append(value.arguments.span().subspan(1));
        BuiltinID entry = applicableEntry(m_builtins.findForInstance(BuiltinKind::Method, *holder, nameOf(method)), typesOf(arguments.span()).span());
        if (!isResolvable(entry))
            return false;
        BuiltinName entryHolder = m_builtins.at(entry).holderName;
        if (entryHolder != engineClass->name && entryHolder != engineClass->listedAs)
            return false;
        become(value, entry, engineClass, value.arguments[0], WTF::move(arguments));
        return true;
    }

    void resolve(Value& value)
    {
        switch (value.opcode) {
        case Opcode::GetById:
            if (BuiltinName holder = globalNameOf(value.child(0))) {
                if (BuiltinID entry = m_builtins.findStatic(BuiltinKind::Constant, holder, nameOf(value)))
                    become(value, entry, nullptr, { }, { });
                return;
            }
            resolveForReceiver(value, BuiltinKind::Getter, value.child(0), nameOf(value), { });
            return;
        case Opcode::PutById:
            resolveForReceiver(value, BuiltinKind::Setter, value.child(0), nameOf(value), { value.child(1) });
            return;
        case Opcode::IteratorValue:
            if (keyedAccessOf(m_program, m_function, value) == KeyedAccess::TypedArray) {
                Type element = elementTypeOf(typeOf(m_function.at(value.child(0)).child(0)).singleEngineClass()->typedArrayType);
                TypeID narrowed = element.isSubsetOf(m_program.at(m_program.types.int32), m_program) ? m_program.types.int32 : m_program.types.number;
                if (m_program.at(narrowed).isSubsetOf(m_program.at(value.type), m_program)) {
                    value.type = narrowed;
                    value.rep = repOf(m_program.at(narrowed));
                }
            }
            return;
        case Opcode::CallMethod:
            if (BuiltinName holder = globalNameOf(value.child(0))) {
                resolveForHolder(value, holder, nameOf(value));
                return;
            }
            if (nameOf(value) == m_builtins.callName() && isBuiltinTableFunction(value.child(0)) && !value.arguments.isEmpty()) {
                if (!resolvePrototypeMethodCall(value, value.child(0))) {
                    value.opcode = Opcode::Call;
                    value.children = { value.child(0), value.arguments[0], ValueID() };
                    value.immediates[0] = Format::none;
                    value.setCallSignature(Format::none);
                    value.arguments.removeAt(0);
                }
                return;
            }
            resolveForReceiver(value, BuiltinKind::Method, value.child(0), nameOf(value), Arguments(value.arguments), typeOf(value.child(0)).hasObjects() ? value.child(1) : ValueID());
            return;
        case Opcode::Call:
            if (resolveCallOfGetById(value))
                return;
            [[fallthrough]];
        case Opcode::Construct:
            if (BuiltinName global = globalNameOf(value.child(0))) {
                BuiltinID first = value.opcode == Opcode::Call ? m_builtins.find(BuiltinKind::Function, { }, global) : m_builtins.findConstructor(global);
                BuiltinID entry = applicableEntry(first, typesOf(value.arguments.span()).span());
                if (isResolvable(entry))
                    become(value, entry, m_builtins.classNamed(global), { }, Arguments(value.arguments));
            }
            return;
        case Opcode::Operation: {
            BuiltinID entry = applicableEntry(value.builtin(), typesOf(value.arguments.span()).span());
            RELEASE_ASSERT(entry);
            if (entry == m_builtins.callIntrinsic()) {
                convertCallIntrinsicToCall(value);
                return;
            }
            if (entry == m_builtins.hasOriginalStructureOperation()) {
                auto* engineClass = typeOf(value.arguments[0]).singleEngineClass();
                if (TypeID prediction = m_function.at(value.arguments[0]).prediction(); !engineClass && prediction)
                    engineClass = m_program.at(prediction).singleEngineClass();
                become(value, entry, engineClass && engineClass->canCheckOriginalStructure() ? engineClass : nullptr, { }, Arguments(value.arguments));
                return;
            }
            become(value, entry, nullptr, { }, Arguments(value.arguments));
            return;
        }
        default:
            return;
        }
    }

    void foldMethodReads()
    {
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::GetById)
                    continue;
                BuiltinID entry;
                if (const EngineClass* holder = classOfMethodHolder(value.child(0)))
                    entry = m_builtins.findForInstance(BuiltinKind::Method, *holder, nameOf(value));
                else if (BuiltinName holder = globalNameOf(value.child(0)))
                    entry = isObjectHasOwnProperty(holder, nameOf(value)) ? m_builtins.hasOwnProperty(nameOf(value)) : m_builtins.findStatic(BuiltinKind::StaticFunction, holder, nameOf(value));
                if (!entry || !m_builtins.hasCell(entry) || !Type::function().isSubsetOf(typeOf(id), m_program))
                    continue;
                value.opcode = Opcode::BuiltinFunction;
                value.children = { };
                value.immediates = { entry.index(), Format::none };
            }
        }
    }

    bool isBuiltinRead(const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::GetGlobal:
        case Opcode::GetGlobalForTypeOf:
            return m_builtins.isGlobal(nameOf(value));
        case Opcode::BuiltinFunction:
            return true;
        case Opcode::GetById: {
            BuiltinName name = nameOf(value);
            if (name == m_builtins.callName() && isBuiltinTableFunction(value.child(0)))
                return true;
            if (BuiltinName holder = globalNameOf(value.child(0)))
                return (name == m_builtins.prototypeName() && m_builtins.classNamed(holder)) || m_builtins.findStatic(BuiltinKind::StaticFunction, holder, name) || isObjectHasOwnProperty(holder, name);
            if (const EngineClass* holder = classOfMethodHolder(value.child(0)))
                return !!m_builtins.findForInstance(BuiltinKind::Method, *holder, name);
            auto& receiver = typeOf(value.child(0));
            const EngineClass* engineClass = classOf(receiver);
            return engineClass && !receiver.hasObjects() && !receiver.mayBeUndefinedOrNull() && m_builtins.findForInstance(BuiltinKind::Method, *engineClass, name);
        }
        default:
            return false;
        }
    }

    void foldLiteralJoins()
    {
        constexpr unsigned maxElements = 8;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& join = m_function.at(id);
                if (join.opcode != Opcode::CallBuiltin || join.arguments.size() != 1 || join.methodGetById())
                    continue;
                auto& implementation = m_builtins.at(join.builtin()).implementation;
                if (implementation.kind != BuiltinImplementation::Inline || implementation.inlined != InlineBuiltin::ArrayJoin)
                    continue;
                if (block.terminator.kind == TerminatorKind::Invoke && block.values.last() == id)
                    continue;
                ValueID receiver = join.child(0);
                ValueID separator = join.arguments[0];
                auto& literal = m_function.at(receiver);
                if (literal.opcode != Opcode::NewArray || m_uses[receiver.index()] != 1 || literal.arguments.isEmpty() || literal.arguments.size() > maxElements || !block.values.contains(receiver))
                    continue;
                if (!typeOf(separator).isString() || !std::ranges::all_of(literal.arguments, [&](ValueID element) { return typeOf(element).isString(); }))
                    continue;
                Arguments parts;
                for (ValueID element : literal.arguments) {
                    if (!parts.isEmpty())
                        parts.append(separator);
                    parts.append(element);
                }
                --m_uses[receiver.index()];
                join.opcode = Opcode::Strcat;
                join.type = m_program.types.string;
                join.rep = Rep::Cell;
                join.children = { };
                join.immediates = { Format::none, Format::none };
                join.arguments = WTF::move(parts);
            }
        }
    }

    void countUses()
    {
        m_uses.fill(0, m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                std::as_const(m_function).at(id).forEachChild([&](ValueID child) {
                    ++m_uses[child.index()];
                });
            }
            block.terminator.forEachChild([&](ValueID& child) {
                ++m_uses[child.index()];
            });
        }
    }

    void removeUnusedBuiltinReads()
    {
        bool changed;
        do {
            changed = false;
            for (auto& block : m_function.blocks) {
                for (size_t i = block.values.size(); i--;) {
                    ValueID id = block.values[i];
                    const Value& value = m_function.at(id);
                    if (m_uses[id.index()] || !isBuiltinRead(value))
                        continue;
                    value.forEachChild([&](ValueID child) {
                        --m_uses[child.index()];
                    });
                    if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
                        block.terminator.kind = TerminatorKind::Jump;
                        for (ValueID argument : block.terminator.targets[1].arguments)
                            --m_uses[argument.index()];
                        block.terminator.targets.shrink(1);
                    }
                    block.values.removeAt(i);
                    changed = true;
                }
            }
        } while (changed);
    }

    void markMethodReads()
    {
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& call = m_function.at(id);
                if (call.opcode != Opcode::CallBuiltin || !call.methodGetById())
                    continue;
                auto& read = m_function.at(call.methodGetById());
                if (read.opcode == Opcode::GetById && m_uses[call.methodGetById().index()] == 1 && m_program.at(read.type).isAnything())
                    read.immediates[1] = call.builtin().index();
            }
        }
    }

    const Program& m_program;
    Function& m_function;
    const Builtins& m_builtins;
    Vector<unsigned> m_uses;
};

} // anonymous namespace

void resolveBuiltins(const Program& program, Function& function)
{
    BuiltinResolution(program, function).run();
}

void convertCallIntrinsicToCall(Value& value)
{
    RELEASE_ASSERT(value.opcode == Opcode::Operation && value.arguments.size() >= 2);
    value.opcode = Opcode::Call;
    value.children = { value.arguments[0], value.arguments[1], ValueID() };
    value.immediates[0] = Format::none;
    value.arguments.removeAt(0, 2);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
