# Windows launcher. Docker Desktop must be running with Linux containers.
# Arguments are forwarded to the existing installer (for example: install).
$installerArgs = $args
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

try {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        throw 'Docker is required. Install and start Docker Desktop, then try again.'
    }
    # Windows PowerShell can turn native stderr into errors when redirected.
    $ErrorActionPreference = 'Continue'
    $osType = & docker info --format '{{.OSType}}' 2>$null
    $dockerExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($dockerExit -ne 0) {
        throw 'Docker is not accessible. Start Docker Desktop, wait until it is ready, then try again.'
    }
    if ($osType -ne 'linux') {
        throw 'Switch Docker Desktop to Linux containers, then try again.'
    }

    $image = 'mib-std2-wirelessbridge-waa'
    New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot 'local') -Force | Out-Null
    $log = Join-Path $PSScriptRoot 'local/setup-build.log'
    # A Windows-specific hash avoids requiring Unix cksum or Git.
    $deps = 'sha256-' + (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'Dockerfile') -Algorithm SHA256).Hash.ToLowerInvariant()
    $ErrorActionPreference = 'Continue'
    $imageJson = & docker image inspect $image 2>$null
    $inspectExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $built = ''
    if ($inspectExit -eq 0) {
        $metadata = ($imageJson -join "`n") | ConvertFrom-Json
        $built = $metadata[0].Config.Labels.'org.wirelessbridge.deps'
    }
    Write-Host "`n wirelessbridge > setup`n"
    if ($inspectExit -eq 0 -and $built -eq $deps) {
        Write-Host ' Setup image ready (already prepared).'
    } else {
        Write-Host ' Preparing setup image. First launch needs internet and may take a few minutes.'
        Write-Host " Build log: $log"
        $ErrorActionPreference = 'Continue'
        & docker build --target runtime --build-arg "DEPS=$deps" -t $image . 2>&1 |
            Tee-Object -FilePath $log
        $buildExit = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($buildExit -ne 0) {
            throw "Could not prepare setup image. Check $log and your internet connection."
        }
    }

    $version = ''
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $ErrorActionPreference = 'Continue'
        $description = & git describe --tags --always --dirty 2>$null
        if ($LASTEXITCODE -eq 0) { $version = "$description" }
        $ErrorActionPreference = 'Stop'
    }
    $gateway = ''
    # Gateway discovery is optional; setup also accepts a manually entered IP.
    try {
        $route = Get-NetRoute -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' |
            Where-Object { $_.NextHop -ne '0.0.0.0' } |
            Sort-Object { $_.RouteMetric + $_.InterfaceMetric } |
            Select-Object -First 1
        if ($route) { $gateway = $route.NextHop }
    } catch { }

    $dockerArgs = @('run', '--rm', '-i')
    if (-not [Console]::IsInputRedirected -and -not [Console]::IsOutputRedirected) {
        $dockerArgs += '-t'
    }
    $dockerArgs += @('--read-only', '--tmpfs', '/tmp')
    foreach ($directory in @('local', 'installer', 'native', 'profiles')) {
        $source = Join-Path $PSScriptRoot $directory
        $mount = "${source}:/app/${directory}"
        if ($directory -ne 'local') { $mount += ':ro' }
        $dockerArgs += @('-v', $mount)
    }
    $notices = Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.md'
    $dockerArgs += @('-v', "${notices}:/app/THIRD_PARTY_NOTICES.md:ro",
        '-p', '41000-41031:41000-41031',
        '-e', "WAA_VERSION=$version", '-e', "WAA_GATEWAY=$gateway", $image)
    $dockerArgs += $installerArgs
    $ErrorActionPreference = 'Continue'
    & docker @dockerArgs
    exit $LASTEXITCODE
} catch {
    Write-Host "`nSetup failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
