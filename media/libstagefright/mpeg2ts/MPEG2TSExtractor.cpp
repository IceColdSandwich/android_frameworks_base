/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "MPEG2TSExtractor"
#include <utils/Log.h>

#include "include/MPEG2TSExtractor.h"
#include "include/LiveSession.h"
#include "include/NuCachedSource2.h"

#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <utils/String8.h>
#include <cutils/properties.h>

#include "AnotherPacketSource.h"
#include "ATSParser.h"
#include <media/stagefright/foundation/ABuffer.h>

#define MAX_NUM_TS_PACKETS_FOR_META_DATA 10000

namespace android {

static const size_t kTSPacketSize = 188;
static const size_t kTSCacheSize  = kTSPacketSize * 1000;

//////////////////////////////////////////////////////////////////////////////////////////
//Cache TS packets, instead of reading 188 bytes each time
//TODO Add support for reading in reverse direction
class TSBuffer : public RefBase {
public:
    status_t getTSPacket(sp<DataSource> dataSource,uint8_t **data, off64_t dataSrcOffset);
    void flush() {
        mOffset = 0;
        mSize = 0;
    }

    TSBuffer(size_t capacity, off64_t clipSize)
          : mData((uint8_t*)malloc(capacity)),
            mCapacity(capacity),
            mOffset(0),
            mSize(0),
            mClipSize(clipSize) {}

    ~TSBuffer() {
        if (mData != NULL) {
            free(mData);
        }
    }

