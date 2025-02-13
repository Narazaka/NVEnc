﻿// -----------------------------------------------------------------------------------------
// NVEnc by rigaya
// -----------------------------------------------------------------------------------------
//
// The MIT License
//
// Copyright (c) 2014-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <vector>
#include <array>
#include <map>
#include <numeric>
#include <deque>
#include <string>
#include <algorithm>
#include <thread>
#include <tchar.h>
#pragma warning(push)
#pragma warning(disable: 4819)
//ファイルは、現在のコード ページ (932) で表示できない文字を含んでいます。
//データの損失を防ぐために、ファイルを Unicode 形式で保存してください。
#include <cuda.h>
#include <cuda_runtime.h>
#include "nvEncodeAPI.h"
#pragma warning(pop)
#include "process.h"
#pragma comment(lib, "winmm.lib")
#include "NVEncCore.h"
#include "rgy_version.h"
#include "rgy_status.h"
#include "rgy_input.h"
#include "rgy_input_raw.h"
#include "rgy_input_avi.h"
#include "rgy_input_avs.h"
#include "rgy_input_vpy.h"
#include "rgy_input_avcodec.h"
#include "rgy_output.h"
#include "rgy_output_avcodec.h"
#include "NVEncParam.h"
#include "NVEncUtil.h"
#include "NVEncFilter.h"
#include "NVEncFilterDelogo.h"
#include "NVEncFilterDenoiseKnn.h"
#include "NVEncFilterDenoisePmd.h"
#include "NVEncFilterDeband.h"
#include "NVEncFilterAfs.h"
#include "NVEncFilterNnedi.h"
#include "NVEncFilterYadif.h"
#include "NVEncFilterRff.h"
#include "NVEncFilterUnsharp.h"
#include "NVEncFilterEdgelevel.h"
#include "NVEncFilterTweak.h"
#include "NVEncFilterColorspace.h"
#include "NVEncFilterSubburn.h"
#include "NVEncFilterSelectEvery.h"
#include "NVEncFeature.h"
#include "chapter_rw.h"
#include "helper_cuda.h"
#include "helper_nvenc.h"
#include "h264_level.h"
#include "hevc_level.h"
#include "shlwapi.h"
#pragma comment(lib, "shlwapi.lib")

#pragma warning(push)
#pragma warning(disable: 4244)
#define TTMATH_NOASM
#include "ttmath/ttmath.h"
#if _M_IX86
typedef ttmath::Int<4> ttint128;
#else
typedef ttmath::Int<2> ttint128;
#endif
#pragma warning(pop)


using std::deque;

#if ENABLE_NVTX
#include "nvToolsExt.h"
#ifdef _M_IX86
#pragma comment(lib, "nvToolsExt32_1.lib")
#else
#pragma comment(lib, "nvToolsExt64_1.lib")
#endif

class NvtxTracer {
public:
    NvtxTracer(const char *name) {
        nvtxRangePushA(name);
    }
    ~NvtxTracer() {
        nvtxRangePop();
    }
};
#define NVTXRANGE(name) NvtxTracer nvtx ## name( #name );
#else
#define NVTXRANGE(name)
#endif


class FrameBufferDataIn {
public:
    FrameBufferDataIn() : m_pInfo(), m_oVPP(), m_frameInfo(), m_bInputHost(), m_heTransferFin(NULL) {

    };
    FrameBufferDataIn(shared_ptr<CUVIDPARSERDISPINFO> pInfo, const CUVIDPROCPARAMS& oVPP, const FrameInfo& frameInfo) : m_pInfo(), m_oVPP(), m_frameInfo(), m_bInputHost(false), m_heTransferFin(NULL) {
        setInfo(pInfo, oVPP, frameInfo);
    };
    FrameBufferDataIn(const FrameBufferDataIn& obj) :
        m_pInfo(obj.m_pInfo),
        m_oVPP(obj.m_oVPP),
        m_frameInfo(obj.m_frameInfo),
        m_bInputHost(obj.m_bInputHost),
        m_heTransferFin(obj.m_heTransferFin) {
    };
    ~FrameBufferDataIn() {
        m_heTransferFin.reset();
        m_pInfo.reset();
    }
    void setHostFrameInfo(
        const FrameInfo& frameInfo, //入力フレームへのポインタと情報
        shared_ptr<void> heTransferFin //入力フレームに関連付けられたイベント、このフレームが不要になったらSetする
    ) {
        m_pInfo.reset();
        memset(&m_oVPP, 0, sizeof(m_oVPP));
        m_frameInfo = frameInfo;
        m_bInputHost = true;
        m_heTransferFin = heTransferFin;
    }
    void setCuvidInfo(shared_ptr<CUVIDPARSERDISPINFO> pInfo, const FrameInfo& frameInfo) {
        m_pInfo = pInfo;
        memset(&m_oVPP, 0, sizeof(m_oVPP));

        m_frameInfo = frameInfo;
        //frameinfo.picstructの決定
        m_frameInfo.picstruct = (pInfo->progressive_frame) ? RGY_PICSTRUCT_FRAME : ((pInfo->top_field_first) ? RGY_PICSTRUCT_FRAME_TFF : RGY_PICSTRUCT_FRAME_BFF);
        //RFFに関するフラグを念のためクリア
        m_frameInfo.flags = RGY_FRAME_FLAG_NONE;
        //RFFフラグの有無
        if (pInfo->repeat_first_field == 1) {
            m_frameInfo.flags |= RGY_FRAME_FLAG_RFF;
        }
        //TFFかBFFか
        m_frameInfo.flags |= (pInfo->top_field_first) ? RGY_FRAME_FLAG_RFF_TFF : RGY_FRAME_FLAG_RFF_BFF;
        m_frameInfo.duration = 0;
        m_frameInfo.timestamp = pInfo->timestamp;
        m_bInputHost = false;
    }
    void setInfo(shared_ptr<CUVIDPARSERDISPINFO> pInfo, const CUVIDPROCPARAMS& oVPP, const FrameInfo& frameInfo) {
        m_pInfo = pInfo;
        m_frameInfo = frameInfo;
        m_oVPP = oVPP;
    }
    shared_ptr<CUVIDPARSERDISPINFO> getCuvidInfo() {
        return m_pInfo;
    }
    CUVIDPROCPARAMS getVppInfo() {
        return m_oVPP;
    }
    void resetCuvidInfo() {
        m_pInfo.reset();
    }
    int64_t getTimeStamp() const {
        return m_frameInfo.timestamp;
    }
    void setTimeStamp(int64_t timestamp) {
        m_frameInfo.timestamp = timestamp;
    }
    int64_t getDuration() const {
        return m_frameInfo.duration;
    }
    void setDuration(int64_t duration) {
        m_frameInfo.duration = duration;
    }
    bool inputIsHost() const {
        return m_bInputHost;
    }
    FrameInfo getFrameInfo() const {
        return m_frameInfo;
    }
    void setInterlaceFlag(RGY_PICSTRUCT picstruct) {
        m_frameInfo.picstruct = picstruct;
    }
    void setInputFrameId(int inputFrameId) {
        m_frameInfo.inputFrameId = inputFrameId;
    }
private:
    shared_ptr<CUVIDPARSERDISPINFO> m_pInfo;
    CUVIDPROCPARAMS m_oVPP;
    FrameInfo m_frameInfo;
    bool m_bInputHost;      //入力フレームへのポインタと情報
    shared_ptr<void> m_heTransferFin; //入力フレームに関連付けられたイベント、このフレームが不要になったらSetする
};

class FrameBufferDataEnc {
public:
    RGY_CSP m_csp;
    uint64_t m_timestamp;
    uint64_t m_duration;
    int m_inputFrameId;
    EncodeBuffer *m_pEncodeBuffer;
    cudaEvent_t *m_pEvent;
    FrameBufferDataEnc(RGY_CSP csp, uint64_t timestamp, uint64_t duration, int inputFrameId, EncodeBuffer *pEncodeBuffer, cudaEvent_t *pEvent) :
        m_csp(csp),
        m_timestamp(timestamp),
        m_duration(duration),
        m_pEncodeBuffer(pEncodeBuffer),
        m_pEvent(pEvent),
        m_inputFrameId(inputFrameId) {
    };
    ~FrameBufferDataEnc() {
    }
};

bool check_if_nvcuda_dll_available() {
    //check for nvcuda.dll
    HMODULE hModule = LoadLibrary(_T("nvcuda.dll"));
    if (hModule == NULL)
        return false;
    FreeLibrary(hModule);
    return true;
}

NVEncoderGPUInfo::NVEncoderGPUInfo(int deviceId, bool getFeatures) {
    CUresult cuResult = CUDA_SUCCESS;

    if (!check_if_nvcuda_dll_available())
        return;

    if (CUDA_SUCCESS != (cuResult = cuInit(0)))
        return;
    if (CUDA_SUCCESS != (cuResult = cuvidInit(0)))
        return;

    int deviceCount = 0;
    if (CUDA_SUCCESS != (cuDeviceGetCount(&deviceCount)) || 0 == deviceCount)
        return;

    for (int currentDevice = 0; currentDevice < deviceCount; currentDevice++) {
        char pci_bus_name[1024] = { 0 };
        CUdevice cuDevice = 0;
        cudaDeviceProp devProp;
        if ((deviceId < 0 || deviceId == currentDevice)
            && cudaSuccess == cudaDeviceGetPCIBusId(pci_bus_name, sizeof(pci_bus_name), currentDevice)
            && CUDA_SUCCESS == cuDeviceGet(&cuDevice, currentDevice)
            && CUDA_SUCCESS == cudaGetDeviceProperties(&devProp, cuDevice)
            && (((devProp.major << 4) + devProp.minor) >= 0x30)) {
            unique_ptr<NVEncFeature> nvFeature;
            if (getFeatures) {
                nvFeature = unique_ptr<NVEncFeature>(new NVEncFeature());
                nvFeature->createCacheAsync(currentDevice, RGY_LOG_INFO);
            }

            NVGPUInfo gpu;
            gpu.id = currentDevice;
            gpu.pciBusId = pci_bus_name;
            gpu.name = char_to_tstring(devProp.name);
            gpu.compute_capability.first = devProp.major;
            gpu.compute_capability.second = devProp.minor;
            gpu.clock_rate = devProp.clockRate;
            gpu.cuda_cores = _ConvertSMVer2Cores(devProp.major, devProp.minor) * devProp.multiProcessorCount;
            gpu.nv_driver_version = INT_MAX;
            gpu.pcie_gen = 0;
            gpu.pcie_link = 0;
#if ENABLE_NVML
            {
                int version = 0, pcie_gen = 0, pcie_link = 0;
                NVMLMonitor nvml_monitor;
                if (NVML_SUCCESS == nvml_monitor.Init(gpu.pciBusId)
                    && NVML_SUCCESS == nvml_monitor.getDriverVersionx1000(version)
                    && NVML_SUCCESS == nvml_monitor.getMaxPCIeLink(pcie_gen, pcie_link)) {
                    gpu.nv_driver_version = version;
                    gpu.pcie_gen = pcie_gen;
                    gpu.pcie_link = pcie_link;
                }
            }
#endif //#if ENABLE_NVML
            if (gpu.nv_driver_version == INT_MAX) {
                TCHAR buffer[1024];
                if (0 == getGPUInfo(GPU_VENDOR, buffer, _countof(buffer), currentDevice, true, true)) {
                    try {
                        gpu.nv_driver_version = (int)(std::stod(buffer) * 1000.0 + 0.5);
                    } catch (...) {
                    }
                }
            }
            if (0 < gpu.nv_driver_version && gpu.nv_driver_version < NV_DRIVER_VER_MIN) {
                gpu.nv_driver_version = -1;
            }

            gpu.cuda_driver_version = 0;
            if (CUDA_SUCCESS != (cuResult = cuDriverGetVersion(&gpu.cuda_driver_version))) {
                gpu.cuda_driver_version = -1;
            }

#if ENABLE_AVSW_READER
            CUcontext cuctx;
            if (CUDA_SUCCESS == (cuResult = cuCtxCreate((CUcontext*)(&cuctx), 0, cuDevice))) {
                gpu.cuvid_csp = getHWDecCodecCsp();
                cuCtxDestroy(cuctx);
            }
#endif

            if (getFeatures) {
                gpu.nvenc_codec_features = nvFeature->GetCachedNVEncCapability();
            }
            nvFeature.reset();
            GPUList.push_back(gpu);
        }
    }
};

std::list<NVGPUInfo> get_gpu_list() {
    NVEncoderGPUInfo gpuinfo(-1, false);
    return gpuinfo.getGPUList();
}

NVEncoderGPUInfo::~NVEncoderGPUInfo() {
};

NVEncCore::NVEncCore() {
    m_pEncodeAPI = nullptr;
    m_ctxLock = NULL;
    m_hinstLib = NULL;
    m_hEncoder = nullptr;
    m_pStatus = nullptr;
    m_pFileReader = nullptr;
    m_encodeBufferCount = 16;
    m_pDevice = nullptr;
    m_nDeviceId = 0;
    m_pAbortByUser = nullptr;
    m_trimParam.list.clear();
    m_trimParam.offset = 0;
#if ENABLE_AVSW_READER
    m_keyFile.clear();
    m_keyOnChapter = false;
#endif
    m_appliedDynamicRC = DYNAMIC_PARAM_NOT_SELECTED;

    INIT_CONFIG(m_stCreateEncodeParams, NV_ENC_INITIALIZE_PARAMS);
    INIT_CONFIG(m_stEncConfig, NV_ENC_CONFIG);

    memset(&m_stEOSOutputBfr, 0, sizeof(m_stEOSOutputBfr));
    memset(&m_stEncodeBuffer, 0, sizeof(m_stEncodeBuffer));
}

NVEncCore::~NVEncCore() {
    Deinitialize();

    if (m_pEncodeAPI) {
        delete m_pEncodeAPI;
        m_pEncodeAPI = nullptr;
    }

    if (m_hinstLib) {
        FreeLibrary(m_hinstLib);
        m_hinstLib = NULL;
    }
}

void NVEncCore::SetAbortFlagPointer(bool *abortFlag) {
    m_pAbortByUser = abortFlag;
}

//エンコーダが出力使用する色空間を入力パラメータをもとに取得
RGY_CSP NVEncCore::GetEncoderCSP(const InEncodeVideoParam *inputParam) {
    const bool bOutputHighBitDepth = inputParam->codec == NV_ENC_HEVC && inputParam->encConfig.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 > 0;
    if (bOutputHighBitDepth) {
        return (inputParam->yuv444) ? RGY_CSP_YUV444_16 : RGY_CSP_P010;
    } else {
        return (inputParam->yuv444) ? RGY_CSP_YUV444 : RGY_CSP_NV12;
    }
}

#pragma warning(push)
#pragma warning(disable:4100)
void NVEncCore::PrintMes(int logLevel, const TCHAR *format, ...) {
    if (m_pNVLog.get() == nullptr) {
        if (logLevel <= RGY_LOG_INFO) {
            return;
        }
    } else if (logLevel < m_pNVLog->getLogLevel()) {
        return;
    }

    va_list args;
    va_start(args, format);

    int len = _vsctprintf(format, args) + 1; // _vscprintf doesn't count terminating '\0'
    vector<TCHAR> buffer(len, 0);
    _vstprintf_s(buffer.data(), len, format, args);
    va_end(args);

    if (m_pNVLog.get() != nullptr) {
        m_pNVLog->write(logLevel, buffer.data());
    } else {
        _ftprintf(stderr, _T("%s"), buffer.data());
    }
}

void NVEncCore::NVPrintFuncError(const TCHAR *funcName, NVENCSTATUS nvStatus) {
    PrintMes(RGY_LOG_ERROR, (FOR_AUO) ? _T("%s() がエラーを返しました。: %d (%s)\n") : _T("Error on %s: %d (%s)\n"), funcName, nvStatus, char_to_tstring(_nvencGetErrorEnum(nvStatus)).c_str());
}

void NVEncCore::NVPrintFuncError(const TCHAR *funcName, CUresult code) {
    PrintMes(RGY_LOG_ERROR, (FOR_AUO) ? _T("%s() がエラーを返しました。: %d (%s)\n") : _T("Error on %s: %d (%s)\n"), funcName, (int)code, char_to_tstring(_cudaGetErrorEnum(code)).c_str());
}

