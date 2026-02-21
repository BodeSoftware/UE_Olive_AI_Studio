// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FOlivePCGAvailability
 *
 * Checks whether the PCG plugin is loaded and available.
 * All PCG tool registration and initialization is guarded by this check.
 */
class OLIVEAIEDITOR_API FOlivePCGAvailability
{
public:
	/** Returns true if the PCG module is loaded and available */
	static bool IsPCGAvailable();
};
