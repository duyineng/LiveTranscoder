# 点播 HLS 实现方案

本文只描述怎么改代码。这一轮实现点播 HLS：文件进、目录出，读多快处理多快。`transcode` 命令的转封装和单文件转码保持现在的行为。

水印、字幕、直播、RTMP、DASH、数据库不在这一轮里。文末只说明以后接在哪里，点播路径里不要预留这些代码。

## 1. 完成标准

单码率：

- 分片目标时长 2 秒。
- 每个分片从 IDR 开始。
- VLC 打开该档的 `index.m3u8`，能连续播放，拖动后从新分片继续播。

三档：

- 1080p、720p、480p。视频只解码一次，缩放到三档后分别编码。
- 音频只编一次，同一批 AAC 包写入每一档。
- 程序自己写 `master.m3u8`，每一档都有 `BANDWIDTH` 和 `RESOLUTION`。

默认三档：

| 目录名 | 分辨率 | 视频码率 |
| --- | --- | --- |
| `1080p` | 1920x1080 | 5 Mbps |
| `720p` | 1280x720 | 2.5 Mbps |
| `480p` | 854x480 | 1 Mbps |

音频共用 128 kbps、48000 Hz。854 是 16:9 下取偶数后的宽度。H.264 的 YUV420P 要求宽和高都是偶数，奇数直接失败。

输出目录：

```text
out_dir/
  master.m3u8
  1080p/index.m3u8
  1080p/seg_000.ts
  720p/index.m3u8
  720p/seg_000.ts
  480p/index.m3u8
  480p/seg_000.ts
```

只出一档时也是这个形状，只是子目录只有一个。单码率和三档走同一条代码，差别只是 `variants` 里有几个元素。

## 2. 现有程序里要守住的行为

`src/Transcoder.cpp` 里的 `Transcoder::transcode` 现在有两条路径。

不传宽高时不解码。从 `avformat_open_input` 读到的压缩包按输出后缀装进新容器，清掉 `codec_tag`，再按输出流的 `time_base` 重算时间戳。这段原样挪到 `Remux.cpp`。

传了宽高时：

- 用 `av_find_best_stream` 找一路视频和一路音频。现在没有音频会失败。第 2 步改成可以没有音频。
- 视频缩放到目标宽高的 YUV420P，`sws_scale` 直接拉伸，不补黑边。HLS 继续这样缩放。
- 视频编码器优先 `libx264`，否则 `h264_mf`，再否则任意接受 YUV420P 的 H.264 编码器。`preset=veryfast`，`tune=zerolatency`，`gop_size` 来自 `TranscodeOptions`（默认 60），`max_b_frames=2`。
- 音频重采样后编 AAC，用 `AVAudioFifo` 凑满一帧，尾帧补静音，PTS 按采样点累加。
- 容器由输出文件后缀决定。`AVFMT_NOFILE` 时不调用 `avio_open`。`AVFMT_GLOBALHEADER` 时给编码器加 `AV_CODEC_FLAG_GLOBAL_HEADER`。

抽出函数之后，下面两条命令仍然成功，分辨率、编码器和能否播放与现在一致。不要求输出文件逐字节相同。

```text
LiveTranscoder transcode test.mp4 output.flv
LiveTranscoder transcode test.mp4 output.flv 1280 720
```

文件转码继续使用 `zerolatency`。点播 HLS 不使用它。两套参数都从 `EncoderSetup.cpp` 打开编码器，由调用方传入配置，避免各写一套打开逻辑，也避免把文件转码改成点播参数。

## 3. 点播数据流

```text
输入文件
  -> 解封装
      -> 视频解码一次 -> 每档 sws_scale -> 每档 libx264 -> 该档 HLS
      -> 音频解码一次 -> 重采样 -> AAC 只编一次 -> 克隆包写入每一档
  -> 全部 trailer 写完后，再写 master.m3u8
```

