@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /W4 /utf-8 pdbq.cpp /Fe:pdbq.exe dbghelp.lib
