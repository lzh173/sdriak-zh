#pragma once
#include <atomic>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace backend {
    struct DevVIDPID {
        uint16_t vid;
        uint16_t pid;
    };

    struct UsbDeviceHandle {
        int fd = -1;
        int vid = -1;
        int pid = -1;
        std::string path;

        bool valid() const {
            return fd >= 0 && !path.empty();
        }
    };

    class UsbDeviceLease {
    public:
        UsbDeviceLease() = default;
        explicit UsbDeviceLease(const std::vector<DevVIDPID>& allowedVidPids);
        ~UsbDeviceLease();

        UsbDeviceLease(const UsbDeviceLease&) = delete;
        UsbDeviceLease& operator=(const UsbDeviceLease&) = delete;

        UsbDeviceLease(UsbDeviceLease&& other) noexcept
            : handle(std::move(other.handle)) {
            other.handle = {};
        }

        UsbDeviceLease& operator=(UsbDeviceLease&& other) noexcept {
            if (this != &other) {
                reset();
                handle = std::move(other.handle);
                other.handle = {};
            }
            return *this;
        }

        bool acquire(const std::vector<DevVIDPID>& allowedVidPids);
        // Returns true if the USB handle was released successfully (or was already invalid).
        // Returns false if the Java-side release failed; the handle is retained in that case.
        bool reset();

        bool valid() const { return handle.valid(); }
        int fd() const { return handle.fd; }
        int vid() const { return handle.vid; }
        int pid() const { return handle.pid; }

    private:
        UsbDeviceHandle handle;
    };

    extern const std::vector<DevVIDPID> AIRSPY_VIDPIDS;
    extern const std::vector<DevVIDPID> AIRSPYHF_VIDPIDS;
    extern const std::vector<DevVIDPID> HACKRF_VIDPIDS;
    extern const std::vector<DevVIDPID> HYDRASDR_VIDPIDS;
    extern const std::vector<DevVIDPID> QMX_VIDPIDS;
    extern const std::vector<DevVIDPID> RTL_SDR_VIDPIDS;
    extern std::atomic<int> usbHotplugGeneration;
    extern std::atomic<int> audioRoutingGeneration;

    int getPreferredAudioOutputDeviceId();
    // Sticky flag to indicate that OpenGL ES is used on old Android devices
    // instead of a modern lower latency AAudio.
    void setAudioOutputUsesOpenSLES(bool usesOpenSLES);
    bool audioOutputUsesOpenSLES();
    bool hasUsbDeviceAvailable(const std::vector<DevVIDPID>& allowedVidPids);

    // Short haptic tick for UI feedback, e.g. a splitter drag handle engaging
    // (calls into MainActivity via JNI).
    void hapticTick();

    // Finish the activity and remove the task from Recents; the app is
    // visibly gone. Stop the SDR before calling (calls into MainActivity via JNI).
    void finishAppAndRemoveTask();

    // Sleep timer control (calls into MainActivity via JNI)
    int startSleepTimer();
    int stopSleepTimer();
    int suspendSleepTimer();   // window gone — release wake lock, keep startRequested
    int resumeSleepTimer();    // window back  — restart if SDR was running
    int resetSleepToActive();
    // mode: 0=Disabled, 1=KeepAlive, 2=DimScreen, 3=DimAndBlank
    // dimAfterSec / darkAfterSec: total seconds from timer start; darkAfterSec > dimAfterSec.
    void setSleepTimerConfig(int mode, int dimAfterSec, int darkAfterSec);
    // Whether to restart the SDR source automatically when the app returns to the foreground.
    void setRestartOnResume(bool value);

    // Opens the Android system document picker (Storage Access Framework) and
    // returns the chosen file as an absolute path, or an empty string if the
    // user cancelled or the pick failed. Documents that are not backed by a
    // filesystem path (cloud providers, archives) are copied into the app cache
    // first, because the native readers open files by path.
    // Blocks the calling thread until the picker returns, so only call this
    // from a worker thread, never from the render loop.
    std::string pickFile();

    // Opens the system "save as" dialog and streams [sourcePath] into the
    // document the user picks. Blocking; call from a worker thread.
    bool saveFile(const std::string& sourcePath, const std::string& suggestedName);
}
