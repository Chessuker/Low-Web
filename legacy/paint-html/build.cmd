@echo off
rem Build paint.wasm. Uses clang from PATH, else the default LLVM install location.
set CLANG=clang
where clang >nul 2>nul || set CLANG="C:\Program Files\LLVM\bin\clang.exe"
cd /d "%~dp0"
%CLANG% --target=wasm32 -O2 -mbulk-memory -mnontrapping-fptoint -nostdlib -Wall -Wextra ^
  -Wl,--no-entry -Wl,--strip-all -o paint.wasm paint.c && echo built paint.wasm
