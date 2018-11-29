#include "picuploader.h"
#include "resource.h"
#include "queue.h"
#include "tsuploaderapi.h"
#include "fixjson.h"
#include <qupload.h>
#include "httptools.h"

#define LINK_PIC_UPLOAD_MAX_FILENAME 256
enum LinkPicUploadSignalType {
        LinkPicUploadSignalIgnore = 0,
        LinkPicUploadSignalStop,
        LinkPicUploadGetPicSignalCallback,
        LinkPicUploadSignalUpload
};

typedef struct {
        LinkAsyncInterface asyncWait_;
        pthread_t workerId_;
        LinkCircleQueue *pSignalQueue_;
        int nQuit_;
        LinkPicUploadFullArg picUpSettings_;
}PicUploader;

typedef struct {
        LinkAsyncInterface asyncWait_;
        enum LinkPicUploadSignalType signalType_;
        enum LinkPicUploadType upType_;
        const char *pData;
        int nDataLen;
        int64_t nTimestamp; //file name need
        pthread_t uploadPicThread;
        char deviceId[64];
        PicUploader *pPicUploader;
}LinkPicUploadSignal;


static void * uploadPicture(void *_pOpaque);

static int pushUploadSignal(PicUploader *pPicUploader, LinkPicUploadSignal *pSig) {
        pSig->pPicUploader = pPicUploader;
        pSig->signalType_ = LinkPicUploadSignalUpload;
        return pPicUploader->pSignalQueue_->Push(pPicUploader->pSignalQueue_, (char *)pSig, sizeof(LinkPicUploadSignal));
}

int LinkSendGetPictureSingalToPictureUploader(PictureUploader *pPicUploader, const char *pDeviceId, int nDeviceIdLen, int64_t nTimestamp) {
        LinkPicUploadSignal sig;
        memset(&sig, 0, sizeof(LinkPicUploadSignal));
        sig.signalType_ = LinkPicUploadGetPicSignalCallback;
        sig.nTimestamp = nTimestamp;
        if( nDeviceIdLen > sizeof(sig.deviceId)) {
                LinkLogWarn("deviceid too long:%d(%s)", nDeviceIdLen, pDeviceId);
                nDeviceIdLen = sizeof(sig.deviceId)-1;
        }
        memcpy(sig.deviceId, pDeviceId, nDeviceIdLen);
        PicUploader *pPicUp = (PicUploader*)pPicUploader;
        return pPicUp->pSignalQueue_->Push(pPicUp->pSignalQueue_, (char *)&sig, sizeof(LinkPicUploadSignal));
}

int LinkSendUploadPictureToPictureUploader(PictureUploader *pPicUploader, void *pOpaque, const char *pBuf, int nBuflen, enum LinkPicUploadType type) {
        PicUploader *pPicUp = (PicUploader *)pPicUploader;
        LinkPicUploadSignal* pSig = (LinkPicUploadSignal*)pOpaque;
        pSig->pPicUploader = pPicUp;
        pSig->signalType_ = LinkPicUploadSignalUpload;
        pSig->pData = pBuf;
        pSig->nDataLen = nBuflen;
        pSig->upType_ = type;
        int ret = pPicUp->pSignalQueue_->Push(pPicUp->pSignalQueue_, (char *)pSig, sizeof(LinkPicUploadSignal));
        free(pSig);
        return ret;
}

void LinkPicUploaderSetUploadZone(PictureUploader *pPicUploader, LinkUploadZone upzone) {
        PicUploader *pPicUp = (PicUploader *)pPicUploader;
        pPicUp->picUpSettings_.uploadZone = upzone;
}

