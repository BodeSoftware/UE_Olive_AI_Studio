# Debugger Agent Memory

## Build System
- UBT path: `"C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe"`
- Build command: `UBT.exe UE_Olive_AI_ToolkitEditor Win64 Development "-Project=B:/Unreal Projects/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject" -WaitMutex`
- Incremental builds take ~8 seconds

## Fixed Bugs & Root Causes

### Component Delegate Events (RC1) - Fixed 2026-02-26
- **File:** `OliveNodeFactory.cpp` `CreateEventNode`
- **Issue:** Events like OnComponentBeginOverlap/OnComponentEndOverlap/OnComponentHit are component delegate events, not native actor events. They need UK2Node_ComponentBoundEvent, not UK2Node_Event.
- **Fix:** Added SCS scan in CreateEventNode to detect multicast delegate properties and create ComponentBoundEvent nodes via InitializeComponentBoundEventParams.
- **Key UE API:** `UK2Node_ComponentBoundEvent::InitializeComponentBoundEventParams(FObjectProperty*, FMulticastDelegateProperty*)`

### Component @ref Expansion (RC2/RC3) - Fixed 2026-02-26
- **Files:** `OliveIRSchema.cpp`, `OliveBlueprintPlanResolver.cpp/.h`
- **Issue:** AI writes `@ComponentName` or `@ParamName.Pin` in plan inputs, but schema validator rejected these as malformed/unknown @refs before resolver could process them.
- **Fix:** (1) Relaxed schema validator to allow dotless @refs and unknown step_id @refs (logged instead of errored). (2) Added `ExpandComponentRefs()` in resolver that synthesizes get_var steps for component names and function parameter names.
- **Pattern:** Schema validation must be lenient about @refs that the resolver will auto-expand.

### Blueprint-Defined Function Resolution (RC4) - Fixed 2026-02-26
- **File:** `OliveFunctionResolver.cpp` `BroadSearch`
- **Issue:** BroadSearch found functions like Pickup/Fire/Drop on SKEL_BP_Gun_C but gave confidence 40 (unrelated gameplay class) instead of 95 (own Blueprint class). Threshold of 60 rejected them.
- **Fix:** Added check for Blueprint->GeneratedClass and Blueprint->SkeletonGeneratedClass; if match, confidence = 95.

### Exec Wiring Conflicts (RC5) - Fixed 2026-02-26
- **Files:** `OlivePlanValidator.cpp/.h`, `OliveBlueprintToolHandlers.cpp`
- **Issue:** AI writes both exec_after and exec_outputs on same step, double-claiming exec output pin.
- **Fix:** Added `AutoFixExecConflicts()` that removes redundant exec_after when target step uses exec_outputs. Called before validator in both preview and apply handlers.

## UE 5.5 API Notes
- `UK2Node_ComponentBoundEvent` lives in `K2Node_ComponentBoundEvent.h` (BlueprintGraph module)
- Component delegate properties (OnComponentBeginOverlap etc.) are `FMulticastDelegateProperty` on `UPrimitiveComponent`
- `Blueprint->SkeletonGeneratedClass` is the SKEL_ class where BP-defined functions live before full compilation
- `FindFProperty<FObjectProperty>(Class, Name)` finds component variable properties on generated class
- Schema validation in `OliveIRSchema.cpp` runs BEFORE plan resolution in the tool handler pipeline
