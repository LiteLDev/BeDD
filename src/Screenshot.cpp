// The `screenshot` op: what the game window shows, captured from inside the game process.
//
// This is input mode's eyes, and the most expensive thing an agent can ask for: one picture costs
// far more tokens than a `world` or `ui` reply. So the defaults are small on purpose (JPEG, quality
// 80, at most 960 px wide); ask for PNG or full size (maxWidth 0) only when the detail matters, and
// prefer a region crop to a bigger picture.
//
// Capture: PrintWindow(PW_RENDERFULLCONTENT) on the game window into a DIB, cropped to the client
// area (the window frame is not the game's). It reads the window's DWM redirection surface, so it
// works while the window is covered or off-screen (where the manager keeps it), but not while it is
// minimized. PrintWindow may send the window messages, which the game thread answers when it pumps:
// the capture runs on a helper thread and the connection gives up waiting after timeoutMs, so a
// busy or hung game thread cannot hang the connection. Never call this on the game thread.
//
// Encoding: WIC (crop -> high-quality downscale -> 24-bit BGR -> PNG/JPEG encoder -> memory stream),
// then base64 so the picture fits in a JSON text message.
#include "Screenshot.h"

#include "BeDD.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace bedd {

namespace {

using json = nlohmann::json;
using Microsoft::WRL::ComPtr;

constexpr int kDefaultQuality  = 80;
constexpr int kDefaultMaxWidth = 960; // enough to read a menu (the title screen: ~100 KB JPEG vs ~1.2 MB full-size PNG)
constexpr int kDefaultTimeout  = 5000;

json fail(char const* code, std::string const& error) {
    return {
        {"ok",    false},
        {"code",  code },
        {"error", error}
    };
}

struct BadArgs : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Unavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Region {
    bool   fractions = false; // rx/ry/rw/rh in 0..1 of the client area, else x/y/w/h in pixels
    double x = 0, y = 0, w = 0, h = 0;
};

struct Params {
    bool                  png      = false;
    int                   quality  = kDefaultQuality;
    int                   maxWidth = kDefaultMaxWidth;
    std::optional<Region> region;
};

// An integer argument (a JSON number with no fraction), or BadArgs.
int intArg(json const& j, char const* name) {
    auto const& v = j[name];
    if (!v.is_number()) throw BadArgs(std::string("'") + name + "' must be a number");
    double d = v.get<double>();
    if (d != std::floor(d) || std::abs(d) > 1e9) throw BadArgs(std::string("'") + name + "' must be an integer");
    return static_cast<int>(d);
}

double numArg(json const& j, char const* name) {
    if (!j.contains(name)) throw BadArgs(std::string("region needs '") + name + "'");
    auto const& v = j[name];
    if (!v.is_number()) throw BadArgs(std::string("region '") + name + "' must be a number");
    double d = v.get<double>();
    if (!std::isfinite(d)) throw BadArgs(std::string("region '") + name + "' must be finite");
    return d;
}

Params parse(json const& req) {
    Params p;
    if (req.contains("format")) {
        if (!req["format"].is_string()) throw BadArgs("'format' must be \"png\" or \"jpeg\"");
        auto f = req["format"].get<std::string>();
        if (f == "png") p.png = true;
        else if (f != "jpeg") throw BadArgs("unknown format '" + f + "' (png or jpeg)");
    }
    if (req.contains("quality")) {
        p.quality = intArg(req, "quality"); // validated for png too, though only JPEG uses it
        if (p.quality < 1 || p.quality > 100) throw BadArgs("'quality' must be 1..100");
    }
    if (req.contains("maxWidth")) {
        p.maxWidth = intArg(req, "maxWidth");
        if (p.maxWidth < 0) throw BadArgs("'maxWidth' must be >= 0 (0 = full size)");
    }
    if (req.contains("region") && !req["region"].is_null()) {
        auto const& r = req["region"];
        if (!r.is_object()) throw BadArgs("'region' must be {x,y,w,h} or {rx,ry,rw,rh}");
        bool px = r.contains("x") || r.contains("y") || r.contains("w") || r.contains("h");
        bool fr = r.contains("rx") || r.contains("ry") || r.contains("rw") || r.contains("rh");
        if (px == fr) throw BadArgs("'region' must be either {x,y,w,h} (pixels) or {rx,ry,rw,rh} (fractions)");
        Region g;
        g.fractions = fr;
        if (fr) {
            g.x = numArg(r, "rx"), g.y = numArg(r, "ry"), g.w = numArg(r, "rw"), g.h = numArg(r, "rh");
            constexpr double eps = 1e-9;
            if (g.x < 0 || g.y < 0 || g.w <= 0 || g.h <= 0 || g.x + g.w > 1 + eps || g.y + g.h > 1 + eps)
                throw BadArgs("region fractions must satisfy 0 <= rx, ry; rw, rh > 0; rx + rw <= 1; ry + rh <= 1");
        } else {
            for (char const* k : {"x", "y", "w", "h"}) {
                numArg(r, k);
                intArg(r, k);
            }
            g.x = r["x"].get<double>(), g.y = r["y"].get<double>(), g.w = r["w"].get<double>(), g.h = r["h"].get<double>();
            if (g.x < 0 || g.y < 0 || g.w <= 0 || g.h <= 0) throw BadArgs("region needs x, y >= 0 and w, h > 0");
        }
        p.region = g;
    }
    return p;
}

// The crop in client pixels, or BadArgs when it does not fit in a cw x ch client area.
RECT resolveRegion(std::optional<Region> const& g, int cw, int ch) {
    if (!g) return {0, 0, cw, ch};
    if (g->fractions) {
        auto x = static_cast<LONG>(std::lround(g->x * cw)), y = static_cast<LONG>(std::lround(g->y * ch));
        auto r = static_cast<LONG>(std::lround((g->x + g->w) * cw)), b = static_cast<LONG>(std::lround((g->y + g->h) * ch));
        r = std::min<LONG>(std::max(r, x + 1), cw);
        b = std::min<LONG>(std::max(b, y + 1), ch);
        x = std::min<LONG>(x, r - 1);
        y = std::min<LONG>(y, b - 1);
        return {x, y, r, b};
    }
    if (g->x + g->w > cw || g->y + g->h > ch)
        throw BadArgs(
            "region " + std::to_string(static_cast<long long>(g->w)) + "x" + std::to_string(static_cast<long long>(g->h)) + " at "
            + std::to_string(static_cast<long long>(g->x)) + "," + std::to_string(static_cast<long long>(g->y))
            + " does not fit in the " + std::to_string(cw) + "x" + std::to_string(ch) + " client area"
        );
    auto x = static_cast<LONG>(g->x), y = static_cast<LONG>(g->y);
    return {x, y, x + static_cast<LONG>(g->w), y + static_cast<LONG>(g->h)};
}

using DcPtr     = std::unique_ptr<std::remove_pointer_t<HDC>, decltype(&DeleteDC)>;
using BitmapPtr = std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)>;

