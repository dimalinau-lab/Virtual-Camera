#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "config_manager.hpp"
#include "audio_dsp.hpp"
#include "mf_shared_mem.hpp"
#include "d3d11_optics_pipeline.hpp"
#include "dshow_cam.hpp"
#include "update_manager.hpp"

#define TEST_CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " #cond " at line " << __LINE__ << "\n"; std::abort(); } } while (0)

static void testConfigJsonAndEscaping() {
    std::string raw = "Samsung \"Galaxy\"\\Pro\n";
    std::string escaped = ConfigManager::escapeJsonString(raw);
    assert(escaped == "Samsung \\\"Galaxy\\\"\\\\Pro\\n");

    std::string json = "{\"name\":\"" + escaped + "\",\"fps\":60,\"mirror_enabled\":true,\"zoom\":2.5}";
    assert(ConfigManager::extractString(json, "name", "") == raw);
    assert(ConfigManager::extractInt(json, "fps", 30) == 60);
    assert(ConfigManager::extractBool(json, "mirror_enabled", false) == true);
    assert(std::abs(ConfigManager::extractFloat(json, "zoom", 1.0f) - 2.5f) < 0.001f);
    assert(ConfigManager::extractString(json, "missing", "fallback") == "fallback");
}

static void testConfigAuthTokens() {
    auto& cfg = ConfigManager::instance();
    cfg.setDiskSaveEnabled(false);
    cfg.setRuntimeToken("192.168.1.99", "ephem-tok-123");
    assert(cfg.getTokenForIp("192.168.1.99") == "ephem-tok-123");

    cfg.addPairedDevice("TestPhone", "192.168.1.99", "perm-tok-456");
    TEST_CHECK(cfg.getTokenForIp("192.168.1.99") == "perm-tok-456");
    TEST_CHECK(cfg.getTokenForDevice("192.168.1.150", "TestPhone") == "perm-tok-456");
    TEST_CHECK(cfg.isDeviceAuthorized("192.168.1.99", "perm-tok-456") == true);
    TEST_CHECK(cfg.isDeviceAuthorized("192.168.1.150", "perm-tok-456") == true);
    TEST_CHECK(cfg.getTokenForIp("192.168.1.150") == "perm-tok-456");
    TEST_CHECK(cfg.isDeviceAuthorized("192.168.1.150", "wrong-tok") == false);
    TEST_CHECK(cfg.isDeviceAuthorized("192.168.1.150", "") == false);

    cfg.syncPhoneConfig("4k", 30, 8);
    TEST_CHECK(cfg.get().phone_resolution == "4k");
    TEST_CHECK(cfg.get().phone_fps == 30);
    TEST_CHECK(cfg.get().phone_bitrate == 8);

    cfg.updateFromJson("{\"aspect_ratio_mode\":2,\"noise_gate_threshold\":0.04}");
    TEST_CHECK(cfg.get().aspect_ratio_mode == 2);
    TEST_CHECK(std::abs(cfg.get().noise_gate_threshold - 0.04f) < 0.001f);

    cfg.removePairedDevice("192.168.1.150");
    TEST_CHECK(cfg.getTokenForIp("192.168.1.150").empty());
    TEST_CHECK(cfg.isDeviceAuthorized("192.168.1.150", "perm-tok-456") == false);
}

static void testAudioDspProcessor() {
    AudioDSPProcessor dsp(48000.0f);
    dsp.setDeclicker(true);
    dsp.setEq(20.0f, -20.0f, 3.0f); // Должно ограничиться диапазоном [-15, 15]

    // Нулевой или пустой буфер не должен падать
    dsp.process(nullptr, 0);

    // Изолированный щелчок должен подавляться
    std::vector<float> buf(64, 0.0f);
    buf[10] = 0.95f;
    dsp.process(buf.data(), buf.size());
    assert(buf[10] < 0.95f);
}

