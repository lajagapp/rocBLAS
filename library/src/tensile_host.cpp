/* ************************************************************************
 * Copyright (C) 2019-2023 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell cop-
 * ies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IM-
 * PLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNE-
 * CTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * ************************************************************************/

// The implementation of the rocBLAS<->Tensile interface layer.

#include "rocblas.h"

extern "C" void rocblas_shutdown();

/*****************************************************************************
 * This is the only file in rocBLAS which should #include Tensile headers    *
 * or reference Tensile identifiers. tensile_host.hpp defines the interface. *
 *****************************************************************************/

#include "tensile_host.hpp"
//#include <Tensile/AMDGPU.hpp>
#include <Tensile/Contractions.hpp>
#include <Tensile/EmbeddedLibrary.hpp>
#include <Tensile/MasterSolutionLibrary.hpp>
#include <Tensile/PlaceholderLibrary.hpp>
#include <Tensile/Tensile.hpp>
#include <Tensile/TensorDescriptor.hpp>
#include <Tensile/Utils.hpp>
#include <Tensile/hip/HipHardware.hpp>
#include <Tensile/hip/HipSolutionAdapter.hpp>
#include <Tensile/hip/HipUtils.hpp>
#include <atomic>
#include <complex>
#include <exception>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <type_traits>
#include <vector>

#ifdef WIN32
#include <windows.h>

#include <fileapi.h>
#include <io.h>
#include <libloaderapi.h>
#define ROCBLAS_LIB_PATH "C:/hipSDK/rocblas/bin"
#else
#include <glob.h>
#include <libgen.h>
#include <link.h>
#include <unistd.h>
#define ROCBLAS_LIB_PATH "/opt/rocm/lib/rocblas"
#endif

#if __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
// cppcheck-suppress preprocessorErrorDirective
#elif __has_include(<experimental/filesystem>)
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
#error no filesystem found
#endif

namespace
{
#ifndef WIN32
    std::string rocblas_so_path;

    int rocblas_dl_iterate_phdr_callback(struct dl_phdr_info* hdr_info, size_t size, void* data)
    {
        // uncomment to see all dependent .so files
        //fprintf(stderr, "rocblas so file: %s\n", hdr_info->dlpi_name);
        if(hdr_info->dlpi_name && strstr(hdr_info->dlpi_name, "rocblas."))
        {
            rocblas_so_path = hdr_info->dlpi_name;
        }
        return 0;
    }
#endif

    /******************************************************
     * Map a rocBLAS type to a corresponding Tensile type *
     ******************************************************/
    template <typename T>
    struct rocblas_to_tensile_type
    {
        using tensile_type = T;
    };

    template <>
    struct rocblas_to_tensile_type<rocblas_float_complex>
    {
        using tensile_type = std::complex<float>;
    };

    template <>
    struct rocblas_to_tensile_type<rocblas_double_complex>
    {
        using tensile_type = std::complex<double>;
    };

    template <>
    struct rocblas_to_tensile_type<rocblas_half>
    {
        using tensile_type = Tensile::Half;
    };

    template <>
    struct rocblas_to_tensile_type<rocblas_bfloat16>
    {
        using tensile_type = Tensile::BFloat16;
    };

    template <>
    struct rocblas_to_tensile_type<int8_t>
    {
        using tensile_type = int8_t;
    };

    /********************************************************************
     * Variable template to map a rocBLAS type into a Tensile::DataType *
     ********************************************************************/
    template <typename>
    constexpr auto tensile_datatype = nullptr;

    template <>
    constexpr auto tensile_datatype<int8_t> = Tensile::DataType::Int8;

    template <>
    constexpr auto tensile_datatype<int32_t> = Tensile::DataType::Int32;

    template <>
    constexpr auto tensile_datatype<rocblas_half> = Tensile::DataType::Half;

    template <>
    constexpr auto tensile_datatype<rocblas_bfloat16> = Tensile::DataType::BFloat16;

    template <>
    constexpr auto tensile_datatype<float> = Tensile::DataType::Float;

    template <>
    constexpr auto tensile_datatype<double> = Tensile::DataType::Double;

    template <>
    constexpr auto tensile_datatype<rocblas_float_complex> = Tensile::DataType::ComplexFloat;

    template <>
    constexpr auto tensile_datatype<rocblas_double_complex> = Tensile::DataType::ComplexDouble;

    // The workspace sizes for Tensile are rounded to multiples of HPA_GSU_WORKSPACE_SIZE_GRANULARITY
    // to reduce fragmentation in the Tensile Solution cache
    constexpr size_t HPA_GSU_WORKSPACE_SIZE_GRANULARITY = 256;

    Tensile::PerformanceMetric performanceMetricMap(rocblas_performance_metric metric)
    {
        switch(metric)
        {
        case rocblas_cu_efficiency_performance_metric:
            return Tensile::PerformanceMetric::CUEfficiency;
        case rocblas_device_efficiency_performance_metric:
        default:
            return Tensile::PerformanceMetric::DeviceEfficiency;
        }
    }

