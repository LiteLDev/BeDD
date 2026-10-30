#pragma once
// Between Tap.cpp (the tap pipe) and NetTap.cpp (the network hooks that feed it).
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace bedd::tap {

/// Hook the client's NetworkSystem/NetworkConnection (remote link capture). Returns false if a hook
/// could not be installed (the rest stay installed; hooks() tells which).
bool installNetTap();
void uninstallNetTap();

/// {name: installed} of the network hooks.
[[nodiscard]] nlohmann::json netTapHooks();
/// {state: "up"|"down", type: "remote"|"local", address?} of the client's link to its server.
[[nodiscard]] nlohmann::json netTapLink();
/// Game packets captured so far: clientbound (in) and echoed serverbound (out).
[[nodiscard]] uint64_t netTapIn();
[[nodiscard]] uint64_t netTapOut();

/// Tap.cpp: whether a captured packet is taken at all (a reader is attached, or another sink in
/// the gEmit chain wants every frame). Lets the hooks skip copying it otherwise.
[[nodiscard]] bool frameWanted();

} // namespace bedd::tap
