# Version 180 — PowerShell progress parser hotfix

## Failure

Version 179 could not start because this expandable string contained `$total:`:

```powershell
$line = "Progress: [$bar] $completed/$total | Test $ordinal/$total: ..."
```

Windows PowerShell treats a colon immediately after an unbraced variable name as part of a scoped/drive-qualified variable reference. The script therefore failed at parse time.

## Fix

The progress line now uses the format operator:

```powershell
$line = 'Progress: [{0}] {1}/{2} | Test {3}/{2}: {4} | Elapsed {5}' -f `
    $bar, $completed, $total, $ordinal, $State.CurrentCase, $elapsedText
```

No variable is adjacent to a colon.

## Prevention

- `Test-N2CPowerShellSyntax.ps1` parses every `.ps1` using `System.Management.Automation.Language.Parser`.
- `RUN_N2C_AUTOMATION_AND_PACK.cmd` runs this parser preflight before the main orchestrator.
- PowerShell and Python static validators scan for unsafe `$variable:` interpolation while allowing valid scopes such as `$env:NAME` and safe braced forms such as `${name}:`.

## Runtime status

Version 180 Windows PowerShell execution, UE4.27 build, main ManualReplay and both restore processes are **NOT RUN** in the packaging environment.