    /*************************************************************************
     * Class for converting alpha and beta between rocBLAS and Tensile types *
     * By default, alpha and beta are the same type as Tc compute_type       *
     *************************************************************************/
    template <typename Ti, typename To = Ti, typename Tc = To>
    struct AlphaBeta
    {
        using tensile_type = typename rocblas_to_tensile_type<Tc>::tensile_type;
        static void copy(tensile_type* dst, const Tc* src)
        {
            static_assert(sizeof(*src) == sizeof(*dst),
                          "Tensile and rocBLAS types are not the same size");
            static_assert(std::is_standard_layout<tensile_type>{} && std::is_standard_layout<Tc>{},
                          "Tensile or rocBLAS types are not standard layout types");
            memcpy(dst, src, sizeof(*dst));
        }
    };

    /****************************************************************
     * Construct a Tensile Problem from a RocblasContractionProblem *
     ****************************************************************/
    template <typename Ti, typename To, typename Tc>
    auto ConstructTensileProblem(const RocblasContractionProblem<Ti, To, Tc>& prob)
    {
        // Tensile DataTypes corresponding to rocBLAS data types
        static constexpr Tensile::DataType Tensile_Ti = tensile_datatype<Ti>;
        static constexpr Tensile::DataType Tensile_To = tensile_datatype<To>;
        static constexpr Tensile::DataType Tensile_Tc = tensile_datatype<Tc>;

        // Tensor descriptors for a, b
        Tensile::TensorDescriptor a, b;

        // Tensor ops for matrices, like complex conjugate
        Tensile::TensorOps aops, bops, cops, dops;

        // Tensile Indices for contraction problem
        Tensile::ContractionProblem::FreeIndices  freeIndex(2);
        Tensile::ContractionProblem::BoundIndices boundIndex(1);
        Tensile::ContractionProblem::BatchIndices batchIndex{{2, 2, 2, 2}};

        // Set up GEMM indices
        freeIndex[0].isA = true;
        freeIndex[1].isA = false;
        freeIndex[0].c = freeIndex[0].d = 0;
        freeIndex[1].c = freeIndex[1].d = 1;

        // We set K=0 when alpha==0.
        // This makes alpha==0 a change in the problem, and not just a change in the inputs.
        // It optimizes all problems with alpha==0 into K=0 and alpha=(don't care)
        auto k = prob.k && *prob.alpha ? prob.k : 0;

        // clang-format off

        // If A is transposed, swap the free and bound dimensions and their ranks
        if(prob.trans_a != rocblas_operation_none)
        {
            a = {
                    Tensile_Ti,
                    {k, prob.m, prob.batch_count},
                    {prob.row_stride_a, prob.col_stride_a, prob.batch_stride_a},
                    prob.buffer_offset_a
                };
            freeIndex[0].i  = 1;
            boundIndex[0].a = 0;
        }
        else
        {
            a = {
                    Tensile_Ti,
                    {prob.m, k, prob.batch_count},
                    {prob.row_stride_a, prob.col_stride_a, prob.batch_stride_a},
                    prob.buffer_offset_a
                };
            freeIndex[0].i  = 0;
            boundIndex[0].a = 1;
        }

        // If A is complex and conjugated, add a ComplexConjugate op to aops
        if(rocblas_is_complex<Ti> && prob.trans_a == rocblas_operation_conjugate_transpose)
            aops.push_back(Tensile::TensorOp::Type::ComplexConjugate);

        // If B is transposed, swap the free and bound dimensions and their ranks
        if(prob.trans_b != rocblas_operation_none)
        {
            b = {
                    Tensile_Ti,
                    {prob.n, k, prob.batch_count},
                    {prob.row_stride_b, prob.col_stride_b, prob.batch_stride_b},
                    prob.buffer_offset_b
                };
            freeIndex[1].i  = 0;
            boundIndex[0].b = 1;
        }
        else
        {
            b = {
                    Tensile_Ti,
                    {k, prob.n, prob.batch_count},
                    {prob.row_stride_b, prob.col_stride_b, prob.batch_stride_b},
                    prob.buffer_offset_b
                };
            freeIndex[1].i  = 1;
            boundIndex[0].b = 0;
        }

        // clang-format on

        // If B is complex and conjugated, add a ComplexConjugate op to bops
        if(rocblas_is_complex<Ti> && prob.trans_b == rocblas_operation_conjugate_transpose)
            bops.push_back(Tensile::TensorOp::Type::ComplexConjugate);

        // Descriptor for input matrix C
        Tensile::TensorDescriptor c{Tensile_To,
                                    {prob.m, prob.n, prob.batch_count},
                                    {prob.row_stride_c, prob.col_stride_c, prob.batch_stride_c},
                                    prob.buffer_offset_c};

        // Descriptor for output matrix D
        Tensile::TensorDescriptor d{Tensile_To,
                                    {prob.m, prob.n, prob.batch_count},
                                    {prob.row_stride_d, prob.col_stride_d, prob.batch_stride_d},
                                    prob.buffer_offset_d};

        // Size of GSU workspace. We set it to max size_t if this is a size query.
        size_t workspace_size
            = prob.handle->is_device_memory_size_query()
                  ? ~size_t{0}
                  : (prob.handle->get_available_workspace() / HPA_GSU_WORKSPACE_SIZE_GRANULARITY)
                        * HPA_GSU_WORKSPACE_SIZE_GRANULARITY;

        // The ContractionProblem
        Tensile::ContractionProblem tensileProblem{a,
                                                   aops,
                                                   b,
                                                   bops,
                                                   c,
                                                   cops,
                                                   d,
                                                   dops,
                                                   freeIndex,
                                                   batchIndex,
                                                   boundIndex,
                                                   value_category(*prob.beta),
                                                   workspace_size};

        tensileProblem.setAlphaType(Tensile_Tc);
        tensileProblem.setBetaType(Tensile_Tc);

        // HPA is active iff sizeof(compute type) > sizeof(input type)
        tensileProblem.setHighPrecisionAccumulate(sizeof(Tc) > sizeof(Ti));

        // Environment variable to force use of VALU for double precision gemm
        static bool force_valu_for_dgemm = std::getenv("ROCBLAS_INTERNAL_FORCE_VALU_FOR_DGEMM");
        if(std::is_same_v<
               Ti,
               double> && std::is_same_v<To, double> && std::is_same_v<Tc, double> && force_valu_for_dgemm)
        {
            tensileProblem.setArithmeticUnit(Tensile::ArithmeticUnit::VALU);
        }

        // Pass atomics mode to Tensile interface
        tensileProblem.setDeterministicMode(prob.handle->atomics_mode
                                            == rocblas_atomics_not_allowed);

        // set batch mode
        tensileProblem.setStridedBatched(prob.strided_batch);

        rocblas_performance_metric metric;
        rocblas_get_performance_metric(prob.handle, &metric);
        //If flag is set use CUEfficiency performance metric
        if(prob.flags & rocblas_gemm_flags_use_cu_efficiency)
            tensileProblem.setPerformanceMetric(Tensile::PerformanceMetric::CUEfficiency);
        //Otherwise use handle to determine metric
        else if(metric != rocblas_default_performance_metric)
            tensileProblem.setPerformanceMetric(performanceMetricMap(metric));

        // alpha and beta are stored by value in Tensile::TypedContractionInputs
        // alpha and beta are copied from host to Tensile::TypedContractionInputs
        // If k==0, we do not need to dereference prob.alpha and can set tensileAlpha=0
        // Not positive if this is necessary here as well
        typename AlphaBeta<Ti, To, Tc>::tensile_type tensileAlpha;
        if(prob.k)
            AlphaBeta<Ti, To, Tc>::copy(&tensileAlpha, prob.alpha);
        else
            memset(&tensileAlpha, 0, sizeof(tensileAlpha));
        tensileProblem.setAlphaRestriction(Tensile::toScalarValueEnum(tensileAlpha));

        // Add problem predicates for CEqualsD
        tensileProblem.setCEqualsD(prob.C == prob.D);

        static const char* fp16AltImplEnvStr = std::getenv("ROCBLAS_INTERNAL_FP16_ALT_IMPL");
        static const int   fp16AltImplEnv
            = (fp16AltImplEnvStr == NULL ? -1 : (std::atoi(fp16AltImplEnvStr) == 0 ? 0 : 1));
        if(fp16AltImplEnv != -1)
            tensileProblem.setFp16AltImpl(fp16AltImplEnv);
        else
            tensileProblem.setFp16AltImpl((prob.flags & rocblas_gemm_flags_fp16_alt_impl) == 0 ? 0
                                                                                               : 1);

        static const char* fp16AltImplRoundEnvStr
            = std::getenv("ROCBLAS_INTERNAL_FP16_ALT_IMPL_RNZ");
        static const int fp16AltImplRoundEnv
            = (fp16AltImplRoundEnvStr == NULL ? -1
                                              : (std::atoi(fp16AltImplRoundEnvStr) == 0 ? 0 : 1));
        if(fp16AltImplRoundEnv != -1)
            tensileProblem.setFp16AltImplRound(fp16AltImplRoundEnv);
        else
            tensileProblem.setFp16AltImplRound(
                (prob.flags & rocblas_gemm_flags_fp16_alt_impl_rnz) == 0 ? 0 : 1);

        return tensileProblem;
    }

