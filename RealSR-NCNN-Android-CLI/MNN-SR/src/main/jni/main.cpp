
#include <stdio.h>

#include <queue>
#include <vector>
#include <clocale>
#include <thread>
#include <filesystem>
#include <regex>

//#undef min
//#undef max

#include "MNN/Tensor.hpp"
#include "MNN/MNNForwardType.h"
#include "MNN/Interpreter.hpp"

#define _DEMO_PATH  false
#define _VERBOSE_LOG  true

#if _WIN32
// image decoder and encoder with wic
//#include "wic_image.h"
#else // _WIN32
// image decoder and encoder with stb
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_STDIO


#define STB_IMAGE_WRITE_IMPLEMENTATION


#endif // _WIN32

#include <chrono>

using namespace std::chrono;
namespace fs = std::filesystem;

#if _WIN32
#include <wchar.h>
static wchar_t* optarg = NULL;
static int optind = 1;
static wchar_t getopt(int argc, wchar_t* const argv[], const wchar_t* optstring)
{
    if (optind >= argc || argv[optind][0] != L'-')
        return -1;

    wchar_t opt = argv[optind][1];
    const wchar_t* p = wcschr(optstring, opt);
    if (p == NULL)
        return L'?';

    optarg = NULL;

    if (p[1] == L':')
    {
        optind++;
        if (optind >= argc)
            return L'?';

        optarg = argv[optind];
    }

    optind++;

    return opt;
}

static std::vector<int> parse_optarg_int_array(const wchar_t* optarg)
{
    std::vector<int> array;
    array.push_back(_wtoi(optarg));

    const wchar_t* p = wcschr(optarg, L',');
    while (p)
    {
        p++;
        array.push_back(_wtoi(p));
        p = wcschr(p, L',');
    }

    return array;
}
#else // _WIN32

#include <unistd.h> // getopt()


#include <cstring> 

static std::vector<int> parse_optarg_int_array(const char *optarg) {
    std::vector<int> array;
    array.push_back(atoi(optarg));

    const char *p = strchr(optarg, ',');
    while (p) {
        p++;
        array.push_back(atoi(p));
        p = strchr(p, ',');
    }

    return array;
}

#endif // _WIN32


#include "mnnsr.h"
#include "filesystem_utils.h"
#include <opencv2/opencv.hpp>
#include <opencv2/core/hal/interface.h>

using namespace cv;

static void print_usage() {
    fprintf(stderr, "Usage: mnnsr -i infile -o outfile [options]...\n\n");
    fprintf(stderr, "  -h                   show this help\n");
    fprintf(stderr, "  -v                   verbose output\n");
    fprintf(stderr, "  -i input-path        input image path (jpg/png/webp) or directory\n");
    fprintf(stderr, "  -o output-path       output image path (jpg/png/webp) or directory\n");
    fprintf(stderr, "  -s scale             upscale ratio (4, default=4)\n");
    fprintf(stderr,
            "  -t tile-size         tile size (>=32/0=auto, default=0) can be 0,0,0 for multi-gpu\n");
    fprintf(stderr, "  -m model-path        realsr model path (default=models-DF2K_JPEG)\n");
    //fprintf(stderr,
    //        "  -g gpu-id            gpu device to use (-1=cpu, default=auto) can be 0,1,2 for multi-gpu\n");
    //fprintf(stderr,
    //        "  -j load:proc:save    thread count for load/proc/save (default=1:2:2) can be 1:2,2,2:2 for multi-gpu\n");
//    fprintf(stderr, "  -x                   enable tta mode\n");
    fprintf(stderr, "  -f format            output image format (jpg/png/webp, default=ext/png)\n");

#ifdef __ANDROID__
    fprintf(stderr, "  -b backend           forward backend type(CPU=0,AUTO=4,OPENCL=3,OPENGL=6,VULKAN=7,NN=5,USER_0=8,USER_1=9,default=3)\n");
#else
    fprintf(stderr, "  -b backend           forward backend type(CPU=0,CUDA=2,OPENCL=3,VULKAN=7,TensorRT=9,AUTO=4,default=3)\n");
#endif // __ANDROID__

    fprintf(stderr,
            "  -c color-type        model & output color space type (RGB=1, BGR=2, YCbCr=5, YUV=6, GRAY=10, GRAY model & YCbCr output=11, GRAY model & YUV output=12, default=1)\n");
    fprintf(stderr,
            "  -d decensor-mode     remove censor mode (Not=-1, Mosaic=0, default=-1)\n");
}

