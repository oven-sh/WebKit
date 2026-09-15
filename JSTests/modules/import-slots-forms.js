import { shouldBe } from "./resources/assert.js";
import * as forms from "./import-slots/forms.js";

async function drain(iterable) { const values = []; for await (const value of iterable) values.push(value); return values; }
for (let i = 0; i < testLoopCount; ++i) {
    forms.set(i);
    shouldBe(forms.arrow(), i);
    shouldBe(forms.nested(), i);
    shouldBe(forms.viaEval(), i);
    shouldBe(forms.viaIndirectEval(), "undefined");
    shouldBe(forms.viaFunctionConstructor(), "undefined");
    shouldBe(forms.defaultParameter(), i);
    shouldBe(forms.defaultParameter("given"), "given");
    shouldBe(forms.withTypeof(), "number,number,object,object");
    shouldBe(forms.WithStatic.read(), i);
    shouldBe(new forms.WithStatic().accessor, i);
    shouldBe(forms.objectLiteral.accessor, i);
    shouldBe(forms.objectLiteral.method(), i);
    shouldBe(forms.destructure(), `${i},${i}`);
    shouldBe(forms.labelled(), i);
    shouldBe(forms.templated(), `${i}:42`);
    shouldBe(forms.optional(), "object,object");
    shouldBe(forms.inTryFinally(), i);
    shouldBe(forms.arrow(), i + 1);
}
shouldBe(forms.WithStatic.captured, 0);
shouldBe(forms.objectLiteral.computed42, 0);
forms.set(10);
shouldBe(JSON.stringify([...forms.generator()]), "[10,11]");
shouldBe(await forms.asynchronous(), 11);
shouldBe(JSON.stringify(await drain(forms.asyncGenerator())), "[11]");
