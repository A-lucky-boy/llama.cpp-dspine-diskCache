#include "server-disk-cache-quarantine.h"

#include <cstring>

// CRC64-ECMA (reflected polynomial 0xC96C5795D7870F42), the same function the entry
// payloads use. Kept local rather than shared because the record must stay readable
// even if the storage code is reorganised: the format outlives the code that writes it.
static uint64_t quarantine_crc64(const uint8_t * data, size_t size) {
    static uint64_t table[256];
    static bool ready = false;

    if (!ready) {
        for (uint64_t i = 0; i < 256; ++i) {
            uint64_t crc = i;
            for (int j = 0; j < 8; ++j) {
                crc = (crc >> 1) ^ (0xC96C5795D7870F42ull & (~(crc & 1) + 1));
            }
            table[(size_t) i] = crc;
        }
        ready = true;
    }

    uint64_t crc = 0xFFFFFFFFFFFFFFFFull;

    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFFFFFFFFFFull;
}

static const char DISK_CACHE_QUARANTINE_MAGIC[8] = { 'L', 'L', 'D', 'S', 'C', 'Q', '0', '1' };
static const uint32_t DISK_CACHE_QUARANTINE_VERSION = 1;

bool disk_cache_is_entry_name(const std::string & name) {
    if (name.size() != 3 + 16 + 4) {
        return false;
    }

    if (name.compare(0, 3, "pc-") != 0 || name.compare(name.size() - 4, 4, ".bin") != 0) {
        return false;
    }

    for (size_t i = 3; i < 3 + 16; i++) {
        const char c = name[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }

    return true;
}

std::string disk_cache_quarantine_serialize(const std::set<std::string> & names) {
    std::string body;

    const auto append = [&](const void * data, size_t size) {
        body.append(reinterpret_cast<const char *>(data), size);
    };

    append(DISK_CACHE_QUARANTINE_MAGIC, sizeof(DISK_CACHE_QUARANTINE_MAGIC));
    append(&DISK_CACHE_QUARANTINE_VERSION, sizeof(DISK_CACHE_QUARANTINE_VERSION));

    const uint32_t count = (uint32_t) names.size();
    append(&count, sizeof(count));

    for (const auto & n : names) {
        const uint32_t n_len = (uint32_t) n.size();
        append(&n_len, sizeof(n_len));
        append(n.data(), n.size());
    }

    const uint64_t crc = quarantine_crc64(reinterpret_cast<const uint8_t *>(body.data()), body.size());
    append(&crc, sizeof(crc));

    return body;
}

quarantine_parse disk_cache_quarantine_parse(const std::string & bytes, std::set<std::string> & out) {
    out.clear();

    if (bytes.size() < sizeof(DISK_CACHE_QUARANTINE_MAGIC) + 4 + 4 + 8) {
        return quarantine_parse::not_current_format;
    }

    if (memcmp(bytes.data(), DISK_CACHE_QUARANTINE_MAGIC, sizeof(DISK_CACHE_QUARANTINE_MAGIC)) != 0) {
        return quarantine_parse::not_current_format;
    }

    size_t off = sizeof(DISK_CACHE_QUARANTINE_MAGIC);

    uint32_t version = 0;
    uint32_t count   = 0;
    memcpy(&version, bytes.data() + off, 4); off += 4;
    memcpy(&count,   bytes.data() + off, 4); off += 4;

    if (version != DISK_CACHE_QUARANTINE_VERSION) {
        // a version this build does not know cannot be validated; refusing is the only
        // safe reading, since the record exists to exclude paths
        return quarantine_parse::corrupt;
    }

    uint64_t stored_crc = 0;
    memcpy(&stored_crc, bytes.data() + bytes.size() - 8, 8);

    const uint64_t real_crc = quarantine_crc64(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size() - 8);
    if (real_crc != stored_crc) {
        return quarantine_parse::corrupt;
    }

    const size_t body_end = bytes.size() - 8;

    for (uint32_t i = 0; i < count; i++) {
        if (off + 4 > body_end) {
            return quarantine_parse::corrupt;
        }

        uint32_t n_len = 0;
        memcpy(&n_len, bytes.data() + off, 4); off += 4;

        if (n_len == 0 || n_len > 256 || off + n_len > body_end) {
            return quarantine_parse::corrupt;
        }

        const std::string name(bytes.data() + off, n_len);
        off += n_len;

        if (!disk_cache_is_entry_name(name)) {
            return quarantine_parse::corrupt;
        }

        // a duplicate means the count does not describe the set
        if (!out.insert(name).second) {
            return quarantine_parse::corrupt;
        }
    }

    if (off != body_end) {
        return quarantine_parse::corrupt;
    }

    return quarantine_parse::ok;
}

quarantine_migrate disk_cache_quarantine_parse_legacy(const std::string & bytes, std::set<std::string> & out) {
    out.clear();

    if (bytes.empty()) {
        return quarantine_migrate::not_old_format;
    }

    // the current format also fails an old-format parse, so its magic is checked first
    if (bytes.size() >= sizeof(DISK_CACHE_QUARANTINE_MAGIC) &&
            memcmp(bytes.data(), DISK_CACHE_QUARANTINE_MAGIC, sizeof(DISK_CACHE_QUARANTINE_MAGIC)) == 0) {
        return quarantine_migrate::not_old_format;
    }

    for (size_t i = 0; i < bytes.size(); i++) {
        const unsigned char c = (unsigned char) bytes[i];
        if (c == 0 || (c < 0x20 && c != '\n' && c != '\r')) {
            return quarantine_migrate::not_old_format;
        }
    }

    // a file not ending on a newline may have been cut mid-write
    if (bytes.back() != '\n' && bytes.back() != '\r') {
        return quarantine_migrate::corrupt;
    }

    size_t pos = 0;

    while (pos < bytes.size()) {
        const size_t nl = bytes.find('\n', pos);
        std::string line = bytes.substr(pos, (nl == std::string::npos ? bytes.size() : nl) - pos);
        pos = (nl == std::string::npos) ? bytes.size() : nl + 1;

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            continue;
        }

        if (!disk_cache_is_entry_name(line)) {
            return quarantine_migrate::corrupt;
        }

        if (!out.insert(line).second) {
            return quarantine_migrate::corrupt;
        }
    }

    return quarantine_migrate::ok;
}
