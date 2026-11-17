#pragma once
void netBegin();
void netLoop();   // call from loop(): nudges reconnect if auto-reconnect stalls
bool netUp();
