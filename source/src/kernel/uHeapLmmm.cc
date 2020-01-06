//                              -*- Mode: C++ -*- 
// 
// uC++ Version 7.0.0, Copyright (C) Peter A. Buhr 1994
// 
// uHeapLmmm.cc -- Lean Mean Malloc Machine - a runtime configurable replacement
//                 for malloc.
// 
// Author           : Peter A. Buhr
// Created On       : Sat Nov 11 16:07:20 1988
// Last Modified By : Peter A. Buhr
// Last Modified On : Mon Jan  6 11:47:21 2020
// Update Count     : 1510
//
// This  library is free  software; you  can redistribute  it and/or  modify it
// under the terms of the GNU Lesser General Public License as published by the
// Free Software  Foundation; either  version 2.1 of  the License, or  (at your
// option) any later version.
// 
// This library is distributed in the  hope that it will be useful, but WITHOUT
// ANY  WARRANTY;  without even  the  implied  warranty  of MERCHANTABILITY  or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
// 
// You should  have received a  copy of the  GNU Lesser General  Public License
// along  with this library.
// 

#define __U_KERNEL__
#include <uC++.h>
#include <uHeapLmmm.h>
#include <uAlign.h>
#ifdef __U_PROFILER__
#include <uProfiler.h>
#endif // __U_PROFILER__

#include <uDebug.h>					// access: uDebugWrite
#undef __U_DEBUG_H__					// turn off debug prints

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <unistd.h>					// sbrk, sysconf

#define LIKELY(x)       __builtin_expect(!!(x), 1)
#define UNLIKELY(x)     __builtin_expect(!!(x), 0)


namespace UPP {
    #ifdef __U_DEBUG__
    static bool uHeapBoot = false;			// detect recursion during boot
    #endif // __U_DEBUG__
    static char uHeapStorage[sizeof(uHeapManager)] __attribute__(( aligned (128) )) = {0}; // size of cache line to prevent false sharing

    uHeapManager * uHeapManager::heapManagerInstance = nullptr;
    size_t uHeapManager::pageSize;			// architecture pagesize
    size_t uHeapManager::heapExpand;			// sbrk advance
    size_t uHeapManager::mmapStart;			// cross over point for mmap
    unsigned int uHeapManager::maxBucketsUsed;		// maximum number of buckets in use

    // Bucket size must be multiple of 16.
    // Powers of 2 are common allocation sizes, so make powers of 2 generate the minimum required size.
    unsigned int uHeapManager::bucketSizes[] = {
	16, 32, 48, 64 + sizeof(uHeapManager::Storage), // 4
	96, 112, 128 + sizeof(uHeapManager::Storage), // 3
	160, 192, 224, 256 + sizeof(uHeapManager::Storage), // 4
	320, 384, 448, 512 + sizeof(uHeapManager::Storage), // 4
	640, 768, 896, 1024 + sizeof(uHeapManager::Storage), // 4
	1536, 2048 + sizeof(uHeapManager::Storage), // 2
	2560, 3072, 3584, 4096 + sizeof(uHeapManager::Storage), // 4
	6144, 8192 + sizeof(uHeapManager::Storage), // 2
	9216, 10240, 11264, 12288, 13312, 14336, 15360, 16384 + sizeof(uHeapManager::Storage), // 8
	18432, 20480, 22528, 24576, 26624, 28672, 30720, 32768 + sizeof(uHeapManager::Storage), // 8
	36864, 40960, 45056, 49152, 53248, 57344, 61440, 65536 + sizeof(uHeapManager::Storage), // 8
	73728, 81920, 90112, 98304, 106496, 114688, 122880, 131072 + sizeof(uHeapManager::Storage), // 8
	147456, 163840, 180224, 196608, 212992, 229376, 245760, 262144 + sizeof(uHeapManager::Storage), // 8
	294912, 327680, 360448, 393216, 425984, 458752, 491520, 524288 + sizeof(uHeapManager::Storage), // 8
	655360, 786432, 917504, 1048576 + sizeof(uHeapManager::Storage), // 4
	1179648, 1310720, 1441792, 1572864, 1703936, 1835008, 1966080, 2097152 + sizeof(uHeapManager::Storage), // 8
	2621440, 3145728, 3670016, 4194304 + sizeof(uHeapManager::Storage) // 4
    };
    // FIX ME
    //static_assert( uHeapManager::NoBucketSizes == sizeof(uHeapManager::bucketSizes) / sizeof(uHeapManager::bucketSizes[0]), "size of bucket array wrong" );
    #ifdef FASTLOOKUP
    unsigned char uHeapManager::lookup[];		// array size defined in .h
    #endif // FASTLOOKUP

    int uHeapManager::mmapFd = -1;			// fake or actual fd for anonymous file
    #ifdef __U_DEBUG__
    unsigned long int uHeapManager::allocfree = 0;
    #endif // __U_DEBUG__

    #ifdef __U_STATISTICS__
    // Heap statistics counters.
    unsigned long long int uHeapManager::mmap_storage = 0;
    unsigned int uHeapManager::mmap_calls = 0;
    unsigned long long int uHeapManager::munmap_storage = 0;
    unsigned int uHeapManager::munmap_calls = 0;
    unsigned long long int uHeapManager::sbrk_storage = 0;
    unsigned int uHeapManager::sbrk_calls = 0;
    unsigned long long int uHeapManager::malloc_storage = 0;
    unsigned int uHeapManager::malloc_calls = 0;
    unsigned long long int uHeapManager::free_storage = 0;
    unsigned int uHeapManager::free_calls = 0;
    unsigned long long int uHeapManager::calloc_storage = 0;
    unsigned int uHeapManager::calloc_calls = 0;
    unsigned long long int uHeapManager::memalign_storage = 0;
    unsigned int uHeapManager::memalign_calls = 0;
    unsigned long long int uHeapManager::cmemalign_storage = 0;
    unsigned int uHeapManager::cmemalign_calls = 0;
    unsigned long long int uHeapManager::realloc_storage = 0;
    unsigned int uHeapManager::realloc_calls = 0;
    // Statistics file descriptor (changed by malloc_stats_fd).
    int uHeapManager::stats_fd = STDERR_FILENO;		// default stderr

