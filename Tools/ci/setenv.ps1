if($IsWindows) {
    $TOOLS_DIR = "C:\tools"
} else {
    $TOOLS_DIR = "/opt"
    sudo chown appveyor:appveyor /opt
}

$env:CI_BUILD_DIR = (Resolve-Path "$PSScriptRoot/../..").Path
$env:SMING_HOME = Join-Path $env:CI_BUILD_DIR "Sming"

# Esp8266 
$env:ESP_HOME = Join-Path $TOOLS_DIR "esp-quick-toolchain"

# Esp32
$env:IDF_PATH = Join-Path $TOOLS_DIR "esp-idf"
$env:IDF_TOOLS_PATH = Join-Path $TOOLS_DIR "esp32"
$env:IDF_BRANCH = "sming/release/v4.3"

# Rp2040
$env:PICO_TOOLCHAIN_PATH = Join-Path $TOOLS_DIR "rp2040"

if ($IsWindows) {
    $env:PATH = "C:\MinGW\msys\1.0\bin;C:\MinGW\bin;$env:PATH"

    $env:PATH = "C:\Python39;C:\Python39\Scripts;$env:PATH"
    $env:PYTHON = "C:\Python39\python"
    $env:ESP32_PYTHON_PATH = "C:\Python39"

    $env:PATH = "$env:PROGRAMFILES\CMake\bin;$env:PATH"

    $env:PATH = "$env:PROGRAMFILES\doxygen\bin;$env:PROGRAMFILES\Graphviz\bin;$env:PATH"

    if (Test-Path "$TOOLS_DIR" ) {
        $env:NINJA = (get-childitem -path "$TOOLS_DIR" -recurse -include "ninja.exe" | select-object -first 1).FullName
    }
}