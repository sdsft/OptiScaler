@echo off

if "%~1"=="" (
    echo Usage: %~nx0 ShaderName
    exit /b 1
)

set ShaderName=%1

echo Creating DX12-compatible FXC CSO
"%~dp0fxc.exe" /T cs_5_0 /E CSMain /O3 "%ShaderName%.hlsl" /Fo "%ShaderName%_Shader_DX12.cso"

if errorlevel 1 (
    echo DX12 FXC compilation failed!
    exit /b 1
)

echo Creating DX12 Header
python "%~dp0create_header.py" "%ShaderName%_Shader_DX12.cso" "%ShaderName%_Shader_DX12.h" %ShaderName%_cso

echo Creating DX11 FXC CSO
"%~dp0fxc.exe" /T cs_5_0 /E CSMain /O3 "%ShaderName%.hlsl" /Fo "%ShaderName%_Shader_DX11.cso"

if errorlevel 1 (
    echo DX11 FXC compilation failed!
    exit /b 1
)

echo Creating DX11 Header
python "%~dp0create_header.py" "%ShaderName%_Shader_DX11.cso" "%ShaderName%_Shader_DX11.h" %ShaderName%_cso