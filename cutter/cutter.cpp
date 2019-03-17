#include <cstdlib>
#include <cstdio>

#include <string>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <vector>
#include <thread>
#include <atomic>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

using namespace std::string_literals;

/*
 * Task is dsv file with the following fields:
 *   id       uint64
 *   gifname  string
 *   start    uint64, msec
 *   end      uint64, msec
 * The field separator is any number of spaces.
 * The line separator is a newline (\n).
 * No escaping supported.
 */

// Should only be used to print immediately
const char *av_error(int errnum)
{
    constexpr size_t bufsize = 4096;
    thread_local char buff[bufsize];
    av_strerror(errnum, buff, bufsize);
    return buff;
}

struct Args
{
    std::string cmd {"cutter"};
    std::string source;
    std::string filter;
    std::string task;
    std::string report;

    Args(int argc, char *argv[])
    {
        int argnum = 0;
        cmd = argv[argnum++];
        if(argc != 5)
            usage();
        source = argv[argnum++];
        filter = argv[argnum++];
        task   = argv[argnum++];
        report = argv[argnum++];
    }

    void usage()
    {
        fprintf(stderr, "Usage: %s <source> <filter> <task> <report>\n",
                cmd.c_str());
        exit(1);
    }
};

size_t get_video_frame_size(AVFrame *avframe)
{
    size_t size = 0;
    for(int i = 0; i < AV_NUM_DATA_POINTERS; ++i)
        size += avframe->linesize[i];
    if(avframe->height)
        size *= avframe->height;
    if(!size)
        size = 1000;
    return size;
}
class Buffer
{
    public:
        ~Buffer()
        {
            for(auto f: q)
                if(f)
                    av_frame_free(&f);
        }
        void set_time_base(double new_time_base)
        {
            std::lock_guard<std::mutex> lock(bufmutex);
            time_base = new_time_base;
        }
        void push(AVFrame *avframe)
        {
            std::unique_lock<std::mutex> lock{bufmutex};
            if(!avframe)
            {
                q.push_back(avframe);
                update.notify_all();
                return;
            }
            size_t size = get_video_frame_size(avframe);
            while(current_size + size > max_size && !q.empty() && cont_flag)
                update.wait(lock);
            if(cont_flag)
            {
                q.push_back(avframe);
                current_size += size;
            }
            else
                av_frame_free(&avframe);
            update.notify_all();
        }
        void get_sequence(double start, double end, std::vector<AVFrame *> &sequence)
        {
            std::unique_lock<std::mutex> lock{bufmutex};
            while(true)
            {
                while(q.empty())
                    update.wait(lock);
                if(q.back() == nullptr)
                    break;
                if(q.front()->pts*time_base < start)
                {
                    current_size -= get_video_frame_size(q.front());
                    av_frame_free(&q.front());
                    q.pop_front();
                    update.notify_all();
                    continue;
                }
                break;
            }
            while(q.back() != nullptr && q.back()->pts*time_base < end)
                update.wait(lock);
            for(auto f: q)
            {
                if(f == nullptr)
                    break;
                if(f->pts*time_base < start)
                    continue;
                if(f->pts*time_base > end)
                    break;
                sequence.push_back(f);
            }
        }
        void signal_finish()
        {
            std::unique_lock<std::mutex> lock(bufmutex);
            cont_flag = false;
            update.notify_all();
        }
    private:
        static constexpr size_t max_size {4*1000UL*1000*1000}; // 4GB

        std::mutex bufmutex;
        double time_base {1.};
        std::condition_variable update;
        std::deque<AVFrame *> q;
        size_t current_size {0};
        bool cont_flag {true};
};
class Decoder
{
    public:
        Decoder(const std::string &source, const std::string &filter, Buffer &buffer);
        void deinit();
        ~Decoder();
        void decode();
        void signal_finish();
    private:
        Buffer &buffer;
        AVFormatContext *fmt_context {nullptr};
        int stream_idx {-1};
        AVCodecContext *decoder_context {nullptr};
        AVFilterGraph *filter_graph {nullptr};
        AVFilterContext *fsink_context {nullptr};
        AVFilterContext *fsrc_context {nullptr};
        AVFrame *frame_decoder {nullptr};
        AVFrame *frame_filter {nullptr};
        std::atomic_flag cont_flag;

        void decode_packet(AVPacket *avpacket);
        void filter_frame(AVFrame *avframe);
};