// The whole window as a top-down 32-bit DIB, and where the crop sits in it.
struct Pixels {
    DcPtr          dc{nullptr, &DeleteDC};
    BitmapPtr      bitmap{nullptr, &DeleteObject};
    uint8_t const* first  = nullptr; // the crop's top-left pixel inside the DIB
    UINT           stride = 0;       // bytes per DIB row
    UINT           size   = 0;       // bytes from first to the end of the DIB
    int            w = 0, h = 0;     // the crop's size
    RECT           crop{};           // the crop in client pixels
};

// PrintWindow the whole window into a top-down 32-bit DIB and locate the crop of its client area.
Pixels capture(std::optional<Region> const& region) {
    HWND h = gameWindow();
    if (!h || !IsWindow(h)) throw Unavailable("no game window yet");
    if (IsIconic(h)) throw Unavailable("the game window is minimized (PrintWindow cannot see it)");
    RECT  wr{}, cr{};
    POINT o{0, 0};
    if (!GetWindowRect(h, &wr) || !GetClientRect(h, &cr) || !ClientToScreen(h, &o))
        throw Unavailable("cannot read the game window's geometry");
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    if (cw <= 0 || ch <= 0 || ww <= 0 || wh <= 0) throw Unavailable("the game window has no client area");
    RECT crop = resolveRegion(region, cw, ch);
    // The client area inside the window bitmap.
    int ox = o.x - wr.left, oy = o.y - wr.top;
    if (ox < 0 || oy < 0 || ox + crop.right > ww || oy + crop.bottom > wh) throw Unavailable("the client area is outside the window");

    HDC screen = GetDC(nullptr);
    if (!screen) throw Unavailable("GetDC failed");
    Pixels p;
    p.dc.reset(CreateCompatibleDC(screen));
    ReleaseDC(nullptr, screen);
    if (!p.dc) throw Unavailable("CreateCompatibleDC failed");
    HDC mem = p.dc.get();

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = ww;
    bi.bmiHeader.biHeight      = -wh; // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void*   bits               = nullptr;
    HBITMAP bmp                = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    p.bitmap.reset(bmp);
    if (!bmp || !bits) throw Unavailable("CreateDIBSection failed");
    HGDIOBJ old   = SelectObject(mem, bmp);
    BOOL    ok    = PrintWindow(h, mem, PW_RENDERFULLCONTENT);
    DWORD   error = ok ? 0 : GetLastError();
    GdiFlush();
    SelectObject(mem, old);
    if (!ok) throw Unavailable("PrintWindow failed (error " + std::to_string(error) + ")");

    p.crop   = crop;
    p.w      = crop.right - crop.left;
    p.h      = crop.bottom - crop.top;
    p.stride = static_cast<UINT>(ww) * 4;
    size_t offset = static_cast<size_t>(oy + crop.top) * p.stride + static_cast<size_t>(ox + crop.left) * 4;
    p.first       = static_cast<uint8_t const*>(bits) + offset;
    p.size        = static_cast<UINT>(static_cast<size_t>(p.stride) * wh - offset);
    return p;
}

