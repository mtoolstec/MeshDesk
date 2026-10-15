#pragma once
#include <string>
using String = std::string;
class Preferences {
public:
    static inline String protocol = "off", configured = "meshcore";
    void begin(const char *, bool) {}
    void end() {}
    String getString(const char *key, const char *) { return std::string(key) == "protocol" ? protocol : configured; }
    void putString(const char *key, const char *value) { (std::string(key) == "protocol" ? protocol : configured) = value; }
};
