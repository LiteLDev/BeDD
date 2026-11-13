add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

set_policy("package.requires_lock", true)

-- BeDD is a test-only mod. target_type=client builds the mod that drives a game (bin\BeDD);
-- target_type=server the one a Bedrock Dedicated Server runs in the client+server mode
-- (bin\BeDD-server). The levilamina package and the levibuildscript rules read the option too.
option("target_type")
    set_default("client")
    set_showmenu(true)
    set_values("client", "server")
option_end()

add_requires("levilamina 26.60.7", {configs = {target_type = get_config("target_type")}})
add_requires("levibuildscript")
add_requires("nlohmann_json v3.12.0")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

-- The mod packer names the mod and its bin\ folder after the target.
target(is_config("target_type", "server") and "BeDD-server" or "BeDD")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker")
    set_kind("shared")
    set_languages("c++20")
    set_toolchains("clang-cl")
    set_symbols("debug")
    set_exceptions("none") -- To avoid conflicts with /EHa.
    add_defines("NOMINMAX", "UNICODE", "WIN32_LEAN_AND_MEAN", "_HAS_CXX23=1")
    add_cxflags("/utf-8", "/W4", "/EHa")
    add_cxflags("/EHs", {tools = {"clang_cl"}})
    add_cxflags(
        "-Wno-microsoft-cast",
        "-Wno-invalid-offsetof",
        "-Wno-c++2b-extensions",
        "-Wno-microsoft-include",
        "-Wno-overloaded-virtual",
        "-Wno-ignored-qualifiers",
        "-Wno-missing-field-initializers",
        "-Wno-potentially-evaluated-expression",
        "-Wno-pragma-system-header-outside-header",
        {tools = {"clang_cl"}}
    )
    add_packages("levilamina", "nlohmann_json")
    add_syslinks("user32", "gdi32", "ws2_32", "ole32", "oleaut32", "uuid", "windowscodecs", "bcrypt")
    add_includedirs("src")
    add_headerfiles("src/**.h")
    if is_config("target_type", "server") then
        -- No game window, input, audio, tap or manager on a server: the WebSocket server, the loopback-only
        -- network (no firewall prompts) and the server units only.
        add_files("src/Mod.cpp", "src/Pipe.cpp", "src/WsServer.cpp", "src/Discovery.cpp", "src/MemoryOperators.cpp", "src/NetHooks.cpp", "src/ServerTimeScale.cpp")
    else
        add_files("src/**.cpp")
        remove_files("src/ServerTimeScale.cpp")
    end
