#include "ggml-clblast.h"

#include <array>
#include <atomic>
#include <sstream>
#include <vector>
#include <limits>

#define CL_TARGET_OPENCL_VERSION 110
#include <clblast.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ggml.h"

#if defined(_MSC_VER)
#pragma warning(disable: 4244 4267) // possible loss of data
#endif

#define CL_DMMV_LOCAL_SIZE 32

#define CL_CHECK(err)                                               \
    do {                                                            \
        cl_int err_ = (err);                                        \
        if (err_ != CL_SUCCESS) {                                   \
            fprintf(stderr, "ggml_opencl: %s error %d at %s:%d\n",  \
                #err, err_, __FILE__, __LINE__);                    \
            exit(1);                                                \
        }                                                           \
    } while (0)

#define CLBLAST_CHECK(err)                                          \
    do {                                                            \
        CLBlastStatusCode err_ = (err);                             \
        if (err_ != CLBlastSuccess) {                               \
            fprintf(stderr, "ggml_opencl: %s error %d at %s:%d\n",  \
                #err, err_, __FILE__, __LINE__);                    \
            exit(1);                                                \
        }                                                           \
    } while (0)

// std::array<std::string, 5> dequant_str_keys = {
//     "KERNEL_NAME", "X_TYPE", "QUANT_K", "QUANT_R", "DEQUANT_FUNC"
// };

// std::array<std::string, 30> dequant_str_values = {
//     "dequantize_row_q4_0", "struct block_q4_0", "QK4_0", "QR4_0", "dequantize_q4_0",
//     "dequantize_row_q4_1", "struct block_q4_1", "QK4_1", "QR4_1", "dequantize_q4_1",
//     "dequantize_row_q5_0", "struct block_q5_0", "QK5_0", "QR5_0", "dequantize_q5_0",
//     "dequantize_row_q5_1", "struct block_q5_1", "QK5_1", "QR5_1", "dequantize_q5_1",
//     "dequantize_row_q8_0", "struct block_q8_0", "QK8_0", "QR8_0", "dequantize_q8_0",
//     "convert_row_f16", "half", "1", "1", "convert_f16"
// };

// std::array<std::string, 30> dequant_mul_mat_vec_str_values = {
//     "dequantize_mul_mat_vec_q4_0", "struct block_q4_0", "QK4_0", "QR4_0", "dequantize_q4_0",
//     "dequantize_mul_mat_vec_q4_1", "struct block_q4_1", "QK4_1", "QR4_1", "dequantize_q4_1",
//     "dequantize_mul_mat_vec_q5_0", "struct block_q5_0", "QK5_0", "QR5_0", "dequantize_q5_0",
//     "dequantize_mul_mat_vec_q5_1", "struct block_q5_1", "QK5_1", "QR5_1", "dequantize_q5_1",
//     "dequantize_mul_mat_vec_q8_0", "struct block_q8_0", "QK8_0", "QR8_0", "dequantize_q8_0",
//     "convert_mul_mat_vec_f16", "half", "1", "1", "convert_f16"
// };

// std::array<std::string, 2> mul_str_keys = {
//     "KERNEL_NAME", "TYPE"
// };
// std::array<std::string, 2> mul_str_values = {
//     "mul_f32", "float"
// };

// static std::string& replace(std::string& s, const std::string& from, const std::string& to) {
//     size_t pos = 0;
//     while ((pos = s.find(from, pos)) != std::string::npos) {
//          s.replace(pos, from.length(), to);
//          pos += to.length();
//     }
//     return s;
// }

// static std::string generate_kernels() {
//     std::stringstream src;
//     src << program_source << '\n';
//     src << k_quants_source << '\n';
//     for (size_t i = 0; i < dequant_str_values.size(); i += dequant_str_keys.size()) {
//         std::string dequant_kernel = dequant_template;
//         std::string dmmv_kernel = dequant_mul_mat_vec_template;
//         for (size_t j = 0; j < dequant_str_keys.size(); j++) {
//             replace(dequant_kernel, dequant_str_keys[j], dequant_str_values[i + j]);
//             replace(dmmv_kernel, dequant_str_keys[j], dequant_mul_mat_vec_str_values[i + j]);
//         }
//         src << dequant_kernel << '\n';
//         src << dmmv_kernel << '\n';
//     }
//     for (size_t i = 0; i < mul_str_values.size(); i += mul_str_keys.size()) {
//         std::string mul_kernel = mul_template;
//         for (size_t j = 0; j < mul_str_keys.size(); j++) {
//             replace(mul_kernel, mul_str_keys[j], mul_str_values[i + j]);
//         }
//         src << mul_kernel << '\n';
//     }

//     return src.str();
// }

static cl_platform_id platform;
static cl_device_id device;
static cl_context context;
static cl_command_queue queue;
static cl_program program;
// static cl_kernel convert_row_f16_cl;
// static cl_kernel dequantize_row_q4_0_cl, dequantize_row_q4_1_cl, dequantize_row_q5_0_cl, dequantize_row_q5_1_cl, dequantize_row_q8_0_cl;
// static cl_kernel dequantize_mul_mat_vec_q4_0_cl, dequantize_mul_mat_vec_q4_1_cl, dequantize_mul_mat_vec_q5_0_cl, dequantize_mul_mat_vec_q5_1_cl, dequantize_mul_mat_vec_q8_0_cl, convert_mul_mat_vec_f16_cl;
// static cl_kernel dequantize_block_q2_k_cl, dequantize_block_q3_k_cl, dequantize_block_q4_k_cl, dequantize_block_q5_k_cl, dequantize_block_q6_k_cl;
// static cl_kernel dequantize_mul_mat_vec_q2_K_cl, dequantize_mul_mat_vec_q3_K_cl, dequantize_mul_mat_vec_q4_K_cl, dequantize_mul_mat_vec_q5_K_cl, dequantize_mul_mat_vec_q6_K_cl;
// static cl_kernel mul_f32_cl;
static bool fp16_support;

// static cl_program build_program_from_source(cl_context ctx, cl_device_id dev, const char* program_buffer) {
//     cl_program p;
//     char *program_log;
//     size_t program_size;
//     size_t log_size;
//     int err;

//     program_size = strlen(program_buffer);

//     p = clCreateProgramWithSource(ctx, 1, (const char**)&program_buffer, &program_size, &err);
//     if(err < 0) {
//         fprintf(stderr, "OpenCL error creating program");
//         exit(1);
//     }

//     std::string compile_opts = "-cl-mad-enable -cl-unsafe-math-optimizations -cl-finite-math-only -cl-fast-relaxed-math "
//                                "-DQK4_0=32 -DQR4_0=2 -DQK4_1=32 -DQR4_1=2 -DQK5_0=32 -DQR5_0=2 -DQK5_1=32 -DQR5_1=2 -DQK8_0=32 -DQR8_0=1 "
//                                "-DQK_K=256 -DK_QUANTS_PER_ITERATION=" + std::to_string(K_QUANTS_PER_ITERATION);

//     err = clBuildProgram(p, 0, NULL, compile_opts.c_str(), NULL, NULL);
//     if(err < 0) {

//         clGetProgramBuildInfo(p, dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
//         program_log = (char*) malloc(log_size + 1);
//         program_log[log_size] = '\0';
//         clGetProgramBuildInfo(p, dev, CL_PROGRAM_BUILD_LOG, log_size + 1, program_log, NULL);
//         fprintf(stderr, "ggml_opencl: kernel compile error:\n\n%s\n", program_log);
//         free(program_log);
//         exit(1);
//     }

//     return p;
// }

void ggml_clblast_init(void) {
    cl_int err;

    struct cl_device;
    struct cl_platform {
        cl_platform_id id;
        unsigned number;
        char name[128];
        char vendor[128];
        struct cl_device * devices;
        unsigned n_devices;
        struct cl_device * default_device;
    };

    struct cl_device {
        struct cl_platform * platform;
        cl_device_id id;
        unsigned number;
        cl_device_type type;
        char name[128];
    };

    enum { NPLAT = 16, NDEV = 16 };

    struct cl_platform platforms[NPLAT];
    unsigned n_platforms = 0;
    struct cl_device devices[NDEV];
    unsigned n_devices = 0;
    struct cl_device * default_device = NULL;

    platform = NULL;
    device = NULL;

    cl_platform_id platform_ids[NPLAT];
    CL_CHECK(clGetPlatformIDs(NPLAT, platform_ids, &n_platforms));

    for (unsigned i = 0; i < n_platforms; i++) {
        struct cl_platform * p = &platforms[i];
        p->number = i;
        p->id = platform_ids[i];
        CL_CHECK(clGetPlatformInfo(p->id, CL_PLATFORM_NAME, sizeof(p->name), &p->name, NULL));
        CL_CHECK(clGetPlatformInfo(p->id, CL_PLATFORM_VENDOR, sizeof(p->vendor), &p->vendor, NULL));

        cl_device_id device_ids[NDEV];
        cl_int clGetDeviceIDsError = clGetDeviceIDs(p->id, CL_DEVICE_TYPE_ALL, NDEV, device_ids, &p->n_devices);
        if (clGetDeviceIDsError == CL_DEVICE_NOT_FOUND) {
            p->n_devices = 0;
        } else {
            CL_CHECK(clGetDeviceIDsError);
        }
        p->devices = p->n_devices > 0 ? &devices[n_devices] : NULL;
        p->default_device = NULL;

        for (unsigned j = 0; j < p->n_devices; j++) {
            struct cl_device * d = &devices[n_devices];
            d->number = n_devices++;
            d->id = device_ids[j];
            d->platform = p;
            CL_CHECK(clGetDeviceInfo(d->id, CL_DEVICE_NAME, sizeof(d->name), &d->name, NULL));
            CL_CHECK(clGetDeviceInfo(d->id, CL_DEVICE_TYPE, sizeof(d->type), &d->type, NULL));

            if (p->default_device == NULL && d->type == CL_DEVICE_TYPE_GPU) {
                p->default_device = d;
            }
        }

        if (default_device == NULL && p->default_device != NULL) {
            default_device = p->default_device;
        }
    }

    if (n_devices == 0) {
        fprintf(stderr, "ggml_opencl: could find any OpenCL devices.\n");
        exit(1);
    }

    char * user_platform_string = getenv("GGML_OPENCL_PLATFORM");
    char * user_device_string = getenv("GGML_OPENCL_DEVICE");
    int user_platform_number = -1;
    int user_device_number = -1;

    unsigned n;
    if (user_platform_string != NULL && sscanf(user_platform_string, " %u", &n) == 1 && n < n_platforms) {
        user_platform_number = (int)n;
    }
    if (user_device_string != NULL && sscanf(user_device_string, " %u", &n) == 1 && n < n_devices) {
        user_device_number = (int)n;
    }
    if (user_platform_number != -1 && user_device_number != -1) {
        cl_platform* platform = &platforms[user_platform_number];
        if ((unsigned)user_device_number >= platform->n_devices) {
            fprintf(stderr, "ggml_opencl: invalid device number %d\n", user_device_number);
            exit(1);
        }
        default_device = &platform->devices[user_device_number];
    } else {

        struct cl_device * selected_devices = devices;
        unsigned n_selected_devices = n_devices;

        if (user_platform_number == -1 && user_platform_string != NULL && user_platform_string[0] != 0) {
            for (unsigned i = 0; i < n_platforms; i++) {
                struct cl_platform * p = &platforms[i];
                if (strstr(p->name, user_platform_string) != NULL ||
                    strstr(p->vendor, user_platform_string) != NULL) {
                    user_platform_number = (int)i;
                    break;
                }
            }
            if (user_platform_number == -1) {
                fprintf(stderr, "ggml_opencl: no platform matching '%s' was found.\n", user_platform_string);
                exit(1);
            }
        }
        if (user_platform_number != -1) {
            struct cl_platform * p = &platforms[user_platform_number];
            selected_devices = p->devices;
            n_selected_devices = p->n_devices;
            default_device = p->default_device;
            if (n_selected_devices == 0) {
                fprintf(stderr, "ggml_opencl: selected platform '%s' does not have any devices.\n", p->name);
                exit(1);
            }
        }

        if (user_device_number == -1 && user_device_string != NULL && user_device_string[0] != 0) {
            for (unsigned i = 0; i < n_selected_devices; i++) {
                struct cl_device * d = &selected_devices[i];
                if (strstr(d->name, user_device_string) != NULL) {
                    user_device_number = d->number;
                    break;
                }
            }
            if (user_device_number == -1) {
                fprintf(stderr, "ggml_opencl: no device matching '%s' was found.\n", user_device_string);
                exit(1);
            }
        }
        if (user_device_number != -1) {
            selected_devices = &devices[user_device_number];
            n_selected_devices = 1;
            default_device = &selected_devices[0];
        }

        GGML_ASSERT(n_selected_devices > 0);

        if (default_device == NULL) {
            default_device = &selected_devices[0];
        }
    }

    fprintf(stderr, "ggml_opencl: selecting platform: '%s'\n", default_device->platform->name);
    fprintf(stderr, "ggml_opencl: selecting device: '%s'\n", default_device->name);
    if (default_device->type != CL_DEVICE_TYPE_GPU) {
        fprintf(stderr, "ggml_opencl: warning, not a GPU: '%s'.\n", default_device->name);
    }

    platform = default_device->platform->id;
    device = default_device->id;

    size_t ext_str_size;
    clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, 0, NULL, &ext_str_size);
    char *ext_buffer = (char *)alloca(ext_str_size + 1);
    clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, ext_str_size, ext_buffer, NULL);
    ext_buffer[ext_str_size] = '\0'; // ensure it is null terminated
    // Check if ext_buffer contains cl_khr_fp16
    fp16_support = strstr(ext_buffer, "cl_khr_fp16") != NULL;
    fprintf(stderr, "ggml_opencl: device FP16 support: %s\n", fp16_support ? "true" : "false");

    cl_context_properties properties[] = {
        (intptr_t)CL_CONTEXT_PLATFORM, (intptr_t)platform, 0
    };

    CL_CHECK((context = clCreateContext(properties, 1, &device, NULL, NULL, &err), err));

    CL_CHECK((queue = clCreateCommandQueue(context, device, CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE, &err),
        (err != CL_INVALID_QUEUE_PROPERTIES && err != CL_INVALID_VALUE ? err :
        (queue = clCreateCommandQueue(context, device, 0, &err), err)
    )));

    // const std::string kernel_src = generate_kernels();

    // program = build_program_from_source(context, device, kernel_src.c_str());

    // // FP16 to FP32 kernel
    // CL_CHECK((convert_row_f16_cl = clCreateKernel(program, "convert_row_f16", &err), err));

    // // Dequantize kernels
    // CL_CHECK((dequantize_row_q4_0_cl = clCreateKernel(program, "dequantize_row_q4_0", &err), err));
    // CL_CHECK((dequantize_row_q4_1_cl = clCreateKernel(program, "dequantize_row_q4_1", &err), err));
    // CL_CHECK((dequantize_row_q5_0_cl = clCreateKernel(program, "dequantize_row_q5_0", &err), err));
    // CL_CHECK((dequantize_row_q5_1_cl = clCreateKernel(program, "dequantize_row_q5_1", &err), err));
    // CL_CHECK((dequantize_row_q8_0_cl = clCreateKernel(program, "dequantize_row_q8_0", &err), err));
    // CL_CHECK((dequantize_row_q8_0_cl = clCreateKernel(program, "dequantize_row_q8_0", &err), err));
    // CL_CHECK((dequantize_block_q2_k_cl = clCreateKernel(program, "dequantize_block_q2_K", &err), err));
    // CL_CHECK((dequantize_block_q3_k_cl = clCreateKernel(program, "dequantize_block_q3_K", &err), err));
    // CL_CHECK((dequantize_block_q4_k_cl = clCreateKernel(program, "dequantize_block_q4_K", &err), err));
    // CL_CHECK((dequantize_block_q5_k_cl = clCreateKernel(program, "dequantize_block_q5_K", &err), err));
    // CL_CHECK((dequantize_block_q6_k_cl = clCreateKernel(program, "dequantize_block_q6_K", &err), err));

    // // dequant mul mat kernel
    // CL_CHECK((dequantize_mul_mat_vec_q4_0_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q4_0", &err), err));
    // CL_CHECK((dequantize_mul_mat_vec_q4_1_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q4_1", &err), err));
    // CL_CHECK((dequantize_mul_mat_vec_q5_0_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q5_0", &err), err));
    // CL_CHECK((dequantize_mul_mat_vec_q5_1_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q5_1", &err), err));
    // CL_CHECK((dequantize_mul_mat_vec_q8_0_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q8_0", &err), err));
    // CL_CHECK((convert_mul_mat_vec_f16_cl = clCreateKernel(program, "convert_mul_mat_vec_f16", &err), err));
    // CL_CHECK((dequantize_mul_mat_vec_q2_K_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q2_K", &err), err));
    // CL_CHECK((dequantize_mul_mat_vec_q3_K_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q3_K", &err), err));
    // CL_CHECK((dequantize_mul_mat_vec_q4_K_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q4_K", &err), err));
    // CL_CHECK((dequantize_mul_mat_vec_q5_K_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q5_K", &err), err));
    // CL_CHECK((dequantize_mul_mat_vec_q6_K_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q6_K", &err), err));

    // // mul kernel
    // CL_CHECK((mul_f32_cl = clCreateKernel(program, "mul_f32", &err), err));
}

