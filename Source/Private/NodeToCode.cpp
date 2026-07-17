// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "NodeToCode.h"

#include "Models/N2CLogging.h"
#include "Core/N2CEditorIntegration.h"
#include "Core/N2CSettings.h"
#include "Models/N2CStyle.h"
#if WITH_EDITOR
#include "ISettingsModule.h"
#endif
DEFINE_LOG_CATEGORY(LogNodeToCode);

#define LOCTEXT_NAMESPACE "FNodeToCodeModule"

void FNodeToCodeModule::StartupModule()
{
    // Initialize logging
    FN2CLogger::Get().Log(TEXT("NodeToCode plugin starting up"), EN2CLogSeverity::Info);

    // Apply configured log severity from settings
    const UN2CSettings* Settings = GetDefault<UN2CSettings>();
    if (Settings)
    {
        FN2CLogger::Get().SetMinSeverity(Settings->MinSeverity);
        FN2CLogger::Get().Log(TEXT("Applied log severity from settings"), EN2CLogSeverity::Debug);
    }

    
    // Initialize style system
    N2CStyle::Initialize();
    FN2CLogger::Get().Log(TEXT("Node to Code style initialized"), EN2CLogSeverity::Debug);

#if WITH_EDITOR
    RegisterN2CEditorPreferences();
#endif

    // Initialize editor integration
    FN2CEditorIntegration::Get().Initialize();
    FN2CLogger::Get().Log(TEXT("Editor integration initialized"), EN2CLogSeverity::Debug);

}

void FNodeToCodeModule::ShutdownModule()
{
#if WITH_EDITOR
    UnregisterN2CEditorPreferences();
#endif

    // Shutdown editor integration
    FN2CEditorIntegration::Get().Shutdown();

    // Shutdown style system
    N2CStyle::Shutdown();

    FN2CLogger::Get().Log(TEXT("NodeToCode plugin shutting down"), EN2CLogSeverity::Info);
}


#if WITH_EDITOR
void FNodeToCodeModule::RegisterN2CEditorPreferences()
{
    if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
    {
        SettingsModule->RegisterSettings(
            TEXT("Editor"),
            TEXT("Plugins"),
            TEXT("Node2Code"),
            LOCTEXT("Node2CodeEditorSettingsName", "Node2Code"),
            LOCTEXT("Node2CodeEditorSettingsDescription", "Configure Node2Code editor integration and import/export helper features."),
            GetMutableDefault<UN2CEditorPreferences>()
        );
    }
}

void FNodeToCodeModule::UnregisterN2CEditorPreferences()
{
    if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
    {
        SettingsModule->UnregisterSettings(TEXT("Editor"), TEXT("Plugins"), TEXT("Node2Code"));
    }
}
#endif

#undef LOCTEXT_NAMESPACE
    
IMPLEMENT_MODULE(FNodeToCodeModule, NodeToCode)
