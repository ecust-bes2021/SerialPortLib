# SerialPortLib 变更日志
该格式基于 [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)，并遵循 [Semantic Versioning](https://semver.org/spec/v2.0.0.html)。

## [0.0.2] - 2025-12-15
### 修改
- 在TT工具或log2bin工具在使用中，发现还是会出现串口在，adb没有设备，串口不吐Log，但是通过CRT工具打开后串口吐log，但adb仍无设备，目前的策略是：强制拉高DTR与RTS,这样效果就与CRT一致了；
  ```c
    EscapeCommFunction(handle, SETDTR);  // Force DTR high
    EscapeCommFunction(handle, SETRTS);  // Force RTS high
  ```

## [0.0.1] - 2025-12-3
### 修改
- 设置参数拉高DTR与RTS，与CRT一致
  ```c
    dcbSerialParams.fOutxCtsFlow = TRUE;
    dcbSerialParams.fRtsControl = RTS_CONTROL_ENABLE;
    dcbSerialParams.fOutxDsrFlow = TRUE;
    dcbSerialParams.fDtrControl = DTR_CONTROL_ENABLE;
  ```

