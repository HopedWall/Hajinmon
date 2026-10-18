@echo off
rem Builds build\release\Hajinmon-<version>-x86_64.exe (launcher + game runtime, no game data).
rem Usage: tools\make_release.cmd [-SkipGameBuild]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make_release.ps1" %*
