/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedIR.h"
#include "Options.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

class Reader {
public:
    Reader(Program& program)
        : m_program(program)
        , m_buffer(program.buffer.span())
    {
    }

    ASCIILiteral run()
    {
        if (m_buffer.size() < sizeof(Format::Header) || reinterpret_cast<uintptr_t>(m_buffer.data()) % alignof(uint32_t))
            return "the program is shorter than its header"_s;
        m_header = reinterpret_cast<const Format::Header*>(m_buffer.data());
        if (m_header->magic != Format::magic)
            return "this is not a program of the front end"_s;
        if (m_header->version != Format::version || m_header->opcodeNamesHash != Format::opcodeNamesHash)
            return "the front end and the engine are of different versions"_s;
        if (m_header->sizeInBytes != m_buffer.size())
            return "the program is not as long as it says"_s;
        for (unsigned i = 0; i < std::size(Format::recordSizes); ++i) {
            if (m_header->recordSizes[i] != Format::recordSizes[i])
                return "the front end and the engine lay out a record differently"_s;
        }

        auto bytes = section<uint8_t>(m_header->bytes);
        m_words = section<uint32_t>(m_header->words);
        m_objectParts = section<Format::ObjectPart>(m_header->objectParts);
        m_fields = section<Format::Field>(m_header->fields);
        m_methods = section<Format::Method>(m_header->methods);
        m_blocks = section<Format::Block>(m_header->blocks);
        m_values = section<Format::Value>(m_header->values);
        m_targets = section<Format::Target>(m_header->targets);
        if (m_failed)
            return "a section lies outside the program"_s;

        for (auto& string : section<Format::String>(m_header->strings)) {
            if (static_cast<uint64_t>(string.offset) + string.length > bytes.size())
                return "a string lies outside the program"_s;
            m_program.strings.append(bytes.subspan(string.offset, string.length));
            m_program.noteString(StringID(m_program.strings.size() - 1));
        }
        for (auto& declared : section<Format::DeclaredType>(m_header->declaredTypes))
            m_types.append(m_program.types.intern(read(declared)));
        auto& types = m_program.types;
        types.never = types.intern(Type::never());
        types.undefined = types.intern(Type::ofPrimitives(Primitive::Undefined));
        types.boolean = types.intern(Type::ofPrimitives(Primitive::Boolean));
        types.int32 = types.intern(Type::integer(Range::int32()));
        types.number = types.intern(Type::ofPrimitives(Primitive::Number));
        types.string = types.intern(Type::ofPrimitives(Primitive::String));
        types.function = types.intern(Type::function());
        types.object = types.intern(Type::object());
        types.anything = types.intern(Type::anything());
        for (auto& shape : section<Format::Shape>(m_header->shapes)) {
            Layout layout;
            layout.kind = static_cast<LayoutKind>(shape.kind);
            layout.name = StringID(shape.name);
            layout.base = LayoutID(shape.base);
            layout.constructor = FunctionID(shape.constructor);
            layout.environment = LayoutID(shape.environment);
            layout.isAllocated = shape.flags & Format::ShapeIsAllocated;
            layout.mayBeFrozen = shape.flags & Format::ShapeMayBeFrozen;
            layout.isLaidOut = shape.flags & Format::ShapeIsLaidOut;
            layout.isEvaluatedOnce = shape.flags & Format::ShapeIsEvaluatedOnce;
            layout.hasForeignRoot = shape.flags & Format::ShapeHasForeignRoot;
            layout.hasFixedMembers = shape.flags & Format::ShapeHasFixedMembers;
            layout.mayHaveOtherPrototype = shape.flags & Format::ShapeMayHaveOtherPrototype;
            layout.hasStaticMemberOfUnknownName = shape.flags & Format::ShapeHasStaticMemberOfUnknownName;
            layout.admitsForeignObjects = shape.flags & Format::ShapeAdmitsForeignObjects;
            for (uint32_t member : slice(m_words, shape.members))
                layout.members.append(LayoutID(member));
            for (uint32_t specialization : slice(m_words, shape.specializations))
                layout.specializations.append(LayoutID(specialization));
            for (auto& from : slice(m_fields, shape.fields)) {
                Field field;
                field.name = StringID(from.name);
                field.type = typeAt(from.declaredType);
                field.flags = (from.flags & (fieldIsOptional | fieldIsImmutable)) | (from.flags & Format::FieldIsMethod ? fieldIsMethod : 0) | (from.flags & Format::FieldIsAnnotated ? fieldIsAnnotated : 0);
                if (layout.kind == LayoutKind::Environment && from.flags & Format::FieldIsNeverSeenUninitialized)
                    field.flags |= fieldIsNeverSeenUninitialized | (from.flags & Format::FieldIsStoredOnce ? fieldIsStoredOnce : 0);
                auto literals = slice(m_words, from.literals);
                if (literals.size() % 3)
                    return "the literals of a field are not whole"_s;
                for (size_t i = 0; i < literals.size(); i += 3) {
                    bool isNumber = literals[i] == static_cast<uint32_t>(Format::LiteralKind::Number);
                    if (!isNumber && (literals[i] != static_cast<uint32_t>(Format::LiteralKind::String) || literals[i + 1] >= m_program.strings.size()))
                        return "a literal of a field is neither a string of the program nor a number"_s;
                    field.literals.append(isNumber ? Literal { StringID(), std::bit_cast<double>(static_cast<uint64_t>(literals[i + 2]) << 32 | literals[i + 1]) } : Literal { StringID(literals[i + 1]), 0 });
                }
                layout.fields.append(WTF::move(field));
            }
            for (auto& method : slice(m_methods, shape.methods)) {
                bool isStatic = method.flags & Format::MethodIsStatic;
                if (method.name == Format::none) {
                    if (method.flags & Format::MethodKeyIsSymbol)
                        m_keysThatAreSymbols.append({ LayoutID(m_program.layouts.size()), method.key });
                    else
                        (isStatic ? layout.hasStaticMemberOfUnknownName : layout.hasMethodOfUnknownName) = true;
                }
                (isStatic ? layout.staticMethods : layout.methods).append(Method { StringID(method.name), FunctionID(method.function), method.kind, method.key, !!(method.flags & Format::MethodMayBeReplaced) });
            }
            auto staticFields = slice(m_words, shape.staticFields);
            for (size_t i = 0; i + 1 < staticFields.size(); i += 2)
                layout.staticFields.append(StaticField { StringID(staticFields[i]), staticFields[i + 1] & ~Format::StaticFieldIsAnnotated, !!(staticFields[i + 1] & Format::StaticFieldIsAnnotated) });
            m_program.layouts.append(WTF::move(layout));
        }
        for (auto& from : section<Format::Variable>(m_header->variables)) {
            Variable variable;
            variable.name = StringID(from.name);
            variable.module = ModuleID(from.module);
            variable.type = typeAt(from.declaredType);
            variable.startsUndefined = from.initialization == Format::Initialization::Undefined;
            variable.isStoredOnce = from.initialization == Format::Initialization::NeverSeenAndStoredOnce;
            variable.isNeverSeenUninitialized = variable.isStoredOnce || from.initialization == Format::Initialization::NeverSeen;
            m_program.variables.append(WTF::move(variable));
        }
        for (auto& module : section<Format::Module>(m_header->modules))
            m_program.modules.append(Module { StringID(module.path), FunctionID(module.initializer) });
        for (auto& function : section<Format::Function>(m_header->functions))
            m_program.functions.append(read(function));
        m_program.entry = FunctionID(m_header->entry);
        for (uint32_t string : slice(m_words, m_header->extendedGlobals)) {
            BuiltinName name = string < m_program.strings.size() ? m_program.builtinName(StringID(string)) : BuiltinName();
            if (Builtins::singleton().mayBeExtended(name))
                m_program.extendedGlobals.set(name.index());
        }
        if (m_failed || m_program.entry.index() >= m_program.functions.size())
            return "a list lies outside its section"_s;
        return checkClasses();
    }

private:
    // The run time makes class objects, prototype objects and the objects of classes from what the shapes say, and reads and writes the
    // variables of a class by their numbers. So what they say has to hang together.
    ASCIILiteral checkClasses()
    {
        auto& layouts = m_program.layouts;
        auto& functions = m_program.functions;
        for (unsigned i = 0; i < layouts.size(); ++i) {
            auto& layout = layouts[i];
            if (layout.kind != LayoutKind::Instance) {
                if (layout.constructor || layout.environment || !layout.staticMethods.isEmpty() || !layout.staticFields.isEmpty())
                    return "a shape that is no class has what only a class has"_s;
                // Of the type of an object literal: the accessors it declares. See Layout::methods.
                for (auto& method : layout.methods) {
                    if (layout.kind != LayoutKind::Record || method.kind == MethodKind::Method || !method.name || method.name.index() >= m_program.strings.size() || method.function.index() >= functions.size())
                        return "a shape that is no class has methods"_s;
                }
                continue;
            }
            // Only types name the class: its declaration is no part of the program, so there is no class object and there are no objects.
            if (!layout.constructor) {
                if (layout.isAllocated || layout.environment || !layout.methods.isEmpty() || !layout.staticMethods.isEmpty() || !layout.staticFields.isEmpty())
                    return "a class without a constructor has objects or members"_s;
                continue;
            }
            if (layout.constructor.index() >= functions.size() || functions[layout.constructor.index()].kind != FunctionKind::Constructor)
                return "what a class has for its constructor is none"_s;
            auto& constructor = functions[layout.constructor.index()];
            if (constructor.constructedClass)
                return "two classes have the same constructor"_s;
            constructor.constructedClass = LayoutID(i);
            if (!constructor.has(Format::FunctionHasThis) || (constructor.has(Format::FunctionHasNewTarget) && constructor.parameterTypes.isEmpty()))
                return "a constructor does not take what it is handed"_s;
            if (layout.hasForeignRoot && !constructor.has(Format::FunctionHasNewTarget))
                return "the constructor of a class with a foreign root does not take new.target"_s;
            if (!layout.isLaidOut && (!layout.fields.isEmpty() || layout.isAllocated || !layout.members.isEmpty() || !layout.specializations.isEmpty()))
                return "a class that is not laid out has fields or objects"_s;
            if (layout.isLaidOut && layout.hasForeignRoot)
                return "a class with a foreign root is laid out"_s;
            if (layout.base) {
                if (layout.base.index() >= i || layouts[layout.base.index()].kind != LayoutKind::Instance)
                    return "a class extends what is no class before it"_s;
                auto& base = layouts[layout.base.index()];
                if (!layout.isLaidOut || !base.isLaidOut || !base.constructor)
                    return "a class that is not laid out extends or is extended by a class of the program"_s;
                if (layout.isEvaluatedOnce && !base.isEvaluatedOnce)
                    return "a class that is evaluated once extends one that is not"_s;
            }
            uint32_t numberOfVariables = m_program.variables.size();
            if (layout.environment) {
                if (layout.isEvaluatedOnce || layout.environment.index() >= layouts.size() || layouts[layout.environment.index()].kind != LayoutKind::Environment)
                    return "a class closes over what is no environment"_s;
                numberOfVariables = layouts[layout.environment.index()].fields.size();
            } else if (!layout.isEvaluatedOnce)
                return "a class that is not evaluated once has no environment"_s;
            for (auto* methods : { &layout.methods, &layout.staticMethods }) {
                for (auto& method : *methods) {
                    if (method.function.index() >= functions.size() || (method.name ? method.name.index() >= m_program.strings.size() : method.key >= numberOfVariables))
                        return "a method has no function or no name"_s;
                    LayoutID environment = functions[method.function.index()].environment;
                    if (environment && environment != layout.environment)
                        return "a method closes over another environment than its class"_s;
                }
            }
            if (constructor.environment && constructor.environment != layout.environment)
                return "a constructor closes over another environment than its class"_s;
            if (!layout.staticFields.isEmpty() && !layout.hasFixedMembers)
                return "a class whose members are not fixed has static fields in variables"_s;
            for (auto& field : layout.staticFields) {
                if (field.name.index() >= m_program.strings.size() || field.variable >= numberOfVariables)
                    return "a static field has no name or no variable"_s;
                // The run time reads and writes it for whoever has the class object, and tells by what it holds whether it is there yet.
                if (layout.environment ? layouts[layout.environment.index()].fields[field.variable].flags & fieldIsNeverSeenUninitialized : m_program.variables[field.variable].isNeverSeenUninitialized)
                    return "a static field is in a variable that is said never to be seen uninitialized"_s;
            }
        }
        for (auto [id, key] : m_keysThatAreSymbols) {
            auto& layout = m_program.at(id);
            TypeID type = layout.environment ? m_program.at(layout.environment).fields[key].type : m_program.variables[key].type;
            if (!type || !m_program.at(type).hasOnlyPrimitives(Primitive::Symbol))
                return "the variable of a computed name that is said to be a symbol holds more than symbols"_s;
        }
        for (unsigned i = 0; i < m_program.types.size(); ++i) {
            for (LayoutID view : m_program.at(TypeID(i)).views) {
                if (view.index() >= layouts.size() || (m_program.at(view).kind == LayoutKind::Instance && !m_program.at(view).isLaidOut))
                    return "a type names a class that is not laid out"_s;
            }
        }
        return { };
    }