// static cl_kernel* ggml_get_to_fp32_cl(ggml_type type) {
//     switch (type) {
//         case GGML_TYPE_Q4_0:
//             return &dequantize_row_q4_0_cl;
//         case GGML_TYPE_Q4_1:
//             return &dequantize_row_q4_1_cl;
//         case GGML_TYPE_Q5_0:
//             return &dequantize_row_q5_0_cl;
//         case GGML_TYPE_Q5_1:
//             return &dequantize_row_q5_1_cl;
//         case GGML_TYPE_Q8_0:
//             return &dequantize_row_q8_0_cl;
//         case GGML_TYPE_Q2_K:
//             return &dequantize_block_q2_k_cl;
//         case GGML_TYPE_Q3_K:
//             return &dequantize_block_q3_k_cl;
//         case GGML_TYPE_Q4_K:
//             return &dequantize_block_q4_k_cl;
//         case GGML_TYPE_Q5_K:
//             return &dequantize_block_q5_k_cl;
//         case GGML_TYPE_Q6_K:
//             return &dequantize_block_q6_k_cl;
//         case GGML_TYPE_F16:
//             return &convert_row_f16_cl;
//         default:
//             return nullptr;
//     }
// }

static size_t ggml_clblast_global_denom(ggml_type type) {
    switch (type) {
        case GGML_TYPE_Q4_0:
        case GGML_TYPE_Q4_1:
        case GGML_TYPE_Q5_0:
        case GGML_TYPE_Q5_1:
        case GGML_TYPE_Q8_0:
            return 1;
        case GGML_TYPE_Q2_K:
        case GGML_TYPE_Q3_K:
            return 4;
        case GGML_TYPE_Q4_K:
            return 8;
        case GGML_TYPE_Q5_K:
        case GGML_TYPE_Q6_K:
            return 4;
        case GGML_TYPE_F16:
        default:
            return 1;
    }
}

