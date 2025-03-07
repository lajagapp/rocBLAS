/* ************************************************************************
 * Copyright (C) 2018-2023 Advanced Micro Devices, Inc. All rights reserved.
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
 * ************************************************************************ */

#pragma once

#include "cblas_interface.hpp"
#include "norm.hpp"
#include "rocblas.hpp"
#include "rocblas_init.hpp"
#include "rocblas_math.hpp"
#include "rocblas_random.hpp"
#include "rocblas_test.hpp"
#include "rocblas_vector.hpp"
#include "unit.hpp"
#include "utility.hpp"

template <typename T>
void testing_rotm_strided_batched_bad_arg(const Arguments& arg)
{
    auto rocblas_rotm_strided_batched_fn = arg.api == FORTRAN
                                               ? rocblas_rotm_strided_batched<T, true>
                                               : rocblas_rotm_strided_batched<T, false>;

    rocblas_int         N            = 100;
    rocblas_int         incx         = 1;
    rocblas_stride      stride_x     = 1;
    rocblas_int         incy         = 1;
    rocblas_stride      stride_y     = 1;
    rocblas_stride      stride_param = 1;
    rocblas_int         batch_count  = 5;
    static const size_t safe_size    = 100;

    rocblas_local_handle handle{arg};

    // Allocate device memory
    device_strided_batch_vector<T> dx(N, incx, stride_x, batch_count);
    device_strided_batch_vector<T> dy(N, incy, stride_y, batch_count);
    device_strided_batch_vector<T> dparam(5, 1, stride_param, batch_count);

    // Check device memory allocation
    CHECK_DEVICE_ALLOCATION(dx.memcheck());
    CHECK_DEVICE_ALLOCATION(dy.memcheck());
    CHECK_DEVICE_ALLOCATION(dparam.memcheck());

    CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_device));
    EXPECT_ROCBLAS_STATUS(
        (rocblas_rotm_strided_batched_fn(
            nullptr, N, dx, incx, stride_x, dy, incy, stride_y, dparam, stride_param, batch_count)),
        rocblas_status_invalid_handle);
    EXPECT_ROCBLAS_STATUS((rocblas_rotm_strided_batched_fn(handle,
                                                           N,
                                                           nullptr,
                                                           incx,
                                                           stride_x,
                                                           dy,
                                                           incy,
                                                           stride_y,
                                                           dparam,
                                                           stride_param,
                                                           batch_count)),
                          rocblas_status_invalid_pointer);
    EXPECT_ROCBLAS_STATUS((rocblas_rotm_strided_batched_fn(handle,
                                                           N,
                                                           dx,
                                                           incx,
                                                           stride_x,
                                                           nullptr,
                                                           incy,
                                                           stride_y,
                                                           dparam,
                                                           stride_param,
                                                           batch_count)),
                          rocblas_status_invalid_pointer);
    EXPECT_ROCBLAS_STATUS(
        (rocblas_rotm_strided_batched_fn(
            handle, N, dx, incx, stride_x, dy, incy, stride_y, nullptr, stride_param, batch_count)),
        rocblas_status_invalid_pointer);
}