    /***************************************************************
     * Construct the inputs to a Tensile ContractionProblem        *
     ***************************************************************/
    template <typename Ti, typename To, typename Tc>
    auto GetTensileInputs(const RocblasContractionProblem<Ti, To, Tc>& prob)
    {
        // Tensile types corresponding to Ti, To, Tc
        using Tensile_Ti          = typename rocblas_to_tensile_type<Ti>::tensile_type;
        using Tensile_To          = typename rocblas_to_tensile_type<To>::tensile_type;
        using Tensile_Talpha_beta = typename AlphaBeta<Ti, To, Tc>::tensile_type;

        // Make sure rocBLAS and Tensile types are compatible
        static_assert(sizeof(Tensile_Ti) == sizeof(Ti) && sizeof(Tensile_To) == sizeof(To),
                      "Tensile and rocBLAS types are not the same size");

        static_assert(std::is_standard_layout<Ti>{} && std::is_standard_layout<Tensile_Ti>{}
                          && std::is_standard_layout<To>{} && std::is_standard_layout<Tensile_To>{},
                      "Tensile or rocBLAS types are not standard layout types");

        // Structure describing the inputs (A, B, C, D, alpha, beta)
        Tensile::TypedContractionInputs<Tensile_Ti,
                                        Tensile_Ti,
                                        Tensile_To,
                                        Tensile_To,
                                        Tensile_Talpha_beta,
                                        Tensile_Talpha_beta>
            inputs;

        // Set the A, B, C, D matrices pointers in Tensile
        inputs.a = reinterpret_cast<const Tensile_Ti*>(prob.A);
        inputs.b = reinterpret_cast<const Tensile_Ti*>(prob.B);
        inputs.c = reinterpret_cast<const Tensile_To*>(prob.C);
        inputs.d = reinterpret_cast<Tensile_To*>(prob.D);

        inputs.batchA = reinterpret_cast<Tensile_Ti const* const*>(prob.batch_A);
        inputs.batchB = reinterpret_cast<Tensile_Ti const* const*>(prob.batch_B);
        inputs.batchC = reinterpret_cast<Tensile_To const* const*>(prob.batch_C);
        inputs.batchD = reinterpret_cast<Tensile_To* const*>(prob.batch_D);

        // Set the GSU workspace
        inputs.ws = prob.handle->gsu_workspace;

        // alpha and beta are stored by value in Tensile::TypedContractionInputs
        // alpha and beta are copied from host to Tensile::TypedContractionInputs
        // If k==0, we do not need to dereference prob.alpha and can set inputs.alpha=0
        if(prob.k)
            AlphaBeta<Ti, To, Tc>::copy(&inputs.alpha, prob.alpha);
        else
            memset(&inputs.alpha, 0, sizeof(inputs.alpha));
        AlphaBeta<Ti, To, Tc>::copy(&inputs.beta, prob.beta);

        return inputs;
    }

