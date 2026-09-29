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

// lib/convert-opcode-metadata.py made this from the headers of CPython 3.14. It is not to be changed by hand. It is for PythonOpcodeModule.cpp and nothing else to include.

#pragma once

#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace Python { namespace Opcode {

// Include/internal/pycore_opcode_metadata.h
static constexpr uint16_t hasArg = 1;
static constexpr uint16_t hasConst = 2;
static constexpr uint16_t hasName = 4;
static constexpr uint16_t hasJump = 8;
static constexpr uint16_t hasFree = 16;
static constexpr uint16_t hasLocal = 32;
static constexpr uint16_t hasEvalBreak = 64;
static constexpr uint16_t hasDeopt = 128;
static constexpr uint16_t hasError = 256;
static constexpr uint16_t hasEscapes = 512;
static constexpr uint16_t hasExit = 1024;
static constexpr uint16_t hasPure = 2048;
static constexpr uint16_t hasPassthrough = 4096;
static constexpr uint16_t hasOpargAnd1 = 8192;
static constexpr uint16_t hasErrorNoPop = 16384;
static constexpr uint16_t hasNoSaveIp = 32768;

struct Metadata {
    bool isValid;
    uint16_t flags;
    uint8_t deoptimized; // _PyOpcode_Deopt: what it is a special case of, or itself.
};