class Task {
public:
    int id;
    int webp;

    int scale;
//    bool check;

    path_t inpath;
    path_t outpath;

    cv::Mat inimage;
    cv::Mat outimage;
    cv::Mat inalpha;
};

class TaskQueue {
public:
    TaskQueue() {
    }

    void put(const Task &v) {
        lock.lock();

        while (tasks.size() >= 8) // FIXME hardcode queue length
        {
            condition.wait(lock);
        }

        tasks.push(v);

        lock.unlock();

        condition.signal();
    }

    void get(Task &v) {
        lock.lock();

        while (tasks.size() == 0) {
            condition.wait(lock);
        }

        v = tasks.front();
        tasks.pop();

        lock.unlock();

        condition.signal();
    }

private:
    ncnn::Mutex lock;
    ncnn::ConditionVariable condition;
    std::queue<Task> tasks;
};

TaskQueue toproc;
TaskQueue tosave;
high_resolution_clock::time_point batch_start = high_resolution_clock::now();

class LoadThreadParams {
public:
    int scale;
    int jobs_load;

    // session data
    std::vector<path_t> input_files;
    std::vector<path_t> output_files;
};

void *load(void *args) {

    const LoadThreadParams *ltp = (const LoadThreadParams *) args;
    const int count = ltp->input_files.size();
    const int scale = ltp->scale;

#pragma omp parallel for schedule(static, 1) num_threads(ltp->jobs_load)
    for (int i = 0; i < count; i++) {
        const path_t &imagepath = ltp->input_files[i];

        if (count == 1) {
#if _WIN32
            fprintf(stderr, "load %ws \n", imagepath.c_str());
#else
            fprintf(stderr, "load %s \n", imagepath.c_str());
#endif
        }
        // 读取图像
        #if _WIN32
            Mat image = imread_unicode(imagepath, IMREAD_UNCHANGED);
        #else
           Mat image = imread(imagepath, IMREAD_UNCHANGED);
        #endif
        if (image.empty()) {
#if _WIN32
            fwprintf(stderr, L"decode image %ls failed\n", imagepath.c_str());
#else // _WIN32

            fprintf(stderr, "decode image %s failed\n", imagepath.c_str());
#endif // _WIN32
            continue;
        }


        Task v;
        v.id = i;
        v.inpath = imagepath;
        v.outpath = ltp->output_files[i];
        v.scale = scale;

        cv::Mat inimage;
        int c = image.channels();

        if (c == 1) {
            // 如果图像有1个通道，转换为3个通道
            cvtColor(image, inimage, COLOR_GRAY2BGR);
            c = 3;
            v.inimage = inimage;
        } else if (image.channels() == 4) {
            // 如果图像有4个通道，分离通道
            std::vector<cv::Mat> channels;
            split(image, channels);
            Mat alphaChannel = channels[3];

            // 判断 alpha 通道是否为单一颜色
            if (countNonZero(alphaChannel != alphaChannel.at<uchar>(0, 0)) == 0) {
                #if _WIN32  
                           fwprintf(stderr, L"ignore alpha channel, %ls\n", imagepath.c_str());  
                #else  
                           fprintf(stderr, "ignore alpha channel, %s\n", imagepath.c_str());  
                #endif
                c = 3;
            } else {
                v.inalpha = alphaChannel;
            }
            merge(channels.data(), 3, inimage);
            v.inimage = inimage;
        } else if (c == 3) {
            v.inimage = image;
        } else {
#if _WIN32  
            fwprintf(stderr, L"[err] channel=%d, %ls\n", image.channels(), imagepath.c_str());
#else  
            fprintf(stderr, "[err] channel=%d, %s\n", image.channels(), imagepath.c_str());
#endif

            continue;
        }

        v.outimage = cv::Mat(v.inimage.rows * scale, v.inimage.cols * scale, CV_8UC3);

        if (ltp->input_files.size() == 1) {
            fprintf(stderr, "scale=%d, w/h/c %d/%d/%d -> %d/%d/%d (%d)\n", scale,
                v.inimage.cols, v.inimage.rows, v.inimage.channels(),
                v.outimage.cols, v.outimage.rows, v.outimage.channels(), c
            );
        }

        path_t ext = get_file_extension(v.outpath);
        if (c == 4 &&
            (ext == PATHSTR("jpg") || ext == PATHSTR("JPG") || ext == PATHSTR("jpeg") ||
             ext == PATHSTR("JPEG"))) {
            path_t output_filename2 = ltp->output_files[i] + PATHSTR(".png");
            v.outpath = output_filename2;
#if _WIN32
			fwprintf(stderr, L"image %ls has alpha channel ! %ls will output %ls\n"
				, imagepath.c_str(), imagepath.c_str(), output_filename2.c_str());
#else // _WIN32
			fprintf(stderr, "image %s has alpha channel ! %s will output %s\n"
				, imagepath.c_str(), imagepath.c_str(), output_filename2.c_str());
#endif // _WIN32
        }

        toproc.put(v);

    }


    return 0;
}