一档内部的对象放在 `VariantOutput` 里：缩放器、视频编码器、HLS 格式上下文、视频流、音频流。音频编码器、重采样器和 FIFO 整个任务只有一份，不放进 `VariantOutput`。

HLS 封装器默认把分片切在关键帧上。`hls_time` 是目标时长，它会等到下一个关键帧再切。因此关键帧间隔必须等于分片时长。不要加 `split_by_time`，那个标志会按时间硬切，分片可能不是从 IDR 开始。

点播使用闭合 GOP：`open-gop=0`，`scenecut=0`，`keyint` 和 `min-keyint` 都等于「帧率 × 分片秒数」。这样场景切换不会提前插入 IDR，播放器也可以单独解码一个分片。点播允许 B 帧，`max_b_frames=2`。B 帧留在 GOP 内部，分片的第一个包仍然是 IDR。

帧率沿用现在的算法：取输入视频的 `avg_frame_rate`，无效时用 30/1。

```cpp
const int fps = std::max(1, static_cast<int>(std::lround(av_q2d(frameRate))));
const int keyint = fps * segmentDurationSeconds;
```

29.97 fps、分片 2 秒时，`keyint` 是 60。24 fps 时是 48。`gop_size` 设成同一个 `keyint`，不再写死 60。

## 4. 文件布局

按输出形态拆。一个 FFmpeg 调用不单独占一个文件。

```text
include/
  FFmpegSupport.h       RAII、错误字符串、编码器查找
  TranscodeOptions.h    现有转码参数，以及 HlsVariant
  Transcoder.h          门面
  Remux.h
  FileTranscode.h
  EncoderSetup.h
  HlsVod.h
src/
  main.cpp
  Transcoder.cpp        打开输入后分发
  Remux.cpp
  FileTranscode.cpp
  EncoderSetup.cpp
  HlsVod.cpp
CMakeLists.txt          把新的 cpp 加进同一个可执行文件
```

`FFmpegSupport.h` 是头文件。里面的函数标成 `inline`，避免多个 cpp 包含后重复定义。不新增库。HLS 封装器在 libavformat 里。

`CMakeLists.txt` 的源文件列表改为：

```cmake
add_executable(LiveTranscoder
    src/main.cpp
    src/Transcoder.cpp
    src/Remux.cpp
    src/FileTranscode.cpp
    src/EncoderSetup.cpp
    src/HlsVod.cpp
)
```

`/utf-8` 和现有的 FFmpeg 查找保持不变。

### 4.1 FFmpegSupport.h

从 `src/Transcoder.cpp` 匿名命名空间移出这些内容：

- `errorString`
- `InputFormatContextDeleter`、`OutputFormatContextDeleter`、`CodecContextDeleter`、`FrameDeleter`、`PacketDeleter`、`SwsContextDeleter`、`SwrContextDeleter`、`AudioFifoDeleter`
- 对应的 `unique_ptr` 别名
- `getCodecConfig`、`chooseSampleRate`、`chooseSampleFormat`、`codecAcceptsYuv420p`、`findSoftwareH264Encoder`

`OutputFormatContextDeleter` 继续在 `pb` 非空时 `avio_closep`，然后 `avformat_free_context`。HLS 封装器自己管 IO，正常情况下不会走到重复关闭。

各 cpp 直接调用 `errorString`。`Transcoder::makeError` 删除。

### 4.2 参数和门面

`include/TranscodeOptions.h` 放现有的 `TranscodeOptions`，并增加点播用的结构：

```cpp
struct HlsVariant {
    int width = 0;
    int height = 0;
    int videoBitrate = 0;
    std::string name;
};

struct HlsRequest {
    std::vector<HlsVariant> variants;
    int audioBitrate = 128'000;
    int audioSampleRate = 48'000;
    int segmentDurationSeconds = 2;
    bool verbose = true;
};
```

`include/Transcoder.h`：