// static size_t ggml_clblast_local_size(ggml_type type) {
//     switch (type) {
//         case GGML_TYPE_Q4_0:
//         case GGML_TYPE_Q4_1:
//         case GGML_TYPE_Q5_0:
//         case GGML_TYPE_Q5_1:
//         case GGML_TYPE_Q8_0:
//             return 0;
//         case GGML_TYPE_Q2_K:
//         case GGML_TYPE_Q3_K:
//             return 64;
//         case GGML_TYPE_Q4_K:
//             return 32;
//         case GGML_TYPE_Q5_K:
//         case GGML_TYPE_Q6_K:
//             return 64;
//         case GGML_TYPE_F16:
//         default:
//             return 0;
//     }
// }

// static cl_kernel* ggml_get_dequantize_mul_mat_vec_cl(ggml_type type) {
//     switch (type) {
//         case GGML_TYPE_Q4_0:
//             return &dequantize_mul_mat_vec_q4_0_cl;
//         case GGML_TYPE_Q4_1:
//             return &dequantize_mul_mat_vec_q4_1_cl;
//         case GGML_TYPE_Q5_0:
//             return &dequantize_mul_mat_vec_q5_0_cl;
//         case GGML_TYPE_Q5_1:
//             return &dequantize_mul_mat_vec_q5_1_cl;
//         case GGML_TYPE_Q8_0:
//             return &dequantize_mul_mat_vec_q8_0_cl;
//         case GGML_TYPE_F16:
//             return &convert_mul_mat_vec_f16_cl;
//         case GGML_TYPE_Q2_K:
//             return &dequantize_mul_mat_vec_q2_K_cl;
//         case GGML_TYPE_Q3_K:
//             return &dequantize_mul_mat_vec_q3_K_cl;
//         case GGML_TYPE_Q4_K:
//             return &dequantize_mul_mat_vec_q4_K_cl;
//         case GGML_TYPE_Q5_K:
//             return &dequantize_mul_mat_vec_q5_K_cl;
//         case GGML_TYPE_Q6_K:
//             return &dequantize_mul_mat_vec_q6_K_cl;
//         default:
//             return nullptr;
//     }
// }

