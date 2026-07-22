#pragma once

// The QBE backend, embedded in simplec (src/qbe/, MIT — see src/qbe/LICENSE).
// Compiles a QBE IL file to assembly. `target` selects the backend
// (e.g. "amd64_apple", "arm64_apple"); NULL or "" means the host.
// Returns 0 on success.
extern "C" int qbe_compile(const char* inpath, const char* outpath, const char* target);