    /**************************************************
     * The TensileHost struct interfaces with Tensile *
     **************************************************/
    class TensileHost
    {
        // The library object
        std::shared_ptr<Tensile::MasterSolutionLibrary<Tensile::ContractionProblem>> m_library;
        std::shared_ptr<hipDeviceProp_t>                                             m_deviceProp;

        // The adapter object. mutable is used to allow adapters to be modified
        // even when they are stored in a const vector which is immutable in size
        struct adapter_s
        {
            mutable std::atomic<Tensile::hip::SolutionAdapter*> adapter{nullptr};
            mutable std::mutex                                  mutex;
        };

        // Each device contains an adapter
        std::vector<adapter_s> const m_adapters;

    public:
        TensileHost()
            : m_adapters(GetDeviceCount())
        {
            // We mark TensileHost as initialized. This is so that CI tests can
            // verify that the initialization occurs in the "multiheaded" tests
            rocblas_internal_tensile_is_initialized() = true;
        }

        // TensileHost is not copyable or assignable
        TensileHost(const TensileHost&) = delete;

        TensileHost& operator=(const TensileHost&) = delete;

        // Get the number of devices
        static int GetDeviceCount()
        {
            int count;
            if(hipGetDeviceCount(&count) != hipSuccess)
            {
                rocblas_cerr
                    << "\nrocBLAS error: Could not initialize Tensile host: No devices found"
                    << std::endl;
                rocblas_abort();
            }
            return count;
        }

        ~TensileHost()
        {
            for(auto& a : m_adapters)
                delete a.adapter;
        }

        auto& get_library() const
        {
            return m_library;
        }

        auto& get_device_property() const
        {
            return m_deviceProp;
        }

        auto& get_adapters() const
        {
            return m_adapters;
        }

        /*******************************************************
         * Testpath() tests that a path exists and is readable *
         *******************************************************/
        static bool TestPath(const std::string& path)
        {
#ifdef WIN32
            return ((_access(path.c_str(), 4) != -1) || (_access(path.c_str(), 6) != -1));
#else
            return access(path.c_str(), R_OK) == 0;
#endif
        }

