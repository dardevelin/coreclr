//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information. 
//

//
// GCSample.cpp
//

//
//  This sample demonstrates:
//
//  * How to initialize GC without the rest of CoreCLR
//  * How to create a type layout information in format that the GC expects
//  * How to implement fast object allocator and write barrier 
//  * How to allocate objects and work with GC handles
//
//  An important part of the sample is the GC environment (gcenv.*) that provides methods for GC to interact 
//  with the OS and execution engine.
//
// The methods to interact with the OS should be no surprise - block memory allocation, synchronization primitives, etc.
//
// The important methods that the execution engine needs to provide to GC are:
//
// * Thread suspend/resume:
//      static void SuspendEE(SUSPEND_REASON reason);
//      static void RestartEE(bool bFinishedGC); //resume threads.
//
// * Enumeration of threads that are running managed code:
//      static Thread * GetThreadList(Thread * pThread);
//
// * Scanning of stack roots of given thread:
//      static void ScanStackRoots(Thread * pThread, promote_func* fn, ScanContext* sc);
//
//  The sample has trivial implementation for these methods. It is single threaded, and there are no stack roots to 
//  be reported. There are number of other callbacks that GC calls to optionally allow the execution engine to do its 
//  own bookkeeping.
//
//  For now, the sample GC environment has some cruft in it to decouple the GC from Windows and rest of CoreCLR. It is something we would like to clean up.
//

#include "common.h"

#include "gcenv.h"

#include "gc.h"
#include "objecthandle.h"

#include "gcdesc.h"

//
// The fast paths for object allocation and write barriers is performance critical. They are often
// hand written in assembly code, etc.
//
Object * AllocateObject(MethodTable * pMT)
{
    alloc_context * acontext = GetThread()->GetAllocContext();
    Object * pObject;

    size_t size = pMT->GetBaseSize();

    BYTE* result = acontext->alloc_ptr;
    BYTE* advance = result + size;
    if (advance <= acontext->alloc_limit)
    {
        acontext->alloc_ptr = advance;
        pObject = (Object *)result;
    }
    else
    {
        pObject = GCHeap::GetGCHeap()->Alloc(acontext, size, 0);
        if (pObject == nullptr)
            return nullptr;
    }

    pObject->SetMethodTable(pMT);

    return pObject;
}

#if defined(_WIN64)
// Card byte shift is different on 64bit.
#define card_byte_shift     11
#else
#define card_byte_shift     10
#endif

#define card_byte(addr) (((size_t)(addr)) >> card_byte_shift)

inline void ErectWriteBarrier(Object ** dst, Object * ref)
{
    // if the dst is outside of the heap (unboxed value classes) then we
    //      simply exit
    if (((BYTE*)dst < g_lowest_address) || ((BYTE*)dst >= g_highest_address))
        return;
        
    if((BYTE*)ref >= g_ephemeral_low && (BYTE*)ref < g_ephemeral_high)
    {
        // volatile is used here to prevent fetch of g_card_table from being reordered 
        // with g_lowest/highest_address check above. See comment in code:gc_heap::grow_brick_card_tables.
        BYTE* pCardByte = (BYTE *)*(volatile BYTE **)(&g_card_table) + card_byte((BYTE *)dst);
        if(*pCardByte != 0xFF)
            *pCardByte = 0xFF;
    }
}

void WriteBarrier(Object ** dst, Object * ref)
{
    *dst = ref;
    ErectWriteBarrier(dst, ref);
}

int main(int argc, char* argv[])
{
    //
    // Initialize system info
    //
    InitializeSystemInfo();

    // 
    // Initialize free object methodtable. The GC uses a special array-like methodtable as placeholder
    // for collected free space.
    //
    static MethodTable freeObjectMT;
    freeObjectMT.InitializeFreeObject();
    g_pFreeObjectMethodTable = &freeObjectMT;

    //
    // Initialize handle table
    //
    if (!Ref_Initialize())
        return -1;

    //
    // Initialize GC heap
    //
    GCHeap *pGCHeap = GCHeap::CreateGCHeap();
    if (!pGCHeap)
        return -1;

    if (FAILED(pGCHeap->Initialize()))
        return -1;

    //
    // Initialize current thread
    //
    ThreadStore::AttachCurrentThread(false);

    //
    // Create a Methodtable with GCDesc
    //

    class My : Object {
    public:
        Object * m_pOther;
    };

    static struct My_MethodTable
    {
        // GCDesc
        CGCDescSeries m_series[1];
        size_t m_numSeries;

        // The actual methodtable
        MethodTable m_MT;
    }
    My_MethodTable;

    My_MethodTable.m_numSeries = 1;
    My_MethodTable.m_series[0].SetSeriesOffset(offsetof(My, m_pOther));
    My_MethodTable.m_series[0].SetSeriesCount(1);

    My_MethodTable.m_MT.m_baseSize = 3 * sizeof(void *);
    My_MethodTable.m_MT.m_componentSize = 0;    // Array component size
    My_MethodTable.m_MT.m_flags = MTFlag_ContainsPointers;

    MethodTable * pMyMethodTable = &My_MethodTable.m_MT;

    // Allocate instance of MyObject
    Object * pObj = AllocateObject(pMyMethodTable);
    if (pObj == nullptr)
        return -1;

    // Create strong handle and store the object into it
    OBJECTHANDLE oh = CreateGlobalHandle(pObj);
    if (oh == nullptr)
        return -1;

    for (int i = 0; i < 1000000; i++)
    {
        Object * pBefore = ((My *)ObjectFromHandle(oh))->m_pOther;

        // Allocate more instances of the same object
        Object * p = AllocateObject(pMyMethodTable);
        if (p == nullptr)
            return -1;

        Object * pAfter = ((My *)ObjectFromHandle(oh))->m_pOther;

        // Uncomment this assert to see how GC triggered inside AllocateObject moved objects around
        // assert(pBefore == pAfter);

        // Store the newly allocated object into a field using WriteBarrier
        WriteBarrier(&(((My *)ObjectFromHandle(oh))->m_pOther), p);
    }

    // Create weak handle that points to our object
    OBJECTHANDLE ohWeak = CreateGlobalWeakHandle(ObjectFromHandle(oh));
    if (ohWeak == nullptr)
        return -1;

    // Destroy the strong handle so that nothing will be keeping out object alive
    DestroyGlobalHandle(oh);

    // Explicitly trigger full GC
    pGCHeap->GarbageCollect();

    // Verify that the weak handle got cleared by the GC
    assert(ObjectFromHandle(ohWeak) == NULL);

    return 0;
}