    // Use "write" because streams may be shutdown when calls are made.
    void uHeapManager::print() {
	char helpText[512];
	int len = snprintf( helpText, sizeof(helpText),
			    "\nHeap statistics:\n"
			    "  malloc: calls %u / storage %llu\n"
			    "  calloc: calls %u / storage %llu\n"
			    "  memalign: calls %u / storage %llu\n"
			    "  cmemalign: calls %u / storage %llu\n"
			    "  realloc: calls %u / storage %llu\n"
			    "  free: calls %u / storage %llu\n"
			    "  mmap: calls %u / storage %llu\n"
			    "  munmap: calls %u / storage %llu\n"
			    "  sbrk: calls %u / storage %llu\n",
			    malloc_calls, malloc_storage,
			    calloc_calls, calloc_storage,
			    memalign_calls, memalign_storage,
			    cmemalign_calls, cmemalign_storage,
			    realloc_calls, realloc_storage,
			    free_calls, free_storage,
			    mmap_calls, mmap_storage,
			    munmap_calls, munmap_storage,
			    sbrk_calls, sbrk_storage
	    );
	uDebugWrite( stats_fd, helpText, len );
    } // uHeapManager::print

    int uHeapManager::printXML( FILE * stream ) {
	char helpText[512];
	int len = snprintf( helpText, sizeof(helpText),
			    "<malloc version=\"1\">\n"
			    "<heap nr=\"0\">\n"
			    "<sizes>\n"
			    "</sizes>\n"
			    "<total type=\"malloc\" count=\"%u\" size=\"%llu\"/>\n"
			    "<total type=\"calloc\" count=\"%u\" size=\"%llu\"/>\n"
			    "<total type=\"memalign\" count=\"%u\" size=\"%llu\"/>\n"
			    "<total type=\"cmemalign\" count=\"%u\" size=\"%llu\"/>\n"
			    "<total type=\"realloc\" count=\"%u\" size=\"%llu\"/>\n"
			    "<total type=\"free\" count=\"%u\" size=\"%llu\"/>\n"
			    "<total type=\"mmap\" count=\"%u\" size=\"%llu\"/>\n"
			    "<total type=\"munmap\" count=\"%u\" size=\"%llu\"/>\n"
			    "<total type=\"sbrk\" count=\"%u\" size=\"%llu\"/>\n"
			    "</malloc>",
			    malloc_calls, malloc_storage,
			    calloc_calls, calloc_storage,
			    memalign_calls, memalign_storage,
			    cmemalign_calls, cmemalign_storage,
			    realloc_calls, realloc_storage,
			    free_calls, free_storage,
			    mmap_calls, mmap_storage,
			    munmap_calls, munmap_storage,
			    sbrk_calls, sbrk_storage
	    );
	uDebugWrite( fileno( stream ), helpText, len );	// ensures all bytes written or exit
	return len;
    } // printXML
    #endif // __U_STATISTICS__

    inline void uHeapManager::noMemory() {
	abort( "Heap memory exhausted at %zu bytes.\n"
		"Possible cause is very large memory allocation and/or large amount of unfreed storage allocated by the program or system/library routines.",
		((char *)(sbrk( 0 )) - (char *)(uHeapManager::heapManagerInstance->heapBegin)) );
    } // uHeapManager::noMemory

    inline void uHeapManager::checkAlign( size_t alignment ) {
	if ( alignment < uAlign() || ! uPow2( alignment ) ) {
	    abort( "Alignment %zu for memory allocation is less than %d and/or not a power of 2.", alignment, uAlign() );
	} // if
    } // uHeapManager::checkAlign

    bool uHeapManager::setHeapExpand( size_t value ) {
      if ( heapExpand < pageSize ) return true;
	heapExpand = value;
	return false;
    } // uHeapManager::setHeapExpand

    bool uHeapManager::setMmapStart( size_t value ) {	// true => mmapped, false => sbrk
      if ( value < pageSize || bucketSizes[NoBucketSizes-1] < value ) return true;
	mmapStart = value;				// set global

	// find the closest bucket size less than or equal to the mmapStart size
	maxBucketsUsed = std::lower_bound( bucketSizes, bucketSizes + (NoBucketSizes - 1), mmapStart ) - bucketSizes; // binary search
	assert( maxBucketsUsed < NoBucketSizes );	// subscript failure ?
	assert( mmapStart <= bucketSizes[maxBucketsUsed] ); // search failure ?
	return false;
    } // uHeapManager::setMmapStart

    static inline void checkHeader( bool check, const char * name, void * addr ) {
	if ( UNLIKELY( check ) ) {			// bad address ?
	    abort( "Attempt to %s storage %p with address outside the heap.\n"
		   "Possible cause is duplicate free on same block or overwriting of memory.",
		   name, addr );
	} // if
    } // checkHeader

    inline void uHeapManager::fakeHeader( Storage::Header *& header, size_t & alignment ) {
	if ( UNLIKELY( (header->kind.fake.alignment & 1) == 1 ) ) { // fake header ?
	    size_t offset = header->kind.fake.offset;
	    alignment = header->kind.fake.alignment & -2; // remove flag from value
	    #ifdef __U_DEBUG__
	    checkAlign( alignment );			// check alignment
	    #endif // __U_DEBUG__
	    header = (Storage::Header *)((char *)header - offset);
	} // if
    } // fakeHeader

    // <-------+----------------------------------------------------> bsize (bucket size)
    // |header |addr
    //==================================================================================
    //                                | alignment
    // <-----------------<------------+-----------------------------> bsize (bucket size)
    //                   |fake-header | addr
    #define headerAddr( addr ) ((UPP::uHeapManager::Storage::Header *)( (char *)addr - sizeof(UPP::uHeapManager::Storage) ))

    // <-------<<--------------------- dsize ---------------------->> bsize (bucket size)
    // |header |addr
    //==================================================================================
    //                                | alignment
    // <------------------------------<<---------- dsize --------->>> bsize (bucket size)
    //                   |fake-header |addr
    #define dataStorage( bsize, addr, header ) (bsize - ( (char *)addr - (char *)header ))