        /*********************************************************************
         * Initialize adapter and library according to environment variables *
         * and default paths based on librocblas.so location and GPU         *
         *********************************************************************/
        void initialize(Tensile::hip::SolutionAdapter& adapter, rocblas_int deviceId)
        {
            std::string path;
            std::string tensileLibraryPath;
            bool        tensile_lazy_load_enabled = false;
            static std::future<
                std::shared_ptr<Tensile::SolutionLibrary<Tensile::ContractionProblem>>>
                ftr_lib;

#ifndef WIN32
            path.reserve(PATH_MAX);
#endif

            // The name of the current GPU platform
            std::string processor = rocblas_internal_get_arch_name();
            // Get current xnack mode
            std::string xnack = rocblas_internal_get_xnack_mode();

            // If xnack mode is set, skip loading kernels for the opposite mode
            std::string skip_xnack;
            if(xnack == "xnack+")
                skip_xnack = "xnack-";
            else if(xnack == "xnack-")
                skip_xnack = "xnack+";

            const char* env = getenv("ROCBLAS_TENSILE_LIBPATH");
            if(env)
            {
                path = env;
            }
            else
            {
                path = ROCBLAS_LIB_PATH;

                // Find the location of librocblas.dll/.so
                // Fall back on hard-coded path if static library or not found

#ifndef ROCBLAS_STATIC_LIB
#ifdef WIN32
                // wchar_t wpath[MAX_PATH + 1] = {0};
                // if(GetModuleFileNameW(GetModuleHandle("rocblas.dll"), wpath, MAX_PATH + 1))
                // {
                //     std::wstring          wspath(wpath);
                //     std::string           tmp(wspath.begin(), wspath.end());

                std::vector<TCHAR> dll_path(MAX_PATH + 1);
                if(GetModuleFileNameA(
                       GetModuleHandleA("rocblas.dll"), dll_path.data(), MAX_PATH + 1))
                {
                    std::string           tmp(dll_path.begin(), dll_path.end());
                    std::filesystem::path exepath = tmp;
                    if(exepath.has_filename())
                    {
                        path = exepath.remove_filename().string();
                    }
                }
#else
                dl_iterate_phdr(rocblas_dl_iterate_phdr_callback, NULL);
                if(rocblas_so_path.size())
                    path = std::string{dirname(&rocblas_so_path[0])};
#endif
#endif // ifndef ROCBLAS_STATIC_LIB

                // Find the location of the libraries
                if(TestPath(path + "/../../Tensile/library"))
                    path += "/../../Tensile/library";
                else if(TestPath(path + "/library"))
                    path += "/library";
                else
                    path += "/rocblas/library";

                if(TestPath(path + "/" + processor))
                    path += "/" + processor;
            }

#ifdef TENSILE_YAML
            tensileLibraryPath = path + "/TensileLibrary_lazy_" + processor + ".yaml";
#else
            tensileLibraryPath = path + "/TensileLibrary_lazy_" + processor + ".dat";
#endif
            if(!TestPath(tensileLibraryPath))
            {

#ifdef TENSILE_YAML
                tensileLibraryPath = path + "/TensileLibrary_" + processor + ".yaml";
#else
                tensileLibraryPath = path + "/TensileLibrary_" + processor + ".dat";
#endif
                if(!TestPath(tensileLibraryPath))
                {
#ifdef TENSILE_YAML
                    tensileLibraryPath = path + "/TensileLibrary.yaml";
#else
                    tensileLibraryPath = path + "/TensileLibrary.dat";
#endif
                    if(!TestPath(tensileLibraryPath))
                    {
#if ROCBLAS_TENSILE_SEPARATE_ARCH
                        rocblas_cerr << "\nrocBLAS error: Cannot read " << tensileLibraryPath
                                     << ": " << strerror(errno) << " for GPU arch : " << processor
                                     << std::endl;
#if ROCBLAS_TENSILE_LAZY_LOAD
                        std::regex fileMatcher(path + "/TensileLibrary_lazy.*");
#else
                        std::regex fileMatcher(path + "/TensileLibrary_gfx\\d+.dat");
#endif
                        rocblas_cerr << " List of available TensileLibrary Files : " << std::endl;
                        for(auto& file_name : fs::directory_iterator(path))
                        {
                            if(std::regex_match(file_name.path().string(), fileMatcher))
                            {
                                rocblas_cerr << file_name << std::endl;
                            }
                        }
#else
                        rocblas_cerr << "\nrocBLAS error: Cannot read " << tensileLibraryPath
                                     << ": " << strerror(errno) << std::endl;
#endif
                        rocblas_abort();
                    }
                }
            }
            else
                tensile_lazy_load_enabled = true;

            if(!tensile_lazy_load_enabled || rocblas_initialize_called())
            {

                static int once = [&] {
                    ftr_lib = std::async(
                        std::launch::async,
                        Tensile::LoadLibraryFilePreload<Tensile::ContractionProblem>,
                        tensileLibraryPath,
                        std::vector<Tensile::LazyLoadingInit>{Tensile::LazyLoadingInit::All});
                    return 0;
                }();

                // only load modules for the current architecture
                auto dir = path + "/*" + processor + "*co";

                bool no_match = false;
#ifdef WIN32
                std::replace(dir.begin(), dir.end(), '/', '\\');
                WIN32_FIND_DATAA finddata;
                HANDLE           hfine = FindFirstFileA(dir.c_str(), &finddata);
                if(hfine != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        std::string codeObjectFile = path + "\\" + finddata.cFileName;
                        if(!skip_xnack.empty()
                           && codeObjectFile.find(skip_xnack) != std::string::npos)
                            continue;
                        // Skip experimental libraries
                        if(codeObjectFile.find("Experimental") != std::string::npos)
                            continue;
                        adapter.loadCodeObjectFile(codeObjectFile.c_str());
                    } while(FindNextFileA(hfine, &finddata));
                }
                else
                {
                    no_match = true;
                }
                FindClose(hfine);
#else
                glob_t glob_result{};
                int    g = glob(dir.c_str(), GLOB_NOSORT, nullptr, &glob_result);
                if(!g)
                {
                    for(size_t i = 0; i < glob_result.gl_pathc; ++i)
                    {
                        std::string cofile = glob_result.gl_pathv[i];
                        if(!skip_xnack.empty() && cofile.find(skip_xnack) != std::string::npos)
                            continue;
                        if(cofile.find("Experimental") != std::string::npos)
                            continue;
                        adapter.loadCodeObjectFile(cofile);
                    }
                }
                else if(g == GLOB_NOMATCH)
                {
                    no_match = true;
                }
                else
                {
                    // clang-format off
                static auto& once = rocblas_cerr
                                    << "\nrocBLAS warning: glob(\"" << dir << "\", ...) returned "
                                    << (g == GLOB_ABORTED ? "GLOB_ABORTED"
                                                          : g == GLOB_NOSPACE ? "GLOB_NOSPACE"
                                                                              : "an unknown error")
                                    << "." << std::endl;
                    // clang-format on
                }
                globfree(&glob_result);
#endif
                if(no_match)
                {
                    static auto& once
                        = rocblas_cerr
                          << "\nrocBLAS warning: No paths matched " << dir
                          << ". Make sure that ROCBLAS_TENSILE_LIBPATH is set correctly."
                          << std::endl;
                }
            }
            else // initialize lazy loading
            {
                static int once = [&] {
                    ftr_lib
                        = std::async(std::launch::async,
                                     Tensile::LoadLibraryFilePreload<Tensile::ContractionProblem>,
                                     tensileLibraryPath,
                                     std::vector<Tensile::LazyLoadingInit>{});

                    return 0;
                }();

                adapter.initializeLazyLoading(processor, path);
            }

