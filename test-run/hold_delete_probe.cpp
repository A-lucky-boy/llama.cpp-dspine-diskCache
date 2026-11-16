// Can a file be deleted while a handle to it is open?
//
// If not, a second process holding a cache entry open is a real, code-free way to make
// fs::remove fail - which is the condition the quarantine path exists for (F01). That
// matters because the plan requires the failure to be injected without putting a
// triggerable fault switch into the production build.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

int main() {
    const std::string dir = "test-run/hold_probe";
    std::error_code ec_clean;
    fs::remove_all(dir, ec_clean);
    fs::create_directories(dir);

    const std::string path = dir + "/entry.bin";

    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "payload";
    }

    // hold it open for reading in this process, then try to delete it
    std::ifstream held(path, std::ios::binary);
    std::printf("open for read: %d\n", (int) held.is_open());

    std::error_code ec;
    const bool removed = fs::remove(path, ec);

    std::printf("remove while open-for-read : removed=%d ec=%s\n",
                (int) removed, ec ? ec.message().c_str() : "none");
    std::printf("file still exists          : %d\n", (int) fs::exists(path));

    held.close();

    std::error_code ec2;
    fs::remove(path, ec2);
    std::printf("after release, exists      : %d\n", (int) fs::exists(path));

    std::error_code ec3;
    fs::remove_all(dir, ec3);
    return 0;
}
