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
    switch (value.opcode) {
    case Opcode::IteratorOpen:
        return isArray(value.child(0)) ? KeyedAccess::Array : KeyedAccess::Generic;
    // An array stands for its own iterator. That it is one shows where the iterator is made.
    case Opcode::IteratorHasNext:
    case Opcode::IteratorValue:
    case Opcode::IteratorClose: {
        auto& iterator = function.at(value.child(0));
        return iterator.opcode == Opcode::IteratorOpen && isArray(iterator.child(0)) ? KeyedAccess::Array : KeyedAccess::Generic;
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
    case Opcode::PutGlobal:
    case Opcode::NewObjectWithShape:
    case Opcode::PutField:
    case Opcode::NewClosure:
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
    case Opcode::GetScope:
    case Opcode::CreateRest:
    case Opcode::HasShapeOf:
    case Opcode::IsClosureOf:
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
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values)
                resolve(m_function.at(id));
        }
        countUses();
        removeUnusedReadsOfBuiltins();
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
        while (m_function.at(id).opcode == Opcode::Cast)
            id = m_function.at(id).child(0);
        return m_function.at(id);
    }

    // The global object of the table that the value is, going by how it is defined. A class that the program may have extended is
    // itself as it was. An object whose properties it may have replaced says nothing.
    BuiltinName globalDefinedBy(ValueID id) const
    {
        auto& value = definitionOf(id);
        if (value.opcode != Opcode::GetGlobal)
            return { };
        BuiltinName name = nameOf(value);
        if (!m_builtins.isGlobal(name) || (m_program.isExtended(name) && !m_builtins.classNamed(name)))
            return { };
        return name;
    }

    // `Array.prototype`: the class.
    const EngineClass* classWhosePrototypeIsDefinedBy(ValueID id) const
    {
        auto& value = definitionOf(id);
        if (value.opcode != Opcode::GetById || nameOf(value) != m_builtins.prototypeName())
            return nullptr;
        return m_builtins.classNamed(globalDefinedBy(value.child(0)));
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

    // `Object.freeze(Map.prototype)`: the table says that the call hands back that argument and nothing else.
    bool handsBackPrototype(const Value& value) const
    {
        if (value.opcode != Opcode::CallMethod)
            return false;
        BuiltinID entry = applicableEntry(m_builtins.findStatic(BuiltinKind::StaticFunction, globalDefinedBy(value.child(0)), nameOf(value)), typesOf(value.arguments.span()).span());
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
    void become(Value& value, BuiltinID entry, const EngineClass* engineClass, ValueID receiver, Arguments&& arguments)
    {
        auto& builtin = m_builtins.at(entry);
        if (builtin.implementation.kind == BuiltinImplementation::Number) {
            if ((receiver && typeOf(receiver).mayBeUndefinedOrNull()) || !m_program.at(m_program.types.number).isWithin(m_program.at(value.type), m_program))
                return;
            value.opcode = Opcode::Double;
            value.type = m_program.types.number;
            value.rep = Rep::Double;
            value.children = { };
            value.arguments.clear();
            value.setBits(std::bit_cast<uint64_t>(builtin.implementation.number));
            return;
        }
        value.opcode = Opcode::CallBuiltin;
        value.children = { receiver, ValueID(), ValueID() };
        value.immediates = { entry.index(), engineClass ? engineClass->index : Format::none };
        value.arguments = WTF::move(arguments);
    }

    // Of what runs any code and yields anything there is nothing to know, and the stubs with their caches are the fastest way.
    bool saysSomething(BuiltinID entry) const
    {
        return entry && !(m_builtins.at(entry).isUnknown() && m_builtins.at(entry).result.isAnything());
    }

    // The class whose properties a receiver of the type finds, and nothing else: an object of a class that extends it may find something
    // else under a name.
    const EngineClass* classOfReceiver(const Type& type, BuiltinKind kind) const
    {
        const EngineClass* engineClass = classOf(type);
        if (!engineClass || m_program.isExtended(engineClass->name) || (engineClass->isExtendedByHost && kind == BuiltinKind::Method))
            return nullptr;
        return engineClass;
    }

    bool resolveForReceiver(Value& value, BuiltinKind kind, ValueID receiver, BuiltinName name, Arguments&& arguments)
    {
        const EngineClass* engineClass = classOfReceiver(typeOf(receiver), kind);
        if (!engineClass)
            return false;
        BuiltinID entry = applicableEntry(m_builtins.findForInstance(kind, *engineClass, name), typesOf(arguments.span()).span());
        if (!saysSomething(entry))
            return false;
        become(value, entry, engineClass, receiver, WTF::move(arguments));
        return true;
    }

    bool resolveForHolder(Value& value, BuiltinName holder, BuiltinName name)
    {
        BuiltinID entry = applicableEntry(m_builtins.findStatic(BuiltinKind::StaticFunction, holder, name), typesOf(value.arguments.span()).span());
        if (!saysSomething(entry))
            return false;
        become(value, entry, m_builtins.classNamed(holder), { }, Arguments(value.arguments));
        return true;
    }

    // `o.m(f())` reads `m` before it calls `f`: the front end writes a read, and a call of what was read with `o` for `this`. What is found
    // cannot be replaced, so it makes no difference when it is looked for. What is left of the read is that it throws where `o` is
    // `undefined` or `null`: a cast with the name, to the type `o` has.
    bool resolveCallOfRead(Value& value)
    {
        ValueID readID = value.child(0);
        auto& read = m_function.at(readID);
        if (read.opcode != Opcode::GetById || &definitionOf(read.child(0)) != &definitionOf(value.child(1)))
            return false;
        if (BuiltinName holder = globalDefinedBy(read.child(0)))
            return resolveForHolder(value, holder, nameOf(read));
        ValueID receiver = read.child(0);
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

    // `Array.prototype.slice.call(receiver, ...)`. What is listed under Object takes `undefined` and `null` too, which a receiver is not.
    bool resolveCallOfPrototypeMethod(Value& value)
    {
        if (nameOf(value) != m_builtins.callName() || value.arguments.isEmpty())
            return false;
        auto& method = definitionOf(value.child(0));
        if (method.opcode != Opcode::GetById)
            return false;
        const EngineClass* holder = classWhosePrototypeIsDefinedBy(method.child(0));
        const EngineClass* engineClass = classOf(typeOf(value.arguments[0]));
        // What is called on an array is looked for by name once the array has changed, see Lowering::callMethodOfArray(). This names the function.
        if (!holder || !engineClass || engineClass->type == ArrayType)
            return false;
        Arguments arguments;
        arguments.append(value.arguments.span().subspan(1));
        BuiltinID entry = applicableEntry(m_builtins.findForInstance(BuiltinKind::Method, *holder, nameOf(method)), typesOf(arguments.span()).span());
        if (!saysSomething(entry))
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
        case Opcode::CallMethod:
            if (BuiltinName holder = globalDefinedBy(value.child(0))) {
                resolveForHolder(value, holder, nameOf(value));
                return;
            }
            if (!resolveCallOfPrototypeMethod(value))
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
                if (saysSomething(entry))
                    become(value, entry, m_builtins.classNamed(global), { }, Arguments(value.arguments));
            }
            return;
        case Opcode::Operation: {
            BuiltinID entry = applicableEntry(m_builtins.find(BuiltinKind::Operation, { }, nameOf(value)), typesOf(value.arguments.span()).span());
            RELEASE_ASSERT(entry);
            become(value, entry, nullptr, { }, Arguments(value.arguments));
            return;
        }
        default:
            return;
        }
    }

    // It finds what cannot change, does nothing besides and cannot throw.
    bool isReadOfBuiltin(const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::GetGlobal:
        case Opcode::GetGlobalForTypeOf:
            return m_builtins.isGlobal(nameOf(value));
        case Opcode::GetById: {
            BuiltinName name = nameOf(value);
            if (BuiltinName holder = globalDefinedBy(value.child(0)))
                return (name == m_builtins.prototypeName() && m_builtins.classNamed(holder)) || m_builtins.findStatic(BuiltinKind::StaticFunction, holder, name);
            if (const EngineClass* holder = classWhosePrototypeIsDefinedBy(value.child(0)))
                return !!m_builtins.findForInstance(BuiltinKind::Method, *holder, name);
            auto& receiver = typeOf(value.child(0));
            const EngineClass* engineClass = classOfReceiver(receiver, BuiltinKind::Method);
            return engineClass && !receiver.mayBeUndefinedOrNull() && m_builtins.findForInstance(BuiltinKind::Method, *engineClass, name);
        }
        default:
            return false;
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
