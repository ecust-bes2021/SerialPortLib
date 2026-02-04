# SerialPortLib 状态日志说明文档

## 日志输出示例

```
[STATUS] silence_ms=3500 CTS=1 DSR=1 XOFF_HOLD=0 XOFF_SENT=0 IN_Q=0 OUT_Q=0 DTR=1 RTS=1 OUTX=1 INX=1 ERR=0x0
```

**正常状态**：设备连接正常，CTS/DSR 高电平，无流控阻塞，队列为空。

```
[STATUS] silence_ms=8000 CTS=1 DSR=1 XOFF_HOLD=1 XOFF_SENT=0 IN_Q=0 OUT_Q=256 DTR=1 RTS=1 OUTX=1 INX=1 ERR=0x0
```

**问题状态**：`XOFF_HOLD=1` 表示对端发送了 XOFF，你的发送被暂停，`OUT_Q=256` 表示有数据堆积在发送队列。

```
[STATUS] silence_ms=5000 CTS=0 DSR=0 XOFF_HOLD=0 XOFF_SENT=0 IN_Q=4096 OUT_Q=0 DTR=1 RTS=1 OUTX=1 INX=1 ERR=0x2
```

**问题状态**：CTS/DSR 都为低（可能设备断开），`IN_Q=4096` 输入队列满，`ERR=0x2` 表示 CE_OVERRUN 错误。

---

## 参数详细说明

| 参数 | 来源函数 | 数据结构/字段 | 含义 |
|------|----------|---------------|------|
| `silence_ms` | `GetTickCount64()` | 计算值 | 距离上次收到数据的毫秒数 |
| `CTS` | `GetCommModemStatus()` | `MS_CTS_ON` | Clear To Send 输入信号状态（1=高，0=低） |
| `DSR` | `GetCommModemStatus()` | `MS_DSR_ON` | Data Set Ready 输入信号状态（1=高，0=低） |
| `XOFF_HOLD` | `ClearCommError()` | `COMSTAT.fXoffHold` | 对端发送了 XOFF，本端发送被暂停（1=暂停） |
| `XOFF_SENT` | `ClearCommError()` | `COMSTAT.fXoffSent` | 本端发送了 XOFF，请求对端暂停发送（1=已发送） |
| `IN_Q` | `ClearCommError()` | `COMSTAT.cbInQue` | 输入缓冲区中等待读取的字节数 |
| `OUT_Q` | `ClearCommError()` | `COMSTAT.cbOutQue` | 输出缓冲区中等待发送的字节数 |
| `DTR` | `GetCommState()` | `DCB.fDtrControl` | DTR 控制模式（0=禁用，1=启用，2=握手） |
| `RTS` | `GetCommState()` | `DCB.fRtsControl` | RTS 控制模式（0=禁用，1=启用，2=握手，3=切换） |
| `OUTX` | `GetCommState()` | `DCB.fOutX` | 是否启用发送方向 XON/XOFF（1=启用） |
| `INX` | `GetCommState()` | `DCB.fInX` | 是否启用接收方向 XON/XOFF（1=启用） |
| `ERR` | `ClearCommError()` | `lpErrors` 参数 | 通信错误位掩码 |

---

## 关键参数诊断指南

### 1. XOFF_HOLD（最重要）

| 值 | 含义 | 可能原因 |
|----|------|----------|
| 0 | 正常，可以发送 | - |
| 1 | **发送被暂停** | 对端发送了 XOFF 字符（0x13），可能是对端缓冲区满或处理不过来 |

**排查**：检查设备端是否有缓冲区溢出或处理瓶颈。

### 2. XOFF_SENT

| 值 | 含义 | 可能原因 |
|----|------|----------|
| 0 | 正常 | - |
| 1 | 本端发送了 XOFF | 本端输入缓冲区快满，请求对端暂停发送 |

### 3. CTS / DSR

