#include "picuploader.h"
#include "servertime.h"

static void *gSvaeWhenAsync; //may race risk. but for test is enough

static int getPicCallback (void *pOpaque, void *pSvaeWhenAsync, OUT char **pBuf, OUT int *pBufSize, OUT enum LinkPicUploadType *pType) {
        const char *file = "./material/3c.jpg";
        int n = strlen(file)+1;
        char * pFile = (char *)malloc(n);
        memcpy(pFile, file, n);
        *pBuf = pFile;
        *pType = LinkPicUploadTypeFile;
        gSvaeWhenAsync = pSvaeWhenAsync;
        return 0;
}

static int getPictureFreeCallback (char *pBuf, int nNameBufSize) {
        fprintf(stderr, "free data\n");
        free(pBuf);
}

static char gtestToken[1024] = {0};
static int getTokenCallback(IN void *pOpaque, OUT char *pBuf, IN int nBuflen) {
        if (nBuflen < strlen(gtestToken)) {
                return LINK_BUFFER_IS_SMALL;
        }
        memcpy(pBuf, gtestToken, strlen(gtestToken));
        return LINK_SUCCESS;
}

void justTestSyncUploadPicture() {
        int ret = LinkGetUploadToken(gtestToken, sizeof(gtestToken), "http://47.105.118.51:8086/qiniu/upload/token/testdvice009");
        assert(ret == LINK_SUCCESS);
        ret = LinkInitTime();
        assert(ret == LINK_SUCCESS);
        fprintf(stderr, "token:%s\n", gtestToken);
        
        LinkPicUploadArg arg;
        arg.getPicCallback = getPicCallback;
        arg.getTokenCallback = getTokenCallback;
        arg.getPictureFreeCallback = getPictureFreeCallback;
        arg.pGetPicCallbackOpaque = NULL;
        arg.pGetTokenCallbackOpaque = NULL;
        PictureUploader *pPicUploader;
        ret = LinkNewPictureUploader(&pPicUploader, &arg);
        assert(ret == LINK_SUCCESS);
        
        int64_t ts = LinkGetCurrentNanosecond() / 1000000;
        while(1) {
                
                LinkSendSyncGetPictureSingalToPictureUploader(pPicUploader, "pic1", 4, ts);
                ts += 4990;
                sleep(5);
        }
        
}

void justTestAsyncUploadPicture() {
        int ret = LinkGetUploadToken(gtestToken, sizeof(gtestToken), "http://47.105.118.51:8086/qiniu/upload/token/testdvice009");
        assert(ret == LINK_SUCCESS);
        ret = LinkInitTime();
        assert(ret == LINK_SUCCESS);
        fprintf(stderr, "token:%s\n", gtestToken);
        
        LinkPicUploadArg arg;
        arg.getPicCallback = getPicCallback;
        arg.getTokenCallback = getTokenCallback;
        arg.getPictureFreeCallback = getPictureFreeCallback;
        arg.pGetPicCallbackOpaque = NULL;
        arg.pGetTokenCallbackOpaque = NULL;
        PictureUploader *pPicUploader;
        ret = LinkNewPictureUploader(&pPicUploader, &arg);
        assert(ret == LINK_SUCCESS);
        
        int64_t ts = LinkGetCurrentNanosecond() / 1000000;
        while(1) {
                
                LinkSendAsyncGetPictureSingalToPictureUploader(pPicUploader, "pic1", 4, ts);
                
                sleep(1);
                
                const char *file = "./material/3c.jpg";
                int n = strlen(file)+1;
                char * pFile = (char *)malloc(n);
                memcpy(pFile, file, n);
                fprintf(stderr, "send upload signal\n");
                LinkSendUploadPictureToPictureUploader(pPicUploader, gSvaeWhenAsync, pFile, n, LinkPicUploadTypeFile);
                
                sleep(4);
                ts += 4990;
        }
        
}
