/*
 * qq-nvenc：把 QQ 屏幕共享的 H.264 编码换成 NVENC。
 *
 * 背景（QQ 3.2.34-53644，resources/app/avsdk/）：
 *   屏幕共享的视频在 ppapi 进程里由 libAVSDKPlugin.so 编码，用的是静态编进该库的
 *   软件 OpenH264（o264rt 包装层）。AVSDK 里没有任何 NVENC 实现
 *   （strings | grep -c 'NV_ENC_|nvenc|libnvidia-encode' = 0），运行时也不加载
 *   libnvidia-encode.so，所以打开 AVSDK_SetHWAbility 的硬件位没有用。
 *
 *   但 AVSDK 的编码器接口很小，对象由一个函数指针工厂造出来：
 *     VENCODER_NS::IVEncoder                    纯虚接口（9 个虚函数）
 *     CO264rtEnc : IVEncoder                    本库唯一实现
 *     CreateH264Encoder(CO264rtEnc **out)       @0xdfce60（导出，入口 14 字节指令边界干净）
 *   因此这里 inline hook CreateH264Encoder 的入口，替掉返回对象的 vtable。
 *   注意对象 vptr 指向的是 C++ ABI 去掉两个表头之后的第一个虚函数，下标要从对象 vptr 数：
 *     [0] ~CO264rtEnc(D1) [1] ~CO264rtEnc(D0) [2] Init [3] UnInit [4] GetErrorCode
 *     [5] SetCodecCallback [6] SetParam [7] GetParam [8] DoEncode
 *
 * 输出契约（实测自原实现的原话）：
 *   原实现在每帧编好后往 AVSDK 递过来的 VideoPacket 里填
 *     +0x08 帧序号   +0x10 平均 QP   +0x14 长度   +0x18 码流指针
 *   然后调 SetCodecCallback 收到的回调：cb0(SetCodecCallback 的 a, &packet, cb1)。
 *   回调最终进 CVideoEncoder::CodecDoneCallback(this, data, size, idx, qp)。
 *   VideoFrame 是 I420：+0x00 宽 +0x04 高 +0x18 Y 行距 +0x1c U/V 行距
 *                       +0x28/+0x30/+0x38 三个平面指针。
 *   EncParam：+0x00 宽 +0x04 高 +0x08 码率(kbps) +0x0c 帧率。
 *   SetParam("frame_type") 值为 1 表示请求一个 IDR。
 *
 * 开关：
 *   QQ_NVENC=1              在 ppapi 里挂钩并旁观（透传，行为与不注入一致）
 *   QQ_NVENC_ACTIVE=1       真正用 NVENC 编码（默认关，失败自动回落原实现）
 *   QQ_NVENC_PROBE_DUMP=0   只挂钩、不打字节
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ffnvcodec/nvEncodeAPI.h>

#define LOG(...) do {                                                     \
        fprintf(stderr, "[qq-nvenc pid=%ld] ", (long)getpid());           \
        fprintf(stderr, __VA_ARGS__);                                     \
        fputc('\n', stderr);                                              \
    } while (0)

/* QQ 3.2.34-53644 里 libAVSDKPlugin.so 内的偏移 */
#define AVSDK_NAME        "libAVSDKPlugin.so"
#define OFF_CREATE_H264   0xdfce60u   /* CreateH264Encoder */
#define HOOK_LEN          14u         /* push rbp; mov rbp,rdi; mov edi,0x210; push rbx; sub rsp,8 */
#define VT_OFF_INIT       2u
#define VT_OFF_UNINIT     3u
#define VT_OFF_SETCB      5u
#define VT_OFF_SETPARAM   6u
#define VT_OFF_ENCODE     8u
#define VT_ENTRIES        9u

/* VideoPacket 里 AVSDK 读的字段 */
#define PKT_OFF_IDX       0x08u
#define PKT_OFF_QP        0x10u
#define PKT_OFF_LEN       0x14u
#define PKT_OFF_DATA      0x18u

/* VideoFrame 布局 */
#define VF_OFF_W          0x00u
#define VF_OFF_H          0x04u
#define VF_OFF_YPITCH     0x18u
#define VF_OFF_UVPITCH    0x1cu
#define VF_OFF_Y          0x28u
#define VF_OFF_U          0x30u
#define VF_OFF_V          0x38u

/* EncParam 布局 */
#define EP_OFF_W          0x00u
#define EP_OFF_H          0x04u
#define EP_OFF_KBPS       0x08u
#define EP_OFF_FPS        0x0cu