            {
                static int once = [&] {
                    auto lib = ftr_lib.get();
                    if(!lib)
                        rocblas_cerr << "\nrocBLAS error: Could not load " << tensileLibraryPath
                                     << std::endl;
                    else
                    {
                        using MSL = Tensile::MasterSolutionLibrary<Tensile::ContractionProblem>;
                        m_library = std::dynamic_pointer_cast<MSL>(lib);
                    }
                    return 0;
                }();
            }
            if(!m_library)
            {
                rocblas_cerr << "\nrocBLAS error: Could not initialize Tensile library"
                             << std::endl;
                rocblas_abort();
            }

            hipDeviceProp_t prop;
            HIP_CHECK_EXC(hipGetDeviceProperties(&prop, deviceId));

            m_deviceProp = std::make_shared<hipDeviceProp_t>(prop);

            // Preload problem/solution mappings
            const char* overrideEnv = getenv("ROCBLAS_TENSILE_GEMM_OVERRIDE_PATH");
            if(overrideEnv)
            {
                std::string                        overridePath = overrideEnv;
                std::shared_ptr<Tensile::Hardware> hardware
                    = Tensile::hip::GetDevice(*m_deviceProp);
                bool success = m_library->setOverridesFromFile(*hardware, overridePath);

                if(!success)
                {
                    rocblas_cerr
                        << "\nrocBLAS warning: One or more problem overrides failed to load from: "
                        << overridePath << std::endl;
                }
            }
        }
    };

    // Return the library and adapter for the current HIP device
    auto& get_library_and_adapter(
        std::shared_ptr<Tensile::MasterSolutionLibrary<Tensile::ContractionProblem>>* library
        = nullptr,
        std::shared_ptr<hipDeviceProp_t>* deviceProp = nullptr,
        int                               device     = -1)
    try
    {
        // TensileHost is initialized on the first call
        static TensileHost host;

        if(device == -1)
            hipGetDevice(&device);

        // Adapter entry for the current HIP device ID
        auto& a       = host.get_adapters().at(device);
        auto* adapter = a.adapter.load(std::memory_order_acquire);

        // Once set, a.adapter contains the adapter for the current HIP device ID
        if(!adapter)
        {
            // Lock so that only one thread performs initialization of the adapter
            std::lock_guard<std::mutex> lock(a.mutex);

            adapter = a.adapter.load(std::memory_order_relaxed);
            if(!adapter)
            {
                // Allocate a new adapter using the current HIP device
                adapter = new Tensile::hip::SolutionAdapter;

                // Initialize the adapter and possibly the library
                host.initialize(*adapter, device);

                // Atomically change the adapter stored for this device ID
                a.adapter.store(adapter, std::memory_order_release);
            }
        }

        // If an adapter is found, it is assumed that the library is initialized
        if(library)
            *library = host.get_library();
        if(deviceProp)
            *deviceProp = host.get_device_property();

        return *adapter;
    }
    catch(const std::exception& e)
    {
        rocblas_cerr << "\nrocBLAS error: Could not initialize Tensile host:\n"
                     << e.what() << std::endl;
        rocblas_abort();
    }
    catch(...)
    {
        rocblas_cerr
            << "\nrocBLAS error: Could not initialize Tensile host:\nUnknown exception thrown"
            << std::endl;
        rocblas_abort();
    }

    /**************************************************************************
    * We normally print error messages only once, to avoid excessive logging *
    **************************************************************************/
    void print_once(const rocblas_internal_ostream& msg)
    {
        if(rocblas_suppress_tensile_error_messages())
            return;
        static constexpr char varname[] = "ROCBLAS_VERBOSE_TENSILE_ERROR";
        static const char*    verbose   = getenv(varname);
        if(!verbose)
        {
            static auto& once = rocblas_cerr
                                << msg
                                << "\nThis message will be only be displayed once, unless the "
                                << varname << " environment variable is set." << std::endl;
        }
        else
            rocblas_cerr << msg << std::endl;
    }

} // namespace

/******************************************************************************
 * runContractionProblem calls Tensile to run a contraction problem described *
 * by RocblasContractionProblem                                               *
 ******************************************************************************/
