#include <ocr.h>
#include <extensions/ocr-affinity.h>

#ifdef OCR_APP_OPTIMIZED_PLACEMENT
/* Placement-optimization layer.  The whole recursion sorts ONE array block:
 * each task acquires it DB_MODE_RW, partitions its range in place, and hands
 * it to its children -- a single-writer chain over one block, which no hint
 * can parallelize across ranks.  What a hint can do is stop the block from
 * following the chain around.  A subarray's tasks are placed by the range they
 * sort rather than by where they were created: a child covering [low, high]
 * goes to the place that owns the MIDDLE of that range, so the tasks that
 * revisit a region keep returning to the same place.
 *
 * The middle and not the start.  Keyed on the start, every range that begins
 * at the front of the array maps to the first place however much of the array
 * it covers, so the root and the entire spine of ranges that keep the front
 * stack on one place while the leaves spread evenly -- the task COUNT balances
 * and the WORK does not.  The middle of a range moves with the range's extent,
 * so a shrinking spine walks down the places instead of resting on one, and a
 * leaf, whose middle is its start, is placed exactly as before.
 *
 * Placing by the creating task instead would keep the whole recursion on the
 * one place the root started on -- perfectly local, and using a single node of
 * however many the machine has.  That is not a scheduling answer, and a run
 * that leaves most of the machine idle is not comparable with one that does
 * not. */
