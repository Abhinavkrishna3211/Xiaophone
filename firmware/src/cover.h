#pragma once
#include <string>
void coverBegin();                                                       // starts the cover task
void coverRequest(const std::string& artist, const std::string& title,  // latest request wins;
                  const std::string& imageUrl = "");                   // with a URL: skip the lookup
void coverFromJpeg(const uint8_t* jpg, size_t len,                       // a ready JPEG (USB): no network
                   const std::string& artist, const std::string& title);
