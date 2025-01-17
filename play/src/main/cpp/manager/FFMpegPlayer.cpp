//
// Created by Administrator on 2023/11/19.
//
#include "FFMpegPlayer.h"
#include "../globals.h"

FFMpegPlayer::FFMpegPlayer() {
    mMutexObj = std::make_shared<MutexObj>();
    LOGI("FFMpegPlayer")
}

FFMpegPlayer::~FFMpegPlayer() {
    mPlayerJni.reset();
    mMutexObj = nullptr;
    LOGI("~FFMpegPlayer")
}

void FFMpegPlayer::init(JNIEnv *env, jobject thiz) {
    jclass jclazz = env->GetObjectClass(thiz);
    if (jclazz == nullptr) {
        return;
    }
    LOGI("FFMpegPlayer init")
    mPlayerJni.reset();
    mPlayerJni.instance = env->NewGlobalRef(thiz);
    mPlayerJni.onVideoConfig = env->GetMethodID(jclazz, "onNativeVideoConfig",
                                                "(IIDDLjava/lang/String;)V");
    mPlayerJni.onPlayProgress = env->GetMethodID(jclazz, "onNativePalyProgress", "(D)V");
    mPlayerJni.onPlayCompleted = env->GetMethodID(jclazz, "onNativePalyComplete", "()V");
    mPlayerJni.onPlayError = env->GetMethodID(jclazz, "onPlayError", "(I)V");
}

bool resultIsFail(int result) {
    return result < 0;
}

bool FFMpegPlayer::prepare(JNIEnv *env, std::string &path, jobject surface, jobject out_config) {
    LOGI("prepare path:%s", path.c_str())
    if (mJvm == nullptr) {
        env->GetJavaVM(&mJvm);
    }
    // 设置JavaVM，否则无法进行硬解码
    av_jni_set_java_vm(mJvm, nullptr);
    //分配 mAvFormatContext
    mAvFormatContext = avformat_alloc_context();

    //打开文件输入流
    int result = avformat_open_input(&mAvFormatContext, path.c_str(), nullptr, nullptr);
    if (resultIsFail(result)) {
        LOGE("avformat_open_input fail,result:%d", result)
        env->CallVoidMethod(mPlayerJni.instance, mPlayerJni.onPlayError, ERRORCODE_PREPARE_FILE);
        return false;
    }

    //提取输入文件中的数据流信息
    result = avformat_find_stream_info(mAvFormatContext, nullptr);
    if (resultIsFail(result)) {
        LOGE("avformat_find_stream_info fail,result:%d", result)
        env->CallVoidMethod(mPlayerJni.instance, mPlayerJni.onPlayError, ERRORCODE_FIND_STERAM);
        return false;
    }
    bool audioPrePared = false;
    bool videoPrepared = false;
    for (int i = 0; i < mAvFormatContext->nb_streams; ++i) {
        AVStream *pStream = mAvFormatContext->streams[i];
        AVCodecParameters *codecpar = pStream->codecpar;
        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            //视频流
            LOGI("video stream,index:%d result:%d", i, mPlayerJni.isValid())
            mVideoDecoder = std::make_shared<VideoDecoder>(i, mAvFormatContext);
            mVideoDecoder->initConfig(env, out_config);

            mVideoDecoder->setSurface(surface);
            videoPrepared = mVideoDecoder->prepare(env);

            if (mPlayerJni.isValid()) {
                int surfaceWidth = mVideoDecoder->getWidth();
                int surfaceHeight = mVideoDecoder->getHeight();
                if (mVideoDecoder->getConfigCropWidth() != 0 &&
                        mVideoDecoder->getConfigCropHeight() != 0) {
                    surfaceWidth = mVideoDecoder->getConfigCropWidth();
                    surfaceHeight = mVideoDecoder->getConfigCropHeight();
                }
                const char *codecName = avcodec_get_name(codecpar->codec_id);

                env->CallVoidMethod(mPlayerJni.instance, mPlayerJni.onVideoConfig,
                                    surfaceWidth, surfaceHeight,
                                    mVideoDecoder->getDuration(), mVideoDecoder->getFps(),
                                    env->NewStringUTF(codecName));
            }
        } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            //音频流
            LOGI("audio stream,index:%d", i)
            mAudioDecoder = std::make_shared<AudioDecoder>(i, mAvFormatContext);
            audioPrePared = mAudioDecoder->prepare(env);
            if (false) {
                mAudioPacketQueue = std::make_shared<AVPacketQueue>(50);
                mAudioThread = new std::thread(&FFMpegPlayer::AudioDecodeLoop, this);
                mAudioDecoder->setErrorMsgListener([](int err, std::string &msg) {
                    LOGE("[audio] err code: %d, msg: %s", err, msg.c_str())
                });
            } else {
                mAudioDecoder = nullptr;
                mAudioPacketQueue = nullptr;
                mAudioThread = nullptr;
                LOGE("audio track prepared failed!!!")
            }
        }
    }
    bool prepared = videoPrepared || audioPrePared;
    LOGI("videoPrepared: %d, audioPrePared: %d, path: %s", videoPrepared, audioPrePared,
         path.c_str())
    if (prepared) {
        updatePlayerState(PlayerState::PREPARE);
    }
    mVideoPacketQueue = std::make_shared<AVPacketQueue>(50);
    mVideoFrameQueue = std::make_shared<AVFrameQueue>(11 * mVideoDecoder->getTargetFps(),
                                                      "cache");
    mVideoThread = new std::thread(&FFMpegPlayer::VideoDecodeLoop, this);
    mReadPacketThread = new std::thread(&FFMpegPlayer::ReadPacketLoop, this);
    mVideoDecodeThread = new std::thread(&FFMpegPlayer::ReadVideoFrameLoop, this);
    return prepared;
}

