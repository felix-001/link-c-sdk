#include "segmentmgr.h"
#include "resource.h"
#include "queue.h"
#include "tsuploaderapi.h"
#include "fixjson.h"
#include "servertime.h"
#include "b64/urlsafe_b64.h"
#include <qupload.h>
#include "httptools.h"

#define SEGMENT_RELEASE 1
#define SEGMENT_UPDATE 2
#define SEGMENT_INTERVAL 3
#define SEGMENT_SET_UPLOADZONE 4
typedef struct {
        int64_t nStart;
        int64_t nEndOrInt;
        SegmentHandle handle;
        uint8_t isRestart;
        uint8_t nOperation;
}SegInfo;

typedef struct {
        int64_t nStart;
        int64_t nEnd;
        SegmentHandle handle;
        int isRestart;
        int segUploadOk;
        char ua[32];
        LinkGetUploadParamCallback getUploadParamCallback;
        void *pGetUploadParamCallbackArg;
        char *pMgrTokenRequestUrl;
        int nMgrTokenRequestUrlLen;
        UploadStatisticCallback pUploadStatisticCb;
        void *pUploadStatArg;
        int useHttps;
        int64_t nUpdateIntervalSeconds;
        int64_t nLastUpdateTime;
        LinkUploadZone uploadZone;
}Seg;

typedef struct {
        pthread_t segMgrThread_;
        LinkCircleQueue *pSegQueue_;
        Seg handles[8];
        int nQuit_;
}SegmentMgr;

static SegmentMgr segmentMgr;
static pthread_mutex_t segMgrMutex = PTHREAD_MUTEX_INITIALIZER;
static int segMgrStarted = 0;

struct MgrToken {
        char * pData;
        int nDataLen;
        int nCurlRet;
        char *pToken;
        int nTokenLen;
        char *pUrlPath;
        int nUrlPathLen;
        int isHttps;
};

static size_t writeMoveToken(void *pTokenStr, size_t size,  size_t nmemb,  void *pUserData) {
        struct MgrToken *pToken = (struct MgrToken *)pUserData;
       
        int nTokenLen = pToken->nDataLen;
        int ret = GetJsonContentByKey(pTokenStr, "\"token\"", pToken->pData, &nTokenLen);
        if (ret != LINK_SUCCESS) {
                pToken->nCurlRet = ret;
                return 0;
        }
        pToken->pToken = pToken->pData;
        pToken->nTokenLen = nTokenLen;
        pToken->pToken[nTokenLen] = 0;
        
        int len = 0;
        if (pToken->isHttps) {
                len = snprintf(pToken->pData+nTokenLen + 1, pToken->nDataLen - nTokenLen - 1,
                         "https://rs.qiniu.com%s", pToken->pUrlPath+8);
        } else {
                len = snprintf(pToken->pData+nTokenLen + 1, pToken->nDataLen - nTokenLen - 1,
                         "http://rs.qiniu.com%s", pToken->pUrlPath+8);
        }
        pToken->pUrlPath = pToken->pData + nTokenLen + 1;
        pToken->nUrlPathLen = len-2;
        pToken->pUrlPath[len-2] = 0;
        
        return size * nmemb;
}

int getMoveToken(char *pBuf, int nBufLen, char *pUrl, char *oldkey, char *key, struct MgrToken *pToken)
{
        if (pUrl == NULL || pBuf == NULL || nBufLen <= 10)
                return LINK_ARG_ERROR;

        char oldkeyB64[96] = {0};
        char keyB64[96] = {0};
        urlsafe_b64_encode(oldkey, strlen(oldkey), oldkeyB64, sizeof(oldkeyB64) - 1);
        urlsafe_b64_encode(key, strlen(key), keyB64, sizeof(keyB64) - 1);
        
        char requetBody[256] = {0};
        snprintf(requetBody, sizeof(requetBody), "{\"key\":\"/move/%s/%s/force/false\"}", oldkeyB64, keyB64);
        //printf("=======>move url:%s\n", pUrl);
        
        char httpResp[1024+256];
        int nHttpRespBufLen = sizeof(httpResp);
        int nRealRespLen = 0;
        int ret = LinkSimpleHttpPost(pUrl, httpResp, nHttpRespBufLen, &nRealRespLen, requetBody, strlen(requetBody), NULL);
        
        if (ret != LINK_SUCCESS) {
                if (ret == LINK_BUFFER_IS_SMALL) {
                        LinkLogError("buffer is small:%d %d", sizeof(httpResp), nRealRespLen);
                }
                return ret;
        }
        
        pToken->pData = pBuf;
        pToken->nDataLen = nBufLen;
        pToken->pUrlPath = requetBody;
        pToken->nUrlPathLen = strlen(requetBody);
        pToken->nCurlRet = 0;
        
        if (writeMoveToken(httpResp, nRealRespLen, 1, pToken) == 0) {
                LinkLogError("maybe response format error:%s", httpResp);
                return LINK_JSON_FORMAT;
        }
        
        return LINK_SUCCESS;
}

