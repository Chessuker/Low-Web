# tests/check_lowd.sh [LOWD] — starts the site server on sites/www and checks its answers
# with curl: pages, redirects, refused paths and methods. LOWD defaults to bin/lowd.exe
# (Windows) or build/lowd (elsewhere). Used by CI on Windows and Linux.
cd "$(dirname "$0")/.."
mkdir -p build
LOWD=${1:-$( [ -x bin/lowd.exe ] && echo bin/lowd.exe || echo build/lowd )}
PORT=18080
"$LOWD" -p $PORT -b 127.0.0.1 sites/www > build/lowd-test.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null' EXIT
for i in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break; sleep 0.1; done

fails=0
expect() {  # NAME WANTED-STATUS CURL-ARGS...
  name=$1 want=$2
  shift 2
  got=$(curl -s -o build/lowd-test.body -w '%{http_code}' "$@")
  if [ "$got" = "$want" ]; then echo "ok    $name"; else echo "FAIL  $name: HTTP $got, wanted $want"; fails=$((fails + 1)); fi
}
U=http://127.0.0.1:$PORT
expect "a site's index.wasm" 200 "$U/"
if [ "$(head -c 4 build/lowd-test.body | od -An -c | tr -d ' ')" = '\0asm' ]; then echo "ok    ... is WebAssembly"
else echo "FAIL  ... is not WebAssembly"; fails=$((fails + 1)); fi
expect "a text file" 200 "$U/about.txt"
expect "a folder without '/' is redirected" 301 "$U/paint"
expect "... to the folder's page" 200 -L "$U/paint"
expect "a missing file" 404 "$U/nothing-here"
expect "'..' can't leave the site" 400 --path-as-is "$U/../server/lowd.cpp"
expect "... nor encoded '%2e%2e'" 400 --path-as-is "$U/%2e%2e/server/lowd.cpp"
expect "POST is refused" 405 -X POST "$U/"
expect "HEAD works" 200 -I "$U/about.txt"

rm -f build/lowd-test.body
[ $fails = 0 ] && echo "ALL OK" || echo "$fails FAILED"
exit $fails
