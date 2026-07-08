# Development Setup Guide

This guide explains how to set up the NodeToCode UE4.27 port for development, including how to link the plugin source to a UE4 project for live iteration.

## Prerequisites

- **Unreal Engine 4.27.2** (launcher or source build)
- **Visual Studio 2019** (Community or higher) with C++ game development workload
- **A UE4.27 project** to host the plugin during development
- **Git** for version control

## Repository Structure

```
C:\NodeToCodeUE4\                  <- Git repository root
  .claude/
    worktrees/
      ue4-port/                    <- Active worktree (or use main directly)
        NodeToCode.uplugin
        Source/
        Resources/
        Content/
        docs/
  ...
```

The plugin source lives in this repository. To test it in UE4, you link it into a UE4 project's `Plugins/` directory.

## Setting Up the Junction (Windows)

A **junction** (directory symlink) allows UE4 to see the plugin source as if it lives inside the project, while the actual files remain in the git repository. Edits in either location affect the same files.

### Step 1: Create the Plugins directory

```cmd
mkdir "F:\UE4\Projects\YourProject\Plugins"
```

### Step 2: Create the junction

```cmd
mklink /J "F:\UE4\Projects\YourProject\Plugins\NodeToCode" "C:\NodeToCodeUE4\.claude\worktrees\ue4-port"
```

Replace paths with your actual locations:
- First path: where UE4 will look for the plugin
- Second path: where the plugin source actually lives (git worktree or repo root)

### Step 3: Verify

```cmd
dir "F:\UE4\Projects\YourProject\Plugins\NodeToCode\NodeToCode.uplugin"
```

You should see the `.uplugin` file listed.

### Notes on Junctions vs Symlinks

| Feature | Junction (`mklink /J`) | Symlink (`mklink /D`) |
|---------|----------------------|---------------------|
| Admin required | No | Yes |
| Cross-drive | Yes (local only) | Yes (including network) |
| UE4 compatible | Yes | Yes |
| Git compatible | Yes | Yes |

**Use junctions** (`mklink /J`) for local development. They don't require admin privileges and work identically to symlinks for this use case.

### Junction Fragility

Junctions can break if:
- The target directory is deleted or moved (e.g., git worktree cleanup)
- The git worktree is pruned

If the junction breaks, UE4 will show "Missing NodeToCode Plugin" on launch. Recreate it:

```cmd
rmdir "F:\UE4\Projects\YourProject\Plugins\NodeToCode"
mklink /J "F:\UE4\Projects\YourProject\Plugins\NodeToCode" "C:\path\to\plugin\source"
```

## Generating Visual Studio Project Files

UE4.27 does not always register the right-click "Generate Visual Studio project files" context menu handler. Use the command line instead:

```cmd
"F:\UE4\UE_4.27\Engine\Binaries\DotNET\UnrealBuildTool.exe" ^
  -projectfiles ^
  -project="F:\UE4\Projects\YourProject\YourProject.uproject" ^
  -game -engine -progress
```

Run this after:
- Initial setup
- Adding/removing source files
- Changing `Build.cs` dependencies

## Building from Command Line

```cmd
"F:\UE4\UE_4.27\Engine\Build\BatchFiles\Build.bat" ^
  YourProjectEditor Win64 Development ^
  -Project="F:\UE4\Projects\YourProject\YourProject.uproject" ^
  -WaitMutex -FromMsBuild
```

**Important:** Close the UE4 editor before building from the command line. If Live Coding is active, the build will fail with:

```
ERROR: Unable to start regular build while Live Coding is active.
```

## Adding the Plugin to a UE4 Project

If the project doesn't already reference NodeToCode, add it to the `.uproject` file's `Plugins` array:

```json
{
    "Name": "NodeToCode",
    "Enabled": true
}
```

## For Claude Code Agents

When starting a new session that needs to build or test this plugin:

1. **Check the junction exists:**
   ```bash
   ls "F:/UE4/Projects/Wiz98/Plugins/NodeToCode/NodeToCode.uplugin"
   ```

2. **If broken, recreate it (replace `<worktree>` with actual worktree directory name):**
   ```bash
   cmd //c "mklink /J F:\UE4\Projects\Wiz98\Plugins\NodeToCode C:\NodeToCodeUE4\.claude\worktrees\<worktree>"
   ```

3. **Build command:**
   ```bash
   "F:/UE4/UE_4.27/Engine/Build/BatchFiles/Build.bat" Wiz98Editor Win64 Development -Project="F:/UE4/Projects/Wiz98/Wiz98.uproject" -WaitMutex -FromMsBuild
   ```

4. **Generate project files:**
   ```bash
   "F:/UE4/UE_4.27/Engine/Binaries/DotNET/UnrealBuildTool.exe" -projectfiles -project="F:/UE4/Projects/Wiz98/Wiz98.uproject" -game -engine -progress
   ```

5. **If build fails with Live Coding error:** The UE4 editor is still running. Ask the user to close it.

6. **After making changes, always commit.** Worktrees can be pruned unexpectedly, losing uncommitted work.

### Current Development Paths

| Path | Purpose |
|------|---------|
| `C:\NodeToCodeUE4` | Git repository root |
| `C:\NodeToCodeUE4\.claude\worktrees\<worktree>` | Active development worktree (name varies per session) |
| `F:\UE4\UE_4.27` | UE 4.27.2 engine install |
| `F:\UE4\Projects\Wiz98` | Test project (Wiz98.uproject) |
| `F:\UE4\Projects\Wiz98\Plugins\NodeToCode` | Junction -> worktree |

### Plugin Settings

Plugin settings are in **Project Settings > Plugins > Node to Code** (not Editor Preferences). Configure:
- **Provider** — Select LLM provider (Anthropic, OpenAI, Gemini, DeepSeek, Ollama, LM Studio)
- **Model** — Select model for the chosen provider
- **API Key** — Paste API key for cloud providers
- **Target Language** — Output language (C++, Python, JavaScript, C#, Swift, Pseudocode)