static void * listenPicUpload(void *_pOpaque)
{
        PicUploader *pPicUploader = (PicUploader *)_pOpaque;
        LinkUploaderStatInfo info;
        while(!pPicUploader->nQuit_ || info.nLen_ != 0) {
                LinkPicUploadSignal sig;
                int ret = pPicUploader->pSignalQueue_->PopWithTimeout(pPicUploader->pSignalQueue_, (char *)(&sig),
                                                                       sizeof(LinkPicUploadSignal), 24 * 60 * 60);
                fprintf(stderr, "----->pu receive a signal:%d\n", sig.signalType_);
                memset(&info, 0, sizeof(info));
                pPicUploader->pSignalQueue_->GetStatInfo(pPicUploader->pSignalQueue_, &info);
                LinkLogDebug("signal queue:%d", info.nLen_);
                if (ret == LINK_TIMEOUT) {
                        continue;
                }
                LinkPicUploadSignal *pUpInfo;
                if (ret == sizeof(LinkPicUploadSignal)) {

                        switch (sig.signalType_) {
                                case LinkPicUploadSignalIgnore:
                                        LinkLogWarn("ignore signal");
                                        continue;
                                case LinkPicUploadSignalStop:
                                        return NULL;
                                case LinkPicUploadSignalUpload:
                                        pUpInfo = (LinkPicUploadSignal*)malloc(sizeof(LinkPicUploadSignal));
                                        if (pUpInfo == NULL) {
                                                LinkLogWarn("upload picture:%"PRId64" no memory", sig.nTimestamp);
                                        } else {
                                                memcpy(pUpInfo, &sig, sizeof(sig));
                                                ret = pthread_create(&pUpInfo->uploadPicThread, NULL, uploadPicture, pUpInfo);
                                        }
                                
                                        break;
                                case LinkPicUploadGetPicSignalCallback:
                                        if (pPicUploader->picUpSettings_.getPicCallback) {
                                                void *pSigBackup = NULL;
                                                pSigBackup = malloc(sizeof(sig));
                                                memcpy(pSigBackup, &sig, sizeof(sig));
                                                enum LinkGetPictureSyncMode syncMode = pPicUploader->picUpSettings_.getPicCallback(
                                                                                            pPicUploader->picUpSettings_.pGetPicCallbackOpaque,
                                                                                            pSigBackup, &sig.pData, &sig.nDataLen, &sig.upType_);
                                                if (syncMode == LinkGetPictureModeSync) {
                                                        free(pSigBackup);
                                                        pushUploadSignal(pPicUploader, &sig);
                                                }
                                        }
                                        break;
                        }

                }
        }
        return NULL;
}

static int waitUploadMgrThread(void * _pOpaque) {
        PicUploader *p = (PicUploader*)_pOpaque;
        p->nQuit_ = 1;
        
        LinkPicUploadSignal sig;
        memset(&sig, 0, sizeof(sig));
        sig.signalType_ = LinkPicUploadSignalStop;
        p->pSignalQueue_->Push(p->pSignalQueue_, (char *)&sig, sizeof(LinkPicUploadSignal));
        
        pthread_join(p->workerId_, NULL);
        LinkDestroyQueue(&p->pSignalQueue_);
        free(p);
        return 0;
}

int LinkNewPictureUploader(PictureUploader **_pPicUploader, LinkPicUploadFullArg *pArg) {
        PicUploader * pPicUploader = (PicUploader *) malloc(sizeof(PicUploader));
        if (pPicUploader == NULL) {
                return LINK_NO_MEMORY;
        }
        memset(pPicUploader, 0, sizeof(PicUploader));
        
        int ret = LinkNewCircleQueue(&pPicUploader->pSignalQueue_, 0, TSQ_FIX_LENGTH, sizeof(LinkPicUploadSignal) + sizeof(int), 50);
        if (ret != 0) {
                free(pPicUploader);
                return ret;
        }
        pPicUploader->asyncWait_.function = waitUploadMgrThread;
        pPicUploader->picUpSettings_ = *pArg;
        
        ret = pthread_create(&pPicUploader->workerId_, NULL, listenPicUpload, pPicUploader);
        if (ret != 0) {
                LinkDestroyQueue(&pPicUploader->pSignalQueue_);
                free(pPicUploader);
                return LINK_THREAD_ERROR;
        }
        *_pPicUploader = (PictureUploader *)pPicUploader;
        
        return LINK_SUCCESS;
}