static const unsigned char want_create16[HOOK_LEN] = {
    0x55, 0x48, 0x89, 0xfd, 0xbf, 0x10, 0x02, 0x00,
    0x00, 0x53, 0x48, 0x83, 0xec, 0x08,
};

static uintptr_t avsdk_base;
static void *trampoline;                  /* 原实现（前 14 字节 + 跳回） */
static int probe_dump = 1;
static int nv_active;                     /* 是否真的用 NVENC */
static int hooked;

/* ---------- 环境开关 ---------- */

static int flag_on(const char *name, int dflt)
{
    const char *v = getenv(name);

    if (!v || !*v)
        return dflt;
    return strcmp(v, "0") != 0;
}

static void put32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(void *p, void *v) { memcpy(p, &v, 8); }
static uint32_t get32(const void *p)
{
    uint32_t v;

    memcpy(&v, p, 4);
    return v;
}
static void *getptr(const void *p)
{
    void *v;

    memcpy(&v, p, 8);
    return v;
}

/* ---------- 进程与模块定位 ---------- */

/* 只有 ppapi 进程才加载并运行 AVSDK 的编码器；其它进程（主进程、zygote…）直接不动。
 * cmdline 是 NUL 分隔的 argv，不能用 strstr（它在第一个 NUL 就停了），要逐条比。 */
static int is_ppapi_process(void)
{
    char buf[8192];
    ssize_t n;
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    for (const char *a = buf; a < buf + n; a += strlen(a) + 1)
        if (!strcmp(a, "--type=ppapi"))
            return 1;
    return 0;
}

static int find_avsdk(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    const char *name = info->dlpi_name ? info->dlpi_name : "";
    const char *base = strrchr(name, '/');

    if (!strcmp(base ? base + 1 : name, AVSDK_NAME)) {
        *(uintptr_t *)data = info->dlpi_addr;
        return 1;
    }
    return 0;
}

/* ---------- 接口类型 ---------- */

typedef struct VideoFrame VideoFrame;
typedef struct VideoPacket VideoPacket;

typedef int (*init_fn)(void *self, const void *param);
typedef int (*uninit_fn)(void *self);
typedef int (*encode_fn)(void *self, VideoFrame *f, VideoPacket *p, void *user);
typedef int (*setcb_fn)(void *self, void *a, void *cb);
typedef int (*setparam_fn)(void *self, const char *name, void *data, uint32_t *len);
typedef int (*create_fn)(void **out);
typedef void (*pkt_cb_fn)(void *ctx, void **pp_packet, void *user);

/* ---------- NVENC 运行时（进程内共享一份 CUDA 上下文与函数表） ---------- */

static pthread_mutex_t nv_global_lock = PTHREAD_MUTEX_INITIALIZER;
static int nv_global_tried;
static int nv_global_ok;
static NV_ENCODE_API_FUNCTION_LIST nvfl;
static void *nv_cuda_ctx;
static void *nv_cuda_handle, *nv_enc_handle;

static int nv_load_unlocked(void)
{
    if (nv_global_tried)
        return nv_global_ok;
    nv_global_tried = 1;

    nv_cuda_handle = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!nv_cuda_handle) {
        LOG("NVENC: dlopen libcuda.so.1 失败: %s", dlerror());
        return 0;
    }
    int (*cuInit)(unsigned) = dlsym(nv_cuda_handle, "cuInit");
    int (*cuCtxCreate)(void **, unsigned, int) =
        dlsym(nv_cuda_handle, "cuCtxCreate_v2");
    if (!cuCtxCreate)
        cuCtxCreate = dlsym(nv_cuda_handle, "cuCtxCreate");
    int (*cuCtxPushCurrent)(void *) = dlsym(nv_cuda_handle, "cuCtxPushCurrent_v2");
    if (!cuCtxPushCurrent)
        cuCtxPushCurrent = dlsym(nv_cuda_handle, "cuCtxPushCurrent");
    if (!cuInit || !cuCtxCreate) {
        LOG("NVENC: libcuda 缺 cuInit/cuCtxCreate");
        return 0;
    }
    if (cuInit(0) != 0) {
        LOG("NVENC: cuInit 失败（没有 NVIDIA 设备？）");
        return 0;
    }
    if (cuCtxCreate(&nv_cuda_ctx, 0, 0) != 0 || !nv_cuda_ctx) {
        LOG("NVENC: cuCtxCreate 失败");
        return 0;
    }
    if (cuCtxPushCurrent)
        cuCtxPushCurrent(nv_cuda_ctx);

    nv_enc_handle = dlopen("libnvidia-encode.so.1", RTLD_NOW);
    if (!nv_enc_handle) {
        LOG("NVENC: dlopen libnvidia-encode.so.1 失败: %s", dlerror());
        return 0;
    }
    NVENCSTATUS (*create_instance)(NV_ENCODE_API_FUNCTION_LIST *) =
        dlsym(nv_enc_handle, "NvEncodeAPICreateInstance");
    if (!create_instance) {
        LOG("NVENC: 找不到 NvEncodeAPICreateInstance");
        return 0;
    }
    memset(&nvfl, 0, sizeof nvfl);
    nvfl.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (create_instance(&nvfl) != NV_ENC_SUCCESS) {
        LOG("NVENC: NvEncodeAPICreateInstance 失败");
        return 0;
    }
    nv_global_ok = 1;
    LOG("NVENC: 就绪（CUDA ctx=%p, apiVersion=0x%x）", nv_cuda_ctx, NVENCAPI_VERSION);
    return 1;
}