    template<typename T>
    std::span<const T> section(Format::Section section)
    {
        if (section.offsetInBytes % alignof(T) || static_cast<uint64_t>(section.offsetInBytes) + static_cast<uint64_t>(section.count) * sizeof(T) > m_buffer.size()) {
            m_failed = true;
            return { };
        }
        return { reinterpret_cast<const T*>(m_buffer.data() + section.offsetInBytes), section.count };
    }

    template<typename T>
    std::span<const T> slice(std::span<const T> all, Format::Span span)
    {
        if (static_cast<uint64_t>(span.start) + span.count > all.size()) {
            m_failed = true;
            return { };
        }
        return all.subspan(span.start, span.count);
    }

    template<typename Container>
    void readValues(Format::Span span, Container& result)
    {
        for (uint32_t word : slice(m_words, span))
            result.append(ValueID(word));
    }

    // None: the front end says nothing.
    TypeID typeAt(uint32_t index)
    {
        if (index == Format::none)
            return { };
        if (index >= m_types.size()) {
            m_failed = true;
            return { };
        }
        return m_types[index];
    }

    Type read(const Format::DeclaredType& declared)
    {
        static constexpr std::pair<uint32_t, uint16_t> primitives[] = {
            { Format::PrimitiveUndefined, Primitive::Undefined },
            { Format::PrimitiveNull, Primitive::Null },
            { Format::PrimitiveFalse, Primitive::False },
            { Format::PrimitiveTrue, Primitive::True },
            { Format::PrimitiveNumber, Primitive::Number },
            { Format::PrimitiveString, Primitive::String },
            { Format::PrimitiveSymbol, Primitive::Symbol },
            { Format::PrimitiveBigInt, Primitive::BigInt },
        };
        uint16_t bits = 0;
        for (auto [from, to] : primitives) {
            if (declared.primitives & from)
                bits |= to;
        }
        Type type = Type::ofPrimitives(bits);
        if (declared.flags & Format::DeclaredIntegers && !(bits & Primitive::Int)) {
            type.primitives |= Primitive::Int;
            type.integers.least = static_cast<int64_t>(static_cast<uint64_t>(declared.leastIntegerHigh) << 32 | declared.leastIntegerLow);
            type.integers.most = static_cast<int64_t>(static_cast<uint64_t>(declared.mostIntegerHigh) << 32 | declared.mostIntegerLow);
        }
        type.anyObject = declared.flags & Format::DeclaredAnyObject;
        type.anyFunction = declared.flags & Format::DeclaredAnyFunction;
        auto& builtins = Builtins::singleton();
        auto addPrimitiveOrItsObject = [&](uint16_t primitive) {
            type.primitives |= primitive;
            if (primitive & Primitive::Int)
                type.integers = Range::full();
            type.anyObject = true;
        };
        for (auto& part : slice(m_objectParts, declared.objects)) {
            switch (part.kind) {
            case Format::ObjectPartKind::Class:
                type.views.append(LayoutID(part.shapeOrName));
                break;
            case Format::ObjectPartKind::Array: {
                type.engineClasses.append(builtins.arrayClass().index);
                auto arguments = slice(m_words, part.typeArguments);
                if (arguments.size() == 1)
                    type.elements = typeAt(arguments[0]);
                break;
            }
            case Format::ObjectPartKind::GlobalClass: {
                BuiltinName name = part.shapeOrName < m_program.strings.size() ? m_program.builtinName(StringID(part.shapeOrName)) : BuiltinName();
                switch (builtins.interfaceNamed(name)) {
                case GlobalInterface::Class:
                    // Only what can be told by its JSType is taken for the class. Anything else is an interface that any object may have.
                    if (builtins.classNamed(name)->type == CellType)
                        type.anyObject = true;
                    else
                        type.engineClasses.append(builtins.classNamed(name)->index);
                    break;
                case GlobalInterface::Function:
                    type.anyFunction = true;
                    break;
                case GlobalInterface::String:
                    addPrimitiveOrItsObject(Primitive::String);
                    break;
                case GlobalInterface::Number:
                    addPrimitiveOrItsObject(Primitive::Number);
                    break;
                case GlobalInterface::Boolean:
                    addPrimitiveOrItsObject(Primitive::Boolean);
                    break;
                case GlobalInterface::Symbol:
                    addPrimitiveOrItsObject(Primitive::Symbol);
                    break;
                case GlobalInterface::BigInt:
                    addPrimitiveOrItsObject(Primitive::BigInt);
                    break;
                case GlobalInterface::Unknown:
                    type.anyObject = true;
                    break;
                }
                break;
            }
            }
        }
        if (type.anyObject) {
            // Any object: a function is one.
            type.anyFunction = true;
            type.views.clear();
            type.engineClasses.clear();
            type.elements = { };
        }
        std::ranges::sort(type.views);
        std::ranges::sort(type.engineClasses);
        type.engineClasses.shrink(std::unique(type.engineClasses.begin(), type.engineClasses.end()) - type.engineClasses.begin());
        return type;
    }

