// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.
// Back2Dead/node2code fork: direct-codegen settings removed.

#include "Core/N2CSettings.h"
#include "Utils/N2CLogger.h"

#define LOCTEXT_NAMESPACE "NodeToCode"

UN2CSettings::UN2CSettings()
{
    FN2CLogger::Get().Log(TEXT("N2CSettings legacy compatibility object initialized"), EN2CLogSeverity::Debug);
}

FText UN2CSettings::GetSectionText() const
{
    return LOCTEXT("SettingsSectionLegacy", "Node2Code Legacy");
}

#if WITH_EDITOR
void UN2CSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    const FProperty* Property = PropertyChangedEvent.Property;
    if (Property && Property->GetFName() == GET_MEMBER_NAME_CHECKED(UN2CSettings, MinSeverity))
    {
        FN2CLogger::Get().SetMinSeverity(MinSeverity);
    }
}
#endif

#undef LOCTEXT_NAMESPACE