static int nv_load(void)
{
    int r;

    pthread_mutex_lock(&nv_global_lock);
    r = nv_load_unlocked();
    pthread_mutex_unlock(&nv_global_lock);
    return r;
}

/* ---------- 每个被包对象的状态 ---------- */

struct wrapped {
    void *obj;
    void **orig_vtable;
    void **vtable;
    int dump_done;
    int cb_logged;
    int init_calls;
    int encode_calls;
    int setparam_logged;

    /* NVENC */
    int nv_on;                    /* 会话可用 */
    int nv_dead;                  /* 出过错，永久回落 */
    int w, h, fps, kbps;
    void *session, *inbuf, *outbuf;
    uint32_t pitch, out_cap;
    long frame_idx;
    int force_idr;

    /* SetCodecCallback 拿到的（原始回调，不是我们的转发壳） */
    int have_cb;
    void *cb_ctx, *cb0, *cb1;
};
#define MAX_WRAPPED 8
static struct wrapped wrapped[MAX_WRAPPED];
static pthread_mutex_t wrap_lock = PTHREAD_MUTEX_INITIALIZER;

static void hexdump(const char *tag, const void *p, size_t n)
{
    const unsigned char *b = p;
    char line[3 * 16 + 1];
    size_t i, j;

    if (!p)
        return;
    for (i = 0; i < n; i += 16) {
        size_t k = (n - i > 16) ? 16 : n - i;
        for (j = 0; j < k; ++j)
            sprintf(line + 3 * j, "%02x ", b[i + j]);
        line[3 * k] = '\0';
        LOG("%s +0x%03zx: %s", tag, i, line);
    }
}

static struct wrapped *find_wrapped(void *self)
{
    struct wrapped *w = NULL;

    pthread_mutex_lock(&wrap_lock);
    for (int i = 0; i < MAX_WRAPPED; ++i)
        if (wrapped[i].obj == self)
            w = &wrapped[i];
    pthread_mutex_unlock(&wrap_lock);
    return w;
}

/* ---------- NVENC 会话 ---------- */

static void nv_close(struct wrapped *w)
{
    if (!nv_global_ok)
        return;
    if (w->session) {
        if (w->outbuf)
            nvfl.nvEncDestroyBitstreamBuffer(w->session, w->outbuf);
        if (w->inbuf)
            nvfl.nvEncDestroyInputBuffer(w->session, w->inbuf);
        nvfl.nvEncDestroyEncoder(w->session);
    }
    w->session = w->inbuf = w->outbuf = NULL;
    w->nv_on = 0;
}

