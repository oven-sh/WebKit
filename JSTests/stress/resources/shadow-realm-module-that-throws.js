// Evaluating this module throws whatever the importing realm staged on its global object.
globalThis.evaluations = (globalThis.evaluations | 0) + 1;
throw globalThis.valueToThrow;
