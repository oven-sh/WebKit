/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyBuffer.h"
#include "PyBytes.h"
#include "PyException.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyModuleHelpers.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/RefCounted.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringHash.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module pyexpat, which is xml.parsers.expat: a non-validating parser for XML 1.0 with namespaces, fed piece by piece, that
// calls handlers. CPython has the library Expat for it. This is a parser written from the specifications, which takes after
// Expat in what a program can tell: into which tokens the text falls (a handler is called for each), the order of the checks,
// the codes, messages and positions of errors. (Modules/pyexpat.c in CPython is the reference for the interface.)
//
// The input is decoded to UTF-16 as it comes, and that is what is parsed. Positions are reckoned from the text that has been
// passed over, in the units of the encoding it came in.

enum ExpatError : int {
    ExpatErrorNone,
    ExpatErrorNoMemory,
    ExpatErrorSyntax,
    ExpatErrorNoElements,
    ExpatErrorInvalidToken,
    ExpatErrorUnclosedToken,
    ExpatErrorPartialChar,
    ExpatErrorTagMismatch,
    ExpatErrorDuplicateAttribute,
    ExpatErrorJunkAfterDocElement,
    ExpatErrorParamEntityRef,
    ExpatErrorUndefinedEntity,
    ExpatErrorRecursiveEntityRef,
    ExpatErrorAsyncEntity,
    ExpatErrorBadCharRef,
    ExpatErrorBinaryEntityRef,
    ExpatErrorAttributeExternalEntityRef,
    ExpatErrorMisplacedXmlPi,
    ExpatErrorUnknownEncoding,
    ExpatErrorIncorrectEncoding,
    ExpatErrorUnclosedCdataSection,
    ExpatErrorExternalEntityHandling,
    ExpatErrorNotStandalone,
    ExpatErrorUnexpectedState,
    ExpatErrorEntityDeclaredInPe,
    ExpatErrorFeatureRequiresXmlDtd,
    ExpatErrorCantChangeFeatureOnceParsing,
    ExpatErrorUnboundPrefix,
    ExpatErrorUndeclaringPrefix,
    ExpatErrorIncompletePe,
    ExpatErrorXmlDecl,
    ExpatErrorTextDecl,
    ExpatErrorPublicId,
    ExpatErrorSuspended,
    ExpatErrorNotSuspended,
    ExpatErrorAborted,
    ExpatErrorFinished,
    ExpatErrorSuspendPe,
    ExpatErrorReservedPrefixXml,
    ExpatErrorReservedPrefixXmlns,
    ExpatErrorReservedNamespaceUri,
    ExpatErrorInvalidArgument,
    ExpatErrorNoBuffer,
    ExpatErrorAmplificationLimitBreach,
    ExpatErrorNotStarted,
    ExpatErrorCount
};

struct ExpatErrorInfo {
    ASCIILiteral name;
    ASCIILiteral message;
};

static constexpr ExpatErrorInfo expatErrors[] = {
    { { }, { } },
    { "XML_ERROR_NO_MEMORY"_s, "out of memory"_s },
    { "XML_ERROR_SYNTAX"_s, "syntax error"_s },
    { "XML_ERROR_NO_ELEMENTS"_s, "no element found"_s },
    { "XML_ERROR_INVALID_TOKEN"_s, "not well-formed (invalid token)"_s },
    { "XML_ERROR_UNCLOSED_TOKEN"_s, "unclosed token"_s },
    { "XML_ERROR_PARTIAL_CHAR"_s, "partial character"_s },
    { "XML_ERROR_TAG_MISMATCH"_s, "mismatched tag"_s },
    { "XML_ERROR_DUPLICATE_ATTRIBUTE"_s, "duplicate attribute"_s },
    { "XML_ERROR_JUNK_AFTER_DOC_ELEMENT"_s, "junk after document element"_s },
    { "XML_ERROR_PARAM_ENTITY_REF"_s, "illegal parameter entity reference"_s },
    { "XML_ERROR_UNDEFINED_ENTITY"_s, "undefined entity"_s },
    { "XML_ERROR_RECURSIVE_ENTITY_REF"_s, "recursive entity reference"_s },
    { "XML_ERROR_ASYNC_ENTITY"_s, "asynchronous entity"_s },
    { "XML_ERROR_BAD_CHAR_REF"_s, "reference to invalid character number"_s },
    { "XML_ERROR_BINARY_ENTITY_REF"_s, "reference to binary entity"_s },
    { "XML_ERROR_ATTRIBUTE_EXTERNAL_ENTITY_REF"_s, "reference to external entity in attribute"_s },
    { "XML_ERROR_MISPLACED_XML_PI"_s, "XML or text declaration not at start of entity"_s },
    { "XML_ERROR_UNKNOWN_ENCODING"_s, "unknown encoding"_s },
    { "XML_ERROR_INCORRECT_ENCODING"_s, "encoding specified in XML declaration is incorrect"_s },
    { "XML_ERROR_UNCLOSED_CDATA_SECTION"_s, "unclosed CDATA section"_s },
    { "XML_ERROR_EXTERNAL_ENTITY_HANDLING"_s, "error in processing external entity reference"_s },
    { "XML_ERROR_NOT_STANDALONE"_s, "document is not standalone"_s },
    { "XML_ERROR_UNEXPECTED_STATE"_s, "unexpected parser state - please send a bug report"_s },
    { "XML_ERROR_ENTITY_DECLARED_IN_PE"_s, "entity declared in parameter entity"_s },
    { "XML_ERROR_FEATURE_REQUIRES_XML_DTD"_s, "requested feature requires XML_DTD support in Expat"_s },
    { "XML_ERROR_CANT_CHANGE_FEATURE_ONCE_PARSING"_s, "cannot change setting once parsing has begun"_s },
    { "XML_ERROR_UNBOUND_PREFIX"_s, "unbound prefix"_s },
    { "XML_ERROR_UNDECLARING_PREFIX"_s, "must not undeclare prefix"_s },
    { "XML_ERROR_INCOMPLETE_PE"_s, "incomplete markup in parameter entity"_s },
    { "XML_ERROR_XML_DECL"_s, "XML declaration not well-formed"_s },
    { "XML_ERROR_TEXT_DECL"_s, "text declaration not well-formed"_s },
    { "XML_ERROR_PUBLICID"_s, "illegal character(s) in public id"_s },
    { "XML_ERROR_SUSPENDED"_s, "parser suspended"_s },
    { "XML_ERROR_NOT_SUSPENDED"_s, "parser not suspended"_s },
    { "XML_ERROR_ABORTED"_s, "parsing aborted"_s },
    { "XML_ERROR_FINISHED"_s, "parsing finished"_s },
    { "XML_ERROR_SUSPEND_PE"_s, "cannot suspend in external parameter entity"_s },
    { "XML_ERROR_RESERVED_PREFIX_XML"_s, "reserved prefix (xml) must not be undeclared or bound to another namespace name"_s },
    { "XML_ERROR_RESERVED_PREFIX_XMLNS"_s, "reserved prefix (xmlns) must not be declared or undeclared"_s },
    { "XML_ERROR_RESERVED_NAMESPACE_URI"_s, "prefix must not be bound to one of the reserved namespace names"_s },
    { "XML_ERROR_INVALID_ARGUMENT"_s, "invalid argument"_s },
    { "XML_ERROR_NO_BUFFER"_s, "a successful prior call to function XML_GetBuffer is required"_s },
    { "XML_ERROR_AMPLIFICATION_LIMIT_BREACH"_s, "limit on input amplification factor (from DTD and entities) breached"_s },
    { "XML_ERROR_NOT_STARTED"_s, "parser not started"_s },
};
static_assert(std::size(expatErrors) == ExpatErrorCount);

// The handlers, in the order in which the fields of the parser object hold them.
#define FOR_EACH_EXPAT_HANDLER(v) \
    v(StartElement, "StartElementHandler") \
    v(EndElement, "EndElementHandler") \
    v(ProcessingInstruction, "ProcessingInstructionHandler") \
    v(CharacterData, "CharacterDataHandler") \
    v(UnparsedEntityDecl, "UnparsedEntityDeclHandler") \
    v(NotationDecl, "NotationDeclHandler") \
    v(StartNamespaceDecl, "StartNamespaceDeclHandler") \
    v(EndNamespaceDecl, "EndNamespaceDeclHandler") \
    v(Comment, "CommentHandler") \
    v(StartCdataSection, "StartCdataSectionHandler") \
    v(EndCdataSection, "EndCdataSectionHandler") \
    v(Default, "DefaultHandler") \
    v(DefaultExpand, "DefaultHandlerExpand") \
    v(NotStandalone, "NotStandaloneHandler") \
    v(ExternalEntityRef, "ExternalEntityRefHandler") \
    v(StartDoctypeDecl, "StartDoctypeDeclHandler") \
    v(EndDoctypeDecl, "EndDoctypeDeclHandler") \
    v(EntityDecl, "EntityDeclHandler") \
    v(XmlDecl, "XmlDeclHandler") \
    v(ElementDecl, "ElementDeclHandler") \
    v(AttlistDecl, "AttlistDeclHandler") \
    v(SkippedEntity, "SkippedEntityHandler")

enum ExpatField : unsigned {
#define EXPAT_DEFINE_FIELD(id, name) ExpatHandler##id,
    FOR_EACH_EXPAT_HANDLER(EXPAT_DEFINE_FIELD)
#undef EXPAT_DEFINE_FIELD
    ExpatHandlerCount,
    ExpatIntern = ExpatHandlerCount, // A dict, or empty.
    ExpatParent, // What ExternalEntityParserCreate() was called on.
    ExpatFieldCount
}; // number: the ExpatParser

static constexpr TypeDef expatErrorTypeDef { "xml.parsers.expat.ExpatError"_s, { TypeFlag::Base, TypeFlag::HasDict, TypeFlag::Mutable, TypeFlag::HasWeakref }, { }, { }, { } };

static PyType* expatErrorType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return rt.typeFor(expatErrorTypeDef, rt.Exception());
}

// ---- The document type declaration, as far as a parser that does not validate cares ----

struct ExpatEntity {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(ExpatEntity);
    String name;
    Vector<char16_t> text; // The replacement text,
    bool hasText { false }; // if it is an internal entity.
    String systemId;
    String publicId;
    String base;
    String notation;
    bool isParameter { false };
    bool isInternal { false }; // Declared in the document itself, not in an entity.
    bool isOpen { false };
};
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(ExpatEntity);

struct ExpatAttributeDeclaration {
    String name;
    bool isCData { true };
    String value; // The default. Null: none.
};

struct ExpatElementType {
    Vector<ExpatAttributeDeclaration> attributes;
};

class ExpatDTD : public RefCounted<ExpatDTD> {
    WTF_MAKE_TZONE_ALLOCATED(ExpatDTD);
public:
    static Ref<ExpatDTD> create() { return adoptRef(*new ExpatDTD); }

    HashMap<String, std::unique_ptr<ExpatEntity>> generalEntities;
    HashMap<String, std::unique_ptr<ExpatEntity>> parameterEntities;
    HashMap<String, ExpatElementType> elementTypes;
    bool keepProcessing { true }; // False once something that could not be read may have declared anything.
    bool hasParamEntityRefs { false };
    bool isStandalone { false };
    bool paramEntityRead { false };

    // What the document has cost, with the parsers of its external entities. Beyond a threshold, what comes of the input may be
    // no more than so many times the input.
    uint64_t directBytes { 0 }; // The document itself.
    uint64_t indirectBytes { 0 }; // What entities have been replaced by.
    uint64_t expansionThreshold { 8 * 1024 * 1024 };
    float maximumExpansion { 100 };
    uint64_t storedBytes { 4096 }; // An estimate of what is kept: declarations, open elements.
    uint64_t storageThreshold { 64 * 1024 * 1024 };
    float maximumStorage { 100 };
};
WTF_MAKE_TZONE_ALLOCATED_IMPL(ExpatDTD);

// A node of the content model of an element type. They are kept in a row, not in one another: a model may be nested very deeply.
struct ExpatContentNode {
    int type { 0 };
    int quantifier { 0 };
    String name;
    int firstChild { -1 };
    int lastChild { -1 };
    int nextSibling { -1 };
    unsigned childCount { 0 };
};

enum class ExpatTok : uint8_t {
    // Of these there is nothing to report.
    None, // The end of the text.
    Invalid,
    Partial, // It may be a token when there is more text.
    PartialChar,
    TrailingCR,
    TrailingRSQB,
    // In content
    StartTag,
    EmptyElement,
    EndTag,
    DataChars,
    DataNewline,
    CdataSectOpen,
    CdataSectClose,
    EntityRef,
    CharRef,
    AttributeValueS,
    // Anywhere
    PI,
    XmlDecl,
    Comment,
    // In the prolog
    BOM,
    PrologS,
    DeclOpen,
    DeclClose,
    Name,
    Nmtoken,
    PoundName,
    Or,
    Percent,
    OpenParen,
    CloseParen,
    CloseParenQuestion,
    CloseParenAsterisk,
    CloseParenPlus,
    OpenBracket,
    CloseBracket,
    Literal,
    ParamEntityRef,
    InstanceStart,
    NameQuestion,
    NameAsterisk,
    NamePlus,
    CondSectOpen,
    CondSectClose,
    Comma,
    PrefixedName,
    IgnoreSect, // What is left of an ignored section, with the "]]>".
};

enum class ExpatRole : uint8_t {
    Error,
    None,
    XmlDecl,
    InstanceStart,
    DoctypeNone,
    DoctypeName,
    DoctypeSystemId,
    DoctypePublicId,
    DoctypeInternalSubset,
    DoctypeClose,
    GeneralEntityName,
    ParamEntityName,
    EntityNone,
    EntityValue,
    EntitySystemId,
    EntityPublicId,
    EntityComplete,
    EntityNotationName,
    NotationNone,
    NotationName,
    NotationSystemId,
    NotationNoSystemId,
    NotationPublicId,
    AttributeName,
    AttributeTypeCData,
    AttributeTypeId,
    AttributeTypeIdRef,
    AttributeTypeIdRefs,
    AttributeTypeEntity,
    AttributeTypeEntities,
    AttributeTypeNmtoken,
    AttributeTypeNmtokens,
    AttributeEnumValue,
    AttributeNotationValue,
    AttlistNone,
    AttlistElementName,
    ImpliedAttributeValue,
    RequiredAttributeValue,
    DefaultAttributeValue,
    FixedAttributeValue,
    ElementNone,
    ElementName,
    ContentAny,
    ContentEmpty,
    ContentPCData,
    GroupOpen,
    GroupClose,
    GroupCloseRep,
    GroupCloseOpt,
    GroupClosePlus,
    GroupChoice,
    GroupSequence,
    ContentElement,
    ContentElementRep,
    ContentElementOpt,
    ContentElementPlus,
    PI,
    Comment,
    ParamEntityRef,
    InnerParamEntityRef, // Within a declaration.
    IgnoreSect,
};

// Where the grammar of the prolog is.
enum class ExpatPrologState : uint8_t {
    Prolog0, Prolog1, Prolog2,
    Doctype0, Doctype1, Doctype2, Doctype3, Doctype4, Doctype5,
    InternalSubset, ExternalSubset0, ExternalSubset,
    Entity0, Entity1, Entity2, Entity3, Entity4, Entity5, Entity6, Entity7, Entity8, Entity9, Entity10,
    Notation0, Notation1, Notation2, Notation3, Notation4,
    Attlist0, Attlist1, Attlist2, Attlist3, Attlist4, Attlist5, Attlist6, Attlist7, Attlist8, Attlist9,
    Element0, Element1, Element2, Element3, Element4, Element5, Element6, Element7,
    DeclClose,
    CondSect0, CondSect1, CondSect2,
    Error,
};

static constexpr char16_t expatInvalidCharacter = 0xffff; // What bytes that are no character are decoded to: it is none either.

static constexpr ASCIILiteral expatXmlNamespace = "http://www.w3.org/XML/1998/namespace"_s;
static constexpr ASCIILiteral expatXmlnsNamespace = "http://www.w3.org/2000/xmlns/"_s;

// The characters beyond ASCII that names are made of: Appendix B of XML 1.0 as it was up to the fourth edition, which is what Expat
// goes by. (The fifth edition allows nearly everything.)
struct ExpatCharacterRange {
    char16_t first;
    char16_t last;
};

// Letter: BaseChar | Ideographic.
static constexpr ExpatCharacterRange expatLetters[] = {
    { 0x00c0, 0x00d6 }, { 0x00d8, 0x00f6 }, { 0x00f8, 0x0131 }, { 0x0134, 0x013e }, { 0x0141, 0x0148 }, { 0x014a, 0x017e }, { 0x0180, 0x01c3 },
    { 0x01cd, 0x01f0 }, { 0x01f4, 0x01f5 }, { 0x01fa, 0x0217 }, { 0x0250, 0x02a8 }, { 0x02bb, 0x02c1 }, { 0x0386, 0x0386 }, { 0x0388, 0x038a },
    { 0x038c, 0x038c }, { 0x038e, 0x03a1 }, { 0x03a3, 0x03ce }, { 0x03d0, 0x03d6 }, { 0x03da, 0x03da }, { 0x03dc, 0x03dc }, { 0x03de, 0x03de },
    { 0x03e0, 0x03e0 }, { 0x03e2, 0x03f3 }, { 0x0401, 0x040c }, { 0x040e, 0x044f }, { 0x0451, 0x045c }, { 0x045e, 0x0481 }, { 0x0490, 0x04c4 },
    { 0x04c7, 0x04c8 }, { 0x04cb, 0x04cc }, { 0x04d0, 0x04eb }, { 0x04ee, 0x04f5 }, { 0x04f8, 0x04f9 }, { 0x0531, 0x0556 }, { 0x0559, 0x0559 },
    { 0x0561, 0x0586 }, { 0x05d0, 0x05ea }, { 0x05f0, 0x05f2 }, { 0x0621, 0x063a }, { 0x0641, 0x064a }, { 0x0671, 0x06b7 }, { 0x06ba, 0x06be },
    { 0x06c0, 0x06ce }, { 0x06d0, 0x06d3 }, { 0x06d5, 0x06d5 }, { 0x06e5, 0x06e6 }, { 0x0905, 0x0939 }, { 0x093d, 0x093d }, { 0x0958, 0x0961 },
    { 0x0985, 0x098c }, { 0x098f, 0x0990 }, { 0x0993, 0x09a8 }, { 0x09aa, 0x09b0 }, { 0x09b2, 0x09b2 }, { 0x09b6, 0x09b9 }, { 0x09dc, 0x09dd },
    { 0x09df, 0x09e1 }, { 0x09f0, 0x09f1 }, { 0x0a05, 0x0a0a }, { 0x0a0f, 0x0a10 }, { 0x0a13, 0x0a28 }, { 0x0a2a, 0x0a30 }, { 0x0a32, 0x0a33 },
    { 0x0a35, 0x0a36 }, { 0x0a38, 0x0a39 }, { 0x0a59, 0x0a5c }, { 0x0a5e, 0x0a5e }, { 0x0a72, 0x0a74 }, { 0x0a85, 0x0a8b }, { 0x0a8d, 0x0a8d },
    { 0x0a8f, 0x0a91 }, { 0x0a93, 0x0aa8 }, { 0x0aaa, 0x0ab0 }, { 0x0ab2, 0x0ab3 }, { 0x0ab5, 0x0ab9 }, { 0x0abd, 0x0abd }, { 0x0ae0, 0x0ae0 },
    { 0x0b05, 0x0b0c }, { 0x0b0f, 0x0b10 }, { 0x0b13, 0x0b28 }, { 0x0b2a, 0x0b30 }, { 0x0b32, 0x0b33 }, { 0x0b36, 0x0b39 }, { 0x0b3d, 0x0b3d },
    { 0x0b5c, 0x0b5d }, { 0x0b5f, 0x0b61 }, { 0x0b85, 0x0b8a }, { 0x0b8e, 0x0b90 }, { 0x0b92, 0x0b95 }, { 0x0b99, 0x0b9a }, { 0x0b9c, 0x0b9c },
    { 0x0b9e, 0x0b9f }, { 0x0ba3, 0x0ba4 }, { 0x0ba8, 0x0baa }, { 0x0bae, 0x0bb5 }, { 0x0bb7, 0x0bb9 }, { 0x0c05, 0x0c0c }, { 0x0c0e, 0x0c10 },
    { 0x0c12, 0x0c28 }, { 0x0c2a, 0x0c33 }, { 0x0c35, 0x0c39 }, { 0x0c60, 0x0c61 }, { 0x0c85, 0x0c8c }, { 0x0c8e, 0x0c90 }, { 0x0c92, 0x0ca8 },
    { 0x0caa, 0x0cb3 }, { 0x0cb5, 0x0cb9 }, { 0x0cde, 0x0cde }, { 0x0ce0, 0x0ce1 }, { 0x0d05, 0x0d0c }, { 0x0d0e, 0x0d10 }, { 0x0d12, 0x0d28 },
    { 0x0d2a, 0x0d39 }, { 0x0d60, 0x0d61 }, { 0x0e01, 0x0e2e }, { 0x0e30, 0x0e30 }, { 0x0e32, 0x0e33 }, { 0x0e40, 0x0e45 }, { 0x0e81, 0x0e82 },
    { 0x0e84, 0x0e84 }, { 0x0e87, 0x0e88 }, { 0x0e8a, 0x0e8a }, { 0x0e8d, 0x0e8d }, { 0x0e94, 0x0e97 }, { 0x0e99, 0x0e9f }, { 0x0ea1, 0x0ea3 },
    { 0x0ea5, 0x0ea5 }, { 0x0ea7, 0x0ea7 }, { 0x0eaa, 0x0eab }, { 0x0ead, 0x0eae }, { 0x0eb0, 0x0eb0 }, { 0x0eb2, 0x0eb3 }, { 0x0ebd, 0x0ebd },
    { 0x0ec0, 0x0ec4 }, { 0x0f40, 0x0f47 }, { 0x0f49, 0x0f69 }, { 0x10a0, 0x10c5 }, { 0x10d0, 0x10f6 }, { 0x1100, 0x1100 }, { 0x1102, 0x1103 },
    { 0x1105, 0x1107 }, { 0x1109, 0x1109 }, { 0x110b, 0x110c }, { 0x110e, 0x1112 }, { 0x113c, 0x113c }, { 0x113e, 0x113e }, { 0x1140, 0x1140 },
    { 0x114c, 0x114c }, { 0x114e, 0x114e }, { 0x1150, 0x1150 }, { 0x1154, 0x1155 }, { 0x1159, 0x1159 }, { 0x115f, 0x1161 }, { 0x1163, 0x1163 },
    { 0x1165, 0x1165 }, { 0x1167, 0x1167 }, { 0x1169, 0x1169 }, { 0x116d, 0x116e }, { 0x1172, 0x1173 }, { 0x1175, 0x1175 }, { 0x119e, 0x119e },
    { 0x11a8, 0x11a8 }, { 0x11ab, 0x11ab }, { 0x11ae, 0x11af }, { 0x11b7, 0x11b8 }, { 0x11ba, 0x11ba }, { 0x11bc, 0x11c2 }, { 0x11eb, 0x11eb },
    { 0x11f0, 0x11f0 }, { 0x11f9, 0x11f9 }, { 0x1e00, 0x1e9b }, { 0x1ea0, 0x1ef9 }, { 0x1f00, 0x1f15 }, { 0x1f18, 0x1f1d }, { 0x1f20, 0x1f45 },
    { 0x1f48, 0x1f4d }, { 0x1f50, 0x1f57 }, { 0x1f59, 0x1f59 }, { 0x1f5b, 0x1f5b }, { 0x1f5d, 0x1f5d }, { 0x1f5f, 0x1f7d }, { 0x1f80, 0x1fb4 },
    { 0x1fb6, 0x1fbc }, { 0x1fbe, 0x1fbe }, { 0x1fc2, 0x1fc4 }, { 0x1fc6, 0x1fcc }, { 0x1fd0, 0x1fd3 }, { 0x1fd6, 0x1fdb }, { 0x1fe0, 0x1fec },
    { 0x1ff2, 0x1ff4 }, { 0x1ff6, 0x1ffc }, { 0x2126, 0x2126 }, { 0x212a, 0x212b }, { 0x212e, 0x212e }, { 0x2180, 0x2182 }, { 0x3007, 0x3007 },
    { 0x3021, 0x3029 }, { 0x3041, 0x3094 }, { 0x30a1, 0x30fa }, { 0x3105, 0x312c }, { 0x4e00, 0x9fa5 }, { 0xac00, 0xd7a3 },
};

