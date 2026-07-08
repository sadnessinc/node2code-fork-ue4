// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.
// Back2Dead fork: Blueprint toolbar is simplified to two safety-first buttons.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"

class FN2CToolbarCommand : public TCommands<FN2CToolbarCommand>
{
public:
    FN2CToolbarCommand();

    virtual void RegisterCommands() override;

    /** Button 1: AI-friendly export from the current Blueprint. */
    TSharedPtr<FUICommandInfo> ExportCommand;

    /** Button 2: Load/apply an N2C_PATCH_V1 patch to the current Blueprint. */
    TSharedPtr<FUICommandInfo> ImportCommand;

    /** Level Editor button: load/apply an N2C_PROJECT_PATCH_V1 patch across multiple assets. */
    TSharedPtr<FUICommandInfo> ProjectImportCommand;

    /** Level Editor button: export all project Blueprints into one ZIP. */
    TSharedPtr<FUICommandInfo> ExportAllCommand;

    static const FName CommandName_Export;
    static const FName CommandName_Import;
    static const FName CommandName_ProjectImport;
    static const FName CommandName_ExportAll;
    static const FText CommandLabel_Export;
    static const FText CommandLabel_Import;
    static const FText CommandLabel_ProjectImport;
    static const FText CommandLabel_ExportAll;
    static const FText CommandTooltip_Export;
    static const FText CommandTooltip_Import;
    static const FText CommandTooltip_ProjectImport;
    static const FText CommandTooltip_ExportAll;
};