    inline bool uHeapManager::headers( const char * name __attribute__(( unused )), void * addr, Storage::Header *& header, FreeHeader *& freeElem, size_t & size, size_t & alignment ) {
	header = headerAddr( addr );

	if ( UNLIKELY( heapEnd < addr ) ) {		// mmapped ?
	    fakeHeader( header, alignment );
	    size = header->kind.real.blockSize & -3;	// mmap size
	    return true;
	} // if

	#ifdef __U_DEBUG__
	checkHeader( addr < heapBegin || header < heapBegin, name, addr ); // bad low address ?
	#endif // __U_DEBUG__

	// header may be safe to dereference
	fakeHeader( header, alignment );
	#ifdef __U_DEBUG__
	checkHeader( header < heapBegin || heapEnd < header, name, addr ); // bad address ? (offset could be + or -)
	#endif // __U_DEBUG__

	freeElem = (FreeHeader *)((size_t)header->kind.real.home & -3);
	#ifdef __U_DEBUG__
	if ( freeElem < &freeLists[0] || &freeLists[NoBucketSizes] <= freeElem ) {
	    abort( "Attempt to %s storage %p with corrupted header.\n"
		   "Possible cause is duplicate free on same block or overwriting of header information.",
		   name, addr );
	} // if
	#endif // __U_DEBUG__
	size = freeElem->blockSize;
	return false;
    } // uHeapManager::headers


    inline void * uHeapManager::extend( size_t size ) {
	extlock.acquire();
	uDEBUGPRT( uDebugPrt( "(uHeapManager &)%p.extend( %zu ), heapBegin:%p, heapEnd:%p, heapRemaining:0x%zx, sbrk:%p\n",
			      this, size, heapBegin, heapEnd, heapRemaining, sbrk(0) ); )
	ptrdiff_t rem = heapRemaining - size;
	if ( rem < 0 ) {
	    // If the size requested is bigger than the current remaining storage, increase the size of the heap.

	    size_t increase = uCeiling( size > heapExpand ? size : heapExpand, uAlign() );
	    if ( sbrk( increase ) == (void *)-1 ) {
		uDEBUGPRT( uDebugPrt( "0x%zx = (uHeapManager &)%p.extend( %zu ), heapBegin:%p, heapEnd:%p, heapRemaining:0x%zx, sbrk:%p\n",
				      nullptr, this, size, heapBegin, heapEnd, heapRemaining, sbrk(0) ); )
		extlock.release();
		errno = ENOMEM;
		return nullptr;
	    } // if
	    #ifdef __U_STATISTICS__
	    sbrk_calls += 1;
	    sbrk_storage += increase;
	    #endif // __U_STATISTICS__
	    #ifdef __U_DEBUG__
	    // Set new memory to garbage so subsequent uninitialized usages might fail.
	    memset( (char *)heapEnd + heapRemaining, '\377', increase );
	    #endif // __U_DEBUG__
	    rem = heapRemaining + increase - size;
	} // if

	Storage * block = (Storage *)heapEnd;
	heapRemaining = rem;
	heapEnd = (char *)heapEnd + size;
	uDEBUGPRT( uDebugPrt( "%p = (uHeapManager &)%p.extend( %zu ), heapBegin:%p, heapEnd:%p, heapRemaining:0x%zx, sbrk:%p\n",
			      block, this, size, heapBegin, heapEnd, heapRemaining, sbrk(0) ); )
	extlock.release();
	return block;
    } // uHeapManager::extend


    inline void * uHeapManager::doMalloc( size_t size ) {
	uDEBUGPRT( uDebugPrt( "(uHeapManager &)%p.doMalloc( %zu )\n", this, size ); )

	Storage * block;

	// Look up size in the size list.  Make sure the user request includes space for the header that must be allocated
	// along with the block and is a multiple of the alignment size.

      if ( UNLIKELY( size > ~0ul - sizeof(Storage) ) ) return nullptr;
	size_t tsize = size + sizeof(Storage);
	if ( LIKELY( tsize < mmapStart ) ) {		// small size => sbrk
	    FreeHeader * freeElem =
		#ifdef FASTLOOKUP
		tsize < LookupSizes ? &freeLists[lookup[tsize]] :
		#endif // FASTLOOKUP
		std::lower_bound( freeLists, freeLists + maxBucketsUsed, tsize ); // binary search
	    assert( freeElem <= &freeLists[maxBucketsUsed] ); // subscripting error ?
	    assert( tsize <= freeElem->blockSize );	// search failure ?
	    tsize = freeElem->blockSize;		// total space needed for request

	    uDEBUGPRT( uDebugPrt( "(uHeapManager &)%p.doMalloc, size after lookup:%zu\n", this, tsize ); )
    
	    // Spin until the lock is acquired for this particular size of block.

	    #if defined( SPINLOCK )
	    freeElem->lock.acquire();
	    block = freeElem->freeList;			// remove node from stack
	    #else
	    block = freeElem->freeList.pop();
	    #endif // SPINLOCK
	    if ( UNLIKELY( block == nullptr ) ) {	// no free block ?
		#if defined( SPINLOCK )
		freeElem->lock.release();
		#endif // SPINLOCK
		// Freelist for that size was empty, so carve it out of the heap if there's enough left, or get some more
		// and then carve it off.

		block = (Storage *)extend( tsize );	// mutual exclusion on call
      if ( UNLIKELY( block == nullptr ) ) return nullptr;
	    #if defined( SPINLOCK )
	    } else {
		freeElem->freeList = block->header.kind.real.next;
		freeElem->lock.release();
	    #endif // SPINLOCK
	    } // if

	    block->header.kind.real.home = freeElem;	// pointer back to free list of apropriate size
	} else {					// large size => mmap
      if ( UNLIKELY( size > ~0ul - pageSize ) ) return nullptr;
	    tsize = uCeiling( tsize, pageSize );	// must be multiple of page size
	    #ifdef __U_STATISTICS__
	    uFetchAdd( mmap_calls, 1 );
	    uFetchAdd( mmap_storage, tsize );
	    #endif // __U_STATISTICS__
	    block = (Storage *)::mmap( 0, tsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, mmapFd, 0 );
	    if ( block == MAP_FAILED ) {
		// Do not call strerror( errno ) as it may call malloc.
		abort( "(uHeapManager &)0x%p.doMalloc() : internal error, mmap failure, size:%zu error:%d.", this, tsize, errno );
	    } // if
	    #ifdef __U_DEBUG__
	    // Set new memory to garbage so subsequent uninitialized usages might fail.
	    memset( block, '\377', tsize );
	    #endif // __U_DEBUG__
	    block->header.kind.real.blockSize = tsize;	// storage size for munmap
	} // if

	void * addr = &(block->data);			// adjust off header to user bytes

	#ifdef __U_DEBUG__
	assert( ((uintptr_t)addr & (uAlign() - 1)) == 0 ); // minimum alignment ?
	uFetchAdd( uHeapManager::allocfree, tsize );
	if ( uHeapControl::traceHeap() ) {
	    enum { BufferSize = 64 };
	    char helpText[BufferSize];
	    int len = snprintf( helpText, BufferSize, "%p = Malloc( %zu ) (allocated %zu)\n", addr, size, tsize );
	    //int len = snprintf( helpText, BufferSize, "Malloc %p %zu\n", addr, size );
	    uDebugWrite( STDERR_FILENO, helpText, len );
	} // if
	#endif // __U_DEBUG__

	uDEBUGPRT( uDebugPrt( "%p = (uHeapManager &)%p.doMalloc\n", addr, this ); )
	return addr;
    } // uHeapManager::doMalloc