static constexpr Metadata metadata[] = {
    { true, 0, 0 }, // 0 CACHE
    { true, 768, 1 }, // 1 BINARY_SLICE
    { true, 768, 2 }, // 2 BUILD_TEMPLATE
    { true, 1952, 44 }, // 3 BINARY_OP_INPLACE_ADD_UNICODE
    { true, 17216, 4 }, // 4 CALL_FUNCTION_EX
    { true, 768, 5 }, // 5 CHECK_EG_MATCH
    { true, 17152, 6 }, // 6 CHECK_EXC_MATCH
    { true, 17152, 7 }, // 7 CLEANUP_THROW
    { true, 768, 8 }, // 8 DELETE_SUBSCR
    { true, 33280, 9 }, // 9 END_FOR
    { true, 2560, 10 }, // 10 END_SEND
    { true, 17152, 11 }, // 11 EXIT_INIT_CHECK
    { true, 768, 12 }, // 12 FORMAT_SIMPLE
    { true, 768, 13 }, // 13 FORMAT_WITH_SPEC
    { true, 768, 14 }, // 14 GET_AITER
    { true, 17152, 15 }, // 15 GET_ANEXT
    { true, 768, 16 }, // 16 GET_ITER
    { true, 0, 17 }, // 17 RESERVED
    { true, 768, 18 }, // 18 GET_LEN
    { true, 17152, 19 }, // 19 GET_YIELD_FROM_ITER
    { true, 512, 20 }, // 20 INTERPRETER_EXIT
    { true, 768, 21 }, // 21 LOAD_BUILD_CLASS
    { true, 768, 22 }, // 22 LOAD_LOCALS
    { true, 768, 23 }, // 23 MAKE_FUNCTION
    { true, 768, 24 }, // 24 MATCH_KEYS
    { true, 0, 25 }, // 25 MATCH_MAPPING
    { true, 0, 26 }, // 26 MATCH_SEQUENCE
    { true, 2048, 27 }, // 27 NOP
    { true, 2048, 28 }, // 28 NOT_TAKEN
    { true, 512, 29 }, // 29 POP_EXCEPT
    { true, 2560, 30 }, // 30 POP_ITER
    { true, 2560, 31 }, // 31 POP_TOP
    { true, 0, 32 }, // 32 PUSH_EXC_INFO
    { true, 2048, 33 }, // 33 PUSH_NULL
    { true, 768, 34 }, // 34 RETURN_GENERATOR
    { true, 512, 35 }, // 35 RETURN_VALUE
    { true, 768, 36 }, // 36 SETUP_ANNOTATIONS
    { true, 768, 37 }, // 37 STORE_SLICE
    { true, 768, 38 }, // 38 STORE_SUBSCR
    { true, 768, 39 }, // 39 TO_BOOL
    { true, 768, 40 }, // 40 UNARY_INVERT
    { true, 768, 41 }, // 41 UNARY_NEGATIVE
    { true, 2048, 42 }, // 42 UNARY_NOT
    { true, 768, 43 }, // 43 WITH_EXCEPT_START
    { true, 17153, 44 }, // 44 BINARY_OP
    { true, 769, 45 }, // 45 BUILD_INTERPOLATION
    { true, 17153, 46 }, // 46 BUILD_LIST
    { true, 769, 47 }, // 47 BUILD_MAP
    { true, 769, 48 }, // 48 BUILD_SET
    { true, 257, 49 }, // 49 BUILD_SLICE
    { true, 257, 50 }, // 50 BUILD_STRING
    { true, 16641, 51 }, // 51 BUILD_TUPLE
    { true, 17217, 52 }, // 52 CALL
    { true, 769, 53 }, // 53 CALL_INTRINSIC_1
    { true, 769, 54 }, // 54 CALL_INTRINSIC_2
    { true, 17153, 55 }, // 55 CALL_KW
    { true, 769, 56 }, // 56 COMPARE_OP
    { true, 769, 57 }, // 57 CONTAINS_OP
    { true, 769, 58 }, // 58 CONVERT_VALUE
    { true, 2049, 59 }, // 59 COPY
    { true, 1, 60 }, // 60 COPY_FREE_VARS
    { true, 773, 61 }, // 61 DELETE_ATTR
    { true, 17169, 62 }, // 62 DELETE_DEREF
    { true, 801, 63 }, // 63 DELETE_FAST
    { true, 17157, 64 }, // 64 DELETE_GLOBAL
    { true, 17157, 65 }, // 65 DELETE_NAME
    { true, 769, 66 }, // 66 DICT_MERGE
    { true, 769, 67 }, // 67 DICT_UPDATE
    { true, 17161, 68 }, // 68 END_ASYNC_FOR
    { true, 1, 69 }, // 69 EXTENDED_ARG
    { true, 17161, 70 }, // 70 FOR_ITER
    { true, 769, 71 }, // 71 GET_AWAITABLE
    { true, 773, 72 }, // 72 IMPORT_FROM
    { true, 773, 73 }, // 73 IMPORT_NAME
    { true, 1, 74 }, // 74 IS_OP
    { true, 841, 75 }, // 75 JUMP_BACKWARD
    { true, 9, 76 }, // 76 JUMP_BACKWARD_NO_INTERRUPT
    { true, 9, 77 }, // 77 JUMP_FORWARD
    { true, 257, 78 }, // 78 LIST_APPEND
    { true, 769, 79 }, // 79 LIST_EXTEND
    { true, 773, 80 }, // 80 LOAD_ATTR
    { true, 1, 81 }, // 81 LOAD_COMMON_CONSTANT
    { true, 3, 82 }, // 82 LOAD_CONST
    { true, 801, 83 }, // 83 LOAD_DEREF
    { true, 2081, 84 }, // 84 LOAD_FAST
    { true, 33, 85 }, // 85 LOAD_FAST_AND_CLEAR
    { true, 2081, 86 }, // 86 LOAD_FAST_BORROW
    { true, 33, 87 }, // 87 LOAD_FAST_BORROW_LOAD_FAST_BORROW
    { true, 801, 88 }, // 88 LOAD_FAST_CHECK
    { true, 33, 89 }, // 89 LOAD_FAST_LOAD_FAST
    { true, 17169, 90 }, // 90 LOAD_FROM_DICT_OR_DEREF
    { true, 17157, 91 }, // 91 LOAD_FROM_DICT_OR_GLOBALS
    { true, 773, 92 }, // 92 LOAD_GLOBAL
    { true, 773, 93 }, // 93 LOAD_NAME
    { true, 1, 94 }, // 94 LOAD_SMALL_INT
    { true, 17153, 95 }, // 95 LOAD_SPECIAL
    { true, 773, 96 }, // 96 LOAD_SUPER_ATTR
    { true, 17169, 97 }, // 97 MAKE_CELL
    { true, 769, 98 }, // 98 MAP_ADD
    { true, 769, 99 }, // 99 MATCH_CLASS
    { true, 9, 100 }, // 100 POP_JUMP_IF_FALSE
    { true, 9, 101 }, // 101 POP_JUMP_IF_NONE
    { true, 9, 102 }, // 102 POP_JUMP_IF_NOT_NONE
    { true, 9, 103 }, // 103 POP_JUMP_IF_TRUE
    { true, 17153, 104 }, // 104 RAISE_VARARGS
    { true, 17153, 105 }, // 105 RERAISE
    { true, 777, 106 }, // 106 SEND
    { true, 769, 107 }, // 107 SET_ADD
    { true, 769, 108 }, // 108 SET_FUNCTION_ATTRIBUTE
    { true, 769, 109 }, // 109 SET_UPDATE
    { true, 773, 110 }, // 110 STORE_ATTR
    { true, 529, 111 }, // 111 STORE_DEREF
    { true, 545, 112 }, // 112 STORE_FAST
    { true, 545, 113 }, // 113 STORE_FAST_LOAD_FAST
    { true, 545, 114 }, // 114 STORE_FAST_STORE_FAST
    { true, 773, 115 }, // 115 STORE_GLOBAL
    { true, 773, 116 }, // 116 STORE_NAME
    { true, 2049, 117 }, // 117 SWAP
    { true, 769, 118 }, // 118 UNPACK_EX
    { true, 769, 119 }, // 119 UNPACK_SEQUENCE
    { true, 513, 120 }, // 120 YIELD_VALUE
    { false, 0, 121 }, // 121
    { false, 0, 122 }, // 122
    { false, 0, 123 }, // 123
    { false, 0, 124 }, // 124
    { false, 0, 125 }, // 125
    { false, 0, 126 }, // 126
    { false, 0, 127 }, // 127
    { true, 17217, 128 }, // 128 RESUME
    { true, 1280, 44 }, // 129 BINARY_OP_ADD_FLOAT
    { true, 1792, 44 }, // 130 BINARY_OP_ADD_INT
    { true, 1280, 44 }, // 131 BINARY_OP_ADD_UNICODE
    { true, 896, 44 }, // 132 BINARY_OP_EXTEND
    { true, 1280, 44 }, // 133 BINARY_OP_MULTIPLY_FLOAT
    { true, 1792, 44 }, // 134 BINARY_OP_MULTIPLY_INT
    { true, 1792, 44 }, // 135 BINARY_OP_SUBSCR_DICT
    { true, 128, 44 }, // 136 BINARY_OP_SUBSCR_GETITEM
    { true, 1664, 44 }, // 137 BINARY_OP_SUBSCR_LIST_INT
    { true, 1792, 44 }, // 138 BINARY_OP_SUBSCR_LIST_SLICE
    { true, 1664, 44 }, // 139 BINARY_OP_SUBSCR_STR_INT
    { true, 1152, 44 }, // 140 BINARY_OP_SUBSCR_TUPLE_INT
    { true, 1280, 44 }, // 141 BINARY_OP_SUBTRACT_FLOAT
    { true, 1792, 44 }, // 142 BINARY_OP_SUBTRACT_INT
    { true, 17281, 52 }, // 143 CALL_ALLOC_AND_ENTER_INIT
    { true, 1665, 52 }, // 144 CALL_BOUND_METHOD_EXACT_ARGS
    { true, 18305, 52 }, // 145 CALL_BOUND_METHOD_GENERAL
    { true, 961, 52 }, // 146 CALL_BUILTIN_CLASS
    { true, 961, 52 }, // 147 CALL_BUILTIN_FAST
    { true, 961, 52 }, // 148 CALL_BUILTIN_FAST_WITH_KEYWORDS
    { true, 1857, 52 }, // 149 CALL_BUILTIN_O
    { true, 17281, 52 }, // 150 CALL_ISINSTANCE
    { true, 1921, 55 }, // 151 CALL_KW_BOUND_METHOD
    { true, 1857, 55 }, // 152 CALL_KW_NON_PY
    { true, 1921, 55 }, // 153 CALL_KW_PY
    { true, 17280, 52 }, // 154 CALL_LEN
    { true, 897, 52 }, // 155 CALL_LIST_APPEND
    { true, 1857, 52 }, // 156 CALL_METHOD_DESCRIPTOR_FAST
    { true, 1857, 52 }, // 157 CALL_METHOD_DESCRIPTOR_FAST_WITH_KEYWORDS
    { true, 1857, 52 }, // 158 CALL_METHOD_DESCRIPTOR_NOARGS
    { true, 1857, 52 }, // 159 CALL_METHOD_DESCRIPTOR_O
    { true, 1857, 52 }, // 160 CALL_NON_PY_GENERAL
    { true, 1153, 52 }, // 161 CALL_PY_EXACT_ARGS
    { true, 18305, 52 }, // 162 CALL_PY_GENERAL
    { true, 961, 52 }, // 163 CALL_STR_1
    { true, 961, 52 }, // 164 CALL_TUPLE_1
    { true, 641, 52 }, // 165 CALL_TYPE_1
    { true, 1025, 56 }, // 166 COMPARE_OP_FLOAT
    { true, 1153, 56 }, // 167 COMPARE_OP_INT
    { true, 1025, 56 }, // 168 COMPARE_OP_STR
    { true, 1793, 57 }, // 169 CONTAINS_OP_DICT
    { true, 897, 57 }, // 170 CONTAINS_OP_SET
    { true, 129, 70 }, // 171 FOR_ITER_GEN
    { true, 1673, 70 }, // 172 FOR_ITER_LIST
    { true, 1289, 70 }, // 173 FOR_ITER_RANGE
    { true, 1545, 70 }, // 174 FOR_ITER_TUPLE
    { true, 841, 75 }, // 175 JUMP_BACKWARD_JIT
    { true, 841, 75 }, // 176 JUMP_BACKWARD_NO_JIT
    { true, 1025, 80 }, // 177 LOAD_ATTR_CLASS
    { true, 1025, 80 }, // 178 LOAD_ATTR_CLASS_WITH_METACLASS_CHECK
    { true, 133, 80 }, // 179 LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN
    { true, 1665, 80 }, // 180 LOAD_ATTR_INSTANCE_VALUE
    { true, 1153, 80 }, // 181 LOAD_ATTR_METHOD_LAZY_DICT
    { true, 1025, 80 }, // 182 LOAD_ATTR_METHOD_NO_DICT
    { true, 1153, 80 }, // 183 LOAD_ATTR_METHOD_WITH_VALUES
    { true, 641, 80 }, // 184 LOAD_ATTR_MODULE
    { true, 1537, 80 }, // 185 LOAD_ATTR_NONDESCRIPTOR_NO_DICT
    { true, 1665, 80 }, // 186 LOAD_ATTR_NONDESCRIPTOR_WITH_VALUES
    { true, 1153, 80 }, // 187 LOAD_ATTR_PROPERTY
    { true, 1153, 80 }, // 188 LOAD_ATTR_SLOT
    { true, 1669, 80 }, // 189 LOAD_ATTR_WITH_HINT
    { true, 3, 82 }, // 190 LOAD_CONST_IMMORTAL
    { true, 3, 82 }, // 191 LOAD_CONST_MORTAL
    { true, 129, 92 }, // 192 LOAD_GLOBAL_BUILTIN
    { true, 129, 92 }, // 193 LOAD_GLOBAL_MODULE
    { true, 901, 96 }, // 194 LOAD_SUPER_ATTR_ATTR
    { true, 17285, 96 }, // 195 LOAD_SUPER_ATTR_METHOD
    { true, 128, 128 }, // 196 RESUME_CHECK
    { true, 129, 106 }, // 197 SEND_GEN
    { true, 1536, 110 }, // 198 STORE_ATTR_INSTANCE_VALUE
    { true, 1664, 110 }, // 199 STORE_ATTR_SLOT
    { true, 1669, 110 }, // 200 STORE_ATTR_WITH_HINT
    { true, 1792, 38 }, // 201 STORE_SUBSCR_DICT
    { true, 1664, 38 }, // 202 STORE_SUBSCR_LIST_INT
    { true, 1536, 39 }, // 203 TO_BOOL_ALWAYS_TRUE
    { true, 1024, 39 }, // 204 TO_BOOL_BOOL
    { true, 1536, 39 }, // 205 TO_BOOL_INT
    { true, 1024, 39 }, // 206 TO_BOOL_LIST
    { true, 1024, 39 }, // 207 TO_BOOL_NONE
    { true, 1536, 39 }, // 208 TO_BOOL_STR
    { true, 1153, 119 }, // 209 UNPACK_SEQUENCE_LIST
    { true, 1153, 119 }, // 210 UNPACK_SEQUENCE_TUPLE
    { true, 1665, 119 }, // 211 UNPACK_SEQUENCE_TWO_TUPLE
    { false, 0, 212 }, // 212
    { false, 0, 213 }, // 213
    { false, 0, 214 }, // 214
    { false, 0, 215 }, // 215
    { false, 0, 216 }, // 216
    { false, 0, 217 }, // 217
    { false, 0, 218 }, // 218
    { false, 0, 219 }, // 219
    { false, 0, 220 }, // 220
    { false, 0, 221 }, // 221
    { false, 0, 222 }, // 222
    { false, 0, 223 }, // 223
    { false, 0, 224 }, // 224
    { false, 0, 225 }, // 225
    { false, 0, 226 }, // 226
    { false, 0, 227 }, // 227
    { false, 0, 228 }, // 228
    { false, 0, 229 }, // 229
    { false, 0, 230 }, // 230
    { false, 0, 231 }, // 231
    { false, 0, 232 }, // 232
    { false, 0, 233 }, // 233
    { true, 49920, 234 }, // 234 INSTRUMENTED_END_FOR
    { true, 512, 235 }, // 235 INSTRUMENTED_POP_ITER
    { true, 17152, 236 }, // 236 INSTRUMENTED_END_SEND
    { true, 17161, 237 }, // 237 INSTRUMENTED_FOR_ITER
    { true, 768, 238 }, // 238 INSTRUMENTED_INSTRUCTION
    { true, 1, 239 }, // 239 INSTRUMENTED_JUMP_FORWARD
    { true, 0, 240 }, // 240 INSTRUMENTED_NOT_TAKEN
    { true, 1, 241 }, // 241 INSTRUMENTED_POP_JUMP_IF_TRUE
    { true, 1, 242 }, // 242 INSTRUMENTED_POP_JUMP_IF_FALSE
    { true, 513, 243 }, // 243 INSTRUMENTED_POP_JUMP_IF_NONE
    { true, 513, 244 }, // 244 INSTRUMENTED_POP_JUMP_IF_NOT_NONE
    { true, 17217, 245 }, // 245 INSTRUMENTED_RESUME
    { true, 768, 246 }, // 246 INSTRUMENTED_RETURN_VALUE
    { true, 17153, 247 }, // 247 INSTRUMENTED_YIELD_VALUE
    { true, 17161, 248 }, // 248 INSTRUMENTED_END_ASYNC_FOR
    { true, 773, 249 }, // 249 INSTRUMENTED_LOAD_SUPER_ATTR
    { true, 17217, 250 }, // 250 INSTRUMENTED_CALL
    { true, 17153, 251 }, // 251 INSTRUMENTED_CALL_KW
    { true, 17216, 252 }, // 252 INSTRUMENTED_CALL_FUNCTION_EX
    { true, 833, 253 }, // 253 INSTRUMENTED_JUMP_BACKWARD
    { true, 512, 254 }, // 254 INSTRUMENTED_LINE
    { true, 1, 255 }, // 255 ENTER_EXECUTOR
    { true, 2048, 0 }, // 256 ANNOTATIONS_PLACEHOLDER
    { true, 841, 0 }, // 257 JUMP
    { true, 777, 0 }, // 258 JUMP_IF_FALSE
    { true, 777, 0 }, // 259 JUMP_IF_TRUE
    { true, 9, 0 }, // 260 JUMP_NO_INTERRUPT
    { true, 2081, 0 }, // 261 LOAD_CLOSURE
    { true, 2048, 0 }, // 262 POP_BLOCK
    { true, 2049, 0 }, // 263 SETUP_CLEANUP
    { true, 2049, 0 }, // 264 SETUP_FINALLY
    { true, 2049, 0 }, // 265 SETUP_WITH
    { true, 545, 0 }, // 266 STORE_FAST_MAYBE_NULL
};

