# tests/dev.sh [browser] — quick rebuild while working on the viewer: builds the viewer and
# puts it next to the exe as bin/viewer.wasm (which overrides the built-in copy); with
# "browser", also rebuilds bin/lowweb.exe. Delete bin/viewer.wasm afterwards (build.cmd
# builds the viewer into the exe).
set -e
cd "$(dirname "$0")/.."
"/c/Program Files/LLVM/bin/clang.exe" --target=wasm32 -O2 -mcpu=mvp -mbulk-memory -mnontrapping-fptoint -msign-ext -mmutable-globals -nostdlib -fno-builtin -Wall -Wextra -Wl,--no-entry -Wl,--strip-all -Isdk -o build/viewer.wasm sites/viewer/viewer.c
cp build/viewer.wasm bin/viewer.wasm
if [ "$1" = "browser" ]; then
  g++ -std=c++20 -O2 -Wall -Wextra -static -mwindows -o bin/lowweb.exe browser/main.cpp browser/wasm.cpp browser/net.cpp browser/cache.cpp browser/cookies.cpp browser/http2.cpp browser/image.cpp browser/inflate.cpp browser/webp.cpp browser/svg.cpp browser/video.cpp build/lowweb_res.o -lws2_32 -lsecur32 -lcomdlg32 -lgdi32 -luser32 -lshell32 -lusp10 -ld3d11 -lole32 -loleaut32
fi
