#include <sys/unistd.h>
#include <sys/stat.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"
#include "errno.h"
#include "esp_system.h"
#include <esp_http_client.h>
#include <strings.h>
#include "ringbuf.hpp"
#include "queue.hpp"
#include "utils.hpp"
#include "httpNode.hpp"

static const char *TAG = "HTTP_NODE";

CodecType HttpNode::codecFromContentType(const char* content_type)
{
    if (strcasecmp(content_type, "mp3") == 0 ||
        strcasecmp(content_type, "audio/mp3") == 0 ||
        strcasecmp(content_type, "audio/mpeg") == 0 ||
        strcasecmp(content_type, "binary/octet-stream") == 0 ||
        strcasecmp(content_type, "application/octet-stream") == 0) {
        return kCodecMp3;
    }
    if (strcasecmp(content_type, "audio/aac") == 0 ||
        strcasecmp(content_type, "audio/x-aac") == 0 ||
        strcasecmp(content_type, "audio/mp4") == 0 ||
        strcasecmp(content_type, "audio/aacp") == 0 ||
        strcasecmp(content_type, "video/MP2T") == 0) {
        return kCodecAac;
    }
    if (strcasecmp(content_type, "application/ogg") == 0) {
        return kCodecOgg;
    }
    if (strcasecmp(content_type, "audio/wav") == 0) {
        return kCodecWav;
    }
    if (strcasecmp(content_type, "audio/opus") == 0) {
        return kCodecOpus;
    }
    if (strcasecmp(content_type, "audio/x-mpegurl") == 0 ||
        strcasecmp(content_type, "application/vnd.apple.mpegurl") == 0 ||
        strcasecmp(content_type, "vnd.apple.mpegURL") == 0) {
        return kPlaylistM3u8;
    }
    if (strncasecmp(content_type, "audio/x-scpls", strlen("audio/x-scpls")) == 0) {
        return kPlaylistPls;
    }
    return kCodecUnknown;
}

bool HttpNode::isPlaylist()
{
    auto codec = mStreamFormat.codec;
    if (codec == kPlaylistM3u8 || codec == kPlaylistPls) {
        return true;
    }
    char *dot = strrchr(mUrl, '.');
    return (dot && ((strcasecmp(dot, ".m3u") == 0 || strcasecmp(dot, ".m3u8") == 0)));
}

void HttpNode::doSetUrl(const char *url)
{
    ESP_LOGI(mTag, "Setting url to %s", url);
    if (mUrl) {
        free(mUrl);
    }
    mUrl = strdup(url);
    if (mClient) {
        esp_http_client_set_url(mClient, mUrl); // do it here to avoid keeping reference to the old, freed one
    }
}
bool HttpNode::createClient()
{
    assert(!mClient);
    esp_http_client_config_t cfg = {};
    cfg.url = mUrl;
    cfg.event_handler = httpHeaderHandler;
    cfg.user_data = this;
    cfg.timeout_ms = kPollTimeoutMs;
    cfg.buffer_size = kClientBufSize;
    cfg.method = HTTP_METHOD_GET;

    mClient = esp_http_client_init(&cfg);
    if (!mClient)
    {
        ESP_LOGE(TAG, "Error creating http client, probably out of memory");
        return false;
    };
    esp_http_client_set_header(mClient, "User-Agent", "curl/7.65.3");
    esp_http_client_set_header(mClient, "Icy-MetaData", "1");
    return true;
}