void check(HRESULT hr, char const* what) {
    if (FAILED(hr)) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
        throw Unavailable(std::string(what) + " failed (" + buf + ")");
    }
}

// Encodes the pixels (scaled down to maxWidth if wider) with WIC. Needs COM on this thread.
std::string encode(Pixels const& px, Params const& p, int& outW, int& outH) {
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "creating the WIC factory");

    // Straight from the DIB: WIC copies the crop once, stepping by the DIB's stride.
    ComPtr<IWICBitmap> bitmap;
    check(
        factory->CreateBitmapFromMemory(
            static_cast<UINT>(px.w),
            static_cast<UINT>(px.h),
            GUID_WICPixelFormat32bppBGR, // PrintWindow leaves alpha undefined: ignore it
            px.stride,
            px.size,
            const_cast<BYTE*>(px.first),
            &bitmap
        ),
        "CreateBitmapFromMemory"
    );
    ComPtr<IWICBitmapSource> source = bitmap;

    outW = px.w;
    outH = px.h;
    if (p.maxWidth > 0 && px.w > p.maxWidth) { // never upscale
        outW = p.maxWidth;
        outH = std::max(1, static_cast<int>(std::lround(static_cast<double>(px.h) * p.maxWidth / px.w)));
        ComPtr<IWICBitmapScaler> scaler;
        check(factory->CreateBitmapScaler(&scaler), "CreateBitmapScaler");
        // High-quality cubic (Windows 10+) keeps small UI text readable; Fant is the older box filter.
        HRESULT hr = scaler->Initialize(source.Get(), static_cast<UINT>(outW), static_cast<UINT>(outH), WICBitmapInterpolationModeHighQualityCubic);
        if (FAILED(hr)) hr = scaler->Initialize(source.Get(), static_cast<UINT>(outW), static_cast<UINT>(outH), WICBitmapInterpolationModeFant);
        check(hr, "scaling the picture");
        source = scaler;
    }

    // Both encoders take 24-bit BGR natively: convert once instead of trusting each to negotiate.
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter), "CreateFormatConverter");
    check(
        converter->Initialize(source.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom),
        "converting the picture"
    );

    ComPtr<IStream> stream;
    check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "CreateStreamOnHGlobal");
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(p.png ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg, nullptr, &encoder), "CreateEncoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "initializing the encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2>         options;
    check(encoder->CreateNewFrame(&frame, &options), "CreateNewFrame");
    if (!p.png && options) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt     = VT_R4;
        v.fltVal = static_cast<float>(p.quality) / 100.0f;
        check(options->Write(1, &opt, &v), "setting the JPEG quality");
    }
    check(frame->Initialize(options.Get()), "initializing the frame");
    check(frame->SetSize(static_cast<UINT>(outW), static_cast<UINT>(outH)), "SetSize");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    check(frame->SetPixelFormat(&fmt), "SetPixelFormat");
    if (fmt != GUID_WICPixelFormat24bppBGR) throw Unavailable("the encoder does not take 24-bit BGR");
    check(frame->WriteSource(converter.Get(), nullptr), "encoding the picture");
    check(frame->Commit(), "committing the frame");
    check(encoder->Commit(), "committing the picture");

    STATSTG st{};
    check(stream->Stat(&st, STATFLAG_NONAME), "IStream::Stat");
    HGLOBAL mem = nullptr;
    check(GetHGlobalFromStream(stream.Get(), &mem), "GetHGlobalFromStream");
    auto        size = static_cast<size_t>(st.cbSize.QuadPart);
    void const* data = GlobalLock(mem);
    if (!data) throw Unavailable("GlobalLock failed");
    std::string out(static_cast<char const*>(data), size);
    GlobalUnlock(mem);
    return out;
}

