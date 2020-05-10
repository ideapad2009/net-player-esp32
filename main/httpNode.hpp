#include <sys/unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include "esp_log.h"
#include "errno.h"
#include "esp_system.h"
#include <esp_http_client.h>
#include <strings.h>
#include "ringbuf.hpp"
#include "queue.hpp"
#include "utils.hpp"
#include "audioNode.hpp"
#include "playlist.hpp"

class HttpNode: public AudioNodeWithTask
{
protected:
    enum { kPollTimeoutMs = 1000, kClientBufSize = 512, kReadSize = 1024,
           kStackSize = 3600 };
    enum: uint16_t {
        kHttpEventType = kEventLastGeneric + 1,
        kEventOnRequest,
        kEventNewTrack,
        kEventNoMoreTracks
    };
    enum: uint8_t { kCommandSetUrl = AudioNodeWithTask::kCommandLast + 1,
                    kCommandNotifyFlushed };
    // Read mode dictates how the pullData() caller behaves. Since it may
    // need to wait for the read mode to change to a specific value, the enum values
    // are flags
    enum ReadMode: uint8_t { kReadAllowed = 0, kReadPrefill = 1, kReadFlushReq = 2 };
    enum: uint8_t { kEvtReadModeChange = kEvtLast << 1 };
    char* mUrl = nullptr;
    StreamFormat mStreamFormat;
    esp_http_client_handle_t mClient = nullptr;
    bool mAutoNextTrack = true; /* connect next track without open/close */
    Playlist mPlaylist; /* media playlist */
    size_t mStackSize;
    RingBuf mRingBuf;
    volatile ReadMode mReadMode = kReadPrefill;
    int mPrefillAmount;
    int64_t mBytesTotal;
    int mRecvSize = 2048;
    static esp_err_t httpHeaderHandler(esp_http_client_event_t *evt);
    static CodecType codecFromContentType(const char* content_type);
    bool isPlaylist();
    bool createClient();
    bool parseContentType();
    bool parseResponseAsPlaylist();
    void doSetUrl(const char* url);
    bool connect(bool isReconnect=false);
    void disconnect();
    void destroyClient();
    bool nextTrack();
    void recv();
    void send();
    void setReadMode(ReadMode mode);
    int8_t waitReadModeChange(int msTimeout);
    void nodeThreadFunc();
    virtual bool dispatchCommand(Command &cmd);
    virtual void doStop();
public:
    HttpNode(size_t bufSize);
    virtual ~HttpNode();
    virtual Type type() const { return kTypeHttpIn; }
    virtual StreamError pullData(DataPullReq &dp, int timeout);
    virtual void confirmRead(int size);
    void setUrl(const char* url);
    bool isConnected() const;
};
