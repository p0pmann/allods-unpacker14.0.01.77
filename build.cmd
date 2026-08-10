@echo off
setlocal

set "ROOT=%~dp0"
set "OUT=%ROOT%build"
set "UNPACKER=%ROOT%src\unpacker"
set "MH=%UNPACKER%\Mhook\minhook"

if defined VSCMD_VER goto toolchain_ready
set "VCVARS="
for %%V in (2017 2019 2022 18) do for %%E in (BuildTools Community Professional Enterprise) do if exist "C:\Program Files\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS (
  echo An MSVC installation with the x86 toolchain was not found.
  exit /b 1
)
call "%VCVARS%" x64_x86 >nul
if errorlevel 1 exit /b 1

:toolchain_ready
pushd "%ROOT%"

if not exist "%OUT%" mkdir "%OUT%"

echo Building pango.dll...
cl /nologo /O2 /MT /LD /W3 ^
  "%ROOT%src\carrier\pango_carrier.c" ^
  /Fe:"%OUT%\pango.dll" /link kernel32.lib
if errorlevel 1 exit /b 1

echo Building AllodsUnpacker14.dll...
cl /nologo /O2 /MT /LD /EHsc /W3 /DWIN32 ^
  /I "%UNPACKER%\Mhook" /I "%MH%\include" /I "%MH%\src" ^
  "%UNPACKER%\AllodsUnpacker14.cpp" ^
  "%UNPACKER%\Containers\Fixups.cpp" ^
  "%UNPACKER%\Containers\HrefMap.cpp" ^
  "%UNPACKER%\Containers\PackReader.cpp" ^
  "%UNPACKER%\Containers\SlotMap.cpp" ^
  "%UNPACKER%\Containers\Types.cpp" ^
  "%UNPACKER%\Mhook\Hooks.cpp" ^
  "%UNPACKER%\Tools\Freeze.cpp" ^
  "%UNPACKER%\Tools\Fs.cpp" ^
  "%UNPACKER%\Tools\Log.cpp" ^
  "%UNPACKER%\Unpack\EngineWriter.cpp" ^
  "%UNPACKER%\Unpack\HrefWriter.cpp" ^
  "%UNPACKER%\Unpack\MapLoader.cpp" ^
  "%UNPACKER%\Unpack\Sink.cpp" ^
  "%MH%\src\hook.c" "%MH%\src\buffer.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde32.c" ^
  /Fe:"%OUT%\AllodsUnpacker14.dll" /link kernel32.lib user32.lib
if errorlevel 1 exit /b 1

del /q *.obj *.exp *.lib 2>nul
copy /y "%ROOT%config\AllodsUnpacker14.ini" "%OUT%\AllodsUnpacker14.ini" >nul
echo Built %OUT%
popd
