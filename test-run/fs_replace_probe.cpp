// Atomic-replacement probe for the quarantine record's update path.
//
// The record is replaced by renaming a fully written temporary over the target. Whether
// that is actually atomic - and when it fails - is a property of the filesystem, not of
// the code, so it is measured here rather than assumed from the POSIX behaviour of
// rename().
//
// Build and run (Windows, repository root, vcvars64 loaded):
//
//   cl /nologo /std:c++17 /EHsc /utf-8 /O2 /MD /Fe:test-run\fs_replace_probe.exe ^
//      test-run\fs_replace_probe.cpp
//   test-run\fs_replace_probe.exe
//
// What it establishes on this machine:
//
//   * renaming over an existing target succeeds and replaces its contents
//   * renaming onto a target that is still open for reading FAILS with
//     "Access is denied"
//
// The second result is the one that matters. The reader that upgrades a legacy record
// writes the same file it has just read, so if its stream were still alive the upgrade
// would fail every time. That is why the read is scoped to close before the write.
//
// The probe exits non-zero if the replacement stops working, so it can be kept as a
// regression check for the assumption the writer depends on.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int g_failures = 0;

#define REQUIRE(cond, what) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s\n", what); \
        g_failures++; \
    } \
} while (0)

static void write_file(const std::string & p, const std::string & s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

static std::string read_file(const std::string & p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main() {
    const std::string dir = "test-run/fs_replace_probe";

    std::error_code ec_rm;
    fs::remove_all(dir, ec_rm);

    fs::create_directories(dir);

    // 1. the operation the writer relies on: replace an existing file in one step
    {
        write_file(dir + "/target.txt", "OLD");
        write_file(dir + "/staged.txt", "NEW");

        std::error_code ec;
        fs::rename(dir + "/staged.txt", dir + "/target.txt", ec);

        REQUIRE(!ec, "rename over an existing target must succeed");
        REQUIRE(read_file(dir + "/target.txt") == "NEW", "the target must hold the new contents");
        REQUIRE(!fs::exists(dir + "/staged.txt"), "the staged file must be gone after the rename");

        std::printf("replace existing   : %s\n", ec ? ec.message().c_str() : "ok");
    }

    // 2. creating the target when it does not exist yet
    {
        write_file(dir + "/fresh_in.txt", "A");

        std::error_code ec;
        fs::rename(dir + "/fresh_in.txt", dir + "/fresh.txt", ec);

        REQUIRE(!ec, "rename onto an absent target must succeed");
        REQUIRE(read_file(dir + "/fresh.txt") == "A", "the new file must hold the contents");

        std::printf("create target      : %s\n", ec ? ec.message().c_str() : "ok");
    }

    // 3. the case that dictates the reader's structure: a target still open for reading
    //    cannot be replaced. This is why load_quarantine() closes its stream before the
    //    migration writes the file back
    {
        write_file(dir + "/open.txt", "OLD");

        std::ifstream held(dir + "/open.txt", std::ios::binary);
        REQUIRE(held.is_open(), "the probe must actually hold the file open");

        write_file(dir + "/staged2.txt", "NEW2");

        std::error_code ec;
        fs::rename(dir + "/staged2.txt", dir + "/open.txt", ec);

        REQUIRE(ec, "replacing an open target is expected to fail on this platform");
        REQUIRE(ec == std::errc::permission_denied,
                "the failure is expected to be access-denied, which a retry cannot fix");

        std::printf("replace while open : %s (expected failure)\n", ec.message().c_str());
    }

    // 4. and the same replacement succeeds once the handle is released, which is what
    //    scoping the read achieves
    {
        write_file(dir + "/closed.txt", "OLD");

        {
            std::ifstream r(dir + "/closed.txt", std::ios::binary);
            REQUIRE(r.is_open(), "read must happen");
        } // scope ends: the handle is released here

        write_file(dir + "/staged3.txt", "NEW3");

        std::error_code ec;
        fs::rename(dir + "/staged3.txt", dir + "/closed.txt", ec);

        REQUIRE(!ec, "the replacement must succeed once the reader has closed");
        REQUIRE(read_file(dir + "/closed.txt") == "NEW3", "and the new contents must be in place");

        std::printf("replace after close: %s\n", ec ? ec.message().c_str() : "ok");
    }

    std::error_code ec_cleanup;
    fs::remove_all(dir, ec_cleanup);

    if (g_failures == 0) {
        std::printf("all atomic-replacement checks passed\n");
        return 0;
    }

    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
