import { Cocoa as Drink, changeCocoa, SubDrink, changeCappuccino } from "./aliasing/drink.js"
import { shouldBe, shouldThrow } from "./resources/assert.js";

shouldBe(Drink, "Cocoa");
shouldBe(SubDrink, "Cappuccino");
shouldThrow(() => {
    Cocoa
}, `ReferenceError: Cocoa is not defined`);

shouldThrow(() => {
    Cappuccino
}, `ReferenceError: Cappuccino is not defined`);

changeCocoa("Mocha");
shouldBe(Drink, "Mocha");

changeCappuccino("Matcha");
shouldBe(SubDrink, "Matcha");
