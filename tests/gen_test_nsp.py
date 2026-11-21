#!/usr/bin/env python3
"""Makes a pair of synthetic NSP files for tests/run_nsp_test.sh: a "base game" and an "update" with the same layout as the
real ones (NSP = PFS0 of NCAs and a ticket; NCA3 header in AES-XTS; ExeFS PFS0 and RomFS sections in AES-CTR; the update's
RomFS a BKTR patch of the base's) and made-up keys. Nothing in it is from a game or a console. It also writes what extracting
them must give (expected/exefs, expected/romfs) and the keys files for the test.

    gen_test_nsp.py OUTDIR [--seed N] [--big]

--big: a bigger RomFS (the relocation and subsection tables then have several buckets of entries, as the real update's).
Needs the `cryptography` package."""
import os
import random
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PROGRAM_ID_BASE = 0x0100770008DD8000
PROGRAM_ID_UPDATE = 0x0100770008DD8800


# ---------------------------------------------------------------- crypto
def ecb(key, data, enc):
    c = Cipher(algorithms.AES(key), modes.ECB())
    x = c.encryptor() if enc else c.decryptor()
    return x.update(data) + x.finalize()


def xts_sector(key32, data, sector):
    # Nintendo's XTS: the tweak is the sector number big-endian
    c = Cipher(algorithms.AES(key32), modes.XTS(sector.to_bytes(16, "big")))
    x = c.encryptor()
    return x.update(data) + x.finalize()


def ctr_crypt(key, hi8, gen, pos, data):
    """AES-CTR of data that sits at byte offset pos of the NCA (symmetric: encrypts and decrypts)."""
    ctr = bytearray(hi8)
    if gen is not None:
        ctr[4:8] = struct.pack(">I", gen)
    skip = pos & 15
    ctr += struct.pack(">Q", pos >> 4)
    c = Cipher(algorithms.AES(key), modes.CTR(bytes(ctr)))
    x = c.encryptor()
    return x.update(b"\0" * skip + data)[skip:]


def generate_kek(src, master, kek_seed, key_seed):
    kek = ecb(master, kek_seed, False)
    s = ecb(kek, src, False)
    return ecb(s, key_seed, False) if key_seed else s


# ---------------------------------------------------------------- containers
def pfs0(files):
    """files: list of (name, bytes) -> PFS0 bytes"""
    names = b""
    entries = b""
    off = 0
    for name, data in files:
        entries += struct.pack("<QQII", off, len(data), len(names), 0)
        names += name.encode() + b"\0"
        off += len(data)
    names += b"\0" * ((-len(names)) % 0x20)
    return b"PFS0" + struct.pack("<III", len(files), len(names), 0) + entries + names + b"".join(d for _, d in files)


def align(n, a):
    return (n + a - 1) // a * a


