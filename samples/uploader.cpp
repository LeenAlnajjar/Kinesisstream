#include "KinesisVideoProducer.h"

#define DEFAULT_RETENTION_PERIOD          480 * HUNDREDS_OF_NANOS_IN_AN_HOUR
#define DEFAULT_BUFFER_DURATION           1200 * HUNDREDS_OF_NANOS_IN_A_SECOND //120 seconds
#define DEFAULT_CALLBACK_CHAIN_COUNT      5
#define DEFAULT_KEY_FRAME_INTERVAL        45
#define DEFAULT_FPS_VALUE                 25
#define DEFAULT_STREAM_DURATION           20 * HUNDREDS_OF_NANOS_IN_A_SECOND
#define DEFAULT_STORAGE_SIZE              20 * 1024 * 1024
#define RECORDED_FRAME_AVG_BITRATE_BIT_PS 3800000
#define NUMBER_OF_FRAME_FILES 403

class EdgeSource {
    public:
        virtual void start() = 0; 
        virtual void read(PFrame pFrame) = 0; 
};

class EdgeSink {
    public:
        virtual void start() = 0; 
        virtual void write(PFrame pFrame) = 0; 
};

class FileSource: public EdgeSource {

    private:
        CHAR frameFilePath[MAX_PATH_LEN + 1];

    public:
        void start() {
            
            /* obtain files from the storage by using the following parameters
               streamName / streamId, 
               starttimestamp, 
               endtimestamp,
               filepath
               
            */
            
            MEMSET(frameFilePath, 0x00, MAX_PATH_LEN + 1);
            STRCPY(frameFilePath, (PCHAR) "../samples/h264SampleFrames");
            
        }
        
        void read(PFrame pFrame) {
            CHAR filePath[MAX_PATH_LEN + 1];
            UINT32 index;
            UINT64 size;
            
            /* parameters needed in the metadata file to construct a frame from the file
               isKeyFrame
               fps
               decoding ts
               presentation ts
               frame duration
               trackinfo list
            */
            
            index = pFrame->index % NUMBER_OF_FRAME_FILES + 1;
            SNPRINTF(filePath, MAX_PATH_LEN, "%s/frame-%03d.h264", frameFilePath, index);
            size = pFrame->size;

            readFile(filePath, TRUE, NULL, &size);
            readFile(filePath, TRUE, pFrame->frameData, &size);
            
            /*
             1. Decrypt the frame file and the metadata file
             2. Map the metadata to the frame
             3. Construct a frame and assign its value to pFrame
            */

            pFrame->size = (UINT32) size;
        }
};

class KvsSink: public EdgeSink {

    private:
        PDeviceInfo pDeviceInfo = NULL;
        PStreamInfo pStreamInfo = NULL;
        PClientCallbacks pClientCallbacks = NULL;
        PStreamCallbacks pStreamCallbacks = NULL;
        CLIENT_HANDLE clientHandle = INVALID_CLIENT_HANDLE_VALUE;
        STREAM_HANDLE streamHandle = INVALID_STREAM_HANDLE_VALUE;
        PCHAR accessKey = NULL, secretKey = NULL, sessionToken = NULL;
        PCHAR streamName = "poc-stream-12", region = "us-west-2", cacertPath = NULL;

    public:
        

        void authenticate() {
           
            accessKey = getenv(ACCESS_KEY_ENV_VAR);
            secretKey = getenv(SECRET_KEY_ENV_VAR);
            region = getenv(DEFAULT_REGION_ENV_VAR);
            cacertPath = getenv(CACERT_PATH_ENV_VAR);
            sessionToken = getenv(SESSION_TOKEN_ENV_VAR);
        }
        
        void initialize() {
            createDefaultDeviceInfo(&pDeviceInfo);
            pDeviceInfo->storageInfo.storageSize = DEFAULT_STORAGE_SIZE;

            createOfflineVideoStreamInfoProvider(streamName, DEFAULT_RETENTION_PERIOD, DEFAULT_BUFFER_DURATION, &pStreamInfo);
            // setStreamInfoBasedOnStorageSize(DEFAULT_STORAGE_SIZE, RECORDED_FRAME_AVG_BITRATE_BIT_PS, 1, pStreamInfo); 

            createDefaultCallbacksProviderWithAwsCredentials(accessKey, secretKey, sessionToken, MAX_UINT64, region, 
                                                            cacertPath, NULL, NULL, &pClientCallbacks);  

            createStreamCallbacks(&pStreamCallbacks);
            addStreamCallbacks(pClientCallbacks, pStreamCallbacks);

            createKinesisVideoClient(pDeviceInfo, pClientCallbacks, &clientHandle);
            createKinesisVideoStreamSync(clientHandle, pStreamInfo, &streamHandle);

        } 

