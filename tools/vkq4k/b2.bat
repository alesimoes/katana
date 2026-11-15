@echo off
rem Compila o vkmm com MSVC.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\source\katana\tools\vkq4k
cl /nologo /O2 /W3 /openmp /I C:\source\katana\vk\include vkmm.c /Fe:vkmm.exe
