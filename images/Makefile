# Makefile для папки images
# Создает и управляет TGA изображениями для DimOS

# Цвета для иконок
FILES_COLOR = #404080
SNAKE_COLOR = #008000
MINES_COLOR = #800000
PAINT_COLOR = #000080
CALC_COLOR = #808000
MUSIC_COLOR = #800080
ABOUT_COLOR = #008080
TERMINAL_COLOR = #000000

# Размеры
ICON_SIZE = 32x32
DESKTOP_SIZE = 320x200

.PHONY: all icons fonts clean

all: icons fonts

# Создать все иконки
icons: files_icon.tga snake_icon.tga mines_icon.tga paint_icon.tga \
       calc_icon.tga music_icon.tga about_icon.tga terminal_icon.tga

files_icon.tga:
	convert -size $(ICON_SIZE) xc:$(FILES_COLOR) -fill white -draw "rectangle 8,8 24,24" $@

snake_icon.tga:
	convert -size $(ICON_SIZE) xc:$(SNAKE_COLOR) -fill yellow -draw "rectangle 8,8 24,24" $@

mines_icon.tga:
	convert -size $(ICON_SIZE) xc:$(MINES_COLOR) -fill white -draw "rectangle 8,8 24,24" $@

paint_icon.tga:
	convert -size $(ICON_SIZE) xc:$(PAINT_COLOR) -fill white -draw "rectangle 8,8 24,24" $@

calc_icon.tga:
	convert -size $(ICON_SIZE) xc:$(CALC_COLOR) -fill white -draw "rectangle 8,8 24,24" $@

music_icon.tga:
	convert -size $(ICON_SIZE) xc:$(MUSIC_COLOR) -fill white -draw "rectangle 8,8 24,24" $@

about_icon.tga:
	convert -size $(ICON_SIZE) xc:$(ABOUT_COLOR) -fill white -draw "rectangle 8,8 24,24" $@

terminal_icon.tga:
	convert -size $(ICON_SIZE) xc:$(TERMINAL_COLOR) -fill white -draw "rectangle 8,8 24,24" $@

# Создать шрифты
fonts: font8x8_full.tga

font8x8_full.tga:
	python3 -c "
import struct, sys
sys.path.insert(0, '/home/user/DimOS/tools/sandbox')
from font8x8 import GLYPHS, _mirror

width = 128
height = 64
header = struct.pack('<BBBBBH', 0, 0, 2, 0, 0)
header += struct.pack('<HHHBB', 0, 0, width, height, 24, 0)

image_data = bytearray(width * height * 3)
sorted_chars = sorted(GLYPHS.keys())

for char_idx, char_code in enumerate(sorted_chars):
    row = char_idx // 16
    col = char_idx % 16
    if char_code in GLYPHS:
        pattern = GLYPHS[char_code]
        raw = bytes.fromhex(pattern)
        bitmap = bytes(_mirror(b) for b in raw)
    else:
        bitmap = bytes([0] * 8)
    for dy in range(8):
        for dx in range(8):
            if bitmap[dy] & (0x80 >> dx):
                x = col * 8 + dx
                y = row * 8 + dy
                idx = (y * width + x) * 3
                image_data[idx] = 255
                image_data[idx+1] = 255
                image_data[idx+2] = 255

with open('$@', 'wb') as f:
    f.write(header)
    f.write(image_data)
"

# Создать базовые изображения
base: desktop.tga dimos_logo.tga title.tga gradient.tga line.tga

desktop.tga:
	convert -size $(DESKTOP_SIZE) xc:black $@

dimos_logo.tga:
	convert -size $(DESKTOP_SIZE) xc:black $@

title.tga:
	python3 -c "
import struct
width = 320
height = 200
header = struct.pack('<BBBBBH', 0, 0, 2, 0, 0)
header += struct.pack('<HHHBB', 0, 0, width, height, 24, 0)
image_data = bytearray(width * height * 3)
D = [0b11100,0b10010,0b10010,0b10010,0b10010,0b10010,0b11100]
I = [0b111,0b010,0b010,0b010,0b010,0b010,0b111]
M = [0b10001,0b11011,0b10101,0b10001,0b10001,0b10001,0b10001]
O = [0b0110,0b1001,0b1001,0b1001,0b1001,0b1001,0b0110]
S = [0b01110,0b10000,0b10000,0b01100,0b00010,0b00010,0b11100]
letters = [D, I, M, O, S]
start_x = (width - len(letters) * 6) // 2
start_y = (height - 7) // 2
for li, letter in enumerate(letters):
    for dy in range(7):
        for dx in range(5):
            if letter[dy] & (1 << (4 - dx)):
                x = start_x + li * 6 + dx
                y = start_y + dy
                if 0 <= x < width and 0 <= y < height:
                    idx = (y * width + x) * 3
                    image_data[idx] = 255
                    image_data[idx+1] = 255
                    image_data[idx+2] = 255
with open('$@', 'wb') as f:
    f.write(header)
    f.write(image_data)
"

gradient.tga:
	convert -size $(DESKTOP_SIZE) gradient:blue-white $@

line.tga:
	convert -size $(DESKTOP_SIZE) xc:black -fill white -draw "line 0,0 320,200" $@

# Удалить все TGA файлы
clean:
	rm -f *.tga

# Пересоздать все
rebuild: clean all
