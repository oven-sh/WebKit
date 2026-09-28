/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "Identifier.h"
#include "PythonOperators.h"
#include "PythonSignatures.h"
#include "PythonToken.h"
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class VM;

namespace Python {

// name is __name__.
#define FOR_EACH_PYTHON_DUNDER_NAME(v) \
    v(abs) v(abstractmethods) v(add) v(aenter) v(aexit) v(aiter) v(all) v(and) v(anext) v(annotate) v(annotate_func) v(annotations) v(annotations_cache) v(args) v(await) v(base) v(bases) v(bool) v(buffer) \
    v(build_class) v(builtins) v(bytes) v(call) v(cause) v(ceil) v(class) v(class_getitem) v(classcell) v(classdict) v(closure) \
    v(code) v(complex) v(conditional_annotations) v(contains) v(context) v(copy) v(debug) v(deepcopy) v(defaults) v(del) v(delattr) v(delete) v(delitem) v(dict) v(dir) v(divmod) v(doc) \
    v(enter) v(eq) v(exit) v(file) v(firstlineno) v(float) v(floor) v(floordiv) v(format) v(func) v(ge) v(get) v(getattr) v(getattribute) \
    v(getitem) v(globals) v(gt) v(hash) v(iadd) v(iand) v(ifloordiv) v(ilshift) v(imatmul) v(imod) v(import) v(imul) v(index) v(init) \
    v(init_subclass) v(instancecheck) v(int) v(invert) v(ior) v(ipow) v(irshift) v(isub) v(iter) v(itruediv) v(ixor) v(kwdefaults) \
    v(le) v(len) v(length_hint) v(loader) v(lshift) v(lt) v(main) v(match_args) v(matmul) v(missing) v(mod) v(module) v(mro) \
    v(mro_entries) v(mul) v(name) v(ne) v(neg) v(new) v(next) v(notes) v(objclass) v(or) v(orig_bases) v(orig_class) v(origin) v(package) v(parameters) v(path) v(pos) v(pow) \
    v(prepare) v(qualname) v(radd) v(rand) v(rdivmod) v(reduce) v(reduce_ex) v(release_buffer) v(repr) v(reversed) v(rfloordiv) v(rlshift) v(rmatmul) \
    v(rmod) v(rmul) v(ror) v(round) v(rpow) v(rrshift) v(rshift) v(rsub) v(rtruediv) v(rxor) v(self) v(set) v(set_name) v(setattr) \
    v(setitem) v(sizeof) v(slots) v(spec) v(static_attributes) v(str) v(sub) v(subclasscheck) v(subclasses) v(subclasshook) \
    v(suppress_context) v(traceback) v(truediv) v(trunc) v(type_params) v(typing_is_unpacked_typevartuple) v(typing_prepare_subst) v(typing_subst) v(typing_unpacked_tuple_args) \
    v(unpacked) v(weakref) v(wrapped) v(xor)

// Properties that Python cannot name, which hold what CPython keeps in the fields of a C struct.
#define FOR_EACH_PYTHON_PRIVATE_NAME(v) \
    v(dict) v(foreignDict) v(slots) v(class) v(capacity) v(descriptor) v(code) v(yieldFrom) v(promise) v(settlement) v(isRunningAsync) v(isClosedAsync) v(handled) v(frame) v(defaults) v(alignedDefaults) v(kwdefaults) v(name) v(qualname) v(doc) v(module) v(annotations) v(annotate) v(typeParams) \
    v(args) v(traceback) v(cause) v(context) v(suppressContext) v(notes) v(propertyName) v(isGettersDoc) v(line) v(fieldNames) v(hiddenFields) v(hasHooks) v(finalizer) v(origin)

// The same, for what the built-in exceptions have besides: what it is called here, and the attribute that Python sees it as.
#define FOR_EACH_PYTHON_EXCEPTION_FIELD(v) \
    v(value, "value") v(code, "code") v(message, "msg") v(filename, "filename") v(line, "lineno") v(offset, "offset") v(text, "text") \
    v(endLine, "end_lineno") v(endOffset, "end_offset") v(printFileAndLine, "print_file_and_line") v(name, "name") v(path, "path") \
    v(nameFrom, "name_from") v(object, "obj") v(errorNumber, "errno") v(errorText, "strerror") v(filename2, "filename2") \
    v(encoding, "encoding") v(subject, "object") v(start, "start") v(end, "end") v(reason, "reason") v(written, "characters_written") v(metadata, "_metadata")

struct CommonNames {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(CommonNames);

    explicit CommonNames(VM&);

#define DECLARE(name) const Identifier dunder_##name;
    FOR_EACH_PYTHON_DUNDER_NAME(DECLARE)
#undef DECLARE
#define DECLARE(name) const Identifier private_##name;
    FOR_EACH_PYTHON_PRIVATE_NAME(DECLARE)
#undef DECLARE
#define DECLARE(name, attribute) const Identifier field_##name;
    FOR_EACH_PYTHON_EXCEPTION_FIELD(DECLARE)
#undef DECLARE

    // The variables of the outermost environment of any code: the object whose properties are its global variables, which for the code of a module is
    // the module, and the one where a name that is not among them is looked for, which is nearly always the module builtins.
    const Identifier globals; // ".globals"
    const Identifier builtins; // ".builtins"

    // The private name that what is in a slot of an instance is a property under: one of __slots__. In CPython a slot is so many bytes into the instance, and
    // this goes by the same, so that two classes that are laid out alike have their slots in the same places, and an instance of one that is made an
    // instance of the other, by assigning to __class__, keeps what is in them.
    const Identifier& slotStorage(unsigned offset);

    // The signature that is written so, taken apart. There is one, for as long as the VM lasts.
    const NativeSignature* signatureFor(ASCIILiteral);

    // __add__, __radd__ and __iadd__ for Add.
    const Identifier& method(BinaryOperator op) const { return *m_binaryMethods[static_cast<unsigned>(op)]; }
    const Identifier& reflectedMethod(BinaryOperator op) const { return *m_reflectedMethods[static_cast<unsigned>(op)]; }
    const Identifier& inPlaceMethod(BinaryOperator op) const { return *m_inPlaceMethods[static_cast<unsigned>(op)]; }

private:
    const Identifier* m_binaryMethods[numberOfBinaryOperators];
    const Identifier* m_reflectedMethods[numberOfBinaryOperators];
    const Identifier* m_inPlaceMethods[numberOfBinaryOperators];
    UncheckedKeyHashMap<const void*, std::unique_ptr<NativeSignature>> m_signatures;
    UncheckedKeyHashMap<unsigned, Identifier, DefaultHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_slotStorage;
};

} } // namespace JSC::Python