// buffer pool for cl
#define MAX_CL_BUFFERS 256

struct scoped_spin_lock {
    std::atomic_flag& lock;
    scoped_spin_lock(std::atomic_flag& lock) : lock(lock) {
        while (lock.test_and_set(std::memory_order_acquire)) {
            ; // spin
        }
    }
    ~scoped_spin_lock() {
        lock.clear(std::memory_order_release);
    }
    scoped_spin_lock(const scoped_spin_lock&) = delete;
    scoped_spin_lock& operator=(const scoped_spin_lock&) = delete;
};

struct cl_buffer {
    cl_mem mem;
    size_t size = 0;
};

static cl_buffer g_cl_buffer_pool[MAX_CL_BUFFERS];
static std::atomic_flag g_cl_pool_lock = ATOMIC_FLAG_INIT;

static cl_mem ggml_clblast_pool_malloc(size_t size, size_t * actual_size) {
    scoped_spin_lock lock(g_cl_pool_lock);
    cl_int err;

    int best_i = -1;
    size_t best_size = std::numeric_limits<size_t>::max(); //smallest unused buffer that fits our needs
    int worst_i = -1;
    size_t worst_size = 0; //largest unused buffer seen so far
    for (int i = 0; i < MAX_CL_BUFFERS; ++i) {
        cl_buffer &b = g_cl_buffer_pool[i];
        if (b.size > 0 && b.size >= size && b.size < best_size)
        {
            best_i = i;
            best_size = b.size;
        }
        if (b.size > 0 && b.size > worst_size)
        {
            worst_i = i;
            worst_size = b.size;
        }
    }
    if(best_i!=-1) //found the smallest buffer that fits our needs
    {
        cl_buffer& b = g_cl_buffer_pool[best_i];
        cl_mem mem = b.mem;
        *actual_size = b.size;
        b.size = 0;
        return mem;
    }
    if(worst_i!=-1) //no buffer that fits our needs, resize largest one to save memory
    {
         cl_buffer& b = g_cl_buffer_pool[worst_i];
         cl_mem mem = b.mem;
         b.size = 0;
         clReleaseMemObject(mem);
    }
    cl_mem mem;
    CL_CHECK((mem = clCreateBuffer(context, CL_MEM_READ_WRITE, size, NULL, &err), err));
    *actual_size = size;
    return mem;
}

static void ggml_clblast_pool_free(cl_mem mem, size_t size) {
    scoped_spin_lock lock(g_cl_pool_lock);

    for (int i = 0; i < MAX_CL_BUFFERS; ++i) {
        cl_buffer& b = g_cl_buffer_pool[i];
        if (b.size == 0) {
            b.mem = mem;
            b.size = size;
            return;
        }
    }
    fprintf(stderr, "WARNING: cl buffer pool full, increase MAX_CL_BUFFERS\n");
    clReleaseMemObject(mem);
}

void ggml_clblast_free_data(const struct ggml_tensor* tensor) {
    if (tensor->backend != GGML_BACKEND_GPU) {
        return;
    }

    cl_mem mem = (cl_mem)tensor->extra;
    clReleaseMemObject(mem);
}