```cpp
class Transcoder {
public:
    bool transcode(
        const std::string& inputUrl,
        const std::string& outputUrl,
        std::optional<TranscodeOptions> transcodeOptions,
        std::string& error);

    bool transcodeHls(
        const std::string& inputUrl,
        const std::string& outputDir,
        const HlsRequest& request,
        std::string& error);
};
```

`transcode` 仍负责打开输入并 `avformat_find_stream_info`，然后：

```cpp
if (!transcodeOptions) {
    return remux(inputFmtCtx.get(), outputUrl, error);
}
return fileTranscode(inputFmtCtx.get(), outputUrl, *transcodeOptions, error);
```

输入上下文的 `unique_ptr` 留在 `transcode` 里。`remux` 和 `fileTranscode` 只拿裸指针，不调用 `avformat_close_input`。

`transcodeHls` 自己打开输入。它可以调用同一个 `openInput` 辅助函数，该函数放在 `FFmpegSupport.h` 或 `Transcoder.cpp` 里均可，返回 `InputFormatContextPtr`。

### 4.3 编码器配置

`include/EncoderSetup.h`：

```cpp
struct VideoEncoderConfig {
    int width = 0;
    int height = 0;
    int bitrate = 0;
    AVRational frameRate{30, 1};
    int gopSize = 60;
    int maxBFrames = 2;
    bool zeroLatency = false;
    bool globalHeader = false;
    bool closedGop = false;
};

struct AudioEncoderConfig {
    int bitrate = 128'000;
    int sampleRate = 48'000;
    int channels = 2;
    bool globalHeader = false;
};

bool openH264Encoder(const VideoEncoderConfig& config, CodecContextPtr& encoder, std::string& error);
bool openAacEncoder(
    const AVCodec* audioEncoder,
    const AudioEncoderConfig& config,
    const AVChannelLayout& decoderLayout,
    CodecContextPtr& encoder,
    std::string& error);
```

`openH264Encoder` 内部自己找编码器。

- 文件转码：`zeroLatency=true`，`closedGop=false`，`gopSize` 用 `TranscodeOptions::gopSize`。允许回退到 `h264_mf`。打开前设置 `preset=veryfast` 和 `tune=zerolatency`。
- 点播：`zeroLatency=false`，`closedGop=true`，`gopSize=keyint`。优先 `libx264`，并在 `avcodec_open2` 之前写入下面的 `x264-params`。当前这套 FFmpeg 如果没有 `libx264`，就用 `findSoftwareH264Encoder` 的结果（这台机器是 `h264_mf`），只靠 `gop_size` 对齐关键帧，不写 `x264-params`。不设置 `tune`。有 `libx264` 时在 `avcodec_open2` 之前写入：

```cpp
av_opt_set(encoder->priv_data, "preset", "veryfast", 0);
const std::string x264Params =
    "keyint=" + std::to_string(config.gopSize) +
    ":min-keyint=" + std::to_string(config.gopSize) +
    ":scenecut=0:open-gop=0";
av_opt_set(encoder->priv_data, "x264-params", x264Params.c_str(), 0);
```

两种模式都设置 `width`、`height`、`pix_fmt=YUV420P`、`framerate`、`time_base=av_inv_q(frameRate)`、`bit_rate`、`gop_size`、`max_b_frames`。`globalHeader` 为真时加上 `AV_CODEC_FLAG_GLOBAL_HEADER`。是否加由调用方看输出格式的 `AVFMT_GLOBALHEADER`，不要在 HLS 路径里强制加上。HLS 默认分片是 MPEG-TS，SPS/PPS 在码流里重复，一般没有这个标志。

`openAacEncoder` 搬出现在的采样率选择、FLTP 优先、声道布局和 `time_base{1, sample_rate}`。文件转码和 HLS 共用。

### 4.4 HlsVod.cpp 里的一档输出