static int doMove(const char *pUrl, const char *pToken) {
        //printf("------->url:%s\n------->token:%s\n", pUrl, pToken);
        
        int r = LINK_SUCCESS;
        LinkPutret putret;
        int ret = LinkMoveFile(pUrl, pToken, &putret);
        if (ret != 0) {
                LinkLogError("http err:%s", putret.error);
                r = LINK_GHTTP_FAIL;
        }
        if (putret.code > 0 && putret.code / 100 != 2) {
                LinkLogError("request err:%d[%s] [%s]",putret.code, putret.reqid, putret.error);
                LinkLogError("request resp:%s", putret.body);
                r = LINK_GHTTP_FAIL;
        }
        LinkFreePutret(&putret);
        return r;
}

static int checkShouldUpdate(Seg* pSeg) {
        int64_t nNow = LinkGetCurrentNanosecond();
        if (nNow - pSeg->nLastUpdateTime <= pSeg->nUpdateIntervalSeconds) {
                return 0;
        }
        pSeg->nLastUpdateTime = nNow;
        return 1;
}

static void setSegmentInt(SegInfo segInfo) {
        // seg/ua/segment_start_timestamp/segment_end_timestamp
        int i, idx = -1;
        for (i = 0; i < sizeof(segmentMgr.handles) / sizeof(Seg); i++) {
                if (segmentMgr.handles[i].handle == segInfo.handle) {
                        idx = i;
                        break;
                }
        }
        if (idx < 0) {
                LinkLogWarn("wrong segment handle:%d", segInfo.handle);
                return;
        }
        
        segmentMgr.handles[idx].nUpdateIntervalSeconds = segInfo.nEndOrInt;
}

static void setSegmentUploadZone(SegInfo segInfo) {
        int i, idx = -1;
        for (i = 0; i < sizeof(segmentMgr.handles) / sizeof(Seg); i++) {
                if (segmentMgr.handles[i].handle == segInfo.handle) {
                        idx = i;
                        break;
                }
        }
        if (idx < 0) {
                LinkLogWarn("wrong segment handle:%d", segInfo.handle);
                return;
        }
        
        segmentMgr.handles[idx].uploadZone = (LinkUploadZone)segInfo.nEndOrInt;
}