static cl_int ggml_clblast_h2d_tensor_2d(cl_command_queue queue, cl_mem dst, size_t offset, const struct ggml_tensor * src, uint64_t i3, uint64_t i2, cl_event* ev) {
    cl_int err;
    const uint64_t ne0 = src->ne[0];
    const uint64_t ne1 = src->ne[1];
    const uint64_t nb0 = src->nb[0];
    const uint64_t nb1 = src->nb[1];
    const uint64_t nb2 = src->nb[2];
    const uint64_t nb3 = src->nb[3];
    const enum ggml_type type = src->type;
    const size_t ts = ggml_type_size(type);
    const size_t bs = ggml_blck_size(type);
    const uint64_t row_size = ts*ne0/bs;

    const char * x = (const char *) src->data + i2*nb2 + i3*nb3;
    if (nb0 == ts && nb1 == row_size) {
        return clEnqueueWriteBuffer(queue, dst, CL_FALSE, offset, ne1*row_size, x, 0, NULL, ev);
    }
    if (nb0 == ts) {
        const size_t buffer_origin[3] = { offset, 0, 0 };
        const size_t host_origin[3] = { 0, 0, 0 };
        const size_t region[3] = { row_size, ne1, 1 };
        return clEnqueueWriteBufferRect(queue, dst, CL_FALSE, buffer_origin, host_origin, region, row_size, 0, nb1, 0, x, 0, NULL, ev);
    }
    std::vector<cl_event> events;
    if (ev && ne1>1) events.reserve(ne1-1);
    for (uint64_t i1 = 0; i1 < ne1; i1++) {
        // pretend the row is a matrix with cols=1
        const size_t buffer_origin[3] = { offset + i1*row_size, 0, 0 };
        const size_t host_origin[3] = { 0, 0, 0 };
        const size_t region[3] = { ts, ne0/bs, 1 };
        // if an event is requested, make the last write wait for all previous writes to complete
        if (ev && i1) {
            events.push_back(*ev);
        }
        cl_uint nevents = i1 == ne1-1 ? events.size() : 0U;
        err = clEnqueueWriteBufferRect(queue, dst, CL_FALSE, buffer_origin, host_origin, region, ts, 0, nb0, 0, x + i1*nb1, nevents, nevents ? events.data() : nullptr, ev);
        if (err != CL_SUCCESS) {
            for (auto event : events) {
                clReleaseEvent(event);
            }
            return err;
        }
    }
    for (auto event : events) {
        CL_CHECK(clReleaseEvent(event));
    }
    return CL_SUCCESS;
}

// static void ggml_clblast_mul_f32(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
//     GGML_ASSERT(src1->backend == GGML_BACKEND_GPU);
//     const int64_t ne00 = src0->ne[0];
//     const int64_t ne01 = src0->ne[1];
//     const int64_t ne02 = src0->ne[2];
//     const int64_t ne03 = src0->ne[3];
//     const int64_t ne10 = src1->ne[0];
//     const int64_t ne11 = src1->ne[1];
//     const int64_t ne12 = src1->ne[2];
//     const int64_t ne13 = src1->ne[3];
//     const int nb2  = dst->nb[2];
//     const int nb3  = dst->nb[3];
//     size_t x_size;
//     size_t d_size;

//     cl_mem d_X = ggml_clblast_pool_malloc(ne00 * ne01 * sizeof(float), &x_size); // src0
//     cl_mem d_Y = (cl_mem) src1->extra; // src1 is already on device, broadcasted.
//     cl_mem d_D = ggml_clblast_pool_malloc(ne00 * ne01 * sizeof(float), &d_size); // dst


//     for (int64_t i03 = 0; i03 < ne03; i03++) {
//         for (int64_t i02 = 0; i02 < ne02; i02++) {
//             cl_event ev;

//             // copy src0 to device
//             CL_CHECK(ggml_clblast_h2d_tensor_2d(queue, d_X, 0, src0, i03, i02, &ev));

//             const int64_t i13 = i03%ne13;
//             const int64_t i12 = i02%ne12;
//             const int i1 = i13*ne12*ne11 + i12*ne11;

//             cl_int x_offset = 0;
//             cl_int y_offset = i1*ne10;
//             cl_int d_offset = 0;

//             size_t global = ne00 * ne01;
//             cl_int ky = ne10 * ne11;

//             CL_CHECK(clSetKernelArg(mul_f32_cl, 0, sizeof(cl_mem), &d_X));
//             CL_CHECK(clSetKernelArg(mul_f32_cl, 1, sizeof(cl_int), &x_offset));
//             CL_CHECK(clSetKernelArg(mul_f32_cl, 2, sizeof(cl_mem), &d_Y));
//             CL_CHECK(clSetKernelArg(mul_f32_cl, 3, sizeof(cl_int), &y_offset));
//             CL_CHECK(clSetKernelArg(mul_f32_cl, 4, sizeof(cl_mem), &d_D));
//             CL_CHECK(clSetKernelArg(mul_f32_cl, 5, sizeof(cl_int), &d_offset));
//             CL_CHECK(clSetKernelArg(mul_f32_cl, 6, sizeof(cl_int), &ky));
//             CL_CHECK(clEnqueueNDRangeKernel(queue, mul_f32_cl, 1, NULL, &global, NULL, 1, &ev, NULL));

//             CL_CHECK(clReleaseEvent(ev));
//             CL_CHECK(clFinish(queue));

//             // copy dst to host
//             float * d = (float *) ((char *) dst->data + i02*nb2 + i03*nb3);
//             CL_CHECK(clEnqueueReadBuffer(queue, d_D, true, 0, sizeof(float) * ne00*ne01, d, 0, NULL, NULL));
//         }
//     }
//     ggml_clblast_pool_free(d_X, x_size);
//     ggml_clblast_pool_free(d_D, d_size);
// }

// void ggml_clblast_mul(const struct ggml_tensor * src0, const struct ggml_tensor * src1, struct ggml_tensor * dst) {
//     GGML_ASSERT(src0->type == GGML_TYPE_F32 && src1->type == GGML_TYPE_F32 && dst->type == GGML_TYPE_F32);
//     ggml_clblast_mul_f32(src0, src1, dst);
// }