template <typename T>
void testing_rotm_strided_batched(const Arguments& arg)
{

    auto rocblas_rotm_strided_batched_fn = arg.api == FORTRAN
                                               ? rocblas_rotm_strided_batched<T, true>
                                               : rocblas_rotm_strided_batched<T, false>;

    rocblas_int N            = arg.N;
    rocblas_int incx         = arg.incx;
    rocblas_int stride_x     = arg.stride_x;
    rocblas_int stride_y     = arg.stride_y;
    rocblas_int stride_param = arg.stride_c;
    rocblas_int incy         = arg.incy;
    rocblas_int batch_count  = arg.batch_count;

    rocblas_local_handle handle{arg};
    double               gpu_time_used, cpu_time_used;
    double norm_error_host_x = 0.0, norm_error_host_y = 0.0, norm_error_device_x = 0.0,
           norm_error_device_y = 0.0;
    T rel_error                = std::numeric_limits<T>::epsilon() * 1000;
    // increase relative error for ieee64 bit
    if(std::is_same_v<T, double> || std::is_same_v<T, rocblas_double_complex>)
        rel_error *= 10.0;

    // check to prevent undefined memory allocation error
    if(N <= 0 || batch_count <= 0)
    {
        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_device));
        EXPECT_ROCBLAS_STATUS((rocblas_rotm_strided_batched_fn(handle,
                                                               N,
                                                               nullptr,
                                                               incx,
                                                               stride_x,
                                                               nullptr,
                                                               incy,
                                                               stride_y,
                                                               nullptr,
                                                               stride_param,
                                                               batch_count)),
                              rocblas_status_success);
        return;
    }

    // Naming: `h` is in CPU (host) memory(eg hx), `d` is in GPU (device) memory (eg dx).
    // Allocate host memory
    host_strided_batch_vector<T> hx(N, incx, stride_x, batch_count);
    host_strided_batch_vector<T> hy(N, incy, stride_y, batch_count);
    host_strided_batch_vector<T> hparam(5, 1, stride_param, batch_count);
    host_vector<T>               hdata(4 * batch_count);

    // Allocate device memory
    device_strided_batch_vector<T> dx(N, incx, stride_x, batch_count);
    device_strided_batch_vector<T> dy(N, incy, stride_y, batch_count);
    device_strided_batch_vector<T> dparam(5, 1, stride_param, batch_count);

    // Check device memory allocation
    CHECK_DEVICE_ALLOCATION(dx.memcheck());
    CHECK_DEVICE_ALLOCATION(dy.memcheck());
    CHECK_DEVICE_ALLOCATION(dparam.memcheck());

    // Initialize data on host memory
    rocblas_init_vector(hx, arg, rocblas_client_alpha_sets_nan, true);
    rocblas_init_vector(hy, arg, rocblas_client_alpha_sets_nan, false);
    rocblas_init_vector(hdata, arg, rocblas_client_alpha_sets_nan, false);

    for(int b = 0; b < batch_count; b++)
    {
        T* hparam_ptr = hparam[b];

        // generating simply one set of hparam which will not be appropriate for testing
        // that it zeros out the second element of the rotm vector parameter
        memset(hparam_ptr, 0, 5 * sizeof(T));

        cblas_rotmg<T>(
            hdata + b * 4, hdata + b * 4 + 1, hdata + b * 4 + 2, hdata + b * 4 + 3, hparam_ptr);
    }

    constexpr int FLAG_COUNT        = 4;
    const T       FLAGS[FLAG_COUNT] = {-1, 0, 1, -2};

    for(int i = 0; i < FLAG_COUNT; i++)
    {
        for(int b = 0; b < batch_count; b++)
            (hparam + b * stride_param)[0] = FLAGS[i];

        // CPU BLAS reference data
        host_strided_batch_vector<T> hx_gold(N, incx, stride_x, batch_count);
        host_strided_batch_vector<T> hy_gold(N, incy, stride_y, batch_count);

        hx_gold.copy_from(hx);
        hy_gold.copy_from(hy);

        cpu_time_used = get_time_us_no_sync();
        for(int b = 0; b < batch_count; b++)
        {
            cblas_rotm<T>(N, hx_gold[b], incx, hy_gold[b], incy, hparam[b]);
        }
        cpu_time_used = get_time_us_no_sync() - cpu_time_used;

        if(arg.unit_check || arg.norm_check)
        {
            // Test rocblas_pointer_mode_host
            // TODO: THIS IS NO LONGER SUPPORTED
            // {
            //     CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_host));
            //     CHECK_HIP_ERROR(hipMemcpy(dx, hx, sizeof(T) * size_x, hipMemcpyHostToDevice));
            //     CHECK_HIP_ERROR(hipMemcpy(dy, hy, sizeof(T) * size_y, hipMemcpyHostToDevice));
            //     CHECK_ROCBLAS_ERROR((rocblas_rotm_strided_batched_fn(
            //         handle, N, dx, incx, stride_x, dy, incy, stride_y, hparam, batch_count)));
            //     host_vector<T> rx(size_x);
            //     host_vector<T> ry(size_y);
            //     CHECK_HIP_ERROR(hipMemcpy(rx, dx, sizeof(T) * size_x, hipMemcpyDeviceToHost));
            //     CHECK_HIP_ERROR(hipMemcpy(ry, dy, sizeof(T) * size_y, hipMemcpyDeviceToHost));
            //     if(arg.unit_check)
            //     {
            //         T rel_error = std::numeric_limits<T>::epsilon() * 1000;
            //         near_check_general<T,T>(1, N, batch_count, incx, stride_x, hx_gold, rx, rel_error);
            //         near_check_general<T,T>(1, N, batch_count, incy, stride_y, hy_gold, ry, rel_error);
            //     }
            //     if(arg.norm_check)
            //     {
            //         norm_error_host_x
            //             = norm_check_general<T>('F', 1, N, incx, stride_x, batch_count, hx_gold, rx);
            //         norm_error_host_y
            //             = norm_check_general<T>('F', 1, N, incy, stride_x, batch_count, hy_gold, ry);
            //     }
            // }

            // Test rocblas_pointer_mode_device
            {
                CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_device));

                CHECK_HIP_ERROR(dx.transfer_from(hx));
                CHECK_HIP_ERROR(dy.transfer_from(hy));
                CHECK_HIP_ERROR(dparam.transfer_from(hparam));
                handle.pre_test(arg);

                CHECK_ROCBLAS_ERROR((rocblas_rotm_strided_batched_fn(handle,
                                                                     N,
                                                                     dx,
                                                                     incx,
                                                                     stride_x,
                                                                     dy,
                                                                     incy,
                                                                     stride_y,
                                                                     dparam,
                                                                     stride_param,
                                                                     batch_count)));
                handle.post_test(arg);

                CHECK_HIP_ERROR(hx.transfer_from(dx));
                CHECK_HIP_ERROR(hy.transfer_from(dy));

                if(arg.unit_check)
                {
                    near_check_general<T>(
                        1, N, incx, stride_x, hx_gold, hx, batch_count, rel_error);
                    near_check_general<T>(
                        1, N, incy, stride_y, hy_gold, hy, batch_count, rel_error);
                }

                if(arg.norm_check)
                {
                    norm_error_device_x += norm_check_general<T>(
                        'F', 1, N, incx, stride_x, hx_gold, hx, batch_count);
                    norm_error_device_y += norm_check_general<T>(
                        'F', 1, N, incy, stride_y, hy_gold, hy, batch_count);
                }
            }
        }
    }
    if(arg.timing)
    {
        // Initializing flag value to -1 for all the batches of hparam
        for(int b = 0; b < batch_count; b++)
        {
            (hparam + b * stride_param)[0] = FLAGS[0];
        }

        int number_cold_calls = arg.cold_iters;
        int number_hot_calls  = arg.iters;
        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_device));

        CHECK_HIP_ERROR(dx.transfer_from(hx));
        CHECK_HIP_ERROR(dy.transfer_from(hy));
        CHECK_HIP_ERROR(dparam.transfer_from(hparam));

        for(int iter = 0; iter < number_cold_calls; iter++)
        {
            rocblas_rotm_strided_batched_fn(handle,
                                            N,
                                            dx,
                                            incx,
                                            stride_x,
                                            dy,
                                            incy,
                                            stride_y,
                                            dparam,
                                            stride_param,
                                            batch_count);
        }
        hipStream_t stream;
        CHECK_ROCBLAS_ERROR(rocblas_get_stream(handle, &stream));
        gpu_time_used = get_time_us_sync(stream); // in microseconds
        for(int iter = 0; iter < number_hot_calls; iter++)
        {
            rocblas_rotm_strided_batched_fn(handle,
                                            N,
                                            dx,
                                            incx,
                                            stride_x,
                                            dy,
                                            incy,
                                            stride_y,
                                            dparam,
                                            stride_param,
                                            batch_count);
        }
        gpu_time_used = (get_time_us_sync(stream) - gpu_time_used);

        ArgumentModel<e_N, e_incx, e_incy, e_stride_x, e_stride_y, e_batch_count>{}.log_args<T>(
            rocblas_cout,
            arg,
            gpu_time_used,
            rotm_gflop_count<T>(N, (hparam + 0 * stride_param)[0]),
            rotm_gbyte_count<T>(N, (hparam + 0 * stride_param)[0]),
            cpu_time_used,
            norm_error_device_x,
            norm_error_device_y);
    }
}