        void write(PFrame pFrame) {
            putKinesisVideoFrame(streamHandle, pFrame);
        }

        void start() {
            authenticate();
            initialize();
        }   
};

UINT64 defaultGetTime() {
#if defined _WIN32 || defined _WIN64
    FILETIME fileTime;
    GetSystemTimeAsFileTime(&fileTime);

    return ((((UINT64) fileTime.dwHighDateTime << 32) | fileTime.dwLowDateTime) - TIME_DIFF_UNIX_WINDOWS_TIME);
#elif defined __MACH__ || defined __CYGWIN__
    struct timeval nowTime;
    if (0 != gettimeofday(&nowTime, NULL)) {
        return 0;
    }

    return (UINT64) nowTime.tv_sec * HUNDREDS_OF_NANOS_IN_A_SECOND + (UINT64) nowTime.tv_usec * HUNDREDS_OF_NANOS_IN_A_MICROSECOND;
#else
    struct timespec nowTime;
    clock_gettime(CLOCK_REALTIME, &nowTime);

    // The precision needs to be on a 100th nanosecond resolution
    return (UINT64) nowTime.tv_sec * HUNDREDS_OF_NANOS_IN_A_SECOND + (UINT64) nowTime.tv_nsec / DEFAULT_TIME_UNIT_IN_NANOS;
#endif
}

VOID defaultThreadSleep(UINT64 time) {
    // Time in microseconds
    UINT64 remaining_time = time / HUNDREDS_OF_NANOS_IN_A_MICROSECOND;

    // The loop will be run till the complete usleep time is reached
    while (remaining_time != 0) {
        // Covers the last case when there is residual time left and
        // when the value provided is less than or equal to MAX_UINT32
        if (remaining_time <= MAX_UINT32) {
            usleep(remaining_time);
            remaining_time = 0;
        }

        // Sleep maximum time supported by usleep repeatedly to cover large
        // sleep time cases
        else {
            usleep(MAX_UINT32);
            remaining_time = remaining_time - (UINT64) MAX_UINT32;
        }
    }
}

INT32 main(INT32 argc, CHAR* argv[]) {

    KvsSink kvssink;
    FileSource filesrc;
    Frame frame;
    BYTE frameBuffer[200000]; // Assuming this is enough
    UINT32 frameSize = SIZEOF(frameBuffer), frameIndex = 0, fileIndex = 0; 
    UINT64 streamStopTime, streamingDuration = 20 * DEFAULT_STREAM_DURATION, timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() / DEFAULT_TIME_UNIT_IN_NANOS;

    UINT64 currentTs = 1657482505000 * 10000; // july 10, 2022, 12:48:25 PDT

    MEMSET(frameBuffer, 0x00, frameSize);
    frame.frameData = frameBuffer;
    frame.version = FRAME_CURRENT_VERSION;
    frame.trackId = DEFAULT_VIDEO_TRACK_ID;
    frame.duration = HUNDREDS_OF_NANOS_IN_A_SECOND / DEFAULT_FPS_VALUE;
    frame.decodingTs = currentTs; 
    frame.presentationTs = frame.decodingTs;

    streamStopTime = 1657482505000 * 10000 + streamingDuration; // 

    filesrc.start();
    kvssink.start();

    while (currentTs < streamStopTime) {
        frame.index = frameIndex;
        frame.flags = fileIndex % DEFAULT_KEY_FRAME_INTERVAL == 0 ? FRAME_FLAG_KEY_FRAME : FRAME_FLAG_NONE;
        frame.size = SIZEOF(frameBuffer);
        
        filesrc.read(&frame);
        kvssink.write(&frame);

        currentTs += frame.duration;

        // // retry
        // do {
        //     err = kvssink.write();
        //     if(err == buffer_full) {
        //         sleep(100us);
        //     }
        // } while(error == buffer_full);

        // defaultThreadSleep(40000);

        frame.decodingTs = currentTs;
        frame.presentationTs = frame.decodingTs;
        frameIndex++;
        fileIndex++;
        fileIndex = fileIndex % NUMBER_OF_FRAME_FILES;
    }
    return 0;
}