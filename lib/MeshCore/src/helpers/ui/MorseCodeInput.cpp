#include "MorseCodeInput.h"
#include <Arduino.h>
#include <string.h>

#ifdef PIN_BUZZER
  #include "buzzer.h"
#endif

// Forward declaration - buzzer instance will be defined elsewhere
#ifdef PIN_BUZZER
  extern genericBuzzer buzzer;
#endif

constexpr const char* MorseCodeInput::MORSE_TABLE[];

MorseCodeInput::MorseCodeInput() 
  : _pattern_length(0), _last_change_time(0), _last_char(0) {
  memset(_current_pattern, 0, sizeof(_current_pattern));
}

void MorseCodeInput::begin() {
  reset();
}

void MorseCodeInput::addDot() {
  _addSymbol('.');
  _last_change_time = millis();
}

void MorseCodeInput::addDash() {
  _addSymbol('-');
  _last_change_time = millis();
}

bool MorseCodeInput::checkGap(char& out_char) {
  out_char = 0;
  
  if (_pattern_length > 0) {
    uint32_t gap_since_last = millis() - _last_change_time;
    
    if (gap_since_last > CHAR_SEPARATOR_TIME) {
      // Character separator detected - decode current pattern
      char decoded = decodePattern(_current_pattern);
      
      if (decoded != 0) {
        _last_char = decoded;
        out_char = decoded;
        reset();
        return true;
      } else {
        // Invalid pattern, reset
        reset();
      }
    }
  }
  return false;
}

void MorseCodeInput::_addSymbol(char symbol) {
  if (_pattern_length < 5) {
    _current_pattern[_pattern_length] = symbol;
    _pattern_length++;
    _current_pattern[_pattern_length] = 0;  // Null terminate
  }
}

void MorseCodeInput::reset() {
  memset(_current_pattern, 0, sizeof(_current_pattern));
  _pattern_length = 0;
  _last_change_time = millis();
}

char MorseCodeInput::decodePattern(const char* pattern) {
  if (!pattern || *pattern == 0) return 0;
  
  // Search morse table for matching pattern
  for (int i = 0; i < 26; i++) {
    if (strcmp(MORSE_TABLE[i], pattern) == 0) {
      return 'A' + i;
    }
  }
  
  // Common numbers and symbols
  if (strcmp(pattern, "-----") == 0) return '0';
  if (strcmp(pattern, ".----") == 0) return '1';
  if (strcmp(pattern, "..---") == 0) return '2';
  if (strcmp(pattern, "...--") == 0) return '3';
  if (strcmp(pattern, "....-") == 0) return '4';
  if (strcmp(pattern, ".....") == 0) return '5';
  if (strcmp(pattern, "-....") == 0) return '6';
  if (strcmp(pattern, "--...") == 0) return '7';
  if (strcmp(pattern, "---..") == 0) return '8';
  if (strcmp(pattern, "----.") == 0) return '9';
  
  // Common punctuation
  if (strcmp(pattern, ".-.-.-") == 0) return '.'; // Period
  if (strcmp(pattern, "--..--") == 0) return ','; // Comma
  if (strcmp(pattern, "..--..") == 0) return '?'; // Question mark
  if (strcmp(pattern, "-....-") == 0) return '-'; // Dash/Hyphen
  if (strcmp(pattern, "-.-.-.") == 0) return ';'; // Semicolon
  if (strcmp(pattern, "---.") == 0) return '@';    // Commercial at (@)
  
  return 0;  // Unknown pattern
}

void MorseCodeInput::playMorseAudio(char c) {
#ifdef PIN_BUZZER
  const char* pattern = nullptr;
  
  // Find pattern for character
  if (c >= 'A' && c <= 'Z') {
    pattern = MORSE_TABLE[c - 'A'];
  } else if (c >= 'a' && c <= 'z') {
    pattern = MORSE_TABLE[c - 'a'];
  }
  
  if (!pattern) return;
  
  // Generate RTTTL notation for morse code
  // Format: name:d=duration,o=octave,b=bpm:notes
  // Morse: dot=1 unit, dash=3 units, gap=1 unit
  // Use 16th notes for dots, 32nd for dashes
  
  static char rtttl_buffer[100];
  strcpy(rtttl_buffer, "morse:d=16,o=4,b=480:");
  
  // Build RTTTL string based on morse pattern
  for (const char* p = pattern; *p; p++) {
    if (*p == '.') {
      strcat(rtttl_buffer, "c,");  // dot as 16th note
    } else if (*p == '-') {
      strcat(rtttl_buffer, "c,c,c,");  // dash as 3 16th notes
    }
  }
  
  // Remove trailing comma if present
  int len = strlen(rtttl_buffer);
  if (len > 0 && rtttl_buffer[len-1] == ',') {
    rtttl_buffer[len-1] = 0;
  }
  
  // Play using buzzer if available
  buzzer.play(rtttl_buffer);
#endif
}

void MorseCodeInput::playMorseMessage(const char* message) {
  if (!message) return;
  
#ifdef PIN_BUZZER
  // Generate full RTTTL string for the message
  static char rtttl_buffer[512];
  strcpy(rtttl_buffer, "msg:d=16,o=4,b=480:");
  
  for (const char* p = message; *p; p++) {
    const char* pattern = nullptr;
    
    if (*p == ' ') {
      // Word gap (already handled by gaps between characters)
      continue;
    } else if (*p >= 'A' && *p <= 'Z') {
      pattern = MORSE_TABLE[*p - 'A'];
    } else if (*p >= 'a' && *p <= 'z') {
      pattern = MORSE_TABLE[*p - 'a'];
    }
    
    if (pattern) {
      // Add morse pattern to RTTTL
      for (const char* s = pattern; *s; s++) {
        if (*s == '.') {
          strcat(rtttl_buffer, "c,");
        } else if (*s == '-') {
          strcat(rtttl_buffer, "c,c,c,");
        }
      }
      // Add gap between characters (skip it for now, handled by RTTTL timing)
    }
  }
  
  // Remove trailing comma if present
  int len = strlen(rtttl_buffer);
  if (len > 0 && rtttl_buffer[len-1] == ',') {
    rtttl_buffer[len-1] = 0;
  }
  
  // Play the complete message
  buzzer.play(rtttl_buffer);
#endif
}

const char* MorseCodeInput::getPattern(char c) {
  if (c >= 'A' && c <= 'Z') return MORSE_TABLE[c - 'A'];
  if (c >= 'a' && c <= 'z') return MORSE_TABLE[c - 'a'];
  if (c == '0') return "-----";
  if (c == '1') return ".----";
  if (c == '2') return "..---";
  if (c == '3') return "...--";
  if (c == '4') return "....-";
  if (c == '5') return ".....";
  if (c == '6') return "-....";
  if (c == '7') return "--...";
  if (c == '8') return "---..";
  if (c == '9') return "----.";
  return nullptr;
}