static ocrHint_t * qsRangeEdtHint(ocrHint_t *h, u64 low, u64 high, u64 arraySize) {
    u64 pdCount;
    ocrAffinityCount(AFFINITY_PD, &pdCount);
    if (pdCount <= 1) return NULL_HINT;
    u64 mid = low + (high - low) / 2;
    u64 place = arraySize ? (mid * pdCount) / arraySize : 0;
    if (place >= pdCount) place = pdCount - 1;
    ocrGuid_t aff;
    ocrAffinityGetAt(AFFINITY_PD, place, &aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}
#else
#define qsRangeEdtHint(h, low, high, n) NULL_HINT
#endif
#include <stdlib.h>
#include "macros.h"

#define CACHE_LINE_SIZE 64
//Size of array to be sorted (compile-time default; overridable at runtime)
#define ARRAY_SIZE 1000
//Range of numbers to be sorted.
#define RANGE 1000000
//Number of tasks the input generation is cut into (overridable at runtime).
#define FILL_TASKS 128

typedef struct {
    u64 low;
    u64 high;
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    //carried only for the placement layer, which needs the whole extent to
    //turn a subarray's range into the place that owns it
    u64 arraySize;
#endif
    ocrGuid_t qsortTemplate;
} qsortPRM_t;

typedef struct {
    u64 low;
    u64 high;
    u64 range;
} fillPRM_t;

typedef struct {
    u64 arraySize;
} finishPRM_t;

//Index-seeded value generator: a pure function of (index, range) carrying no
//state between calls, so any task can produce any range of the input and the
//multiset does not depend on how the array was divided among its producers.
static u64 genValue(u64 index, u64 range) {
    u64 z = (index + 1) * 0x9E3779B97F4A7C15UL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
    z = z ^ (z >> 31);
    return z % range;
}

//Pseudo-RNG the partition draws its pivot index from.  Gets rid of C stdlib
//dependence.
int getRandNum(int seed) {
    int MAX = 1000;
    int i;
    int r[MAX];
    int ret;
    r[0] = seed;
    for(i=1; i<31; i++){
        r[i] = (16807LL * r[i-1]) % 2147483647;
        if (r[i] < 0){
            r[i] += 2147483647;
        }
    }
    for(i=31; i<34; i++){
        r[i] = r[i-31];
    }
    for(i=34; i<344; i++){
        r[i] = r[i-31] + r[i-3];
    }
    for(i=344; i<MAX; i++){
        r[i] = r[i-31] + r[i-3];
        ret = ((unsigned int)r[i]) >> 1;
    }
    return ret;
}

//Insertion sort for very small problem sizes that don't need parallelized
//DSS: fixed error (tracking jmin to swap correct elements)
void sortSerial(u64 *data, u64 low, u64 high) {
    u64 min, i, j, temp, jmin;
    //A one-element range is already sorted, and the bound below cannot say so:
    //the indices are unsigned, so at the front of the block `high-1` wraps and
    //the loop walks off the end instead of not running at all.
    if(low >= high) return;
    for(i = low; i <= high-1; i++) {
        min = 0xFFFFFFFFFFFFFFFFUL;
        for(j = i; j <=high; j++)
            if(data[j] < min){
                min = data[j];
                jmin = j;
            }
        temp = data[i];
        data[i] = min;
        data[jmin] = temp;
    }
}

// paramv 0: first index; 1: one past the last; 2: value range
// depv   0: array
ocrGuid_t fillTask( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    fillPRM_t *fillParamvIn = (fillPRM_t *)paramv;
    u64 low = fillParamvIn->low, high = fillParamvIn->high;
    u64 range = fillParamvIn->range;
    u64 *data = depv[0].ptr;
    u64 i;
    for(i = low; i < high; i++)
        data[i] = genValue(i, range);
    return NULL_GUID;
}

// paramv 0: low index (inclusive)
// paramv 1: high index (inclusive)
// paramv 2: qsort edt template
// depv   0: array
ocrGuid_t qsortTask( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    qsortPRM_t *qsortParamvIn = (qsortPRM_t *)paramv;
    u64 low = qsortParamvIn->low;
    u64 high = qsortParamvIn->high;
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    u64 arraySize = qsortParamvIn->arraySize;
#endif
    ocrGuid_t qsortTemplate = qsortParamvIn->qsortTemplate;
    u64 size = high - low + 1;
    ocrGuid_t dbGuid = depv[0].guid;
    u64 * data = depv[0].ptr;
    if(size * sizeof(u64) <= CACHE_LINE_SIZE)
        sortSerial(data, low, high);
    else {
        //Set pivot point. The pivot is randomly selected.
        //Below: (size/2) is an arbitrary number, as getRandNum requires a seed.
        u64 pivotIndex = low + (getRandNum(size/2))%(high-low);
        u64 pivot = data[pivotIndex];

        // partition
        u64 curIndex = low, swapIndex = high-1;
        u64 temp;
        data[pivotIndex] = data[high];
        data[high] = pivot;

        //Find something smaller and larger than pivot to swap
        //DSS: modified to search from both ends. Previous was correct but inefficient
        while(1) {
            //look for something bigger
            while((data[curIndex] <= pivot) && (curIndex < swapIndex)) {
                curIndex++;
            }
            if(curIndex == swapIndex) {
                break;
            }
            //look for something smaller
            while((data[swapIndex] >= pivot) && (curIndex < swapIndex)) {
                swapIndex--;
            }
            if(curIndex == swapIndex) {
                break;
            }
            //swap
            temp = data[swapIndex];
            data[swapIndex] = data[curIndex];
            data[curIndex] = temp;
            curIndex++;
        }

        //Place the pivot at the first index ABOVE it.  The scan can stop on an
        //element below the pivot, and this index is also the boundary the two
        //children are cut at, so leaving the pivot there strands that element
        //on the wrong side of a partition nothing revisits.
        if(data[swapIndex] < pivot) swapIndex++;
        data[high] = data[swapIndex];
        data[swapIndex] = pivot;
        ocrDbRelease(dbGuid);
        pivotIndex = swapIndex;

        // recursively create EDTs and quicksort the high/low partitioned subarrays.
        //A side that the pivot leaves empty gets no task.  The bounds are
        //unsigned, so with the pivot at the low end `pivotIndex-1` does not
        //describe an empty range -- it wraps, and the child reads off the block.
        ocrGuid_t qsortLowEdt, qsortHighEdt;
        ocrGuid_t qsortLowDataEvt, qsortHighDataEvt;

        if(pivotIndex > low) {
            ocrEventCreate(&qsortLowDataEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
            qsortPRM_t qsortLowParamv;
            qsortLowParamv.low = low;
            qsortLowParamv.high = pivotIndex-1;
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
            qsortLowParamv.arraySize = arraySize;
#endif
            qsortLowParamv.qsortTemplate = qsortTemplate;
            ocrHint_t lowHNT;
            ocrEdtCreate(&qsortLowEdt, qsortTemplate, EDT_PARAM_DEF, (u64 *)&qsortLowParamv,
                     EDT_PARAM_DEF, &qsortLowDataEvt, EDT_PROP_FINISH,
                     qsRangeEdtHint(&lowHNT, low, pivotIndex-1, arraySize), NULL);
            ocrEventSatisfy(qsortLowDataEvt, dbGuid);
        }

        if(pivotIndex < high) {
            ocrEventCreate(&qsortHighDataEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
            qsortPRM_t qsortHighParamv;
            qsortHighParamv.low = pivotIndex+1;
            qsortHighParamv.high = high;
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
            qsortHighParamv.arraySize = arraySize;
#endif
            qsortHighParamv.qsortTemplate = qsortTemplate;
            ocrHint_t highHNT;
            ocrEdtCreate(&qsortHighEdt, qsortTemplate, EDT_PARAM_DEF, (u64 *)&qsortHighParamv,
                     EDT_PARAM_DEF, &qsortHighDataEvt, EDT_PROP_FINISH,
                     qsRangeEdtHint(&highHNT, pivotIndex+1, high, arraySize), NULL);
            ocrEventSatisfy(qsortHighDataEvt, dbGuid);
        }
    }
    return NULL_GUID;
}

//Print validation feedback and quit.
ocrGuid_t finishTask( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    finishPRM_t *finishParamvIn = (finishPRM_t *)paramv;
    u64 arraySize = finishParamvIn->arraySize;
    u64 showCount = arraySize < 30 ? arraySize : 30;
    ocrPrintf("Showing first %lu elements: \n", showCount);
    u64 i;
    u64 *data = depv[0].ptr;
    for(i = 0; i < showCount; i++)
        ocrPrintf("%lu \n", data[i]);

    /* What the program printed before was its first thirty elements, which a
     * sort that dropped or duplicated values passes just as easily.  Walk the
     * whole array instead: non-decreasing, and the sum a permutation of the
     * input must preserve. */
    u64 sum = 0, sorted = 1;
    for(i = 0; i < arraySize; i++) {
        if(i && data[i] < data[i-1]) sorted = 0;
        sum += data[i];
    }
    ocrPrintf("QSORT_VALID sum=%lu sorted=%lu n=%lu\n", sum, sorted, arraySize);

    ocrPrintf("Sorting Finished. Shutting Down OCR\n");
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t mainEdt( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t qsortTemplate, qsortRootTemplate, fillTemplate;
    ocrGuid_t qsortEdt;
    ocrGuid_t dataDb;
    ocrGuid_t outEvt;
    u64 *data;
    u64 k;

    qsortPRM_t  qsortParamv;
    finishPRM_t finishParamv;

    // Positional argv: [arraySize] [range] [fillTasks]; absent arguments keep
    // the #define defaults, and an argument that cannot describe a run is
    // reported rather than replaced.
    u64 arraySize = ARRAY_SIZE;
    u64 range = RANGE;
    u64 fillTasks = FILL_TASKS;
    u64 argc = 0;
    if(depc >= 1 && depv[0].ptr) argc = getArgc(depv[0].ptr);
    if(argc > 4) {
        ocrPrintf("QSORT_ARG_ERROR at most three arguments: arraySize range fillTasks\n");
        ocrShutdown();
        return NULL_GUID;
    }
    if(argc > 1) arraySize = strtoull(getArgv(depv[0].ptr, 1), NULL, 10);
    if(argc > 2) range     = strtoull(getArgv(depv[0].ptr, 2), NULL, 10);
    if(argc > 3) fillTasks = strtoull(getArgv(depv[0].ptr, 3), NULL, 10);
    if(arraySize == 0 || range == 0 || fillTasks == 0) {
        ocrPrintf("QSORT_ARG_ERROR arraySize, range and fillTasks must be positive\n");
        ocrShutdown();
        return NULL_GUID;
    }
    if(fillTasks > arraySize) fillTasks = arraySize;
    ocrPrintf("QSORT_ARGS arraySize=%lu range=%lu fillTasks=%lu\n",
              arraySize, range, fillTasks);

    ocrEdtTemplateCreate(&qsortTemplate, qsortTask, PRMNUM(qsort), 1);

    ocrDbCreate(&dataDb, (void**)&data, sizeof(u64) * (arraySize),
        /*flags=*/0, /*location=*/NULL_HINT, NO_ALLOC);
    ocrDbRelease(dataDb);

    /* The input is generated by the tasks that own its ranges, not by this
     * one: every value is a pure function of its index, so a range can be
     * produced wherever its producer runs and the array does not depend on how
     * the ranges were cut.  They are placed where the block is -- one block
     * holds the whole array, and a writer anywhere else would move all of it
     * to write a slice. */
    ocrGuid_t homeAff;
    ocrHint_t fillHNT;
    ocrAffinityGetCurrent(&homeAff);
    ocrHintInit(&fillHNT, OCR_HINT_EDT_T);
    ocrSetHintValue(&fillHNT, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(homeAff));

    ocrGuid_t *fillEdt = (ocrGuid_t*)malloc(fillTasks * sizeof(ocrGuid_t));
    ocrGuid_t *fillOut = (ocrGuid_t*)malloc(fillTasks * sizeof(ocrGuid_t));
    ocrEdtTemplateCreate(&fillTemplate, fillTask, PRMNUM(fill), 1);
    for(k = 0; k < fillTasks; k++) {
        fillPRM_t fillParamv;
        fillParamv.low   = (k * arraySize) / fillTasks;
        fillParamv.high  = ((k + 1) * arraySize) / fillTasks;
        fillParamv.range = range;
        ocrEdtCreate(&fillEdt[k], fillTemplate, EDT_PARAM_DEF, (u64 *)&fillParamv,
                     EDT_PARAM_DEF, NULL, EDT_PROP_NONE, &fillHNT, &fillOut[k]);
    }
    ocrEdtTemplateDestroy(fillTemplate);

    qsortParamv.low = 0;
    qsortParamv.high = arraySize-1;
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    qsortParamv.arraySize = arraySize;
#endif
    qsortParamv.qsortTemplate = qsortTemplate;

    //The root is the only task in the recursion that waits for anything but
    //the array: its remaining slots are the generation it follows.  The
    //children carry the one-slot template.
    ocrHint_t rootHNT;
    ocrEdtTemplateCreate(&qsortRootTemplate, qsortTask, PRMNUM(qsort),
                         (u32)(1 + fillTasks));
    ocrEdtCreate(&qsortEdt, qsortRootTemplate, EDT_PARAM_DEF, (u64 *)&qsortParamv,
        EDT_PARAM_DEF, NULL, EDT_PROP_FINISH,
        qsRangeEdtHint(&rootHNT, 0, arraySize-1, arraySize), &outEvt);
    ocrEdtTemplateDestroy(qsortRootTemplate);

    // Link up output event to a coordination event to be used by the finishEddt
    ocrGuid_t coordEvt;
    ocrEventCreate(&coordEvt, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);
    ocrAddDependence(outEvt, coordEvt, 0, DB_MODE_RO);

    // Enables qsortEdt
    ocrAddDependence(dataDb, qsortEdt, 0, DB_MODE_RW);
    for(k = 0; k < fillTasks; k++)
        ocrAddDependence(fillOut[k], qsortEdt, (u32)(1 + k), DB_MODE_NULL);
    //every consumer of the generation is registered, so the generators may run
    for(k = 0; k < fillTasks; k++)
        ocrAddDependence(dataDb, fillEdt[k], 0, DB_MODE_RW);
    free(fillEdt);
    free(fillOut);

    ocrGuid_t finishTemplate;
    ocrGuid_t finishEdt;

    finishParamv.arraySize = arraySize;

    ocrGuid_t finishDepv[2] = {dataDb, coordEvt};
    ocrEdtTemplateCreate(&finishTemplate, finishTask, PRMNUM(finish), 2);
    ocrHint_t finHNT;
    ocrEdtCreate(&finishEdt, finishTemplate, EDT_PARAM_DEF, (u64 *)&finishParamv,
        EDT_PARAM_DEF, finishDepv, 0,
        qsRangeEdtHint(&finHNT, 0, arraySize-1, arraySize), NULL);
    return NULL_GUID;
}
