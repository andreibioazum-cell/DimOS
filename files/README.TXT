DimOS 14 - a pocket phone OS on bare metal

The i686 protected-mode kernel drives a VBE linear framebuffer and draws
an 844x390 phone screen (the proportions of an iPhone 14 held sideways).
Any landscape VBE mode from 640x400 to 1920x1200 works: the mode is
picked by scoring the BIOS list, then the phone canvas is scaled into it.
RGB565 and VGA compatibility fallbacks remain available.

The phone boots locked. Tap the screen, press Enter, or click the home bar
at the bottom edge to unlock. The grid of rounded icons opens the apps,
the dock holds the four favourites, and the home bar or Esc always brings
you back. A tap in the top right corner opens Control Centre: Wi-Fi,
Bluetooth, Airplane and Focus switches, brightness and volume sliders,
Now playing and the Lock screen button.

The nine apps are Files, Music, Calculator, Paint, Snake, Mines, Cheesy
Balls, Terminal and Settings. Settings shows live facts about the machine
(model, chip, VBE mode, RAM disk size, BIOS memory, file count, mouse and
the RTC clock) and switches themes or restarts the machine.

Cheesy Balls keeps the mascot fed: NYAM eats a cheese ball with ketchup,
twelve of them earn one big BURP, Cook more fries a refill.

DIR lists the real FAT12 files on this disk.
TYPE README.TXT reads this file.
DEL removes only a user file for the current session.
KERNEL.BIN is protected so deleting files can never stop DimOS booting.
