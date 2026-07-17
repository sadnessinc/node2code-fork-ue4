// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class NodeToCode : ModuleRules
{
	public NodeToCode(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        
		// UE4.27 NiagaraNodeParameterMapSet/Get are NO_API editor-private classes.
		// Their headers can be included from NiagaraEditor/Private, but calling StaticClass()
		// or using FGraphNodeCreator<UNiagaraNodeParameterMapSet/Get> from this external
		// plugin produces LNK2019. Keep typed private graph creation disabled and create
		// these nodes only by runtime UClass lookup / reflection fallback.
		PublicDefinitions.Add("N2C_WITH_NIAGARA_PRIVATE_GRAPH_API=0");
        
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"Json",
				"UnrealEd",
				"BlueprintGraph",
				"Slate",
				"SlateCore",
				"Kismet",
				"GraphEditor",
				"ApplicationCore",
				"Projects",
				"EditorStyle",
				"ToolMenus",
				"ContentBrowser",
				"DesktopPlatform",
				"LevelEditor",
				"AssetRegistry",
				"Niagara",
				"NiagaraCore",
				"NiagaraEditor",
				"UMG",
				"UMGEditor",
				"AnimGraph",
				"AIModule"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"DeveloperSettings",
				"Settings"
			}
		);

		// Optional Blueprint Assist integration.
		// The code is compiled only when the BlueprintAssist source module is present.
		// Supported locations:
		// - <Project>/Plugins/.../BlueprintAssist
		// - <Engine>/Plugins/Editor/.../BlueprintAssist
		// - <Engine>/Plugins/.../BlueprintAssist
		// Runtime code still checks that the plugin is enabled before calling it.
		bool bHasBlueprintAssistSource = false;
		var BlueprintAssistSearchRoots = new System.Collections.Generic.List<string>();

		if (Target.ProjectFile != null)
		{
			BlueprintAssistSearchRoots.Add(Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins"));
		}

		if (!string.IsNullOrEmpty(Target.RelativeEnginePath))
		{
			string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
			BlueprintAssistSearchRoots.Add(Path.Combine(EngineDir, "Plugins", "Editor"));
			BlueprintAssistSearchRoots.Add(Path.Combine(EngineDir, "Plugins"));
		}

		foreach (string PluginsRoot in BlueprintAssistSearchRoots)
		{
			if (string.IsNullOrEmpty(PluginsRoot) || !Directory.Exists(PluginsRoot))
			{
				continue;
			}

			string DirectHeader = Path.Combine(PluginsRoot, "BlueprintAssist", "Source", "BlueprintAssist", "Public", "BlueprintAssistModule.h");
			if (File.Exists(DirectHeader))
			{
				bHasBlueprintAssistSource = true;
				break;
			}

			foreach (string Header in Directory.GetFiles(PluginsRoot, "BlueprintAssistModule.h", SearchOption.AllDirectories))
			{
				string Normalized = Header.Replace('\\', '/');
				if (Normalized.EndsWith("/Source/BlueprintAssist/Public/BlueprintAssistModule.h"))
				{
					bHasBlueprintAssistSource = true;
					break;
				}
			}

			if (bHasBlueprintAssistSource)
			{
				break;
			}
		}

		if (bHasBlueprintAssistSource)
		{
			PrivateDependencyModuleNames.Add("BlueprintAssist");
			PublicDefinitions.Add("N2C_WITH_BLUEPRINT_ASSIST=1");
		}
		else
		{
			PublicDefinitions.Add("N2C_WITH_BLUEPRINT_ASSIST=0");
		}
	}
}
