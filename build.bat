@echo off
rem Compila o katana com MSVC (AVX2; threads proprias, sem OpenMP).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /arch:AVX2 /fp:fast /W3 /I . /I src /I vk/include src\katana.c /Fe:katana.exe shell32.lib synchronization.lib ws2_32.lib