// CombiningChar | Digit | Extender.
static constexpr ExpatCharacterRange expatNameMarks[] = {
    { 0x00b7, 0x00b7 }, { 0x02d0, 0x02d1 }, { 0x0300, 0x0345 }, { 0x0360, 0x0361 }, { 0x0387, 0x0387 }, { 0x0483, 0x0486 }, { 0x0591, 0x05a1 },
    { 0x05a3, 0x05b9 }, { 0x05bb, 0x05bd }, { 0x05bf, 0x05bf }, { 0x05c1, 0x05c2 }, { 0x05c4, 0x05c4 }, { 0x0640, 0x0640 }, { 0x064b, 0x0652 },
    { 0x0660, 0x0669 }, { 0x0670, 0x0670 }, { 0x06d6, 0x06e4 }, { 0x06e7, 0x06e8 }, { 0x06ea, 0x06ed }, { 0x06f0, 0x06f9 }, { 0x0901, 0x0903 },
    { 0x093c, 0x093c }, { 0x093e, 0x094d }, { 0x0951, 0x0954 }, { 0x0962, 0x0963 }, { 0x0966, 0x096f }, { 0x0981, 0x0983 }, { 0x09bc, 0x09bc },
    { 0x09be, 0x09c4 }, { 0x09c7, 0x09c8 }, { 0x09cb, 0x09cd }, { 0x09d7, 0x09d7 }, { 0x09e2, 0x09e3 }, { 0x09e6, 0x09ef }, { 0x0a02, 0x0a02 },
    { 0x0a3c, 0x0a3c }, { 0x0a3e, 0x0a42 }, { 0x0a47, 0x0a48 }, { 0x0a4b, 0x0a4d }, { 0x0a66, 0x0a71 }, { 0x0a81, 0x0a83 }, { 0x0abc, 0x0abc },
    { 0x0abe, 0x0ac5 }, { 0x0ac7, 0x0ac9 }, { 0x0acb, 0x0acd }, { 0x0ae6, 0x0aef }, { 0x0b01, 0x0b03 }, { 0x0b3c, 0x0b3c }, { 0x0b3e, 0x0b43 },
    { 0x0b47, 0x0b48 }, { 0x0b4b, 0x0b4d }, { 0x0b56, 0x0b57 }, { 0x0b66, 0x0b6f }, { 0x0b82, 0x0b83 }, { 0x0bbe, 0x0bc2 }, { 0x0bc6, 0x0bc8 },
    { 0x0bca, 0x0bcd }, { 0x0bd7, 0x0bd7 }, { 0x0be7, 0x0bef }, { 0x0c01, 0x0c03 }, { 0x0c3e, 0x0c44 }, { 0x0c46, 0x0c48 }, { 0x0c4a, 0x0c4d },
    { 0x0c55, 0x0c56 }, { 0x0c66, 0x0c6f }, { 0x0c82, 0x0c83 }, { 0x0cbe, 0x0cc4 }, { 0x0cc6, 0x0cc8 }, { 0x0cca, 0x0ccd }, { 0x0cd5, 0x0cd6 },
    { 0x0ce6, 0x0cef }, { 0x0d02, 0x0d03 }, { 0x0d3e, 0x0d43 }, { 0x0d46, 0x0d48 }, { 0x0d4a, 0x0d4d }, { 0x0d57, 0x0d57 }, { 0x0d66, 0x0d6f },
    { 0x0e31, 0x0e31 }, { 0x0e34, 0x0e3a }, { 0x0e46, 0x0e4e }, { 0x0e50, 0x0e59 }, { 0x0eb1, 0x0eb1 }, { 0x0eb4, 0x0eb9 }, { 0x0ebb, 0x0ebc },
    { 0x0ec6, 0x0ec6 }, { 0x0ec8, 0x0ecd }, { 0x0ed0, 0x0ed9 }, { 0x0f18, 0x0f19 }, { 0x0f20, 0x0f29 }, { 0x0f35, 0x0f35 }, { 0x0f37, 0x0f37 },
    { 0x0f39, 0x0f39 }, { 0x0f3e, 0x0f3f }, { 0x0f71, 0x0f84 }, { 0x0f86, 0x0f8b }, { 0x0f90, 0x0f95 }, { 0x0f97, 0x0f97 }, { 0x0f99, 0x0fad },
    { 0x0fb1, 0x0fb7 }, { 0x0fb9, 0x0fb9 }, { 0x20d0, 0x20dc }, { 0x20e1, 0x20e1 }, { 0x3005, 0x3005 }, { 0x302a, 0x302f }, { 0x3031, 0x3035 },
    { 0x3099, 0x309a }, { 0x309d, 0x309e }, { 0x30fc, 0x30fe },
};

template<size_t size>
static bool expatIsInRanges(const ExpatCharacterRange (&ranges)[size], char16_t c)
{
    auto* range = std::upper_bound(std::begin(ranges), std::end(ranges), c, [](char16_t c, const ExpatCharacterRange& range) { return c < range.first; });
    return range != std::begin(ranges) && c <= range[-1].last;
}

class ExpatParser {
    WTF_MAKE_TZONE_ALLOCATED(ExpatParser);
public:
    using Text = std::span<const char16_t>;

    ExpatParser(PyRecord* owner, const String& encoding, bool hasNamespaces, char16_t separator)
        : m_owner(owner)
        , m_dtd(ExpatDTD::create())
        , m_protocolEncoding(encoding)
        , m_hasNamespaces(hasNamespaces)
        , m_separator(separator)
    {
        m_bindings.add("xml"_s, Vector<String> { String(expatXmlNamespace) });
    }

    // For an external entity that `parent` has come upon a reference to. A null context: a parameter entity, or the external subset.
    ExpatParser(PyRecord* owner, ExpatParser& parent, const String& context, const String& encoding)
        : m_owner(owner)
        , m_dtd(parent.m_dtd.copyRef())
        , m_protocolEncoding(encoding)
        , m_hasNamespaces(parent.m_hasNamespaces)
        , m_separator(parent.m_separator)
        , m_returnsTriplets(parent.m_returnsTriplets)
        , m_isChild(true)
        , m_base(parent.m_base)
        , m_paramEntityParsing(parent.m_paramEntityParsing)
        , m_defaultHandler(parent.m_defaultHandler)
        , m_expandsInternalEntities(parent.m_expandsInternalEntities)
        , m_isReparseDeferralEnabled(parent.m_isReparseDeferralEnabled)
    {
        m_bindings.add("xml"_s, Vector<String> { String(expatXmlNamespace) });
        if (context.isNull()) {
            m_isParameterEntity = true;
            m_prologState = ExpatPrologState::ExternalSubset0;
            m_processor = Processor::ExternalParameterEntityInit;
        } else {
            m_processor = Processor::ExternalEntityInit;
            setContext(context);
        }
    }

    // ---- What the object in Python shows ----

    bool orderedAttributes { false };
    bool specifiedAttributes { false };
    bool namespacePrefixes { false };
    bool isInCallback { false };
    bool buffersText { false };
    int64_t bufferSize { 8192 };
    ExpatDTD& dtd() { return m_dtd.get(); }

    int errorCode() const { return m_errorCode; }
    uint64_t currentLine() const { return m_reportedLine + 1; }
    uint64_t currentColumn() const { return m_reportedColumn; }
    int64_t currentByteIndex() const { return m_hasEvent ? static_cast<int64_t>(m_eventByte) : -1; }
    int64_t bufferUsed() const { return m_characterBufferUsed; }
    void setReturnsTriplets(bool value)
    {
        // Not once parsing has begun.
        if (!m_hasStarted)
            m_returnsTriplets = value;
    }
    const String& base() const { return m_base; }
    void setBase(const String& base) { m_base = base; }
    bool isReparseDeferralEnabled() const { return m_isReparseDeferralEnabled; }
    void setReparseDeferralEnabled(bool value) { m_isReparseDeferralEnabled = value; }

    bool setParamEntityParsing(int flag)
    {
        if (m_hasStarted)
            return false;
        m_paramEntityParsing = flag;
        return true;
    }

    ExpatError useForeignDTD(bool flag)
    {
        if (m_hasStarted)
            return ExpatErrorCantChangeFeatureOnceParsing;
        m_usesForeignDTD = flag;
        return ExpatErrorNone;
    }

    void setEncodingIfNotStarted(const String& encoding)
    {
        if (!m_hasStarted)
            m_protocolEncoding = encoding;
    }

    // A handler has been set, or taken away.
    void didSetHandler(unsigned handler, bool isSet)
    {
        if (handler == ExpatHandlerDefault || handler == ExpatHandlerDefaultExpand) {
            m_defaultHandler = isSet ? static_cast<int>(handler) : -1;
            m_expandsInternalEntities = handler == ExpatHandlerDefaultExpand;
        }
    }

    std::span<const uint8_t> inputContext() const
    {
        if (!m_hasEvent || m_eventByte < m_rawBase)
            return { };
        return m_raw.span().subspan(std::min<size_t>(m_eventByte - m_rawBase, m_raw.size()));
    }

    // False: an exception.
    bool flushCharacterBuffer(JSGlobalObject* globalObject)
    {
        if (!buffersText || !m_characterBufferUsed)
            return true;
        SetForScope scope(m_globalObject, globalObject);
        Vector<char16_t> data = std::exchange(m_characterBuffer, { });
        m_characterBufferUsed = 0;
        return callCharacterHandler(data.span());
    }

    void discardCharacterBuffer()
    {
        m_characterBuffer.clear();
        m_characterBufferUsed = 0;
    }

    // True: all is well so far. False: there is an error, which errorCode() tells, or an exception.
    bool parse(JSGlobalObject* globalObject, std::span<const uint8_t> data, bool isFinal)
    {
        SetForScope scope(m_globalObject, globalObject);
        if (m_isFinished || m_isStopped) {
            m_isFinished = true;
            m_errorCode = ExpatErrorFinished;
            return false;
        }
        if (m_processor == Processor::Error) {
            // Expat reckons the way from where it had got to, to the error, once more, on top of what it had. So be it.
            uint64_t byte = m_byte;
            m_positionIndex = m_resume;
            advancePositionTo(m_eventIndex);
            m_byte = byte;
            m_reportedLine = m_line;
            m_reportedColumn = m_column;
            return false;
        }
        m_hasStarted = true;
        m_isFinal = isFinal;
        m_raw.append(data);

        size_t start = m_resume;
        ExpatError error = ExpatErrorNone;
        if (m_isChild)
            error = accountForExpansion(data.size());
        else
            m_dtd->directBytes += data.size();
        if (!error)
            error = run();
        if (error != ExpatErrorNone) {
            m_errorCode = error;
            m_processor = Processor::Error;
            m_isFinished = m_isStopped;
            m_resume = start;
            m_reportedLine = m_eventLine;
            m_reportedColumn = m_eventColumn;
            return false;
        }
        advancePositionTo(m_resume);
        if (isFinal)
            m_isFinished = true; // The position stays that of what was reported last.
        else {
            // From now on it is where parsing will go on.
            m_reportedLine = m_line;
            m_reportedColumn = m_column;
        }
        dropConsumedInput();
        return true;
    }

private:
    enum class Processor : uint8_t { PrologInit, Prolog, IgnoreSection, Content, CdataSection, Epilog, ExternalEntityInit, ExternalEntityContent, ExternalParameterEntityInit, Error };
    enum class Encoding : uint8_t { Undetermined, UTF8, UTF16LE, UTF16BE, Latin1, ASCII, Table };

    struct Source {
        Text text;
        bool isDocument; // Not the replacement text of an entity.
    };

    struct RawAttribute {
        size_t nameStart;
        size_t nameEnd;
        size_t valueStart;
        size_t valueEnd;
    };

    struct Binding {
        String prefix; // Empty: the default namespace.
    };

    struct Tag {
        String rawName;
        String name;
        Vector<Binding> bindings;
    };

    // ---- Characters ----