```cpp
struct VariantOutput {
    HlsVariant spec;
    SwsContextPtr scaler;
    CodecContextPtr videoEncoder;
    OutputFormatContextPtr format;
    AVStream* videoStream = nullptr;
    AVStream* audioStream = nullptr;
};
```

声明放在 `HlsVod.cpp` 的匿名命名空间，不放进公共头。

每个变体：

1. `std::filesystem::create_directories(outputDir / name)`。目录已存在不算错误。
2. 播放列表路径是 `outputDir/name/index.m3u8`。
3. `avformat_alloc_output_context2(&raw, nullptr, "hls", playlistPath)`。格式名写 `"hls"`，不靠后缀猜测。
4. 在 `priv_data` 上设置：
   - `hls_time` = 分片秒数减 0.5。封装器会在超过这个时间后的下一个关键帧切开。关键帧若落在整 2 秒前一点点，`hls_time` 仍写成 2 会拖到下一个 GOP，分片变成约 4 秒。
   - `hls_playlist_type` = `vod`
   - `hls_list_size` = `0`
   - `hls_flags` = `independent_segments`
   - `hls_segment_filename` = 该档目录下的 `seg_%03d.ts`
5. 路径传给 FFmpeg 时用正斜杠。写完后打开 `index.m3u8`，分片行应是 `seg_000.ts` 这种相对名字。若是绝对路径，把 `hls_segment_filename` 改成和播放列表同一目录下的文件名模板。
6. 为该档 `sws_getContext`，源是解码器宽高和像素格式，目标是该档宽高和 `AV_PIX_FMT_YUV420P`，算法 `SWS_BILINEAR`。
7. `avformat_new_stream` 建视频流。有音频时再为这一档建音频流，时间基等于那个唯一的 AAC 编码器。`avcodec_parameters_from_context` 把参数抄进去。
8. 遵守 `AVFMT_NOFILE`：HLS 通常自己打开文件，调用方不要再 `avio_open`。然后 `avformat_write_header`。

`master.m3u8` 在所有档的 `av_write_trailer` 成功之后再写。中途失败不写 master。用 `std::ofstream` 写 UTF-8 文本。变体按视频码率从高到低排。

无音频时：

```text
#EXTM3U
#EXT-X-VERSION:3
#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080
1080p/index.m3u8
```

有音频时 `BANDWIDTH` 是该档视频码率加音频码率。5 Mbps 视频加 128 kbps 音频就是 `5128000`。

重跑不会清空目录。播放列表只引用这一轮写出的分片；上一轮多出来的 `seg_010.ts` 不会进列表，但会留在磁盘上。重跑前删掉输出目录即可。

## 5. 实现步骤

每一步做完并验证，再做下一步。

### 步骤 1：抽出现有路径

改这些文件：

- 新增 `include/FFmpegSupport.h`、`include/TranscodeOptions.h`、`include/Remux.h`、`include/FileTranscode.h`、`include/EncoderSetup.h`
- `include/Transcoder.h` 只保留门面，`TranscodeOptions` 从这里移走
- 新增 `src/Remux.cpp`、`src/FileTranscode.cpp`、`src/EncoderSetup.cpp`
- `src/Transcoder.cpp` 只保留打开输入和分发
- `CMakeLists.txt` 加上三个新 cpp

`Remux.cpp` 承接现在 `if (!transcodeOptions)` 整段，从创建输出上下文到 `av_write_trailer`。

`FileTranscode.cpp` 承接现在传了宽高的整段，包括解码、缩放、音频 FIFO、flush。这一步先原样搬，行为不变，包括「没有音频就失败」。

编码器的打开改成调用 `openH264Encoder` / `openAacEncoder`，文件转码传入的配置必须和现在的字段一致：`veryfast`、`zerolatency`、`gopSize`、`max_b_frames=2`、原码率。

做完的标准：第 2 节的两条 `transcode` 命令仍然成功。带宽高的输出能播，分辨率是 1280x720。不带宽高的输出能播，编码格式与输入相同。

