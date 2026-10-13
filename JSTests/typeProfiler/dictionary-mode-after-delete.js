var findTypeForExpression = $vm.findTypeForExpression;

load("./driver/driver.js");

// An object with more than 128 properties has a dictionary structure. A delete moves the object to a new
// structure, which takes the dictionary's property table. The log still names the old structure: the shape
// comes from the object's structure then, and has the properties.

function wrapper()
{

var foo = {};
for (var i = 0; i < 150; i++)
    foo["hello" + i] = i;
var loggedThenDeleted = foo;
delete foo.hello0;
var loggedAgain = foo;

}
wrapper();

var types = findTypeForExpression(wrapper, "loggedThenDeleted");
assert(types.globalTypeSet.structures.length === 1, "Should have one structure.");
assert(types.globalTypeSet.structures[0].isInDictionaryMode, "Should be in dictionary mode");
assert(types.globalTypeSet.structures[0].fields.length >= 149, "Should have the 149 properties the object has now, had " + types.globalTypeSet.structures[0].fields.length);

types = findTypeForExpression(wrapper, "loggedAgain");
assert(types.globalTypeSet.structures.length === 1, "Should have one structure.");
assert(types.globalTypeSet.structures[0].fields.length >= 149, "Should have 149 properties, had " + types.globalTypeSet.structures[0].fields.length);