// _PyOpcode_num_popped()
inline int numberPopped(int opcode, [[maybe_unused]] int oparg)
{
    switch (opcode) {
    case 0: case 17: case 21: case 22: case 27: case 28: case 33: case 34: case 36: case 60: case 62: case 63:
    case 64: case 65: case 69: case 75: case 76: case 77: case 81: case 82: case 83: case 84: case 85: case 86:
    case 87: case 88: case 89: case 92: case 93: case 94: case 97: case 128: case 175: case 176: case 190: case 191:
    case 192: case 193: case 196: case 238: case 239: case 240: case 245: case 253: case 254: case 255: case 256:
    case 257: case 260: case 261: case 262: case 263: case 264: case 265:
        return 0;
    case 2: case 3: case 5: case 6: case 8: case 10: case 13: case 24: case 44: case 54: case 56: case 57: case 68:
    case 73: case 74: case 106: case 108: case 110: case 114: case 129: case 130: case 131: case 132: case 133:
    case 134: case 135: case 136: case 137: case 138: case 139: case 140: case 141: case 142: case 166: case 167:
    case 168: case 169: case 170: case 197: case 198: case 199: case 200: case 234: case 236: case 248:
        return 2;
    case 1: case 7: case 38: case 96: case 99: case 154: case 155: case 163: case 164: case 165: case 194: case 195:
    case 201: case 202: case 249:
        return 3;
    case 45:
        return 2 + (oparg & 1);
    case 46: case 48: case 49: case 50: case 51: case 104:
        return oparg;
    case 47:
        return oparg*2;
    case 52: case 143: case 144: case 145: case 146: case 147: case 148: case 149: case 150: case 156: case 157:
    case 158: case 159: case 160: case 161: case 162: case 250:
        return 2 + oparg;
    case 4: case 37: case 252:
        return 4;
    case 9: case 11: case 12: case 14: case 15: case 16: case 18: case 19: case 20: case 23: case 25: case 26:
    case 29: case 30: case 31: case 32: case 35: case 39: case 40: case 41: case 42: case 53: case 58: case 61:
    case 70: case 71: case 72: case 80: case 90: case 91: case 95: case 100: case 101: case 102: case 103: case 111:
    case 112: case 113: case 115: case 116: case 118: case 119: case 120: case 171: case 172: case 173: case 174:
    case 177: case 178: case 179: case 180: case 181: case 182: case 183: case 184: case 185: case 186: case 187:
    case 188: case 189: case 203: case 204: case 205: case 206: case 207: case 208: case 209: case 210: case 211:
    case 235: case 237: case 241: case 242: case 243: case 244: case 246: case 247: case 258: case 259: case 266:
        return 1;
    case 55: case 151: case 152: case 153: case 251:
        return 3 + oparg;
    case 59:
        return 1 + (oparg-1);
    case 66:
        return 5 + (oparg - 1);
    case 67:
        return 2 + (oparg - 1);
    case 78: case 79: case 107: case 109:
        return 2 + (oparg-1);
    case 98:
        return 3 + (oparg - 1);
    case 105:
        return 1 + oparg;
    case 117:
        return 2 + (oparg-2);
    case 43:
        return 5;
    default:
        return -1;
    }
}