    uint8_t* mData;     //TS cache data
    size_t   mCapacity; //Max length of buffer - allocated
    size_t   mOffset;   //CUrrent offset, dats is read from this offset
    size_t   mSize;     //size of data read from datasource
    off64_t  mClipSize; //Actual clip size, to check for end of stream case
};

status_t TSBuffer::getTSPacket(sp<DataSource> dataSource, uint8_t **data, off64_t dataSrcOffset) {
    if (mSize - mOffset < kTSPacketSize) {
        size_t size = mCapacity;
        if (dataSrcOffset + size > mClipSize) {
            size = mClipSize - dataSrcOffset;
        }
        if (size < kTSPacketSize) {
            LOGW("Completed reading, end of file");
            return ERROR_END_OF_STREAM;
        }
        ssize_t retVal = dataSource->readAt(dataSrcOffset, mData, size);
        if (retVal < size) {
            LOGE("Cannot read data from data source %d, %lld", size, dataSrcOffset);
            return (retVal < 0) ? (status_t)retVal : ERROR_END_OF_STREAM;
        }
        mSize = retVal;
        mOffset = 0;
    }
    (*data) = (uint8_t *)mData + mOffset;
    mOffset += kTSPacketSize;
    return OK;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
struct MPEG2TSSource : public MediaSource {

    //PES stream info
    struct StreamInfo : public RefBase {
    public:
        unsigned mStreamPID;
        unsigned mProgramPID;
        uint64_t mFirstPTS;
        uint64_t mLastPTS;
        int64_t  mDurationUs;
        off64_t  mFirstPTSOffset;
        off64_t  mLastPTSOffset;
        uint32_t mIndex;
        off64_t  mOffset;
    };

    MPEG2TSSource(
            const sp<MPEG2TSExtractor> &extractor,
            const sp<AnotherPacketSource> &impl,
            const sp<DataSource> &dataSource,
            bool  isVideo);

    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop();
    virtual sp<MetaData> getFormat();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options = NULL);

    status_t findStreamDuration();
private:
    sp<MPEG2TSExtractor>      mExtractor;
    sp<AnotherPacketSource>   mImpl;
    sp<MetaData>              mFormat;
    sp<StreamInfo>            mStream;
    sp<DataSource>            mDataSource;
    bool                      mIsVideo;
    sp<TSBuffer>              mTSBuffer;
    uint64_t                  mLastKnownSyncFrameTime;

    Mutex mLock;

    status_t findOffsetForPTS(off64_t& seekOffset, uint64_t seekPTS);
    status_t feedMoreForStream();
    status_t seekToSync();
    status_t seekPrepare( int64_t seekTimeUs, bool* seekError);
    DISALLOW_EVIL_CONSTRUCTORS(MPEG2TSSource);
};

MPEG2TSSource::MPEG2TSSource(
        const sp<MPEG2TSExtractor> &extractor,
        const sp<AnotherPacketSource> &impl,
        const sp<DataSource> &dataSource,
        bool  isVideo )
    : mExtractor(extractor),
      mImpl(impl),
      mDataSource(dataSource),
      mLastKnownSyncFrameTime(-1),
      mIsVideo(isVideo) {

    // Create stream info
    mStream = new StreamInfo;

    CHECK(mImpl != NULL);
    CHECK(mDataSource != NULL);
    CHECK(mExtractor != NULL);
    CHECK(mStream != NULL);

    impl->getStreamInfo(mStream->mStreamPID, mStream->mProgramPID, mStream->mFirstPTS);
    LOGV("Stream PID %d, program PID %d", mStream->mStreamPID, mStream->mProgramPID);
    mStream->mOffset = mExtractor->mOffset;
    mStream->mFirstPTSOffset = mStream->mOffset;
    mFormat = mImpl->getFormat();

    //Allocate TS cache buffer
    if (mExtractor->mClipSize == 0) {
        mTSBuffer = new TSBuffer(kTSCacheSize, kTSCacheSize);
    } else {
        mTSBuffer = new TSBuffer(kTSCacheSize, mExtractor->mClipSize);
    }
    CHECK(mTSBuffer != NULL);
}

status_t MPEG2TSSource::start(MetaData *params) {
    //Update offset
    mStream->mOffset = mExtractor->mOffset;
    return mImpl->start(params);
}

status_t MPEG2TSSource::stop() {
    return mImpl->stop();
}

sp<MetaData> MPEG2TSSource::getFormat() {
    mFormat = mImpl->getFormat();
    return mFormat;
}


status_t MPEG2TSSource::seekPrepare( int64_t seekTimeUs,bool* seekError) {
     // Get file offset for seek position
     status_t err = OK;
     uint64_t seekPTS = ((seekTimeUs*9/100) + mStream->mFirstPTS);
     off64_t  seekOffset = (seekTimeUs * mExtractor->mClipSize) / mStream->mDurationUs;
     seekOffset = (off64_t)(seekOffset / kTSPacketSize) * kTSPacketSize;
     LOGV("Seek PTS %lld , start searching from offset %lld", seekPTS, seekOffset);

     err = findOffsetForPTS(seekOffset, seekPTS);
     if (err != OK) {
         LOGE("Cannot seek, unable to find offset %lld",seekTimeUs);
         return err;
     }
     mStream->mOffset = seekOffset;
     LOGV("Found seek offset at %lld", seekOffset);
     mTSBuffer->flush();

     //Flush all PES data in parser
     mExtractor->seekTo(seekTimeUs);

     //Seek to I frame for video
     if (mIsVideo) {
         err = seekToSync();
         if (err != OK) {
             LOGE("Cannot seek this TS clip %d", err);
             *seekError = true;
             return err;
         }
     }
     return err;
}


status_t MPEG2TSSource::read(
        MediaBuffer **out, const ReadOptions *options) {
    Mutex::Autolock autoLock(mLock);

    *out = NULL;

    int64_t seekTimeUs = 0;
    ReadOptions::SeekMode seekMode;
    status_t err = OK;
    bool seekAble = mExtractor->isSeekable();

    if (seekAble && options && options->getSeekTo(&seekTimeUs, &seekMode)) {
        bool seekErr = false;
        err = seekPrepare(seekTimeUs, &seekErr);
        if(err == DEAD_OBJECT) {
            return err;
        }else if(err != OK && seekErr) {
            //reset to last known IFrame location
            //call seekPrepare again;

            if(mLastKnownSyncFrameTime != -1) {
                err = seekPrepare(mLastKnownSyncFrameTime, &seekErr);
            } else{
                err = seekPrepare(0, &seekErr);
            }
        }

        if(err != OK) {
            return err;
        }
    }

    status_t finalResult = OK;
    while (!mImpl->hasBufferAvailable(&finalResult)) {
        if (finalResult != OK) {
            return ERROR_END_OF_STREAM;
        }

        err = feedMoreForStream();
        if (err != OK) {
            mImpl->signalEOS(err);
        }
    }
    bool isSync = false;
    mImpl->nextBufferIsSync(&isSync);
    int64_t curPts =0;

    if(isSync){
        mImpl->nextBufferTime(&curPts);
    }

    err = mImpl->read(out, options);

    if((err == OK) && isSync) {
        mLastKnownSyncFrameTime = curPts;
    }

    return err;
}

status_t MPEG2TSSource::findStreamDuration(){
     Mutex::Autolock autoLock(mLock);

     if (mExtractor->mClipSize == 0){
         return INVALID_OPERATION;
     }

     off64_t offset = 0;
     status_t status = OK;
     bool foundPTS = false;
     uint8_t packet[kTSPacketSize];
     ssize_t retVal = 0;
     uint64_t PTS = 0;

     LOGV("First PTS found %lld, for stream %d, at %lld", mStream->mFirstPTS, mStream->mStreamPID, mStream->mFirstPTSOffset);

     //find last PTS
     offset = mExtractor->mClipSize - kTSPacketSize;
     while (offset > 0) {
         retVal = mDataSource->readAt(offset, packet, kTSPacketSize);
         if (retVal < 0) {
            LOGE("Error while reading data from datasource");
            return status_t(retVal);
         }
         if (retVal < (ssize_t)kTSPacketSize) {
             LOGV("Reached end of stream while searchin for last PTS");
             return ERROR_END_OF_STREAM;
         }

         status = mExtractor->parseTSToGetPTS(packet, kTSPacketSize,
                                           mStream->mStreamPID, PTS);
         if (status == DEAD_OBJECT) {
             LOGE("findStreamDuration:: Hit an invalid TS packet .. bailing out gracefully");
             return status;
         }

         if (status == OK) {
             mStream->mLastPTS = PTS;
             mStream->mLastPTSOffset = offset;
             LOGV("Last PTS found %lld, for stream %d, at %lld", mStream->mLastPTS, mStream->mStreamPID, offset);
             break;
         }
         offset -= kTSPacketSize;
     }
     if (status != OK) {
         LOGE("Could not find last PTS %d", status);
         return status;
     }
     mStream->mDurationUs = ((mStream->mLastPTS - mStream->mFirstPTS) * 100 )/9;
     CHECK(mStream->mDurationUs != 0);

     LOGV("Stream duration %lld", mStream->mDurationUs);

     if (mFormat != NULL) {
         mFormat->setInt64(kKeyDuration, mStream->mDurationUs);
     }

     return status;
}

status_t MPEG2TSSource::feedMoreForStream() {
    CHECK(mStream != NULL);

    uint8_t* packet = NULL;
    off64_t offset = mStream->mOffset;
    bool found = 0;
    unsigned PID = 0;

    while (!found) {
        status_t status = mTSBuffer->getTSPacket(mDataSource, &packet, offset);
        if (status != OK || packet == NULL) {
            return status;
        }

        if (mExtractor->parseTSToGetPID(packet,kTSPacketSize,PID) != OK) {
            LOGE("Error parsing PID");
            return BAD_VALUE;
        }
        if (PID == mStream->mStreamPID) {
            found = true;
            offset += kTSPacketSize;
            break;
        }
        //TODO handle program/stream PID change
        if (PID == 0 || PID == mStream->mProgramPID) {
            LOGE("PID Changed ... at these clips are not supported");
            return DEAD_OBJECT;
        }

        offset += kTSPacketSize;
    }
    mStream->mOffset = offset;
    return mExtractor->feedTSPacket(packet, kTSPacketSize);
}

status_t MPEG2TSSource::findOffsetForPTS(off64_t& seekOffset, uint64_t seekPTS){
    CHECK(mStream != NULL);

    if (seekPTS <= mStream->mFirstPTS) {
        seekOffset = kTSPacketSize;  //start from beginning of clip
        LOGI("seek to first pts");
        return OK;
    }
    if (seekPTS >= mStream->mLastPTS) {
        seekOffset = mStream->mLastPTSOffset;
        LOGI("seek to last pts");
        return OK;
    }
    status_t status = OK;
    uint8_t packet[kTSPacketSize];
    ssize_t retVal = 0;
    uint64_t currPTS = 0,prevPTS = 0;
    off64_t fileOffset = seekOffset, prevOffset = 0;
    bool found = false;
    bool searchBack = false;

    if (fileOffset >= mStream->mLastPTSOffset) {
        searchBack = true;
        fileOffset = mStream->mLastPTSOffset - kTSPacketSize;
        prevPTS = mStream->mLastPTS;
        prevOffset = mStream->mLastPTSOffset;
    }
    if (fileOffset <= mStream->mFirstPTSOffset) {
        searchBack = false;
        fileOffset = mStream->mFirstPTSOffset + kTSPacketSize;
        prevOffset = mStream->mFirstPTSOffset;
        prevPTS = mStream->mFirstPTS;
    }

    LOGV("In prev PTS %lld, curr PTS %lld, actual pTS %lld", prevPTS, currPTS, seekPTS);
    LOGV("In offset %lld, prev offset %lld ", fileOffset, prevOffset);
    while(!found) {
        retVal = mDataSource->readAt(fileOffset, packet, kTSPacketSize);
        if (retVal <= 0 || retVal < (ssize_t)kTSPacketSize) {
            LOGW("Error while reading data from datasource");
            status = (retVal < 0) ? status_t(retVal) : ERROR_END_OF_STREAM;
            break;
        }
        status = mExtractor->parseTSToGetPTS(packet, kTSPacketSize,
                                          mStream->mStreamPID, currPTS);

        if (status == DEAD_OBJECT) {
            LOGE("findOffsetForPTS:: bad TS packet found");
            return status;
        }

        if (status == OK){
             if ((seekPTS == currPTS) ||
                 ((prevOffset != 0) && ((seekPTS < currPTS && seekPTS > prevPTS) ||
                                       (seekPTS > currPTS && seekPTS < prevPTS)))) {
                 LOGV("Seek PTS found %lld, for stream %d at %lld", seekPTS, mStream->mStreamPID, fileOffset);
                 found = true;
                 break;
             }
             if (currPTS < seekPTS){
                 searchBack = false;
             } else {
                 searchBack = true;
             }
             prevPTS = currPTS;
             prevOffset = fileOffset;
        }
        if (searchBack) {
            if ((fileOffset > kTSPacketSize) && (fileOffset - kTSPacketSize > mStream->mFirstPTSOffset)) {
                 fileOffset -= kTSPacketSize;
            } else {
                 LOGW("Reached start of file searching for seek PTS %lld", seekPTS);
                 fileOffset = mStream->mFirstPTSOffset;
                 found = true;
                 break;
            }
        } else {
            if (fileOffset + kTSPacketSize <= mStream->mLastPTSOffset) {
                 fileOffset += kTSPacketSize;
            } else {
                 LOGW("Reached end of file searching for seek PTS %lld", seekPTS);
                 fileOffset = mStream->mLastPTSOffset;
                 found = true;
                 break;
            }
        }
    }
    LOGV("In prev PTS %lld, curr PTS %lld, actual pTS %lld", prevPTS, currPTS, seekPTS);
    LOGV("In offset %lld, prev offset %lld ", fileOffset, prevOffset);
    if (found) {
        //find the closest offset
        uint64_t deltaCurr = seekPTS < currPTS ? currPTS - seekPTS : seekPTS - currPTS;
        uint64_t deltaPrev = seekPTS < prevPTS ? prevPTS - seekPTS : seekPTS - prevPTS;
        if (deltaCurr < deltaPrev) {
            seekOffset = fileOffset;
        } else {
            seekOffset = prevOffset;
        }
        status = OK;
    }
    return status;
}

//TODO seek to closest sync
status_t MPEG2TSSource::seekToSync() {
    bool isSync = false;
    status_t status = OK,finalResult = OK;

    while(!isSync && status == OK) {
        if (!mImpl->hasBufferAvailable(&finalResult)) {
            if (finalResult == OK) {
                status = feedMoreForStream();
            } else {
                status = finalResult;
                break;
            }
            continue;
         }
         LOGV("Found a frame at seek offset, check if it is reference frame");
         status = mImpl->nextBufferIsSync(&isSync);
         if (status == OK && !isSync) {
             sp<ABuffer> accessUnit = NULL;
             LOGI("dropping access unit");
             status = mImpl->dequeueAccessUnit(&accessUnit);
         }
    }

    if (status != OK) {
        LOGE("Cannot find sync frame for video");
    }

    return status;
}


////////////////////////////////////////////////////////////////////////////////

MPEG2TSExtractor::MPEG2TSExtractor(const sp<DataSource> &source)
    : mDataSource(source),
      mParser(new ATSParser),
      mOffset(0),
      mSeekable(false),
      mClipSize(0) {

    CHECK(mParser != NULL);
    CHECK(mDataSource != NULL);
    CHECK(kTSPacketSize != 0);

    mDataSource->getSize(&mClipSize);
    if (mClipSize == 0) {
        mTSBuffer = new TSBuffer(kTSCacheSize, kTSCacheSize);
    } else {
        mTSBuffer = new TSBuffer(kTSCacheSize, mClipSize);
    }
    CHECK(mTSBuffer != NULL);

    if (mClipSize % kTSPacketSize != 0) {
        LOGW("Clipsize is not correct %lld", mClipSize);
        mClipSize = 0;
        return;
    }

    init();
}

MPEG2TSExtractor::~MPEG2TSExtractor() {
    mSourceList.clear();
}

size_t MPEG2TSExtractor::countTracks() {
    return mSourceList.size();
}

sp<MediaSource> MPEG2TSExtractor::getTrack(size_t index) {
    if (index >= mSourceList.size()) {
        return NULL;
    }

    return mSourceList.editItemAt(index);
}

sp<MetaData> MPEG2TSExtractor::getTrackMetaData(
        size_t index, uint32_t flags) {
    return index < mSourceList.size()
        ? mSourceList.editItemAt(index)->getFormat() : NULL;
}

sp<MetaData> MPEG2TSExtractor::getMetaData() {
    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_CONTAINER_MPEG2TS);

