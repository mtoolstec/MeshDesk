#pragma once

#include <Arduino.h>
#include <stdint.h>

// Morse Code representation and detection
class MorseCodeInput {
public:
  // Morse symbols: . = dot, - = dash, space separates characters
  static constexpr const char* MORSE_TABLE[] = {
    ".-",    // A
    "-...",  // B
    "-.-.",  // C
    "-..",   // D
    ".",     // E
    "..-.",  // F
    "--.",   // G
    "....",  // H
    "..",    // I
    ".---",  // J
    "-.-",   // K
    ".-..",  // L
    "--",    // M
    "-.",    // N
    "---",   // O
    ".--.",  // P
    "--.-",  // Q
    ".-.",   // R
    "...",   // S
    "-",     // T
    "..-",   // U
    "...-",  // V
    ".--",   // W
    "-..-",  // X
    "-.--",  // Y
    "--.."   // Z
  };

  // Morse code timings (in milliseconds)
  static constexpr uint32_t DOT_THRESHOLD = 150;    // If cover duration < 150ms: dot
  static constexpr uint32_t DASH_THRESHOLD = 400;   // If cover duration >= 150ms and < 400ms: dash
  static constexpr uint32_t LONG_DASH_MAX = 800;    // Maximum dash duration
  
  static constexpr uint32_t CHAR_SEPARATOR_TIME = 800;   // Gap between characters
  static constexpr uint32_t WORD_SEPARATOR_TIME = 2000;  // Gap between words
  
  struct MorseChar {
    char pattern[6]; // max 5 morse symbols + null terminator
    uint8_t length;  // number of symbols (dots/dashes)
  };
  
  MorseCodeInput();
  
  // Initialize morse code input (call once at startup)
  void begin();
  
  // Add a Dot to the pattern
  void addDot();

  // Add a Dash to the pattern
  void addDash();

  // Check for timeout/gap. Returns true if a character was decoded.
  bool checkGap(char& out_char);
  
  // Get current morse pattern being input
  const char* getCurrentPattern() const { return _current_pattern; }
  
  // Get last decoded character
  char getLastChar() const { return _last_char; }

  // Get last change time
  uint32_t getLastChangeTime() const { return _last_change_time; }
  
  // Reset current input
  void reset();
  
  // Decode morse pattern to character
  static char decodePattern(const char* pattern);
  
  // Play morse code audio for a character
  static void playMorseAudio(char c);
  
  // Play complete morse code for a message
  static void playMorseMessage(const char* message);

  // Get pattern for a character (letters and numbers)
  static const char* getPattern(char c);

private:
  char _current_pattern[6];  // Current morse pattern being input
  uint8_t _pattern_length;
  
  uint32_t _last_change_time;
  
  char _last_char;
  
  // Helper to decode morse pattern
  void _addSymbol(char symbol);
};
