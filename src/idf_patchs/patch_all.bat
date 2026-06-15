@echo off
setlocal

echo make sure esp-idf work space clean!
if "%IDF_PATH%"=="" (
    echo please run esp-idf environment first so IDF_PATH is set
    exit /b 1
)

set "PATCH_PATH=%~dp0"

pushd "%IDF_PATH%" || (
    echo failed to cd to IDF_PATH: "%IDF_PATH%"
    exit /b 1
)
echo apply patch_spi.patch
git apply "%PATCH_PATH%patch_spi.patch"
if errorlevel 1 (
    popd
    exit /b 1
)
popd

pushd "%IDF_PATH%\components\bt\host\nimble\nimble" || (
    echo failed to cd to nimble dir under IDF_PATH
    exit /b 1
)
echo apply patch_nimble.patch
git apply "%PATCH_PATH%patch_nimble.patch"
if errorlevel 1 (
    popd
    exit /b 1
)
popd

echo done
