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

#include "bytes.hpp"
#include "cblas_interface.hpp"
#include "flops.hpp"
#include "near.hpp"
#include "norm.hpp"
#include "rocblas.hpp"
#include "rocblas_init.hpp"
#include "rocblas_math.hpp"
#include "rocblas_matrix.hpp"
#include "rocblas_random.hpp"
#include "rocblas_test.hpp"
#include "rocblas_vector.hpp"
#include "unit.hpp"
#include "utility.hpp"

template <typename T>
void testing_hpr_batched_bad_arg(const Arguments& arg)
{
    auto rocblas_hpr_batched_fn
        = arg.api == FORTRAN ? rocblas_hpr_batched<T, true> : rocblas_hpr_batched<T, false>;

    for(auto pointer_mode : {rocblas_pointer_mode_host, rocblas_pointer_mode_device})
    {
        rocblas_local_handle handle{arg};
        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, pointer_mode));

        rocblas_fill uplo        = rocblas_fill_upper;
        rocblas_int  N           = 100;
        rocblas_int  incx        = 1;
        rocblas_int  batch_count = 2;

        using U = real_t<T>;

        device_vector<U> alpha_d(1), zero_d(1);

        const U alpha_h(1), zero_h(0);

        const U* alpha = &alpha_h;
        const U* zero  = &zero_h;

        if(pointer_mode == rocblas_pointer_mode_device)
        {
            CHECK_HIP_ERROR(hipMemcpy(alpha_d, alpha, sizeof(*alpha), hipMemcpyHostToDevice));
            alpha = alpha_d;
            CHECK_HIP_ERROR(hipMemcpy(zero_d, zero, sizeof(*zero), hipMemcpyHostToDevice));
            zero = zero_d;
        }

        // Allocate device memory
        device_batch_vector<T> dx(N, incx, batch_count);
        device_batch_matrix<T> dAp_1(1, rocblas_packed_matrix_size(N), 1, batch_count);

        // Check device memory allocation
        CHECK_DEVICE_ALLOCATION(dx.memcheck());
        CHECK_DEVICE_ALLOCATION(dAp_1.memcheck());

        EXPECT_ROCBLAS_STATUS(
            rocblas_hpr_batched_fn(nullptr, uplo, N, alpha, dx, incx, dAp_1, batch_count),
            rocblas_status_invalid_handle);

        EXPECT_ROCBLAS_STATUS(
            rocblas_hpr_batched_fn(
                handle, rocblas_fill_full, N, alpha, dx, incx, dAp_1, batch_count),
            rocblas_status_invalid_value);

        EXPECT_ROCBLAS_STATUS(
            rocblas_hpr_batched_fn(handle, uplo, N, nullptr, dx, incx, dAp_1, batch_count),
            rocblas_status_invalid_pointer);

        if(pointer_mode == rocblas_pointer_mode_host)
        {
            EXPECT_ROCBLAS_STATUS(
                rocblas_hpr_batched_fn(handle, uplo, N, alpha, nullptr, incx, dAp_1, batch_count),
                rocblas_status_invalid_pointer);

            EXPECT_ROCBLAS_STATUS(
                rocblas_hpr_batched_fn(handle, uplo, N, alpha, dx, incx, nullptr, batch_count),
                rocblas_status_invalid_pointer);
        }

        // N==0 all pointers may be null
        EXPECT_ROCBLAS_STATUS(
            rocblas_hpr_batched_fn(handle, uplo, 0, nullptr, nullptr, incx, nullptr, batch_count),
            rocblas_status_success);

        // alpha==0 all pointers may be null
        EXPECT_ROCBLAS_STATUS(
            rocblas_hpr_batched_fn(handle, uplo, 0, zero, nullptr, incx, nullptr, batch_count),
            rocblas_status_success);

        // batch_count==0 all pointers may be null
        EXPECT_ROCBLAS_STATUS(
            rocblas_hpr_batched_fn(handle, uplo, N, nullptr, nullptr, incx, nullptr, 0),
            rocblas_status_success);
    }
}