static void testMuxAndSharedMemInvariants() {
    assert(MC_MAGIC_0 == 0x4D); // 'M'
    assert(MC_MAGIC_1 == 0x43); // 'C'
    assert(sizeof(MFVirtualCamHeader) > 0);
}

static void testDShowI420NegotiationAndConversion() {
    // Verify NV12 -> I420 SIMD + scalar tail conversion (68x4 => 136 UV bytes: 4x32 SIMD + 8 scalar tail)
    const int w = 68;
    const int h = 4;
    const size_t yBytes = (size_t)w * h;
    const size_t chromaBytes = yBytes / 4;
    std::vector<uint8_t> nv12(yBytes + yBytes / 2);
    for (size_t i = 0; i < yBytes; ++i) nv12[i] = static_cast<uint8_t>((i * 3) & 0xFF);
    for (size_t i = 0; i < chromaBytes; ++i) {
        nv12[yBytes + i * 2] = static_cast<uint8_t>((i + 17) & 0xFF);      // U
        nv12[yBytes + i * 2 + 1] = static_cast<uint8_t>((i + 143) & 0xFF); // V
    }

    std::vector<uint8_t> i420(yBytes + yBytes / 2, 0);
    ConvertNV12toI420(nv12.data(), nv12.data() + yBytes, i420.data(), w, h);

    for (size_t i = 0; i < yBytes; ++i) {
        TEST_CHECK(i420[i] == nv12[i]);
    }
    for (size_t i = 0; i < chromaBytes; ++i) {
        TEST_CHECK(i420[yBytes + i] == static_cast<uint8_t>((i + 17) & 0xFF));
        TEST_CHECK(i420[yBytes + chromaBytes + i] == static_cast<uint8_t>((i + 143) & 0xFF));
    }

    // Verify DirectShow pin dynamic capabilities for 720p, 1080p, and 4K ceilings
    DShowCaptureFilter* filter = new DShowCaptureFilter();
    IPin* pin = nullptr;
    HRESULT hr = filter->FindPin(L"Capture", &pin);
    TEST_CHECK(SUCCEEDED(hr) && pin != nullptr);

    DShowPin* dshowPin = static_cast<DShowPin*>(pin);
    IAMStreamConfig* cfg = nullptr;
    hr = pin->QueryInterface(IID_IAMStreamConfig, reinterpret_cast<void**>(&cfg));
    TEST_CHECK(SUCCEEDED(hr) && cfg != nullptr);

    auto getCapDims = [&](int idx, GUID& outSubtype, int& outW, int& outH) {
        VIDEO_STREAM_CONFIG_CAPS scc{};
        AM_MEDIA_TYPE* pmt = nullptr;
        HRESULT capHr = cfg->GetStreamCaps(idx, &pmt, reinterpret_cast<BYTE*>(&scc));
        TEST_CHECK(capHr == S_OK && pmt != nullptr && pmt->pbFormat != nullptr);
        outSubtype = pmt->subtype;
        auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(pmt->pbFormat);
        outW = vih->bmiHeader.biWidth;
        outH = abs(vih->bmiHeader.biHeight);
        CoTaskMemFree(pmt->pbFormat);
        CoTaskMemFree(pmt);
    };

    int count = 0, size = 0;
    GUID st{};
    int capW = 0, capH = 0;

    // 720p ceiling: 8 caps (I420: 720,540,360; NV12: 720,540,360; RGB24: 720,360)
    dshowPin->SetCapabilityCeilingForTest(1280);
    hr = cfg->GetNumberOfCapabilities(&count, &size);
    TEST_CHECK(SUCCEEDED(hr));
    TEST_CHECK(count == 8);
    TEST_CHECK(size == static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS)));
    getCapDims(0, st, capW, capH);
    TEST_CHECK(st == MEDIASUBTYPE_I420_VCAM && capW == 1280 && capH == 720);
    getCapDims(3, st, capW, capH);
    TEST_CHECK(st == MEDIASUBTYPE_NV12 && capW == 1280 && capH == 720);
    getCapDims(6, st, capW, capH);
    TEST_CHECK(st == MEDIASUBTYPE_RGB24 && capW == 1280 && capH == 720);

    // 1080p ceiling: 11 caps (I420: 1080,720,540,360; NV12: 1080,720,540,360; RGB24: 1080,720,360)
    dshowPin->SetCapabilityCeilingForTest(1920);
    hr = cfg->GetNumberOfCapabilities(&count, &size);
    TEST_CHECK(SUCCEEDED(hr) && count == 11);
    getCapDims(0, st, capW, capH);
    TEST_CHECK(st == MEDIASUBTYPE_I420_VCAM && capW == 1920 && capH == 1080);

    // 4K ceiling: 13 caps (I420: 4K..360p; NV12: 4K..360p; RGB24: 1080p,720p,360p)
    dshowPin->SetCapabilityCeilingForTest(3840);
    hr = cfg->GetNumberOfCapabilities(&count, &size);
    TEST_CHECK(SUCCEEDED(hr) && count == 13);
    getCapDims(0, st, capW, capH);
    TEST_CHECK(st == MEDIASUBTYPE_I420_VCAM && capW == 3840 && capH == 2160);

    // Verify GetFormat reflects ceiling before explicit SetFormat, and honors SetFormat afterwards
    AM_MEDIA_TYPE* pmtDefault = nullptr;
    hr = cfg->GetFormat(&pmtDefault);
    TEST_CHECK(SUCCEEDED(hr) && pmtDefault != nullptr);
    auto* vihDef = reinterpret_cast<VIDEOINFOHEADER*>(pmtDefault->pbFormat);
    TEST_CHECK(vihDef->bmiHeader.biWidth == 3840 && vihDef->bmiHeader.biHeight == 2160);
    CoTaskMemFree(pmtDefault->pbFormat);
    CoTaskMemFree(pmtDefault);

    // Select 1080p I420 explicitly and verify SetFormat / GetFormat
    VIDEO_STREAM_CONFIG_CAPS sccSel{};
    AM_MEDIA_TYPE* pmt1080 = nullptr;
    hr = cfg->GetStreamCaps(1, &pmt1080, reinterpret_cast<BYTE*>(&sccSel));
    TEST_CHECK(hr == S_OK && pmt1080 != nullptr);
    TEST_CHECK(pin->QueryAccept(pmt1080) == S_OK);
    TEST_CHECK(SUCCEEDED(cfg->SetFormat(pmt1080)));
    CoTaskMemFree(pmt1080->pbFormat);
    CoTaskMemFree(pmt1080);

    AM_MEDIA_TYPE* pmtCur = nullptr;
    hr = cfg->GetFormat(&pmtCur);
    TEST_CHECK(SUCCEEDED(hr) && pmtCur != nullptr);
    auto* vihCur = reinterpret_cast<VIDEOINFOHEADER*>(pmtCur->pbFormat);
    TEST_CHECK(vihCur->bmiHeader.biWidth == 1920 && vihCur->bmiHeader.biHeight == 1080);
    CoTaskMemFree(pmtCur->pbFormat);
    CoTaskMemFree(pmtCur);

    // Verify ScaleNV12: 2x2 SSE2 fast path (1280x720 -> 640x360 and 3840x2160 -> 1920x1080) & bilinear (1280x720 -> 960x540)
    std::vector<uint8_t> src720(1280 * 720 * 3 / 2, 200);
    std::vector<uint8_t> dst360(640 * 360 * 3 / 2, 0);
    ScaleNV12(src720.data(), 1280, 720, dst360.data(), 640, 360);
    TEST_CHECK(dst360[0] == 200 && dst360[640 * 360 - 1] == 200 && dst360[640 * 360 + 100] == 200);

    std::vector<uint8_t> dst540(960 * 540 * 3 / 2, 0);
    ScaleNV12(src720.data(), 1280, 720, dst540.data(), 960, 540);
    TEST_CHECK(dst540[0] == 200 && dst540[960 * 540 - 1] == 200 && dst540[960 * 540 + 100] == 200);

    std::vector<uint8_t> src4k(3840ULL * 2160ULL * 3 / 2, 180);
    std::vector<uint8_t> dst1080(1920ULL * 1080ULL * 3 / 2, 0);
    ScaleNV12(src4k.data(), 3840, 2160, dst1080.data(), 1920, 1080);
    TEST_CHECK(dst1080[0] == 180 && dst1080[1920 * 1080 - 1] == 180 && dst1080[1920 * 1080 + 500] == 180);

    cfg->Release();
    pin->Release();
    filter->Release();
}

