@echo off
rem Compila o teste do kvk com MSVC.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /I..\ref\vk\include kvk_test.c /Fe:kvk_test.exe
