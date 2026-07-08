# NodeToCode - Project Overview

NodeToCode is an Unreal Engine editor plugin that translates Blueprint visual scripting graphs into structured code (C++, Python, JavaScript, C#, Swift, or pseudocode) using LLM providers. This fork targets **UE 4.27.2**, ported from the original UE 5.3+ codebase.

## Architecture

The plugin follows a pipeline architecture: **Collect -> Translate -> Serialize -> Send -> Parse -> Display**.

```
Blueprint Editor
    |
    v
FN2CNodeCollector        Extracts UK2Node objects from the focused graph
    |
    v
FN2CNodeTranslator       Converts nodes into FN2CBlueprint data structures
    |                     Uses FN2CNodeProcessorFactory (12 specialized processors)
    v
FN2CSerializer            Serializes FN2CBlueprint to compact JSON
    |
    v
UN2CLLMModule             Orchestrates the LLM request/response cycle
    |
    v
UN2CBaseLLMService        Provider-specific formatting & HTTP dispatch
    |
    v
UN2CResponseParserBase    Parses LLM response into FN2CTranslationResponse
    |
    v
SN2CEditorWindow          Displays results in full Slate UI (code editors, loading states)
```

## Source Layout

```
Source/
  NodeToCode.Build.cs           Module build rules & dependencies
  Public/                       Headers (public API surface)
    NodeToCode.h                Module declaration
    Core/
      N2CEditorIntegration.h    Blueprint Editor toolbar & event hooks
      N2CEditorWindow.h         Dockable editor tab (SN2CEditorWindow)
      N2CNodeCollector.h        Extracts nodes from Blueprint graphs
      N2CNodeTranslator.h       Converts UK2Nodes into N2C data model
      N2CSerializer.h           JSON serialization of FN2CBlueprint
      N2CSettings.h             Plugin settings (UDeveloperSettings)
      N2CToolbarCommand.h       Toolbar button command definitions
      N2CUserSecrets.h          API key storage (outside version control)
    LLM/
      N2CLLMModule.h            Main LLM orchestrator
      N2CLLMTypes.h             Enums, delegates, FN2CLLMConfig
      N2CLLMModels.h            Model name constants per provider
      N2CHttpHandlerBase.h      HTTP request helper
      N2CResponseParserBase.h   Base class for parsing LLM output
      N2CSystemPromptManager.h  Template-based system prompts
      N2COllamaConfig.h         Ollama-specific configuration
      N2CLLMPayloadBuilder.h    Constructs provider request bodies
      N2CLLMProviderRegistry.h  Provider factory registry (singleton)
      N2CLLMService.h           IN2CLLMService interface
      N2CBaseLLMService.h       Abstract base for all providers
      Providers/                Per-provider service & parser classes
        N2CAnthropicService.h
        N2COpenAIService.h
        N2CGeminiService.h
        N2CDeepSeekService.h
        N2COllamaService.h
        N2CLMStudioService.h
    Code Editor/
      Models/
        N2CCodeLanguage.h       EN2CCodeLanguage enum (Cpp, Python, etc.)
        N2CCodeEditorStyle.h    Style/theme system for the code editor
      Syntax/
        N2CSyntaxDefinition.h   Base syntax definition (keywords, operators)
        N2CSyntaxDefinitionFactory.h
        N2CRichTextSyntaxHighlighter.h   Slate rich text syntax coloring
        N2CWhiteSpaceRun.h      Whitespace token for proper indentation
      Widgets/
        SN2CCodeEditor.h        Slate code editor (SMultiLineEditableText)
    Models/
      N2CBlueprint.h            FN2CBlueprint, FN2CGraph, FN2CFlows
      N2CNode.h                 FN2CNodeDefinition, EN2CNodeType
      N2CPin.h                  FN2CPinDefinition, EN2CPinType
      N2CTranslation.h          FN2CTranslationResponse, FN2CGeneratedCode
      N2CLogging.h              EN2CLogSeverity
      N2CStyle.h                Plugin toolbar icon style
    Utils/
      N2CLogger.h               Centralized logging
      N2CNodeTypeRegistry.h     Maps UK2Node classes to EN2CNodeType
      N2CPinTypeCompatibility.h Pin connection validation
      Processors/               12 node-type-specific processors
        N2CNodeProcessorFactory.h
        N2CFunctionCallProcessor.h
        N2CVariableProcessor.h
        N2CEventProcessor.h
        N2CFlowControlProcessor.h
        ...
      Validators/
        N2CBlueprintValidator.h
        N2CNodeValidator.h
        N2CPinValidator.h
  Private/                      Implementation files (mirrors Public/)
```

## LLM Providers

Six providers are supported via a plugin/registry pattern:

| Provider | Class | Default Endpoint |
|----------|-------|-----------------|
| Anthropic (Claude) | UN2CAnthropicService | api.anthropic.com/v1/messages |
| OpenAI (GPT) | UN2COpenAIService | api.openai.com/v1/chat/completions |
| Google Gemini | UN2CGeminiService | generativelanguage.googleapis.com |
| DeepSeek | UN2CDeepSeekService | api.deepseek.com |
| Ollama (local) | UN2COllamaService | localhost:11434/api/chat |
| LM Studio (local) | UN2CLMStudioService | localhost:1234 |

Each provider implements `IN2CLLMService` and overrides:
- `FormatRequestPayload()` - Provider-specific JSON body
- `CreateResponseParser()` - Provider-specific response parsing
- `GetProviderHeaders()` - Auth headers

## Key Data Flow

1. User clicks "Node to Code" in Blueprint Editor toolbar
2. `FN2CEditorIntegration::ExecuteCollectNodesForEditor()` is called
3. `FN2CNodeCollector::CollectNodesFromGraph()` gathers all UK2Nodes from the focused graph
4. `FN2CNodeTranslator::GenerateN2CStruct()` converts them into `FN2CBlueprint`
5. `FN2CSerializer::ToJson()` produces compact JSON (60-90% fewer tokens than raw Blueprint text)
6. `UN2CLLMModule::ProcessN2CJson()` sends the JSON to the configured LLM provider
7. The provider's response parser extracts `FN2CTranslationResponse` with generated code
8. Results are displayed in the editor window and optionally saved to disk

## Module Dependencies (UE 4.27.2)

**Public:** Core, CoreUObject, Engine, InputCore, Json, UnrealEd, BlueprintGraph, Slate, SlateCore, Kismet, GraphEditor, HTTP, ApplicationCore, Projects, EditorStyle, ToolMenus

**Private:** DeveloperSettings
