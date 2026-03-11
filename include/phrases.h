#ifndef PHRASES_H
#define PHRASES_H

// ─────────────────────────────────────────────
// Human-friendly categories (used in menu)
const char* categoryLabels[] = {
  "STATUS",
  "REQUEST",
  "LOCATION",
  "INTERACTION"
};

// ─────────────────────────────────────────────
// Prebuilt phrases, grouped 10 per category
const char* phraseLibrary[] = {
  // STATUS (0–9)
  "I AM OK", "I AM LOST", "I AM INJURED", "I AM RESTING", "I AM MOVING",
  "I AM SAFE", "I AM WAITING", "I AM COLD", "I AM SCARED", "I AM WET",

  // REQUEST (10–19)
  "SEND HELP", "NEED WATER", "NEED FOOD", "NEED BATTERY", "NEED WARMTH",
  "BRING SUPPLIES", "RADIO CHECK", "GPS HELP", "MEDIC NEEDED", "SEND TRANSPORT",

  // LOCATION (20–29)
  "AT BASE", "NEAR RIVER", "AT CAMP", "NEAR ROAD", "NEAR TREE",
  "NEAR BUILDING", "ON TRAIL", "INSIDE SHELTER", "AT CHECKPOINT", "LOST POSITION",

  // INTERACTION (30–39)
  "WHERE ARE YOU", "I SEE YOU", "COME TO ME", "GO TO BASE", "FOLLOW ME",
  "STAY THERE", "TURN BACK", "JOIN ME", "MEET AT CAMP", "CONFIRM SIGNAL"
};

// ─────────────────────────────────────────────
// Constants for safety + iteration
const int phrasesPerCategory = 10;
const int phraseCount = sizeof(phraseLibrary) / sizeof(phraseLibrary[0]);
const int categoryCount = sizeof(categoryLabels) / sizeof(categoryLabels[0]);

#endif
