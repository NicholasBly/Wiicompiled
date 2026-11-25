<#
.SYNOPSIS
Builds a test copy of Retro Rewind from the low-latency-pacing branch, next to an existing
WiiCompiled installation, without changing that installation.

.DESCRIPTION
Reuses what the WiiCompiled installer already set up: its Toolkit (translator, CMake, Ninja,
LLVM-MinGW), the extracted game files and offline dependencies in BuildWorkspace, and the
installed Retro Rewind folder. It downloads the branch's source from GitHub (no git needed),
builds with the project's own LocalBuild.ps1, and writes the result to
<install>\RetroRewind-LowLatency. Your normal RetroRewind folder is never touched.

Run it from PowerShell:
    powershell -ExecutionPolicy Bypass -File .\Build-LowLatencyRetroRewind.ps1 -Launch

To build the PR branches instead (everything except the latency overlay):
    powershell -ExecutionPolicy Bypass -File .\Build-LowLatencyRetroRewind.ps1 -Branch reflex -AuroraBranch frame-pacing -Launch

The first run takes a while (it translates the game and compiles aurora from source).
#>
param(
    # Where the game is installed. Found automatically for the WiiCompiled installer and Wheel Wizard.
    [string]$InstallDir,
    # Your RetroRewind6 folder (the one with Binaries\Code.pul). Read from Config.toml if omitted.
    [string]$RetroRewindDir,
    [string]$Owner = 'NicholasBly',
    [string]$Branch = 'low-latency-pacing',
    # The aurora-wiicompiled branch. Defaults to the same name as -Branch. For the PR branches
    # (late-tick-start, pacing-settings, reflex) use: -AuroraBranch frame-pacing
    [string]$AuroraBranch,
    # v3 needs AVX2 (any desktop CPU from about 2015 on). Use v2 for older CPUs.
    [ValidateSet('v2', 'v3')] [string]$CpuProfile = 'v3',
    # Re-download the source even if it was downloaded before.
    [switch]$Refresh,
    # Start the game when the build finishes.
    [switch]$Launch
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Step([string]$Message) { Write-Host "`n==> $Message" -ForegroundColor Cyan }
function Fail([string]$Message) { Write-Host "`nERROR: $Message" -ForegroundColor Red; exit 1 }

# robocopy exit codes below 8 mean success.
function Copy-Tree([string]$From, [string]$To, [string[]]$ExcludeDirs = @()) {
    $arguments = @($From, $To, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/R:1', '/W:1')
    if ($ExcludeDirs.Count -gt 0) { $arguments += '/XD'; $arguments += $ExcludeDirs }
    & robocopy @arguments | Out-Null
    if ($LASTEXITCODE -ge 8) { Fail "Copying $From to $To failed (robocopy exit $LASTEXITCODE)." }
}

function Get-Source([string]$Repo, [string]$Name, [string]$BranchName, [string]$Downloads) {
    # Each branch gets its own download, so switching branches never reuses the wrong source.
    $zip = Join-Path $Downloads "$Name-$BranchName.zip"
    $folder = Join-Path $Downloads "$Name-$BranchName"
    if ($Refresh -or -not (Test-Path -LiteralPath $folder)) {
        $url = "https://github.com/$Owner/$Repo/archive/refs/heads/$BranchName.zip"
        Write-Host "Downloading $url"
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
        if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
        Expand-Archive -LiteralPath $zip -DestinationPath $folder -Force
    }
    # GitHub zips contain one top-level folder named <repo>-<branch>.
    $inner = @(Get-ChildItem -LiteralPath $folder -Directory)
    if ($inner.Count -ne 1) { Fail "Unexpected layout in the $Repo download." }
    return $inner[0].FullName
}

# --- 1. Check the existing installation -------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    $candidates = @(
        (Join-Path $env:APPDATA 'CT-MKWII\Recomp\Install'),        # Wheel Wizard
        (Join-Path $env:LOCALAPPDATA 'Programs\WiiCompiled')        # WiiCompiled installer
    )
    $InstallDir = $candidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'Toolkit\Translator\Translator.Cli.exe') } |
        Select-Object -First 1
    if (-not $InstallDir) { Fail "No installation found in: $($candidates -join ', '). Pass -InstallDir." }
}
Step "Checking the installation in $InstallDir"
$toolkit = Join-Path $InstallDir 'Toolkit'
$installedWorkspace = Join-Path $InstallDir 'BuildWorkspace'
$installedRetro = Join-Path $InstallDir 'RetroRewind'
foreach ($required in @(
    (Join-Path $toolkit 'Translator\Translator.Cli.exe'),
    (Join-Path $installedWorkspace 'Assets\main.dol'),
    (Join-Path $installedWorkspace 'Assets\StaticR.rel'),
    (Join-Path $installedWorkspace 'Dependencies'),
    (Join-Path $installedRetro 'RetroRewind.exe')
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        Fail "Missing $required. Make sure Retro Rewind is installed and built, or pass -InstallDir."
    }
}

