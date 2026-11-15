@echo off
rem Compila o vkq4k com MSVC.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\source\katana\tools\vkq4k
cl /nologo /O2 /W3 /I C:\source\katana\ref\vk\include vkq4k.c /Fe:vkq4k.exe