esp_err_t HttpNode::httpHeaderHandler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "hdr: '%s': '%s'", evt->header_key, evt->header_value);

    auto self = static_cast<HttpNode*>(evt->user_data);
    auto key = evt->header_key;
    if (strcasecmp(key, "Content-Type") == 0) {
        self->mStreamFormat.codec = self->codecFromContentType(evt->header_value);
        ESP_LOGI(TAG, "Parsed content-type '%s' as %s", evt->header_value,
            self->mStreamFormat.codecTypeStr());
    } else if (strcasecmp(key, "icy-metaint") == 0) {
        auto self = static_cast<HttpNode*>(evt->user_data);
        self->mIcyInterval = atoi(evt->header_value);
        self->mIcyCtr = 0;
        ESP_LOGI(TAG, "Response contains ICY metadata with interval %d", self->mIcyInterval);
    } else if (strcasecmp(key, "icy-name") == 0) {
        MutexLocker locker(self->icyInfo.mutex);
        self->icyInfo.mStaName.freeAndReset(strdup(evt->header_value));
    } else if (strcasecmp(key, "icy-description") == 0) {
        MutexLocker locker(self->icyInfo.mutex);
        self->icyInfo.mStaDesc.freeAndReset(strdup(evt->header_value));
    } else if (strcasecmp(key, "icy-genre") == 0) {
        MutexLocker locker(self->icyInfo.mutex);
        self->icyInfo.mStaGenre.freeAndReset(strdup(evt->header_value));
    } else if (strcasecmp(key, "icy-url") == 0) {
        MutexLocker locker(self->icyInfo.mutex);
        self->icyInfo.mStaUrl.freeAndReset(strdup(evt->header_value));
    }
    return ESP_OK;
}

bool HttpNode::connect(bool isReconnect)
{
    myassert(mState != kStateStopped);
    if (!mUrl) {
        ESP_LOGE(mTag, "connect: URL has not been set");
        return false;
    }

    ESP_LOGI(mTag, "Connecting to '%s'...", mUrl);
    if (!isReconnect) {
        ESP_LOGI(mTag, "connect: Waiting for buffer to drain...");
        // Wait till buffer is drained before changing format descriptor
        if (mWaitingPrefill && mRingBuf.hasData()) {
            ESP_LOGW(mTag, "Connect: Read state is kReadPrefill, but the buffer should be drained, allowing read");
            setWaitingPrefill(false);
        }
        bool ret = mRingBuf.waitForEmpty();
        if (!ret) {
            return false;
        }
        ESP_LOGI(mTag, "connect: Buffer drained");
        mStreamFormat.reset();
        mBytePos = 0;
    }

    if (!mClient) {
        if (!createClient()) {
            ESP_LOGE(mTag, "connect: Error creating http client");
            return false;
        }
    }
    // request IceCast stream metadata
    clearAllIcyInfo();

    if (mBytePos) { // we are resuming, send position
        char rang_header[32];
        snprintf(rang_header, 32, "bytes=%lld-", mBytePos);
        esp_http_client_set_header(mClient, "Range", rang_header);
    }
    sendEvent(kEventConnecting, nullptr, isReconnect);

    for (int tries = 0; tries < 4; tries++) {
        auto err = esp_http_client_open(mClient, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open http stream, error %s", esp_err_to_name(err));
            continue;
        }

        mContentLen = esp_http_client_fetch_headers(mClient);

        int status_code = esp_http_client_get_status_code(mClient);
        ESP_LOGI(TAG, "Connected to '%s': http code: %d, content-length: %u",
            mUrl, status_code, mContentLen);

        if (status_code == 301 || status_code == 302) {
            ESP_LOGI(TAG, "Following redirect...");
            esp_http_client_set_redirection(mClient);
            continue;
        }
        else if (status_code != 200 && status_code != 206) {
            if (status_code < 0) {
                ESP_LOGE(mTag, "Error connecting, will retry");
                continue;
            }
            ESP_LOGE(mTag, "Non-200 response code %d", status_code);
            return false;
        }
        ESP_LOGI(TAG, "Checking if response is a playlist");
        if (parseResponseAsPlaylist()) {
            ESP_LOGI(TAG, "Response parsed as playlist");
            auto url = mPlaylist.getNextTrack();
            if (!url) {
                ESP_LOGE(TAG, "Response is a playlist, but couldn't obtain an url from it");
                return false;
            }
            doSetUrl(url);
            continue;
        }
        if (!mIcyInterval) {
            ESP_LOGW(TAG, "Source does not send ShoutCast metadata");
        }
        {
            MutexLocker locker(icyInfo.mutex);
            if (!icyInfo.mStaUrl) {
                icyInfo.mStaUrl.freeAndReset(strdup(mUrl));
            }
        }
        sendEvent(kEventConnected, nullptr, isReconnect);
        return true;
    }
    return false;
}

