#pragma once
// The `screenshot` op (input mode): the game's client area, captured from inside the game
// process and encoded as PNG or JPEG. Transport-agnostic: takes the request, returns the reply.
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace bedd {

/// {op:"screenshot", format?:"png"|"jpeg", quality?:1..100, maxWidth?:px (0 = full size),
///  region?:{x,y,w,h} | {rx,ry,rw,rh}} -> {ok:true, width, height, format, bytes, data:"<base64>", region}
/// or {ok:false, error, code:"bad_args"|"unavailable"}. Call it from a connection thread (it waits
/// up to timeoutMs, default 5000, for a helper thread), never from the game thread, which has to
/// answer the capture's window messages. Takes no BeDD lock.
[[nodiscard]] nlohmann::json screenshotOp(nlohmann::json const& req);

/// Standard base64 with padding.
[[nodiscard]] std::string base64Encode(std::string_view in);

} // namespace bedd