### 步骤 2：允许没有音频

改 `src/FileTranscode.cpp`。`av_find_best_stream` 找不到音频时不要失败。`audioIndex < 0` 时：

- 不打开音频解码器和 AAC 编码器
- 不建音频输出流，不分配 FIFO，不建重采样器
- 读包循环里只处理视频包
- flush 时只 flush 视频解码器和视频编码器

输入仍然必须有视频。有音频时，FIFO、补静音和按采样点累加 PTS 的逻辑不变。

做完的标准：把一段有声 MP4 去掉音频后再执行 `transcode input_video_only.mp4 out.flv 1280 720`，命令成功，输出只有视频。原来的有声样本仍然有声。

可以用 FFmpeg 命令行准备样本：`ffmpeg -i test.mp4 -an input_video_only.mp4`。

### 步骤 3：单码率 HLS

新增 `include/HlsVod.h` 和 `src/HlsVod.cpp`。`Transcoder::transcodeHls` 转给 `hlsVod(...)`。这一步 `variants` 里只有一档，但循环按多档来写。

`src/main.cpp` 增加子命令，先够用来测一档：

```text
LiveTranscoder hls input.mp4 out_dir 1280 720
```

这一档的视频码率用现有默认值 2.5 Mbps，目录名 `720p`。宽和高必须成对出现。`transcode` 的参数解析不动。

`HlsVod.cpp` 的主流程：

1. 打开输入，找到视频流。音频流可以没有。
2. 打开视频解码器。有音频时打开音频解码器、唯一的 AAC 编码器、重采样器和 FIFO。
3. 计算 `keyint`。为这一档创建 `VariantOutput`：HLS 上下文、libx264、缩放器、写 header。
4. `av_read_frame`。视频包送进视频解码器；解出一帧后，对该档做第 5 点。音频包送进音频解码器；解出一帧后做第 6 点。其他流丢掉。
5. 视频帧：`sws_scale` 到该档的编码器帧，PTS 从输入流 `time_base` 换到编码器 `time_base`。没有 PTS 时用已编码帧数，这和现在的文件转码相同。`avcodec_send_frame`，再把 `avcodec_receive_packet` 得到的包换到该档视频流的 `time_base`，`av_interleaved_write_frame`。
6. 音频帧：重采样进那个唯一的 FIFO，凑满 AAC 的 `frame_size` 就编码。每个 AAC 包用 `av_packet_clone` 后写入每一档的音频流，写之前改 `stream_index` 并重算时间戳。`av_interleaved_write_frame` 会把传入的包 unref 掉，所以不能把同一个包指针写给两档。
7. 读到 EOF 后，向视频解码器 `send_packet(nullptr)`，把剩余帧编码进每一档。再向每一档视频编码器 `send_frame(nullptr)` 并取走剩余包。有音频时同样 flush 解码器、重采样器、FIFO 和 AAC 编码器，最后一帧不足 `frame_size` 时补静音。
8. 每一档 `av_write_trailer`。成功后再写 `master.m3u8`。

做完先不要加默认三档。用步骤 4 检查这一档。

### 步骤 4：检查单码率

看第一个分片的帧类型：

```text
ffprobe -select_streams v -show_frames -show_entries frame=pict_type,best_effort_timestamp_time -of csv out_dir/720p/seg_000.ts
```

第一行的 `pict_type` 必须是 `I`。再看第二个分片，第一帧也必须是 `I`。

看分片时长：

```text
ffprobe -show_entries format=duration -of default=nw=1:nk=1 out_dir/720p/seg_000.ts
```

时长大约 2 秒。最后一个分片可以更短。

打开 `out_dir/720p/index.m3u8`，确认里面有：

- `#EXT-X-PLAYLIST-TYPE:VOD`
- `#EXT-X-INDEPENDENT-SEGMENTS`
- `#EXT-X-ENDLIST`
- 分片文件名是 `seg_000.ts` 这种相对路径