# The game keeps its settings in <folder with portable.txt>\UserData when it finds that marker up
# to 4 folders above the .exe (Wheel Wizard installs do this), otherwise in %LOCALAPPDATA%\WiiCompiled.
$dataDir = Join-Path $env:LOCALAPPDATA 'WiiCompiled'
$probe = $installedRetro
for ($level = 0; $level -le 4 -and $probe; $level++) {
    if (Test-Path -LiteralPath (Join-Path $probe 'portable.txt')) { $dataDir = Join-Path $probe 'UserData'; break }
    $probe = Split-Path -Parent $probe
}
Write-Host "Game settings folder: $dataDir"

# --- 2. Find the Retro Rewind package ---------------------------------------------------------
Step 'Finding your Retro Rewind files'
if ([string]::IsNullOrWhiteSpace($RetroRewindDir)) {
    $config = Join-Path $dataDir 'Config.toml'
    if (Test-Path -LiteralPath $config) {
        $match = Select-String -LiteralPath $config -Pattern '^\s*retro_rewind_root\s*=\s*["''](.+?)["'']' | Select-Object -First 1
        if ($match) { $RetroRewindDir = $match.Matches[0].Groups[1].Value -replace '\\\\', '\' }
    }
}
if ([string]::IsNullOrWhiteSpace($RetroRewindDir)) {
    $staged = Join-Path $installedWorkspace 'PulsarPacks\completed\RetroRewind\RetroRewind6'
    if (Test-Path -LiteralPath (Join-Path $staged 'Binaries\Code.pul')) { $RetroRewindDir = $staged }
}
if ([string]::IsNullOrWhiteSpace($RetroRewindDir)) {
    Fail 'Could not find your Retro Rewind files. Pass -RetroRewindDir "C:\path\to\RetroRewind6".'
}
if (-not (Test-Path -LiteralPath (Join-Path $RetroRewindDir 'Binaries\Code.pul')) -and
    (Test-Path -LiteralPath (Join-Path $RetroRewindDir 'RetroRewind6\Binaries\Code.pul'))) {
    $RetroRewindDir = Join-Path $RetroRewindDir 'RetroRewind6'
}
if (-not (Test-Path -LiteralPath (Join-Path $RetroRewindDir 'Binaries\Code.pul'))) {
    Fail "$RetroRewindDir has no Binaries\Code.pul. Point -RetroRewindDir at your RetroRewind6 folder."
}
Write-Host "Using $RetroRewindDir"

# --- 3. Download the branch source ------------------------------------------------------------
$work = Join-Path $InstallDir 'LowLatencyTest'
$downloads = Join-Path $work 'Downloads'
[IO.Directory]::CreateDirectory($downloads) | Out-Null
if ([string]::IsNullOrWhiteSpace($AuroraBranch)) { $AuroraBranch = $Branch }
Step "Downloading $Owner/Wiicompiled ($Branch) and $Owner/aurora-wiicompiled ($AuroraBranch)"
$wiiSource = Get-Source 'Wiicompiled' 'wiicompiled' $Branch $downloads
$auroraSource = Get-Source 'aurora-wiicompiled' 'aurora' $AuroraBranch $downloads

# --- 4. Make a separate build workspace -------------------------------------------------------
# A copy of the installed workspace (game files, offline dependencies, projects) with the
# branch's runtime, aurora and build scripts swapped in. Old build caches are left out.
Step 'Preparing a separate build workspace'
$workspace = Join-Path $work 'BuildWorkspace'
Copy-Tree $installedWorkspace $workspace @(
    (Join-Path $installedWorkspace 'generated'), (Join-Path $installedWorkspace 'native-build'),
    (Join-Path $installedWorkspace 'build'), (Join-Path $installedWorkspace 'runtime'),
    (Join-Path $installedWorkspace 'aurora-main'))
foreach ($name in @('runtime', 'aurora-main')) {
    $target = Join-Path $workspace $name
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
}
Copy-Tree (Join-Path $wiiSource 'runtime') (Join-Path $workspace 'runtime')
Copy-Tree $auroraSource (Join-Path $workspace 'aurora-main')
# Like the installer: aurora's third-party sources come from Dependencies, not extern\.
$auroraExtern = Join-Path $workspace 'aurora-main\extern'
if (Test-Path -LiteralPath $auroraExtern) {
    Get-ChildItem -LiteralPath $auroraExtern -Directory -Force | Remove-Item -Recurse -Force
}
foreach ($script in @('LocalBuild.ps1', 'NativeBuildFlags.ps1')) {
    Copy-Item -LiteralPath (Join-Path $wiiSource "Launcher\$script") -Destination (Join-Path $workspace $script) -Force
}
$translator = Join-Path $toolkit 'Translator\Translator.Cli.exe'
& $translator emit-native-index --native-source-dir (Join-Path $workspace 'runtime\src') `
    --out (Join-Path $workspace 'runtime\native-index.json')
if ($LASTEXITCODE -ne 0) { Fail "The translator could not index the runtime (exit $LASTEXITCODE)." }

# --- 5. Build ---------------------------------------------------------------------------------
$buildOutput = Join-Path $work 'Build'
$log = Join-Path $work 'build.log'
$buildArgs = @(
    '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $workspace 'LocalBuild.ps1'),
    '-Workspace', $workspace, '-Toolkit', $toolkit, '-Profile', 'retro-rewind',
    '-OutputDirectory', $buildOutput, '-RetroRewindPackageDirectory', $RetroRewindDir,
    '-X86CpuProfile', $CpuProfile)
$payload = Join-Path $workspace 'Assets\OfflinePayload'
if (Test-Path -LiteralPath (Join-Path $payload 'binary')) {
    $buildArgs += @('-RetroWfcOfflineDirectory', $payload, '-RetroWfcPayloadOrigin', 'downloaded')
} else {
    Write-Host 'No Retro-WFC payload found: this test build will not go online (Time Trials are fine).' -ForegroundColor Yellow
    $buildArgs += '-SkipRetroWfcPayload'
}
Step "Building Retro Rewind (log: $log). The first build can take 20-60 minutes."
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
# Windows PowerShell turns a program's stderr lines into errors under 2>&1; don't let them stop us.
$ErrorActionPreference = 'Continue'
& $powershell @buildArgs 2>&1 | Tee-Object -FilePath $log | ForEach-Object {
    $line = "$_"
    if ($line -like 'MKWCBUILD:STEP:*') { Write-Host ($line -replace '^MKWCBUILD:STEP:\S+\s*', '  - ') }
    # Real failures only: compiler/linker errors and failed build steps, not CMake feature checks.
    elseif ($line -match '^FAILED:|\berror:|\bfatal error\b|CMake Error') { Write-Host $line -ForegroundColor Red }
}
$buildExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($buildExit -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $buildOutput 'RetroRewind.exe'))) {
    Fail "The build failed (exit $buildExit). Send $log to get it fixed."
}

# --- 6. Put the test game next to the real one ------------------------------------------------
Step 'Assembling RetroRewind-LowLatency'
$product = Join-Path $InstallDir 'RetroRewind-LowLatency'
if (Test-Path -LiteralPath $product) { Remove-Item -LiteralPath $product -Recurse -Force }
# Start from the installed game (runtime assets) and lay the new build over it.
Copy-Tree $installedRetro $product
Copy-Tree $buildOutput $product
$exe = Join-Path $product 'RetroRewind.exe'

Write-Host "`nDone. Test build: $exe" -ForegroundColor Green
Write-Host 'In game: F10 -> Graphics -> Latency overlay, Smooth frames, Frame rate cap, NVIDIA Reflex.'
Write-Host "Latency CSVs: $(Join-Path $dataDir 'LatencyLogs')"
Write-Host 'Settings live in the same Config.toml as your normal install; the normal build ignores the new ones.'

if ($Launch) {
    Start-Process -FilePath $exe -WorkingDirectory $product
}
