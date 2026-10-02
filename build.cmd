@echo off
rem Builds everything: the browser, the server, the sample sites and the test tools.
rem Needs g++ (MinGW-w64) and clang with the wasm32 target (LLVM).
setlocal
cd /d "%~dp0"

set CLANG=clang
where clang >nul 2>nul || set CLANG="C:\Program Files\LLVM\bin\clang.exe"
set CXX=g++ -std=c++20 -O2 -Wall -Wextra -static
set WASM=%CLANG% --target=wasm32 -O2 -mcpu=mvp -mbulk-memory -mnontrapping-fptoint -msign-ext -mmutable-globals ^
  -nostdlib -fno-builtin -Wall -Wextra -Wl,--no-entry -Wl,--strip-all -Isdk

if not exist bin mkdir bin
if not exist build mkdir build
if not exist sites\www\paint mkdir sites\www\paint

echo [1/5] viewer   build\viewer.wasm (built into the browser)
%WASM% -o build\viewer.wasm sites\viewer\viewer.c || goto :fail

echo [2/5] browser  bin\lowweb.exe
windres browser\lowweb.rc -O coff -o build\lowweb_res.o --include-dir browser --include-dir build || goto :fail
%CXX% -mwindows -o bin\lowweb.exe browser\main.cpp browser\wasm.cpp browser\net.cpp browser\cache.cpp browser\cookies.cpp browser\http2.cpp browser\image.cpp browser\inflate.cpp browser\webp.cpp browser\svg.cpp browser\video.cpp ^
  build\lowweb_res.o -lws2_32 -lsecur32 -lcomdlg32 -lgdi32 -luser32 -lshell32 -lusp10 -ld3d11 -lole32 -loleaut32 || goto :fail

echo [3/5] server   bin\lowd.exe
%CXX% -o build\lowd.exe server\lowd.cpp -lws2_32 || goto :fail
copy /y build\lowd.exe bin\lowd.exe >nul 2>nul || echo       note: bin\lowd.exe is running, so it was not replaced (new build: build\lowd.exe)

echo [4/5] sites    sites\www\index.wasm, sites\www\paint\index.wasm
%WASM% -o sites\www\index.wasm sites\home\home.c || goto :fail
%WASM% -o sites\www\paint\index.wasm sites\paint\paint.c || goto :fail

echo [5/5] tools    bin\wasmrun.exe, bin\imgdump.exe, bin\fetchtest.exe, bin\inflatetest.exe, bin\cookietest.exe, bin\hpacktest.exe, bin\viewerbench.exe
%CXX% -o bin\wasmrun.exe tests\wasmrun.cpp browser\wasm.cpp browser\image.cpp browser\inflate.cpp browser\webp.cpp browser\svg.cpp || goto :fail
%CXX% -o bin\imgdump.exe tests\imgdump.cpp browser\image.cpp browser\inflate.cpp browser\webp.cpp browser\svg.cpp || goto :fail
%CXX% -o bin\fetchtest.exe tests\fetchtest.cpp browser\net.cpp browser\cache.cpp browser\cookies.cpp browser\http2.cpp browser\inflate.cpp -lws2_32 -lsecur32 || goto :fail
%CXX% -o bin\inflatetest.exe tests\inflatetest.cpp browser\inflate.cpp || goto :fail
%CXX% -o bin\viewerbench.exe tests\viewerbench.cpp browser\wasm.cpp || goto :fail
%CXX% -o bin\hpacktest.exe tests\hpacktest.cpp browser\http2.cpp -lws2_32 -lsecur32 || goto :fail
%CXX% -o bin\cookietest.exe tests\cookietest.cpp browser\cookies.cpp browser\http2.cpp browser\cache.cpp browser\net.cpp browser\inflate.cpp -lws2_32 -lsecur32 || goto :fail
%WASM% -o build\ops.wasm tests\ops.c || goto :fail

echo done.
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
