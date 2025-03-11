"""
Drawing Text to an Image
++++++++++++++++++++++++

Example of using the blf module. For this module to work we
need to use the image buffer module :mod:`imbuf` as well.
"""

import blf
import imbuf

image_size = 512, 512
font_size = 20

ibuf = imbuf.new(image_size)

font_id = blf.load("/path/to/font.ttf")

with blf.bind_imbuf(font_id, ibuf, display_name="sRGB"):
    blf.color(font_id, 1.0, 1.0, 1.0, 1.0)
    blf.enable(font_id, blf.WORD_WRAP)
    blf.size(font_id, font_size)
    blf.position(font_id, 0, image_size[0] - font_size, 0)
    blf.word_wrap(font_id, image_size[0])
    blf.draw(font_id, "Lots of wrapped text. " * 50)

ibuf.filepath = "/path/to/image.png"
imbuf.write(ibuf)