def build_romfs(files, table_space, data_align, rng):
    """files: list of (path, bytes) in file-table order -> RomFS image. The tables get table_space bytes in all (the update's
    tables fit in the same room, so the data stays where it was)."""
    dirs = {"": None}  # path -> index in creation order
    for p, _ in files:
        parts = p.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            dirs.setdefault("/".join(parts[:i]), None)
    dir_list = sorted(dirs, key=lambda d: (d.count("/") if d else -1, d))
    dir_off = {}
    off = 0
    for d in dir_list:
        name = d.split("/")[-1] if d else ""
        dir_off[d] = off
        off += 0x18 + align(len(name.encode()), 4)
    dir_table_size = off
    # file table order as given; names
    file_off = {}
    off = 0
    for p, _ in files:
        file_off[p] = off
        off += 0x20 + align(len(p.split("/")[-1].encode()), 4)
    file_table_size = off
    # data
    data_pos = {}
    pos = 0
    for p, d in files:
        pos = align(pos, data_align(p))
        data_pos[p] = pos
        pos += len(d)
    data_size = pos
    dir_hash = 3 * 4
    file_hash = 5 * 4
    hdr_end = 0x50
    dh_off = hdr_end
    dm_off = align(dh_off + dir_hash, 8)
    fh_off = align(dm_off + dir_table_size, 8)
    fm_off = align(fh_off + file_hash, 8)
    assert fm_off + file_table_size <= table_space, (fm_off + file_table_size, table_space)
    data_off = table_space
    img = bytearray(data_off + data_size)
    struct.pack_into("<QQQQQQQQQQ", img, 0, 0x50, dh_off, dir_hash, dm_off, dir_table_size, fh_off, file_hash,
                     fm_off, file_table_size, data_off)
    img[dh_off:dh_off + dir_hash] = b"\xff" * dir_hash
    img[fh_off:fh_off + file_hash] = b"\xff" * file_hash
    for d in dir_list:
        name = d.split("/")[-1].encode() if d else b""
        parent = d.rsplit("/", 1)[0] if "/" in d else ""
        sibs = [x for x in dir_list if x and (x.rsplit("/", 1)[0] if "/" in x else "") == (d.rsplit("/", 1)[0] if "/" in d else "")]
        nxt = sibs[sibs.index(d) + 1] if d and sibs.index(d) + 1 < len(sibs) else None
        kids = [x for x in dir_list if x and (x.rsplit("/", 1)[0] if "/" in x else "") == d]
        fl = [p for p, _ in files if (p.rsplit("/", 1)[0] if "/" in p else "") == d]
        o = dm_off + dir_off[d]
        struct.pack_into("<IIIIII", img, o, dir_off[parent] if d else 0,
                         dir_off[nxt] if nxt is not None else 0xFFFFFFFF,
                         dir_off[kids[0]] if kids else 0xFFFFFFFF,
                         file_off[fl[0]] if fl else 0xFFFFFFFF, 0xFFFFFFFF, len(name))
        img[o + 0x18:o + 0x18 + len(name)] = name
    for p, d in files:
        dpath = p.rsplit("/", 1)[0] if "/" in p else ""
        sibs = [q for q, _ in files if (q.rsplit("/", 1)[0] if "/" in q else "") == dpath]
        nxt = sibs[sibs.index(p) + 1] if sibs.index(p) + 1 < len(sibs) else None
        name = p.split("/")[-1].encode()
        o = fm_off + file_off[p]
        struct.pack_into("<IIQQII", img, o, dir_off[dpath], file_off[nxt] if nxt else 0xFFFFFFFF, data_pos[p], len(d),
                         0xFFFFFFFF, len(name))
        img[o + 0x20:o + 0x20 + len(name)] = name
        img[data_off + data_pos[p]:data_off + data_pos[p] + len(d)] = d
    return bytes(img)


# ---------------------------------------------------------------- NCA
class Keys:
    def __init__(self, rng):
        self.header_key = rng.randbytes(32)
        self.master = [rng.randbytes(16) for _ in range(4)]
        self.kek_gen = rng.randbytes(16)
        self.key_gen = rng.randbytes(16)
        self.kak_src = rng.randbytes(16)
        self.titlekek_src = rng.randbytes(16)
        self.kak = [generate_kek(self.kak_src, m, self.kek_gen, self.key_gen) for m in self.master]
        self.titlekek = [ecb(m, self.titlekek_src, False) for m in self.master]

    def write(self, path, direct):
        with open(path, "w") as f:
            f.write("; made-up keys for the importer's test\n")
            f.write("header_key = %s\n" % self.header_key.hex())
            for i, m in enumerate(self.master):
                f.write("master_key_%02x = %s\n" % (i, m.hex()))
            if direct:
                for i in range(4):
                    f.write("key_area_key_application_%02x = %s\n" % (i, self.kak[i].hex()))
                    f.write("titlekek_%02x = %s\n" % (i, self.titlekek[i].hex()))
            else:
                f.write("aes_kek_generation_source = %s\n" % self.kek_gen.hex())
                f.write("aes_key_generation_source = %s\n" % self.key_gen.hex())
                f.write("key_area_key_application_source = %s\n" % self.kak_src.hex())
                f.write("titlekek_source = %s\n" % self.titlekek_src.hex())


