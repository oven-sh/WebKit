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

export interface CountedWords {
  readonly count: WireOffset;
  readonly wordsPerElement: number;
}

export interface WireOffset {
  readonly fixedWords: number;
  readonly counted: readonly CountedWords[];
}

export interface WireSlot {
  readonly parameter: Parameter;
  readonly offset: WireOffset;
  readonly fixedWords: number;
  readonly wordsPerElement: number | null;
}

type WireWidth = Pick<WireSlot, "fixedWords" | "wordsPerElement">;

interface WireLayout {
  readonly slots: readonly WireSlot[];
  readonly end: WireOffset;
}

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

function textOf({ fixedWords, counted }: WireOffset): string {
  const terms = counted.map(({ count, wordsPerElement }) => {
    const elements = `word(${textOf(count)})`;
    return wordsPerElement === 1 ? elements : `${elements} * ${wordsPerElement}`;
  });
  return [String(fixedWords), ...terms].join(" + ");
}

export function wireSlotsOf(entry: Entry): readonly WireSlot[] {
  return layoutOf(entry).slots;
}

export function offsetText(slot: WireSlot): string {
  return textOf(slot.offset);
}

export function fieldWordsText(entry: Entry): string {
  return textOf(layoutOf(entry).end);
}

export function hasFixedSize(entry: Entry): boolean {
  return layoutOf(entry).end.counted.length === 0;
}

export function wordsPerElementOf(element: Kind): number {
  const words = fieldWordsOf(element);
  if (words === null) throw new Error(`no list in a body holds ${kindText(element)}`);
  return words;
}

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