template <typename Ti, typename To, typename Tc>
rocblas_status runContractionProblem(const RocblasContractionProblem<Ti, To, Tc>& prob,
                                     rocblas_gemm_algo                            algo,
                                     int32_t                                      solution_index)
{
    rocblas_status                                status = rocblas_status_internal_error;
    std::shared_ptr<Tensile::ContractionSolution> solution;

    try
    {
        std::shared_ptr<Tensile::MasterSolutionLibrary<Tensile::ContractionProblem>> library;
        std::shared_ptr<hipDeviceProp_t>                                             deviceProp;
        std::shared_ptr<Tensile::Hardware>                                           hardware;

        auto& adapter = get_library_and_adapter(&library, &deviceProp, prob.handle->getDevice());

        hardware = Tensile::hip::GetDevice(*deviceProp);

        auto  tensile_prob  = ConstructTensileProblem(prob);
        auto  handle        = prob.handle;
        auto* fitness_query = handle->get_solution_fitness_query();

        if(algo == rocblas_gemm_algo_solution_index && solution_index > 0)
        {
            solution = library->getSolutionByIndex(solution_index - 1);
            // load solution if not already loaded
            if(!solution)
            {
                library->findAllSolutions(tensile_prob, *hardware);
                solution = library->getSolutionByIndex(solution_index - 1);
            }
        }
        else
        {
            solution = library->findBestSolution(tensile_prob, *hardware, fitness_query);
        }

        if(!solution)
        {
            if(solution_index > 0)
            {
                status = rocblas_status_invalid_value;
            }
            else
            {
                rocblas_internal_ostream msg;
                print_once(msg << "\nrocBLAS error: No Tensile solution found for " << prob);
                status = rocblas_status_not_implemented;
            }
        }
        else
        {
            if(fitness_query)
                status = rocblas_status_success;
            else if(handle->is_device_memory_size_query())
            {
                status = handle->set_optimal_device_memory_size(
                    ((solution->requiredWorkspaceSize(tensile_prob)
                      + HPA_GSU_WORKSPACE_SIZE_GRANULARITY - 1)
                     / HPA_GSU_WORKSPACE_SIZE_GRANULARITY)
                    * HPA_GSU_WORKSPACE_SIZE_GRANULARITY);
            }
            else
            {
                // check if the solution requires workspace for GSU and allocate it.
                size_t WorkspaceSize = solution->requiredWorkspaceSize(tensile_prob);
                auto   gsu_malloc    = prob.handle->gsu_malloc_by_size(WorkspaceSize);

                if(solution->canSolve(tensile_prob, *hardware))
                {
                    if(!(prob.flags & rocblas_gemm_flags_check_solution_index))
                    {
                        adapter.launchKernels(
                            solution->solve(tensile_prob, GetTensileInputs(prob), *hardware),
                            handle->get_stream(),
                            handle->startEvent,
                            handle->stopEvent);
                    }
                    status = rocblas_status_success;
                }
                else
                    status = rocblas_status_invalid_value;
            }
        }
    }
    catch(const std::exception& e)
    {
        rocblas_internal_ostream msg;
        print_once(msg << "\nrocBLAS error: " << (solution ? "" : "No ")
                       << "Tensile solution found, but exception thrown for " << prob << e.what());
    }
    catch(...)
    {
        rocblas_internal_ostream msg;
        print_once(msg << "\nrocBLAS error: " << (solution ? "" : "No ")
                       << "Tensile solution found, but unknown exception thrown for " << prob);
    }

    return status;
}

template <typename Ti, typename To, typename Tc>
rocblas_status getAllSolutions(const RocblasContractionProblem<Ti, To, Tc>& prob,
                               rocblas_tensile_get_solution_option          option,
                               rocblas_int*                                 list_array,
                               rocblas_int*                                 list_size)
{
    rocblas_status                                          status = rocblas_status_internal_error;
    std::set<std::shared_ptr<Tensile::ContractionSolution>> solutions;
    try
    {
        std::shared_ptr<Tensile::MasterSolutionLibrary<Tensile::ContractionProblem>> library;
        std::shared_ptr<hipDeviceProp_t>                                             deviceProp;
        std::shared_ptr<Tensile::Hardware>                                           hardware;

        auto& adapter = get_library_and_adapter(&library, &deviceProp, prob.handle->getDevice());
        hardware      = Tensile::hip::GetDevice(*deviceProp);
        auto tensile_prob = ConstructTensileProblem(prob);

        if(option == CAN_SOLVE)
        {
            solutions = library->findAllSolutions(tensile_prob, *hardware);
        }
        else if(option == MATCHES_TYPE)
        {
            solutions = library->findAllSolutionsMatchingType(tensile_prob, *hardware);
        }
        else
        {
            return rocblas_status_invalid_value;
        }

        if(list_size == nullptr)
        {
            status = rocblas_status_invalid_pointer;
        }
        else if(list_array == nullptr)
        {
            *list_size = solutions.size();
            status     = rocblas_status_success;
        }
        else
        {
            rocblas_int i  = 0;
            auto        it = solutions.begin();
            while(i < *list_size && it != solutions.end())
            {
                list_array[i] = it->get()->index + 1;
                ++it;
                ++i;
            }
            status = rocblas_status_success;
        }
    }
    catch(const std::exception& e)
    {
        rocblas_internal_ostream msg;
        print_once(msg << "\nrocBLAS error: exception thrown for " << prob << e.what());
    }
    catch(...)
    {
        rocblas_internal_ostream msg;
        print_once(msg << "\nrocBLAS error: unknown exception thrown for " << prob);
    }
    return status;
}

/***************************************************************
 * ! \brief  Initialize rocBLAS for the current HIP device, to *
 * avoid costly startup time at the first call on that device. *
 ***************************************************************/