    static bool isSpace(char16_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    // Expat has a table of its own for the first 256 characters of input in ISO-8859-1 and UTF-16, with three letters more.
    bool isLetterOfLatin1Only(char16_t c) const
    {
        return (c == 0xaa || c == 0xb5 || c == 0xba) && !m_isInReplacementText && (m_encoding == Encoding::Latin1 || isTwoBytes(m_encoding));
    }

    // With namespaces a colon is punctuation, which those who scan names see to themselves.
    bool isNameStart(char16_t c) const
    {
        if (c < 0x80)
            return isASCIIAlpha(c) || c == '_' || (c == ':' && !m_hasNamespaces);
        return expatIsInRanges(expatLetters, c) || isLetterOfLatin1Only(c);
    }

    bool isNameChar(char16_t c) const
    {
        if (c < 0x80)
            return isASCIIAlphanumeric(c) || c == '_' || c == '-' || c == '.' || (c == ':' && !m_hasNamespaces);
        return expatIsInRanges(expatLetters, c) || expatIsInRanges(expatNameMarks, c) || isLetterOfLatin1Only(c);
    }

    // How many code units the character at `p` takes. 0: it is none that XML allows.
    static unsigned characterLength(Text t, size_t p)
    {
        char16_t c = t[p];
        if (c < 0x20)
            return c == '\t' || c == '\n' || c == '\r';
        if (c < 0xd800)
            return 1;
        if (c >= 0xe000)
            return c < 0xfffe;
        return U16_IS_LEAD(c) && p + 1 < t.size() && U16_IS_TRAIL(t[p + 1]) ? 2 : 0;
    }

    // Invalid, or, for what stands for the bytes that the input ended in the middle of, PartialChar.
    ExpatTok invalidAt(Text t, size_t p, size_t& next) const
    {
        next = p;
        return t.data() == m_text.span().data() && p == m_partialCharacterIndex ? ExpatTok::PartialChar : ExpatTok::Invalid;
    }

    static bool matches(Text t, size_t start, size_t end, ASCIILiteral word)
    {
        if (end - start != word.length())
            return false;
        for (size_t i = 0; i < word.length(); ++i) {
            if (t[start + i] != word[i])
                return false;
        }
        return true;
    }

    static String stringOfText(Text t)
    {
        if (t.empty())
            return emptyString();
        return WTF::charactersAreAllLatin1(t) ? String::make8Bit(t) : String(t);
    }
    static String stringOfText(Text t, size_t start, size_t end) { return stringOfText(t.subspan(start, end - start)); }

    static void append(Vector<char16_t>& out, const String& string)
    {
        for (unsigned i = 0; i < string.length(); ++i)
            out.append(string[i]);
    }

    static void appendCodePointTo(Vector<char16_t>& out, char32_t c)
    {
        if (c < 0x10000)
            out.append(static_cast<char16_t>(c));
        else {
            out.append(U16_LEAD(c));
            out.append(U16_TRAIL(c));
        }
    }

    // ---- Tokens. `p` is where the token starts; `next` is set to where it ends, or to what is wrong. ----

#define EXPAT_REQUIRE(p) \
    do { \
        if ((p) >= t.size()) \
            return ExpatTok::Partial; \
    } while (0)

    ExpatTok scanCharRef(Text t, size_t p, size_t& next) const
    {
        EXPAT_REQUIRE(p);
        bool isHex = t[p] == 'x';
        if (isHex) {
            ++p;
            EXPAT_REQUIRE(p);
        }
        auto isDigit = [&](char16_t c) { return isHex ? isASCIIHexDigit(c) : isASCIIDigit(c); };
        if (!isDigit(t[p]))
            return invalidAt(t, p, next);
        for (++p; p < t.size(); ++p) {
            if (isDigit(t[p]))
                continue;
            if (t[p] == ';') {
                next = p + 1;
                return ExpatTok::CharRef;
            }
            return invalidAt(t, p, next);
        }
        return ExpatTok::Partial;
    }

    // After the "&".
    ExpatTok scanRef(Text t, size_t p, size_t& next) const
    {
        EXPAT_REQUIRE(p);
        if (t[p] == '#')
            return scanCharRef(t, p + 1, next);
        if (!isNameStart(t[p]))
            return invalidAt(t, p, next);
        for (++p; p < t.size(); ++p) {
            if (isNameChar(t[p]))
                continue;
            if (t[p] == ';') {
                next = p + 1;
                return ExpatTok::EntityRef;
            }
            return invalidAt(t, p, next);
        }
        return ExpatTok::Partial;
    }

    // After the "%".
    ExpatTok scanPercent(Text t, size_t p, size_t& next) const
    {
        EXPAT_REQUIRE(p);
        if (isSpace(t[p]) || t[p] == '%') {
            next = p;
            return ExpatTok::Percent;
        }
        if (!isNameStart(t[p]))
            return invalidAt(t, p, next);
        for (++p; p < t.size(); ++p) {
            if (isNameChar(t[p]))
                continue;
            if (t[p] == ';') {
                next = p + 1;
                return ExpatTok::ParamEntityRef;
            }
            return invalidAt(t, p, next);
        }
        return ExpatTok::Partial;
    }

    // After the "<!-".
    ExpatTok scanComment(Text t, size_t p, size_t& next) const
    {
        EXPAT_REQUIRE(p);
        if (t[p] != '-')
            return invalidAt(t, p, next);
        ++p;
        while (p < t.size()) {
            if (t[p] == '-') {
                ++p;
                EXPAT_REQUIRE(p);
                if (t[p] == '-') {
                    ++p;
                    EXPAT_REQUIRE(p);
                    if (t[p] != '>')
                        return invalidAt(t, p, next);
                    next = p + 1;
                    return ExpatTok::Comment;
                }
                continue;
            }
            unsigned length = characterLength(t, p);
            if (!length)
                return invalidAt(t, p, next);
            p += length;
        }
        return ExpatTok::Partial;
    }

    // After the "<?".
    ExpatTok scanPI(Text t, size_t p, size_t& next) const
    {
        size_t target = p;
        EXPAT_REQUIRE(p);
        if (!isNameStart(t[p]))
            return invalidAt(t, p, next);
        // "xml" is the declaration; in any other case it is reserved.
        auto check = [&](size_t end, ExpatTok& tok) {
            tok = ExpatTok::PI;
            if (end - target != 3 || toASCIILower(t[target]) != 'x' || toASCIILower(t[target + 1]) != 'm' || toASCIILower(t[target + 2]) != 'l')
                return true;
            if (!matches(t, target, end, "xml"_s))
                return false;
            tok = ExpatTok::XmlDecl;
            return true;
        };
        ExpatTok tok;
        for (++p; p < t.size(); ++p) {
            if (isNameChar(t[p]))
                continue;
            if (isSpace(t[p])) {
                if (!check(p, tok))
                    return invalidAt(t, p, next);
                ++p;
                while (p < t.size()) {
                    if (t[p] == '?') {
                        ++p;
                        EXPAT_REQUIRE(p);
                        if (t[p] == '>') {
                            next = p + 1;
                            return tok;
                        }
                        continue;
                    }
                    unsigned length = characterLength(t, p);
                    if (!length)
                        return invalidAt(t, p, next);
                    p += length;
                }
                return ExpatTok::Partial;
            }
            if (t[p] == '?') {
                if (!check(p, tok))
                    return invalidAt(t, p, next);
                ++p;
                EXPAT_REQUIRE(p);
                if (t[p] == '>') {
                    next = p + 1;
                    return tok;
                }
            }
            return invalidAt(t, p, next);
        }
        return ExpatTok::Partial;
    }

    // After the "</".
    ExpatTok scanEndTag(Text t, size_t p, size_t& next) const
    {
        EXPAT_REQUIRE(p);
        if (!isNameStart(t[p]))
            return invalidAt(t, p, next);
        for (++p; p < t.size(); ++p) {
            // A colon out of place will not match the start tag.
            if (isNameChar(t[p]) || t[p] == ':')
                continue;
            if (isSpace(t[p])) {
                for (++p; p < t.size(); ++p) {
                    if (isSpace(t[p]))
                        continue;
                    if (t[p] == '>') {
                        next = p + 1;
                        return ExpatTok::EndTag;
                    }
                    return invalidAt(t, p, next);
                }
                return ExpatTok::Partial;
            }
            if (t[p] == '>') {
                next = p + 1;
                return ExpatTok::EndTag;
            }
            return invalidAt(t, p, next);
        }
        return ExpatTok::Partial;
    }

    // The rest of a name of which the first character is before `p`. Leaves `p` at what follows. None: it is fine.
    ExpatTok scanQName(Text t, size_t& p, size_t& next) const
    {
        bool hadColon = false;
        while (p < t.size()) {
            if (isNameChar(t[p])) {
                ++p;
                continue;
            }
            if (t[p] != ':')
                return ExpatTok::None;
            if (hadColon)
                return invalidAt(t, p, next);
            hadColon = true;
            ++p;
            EXPAT_REQUIRE(p);
            if (!isNameStart(t[p]))
                return invalidAt(t, p, next);
            ++p;
        }
        return ExpatTok::Partial;
    }

    // The attributes and the end of a start tag. `p` is after the first character of the name of the first attribute.
    ExpatTok scanAttributes(Text t, size_t p, size_t& next) const
    {
        while (true) {
            if (ExpatTok tok = scanQName(t, p, next); tok != ExpatTok::None)
                return tok;
            while (isSpace(t[p])) {
                ++p;
                EXPAT_REQUIRE(p);
            }
            if (t[p] != '=')
                return invalidAt(t, p, next);
            char16_t quote;
            while (true) {
                ++p;
                EXPAT_REQUIRE(p);
                quote = t[p];
                if (quote == '"' || quote == '\'')
                    break;
                if (!isSpace(quote))
                    return invalidAt(t, p, next);
            }
            ++p;
            while (true) {
                EXPAT_REQUIRE(p);
                char16_t c = t[p];
                if (c == quote)
                    break;
                if (c == '&') {
                    size_t afterRef;
                    ExpatTok tok = scanRef(t, p + 1, afterRef);
                    if (tok != ExpatTok::EntityRef && tok != ExpatTok::CharRef) {
                        next = afterRef;
                        return tok;
                    }
                    p = afterRef;
                    continue;
                }
                if (c == '<')
                    return invalidAt(t, p, next);
                unsigned length = characterLength(t, p);
                if (!length)
                    return invalidAt(t, p, next);
                p += length;
            }
            ++p;
            EXPAT_REQUIRE(p);
            if (!isSpace(t[p]) && t[p] != '/' && t[p] != '>')
                return invalidAt(t, p, next);
            while (isSpace(t[p])) {
                ++p;
                EXPAT_REQUIRE(p);
            }
            if (t[p] == '>') {
                next = p + 1;
                return ExpatTok::StartTag;
            }
            if (t[p] == '/') {
                ++p;
                EXPAT_REQUIRE(p);
                if (t[p] != '>')
                    return invalidAt(t, p, next);
                next = p + 1;
                return ExpatTok::EmptyElement;
            }
            if (!isNameStart(t[p]))
                return invalidAt(t, p, next);
            ++p;
        }
    }

    // After the "<", in content.
    ExpatTok scanLt(Text t, size_t p, size_t& next) const
    {
        EXPAT_REQUIRE(p);
        switch (t[p]) {
        case '!':
            ++p;
            EXPAT_REQUIRE(p);
            if (t[p] == '-')
                return scanComment(t, p + 1, next);
            if (t[p] == '[') {
                ++p;
                static constexpr ASCIILiteral word = "CDATA["_s;
                if (t.size() - p < word.length())
                    return ExpatTok::Partial;
                for (size_t i = 0; i < word.length(); ++i, ++p) {
                    if (t[p] != word[i])
                        return invalidAt(t, p, next);
                }
                next = p;
                return ExpatTok::CdataSectOpen;
            }
            return invalidAt(t, p, next);
        case '?':
            return scanPI(t, p + 1, next);
        case '/':
            return scanEndTag(t, p + 1, next);
        default:
            break;
        }
        if (!isNameStart(t[p]))
            return invalidAt(t, p, next);
        ++p;
        if (ExpatTok tok = scanQName(t, p, next); tok != ExpatTok::None)
            return tok;
        if (isSpace(t[p])) {
            while (isSpace(t[p])) {
                ++p;
                EXPAT_REQUIRE(p);
            }
            if (isNameStart(t[p]))
                return scanAttributes(t, p + 1, next);
        }
        if (t[p] == '>') {
            next = p + 1;
            return ExpatTok::StartTag;
        }
        if (t[p] == '/') {
            ++p;
            EXPAT_REQUIRE(p);
            if (t[p] != '>')
                return invalidAt(t, p, next);
            next = p + 1;
            return ExpatTok::EmptyElement;
        }
        return invalidAt(t, p, next);
    }

    ExpatTok contentTok(Text t, size_t p, size_t& next) const
    {
        if (p >= t.size())
            return ExpatTok::None;
        switch (t[p]) {
        case '<':
            return scanLt(t, p + 1, next);
        case '&':
            return scanRef(t, p + 1, next);
        case '\r':
            ++p;
            if (p == t.size())
                return ExpatTok::TrailingCR;
            if (t[p] == '\n')
                ++p;
            next = p;
            return ExpatTok::DataNewline;
        case '\n':
            next = p + 1;
            return ExpatTok::DataNewline;
        case ']':
            ++p;
            if (p == t.size())
                return ExpatTok::TrailingRSQB;
            if (t[p] != ']')
                break;
            ++p;
            if (p == t.size())
                return ExpatTok::TrailingRSQB;
            if (t[p] != '>') {
                --p;
                break;
            }
            next = p;
            return ExpatTok::Invalid;
        default: {
            unsigned length = characterLength(t, p);
            if (!length)
                return invalidAt(t, p, next);
            p += length;
            break;
        }
        }
        while (p < t.size()) {
            char16_t c = t[p];
            if (c == '&' || c == '<' || c == '\r' || c == '\n')
                break;
            if (c == ']') {
                // "]]>" is not allowed. Whether this is one may depend on what is to come.
                if (p + 1 >= t.size())
                    break;
                if (t[p + 1] == ']') {
                    if (p + 2 >= t.size())
                        break;
                    if (t[p + 2] == '>') {
                        next = p + 2;
                        return ExpatTok::Invalid;
                    }
                }
                ++p;
                continue;
            }
            unsigned length = characterLength(t, p);
            if (!length)
                break;
            p += length;
        }
        next = p;
        return ExpatTok::DataChars;
    }

    // From after "<![IGNORE[". Sections within it count.
    ExpatTok ignoreSectionTok(Text t, size_t p, size_t& next) const
    {
        unsigned level = 0;
        while (p < t.size()) {
            switch (t[p]) {
            case '<':
                ++p;
                EXPAT_REQUIRE(p);
                if (t[p] != '!')
                    break;
                ++p;
                EXPAT_REQUIRE(p);
                if (t[p] == '[') {
                    ++level;
                    ++p;
                }
                break;
            case ']':
                ++p;
                EXPAT_REQUIRE(p);
                if (t[p] != ']')
                    break;
                ++p;
                EXPAT_REQUIRE(p);
                if (t[p] != '>')
                    break;
                ++p;
                if (!level) {
                    next = p;
                    return ExpatTok::IgnoreSect;
                }
                --level;
                break;
            default: {
                unsigned length = characterLength(t, p);
                if (!length)
                    return invalidAt(t, p, next);
                p += length;
                break;
            }
            }
        }
        return ExpatTok::Partial;
    }

    ExpatTok cdataSectionTok(Text t, size_t p, size_t& next) const
    {
        if (p >= t.size())
            return ExpatTok::None;
        switch (t[p]) {
        case ']':
            ++p;
            EXPAT_REQUIRE(p);
            if (t[p] != ']')
                break;
            ++p;
            EXPAT_REQUIRE(p);
            if (t[p] != '>') {
                --p;
                break;
            }
            next = p + 1;
            return ExpatTok::CdataSectClose;
        case '\r':
            ++p;
            EXPAT_REQUIRE(p);
            if (t[p] == '\n')
                ++p;
            next = p;
            return ExpatTok::DataNewline;
        case '\n':
            next = p + 1;
            return ExpatTok::DataNewline;
        default: {
            unsigned length = characterLength(t, p);
            if (!length)
                return invalidAt(t, p, next);
            p += length;
            break;
        }
        }
        while (p < t.size()) {
            char16_t c = t[p];
            if (c == ']' || c == '\r' || c == '\n')
                break;
            unsigned length = characterLength(t, p);
            if (!length)
                break;
            p += length;
        }
        next = p;
        return ExpatTok::DataChars;
    }

    // In the value of an attribute, or (`isEntityValue`) of an entity, between the quotes.
    ExpatTok valueTok(Text t, size_t p, size_t& next, bool isEntityValue) const
    {
        if (p >= t.size())
            return ExpatTok::None;
        size_t start = p;
        for (; p < t.size(); ++p) {
            char16_t c = t[p];
            bool isSpecial = c == '&' || c == '\n' || c == '\r' || (isEntityValue ? c == '%' : c == '<' || c == ' ' || c == '\t');
            if (!isSpecial)
                continue;
            if (c == '<') {
                // It got here in the replacement text of an entity.
                next = p;
                return ExpatTok::Invalid;
            }
            if (p != start)
                break;
            switch (c) {
            case '&':
                return scanRef(t, p + 1, next);
            case '%': {
                ExpatTok tok = scanPercent(t, p + 1, next);
                return tok == ExpatTok::Percent ? ExpatTok::Invalid : tok;
            }
            case '\n':
                next = p + 1;
                return ExpatTok::DataNewline;
            case '\r':
                ++p;
                if (p == t.size())
                    return ExpatTok::TrailingCR;
                if (t[p] == '\n')
                    ++p;
                next = p;
                return ExpatTok::DataNewline;
            default:
                next = p + 1;
                return ExpatTok::AttributeValueS;
            }
        }
        next = p;
        return ExpatTok::DataChars;
    }

    // `isProvisional`: the text ends where the token may; with more text it might go on.
    ExpatTok prologTok(Text t, size_t p, size_t& next, bool& isProvisional) const
    {
        isProvisional = false;
        if (p >= t.size())
            return ExpatTok::None;
        auto provisional = [&](ExpatTok tok) {
            isProvisional = true;
            next = t.size();
            return tok;
        };
        char16_t c = t[p];
        switch (c) {
        case '"':
        case '\'':
            for (++p; p < t.size();) {
                if (t[p] == c) {
                    ++p;
                    if (p == t.size())
                        return provisional(ExpatTok::Literal);
                    if (isSpace(t[p]) || t[p] == '>' || t[p] == '%' || t[p] == '[') {
                        next = p;
                        return ExpatTok::Literal;
                    }
                    return invalidAt(t, p, next);
                }
                unsigned length = characterLength(t, p);
                if (!length)
                    return invalidAt(t, p, next);
                p += length;
            }
            return ExpatTok::Partial;
        case '<':
            ++p;
            EXPAT_REQUIRE(p);
            if (t[p] == '!') {
                ++p;
                EXPAT_REQUIRE(p);
                if (t[p] == '-')
                    return scanComment(t, p + 1, next);
                if (t[p] == '[') {
                    next = p + 1;
                    return ExpatTok::CondSectOpen;
                }
                if (!isASCIIAlpha(t[p]))
                    return invalidAt(t, p, next);
                for (++p; p < t.size(); ++p) {
                    if (isASCIIAlpha(t[p]))
                        continue;
                    if (t[p] == '%') {
                        // "<!ENTITY%" will not do, unless a reference follows.
                        if (p + 1 >= t.size())
                            return ExpatTok::Partial;
                        if (isSpace(t[p + 1]) || t[p + 1] == '%')
                            return invalidAt(t, p, next);
                    } else if (!isSpace(t[p]))
                        return invalidAt(t, p, next);
                    next = p;
                    return ExpatTok::DeclOpen;
                }
                return ExpatTok::Partial;
            }
            if (t[p] == '?')
                return scanPI(t, p + 1, next);
            if (isNameStart(t[p])) {
                next = p - 1;
                return ExpatTok::InstanceStart;
            }
            return invalidAt(t, p, next);
        case '\r':
            if (p + 1 == t.size())
                return provisional(ExpatTok::PrologS); // A line feed may follow.
            [[fallthrough]];
        case ' ':
        case '\t':
        case '\n':
            for (++p; p < t.size(); ++p) {
                if (t[p] == '\r' && p + 1 == t.size())
                    break; // Left for the next token.
                if (!isSpace(t[p]))
                    break;
            }
            next = p;
            return ExpatTok::PrologS;
        case '%':
            return scanPercent(t, p + 1, next);
        case ',':
            next = p + 1;
            return ExpatTok::Comma;
        case '[':
            next = p + 1;
            return ExpatTok::OpenBracket;
        case ']':
            ++p;
            if (p == t.size())
                return provisional(ExpatTok::CloseBracket);
            if (t[p] == ']') {
                if (p + 1 >= t.size())
                    return ExpatTok::Partial;
                if (t[p + 1] == '>') {
                    next = p + 2;
                    return ExpatTok::CondSectClose;
                }
            }
            next = p;
            return ExpatTok::CloseBracket;
        case '(':
            next = p + 1;
            return ExpatTok::OpenParen;
        case ')':
            ++p;
            if (p == t.size())
                return provisional(ExpatTok::CloseParen);
            next = p + 1;
            switch (t[p]) {
            case '*':
                return ExpatTok::CloseParenAsterisk;
            case '?':
                return ExpatTok::CloseParenQuestion;
            case '+':
                return ExpatTok::CloseParenPlus;
            case ' ':
            case '\t':
            case '\r':
            case '\n':
            case '>':
            case ',':
            case '|':
            case ')':
                next = p;
                return ExpatTok::CloseParen;
            default:
                return invalidAt(t, p, next);
            }
        case '|':
            next = p + 1;
            return ExpatTok::Or;
        case '>':
            next = p + 1;
            return ExpatTok::DeclClose;
        case '#':
            ++p;
            EXPAT_REQUIRE(p);
            if (!isNameStart(t[p]))
                return invalidAt(t, p, next);
            for (++p; p < t.size(); ++p) {
                if (isNameChar(t[p]))
                    continue;
                if (isSpace(t[p]) || t[p] == ')' || t[p] == '>' || t[p] == '%' || t[p] == '|') {
                    next = p;
                    return ExpatTok::PoundName;
                }
                return invalidAt(t, p, next);
            }
            return provisional(ExpatTok::PoundName);
        default:
            break;
        }

        ExpatTok tok;
        if (isNameStart(c))
            tok = ExpatTok::Name;
        else if (isNameChar(c) || c == ':')
            tok = ExpatTok::Nmtoken;
        else
            return invalidAt(t, p, next);
        for (++p; p < t.size(); ++p) {
            c = t[p];
            if (isNameChar(c))
                continue;
            next = p;
            switch (c) {
            case '>':
            case ')':
            case ',':
            case '|':
            case '[':
            case '%':
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                return tok;
            case ':':
                if (tok == ExpatTok::Name) {
                    if (p + 1 >= t.size())
                        return ExpatTok::Partial;
                    tok = ExpatTok::PrefixedName;
                    if (!isNameChar(t[p + 1]))
                        tok = ExpatTok::Nmtoken;
                    else
                        ++p;
                } else if (tok == ExpatTok::PrefixedName)
                    tok = ExpatTok::Nmtoken;
                continue;
            case '+':
            case '*':
            case '?':
                if (tok == ExpatTok::Nmtoken)
                    return ExpatTok::Invalid;
                next = p + 1;
                return c == '+' ? ExpatTok::NamePlus : c == '*' ? ExpatTok::NameAsterisk : ExpatTok::NameQuestion;
            default:
                return invalidAt(t, p, next);
            }
        }
        return provisional(tok);
    }

#undef EXPAT_REQUIRE

    // ---- References ----

    // The character a reference such as "&#38;" is to. -1: there is no such character.
    static int32_t charRefNumber(Text t, size_t start)
    {
        size_t p = start + 2;
        uint32_t result = 0;
        bool isHex = t[p] == 'x';
        if (isHex)
            ++p;
        for (; t[p] != ';'; ++p) {
            result = isHex ? (result << 4) | toASCIIHexValue(t[p]) : result * 10 + (t[p] - '0');
            if (result >= 0x110000)
                return -1;
        }
        if ((result >= 0xd800 && result <= 0xdfff) || result == 0xfffe || result == 0xffff)
            return -1;
        if (result < 0x20 && result != '\t' && result != '\n' && result != '\r')
            return -1;
        return static_cast<int32_t>(result);
    }

    // For the name in "&name;". 0: it is not one of the five.
    static char16_t predefinedEntity(Text t, size_t start, size_t end)
    {
        if (matches(t, start, end, "lt"_s))
            return '<';
        if (matches(t, start, end, "gt"_s))
            return '>';
        if (matches(t, start, end, "amp"_s))
            return '&';
        if (matches(t, start, end, "quot"_s))
            return '"';
        if (matches(t, start, end, "apos"_s))
            return '\'';
        return 0;
    }

    // ---- Positions ----

    unsigned bytesOfUnit(char16_t c) const
    {
        switch (m_encoding) {
        case Encoding::UTF16LE:
        case Encoding::UTF16BE:
            return 2;
        case Encoding::Undetermined:
        case Encoding::UTF8:
            // The four bytes of a pair are put down to its first half.
            return c < 0x80 ? 1 : c < 0x800 ? 2 : U16_IS_LEAD(c) ? 4 : U16_IS_TRAIL(c) ? 0 : 3;
        default:
            return 1;
        }
    }

    void advancePositionTo(size_t index)
    {
        Text t = m_text.span();
        while (m_positionIndex < index) {
            char16_t c = t[m_positionIndex++];
            m_byte += bytesOfUnit(c);
            if (c == '\n') {
                ++m_line;
                m_column = 0;
            } else if (c == '\r') {
                ++m_line;
                m_column = 0;
                if (m_positionIndex < index && t[m_positionIndex] == '\n') {
                    m_byte += bytesOfUnit('\n');
                    ++m_positionIndex;
                }
            } else if (!U16_IS_TRAIL(c))
                ++m_column;
        }
    }

    // What is being reported, or is wrong, starts here. (In the replacement text of an entity it stays at the reference.)
    void setEvent(const Source& source, size_t index)
    {
        if (!source.isDocument)
            return;
        advancePositionTo(index);
        m_hasEvent = true;
        m_eventIndex = index;
        m_eventLine = m_reportedLine = m_line;
        m_eventColumn = m_reportedColumn = m_column;
        m_eventByte = m_byte;
    }

    void dropConsumedInput()
    {
        ASSERT(m_positionIndex == m_resume);
        m_text.removeAt(0, m_resume);
        if (m_partialCharacterIndex != notFound)
            m_partialCharacterIndex -= m_resume;
        m_positionIndex = 0;
        m_resume = 0;
        size_t consumedBytes = std::min<size_t>(m_byte - m_rawBase, m_raw.size());
        m_raw.removeAt(0, consumedBytes);
        m_rawBase += consumedBytes;
        m_rawDecoded -= std::min(m_rawDecoded, consumedBytes);
    }

    // ---- Decoding ----

    static Encoding encodingNamed(const String& name)
    {
        if (equalLettersIgnoringASCIICase(name, "utf-8"_s))
            return Encoding::UTF8;
        if (equalLettersIgnoringASCIICase(name, "iso-8859-1"_s))
            return Encoding::Latin1;
        if (equalLettersIgnoringASCIICase(name, "us-ascii"_s))
            return Encoding::ASCII;
        if (equalLettersIgnoringASCIICase(name, "utf-16be"_s) || equalLettersIgnoringASCIICase(name, "utf-16"_s))
            return Encoding::UTF16BE;
        if (equalLettersIgnoringASCIICase(name, "utf-16le"_s))
            return Encoding::UTF16LE;
        return Encoding::Undetermined;
    }

    static bool isTwoBytes(Encoding encoding) { return encoding == Encoding::UTF16LE || encoding == Encoding::UTF16BE; }

    // An encoding that is not built in: Python's codec for it says what each of the 256 bytes is.
    ExpatError useUnknownEncoding(const String& name)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        uint8_t bytes[256];
        for (unsigned i = 0; i < 256; ++i)
            bytes[i] = static_cast<uint8_t>(i);
        JSValue decoded = bytesDecode(m_globalObject, PyBytes::create(m_globalObject, std::span<const uint8_t> { bytes, 256 }), makeStr(vm, name), makeStr(vm, "replace"_s));
        RETURN_IF_EXCEPTION(scope, ExpatErrorUnknownEncoding);
        StrView view(m_globalObject, strValue(decoded));
        RETURN_IF_EXCEPTION(scope, ExpatErrorUnknownEncoding);
        if (view.length() != 256) {
            raiseValueError(m_globalObject, scope, "multi-byte encodings are not supported"_s);
            return ExpatErrorUnknownEncoding;
        }
        // What the syntax of XML is made of has to be where it is in ASCII, and nowhere else.
        auto isSyntax = [](char32_t c) {
            return c < 0x80 && (isASCIIAlphanumeric(c) || isSpace(c) || (c > ' ' && !WTF::contains("$@\\^`{}~\x7f"_span, static_cast<char>(c))));
        };
        for (unsigned i = 0; i < 256; ++i) {
            char32_t c = view.at(i);
            if (c != i && (isSyntax(i) || isSyntax(c)))
                return ExpatErrorUnknownEncoding;
            bool isCharacter = c != 0xfffd && !U_IS_SURROGATE(c) && c != 0xfffe && c != 0xffff;
            if (isCharacter && c > 0xffff)
                return ExpatErrorUnknownEncoding;
            m_table[i] = isCharacter ? static_cast<char16_t>(c) : expatInvalidCharacter;
        }
        m_encoding = Encoding::Table;
        return ExpatErrorNone;
    }

    // Finds out the encoding from how the input starts, and from what the caller said. False: there is too little input to tell.
    bool determineEncoding(ExpatError& error)
    {
        error = ExpatErrorNone;
        Encoding given = Encoding::Undetermined;
        bool isPlainUTF16 = false;
        if (!m_protocolEncoding.isNull()) {
            given = encodingNamed(m_protocolEncoding);
            isPlainUTF16 = equalLettersIgnoringASCIICase(m_protocolEncoding, "utf-16"_s);
            if (given == Encoding::Undetermined) {
                error = useUnknownEncoding(m_protocolEncoding);
                m_mayHaveEncodingDeclaration = false;
                return true;
            }
        }
        auto raw = m_raw.span();
        if (raw.empty()) {
            if (!m_isFinal)
                return false;
            m_encoding = given == Encoding::Undetermined ? Encoding::UTF8 : given;
            return true;
        }
        // How a document starts tells more than what the caller said. An external entity may start with anything.
        bool isContent = m_isChild && !m_isParameterEntity;
        bool isGivenUTF16 = isTwoBytes(given);
        Encoding fallback = given == Encoding::Undetermined ? Encoding::UTF8 : given;
        auto isIncomplete = [&] {
            if (!m_isFinal)
                return false;
            m_hasEvent = true;
            error = ExpatErrorUnclosedToken;
            return true;
        };
        unsigned first = raw[0];
        if (raw.size() == 1) {
            // One byte tells only if it cannot be the first of a byte order mark or of a character in UTF-16.
            if (isGivenUTF16 || !first || first == '<' || ((first == 0xfe || first == 0xff || first == 0xef) && !(isContent && given == Encoding::Latin1)))
                return isIncomplete();
            m_encoding = fallback;
            return true;
        }
        unsigned second = raw[1];
        m_encoding = fallback;
        if (first == 0xfe && second == 0xff) {
            if (!(isContent && given == Encoding::Latin1))
                m_encoding = Encoding::UTF16BE;
        } else if (first == 0xff && second == 0xfe) {
            if (!(isContent && given == Encoding::Latin1))
                m_encoding = Encoding::UTF16LE;
        } else if (first == 0xef && second == 0xbb) {
            if (!(isContent && (given == Encoding::Latin1 || isGivenUTF16))) {
                if (raw.size() < 3) {
                    m_encoding = Encoding::Undetermined;
                    return isIncomplete();
                }
                if (raw[2] == 0xbf)
                    m_encoding = Encoding::UTF8;
            }
        } else if (first == '<' && !second) {
            if (!(isContent && given == Encoding::UTF16BE))
                m_encoding = Encoding::UTF16LE;
        } else if (!first) {
            if (!(isContent && given == Encoding::UTF16LE))
                m_encoding = Encoding::UTF16BE;
        } else if (!second && !isContent)
            m_encoding = Encoding::UTF16LE;
        UNUSED_VARIABLE(isPlainUTF16);
        return true;
    }

    // Decodes what has come in and has not been yet. An XML declaration may name another encoding for what follows it, so
    // until it has been seen nothing beyond it is decoded.
    void decode()
    {
        auto raw = m_raw.span();
        size_t limit = raw.size();
        if (m_mayHaveEncodingDeclaration && !isTwoBytes(m_encoding)) {
            size_t start = raw.size() >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf ? 3 : 0;
            auto rest = raw.subspan(start);
            static constexpr uint8_t opening[] = { '<', '?', 'x', 'm', 'l' };
            size_t common = std::min(rest.size(), std::size(opening));
            if (memcmp(rest.data(), opening, common))
                m_mayHaveEncodingDeclaration = false;
            else if (!m_isFinal || common == std::size(opening)) {
                size_t end = notFound;
                for (size_t i = 0; i + 1 < rest.size(); ++i) {
                    if (rest[i] == '?' && rest[i + 1] == '>' && i >= std::size(opening)) {
                        end = i + 2;
                        break;
                    }
                }
                if (end != notFound)
                    limit = start + end;
                else if (!m_isFinal)
                    limit = m_rawDecoded;
            }
        }

        size_t p = m_rawDecoded;
        switch (m_encoding) {
        case Encoding::Undetermined:
            break;
        case Encoding::Latin1:
            for (; p < limit; ++p)
                m_text.append(raw[p]);
            break;
        case Encoding::ASCII:
            for (; p < limit; ++p)
                m_text.append(raw[p] < 0x80 ? raw[p] : expatInvalidCharacter);
            break;
        case Encoding::Table:
            for (; p < limit; ++p)
                m_text.append(m_table[raw[p]]);
            break;
        case Encoding::UTF16LE:
        case Encoding::UTF16BE: {
            auto unitAt = [&](size_t i) -> char16_t {
                return m_encoding == Encoding::UTF16LE ? raw[i] | (raw[i + 1] << 8) : (raw[i] << 8) | raw[i + 1];
            };
            while (p + 2 <= limit) {
                char16_t unit = unitAt(p);
                if (U16_IS_LEAD(unit) && p + 4 > limit)
                    break; // The other half is to come.
                m_text.append(unit);
                p += 2;
            }
            break;
        }
        case Encoding::UTF8:
            while (p < limit) {
                uint8_t lead = raw[p];
                if (lead < 0x80) {
                    m_text.append(lead);
                    ++p;
                    continue;
                }
                unsigned length = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
                bool isValid = lead >= 0xc2 && lead <= 0xf4;
                unsigned available = static_cast<unsigned>(std::min<size_t>(length, limit - p));
                char32_t c = lead & (0xff >> (length + 1));
                for (unsigned i = 1; isValid && i < available; ++i) {
                    uint8_t byte = raw[p + i];
                    uint8_t low = 0x80, high = 0xbf;
                    if (i == 1) {
                        // Nothing in more bytes than it takes, no surrogates, nothing beyond U+10FFFF.
                        if (lead == 0xe0)
                            low = 0xa0;
                        else if (lead == 0xed)
                            high = 0x9f;
                        else if (lead == 0xf0)
                            low = 0x90;
                        else if (lead == 0xf4)
                            high = 0x8f;
                    }
                    isValid = byte >= low && byte <= high;
                    c = (c << 6) | (byte & 0x3f);
                }
                if (!isValid) {
                    m_text.append(expatInvalidCharacter);
                    p = limit; // Nobody will look beyond.
                    m_hasUndecodableInput = true;
                    break;
                }
                if (available < length)
                    break; // The rest is to come.
                appendCodePointTo(m_text, c);
                p += length;
            }
            break;
        }
        m_rawDecoded = p;
        // (Half a code unit of UTF-16 is overlooked, and what it is the end of is found to be incomplete.)
        bool isHalfUnit = isTwoBytes(m_encoding) && raw.size() - p == 1;
        m_hasHalfUnit = m_isFinal && isHalfUnit;
        if (m_isFinal && p < raw.size() && limit == raw.size() && !m_hasUndecodableInput && m_partialCharacterIndex == notFound && !isHalfUnit) {
            // The input ends in the middle of a character.
            m_partialCharacterIndex = m_text.size();
            m_text.append(expatInvalidCharacter);
        }
    }

