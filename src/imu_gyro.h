#pragma once
#include "config.h"

bool imuBegin();
// Returns true and writes a candidate orientation when gravity is clear.
bool imuReadOrientation(Orientation& out);