// _PyOpcode_num_pushed()
inline int numberPushed(int opcode, [[maybe_unused]] int oparg)
{
    switch (opcode) {
    case 0: case 3: case 8: case 9: case 11: case 17: case 20: case 27: case 28: case 29: case 30: case 31: case 36:
    case 37: case 38: case 60: case 61: case 62: case 63: case 64: case 65: case 68: case 69: case 75: case 76:
    case 77: case 97: case 100: case 101: case 102: case 103: case 104: case 110: case 111: case 112: case 114:
    case 115: case 116: case 128: case 136: case 143: case 144: case 145: case 151: case 153: case 155: case 161:
    case 162: case 175: case 176: case 187: case 196: case 198: case 199: case 200: case 201: case 202: case 235:
    case 238: case 239: case 240: case 241: case 242: case 243: case 244: case 245: case 248: case 253: case 254:
    case 255: case 256: case 257: case 260: case 262: case 266:
        return 0;
    case 1: case 2: case 4: case 10: case 12: case 13: case 14: case 16: case 19: case 21: case 22: case 23: case 33:
    case 34: case 35: case 39: case 40: case 41: case 42: case 44: case 45: case 46: case 47: case 48: case 49:
    case 50: case 51: case 52: case 53: case 54: case 55: case 56: case 57: case 58: case 71: case 73: case 74:
    case 81: case 82: case 83: case 84: case 85: case 86: case 88: case 90: case 91: case 93: case 94: case 99:
    case 108: case 113: case 120: case 129: case 130: case 131: case 132: case 133: case 134: case 135: case 137:
    case 138: case 139: case 140: case 141: case 142: case 146: case 147: case 148: case 149: case 150: case 152:
    case 154: case 156: case 157: case 158: case 159: case 160: case 163: case 164: case 165: case 166: case 167:
    case 168: case 169: case 170: case 171: case 179: case 185: case 186: case 190: case 191: case 194: case 197:
    case 203: case 204: case 205: case 206: case 207: case 208: case 234: case 236: case 246: case 247: case 250:
    case 251: case 252: case 258: case 259: case 261: case 264: case 265:
        return 1;
    case 5: case 6: case 7: case 15: case 18: case 25: case 26: case 32: case 70: case 72: case 87: case 89: case 95:
    case 106: case 172: case 173: case 174: case 181: case 182: case 183: case 195: case 211: case 237: case 263:
        return 2;
    case 59:
        return 2 + (oparg-1);
    case 66:
        return 4 + (oparg - 1);
    case 67: case 98:
        return 1 + (oparg - 1);
    case 92: case 96: case 177: case 178: case 180: case 184: case 188: case 189: case 192: case 193: case 249:
        return 1 + (oparg & 1);
    case 78: case 79: case 107: case 109:
        return 1 + (oparg-1);
    case 80:
        return 1 + (oparg&1);
    case 24:
        return 3;
    case 105: case 119: case 209: case 210:
        return oparg;
    case 117:
        return 2 + (oparg-2);
    case 118:
        return 1 + (oparg & 0xFF) + (oparg >> 8);
    case 43:
        return 6;
    default:
        return -1;
    }
}