template <typename T>
void testing_hpr_batched(const Arguments& arg)
{
    auto rocblas_hpr_batched_fn
        = arg.api == FORTRAN ? rocblas_hpr_batched<T, true> : rocblas_hpr_batched<T, false>;

    rocblas_int  N           = arg.N;
    rocblas_int  incx        = arg.incx;
    real_t<T>    h_alpha     = arg.get_alpha<real_t<T>>();
    rocblas_fill uplo        = char2rocblas_fill(arg.uplo);
    rocblas_int  batch_count = arg.batch_count;

    rocblas_local_handle handle{arg};

    // argument check before allocating invalid memory
    bool invalid_size = N < 0 || !incx || batch_count < 0;
    if(invalid_size || !N || !batch_count)
    {
        EXPECT_ROCBLAS_STATUS(
            rocblas_hpr_batched_fn(handle, uplo, N, nullptr, nullptr, incx, nullptr, batch_count),
            invalid_size ? rocblas_status_invalid_size : rocblas_status_success);
        return;
    }

    size_t size_A = rocblas_packed_matrix_size(N);

    // Naming: `h` is in CPU (host) memory(eg hAp_1), `d` is in GPU (device) memory (eg dAp_1).
    // Allocate host memory
    host_batch_matrix<T>   hA(N, N, N, batch_count);
    host_batch_matrix<T>   hAp_1(1, size_A, 1, batch_count);
    host_batch_matrix<T>   hAp_2(1, size_A, 1, batch_count);
    host_batch_matrix<T>   hAp_gold(1, size_A, 1, batch_count);
    host_batch_vector<T>   hx(N, incx, batch_count);
    host_vector<real_t<T>> halpha(1);

    // Check host memory allocation
    CHECK_HIP_ERROR(hA.memcheck());
    CHECK_HIP_ERROR(hAp_1.memcheck());
    CHECK_HIP_ERROR(hAp_2.memcheck());
    CHECK_HIP_ERROR(hAp_gold.memcheck());
    CHECK_HIP_ERROR(hx.memcheck());

    halpha[0] = h_alpha;

    // Allocate device memory
    device_batch_matrix<T>   dAp_1(1, size_A, 1, batch_count);
    device_batch_matrix<T>   dAp_2(1, size_A, 1, batch_count);
    device_batch_vector<T>   dx(N, incx, batch_count);
    device_vector<real_t<T>> d_alpha(1);

    // Check device memory allocation
    CHECK_DEVICE_ALLOCATION(dAp_1.memcheck());
    CHECK_DEVICE_ALLOCATION(dAp_2.memcheck());
    CHECK_DEVICE_ALLOCATION(dx.memcheck());
    CHECK_DEVICE_ALLOCATION(d_alpha.memcheck());

    // Initialize data on host memory
    rocblas_init_matrix(
        hA, arg, rocblas_client_never_set_nan, rocblas_client_hermitian_matrix, true);
    rocblas_init_vector(hx, arg, rocblas_client_alpha_sets_nan, false, true);

    //regular to packed matrix conversion
    regular_to_packed(uplo == rocblas_fill_upper, hA, hAp_1, N);

    hAp_2.copy_from(hAp_1);
    hAp_gold.copy_from(hAp_1);

    // copy data from CPU to device
    CHECK_HIP_ERROR(dAp_1.transfer_from(hAp_1));
    CHECK_HIP_ERROR(dx.transfer_from(hx));

    double gpu_time_used, cpu_time_used;
    double rocblas_error_1;
    double rocblas_error_2;

    if(arg.unit_check || arg.norm_check)
    {
        // copy data from CPU to device
        CHECK_HIP_ERROR(dAp_2.transfer_from(hAp_1));
        CHECK_HIP_ERROR(d_alpha.transfer_from(halpha));

        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_host));
        handle.pre_test(arg);
        CHECK_ROCBLAS_ERROR(rocblas_hpr_batched_fn(handle,
                                                   uplo,
                                                   N,
                                                   &h_alpha,
                                                   dx.ptr_on_device(),
                                                   incx,
                                                   dAp_1.ptr_on_device(),
                                                   batch_count));
        handle.post_test(arg);

        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_device));
        handle.pre_test(arg);
        CHECK_ROCBLAS_ERROR(rocblas_hpr_batched_fn(handle,
                                                   uplo,
                                                   N,
                                                   d_alpha,
                                                   dx.ptr_on_device(),
                                                   incx,
                                                   dAp_2.ptr_on_device(),
                                                   batch_count));
        handle.post_test(arg);

        // CPU BLAS
        cpu_time_used = get_time_us_no_sync();
        for(int i = 0; i < batch_count; i++)
        {
            cblas_hpr<T>(uplo, N, h_alpha, hx[i], incx, hAp_gold[i]);
        }
        cpu_time_used = get_time_us_no_sync() - cpu_time_used;

        // copy output from device to CPU
        CHECK_HIP_ERROR(hAp_1.transfer_from(dAp_1));
        CHECK_HIP_ERROR(hAp_2.transfer_from(dAp_2));

        if(arg.unit_check)
        {
            const double tol = N * sum_error_tolerance<T>;
            near_check_general<T>(1, size_A, 1, hAp_gold, hAp_1, batch_count, tol);
            near_check_general<T>(1, size_A, 1, hAp_gold, hAp_2, batch_count, tol);
        }

        if(arg.norm_check)
        {
            rocblas_error_1
                = norm_check_general<T>('F', 1, size_A, 1, hAp_gold, hAp_1, batch_count);
            rocblas_error_2
                = norm_check_general<T>('F', 1, size_A, 1, hAp_gold, hAp_2, batch_count);
        }
    }

    if(arg.timing)
    {
        int number_cold_calls = arg.cold_iters;
        int number_hot_calls  = arg.iters;
        CHECK_ROCBLAS_ERROR(rocblas_set_pointer_mode(handle, rocblas_pointer_mode_host));

        for(int iter = 0; iter < number_cold_calls; iter++)
        {
            rocblas_hpr_batched_fn(handle,
                                   uplo,
                                   N,
                                   &h_alpha,
                                   dx.ptr_on_device(),
                                   incx,
                                   dAp_1.ptr_on_device(),
                                   batch_count);
        }

        hipStream_t stream;
        CHECK_ROCBLAS_ERROR(rocblas_get_stream(handle, &stream));
        gpu_time_used = get_time_us_sync(stream); // in microseconds

        for(int iter = 0; iter < number_hot_calls; iter++)
        {
            rocblas_hpr_batched_fn(handle,
                                   uplo,
                                   N,
                                   &h_alpha,
                                   dx.ptr_on_device(),
                                   incx,
                                   dAp_1.ptr_on_device(),
                                   batch_count);
        }

        gpu_time_used = get_time_us_sync(stream) - gpu_time_used;

        ArgumentModel<e_uplo, e_N, e_alpha, e_incx, e_batch_count>{}.log_args<T>(
            rocblas_cout,
            arg,
            gpu_time_used,
            hpr_gflop_count<T>(N),
            hpr_gbyte_count<T>(N),
            cpu_time_used,
            rocblas_error_1,
            rocblas_error_2);
    }
}
