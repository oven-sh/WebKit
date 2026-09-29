globalThis.thrown = (globalThis.thrown || 0) + 1; throw new RangeError("as it is run, " + globalThis.thrown);
