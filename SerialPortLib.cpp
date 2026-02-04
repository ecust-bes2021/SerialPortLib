#include "SerialPortLib.h" // Include our header

// ACE Headers
#include <ace/OS_NS_unistd.h>
#include <ace/OS_NS_fcntl.h>
#include <ace/OS_NS_errno.h>
#include <ace/OS_NS_thread.h> // For ACE_thread_t, ACE_THR_FUNC_RETURN, ACE_OS::thr_create, ACE_OS::thr_join
#include <ace/Log_Msg.h>
#include <ace/Global_Macros.h> // For ACE_UNUSED_ARG
#include <ace/Synch_Traits.h> // Might need for thread primitives if not using WinAPI directly
#include <ace/Basic_Types.h> // For ACE_Atomic_Op

// Windows Headers (Required for non-standard baud rate and config)
#include <windows.h>
#include <process.h> // For _beginthreadex, _endthreadex (alternative to ACE_OS::thr_create)

#include <string>
#include <atomic> // Use C++11 atomic for thread safety flag
#include <cstdio> // For snprintf

// --- Internal State ---
static ACE_HANDLE g_serialHandle = ACE_INVALID_HANDLE;
static HANDLE g_threadHandle = NULL; // Windows thread handle
static std::atomic<bool> g_isRunning(false); // Thread running flag
static SerialDataCallback g_dataCallback = nullptr;
static SerialErrorCallback g_errorCallback = nullptr;
static void* g_userData = nullptr;

const size_t READ_BUFFER_SIZE = 4096; // Internal read buffer size
const DWORD STATUS_LOG_INTERVAL_MS = 3000; // Low-frequency status log interval
const DWORD SILENCE_THRESHOLD_MS = 1500;   // Only log when no data for this long

// --- Internal Helper Functions ---

// Windows specific function to set attributes (modified from previous example)
static int set_serial_attributes_internal(ACE_HANDLE handle, unsigned long baudRate) {
    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(handle, &dcbSerialParams)) {
        if (g_errorCallback) g_errorCallback(g_userData, GetLastError(), "GetCommState failed");
        return -1;
    }

    dcbSerialParams.BaudRate = baudRate;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;
    // --- Flow Control (Disabled to match SecureCRT behavior) ---
    dcbSerialParams.fOutxCtsFlow = FALSE;  // No CTS hardware flow control
    dcbSerialParams.fRtsControl = RTS_CONTROL_ENABLE;  // Raise RTS on open, keep high
    dcbSerialParams.fOutxDsrFlow = FALSE;  // No DSR hardware flow control
    dcbSerialParams.fDtrControl = DTR_CONTROL_ENABLE;  // Raise DTR on open, keep high

    dcbSerialParams.fOutX = TRUE;
    dcbSerialParams.fInX = TRUE;
    dcbSerialParams.fBinary = TRUE; // MUST be true for binary data
    dcbSerialParams.fAbortOnError = FALSE; // Don't abort reads/writes on error

    if (!SetCommState(handle, &dcbSerialParams)) {
        if (g_errorCallback) g_errorCallback(g_userData, GetLastError(), "SetCommState failed");
        return -1;
    }

    COMMTIMEOUTS timeouts = { 0 };
    // Configure for non-blocking read with short interval timeout
    // This allows the read loop to check the g_isRunning flag periodically
    timeouts.ReadIntervalTimeout = 50; // Max time between bytes (ms)
    timeouts.ReadTotalTimeoutMultiplier = 0; // Total timeout = constant + (multiplier * bytes)
    timeouts.ReadTotalTimeoutConstant = 100; // Constant part of total timeout (ms)
    // If ReadFile times out, it returns 0 bytes read, GetLastError() won't necessarily show an error
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 500; // Write timeout 500ms

    if (!SetCommTimeouts(handle, &timeouts)) {
        if (g_errorCallback) g_errorCallback(g_userData, GetLastError(), "SetCommTimeouts failed");
        return -1;
    }

    // Explicitly set DTR/RTS high (belt and suspenders with DTR_CONTROL_ENABLE)
    EscapeCommFunction(handle, SETDTR);
    EscapeCommFunction(handle, SETRTS);
    return 0;
}