class ProcThreadParams {
public:
    MNNSR *mnnsr;
    int decensor_mode = -1;
};

void *proc(void *args) {
    const ProcThreadParams *ptp = (const ProcThreadParams *) args;
    MNNSR *mnnsr = ptp->mnnsr;
    fprintf(stderr, "proc thread start, decensor_mode=%d\n", ptp->decensor_mode);

    for (;;) {
        Task v;

        toproc.get(v);

        if (v.id == -233)
            break;

        if (ptp->decensor_mode == -1)
            mnnsr->process(v.inimage, v.outimage);
        else
            mnnsr->decensor(v.inimage, v.outimage);

        tosave.put(v);
    }

    return 0;
}

class SaveThreadParams {
public:
    int verbose;
    int input_files_size;
};

 

void *save(void *args) {
    const SaveThreadParams *stp = (const SaveThreadParams *) args;
    const int verbose = stp->verbose;

    for (int saved_count = 1;;saved_count++) {
        Task v;

        tosave.get(v);

        if (v.id == -233)
            break;

        if (v.outimage.empty()) {
#if _WIN32  
            fwprintf(stderr, L"[err] invalid result %ls\n", v.inpath.c_str());
#else  
            fprintf(stderr, "[err] invalid result %s\n", v.inpath.c_str());
#endif
            continue;
        }

        high_resolution_clock::time_point begin = high_resolution_clock::now();

        int success = 0;

        path_t ext = get_file_extension(v.outpath);

        if (ext == PATHSTR("jpg") || ext == PATHSTR("JPG") || ext == PATHSTR("jpeg") ||
            ext == PATHSTR("JPEG")) {
#if _WIN32
            success = (imwrite_unicode(v.outpath, v.outimage, { cv::IMWRITE_JPEG_QUALITY, 90 }));
#else
            success = (cv::imwrite(v.outpath.c_str(), v.outimage, { cv::IMWRITE_JPEG_QUALITY, 90 }));
#endif
        } else {
            if (!v.inalpha.empty()) {
                cv::Mat scaledAlphaChannel;
                cv::resize(v.inalpha, scaledAlphaChannel, cv::Size(), v.scale, v.scale,
                           cv::INTER_LANCZOS4);
                std::vector<cv::Mat> outChannels;
                cv::split(v.outimage, outChannels);
                std::vector<cv::Mat> channelsToMerge;
                channelsToMerge.push_back(outChannels[0]);
                channelsToMerge.push_back(outChannels[1]);
                channelsToMerge.push_back(outChannels[2]);
                channelsToMerge.push_back(scaledAlphaChannel);     // Alpha
                cv::merge(channelsToMerge, v.outimage);
//                fprintf(stderr, "merge alpha channel, %d/%d/%d/%d\n",v.outimage.rows, v.outimage.cols, v.outimage.channels());
            }

#if _WIN32
            success = (imwrite_unicode(v.outpath, v.outimage ));
#else
            success = (cv::imwrite(v.outpath.c_str(), v.outimage));
#endif
        }
        if (success) {
            high_resolution_clock::time_point end = high_resolution_clock::now();
            duration<double> time_span = duration_cast<duration<double>>(end - begin);
            if (stp->input_files_size==1)
                fprintf(stderr, "save result use time: %.3lf\n", time_span.count());
			else {

				duration<double> batch_time_span = duration_cast<duration<double>>(end - batch_start);
#if _WIN32
				fwprintf(stdout, L"[done] %d/%d %ls -> %ls, %hs/%hs\n", saved_count, stp->input_files_size, v.inpath.c_str(), v.outpath.c_str()
					, format_time_s(batch_time_span.count()).c_str(), format_time_s(batch_time_span.count() * (stp->input_files_size - saved_count) / saved_count).c_str()
				);
#else
				fprintf(stdout, "[done] %d/%d %s -> %s, %s/%s\n", saved_count, stp->input_files_size, v.inpath.c_str(), v.outpath.c_str()
                    , format_time_s(batch_time_span.count()).c_str(), format_time_s(batch_time_span.count() * (stp->input_files_size - saved_count) / saved_count).c_str()
				);
#endif
			}

        } else {
#if _WIN32
            fwprintf(stderr, L"save result failed: %ls\n", v.outpath.c_str());
#else
            fprintf(stderr, "save result failed: %s\n", v.outpath.c_str());
#endif
        }
    }

    return 0;
}

