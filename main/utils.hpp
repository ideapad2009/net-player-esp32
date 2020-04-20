#ifndef NETPLAYER_UTILS_HPP
#define NETPLAYER_UTILS_HPP

#include <esp_http_server.h>
#include <vector>
#include <stdarg.h>
#include <audio_element.h>
#include <http_stream.h>
#include <i2s_stream.h>
#include <esp_log.h>
#include <memory>

#define myassert(cond) if (!(cond)) { \
    ESP_LOGE("RINGBUFFER", "Assertion failed: %s at %s:%d", #cond, __FILE__, __LINE__); \
    *((int*)nullptr) = 0; }

#define TRACE ESP_LOGI("TRC", "%s:%d", __FILE__, __LINE__);

template<typename T>
struct BufPtr
{
protected:
    T* mPtr;
    BufPtr() {} // mPtr remains uninitialized, only for derived classes
public:
    T* ptr() { return mPtr; }
    BufPtr(T* ptr): mPtr(ptr){}
    BufPtr(BufPtr<T>&& other) {
        mPtr = other.mPtr;
        other.mPtr = nullptr;
    }
    ~BufPtr() {
        if (mPtr) {
            ::free(mPtr);
        }
    }
    void free() {
        if (mPtr) {
            ::free(mPtr);
        }
    }
    void freeAndReset(T* newPtr) {
        free();
        mPtr = newPtr;
    }
    void* release() {
        auto ret = mPtr;
        mPtr = nullptr;
        return ret;
    }
};

class DynBuffer: public std::vector<char>
{
public:
    DynBuffer(size_t allocSize) { reserve(allocSize); }
    int vprintf(const char *fmt, va_list args)
    {
        size_t avail = capacity() - size();
        if (avail < 2) {
            reserve(capacity() + 64);
            avail = capacity() - size();
        }
        auto oldSize = size();
        resize(capacity());
        for (;;) {
            int num = ::vsnprintf(data() + oldSize, avail, fmt, args);
            if (num < 0) {
                ESP_LOGE("DynBuffer", "printf: vsnprintf() returned error");
                return num;
            } else if (num < size()) {
                resize(oldSize + num + 1);
                return num;
            }
        }
    }
    int printf(const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        int ret = vprintf(fmt, args);
        va_end(args);
        return ret;
    }
};

char* binToHex(const uint8_t* data, size_t len, char* str);

extern const char* _utils_hexDigits;
template <typename T>
char* numToHex(T val, char* str)
{
    const uint8_t* start = (const uint8_t*) &val;
    const uint8_t* data = start + sizeof(T) - 1;
    while (data >= start) {
        *(str++) = _utils_hexDigits[*data >> 4];
        *(str++) = _utils_hexDigits[*data & 0x0f];
        data--;
    }
    *str = 0;
    return str;
}

bool unescapeUrlParam(char* str, size_t len);

uint8_t hexDigitVal(char digit);
long strToInt(const char* str, size_t len, long defVal, int base=10);

class KeyValParser: public BufPtr<char>
{
public:
    struct Substring
    {
        char* str;
        size_t len;
        Substring(char* aStr, size_t aLen): str(aStr), len(aLen) {}
        Substring() {}
        operator bool() const { return str != nullptr; }
        void trimSpaces();
        long toInt(long defVal, int base=10) const { return strToInt(str, len, defVal, base); }
    };
    struct KeyVal
    {
        Substring key;
        Substring val;
    };
protected:
    size_t mSize;
    std::vector<KeyVal> mKeyVals;
    KeyValParser() {} // ctor to inherit when derived class has its own initialization
public:
    enum Flags: uint8_t { kUrlUnescape = 1, kTrimSpaces = 2 };
    const std::vector<KeyVal>& keyVals() const { return mKeyVals; }
    KeyValParser(char* str, size_t len): BufPtr(str), mSize(len) {}
    bool parse(char pairDelim, char keyValDelim, Flags flags);
    Substring strVal(const char* name);
    long intVal(const char* name, long defVal);
};

class UrlParams: public KeyValParser
{
public:
    UrlParams(httpd_req_t* req);
    Substring strParam(const char* name) { return strVal(name); }
    long intParam(const char* name, long defVal) { return intVal(name, defVal); }
};

const char* getUrlFile(const char* url);

static inline TaskHandle_t currentTaskHandle()
{
    extern volatile void * volatile pxCurrentTCB;
    return (TaskHandle_t)pxCurrentTCB;
}

class Mutex
{
    SemaphoreHandle_t mMutex;
    StaticSemaphore_t mMutexMem;
public:
    Mutex() {
        mMutex = xSemaphoreCreateRecursiveMutexStatic(&mMutexMem);
    }
    void lock() { xSemaphoreTakeRecursive(mMutex, portMAX_DELAY); }
    void unlock() { xSemaphoreGiveRecursive(mMutex); }
};

class MutexLocker
{
    Mutex& mMutex;
public:
    MutexLocker(Mutex& aMutex): mMutex(aMutex) { mMutex.lock(); }
    ~MutexLocker() { mMutex.unlock(); }
};

class MutexUnlocker
{
    Mutex& mMutex;
public:
    MutexUnlocker(Mutex& aMutex): mMutex(aMutex) { mMutex.unlock(); }
    ~MutexUnlocker() { mMutex.lock(); }
};

template <class F, bool isOneShot>
struct TimerCtx
{
    typedef TimerCtx<F, isOneShot> Self;
    F mUserCb;
    esp_timer_handle_t mTimer;
    virtual ~TimerCtx() { esp_timer_delete(mTimer); }
    TimerCtx(F&& userCb): mUserCb(std::forward<F>(userCb)) {}
    static void cFunc(void* ctx)
    {
        auto self = static_cast<Self*>(ctx);
        self->mUserCb();
        if (isOneShot) {
            delete self;
        }
    }
};

template<class F>
void setTimeout(uint32_t ms, F&& cb)
{
    esp_timer_create_args_t args = {};
    args.dispatch_method = ESP_TIMER_TASK;
    auto ctx = new TimerCtx<F, true>(std::forward<F>(cb));
    args.callback = ctx->cFunc;
    args.arg = ctx;
    args.name = "userOneshotTimer";
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ctx->mTimer = timer;
    ESP_ERROR_CHECK(esp_timer_start_once(timer, ms * 1000));
}

namespace std {
template<>
    struct default_delete<FILE>
    {
        void operator()(FILE* file) const { fclose(file); }
    };
}

int16_t currentCpuFreq();

extern "C" const i2s_stream_cfg_t myI2S_STREAM_INTERNAL_DAC_CFG_DEFAULT;
extern "C" const http_stream_cfg_t myHTTP_STREAM_CFG_DEFAULT;

#endif
