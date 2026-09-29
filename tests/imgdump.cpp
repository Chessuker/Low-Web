// imgdump in.img out.rgba — decode with the browser's decoders, write "w h\n" + RGBA bytes.
#include <cstdio>
#include <fstream>
#include <iterator>
#include "../browser/image.h"
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    image::Image img;
    std::string err;
    if (!image::decode(d.data(), d.size(), img, err)) { printf("ERROR %s\n", err.c_str()); return 1; }
    FILE *o = fopen(argv[2], "wb");
    fprintf(o, "%d %d\n", img.w, img.h);
    fwrite(img.rgba.data(), 1, img.rgba.size(), o);
    fclose(o);
    return 0;
}
