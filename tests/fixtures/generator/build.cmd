@echo off
rem This Source Code Form is subject to the terms of the Mozilla Public
rem License, v. 2.0. If a copy of the MPL was not distributed with this
rem file, You can obtain one at https://mozilla.org/MPL/2.0/.
rem
rem Builds the fixture generator from a Developer Command Prompt. Run "vcpkg install --triplet x64-windows-static-md" here
rem first, or pass an installed tree: build.cmd <vcpkg_installed\x64-windows-static-md> <output directory>
setlocal
set HERE=%~dp0
set ROOT=%HERE%..\..\..
set TREE=%~1
if "%TREE%"=="" set TREE=%HERE%vcpkg_installed\x64-windows-static-md
set OUT=%~2
if "%OUT%"=="" set OUT=%HERE%output
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%" || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /MD /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I"%ROOT%\src" /I"%TREE%\include" ^
    "%HERE%generate.cpp" "%ROOT%\src\Tools\testaudio\TestAudio.cpp" "%ROOT%\src\Common\util\Text.cpp" /Fe:generate.exe ^
    /link /LIBPATH:"%TREE%\lib" avformat.lib avcodec.lib swresample.lib avutil.lib FLAC.lib sndfile.lib ogg.lib vorbis.lib ^
    vorbisenc.lib vorbisfile.lib opus.lib mpg123.lib libmp3lame-static.lib libmpghip-static.lib ^
    zs.lib mfuuid.lib strmiids.lib mfplat.lib oleaut32.lib uuid.lib bcrypt.lib ws2_32.lib secur32.lib user32.lib ole32.lib ^
    shlwapi.lib crypt32.lib ncrypt.lib gdi32.lib || exit /b 1
.\generate.exe "%HERE%.."
