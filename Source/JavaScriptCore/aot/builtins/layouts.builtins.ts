import * as b from "bun-builtins-table";

const bufferLength = b.int(0, b.checked(2 ** 32, "MAX_ARRAY_BUFFER_SIZE"));

export default b.declarations(
  b.layout("IteratorResult", { value: b.any, done: b.bool }),
  b.layout("PropertyDescriptor", {
    value: b.maybeAbsent(b.any),
    writable: b.maybeAbsent(b.bool),
    get: b.maybeAbsent(b.any),
    set: b.maybeAbsent(b.any),
    enumerable: b.bool,
    configurable: b.bool,
  }),
  b.tuple("EntriesPair", [b.any, b.any]),
  b.layout("ReadAndWrittenCounts", { read: bufferLength, written: bufferLength }),
);