extern "C" void rocblas_initialize()
{
    rocblas_initialize_called() = true;
    get_library_and_adapter();
}

/******************************************************************************
 * Intantiate the cases of runContractionProblem which are needed to satisfy  *
 * rocBLAS dependencies. This file's template functions are not defined in a  *
 * header file, in order to keep Tensile and rocBLAS separate.                *
 ******************************************************************************/

// Non-HPA/GEMM types
template rocblas_status runContractionProblem(const RocblasContractionProblem<rocblas_half>&,
                                              rocblas_gemm_algo algo,
                                              int32_t           solution_index);

template rocblas_status runContractionProblem(const RocblasContractionProblem<float>&,
                                              rocblas_gemm_algo algo,
                                              int32_t           solution_index);

template rocblas_status runContractionProblem(const RocblasContractionProblem<double>&,
                                              rocblas_gemm_algo algo,
                                              int32_t           solution_index);

template rocblas_status
    runContractionProblem(const RocblasContractionProblem<rocblas_float_complex>&,
                          rocblas_gemm_algo algo,
                          int32_t           solution_index);

template rocblas_status
    runContractionProblem(const RocblasContractionProblem<rocblas_double_complex>&,
                          rocblas_gemm_algo algo,
                          int32_t           solution_index);

// HPA types
template rocblas_status
    runContractionProblem(const RocblasContractionProblem<rocblas_half, rocblas_half, float>&,
                          rocblas_gemm_algo algo,
                          int32_t           solution_index);

template rocblas_status
    runContractionProblem(const RocblasContractionProblem<rocblas_half, float, float>&,
                          rocblas_gemm_algo algo,
                          int32_t           solution_index);

template rocblas_status runContractionProblem(
    const RocblasContractionProblem<rocblas_bfloat16, rocblas_bfloat16, float>&,
    rocblas_gemm_algo algo,
    int32_t           solution_index);

template rocblas_status
    runContractionProblem(const RocblasContractionProblem<rocblas_bfloat16, float, float>&,
                          rocblas_gemm_algo algo,
                          int32_t           solution_index);

template rocblas_status
    runContractionProblem(const RocblasContractionProblem<int8_t, int32_t, int32_t>&,
                          rocblas_gemm_algo algo,
                          int32_t           solution_index);

// ********** get all solutions explicits ********
// Non-HPA/GEMM types
template rocblas_status getAllSolutions(const RocblasContractionProblem<rocblas_half>&,
                                        rocblas_tensile_get_solution_option option,
                                        rocblas_int*                        list_array,
                                        rocblas_int*                        list_size);

template rocblas_status getAllSolutions(const RocblasContractionProblem<float>&,
                                        rocblas_tensile_get_solution_option option,
                                        rocblas_int*                        list_array,
                                        rocblas_int*                        list_size);

template rocblas_status getAllSolutions(const RocblasContractionProblem<double>&,
                                        rocblas_tensile_get_solution_option option,
                                        rocblas_int*                        list_array,
                                        rocblas_int*                        list_size);

template rocblas_status getAllSolutions(const RocblasContractionProblem<rocblas_float_complex>&,
                                        rocblas_tensile_get_solution_option option,
                                        rocblas_int*                        list_array,
                                        rocblas_int*                        list_size);

template rocblas_status getAllSolutions(const RocblasContractionProblem<rocblas_double_complex>&,
                                        rocblas_tensile_get_solution_option option,
                                        rocblas_int*                        list_array,
                                        rocblas_int*                        list_size);

// HPA types
template rocblas_status
    getAllSolutions(const RocblasContractionProblem<rocblas_half, rocblas_half, float>&,
                    rocblas_tensile_get_solution_option option,
                    rocblas_int*                        list_array,
                    rocblas_int*                        list_size);

template rocblas_status
    getAllSolutions(const RocblasContractionProblem<rocblas_half, float, float>&,
                    rocblas_tensile_get_solution_option option,
                    rocblas_int*                        list_array,
                    rocblas_int*                        list_size);

template rocblas_status
    getAllSolutions(const RocblasContractionProblem<rocblas_bfloat16, rocblas_bfloat16, float>&,
                    rocblas_tensile_get_solution_option option,
                    rocblas_int*                        list_array,
                    rocblas_int*                        list_size);

template rocblas_status
    getAllSolutions(const RocblasContractionProblem<rocblas_bfloat16, float, float>&,
                    rocblas_tensile_get_solution_option option,
                    rocblas_int*                        list_array,
                    rocblas_int*                        list_size);

template rocblas_status getAllSolutions(const RocblasContractionProblem<int8_t, int32_t, int32_t>&,
                                        rocblas_tensile_get_solution_option option,
                                        rocblas_int*                        list_array,
                                        rocblas_int*                        list_size);

/***********************************************************************************
 * Whether Tensile has been initialized for at least one device (used for testing) *
 ***********************************************************************************/
ROCBLAS_INTERNAL_EXPORT std::atomic_bool& rocblas_internal_tensile_is_initialized()
{
    static std::atomic_bool init;
    return init;
}

/***********************************************************************************
 * Whether rocblas_initialize() is invoked to load all tensile kernels at startup  *
 ***********************************************************************************/
std::atomic_bool& rocblas_initialize_called()
{
    static std::atomic_bool rocblas_init_called;
    return rocblas_init_called;
}