static int nv_open(struct wrapped *w, int wpx, int hpx, int fps, int kbps)
{
    NVENCSTATUS st;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp;
    NV_ENC_PRESET_CONFIG pc;
    NV_ENC_CONFIG cfg;
    NV_ENC_INITIALIZE_PARAMS ip;
    NV_ENC_CREATE_INPUT_BUFFER ib;
    NV_ENC_CREATE_BITSTREAM_BUFFER bb;

    if (!nv_load())
        return -1;
    if (!wpx || !hpx || wpx > 4096 || hpx > 4096)
        return -1;
    if (fps <= 0 || fps > 240)
        fps = 30;
    if (kbps <= 0)
        kbps = 2000;

    nv_close(w);

    memset(&sp, 0, sizeof sp);
    sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sp.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sp.device = nv_cuda_ctx;
    sp.apiVersion = NVENCAPI_VERSION;
    if ((st = nvfl.nvEncOpenEncodeSessionEx(&sp, &w->session)) != NV_ENC_SUCCESS) {
        LOG("NVENC: 开会话失败 status=%d", st);
        return -1;
    }

    memset(&pc, 0, sizeof pc);
    pc.version = NV_ENC_PRESET_CONFIG_VER;
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    if ((st = nvfl.nvEncGetEncodePresetConfigEx(w->session, NV_ENC_CODEC_H264_GUID,
                                                NV_ENC_PRESET_P4_GUID,
                                                NV_ENC_TUNING_INFO_LOW_LATENCY,
                                                &pc)) != NV_ENC_SUCCESS) {
        LOG("NVENC: 取 preset 失败 status=%d", st);
        nv_close(w);
        return -1;
    }
    cfg = pc.presetCfg;
    cfg.gopLength = (uint32_t)fps * 2;              /* 与 AVSDK 的 i_gop 同量级 */
    cfg.frameIntervalP = 1;                         /* 无 B 帧，零重排延迟 */
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate = (uint32_t)kbps * 1000;
    cfg.rcParams.maxBitRate = (uint32_t)kbps * 1000;
    cfg.rcParams.zeroReorderDelay = 1;
    cfg.encodeCodecConfig.h264Config.idrPeriod = (uint32_t)fps * 2;
    cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;   /* 原实现每帧都带 SPS/PPS */
    cfg.encodeCodecConfig.h264Config.outputAUD = 1;
    cfg.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_AUTOSELECT;

    memset(&ip, 0, sizeof ip);
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
    ip.presetGUID = NV_ENC_PRESET_P4_GUID;
    ip.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    ip.encodeWidth = (uint32_t)wpx;
    ip.encodeHeight = (uint32_t)hpx;
    ip.darWidth = (uint32_t)wpx;
    ip.darHeight = (uint32_t)hpx;
    ip.frameRateNum = (uint32_t)fps;
    ip.frameRateDen = 1;
    ip.enablePTD = 1;
    ip.enableEncodeAsync = 0;                       /* 同步：编完即可读码流 */
    ip.encodeConfig = &cfg;
    if ((st = nvfl.nvEncInitializeEncoder(w->session, &ip)) != NV_ENC_SUCCESS) {
        LOG("NVENC: Initialize 失败 status=%d (%dx%d @%d)", st, wpx, hpx, fps);
        nv_close(w);
        return -1;
    }

    memset(&ib, 0, sizeof ib);
    ib.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    ib.width = (uint32_t)wpx;
    ib.height = (uint32_t)hpx;
    ib.bufferFmt = NV_ENC_BUFFER_FORMAT_IYUV;
    ib.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;
    if ((st = nvfl.nvEncCreateInputBuffer(w->session, &ib)) != NV_ENC_SUCCESS) {
        LOG("NVENC: 建输入缓冲失败 status=%d", st);
        nv_close(w);
        return -1;
    }
    w->inbuf = ib.inputBuffer;

    memset(&bb, 0, sizeof bb);
    bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    bb.size = (uint32_t)wpx * (uint32_t)hpx;        /* 4:2:0 一帧原始大小 */
    if (bb.size < (4u << 20))
        bb.size = 4u << 20;
    if ((st = nvfl.nvEncCreateBitstreamBuffer(w->session, &bb)) != NV_ENC_SUCCESS) {
        LOG("NVENC: 建码流缓冲失败 status=%d", st);
        nv_close(w);
        return -1;
    }
    w->outbuf = bb.bitstreamBuffer;
    w->out_cap = bb.size;
    w->w = wpx;
    w->h = hpx;
    w->fps = fps;
    w->kbps = kbps;
    w->frame_idx = 0;
    w->nv_on = 1;
    LOG("NVENC: 会话就绪 %dx%d @%d, %d kbps CBR, idr=%d, 码流缓冲 %u 字节",
        wpx, hpx, fps, kbps, fps * 2, bb.size);
    return 0;
}

/* 把 VideoFrame(I420) 拷进 NVENC 输入缓冲，编码一帧，返回码流。
 * 成功时 sz/ptr/qp 有效；失败返回 -1（调用方回落原实现）。 */
