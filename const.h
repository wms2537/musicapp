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

// 函数声明
bool debug_msg(int result, const char *str);
void open_music_file(const char *path_name);

#define SND_PCM_FORMAT_S24_3BE 2432 // Example, replace with actual value if different
#endif

// --- ALSA PCM Globals (Moved from MusicApp.c for clarity if they are constants or types) ---
// These were previously global in MusicApp.c. If they are truly constant or setup parameters,
// they could be here, or part of a config struct. For now, keeping them as extern declarations
// if they are modified in MusicApp.c, or define them here if they are truly constant.
// For simplicity in this step, I'll assume they are accessed from MusicApp.c.

// Example: If period_size and periods were fixed constants:
// #define DEFAULT_PERIOD_SIZE 1024 // bytes
// #define DEFAULT_PERIODS 4

// --- FIR Filter Definitions for Equalizer ---
#define MAX_FIR_TAPS 64 // Maximum number of FIR filter taps (coefficients)

typedef struct {
    char name[50];
    double coeffs[MAX_FIR_TAPS];
    int num_taps;
} FIRFilter;

// Example FIR Coefficients (These are placeholders and need proper design for desired frequency responses)
// For a real equalizer, these coefficients would be designed using filter design tools (e.g., Parks-McClellan, windowing methods).

// 1. Normal / Flat (Bypass) - a simple identity filter (delta function)
// For S16_LE, samples are short. Coefficients should sum to 1 for no gain change, or be normalized.
// These are floating point, conversion/scaling to fixed-point might be needed if performance is critical
// and floating point operations are slow on the target. Assuming floating point for now.
const FIRFilter FIR_NORMAL = {
    "Normal (Flat)",
    {1.0, 0.0, 0.0, 0.0, 0.0}, // Simplest: passes signal through
    1 // Only one tap needed for identity
};

// 2. Bass Boost (Illustrative - very simple low-pass-like, not a proper bass boost)
// This is a very naive example. A real bass boost would involve a low-shelf filter.
const FIRFilter FIR_BASS_BOOST = {
    "Bass Boost",
    // Example: A simple averaging filter which acts as a low-pass filter.
    // For more pronounced bass, coefficients would be designed to boost low frequencies.
    // These coefficients are arbitrary and will likely need significant tuning.
    // For simplicity, let's try a 5-tap filter that slightly emphasizes earlier samples.
    // This is NOT a well-designed bass boost, just a placeholder.
    {0.4, 0.3, 0.2, 0.1, 0.05}, // Needs normalization if sum != 1, and proper design
    5
};

// 3. Treble Boost (Illustrative - very simple high-pass-like, not a proper treble boost)
// This is also a naive example. A real treble boost would involve a high-shelf filter.
// Example: A simple filter emphasizing differences, which can boost higher frequencies.
// This is NOT a well-designed treble boost, just a placeholder.
// { -0.1, -0.2, 0.6, -0.2, -0.1} - sum to 0, careful with gain
// Let's try one that attempts to sharpen, again, placeholder:
const FIRFilter FIR_TREBLE_BOOST = {
    "Treble Boost",
    {-0.1, -0.15, 0.5, -0.15, -0.1}, // Sum is 0, this will likely reduce overall volume. Needs proper design.
                                 // A better simple approach might be { -0.5, 1.0, -0.5 } (sharpening) - sum is 0
                                 // Or more like: {0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1} // for smoothing, then accentuate differences
                                 // This is hard to make meaningful without design tools.
                                 // Let's use a very simple high-pass like FIR (derivative-like)
                                 // This will also need normalization and careful gain adjustment.
    {0.5, -0.5, 0.0, 0.0, 0.0},  // Simple first-order difference (scaled) - crude high-pass.
    2
};

