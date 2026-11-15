import os, shutil, struct, sys

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cache')
ENTRY = 'pc-9d26d6e9f730bb70.bin'

def header_fields(path):
    with open(path, 'rb') as f:
        magic = f.read(8)
        version, sig_len = struct.unpack('<II', f.read(8))
        sig = f.read(sig_len)
        (packed_size,) = struct.unpack('<I', f.read(4))
        f.seek(packed_size, 1)
        base = 8 + 4 + 4 + sig_len + 4 + packed_size
        f.seek(base)
        size_main, size_drft, n_ckpt = struct.unpack('<QQI', f.read(20))
    return dict(version=version, sig_len=sig_len, packed_size=packed_size, base=base,
                size_main=size_main, size_drft=size_drft, n_ckpt=n_ckpt)

H = header_fields(os.path.join(BASE, ENTRY))
FIXED_TAIL = 60  # size_main..crc_packed
CKPT_ENTRY = 64  # n_tokens,pos_min,pos_max + 6 x u64
base = H['base']
off_last_used = base + 28
off_crc_main  = base + 36
off_crc_drft  = base + 44
off_crc_packed= base + 52
ckpt_table = base + FIXED_TAIL
main_start = ckpt_table + H['n_ckpt'] * CKPT_ENTRY
print(f"signature={H['sig_len']} packed={H['packed_size']} n_ckpt={H['n_ckpt']} size_main={H['size_main']}")
print(f"base={base} last_used={off_last_used} crc_main={off_crc_main} crc_drft={off_crc_drft} crc_packed={off_crc_packed}")
print(f"ckpt_table={ckpt_table} main_start={main_start}")

def fresh(name):
    dst = os.path.join(os.path.dirname(BASE), name)
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(BASE, dst)
    return os.path.join(dst, ENTRY), os.path.join(dst, 'pc-index.dat')

def poke(path, offset, value):
    with open(path, 'r+b') as f:
        f.seek(offset)
        f.write(value)

def flip(path, offset):
    with open(path, 'r+b') as f:
        f.seek(offset)
        b = f.read(1)
        f.seek(offset)
        f.write(bytes([b[0] ^ 0xFF]))

TS = struct.pack('<Q', 0x00000000671DB480)

# 1. what the FIXED touch_entry writes: last_used, 8 bytes, correct offset
p, _ = fresh('case-correct')
poke(p, off_last_used, TS)

# 2. what the OLD formula wrote into a v7 header: crc_drft
p, _ = fresh('case-v7off')
poke(p, off_crc_drft, TS)

# 3. what the OLD formula writes into a v8 header: crc_packed
p, _ = fresh('case-v8off')
poke(p, off_crc_packed, TS)

# 4. one bit flipped inside the packed token metadata
p, _ = fresh('case-packedflip')
flip(p, base - H['packed_size'] + H['packed_size'] // 2)

# 5. one bit flipped inside the main KV payload
p, _ = fresh('case-mainflip')
flip(p, main_start + H['size_main'] // 2)

# 6. corrupted startup index body (torn write / bad sector)
p, idx = fresh('case-indexcorrupt')
size = os.path.getsize(idx)
with open(idx, 'r+b') as f:
    f.seek(size // 2)
    b = f.read(1)
    f.seek(size // 2)
    f.write(bytes([b[0] ^ 0xFF]))

print("cases prepared")
