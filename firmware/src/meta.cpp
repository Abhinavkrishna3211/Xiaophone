#include "meta.h"
#include <cctype>

std::string metaLower(std::string s) {
  for (auto& c : s) c = std::tolower((unsigned char)c);
  return s;
}

static std::string clean(std::string s) {
  // drop (...) and [...] groups: "(Official Video)", "[Remastered]", "(radio edit)"
  std::string out;
  int depth = 0;
  for (char c : s) {
    if (c == '(' || c == '[') depth++;
    else if ((c == ')' || c == ']') && depth) depth--;
    else if (!depth) out += c;
  }
  // cut at feat./ft./featuring
  std::string low = metaLower(out);
  for (const char* k : {" feat.", " feat ", " ft.", " ft ", " featuring "}) {
    size_t p = low.find(k);
    if (p != std::string::npos) { out.resize(p); low.resize(p); }
  }
  // collapse whitespace + trim
  std::string r;
  for (char c : out) {
    if (std::isspace((unsigned char)c)) { if (!r.empty() && r.back() != ' ') r += ' '; }
    else r += c;
  }
  while (!r.empty() && (r.back() == ' ' || r.back() == '-')) r.pop_back();
  return r;
}

bool metaParse(const std::string& icy, std::string& artist, std::string& title) {
  size_t p = icy.find(" - ");
  if (p == std::string::npos) return false;
  artist = clean(icy.substr(0, p));
  title = clean(icy.substr(p + 3));
  if (artist.empty() || title.empty()) return false;
  std::string a = metaLower(artist);
  for (const char* k : {"radio paradise", "somafm", "commercial", "advert", "station id"})
    if (a.find(k) != std::string::npos) return false;
  return true;
}