// Capture + encode on the current (helper) thread. Never throws.
json shoot(Params const& p) {
    try {
        Pixels  px = capture(p.region);
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // RPC_E_CHANGED_MODE: COM is already up on this thread in the other model, which WIC is fine with.
        if (FAILED(co) && co != RPC_E_CHANGED_MODE) check(co, "CoInitializeEx");
        struct CoGuard {
            bool on;
            ~CoGuard() {
                if (on) CoUninitialize();
            }
        } coGuard{SUCCEEDED(co)};
        int         w = 0, h = 0;
        std::string bytes = encode(px, p, w, h);
        return {
            {"ok",     true                                                                                               },
            {"width",  w                                                                                                  },
            {"height", h                                                                                                  },
            {"format", p.png ? "png" : "jpeg"                                                                             },
            {"bytes",  bytes.size()                                                                                       },
            // The crop in client pixels, before scaling: maps a point in the picture back to the window.
            {"region", {{"x", px.crop.left}, {"y", px.crop.top}, {"w", px.w}, {"h", px.h}}                               },
            {"data",   base64Encode(bytes)                                                                                },
        };
    } catch (BadArgs const& e) {
        return fail("bad_args", e.what());
    } catch (std::exception const& e) {
        return fail("unavailable", e.what());
    }
}

} // namespace

std::string base64Encode(std::string_view in) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string           out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        uint32_t n = static_cast<uint8_t>(in[i]) << 16 | static_cast<uint8_t>(in[i + 1]) << 8 | static_cast<uint8_t>(in[i + 2]);
        out.push_back(kAlphabet[n >> 18 & 63]);
        out.push_back(kAlphabet[n >> 12 & 63]);
        out.push_back(kAlphabet[n >> 6 & 63]);
        out.push_back(kAlphabet[n & 63]);
    }
    if (size_t rest = in.size() - i) {
        uint32_t n = static_cast<uint32_t>(static_cast<uint8_t>(in[i])) << 16;
        if (rest == 2) n |= static_cast<uint32_t>(static_cast<uint8_t>(in[i + 1])) << 8;
        out.push_back(kAlphabet[n >> 18 & 63]);
        out.push_back(kAlphabet[n >> 12 & 63]);
        out.push_back(rest == 2 ? kAlphabet[n >> 6 & 63] : '=');
        out.push_back('=');
    }
    return out;
}

json screenshotOp(json const& req) {
    Params p;
    int    timeoutMs = kDefaultTimeout;
    try {
        p = parse(req);
        if (req.contains("timeoutMs")) timeoutMs = std::clamp(intArg(req, "timeoutMs"), 100, 60000);
    } catch (BadArgs const& e) {
        return fail("bad_args", e.what());
    }
    if (!gameWindow()) return fail("unavailable", "no game window yet");
    if (gameThreadIsCurrent()) return shoot(p); // cannot wait for ourselves (not a path any op takes today)

    // One capture at a time, process-wide: a helper stuck on a busy game thread makes later requests
    // wait for it (up to their own timeout) instead of piling up threads and window-sized DIBs. The
    // state is never destroyed, so a helper that outlives its request finishes harmlessly.
    struct Shared {
        std::mutex              m;
        std::condition_variable cv;
        bool                    running = false;
    };
    static Shared& shared = *new Shared;
    struct Job {
        bool done = false;
        json result;
    };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock lock(shared.m);
    if (!shared.cv.wait_until(lock, deadline, [] { return !shared.running; }))
        return fail("unavailable", "an earlier capture is still waiting on the game window (game thread busy?)");
    shared.running = true;
    auto job       = std::make_shared<Job>();
    std::thread([job, p] {
        json r = shoot(p);
        {
            std::lock_guard l(shared.m);
            job->result    = std::move(r);
            job->done      = true;
            shared.running = false;
        }
        shared.cv.notify_all();
    }).detach();
    if (!shared.cv.wait_until(lock, deadline, [&] { return job->done; }))
        return fail("unavailable", "the game window did not answer the capture within " + std::to_string(timeoutMs) + " ms (game thread busy?)");
    return std::move(job->result);
}

} // namespace bedd