static void upadateSegmentFile(SegInfo segInfo) {
        
        // seg/ua/segment_start_timestamp/segment_end_timestamp
        int i, idx = -1;
        for (i = 0; i < sizeof(segmentMgr.handles) / sizeof(Seg); i++) {
                if (segmentMgr.handles[i].handle == segInfo.handle) {
                        idx = i;
                        break;
                }
        }
        if (idx < 0) {
                LinkLogWarn("wrong segment handle:%d", segInfo.handle);
                return;
        }
        if (segmentMgr.handles[idx].segUploadOk) {
                if (!checkShouldUpdate(&segmentMgr.handles[idx])) {
                        return;
                }
        }
        
        char key[64] = {0};
        memset(key, 0, sizeof(key));
        char oldKey[64] = {0};
        memset(oldKey, 0, sizeof(oldKey));
        int isNewSeg = 1;
        if (segmentMgr.handles[idx].segUploadOk == 0) {
                snprintf(key, sizeof(key), "seg/%s/%"PRId64"/%"PRId64"", segmentMgr.handles[idx].ua, segInfo.nStart, segInfo.nEndOrInt);
                segmentMgr.handles[idx].nStart = segInfo.nStart;
                segmentMgr.handles[idx].nEnd = segInfo.nEndOrInt;
        } else {
                if (segInfo.nEndOrInt < segmentMgr.handles[idx].nEnd) {
                        LinkLogDebug("not update segment:%"PRId64" %"PRId64"", segmentMgr.handles[idx].nEnd, segInfo.nEndOrInt);
                        return;
                }
                snprintf(oldKey, sizeof(oldKey), "seg/%s/%"PRId64"/%"PRId64"", segmentMgr.handles[idx].ua,
                         segmentMgr.handles[idx].nStart, segmentMgr.handles[idx].nEnd);
                snprintf(key, sizeof(key), "seg/%s/%"PRId64"/%"PRId64"", segmentMgr.handles[idx].ua,
                         segmentMgr.handles[idx].nStart, segInfo.nEndOrInt);
                segmentMgr.handles[idx].nEnd = segInfo.nEndOrInt;
                isNewSeg = 0;
        }
        
        char uptoken[1536] = {0};
        LinkUploadResult uploadResult = LINK_UPLOAD_RESULT_FAIL;
        if(!isNewSeg) {
                struct MgrToken mgrToken;
                int nUrlLen = 0;
                nUrlLen = sprintf(uptoken, "%s/%s", segmentMgr.handles[idx].pMgrTokenRequestUrl, segmentMgr.handles[idx].ua);
                uptoken[nUrlLen] = 0;
                mgrToken.isHttps = segmentMgr.handles[idx].useHttps;
                int ret = getMoveToken(uptoken, sizeof(uptoken), uptoken, oldKey, key, &mgrToken);
                if (ret != 0 || mgrToken.nCurlRet != 0) {
                        LinkLogError("getMoveToken fail:%d", ret, mgrToken.nCurlRet);
                        return;
                }
                
                ret = doMove(mgrToken.pUrlPath, mgrToken.pToken);
                if (ret != 0) {
                        LinkLogError("move %s to %s fail", oldKey, key);
                } else {
                        uploadResult = LINK_UPLOAD_RESULT_OK;
                        LinkLogDebug("move %s to %s success", oldKey, key);
                }
                
                if (segmentMgr.handles[idx].pUploadStatisticCb) {
                        segmentMgr.handles[idx].pUploadStatisticCb(segmentMgr.handles[idx].pUploadStatArg, LINK_UPLOAD_MOVE_SEG, uploadResult);
                }
                return;
        }
        
        LinkUploadParam param;
        memset(&param, 0, sizeof(param));
        param.pTokenBuf = uptoken;
        param.nTokenBufLen = sizeof(uptoken);
        int ret = segmentMgr.handles[idx].getUploadParamCallback(segmentMgr.handles[idx].pGetUploadParamCallbackArg,
                                                                      &param);
        if (ret == LINK_BUFFER_IS_SMALL) {
                LinkLogError("token buffer %d is too small. drop file:%s", sizeof(uptoken), key);
                return;
        }
        
        const char *upHost = LinkGetUploadHost(0, segmentMgr.handles[idx].uploadZone);

        
        LinkPutret putret;
        ret = LinkUploadBuffer("", 0, upHost, uptoken, key, NULL, 0, NULL, &putret);
       
        if (ret != 0) {
                LinkLogError("upload segment:%s http error:%d %s", key, ret, putret.error);
        } else {//http error
                if (putret.code / 100 == 2) {
                        segmentMgr.handles[idx].segUploadOk = 1;
                        uploadResult = LINK_UPLOAD_RESULT_OK;
                        LinkLogDebug("upload segment: %s success", key);
                } else {
                        if (putret.body != NULL) {
                                LinkLogError("upload segment:%s httpcode=%d reqid:%s errmsg=%s", key, putret.code, putret.reqid, putret.body);
                        } else {
                                LinkLogError("upload segment:%s httpcode=%d reqid:%s errmsg={not receive response}", key, putret.reqid, putret.code);
                        }
                }
        }
        
        if (segmentMgr.handles[idx].pUploadStatisticCb) {
                segmentMgr.handles[idx].pUploadStatisticCb(segmentMgr.handles[idx].pUploadStatArg, LINK_UPLOAD_SEG, uploadResult);
        }
        
        return;
}

static void linkReleaseSegmentHandle(SegmentHandle seg) {
        pthread_mutex_lock(&segMgrMutex);
        if (seg >= 0 && seg < sizeof(segmentMgr.handles) / sizeof(SegmentHandle)) {
                segmentMgr.handles[seg].handle = -1;
                segmentMgr.handles[seg].nStart = 0;
                segmentMgr.handles[seg].nEnd = 0;
                segmentMgr.handles[seg].isRestart = 0;
                segmentMgr.handles[seg].getUploadParamCallback = NULL;
                segmentMgr.handles[seg].pGetUploadParamCallbackArg = NULL;
                segmentMgr.handles[seg].useHttps = 0;
                segmentMgr.handles[seg].segUploadOk = 0;
                segmentMgr.handles[seg].nMgrTokenRequestUrlLen = 0;
                if (segmentMgr.handles[seg].pMgrTokenRequestUrl) {
                        free(segmentMgr.handles[seg].pMgrTokenRequestUrl);
                        segmentMgr.handles[seg].pMgrTokenRequestUrl = NULL;
                }
                segmentMgr.handles[seg].nLastUpdateTime = 0;
                segmentMgr.handles[seg].nUpdateIntervalSeconds = 30 * 1000000000LL;
        }
        pthread_mutex_unlock(&segMgrMutex);
        return;
}