    inline void uHeapManager::doFree( void * addr ) {
	uDEBUGPRT( uDebugPrt( "(uHeapManager &)%p.doFree( %p )\n", this, addr ); )

	#ifdef __U_DEBUG__
	if ( UNLIKELY( uHeapManager::heapManagerInstance == nullptr ) ) {
	    abort( "uHeapManager::doFree( %p ) : internal error, called before heap is initialized.", addr );
	} // if
	#endif // __U_DEBUG__

	Storage::Header * header;
	FreeHeader * freeElem;
	size_t size, alignment;				// not used (see realloc)

	if ( headers( "free", addr, header, freeElem, size, alignment ) ) { // mmapped ?
	    #ifdef __U_STATISTICS__
	    uFetchAdd( munmap_calls, 1 );
	    uFetchAdd( munmap_storage, size );
	    #endif // __U_STATISTICS__
	    if ( munmap( header, size ) == -1 ) {
		#ifdef __U_DEBUG__
		abort( "Attempt to deallocate storage %p not allocated or with corrupt header.\n"
			"Possible cause is invalid pointer.",
			addr );
		#endif // __U_DEBUG__
	    } // if
	} else {
	    #ifdef __U_PROFILER__
	    if ( uThisTask().profileActive && uProfiler::uProfiler_registerMemoryDeallocate ) {
		(* uProfiler::uProfiler_registerMemoryDeallocate)( uProfiler::profilerInstance, addr, freeElem->blockSize, PROFILEMALLOCENTRY( header ) ); 
	    } // if
	    #endif // __U_PROFILER__

	    #ifdef __U_DEBUG__
	    // Set free memory to garbage so subsequent usages might fail.
	    memset( ((Storage *)header)->data, '\377', freeElem->blockSize - sizeof( Storage ) );
	    #endif // __U_DEBUG__

	    uDEBUGPRT( uDebugPrt( "(uHeapManager &)%p.doFree( %p ) header:%p freeElem:%p\n", this, addr, &header, &freeElem ); )

	    #ifdef __U_STATISTICS__
	    free_storage += size;
	    #endif // __U_STATISTICS__
	    #if defined( SPINLOCK )
	    freeElem->lock.acquire();			// acquire spin lock
	    header->kind.real.next = freeElem->freeList; // push on stack
	    freeElem->freeList = (Storage *)header;
	    freeElem->lock.release();			// release spin lock
	    #else	    
	    freeElem->freeList.push( *(Storage *)header );
	    #endif // SPINLOCK
	    uDEBUGPRT( uDebugPrt( "(uHeapManager &)%p.doFree( %p ) returning free block in list 0x%zx\n", this, addr, size ); )
	} // if

	#ifdef __U_DEBUG__
	uFetchAdd( uHeapManager::allocfree, -size );
	if ( uHeapControl::traceHeap() ) {
	    enum { BufferSize = 64 };
	    char helpText[BufferSize];
	    int len = snprintf( helpText, BufferSize, "Free( %p ) size:%zu\n", addr, size );
	    uDebugWrite( STDERR_FILENO, helpText, len );
	} // if
	#endif // __U_DEBUG__
    } // uHeapManager::doFree


