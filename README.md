# Enhanced Music Player

## 功能说明

### 已实现的增强功能

1. **播放控制**
   - 暂停/继续播放 (空格键)
   - 上一曲/下一曲 (p/n键)
   - 退出程序 (q键)

2. **多种倍速播放**
   - 0.5倍速
   - 1.0倍速 (正常)
   - 1.5倍速
   - 2.0倍速
   - 使用 's' 键循环切换

3. **快进快退功能**
   - 快进10秒 (f键)
   - 快退10秒 (b键)

4. **均衡器系统**
   - 使用FIR滤波器实现
   - 四种音效模式：
     - 正常模式
     - 低音增强模式
     - 高音增强模式  
     - 人声增强模式
   - 使用 'e' 键切换模式

5. **完整日志系统**
   - 时间戳精确到秒
   - 记录用户操作和结果
   - 记录程序运行信息（警告、错误）
   - 不在命令行打印错误，全部记录到日志文件
   - 日志文件：music_app.log

6. **状态显示**
   - 显示当前播放状态
   - 显示播放进度
   - 显示当前设置
   - 使用 'i' 键查看状态

### 操作说明

```
空格: 暂停/继续
n: 下一首
p: 上一首  
s: 切换速度 (0.5x/1x/1.5x/2x)
f: 快进10秒
b: 快退10秒
e: 切换均衡器模式
+/-: 音量调节
i: 显示状态信息
h: 显示帮助
q: 退出
```

### 连续操作支持

- 所有操作都可以连续执行
- 倍速播放时支持快进快退和上下一曲
- 暂停时除了继续和上下一曲外，其他功能被禁用
- 无需暂停或退出程序即可执行其他命令

### 编译和运行

在Linux系统上使用ALSA：
```bash
gcc -o Music_App Music_App.c -lasound -lm
./Music_App -m song.wav
```

### 日志格式示例

```
[2024-05-25 12:30:45] SYSTEM: STARTUP: Music player starting up
[2024-05-25 12:30:45] SYSTEM: INFO: Successfully opened file: song.wav
[2024-05-25 12:30:46] SYSTEM: PLAYBACK: Playback started
[2024-05-25 12:30:50] USER: User operation 'PAUSE' - SUCCESS
[2024-05-25 12:30:52] USER: User operation 'RESUME' - SUCCESS
[2024-05-25 12:30:55] USER: User operation 'CHANGE_SPEED' - SUCCESS
[2024-05-25 12:31:00] USER: User operation 'TOGGLE_EQUALIZER' - SUCCESS
```

### 技术实现

1. **FIR滤波器**: 32阶FIR滤波器实现音频均衡
2. **多倍速播放**: 通过帧跳跃和延时控制实现
3. **实时控制**: 非阻塞输入处理，响应用户命令
4. **内存管理**: 安全的缓冲区管理和错误处理
5. **状态机**: 清晰的播放状态管理