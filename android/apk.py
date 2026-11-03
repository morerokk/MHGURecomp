"""apk.py base.apk out.apk classes.dex libdir: the APK aapt2 made (manifest), plus classes.dex and every
libdir/<abi>/*.so as lib/<abi>/*.so, stored uncompressed (android:extractNativeLibs="false": Android maps them
straight from the APK, which zipalign -P 16 then aligns). android/apk.mk."""
import os
import sys
import zipfile

base, out, dex, libdir = sys.argv[1:5]
with zipfile.ZipFile(base) as src, zipfile.ZipFile(out, 'w') as dst:
    for item in src.infolist():
        dst.writestr(item, src.read(item.filename))
    dst.write(dex, 'classes.dex', compress_type=zipfile.ZIP_DEFLATED)
    for abi in sorted(os.listdir(libdir)):
        for name in sorted(os.listdir(os.path.join(libdir, abi))):
            if name.endswith('.so'):
                dst.write(os.path.join(libdir, abi, name), f'lib/{abi}/{name}', compress_type=zipfile.ZIP_STORED)
