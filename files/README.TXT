DimOS 14 - a pocket phone OS on bare metal

The i686 protected-mode kernel drives a VBE linear framebuffer and draws
an 844x390 phone screen (an iPhone 14 held sideways). The default panel
is 2532 x 1170 - exactly 3x the canvas, with no letterbox at all. Four
phone profiles are built in: IPHONE14 (1170x2532, 460 ppi, 60 Hz),
IPHONE15PRO (1179x2556, 120 Hz), 1.5K (1220x2712, 120 Hz) and FHD
(1080x2400, 60 Hz). Any landscape VBE mode from 640x400 to 4K works.

Pick a panel with DIMOS_PHONE=iphone15pro ./run-linux.sh, with the
"Change panel" button in Control Centre or Settings, or in Terminal:
  PANEL           show the current profile and the list of slots
  PANEL 1.5K      switch to that phone panel (saved to CONFIG.CFG)

The phone boots locked. Tap, press Enter, or click the round Home button
at the bottom to unlock. The desktop is deliberately plain: just a
vertical gradient and a 5x2 grid of icons with their names underneath.
No dock, no page dots, no window frames, no gloss. At the top there are
only the clock, a small "Google" text button and the battery percent.
Tap "Google", type a query and press Enter (there is no network stack).
Apps open full screen.

Bottom navigation: triangle = Back, circle = Home, square = Overview
with the three most recently opened apps. Esc also goes home. A tap in
the top right corner opens Control Centre: Wi-Fi, Bluetooth, Airplane
and Focus, brightness and volume, Now playing, theme, panel and Lock.

The nine apps are Files, Music, Calculator, Paint, Snake, Mines, Cheesy
Balls, Terminal and Settings. Settings has two tabs: THIS PHONE with
live machine facts, and DISPLAY & PANEL with resolution, points, PPI
and dpi, @3x scale, 19.5:9, refresh rate and safe-area insets.

Cheesy Balls keeps the mascot fed: NYAM eats a cheese ball with ketchup,
twelve of them earn one big BURP, Cook more fries a refill.

This phone remembers: theme, brightness, volume and the chosen panel
live in CONFIG.CFG (version 2) on the IDE disk. Terminal writes real
files to that disk:
  DISK          the drive DimOS found and how full it is
  SAVE NOTES.TXT text to keep
  LOAD NOTES.TXT
  DEL NOTES.TXT
  FORMAT YES    write a fresh FAT12 volume

DIR lists the real FAT12 files on this disk.
TYPE README.TXT reads this file.
DEL removes only a user file for the current session.
KERNEL.BIN is protected so deleting files can never stop DimOS booting.
