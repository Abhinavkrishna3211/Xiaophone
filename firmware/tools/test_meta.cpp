// Host check for title parsing: g++ -std=c++17 -Isrc tools/test_meta.cpp src/meta.cpp -o t && ./t
#include <cassert>
#include <cstdio>
#include "meta.h"

int main() {
  std::string a, t;
  assert(metaParse("Lucid Express - Promise Me (radio edit)", a, t) && a == "Lucid Express" && t == "Promise Me");
  assert(metaParse("Daft Punk feat. Pharrell Williams - Get Lucky [Official Video]", a, t) && a == "Daft Punk" && t == "Get Lucky");
  assert(metaParse("A.R. Rahman - Jai Ho - Live", a, t) && a == "A.R. Rahman" && t == "Jai Ho - Live");
  assert(metaParse("Arijit Singh  -  Tum Hi Ho ft. Someone", a, t) && a == "Arijit Singh" && t == "Tum Hi Ho");
  assert(!metaParse("Radio Paradise - commercial-free", a, t));
  assert(!metaParse("SomaFM", a, t));
  assert(!metaParse(" - Untitled", a, t));
  puts("meta ok");
}