// --- Reader Thread Function ---
// Using Windows API threading (_beginthreadex) for simplicity here
// Can also use ACE_OS::thr_create
static unsigned __stdcall ReadThreadFunc(void* param) {
    ACE_UNUSED_ARG(param);
    char read_buffer[READ_BUFFER_SIZE];
    ssize_t bytes_read; // Use ssize_t for ACE_OS::read return
    ULONGLONG last_rx_time = GetTickCount64();
    ULONGLONG last_status_time = 0;

    struct StatusSnapshot {
        bool valid = false;
        DWORD modemStatus = 0;
        DWORD errors = 0;
        DWORD cbInQue = 0;
        DWORD cbOutQue = 0;
        bool fXoffHold = false;
        bool fXoffSent = false;
        BYTE fDtrControl = 0;
        BYTE fRtsControl = 0;
        BYTE fOutX = 0;
        BYTE fInX = 0;
    };
    StatusSnapshot last;

    ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%t) Read thread started.\n")));

    while (g_isRunning.load()) { // Check atomic flag
        bytes_read = ACE_OS::read(g_serialHandle, read_buffer, READ_BUFFER_SIZE);

        if (bytes_read > 0) {
            last_rx_time = GetTickCount64();
            // Data received, call the callback
            if (g_dataCallback) {
                try {
                    g_dataCallback(g_userData, read_buffer, static_cast<size_t>(bytes_read));
                }
                catch (...) {
                    // Catch potential exceptions from user callback
                    if (g_errorCallback) g_errorCallback(g_userData, -1, "Exception in data callback");
                }
            }
        }
        else if (bytes_read == 0) {
            // Timeout occurred (based on COMMTIMEOUTS settings), this is normal
            // Allows us to loop and check g_isRunning without busy-waiting
            ULONGLONG now = GetTickCount64();
            if ((now - last_rx_time) >= SILENCE_THRESHOLD_MS &&
                (now - last_status_time) >= STATUS_LOG_INTERVAL_MS) {
                last_status_time = now;

                HANDLE h = (HANDLE)g_serialHandle;
                DWORD modemStatus = 0;
                DWORD errors = 0;
                COMSTAT comstat = { 0 };
                DCB dcb = { 0 };
                dcb.DCBlength = sizeof(dcb);

                BOOL modem_ok = GetCommModemStatus(h, &modemStatus);
                BOOL com_ok = ClearCommError(h, &errors, &comstat);
                BOOL dcb_ok = GetCommState(h, &dcb);

                StatusSnapshot cur;
                cur.valid = true;
                cur.modemStatus = modem_ok ? modemStatus : 0;
                cur.errors = com_ok ? errors : 0;
                cur.cbInQue = com_ok ? comstat.cbInQue : 0;
                cur.cbOutQue = com_ok ? comstat.cbOutQue : 0;
                cur.fXoffHold = com_ok ? (comstat.fXoffHold != 0) : false;
                cur.fXoffSent = com_ok ? (comstat.fXoffSent != 0) : false;
                cur.fDtrControl = dcb_ok ? dcb.fDtrControl : 0;
                cur.fRtsControl = dcb_ok ? dcb.fRtsControl : 0;
                cur.fOutX = dcb_ok ? dcb.fOutX : 0;
                cur.fInX = dcb_ok ? dcb.fInX : 0;

                bool changed = !last.valid ||
                               last.modemStatus != cur.modemStatus ||
                               last.errors != cur.errors ||
                               last.cbInQue != cur.cbInQue ||
                               last.cbOutQue != cur.cbOutQue ||
                               last.fXoffHold != cur.fXoffHold ||
                               last.fXoffSent != cur.fXoffSent ||
                               last.fDtrControl != cur.fDtrControl ||
                               last.fRtsControl != cur.fRtsControl ||
                               last.fOutX != cur.fOutX ||
                               last.fInX != cur.fInX;

                if (changed) {
                    char buf[256];
                    unsigned long long silence_ms = (unsigned long long)(now - last_rx_time);
                    int n = snprintf(
                        buf,
                        sizeof(buf),
                        "[STATUS] silence_ms=%llu CTS=%d DSR=%d XOFF_HOLD=%d XOFF_SENT=%d IN_Q=%lu OUT_Q=%lu DTR=%u RTS=%u OUTX=%u INX=%u ERR=0x%lX",
                        silence_ms,
                        (cur.modemStatus & MS_CTS_ON) ? 1 : 0,
                        (cur.modemStatus & MS_DSR_ON) ? 1 : 0,
                        cur.fXoffHold ? 1 : 0,
                        cur.fXoffSent ? 1 : 0,
                        cur.cbInQue,
                        cur.cbOutQue,
                        (unsigned)cur.fDtrControl,
                        (unsigned)cur.fRtsControl,
                        (unsigned)cur.fOutX,
                        (unsigned)cur.fInX,
                        cur.errors);

                    if (n > 0) {
                        if (g_errorCallback) {
                            g_errorCallback(g_userData, 0, buf);
                        } else {
                            ACE_DEBUG((LM_DEBUG, ACE_TEXT("%C\n"), buf));
                        }
                    }
                }

                last = cur;
            }
            continue;
        }
        else { // bytes_read < 0
            // An error occurred during read
            int error_code = ACE_OS::last_error(); // Use ACE wrapper for GetLastError
            std::string error_msg = "ACE_OS::read failed";
            // You might want to get a more descriptive message using FormatMessage on Windows
            if (g_errorCallback) {
                try {
                    g_errorCallback(g_userData, error_code, error_msg.c_str());
                }
                catch (...) {/*Ignore exceptions in error callback*/ }
            }
            // Decide if the error is fatal and should stop the thread
            // For now, let's break the loop on any read error
            g_isRunning.store(false); // Signal thread to stop
            break;
        }
    }

    ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%t) Read thread exiting.\n")));
    _endthreadex(0); // Windows specific thread exit
    return 0;
}


