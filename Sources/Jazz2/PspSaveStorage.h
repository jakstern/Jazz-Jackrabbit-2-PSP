#pragma once

// ============================================================================
//  PSP save/load storage backend - IMPLEMENTATION MAP
// ----------------------------------------------------------------------------
//  The save/load SPLIT the task asks for is already in place in this fork:
//
//    * Save-FORMAT / game-state serialization  ->  PRESERVED and COMPILED.
//      The whole resumable-state format is engine code and is built into the
//      PSP EBOOT unchanged:
//        - IResumable                          (IResumable.h)
//        - LevelHandler::SerializeResumableToStream / Initialize(Stream&, ver)
//        - Player / TileMap / EventMap ::SerializeResumableToStream
//      `Initialize(Stream& src, std::uint16_t version)` carries the save
//      VERSION, so migration logic has a home. None of this is stubbed.
//
//    * Config / progress persistence           ->  PRESERVED and COMPILED.
//      PreferencesCache (Initialize/Save, GetDirectory) reads & writes
//      "Jazz2.config" through Death::IO::FileStream on the memory stick.
//
//    * STORAGE backend for RESUMABLE SESSIONS  ->  App/main.cpp.
//      PspSavedataManager serializes the running LevelHandler through the engine's
//      existing version-4 Deflate stream and presents it with the SDK LISTSAVE /
//      LISTLOAD utility modes. Five GAME000x slots are available. Loaded bytes are
//      validated and passed to LevelHandler::Initialize(Stream&, version).
//
//  The fixed PROFILE savedata entry is deliberately separate: it stores volumes,
//  progression and recent highscores, and is silently autoloaded at boot. It must
//  never be offered as one of the resumable-game slots.
// ============================================================================