bool HttpNode::parseResponseAsPlaylist()
{
    if (!isPlaylist()) {
        ESP_LOGI(TAG, "Content length and url don't looke like a playlist");
        return false;
    }
    std::unique_ptr<char, decltype(::free)*> buf((char*)malloc(mContentLen), ::free);
    int bufLen = mContentLen;
    for (int retry = 0; retry < 4; retry++) {
        if (!buf.get()) {
            ESP_LOGE(TAG, "Out of memory allocating buffer for playlist download");
            return true; // return empty playlist
        }
        int rlen = esp_http_client_read(mClient, buf.get(), mContentLen);
        if (rlen < 0) {
            disconnect();
            connect();
            if (mContentLen != bufLen) {
                buf.reset((char*)realloc(buf.get(), mContentLen));
            }
            continue;
        }
        buf.get()[rlen] = 0;
        mPlaylist.load(buf.get());
        return true;
    }
    return true;
}

void HttpNode::disconnect()
{
    mPlaylist.clear();
    destroyClient();
}
void HttpNode::destroyClient()
{
    if (!mClient) {
        return;
    }
    esp_http_client_close(mClient);
    esp_http_client_cleanup(mClient);
    mClient = NULL;
}

bool HttpNode::isConnected() const
{
    return mClient != nullptr;
}

bool HttpNode::nextTrack()
{
    if (!mAutoNextTrack) {
        return false;
    }
    auto url = mPlaylist.getNextTrack();
    if (!url) {
        return false;
    }
    doSetUrl(url);
    return true;
}