void FFMpegPlayer::start() {
    LOGI("FFMpegPlayer::start, state: %d", mPlayerState)
    resume();
}

bool FFMpegPlayer::seekTo(int64_t seekTime) {
    pause();
    LOGI("seek start:%ld", seekTime)
    if (mHasAbort) {
        return false;
    }
    double duration = mVideoDecoder->getDuration();
    duration = floor((duration * 100 + 0.5)) / 100 * 1000;
    if (seekTime > (int64_t) duration) {
        LOGI("seek seekTime(%ld)>dration(%f) ", seekTime, duration)
        return false;
    }
    int64_t currPlayTime = mVideoDecoder->getTimestamp();
    if (currPlayTime == seekTime) {
        LOGI("seek currPlayTime(%ld) == seekTime(%ld)", currPlayTime, seekTime)
        return true;
    }
    mVideoDecoder->seekLock();
    mCurrSeekTime = seekTime;
    mIsSeek = true;

    int64_t lastCacheFrameTime = 0;

    AVFrame *pFrame = nullptr;
    bool callSeek = false;
    bool callQueueClear = false;
    mIsBackSeek = currPlayTime > seekTime;
    if (mIsBackSeek) {
        pFrame = mVideoFrameQueue->front();
    } else {
        pFrame = mVideoFrameQueue->back();
    }
    if (pFrame == nullptr) {
        callSeek = true;
        callQueueClear = true;
    } else {
        if (pFrame->pkt_size == 0) {
            LOGI("seek bak is EOF")
            lastCacheFrameTime = duration * 1000;
        } else {
            lastCacheFrameTime = pFrame->pts * av_q2d(pFrame->time_base) * 1000;
        }
        if (mIsBackSeek) {
            if (seekTime < lastCacheFrameTime) {
                callSeek = true;
                callQueueClear = true;
            }
        } else {
            if (seekTime > lastCacheFrameTime) {
                callSeek = true;
                callQueueClear = true;
            }
        }
    }
    LOGI("seek seekTime:%ld,currPlayTime:%ld,lastCache:%ld", seekTime,
         currPlayTime,
         lastCacheFrameTime)
    LOGI("seek callSeek:%d callQueueClear:%d", callSeek, callQueueClear)
    if (callSeek) {
        mVideoDecoder->seek(seekTime);
    }
    if (callQueueClear) {
        mVideoPacketQueue->clear();
        mVideoFrameQueue->clear(true);
    }
    mVideoDecoder->seekUnlock();
    if (callSeek) {
        LOGI("seek back wait start")
        mVideoFrameQueue->wait();
        LOGI("seek back wait end")
        if (mHasAbort) {
            return false;
        }
    }

    pFrame = mVideoFrameQueue->getFrameByTime(seekTime, mIsBackSeek);
    if (pFrame == nullptr) {
        LOGI("seek %ld get frame fail", seekTime)
        //        if (ret == AVERROR(EAGAIN)) {
        //            LOGI("seek %lf start", seekTime)
        //            mVideoFrameQueue->wait();
        //            LOGI("seek %lf end", seekTime)
        //            return seekTo(seekTime);
        //        }
        mIsSeek = false;
        return false;
    }
    int64_t frameTime = pFrame->pts * av_q2d(pFrame->time_base) * 1000;
    //    if (!mIsBackSeek && seekTime > frameTime) {
    //        LOGI("seek seekTime:%ld>frameTime %ld,continue seek", seekTime, frameTime)
    //        av_frame_free(&pFrame);
    //        return seekTo(seekTime);
    //    }
    LOGI("seek %ld done,show %ld", seekTime, frameTime)
    mVideoDecoder->resultCallback(pFrame);
    LOGI("seek %ld done", seekTime)
    mIsSeek = false;
    return true;
}

void FFMpegPlayer::stop() {
    LOGI("FFMpegPlayer::stop")
    mHasAbort = true;
    mIsMute = false;
    updatePlayerState(PlayerState::STOP);
    if (mVideoPacketQueue) {
        mVideoPacketQueue->notify();
    }
    if (mVideoFrameQueue) {
        mVideoFrameQueue->notify();
    }
    mMutexObj->wakeUp();

    if (mReadPacketThread != nullptr) {
        LOGI("join mReadPacketThread")
        mReadPacketThread->join();
        delete mReadPacketThread;
        mReadPacketThread = nullptr;
        LOGI("release mReadPacketThread")
    }
    if (mVideoDecodeThread != nullptr) {
        LOGI("join mVideoDecodeThread")
        mVideoDecodeThread->join();
        delete mVideoDecodeThread;
        mVideoDecodeThread = nullptr;
        LOGI("release mVideoDecodeThread")
    }

    if (mVideoThread != nullptr) {
        LOGI("join mVideoThread")
        mVideoThread->join();
        delete mVideoThread;
        mVideoThread = nullptr;
        LOGI("release mVideoThread")
    }
    mVideoDecoder = nullptr;
    LOGI("release video res")

    mVideoFrameQueue->clear(true);
    mVideoFrameQueue = nullptr;

    mVideoPacketQueue->clear();
    mVideoPacketQueue = nullptr;

    // release audio res
    if (mAudioThread != nullptr) {
        LOGI("join audio thread")
        if (mAudioPacketQueue) {
            mAudioPacketQueue->clear();
        }
        mAudioThread->join();
        delete mAudioThread;
        mAudioThread = nullptr;
    }
    mAudioDecoder = nullptr;
    LOGI("release audio res")

    if (mAvFormatContext != nullptr) {
        avformat_close_input(&mAvFormatContext);
        avformat_free_context(mAvFormatContext);
        mAvFormatContext = nullptr;
        LOGI("format context...release")
    }
}

