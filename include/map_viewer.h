#pragma once

#include "global.h"
#include "global.fieldmap.h"

// Overworld special: shows the current map's layout. Use "waitstate" after it.
void ShowCurrentMapViewer(void);

// Must be called from a main callback (not a task) while the screen is faded to black.
// Resets tasks and sprites, shows the given map's layout, object events and wild encounters,
// then fades out and switches to returnCallback.
void OpenMapViewer(u8 mapGroup, u8 mapNum, MainCallback returnCallback);