    // ---- Calling Python ----

    bool hasHandler(unsigned handler) const { return !!m_owner->get(handler); }
    bool hasDefaultHandler() const { return m_defaultHandler >= 0 && hasHandler(m_defaultHandler); }

    JSValue toStr(const String& string) const { return string.isNull() ? jsUndefined() : JSValue(makeStr(getVM(m_globalObject), string)); }
    JSValue toStr(Text t) const { return makeStr(getVM(m_globalObject), stringOfText(t)); }

    // A str of which there is one for all that are equal to it, if the parser has a dict to keep them in.
    JSValue intern(const String& string)
    {
        if (string.isNull())
            return jsUndefined();
        JSValue result = toStr(string);
        JSValue dict = m_owner->get(ExpatIntern);
        if (!dict)
            return result;
        return PyDict::from(dict)->setDefault(m_globalObject, result, result);
    }

    // After an exception: no handler is called any more, and parsing ends.
    void stopAfterException()
    {
        VM& vm = getVM(m_globalObject);
        for (unsigned i = 0; i < ExpatHandlerCount; ++i)
            m_owner->set(vm, i, { });
        m_defaultHandler = -1;
        m_isStopped = true;
    }

    bool callCharacterHandler(Text data)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSValue handler = m_owner->get(ExpatHandlerCharacterData);
        if (!handler)
            return true;
        {
            SetForScope inCallback(isInCallback, true);
            call(m_globalObject, handler, toStr(data));
        }
        if (scope.exception()) {
            stopAfterException();
            return false;
        }
        return true;
    }

    static int64_t utf8Length(Text t)
    {
        int64_t length = 0;
        for (char16_t c : t)
            length += c < 0x80 ? 1 : c < 0x800 ? 2 : U16_IS_SURROGATE(c) ? 2 : 3;
        return length;
    }

    void reportCharacterData(Text data)
    {
        if (m_isStopped)
            return;
        // The room in the buffer is reckoned in bytes of UTF-8, which is what it holds in CPython.
        int64_t length = utf8Length(data);
        if (buffersText && length > bufferSize - m_characterBufferUsed) {
            if (!flushCharacterBuffer(m_globalObject) || !hasHandler(ExpatHandlerCharacterData))
                return;
        }
        if (!buffersText || length > bufferSize - m_characterBufferUsed) {
            callCharacterHandler(data);
            return;
        }
        m_characterBuffer.append(data);
        m_characterBufferUsed += length;
    }

    // Calls the handler, if there is one. The result, or empty after an exception or if there is no handler.
    JSValue callHandler(unsigned which, std::initializer_list<JSValue> arguments)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (m_isStopped || !hasHandler(which))
            return { };
        if (scope.exception()) {
            stopAfterException();
            return { };
        }
        if (!flushCharacterBuffer(m_globalObject))
            return { };
        JSValue handler = m_owner->get(which);
        if (!handler)
            return { };
        JSValue result;
        {
            SetForScope inCallback(isInCallback, true);
            result = call(m_globalObject, handler, CallArgs(arguments));
        }
        if (scope.exception()) {
            stopAfterException();
            return { };
        }
        return result;
    }

    // For the handlers that answer with a number. 0 after an exception.
    int64_t callIntHandler(unsigned which, ASCIILiteral name, std::initializer_list<JSValue> arguments)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSValue result = callHandler(which, arguments);
        if (!result)
            return 0;
        int64_t number = toInt64(m_globalObject, result);
        if (scope.exception()) {
            JSValue exception = takePendingException(m_globalObject);
            JSValue ignored = callMethod(m_globalObject, exception, internString(vm, "add_note"_s), makeStr(vm, makeString("invalid '"_s, name, "' event handler return value"_s)));
            UNUSED_PARAM(ignored);
            takePendingException(m_globalObject);
            throwPythonException(m_globalObject, scope, exception);
            m_isStopped = true;
            return 0;
        }
        return number;
    }

    // Text that is not in UTF-8 is made so by Expat in a buffer of 1024 bytes, and reported a bufferful at a time.
    template<typename Report>
    void forEachConvertedPiece(const Source& source, size_t start, size_t end, const Report& report)
    {
        Text t = source.text;
        if (!source.isDocument || m_encoding == Encoding::UTF8 || m_encoding == Encoding::ASCII) {
            report(t.subspan(start, end - start));
            return;
        }
        while (true) {
            size_t p = start;
            unsigned room = 1024;
            while (p < end) {
                unsigned units = U16_IS_LEAD(t[p]) && p + 1 < end ? 2 : 1;
                unsigned length = units == 2 ? 4 : t[p] < 0x80 ? 1 : t[p] < 0x800 ? 2 : 3;
                if (length > room)
                    break;
                room -= length;
                p += units;
            }
            report(t.subspan(start, p - start));
            if (p == end || m_isStopped)
                return;
            start = p;
            setEvent(source, start);
        }
    }

    void reportCharacterData(const Source& source, size_t start, size_t end)
    {
        forEachConvertedPiece(source, start, end, [&](Text piece) { reportCharacterData(piece); });
    }

    void reportDefault(const Source& source, size_t start, size_t end)
    {
        if (hasDefaultHandler())
            forEachConvertedPiece(source, start, end, [&](Text piece) { callHandler(m_defaultHandler, { toStr(piece) }); });
    }

    // Where the input ends with half a code unit of UTF-16, there is the start of something.
    ExpatTok unlessHalfUnit(const Source& source, ExpatTok tok) const { return tok == ExpatTok::None && source.isDocument && m_hasHalfUnit ? ExpatTok::Partial : tok; }

    // With "\r\n" and "\r" as "\n".
    static String normalizeLines(Text t)
    {
        Vector<char16_t> out;
        out.reserveInitialCapacity(t.size());
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '\r') {
                out.append('\n');
                if (i + 1 < t.size() && t[i + 1] == '\n')
                    ++i;
            } else
                out.append(t[i]);
        }
        return stringOfText(out.span());
    }

    void reportProcessingInstruction(const Source& source, size_t start, size_t end)
    {
        if (!hasHandler(ExpatHandlerProcessingInstruction)) {
            reportDefault(source, start, end);
            return;
        }
        Text t = source.text;
        size_t target = start + 2;
        size_t p = target;
        while (isNameChar(t[p]))
            ++p;
        size_t targetEnd = p;
        while (isSpace(t[p]))
            ++p;
        callHandler(ExpatHandlerProcessingInstruction, { intern(stringOfText(t, target, targetEnd)), toStr(normalizeLines(t.subspan(p, end - 2 - p))) });
    }

    void reportComment(const Source& source, size_t start, size_t end)
    {
        if (!hasHandler(ExpatHandlerComment)) {
            reportDefault(source, start, end);
            return;
        }
        callHandler(ExpatHandlerComment, { toStr(normalizeLines(source.text.subspan(start + 4, end - start - 7))) });
    }

    // ---- The XML declaration ----

    struct PseudoAttribute {
        size_t nameStart { notFound }; // notFound: there is none.
        size_t nameEnd { 0 };
        size_t valueStart { 0 };
        size_t valueEnd { 0 };
    };

    // False: it is malformed at `p`.
    static bool parsePseudoAttribute(Text t, size_t& p, size_t end, PseudoAttribute& attribute)
    {
        attribute.nameStart = notFound;
        if (p == end)
            return true;
        if (!isSpace(t[p]))
            return false;
        while (p < end && isSpace(t[p]))
            ++p;
        if (p == end)
            return true;
        size_t nameStart = p;
        for (;; ++p) {
            if (p == end || t[p] >= 0x80)
                return false;
            if (t[p] == '=') {
                attribute.nameEnd = p;
                break;
            }
            if (isSpace(t[p])) {
                attribute.nameEnd = p;
                while (p < end && isSpace(t[p]))
                    ++p;
                if (p == end || t[p] != '=')
                    return false;
                break;
            }
        }
        if (p == nameStart)
            return false;
        ++p;
        while (p < end && isSpace(t[p]))
            ++p;
        if (p == end || (t[p] != '"' && t[p] != '\''))
            return false;
        char16_t quote = t[p++];
        attribute.valueStart = p;
        for (;; ++p) {
            if (p == end)
                return false;
            if (t[p] == quote)
                break;
            if (!isASCIIAlphanumeric(t[p]) && t[p] != '.' && t[p] != '-' && t[p] != '_')
                return false;
        }
        attribute.valueEnd = p++;
        attribute.nameStart = nameStart;
        return true;
    }

    // `isTextDeclaration`: at the start of an external entity, where there has to be an encoding and may be no version.
    ExpatError processXmlDeclaration(const Source& source, size_t start, size_t next, bool isTextDeclaration)
    {
        Text t = source.text;
        size_t p = start + 5;
        size_t end = next - 2;
        String version, encodingName;
        size_t encodingStart = 0;
        int standalone = -1;
        auto parse = [&]() -> bool {
            PseudoAttribute attribute;
            if (!parsePseudoAttribute(t, p, end, attribute) || attribute.nameStart == notFound)
                return false;
            if (!matches(t, attribute.nameStart, attribute.nameEnd, "version"_s)) {
                if (!isTextDeclaration) {
                    p = attribute.nameStart;
                    return false;
                }
            } else {
                version = stringOfText(t, attribute.valueStart, attribute.valueEnd);
                if (!parsePseudoAttribute(t, p, end, attribute))
                    return false;
                if (attribute.nameStart == notFound)
                    return !isTextDeclaration;
            }
            if (matches(t, attribute.nameStart, attribute.nameEnd, "encoding"_s)) {
                if (attribute.valueStart == attribute.valueEnd || !isASCIIAlpha(t[attribute.valueStart])) {
                    p = attribute.valueStart;
                    return false;
                }
                encodingName = stringOfText(t, attribute.valueStart, attribute.valueEnd);
                encodingStart = attribute.valueStart;
                if (!parsePseudoAttribute(t, p, end, attribute))
                    return false;
                if (attribute.nameStart == notFound)
                    return true;
            }
            if (!matches(t, attribute.nameStart, attribute.nameEnd, "standalone"_s) || isTextDeclaration) {
                p = attribute.nameStart;
                return false;
            }
            if (matches(t, attribute.valueStart, attribute.valueEnd, "yes"_s))
                standalone = 1;
            else if (matches(t, attribute.valueStart, attribute.valueEnd, "no"_s))
                standalone = 0;
            else {
                p = attribute.valueStart;
                return false;
            }
            while (p < end && isSpace(t[p]))
                ++p;
            return p == end;
        };
        if (!parse()) {
            setEvent(source, p);
            return isTextDeclaration ? ExpatErrorTextDecl : ExpatErrorXmlDecl;
        }
        if (!isTextDeclaration && standalone == 1) {
            m_dtd->isStandalone = true;
            if (m_paramEntityParsing == 1)
                m_paramEntityParsing = 0;
        }
        if (hasHandler(ExpatHandlerXmlDecl))
            callHandler(ExpatHandlerXmlDecl, { toStr(version), toStr(encodingName), jsNumber(standalone) });
        else
            reportDefault(source, start, next);

        m_mayHaveEncodingDeclaration = false;
        if (!m_protocolEncoding.isNull() || encodingName.isNull())
            return ExpatErrorNone;
        Encoding declared = encodingNamed(encodingName);
        if (declared == Encoding::Undetermined) {
            // The position is reckoned in the encoding so far, up to here.
            auto saved = std::tuple { m_positionIndex, m_line, m_column, m_byte };
            advancePositionTo(next);
            bool wasTwoBytes = isTwoBytes(m_encoding);
            ExpatError error = useUnknownEncoding(encodingName);
            if (!error && wasTwoBytes) {
                // Taken at its word: what follows is read again, byte by byte.
                m_rawDecoded = m_byte - m_rawBase;
                m_text.shrink(next);
            }
            if (error == ExpatErrorUnknownEncoding) {
                std::tie(m_positionIndex, m_line, m_column, m_byte) = saved;
                setEvent(source, encodingStart);
            }
            return error;
        }
        if (equalLettersIgnoringASCIICase(encodingName, "utf-16"_s) && isTwoBytes(m_encoding))
            return ExpatErrorNone;
        if (isTwoBytes(declared) != isTwoBytes(m_encoding) || (isTwoBytes(declared) && declared != m_encoding)) {
            setEvent(source, encodingStart);
            return ExpatErrorIncorrectEncoding;
        }
        advancePositionTo(next);
        m_encoding = declared;
        return ExpatErrorNone;
    }

    // ---- Entities ----

    // Entities that expand to many times the input are an attack.
    ExpatError accountForExpansion(size_t length)
    {
        ExpatDTD& dtd = m_dtd.get();
        dtd.indirectBytes += length;
        uint64_t total = dtd.directBytes + dtd.indirectBytes;
        if (total >= dtd.expansionThreshold && dtd.directBytes && static_cast<float>(total) / static_cast<float>(dtd.directBytes) > dtd.maximumExpansion)
            return ExpatErrorAmplificationLimitBreach;
        return ExpatErrorNone;
    }

    // So is a document that makes the parser keep many times the input.
    ExpatError accountForStorage(size_t bytes)
    {
        ExpatDTD& dtd = m_dtd.get();
        dtd.storedBytes += bytes;
        if (dtd.storedBytes >= dtd.storageThreshold && static_cast<float>(dtd.storedBytes) / static_cast<float>(dtd.directBytes) > dtd.maximumStorage)
            return ExpatErrorNoMemory;
        return ExpatErrorNone;
    }

    ExpatEntity* generalEntity(const String& name) const
    {
        auto it = m_dtd->generalEntities.find(name);
        return it == m_dtd->generalEntities.end() ? nullptr : it->value.get();
    }

    // The names of the entities that are open and the namespaces in scope, for the parser of an external entity to start from.
    String context() const
    {
        StrBuilder result;
        auto separate = [&] {
            if (!result.isEmpty())
                result.append('\f');
        };
        if (auto it = m_bindings.find(emptyString()); it != m_bindings.end() && !it->value.isEmpty() && !it->value.last().isNull()) {
            result.append('=', it->value.last());
        }
        Vector<String> prefixes;
        for (auto& entry : m_bindings) {
            if (!entry.key.isEmpty() && !entry.value.isEmpty() && m_hasNamespaces)
                prefixes.append(entry.key);
        }
        std::sort(prefixes.begin(), prefixes.end(), WTF::codePointCompareLessThan);
        for (auto& prefix : prefixes) {
            separate();
            result.append(prefix, '=', m_bindings.get(prefix).last());
        }
        Vector<String> open;
        for (auto& entry : m_dtd->generalEntities) {
            if (entry.value->isOpen)
                open.append(entry.key);
        }
        std::sort(open.begin(), open.end(), WTF::codePointCompareLessThan);
        for (auto& name : open) {
            separate();
            result.append(name);
        }
        return result.toString();
    }

    void setContext(const String& context)
    {
        for (auto part : StringView(context).split('\f')) {
            size_t equals = part.find('=');
            if (equals == notFound) {
                if (ExpatEntity* entity = generalEntity(part.toString()))
                    entity->isOpen = true;
                m_openedByContext.append(part.toString());
                continue;
            }
            m_bindings.ensure(part.left(equals).toString(), [] { return Vector<String>(); }).iterator->value.append(part.substring(equals + 1).toString());
        }
    }

    // The value of an attribute as it is passed on: references replaced, white space as spaces, and, if it is not CDATA, no
    // more of those than one between words.
    ExpatError appendAttributeValue(const Source& source, size_t start, size_t end, bool isCData, bool isInProlog, Vector<char16_t>& out)
    {
        Text t = source.text.first(end);
        size_t p = start;
        while (true) {
            size_t next = p;
            ExpatTok tok = valueTok(t, p, next, false);
            switch (tok) {
            case ExpatTok::None:
                return ExpatErrorNone;
            case ExpatTok::Invalid:
                setEvent(source, next);
                return ExpatErrorInvalidToken;
            case ExpatTok::Partial:
            case ExpatTok::PartialChar:
                setEvent(source, p);
                return ExpatErrorInvalidToken;
            case ExpatTok::CharRef: {
                int32_t c = charRefNumber(t, p);
                if (c < 0) {
                    setEvent(source, p);
                    return ExpatErrorBadCharRef;
                }
                if (!isCData && c == ' ' && (out.isEmpty() || out.last() == ' '))
                    break;
                appendCodePointTo(out, c);
                break;
            }
            case ExpatTok::DataChars:
                out.append(t.subspan(p, next - p));
                break;
            case ExpatTok::TrailingCR:
                next = p + 1;
                [[fallthrough]];
            case ExpatTok::AttributeValueS:
            case ExpatTok::DataNewline:
                if (!isCData && (out.isEmpty() || out.last() == ' '))
                    break;
                out.append(' ');
                break;
            case ExpatTok::EntityRef: {
                if (char16_t c = predefinedEntity(t, p + 1, next - 1)) {
                    out.append(c);
                    break;
                }
                ExpatEntity* entity = generalEntity(stringOfText(t, p + 1, next - 1));
                bool mustBeDeclared;
                if (isInProlog)
                    mustBeDeclared = !m_isParameterEntity && (m_dtd->isStandalone ? !m_openInternalEntities : !m_dtd->hasParamEntityRefs);
                else
                    mustBeDeclared = !m_dtd->hasParamEntityRefs || m_dtd->isStandalone;
                if (mustBeDeclared) {
                    if (!entity)
                        return ExpatErrorUndefinedEntity;
                    if (!entity->isInternal)
                        return ExpatErrorEntityDeclaredInPe;
                } else if (!entity)
                    break;
                if (entity->isOpen) {
                    setEvent(source, p);
                    return ExpatErrorRecursiveEntityRef;
                }
                if (!entity->notation.isNull()) {
                    setEvent(source, p);
                    return ExpatErrorBinaryEntityRef;
                }
                if (!entity->hasText) {
                    setEvent(source, p);
                    return ExpatErrorAttributeExternalEntityRef;
                }
                if (ExpatError error = accountForExpansion(entity->text.size()))
                    return error;
                entity->isOpen = true;
                SetForScope inReplacementText(m_isInReplacementText, true);
                Source replacement { entity->text.span(), false };
                ExpatError error = appendAttributeValue(replacement, 0, entity->text.size(), isCData, isInProlog, out);
                entity->isOpen = false;
                if (error)
                    return error;
                break;
            }
            default:
                setEvent(source, p);
                return ExpatErrorUnexpectedState;
            }
            p = next;
        }
    }

    ExpatError storeAttributeValue(const Source& source, size_t start, size_t end, bool isCData, bool isInProlog, String& result)
    {
        Vector<char16_t> out;
        if (ExpatError error = appendAttributeValue(source, start, end, isCData, isInProlog, out))
            return error;
        if (!isCData && !out.isEmpty() && out.last() == ' ')
            out.removeLast();
        result = stringOfText(out.span());
        return ExpatErrorNone;
    }

    // The replacement text of an entity, from the literal in its declaration.
    ExpatError storeEntityValue(const Source& source, size_t start, size_t end, Vector<char16_t>& out)
    {
        Text t = source.text.first(end);
        size_t p = start;
        while (true) {
            size_t next = p;
            ExpatTok tok = valueTok(t, p, next, true);
            switch (tok) {
            case ExpatTok::None:
                return ExpatErrorNone;
            case ExpatTok::ParamEntityRef: {
                if (!m_isParameterEntity && source.isDocument) {
                    // In the internal subset they are allowed between declarations only.
                    setEvent(source, p);
                    return ExpatErrorParamEntityRef;
                }
                auto it = m_dtd->parameterEntities.find(stringOfText(t, p + 1, next - 1));
                if (it == m_dtd->parameterEntities.end()) {
                    m_dtd->keepProcessing = m_dtd->isStandalone;
                    return ExpatErrorNone;
                }
                ExpatEntity* entity = it->value.get();
                if (entity->isOpen || entity == m_declaredEntity) {
                    setEvent(source, p);
                    return ExpatErrorRecursiveEntityRef;
                }
                if (!entity->hasText) {
                    m_dtd->keepProcessing = m_dtd->isStandalone;
                    break;
                }
                entity->isOpen = true;
                SetForScope inReplacementText(m_isInReplacementText, true);
                Source replacement { entity->text.span(), false };
                ExpatError error = storeEntityValue(replacement, 0, entity->text.size(), out);
                entity->isOpen = false;
                if (error)
                    return error;
                break;
            }
            case ExpatTok::EntityRef:
            case ExpatTok::DataChars:
                out.append(t.subspan(p, next - p));
                break;
            case ExpatTok::TrailingCR:
                next = p + 1;
                [[fallthrough]];
            case ExpatTok::DataNewline:
                out.append('\n');
                break;
            case ExpatTok::CharRef: {
                int32_t c = charRefNumber(t, p);
                if (c < 0) {
                    setEvent(source, p);
                    return ExpatErrorBadCharRef;
                }
                appendCodePointTo(out, c);
                break;
            }
            case ExpatTok::Partial:
            case ExpatTok::PartialChar:
                setEvent(source, p);
                return ExpatErrorInvalidToken;
            case ExpatTok::Invalid:
                setEvent(source, next);
                return ExpatErrorInvalidToken;
            default:
                setEvent(source, p);
                return ExpatErrorUnexpectedState;
            }
            p = next;
        }
    }

    // ---- Start tags ----

    // Where the attributes of the tag that starts at `start` are.
    void findAttributes(Text t, size_t start, Vector<RawAttribute, 8>& attributes, size_t& nameEnd) const
    {
        size_t p = start + 1;
        while (isNameChar(t[p]) || t[p] == ':')
            ++p;
        nameEnd = p;
        while (true) {
            while (isSpace(t[p]))
                ++p;
            if (t[p] == '>' || t[p] == '/')
                return;
            RawAttribute attribute;
            attribute.nameStart = p;
            while (isNameChar(t[p]) || t[p] == ':')
                ++p;
            attribute.nameEnd = p;
            while (t[p] != '"' && t[p] != '\'')
                ++p;
            char16_t quote = t[p++];
            attribute.valueStart = p;
            while (t[p] != quote)
                ++p;
            attribute.valueEnd = p++;
            attributes.append(attribute);
        }
    }

    static bool isURICharacter(char16_t c)
    {
        return isASCIIAlphanumeric(c) || (c < 0x80 && c && WTF::contains("-._~%:/?#[]@!$&'()*+,;="_span, static_cast<char>(c)));
    }

    // xmlns:prefix="uri", or, with an empty prefix, xmlns="uri".
    ExpatError addBinding(const String& prefix, const String& uri, bool isFromAttribute, Vector<Binding>& bindings)
    {
        if (uri.isEmpty() && !prefix.isEmpty())
            return ExpatErrorUndeclaringPrefix;
        bool mustBeXml = false;
        if (prefix == "xmlns"_s)
            return ExpatErrorReservedPrefixXmlns;
        if (prefix == "xml"_s)
            mustBeXml = true;
        bool isXml = uri == expatXmlNamespace;
        if (mustBeXml != isXml)
            return mustBeXml ? ExpatErrorReservedPrefixXml : ExpatErrorReservedNamespaceUri;
        if (uri == expatXmlnsNamespace)
            return ExpatErrorReservedNamespaceUri;
        // The separator in a URI would make what is passed on ambiguous.
        if (m_separator && !isURICharacter(m_separator) && uri.contains(m_separator))
            return ExpatErrorSyntax;
        bool undeclaresDefault = uri.isEmpty() && prefix.isEmpty();
        m_bindings.ensure(prefix, [] { return Vector<String>(); }).iterator->value.append(undeclaresDefault ? String() : uri);
        bindings.append({ prefix });
        if (isFromAttribute)
            callHandler(ExpatHandlerStartNamespaceDecl, { intern(prefix.isEmpty() ? String() : prefix), intern(undeclaresDefault ? String() : uri) });
        return ExpatErrorNone;
    }

    // Null: the prefix is not bound.
    String namespaceOf(const String& prefix) const
    {
        auto it = m_bindings.find(prefix);
        return it == m_bindings.end() || it->value.isEmpty() ? String() : it->value.last();
    }

    String expandedName(const String& uri, StringView localName, StringView prefix) const
    {
        StrBuilder result;
        result.append(uri);
        if (m_separator)
            result.append(m_separator);
        result.append(localName);
        // Without a separator there is no telling the prefix from the name, and it is left out.
        if (m_returnsTriplets && !prefix.isEmpty() && m_separator)
            result.append(m_separator, prefix);
        return result.toString();
    }

    void endBindings(Vector<Binding>& bindings)
    {
        while (!bindings.isEmpty()) {
            Binding binding = bindings.takeLast();
            callHandler(ExpatHandlerEndNamespaceDecl, { intern(binding.prefix.isEmpty() ? String() : binding.prefix) });
            m_bindings.find(binding.prefix)->value.removeLast();
        }
    }

    struct Attribute {
        String name;
        String value;
        bool hasPrefix;
    };

    // Works out the attributes of a start tag, and with namespaces what its name and theirs stand for.
    ExpatError storeAttributes(const Source& source, size_t start, Tag& tag, Vector<Attribute, 8>& attributes, unsigned& specifiedCount)
    {
        Text t = source.text;
        Vector<RawAttribute, 8> raw;
        size_t nameEnd;
        findAttributes(t, start, raw, nameEnd);
        tag.rawName = stringOfText(t, start + 1, nameEnd);
        tag.name = tag.rawName;

        ExpatElementType* elementType = nullptr;
        if (auto it = m_dtd->elementTypes.find(tag.rawName); it != m_dtd->elementTypes.end())
            elementType = &it->value;

        auto isNamespaceDeclaration = [&](const String& name, String& prefix) {
            if (!m_hasNamespaces || !name.startsWith("xmlns"_s))
                return false;
            if (name.length() == 5) {
                prefix = emptyString();
                return true;
            }
            if (name[5] != ':')
                return false;
            prefix = name.substring(6);
            return true;
        };

        HashSet<String> seen;
        for (auto& rawAttribute : raw) {
            String name = stringOfText(t, rawAttribute.nameStart, rawAttribute.nameEnd);
            if (!seen.add(name).isNewEntry) {
                setEvent(source, rawAttribute.nameStart);
                return ExpatErrorDuplicateAttribute;
            }
            bool isCData = true;
            if (elementType) {
                for (auto& declaration : elementType->attributes) {
                    if (declaration.name == name) {
                        isCData = declaration.isCData;
                        break;
                    }
                }
            }
            String value;
            if (ExpatError error = storeAttributeValue(source, rawAttribute.valueStart, rawAttribute.valueEnd, isCData, false, value))
                return error;
            String prefix;
            if (isNamespaceDeclaration(name, prefix)) {
                if (ExpatError error = addBinding(prefix, value, true, tag.bindings))
                    return error;
                continue;
            }
            attributes.append({ name, value, m_hasNamespaces && name.contains(':') });
        }
        specifiedCount = attributes.size();

        if (elementType) {
            for (auto& declaration : elementType->attributes) {
                if (declaration.value.isNull() || seen.contains(declaration.name))
                    continue;
                String prefix;
                if (isNamespaceDeclaration(declaration.name, prefix)) {
                    if (ExpatError error = addBinding(prefix, declaration.value, true, tag.bindings))
                        return error;
                    continue;
                }
                attributes.append({ declaration.name, declaration.value, m_hasNamespaces && declaration.name.contains(':') });
            }
        }
        if (!m_hasNamespaces)
            return ExpatErrorNone;

        HashSet<String> expanded;
        for (auto& attribute : attributes) {
            if (!attribute.hasPrefix)
                continue;
            size_t colon = attribute.name.find(':');
            String prefix = attribute.name.left(colon);
            String uri = namespaceOf(prefix);
            if (uri.isNull())
                return ExpatErrorUnboundPrefix;
            StringView localName = StringView(attribute.name).substring(colon + 1);
            // Two prefixes for one namespace do not make two attributes.
            if (!expanded.add(makeString(uri, '\0', localName)).isNewEntry)
                return ExpatErrorDuplicateAttribute;
            attribute.name = expandedName(uri, localName, prefix);
        }

        size_t colon = tag.rawName.find(':');
        String prefix = colon == notFound ? emptyString() : tag.rawName.left(colon);
        String uri = namespaceOf(prefix);
        if (uri.isNull())
            return colon == notFound ? ExpatErrorNone : ExpatErrorUnboundPrefix;
        tag.name = expandedName(uri, colon == notFound ? StringView(tag.rawName) : StringView(tag.rawName).substring(colon + 1), prefix);
        return ExpatErrorNone;
    }

    void reportStartElement(const Tag& tag, const Vector<Attribute, 8>& attributes, unsigned specifiedCount)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (m_isStopped || !hasHandler(ExpatHandlerStartElement) || !flushCharacterBuffer(m_globalObject))
            return;
        unsigned count = specifiedAttributes ? specifiedCount : attributes.size();
        JSValue container;
        if (orderedAttributes) {
            JSArray* list = newList(m_globalObject, count * 2);
            for (unsigned i = 0; i < count; ++i) {
                listAppend(m_globalObject, list, intern(attributes[i].name));
                listAppend(m_globalObject, list, toStr(attributes[i].value));
            }
            container = list;
        } else {
            PyDict* dict = PyDict::create(m_globalObject);
            for (unsigned i = 0; i < count; ++i)
                dict->set(m_globalObject, intern(attributes[i].name), toStr(attributes[i].value));
            container = dict;
        }
        if (scope.exception()) {
            stopAfterException();
            return;
        }
        scope.release();
        callHandler(ExpatHandlerStartElement, { intern(tag.name), container });
    }

    // ---- Content ----

    // Parses the replacement text of an entity in place of the reference to it.
    ExpatError processInternalEntity(ExpatEntity* entity, bool isBetweenDeclarations)
    {
        if (ExpatError error = accountForExpansion(entity->text.size()))
            return error;
        if (!getVM(m_globalObject).isSafeToRecurseSoft())
            return ExpatErrorNoMemory;
        entity->isOpen = true;
        ++m_openInternalEntities;
        SetForScope inReplacementText(m_isInReplacementText, true);
        Source replacement { entity->text.span(), false };
        size_t next = 0;
        ExpatError error;
        if (entity->isParameter) {
            SetForScope between(m_isBetweenDeclarations, isBetweenDeclarations);
            error = doProlog(replacement, 0, false, next);
        } else
            error = doContent(replacement, 0, false, m_tags.size(), next);
        --m_openInternalEntities;
        entity->isOpen = false;
        return error;
    }

    // `baseLevel`: how many elements were open where the text starts; it may not close those. 0 for the document itself.
    ExpatError doContent(const Source& source, size_t s, bool haveMore, size_t baseLevel, size_t& resume)
    {
        Text t = source.text;
        bool isTopLevel = source.isDocument && !m_isChild;
        while (true) {
            size_t next = s;
            ExpatTok tok = unlessHalfUnit(source, contentTok(t, s, next));
            setEvent(source, s);
            switch (tok) {
            case ExpatTok::TrailingCR:
                if (haveMore) {
                    resume = s;
                    return ExpatErrorNone;
                }
                if (hasHandler(ExpatHandlerCharacterData)) {
                    char16_t newline = '\n';
                    reportCharacterData({ &newline, 1 });
                } else
                    reportDefault(source, s, t.size());
                if (isTopLevel)
                    return ExpatErrorNoElements;
                if (m_tags.size() != baseLevel)
                    return ExpatErrorAsyncEntity;
                resume = t.size();
                return ExpatErrorNone;
            case ExpatTok::None:
                if (haveMore) {
                    resume = s;
                    return ExpatErrorNone;
                }
                if (!isTopLevel) {
                    if (m_tags.size() != baseLevel)
                        return ExpatErrorAsyncEntity;
                    resume = s;
                    return ExpatErrorNone;
                }
                return ExpatErrorNoElements;
            case ExpatTok::Invalid:
                setEvent(source, next);
                return ExpatErrorInvalidToken;
            case ExpatTok::Partial:
                if (haveMore) {
                    resume = s;
                    return ExpatErrorNone;
                }
                return ExpatErrorUnclosedToken;
            case ExpatTok::PartialChar:
                if (haveMore) {
                    resume = s;
                    return ExpatErrorNone;
                }
                return ExpatErrorPartialChar;
            case ExpatTok::EntityRef: {
                if (char16_t c = predefinedEntity(t, s + 1, next - 1)) {
                    if (hasHandler(ExpatHandlerCharacterData))
                        reportCharacterData({ &c, 1 });
                    else
                        reportDefault(source, s, next);
                    break;
                }
                String name = stringOfText(t, s + 1, next - 1);
                ExpatEntity* entity = generalEntity(name);
                // Whether it has to have been declared depends on whether all declarations have been read.
                if (!m_dtd->hasParamEntityRefs || m_dtd->isStandalone) {
                    if (!entity)
                        return ExpatErrorUndefinedEntity;
                    if (!entity->isInternal)
                        return ExpatErrorEntityDeclaredInPe;
                } else if (!entity) {
                    if (hasHandler(ExpatHandlerSkippedEntity))
                        callHandler(ExpatHandlerSkippedEntity, { intern(name), jsNumber(0) });
                    else
                        reportDefault(source, s, next);
                    break;
                }
                if (entity->isOpen)
                    return ExpatErrorRecursiveEntityRef;
                if (!entity->notation.isNull())
                    return ExpatErrorBinaryEntityRef;
                if (entity->hasText) {
                    if (!m_expandsInternalEntities) {
                        if (hasHandler(ExpatHandlerSkippedEntity))
                            callHandler(ExpatHandlerSkippedEntity, { intern(entity->name), jsNumber(0) });
                        else
                            reportDefault(source, s, next);
                        break;
                    }
                    if (ExpatError error = processInternalEntity(entity, false))
                        return error;
                } else if (hasHandler(ExpatHandlerExternalEntityRef)) {
                    entity->isOpen = true;
                    String openContext = context();
                    entity->isOpen = false;
                    int64_t result = callIntHandler(ExpatHandlerExternalEntityRef, "ExternalEntityRef"_s, { toStr(openContext), intern(entity->base), intern(entity->systemId), intern(entity->publicId) });
                    if (!result)
                        return ExpatErrorExternalEntityHandling;
                } else
                    reportDefault(source, s, next);
                break;
            }
            case ExpatTok::StartTag:
            case ExpatTok::EmptyElement: {
                Tag tag;
                Vector<Attribute, 8> attributes;
                unsigned specifiedCount = 0;
                if (ExpatError error = storeAttributes(source, s, tag, attributes, specifiedCount)) {
                    // What has been told of the start of a namespace is told of its end, as far as Expat does that.
                    if (tok == ExpatTok::EmptyElement)
                        endBindings(tag.bindings);
                    return error;
                }
                if (tok == ExpatTok::StartTag) {
                    if (hasHandler(ExpatHandlerStartElement))
                        reportStartElement(tag, attributes, specifiedCount);
                    else
                        reportDefault(source, s, next);
                    m_tags.append(WTF::move(tag));
                    if (m_tags.size() > m_deepestLevel) {
                        m_deepestLevel = m_tags.size();
                        if (ExpatError error = accountForStorage(sizeof(Tag) + next - s))
                            return error;
                    }
                    break;
                }
                bool hasElementHandlers = false;
                if (hasHandler(ExpatHandlerStartElement)) {
                    reportStartElement(tag, attributes, specifiedCount);
                    hasElementHandlers = true;
                }
                if (hasHandler(ExpatHandlerEndElement)) {
                    if (hasElementHandlers)
                        setEvent(source, next);
                    callHandler(ExpatHandlerEndElement, { intern(tag.name) });
                    hasElementHandlers = true;
                }
                if (!hasElementHandlers)
                    reportDefault(source, s, next);
                endBindings(tag.bindings);
                if (m_tags.isEmpty() && isTopLevel && !m_isStopped) {
                    m_processor = Processor::Epilog;
                    return doEpilog(source, next, haveMore, resume);
                }
                break;
            }
            case ExpatTok::EndTag: {
                if (m_tags.size() == baseLevel)
                    return ExpatErrorAsyncEntity;
                size_t nameEnd = s + 2;
                while (isNameChar(t[nameEnd]) || t[nameEnd] == ':')
                    ++nameEnd;
                Tag& top = m_tags.last();
                if (!WTF::equal(StringView(top.rawName), StringView(t.subspan(s + 2, nameEnd - s - 2)))) {
                    setEvent(source, s + 2);
                    return ExpatErrorTagMismatch;
                }
                Tag tag = m_tags.takeLast();
                if (hasHandler(ExpatHandlerEndElement))
                    callHandler(ExpatHandlerEndElement, { intern(tag.name) });
                else
                    reportDefault(source, s, next);
                endBindings(tag.bindings);
                if (m_tags.isEmpty() && isTopLevel && !m_isStopped) {
                    m_processor = Processor::Epilog;
                    return doEpilog(source, next, haveMore, resume);
                }
                break;
            }
            case ExpatTok::CharRef: {
                int32_t c = charRefNumber(t, s);
                if (c < 0)
                    return ExpatErrorBadCharRef;
                if (hasHandler(ExpatHandlerCharacterData)) {
                    Vector<char16_t, 2> units;
                    if (c < 0x10000)
                        units.append(static_cast<char16_t>(c));
                    else {
                        units.append(U16_LEAD(c));
                        units.append(U16_TRAIL(c));
                    }
                    reportCharacterData(units.span());
                } else
                    reportDefault(source, s, next);
                break;
            }
            case ExpatTok::XmlDecl:
                return ExpatErrorMisplacedXmlPi;
            case ExpatTok::DataNewline:
                if (hasHandler(ExpatHandlerCharacterData)) {
                    char16_t newline = '\n';
                    reportCharacterData({ &newline, 1 });
                } else
                    reportDefault(source, s, next);
                break;
            case ExpatTok::CdataSectOpen: {
                if (hasHandler(ExpatHandlerStartCdataSection))
                    callHandler(ExpatHandlerStartCdataSection, { });
                else
                    reportDefault(source, s, next);
                bool isClosed = false;
                if (ExpatError error = doCdataSection(source, next, haveMore, next, isClosed))
                    return error;
                if (!isClosed) {
                    m_processor = Processor::CdataSection;
                    resume = next;
                    return ExpatErrorNone;
                }
                break;
            }
            case ExpatTok::TrailingRSQB:
                if (haveMore) {
                    resume = s;
                    return ExpatErrorNone;
                }
                if (hasHandler(ExpatHandlerCharacterData))
                    reportCharacterData(t.subspan(s));
                else
                    reportDefault(source, s, t.size());
                if (isTopLevel) {
                    setEvent(source, t.size());
                    return ExpatErrorNoElements;
                }
                if (m_tags.size() != baseLevel) {
                    setEvent(source, t.size());
                    return ExpatErrorAsyncEntity;
                }
                resume = t.size();
                return ExpatErrorNone;
            case ExpatTok::DataChars:
                if (hasHandler(ExpatHandlerCharacterData))
                    reportCharacterData(source, s, next);
                else
                    reportDefault(source, s, next);
                break;
            case ExpatTok::PI:
                reportProcessingInstruction(source, s, next);
                break;
            case ExpatTok::Comment:
                reportComment(source, s, next);
                break;
            default:
                reportDefault(source, s, next);
                break;
            }
            setEvent(source, next);
            if (m_isStopped)
                return ExpatErrorAborted;
            s = next;
        }
    }

    // From after "<![CDATA[". `isClosed`: the end has been found; otherwise it is to come.
    ExpatError doCdataSection(const Source& source, size_t s, bool haveMore, size_t& resume, bool& isClosed)
    {
        Text t = source.text;
        while (true) {
            size_t next = s;
            ExpatTok tok = cdataSectionTok(t, s, next);
            setEvent(source, s);
            switch (tok) {
            case ExpatTok::CdataSectClose:
                if (hasHandler(ExpatHandlerEndCdataSection))
                    callHandler(ExpatHandlerEndCdataSection, { });
                else
                    reportDefault(source, s, next);
                resume = next;
                isClosed = true;
                return m_isStopped ? ExpatErrorAborted : ExpatErrorNone;
            case ExpatTok::DataNewline:
                if (hasHandler(ExpatHandlerCharacterData)) {
                    char16_t newline = '\n';
                    reportCharacterData({ &newline, 1 });
                } else
                    reportDefault(source, s, next);
                break;
            case ExpatTok::DataChars:
                if (hasHandler(ExpatHandlerCharacterData))
                    reportCharacterData(source, s, next);
                else
                    reportDefault(source, s, next);
                break;
            case ExpatTok::Invalid:
                setEvent(source, next);
                return ExpatErrorInvalidToken;
            case ExpatTok::PartialChar:
                if (haveMore) {
                    resume = s;
                    return ExpatErrorNone;
                }
                return ExpatErrorPartialChar;
            case ExpatTok::Partial:
            case ExpatTok::None:
                if (haveMore) {
                    resume = s;
                    return ExpatErrorNone;
                }
                return ExpatErrorUnclosedCdataSection;
            default:
                return ExpatErrorUnexpectedState;
            }
            setEvent(source, next);
            if (m_isStopped)
                return ExpatErrorAborted;
            s = next;
        }
    }

    // From after "<![IGNORE[". `isClosed`: the end has been found; otherwise it is to come.
    ExpatError doIgnoreSection(const Source& source, size_t s, bool haveMore, size_t& resume, bool& isClosed)
    {
        size_t next = s;
        setEvent(source, s);
        switch (ignoreSectionTok(source.text, s, next)) {
        case ExpatTok::IgnoreSect:
            reportDefault(source, s, next);
            resume = next;
            isClosed = true;
            return m_isStopped ? ExpatErrorAborted : ExpatErrorNone;
        case ExpatTok::Invalid:
            setEvent(source, next);
            return ExpatErrorInvalidToken;
        case ExpatTok::PartialChar:
            if (!haveMore)
                return ExpatErrorPartialChar;
            break;
        default:
            if (!haveMore)
                return ExpatErrorSyntax;
            break;
        }
        resume = s;
        return ExpatErrorNone;
    }

    // After the end of the document element.
    ExpatError doEpilog(const Source& source, size_t s, bool haveMore, size_t& resume)
    {
        Text t = source.text;
        setEvent(source, s);
        while (true) {
            size_t next = s;
            bool isProvisional;
            ExpatTok tok = unlessHalfUnit(source, prologTok(t, s, next, isProvisional));
            if (isProvisional && tok == ExpatTok::PrologS) {
                // A carriage return at the very end.
                reportDefault(source, s, next);
                if (m_isStopped)
                    return ExpatErrorAborted;
                resume = next;
                return ExpatErrorNone;
            }
            switch (tok) {
            case ExpatTok::None:
                resume = s;
                return ExpatErrorNone;
            case ExpatTok::PrologS:
                reportDefault(source, s, next);
                break;
            case ExpatTok::PI:
                reportProcessingInstruction(source, s, next);
                break;
            case ExpatTok::Comment:
                reportComment(source, s, next);
                break;
            case ExpatTok::Invalid:
                setEvent(source, next);
                return ExpatErrorInvalidToken;
            case ExpatTok::Partial:
                if (haveMore) {
                    resume = s;
                    return ExpatErrorNone;
                }
                return ExpatErrorUnclosedToken;
            case ExpatTok::PartialChar:
                if (haveMore) {
                    resume = s;
                    return ExpatErrorNone;
                }
                return ExpatErrorPartialChar;
            default:
                return ExpatErrorJunkAfterDocElement;
            }
            s = next;
            setEvent(source, s);
            if (m_isStopped)
                return ExpatErrorAborted;
        }
    }

    // ---- The prolog ----

    // For a token that the grammar has no place for. In an external entity a parameter entity may be referred to anywhere.
    ExpatRole prologError(ExpatTok tok)
    {
        if (m_isParameterEntity && tok == ExpatTok::ParamEntityRef)
            return ExpatRole::InnerParamEntityRef;
        m_prologState = ExpatPrologState::Error;
        return ExpatRole::Error;
    }

    ExpatPrologState subsetState() const { return m_isParameterEntity ? ExpatPrologState::ExternalSubset : ExpatPrologState::InternalSubset; }

    // What the token means where the grammar is, which moves on.
    ExpatRole tokenRole(ExpatTok tok, Text t, size_t s, size_t next)
    {
        using S = ExpatPrologState;
        using R = ExpatRole;
        using T = ExpatTok;
        auto go = [&](S state, R role) {
            m_prologState = state;
            return role;
        };
        auto closeDeclaration = [&](R roleNone, R role) {
            m_roleNone = roleNone;
            return go(S::DeclClose, role);
        };
        auto isName = [&](ASCIILiteral word) { return matches(t, s, next, word); };
        bool isAnyName = tok == T::Name || tok == T::PrefixedName;

        switch (m_prologState) {
        case S::Prolog0:
            if (tok == T::XmlDecl)
                return go(S::Prolog1, R::XmlDecl);
            if (tok == T::PrologS)
                return go(S::Prolog1, R::None);
            [[fallthrough]];
        case S::Prolog1:
            switch (tok) {
            case T::PrologS:
            case T::BOM:
                return R::None;
            case T::PI:
                return go(S::Prolog1, R::PI);
            case T::Comment:
                return go(S::Prolog1, R::Comment);
            case T::DeclOpen:
                if (!matches(t, s + 2, next, "DOCTYPE"_s))
                    break;
                return go(S::Doctype0, R::DoctypeNone);
            case T::InstanceStart:
                return go(S::Error, R::InstanceStart);
            default:
                break;
            }
            break;
        case S::Prolog2:
            switch (tok) {
            case T::PrologS:
                return R::None;
            case T::PI:
                return R::PI;
            case T::Comment:
                return R::Comment;
            case T::InstanceStart:
                return go(S::Error, R::InstanceStart);
            default:
                break;
            }
            break;
        case S::Doctype0:
            if (tok == T::PrologS)
                return R::DoctypeNone;
            if (isAnyName)
                return go(S::Doctype1, R::DoctypeName);
            break;
        case S::Doctype1:
            switch (tok) {
            case T::PrologS:
                return R::DoctypeNone;
            case T::OpenBracket:
                return go(S::InternalSubset, R::DoctypeInternalSubset);
            case T::DeclClose:
                return go(S::Prolog2, R::DoctypeClose);
            case T::Name:
                if (isName("SYSTEM"_s))
                    return go(S::Doctype3, R::DoctypeNone);
                if (isName("PUBLIC"_s))
                    return go(S::Doctype2, R::DoctypeNone);
                break;
            default:
                break;
            }
            break;
        case S::Doctype2:
            if (tok == T::PrologS)
                return R::DoctypeNone;
            if (tok == T::Literal)
                return go(S::Doctype3, R::DoctypePublicId);
            break;
        case S::Doctype3:
            if (tok == T::PrologS)
                return R::DoctypeNone;
            if (tok == T::Literal)
                return go(S::Doctype4, R::DoctypeSystemId);
            break;
        case S::Doctype4:
            if (tok == T::PrologS)
                return R::DoctypeNone;
            if (tok == T::OpenBracket)
                return go(S::InternalSubset, R::DoctypeInternalSubset);
            if (tok == T::DeclClose)
                return go(S::Prolog2, R::DoctypeClose);
            break;
        case S::Doctype5:
            if (tok == T::PrologS)
                return R::DoctypeNone;
            if (tok == T::DeclClose)
                return go(S::Prolog2, R::DoctypeClose);
            break;
        case S::ExternalSubset0:
            if (tok == T::BOM)
                return R::None;
            m_prologState = S::ExternalSubset;
            if (tok == T::XmlDecl)
                return R::XmlDecl;
            [[fallthrough]];
        case S::InternalSubset:
        case S::ExternalSubset:
            switch (tok) {
            case T::PrologS:
            case T::BOM:
                return R::None;
            case T::DeclOpen:
                if (matches(t, s + 2, next, "ENTITY"_s))
                    return go(S::Entity0, R::EntityNone);
                if (matches(t, s + 2, next, "ATTLIST"_s))
                    return go(S::Attlist0, R::AttlistNone);
                if (matches(t, s + 2, next, "ELEMENT"_s))
                    return go(S::Element0, R::ElementNone);
                if (matches(t, s + 2, next, "NOTATION"_s))
                    return go(S::Notation0, R::NotationNone);
                break;
            case T::PI:
                return R::PI;
            case T::Comment:
                return R::Comment;
            case T::ParamEntityRef:
                return R::ParamEntityRef;
            case T::CloseBracket:
                if (m_prologState == S::ExternalSubset)
                    break;
                return go(S::Doctype5, R::DoctypeNone);
            case T::CondSectOpen:
                if (m_prologState != S::ExternalSubset)
                    break;
                return go(S::CondSect0, R::None);
            case T::CondSectClose:
                if (m_prologState != S::ExternalSubset || !m_includeLevel)
                    break;
                --m_includeLevel;
                return R::None;
            case T::None:
                if (m_prologState == S::ExternalSubset && m_includeLevel)
                    break;
                return R::None;
            default:
                break;
            }
            break;
        case S::Entity0:
            if (tok == T::PrologS)
                return R::EntityNone;
            if (tok == T::Percent)
                return go(S::Entity1, R::EntityNone);
            if (tok == T::Name)
                return go(S::Entity2, R::GeneralEntityName);
            break;
        case S::Entity1:
            if (tok == T::PrologS)
                return R::EntityNone;
            if (tok == T::Name)
                return go(S::Entity7, R::ParamEntityName);
            break;
        case S::Entity2:
        case S::Entity7: {
            bool isGeneral = m_prologState == S::Entity2;
            if (tok == T::PrologS)
                return R::EntityNone;
            if (tok == T::Name) {
                if (isName("SYSTEM"_s))
                    return go(isGeneral ? S::Entity4 : S::Entity9, R::EntityNone);
                if (isName("PUBLIC"_s))
                    return go(isGeneral ? S::Entity3 : S::Entity8, R::EntityNone);
                break;
            }
            if (tok == T::Literal)
                return closeDeclaration(R::EntityNone, R::EntityValue);
            break;
        }
        case S::Entity3:
        case S::Entity8:
            if (tok == T::PrologS)
                return R::EntityNone;
            if (tok == T::Literal)
                return go(m_prologState == S::Entity3 ? S::Entity4 : S::Entity9, R::EntityPublicId);
            break;
        case S::Entity4:
        case S::Entity9:
            if (tok == T::PrologS)
                return R::EntityNone;
            if (tok == T::Literal)
                return go(m_prologState == S::Entity4 ? S::Entity5 : S::Entity10, R::EntitySystemId);
            break;
        case S::Entity5:
            if (tok == T::PrologS)
                return R::EntityNone;
            if (tok == T::DeclClose)
                return go(subsetState(), R::EntityComplete);
            if (tok == T::Name && isName("NDATA"_s))
                return go(S::Entity6, R::EntityNone);
            break;
        case S::Entity6:
            if (tok == T::PrologS)
                return R::EntityNone;
            if (tok == T::Name)
                return closeDeclaration(R::EntityNone, R::EntityNotationName);
            break;
        case S::Entity10:
            if (tok == T::PrologS)
                return R::EntityNone;
            if (tok == T::DeclClose)
                return go(subsetState(), R::EntityComplete);
            break;
        case S::Notation0:
            if (tok == T::PrologS)
                return R::NotationNone;
            if (tok == T::Name)
                return go(S::Notation1, R::NotationName);
            break;
        case S::Notation1:
            if (tok == T::PrologS)
                return R::NotationNone;
            if (tok == T::Name) {
                if (isName("SYSTEM"_s))
                    return go(S::Notation3, R::NotationNone);
                if (isName("PUBLIC"_s))
                    return go(S::Notation2, R::NotationNone);
            }
            break;
        case S::Notation2:
            if (tok == T::PrologS)
                return R::NotationNone;
            if (tok == T::Literal)
                return go(S::Notation4, R::NotationPublicId);
            break;
        case S::Notation3:
            if (tok == T::PrologS)
                return R::NotationNone;
            if (tok == T::Literal)
                return closeDeclaration(R::NotationNone, R::NotationSystemId);
            break;
        case S::Notation4:
            if (tok == T::PrologS)
                return R::NotationNone;
            if (tok == T::Literal)
                return closeDeclaration(R::NotationNone, R::NotationSystemId);
            if (tok == T::DeclClose)
                return go(subsetState(), R::NotationNoSystemId);
            break;
        case S::Attlist0:
            if (tok == T::PrologS)
                return R::AttlistNone;
            if (isAnyName)
                return go(S::Attlist1, R::AttlistElementName);
            break;
        case S::Attlist1:
            if (tok == T::PrologS)
                return R::AttlistNone;
            if (tok == T::DeclClose)
                return go(subsetState(), R::AttlistNone);
            if (isAnyName)
                return go(S::Attlist2, R::AttributeName);
            break;
        case S::Attlist2:
            if (tok == T::PrologS)
                return R::AttlistNone;
            if (tok == T::Name) {
                static constexpr ASCIILiteral types[] = { "CDATA"_s, "ID"_s, "IDREF"_s, "IDREFS"_s, "ENTITY"_s, "ENTITIES"_s, "NMTOKEN"_s, "NMTOKENS"_s };
                for (unsigned i = 0; i < std::size(types); ++i) {
                    if (isName(types[i]))
                        return go(S::Attlist8, static_cast<R>(static_cast<unsigned>(R::AttributeTypeCData) + i));
                }
                if (isName("NOTATION"_s))
                    return go(S::Attlist5, R::AttlistNone);
                break;
            }
            if (tok == T::OpenParen)
                return go(S::Attlist3, R::AttlistNone);
            break;
        case S::Attlist3:
            if (tok == T::PrologS)
                return R::AttlistNone;
            if (tok == T::Nmtoken || isAnyName)
                return go(S::Attlist4, R::AttributeEnumValue);
            break;
        case S::Attlist4:
            if (tok == T::PrologS)
                return R::AttlistNone;
            if (tok == T::CloseParen)
                return go(S::Attlist8, R::AttlistNone);
            if (tok == T::Or)
                return go(S::Attlist3, R::AttlistNone);
            break;
        case S::Attlist5:
            if (tok == T::PrologS)
                return R::AttlistNone;
            if (tok == T::OpenParen)
                return go(S::Attlist6, R::AttlistNone);
            break;
        case S::Attlist6:
            if (tok == T::PrologS)
                return R::AttlistNone;
            if (tok == T::Name)
                return go(S::Attlist7, R::AttributeNotationValue);
            break;
        case S::Attlist7:
            if (tok == T::PrologS)
                return R::AttlistNone;
            if (tok == T::CloseParen)
                return go(S::Attlist8, R::AttlistNone);
            if (tok == T::Or)
                return go(S::Attlist6, R::AttlistNone);
            break;
        case S::Attlist8:
            if (tok == T::PrologS)
                return R::AttlistNone;
            if (tok == T::PoundName) {
                if (matches(t, s + 1, next, "IMPLIED"_s))
                    return go(S::Attlist1, R::ImpliedAttributeValue);
                if (matches(t, s + 1, next, "REQUIRED"_s))
                    return go(S::Attlist1, R::RequiredAttributeValue);
                if (matches(t, s + 1, next, "FIXED"_s))
                    return go(S::Attlist9, R::AttlistNone);
                break;
            }
            if (tok == T::Literal)
                return go(S::Attlist1, R::DefaultAttributeValue);
            break;
        case S::Attlist9:
            if (tok == T::PrologS)
                return R::AttlistNone;
            if (tok == T::Literal)
                return go(S::Attlist1, R::FixedAttributeValue);
            break;
        case S::Element0:
            if (tok == T::PrologS)
                return R::ElementNone;
            if (isAnyName)
                return go(S::Element1, R::ElementName);
            break;
        case S::Element1:
            if (tok == T::PrologS)
                return R::ElementNone;
            if (tok == T::Name) {
                if (isName("EMPTY"_s))
                    return closeDeclaration(R::ElementNone, R::ContentEmpty);
                if (isName("ANY"_s))
                    return closeDeclaration(R::ElementNone, R::ContentAny);
                break;
            }
            if (tok == T::OpenParen) {
                m_groupLevel = 1;
                return go(S::Element2, R::GroupOpen);
            }
            break;
        case S::Element2:
            if (tok == T::PoundName) {
                if (matches(t, s + 1, next, "PCDATA"_s))
                    return go(S::Element3, R::ContentPCData);
                break;
            }
            [[fallthrough]];
        case S::Element6:
            switch (tok) {
            case T::PrologS:
                return R::ElementNone;
            case T::OpenParen:
                if (m_prologState == S::Element2)
                    m_groupLevel = 2;
                else
                    ++m_groupLevel;
                return go(S::Element6, R::GroupOpen);
            case T::Name:
            case T::PrefixedName:
                return go(S::Element7, R::ContentElement);
            case T::NameQuestion:
                return go(S::Element7, R::ContentElementOpt);
            case T::NameAsterisk:
                return go(S::Element7, R::ContentElementRep);
            case T::NamePlus:
                return go(S::Element7, R::ContentElementPlus);
            default:
                break;
            }
            break;
        case S::Element3:
            if (tok == T::PrologS)
                return R::ElementNone;
            if (tok == T::CloseParen)
                return closeDeclaration(R::ElementNone, R::GroupClose);
            if (tok == T::CloseParenAsterisk)
                return closeDeclaration(R::ElementNone, R::GroupCloseRep);
            if (tok == T::Or)
                return go(S::Element4, R::ElementNone);
            break;
        case S::Element4:
            if (tok == T::PrologS)
                return R::ElementNone;
            if (isAnyName)
                return go(S::Element5, R::ContentElement);
            break;
        case S::Element5:
            if (tok == T::PrologS)
                return R::ElementNone;
            if (tok == T::CloseParenAsterisk)
                return closeDeclaration(R::ElementNone, R::GroupCloseRep);
            if (tok == T::Or)
                return go(S::Element4, R::ElementNone);
            break;
        case S::Element7: {
            R role;
            switch (tok) {
            case T::PrologS:
                return R::ElementNone;
            case T::CloseParen:
                role = R::GroupClose;
                break;
            case T::CloseParenAsterisk:
                role = R::GroupCloseRep;
                break;
            case T::CloseParenQuestion:
                role = R::GroupCloseOpt;
                break;
            case T::CloseParenPlus:
                role = R::GroupClosePlus;
                break;
            case T::Comma:
                return go(S::Element6, R::GroupSequence);
            case T::Or:
                return go(S::Element6, R::GroupChoice);
            default:
                return prologError(tok);
            }
            if (!--m_groupLevel)
                return closeDeclaration(R::ElementNone, role);
            return role;
        }
        case S::DeclClose:
            if (tok == T::PrologS)
                return m_roleNone;
            if (tok == T::DeclClose)
                return go(subsetState(), m_roleNone);
            break;
        case S::CondSect0:
            if (tok == T::PrologS)
                return R::None;
            if (tok == T::Name && isName("INCLUDE"_s))
                return go(S::CondSect1, R::None);
            if (tok == T::Name && isName("IGNORE"_s))
                return go(S::CondSect2, R::None);
            break;
        case S::CondSect1:
            if (tok == T::PrologS)
                return R::None;
            if (tok == T::OpenBracket) {
                ++m_includeLevel;
                return go(S::ExternalSubset, R::None);
            }
            break;
        case S::CondSect2:
            if (tok == T::PrologS)
                return R::None;
            if (tok == T::OpenBracket)
                return go(S::ExternalSubset, R::IgnoreSect);
            break;
        case S::Error:
            return R::None;
        }
        return prologError(tok);
    }

    static bool isPublicId(Text t, size_t start, size_t end, size_t& bad)
    {
        for (size_t p = start; p < end; ++p) {
            char16_t c = t[p];
            if (isASCIIAlphanumeric(c) || c == ' ' || c == '\r' || c == '\n' || (c < 0x80 && c && WTF::contains("-'()+,./:=?;!*#@$_%"_span, static_cast<char>(c))))
                continue;
            bad = p;
            return false;
        }
        return true;
    }

    // With each run of white space as one space, and none at the ends.
    static String normalizePublicId(Text t)
    {
        Vector<char16_t> out;
        for (char16_t c : t) {
            if (c == ' ' || c == '\r' || c == '\n') {
                if (!out.isEmpty() && out.last() != ' ')
                    out.append(' ');
            } else
                out.append(c);
        }
        if (!out.isEmpty() && out.last() == ' ')
            out.removeLast();
        return stringOfText(out.span());
    }

    // (type, quantifier, name, children). Empty: an exception.
    JSValue contentModelToTuple(int index)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!vm.isSafeToRecurseSoft())
            RELEASE_AND_RETURN(scope, raiseRecursionError(m_globalObject, scope, "maximum recursion depth exceeded in conv_content_model"_s));
        const ExpatContentNode& node = m_contentNodes[index];
        MarkedArgumentBuffer children;
        for (int child = node.firstChild; child >= 0; child = m_contentNodes[child].nextSibling) {
            JSValue tuple = contentModelToTuple(child);
            RETURN_IF_EXCEPTION(scope, { });
            children.append(tuple);
        }
        RELEASE_AND_RETURN(scope, PyTuple::create(m_globalObject, { jsNumber(node.type), jsNumber(node.quantifier), toStr(node.name), PyTuple::create(m_globalObject, spanOf(children)) }));
    }

    // A new node, in the group that is open if any is.
    int addContentNode(int type, int quantifier, const String& name)
    {
        int index = m_contentNodes.size();
        m_contentNodes.append({ type, quantifier, name });
        if (!m_openGroups.isEmpty()) {
            ExpatContentNode& parent = m_contentNodes[m_openGroups.last()];
            if (parent.lastChild >= 0)
                m_contentNodes[parent.lastChild].nextSibling = index;
            else
                parent.firstChild = index;
            parent.lastChild = index;
            ++parent.childCount;
        }
        return index;
    }

    void reportElementDeclaration()
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (m_isStopped || !hasHandler(ExpatHandlerElementDecl) || !flushCharacterBuffer(m_globalObject))
            return;
        JSValue model = contentModelToTuple(0);
        if (scope.exception()) {
            stopAfterException();
            return;
        }
        scope.release();
        callHandler(ExpatHandlerElementDecl, { intern(m_declaredElement), model });
    }

    void defineAttribute(const String& elementName, const String& name, bool isCData, const String& value)
    {
        auto& elementType = m_dtd->elementTypes.ensure(elementName, [] { return ExpatElementType(); }).iterator->value;
        // The first declaration is the one that counts.
        for (auto& declaration : elementType.attributes) {
            if (declaration.name == name)
                return;
        }
        elementType.attributes.append({ name, isCData, value });
    }

    ExpatContentNode& currentGroup() { return m_contentNodes[m_openGroups.last()]; }

    ExpatError callNotStandalone()
    {
        if (!m_dtd->isStandalone && hasHandler(ExpatHandlerNotStandalone) && !callIntHandler(ExpatHandlerNotStandalone, "NotStandalone"_s, { }))
            return ExpatErrorNotStandalone;
        return ExpatErrorNone;
    }

    ExpatError doProlog(const Source& source, size_t s, bool haveMore, size_t& resume)
    {
        using R = ExpatRole;
        Text t = source.text;
        ExpatDTD& dtd = m_dtd.get();
        enum { CTypeEmpty = 1, CTypeAny, CTypeMixed, CTypeName, CTypeChoice, CTypeSeq };
        enum { CQuantNone, CQuantOpt, CQuantRep, CQuantPlus };

        while (true) {
            size_t next = s;
            bool isProvisional;
            ExpatTok tok = unlessHalfUnit(source, prologTok(t, s, next, isProvisional));
            if (source.isDocument && !m_hasSeenStart && s < t.size() && t[s] == 0xfeff) {
                tok = ExpatTok::BOM;
                next = s + 1;
                isProvisional = false;
            }
            setEvent(source, s);
            if (tok <= ExpatTok::TrailingRSQB || isProvisional) {
                if (haveMore && tok != ExpatTok::Invalid) {
                    resume = s;
                    return ExpatErrorNone;
                }
                switch (tok) {
                case ExpatTok::Invalid:
                    setEvent(source, next);
                    return ExpatErrorInvalidToken;
                case ExpatTok::Partial:
                    return ExpatErrorUnclosedToken;
                case ExpatTok::PartialChar:
                    return ExpatErrorPartialChar;
                case ExpatTok::None:
                    if (!source.isDocument && !m_isBetweenDeclarations) {
                        resume = s;
                        return ExpatErrorNone;
                    }
                    // An entity referred to between declarations has to hold whole ones.
                    if (m_isParameterEntity || !source.isDocument) {
                        if (tokenRole(ExpatTok::None, t, s, s) == R::Error)
                            return ExpatErrorIncompletePe;
                        resume = s;
                        return ExpatErrorNone;
                    }
                    return ExpatErrorNoElements;
                default:
                    break;
                }
            }

            m_hasSeenStart |= source.isDocument;
            R role = tokenRole(tok, t, s, next);
            bool handleDefault = true;
            auto literal = [&] { return t.subspan(s + 1, next - s - 2); };

            switch (role) {
            case R::XmlDecl:
                if (ExpatError error = processXmlDeclaration(source, s, next, m_isParameterEntity))
                    return error;
                handleDefault = false;
                if (source.isDocument && m_rawDecoded < m_raw.size()) {
                    // What follows can be decoded now that the encoding is known.
                    if (m_isStopped)
                        return ExpatErrorAborted;
                    resume = next;
                    m_needsDecoding = true;
                    return ExpatErrorNone;
                }
                break;
            case R::DoctypeName:
                if (hasHandler(ExpatHandlerStartDoctypeDecl)) {
                    m_doctypeName = stringOfText(t, s, next);
                    m_doctypePublicId = String();
                    handleDefault = false;
                }
                m_doctypeSystemId = String();
                break;
            case R::DoctypeInternalSubset:
                if (hasHandler(ExpatHandlerStartDoctypeDecl)) {
                    callHandler(ExpatHandlerStartDoctypeDecl, { intern(m_doctypeName), intern(m_doctypeSystemId), intern(m_doctypePublicId), jsNumber(1) });
                    m_doctypeName = String();
                    handleDefault = false;
                }
                break;
            case R::DoctypePublicId:
            case R::EntityPublicId: {
                if (role == R::DoctypePublicId) {
                    m_usesForeignDTD = false;
                    m_externalSubset = makeUnique<ExpatEntity>();
                    m_declaredEntity = m_externalSubset.get();
                    dtd.hasParamEntityRefs = true;
                }
                size_t bad;
                if (!isPublicId(t, s + 1, next - 1, bad)) {
                    setEvent(source, bad);
                    return ExpatErrorPublicId;
                }
                if (role == R::DoctypePublicId && hasHandler(ExpatHandlerStartDoctypeDecl)) {
                    m_doctypePublicId = normalizePublicId(literal());
                    handleDefault = false;
                }
                if (dtd.keepProcessing && m_declaredEntity) {
                    m_declaredEntity->publicId = normalizePublicId(literal());
                    if (hasHandler(ExpatHandlerEntityDecl) && role == R::EntityPublicId)
                        handleDefault = false;
                }
                break;
            }
            case R::DoctypeClose:
                if (!m_doctypeName.isNull()) {
                    callHandler(ExpatHandlerStartDoctypeDecl, { intern(m_doctypeName), intern(m_doctypeSystemId), intern(m_doctypePublicId), jsNumber(0) });
                    m_doctypeName = String();
                    handleDefault = false;
                }
                // The external subset comes after the internal one, if it is wanted at all.
                if (!m_doctypeSystemIdForSubset.isNull() || m_usesForeignDTD) {
                    bool hadParamEntityRefs = dtd.hasParamEntityRefs;
                    dtd.hasParamEntityRefs = true;
                    if (m_paramEntityParsing && hasHandler(ExpatHandlerExternalEntityRef)) {
                        if (!m_externalSubset)
                            m_externalSubset = makeUnique<ExpatEntity>();
                        if (m_usesForeignDTD)
                            m_externalSubset->base = m_base;
                        dtd.paramEntityRead = false;
                        if (!callIntHandler(ExpatHandlerExternalEntityRef, "ExternalEntityRef"_s, { jsUndefined(), intern(m_externalSubset->base), intern(m_externalSubset->systemId), intern(m_externalSubset->publicId) }))
                            return ExpatErrorExternalEntityHandling;
                        if (dtd.paramEntityRead) {
                            if (ExpatError error = callNotStandalone())
                                return error;
                        } else if (m_doctypeSystemIdForSubset.isNull())
                            dtd.hasParamEntityRefs = hadParamEntityRefs;
                    }
                    m_usesForeignDTD = false;
                }
                if (hasHandler(ExpatHandlerEndDoctypeDecl)) {
                    callHandler(ExpatHandlerEndDoctypeDecl, { });
                    handleDefault = false;
                }
                break;
            case R::InstanceStart:
                if (m_usesForeignDTD) {
                    bool hadParamEntityRefs = dtd.hasParamEntityRefs;
                    dtd.hasParamEntityRefs = true;
                    if (m_paramEntityParsing && hasHandler(ExpatHandlerExternalEntityRef)) {
                        dtd.paramEntityRead = false;
                        if (!callIntHandler(ExpatHandlerExternalEntityRef, "ExternalEntityRef"_s, { jsUndefined(), intern(m_base), jsUndefined(), jsUndefined() }))
                            return ExpatErrorExternalEntityHandling;
                        if (dtd.paramEntityRead) {
                            if (ExpatError error = callNotStandalone())
                                return error;
                        } else
                            dtd.hasParamEntityRefs = hadParamEntityRefs;
                    }
                }
                m_processor = Processor::Content;
                return doContent(source, s, haveMore, 0, resume);
            case R::AttlistElementName:
                m_declaredElement = stringOfText(t, s, next);
                goto checkAttlistHandler;
            case R::AttributeName:
                m_declaredAttribute = stringOfText(t, s, next);
                m_declaredAttributeIsCData = false;
                m_declaredAttributeType = String();
                goto checkAttlistHandler;
            case R::AttributeTypeCData:
                m_declaredAttributeIsCData = true;
                m_declaredAttributeType = "CDATA"_s;
                goto checkAttlistHandler;
            case R::AttributeTypeId:
                m_declaredAttributeType = "ID"_s;
                goto checkAttlistHandler;
            case R::AttributeTypeIdRef:
                m_declaredAttributeType = "IDREF"_s;
                goto checkAttlistHandler;
            case R::AttributeTypeIdRefs:
                m_declaredAttributeType = "IDREFS"_s;
                goto checkAttlistHandler;
            case R::AttributeTypeEntity:
                m_declaredAttributeType = "ENTITY"_s;
                goto checkAttlistHandler;
            case R::AttributeTypeEntities:
                m_declaredAttributeType = "ENTITIES"_s;
                goto checkAttlistHandler;
            case R::AttributeTypeNmtoken:
                m_declaredAttributeType = "NMTOKEN"_s;
                goto checkAttlistHandler;
            case R::AttributeTypeNmtokens:
                m_declaredAttributeType = "NMTOKENS"_s;
            checkAttlistHandler:
                if (dtd.keepProcessing && hasHandler(ExpatHandlerAttlistDecl))
                    handleDefault = false;
                break;
            case R::AttributeEnumValue:
            case R::AttributeNotationValue:
                if (dtd.keepProcessing && hasHandler(ExpatHandlerAttlistDecl)) {
                    ASCIILiteral prefix = !m_declaredAttributeType.isNull() ? "|"_s : role == R::AttributeNotationValue ? "NOTATION("_s : "("_s;
                    m_declaredAttributeType = makeString(m_declaredAttributeType.isNull() ? emptyString() : m_declaredAttributeType, prefix, StringView(t.subspan(s, next - s)));
                    handleDefault = false;
                }
                break;
            case R::ImpliedAttributeValue:
            case R::RequiredAttributeValue:
            case R::DefaultAttributeValue:
            case R::FixedAttributeValue:
                if (dtd.keepProcessing) {
                    String value;
                    if (role == R::DefaultAttributeValue || role == R::FixedAttributeValue) {
                        if (ExpatError error = storeAttributeValue(source, s + 1, next - 1, m_declaredAttributeIsCData, true, value))
                            return error;
                    }
                    defineAttribute(m_declaredElement, m_declaredAttribute, m_declaredAttributeIsCData, value);
                    if (hasHandler(ExpatHandlerAttlistDecl) && !m_declaredAttributeType.isNull()) {
                        if (m_declaredAttributeType[0] == '(' || m_declaredAttributeType.startsWith("NO"_s))
                            m_declaredAttributeType = makeString(m_declaredAttributeType, ')');
                        callHandler(ExpatHandlerAttlistDecl, { intern(m_declaredElement), intern(m_declaredAttribute), toStr(m_declaredAttributeType), toStr(value), jsNumber(role == R::RequiredAttributeValue || role == R::FixedAttributeValue) });
                        handleDefault = false;
                    }
                }
                break;
            case R::EntityValue:
                if (dtd.keepProcessing) {
                    Vector<char16_t> value;
                    ExpatError error = storeEntityValue(source, s + 1, next - 1, value);
                    if (m_declaredEntity) {
                        m_declaredEntity->text = WTF::move(value);
                        m_declaredEntity->hasText = true;
                        if (hasHandler(ExpatHandlerEntityDecl)) {
                            callHandler(ExpatHandlerEntityDecl, { intern(m_declaredEntity->name), jsNumber(m_declaredEntity->isParameter), toStr(m_declaredEntity->text.span()), intern(m_base), jsUndefined(), jsUndefined(), jsUndefined() });
                            handleDefault = false;
                        }
                    }
                    if (error)
                        return error;
                }
                break;
            case R::DoctypeSystemId:
                m_usesForeignDTD = false;
                dtd.hasParamEntityRefs = true;
                m_doctypeSystemIdForSubset = stringOfText(literal());
                if (hasHandler(ExpatHandlerStartDoctypeDecl)) {
                    m_doctypeSystemId = m_doctypeSystemIdForSubset;
                    handleDefault = false;
                }
                if (!dtd.isStandalone && !m_paramEntityParsing) {
                    if (ExpatError error = callNotStandalone())
                        return error;
                }
                if (!m_externalSubset || m_declaredEntity != m_externalSubset.get()) {
                    m_externalSubset = makeUnique<ExpatEntity>();
                    m_declaredEntity = m_externalSubset.get();
                }
                [[fallthrough]];
            case R::EntitySystemId:
                if (dtd.keepProcessing && m_declaredEntity) {
                    m_declaredEntity->systemId = stringOfText(literal());
                    m_declaredEntity->base = m_base;
                    if (hasHandler(ExpatHandlerEntityDecl) && role == R::EntitySystemId)
                        handleDefault = false;
                }
                break;
            case R::EntityComplete:
                if (dtd.keepProcessing && m_declaredEntity && hasHandler(ExpatHandlerEntityDecl)) {
                    callHandler(ExpatHandlerEntityDecl, { intern(m_declaredEntity->name), jsNumber(m_declaredEntity->isParameter), jsUndefined(), intern(m_declaredEntity->base), intern(m_declaredEntity->systemId), intern(m_declaredEntity->publicId), jsUndefined() });
                    handleDefault = false;
                }
                break;
            case R::EntityNotationName:
                if (dtd.keepProcessing && m_declaredEntity) {
                    m_declaredEntity->notation = stringOfText(t, s, next);
                    if (hasHandler(ExpatHandlerUnparsedEntityDecl)) {
                        callHandler(ExpatHandlerUnparsedEntityDecl, { intern(m_declaredEntity->name), intern(m_declaredEntity->base), intern(m_declaredEntity->systemId), intern(m_declaredEntity->publicId), intern(m_declaredEntity->notation) });
                        handleDefault = false;
                    } else if (hasHandler(ExpatHandlerEntityDecl)) {
                        callHandler(ExpatHandlerEntityDecl, { intern(m_declaredEntity->name), jsNumber(0), jsUndefined(), intern(m_declaredEntity->base), intern(m_declaredEntity->systemId), intern(m_declaredEntity->publicId), intern(m_declaredEntity->notation) });
                        handleDefault = false;
                    }
                }
                break;
            case R::GeneralEntityName:
            case R::ParamEntityName: {
                m_declaredEntity = nullptr;
                bool isParameter = role == R::ParamEntityName;
                if (!isParameter && predefinedEntity(t, s, next))
                    break;
                if (!dtd.keepProcessing)
                    break;
                String name = stringOfText(t, s, next);
                auto& entities = isParameter ? dtd.parameterEntities : dtd.generalEntities;
                // The first declaration is the one that counts.
                auto result = entities.ensure(name, [] { return makeUnique<ExpatEntity>(); });
                if (!result.isNewEntry)
                    break;
                if (ExpatError error = accountForStorage(sizeof(ExpatEntity) + name.length()))
                    return error;
                m_declaredEntity = result.iterator->value.get();
                m_declaredEntity->name = name;
                m_declaredEntity->isParameter = isParameter;
                m_declaredEntity->isInternal = !(m_isChild || m_openInternalEntities);
                if (hasHandler(ExpatHandlerEntityDecl))
                    handleDefault = false;
                break;
            }
            case R::NotationName:
                m_notationPublicId = String();
                m_notationName = String();
                if (hasHandler(ExpatHandlerNotationDecl)) {
                    m_notationName = stringOfText(t, s, next);
                    handleDefault = false;
                }
                break;
            case R::NotationPublicId: {
                size_t bad;
                if (!isPublicId(t, s + 1, next - 1, bad)) {
                    setEvent(source, bad);
                    return ExpatErrorPublicId;
                }
                if (!m_notationName.isNull()) {
                    m_notationPublicId = normalizePublicId(literal());
                    handleDefault = false;
                }
                break;
            }
            case R::NotationSystemId:
                if (!m_notationName.isNull() && hasHandler(ExpatHandlerNotationDecl)) {
                    callHandler(ExpatHandlerNotationDecl, { intern(m_notationName), intern(m_base), intern(stringOfText(literal())), intern(m_notationPublicId) });
                    handleDefault = false;
                }
                break;
            case R::NotationNoSystemId:
                if (!m_notationPublicId.isNull() && hasHandler(ExpatHandlerNotationDecl)) {
                    callHandler(ExpatHandlerNotationDecl, { intern(m_notationName), intern(m_base), jsUndefined(), intern(m_notationPublicId) });
                    handleDefault = false;
                }
                break;
            case R::Error:
                if (tok == ExpatTok::ParamEntityRef)
                    return ExpatErrorParamEntityRef;
                if (tok == ExpatTok::XmlDecl)
                    return ExpatErrorMisplacedXmlPi;
                return ExpatErrorSyntax;
            case R::GroupOpen:
                if (m_groupConnectors.size() <= m_groupLevel)
                    m_groupConnectors.grow(m_groupLevel + 1);
                m_groupConnectors[m_groupLevel] = 0;
                if (m_isInElementDeclaration) {
                    m_openGroups.append(addContentNode(CTypeSeq, CQuantNone, String()));
                    if (hasHandler(ExpatHandlerElementDecl))
                        handleDefault = false;
                }
                break;
            case R::GroupSequence:
                if (m_groupConnectors[m_groupLevel] == '|')
                    return ExpatErrorSyntax;
                m_groupConnectors[m_groupLevel] = ',';
                if (m_isInElementDeclaration && hasHandler(ExpatHandlerElementDecl))
                    handleDefault = false;
                break;
            case R::GroupChoice:
                if (m_groupConnectors[m_groupLevel] == ',')
                    return ExpatErrorSyntax;
                if (m_isInElementDeclaration && !m_groupConnectors[m_groupLevel] && currentGroup().type != CTypeMixed) {
                    currentGroup().type = CTypeChoice;
                    if (hasHandler(ExpatHandlerElementDecl))
                        handleDefault = false;
                }
                m_groupConnectors[m_groupLevel] = '|';
                break;
            case R::IgnoreSect: {
                reportDefault(source, s, next);
                handleDefault = false;
                bool isClosed = false;
                if (ExpatError error = doIgnoreSection(source, next, haveMore, next, isClosed))
                    return error;
                if (!isClosed) {
                    m_processor = Processor::IgnoreSection;
                    resume = next;
                    return ExpatErrorNone;
                }
                break;
            }
            case R::ParamEntityRef:
            case R::InnerParamEntityRef: {
                dtd.hasParamEntityRefs = true;
                if (!m_paramEntityParsing)
                    dtd.keepProcessing = dtd.isStandalone;
                else {
                    String name = stringOfText(t, s + 1, next - 1);
                    auto it = dtd.parameterEntities.find(name);
                    ExpatEntity* entity = it == dtd.parameterEntities.end() ? nullptr : it->value.get();
                    if (!m_isParameterEntity && dtd.isStandalone && !m_openInternalEntities) {
                        if (!entity)
                            return ExpatErrorUndefinedEntity;
                        if (!entity->isInternal)
                            return ExpatErrorEntityDeclaredInPe;
                    } else if (!entity) {
                        dtd.keepProcessing = dtd.isStandalone;
                        if (role == R::ParamEntityRef && hasHandler(ExpatHandlerSkippedEntity)) {
                            callHandler(ExpatHandlerSkippedEntity, { intern(name), jsNumber(1) });
                            handleDefault = false;
                        }
                        break;
                    }
                    if (entity->isOpen)
                        return ExpatErrorRecursiveEntityRef;
                    if (entity->hasText) {
                        if (ExpatError error = processInternalEntity(entity, role == R::ParamEntityRef))
                            return error;
                        handleDefault = false;
                        break;
                    }
                    if (hasHandler(ExpatHandlerExternalEntityRef)) {
                        dtd.paramEntityRead = false;
                        entity->isOpen = true;
                        int64_t result = callIntHandler(ExpatHandlerExternalEntityRef, "ExternalEntityRef"_s, { jsUndefined(), intern(entity->base), intern(entity->systemId), intern(entity->publicId) });
                        entity->isOpen = false;
                        if (!result)
                            return ExpatErrorExternalEntityHandling;
                        handleDefault = false;
                        if (!dtd.paramEntityRead) {
                            dtd.keepProcessing = dtd.isStandalone;
                            break;
                        }
                    } else {
                        dtd.keepProcessing = dtd.isStandalone;
                        break;
                    }
                }
                if (ExpatError error = callNotStandalone())
                    return error;
                break;
            }
            case R::ElementName:
                if (hasHandler(ExpatHandlerElementDecl)) {
                    m_declaredElement = stringOfText(t, s, next);
                    m_contentNodes.clear();
                    m_openGroups.clear();
                    m_isInElementDeclaration = true;
                    handleDefault = false;
                }
                break;
            case R::ContentAny:
            case R::ContentEmpty:
                if (m_isInElementDeclaration) {
                    if (hasHandler(ExpatHandlerElementDecl)) {
                        addContentNode(role == R::ContentAny ? CTypeAny : CTypeEmpty, CQuantNone, String());
                        reportElementDeclaration();
                        handleDefault = false;
                    }
                    m_isInElementDeclaration = false;
                }
                break;
            case R::ContentPCData:
                if (m_isInElementDeclaration) {
                    currentGroup().type = CTypeMixed;
                    if (hasHandler(ExpatHandlerElementDecl))
                        handleDefault = false;
                }
                break;
            case R::ContentElement:
            case R::ContentElementOpt:
            case R::ContentElementRep:
            case R::ContentElementPlus:
                if (m_isInElementDeclaration) {
                    int quantifier = role == R::ContentElement ? CQuantNone : role == R::ContentElementOpt ? CQuantOpt : role == R::ContentElementRep ? CQuantRep : CQuantPlus;
                    size_t nameEnd = quantifier == CQuantNone ? next : next - 1;
                    addContentNode(CTypeName, quantifier, stringOfText(t, s, nameEnd));
                    if (hasHandler(ExpatHandlerElementDecl))
                        handleDefault = false;
                }
                break;
            case R::GroupClose:
            case R::GroupCloseOpt:
            case R::GroupCloseRep:
            case R::GroupClosePlus:
                if (m_isInElementDeclaration) {
                    if (hasHandler(ExpatHandlerElementDecl))
                        handleDefault = false;
                    currentGroup().quantifier = role == R::GroupClose ? CQuantNone : role == R::GroupCloseOpt ? CQuantOpt : role == R::GroupCloseRep ? CQuantRep : CQuantPlus;
                    m_openGroups.removeLast();
                    if (m_openGroups.isEmpty()) {
                        if (!handleDefault)
                            reportElementDeclaration();
                        m_isInElementDeclaration = false;
                    }
                }
                break;
            case R::PI:
                reportProcessingInstruction(source, s, next);
                handleDefault = false;
                break;
            case R::Comment:
                reportComment(source, s, next);
                handleDefault = false;
                break;
            case R::None:
                if (tok == ExpatTok::BOM)
                    handleDefault = false;
                break;
            case R::DoctypeNone:
                if (hasHandler(ExpatHandlerStartDoctypeDecl))
                    handleDefault = false;
                break;
            case R::EntityNone:
                if (dtd.keepProcessing && hasHandler(ExpatHandlerEntityDecl))
                    handleDefault = false;
                break;
            case R::NotationNone:
                if (hasHandler(ExpatHandlerNotationDecl))
                    handleDefault = false;
                break;
            case R::AttlistNone:
                if (dtd.keepProcessing && hasHandler(ExpatHandlerAttlistDecl))
                    handleDefault = false;
                break;
            case R::ElementNone:
                if (hasHandler(ExpatHandlerElementDecl))
                    handleDefault = false;
                break;
            }

            if (handleDefault)
                reportDefault(source, s, next);
            if (m_isStopped)
                return ExpatErrorAborted;
            s = next;
        }
    }

    // ---- External entities ----

    // At the start of an external general entity: a byte order mark and a text declaration may come first.
    ExpatError doExternalEntityStart(const Source& source, size_t s, bool haveMore, size_t& resume)
    {
        Text t = source.text;
        auto wait = [&] {
            resume = s;
            return ExpatErrorNone;
        };
        // What may be the start of a text declaration is not decoded before its end has come.
        if (s == t.size() && haveMore && m_rawDecoded < m_raw.size())
            return wait();
        if (s < t.size() && !m_hasSeenStart) {
            m_hasSeenStart = true;
            if (t[s] == 0xfeff && ++s == t.size() && haveMore)
                return wait();
        }
        size_t next = s;
        ExpatTok tok = contentTok(t, s, next);
        setEvent(source, s);
        switch (tok) {
        case ExpatTok::XmlDecl:
            if (ExpatError error = processXmlDeclaration(source, s, next, true))
                return error;
            if (m_isStopped)
                return ExpatErrorAborted;
            m_processor = Processor::ExternalEntityContent;
            if (m_rawDecoded < m_raw.size()) {
                resume = next;
                m_needsDecoding = true;
                return ExpatErrorNone;
            }
            s = next;
            break;
        case ExpatTok::Partial:
            if (haveMore) {
                resume = s;
                return ExpatErrorNone;
            }
            return ExpatErrorUnclosedToken;
        case ExpatTok::PartialChar:
            if (haveMore) {
                resume = s;
                return ExpatErrorNone;
            }
            return ExpatErrorPartialChar;
        default:
            m_mayHaveEncodingDeclaration = false;
            if (m_rawDecoded < m_raw.size()) {
                m_processor = Processor::ExternalEntityContent;
                resume = s;
                m_needsDecoding = true;
                return ExpatErrorNone;
            }
            break;
        }
        m_processor = Processor::ExternalEntityContent;
        return doContent(source, s, haveMore, 0, resume);
    }

    // ---- Driving it ----

    ExpatError run()
    {
        // A token that was incomplete is not tried again until there is a good deal more input: going over a long one each
        // time a little is added would take time quadratic in its length.
        size_t pending = m_raw.size() - std::min<size_t>(m_byte - m_rawBase, m_raw.size());
        if (m_isReparseDeferralEnabled && !m_isFinal && pending < 2 * m_partialTokenBytesBefore)
            return ExpatErrorNone;

        if (m_encoding == Encoding::Undetermined) {
            ExpatError error;
            if (!determineEncoding(error)) {
                m_partialTokenBytesBefore = pending;
                return ExpatErrorNone;
            }
            if (error || m_isStopped)
                return error ? error : ExpatErrorAborted;
        }

        size_t start = m_resume;
        while (true) {
            decode();
            m_needsDecoding = false;
            Source source { m_text.span(), true };
            bool haveMore = !m_isFinal;
            ExpatError error = ExpatErrorNone;
            switch (m_processor) {
            case Processor::PrologInit:
                m_processor = Processor::Prolog;
                [[fallthrough]];
            case Processor::Prolog:
                error = doProlog(source, m_resume, haveMore, m_resume);
                break;
            case Processor::ExternalParameterEntityInit:
                m_dtd->paramEntityRead = true;
                m_processor = Processor::Prolog;
                error = doProlog(source, m_resume, haveMore, m_resume);
                break;
            case Processor::IgnoreSection: {
                bool isClosed = false;
                error = doIgnoreSection(source, m_resume, haveMore, m_resume, isClosed);
                if (!error && isClosed) {
                    m_processor = Processor::Prolog;
                    continue;
                }
                break;
            }
            case Processor::Content:
                error = doContent(source, m_resume, haveMore, 0, m_resume);
                break;
            case Processor::ExternalEntityInit:
                error = doExternalEntityStart(source, m_resume, haveMore, m_resume);
                break;
            case Processor::ExternalEntityContent:
                error = doContent(source, m_resume, haveMore, 0, m_resume);
                break;
            case Processor::CdataSection: {
                bool isClosed = false;
                error = doCdataSection(source, m_resume, haveMore, m_resume, isClosed);
                if (!error && isClosed) {
                    m_processor = m_isChild ? Processor::ExternalEntityContent : Processor::Content;
                    continue;
                }
                break;
            }
            case Processor::Epilog:
                error = doEpilog(source, m_resume, haveMore, m_resume);
                break;
            case Processor::Error:
                return static_cast<ExpatError>(m_errorCode);
            }
            if (error)
                return error;
            if (!m_needsDecoding)
                break;
        }
        m_partialTokenBytesBefore = m_resume == start ? pending : 0;
        if (m_isFinal) {
            for (auto& name : m_openedByContext) {
                if (ExpatEntity* entity = generalEntity(name))
                    entity->isOpen = false;
            }
        }
        return ExpatErrorNone;
    }

    PyRecord* m_owner;
    JSGlobalObject* m_globalObject { nullptr }; // While a method is running.
    Ref<ExpatDTD> m_dtd;

    String m_protocolEncoding; // What the caller said the encoding is. Null: nothing.
    bool m_hasNamespaces;
    char16_t m_separator;
    bool m_returnsTriplets { false };
    bool m_isChild { false }; // It parses an external entity,
    bool m_isParameterEntity { false }; // a parameter entity or the external subset at that.
    String m_base;
    int m_paramEntityParsing { 0 };
    bool m_usesForeignDTD { false };
    int m_defaultHandler { -1 }; // Which of the two, or neither.
    bool m_expandsInternalEntities { true };
    bool m_isReparseDeferralEnabled { true };

    Processor m_processor { Processor::PrologInit };
    bool m_hasStarted { false };
    bool m_isFinal { false };
    bool m_isFinished { false };
    bool m_isStopped { false };
    int m_errorCode { ExpatErrorNone };

    // The input: as it came, from the byte numbered m_rawBase on, and decoded.
    Vector<uint8_t> m_raw;
    uint64_t m_rawBase { 0 };
    size_t m_rawDecoded { 0 };
    Vector<char16_t> m_text;
    size_t m_resume { 0 }; // Where in m_text parsing goes on.
    Encoding m_encoding { Encoding::Undetermined };
    std::array<char16_t, 256> m_table { };
    bool m_mayHaveEncodingDeclaration { true };
    bool m_needsDecoding { false };
    bool m_hasUndecodableInput { false };
    bool m_hasSeenStart { false };
    size_t m_partialCharacterIndex { notFound };
    size_t m_partialTokenBytesBefore { 0 };
    size_t m_deepestLevel { 0 };
    bool m_hasHalfUnit { false };
    bool m_isInReplacementText { false };

    // The position of m_text[m_positionIndex],
    size_t m_positionIndex { 0 };
    uint64_t m_line { 0 };
    uint64_t m_column { 0 };
    uint64_t m_byte { 0 };
    // of what is being reported,
    bool m_hasEvent { false };
    size_t m_eventIndex { 0 };
    uint64_t m_eventLine { 0 };
    uint64_t m_eventColumn { 0 };
    uint64_t m_eventByte { 0 };
    // and what CurrentLineNumber and the like say.
    uint64_t m_reportedLine { 0 };
    uint64_t m_reportedColumn { 0 };

    Vector<char16_t> m_characterBuffer;
    int64_t m_characterBufferUsed { 0 };

    Vector<Tag> m_tags;
    HashMap<String, Vector<String>> m_bindings; // For each prefix, the namespaces it has been bound to, the innermost last.
    unsigned m_openInternalEntities { 0 };
    Vector<String> m_openedByContext;

    ExpatPrologState m_prologState { ExpatPrologState::Prolog0 };
    ExpatRole m_roleNone { ExpatRole::None };
    unsigned m_includeLevel { 0 }; // How many sections to be included are open.
    unsigned m_groupLevel { 0 };
    Vector<char> m_groupConnectors;
    bool m_isBetweenDeclarations { false };
    String m_doctypeName;
    String m_doctypeSystemId;
    String m_doctypePublicId;
    String m_doctypeSystemIdForSubset;
    std::unique_ptr<ExpatEntity> m_externalSubset;
    ExpatEntity* m_declaredEntity { nullptr };
    String m_declaredElement;
    String m_declaredAttribute;
    String m_declaredAttributeType;
    bool m_declaredAttributeIsCData { false };
    String m_notationName;
    String m_notationPublicId;
    bool m_isInElementDeclaration { false };
    Vector<ExpatContentNode> m_contentNodes;
    Vector<int> m_openGroups;
};
WTF_MAKE_TZONE_ALLOCATED_IMPL(ExpatParser);

