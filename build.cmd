@echo off
rem This Source Code Form is subject to the terms of the Mozilla Public
rem License, v. 2.0. If a copy of the MPL was not distributed with this
rem file, You can obtain one at https://mozilla.org/MPL/2.0/.
rem Thin wrapper so build.ps1 runs from cmd / Explorer: build.cmd [-Configuration Release] [-Test] [-NoPackage]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