void FFMpegPlayer::resume() {
    if (mIsSeek) {
        LOGI("resume is seeking")
        return;
    }
    mIsSeek = false;
    mIsBackSeek = false;
    updatePlayerState(PlayerState::PLAYING);
    if (mVideoDecoder) {
        mVideoDecoder->needFixStartTime();
    }
    if (mAudioDecoder) {
        mAudioDecoder->needFixStartTime();
    }
    mMutexObj->wakeUp();
    if (mAudioPacketQueue) {
        mAudioPacketQueue->notify();
    }
    mVideoPacketQueue->notify();
    mVideoFrameQueue->notify();
}

void FFMpegPlayer::pause() {
    updatePlayerState(PlayerState::PAUSE);
}

void FFMpegPlayer::VideoDecodeLoop() {
    if (mVideoDecoder == nullptr || mVideoPacketQueue == nullptr) {
        return;
    }
    JNIEnv *env = nullptr;
    bool needAttach = mJvm->GetEnv((void **) &env, JNI_VERSION_1_4) == JNI_EDETACHED;
    if (needAttach) {
        mJvm->AttachCurrentThread(&env, nullptr);
        LOGI("[video] AttachCurrentThread")
    }
    mVideoDecoder->setOnFrameArrived([this](AVFrame *frame) {
        JNIEnv *env = nullptr;
        bool needAttach = mJvm->GetEnv((void **) &env, JNI_VERSION_1_4) == JNI_EDETACHED;
        if (needAttach) {
            mJvm->AttachCurrentThread(&env, nullptr);
            LOGI("[video] OnFrameArrived AttachCurrentThread")
        }
        LOGI("OnFrameArrived start needAttach:%d", needAttach)
        if (!mHasAbort && mVideoDecoder) {
            if (mAudioDecoder) {
                auto diff = mAudioDecoder->getTimestamp() - mVideoDecoder->getTimestamp();
                LOGW("[video] frame arrived, AV time diff: %ld,mIsSeek: %d", diff, mIsSeek)
            }
            if (!mIsSeek) {
                int64_t timestamp = mVideoDecoder->getTimestamp();
                LOGI("avSync start %ld,mIsSeek: %d", timestamp, mIsSeek)
                mVideoDecoder->avSync(frame);
                LOGI("avSync end %ld,mIsSeek: %d", timestamp, mIsSeek)
                if (mIsSeek) {
                    return;
                }
            }
            mVideoDecoder->showFrameToWindow(frame);
            LOGI("async done")
            if (!mIsSeek && !mAudioDecoder && mPlayerJni.isValid()) { // no audio track
                double timestamp = mVideoDecoder->getTimestamp();
                env->CallVoidMethod(mPlayerJni.instance, mPlayerJni.onPlayProgress, timestamp);
            }
            if (needAttach) {
                mJvm->DetachCurrentThread();
            }
        } else {
            LOGI("[video] setOnFrameArrived, has abort")
        }
    });

    LOGI("[video] VideoDecodeLoop start")
    while (true) {
        if (!mHasAbort) {
            mVideoFrameQueue->checkEmptyWait();
        }

        if (mPlayerState == PlayerState::PAUSE || mPlayerState == PlayerState::PREPARE) {
            LOGI("[video] VideoDecodeLoop decode pause wait")
            mMutexObj->wait();
            LOGI("[video] VideoDecodeLoop decode pause wakup state:%d", mPlayerState);
        }

        if (mHasAbort) {
            LOGI("[video] VideoDecodeLoop has abort...")
            break;
        }

        AVFrame *frame = mVideoFrameQueue->getFrame(false, false);
        if (frame == nullptr) {
            mVideoFrameQueue->wait();
            LOGI("[video] VideoDecodeLoop getFrame is null")
            continue;
        }
        if (frame->pkt_size == 0) {
            onPlayCompleted(env);
            LOGI("[video] VideoDecodeLoop AVERROR_EOF wait start")
            mVideoFrameQueue->wait();
            LOGI("[video] VideoDecodeLoop AVERROR_EOF wait end")
        } else {
            LOGI("[video] VideoDecodeLoop pts:%ld(%lf)", frame->pts,
                 frame->pts * av_q2d(frame->time_base))
            mVideoDecoder->resultCallback(frame);
        }
    }
    if (needAttach) {
        mJvm->DetachCurrentThread();
    }
    LOGI("[video] VideoDecodeLoop end")
}

