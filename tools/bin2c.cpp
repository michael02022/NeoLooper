// Portable replacement for `xxd -i`: emits a byte array + length symbol from
// a binary file, in the exact shape xxd -i produces (see
// src/gui/fontawesome_data.hpp). xxd isn't available on Windows, and this
// avoids the external-tool dependency on Linux too.
// Usage: bin2c <input-file> <output.cpp> <symbol-name>
#include <cstdio>
#include <cstdint>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <input-file> <output.cpp> <symbol-name>\n", argv[0]);
        return 1;
    }
    FILE* in = fopen(argv[1], "rb");
    if (!in) {
        fprintf(stderr, "bin2c: could not open %s\n", argv[1]);
        return 1;
    }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        data.insert(data.end(), buf, buf + n);
    fclose(in);

    FILE* out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "bin2c: could not open %s for writing\n", argv[2]);
        return 1;
    }
    const char* sym = argv[3];
    fprintf(out, "unsigned char %s[] = {\n", sym);
    for (size_t i = 0; i < data.size(); ++i) {
        fprintf(out, "0x%02x,%s", data[i], (i % 12 == 11) ? "\n" : " ");
    }
    fprintf(out, "\n};\nunsigned int %s_len = %zu;\n", sym, data.size());
    fclose(out);
    return 0;
}
