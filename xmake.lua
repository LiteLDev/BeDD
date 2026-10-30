add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

set_policy("package.requires_lock", true)

-- BeDD is a client-only, test-only mod. The option exists because the levilamina
-- package and levibuildscript rules read it.
option("target_type")
    set_default("client")
    set_showmenu(true)
    set_values("client")
option_end()

add_requires("levilamina 26.51.7", {configs = {target_type = "client"}})
add_requires("levibuildscript")
add_requires("nlohmann_json v3.12.0")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("BeDD")
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
    add_syslinks("user32", "ws2_32", "ole32", "uuid")
    add_includedirs("src")
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    after_build(function (target)
        -- The PowerShell client and harness shim ship next to the mod, so a harness only needs bin\BeDD.
        local bindir = path.join(os.projectdir(), "bin", target:name())
        os.mkdir(bindir)
        os.cp(path.join(os.projectdir(), "client", "*"), bindir)
    end)