Decoder::Decoder(const std::string &source, const std::string &filter, Buffer &buffer)
    : buffer(buffer)
{
    cont_flag.test_and_set();
    try
    {
        if(avformat_open_input(&fmt_context, source.c_str(), nullptr, nullptr) < 0)
            throw "Can't open input"s;
        if(avformat_find_stream_info(fmt_context, nullptr) < 0)
            throw "Can't find stream info"s;
        stream_idx = av_find_best_stream(fmt_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if(stream_idx < 0)
            throw "Can't find video stream"s;
        AVStream *stream = fmt_context->streams[stream_idx];
        AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if(!decoder)
            throw "Can't find codec for video stream"s;
        decoder_context = avcodec_alloc_context3(decoder);
        if(!decoder_context)
            throw "Can't allocate decoder context"s;
        if(avcodec_parameters_to_context(decoder_context, stream->codecpar) < 0)
            throw "Can't copy codec parameters"s;
        AVDictionary *opts = nullptr;
        if(av_dict_set(&opts, "threads", "auto", 0) < 0)
            throw "Can't set decoder options"s;
        if(avcodec_open2(decoder_context, decoder, &opts) < 0)
            throw "Can't open codec"s;
        std::string filter_args;
        AVRational time_base = stream->time_base;
        filter_args += "video_size=" + std::to_string(decoder_context->width) +
                    "x" + std::to_string(decoder_context->height) +
                ":pix_fmt=" + std::to_string(decoder_context->pix_fmt) +
                ":time_base=" + std::to_string(time_base.num) +
                    "/" + std::to_string(time_base.den) +
                ":pixel_aspect=" + std::to_string(decoder_context->sample_aspect_ratio.num) +
                    "/" + std::to_string(decoder_context->sample_aspect_ratio.den);
        filter_graph = avfilter_graph_alloc();
        if(!filter_graph)
            throw "Can't allocate filter graph"s;
        if(avfilter_graph_parse_ptr(filter_graph, filter.c_str(), nullptr, nullptr, nullptr) < 0)
            throw "Can't parse filter";
        AVFilterContext *out = filter_graph->filters[filter_graph->nb_filters-1];
        if(avfilter_graph_create_filter(&fsink_context, avfilter_get_by_name("buffersink"), "out",
                nullptr, nullptr, filter_graph) < 0)
            throw "Can't create buffer sink"s;
        if(avfilter_link(out, 0, fsink_context, 0) < 0)
            throw "Can't link buffer sink"s;
        AVFilterContext *in = filter_graph->filters[0];
        if(avfilter_graph_create_filter(&fsrc_context, avfilter_get_by_name("buffer"), "in",
                filter_args.c_str(), nullptr, filter_graph) < 0)
            throw "Can't create buffer src"s;
        if(avfilter_link(fsrc_context, 0, in, 0) < 0)
            throw "Can't link buffer src"s;
        if(avfilter_graph_config(filter_graph, nullptr) < 0)
            throw "Error verifying filter config"s;
        buffer.set_time_base(double(time_base.num)/time_base.den);
    }
    catch(...)
    {
        deinit();
        throw;
    }
}

void Decoder::deinit() {
    if (fmt_context) {
        avformat_free_context(fmt_context);
        fmt_context = nullptr;
    }
    if (decoder_context) {
        avcodec_free_context(&decoder_context);
        decoder_context = nullptr;
    }
    if (filter_graph) {
        avfilter_graph_free(&filter_graph);
        filter_graph = nullptr;
    }
    if (frame_decoder) {
        av_frame_free(&frame_decoder);
        frame_decoder = nullptr;
    }
    if (frame_filter)
    {
        av_frame_free(&frame_filter);
        frame_filter = nullptr;
    }
}

Decoder::~Decoder() {
    deinit();
}

void Decoder::decode_packet(AVPacket *avpacket) {
    int ret = avcodec_send_packet(decoder_context, avpacket);
    if(ret != 0)
        return;
    while(true)
    {
        if(!frame_decoder)
        {
            frame_decoder = av_frame_alloc();
            if(!frame_decoder)
                throw "Can't alloc avframe"s;
        }
        ret = avcodec_receive_frame(decoder_context, frame_decoder);
        if(ret == AVERROR(EAGAIN))
            break;
        if(ret == AVERROR_EOF)
            break;
        if(ret < 0)
            continue;
        filter_frame(frame_decoder);
        frame_decoder = nullptr;
    }
    if(!avpacket)
        filter_frame(nullptr);
}

void Decoder::decode() {
    AVPacket avpacket {0};
    while(cont_flag.test_and_set() && av_read_frame(fmt_context, &avpacket) >= 0)
    {
        if(avpacket.stream_index == stream_idx)
            decode_packet(&avpacket);
        av_packet_unref(&avpacket);
    }
    decode_packet(nullptr);
    buffer.push(nullptr);
}

void Decoder::filter_frame(AVFrame *avframe)
{
    if(av_buffersrc_add_frame_flags(fsrc_context, avframe, 0) < 0)
        return;
    while(true)
    {
        if(!frame_filter)
        {
            frame_filter = av_frame_alloc();
            if(!frame_filter)
                throw "Can't allocate frame"s;
        }
        int ret = av_buffersink_get_frame_flags(fsink_context, frame_filter, 0);
        if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if(ret != 0)
            continue;
        buffer.push(frame_filter);
        frame_filter = nullptr;
    }
}

void Decoder::signal_finish()
{
    cont_flag.clear();
    buffer.signal_finish();
}

class Encoder
{
    public:
        Encoder(const std::string &dest, AVFrame *sample)
        {
            try
            {
                avformat_alloc_output_context2(&format_ctx, nullptr, nullptr, dest.c_str());
                if(!format_ctx)
                    throw "Can't alloc output format context"s;
                AVCodec *encoder = avcodec_find_encoder_by_name("libx264");
                if(!encoder)
                    throw "Can't find encoder"s;
                stream = avformat_new_stream(format_ctx, encoder);
                if(!stream)
                    throw "Can't create output stream"s;
                encoder_ctx = avcodec_alloc_context3(encoder);
                if(!encoder_ctx)
                    throw "Can't alloc encoder context"s;
                encoder_ctx->width = sample->width;
                encoder_ctx->height = sample->height;
                encoder_ctx->time_base = {1, 25};
                encoder_ctx->pix_fmt = (AVPixelFormat)sample->format;
                AVDictionary *opts = nullptr;
                if(av_dict_set(&opts, "threads", "auto", 0) < 0 ||
                        av_dict_set(&opts, "preset", "slow", 0) < 0)
                    throw "Can't set encoder options"s;
                if(avcodec_open2(encoder_ctx, encoder, &opts) < 0)
                    throw "Can't open encoder"s;
                if(avcodec_parameters_from_context(stream->codecpar, encoder_ctx) < 0)
                    throw "Can't copy codec parameters"s;
                int ret = 0;
                if((ret = avio_open(&format_ctx->pb, dest.c_str(), AVIO_FLAG_WRITE)) < 0)
                    throw "Can't open avio context: "s + av_error(ret);
                if(avformat_write_header(format_ctx, nullptr) < 0)
                    throw "Can't write header"s;
            }
            catch(...)
            {
                deinit();
                throw;
            }
        }
        void deinit()
        {
            if(format_ctx)
            {
                if(format_ctx->pb)
                    avio_closep(&format_ctx->pb);
                avformat_free_context(format_ctx);
                format_ctx = nullptr;
            }
            if(encoder_ctx)
            {
                avcodec_free_context(&encoder_ctx);
                encoder_ctx = nullptr;
            }
        }
        ~Encoder()
        {
            deinit();
        }
        void encode(AVFrame *frame)
        {
            AVFrame *cloned = nullptr;
            if(frame)
            {
                cloned = av_frame_clone(frame);
                if(!cloned)
                    throw "Can't clone frame"s;
                cloned->pts = enc_pts;
                cloned->pict_type = AV_PICTURE_TYPE_NONE;
            }
            ++enc_pts;
            int ret = avcodec_send_frame(encoder_ctx, cloned);
            av_frame_free(&cloned);
            if(ret < 0)
                return;
            while(true)
            {
                AVPacket avpacket {0};
                ret = avcodec_receive_packet(encoder_ctx, &avpacket);
                if(ret == AVERROR(EAGAIN))
                    break;
                if(ret == AVERROR_EOF)
                {
                    if(av_write_frame(format_ctx, nullptr) < 0)
                        throw "Can't flush muxer"s;
                    if(av_write_trailer(format_ctx) < 0)
                        throw "Can't write trailer"s;
                    break;
                }
                av_packet_rescale_ts(&avpacket, encoder_ctx->time_base,
                        stream->time_base);
                if(av_write_frame(format_ctx, &avpacket) < 0)
                    throw "Can't write frame"s;
                av_packet_unref(&avpacket);
            }
        }
    private:
        AVFormatContext *format_ctx {nullptr};
        AVCodecContext *encoder_ctx {nullptr};
        AVStream *stream {nullptr};
        int enc_pts {0};
};

void encode(const std::string &dest, Buffer &buffer, double start, double end)
{
    fprintf(stderr, "Encoding %s: %lf-%lf\n", dest.c_str(), start, end);
    std::vector<AVFrame *> frames;
    buffer.get_sequence(start, end, frames);
    if(frames.empty())
        throw "No frames in this interval found"s;
    Encoder encoder(dest, frames.at(0));
    for (auto f: frames)
        encoder.encode(f);
    encoder.encode(nullptr);
}

std::vector<const char*> split(char *line, size_t len)
{
    std::vector<const char *> ret;
    while(len > 0 && *line == ' ')
    {
        ++line;
        --len;
    }
    bool started = true;
    if(len > 0)
        ret.push_back(line);
    for(int i = 0; i < len; ++i)
    {
        char &c = line[i];
        if(c == ' ')
        {
            if(started)
            {
                c = '\0';
                started = false;
            }
            continue;
        }
        if(!started)
        {
            ret.push_back(line + i);
            started = true;
        }
    }
    return ret;
}

class Worker
{
    public:
        Worker(Buffer &buffer, const std::string &taskfname, const std::string &reportfname)
            : buffer(buffer)
            , task_f(fopen(taskfname.c_str(), "rt"))
            , report_f(fopen(reportfname.c_str(), "wt"))
        {
            try
            {
                if (!task_f)
                    throw "Can't open task file"s;
                if (!report_f)
                    throw "Can't open report file"s;
            }
            catch(...)
            {
                deinit();
                throw;
            }
        }
        ~Worker()
        {
            deinit();
        }
        void run()
        {
            ssize_t read = 0;
            size_t len {0};
            while((read = getline(&line, &len, task_f)) >= 0)
            {
                if(read == 0)
                    continue;
                auto splitted = split(line, (size_t)read);
                if(splitted.size() != 4)
                    throw "Bad task format"s;
                uint64_t id = std::strtoul(splitted[0], nullptr, 10);
                std::string gifname = splitted[1];
                uint64_t start = std::strtoul(splitted[2], nullptr, 10);
                uint64_t end   = std::strtoul(splitted[3], nullptr, 10);
                encode(gifname, buffer, start/1e3, end/1e3);
                fprintf(report_f, "%lu %s %lu %lu\n", id, gifname.c_str(), start, end);
                fflush(report_f);
            }
            if(!feof(task_f))
                throw "Error reading file"s;
        }
    private:
        Buffer &buffer;
        FILE *task_f {nullptr};
        FILE *report_f {nullptr};
        char *line {nullptr};

        void deinit()
        {
            if(task_f)
            {
                fclose(task_f);
                task_f = nullptr;
            }
            if(report_f)
            {
                fclose(report_f);
                report_f = nullptr;
            }
            if(line)
            {
                free(line);
                line = nullptr;
            }
        }
};


int main(int argc, char *argv[])
{
    Args args(argc, argv);
    Buffer buffer;
    std::unique_ptr<Decoder> decoder;
    try
    {
        decoder = std::make_unique<Decoder>(args.source, args.filter, buffer);
    }
    catch(std::string &ex)
    {
        fprintf(stderr, "Decoder initialization error: %s\n", ex.c_str());
        exit(1);
    }
    auto decoder_thread = std::thread{[&]{
        try
        {
            decoder->decode();
            fprintf(stderr, "Decoding finished\n");
        }
        catch(std::string &ex)
        {
            fprintf(stderr, "Decoder error: %s\n", ex.c_str());
            exit(1);
        }
    }};
    try
    {
        Worker worker(buffer, args.task, args.report);
        worker.run();
    }
    catch(std::string &ex)
    {
        fprintf(stderr, "Error: %s\n", ex.c_str());
        exit(1);
    }
    fprintf(stderr, "Encoding finished\n");
    decoder->signal_finish();
    decoder_thread.join();
    return 0;
}