// ---- The parser object ----

static ExpatParser& expatParserOf(JSValue self)
{
    return *std::bit_cast<ExpatParser*>(static_cast<uintptr_t>(PyRecord::from(self)->number));
}

static void expatDestroyParser(JSCell* cell)
{
    // The collector may be sweeping, when a cell cannot be asked what it is.
    auto* record = static_cast<PyRecord*>(cell);
    delete std::bit_cast<ExpatParser*>(static_cast<uintptr_t>(record->number));
    record->number = 0;
}

static PyType* expatParserType(JSGlobalObject*);

template<typename... Arguments>
static PyRecord* expatCreateParserObject(JSGlobalObject* globalObject, Arguments&&... arguments)
{
    VM& vm = getVM(globalObject);
    PyRecord* record = PyRecord::create(globalObject, expatParserType(globalObject), ExpatFieldCount);
    record->number = static_cast<int64_t>(std::bit_cast<uintptr_t>(new ExpatParser(record, std::forward<Arguments>(arguments)...)));
    vm.heap.addFinalizerForAnyThread(record, expatDestroyParser);
    return record;
}

// ExpatError("message: line 1, column 2"), with the attributes code, lineno and offset.
static JSValue expatRaise(JSGlobalObject* globalObject, int code, uint64_t line, uint64_t column, const String& message = { })
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = !message.isNull() ? message : makeString(expatErrors[code].message, ": line "_s, line, ", column "_s, column);
    JSValue error = call(globalObject, expatErrorType(globalObject), makeStr(vm, text));
    RETURN_IF_EXCEPTION(scope, { });
    setAttr(globalObject, error, internString(vm, "code"_s), jsNumber(code));
    RETURN_IF_EXCEPTION(scope, { });
    setAttr(globalObject, error, internString(vm, "lineno"_s), makeInt(globalObject, line));
    RETURN_IF_EXCEPTION(scope, { });
    setAttr(globalObject, error, internString(vm, "offset"_s), makeInt(globalObject, column));
    RETURN_IF_EXCEPTION(scope, { });
    return raiseException(globalObject, scope, error);
}