    Function read(const Format::Function& source)
    {
        Function function;
        function.name = StringID(source.name);
        function.module = ModuleID(source.module);
        function.position = source.position;
        if (source.kind > FunctionKind::Module)
            m_failed = true;
        function.kind = source.kind;
        function.flags = source.flags;
        function.declaredAs = (source.flags & Format::FunctionIsAsync ? functionIsAsync : 0) | (source.flags & Format::FunctionIsGenerator ? functionIsGenerator : 0);
        function.length = source.length;
        function.environment = LayoutID(source.environment);
        function.thisType = typeAt(source.thisType);
        function.resultType = typeAt(source.resultType);
        for (uint32_t word : slice(m_words, source.parameterTypes)) {
            if (word != Format::none && word & Format::ParameterIsAnnotated) {
                function.annotatedParameters.set(function.parameterTypes.size());
                word &= ~Format::ParameterIsAnnotated;
            }
            function.parameterTypes.append(typeAt(word));
        }
        for (auto& from : slice(m_values, source.values)) {
            Value value;
            if (static_cast<uint32_t>(from.opcode) >= numberOfFormatOpcodes) {
                m_failed = true;
                break;
            }
            value.opcode = static_cast<Opcode>(from.opcode);
            value.position = from.position;
            for (unsigned i = 0; i < 3; ++i)
                value.children[i] = ValueID(from.children[i]);
            value.immediates = { from.immediates[0], from.immediates[1] };
            readValues(from.arguments, value.arguments);
            value.type = typeAt(from.type);
            if (!value.type) {
                m_failed = true;
                break;
            }
            if (from.prediction != Format::none && from.prediction & Format::PredictionIsClosure) {
                value.predictedClosure = FunctionID(from.prediction & ~Format::PredictionIsClosure);
                if (value.predictedClosure.index() >= m_header->functions.count) {
                    m_failed = true;
                    break;
                }
            } else
                value.prediction = typeAt(from.prediction);
            // The type a cast holds its operand to is the type of the cast, but for a coercion.
            if (value.opcode == Opcode::Cast) {
                if (from.immediates[1] > static_cast<uint32_t>(CastReason::Coerce))
                    m_failed = true;
                TypeID coercedTo = value.castReason() == CastReason::Coerce ? typeAt(from.immediates[0]) : TypeID();
                if (value.castReason() == CastReason::Coerce && !coercedTo)
                    m_failed = true;
                value.immediates[0] = coercedTo ? coercedTo.index() : Format::none;
                // Then `x!` on `undefined` or `null` says so and goes on, where there is room for them. Of all types without the two this is
                // the one that nothing but the cast makes sure of: any other is that of what an operation makes, or is tested for. So
                // with them back in it, no value is taken to be neither that may be either.
                if (value.castReason() == CastReason::NonNull && Options::reportAOTTypedCoercions()) [[unlikely]] {
                    Type type = m_program.at(value.type);
                    if (holdsCells(repOf(type))) {
                        type.primitives |= Primitive::UndefinedOrNull;
                        value.type = m_program.types.intern(WTF::move(type));
                    }
                }
            }
            value.rep = repOf(m_program.at(value.type));
            function.values.append(WTF::move(value));
        }
        for (auto& from : slice(m_blocks, source.blocks)) {
            Block block;
            readValues(from.parameters, block.parameters);
            readValues(from.values, block.values);
            block.terminator.kind = from.terminator;
            block.terminator.value = ValueID(from.value);
            for (auto& target : slice(m_targets, from.targets)) {
                Target result { BlockID(target.block), { } };
                readValues(target.arguments, result.arguments);
                block.terminator.targets.append(WTF::move(result));
            }
            auto cases = slice(m_words, from.cases);
            for (size_t i = 0; i + 1 < cases.size(); i += 2)
                block.terminator.cases.append(static_cast<int64_t>(static_cast<uint64_t>(cases[i + 1]) << 32 | cases[i]));
            function.blocks.append(WTF::move(block));
        }
        return function;
    }

    Program& m_program;
    std::span<const uint8_t> m_buffer;
    const Format::Header* m_header { nullptr };
    std::span<const uint32_t> m_words;
    std::span<const Format::ObjectPart> m_objectParts;
    // By the number the front end knows a type by.
    Vector<TypeID> m_types;
    std::span<const Format::Field> m_fields;
    std::span<const Format::Method> m_methods;
    std::span<const Format::Block> m_blocks;
    std::span<const Format::Value> m_values;
    std::span<const Format::Target> m_targets;
    // The class, and its variable.
    Vector<std::pair<LayoutID, uint32_t>> m_keysThatAreSymbols;
    bool m_failed { false };
};

std::unique_ptr<Program> Program::tryRead(std::span<const uint8_t> bytes, ASCIILiteral& whyFailed)
{
    auto program = makeUnique<Program>();
    program->buffer.append(bytes);
    whyFailed = Reader(*program).run();
    if (!whyFailed.isNull())
        return nullptr;
    return program;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
