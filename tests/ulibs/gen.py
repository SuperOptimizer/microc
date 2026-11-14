#!/usr/bin/env python3
# gen.py: make the reference data of tests/ulibs with the Mac's own tools (python3 -I gen.py, run in tests/ulibs).
# Needs: python's zlib and blosc modules, and gzip, zstd, lz4, cjpeg/djpeg (libjpeg-turbo), shasum, xxh128sum on PATH.
# The outputs (data/*, expect.h) are checked in; the tests themselves need none of these tools.
import zlib, struct, subprocess, os, hashlib
D = 'data'
def lcg(seed):
    x = seed
    while True:
        x = (x * 6364136223846793005 + 1442695040888963407) & (2**64 - 1)
        yield x >> 33
g = lcg(7)
words = [b'scroll', b'papyrus', b'ink', b'fibre', b'voxel', b'sheet', b'winding', b'umbilicus', b'chunk', b'zarr']
out = bytearray()
while len(out) < 20000:                      # text
    out += words[next(g) % len(words)] + (b' ' if next(g) % 7 else b'\n')
out = out[:20000]
for i in range(5000):                        # slowly changing 32-bit integers: what shuffle is for
    out += struct.pack('<I', 100000 + i * 3 + next(g) % 5)
for i in range(5000):                        # 16-bit samples with noise
    out += struct.pack('<H', (30000 + int(2000 * ((i % 200) / 200.0)) + next(g) % 64) & 0xffff)
out += bytes(next(g) & 0xff for _ in range(6000))   # noise
out += bytes(4000)                           # zeros
plain = bytes(out)
open(f'{D}/plain.bin', 'wb').write(plain)
open(f'{D}/plain.zz', 'wb').write(zlib.compress(plain, 9))
co = zlib.compressobj(6, zlib.DEFLATED, -15); open(f'{D}/plain.deflate', 'wb').write(co.compress(plain) + co.flush())
subprocess.run(f'gzip -9 -n -c {D}/plain.bin > {D}/plain.gz', shell=True, check=True)
subprocess.run(f'zstd -q -f -19 {D}/plain.bin -o {D}/plain.zst', shell=True, check=True)
subprocess.run(f'zstd -q -f -1 --no-check -B4096 {D}/plain.bin -o {D}/plain.1.zst', shell=True, check=True)
import blosc
blosc.set_nthreads(1)
for name, kw in [('lz4', dict(cname='lz4', typesize=4, shuffle=blosc.SHUFFLE)),
                 ('lz4hc', dict(cname='lz4hc', typesize=2, shuffle=blosc.SHUFFLE, clevel=9)),
                 ('zstd', dict(cname='zstd', typesize=4, shuffle=blosc.BITSHUFFLE, clevel=5)),
                 ('zlib', dict(cname='zlib', typesize=8, shuffle=blosc.SHUFFLE)),
                 ('blosclz', dict(cname='blosclz', typesize=4, shuffle=blosc.SHUFFLE, clevel=9)),
                 ('raw', dict(cname='lz4', typesize=4, shuffle=blosc.NOSHUFFLE, clevel=0))]:
    if name in ('zstd', 'zlib'): blosc.set_blocksize(8192)       # several blocks (lz4 and blosclz frames get 64 KB blocks at least)
    else: blosc.set_blocksize(0)
    open(f'{D}/plain.{name}.blosc', 'wb').write(blosc.compress(plain, **kw))
# an image: smooth gradients, edges and a little noise
W, H = 120, 88
px = bytearray()
for y in range(H):
    for x in range(W):
        r = (x * 255) // (W - 1); gr = (y * 255) // (H - 1); b = ((x // 12 + y // 11) % 2) * 40 + 100 + next(g) % 4
        if (x - 60) ** 2 + (y - 44) ** 2 < 500: r, gr, b = 250, 240, 20
        px += bytes((r, gr, b))
open(f'{D}/img.ppm', 'wb').write(b'P6\n%d %d\n255\n' % (W, H) + bytes(px))
subprocess.run(f'cjpeg -quality 90 -outfile {D}/img.jpg {D}/img.ppm', shell=True, check=True)
subprocess.run(f'cjpeg -quality 80 -grayscale -progressive -outfile {D}/gray.jpg {D}/img.ppm', shell=True, check=True)
subprocess.run(f'djpeg -dct int -outfile {D}/img.dec.ppm {D}/img.jpg', shell=True, check=True)
subprocess.run(f'djpeg -dct int -outfile {D}/gray.dec.pgm {D}/gray.jpg', shell=True, check=True)
def tool(cmd): return subprocess.run(cmd, shell=True, check=True, capture_output=True, text=True).stdout.split()[0]
with open('expect.h', 'w') as f:
    f.write('/* made by gen.py with the host\'s tools: do not edit */\n')
    f.write('#define PLAIN_LEN %d\n' % len(plain))
    f.write('#define PLAIN_CRC32 0x%08xUL\n#define PLAIN_ADLER32 0x%08xUL\n' % (zlib.crc32(plain), zlib.adler32(plain)))
    f.write('#define PLAIN_SHA256 "%s"\n' % tool(f'shasum -a 256 {D}/plain.bin'))
    f.write('#define PLAIN_XXH128 "%s"\n' % tool(f'xxh128sum {D}/plain.bin'))
    f.write('#define HEAD300_XXH128 "%s"\n' % tool(f'head -c 300 {D}/plain.bin | xxh128sum'))
    f.write('#define HEAD100_XXH128 "%s"\n' % tool(f'head -c 100 {D}/plain.bin | xxh128sum'))
    f.write('#define HEAD9_XXH128 "%s"\n' % tool(f'head -c 9 {D}/plain.bin | xxh128sum'))
    f.write('#define IMG_W %d\n#define IMG_H %d\n' % (W, H))
    assert hashlib.sha256(plain).hexdigest() == tool(f'shasum -a 256 {D}/plain.bin')
# a certificate authority and a server certificate for "localhost" (RSA 2048, valid for a century) for the HTTPS test
if not os.path.exists(f'{D}/tls-key.der'):
    T = '../../build/test/ulibs/tlsgen'; os.makedirs(T, exist_ok=True)
    def sh(c): subprocess.run(c, shell=True, check=True, capture_output=True)
    sh(f'openssl req -x509 -newkey rsa:2048 -nodes -keyout {T}/ca.key -out {D}/tls-ca.pem -days 36500 -subj "/CN=ulibs test CA" '
       f'-addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign"')
    sh(f'openssl req -newkey rsa:2048 -nodes -keyout {T}/srv.key -out {T}/srv.csr -subj "/CN=localhost"')
    open(f'{T}/ext', 'w').write('subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n')
    sh(f'openssl x509 -req -in {T}/srv.csr -CA {D}/tls-ca.pem -CAkey {T}/ca.key -CAcreateserial -days 36500 -extfile {T}/ext -outform DER -out {D}/tls-cert.der')
    sh(f'openssl pkey -in {T}/srv.key -outform DER -out {D}/tls-key.der')
    # a second authority that signed nothing: verification against it must fail
    sh(f'openssl req -x509 -newkey rsa:2048 -nodes -keyout {T}/other.key -out {D}/tls-other-ca.pem -days 36500 -subj "/CN=some other CA" -addext "basicConstraints=critical,CA:TRUE"')
