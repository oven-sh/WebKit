//@ runNoisyTestWithEnv "host-locale", "LC_ALL=ar_EG.UTF-8"

// The test driver sets LANG=en_US.UTF-8, and the data of en_US has the answers that ECMA-402 gives to a tag without a
// locale. ICU reads LC_ALL first, and with an Arabic host the collations, the numbering system and the direction of
// the text of ICU's default locale are all other than those answers.
load("./intl-locale-info-data-locale.js", "caller relative");