// --- Exported Function Implementations ---

SERIALPORTLIB_API int SerialPort_Open(
    const char* portName,
    unsigned long baudRate,
    SerialDataCallback dataCallback,
    SerialErrorCallback errorCallback,
    void* userData)
{
    if (g_serialHandle != ACE_INVALID_HANDLE) {
        if (g_errorCallback) g_errorCallback(userData, -1, "Port already open");
        return -1; // Already open
    }
    if (!portName || !dataCallback) {
        // Error callback might not be set yet
        return -2; // Invalid arguments
    }

    // Store callbacks and user data FIRST
    g_dataCallback = dataCallback;
    g_errorCallback = errorCallback;
    g_userData = userData;

    // Open port using ACE
    g_serialHandle = ACE_OS::open(ACE_TEXT(portName), O_RDWR);

    if (g_serialHandle == ACE_INVALID_HANDLE) {
        if (g_errorCallback) g_errorCallback(g_userData, ACE_OS::last_error(), "ACE_OS::open failed");
        g_dataCallback = nullptr; // Clear state on failure
        g_errorCallback = nullptr;
        g_userData = nullptr;
        return -3; // Failed to open port
    }

    // Configure port (baud rate, timeouts, etc.) using internal helper
    if (set_serial_attributes_internal(g_serialHandle, baudRate) != 0) {
        ACE_OS::close(g_serialHandle);
        g_serialHandle = ACE_INVALID_HANDLE;
        g_dataCallback = nullptr;
        g_errorCallback = nullptr;
        g_userData = nullptr;
        // Error callback was already called inside set_serial_attributes_internal
        return -4; // Failed to set attributes
    }

    // Set running flag and start the read thread
    g_isRunning.store(true);
    unsigned threadID;
    g_threadHandle = (HANDLE)_beginthreadex(NULL, 0, &ReadThreadFunc, NULL, 0, &threadID);

    if (g_threadHandle == NULL) {
        if (g_errorCallback) g_errorCallback(g_userData, errno, "_beginthreadex failed"); // errno might be set by C runtime
        g_isRunning.store(false);
        ACE_OS::close(g_serialHandle);
        g_serialHandle = ACE_INVALID_HANDLE;
        g_dataCallback = nullptr;
        g_errorCallback = nullptr;
        g_userData = nullptr;
        return -5; // Failed to start thread
    }

    ACE_DEBUG((LM_DEBUG, ACE_TEXT("Serial port %s opened successfully.\n"), ACE_TEXT(portName)));
    return 0; // Success
}