static void * segmetMgrRun(void *_pOpaque) {
        
        LinkUploaderStatInfo info = {0};
        while(!segmentMgr.nQuit_ || info.nLen_ != 0) {
                SegInfo segInfo;
                segInfo.handle = -1;
                int ret = segmentMgr.pSegQueue_->PopWithTimeout(segmentMgr.pSegQueue_, (char *)(&segInfo), sizeof(segInfo), 24 * 60 * 60);
                
                segmentMgr.pSegQueue_->GetStatInfo(segmentMgr.pSegQueue_, &info);
                
                LinkLogDebug("segment queue:%d", info.nLen_);
                if (ret == LINK_TIMEOUT) {
                        continue;
                }
                if (ret == sizeof(segInfo)) {
                        LinkLogInfo("pop segment info:%d %"PRId64" %"PRId64"\n", segInfo.handle, segInfo.nStart, segInfo.nEndOrInt);
                        if (segInfo.handle < 0) {
                                LinkLogWarn("wrong segment handle:%d", segInfo.handle);
                        } else {
                                if (segInfo.nOperation == SEGMENT_RELEASE) {
                                        linkReleaseSegmentHandle(segInfo.handle);
                                } else if (segInfo.nOperation == SEGMENT_UPDATE) {
                                        upadateSegmentFile(segInfo);
                                } else if (segInfo.nOperation == SEGMENT_INTERVAL) {
                                        setSegmentInt(segInfo);
                                } else if (segInfo.nOperation == SEGMENT_SET_UPLOADZONE) {
                                        setSegmentUploadZone(segInfo);
                                }
                        }
                }
                segmentMgr.pSegQueue_->GetStatInfo(segmentMgr.pSegQueue_, &info);
        }
        
        return NULL;
}

int LinkInitSegmentMgr() {
        pthread_mutex_lock(&segMgrMutex);
        if (segMgrStarted) {
                pthread_mutex_unlock(&segMgrMutex);
                return LINK_SUCCESS;
        }
        int i = 0;
        for (i = 0; i < sizeof(segmentMgr.handles) / sizeof(Seg); i++) {
                segmentMgr.handles[i].handle = -1;
        }
        
        LinkCircleQueue *pQueue;
        int ret = LinkNewCircleQueue(&pQueue, 0, TSQ_FIX_LENGTH, sizeof(SegInfo), 256);
        if (ret != LINK_SUCCESS) {
                pthread_mutex_unlock(&segMgrMutex);
                return ret;
        }
        segmentMgr.pSegQueue_ = pQueue;
        
        ret = pthread_create(&segmentMgr.segMgrThread_, NULL, segmetMgrRun, NULL);
        if (ret != 0) {
                LinkDestroyQueue(&pQueue);
                pthread_mutex_unlock(&segMgrMutex);
                return LINK_THREAD_ERROR;
        }
        segMgrStarted = 1;
        pthread_mutex_unlock(&segMgrMutex);
        return LINK_SUCCESS;
}

