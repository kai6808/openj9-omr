/*******************************************************************************
 * Copyright IBM Corp. and others 1991
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

/**
 * @file
 * @ingroup GC_Base_Core
 */

#include "omrcfg.h"
#include "omr.h"

#include "EnvironmentBase.hpp"
#include "GCExtensionsBase.hpp"
#include "Heap.hpp"
#include "HeapRegionDescriptor.hpp"
#include "HeapRegionIterator.hpp"
#include "HeapRegionManager.hpp"
#include "MarkMap.hpp"
#include "ParallelDispatcher.hpp"
#include "Task.hpp"


/**
 * Object creation and destruction
 */
MM_MarkMap *
MM_MarkMap::newInstance(MM_EnvironmentBase *env, uintptr_t maxHeapSize)
{
	MM_MarkMap *markMap = (MM_MarkMap *)env->getForge()->allocate(sizeof(MM_MarkMap), OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
	if (NULL != markMap) {
		new(markMap) MM_MarkMap(env, maxHeapSize);
		if (!markMap->initialize(env)) {
			markMap->kill(env);
			markMap = NULL;
		}
	}

	return markMap;
}

void
MM_MarkMap::initializeMarkMap(MM_EnvironmentBase *env)
{
	/* TODO: The multiplier should really be some constant defined globally */
	const uintptr_t MODRON_PARALLEL_MULTIPLIER = 32;
	uintptr_t heapAlignment = _extensions->heapAlignment;

	/* Determine the size of heap that a work unit of mark map clearing corresponds to */
	uintptr_t heapClearUnitFactor = env->_currentTask->getThreadCount();
	heapClearUnitFactor = ((heapClearUnitFactor == 1) ? 1 : heapClearUnitFactor * MODRON_PARALLEL_MULTIPLIER);
	uintptr_t heapClearUnitSize = _extensions->heap->getMemorySize() / heapClearUnitFactor;
	heapClearUnitSize = MM_Math::roundToCeiling(heapAlignment, heapClearUnitSize);

	/* Walk all object segments to determine what ranges of the mark map should be cleared */
	MM_HeapRegionDescriptor *region;
	MM_Heap *heap = _extensions->getHeap();
	MM_HeapRegionManager *regionManager = heap->getHeapRegionManager();
	GC_HeapRegionIterator regionIterator(regionManager);
	while(NULL != (region = regionIterator.nextRegion())) {
		if (region->isCommitted()) {
			/* Walk the segment in chunks the size of the heapClearUnit size, checking if the corresponding mark map
			 * range should  be cleared.
			 */
			uint8_t* heapClearAddress = (uint8_t*)region->getLowAddress();
			uintptr_t heapClearSizeRemaining = region->getSize();

			while(0 != heapClearSizeRemaining) {
				/* Calculate the size of heap that is to be processed */
				uintptr_t heapCurrentClearSize = (heapClearUnitSize > heapClearSizeRemaining) ? heapClearSizeRemaining : heapClearUnitSize;
				Assert_MM_true(heapCurrentClearSize > 0);

				/* Check if the thread should clear the corresponding mark map range for the current heap range */
				if(J9MODRON_HANDLE_NEXT_WORK_UNIT(env)) {
					/* Convert the heap address/size to its corresponding mark map address/size */
					/* NOTE: We calculate the low and high heap offsets, and build the mark map index and size values
					 * from these to avoid rounding errors (if we use the size, the conversion routine could get a different
					 * rounding result then the actual end address)
					 */
					uintptr_t heapClearOffset = ((uintptr_t)heapClearAddress) - _heapMapBaseDelta;
					uintptr_t heapMapClearIndex = convertHeapIndexToHeapMapIndex(env, heapClearOffset, sizeof(uintptr_t));
					uintptr_t heapMapClearSize =
						convertHeapIndexToHeapMapIndex(env, heapClearOffset + heapCurrentClearSize, sizeof(uintptr_t))
						- heapMapClearIndex;

					/* And clear the mark map */
					OMRZeroMemory((void *) (((uintptr_t)_heapMapBits) + heapMapClearIndex), heapMapClearSize);
				}

				/* Move to the next address range in the segment */
				heapClearAddress += heapCurrentClearSize;
				heapClearSizeRemaining -= heapCurrentClearSize;
			}
		}
	}
}

void
MM_MarkMap::dumpMarkMap(MM_EnvironmentBase *env, FILE *file, uintptr_t numPages)
{
	assert(file);
	assert(_heapMapBits);

	MM_GCExtensionsBase *extensions = env->getExtensions();
	MM_MemoryManager *memoryManager = extensions->memoryManager;

	void *heapMapTop = memoryManager->getHeapTop(&_heapMapMemoryHandle);
	uintptr_t heapMapSize = memoryManager->getMaximumSize(&_heapMapMemoryHandle);
	
	fprintf(file, "mark map base: %p, top: %p, size: 0x%lx, num of long hex(8 bytes): %lu\n", _heapMapBits, heapMapTop,  heapMapSize, heapMapSize/sizeof(uintptr_t));

	// each page -> 8 uintptr_t in mark map
	for (uintptr_t i = 0; i < numPages; i++) {
		fprintf(file, "Page %lu: 0x", i);
		for (uintptr_t j = 0; j < 8; j++) {
			fprintf(file, "%016lx",
					*(_heapMapBits + i * 8 + j)
			);
		}
		fprintf(file, "\n");
	}

}