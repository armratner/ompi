/*
 *  Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                          University Research and Technology
 *                          Corporation.  All rights reserved.
 *  Copyright (c) 2004-2016 The University of Tennessee and The University
 *                          of Tennessee Research Foundation.  All rights
 *                          reserved.
 *  Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                          University of Stuttgart.  All rights reserved.
 *  Copyright (c) 2004-2005 The Regents of the University of California.
 *                          All rights reserved.
 *  Copyright (c) 2008-2019 University of Houston. All rights reserved.
 *  Copyright (c) 2022      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 *  Copyright (c) 2025      Advanced Micro Devices, Inc. All rights reserved.
 *  $COPYRIGHT$
 *
 *  Additional copyrights may follow
 *
 *  $HEADER$
 */

#include "ompi_config.h"

#include "opal/datatype/opal_convertor.h"
#include "opal/mca/accelerator/accelerator.h"
#include "opal/util/sys_limits.h"

#include "opal/mca/allocator/allocator.h"
#include "opal/mca/allocator/base/base.h"
#include "common_ompio.h"
#include "common_ompio_buffer.h"


static opal_mutex_t     mca_common_ompio_buffer_mutex;      /* lock for thread safety */
static mca_allocator_base_component_t* mca_common_ompio_allocator_component=NULL;
static mca_allocator_base_module_t* mca_common_ompio_allocator=NULL;  

static opal_atomic_int32_t  mca_common_ompio_buffer_init = 0;
static int32_t  mca_common_ompio_pagesize=4096;
static void* mca_common_ompio_buffer_alloc_seg ( void *ctx, size_t *size );
static void mca_common_ompio_buffer_free_seg ( void *ctx, void *buf );

void mca_common_ompio_check_gpu_buf ( ompio_file_t *fh, const void *buf, int *is_gpu, 
				      int *is_managed)
{
    uint64_t flags = 0;
    int dev_id;

    *is_gpu=0;
    *is_managed=0;

    if (fh->f_fh->f_flags & OMPI_FILE_ASSERT_NO_ACCEL_BUF) {
        return;
    }

    if (0 < opal_accelerator.check_addr(buf, &dev_id, &flags)) {
        *is_gpu = 1;
        if (flags & MCA_ACCELERATOR_FLAGS_UNIFIED_MEMORY) {
            *is_managed = 1;
        }
    }

    return;
}

static void* mca_common_ompio_buffer_alloc_seg ( void*ctx, size_t *size )
{
    char *buf=NULL;
    size_t realsize, numpages;

    numpages = (*size + mca_common_ompio_pagesize -1 )/mca_common_ompio_pagesize;
    realsize = numpages * mca_common_ompio_pagesize;

    buf = malloc (realsize);

    if (NULL != buf) {
        opal_accelerator.host_register(MCA_ACCELERATOR_NO_DEVICE_ID, (void *)buf, realsize);
    }

    *size = realsize;
    return buf;
}

static void mca_common_ompio_buffer_free_seg ( void *ctx, void *buf )
{
    uint64_t flags = 0;
    int dev_id;

    if ( NULL != buf ) {
        if (0 == opal_accelerator.check_addr(buf, &dev_id, &flags)) {
            opal_accelerator.host_unregister(dev_id, (void *)buf);
        }
        free ( buf );
    }
    return;
}

int mca_common_ompio_buffer_alloc_init ( void )
{
    bool thread_safe=true;

    if(OPAL_THREAD_ADD_FETCH32(&mca_common_ompio_buffer_init, 1) > 1)
        return OMPI_SUCCESS;

    /* initialize static objects */
    OBJ_CONSTRUCT(&mca_common_ompio_buffer_mutex, opal_mutex_t);

    OPAL_THREAD_LOCK (&mca_common_ompio_buffer_mutex );
    /* lookup name of the allocator to use */
    if(NULL == (mca_common_ompio_allocator_component = mca_allocator_component_lookup("basic"))) {
        OPAL_THREAD_UNLOCK(&mca_common_ompio_buffer_mutex);
        return OMPI_ERR_BUFFER;
    }

    /* create an instance of the allocator */
    mca_common_ompio_allocator = mca_common_ompio_allocator_component->allocator_init(thread_safe, 
                                                                                      mca_common_ompio_buffer_alloc_seg, 
                                                                                      mca_common_ompio_buffer_free_seg, 
                                                                                      NULL);
    if(NULL == mca_common_ompio_allocator) {
        OPAL_THREAD_UNLOCK(&mca_common_ompio_buffer_mutex);
        return OMPI_ERR_BUFFER;
    }

    mca_common_ompio_pagesize = opal_getpagesize();

    OPAL_THREAD_UNLOCK(&mca_common_ompio_buffer_mutex);
    return OMPI_SUCCESS;
}

int mca_common_ompio_buffer_alloc_fini ( void )
{
    if ( NULL != mca_common_ompio_allocator ) {
        OPAL_THREAD_LOCK (&mca_common_ompio_buffer_mutex);
        mca_common_ompio_allocator->alc_finalize(mca_common_ompio_allocator);
        mca_common_ompio_allocator=NULL;
        OPAL_THREAD_UNLOCK (&mca_common_ompio_buffer_mutex);
        OBJ_DESTRUCT (&mca_common_ompio_buffer_mutex);
    }

    return OMPI_SUCCESS;
}

void *mca_common_ompio_alloc_buf ( ompio_file_t *fh, size_t bufsize )
{
    char *tmp=NULL;

    if ( !mca_common_ompio_buffer_init ){
        mca_common_ompio_buffer_alloc_init ();
    }
    
    OPAL_THREAD_LOCK (&mca_common_ompio_buffer_mutex);
    tmp = mca_common_ompio_allocator->alc_alloc (mca_common_ompio_allocator,
                                                 bufsize, 0 );
    OPAL_THREAD_UNLOCK (&mca_common_ompio_buffer_mutex);
    return tmp;
}

void mca_common_ompio_release_buf ( ompio_file_t *fh, void *buf )
{

    if ( !mca_common_ompio_buffer_init ){
        /* Should not happen. You can not release a buf without
        ** having it allocated first. 
        */
        opal_output (1, "error in mca_common_ompio_release_buf: allocator not initialized\n");
    }

    OPAL_THREAD_LOCK (&mca_common_ompio_buffer_mutex);
    mca_common_ompio_allocator->alc_free (mca_common_ompio_allocator,
                                          buf);
    OPAL_THREAD_UNLOCK (&mca_common_ompio_buffer_mutex);

    return;
}