void FFMpegPlayer::AudioDecodeLoop() {
    if (mAudioDecoder == nullptr || mAudioPacketQueue == nullptr) {
        return;
    }

    JNIEnv *env = nullptr;
    if (mJvm->GetEnv((void **) &env, JNI_VERSION_1_4) == JNI_EDETACHED) {
        mJvm->AttachCurrentThread(&env, nullptr);
        LOGI("[audio] AttachCurrentThread")
    }

    mAudioDecoder->setOnFrameArrived([this, env](AVFrame *frame) {
        if (!mHasAbort && mAudioDecoder) {
            mAudioDecoder->avSync(frame);
            mAudioDecoder->playAudio(frame);
            if (mPlayerJni.isValid()) {
                double timestamp = mAudioDecoder->getTimestamp();
                env->CallVoidMethod(mPlayerJni.instance, mPlayerJni.onPlayProgress, timestamp);
            }
        } else {
            LOGI("[audio] setOnFrameArrived, has abort")
        }
    });

    while (true) {

        while (!mHasAbort && mAudioPacketQueue->isEmpty()) {
            LOGI("[audio] no packet, wait...")
            mAudioPacketQueue->wait();
        }
        if (mPlayerState == PlayerState::PAUSE) {
            LOGI("[audio] decode pause wait")
            mMutexObj->wait();
            LOGI("[audio] decode pause wakup state:%d", mPlayerState);
        }
        if (mHasAbort) {
            LOGI("[audio] has abort...")
            break;
        }

        AVPacket *packet = mAudioPacketQueue->pop();
        if (packet != nullptr) {
            int ret = 0;
            do {
                ret = mAudioDecoder->decode(packet, NULL);
            } while (mAudioDecoder->isNeedResent());

            av_packet_unref(packet);
            av_packet_free(&packet);
            if (ret == AVERROR_EOF) {
                LOGI("AudioDecodeLoop AVERROR_EOF")
                onPlayCompleted(env);
            }
        } else {
            LOGI("AudioDecodeLoop pop packet failed...")
        }
    }

    mAudioPacketQueue->clear();
    mAudioPacketQueue = nullptr;

    mJvm->DetachCurrentThread();
    LOGI("[audio] DetachCurrentThread");
}

void FFMpegPlayer::ReadVideoFrameLoop() {
    LOGI("ReadVideoFrameLoop start")
    while (true) {
        if (!mHasAbort) {
            mVideoPacketQueue->checkEmptyWait();
        }

        if (mHasAbort) {
            LOGE("[video] ReadVideoFrameLoop has abort...")
            break;
        }

        int decodeResult;

        do {
            mVideoDecoder->seekLock();
            decodeResult = -1;
            AVPacket *packet = mVideoPacketQueue->pop();
            std::shared_ptr<AVFrameQueue> tempFrameQueue = std::make_shared<AVFrameQueue>(50,
                                                                                          "temp");
            if (packet != nullptr) {
                LOGI("ReadVideoFrameLoop popto pts:%ld size:%ld", packet->pts,
                     mVideoFrameQueue->getSize())
                do {
                    if (tempFrameQueue->isFull()) {
                        break;
                    }
                    AVFrame *pFrame = av_frame_alloc();

                    decodeResult = mVideoDecoder->decode(packet, pFrame);
                    if (mHasAbort) {
                        decodeResult = -1;
                        break;
                    }
                    if (decodeResult == 0) {
                        tempFrameQueue->pushBack(pFrame, true);
                    } else {
                        av_frame_free(&pFrame);
                    }
                } while (mVideoDecoder->isNeedResent());
                LOGI("[video] ReadVideoFrameLoop decode %d,tempQueueSize:%ld", decodeResult,
                     tempFrameQueue->getSize())
                if (packet->size == 0 && packet->data == nullptr && decodeResult != AVERROR_EOF) {
                    //并发问题,解码添加了filter,需要多次推eof帧获取frame
                    //packet pop最后一个,还未走到ReadPacketLoop的wait,就走到解码checkEmptyWait,再走ReadPacketLoop wait,导致两边都在wait

                    //主动push eof处理
                    AVPacket *videoFlushPkt = av_packet_alloc();
                    videoFlushPkt->size = 0;
                    videoFlushPkt->data = nullptr;
                    if (!pushPacketToQueue(videoFlushPkt, mVideoPacketQueue)) {
                        av_packet_free(&videoFlushPkt);
                        av_freep(&videoFlushPkt);
                    }
                }
            } else {
                LOGE("ReadVideoFrameLoop pop packet failed...")
            }
            av_packet_free(&packet);
            mVideoDecoder->seekUnlock();
            while (!tempFrameQueue->isEmpty()) {
                AVFrame *pFrame = tempFrameQueue->getFrame(true, false);
                if (pFrame == nullptr) {
                    LOGI("ReadVideoFrameLoop getFrame is null")
                    tempFrameQueue->wait();
                    continue;
                }
                if (!pushFrameToQueue(pFrame, mVideoFrameQueue, false)) {
                    av_frame_free(&pFrame);
                    pFrame = nullptr;
                } else {
                    double time = pFrame->pts * av_q2d(pFrame->time_base) * 1000;
                    LOGI("pushFrameToQueue success %ld(%lf)", pFrame->pts, time)
                }
            }
            tempFrameQueue = nullptr;
            if (decodeResult == AVERROR_EOF) {
                AVFrame *doneFrame = av_frame_alloc();
                doneFrame->pkt_size = 0;
                pushFrameToQueue(doneFrame, mVideoFrameQueue, false);
                LOGI("ReadVideoFrameLoop AVERROR_EOF")
                mVideoPacketQueue->checkNotEmptyWait();
                break;
            }
            if (mHasAbort) {
                break;
            }
        } while (decodeResult == AVERROR(EAGAIN));
    }

    LOGI("ReadVideoFrameLoop end")
}

