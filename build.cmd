@echo off
rem Thin wrapper so build.ps1 runs from cmd / Explorer: build.cmd [-Configuration Release] [-Test] [-Package]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
