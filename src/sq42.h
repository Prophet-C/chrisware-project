#pragma once
#include "common.h"

void ResolveSq42Api(const Section& text, const Section& rdata);
void ProcessSq42();
bool RunConsoleNow(const char* cmd);
bool SetCVarNow(const char* name, float value);
