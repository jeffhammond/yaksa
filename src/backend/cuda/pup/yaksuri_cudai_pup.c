/*
* Copyright (C) by Argonne National Laboratory
*     See COPYRIGHT in top-level directory
*/

#include <assert.h>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include "yaksi.h"
#include "yaksuri_cudai.h"
#include <stdlib.h>

#define THREAD_BLOCK_SIZE  (256)
#define MAX_GRIDSZ_X       ((1U << 31) - 1)
#define MAX_GRIDSZ_Y       (65535U)
#define MAX_GRIDSZ_Z       (65535U)

#define MAX_IOV_LENGTH (8192)

static int get_thread_block_dims(uintptr_t count, yaksi_type_s * type, unsigned int *n_threads,
                                 unsigned int *n_blocks_x, unsigned int *n_blocks_y,
                                 unsigned int *n_blocks_z)
{
    int rc = YAKSA_SUCCESS;
    yaksuri_cudai_type_s *cuda_type = (yaksuri_cudai_type_s *) type->backend.cuda.priv;

    *n_threads = THREAD_BLOCK_SIZE;
    uintptr_t n_blocks = count * cuda_type->num_elements / THREAD_BLOCK_SIZE;
    n_blocks += ! !(count * cuda_type->num_elements % THREAD_BLOCK_SIZE);

    if (n_blocks <= MAX_GRIDSZ_X) {
        *n_blocks_x = (unsigned int) n_blocks;
        *n_blocks_y = 1;
        *n_blocks_z = 1;
    } else if (n_blocks <= MAX_GRIDSZ_X * MAX_GRIDSZ_Y) {
        *n_blocks_x = (unsigned int) (YAKSU_CEIL(n_blocks, MAX_GRIDSZ_Y));
        *n_blocks_y = (unsigned int) (YAKSU_CEIL(n_blocks, (*n_blocks_x)));
        *n_blocks_z = 1;
    } else {
        uintptr_t n_blocks_xy = YAKSU_CEIL(n_blocks, MAX_GRIDSZ_Z);
        *n_blocks_x = (unsigned int) (YAKSU_CEIL(n_blocks_xy, MAX_GRIDSZ_Y));
        *n_blocks_y = (unsigned int) (YAKSU_CEIL(n_blocks_xy, (*n_blocks_x)));
        *n_blocks_z =
            (unsigned int) (YAKSU_CEIL(n_blocks, (uintptr_t) (*n_blocks_x) * (*n_blocks_y)));
    }

    return rc;
}

int yaksuri_cudai_pup_is_supported(yaksi_type_s * type, bool * is_supported)
{
    int rc = YAKSA_SUCCESS;
    yaksuri_cudai_type_s *cuda_type = (yaksuri_cudai_type_s *) type->backend.cuda.priv;

    if (type->is_contig || cuda_type->pack)
        *is_supported = true;
    else
        *is_supported = false;

    return rc;
}

uintptr_t yaksuri_cudai_get_iov_pack_threshold(yaksi_info_s * info)
{
    uintptr_t iov_pack_threshold = YAKSURI_CUDAI_INFO__DEFAULT_IOV_PUP_THRESHOLD;
    if (info) {
        yaksuri_cudai_info_s *cuda_info = (yaksuri_cudai_info_s *) info->backend.cuda.priv;
        iov_pack_threshold = cuda_info->iov_pack_threshold;
    }

    return iov_pack_threshold;
}

uintptr_t yaksuri_cudai_get_iov_unpack_threshold(yaksi_info_s * info)
{
    uintptr_t iov_unpack_threshold = YAKSURI_CUDAI_INFO__DEFAULT_IOV_PUP_THRESHOLD;
    if (info) {
        yaksuri_cudai_info_s *cuda_info = (yaksuri_cudai_info_s *) info->backend.cuda.priv;
        iov_unpack_threshold = cuda_info->iov_unpack_threshold;
    }

    return iov_unpack_threshold;
}

