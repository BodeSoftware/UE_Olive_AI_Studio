// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FOliveNiagaraAvailability
 *
 * Checks whether the Niagara editor plugin is loaded and available.
 * All Niagara tool registration and initialization is guarded by this check.
 */
class OLIVEAIEDITOR_API FOliveNiagaraAvailability
{
public:
	/** Returns true if the NiagaraEditor module is loaded and available */
	static bool IsNiagaraAvailable();
};