    size_t uHeapManager::prtFree() {
	size_t total = 0;
	#ifdef __U_STATISTICS__
	uDebugAcquire();
	uDebugPrt2( "\nBin lists (bin size : free blocks on list)\n" );
	#endif // __U_STATISTICS__
	for ( unsigned int i = 0; i < maxBucketsUsed; i += 1 ) {
	    size_t size = freeLists[i].blockSize;
	    #ifdef __U_STATISTICS__
	    unsigned int N = 0;
	    #endif // __U_STATISTICS__
	    #if defined( SPINLOCK )
	    for ( Storage * p = freeLists[i].freeList; p != nullptr; p = p->header.kind.real.next ) {
	    #else
	    for ( Storage * p = freeLists[i].freeList.top(); p != nullptr; p = p->header.kind.real.next.top ) {
	    #endif // SPINLOCK
		total += size;
		#ifdef __U_STATISTICS__
		N += 1;
		#endif // __U_STATISTICS__
	    } // for
	    #ifdef __U_STATISTICS__
	    uDebugPrt2( "%7zu, %-7u  ", size, N );
	    if ( (i + 1) % 8 == 0 ) uDebugPrt2( "\n" );
	    #endif // __U_STATISTICS__
	} // for
	    #ifdef __U_STATISTICS__
	uDebugPrt2( "\ntotal free blocks:%zu\n", total );
	uDebugRelease();
	#endif // __U_STATISTICS__
	return (char *)heapEnd - (char *)heapBegin - total;
    } // uHeapManager::prtFree


    uHeapManager::uHeapManager() {
	uDEBUGPRT( uDebugPrt( "(uHeapManager &)%p.uHeap()\n", this ); )
	pageSize = sysconf( _SC_PAGESIZE );

	for ( unsigned int i = 0; i < NoBucketSizes; i += 1 ) { // initialize the free lists
	    freeLists[i].blockSize = bucketSizes[i];
	} // for

	#ifdef FASTLOOKUP
	unsigned int idx = 0;
	for ( unsigned int i = 0; i < LookupSizes; i += 1 ) {
	    if ( i > bucketSizes[idx] ) idx += 1;
	    lookup[i] = idx;
	} // for
	#endif // FASTLOOKUP

	if ( setMmapStart( uDefaultMmapStart() ) ) {
	    abort( "uHeapManager::uHeapManager : internal error, mmap start initialization failure." );
	} // if
	heapExpand = uDefaultHeapExpansion();

	char * end = (char *)sbrk( 0 );
	sbrk( (char *)uCeiling( (long unsigned int)end, uAlign() ) - end ); // move start of heap to multiple of alignment
	heapBegin = heapEnd = sbrk( 0 );		// get new start point

	uDEBUGPRT( uDebugPrt( "(uHeapManager &)%p.uHeap() heapBegin:%p, heapEnd:%p\n", this, heapBegin, heapEnd ); )
    } // uHeapManager::uHeapManager


    uHeapManager::~uHeapManager() {
	#ifdef __U_STATISTICS__
	if ( UPP::uHeapControl::prtHeapTerm() ) {
	    print();
	    if ( UPP::uHeapControl::prtFree() ) uHeapManager::heapManagerInstance->prtFree();
	} // if
	#endif // __U_STATISTICS__
	#ifdef __U_DEBUG__
	if ( uHeapManager::allocfree != 0 ) {
	    // DO NOT USE STREAMS AS THEY MAY BE UNAVAILABLE AT THIS POINT.
	    char helpText[512];
	    int len = snprintf( helpText, 512, "uC++ Runtime warning (UNIX pid:%ld) : program terminating with %lu(0x%lx) bytes of storage allocated but not freed.\n"
		     "Possible cause is unfreed storage allocated by the program or system/library routines called from the program.\n",
		     (long int)getpid(), uHeapManager::allocfree, uHeapManager::allocfree ); // always print the UNIX pid
	    uDebugWrite( STDERR_FILENO, helpText, len );
	} // if
	#endif // __U_DEBUG__
    } // uHeapManager::~uHeapManager


    void uHeapManager::boot() {
	uDEBUGPRT( uDebugPrt( "uHeapManager::boot() enter\n" ); )
	if ( ! uKernelModule::kernelModuleInitialized ) {
	    uKernelModule::startup();
	} // if

	#ifdef __U_DEBUG__
	if ( uHeapBoot ) {				// check for recursion during system boot
	    // DO NOT USE STREAMS AS THEY MAY BE UNAVAILABLE AT THIS POINT.
	    abort( "uHeapManager::boot() : internal error, recursively invoked during system boot." );
	} // if
	uHeapBoot = true;
	#endif // __U_DEBUG__

	uHeapManager::heapManagerInstance = new( &uHeapStorage ) uHeapManager;

	std::set_new_handler( noMemory );		// don't throw exception as the default

	uDEBUGPRT( uDebugPrt( "uHeapManager::boot() exit\n" ); )
    } // uHeapManager::boot


    void * uHeapManager::operator new( size_t, void * storage ) {
	return storage;
    } // uHeapManager::operator new


    void * uHeapManager::operator new( size_t size ) {
	return ::operator new( size );
    } // uHeapManager::operator new


    void uHeapControl::startup() {
	// Just in case no previous malloc, initialization of heap.

	if ( uHeapManager::heapManagerInstance == nullptr ) {
	    uHeapManager::boot();
	} // if

	// Storage allocated before the start of uC++ is normally not freed until after uC++ completes (if at all). Hence,
	// this storage is not considered when calculating unfreed storage when the heap's destructor is called in finishup.

	#ifdef __U_DEBUG__
	uHeapManager::allocfree = 0;
	#endif // __U_DEBUG__
    } // uHeapControl::startup

    void uHeapControl::finishup() {
	// Explicitly invoking the destructor does not close down the heap because it might still be used before the
	// application terminates. The heap's destructor does check for unreleased storage at this point. (The constructor
	// for the heap is called on the first call to malloc.)

	uHeapManager::heapManagerInstance->uHeapManager::~uHeapManager();
    } // uHeapControl::finishup

    void uHeapControl::prepareTask( uBaseTask * /* task */ ) {
    } // uHeapControl::prepareTask

    // void uHeapControl::startTask() {
    // } // uHeapControl::startTask

    // void uHeapControl::finishTask() {
    // } // uHeapControl::finishTask


    inline void * uHeapManager::mallocNoStats( size_t size ) __THROW { // necessary for malloc statistics
	if ( UNLIKELY( UPP::uHeapManager::heapManagerInstance == nullptr ) ) {
	    UPP::uHeapManager::boot();
	} // if

	void * addr = UPP::uHeapManager::heapManagerInstance->doMalloc( size );
	if ( UNLIKELY( addr == nullptr ) ) errno = ENOMEM; // POSIX

	#ifdef __U_PROFILER__
	if ( uThisTask().profileActive && uProfiler::uProfiler_registerMemoryAllocate ) {
	    UPP::uHeapManager::Storage::Header * header = headerAddr( addr );
	    PROFILEMALLOCENTRY( header ) = (* uProfiler::uProfiler_registerMemoryAllocate)( uProfiler::profilerInstance, addr, size, header->kind.real.blockSize & -3 );
	} // if
	#endif // __U_PROFILER__
	return addr;
    } // mallocNoStats


    inline void * uHeapManager::callocNoStats( size_t noOfElems, size_t elemSize ) __THROW {
	size_t size = noOfElems * elemSize;
	char * addr = (char *)UPP::uHeapManager::mallocNoStats( size );
      if ( UNLIKELY( addr == nullptr ) ) return nullptr;

	UPP::uHeapManager::Storage::Header * header;
	UPP::uHeapManager::FreeHeader * freeElem;
	size_t bsize, alignment;
	bool mapped __attribute__(( unused )) = UPP::uHeapManager::heapManagerInstance->headers( "calloc", addr, header, freeElem, bsize, alignment );
	#ifndef __U_DEBUG__
	// Mapped storage is zero filled, but in debug mode mapped memory is scrubbed in doMalloc, so it has to be reset to zero. 
	if ( ! mapped )
	#endif // __U_DEBUG__
	    // Zero entire data space even when > than size => realloc without a new allocation and zero fill works.
	    // <-------00000000000000000000000000000000000000000000000000000> bsize (bucket size)
	    // `-header`-addr                      `-size
	    memset( addr, '\0', bsize - sizeof(UPP::uHeapManager::Storage) ); // set to zeros

	header->kind.real.blockSize |= 2;		// mark as zero filled
	return addr;
    } // callocNoStats


    inline void * uHeapManager::memalignNoStats( size_t alignment, size_t size ) __THROW {
	#ifdef __U_DEBUG__
	UPP::uHeapManager::checkAlign( alignment );	// check alignment
	#endif // __U_DEBUG__

	// if alignment <= default alignment, do normal malloc as two headers are unnecessary
      if ( UNLIKELY( alignment <= uAlign() ) ) return UPP::uHeapManager::mallocNoStats( size );

	// Allocate enough storage to guarantee an address on the alignment boundary, and sufficient space before it for
	// administrative storage. NOTE, WHILE THERE ARE 2 HEADERS, THE FIRST ONE IS IMPLICITLY CREATED BY DOMALLOC.
	//      .-------------v-----------------v----------------v----------,
	//      | Real Header | ... padding ... |   Fake Header  | data ... |
	//      `-------------^-----------------^-+--------------^----------'
	//      |<--------------------------------' offset/align |<-- alignment boundary

	// subtract uAlign() because it is already the minimum alignment
	// add sizeof(Storage) for fake header
	char * addr = (char *)mallocNoStats( size + alignment - uAlign() + sizeof(UPP::uHeapManager::Storage) );
      if ( UNLIKELY( addr == nullptr ) ) return addr;

	// address in the block of the "next" alignment address
	char * user = (char *)uCeiling( (uintptr_t)(addr + sizeof(UPP::uHeapManager::Storage)), alignment );

	// address of header from malloc
	UPP::uHeapManager::Storage::Header * realHeader = headerAddr( addr );
	// address of fake header * before* the alignment location
	UPP::uHeapManager::Storage::Header * fakeHeader = headerAddr( user );
	// SKULLDUGGERY: insert the offset to the start of the actual storage block and remember alignment
	fakeHeader->kind.fake.offset = (char *)fakeHeader - (char *)realHeader;
	// SKULLDUGGERY: odd alignment imples fake header
	fakeHeader->kind.fake.alignment = alignment | 1;

	#ifdef __U_PROFILER__
	if ( uThisTask().profileActive && uProfiler::uProfiler_registerMemoryAllocate ) {
	    PROFILEMALLOCENTRY( fakeHeader ) = (* uProfiler::uProfiler_registerMemoryAllocate)( uProfiler::profilerInstance, addr, size, realHeader->kind.real.home->blockSize & -3 );
	} // if
	#endif // __U_PROFILER__

	return user;
    } // memalignNoStats


    inline void * uHeapManager::cmemalignNoStats( size_t alignment, size_t noOfElems, size_t elemSize ) __THROW {
	size_t size = noOfElems * elemSize;
	char * addr = (char *)UPP::uHeapManager::memalignNoStats( alignment, size );
      if ( UNLIKELY( addr == nullptr ) ) return nullptr;
	UPP::uHeapManager::Storage::Header * header;
	UPP::uHeapManager::FreeHeader * freeElem;
	size_t bsize;
	bool mapped __attribute__(( unused )) = UPP::uHeapManager::heapManagerInstance->headers( "cmemalign", addr, header, freeElem, bsize, alignment );
	#ifndef __U_DEBUG__
	// Mapped storage is zero filled, but in debug mode mapped memory is scrubbed in doMalloc, so it has to be reset to zero.
	if ( ! mapped )
	#endif // __U_DEBUG__
	    memset( addr, '\0', dataStorage( bsize, addr, header ) ); // set to zeros
	header->kind.real.blockSize |= 2;		// mark as zero filled

	return addr;
    } // cmemalignNoStats
} // UPP


// Operators new and new [] call malloc; delete calls free

extern "C" {
    void * malloc( size_t size ) __THROW {
	#ifdef __U_STATISTICS__
	uFetchAdd( UPP::uHeapManager::malloc_calls, 1 );
	uFetchAdd( UPP::uHeapManager::malloc_storage, size );
	#endif // __U_STATISTICS__

	void * addr = UPP::uHeapManager::mallocNoStats( size );

	uDEBUGPRT( uDebugPrt( "%p = malloc( %zu )\n", addr, size ); )
	return addr;
    } // malloc


    void * calloc( size_t noOfElems, size_t elemSize ) __THROW {
	#ifdef __U_STATISTICS__
	uFetchAdd( UPP::uHeapManager::calloc_calls, 1 );
	uFetchAdd( UPP::uHeapManager::calloc_storage, noOfElems * elemSize );
	#endif // __U_STATISTICS__

	char * addr = (char *)UPP::uHeapManager::callocNoStats( noOfElems, elemSize );

	uDEBUGPRT( uDebugPrt( "%p = calloc( %zu, %zu )\n", addr, noOfElems, elemSize ); )
	return addr;
    } // calloc


    void * realloc( void * oaddr, size_t size ) __THROW {
	#ifdef __U_STATISTICS__
	uFetchAdd( UPP::uHeapManager::realloc_calls, 1 );
	#endif // __U_STATISTICS__

	// If size is equal to 0, either NULL or a pointer suitable to be passed to free() is returned.
      if ( UNLIKELY( size == 0 ) ) { free( oaddr ); return UPP::uHeapManager::mallocNoStats( size ); } // special cases
      if ( UNLIKELY( oaddr == nullptr ) ) return UPP::uHeapManager::mallocNoStats( size );

	UPP::uHeapManager::Storage::Header * header;
	UPP::uHeapManager::FreeHeader * freeElem;
	size_t bsize, oalign = 0;
	UPP::uHeapManager::heapManagerInstance->headers( "realloc", oaddr, header, freeElem, bsize, oalign );

	size_t odsize = dataStorage( bsize, oaddr, header ); // data storage available in bucket
      if ( size <= odsize && odsize <= size * 2 ) {	// allow up to 50% wasted storage in smaller size
	    // Do not know size of original allocation => cannot do 0 fill for any additional space because do not know
	    // where to start filling, i.e., do not overwrite existing values in space.
	    //
	    // This case does not result in a new profiler entry because the previous one still exists and it must match with
	    // the free for this memory.  Hence, this realloc does not appear in the profiler output.
	    return oaddr;
	} // if

	#ifdef __U_STATISTICS__
	uFetchAdd( UPP::uHeapManager::realloc_storage, size );
	#endif // __U_STATISTICS__

	// change size and copy old content to new storage

	void * naddr;
	if ( UNLIKELY( oalign != 0 ) ) {		// previous request memalign?
	    if ( UNLIKELY( header->kind.real.blockSize & 2 ) ) { // previous request zero fill
		naddr = UPP::uHeapManager::cmemalignNoStats( oalign, 1, size ); // create new aligned area
	    } else {
		naddr = UPP::uHeapManager::memalignNoStats( oalign, size ); // create new aligned area
	    } // if
	} else {
	    if ( UNLIKELY( header->kind.real.blockSize & 2 ) ) { // previous request zero fill
		naddr = UPP::uHeapManager::callocNoStats( 1, size ); // create new area
	    } else {
		naddr = UPP::uHeapManager::mallocNoStats( size ); // create new area
	    } // if
	} // if
      if ( UNLIKELY( naddr == 0 ) ) return nullptr;
	UPP::uHeapManager::heapManagerInstance->headers( "realloc", naddr, header, freeElem, bsize, oalign );
	size_t ndsize = dataStorage( bsize, naddr, header ); // data storage avilable in bucket
	// To preserve prior fill, the entire bucket must be copied versus the size.
	memcpy( naddr, oaddr, std::min( odsize, ndsize ) ); // copy bytes
	free( oaddr );
	uDEBUGPRT( uDebugPrt( "%p = realloc( %p, %zu )\n", naddr, oaddr, size ); )
	return naddr;
    } // realloc


    void * memalign( size_t alignment, size_t size ) __THROW {
	#ifdef __U_STATISTICS__
	uFetchAdd( UPP::uHeapManager::memalign_calls, 1 );
	uFetchAdd( UPP::uHeapManager::memalign_storage, size );
	#endif // __U_STATISTICS__

	void * addr = UPP::uHeapManager::memalignNoStats( alignment, size );

	uDEBUGPRT( uDebugPrt( "%p = memalign( %zu, %zu )\n", addr, alignment, size ); )
	return addr;
    } // memalign


    void * cmemalign( size_t alignment, size_t noOfElems, size_t elemSize ) __THROW {
	#ifdef __U_STATISTICS__
	uFetchAdd( UPP::uHeapManager::cmemalign_calls, 1 );
	uFetchAdd( UPP::uHeapManager::cmemalign_storage, noOfElems * elemSize );
	#endif // __U_STATISTICS__

	char * addr = (char *)UPP::uHeapManager::cmemalignNoStats( alignment, noOfElems, elemSize );

	uDEBUGPRT( uDebugPrt( "%p = cmemalign( %zu, %zu, %zu )\n", addr, alignment, noOfElems, elemSize ); )
	return addr;
    } // cmemalign


    void * aligned_alloc( size_t alignment, size_t size ) {
	return memalign( alignment, size );
    } // aligned_alloc


    int posix_memalign( void ** memptr, size_t alignment, size_t size ) {
      if ( alignment < sizeof(void *) || ! uPow2( alignment ) ) return EINVAL; // check alignment
	* memptr = memalign( alignment, size );
      if ( UNLIKELY( * memptr == nullptr ) ) return ENOMEM;
	return 0;
    } // posix_memalign


    void * valloc( size_t size ) __THROW {
	return memalign( UPP::uHeapManager::pageSize, size );
    } // valloc


    void free( void * addr ) __THROW {
	#ifdef __U_STATISTICS__
	uFetchAdd( UPP::uHeapManager::free_calls, 1 );
	#endif // __U_STATISTICS__

      if ( UNLIKELY( addr == nullptr ) ) {			// special case
	    #ifdef __U_PROFILER__
	    if ( uThisTask().profileActive && uProfiler::uProfiler_registerMemoryDeallocate ) {
		(* uProfiler::uProfiler_registerMemoryDeallocate)( uProfiler::profilerInstance, addr, 0, 0 ); 
	    } // if
	    #endif // __U_PROFILER__
	    // #ifdef __U_DEBUG__
	    // if ( UPP::uHeapControl::traceHeap() ) {
	    // 	#define nullmsg "Free( 0x0 ) size:0\n"
	    // 	// Do not debug print free( nullptr ), as it can cause recursive entry from sprintf.
	    // 	uDebugWrite( STDERR_FILENO, nullmsg, sizeof(nullmsg) - 1 );
	    // } // if
	    // #endif // __U_DEBUG__
	    return;
	} // exit

	UPP::uHeapManager::heapManagerInstance->doFree( addr );
	// Do not debug print free( nullptr ), as it can cause recursive entry from sprintf.
	uDEBUGPRT( uDebugPrt( "free( %p )\n", addr ); )
    } // free


    size_t malloc_alignment( void * addr ) __THROW {
      if ( UNLIKELY( addr == nullptr ) ) return uAlign(); // minimum alignment
	UPP::uHeapManager::Storage::Header * header = headerAddr( addr );
	if ( (header->kind.fake.alignment & 1) == 1 ) {	// fake header ?
	    return header->kind.fake.alignment & -2;	// remove flag from value
	} else {
	    return uAlign();				// minimum alignment
	} // if
    } // malloc_alignment


    bool malloc_zero_fill( void * addr ) __THROW {
      if ( UNLIKELY( addr == nullptr ) ) return false;	// null allocation is not zero fill
	UPP::uHeapManager::Storage::Header * header = headerAddr( addr );
	if ( (header->kind.fake.alignment & 1) == 1 ) { // fake header ?
	    header = (UPP::uHeapManager::Storage::Header *)((char *)header - header->kind.fake.offset);
	} // if
	return (header->kind.real.blockSize & 2) != 0;	// zero filled (calloc/cmemalign) ?
    } // malloc_zero_fill


    size_t malloc_usable_size( void * addr ) __THROW {
      if ( UNLIKELY( addr == nullptr ) ) return 0;	// null allocation has 0 size
 	UPP::uHeapManager::Storage::Header * header;
 	UPP::uHeapManager::FreeHeader * freeElem;
 	size_t bsize, alignment;

 	UPP::uHeapManager::heapManagerInstance->headers( "malloc_usable_size", addr, header, freeElem, bsize, alignment );
	return dataStorage( bsize, addr, header );	// data storage in bucket
    } // malloc_usable_size


    void malloc_stats() __THROW {
	#ifdef __U_STATISTICS__
	UPP::uHeapManager::print();
	if ( UPP::uHeapControl::prtFree() ) UPP::uHeapManager::heapManagerInstance->prtFree();
	#endif // __U_STATISTICS__
    } // malloc_stats


    int malloc_stats_fd( int fd __attribute__(( unused )) ) __THROW {
	#ifdef __U_STATISTICS__
	int temp = UPP::uHeapManager::stats_fd;
	UPP::uHeapManager::stats_fd = fd;
	return temp;
	#else
	return -1;
	#endif // __U_STATISTICS__
    } // malloc_stats_fd


    int mallopt( int option, int value ) __THROW {
	switch( option ) {
	  case M_TOP_PAD:
	    if ( UPP::uHeapManager::heapManagerInstance->setHeapExpand( value ) ) return 1;
	    break;
	  case M_MMAP_THRESHOLD:
	    if ( UPP::uHeapManager::heapManagerInstance->setMmapStart( value ) ) return 1;
	    break;
	} // switch
	return 0;
    } // mallopt


    int malloc_trim( size_t ) {
	return 0;					// => impossible to release memory
    } // malloc_trim


    int malloc_info( int options, FILE * stream __attribute__(( unused )) ) {
	if ( options != 0 ) { errno = EINVAL; return -1; }
	#ifdef __U_STATISTICS__
	return UPP::uHeapManager::printXML( stream );
	#else
	return 0;					// unsupported
	#endif // __U_STATISTICS__
    } // malloc_info


    void * malloc_get_state( void ) {
	return nullptr;					// unsupported
    } // malloc_get_state


    int malloc_set_state( void * ) {
	return 0;					// unsupported
    } // malloc_set_state
} // extern "C"


// Must have C++ linkage to overload with C linkage realloc.
void * realloc( void * oaddr, size_t nalign, size_t size ) __THROW {
    #ifdef __U_STATISTICS__
    uFetchAdd( UPP::uHeapManager::realloc_calls, 1 );
    #endif // __U_STATISTICS__

    // If size is equal to 0, either NULL or a pointer suitable to be passed to free() is returned.
  if ( UNLIKELY( size == 0 ) ) { free( oaddr ); return UPP::uHeapManager::mallocNoStats( size ); } // special cases
  if ( UNLIKELY( oaddr == nullptr ) ) return UPP::uHeapManager::mallocNoStats( size );

    if ( UNLIKELY( nalign == 0 ) ) nalign = uAlign();	// reset alignment to minimum
    #ifdef __U_DEBUG__
    else
	UPP::uHeapManager::checkAlign( nalign );	// check alignment
    #endif // __U_DEBUG__

    UPP::uHeapManager::Storage::Header * header;
    UPP::uHeapManager::FreeHeader * freeElem;
    size_t bsize, oalign = 0;
    UPP::uHeapManager::heapManagerInstance->headers( "realloc", oaddr, header, freeElem, bsize, oalign );
    size_t odsize = dataStorage( bsize, oaddr, header ); // data storage available in bucket

  if ( oalign != 0 && (uintptr_t)oaddr % nalign == 0 ) { // has alignment and just happens to work out
	headerAddr( oaddr )->kind.fake.alignment = nalign | 1; // update alignment (could be the same)
	return realloc( oaddr, size );
    } // if

    #ifdef __U_STATISTICS__
    uFetchAdd( UPP::uHeapManager::realloc_storage, size );
    #endif // __U_STATISTICS__

    // change size and copy old content to new storage

    void * naddr;
    if ( UNLIKELY( header->kind.real.blockSize & 2 ) ) { // previous request zero fill
        naddr = UPP::uHeapManager::cmemalignNoStats( nalign, 1, size ); // create new aligned area
    } else {
        naddr = UPP::uHeapManager::memalignNoStats( nalign, size ); // create new aligned area
    } // if
    UPP::uHeapManager::heapManagerInstance->headers( "realloc", naddr, header, freeElem, bsize, oalign );
    size_t ndsize = dataStorage( bsize, naddr, header ); // data storage avilable in bucket
    // To preserve prior fill, the entire bucket must be copied versus the size.
    memcpy( naddr, oaddr, std::min( odsize, ndsize ) ); // copy bytes
    free( oaddr );
    uDEBUGPRT( uDebugPrt( "%p = realloc( %p, %zu )\n", naddr, oaddr, size ); )
    return naddr;
} // realloc


// Local Variables: //
// compile-command: "make install" //
// End: //
