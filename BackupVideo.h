#ifndef BACKUPVIDEO_HH_
#define BACKUPVIDEO_HH_

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <utils/misc.h>

#include <cutils/properties.h>

#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/threads.h>

namespace android {
class BackupVideo : public IBinder::DeathRecipient {
public:
    BackupVideo();
    virtual ~BackupVideo();

private:
    virtual void onFirstRef();
    /* virtual bool        threadLoop(); */
    /* virtual status_t    readyToRun(); */
    virtual void binderDied(const wp<IBinder>& who);
    int StartBackupVideo();

    pthread_t state_thread_id;
    static void* qc_thread_func(void* arg);
};
};

#endif