用 VLC 打开这个 `index.m3u8`，连续播放，并拖到后半段。拖动后画面应马上恢复，不应长时间花屏。

再用 VLC 打开 `out_dir/master.m3u8`。这一步只有一档，它应播同一路画面。

有声样本要能听到声音。步骤 2 的无音频样本要能出只有视频的 HLS，`master.m3u8` 的 `BANDWIDTH` 不含 128000。

这一步不过，就不做三档。常见原因是 `keyint` 没和 2 秒对齐，或者打开了 `split_by_time`。

### 步骤 5：三档

仍只改 `src/main.cpp` 和 `src/HlsVod.cpp`。不新增文件。

不带宽高时使用第 1 节的三档：

```text
LiveTranscoder hls input.mp4 out_dir
```

带宽高时仍只出一档，方便回到步骤 4：

```text
LiveTranscoder hls input.mp4 out_dir 1280 720
```

解码循环里，一帧视频对 `variants` 逐个 `sws_scale` 和编码。音频编码仍在循环外只做一次，再把克隆包写进每一档。某一档失败就整次失败。

做完的标准：

- 三个子目录都有 `index.m3u8`，各自的 `seg_000.ts` 第一帧都是 I 帧。
- `master.m3u8` 有三条 `EXT-X-STREAM-INF`，码率从高到低，分辨率分别是 1920x1080、1280x720、854x480。
- 有音频时三条 `BANDWIDTH` 分别是 `5128000`、`2628000`、`1128000`。
- VLC 打开 `master.m3u8` 能播。播放中切换档不是这一步的必测项；三份媒体列表各自能播即可。

### 步骤 6：命令汇总

实现完后用法是：

```text
LiveTranscoder transcode input.mp4 output.flv
LiveTranscoder transcode input.mp4 output.flv 1280 720
LiveTranscoder hls input.mp4 out_dir
LiveTranscoder hls input.mp4 out_dir 1280 720
```

第一条转封装。第二条单文件转码，参数仍是现在的 `zerolatency`。第三条点播三档。第四条点播一档。`hls` 后面缺输入或输出目录时，打印用法并返回 1。宽和高只给了一个时，也返回 1。

## 6. 验证清单

| 命令或检查 | 通过标准 |
| --- | --- |
| `transcode` 不带宽高 | 成功，容器变了，编码格式没变 |
| `transcode` 带 1280 720 | 成功，能播，分辨率 1280x720，仍是低延迟那套参数 |
| 无音频输入做 `transcode` | 成功，输出没有音频流 |
| `hls` 带 1280 720 | `720p/seg_000.ts` 首帧是 I 帧，时长约 2 秒 |
| VLC 打开单档 `index.m3u8` | 能播，拖动后很快恢复画面 |
| `hls` 不带宽高 | 三档目录和 `master.m3u8` 齐全 |
| 有声与无声各跑一次 `hls` | 有声三档都能听到声音；无声不写音频流 |

文件转码的 `zerolatency` 可以用 `ffprobe -show_frames` 看 `pict_type`。点播分片里允许出现 B 帧，只要分片第一个是 I 帧。文件转码若仍然没有 B 帧，说明 `zerolatency` 还在，这条路径没有被点播参数改掉。

## 7. 以后再加的内容

这一轮不要写对应代码。

水印和字幕加在点播循环里、`sws_scale` 之后、`avcodec_send_frame` 之前。三档可以共用一次滤镜结果再缩放，也可以每档各画一次。滤镜用 `libavfilter`，不改 HLS 封装参数。

直播仍从 `HlsVod` 分出另一条入口。要改的是按帧 PTS 等待、播放列表改成滑动窗口、删掉旧分片，以及把输出推到 RTMP。点播的 `hls_playlist_type=vod` 和「尽快读完整个文件」不要提前改成直播语义。
