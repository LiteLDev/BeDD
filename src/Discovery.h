#pragma once
// Discovery (Discovery.cpp): run\<pid>.json tells BeDD.exe (and any client) where this process's
// WebSocket server listens and with which token.
#include <string>

namespace bedd {

/// Writes %LOCALAPPDATA%\BeDD\run\<pid>.json (root: env BEDD_HOME) atomically (a temporary file
/// renamed over it). False (logged) when it could not.
bool writeDiscovery(int port, std::string const& token);
/// Deletes the file writeDiscovery wrote (at shutdown).
void deleteDiscovery();

/// The instance: the folder of the process's exe (a game clone, or a server folder).
[[nodiscard]] std::string instanceName();
/// Who started the process (env BEDD_LABEL), or "".
[[nodiscard]] std::string label();
/// This mod's version from its manifest, or "".
[[nodiscard]] std::string modVersion();

} // namespace bedd
