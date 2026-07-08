# UE4.27 Port Guide

This document records every API change required to port NodeToCode from UE 5.3+ to UE 4.27.2. Use it as a reference when porting other UE5 plugins or when extending this plugin with new features.

## Summary of Changes

52 files changed, 86 insertions, 740 deletions. The majority of deletions were UE5-only UMG widget files and Content/UI assets that have no equivalent in 4.27.

## .uplugin

| UE5 | UE4.27 | Notes |
|-----|--------|-------|
| `PlatformAllowList` | `WhitelistPlatforms` | Renamed in UE5 |

## Module Dependencies (Build.cs)

**Removed (UE5-only):**
- `UMG` - UMG widget system used for Editor Utility Widgets
- `Blutility` - Blueprint Utility / Editor Utility Widget support
- `UMGEditor` - UMG editor integration

**Added (UE4.27 equivalents):**
- `EditorStyle` - Provides `FEditorStyle` (replaces UE5's `FAppStyle`)
- `ToolMenus` - Exists in UE4.27 (not UE5-only as initially assumed)

## Deleted Files (No UE4.27 Equivalent)

These files relied on UE5's Editor Utility Widget / UMG system which does not exist in 4.27:

| File | Purpose |
|------|---------|
| `N2CCodeEditorWidget.h/.cpp` | UWidget-based wrapper for the code editor |
| `N2CCodeEditorWidgetFactory.h/.cpp` | Factory for registering the UMG widget |
| `N2CWidgetContainer.h/.cpp` | Persistent UObject container for editor widgets |
| `Content/UI/*` (27 .uasset files) | Editor Utility Widget blueprints, textures, theming |

The editor window (`SN2CEditorWindow`) was replaced with a placeholder `STextBlock` pending a full Slate UI rebuild.

## API Replacements

### Style System
| UE5 | UE4.27 | File |
|-----|--------|------|
| `#include "Styling/AppStyle.h"` | `#include "EditorStyleSet.h"` | N2CToolbarCommand.cpp |
| `FAppStyle::GetAppStyleSetName()` | `FEditorStyle::GetStyleSetName()` | N2CToolbarCommand.cpp |

### Asset Editor Events
| UE5 | UE4.27 | File |
|-----|--------|------|
| `UAssetEditorSubsystem` (via `GEditor->GetEditorSubsystem<>()`) | Same - `UAssetEditorSubsystem` exists in 4.27 | N2CEditorIntegration.cpp |
| `FAssetEditorManager::OnAssetOpenedInEditor()` | Deprecated in 4.27, use `UAssetEditorSubsystem` instead | N2CEditorIntegration.cpp |

**Important:** `FAssetEditorManager` exists in 4.27 but its `OnAssetOpenedInEditor` delegate does not reliably fire for Blueprint Editors. `UAssetEditorSubsystem` (available from `GEditor->GetEditorSubsystem<>()`) works correctly in both 4.27 and UE5.

### HTTP Requests
| UE5 | UE4.27 | File |
|-----|--------|------|
| `TSharedRef<IHttpRequest>` (no template param) | `TSharedRef<IHttpRequest, ESPMode::ThreadSafe>` | N2CHttpHandlerBase.cpp |

In UE5, the `ESPMode::ThreadSafe` template parameter was made the default. In 4.27, it must be specified explicitly. `FHttpModule::Get().CreateRequest()` returns `TSharedRef<IHttpRequest, ESPMode::ThreadSafe>` in both versions.

### Syntax Highlighter
| UE5 | UE4.27 | File |
|-----|--------|------|
| `ISyntaxTokenizer` | `FSyntaxTokenizer` | N2CRichTextSyntaxHighlighter.h/.cpp |
| `ISyntaxTokenizer::FTokenizedLine` | `FSyntaxTokenizer::FTokenizedLine` | Same |
| `TSharedPtr<ISyntaxTokenizer>` | `TSharedPtr<FSyntaxTokenizer>` | Same |

The interface was renamed from `ISyntaxTokenizer` to `FSyntaxTokenizer` between UE4 and UE5.

### Blueprint Pin Types
| UE5 | UE4.27 | File |
|-----|--------|------|
| `UEdGraphSchema_K2::PC_Double` | Does not exist | N2CNodeTranslator.cpp |
| `UEdGraphSchema_K2::PC_Real` | Does not exist | N2CNodeTranslator.cpp |

UE4.27 only has `PC_Float` for floating-point pins. Double-precision support was added in UE5.

### K2Node Types
| UE5 | UE4.27 | File |
|-----|--------|------|
| `UK2Node_ExternalGraphInterface` | Does not exist | N2CNodeTypeRegistry.h/.cpp |
| `UK2Node_PromotableOperator` | Does not exist | N2CNodeTypeRegistry.h/.cpp |

These node types were added in UE5. The include lines and `IsA<>()` checks were removed.

### JSON Serialization
| UE5 | UE4.27 | File |
|-----|--------|------|
| `TCondensedJsonPrintPolicy` (auto-included) | Needs `#include "Policies/CondensedJsonPrintPolicy.h"` | N2CSerializer.cpp |

The class exists in both versions but is not transitively included in 4.27.

### UScriptStruct Path
| UE5 | UE4.27 | File |
|-----|--------|------|
| `Struct->GetStructPathName().ToString()` | `Struct->GetPathName()` | N2CStructProcessor.cpp |

`GetStructPathName()` was added in UE5. `GetPathName()` (inherited from UObject) works in both.

### TScriptInterface
| UE5 | UE4.27 | File |
|-----|--------|------|
| `ServiceInterface.GetInterface()->Method()` | `ServiceInterface->Method()` | N2CLLMModule.cpp |

In UE4.27, `TScriptInterface::GetInterface()` returns `void*`. Use `operator->` instead, which performs the cast automatically.

### JSON Number Fields
| UE5 | UE4.27 | File |
|-----|--------|------|
| `TryGetNumberField(Key, float&)` | `TryGetNumberField(Key, double&)` | N2CLMStudioResponseParser.cpp |

UE4.27's `FJsonObject::TryGetNumberField` expects `double&`, not `float&`.

### Regex
| UE5 | UE4.27 | File |
|-----|--------|------|
| `FRegexPattern`/`FRegexMatcher` (auto-included) | Needs `#include "Internationalization/Regex.h"` | N2CNodeTranslator.cpp |

Classes exist in both versions but require an explicit include in 4.27.

### Missing Includes
| Include | Needed For | File |
|---------|-----------|------|
| `Misc/FileHelper.h` | `FFileHelper::LoadFileToString` | N2CSettings.h |
| `Widgets/Layout/SGridPanel.h` | `SGridPanel` widget | SN2CCodeEditor.cpp |
| `Widgets/Text/STextBlock.h` | `STextBlock` widget | N2CEditorWindow.cpp |

### SMultiLineEditableText (Stubbed Methods)

These methods exist in UE5's `SMultiLineEditableText` but not in 4.27:

| Method | Workaround | File |
|--------|-----------|------|
| `DeleteSelectedText()` | `InsertTextAtCursor(FString())` | SN2CCodeEditor.cpp |
| `GetCursorLocation()` | Return `FTextLocation(0,0)` | SN2CCodeEditor.cpp |
| `SelectText()` | No-op stub | SN2CCodeEditor.cpp |
| `GetSelection()` | Return 0,0 | SN2CCodeEditor.cpp |

These are in the code editor widget which is marked for a full Slate rebuild.

### Removed UE5-Only Features

| Feature | Reason | File |
|---------|--------|------|
| `UEditorPerformanceSettings::bThrottleCPUWhenNotForeground` | Class doesn't exist in 4.27 | NodeToCode.cpp |
| `UToolMenus::UnRegisterStartupCallback/UnregisterOwner` | Not used in our toolbar approach | NodeToCode.cpp |
| `FN2CCodeEditorWidgetFactory::Register/Unregister` | UMG widget system removed | NodeToCode.cpp |
| `UN2CWidgetContainer::Reset()` | Widget container removed | NodeToCode.cpp |
| `TimeoutConfig->TryUpdateDefaultConfigFile()` | Use `SaveConfig()` instead | NodeToCode.cpp |
| `FConfigCacheIni::LoadGlobalIniFile()` | Not needed with `SaveConfig()` | NodeToCode.cpp |

### Dynamic Delegate Binding from Slate Widgets
| UE5 | UE4.27 | File |
|-----|--------|------|
| `DECLARE_DYNAMIC_MULTICAST_DELEGATE` + AddDynamic on UObject | Same limitation exists | N2CLLMTypes.h, N2CLLMModule.h |

In both UE4.27 and UE5, `DECLARE_DYNAMIC_MULTICAST_DELEGATE` requires a UObject with a UFUNCTION to bind via `AddDynamic`. Slate widgets (`SCompoundWidget`) are not UObjects. The solution is to add parallel native C++ delegates (`DECLARE_MULTICAST_DELEGATE`) that support `AddRaw()` binding to any class, and broadcast both delegate types. This was done for `OnTranslationRequestSent` and `OnTranslationResponseReceived` in `UN2CLLMModule`.

### UDeveloperSettings Container Name
| UE5 | UE4.27 | File |
|-----|--------|------|
| `GetContainerName()` defaults based on Config name | Same behavior | N2CSettings.h |

`UDeveloperSettings::GetContainerName()` returns `"Editor"` only if `ClassConfigName` is `"EditorSettings"` or `"EditorPerProjectUserSettings"`. Custom config names (like `Config=NodeToCode`) default to `"Project"`, so settings appear in **Project Settings > Plugins**, not Editor Preferences.

## Completed Work

- **Slate UI Rebuild:** `SN2CEditorWindow` fully rebuilt with `SWidgetSwitcher` (welcome/loading/results/error panels), dual `SN2CCodeEditor` widgets with syntax highlighting, `SComboBox` graph selector for multi-graph Blueprints, copy-to-clipboard buttons, and Open Folder button. Uses native delegate binding to `UN2CLLMModule`.
- **Anthropic Model Updates:** Updated to Claude Opus 4.6, Sonnet 4.6, Haiku 4.5 with correct API model IDs. Note: 4.6 models use IDs without date suffix (e.g., `claude-sonnet-4-6` not `claude-sonnet-4-6-20250514`).
- **Max Tokens:** Increased Anthropic `max_tokens` from 8192 to 16384 to prevent truncated responses on larger Blueprint graphs.

## Remaining Work

- **Code Editor Widget Stubs:** `SN2CCodeEditor` has stubbed-out cursor/selection methods. These should be implemented if interactive editing is needed.
- **Toolbar Icon:** The toolbar button icon (`Resources/button_icon.png`) is registered at 40x40/20x20 but may not render on all toolbar configurations. Investigate if the icon is showing correctly.