static int nv_encode(struct wrapped *w, VideoFrame *f, uint32_t *sz, void **ptr,
                     uint32_t *qp, int *is_idr)
{
    NVENCSTATUS st;
    NV_ENC_LOCK_INPUT_BUFFER lib;
    NV_ENC_PIC_PARAMS pp;
    NV_ENC_LOCK_BITSTREAM lb;
    uint32_t wpx, hpx, ypitch, uvpitch, hp;
    const uint8_t *py, *pu, *pv;
    uint8_t *dst;
    uint32_t rows;

    wpx = get32((char *)f + VF_OFF_W);
    hpx = get32((char *)f + VF_OFF_H);
    ypitch = get32((char *)f + VF_OFF_YPITCH);
    uvpitch = get32((char *)f + VF_OFF_UVPITCH);
    py = getptr((char *)f + VF_OFF_Y);
    pu = getptr((char *)f + VF_OFF_U);
    pv = getptr((char *)f + VF_OFF_V);
    if (py && pu && pv && (!wpx || !hpx || wpx > 8192 || hpx > 8192))
        return -1;                                /* 尺寸离谱，宁可回落 */
    if (!py || !pu || !pv || !wpx || !hpx)
        return -1;
    if (!ypitch)
        ypitch = wpx;
    if (!uvpitch)
        uvpitch = wpx / 2;

    if (!w->nv_on || wpx != (uint32_t)w->w || hpx != (uint32_t)w->h) {
        LOG("NVENC: 需要(重)开会话 帧=%ux%u 会话=%dx%d", wpx, hpx, w->w, w->h);
        if (nv_open(w, (int)wpx, (int)hpx, w->fps ? w->fps : 30,
                    w->kbps ? w->kbps : 2000))
            return -1;
    }
    if (w->encode_calls <= 3)
        LOG("NVENC: 开始编码 %ux%u ypitch=%u uvpitch=%u 平面=%p/%p/%p",
            wpx, hpx, ypitch, uvpitch, (void *)py, (void *)pu, (void *)pv);

    memset(&lib, 0, sizeof lib);
    lib.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lib.inputBuffer = w->inbuf;
    if ((st = nvfl.nvEncLockInputBuffer(w->session, &lib)) != NV_ENC_SUCCESS) {
        LOG("NVENC: 锁输入缓冲失败 status=%d", st);
        return -1;
    }
    w->pitch = lib.pitch;
    dst = lib.bufferDataPtr;
    /* NVENC 的 IYUV 输入缓冲：Y 平面用整行距，U/V 平面是【半行距】(pitch/2)。
     * 早先按整行距推进 U/V 会越界写（1920x1088 时写出 4.46MB 而缓冲只有 3.34MB），
     * 在 ppapi 里表现为进程被段错误打死。 */
    hp = w->pitch / 2;
    if (w->encode_calls <= 3)
        LOG("NVENC: 输入缓冲已锁 目标=%p pitch=%u (U/V 行距 %u)", (void *)dst, w->pitch, hp);
    for (rows = 0; rows < hpx; ++rows, dst += w->pitch, py += ypitch)
        memcpy(dst, py, wpx);
    for (rows = 0; rows < hpx / 2; ++rows, dst += hp, pu += uvpitch)
        memcpy(dst, pu, wpx / 2);
    for (rows = 0; rows < hpx / 2; ++rows, dst += hp, pv += uvpitch)
        memcpy(dst, pv, wpx / 2);
    nvfl.nvEncUnlockInputBuffer(w->session, w->inbuf);

    memset(&pp, 0, sizeof pp);
    pp.version = NV_ENC_PIC_PARAMS_VER;
    pp.inputWidth = wpx;
    pp.inputHeight = hpx;
    pp.inputPitch = w->pitch;
    pp.inputBuffer = w->inbuf;
    pp.outputBitstream = w->outbuf;
    pp.bufferFmt = NV_ENC_BUFFER_FORMAT_IYUV;
    pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pp.encodePicFlags = NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    if (w->force_idr || w->frame_idx == 0) {
        pp.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
        w->force_idr = 0;
    }
    if ((st = nvfl.nvEncEncodePicture(w->session, &pp)) != NV_ENC_SUCCESS) {
        LOG("NVENC: EncodePicture 失败 status=%d", st);
        return -1;
    }

    memset(&lb, 0, sizeof lb);
    lb.version = NV_ENC_LOCK_BITSTREAM_VER;
    lb.outputBitstream = w->outbuf;
    if ((st = nvfl.nvEncLockBitstream(w->session, &lb)) != NV_ENC_SUCCESS) {
        LOG("NVENC: LockBitstream 失败 status=%d", st);
        return -1;
    }
    *sz = lb.bitstreamSizeInBytes;
    *ptr = lb.bitstreamBufferPtr;
    *qp = lb.frameAvgQP;
    *is_idr = (lb.pictureType == NV_ENC_PIC_TYPE_IDR);
    nvfl.nvEncUnlockBitstream(w->session, w->outbuf);
    if (w->encode_calls <= 3)
        LOG("NVENC: 出流 %u 字节 qp=%u type=%d", *sz, *qp, (int)lb.pictureType);
    return (*sz && *ptr) ? 0 : -1;
}

