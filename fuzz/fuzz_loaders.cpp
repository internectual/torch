// libFuzzer harness: throw the same untrusted blob at every binary parser.
//
// The point is to prove the hardening caps hold: no crash, no hang, and no
// unbounded allocation regardless of input. DIF/DTS/GLB/DSO/VOL all parse
// attacker-controlled bytes; their reserve/resize counts, bounds-checked
// readers and OOB guards must keep this safe.
//
// Build with: fuzz/build.sh   (clang++ -fsanitize=fuzzer,address,undefined)
// Run with:   fuzz/fuzz_loaders [-runs=N | -max_total_time=S] [corpus_dir]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <string>

#include "render/dif_loader.h"
#include "render/dts_loader.h"
#include "render/glb_loader.h"
#include "script/dso_reader.h"
#include "fs/vol_archive.h"
#include "fs/vl2_archive.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;

    // DIF interior parser. skipGpu=true => no GL upload (we only test bounds).
    {
        DIFLoadResult r = loadDIF(data, size, "fuzz", /*skipGpu=*/true);
        (void)r;
    }

    // DTS shape parser.
    {
        DTSLoadResult r = loadDTS(data, size, "fuzz");
        (void)r;
    }

    // GLB model parser. Texture::load / MeshData::upload are stubbed in this
    // headless harness, so the parse loops (vertex/index/accessor, JSON,
    // animations) run fully without a GL context.
    {
        GLBMesh r = loadGLB(data, size);
        (void)r;
    }

    // DSO bytecode reader.
    {
        DSOReader reader;
        DSOFile f;
        reader.read(data, size, f);
    }

    // VOL archive: dump blob to a temp file, open it (parses header + clamps
    // every entry size), then enumerate and read back each entry.
    {
        char tmpl[] = "/tmp/torch_fuzz_vol_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) {
            size_t off = 0;
            while (off < size) {
                ssize_t n = write(fd, data + off, size - off);
                if (n <= 0) break;
                off += (size_t)n;
            }
            close(fd);
            VolArchive vol;
            if (vol.open(tmpl)) {
                std::vector<std::string> names;
                vol.listFiles("*", names);
                for (const auto& nm : names) {
                    std::vector<uint8_t> out;
                    vol.readFile(nm.c_str(), out);
                }
            }
            unlink(tmpl);
        }
    }

    // VL2 archive (zlib-backed): same file-based approach. inflate() on
    // untrusted compressed bytes is exactly what fuzzing is good at.
    {
        char tmpl[] = "/tmp/torch_fuzz_vl2_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) {
            size_t off = 0;
            while (off < size) {
                ssize_t n = write(fd, data + off, size - off);
                if (n <= 0) break;
                off += (size_t)n;
            }
            close(fd);
            Vl2Archive vl2;
            if (vl2.open(tmpl)) {
                std::vector<std::string> names;
                vl2.listFiles("*", names);
                for (const auto& nm : names) {
                    std::vector<uint8_t> out;
                    vl2.readFile(nm.c_str(), out);
                }
            }
            unlink(tmpl);
        }
    }

    return 0;
}