void FFMpegPlayer::ReadPacketLoop() {
    LOGI("FFMpegPlayer::ReadPacketLoop start")
    while (mPlayerState != PlayerState::STOP) {
        if (mHasAbort) {
            break;
        }
        bool isEnd = readAvPacketToQueue(ReadPackType::ANY) != 0;
        if (isEnd) {
            LOGI("read av packet end,wait start, mPlayerState: %d", mPlayerState)
            //并发问题,避免其他地方pop,到这又是empty,期望notempty才wait
            mVideoPacketQueue->checkNotEmptyWait();
            LOGI("read av packet end,wait end, mPlayerState: %d", mPlayerState)
        }
    }
    LOGI("FFMpegPlayer::ReadPacketLoop end")
}

bool FFMpegPlayer::readAvPacketToQueue(ReadPackType type) {
    reread:
    AVPacket *avPacket = av_packet_alloc();
    mVideoDecoder->seekLock();
    bool isEnd;
    int ret = av_read_frame(mAvFormatContext, avPacket);
    mVideoDecoder->seekUnlock();
    bool suc = false;
    if (ret == 0) {
        if (type == ReadPackType::VIDEO) {
            if (avPacket->stream_index != mVideoDecoder->getStreamIndex()) {
                av_packet_free(&avPacket);
                goto reread;
            }
        } else if (type == ReadPackType::AUDIO) {
            if (avPacket->stream_index != mAudioDecoder->getStreamIndex()) {
                av_packet_free(&avPacket);
                goto reread;
            }
        }
        if (mVideoDecoder && mVideoPacketQueue &&
                avPacket->stream_index == mVideoDecoder->getStreamIndex()) {
            LOGI("pushBack video pts:%ld(%f)", avPacket->pts,
                 avPacket->pts * av_q2d(mVideoDecoder->getTimeBase()))
            suc = pushPacketToQueue(avPacket, mVideoPacketQueue);
        } else if (mAudioDecoder && mAudioPacketQueue &&
                avPacket->stream_index == mAudioDecoder->getStreamIndex()) {
            LOGI("pushBack audio")
            suc = pushPacketToQueue(avPacket, mAudioPacketQueue);
        }
        isEnd = false;
    } else {
        // send flush packet
        AVPacket *videoFlushPkt = av_packet_alloc();
        videoFlushPkt->size = 0;
        videoFlushPkt->data = nullptr;
        isEnd = true;
        if (!pushPacketToQueue(videoFlushPkt, mVideoPacketQueue)) {
            av_packet_free(&videoFlushPkt);
            av_freep(&videoFlushPkt);
        }

        if (mAudioPacketQueue) {
            AVPacket *audioFlushPkt = av_packet_alloc();
            audioFlushPkt->size = 0;
            audioFlushPkt->data = nullptr;
            if (!pushPacketToQueue(audioFlushPkt, mAudioPacketQueue)) {
                av_packet_free(&audioFlushPkt);
                av_freep(&audioFlushPkt);
            }
        }
        LOGE("read packet...end or failed: %d", ret)

    }

    if (!suc) {
        LOGI("av_read_frame, other...pts: %" PRId64 ", index: %d", avPacket->pts,
             avPacket->stream_index)
        av_packet_free(&avPacket);
    }
    return isEnd;
}


bool FFMpegPlayer::pushPacketToQueue(AVPacket *packet,
                                     const std::shared_ptr<AVPacketQueue> &queue) const {
    if (queue == nullptr) {
        return false;
    }

    while (queue->isFull()) {
        if (mHasAbort) {
            return false;
        }
        LOGI("pushPacketToQueue is full, wait start")
        queue->wait();
        LOGI("pushPacketToQueue is full, wait end")
        if (mHasAbort) {
            return false;
        }
        if (mIsSeek && queue->isEmpty()) {
            LOGI("pushPacketToQueue is full, wait end and filter")
            return false;
        }
    }
    LOGI("pushPacketToQueue pts:%ld size:%d", packet->pts, packet->size)
    if (packet->size == 0 && packet->data == nullptr) {
        if (queue->checkLastIsEofPack()) {
            LOGI("pushPacketToQueue last is eof.filter curr packet")
            return false;
        }
    }
    queue->push(packet);
    return true;
}

