// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.
// Back2Dead fork: Blueprint toolbar is simplified to two safety-first buttons.

#include "Core/N2CToolbarCommand.h"

#include "EditorStyleSet.h"
#include "Utils/N2CLogger.h"

#define LOCTEXT_NAMESPACE "NodeToCode"

const FName FN2CToolbarCommand::CommandName_Export = TEXT("NodeToCode_Back2Dead_Export");
const FName FN2CToolbarCommand::CommandName_Import = TEXT("NodeToCode_Back2Dead_Import");
const FName FN2CToolbarCommand::CommandName_ProjectImport = TEXT("NodeToCode_Back2Dead_ProjectImport");
const FName FN2CToolbarCommand::CommandName_ExportAll = TEXT("NodeToCode_Back2Dead_ExportAll");
const FText FN2CToolbarCommand::CommandLabel_Export = NSLOCTEXT("NodeToCode", "Back2DeadExport", "Export N2C");
const FText FN2CToolbarCommand::CommandLabel_Import = NSLOCTEXT("NodeToCode", "Back2DeadImport", "Import N2C");
const FText FN2CToolbarCommand::CommandLabel_ProjectImport = NSLOCTEXT("NodeToCode", "Back2DeadProjectImport", "Import N2C");
const FText FN2CToolbarCommand::CommandLabel_ExportAll = NSLOCTEXT("NodeToCode", "Back2DeadExportProject", "Export Project");
const FText FN2CToolbarCommand::CommandTooltip_Export = NSLOCTEXT("NodeToCode", "Back2DeadExportTooltip", "Export current Blueprint as AI-friendly N2C_AI_EXPORT_V2 JSON and ZIP.");
const FText FN2CToolbarCommand::CommandTooltip_Import = NSLOCTEXT("NodeToCode", "Back2DeadImportTooltip", "Import an N2C_PATCH_V1 JSON patch safely. Dry-run, confirmation, backup, apply, compile.");
const FText FN2CToolbarCommand::CommandTooltip_ProjectImport = NSLOCTEXT("NodeToCode", "Back2DeadProjectImportTooltip", "Import an N2C_PROJECT_PATCH_V1 JSON patch from the main editor toolbar. Supports multiple Blueprint assets in one file.");
const FText FN2CToolbarCommand::CommandTooltip_ExportAll = NSLOCTEXT("NodeToCode", "Back2DeadExportProjectTooltip", "Export project assets into one N2C ZIP archive. Choose Blueprint, Niagara System, Enum and Struct in the export window.");

FN2CToolbarCommand::FN2CToolbarCommand()
    : TCommands<FN2CToolbarCommand>(
        TEXT("NodeToCode"),
        NSLOCTEXT("NodeToCode", "NodeToCode", "Node to Code"),
        NAME_None,
        FEditorStyle::GetStyleSetName()
    )
{
}

void FN2CToolbarCommand::RegisterCommands()
{
    FN2CLogger::Get().Log(TEXT("Registering Back2Dead N2C toolbar commands"), EN2CLogSeverity::Debug);

    UI_COMMAND(
        ExportCommand,
        "Export N2C",
        "Export current Blueprint as AI-friendly N2C_AI_EXPORT_V2 JSON and ZIP",
        EUserInterfaceActionType::Button,
        FInputChord()
    );

    UI_COMMAND(
        ImportCommand,
        "Import N2C",
        "Load/apply N2C_PATCH_V1 Blueprint patch safely",
        EUserInterfaceActionType::Button,
        FInputChord()
    );

    UI_COMMAND(
        ProjectImportCommand,
        "Import N2C",
        "Load/apply N2C_PROJECT_PATCH_V1 project patch safely",
        EUserInterfaceActionType::Button,
        FInputChord()
    );

    UI_COMMAND(
        ExportAllCommand,
        "Export Project",
        "Export project assets into one N2C ZIP archive. Choose Blueprint, Niagara System, Enum and Struct in the export window",
        EUserInterfaceActionType::Button,
        FInputChord()
    );
}

#undef LOCTEXT_NAMESPACE
