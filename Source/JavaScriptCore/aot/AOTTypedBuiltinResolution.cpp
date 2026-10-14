/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

// Whether the conversion the built-in applies to the argument runs no code of the program and does not throw.
static bool accepts(ArgumentConversion conversion, const Type& argument)
{
    switch (conversion) {
    case ArgumentConversion::None:
        return true;
    case ArgumentConversion::RequiresNumber:
        return argument.isNumber();
    case ArgumentConversion::RequiresInteger:
        return argument.isInteger();
    // `undefined` and `null` are turned away, which an entry with such a parameter says it may do.
    case ArgumentConversion::RequiresTypedArray:
        return argument.onlyEngineClass() && argument.onlyEngineClass()->typedArrayType != NotTypedArray;
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
        return Type::ofPrimitives(Primitive::Number);
    case TypeBigInt64:
    case TypeBigUint64:
        return Type::ofPrimitives(Primitive::BigInt);
    case NotTypedArray:
    case TypeDataView:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

KeyedAccess keyedAccessOf(const Program& program, const Function& function, const Value& value)
{
    auto isArray = [&](ValueID id) {
        auto* engineClass = program.typeOf(function, id).onlyEngineClass();
        return engineClass && engineClass->type == ArrayType;
    };
    auto isTypedArray = [&](ValueID id) {
        auto* engineClass = program.typeOf(function, id).onlyEngineClass();
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
    if (!program.typeOf(function, value.child(1)).isNumber())
        return KeyedAccess::Generic;
    auto& receiver = program.typeOf(function, value.child(0));
    if (auto* engineClass = receiver.onlyEngineClass()) {
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
            // ToNumber of what is stored comes before everything else.
            return value.opcode == Opcode::GetByVal || program.typeOf(function, value.child(2)).isNumber() ? KeyedAccess::TypedArray : KeyedAccess::Generic;
        // Rounding to a clamped byte is left to the engine.
        case TypeUint8Clamped:
            return value.opcode == Opcode::GetByVal ? KeyedAccess::TypedArray : KeyedAccess::Generic;
        default:
            return KeyedAccess::Generic;
        }
    }
    bool isString = receiver.primitives & Primitive::String && receiver.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
    return isString && value.opcode == Opcode::GetByVal ? KeyedAccess::String : KeyedAccess::Generic;
}

// What a read of the field yields is what the slot holds, which other views may say more of than this one.
static bool fieldVouchesFor(const Program& program, const Layout& layout, const Field& field, const Type& type)
{
    // Before placement. Only what the language does not see as an object is looked at in one way.
    if (!field.stored)
        return !isObjectLayout(layout.kind) && field.type && program.at(field.type).isWithin(type, program);
    auto& stored = program.at(field.stored);
    if (!stored.withoutPrimitives(Primitive::Absent).isWithin(type, program))
        return false;
    // A property that is not there reads as `undefined`. A variable that holds nothing yet is not read: that throws.
    bool mayBeAbsent = isObjectLayout(layout.kind) && (stored.primitives & Primitive::Absent || field.isLackedBySomeMember);
    return !mayBeAbsent || type.primitives & Primitive::Undefined;
}

static bool tableVouchesFor(const Program& program, const Function& function, const Value& value, const Type& type)
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
    if (!Type::ofPrimitives(bits).isWithin(type, program))
        return false;
    if (result.kinds & BuiltinResult::Integer && !Type::integer({ result.least, result.most }).isWithin(type, program))
        return false;
    if (result.kinds & BuiltinResult::Instance && !Type::engineClass(builtins.classes()[result.engineClass]).isWithin(type, program))
        return false;
    if (result.kinds & BuiltinResult::InstanceOfSameClass && !Type::engineClass(builtins.classes()[value.engineClass()]).isWithin(type, program))
        return false;
    // Of `undefined` and `null` nothing is called.
    if (result.kinds & BuiltinResult::This && !program.typeOf(function, value.child(0)).withoutPrimitives(Primitive::UndefinedOrNull).isWithin(type, program))
        return false;
    for (unsigned i = 0; i < 8; ++i) {
        if (!(result.arguments & 1 << i))
            continue;
        auto& argument = i < value.arguments.size() ? program.typeOf(function, value.arguments[i]) : program.at(program.types.undefined);
        if (!argument.isWithin(type, program))
            return false;
    }
    return true;
}

bool vouchesForItsType(const Program& program, const Function& function, const Value& value)
{
    auto& type = program.at(value.type);
    if (type.isAnything())
        return true;
    switch (value.opcode) {
    // What it is follows from what is done.
    case Opcode::Parameter:
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::Boolean:
    case Opcode::Integer:
    case Opcode::Double:
    case Opcode::String:
    case Opcode::BigInt:
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
    case Opcode::GetResumeMode:
    case Opcode::GetCapture:
    case Opcode::CreateRest:
    case Opcode::GetRestLength:
    case Opcode::CreateThis:
    case Opcode::HasShapeOf:
    case Opcode::IsClassObjectAsDeclared:
    case Opcode::IsClosureOf:
    case Opcode::IsBuiltinFunction:
    case Opcode::BuiltinFunction:
    case Opcode::TypeOfIs:
    case Opcode::AssertNotAllocated:
    case Opcode::MaterializeEnvironment:
    // It is the test.
    case Opcode::Cast:
        return true;

    case Opcode::GetModuleVariable: {
        TypeID held = program.variables[value.variable().index()].type;
        return held && program.at(held).isWithin(type, program);
    }
    case Opcode::GetField: {
        auto& layout = program.at(value.layout());
        return fieldVouchesFor(program, layout, layout.fields[value.field()], type);
    }
    case Opcode::GetById: {
        auto& receiver = program.typeOf(function, value.child(0));
        // What any object comes up with.
        if (receiver.admitsForeignObjects(program))
            return false;
        if (auto access = fieldOf(program, receiver, value.string())) {
            auto& layout = program.at(access->layout);
            return fieldVouchesFor(program, layout, layout.fields[access->field], type);
        }
        // A method of a class, read as a value.
        LayoutID view = receiver.onlyView();
        return view && findMethod(program, view, value.string(), MethodKind::Method) && Type::function().isWithin(type, program);
    }
    case Opcode::CallDirect: {
        TypeID yielded = program.at(value.function()).resultType;
        return yielded && program.at(yielded).isWithin(type, program);
    }
    case Opcode::CallBuiltin:
        return tableVouchesFor(program, function, value, type);
    case Opcode::Construct:
    case Opcode::ConstructSuper:
    case Opcode::ConstructSuperForwardVarargs:
        return Type::object().isWithin(type, program);

    // It is what code that is not known, or a container, comes up with.
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
    if (!builtin.takesReceiver() || !program.typeOf(function, value.child(0)).hasObjects())
        return false;
    if (value.methodRead() || builtin.implementation.kind != BuiltinImplementation::Inline || !isExactForEveryReceiver(builtin.implementation.inlined))
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
        retypeReadsOfPrototypes();
        typeElementsAsClaimed();
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values)
                resolve(m_function.at(id));
        }
        foldReadsOfMethods();
        countUses();
        foldJoinsOfLiterals();
        removeUnusedReadsOfBuiltins();
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

    // The global object of the table that the value is, going by how it is defined. A class that the program may have extended is
    // itself as it was.
    BuiltinName globalDefinedBy(ValueID id) const
    {
        auto& value = definitionOf(id);
        if (value.opcode != Opcode::GetGlobal)
            return { };
        BuiltinName name = nameOf(value);
        return m_builtins.isGlobal(name) ? name : BuiltinName();
    }

    // `Array.prototype`: the class.
    const EngineClass* classWhosePrototypeIsDefinedBy(ValueID id) const
    {
        auto& value = definitionOf(id);
        if (value.opcode != Opcode::GetById || nameOf(value) != m_builtins.prototypeName())
            return nullptr;
        return m_builtins.classNamed(globalDefinedBy(value.child(0)));
    }

    // A function of the table, going by how the value is defined. What it finds under `call` cannot change.
    bool isFunctionOfTable(ValueID id) const
    {
        auto& value = definitionOf(id);
        if (value.opcode == Opcode::BuiltinFunction)
            return true;
        if (value.opcode != Opcode::GetById)
            return false;
        if (BuiltinName holder = globalDefinedBy(value.child(0)))
            return !!m_builtins.findStatic(BuiltinKind::StaticFunction, holder, nameOf(value));
        const EngineClass* holder = classWhosePrototypeIsDefinedBy(value.child(0));
        return holder && m_builtins.findForInstance(BuiltinKind::Method, *holder, nameOf(value));
    }

    // The one class a value of the type is of. `undefined` and `null` are of none: whatever is asked of them throws. Beside a number or
    // a boolean there is no telling them without knowing how the value is kept.
    const EngineClass* classOf(const Type& type) const
    {
        if (auto* engineClass = type.onlyEngineClass())
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

    // `Object.freeze(Map.prototype)`: the table says that the call hands back that argument and nothing else. As a call by name, or as a
    // read of the function and a call of what was read.
    bool handsBackPrototype(const Value& value) const
    {
        BuiltinID first;
        if (value.opcode == Opcode::CallMethod)
            first = m_builtins.findStatic(BuiltinKind::StaticFunction, globalDefinedBy(value.child(0)), nameOf(value));
        else if (value.opcode == Opcode::Call) {
            auto& read = m_function.at(value.child(0));
            if (read.opcode == Opcode::GetById && &definitionOf(read.child(0)) == &definitionOf(value.child(1)))
                first = m_builtins.findStatic(BuiltinKind::StaticFunction, globalDefinedBy(read.child(0)), nameOf(read));
        }
        BuiltinID entry = applicableEntry(first, typesOf(value.arguments.span()).span());
        if (!entry)
            return false;
        auto& result = m_builtins.at(entry).result;
        if (result.kinds || !hasOneBitSet(result.arguments))
            return false;
        unsigned argument = WTF::ctz(static_cast<unsigned>(result.arguments));
        return argument < value.arguments.size() && classWhosePrototypeIsDefinedBy(value.arguments[argument]);
    }

    // `Map.prototype` is declared to be a Map and is none, so that to hold it to its type would throw. Where nothing goes by the type, as in
    // `Map.prototype.get.call(map, key)`, it is read as anything, and so is what hands it back.
    void typeElementsAsClaimed()
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
                if (claimed.hasObjects() || !claimed.isWithin(m_program.at(read.type), m_program))
                    continue;
                if (read.readsElementKnownByTheStores() && read.type != cast.type)
                    continue;
                read.type = cast.type;
                read.rep = repOf(claimed);
                cast.setCastReason(CastReason::Upcast);
            }
        }
    }

    void retypeReadsOfPrototypes()
    {
        Vector<ValueID, 4> reads;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                if (typeOf(id).engineClasses.isEmpty())
                    continue;
                auto& value = m_function.at(id);
                if ((value.opcode == Opcode::GetById && classWhosePrototypeIsDefinedBy(id)) || handsBackPrototype(value))
                    reads.append(id);
            }
        }
        if (reads.isEmpty())
            return;
        Vector<unsigned, 4> usesAsAnything;
        usesAsAnything.fill(0, reads.size());
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
                            ++usesAsAnything[at];
                    });
                    break;
                default:
                    break;
                }
            }
        }
        for (unsigned i = 0; i < reads.size(); ++i) {
            if (usesAsAnything[i] != m_uses[reads[i].index()])
                continue;
            m_function.at(reads[i]).type = m_program.types.anything;
            m_function.at(reads[i]).rep = Rep::JSValue;
        }
    }

    // The receiver: `this`, of what takes one. The class: of the receiver, or the one the function or the constructor was found on.
    // `methodRead`: see Value::methodRead().
    void become(Value& value, BuiltinID entry, const EngineClass* engineClass, ValueID receiver, Arguments&& arguments, ValueID methodRead = { })
    {
        auto& builtin = m_builtins.at(entry);
        if (builtin.implementation.kind == BuiltinImplementation::Number && !receiver) {
            if (!m_program.at(m_program.types.number).isWithin(m_program.at(value.type), m_program))
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
        value.children = { receiver, methodRead, ValueID() };
        value.immediates = { entry.index(), engineClass ? engineClass->index : Format::none };
        value.arguments = WTF::move(arguments);

        // The one table says which integers the built-in yields. Where that is less than the value was taken for it is what the value is: the checker gives `a.length` its range only where it
        // relies on the type of `a`, not behind an `as`; here it is known what is carried out.
        // Not where other code may be carried out in its place: a method of an object may be another's, and a getter of a prototype is shadowed by a property of the object's own, as `length` of a typed
        // array and `size` of a Map are: there the general arm yields whatever it yields.
        TypeID yielded = m_program.integersYieldedByBuiltins[entry.index()];
        if (!yielded || !m_program.at(yielded).isWithin(m_program.at(value.type), m_program) || m_program.at(value.type).isWithin(m_program.at(yielded), m_program))
            return;
        if (builtin.takesReceiver() && typeOf(receiver).hasObjects() && (builtin.kind != BuiltinKind::Getter || hasGenericFallback(m_program, m_function, value)))
            return;
        value.type = yielded;
        value.rep = repOf(m_program.at(yielded));
    }

    bool isResolvable(BuiltinID entry) const
    {
        return entry && m_builtins.at(entry).implementation.kind != BuiltinImplementation::Generic;
    }

    // The class whose properties a receiver of the type finds while it has its original structure, if that can be tested. A primitive
    // always finds them.
    const EngineClass* classOfReceiver(const Type& type) const
    {
        const EngineClass* engineClass = classOf(type);
        if (!engineClass || (type.hasObjects() && !engineClass->canCheckOriginalStructure()))
            return nullptr;
        return engineClass;
    }

    bool resolveForReceiver(Value& value, BuiltinKind kind, ValueID receiver, BuiltinName name, Arguments&& arguments, ValueID methodRead = { })
    {
        const EngineClass* engineClass = classOfReceiver(typeOf(receiver));
        if (!engineClass && !typeOf(receiver).hasObjects() && typeOf(receiver).mayBeUndefinedOrNull())
            engineClass = classOf(typeOf(receiver).withoutPrimitives(Primitive::UndefinedOrNull));
        if (!engineClass)
            return false;
        BuiltinID entry = applicableEntry(m_builtins.findForInstance(kind, *engineClass, name), typesOf(arguments.span()).span());
        if (!isResolvable(entry))
            return false;
        become(value, entry, engineClass, receiver, WTF::move(arguments), methodRead);
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

    // `o.m(f())` reads `m` before it calls `f`: the front end writes a read, and a call of what was read with `o` for `this`. What a global
    // object or a primitive finds is immutable, so it makes no difference when it is looked for: what is left of the read is that it
    // throws where `o` is `undefined` or `null`, a cast with the name, to the type `o` has. An object may lose its original structure in
    // between: the read stays, for the fallback.
    bool resolveCallOfRead(Value& value)
    {
        ValueID readID = value.child(0);
        auto& read = m_function.at(readID);
        if (read.opcode != Opcode::GetById || &definitionOf(read.child(0)) != &definitionOf(value.child(1)))
            return false;
        // `String.prototype.slice.call(receiver, i + 1)`
        if (nameOf(read) == m_builtins.callName() && isFunctionOfTable(read.child(0)) && !value.arguments.isEmpty()) {
            if (!resolveCallOfPrototypeMethod(value, read.child(0))) {
                value.children = { read.child(0), value.arguments[0], ValueID() };
                value.arguments.removeAt(0);
            }
            return true;
        }
        if (BuiltinName holder = globalDefinedBy(read.child(0)))
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

    // `String.prototype.slice.call(receiver, ...)`. What is listed under Object takes `undefined` and `null` too, which a receiver is not.
    // The function is named, so a receiver that would take the call by name instead is left alone.
    bool resolveCallOfPrototypeMethod(Value& value, ValueID function)
    {
        if (value.arguments.isEmpty())
            return false;
        auto& method = definitionOf(function);
        if (method.opcode != Opcode::GetById)
            return false;
        const EngineClass* holder = classWhosePrototypeIsDefinedBy(method.child(0));
        if (BuiltinID found = m_builtins.hasOwnProperty(nameOf(method)); found && holder && m_builtins.findForInstance(BuiltinKind::Method, *holder, nameOf(method)) == found) {
            BuiltinID entry = applicableEntry(m_builtins.hasOwn(), typesOf(value.arguments.span()).span());
            if (!isResolvable(entry))
                return false;
            become(value, entry, holder, { }, Arguments(value.arguments));
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
        BuiltinName listedUnder = m_builtins.at(entry).holderName;
        if (listedUnder != engineClass->name && listedUnder != engineClass->listedAs)
            return false;
        become(value, entry, engineClass, value.arguments[0], WTF::move(arguments));
        return true;
    }

    void resolve(Value& value)
    {
        switch (value.opcode) {
        case Opcode::GetById:
            if (BuiltinName holder = globalDefinedBy(value.child(0))) {
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
                Type element = elementTypeOf(typeOf(m_function.at(value.child(0)).child(0)).onlyEngineClass()->typedArrayType);
                TypeID narrowed = element.isWithin(m_program.at(m_program.types.int32), m_program) ? m_program.types.int32 : m_program.types.number;
                if (m_program.at(narrowed).isWithin(m_program.at(value.type), m_program)) {
                    value.type = narrowed;
                    value.rep = repOf(m_program.at(narrowed));
                }
            }
            return;
        case Opcode::CallMethod:
            if (BuiltinName holder = globalDefinedBy(value.child(0))) {
                resolveForHolder(value, holder, nameOf(value));
                return;
            }
            if (nameOf(value) == m_builtins.callName() && isFunctionOfTable(value.child(0)) && !value.arguments.isEmpty()) {
                if (!resolveCallOfPrototypeMethod(value, value.child(0))) {
                    value.opcode = Opcode::Call;
                    value.children = { value.child(0), value.arguments[0], ValueID() };
                    value.immediates[0] = Format::none;
                    value.arguments.removeAt(0);
                }
                return;
            }
            resolveForReceiver(value, BuiltinKind::Method, value.child(0), nameOf(value), Arguments(value.arguments));
            return;
        case Opcode::Call:
            if (resolveCallOfRead(value))
                return;
            [[fallthrough]];
        case Opcode::Construct:
            if (BuiltinName global = globalDefinedBy(value.child(0))) {
                BuiltinID first = value.opcode == Opcode::Call ? m_builtins.find(BuiltinKind::Function, { }, global) : m_builtins.findConstructor(global);
                BuiltinID entry = applicableEntry(first, typesOf(value.arguments.span()).span());
                if (isResolvable(entry))
                    become(value, entry, m_builtins.classNamed(global), { }, Arguments(value.arguments));
            }
            return;
        case Opcode::Operation: {
            BuiltinID entry = applicableEntry(m_builtins.find(BuiltinKind::Operation, { }, nameOf(value)), typesOf(value.arguments.span()).span());
            RELEASE_ASSERT(entry);
            if (entry == m_builtins.callIntrinsic()) {
                convertCallIntrinsicToCall(value);
                return;
            }
            // Asked for by name it is about the class its operand is of by type. Of none by now, or of one that cannot be asked: then it does not hold.
            if (entry == m_builtins.hasOriginalStructureOperation()) {
                auto* engineClass = typeOf(value.arguments[0]).onlyEngineClass();
                if (TypeID prediction = m_function.at(value.arguments[0]).prediction; !engineClass && prediction)
                    engineClass = m_program.at(prediction).onlyEngineClass();
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

    // `Object.prototype.toString`, `Object.keys`, where the call it was read for is not resolved, or it was read for its own sake. What is found
    // there cannot change, so it is not looked for.
    void foldReadsOfMethods()
    {
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::GetById)
                    continue;
                BuiltinID entry;
                if (const EngineClass* holder = classWhosePrototypeIsDefinedBy(value.child(0)))
                    entry = m_builtins.findForInstance(BuiltinKind::Method, *holder, nameOf(value));
                else if (BuiltinName holder = globalDefinedBy(value.child(0)))
                    entry = m_builtins.findStatic(BuiltinKind::StaticFunction, holder, nameOf(value));
                if (!entry || !m_builtins.hasCell(entry) || !Type::function().isWithin(typeOf(id), m_program))
                    continue;
                value.opcode = Opcode::BuiltinFunction;
                value.children = { };
                value.immediates = { entry.index(), Format::none };
            }
        }
    }

    // It finds what cannot change, does nothing besides and cannot throw.
    bool isReadOfBuiltin(const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::GetGlobal:
        case Opcode::GetGlobalForTypeOf:
            return m_builtins.isGlobal(nameOf(value));
        case Opcode::BuiltinFunction:
            return true;
        case Opcode::GetById: {
            BuiltinName name = nameOf(value);
            if (name == m_builtins.callName() && isFunctionOfTable(value.child(0)))
                return true;
            if (BuiltinName holder = globalDefinedBy(value.child(0)))
                return (name == m_builtins.prototypeName() && m_builtins.classNamed(holder)) || m_builtins.findStatic(BuiltinKind::StaticFunction, holder, name);
            if (const EngineClass* holder = classWhosePrototypeIsDefinedBy(value.child(0)))
                return !!m_builtins.findForInstance(BuiltinKind::Method, *holder, name);
            auto& receiver = typeOf(value.child(0));
            const EngineClass* engineClass = classOf(receiver);
            return engineClass && !receiver.hasObjects() && !receiver.mayBeUndefinedOrNull() && m_builtins.findForInstance(BuiltinKind::Method, *engineClass, name);
        }
        default:
            return false;
        }
    }

    void foldJoinsOfLiterals()
    {
        constexpr unsigned maxElements = 8;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& join = m_function.at(id);
                if (join.opcode != Opcode::CallBuiltin || join.arguments.size() != 1 || join.methodRead())
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

    // What was read only to find the function is of no use once the function is known.
    void removeUnusedReadsOfBuiltins()
    {
        bool changed;
        do {
            changed = false;
            for (auto& block : m_function.blocks) {
                for (size_t i = block.values.size(); i--;) {
                    ValueID id = block.values[i];
                    const Value& value = m_function.at(id);
                    if (m_uses[id.index()] || !isReadOfBuiltin(value))
                        continue;
                    value.forEachChild([&](ValueID child) {
                        --m_uses[child.index()];
                    });
                    // Where a handler watches, what may throw is the last in its block. Nothing before it may.
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

    // See Value::readsMethodFor().
    void markMethodReads()
    {
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& call = m_function.at(id);
                if (call.opcode != Opcode::CallBuiltin || !call.methodRead())
                    continue;
                auto& read = m_function.at(call.methodRead());
                if (read.opcode == Opcode::GetById && m_uses[call.methodRead().index()] == 1 && m_program.at(read.type).isAnything())
                    read.immediates[1] = call.builtin().index();
            }
        }
    }

    const Program& m_program;
    Function& m_function;
    const Builtins& m_builtins;
    // By value: how many operands of values and terminators it is.
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
    // The second is the inlining depth, as of every call.
    value.immediates[0] = Format::none;
    value.arguments.removeAt(0, 2);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