static void ggml_clblast_mul_mat_f32(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    const int64_t ne00 = src0->ne[0];
    const int64_t ne01 = src0->ne[1];
    const int64_t ne02 = src0->ne[2];
    const int64_t ne03 = src0->ne[3];

    const int64_t ne10 = src1->ne[0];
    const int64_t ne11 = src1->ne[1];
    const int64_t ne12 = src1->ne[2];
    const int64_t ne13 = src1->ne[3];

    const int nb2  = dst->nb[2];
    const int nb3  = dst->nb[3];

    const int64_t r2 = ne12 / ne02;
    const int64_t r3 = ne13 / ne03;

    const float alpha = 1.0f;
    const float beta = 0.0f;
    const int x_ne = ne01 * ne00;
    const int y_ne = ne11 * ne10;
    const int d_ne = ne11 * ne01;

    size_t x_size;
    size_t y_size;
    size_t d_size;
    cl_mem d_X;
    if (src0->backend == GGML_BACKEND_GPU) { // NOLINT
        d_X = (cl_mem) src0->extra;
    } else {
        d_X = ggml_clblast_pool_malloc(sizeof(float) * x_ne, &x_size);
    }
    cl_mem d_Y = ggml_clblast_pool_malloc(sizeof(float) * y_ne, &y_size);
    cl_mem d_D = ggml_clblast_pool_malloc(sizeof(float) * d_ne, &d_size);

    size_t x_offset = 0;

    for (int64_t i03 = 0; i03 < ne03; i03++) {
        // TODO: copy src0 here when r3>1
        for (int64_t i13 = i03 * r3, e13 = i13 + r3; i13 < e13; i13++) {
            for (int64_t i02 = 0; i02 < ne02; i02++) {
                // copy src0 to device
                CL_CHECK(ggml_clblast_h2d_tensor_2d(queue, d_X, 0, src0, i03, i02, NULL));

                for (int64_t i12 = i02 * r2, e12 = i12 + r2; i12 < e12; i12++) {
                    // copy src1 to device
                    CL_CHECK(ggml_clblast_h2d_tensor_2d(queue, d_Y, 0, src1, i13, i12, NULL));

                    CL_CHECK(clFinish(queue));

                    // compute
                    cl_event ev_sgemm;
                    clblast::StatusCode status = clblast::Gemm<cl_float>(clblast::Layout::kColMajor,
                                                               clblast::Transpose::kYes, clblast::Transpose::kNo,
                                                               ne01, ne11, ne10,
                                                               alpha,
                                                               d_X, x_offset, ne00,
                                                               d_Y, 0, ne10,
                                                               beta,
                                                               d_D, 0, ne01,
                                                               &queue, &ev_sgemm);

                    if (status != clblast::StatusCode::kSuccess) {
                        GGML_ASSERT(false);
                    }

                    // copy dst to host
                    float * d = (float *) ((char *) dst->data + i12*nb2 + i13*nb3);
                    CL_CHECK(clEnqueueReadBuffer(queue, d_D, true, 0, sizeof(float) * d_ne, d, 1, &ev_sgemm, NULL));
                }
            }
        }
    }

    if (src0->backend != GGML_BACKEND_GPU) {
        ggml_clblast_pool_free(d_X, x_size);
    }
    ggml_clblast_pool_free(d_Y, y_size);
    ggml_clblast_pool_free(d_D, d_size);
}

static void ggml_clblast_mul_mat_f16(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst, void * wdata, size_t wsize) {
    GGML_ASSERT(fp16_support);

    const int64_t ne00 = src0->ne[0];
    const int64_t ne01 = src0->ne[1];
    const int64_t ne02 = src0->ne[2];
    const int64_t ne03 = src0->ne[3];

    const int64_t ne10 = src1->ne[0];
    const int64_t ne11 = src1->ne[1];
    const int64_t ne12 = src1->ne[2];
    const int64_t ne13 = src1->ne[3];

    const int nb10 = src1->nb[0];
    const int nb11 = src1->nb[1];
    const int nb12 = src1->nb[2];
    const int nb13 = src1->nb[3];

    const int nb2  = dst->nb[2];
    const int nb3  = dst->nb[3];

    const int64_t r2 = ne12 / ne02;
    const int64_t r3 = ne13 / ne03;

    const ggml_fp16_t alpha = ggml_fp32_to_fp16(1.0f);
    const ggml_fp16_t beta = ggml_fp32_to_fp16(0.0f);
    const int x_ne = ne01 * ne00;
    const int y_ne = ne11 * ne10;
    const int d_ne = ne11 * ne01;

    GGML_ASSERT(wsize >= sizeof(ggml_fp16_t) * y_ne);
    GGML_ASSERT(wsize >= sizeof(ggml_fp16_t) * d_ne);
    ggml_fp16_t * const tmp = (ggml_fp16_t *) wdata;

    size_t x_size;
    size_t y_size;
    size_t d_size;
    cl_mem d_X;
    if (src0->backend == GGML_BACKEND_GPU) { // NOLINT
        d_X = (cl_mem) src0->extra;
    } else {
        d_X = ggml_clblast_pool_malloc(sizeof(ggml_fp16_t) * x_ne, &x_size);
    }
    cl_mem d_Y = ggml_clblast_pool_malloc(sizeof(ggml_fp16_t) * y_ne, &y_size);
    cl_mem d_D = ggml_clblast_pool_malloc(sizeof(ggml_fp16_t) * d_ne, &d_size);

    bool src1_cont_rows = nb10 == sizeof(float);
    bool src1_cont_cols = (size_t)nb11 == ne11*sizeof(float);

    size_t x_offset = 0;

    for (int64_t i03 = 0; i03 < ne03; i03++) {
        // TODO: copy src0 here when r3>1
        for (int64_t i13 = i03 * r3, e13 = i13 + r3; i13 < e13; i13++) {
            for (int64_t i02 = 0; i02 < ne02; i02++) {
                if (src0->backend == GGML_BACKEND_GPU) {
                    x_offset = (i03 * ne02 + i02) * x_ne;
                } else {
                    // copy src0 to device
                    CL_CHECK([[[[[[[[[[[[[[[[[ a0]]]]]]]]]]]]]]]]](queue, d_X, 0, src0, i03, i02, NULL));
                }

                for (int64_t i12 = i02 * r2, e12 = i12 + r2; i12 < e12; i12++) {
                    // convert src1 to fp16
                    // TODO: use multiple threads
                    char * src1i = (char *) src1->data + i13*nb13 + i12*nb12;
                    if (src1_cont_rows) {
                        if (src1_cont_cols) {
                            ggml_fp32_to_fp16_row((float *) src1i, tmp, ne10*ne11);
                        }
                        else {
                            for (int64_t i11 = 0; i11 < ne11; i11++) {
                                ggml_fp32_to_fp16_row((float *) (src1i + i11*nb11), tmp + i11*ne10, ne10);
                            }
                        }
                    }
                    else {
                        for (int64_t i11 = 0; i11 < ne11; i11++) {
                            for (int64_t i10 = 0; i10 < ne10; i10++) {
                                // very slow due to no inlining
                                tmp[i11*ne10 + i10] = ggml_fp32_to_fp16(*(float *) (src1i + i11*nb11 + i10*nb10));
                            }
                        }
                    }

                    // copy src1 to device
                    CL_CHECK(clEnqueueWriteBuffer(queue, d_Y, false, 0, sizeof(ggml_fp16_t) * y_ne, tmp, 0, NULL, NULL));

                    CL_CHECK(clFinish(queue));

                    // compute
                    cl_event ev_sgemm;
                    clblast::StatusCode status = clblast::Gemm<cl_half>(clblast::Layout::kColMajor,
                                                               clblast::Transpose::kYes, clblast::Transpose::kNo,
                                                               ne01, ne11, ne10,
                                                               alpha,
                                                               d_X, x_offset, ne00,
                                                               d_Y, 0, ne10,
                                                               beta,
                                                               d_D, 0, ne01,
                                                               &queue, &ev_sgemm);

                    if (status != clblast::StatusCode::kSuccess) {
                        GGML_ASSERT(false);
                    }

                    // copy dst to host, then convert to float
                    CL_CHECK(clEnqueueReadBuffer(queue, d_D, true, 0, sizeof(ggml_fp16_t) * d_ne, tmp, 1, &ev_sgemm, NULL));

                    float * d = (float *) ((char *) dst->data + i12*nb2 + i13*nb3);

                    ggml_fp16_to_fp32_row(tmp, d, d_ne);
                }
            }
        }
    }

    if (src0->backend != GGML_BACKEND_GPU) {
        ggml_clblast_pool_free(d_X, x_size);
    }
    ggml_clblast_pool_free(d_Y, y_size);
    ggml_clblast_pool_free(d_D, d_size);
}