// Include/internal/pycore_opcode_utils.h
static constexpr int maximumRealOpcode = 254;
inline bool isBlockPush(int opcode) { return opcode == 264 || opcode == 265 || opcode == 263; } // SETUP_FINALLY, SETUP_WITH and SETUP_CLEANUP

// Include/internal/pycore_intrinsics.h
static constexpr ASCIILiteral unaryIntrinsics[] = {
    "INTRINSIC_1_INVALID"_s,
    "INTRINSIC_PRINT"_s,
    "INTRINSIC_IMPORT_STAR"_s,
    "INTRINSIC_STOPITERATION_ERROR"_s,
    "INTRINSIC_ASYNC_GEN_WRAP"_s,
    "INTRINSIC_UNARY_POSITIVE"_s,
    "INTRINSIC_LIST_TO_TUPLE"_s,
    "INTRINSIC_TYPEVAR"_s,
    "INTRINSIC_PARAMSPEC"_s,
    "INTRINSIC_TYPEVARTUPLE"_s,
    "INTRINSIC_SUBSCRIPT_GENERIC"_s,
    "INTRINSIC_TYPEALIAS"_s,
};

static constexpr ASCIILiteral binaryIntrinsics[] = {
    "INTRINSIC_2_INVALID"_s,
    "INTRINSIC_PREP_RERAISE_STAR"_s,
    "INTRINSIC_TYPEVAR_WITH_BOUND"_s,
    "INTRINSIC_TYPEVAR_WITH_CONSTRAINTS"_s,
    "INTRINSIC_SET_FUNCTION_TYPE_PARAMS"_s,
    "INTRINSIC_SET_TYPEPARAM_DEFAULT"_s,
};

