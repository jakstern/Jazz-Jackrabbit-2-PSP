#pragma once

// Extracts the engine-owned metadata, fonts and supplemental animations bundled in the EBOOT.
// Existing matching content is left untouched. Returns false on malformed data or Memory Stick I/O failure.
bool EnsureEmbeddedPspContent();
