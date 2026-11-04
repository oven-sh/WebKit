import { twoWordScalars } from "./cpp-types.ts";
import { kindText } from "./hash.ts";
import type { Entry, Kind, Parameter, Schema, SchemaRecord } from "./model.ts";
import {
  entriesOf,
  innermostKindOf,
  isWireType,
  payloadWordsOf,
  recordsOf,
  wireFlagsOf,
  wireParametersOf,
  wireWordsOf,
} from "./model.ts";
import { SchemaError } from "./source.ts";

// emit-rust-builder.ts generates the code that writes these words: a width changed here must be changed there too.

/** The elements of one list or of one variadic operand. */
export interface CountedWords {
  /** The offset of the word that holds the number of elements. */
  readonly count: WireOffset;
  readonly wordsPerElement: number;
}

/** Words counted from the first field of an entry. The head and a value's flags word come before that field. */
export interface WireOffset {
  readonly fixedWords: number;
  /** The lists and variadic operands before this offset, in stream order. */
  readonly counted: readonly CountedWords[];
}

/** Where one parameter of wireParametersOf(entry) starts, and how many words it takes. */
export interface WireSlot {
  readonly parameter: Parameter;
  /** Where the first word is. In a list or a variadic operand it is the count, and the elements follow it. */
  readonly offset: WireOffset;
  /** How many words are there whatever the count says. A list or a variadic operand has one: the count. */
  readonly fixedWords: number;
  /** Null: the field is neither a list nor a variadic operand. */
  readonly wordsPerElement: number | null;
}

type WireWidth = Pick<WireSlot, "fixedWords" | "wordsPerElement">;

interface WireLayout {
  readonly slots: readonly WireSlot[];
  /** The offset just past the last field. */
  readonly end: WireOffset;
}

/** How many words a field of this kind takes, alone or as one element of a list. Null: no body holds one. */
function fieldWordsOf(kind: Kind): number | null {
  switch (kind.tag) {
    case "Scalar":
      return twoWordScalars.has(kind.scalar) ? 2 : 1;
    case "Id":
    case "PlacedWord":
    case "Enum":
    case "Flags":
    case "External":
      return 1;
    case "Record":
    case "Variant": {
      const words = isWireType(kind) ? wireWordsOf(kind) : payloadWordsOf(kind);
      return words > 0 ? words : null;
    }
    case "Span":
    case "List":
      return null;
  }
}

function widthOf(entry: Entry, parameter: Parameter): WireWidth {
  if (parameter.tag === "Operand") return { fixedWords: 1, wordsPerElement: parameter.isVariadic ? 1 : null };
  const { kind, location, name } = parameter;
  const words = fieldWordsOf(kind.tag === "List" ? kind.element : kind);
  if (words === null) {
    throw new SchemaError(location, `field ${name} of ${entry.name}: no body holds ${kindText(kind)}`);
  }
  if (kind.tag === "List") return { fixedWords: 1, wordsPerElement: words };
  return { fixedWords: words, wordsPerElement: null };
}

function layoutOf(entry: Entry): WireLayout {
  if (!entry.isWire) throw new Error(`${entry.name} is no entry of the wire: no body holds its fields`);
  // BodyTerminator::fieldWords() starts right after the head.
  const flag = entry.isTerminator ? wireFlagsOf(entry).at(0) : undefined;
  if (flag !== undefined) {
    const problem = `flag ${flag.name} of ${entry.name}: a terminator has no flags word on the wire`;
    throw new SchemaError(flag.location, problem);
  }
  const slots: WireSlot[] = [];
  let end: WireOffset = { fixedWords: 0, counted: [] };
  for (const parameter of wireParametersOf(entry)) {
    const offset = end;
    const { fixedWords, wordsPerElement } = widthOf(entry, parameter);
    slots.push({ parameter, offset, fixedWords, wordsPerElement });
    end = {
      fixedWords: offset.fixedWords + fixedWords,
      counted: wordsPerElement === null ? offset.counted : [...offset.counted, { count: offset, wordsPerElement }],
    };
  }
  return { slots, end };
}

// The emitters append ` + 1` to the text, add the text to a pointer, and put a receiver in front of each `word(`.
// So it is a top-level sum, a plain decimal literal where nothing is counted, and reads a count only as `word(`.
function textOf({ fixedWords, counted }: WireOffset): string {
  const terms = counted.map(({ count, wordsPerElement }) => {
    const elements = `word(${textOf(count)})`;
    return wordsPerElement === 1 ? elements : `${elements} * ${wordsPerElement}`;
  });
  return [String(fixedWords), ...terms].join(" + ");
}

/** Slot i belongs to wireParametersOf(entry)[i]. */
export function wireSlotsOf(entry: Entry): readonly WireSlot[] {
  return layoutOf(entry).slots;
}

/** `0`, `3 + word(1)`, `5 + word(2) + word(3 + word(2)) * 2`, where word(n) is the word at offset n. */
export function offsetText(slot: WireSlot): string {
  return textOf(slot.offset);
}

/** How many words all the fields take together, in the notation of offsetText(). */
export function fieldWordsText(entry: Entry): string {
  return textOf(layoutOf(entry).end);
}

export function hasFixedSize(entry: Entry): boolean {
  return layoutOf(entry).end.counted.length === 0;
}

/** `element` is the T of a List<T>. */
export function wordsPerElementOf(element: Kind): number {
  const words = fieldWordsOf(element);
  if (words === null) throw new Error(`no list in a body holds ${kindText(element)}`);
  return words;
}

/**
 * The wire records that a field of a wire entry holds, alone or in a list, and the wire records that those embed.
 * They are in declaration order, except that a record comes after the records it embeds.
 */
export function wireRecordsInFields(schema: Schema): SchemaRecord[] {
  const named = new Set<SchemaRecord>();
  for (const entry of entriesOf(schema)) {
    if (!entry.isWire) continue;
    for (const parameter of wireParametersOf(entry)) {
      if (parameter.tag !== "Payload") continue;
      const kind = innermostKindOf(parameter.kind);
      if (kind.tag === "Record" && isWireType(kind)) named.add(kind.record);
    }
  }
  const ordered: SchemaRecord[] = [];
  const place = (record: SchemaRecord): void => {
    if (ordered.includes(record)) return;
    for (const { kind } of record.wireFields) {
      if (kind.tag === "Record" && isWireType(kind)) place(kind.record);
    }
    ordered.push(record);
  };
  for (const record of recordsOf(schema)) {
    if (named.has(record)) place(record);
  }
  return ordered;
}