static void testD3D11MotionSynthesisPipeline() {
    D3D11OpticsPipeline pipe;
    bool inited = pipe.init(1280, 720);
    TEST_CHECK(inited);

    const int inW = 1280;
    const int inH = 720;
    const size_t ySize = (size_t)inW * inH;
    const size_t uvSize = ySize / 2;
    std::vector<uint8_t> src1(ySize + uvSize, 128);
    std::vector<uint8_t> src2(ySize + uvSize, 128);
    memset(src1.data(), 32, ySize);
    memset(src2.data(), 32, ySize);

    // Paint bright vertical bar at x=300..360 in frame 1, shifted +8px to x=308..368 in frame 2
    for (int y = 200; y < 520; ++y) {
        memset(src1.data() + y * inW + 300, 220, 60);
        memset(src2.data() + y * inW + 308, 220, 60);
    }

    D3D11ShaderParams params{};
    params.brightness = 0.0f;
    params.contrast = 1.0f;
    params.saturation = 1.0f;
    params.zoom = 1.0f;
    params.canvasWidth = 1280.0f;
    params.canvasHeight = 720.0f;
    params.animAspectHalfWidth = 0.5f;

    std::vector<uint8_t> outCurr(ySize + uvSize, 0);
    std::vector<uint8_t> outInterp(ySize + uvSize, 0);
    bool didSynth = false;

    bool ok1 = pipe.processNv12ToNv12WithSynth(
        src1.data(), inW, src1.data() + ySize, inW, inW, inH,
        outCurr.data(), outInterp.data(), true, &didSynth, params
    );
    TEST_CHECK(ok1);
    TEST_CHECK(!didSynth); // First warmup frame

    bool ok2 = pipe.processNv12ToNv12WithSynth(
        src2.data(), inW, src2.data() + ySize, inW, inW, inH,
        outCurr.data(), outInterp.data(), true, &didSynth, params
    );
    TEST_CHECK(ok2);
    TEST_CHECK(didSynth); // Second frame synthesizes midpoint frame

    // Midpoint bar center around x=334 should be bright in synthesized frame
    uint8_t midY = outInterp[360 * inW + 334];
    TEST_CHECK(midY > 150);

    // Dynamic resize: 720p -> 1080p -> 4K -> 720p preserving D3D11 device & shaders
    ID3D11Device* devBefore = pipe.getDevice();
    TEST_CHECK(pipe.resize(1920, 1080) && pipe.isReady());
    TEST_CHECK(pipe.getDevice() == devBefore);

    std::vector<uint8_t> out1080(1920ULL * 1080ULL * 3 / 2, 0);
    D3D11ShaderParams params1080 = params;
    params1080.canvasWidth = 1920.0f;
    params1080.canvasHeight = 1080.0f;
    TEST_CHECK(pipe.processNv12ToNv12WithSynth(
        src1.data(), inW, src1.data() + ySize, inW, inW, inH,
        out1080.data(), out1080.data(), false, &didSynth, params1080
    ));

    TEST_CHECK(pipe.resize(3840, 2160) && pipe.isReady());
    TEST_CHECK(pipe.getDevice() == devBefore);
    std::vector<uint8_t> out4k(3840ULL * 2160ULL * 3 / 2, 0);
    D3D11ShaderParams params4k = params;
    params4k.canvasWidth = 3840.0f;
    params4k.canvasHeight = 2160.0f;
    TEST_CHECK(pipe.processNv12ToNv12WithSynth(
        src1.data(), inW, src1.data() + ySize, inW, inW, inH,
        out4k.data(), out4k.data(), false, &didSynth, params4k
    ));

    TEST_CHECK(pipe.resize(1280, 720) && pipe.isReady());

    pipe.resetMotionHistory();
    pipe.cleanup();
}

