struct E { }
print("declared");
let e = new E();
print("constructed");
if (Object.isExtensible(e)) throw new Error("extensible");
