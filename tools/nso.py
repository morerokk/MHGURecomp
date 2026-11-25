"""NSO loader for 32-bit (AArch32) modules: decompress segments, build the memory image, parse MOD0 / ELF32
dynamic info (symbols, REL relocations)."""
import struct
import sys

try:
    import lz4.block

    def lz4_block(src, size):
        return lz4.block.decompress(src, uncompressed_size=size)
except ImportError:  # (macOS's own python3, which builds the game code on a player's Mac: tools/build_gamecode.py)
    def lz4_block(src, size):
        """An LZ4 block, decompressed: literals, then a match (offset back into the output, length) per sequence."""
        dst = bytearray()
        i, n = 0, len(src)
        while i < n:
            token = src[i]
            i += 1
            lit = token >> 4
            if lit == 15:
                while True:
                    b = src[i]
                    i += 1
                    lit += b
                    if b != 255:
                        break
            dst += src[i:i + lit]
            i += lit
            if i >= n:
                break
            off = src[i] | src[i + 1] << 8
            i += 2
            length = token & 15
            if length == 15:
                while True:
                    b = src[i]
                    i += 1
                    length += b
                    if b != 255:
                        break
            length += 4
            start = len(dst) - off
            if off >= length:
                dst += dst[start:start + length]
            else:  # (overlapping: the last `off` bytes repeat)
                dst += (dst[start:] * (length // off + 1))[:length]
        if len(dst) != size:
            raise ValueError(f"LZ4 block: {len(dst)} bytes, {size} expected")
        return bytes(dst)

DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 0, 1, 2, 4, 5, 6, 7, 8
DT_STRSZ, DT_INIT, DT_FINI, DT_REL, DT_RELSZ, DT_JMPREL = 10, 12, 13, 17, 18, 23
DT_INIT_ARRAY, DT_FINI_ARRAY, DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ = 25, 26, 27, 28
R_ARM_ABS32, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_RELATIVE = 2, 21, 22, 23
STT_FUNC = 2


class Nso:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        assert raw[:4] == b'NSO0', path
        flags = struct.unpack_from('<I', raw, 0xC)[0]
        self.build_id = raw[0x40:0x60].rstrip(b'\0').hex()
        segs = []
        for i in range(3):
            file_off, mem_off, dec_size = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
            comp_size = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
            data = raw[file_off:file_off + comp_size]
            if flags & (1 << i):
                data = lz4_block(data, dec_size)
            segs.append((mem_off, data))
        self.bss_size = struct.unpack_from('<I', raw, 0x3C)[0]
        self.text = (segs[0][0], len(segs[0][1]))
        self.rodata = (segs[1][0], len(segs[1][1]))
        self.data = (segs[2][0], len(segs[2][1]))
        end = segs[2][0] + len(segs[2][1]) + self.bss_size
        self.size = (end + 0xFFF) & ~0xFFF
        img = bytearray(self.size)
        for off, d in segs:
            img[off:off + len(d)] = d
        self.image = img
        self._parse_mod0()

    def u32(self, off): return struct.unpack_from('<I', self.image, off)[0]

    def _parse_mod0(self):
        mod0 = self.u32(4)
        assert self.image[mod0:mod0 + 4] == b'MOD0'
        rel = lambda i: mod0 + struct.unpack_from('<i', self.image, mod0 + 4 + i * 4)[0]
        self.dynamic, self.bss_start, self.bss_end = rel(0), rel(1), rel(2)
        self.eh_start, self.eh_end, self.module_obj = rel(3), rel(4), rel(5)
        dyn = {}
        needed = []
        off = self.dynamic
        while True:
            tag, val = struct.unpack_from('<iI', self.image, off)
            off += 8
            if tag == DT_NULL:
                break
            if tag == DT_NEEDED:
                needed.append(val)
            dyn.setdefault(tag, val)
        self.dyn = dyn
        strtab = dyn.get(DT_STRTAB, 0)
        self.strtab = strtab
        self.needed = [self.cstr(strtab + n) for n in needed]
        self.syms = []  # (name, info, shndx, value, size)
        if DT_HASH in dyn and DT_SYMTAB in dyn:
            nchain = self.u32(dyn[DT_HASH] + 4)
            st = dyn[DT_SYMTAB]
            for i in range(nchain):
                name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', self.image, st + i * 16)
                self.syms.append((self.cstr(strtab + name), info, shndx, value, size))
        self.relocs = []  # (offset, type, symidx, addend)
        if DT_REL in dyn:
            b, n = dyn[DT_REL], dyn[DT_RELSZ] // 8
            for i in range(n):
                r_off, r_info = struct.unpack_from('<II', self.image, b + i * 8)
                self.relocs.append((r_off, r_info & 0xFF, r_info >> 8, self.u32(r_off)))
        if DT_RELA in dyn:
            b, n = dyn[DT_RELA], dyn[DT_RELASZ] // 12
            for i in range(n):
                r_off, r_info, r_add = struct.unpack_from('<IIi', self.image, b + i * 12)
                self.relocs.append((r_off, r_info & 0xFF, r_info >> 8, r_add))
        if DT_JMPREL in dyn:
            ent = 12 if dyn.get(20) == DT_RELA else 8  # DT_PLTREL
            b, n = dyn[DT_JMPREL], dyn[DT_PLTRELSZ] // ent
            for i in range(n):
                r_off, r_info = struct.unpack_from('<II', self.image, b + i * ent)
                add = struct.unpack_from('<i', self.image, b + i * ent + 8)[0] if ent == 12 else self.u32(r_off)
                self.relocs.append((r_off, r_info & 0xFF, r_info >> 8, add))

    def cstr(self, off):
        end = self.image.index(b'\0', off)
        return self.image[off:end].decode('utf-8', 'replace')

    def imports(self):
        return [s for s in self.syms if s[0] and s[2] == 0]

    def exports(self):
        return [s for s in self.syms if s[0] and s[2] != 0]


if __name__ == '__main__':
    from collections import Counter
    for p in sys.argv[1:]:
        n = Nso(p)
        print(f'== {p}  build {n.build_id[:16]}  image 0x{n.size:x}')
        print(f'  text 0x{n.text[0]:x}+0x{n.text[1]:x}  ro 0x{n.rodata[0]:x}+0x{n.rodata[1]:x}  '
              f'data 0x{n.data[0]:x}+0x{n.data[1]:x}  bss 0x{n.bss_size:x}')
        print(f'  dyn tags {sorted(n.dyn)}')
        print(f'  needed {n.needed}  syms {len(n.syms)}  imports {len(n.imports())}  exports {len(n.exports())}')
        print(f'  relocs {dict(Counter(r[1] for r in n.relocs))}')
        print(f'  eh {n.eh_start:x}-{n.eh_end:x}')