static void testDriverUpdateAndVersionCheck() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path tmpDir = fs::temp_directory_path(ec) / L"vcn_unit_driver_swap";
    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir, ec);

    {
        std::ofstream fOld(tmpDir / L"VirtualCam.dll", std::ios::binary);
        fOld << "old-driver";
    }
    {
        std::ofstream fPend(tmpDir / L"VirtualCam.dll.pending", std::ios::binary);
        fPend << "new-driver";
    }
    {
        std::ofstream fLegacy(tmpDir / L"NativeMFVirtualCam.dll", std::ios::binary);
        fLegacy << "legacy-driver";
    }
    {
        std::ofstream fGarbage(tmpDir / L"VirtualCam.dll.999.old", std::ios::binary);
        fGarbage << "stale";
    }

    TEST_CHECK(UpdateManager::applyPendingDriverAndCleanup(tmpDir));
    TEST_CHECK(!fs::exists(tmpDir / L"VirtualCam.dll.pending", ec));
    TEST_CHECK(!fs::exists(tmpDir / L"NativeMFVirtualCam.dll", ec));
    TEST_CHECK(!fs::exists(tmpDir / L"VirtualCam.dll.999.old", ec));
    TEST_CHECK(fs::exists(tmpDir / L"VirtualCam.dll", ec));

    {
        std::ifstream fNew(tmpDir / L"VirtualCam.dll", std::ios::binary);
        std::string content;
        std::getline(fNew, content);
        TEST_CHECK(content == "new-driver");
    }
    fs::remove_all(tmpDir, ec);

    // Verify the compiled VirtualCam.dll next to VirtualCamTests.exe exports matching version
    wchar_t exePath[MAX_PATH] = {};
    TEST_CHECK(GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0);
    fs::path builtDll = fs::path(exePath).parent_path() / L"VirtualCam.dll";
    TEST_CHECK(fs::exists(builtDll, ec));
    TEST_CHECK(UpdateManager::readInstalledDriverVersion(builtDll) == VIRTUALCAM_VERSION);
    TEST_CHECK(UpdateManager::unescapeJsonString("line1\\r\\n- \\\"test\\\"") == "line1\n- \"test\"");
}

int main() {
    std::cout << "[1] testConfigJsonAndEscaping...\n" << std::flush;
    testConfigJsonAndEscaping();
    std::cout << "[2] testConfigAuthTokens...\n" << std::flush;
    testConfigAuthTokens();
    std::cout << "[3] testAudioDspProcessor...\n" << std::flush;
    testAudioDspProcessor();
    std::cout << "[4] testMuxAndSharedMemInvariants...\n" << std::flush;
    testMuxAndSharedMemInvariants();
    std::cout << "[5] testDShowI420NegotiationAndConversion...\n" << std::flush;
    testDShowI420NegotiationAndConversion();
    std::cout << "[6] testD3D11MotionSynthesisPipeline...\n" << std::flush;
    testD3D11MotionSynthesisPipeline();
    std::cout << "[7] testDriverUpdateAndVersionCheck...\n" << std::flush;
    testDriverUpdateAndVersionCheck();
    std::cout << "[OK] All VirtualCamTests passed.\n";
    return 0;
}
