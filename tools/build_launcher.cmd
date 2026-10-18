@echo off
rem Builds the Hajinmon launcher into build\launcher\Hajinmon.exe.
rem Requires the .NET 9 SDK (winget install --id Microsoft.DotNet.SDK.9 -e).
setlocal
set "ROOT=%~dp0.."
dotnet publish "%ROOT%\launcher\Hajinmon.Launcher.csproj" -c Release -o "%ROOT%\build\launcher"
if errorlevel 1 exit /b 1
echo.
echo Launcher built: %ROOT%\build\launcher\Hajinmon.exe
