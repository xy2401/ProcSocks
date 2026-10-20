param(
    [string]$Compiler = 'g++',
    [switch]$Test,
    [switch]$BuildTests
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'build'
$distRoot = Join-Path $projectRoot 'dist'
$objectRoot = Join-Path $buildRoot 'obj'
$testRoot = Join-Path $buildRoot 'tests'
$symbolRoot = Join-Path $buildRoot 'symbols'
New-Item -ItemType Directory -Force -Path $buildRoot,$distRoot,$objectRoot,$testRoot,$symbolRoot,(Join-Path $distRoot 'driver') | Out-Null
$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$target = & $compilerPath -dumpmachine
if ($LASTEXITCODE -ne 0 -or $target.Trim() -ne 'x86_64-w64-mingw32') { throw 'Requires a Windows x64 MinGW-w64 g++ compiler.' }

$sdkArchive = Join-Path $projectRoot 'nfsdk-demo-1.7.8.4.zip'
if (!(Test-Path -LiteralPath $sdkArchive)) { throw 'Place nfsdk-demo-1.7.8.4.zip in the project root.' }
$sdkRoot = Join-Path $buildRoot 'sdk\nfsdk'
$sdkHash = (Get-FileHash -LiteralPath $sdkArchive -Algorithm SHA256).Hash
$sdkStamp = Join-Path $buildRoot 'sdk.sha256'
if (!(Test-Path -LiteralPath $sdkStamp) -or (Get-Content -LiteralPath $sdkStamp -Raw).Trim() -ne $sdkHash -or !(Test-Path -LiteralPath (Join-Path $sdkRoot 'wfp\include\nfapi.h'))) {
    Expand-Archive -LiteralPath $sdkArchive -DestinationPath (Join-Path $buildRoot 'sdk') -Force
    Set-Content -LiteralPath $sdkStamp -Value $sdkHash -Encoding ascii
}
$jsonHeader = Join-Path $projectRoot 'third_party\json.hpp'
if ((Get-FileHash -LiteralPath $jsonHeader -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63') { throw 'nlohmann/json v3.12.0 checksum mismatch.' }

$flags = @('-std=c++17','-O2','-g','-Wall','-Wextra','-Wpedantic','-pthread','-D_WIN32_WINNT=0x0A00','-DWIN32','-D_C_API','-DUNICODE','-D_UNICODE','-DWIN32_LEAN_AND_MEAN','-DNOMINMAX',
    '-I',(Join-Path $projectRoot 'src'),'-isystem',(Join-Path $projectRoot 'third_party'),'-isystem',(Join-Path $sdkRoot 'include'),'-isystem',(Join-Path $sdkRoot 'wfp\include'))
$objects = @{}
$sourceFiles = Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src') -Filter '*.cpp' | Sort-Object Name
foreach ($sourceFile in $sourceFiles) {
    Write-Host "Compiling $($sourceFile.Name)"
    $objectFile = Join-Path $objectRoot ($sourceFile.BaseName + '.o')
    & $compilerPath @flags -c $sourceFile.FullName -o $objectFile
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($sourceFile.Name)" }
    $objects[$sourceFile.BaseName] = $objectFile
}
$sdkLibrary = Join-Path $sdkRoot 'wfp\lib\release_c_api\x64\nfapi.lib'
$libraries = @($sdkLibrary,'-lws2_32','-ladvapi32','-ltdh','-liphlpapi','-lshell32','-lole32')
$linkFlags = @('-static','-static-libgcc','-static-libstdc++','-pthread')
$executable = Join-Path $distRoot 'ProcSocks.exe'
$debugExecutable = Join-Path $symbolRoot 'ProcSocks.debug.exe'
$allObjects = @($objects.Values | Sort-Object)
& $compilerPath @linkFlags -municode @allObjects @libraries -o $debugExecutable
if ($LASTEXITCODE -ne 0) { throw 'Executable link failed.' }
# Keep the matching full-symbol executable for debugging, outside the distribution.
Copy-Item -LiteralPath $debugExecutable -Destination $executable -Force
$strip = Join-Path (Split-Path -Parent $compilerPath) 'strip.exe'
if (!(Test-Path -LiteralPath $strip)) { throw 'Cannot find strip.exe beside the compiler.' }
& $strip --strip-unneeded $executable
if ($LASTEXITCODE -ne 0) { throw 'Release symbol stripping failed.' }
Copy-Item -LiteralPath (Join-Path $sdkRoot 'wfp\bin\release_c_api\x64\nfapi.dll') -Destination (Join-Path $distRoot 'nfapi.dll') -Force
Copy-Item -LiteralPath (Join-Path $distRoot 'nfapi.dll') -Destination $symbolRoot -Force
Copy-Item -LiteralPath (Join-Path $sdkRoot 'wfp\bin\driver\x64\netfilter2.sys') -Destination (Join-Path $distRoot 'driver\procsocks_nf.sys') -Force
Copy-Item -LiteralPath (Join-Path $sdkRoot 'license.rtf') -Destination (Join-Path $distRoot 'NetFilter-SDK-license.rtf') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party\json.LICENSE.MIT') -Destination (Join-Path $distRoot 'nlohmann-json-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'config.example.json') -Destination $distRoot -Force
foreach ($document in @('README.md','README.en.md','THIRD_PARTY_NOTICES.md','TEST_RESULTS.md')) {
    if (Test-Path -LiteralPath (Join-Path $projectRoot $document)) { Copy-Item -LiteralPath (Join-Path $projectRoot $document) -Destination $distRoot -Force }
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'platforms') -Destination $distRoot -Recurse -Force

$objdump = Join-Path (Split-Path -Parent $compilerPath) 'objdump.exe'
if (!(Test-Path -LiteralPath $objdump)) { $objdump = (Get-Command objdump -ErrorAction Stop).Source }
$dependencies = & $objdump -p $executable
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect executable imports.' }
$dllNames = @($dependencies | Select-String 'DLL Name:\s*(.+)' | ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() })
if ($dllNames -match 'libgcc|libstdc\+\+|libwinpthread') { throw 'Unexpected dynamic GCC runtime dependency.' }
Write-Host "Executable DLL imports: $($dllNames -join ', ')"
& $executable check --config (Join-Path $distRoot 'config.example.json')
if ($LASTEXITCODE -ne 0) { throw 'Packaged configuration check failed.' }
Write-Host "Built $executable"
Write-Host "Release size: $((Get-Item -LiteralPath $executable).Length) bytes; full symbols: $debugExecutable"

if ($Test -or $BuildTests) {
    foreach ($testSource in (Get-ChildItem -LiteralPath (Join-Path $projectRoot 'tests') -Filter '*_tests.cpp' | Sort-Object Name)) {
        $testExe = Join-Path $testRoot ($testSource.BaseName + '.exe')
        Write-Host "Building test $($testSource.Name)"
        if ($testSource.BaseName -eq 'engine_tests') {
            # This suite supplies SDK/tracker/log doubles. Never link the real driver API here.
            & $compilerPath @flags @linkFlags -D_NFAPI_STATIC_LIB $testSource.FullName (Join-Path $projectRoot 'src\engine.cpp') $objects['net'] $objects['socks'] $objects['l10n'] -lws2_32 -o $testExe
        } else {
            $excluded = @('main')
            # Driver tests include driver.cpp to exercise private pure helpers.
            if ($testSource.BaseName -in @('driver_guard_tests','driver_status_tests')) { $excluded += 'driver' }
            if ($testSource.BaseName -eq 'console_tests') { $excluded += 'tui' }
            $unitObjects = @($objects.GetEnumerator() | Where-Object { $_.Key -notin $excluded } | ForEach-Object { $_.Value })
            & $compilerPath @flags @linkFlags $testSource.FullName @unitObjects @libraries -o $testExe
        }
        if ($LASTEXITCODE -ne 0) { throw "Test compilation failed: $($testSource.Name)" }
    }
    Copy-Item -LiteralPath (Join-Path $distRoot 'nfapi.dll') -Destination $testRoot -Force
    if ($Test) { & (Join-Path $PSScriptRoot 'test.ps1') -NoBuild }
}