| CTS | DSR | 含义 |
|-----|-----|------|
| 1 | 1 | 设备连接正常，信号线正确 |
| 0 | 0 | 设备可能断开，或信号线未连接 |
| 1 | 0 | DSR 未连接（某些设备不使用） |
| 0 | 1 | CTS 未连接（某些设备不使用） |

**注意**：由于禁用了硬件流控，CTS/DSR 不会阻塞通信，但可以作为设备连接状态的参考。

### 4. IN_Q / OUT_Q

| 情况 | 含义 |
|------|------|
| `IN_Q` 持续增长 | 数据进来但没被读取，可能读取线程卡住 |
| `OUT_Q` 持续增长 | 数据发不出去，可能被 XOFF 阻塞或硬件问题 |
| 两者都为 0 | 正常，数据流通畅 |

### 5. ERR 错误码

| 位 | 宏定义 | 值 | 含义 |
|----|--------|-----|------|
| 0 | `CE_RXOVER` | 0x0001 | 接收缓冲区溢出 |
| 1 | `CE_OVERRUN` | 0x0002 | 硬件接收溢出（数据丢失） |
| 2 | `CE_RXPARITY` | 0x0004 | 奇偶校验错误 |
| 3 | `CE_FRAME` | 0x0008 | 帧错误（停止位错误） |
| 4 | `CE_BREAK` | 0x0010 | 检测到 Break 信号 |
| 8 | `CE_TXFULL` | 0x0100 | 发送缓冲区满 |

**示例**：`ERR=0x2` 表示 `CE_OVERRUN`，硬件接收溢出。

### 6. DTR / RTS 控制模式

| 值 | DTR 含义 | RTS 含义 |
|----|----------|----------|
| 0 | `DTR_CONTROL_DISABLE` - 保持低 | `RTS_CONTROL_DISABLE` - 保持低 |
| 1 | `DTR_CONTROL_ENABLE` - 保持高 | `RTS_CONTROL_ENABLE` - 保持高 |
| 2 | `DTR_CONTROL_HANDSHAKE` - 自动握手 | `RTS_CONTROL_HANDSHAKE` - 自动握手 |
| 3 | - | `RTS_CONTROL_TOGGLE` - 发送时切换 |

**当前配置**：DTR=1, RTS=1 表示两者都保持高电平。

---

## 典型问题场景

### 场景 1：突然不吐 log

```
[STATUS] silence_ms=10000 CTS=1 DSR=1 XOFF_HOLD=1 XOFF_SENT=0 IN_Q=0 OUT_Q=512 DTR=1 RTS=1 OUTX=1 INX=1 ERR=0x0
```

**诊断**：`XOFF_HOLD=1`，对端发送了 XOFF。检查设备端为什么发 XOFF（缓冲区满？处理卡住？）

### 场景 2：设备断开

```
[STATUS] silence_ms=15000 CTS=0 DSR=0 XOFF_HOLD=0 XOFF_SENT=0 IN_Q=0 OUT_Q=0 DTR=1 RTS=1 OUTX=1 INX=1 ERR=0x0
```

**诊断**：CTS=0, DSR=0，设备可能物理断开或信号线问题。

### 场景 3：数据丢失

```
[STATUS] silence_ms=5000 CTS=1 DSR=1 XOFF_HOLD=0 XOFF_SENT=0 IN_Q=0 OUT_Q=0 DTR=1 RTS=1 OUTX=1 INX=1 ERR=0x2
```

**诊断**：`ERR=0x2` (CE_OVERRUN)，硬件接收溢出，数据可能丢失。考虑降低波特率或优化读取速度。

---

## 相关 Windows API 参考

- [GetCommModemStatus](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getcommmodemstatus)
- [ClearCommError](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-clearcommerror)
- [GetCommState](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getcommstate)
- [DCB 结构体](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-dcb)
- [COMSTAT 结构体](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-comstat)