int yaksuri_cudai_ipack(const void *inbuf, void *outbuf, uintptr_t count, yaksi_type_s * type,
                        yaksi_info_s * info, yaksi_request_s * request, int target)
{
    int rc = YAKSA_SUCCESS;
    yaksuri_cudai_type_s *cuda_type = (yaksuri_cudai_type_s *) type->backend.cuda.priv;
    cudaError_t cerr;

    uintptr_t iov_pack_threshold = yaksuri_cudai_get_iov_pack_threshold(info);

    int cur_device;
    cerr = cudaGetDevice(&cur_device);
    YAKSURI_CUDAI_CUDA_ERR_CHKANDJUMP(cerr, rc, fn_fail);

    cerr = cudaSetDevice(target);
    YAKSURI_CUDAI_CUDA_ERR_CHKANDJUMP(cerr, rc, fn_fail);

    /* shortcut for contiguous types */
    if (type->is_contig) {
        /* cuda performance is optimized when we synchronize on the
         * source buffer's GPU */
        cerr = cudaMemcpyAsync(outbuf, inbuf, count * type->size, cudaMemcpyDefault,
                               yaksuri_cudai_global.stream[target]);
        YAKSURI_CUDAI_CUDA_ERR_CHKANDJUMP(cerr, rc, fn_fail);
    } else if (type->size / type->num_contig >= iov_pack_threshold) {
        struct iovec iov[MAX_IOV_LENGTH];
        char *dbuf = (char *) outbuf;
        uintptr_t offset = 0;

        while (offset < type->num_contig * count) {
            uintptr_t actual_iov_len;
            rc = yaksi_iov(inbuf, count, type, offset, iov, MAX_IOV_LENGTH, &actual_iov_len);
            YAKSU_ERR_CHECK(rc, fn_fail);

            for (uintptr_t i = 0; i < actual_iov_len; i++) {
                cudaMemcpyAsync(dbuf, iov[i].iov_base, iov[i].iov_len, cudaMemcpyDefault,
                                yaksuri_cudai_global.stream[target]);
                dbuf += iov[i].iov_len;
            }

            offset += actual_iov_len;
        }
    } else {
        rc = yaksuri_cudai_md_alloc(type);
        YAKSU_ERR_CHECK(rc, fn_fail);

        unsigned int n_threads;
        unsigned int n_blocks_x, n_blocks_y, n_blocks_z;
        rc = get_thread_block_dims(count, type, &n_threads, &n_blocks_x, &n_blocks_y, &n_blocks_z);
        YAKSU_ERR_CHECK(rc, fn_fail);

        if ((request->backend.inattr.type == YAKSUR_PTR_TYPE__MANAGED &&
             request->backend.outattr.type == YAKSUR_PTR_TYPE__MANAGED) ||
            (request->backend.inattr.type == YAKSUR_PTR_TYPE__GPU &&
             request->backend.outattr.type == YAKSUR_PTR_TYPE__MANAGED) ||
            (request->backend.inattr.type == YAKSUR_PTR_TYPE__GPU &&
             request->backend.outattr.type == YAKSUR_PTR_TYPE__GPU &&
             request->backend.inattr.device == request->backend.outattr.device)) {
            cuda_type->pack(inbuf, outbuf, count, cuda_type->md, n_threads, n_blocks_x, n_blocks_y,
                            n_blocks_z, target);
        } else if (request->backend.inattr.type == YAKSUR_PTR_TYPE__MANAGED &&
                   request->backend.outattr.type == YAKSUR_PTR_TYPE__GPU) {
            cuda_type->pack(inbuf, outbuf, count, cuda_type->md, n_threads, n_blocks_x, n_blocks_y,
                            n_blocks_z, target);
        } else {
            rc = YAKSA_ERR__INTERNAL;
            goto fn_fail;
        }
    }

    cerr = cudaSetDevice(cur_device);
    YAKSURI_CUDAI_CUDA_ERR_CHKANDJUMP(cerr, rc, fn_fail);

  fn_exit:
    return rc;
  fn_fail:
    goto fn_exit;
}

