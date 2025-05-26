
// 定义音乐全局结构体，参考 https://www.cnblogs.com/ranson7zop/p/7657874.html 表3
// int 由uint32_t代替，short 由uint16_t代替，因为在跨平台后有可能不兼容，类型长度不一致，使用统一的类型
struct WAV_HEADER
{

	char 		chunk_id[4]; // riff 标志号
	uint32_t 	chunk_size; // riff长度
	char 		format[4]; // 格式类型(wav)
	
	char 		sub_chunk1_id[4]; // fmt 格式块标识
	uint32_t 	sub_chunk1_size; // fmt 长度 格式块长度。
	uint16_t 	audio_format; // 编码格式代码									常见的 WAV 文件使用 PCM 脉冲编码调制格式,该数值通常为 1
	uint16_t 	num_channels; // 声道数 									单声道为 1,立体声或双声道为 2
	uint32_t  	sample_rate; // 采样频率 									每个声道单位时间采样次数。常用的采样频率有 11025, 22050 和 44100 kHz。
	uint32_t 	byte_rate; // 传输速率 										该数值为:声道数×采样频率×每样本的数据位数/8。播放软件利用此值可以估计缓冲区的大小。
	uint16_t	block_align; // 数据块对齐单位									采样帧大小。该数值为:声道数×位数/8。播放软件需要一次处理多个该值大小的字节数据,用该数值调整缓冲区。
	uint16_t 	bits_per_sample; // 采样位数								存储每个采样值所用的二进制数位数。常见的位数有 4、8、12、16、24、32

	char 		sub_chunk2_id[4]; // 数据  不知道什么数据
	uint32_t 	sub_chunk2_size; // 数据大小
	
} wav_header;
int wav_header_size; // 接收wav_header数据结构体的大小


/**
	ALSA的变量定义
**/
// 定义用于PCM流和硬件的 
snd_pcm_hw_params_t *hw_params;
// PCM设备的句柄  想要操作PCM设备必须定义
snd_pcm_t *pcm_handle;
// 定义pcm的name snd_pcm_open函数会用到,strdup可以直接把要复制的内容复制给没有初始化的指针，因为它会自动分配空间给目的指针，需要手动free()进行内存回收。
char *pcm_name;
// 定义是播放还是回放等等，播放流 snd_pcm_open函数会用到 可以在 https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html#gac23b43ff55add78638e503b9cc892c24 查看
snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK;
// 定义采样位数
snd_pcm_format_t pcm_format = 161;


// 缓存大小
#define BUF_LEN 1024
//char buf[BUF_LEN];
unsigned char *buff;
unsigned char *buff1;


// 周期数
int periods = 2;
// 一个周期的大小，这里虽然是设置的字节大小，但是在有时候需要将此大小转换为帧，所以在用的时候要换算成帧数大小才可以
snd_pcm_uframes_t period_size = 12 * 1024;
snd_pcm_uframes_t frames;
snd_pcm_uframes_t buffer_size;





// 初始化采样率
unsigned int rate;

// 音乐文件指针变量
FILE *fp;


// 播放状态枚举
typedef enum {
    PLAYING,
    PAUSED,
    STOPPED
} playback_state_t;

// 播放速度枚举
typedef enum {
    SPEED_0_5X = 0,
    SPEED_1_0X = 1,
    SPEED_1_5X = 2,
    SPEED_2_0X = 3
} playback_speed_t;

// 均衡器模式枚举
typedef enum {
    EQ_NORMAL = 0,
    EQ_BASS_BOOST = 1,
    EQ_TREBLE_BOOST = 2,
    EQ_VOCAL_ENHANCE = 3
} equalizer_mode_t;

// 全局播放状态变量
playback_state_t current_state;
playback_speed_t current_speed;
equalizer_mode_t current_eq_mode;
long current_position;
long total_frames;
char playlist[10][256]; // 简单播放列表，最多10首歌
int playlist_count;
int current_track;

// FIR滤波器参数
#define FIR_TAP_NUM 32
#define AUDIO_BUFFER_SIZE 4096

// 时间拉伸参数 (Time stretching parameters)
#define STRETCH_FRAME_SIZE 512
#define STRETCH_HOP_SIZE 256
#define STRETCH_OVERLAP_SIZE 256

// 日志相关
#define LOG_BUFFER_SIZE 256
void write_log(const char* type, const char* message);
void log_user_operation(const char* operation, const char* result);
void log_program_info(const char* info_type, const char* message);

// 函数声明
bool debug_msg(int result, const char *str);
void open_music_file(const char *path_name);
void toggle_pause();
void next_track();
void previous_track();
void change_speed();
void seek_forward();
void seek_backward();
void toggle_equalizer();
void apply_fir_filter(short* input, short* output, int length, equalizer_mode_t mode);
void apply_time_stretch(short* input, short* output, int input_length, int* output_length, float speed_factor, int max_output_length);
void print_status();
void handle_user_input(char input);


