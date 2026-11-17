#pragma once
#include <string>

// Parse an ICY StreamTitle "Artist - Title" into clean parts.
// Returns false for station IDs, ads and anything without an artist.
bool metaParse(const std::string& icy, std::string& artist, std::string& title);
std::string metaLower(std::string s);
