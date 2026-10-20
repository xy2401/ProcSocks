param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (!$NoBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Test
    if ($LASTEXITCODE -ne 0) { throw 'Build or tests failed.' }
    return
}
$testRoot = Join-Path $projectRoot 'build\tests'
$testPrograms = @(Get-ChildItem -LiteralPath $testRoot -Filter '*_tests.exe' | Sort-Object Name)
$expectedTests = @('config_tests','console_tests','driver_guard_tests','driver_status_tests','engine_tests','l10n_tests','launch_tests','process_tests','session_tests','socks_tests','tui_tests')
foreach ($expectedTest in $expectedTests) {
    if ($expectedTest -notin $testPrograms.BaseName) { throw "Missing $expectedTest. Run build.ps1 -Test first." }
}
foreach ($testProgram in $testPrograms) {
    Write-Host "Running $($testProgram.Name)"
    & $testProgram.FullName
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $($testProgram.Name)" }
}
& (Join-Path $testRoot 'process_tests.exe') --metadata-smoke
if ($LASTEXITCODE -ne 0) { throw 'Native process metadata smoke test failed.' }
$app = Join-Path $projectRoot 'dist\ProcSocks.exe'
& $app --help
if ($LASTEXITCODE -ne 0) { throw 'CLI help failed.' }
& $app check --config (Join-Path $projectRoot 'config.example.json')
if ($LASTEXITCODE -ne 0) { throw 'CLI config check failed.' }
& $app check --config (Join-Path $testRoot 'missing-config.json')
if ($LASTEXITCODE -eq 0) { throw 'CLI accepted nonexistent config.' }
function Invoke-Headless([string]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $app
    $start.Arguments = $Arguments
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false, $true)
    $start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false, $true)
    $process = [Diagnostics.Process]::Start($start)
    try {
        $process.StandardInput.Close()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(5000)) { $process.Kill(); $process.WaitForExit(); throw 'Non-interactive CLI hung.' }
        return [pscustomobject]@{ ExitCode = $process.ExitCode; Output = $stdout.Result; Error = $stderr.Result }
    } finally { $process.Dispose() }
}
$noArgs = Invoke-Headless ''
if ($noArgs.ExitCode -ne 2 -or $noArgs.Output -notmatch 'ProcSocks.exe tui') { throw 'Non-interactive no-argument behavior failed.' }
$noConsoleTui = Invoke-Headless 'tui'
if ($noConsoleTui.ExitCode -ne 1 -or [string]::IsNullOrWhiteSpace($noConsoleTui.Error)) { throw 'Redirected TUI must fail without launching UAC.' }
$fixtureRoot = Join-Path $testRoot ('language-cli-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$fixtures = @()
try {
    $configPath = Join-Path $fixtureRoot '中文 配置.json'
    $fixtures += $configPath
    foreach ($language in @('zh-CN', 'en-US')) {
        $config = [ordered]@{ language = $language; proxy = @{ host = '127.0.0.1'; port = 1080; username = 'PRIVATE_USER_7X'; password = 'PRIVATE_PASS_8Y' }; processes = @('example.exe') }
        [IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
        $result = Invoke-Headless ('check --config "' + $configPath + '"')
        $expected = if ($language -eq 'zh-CN') { '配置检查通过' } else { 'Configuration OK' }
        if ($result.ExitCode -ne 0 -or !$result.Output.Contains($expected) -or $result.Error.Length -ne 0) { throw "CLI language override failed: $language" }
        if (($result.Output + $result.Error) -match 'PRIVATE_USER_7X|PRIVATE_PASS_8Y') { throw 'CLI exposed credentials.' }
        $tuiResult = Invoke-Headless ('tui --config "' + $configPath + '"')
        $tuiExpected = if ($language -eq 'zh-CN') { '交互菜单需要可交互的控制台' } else { 'The TUI requires interactive console' }
        if ($tuiResult.ExitCode -ne 1 -or !$tuiResult.Error.Contains($tuiExpected) -or $tuiResult.Output.Length -ne 0) { throw "Headless TUI language/exit failed: $language" }
        $config.proxy.port = 0
        [IO.File]::WriteAllText($configPath, ($config | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
        $result = Invoke-Headless ('check --config "' + $configPath + '"')
        $expected = if ($language -eq 'zh-CN') { 'proxy.port 必须是' } else { 'proxy.port must be' }
        if ($result.ExitCode -ne 1 -or !$result.Error.Contains($expected) -or $result.Output.Length -ne 0) { throw "CLI validation language failed: $language" }
        if (($result.Output + $result.Error) -match 'PRIVATE_USER_7X|PRIVATE_PASS_8Y') { throw 'CLI error exposed credentials.' }
    }
    $systemChinese = $noArgs.Output.Contains('按进程')
    $oppositeLanguage = if ($systemChinese) { 'en-US' } else { 'zh-CN' }
    $brokenJson = '{"language":"' + $oppositeLanguage + '","password":"PRIVATE_PASS_8Y",'
    [IO.File]::WriteAllText($configPath, $brokenJson, [Text.UTF8Encoding]::new($false))
    $result = Invoke-Headless ('check --config "' + $configPath + '"')
    $expected = if ($systemChinese) { 'JSON 格式错误' } else { 'Invalid JSON' }
    if ($result.ExitCode -ne 1 -or !$result.Error.Contains($expected) -or $result.Error.Contains('PRIVATE_PASS_8Y')) { throw 'Malformed JSON language fallback/redaction failed.' }
    $duplicateJson = '{"language":"' + $oppositeLanguage + '","proxy":{"password":"PRIVATE_PASS_8Y","password":"again"}}'
    [IO.File]::WriteAllText($configPath, $duplicateJson, [Text.UTF8Encoding]::new($false))
    $result = Invoke-Headless ('check --config "' + $configPath + '"')
    $expected = if ($systemChinese) { 'JSON 包含重复键' } else { 'Duplicate JSON key' }
    if ($result.ExitCode -ne 1 -or !$result.Error.Contains($expected) -or $result.Error.Contains('PRIVATE_PASS_8Y')) { throw 'Duplicate-key language fallback/redaction failed.' }
    [IO.File]::WriteAllText($configPath, '{"language":"PRIVATE_PASS_8Y"}', [Text.UTF8Encoding]::new($false))
    $result = Invoke-Headless ('check --config "' + $configPath + '"')
    $expected = if ($systemChinese) { 'language 必须是' } else { 'language must be' }
    if ($result.ExitCode -ne 1 -or !$result.Error.Contains($expected) -or $result.Error.Contains('PRIVATE_PASS_8Y')) { throw 'Invalid language fallback/redaction failed.' }
    Write-Host 'Bilingual redirected CLI checks passed (UTF-8, Chinese/space path, language fallback, credential redaction).'
} finally {
    foreach ($fixture in $fixtures) { if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Force } }
    Remove-Item -LiteralPath $fixtureRoot
}
Write-Host 'All automated tests passed. No NetFilter driver was installed or loaded by these tests.'
$global:LASTEXITCODE = 0