    return meta;
}

void MPEG2TSExtractor::init() {
    bool haveAudio = false;
    bool haveVideo = false;
    int numPacketsParsed = 0;
    sp<MPEG2TSSource> audioSource = NULL;
    sp<MPEG2TSSource> videoSource = NULL;

    while (feedMore() == OK) {
        ATSParser::SourceType type;
        if (haveAudio && haveVideo) {
            break;
        }
        if (!haveVideo) {
            sp<AnotherPacketSource> impl =
                (AnotherPacketSource *)mParser->getSource(
                        ATSParser::VIDEO).get();

            if (impl != NULL) {
                haveVideo = true;
                videoSource = new MPEG2TSSource(this, impl, mDataSource, true);
                if (videoSource == NULL) {
                    LOGE("Unable to create video TS source");
                } else {
                    mSourceList.push(videoSource);
                }
            }
        }

        if (!haveAudio) {
            sp<AnotherPacketSource> impl =
                (AnotherPacketSource *)mParser->getSource(
                        ATSParser::AUDIO).get();

            if (impl != NULL) {
                haveAudio = true;
                sp<MetaData> meta = impl->getFormat();
                const char *mime;
                CHECK(meta->findCString(kKeyMIMEType, &mime));

               //if this audio/mpeg* then drop the audio
               //we are intrested in only audio/mpeg (size ==10) if audio is mpeg format (mp3)
               if ((!strncasecmp("audio/mpeg", mime, 10)) && (strlen(mime) > 10)) {
                    LOGE("Audio is %s - Droping this",mime);
                } else{
                    LOGI("Audio is %s - keeping this",mime);
                    audioSource = new MPEG2TSSource(this, impl, mDataSource, false);
                    if (audioSource != NULL) {
                        mSourceList.push(audioSource);
                    } else {
                        LOGE("Unable to create audio TS source");
                    }
                }
            }
        }

        if (++numPacketsParsed > MAX_NUM_TS_PACKETS_FOR_META_DATA) {
            LOGW("Parsed more than 10000 TS packets and could not find AV data");
            break;
        }
    }

    LOGI("haveAudio=%d, haveVideo=%d", haveAudio, haveVideo);

    if (!haveAudio && !haveVideo) {
        mSeekable = false;
        LOGE("Could not find any audio/video data");
        return;
    }

    bool mAudioSeekable = true ,mvideoSeekable = true;

    if (audioSource != NULL) {
        if (audioSource->findStreamDuration() != OK) {
            mAudioSeekable = false;
        }
    }
    if (videoSource != NULL) {
        if (videoSource->findStreamDuration() != OK){
            mvideoSeekable = false;
        }
    }

    char value[PROPERTY_VALUE_MAX];
    if(property_get("TSParser.disable.seek", value, NULL) &&
            (!strcasecmp(value, "true") || !strcmp(value, "1"))) {
        mSeekable = false;
    } else if(mAudioSeekable && mvideoSeekable) {
        mSeekable = true;
    }
}

