/*
 * Copyright (c) 2008, 2009, 2010, 2011 Kernel Drivers, LLC.
 * Copyright (c) 2009, 2010, 2011 Your File System, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice,
 *   this list of conditions and the following disclaimer in the
 *   documentation
 *   and/or other materials provided with the distribution.
 * - Neither the names of Kernel Drivers, LLC and Your File System, Inc.
 *   nor the names of their contributors may be used to endorse or promote
 *   products derived from this software without specific prior written
 *   permission from Kernel Drivers, LLC and Your File System, Inc.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//
// File: AFSRead.cpp
//

#include "AFSCommon.h"

static
NTSTATUS
AFSCachedRead( IN PDEVICE_OBJECT DeviceObject,
               IN PIRP Irp,
               IN LARGE_INTEGER StartingByte,
               IN ULONG ByteCount)
{
    NTSTATUS           ntStatus = STATUS_SUCCESS;
    IO_STACK_LOCATION *pIrpSp = IoGetCurrentIrpStackLocation( Irp);
    PFILE_OBJECT       pFileObject = pIrpSp->FileObject;
    AFSFcb            *pFcb = (AFSFcb *)pFileObject->FsContext;
    AFSCcb            *pCcb = (AFSCcb *)pFileObject->FsContext2;
    BOOLEAN            bSynchronousIo = BooleanFlagOn( pFileObject->Flags, FO_SYNCHRONOUS_IO);
    VOID              *pSystemBuffer = NULL;
    ULONG              ulCurrentIO = 0, ulTotalLen = ByteCount;
    PMDL               pCurrentMdl = Irp->MdlAddress;
    LARGE_INTEGER      liCurrentOffset;

    __Enter
    {

        Irp->IoStatus.Information = 0;

        liCurrentOffset.QuadPart = StartingByte.QuadPart;

        while( ulTotalLen > 0)
        {

            ntStatus = STATUS_SUCCESS;

            if( pCurrentMdl != NULL)
            {

                ASSERT( pCurrentMdl != NULL);

                pSystemBuffer = MmGetSystemAddressForMdlSafe( pCurrentMdl,
                                                              NormalPagePriority);

                ulCurrentIO = MmGetMdlByteCount( pCurrentMdl);

                if( ulCurrentIO > ulTotalLen)
                {
                    ulCurrentIO = ulTotalLen;
                }
            }
            else
            {

                pSystemBuffer = AFSLockSystemBuffer( Irp,
                                                     ulTotalLen);

                ulCurrentIO = ulTotalLen;
            }

            if( pSystemBuffer == NULL)
            {

                AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                              AFS_TRACE_LEVEL_ERROR,
                              "AFSCachedRead (%08lX) Failed to lock system buffer\n",
                              Irp);

                try_return( ntStatus = STATUS_INSUFFICIENT_RESOURCES);
            }

            __try
            {

                if( !CcCopyRead( pFileObject,
                                 &liCurrentOffset,
                                 ulCurrentIO,
                                 TRUE,
                                 pSystemBuffer,
                                 &Irp->IoStatus))
                {

                    //
                    // Failed to process request.
                    //

                    AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                                  AFS_TRACE_LEVEL_ERROR,
                                  "AFSCachedRead (%08lX) Failed CcCopyRead() %wZ @ %0I64X Status %08lX\n",
                                  Irp,
                                  &pFileObject->FileName,
                                  liCurrentOffset.QuadPart,
                                  Irp->IoStatus.Status);

                    try_return( ntStatus = Irp->IoStatus.Status);
                }
            }
            __except( EXCEPTION_EXECUTE_HANDLER)
            {
                ntStatus = GetExceptionCode();

                AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                              AFS_TRACE_LEVEL_ERROR,
                              "AFSCachedRead (%08lX) CcCopyRead() Threw exception %wZ @ %0I64X Status %08lX\n",
                              Irp,
                              &pFileObject->FileName,
                              liCurrentOffset.QuadPart,
                              ntStatus);
            }

            if( !NT_SUCCESS( ntStatus))
            {

                try_return( ntStatus);
            }

            if( ulTotalLen <= ulCurrentIO)
            {
                break;
            }

            liCurrentOffset.QuadPart += ulCurrentIO;

            ulTotalLen -= ulCurrentIO;

            pCurrentMdl = pCurrentMdl->Next;
        }

try_exit:

        if( NT_SUCCESS( ntStatus))
        {

            Irp->IoStatus.Information = ByteCount;

            //
            // Update the CBO if this is a sync read
            //

            if( bSynchronousIo)
            {

                pFileObject->CurrentByteOffset.QuadPart = StartingByte.QuadPart + ByteCount;
            }
        }

        AFSCompleteRequest( Irp,
                            ntStatus);
    }

    return ntStatus;
}

static
NTSTATUS
AFSNonCachedRead( IN PDEVICE_OBJECT DeviceObject,
                  IN PIRP Irp,
                  IN LARGE_INTEGER StartingByte)
{
    NTSTATUS           ntStatus = STATUS_UNSUCCESSFUL;
    IO_STACK_LOCATION *pIrpSp = IoGetCurrentIrpStackLocation( Irp);
    PFILE_OBJECT       pFileObject = pIrpSp->FileObject;
    AFSFcb            *pFcb = (AFSFcb *)pFileObject->FsContext;
    AFSCcb            *pCcb = (AFSCcb *)pFileObject->FsContext2;
    BOOLEAN            bSynchronousIo = IoIsOperationSynchronous(Irp);
    VOID              *pSystemBuffer = NULL;
    BOOLEAN            bPagingIo = BooleanFlagOn( Irp->Flags, IRP_PAGING_IO);
    BOOLEAN            bLocked = FALSE;
    AFSGatherIo       *pGatherIo = NULL;
    AFSIoRun          *pIoRuns = NULL;
    AFSIoRun           stIoRuns[AFS_MAX_STACK_IO_RUNS];
    ULONG              extentsCount = 0, runCount = 0;
    AFSExtent         *pStartExtent = NULL;
    AFSExtent         *pIgnoreExtent = NULL;
    BOOLEAN            bExtentsMapped = FALSE;
    BOOLEAN            bCompleteIrp = TRUE;
    ULONG              ulReadByteCount;
    ULONG              ulByteCount;
    AFSDeviceExt      *pDevExt = (AFSDeviceExt *)DeviceObject->DeviceExtension;
    ULONG              ulRequestCount = 0;
    LARGE_INTEGER      liCurrentTime, liLastRequestTime;
    AFSDeviceExt      *pControlDevExt = (AFSDeviceExt *)AFSControlDeviceObject->DeviceExtension;
    PFILE_OBJECT       pCacheFileObject = NULL;

    __Enter
    {

        ulByteCount = pIrpSp->Parameters.Read.Length;

        if (ulByteCount > pDevExt->Specific.RDR.MaxIo.QuadPart)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSNonCachedRead (%08lX) Request larger than MaxIO %I64X\n",
                          Irp,
                          pDevExt->Specific.RDR.MaxIo.QuadPart);

            try_return( ntStatus = STATUS_UNSUCCESSFUL);
        }

        pSystemBuffer = AFSLockSystemBuffer( Irp,
                                             ulByteCount);

        if( pSystemBuffer == NULL)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSNonCachedRead (%08lX) Failed to map system buffer\n",
                          Irp);

            try_return( ntStatus = STATUS_INSUFFICIENT_RESOURCES);
        }

        if( StartingByte.QuadPart + ulByteCount > pFcb->Header.FileSize.QuadPart)
        {
            ULONG zeroCount = (ULONG) (StartingByte.QuadPart + ulByteCount - pFcb->Header.FileSize.QuadPart);
            ulReadByteCount = (ULONG)(pFcb->Header.FileSize.QuadPart - StartingByte.QuadPart);

            //
            // Clear up to EOF
            //

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSNonCachedRead (%08lX) Zeroing to EOF zero byte length %08lX\n",
                          Irp,
                          zeroCount);

            RtlZeroMemory( ((PCHAR)pSystemBuffer) + ulReadByteCount, zeroCount);
        }
        else
        {
            ulReadByteCount = ulByteCount;
        }

        //
        // Provoke a get of the extents - if we need to.
        //

        pStartExtent = NULL;

        AFSDbgLogMsg( AFS_SUBSYSTEM_EXTENT_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSNonCachedRead Requesting extents for fid %08lX-%08lX-%08lX-%08lX Offset %I64X Length %08lX\n",
                      pFcb->ObjectInformation->FileId.Cell,
                      pFcb->ObjectInformation->FileId.Volume,
                      pFcb->ObjectInformation->FileId.Vnode,
                      pFcb->ObjectInformation->FileId.Unique,
                      StartingByte.QuadPart,
                      ulReadByteCount);

        ntStatus = AFSRequestExtentsAsync( pFcb,
                                           pCcb,
                                           &StartingByte,
                                           ulReadByteCount);

        if (!NT_SUCCESS(ntStatus))
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_EXTENT_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSNonCachedRead (%08lX) Failed to request extents Status %08lX\n",
                          Irp,
                          ntStatus);

            try_return( ntStatus);
        }

        KeQueryTickCount( &liLastRequestTime);

        while (TRUE)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_LOCK_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSNonCachedRead Acquiring Fcb extents lock %08lX SHARED %08lX\n",
                          &pFcb->NPFcb->Specific.File.ExtentsResource,
                          PsGetCurrentThread());

            AFSAcquireShared( &pFcb->NPFcb->Specific.File.ExtentsResource, TRUE );
            bLocked = TRUE;

            pStartExtent = NULL;
            pIgnoreExtent = NULL;

            if( AFSDoExtentsMapRegion( pFcb,
                                       &StartingByte,
                                       ulReadByteCount,
                                       &pStartExtent,
                                       &pIgnoreExtent ))
            {
                break;
            }

            KeClearEvent( &pFcb->NPFcb->Specific.File.ExtentsRequestComplete);

            AFSDbgLogMsg( AFS_SUBSYSTEM_LOCK_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSNonCachedRead Releasing Fcb extents lock %08lX SHARED %08lX\n",
                          &pFcb->NPFcb->Specific.File.ExtentsResource,
                          PsGetCurrentThread());

            AFSReleaseResource( &pFcb->NPFcb->Specific.File.ExtentsResource );
            bLocked= FALSE;

            //
            // We will re-request the extents after 10 seconds of waiting for them
            //

            KeQueryTickCount( &liCurrentTime);

            if( liCurrentTime.QuadPart - liLastRequestTime.QuadPart >= pControlDevExt->Specific.Control.ExtentRequestTimeCount.QuadPart)
            {

                AFSDbgLogMsg( AFS_SUBSYSTEM_EXTENT_PROCESSING,
                              AFS_TRACE_LEVEL_VERBOSE,
                              "AFSNonCachedRead Requesting extents, again, for fid %08lX-%08lX-%08lX-%08lX Offset %I64X Length %08lX\n",
                              pFcb->ObjectInformation->FileId.Cell,
                              pFcb->ObjectInformation->FileId.Volume,
                              pFcb->ObjectInformation->FileId.Vnode,
                              pFcb->ObjectInformation->FileId.Unique,
                              StartingByte.QuadPart,
                              ulReadByteCount);

                ntStatus = AFSRequestExtentsAsync( pFcb,
                                                   pCcb,
                                                   &StartingByte,
                                                   ulReadByteCount);

                if (!NT_SUCCESS(ntStatus))
                {

                    AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                                  AFS_TRACE_LEVEL_ERROR,
                                  "AFSNonCachedRead (%08lX) Failed to request extents Status %08lX\n",
                                  Irp,
                                  ntStatus);

                    try_return( ntStatus);
                }

                KeQueryTickCount( &liLastRequestTime);
            }

            AFSDbgLogMsg( AFS_SUBSYSTEM_EXTENT_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSNonCachedRead Waiting for extents for fid %08lX-%08lX-%08lX-%08lX Offset %I64X Length %08lX\n",
                          pFcb->ObjectInformation->FileId.Cell,
                          pFcb->ObjectInformation->FileId.Volume,
                          pFcb->ObjectInformation->FileId.Vnode,
                          pFcb->ObjectInformation->FileId.Unique,
                          StartingByte.QuadPart,
                          ulReadByteCount);

            ntStatus =  AFSWaitForExtentMapping ( pFcb, pCcb);

            if (!NT_SUCCESS(ntStatus))
            {

                AFSDbgLogMsg( AFS_SUBSYSTEM_EXTENT_PROCESSING,
                              AFS_TRACE_LEVEL_ERROR,
                              "AFSNonCachedRead Failed wait for extents for fid %08lX-%08lX-%08lX-%08lX Offset %I64X Length %08lX Status %08lX\n",
                              pFcb->ObjectInformation->FileId.Cell,
                              pFcb->ObjectInformation->FileId.Volume,
                              pFcb->ObjectInformation->FileId.Vnode,
                              pFcb->ObjectInformation->FileId.Unique,
                              StartingByte.QuadPart,
                              ulReadByteCount,
                              ntStatus);

                try_return( ntStatus);
            }
        }

        //
        // At this stage we know that the extents are fully mapped and
        // that, because we took a reference they won't be unmapped.
        // Thus the list will not move between the start and end.
        //

        AFSDbgLogMsg( AFS_SUBSYSTEM_EXTENT_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSNonCachedRead Extents mapped for fid %08lX-%08lX-%08lX-%08lX Offset %I64X Length %08lX\n",
                      pFcb->ObjectInformation->FileId.Cell,
                      pFcb->ObjectInformation->FileId.Volume,
                      pFcb->ObjectInformation->FileId.Vnode,
                      pFcb->ObjectInformation->FileId.Unique,
                      StartingByte.QuadPart,
                      ulReadByteCount);

        ntStatus = AFSGetExtents( pFcb,
                                  &StartingByte,
                                  ulReadByteCount,
                                  pStartExtent,
                                  &extentsCount,
                                  &runCount);

        if (!NT_SUCCESS(ntStatus))
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_EXTENT_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSNonCachedRead (%08lX) Failed to retrieve mapped extents Status %08lX\n",
                          Irp,
                          ntStatus);

            try_return( ntStatus );
        }

        AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSNonCachedRead (%08lX) Successfully retrieved map extents count %08lX run count %08lX\n",
                      Irp,
                      extentsCount,
                      runCount);

        if( BooleanFlagOn( AFSLibControlFlags, AFS_REDIR_LIB_FLAGS_NONPERSISTENT_CACHE))
        {

            Irp->IoStatus.Information = ulReadByteCount;

            ntStatus = AFSProcessExtentRun( pSystemBuffer,
                                            &StartingByte,
                                            ulReadByteCount,
                                            pStartExtent,
                                            FALSE);

            if (!NT_SUCCESS(ntStatus))
            {

                AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                              AFS_TRACE_LEVEL_ERROR,
                              "AFSNonCachedRead (%08lX) Failed to process extent run for non-persistent cache Status %08lX\n",
                              Irp,
                              ntStatus);
            }

            try_return( ntStatus);
        }

        //
        // Retrieve the cache file object
        //

        pCacheFileObject = AFSReferenceCacheFileObject();

        if( pCacheFileObject == NULL)
        {

            ntStatus = STATUS_DEVICE_NOT_READY;

            AFSDbgLogMsg( AFS_SUBSYSTEM_EXTENT_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSNonCachedRead Failed to retrieve cache fileobject for fid %08lX-%08lX-%08lX-%08lX Offset %I64X Length %08lX Status %08lX\n",
                          pFcb->ObjectInformation->FileId.Cell,
                          pFcb->ObjectInformation->FileId.Volume,
                          pFcb->ObjectInformation->FileId.Vnode,
                          pFcb->ObjectInformation->FileId.Unique,
                          StartingByte.QuadPart,
                          ulReadByteCount,
                          ntStatus);

            try_return( ntStatus);
        }

        if (runCount > AFS_MAX_STACK_IO_RUNS)
        {

            pIoRuns = (AFSIoRun*) AFSExAllocatePoolWithTag( PagedPool,
                                                            runCount * sizeof( AFSIoRun ),
                                                            AFS_IO_RUN_TAG );
            if (NULL == pIoRuns)
            {

                AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                              AFS_TRACE_LEVEL_ERROR,
                              "AFSNonCachedRead (%08lX) Failed to allocate IO run block\n",
                              Irp);

                try_return( ntStatus = STATUS_INSUFFICIENT_RESOURCES );
            }
        }
        else
        {

            pIoRuns = stIoRuns;
        }

        RtlZeroMemory( pIoRuns, runCount * sizeof( AFSIoRun ));

        ntStatus = AFSSetupIoRun( IoGetRelatedDeviceObject( pCacheFileObject),
                                  Irp,
                                  pSystemBuffer,
                                  pIoRuns,
                                  &StartingByte,
                                  ulReadByteCount,
                                  pStartExtent,
                                  &runCount );

        if (!NT_SUCCESS(ntStatus))
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSNonCachedRead (%08lX) Failed to initialize IO run block Status %08lX\n",
                          Irp,
                          ntStatus);

            try_return( ntStatus );
        }

        AFSReferenceActiveExtents( pStartExtent,
                                   extentsCount);

        AFSDbgLogMsg( AFS_SUBSYSTEM_LOCK_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSNonCachedRead Releasing Fcb extents lock %08lX SHARED %08lX\n",
                      &pFcb->NPFcb->Specific.File.ExtentsResource,
                      PsGetCurrentThread());

        AFSReleaseResource( &pFcb->NPFcb->Specific.File.ExtentsResource );
        bLocked = FALSE;

        pGatherIo = (AFSGatherIo*) AFSExAllocatePoolWithTag( NonPagedPool,
                                                             sizeof( AFSGatherIo ),
                                                             AFS_GATHER_TAG );

        if (NULL == pGatherIo)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSNonCachedRead (%08lX) Failed to allocate IO gather block\n",
                          Irp);

            AFSDbgLogMsg( AFS_SUBSYSTEM_LOCK_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSNonCachedRead Acquiring(2) Fcb extents lock %08lX SHARED %08lX\n",
                          &pFcb->NPFcb->Specific.File.ExtentsResource,
                          PsGetCurrentThread());

            AFSAcquireShared( &pFcb->NPFcb->Specific.File.ExtentsResource,
                              TRUE);
            bLocked = TRUE;

            AFSDereferenceActiveExtents( pStartExtent,
                                         extentsCount);

            try_return (ntStatus = STATUS_INSUFFICIENT_RESOURCES );
        }

        RtlZeroMemory( pGatherIo, sizeof( AFSGatherIo ));

        //
        // Initialize count to 1, that was we won't get an early
        // completion if the first irp completes before the second is
        // queued.
        //
        pGatherIo->Count = 1;
        pGatherIo->Status = STATUS_SUCCESS;
        pGatherIo->MasterIrp = Irp;
        pGatherIo->Synchronous = TRUE;
        pGatherIo->Fcb = pFcb;
        pGatherIo->CompleteMasterIrp = FALSE;

        bCompleteIrp = TRUE;

        if (pGatherIo->Synchronous)
        {
            KeInitializeEvent( &pGatherIo->Event, NotificationEvent, FALSE );
        }
        else
        {
            IoMarkIrpPending( Irp);
        }

        //
        // Pre-emptively set up the count
        //

        Irp->IoStatus.Information = ulByteCount;

        ntStatus = AFSQueueStartIos( pCacheFileObject,
                                     IRP_MJ_READ,
                                     IRP_READ_OPERATION | IRP_SYNCHRONOUS_API,
                                     pIoRuns,
                                     runCount,
                                     pGatherIo);

        AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSNonCachedRead (%08lX) AFSStartIos completed Status %08lX\n",
                      Irp,
                      ntStatus);

        if( !NT_SUCCESS( ntStatus))
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_LOCK_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSNonCachedRead Acquiring(3) Fcb extents lock %08lX SHARED %08lX\n",
                          &pFcb->NPFcb->Specific.File.ExtentsResource,
                          PsGetCurrentThread());

            AFSAcquireShared( &pFcb->NPFcb->Specific.File.ExtentsResource,
                              TRUE);
            bLocked = TRUE;

            AFSDereferenceActiveExtents( pStartExtent,
                                         extentsCount);

            try_return( ntStatus);
        }

        //
        // Wait for completion of all IOs we started.
        //
        (VOID) KeWaitForSingleObject( &pGatherIo->Event,
                                      Executive,
                                      KernelMode,
                                      FALSE,
                                      NULL);

        ntStatus = pGatherIo->Status;

        AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSNonCachedRead (%08lX) AFSStartIos wait completed Status %08lX\n",
                      Irp,
                      ntStatus);

        AFSDbgLogMsg( AFS_SUBSYSTEM_LOCK_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSNonCachedRead Acquiring(4) Fcb extents lock %08lX SHARED %08lX\n",
                      &pFcb->NPFcb->Specific.File.ExtentsResource,
                      PsGetCurrentThread());

        AFSAcquireShared( &pFcb->NPFcb->Specific.File.ExtentsResource,
                          TRUE);
        bLocked = TRUE;

        AFSDereferenceActiveExtents( pStartExtent,
                                     extentsCount);

        AFSDbgLogMsg( AFS_SUBSYSTEM_LOCK_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSNonCachedRead Releasing Fcb extents lock %08lX SHARED %08lX\n",
                      &pFcb->NPFcb->Specific.File.ExtentsResource,
                      PsGetCurrentThread());

        AFSReleaseResource( &pFcb->NPFcb->Specific.File.ExtentsResource );
        bLocked = FALSE;

        //
        // The data is there now.  Give back the extents now so the service
        // has some in hand
        //

        if ( pFcb->Specific.File.ExtentLength > 4096)
        {

            (VOID) AFSReleaseExtentsWithFlush( pFcb,
                                               &pCcb->AuthGroup,
                                               FALSE);
        }

try_exit:

        if( pCacheFileObject != NULL)
        {
            AFSReleaseCacheFileObject( pCacheFileObject);
        }

        AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSNonCachedRead (%08lX) Completed request Status %08lX\n",
                      Irp,
                      ntStatus);

        if (NT_SUCCESS(ntStatus) &&
            !bPagingIo &&
            BooleanFlagOn( pFileObject->Flags, FO_SYNCHRONOUS_IO))
        {
            //
            // Update the CBO if this is a sync, nopaging read
            //
            pFileObject->CurrentByteOffset.QuadPart = StartingByte.QuadPart + ulByteCount;
        }

        if (bLocked)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_LOCK_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSNonCachedRead Releasing Fcb extents lock %08lX SHARED %08lX\n",
                          &pFcb->NPFcb->Specific.File.ExtentsResource,
                          PsGetCurrentThread());

            AFSReleaseResource( &pFcb->NPFcb->Specific.File.ExtentsResource );
        }

        if (pGatherIo)
        {
            AFSExFreePoolWithTag(pGatherIo, AFS_GATHER_TAG);
        }

        if (NULL != pIoRuns && stIoRuns != pIoRuns)
        {
            AFSExFreePoolWithTag(pIoRuns, AFS_IO_RUN_TAG);
        }

        if (bCompleteIrp)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSNonCachedRead Completing irp %08lX Status %08lX Info %08lX\n",
                          Irp,
                          ntStatus,
                          Irp->IoStatus.Information);

            AFSCompleteRequest( Irp, ntStatus );
        }
    }
    return ntStatus;
}
//
// Function: AFSDispatch
//
// Description:
//
// A shim around AFSCommonRead (qv)
//
NTSTATUS
AFSRead( IN PDEVICE_OBJECT LibDeviceObject,
         IN PIRP Irp)
{

    NTSTATUS ntStatus = STATUS_SUCCESS;

    __try
    {

        ntStatus = AFSCommonRead( AFSRDRDeviceObject, Irp, NULL);
    }
    __except( AFSExceptionFilter( __FUNCTION__, GetExceptionCode(), GetExceptionInformation()) )
    {

        ntStatus = STATUS_INSUFFICIENT_RESOURCES;

        AFSDumpTraceFilesFnc();
    }

    return ntStatus;
}
//
// Function: AFSRead
//
// Description:
//
//      This function is a slightly widened Dispatch handler for the
//      AFS Read function.  The third parameter is NULL if we were called
//      at our dispatch point and a process handle if we have been posted.
//
//      After doing the obvious (completing MDL writes and so forth)
//      we then post, or not.
//
// Return:
//
//      A status is returned for the function
//

NTSTATUS
AFSCommonRead( IN PDEVICE_OBJECT DeviceObject,
               IN PIRP Irp,
               IN HANDLE OnBehalfOf)
{

    NTSTATUS            ntStatus = STATUS_SUCCESS;
    AFSDeviceExt       *pDeviceExt;
    IO_STACK_LOCATION  *pIrpSp;
    AFSFcb             *pFcb = NULL;
    AFSCcb             *pCcb = NULL;
    BOOLEAN             bReleaseMain = FALSE;
    BOOLEAN             bReleasePaging = FALSE;
    BOOLEAN             bPagingIo = FALSE;
    BOOLEAN             bNonCachedIo = FALSE;
    BOOLEAN             bCompleteIrp = TRUE;
    BOOLEAN             bMapped;
    PFILE_OBJECT        pFileObject = NULL;
    LARGE_INTEGER       liStartingByte;
    ULONG               ulByteCount;
    VOID               *pSystemBuffer = NULL;

    pIrpSp = IoGetCurrentIrpStackLocation( Irp);
    pDeviceExt = (AFSDeviceExt *)DeviceObject->DeviceExtension;

    __Enter
    {
        //
        // Decode the fileobject
        //
        pFileObject = pIrpSp->FileObject;

        //
        // There is a risk (albeit infinitely small) that the Irp will
        // complete before this function exits, then a cleanup and
        // close will happen and the FCB will be torn down before we
        // get to the try_exit.  Pin the file Object which will pin the FCB
        //

        ObReferenceObject( pFileObject);

        //
        // If we are in shutdown mode then fail the request
        //

        if( BooleanFlagOn( pDeviceExt->DeviceFlags, AFS_DEVICE_FLAG_REDIRECTOR_SHUTDOWN))
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_FILE_PROCESSING,
                          AFS_TRACE_LEVEL_WARNING,
                          "AFSCommonRead (%08lX) Open request after shutdown\n",
                          Irp);

            try_return( ntStatus = STATUS_TOO_LATE);
        }

        pFcb = (AFSFcb *)pFileObject->FsContext;

        pCcb = (AFSCcb *)pFileObject->FsContext2;

        if( pFcb == NULL)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSCommonRead Attempted read (%08lX) when pFcb == NULL\n",
                          Irp);

            try_return( ntStatus = STATUS_INVALID_DEVICE_REQUEST);
        }

        bPagingIo = BooleanFlagOn( Irp->Flags, IRP_PAGING_IO);
        bNonCachedIo = BooleanFlagOn( Irp->Flags,IRP_NOCACHE);

        liStartingByte = pIrpSp->Parameters.Read.ByteOffset;
        ulByteCount = pIrpSp->Parameters.Read.Length;

        if( pFcb->Header.NodeTypeCode != AFS_IOCTL_FCB &&
            pFcb->Header.NodeTypeCode != AFS_FILE_FCB &&
            pFcb->Header.NodeTypeCode != AFS_SPECIAL_SHARE_FCB)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSCommonRead Attempted read (%08lX) on an invalid node type %08lX\n",
                          Irp,
                          pFcb->Header.NodeTypeCode);

            try_return( ntStatus = STATUS_INVALID_DEVICE_REQUEST);
        }

        //
        // If this is a read against an IOCtl node then handle it
        // in a different pathway
        //

        if( pFcb->Header.NodeTypeCode == AFS_IOCTL_FCB)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSCommonRead (%08lX) Processing file (PIOCTL) Offset %I64X Length %08lX Irp Flags %08lX\n",
                          Irp,
                          liStartingByte.QuadPart,
                          ulByteCount,
                          Irp->Flags);

            ntStatus = AFSIOCtlRead( DeviceObject,
                                     Irp);

            try_return( ntStatus);
        }
        else if( pFcb->Header.NodeTypeCode == AFS_SPECIAL_SHARE_FCB)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSCommonRead (%08lX) Processing file (SHARE) Offset %I64X Length %08lX Irp Flags %08lX\n",
                          Irp,
                          liStartingByte.QuadPart,
                          ulByteCount,
                          Irp->Flags);

            ntStatus = AFSShareRead( DeviceObject,
                                     Irp);

            try_return( ntStatus);
        }

        //
        // No fileobject yet?  Bail.
        //
        if( !BooleanFlagOn( AFSLibControlFlags, AFS_REDIR_LIB_FLAGS_NONPERSISTENT_CACHE) &&
            NULL == pDeviceExt->Specific.RDR.CacheFileObject)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSCommonRead (%08lX) Request failed due to AFS cache closed\n",
                          Irp);

            try_return( ntStatus = STATUS_TOO_LATE );
        }

        if( pIrpSp->Parameters.Read.Length == 0)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSCommonRead (%08lX) Request completed due to zero length\n",
                          Irp);

            try_return( ntStatus);
        }

        if ( FlagOn(pIrpSp->MinorFunction, IRP_MN_COMPLETE) )
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSCommonRead (%08lX) IRP_MN_COMPLETE being processed\n",
                          Irp);

            CcMdlReadComplete(pIrpSp->FileObject, Irp->MdlAddress);

            //
            // Mdl is now Deallocated
            //

            Irp->MdlAddress = NULL;

            try_return( ntStatus = STATUS_SUCCESS );
        }

        //
        // If we get a non cached IO for a cached file we should do a purge.
        // For now we will just promote to cached
        //
        if (NULL != pFileObject->SectionObjectPointer->DataSectionObject && !bPagingIo)
        {

            bNonCachedIo = FALSE;
        }

        //
        // We acquire the main/paging resource first to synchronize
        // against size checks.
        //

        if( bPagingIo)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_LOCK_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSCommonRead Acquiring Fcb PagingIo lock %08lX SHARED %08lX\n",
                                                              &pFcb->NPFcb->PagingResource,
                                                              PsGetCurrentThread());

            AFSAcquireShared( &pFcb->NPFcb->PagingResource,
                              TRUE);

            bReleasePaging = TRUE;
        }
        else
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_LOCK_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSCommonRead Acquiring Fcb lock %08lX SHARED %08lX\n",
                                                              &pFcb->NPFcb->Resource,
                                                              PsGetCurrentThread());

            AFSAcquireShared( &pFcb->NPFcb->Resource,
                              TRUE);

            bReleaseMain = TRUE;

            //
            // Check the BR locks
            //

            if( !FsRtlCheckLockForReadAccess( &pFcb->Specific.File.FileLock,
                                              Irp))
            {

                AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                              AFS_TRACE_LEVEL_ERROR,
                              "AFSCommonRead (%08lX) Request failed due to lock conflict\n",
                              Irp);

                try_return( ntStatus = STATUS_FILE_LOCK_CONFLICT);
            }
        }

        if( BooleanFlagOn( pCcb->DirectoryCB->Flags, AFS_DIR_ENTRY_DELETED) ||
            BooleanFlagOn( pFcb->ObjectInformation->Flags, AFS_OBJECT_FLAGS_DELETED))
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSCommonRead (%08lX) Request failed due to file deleted\n",
                          Irp);

            try_return( ntStatus = STATUS_FILE_DELETED);
        }

        //
        // If the read starts beyond End of File, return EOF.
        //

        if( liStartingByte.QuadPart >= pFcb->Header.FileSize.QuadPart)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSCommonRead (%08lX) Request beyond EOF %I64X\n",
                          Irp,
                          pFcb->Header.FileSize.QuadPart);

            try_return( ntStatus = STATUS_END_OF_FILE);
        }

        //
        //

        if( liStartingByte.QuadPart + ulByteCount > pFcb->Header.FileSize.QuadPart)
        {

            ulByteCount = (ULONG)(pFcb->Header.FileSize.QuadPart - liStartingByte.QuadPart);

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSCommonRead (%08lX) Truncated read request to %08lX\n",
                          Irp,
                          ulByteCount);
        }

        //
        // Is this Fcb valid???
        //

        if( BooleanFlagOn( pFcb->ObjectInformation->Flags, AFS_OBJECT_FLAGS_OBJECT_INVALID))
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSCommonRead (%08lX) Failing request due to INVALID fcb\n",
                          Irp);

            Irp->IoStatus.Information = 0;

            try_return( ntStatus = STATUS_FILE_DELETED);
        }

        //
        // If this is an cached IO
        //
        if( (!bPagingIo && !bNonCachedIo))
        {
            //
            // This is a top level irp. Init the caching if it has not yet
            // been initialzed for this FO
            //

            if( pFileObject->PrivateCacheMap == NULL)
            {

                __try
                {

                    AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                                  AFS_TRACE_LEVEL_VERBOSE,
                                  "AFSCommonRead Initialize caching on Fcb %08lX FO %08lX\n",
                                      pFcb,
                                      pFileObject);

                    CcInitializeCacheMap( pFileObject,
                                          (PCC_FILE_SIZES)&pFcb->Header.AllocationSize,
                                          FALSE,
                                          AFSLibCacheManagerCallbacks,
                                          pFcb);

                    CcSetReadAheadGranularity( pFileObject,
                                               pDeviceExt->Specific.RDR.MaximumRPCLength);

                    CcSetDirtyPageThreshold( pFileObject,
                                             AFS_DIRTY_CHUNK_THRESHOLD * pDeviceExt->Specific.RDR.MaximumRPCLength);
                }
                __except( EXCEPTION_EXECUTE_HANDLER)
                {

                    ntStatus = GetExceptionCode();

                    AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                                  AFS_TRACE_LEVEL_ERROR,
                                  "AFSCommonRead (%08lX) Exception thrown while initializing cache map Status %08lX\n",
                                  Irp,
                                  ntStatus);
                }

                if( !NT_SUCCESS( ntStatus))
                {

                    try_return( ntStatus);
                }
            }

            //
            // And if this is MDL operation, deal with it now (yes we
            // could post, but we are almost certainly posted
            // already and we don't want to grow and SVA for it..)
            //

            if( BooleanFlagOn( pIrpSp->MinorFunction, IRP_MN_MDL))
            {
                __try
                {

                    CcMdlRead( pFileObject,
                               &liStartingByte,
                               ulByteCount,
                               &Irp->MdlAddress,
                               &Irp->IoStatus);
                    ntStatus = Irp->IoStatus.Status;
                }
                __except( EXCEPTION_EXECUTE_HANDLER)
                {
                    ntStatus = GetExceptionCode();

                    AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                                  AFS_TRACE_LEVEL_ERROR,
                                  "AFSCommonRead (%08lX) Exception thrown during mdl read Status %08lX\n",
                                  Irp,
                                  ntStatus);
                }

                if( !NT_SUCCESS( ntStatus))
                {

                    AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                                  AFS_TRACE_LEVEL_ERROR,
                                  "AFSCommonRead (%08lX) Failed to process MDL read Status %08lX\n",
                                  Irp,
                                  ntStatus);

                    try_return( ntStatus);
                }

                //
                // Update the CBO if this is a sync read
                //

                if( BooleanFlagOn( pFileObject->Flags, FO_SYNCHRONOUS_IO))
                {

                    pFileObject->CurrentByteOffset.QuadPart = liStartingByte.QuadPart + ulByteCount;
                }

                try_return( ntStatus);
            }
        }

        //
        // The called request completes the IRP for us.
        //

        bCompleteIrp = FALSE;

        if( !bPagingIo &&
            !bNonCachedIo)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSCommonRead (%08lX) Processing CACHED request Offset %I64X Len %08lX\n",
                          Irp,
                          liStartingByte.QuadPart,
                          ulByteCount);

            ntStatus = AFSCachedRead( DeviceObject, Irp, liStartingByte, ulByteCount);
        }
        else
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                          AFS_TRACE_LEVEL_VERBOSE,
                          "AFSCommonRead (%08lX) Processing NON-CACHED request Offset %I64X Len %08lX\n",
                          Irp,
                          liStartingByte.QuadPart,
                          ulByteCount);

            ntStatus = AFSNonCachedRead( DeviceObject, Irp,  liStartingByte);
        }

try_exit:

        AFSDbgLogMsg( AFS_SUBSYSTEM_IO_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSCommonRead (%08lX) Process complete Status %08lX\n",
                      Irp,
                      ntStatus);

        if( bReleasePaging)
        {

            AFSReleaseResource( &pFcb->NPFcb->PagingResource);
        }

        if( bReleaseMain)
        {

            AFSReleaseResource( &pFcb->NPFcb->Resource);
        }

        if( bCompleteIrp)
        {

            AFSCompleteRequest( Irp, ntStatus);
        }

        ObDereferenceObject( pFileObject);
    }

    return ntStatus;
}

NTSTATUS
AFSIOCtlRead( IN PDEVICE_OBJECT DeviceObject,
              IN PIRP Irp)
{

    NTSTATUS ntStatus = STATUS_SUCCESS;
    AFSPIOCtlIORequestCB stIORequestCB;
    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation( Irp);
    AFSFcb *pFcb = NULL;
    AFSCcb *pCcb = NULL;
    AFSPIOCtlIOResultCB stIOResultCB;
    ULONG ulBytesReturned = 0;
    AFSFileID   stParentFID;

    __Enter
    {

        RtlZeroMemory( &stIORequestCB,
                       sizeof( AFSPIOCtlIORequestCB));

        if( pIrpSp->Parameters.Read.Length == 0)
        {

            //
            // Nothing to do in this case
            //

            try_return( ntStatus);
        }

        pFcb = (AFSFcb *)pIrpSp->FileObject->FsContext;

        pCcb = (AFSCcb *)pIrpSp->FileObject->FsContext2;

        AFSDbgLogMsg( AFS_SUBSYSTEM_LOCK_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSIOCtlRead Acquiring Fcb lock %08lX SHARED %08lX\n",
                      &pFcb->NPFcb->Resource,
                      PsGetCurrentThread());

        AFSAcquireShared( &pFcb->NPFcb->Resource,
                          TRUE);

        //
        // Get the parent fid to pass to the cm
        //

        RtlZeroMemory( &stParentFID,
                       sizeof( AFSFileID));

        if( pFcb->ObjectInformation->ParentObjectInformation != NULL)
        {

            //
            // The parent directory FID of the node
            //

            ASSERT( pFcb->ObjectInformation->ParentObjectInformation->FileType == AFS_FILE_TYPE_DIRECTORY);

            stParentFID = pFcb->ObjectInformation->ParentObjectInformation->FileId;
        }

        //
        // Set the control block up
        //

        stIORequestCB.RequestId = pCcb->RequestID;

        if( pFcb->ObjectInformation->VolumeCB != NULL)
        {
            stIORequestCB.RootId = pFcb->ObjectInformation->VolumeCB->ObjectInformation.FileId;
        }

        //
        // Lock down the buffer
        //

        stIORequestCB.MappedBuffer = AFSMapToService( Irp,
                                                      pIrpSp->Parameters.Read.Length);

        if( stIORequestCB.MappedBuffer == NULL)
        {

            try_return( ntStatus = STATUS_INSUFFICIENT_RESOURCES);
        }

        stIORequestCB.BufferLength = pIrpSp->Parameters.Read.Length;

        stIOResultCB.BytesProcessed = 0;

        ulBytesReturned = sizeof( AFSPIOCtlIOResultCB);

        //
        // Issue the request to the service
        //

        ntStatus = AFSProcessRequest( AFS_REQUEST_TYPE_PIOCTL_READ,
                                      AFS_REQUEST_FLAG_SYNCHRONOUS,
                                      &pCcb->AuthGroup,
                                      NULL,
                                      &stParentFID,
                                      (void *)&stIORequestCB,
                                      sizeof( AFSPIOCtlIORequestCB),
                                      &stIOResultCB,
                                      &ulBytesReturned);

        if( !NT_SUCCESS( ntStatus))
        {

            try_return( ntStatus);
        }

        //
        // Update the length read
        //

        Irp->IoStatus.Information = stIOResultCB.BytesProcessed;

try_exit:

        if( stIORequestCB.MappedBuffer != NULL)
        {

            AFSUnmapServiceMappedBuffer( stIORequestCB.MappedBuffer,
                                         Irp->MdlAddress);
        }

        if( pFcb != NULL)
        {

            AFSReleaseResource( &pFcb->NPFcb->Resource);
        }
    }

    return ntStatus;
}

NTSTATUS
AFSShareRead( IN PDEVICE_OBJECT DeviceObject,
              IN PIRP Irp)
{

    NTSTATUS ntStatus = STATUS_SUCCESS;
    AFSPIOCtlIORequestCB stIORequestCB;
    PIO_STACK_LOCATION pIrpSp = IoGetCurrentIrpStackLocation( Irp);
    AFSFcb *pFcb = NULL;
    AFSCcb *pCcb = NULL;
    ULONG ulBytesReturned = 0;
    void *pBuffer = NULL;
    AFSPipeIORequestCB stIoRequest;

    __Enter
    {

        pFcb = (AFSFcb *)pIrpSp->FileObject->FsContext;

        pCcb = (AFSCcb *)pIrpSp->FileObject->FsContext2;

        AFSDbgLogMsg( AFS_SUBSYSTEM_PIPE_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSShareRead On pipe %wZ Length %08lX\n",
                      &pCcb->DirectoryCB->NameInformation.FileName,
                      pIrpSp->Parameters.Read.Length);

        if( pIrpSp->Parameters.Read.Length == 0)
        {

            //
            // Nothing to do in this case
            //

            try_return( ntStatus);
        }

        AFSAcquireShared( &pFcb->NPFcb->Resource,
                          TRUE);

        //
        // Retrieve the buffer for the read request
        //

        pBuffer = AFSLockSystemBuffer( Irp,
                                       pIrpSp->Parameters.Read.Length);

        if( pBuffer == NULL)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_PIPE_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSShareRead Failed to map buffer on pipe %wZ\n",
                          &pCcb->DirectoryCB->NameInformation.FileName);

            try_return( ntStatus = STATUS_INSUFFICIENT_RESOURCES);
        }

        RtlZeroMemory( &stIoRequest,
                       sizeof( AFSPipeIORequestCB));

        stIoRequest.RequestId = pCcb->RequestID;

        stIoRequest.RootId = pFcb->ObjectInformation->VolumeCB->ObjectInformation.FileId;

        stIoRequest.BufferLength = pIrpSp->Parameters.Read.Length;

        ulBytesReturned = pIrpSp->Parameters.Read.Length;

        //
        // Issue the open request to the service
        //

        ntStatus = AFSProcessRequest( AFS_REQUEST_TYPE_PIPE_READ,
                                      AFS_REQUEST_FLAG_SYNCHRONOUS,
                                      &pCcb->AuthGroup,
                                      &pCcb->DirectoryCB->NameInformation.FileName,
                                      NULL,
                                      (void *)&stIoRequest,
                                      sizeof( AFSPipeIORequestCB),
                                      pBuffer,
                                      &ulBytesReturned);

        if( !NT_SUCCESS( ntStatus) &&
            ntStatus != STATUS_BUFFER_OVERFLOW)
        {

            AFSDbgLogMsg( AFS_SUBSYSTEM_FILE_PROCESSING,
                          AFS_TRACE_LEVEL_ERROR,
                          "AFSShareRead (%08lX) Failed service read Status %08lX\n",
                          Irp,
                          ntStatus);

            try_return( ntStatus);
        }

        AFSDbgLogMsg( AFS_SUBSYSTEM_PIPE_PROCESSING,
                      AFS_TRACE_LEVEL_VERBOSE,
                      "AFSShareRead Completed on pipe %wZ Length read %08lX Status %08lX\n",
                      &pCcb->DirectoryCB->NameInformation.FileName,
                      ulBytesReturned,
                      ntStatus);

        Irp->IoStatus.Information = ulBytesReturned;

try_exit:

        if( pFcb != NULL)
        {

            AFSReleaseResource( &pFcb->NPFcb->Resource);
        }
    }

    return ntStatus;
}
