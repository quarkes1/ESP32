# ESP32 开发指南 (EndeavourOS + PlatformIO)

> 本指南适用于 `~/项目/ESP32/` 下的所有 ESP32 子项目。

## 目录结构

```
~/项目/ESP32/
├── README.md                ← 本指南
├── SekaisenDetector/        ← 示例：世界线探测仪
│   ├── platformio.ini
│   └── src/main.cpp
└── YourNewProject/          ← 新项目放这里即可
    ├── platformio.ini
    └── src/main.cpp
```

## 环境

- **IDE**: VSCode + PlatformIO 扩展
- **框架**: Arduino (ESP32)
- **编译器**: xtensa-esp32-elf-gcc 8.4.0

---

## 新建项目

```bash
mkdir 项目名 && cd 项目名
pio init --board esp32dev
```

自动生成 `platformio.ini` 和 `src/` 目录。

> 常用开发板: `esp32dev` | `esp32-c3-devkitm-1` | `esp32-s3-devkitc-1`。查看全部: `pio boards espressif32`

---

## 编写代码

将 `.cpp` 源码放入 `src/`，入口为 `src/main.cpp`。

**Arduino `.ino` 移植注意事项：**
- 重命名为 `.cpp`
- 文件顶部加 `#include <Arduino.h>`
- 添加函数前置声明（Arduino IDE 会自动生成，但 C++ 不会）

---

## 编译 & 烧录

```bash
cd 项目目录

# 仅编译
pio run

# 编译 + 烧录
pio run -t upload

# 编译 + 烧录 + 串口监视（一键）
pio run -t upload -t monitor
```

---

## 串口监视

```bash
pio device monitor          # 默认 9600 波特率
pio device monitor -b 115200  # 指定波特率
```

---

## IntelliSense 修复

VSCode 中 `#include` 报红时执行：

```bash
pio run -t compiledb
```

已配置全局 `C_Cpp.default.compileCommands`，生成的 `compile_commands.json` 会自动被 VSCode 读取。

---

## 项目结构参考

```
项目目录/
├── platformio.ini          # 板型、框架、波特率配置
├── compile_commands.json   # IntelliSense 索引（pio run -t compiledb 生成）
├── src/
│   └── main.cpp            # 主程序
└── .pio/                   # 编译产物（自动生成，勿手动修改）
```

---

## 常用命令速查

| 操作    | 命令 |
|------  |------|
| 新建项目 | `pio init --board esp32dev` |
| 编译    | `pio run` |
| 烧录    | `pio run -t upload` |
| 串口监视 | `pio device monitor` |
| 编译+烧录+监视 | `pio run -t upload -t monitor` |
| 生成 IntelliSense | `pio run -t compiledb` |
| 查看可用开发板 | `pio boards espressif32` |
| 清理编译产物 | `pio run -t clean` |
| 查看库依赖 | `pio pkg list` |