static JSValue expatParseResult(JSGlobalObject* globalObject, ExpatParser& parser, bool ok)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    RETURN_IF_EXCEPTION(scope, { });
    if (!ok)
        RELEASE_AND_RETURN(scope, expatRaise(globalObject, parser.errorCode(), parser.currentLine(), parser.currentColumn()));
    if (!parser.flushCharacterBuffer(globalObject))
        return { };
    return jsNumber(1);
}

// The arguments of a method that takes at most `max` of them, by position only, and complains as those of pyexpat do.
static bool expatCheckArguments(JSGlobalObject* globalObject, ASCIILiteral name, const CallArgs& args, unsigned min, unsigned max)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto plural = [](unsigned count) { return count == 1 ? ""_s : "s"_s; };
    unsigned total = args.totalCount();
    if (total > max) {
        raiseTypeError(globalObject, scope, name, "() takes at most "_s, max, " argument"_s, plural(max), " ("_s, total, " given)"_s);
        return false;
    }
    if (args.size() < min) {
        raiseTypeError(globalObject, scope, name, "() takes "_s, min == max ? "exactly "_s : "at least "_s, min, " positional argument"_s, plural(min), " ("_s, args.size(), " given)"_s);
        return false;
    }
    if (args.keywordCount()) {
        raiseTypeError(globalObject, scope, name, "() got an unexpected keyword argument '"_s, stringOf(args.keywordName(0)), '\'');
        return false;
    }
    return true;
}