//ログを初期化
NVENCSTATUS NVEncCore::InitLog(const InEncodeVideoParam *inputParam) {
    //ログの初期化
    m_pNVLog.reset(new RGYLog(inputParam->logfile.c_str(), inputParam->loglevel));
    if (inputParam->logfile.length()) {
        m_pNVLog->writeFileHeader(inputParam->outputFilename.c_str());
    }
    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVEncCore::readChapterFile(const tstring& chapfile) {
#if ENABLE_AVSW_READER
    ChapterRW chapter;
    auto err = chapter.read_file(chapfile.c_str(), CODE_PAGE_UNSET, 0.0);
    if (err != AUO_CHAP_ERR_NONE) {
        PrintMes(RGY_LOG_ERROR, _T("failed to %s chapter file: \"%s\".\n"), (err == AUO_CHAP_ERR_FILE_OPEN) ? _T("open") : _T("read"), chapfile.c_str());
        return NV_ENC_ERR_GENERIC;
    }
    if (chapter.chapterlist().size() == 0) {
        PrintMes(RGY_LOG_ERROR, _T("no chapter found from chapter file: \"%s\".\n"), chapfile.c_str());
        return NV_ENC_ERR_GENERIC;
    }
    m_Chapters.clear();
    const auto& chapter_list = chapter.chapterlist();
    tstring chap_log;
    for (size_t i = 0; i < chapter_list.size(); i++) {
        unique_ptr<AVChapter> avchap(new AVChapter);
        avchap->time_base = av_make_q(1, 1000);
        avchap->start = chapter_list[i]->get_ms();
        avchap->end = (i < chapter_list.size()-1) ? chapter_list[i+1]->get_ms() : avchap->start + 1;
        avchap->id = (int)m_Chapters.size();
        avchap->metadata = nullptr;
        av_dict_set(&avchap->metadata, "title", wstring_to_string(chapter_list[i]->name, CP_UTF8).c_str(), 0);
        chap_log += strsprintf(_T("chapter #%02d [%d.%02d.%02d.%03d]: %s.\n"),
            avchap->id, chapter_list[i]->h, chapter_list[i]->m, chapter_list[i]->s, chapter_list[i]->ms,
            wstring_to_tstring(chapter_list[i]->name).c_str());
        m_Chapters.push_back(std::move(avchap));
    }
    PrintMes(RGY_LOG_DEBUG, _T("%s"), chap_log.c_str());
    return NV_ENC_SUCCESS;
#else
    PrintMes(RGY_LOG_ERROR, _T("chater reading unsupportted in this build"));
    return NV_ENC_ERR_UNIMPLEMENTED;
#endif //#if ENABLE_AVCODEC_QSV_READER
}

NVENCSTATUS NVEncCore::InitChapters(const InEncodeVideoParam *inputParam) {
#if ENABLE_AVSW_READER
    m_Chapters.clear();
    if (inputParam->sChapterFile.length() > 0) {
        //チャプターファイルを読み込む
        auto chap_sts = readChapterFile(inputParam->sChapterFile);
        if (chap_sts != NV_ENC_SUCCESS) {
            return chap_sts;
        }
    }
    if (m_Chapters.size() == 0) {
        auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
        if (pAVCodecReader != nullptr) {
            auto chapterList = pAVCodecReader->GetChapterList();
            //入力ファイルのチャプターをコピーする
            for (uint32_t i = 0; i < chapterList.size(); i++) {
                unique_ptr<AVChapter> avchap(new AVChapter);
                *avchap = *chapterList[i];
                m_Chapters.push_back(std::move(avchap));
            }
        }
    }
    if (m_Chapters.size() > 0) {
        if (inputParam->keyOnChapter && m_trimParam.list.size() > 0) {
            PrintMes(RGY_LOG_WARN, _T("--key-on-chap not supported when using --trim.\n"));
        } else {
            m_keyOnChapter = inputParam->keyOnChapter;
        }
    }
#endif //#if ENABLE_AVSW_READER
    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVEncCore::InitInput(InEncodeVideoParam *inputParam) {
    int sourceAudioTrackIdStart = 1;    //トラック番号は1スタート
    int sourceSubtitleTrackIdStart = 1; //トラック番号は1スタート
    int sourceDataTrackIdStart = 1;     //トラック番号は1スタート

#if ENABLE_RAW_READER
    if (inputParam->input.type == RGY_INPUT_FMT_AUTO) {
        if (check_ext(inputParam->inputFilename, { ".y4m" })) {
            inputParam->input.type = RGY_INPUT_FMT_Y4M;
        } else if (check_ext(inputParam->inputFilename, { ".yuv" })) {
            inputParam->input.type = RGY_INPUT_FMT_RAW;
#if ENABLE_AVI_READER
        } else if (check_ext(inputParam->inputFilename, { ".avi" })) {
            inputParam->input.type = RGY_INPUT_FMT_AVI;
#endif
#if ENABLE_AVISYNTH_READER
        } else if (check_ext(inputParam->inputFilename, { ".avs" })) {
            inputParam->input.type = RGY_INPUT_FMT_AVS;
#endif
#if ENABLE_VAPOURSYNTH_READER
        } else if (check_ext(inputParam->inputFilename, { ".vpy" })) {
            inputParam->input.type = RGY_INPUT_FMT_VPY_MT;
#endif
        } else {
#if ENABLE_AVSW_READER
            inputParam->input.type = RGY_INPUT_FMT_AVANY;
#else
            inputParam->input.type = RGY_INPUT_FMT_RAW;
#endif
        }
    }

    //Check if selected format is enabled
    if (inputParam->input.type == RGY_INPUT_FMT_AVS && !ENABLE_AVISYNTH_READER) {
        PrintMes(RGY_LOG_ERROR, _T("avs reader not compiled in this binary.\n"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    if (inputParam->input.type == RGY_INPUT_FMT_VPY_MT && !ENABLE_VAPOURSYNTH_READER) {
        PrintMes(RGY_LOG_ERROR, _T("vpy reader not compiled in this binary.\n"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    if (inputParam->input.type == RGY_INPUT_FMT_AVI && !ENABLE_AVI_READER) {
        PrintMes(RGY_LOG_ERROR, _T("avi reader not compiled in this binary.\n"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    if (inputParam->input.type == RGY_INPUT_FMT_AVHW && !ENABLE_AVSW_READER) {
        PrintMes(RGY_LOG_ERROR, _T("avcodec + cuvid reader not compiled in this binary.\n"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    if (inputParam->input.type == RGY_INPUT_FMT_AVSW && !ENABLE_AVSW_READER) {
        PrintMes(RGY_LOG_ERROR, _T("avsw reader not compiled in this binary.\n"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }

    RGYInputPrm inputPrm;
    inputPrm.threadCsp = inputParam->threadCsp;
    inputPrm.simdCsp = inputParam->simdCsp;
    RGYInputPrm *pInputPrm = &inputPrm;

    auto subBurnTrack = std::make_unique<SubtitleSelect>();
    SubtitleSelect *subBurnTrackPtr = subBurnTrack.get();
#if ENABLE_AVSW_READER
    RGYInputAvcodecPrm inputInfoAVCuvid(inputPrm);
    DeviceCodecCsp HWDecCodecCsp;
    for (const auto& gpu : m_GPUList) {
        HWDecCodecCsp.push_back(std::make_pair(gpu.id, gpu.cuvid_csp));
    }
#endif

    switch (inputParam->input.type) {
#if ENABLE_AVI_READER
    case RGY_INPUT_FMT_AVI:
        PrintMes(RGY_LOG_DEBUG, _T("avi reader selected.\n"));
        m_pFileReader.reset(new RGYInputAvi());
        break;
#endif //ENABLE_AVI_READER
#if ENABLE_AVISYNTH_READER
    case RGY_INPUT_FMT_AVS:
        PrintMes(RGY_LOG_DEBUG, _T("avs reader selected.\n"));
        m_pFileReader.reset(new RGYInputAvs());
        break;
#endif //ENABLE_AVISYNTH_READER
#if ENABLE_VAPOURSYNTH_READER
    case RGY_INPUT_FMT_VPY:
    case RGY_INPUT_FMT_VPY_MT:
        PrintMes(RGY_LOG_DEBUG, _T("vpy reader selected.\n"));
        m_pFileReader.reset(new RGYInputVpy());
        break;
#endif //ENABLE_VAPOURSYNTH_READER
#if ENABLE_AVSW_READER
    case RGY_INPUT_FMT_AVHW:
    case RGY_INPUT_FMT_AVSW:
    case RGY_INPUT_FMT_AVANY: {
        if (inputParam->nSubtitleSelectCount > 0 && inputParam->vpp.subburn.size() > 0) {
            PrintMes(RGY_LOG_ERROR, _T("--sub-copy and --vpp-subburn should not be set at the same time.\n"));
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
        int subburnTrackId = 0;
        for (const auto &subburn : inputParam->vpp.subburn) {
            if (subburn.trackId > 0) {
                subburnTrackId = subburn.trackId;
                break;
            }
        }
        subBurnTrack->trackID = subburnTrackId;
        inputInfoAVCuvid.threadCsp = inputParam->threadCsp;
        inputInfoAVCuvid.simdCsp = inputParam->simdCsp;
        inputInfoAVCuvid.pInputFormat = inputParam->pAVInputFormat;
        inputInfoAVCuvid.bReadVideo = true;
        inputInfoAVCuvid.nVideoTrack = inputParam->nVideoTrack;
        inputInfoAVCuvid.nVideoStreamId = inputParam->nVideoStreamId;
        inputInfoAVCuvid.nReadAudio = inputParam->nAudioSelectCount > 0;
        inputInfoAVCuvid.bReadSubtitle = (inputParam->nSubtitleSelectCount > 0) || (subburnTrackId > 0);
        inputInfoAVCuvid.bReadData = inputParam->nDataSelectCount > 0;
        inputInfoAVCuvid.bReadChapter = true;
        inputInfoAVCuvid.nVideoAvgFramerate = std::make_pair(inputParam->input.fpsN, inputParam->input.fpsD);
        inputInfoAVCuvid.nAnalyzeSec = inputParam->nAVDemuxAnalyzeSec;
        inputInfoAVCuvid.nTrimCount = inputParam->nTrimCount;
        inputInfoAVCuvid.pTrimList = inputParam->pTrimList;
        inputInfoAVCuvid.nAudioTrackStart = sourceAudioTrackIdStart;
        inputInfoAVCuvid.nSubtitleTrackStart = sourceSubtitleTrackIdStart;
        inputInfoAVCuvid.nDataTrackStart = sourceDataTrackIdStart;
        inputInfoAVCuvid.nAudioSelectCount = inputParam->nAudioSelectCount;
        inputInfoAVCuvid.ppAudioSelect = inputParam->ppAudioSelectList;
        inputInfoAVCuvid.ppSubtitleSelect = (subburnTrackId) ? &subBurnTrackPtr : inputParam->ppSubtitleSelectList;
        inputInfoAVCuvid.nSubtitleSelectCount = (subburnTrackId) ? 1 : inputParam->nSubtitleSelectCount;
        inputInfoAVCuvid.ppDataSelect = inputParam->ppDataSelectList;
        inputInfoAVCuvid.nDataSelectCount = inputParam->nDataSelectCount;
        inputInfoAVCuvid.nProcSpeedLimit = inputParam->nProcSpeedLimit;
        inputInfoAVCuvid.nAVSyncMode = RGY_AVSYNC_ASSUME_CFR;
        inputInfoAVCuvid.fSeekSec = inputParam->fSeekSec;
        inputInfoAVCuvid.pFramePosListLog = inputParam->sFramePosListLog.c_str();
        inputInfoAVCuvid.nInputThread = inputParam->nInputThread;
        inputInfoAVCuvid.pQueueInfo = (m_pPerfMonitor) ? m_pPerfMonitor->GetQueueInfoPtr() : nullptr;
        inputInfoAVCuvid.pHWDecCodecCsp = &HWDecCodecCsp;
        inputInfoAVCuvid.bVideoDetectPulldown = !inputParam->vpp.rff && !inputParam->vpp.afs.enable && inputParam->nAVSyncMode == RGY_AVSYNC_ASSUME_CFR;
        inputInfoAVCuvid.caption2ass = inputParam->caption2ass;
        pInputPrm = &inputInfoAVCuvid;
        PrintMes(RGY_LOG_DEBUG, _T("avhw reader selected.\n"));
        m_pFileReader.reset(new RGYInputAvcodec());
        } break;
#endif //#if ENABLE_AVSW_READER
    case RGY_INPUT_FMT_RAW:
    case RGY_INPUT_FMT_Y4M:
    default: {
        if (inputParam->input.type == RGY_INPUT_FMT_RAW &&
            (inputParam->input.fpsN <= 0 || inputParam->input.fpsD <= 0)) {
            PrintMes(RGY_LOG_ERROR, _T("Please set fps when using raw input.\n"));
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
        PrintMes(RGY_LOG_DEBUG, _T("raw/y4m reader selected.\n"));
        m_pFileReader.reset(new RGYInputRaw());
        break; }
    }
    PrintMes(RGY_LOG_DEBUG, _T("InitInput: input selected : %d.\n"), inputParam->input.type);

    VideoInfo inputParamCopy = inputParam->input;
    m_pStatus.reset(new EncodeStatus());
    int ret = m_pFileReader->Init(inputParam->inputFilename.c_str(), &inputParam->input, pInputPrm, m_pNVLog, m_pStatus);
    if (ret != 0) {
        PrintMes(RGY_LOG_ERROR, m_pFileReader->GetInputMessage());
        return NV_ENC_ERR_GENERIC;
    }
    sourceAudioTrackIdStart    += m_pFileReader->GetAudioTrackCount();
    sourceSubtitleTrackIdStart += m_pFileReader->GetSubtitleTrackCount();
    sourceDataTrackIdStart     += m_pFileReader->GetDataTrackCount();

    //ユーザー指定のオプションを必要に応じて復元する
    inputParam->input.picstruct = inputParamCopy.picstruct;
    if (inputParamCopy.fpsN * inputParamCopy.fpsD > 0) {
        inputParam->input.fpsN = inputParamCopy.fpsN;
        inputParam->input.fpsD = inputParamCopy.fpsD;
    }
    if (inputParamCopy.sar[0] * inputParamCopy.sar[1] > 0) {
        inputParam->input.sar[0] = inputParamCopy.sar[0];
        inputParam->input.sar[1] = inputParamCopy.sar[1];
    }

    m_inputFps = rgy_rational<int>(inputParam->input.fpsN, inputParam->input.fpsD);
    m_outputTimebase = m_inputFps.inv() * rgy_rational<int>(1, 4);
    auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
    if (pAVCodecReader && (m_nAVSyncMode & RGY_AVSYNC_VFR)) {
        //avsync vfr時は、入力streamのtimebaseをそのまま使用する
        m_outputTimebase = to_rgy(pAVCodecReader->GetInputVideoStream()->time_base);
    }

    if (
#if ENABLE_AVSW_READER
        std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader) == nullptr &&
#endif
        inputParam->pTrimList && inputParam->nTrimCount > 0) {
        //avhw/avswリーダー以外は、trimは自分ではセットされないので、ここでセットする
        sTrimParam trimParam;
        trimParam.list = make_vector(inputParam->pTrimList, inputParam->nTrimCount);
        trimParam.offset = 0;
        m_pFileReader->SetTrimParam(trimParam);
    }
    //trim情報をリーダーから取得する
    m_trimParam = m_pFileReader->GetTrimParam();
    if (m_trimParam.list.size() > 0) {
        PrintMes(RGY_LOG_DEBUG, _T("Input: trim options\n"));
        for (int i = 0; i < (int)m_trimParam.list.size(); i++) {
            PrintMes(RGY_LOG_DEBUG, _T("%d-%d "), m_trimParam.list[i].start, m_trimParam.list[i].fin);
        }
        PrintMes(RGY_LOG_DEBUG, _T(" (offset: %d)\n"), m_trimParam.offset);
    }

#if ENABLE_AVSW_READER
    if ((m_nAVSyncMode & (RGY_AVSYNC_VFR | RGY_AVSYNC_FORCE_CFR)) || inputParam->vpp.rff) {
        tstring err_target;
        if (m_nAVSyncMode & RGY_AVSYNC_VFR)       err_target += _T("avsync vfr, ");
        if (m_nAVSyncMode & RGY_AVSYNC_FORCE_CFR) err_target += _T("avsync forcecfr, ");
        if (inputParam->vpp.rff)                  err_target += _T("vpp-rff, ");
        err_target = err_target.substr(0, err_target.length()-2);

        if (pAVCodecReader) {
            //timestampになんらかの問題がある場合、vpp-rffとavsync vfrは使用できない
            const auto timestamp_status = pAVCodecReader->GetFramePosList()->getStreamPtsStatus();
            if ((timestamp_status & (~RGY_PTS_NORMAL)) != 0) {

                tstring err_sts;
                if (timestamp_status & RGY_PTS_SOMETIMES_INVALID) err_sts += _T("SOMETIMES_INVALID, "); //時折、無効なptsを得る
                if (timestamp_status & RGY_PTS_HALF_INVALID)      err_sts += _T("HALF_INVALID, "); //PAFFなため、半分のフレームのptsやdtsが無効
                if (timestamp_status & RGY_PTS_ALL_INVALID)       err_sts += _T("ALL_INVALID, "); //すべてのフレームのptsやdtsが無効
                if (timestamp_status & RGY_PTS_NONKEY_INVALID)    err_sts += _T("NONKEY_INVALID, "); //キーフレーム以外のフレームのptsやdtsが無効
                if (timestamp_status & RGY_PTS_DUPLICATE)         err_sts += _T("PTS_DUPLICATE, "); //重複するpts/dtsが存在する
                if (timestamp_status & RGY_DTS_SOMETIMES_INVALID) err_sts += _T("DTS_SOMETIMES_INVALID, "); //時折、無効なdtsを得る
                err_sts = err_sts.substr(0, err_sts.length()-2);

                PrintMes(RGY_LOG_ERROR, _T("timestamp not acquired successfully from input stream, %s cannot be used. \n  [0x%x] %s\n"),
                    err_target.c_str(), (uint32_t)timestamp_status, err_sts.c_str());
                return NV_ENC_ERR_GENERIC;
            }
            PrintMes(RGY_LOG_DEBUG, _T("timestamp check: 0x%x\n"), timestamp_status);
        } else {
            PrintMes(RGY_LOG_ERROR, _T("%s can only be used with avhw /avsw reader.\n"), err_target.c_str());
            return NV_ENC_ERR_GENERIC;
        }
    } else if (pAVCodecReader && ((pAVCodecReader->GetFramePosList()->getStreamPtsStatus() & (~RGY_PTS_NORMAL)) == 0)) {
        m_nAVSyncMode |= RGY_AVSYNC_VFR;
        const auto timebaseStreamIn = to_rgy(pAVCodecReader->GetInputVideoStream()->time_base);
        if ((timebaseStreamIn.inv() * m_inputFps.inv()).d() == 1 || timebaseStreamIn.n() > 1000) { //fpsを割り切れるtimebaseなら
            if (!inputParam->vpp.afs.enable && !inputParam->vpp.rff) {
                m_outputTimebase = m_inputFps.inv() * rgy_rational<int>(1, 8);
            }
        }
        PrintMes(RGY_LOG_DEBUG, _T("vfr mode automatically enabled with timebase %d/%d\n"), m_outputTimebase.n(), m_outputTimebase.d());
    }

    if (inputParam->nAudioSourceCount > 0) {

        for (int i = 0; i < (int)inputParam->nAudioSourceCount; i++) {
            VideoInfo inputInfo = inputParam->input;

            RGYInputAvcodecPrm inputInfoAVAudioReader(inputPrm);
            inputInfoAVAudioReader.bReadVideo = false;
            inputInfoAVAudioReader.nReadAudio = inputParam->nAudioSourceCount > 0;
            inputInfoAVAudioReader.bReadSubtitle = false;
            inputInfoAVAudioReader.bReadChapter = false;
            inputInfoAVAudioReader.bReadData = false;
            inputInfoAVAudioReader.nVideoAvgFramerate = std::make_pair(m_pStatus->m_sData.outputFPSRate, m_pStatus->m_sData.outputFPSScale);
            inputInfoAVAudioReader.nAnalyzeSec = inputParam->nAVDemuxAnalyzeSec;
            inputInfoAVAudioReader.nTrimCount = inputParam->nTrimCount;
            inputInfoAVAudioReader.pTrimList = inputParam->pTrimList;
            inputInfoAVAudioReader.nAudioTrackStart = sourceAudioTrackIdStart;
            inputInfoAVAudioReader.nSubtitleTrackStart = sourceSubtitleTrackIdStart;
            inputInfoAVAudioReader.nDataTrackStart = sourceDataTrackIdStart;
            inputInfoAVAudioReader.nAudioSelectCount = inputParam->nAudioSelectCount;
            inputInfoAVAudioReader.ppAudioSelect = inputParam->ppAudioSelectList;
            inputInfoAVAudioReader.nProcSpeedLimit = inputParam->nProcSpeedLimit;
            inputInfoAVAudioReader.nAVSyncMode = RGY_AVSYNC_ASSUME_CFR;
            inputInfoAVAudioReader.fSeekSec = inputParam->fSeekSec;
            inputInfoAVAudioReader.pFramePosListLog = inputParam->sFramePosListLog.c_str();
            inputInfoAVAudioReader.nInputThread = 0;

            shared_ptr<RGYInput> audioReader(new RGYInputAvcodec());
            ret = audioReader->Init(inputParam->ppAudioSourceList[i], &inputInfo, &inputInfoAVAudioReader, m_pNVLog, nullptr);
            if (ret != 0) {
                PrintMes(RGY_LOG_ERROR, audioReader->GetInputMessage());
                return NV_ENC_ERR_GENERIC;
            }
            sourceAudioTrackIdStart += audioReader->GetAudioTrackCount();
            sourceSubtitleTrackIdStart += audioReader->GetSubtitleTrackCount();
            sourceDataTrackIdStart += audioReader->GetDataTrackCount();
            m_AudioReaders.push_back(std::move(audioReader));
        }
    }
    if (inputParam->dynamicHdr10plusJson.length() > 0) {
        if (!PathFileExists(inputParam->dynamicHdr10plusJson.c_str())) {
            PrintMes(RGY_LOG_ERROR, _T("Cannot find the file specified : %s.\n"), inputParam->dynamicHdr10plusJson.c_str());
            return NV_ENC_ERR_GENERIC;
        }
        m_hdr10plus = std::make_unique<RGYHDR10Plus>();
        ret = m_hdr10plus->init(inputParam->dynamicHdr10plusJson);
        if (ret == RGY_ERR_NOT_FOUND) {
            PrintMes(RGY_LOG_ERROR, _T("Cannot find \"%s\" required for --dhdr10-info.\n"), RGYHDR10Plus::HDR10PLUS_GEN_EXE_NAME);
            return NV_ENC_ERR_GENERIC;
        } else if (ret != RGY_ERR_NONE) {
            PrintMes(RGY_LOG_ERROR, _T("Failed to initialize hdr10plus reader: %s.\n"), get_err_mes((RGY_ERR)ret));
            return NV_ENC_ERR_GENERIC;
        }
        PrintMes(RGY_LOG_DEBUG, _T("initialized hdr10plus reader: %s\n"), inputParam->dynamicHdr10plusJson.c_str());
    }
#endif //#if ENABLE_AVSW_READER
    return NV_ENC_SUCCESS;
#else
    return NV_ENC_ERR_INVALID_CALL;
#endif //ENABLE_RAW_READER
}
#pragma warning(pop)

NVENCSTATUS NVEncCore::InitOutput(InEncodeVideoParam *inputParams, NV_ENC_BUFFER_FORMAT encBufferFormat) {
    int sts = 0;
    bool stdoutUsed = false;
    const auto outputVideoInfo = videooutputinfo(m_stCodecGUID, encBufferFormat,
        m_uEncWidth, m_uEncHeight,
        &m_stEncConfig, m_stPicStruct,
        std::make_pair(m_sar.n(), m_sar.d()),
        std::make_pair(m_stCreateEncodeParams.frameRateNum, m_stCreateEncodeParams.frameRateDen));
    HEVCHDRSei hedrsei;
    if (hedrsei.parse(inputParams->sMaxCll, inputParams->sMasterDisplay)) {
        PrintMes(RGY_LOG_ERROR, _T("Failed to parse HEVC HDR10 metadata.\n"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
#if ENABLE_AVSW_READER
    vector<int> streamTrackUsed; //使用した音声/字幕のトラックIDを保存する
    bool useH264ESOutput =
        ((inputParams->sAVMuxOutputFormat.length() > 0 && 0 == _tcscmp(inputParams->sAVMuxOutputFormat.c_str(), _T("raw")))) //--formatにrawが指定されている
        || (PathFindExtension(inputParams->outputFilename.c_str()) == nullptr || PathFindExtension(inputParams->outputFilename.c_str())[0] != '.') //拡張子がしない
        || check_ext(inputParams->outputFilename.c_str(), { ".m2v", ".264", ".h264", ".avc", ".avc1", ".x264", ".265", ".h265", ".hevc" }); //特定の拡張子
    if (!useH264ESOutput) {
        inputParams->nAVMux |= RGY_MUX_VIDEO;
    }

    double inputFileDuration = 0.0;
    { auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
        if (pAVCodecReader != nullptr) {
            //caption2ass用の解像度情報の提供
            //これをしないと入力ファイルのデータをずっとバッファし続けるので注意
            pAVCodecReader->setOutputVideoInfo(m_uEncWidth, m_uEncHeight,
                m_sar.n(), m_sar.d(),
                (inputParams->nAVMux & RGY_MUX_VIDEO) != 0);
            inputFileDuration = pAVCodecReader->GetInputVideoDuration();
        }
    }

    m_pStatus->Init(m_encFps.n(), m_encFps.d(), inputParams->input.frames, inputFileDuration, m_trimParam, m_pNVLog, m_pPerfMonitor);
    if (inputParams->nPerfMonitorSelect || inputParams->nPerfMonitorSelectMatplot) {
        m_pPerfMonitor->SetEncStatus(m_pStatus);
    }

    //if (inputParams->CodecId == MFX_CODEC_RAW) {
    //    inputParams->nAVMux &= ~RGY_MUX_VIDEO;
    //}
    bool audioCopyAll = false;
    if (inputParams->nAVMux & RGY_MUX_VIDEO) {
        PrintMes(RGY_LOG_DEBUG, _T("Output: Using avformat writer.\n"));
        m_pFileWriter = std::make_shared<RGYOutputAvcodec>();
        AvcodecWriterPrm writerPrm;
        writerPrm.outputFormat            = inputParams->sAVMuxOutputFormat;
        writerPrm.trimList                = m_trimParam.list;
        writerPrm.bVideoDtsUnavailable    = false;
        writerPrm.nOutputThread           = inputParams->nOutputThread;
        writerPrm.nAudioThread            = inputParams->nAudioThread;
        writerPrm.nBufSizeMB              = inputParams->nOutputBufSizeMB;
        writerPrm.nAudioResampler         = inputParams->nAudioResampler;
        writerPrm.nAudioIgnoreDecodeError = inputParams->nAudioIgnoreDecodeError;
        writerPrm.pQueueInfo = (m_pPerfMonitor) ? m_pPerfMonitor->GetQueueInfoPtr() : nullptr;
        writerPrm.muxVidTsLogFile         = (inputParams->pMuxVidTsLogFile) ? inputParams->pMuxVidTsLogFile : _T("");
        writerPrm.rBitstreamTimebase      = av_make_q(m_outputTimebase);
        writerPrm.pHEVCHdrSei             = &hedrsei;
        writerPrm.videoCodecTag           = inputParams->videoCodecTag;
        if (inputParams->pMuxOpt > 0) {
            writerPrm.vMuxOpt = *inputParams->pMuxOpt;
        }
        auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
        if (pAVCodecReader != nullptr) {
            writerPrm.pInputFormatMetadata = pAVCodecReader->GetInputFormatMetadata();
            if (m_Chapters.size() > 0 && (inputParams->bCopyChapter || inputParams->sChapterFile.length() > 0)) {
                writerPrm.chapterList.clear();
                for (uint32_t i = 0; i < m_Chapters.size(); i++) {
                    writerPrm.chapterList.push_back(m_Chapters[i].get());
                }
            }
            writerPrm.nVideoInputFirstKeyPts = pAVCodecReader->GetVideoFirstKeyPts();
            writerPrm.pVideoInputStream = pAVCodecReader->GetInputVideoStream();
        }
        if (inputParams->nAVMux & (RGY_MUX_AUDIO | RGY_MUX_SUBTITLE)) {
            PrintMes(RGY_LOG_DEBUG, _T("Output: Audio/Subtitle muxing enabled.\n"));
            for (int i = 0; !audioCopyAll && i < inputParams->nAudioSelectCount; i++) {
                //トラック"0"が指定されていれば、すべてのトラックをコピーするということ
                audioCopyAll = (inputParams->ppAudioSelectList[i]->trackID == 0);
            }
            PrintMes(RGY_LOG_DEBUG, _T("Output: CopyAll=%s\n"), (audioCopyAll) ? _T("true") : _T("false"));
            pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
            vector<AVDemuxStream> streamList;
            if (pAVCodecReader) {
                streamList = pAVCodecReader->GetInputStreamInfo();
            }
            for (const auto& audioReader : m_AudioReaders) {
                if (audioReader->GetAudioTrackCount()) {
                    auto pAVCodecAudioReader = std::dynamic_pointer_cast<RGYInputAvcodec>(audioReader);
                    if (pAVCodecAudioReader) {
                        vector_cat(streamList, pAVCodecAudioReader->GetInputStreamInfo());
                    }
                    //もしavqsvリーダーでないなら、音声リーダーから情報を取得する必要がある
                    if (pAVCodecReader == nullptr) {
                        writerPrm.nVideoInputFirstKeyPts = pAVCodecAudioReader->GetVideoFirstKeyPts();
                        writerPrm.pVideoInputStream = pAVCodecAudioReader->GetInputVideoStream();
                    }
                }
            }

            for (auto& stream : streamList) {
                const auto streamMediaType = trackMediaType(stream.nTrackId);
                //audio-fileで別ファイルとして抽出するものは除く
                bool usedInAudioFile = false;
                for (int i = 0; i < (int)inputParams->nAudioSelectCount; i++) {
                    if (trackID(stream.nTrackId) == inputParams->ppAudioSelectList[i]->trackID
                        && inputParams->ppAudioSelectList[i]->extractFilename.length() > 0) {
                        usedInAudioFile = true;
                    }
                }
                if (usedInAudioFile) {
                    continue;
                }
                const AudioSelect *pAudioSelect = nullptr;
                for (int i = 0; i < (int)inputParams->nAudioSelectCount; i++) {
                    if (trackID(stream.nTrackId) == inputParams->ppAudioSelectList[i]->trackID
                        && inputParams->ppAudioSelectList[i]->extractFilename.length() == 0) {
                        pAudioSelect = inputParams->ppAudioSelectList[i];
                    }
                }
                if (pAudioSelect == nullptr) {
                    //一致するTrackIDがなければ、trackID = 0 (全指定)を探す
                    for (int i = 0; i < inputParams->nAudioSelectCount; i++) {
                        if (inputParams->ppAudioSelectList[i]->trackID == 0
                            && inputParams->ppAudioSelectList[i]->extractFilename.length() == 0) {
                            pAudioSelect = inputParams->ppAudioSelectList[i];
                        }
                    }
                }
                const SubtitleSelect *pSubtitleSelect = nullptr;
                if (streamMediaType == AVMEDIA_TYPE_SUBTITLE) {
                    for (int i = 0; i < inputParams->nSubtitleSelectCount; i++) {
                        if (trackID(stream.nTrackId) == inputParams->ppSubtitleSelectList[i]->trackID) {
                            pSubtitleSelect = inputParams->ppSubtitleSelectList[i];
                        }
                    }
                    if (pSubtitleSelect == nullptr) {
                        //一致するTrackIDがなければ、trackID = 0 (全指定)を探す
                        for (int i = 0; i < inputParams->nSubtitleSelectCount; i++) {
                            if (inputParams->ppAudioSelectList[i]->trackID == 0) {
                                pSubtitleSelect = inputParams->ppSubtitleSelectList[i];
                                break;
                            }
                        }
                    }
                }
                const DataSelect *pDataSelect = nullptr;
                if (streamMediaType == AVMEDIA_TYPE_DATA) {
                    for (int i = 0; i < inputParams->nDataSelectCount; i++) {
                        if (trackID(stream.nTrackId) == inputParams->ppDataSelectList[i]->trackID) {
                            pDataSelect = inputParams->ppDataSelectList[i];
                        }
                    }
                    if (pSubtitleSelect == nullptr) {
                        //一致するTrackIDがなければ、trackID = 0 (全指定)を探す
                        for (int i = 0; i < inputParams->nDataSelectCount; i++) {
                            if (inputParams->ppDataSelectList[i]->trackID == 0) {
                                pDataSelect = inputParams->ppDataSelectList[i];
                                break;
                            }
                        }
                    }
                }
                if (pAudioSelect != nullptr || audioCopyAll || streamMediaType != AVMEDIA_TYPE_AUDIO) {
                    streamTrackUsed.push_back(stream.nTrackId);
                    int subburnTrackId = 0;
                    for (const auto &subburn : inputParams->vpp.subburn) {
                        if (subburn.trackId > 0) {
                            subburnTrackId = subburn.trackId;
                            break;
                        }
                    }
                    if (streamMediaType == AVMEDIA_TYPE_SUBTITLE && subburnTrackId > 0) {
                        continue;
                    }
                    AVOutputStreamPrm prm;
                    prm.src = stream;
                    //pAudioSelect == nullptrは "copyAllStreams" か 字幕ストリーム によるもの
                    if (pAudioSelect != nullptr) {
                        prm.decodeCodecPrm = pAudioSelect->decCodecPrm;
                        prm.bitrate = pAudioSelect->encBitrate;
                        prm.samplingRate = pAudioSelect->encSamplingRate;
                        prm.encodeCodec = pAudioSelect->encCodec;
                        prm.encodeCodecPrm = pAudioSelect->encCodecPrm;
                        prm.encodeCodecProfile = pAudioSelect->encCodecProfile;
                        prm.filter = pAudioSelect->filter;
                    }
                    if (pSubtitleSelect != nullptr) {
                        prm.encodeCodec = pSubtitleSelect->encCodec;
                        prm.encodeCodecPrm = pSubtitleSelect->encCodecPrm;
                        prm.asdata = pSubtitleSelect->asdata;
                    }
                    PrintMes(RGY_LOG_DEBUG, _T("Output: Added %s track#%d (stream idx %d) for mux, bitrate %d, codec: %s %s %s\n"),
                        char_to_tstring(av_get_media_type_string(streamMediaType)).c_str(),
                        stream.nTrackId, stream.nIndex, prm.bitrate, prm.encodeCodec.c_str(),
                        prm.encodeCodecProfile.c_str(),
                        prm.encodeCodecPrm.c_str());
                    writerPrm.inputStreamList.push_back(std::move(prm));
                }
            }
        }
        sts = m_pFileWriter->Init(inputParams->outputFilename.c_str(), &outputVideoInfo, &writerPrm, m_pNVLog, m_pStatus);
        if (sts != 0) {
            PrintMes(RGY_LOG_ERROR, m_pFileWriter->GetOutputMessage());
            return NV_ENC_ERR_GENERIC;
        } else if (inputParams->nAVMux & (RGY_MUX_AUDIO | RGY_MUX_SUBTITLE)) {
            m_pFileWriterListAudio.push_back(m_pFileWriter);
        }
        stdoutUsed = m_pFileWriter->outputStdout();
        PrintMes(RGY_LOG_DEBUG, _T("Output: Initialized avformat writer%s.\n"), (stdoutUsed) ? _T("using stdout") : _T(""));
    } else if (inputParams->nAVMux & (RGY_MUX_AUDIO | RGY_MUX_SUBTITLE)) {
        PrintMes(RGY_LOG_ERROR, _T("Audio mux cannot be used alone, should be use with video mux.\n"));
        return NV_ENC_ERR_GENERIC;
    } else {
#endif //ENABLE_AVSW_READER
        m_pFileWriter = std::make_shared<RGYOutputRaw>();
        RGYOutputRawPrm rawPrm;
        rawPrm.nBufSizeMB = inputParams->nOutputBufSizeMB;
        rawPrm.bBenchmark = false;
        rawPrm.codecId = inputParams->codec == NV_ENC_H264 ? RGY_CODEC_H264 : RGY_CODEC_HEVC;
        rawPrm.seiNal = hedrsei.gen_nal();
        sts = m_pFileWriter->Init(inputParams->outputFilename.c_str(), &outputVideoInfo, &rawPrm, m_pNVLog, m_pStatus);
        if (sts != 0) {
            PrintMes(RGY_LOG_ERROR, m_pFileWriter->GetOutputMessage());
            return NV_ENC_ERR_GENERIC;
        }
        stdoutUsed = m_pFileWriter->outputStdout();
        PrintMes(RGY_LOG_DEBUG, _T("Output: Initialized bitstream writer%s.\n"), (stdoutUsed) ? _T("using stdout") : _T(""));
#if ENABLE_AVSW_READER
    }

    //音声の抽出
    if (inputParams->nAudioSelectCount + inputParams->nSubtitleSelectCount - (audioCopyAll ? 1 : 0) > (int)streamTrackUsed.size()) {
        PrintMes(RGY_LOG_DEBUG, _T("Output: Audio file output enabled.\n"));
        auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
        if (pAVCodecReader == nullptr) {
            PrintMes(RGY_LOG_ERROR, _T("Audio output is only supported with transcoding (avhw/avsw reader).\n"));
            return NV_ENC_ERR_GENERIC;
        } else {
            auto inutAudioInfoList = pAVCodecReader->GetInputStreamInfo();
            for (auto& audioTrack : inutAudioInfoList) {
                bool bTrackAlreadyUsed = false;
                for (auto usedTrack : streamTrackUsed) {
                    if (usedTrack == audioTrack.nTrackId) {
                        bTrackAlreadyUsed = true;
                        PrintMes(RGY_LOG_DEBUG, _T("Audio track #%d is already set to be muxed, so cannot be extracted to file.\n"), trackID(audioTrack.nTrackId));
                        break;
                    }
                }
                if (bTrackAlreadyUsed) {
                    continue;
                }
                const AudioSelect *pAudioSelect = nullptr;
                for (int i = 0; i < (int)inputParams->nAudioSelectCount; i++) {
                    if (trackID(audioTrack.nTrackId) == inputParams->ppAudioSelectList[i]->trackID
                        && inputParams->ppAudioSelectList[i]->extractFilename.length() > 0) {
                        pAudioSelect = inputParams->ppAudioSelectList[i];
                    }
                }
                if (pAudioSelect == nullptr) {
                    PrintMes(RGY_LOG_ERROR, _T("Audio track #%d is not used anyware, this should not happen.\n"), trackID(audioTrack.nTrackId));
                    return NV_ENC_ERR_GENERIC;
                }
                PrintMes(RGY_LOG_DEBUG, _T("Output: Output audio track #%d (stream index %d) to \"%s\", format: %s, codec %s, bitrate %d\n"),
                    trackID(audioTrack.nTrackId), audioTrack.nIndex, pAudioSelect->extractFilename.c_str(), pAudioSelect->extractFormat.c_str(), pAudioSelect->encCodec.c_str(), pAudioSelect->encBitrate);

                AVOutputStreamPrm prm;
                prm.src = audioTrack;
                //pAudioSelect == nullptrは "copyAll" によるもの
                prm.bitrate = pAudioSelect->encBitrate;
                prm.filter = pAudioSelect->filter;
                prm.encodeCodec = pAudioSelect->encCodec;
                prm.samplingRate = pAudioSelect->encSamplingRate;

                AvcodecWriterPrm writerAudioPrm;
                writerAudioPrm.nOutputThread   = inputParams->nOutputThread;
                writerAudioPrm.nAudioThread    = inputParams->nAudioThread;
                writerAudioPrm.nBufSizeMB      = inputParams->nOutputBufSizeMB;
                writerAudioPrm.outputFormat   = pAudioSelect->extractFormat;
                writerAudioPrm.nAudioIgnoreDecodeError = inputParams->nAudioIgnoreDecodeError;
                writerAudioPrm.nAudioResampler = inputParams->nAudioResampler;
                writerAudioPrm.inputStreamList.push_back(prm);
                writerAudioPrm.trimList = m_trimParam.list;
                writerAudioPrm.nVideoInputFirstKeyPts = pAVCodecReader->GetVideoFirstKeyPts();
                writerAudioPrm.pVideoInputStream = pAVCodecReader->GetInputVideoStream();
                writerAudioPrm.rBitstreamTimebase = av_make_q(m_outputTimebase);

                shared_ptr<RGYOutput> pWriter = std::make_shared<RGYOutputAvcodec>();
                sts = pWriter->Init(pAudioSelect->extractFilename.c_str(), nullptr, &writerAudioPrm, m_pNVLog, m_pStatus);
                if (sts != 0) {
                    PrintMes(RGY_LOG_ERROR, pWriter->GetOutputMessage());
                    return NV_ENC_ERR_GENERIC;
                }
                PrintMes(RGY_LOG_DEBUG, _T("Output: Intialized audio output for track #%d.\n"), trackID(audioTrack.nTrackId));
                bool audioStdout = pWriter->outputStdout();
                if (stdoutUsed && audioStdout) {
                    PrintMes(RGY_LOG_ERROR, _T("Multiple stream outputs are set to stdout, please remove conflict.\n"));
                    return NV_ENC_ERR_GENERIC;
                }
                stdoutUsed |= audioStdout;
                m_pFileWriterListAudio.push_back(std::move(pWriter));
            }
        }
    }
#endif //ENABLE_AVSW_READER
    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVEncCore::InitCuda(int cudaSchedule) {
    //ひとまず、これまでのすべてのエラーをflush
    auto cudaerr = cudaGetLastError();
    PrintMes(RGY_LOG_DEBUG, _T("InitCuda: device #%d.\n"), m_nDeviceId);

    PrintMes(RGY_LOG_DEBUG, _T("\n"), m_nDeviceId);
    PrintMes(RGY_LOG_DEBUG, _T("Checking Environment Info...\n"));
    PrintMes(RGY_LOG_DEBUG, _T("%s\n"), get_encoder_version());

    OSVERSIONINFOEXW osversioninfo = { 0 };
    tstring osversionstr = getOSVersion(&osversioninfo);
    PrintMes(RGY_LOG_DEBUG, _T("OS Version     %s %s (%d)\n"), osversionstr.c_str(), rgy_is_64bit_os() ? _T("x64") : _T("x86"), osversioninfo.dwBuildNumber);

    TCHAR cpu_info[1024] = { 0 };
    getCPUInfo(cpu_info, _countof(cpu_info));
    PrintMes(RGY_LOG_DEBUG, _T("CPU            %s\n"), cpu_info);

    TCHAR gpu_info[1024] = { 0 };
    const auto len = _stprintf_s(gpu_info, _T("#%d: "), m_nDeviceId);
    if (0 == m_GPUList.size()) {
        //NVEncFeatureから呼ばれたとき、ここに入る
        //NVEncFeatureが再帰的に呼び出すのを避けるため、getFeatures = falseで呼び出す
        NVEncoderGPUInfo gpuInfo(m_nDeviceId, false);
        m_GPUList = gpuInfo.getGPUList();
        if (0 == m_GPUList.size()) {
            //NVEncFeatureが再帰的に呼び出すのを避けるため、getFeatures = falseで呼び出す
            gpuInfo = NVEncoderGPUInfo(-1, false);
            m_GPUList = gpuInfo.getGPUList();
            if (0 == m_GPUList.size()) {
                PrintMes(RGY_LOG_ERROR, _T("No GPU found suitable for NVEnc Encoding.\n"));
                return NV_ENC_ERR_NO_ENCODE_DEVICE;
            } else {
                PrintMes(RGY_LOG_WARN, _T("DeviceId #%d not found, automatically selected default device.\n"), m_nDeviceId);
                m_nDeviceId = 0;
            }
        }
    }
    const auto gpuInfo = std::find_if(m_GPUList.begin(), m_GPUList.end(), [device_id = m_nDeviceId](const NVGPUInfo& info) { return info.id == device_id; });
    if (gpuInfo != m_GPUList.end()) {
        if (m_nDeviceId == gpuInfo->id) {
            _stprintf_s(gpu_info, _T("#%d: %s (%d.%d)"), gpuInfo->id, gpuInfo->name.c_str(),
                gpuInfo->nv_driver_version / 1000, (gpuInfo->nv_driver_version % 1000) / 10);
            PrintMes(RGY_LOG_DEBUG, _T("GPU            %s\n"), gpu_info);
            if (0 < gpuInfo->nv_driver_version && gpuInfo->nv_driver_version < NV_DRIVER_VER_MIN) {
                PrintMes(RGY_LOG_ERROR, _T("Insufficient NVIDIA driver version, Required %d.%d, Installed %d.%d\n"),
                    NV_DRIVER_VER_MIN / 1000, (NV_DRIVER_VER_MIN % 1000) / 10,
                    gpuInfo->nv_driver_version / 1000, (gpuInfo->nv_driver_version % 1000) / 10);
                return NV_ENC_ERR_NO_ENCODE_DEVICE;
            }
            PrintMes(RGY_LOG_DEBUG, _T("NVENC / CUDA   NVENC API %d.%d, CUDA %d.%d, schedule mode: %s\n"),
                NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION,
                gpuInfo->cuda_driver_version / 1000, (gpuInfo->cuda_driver_version % 1000) / 10, get_chr_from_value(list_cuda_schedule, m_cudaSchedule));
        }
    } else {
        PrintMes(RGY_LOG_ERROR, _T("Failed to check NVIDIA driver version.\n"));
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }

    //ひとまず、これまでのすべてのエラーをflush
    cudaerr = cudaGetLastError();

    CUresult cuResult;
    if (CUDA_SUCCESS != (cuResult = cuInit(0))) {
        PrintMes(RGY_LOG_ERROR, _T("cuInit error:0x%x (%s)\n"), cuResult, char_to_tstring(_cudaGetErrorEnum(cuResult)).c_str());
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }
    PrintMes(RGY_LOG_DEBUG, _T("cuInit: Success.\n"));
    int deviceCount = 0;
    if (CUDA_SUCCESS != (cuResult = cuDeviceGetCount(&deviceCount))) {
        PrintMes(RGY_LOG_ERROR, _T("cuDeviceGetCount error:0x%x (%s)\n"), cuResult, char_to_tstring(_cudaGetErrorEnum(cuResult)).c_str());
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }
    PrintMes(RGY_LOG_DEBUG, _T("cuDeviceGetCount: Success.\n"));

    if (m_nDeviceId > deviceCount - 1) {
        PrintMes(RGY_LOG_ERROR, _T("Invalid Device Id = %d\n"), m_nDeviceId);
        return NV_ENC_ERR_INVALID_ENCODERDEVICE;
    }

    if (CUDA_SUCCESS != (cuResult = cuDeviceGet(&m_device, m_nDeviceId))) {
        PrintMes(RGY_LOG_ERROR, _T("cuDeviceGet error:0x%x (%s)\n"), cuResult, char_to_tstring(_cudaGetErrorEnum(cuResult)).c_str());
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }
    PrintMes(RGY_LOG_DEBUG, _T("cuDeviceGet: ID:%d.\n"), m_nDeviceId);

    int SMminor = 0, SMmajor = 0;
    if (CUDA_SUCCESS != (cuDeviceComputeCapability(&SMmajor, &SMminor, m_device))) {
        PrintMes(RGY_LOG_ERROR, _T("cuDeviceComputeCapability error:0x%x (%s)\n"), cuResult, char_to_tstring(_cudaGetErrorEnum(cuResult)).c_str());
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }
    PrintMes(RGY_LOG_DEBUG, _T("cuDeviceComputeCapability: Success: %d.%d.\n"), SMmajor, SMminor);

    if (((SMmajor << 4) + SMminor) < 0x30) {
        PrintMes(RGY_LOG_ERROR, _T("GPU %d does not have NVENC capabilities exiting\n"), m_nDeviceId);
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }
    PrintMes(RGY_LOG_DEBUG, _T("NVENC capabilities: OK.\n"));

    m_cudaSchedule = (CUctx_flags)(cudaSchedule & CU_CTX_SCHED_MASK);
    PrintMes(RGY_LOG_DEBUG, _T("using cuda schedule mode: %s.\n"), get_chr_from_value(list_cuda_schedule, m_cudaSchedule));
    if (CUDA_SUCCESS != (cuResult = cuCtxCreate((CUcontext*)(&m_pDevice), m_cudaSchedule, m_device))) {
        if (m_cudaSchedule != 0) {
            PrintMes(RGY_LOG_WARN, _T("cuCtxCreate error:0x%x (%s)\n"), cuResult, char_to_tstring(_cudaGetErrorEnum(cuResult)).c_str());
            PrintMes(RGY_LOG_WARN, _T("retry cuCtxCreate with auto scheduling mode.\n"));
            if (CUDA_SUCCESS != (cuResult = cuCtxCreate((CUcontext*)(&m_pDevice), 0, m_device))) {
                PrintMes(RGY_LOG_ERROR, _T("cuCtxCreate error:0x%x (%s)\n"), cuResult, char_to_tstring(_cudaGetErrorEnum(cuResult)).c_str());
                return NV_ENC_ERR_NO_ENCODE_DEVICE;
            }
        } else {
            PrintMes(RGY_LOG_ERROR, _T("cuCtxCreate error:0x%x (%s)\n"), cuResult, char_to_tstring(_cudaGetErrorEnum(cuResult)).c_str());
            return NV_ENC_ERR_NO_ENCODE_DEVICE;
        }
    }
    PrintMes(RGY_LOG_DEBUG, _T("cuCtxCreate: Success.\n"));

#if ENABLE_AVSW_READER
    if (CUDA_SUCCESS != (cuResult = cuCtxPopCurrent(&m_cuContextCurr))) {
        PrintMes(RGY_LOG_ERROR, _T("cuCtxPopCurrent error:0x%x (%s)\n"), cuResult, char_to_tstring(_cudaGetErrorEnum(cuResult)).c_str());
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }
    PrintMes(RGY_LOG_DEBUG, _T("cuCtxPopCurrent: Success.\n"));

    if (CUDA_SUCCESS != (cuResult = cuvidInit(0))) {
        PrintMes(RGY_LOG_ERROR, _T("cuvidInit error:0x%x (%s)\n"), cuResult, char_to_tstring(_cudaGetErrorEnum(cuResult)).c_str());
        return NV_ENC_ERR_UNSUPPORTED_DEVICE;
    }
    PrintMes(RGY_LOG_DEBUG, _T("cuvidInit: Success.\n"));

    if (CUDA_SUCCESS != (cuResult = cuvidCtxLockCreate(&m_ctxLock, m_cuContextCurr))) {
        PrintMes(RGY_LOG_ERROR, _T("Failed cuvidCtxLockCreate: 0x%x (%s)\n"), cuResult, char_to_tstring(_cudaGetErrorEnum(cuResult)).c_str());
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }
    PrintMes(RGY_LOG_DEBUG, _T("cuvidCtxLockCreate: Success.\n"));
#endif //#if ENABLE_AVSW_READER
    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVEncCore::NvEncCreateInputBuffer(uint32_t width, uint32_t height, void **inputBuffer, NV_ENC_BUFFER_FORMAT inputFormat) {
    NV_ENC_CREATE_INPUT_BUFFER createInputBufferParams;
    INIT_CONFIG(createInputBufferParams, NV_ENC_CREATE_INPUT_BUFFER);

    createInputBufferParams.width = width;
    createInputBufferParams.height = height;
    createInputBufferParams.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;
    createInputBufferParams.bufferFmt = inputFormat;

    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncCreateInputBuffer(m_hEncoder, &createInputBufferParams);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncCreateInputBuffer"), nvStatus);
        return nvStatus;
    }

    *inputBuffer = createInputBufferParams.inputBuffer;

    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncDestroyInputBuffer(NV_ENC_INPUT_PTR inputBuffer) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
    if (inputBuffer) {
        nvStatus = m_pEncodeAPI->nvEncDestroyInputBuffer(m_hEncoder, inputBuffer);
        if (nvStatus != NV_ENC_SUCCESS) {
            NVPrintFuncError(_T("nvEncDestroyInputBuffer"), nvStatus);
            return nvStatus;
        }
    }
    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncCreateBitstreamBuffer(uint32_t size, void **bitstreamBuffer) {
    UNREFERENCED_PARAMETER(size);
    NV_ENC_CREATE_BITSTREAM_BUFFER createBitstreamBufferParams;
    INIT_CONFIG(createBitstreamBufferParams, NV_ENC_CREATE_BITSTREAM_BUFFER);

    //ここでは特に指定せず、ドライバにバッファサイズを決めさせる
    //createBitstreamBufferParams.size = size;
    //createBitstreamBufferParams.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;

    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncCreateBitstreamBuffer(m_hEncoder, &createBitstreamBufferParams);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncCreateBitstreamBuffer"), nvStatus);
        return nvStatus;
    }

    *bitstreamBuffer = createBitstreamBufferParams.bitstreamBuffer;

    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncDestroyBitstreamBuffer(NV_ENC_OUTPUT_PTR bitstreamBuffer) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
    if (bitstreamBuffer) {
        nvStatus = m_pEncodeAPI->nvEncDestroyBitstreamBuffer(m_hEncoder, bitstreamBuffer);
        if (nvStatus != NV_ENC_SUCCESS) {
            NVPrintFuncError(_T("nvEncDestroyBitstreamBuffer"), nvStatus);
            return nvStatus;
        }
    }
    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncLockBitstream(NV_ENC_LOCK_BITSTREAM *lockBitstreamBufferParams) {
    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncLockBitstream(m_hEncoder, lockBitstreamBufferParams);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncLockBitstream"), nvStatus);
        return nvStatus;
    }
    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncUnlockBitstream(NV_ENC_OUTPUT_PTR bitstreamBuffer) {
    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncUnlockBitstream(m_hEncoder, bitstreamBuffer);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncUnlockBitstream"), nvStatus);
        return nvStatus;
    }
    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncLockInputBuffer(void *inputBuffer, void **bufferDataPtr, uint32_t *pitch) {
    NV_ENC_LOCK_INPUT_BUFFER lockInputBufferParams;
    INIT_CONFIG(lockInputBufferParams, NV_ENC_LOCK_INPUT_BUFFER);

    lockInputBufferParams.inputBuffer = inputBuffer;
    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncLockInputBuffer(m_hEncoder, &lockInputBufferParams);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncLockInputBuffer"), nvStatus);
        return nvStatus;
    }

    *bufferDataPtr = lockInputBufferParams.bufferDataPtr;
    *pitch = lockInputBufferParams.pitch;

    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncUnlockInputBuffer(NV_ENC_INPUT_PTR inputBuffer) {
    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncUnlockInputBuffer(m_hEncoder, inputBuffer);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncUnlockInputBuffer"), nvStatus);
        return nvStatus;
    }
    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncGetEncodeStats(NV_ENC_STAT *encodeStats) {
    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncGetEncodeStats(m_hEncoder, encodeStats);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncGetEncodeStats"), nvStatus);
        return nvStatus;
    }

    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncGetSequenceParams(NV_ENC_SEQUENCE_PARAM_PAYLOAD *sequenceParamPayload) {
    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncGetSequenceParams(m_hEncoder, sequenceParamPayload);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncGetSequenceParams"), nvStatus);
        return nvStatus;
    }

    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncRegisterAsyncEvent(void **completionEvent) {
    NV_ENC_EVENT_PARAMS eventParams;
    INIT_CONFIG(eventParams, NV_ENC_EVENT_PARAMS);

    eventParams.completionEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncRegisterAsyncEvent(m_hEncoder, &eventParams);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncRegisterAsyncEvent"), nvStatus);
        return nvStatus;
    }

    *completionEvent = eventParams.completionEvent;

    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncUnregisterAsyncEvent(void *completionEvent) {
    if (completionEvent) {
        NV_ENC_EVENT_PARAMS eventParams;
        INIT_CONFIG(eventParams, NV_ENC_EVENT_PARAMS);

        eventParams.completionEvent = completionEvent;

        NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncUnregisterAsyncEvent(m_hEncoder, &eventParams);
        if (nvStatus != NV_ENC_SUCCESS) {
            NVPrintFuncError(_T("nvEncUnregisterAsyncEvent"), nvStatus);
            return nvStatus;
        }
    }

    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVEncCore::NvEncRegisterResource(NV_ENC_INPUT_RESOURCE_TYPE resourceType, void* resourceToRegister, uint32_t width, uint32_t height, uint32_t pitch, NV_ENC_BUFFER_FORMAT inputFormat, void** registeredResource) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
    NV_ENC_REGISTER_RESOURCE registerResParams;

    INIT_CONFIG(registerResParams, NV_ENC_REGISTER_RESOURCE);

    registerResParams.resourceType = resourceType;
    registerResParams.resourceToRegister = resourceToRegister;
    registerResParams.width = width;
    registerResParams.height = height;
    registerResParams.pitch = pitch;
    registerResParams.bufferFormat = inputFormat;
    registerResParams.bufferUsage = NV_ENC_INPUT_IMAGE;

    nvStatus = m_pEncodeAPI->nvEncRegisterResource(m_hEncoder, &registerResParams);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncRegisterResource"), nvStatus);
    }

    *registeredResource = registerResParams.registeredResource;

    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncUnregisterResource(NV_ENC_REGISTERED_PTR registeredRes) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;

    nvStatus = m_pEncodeAPI->nvEncUnregisterResource(m_hEncoder, registeredRes);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncUnregisterResource"), nvStatus);
    }
    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncMapInputResource(void *registeredResource, void **mappedResource) {
    NV_ENC_MAP_INPUT_RESOURCE mapInputResParams;
    INIT_CONFIG(mapInputResParams, NV_ENC_MAP_INPUT_RESOURCE);

    mapInputResParams.registeredResource = registeredResource;

    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncMapInputResource(m_hEncoder, &mapInputResParams);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncMapInputResource"), nvStatus);
        return nvStatus;
    }

    *mappedResource = mapInputResParams.mappedResource;

    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncUnmapInputResource(NV_ENC_INPUT_PTR mappedInputBuffer) {
    if (mappedInputBuffer) {
        NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncUnmapInputResource(m_hEncoder, mappedInputBuffer);
        if (nvStatus != NV_ENC_SUCCESS) {
            NVPrintFuncError(_T("nvEncUnmapInputResource"), nvStatus);
            return nvStatus;
        }
    }

    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVEncCore::NvEncDestroyEncoder() {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;

    if (m_hEncoder && m_pEncodeAPI) {
        nvStatus = m_pEncodeAPI->nvEncDestroyEncoder(m_hEncoder);
        m_hEncoder = NULL;
        m_pEncodeAPI = nullptr;
        PrintMes(RGY_LOG_DEBUG, _T("nvEncDestroyEncoder: success.\n"));
    }

    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncFlushEncoderQueue(void *hEOSEvent) {
    NV_ENC_PIC_PARAMS encPicParams;
    INIT_CONFIG(encPicParams, NV_ENC_PIC_PARAMS);
    encPicParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    encPicParams.completionEvent = hEOSEvent;

    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncEncodePicture(m_hEncoder, &encPicParams);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("nvEncEncodePicture"), nvStatus);
        return nvStatus;
    }
    return nvStatus;
}

NVENCSTATUS NVEncCore::ProcessOutput(const EncodeBuffer *pEncodeBuffer) {
    if (pEncodeBuffer->stOutputBfr.hBitstreamBuffer == NULL && pEncodeBuffer->stOutputBfr.bEOSFlag == FALSE) {
        return NV_ENC_ERR_INVALID_PARAM;
    }

    if (pEncodeBuffer->stOutputBfr.bWaitOnEvent == TRUE) {
        if (!pEncodeBuffer->stOutputBfr.hOutputEvent) {
            return NV_ENC_ERR_INVALID_PARAM;
        }
        NVTXRANGE(ProcessOutputWait);
        WaitForSingleObject(pEncodeBuffer->stOutputBfr.hOutputEvent, INFINITE);
    }

    if (pEncodeBuffer->stOutputBfr.bEOSFlag)
        return NV_ENC_SUCCESS;

    NVTXRANGE(ProcessOutput);
    NV_ENC_LOCK_BITSTREAM lockBitstreamData;
    INIT_CONFIG(lockBitstreamData, NV_ENC_LOCK_BITSTREAM);
    lockBitstreamData.outputBitstream = pEncodeBuffer->stOutputBfr.hBitstreamBuffer;
    lockBitstreamData.doNotWait = false;

    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncLockBitstream(m_hEncoder, &lockBitstreamData);
    if (nvStatus == NV_ENC_SUCCESS) {
        RGYBitstream bitstream = RGYBitstreamInit(lockBitstreamData);
        m_pFileWriter->WriteNextFrame(&bitstream);
        nvStatus = m_pEncodeAPI->nvEncUnlockBitstream(m_hEncoder, pEncodeBuffer->stOutputBfr.hBitstreamBuffer);
    } else {
        NVPrintFuncError(_T("nvEncLockBitstream"), nvStatus);
        return nvStatus;
    }

    return nvStatus;
}

NVENCSTATUS NVEncCore::FlushEncoder() {
    NVENCSTATUS nvStatus = NvEncFlushEncoderQueue(m_stEOSOutputBfr.hOutputEvent);
    if (nvStatus != NV_ENC_SUCCESS) {
        NVPrintFuncError(_T("NvEncFlushEncoderQueue"), nvStatus);
        return nvStatus;
    }

    EncodeBuffer *pEncodeBufer = m_EncodeBufferQueue.GetPending();
    while (pEncodeBufer) {
        ProcessOutput(pEncodeBufer);
        pEncodeBufer = m_EncodeBufferQueue.GetPending();
    }

    if (WaitForSingleObject(m_stEOSOutputBfr.hOutputEvent, 500) != WAIT_OBJECT_0) {
        PrintMes(RGY_LOG_ERROR, _T("m_stEOSOutputBfr.hOutputEvent%s"), (FOR_AUO) ? _T("が終了しません。") : _T(" does not finish within proper time."));
        nvStatus = NV_ENC_ERR_GENERIC;
    }

    return nvStatus;
}

NVENCSTATUS NVEncCore::Deinitialize() {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;

    m_hdr10plus.reset();
    m_AudioReaders.clear();
    m_pFileReader.reset();
    m_pFileWriter.reset();
    m_pFileWriterListAudio.clear();

    if (m_vpFilters.size()) {
        NVEncCtxAutoLock(ctxlock(m_ctxLock));
        m_vpFilters.clear();
    }
    ReleaseIOBuffers();

    nvStatus = NvEncDestroyEncoder();

#if ENABLE_AVSW_READER
    m_cuvidDec.reset();

    if (m_ctxLock) {
        cuvidCtxLockDestroy(m_ctxLock);
        m_ctxLock = nullptr;
    }
    m_keyFile.clear();
#endif //#if ENABLE_AVSW_READER

    m_pStatus.reset();

    if (m_pDevice) {
        CUresult cuResult = CUDA_SUCCESS;
        cuResult = cuCtxDestroy((CUcontext)m_pDevice);
        if (cuResult != CUDA_SUCCESS)
            PrintMes(RGY_LOG_ERROR, _T("cuCtxDestroy error:0x%x: %s\n"), cuResult, char_to_tstring(_cudaGetErrorEnum(cuResult)).c_str());

        m_pDevice = NULL;
    }

    PrintMes(RGY_LOG_DEBUG, _T("Closing perf monitor...\n"));
    m_pPerfMonitor.reset();

    m_pNVLog.reset();
    m_pAbortByUser = nullptr;
    m_trimParam.list.clear();
    m_trimParam.offset = 0;
    //すべてのエラーをflush - 次回に影響しないように
    auto cudaerr = cudaGetLastError();
    UNREFERENCED_PARAMETER(cudaerr);
    return nvStatus;
}

NVENCSTATUS NVEncCore::AllocateIOBuffers(uint32_t uInputWidth, uint32_t uInputHeight, NV_ENC_BUFFER_FORMAT inputFormat, const VideoInfo *pInputInfo) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;

    m_EncodeBufferQueue.Initialize(m_stEncodeBuffer, m_encodeBufferCount);
    uint32_t uInputWidthByte = 0;
    uint32_t uInputHeightTotal = 0;
    switch (inputFormat) {
    case NV_ENC_BUFFER_FORMAT_UNDEFINED: /**< Undefined buffer format */
    case NV_ENC_BUFFER_FORMAT_YV12:      /**< Planar YUV [Y plane followed by V and U planes] */
    case NV_ENC_BUFFER_FORMAT_IYUV:      /**< Planar YUV [Y plane followed by U and V planes] */
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    case NV_ENC_BUFFER_FORMAT_YUV444:    /**< Planar YUV [Y plane followed by U and V planes] */
        uInputWidthByte = uInputWidth;
        uInputHeightTotal = uInputHeight * 3;
        break;
    case NV_ENC_BUFFER_FORMAT_YUV420_10BIT: /**< 10 bit Semi-Planar YUV [Y plane followed by interleaved UV plane]. Each pixel of size 2 bytes. Most Significant 10 bits contain pixel data. */
        uInputWidthByte = uInputWidth * 2;
        uInputHeightTotal = uInputHeight * 3 / 2;
        break;
    case NV_ENC_BUFFER_FORMAT_YUV444_10BIT: /**< 10 bit Planar YUV444 [Y plane followed by U and V planes]. Each pixel of size 2 bytes. Most Significant 10 bits contain pixel data.  */
        uInputWidthByte = uInputWidth * 2;
        uInputHeightTotal = uInputHeight * 3;
        break;
    case NV_ENC_BUFFER_FORMAT_ARGB:    /**< 8 bit Packed A8R8G8B8 */
    case NV_ENC_BUFFER_FORMAT_ARGB10:  /**< 10 bit Packed A2R10G10B10. Each pixel of size 2 bytes. Most Significant 10 bits contain pixel data.  */
    case NV_ENC_BUFFER_FORMAT_AYUV:    /**< 8 bit Packed A8Y8U8V8 */
    case NV_ENC_BUFFER_FORMAT_ABGR:    /**< 8 bit Packed A8B8G8R8 */
    case NV_ENC_BUFFER_FORMAT_ABGR10:  /**< 10 bit Packed A2B10G10R10. Each pixel of size 2 bytes. Most Significant 10 bits contain pixel data.  */
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    case NV_ENC_BUFFER_FORMAT_NV12:    /**< Semi-Planar YUV [Y plane followed by interleaved UV plane] */
        uInputWidthByte = uInputWidth;
        uInputHeightTotal = uInputHeight * 3 / 2;
        break;
    default:
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    for (int i = 0; i < m_encodeBufferCount; i++) {
        if (m_stPicStruct == NV_ENC_PIC_STRUCT_FRAME) {
#if ENABLE_AVSW_READER
            cuvidCtxLock(m_ctxLock, 0);
#endif //#if ENABLE_AVSW_READER
            auto cudaerr = cudaMallocPitch((void **)&m_stEncodeBuffer[i].stInputBfr.pNV12devPtr,
                (size_t *)&m_stEncodeBuffer[i].stInputBfr.uNV12Stride, uInputWidthByte, uInputHeightTotal);
#if ENABLE_AVSW_READER
            cuvidCtxUnlock(m_ctxLock, 0);
#endif //#if ENABLE_AVSW_READER
            if (cudaerr != cudaSuccess) {
                PrintMes(RGY_LOG_ERROR, _T("Failed to cuMemAllocPitch, %d (%s)\n"), cudaerr, char_to_tstring(_cudaGetErrorEnum(cudaerr)).c_str());
                return NV_ENC_ERR_OUT_OF_MEMORY;
            }

            if (NV_ENC_SUCCESS != (nvStatus = NvEncRegisterResource(NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR,
                (void*)m_stEncodeBuffer[i].stInputBfr.pNV12devPtr,
                uInputWidth, uInputHeight, m_stEncodeBuffer[i].stInputBfr.uNV12Stride, inputFormat,
                &m_stEncodeBuffer[i].stInputBfr.nvRegisteredResource))) {
                PrintMes(RGY_LOG_ERROR, _T("Failed to register input device memory.\n"));
                return nvStatus;
            }
        } else {
            //インタレ保持の場合は、NvEncCreateInputBuffer経由でフレームを渡さないと正常にエンコードできない
            nvStatus = NvEncCreateInputBuffer(uInputWidth, uInputHeight, &m_stEncodeBuffer[i].stInputBfr.hInputSurface, inputFormat);
            if (nvStatus != NV_ENC_SUCCESS) {
                PrintMes(RGY_LOG_ERROR, _T("Failed to allocate Input Buffer, Please reduce MAX_FRAMES_TO_PRELOAD\n"));
                return nvStatus;
            }
        }

        m_stEncodeBuffer[i].stInputBfr.bufferFmt = inputFormat;
        m_stEncodeBuffer[i].stInputBfr.dwWidth = uInputWidth;
        m_stEncodeBuffer[i].stInputBfr.dwHeight = uInputHeight;

        nvStatus = NvEncCreateBitstreamBuffer(BITSTREAM_BUFFER_SIZE, &m_stEncodeBuffer[i].stOutputBfr.hBitstreamBuffer);
        if (nvStatus != NV_ENC_SUCCESS) {
            PrintMes(RGY_LOG_ERROR, _T("Failed to allocate Output Buffer, Please reduce MAX_FRAMES_TO_PRELOAD\n"));
            return nvStatus;
        }
        m_stEncodeBuffer[i].stOutputBfr.dwBitstreamBufferSize = BITSTREAM_BUFFER_SIZE;

        nvStatus = NvEncRegisterAsyncEvent(&m_stEncodeBuffer[i].stOutputBfr.hOutputEvent);
        if (nvStatus != NV_ENC_SUCCESS)
            return nvStatus;
        m_stEncodeBuffer[i].stOutputBfr.bWaitOnEvent = true;
    }

#if ENABLE_AVSW_READER
    if (!m_cuvidDec) {
#else
    {
#endif //#if ENABLE_AVSW_READER
        m_inputHostBuffer.resize(PIPELINE_DEPTH);
        //このアライメントは読み込み時の色変換の並列化のために必要
        const int align = 64 * (RGY_CSP_BIT_DEPTH[pInputInfo->csp] > 8 ? 2 : 1);
        const int bufWidth  = pInputInfo->srcWidth  - pInputInfo->crop.e.left - pInputInfo->crop.e.right;
        const int bufHeight = pInputInfo->srcHeight - pInputInfo->crop.e.bottom - pInputInfo->crop.e.up;
        int bufPitch = 0;
        int bufSize = 0;
        switch (pInputInfo->csp) {
        case RGY_CSP_NV12:
        case RGY_CSP_YV12:
            bufPitch  = ALIGN(bufWidth, align);
            bufSize = bufPitch * bufHeight * 3 / 2; break;
        case RGY_CSP_P010:
        case RGY_CSP_YV12_09:
        case RGY_CSP_YV12_10:
        case RGY_CSP_YV12_12:
        case RGY_CSP_YV12_14:
        case RGY_CSP_YV12_16:
            bufPitch = ALIGN(bufWidth * 2, align);
            bufSize = bufPitch * bufHeight * 3 / 2; break;
        case RGY_CSP_NV16:
        case RGY_CSP_YUY2:
        case RGY_CSP_YUV422:
            bufPitch  = ALIGN(bufWidth, align);
            bufSize = bufPitch * bufHeight * 2; break;
        case RGY_CSP_P210:
        case RGY_CSP_YUV422_09:
        case RGY_CSP_YUV422_10:
        case RGY_CSP_YUV422_12:
        case RGY_CSP_YUV422_14:
        case RGY_CSP_YUV422_16:
            bufPitch = ALIGN(bufWidth * 2, align);
            bufSize = bufPitch * bufHeight * 2; break;
        case RGY_CSP_YUV444:
            bufPitch  = ALIGN(bufWidth, align);
            bufSize = bufPitch * bufHeight * 3; break;
        case RGY_CSP_YUV444_09:
        case RGY_CSP_YUV444_10:
        case RGY_CSP_YUV444_12:
        case RGY_CSP_YUV444_14:
        case RGY_CSP_YUV444_16:
            bufPitch = ALIGN(bufWidth * 2, align);
            bufSize = bufPitch * bufHeight * 3; break;
        case RGY_CSP_RGB24:
        case RGY_CSP_RGB24R:
            bufPitch = ALIGN(bufWidth * 3, align);
            bufSize = bufPitch * bufHeight; break;
        case RGY_CSP_RGB32:
        case RGY_CSP_RGB32R:
            bufPitch = ALIGN(bufWidth * 4, align);
            bufSize = bufPitch * bufHeight; break;
        case RGY_CSP_RGB:
        case RGY_CSP_GBR:
            bufPitch  = ALIGN(bufWidth, align);
            bufSize = bufPitch * bufHeight * 3; break;
        case RGY_CSP_RGBA:
        case RGY_CSP_GBRA:
            bufPitch  = ALIGN(bufWidth, align);
            bufSize = bufPitch * bufHeight * 4; break;
        default:
            PrintMes(RGY_LOG_ERROR, _T("Unsupported csp at AllocateIOBuffers.\n"));
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
        for (uint32_t i = 0; i < m_inputHostBuffer.size(); i++) {
            m_inputHostBuffer[i].frameInfo.width = bufWidth;
            m_inputHostBuffer[i].frameInfo.height = bufHeight;
            m_inputHostBuffer[i].frameInfo.pitch = bufPitch;
            m_inputHostBuffer[i].frameInfo.csp = pInputInfo->csp;
            m_inputHostBuffer[i].frameInfo.picstruct = pInputInfo->picstruct;
            m_inputHostBuffer[i].frameInfo.flags = RGY_FRAME_FLAG_NONE;
            m_inputHostBuffer[i].frameInfo.duration = 0;
            m_inputHostBuffer[i].frameInfo.timestamp = 0;
            m_inputHostBuffer[i].frameInfo.deivce_mem = false;
            m_inputHostBuffer[i].heTransferFin = unique_ptr<void, handle_deleter>(CreateEvent(NULL, FALSE, TRUE, NULL), handle_deleter());

#if ENABLE_AVSW_READER
            CCtxAutoLock ctxLock(m_ctxLock);
#endif //#if ENABLE_AVSW_READER
            auto cudaret = cudaMallocHost(&m_inputHostBuffer[i].frameInfo.ptr, bufSize);
            if (cudaret != cudaSuccess) {
                PrintMes(RGY_LOG_ERROR, _T("Error cudaEventRecord: %d (%s).\n"), cudaret, char_to_tstring(_cudaGetErrorEnum(cudaret)).c_str());
                return NV_ENC_ERR_GENERIC;
            }
        }
    }

    m_stEOSOutputBfr.bEOSFlag = TRUE;

    nvStatus = NvEncRegisterAsyncEvent(&m_stEOSOutputBfr.hOutputEvent);
    if (nvStatus != NV_ENC_SUCCESS)
        return nvStatus;

    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVEncCore::ReleaseIOBuffers() {
    for (int i = 0; i < m_encodeBufferCount; i++) {
        if (m_stEncodeBuffer[i].stInputBfr.pNV12devPtr) {
#if ENABLE_AVSW_READER
            cuvidCtxLock(m_ctxLock, 0);
#endif //#if ENABLE_AVSW_READER
            cuMemFree(m_stEncodeBuffer[i].stInputBfr.pNV12devPtr);
#if ENABLE_AVSW_READER
            cuvidCtxUnlock(m_ctxLock, 0);
#endif //#if ENABLE_AVSW_READER
            m_stEncodeBuffer[i].stInputBfr.pNV12devPtr = NULL;
        } else {
            //インタレ保持の場合にはこちらを使用
            if (m_stEncodeBuffer[i].stInputBfr.hInputSurface) {
                NvEncDestroyInputBuffer(m_stEncodeBuffer[i].stInputBfr.hInputSurface);
                m_stEncodeBuffer[i].stInputBfr.hInputSurface = NULL;
            }
        }

        if (m_stEncodeBuffer[i].stOutputBfr.hBitstreamBuffer) {
            NvEncDestroyBitstreamBuffer(m_stEncodeBuffer[i].stOutputBfr.hBitstreamBuffer);
            m_stEncodeBuffer[i].stOutputBfr.hBitstreamBuffer = NULL;
        }
        if (m_stEncodeBuffer[i].stOutputBfr.hOutputEvent) {
            NvEncUnregisterAsyncEvent(m_stEncodeBuffer[i].stOutputBfr.hOutputEvent);
            nvCloseFile(m_stEncodeBuffer[i].stOutputBfr.hOutputEvent);
            m_stEncodeBuffer[i].stOutputBfr.hOutputEvent = NULL;
        }
    }

    if (m_stEOSOutputBfr.hOutputEvent) {
        NvEncUnregisterAsyncEvent(m_stEOSOutputBfr.hOutputEvent);
        nvCloseFile(m_stEOSOutputBfr.hOutputEvent);
        m_stEOSOutputBfr.hOutputEvent = NULL;
    }

    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVEncCore::NvEncOpenEncodeSessionEx(void *device, NV_ENC_DEVICE_TYPE deviceType, const int sessionRetry) {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openSessionExParams;
    INIT_CONFIG(openSessionExParams, NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS);

    openSessionExParams.device = device;
    openSessionExParams.deviceType = deviceType;
    openSessionExParams.reserved = NULL;
    openSessionExParams.apiVersion = NVENCAPI_VERSION;

    static const int retry_millisec = 500;
    static const int retry_max = sessionRetry * 1000 / retry_millisec;
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
    for (int retry = 0; NV_ENC_SUCCESS != (nvStatus = m_pEncodeAPI->nvEncOpenEncodeSessionEx(&openSessionExParams, &m_hEncoder)); retry++) {
        if (nvStatus != NV_ENC_ERR_OUT_OF_MEMORY) {
            NVPrintFuncError(_T("nvEncOpenEncodeSessionEx"), nvStatus);
            break;
        }
        if (retry >= retry_max) {
            PrintMes(RGY_LOG_ERROR,
                FOR_AUO ? _T("このエラーはメモリが不足しているか、同時にNVEncで3ストリーム以上エンコードしようとすると発生することがあります。\n")
                          _T("Geforceでは、NVIDIAのドライバの制限により3ストリーム以上の同時エンコードが行えません。\n")
                        : _T("This error might occur when shortage of memory, or when trying to encode more than 2 streams by NVEnc.\n")
                          _T("In Geforce, simultaneous encoding is limited up to 2, due to the NVIDIA's driver limitation.\n"));
            break;
        }
        if ((retry % (10 * 1000 / retry_millisec)) == 0) {
            PrintMes(RGY_LOG_INFO, _T("Waiting for other encode to finish...\n"));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(retry_millisec));
    }

    return nvStatus;
}

NVENCSTATUS NVEncCore::SetEncodeCodecList(void *hEncoder) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
    uint32_t dwEncodeGUIDCount = 0;
    if (NV_ENC_SUCCESS != (nvStatus = m_pEncodeAPI->nvEncGetEncodeGUIDCount(hEncoder, &dwEncodeGUIDCount))) {
        NVPrintFuncError(_T("nvEncGetEncodeGUIDCount"), nvStatus);
        return nvStatus;
    }
    uint32_t uArraysize = 0;
    GUID guid_init = { 0 };
    std::vector<GUID> list_codecs;
    list_codecs.resize(dwEncodeGUIDCount, guid_init);
    if (NV_ENC_SUCCESS != (nvStatus = m_pEncodeAPI->nvEncGetEncodeGUIDs(hEncoder, &list_codecs[0], dwEncodeGUIDCount, &uArraysize))) {
        NVPrintFuncError(_T("nvEncGetEncodeGUIDs"), nvStatus);
        return nvStatus;
    }
    for (auto codec : list_codecs) {
        m_EncodeFeatures.push_back(NVEncCodecFeature(codec));
    }
    return nvStatus;
}

NVENCSTATUS NVEncCore::setCodecProfileList(void *hEncoder, NVEncCodecFeature& codecFeature) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
    uint32_t dwCodecProfileGUIDCount = 0;
    if (NV_ENC_SUCCESS != (nvStatus = m_pEncodeAPI->nvEncGetEncodeProfileGUIDCount(hEncoder, codecFeature.codec, &dwCodecProfileGUIDCount))) {
        NVPrintFuncError(_T("nvEncGetEncodeProfileGUIDCount"), nvStatus);
        return nvStatus;
    }
    uint32_t uArraysize = 0;
    GUID guid_init = { 0 };
    codecFeature.profiles.resize(dwCodecProfileGUIDCount, guid_init);
    if (NV_ENC_SUCCESS != (nvStatus = m_pEncodeAPI->nvEncGetEncodeProfileGUIDs(hEncoder, codecFeature.codec, &codecFeature.profiles[0], dwCodecProfileGUIDCount, &uArraysize))) {
        NVPrintFuncError(_T("nvEncGetEncodeProfileGUIDs"), nvStatus);
        return nvStatus;
    }
    return nvStatus;
}

NVENCSTATUS NVEncCore::setCodecPresetList(void *hEncoder, NVEncCodecFeature& codecFeature, bool getPresetConfig) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
    uint32_t dwCodecProfileGUIDCount = 0;
    if (NV_ENC_SUCCESS != (nvStatus = m_pEncodeAPI->nvEncGetEncodePresetCount(hEncoder, codecFeature.codec, &dwCodecProfileGUIDCount))) {
        NVPrintFuncError(_T("nvEncGetEncodePresetCount"), nvStatus);
        return nvStatus;
    }
    uint32_t uArraysize = 0;
    GUID guid_init = { 0 };
    NV_ENC_PRESET_CONFIG config_init = { 0 };
    codecFeature.presets.resize(dwCodecProfileGUIDCount, guid_init);
    codecFeature.presetConfigs.resize(dwCodecProfileGUIDCount, config_init);
    if (NV_ENC_SUCCESS != (nvStatus = m_pEncodeAPI->nvEncGetEncodePresetGUIDs(hEncoder, codecFeature.codec, &codecFeature.presets[0], dwCodecProfileGUIDCount, &uArraysize))) {
        NVPrintFuncError(_T("nvEncGetEncodePresetGUIDs"), nvStatus);
        return nvStatus;
    }
    if (getPresetConfig) {
        for (uint32_t i = 0; i < codecFeature.presets.size(); i++) {
            INIT_CONFIG(codecFeature.presetConfigs[i], NV_ENC_PRESET_CONFIG);
            SET_VER(codecFeature.presetConfigs[i].presetCfg, NV_ENC_CONFIG);
            if (NV_ENC_SUCCESS != (nvStatus = m_pEncodeAPI->nvEncGetEncodePresetConfig(hEncoder, codecFeature.codec, codecFeature.presets[i], &codecFeature.presetConfigs[i]))) {
                NVPrintFuncError(_T("nvEncGetEncodePresetConfig"), nvStatus);
                return nvStatus;
            }
        }
    }
    return nvStatus;
}

NVENCSTATUS NVEncCore::setInputFormatList(void *hEncoder, NVEncCodecFeature& codecFeature) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
    uint32_t dwInputFmtCount = 0;
    if (NV_ENC_SUCCESS != (nvStatus = m_pEncodeAPI->nvEncGetInputFormatCount(hEncoder, codecFeature.codec, &dwInputFmtCount))) {
        NVPrintFuncError(_T("nvEncGetInputFormatCount"), nvStatus);
        return nvStatus;
    }
    uint32_t uArraysize = 0;
    codecFeature.surfaceFmt.resize(dwInputFmtCount);
    if (NV_ENC_SUCCESS != (nvStatus = m_pEncodeAPI->nvEncGetInputFormats(hEncoder, codecFeature.codec, &codecFeature.surfaceFmt[0], dwInputFmtCount, &uArraysize))) {
        NVPrintFuncError(_T("nvEncGetInputFormats"), nvStatus);
        return nvStatus;
    }

    return nvStatus;
}

NVENCSTATUS NVEncCore::GetCurrentDeviceNVEncCapability(void *hEncoder, NVEncCodecFeature& codecFeature) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
    bool check_h264 = get_value_from_guid(codecFeature.codec, list_nvenc_codecs) == NV_ENC_H264;
    auto add_cap_info = [&](NV_ENC_CAPS cap_id, bool for_h264_only, bool is_boolean, const TCHAR *cap_name) {
        if (!(!check_h264 && for_h264_only)) {
            NV_ENC_CAPS_PARAM param;
            INIT_CONFIG(param, NV_ENC_CAPS_PARAM);
            param.capsToQuery = cap_id;
            int value = 0;
            NVENCSTATUS result = m_pEncodeAPI->nvEncGetEncodeCaps(hEncoder, codecFeature.codec, &param, &value);
            if (NV_ENC_SUCCESS == result) {
                NVEncCap cap = { 0 };
                cap.id = cap_id;
                cap.isBool = is_boolean;
                cap.name = cap_name;
                cap.value = value;
                codecFeature.caps.push_back(cap);
            } else {
                nvStatus = result;
            }
        }
    };

    add_cap_info(NV_ENC_CAPS_NUM_MAX_BFRAMES,              false, false, _T("Max Bframes"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE,      false, true,  _T("B Ref Mode"));
    add_cap_info(NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES,  false, false, _T("RC Modes"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_FIELD_ENCODING,       false, true,  _T("Field Encoding"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_MONOCHROME,           false, true,  _T("MonoChrome"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_FMO,                  true,  true,  _T("FMO"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_QPELMV,               false, true,  _T("Quater-Pel MV"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_BDIRECT_MODE,         false, true,  _T("B Direct Mode"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_CABAC,                true,  true,  _T("CABAC"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_ADAPTIVE_TRANSFORM,   true,  true,  _T("Adaptive Transform"));
    add_cap_info(NV_ENC_CAPS_NUM_MAX_TEMPORAL_LAYERS,      false, false, _T("Max Temporal Layers"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_HIERARCHICAL_PFRAMES, false, true,  _T("Hierarchial P Frames"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_HIERARCHICAL_BFRAMES, false, true,  _T("Hierarchial B Frames"));
    add_cap_info(NV_ENC_CAPS_LEVEL_MAX,                    false, false, _T("Max Level"));
    add_cap_info(NV_ENC_CAPS_LEVEL_MIN,                    false, false, _T("Min Level"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE,        false, true,  _T("4:4:4"));
    add_cap_info(NV_ENC_CAPS_WIDTH_MAX,                    false, false, _T("Max Width"));
    add_cap_info(NV_ENC_CAPS_HEIGHT_MAX,                   false, false, _T("Max Height"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE,       false, true,  _T("Dynamic Resolution Change"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE,   false, true,  _T("Dynamic Bitrate Change"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_DYN_FORCE_CONSTQP,    false, true,  _T("Forced constant QP"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_DYN_RCMODE_CHANGE,    false, true,  _T("Dynamic RC Mode Change"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_SUBFRAME_READBACK,    false, true,  _T("Subframe Readback"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_CONSTRAINED_ENCODING, false, true,  _T("Constrained Encoding"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_INTRA_REFRESH,        false, true,  _T("Intra Refresh"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE,  false, true,  _T("Custom VBV Bufsize"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_DYNAMIC_SLICE_MODE,   false, true,  _T("Dynamic Slice Mode"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION, false, true,  _T("Ref Pic Invalidiation"));
    add_cap_info(NV_ENC_CAPS_PREPROC_SUPPORT,              false, true,  _T("PreProcess"));
    add_cap_info(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT,         false, true,  _T("Async Encoding"));
    add_cap_info(NV_ENC_CAPS_MB_NUM_MAX,                   false, false, _T("Max MBs"));
    //add_cap_info(NV_ENC_CAPS_MB_PER_SEC_MAX,               false, false, _T("MAX MB per sec"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE,      false, true,  _T("Lossless"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_SAO,                  false, true,  _T("SAO"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_MEONLY_MODE,          false, true,  _T("Me Only Mode"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_LOOKAHEAD,            false, true,  _T("Lookahead"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ,          false, true,  _T("AQ (temporal)"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION,  false, true,  _T("Weighted Prediction"));
    add_cap_info(NV_ENC_CAPS_NUM_MAX_LTR_FRAMES,           false, false, _T("Max LTR Frames"));
    add_cap_info(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE,         false, true,  _T("10bit depth"));
    return nvStatus;
}

NVENCSTATUS NVEncCore::createDeviceCodecList() {
    return SetEncodeCodecList(m_hEncoder);
}

NVENCSTATUS NVEncCore::createDeviceFeatureList(bool getPresetConfig) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
    //m_EncodeFeaturesが作成されていなければ、自動的に作成
    if (m_EncodeFeatures.size() == 0)
        nvStatus = SetEncodeCodecList(m_hEncoder);

    if (NV_ENC_SUCCESS == nvStatus) {
        for (uint32_t i = 0; i < m_EncodeFeatures.size(); i++) {
            setCodecProfileList(m_hEncoder, m_EncodeFeatures[i]);
            setCodecPresetList(m_hEncoder, m_EncodeFeatures[i], getPresetConfig);
            setInputFormatList(m_hEncoder, m_EncodeFeatures[i]);
            GetCurrentDeviceNVEncCapability(m_hEncoder, m_EncodeFeatures[i]);
        }
    }
    return nvStatus;
}

const std::vector<NVEncCodecFeature>& NVEncCore::GetNVEncCapability() {
    if (m_EncodeFeatures.size() == 0) {
        createDeviceFeatureList();
    }
    return m_EncodeFeatures;
}

const NVEncCodecFeature *NVEncCore::getCodecFeature(const GUID& codec) {
    for (uint32_t i = 0; i < m_EncodeFeatures.size(); i++) {
        if (0 == memcmp(&m_EncodeFeatures[i].codec, &codec, sizeof(m_stCodecGUID))) {
            return &m_EncodeFeatures[i];
        }
    }
    return nullptr;
}

int NVEncCore::getCapLimit(NV_ENC_CAPS flag, const NVEncCodecFeature *codecFeature) {
    if (nullptr == codecFeature) {
        if (nullptr == (codecFeature = getCodecFeature(m_stCodecGUID))) {
            return 0;
        }
    }
    return get_value(flag, codecFeature->caps);
}

bool NVEncCore::checkProfileSupported(GUID profile, const NVEncCodecFeature *codecFeature) {
    if (nullptr == codecFeature) {
        if (nullptr == (codecFeature = getCodecFeature(m_stCodecGUID))) {
            return false;
        }
    }
    for (auto codecProf : codecFeature->profiles) {
        if (0 == memcmp(&profile, &codecProf, sizeof(codecProf))) {
            return true;
        }
    }
    return false;
}

bool NVEncCore::checkPresetSupported(GUID preset, const NVEncCodecFeature *codecFeature) {
    if (nullptr == codecFeature) {
        if (nullptr == (codecFeature = getCodecFeature(m_stCodecGUID))) {
            return false;
        }
    }
    for (auto codecPreset : codecFeature->presets) {
        if (0 == memcmp(&preset, &codecPreset, sizeof(codecPreset))) {
            return true;
        }
    }
    return false;
}

bool NVEncCore::checkSurfaceFmtSupported(NV_ENC_BUFFER_FORMAT surfaceFormat, const NVEncCodecFeature *codecFeature) {
    if (nullptr == codecFeature) {
        if (nullptr == (codecFeature = getCodecFeature(m_stCodecGUID))) {
            return false;
        }
    }
    for (auto codecFmt : codecFeature->surfaceFmt) {
        if (0 == memcmp(&surfaceFormat, &codecFmt, sizeof(surfaceFormat))) {
            return true;
        }
    }
    return false;
}

bool NVEncCore::enableCuvidResize(const InEncodeVideoParam *inputParam) {
    const bool interlacedEncode = ((inputParam->input.picstruct & RGY_PICSTRUCT_INTERLACED)
        && (inputParam->vpp.deinterlace == cudaVideoDeinterlaceMode_Weave
            && !inputParam->vpp.afs.enable
            && !inputParam->vpp.nnedi.enable
            && !inputParam->vpp.yadif.enable));
    return
         //デフォルトの補間方法
        inputParam->vpp.resizeInterp == NPPI_INTER_UNDEFINED
        //deinterlace bobとリサイズを有効にすると色成分が正常に出力されない場合がある
        && inputParam->vpp.deinterlace != cudaVideoDeinterlaceMode_Bob
#if CUVID_DISABLE_CROP
        //cropが行われていない (cuvidのcropはよくわからん)
        && !cropEnabled(inputParam->input.crop)
#endif //#if CUVID_DISABLE_CROP
        //インタレ保持でない (インタレ保持リサイズをするにはCUDAで行う必要がある)
        && !interlacedEncode
        //フィルタ処理が必要
        && !(  inputParam->vpp.delogo.enable
            || inputParam->vpp.gaussMaskSize > 0
            || inputParam->vpp.unsharp.enable
            || inputParam->vpp.knn.enable
            || inputParam->vpp.pmd.enable
            || inputParam->vpp.deband.enable
            || inputParam->vpp.edgelevel.enable
            || inputParam->vpp.afs.enable
            || inputParam->vpp.nnedi.enable
            || inputParam->vpp.yadif.enable
            || inputParam->vpp.tweak.enable
            || inputParam->vpp.colorspace.enable
            || inputParam->vpp.subburn.size() > 0
            || inputParam->vpp.pad.enable);
}

#pragma warning(push)
#pragma warning(disable: 4100)
NVENCSTATUS NVEncCore::InitDecoder(const InEncodeVideoParam *inputParam) {
#if ENABLE_AVSW_READER
    if (m_pFileReader->getInputCodec() != RGY_CODEC_UNKNOWN) {
        const AVStream *pStreamIn = nullptr;
        RGYInputAvcodec *pReader = dynamic_cast<RGYInputAvcodec *>(m_pFileReader.get());
        if (pReader != nullptr) {
            pStreamIn = pReader->GetInputVideoStream();
        }
        if (pStreamIn == nullptr) {
            PrintMes(RGY_LOG_ERROR, _T("failed to get stream info when initializing cuvid decoder.\n"));
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
        }

        m_cuvidDec.reset(new CuvidDecode());

        auto result = m_cuvidDec->InitDecode(m_ctxLock, &inputParam->input, &inputParam->vpp, pStreamIn->time_base, m_pNVLog, inputParam->nHWDecType, enableCuvidResize(inputParam));
        if (result != CUDA_SUCCESS) {
            PrintMes(RGY_LOG_ERROR, _T("failed to init decoder.\n"));
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
    }
#endif //#if ENABLE_AVSW_READER
    return NV_ENC_SUCCESS;
}
#pragma warning(pop)

NVENCSTATUS NVEncCore::SetInputParam(const InEncodeVideoParam *inputParam) {
    memcpy(&m_stEncConfig, &inputParam->encConfig, sizeof(m_stEncConfig));

    //コーデックの決定とチェックNV_ENC_PIC_PARAMS
    m_stCodecGUID = inputParam->codec == NV_ENC_H264 ? NV_ENC_CODEC_H264_GUID : NV_ENC_CODEC_HEVC_GUID;
    if (nullptr == getCodecFeature(m_stCodecGUID)) {
        PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("指定されたコーデックはサポートされていません。\n") : _T("Selected codec is not supported.\n"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }

    //プロファイルのチェック
    if (inputParam->codec == NV_ENC_HEVC) {
        //m_stEncConfig.profileGUIDはデフォルトではH.264のプロファイル情報
        //HEVCのプロファイル情報は、m_stEncConfig.encodeCodecConfig.hevcConfig.tierの下位16bitに保存されている
        m_stEncConfig.profileGUID = get_guid_from_value(m_stEncConfig.encodeCodecConfig.hevcConfig.tier & 0xffff, h265_profile_names);
        m_stEncConfig.encodeCodecConfig.hevcConfig.tier >>= 16;
    }
    if (!checkProfileSupported(m_stEncConfig.profileGUID)) {
        PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("指定されたプロファイルはサポートされていません。\n") : _T("Selected profile is not supported.\n"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }

    //プリセットのチェック
    if (!checkPresetSupported(get_guid_from_value(inputParam->preset, list_nvenc_preset_names))) {
        PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("指定されたプリセットはサポートされていません。\n") : _T("Selected preset is not supported.\n"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }

    //入力フォーマットはここでは気にしない
    //NV_ENC_BUFFER_FORMAT_NV12_TILED64x16
    //if (!checkSurfaceFmtSupported(NV_ENC_BUFFER_FORMAT_NV12_TILED64x16)) {
    //    PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("入力フォーマットが決定できません。\n") : _T("Input format is not supported.\n"));
    //    return NV_ENC_ERR_UNSUPPORTED_PARAM;
    //}

    //解像度の決定
    //この段階では、フィルタを使用した場合は解像度を変更しないものとする
    m_uEncWidth  = (m_pLastFilterParam) ? m_pLastFilterParam->frameOut.width  : inputParam->input.srcWidth  - inputParam->input.crop.e.left - inputParam->input.crop.e.right;
    m_uEncHeight = (m_pLastFilterParam) ? m_pLastFilterParam->frameOut.height : inputParam->input.srcHeight - inputParam->input.crop.e.bottom - inputParam->input.crop.e.up;

    //この段階では、フィルタを使用した場合は解像度を変更しないものとする
    if (!m_pLastFilterParam) {
        if (inputParam->input.dstWidth && inputParam->input.dstHeight) {
#if ENABLE_AVSW_READER
            if (m_pFileReader->getInputCodec() != RGY_CODEC_UNKNOWN) {
                m_uEncWidth  = inputParam->input.dstWidth;
                m_uEncHeight = inputParam->input.dstHeight;
            } else
#endif
            if (m_uEncWidth != inputParam->input.srcWidth || m_uEncHeight != inputParam->input.srcHeight) {
                PrintMes(RGY_LOG_ERROR, _T("resizing requires to be used with avhw reader.\n"));
                PrintMes(RGY_LOG_ERROR, _T(" input %dx%d -> output %dx%d.\n"), m_uEncWidth, m_uEncHeight, inputParam->input.dstWidth, inputParam->input.dstHeight);
                return NV_ENC_ERR_UNSUPPORTED_PARAM;
            }
        }
    }

    if (m_encFps.n() <= 0 || m_encFps.d() <= 0) {
        PrintMes(RGY_LOG_ERROR, _T("Invalid fps: %d/%d.\n"), m_encFps.n(), m_encFps.d());
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }

    //picStructの設定
    m_stPicStruct = picstruct_rgy_to_enc(inputParam->input.picstruct);

    if (inputParam->vpp.deinterlace != cudaVideoDeinterlaceMode_Weave) {
#if ENABLE_AVSW_READER
        if (m_pFileReader->getInputCodec() == RGY_CODEC_UNKNOWN) {
            PrintMes(RGY_LOG_ERROR, _T("vpp-deinterlace requires to be used with avhw reader.\n"));
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
#endif
        m_stPicStruct = NV_ENC_PIC_STRUCT_FRAME;
    } else if (inputParam->vpp.afs.enable || inputParam->vpp.nnedi.enable || inputParam->vpp.yadif.enable) {
        m_stPicStruct = NV_ENC_PIC_STRUCT_FRAME;
    }

    //制限事項チェック
    if (inputParam->input.srcWidth < 0 && inputParam->input.srcHeight < 0) {
        PrintMes(RGY_LOG_ERROR, _T("%s: %dx%d\n"), FOR_AUO ? _T("解像度が無効です。") : _T("Invalid resolution."), inputParam->input.srcWidth, inputParam->input.srcHeight);
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    if (   (int)inputParam->input.srcWidth  <= inputParam->input.crop.e.left + inputParam->input.crop.e.right
        && (int)inputParam->input.srcHeight <= inputParam->input.crop.e.up   + inputParam->input.crop.e.bottom) {
        PrintMes(RGY_LOG_ERROR, _T("%s: %dx%d, Crop [%d,%d,%d,%d]\n"),
             FOR_AUO ? _T("Crop値が無効です。") : _T("Invalid crop value."),
            inputParam->input.srcWidth, inputParam->input.srcHeight,
            inputParam->input.crop.c[0], inputParam->input.crop.c[1], inputParam->input.crop.c[2], inputParam->input.crop.c[3]);
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }

    const int height_check_mask = 1 + 2 * !!is_interlaced(m_stPicStruct);
    if ((m_uEncWidth & 1) || (m_uEncHeight & height_check_mask)) {
        PrintMes(RGY_LOG_ERROR, _T("%s: %dx%d\n"), FOR_AUO ? _T("解像度が無効です。") : _T("Invalid resolution."), m_uEncWidth, m_uEncHeight);
        PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("縦横の解像度は2の倍数である必要があります。\n") : _T("Relosution of mod2 required.\n"));
        if (is_interlaced(m_stPicStruct)) {
            PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("さらに、インタレ保持エンコードでは縦解像度は4の倍数である必要があります。\n") : _T("For interlaced encoding, mod4 is required for height.\n"));
        }
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    if ((inputParam->input.crop.e.left & 1) || (inputParam->input.crop.e.right & 1)
        || (inputParam->input.crop.e.up & height_check_mask) || (inputParam->input.crop.e.bottom & height_check_mask)) {
        PrintMes(RGY_LOG_ERROR, _T("%s: %dx%d, Crop [%d,%d,%d,%d]\n"),
             FOR_AUO ? _T("Crop値が無効です。") : _T("Invalid crop value."),
            inputParam->input.srcWidth, inputParam->input.srcHeight,
            inputParam->input.crop.c[0], inputParam->input.crop.c[1], inputParam->input.crop.c[2], inputParam->input.crop.c[3]);
        PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("Crop値は2の倍数である必要があります。\n") : _T("Crop value of mod2 required.\n"));
        if (is_interlaced(m_stPicStruct)) {
            PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("さらに、インタレ保持エンコードでは縦Crop値は4の倍数である必要があります。\n") : _T("For interlaced encoding, mod4 is required for height.\n"));
        }
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    if ((inputParam->nAVSyncMode & RGY_AVSYNC_FORCE_CFR) != 0 && inputParam->nTrimCount > 0) {
        PrintMes(RGY_LOG_ERROR, _T("avsync forcecfr + trim is not supported.\n"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    //環境による制限
    auto error_resolution_over_limit = [&](const TCHAR *feature, uint32_t featureValue, NV_ENC_CAPS featureID) {
        const TCHAR *error_mes = FOR_AUO ? _T("解像度が上限を超えています。") : _T("Resolution is over limit.");
        if (nullptr == feature)
            PrintMes(RGY_LOG_ERROR, _T("%s: %dx%d [上限: %dx%d]\n"), error_mes, m_uEncWidth, m_uEncHeight, getCapLimit(NV_ENC_CAPS_WIDTH_MAX), getCapLimit(NV_ENC_CAPS_HEIGHT_MAX));
        else
            PrintMes(RGY_LOG_ERROR, _T("%s: %dx%d, [%s]: %d [上限: %d]\n"), error_mes, m_uEncWidth, m_uEncHeight, feature, featureValue, getCapLimit(featureID));
    };

    if (m_uEncWidth > (uint32_t)getCapLimit(NV_ENC_CAPS_WIDTH_MAX) || m_uEncHeight > (uint32_t)getCapLimit(NV_ENC_CAPS_HEIGHT_MAX)) {
        error_resolution_over_limit(nullptr, 0, (NV_ENC_CAPS)0);
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    uint32_t heightMod = 16 * (1 + !!is_interlaced(m_stPicStruct));
    uint32_t targetMB = ((m_uEncWidth + 15) / 16) * ((m_uEncHeight + (heightMod - 1)) / heightMod);
    if (targetMB > (uint32_t)getCapLimit(NV_ENC_CAPS_MB_NUM_MAX)) {
        error_resolution_over_limit(_T("MB"), targetMB, NV_ENC_CAPS_MB_NUM_MAX);
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    //MB/sの制限は特にチェックする必要がなさそう
    //uint32_t targetMBperSec = (targetMB * inputParam->input.rate + inputParam->input.scale - 1) / inputParam->input.scale;
    //if (targetMBperSec > (uint32_t)getCapLimit(NV_ENC_CAPS_MB_PER_SEC_MAX)) {
    //    error_resolution_over_limit(_T("MB/s"), targetMBperSec, NV_ENC_CAPS_MB_PER_SEC_MAX);
    //    return NV_ENC_ERR_UNSUPPORTED_PARAM;
    //}

    auto error_feature_unsupported = [&](int log_level, const TCHAR *feature_name) {
        PrintMes(log_level, FOR_AUO ? _T("%sはサポートされていません。\n") : _T("%s unsupported.\n"), feature_name);
    };

    if (is_interlaced(m_stPicStruct) && !getCapLimit(NV_ENC_CAPS_SUPPORT_FIELD_ENCODING)) {
        if (inputParam->codec == NV_ENC_HEVC) {
            PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("HEVCではインタレ保持出力はサポートされていません。\n") : _T("interlaced output is not supported for HEVC codec.\n"));
        } else {
            PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("現在の設定ではインタレ保持出力はサポートされていません。\n") : _T("interlaced output is not supported for current setting.\n"));
        }
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    if (m_stEncConfig.rcParams.rateControlMode != (m_stEncConfig.rcParams.rateControlMode & getCapLimit(NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES))) {
        error_feature_unsupported(RGY_LOG_ERROR, FOR_AUO ? _T("選択されたレート制御モード") : _T("Selected encode mode"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    if (m_stEncConfig.frameIntervalP < 0) {
        PrintMes(RGY_LOG_ERROR, _T("%s: %d\n"),
            FOR_AUO ? _T("Bフレーム設定が無効です。正の値を使用してください。\n") : _T("B frame settings are invalid. Please use a number > 0.\n"),
            m_stEncConfig.frameIntervalP - 1);
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    if (m_stEncConfig.rcParams.enableLookahead && !getCapLimit(NV_ENC_CAPS_SUPPORT_LOOKAHEAD)) {
        error_feature_unsupported(RGY_LOG_WARN, _T("Lookahead"));
        m_stEncConfig.rcParams.enableLookahead = 0;
        m_stEncConfig.rcParams.lookaheadDepth = 0;
        m_stEncConfig.rcParams.disableBadapt = 0;
        m_stEncConfig.rcParams.disableIadapt = 0;
    }
    if (m_stEncConfig.rcParams.enableTemporalAQ && !getCapLimit(NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ)) {
        error_feature_unsupported(RGY_LOG_WARN, _T("Temporal AQ"));
        m_stEncConfig.rcParams.enableTemporalAQ = 0;
    }
    if (inputParam->bluray) {
        if (inputParam->codec == NV_ENC_HEVC) {
            PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("HEVCではBluray用出力はサポートされていません。\n") : _T("Bluray output is not supported for HEVC codec.\n"));
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
        const auto VBR_RC_LIST = make_array<NV_ENC_PARAMS_RC_MODE>(NV_ENC_PARAMS_RC_VBR, NV_ENC_PARAMS_RC_VBR_MINQP, NV_ENC_PARAMS_RC_2_PASS_VBR, NV_ENC_PARAMS_RC_CBR, NV_ENC_PARAMS_RC_CBR2);
        if (std::find(VBR_RC_LIST.begin(), VBR_RC_LIST.end(), inputParam->encConfig.rcParams.rateControlMode) == VBR_RC_LIST.end()) {
            PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("Bluray用出力では、VBRモードを使用してください。\n") :  _T("Please use VBR mode for bluray output.\n"));
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
        if (!getCapLimit(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)) {
            error_feature_unsupported(RGY_LOG_ERROR, FOR_AUO ? _T("VBVバッファサイズの指定") : _T("Custom VBV Bufsize"));
            PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("Bluray用出力を行えません。\n") :  _T("Therfore you cannot output for bluray.\n"));
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
    }
    if (m_stEncConfig.frameIntervalP - 1 > getCapLimit(NV_ENC_CAPS_NUM_MAX_BFRAMES)) {
        m_stEncConfig.frameIntervalP = getCapLimit(NV_ENC_CAPS_NUM_MAX_BFRAMES) + 1;
        PrintMes(RGY_LOG_WARN, FOR_AUO ? _T("Bフレームの最大数は%dです。\n") : _T("Max B frames are %d frames.\n"), getCapLimit(NV_ENC_CAPS_NUM_MAX_BFRAMES));
    }
    if (inputParam->codec == NV_ENC_H264) {
        if (NV_ENC_H264_ENTROPY_CODING_MODE_CABAC == m_stEncConfig.encodeCodecConfig.h264Config.entropyCodingMode && !getCapLimit(NV_ENC_CAPS_SUPPORT_CABAC)) {
            m_stEncConfig.encodeCodecConfig.h264Config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
            error_feature_unsupported(RGY_LOG_WARN, _T("CABAC"));
        }
        if (NV_ENC_H264_FMO_ENABLE == m_stEncConfig.encodeCodecConfig.h264Config.fmoMode && !getCapLimit(NV_ENC_CAPS_SUPPORT_FMO)) {
            m_stEncConfig.encodeCodecConfig.h264Config.fmoMode = NV_ENC_H264_FMO_DISABLE;
            error_feature_unsupported(RGY_LOG_WARN, _T("FMO"));
        }
        if (NV_ENC_H264_BDIRECT_MODE_TEMPORAL & m_stEncConfig.encodeCodecConfig.h264Config.bdirectMode && !getCapLimit(NV_ENC_CAPS_SUPPORT_BDIRECT_MODE)) {
            m_stEncConfig.encodeCodecConfig.h264Config.bdirectMode = NV_ENC_H264_BDIRECT_MODE_DISABLE;
            error_feature_unsupported(RGY_LOG_WARN, _T("B Direct mode"));
        }
        if (NV_ENC_H264_ADAPTIVE_TRANSFORM_ENABLE != m_stEncConfig.encodeCodecConfig.h264Config.adaptiveTransformMode && !getCapLimit(NV_ENC_CAPS_SUPPORT_ADAPTIVE_TRANSFORM)) {
            m_stEncConfig.encodeCodecConfig.h264Config.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_DISABLE;
            error_feature_unsupported(RGY_LOG_WARN, _T("Adaptive Tranform"));
        }
        if (m_stEncConfig.encodeCodecConfig.h264Config.useBFramesAsRef != NV_ENC_BFRAME_REF_MODE_DISABLED && !getCapLimit(NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE)) {
            m_stEncConfig.encodeCodecConfig.h264Config.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
            error_feature_unsupported(RGY_LOG_WARN, _T("B Ref Mode"));
        }
    }
    if ((NV_ENC_MV_PRECISION_QUARTER_PEL == m_stEncConfig.mvPrecision) && !getCapLimit(NV_ENC_CAPS_SUPPORT_QPELMV)) {
        m_stEncConfig.mvPrecision = NV_ENC_MV_PRECISION_HALF_PEL;
        error_feature_unsupported(RGY_LOG_WARN, FOR_AUO ? _T("1/4画素精度MV探索") : _T("Q-Pel MV"));
    }
    if (0 != m_stEncConfig.rcParams.vbvBufferSize && !getCapLimit(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)) {
        m_stEncConfig.rcParams.vbvBufferSize = 0;
        error_feature_unsupported(RGY_LOG_WARN, FOR_AUO ? _T("VBVバッファサイズの指定") : _T("Custom VBV Bufsize"));
    }
    if (inputParam->lossless && !getCapLimit(NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE)) {
        error_feature_unsupported(RGY_LOG_ERROR, _T("lossless"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    if (inputParam->codec == NV_ENC_HEVC) {
        if (   ( m_stEncConfig.encodeCodecConfig.hevcConfig.maxCUSize != NV_ENC_HEVC_CUSIZE_AUTOSELECT
              && m_stEncConfig.encodeCodecConfig.hevcConfig.maxCUSize != NV_ENC_HEVC_CUSIZE_32x32)
            || ( m_stEncConfig.encodeCodecConfig.hevcConfig.minCUSize != NV_ENC_HEVC_CUSIZE_AUTOSELECT
              && m_stEncConfig.encodeCodecConfig.hevcConfig.minCUSize != NV_ENC_HEVC_CUSIZE_8x8)) {
            PrintMes(RGY_LOG_WARN, _T("it is not recommended to use --cu-max or --cu-min, leaving it auto will enhance video quality.\n"));
        }
        if (m_stEncConfig.encodeCodecConfig.hevcConfig.useBFramesAsRef != NV_ENC_BFRAME_REF_MODE_DISABLED && !getCapLimit(NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE)) {
            m_stEncConfig.encodeCodecConfig.hevcConfig.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
            error_feature_unsupported(RGY_LOG_WARN, _T("B Ref Mode"));
        }
    }
    if (m_dynamicRC.size() > 0 && !getCapLimit(NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE)) {
        error_feature_unsupported(RGY_LOG_ERROR, _T("dynamic RC Change"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    //自動決定パラメータ
    if (0 == m_stEncConfig.gopLength) {
        m_stEncConfig.gopLength = (int)(m_encFps.n() / (double)m_encFps.d() + 0.5) * 10;
    }
    if (m_stEncConfig.encodeCodecConfig.h264Config.enableLTR && m_stEncConfig.encodeCodecConfig.h264Config.ltrNumFrames == 0) {
        m_stEncConfig.encodeCodecConfig.h264Config.ltrNumFrames = m_stEncConfig.encodeCodecConfig.h264Config.maxNumRefFrames;
    }
    if (m_stEncConfig.encodeCodecConfig.hevcConfig.enableLTR && m_stEncConfig.encodeCodecConfig.hevcConfig.ltrNumFrames == 0) {
        m_stEncConfig.encodeCodecConfig.hevcConfig.ltrNumFrames = m_stEncConfig.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB;
    }
    //SAR自動設定
    auto par = std::make_pair(inputParam->par[0], inputParam->par[1]);
    if ((!inputParam->par[0] || !inputParam->par[1]) //SAR比の指定がない
        && inputParam->input.sar[0] && inputParam->input.sar[1] //入力側からSAR比を取得ずみ
        && (m_uEncWidth == inputParam->input.srcWidth && m_uEncHeight == inputParam->input.srcHeight)) {//リサイズは行われない
        par = std::make_pair(inputParam->input.sar[0], inputParam->input.sar[1]);
    }
    adjust_sar(&par.first, &par.second, m_uEncWidth, m_uEncHeight);
    m_sar = rgy_rational<int>(par.first, par.second);

    //色空間設定自動
    int frame_height = m_uEncHeight;
    auto apply_auto_colormatrix = [frame_height](uint32_t& value, const CX_DESC *list) {
        if (COLOR_VALUE_AUTO == value)
            value = list[(frame_height >= HD_HEIGHT_THRESHOLD) ? HD_INDEX : SD_INDEX].value;
    };
    //最大ビットレート自動
    if (m_stEncConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP) {
        //CQPモードでは、最大ビットレートの指定は不要
        m_stEncConfig.rcParams.maxBitRate = 0;
    } else if (m_stEncConfig.rcParams.maxBitRate == 0) {
        //指定されたビットレートの1.5倍は最大ビットレートを確保する
        const int prefered_bitrate_kbps = m_stEncConfig.rcParams.averageBitRate * 3 / 2 / 1000;
        if (inputParam->codec == NV_ENC_H264) {
            const int profile = get_value_from_guid(m_stEncConfig.profileGUID, h264_profile_names);
            int level = m_stEncConfig.encodeCodecConfig.h264Config.level;
            if (level == 0) {
                level = calc_h264_auto_level(m_uEncWidth, m_uEncHeight, m_stEncConfig.encodeCodecConfig.h264Config.maxNumRefFrames, is_interlaced(m_stPicStruct),
                    m_encFps.n(), m_encFps.d(), profile, prefered_bitrate_kbps, m_stEncConfig.rcParams.vbvBufferSize / 1000);
            }
            int max_bitrate_kbps = 0, vbv_bufsize_kbps = 0;
            get_h264_vbv_value(&max_bitrate_kbps, &vbv_bufsize_kbps, level, profile);
            if (profile >= 100) {
                //なぜかhigh profileではぎりぎりを指定するとエラー終了するので、すこし減らす
                max_bitrate_kbps = (int)(max_bitrate_kbps * 0.96 + 0.5);
                vbv_bufsize_kbps = (int)(vbv_bufsize_kbps * 0.96 + 0.5);
            }
            m_stEncConfig.rcParams.maxBitRate = max_bitrate_kbps * 1000;
        } else if (inputParam->codec == NV_ENC_HEVC) {
            const bool high_tier = m_stEncConfig.encodeCodecConfig.hevcConfig.tier == NV_ENC_TIER_HEVC_HIGH;
            int level = m_stEncConfig.encodeCodecConfig.hevcConfig.level;
            if (level == 0) {
                level = calc_hevc_auto_level(m_uEncWidth, m_uEncHeight, //m_stEncConfig.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB,
                    m_encFps.n(), m_encFps.d(), high_tier, prefered_bitrate_kbps);
            }
            //なぜかぎりぎりを指定するとエラー終了するので、すこし減らす
            m_stEncConfig.rcParams.maxBitRate = get_hevc_max_bitrate(level, high_tier) * 960;
        } else {
            m_stEncConfig.rcParams.maxBitRate = DEFAULT_MAX_BITRATE;
        }
    }
    if (inputParam->codec == NV_ENC_H264) {
        apply_auto_colormatrix(m_stEncConfig.encodeCodecConfig.h264Config.h264VUIParameters.colourPrimaries,         list_colorprim);
        apply_auto_colormatrix(m_stEncConfig.encodeCodecConfig.h264Config.h264VUIParameters.transferCharacteristics, list_transfer);
        apply_auto_colormatrix(m_stEncConfig.encodeCodecConfig.h264Config.h264VUIParameters.colourMatrix,            list_colormatrix);
        if (inputParam->yuv444) {
            m_stEncConfig.encodeCodecConfig.h264Config.h264VUIParameters.chromaSampleLocationFlag = 0;
            m_stEncConfig.encodeCodecConfig.h264Config.h264VUIParameters.chromaSampleLocationTop = 0;
            m_stEncConfig.encodeCodecConfig.h264Config.h264VUIParameters.chromaSampleLocationBot = 0;
        }
    } else if (inputParam->codec == NV_ENC_HEVC) {
        apply_auto_colormatrix(m_stEncConfig.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourPrimaries,         list_colorprim);
        apply_auto_colormatrix(m_stEncConfig.encodeCodecConfig.hevcConfig.hevcVUIParameters.transferCharacteristics, list_transfer);
        apply_auto_colormatrix(m_stEncConfig.encodeCodecConfig.hevcConfig.hevcVUIParameters.colourMatrix,            list_colormatrix);
        if (inputParam->yuv444) {
            m_stEncConfig.encodeCodecConfig.hevcConfig.hevcVUIParameters.chromaSampleLocationFlag = 0;
            m_stEncConfig.encodeCodecConfig.hevcConfig.hevcVUIParameters.chromaSampleLocationTop = 0;
            m_stEncConfig.encodeCodecConfig.hevcConfig.hevcVUIParameters.chromaSampleLocationBot = 0;
        }
    }

    //バッファサイズ
    int extraBufSize = 0;
    if (m_uEncWidth * m_uEncHeight <= 2048 * 1080) {
        extraBufSize = 4;
    } else if (m_uEncWidth * m_uEncHeight <= 4096 * 2160) {
        extraBufSize = 2;
    }
    int requiredBufferFrames = m_stEncConfig.frameIntervalP + 4;
    if (m_stEncConfig.rcParams.enableLookahead) {
        requiredBufferFrames += m_stEncConfig.rcParams.lookaheadDepth;
    }
    //PIPELINE_DEPTH分拡張しないと、バッファ不足でエンコードが止まってしまう
    m_encodeBufferCount = requiredBufferFrames + PIPELINE_DEPTH;
    m_encodeBufferCount = std::max(m_encodeBufferCount, std::min(m_encodeBufferCount + extraBufSize, 32));
    if (m_encodeBufferCount > MAX_ENCODE_QUEUE) {
#if FOR_AUO
        PrintMes(RGY_LOG_ERROR, _T("入力バッファは多すぎます。: %d フレーム\n"), m_encodeBufferCount);
        PrintMes(RGY_LOG_ERROR, _T("%d フレームまでに設定して下さい。\n"), MAX_ENCODE_QUEUE);
#else
        PrintMes(RGY_LOG_ERROR, _T("Input frame of %d exceeds the maximum size allowed (%d).\n"), m_encodeBufferCount, MAX_ENCODE_QUEUE);
#endif
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }

    INIT_CONFIG(m_stCreateEncodeParams, NV_ENC_INITIALIZE_PARAMS);
    m_stCreateEncodeParams.encodeConfig        = &m_stEncConfig;
    m_stCreateEncodeParams.encodeHeight        = m_uEncHeight;
    m_stCreateEncodeParams.encodeWidth         = m_uEncWidth;
    m_stCreateEncodeParams.darHeight           = m_uEncHeight;
    m_stCreateEncodeParams.darWidth            = m_uEncWidth;
    get_dar_pixels(&m_stCreateEncodeParams.darWidth, &m_stCreateEncodeParams.darHeight, par.first, par.second);

    m_stCreateEncodeParams.maxEncodeHeight     = m_uEncHeight;
    m_stCreateEncodeParams.maxEncodeWidth      = m_uEncWidth;

    m_stCreateEncodeParams.frameRateNum        = m_encFps.n();
    m_stCreateEncodeParams.frameRateDen        = m_encFps.d();
    if (inputParam->nWeightP) {
        if (!getCapLimit(NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION)) {
            error_feature_unsupported(RGY_LOG_WARN, _T("weighted prediction"));
        } else if (m_stEncConfig.frameIntervalP - 1 > 0) {
            error_feature_unsupported(RGY_LOG_WARN, _T("weighted prediction with B frames"));
        } else {
            if (inputParam->codec == NV_ENC_HEVC) {
                PrintMes(RGY_LOG_WARN, _T("HEVC encode with weightp is known to be unstable on some environments.\n"));
                PrintMes(RGY_LOG_WARN, _T("Consider not using weightp with HEVC encode if unstable.\n"));
            }
            m_stCreateEncodeParams.enableWeightedPrediction = 1;
        }
    }

    m_stCreateEncodeParams.enableEncodeAsync   = true;
    m_stCreateEncodeParams.enablePTD           = true;
    m_stCreateEncodeParams.encodeGUID          = m_stCodecGUID;
    m_stCreateEncodeParams.presetGUID          = list_nvenc_preset_names[inputParam->preset].id;
    if (inputParam->lossless) {
        switch (list_nvenc_preset_names[inputParam->preset].value) {
        case NVENC_PRESET_HP:
        case NVENC_PRESET_LL_HP:
            m_stCreateEncodeParams.presetGUID = NV_ENC_PRESET_LOSSLESS_HP_GUID;
            break;
        default:
            m_stCreateEncodeParams.presetGUID = NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID;
            break;
        }
    }

    //ロスレス出力
    if (inputParam->lossless) {
        //profileは0にしておかないと正常に動作しない
        if (inputParam->codec == NV_ENC_H264) {
            memset(&m_stCreateEncodeParams.encodeConfig->profileGUID, 0, sizeof(m_stCreateEncodeParams.encodeConfig->profileGUID));
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.qpPrimeYZeroTransformBypassFlag = 1;
        }
        m_stCreateEncodeParams.encodeConfig->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        m_stCreateEncodeParams.encodeConfig->rcParams.averageBitRate = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.maxBitRate = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.qpMapMode = NV_ENC_QP_MAP_DISABLED;
        m_stCreateEncodeParams.encodeConfig->rcParams.aqStrength = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.enableAQ = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.enableTemporalAQ = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.targetQuality = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.targetQualityLSB = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.temporallayerIdxMask = 0;
        memset(&m_stCreateEncodeParams.encodeConfig->rcParams.temporalLayerQP, 0, sizeof(m_stCreateEncodeParams.encodeConfig->rcParams.temporalLayerQP));
        m_stCreateEncodeParams.encodeConfig->rcParams.vbvBufferSize = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.vbvInitialDelay = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.constQP.qpIntra = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.constQP.qpInterP = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.constQP.qpInterB = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.enableMinQP = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.enableMaxQP = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.enableInitialRCQP = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.minQP.qpIntra = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.minQP.qpInterP = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.minQP.qpInterB = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.maxQP.qpIntra = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.maxQP.qpInterP = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.maxQP.qpInterB = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.initialRCQP.qpIntra = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.initialRCQP.qpInterP = 0;
        m_stCreateEncodeParams.encodeConfig->rcParams.initialRCQP.qpInterB = 0;
    }

    if (inputParam->codec == NV_ENC_HEVC) {
        if (m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.sliceMode != 3) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.sliceMode = 3;
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.sliceModeData = 1;
        }
        //整合性チェック (一般, H.265/HEVC)
        m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.idrPeriod = m_stCreateEncodeParams.encodeConfig->gopLength;

        if (m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.outputPictureTimingSEI) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.outputBufferingPeriodSEI = 1;
        }
        //YUV444出力
        if (inputParam->yuv444) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.chromaFormatIDC = 3;
            //m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.separateColourPlaneFlag = 1;
            m_stCreateEncodeParams.encodeConfig->profileGUID = NV_ENC_HEVC_PROFILE_FREXT_GUID;
        } else if (m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 > 0) {
            m_stCreateEncodeParams.encodeConfig->profileGUID = (inputParam->yuv444) ? NV_ENC_HEVC_PROFILE_FREXT_GUID : NV_ENC_HEVC_PROFILE_MAIN10_GUID;
        }
        //整合性チェック (HEVC VUI)
        m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.overscanInfoPresentFlag = 1;
        m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.overscanInfo = 0;

        m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.colourDescriptionPresentFlag =
            (      get_cx_value(list_colorprim,   _T("undef")) != (int)m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.colourPrimaries
                || get_cx_value(list_transfer,    _T("undef")) != (int)m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.transferCharacteristics
                || get_cx_value(list_colormatrix, _T("undef")) != (int)m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.colourMatrix) ? 1 : 0;
        if (!m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.colourDescriptionPresentFlag) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.colourPrimaries = 0;
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.transferCharacteristics = 0;
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.colourMatrix = 0;
        }

        m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.videoSignalTypePresentFlag =
            (get_cx_value(list_videoformat, _T("undef")) != (int)m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFormat
                || m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFullRangeFlag
                || m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.colourDescriptionPresentFlag) ? 1 : 0;
        if (!m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.videoSignalTypePresentFlag) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.hevcVUIParameters.videoFormat = 0;
        }
        if (m_hdr10plus) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
        }
    } else if (inputParam->codec == NV_ENC_H264) {
        if (m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.sliceMode != 3) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.sliceMode = 3;
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.sliceModeData = 1;
        }
        //Bluray 互換出力
        if (inputParam->bluray) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.outputPictureTimingSEI = 1;
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.outputRecoveryPointSEI = 1;
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.outputAUD = 1;
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.sliceMode = 3;
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.sliceModeData = 4;
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.level = std::min<uint32_t>(m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.level, NV_ENC_LEVEL_H264_41);
            m_stCreateEncodeParams.encodeConfig->rcParams.maxBitRate = std::min<uint32_t>(m_stCreateEncodeParams.encodeConfig->rcParams.maxBitRate, 40000 * 1000);
            if (m_stCreateEncodeParams.encodeConfig->rcParams.vbvBufferSize == 0) {
                m_stCreateEncodeParams.encodeConfig->rcParams.vbvBufferSize = m_stCreateEncodeParams.encodeConfig->rcParams.maxBitRate;
            }
            m_stCreateEncodeParams.encodeConfig->rcParams.vbvInitialDelay = m_stCreateEncodeParams.encodeConfig->rcParams.vbvBufferSize / 2;
            m_stCreateEncodeParams.encodeConfig->rcParams.averageBitRate = std::min(m_stCreateEncodeParams.encodeConfig->rcParams.averageBitRate, m_stCreateEncodeParams.encodeConfig->rcParams.maxBitRate);
            m_stCreateEncodeParams.encodeConfig->frameIntervalP = std::min(m_stCreateEncodeParams.encodeConfig->frameIntervalP, 3+1);
            const auto maxGOPLen =
                (m_uEncWidth <= 1280 && m_uEncHeight <= 720
                && (int)(m_encFps.n() / (double)m_encFps.d() + 0.9) >= 60)
                ? 60u : 30u;
            const bool overMaxGOPLen = m_stCreateEncodeParams.encodeConfig->gopLength > maxGOPLen;
            m_stCreateEncodeParams.encodeConfig->gopLength = (std::min(m_stCreateEncodeParams.encodeConfig->gopLength, maxGOPLen) / m_stCreateEncodeParams.encodeConfig->frameIntervalP) * m_stCreateEncodeParams.encodeConfig->frameIntervalP;
            if (maxGOPLen == 30u && overMaxGOPLen) {
                m_stCreateEncodeParams.encodeConfig->gopLength = 30u;
            }
        }
        if (m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.outputPictureTimingSEI) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.outputBufferingPeriodSEI = 1;
        }
        //YUV444出力
        if (inputParam->yuv444) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.chromaFormatIDC = 3;
            //m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.separateColourPlaneFlag = 1;
            m_stCreateEncodeParams.encodeConfig->profileGUID = NV_ENC_H264_PROFILE_HIGH_444_GUID;
        }

        //整合性チェック (一般, H.264/AVC)
        m_stCreateEncodeParams.encodeConfig->frameFieldMode = (m_stPicStruct == NV_ENC_PIC_STRUCT_FRAME) ? NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME : NV_ENC_PARAMS_FRAME_FIELD_MODE_FIELD;
        //m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.entropyCodingMode = (m_stEncoderInput[0].profile > 66) ? NV_ENC_H264_ENTROPY_CODING_MODE_CABAC : NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
        m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.idrPeriod = m_stCreateEncodeParams.encodeConfig->gopLength;
        if (m_stCreateEncodeParams.encodeConfig->frameIntervalP - 1 <= 0) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.bdirectMode = NV_ENC_H264_BDIRECT_MODE_DISABLE;
        }

        //整合性チェック (H.264 VUI)
        m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.overscanInfoPresentFlag = 1;
        m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.overscanInfo = 0;

        m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.colourDescriptionPresentFlag =
            (  get_cx_value(list_colorprim,   _T("undef")) != (int)m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.colourPrimaries
            || get_cx_value(list_transfer,    _T("undef")) != (int)m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.transferCharacteristics
            || get_cx_value(list_colormatrix, _T("undef")) != (int)m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.colourMatrix) ? 1 : 0;
        if (!m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.colourDescriptionPresentFlag) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.colourPrimaries = 0;
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.transferCharacteristics = 0;
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.colourMatrix = 0;
        }

        m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.videoSignalTypePresentFlag =
            (get_cx_value(list_videoformat, _T("undef")) != (int)m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.videoFormat
            || m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.videoFullRangeFlag
            || m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.colourDescriptionPresentFlag) ? 1 : 0;
        if (!m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.videoSignalTypePresentFlag) {
            m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.h264VUIParameters.videoFormat = 0;
        }
    }

    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVEncCore::CreateEncoder(const InEncodeVideoParam *inputParam) {
    NVENCSTATUS nvStatus;

    if (NV_ENC_SUCCESS != (nvStatus = SetInputParam(inputParam)))
        return nvStatus;
    PrintMes(RGY_LOG_DEBUG, _T("SetInputParam: Success.\n"));

    if (NV_ENC_SUCCESS != (nvStatus = m_pEncodeAPI->nvEncInitializeEncoder(m_hEncoder, &m_stCreateEncodeParams))) {
        PrintMes(RGY_LOG_ERROR,
            _T("%s: %d (%s)\n"), FOR_AUO ? _T("エンコーダの初期化に失敗しました。\n") : _T("Failed to Initialize the encoder\n."),
            nvStatus, char_to_tstring(_nvencGetErrorEnum(nvStatus)).c_str());
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("m_pEncodeAPI->nvEncInitializeEncoder: Success.\n"));

    return nvStatus;
}

RGY_ERR NVEncCore::InitFilters(const InEncodeVideoParam *inputParam) {
    //cuvidデコーダの場合、cropを入力時に行っていない場合がある
    const bool cropRequired = cropEnabled(inputParam->input.crop)
        && m_pFileReader->getInputCodec() != RGY_CODEC_UNKNOWN
        && CUVID_DISABLE_CROP;

    FrameInfo inputFrame = { 0 };
    inputFrame.width = inputParam->input.srcWidth;
    inputFrame.height = inputParam->input.srcHeight;
    inputFrame.csp = inputParam->input.csp;
    const int croppedWidth = inputFrame.width - inputParam->input.crop.e.left - inputParam->input.crop.e.right;
    const int croppedHeight = inputFrame.height - inputParam->input.crop.e.bottom - inputParam->input.crop.e.up;
    if (!cropRequired) {
        //入力時にcrop済み
        inputFrame.width = croppedWidth;
        inputFrame.height = croppedHeight;
    }
    if (m_pFileReader->getInputCodec() != RGY_CODEC_UNKNOWN) {
        inputFrame.deivce_mem = true;
    }
    m_encFps = rgy_rational<int>(inputParam->input.fpsN, inputParam->input.fpsD);
    if (inputParam->vpp.deinterlace == cudaVideoDeinterlaceMode_Bob) {
        m_encFps *= 2;
    }

    //リサイザの出力すべきサイズ
    int resizeWidth  = croppedWidth;
    int resizeHeight = croppedHeight;
    m_uEncWidth = resizeWidth;
    m_uEncHeight = resizeHeight;
    if (inputParam->vpp.pad.enable) {
        m_uEncWidth  += inputParam->vpp.pad.right + inputParam->vpp.pad.left;
        m_uEncHeight += inputParam->vpp.pad.bottom + inputParam->vpp.pad.top;
    }

    //指定のリサイズがあればそのサイズに設定する
    if (inputParam->input.dstWidth > 0 && inputParam->input.dstHeight > 0) {
        m_uEncWidth = inputParam->input.dstWidth;
        m_uEncHeight = inputParam->input.dstHeight;
        resizeWidth = m_uEncWidth;
        resizeHeight = m_uEncHeight;
        if (inputParam->vpp.pad.enable) {
            resizeWidth -= (inputParam->vpp.pad.right + inputParam->vpp.pad.left);
            resizeHeight -= (inputParam->vpp.pad.bottom + inputParam->vpp.pad.top);
        }
    }
    bool resizeRequired = false;
    if (croppedWidth != resizeWidth || croppedHeight != resizeHeight) {
        resizeRequired = true;
    }
    //avhw読みではデコード直後にリサイズが可能
    if (resizeRequired && m_pFileReader->getInputCodec() != RGY_CODEC_UNKNOWN && enableCuvidResize(inputParam)) {
        inputFrame.width  = inputParam->input.dstWidth;
        inputFrame.height = inputParam->input.dstHeight;
        resizeRequired = false;
    }

    //picStructの設定
    m_stPicStruct = picstruct_rgy_to_enc(inputParam->input.picstruct);
    if (inputParam->vpp.deinterlace != cudaVideoDeinterlaceMode_Weave) {
        m_stPicStruct = NV_ENC_PIC_STRUCT_FRAME;
    } else if (inputParam->vpp.afs.enable || inputParam->vpp.nnedi.enable || inputParam->vpp.yadif.enable) {
        m_stPicStruct = NV_ENC_PIC_STRUCT_FRAME;
    }
    //インタレ解除の個数をチェック
    int deinterlacer = 0;
    if (inputParam->vpp.deinterlace != cudaVideoDeinterlaceMode_Weave) deinterlacer++;
    if (inputParam->vpp.afs.enable) deinterlacer++;
    if (inputParam->vpp.nnedi.enable) deinterlacer++;
    if (inputParam->vpp.yadif.enable) deinterlacer++;
    if (deinterlacer >= 2) {
        PrintMes(RGY_LOG_ERROR, _T("Activating 2 or more deinterlacer is not supported.\n"));
        return RGY_ERR_UNSUPPORTED;
    }

    //vpp-rffの制約事項
    if (inputParam->vpp.rff) {
#if ENABLE_AVSW_READER
        if (!m_cuvidDec) {
            PrintMes(RGY_LOG_ERROR, _T("vpp-rff can only be used with hw decoder.\n"));
            return RGY_ERR_UNSUPPORTED;
        }
#endif //#if ENABLE_AVSW_READER
        if (inputParam->vpp.deinterlace != cudaVideoDeinterlaceMode_Weave) {
            PrintMes(RGY_LOG_ERROR, _T("vpp-rff cannot be used with vpp-deinterlace.\n"));
            return RGY_ERR_UNSUPPORTED;
        }
        if (trim_active(&m_trimParam)) {
            PrintMes(RGY_LOG_ERROR, _T("vpp-rff cannot be used with trim.\n"));
            return RGY_ERR_UNSUPPORTED;
        }
    }
    //フィルタが必要
    if (resizeRequired
        || cropRequired
        || inputParam->vpp.delogo.enable
        || inputParam->vpp.gaussMaskSize > 0
        || inputParam->vpp.unsharp.enable
        || inputParam->vpp.knn.enable
        || inputParam->vpp.pmd.enable
        || inputParam->vpp.deband.enable
        || inputParam->vpp.edgelevel.enable
        || inputParam->vpp.afs.enable
        || inputParam->vpp.nnedi.enable
        || inputParam->vpp.yadif.enable
        || inputParam->vpp.tweak.enable
        || inputParam->vpp.colorspace.enable
        || inputParam->vpp.pad.enable
        || inputParam->vpp.subburn.size() > 0
        || inputParam->vpp.rff
        || inputParam->vpp.selectevery.enable
        ) {
        //swデコードならGPUに上げる必要がある
        if (m_pFileReader->getInputCodec() == RGY_CODEC_UNKNOWN) {
            unique_ptr<NVEncFilter> filterCrop(new NVEncFilterCspCrop());
            shared_ptr<NVEncFilterParamCrop> param(new NVEncFilterParamCrop());
            param->frameIn = inputFrame;
            param->frameOut.csp = param->frameIn.csp;
            param->frameOut.deivce_mem = true;
            param->baseFps = m_encFps;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filterCrop->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filterCrop));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        const auto encCsp = GetEncoderCSP(inputParam);
        auto filterCsp = encCsp;
        switch (filterCsp) {
        case RGY_CSP_NV12: filterCsp = RGY_CSP_YV12; break;
        case RGY_CSP_P010: filterCsp = RGY_CSP_YV12_16; break;
        default: break;
        }
        if (inputParam->vpp.afs.enable && RGY_CSP_CHROMA_FORMAT[inputFrame.csp] == RGY_CHROMAFMT_YUV444) {
            filterCsp = (RGY_CSP_BIT_DEPTH[inputFrame.csp] > 8) ? RGY_CSP_YUV444_16 : RGY_CSP_YUV444;
        }
        //colorspace
        if (inputParam->vpp.colorspace.enable) {
            unique_ptr<NVEncFilter> filter(new NVEncFilterColorspace());
            shared_ptr<NVEncFilterParamColorspace> param(new NVEncFilterParamColorspace());
            param->colorspace = inputParam->vpp.colorspace;
            param->encCsp = encCsp;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps = m_encFps;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filter->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filter));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        if (filterCsp != inputFrame.csp
            || cropRequired) { //cropが必要ならただちに適用する
            unique_ptr<NVEncFilter> filterCrop(new NVEncFilterCspCrop());
            shared_ptr<NVEncFilterParamCrop> param(new NVEncFilterParamCrop());
            param->frameIn = inputFrame;
            param->frameOut.csp = encCsp;
            switch (param->frameOut.csp) {
            case RGY_CSP_NV12:
                param->frameOut.csp = RGY_CSP_YV12;
                break;
            case RGY_CSP_P010:
                param->frameOut.csp = RGY_CSP_YV12_16;
                break;
            default:
                break;
            }
            if (cropRequired) {
                param->crop = inputParam->input.crop;
            }
            param->baseFps = m_encFps;
            param->frameOut.deivce_mem = true;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filterCrop->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filterCrop));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //rff
        if (inputParam->vpp.rff) {
            unique_ptr<NVEncFilter> filter(new NVEncFilterRff());
            shared_ptr<NVEncFilterParamRff> param(new NVEncFilterParamRff());
            param->frameIn  = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps  = m_encFps;
            param->inFps    = m_inputFps;
            param->timebase = m_outputTimebase;
            param->bOutOverwrite = true;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filter->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filter));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //delogo
        if (inputParam->vpp.delogo.enable) {
            unique_ptr<NVEncFilter> filter(new NVEncFilterDelogo());
            shared_ptr<NVEncFilterParamDelogo> param(new NVEncFilterParamDelogo());
            param->inputFileName = inputParam->inputFilename.c_str();
            param->cudaSchedule  = m_cudaSchedule;
            param->delogo        = inputParam->vpp.delogo;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps = m_encFps;
            param->bOutOverwrite = true;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filter->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filter));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //afs
        if (inputParam->vpp.afs.enable) {
            if ((inputParam->input.picstruct & (RGY_PICSTRUCT_TFF | RGY_PICSTRUCT_BFF)) == 0) {
                PrintMes(RGY_LOG_ERROR, _T("Please set input interlace field order (--interlace tff/bff) for vpp-afs.\n"));
                return RGY_ERR_INVALID_PARAM;
            }
            unique_ptr<NVEncFilter> filter(new NVEncFilterAfs());
            shared_ptr<NVEncFilterParamAfs> param(new NVEncFilterParamAfs());
            param->afs = inputParam->vpp.afs;
            param->afs.tb_order = (inputParam->input.picstruct & RGY_PICSTRUCT_TFF) != 0;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->inFps = m_inputFps;
            param->inTimebase = m_outputTimebase;
            param->outTimebase = m_outputTimebase;
            param->baseFps = m_encFps;
            param->outFilename = inputParam->outputFilename;
            param->cudaSchedule = m_cudaSchedule;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filter->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filter));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //nnedi
        if (inputParam->vpp.nnedi.enable) {
            if ((inputParam->input.picstruct & (RGY_PICSTRUCT_TFF | RGY_PICSTRUCT_BFF)) == 0) {
                PrintMes(RGY_LOG_ERROR, _T("Please set input interlace field order (--interlace tff/bff) for vpp-nnedi.\n"));
                return RGY_ERR_INVALID_PARAM;
            }
            const auto selectedGpu = std::find_if(m_GPUList.begin(), m_GPUList.end(), [device_id = m_nDeviceId](const NVGPUInfo& gpuinfo) {
                return gpuinfo.id == device_id;
            });
            unique_ptr<NVEncFilter> filter(new NVEncFilterNnedi());
            shared_ptr<NVEncFilterParamNnedi> param(new NVEncFilterParamNnedi());
            param->nnedi = inputParam->vpp.nnedi;
            param->compute_capability = selectedGpu->compute_capability;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps = m_encFps;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filter->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filter));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //yadif
        if (inputParam->vpp.yadif.enable) {
            if ((inputParam->input.picstruct & (RGY_PICSTRUCT_TFF | RGY_PICSTRUCT_BFF)) == 0) {
                PrintMes(RGY_LOG_ERROR, _T("Please set input interlace field order (--interlace tff/bff) for vpp-yadif.\n"));
                return RGY_ERR_INVALID_PARAM;
            }
            unique_ptr<NVEncFilter> filter(new NVEncFilterYadif());
            shared_ptr<NVEncFilterParamYadif> param(new NVEncFilterParamYadif());
            param->yadif = inputParam->vpp.yadif;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps = m_encFps;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filter->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filter));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //select-every
        if (inputParam->vpp.selectevery.enable) {
            unique_ptr<NVEncFilter> filter(new NVEncFilterSelectEvery());
            shared_ptr<NVEncFilterParamSelectEvery> param(new NVEncFilterParamSelectEvery());
            param->frameIn  = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps  = m_encFps;
            param->selectevery = inputParam->vpp.selectevery;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filter->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filter));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //ノイズ除去 (knn)
        if (inputParam->vpp.knn.enable) {
            unique_ptr<NVEncFilter> filter(new NVEncFilterDenoiseKnn());
            shared_ptr<NVEncFilterParamDenoiseKnn> param(new NVEncFilterParamDenoiseKnn());
            param->knn = inputParam->vpp.knn;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps = m_encFps;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filter->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filter));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //ノイズ除去 (pmd)
        if (inputParam->vpp.pmd.enable) {
            unique_ptr<NVEncFilter> filter(new NVEncFilterDenoisePmd());
            shared_ptr<NVEncFilterParamDenoisePmd> param(new NVEncFilterParamDenoisePmd());
            param->pmd = inputParam->vpp.pmd;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps = m_encFps;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filter->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filter));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //ノイズ除去
        if (inputParam->vpp.gaussMaskSize > 0) {
#if _M_IX86
            PrintMes(RGY_LOG_ERROR, _T("gauss denoise filter not supported in x86.\n"));
            return RGY_ERR_UNSUPPORTED;
#else
            unique_ptr<NVEncFilter> filterGauss(new NVEncFilterDenoiseGauss());
            shared_ptr<NVEncFilterParamGaussDenoise> param(new NVEncFilterParamGaussDenoise());
            param->masksize = inputParam->vpp.gaussMaskSize;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps = m_encFps;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filterGauss->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filterGauss));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
#endif
        }
        //字幕焼きこみ
        for (const auto& subburn : inputParam->vpp.subburn) {
            if (!subburn.enable)
#if ENABLE_AVSW_READER
            if (subburn.filename.length() > 0
                && m_trimParam.list.size() > 0) {
                PrintMes(RGY_LOG_ERROR, _T("--vpp-subburn with input as file cannot be used with --trim.\n"));
                return RGY_ERR_UNSUPPORTED;
            }
            unique_ptr<NVEncFilter> filter(new NVEncFilterSubburn());
            shared_ptr<NVEncFilterParamSubburn> param(new NVEncFilterParamSubburn());
            param->subburn = subburn;
            auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
            if (pAVCodecReader != nullptr) {
                param->videoInputStream = pAVCodecReader->GetInputVideoStream();
                param->videoInputFirstKeyPts = pAVCodecReader->GetVideoFirstKeyPts();
                param->videoInfo = m_pFileReader->GetInputFrameInfo();
                for (const auto &stream : pAVCodecReader->GetInputStreamInfo()) {
                    if (stream.nTrackId == trackFullID(AVMEDIA_TYPE_SUBTITLE, param->subburn.trackId)) {
                        param->streamIn = stream;
                        break;
                    }
                }
            }
            param->bOutOverwrite = true;
            param->videoTimebase = av_make_q(m_outputTimebase);
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps = m_encFps;
            param->crop = inputParam->input.crop;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filter->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filter));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
#else
            PrintMes(RGY_LOG_ERROR, _T("--vpp-subburn not supported in this build.\n"));
            return RGY_ERR_UNSUPPORTED;
#endif
        }
        //リサイズ
        if (resizeRequired) {
            unique_ptr<NVEncFilter> filterCrop(new NVEncFilterResize());
            shared_ptr<NVEncFilterParamResize> param(new NVEncFilterParamResize());
            param->interp = (inputParam->vpp.resizeInterp != NPPI_INTER_UNDEFINED) ? inputParam->vpp.resizeInterp : RESIZE_CUDA_SPLINE36;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->frameOut.width = resizeWidth;
            param->frameOut.height = resizeHeight;
            param->baseFps = m_encFps;
            param->bOutOverwrite = false;
#if _M_IX86
            if (param->interp <= NPPI_INTER_MAX) {
                param->interp = RESIZE_CUDA_SPLINE36;
                PrintMes(RGY_LOG_WARN, _T("npp resize filters not supported in x86, switching to %s.\n"), get_chr_from_value(list_nppi_resize, param->interp));
            }
#endif
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filterCrop->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filterCrop));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //unsharp
        if (inputParam->vpp.unsharp.enable) {
            unique_ptr<NVEncFilter> filterUnsharp(new NVEncFilterUnsharp());
            shared_ptr<NVEncFilterParamUnsharp> param(new NVEncFilterParamUnsharp());
            param->unsharp.radius = inputParam->vpp.unsharp.radius;
            param->unsharp.weight = inputParam->vpp.unsharp.weight;
            param->unsharp.threshold = inputParam->vpp.unsharp.threshold;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps = m_encFps;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filterUnsharp->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filterUnsharp));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //edgelevel
        if (inputParam->vpp.edgelevel.enable) {
            unique_ptr<NVEncFilter> filterEdgelevel(new NVEncFilterEdgelevel());
            shared_ptr<NVEncFilterParamEdgelevel> param(new NVEncFilterParamEdgelevel());
            param->edgelevel = inputParam->vpp.edgelevel;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps = m_encFps;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filterEdgelevel->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filterEdgelevel));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //tweak
        if (inputParam->vpp.tweak.enable) {
            unique_ptr<NVEncFilter> filterEq(new NVEncFilterTweak());
            shared_ptr<NVEncFilterParamTweak> param(new NVEncFilterParamTweak());
            param->tweak = inputParam->vpp.tweak;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps = m_encFps;
            param->bOutOverwrite = true;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filterEq->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filterEq));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //deband
        if (inputParam->vpp.deband.enable) {
            unique_ptr<NVEncFilter> filter(new NVEncFilterDeband());
            shared_ptr<NVEncFilterParamDeband> param(new NVEncFilterParamDeband());
            param->deband = inputParam->vpp.deband;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->baseFps = m_encFps;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filter->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filter));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
        //padding
        if (inputParam->vpp.pad.enable) {
            unique_ptr<NVEncFilter> filter(new NVEncFilterPad());
            shared_ptr<NVEncFilterParamPad> param(new NVEncFilterParamPad());
            param->pad = inputParam->vpp.pad;
            param->frameIn = inputFrame;
            param->frameOut = inputFrame;
            param->frameOut.width = m_uEncWidth;
            param->frameOut.height = m_uEncHeight;
            param->frameOut.pitch = 0;
            param->baseFps = m_encFps;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filter->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            //フィルタチェーンに追加
            m_vpFilters.push_back(std::move(filter));
            //パラメータ情報を更新
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
            m_encFps = param->baseFps;
        }
    }
    //最後のフィルタ
    {
        //もし入力がCPUメモリで色空間が違うなら、一度そのままGPUに転送する必要がある
        if (inputFrame.deivce_mem == false && inputFrame.csp != GetEncoderCSP(inputParam)) {
            unique_ptr<NVEncFilter> filterCrop(new NVEncFilterCspCrop());
            shared_ptr<NVEncFilterParamCrop> param(new NVEncFilterParamCrop());
            param->frameIn = inputFrame;
            param->frameOut.csp = param->frameIn.csp;
            //インタレ保持であれば、CPU側にフレームを戻す必要がある
            //色空間が同じなら、ここでやってしまう
            param->frameOut.deivce_mem = true;
            param->bOutOverwrite = false;
            NVEncCtxAutoLock(cxtlock(m_ctxLock));
            auto sts = filterCrop->init(param, m_pNVLog);
            if (sts != NV_ENC_SUCCESS) {
                return sts;
            }
            m_vpFilters.push_back(std::move(filterCrop));
            m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
            //入力フレーム情報を更新
            inputFrame = param->frameOut;
        }
        const bool bDeviceMemFinal = (m_stPicStruct != NV_ENC_PIC_STRUCT_FRAME && inputFrame.csp == GetEncoderCSP(inputParam)) ? false : true;
        unique_ptr<NVEncFilter> filterCrop(new NVEncFilterCspCrop());
        shared_ptr<NVEncFilterParamCrop> param(new NVEncFilterParamCrop());
        param->frameIn = inputFrame;
        param->frameOut.csp = GetEncoderCSP(inputParam);
        //インタレ保持であれば、CPU側にフレームを戻す必要がある
        //色空間が同じなら、ここでやってしまう
        param->frameOut.deivce_mem = bDeviceMemFinal;
        param->bOutOverwrite = false;
        NVEncCtxAutoLock(cxtlock(m_ctxLock));
        auto sts = filterCrop->init(param, m_pNVLog);
        if (sts != NV_ENC_SUCCESS) {
            return sts;
        }
        m_vpFilters.push_back(std::move(filterCrop));
        m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
    }

    //インタレ保持の場合は、CPU側に戻す必要がある
    if (m_stPicStruct != NV_ENC_PIC_STRUCT_FRAME && m_pLastFilterParam->frameOut.deivce_mem) {
        unique_ptr<NVEncFilter> filterCopyDtoH(new NVEncFilterCspCrop());
        shared_ptr<NVEncFilterParamCrop> param(new NVEncFilterParamCrop());
        param->frameIn = inputFrame;
        param->frameOut = inputFrame;
        param->frameOut.deivce_mem = false;
        param->bOutOverwrite = false;
        NVEncCtxAutoLock(cxtlock(m_ctxLock));
        auto sts = filterCopyDtoH->init(param, m_pNVLog);
        if (sts != NV_ENC_SUCCESS) {
            return sts;
        }
        //フィルタチェーンに追加
        m_vpFilters.push_back(std::move(filterCopyDtoH));
        //パラメータ情報を更新
        m_pLastFilterParam = std::dynamic_pointer_cast<NVEncFilterParam>(param);
        //入力フレーム情報を更新
        inputFrame = param->frameOut;
    }
    //パフォーマンスチェックを行うかどうか
    {
        NVEncCtxAutoLock(cxtlock(m_ctxLock));
        for (auto& filter : m_vpFilters) {
            filter->CheckPerformance(inputParam->vpp.bCheckPerformance);
        }
    }
    return RGY_ERR_NONE;
}

bool NVEncCore::VppRffEnabled() {
    return std::find_if(m_vpFilters.begin(), m_vpFilters.end(),
        [](unique_ptr<NVEncFilter>& filter) { return typeid(*filter) == typeid(NVEncFilterRff); }
    ) != m_vpFilters.end();
}

bool NVEncCore::VppAfsRffAware() {
    //vpp-afsのrffが使用されているか
    const auto vpp_afs_filter = std::find_if(m_vpFilters.begin(), m_vpFilters.end(),
        [](unique_ptr<NVEncFilter>& filter) { return typeid(*filter) == typeid(NVEncFilterAfs); }
    );
    bool vpp_afs_rff_aware = false;
    if (vpp_afs_filter != m_vpFilters.end()) {
        auto afs_prm = reinterpret_cast<const NVEncFilterParamAfs *>(vpp_afs_filter->get()->GetFilterParam());
        if (afs_prm != nullptr) {
            vpp_afs_rff_aware = afs_prm->afs.rff;
        }
    }
    return vpp_afs_rff_aware;
}

NVENCSTATUS NVEncCore::CheckGPUListByEncoder(const InEncodeVideoParam *inputParam) {
    if (m_nDeviceId >= 0) {
        //手動で設定されている
        return NV_ENC_SUCCESS;
    }
    RGY_CODEC rgy_codec = RGY_CODEC_UNKNOWN;
    switch (inputParam->codec) {
    case NV_ENC_H264: rgy_codec = RGY_CODEC_H264; break;
    case NV_ENC_HEVC: rgy_codec = RGY_CODEC_HEVC; break;
    default:
        PrintMes(RGY_LOG_ERROR, _T("Unknown codec.\n"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    //エンコーダの対応をチェック
    tstring message; //GPUチェックのメッセージ
    for (auto gpu = m_GPUList.begin(); gpu != m_GPUList.end(); ) {
        //コーデックのチェック
        const auto codec = std::find_if(gpu->nvenc_codec_features.begin(), gpu->nvenc_codec_features.end(), [rgy_codec](const NVEncCodecFeature& codec) {
            return codec.codec == codec_guid_rgy_to_enc(rgy_codec);
        });
        if (codec == gpu->nvenc_codec_features.end()) {
            message += strsprintf(_T("GPU #%d (%s) does not support %s encoding.\n"),
                gpu->id, gpu->name.c_str(), CodecToStr(rgy_codec).c_str());
            gpu = m_GPUList.erase(gpu);
            continue;
        }
        //プロファイルのチェック
        auto codecProfileGUID = inputParam->encConfig.profileGUID;
        if (rgy_codec == RGY_CODEC_HEVC) {
            //はデフォルトではH.264のプロファイル情報
            //HEVCのプロファイル情報は、inputParam->encConfig.encodeCodecConfig.hevcConfig.tierの下位16bitに保存されている
            codecProfileGUID = get_guid_from_value(inputParam->encConfig.encodeCodecConfig.hevcConfig.tier & 0xffff, h265_profile_names);
            if (inputParam->yuv444) {
                codecProfileGUID = NV_ENC_HEVC_PROFILE_FREXT_GUID;
            } else if (inputParam->encConfig.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 > 0) {
                codecProfileGUID = (inputParam->yuv444) ? NV_ENC_HEVC_PROFILE_FREXT_GUID : NV_ENC_HEVC_PROFILE_MAIN10_GUID;
            }
        } else if (rgy_codec == RGY_CODEC_H264) {
            if (inputParam->yuv444) {
                codecProfileGUID = NV_ENC_H264_PROFILE_HIGH_444_GUID;
            }
        } else {
            PrintMes(RGY_LOG_ERROR, _T("Unknown codec.\n"));
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
        const auto profile = std::find_if(codec->profiles.begin(), codec->profiles.end(), [codecProfileGUID](const GUID& profile_guid) {
            return 0 == memcmp(&codecProfileGUID, &profile_guid, sizeof(profile_guid));
        });
        if (profile == codec->profiles.end()) {
            message += strsprintf(_T("GPU #%d (%s) cannot encode %s %s.\n"), gpu->id, gpu->name.c_str(),
                CodecToStr(rgy_codec).c_str(),
                get_codec_profile_name_from_guid(rgy_codec, codecProfileGUID).c_str());
            gpu = m_GPUList.erase(gpu);
            continue;
        }
        if (inputParam->lossless && !get_value(NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE, codec->caps)) {
            message += strsprintf(_T("GPU #%d (%s) does not support lossless encoding.\n"), gpu->id, gpu->name.c_str());
            gpu = m_GPUList.erase(gpu);
            continue;
        }
        if (inputParam->yuv444 && !get_value(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE, codec->caps)) {
            message += strsprintf(_T("GPU #%d (%s) does not support yuv444 encoding.\n"), gpu->id, gpu->name.c_str());
            gpu = m_GPUList.erase(gpu);
            continue;
        }
        if (inputParam->codec == NV_ENC_HEVC && inputParam->encConfig.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 > 0 && !get_value(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE, codec->caps)) {
            message += strsprintf(_T("GPU #%d (%s) does not support HEVC 10bit depth encoding.\n"), gpu->id, gpu->name.c_str());
            gpu = m_GPUList.erase(gpu);
            continue;
        }
        if (inputParam->codec == NV_ENC_H264
            && (
                (inputParam->input.picstruct & RGY_PICSTRUCT_INTERLACED)
                && (inputParam->vpp.deinterlace == cudaVideoDeinterlaceMode_Weave
                    && !inputParam->vpp.afs.enable
                    && !inputParam->vpp.nnedi.enable
                    && !inputParam->vpp.yadif.enable))
            && !get_value(NV_ENC_CAPS_SUPPORT_FIELD_ENCODING, codec->caps)) {
            message += strsprintf(_T("GPU #%d (%s) does not support H.264 interlaced encoding.\n"), gpu->id, gpu->name.c_str());
            gpu = m_GPUList.erase(gpu);
            continue;
        }
        PrintMes(RGY_LOG_DEBUG, _T("GPU #%d (%s) available for encode.\n"), gpu->id, gpu->name.c_str());
        gpu++;
    }
    PrintMes((m_GPUList.size() == 0) ? RGY_LOG_ERROR : RGY_LOG_DEBUG, _T("%s\n"), message.c_str());
    if (m_GPUList.size() == 0) {
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    if (m_GPUList.size() == 1) {
        m_nDeviceId = m_GPUList.front().id;
        return NV_ENC_SUCCESS;
    }

    if (inputParam->encConfig.frameIntervalP > 1) {
        bool support_bframe = false;
        //エンコード対象のBフレームサポートのあるGPUがあるかを確認する
        for (const auto& gpu : m_GPUList) {
            const auto codec = std::find_if(gpu.nvenc_codec_features.begin(), gpu.nvenc_codec_features.end(), [rgy_codec](const NVEncCodecFeature& codec) {
                return codec.codec == codec_guid_rgy_to_enc(rgy_codec);
            });
            assert(codec != gpu.nvenc_codec_features.end());
            if (get_value(NV_ENC_CAPS_NUM_MAX_BFRAMES, codec->caps) > 0) {
                support_bframe = true;
                break;
            }
        }
        //BフレームサポートのあるGPUがあれば、そのGPU以外は除外する
        if (support_bframe) {
            for (auto gpu = m_GPUList.begin(); gpu != m_GPUList.end(); ) {
                //コーデックのチェック
                const auto codec = std::find_if(gpu->nvenc_codec_features.begin(), gpu->nvenc_codec_features.end(), [rgy_codec](const NVEncCodecFeature& codec) {
                    return codec.codec == codec_guid_rgy_to_enc(rgy_codec);
                });
                assert(codec != gpu->nvenc_codec_features.end());
                if (get_value(NV_ENC_CAPS_NUM_MAX_BFRAMES, codec->caps) == 0) {
                    gpu = m_GPUList.erase(gpu);
                    continue;
                }
                gpu++;
            }
        }
    }
    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVEncCore::GPUAutoSelect(const InEncodeVideoParam *inputParam) {
    UNREFERENCED_PARAMETER(inputParam);
    if (m_GPUList.size() <= 1) {
        m_nDeviceId = m_GPUList.front().id;
        return NV_ENC_SUCCESS;
    }
    std::map<int, double> gpuscore;
    for (const auto& gpu : m_GPUList) {
        double core_score = gpu.cuda_cores * inputParam->gpuSelect.cores;
        double cc_score = (gpu.compute_capability.first * 10.0 + gpu.compute_capability.second) * inputParam->gpuSelect.gen;
        double ve_score = 0.0;
        double gpu_score = 0.0;

        NVMLMonitorInfo info;
#if ENABLE_NVML
        NVMLMonitor monitor;
        auto nvml_ret = monitor.Init(gpu.pciBusId);
        if (nvml_ret == NVML_SUCCESS
            && monitor.getData(&info) == NVML_SUCCESS) {
#else
        NVSMIInfo nvsmi;
        if (nvsmi.getData(&info, gpu.pciBusId) == 0) {
#endif
            ve_score  = 100.0 * (1.0 - std::pow(info.VEELoad / 100.0, 1.0)) * inputParam->gpuSelect.ve;
            gpu_score = 100.0 * (1.0 - std::pow(info.GPULoad / 100.0, 1.5)) * inputParam->gpuSelect.gpu;
            PrintMes(RGY_LOG_DEBUG, _T("GPU #%d (%s) Load: GPU %.1f, VE: %.1f.\n"), gpu.id, gpu.name.c_str(), info.GPULoad, info.VEELoad);
        }
        gpuscore[gpu.id] = cc_score + ve_score + gpu_score + core_score;
        PrintMes(RGY_LOG_DEBUG, _T("GPU #%d (%s) score: %.1f: VE %.1f, GPU %.1f, CC %.1f, Core %.1f.\n"), gpu.id, gpu.name.c_str(),
            gpuscore[gpu.id], ve_score, gpu_score, cc_score, core_score);
    }
    m_GPUList.sort([&](NVGPUInfo& a, NVGPUInfo& b) {
        if (gpuscore.at(a.id) != gpuscore.at(b.id)) {
            return gpuscore.at(a.id) > gpuscore.at(b.id);
        }
        return a.id < b.id;
    });

    PrintMes(RGY_LOG_DEBUG, _T("GPU Priority\n"));
    for (const auto& gpu : m_GPUList) {
        PrintMes(RGY_LOG_DEBUG, _T("GPU #%d (%s): score %.1f\n"), gpu.id, gpu.name.c_str(), gpuscore[gpu.id]);
    }
    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVEncCore::InitDevice(const InEncodeVideoParam *inputParam) {
    auto nvStatus = NV_ENC_SUCCESS;
    if (NV_ENC_SUCCESS != (nvStatus = InitCuda(inputParam->nCudaSchedule))) {
        PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("Cudaの初期化に失敗しました。\n") : _T("Failed to initialize CUDA.\n"));
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("InitCuda: Success.\n"));

    MYPROC nvEncodeAPICreateInstance; // function pointer to create instance in nvEncodeAPI
    if (NULL == (nvEncodeAPICreateInstance = (MYPROC)GetProcAddress(m_hinstLib, "NvEncodeAPICreateInstance"))) {
        PrintMes(RGY_LOG_ERROR, _T("Failed to load address of NvEncodeAPICreateInstance from %s.\n"), NVENCODE_API_DLL);
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }
    if (NULL == (m_pEncodeAPI = new NV_ENCODE_API_FUNCTION_LIST)) {
        PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("NV_ENCODE_API_FUNCTION_LIST用のメモリ確保に失敗しました。\n") : _T("Failed to allocate memory for NV_ENCODE_API_FUNCTION_LIST.\n"));
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }

    memset(m_pEncodeAPI, 0, sizeof(NV_ENCODE_API_FUNCTION_LIST));
    m_pEncodeAPI->version = NV_ENCODE_API_FUNCTION_LIST_VER;

    if (NV_ENC_SUCCESS != (nvStatus = nvEncodeAPICreateInstance(m_pEncodeAPI))) {
        if (nvStatus == NV_ENC_ERR_INVALID_VERSION) {
            PrintMes(RGY_LOG_ERROR, _T("Failed to create instance of nvEncodeAPI(ver=0x%x), please consider updating your GPU driver.\n"), NV_ENCODE_API_FUNCTION_LIST_VER);
        } else {
            NVPrintFuncError(_T("nvEncodeAPICreateInstance"), nvStatus);
        }
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("nvEncodeAPICreateInstance(APIVer=0x%x): Success.\n"), NV_ENCODE_API_FUNCTION_LIST_VER);

    if (NV_ENC_SUCCESS != (nvStatus = NvEncOpenEncodeSessionEx(m_pDevice, NV_ENC_DEVICE_TYPE_CUDA, inputParam->sessionRetry))) {
        if (nvStatus == NV_ENC_ERR_INVALID_VERSION) {
            PrintMes(RGY_LOG_ERROR, _T("Failed to create instance of NvEncOpenEncodeSessionEx(device_type=NV_ENC_DEVICE_TYPE_CUDA), please consider updating your GPU driver.\n"), NV_ENCODE_API_FUNCTION_LIST_VER);
        } else {
            NVPrintFuncError(_T("NvEncOpenEncodeSessionEx(device_type=NV_ENC_DEVICE_TYPE_CUDA)"), nvStatus);
        }
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("NvEncOpenEncodeSessionEx(device_type=NV_ENC_DEVICE_TYPE_CUDA): Success.\n"));
    return NV_ENC_SUCCESS;
}

RGY_ERR NVEncCore::CheckDynamicRCParams(std::vector<DynamicRCParam>& dynamicRC) {
    if (dynamicRC.size() == 0) {
        return RGY_ERR_NONE;
    }
    std::sort(dynamicRC.begin(), dynamicRC.end(), [](const DynamicRCParam& a, const DynamicRCParam& b) {
        return (a.start == b.start) ? a.end < b.end : a.start < b.start;
    });
    std::for_each(dynamicRC.begin(), dynamicRC.end(), [](DynamicRCParam &a) {
        if (a.end <= 0) {
            a.end = TRIM_MAX;
        }
    });
    int id = 0;
    for (auto a : dynamicRC) {
        if (a.start < id) {
            PrintMes(RGY_LOG_ERROR, _T("Invalid sequence of frame ID in --dynamic-rc.\n"));
            PrintMes(RGY_LOG_ERROR, _T("%s\n"), printParams(dynamicRC).c_str());
            return RGY_ERR_INVALID_PARAM;
        }
        id = a.start;
        if (a.end > 0 && a.end < id) {
            PrintMes(RGY_LOG_ERROR, _T("Invalid sequence of frame ID in --dynamic-rc.\n"));
            PrintMes(RGY_LOG_ERROR, _T("%s\n"), printParams(dynamicRC).c_str());
            return RGY_ERR_INVALID_PARAM;
        }
    }
    PrintMes(RGY_LOG_DEBUG, _T("%s\n"), printParams(dynamicRC).c_str());
    m_dynamicRC = dynamicRC;
    m_appliedDynamicRC = DYNAMIC_PARAM_NOT_SELECTED;
    return RGY_ERR_NONE;
}

NVENCSTATUS NVEncCore::InitEncode(InEncodeVideoParam *inputParam) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;

    bool bOutputHighBitDepth = inputParam->codec == NV_ENC_HEVC && inputParam->encConfig.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 > 0;
    if (inputParam->lossless || inputParam->vpp.colorspace.enable) {
        inputParam->input.csp = RGY_CSP_NA; //なるべくそのままの色空間のままGPUへ転送する
    } else {
        if (bOutputHighBitDepth) {
            inputParam->input.csp = (inputParam->yuv444) ? RGY_CSP_YUV444_16 : RGY_CSP_P010;
        } else {
            inputParam->input.csp = (inputParam->yuv444) ? RGY_CSP_YUV444 : RGY_CSP_NV12;
        }
    }
    m_nAVSyncMode = inputParam->nAVSyncMode;
    m_nProcSpeedLimit = inputParam->nProcSpeedLimit;

    //デコーダが使用できるか確認する必要があるので、先にGPU関係の情報を取得しておく必要がある
    NVEncoderGPUInfo gpuInfo(m_nDeviceId, true);
    m_GPUList = gpuInfo.getGPUList();
    if (0 == m_GPUList.size()) {
        gpuInfo = NVEncoderGPUInfo(-1, true);
        m_GPUList = gpuInfo.getGPUList();
        if (0 == m_GPUList.size()) {
            PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("NVEncが使用可能なGPUが見つかりませんでした。\n") : _T("No GPU found suitable for NVEnc Encoding.\n"));
            return NV_ENC_ERR_NO_ENCODE_DEVICE;
        } else {
            PrintMes(RGY_LOG_WARN, _T("DeviceId #%d not found, automatically selected default device.\n"), m_nDeviceId);
            m_nDeviceId = -1;
        }
    }
    //リスト中のGPUのうち、まずは指定されたHWエンコードが可能なもののみを選択
    if (NV_ENC_SUCCESS != (nvStatus = CheckGPUListByEncoder(inputParam))) {
        PrintMes(RGY_LOG_ERROR, _T("Unknown erro occurred during checking GPU.\n"));
        return nvStatus;
    }
    if (0 == m_GPUList.size()) {
        PrintMes(RGY_LOG_ERROR, FOR_AUO
            ? _T("指定されたコーデック/プロファイルをエンコード可能なGPUがみつかりまえせんでした。\n")
            : _T("No suitable GPU found for codec / profile specified.\n"));
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    }

    //使用するGPUの優先順位を決定
    if (NV_ENC_SUCCESS != (nvStatus = GPUAutoSelect(inputParam))) {
        PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("GPUの自動選択に失敗しました。\n") : _T("Failed to select gpu.\n"));
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("GPUAutoSelect: Success.\n"));

    auto rgy_err = CheckDynamicRCParams(inputParam->dynamicRC);
    if (rgy_err != RGY_ERR_NONE) {
        PrintMes(RGY_LOG_DEBUG, _T("Error in dynamic rate control params.\n"));
        return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }

    //入力ファイルを開き、入力情報も取得
    //デコーダが使用できるか確認する必要があるので、先にGPU関係の情報を取得しておく必要がある
    if (NV_ENC_SUCCESS != (nvStatus = InitInput(inputParam))) {
        PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("入力ファイルを開けませんでした。\n") : _T("Failed to open input file.\n"));
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("InitInput: Success.\n"));

    if (inputParam->lossless) {
        const auto inputFrameInfo = m_pFileReader->GetInputFrameInfo();
        //入力ファイルの情報をもとに修正
        //なるべくオリジナルに沿ったものにエンコードする
        inputParam->yuv444 = (RGY_CSP_CHROMA_FORMAT[inputFrameInfo.csp] != RGY_CHROMAFMT_YUV420);

        if (inputParam->codec == NV_ENC_HEVC && RGY_CSP_BIT_DEPTH[inputFrameInfo.csp] > 8) {
            inputParam->encConfig.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 = 2;
            bOutputHighBitDepth = true;
        }
    }

    if (m_GPUList.size() > 1 && m_nDeviceId < 0) {
#if ENABLE_AVSW_READER
        RGYInputAvcodec *pReader = dynamic_cast<RGYInputAvcodec *>(m_pFileReader.get());
        if (pReader != nullptr) {
            m_nDeviceId = pReader->GetHWDecDeviceID();
            if (m_nDeviceId >= 0) {
                const auto gpu = std::find_if(m_GPUList.begin(), m_GPUList.end(), [device_id = m_nDeviceId](const NVGPUInfo& gpuinfo) {
                    return gpuinfo.id == device_id;
                });
                PrintMes(RGY_LOG_DEBUG, _T("device #%d (%s) selected by reader.\n"), gpu->id, gpu->name.c_str());
            } else {
                PrintMes(RGY_LOG_DEBUG, _T("reader has not selected device.\n"));
            }
        }
#endif
        if (m_nDeviceId < 0) {
            m_nDeviceId = m_GPUList.front().id;
            PrintMes(RGY_LOG_DEBUG, _T("device #%d (%s) selected.\n"), m_GPUList.front().id, m_GPUList.front().name.c_str());
        }
    }

    if (NV_ENC_SUCCESS != (nvStatus = InitDevice(inputParam))) {
        PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("NVENCのインスタンス作成に失敗しました。\n") : _T("Failed to create NVENC instance.\n"));
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("InitNVEncInstance: Success.\n"));

    const auto selectedGpu = std::find_if(m_GPUList.begin(), m_GPUList.end(), [device_id = m_nDeviceId](const NVGPUInfo& gpuinfo) {
        return gpuinfo.id == device_id;
    });
    if (true) {
        const bool bLogOutput = inputParam->nPerfMonitorSelect || inputParam->nPerfMonitorSelectMatplot;
        tstring perfMonLog;
        if (bLogOutput) {
            perfMonLog = inputParam->outputFilename + _T("_perf.csv");
        }
        CPerfMonitorPrm perfMonitorPrm = { 0 };
#if ENABLE_NVML
        perfMonitorPrm.pciBusId = selectedGpu->pciBusId.c_str();
#endif
        if (m_pPerfMonitor->init(perfMonLog.c_str(), _T(""), (bLogOutput) ? inputParam->nPerfMonitorInterval : 1000,
            (int)inputParam->nPerfMonitorSelect, (int)inputParam->nPerfMonitorSelectMatplot,
#if defined(_WIN32) || defined(_WIN64)
            std::unique_ptr<void, handle_deleter>(OpenThread(SYNCHRONIZE | THREAD_QUERY_INFORMATION, false, GetCurrentThreadId()), handle_deleter()),
#else
            nullptr,
#endif
            m_pNVLog, &perfMonitorPrm)) {
            PrintMes(RGY_LOG_WARN, _T("Failed to initialize performance monitor, disabled.\n"));
            m_pPerfMonitor.reset();
        }
    }

    //作成したデバイスの情報をfeature取得
    if (NV_ENC_SUCCESS != (nvStatus = createDeviceFeatureList(false))) {
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("createDeviceFeatureList: Success.\n"));

    //必要ならデコーダを作成
    if (NV_ENC_SUCCESS != (nvStatus = InitDecoder(inputParam))) {
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("InitDecoder: Success.\n"));

    //必要ならフィルターを作成
    if (InitFilters(inputParam) != RGY_ERR_NONE) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    PrintMes(RGY_LOG_DEBUG, _T("InitFilters: Success.\n"));

    //エンコーダにパラメータを渡し、初期化
    if (NV_ENC_SUCCESS != (nvStatus = CreateEncoder(inputParam))) {
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("CreateEncoder: Success.\n"));

    //入出力用メモリ確保
    NV_ENC_BUFFER_FORMAT encBufferFormat;
    if (bOutputHighBitDepth) {
        encBufferFormat = (inputParam->yuv444) ? NV_ENC_BUFFER_FORMAT_YUV444_10BIT : NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
    } else {
        encBufferFormat = (inputParam->yuv444) ? NV_ENC_BUFFER_FORMAT_YUV444_PL : NV_ENC_BUFFER_FORMAT_NV12_PL;
    }
    m_nAVSyncMode = inputParam->nAVSyncMode;
    if (NV_ENC_SUCCESS != (nvStatus = AllocateIOBuffers(m_uEncWidth, m_uEncHeight, encBufferFormat, &inputParam->input))) {
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("AllocateIOBuffers: Success.\n"));

    //エンコーダにパラメータを渡し、初期化
    if (NV_ENC_SUCCESS != (nvStatus = InitChapters(inputParam))) {
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("InitChapters: Success.\n"));

#if ENABLE_AVSW_READER
    if (inputParam->keyFile.length() > 0) {
        if (m_trimParam.list.size() > 0) {
            PrintMes(RGY_LOG_WARN, _T("--keyfile could not be used with --trim, disabled.\n"));
        } else {
            m_keyFile = read_keyfile(inputParam->keyFile);
            if (m_keyFile.size() == 0) {
                PrintMes(RGY_LOG_ERROR, _T("Failed to read keyFile \"%s\".\n"), inputParam->keyFile.c_str());
                return NV_ENC_ERR_GENERIC;
            }
        }
    }
#endif //#if ENABLE_AVSW_READER

    //出力ファイルを開く
    if (NV_ENC_SUCCESS != (nvStatus = InitOutput(inputParam, encBufferFormat))) {
        PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("出力ファイルのオープンに失敗しました。: \"%s\"\n") : _T("Failed to open output file: \"%s\"\n"), inputParam->outputFilename.c_str());
        return nvStatus;
    }
    PrintMes(RGY_LOG_DEBUG, _T("InitOutput: Success.\n"), inputParam->outputFilename.c_str());
    return nvStatus;
}

NVENCSTATUS NVEncCore::Initialize(InEncodeVideoParam *inputParam) {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;

    InitLog(inputParam);

    if (NULL == m_hinstLib) {
        if (NULL == (m_hinstLib = LoadLibrary(NVENCODE_API_DLL))) {
#if FOR_AUO
            PrintMes(RGY_LOG_ERROR, _T("%sがシステムに存在しません。\n"), NVENCODE_API_DLL);
            PrintMes(RGY_LOG_ERROR, _T("NVIDIAのドライバが動作条件を満たしているか確認して下さい。"));
#else
            PrintMes(RGY_LOG_ERROR, _T("%s does not exists in your system.\n"), NVENCODE_API_DLL);
            PrintMes(RGY_LOG_ERROR, _T("Please check if the GPU driver is propery installed."));
#endif
            return NV_ENC_ERR_OUT_OF_MEMORY;
        }
    }
    PrintMes(RGY_LOG_DEBUG, _T("Loaded %s.\n"), NVENCODE_API_DLL);

    //m_pDeviceを初期化
    if (!check_if_nvcuda_dll_available()) {
        PrintMes(RGY_LOG_ERROR,
            FOR_AUO ? _T("CUDAが使用できないため、NVEncによるエンコードが行えません。(check_if_nvcuda_dll_available)\n") : _T("CUDA not available.\n"));
        return NV_ENC_ERR_UNSUPPORTED_DEVICE;
    }
    m_nDeviceId = inputParam->deviceID;
    //入力などにも渡すため、まずはインスタンスを作っておく必要がある
    m_pPerfMonitor = std::unique_ptr<CPerfMonitor>(new CPerfMonitor());
    return nvStatus;
}

NVENCSTATUS NVEncCore::NvEncEncodeFrame(EncodeBuffer *pEncodeBuffer, int id, uint64_t timestamp, uint64_t duration, int inputFrameId) {

    NV_ENC_PIC_PARAMS encPicParams;
    INIT_CONFIG(encPicParams, NV_ENC_PIC_PARAMS);

    if (m_dynamicRC.size() > 0) {
        int selectedIdx = DYNAMIC_PARAM_NOT_SELECTED;
        for (int i = 0; i < (int)m_dynamicRC.size(); i++) {
            if (m_dynamicRC[i].start <= id && id <= m_dynamicRC[i].end) {
                selectedIdx = i;
            }
            if (m_dynamicRC[i].start > id) {
                break;
            }
        }
        if (m_appliedDynamicRC != selectedIdx) {
            NV_ENC_CONFIG encConfig = m_stEncConfig; //エンコード設定
            NV_ENC_RECONFIGURE_PARAMS reconf_params = { 0 };
            reconf_params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
            reconf_params.resetEncoder = 1;
            reconf_params.forceIDR = 1;
            reconf_params.reInitEncodeParams = m_stCreateEncodeParams;
            reconf_params.reInitEncodeParams.encodeConfig = &encConfig;
            if (selectedIdx >= 0) {
                const auto &selectedPrms = m_dynamicRC[selectedIdx];
                encConfig.rcParams.rateControlMode = selectedPrms.rc_mode;
                if (encConfig.rcParams.rateControlMode == NV_ENC_PARAMS_RC_CONSTQP) {
                    encConfig.rcParams.constQP = selectedPrms.qp;
                } else {
                    encConfig.rcParams.averageBitRate = selectedPrms.avg_bitrate;
                    if (selectedPrms.targetQuality >= 0) {
                        encConfig.rcParams.targetQuality    = (uint8_t)selectedPrms.targetQuality;
                        encConfig.rcParams.targetQualityLSB = (uint8_t)selectedPrms.targetQualityLSB;
                    }
                }
                if (selectedPrms.max_bitrate > 0) {
                    encConfig.rcParams.maxBitRate = selectedPrms.max_bitrate;
                }
            }
            NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncReconfigureEncoder(m_hEncoder, &reconf_params);
            if (nvStatus != NV_ENC_SUCCESS) {
                PrintMes(RGY_LOG_ERROR, _T("Failed to reconfigure the encoder.\n"));
                return nvStatus;
            }
            m_appliedDynamicRC = selectedIdx;
        }
    }

#if ENABLE_AVSW_READER
    if (m_Chapters.size() > 0 && m_keyOnChapter) {
        for (const auto& chap : m_Chapters) {
            //av_cmopare_tsを使うと、timebaseが粗く端数が出る場合に厳密に比較できないことがある
            //そこで、ここでは、最小公倍数をとって厳密な比較を行う
            const auto timebase_lcm = rgy_lcm<int64_t>(chap->time_base.den, m_outputTimebase.d());
            ttint128 ts_frame = timestamp;
            ts_frame *= m_outputTimebase.n();
            ts_frame *= timebase_lcm / m_outputTimebase.d();

            ttint128 ts_chap = chap->start;
            ts_chap *= chap->time_base.num;
            ts_chap *= timebase_lcm / chap->time_base.den;

            if (chap->id >= 0 && ts_chap <= ts_frame) {
                PrintMes(RGY_LOG_DEBUG, _T("Insert Keyframe on chapter %d: %s at frame #%s: %s (timebase: %lld).\n"),
                    chap->id,
                    wstring_to_tstring(ts_chap.ToWString()).c_str(),
                    id,
                    wstring_to_tstring(ts_frame.ToWString()).c_str(),
                    timebase_lcm);
                chap->id = -1;
                encPicParams.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
                break;
            }
        }
    }
    if (std::find(m_keyFile.begin(), m_keyFile.end(), id) != m_keyFile.end()) {
        PrintMes(RGY_LOG_DEBUG, _T("Insert Keyframe on frame #%d.\n"), id);
        encPicParams.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
    }
#endif //#if ENABLE_AVSW_READER
    vector<NV_ENC_SEI_PAYLOAD> sei_payload;
    vector<uint8_t> dhdr10plus_sei;
    const int codec = get_value_from_guid(m_stCodecGUID, list_nvenc_codecs);
    if (codec == NV_ENC_HEVC && m_hdr10plus) {
        const auto data = m_hdr10plus->getData(inputFrameId);
        if (data && data->size() > 0) {
            dhdr10plus_sei = *data;

            NV_ENC_SEI_PAYLOAD payload;
            payload.payload = dhdr10plus_sei.data();
            payload.payloadSize = (uint32_t)dhdr10plus_sei.size();
            payload.payloadType = USER_DATA_REGISTERED_ITU_T_T35;
            sei_payload.push_back(payload);

            encPicParams.codecPicParams.hevcPicParams.seiPayloadArrayCnt = (uint32_t)sei_payload.size();
            encPicParams.codecPicParams.hevcPicParams.seiPayloadArray = sei_payload.data();
        }
    }

    encPicParams.inputBuffer = pEncodeBuffer->stInputBfr.hInputSurface;
    encPicParams.bufferFmt = pEncodeBuffer->stInputBfr.bufferFmt;
    encPicParams.inputWidth = m_uEncWidth;
    encPicParams.inputHeight = m_uEncHeight;
    encPicParams.inputPitch = pEncodeBuffer->stInputBfr.uNV12Stride;
    encPicParams.outputBitstream = pEncodeBuffer->stOutputBfr.hBitstreamBuffer;
    encPicParams.completionEvent = pEncodeBuffer->stOutputBfr.hOutputEvent;
    encPicParams.inputTimeStamp = timestamp;
    encPicParams.inputDuration = duration;
    encPicParams.pictureStruct = m_stPicStruct;
    //encPicParams.qpDeltaMap = qpDeltaMapArray;
    //encPicParams.qpDeltaMapSize = qpDeltaMapArraySize;

    //if (encPicCommand)
    //{
    //    if (encPicCommand->bForceIDR)
    //    {
    //        encPicParams.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
    //    }

    //    if (encPicCommand->bForceIntraRefresh)
    //    {
    //        if (codecGUID == NV_ENC_CODEC_HEVC_GUID)
    //        {
    //            encPicParams.codecPicParams.hevcPicParams.forceIntraRefreshWithFrameCnt = encPicCommand->intraRefreshDuration;
    //        }
    //        else
    //        {
    //            encPicParams.codecPicParams.h264PicParams.forceIntraRefreshWithFrameCnt = encPicCommand->intraRefreshDuration;
    //        }
    //    }
    //}

    NVENCSTATUS nvStatus = m_pEncodeAPI->nvEncEncodePicture(m_hEncoder, &encPicParams);
    if (nvStatus != NV_ENC_SUCCESS && nvStatus != NV_ENC_ERR_NEED_MORE_INPUT) {
        PrintMes(RGY_LOG_ERROR, FOR_AUO ? _T("フレームの投入に失敗しました。\n") : _T("Failed to add frame into the encoder.\n"));
        return nvStatus;
    }

    return NV_ENC_SUCCESS;
}

#if 1
NVENCSTATUS NVEncCore::Encode() {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
    const uint32_t nPipelineDepth = PIPELINE_DEPTH;
    m_pStatus->SetStart();

    const int nEventCount = nPipelineDepth + CHECK_PTS_MAX_INSERT_FRAMES + 1 + MAX_FILTER_OUTPUT;

    const int cudaEventFlags = (m_cudaSchedule & CU_CTX_SCHED_BLOCKING_SYNC) ? cudaEventBlockingSync : cudaEventDefault;

    //vpp-afsのrffが使用されているか
    const bool vpp_afs_rff_aware = VppAfsRffAware();

    //vpp-rffが使用されているか
    const bool vpp_rff = VppRffEnabled();

    //エンコードを開始してもよいかを示すcueventの入れ物
    //FrameBufferDataEncに関連付けて使用する
    vector<unique_ptr<cudaEvent_t, cudaevent_deleter>> vEncStartEvents(nEventCount);
    for (uint32_t i = 0; i < vEncStartEvents.size(); i++) {
        //ctxlockした状態でcudaEventCreateを行わないと、イベントは正常に動作しない
        NVEncCtxAutoLock(ctxlock(m_ctxLock));
        vEncStartEvents[i] = std::unique_ptr<cudaEvent_t, cudaevent_deleter>(new cudaEvent_t(), cudaevent_deleter());
        auto cudaret = cudaEventCreateWithFlags(vEncStartEvents[i].get(), cudaEventFlags | cudaEventDisableTiming);
        if (cudaret != CUDA_SUCCESS) {
            PrintMes(RGY_LOG_ERROR, _T("Error cudaEventCreate: %d (%s).\n"), cudaret, char_to_tstring(_cudaGetErrorEnum(cudaret)).c_str());
            return NV_ENC_ERR_GENERIC;
        }
    }

    //入力フレームの転送管理用のイベント FrameTransferDataで使用する
    vector<unique_ptr<cudaEvent_t, cudaevent_deleter>> vInFrameTransferFin(std::max(nPipelineDepth, (uint32_t)m_inputHostBuffer.size()));
    for (uint32_t i = 0; i < vInFrameTransferFin.size(); i++) {
        //ctxlockした状態でcudaEventCreateを行わないと、イベントは正常に動作しない
        NVEncCtxAutoLock(ctxlock(m_ctxLock));
        vInFrameTransferFin[i] = std::unique_ptr<cudaEvent_t, cudaevent_deleter>(new cudaEvent_t(), cudaevent_deleter());
        auto cudaret = cudaEventCreateWithFlags(vInFrameTransferFin[i].get(), cudaEventFlags | cudaEventDisableTiming);
        if (cudaret != CUDA_SUCCESS) {
            PrintMes(RGY_LOG_ERROR, _T("Error cudaEventCreate: %d (%s).\n"), cudaret, char_to_tstring(_cudaGetErrorEnum(cudaret)).c_str());
            return NV_ENC_ERR_GENERIC;
        }
    }

    //入力フレームの最初の転送に関連するリソースを管理するデータ構造
    struct FrameTransferData {
        cudaEvent_t *eventFin; //転送終了後セットされるイベント (vInFrameTransferFinのものを使用)
        unique_ptr<FrameBufferDataIn> frameData; //入力フレームデータ
        shared_ptr<void> deviceFrame; //入力フレーム(cuvidのmap->unmap用)
    };
    //入力フレームの最初の転送に関連するリソースを管理するキュー
    //転送中のフレームに関するデータを積んでおく
    //転送を行うたびに、キューにeventとともにデータが追加される
    //転送が終了するとeventFinがセットされるので、キューからデータを削除する
    //shared_ptrなどの参照が0になればデストラクタが呼ばれ、リソースが自動的に解放される
    deque<FrameTransferData> dqFrameTransferData;

    //eventFinのセットを待って、キューからデータを削除する
    //nPipelineDepth以上キューに積まれていたら、eventのセットを強制的に待機する
    auto check_inframe_transfer = [&dqFrameTransferData](const uint32_t nPipelineDepth) {
        const auto queueLength = dqFrameTransferData.size();
        cudaError_t cuerr = cudaSuccess;
        if (queueLength > 0) {
            auto cuevent = *dqFrameTransferData.front().eventFin;
            if (cudaSuccess == (cuerr = (queueLength >= nPipelineDepth) ? cudaEventSynchronize(cuevent) : cudaEventQuery(cuevent))) {
                dqFrameTransferData.pop_front();
            }
            if (cuerr == cudaErrorNotReady) {
                //queueLength < nPipelineDepthならcudaErrorNotReadyがあり得る
                cuerr = cudaSuccess;
            }
        }
        return cuerr;
    };

#if ENABLE_AVSW_READER
    const AVStream *pStreamIn = nullptr;
    RGYInputAvcodec *pReader = dynamic_cast<RGYInputAvcodec *>(m_pFileReader.get());
    if (pReader != nullptr) {
        pStreamIn = pReader->GetInputVideoStream();
    }
    //cuvidデコード時は、timebaseの分子はかならず1
    const auto srcTimebase = (pStreamIn) ? rgy_rational<int>((m_cuvidDec) ? 1 : pStreamIn->time_base.num, pStreamIn->time_base.den) : rgy_rational<int>();

    //streamのindexから必要なwriteへのポインタを返すテーブルを作成
    std::map<int, shared_ptr<RGYOutputAvcodec>> pWriterForAudioStreams;
    for (auto pWriter : m_pFileWriterListAudio) {
        auto pAVCodecWriter = std::dynamic_pointer_cast<RGYOutputAvcodec>(pWriter);
        if (pAVCodecWriter) {
            auto trackIdList = pAVCodecWriter->GetStreamTrackIdList();
            for (auto trackID : trackIdList) {
                pWriterForAudioStreams[trackID] = pAVCodecWriter;
            }
        }
    }
    //streamのtrackIdからパケットを送信するvppフィルタへのポインタを返すテーブルを作成
    std::map<int, NVEncFilter*> pFilterForStreams;
    for (uint32_t ifilter = 0; ifilter < m_vpFilters.size(); ifilter++) {
        const auto targetTrackId = m_vpFilters[ifilter]->targetTrackIdx();
        if (targetTrackId != 0) {
            pFilterForStreams[targetTrackId] = m_vpFilters[ifilter].get();
        }
    }

    auto extract_audio = [&]() {
        auto sts = RGY_ERR_NONE;
        if ((m_pFileWriterListAudio.size() + pFilterForStreams.size()) > 0) {
            auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
            vector<AVPacket> packetList;
            if (pAVCodecReader != nullptr) {
                packetList = pAVCodecReader->GetStreamDataPackets();
            }
            //音声ファイルリーダーからのトラックを結合する
            for (const auto& reader : m_AudioReaders) {
                auto pReader = std::dynamic_pointer_cast<RGYInputAvcodec>(reader);
                if (pReader != nullptr) {
                    vector_cat(packetList, pReader->GetStreamDataPackets());
                }
            }
            //パケットを各Writerに分配する
            for (uint32_t i = 0; i < packetList.size(); i++) {
                const int nTrackId = (int)((uint32_t)packetList[i].flags >> 16);
                if (pWriterForAudioStreams.count(nTrackId)) {
                    auto pWriter = pWriterForAudioStreams[nTrackId];
                    if (pWriter == nullptr) {
                        PrintMes(RGY_LOG_ERROR, _T("Invalid writer found for audio track %d\n"), nTrackId);
                        return RGY_ERR_NOT_FOUND;
                    }
                    if ((sts = pWriter->WriteNextPacket(&packetList[i])) != RGY_ERR_NONE) {
                        return sts;
                    }
                } else if (pFilterForStreams.count(nTrackId)) {
                    if ((sts = pFilterForStreams[nTrackId]->addStreamPacket(&packetList[i])) != RGY_ERR_NONE) {
                        return sts;
                    }
                } else {
                    PrintMes(RGY_LOG_ERROR, _T("Failed to find writer for audio track %d\n"), nTrackId);
                    return RGY_ERR_NOT_FOUND;
                }
            }
        }
        return RGY_ERR_NONE;
    };

    std::thread th_input;
    if (m_cuvidDec) {
        th_input = std::thread([this, pStreamIn, &nvStatus]() {
            CUresult curesult = CUDA_SUCCESS;
            RGYBitstream bitstream = RGYBitstreamInit();
            RGY_ERR sts = RGY_ERR_NONE;
            for (int i = 0; sts == RGY_ERR_NONE && nvStatus == NV_ENC_SUCCESS && !m_cuvidDec->GetError(); i++) {
                sts = m_pFileReader->LoadNextFrame(nullptr);
                m_pFileReader->GetNextBitstream(&bitstream);
                PrintMes(RGY_LOG_TRACE, _T("Set packet %d\n"), i);
                if (CUDA_SUCCESS != (curesult = m_cuvidDec->DecodePacket(bitstream.bufptr() + bitstream.offset(), bitstream.size(), bitstream.pts(), pStreamIn->time_base))) {
                    PrintMes(RGY_LOG_ERROR, _T("Error in DecodePacket: %d (%s).\n"), curesult, char_to_tstring(_cudaGetErrorEnum(curesult)).c_str());
                    return curesult;
                }
                bitstream.setSize(0);
                bitstream.setOffset(0);
            }
            if (CUDA_SUCCESS != (curesult = m_cuvidDec->DecodePacket(nullptr, 0, AV_NOPTS_VALUE, pStreamIn->time_base))) {
                PrintMes(RGY_LOG_ERROR, _T("Error in DecodePacketFin: %d (%s).\n"), curesult, char_to_tstring(_cudaGetErrorEnum(curesult)).c_str());
            }
            return curesult;
        });
        PrintMes(RGY_LOG_DEBUG, _T("Started Encode thread\n"));
    }

    if (m_pPerfMonitor) {
        HANDLE thOutput = NULL;
        HANDLE thInput = NULL;
        HANDLE thAudProc = NULL;
        HANDLE thAudEnc = NULL;
        auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
        if (pAVCodecReader != nullptr) {
            thInput = pAVCodecReader->getThreadHandleInput();
        }
        auto pAVCodecWriter = std::dynamic_pointer_cast<RGYOutputAvcodec>(m_pFileWriter);
        if (pAVCodecWriter != nullptr) {
            thOutput = pAVCodecWriter->getThreadHandleOutput();
            thAudProc = pAVCodecWriter->getThreadHandleAudProcess();
            thAudEnc = pAVCodecWriter->getThreadHandleAudEncode();
        }
        m_pPerfMonitor->SetThreadHandles((HANDLE)(th_input.native_handle()), thInput, thOutput, thAudProc, thAudEnc);
    }
    int64_t nOutFirstPts = AV_NOPTS_VALUE; //入力のptsに対する補正 (スケール: m_outputTimebase)
#endif //#if ENABLE_AVSW_READER
    int64_t lastTrimFramePts = AV_NOPTS_VALUE; //直前のtrimで落とされたフレームのpts, trimで落とされてない場合はAV_NOPTS_VALUE (スケール: m_outputTimebase)
    int64_t nOutEstimatedPts = 0; //固定fpsを仮定した時のfps (スケール: m_outputTimebase)
    const int64_t nOutFrameDuration = std::max<int64_t>(1, rational_rescale(1, m_inputFps.inv(), m_outputTimebase)); //固定fpsを仮定した時の1フレームのduration (スケール: m_outputTimebase)

    int dec_vpp_rff_sts = 0; //rffの展開状態を示すフラグ

    auto add_dec_vpp_param = [&](FrameBufferDataIn *pInputFrame, vector<unique_ptr<FrameBufferDataIn>>& vppParams, int64_t outPts, int64_t outDuration) {
        if (pInputFrame->inputIsHost()) {
            pInputFrame->setTimeStamp(outPts);
            pInputFrame->setDuration(outDuration);
            vppParams.push_back(std::move(unique_ptr<FrameBufferDataIn>(new FrameBufferDataIn(*pInputFrame))));
        }
#if ENABLE_AVSW_READER
        else {
            auto deint = m_cuvidDec->getDeinterlaceMode();
            auto frameinfo = pInputFrame->getFrameInfo();
            frameinfo.timestamp = outPts;
            frameinfo.duration = outDuration;
            CUVIDPROCPARAMS oVPP = { 0 };
            oVPP.top_field_first = pInputFrame->getCuvidInfo()->top_field_first;
            switch (deint) {
            case cudaVideoDeinterlaceMode_Weave:
                oVPP.progressive_frame = pInputFrame->getCuvidInfo()->progressive_frame;
                oVPP.unpaired_field = 0;// oVPP.progressive_frame;
                if (vpp_rff) {
                    //rffを展開する場合、時間を補正する
                    if (frameinfo.flags & RGY_FRAME_FLAG_RFF) {
                        frameinfo.duration = (frameinfo.duration * 2) / 3;
                    }
                    if (dec_vpp_rff_sts) { //rff展開中の場合、ptsを1フィールド戻す
                        frameinfo.timestamp -= frameinfo.duration / 2;
                    }
                }
                vppParams.push_back(std::move(unique_ptr<FrameBufferDataIn>(new FrameBufferDataIn(pInputFrame->getCuvidInfo(), oVPP, frameinfo))));
                //PrintMes(RGY_LOG_INFO, _T("pts: %lld, duration %lld, progressive:%d, rff:%d\n"), (lls)frameinfo.timestamp, (lls)frameinfo.duration, oVPP.progressive_frame, (frameinfo.flags & RGY_FRAME_FLAG_RFF) ? 1 : 0);

                if (vpp_rff && (frameinfo.flags & RGY_FRAME_FLAG_RFF)) {
                    if (dec_vpp_rff_sts) {
                        frameinfo.flags |= RGY_FRAME_FLAG_RFF_COPY;
                        //rffを展開する場合、時間を補正する
                        frameinfo.timestamp += frameinfo.duration;
                        vppParams.push_back(std::move(unique_ptr<FrameBufferDataIn>(new FrameBufferDataIn(pInputFrame->getCuvidInfo(), oVPP, frameinfo))));
                        //PrintMes(RGY_LOG_INFO, _T("pts: %lld, duration %lld\n"), (lls)frameinfo.timestamp, (lls)frameinfo.duration);
                    }
                    dec_vpp_rff_sts ^= 1; //反転
                }
                break;
            case cudaVideoDeinterlaceMode_Bob:
                //RFFに関するフラグを念のためクリア
                frameinfo.flags &= (~(RGY_FRAME_FLAG_RFF | RGY_FRAME_FLAG_RFF_COPY | RGY_FRAME_FLAG_RFF_TFF | RGY_FRAME_FLAG_RFF_BFF));
                pInputFrame->setInterlaceFlag(RGY_PICSTRUCT_FRAME);
                oVPP.progressive_frame = 0;
                oVPP.second_field = 0;
                frameinfo.duration >>= 1;
                vppParams.push_back(std::move(unique_ptr<FrameBufferDataIn>(new FrameBufferDataIn(pInputFrame->getCuvidInfo(), oVPP, frameinfo))));
                oVPP.second_field = 1;
                frameinfo.timestamp += frameinfo.duration;
                vppParams.push_back(std::move(unique_ptr<FrameBufferDataIn>(new FrameBufferDataIn(pInputFrame->getCuvidInfo(), oVPP, frameinfo))));
                break;
            case cudaVideoDeinterlaceMode_Adaptive:
                //RFFに関するフラグを念のためクリア
                frameinfo.flags &= (~(RGY_FRAME_FLAG_RFF | RGY_FRAME_FLAG_RFF_COPY | RGY_FRAME_FLAG_RFF_TFF | RGY_FRAME_FLAG_RFF_BFF));
                pInputFrame->setInterlaceFlag(RGY_PICSTRUCT_FRAME);
                oVPP.progressive_frame = 0;
                vppParams.push_back(std::move(unique_ptr<FrameBufferDataIn>(new FrameBufferDataIn(pInputFrame->getCuvidInfo(), oVPP, frameinfo))));
                break;
            default:
                PrintMes(RGY_LOG_ERROR, _T("Unknown Deinterlace mode\n"));
                break;
            }
        }
#endif //#if ENABLE_AVSW_READER
        return;
    };

    uint32_t nInputFramePosIdx = UINT32_MAX;
    auto check_pts = [&](FrameBufferDataIn *pInputFrame) {
        vector<unique_ptr<FrameBufferDataIn>> decFrames;
        int64_t outPtsSource = nOutEstimatedPts;
        int64_t outDuration = nOutFrameDuration; //入力fpsに従ったduration
#if ENABLE_AVSW_READER
        if (pStreamIn && ((m_nAVSyncMode & (RGY_AVSYNC_VFR | RGY_AVSYNC_FORCE_CFR)) || vpp_rff || vpp_afs_rff_aware)) {
            //CFR仮定ではなく、オリジナルの時間を見る
            outPtsSource = (pStreamIn) ? rational_rescale(pInputFrame->getTimeStamp(), srcTimebase, m_outputTimebase) : nOutEstimatedPts;
        }
        if (nOutFirstPts == AV_NOPTS_VALUE) {
            nOutFirstPts = outPtsSource; //最初のpts
        }
        //最初のptsを0に修正
        outPtsSource -= nOutFirstPts;

        if (pStreamIn
            && ((m_nAVSyncMode & RGY_AVSYNC_VFR) || vpp_rff || vpp_afs_rff_aware)) {
            if (vpp_rff || vpp_afs_rff_aware) {
                if (std::abs(outPtsSource - nOutEstimatedPts) >= 32 * nOutFrameDuration) {
                    //timestampに一定以上の差があればそれを無視する
                    nOutFirstPts += (outPtsSource - nOutEstimatedPts); //今後の位置合わせのための補正
                    outPtsSource = nOutEstimatedPts;
                }
                auto ptsDiff = outPtsSource - nOutEstimatedPts;
                if (ptsDiff <= std::min<int64_t>(-1, -1 * nOutFrameDuration * 7 / 8)) {
                    //間引きが必要
                    return decFrames;
                }
            }
            //cuvidデコード時は、timebaseの分子はかならず1なので、pStreamIn->time_baseとズレているかもしれないのでオリジナルを計算
            const auto orig_pts = rational_rescale(pInputFrame->getTimeStamp(), srcTimebase, to_rgy(pStreamIn->time_base));
            //ptsからフレーム情報を取得する
            const auto framePos = pReader->GetFramePosList()->findpts(orig_pts, &nInputFramePosIdx);
            if (framePos.poc != FRAMEPOS_POC_INVALID && framePos.duration > 0) {
                //有効な値ならオリジナルのdurationを使用する
                outDuration = rational_rescale(framePos.duration, to_rgy(pStreamIn->time_base), m_outputTimebase);
            }
        }
        if (m_nAVSyncMode & RGY_AVSYNC_FORCE_CFR) {
            if (std::abs(outPtsSource - nOutEstimatedPts) >= CHECK_PTS_MAX_INSERT_FRAMES * nOutFrameDuration) {
                //timestampに一定以上の差があればそれを無視する
                nOutFirstPts += (outPtsSource - nOutEstimatedPts); //今後の位置合わせのための補正
                outPtsSource = nOutEstimatedPts;
                PrintMes(RGY_LOG_WARN, _T("Big Gap was found between 2 frames, avsync might be corrupted.\n"));
            }
            auto ptsDiff = outPtsSource - nOutEstimatedPts;
            if (ptsDiff <= std::min<int64_t>(-1, -1 * nOutFrameDuration * 7 / 8)) {
                //間引きが必要
                return decFrames;
            }
            while (ptsDiff >= std::max<int64_t>(1, nOutFrameDuration * 7 / 8)) {
                //水増しが必要
                add_dec_vpp_param(pInputFrame, decFrames, nOutEstimatedPts, outDuration);
                nOutEstimatedPts += nOutFrameDuration;
                ptsDiff = outPtsSource - nOutEstimatedPts;
            }
            outPtsSource = nOutEstimatedPts;
        }
#endif //#if ENABLE_AVSW_READER
        //次のフレームのptsの予想
        nOutEstimatedPts += outDuration;
        add_dec_vpp_param(pInputFrame, decFrames, outPtsSource, outDuration);
        return std::move(decFrames);
    };

    auto add_frame_transfer_data = [&](cudaEvent_t *pCudaEvent, unique_ptr<FrameBufferDataIn>& inframe, shared_ptr<void>& deviceFrame) {
        //最初のフィルタ(転送)が終了したことを示すイベント
        //ctxlockのロック外で行う必要があるよう
        auto cudaret = cudaEventRecord(*pCudaEvent);
        if (cudaret != cudaSuccess) {
            PrintMes(RGY_LOG_ERROR, _T("Error cudaEventRecord [add_frame_transfer_data]: %d (%s).\n"), cudaret, char_to_tstring(_cudaGetErrorEnum(cudaret)).c_str());
            return NV_ENC_ERR_GENERIC;
        }

        //関連するリソースに関する参照を移して、キューに積む
        FrameTransferData transferData;
        transferData.eventFin = pCudaEvent; //転送終了のイベント
        transferData.frameData = std::move(inframe); //入力バッファへの参照
        transferData.deviceFrame = std::move(deviceFrame); //cuvidのmap->unmap
        dqFrameTransferData.push_back(std::move(transferData));
        return NV_ENC_SUCCESS;
    };

    auto filter_frame = [&](int& nFilterFrame, unique_ptr<FrameBufferDataIn>& inframe, deque<unique_ptr<FrameBufferDataEnc>>& dqEncFrames, bool& bDrain) {
        cudaMemcpyKind memcpyKind = cudaMemcpyDeviceToDevice;
        FrameInfo frameInfo = { 0 };
        shared_ptr<void> deviceFrame;
        if (!bDrain) {
            if (inframe->inputIsHost()) {
                memcpyKind = cudaMemcpyHostToDevice;
                frameInfo = inframe->getFrameInfo();
                deviceFrame = shared_ptr<void>(frameInfo.ptr, [&](void *ptr) {
                    ptr = ptr;
                    //このメモリはm_inputHostBufferのメモリであり、使いまわすため、解放しない
                });
            }
#if ENABLE_AVSW_READER
            else {
                //ここで前のフレームの転送を必ず待機する
                //ここで待機しないと、vpp-deinterlace bobのときに同一フレームに対する
                //cuvidMapVideoFrameが2連続で発生してしまい、エラーになってしまう
                auto cuerr = check_inframe_transfer(1);
                if (cuerr != cudaSuccess) {
                    PrintMes(RGY_LOG_ERROR, _T("Error cudaEventSynchronize: %d (%s).\n"), cuerr, char_to_tstring(_cudaGetErrorEnum(cuerr)).c_str());
                    return NV_ENC_ERR_GENERIC;
                }
                CUresult curesult = CUDA_SUCCESS;
                CUdeviceptr dMappedFrame = 0;
                memcpyKind = cudaMemcpyDeviceToDevice;
                auto vppinfo = inframe->getVppInfo();
                uint32_t pitch = 0;
                if (CUDA_SUCCESS != (curesult = cuvidMapVideoFrame(m_cuvidDec->GetDecoder(), inframe->getCuvidInfo()->picture_index, &dMappedFrame, &pitch, &vppinfo))) {
                    PrintMes(RGY_LOG_ERROR, _T("Error cuvidMapVideoFrame: %d (%s).\n"), curesult, char_to_tstring(_cudaGetErrorEnum(curesult)).c_str());
                    return NV_ENC_ERR_GENERIC;
                }
                frameInfo = inframe->getFrameInfo();
                frameInfo.pitch = pitch;
                frameInfo.ptr = (uint8_t *)dMappedFrame;
                deviceFrame = shared_ptr<void>(frameInfo.ptr, [&](void *ptr) {
                    cuvidUnmapVideoFrame(m_cuvidDec->GetDecoder(), (CUdeviceptr)ptr);
                });
            }
#endif //#if ENABLE_AVSW_READER
        }

        deque<std::pair<FrameInfo, uint32_t>> filterframes;
        filterframes.push_back(std::make_pair(frameInfo, 0u));

        while (filterframes.size() > 0 || bDrain) {
            //フィルタリングするならここ
            for (uint32_t ifilter = filterframes.front().second; ifilter < m_vpFilters.size() - 1; ifilter++) {
                NVEncCtxAutoLock(ctxlock(m_ctxLock));
                int nOutFrames = 0;
                FrameInfo *outInfo[16] = { 0 };
                auto sts_filter = m_vpFilters[ifilter]->filter(&filterframes.front().first, (FrameInfo **)&outInfo, &nOutFrames);
                if (sts_filter != RGY_ERR_NONE) {
                    PrintMes(RGY_LOG_ERROR, _T("Error while running filter \"%s\".\n"), m_vpFilters[ifilter]->name().c_str());
                    return NV_ENC_ERR_GENERIC;
                }
                if (nOutFrames == 0) {
                    if (bDrain) {
                        filterframes.front().second++;
                        continue;
                    }
                    return NV_ENC_SUCCESS;
                }
                filterframes.pop_front();
                if (ifilter == 0) { //最初のフィルタなら転送なので、イベントをここでセットする
                    auto pCudaEvent = vInFrameTransferFin[nFilterFrame % vInFrameTransferFin.size()].get();
                    add_frame_transfer_data(pCudaEvent, inframe, deviceFrame);
                }
                bDrain = false; //途中でフレームが出てきたら、drain完了していない

                //最初に出てきたフレームは先頭に追加する
                for (int jframe = nOutFrames-1; jframe >= 0; jframe--) {
                    filterframes.push_front(std::make_pair(*outInfo[jframe], ifilter+1));
                }
            }
            if (bDrain) {
                return NV_ENC_SUCCESS; //最後までbDrain = trueなら、drain完了
            }

            //エンコードバッファを取得
            EncodeBuffer *pEncodeBuffer = m_EncodeBufferQueue.GetAvailable();
            if (!pEncodeBuffer) {
                pEncodeBuffer = m_EncodeBufferQueue.GetPending();
                ProcessOutput(pEncodeBuffer);
                PrintMes(RGY_LOG_TRACE, _T("Output frame %d\n"), m_pStatus->m_sData.frameOut);
                if (pEncodeBuffer->stInputBfr.pNV12devPtr) {
                    if (pEncodeBuffer->stInputBfr.hInputSurface) {
                        auto nvencret = NvEncUnmapInputResource(pEncodeBuffer->stInputBfr.hInputSurface);
                        if (nvencret != NV_ENC_SUCCESS) {
                            PrintMes(RGY_LOG_ERROR, _T("Failed to Unmap input buffer %p: %s\n"), pEncodeBuffer->stInputBfr.hInputSurface, char_to_tstring(_nvencGetErrorEnum(nvencret)).c_str());
                            return nvencret;
                        }
                        pEncodeBuffer->stInputBfr.hInputSurface = nullptr;
                    }
                }
                pEncodeBuffer = m_EncodeBufferQueue.GetAvailable();
                if (!pEncodeBuffer) {
                    PrintMes(RGY_LOG_ERROR, _T("Error get enc buffer from queue.\n"));
                    return NV_ENC_ERR_GENERIC;
                }
            }
            //エンコードバッファにコピー
            //エンコード直前のバッファまで転送を完了し、そのフレームのエンコードを開始できることを示すイベント
            //ここでnFilterFrameをインクリメントする
            {
                NVEncCtxAutoLock(ctxlock(m_ctxLock));
                auto& lastFilter = m_vpFilters[m_vpFilters.size()-1];
                //最後のフィルタはNVEncFilterCspCropでなければならない
                if (typeid(*lastFilter.get()) != typeid(NVEncFilterCspCrop)) {
                    PrintMes(RGY_LOG_ERROR, _T("Last filter setting invalid.\n"));
                    return NV_ENC_ERR_GENERIC;
                }
                int nOutFrames = 0;
                FrameInfo *outInfo[16] = { 0 };
                //エンコードバッファの情報を設定１
                FrameInfo encFrameInfo ={ 0 };
                if (pEncodeBuffer->stInputBfr.pNV12devPtr) {
                    encFrameInfo.ptr = (uint8_t *)pEncodeBuffer->stInputBfr.pNV12devPtr;
                    encFrameInfo.pitch = pEncodeBuffer->stInputBfr.uNV12Stride;
                    encFrameInfo.width = pEncodeBuffer->stInputBfr.dwWidth;
                    encFrameInfo.height = pEncodeBuffer->stInputBfr.dwHeight;
                    encFrameInfo.deivce_mem = true;
                    encFrameInfo.csp = getEncCsp(pEncodeBuffer->stInputBfr.bufferFmt);
                } else {
                    //インタレ保持の場合は、NvEncCreateInputBuffer経由でフレームを渡さないと正常にエンコードできない
                    uint32_t lockedPitch = 0;
                    unsigned char *pInputSurface = nullptr;
                    NvEncLockInputBuffer(pEncodeBuffer->stInputBfr.hInputSurface, (void**)&pInputSurface, &lockedPitch);
                    encFrameInfo.ptr = (uint8_t *)pInputSurface;
                    encFrameInfo.pitch = lockedPitch;
                    encFrameInfo.width = pEncodeBuffer->stInputBfr.dwWidth;
                    encFrameInfo.height = pEncodeBuffer->stInputBfr.dwHeight;
                    encFrameInfo.deivce_mem = false; //CPU側にフレームデータを戻す
                    encFrameInfo.csp = getEncCsp(pEncodeBuffer->stInputBfr.bufferFmt);
                }
                //エンコードバッファのポインタを渡す
                outInfo[0] = &encFrameInfo;
                auto sts_filter = lastFilter->filter(&filterframes.front().first, (FrameInfo **)&outInfo, &nOutFrames);
                filterframes.pop_front();
                if (sts_filter != RGY_ERR_NONE) {
                    PrintMes(RGY_LOG_ERROR, _T("Error while running filter \"%s\".\n"), lastFilter->name().c_str());
                    return NV_ENC_ERR_GENERIC;
                }
                auto pCudaEvent = vEncStartEvents[nFilterFrame++ % vEncStartEvents.size()].get();
                auto cudaret = cudaEventRecord(*pCudaEvent);
                if (cudaret != cudaSuccess) {
                    PrintMes(RGY_LOG_ERROR, _T("Error cudaEventRecord: %d (%s).\n"), cudaret, char_to_tstring(_cudaGetErrorEnum(cudaret)).c_str());
                    return NV_ENC_ERR_GENERIC;
                }
                if (m_vpFilters.size() == 1) {
                    //フィルタの数が1のときは、ここが最初(かつ最後)のフィルタであり、転送フィルタである
                    add_frame_transfer_data(pCudaEvent, inframe, deviceFrame);
                }
                unique_ptr<FrameBufferDataEnc> frameEnc(new FrameBufferDataEnc(RGY_CSP_NV12, encFrameInfo.timestamp, encFrameInfo.duration, encFrameInfo.inputFrameId, pEncodeBuffer, pCudaEvent));
                dqEncFrames.push_back(std::move(frameEnc));
            }
        }
        return NV_ENC_SUCCESS;
    };

    auto send_encoder = [&](int& nEncodeFrame, unique_ptr<FrameBufferDataEnc>& encFrame) {
        //エンコーダ用のバッファまで転送が終了するのを待機
        if (encFrame->m_pEvent) { cudaEventSynchronize(*encFrame->m_pEvent); }
        EncodeBuffer *pEncodeBuffer = encFrame->m_pEncodeBuffer;
        if (pEncodeBuffer->stInputBfr.pNV12devPtr) {
            auto nvencret = NvEncMapInputResource(pEncodeBuffer->stInputBfr.nvRegisteredResource, &pEncodeBuffer->stInputBfr.hInputSurface);
            if (nvencret != NV_ENC_SUCCESS) {
                PrintMes(RGY_LOG_ERROR, _T("Failed to Map input buffer %p\n"), pEncodeBuffer->stInputBfr.hInputSurface);
                return nvencret;
            }
        } else {
            NvEncUnlockInputBuffer(pEncodeBuffer->stInputBfr.hInputSurface);
        }
        return NvEncEncodeFrame(pEncodeBuffer, nEncodeFrame++, encFrame->m_timestamp, encFrame->m_duration, encFrame->m_inputFrameId);
    };

#define NV_ENC_ERR_ABORT ((NVENCSTATUS)-1)
    unique_ptr<FrameBufferDataIn> dummyFrame;
    CProcSpeedControl speedCtrl(m_nProcSpeedLimit);
    deque<unique_ptr<FrameBufferDataIn>> dqInFrames;
    deque<unique_ptr<FrameBufferDataEnc>> dqEncFrames;
    int nEncodeFrames = 0;
    bool bInputEmpty = false;
    bool bFilterEmpty = false;
    for (int nInputFrame = 0, nFilterFrame = 0; nvStatus == NV_ENC_SUCCESS && !bInputEmpty && !bFilterEmpty; ) {
        if (m_pAbortByUser && *m_pAbortByUser) {
            nvStatus = NV_ENC_ERR_ABORT;
            break;
        }
        speedCtrl.wait();
#if ENABLE_AVSW_READER
        if (0 != extract_audio()) {
            nvStatus = NV_ENC_ERR_GENERIC;
            break;
        }
#endif //#if ENABLE_AVSW_READER

        //転送の終了状況を確認、可能ならリソースの開放を行う
        auto cuerr = check_inframe_transfer(nPipelineDepth);
        if (cuerr != cudaSuccess) {
            PrintMes(RGY_LOG_ERROR, _T("Error cudaEventSynchronize: %d (%s).\n"), cuerr, char_to_tstring(_cudaGetErrorEnum(cuerr)).c_str());
            return NV_ENC_ERR_GENERIC;
        }

        //デコード
        FrameBufferDataIn inputFrame;
#if ENABLE_AVSW_READER
        if (m_cuvidDec) {
            if (m_cuvidDec->GetError()
                || (m_cuvidDec->frameQueue()->isEndOfDecode() && m_cuvidDec->frameQueue()->isEmpty())) {
                bInputEmpty = true;
            }
            if (!bInputEmpty) {
                CUVIDPARSERDISPINFO dispInfo = { 0 };
                if (!m_cuvidDec->frameQueue()->dequeue(&dispInfo)) {
                    //転送の終了状況を確認、可能ならリソースの開放を行う
                    cuerr = check_inframe_transfer(nPipelineDepth);
                    if (cuerr != cudaSuccess) {
                        PrintMes(RGY_LOG_ERROR, _T("Error cudaEventSynchronize: %d (%s).\n"), cuerr, char_to_tstring(_cudaGetErrorEnum(cuerr)).c_str());
                        return NV_ENC_ERR_GENERIC;
                    }
                    m_cuvidDec->frameQueue()->waitForQueueUpdate();
                    continue;
                }
                inputFrame.setCuvidInfo(shared_ptr<CUVIDPARSERDISPINFO>(new CUVIDPARSERDISPINFO(dispInfo), [&](CUVIDPARSERDISPINFO *ptr) {
                    m_cuvidDec->frameQueue()->releaseFrame(ptr);
                    delete ptr;
                }), m_cuvidDec->GetDecFrameInfo());
                inputFrame.setInputFrameId(nInputFrame);
            }
        } else
#endif //#if ENABLE_AVSW_READER
        if (m_inputHostBuffer.size()) {
            auto& inputFrameBuf = m_inputHostBuffer[nInputFrame % m_inputHostBuffer.size()];
            if (inputFrameBuf.heTransferFin) {
                //対象バッファの転送が終了しているかを確認
                while (WaitForSingleObject(inputFrameBuf.heTransferFin.get(), 0) == WAIT_TIMEOUT) {
                    cuerr = check_inframe_transfer(nPipelineDepth);
                    if (cuerr != cudaSuccess) {
                        PrintMes(RGY_LOG_ERROR, _T("Error cudaEventSynchronize: %d (%s).\n"), cuerr, char_to_tstring(_cudaGetErrorEnum(cuerr)).c_str());
                        return NV_ENC_ERR_GENERIC;
                    }
                }
            }
            NVTXRANGE(LoadNextFrame);
            RGYFrame frame = RGYFrameInit(inputFrameBuf.frameInfo);
            auto rgy_err = m_pFileReader->LoadNextFrame(&frame);
            if (rgy_err != RGY_ERR_NONE) {
                if (rgy_err != RGY_ERR_MORE_DATA) { //RGY_ERR_MORE_DATAは読み込みの正常終了を示す
                    nvStatus = err_to_nv(rgy_err);
                }
                bInputEmpty = true;
            }
            auto heTransferFin = shared_ptr<void>(inputFrameBuf.heTransferFin.get(), [&](void *ptr) {
                SetEvent((HANDLE)ptr);
            });
            inputFrame.setHostFrameInfo(frame.getInfo(), heTransferFin);
            inputFrame.setInputFrameId(nInputFrame);
        } else {
            PrintMes(RGY_LOG_ERROR, _T("Unexpected error at Encode().\n"));
            return NV_ENC_ERR_GENERIC;
        }

        if (!bInputEmpty) {
            //trim反映
            const auto trimSts = frame_inside_range(nInputFrame++, m_trimParam.list);
#if ENABLE_AVSW_READER
            const auto inputFramePts = rational_rescale(inputFrame.getTimeStamp(), srcTimebase, m_outputTimebase);
            if (((m_nAVSyncMode & RGY_AVSYNC_VFR) || vpp_rff || vpp_afs_rff_aware)
                && (trimSts.second > 0) //check_pts内で最初のフレームのptsを0とするようnOutFirstPtsが設定されるので、先頭のtrim blockについてはここでは処理しない
                && (lastTrimFramePts != AV_NOPTS_VALUE)) { //前のフレームがtrimで脱落させたフレームなら
                nOutFirstPts += inputFramePts - lastTrimFramePts; //trimで脱落させたフレームの分の時間を加算
            }
            if (!trimSts.first) {
                lastTrimFramePts = inputFramePts; //脱落させたフレームの時間を記憶
            }
#endif
            if (!trimSts.first) {
                continue; //trimにより脱落させるフレーム
            }
            lastTrimFramePts = AV_NOPTS_VALUE;
            auto decFrames = check_pts(&inputFrame);

            for (auto idf = decFrames.begin(); idf != decFrames.end(); idf++) {
                dqInFrames.push_back(std::move(*idf));
            }
        }
        inputFrame.resetCuvidInfo();

        while (((dqInFrames.size() || bInputEmpty) && !bFilterEmpty) && nvStatus == NV_ENC_SUCCESS) {
            const bool bDrain = (dqInFrames.size()) ? false : bInputEmpty;
            auto& inframe = (dqInFrames.size()) ? dqInFrames.front() : dummyFrame;
            bool bDrainFin = bDrain;
            if (NV_ENC_SUCCESS != (nvStatus = filter_frame(nFilterFrame, inframe, dqEncFrames, bDrainFin))) {
                break;
            }
            bFilterEmpty = bDrainFin;
            if (!bDrain) {
                dqInFrames.pop_front();
            }
            while (dqEncFrames.size() >= nPipelineDepth) {
                auto& encframe = dqEncFrames.front();
                if (NV_ENC_SUCCESS != (nvStatus = send_encoder(nEncodeFrames, encframe))) {
                    break;
                }
                dqEncFrames.pop_front();
            }
        }
    }
    //すべての転送を終了させる
    while (dqFrameTransferData.size()) {
        auto cuerr = check_inframe_transfer(1);
        if (cuerr != cudaSuccess) {
            PrintMes(RGY_LOG_ERROR, _T("Error cudaEventSynchronize: %d (%s).\n"), cuerr, char_to_tstring(_cudaGetErrorEnum(cuerr)).c_str());
            return NV_ENC_ERR_GENERIC;
        }
    }
    //エンコードバッファのフレームをすべて転送
    while (dqEncFrames.size()) {
        auto& encframe = dqEncFrames.front();
        auto nvStatusFlush = send_encoder(nEncodeFrames, encframe);
        if (nvStatusFlush != NV_ENC_SUCCESS) {
            nvStatus = nvStatusFlush;
            break;
        }
        dqEncFrames.pop_front();
    }

#if ENABLE_AVSW_READER
    if (th_input.joinable()) {
        //ここでフレームをすべて吐き出し切らないと、中断時にデコードスレッドが終了しない
        if (m_cuvidDec) {
            while (!m_cuvidDec->GetError()
                && !(m_cuvidDec->frameQueue()->isEndOfDecode() && m_cuvidDec->frameQueue()->isEmpty())) {
                CUVIDPARSERDISPINFO pInfo;
                if (m_cuvidDec->frameQueue()->dequeue(&pInfo)) {
                    m_cuvidDec->frameQueue()->releaseFrame(&pInfo);
                }
            }
        }
        th_input.join();
    }
    for (const auto& writer : m_pFileWriterListAudio) {
        auto pAVCodecWriter = std::dynamic_pointer_cast<RGYOutputAvcodec>(writer);
        if (pAVCodecWriter != nullptr) {
            //エンコーダなどにキャッシュされたパケットを書き出す
            pAVCodecWriter->WriteNextPacket(nullptr);
        }
    }
#endif //#if ENABLE_AVSW_READER
    PrintMes(RGY_LOG_INFO, _T("                                                                             \n"));
    //FlushEncoderはかならず行わないと、NvEncDestroyEncoderで異常終了する
    auto encstatus = nvStatus;
    if (nEncodeFrames > 0 || nvStatus == NV_ENC_SUCCESS) {
        encstatus = FlushEncoder();
        if (encstatus != NV_ENC_SUCCESS) {
            PrintMes(RGY_LOG_ERROR, _T("Error FlushEncoder: %d.\n"), encstatus);
            nvStatus = encstatus;
        } else {
            PrintMes(RGY_LOG_DEBUG, _T("Flushed Encoder\n"));
        }
    }
    m_pFileWriter->Close();
    m_pFileReader->Close();
    m_pStatus->WriteResults();
    vector<std::pair<tstring, double>> filter_result;
    for (auto& filter : m_vpFilters) {
        auto avgtime = filter->GetAvgTimeElapsed();
        if (avgtime > 0.0) {
            filter_result.push_back({ filter->name(), avgtime });
        }
    }
    if (filter_result.size()) {
        PrintMes(RGY_LOG_INFO, _T("\nVpp Filter Performance\n"));
        const auto max_len = std::accumulate(filter_result.begin(), filter_result.end(), 0u, [](uint32_t max_length, std::pair<tstring, double> info) {
            return std::max(max_length, (uint32_t)info.first.length());
        });
        for (const auto& info : filter_result) {
            tstring str = info.first + _T(":");
            for (uint32_t i = (uint32_t)info.first.length(); i < max_len; i++) {
                str += _T(" ");
            }
            PrintMes(RGY_LOG_INFO, _T("%s %7.1f us\n"), str.c_str(), info.second * 1000.0);
        }
    }
    return nvStatus;
}
#else
NVENCSTATUS NVEncCore::Encode() {
    NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
    if (m_cuvidDec || m_inputHostBuffer.size() > 0) {
        return Encode2();
    }

    m_pStatus->SetStart();

    int ret = 0;
    const int bufferCount = m_encodeBufferCount;
#if ENABLE_AVSW_READER
    const AVCodecContext *pVideoCtx = nullptr;
    RGYInputAvcodec *pReader = dynamic_cast<RGYInputAvcodec *>(m_pFileReader.get());
    if (pReader != nullptr) {
        pVideoCtx = pReader->GetInputVideoCodecCtx();
    }

    //streamのindexから必要なwriteへのポインタを返すテーブルを作成
    std::map<int, shared_ptr<RGYOutputAvcodec>> pWriterForAudioStreams;
    for (auto pWriter : m_pFileWriterListAudio) {
        auto pAVCodecWriter = std::dynamic_pointer_cast<RGYOutputAvcodec>(pWriter);
        if (pAVCodecWriter) {
            auto trackIdList = pAVCodecWriter->GetStreamTrackIdList();
            for (auto trackID : trackIdList) {
                pWriterForAudioStreams[trackID] = pAVCodecWriter;
            }
        }
    }

    auto extract_audio =[&]() {
        int sts = 0;
        if (m_pFileWriterListAudio.size()) {
            auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(m_pFileReader);
            vector<AVPacket> packetList;
            if (pAVCodecReader != nullptr) {
                packetList = pAVCodecReader->GetStreamDataPackets();
            }
            //音声ファイルリーダーからのトラックを結合する
            for (const auto& reader : m_AudioReaders) {
                auto pReader = std::dynamic_pointer_cast<RGYInputAvcodec>(reader);
                if (pReader != nullptr) {
                    vector_cat(packetList, pReader->GetStreamDataPackets());
                }
            }
            //パケットを各Writerに分配する
            for (uint32_t i = 0; i < packetList.size(); i++) {
                const int nTrackId = (int16_t)(packetList[i].flags >> 16);
                if (pWriterForAudioStreams.count(nTrackId)) {
                    auto pWriter = pWriterForAudioStreams[nTrackId];
                    if (pWriter == nullptr) {
                        PrintMes(RGY_LOG_ERROR, _T("Invalid writer found for audio track %d\n"), nTrackId);
                        return 1;
                    }
                    if (0 != (sts = pWriter->WriteNextPacket(&packetList[i]))) {
                        return 1;
                    }
                } else {
                    PrintMes(RGY_LOG_ERROR, _T("Failed to find writer for audio track %d\n"), nTrackId);
                    return 1;
                }
            }
        }
        return 0;
    };

    if (m_cuvidDec) {
        auto th_input = std::thread([this, pVideoCtx, &nvStatus](){
            CUresult curesult = CUDA_SUCCESS;
            vector<uint8_t> bitstream;
            int sts = NVENC_THREAD_RUNNING;
            for (int i = 0; sts == NVENC_THREAD_RUNNING && nvStatus == NV_ENC_SUCCESS && !m_cuvidDec->GetError(); i++) {
                sts = m_pFileReader->LoadNextFrame(nullptr, 0);
                int64_t pts;
                m_pFileReader->GetNextBitstream(bitstream, &pts);
                PrintMes(RGY_LOG_TRACE, _T("Set packet %d\n"), i);
                if (CUDA_SUCCESS != (curesult = m_cuvidDec->DecodePacket(bitstream.data(), bitstream.size(), pts, pVideoCtx->pkt_timebase))) {
                    PrintMes(RGY_LOG_ERROR, _T("Error in DecodePacket: %d (%s).\n"), curesult, char_to_tstring(_cudaGetErrorEnum(curesult)).c_str());
                    return curesult;
                }
            }
            if (CUDA_SUCCESS != (curesult = m_cuvidDec->DecodePacket(nullptr, 0, AV_NOPTS_VALUE, pVideoCtx->pkt_timebase))) {
                PrintMes(RGY_LOG_ERROR, _T("Error in DecodePacketFin: %d (%s).\n"), curesult, char_to_tstring(_cudaGetErrorEnum(curesult)).c_str());
            }
            return curesult;
        });
        PrintMes(RGY_LOG_DEBUG, _T("Started Encode thread\n"));

        CProcSpeedControl speedCtrl(m_nProcSpeedLimit);
        int64_t nEstimatedPts = AV_NOPTS_VALUE;
        const int nFrameDuration = (int)av_rescale_q(1, av_make_q(m_inputFps.second, m_inputFps.first), pVideoCtx->pkt_timebase);
        int decodedFrame = 0;
        int encodedFrame = 0;
        while (!m_cuvidDec->GetError()
            && !(m_cuvidDec->frameQueue()->isEndOfDecode() && m_cuvidDec->frameQueue()->isEmpty())) {
            if (m_pAbortByUser && *m_pAbortByUser) {
                nvStatus = NV_ENC_ERR_ABORT;
                break;
            }

            CUVIDPARSERDISPINFO pInfo;
            if (m_cuvidDec->frameQueue()->dequeue(&pInfo)) {
                int64_t pts = av_rescale_q(pInfo.timestamp, HW_NATIVE_TIMEBASE, pVideoCtx->pkt_timebase);
                if (m_pTrimParam && !frame_inside_range(decodedFrame++, m_trimParam.list)) {
                    m_cuvidDec->frameQueue()->releaseFrame(&pInfo);
                    continue;
                }

                auto encode = [&](CUVIDPROCPARAMS oVPP) {
                    speedCtrl.wait();
                    CUresult curesult = CUDA_SUCCESS;
                    PrintMes(RGY_LOG_TRACE, _T("Get decoded frame %d\n"), decodedFrame);
                    CUdeviceptr dMappedFrame = 0;
                    unsigned int pitch;
                    if (CUDA_SUCCESS != (curesult = cuvidMapVideoFrame(m_cuvidDec->GetDecoder(), pInfo.picture_index, &dMappedFrame, &pitch, &oVPP))) {
                        PrintMes(RGY_LOG_ERROR, _T("Error cuvidMapVideoFrame: %d (%s).\n"), curesult, char_to_tstring(_cudaGetErrorEnum(curesult)).c_str());
                        return NV_ENC_ERR_GENERIC;
                    }

                    EncodeFrameConfig stEncodeConfig = { 0 };
                    stEncodeConfig.dptr = dMappedFrame;
                    stEncodeConfig.pitch = pitch;
                    stEncodeConfig.width = m_uEncWidth;
                    stEncodeConfig.height = m_uEncHeight;
                    PrintMes(RGY_LOG_TRACE, _T("Set frame to encode %d\n"), decodedFrame);
                    pInfo.timestamp = encodedFrame;
                    auto encstatus = EncodeFrame(&stEncodeConfig, pInfo.timestamp);
                    encodedFrame++;
                    if (NV_ENC_SUCCESS != encstatus) {
                        PrintMes(RGY_LOG_ERROR, _T("Error EncodeFrame: %d.\n"), encstatus);
                        return encstatus;
                    }

                    if (CUDA_SUCCESS != (curesult = cuvidUnmapVideoFrame(m_cuvidDec->GetDecoder(), dMappedFrame))) {
                        PrintMes(RGY_LOG_ERROR, _T("Error cuvidMapVideoFrame: %d (%s).\n"), curesult, char_to_tstring(_cudaGetErrorEnum(curesult)).c_str());
                        return NV_ENC_ERR_GENERIC;
                    }
                    return NV_ENC_SUCCESS;
                };
                CUVIDPROCPARAMS oVPP = { 0 };
                oVPP.top_field_first = m_stPicStruct != NV_ENC_PIC_STRUCT_FIELD_BOTTOM_TOP;

                auto encode_frame = [&]() {
                    NVENCSTATUS status = NV_ENC_SUCCESS;
                    auto deint = m_cuvidDec->getDeinterlaceMode();
                    switch (deint) {
                    case cudaVideoDeinterlaceMode_Weave:
                        oVPP.unpaired_field = 1;
                        oVPP.progressive_frame = (m_stPicStruct == NV_ENC_PIC_STRUCT_FRAME);
                        status = encode(oVPP);
                        break;
                    case cudaVideoDeinterlaceMode_Bob:
                        oVPP.progressive_frame = 0;
                        oVPP.second_field = 0;
                        status = encode(oVPP);
                        if (NV_ENC_SUCCESS != status) return status;
                        oVPP.second_field = 1;
                        status = encode(oVPP);
                        break;
                    case cudaVideoDeinterlaceMode_Adaptive:
                        oVPP.progressive_frame = 0;
                        status = encode(oVPP);
                        break;
                    default:
                        PrintMes(RGY_LOG_ERROR, _T("Unknown Deinterlace mode\n"));
                        status = NV_ENC_ERR_GENERIC;
                        break;
                    }
                    return status;
                };

                if ((m_nAVSyncMode & NV_AVSYNC_FORCE_CFR) == NV_AVSYNC_FORCE_CFR) {
                    if (nEstimatedPts == AV_NOPTS_VALUE) {
                        nEstimatedPts = pts;
                    }
                    auto ptsDiff = pts - nEstimatedPts;
                    nEstimatedPts += nFrameDuration;
                    if (ptsDiff >= std::max(1, nFrameDuration * 3 / 4)) {
                        //水増しが必要
                        if (NV_ENC_SUCCESS != (nvStatus = encode_frame())) break;
                        nEstimatedPts += nFrameDuration;
                        if (NV_ENC_SUCCESS != (nvStatus = encode_frame())) break;
                    } else {
                        if (ptsDiff <= std::min(-1, -1 * nFrameDuration * 3 / 4)) {
                            //間引きが必要
                            continue;
                        } else {
                            if (NV_ENC_SUCCESS != (nvStatus = encode_frame())) break;
                        }
                    }
                } else {
                    if (NV_ENC_SUCCESS != (nvStatus = encode_frame())) break;
                }

                if (0 != extract_audio()) {
                    nvStatus = NV_ENC_ERR_GENERIC;
                    break;
                }
                m_cuvidDec->frameQueue()->releaseFrame(&pInfo);
            } else {
                m_cuvidDec->frameQueue()->waitForQueueUpdate();
            }
        }
        if (th_input.joinable()) {
            //ここでフレームをすべて吐き出し切らないと、中断時にデコードスレッドが終了しない
            while (!m_cuvidDec->GetError()
                && !(m_cuvidDec->frameQueue()->isEndOfDecode() && m_cuvidDec->frameQueue()->isEmpty())) {
                CUVIDPARSERDISPINFO pInfo;
                if (m_cuvidDec->frameQueue()->dequeue(&pInfo)) {
                    m_cuvidDec->frameQueue()->releaseFrame(&pInfo);
                }
            }
            th_input.join();
        }
        PrintMes(RGY_LOG_DEBUG, _T("Joined Encode thread\n"));
        if (m_cuvidDec->GetError()) {
            nvStatus = NV_ENC_ERR_GENERIC;
        }
    } else
#endif //#if ENABLE_AVSW_READER
    {
        CProcSpeedControl speedCtrl(m_nProcSpeedLimit);
        for (int iFrame = 0; nvStatus == NV_ENC_SUCCESS; iFrame++) {
            if (m_pAbortByUser && *m_pAbortByUser) {
                nvStatus = NV_ENC_ERR_ABORT;
                break;
            }
            speedCtrl.wait();
#if ENABLE_AVSW_READER
            if (0 != extract_audio()) {
                nvStatus = NV_ENC_ERR_GENERIC;
                break;
            }
#endif //#if ENABLE_AVSW_READER
            uint32_t lockedPitch = 0;
            unsigned char *pInputSurface = nullptr;
            const int index = iFrame % bufferCount;
            NvEncLockInputBuffer(m_stEncodeBuffer[index].stInputBfr.hInputSurface, (void**)&pInputSurface, &lockedPitch);
            ret = m_pFileReader->LoadNextFrame(pInputSurface, lockedPitch);
            NvEncUnlockInputBuffer(m_stEncodeBuffer[index].stInputBfr.hInputSurface);
            if (ret)
                break;
            nvStatus = EncodeFrame(iFrame);
        }
    }
#if ENABLE_AVSW_READER
    for (const auto& writer : m_pFileWriterListAudio) {
        auto pAVCodecWriter = std::dynamic_pointer_cast<RGYOutputAvcodec>(writer);
        if (pAVCodecWriter != nullptr) {
            //エンコーダなどにキャッシュされたパケットを書き出す
            pAVCodecWriter->WriteNextPacket(nullptr);
        }
    }
#endif //#if ENABLE_AVSW_READER
    PrintMes(RGY_LOG_INFO, _T("                                                                         \n"));
    //FlushEncoderはかならず行わないと、NvEncDestroyEncoderで異常終了する
    auto encstatus = FlushEncoder();
    if (nvStatus == NV_ENC_SUCCESS && encstatus != NV_ENC_SUCCESS) {
        PrintMes(RGY_LOG_ERROR, _T("Error FlushEncoder: %d.\n"), encstatus);
        nvStatus = encstatus;
    } else {
        PrintMes(RGY_LOG_DEBUG, _T("Flushed Encoder\n"));
    }
    m_pFileReader->Close();
    m_pFileWriter->Close();
    m_pStatus->writeResult();
    return nvStatus;
}
#endif

tstring NVEncCore::GetEncodingParamsInfo(int output_level) {
    tstring str;
    auto add_str =[output_level, &str](int info_level, const TCHAR *fmt, ...) {
        if (info_level >= output_level) {
            va_list args;
            va_start(args, fmt);
            const size_t append_len = _vsctprintf(fmt, args) + 1;
            size_t current_len = _tcslen(str.c_str());
            str.resize(current_len + append_len, 0);
            _vstprintf_s(&str[current_len], append_len, fmt, args);
        }
    };

    auto value_or_auto =[](int value, int value_auto, const TCHAR *unit) {
        tstring str;
        if (value == value_auto) {
            str = _T("auto");
        } else {
            TCHAR buf[256];
            _stprintf_s(buf, _countof(buf), _T("%d %s"), value, unit);
            str = buf;
        }
        return str;
    };

    auto on_off =[](int value) {
        return (value) ? _T("on") : _T("off");
    };

    TCHAR cpu_info[1024] = { 0 };
    getCPUInfo(cpu_info, _countof(cpu_info));

    tstring gpu_info;
    {
        const auto& gpuInfo = std::find_if(m_GPUList.begin(), m_GPUList.end(), [device_id = m_nDeviceId](const NVGPUInfo& info) { return info.id == device_id; });
        if (gpuInfo != m_GPUList.end()) {
            gpu_info = strsprintf(_T("#%d: %s"), gpuInfo->id, gpuInfo->name.c_str());
            if (gpuInfo->cuda_cores > 0) {
                gpu_info += strsprintf(_T(" (%d cores"), gpuInfo->cuda_cores);
                if (gpuInfo->clock_rate > 0) {
                    gpu_info += strsprintf(_T(", %d MHz"), gpuInfo->clock_rate / 1000);
                }
                gpu_info += strsprintf(_T(")"));
            }
            if (gpuInfo->pcie_gen > 0 && gpuInfo->pcie_link > 0) {
                gpu_info += strsprintf(_T("[PCIe%dx%d]"), gpuInfo->pcie_gen, gpuInfo->pcie_link);
            }
            if (gpuInfo->nv_driver_version) {
                gpu_info += strsprintf(_T("[%d.%d]"), gpuInfo->nv_driver_version / 1000, (gpuInfo->nv_driver_version % 1000) / 10);
            }
        }
    }

    int cudaDriverVersion = 0;
    cuDriverGetVersion(&cudaDriverVersion);

    OSVERSIONINFOEXW osversioninfo = { 0 };
    tstring osversionstr = getOSVersion(&osversioninfo);
    const int codec = get_value_from_guid(m_stCodecGUID, list_nvenc_codecs);
    const RGY_CODEC rgy_codec = codec_guid_enc_to_rgy(m_stCodecGUID);
    auto sar = get_sar(m_uEncWidth, m_uEncHeight, m_stCreateEncodeParams.darWidth, m_stCreateEncodeParams.darHeight);
    add_str(RGY_LOG_ERROR, _T("%s\n"), get_encoder_version());
    add_str(RGY_LOG_INFO,  _T("OS Version     %s %s (%d)\n"), osversionstr.c_str(), rgy_is_64bit_os() ? _T("x64") : _T("x86"), osversioninfo.dwBuildNumber);
    add_str(RGY_LOG_INFO,  _T("CPU            %s\n"), cpu_info);
    add_str(RGY_LOG_INFO,  _T("GPU            %s\n"), gpu_info.c_str());
    add_str(RGY_LOG_INFO,  _T("NVENC / CUDA   NVENC API %d.%d, CUDA %d.%d, schedule mode: %s\n"),
        NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION,
        cudaDriverVersion / 1000, (cudaDriverVersion % 1000) / 10, get_chr_from_value(list_cuda_schedule, m_cudaSchedule));
    add_str(RGY_LOG_ERROR, _T("Input Buffers  %s, %d frames\n"), _T("CUDA"), m_encodeBufferCount);
    tstring inputMes = m_pFileReader->GetInputMessage();
    for (const auto& reader : m_AudioReaders) {
        inputMes += _T("\n") + tstring(reader->GetInputMessage());
    }
    auto inputMesSplitted = split(inputMes, _T("\n"));
    for (uint32_t i = 0; i < (uint32_t)inputMesSplitted.size(); i++) {
        add_str(RGY_LOG_ERROR, _T("%s%s\n"), (i == 0) ? _T("Input Info     ") : _T("               "), inputMesSplitted[i].c_str());
    }
#if ENABLE_AVSW_READER
    if (m_cuvidDec && m_cuvidDec->getDeinterlaceMode() != cudaVideoDeinterlaceMode_Weave) {
        add_str(RGY_LOG_ERROR, _T("Deinterlace    %s\n"), get_chr_from_value(list_deinterlace, m_cuvidDec->getDeinterlaceMode()));
    }
#endif //#if ENABLE_AVSW_READER
    if (m_trimParam.list.size()
        && !(m_trimParam.list[0].start == 0 && m_trimParam.list[0].fin == TRIM_MAX)) {
        add_str(RGY_LOG_ERROR, _T("%s"), _T("Trim           "));
        for (auto trim : m_trimParam.list) {
            if (trim.fin == TRIM_MAX) {
                add_str(RGY_LOG_ERROR, _T("%d-fin "), trim.start + m_trimParam.offset);
            } else {
                add_str(RGY_LOG_ERROR, _T("%d-%d "), trim.start + m_trimParam.offset, trim.fin + m_trimParam.offset);
            }
        }
        add_str(RGY_LOG_ERROR, _T("[offset: %d]\n"), m_trimParam.offset);
    }
    if (m_nAVSyncMode != RGY_AVSYNC_ASSUME_CFR) {
        add_str(RGY_LOG_ERROR, _T("AVSync         %s\n"), get_chr_from_value(list_avsync, m_nAVSyncMode));
    }
    tstring vppFilterMes;
    for (const auto& filter : m_vpFilters) {
        vppFilterMes += strsprintf(_T("%s%s\n"), (vppFilterMes.length()) ? _T("               ") : _T("Vpp Filters    "), filter->GetInputMessage().c_str());
    }
    add_str(RGY_LOG_ERROR, vppFilterMes.c_str());
    add_str(RGY_LOG_ERROR, _T("Output Info    %s %s%s @ Level %s\n"), get_name_from_guid(m_stCodecGUID, list_nvenc_codecs),
        get_codec_profile_name_from_guid(rgy_codec, m_stEncConfig.profileGUID).c_str(),
        (codec == NV_ENC_HEVC && 0 == memcmp(&NV_ENC_HEVC_PROFILE_FREXT_GUID, &m_stEncConfig.profileGUID, sizeof(GUID)) && m_stEncConfig.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 > 0) ? _T(" 10bit") : _T(""),
        get_codec_level_name(rgy_codec, m_stEncConfig.encodeCodecConfig.h264Config.level).c_str());
    add_str(RGY_LOG_ERROR, _T("               %dx%d%s %d:%d %.3ffps (%d/%dfps)\n"), m_uEncWidth, m_uEncHeight, (m_stEncConfig.frameFieldMode != NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME) ? _T("i") : _T("p"), sar.first, sar.second, m_stCreateEncodeParams.frameRateNum / (double)m_stCreateEncodeParams.frameRateDen, m_stCreateEncodeParams.frameRateNum, m_stCreateEncodeParams.frameRateDen);
    if (m_pFileWriter) {
        inputMesSplitted = split(m_pFileWriter->GetOutputMessage(), _T("\n"));
        for (auto mes : inputMesSplitted) {
            if (mes.length()) {
                add_str(RGY_LOG_ERROR,_T("%s%s\n"), _T("               "), mes.c_str());
            }
        }
    }
    for (auto pWriter : m_pFileWriterListAudio) {
        if (pWriter && pWriter != m_pFileWriter) {
            inputMesSplitted = split(pWriter->GetOutputMessage(), _T("\n"));
            for (auto mes : inputMesSplitted) {
                if (mes.length()) {
                    add_str(RGY_LOG_ERROR,_T("%s%s\n"), _T("               "), mes.c_str());
                }
            }
        }
    }
    add_str(RGY_LOG_INFO,  _T("Encoder Preset %s\n"), get_name_from_guid(m_stCreateEncodeParams.presetGUID, list_nvenc_preset_names));
    add_str(RGY_LOG_ERROR, _T("Rate Control   %s"), get_chr_from_value(list_nvenc_rc_method_en, m_stEncConfig.rcParams.rateControlMode));
    const bool lossless = (get_value_from_guid(m_stCodecGUID, list_nvenc_codecs) == NV_ENC_H264 && m_stCreateEncodeParams.encodeConfig->encodeCodecConfig.h264Config.qpPrimeYZeroTransformBypassFlag)
        || memcmp(&m_stCreateEncodeParams.presetGUID, &NV_ENC_PRESET_LOSSLESS_HP_GUID, sizeof(m_stCreateEncodeParams.presetGUID)) == 0
        || memcmp(&m_stCreateEncodeParams.presetGUID, &NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID, sizeof(m_stCreateEncodeParams.presetGUID)) == 0;
    if (NV_ENC_PARAMS_RC_CONSTQP == m_stEncConfig.rcParams.rateControlMode) {
        add_str(RGY_LOG_ERROR, _T("  I:%d  P:%d  B:%d%s\n"), m_stEncConfig.rcParams.constQP.qpIntra, m_stEncConfig.rcParams.constQP.qpInterP, m_stEncConfig.rcParams.constQP.qpInterB,
            lossless ? _T(" (lossless)") : _T(""));
    } else {
        add_str(RGY_LOG_ERROR, _T("\n"));
        add_str(RGY_LOG_ERROR, _T("Bitrate        %d kbps (Max: %d kbps)\n"), m_stEncConfig.rcParams.averageBitRate / 1000, m_stEncConfig.rcParams.maxBitRate / 1000);
        if (m_stEncConfig.rcParams.targetQuality) {
            double targetQuality = m_stEncConfig.rcParams.targetQuality + m_stEncConfig.rcParams.targetQualityLSB / 256.0;
            add_str(RGY_LOG_ERROR, _T("Target Quality %.2f\n"), targetQuality);
        } else {
            add_str(RGY_LOG_ERROR, _T("Target Quality auto\n"));
        }
        if (m_stEncConfig.rcParams.enableInitialRCQP) {
            add_str(RGY_LOG_INFO,  _T("Initial QP     I:%d  P:%d  B:%d\n"), m_stEncConfig.rcParams.initialRCQP.qpIntra, m_stEncConfig.rcParams.initialRCQP.qpInterP, m_stEncConfig.rcParams.initialRCQP.qpInterB);
        }
        if (m_stEncConfig.rcParams.enableMaxQP || m_stEncConfig.rcParams.enableMinQP) {
            int minQPI = (m_stEncConfig.rcParams.enableMinQP) ? m_stEncConfig.rcParams.minQP.qpIntra  :  0;
            int maxQPI = (m_stEncConfig.rcParams.enableMaxQP) ? m_stEncConfig.rcParams.maxQP.qpIntra  : 51;
            int minQPP = (m_stEncConfig.rcParams.enableMinQP) ? m_stEncConfig.rcParams.minQP.qpInterP :  0;
            int maxQPP = (m_stEncConfig.rcParams.enableMaxQP) ? m_stEncConfig.rcParams.maxQP.qpInterP : 51;
            int minQPB = (m_stEncConfig.rcParams.enableMinQP) ? m_stEncConfig.rcParams.minQP.qpInterB :  0;
            int maxQPB = (m_stEncConfig.rcParams.enableMaxQP) ? m_stEncConfig.rcParams.maxQP.qpInterB : 51;
            add_str(RGY_LOG_INFO,  _T("QP range       I:%d-%d  P:%d-%d  B:%d-%d\n"), minQPI, maxQPI, minQPP, maxQPP, minQPB, maxQPB);
        }
        add_str(RGY_LOG_INFO,  _T("VBV buf size   %s\n"), value_or_auto(m_stEncConfig.rcParams.vbvBufferSize / 1000,   0, _T("kbit")).c_str());
        add_str(RGY_LOG_DEBUG, _T("VBV init delay %s\n"), value_or_auto(m_stEncConfig.rcParams.vbvInitialDelay / 1000, 0, _T("kbit")).c_str());
    }
    if (m_dynamicRC.size() > 0) {
        tstring strDynamicRC = tstring(_T("DynamicRC      ")) + m_dynamicRC[0].print();
        for (int i = 1; i < (int)m_dynamicRC.size(); i++) {
            strDynamicRC += _T("\n               ") + m_dynamicRC[i].print();
        }
        add_str(RGY_LOG_INFO, _T("%s\n"), strDynamicRC.c_str());
    }
    tstring strLookahead = _T("Lookahead      ");
    if (m_stEncConfig.rcParams.enableLookahead) {
        strLookahead += strsprintf(_T("on, %d frames"), m_stEncConfig.rcParams.lookaheadDepth);
        if (!m_stEncConfig.rcParams.disableBadapt || !m_stEncConfig.rcParams.disableIadapt) {
            strLookahead += _T(", Adaptive ");
            if (!m_stEncConfig.rcParams.disableIadapt) strLookahead += _T("I");
            if (!m_stEncConfig.rcParams.disableBadapt && !m_stEncConfig.rcParams.disableIadapt) strLookahead += _T(", ");
            if (!m_stEncConfig.rcParams.disableBadapt) strLookahead += _T("B");
            strLookahead += _T(" Insert");
        }
    } else {
        strLookahead += _T("off");
    }
    add_str(RGY_LOG_INFO,  _T("%s\n"), strLookahead.c_str());
    add_str(RGY_LOG_INFO,  _T("GOP length     %d frames\n"), m_stEncConfig.gopLength);
    const auto bref_mode = (codec == NV_ENC_H264) ? m_stEncConfig.encodeCodecConfig.h264Config.useBFramesAsRef : m_stEncConfig.encodeCodecConfig.hevcConfig.useBFramesAsRef;
    add_str(RGY_LOG_INFO,  _T("B frames       %d frames [ref mode: %s]\n"), m_stEncConfig.frameIntervalP - 1, get_chr_from_value(list_bref_mode, bref_mode));
    if (codec == NV_ENC_H264) {
        add_str(RGY_LOG_DEBUG, _T("Output         "));
        TCHAR bitstream_info[256] ={ 0 };
        if (m_stEncConfig.encodeCodecConfig.h264Config.outputBufferingPeriodSEI) _tcscat_s(bitstream_info, _countof(bitstream_info), _T("BufferingPeriodSEI,"));
        if (m_stEncConfig.encodeCodecConfig.h264Config.outputPictureTimingSEI)   _tcscat_s(bitstream_info, _countof(bitstream_info), _T("PicTimingSEI,"));
        if (m_stEncConfig.encodeCodecConfig.h264Config.outputAUD)                _tcscat_s(bitstream_info, _countof(bitstream_info), _T("AUD,"));
        if (m_stEncConfig.encodeCodecConfig.h264Config.outputFramePackingSEI)    _tcscat_s(bitstream_info, _countof(bitstream_info), _T("FramePackingSEI,"));
        if (m_stEncConfig.encodeCodecConfig.h264Config.outputRecoveryPointSEI)   _tcscat_s(bitstream_info, _countof(bitstream_info), _T("RecoveryPointSEI,"));
        if (m_stEncConfig.encodeCodecConfig.h264Config.repeatSPSPPS)             _tcscat_s(bitstream_info, _countof(bitstream_info), _T("repeatSPSPPS,"));
        if (_tcslen(bitstream_info)) {
            bitstream_info[_tcslen(bitstream_info)-1] = _T('\0');
        } else {
            _tcscpy_s(bitstream_info, _countof(bitstream_info), _T("-"));
        }
        add_str(RGY_LOG_DEBUG, _T("%s\n"), bitstream_info);
    }

    const bool bEnableLTR = (codec == NV_ENC_H264) ? m_stEncConfig.encodeCodecConfig.h264Config.enableLTR : m_stEncConfig.encodeCodecConfig.hevcConfig.enableLTR;
    tstring strRef = strsprintf(_T("%d frames, LTR: %s"),
        (codec == NV_ENC_H264) ? m_stEncConfig.encodeCodecConfig.h264Config.maxNumRefFrames : m_stEncConfig.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB,
        (bEnableLTR) ? _T("on") : _T("off"));
    add_str(RGY_LOG_INFO,  _T("Ref frames     %s\n"), strRef.c_str());

    tstring strAQ;
    if (m_stEncConfig.rcParams.enableAQ || m_stEncConfig.rcParams.enableTemporalAQ) {
        strAQ = _T("on");
        if (codec == NV_ENC_H264) {
            strAQ += _T("(");
            if (m_stEncConfig.rcParams.enableAQ)         strAQ += _T("spatial");
            if (m_stEncConfig.rcParams.enableAQ && m_stEncConfig.rcParams.enableTemporalAQ) strAQ += _T(", ");
            if (m_stEncConfig.rcParams.enableTemporalAQ) strAQ += _T("temporal");
            strAQ += _T(", strength ");
            strAQ += (m_stEncConfig.rcParams.aqStrength == 0) ? _T("auto") : strsprintf(_T("%d"), m_stEncConfig.rcParams.aqStrength);
            strAQ += _T(")");
        }
    } else {
        strAQ = _T("off");
    }
    add_str(RGY_LOG_INFO,  _T("AQ             %s\n"), strAQ.c_str());
    if (codec == NV_ENC_H264) {
        if (m_stEncConfig.encodeCodecConfig.h264Config.sliceMode == 3) {
            add_str(RGY_LOG_DEBUG, _T("Slices            %d\n"), m_stEncConfig.encodeCodecConfig.h264Config.sliceModeData);
        } else {
            add_str(RGY_LOG_DEBUG, _T("Slice          Mode:%d, ModeData:%d\n"), m_stEncConfig.encodeCodecConfig.h264Config.sliceMode, m_stEncConfig.encodeCodecConfig.h264Config.sliceModeData);
        }
    } else if (codec == NV_ENC_HEVC) {
        if (m_stEncConfig.encodeCodecConfig.hevcConfig.sliceMode == 3) {
            add_str(RGY_LOG_DEBUG, _T("Slices            %d\n"), m_stEncConfig.encodeCodecConfig.hevcConfig.sliceModeData);
        } else {
            add_str(RGY_LOG_DEBUG, _T("Slice          Mode:%d, ModeData:%d\n"), m_stEncConfig.encodeCodecConfig.hevcConfig.sliceMode, m_stEncConfig.encodeCodecConfig.hevcConfig.sliceModeData);
        }
    }
    if (codec == NV_ENC_HEVC) {
        add_str(RGY_LOG_INFO, _T("CU max / min   %s / %s\n"),
            get_chr_from_value(list_hevc_cu_size, m_stEncConfig.encodeCodecConfig.hevcConfig.maxCUSize),
            get_chr_from_value(list_hevc_cu_size, m_stEncConfig.encodeCodecConfig.hevcConfig.minCUSize));
        if (m_hdr10plus) {
            add_str(RGY_LOG_DEBUG, _T("Dynamic HDR10     %s\n"), m_hdr10plus->inputJson().c_str());
        }
    }
    add_str(RGY_LOG_INFO, _T("Others         "));
    add_str(RGY_LOG_INFO, _T("mv:%s "), get_chr_from_value(list_mv_presicion, m_stEncConfig.mvPrecision));
    if (m_stCreateEncodeParams.enableWeightedPrediction) {
        add_str(RGY_LOG_INFO, _T("weightp "));
    }
    if (m_stCreateEncodeParams.encodeConfig->rcParams.enableNonRefP) {
        add_str(RGY_LOG_INFO, _T("nonrefp "));
    }
    if (codec == NV_ENC_H264) {
        add_str(RGY_LOG_INFO, _T("%s "), get_chr_from_value(list_entropy_coding, m_stEncConfig.encodeCodecConfig.h264Config.entropyCodingMode));
        add_str(RGY_LOG_INFO, (m_stEncConfig.encodeCodecConfig.h264Config.disableDeblockingFilterIDC == 0) ? _T("deblock ") : _T("no_deblock "));
        add_str(RGY_LOG_DEBUG, _T("hierarchyFrame P:%s  B:%s\n"), on_off(m_stEncConfig.encodeCodecConfig.h264Config.hierarchicalPFrames), on_off(m_stEncConfig.encodeCodecConfig.h264Config.hierarchicalBFrames));
        add_str(RGY_LOG_DEBUG, m_stEncConfig.encodeCodecConfig.h264Config.enableVFR ? _T("VFR ") : _T(""));
        add_str(RGY_LOG_INFO,  _T("adapt-transform:%s "), get_chr_from_value(list_adapt_transform, m_stEncConfig.encodeCodecConfig.h264Config.adaptiveTransformMode));
        add_str(RGY_LOG_DEBUG, _T("fmo:%s "), get_chr_from_value(list_fmo, m_stEncConfig.encodeCodecConfig.h264Config.fmoMode));
        if (m_stCreateEncodeParams.encodeConfig->frameIntervalP - 1 > 0) {
            add_str(RGY_LOG_INFO, _T("bdirect:%s "), get_chr_from_value(list_bdirect, m_stEncConfig.encodeCodecConfig.h264Config.bdirectMode));
        }
        if (m_stEncConfig.encodeCodecConfig.h264Config.outputAUD) {
            add_str(RGY_LOG_INFO, _T("aud "));
        }
        if (m_stEncConfig.encodeCodecConfig.h264Config.outputPictureTimingSEI) {
            add_str(RGY_LOG_INFO, _T("pic-struct "));
        }
    } else if (codec == NV_ENC_HEVC) {
        if (m_stEncConfig.encodeCodecConfig.hevcConfig.outputAUD) {
            add_str(RGY_LOG_INFO, _T("aud "));
        }
        if (m_stEncConfig.encodeCodecConfig.hevcConfig.outputPictureTimingSEI) {
            add_str(RGY_LOG_INFO, _T("pic-struct "));
        }
    }
    add_str(RGY_LOG_INFO, _T("\n"));
    return str;
}

void NVEncCore::PrintEncodingParamsInfo(int output_level) {
    PrintMes(RGY_LOG_INFO, _T("%s"), GetEncodingParamsInfo(output_level).c_str());
}