/* ---------- vtable 覆盖后的实现 ---------- */

static int our_init(void *self, const void *param)
{
    struct wrapped *w = find_wrapped(self);
    init_fn orig;

    if (!w)
        return -1;
    orig = (init_fn)w->orig_vtable[VT_OFF_INIT];
    w->init_calls++;
    if (!w->dump_done) {
        w->dump_done = 1;
        LOG("Init(self=%p, EncParam=%p) 宽=%u 高=%u 码率=%u kbps 帧率=%u",
            self, param, get32((char *)param + EP_OFF_W), get32((char *)param + EP_OFF_H),
            get32((char *)param + EP_OFF_KBPS), get32((char *)param + EP_OFF_FPS));
        if (probe_dump)
            hexdump("  EncParam", param, 0x100);
    }
    /* 无论是否启用 NVENC，都先让原实现初始化好（回落时可直接用） */
    int r = orig(self, param);

    w->nv_dead = 0;
    w->nv_on = 0;
    if (nv_active && r >= 0) {
        w->kbps = (int)get32((char *)param + EP_OFF_KBPS);
        w->fps = (int)get32((char *)param + EP_OFF_FPS);
        w->w = w->h = 0;
        if (nv_open(w, (int)get32((char *)param + EP_OFF_W),
                    (int)get32((char *)param + EP_OFF_H), w->fps, w->kbps)) {
            LOG("NVENC: 初始化失败，本对象回落软件编码");
            w->nv_dead = 1;
        }
    }
    return r;
}

static int our_uninit(void *self)
{
    struct wrapped *w = find_wrapped(self);
    uninit_fn orig;

    if (!w)
        return -1;
    orig = (uninit_fn)w->orig_vtable[VT_OFF_UNINIT];
    nv_close(w);
    return orig(self);
}

static int our_encode(void *self, VideoFrame *frame, VideoPacket *packet, void *user)
{
    struct wrapped *w = find_wrapped(self);
    encode_fn orig;

    if (!w)
        return -1;
    orig = (encode_fn)w->orig_vtable[VT_OFF_ENCODE];

    /* 记录（只在采样窗口内） */
    if (w->encode_calls < 3) {
        LOG("DoEncode(self=%p frame=%p packet=%p user=%p) 第 %d 次【前】宽=%u 高=%u",
            self, (void *)frame, (void *)packet, user, w->encode_calls + 1,
            get32((char *)frame + VF_OFF_W), get32((char *)frame + VF_OFF_H));
        if (probe_dump) {
            hexdump("  VideoFrame ", frame, 0x80);
            hexdump("  VideoPacket", packet, 0x80);
        }
    }
    w->encode_calls++;

    if (w->nv_on && !w->nv_dead && w->have_cb) {
        uint32_t sz = 0, qp = 0;
        void *data = NULL;
        int is_idr = 0;

        if (nv_encode(w, frame, &sz, &data, &qp, &is_idr) == 0) {
            void *pk = packet;
            long long idx = w->frame_idx++;

            put32((char *)pk + PKT_OFF_IDX, (uint32_t)idx);
            put32((char *)pk + PKT_OFF_QP, qp);
            put32((char *)pk + PKT_OFF_LEN, sz);
            put64((char *)pk + PKT_OFF_DATA, data);
            if (w->encode_calls <= 4)
                LOG("NVENC 出帧 idx=%lld %s %u 字节 qp=%u -> 回调 ctx=%p cb=%p",
                    idx, is_idr ? "IDR" : "P", sz, qp, w->cb_ctx, w->cb0);
            ((pkt_cb_fn)w->cb0)(w->cb_ctx, &pk, w->cb1);
            if (w->encode_calls <= 4)
                LOG("NVENC: 回调已返回");
            return 0;
        }
        LOG("NVENC: 本帧失败，之后回落原实现");
        w->nv_dead = 1;
        nv_close(w);
    } else if (w->encode_calls <= 3) {
        LOG("NVENC: 未启用本对象（nv_on=%d dead=%d have_cb=%d）-> 走原实现",
            w->nv_on, w->nv_dead, w->have_cb);
    }

    {
        int r = orig(self, frame, packet, user);

        if (w->encode_calls <= 3) {
            LOG("原实现 DoEncode 返回 %d【后】packet 前 0x60：", r);
            if (probe_dump)
                hexdump("  VideoPacket", packet, 0x60);
        }
        return r;
    }
}

