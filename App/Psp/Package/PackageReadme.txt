Jazz Jackrabbit 2 - PSP
=======================

An unofficial homebrew port of Jazz Jackrabbit 2 for the Sony PSP.

This folder is the complete engine, ready to copy to a Memory Stick. It does
NOT contain game data - you supply that yourself from a legally owned copy of
Jazz Jackrabbit 2 (retail, the GOG release, or the free v1.23 Shareware).

Jazz Jackrabbit 2 is copyright Epic MegaGames, Inc. (now Epic Games, Inc.). This
port is unofficial, fan-made, and not affiliated with or endorsed by Epic Games.


WHAT'S IN HERE
--------------
    EBOOT.PBP       The PSP engine.
    Content/        Engine data (metadata, animations, translations). This is
                    NOT game data - it ships with the engine and must sit beside
                    EBOOT.PBP or the game will not start.
    Source/         Empty. Put your original Jazz Jackrabbit 2 files here.
    Cache/          Empty. Converted, PSP-ready assets end up here - either
                    baked on the PSP from Source/, or pre-baked on a PC.
    README.txt      This file.
    LICENSE.txt     The engine's license (GNU GPL v3).


REQUIREMENTS
------------
A PSP with custom firmware (CFW) - a homebrew-enabled console.


INSTALL
-------
1. Copy this whole folder to the Memory Stick under PSP/GAME/, so you get:
       ms0:/PSP/GAME/JAZZ2-PSP/EBOOT.PBP
       ms0:/PSP/GAME/JAZZ2-PSP/Content/
       ms0:/PSP/GAME/JAZZ2-PSP/Source/
       ms0:/PSP/GAME/JAZZ2-PSP/Cache/
   You may rename the JAZZ2-PSP folder to anything you like. Copying EBOOT.PBP
   on its own will NOT work - Content/ is required.

2. Provide the game data. Choose ONE of the two options below.

   OPTION A - let the PSP do it (PSP-2000/3000)
       Copy every file from your Jazz Jackrabbit 2 installation into Source/:
           Source/Anims.j2a      (retail)  or  AnimsSw.j2a  (shareware)
           Source/Data.j2d
           Source/*.j2l, *.j2t, *.j2e, *.j2b, ...
       Launch the game. It converts Source/ into Cache/ on first start and shows
       progress. Keep the console powered and the Memory Stick inserted.
       The full retail game takes roughly 30-40 minutes this way.

   OPTION B - pre-bake on a PC (required on PSP-1000, faster everywhere)
       Run the psp-bake tool on Windows or Linux; it finishes in few seconds:
           psp-bake --source "path/to/Jazz Jackrabbit 2/Source" --output .
       Copy the resulting Cache folder over the empty Cache/ in this package.
       Source/ can then stay empty.

   PSP-1000: on-device baking is DISABLED. The original 32 MB console does not
   have enough memory to convert the JJ2 files, so the game refuses instead of
   failing part-way. Use OPTION B.

3. On the PSP: Game -> Memory Stick -> launch "Jazz Jackrabbit 2".


MULTIPLAYER
-----------
    * PSP <-> PSP:    both consoles on the same ad-hoc network. Either console
      can host.
    * PSP <-> PPSSPP: on the PSP choose Options -> Network: PPSSPP; the PPSSPP
      side uses its normal Ad-Hoc networking. PPSSPP MUST host - in this mode
      the PSP can only join a game hosted by someone else.
A host cannot start a match alone - at least one other player must be ready.


CREDITS AND LICENSE
-------------------
    * Jazz Jackrabbit 2 (the original game) is copyright Epic MegaGames, Inc.,
      now Epic Games, Inc. This package contains NO original game data - no art,
      audio, levels, or other assets from the game. You supply those yourself
      from a copy you obtained legally.
    * Based on Jazz2 Resurrection by Dan R. (@deathkiller):
          https://github.com/deathkiller/jazz2-native
    * PSP port by jakstern, 2026.
    * The engine is free software under the GNU General Public License v3; the
      full text is in LICENSE.txt. That license covers the engine and Content/
      only - never Epic's game data.