int LinkNewSegmentHandle(SegmentHandle *pSeg, const SegmentArg *pArg) {
       pthread_mutex_lock(&segMgrMutex);
        if (!segMgrStarted) {
                pthread_mutex_unlock(&segMgrMutex);
                return LINK_NOT_INITED;
        }
        int i = 0;
        for (i = 0; i < sizeof(segmentMgr.handles) / sizeof(Seg); i++) {
                if (segmentMgr.handles[i].handle == -1) {
                        //TODO mgrTokenRequestUrl malloc and free
                        int nMoveUrlLen = pArg->nMgrTokenRequestUrlLen + 1;
                        char *pTmp = (char *)malloc(nMoveUrlLen);
                        if (pTmp == NULL) {
                                pthread_mutex_unlock(&segMgrMutex);
                                return LINK_NO_MEMORY;
                        }
                        memcpy(pTmp, pArg->pMgrTokenRequestUrl, pArg->nMgrTokenRequestUrlLen);
                        pTmp[nMoveUrlLen - 1] = 0;
                        segmentMgr.handles[i].pMgrTokenRequestUrl = pTmp;
                        segmentMgr.handles[i].nMgrTokenRequestUrlLen = pArg->nMgrTokenRequestUrlLen;
                        
                        *pSeg = i;
                        segmentMgr.handles[i].handle  = i;
                        segmentMgr.handles[i].getUploadParamCallback = pArg->getUploadParamCallback;
                        segmentMgr.handles[i].pGetUploadParamCallbackArg = pArg->pGetUploadParamCallbackArg;
                        memcpy(segmentMgr.handles[i].ua, pArg->pDeviceId, pArg->nDeviceIdLen);
                        segmentMgr.handles[i].ua[pArg->nDeviceIdLen] = 0;
                        
                        segmentMgr.handles[i].pUploadStatisticCb = pArg->pUploadStatisticCb;
                        segmentMgr.handles[i].pUploadStatArg = pArg->pUploadStatArg;
                        segmentMgr.handles[i].useHttps = pArg->useHttps;
                        segmentMgr.handles[i].nUpdateIntervalSeconds = pArg->nUpdateIntervalSeconds;
                        segmentMgr.handles[i].uploadZone = pArg->uploadZone;
                        if (pArg->nUpdateIntervalSeconds <= 0) {
                                segmentMgr.handles[i].nUpdateIntervalSeconds = 30 * 1000000000LL;
                        }
                        pthread_mutex_unlock(&segMgrMutex);
                        return LINK_SUCCESS;
                }
        }
        pthread_mutex_unlock(&segMgrMutex);
        return LINK_MAX_SEG;
}

void LinkSetSegmentUpdateInt(SegmentHandle seg, int64_t nSeconds) {
        if (!segMgrStarted) {
                return;
        }
        
        SegInfo segInfo;
        segInfo.handle = seg;
        segInfo.nStart = 0;
        segInfo.nEndOrInt = nSeconds * 1000000000;
        segInfo.isRestart = 0;
        segInfo.nOperation = SEGMENT_INTERVAL;
        
        segmentMgr.pSegQueue_->Push(segmentMgr.pSegQueue_, (char *)&segInfo, sizeof(segInfo));
        
        return;
}

void LinkSetSegmentUploadZone(SegmentHandle seg, LinkUploadZone upzone) {
        if (!segMgrStarted) {
                return;
        }
        
        SegInfo segInfo;
        segInfo.handle = seg;
        segInfo.nStart = 0;
        segInfo.nEndOrInt = (int)upzone;
        segInfo.isRestart = 0;
        segInfo.nOperation = SEGMENT_SET_UPLOADZONE;
        
        segmentMgr.pSegQueue_->Push(segmentMgr.pSegQueue_, (char *)&segInfo, sizeof(segInfo));
        
        return;
}

void LinkReleaseSegmentHandle(SegmentHandle *pSeg) {
        if (*pSeg < 0 || !segMgrStarted) {
                return;
        }
        SegInfo segInfo;
        segInfo.handle = *pSeg;
        segInfo.nStart = 0;
        segInfo.nEndOrInt = 0;
        segInfo.isRestart = 0;
        segInfo.nOperation = SEGMENT_RELEASE;
        *pSeg = -1;
        
        segmentMgr.pSegQueue_->Push(segmentMgr.pSegQueue_, (char *)&segInfo, sizeof(segInfo));
}

int LinkUpdateSegment(SegmentHandle seg, int64_t nStart, int64_t nEnd, int isRestart) {
        SegInfo segInfo;
        segInfo.handle = seg;
        segInfo.nStart = nStart;
        segInfo.nEndOrInt = nEnd;
        segInfo.isRestart = isRestart;
        segInfo.nOperation = SEGMENT_UPDATE;
        return segmentMgr.pSegQueue_->Push(segmentMgr.pSegQueue_, (char *)&segInfo, sizeof(segInfo));
}

void LinkUninitSegmentMgr() {
        pthread_mutex_lock(&segMgrMutex);
        if (!segMgrStarted) {
                pthread_mutex_unlock(&segMgrMutex);
                return;
        }
        segmentMgr.nQuit_ = 1;
        pthread_mutex_unlock(&segMgrMutex);
        pthread_join(segmentMgr.segMgrThread_, NULL);
        return;
}
