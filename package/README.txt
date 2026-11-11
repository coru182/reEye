reEye - GoldenEye 007 (Xbox 360 / XBLA), recompiled for the PS5
================================================================

An unofficial fan port: the Xbox 360 game's code, statically recompiled into a native PS5 app. Not an emulator.
No game files are included.

WORK IN PROGRESS: this build is for testing the port, not for playing.

Install
-------
1. Unzip PPSA99070-<version>.zip on your computer. You get a folder named PPSA99070.
2. Over FTP, copy the whole PPSA99070 folder to /data/homebrew/ on the PS5.
   ShadowMountPlus adds a "reEye" tile to the home screen (give it about 30 seconds).
3. Over FTP, create the folder /data/reeye/game/ and copy the game's extracted XBLA files into it
   (default.xex and everything next to it).
4. Start the reEye tile. A log is written to /data/reeye/log.txt.

To update, copy the new PPSA99070 folder over the old one. Your files in /data/reeye/ are kept.