static void ggml_clblast_mul_mat_q_f32(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    const int64_t ne00 = src0->ne[0];
    const int64_t ne01 = src0->ne[1];
    const int64_t ne02 = src0->ne[2];
    const int64_t ne03 = src0->ne[3];

    const int64_t ne10 = src1->ne[0];
    const int64_t ne11 = src1->ne[1];
    const int64_t ne12 = src1->ne[2];
    const int64_t ne13 = src1->ne[3];

    const int nb2  = dst->nb[2];
    const int nb3  = dst->nb[3];
    const ggml_type type = src0->type;
    const bool mul_mat_vec = ne11 == 1 && ne00%2 == 0;

    const int64_t r2 = ne12 / ne02;
    const int64_t r3 = ne13 / ne03;

    const float alpha = 1.0f;
    const float beta = 0.0f;
    const int x_ne = ne01 * ne00;
    const int y_ne = ne11 * ne10;
    const int d_ne = ne11 * ne01;
    const int x_bps = x_ne / ggml_blck_size(type); // blocks per 2D slice
    const size_t q_sz = ggml_type_size(type) * x_bps;

    size_t x_size;
    size_t y_size;
    size_t d_size;
    size_t q_size;
    cl_mem d_X = ggml_clblast_pool_malloc(sizeof(float) * x_ne, &x_size);
    cl_mem d_Y = ggml_clblast_pool_malloc(sizeof(float) * y_ne, &y_size);
    cl_mem d_D = ggml_clblast_pool_malloc(sizeof(float) * d_ne, &d_size);

    std::vector<no_init<float>> X;
    float * f32_data = nullptr;

    if (type == GGML_TYPE_F32) {
        f32_data = (float *) src0->data;
    } else {
        X.resize(x_size);
        f32_data = (float *) X.data();
        if (type == GGML_TYPE_F16) {
            ggml_fp16_to_fp32_row((ggml_fp16_t *)src0->data, f32_data, x_size);
        } else {
            GGML_ASSERT(ggml_is_quantized(type));
            auto qtype = ggml_internal_get_type_traits(type);
            qtype.to_float(src0->data, f32_data, x_size);
        }
    }
    
    // copy float32 of src0 to device
    CL_CHECK(clEnqueueWriteBuffer(queue, d_X, CL_FALSE, 0, sizeof(float) * x_size, f32_data, 0, NULL, NULL));
    CL_CHECK(clFinish(queue));

    // CL_CHECK(ggml_clblast_h2d_tensor_2d(queue, d_X, 0, src1, i13, i12, NULL));

    const size_t global_denom = ggml_clblast_global_denom(type);
    const size_t local = mul_mat_vec ? CL_DMMV_LOCAL_SIZE : ggml_clblast_local_size(type);

    size_t ev_idx = 0;
    std::vector<cl_event> events;

    for (int64_t i03 = 0; i03 < ne03; i03++) {
        // TODO: copy and dequantize src0 here when r3>1
        for (int64_t i13 = i03 * r3, e13 = i13 + r3; i13 < e13; i13++) {
            for (int64_t i02 = 0; i02 < ne02; i02++) {
                // copy src0 to device if necessary
                if (src0->backend == GGML_BACKEND_CPU) {
                    events.emplace_back();
                    CL_CHECK(ggml_clblast_h2d_tensor_2d(queue, d_Q, 0, src0, i03, i02, events.data() + ev_idx++));
                } else if (src0->backend == GGML_BACKEND_GPU) {
                    d_Q = (cl_mem) src0->extra;
                } else {
                    GGML_ASSERT(false);
                }

                for (int64_t i12 = i02 * r2, e12 = i12 + r2; i12 < e12; i12++) {
                    // Note: no specialized dequantize_mul_mat_vec kernel
                    { // CLBlast matrix matrix multiplication
                        // copy src1 to device
                        CL_CHECK(ggml_clblast_h2d_tensor_2d(queue, d_Y, 0, src1, i13, i12, NULL));

                        // wait for conversion
                        CL_CHECK(clFinish(queue));

                        // compute
                        events.emplace_back();
                        clblast::StatusCode status = clblast::Gemm<cl_float>(clblast::Layout::kColMajor,
                                                                   clblast::Transpose::kYes, clblast::Transpose::kNo,
                                                                   ne01, ne11, ne10,
                                                                   alpha,
                                                                   d_X, 0, ne00,
                                                                   d_Y, 0, ne10,
                                                                   beta,
                                                                   d_D, 0, ne01,
                                                                   &queue, events.data() + ev_idx++);

                        if (status != clblast::StatusCode::kSuccess) {
                            GGML_ASSERT(false);
                        }
                    }

                    // copy dst to host
                    float * d = (float *) ((char *) dst->data + i12*nb2 + i13*nb3);
                    CL_CHECK(clEnqueueReadBuffer(queue, d_D, true, 0, sizeof(float) * d_ne, d, 1, &events[events.size() - 1], NULL));
                    for (auto *event : events) {
                        clReleaseEvent(event);
                    }

                    ev_idx = 0;
                    events.clear();
                }
            }
        }
    }

    if (!mul_mat_vec) {
        ggml_clblast_pool_free(d_X, x_size);
    }
    ggml_clblast_pool_free(d_Y, y_size);
    ggml_clblast_pool_free(d_D, d_size);
    if (src0->backend == GGML_BACKEND_CPU) {
        ggml_clblast_pool_free(d_Q, q_size);
    }
}


