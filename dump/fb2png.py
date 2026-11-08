import sys
from PIL import Image

W, H = 1024, 600
path = r"E:\ms5351m\dump\fb.bin"
out = r"E:\ms5351m\dump\fb.png"

data = open(path, "rb").read()
assert len(data) == W * H * 2, len(data)

img = Image.new("RGB", (W, H))
px = img.load()
for y in range(H):
    row = y * W * 2
    for x in range(W):
        v = data[row + x * 2] | (data[row + x * 2 + 1] << 8)
        r = ((v >> 11) & 0x1F) * 255 // 31
        g = ((v >> 5) & 0x3F) * 255 // 63
        b = (v & 0x1F) * 255 // 31
        px[x, y] = (r, g, b)

img.save(out)
print("saved", out)
