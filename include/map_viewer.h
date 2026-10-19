#pragma once

#include "global.h"
#include "global.fieldmap.h"

// Overworld special: shows the current map's layout. Use "waitstate" after it.
void ShowCurrentMapViewer(void);

// Must be called from a main callback (not a task) while the screen is faded to black.
// Resets tasks and sprites, shows the given layout, then fades out and switches to returnCallback.
void OpenMapViewer(const struct MapLayout *layout, MainCallback returnCallback);
