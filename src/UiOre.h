#pragma once

#include <nlohmann/json_fwd.hpp>

namespace bedd {

/// The `oreui` op (proof of concept, mode `ui`): a read-only snapshot of the DOM of the Ore UI view
/// (Coherent Gameface), taken by a script run inside the view's own JavaScript context.
/// {op:"oreui", maxNodes?, maxText?, url?, timeoutMs?} -> {ok, url, width, height, nodes, truncated}
/// or {ok:false, code:"unsupported"|"not_ready", error, findings}. See UiOre.cpp.
[[nodiscard]] nlohmann::json oreUiOp(nlohmann::json const& req);

} // namespace bedd