// Include/internal/pycore_ceval.h
static constexpr ASCIILiteral specialMethods[] = {
    "__enter__"_s,
    "__exit__"_s,
    "__aenter__"_s,
    "__aexit__"_s,
};

// Include/opcode.h and Modules/_opcode.c
static constexpr std::pair<ASCIILiteral, ASCIILiteral> binaryOperators[] = {
    { "NB_ADD"_s, "+"_s },
    { "NB_AND"_s, "&"_s },
    { "NB_FLOOR_DIVIDE"_s, "//"_s },
    { "NB_LSHIFT"_s, "<<"_s },
    { "NB_MATRIX_MULTIPLY"_s, "@"_s },
    { "NB_MULTIPLY"_s, "*"_s },
    { "NB_REMAINDER"_s, "%"_s },
    { "NB_OR"_s, "|"_s },
    { "NB_POWER"_s, "**"_s },
    { "NB_RSHIFT"_s, ">>"_s },
    { "NB_SUBTRACT"_s, "-"_s },
    { "NB_TRUE_DIVIDE"_s, "/"_s },
    { "NB_XOR"_s, "^"_s },
    { "NB_INPLACE_ADD"_s, "+="_s },
    { "NB_INPLACE_AND"_s, "&="_s },
    { "NB_INPLACE_FLOOR_DIVIDE"_s, "//="_s },
    { "NB_INPLACE_LSHIFT"_s, "<<="_s },
    { "NB_INPLACE_MATRIX_MULTIPLY"_s, "@="_s },
    { "NB_INPLACE_MULTIPLY"_s, "*="_s },
    { "NB_INPLACE_REMAINDER"_s, "%="_s },
    { "NB_INPLACE_OR"_s, "|="_s },
    { "NB_INPLACE_POWER"_s, "**="_s },
    { "NB_INPLACE_RSHIFT"_s, ">>="_s },
    { "NB_INPLACE_SUBTRACT"_s, "-="_s },
    { "NB_INPLACE_TRUE_DIVIDE"_s, "/="_s },
    { "NB_INPLACE_XOR"_s, "^="_s },
    { "NB_SUBSCR"_s, "[]"_s },
};

} } } // namespace JSC::Python::Opcode