bool FFMpegPlayer::pushFrameToQueue(AVFrame *frame, const std::shared_ptr<AVFrameQueue> &queue,
                                    bool fornt) {
    if (queue == nullptr) {
        return false;
    }

    bool suc = false;
    while (queue->isFull()) {
        if (mHasAbort) {
            return false;
        }
        LOGI("pushFrameToQueue is full, wait start")
        queue->wait();
        LOGI("pushFrameToQueue is full, wait end")
        if (mHasAbort) {
            return false;
        }
        if (mIsSeek && queue->isEmpty()) {
            LOGI("pushFrameToQueue is full, wait end and filter")
            return false;
        }

    }
    LOGI("pushFrameToQueue pts:%ld(%f),pkt_size:(%d),mCurrSeekTime:%ld,mIsBackSeek:%d", frame->pts,
         frame->pts * av_q2d(frame->time_base), frame->pkt_size, mCurrSeekTime, mIsBackSeek)
    if (frame->pkt_size == 0) {
        if (queue->checkLastIsEofFrame()) {
            LOGI("pushFrameToQueue last is eof.filter curr frame")
            return false;
        }
    }
    //seek情况下需要满足条件才notify,否则拿的frame太前了
    bool notify = true;
    if (mIsSeek) {
        double pts = frame->pts * av_q2d(frame->time_base) * 1000;
        if (pts <= mCurrSeekTime) {
            //seek不包含=,push需要排除=
            notify = false;
        }
        if (frame->pkt_size == 0) {
            notify = true;
        }
        LOGI("pushFrameToQueue isseek pts:%lf mCurrSeekTime:%ld notify:%d", pts, mCurrSeekTime,
             notify)
    }
    if (fornt) {
        queue->pushFront(frame);
    } else {
        queue->pushBack(frame, notify);
    }
    //seek后,拿到的time还不满足条件就已经full
    if (mIsSeek && !notify && queue->isFull()) {
        LOGI("pushFrameToQueue is seek,full and no find frame,clear")
        queue->clear(false);
    }
    if (mShowFirstFrame) {
        mVideoDecoder->resultCallback(frame);
        mShowFirstFrame = false;
    }
    suc = true;
    return suc;
}

void FFMpegPlayer::updatePlayerState(PlayerState state) {
    if (mPlayerState != state) {
        LOGI("updatePlayerState from %d to %d", mPlayerState, state);
        mPlayerState = state;
    }
}

void FFMpegPlayer::onPlayCompleted(JNIEnv *env) {
    updatePlayerState(PlayerState::PAUSE);
    if (mPlayerJni.isValid()) {
        env->CallVoidMethod(mPlayerJni.instance, mPlayerJni.onPlayCompleted);
    }
}

void FFMpegPlayer::surfaceReCreate(JNIEnv *env, jobject surface) {
    mVideoDecoder->surfaceReCreate(env, surface);
}

void FFMpegPlayer::surfaceDestroy(JNIEnv *env) {
    mVideoDecoder->surfaceDestroy(env);
}

int64_t FFMpegPlayer::getCurrTimestamp() {
    if (mVideoDecoder) {
        return mVideoDecoder->getTimestamp();
    }
    return 0;
}

int FFMpegPlayer::getPlayerState() {
    AVFrame *front = mVideoFrameQueue->front();
    AVFrame *back = mVideoFrameQueue->back();
    LOGI("cache time,front:%f back:%f", front->pts * av_q2d(front->time_base),
         back->pts * av_q2d(back->time_base))
    return mPlayerState;
}