status_t MPEG2TSExtractor::feedMore() {
    Mutex::Autolock autoLock(mLock);

    uint8_t* packet = NULL;
    status_t status = mTSBuffer->getTSPacket(mDataSource, &packet, mOffset);

    if (status != OK || packet == NULL) {
        return status;
    }

    mOffset += kTSPacketSize;
    return mParser->feedTSPacket(packet, kTSPacketSize);
}

void MPEG2TSExtractor::setLiveSession(const sp<LiveSession> &liveSession) {
    Mutex::Autolock autoLock(mLock);

    if (liveSession != NULL) {
        liveSession->isSeekable();
    }
}

void MPEG2TSExtractor::seekTo(int64_t seekTimeUs) {
    Mutex::Autolock autoLock(mLock);

    if (!mSeekable) {
        LOGE("Cannot seek for this clip");
        return;
    }
    //Flush all PES data in parser
    mParser->signalDiscontinuity(ATSParser::DISCONTINUITY_TS_PLAYER_SEEK, NULL);
}

uint32_t MPEG2TSExtractor::flags() const {
    Mutex::Autolock autoLock(mLock);

    uint32_t flags = CAN_PAUSE;

    if (mSeekable) {
        flags |= CAN_SEEK_FORWARD | CAN_SEEK_BACKWARD | CAN_SEEK;
    }

    return flags;
}

