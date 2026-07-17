// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * @class FNodeToCodeModule
 * @brief Main module implementation for the Node to Code plugin
 *
 * Provides Node2Code Blueprint/Niagara JSON export/import editor integration.
 */
class FNodeToCodeModule : public IModuleInterface
{
public:
    /** IModuleInterface implementation */
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
#if WITH_EDITOR
    /** Register Node2Code settings under Editor Preferences -> Plugins. */
    void RegisterN2CEditorPreferences();

    /** Unregister Node2Code editor preferences. */
    void UnregisterN2CEditorPreferences();
#endif
};