#if _WIN32
const std::wstring& optarg_in (L"");
const std::wstring& optarg_out(L"");
const std::wstring& optarg_mo (L"models-MNN/ESRGAN-MoeSR-jp_Illustration-x4.mnn");
#else
const char *optarg_in = "input.png";
const char *optarg_out = "output.png";
const char *optarg_mo = "models-MNN/ESRGAN-MoeSR-jp_Illustration-x4.mnn";
#endif

#if _WIN32
int wmain(int argc, wchar_t** argv)
#else

int main(int argc, char **argv)
#endif
{

    high_resolution_clock::time_point prg_start = high_resolution_clock::now();
    int backend_type = MNN_FORWARD_OPENCL;
    path_t inputpath;
    path_t outputpath;
    int scale = 4;
    int tilesize = 0;
    int color_type = UnSet;
    int decensor_mode = -1;
    path_t model = optarg_mo;
    std::vector<int> gpuid;
    int jobs_load = 1;
    std::vector<int> jobs_proc;
    int jobs_save = 1;
    int verbose = 0;
    path_t format = PATHSTR("png");

#if _WIN32
    setlocale(LC_ALL, "");
    wchar_t opt;
    while ((opt = getopt(argc, argv, L"b:i:o:s:c:d:t:m:g:j:f:vxh")) != (wchar_t)-1)
    {
        switch (opt)
        {
        case L'i':
            inputpath = optarg;
            break;
        case L'o':
            outputpath = optarg;
            break;
        case L's':
            scale = _wtoi(optarg);
            break;
        case L't':
            tilesize = _wtoi(optarg);
            break;
        case L'm':
            model = optarg;
            break;
        case L'g':
            gpuid = parse_optarg_int_array(optarg);
            if(gpuid.size()>0 && gpuid[0]==-1){
                backend_type = MNN_FORWARD_CPU;
            }
            break;
        //case L'j':
        //    swscanf(optarg, L"%d:%*[^:]:%d", &jobs_load, &jobs_save);
        //    jobs_proc = parse_optarg_int_array(wcschr(optarg, L':') + 1);
        //    break;
        case L'f':
            format = optarg;
            break;
        case L'v':
            verbose = 1;
            break;
        case L'x':
            break;
        case 'd':
            decensor_mode = _wtoi(optarg);
            break;
        case L'c':
            color_type = _wtoi(optarg);
            break;
        case L'b':
            if(backend_type != MNN_FORWARD_CPU)
                backend_type =_wtoi(optarg);
            break;
        case L'h':
        default:
            print_usage();
            return -1;
        }
    }
#else // _WIN32
    int opt;
    while ((opt = getopt(argc, argv, "b:i:o:s:c:d:t:m:g:j:f:vxh")) != -1) {
        switch (opt) {
            case 'i':
                inputpath = optarg;
                break;
            case 'o':
                outputpath = optarg;
                break;
            case 's':
                scale = atoi(optarg);
                break;
            case 't':
                tilesize = atoi(optarg);
                break;
            case 'm':
                model = optarg;
                break;
            case 'g':
                gpuid = parse_optarg_int_array(optarg);
                if (gpuid.size() > 0 && gpuid[0] == -1) {
                    backend_type = MNN_FORWARD_CPU;
                }
                break;
            //case 'j':
            //    sscanf(optarg, "%d:%*[^:]:%d", &jobs_load, &jobs_save);
            //    jobs_proc = parse_optarg_int_array(strchr(optarg, ':') + 1);
            //    break;
            case 'f':
                format = optarg;
                break;
            case 'v':
                verbose = 1;
                break;
            case 'x':
                break;
            case 'd':
                decensor_mode = atoi(optarg);
                break;
            case 'c':
                color_type = atoi(optarg);
                break;
            case 'b':
                if (backend_type != MNN_FORWARD_CPU)
                    backend_type = atoi(optarg);
                break;
            case 'h':
            default:
                print_usage();
                return -1;
        }
    }
#endif // _WIN32


    if (inputpath.empty()) {
        print_usage();
#if _DEMO_PATH
        fprintf(stderr, "demo input argument\n");
        inputpath = optarg_in;
#else
        return -1;
#endif
    }


    if (outputpath.empty()) {
        print_usage();
#if _DEMO_PATH
        fprintf(stderr, "demo output argument\n");
        outputpath = optarg_ou;
#else
        return -1;
#endif
    }


    if (jobs_load < 1 || jobs_save < 1) {
        fprintf(stderr, "invalid thread count argument\n");
        return -1;
    }



    // collect input and output filepath
    std::vector<path_t> input_files;
    std::vector<path_t> output_files;
    {
        if (path_is_directory(inputpath)) {

			if (fs::exists(outputpath)) {
				if (!path_is_directory(outputpath)) {
					fprintf(stderr, "[err]inputpath is directory, outputpath is file\n");
					return -1;
				}
			}
			else {
                fs::create_directories(outputpath);
				if (!fs::exists(outputpath)) {
                    #if _WIN32  
                    fwprintf(stderr, L"[err]create outputpath failed: %ls\n", outputpath.c_str());  
                    #else  
                    fprintf(stderr, "[err]create outputpath failed: %s\n", outputpath.c_str());  
                    #endif
					return -1;
				}
			}

            std::vector<path_t> filenames;
            int lr = list_directory(inputpath, filenames);
            if (lr != 0)
                return -1;

            const int count = filenames.size();
            input_files.resize(count);
            output_files.resize(count);

            path_t last_filename;
            path_t last_filename_noext;
            for (int i = 0; i < count; i++) {
                path_t filename = filenames[i];
                path_t filename_noext = get_file_name_without_extension(filename);
                path_t output_filename = filename_noext + PATHSTR('.') + format;

                if (filename_noext == last_filename_noext) {
                    path_t output_filename2 = filename + PATHSTR('.') + format;
#if _WIN32
                    fwprintf(stderr, L"both %ls and %ls output %ls ! %ls will output %ls\n", filename.c_str(), last_filename.c_str(), output_filename.c_str(), filename.c_str(), output_filename2.c_str());
#else
                    fprintf(stderr, "both %s and %s output %s ! %s will output %s\n",
                            filename.c_str(), last_filename.c_str(), output_filename.c_str(),
                            filename.c_str(), output_filename2.c_str());
#endif
                    output_filename = output_filename2;
                } else {
                    last_filename = filename;
                    last_filename_noext = filename_noext;
                }

                input_files[i] = inputpath + PATHSTR('/') + filename;
                output_files[i] = outputpath + PATHSTR('/') + output_filename;
            }
		}
		else if (fs::exists(inputpath)) {

			format = get_lowcase_extension(outputpath);
			if (format == PATHSTR("jpeg"))
				format = PATHSTR("jpg");
			if (format != PATHSTR("png") && format != PATHSTR("webp") && format != PATHSTR("jpg")) {
				//fprintf(stderr, "invalid format argument -%s-\n", format);
				return -1;
			}

			input_files.push_back(inputpath);
			output_files.push_back(outputpath);
		}
		else {
			fprintf(stderr, "[err]inputpath not exists\n");
			return -1;
		}
    }

	int prepadding = 0;

	//if (model.find(PATHSTR("models-")) != path_t::npos || model.ends_with(".mnn")) {
#if _WIN32
	if (model.find(PATHSTR("models-")) != path_t::npos || model.rfind(L".mnn") == (model.size() - 4)) {
#else
	if (model.find(PATHSTR("models-")) != path_t::npos || model.ends_with(".mnn")) {
#endif
		prepadding = 4;
	}
	else {
		fprintf(stderr, "unknown model dir type\n");
		return -1;
	}

    std::cout << "build time: " << __DATE__ << " " << __TIME__ << std::endl;

    if (color_type == UnSet) {
        if (model.find(PATHSTR("Grayscale")) != path_t::npos)
            color_type = GRAY;
        else if (model.find(PATHSTR("Gray2YCbCr")) != path_t::npos)
            color_type = Gray2YCbCr;
        else if (model.find(PATHSTR("Gray2YUV")) != path_t::npos)
            color_type = Gray2YUV;
        else if (model.find(PATHSTR("YCbCr")) != path_t::npos)
            color_type = YCbCr;
        else if (model.find(PATHSTR("YUV")) != path_t::npos)
            color_type = YUV;
        else
            color_type = RGB;
    }

    int scales[] = {4, 2, 1, 8};
    int sp = 0;

#if _WIN32
    wchar_t modelpath[256];
    //if(model.ends_with(".mnn")){
    if (model.rfind(L".mnn") == (model.size() - 4)) {
        swprintf(modelpath, 256, L"%s", model.c_str());
    }else{
        swprintf(modelpath, 256, L"%s/x%d.mnn", model.c_str(), scale);
    }
    fprintf(stderr, "search model: %ws\n", modelpath);

    path_t modelfullpath = sanitize_filepath(modelpath);
    FILE* mp = _wfopen(modelfullpath.c_str(), L"rb");
#else
    char modelpath[256];
    if (model.ends_with(".mnn"))
        sprintf(modelpath, "%s", model.c_str());
    else
        sprintf(modelpath, "%s/x%d.mnn", model.c_str(), scale);
    fprintf(stderr, "search model: %s\n", modelpath);

    path_t modelfullpath = sanitize_filepath(modelpath);
    FILE *mp = fopen(modelfullpath.c_str(), "rb");
#endif

    while (!mp && sp < 4) {
        int s = scales[sp];
#if _WIN32
        swprintf(modelpath, 256, L"%s/x%d.mnn", model.c_str(), s);

        modelfullpath = sanitize_filepath(modelpath);
        mp = _wfopen(modelfullpath.c_str(), L"rb");
#else
        sprintf(modelpath, "%s/x%d.mnn", model.c_str(), s);

        modelfullpath = sanitize_filepath(modelpath);
        mp = fopen(modelfullpath.c_str(), "rb");
#endif
        if (mp) {
            fprintf(stderr, "Fix scale: %d -> %d\n", scale, s);
            scale = s;
            break;
        } else {
            fprintf(stderr, "Fix scale fail -> %d\n", s);
            sp++;
        }
    };

    if (!mp) {
#if _WIN32
        fprintf(stderr, "Unknow scale for the model (%ws)\n", modelfullpath.c_str());
#else
        fprintf(stderr, "Unknow scale for the model (%s)\n", modelfullpath.c_str());
#endif
        return -1;
    }

    if (scale == 0) {
#if _WIN32
        using string_t = std::wstring;
        using regex_t = std::wregex;
        using smatch_t = std::wsmatch;
#define STR(x) L##x
#else
        using string_t = std::string;
        using regex_t = std::regex;
        using smatch_t = std::smatch;
#define STR(x) x
#endif
        // 获取文件名
        string_t filename = std::filesystem::path(model).filename().native();

        //regex_t re1(STR("(.+-|^)[xX]([0-9]+(\\.[0-9]+)?).*"));
        //regex_t re2(STR("(.+-|^)([0-9]+(\\.[0-9]+)?)[xX].*"));
        regex_t re1(STR("(.+-|^)[xX]([0-9]+).*"));
        regex_t re2(STR("(.+-|^)([0-9]+)[xX].*"));

        smatch_t match;
        if (std::regex_search(filename, match, re1) && match.size() > 1) {
            //scale = std::stod(match[2]);
            scale = std::stoi(match[2]);
        }
        else if (std::regex_search(filename, match, re2) && match.size() > 1) {
            //scale = std::stod(match[2]);
            scale = std::stoi(match[2]);
        }
    }


#include <cstdio>

#if _WIN32
#define FTELL _ftelli64
#define FSEEK _fseeki64
    typedef __int64 file_offset_t;
#else
#define FTELL ftello
#define FSEEK fseeko
    typedef off_t file_offset_t;
#endif

    FSEEK(mp, 0, SEEK_END);
    long modelsize = FTELL(mp) / 1000000;
    fclose(mp);

//#if _WIN32
//    CoInitializeEx(NULL, COINIT_MULTITHREADED);
//#endif


    int cpu_count = 4;
    jobs_load = std::min(jobs_load, cpu_count);
    jobs_save = 1;

    fprintf(stderr, "busy...\n");
    {

        MNNSR mnnsr = MNNSR(color_type,decensor_mode);
        if (tilesize == 0) {
            tilesize = 128;
            if (modelsize < 10)
                tilesize = 256;
            else if (modelsize < 16)
                tilesize = 128;
            else if (modelsize < 24)
                tilesize = 96;
            else
                tilesize = 64;
        }
        if (tilesize < 64)
            tilesize = 64;
        mnnsr.tilesize = tilesize;
        mnnsr.prepadding = prepadding;
        if (backend_type >= 0 && backend_type <= 14)
            mnnsr.backend_type = static_cast<MNNForwardType>(backend_type);

        //fprintf(stderr, "model loaded, %d MB, %s\n", modelsize, modelsize > 10 ? "cache" : "not cache");
        mnnsr.scale = scale;
        mnnsr.load(modelfullpath, modelsize > 10);

        // main routine
        {
            // load image
            LoadThreadParams ltp;
            if (decensor_mode == -1)
                ltp.scale = scale;
            else
                ltp.scale = 1;
            ltp.jobs_load = jobs_load;
            ltp.input_files = input_files;
            ltp.output_files = output_files;

            ncnn::Thread load_thread(load, (void *) &ltp);

            ProcThreadParams ptp;
            ptp.mnnsr = &mnnsr;
            ptp.decensor_mode = decensor_mode;

            ncnn::Thread *proc_thread;
            proc_thread = new ncnn::Thread(proc, (void *) &ptp);

            // save image
            SaveThreadParams stp;
            stp.verbose = verbose;
            stp.input_files_size = input_files.size();

            ncnn::Thread *save_thread;
            save_thread = new ncnn::Thread(save, (void *) &stp);

            // end
            load_thread.join();
            batch_start = high_resolution_clock::now();

            Task end;
            end.id = -233;

            toproc.put(end);

            proc_thread->join();
            delete proc_thread;

            tosave.put(end);

            save_thread->join();
            delete save_thread;
        }
    }


    high_resolution_clock::time_point prg_end = high_resolution_clock::now();
    duration<double> time_span = duration_cast<duration<double>>(prg_end - prg_start);
    fprintf(stderr, "Total use time: %.3lf\n", time_span.count());


    return 0;
}