SERIALPORTLIB_API int SerialPort_Close() {
    if (g_serialHandle == ACE_INVALID_HANDLE) {
        return 0; // Already closed or never opened
    }

    // Signal the thread to stop
    g_isRunning.store(false);

    // Wait for the thread to finish
    if (g_threadHandle) {
        WaitForSingleObject(g_threadHandle, INFINITE); // Wait indefinitely
        CloseHandle(g_threadHandle); // Close the thread handle
        g_threadHandle = NULL;
    }

    // Close the serial port handle
    int close_result = ACE_OS::close(g_serialHandle);
    g_serialHandle = ACE_INVALID_HANDLE;

    // Clear callbacks and user data
    g_dataCallback = nullptr;
    g_errorCallback = nullptr;
    g_userData = nullptr;

    ACE_DEBUG((LM_DEBUG, ACE_TEXT("Serial port closed.\n")));

    return (close_result == 0) ? 0 : -1; // Return 0 on success, -1 on close error
}

SERIALPORTLIB_API int SerialPort_Write(const char* data, size_t length) {
    if (g_serialHandle == ACE_INVALID_HANDLE || !g_isRunning.load()) {
        if (g_errorCallback) g_errorCallback(g_userData, -1, "Port not open or write attempted while closing");
        return -1; // Port not open or closing
    }
    if (!data || length == 0) {
        return 0; // Nothing to write
    }

    ssize_t bytes_written = ACE_OS::write(g_serialHandle, data, length);

    if (bytes_written < 0) {
        if (g_errorCallback) g_errorCallback(g_userData, ACE_OS::last_error(), "ACE_OS::write failed");
        return -1; // Error during write
    }

    return static_cast<int>(bytes_written); // Return number of bytes written
}

SERIALPORTLIB_API int SerialPort_IsOpen() {
    return (g_serialHandle != ACE_INVALID_HANDLE && g_isRunning.load()) ? 1 : 0;
}

// --- DllMain (Optional but good practice) ---
BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    ACE_UNUSED_ARG(hModule);
    ACE_UNUSED_ARG(lpReserved);

    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        // Optional: Initialize ACE logging or other resources once per process
        // ACE_Log_Msg::instance()->open(...);
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("SerialPortLib DLL_PROCESS_ATTACH\n")));
        break;
    case DLL_THREAD_ATTACH:
        break;
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        // Optional: Clean up resources
        // Ensure port is closed if application terminates unexpectedly? Risky.
        // Better rely on application calling SerialPort_Close explicitly.
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("SerialPortLib DLL_PROCESS_DETACH\n")));
        // If the process is terminating uncleanly, trying to close might hang.
        // If lpReserved is non-NULL, the process is terminating normally.
        // if (lpReserved == NULL) { // Terminating due to FreeLibrary
             // if (SerialPort_IsOpen()) { SerialPort_Close(); } // Try cleanup
        // } else { // Terminating process
             // Maybe log, but don't do complex cleanup
        // }

        break;
    }
    return TRUE;
}
