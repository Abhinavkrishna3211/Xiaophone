#pragma once
#include <string>
void coverBegin();                                                       // starts the cover task
void coverRequest(const std::string& artist, const std::string& title);  // latest request wins
