#ifndef SERIALPORTLIB_H
#define SERIALPORTLIB_H

#include <stddef.h> // For size_t

// --- DLL Export/Import Macros ---
#ifdef SERIALPORTLIB_EXPORTS // Defined when building the DLL
#define SERIALPORTLIB_API __declspec(dllexport)
#else // Defined when using the DLL
#define SERIALPORTLIB_API __declspec(dllimport)
#endif

// --- Callback Function Pointer Types ---
// Callback for received data
// Parameters: user_data (passed during Open), buffer (data received), length (bytes received)
typedef void (*SerialDataCallback)(void* user_data, const char* buffer, size_t length);

// Callback for errors
// Parameters: user_data (passed during Open), error_code (e.g., Windows GetLastError()), error_message
typedef void (*SerialErrorCallback)(void* user_data, int error_code, const char* error_message);


// --- Exported Functions ---
#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief 打开并配置串口，启动接收线程。
     *
     * @param portName 串口名称 (e.g., "\\\\.\\COM3")
     * @param baudRate 波特率 (e.g., 12000000)
     * @param dataCallback 数据接收回调函数指针
     * @param errorCallback 错误回调函数指针
     * @param userData 用户自定义数据，会回传给回调函数
     * @return int 0 表示成功, 非 0 表示失败 (可以定义具体的错误码)
     */
    SERIALPORTLIB_API int SerialPort_Open(
        const char* portName,
        unsigned long baudRate, // Make baud rate configurable
        SerialDataCallback dataCallback,
        SerialErrorCallback errorCallback,
        void* userData
    );

    /**
     * @brief 关闭串口，停止接收线程。
     *
     * @return int 0 表示成功, 非 0 表示失败
     */
    SERIALPORTLIB_API int SerialPort_Close();

    /**
     * @brief 向串口发送数据。
     *
     * @param data 要发送的数据缓冲区
     * @param length 要发送的数据长度
     * @return int 实际发送的字节数, -1 表示错误
     */
    SERIALPORTLIB_API int SerialPort_Write(const char* data, size_t length);

    /**
     * @brief 检查串口是否已打开。
     *
     * @return int 1 表示已打开, 0 表示未打开
     */
    SERIALPORTLIB_API int SerialPort_IsOpen();


#ifdef __cplusplus
}
#endif

#endif // SERIALPORTLIB_H