bool ggml_clblast_can_mul_mat(const struct ggml_tensor * src0, const struct ggml_tensor * src1, struct ggml_tensor * dst) {
    const int64_t ne10 = src1->ne[0];

    const int64_t ne0 = dst->ne[0];
    const int64_t ne1 = dst->ne[1];

    // TODO: find the optimal values for these
    if ((src0->type == GGML_TYPE_F32 || src0->type == GGML_TYPE_F16 || ggml_is_quantized(src0->type)) &&
        src1->type == GGML_TYPE_F32 &&
        dst->type == GGML_TYPE_F32 &&
        ((ne0 >= 32 && ne1 >= 32 && ne10 >= 32) || src0->backend == GGML_BACKEND_GPU)) {
        return true;
    }

    return false;
}

static bool ggml_clblast_mul_mat_use_f16(const struct ggml_tensor * src0, const struct ggml_tensor * src1, struct ggml_tensor * /* dst */) {
    // If device doesn't support FP16
    if (!fp16_support) {
        return false;
    }

    size_t src0_sz = ggml_nbytes(src0);
    size_t src1_sz = ggml_nbytes(src1);

    // mul_mat_q: src0 is converted to fp32 on device
    size_t mul_mat_q_transfer = src0_sz + src1_sz;

    // mul_mat_f16: src1 is converted to fp16 on cpu
    size_t mul_mat_f16_transfer = src0_sz + sizeof(ggml_fp16_t) * ggml_nelements(src1);

    // choose the smaller one to transfer to the device
    // TODO: this is not always the best choice due to the overhead of converting to fp16
    return mul_mat_f16_transfer < mul_mat_q_transfer;
}

void ggml_clblast_mul_mat(const struct ggml_tensor * src0, const struct ggml_tensor * src1, struct ggml_tensor * dst, void * wdata, size_t wsize) {
    GGML_ASSERT(ggml_clblast_can_mul_mat(src0, src1, dst));

    if (src0->type == GGML_TYPE_F32) {
        ggml_clblast_mul_mat_f32(src0, src1, dst);
    }
    else if (src0->type == GGML_TYPE_F16) {
        if (ggml_clblast_mul_mat_use_f16(src0, src1, dst)) {
            ggml_clblast_mul_mat_f16(src0, src1, dst, wdata, wsize);
        }
        else {
            ggml_clblast_mul_mat_q_f32(src0, src1, dst);
        }
    }
    else if (ggml_is_quantized(src0->type)) {
        ggml_clblast_mul_mat_q_f32(src0, src1, dst);
    }
    else {
        GGML_ASSERT(false);
    }
}

size_t ggml_clblast_mul_mat_get_wsize(const struct ggml_tensor * src0, const struct ggml_tensor * src1, struct ggml_tensor * dst) {
    if (src0->type == GGML_TYPE_F16 && ggml_clblast_mul_mat_use_f16(src0, src1, dst)) {
        return sizeof(ggml_fp16_t) * std::max(src1->ne[0] * src1->ne[1], dst->ne[0] * dst->ne[1]);
    }
    return 0;
}

void ggml_clblast_transform_tensor(void * data, ggml_tensor * tensor) {
    const int64_t ne0 = tensor->ne[0];
    const int64_t ne1 = tensor->ne[1];
    const int64_t ne2 = tensor->ne[2];
    const int64_t ne3 = tensor->ne[3];

    const ggml_type type = tensor->type;
    const size_t s_sz = ggml_type_size(type) * (size_t) (ne0 * ne1 / ggml_blck_size(type));
    const size_t q_sz = s_sz * (size_t) (ne2 * ne3);

    size_t q_size;
    cl_mem dst = ggml_clblast_pool_malloc(q_sz, &q_size);

    tensor->data = data;
    // copy tensor to device
    size_t offset = 0;
    for (int64_t i3 = 0; i3 < ne3; i3++) {
        for (int64_t i2 = 0; i2 < ne2; i2++) {
            CL_CHECK(ggml_clblast_h2d_tensor_2d(queue, dst, offset, tensor, i3, i2, NULL));
            offset += s_sz;
        }
    }

    CL_CHECK(clFinish(queue));

    tensor->extra = dst;
    GGML_ASSERT(tensor->backend == GGML_BACKEND_GPU);
}
