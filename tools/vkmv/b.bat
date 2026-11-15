@echo off
rem Compila o vkmv com MSVC
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /W3 /I..\..\ref\vk\include vkmv.c /Fe:vkmv.exe
