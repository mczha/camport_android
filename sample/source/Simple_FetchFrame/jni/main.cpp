#include "common/common.hpp"
#include <sys/time.h>  

static char buffer[1024*1024];
static int  n;
static volatile bool exit_main;

static int fps_counter = 0;
static clock_t fps_tm = 0;

int get_fps() {
  const int kMaxCounter = 20;
  struct timeval start;
  fps_counter++;
  if (fps_counter < kMaxCounter) {
    return -1;
  }

  gettimeofday(&start, NULL);
  int elapse = start.tv_sec * 1000 + start.tv_usec / 1000 - fps_tm;
  int v = (int)(((float)fps_counter) / elapse * 1000);
  gettimeofday(&start, NULL);
  fps_tm = start.tv_sec * 1000 + start.tv_usec / 1000;

  fps_counter = 0;
  return v;
}

struct CallbackData {
    int             index;
    TY_DEV_HANDLE   hDevice;
    DepthRender*    render;
};

void frameHandler(TY_FRAME_DATA* frame, void* userdata)
{
    CallbackData* pData = (CallbackData*) userdata;
    LOGD("=== Get frame %d", ++pData->index);

    int ret = get_fps();
	if (ret > 0)
        LOGD("fps: %d\n", ret);

    for( int i = 0; i < frame->validCount; i++ ){
        // get & show depth image
        if(frame->image[i].componentID == TY_COMPONENT_DEPTH_CAM){
            cv::Mat depth(frame->image[i].height, frame->image[i].width
                    , CV_16U, frame->image[i].buffer);
            cv::Mat colorDepth = pData->render->Compute(depth);
            //printf("compute depth\n");
        }
        // get & show left ir image
        if(frame->image[i].componentID == TY_COMPONENT_IR_CAM_LEFT){
            cv::Mat leftIR(frame->image[i].height, frame->image[i].width
                    , CV_8U, frame->image[i].buffer);
        }
        // get & show right ir image
        if(frame->image[i].componentID == TY_COMPONENT_IR_CAM_RIGHT){
            cv::Mat rightIR(frame->image[i].height, frame->image[i].width
                    , CV_8U, frame->image[i].buffer);
        }
        // get point3D
        if(frame->image[i].componentID == TY_COMPONENT_POINT3D_CAM){
            cv::Mat point3D(frame->image[i].height, frame->image[i].width
                    , CV_32FC3, frame->image[i].buffer);
        }
        // get & show RGB
        if(frame->image[i].componentID == TY_COMPONENT_RGB_CAM){
            /*  
            cv::Mat rgb(frame->image[i].height, frame->image[i].width
                    , CV_8UC3, frame->image[i].buffer);
            cv::Mat bgr;
            cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
            */
        }
    }

    LOGD("=== Callback: Re-enqueue buffer(%p, %d)", frame->userBuffer, frame->bufferSize);
    ASSERT_OK( TYEnqueueBuffer(pData->hDevice, frame->userBuffer, frame->bufferSize) );
}

int main()
{
    LOGD("=== Init lib");
    ASSERT_OK( TYInitLib() );
    TY_VERSION_INFO* pVer = (TY_VERSION_INFO*)buffer;
    ASSERT_OK( TYLibVersion(pVer) );
    LOGD("     - lib version: %d.%d.%d", pVer->major, pVer->minor, pVer->patch);

    ASSERT_OK( TYGetDeviceNumber(&n) );
    LOGD("     - device number %d", n);

    TY_DEVICE_BASE_INFO* pBaseInfo = (TY_DEVICE_BASE_INFO*)buffer;
    ASSERT_OK( TYGetDeviceList(pBaseInfo, 100, &n) );

    if(n == 0){
        LOGD("=== No device got");
        return -1;
    }

    TY_DEV_HANDLE hDevice;
    ASSERT_OK( TYOpenDevice(pBaseInfo[0].id, &hDevice) );

    int32_t allComps;
    ASSERT_OK( TYGetComponentIDs(hDevice, &allComps) );
    if(allComps & TY_COMPONENT_RGB_CAM){
        LOGD("=== Has RGB camera, open RGB cam");
        ASSERT_OK( TYEnableComponents(hDevice, TY_COMPONENT_RGB_CAM) );
    }

    LOGD("=== Configure components, open depth cam");
    int32_t componentIDs = TY_COMPONENT_DEPTH_CAM | TY_COMPONENT_IR_CAM_LEFT | TY_COMPONENT_IR_CAM_RIGHT;
    ASSERT_OK( TYEnableComponents(hDevice, componentIDs) );

    LOGD("=== Configure feature, set resolution to 640x480.");
    LOGD("Note: DM460 resolution feature is in component TY_COMPONENT_DEVICE,");
    LOGD("      other device may lays in some other components.");
    int err = TYSetEnum(hDevice, TY_COMPONENT_DEPTH_CAM, TY_ENUM_IMAGE_MODE, TY_IMAGE_MODE_640x480);
    printf("err: %d\n", err);
    ASSERT(err == TY_STATUS_OK || err == TY_STATUS_NOT_PERMITTED);

    LOGD("=== Prepare image buffer");
    int32_t frameSize;
    ASSERT_OK( TYGetFrameBufferSize(hDevice, &frameSize) );
    LOGD("     - Get size of framebuffer, %d", frameSize);
    ASSERT( frameSize >= 640*480*2 );

    LOGD("     - Allocate & enqueue buffers");
    char* frameBuffer[2];
    frameBuffer[0] = new char[frameSize];
    frameBuffer[1] = new char[frameSize];
    LOGD("     - Enqueue buffer (%p, %d)", frameBuffer[0], frameSize);
    ASSERT_OK( TYEnqueueBuffer(hDevice, frameBuffer[0], frameSize) );
    LOGD("     - Enqueue buffer (%p, %d)", frameBuffer[1], frameSize);
    ASSERT_OK( TYEnqueueBuffer(hDevice, frameBuffer[1], frameSize) );

    LOGD("=== Register callback");
    LOGD("Note: Callback may block internal data receiving,");
    LOGD("      so that user should not do long time work in callback.");
    LOGD("      To avoid copying data, we pop the framebuffer from buffer queue and");
    LOGD("      give it back to user, user should call TYEnqueueBuffer to re-enqueue it.");
    DepthRender render;
    CallbackData cb_data;
    cb_data.index = 0;
    cb_data.hDevice = hDevice;
    cb_data.render = &render;
    // ASSERT_OK( TYRegisterCallback(hDevice, frameHandler, &cb_data) );

    LOGD("=== Disable trigger mode");
    ASSERT_OK( TYSetBool(hDevice, TY_COMPONENT_DEVICE, TY_BOOL_TRIGGER_MODE, false) );

    LOGD("=== Start capture");
    ASSERT_OK( TYStartCapture(hDevice) );

    LOGD("=== While loop to fetch frame");
    exit_main = false;
    TY_FRAME_DATA frame;

    while(!exit_main){
        int err = TYFetchFrame(hDevice, &frame, -1);
        if( err != TY_STATUS_OK ){
            LOGD("... Drop one frame");
            continue;
        }

        frameHandler(&frame, &cb_data);
    }

    ASSERT_OK( TYStopCapture(hDevice) );
    ASSERT_OK( TYCloseDevice(hDevice) );
    ASSERT_OK( TYDeinitLib() );
    // MSLEEP(10); // sleep to ensure buffer is not used any more
    delete frameBuffer[0];
    delete frameBuffer[1];

    LOGD("=== Main done!");
    return 0;
}