int yaksuri_cudai_iunpack(const void *inbuf, void *outbuf, uintptr_t count, yaksi_type_s * type,
                          yaksi_info_s * info, yaksi_request_s * request, int target)
{
    int rc = YAKSA_SUCCESS;
    yaksuri_cudai_type_s *cuda_type = (yaksuri_cudai_type_s *) type->backend.cuda.priv;
    cudaError_t cerr;

    uintptr_t iov_unpack_threshold = yaksuri_cudai_get_iov_unpack_threshold(info);

    int cur_device;
    cerr = cudaGetDevice(&cur_device);
    YAKSURI_CUDAI_CUDA_ERR_CHKANDJUMP(cerr, rc, fn_fail);

    cerr = cudaSetDevice(target);
    YAKSURI_CUDAI_CUDA_ERR_CHKANDJUMP(cerr, rc, fn_fail);

    /* shortcut for contiguous types */
    if (type->is_contig) {
        /* cuda performance is optimized when we synchronize on the
         * source buffer's GPU */
        cerr = cudaMemcpyAsync(outbuf, inbuf, count * type->size, cudaMemcpyDefault,
                               yaksuri_cudai_global.stream[target]);
        YAKSURI_CUDAI_CUDA_ERR_CHKANDJUMP(cerr, rc, fn_fail);
    } else if (type->size / type->num_contig >= iov_unpack_threshold) {
        struct iovec iov[MAX_IOV_LENGTH];
        const char *sbuf = (const char *) inbuf;
        uintptr_t offset = 0;

        while (offset < type->num_contig * count) {
            uintptr_t actual_iov_len;
            rc = yaksi_iov(outbuf, count, type, offset, iov, MAX_IOV_LENGTH, &actual_iov_len);
            YAKSU_ERR_CHECK(rc, fn_fail);

            for (uintptr_t i = 0; i < actual_iov_len; i++) {
                cudaMemcpyAsync(iov[i].iov_base, sbuf, iov[i].iov_len, cudaMemcpyDefault,
                                yaksuri_cudai_global.stream[target]);
                sbuf += iov[i].iov_len;
            }

            offset += actual_iov_len;
        }
    } else {
        rc = yaksuri_cudai_md_alloc(type);
        YAKSU_ERR_CHECK(rc, fn_fail);

        unsigned int n_threads;
        unsigned int n_blocks_x, n_blocks_y, n_blocks_z;
        rc = get_thread_block_dims(count, type, &n_threads, &n_blocks_x, &n_blocks_y, &n_blocks_z);
        YAKSU_ERR_CHECK(rc, fn_fail);

        if ((request->backend.inattr.type == YAKSUR_PTR_TYPE__MANAGED &&
             request->backend.outattr.type == YAKSUR_PTR_TYPE__MANAGED) ||
            (request->backend.inattr.type == YAKSUR_PTR_TYPE__MANAGED &&
             request->backend.outattr.type == YAKSUR_PTR_TYPE__GPU) ||
            (request->backend.inattr.type == YAKSUR_PTR_TYPE__GPU &&
             request->backend.outattr.type == YAKSUR_PTR_TYPE__GPU &&
             request->backend.inattr.device == request->backend.outattr.device)) {
            cuda_type->unpack(inbuf, outbuf, count, cuda_type->md, n_threads, n_blocks_x,
                              n_blocks_y, n_blocks_z, target);
        } else if (request->backend.inattr.type == YAKSUR_PTR_TYPE__GPU &&
                   request->backend.outattr.type == YAKSUR_PTR_TYPE__MANAGED) {
            cuda_type->unpack(inbuf, outbuf, count, cuda_type->md, n_threads, n_blocks_x,
                              n_blocks_y, n_blocks_z, target);
        } else {
            rc = YAKSA_ERR__INTERNAL;
            goto fn_fail;
        }
    }

    cerr = cudaSetDevice(cur_device);
    YAKSURI_CUDAI_CUDA_ERR_CHKANDJUMP(cerr, rc, fn_fail);

  fn_exit:
    return rc;
  fn_fail:
    goto fn_exit;
}
