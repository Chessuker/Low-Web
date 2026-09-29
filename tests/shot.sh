# tests/shot.sh NAME URL [WxH] [script] — opens URL in the browser, saves a screenshot as
# build/shots/NAME.png (PNG: the browser writes BMP, which is ~10x larger) and shows the log.
cd "$(dirname "$0")/.."
mkdir -p build/shots
size=${3:-1100x760}
script=${4:-"wait 1500"}
timeout 90 bin/lowweb.exe "$2" --size $size --log build/shots/$1.log --script "$script" --screenshot build/shots/$1.bmp
python -c "from PIL import Image; Image.open('build/shots/$1.bmp').save('build/shots/$1.png')" && rm -f build/shots/$1.bmp
cat build/shots/$1.log