void
FFMpegPlayer::cutting(JNIEnv *env, const char *srcPath, const char *destPath, jlong starttime,
                      jlong endtime,
                      jobject out_config, jobject callback) {
    pause();
    double start_time = starttime / 1000.0;
    double end_time = endtime / 1000.0;
    double progress = 0;
    // callback
    jclass jcalss = env->GetObjectClass(callback);
    jmethodID onStart = env->GetMethodID(jcalss, "onStart", "()V");
    jmethodID onProgress = env->GetMethodID(jcalss, "onProgress", "(D)V");
    jmethodID onFail = env->GetMethodID(jcalss, "onFail", "(I)V");
    jmethodID onDone = env->GetMethodID(jcalss, "onDone", "()V");
    env->CallVoidMethod(callback, onStart);

    int result = 0;

    int outFps = (int) mVideoDecoder->getTargetFps();
    AVStream *inSteram = mVideoDecoder->getStream();
    AVCodecParameters *codec_params = inSteram->codecpar;
    AVCodecContext *decodecContext = mVideoDecoder->getCodecContext();

    AVRational outTimeBase = {1, outFps * TimeBaseDiff};
    LOGI("cutting config,start_time:%lf end_time:%lf fps:%d timeBase:{%d,%d}", start_time, end_time,
         outFps, outTimeBase.num, outTimeBase.den)


    AVFormatContext *outFormatContext = avformat_alloc_context();
    result = avformat_alloc_output_context2(&outFormatContext, NULL, NULL, destPath);
    if (result != 0) {
        LOGE("cutting avformat_alloc_output_context2 fail,%d %s", result, av_err2str(result))
        env->CallVoidMethod(callback, onFail, ERRORCODE_ALLOC_OUTPUT_CONTEXT);
        return;
    }

    AVStream *outStream = avformat_new_stream(outFormatContext, NULL);
    if (!outStream) {
        LOGE("cutting avformat_new_stream fail,%d %s", result, av_err2str(result))
        env->CallVoidMethod(callback, onFail, ERRORCODE_COMMON);
        return;
    }
    if (avcodec_parameters_copy(outStream->codecpar, codec_params) < 0) {
        LOGE("cutting avcodec_parameters_copy fail,%d %s", result, av_err2str(result))
        env->CallVoidMethod(callback, onFail, ERRORCODE_COMMON);
        return;
    }
    //设置对应参数
    outStream->codecpar->codec_id = AV_CODEC_ID_H264;
    outStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    outStream->codecpar->codec_tag = 0;
    outStream->avg_frame_rate = {outFps, 1};
    outStream->time_base = outTimeBase;
    outStream->start_time = 0;
    outStream->codecpar->width = mVideoDecoder->getTargetWidth();
    outStream->codecpar->height = mVideoDecoder->getTargetHeight();
    //    av_dict_copy(&outStream->metadata, inSteram->metadata, 0);
    //    outStream->side_data = inSteram->side_data;
    //    outStream->nb_side_data = inSteram->nb_side_data;

    const AVCodec *encoder = avcodec_find_encoder(outStream->codecpar->codec_id);
    if (!encoder) {
        LOGE("cutting not find encoder")
        env->CallVoidMethod(callback, onFail, ERRORCODE_NOT_FIND_ENCODE);
        return;
    }
    LOGI("cutting find encoder %s", encoder->name)
    AVCodecContext *encodeContext = avcodec_alloc_context3(encoder);
    encodeContext->codec_type = AVMEDIA_TYPE_VIDEO;
    encodeContext->width = outStream->codecpar->width;
    encodeContext->height = outStream->codecpar->height;
    encodeContext->time_base = outStream->time_base;
    encodeContext->framerate = outStream->avg_frame_rate;
    if (encoder->pix_fmts) {
        //设置解码器支持的第一个
        encodeContext->pix_fmt = encoder->pix_fmts[0];
    } else {
        encodeContext->pix_fmt = decodecContext->pix_fmt;
    }
    //    encodeContext->sample_aspect_ratio = decodecContext->sample_aspect_ratio;
    encodeContext->max_b_frames = 0;//不需要B帧
    encodeContext->gop_size = outStream->avg_frame_rate.num;//多少帧一个I帧
    encodeContext->qmax = 35;
    encodeContext->qmin = 10;
    encodeContext->bit_rate = 300*1000;

    //x264 ,设置编译速度,ultrafast,superfast,veryfast
    result = av_opt_set(encodeContext->priv_data, "preset", "veryfast", 0);
    if (result != 0) {
        LOGI("cutting preset set fail,%d %s", result, av_err2str(result))
    }

    //openh264,使用比特率模式
    result = av_opt_set(encodeContext->priv_data, "rc_mode", "bitrate", 0);
    if (result != 0) {
        LOGI("cutting rc_mode set fail,%d %s", result, av_err2str(result))
    }
    result = av_opt_set(encodeContext->priv_data, "profile", "high", 0);
    if (result != 0) {
        LOGI("cutting profile set fail,%d %s", result, av_err2str(result))
    }

    result = avcodec_open2(encodeContext, encoder, NULL);
    if (result != 0) {
        LOGI("cutting avcodec_open2 fail,%d %s", result, av_err2str(result))
        env->CallVoidMethod(callback, onFail, ERRORCODE_AVCODEC_OPEN2);
        return;
    }

    result = avcodec_parameters_from_context(outStream->codecpar, encodeContext);
    if (result < 0) {
        LOGE("cutting avcodec_parameters_from_context fail,%d %s", result, av_err2str(result))
        env->CallVoidMethod(callback, onFail, ERRORCODE_PARAMETERS_TO_CONTEXT);
        return;
    }
    // 打开输出文件
    if (avio_open(&outFormatContext->pb, destPath, AVIO_FLAG_WRITE) < 0 ||
            avformat_write_header(outFormatContext, NULL) < 0) {
        LOGE("cutting not open des");
        env->CallVoidMethod(callback, onFail, ERRORCODE_OPEN_FILE);
        return;
    }

    jdouble frameCount = outFps * (end_time - start_time);
    progress += 1;
    env->CallVoidMethod(callback, onProgress, progress);
    if (mVideoFrameQueue->isEmpty()) {
        LOGI("cutting is empty")
        mVideoFrameQueue->wait();
    }
    AVFrame *front = mVideoFrameQueue->front();
    bool clear = true;
    if (front) {
        double firstTime = front->pts * av_q2d(front->time_base);
        clear = firstTime > start_time;
        LOGI("cutting cache front frame time:%f,clear:%d", firstTime, clear)
    }
    bool pop = true;
    AVFrame *back = mVideoFrameQueue->back();
    if (back) {
        double backTime = back->pts * av_q2d(back->time_base);
        pop = backTime < end_time;
        if (back->pkt_size == 0) {
            pop = false;
        }
        LOGI("cutting cache back frame time:%f,pop:%d", backTime, pop)
    }
    if (clear) {
        LOGI("cutting clear and seek to cutting start time")
        pop = true;
        mVideoDecoder->seekLock();
        mCurrSeekTime = starttime;
        mIsSeek = true;
        mVideoDecoder->seek(starttime);
        mVideoPacketQueue->clear();
        mVideoFrameQueue->clear(true);
        mVideoDecoder->seekUnlock();
        mVideoFrameQueue->wait();
    }

    mVideoFrameQueue->resetIndex();
    int writeFramsCount = 0;
    int pts = 0;
    while (1) {
        int sendResult = 0;
        int receiveResult = 0;
        if (mVideoFrameQueue->isEmpty()) {
            mVideoFrameQueue->wait();
        }
        AVFrame *cacheFrame = mVideoFrameQueue->getFrame(pop, false);
        if (cacheFrame == nullptr) {
            mVideoFrameQueue->wait();
            continue;
        }
        if (cacheFrame->pkt_size == 0) {
            //eof
            LOGI("cutting eof")
            break;
        }
        double time = cacheFrame->pts * av_q2d(cacheFrame->time_base);
        if (time < start_time) {
            LOGI("cutting time(%f) < start_time(%f) ", time, start_time)
            continue;
        }
        if (time >= end_time) {
            LOGI("cutting time(%f) > end_time(%f) ", time, end_time)
            break;
        }
        LOGI("cutting red %f", time)
        AVFrame *frame = av_frame_alloc();
        av_frame_ref(frame, cacheFrame);
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        frame->pts = pts;
        pts++;

        do {
            sendResult = -1;
            receiveResult = -1;
            sendResult = avcodec_send_frame(encodeContext, frame);
            if (sendResult == 0) {
                AVPacket *receivePacket = av_packet_alloc();
                receiveResult = avcodec_receive_packet(encodeContext, receivePacket);
                if (receiveResult == 0) {
                    receivePacket->stream_index = outStream->index;
                    receivePacket->pts = writeFramsCount;
                    receivePacket->dts = receivePacket->pts - TimeBaseDiff;
                    receivePacket->duration = TimeBaseDiff;

                    LOGI("cutting av_interleaved_write_frame result pts:%ld(%f) dts:%ld %f duration:%ld %f",
                         receivePacket->pts,
                         receivePacket->pts * av_q2d(encodeContext->time_base),
                         receivePacket->dts,
                         receivePacket->dts * av_q2d(encodeContext->time_base),
                         receivePacket->duration,
                         receivePacket->duration * av_q2d(encodeContext->time_base))

                    result = av_write_frame(outFormatContext,
                                            receivePacket);
                    if (result == 0) {
                        writeFramsCount += TimeBaseDiff;
                        LOGI("cutting progress:%f %d %f",
                             writeFramsCount / TimeBaseDiff / frameCount,
                             writeFramsCount / TimeBaseDiff, frameCount)
                        progress = std::min(
                                writeFramsCount / TimeBaseDiff / frameCount * 100 + 1,
                                99.0);
                        env->CallVoidMethod(callback, onProgress, progress);
                    } else {
                        LOGI("cutting av_interleaved_write_frame fail result %d %s", result,
                             av_err2str(result))
                    }
                }

                av_packet_free(&receivePacket);
            }

            LOGI("cutting encode sendResult:%d receiveResult:%d", sendResult, receiveResult)
        } while (sendResult == AVERROR(EAGAIN) && receiveResult == AVERROR(EAGAIN));
        av_frame_unref(frame);
        av_frame_free(&frame);
        if (pop) {
            av_frame_free(&cacheFrame);
        }
        LOGI("cutting av_write_frame")
    }

    int sendResult = -1;
    int receiveResult = -1;

    do {
        sendResult = -1;
        receiveResult = -1;
        AVPacket *receivePacket = av_packet_alloc();
        sendResult = avcodec_send_frame(encodeContext, nullptr);
        if (sendResult == 0 || sendResult == AVERROR_EOF) {
            receiveResult = avcodec_receive_packet(encodeContext, receivePacket);
            if (receiveResult == 0) {
                receivePacket->stream_index = outStream->index;
                receivePacket->pts = writeFramsCount;
                receivePacket->dts = receivePacket->pts - TimeBaseDiff;
                receivePacket->duration = TimeBaseDiff;

                LOGI("cutting av_interleaved_write_frame result flush pts:%ld %f dts:%ld %f duration:%ld %f",
                     receivePacket->pts,
                     receivePacket->pts * av_q2d(encodeContext->time_base),
                     receivePacket->dts,
                     receivePacket->dts * av_q2d(encodeContext->time_base),
                     receivePacket->duration,
                     receivePacket->duration * av_q2d(encodeContext->time_base))

                result = av_write_frame(outFormatContext,
                                        receivePacket);
                if (result == 0) {
                    writeFramsCount += TimeBaseDiff;
                    LOGI("cutting progress:%f %d %f",
                         writeFramsCount / TimeBaseDiff / frameCount,
                         writeFramsCount / TimeBaseDiff, frameCount)
                    progress = std::min(
                            writeFramsCount / TimeBaseDiff / frameCount * 100 - 1,
                            99.0);
                    env->CallVoidMethod(callback, onProgress, progress);
                } else {
                    LOGI("cutting av_interleaved_write_frame fail flush result %d %s", result,
                         av_err2str(result))
                }
            }
        }
        LOGI("cutting encode flush sendResult:%d %s receiveResult:%d ",
             sendResult, av_err2str(sendResult), receiveResult)
        av_packet_free(&receivePacket);
    } while (sendResult >= 0 || receiveResult >= 0);
    av_write_trailer(outFormatContext);
    // 关闭输出文件
    avio_close(outFormatContext->pb);
    avformat_free_context(outFormatContext);
    outFormatContext = nullptr;

    avcodec_close(encodeContext);
    avcodec_free_context(&encodeContext);
    LOGI("cutting done!!!")
    progress = 100;
    env->CallVoidMethod(callback, onProgress, progress);
    env->CallVoidMethod(callback, onDone);
    jcalss = nullptr;
    onStart = nullptr;
    onProgress = nullptr;
    onDone = nullptr;
    onFail = nullptr;
}