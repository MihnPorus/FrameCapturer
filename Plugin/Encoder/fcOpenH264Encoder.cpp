﻿#include "pch.h"
#include <openh264/codec_api.h>
#include <libyuv/libyuv.h>
#include "fcFoundation.h"
#include "fcMP4Internal.h"
#include "fcH264Encoder.h"

#ifdef fcWindows
    #pragma comment(lib, "libbz2.lib")
    #pragma comment(lib, "libcurl.lib")
    #pragma comment(lib, "ws2_32.lib")
#endif

#define OpenH264Version "1.5.0"
#ifdef fcWindows
    #if defined(_M_AMD64)
        #define OpenH264URL "http://ciscobinary.openh264.org/openh264-" OpenH264Version "-win64msvc.dll.bz2"
        #define OpenH264DLL "openh264-" OpenH264Version "-win64msvc.dll"
    #elif defined(_M_IX86)
        #define OpenH264URL "http://ciscobinary.openh264.org/openh264-" OpenH264Version "-win32msvc.dll.bz2"
        #define OpenH264DLL "openh264-" OpenH264Version "-win32msvc.dll"
    #endif
#else 
    // Mac
    #define OpenH264URL "http://ciscobinary.openh264.org/libopenh264-" OpenH264Version "-osx64.dylib.bz2"
    #define OpenH264DLL "libopenh264-" OpenH264Version "-osx64.dylib"
#endif



class fcOpenH264Encoder : public fcIH264Encoder
{
public:
    fcOpenH264Encoder(const fcH264EncoderConfig& conf);
    ~fcOpenH264Encoder();
    bool encode(fcH264Frame& dst, const fcI420Image& image, uint64_t timestamp, bool force_keyframe) override;

private:
    fcH264EncoderConfig m_conf;
    ISVCEncoder *m_encoder;
};

fcIH264Encoder* fcCreateOpenH264Encoder(const fcH264EncoderConfig& conf)
{
    if (!fcLoadOpenH264Module()) { return nullptr; }
    return new fcOpenH264Encoder(conf);
}


namespace {

typedef int  (*WelsCreateSVCEncoder_t)(ISVCEncoder** ppEncoder);
typedef void (*WelsDestroySVCEncoder_t)(ISVCEncoder* pEncoder);

#define decl(name) name##_t name##_imp;
decl(WelsCreateSVCEncoder)
decl(WelsDestroySVCEncoder)
#undef decl

module_t g_mod_h264;

} // namespace


bool fcLoadOpenH264Module()
{
    if (g_mod_h264 != nullptr) { return true; }

    g_mod_h264 = DLLLoad(OpenH264DLL);
    if (g_mod_h264 == nullptr) { return false; }

#define imp(name) (void*&)name##_imp = DLLGetSymbol(g_mod_h264, #name);
    imp(WelsCreateSVCEncoder)
        imp(WelsDestroySVCEncoder)
#undef imp
        return true;
}


fcOpenH264Encoder::fcOpenH264Encoder(const fcH264EncoderConfig& conf)
    : m_conf(conf), m_encoder(nullptr)
{
    fcLoadOpenH264Module();
    if (g_mod_h264 == nullptr) { return; }

    WelsCreateSVCEncoder_imp(&m_encoder);

    SEncParamBase param;
    memset(&param, 0, sizeof(SEncParamBase));
    param.iUsageType = SCREEN_CONTENT_REAL_TIME;
    param.fMaxFrameRate = conf.target_framerate;
    param.iPicWidth = conf.width;
    param.iPicHeight = conf.height;
    param.iTargetBitrate = conf.target_bitrate;
    param.iRCMode = RC_BITRATE_MODE;
    int ret = m_encoder->Initialize(&param);
}

fcOpenH264Encoder::~fcOpenH264Encoder()
{
    if (g_mod_h264 == nullptr) { return; }

    WelsDestroySVCEncoder_imp(m_encoder);
}

bool fcOpenH264Encoder::encode(fcH264Frame& dst, const fcI420Image& image, uint64_t timestamp, bool force_keyframe)
{
    if (!m_encoder) { return false; }

    SSourcePicture src;
    memset(&src, 0, sizeof(src));
    src.iPicWidth = m_conf.width;
    src.iPicHeight = m_conf.height;
    src.iColorFormat = videoFormatI420;
    src.pData[0] = (unsigned char*)image.y;
    src.pData[1] = (unsigned char*)image.u;
    src.pData[2] = (unsigned char*)image.v;
    src.iStride[0] = m_conf.width;
    src.iStride[1] = m_conf.width >> 1;
    src.iStride[2] = m_conf.width >> 1;
    src.uiTimeStamp = timestamp / 1000000; // nanosec to millisec

    SFrameBSInfo frame;
    memset(&frame, 0, sizeof(frame));

    if (m_encoder->EncodeFrame(&src, &frame) != 0) {
        return false;
    }

    dst.nal_sizes.clear();
    dst.data.clear();
    dst.h264_type = (fcH264FrameType)frame.eFrameType;

    for (int li = 0; li < frame.iLayerNum; ++li) {
        auto& layer = frame.sLayerInfo[li];
        dst.nal_sizes.insert(dst.nal_sizes.end(), layer.pNalLengthInByte, layer.pNalLengthInByte + layer.iNalCount);

        int total = 0;
        fcH264NALHeader header;
        for (int ni = 0; ni < layer.iNalCount; ++ni) {
            header = fcH264NALHeader(layer.pBsBuf[total + 4]);
            total += layer.pNalLengthInByte[ni];
        }
        dst.data.append(layer.pBsBuf, total);
    }

    return true;
}


// -------------------------------------------------------------
// OpenH264 downloader
// -------------------------------------------------------------

extern std::string g_fcModulePath;

namespace {

    std::thread *g_download_thread;

    std::string fcGetOpenH264ModulePath()
    {
        std::string ret = !g_fcModulePath.empty() ? g_fcModulePath : DLLGetDirectoryOfCurrentModule();
        if (!ret.empty() && (ret.back() != '/' && ret.back() != '\\')) {
            ret += "/";
        }
        ret += OpenH264DLL;
        return ret;
    }

    void fcDownloadCB_Dummy(bool, const char*)
    {
    }

    void fcMP4DownloadCodecBody(fcDownloadCallback cb)
    {
        std::string response;
        if (HTTPGet(OpenH264URL, response)) {
            cb(false, "HTTP Get completed");
            if (BZ2DecompressToFile(fcGetOpenH264ModulePath().c_str(), &response[0], response.size()))
            {
                cb(true, "BZ2 Decompress completed");
            }
            else {
                cb(true, "BZ2 Decompress failed");
            }
        }
        else {
            cb(true, "HTTP Get failed");
        }

        g_download_thread->detach();
        delete g_download_thread;
        g_download_thread = nullptr;
    }

} // namespace

bool fcDownloadOpenH264(fcDownloadCallback cb)
{
    if (cb == nullptr) { cb = &fcDownloadCB_Dummy; }

    if (fcLoadOpenH264Module()) {
        cb(true, "module already exists");
        return true;
    }

    // download thread is already running
    if (g_download_thread != nullptr) { return false; }

    g_download_thread = new std::thread([=]() { fcMP4DownloadCodecBody(cb); });
    return true;
}