void HttpNode::recv()
{
    for(;;) { // retry with next playlist track
        for (int retries = 0; retries < 4; retries++) { // retry net errors
            char* buf;
            auto bufSize = mRingBuf.getWriteBuf(buf, kReadSize);

            if (bufSize < 0) { // command queued
                return;
            }
            int rlen;
            for (;;) { // periodic timeout - check abort request flag
                rlen = esp_http_client_read(mClient, buf, bufSize);
                if (rlen <= 0) {
                    if (errno == ETIMEDOUT) {
                        return;
                    }
                    ESP_LOGW(TAG, "Error receiving http stream, errno: %d, contentLen: %u, rlen = %d",
                        errno, mContentLen, rlen);
                }
                break;
            }
            if (rlen > 0) {
                if (mIcyInterval) {
                    rlen = icyProcessRecvData(buf, rlen);
                }
                mRingBuf.commitWrite(rlen);
                // First commit the write, only after that record to SD card,
                // to avoid blocking the stream consumer
                // Note: The buffer is still valid, even if it has been consumed
                // before we reach the next line - ringbuf consumers are read-only
                if (mRecorder) {
                    mRecorder->onData(buf, rlen);
                }
                mBytePos += rlen;
                //ESP_LOGI(TAG, "Received %d bytes, wrote to ringbuf (%d)", rlen, mRingBuf.totalDataAvail());
                //TODO: Implement IceCast metadata support
                if (mWaitingPrefill && mRingBuf.totalDataAvail() >= mPrefillAmount) {
                    setWaitingPrefill(false);
                }
                return;
            }
            // even though len == 0 means graceful disconnect, i.e.
            //track end => should go to next track, this often happens when
            // network lags and stream sender aborts sending to us
            // => we should reconnect.
            ESP_LOGW(TAG, "Reconnecting and retrying...");
            destroyClient(); // just in case
            connect(true);
        }
        // network retry gave up
        if (!nextTrack()) {
            setState(kStatePaused);
            sendEvent(kEventNoMoreTracks, nullptr, 0);
            return;
        }
        // Try next track
        mBytePos = 0;
        sendEvent(kEventNextTrack, mUrl, 0);
        destroyClient(); // just in case
        connect();
    }
}
int HttpNode::icyProcessRecvData(char* buf, int rlen)
{
    if (mIcyRemaining) { // we are receiving metadata
        MutexLocker locker(icyInfo.mutex);
        auto& icyMeta = icyInfo.mIcyMetaBuf;
        myassert(icyMeta.buf());
        auto metaLen = std::min(rlen, (int)mIcyRemaining);
        icyMeta.append(buf, metaLen);
        mIcyRemaining -= metaLen;
        if (mIcyRemaining <= 0) { // meta data complete
            myassert(mIcyRemaining == 0);
            icyMeta.appendChar(0);
            mIcyCtr = rlen - metaLen;
            if (mIcyCtr > 0) {
                // there is stream data after end of metadata
                memmove(buf, buf + metaLen, mIcyCtr);
            } else { // no stream data after meta
                myassert(mIcyCtr == 0);
            }
            icyParseMetaData();
            return mIcyCtr;
        } else { // metadata continues in next chunk
            return 0; // all this chunk was metadata
        }
    } else { // not receiving metadata
        if (mIcyCtr + rlen <= mIcyInterval) {
            mIcyCtr += rlen;
            return rlen; // no metadata in buffer
        }
        // metadata starts somewhere in our buffer
        int metaOffset = mIcyInterval - mIcyCtr;
        auto metaStart = buf + metaOffset;
        int metaSize = (*metaStart << 4) + 1;
        mIcyRemaining = metaSize; // includes the length byte
        int metaChunkSize = std::min(rlen - metaOffset, metaSize);

        MutexLocker locker(icyInfo.mutex);
        auto& icyMeta = icyInfo.mIcyMetaBuf;
        if (metaSize > 1) {
            icyMeta.clear();
            icyMeta.ensureFreeSpace(mIcyRemaining); // includes terminating null
            if (metaChunkSize > 1) {
                icyMeta.append(metaStart+1, metaChunkSize-1);
            }
        }
        mIcyRemaining -= metaChunkSize;
        if (mIcyRemaining > 0) { // metadata continues in next chunk
            mIcyCtr = 0;
            return metaOffset;
        }
        // meta starts and ends in our buffer
        myassert(mIcyRemaining == 0);
        int remLen = rlen - metaOffset - metaSize;
        mIcyCtr = remLen;
        if (metaSize > 1) {
            icyMeta.appendChar(0);
            icyParseMetaData();
        }
        if (remLen > 0) { // we have stream data after the metadata
            memmove(metaStart, metaStart + metaSize, remLen);
            return metaOffset + remLen;
        } else {
            return metaOffset; // no stream data after metadata
        }
    }
}
void HttpNode::icyParseMetaData()
{
    const char kStreamTitlePrefix[] = "StreamTitle='";
    auto& icyMetaBuf = icyInfo.mIcyMetaBuf;
    auto start = strstr(icyMetaBuf.buf(), kStreamTitlePrefix);
    if (!start) {
        ESP_LOGW(TAG, "ICY parse error: StreamTitle= string not found");
        return;
    }
    start += sizeof(kStreamTitlePrefix)-1; //sizeof(kStreamTitlePreix) incudes the terminating NULL
    auto end = strchr(start, ';');
    int titleSize;
    if (!end) {
        ESP_LOGW(TAG, "ICY parse error: Closing quote of StreamTitle not found");
        titleSize = icyMetaBuf.dataSize() - sizeof(kStreamTitlePrefix) - 1;
    } else {
        end--; // move to closing quote
        titleSize = end - start;
    }
    memmove(icyMetaBuf.buf(), start, titleSize);
    icyMetaBuf[titleSize] = 0;
    icyMetaBuf.setDataSize(titleSize + 1);
    ESP_LOGW(TAG, "Track title changed to: '%s'", icyMetaBuf.buf());
    sendEvent(kEventTrackInfo, icyMetaBuf.buf(), icyMetaBuf.dataSize());
    if (mRecorder && mBytePos) {
        mRecorder->onNewTrack(icyMetaBuf.buf(), mStreamFormat);
    }
}

void HttpNode::setUrl(const char* url)
{
    if (!mTaskId) {
        doSetUrl(url);
    } else {
        ESP_LOGI(mTag, "Posting setUrl command");
        mCmdQueue.post(kCommandSetUrl, strdup(url));
    }
}