/* ---------- 回调旁听（转发壳） ---------- */

static void *g_cb_tramp;
static void *g_cb_ctx_seen;
static int g_cb_calls;
static void *g_cb_tramp_slot_user;
static void *g_cb_pair[2];

static void cb_tramp(void *ctx, void **pp_packet, void *user)
{
    (void)user;
    g_cb_ctx_seen = ctx;
    if (g_cb_calls < 3) {
        void *pk = pp_packet ? *pp_packet : NULL;
        LOG("原实现回调第 %d 次: ctx=%p *pp_packet=%p", g_cb_calls + 1, ctx, pk);
        if (pk && probe_dump) {
            hexdump("  cb packet", pk, 0x40);
            void *d = getptr((char *)pk + PKT_OFF_DATA);
            uint32_t n = get32((char *)pk + PKT_OFF_LEN);
            LOG("  -> data=%p len=%u idx=%u", d, n, get32((char *)pk + PKT_OFF_IDX));
            if (d && n > 0 && n < (1u << 22))
                hexdump("  bitstream[0..0x20]", d, 0x20);
        }
    }
    g_cb_calls++;
    if (g_cb_tramp_slot_user) {
        void **slots = (void **)g_cb_tramp_slot_user;
        pkt_cb_fn real = (pkt_cb_fn)slots[0];

        if (real)
            real(ctx, pp_packet, slots[1]);
    }
}

static int our_setcb(void *self, void *a, void *cb)
{
    struct wrapped *w = find_wrapped(self);
    setcb_fn orig;
    void **in = (void **)cb;

    if (!w)
        return -1;
    orig = (setcb_fn)w->orig_vtable[VT_OFF_SETCB];

    if (in) {
        /* 记住原始回调，NVENC 出帧时直接用它 */
        w->cb_ctx = a;
        w->cb0 = in[0];
        w->cb1 = in[1];
        w->have_cb = in[0] != NULL;
        if (!g_cb_tramp) {
            g_cb_pair[0] = in[0];
            g_cb_pair[1] = in[1];
            g_cb_tramp_slot_user = g_cb_pair;
            g_cb_tramp = (void *)cb_tramp;
        }
    }
    if (!w->cb_logged) {
        w->cb_logged = 1;
        LOG("SetCodecCallback(self=%p, a=%p, cb=%p) cb[0]=%p cb[1]=%p",
            self, a, cb, in ? in[0] : NULL, in ? in[1] : NULL);
    }
    /* 传给原实现的是我们的转发壳：原实现照常调用，我们旁听（回落时也能看到） */
    if (in && g_cb_tramp) {
        void *sub[2] = { g_cb_tramp, in[1] };

        return orig(self, a, (void *)sub);
    }
    return orig(self, a, cb);
}

static int our_setparam(void *self, const char *name, void *data, uint32_t *len)
{
    struct wrapped *w = find_wrapped(self);
    setparam_fn orig;
    uint32_t v = 0;

    if (!w)
        return -1;
    orig = (setparam_fn)w->orig_vtable[VT_OFF_SETPARAM];
    if (data && len && *len >= 4)
        v = get32(data);

    if (!strcmp(name, "frame_type") && v == 1)
        w->force_idr = 1;                       /* 上层要一个关键帧 */
    if (!strcmp(name, "bitrate") && v)
        w->kbps = (int)v;
    if (!strcmp(name, "fps") && v)
        w->fps = (int)v;
    if (!w->setparam_logged) {
        w->setparam_logged = 1;
        LOG("SetParam(name=%s, value=%u) nv_on=%d", name, v, w->nv_on);
    }
    return orig(self, name, data, len);
}

/* ---------- 函数入口 inline hook ---------- */

static long page_size(void)
{
    return sysconf(_SC_PAGESIZE);
}

static int make_range_writable(void *p, size_t n, int prot)
{
    uintptr_t pg = (uintptr_t)page_size();
    uintptr_t start = (uintptr_t)p & ~(pg - 1);
    size_t len = (uintptr_t)p + n - start;

    return mprotect((void *)start, len, prot);
}