static JSValue expatParse(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, expatCheckArguments(globalObject, "Parse"_s, args, 1, 2)))
        return { };
    bool isFinal = args.size() > 1 && isTrue(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    ExpatParser& parser = expatParserOf(self);
    if (parser.isInCallback)
        return raiseRuntimeError(globalObject, scope, "cannot call Parse() from within a handler"_s);

    JSValue data = args[0];
    bool ok;
    if (isStr(data)) {
        String text = strValue(data)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        auto encoded = tryEncodeUTF8(text);
        if (!encoded) {
            // For the sake of the exception.
            strEncode(globalObject, strValue(data), nullptr, nullptr);
            RETURN_IF_EXCEPTION(scope, { });
            return raiseValueError(globalObject, scope, "surrogates not allowed"_s);
        }
        parser.setEncodingIfNotStarted("utf-8"_s);
        ok = parser.parse(globalObject, encoded->span(), isFinal);
    } else {
        BufferHolder holder(globalObject);
        if (!holder.get(data, BufferSimple))
            return { };
        // The handlers may do as they like with the object, so this goes by a copy.
        Vector<uint8_t> copy(holder.buffer.span());
        ok = parser.parse(globalObject, copy.span(), isFinal);
    }
    RELEASE_AND_RETURN(scope, expatParseResult(globalObject, parser, ok));
}