static int waitUploadThread(void * _pOpaque) {
        LinkPicUploadSignal *pSig = (LinkPicUploadSignal*)_pOpaque;
        pthread_join(pSig->uploadPicThread, NULL);
        free(pSig);
        return 0;
}

static void * uploadPicture(void *_pOpaque) {
        LinkPicUploadSignal *pSig = (LinkPicUploadSignal*)_pOpaque;
        pSig->asyncWait_.function = waitUploadThread;
        
        // frame/ua/ts_start_timestamp/fragment_start_timestamp.jpeg
        char key[160] = {0};
        memset(key, 0, sizeof(key));
        snprintf(key, sizeof(key), "frame/%s/%"PRId64"/0.jpg", pSig->deviceId, pSig->nTimestamp);
        
        char uptoken[1024] = {0};
        LinkUploadParam param;
        memset(&param, 0, sizeof(param));
        param.pTokenBuf = uptoken;
        param.nTokenBufLen = sizeof(uptoken);
        int ret = pSig->pPicUploader->picUpSettings_.getUploadParamCallback(pSig->pPicUploader->picUpSettings_.pGetUploadParamCallbackOpaque,
                                                                      &param);
        if (ret == LINK_BUFFER_IS_SMALL) {
                LinkLogError("token buffer %d is too small. drop file:%s", sizeof(uptoken), key);
                LinkPushFunction(pSig);
                return NULL;
        }
        
        const char *upHost = LinkGetUploadHost(pSig->pPicUploader->picUpSettings_.useHttps, pSig->pPicUploader->picUpSettings_.uploadZone);

        LinkPutret putret;

        if (pSig->upType_ == LinkPicUploadTypeFile) {
                ret = LinkUploadFile((const char*)pSig->pData, upHost, uptoken, key, NULL, &putret);
        } else {
                ret = LinkUploadBuffer(pSig->pData, pSig->nDataLen, upHost, uptoken, key, NULL, 0, NULL, &putret);
        }
        
        LinkUploadResult uploadResult = LINK_UPLOAD_RESULT_FAIL;
        
        if (ret != 0) { //http error
                LinkLogError("upload picture:%s errorcode=%d error:%s", key, ret, putret.error);
        } else {
                if (putret.code / 100 == 2) {
                        uploadResult = LINK_UPLOAD_RESULT_OK;
                        LinkLogDebug("upload picture: %s success", key);
                } else {
                        if (putret.body != NULL) {
                                LinkLogError("upload pic:%s httpcode=%d reqid:%s errmsg=%s",
                                             key, putret.code, putret.reqid, putret.body);
                        } else {
                                LinkLogError("upload pic:%s httpcode=%d reqid:%s errmsg={not receive response}",
                                             key, putret.code, putret.reqid);
                        }
                }
        }
        
        LinkFreePutret(&putret);
        
        
        if (pSig->pPicUploader->picUpSettings_.getPictureFreeCallback) {
                pSig->pPicUploader->picUpSettings_.getPictureFreeCallback((char *)pSig->pData, pSig->nDataLen);
        }
        
        if (pSig->pPicUploader->picUpSettings_.pUploadStatisticCb) {
                pSig->pPicUploader->picUpSettings_.pUploadStatisticCb(pSig->pPicUploader->picUpSettings_.pUploadStatArg, LINK_UPLOAD_PIC, uploadResult);
        }
        
        LinkPushFunction(pSig);
        
        return NULL;
}

void LinkDestroyPictureUploader(PictureUploader **pPicUploader) {
        if (*pPicUploader == NULL)
                return;
        LinkPushFunction(*pPicUploader);
        *pPicUploader = NULL;
        return;
}
