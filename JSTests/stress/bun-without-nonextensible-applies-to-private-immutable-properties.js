//@ requireOptions("--useNonExtensibleAppliesToPrivate=0")
// Bun: immutable-properties-private-names.js holds without the rule of WebKit/WebKit@b0be2f45a9 too. The engine then refuses such an object by a test of its own.

load("./immutable-properties-private-names.js", "caller relative");
