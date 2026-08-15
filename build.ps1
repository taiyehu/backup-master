param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

$compiler = (Get-Command g++ -ErrorAction Stop).Source
$buildDirectory = "build-mingw"

cmake -S . -B $buildDirectory -G "MinGW Makefiles" `
    "-DCMAKE_CXX_COMPILER=$compiler" `
    "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $buildDirectory --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

ctest --test-dir $buildDirectory --output-on-failure
exit $LASTEXITCODE