status_t MPEG2TSExtractor::parseTSToGetPTS(const void *data, size_t size,
                                           unsigned streamPID, uint64_t& PTS) {
    Mutex::Autolock autoLock(mLock);

    return mParser->parseTSToGetPTS(data,size,streamPID,PTS);
}

bool MPEG2TSExtractor::isSeekable() {
    Mutex::Autolock autoLock(mLock);
    return mSeekable;
}

status_t MPEG2TSExtractor::parseTSToGetPID(const void *data, size_t size,
                             unsigned& streamPID) {
    Mutex::Autolock autoLock(mLock);

    return mParser->parseTSToGetPID(data,size,streamPID);
}

status_t MPEG2TSExtractor::feedTSPacket(const void *data, size_t size) {
    Mutex::Autolock autoLock(mLock);

    return mParser->feedTSPacket(data,size);
}


////////////////////////////////////////////////////////////////////////////////

bool SniffMPEG2TS(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *) {
    for (int i = 0; i < 5; ++i) {
        char header;
        if (source->readAt(kTSPacketSize * i, &header, 1) != 1
                || header != 0x47) {
            return false;
        }
    }

    *confidence = 0.6f;
    mimeType->setTo(MEDIA_MIMETYPE_CONTAINER_MPEG2TS);

    return true;
}

}  // namespace android