def fs_header(partition, fs_type, crypt, superblock, gen, secure):
    h = bytearray(0x200)
    struct.pack_into("<HBBB", h, 0, 2, partition, fs_type, crypt)
    h[8:8 + len(superblock)] = superblock
    struct.pack_into("<II", h, 0x140, gen, secure)
    return bytes(h)


def ivfc_header(level5_off, level5_size, rng):
    h = bytearray(0xE0)
    struct.pack_into("<4sIII", h, 0, b"IVFC", 0x20000, 0x20, 7)
    for i in range(6):
        off = level5_off if i == 5 else i * 0x1000
        size = level5_size if i == 5 else 0x1000
        struct.pack_into("<QQII", h, 0x10 + 0x18 * i, off, size, 14, 0)
    h[0xC0:0xE0] = rng.randbytes(0x20)
    return bytes(h)


def hdr_ctr(gen, secure):
    fsh = bytearray(0x200)
    struct.pack_into("<II", fsh, 0x140, gen, secure)
    return bytes(fsh[0x140 + 7 - j] for j in range(8))


def make_nca(rng, keys, program_id, content_type, sections, rights_id=None, titlekey=None, crypto_old=2, crypto_new=3):
    """sections: list of (fs_header_bytes, section_data_plain_or_function, hi8). The section data is encrypted here with
    AES-CTR; a BKTR section's data comes already encrypted (callable taking the section's start in the NCA)."""
    gen_idx = max(crypto_old, crypto_new)
    gen_idx = gen_idx - 1 if gen_idx else 0
    ctr_key = titlekey if rights_id else rng.randbytes(16)
    key_area = rng.randbytes(0x40)
    key_area = key_area[:0x20] + ctr_key + key_area[0x30:]
    hdr = bytearray(0xC00)
    hdr[0:0x200] = rng.randbytes(0x200)
    hdr[0x200:0x204] = b"NCA3"
    hdr[0x204] = 0
    hdr[0x205] = content_type
    hdr[0x206] = crypto_old
    hdr[0x207] = 0
    struct.pack_into("<Q", hdr, 0x210, program_id)
    hdr[0x220] = crypto_new
    if rights_id:
        hdr[0x230:0x240] = rights_id
    else:
        hdr[0x300:0x340] = ecb(keys.kak[gen_idx], key_area, True)
    body = b""
    pos = 0xC00
    for i, (fsh, data, hi8) in enumerate(sections):
        start = pos
        if callable(data):
            enc = data(start)
        else:
            data = data + b"\0" * ((-len(data)) % 0x200)
            enc = ctr_crypt(ctr_key, hi8, None, start, data)
        assert len(enc) % 0x200 == 0
        struct.pack_into("<II", hdr, 0x240 + 0x10 * i, start // 0x200, (start + len(enc)) // 0x200)
        hdr[0x400 + 0x200 * i:0x400 + 0x200 * (i + 1)] = fsh
        body += enc
        pos += len(enc)
    struct.pack_into("<Q", hdr, 0x208, pos)
    out = bytearray()
    for s in range(6):  # (NCA3: the whole header, sector 0 on)
        out += xts_sector(keys.header_key, bytes(hdr[0x200 * s:0x200 * (s + 1)]), s)
    return bytes(out) + body, ctr_key


def pfs0_section(files, rng):
    p = pfs0(files)
    pfs_off = 0x200
    sb = bytearray(0x138)
    sb[0:0x20] = rng.randbytes(0x20)
    struct.pack_into("<IIQQQQ", sb, 0x20, 0x1000, 2, 0, 0x20, pfs_off, len(p))
    data = bytearray(pfs_off + len(p))
    data[0:0x20] = rng.randbytes(0x20)
    data[pfs_off:] = p
    return bytes(sb), bytes(data)


# ---------------------------------------------------------------- the BKTR update romfs
def bucket_tree(entries, entry_size, per_node, pack, total_size, first_key):
    """entries: list of entry bytes; keys by first_key(entry bytes). Returns the table: a 0x4000 header node, then the
    buckets of 0x4000 bytes."""
    buckets = [entries[i:i + per_node] for i in range(0, len(entries), per_node)]
    node0 = bytearray(0x4000)
    struct.pack_into("<IIQ", node0, 0, 0, len(buckets), total_size)
    for i, b in enumerate(buckets):
        struct.pack_into("<Q", node0, 0x10 + 8 * i, first_key(b[0]))
    out = bytes(node0)
    for i, b in enumerate(buckets):
        end = first_key(buckets[i + 1][0]) if i + 1 < len(buckets) else total_size
        node = bytearray(0x4000)
        struct.pack_into("<IIQ", node, 0, i, len(b), end)
        for j, e in enumerate(b):
            node[0x10 + entry_size * j:0x10 + entry_size * (j + 1)] = e
        out += bytes(node)
    return out


def make_bktr_section(base_logical, new_logical, ctr_key, gen, secure, rng, chunk=0x1000):
    """The update's section 1 (data encrypted, as stored) for the patched logical section new_logical over base_logical."""
    hi8 = hdr_ctr(gen, secure)
    reloc = []  # (virt, phys, is_patch)
    patch = bytearray()
    n = len(new_logical)
    off = 0
    while off < n:
        end = min(off + chunk, n)
        same = end <= len(base_logical) and new_logical[off:end] == base_logical[off:end]
        if reloc and reloc[-1][2] == (0 if same else 1) and (same or True):
            pass  # merged below
        if same:
            if reloc and reloc[-1][2] == 0:
                pass
            else:
                reloc.append((off, off, 0))
        else:
            if reloc and reloc[-1][2] == 1:
                pass
            else:
                reloc.append((off, len(patch), 1))
            patch += new_logical[off:end]
        off = end
    patch += b"\0" * ((-len(patch)) % 0x200)
    data_end = len(patch)
    # subsections: random ranges with their own counter values
    subs = []
    pos = 0
    while pos < data_end:
        subs.append((pos, rng.getrandbits(32)))
        pos += 16 * rng.randint(1, 0x4000)
    reloc_entries = [struct.pack("<QQI", v, p, f) for v, p, f in reloc]
    rtab = bucket_tree(reloc_entries, 0x14, (0x4000 - 0x10) // 0x14, None, n, lambda e: struct.unpack_from("<Q", e)[0])
    roff = data_end
    sub_entries = [struct.pack("<QII", o, 0, c) for o, c in subs] + [struct.pack("<QII", data_end, 0, gen)]
    soff = roff + len(rtab)
    stab = bucket_tree(sub_entries, 0x10, (0x4000 - 0x10) // 0x10, None, soff, lambda e: struct.unpack_from("<Q", e)[0])
    sec_size = soff + len(stab)

    def encrypted(start):
        out = bytearray()
        for i, (o, c) in enumerate(subs):
            e = subs[i + 1][0] if i + 1 < len(subs) else data_end
            out += ctr_crypt(ctr_key, hi8, c, start + o, bytes(patch[o:e]))
        out += ctr_crypt(ctr_key, hi8, None, start + roff, rtab)
        out += ctr_crypt(ctr_key, hi8, None, start + soff, stab)
        return bytes(out)

    return encrypted, roff, len(rtab), soff, len(stab), len(reloc), len(sub_entries), sec_size


# ---------------------------------------------------------------- the whole set
def make_files(rng, big):
    def blob(n):
        return rng.randbytes(n)

    files = []
    files.append(("nativeNX/system/app_shader/AppShaderPackage.mfx", blob(0x300000 if big else 0x6000)))
    files.append(("nativeNX/big.bin", blob(0xC00000 if big else 0x30000)))
    for i in range(40):
        d = rng.choice(["nativeNX", "nativeNX/model", "nativeNX/model/em", "sound", "sound/bgm", "ui/menu"])
        files.append(("%s/f%02d.dat" % (d, i), blob(rng.choice([0, 1, 15, 16, 17, 4095, 4096, 65537, rng.randint(1, 70000)]))))
    files.append(("empty/e.bin", b""))
    return files


def main():
    args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith("--") and not (i > 1 and sys.argv[i - 1] in ("--seed", "--app-id"))]
    big = "--big" in sys.argv
    seed = 1
    if "--seed" in sys.argv:
        seed = int(sys.argv[sys.argv.index("--seed") + 1])
    out = args[0]
    app_id = PROGRAM_ID_BASE
    if "--app-id" in sys.argv:  # another game's application id (a base title id, hex)
        app_id = int(sys.argv[sys.argv.index("--app-id") + 1], 16)
    same_id = "--same-id" in sys.argv  # the update's Program NCA carrying the base game's id, as real update files do
    upd_id = app_id if same_id else app_id | 0x800
    os.makedirs(out, exist_ok=True)
    rng = random.Random(seed)
    keys = Keys(rng)
    keys.write(os.path.join(out, "prod.keys"), False)
    keys.write(os.path.join(out, "prod.direct.keys"), True)

    # ---- the base game
    base_files = make_files(rng, big)
    base_exefs = [("main", rng.randbytes(0x3000)), ("main.npdm", rng.randbytes(0x400)), ("rtld", rng.randbytes(0x500)),
                  ("sdk", rng.randbytes(0x700)), ("subsdk0", rng.randbytes(0x333))]
    TABLE_SPACE = 0x10000
    align_of = lambda p: 0x1000 if p == "nativeNX/big.bin" else 0x10
    base_img = build_romfs(base_files, TABLE_SPACE, align_of, rng)
    D = 0x4000  # where the RomFS image starts in the section (the hash levels before it)
    base_logical = rng.randbytes(D) + base_img
    gen_b, sec_b = rng.getrandbits(32), rng.getrandbits(32)
    sb0, d0 = pfs0_section(base_exefs, rng)
    fsh0 = fs_header(1, 2, 3, sb0, gen_b, sec_b)
    ivfc = ivfc_header(D, len(base_img), rng)
    fsh1 = fs_header(0, 3, 3, ivfc + b"\0" * (0x138 - len(ivfc)), gen_b, sec_b)
    base_nca, _ = make_nca(rng, keys, app_id, 0, [(fsh0, d0, hdr_ctr(gen_b, sec_b)), (fsh1, base_logical, hdr_ctr(gen_b, sec_b))],
                           crypto_old=2, crypto_new=3)

    # ---- the update: new ExeFS, some files changed in place, some added
    upd_files = list(base_files)
    big_i = [p for p, _ in upd_files].index("nativeNX/big.bin")
    bigdata = bytearray(upd_files[big_i][1])
    for c in range(0, len(bigdata) // 0x1000):
        if rng.random() < (0.5 if big else 0.2):
            bigdata[c * 0x1000:(c + 1) * 0x1000] = rng.randbytes(0x1000)
    upd_files[big_i] = (upd_files[big_i][0], bytes(bigdata))
    for k in (2, 9, 20):
        p, d = upd_files[k]
        if d:
            nd = bytearray(d)
            nd[len(nd) // 2] ^= 0xFF
            upd_files[k] = (p, bytes(nd))
    upd_files.append(("nativeNX/new/added1.bin", rng.randbytes(5000)))
    upd_files.append(("nativeNX/new/added2.bin", rng.randbytes(123)))
    upd_files.append(("update_only/readme.txt", b"hello"))
    upd_exefs = [("main", rng.randbytes(0x3100)), ("main.npdm", rng.randbytes(0x400)), ("rtld", base_exefs[2][1]),
                 ("sdk", rng.randbytes(0x700)), ("subsdk0", rng.randbytes(0x333)), ("subsdk1", rng.randbytes(77))]
    upd_img = build_romfs(upd_files, TABLE_SPACE, align_of, rng)
    new_logical = base_logical[:D] + upd_img
    new_logical = bytearray(new_logical)
    new_logical[5:9] = b"\x01\x02\x03\x04"  # (a changed hash-level byte: the first chunk comes from the patch)
    new_logical = bytes(new_logical)
    gen_u, sec_u = rng.getrandbits(32), rng.getrandbits(32)
    title_key = rng.randbytes(16)
    rights_id = rng.randbytes(8) + b"\0" * 7 + bytes([2])
    sb0u, d0u = pfs0_section(upd_exefs, rng)
    fsh0u = fs_header(1, 2, 3, sb0u, gen_u, sec_u)
    enc_fn, roff, rsize, soff, ssize, nrel, nsub, sec_size = make_bktr_section(base_logical, new_logical, title_key, gen_u, sec_u, rng)
    ivfc_u = ivfc_header(D, len(upd_img), rng)
    bk = bytearray(0x138)
    bk[0:0xE0] = ivfc_u
    struct.pack_into("<QQ4sIII", bk, 0xF8, roff, rsize, b"BKTR", 1, nrel, 0)
    struct.pack_into("<QQ4sIII", bk, 0x118, soff, ssize, b"BKTR", 1, nsub, 0)
    fsh1u = fs_header(0, 3, 4, bytes(bk), gen_u, sec_u)
    upd_nca, _ = make_nca(rng, keys, upd_id, 0, [(fsh0u, d0u, hdr_ctr(gen_u, sec_u)), (fsh1u, enc_fn, None)],
                          rights_id=rights_id, titlekey=title_key, crypto_old=2, crypto_new=2)
    # (a Meta NCA and a ticket, as an NSP has: the importer has to skip the one and use the other)
    meta_nca, _ = make_nca(rng, keys, upd_id, 1, [], crypto_old=2, crypto_new=2)
    enc_tk = ecb(keys.titlekek[1], title_key, True)
    tik = bytearray(0x2C0)
    struct.pack_into("<I", tik, 0, 0x10004)
    body = 0x140
    tik[body:body + 0x40] = b"Root-CA00000003-XS00000020".ljust(0x40, b"\0")
    tik[body + 0x40:body + 0x50] = enc_tk
    tik[body + 0x141] = 0
    tik[body + 0x145] = 1
    tik[body + 0x160:body + 0x170] = rights_id

    def nsp(path, files):
        with open(path, "wb") as f:
            f.write(pfs0(files))

    nsp(os.path.join(out, "base.nsp"), [("%032x.nca" % rng.getrandbits(128), base_nca), ("%032x.cnmt.nca" % rng.getrandbits(128), meta_nca)])
    nsp(os.path.join(out, "update.nsp"), [("%032x.nca" % rng.getrandbits(128), upd_nca), ("%032x.cnmt.nca" % rng.getrandbits(128), meta_nca),
                                         ("%s.tik" % rights_id.hex(), bytes(tik)), ("%s.cert" % rights_id.hex(), b"cert")])
    # the NCAs alone, for the reference tool
    open(os.path.join(out, "base.nca"), "wb").write(base_nca)
    open(os.path.join(out, "update.nca"), "wb").write(upd_nca)
    open(os.path.join(out, "titlekey.enc"), "w").write(enc_tk.hex())
    # the patched RomFS section as the reference tool should see it (tests/run_nsp_test.sh compares its --raw dump)
    open(os.path.join(out, "update.section1.expected"), "wb").write(new_logical)

    # what the importer must write
    for sub, files in (("exefs", upd_exefs), ("romfs", upd_files)):
        for p, d in files:
            path = os.path.join(out, "expected", sub, p)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "wb").write(d)
    print("relocation entries %d, subsections %d, romfs files %d" % (nrel, nsub, len(upd_files)))


if __name__ == "__main__":
    main()