static JSValue expatParseFile(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, expatCheckArguments(globalObject, "ParseFile"_s, args, 1, 1)))
        return { };
    ExpatParser& parser = expatParserOf(self);
    if (parser.isInCallback)
        return raiseRuntimeError(globalObject, scope, "cannot call ParseFile() from within a handler"_s);
    JSValue read = getAttrIfPresent(globalObject, args[0], rt.names.read());
    RETURN_IF_EXCEPTION(scope, { });
    if (!read)
        return raiseTypeError(globalObject, scope, "argument must have 'read' attribute"_s);
    constexpr unsigned chunkSize = 2048;
    bool ok = true;
    while (true) {
        JSValue data = call(globalObject, read, jsNumber(chunkSize));
        RETURN_IF_EXCEPTION(scope, { });
        Vector<uint8_t> bytes;
        if (auto* object = PyBytes::tryFrom(data))
            bytes.append(object->span());
        else if (auto* array = PyByteArray::tryFrom(data))
            bytes.append(array->span());
        else
            return raiseTypeError(globalObject, scope, "read() did not return a bytes object (type="_s, typeName(globalObject, data), ')');
        if (bytes.size() > chunkSize)
            return raiseValueError(globalObject, scope, "read() returned too much data: "_s, chunkSize, " bytes requested, "_s, bytes.size(), " returned"_s);
        ok = parser.parse(globalObject, bytes.span(), bytes.isEmpty());
        RETURN_IF_EXCEPTION(scope, { });
        if (!ok || bytes.isEmpty())
            break;
    }
    RELEASE_AND_RETURN(scope, expatParseResult(globalObject, parser, ok));
}

// An argument that CPython takes as a C string. False: an exception.
static bool expatStringArgument(JSGlobalObject* globalObject, ASCIILiteral function, ASCIILiteral argument, JSValue value, bool allowsNone, String& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    result = String();
    if (allowsNone && isNone(value))
        return true;
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, function, "() "_s, argument, " must be str"_s, allowsNone ? " or None"_s : ""_s, ", not "_s, typeNameOfArgument(globalObject, value));
        return false;
    }
    result = strValue(value)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (result.contains(static_cast<char16_t>(0))) {
        raiseValueError(globalObject, scope, "embedded null character"_s);
        return false;
    }
    if (result.isNull())
        result = emptyString();
    return true;
}

static JSValue expatSetBase(JSGlobalObject* globalObject, JSValue self, JSValue base)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text;
    if (failed(scope, expatStringArgument(globalObject, "SetBase"_s, "argument"_s, base, false, text)))
        return { };
    expatParserOf(self).setBase(text);
    return jsUndefined();
}

static JSValue expatGetBase(JSGlobalObject* globalObject, JSValue self)
{
    const String& base = expatParserOf(self).base();
    return base.isNull() ? jsUndefined() : JSValue(makeStr(getVM(globalObject), base));
}

static JSValue expatGetInputContext(JSGlobalObject* globalObject, JSValue self)
{
    ExpatParser& parser = expatParserOf(self);
    if (!parser.isInCallback)
        return jsUndefined();
    return PyBytes::create(globalObject, parser.inputContext());
}

static JSValue expatExternalEntityParserCreate(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, expatCheckArguments(globalObject, "ExternalEntityParserCreate"_s, args, 1, 2)))
        return { };
    String context, encoding;
    if (failed(scope, expatStringArgument(globalObject, "ExternalEntityParserCreate"_s, "argument 1"_s, args[0], true, context)))
        return { };
    if (args.size() > 1 && failed(scope, expatStringArgument(globalObject, "ExternalEntityParserCreate"_s, "argument 2"_s, args[1], false, encoding)))
        return { };
    ExpatParser& parent = expatParserOf(self);
    PyRecord* record = expatCreateParserObject(globalObject, parent, context, encoding);
    ExpatParser& parser = expatParserOf(record);
    parser.bufferSize = parent.bufferSize;
    parser.buffersText = parent.buffersText;
    parser.orderedAttributes = parent.orderedAttributes;
    parser.specifiedAttributes = parent.specifiedAttributes;
    parser.namespacePrefixes = parent.namespacePrefixes;
    auto* parentRecord = PyRecord::from(self);
    for (unsigned i = 0; i < ExpatHandlerCount; ++i)
        record->set(vm, i, parentRecord->get(i));
    record->set(vm, ExpatIntern, parentRecord->get(ExpatIntern));
    record->set(vm, ExpatParent, self);
    return record;
}

static JSValue expatSetParamEntityParsing(JSGlobalObject* globalObject, JSValue self, JSValue flag)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int value = toCInt(globalObject, flag);
    RETURN_IF_EXCEPTION(scope, { });
    return jsNumber(expatParserOf(self).setParamEntityParsing(value));
}

static JSValue expatUseForeignDTD(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, expatCheckArguments(globalObject, "UseForeignDTD"_s, args, 0, 1)))
        return { };
    bool flag = !args.size() || isTrue(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    ExpatParser& parser = expatParserOf(self);
    if (ExpatError error = parser.useForeignDTD(flag))
        RELEASE_AND_RETURN(scope, expatRaise(globalObject, error, parser.currentLine(), parser.currentColumn()));
    return jsUndefined();
}

static JSValue expatSetReparseDeferralEnabled(JSGlobalObject* globalObject, JSValue self, JSValue enabled)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool value = isTrue(globalObject, enabled);
    RETURN_IF_EXCEPTION(scope, { });
    expatParserOf(self).setReparseDeferralEnabled(value);
    return jsUndefined();
}

static JSValue expatGetReparseDeferralEnabled(JSGlobalObject*, JSValue self) { return jsBoolean(expatParserOf(self).isReparseDeferralEnabled()); }

// The limits that Expat can be told: checked, and otherwise left as they are.
template<bool isFactor, bool isForExpansion>
static JSValue expatSetLimit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, expatCheckArguments(globalObject, isFactor ? "SetMaximumAmplification"_s : "SetActivationThreshold"_s, args, 1, 1)))
        return { };
    ExpatParser& parser = expatParserOf(self);
    if (PyRecord::from(self)->get(ExpatParent))
        RELEASE_AND_RETURN(scope, expatRaise(globalObject, ExpatErrorInvalidArgument, parser.currentLine(), parser.currentColumn(), "parser must be a root parser"_s));
    if (isFactor) {
        double factor = toDouble(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        if (!(factor >= 1.0))
            RELEASE_AND_RETURN(scope, expatRaise(globalObject, ExpatErrorInvalidArgument, parser.currentLine(), parser.currentColumn(), "'max_factor' must be at least 1.0"_s));
        (isForExpansion ? parser.dtd().maximumExpansion : parser.dtd().maximumStorage) = static_cast<float>(factor);
    } else {
        JSValue index = toIndex(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        if (intSign(index) < 0)
            return raiseValueError(globalObject, scope, "Cannot convert negative int"_s);
        if (intBitLength(index) > 64)
            return raiseOverflowError(globalObject, scope, "Python int too large for C unsigned long long"_s);
        (isForExpansion ? parser.dtd().expansionThreshold : parser.dtd().storageThreshold) = tryInt64(index) ? static_cast<uint64_t>(*tryInt64(index)) : std::numeric_limits<uint64_t>::max();
    }
    return jsUndefined();
}

static constexpr auto expatSetExpansionThreshold = expatSetLimit<false, true>;
static constexpr auto expatSetExpansionFactor = expatSetLimit<true, true>;
static constexpr auto expatSetAllocationThreshold = expatSetLimit<false, false>;
static constexpr auto expatSetAllocationFactor = expatSetLimit<true, false>;

// ---- Attributes ----

template<unsigned handler>
static JSValue expatGetHandler(JSGlobalObject*, JSValue self)
{
    JSValue value = PyRecord::from(self)->get(handler);
    return value ? value : jsUndefined();
}

template<unsigned handler>
static void expatSetHandler(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseRuntimeError(globalObject, scope, "Cannot delete attribute"_s);
        return;
    }
    ExpatParser& parser = expatParserOf(self);
    // What has been collected is for the handler that was there when it was.
    if (handler == ExpatHandlerCharacterData && !parser.flushCharacterBuffer(globalObject))
        return;
    if (isNone(value))
        value = { };
    PyRecord::from(self)->set(vm, handler, value);
    parser.didSetHandler(handler, !!value);
}

static bool expatFlagValue(JSGlobalObject* globalObject, JSValue value, bool& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseRuntimeError(globalObject, scope, "Cannot delete attribute"_s);
        return false;
    }
    result = isTrue(globalObject, value);
    return !scope.exception();
}

static JSValue expatGetErrorCode(JSGlobalObject*, JSValue self) { return jsNumber(expatParserOf(self).errorCode()); }
static JSValue expatGetLineNumber(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, expatParserOf(self).currentLine()); }
static JSValue expatGetColumnNumber(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, expatParserOf(self).currentColumn()); }
static JSValue expatGetByteIndex(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, expatParserOf(self).currentByteIndex()); }
static JSValue expatGetBufferSize(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, expatParserOf(self).bufferSize); }
static JSValue expatGetBufferText(JSGlobalObject*, JSValue self) { return jsBoolean(expatParserOf(self).buffersText); }
static JSValue expatGetBufferUsed(JSGlobalObject* globalObject, JSValue self) { return makeInt(globalObject, expatParserOf(self).bufferUsed()); }
static JSValue expatGetNamespacePrefixes(JSGlobalObject*, JSValue self) { return jsBoolean(expatParserOf(self).namespacePrefixes); }
static JSValue expatGetOrderedAttributes(JSGlobalObject*, JSValue self) { return jsBoolean(expatParserOf(self).orderedAttributes); }
static JSValue expatGetSpecifiedAttributes(JSGlobalObject*, JSValue self) { return jsBoolean(expatParserOf(self).specifiedAttributes); }

static JSValue expatGetIntern(JSGlobalObject*, JSValue self)
{
    JSValue intern = PyRecord::from(self)->get(ExpatIntern);
    return intern ? intern : jsUndefined();
}

static void expatSetBufferText(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool flag;
    if (failed(scope, expatFlagValue(globalObject, value, flag)))
        return;
    ExpatParser& parser = expatParserOf(self);
    if (flag) {
        if (!parser.buffersText) {
            parser.discardCharacterBuffer();
            parser.buffersText = true;
        }
        return;
    }
    if (parser.buffersText) {
        if (!parser.flushCharacterBuffer(globalObject))
            return;
        parser.buffersText = false;
    }
}

static void expatSetBufferSize(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseRuntimeError(globalObject, scope, "Cannot delete attribute"_s);
        return;
    }
    if (!isInt(value)) {
        raiseTypeError(globalObject, scope, "buffer_size must be an integer"_s);
        return;
    }
    auto size = tryInt64(intValue(value));
    if (!size) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C long"_s);
        return;
    }
    if (*size <= 0) {
        raiseValueError(globalObject, scope, "buffer_size must be greater than zero"_s);
        return;
    }
    ExpatParser& parser = expatParserOf(self);
    if (*size == parser.bufferSize)
        return;
    if (*size > INT_MAX) {
        raiseValueError(globalObject, scope, "buffer_size must not be greater than "_s, INT_MAX);
        return;
    }
    if (parser.buffersText && !parser.flushCharacterBuffer(globalObject))
        return;
    // As in CPython: there is a buffer from now on, whether there was one or not.
    parser.buffersText = true;
    parser.bufferSize = *size;
}

static void expatSetNamespacePrefixes(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool flag;
    if (failed(scope, expatFlagValue(globalObject, value, flag)))
        return;
    expatParserOf(self).namespacePrefixes = flag;
    expatParserOf(self).setReturnsTriplets(flag);
}

static void expatSetOrderedAttributes(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    bool flag;
    if (expatFlagValue(globalObject, value, flag))
        expatParserOf(self).orderedAttributes = flag;
}

static void expatSetSpecifiedAttributes(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    bool flag;
    if (expatFlagValue(globalObject, value, flag))
        expatParserOf(self).specifiedAttributes = flag;
}

static constexpr NativeDef expatParserMethods[] = {
    PY_METHOD("Parse", expatParse, "!($self, data, isfinal=False, /)"),
    PY_METHOD("ParseFile", expatParseFile, "!($self, file, /)"),
    PY_METHOD("SetBase", expatSetBase, "($self, base, /)"),
    PY_METHOD("GetBase", expatGetBase, "($self, /)"),
    PY_METHOD("GetInputContext", expatGetInputContext, "($self, /)"),
    PY_METHOD("ExternalEntityParserCreate", expatExternalEntityParserCreate, "($self, *args, **kwargs)"),
    PY_METHOD("SetParamEntityParsing", expatSetParamEntityParsing, "($self, flag, /)"),
    PY_METHOD("UseForeignDTD", expatUseForeignDTD, "!($self, flag=True, /)"),
    PY_METHOD("SetBillionLaughsAttackProtectionActivationThreshold", expatSetExpansionThreshold, "!($self, threshold, /)"),
    PY_METHOD("SetBillionLaughsAttackProtectionMaximumAmplification", expatSetExpansionFactor, "!($self, max_factor, /)"),
    PY_METHOD("SetAllocTrackerActivationThreshold", expatSetAllocationThreshold, "!($self, threshold, /)"),
    PY_METHOD("SetAllocTrackerMaximumAmplification", expatSetAllocationFactor, "!($self, max_factor, /)"),
    PY_METHOD("SetReparseDeferralEnabled", expatSetReparseDeferralEnabled, "($self, enabled, /)"),
    PY_METHOD("GetReparseDeferralEnabled", expatGetReparseDeferralEnabled, "($self, /)"),
};

static constexpr NativeGetSet expatParserGetSets[] = {
    PY_MEMBER("intern", expatGetIntern),
    { "ErrorCode"_s, expatGetErrorCode },
    { "ErrorLineNumber"_s, expatGetLineNumber },
    { "ErrorColumnNumber"_s, expatGetColumnNumber },
    { "ErrorByteIndex"_s, expatGetByteIndex },
    { "CurrentLineNumber"_s, expatGetLineNumber },
    { "CurrentColumnNumber"_s, expatGetColumnNumber },
    { "CurrentByteIndex"_s, expatGetByteIndex },
    { "buffer_size"_s, expatGetBufferSize, expatSetBufferSize },
    { "buffer_text"_s, expatGetBufferText, expatSetBufferText },
    { "buffer_used"_s, expatGetBufferUsed },
    { "namespace_prefixes"_s, expatGetNamespacePrefixes, expatSetNamespacePrefixes },
    { "ordered_attributes"_s, expatGetOrderedAttributes, expatSetOrderedAttributes },
    { "specified_attributes"_s, expatGetSpecifiedAttributes, expatSetSpecifiedAttributes },
#define EXPAT_DEFINE_GETSET(id, name) { name ""_s, expatGetHandler<ExpatHandler##id>, expatSetHandler<ExpatHandler##id> },
    FOR_EACH_EXPAT_HANDLER(EXPAT_DEFINE_GETSET)
#undef EXPAT_DEFINE_GETSET
};

static constexpr TypeDef expatParserTypeDef {
    "pyexpat.xmlparser"_s, { TypeFlag::OwnLayout, TypeFlag::Immutable, TypeFlag::DisallowInstantiation }, expatParserMethods, expatParserGetSets, "XML parser"_s
};

static PyType* expatParserType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(expatParserTypeDef); }

// ---- The module ----

static JSValue expatParserCreate(JSGlobalObject* globalObject, JSValue encodingValue, JSValue separatorValue, JSValue intern)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String encoding, separator;
    if (encodingValue && failed(scope, expatStringArgument(globalObject, "ParserCreate"_s, "argument 'encoding'"_s, encodingValue, true, encoding)))
        return { };
    if (separatorValue && failed(scope, expatStringArgument(globalObject, "ParserCreate"_s, "argument 'namespace_separator'"_s, separatorValue, true, separator)))
        return { };
    if (!separator.isNull()) {
        StrView view(separator);
        if (view.length() > 1)
            return raiseValueError(globalObject, scope, "namespace_separator must be at most one character, omitted, or None"_s);
        if (!view.isSimple())
            return raiseValueError(globalObject, scope, "namespace_separator must be at most one character, omitted, or None"_s);
    }
    if (!intern)
        intern = PyDict::create(globalObject);
    else if (isNone(intern))
        intern = { };
    else if (!isDict(intern))
        return raiseTypeError(globalObject, scope, "intern must be a dictionary"_s);
    PyRecord* record = expatCreateParserObject(globalObject, encoding, !separator.isNull(), separator.isEmpty() ? static_cast<char16_t>(0) : separator[0]);
    record->set(vm, ExpatIntern, intern);
    return record;
}

static JSValue expatErrorString(JSGlobalObject* globalObject, JSValue code)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t value = toInt64(globalObject, code);
    RETURN_IF_EXCEPTION(scope, { });
    if (value <= 0 || value >= ExpatErrorCount)
        return jsUndefined();
    return makeStr(vm, expatErrors[value].message);
}

static constexpr NativeDef expatFunctions[] = {
    PY_FUNCTION("ParserCreate", expatParserCreate, "(encoding=None, namespace_separator=None, intern=<unrepresentable>)"),
    PY_FUNCTION("ErrorString", expatErrorString, "(code, /)"),
};

void initializeExpatModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    module->add(globalObject, "__doc__"_s, makeStr(vm, "Python wrapper for Expat parser."_s));
    module->addFunctions(globalObject, expatFunctions);
    module->add(globalObject, "error"_s, expatErrorType(globalObject));
    module->add(globalObject, "ExpatError"_s, expatErrorType(globalObject));
    module->add(globalObject, "XMLParserType"_s, expatParserType(globalObject));
    module->add(globalObject, "EXPAT_VERSION"_s, makeStr(vm, "expat_2.9.0"_s));
    module->add(globalObject, "version_info"_s, PyTuple::create(globalObject, { jsNumber(2), jsNumber(9), jsNumber(0) }));
    module->add(globalObject, "native_encoding"_s, makeStr(vm, "UTF-8"_s));
    module->add(globalObject, "XML_PARAM_ENTITY_PARSING_NEVER"_s, jsNumber(0));
    module->add(globalObject, "XML_PARAM_ENTITY_PARSING_UNLESS_STANDALONE"_s, jsNumber(1));
    module->add(globalObject, "XML_PARAM_ENTITY_PARSING_ALWAYS"_s, jsNumber(2));

    struct Feature {
        ASCIILiteral name;
        int value;
    };
    static constexpr Feature features[] = {
        { "sizeof(XML_Char)"_s, 1 }, { "sizeof(XML_LChar)"_s, 1 }, { "XML_DTD"_s, 0 }, { "XML_CONTEXT_BYTES"_s, 1024 }, { "XML_NS"_s, 0 },
        { "XML_BLAP_MAX_AMP"_s, 100 }, { "XML_BLAP_ACT_THRES"_s, 8388608 }, { "XML_GE"_s, 0 }, { "XML_AT_MAX_AMP"_s, 100 }, { "XML_AT_ACT_THRES"_s, 67108864 },
    };
    JSArray* featureList = newList(globalObject);
    for (auto& feature : features)
        listAppend(globalObject, featureList, PyTuple::create(globalObject, { makeStr(vm, feature.name), jsNumber(feature.value) }));
    module->add(globalObject, "features"_s, featureList);

    PyModule* errors = PyModule::create(globalObject, "pyexpat.errors"_s);
    errors->add(globalObject, "__doc__"_s, makeStr(vm, "Constants used to describe error conditions."_s));
    PyDict* codes = PyDict::create(globalObject);
    PyDict* messages = PyDict::create(globalObject);
    for (int code = 1; code < ExpatErrorCount; ++code) {
        JSValue message = makeStr(vm, expatErrors[code].message);
        errors->add(globalObject, expatErrors[code].name, message);
        codes->set(globalObject, message, jsNumber(code));
        messages->set(globalObject, jsNumber(code), message);
    }
    errors->add(globalObject, "codes"_s, codes);
    errors->add(globalObject, "messages"_s, messages);
    module->add(globalObject, "errors"_s, errors);
    rt.modules()->set(globalObject, makeStr(vm, "pyexpat.errors"_s), errors);

    PyModule* model = PyModule::create(globalObject, "pyexpat.model"_s);
    model->add(globalObject, "__doc__"_s, makeStr(vm, "Constants used to interpret content model information."_s));
    static constexpr ASCIILiteral modelNames[] = {
        "XML_CTYPE_EMPTY"_s, "XML_CTYPE_ANY"_s, "XML_CTYPE_MIXED"_s, "XML_CTYPE_NAME"_s, "XML_CTYPE_CHOICE"_s, "XML_CTYPE_SEQ"_s,
        "XML_CQUANT_NONE"_s, "XML_CQUANT_OPT"_s, "XML_CQUANT_REP"_s, "XML_CQUANT_PLUS"_s,
    };
    for (int i = 0; i < 6; ++i)
        model->add(globalObject, modelNames[i], jsNumber(i + 1));
    for (int i = 0; i < 4; ++i)
        model->add(globalObject, modelNames[6 + i], jsNumber(i));
    module->add(globalObject, "model"_s, model);
    rt.modules()->set(globalObject, makeStr(vm, "pyexpat.model"_s), model);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