bool HttpNode::dispatchCommand(Command &cmd)
{
    if (AudioNodeWithTask::dispatchCommand(cmd)) {
        return true;
    }
    switch(cmd.opcode) {
    case kCommandSetUrl:
        destroyClient();
        doSetUrl((const char*)cmd.arg);
        free(cmd.arg);
        cmd.arg = nullptr;
        mRingBuf.clear();
        mFlushRequested = true; // request flush along the pipeline
        setWaitingPrefill(true);
        setState(kStateRunning);
        ESP_LOGI(TAG, "Url set, switched to running state");
        break;
    default: return false;
    }
    return true;
}

void HttpNode::nodeThreadFunc()
{
    ESP_LOGI(TAG, "Task started");
    mRingBuf.clearStopSignal();
    for (;;) {
        processMessages();
        if (mTerminate) {
            return;
        }
        myassert(mState == kStateRunning);
        if (!isConnected()) {
            if (!connect()) {
                setState(kStatePaused);
                continue;
            }
        }
        while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
            recv(); // retries and goes to new playlist track
        }
    }
}

void HttpNode::doStop()
{
    mRingBuf.setStopSignal();
}

HttpNode::~HttpNode()
{
    stop();
    destroyClient();
    clearAllIcyInfo();
}

HttpNode::HttpNode(size_t bufSize)
: AudioNodeWithTask("node-http", kStackSize), mRingBuf(bufSize),
  mPrefillAmount(bufSize * 3 / 4)
{
}

AudioNode::StreamError HttpNode::pullData(DataPullReq& dp, int timeout)
{
    ElapsedTimer tim;
    if (mFlushRequested) {
        mFlushRequested = false;
        return kStreamFlush;
    }
    while (mWaitingPrefill) {
        auto ret = waitPrefillChange(timeout);
        if (ret < 0) {
            return kStreamStopped;
        } else if (ret == 0) {
            return kTimeout;
        }
    }
    timeout -= tim.msElapsed();
    if (timeout <= 0) {
        return kTimeout;
    }
    if (!dp.size) { // caller only wants to get the stream format
        auto ret = mRingBuf.waitForData(timeout);
        if (ret < 0) {
            return kStreamStopped;
        } else if (ret == 0) {
            return kTimeout;
        }
        dp.fmt = mStreamFormat;
        return kNoError;
    }
    tim.reset();
    auto ret = mRingBuf.contigRead(dp.buf, dp.size, timeout);
    if (tim.msElapsed() > timeout) {
        ESP_LOGW(mTag, "RingBuf read took more than timeout: took %d, timeout %d", tim.msElapsed(), timeout);
    }
    if (ret < 0) {
        return kStreamStopped;
    } else if (ret == 0){
        return kTimeout;
    } else {
        dp.size = ret;
        return kNoError;
    }
}

void HttpNode::confirmRead(int size)
{
    mRingBuf.commitContigRead(size);
}

void HttpNode::setWaitingPrefill(bool prefill)
{
    mWaitingPrefill = prefill;
    mEvents.setBits(kEvtPrefillChange);
}

int8_t HttpNode::waitPrefillChange(int msTimeout)
{
    auto bits = mEvents.waitForOneAndReset(kEvtPrefillChange|kEvtStopRequest, msTimeout);
    if (bits & kEvtStopRequest) {
        return -1;
    } else if (bits & kEvtPrefillChange) {
        return 1;
    } else {
        return 0;
    }
}

void HttpNode::clearAllIcyInfo()
{
    mIcyInterval = mIcyCtr = mIcyRemaining = 0;
    icyInfo.clear();
}

void HttpNode::IcyInfo::clear()
{
    mStaName.free();
    mStaDesc.free();
    mStaGenre.free();
    mStaUrl.free();
    mIcyMetaBuf.clear();
}

void HttpNode::startRecording(const char* stationName) {
    if (!mRecorder) {
        mRecorder.reset(new TrackRecorder("/sdcard/rec"));
    }
    mRecorder->setStation(stationName);
}