static int our_create_h264(void **out);

static int install_hook(uintptr_t base)
{
    unsigned char *fn = (unsigned char *)(base + OFF_CREATE_H264);
    long pg = page_size();
    unsigned char *tramp;

    if (memcmp(fn, want_create16, HOOK_LEN)) {
        LOG("CreateH264Encoder 头 14 字节与预期不符（QQ 更新了？），不挂钩");
        return -1;
    }
    tramp = mmap(NULL, (size_t)pg, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        LOG("mmap 蹦床失败: %s", strerror(errno));
        return -1;
    }
    memcpy(tramp, fn, HOOK_LEN);
    tramp[HOOK_LEN + 0] = 0xff;
    tramp[HOOK_LEN + 1] = 0x25;
    *(uint32_t *)(tramp + HOOK_LEN + 2) = 0;
    *(uint64_t *)(tramp + HOOK_LEN + 6) = (uint64_t)(fn + HOOK_LEN);
    __builtin___clear_cache((char *)tramp, (char *)tramp + HOOK_LEN + HOOK_LEN);

    if (make_range_writable(fn, HOOK_LEN, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        LOG("mprotect 代码段失败: %s", strerror(errno));
        return -1;
    }
    fn[0] = 0xff;
    fn[1] = 0x25;
    *(uint32_t *)(fn + 2) = 0;
    *(uint64_t *)(fn + 6) = (uint64_t)&our_create_h264;
    make_range_writable(fn, HOOK_LEN, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)fn, (char *)fn + HOOK_LEN);

    trampoline = tramp;
    LOG("已挂钩 CreateH264Encoder@%p（蹦床 %p），NVENC=%s",
        (void *)fn, (void *)tramp, nv_active ? "启用" : "仅旁观");
    return 0;
}

static int our_create_h264(void **out)
{
    int r;
    struct wrapped *w = NULL;

    if (!trampoline) {
        LOG("trampoline 未就绪，拒绝调用");
        return -1;
    }
    r = ((create_fn)trampoline)(out);
    if (r < 0 || !out || !*out)
        return r;

    pthread_mutex_lock(&wrap_lock);
    for (int i = 0; i < MAX_WRAPPED; ++i)
        if (!wrapped[i].obj) {
            w = &wrapped[i];
            break;
        }
    pthread_mutex_unlock(&wrap_lock);
    if (!w) {
        LOG("包装槽位用完，%p 不包装", *out);
        return r;
    }

    void **orig = *(void ***)(*out);

    memset(w, 0, sizeof *w);
    w->obj = *out;
    w->orig_vtable = orig;
    w->vtable = malloc(sizeof(void *) * VT_ENTRIES);
    if (!w->vtable)
        return r;
    memcpy(w->vtable, orig, sizeof(void *) * VT_ENTRIES);
    w->vtable[VT_OFF_INIT] = (void *)our_init;
    w->vtable[VT_OFF_UNINIT] = (void *)our_uninit;
    w->vtable[VT_OFF_SETCB] = (void *)our_setcb;
    w->vtable[VT_OFF_SETPARAM] = (void *)our_setparam;
    w->vtable[VT_OFF_ENCODE] = (void *)our_encode;
    *(void ***)(*out) = w->vtable;

    LOG("包装编码器对象 %p（原 vtable %p）", *out, (void *)orig);
    return r;
}

/* ---------- 启动 ---------- */

static void *hook_thread(void *arg)
{
    (void)arg;

    for (int i = 0; i < 3000; ++i) {           /* 最多等 10 分钟 */
        uintptr_t base = 0;

        dl_iterate_phdr(find_avsdk, &base);
        if (base) {
            avsdk_base = base;
            if (install_hook(base) == 0)
                hooked = 1;
            return NULL;
        }
        usleep(200 * 1000);
    }
    LOG("等 AVSDK 超时，未挂钩");
    return NULL;
}

__attribute__((constructor))
static void qq_nvenc_init(void)
{
    pthread_t th;

    if (!flag_on("QQ_NVENC", 0))
        return;
    probe_dump = flag_on("QQ_NVENC_PROBE_DUMP", 1);
    nv_active = flag_on("QQ_NVENC_ACTIVE", 0);
    if (!is_ppapi_process())
        return;

    LOG("ppapi 进程，等待 AVSDK 加载（QQ_NVENC=1, ACTIVE=%d）", nv_active);
    if (pthread_create(&th, NULL, hook_thread, NULL) == 0)
        pthread_detach(th);
}
