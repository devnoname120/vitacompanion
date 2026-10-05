import pathlib
import struct
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]

PROGRAM = r"""
#include "screenshot_format.h"

#include <stdio.h>

int main(void)
{
    /* 5x2 frame: row 0 is A8B8G8R8 bytes R,G,B,A; row 1 is A2B10G10R10. */
    uint8_t row0[5 * 4];
    uint8_t row1[5 * 4];
    uint8_t out[16];
    uint8_t header[BMP_HEADER_SIZE];
    uint32_t x;
    FILE *f;

    for (x = 0; x < 5; ++x)
    {
        uint32_t r = x * 40, g = 255 - x * 40, b = 100 + x;
        uint32_t pixel = (3U << 30) | ((b * 4) << 20) | ((g * 4) << 10) |
            (r * 4);

        row0[x * 4 + 0] = (uint8_t)r;
        row0[x * 4 + 1] = (uint8_t)g;
        row0[x * 4 + 2] = (uint8_t)b;
        row0[x * 4 + 3] = 0xFF;
        row1[x * 4 + 0] = (uint8_t)pixel;
        row1[x * 4 + 1] = (uint8_t)(pixel >> 8);
        row1[x * 4 + 2] = (uint8_t)(pixel >> 16);
        row1[x * 4 + 3] = (uint8_t)(pixel >> 24);
    }

    if (bmp_row_stride(5) != 16 || bmp_row_stride(960) != 2880 ||
        !screen_pixelformat_is_supported(0) ||
        !screen_pixelformat_is_supported(0x60800000U) ||
        screen_pixelformat_is_supported(0x20000000U))
        return 1;

    f = fopen("shot.bmp", "wb");
    bmp_write_header(header, 5, 2);
    fwrite(header, 1, sizeof(header), f);
    screen_convert_row(row0, out, 5, 0);
    fwrite(out, 1, 16, f);
    screen_convert_row(row1, out, 5, 0x60800000U);
    fwrite(out, 1, 16, f);
    fclose(f);
    return 0;
}
"""


class ScreenshotFormatTests(unittest.TestCase):
    def test_writes_top_down_24_bit_bmp(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            (tmp / "test.c").write_text(textwrap.dedent(PROGRAM))
            subprocess.run(
                [
                    "cc",
                    "-std=gnu99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(tmp / "test.c"),
                    str(ROOT / "src" / "screenshot_format.c"),
                    "-o",
                    str(tmp / "test"),
                ],
                check=True,
            )
            subprocess.run([str(tmp / "test")], check=True, cwd=tmp)
            data = (tmp / "shot.bmp").read_bytes()

        self.assertEqual(data[:2], b"BM")
        size, offset, info_size, width, height, planes, bpp, compression = (
            struct.unpack("<I4xIIiiHHI", data[2:34])
        )
        self.assertEqual((size, offset, info_size), (len(data), 54, 40))
        self.assertEqual((width, height, planes, bpp, compression), (5, -2, 1, 24, 0))

        for row in range(2):
            line = data[54 + row * 16 : 54 + (row + 1) * 16]
            for x in range(5):
                b, g, r = line[x * 3 : x * 3 + 3]
                self.assertEqual((r, g, b), (x * 40, 255 - x * 40, 100 + x))
            self.assertEqual(line[15], 0)


if __name__ == "__main__":
    unittest.main()
